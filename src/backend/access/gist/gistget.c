/*-------------------------------------------------------------------------
 *
 * gistget.c
 *	  fetch tuples from a GiST scan.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/gist/gistget.c,v 1.62.2.2 2008/10/22 12:55:59 teodor Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gist_private.h"
#include "executor/execdebug.h"
#include "pgstat.h"
#include "utils/memutils.h"


static OffsetNumber gistfindnext(IndexScanDesc scan, OffsetNumber n,
			 ScanDirection dir);
static int	gistnext(IndexScanDesc scan, ScanDirection dir, ItemPointer tids, int maxtids, bool ignore_killed_tuples);
static bool gistindex_keytest(IndexTuple tuple, IndexScanDesc scan,
				  OffsetNumber offset);

static void
killtuple(Relation r, GISTScanOpaque so, ItemPointer iptr)
{
	Page        p;
	OffsetNumber offset;
 
	LockBuffer(so->curbuf, GIST_SHARE);
	gistcheckpage(r, so->curbuf);
	p = (Page) BufferGetPage(so->curbuf);

	if (XLByteEQ(so->stack->lsn, PageGetLSN(p)))
	{
		/* page unchanged, so all is simple */
		offset = ItemPointerGetOffsetNumber(iptr);
		PageGetItemId(p, offset)->lp_flags |= LP_DELETE;
		SetBufferCommitInfoNeedsSave(so->curbuf);
	}
	else
	{
		OffsetNumber maxoff = PageGetMaxOffsetNumber(p);

		for (offset = FirstOffsetNumber; offset <= maxoff; offset = OffsetNumberNext(offset))
		{
			IndexTuple  ituple = (IndexTuple) PageGetItem(p, PageGetItemId(p, offset));

			if (ItemPointerEquals(&(ituple->t_tid), iptr))
			{
				/* found */
				PageGetItemId(p, offset)->lp_flags |= LP_DELETE;
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
	ItemPointerData tid;
	bool		res;

	so = (GISTScanOpaque) scan->opaque;

	/*
	 * If we have produced an index tuple in the past and the executor has
	 * informed us we need to mark it as "killed", do so now.
	 */
	if (scan->kill_prior_tuple && ItemPointerIsValid(&(scan->currentItemData)))
		killtuple(scan->indexRelation, so, &(scan->currentItemData));

	/*
	 * Get the next tuple that matches the search key. If asked to skip killed
	 * tuples, continue looping until we find a non-killed tuple that matches
	 * the search key.
	 */
	res = (gistnext(scan, dir, &tid, 1, scan->ignore_killed_tuples)) ? true : false;

	PG_RETURN_BOOL(res);
}

Datum
gistgetmulti(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ItemPointer tids = (ItemPointer) PG_GETARG_POINTER(1);
	int32		max_tids = PG_GETARG_INT32(2);
	int32	   *returned_tids = (int32 *) PG_GETARG_POINTER(3);

	*returned_tids = gistnext(scan, ForwardScanDirection, tids, max_tids, false);

	PG_RETURN_BOOL(*returned_tids == max_tids);
}

/*
 * Fetch a tuples that matchs the search key; this can be invoked
 * either to fetch the first such tuple or subsequent matching
 * tuples. Returns true iff a matching tuple was found.
 */
static int
gistnext(IndexScanDesc scan, ScanDirection dir, ItemPointer tids, int maxtids, bool ignore_killed_tuples)
{
	Page		p;
	OffsetNumber n;
	GISTScanOpaque so;
	GISTSearchStack *stk;
	IndexTuple	it;
	GISTPageOpaque opaque;
	int			ntids = 0;

	so = (GISTScanOpaque) scan->opaque;

	if (ItemPointerIsValid(&scan->currentItemData) == false)
	{
		/* Being asked to fetch the first entry, so start at the root */
		Assert(so->curbuf == InvalidBuffer);
		Assert(so->stack == NULL);

		so->curbuf = ReadBuffer(scan->indexRelation, GIST_ROOT_BLKNO);

		stk = so->stack = (GISTSearchStack *) palloc0(sizeof(GISTSearchStack));

		stk->next = NULL;
		stk->block = GIST_ROOT_BLKNO;

		pgstat_count_index_scan(&scan->xs_pgstat_info);
	}
	else if (so->curbuf == InvalidBuffer)
	{
		return 0;
	}

	/*
	 * check stored pointers from last visit 
	 */
	if ( so->nPageData > 0 ) 
	{
		while( ntids < maxtids && so->curPageData < so->nPageData )
		{
			tids[ ntids ] = scan->xs_ctup.t_self = so->pageData[ so->curPageData ].heapPtr;
			ItemPointerSet(&scan->currentItemData,
							   BufferGetBlockNumber(so->curbuf), 
							   so->pageData[ so->curPageData ].pageOffset);

				
			so->curPageData ++;
			ntids++;
		}

		if ( ntids == maxtids )
			return ntids;
		
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
			return ntids;
		}

		so->curbuf = ReleaseAndReadBuffer(so->curbuf,
										  scan->indexRelation,
										  stk->block);
	}

	for (;;)
	{
		/* First of all, we need lock buffer */
		Assert(so->curbuf != InvalidBuffer);
		LockBuffer(so->curbuf, GIST_SHARE);
		gistcheckpage(scan->indexRelation, so->curbuf);
		p = BufferGetPage(so->curbuf);
		opaque = GistPageGetOpaque(p);

		/* remember lsn to identify page changed for tuple's killing */
		so->stack->lsn = PageGetLSN(p);

		/* check page split, occured from last visit or visit to parent */
		if (!XLogRecPtrIsInvalid(so->stack->parentlsn) &&
			XLByteLT(so->stack->parentlsn, opaque->nsn) &&
			opaque->rightlink != InvalidBlockNumber /* sanity check */ &&
			(so->stack->next == NULL || so->stack->next->block != opaque->rightlink)		/* check if already
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

		if (ScanDirectionIsBackward(dir))
			n = PageGetMaxOffsetNumber(p);
		else
			n = FirstOffsetNumber;

		/* wonderful, we can look at page */
		so->nPageData = so->curPageData = 0;

		for (;;)
		{
			n = gistfindnext(scan, n, dir);

			if (!OffsetNumberIsValid(n))
			{
				while( ntids < maxtids && so->curPageData < so->nPageData )
				{
					tids[ ntids ] = scan->xs_ctup.t_self = 
						so->pageData[ so->curPageData ].heapPtr;
				
					ItemPointerSet(&scan->currentItemData,
								   BufferGetBlockNumber(so->curbuf), 
								   so->pageData[ so->curPageData ].pageOffset);

					so->curPageData ++;
					ntids++;
				}

				if ( ntids == maxtids )
				{
					LockBuffer(so->curbuf, GIST_UNLOCK);
					return ntids;
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

				if (!(ignore_killed_tuples && ItemIdDeleted(PageGetItemId(p, n))))
				{
					it = (IndexTuple) PageGetItem(p, PageGetItemId(p, n));
					so->pageData[ so->nPageData ].heapPtr = it->t_tid;
					so->pageData[ so->nPageData ].pageOffset = n;
					so->nPageData ++;
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

			if (ScanDirectionIsBackward(dir))
				n = OffsetNumberPrev(n);
			else
				n = OffsetNumberNext(n);
		}
	}

	return ntids;
}

/*
 * gistindex_keytest() -- does this index tuple satisfy the scan key(s)?
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
		GISTENTRY	de;

		datum = index_getattr(tuple,
							  key->sk_attno,
							  giststate->tupdesc,
							  &isNull);

		if (key->sk_flags & SK_ISNULL)
		{
			/*
			 * is the compared-to datum NULL? on non-leaf page it's possible
			 * to have nulls in childs :(
			 */

			if (isNull || !GistPageIsLeaf(p))
				return true;
			return false;
		}
		else if (isNull)
			return false;

		gistdentryinit(giststate, key->sk_attno - 1, &de,
					   datum, r, p, offset,
					   FALSE, isNull);

		/*
		 * Call the Consistent function to evaluate the test.  The arguments
		 * are the index datum (as a GISTENTRY*), the comparison datum, and
		 * the comparison operator's strategy number and subtype from pg_amop.
		 *
		 * (Presently there's no need to pass the subtype since it'll always
		 * be zero, but might as well pass it for possible future use.)
		 */
		test = FunctionCall4(&key->sk_func,
							 PointerGetDatum(&de),
							 key->sk_argument,
							 Int32GetDatum(key->sk_strategy),
							 ObjectIdGetDatum(key->sk_subtype));

		if (!DatumGetBool(test))
			return false;

		keySize--;
		key++;
	}

	return true;
}

/*
 * Return the offset of the first index entry that is consistent with
 * the search key after offset 'n' in the current page. If there are
 * no more consistent entries, return InvalidOffsetNumber.
 * Page should be locked....
 */
static OffsetNumber
gistfindnext(IndexScanDesc scan, OffsetNumber n, ScanDirection dir)
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

	/*
	 * If we modified the index during the scan, we may have a pointer to a
	 * ghost tuple, before the scan.  If this is the case, back up one.
	 */
	if (so->flags & GS_CURBEFORE)
	{
		so->flags &= ~GS_CURBEFORE;
		n = OffsetNumberPrev(n);
	}

	while (n >= FirstOffsetNumber && n <= maxoff)
	{
		it = (IndexTuple) PageGetItem(p, PageGetItemId(p, n));
		if (gistindex_keytest(it, scan, n))
			break;

		if (ScanDirectionIsBackward(dir))
			n = OffsetNumberPrev(n);
		else
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
