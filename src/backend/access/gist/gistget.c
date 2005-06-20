/*-------------------------------------------------------------------------
 *
 * gistget.c
 *	  fetch tuples from a GiST scan.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/gist/gistget.c,v 1.49 2005/06/20 10:29:36 teodor Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/itup.h"
#include "access/gist_private.h"
#include "executor/execdebug.h"
#include "utils/memutils.h"

static OffsetNumber gistfindnext(IndexScanDesc scan, OffsetNumber n,
								 ScanDirection dir);
static bool gistnext(IndexScanDesc scan, ScanDirection dir);
static bool gistindex_keytest(IndexTuple tuple, IndexScanDesc scan,
							  OffsetNumber offset);


/*
 * gistgettuple() -- Get the next tuple in the scan
 */
Datum
gistgettuple(PG_FUNCTION_ARGS)
{
	IndexScanDesc	scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ScanDirection	dir = (ScanDirection) PG_GETARG_INT32(1);
	Page			page;
	OffsetNumber	offnum;
	GISTScanOpaque	so;

	so = (GISTScanOpaque) scan->opaque;

	/*
	 * If we have produced an index tuple in the past and the executor
	 * has informed us we need to mark it as "killed", do so now.
	 *
	 * XXX: right now there is no concurrent access. In the
	 * future, we should (a) get a read lock on the page (b) check
	 * that the location of the previously-fetched tuple hasn't
	 * changed due to concurrent insertions.
	 */
	if (scan->kill_prior_tuple && ItemPointerIsValid(&(scan->currentItemData)))
	{
		offnum = ItemPointerGetOffsetNumber(&(scan->currentItemData));
		page = BufferGetPage(so->curbuf);
		PageGetItemId(page, offnum)->lp_flags |= LP_DELETE;
		SetBufferCommitInfoNeedsSave(so->curbuf);
	}

	/*
	 * Get the next tuple that matches the search key. If asked to
	 * skip killed tuples, continue looping until we find a non-killed
	 * tuple that matches the search key.
	 */
	for (;;)
	{
		bool res = gistnext(scan, dir);

		if (res == true && scan->ignore_killed_tuples)
		{
			offnum = ItemPointerGetOffsetNumber(&(scan->currentItemData));
			page = BufferGetPage(so->curbuf);
			if (ItemIdDeleted(PageGetItemId(page, offnum)))
				continue;
		}

		PG_RETURN_BOOL(res);
	}
}

Datum
gistgetmulti(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ItemPointer	tids = (ItemPointer) PG_GETARG_POINTER(1);
	int32		max_tids = PG_GETARG_INT32(2);
	int32	   *returned_tids = (int32 *) PG_GETARG_POINTER(3);
	bool		res = true;
	int32		ntids = 0;

	/* XXX generic implementation: loop around guts of gistgettuple */
	while (ntids < max_tids)
	{
		res = gistnext(scan, ForwardScanDirection);
		if (!res)
			break;
		tids[ntids] = scan->xs_ctup.t_self;
		ntids++;
	}

	*returned_tids = ntids;
	PG_RETURN_BOOL(res);
}

/*
 * Fetch a tuple that matchs the search key; this can be invoked
 * either to fetch the first such tuple or subsequent matching
 * tuples. Returns true iff a matching tuple was found.
 */
