/*-------------------------------------------------------------------------
 *
 * gistget.c
 *	  fetch tuples from a GiST scan.
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/gist/gistget.c,v 1.81 2009/06/11 14:48:53 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gist_private.h"
#include "access/relscan.h"
#include "executor/execdebug.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"


static OffsetNumber gistfindnext(IndexScanDesc scan, OffsetNumber n);
static int64 gistnext(IndexScanDesc scan, TIDBitmap *tbm);
static bool gistindex_keytest(IndexTuple tuple, IndexScanDesc scan,
				  OffsetNumber offset);

static void
killtuple(Relation r, GISTScanOpaque so, ItemPointer iptr)
{
	Page		p;
	OffsetNumber offset;

	LockBuffer(so->curbuf, GIST_SHARE);
	gistcheckpage(r, so->curbuf);
	p = (Page) BufferGetPage(so->curbuf);

	if (XLByteEQ(so->stack->lsn, PageGetLSN(p)))
	{
		/* page unchanged, so all is simple */
		offset = ItemPointerGetOffsetNumber(iptr);
		ItemIdMarkDead(PageGetItemId(p, offset));
		SetBufferCommitInfoNeedsSave(so->curbuf);
	}
	else
	{
		OffsetNumber maxoff = PageGetMaxOffsetNumber(p);

		for (offset = FirstOffsetNumber; offset <= maxoff; offset = OffsetNumberNext(offset))
		{
			IndexTuple	ituple = (IndexTuple) PageGetItem(p, PageGetItemId(p, offset));

			if (ItemPointerEquals(&(ituple->t_tid), iptr))
			{
				/* found */
				ItemIdMarkDead(PageGetItemId(p, offset));
				SetBufferCommitInfoNeedsSave(so->curbuf);
				break;
			}
		}
	}

	LockBuffer(so->curbuf, GIST_UNLOCK);
}

/*
 * gistgettuple() -- Get the next tuple in the scan
 */
Datum
gistgettuple(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ScanDirection dir = (ScanDirection) PG_GETARG_INT32(1);
	GISTScanOpaque so;
	bool		res;

	so = (GISTScanOpaque) scan->opaque;

	if (dir != ForwardScanDirection)
		elog(ERROR, "GiST doesn't support other scan directions than forward");

	/*
	 * If we have produced an index tuple in the past and the executor has
	 * informed us we need to mark it as "killed", do so now.
	 */
	if (scan->kill_prior_tuple && ItemPointerIsValid(&(so->curpos)))
		killtuple(scan->indexRelation, so, &(so->curpos));

	/*
	 * Get the next tuple that matches the search key.
	 */
	res = (gistnext(scan, NULL) > 0);

	PG_RETURN_BOOL(res);
}

Datum
gistgetbitmap(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	TIDBitmap  *tbm = (TIDBitmap *) PG_GETARG_POINTER(1);
	int64		ntids;

	ntids = gistnext(scan, tbm);

	PG_RETURN_INT64(ntids);
}

/*
 * Fetch tuple(s) that match the search key; this can be invoked
 * either to fetch the first such tuple or subsequent matching tuples.
 *
 * This function is used by both gistgettuple and gistgetbitmap. When
 * invoked from gistgettuple, tbm is null and the next matching tuple
 * is returned in scan->xs_ctup.t_self.  When invoked from getbitmap,
 * tbm is non-null and all matching tuples are added to tbm before
 * returning.  In both cases, the function result is the number of
 * returned tuples.
 *
 * If scan specifies to skip killed tuples, continue looping until we find a
 * non-killed tuple that matches the search key.
 */
