/*-------------------------------------------------------------------------
 *
 * spgutils.c
 *	  various support functions for SP-GiST
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/spgist/spgutils.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/amvalidate.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/spgist_private.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/pg_amop.h"
#include "commands/vacuum.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/index_selfuncs.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/*
 * SP-GiST handler function: return IndexAmRoutine with access method parameters
 * and callbacks.
 */
Datum
spghandler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies = 0;
	amroutine->amsupport = SPGISTNProc;
	amroutine->amoptsprocnum = SPGIST_OPTIONS_PROC;
	amroutine->amcanorder = false;
	amroutine->amcanorderbyop = true;
	amroutine->amcanbackward = false;
	amroutine->amcanunique = false;
	amroutine->amcanmulticol = false;
	amroutine->amoptionalkey = true;
	amroutine->amsearcharray = false;
	amroutine->amsearchnulls = true;
	amroutine->amstorage = false;
	amroutine->amclusterable = false;
	amroutine->ampredlocks = false;
	amroutine->amcanparallel = false;
	amroutine->amcaninclude = false;
	amroutine->amusemaintenanceworkmem = false;
	amroutine->amparallelvacuumoptions =
		VACUUM_OPTION_PARALLEL_BULKDEL | VACUUM_OPTION_PARALLEL_COND_CLEANUP;
	amroutine->amkeytype = InvalidOid;

	amroutine->ambuild = spgbuild;
	amroutine->ambuildempty = spgbuildempty;
	amroutine->aminsert = spginsert;
	amroutine->ambulkdelete = spgbulkdelete;
	amroutine->amvacuumcleanup = spgvacuumcleanup;
	amroutine->amcanreturn = spgcanreturn;
	amroutine->amcostestimate = spgcostestimate;
	amroutine->amoptions = spgoptions;
	amroutine->amproperty = spgproperty;
	amroutine->ambuildphasename = NULL;
	amroutine->amvalidate = spgvalidate;
	amroutine->ambeginscan = spgbeginscan;
	amroutine->amrescan = spgrescan;
	amroutine->amgettuple = spggettuple;
	amroutine->amgetbitmap = spggetbitmap;
	amroutine->amendscan = spgendscan;
	amroutine->ammarkpos = NULL;
	amroutine->amrestrpos = NULL;
	amroutine->amestimateparallelscan = NULL;
	amroutine->aminitparallelscan = NULL;
	amroutine->amparallelrescan = NULL;

	PG_RETURN_POINTER(amroutine);
}

/* Fill in a SpGistTypeDesc struct with info about the specified data type */
static void
fillTypeDesc(SpGistTypeDesc *desc, Oid type)
{
	desc->type = type;
	get_typlenbyval(type, &desc->attlen, &desc->attbyval);
}

/*
 * Fetch local cache of AM-specific info about the index, initializing it
 * if necessary
 */
