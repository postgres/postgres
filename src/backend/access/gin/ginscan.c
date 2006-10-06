/*-------------------------------------------------------------------------
 *
 * ginscan.c
 *	  routines to manage scans inverted index relations
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			$PostgreSQL: pgsql/src/backend/access/gin/ginscan.c,v 1.7 2006/10/06 17:13:58 petere Exp $
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/genam.h"
#include "access/gin.h"
#include "pgstat.h"
#include "utils/memutils.h"


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
fillScanKey(GinState *ginstate, GinScanKey key, Datum query,
			Datum *entryValues, uint32 nEntryValues, StrategyNumber strategy)
{
	uint32		i,
				j;

	key->nentries = nEntryValues;
	key->entryRes = (bool *) palloc0(sizeof(bool) * nEntryValues);
	key->scanEntry = (GinScanEntry) palloc(sizeof(GinScanEntryData) * nEntryValues);
	key->strategy = strategy;
	key->query = query;
	key->firstCall = TRUE;
	ItemPointerSet(&(key->curItem), InvalidBlockNumber, InvalidOffsetNumber);

	for (i = 0; i < nEntryValues; i++)
	{
		key->scanEntry[i].pval = key->entryRes + i;
		key->scanEntry[i].entry = entryValues[i];
		ItemPointerSet(&(key->scanEntry[i].curItem), InvalidBlockNumber, InvalidOffsetNumber);
		key->scanEntry[i].offset = InvalidOffsetNumber;
		key->scanEntry[i].buffer = InvalidBuffer;
		key->scanEntry[i].list = NULL;
		key->scanEntry[i].nlist = 0;

		/* link to the equals entry in current scan key */
		key->scanEntry[i].master = NULL;
		for (j = 0; j < i; j++)
			if (compareEntries(ginstate, entryValues[i], entryValues[j]) == 0)
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
		}
	}
}
#endif

static void
freeScanKeys(GinScanKey keys, uint32 nkeys, bool removeRes)
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
			if (removeRes && key->scanEntry[j].list)
				pfree(key->scanEntry[j].list);
		}

		if (removeRes)
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

	so->keys = (GinScanKey) palloc(scan->numberOfKeys * sizeof(GinScanKeyData));

	if (scan->numberOfKeys < 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("GIN indexes do not support whole-index scans")));

	for (i = 0; i < scan->numberOfKeys; i++)
	{
		Datum	   *entryValues;
		uint32		nEntryValues;

		if (scankey[i].sk_flags & SK_ISNULL)
			elog(ERROR, "Gin doesn't support NULL as scan key");
		Assert(scankey[i].sk_attno == 1);

		entryValues = (Datum *) DatumGetPointer(
												FunctionCall3(
												&so->ginstate.extractQueryFn,
													  scankey[i].sk_argument,
											  PointerGetDatum(&nEntryValues),
									   UInt16GetDatum(scankey[i].sk_strategy)
															  )
			);
		if (entryValues == NULL || nEntryValues == 0)
			/* full scan... */
			continue;

		fillScanKey(&so->ginstate, &(so->keys[nkeys]), scankey[i].sk_argument,
					entryValues, nEntryValues, scankey[i].sk_strategy);
		nkeys++;
	}

	so->nkeys = nkeys;

	if (so->nkeys == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("GIN index does not support search with void query")));

	pgstat_count_index_scan(&scan->xs_pgstat_info);
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
		freeScanKeys(so->keys, so->nkeys, TRUE);
		freeScanKeys(so->markPos, so->nkeys, FALSE);
	}

	so->markPos = so->keys = NULL;

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
		freeScanKeys(so->keys, so->nkeys, TRUE);
		freeScanKeys(so->markPos, so->nkeys, FALSE);

		MemoryContextDelete(so->tempCtx);

		pfree(so);
	}

	PG_RETURN_VOID();
}

static GinScanKey
copyScanKeys(GinScanKey keys, uint32 nkeys)
{
	GinScanKey	newkeys;
	uint32		i,
				j;

	newkeys = (GinScanKey) palloc(sizeof(GinScanKeyData) * nkeys);
	memcpy(newkeys, keys, sizeof(GinScanKeyData) * nkeys);

	for (i = 0; i < nkeys; i++)
	{
		newkeys[i].scanEntry = (GinScanEntry) palloc(sizeof(GinScanEntryData) * keys[i].nentries);
		memcpy(newkeys[i].scanEntry, keys[i].scanEntry, sizeof(GinScanEntryData) * keys[i].nentries);

		for (j = 0; j < keys[i].nentries; j++)
		{
			if (keys[i].scanEntry[j].buffer != InvalidBuffer)
				IncrBufferRefCount(keys[i].scanEntry[j].buffer);
			if (keys[i].scanEntry[j].master)
			{
				int			masterN = keys[i].scanEntry[j].master - keys[i].scanEntry;

				newkeys[i].scanEntry[j].master = newkeys[i].scanEntry + masterN;
			}
		}
	}

	return newkeys;
}

Datum
ginmarkpos(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	GinScanOpaque so = (GinScanOpaque) scan->opaque;

	freeScanKeys(so->markPos, so->nkeys, FALSE);
	so->markPos = copyScanKeys(so->keys, so->nkeys);

	PG_RETURN_VOID();
}

Datum
ginrestrpos(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	GinScanOpaque so = (GinScanOpaque) scan->opaque;

	freeScanKeys(so->keys, so->nkeys, FALSE);
	so->keys = copyScanKeys(so->markPos, so->nkeys);

	PG_RETURN_VOID();
}
