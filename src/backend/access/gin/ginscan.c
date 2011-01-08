/*-------------------------------------------------------------------------
 *
 * ginscan.c
 *	  routines to manage scans of inverted index relations
 *
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/ginscan.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin_private.h"
#include "access/relscan.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"


Datum
ginbeginscan(PG_FUNCTION_ARGS)
{
	Relation	rel = (Relation) PG_GETARG_POINTER(0);
	int			nkeys = PG_GETARG_INT32(1);
	int			norderbys = PG_GETARG_INT32(2);
	IndexScanDesc scan;
	GinScanOpaque so;

	/* no order by operators allowed */
	Assert(norderbys == 0);

	scan = RelationGetIndexScan(rel, nkeys, norderbys);

	/* allocate private workspace */
	so = (GinScanOpaque) palloc(sizeof(GinScanOpaqueData));
	so->keys = NULL;
	so->nkeys = 0;
	so->tempCtx = AllocSetContextCreate(CurrentMemoryContext,
										"Gin scan temporary context",
										ALLOCSET_DEFAULT_MINSIZE,
										ALLOCSET_DEFAULT_INITSIZE,
										ALLOCSET_DEFAULT_MAXSIZE);
	initGinState(&so->ginstate, scan->indexRelation);

	scan->opaque = so;

	PG_RETURN_POINTER(scan);
}

/*
 * Initialize a GinScanKey using the output from the extractQueryFn
 */
static void
ginFillScanKey(GinState *ginstate, GinScanKey key,
			   OffsetNumber attnum, Datum query,
			   Datum *queryValues, GinNullCategory *queryCategories,
			   bool *partial_matches, uint32 nQueryValues,
			   StrategyNumber strategy, Pointer *extra_data,
			   int32 searchMode)
{
	uint32		nUserQueryValues = nQueryValues;
	uint32		i,
				j;

	/* Non-default search modes add one "hidden" entry to each key */
	if (searchMode != GIN_SEARCH_MODE_DEFAULT)
		nQueryValues++;
	key->nentries = nQueryValues;
	key->nuserentries = nUserQueryValues;

	key->scanEntry = (GinScanEntry) palloc(sizeof(GinScanEntryData) * nQueryValues);
	key->entryRes = (bool *) palloc0(sizeof(bool) * nQueryValues);
	key->query = query;
	key->queryValues = queryValues;
	key->queryCategories = queryCategories;
	key->extra_data = extra_data;
	key->strategy = strategy;
	key->searchMode = searchMode;
	key->attnum = attnum;

	key->firstCall = TRUE;
	ItemPointerSetMin(&key->curItem);

	for (i = 0; i < nQueryValues; i++)
	{
		GinScanEntry scanEntry = key->scanEntry + i;

		scanEntry->pval = key->entryRes + i;
		if (i < nUserQueryValues)
		{
			scanEntry->queryKey = queryValues[i];
			scanEntry->queryCategory = queryCategories[i];
			scanEntry->isPartialMatch =
				(ginstate->canPartialMatch[attnum - 1] && partial_matches)
				? partial_matches[i] : false;
			scanEntry->extra_data = (extra_data) ? extra_data[i] : NULL;
		}
		else
		{
			/* set up hidden entry */
			scanEntry->queryKey = (Datum) 0;
			switch (searchMode)
			{
				case GIN_SEARCH_MODE_INCLUDE_EMPTY:
					scanEntry->queryCategory = GIN_CAT_EMPTY_ITEM;
					break;
				case GIN_SEARCH_MODE_ALL:
					scanEntry->queryCategory = GIN_CAT_EMPTY_QUERY;
					break;
				case GIN_SEARCH_MODE_EVERYTHING:
					scanEntry->queryCategory = GIN_CAT_EMPTY_QUERY;
					break;
				default:
					elog(ERROR, "unexpected searchMode: %d", searchMode);
					break;
			}
			scanEntry->isPartialMatch = false;
			scanEntry->extra_data = NULL;
		}
		scanEntry->strategy = strategy;
		scanEntry->searchMode = searchMode;
		scanEntry->attnum = attnum;

		ItemPointerSetMin(&scanEntry->curItem);
		scanEntry->isFinished = FALSE;
		scanEntry->offset = InvalidOffsetNumber;
		scanEntry->buffer = InvalidBuffer;
		scanEntry->list = NULL;
		scanEntry->nlist = 0;
		scanEntry->matchBitmap = NULL;
		scanEntry->matchIterator = NULL;
		scanEntry->matchResult = NULL;

		/*
		 * Link to any preceding identical entry in current scan key.
		 *
		 * Entries with non-null extra_data are never considered identical,
		 * since we can't know exactly what the opclass might be doing with
		 * that.
		 */
		scanEntry->master = NULL;
		if (scanEntry->extra_data == NULL)
		{
			for (j = 0; j < i; j++)
			{
				GinScanEntry prevEntry = key->scanEntry + j;

				if (prevEntry->extra_data == NULL &&
					scanEntry->isPartialMatch == prevEntry->isPartialMatch &&
					ginCompareEntries(ginstate, attnum,
									  scanEntry->queryKey,
									  scanEntry->queryCategory,
									  prevEntry->queryKey,
									  prevEntry->queryCategory) == 0)
				{
					scanEntry->master = prevEntry;
					break;
				}
			}
		}
	}
}

