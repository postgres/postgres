/*-------------------------------------------------------------------------
 *
 * gistscan.c
 *	  routines to manage scans on GiST index relations
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/gist/gistscan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gist_private.h"
#include "access/gistscan.h"
#include "access/relscan.h"
#include "utils/float.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"


/*
 * Pairing heap comparison function for the GISTSearchItem queue
 */
static int
pairingheap_GISTSearchItem_cmp(const pairingheap_node *a, const pairingheap_node *b, void *arg)
{
	const GISTSearchItem *sa = (const GISTSearchItem *) a;
	const GISTSearchItem *sb = (const GISTSearchItem *) b;
	IndexScanDesc scan = (IndexScanDesc) arg;
	int			i;

	/* Order according to distance comparison */
	for (i = 0; i < scan->numberOfOrderBys; i++)
	{
		if (sa->distances[i].isnull)
		{
			if (!sb->distances[i].isnull)
				return -1;
		}
		else if (sb->distances[i].isnull)
		{
			return 1;
		}
		else
		{
			int			cmp = -float8_cmp_internal(sa->distances[i].value,
												   sb->distances[i].value);

			if (cmp != 0)
				return cmp;
		}
	}

	/* Heap items go before inner pages, to ensure a depth-first search */
	if (GISTSearchItemIsHeap(*sa) && !GISTSearchItemIsHeap(*sb))
		return 1;
	if (!GISTSearchItemIsHeap(*sa) && GISTSearchItemIsHeap(*sb))
		return -1;

	return 0;
}


/*
 * Index AM API functions for scanning GiST indexes
 */

IndexScanDesc
gistbeginscan(Relation r, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	GISTSTATE  *giststate;
	GISTScanOpaque so;
	MemoryContext oldCxt;

	scan = RelationGetIndexScan(r, nkeys, norderbys);

	/* First, set up a GISTSTATE with a scan-lifespan memory context */
	giststate = initGISTstate(scan->indexRelation);

	/*
	 * Everything made below is in the scanCxt, or is a child of the scanCxt,
	 * so it'll all go away automatically in gistendscan.
	 */
	oldCxt = MemoryContextSwitchTo(giststate->scanCxt);

	/* initialize opaque data */
	so = (GISTScanOpaque) palloc0(sizeof(GISTScanOpaqueData));
	so->giststate = giststate;
	giststate->tempCxt = createTempGistContext();
	so->queue = NULL;
	so->queueCxt = giststate->scanCxt;	/* see gistrescan */

	/* workspaces with size dependent on numberOfOrderBys: */
	so->distances = palloc(sizeof(so->distances[0]) * scan->numberOfOrderBys);
	so->qual_ok = true;			/* in case there are zero keys */
	if (scan->numberOfOrderBys > 0)
	{
		scan->xs_orderbyvals = palloc0(sizeof(Datum) * scan->numberOfOrderBys);
		scan->xs_orderbynulls = palloc(sizeof(bool) * scan->numberOfOrderBys);
		memset(scan->xs_orderbynulls, true, sizeof(bool) * scan->numberOfOrderBys);
	}

	so->killedItems = NULL;		/* until needed */
	so->numKilled = 0;
	so->curBlkno = InvalidBlockNumber;
	so->curPageLSN = InvalidXLogRecPtr;

	scan->opaque = so;

	/*
	 * All fields required for index-only scans are initialized in gistrescan,
	 * as we don't know yet if we're doing an index-only scan or not.
	 */

	MemoryContextSwitchTo(oldCxt);

	return scan;
}

