/*-------------------------------------------------------------------------
 *
 * blutils.c
 *		Bloom index utilities.
 *
 * Portions Copyright (c) 2016, PostgreSQL Global Development Group
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
#include "catalog/index.h"
#include "storage/lmgr.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "utils/memutils.h"
#include "access/reloptions.h"
#include "storage/freespace.h"
#include "storage/indexfsm.h"

#include "bloom.h"

/* Signature dealing macros */
#define BITSIGNTYPE (BITS_PER_BYTE * sizeof(SignType))
#define GETWORD(x,i) ( *( (SignType*)(x) + (int)( (i) / BITSIGNTYPE ) ) )
#define CLRBIT(x,i)   GETWORD(x,i) &= ~( 0x01 << ( (i) % BITSIGNTYPE ) )
#define SETBIT(x,i)   GETWORD(x,i) |=  ( 0x01 << ( (i) % BITSIGNTYPE ) )
#define GETBIT(x,i) ( (GETWORD(x,i) >> ( (i) % BITSIGNTYPE )) & 0x01 )

PG_FUNCTION_INFO_V1(blhandler);

/* Kind of relation optioms for bloom index */
static relopt_kind bl_relopt_kind;

static int32 myRand();
static void mySrand(uint32 seed);

/*
 * Module initialize function: initilized relation options.
 */
void
_PG_init(void)
{
	int			i;
	char		buf[16];

	bl_relopt_kind = add_reloption_kind();

	add_int_reloption(bl_relopt_kind, "length",
					  "Length of signature in uint16 type", 5, 1, 256);

	for (i = 0; i < INDEX_MAX_KEYS; i++)
	{
		snprintf(buf, 16, "col%d", i + 1);
		add_int_reloption(bl_relopt_kind, buf,
					  "Number of bits for corresponding column", 2, 1, 2048);
	}
}

/*
 * Bloom handler function: return IndexAmRoutine with access method parameters
 * and callbacks.
 */
Datum
blhandler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies = 1;
	amroutine->amsupport = 1;
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
	amroutine->amkeytype = 0;

	amroutine->aminsert = blinsert;
	amroutine->ambeginscan = blbeginscan;
	amroutine->amgettuple = NULL;
	amroutine->amgetbitmap = blgetbitmap;
	amroutine->amrescan = blrescan;
	amroutine->amendscan = blendscan;
	amroutine->ammarkpos = NULL;
	amroutine->amrestrpos = NULL;
	amroutine->ambuild = blbuild;
	amroutine->ambuildempty = blbuildempty;
	amroutine->ambulkdelete = blbulkdelete;
	amroutine->amvacuumcleanup = blvacuumcleanup;
	amroutine->amcanreturn = NULL;
	amroutine->amcostestimate = blcostestimate;
	amroutine->amoptions = bloptions;
	amroutine->amvalidate = blvalidate;

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

		page = BufferGetPage(buffer, NULL, NULL, BGP_NO_SNAPSHOT_TEST);

		if (!BloomPageIsMeta(page))
			elog(ERROR, "Relation is not a bloom index");
		meta = BloomPageGetMeta(BufferGetPage(buffer, NULL, NULL,
											  BGP_NO_SNAPSHOT_TEST));

		if (meta->magickNumber != BLOOM_MAGICK_NUMBER)
			elog(ERROR, "Relation is not a bloom index");

		*opts = meta->opts;

		UnlockReleaseBuffer(buffer);

		index->rd_amcache = (void *) opts;
	}

	memcpy(&state->opts, index->rd_amcache, sizeof(state->opts));
	state->sizeOfBloomTuple = BLOOMTUPLEHDRSZ +
		sizeof(SignType) * state->opts.bloomLength;
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
myRand()
{
	/*
	 * Compute x = (7^5 * x) mod (2^31 - 1)
	 * without overflowing 31 bits:
	 *		(2^31 - 1) = 127773 * (7^5) + 2836
	 * From "Random number generators: good ones are hard to find",
	 * Park and Miller, Communications of the ACM, vol. 31, no. 10,
	 * October 1988, p. 1195.
	 */
	int32 hi, lo, x;

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
signValue(BloomState *state, SignType *sign, Datum value, int attno)
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
	hashVal = DatumGetInt32(FunctionCall1(&state->hashFn[attno], value));
	mySrand(hashVal ^ myRand());

	for (j = 0; j < state->opts.bitSize[attno]; j++)
	{
		/* prevent mutiple evaluation */
		nBit = myRand() % (state->opts.bloomLength * BITSIGNTYPE);
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
 * added to the page.  Returns false if it doesn't fit the page.
 */
bool
BloomPageAddItem(BloomState *state, Page page, BloomTuple *tuple)
{
	BloomTuple *itup;
	BloomPageOpaque opaque;
	Pointer		ptr;

	/* Does new tuple fit the page */
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

	return true;
}

/*
 * Allocate a new page (either by recycling, or by extending the index file)
 * The returned buffer is already pinned and exclusive-locked
 * Caller is responsible for initializing the page by calling BloomInitBuffer
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
			Page		page = BufferGetPage(buffer, NULL, NULL,
											 BGP_NO_SNAPSHOT_TEST);

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
 * Initialize bloom page.
 */
void
BloomInitPage(Page page, uint16 flags)
{
	BloomPageOpaque opaque;

	PageInit(page, BLCKSZ, sizeof(BloomPageOpaqueData));

	opaque = BloomPageGetOpaque(page);
	memset(opaque, 0, sizeof(BloomPageOpaqueData));
	opaque->flags = flags;
}

/*
 * Adjust options of bloom index.
 */
static void
adjustBloomOptions(BloomOptions *opts)
{
	int				i;

	/* Default length of bloom filter is 5 of 16-bit integers */
	if (opts->bloomLength <= 0)
		opts->bloomLength = 5;
	else if (opts->bloomLength > MAX_BLOOM_LENGTH)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("length of bloom signature (%d) is greater than maximum %d",
						opts->bloomLength, MAX_BLOOM_LENGTH)));

	/* Check singnature length */
	for (i = 0; i < INDEX_MAX_KEYS; i++)
	{
		/*
		 * Zero and negative number of bits is meaningless.  Also setting
		 * more bits than signature have seems useless.  Replace both cases
		 * with 2 bits default.
		 */
		if (opts->bitSize[i] <= 0
			|| opts->bitSize[i] >= opts->bloomLength * sizeof(SignType) * BITS_PER_BYTE)
			opts->bitSize[i] = 2;
	}
}