SpGistCache *
spgGetCache(Relation index)
{
	SpGistCache *cache;

	if (index->rd_amcache == NULL)
	{
		Oid			atttype;
		spgConfigIn in;
		FmgrInfo   *procinfo;
		Buffer		metabuffer;
		SpGistMetaPageData *metadata;

		cache = MemoryContextAllocZero(index->rd_indexcxt,
									   sizeof(SpGistCache));

		/* SPGiST doesn't support multi-column indexes */
		Assert(index->rd_att->natts == 1);

		/*
		 * Get the actual data type of the indexed column from the index
		 * tupdesc.  We pass this to the opclass config function so that
		 * polymorphic opclasses are possible.
		 */
		atttype = TupleDescAttr(index->rd_att, 0)->atttypid;

		/* Call the config function to get config info for the opclass */
		in.attType = atttype;

		procinfo = index_getprocinfo(index, 1, SPGIST_CONFIG_PROC);
		FunctionCall2Coll(procinfo,
						  index->rd_indcollation[0],
						  PointerGetDatum(&in),
						  PointerGetDatum(&cache->config));

		/* Get the information we need about each relevant datatype */
		fillTypeDesc(&cache->attType, atttype);

		if (OidIsValid(cache->config.leafType) &&
			cache->config.leafType != atttype)
		{
			if (!OidIsValid(index_getprocid(index, 1, SPGIST_COMPRESS_PROC)))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("compress method must be defined when leaf type is different from input type")));

			fillTypeDesc(&cache->attLeafType, cache->config.leafType);
		}
		else
		{
			cache->attLeafType = cache->attType;
		}

		fillTypeDesc(&cache->attPrefixType, cache->config.prefixType);
		fillTypeDesc(&cache->attLabelType, cache->config.labelType);

		/* Last, get the lastUsedPages data from the metapage */
		metabuffer = ReadBuffer(index, SPGIST_METAPAGE_BLKNO);
		LockBuffer(metabuffer, BUFFER_LOCK_SHARE);

		metadata = SpGistPageGetMeta(BufferGetPage(metabuffer));

		if (metadata->magicNumber != SPGIST_MAGIC_NUMBER)
			elog(ERROR, "index \"%s\" is not an SP-GiST index",
				 RelationGetRelationName(index));

		cache->lastUsedPages = metadata->lastUsedPages;

		UnlockReleaseBuffer(metabuffer);

		index->rd_amcache = (void *) cache;
	}
	else
	{
		/* assume it's up to date */
		cache = (SpGistCache *) index->rd_amcache;
	}

	return cache;
}

/* Initialize SpGistState for working with the given index */
void
initSpGistState(SpGistState *state, Relation index)
{
	SpGistCache *cache;

	/* Get cached static information about index */
	cache = spgGetCache(index);

	state->config = cache->config;
	state->attType = cache->attType;
	state->attLeafType = cache->attLeafType;
	state->attPrefixType = cache->attPrefixType;
	state->attLabelType = cache->attLabelType;

	/* Make workspace for constructing dead tuples */
	state->deadTupleStorage = palloc0(SGDTSIZE);

	/* Set XID to use in redirection tuples */
	state->myXid = GetTopTransactionIdIfAny();

	/* Assume we're not in an index build (spgbuild will override) */
	state->isBuild = false;
}

/*
 * Allocate a new page (either by recycling, or by extending the index file).
 *
 * The returned buffer is already pinned and exclusive-locked.
 * Caller is responsible for initializing the page by calling SpGistInitBuffer.
 */
Buffer
SpGistNewBuffer(Relation index)
{
	Buffer		buffer;
	bool		needLock;

	/* First, try to get a page from FSM */
	for (;;)
	{
		BlockNumber blkno = GetFreeIndexPage(index);

		if (blkno == InvalidBlockNumber)
			break;				/* nothing known to FSM */

		/*
		 * The fixed pages shouldn't ever be listed in FSM, but just in case
		 * one is, ignore it.
		 */
		if (SpGistBlockIsFixed(blkno))
			continue;

		buffer = ReadBuffer(index, blkno);

		/*
		 * We have to guard against the possibility that someone else already
		 * recycled this page; the buffer may be locked if so.
		 */
		if (ConditionalLockBuffer(buffer))
		{
			Page		page = BufferGetPage(buffer);

			if (PageIsNew(page))
				return buffer;	/* OK to use, if never initialized */

			if (SpGistPageIsDeleted(page) || PageIsEmpty(page))
				return buffer;	/* OK to use */

			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		}

		/* Can't use it, so release buffer and try again */
		ReleaseBuffer(buffer);
	}

	/* Must extend the file */
	needLock = !RELATION_IS_LOCAL(index);
	if (needLock)
		LockRelationForExtension(index, ExclusiveLock);

	buffer = ReadBuffer(index, P_NEW);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	if (needLock)
		UnlockRelationForExtension(index, ExclusiveLock);

	return buffer;
}

/*
 * Update index metapage's lastUsedPages info from local cache, if possible
 *
 * Updating meta page isn't critical for index working, so
 * 1 use ConditionalLockBuffer to improve concurrency
 * 2 don't WAL-log metabuffer changes to decrease WAL traffic
 */
