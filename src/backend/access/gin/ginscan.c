/*-------------------------------------------------------------------------
 *
 * ginscan.c
 *	  routines to manage scans of inverted index relations
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
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


IndexScanDesc
ginbeginscan(Relation rel, int nkeys, int norderbys)
{
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
										ALLOCSET_DEFAULT_SIZES);
	so->keyCtx = AllocSetContextCreate(CurrentMemoryContext,
									   "Gin scan key context",
									   ALLOCSET_DEFAULT_SIZES);
	initGinState(&so->ginstate, scan->indexRelation);

	scan->opaque = so;

	return scan;
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
 * Append hidden scan entry of given category to the scan key.
 *
 * NB: this had better be called at most once per scan key, since
 * ginFillScanKey leaves room for only one hidden entry.  Currently,
 * it seems sufficiently clear that this is true that we don't bother
 * with any cross-check logic.
 */
static void
ginScanKeyAddHiddenEntry(GinScanOpaque so, GinScanKey key,
						 GinNullCategory queryCategory)
{
	int			i = key->nentries++;

	/* strategy is of no interest because this is not a partial-match item */
	key->scanEntry[i] = ginFillScanEntry(so, key->attnum,
										 InvalidStrategy, key->searchMode,
										 (Datum) 0, queryCategory,
										 false, NULL);
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
	uint32		i;

	key->nentries = nQueryValues;
	key->nuserentries = nQueryValues;

	/* Allocate one extra array slot for possible "hidden" entry */
	key->scanEntry = (GinScanEntry *) palloc(sizeof(GinScanEntry) *
											 (nQueryValues + 1));
	key->entryRes = (GinTernaryValue *) palloc0(sizeof(GinTernaryValue) *
												(nQueryValues + 1));

	key->query = query;
	key->queryValues = queryValues;
	key->queryCategories = queryCategories;
	key->extra_data = extra_data;
	key->strategy = strategy;
	key->searchMode = searchMode;
	key->attnum = attnum;

	/*
	 * Initially, scan keys of GIN_SEARCH_MODE_ALL mode are marked
	 * excludeOnly.  This might get changed later.
	 */
	key->excludeOnly = (searchMode == GIN_SEARCH_MODE_ALL);

	ItemPointerSetMin(&key->curItem);
	key->curItemMatches = false;
	key->recheckCurItem = false;
	key->isFinished = false;
	key->nrequired = 0;
	key->nadditional = 0;
	key->requiredEntries = NULL;
	key->additionalEntries = NULL;

	ginInitConsistentFunction(ginstate, key);

	/* Set up normal scan entries using extractQueryFn's outputs */
	for (i = 0; i < nQueryValues; i++)
	{
		Datum		queryKey;
		GinNullCategory queryCategory;
		bool		isPartialMatch;
		Pointer		this_extra;

		queryKey = queryValues[i];
		queryCategory = queryCategories[i];
		isPartialMatch =
			(ginstate->canPartialMatch[attnum - 1] && partial_matches)
			? partial_matches[i] : false;
		this_extra = (extra_data) ? extra_data[i] : NULL;

		key->scanEntry[i] = ginFillScanEntry(so, attnum,
											 strategy, searchMode,
											 queryKey, queryCategory,
											 isPartialMatch, this_extra);
	}

	/*
	 * For GIN_SEARCH_MODE_INCLUDE_EMPTY and GIN_SEARCH_MODE_EVERYTHING search
	 * modes, we add the "hidden" entry immediately.  GIN_SEARCH_MODE_ALL is
	 * handled later, since we might be able to omit the hidden entry for it.
	 */
	if (searchMode == GIN_SEARCH_MODE_INCLUDE_EMPTY)
		ginScanKeyAddHiddenEntry(so, key, GIN_CAT_EMPTY_ITEM);
	else if (searchMode == GIN_SEARCH_MODE_EVERYTHING)
		ginScanKeyAddHiddenEntry(so, key, GIN_CAT_EMPTY_QUERY);
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
	bool		attrHasNormalScan[INDEX_MAX_KEYS] = {false};
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
		GinNullCategory *categories;
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
		 * Create GinNullCategory representation.  If the extractQueryFn
		 * didn't create a nullFlags array, we assume everything is non-null.
		 * While at it, detect whether any null keys are present.
		 */
		categories = (GinNullCategory *) palloc0(nQueryValues * sizeof(GinNullCategory));
		if (nullFlags)
		{
			int32		j;

			for (j = 0; j < nQueryValues; j++)
			{
				if (nullFlags[j])
				{
					categories[j] = GIN_CAT_NULL_KEY;
					hasNullQuery = true;
				}
			}
		}

		ginFillScanKey(so, skey->sk_attno,
					   skey->sk_strategy, searchMode,
					   skey->sk_argument, nQueryValues,
					   queryValues, categories,
					   partial_matches, extra_data);

		/* Remember if we had any non-excludeOnly keys */
		if (searchMode != GIN_SEARCH_MODE_ALL)
			attrHasNormalScan[skey->sk_attno - 1] = true;
	}

	/*
	 * Processing GIN_SEARCH_MODE_ALL scan keys requires us to make a second
	 * pass over the scan keys.  Above we marked each such scan key as
	 * excludeOnly.  If the involved column has any normal (not excludeOnly)
	 * scan key as well, then we can leave it like that.  Otherwise, one
	 * excludeOnly scan key must receive a GIN_CAT_EMPTY_QUERY hidden entry
	 * and be set to normal (excludeOnly = false).
	 */
	for (i = 0; i < so->nkeys; i++)
	{
		GinScanKey	key = &so->keys[i];

		if (key->searchMode != GIN_SEARCH_MODE_ALL)
			continue;

		if (!attrHasNormalScan[key->attnum - 1])
		{
			key->excludeOnly = false;
			ginScanKeyAddHiddenEntry(so, key, GIN_CAT_EMPTY_QUERY);
			attrHasNormalScan[key->attnum - 1] = true;
		}
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

void
ginrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
		  ScanKey orderbys, int norderbys)
{
	GinScanOpaque so = (GinScanOpaque) scan->opaque;

	ginFreeScanKeys(so);

	if (scankey && scan->numberOfKeys > 0)
	{
		memmove(scan->keyData, scankey,
				scan->numberOfKeys * sizeof(ScanKeyData));
	}
}


void
ginendscan(IndexScanDesc scan)
{
	GinScanOpaque so = (GinScanOpaque) scan->opaque;

	ginFreeScanKeys(so);

	MemoryContextDelete(so->tempCtx);
	MemoryContextDelete(so->keyCtx);

	pfree(so);
}
