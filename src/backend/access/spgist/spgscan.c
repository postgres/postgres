/*-------------------------------------------------------------------------
 *
 * spgscan.c
 *	  routines for scanning SP-GiST indexes
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/spgist/spgscan.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/relscan.h"
#include "access/spgist_private.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "utils/datum.h"
#include "utils/float.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

typedef void (*storeRes_func) (SpGistScanOpaque so, ItemPointer heapPtr,
							   Datum leafValue, bool isNull,
							   SpGistLeafTuple leafTuple, bool recheck,
							   bool recheckDistances, double *distances);

/*
 * Pairing heap comparison function for the SpGistSearchItem queue.
 * KNN-searches currently only support NULLS LAST.  So, preserve this logic
 * here.
 */
static int
pairingheap_SpGistSearchItem_cmp(const pairingheap_node *a,
								 const pairingheap_node *b, void *arg)
{
	const SpGistSearchItem *sa = (const SpGistSearchItem *) a;
	const SpGistSearchItem *sb = (const SpGistSearchItem *) b;
	SpGistScanOpaque so = (SpGistScanOpaque) arg;
	int			i;

	if (sa->isNull)
	{
		if (!sb->isNull)
			return -1;
	}
	else if (sb->isNull)
	{
		return 1;
	}
	else
	{
		/* Order according to distance comparison */
		for (i = 0; i < so->numberOfNonNullOrderBys; i++)
		{
			if (isnan(sa->distances[i]) && isnan(sb->distances[i]))
				continue;		/* NaN == NaN */
			if (isnan(sa->distances[i]))
				return -1;		/* NaN > number */
			if (isnan(sb->distances[i]))
				return 1;		/* number < NaN */
			if (sa->distances[i] != sb->distances[i])
				return (sa->distances[i] < sb->distances[i]) ? 1 : -1;
		}
	}

	/* Leaf items go before inner pages, to ensure a depth-first search */
	if (sa->isLeaf && !sb->isLeaf)
		return 1;
	if (!sa->isLeaf && sb->isLeaf)
		return -1;

	return 0;
}

static void
spgFreeSearchItem(SpGistScanOpaque so, SpGistSearchItem *item)
{
	/* value is of type attType if isLeaf, else of type attLeafType */
	/* (no, that is not backwards; yes, it's confusing) */
	if (!(item->isLeaf ? so->state.attType.attbyval :
		  so->state.attLeafType.attbyval) &&
		DatumGetPointer(item->value) != NULL)
		pfree(DatumGetPointer(item->value));

	if (item->leafTuple)
		pfree(item->leafTuple);

	if (item->traversalValue)
		pfree(item->traversalValue);

	pfree(item);
}

/*
 * Add SpGistSearchItem to queue
 *
 * Called in queue context
 */
static void
spgAddSearchItemToQueue(SpGistScanOpaque so, SpGistSearchItem *item)
{
	pairingheap_add(so->scanQueue, &item->phNode);
}

static SpGistSearchItem *
spgAllocSearchItem(SpGistScanOpaque so, bool isnull, double *distances)
{
	/* allocate distance array only for non-NULL items */
	SpGistSearchItem *item =
		palloc(SizeOfSpGistSearchItem(isnull ? 0 : so->numberOfNonNullOrderBys));

	item->isNull = isnull;

	if (!isnull && so->numberOfNonNullOrderBys > 0)
		memcpy(item->distances, distances,
			   sizeof(item->distances[0]) * so->numberOfNonNullOrderBys);

	return item;
}

static void
spgAddStartItem(SpGistScanOpaque so, bool isnull)
{
	SpGistSearchItem *startEntry =
		spgAllocSearchItem(so, isnull, so->zeroDistances);

	ItemPointerSet(&startEntry->heapPtr,
				   isnull ? SPGIST_NULL_BLKNO : SPGIST_ROOT_BLKNO,
				   FirstOffsetNumber);
	startEntry->isLeaf = false;
	startEntry->level = 0;
	startEntry->value = (Datum) 0;
	startEntry->leafTuple = NULL;
	startEntry->traversalValue = NULL;
	startEntry->recheck = false;
	startEntry->recheckDistances = false;

	spgAddSearchItemToQueue(so, startEntry);
}

/*
 * Initialize queue to search the root page, resetting
 * any previously active scan
 */