void
SpGistUpdateMetaPage(Relation index)
{
	SpGistCache *cache = (SpGistCache *) index->rd_amcache;

	if (cache != NULL)
	{
		Buffer		metabuffer;

		metabuffer = ReadBuffer(index, SPGIST_METAPAGE_BLKNO);

		if (ConditionalLockBuffer(metabuffer))
		{
			Page		metapage = BufferGetPage(metabuffer);
			SpGistMetaPageData *metadata = SpGistPageGetMeta(metapage);

			metadata->lastUsedPages = cache->lastUsedPages;

			/*
			 * Set pd_lower just past the end of the metadata.  This is
			 * essential, because without doing so, metadata will be lost if
			 * xlog.c compresses the page.  (We must do this here because
			 * pre-v11 versions of PG did not set the metapage's pd_lower
			 * correctly, so a pg_upgraded index might contain the wrong
			 * value.)
			 */
			((PageHeader) metapage)->pd_lower =
				((char *) metadata + sizeof(SpGistMetaPageData)) - (char *) metapage;

			MarkBufferDirty(metabuffer);
			UnlockReleaseBuffer(metabuffer);
		}
		else
		{
			ReleaseBuffer(metabuffer);
		}
	}
}

/* Macro to select proper element of lastUsedPages cache depending on flags */
/* Masking flags with SPGIST_CACHED_PAGES is just for paranoia's sake */
#define GET_LUP(c, f)  (&(c)->lastUsedPages.cachedPage[((unsigned int) (f)) % SPGIST_CACHED_PAGES])

/*
 * Allocate and initialize a new buffer of the type and parity specified by
 * flags.  The returned buffer is already pinned and exclusive-locked.
 *
 * When requesting an inner page, if we get one with the wrong parity,
 * we just release the buffer and try again.  We will get a different page
 * because GetFreeIndexPage will have marked the page used in FSM.  The page
 * is entered in our local lastUsedPages cache, so there's some hope of
 * making use of it later in this session, but otherwise we rely on VACUUM
 * to eventually re-enter the page in FSM, making it available for recycling.
 * Note that such a page does not get marked dirty here, so unless it's used
 * fairly soon, the buffer will just get discarded and the page will remain
 * as it was on disk.
 *
 * When we return a buffer to the caller, the page is *not* entered into
 * the lastUsedPages cache; we expect the caller will do so after it's taken
 * whatever space it will use.  This is because after the caller has used up
 * some space, the page might have less space than whatever was cached already
 * so we'd rather not trash the old cache entry.
 */
static Buffer
allocNewBuffer(Relation index, int flags)
{
	SpGistCache *cache = spgGetCache(index);
	uint16		pageflags = 0;

	if (GBUF_REQ_LEAF(flags))
		pageflags |= SPGIST_LEAF;
	if (GBUF_REQ_NULLS(flags))
		pageflags |= SPGIST_NULLS;

	for (;;)
	{
		Buffer		buffer;

		buffer = SpGistNewBuffer(index);
		SpGistInitBuffer(buffer, pageflags);

		if (pageflags & SPGIST_LEAF)
		{
			/* Leaf pages have no parity concerns, so just use it */
			return buffer;
		}
		else
		{
			BlockNumber blkno = BufferGetBlockNumber(buffer);
			int			blkFlags = GBUF_INNER_PARITY(blkno);

			if ((flags & GBUF_PARITY_MASK) == blkFlags)
			{
				/* Page has right parity, use it */
				return buffer;
			}
			else
			{
				/* Page has wrong parity, record it in cache and try again */
				if (pageflags & SPGIST_NULLS)
					blkFlags |= GBUF_NULLS;
				cache->lastUsedPages.cachedPage[blkFlags].blkno = blkno;
				cache->lastUsedPages.cachedPage[blkFlags].freeSpace =
					PageGetExactFreeSpace(BufferGetPage(buffer));
				UnlockReleaseBuffer(buffer);
			}
		}
	}
}

