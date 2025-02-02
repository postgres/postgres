/*-------------------------------------------------------------------------
 *
 * spgutils.c
 *	  various support functions for SP-GiST
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
#include "access/toast_compression.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/pg_amop.h"
#include "commands/vacuum.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_coerce.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "utils/catcache.h"
#include "utils/fmgrprotos.h"
#include "utils/index_selfuncs.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
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
	amroutine->amstorage = true;
	amroutine->amclusterable = false;
	amroutine->ampredlocks = false;
	amroutine->amcanparallel = false;
	amroutine->amcanbuildparallel = false;
	amroutine->amcaninclude = true;
	amroutine->amusemaintenanceworkmem = false;
	amroutine->amsummarizing = false;
	amroutine->amparallelvacuumoptions =
		VACUUM_OPTION_PARALLEL_BULKDEL | VACUUM_OPTION_PARALLEL_COND_CLEANUP;
	amroutine->amkeytype = InvalidOid;

	amroutine->ambuild = spgbuild;
	amroutine->ambuildempty = spgbuildempty;
	amroutine->aminsert = spginsert;
	amroutine->aminsertcleanup = NULL;
	amroutine->ambulkdelete = spgbulkdelete;
	amroutine->amvacuumcleanup = spgvacuumcleanup;
	amroutine->amcanreturn = spgcanreturn;
	amroutine->amcostestimate = spgcostestimate;
	amroutine->amgettreeheight = NULL;
	amroutine->amoptions = spgoptions;
	amroutine->amproperty = spgproperty;
	amroutine->ambuildphasename = NULL;
	amroutine->amvalidate = spgvalidate;
	amroutine->amadjustmembers = spgadjustmembers;
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
	amroutine->amtranslatestrategy = NULL;
	amroutine->amtranslatecmptype = NULL;

	PG_RETURN_POINTER(amroutine);
}

/*
 * GetIndexInputType
 *		Determine the nominal input data type for an index column
 *
 * We define the "nominal" input type as the associated opclass's opcintype,
 * or if that is a polymorphic type, the base type of the heap column or
 * expression that is the index's input.  The reason for preferring the
 * opcintype is that non-polymorphic opclasses probably don't want to hear
 * about binary-compatible input types.  For instance, if a text opclass
 * is being used with a varchar heap column, we want to report "text" not
 * "varchar".  Likewise, opclasses don't want to hear about domain types,
 * so if we do consult the actual input type, we make sure to flatten domains.
 *
 * At some point maybe this should go somewhere else, but it's not clear
 * if any other index AMs have a use for it.
 */
static Oid
GetIndexInputType(Relation index, AttrNumber indexcol)
{
	Oid			opcintype;
	AttrNumber	heapcol;
	List	   *indexprs;
	ListCell   *indexpr_item;

	Assert(index->rd_index != NULL);
	Assert(indexcol > 0 && indexcol <= index->rd_index->indnkeyatts);
	opcintype = index->rd_opcintype[indexcol - 1];
	if (!IsPolymorphicType(opcintype))
		return opcintype;
	heapcol = index->rd_index->indkey.values[indexcol - 1];
	if (heapcol != 0)			/* Simple index column? */
		return getBaseType(get_atttype(index->rd_index->indrelid, heapcol));

	/*
	 * If the index expressions are already cached, skip calling
	 * RelationGetIndexExpressions, as it will make a copy which is overkill.
	 * We're not going to modify the trees, and we're not going to do anything
	 * that would invalidate the relcache entry before we're done.
	 */
	if (index->rd_indexprs)
		indexprs = index->rd_indexprs;
	else
		indexprs = RelationGetIndexExpressions(index);
	indexpr_item = list_head(indexprs);
	for (int i = 1; i <= index->rd_index->indnkeyatts; i++)
	{
		if (index->rd_index->indkey.values[i - 1] == 0)
		{
			/* expression column */
			if (indexpr_item == NULL)
				elog(ERROR, "wrong number of index expressions");
			if (i == indexcol)
				return getBaseType(exprType((Node *) lfirst(indexpr_item)));
			indexpr_item = lnext(indexprs, indexpr_item);
		}
	}
	elog(ERROR, "wrong number of index expressions");
	return InvalidOid;			/* keep compiler quiet */
}

