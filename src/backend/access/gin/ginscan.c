/*-------------------------------------------------------------------------
 *
 * ginscan.c
 *	  routines to manage scans of inverted index relations
 *
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
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
	so->keyCtx = AllocSetContextCreate(CurrentMemoryContext,
									   "Gin scan key context",
									   ALLOCSET_DEFAULT_MINSIZE,
									   ALLOCSET_DEFAULT_INITSIZE,
									   ALLOCSET_DEFAULT_MAXSIZE);
	initGinState(&so->ginstate, scan->indexRelation);

	scan->opaque = so;

	PG_RETURN_POINTER(scan);
}

/*
 * Create a new GinScanEntry, unless an equivalent one already exists,
 * in which case just return it
 */
static GinScanEntry
ginFillScanEntry(GinScanOpaque so, OffsetNumber attnum,
				 StrategyNumber strategy, int32 searchMode,
				 Datum queryKey, GinNullCategory queryCategory,
				 bool isPartialMatch, Pointer extra_data)
{
	GinState   *ginstate = &so->ginstate;
	GinScanEntry scanEntry;
	uint32		i;

	/*
	 * Look for an existing equivalent entry.
	 *
	 * Entries with non-null extra_data are never considered identical, since
	 * we can't know exactly what the opclass might be doing with that.
	 */
	if (extra_data == NULL)
	{
		for (i = 0; i < so->totalentries; i++)
		{
			GinScanEntry prevEntry = so->entries[i];

			if (prevEntry->extra_data == NULL &&
				prevEntry->isPartialMatch == isPartialMatch &&
				prevEntry->strategy == strategy &&
				prevEntry->searchMode == searchMode &&
				prevEntry->attnum == attnum &&
				ginCompareEntries(ginstate, attnum,
								  prevEntry->queryKey,
								  prevEntry->queryCategory,
								  queryKey,
								  queryCategory) == 0)
			{
				/* Successful match */
				return prevEntry;
			}
		}
	}

	/* Nope, create a new entry */
	scanEntry = (GinScanEntry) palloc(sizeof(GinScanEntryData));
	scanEntry->queryKey = queryKey;
	scanEntry->queryCategory = queryCategory;
	scanEntry->isPartialMatch = isPartialMatch;
	scanEntry->extra_data = extra_data;
	scanEntry->strategy = strategy;
	scanEntry->searchMode = searchMode;
	scanEntry->attnum = attnum;

	scanEntry->buffer = InvalidBuffer;
	ItemPointerSetMin(&scanEntry->curItem);
	scanEntry->matchBitmap = NULL;
	scanEntry->matchIterator = NULL;
	scanEntry->matchResult = NULL;
	scanEntry->list = NULL;
	scanEntry->nlist = 0;
	scanEntry->offset = InvalidOffsetNumber;
	scanEntry->isFinished = false;
	scanEntry->reduceResult = false;

	/* Add it to so's array */
	if (so->totalentries >= so->allocentries)
	{
		so->allocentries *= 2;
		so->entries = (GinScanEntry *)
			repalloc(so->entries, so->allocentries * sizeof(GinScanEntry));
	}
	so->entries[so->totalentries++] = scanEntry;

	return scanEntry;
}

/*
 * Initialize the next GinScanKey using the output from the extractQueryFn
 */