static void
resetSpGistScanOpaque(SpGistScanOpaque so)
{
	MemoryContext oldCtx;

	MemoryContextReset(so->traversalCxt);

	oldCtx = MemoryContextSwitchTo(so->traversalCxt);

	/* initialize queue only for distance-ordered scans */
	so->scanQueue = pairingheap_allocate(pairingheap_SpGistSearchItem_cmp, so);

	if (so->searchNulls)
		/* Add a work item to scan the null index entries */
		spgAddStartItem(so, true);

	if (so->searchNonNulls)
		/* Add a work item to scan the non-null index entries */
		spgAddStartItem(so, false);

	MemoryContextSwitchTo(oldCtx);

	if (so->numberOfOrderBys > 0)
	{
		/* Must pfree distances to avoid memory leak */
		int			i;

		for (i = 0; i < so->nPtrs; i++)
			if (so->distances[i])
				pfree(so->distances[i]);
	}

	if (so->want_itup)
	{
		/* Must pfree reconstructed tuples to avoid memory leak */
		int			i;

		for (i = 0; i < so->nPtrs; i++)
			pfree(so->reconTups[i]);
	}
	so->iPtr = so->nPtrs = 0;
}

/*
 * Prepare scan keys in SpGistScanOpaque from caller-given scan keys
 *
 * Sets searchNulls, searchNonNulls, numberOfKeys, keyData fields of *so.
 *
 * The point here is to eliminate null-related considerations from what the
 * opclass consistent functions need to deal with.  We assume all SPGiST-
 * indexable operators are strict, so any null RHS value makes the scan
 * condition unsatisfiable.  We also pull out any IS NULL/IS NOT NULL
 * conditions; their effect is reflected into searchNulls/searchNonNulls.
 */
static void
spgPrepareScanKeys(IndexScanDesc scan)
{
	SpGistScanOpaque so = (SpGistScanOpaque) scan->opaque;
	bool		qual_ok;
	bool		haveIsNull;
	bool		haveNotNull;
	int			nkeys;
	int			i;

	so->numberOfOrderBys = scan->numberOfOrderBys;
	so->orderByData = scan->orderByData;

	if (so->numberOfOrderBys <= 0)
		so->numberOfNonNullOrderBys = 0;
	else
	{
		int			j = 0;

		/*
		 * Remove all NULL keys, but remember their offsets in the original
		 * array.
		 */
		for (i = 0; i < scan->numberOfOrderBys; i++)
		{
			ScanKey		skey = &so->orderByData[i];

			if (skey->sk_flags & SK_ISNULL)
				so->nonNullOrderByOffsets[i] = -1;
			else
			{
				if (i != j)
					so->orderByData[j] = *skey;

				so->nonNullOrderByOffsets[i] = j++;
			}
		}

		so->numberOfNonNullOrderBys = j;
	}

	if (scan->numberOfKeys <= 0)
	{
		/* If no quals, whole-index scan is required */
		so->searchNulls = true;
		so->searchNonNulls = true;
		so->numberOfKeys = 0;
		return;
	}

	/* Examine the given quals */
	qual_ok = true;
	haveIsNull = haveNotNull = false;
	nkeys = 0;
	for (i = 0; i < scan->numberOfKeys; i++)
	{
		ScanKey		skey = &scan->keyData[i];

		if (skey->sk_flags & SK_SEARCHNULL)
			haveIsNull = true;
		else if (skey->sk_flags & SK_SEARCHNOTNULL)
			haveNotNull = true;
		else if (skey->sk_flags & SK_ISNULL)
		{
			/* ordinary qual with null argument - unsatisfiable */
			qual_ok = false;
			break;
		}
		else
		{
			/* ordinary qual, propagate into so->keyData */
			so->keyData[nkeys++] = *skey;
			/* this effectively creates a not-null requirement */
			haveNotNull = true;
		}
	}

	/* IS NULL in combination with something else is unsatisfiable */
	if (haveIsNull && haveNotNull)
		qual_ok = false;

	/* Emit results */
	if (qual_ok)
	{
		so->searchNulls = haveIsNull;
		so->searchNonNulls = haveNotNull;
		so->numberOfKeys = nkeys;
	}
	else
	{
		so->searchNulls = false;
		so->searchNonNulls = false;
		so->numberOfKeys = 0;
	}
}