#ifdef NOT_USED

static void
resetScanKeys(GinScanKey keys, uint32 nkeys)
{
	uint32		i,
				j;

	if (keys == NULL)
		return;

	for (i = 0; i < nkeys; i++)
	{
		GinScanKey	key = keys + i;

		key->firstCall = TRUE;
		ItemPointerSetMin(&key->curItem);

		for (j = 0; j < key->nentries; j++)
		{
			if (key->scanEntry[j].buffer != InvalidBuffer)
				ReleaseBuffer(key->scanEntry[i].buffer);

			ItemPointerSetMin(&key->scanEntry[j].curItem);
			key->scanEntry[j].isFinished = FALSE;
			key->scanEntry[j].offset = InvalidOffsetNumber;
			key->scanEntry[j].buffer = InvalidBuffer;
			key->scanEntry[j].list = NULL;
			key->scanEntry[j].nlist = 0;
			key->scanEntry[j].matchBitmap = NULL;
			key->scanEntry[j].matchIterator = NULL;
			key->scanEntry[j].matchResult = NULL;
		}
	}
}
#endif

static void
freeScanKeys(GinScanKey keys, uint32 nkeys)
{
	uint32		i,
				j;

	if (keys == NULL)
		return;

	for (i = 0; i < nkeys; i++)
	{
		GinScanKey	key = keys + i;

		for (j = 0; j < key->nentries; j++)
		{
			if (key->scanEntry[j].buffer != InvalidBuffer)
				ReleaseBuffer(key->scanEntry[j].buffer);
			if (key->scanEntry[j].list)
				pfree(key->scanEntry[j].list);
			if (key->scanEntry[j].matchIterator)
				tbm_end_iterate(key->scanEntry[j].matchIterator);
			if (key->scanEntry[j].matchBitmap)
				tbm_free(key->scanEntry[j].matchBitmap);
		}

		pfree(key->entryRes);
		pfree(key->scanEntry);
	}

	pfree(keys);
}