/*
 * Initialize metapage for bloom index.
 */
void
BloomInitMetapage(Relation index)
{
	Page		metaPage;
	Buffer		metaBuffer;
	BloomMetaPageData *metadata;
	GenericXLogState *state;

	/*
	 * Make a new buffer, since it first buffer it should be associated with
	 * block number 0 (BLOOM_METAPAGE_BLKNO).
	 */
	metaBuffer = BloomNewBuffer(index);
	Assert(BufferGetBlockNumber(metaBuffer) == BLOOM_METAPAGE_BLKNO);

	/* Initialize bloom index options */
	if (!index->rd_options)
		index->rd_options = palloc0(sizeof(BloomOptions));
	adjustBloomOptions((BloomOptions *) index->rd_options);

	/* Initialize contents of meta page */
	state = GenericXLogStart(index);
	metaPage = GenericXLogRegister(state, metaBuffer, true);

	BloomInitPage(metaPage, BLOOM_META);
	metadata = BloomPageGetMeta(metaPage);
	memset(metadata, 0, sizeof(BloomMetaPageData));
	metadata->magickNumber = BLOOM_MAGICK_NUMBER;
	metadata->opts = *((BloomOptions *) index->rd_options);
	((PageHeader) metaPage)->pd_lower += sizeof(BloomMetaPageData);

	GenericXLogFinish(state);
	UnlockReleaseBuffer(metaBuffer);
}

/*
 * Initialize options for bloom index.
 */
bytea *
bloptions(Datum reloptions, bool validate)
{
	relopt_value *options;
	int			numoptions;
	BloomOptions *rdopts;
	relopt_parse_elt tab[INDEX_MAX_KEYS + 1];
	int			i;
	char		buf[16];

	/* Option for length of signature */
	tab[0].optname = "length";
	tab[0].opttype = RELOPT_TYPE_INT;
	tab[0].offset = offsetof(BloomOptions, bloomLength);

	/* Number of bits for each of possible columns: col1, col2, ... */
	for (i = 0; i < INDEX_MAX_KEYS; i++)
	{
		snprintf(buf, sizeof(buf), "col%d", i + 1);
		tab[i + 1].optname = pstrdup(buf);
		tab[i + 1].opttype = RELOPT_TYPE_INT;
		tab[i + 1].offset = offsetof(BloomOptions, bitSize[i]);
	}

	options = parseRelOptions(reloptions, validate, bl_relopt_kind, &numoptions);
	rdopts = allocateReloptStruct(sizeof(BloomOptions), options, numoptions);
	fillRelOptions((void *) rdopts, sizeof(BloomOptions), options, numoptions,
				   validate, tab, INDEX_MAX_KEYS + 1);

	adjustBloomOptions(rdopts);

	return (bytea *) rdopts;
}
