/*-------------------------------------------------------------------------
 *
 * blutils.c
 *		Bloom index utilities.
 *
 * Portions Copyright (c) 2016-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1990-1993, Regents of the University of California
 *
 * IDENTIFICATION
 *	  contrib/bloom/blutils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/generic_xlog.h"
#include "access/reloptions.h"
#include "bloom.h"
#include "catalog/index.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"

/* Signature dealing macros - note i is assumed to be of type int */
#define GETWORD(x,i) ( *( (BloomSignatureWord *)(x) + ( (i) / SIGNWORDBITS ) ) )
#define CLRBIT(x,i)   GETWORD(x,i) &= ~( 0x01 << ( (i) % SIGNWORDBITS ) )
#define SETBIT(x,i)   GETWORD(x,i) |=  ( 0x01 << ( (i) % SIGNWORDBITS ) )
#define GETBIT(x,i) ( (GETWORD(x,i) >> ( (i) % SIGNWORDBITS )) & 0x01 )

PG_FUNCTION_INFO_V1(blhandler);

/* Kind of relation options for bloom index */
static relopt_kind bl_relopt_kind;

/* parse table for fillRelOptions */
static relopt_parse_elt bl_relopt_tab[INDEX_MAX_KEYS + 1];

static int32 myRand(void);
static void mySrand(uint32 seed);

/*
 * Module initialize function: initialize info about Bloom relation options.
 *
 * Note: keep this in sync with makeDefaultBloomOptions().
 */
void
_PG_init(void)
{
	int			i;
	char		buf[16];

	bl_relopt_kind = add_reloption_kind();

	/* Option for length of signature */
	add_int_reloption(bl_relopt_kind, "length",
					  "Length of signature in bits",
					  DEFAULT_BLOOM_LENGTH, 1, MAX_BLOOM_LENGTH,
					  AccessExclusiveLock);
	bl_relopt_tab[0].optname = "length";
	bl_relopt_tab[0].opttype = RELOPT_TYPE_INT;
	bl_relopt_tab[0].offset = offsetof(BloomOptions, bloomLength);

	/* Number of bits for each possible index column: col1, col2, ... */
	for (i = 0; i < INDEX_MAX_KEYS; i++)
	{
		snprintf(buf, sizeof(buf), "col%d", i + 1);
		add_int_reloption(bl_relopt_kind, buf,
						  "Number of bits generated for each index column",
						  DEFAULT_BLOOM_BITS, 1, MAX_BLOOM_BITS,
						  AccessExclusiveLock);
		bl_relopt_tab[i + 1].optname = MemoryContextStrdup(TopMemoryContext,
														   buf);
		bl_relopt_tab[i + 1].opttype = RELOPT_TYPE_INT;
		bl_relopt_tab[i + 1].offset = offsetof(BloomOptions, bitSize[0]) + sizeof(int) * i;
	}
}

/*
 * Construct a default set of Bloom options.
 */
static BloomOptions *
makeDefaultBloomOptions(void)
{
	BloomOptions *opts;
	int			i;

	opts = (BloomOptions *) palloc0(sizeof(BloomOptions));
	/* Convert DEFAULT_BLOOM_LENGTH from # of bits to # of words */
	opts->bloomLength = (DEFAULT_BLOOM_LENGTH + SIGNWORDBITS - 1) / SIGNWORDBITS;
	for (i = 0; i < INDEX_MAX_KEYS; i++)
		opts->bitSize[i] = DEFAULT_BLOOM_BITS;
	SET_VARSIZE(opts, sizeof(BloomOptions));
	return opts;
}

/*
 * Bloom handler function: return IndexAmRoutine with access method parameters
 * and callbacks.
 */
Datum
blhandler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies = BLOOM_NSTRATEGIES;
	amroutine->amsupport = BLOOM_NPROC;
	amroutine->amoptsprocnum = BLOOM_OPTIONS_PROC;
	amroutine->amcanorder = false;
	amroutine->amcanorderbyop = false;
	amroutine->amcanbackward = false;
	amroutine->amcanunique = false;
	amroutine->amcanmulticol = true;
	amroutine->amoptionalkey = true;
	amroutine->amsearcharray = false;
	amroutine->amsearchnulls = false;
	amroutine->amstorage = false;
	amroutine->amclusterable = false;
	amroutine->ampredlocks = false;
	amroutine->amcanparallel = false;
	amroutine->amcaninclude = false;
	amroutine->amusemaintenanceworkmem = false;
	amroutine->amparallelvacuumoptions =
		VACUUM_OPTION_PARALLEL_BULKDEL | VACUUM_OPTION_PARALLEL_CLEANUP;
	amroutine->amkeytype = InvalidOid;

	amroutine->ambuild = blbuild;
	amroutine->ambuildempty = blbuildempty;
	amroutine->aminsert = blinsert;
	amroutine->ambulkdelete = blbulkdelete;
	amroutine->amvacuumcleanup = blvacuumcleanup;
	amroutine->amcanreturn = NULL;
	amroutine->amcostestimate = blcostestimate;
	amroutine->amoptions = bloptions;
	amroutine->amproperty = NULL;
	amroutine->ambuildphasename = NULL;
	amroutine->amvalidate = blvalidate;
	amroutine->amadjustmembers = NULL;
	amroutine->ambeginscan = blbeginscan;
	amroutine->amrescan = blrescan;
	amroutine->amgettuple = NULL;
	amroutine->amgetbitmap = blgetbitmap;
	amroutine->amendscan = blendscan;
	amroutine->ammarkpos = NULL;
	amroutine->amrestrpos = NULL;
	amroutine->amestimateparallelscan = NULL;
	amroutine->aminitparallelscan = NULL;
	amroutine->amparallelrescan = NULL;

	PG_RETURN_POINTER(amroutine);
}

