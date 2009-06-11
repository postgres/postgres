/*-------------------------------------------------------------------------
 *
 * gistscan.c
 *	  routines to manage scans on GiST index relations
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/gist/gistscan.c,v 1.76 2009/06/11 14:48:53 momjian Exp $
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

	so = (GISTScanOpaque) scan->opaque;
	if (so != NULL)
	{
		/* rescan an existing indexscan --- reset state */
		gistfreestack(so->stack);
		so->stack = NULL;
		/* drop pins on buffers -- no locks held */
		if (BufferIsValid(so->curbuf))
		{
			ReleaseBuffer(so->curbuf);
			so->curbuf = InvalidBuffer;
		}
	}
	else
	{
		/* initialize opaque data */
		so = (GISTScanOpaque) palloc(sizeof(GISTScanOpaqueData));
		so->stack = NULL;
		so->tempCxt = createTempGistContext();
		so->curbuf = InvalidBuffer;
		so->giststate = (GISTSTATE *) palloc(sizeof(GISTSTATE));
		initGISTstate(so->giststate, scan->indexRelation);

		scan->opaque = so;
	}

	/*
	 * Clear all the pointers.
	 */
	ItemPointerSetInvalid(&so->curpos);
	so->nPageData = so->curPageData = 0;

	so->qual_ok = true;

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
		 *
		 * Next, if any of keys is a NULL and that key is not marked with
		 * SK_SEARCHNULL then nothing can be found.
		 */
		for (i = 0; i < scan->numberOfKeys; i++)
		{
			scan->keyData[i].sk_func = so->giststate->consistentFn[scan->keyData[i].sk_attno - 1];

			if (scan->keyData[i].sk_flags & SK_ISNULL)
			{
				if ((scan->keyData[i].sk_flags & SK_SEARCHNULL) == 0)
					so->qual_ok = false;
			}
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
	GISTScanOpaque so;

	so = (GISTScanOpaque) scan->opaque;

	if (so != NULL)
	{
		gistfreestack(so->stack);
		if (so->giststate != NULL)
			freeGISTstate(so->giststate);
		/* drop pins on buffers -- we aren't holding any locks */
		if (BufferIsValid(so->curbuf))
			ReleaseBuffer(so->curbuf);
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