/*
 * Get a buffer of the type and parity specified by flags, having at least
 * as much free space as indicated by needSpace.  We use the lastUsedPages
 * cache to assign the same buffer previously requested when possible.
 * The returned buffer is already pinned and exclusive-locked.
 *
 * *isNew is set true if the page was initialized here, false if it was
 * already valid.
 */
Buffer
SpGistGetBuffer(Relation index, int flags, int needSpace, bool *isNew)
{
	SpGistCache *cache = spgGetCache(index);
	SpGistLastUsedPage *lup;

	/* Bail out if even an empty page wouldn't meet the demand */
	if (needSpace > SPGIST_PAGE_CAPACITY)
		elog(ERROR, "desired SPGiST tuple size is too big");

	/*
	 * If possible, increase the space request to include relation's
	 * fillfactor.  This ensures that when we add unrelated tuples to a page,
	 * we try to keep 100-fillfactor% available for adding tuples that are
	 * related to the ones already on it.  But fillfactor mustn't cause an
	 * error for requests that would otherwise be legal.
	 */
	needSpace += SpGistGetTargetPageFreeSpace(index);
	needSpace = Min(needSpace, SPGIST_PAGE_CAPACITY);

	/* Get the cache entry for this flags setting */
	lup = GET_LUP(cache, flags);

	/* If we have nothing cached, just turn it over to allocNewBuffer */
	if (lup->blkno == InvalidBlockNumber)
	{
		*isNew = true;
		return allocNewBuffer(index, flags);
	}

	/* fixed pages should never be in cache */
	Assert(!SpGistBlockIsFixed(lup->blkno));

	/* If cached freeSpace isn't enough, don't bother looking at the page */
	if (lup->freeSpace >= needSpace)
	{
		Buffer		buffer;
		Page		page;

		buffer = ReadBuffer(index, lup->blkno);

		if (!ConditionalLockBuffer(buffer))
		{
			/*
			 * buffer is locked by another process, so return a new buffer
			 */
			ReleaseBuffer(buffer);
			*isNew = true;
			return allocNewBuffer(index, flags);
		}

		page = BufferGetPage(buffer);

		if (PageIsNew(page) || SpGistPageIsDeleted(page) || PageIsEmpty(page))
		{
			/* OK to initialize the page */
			uint16		pageflags = 0;

			if (GBUF_REQ_LEAF(flags))
				pageflags |= SPGIST_LEAF;
			if (GBUF_REQ_NULLS(flags))
				pageflags |= SPGIST_NULLS;
			SpGistInitBuffer(buffer, pageflags);
			lup->freeSpace = PageGetExactFreeSpace(page) - needSpace;
			*isNew = true;
			return buffer;
		}

		/*
		 * Check that page is of right type and has enough space.  We must
		 * recheck this since our cache isn't necessarily up to date.
		 */
		if ((GBUF_REQ_LEAF(flags) ? SpGistPageIsLeaf(page) : !SpGistPageIsLeaf(page)) &&
			(GBUF_REQ_NULLS(flags) ? SpGistPageStoresNulls(page) : !SpGistPageStoresNulls(page)))
		{
			int			freeSpace = PageGetExactFreeSpace(page);

			if (freeSpace >= needSpace)
			{
				/* Success, update freespace info and return the buffer */
				lup->freeSpace = freeSpace - needSpace;
				*isNew = false;
				return buffer;
			}
		}

		/*
		 * fallback to allocation of new buffer
		 */
		UnlockReleaseBuffer(buffer);
	}

	/* No success with cache, so return a new buffer */
	*isNew = true;
	return allocNewBuffer(index, flags);
}

/*
 * Update lastUsedPages cache when done modifying a page.
 *
 * We update the appropriate cache entry if it already contained this page
 * (its freeSpace is likely obsolete), or if this page has more space than
 * whatever we had cached.
 */