IndexScanDesc
spgbeginscan(Relation rel, int keysz, int orderbysz)
{
	IndexScanDesc scan;
	SpGistScanOpaque so;
	int			i;

	scan = RelationGetIndexScan(rel, keysz, orderbysz);

	so = (SpGistScanOpaque) palloc0(sizeof(SpGistScanOpaqueData));
	if (keysz > 0)
		so->keyData = (ScanKey) palloc(sizeof(ScanKeyData) * keysz);
	else
		so->keyData = NULL;
	initSpGistState(&so->state, scan->indexRelation);

	so->tempCxt = AllocSetContextCreate(CurrentMemoryContext,
										"SP-GiST search temporary context",
										ALLOCSET_DEFAULT_SIZES);
	so->traversalCxt = AllocSetContextCreate(CurrentMemoryContext,
											 "SP-GiST traversal-value context",
											 ALLOCSET_DEFAULT_SIZES);

	/*
	 * Set up reconTupDesc and xs_hitupdesc in case it's an index-only scan,
	 * making sure that the key column is shown as being of type attType.
	 * (It's rather annoying to do this work when it might be wasted, but for
	 * most opclasses we can re-use the index reldesc instead of making one.)
	 */
	so->reconTupDesc = scan->xs_hitupdesc =
		getSpGistTupleDesc(rel, &so->state.attType);

	/* Allocate various arrays needed for order-by scans */
	if (scan->numberOfOrderBys > 0)
	{
		/* This will be filled in spgrescan, but allocate the space here */
		so->orderByTypes = (Oid *)
			palloc(sizeof(Oid) * scan->numberOfOrderBys);
		so->nonNullOrderByOffsets = (int *)
			palloc(sizeof(int) * scan->numberOfOrderBys);

		/* These arrays have constant contents, so we can fill them now */
		so->zeroDistances = (double *)
			palloc(sizeof(double) * scan->numberOfOrderBys);
		so->infDistances = (double *)
			palloc(sizeof(double) * scan->numberOfOrderBys);

		for (i = 0; i < scan->numberOfOrderBys; i++)
		{
			so->zeroDistances[i] = 0.0;
			so->infDistances[i] = get_float8_infinity();
		}

		scan->xs_orderbyvals = (Datum *)
			palloc0(sizeof(Datum) * scan->numberOfOrderBys);
		scan->xs_orderbynulls = (bool *)
			palloc(sizeof(bool) * scan->numberOfOrderBys);
		memset(scan->xs_orderbynulls, true,
			   sizeof(bool) * scan->numberOfOrderBys);
	}

	fmgr_info_copy(&so->innerConsistentFn,
				   index_getprocinfo(rel, 1, SPGIST_INNER_CONSISTENT_PROC),
				   CurrentMemoryContext);

	fmgr_info_copy(&so->leafConsistentFn,
				   index_getprocinfo(rel, 1, SPGIST_LEAF_CONSISTENT_PROC),
				   CurrentMemoryContext);

	so->indexCollation = rel->rd_indcollation[0];

	scan->opaque = so;

	return scan;
}

void
spgrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
		  ScanKey orderbys, int norderbys)
{
	SpGistScanOpaque so = (SpGistScanOpaque) scan->opaque;

	/* copy scankeys into local storage */
	if (scankey && scan->numberOfKeys > 0)
		memcpy(scan->keyData, scankey, scan->numberOfKeys * sizeof(ScanKeyData));

	/* initialize order-by data if needed */
	if (orderbys && scan->numberOfOrderBys > 0)
	{
		int			i;

		memcpy(scan->orderByData, orderbys, scan->numberOfOrderBys * sizeof(ScanKeyData));

		for (i = 0; i < scan->numberOfOrderBys; i++)
		{
			ScanKey		skey = &scan->orderByData[i];

			/*
			 * Look up the datatype returned by the original ordering
			 * operator. SP-GiST always uses a float8 for the distance
			 * function, but the ordering operator could be anything else.
			 *
			 * XXX: The distance function is only allowed to be lossy if the
			 * ordering operator's result type is float4 or float8.  Otherwise
			 * we don't know how to return the distance to the executor.  But
			 * we cannot check that here, as we won't know if the distance
			 * function is lossy until it returns *recheck = true for the
			 * first time.
			 */
			so->orderByTypes[i] = get_func_rettype(skey->sk_func.fn_oid);
		}
	}

	/* preprocess scankeys, set up the representation in *so */
	spgPrepareScanKeys(scan);

	/* set up starting queue entries */
	resetSpGistScanOpaque(so);

	/* count an indexscan for stats */
	pgstat_count_index_scan(scan->indexRelation);
}