void
ginNewScanKey(IndexScanDesc scan)
{
	ScanKey		scankey = scan->keyData;
	GinScanOpaque so = (GinScanOpaque) scan->opaque;
	int			i;
	uint32		nkeys = 0;
	bool		hasNullQuery = false;

	/* if no scan keys provided, allocate extra EVERYTHING GinScanKey */
	so->keys = (GinScanKey)
		palloc(Max(scan->numberOfKeys, 1) * sizeof(GinScanKeyData));

	so->isVoidRes = false;

	for (i = 0; i < scan->numberOfKeys; i++)
	{
		ScanKey		skey = &scankey[i];
		Datum	   *queryValues;
		int32		nQueryValues = 0;
		bool	   *partial_matches = NULL;
		Pointer    *extra_data = NULL;
		bool	   *nullFlags = NULL;
		int32		searchMode = GIN_SEARCH_MODE_DEFAULT;

		/*
		 * We assume that GIN-indexable operators are strict, so a null
		 * query argument means an unsatisfiable query.
		 */
		if (skey->sk_flags & SK_ISNULL)
		{
			so->isVoidRes = true;
			break;
		}

		/* OK to call the extractQueryFn */
		queryValues = (Datum *)
			DatumGetPointer(FunctionCall7(&so->ginstate.extractQueryFn[skey->sk_attno - 1],
										  skey->sk_argument,
										  PointerGetDatum(&nQueryValues),
										  UInt16GetDatum(skey->sk_strategy),
										  PointerGetDatum(&partial_matches),
										  PointerGetDatum(&extra_data),
										  PointerGetDatum(&nullFlags),
										  PointerGetDatum(&searchMode)));

		/*
		 * If bogus searchMode is returned, treat as GIN_SEARCH_MODE_ALL;
		 * note in particular we don't allow extractQueryFn to select
		 * GIN_SEARCH_MODE_EVERYTHING.
		 */
		if (searchMode < GIN_SEARCH_MODE_DEFAULT ||
			searchMode > GIN_SEARCH_MODE_ALL)
			searchMode = GIN_SEARCH_MODE_ALL;

		/* Non-default modes require the index to have placeholders */
		if (searchMode != GIN_SEARCH_MODE_DEFAULT)
			hasNullQuery = true;

		/*
		 * In default mode, no keys means an unsatisfiable query.
		 */
		if (queryValues == NULL || nQueryValues <= 0)
		{
			if (searchMode == GIN_SEARCH_MODE_DEFAULT)
			{
				so->isVoidRes = true;
				break;
			}
			nQueryValues = 0;	/* ensure sane value */
		}

		/*
		 * If the extractQueryFn didn't create a nullFlags array, create one,
		 * assuming that everything's non-null.  Otherwise, run through the
		 * array and make sure each value is exactly 0 or 1; this ensures
		 * binary compatibility with the GinNullCategory representation.
		 * While at it, detect whether any null keys are present.
		 */
		if (nullFlags == NULL)
			nullFlags = (bool *) palloc0(nQueryValues * sizeof(bool));
		else
		{
			int32 j;

			for (j = 0; j < nQueryValues; j++)
			{
				if (nullFlags[j])
				{
					nullFlags[j] = true;	/* not any other nonzero value */
					hasNullQuery = true;
				}
			}
		}
		/* now we can use the nullFlags as category codes */

		ginFillScanKey(&so->ginstate, &(so->keys[nkeys]),
					   skey->sk_attno, skey->sk_argument,
					   queryValues, (GinNullCategory *) nullFlags,
					   partial_matches, nQueryValues,
					   skey->sk_strategy, extra_data, searchMode);
		nkeys++;
	}

	/*
	 * If there are no regular scan keys, generate an EVERYTHING scankey to
	 * drive a full-index scan.
	 */
	if (nkeys == 0 && !so->isVoidRes)
	{
		hasNullQuery = true;
		ginFillScanKey(&so->ginstate, &(so->keys[nkeys]),
					   FirstOffsetNumber, (Datum) 0,
					   NULL, NULL, NULL, 0,
					   InvalidStrategy, NULL, GIN_SEARCH_MODE_EVERYTHING);
		nkeys++;
	}

	/*
	 * If the index is version 0, it may be missing null and placeholder
	 * entries, which would render searches for nulls and full-index scans
	 * unreliable.  Throw an error if so.
	 */
	if (hasNullQuery && !so->isVoidRes)
	{
		GinStatsData   ginStats;

		ginGetStats(scan->indexRelation, &ginStats);
		if (ginStats.ginVersion < 1)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("old GIN indexes do not support whole-index scans nor searches for nulls"),
					 errhint("To fix this, do REINDEX INDEX \"%s\".",
							 RelationGetRelationName(scan->indexRelation))));
	}

	so->nkeys = nkeys;

	pgstat_count_index_scan(scan->indexRelation);
}

Datum
ginrescan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ScanKey		scankey = (ScanKey) PG_GETARG_POINTER(1);
	/* remaining arguments are ignored */
	GinScanOpaque so = (GinScanOpaque) scan->opaque;

	freeScanKeys(so->keys, so->nkeys);
	so->keys = NULL;

	if (scankey && scan->numberOfKeys > 0)
	{
		memmove(scan->keyData, scankey,
				scan->numberOfKeys * sizeof(ScanKeyData));
	}

	PG_RETURN_VOID();
}


Datum
ginendscan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	GinScanOpaque so = (GinScanOpaque) scan->opaque;

	freeScanKeys(so->keys, so->nkeys);

	MemoryContextDelete(so->tempCtx);

	pfree(so);

	PG_RETURN_VOID();
}

Datum
ginmarkpos(PG_FUNCTION_ARGS)
{
	elog(ERROR, "GIN does not support mark/restore");
	PG_RETURN_VOID();
}

Datum
ginrestrpos(PG_FUNCTION_ARGS)
{
	elog(ERROR, "GIN does not support mark/restore");
	PG_RETURN_VOID();
}