static void
ginFillScanKey(GinScanOpaque so, OffsetNumber attnum,
			   StrategyNumber strategy, int32 searchMode,
			   Datum query, uint32 nQueryValues,
			   Datum *queryValues, GinNullCategory *queryCategories,
			   bool *partial_matches, Pointer *extra_data)
{
	GinScanKey	key = &(so->keys[so->nkeys++]);
	GinState   *ginstate = &so->ginstate;
	uint32		nUserQueryValues = nQueryValues;
	uint32		i;

	/* Non-default search modes add one "hidden" entry to each key */
	if (searchMode != GIN_SEARCH_MODE_DEFAULT)
		nQueryValues++;
	key->nentries = nQueryValues;
	key->nuserentries = nUserQueryValues;

	key->scanEntry = (GinScanEntry *) palloc(sizeof(GinScanEntry) * nQueryValues);
	key->entryRes = (bool *) palloc0(sizeof(bool) * nQueryValues);

	key->query = query;
	key->queryValues = queryValues;
	key->queryCategories = queryCategories;
	key->extra_data = extra_data;
	key->strategy = strategy;
	key->searchMode = searchMode;
	key->attnum = attnum;

	ItemPointerSetMin(&key->curItem);
	key->curItemMatches = false;
	key->recheckCurItem = false;
	key->isFinished = false;
	key->nrequired = 0;
	key->nadditional = 0;
	key->requiredEntries = NULL;
	key->additionalEntries = NULL;

	ginInitConsistentFunction(ginstate, key);

	for (i = 0; i < nQueryValues; i++)
	{
		Datum		queryKey;
		GinNullCategory queryCategory;
		bool		isPartialMatch;
		Pointer		this_extra;

		if (i < nUserQueryValues)
		{
			/* set up normal entry using extractQueryFn's outputs */
			queryKey = queryValues[i];
			queryCategory = queryCategories[i];
			isPartialMatch =
				(ginstate->canPartialMatch[attnum - 1] && partial_matches)
				? partial_matches[i] : false;
			this_extra = (extra_data) ? extra_data[i] : NULL;
		}
		else
		{
			/* set up hidden entry */
			queryKey = (Datum) 0;
			switch (searchMode)
			{
				case GIN_SEARCH_MODE_INCLUDE_EMPTY:
					queryCategory = GIN_CAT_EMPTY_ITEM;
					break;
				case GIN_SEARCH_MODE_ALL:
					queryCategory = GIN_CAT_EMPTY_QUERY;
					break;
				case GIN_SEARCH_MODE_EVERYTHING:
					queryCategory = GIN_CAT_EMPTY_QUERY;
					break;
				default:
					elog(ERROR, "unexpected searchMode: %d", searchMode);
					queryCategory = 0;	/* keep compiler quiet */
					break;
			}
			isPartialMatch = false;
			this_extra = NULL;

			/*
			 * We set the strategy to a fixed value so that ginFillScanEntry
			 * can combine these entries for different scan keys.  This is
			 * safe because the strategy value in the entry struct is only
			 * used for partial-match cases.  It's OK to overwrite our local
			 * variable here because this is the last loop iteration.
			 */
			strategy = InvalidStrategy;
		}

		key->scanEntry[i] = ginFillScanEntry(so, attnum,
											 strategy, searchMode,
											 queryKey, queryCategory,
											 isPartialMatch, this_extra);
	}
}

/*
 * Release current scan keys, if any.
 */
void
ginFreeScanKeys(GinScanOpaque so)
{
	uint32		i;

	if (so->keys == NULL)
		return;

	for (i = 0; i < so->totalentries; i++)
	{
		GinScanEntry entry = so->entries[i];

		if (entry->buffer != InvalidBuffer)
			ReleaseBuffer(entry->buffer);
		if (entry->list)
			pfree(entry->list);
		if (entry->matchIterator)
			tbm_end_iterate(entry->matchIterator);
		if (entry->matchBitmap)
			tbm_free(entry->matchBitmap);
	}

	MemoryContextResetAndDeleteChildren(so->keyCtx);

	so->keys = NULL;
	so->nkeys = 0;
	so->entries = NULL;
	so->totalentries = 0;
}