void
SpGistSetLastUsedPage(Relation index, Buffer buffer)
{
	SpGistCache *cache = spgGetCache(index);
	SpGistLastUsedPage *lup;
	int			freeSpace;
	Page		page = BufferGetPage(buffer);
	BlockNumber blkno = BufferGetBlockNumber(buffer);
	int			flags;

	/* Never enter fixed pages (root pages) in cache, though */
	if (SpGistBlockIsFixed(blkno))
		return;

	if (SpGistPageIsLeaf(page))
		flags = GBUF_LEAF;
	else
		flags = GBUF_INNER_PARITY(blkno);
	if (SpGistPageStoresNulls(page))
		flags |= GBUF_NULLS;

	lup = GET_LUP(cache, flags);

	freeSpace = PageGetExactFreeSpace(page);
	if (lup->blkno == InvalidBlockNumber || lup->blkno == blkno ||
		lup->freeSpace < freeSpace)
	{
		lup->blkno = blkno;
		lup->freeSpace = freeSpace;
	}
}

/*
 * Initialize an SPGiST page to empty, with specified flags
 */
void
SpGistInitPage(Page page, uint16 f)
{
	SpGistPageOpaque opaque;

	PageInit(page, BLCKSZ, MAXALIGN(sizeof(SpGistPageOpaqueData)));
	opaque = SpGistPageGetOpaque(page);
	memset(opaque, 0, sizeof(SpGistPageOpaqueData));
	opaque->flags = f;
	opaque->spgist_page_id = SPGIST_PAGE_ID;
}

/*
 * Initialize a buffer's page to empty, with specified flags
 */
void
SpGistInitBuffer(Buffer b, uint16 f)
{
	Assert(BufferGetPageSize(b) == BLCKSZ);
	SpGistInitPage(BufferGetPage(b), f);
}

/*
 * Initialize metadata page
 */
void
SpGistInitMetapage(Page page)
{
	SpGistMetaPageData *metadata;
	int			i;

	SpGistInitPage(page, SPGIST_META);
	metadata = SpGistPageGetMeta(page);
	memset(metadata, 0, sizeof(SpGistMetaPageData));
	metadata->magicNumber = SPGIST_MAGIC_NUMBER;

	/* initialize last-used-page cache to empty */
	for (i = 0; i < SPGIST_CACHED_PAGES; i++)
		metadata->lastUsedPages.cachedPage[i].blkno = InvalidBlockNumber;

	/*
	 * Set pd_lower just past the end of the metadata.  This is essential,
	 * because without doing so, metadata will be lost if xlog.c compresses
	 * the page.
	 */
	((PageHeader) page)->pd_lower =
		((char *) metadata + sizeof(SpGistMetaPageData)) - (char *) page;
}

/*
 * reloptions processing for SPGiST
 */
bytea *
spgoptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"fillfactor", RELOPT_TYPE_INT, offsetof(SpGistOptions, fillfactor)},
	};

	return (bytea *) build_reloptions(reloptions, validate,
									  RELOPT_KIND_SPGIST,
									  sizeof(SpGistOptions),
									  tab, lengthof(tab));

}

/*
 * Get the space needed to store a non-null datum of the indicated type.
 * Note the result is already rounded up to a MAXALIGN boundary.
 * Also, we follow the SPGiST convention that pass-by-val types are
 * just stored in their Datum representation (compare memcpyDatum).
 */
unsigned int
SpGistGetTypeSize(SpGistTypeDesc *att, Datum datum)
{
	unsigned int size;

	if (att->attbyval)
		size = sizeof(Datum);
	else if (att->attlen > 0)
		size = att->attlen;
	else
		size = VARSIZE_ANY(datum);

	return MAXALIGN(size);
}

/*
 * Copy the given non-null datum to *target
 */
static void
memcpyDatum(void *target, SpGistTypeDesc *att, Datum datum)
{
	unsigned int size;

	if (att->attbyval)
	{
		memcpy(target, &datum, sizeof(Datum));
	}
	else
	{
		size = (att->attlen > 0) ? att->attlen : VARSIZE_ANY(datum);
		memcpy(target, DatumGetPointer(datum), size);
	}
}

/*
 * Construct a leaf tuple containing the given heap TID and datum value
 */