/*
 * Fill BloomState structure for particular index.
 */
void
initBloomState(BloomState *state, Relation index)
{
	int			i;

	state->nColumns = index->rd_att->natts;

	/* Initialize hash function for each attribute */
	for (i = 0; i < index->rd_att->natts; i++)
	{
		fmgr_info_copy(&(state->hashFn[i]),
					   index_getprocinfo(index, i + 1, BLOOM_HASH_PROC),
					   CurrentMemoryContext);
		state->collations[i] = index->rd_indcollation[i];
	}

	/* Initialize amcache if needed with options from metapage */
	if (!index->rd_amcache)
	{
		Buffer		buffer;
		Page		page;
		BloomMetaPageData *meta;
		BloomOptions *opts;

		opts = MemoryContextAlloc(index->rd_indexcxt, sizeof(BloomOptions));

		buffer = ReadBuffer(index, BLOOM_METAPAGE_BLKNO);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);

		page = BufferGetPage(buffer);

		if (!BloomPageIsMeta(page))
			elog(ERROR, "Relation is not a bloom index");
		meta = BloomPageGetMeta(BufferGetPage(buffer));

		if (meta->magickNumber != BLOOM_MAGICK_NUMBER)
			elog(ERROR, "Relation is not a bloom index");

		*opts = meta->opts;

		UnlockReleaseBuffer(buffer);

		index->rd_amcache = (void *) opts;
	}

	memcpy(&state->opts, index->rd_amcache, sizeof(state->opts));
	state->sizeOfBloomTuple = BLOOMTUPLEHDRSZ +
		sizeof(BloomSignatureWord) * state->opts.bloomLength;
}

/*
 * Random generator copied from FreeBSD.  Using own random generator here for
 * two reasons:
 *
 * 1) In this case random numbers are used for on-disk storage.  Usage of
 *	  PostgreSQL number generator would obstruct it from all possible changes.
 * 2) Changing seed of PostgreSQL random generator would be undesirable side
 *	  effect.
 */
static int32 next;

static int32
myRand(void)
{
	/*----------
	 * Compute x = (7^5 * x) mod (2^31 - 1)
	 * without overflowing 31 bits:
	 *		(2^31 - 1) = 127773 * (7^5) + 2836
	 * From "Random number generators: good ones are hard to find",
	 * Park and Miller, Communications of the ACM, vol. 31, no. 10,
	 * October 1988, p. 1195.
	 *----------
	 */
	int32		hi,
				lo,
				x;

	/* Must be in [1, 0x7ffffffe] range at this point. */
	hi = next / 127773;
	lo = next % 127773;
	x = 16807 * lo - 2836 * hi;
	if (x < 0)
		x += 0x7fffffff;
	next = x;
	/* Transform to [0, 0x7ffffffd] range. */
	return (x - 1);
}

static void
mySrand(uint32 seed)
{
	next = seed;
	/* Transform to [1, 0x7ffffffe] range. */
	next = (next % 0x7ffffffe) + 1;
}

/*
 * Add bits of given value to the signature.
 */
void
signValue(BloomState *state, BloomSignatureWord *sign, Datum value, int attno)
{
	uint32		hashVal;
	int			nBit,
				j;

	/*
	 * init generator with "column's" number to get "hashed" seed for new
	 * value. We don't want to map the same numbers from different columns
	 * into the same bits!
	 */
	mySrand(attno);

	/*
	 * Init hash sequence to map our value into bits. the same values in
	 * different columns will be mapped into different bits because of step
	 * above
	 */
	hashVal = DatumGetInt32(FunctionCall1Coll(&state->hashFn[attno], state->collations[attno], value));
	mySrand(hashVal ^ myRand());

	for (j = 0; j < state->opts.bitSize[attno]; j++)
	{
		/* prevent multiple evaluation in SETBIT macro */
		nBit = myRand() % (state->opts.bloomLength * SIGNWORDBITS);
		SETBIT(sign, nBit);
	}
}

/*
 * Make bloom tuple from values.
 */
BloomTuple *
BloomFormTuple(BloomState *state, ItemPointer iptr, Datum *values, bool *isnull)
{
	int			i;
	BloomTuple *res = (BloomTuple *) palloc0(state->sizeOfBloomTuple);

	res->heapPtr = *iptr;

	/* Blooming each column */
	for (i = 0; i < state->nColumns; i++)
	{
		/* skip nulls */
		if (isnull[i])
			continue;

		signValue(state, res->sign, values[i], i);
	}

	return res;
}