void
spgendscan(IndexScanDesc scan)
{
	SpGistScanOpaque so = (SpGistScanOpaque) scan->opaque;

	MemoryContextDelete(so->tempCxt);
	MemoryContextDelete(so->traversalCxt);

	if (so->keyData)
		pfree(so->keyData);

	if (so->state.leafTupDesc &&
		so->state.leafTupDesc != RelationGetDescr(so->state.index))
		FreeTupleDesc(so->state.leafTupDesc);

	if (so->state.deadTupleStorage)
		pfree(so->state.deadTupleStorage);

	if (scan->numberOfOrderBys > 0)
	{
		pfree(so->orderByTypes);
		pfree(so->nonNullOrderByOffsets);
		pfree(so->zeroDistances);
		pfree(so->infDistances);
		pfree(scan->xs_orderbyvals);
		pfree(scan->xs_orderbynulls);
	}

	pfree(so);
}

/*
 * Leaf SpGistSearchItem constructor, called in queue context
 */
static SpGistSearchItem *
spgNewHeapItem(SpGistScanOpaque so, int level, SpGistLeafTuple leafTuple,
			   Datum leafValue, bool recheck, bool recheckDistances,
			   bool isnull, double *distances)
{
	SpGistSearchItem *item = spgAllocSearchItem(so, isnull, distances);

	item->level = level;
	item->heapPtr = leafTuple->heapPtr;

	/*
	 * If we need the reconstructed value, copy it to queue cxt out of tmp
	 * cxt.  Caution: the leaf_consistent method may not have supplied a value
	 * if we didn't ask it to, and mildly-broken methods might supply one of
	 * the wrong type.  The correct leafValue type is attType not leafType.
	 */
	if (so->want_itup)
	{
		item->value = isnull ? (Datum) 0 :
			datumCopy(leafValue, so->state.attType.attbyval,
					  so->state.attType.attlen);

		/*
		 * If we're going to need to reconstruct INCLUDE attributes, store the
		 * whole leaf tuple so we can get the INCLUDE attributes out of it.
		 */
		if (so->state.leafTupDesc->natts > 1)
		{
			item->leafTuple = palloc(leafTuple->size);
			memcpy(item->leafTuple, leafTuple, leafTuple->size);
		}
		else
			item->leafTuple = NULL;
	}
	else
	{
		item->value = (Datum) 0;
		item->leafTuple = NULL;
	}
	item->traversalValue = NULL;
	item->isLeaf = true;
	item->recheck = recheck;
	item->recheckDistances = recheckDistances;

	return item;
}

/*
 * Test whether a leaf tuple satisfies all the scan keys
 *
 * *reportedSome is set to true if:
 *		the scan is not ordered AND the item satisfies the scankeys
 */
static bool
spgLeafTest(SpGistScanOpaque so, SpGistSearchItem *item,
			SpGistLeafTuple leafTuple, bool isnull,
			bool *reportedSome, storeRes_func storeRes)
{
	Datum		leafValue;
	double	   *distances;
	bool		result;
	bool		recheck;
	bool		recheckDistances;

	if (isnull)
	{
		/* Should not have arrived on a nulls page unless nulls are wanted */
		Assert(so->searchNulls);
		leafValue = (Datum) 0;
		distances = NULL;
		recheck = false;
		recheckDistances = false;
		result = true;
	}
	else
	{
		spgLeafConsistentIn in;
		spgLeafConsistentOut out;

		/* use temp context for calling leaf_consistent */
		MemoryContext oldCxt = MemoryContextSwitchTo(so->tempCxt);

		in.scankeys = so->keyData;
		in.nkeys = so->numberOfKeys;
		in.orderbys = so->orderByData;
		in.norderbys = so->numberOfNonNullOrderBys;
		Assert(!item->isLeaf);	/* else reconstructedValue would be wrong type */
		in.reconstructedValue = item->value;
		in.traversalValue = item->traversalValue;
		in.level = item->level;
		in.returnData = so->want_itup;
		in.leafDatum = SGLTDATUM(leafTuple, &so->state);

		out.leafValue = (Datum) 0;
		out.recheck = false;
		out.distances = NULL;
		out.recheckDistances = false;

		result = DatumGetBool(FunctionCall2Coll(&so->leafConsistentFn,
												so->indexCollation,
												PointerGetDatum(&in),
												PointerGetDatum(&out)));
		recheck = out.recheck;
		recheckDistances = out.recheckDistances;
		leafValue = out.leafValue;
		distances = out.distances;

		MemoryContextSwitchTo(oldCxt);
	}

	if (result)
	{
		/* item passes the scankeys */
		if (so->numberOfNonNullOrderBys > 0)
		{
			/* the scan is ordered -> add the item to the queue */
			MemoryContext oldCxt = MemoryContextSwitchTo(so->traversalCxt);
			SpGistSearchItem *heapItem = spgNewHeapItem(so, item->level,
														leafTuple,
														leafValue,
														recheck,
														recheckDistances,
														isnull,
														distances);

			spgAddSearchItemToQueue(so, heapItem);

			MemoryContextSwitchTo(oldCxt);
		}
		else
		{
			/* non-ordered scan, so report the item right away */
			Assert(!recheckDistances);
			storeRes(so, &leafTuple->heapPtr, leafValue, isnull,
					 leafTuple, recheck, false, NULL);
			*reportedSome = true;
		}
	}

	return result;
}