void
ginNewScanKey(IndexScanDesc scan)
{
	ScanKey		scankey = scan->keyData;
	GinScanOpaque so = (GinScanOpaque) scan->opaque;
	int			i;
	bool		hasNullQuery = false;
	MemoryContext oldCtx;

	/*
	 * Allocate all the scan key information in the key context. (If
	 * extractQuery leaks anything there, it won't be reset until the end of
	 * scan or rescan, but that's OK.)
	 */
	oldCtx = MemoryContextSwitchTo(so->keyCtx);

	/* if no scan keys provided, allocate extra EVERYTHING GinScanKey */
	so->keys = (GinScanKey)
		palloc(Max(scan->numberOfKeys, 1) * sizeof(GinScanKeyData));
	so->nkeys = 0;

	/* initialize expansible array of GinScanEntry pointers */
	so->totalentries = 0;
	so->allocentries = 32;
	so->entries = (GinScanEntry *)
		palloc(so->allocentries * sizeof(GinScanEntry));

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
		 * We assume that GIN-indexable operators are strict, so a null query
		 * argument means an unsatisfiable query.
		 */
		if (skey->sk_flags & SK_ISNULL)
		{
			so->isVoidRes = true;
			break;
		}

		/* OK to call the extractQueryFn */
		queryValues = (Datum *)
			DatumGetPointer(FunctionCall7Coll(&so->ginstate.extractQueryFn[skey->sk_attno - 1],
						   so->ginstate.supportCollation[skey->sk_attno - 1],
											  skey->sk_argument,
											  PointerGetDatum(&nQueryValues),
										   UInt16GetDatum(skey->sk_strategy),
										   PointerGetDatum(&partial_matches),
											  PointerGetDatum(&extra_data),
											  PointerGetDatum(&nullFlags),
											  PointerGetDatum(&searchMode)));

		/*
		 * If bogus searchMode is returned, treat as GIN_SEARCH_MODE_ALL; note
		 * in particular we don't allow extractQueryFn to select
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
		 * binary compatibility with the GinNullCategory representation. While
		 * at it, detect whether any null keys are present.
		 */
		if (nullFlags == NULL)
			nullFlags = (bool *) palloc0(nQueryValues * sizeof(bool));
		else
		{
			int32		j;

			for (j = 0; j < nQueryValues; j++)
			{
				if (nullFlags[j])
				{
					nullFlags[j] = true;		/* not any other nonzero value */
					hasNullQuery = true;
				}
			}
		}
		/* now we can use the nullFlags as category codes */

		ginFillScanKey(so, skey->sk_attno,
					   skey->sk_strategy, searchMode,
					   skey->sk_argument, nQueryValues,
					   queryValues, (GinNullCategory *) nullFlags,
					   partial_matches, extra_data);
	}

	/*
	 * If there are no regular scan keys, generate an EVERYTHING scankey to
	 * drive a full-index scan.
	 */
	if (so->nkeys == 0 && !so->isVoidRes)
	{
		hasNullQuery = true;
		ginFillScanKey(so, FirstOffsetNumber,
					   InvalidStrategy, GIN_SEARCH_MODE_EVERYTHING,
					   (Datum) 0, 0,
					   NULL, NULL, NULL, NULL);
	}

	/*
	 * If the index is version 0, it may be missing null and placeholder
	 * entries, which would render searches for nulls and full-index scans
	 * unreliable.  Throw an error if so.
	 */
	if (hasNullQuery && !so->isVoidRes)
	{
		GinStatsData ginStats;

		ginGetStats(scan->indexRelation, &ginStats);
		if (ginStats.ginVersion < 1)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("old GIN indexes do not support whole-index scans nor searches for nulls"),
					 errhint("To fix this, do REINDEX INDEX \"%s\".",
							 RelationGetRelationName(scan->indexRelation))));
	}

	MemoryContextSwitchTo(oldCtx);

	pgstat_count_index_scan(scan->indexRelation);
}

Datum
ginrescan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ScanKey		scankey = (ScanKey) PG_GETARG_POINTER(1);

	/* remaining arguments are ignored */
	GinScanOpaque so = (GinScanOpaque) scan->opaque;

	ginFreeScanKeys(so);

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

	ginFreeScanKeys(so);

	MemoryContextDelete(so->tempCtx);
	MemoryContextDelete(so->keyCtx);

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