/*
 * Add new bloom tuple to the page.  Returns true if new tuple was successfully
 * added to the page.  Returns false if it doesn't fit on the page.
 */
bool
BloomPageAddItem(BloomState *state, Page page, BloomTuple *tuple)
{
	BloomTuple *itup;
	BloomPageOpaque opaque;
	Pointer		ptr;

	/* We shouldn't be pointed to an invalid page */
	Assert(!PageIsNew(page) && !BloomPageIsDeleted(page));

	/* Does new tuple fit on the page? */
	if (BloomPageGetFreeSpace(state, page) < state->sizeOfBloomTuple)
		return false;

	/* Copy new tuple to the end of page */
	opaque = BloomPageGetOpaque(page);
	itup = BloomPageGetTuple(state, page, opaque->maxoff + 1);
	memcpy((Pointer) itup, (Pointer) tuple, state->sizeOfBloomTuple);

	/* Adjust maxoff and pd_lower */
	opaque->maxoff++;
	ptr = (Pointer) BloomPageGetTuple(state, page, opaque->maxoff + 1);
	((PageHeader) page)->pd_lower = ptr - page;

	/* Assert we didn't overrun available space */
	Assert(((PageHeader) page)->pd_lower <= ((PageHeader) page)->pd_upper);

	return true;
}

/*
 * Allocate a new page (either by recycling, or by extending the index file)
 * The returned buffer is already pinned and exclusive-locked
 * Caller is responsible for initializing the page by calling BloomInitPage
 */
Buffer
BloomNewBuffer(Relation index)
{
	Buffer		buffer;
	bool		needLock;

	/* First, try to get a page from FSM */
	for (;;)
	{
		BlockNumber blkno = GetFreeIndexPage(index);

		if (blkno == InvalidBlockNumber)
			break;

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

			if (BloomPageIsDeleted(page))
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
 * Initialize any page of a bloom index.
 */
void
BloomInitPage(Page page, uint16 flags)
{
	BloomPageOpaque opaque;

	PageInit(page, BLCKSZ, sizeof(BloomPageOpaqueData));

	opaque = BloomPageGetOpaque(page);
	opaque->flags = flags;
	opaque->bloom_page_id = BLOOM_PAGE_ID;
}

/*
 * Fill in metapage for bloom index.
 */
void
BloomFillMetapage(Relation index, Page metaPage)
{
	BloomOptions *opts;
	BloomMetaPageData *metadata;

	/*
	 * Choose the index's options.  If reloptions have been assigned, use
	 * those, otherwise create default options.
	 */
	opts = (BloomOptions *) index->rd_options;
	if (!opts)
		opts = makeDefaultBloomOptions();

	/*
	 * Initialize contents of meta page, including a copy of the options,
	 * which are now frozen for the life of the index.
	 */
	BloomInitPage(metaPage, BLOOM_META);
	metadata = BloomPageGetMeta(metaPage);
	memset(metadata, 0, sizeof(BloomMetaPageData));
	metadata->magickNumber = BLOOM_MAGICK_NUMBER;
	metadata->opts = *opts;
	((PageHeader) metaPage)->pd_lower += sizeof(BloomMetaPageData);

	/* If this fails, probably FreeBlockNumberArray size calc is wrong: */
	Assert(((PageHeader) metaPage)->pd_lower <= ((PageHeader) metaPage)->pd_upper);
}

/*
 * Initialize metapage for bloom index.
 */
void
BloomInitMetapage(Relation index)
{
	Buffer		metaBuffer;
	Page		metaPage;
	GenericXLogState *state;

	/*
	 * Make a new page; since it is first page it should be associated with
	 * block number 0 (BLOOM_METAPAGE_BLKNO).
	 */
	metaBuffer = BloomNewBuffer(index);
	Assert(BufferGetBlockNumber(metaBuffer) == BLOOM_METAPAGE_BLKNO);

	/* Initialize contents of meta page */
	state = GenericXLogStart(index);
	metaPage = GenericXLogRegisterBuffer(state, metaBuffer,
										 GENERIC_XLOG_FULL_IMAGE);
	BloomFillMetapage(index, metaPage);
	GenericXLogFinish(state);

	UnlockReleaseBuffer(metaBuffer);
}

/*
 * Parse reloptions for bloom index, producing a BloomOptions struct.
 */
bytea *
bloptions(Datum reloptions, bool validate)
{
	BloomOptions *rdopts;

	/* Parse the user-given reloptions */
	rdopts = (BloomOptions *) build_reloptions(reloptions, validate,
											   bl_relopt_kind,
											   sizeof(BloomOptions),
											   bl_relopt_tab,
											   lengthof(bl_relopt_tab));

	/* Convert signature length from # of bits to # to words, rounding up */
	if (rdopts)
		rdopts->bloomLength = (rdopts->bloomLength + SIGNWORDBITS - 1) / SIGNWORDBITS;

	return (bytea *) rdopts;
}