SpGistLeafTuple
spgFormLeafTuple(SpGistState *state, ItemPointer heapPtr,
				 Datum datum, bool isnull)
{
	SpGistLeafTuple tup;
	unsigned int size;

	/* compute space needed (note result is already maxaligned) */
	size = SGLTHDRSZ;
	if (!isnull)
		size += SpGistGetTypeSize(&state->attLeafType, datum);

	/*
	 * Ensure that we can replace the tuple with a dead tuple later.  This
	 * test is unnecessary when !isnull, but let's be safe.
	 */
	if (size < SGDTSIZE)
		size = SGDTSIZE;

	/* OK, form the tuple */
	tup = (SpGistLeafTuple) palloc0(size);

	tup->size = size;
	tup->nextOffset = InvalidOffsetNumber;
	tup->heapPtr = *heapPtr;
	if (!isnull)
		memcpyDatum(SGLTDATAPTR(tup), &state->attLeafType, datum);

	return tup;
}

/*
 * Construct a node (to go into an inner tuple) containing the given label
 *
 * Note that the node's downlink is just set invalid here.  Caller will fill
 * it in later.
 */
SpGistNodeTuple
spgFormNodeTuple(SpGistState *state, Datum label, bool isnull)
{
	SpGistNodeTuple tup;
	unsigned int size;
	unsigned short infomask = 0;

	/* compute space needed (note result is already maxaligned) */
	size = SGNTHDRSZ;
	if (!isnull)
		size += SpGistGetTypeSize(&state->attLabelType, label);

	/*
	 * Here we make sure that the size will fit in the field reserved for it
	 * in t_info.
	 */
	if ((size & INDEX_SIZE_MASK) != size)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("index row requires %zu bytes, maximum size is %zu",
						(Size) size, (Size) INDEX_SIZE_MASK)));

	tup = (SpGistNodeTuple) palloc0(size);

	if (isnull)
		infomask |= INDEX_NULL_MASK;
	/* we don't bother setting the INDEX_VAR_MASK bit */
	infomask |= size;
	tup->t_info = infomask;

	/* The TID field will be filled in later */
	ItemPointerSetInvalid(&tup->t_tid);

	if (!isnull)
		memcpyDatum(SGNTDATAPTR(tup), &state->attLabelType, label);

	return tup;
}

/*
 * Construct an inner tuple containing the given prefix and node array
 */
SpGistInnerTuple
spgFormInnerTuple(SpGistState *state, bool hasPrefix, Datum prefix,
				  int nNodes, SpGistNodeTuple *nodes)
{
	SpGistInnerTuple tup;
	unsigned int size;
	unsigned int prefixSize;
	int			i;
	char	   *ptr;

	/* Compute size needed */
	if (hasPrefix)
		prefixSize = SpGistGetTypeSize(&state->attPrefixType, prefix);
	else
		prefixSize = 0;

	size = SGITHDRSZ + prefixSize;

	/* Note: we rely on node tuple sizes to be maxaligned already */
	for (i = 0; i < nNodes; i++)
		size += IndexTupleSize(nodes[i]);

	/*
	 * Ensure that we can replace the tuple with a dead tuple later.  This
	 * test is unnecessary given current tuple layouts, but let's be safe.
	 */
	if (size < SGDTSIZE)
		size = SGDTSIZE;

	/*
	 * Inner tuple should be small enough to fit on a page
	 */
	if (size > SPGIST_PAGE_CAPACITY - sizeof(ItemIdData))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("SP-GiST inner tuple size %zu exceeds maximum %zu",
						(Size) size,
						SPGIST_PAGE_CAPACITY - sizeof(ItemIdData)),
				 errhint("Values larger than a buffer page cannot be indexed.")));

	/*
	 * Check for overflow of header fields --- probably can't fail if the
	 * above succeeded, but let's be paranoid
	 */
	if (size > SGITMAXSIZE ||
		prefixSize > SGITMAXPREFIXSIZE ||
		nNodes > SGITMAXNNODES)
		elog(ERROR, "SPGiST inner tuple header field is too small");

	/* OK, form the tuple */
	tup = (SpGistInnerTuple) palloc0(size);

	tup->nNodes = nNodes;
	tup->prefixSize = prefixSize;
	tup->size = size;

	if (hasPrefix)
		memcpyDatum(SGITDATAPTR(tup), &state->attPrefixType, prefix);

	ptr = (char *) SGITNODEPTR(tup);

	for (i = 0; i < nNodes; i++)
	{
		SpGistNodeTuple node = nodes[i];

		memcpy(ptr, node, IndexTupleSize(node));
		ptr += IndexTupleSize(node);
	}

	return tup;
}