/* Fill in a SpGistTypeDesc struct with info about the specified data type */
static void
fillTypeDesc(SpGistTypeDesc *desc, Oid type)
{
	HeapTuple	tp;
	Form_pg_type typtup;

	desc->type = type;
	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for type %u", type);
	typtup = (Form_pg_type) GETSTRUCT(tp);
	desc->attlen = typtup->typlen;
	desc->attbyval = typtup->typbyval;
	desc->attalign = typtup->typalign;
	desc->attstorage = typtup->typstorage;
	ReleaseSysCache(tp);
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

		cache = MemoryContextAllocZero(index->rd_indexcxt,
									   sizeof(SpGistCache));

		/* SPGiST must have one key column and can also have INCLUDE columns */
		Assert(IndexRelationGetNumberOfKeyAttributes(index) == 1);
		Assert(IndexRelationGetNumberOfAttributes(index) <= INDEX_MAX_KEYS);

		/*
		 * Get the actual (well, nominal) data type of the key column.  We
		 * pass this to the opclass config function so that polymorphic
		 * opclasses are possible.
		 */
		atttype = GetIndexInputType(index, spgKeyColumn + 1);

		/* Call the config function to get config info for the opclass */
		in.attType = atttype;

		procinfo = index_getprocinfo(index, 1, SPGIST_CONFIG_PROC);
		FunctionCall2Coll(procinfo,
						  index->rd_indcollation[spgKeyColumn],
						  PointerGetDatum(&in),
						  PointerGetDatum(&cache->config));

		/*
		 * If leafType isn't specified, use the declared index column type,
		 * which index.c will have derived from the opclass's opcintype.
		 * (Although we now make spgvalidate.c warn if these aren't the same,
		 * old user-defined opclasses may not set the STORAGE parameter
		 * correctly, so believe leafType if it's given.)
		 */
		if (!OidIsValid(cache->config.leafType))
		{
			cache->config.leafType =
				TupleDescAttr(RelationGetDescr(index), spgKeyColumn)->atttypid;

			/*
			 * If index column type is binary-coercible to atttype (for
			 * example, it's a domain over atttype), treat it as plain atttype
			 * to avoid thinking we need to compress.
			 */
			if (cache->config.leafType != atttype &&
				IsBinaryCoercible(cache->config.leafType, atttype))
				cache->config.leafType = atttype;
		}

		/* Get the information we need about each relevant datatype */
		fillTypeDesc(&cache->attType, atttype);

		if (cache->config.leafType != atttype)
		{
			if (!OidIsValid(index_getprocid(index, 1, SPGIST_COMPRESS_PROC)))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("compress method must be defined when leaf type is different from input type")));

			fillTypeDesc(&cache->attLeafType, cache->config.leafType);
		}
		else
		{
			/* Save lookups in this common case */
			cache->attLeafType = cache->attType;
		}

		fillTypeDesc(&cache->attPrefixType, cache->config.prefixType);
		fillTypeDesc(&cache->attLabelType, cache->config.labelType);

		/*
		 * Finally, if it's a real index (not a partitioned one), get the
		 * lastUsedPages data from the metapage
		 */
		if (index->rd_rel->relkind != RELKIND_PARTITIONED_INDEX)
		{
			Buffer		metabuffer;
			SpGistMetaPageData *metadata;

			metabuffer = ReadBuffer(index, SPGIST_METAPAGE_BLKNO);
			LockBuffer(metabuffer, BUFFER_LOCK_SHARE);

			metadata = SpGistPageGetMeta(BufferGetPage(metabuffer));

			if (metadata->magicNumber != SPGIST_MAGIC_NUMBER)
				elog(ERROR, "index \"%s\" is not an SP-GiST index",
					 RelationGetRelationName(index));

			cache->lastUsedPages = metadata->lastUsedPages;

			UnlockReleaseBuffer(metabuffer);
		}

		index->rd_amcache = cache;
	}
	else
	{
		/* assume it's up to date */
		cache = (SpGistCache *) index->rd_amcache;
	}

	return cache;
}

