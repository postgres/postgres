/*-------------------------------------------------------------------------
 *
 * gistscan.c
 *	  routines to manage scans on GiST index relations
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
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
#include "utils/memutils.h"
#include "utils/rel.h"


/*
 * RBTree support functions for the GISTSearchTreeItem queue
 */

static int
GISTSearchTreeItemComparator(const RBNode *a, const RBNode *b, void *arg)
{
	const GISTSearchTreeItem *sa = (const GISTSearchTreeItem *) a;
	const GISTSearchTreeItem *sb = (const GISTSearchTreeItem *) b;
	IndexScanDesc scan = (IndexScanDesc) arg;
	int			i;

	/* Order according to distance comparison */
	for (i = 0; i < scan->numberOfOrderBys; i++)
	{
		if (sa->distances[i] != sb->distances[i])
			return (sa->distances[i] > sb->distances[i]) ? 1 : -1;
	}

	return 0;
}

static void
GISTSearchTreeItemCombiner(RBNode *existing, const RBNode *newrb, void *arg)
{
	GISTSearchTreeItem *scurrent = (GISTSearchTreeItem *) existing;
	const GISTSearchTreeItem *snew = (const GISTSearchTreeItem *) newrb;
	GISTSearchItem *newitem = snew->head;

	/* snew should have just one item in its chain */
	Assert(newitem && newitem->next == NULL);

	/*
	 * If new item is heap tuple, it goes to front of chain; otherwise insert
	 * it before the first index-page item, so that index pages are visited in
	 * LIFO order, ensuring depth-first search of index pages.	See comments
	 * in gist_private.h.
	 */
	if (GISTSearchItemIsHeap(*newitem))
	{
		newitem->next = scurrent->head;
		scurrent->head = newitem;
		if (scurrent->lastHeap == NULL)
			scurrent->lastHeap = newitem;
	}
	else if (scurrent->lastHeap == NULL)
	{
		newitem->next = scurrent->head;
		scurrent->head = newitem;
	}
	else
	{
		newitem->next = scurrent->lastHeap->next;
		scurrent->lastHeap->next = newitem;
	}
}

static RBNode *
GISTSearchTreeItemAllocator(void *arg)
{
	IndexScanDesc scan = (IndexScanDesc) arg;

	return palloc(GSTIHDRSZ + sizeof(double) * scan->numberOfOrderBys);
}

static void
GISTSearchTreeItemDeleter(RBNode *rb, void *arg)
{
	pfree(rb);
}


/*
 * Index AM API functions for scanning GiST indexes
 */

Datum
gistbeginscan(PG_FUNCTION_ARGS)
{
	Relation	r = (Relation) PG_GETARG_POINTER(0);
	int			nkeys = PG_GETARG_INT32(1);
	int			norderbys = PG_GETARG_INT32(2);
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
	so->tmpTreeItem = palloc(GSTIHDRSZ + sizeof(double) * scan->numberOfOrderBys);
	so->distances = palloc(sizeof(double) * scan->numberOfOrderBys);
	so->qual_ok = true;			/* in case there are zero keys */

	scan->opaque = so;

	MemoryContextSwitchTo(oldCxt);

	PG_RETURN_POINTER(scan);
}

Datum
gistrescan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ScanKey		key = (ScanKey) PG_GETARG_POINTER(1);
	ScanKey		orderbys = (ScanKey) PG_GETARG_POINTER(3);

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
	 * context.  If we do rescan more than once, the first RBTree is just left
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
											 ALLOCSET_DEFAULT_MINSIZE,
											 ALLOCSET_DEFAULT_INITSIZE,
											 ALLOCSET_DEFAULT_MAXSIZE);
		first_time = false;
	}
	else
	{
		/* third or later time through */
		MemoryContextReset(so->queueCxt);
		first_time = false;
	}

	/* create new, empty RBTree for search queue */
	oldCxt = MemoryContextSwitchTo(so->queueCxt);
	so->queue = rb_create(GSTIHDRSZ + sizeof(double) * scan->numberOfOrderBys,
						  GISTSearchTreeItemComparator,
						  GISTSearchTreeItemCombiner,
						  GISTSearchTreeItemAllocator,
						  GISTSearchTreeItemDeleter,
						  scan);
	MemoryContextSwitchTo(oldCxt);

	so->curTreeItem = NULL;
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

			fmgr_info_copy(&(skey->sk_func), finfo, so->giststate->scanCxt);

			/* Restore prior fn_extra pointers, if not first time */
			if (!first_time)
				skey->sk_func.fn_extra = fn_extras[i];
		}

		if (!first_time)
			pfree(fn_extras);
	}

	PG_RETURN_VOID();
}

Datum
gistmarkpos(PG_FUNCTION_ARGS)
{
	elog(ERROR, "GiST does not support mark/restore");
	PG_RETURN_VOID();
}

Datum
gistrestrpos(PG_FUNCTION_ARGS)
{
	elog(ERROR, "GiST does not support mark/restore");
	PG_RETURN_VOID();
}

Datum
gistendscan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	GISTScanOpaque so = (GISTScanOpaque) scan->opaque;

	/*
	 * freeGISTstate is enough to clean up everything made by gistbeginscan,
	 * as well as the queueCxt if there is a separate context for it.
	 */
	freeGISTstate(so->giststate);

	PG_RETURN_VOID();
}