/*
 * Construct a "dead" tuple to replace a tuple being deleted.
 *
 * The state can be SPGIST_REDIRECT, SPGIST_DEAD, or SPGIST_PLACEHOLDER.
 * For a REDIRECT tuple, a pointer (blkno+offset) must be supplied, and
 * the xid field is filled in automatically.
 *
 * This is called in critical sections, so we don't use palloc; the tuple
 * is built in preallocated storage.  It should be copied before another
 * call with different parameters can occur.
 */
SpGistDeadTuple
spgFormDeadTuple(SpGistState *state, int tupstate,
				 BlockNumber blkno, OffsetNumber offnum)
{
	SpGistDeadTuple tuple = (SpGistDeadTuple) state->deadTupleStorage;

	tuple->tupstate = tupstate;
	tuple->size = SGDTSIZE;
	tuple->nextOffset = InvalidOffsetNumber;

	if (tupstate == SPGIST_REDIRECT)
	{
		ItemPointerSet(&tuple->pointer, blkno, offnum);
		Assert(TransactionIdIsValid(state->myXid));
		tuple->xid = state->myXid;
	}
	else
	{
		ItemPointerSetInvalid(&tuple->pointer);
		tuple->xid = InvalidTransactionId;
	}

	return tuple;
}

/*
 * Extract the label datums of the nodes within innerTuple
 *
 * Returns NULL if label datums are NULLs
 */
Datum *
spgExtractNodeLabels(SpGistState *state, SpGistInnerTuple innerTuple)
{
	Datum	   *nodeLabels;
	int			i;
	SpGistNodeTuple node;

	/* Either all the labels must be NULL, or none. */
	node = SGITNODEPTR(innerTuple);
	if (IndexTupleHasNulls(node))
	{
		SGITITERATE(innerTuple, i, node)
		{
			if (!IndexTupleHasNulls(node))
				elog(ERROR, "some but not all node labels are null in SPGiST inner tuple");
		}
		/* They're all null, so just return NULL */
		return NULL;
	}
	else
	{
		nodeLabels = (Datum *) palloc(sizeof(Datum) * innerTuple->nNodes);
		SGITITERATE(innerTuple, i, node)
		{
			if (IndexTupleHasNulls(node))
				elog(ERROR, "some but not all node labels are null in SPGiST inner tuple");
			nodeLabels[i] = SGNTDATUM(node, state);
		}
		return nodeLabels;
	}
}

/*
 * Add a new item to the page, replacing a PLACEHOLDER item if possible.
 * Return the location it's inserted at, or InvalidOffsetNumber on failure.
 *
 * If startOffset isn't NULL, we start searching for placeholders at
 * *startOffset, and update that to the next place to search.  This is just
 * an optimization for repeated insertions.
 *
 * If errorOK is false, we throw error when there's not enough room,
 * rather than returning InvalidOffsetNumber.
 */