/* A bundle initializer for inner_consistent methods */
static void
spgInitInnerConsistentIn(spgInnerConsistentIn *in,
						 SpGistScanOpaque so,
						 SpGistSearchItem *item,
						 SpGistInnerTuple innerTuple)
{
	in->scankeys = so->keyData;
	in->orderbys = so->orderByData;
	in->nkeys = so->numberOfKeys;
	in->norderbys = so->numberOfNonNullOrderBys;
	Assert(!item->isLeaf);		/* else reconstructedValue would be wrong type */
	in->reconstructedValue = item->value;
	in->traversalMemoryContext = so->traversalCxt;
	in->traversalValue = item->traversalValue;
	in->level = item->level;
	in->returnData = so->want_itup;
	in->allTheSame = innerTuple->allTheSame;
	in->hasPrefix = (innerTuple->prefixSize > 0);
	in->prefixDatum = SGITDATUM(innerTuple, &so->state);
	in->nNodes = innerTuple->nNodes;
	in->nodeLabels = spgExtractNodeLabels(&so->state, innerTuple);
}

static SpGistSearchItem *
spgMakeInnerItem(SpGistScanOpaque so,
				 SpGistSearchItem *parentItem,
				 SpGistNodeTuple tuple,
				 spgInnerConsistentOut *out, int i, bool isnull,
				 double *distances)
{
	SpGistSearchItem *item = spgAllocSearchItem(so, isnull, distances);

	item->heapPtr = tuple->t_tid;
	item->level = out->levelAdds ? parentItem->level + out->levelAdds[i]
		: parentItem->level;

	/* Must copy value out of temp context */
	/* (recall that reconstructed values are of type leafType) */
	item->value = out->reconstructedValues
		? datumCopy(out->reconstructedValues[i],
					so->state.attLeafType.attbyval,
					so->state.attLeafType.attlen)
		: (Datum) 0;

	item->leafTuple = NULL;

	/*
	 * Elements of out.traversalValues should be allocated in
	 * in.traversalMemoryContext, which is actually a long lived context of
	 * index scan.
	 */
	item->traversalValue =
		out->traversalValues ? out->traversalValues[i] : NULL;

	item->isLeaf = false;
	item->recheck = false;
	item->recheckDistances = false;

	return item;
}