/*
 * Compute a tuple descriptor for leaf tuples or index-only-scan result tuples.
 *
 * We can use the relcache's tupdesc as-is in many cases, and it's always
 * OK so far as any INCLUDE columns are concerned.  However, the entry for
 * the key column has to match leafType in the first case or attType in the
 * second case.  While the relcache's tupdesc *should* show leafType, this
 * might not hold for legacy user-defined opclasses, since before v14 they
 * were not allowed to declare their true storage type in CREATE OPCLASS.
 * Also, attType can be different from what is in the relcache.
 *
 * This function gives back either a pointer to the relcache's tupdesc
 * if that is suitable, or a palloc'd copy that's been adjusted to match
 * the specified key column type.  We can avoid doing any catalog lookups
 * here by insisting that the caller pass an SpGistTypeDesc not just an OID.
 */
TupleDesc
getSpGistTupleDesc(Relation index, SpGistTypeDesc *keyType)
{
	TupleDesc	outTupDesc;
	Form_pg_attribute att;

	if (keyType->type ==
		TupleDescAttr(RelationGetDescr(index), spgKeyColumn)->atttypid)
		outTupDesc = RelationGetDescr(index);
	else
	{
		outTupDesc = CreateTupleDescCopy(RelationGetDescr(index));
		att = TupleDescAttr(outTupDesc, spgKeyColumn);
		/* It's sufficient to update the type-dependent fields of the column */
		att->atttypid = keyType->type;
		att->atttypmod = -1;
		att->attlen = keyType->attlen;
		att->attbyval = keyType->attbyval;
		att->attalign = keyType->attalign;
		att->attstorage = keyType->attstorage;
		/* We shouldn't need to bother with making these valid: */
		att->attcompression = InvalidCompressionMethod;
		att->attcollation = InvalidOid;
		/* In case we changed typlen, we'd better reset following offsets */
		for (int i = spgFirstIncludeColumn; i < outTupDesc->natts; i++)
			TupleDescCompactAttr(outTupDesc, i)->attcacheoff = -1;

		populate_compact_attribute(outTupDesc, spgKeyColumn);
	}
	return outTupDesc;
}