static bool
gistnext(IndexScanDesc scan, ScanDirection dir)
{
	Page		p;
	OffsetNumber n;
	GISTScanOpaque so;
	GISTSTACK  *stk;
	IndexTuple	it;

	so = (GISTScanOpaque) scan->opaque;

	if (ItemPointerIsValid(&scan->currentItemData) == false)
	{
		/* Being asked to fetch the first entry, so start at the root */
		Assert(so->curbuf == InvalidBuffer);
		so->curbuf = ReadBuffer(scan->indexRelation, GIST_ROOT_BLKNO);
	}

	p = BufferGetPage(so->curbuf);

	if (ItemPointerIsValid(&scan->currentItemData) == false)
	{
		if (ScanDirectionIsBackward(dir))
			n = PageGetMaxOffsetNumber(p);
		else
			n = FirstOffsetNumber;
	}
	else
	{
		n = ItemPointerGetOffsetNumber(&(scan->currentItemData));

		if (ScanDirectionIsBackward(dir))
			n = OffsetNumberPrev(n);
		else
			n = OffsetNumberNext(n);
	}

	for (;;)
	{
		n = gistfindnext(scan, n, dir);

		if (!OffsetNumberIsValid(n))
		{
			/*
			 * We ran out of matching index entries on the current
			 * page, so pop the top stack entry and use it to continue
			 * the search.
			 */
			/* If we're out of stack entries, we're done */
			if (so->stack == NULL)
			{
				ReleaseBuffer(so->curbuf);
				so->curbuf = InvalidBuffer;
				return false;
			}

			stk = so->stack;
			so->curbuf = ReleaseAndReadBuffer(so->curbuf, scan->indexRelation,
											  stk->block);
			p = BufferGetPage(so->curbuf);

			if (ScanDirectionIsBackward(dir))
				n = OffsetNumberPrev(stk->offset);
			else
				n = OffsetNumberNext(stk->offset);

			so->stack = stk->parent;
			pfree(stk);

			continue;
		}

		if (GistPageIsLeaf(p))
		{
			/*
			 * We've found a matching index entry in a leaf page, so
			 * return success. Note that we keep "curbuf" pinned so
			 * that we can efficiently resume the index scan later.
			 */
			ItemPointerSet(&(scan->currentItemData),
						   BufferGetBlockNumber(so->curbuf), n);

			it = (IndexTuple) PageGetItem(p, PageGetItemId(p, n));
			scan->xs_ctup.t_self = it->t_tid;
			return true;
		}
		else
		{
			/*
			 * We've found an entry in an internal node whose key is
			 * consistent with the search key, so continue the search
			 * in the pointed-to child node (i.e. we search depth
			 * first). Push the current node onto the stack so we
			 * resume searching from this node later.
			 */
			BlockNumber child_block;

			stk = (GISTSTACK *) palloc(sizeof(GISTSTACK));
			stk->offset = n;
			stk->block = BufferGetBlockNumber(so->curbuf);
			stk->parent = so->stack;
			so->stack = stk;

			it = (IndexTuple) PageGetItem(p, PageGetItemId(p, n));
			child_block = ItemPointerGetBlockNumber(&(it->t_tid));

			so->curbuf = ReleaseAndReadBuffer(so->curbuf, scan->indexRelation,
											  child_block);
			p = BufferGetPage(so->curbuf);

			if (ScanDirectionIsBackward(dir))
				n = PageGetMaxOffsetNumber(p);
			else
				n = FirstOffsetNumber;
		}
	}
}

/*
 * Similar to index_keytest, but first decompress the key in the
 * IndexTuple before passing it to the sk_func (and we have previously
 * overwritten the sk_func to use the user-defined Consistent method,
 * so we actually invoke that). Note that this function is always
 * invoked in a short-lived memory context, so we don't need to worry
 * about cleaning up allocated memory (either here or in the
 * implementation of any Consistent methods).
 */
static bool
gistindex_keytest(IndexTuple tuple,
				  IndexScanDesc scan,
				  OffsetNumber offset)
{
	int keySize = scan->numberOfKeys;
	ScanKey key = scan->keyData;
	Relation r = scan->indexRelation;
	GISTScanOpaque so;
	Page p;
	GISTSTATE *giststate;

	so = (GISTScanOpaque) scan->opaque;
	giststate = so->giststate;
	p = BufferGetPage(so->curbuf);

	IncrIndexProcessed();

	/*
         * Tuple doesn't restore after crash recovery because of inclomplete insert 
         */
	if ( !GistPageIsLeaf(p) && GistTupleIsInvalid(tuple) ) 
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
		/* is the index entry NULL? */
		if (isNull)
		{
			/* XXX eventually should check if SK_ISNULL */
			return false;
		}
		/* is the compared-to datum NULL? */
		if (key->sk_flags & SK_ISNULL)
			return false;

		gistdentryinit(giststate, key->sk_attno - 1, &de,
					   datum, r, p, offset,
					   IndexTupleSize(tuple) - sizeof(IndexTupleData),
					   FALSE, isNull);

		/*
		 * Call the Consistent function to evaluate the test.  The
		 * arguments are the index datum (as a GISTENTRY*), the comparison
		 * datum, and the comparison operator's strategy number and
		 * subtype from pg_amop.
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
 */
static OffsetNumber
gistfindnext(IndexScanDesc scan, OffsetNumber n, ScanDirection dir)
{
	OffsetNumber	maxoff;
	IndexTuple		it;
	GISTScanOpaque	so;
	MemoryContext	oldcxt;
	Page			p;

	so = (GISTScanOpaque) scan->opaque;
	p = BufferGetPage(so->curbuf);
	maxoff = PageGetMaxOffsetNumber(p);

	/*
	 * Make sure we're in a short-lived memory context when we invoke
	 * a user-supplied GiST method in gistindex_keytest(), so we don't
	 * leak memory
	 */
	oldcxt = MemoryContextSwitchTo(so->tempCxt);

	/*
	 * If we modified the index during the scan, we may have a pointer to
	 * a ghost tuple, before the scan.	If this is the case, back up one.
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
	 * If we found a matching entry, return its offset; otherwise
	 * return InvalidOffsetNumber to inform the caller to go to the
	 * next page.
	 */
	if (n >= FirstOffsetNumber && n <= maxoff)
		return n;
	else
		return InvalidOffsetNumber;
}