static void
spgInnerTest(SpGistScanOpaque so, SpGistSearchItem *item,
			 SpGistInnerTuple innerTuple, bool isnull)
{
	MemoryContext oldCxt = MemoryContextSwitchTo(so->tempCxt);
	spgInnerConsistentOut out;
	int			nNodes = innerTuple->nNodes;
	int			i;

	memset(&out, 0, sizeof(out));

	if (!isnull)
	{
		spgInnerConsistentIn in;

		spgInitInnerConsistentIn(&in, so, item, innerTuple);

		/* use user-defined inner consistent method */
		FunctionCall2Coll(&so->innerConsistentFn,
						  so->indexCollation,
						  PointerGetDatum(&in),
						  PointerGetDatum(&out));
	}
	else
	{
		/* force all children to be visited */
		out.nNodes = nNodes;
		out.nodeNumbers = (int *) palloc(sizeof(int) * nNodes);
		for (i = 0; i < nNodes; i++)
			out.nodeNumbers[i] = i;
	}

	/* If allTheSame, they should all or none of them match */
	if (innerTuple->allTheSame && out.nNodes != 0 && out.nNodes != nNodes)
		elog(ERROR, "inconsistent inner_consistent results for allTheSame inner tuple");

	if (out.nNodes)
	{
		/* collect node pointers */
		SpGistNodeTuple node;
		SpGistNodeTuple *nodes = (SpGistNodeTuple *) palloc(sizeof(SpGistNodeTuple) * nNodes);

		SGITITERATE(innerTuple, i, node)
		{
			nodes[i] = node;
		}

		MemoryContextSwitchTo(so->traversalCxt);

		for (i = 0; i < out.nNodes; i++)
		{
			int			nodeN = out.nodeNumbers[i];
			SpGistSearchItem *innerItem;
			double	   *distances;

			Assert(nodeN >= 0 && nodeN < nNodes);

			node = nodes[nodeN];

			if (!ItemPointerIsValid(&node->t_tid))
				continue;

			/*
			 * Use infinity distances if innerConsistentFn() failed to return
			 * them or if is a NULL item (their distances are really unused).
			 */
			distances = out.distances ? out.distances[i] : so->infDistances;

			innerItem = spgMakeInnerItem(so, item, node, &out, i, isnull,
										 distances);

			spgAddSearchItemToQueue(so, innerItem);
		}
	}

	MemoryContextSwitchTo(oldCxt);
}

/* Returns a next item in an (ordered) scan or null if the index is exhausted */
static SpGistSearchItem *
spgGetNextQueueItem(SpGistScanOpaque so)
{
	if (pairingheap_is_empty(so->scanQueue))
		return NULL;			/* Done when both heaps are empty */

	/* Return item; caller is responsible to pfree it */
	return (SpGistSearchItem *) pairingheap_remove_first(so->scanQueue);
}

enum SpGistSpecialOffsetNumbers
{
	SpGistBreakOffsetNumber = InvalidOffsetNumber,
	SpGistRedirectOffsetNumber = MaxOffsetNumber + 1,
	SpGistErrorOffsetNumber = MaxOffsetNumber + 2,
};

static OffsetNumber
spgTestLeafTuple(SpGistScanOpaque so,
				 SpGistSearchItem *item,
				 Page page, OffsetNumber offset,
				 bool isnull, bool isroot,
				 bool *reportedSome,
				 storeRes_func storeRes)
{
	SpGistLeafTuple leafTuple = (SpGistLeafTuple)
		PageGetItem(page, PageGetItemId(page, offset));

	if (leafTuple->tupstate != SPGIST_LIVE)
	{
		if (!isroot)			/* all tuples on root should be live */
		{
			if (leafTuple->tupstate == SPGIST_REDIRECT)
			{
				/* redirection tuple should be first in chain */
				Assert(offset == ItemPointerGetOffsetNumber(&item->heapPtr));
				/* transfer attention to redirect point */
				item->heapPtr = ((SpGistDeadTuple) leafTuple)->pointer;
				Assert(ItemPointerGetBlockNumber(&item->heapPtr) != SPGIST_METAPAGE_BLKNO);
				return SpGistRedirectOffsetNumber;
			}

			if (leafTuple->tupstate == SPGIST_DEAD)
			{
				/* dead tuple should be first in chain */
				Assert(offset == ItemPointerGetOffsetNumber(&item->heapPtr));
				/* No live entries on this page */
				Assert(SGLT_GET_NEXTOFFSET(leafTuple) == InvalidOffsetNumber);
				return SpGistBreakOffsetNumber;
			}
		}

		/* We should not arrive at a placeholder */
		elog(ERROR, "unexpected SPGiST tuple state: %d", leafTuple->tupstate);
		return SpGistErrorOffsetNumber;
	}

	Assert(ItemPointerIsValid(&leafTuple->heapPtr));

	spgLeafTest(so, item, leafTuple, isnull, reportedSome, storeRes);

	return SGLT_GET_NEXTOFFSET(leafTuple);
}

/*
 * Walk the tree and report all tuples passing the scan quals to the storeRes
 * subroutine.
 *
 * If scanWholeIndex is true, we'll do just that.  If not, we'll stop at the
 * next page boundary once we have reported at least one tuple.
 */