OffsetNumber
SpGistPageAddNewItem(SpGistState *state, Page page, Item item, Size size,
					 OffsetNumber *startOffset, bool errorOK)
{
	SpGistPageOpaque opaque = SpGistPageGetOpaque(page);
	OffsetNumber i,
				maxoff,
				offnum;

	if (opaque->nPlaceholder > 0 &&
		PageGetExactFreeSpace(page) + SGDTSIZE >= MAXALIGN(size))
	{
		/* Try to replace a placeholder */
		maxoff = PageGetMaxOffsetNumber(page);
		offnum = InvalidOffsetNumber;

		for (;;)
		{
			if (startOffset && *startOffset != InvalidOffsetNumber)
				i = *startOffset;
			else
				i = FirstOffsetNumber;
			for (; i <= maxoff; i++)
			{
				SpGistDeadTuple it = (SpGistDeadTuple) PageGetItem(page,
																   PageGetItemId(page, i));

				if (it->tupstate == SPGIST_PLACEHOLDER)
				{
					offnum = i;
					break;
				}
			}

			/* Done if we found a placeholder */
			if (offnum != InvalidOffsetNumber)
				break;

			if (startOffset && *startOffset != InvalidOffsetNumber)
			{
				/* Hint was no good, re-search from beginning */
				*startOffset = InvalidOffsetNumber;
				continue;
			}

			/* Hmm, no placeholder found? */
			opaque->nPlaceholder = 0;
			break;
		}

		if (offnum != InvalidOffsetNumber)
		{
			/* Replace the placeholder tuple */
			PageIndexTupleDelete(page, offnum);

			offnum = PageAddItem(page, item, size, offnum, false, false);

			/*
			 * We should not have failed given the size check at the top of
			 * the function, but test anyway.  If we did fail, we must PANIC
			 * because we've already deleted the placeholder tuple, and
			 * there's no other way to keep the damage from getting to disk.
			 */
			if (offnum != InvalidOffsetNumber)
			{
				Assert(opaque->nPlaceholder > 0);
				opaque->nPlaceholder--;
				if (startOffset)
					*startOffset = offnum + 1;
			}
			else
				elog(PANIC, "failed to add item of size %u to SPGiST index page",
					 (int) size);

			return offnum;
		}
	}

	/* No luck in replacing a placeholder, so just add it to the page */
	offnum = PageAddItem(page, item, size,
						 InvalidOffsetNumber, false, false);

	if (offnum == InvalidOffsetNumber && !errorOK)
		elog(ERROR, "failed to add item of size %u to SPGiST index page",
			 (int) size);

	return offnum;
}

/*
 *	spgproperty() -- Check boolean properties of indexes.
 *
 * This is optional for most AMs, but is required for SP-GiST because the core
 * property code doesn't support AMPROP_DISTANCE_ORDERABLE.
 */
bool
spgproperty(Oid index_oid, int attno,
			IndexAMProperty prop, const char *propname,
			bool *res, bool *isnull)
{
	Oid			opclass,
				opfamily,
				opcintype;
	CatCList   *catlist;
	int			i;

	/* Only answer column-level inquiries */
	if (attno == 0)
		return false;

	switch (prop)
	{
		case AMPROP_DISTANCE_ORDERABLE:
			break;
		default:
			return false;
	}

	/*
	 * Currently, SP-GiST distance-ordered scans require that there be a
	 * distance operator in the opclass with the default types. So we assume
	 * that if such an operator exists, then there's a reason for it.
	 */

	/* First we need to know the column's opclass. */
	opclass = get_index_column_opclass(index_oid, attno);
	if (!OidIsValid(opclass))
	{
		*isnull = true;
		return true;
	}

	/* Now look up the opclass family and input datatype. */
	if (!get_opclass_opfamily_and_input_type(opclass, &opfamily, &opcintype))
	{
		*isnull = true;
		return true;
	}

	/* And now we can check whether the operator is provided. */
	catlist = SearchSysCacheList1(AMOPSTRATEGY,
								  ObjectIdGetDatum(opfamily));

	*res = false;

	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	amoptup = &catlist->members[i]->tuple;
		Form_pg_amop amopform = (Form_pg_amop) GETSTRUCT(amoptup);

		if (amopform->amoppurpose == AMOP_ORDER &&
			(amopform->amoplefttype == opcintype ||
			 amopform->amoprighttype == opcintype) &&
			opfamily_can_sort_type(amopform->amopsortfamily,
								   get_op_rettype(amopform->amopopr)))
		{
			*res = true;
			break;
		}
	}

	ReleaseSysCacheList(catlist);

	*isnull = false;

	return true;
}
