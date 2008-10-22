/*-------------------------------------------------------------------------
 *
 * gistscan.c
 *	  routines to manage scans on GiST index relations
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/gist/gistscan.c,v 1.65.2.2 2008/10/22 12:55:59 teodor Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/gist_private.h"
#include "access/gistscan.h"
#include "utils/memutils.h"

static void gistfreestack(GISTSearchStack *s);

Datum
gistbeginscan(PG_FUNCTION_ARGS)
{
	Relation	r = (Relation) PG_GETARG_POINTER(0);
	int			nkeys = PG_GETARG_INT32(1);
	ScanKey		key = (ScanKey) PG_GETARG_POINTER(2);
	IndexScanDesc scan;

	scan = RelationGetIndexScan(r, nkeys, key);

	PG_RETURN_POINTER(scan);
}

Datum
gistrescan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ScanKey		key = (ScanKey) PG_GETARG_POINTER(1);
	GISTScanOpaque so;
	int			i;

	/*
	 * Clear all the pointers.
	 */
	ItemPointerSetInvalid(&scan->currentItemData);
	ItemPointerSetInvalid(&scan->currentMarkData);

	so = (GISTScanOpaque) scan->opaque;
	if (so != NULL)
	{
		/* rescan an existing indexscan --- reset state */
		gistfreestack(so->stack);
		gistfreestack(so->markstk);
		so->stack = so->markstk = NULL;
		so->flags = 0x0;
		/* drop pins on buffers -- no locks held */
		if (BufferIsValid(so->curbuf))
		{
			ReleaseBuffer(so->curbuf);
			so->curbuf = InvalidBuffer;
		}
		if (BufferIsValid(so->markbuf))
		{
			ReleaseBuffer(so->markbuf);
			so->markbuf = InvalidBuffer;
		}

	}
	else
	{
		/* initialize opaque data */
		so = (GISTScanOpaque) palloc(sizeof(GISTScanOpaqueData));
		so->stack = so->markstk = NULL;
		so->flags = 0x0;
		so->tempCxt = createTempGistContext();
		so->curbuf = so->markbuf = InvalidBuffer;
		so->giststate = (GISTSTATE *) palloc(sizeof(GISTSTATE));
		initGISTstate(so->giststate, scan->indexRelation);

		scan->opaque = so;
	}

	so->nPageData = so->curPageData = 0;

	/* Update scan key, if a new one is given */
	if (key && scan->numberOfKeys > 0)
	{
		memmove(scan->keyData, key,
				scan->numberOfKeys * sizeof(ScanKeyData));

		/*
		 * Modify the scan key so that all the Consistent method is called for
		 * all comparisons. The original operator is passed to the Consistent
		 * function in the form of its strategy number, which is available
		 * from the sk_strategy field, and its subtype from the sk_subtype
		 * field.
		 */
		for (i = 0; i < scan->numberOfKeys; i++)
			scan->keyData[i].sk_func = so->giststate->consistentFn[scan->keyData[i].sk_attno - 1];
	}

	PG_RETURN_VOID();
}

Datum
gistmarkpos(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	GISTScanOpaque so;
	GISTSearchStack *o,
			   *n,
			   *tmp;

	scan->currentMarkData = scan->currentItemData;
	so = (GISTScanOpaque) scan->opaque;
	if (so->flags & GS_CURBEFORE)
		so->flags |= GS_MRKBEFORE;
	else
		so->flags &= ~GS_MRKBEFORE;

	o = NULL;
	n = so->stack;

	/* copy the parent stack from the current item data */
	while (n != NULL)
	{
		tmp = (GISTSearchStack *) palloc(sizeof(GISTSearchStack));
		tmp->lsn = n->lsn;
		tmp->parentlsn = n->parentlsn;
		tmp->block = n->block;
		tmp->next = o;
		o = tmp;
		n = n->next;
	}

	gistfreestack(so->markstk);
	so->markstk = o;

	/* Update markbuf: make sure to bump ref count on curbuf */
	if (BufferIsValid(so->markbuf))
	{
		ReleaseBuffer(so->markbuf);
		so->markbuf = InvalidBuffer;
	}
	if (BufferIsValid(so->curbuf))
	{
		IncrBufferRefCount(so->curbuf);
		so->markbuf = so->curbuf;
	}

	so->markNPageData = so->nPageData;
	so->markCurPageData = so->curPageData;
	if ( so->markNPageData > 0 )
		memcpy( so->markPageData, so->pageData, sizeof(MatchedItemPtr) * so->markNPageData );		

	PG_RETURN_VOID();
}

Datum
gistrestrpos(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	GISTScanOpaque so;
	GISTSearchStack *o,
			   *n,
			   *tmp;

	scan->currentItemData = scan->currentMarkData;
	so = (GISTScanOpaque) scan->opaque;
	if (so->flags & GS_MRKBEFORE)
		so->flags |= GS_CURBEFORE;
	else
		so->flags &= ~GS_CURBEFORE;

	o = NULL;
	n = so->markstk;

	/* copy the parent stack from the current item data */
	while (n != NULL)
	{
		tmp = (GISTSearchStack *) palloc(sizeof(GISTSearchStack));
		tmp->lsn = n->lsn;
		tmp->parentlsn = n->parentlsn;
		tmp->block = n->block;
		tmp->next = o;
		o = tmp;
		n = n->next;
	}

	gistfreestack(so->stack);
	so->stack = o;

	/* Update curbuf: be sure to bump ref count on markbuf */
	if (BufferIsValid(so->curbuf))
	{
		ReleaseBuffer(so->curbuf);
		so->curbuf = InvalidBuffer;
	}
	if (BufferIsValid(so->markbuf))
	{
		IncrBufferRefCount(so->markbuf);
		so->curbuf = so->markbuf;
	}

	so->nPageData = so->markNPageData;
	so->curPageData = so->markNPageData;
	if ( so->markNPageData > 0 )
		memcpy( so->pageData, so->markPageData, sizeof(MatchedItemPtr) * so->markNPageData );		

	PG_RETURN_VOID();
}

Datum
gistendscan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	GISTScanOpaque so;

	so = (GISTScanOpaque) scan->opaque;

	if (so != NULL)
	{
		gistfreestack(so->stack);
		gistfreestack(so->markstk);
		if (so->giststate != NULL)
			freeGISTstate(so->giststate);
		/* drop pins on buffers -- we aren't holding any locks */
		if (BufferIsValid(so->curbuf))
			ReleaseBuffer(so->curbuf);
		if (BufferIsValid(so->markbuf))
			ReleaseBuffer(so->markbuf);
		MemoryContextDelete(so->tempCxt);
		pfree(scan->opaque);
	}

	PG_RETURN_VOID();
}

static void
gistfreestack(GISTSearchStack *s)
{
	while (s != NULL)
	{
		GISTSearchStack *p = s->next;

		pfree(s);
		s = p;
	}
}