static void
spgWalk(Relation index, SpGistScanOpaque so, bool scanWholeIndex,
		storeRes_func storeRes)
{
	Buffer		buffer = InvalidBuffer;
	bool		reportedSome = false;

	while (scanWholeIndex || !reportedSome)
	{
		SpGistSearchItem *item = spgGetNextQueueItem(so);

		if (item == NULL)
			break;				/* No more items in queue -> done */

redirect:
		/* Check for interrupts, just in case of infinite loop */
		CHECK_FOR_INTERRUPTS();

		if (item->isLeaf)
		{
			/* We store heap items in the queue only in case of ordered search */
			Assert(so->numberOfNonNullOrderBys > 0);
			storeRes(so, &item->heapPtr, item->value, item->isNull,
					 item->leafTuple, item->recheck,
					 item->recheckDistances, item->distances);
			reportedSome = true;
		}
		else
		{
			BlockNumber blkno = ItemPointerGetBlockNumber(&item->heapPtr);
			OffsetNumber offset = ItemPointerGetOffsetNumber(&item->heapPtr);
			Page		page;
			bool		isnull;

			if (buffer == InvalidBuffer)
			{
				buffer = ReadBuffer(index, blkno);
				LockBuffer(buffer, BUFFER_LOCK_SHARE);
			}
			else if (blkno != BufferGetBlockNumber(buffer))
			{
				UnlockReleaseBuffer(buffer);
				buffer = ReadBuffer(index, blkno);
				LockBuffer(buffer, BUFFER_LOCK_SHARE);
			}

			/* else new pointer points to the same page, no work needed */

			page = BufferGetPage(buffer);

			isnull = SpGistPageStoresNulls(page) ? true : false;

			if (SpGistPageIsLeaf(page))
			{
				/* Page is a leaf - that is, all its tuples are heap items */
				OffsetNumber max = PageGetMaxOffsetNumber(page);

				if (SpGistBlockIsRoot(blkno))
				{
					/* When root is a leaf, examine all its tuples */
					for (offset = FirstOffsetNumber; offset <= max; offset++)
						(void) spgTestLeafTuple(so, item, page, offset,
												isnull, true,
												&reportedSome, storeRes);
				}
				else
				{
					/* Normal case: just examine the chain we arrived at */
					while (offset != InvalidOffsetNumber)
					{
						Assert(offset >= FirstOffsetNumber && offset <= max);
						offset = spgTestLeafTuple(so, item, page, offset,
												  isnull, false,
												  &reportedSome, storeRes);
						if (offset == SpGistRedirectOffsetNumber)
							goto redirect;
					}
				}
			}
			else				/* page is inner */
			{
				SpGistInnerTuple innerTuple = (SpGistInnerTuple)
					PageGetItem(page, PageGetItemId(page, offset));

				if (innerTuple->tupstate != SPGIST_LIVE)
				{
					if (innerTuple->tupstate == SPGIST_REDIRECT)
					{
						/* transfer attention to redirect point */
						item->heapPtr = ((SpGistDeadTuple) innerTuple)->pointer;
						Assert(ItemPointerGetBlockNumber(&item->heapPtr) !=
							   SPGIST_METAPAGE_BLKNO);
						goto redirect;
					}
					elog(ERROR, "unexpected SPGiST tuple state: %d",
						 innerTuple->tupstate);
				}

				spgInnerTest(so, item, innerTuple, isnull);
			}
		}

		/* done with this scan item */
		spgFreeSearchItem(so, item);
		/* clear temp context before proceeding to the next one */
		MemoryContextReset(so->tempCxt);
	}

	if (buffer != InvalidBuffer)
		UnlockReleaseBuffer(buffer);
}


/* storeRes subroutine for getbitmap case */
static void
storeBitmap(SpGistScanOpaque so, ItemPointer heapPtr,
			Datum leafValue, bool isnull,
			SpGistLeafTuple leafTuple, bool recheck,
			bool recheckDistances, double *distances)
{
	Assert(!recheckDistances && !distances);
	tbm_add_tuples(so->tbm, heapPtr, 1, recheck);
	so->ntids++;
}

int64
spggetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	SpGistScanOpaque so = (SpGistScanOpaque) scan->opaque;

	/* Copy want_itup to *so so we don't need to pass it around separately */
	so->want_itup = false;

	so->tbm = tbm;
	so->ntids = 0;

	spgWalk(scan->indexRelation, so, true, storeBitmap);

	return so->ntids;
}