/* Initialize SpGistState for working with the given index */
void
initSpGistState(SpGistState *state, Relation index)
{
	SpGistCache *cache;

	state->index = index;

	/* Get cached static information about index */
	cache = spgGetCache(index);

	state->config = cache->config;
	state->attType = cache->attType;
	state->attLeafType = cache->attLeafType;
	state->attPrefixType = cache->attPrefixType;
	state->attLabelType = cache->attLabelType;

	/* Ensure we have a valid descriptor for leaf tuples */
	state->leafTupDesc = getSpGistTupleDesc(state->index, &state->attLeafType);

	/* Make workspace for constructing dead tuples */
	state->deadTupleStorage = palloc0(SGDTSIZE);

	/*
	 * Set horizon XID to use in redirection tuples.  Use our own XID if we
	 * have one, else use InvalidTransactionId.  The latter case can happen in
	 * VACUUM or REINDEX CONCURRENTLY, and in neither case would it be okay to
	 * force an XID to be assigned.  VACUUM won't create any redirection
	 * tuples anyway, but REINDEX CONCURRENTLY can.  Fortunately, REINDEX
	 * CONCURRENTLY doesn't mark the index valid until the end, so there could
	 * never be any concurrent scans "in flight" to a redirection tuple it has
	 * inserted.  And it locks out VACUUM until the end, too.  So it's okay
	 * for VACUUM to immediately expire a redirection tuple that contains an
	 * invalid xid.
	 */
	state->redirectXid = GetTopTransactionIdIfAny();

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

	buffer = ExtendBufferedRel(BMR_REL(index), MAIN_FORKNUM, NULL,
							   EB_LOCK_FIRST);

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

	PageInit(page, BLCKSZ, sizeof(SpGistPageOpaqueData));
	opaque = SpGistPageGetOpaque(page);
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
 * Get the space needed to store a non-null datum of the indicated type
 * in an inner tuple (that is, as a prefix or node label).
 * Note the result is already rounded up to a MAXALIGN boundary.
 * Here we follow the convention that pass-by-val types are just stored
 * in their Datum representation (compare memcpyInnerDatum).
 */
unsigned int
SpGistGetInnerTypeSize(SpGistTypeDesc *att, Datum datum)
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
 * Copy the given non-null datum to *target, in the inner-tuple case
 */
static void
memcpyInnerDatum(void *target, SpGistTypeDesc *att, Datum datum)
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
 * Compute space required for a leaf tuple holding the given data.
 *
 * This must match the size-calculation portion of spgFormLeafTuple.
 */
Size
SpGistGetLeafTupleSize(TupleDesc tupleDescriptor,
					   const Datum *datums, const bool *isnulls)
{
	Size		size;
	Size		data_size;
	bool		needs_null_mask = false;
	int			natts = tupleDescriptor->natts;

	/*
	 * Decide whether we need a nulls bitmask.
	 *
	 * If there is only a key attribute (natts == 1), never use a bitmask, for
	 * compatibility with the pre-v14 layout of leaf tuples.  Otherwise, we
	 * need one if any attribute is null.
	 */
	if (natts > 1)
	{
		for (int i = 0; i < natts; i++)
		{
			if (isnulls[i])
			{
				needs_null_mask = true;
				break;
			}
		}
	}

	/*
	 * Calculate size of the data part; same as for heap tuples.
	 */
	data_size = heap_compute_data_size(tupleDescriptor, datums, isnulls);

	/*
	 * Compute total size.
	 */
	size = SGLTHDRSZ(needs_null_mask);
	size += data_size;
	size = MAXALIGN(size);

	/*
	 * Ensure that we can replace the tuple with a dead tuple later. This test
	 * is unnecessary when there are any non-null attributes, but be safe.
	 */
	if (size < SGDTSIZE)
		size = SGDTSIZE;

	return size;
}

/*
 * Construct a leaf tuple containing the given heap TID and datum values
 */
SpGistLeafTuple
spgFormLeafTuple(SpGistState *state, ItemPointer heapPtr,
				 const Datum *datums, const bool *isnulls)
{
	SpGistLeafTuple tup;
	TupleDesc	tupleDescriptor = state->leafTupDesc;
	Size		size;
	Size		hoff;
	Size		data_size;
	bool		needs_null_mask = false;
	int			natts = tupleDescriptor->natts;
	char	   *tp;				/* ptr to tuple data */
	uint16		tupmask = 0;	/* unused heap_fill_tuple output */

	/*
	 * Decide whether we need a nulls bitmask.
	 *
	 * If there is only a key attribute (natts == 1), never use a bitmask, for
	 * compatibility with the pre-v14 layout of leaf tuples.  Otherwise, we
	 * need one if any attribute is null.
	 */
	if (natts > 1)
	{
		for (int i = 0; i < natts; i++)
		{
			if (isnulls[i])
			{
				needs_null_mask = true;
				break;
			}
		}
	}

	/*
	 * Calculate size of the data part; same as for heap tuples.
	 */
	data_size = heap_compute_data_size(tupleDescriptor, datums, isnulls);

	/*
	 * Compute total size.
	 */
	hoff = SGLTHDRSZ(needs_null_mask);
	size = hoff + data_size;
	size = MAXALIGN(size);

	/*
	 * Ensure that we can replace the tuple with a dead tuple later. This test
	 * is unnecessary when there are any non-null attributes, but be safe.
	 */
	if (size < SGDTSIZE)
		size = SGDTSIZE;

	/* OK, form the tuple */
	tup = (SpGistLeafTuple) palloc0(size);

	tup->size = size;
	SGLT_SET_NEXTOFFSET(tup, InvalidOffsetNumber);
	tup->heapPtr = *heapPtr;

	tp = (char *) tup + hoff;

	if (needs_null_mask)
	{
		bits8	   *bp;			/* ptr to null bitmap in tuple */

		/* Set nullmask presence bit in SpGistLeafTuple header */
		SGLT_SET_HASNULLMASK(tup, true);
		/* Fill the data area and null mask */
		bp = (bits8 *) ((char *) tup + sizeof(SpGistLeafTupleData));
		heap_fill_tuple(tupleDescriptor, datums, isnulls, tp, data_size,
						&tupmask, bp);
	}
	else if (natts > 1 || !isnulls[spgKeyColumn])
	{
		/* Fill data area only */
		heap_fill_tuple(tupleDescriptor, datums, isnulls, tp, data_size,
						&tupmask, (bits8 *) NULL);
	}
	/* otherwise we have no data, nor a bitmap, to fill */

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
		size += SpGistGetInnerTypeSize(&state->attLabelType, label);

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
		memcpyInnerDatum(SGNTDATAPTR(tup), &state->attLabelType, label);

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
		prefixSize = SpGistGetInnerTypeSize(&state->attPrefixType, prefix);
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
		memcpyInnerDatum(SGITDATAPTR(tup), &state->attPrefixType, prefix);

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
	SGLT_SET_NEXTOFFSET(tuple, InvalidOffsetNumber);

	if (tupstate == SPGIST_REDIRECT)
	{
		ItemPointerSet(&tuple->pointer, blkno, offnum);
		tuple->xid = state->redirectXid;
	}
	else
	{
		ItemPointerSetInvalid(&tuple->pointer);
		tuple->xid = InvalidTransactionId;
	}

	return tuple;
}

/*
 * Convert an SPGiST leaf tuple into Datum/isnull arrays.
 *
 * The caller must allocate sufficient storage for the output arrays.
 * (INDEX_MAX_KEYS entries should be enough.)
 */
void
spgDeformLeafTuple(SpGistLeafTuple tup, TupleDesc tupleDescriptor,
				   Datum *datums, bool *isnulls, bool keyColumnIsNull)
{
	bool		hasNullsMask = SGLT_GET_HASNULLMASK(tup);
	char	   *tp;				/* ptr to tuple data */
	bits8	   *bp;				/* ptr to null bitmap in tuple */

	if (keyColumnIsNull && tupleDescriptor->natts == 1)
	{
		/*
		 * Trivial case: there is only the key attribute and we're in a nulls
		 * tree.  The hasNullsMask bit in the tuple header should not be set
		 * (and thus we can't use index_deform_tuple_internal), but
		 * nonetheless the result is NULL.
		 *
		 * Note: currently this is dead code, because noplace calls this when
		 * there is only the key attribute.  But we should cover the case.
		 */
		Assert(!hasNullsMask);

		datums[spgKeyColumn] = (Datum) 0;
		isnulls[spgKeyColumn] = true;
		return;
	}

	tp = (char *) tup + SGLTHDRSZ(hasNullsMask);
	bp = (bits8 *) ((char *) tup + sizeof(SpGistLeafTupleData));

	index_deform_tuple_internal(tupleDescriptor,
								datums, isnulls,
								tp, bp, hasNullsMask);

	/*
	 * Key column isnull value from the tuple should be consistent with
	 * keyColumnIsNull flag from the caller.
	 */
	Assert(keyColumnIsNull == isnulls[spgKeyColumn]);
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
				elog(PANIC, "failed to add item of size %zu to SPGiST index page",
					 size);

			return offnum;
		}
	}

	/* No luck in replacing a placeholder, so just add it to the page */
	offnum = PageAddItem(page, item, size,
						 InvalidOffsetNumber, false, false);

	if (offnum == InvalidOffsetNumber && !errorOK)
		elog(ERROR, "failed to add item of size %zu to SPGiST index page",
			 size);

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
