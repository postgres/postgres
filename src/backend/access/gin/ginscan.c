/*-------------------------------------------------------------------------
 *
 * ginscan.c
 *	  routines to manage scans inverted index relations
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			$PostgreSQL: pgsql/src/backend/access/gin/ginscan.c,v 1.24 2009/06/11 14:48:53 momjian Exp $
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin.h"
#include "access/relscan.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"


Datum
ginbeginscan(PG_FUNCTION_ARGS)
{
	Relation	rel = (Relation) PG_GETARG_POINTER(0);
	int			keysz = PG_GETARG_INT32(1);
	ScanKey		scankey = (ScanKey) PG_GETARG_POINTER(2);
	IndexScanDesc scan;

	scan = RelationGetIndexScan(rel, keysz, scankey);

	PG_RETURN_POINTER(scan);
}

static void
fillScanKey(GinState *ginstate, GinScanKey key, OffsetNumber attnum, Datum query,
			Datum *entryValues, bool *partial_matches, uint32 nEntryValues,
			StrategyNumber strategy, Pointer *extra_data)
{
	uint32		i,
				j;

	key->nentries = nEntryValues;
	key->entryRes = (bool *) palloc0(sizeof(bool) * nEntryValues);
	key->scanEntry = (GinScanEntry) palloc(sizeof(GinScanEntryData) * nEntryValues);
	key->strategy = strategy;
	key->attnum = attnum;
	key->extra_data = extra_data;
	key->query = query;
	key->firstCall = TRUE;
	ItemPointerSet(&(key->curItem), InvalidBlockNumber, InvalidOffsetNumber);

	for (i = 0; i < nEntryValues; i++)
	{
		key->scanEntry[i].pval = key->entryRes + i;
		key->scanEntry[i].entry = entryValues[i];
		key->scanEntry[i].attnum = attnum;
		key->scanEntry[i].extra_data = (extra_data) ? extra_data[i] : NULL;
		ItemPointerSet(&(key->scanEntry[i].curItem), InvalidBlockNumber, InvalidOffsetNumber);
		key->scanEntry[i].offset = InvalidOffsetNumber;
		key->scanEntry[i].buffer = InvalidBuffer;
		key->scanEntry[i].partialMatch = NULL;
		key->scanEntry[i].partialMatchIterator = NULL;
		key->scanEntry[i].partialMatchResult = NULL;
		key->scanEntry[i].strategy = strategy;
		key->scanEntry[i].list = NULL;
		key->scanEntry[i].nlist = 0;
		key->scanEntry[i].isPartialMatch = (ginstate->canPartialMatch[attnum - 1] && partial_matches)
			? partial_matches[i] : false;

		/* link to the equals entry in current scan key */
		key->scanEntry[i].master = NULL;
		for (j = 0; j < i; j++)
			if (compareEntries(ginstate, attnum, entryValues[i], entryValues[j]) == 0)
			{
				key->scanEntry[i].master = key->scanEntry + j;
				break;
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
		ItemPointerSet(&(key->curItem), InvalidBlockNumber, InvalidOffsetNumber);

		for (j = 0; j < key->nentries; j++)
		{
			if (key->scanEntry[j].buffer != InvalidBuffer)
				ReleaseBuffer(key->scanEntry[i].buffer);

			ItemPointerSet(&(key->scanEntry[j].curItem), InvalidBlockNumber, InvalidOffsetNumber);
			key->scanEntry[j].offset = InvalidOffsetNumber;
			key->scanEntry[j].buffer = InvalidBuffer;
			key->scanEntry[j].list = NULL;
			key->scanEntry[j].nlist = 0;
			key->scanEntry[j].partialMatch = NULL;
			key->scanEntry[j].partialMatchIterator = NULL;
			key->scanEntry[j].partialMatchResult = NULL;
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
			if (key->scanEntry[j].partialMatchIterator)
				tbm_end_iterate(key->scanEntry[j].partialMatchIterator);
			if (key->scanEntry[j].partialMatch)
				tbm_free(key->scanEntry[j].partialMatch);
		}

		pfree(key->entryRes);
		pfree(key->scanEntry);
	}

	pfree(keys);
}

void
newScanKey(IndexScanDesc scan)
{
	ScanKey		scankey = scan->keyData;
	GinScanOpaque so = (GinScanOpaque) scan->opaque;
	int			i;
	uint32		nkeys = 0;

	if (scan->numberOfKeys < 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("GIN indexes do not support whole-index scans")));

	so->keys = (GinScanKey) palloc(scan->numberOfKeys * sizeof(GinScanKeyData));

	so->isVoidRes = false;

	for (i = 0; i < scan->numberOfKeys; i++)
	{
		ScanKey		skey = &scankey[i];
		Datum	   *entryValues;
		int32		nEntryValues = 0;
		bool	   *partial_matches = NULL;
		Pointer    *extra_data = NULL;

		/*
		 * Assume, that GIN-indexable operators are strict, so nothing could
		 * be found
		 */
		if (skey->sk_flags & SK_ISNULL)
		{
			so->isVoidRes = true;
			break;
		}

		entryValues = (Datum *)
			DatumGetPointer(FunctionCall5(&so->ginstate.extractQueryFn[skey->sk_attno - 1],
										  skey->sk_argument,
										  PointerGetDatum(&nEntryValues),
										  UInt16GetDatum(skey->sk_strategy),
										  PointerGetDatum(&partial_matches),
										  PointerGetDatum(&extra_data)));

		if (nEntryValues < 0)
		{
			/*
			 * extractQueryFn signals that nothing can match, so we can just
			 * set isVoidRes flag.	No need to examine any more keys.
			 */
			so->isVoidRes = true;
			break;
		}

		if (entryValues == NULL || nEntryValues == 0)
		{
			/*
			 * extractQueryFn signals that everything matches.	This would
			 * require a full scan, which we can't do, but perhaps there is
			 * another scankey that provides a restriction to use.	So we keep
			 * going and check only at the end.
			 */
			continue;
		}

		fillScanKey(&so->ginstate, &(so->keys[nkeys]),
					skey->sk_attno, skey->sk_argument,
					entryValues, partial_matches, nEntryValues,
					skey->sk_strategy, extra_data);
		nkeys++;
	}

	if (nkeys == 0 && !so->isVoidRes)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("GIN indexes do not support whole-index scans")));

	so->nkeys = nkeys;

	pgstat_count_index_scan(scan->indexRelation);
}

Datum
ginrescan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ScanKey		scankey = (ScanKey) PG_GETARG_POINTER(1);
	GinScanOpaque so;

	so = (GinScanOpaque) scan->opaque;

	if (so == NULL)
	{
		/* if called from ginbeginscan */
		so = (GinScanOpaque) palloc(sizeof(GinScanOpaqueData));
		so->tempCtx = AllocSetContextCreate(CurrentMemoryContext,
											"Gin scan temporary context",
											ALLOCSET_DEFAULT_MINSIZE,
											ALLOCSET_DEFAULT_INITSIZE,
											ALLOCSET_DEFAULT_MAXSIZE);
		initGinState(&so->ginstate, scan->indexRelation);
		scan->opaque = so;
	}
	else
	{
		freeScanKeys(so->keys, so->nkeys);
	}

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

	if (so != NULL)
	{
		freeScanKeys(so->keys, so->nkeys);

		MemoryContextDelete(so->tempCtx);

		pfree(so);
	}

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