/* storeRes subroutine for gettuple case */
static void
storeGettuple(SpGistScanOpaque so, ItemPointer heapPtr,
			  Datum leafValue, bool isnull,
			  SpGistLeafTuple leafTuple, bool recheck,
			  bool recheckDistances, double *nonNullDistances)
{
	Assert(so->nPtrs < MaxIndexTuplesPerPage);
	so->heapPtrs[so->nPtrs] = *heapPtr;
	so->recheck[so->nPtrs] = recheck;
	so->recheckDistances[so->nPtrs] = recheckDistances;

	if (so->numberOfOrderBys > 0)
	{
		if (isnull || so->numberOfNonNullOrderBys <= 0)
			so->distances[so->nPtrs] = NULL;
		else
		{
			IndexOrderByDistance *distances =
				palloc(sizeof(distances[0]) * so->numberOfOrderBys);
			int			i;

			for (i = 0; i < so->numberOfOrderBys; i++)
			{
				int			offset = so->nonNullOrderByOffsets[i];

				if (offset >= 0)
				{
					/* Copy non-NULL distance value */
					distances[i].value = nonNullDistances[offset];
					distances[i].isnull = false;
				}
				else
				{
					/* Set distance's NULL flag. */
					distances[i].value = 0.0;
					distances[i].isnull = true;
				}
			}

			so->distances[so->nPtrs] = distances;
		}
	}

	if (so->want_itup)
	{
		/*
		 * Reconstruct index data.  We have to copy the datum out of the temp
		 * context anyway, so we may as well create the tuple here.
		 */
		Datum		leafDatums[INDEX_MAX_KEYS];
		bool		leafIsnulls[INDEX_MAX_KEYS];

		/* We only need to deform the old tuple if it has INCLUDE attributes */
		if (so->state.leafTupDesc->natts > 1)
			spgDeformLeafTuple(leafTuple, so->state.leafTupDesc,
							   leafDatums, leafIsnulls, isnull);

		leafDatums[spgKeyColumn] = leafValue;
		leafIsnulls[spgKeyColumn] = isnull;

		so->reconTups[so->nPtrs] = heap_form_tuple(so->reconTupDesc,
												   leafDatums,
												   leafIsnulls);
	}
	so->nPtrs++;
}

bool
spggettuple(IndexScanDesc scan, ScanDirection dir)
{
	SpGistScanOpaque so = (SpGistScanOpaque) scan->opaque;

	if (dir != ForwardScanDirection)
		elog(ERROR, "SP-GiST only supports forward scan direction");

	/* Copy want_itup to *so so we don't need to pass it around separately */
	so->want_itup = scan->xs_want_itup;

	for (;;)
	{
		if (so->iPtr < so->nPtrs)
		{
			/* continuing to return reported tuples */
			scan->xs_heaptid = so->heapPtrs[so->iPtr];
			scan->xs_recheck = so->recheck[so->iPtr];
			scan->xs_hitup = so->reconTups[so->iPtr];

			if (so->numberOfOrderBys > 0)
				index_store_float8_orderby_distances(scan, so->orderByTypes,
													 so->distances[so->iPtr],
													 so->recheckDistances[so->iPtr]);
			so->iPtr++;
			return true;
		}

		if (so->numberOfOrderBys > 0)
		{
			/* Must pfree distances to avoid memory leak */
			int			i;

			for (i = 0; i < so->nPtrs; i++)
				if (so->distances[i])
					pfree(so->distances[i]);
		}

		if (so->want_itup)
		{
			/* Must pfree reconstructed tuples to avoid memory leak */
			int			i;

			for (i = 0; i < so->nPtrs; i++)
				pfree(so->reconTups[i]);
		}
		so->iPtr = so->nPtrs = 0;

		spgWalk(scan->indexRelation, so, false, storeGettuple);

		if (so->nPtrs == 0)
			break;				/* must have completed scan */
	}

	return false;
}

bool
spgcanreturn(Relation index, int attno)
{
	SpGistCache *cache;

	/* INCLUDE attributes can always be fetched for index-only scans */
	if (attno > 1)
		return true;

	/* We can do it if the opclass config function says so */
	cache = spgGetCache(index);

	return cache->config.canReturnData;
}