void
gistrescan(IndexScanDesc scan, ScanKey key, int nkeys,
		   ScanKey orderbys, int norderbys)
{
	/* nkeys and norderbys arguments are ignored */
	GISTScanOpaque so = (GISTScanOpaque) scan->opaque;
	bool		first_time;
	int			i;
	MemoryContext oldCxt;

	/* rescan an existing indexscan --- reset state */

	/*
	 * The first time through, we create the search queue in the scanCxt.
	 * Subsequent times through, we create the queue in a separate queueCxt,
	 * which is created on the second call and reset on later calls.  Thus, in
	 * the common case where a scan is only rescan'd once, we just put the
	 * queue in scanCxt and don't pay the overhead of making a second memory
	 * context.  If we do rescan more than once, the first queue is just left
	 * for dead until end of scan; this small wastage seems worth the savings
	 * in the common case.
	 */
	if (so->queue == NULL)
	{
		/* first time through */
		Assert(so->queueCxt == so->giststate->scanCxt);
		first_time = true;
	}
	else if (so->queueCxt == so->giststate->scanCxt)
	{
		/* second time through */
		so->queueCxt = AllocSetContextCreate(so->giststate->scanCxt,
											 "GiST queue context",
											 ALLOCSET_DEFAULT_SIZES);
		first_time = false;
	}
	else
	{
		/* third or later time through */
		MemoryContextReset(so->queueCxt);
		first_time = false;
	}

	/*
	 * If we're doing an index-only scan, on the first call, also initialize a
	 * tuple descriptor to represent the returned index tuples and create a
	 * memory context to hold them during the scan.
	 */
	if (scan->xs_want_itup && !scan->xs_hitupdesc)
	{
		int			natts;
		int			nkeyatts;
		int			attno;

		/*
		 * The storage type of the index can be different from the original
		 * datatype being indexed, so we cannot just grab the index's tuple
		 * descriptor. Instead, construct a descriptor with the original data
		 * types.
		 */
		natts = RelationGetNumberOfAttributes(scan->indexRelation);
		nkeyatts = IndexRelationGetNumberOfKeyAttributes(scan->indexRelation);
		so->giststate->fetchTupdesc = CreateTemplateTupleDesc(natts);
		for (attno = 1; attno <= nkeyatts; attno++)
		{
			TupleDescInitEntry(so->giststate->fetchTupdesc, attno, NULL,
							   scan->indexRelation->rd_opcintype[attno - 1],
							   -1, 0);
		}

		for (; attno <= natts; attno++)
		{
			/* taking opcintype from giststate->tupdesc */
			TupleDescInitEntry(so->giststate->fetchTupdesc, attno, NULL,
							   TupleDescAttr(so->giststate->leafTupdesc,
											 attno - 1)->atttypid,
							   -1, 0);
		}
		scan->xs_hitupdesc = so->giststate->fetchTupdesc;

		/* Also create a memory context that will hold the returned tuples */
		so->pageDataCxt = AllocSetContextCreate(so->giststate->scanCxt,
												"GiST page data context",
												ALLOCSET_DEFAULT_SIZES);
	}

	/* create new, empty pairing heap for search queue */
	oldCxt = MemoryContextSwitchTo(so->queueCxt);
	so->queue = pairingheap_allocate(pairingheap_GISTSearchItem_cmp, scan);
	MemoryContextSwitchTo(oldCxt);

	so->firstCall = true;

	/* Update scan key, if a new one is given */
	if (key && scan->numberOfKeys > 0)
	{
		void	  **fn_extras = NULL;

		/*
		 * If this isn't the first time through, preserve the fn_extra
		 * pointers, so that if the consistentFns are using them to cache
		 * data, that data is not leaked across a rescan.
		 */
		if (!first_time)
		{
			fn_extras = (void **) palloc(scan->numberOfKeys * sizeof(void *));
			for (i = 0; i < scan->numberOfKeys; i++)
				fn_extras[i] = scan->keyData[i].sk_func.fn_extra;
		}

		memmove(scan->keyData, key,
				scan->numberOfKeys * sizeof(ScanKeyData));

		/*
		 * Modify the scan key so that the Consistent method is called for all
		 * comparisons. The original operator is passed to the Consistent
		 * function in the form of its strategy number, which is available
		 * from the sk_strategy field, and its subtype from the sk_subtype
		 * field.
		 *
		 * Next, if any of keys is a NULL and that key is not marked with
		 * SK_SEARCHNULL/SK_SEARCHNOTNULL then nothing can be found (ie, we
		 * assume all indexable operators are strict).
		 */
		so->qual_ok = true;

		for (i = 0; i < scan->numberOfKeys; i++)
		{
			ScanKey		skey = scan->keyData + i;

			/*
			 * Copy consistent support function to ScanKey structure instead
			 * of function implementing filtering operator.
			 */
			fmgr_info_copy(&(skey->sk_func),
						   &(so->giststate->consistentFn[skey->sk_attno - 1]),
						   so->giststate->scanCxt);

			/* Restore prior fn_extra pointers, if not first time */
			if (!first_time)
				skey->sk_func.fn_extra = fn_extras[i];

			if (skey->sk_flags & SK_ISNULL)
			{
				if (!(skey->sk_flags & (SK_SEARCHNULL | SK_SEARCHNOTNULL)))
					so->qual_ok = false;
			}
		}

		if (!first_time)
			pfree(fn_extras);
	}

	/* Update order-by key, if a new one is given */
	if (orderbys && scan->numberOfOrderBys > 0)
	{
		void	  **fn_extras = NULL;

		/* As above, preserve fn_extra if not first time through */
		if (!first_time)
		{
			fn_extras = (void **) palloc(scan->numberOfOrderBys * sizeof(void *));
			for (i = 0; i < scan->numberOfOrderBys; i++)
				fn_extras[i] = scan->orderByData[i].sk_func.fn_extra;
		}

		memmove(scan->orderByData, orderbys,
				scan->numberOfOrderBys * sizeof(ScanKeyData));

		so->orderByTypes = (Oid *) palloc(scan->numberOfOrderBys * sizeof(Oid));

		/*
		 * Modify the order-by key so that the Distance method is called for
		 * all comparisons. The original operator is passed to the Distance
		 * function in the form of its strategy number, which is available
		 * from the sk_strategy field, and its subtype from the sk_subtype
		 * field.
		 */
		for (i = 0; i < scan->numberOfOrderBys; i++)
		{
			ScanKey		skey = scan->orderByData + i;
			FmgrInfo   *finfo = &(so->giststate->distanceFn[skey->sk_attno - 1]);

			/* Check we actually have a distance function ... */
			if (!OidIsValid(finfo->fn_oid))
				elog(ERROR, "missing support function %d for attribute %d of index \"%s\"",
					 GIST_DISTANCE_PROC, skey->sk_attno,
					 RelationGetRelationName(scan->indexRelation));

			/*
			 * Look up the datatype returned by the original ordering
			 * operator. GiST always uses a float8 for the distance function,
			 * but the ordering operator could be anything else.
			 *
			 * XXX: The distance function is only allowed to be lossy if the
			 * ordering operator's result type is float4 or float8.  Otherwise
			 * we don't know how to return the distance to the executor.  But
			 * we cannot check that here, as we won't know if the distance
			 * function is lossy until it returns *recheck = true for the
			 * first time.
			 */
			so->orderByTypes[i] = get_func_rettype(skey->sk_func.fn_oid);

			/*
			 * Copy distance support function to ScanKey structure instead of
			 * function implementing ordering operator.
			 */
			fmgr_info_copy(&(skey->sk_func), finfo, so->giststate->scanCxt);

			/* Restore prior fn_extra pointers, if not first time */
			if (!first_time)
				skey->sk_func.fn_extra = fn_extras[i];
		}

		if (!first_time)
			pfree(fn_extras);
	}

	/* any previous xs_hitup will have been pfree'd in context resets above */
	scan->xs_hitup = NULL;
}

void
gistendscan(IndexScanDesc scan)
{
	GISTScanOpaque so = (GISTScanOpaque) scan->opaque;

	/*
	 * freeGISTstate is enough to clean up everything made by gistbeginscan,
	 * as well as the queueCxt if there is a separate context for it.
	 */
	freeGISTstate(so->giststate);
}
