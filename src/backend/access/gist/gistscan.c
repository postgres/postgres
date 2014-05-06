/*-------------------------------------------------------------------------
 *
 * gistscan.c
 *	  routines to manage scans on GiST index relations
 *
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/gist/gistscan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/gist_private.h"
#include "access/gistscan.h"
#include "access/relscan.h"
#include "storage/bufmgr.h"
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
	 * LIFO order, ensuring depth-first search of index pages.  See comments
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
	GISTScanOpaque so;

	scan = RelationGetIndexScan(r, nkeys, norderbys);

	/* initialize opaque data */
	so = (GISTScanOpaque) palloc0(sizeof(GISTScanOpaqueData));
	so->queueCxt = AllocSetContextCreate(CurrentMemoryContext,
										 "GiST queue context",
										 ALLOCSET_DEFAULT_MINSIZE,
										 ALLOCSET_DEFAULT_INITSIZE,
										 ALLOCSET_DEFAULT_MAXSIZE);
	so->tempCxt = createTempGistContext();
	so->giststate = (GISTSTATE *) palloc(sizeof(GISTSTATE));
	initGISTstate(so->giststate, scan->indexRelation);
	/* workspaces with size dependent on numberOfOrderBys: */
	so->tmpTreeItem = palloc(GSTIHDRSZ + sizeof(double) * scan->numberOfOrderBys);
	so->distances = palloc(sizeof(double) * scan->numberOfOrderBys);
	so->qual_ok = true;			/* in case there are zero keys */

	scan->opaque = so;

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
	int			i;
	MemoryContext oldCxt;

	/* rescan an existing indexscan --- reset state */
	MemoryContextReset(so->queueCxt);
	so->curTreeItem = NULL;

	/* create new, empty RBTree for search queue */
	oldCxt = MemoryContextSwitchTo(so->queueCxt);
	so->queue = rb_create(GSTIHDRSZ + sizeof(double) * scan->numberOfOrderBys,
						  GISTSearchTreeItemComparator,
						  GISTSearchTreeItemCombiner,
						  GISTSearchTreeItemAllocator,
						  GISTSearchTreeItemDeleter,
						  scan);
	MemoryContextSwitchTo(oldCxt);

	so->firstCall = true;

	/* Update scan key, if a new one is given */
	if (key && scan->numberOfKeys > 0)
	{
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

			skey->sk_func = so->giststate->consistentFn[skey->sk_attno - 1];

			if (skey->sk_flags & SK_ISNULL)
			{
				if (!(skey->sk_flags & (SK_SEARCHNULL | SK_SEARCHNOTNULL)))
					so->qual_ok = false;
			}
		}
	}

	/* Update order-by key, if a new one is given */
	if (orderbys && scan->numberOfOrderBys > 0)
	{
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

			skey->sk_func = so->giststate->distanceFn[skey->sk_attno - 1];

			/* Check we actually have a distance function ... */
			if (!OidIsValid(skey->sk_func.fn_oid))
				elog(ERROR, "missing support function %d for attribute %d of index \"%s\"",
					 GIST_DISTANCE_PROC, skey->sk_attno,
					 RelationGetRelationName(scan->indexRelation));
		}
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

	freeGISTstate(so->giststate);
	pfree(so->giststate);
	MemoryContextDelete(so->queueCxt);
	MemoryContextDelete(so->tempCxt);
	pfree(so->tmpTreeItem);
	pfree(so->distances);
	pfree(so);

	PG_RETURN_VOID();
}