static int64
gistnext(IndexScanDesc scan, TIDBitmap *tbm)
{
	Page		p;
	OffsetNumber n;
	GISTScanOpaque so;
	GISTSearchStack *stk;
	IndexTuple	it;
	GISTPageOpaque opaque;
	int64		ntids = 0;

	so = (GISTScanOpaque) scan->opaque;

	if (so->qual_ok == false)
		return 0;

	if (so->curbuf == InvalidBuffer)
	{
		if (ItemPointerIsValid(&so->curpos) == false)
		{
			/* Being asked to fetch the first entry, so start at the root */
			Assert(so->curbuf == InvalidBuffer);
			Assert(so->stack == NULL);

			so->curbuf = ReadBuffer(scan->indexRelation, GIST_ROOT_BLKNO);

			stk = so->stack = (GISTSearchStack *) palloc0(sizeof(GISTSearchStack));

			stk->next = NULL;
			stk->block = GIST_ROOT_BLKNO;

			pgstat_count_index_scan(scan->indexRelation);
		}
		else
		{
			/* scan is finished */
			return 0;
		}
	}

	/*
	 * check stored pointers from last visit
	 */
	if (so->nPageData > 0)
	{
		/*
		 * gistgetmulti never should go here
		 */
		Assert(tbm == NULL);

		if (so->curPageData < so->nPageData)
		{
			scan->xs_ctup.t_self = so->pageData[so->curPageData].heapPtr;
			scan->xs_recheck = so->pageData[so->curPageData].recheck;

			ItemPointerSet(&so->curpos,
						   BufferGetBlockNumber(so->curbuf),
						   so->pageData[so->curPageData].pageOffset);

			so->curPageData++;

			return 1;
		}
		else
		{
			/*
			 * Go to the next page
			 */
			stk = so->stack->next;
			pfree(so->stack);
			so->stack = stk;

			/* If we're out of stack entries, we're done */
			if (so->stack == NULL)
			{
				ReleaseBuffer(so->curbuf);
				so->curbuf = InvalidBuffer;
				return 0;
			}

			so->curbuf = ReleaseAndReadBuffer(so->curbuf,
											  scan->indexRelation,
											  stk->block);
		}
	}

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		/* First of all, we need lock buffer */
		Assert(so->curbuf != InvalidBuffer);
		LockBuffer(so->curbuf, GIST_SHARE);
		gistcheckpage(scan->indexRelation, so->curbuf);
		p = BufferGetPage(so->curbuf);
		opaque = GistPageGetOpaque(p);

		/* remember lsn to identify page changed for tuple's killing */
		so->stack->lsn = PageGetLSN(p);

		/* check page split, occured since visit to parent */
		if (!XLogRecPtrIsInvalid(so->stack->parentlsn) &&
			XLByteLT(so->stack->parentlsn, opaque->nsn) &&
			opaque->rightlink != InvalidBlockNumber /* sanity check */ &&
			(so->stack->next == NULL || so->stack->next->block != opaque->rightlink)	/* check if already
				added */ )
		{
			/* detect page split, follow right link to add pages */

			stk = (GISTSearchStack *) palloc(sizeof(GISTSearchStack));
			stk->next = so->stack->next;
			stk->block = opaque->rightlink;
			stk->parentlsn = so->stack->parentlsn;
			memset(&(stk->lsn), 0, sizeof(GistNSN));
			so->stack->next = stk;
		}

		/* if page is empty, then just skip it */
		if (PageIsEmpty(p))
		{
			LockBuffer(so->curbuf, GIST_UNLOCK);
			stk = so->stack->next;
			pfree(so->stack);
			so->stack = stk;

			if (so->stack == NULL)
			{
				ReleaseBuffer(so->curbuf);
				so->curbuf = InvalidBuffer;
				return ntids;
			}

			so->curbuf = ReleaseAndReadBuffer(so->curbuf, scan->indexRelation,
											  stk->block);
			continue;
		}

		n = FirstOffsetNumber;

		/* wonderful, we can look at page */
		so->nPageData = so->curPageData = 0;

		for (;;)
		{
			n = gistfindnext(scan, n);

			if (!OffsetNumberIsValid(n))
			{
				/*
				 * If we was called from gistgettuple and current buffer
				 * contains something matched then make a recursive call - it
				 * will return ItemPointer from so->pageData. But we save
				 * buffer pinned to support tuple's killing
				 */
				if (!tbm && so->nPageData > 0)
				{
					LockBuffer(so->curbuf, GIST_UNLOCK);
					return gistnext(scan, NULL);
				}

				/*
				 * We ran out of matching index entries on the current page,
				 * so pop the top stack entry and use it to continue the
				 * search.
				 */
				LockBuffer(so->curbuf, GIST_UNLOCK);
				stk = so->stack->next;
				pfree(so->stack);
				so->stack = stk;

				/* If we're out of stack entries, we're done */

				if (so->stack == NULL)
				{
					ReleaseBuffer(so->curbuf);
					so->curbuf = InvalidBuffer;
					return ntids;
				}

				so->curbuf = ReleaseAndReadBuffer(so->curbuf,
												  scan->indexRelation,
												  stk->block);
				/* XXX	go up */
				break;
			}

			if (GistPageIsLeaf(p))
			{
				/*
				 * We've found a matching index entry in a leaf page, so
				 * return success. Note that we keep "curbuf" pinned so that
				 * we can efficiently resume the index scan later.
				 */

				if (!(scan->ignore_killed_tuples &&
					  ItemIdIsDead(PageGetItemId(p, n))))
				{
					it = (IndexTuple) PageGetItem(p, PageGetItemId(p, n));
					ntids++;
					if (tbm != NULL)
						tbm_add_tuples(tbm, &it->t_tid, 1, scan->xs_recheck);
					else
					{
						so->pageData[so->nPageData].heapPtr = it->t_tid;
						so->pageData[so->nPageData].pageOffset = n;
						so->pageData[so->nPageData].recheck = scan->xs_recheck;
						so->nPageData++;
					}
				}
			}
			else
			{
				/*
				 * We've found an entry in an internal node whose key is
				 * consistent with the search key, so push it to stack
				 */
				stk = (GISTSearchStack *) palloc(sizeof(GISTSearchStack));

				it = (IndexTuple) PageGetItem(p, PageGetItemId(p, n));
				stk->block = ItemPointerGetBlockNumber(&(it->t_tid));
				memset(&(stk->lsn), 0, sizeof(GistNSN));
				stk->parentlsn = so->stack->lsn;

				stk->next = so->stack->next;
				so->stack->next = stk;
			}

			n = OffsetNumberNext(n);
		}
	}

	return ntids;
}

/*
 * gistindex_keytest() -- does this index tuple satisfy the scan key(s)?
 *
 * On success return for a leaf tuple, scan->xs_recheck is set to indicate
 * whether recheck is needed.  We recheck if any of the consistent() functions
 * request it.
 *
 * We must decompress the key in the IndexTuple before passing it to the
 * sk_func (and we have previously overwritten the sk_func to use the
 * user-defined Consistent method, so we actually are invoking that).
 *
 * Note that this function is always invoked in a short-lived memory context,
 * so we don't need to worry about cleaning up allocated memory, either here
 * or in the implementation of any Consistent methods.
 */
static bool
gistindex_keytest(IndexTuple tuple,
				  IndexScanDesc scan,
				  OffsetNumber offset)
{
	int			keySize = scan->numberOfKeys;
	ScanKey		key = scan->keyData;
	Relation	r = scan->indexRelation;
	GISTScanOpaque so;
	Page		p;
	GISTSTATE  *giststate;

	so = (GISTScanOpaque) scan->opaque;
	giststate = so->giststate;
	p = BufferGetPage(so->curbuf);

	IncrIndexProcessed();

	scan->xs_recheck = false;

	/*
	 * Tuple doesn't restore after crash recovery because of incomplete insert
	 */
	if (!GistPageIsLeaf(p) && GistTupleIsInvalid(tuple))
		return true;

	while (keySize > 0)
	{
		Datum		datum;
		bool		isNull;
		Datum		test;
		bool		recheck;
		GISTENTRY	de;

		datum = index_getattr(tuple,
							  key->sk_attno,
							  giststate->tupdesc,
							  &isNull);

		if (key->sk_flags & SK_ISNULL)
		{
			/*
			 * On non-leaf page we can't conclude that child hasn't NULL
			 * values because of assumption in GiST: uinon (VAL, NULL) is VAL
			 * But if on non-leaf page key IS  NULL then all childs has NULL.
			 */

			Assert(key->sk_flags & SK_SEARCHNULL);

			if (GistPageIsLeaf(p) && !isNull)
				return false;
		}
		else if (isNull)
		{
			return false;
		}
		else
		{
			gistdentryinit(giststate, key->sk_attno - 1, &de,
						   datum, r, p, offset,
						   FALSE, isNull);

			/*
			 * Call the Consistent function to evaluate the test.  The
			 * arguments are the index datum (as a GISTENTRY*), the comparison
			 * datum, the comparison operator's strategy number and subtype
			 * from pg_amop, and the recheck flag.
			 *
			 * (Presently there's no need to pass the subtype since it'll
			 * always be zero, but might as well pass it for possible future
			 * use.)
			 *
			 * We initialize the recheck flag to true (the safest assumption)
			 * in case the Consistent function forgets to set it.
			 */
			recheck = true;

			test = FunctionCall5(&key->sk_func,
								 PointerGetDatum(&de),
								 key->sk_argument,
								 Int32GetDatum(key->sk_strategy),
								 ObjectIdGetDatum(key->sk_subtype),
								 PointerGetDatum(&recheck));

			if (!DatumGetBool(test))
				return false;
			scan->xs_recheck |= recheck;
		}

		keySize--;
		key++;
	}

	return true;
}

/*
 * Return the offset of the first index entry that is consistent with
 * the search key after offset 'n' in the current page. If there are
 * no more consistent entries, return InvalidOffsetNumber.
 * On success, scan->xs_recheck is set correctly, too.
 * Page should be locked....
 */
static OffsetNumber
gistfindnext(IndexScanDesc scan, OffsetNumber n)
{
	OffsetNumber maxoff;
	IndexTuple	it;
	GISTScanOpaque so;
	MemoryContext oldcxt;
	Page		p;

	so = (GISTScanOpaque) scan->opaque;
	p = BufferGetPage(so->curbuf);
	maxoff = PageGetMaxOffsetNumber(p);

	/*
	 * Make sure we're in a short-lived memory context when we invoke a
	 * user-supplied GiST method in gistindex_keytest(), so we don't leak
	 * memory
	 */
	oldcxt = MemoryContextSwitchTo(so->tempCxt);

	while (n >= FirstOffsetNumber && n <= maxoff)
	{
		it = (IndexTuple) PageGetItem(p, PageGetItemId(p, n));
		if (gistindex_keytest(it, scan, n))
			break;

		n = OffsetNumberNext(n);
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextReset(so->tempCxt);

	/*
	 * If we found a matching entry, return its offset; otherwise return
	 * InvalidOffsetNumber to inform the caller to go to the next page.
	 */
	if (n >= FirstOffsetNumber && n <= maxoff)
		return n;
	else
		return InvalidOffsetNumber;
}
