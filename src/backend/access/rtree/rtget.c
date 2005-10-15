/*-------------------------------------------------------------------------
 *
 * rtget.c
 *	  fetch tuples from an rtree scan.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/rtree/rtget.c,v 1.37 2005/10/15 02:49:09 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/iqual.h"
#include "access/relscan.h"
#include "access/rtree.h"
#include "pgstat.h"


static OffsetNumber findnext(IndexScanDesc s, OffsetNumber n,
		 ScanDirection dir);
static bool rtnext(IndexScanDesc s, ScanDirection dir);


Datum
rtgettuple(PG_FUNCTION_ARGS)
{
	IndexScanDesc s = (IndexScanDesc) PG_GETARG_POINTER(0);
	ScanDirection dir = (ScanDirection) PG_GETARG_INT32(1);
	RTreeScanOpaque so = (RTreeScanOpaque) s->opaque;
	Page		page;
	OffsetNumber offnum;

	/*
	 * If we've already produced a tuple and the executor has informed us that
	 * it should be marked "killed", do so now.
	 */
	if (s->kill_prior_tuple && ItemPointerIsValid(&(s->currentItemData)))
	{
		offnum = ItemPointerGetOffsetNumber(&(s->currentItemData));
		page = BufferGetPage(so->curbuf);
		PageGetItemId(page, offnum)->lp_flags |= LP_DELETE;
		SetBufferCommitInfoNeedsSave(so->curbuf);
	}

	/*
	 * Get the next tuple that matches the search key; if asked to skip killed
	 * tuples, find the first non-killed tuple that matches. Return as soon as
	 * we've run out of matches or we've found an acceptable match.
	 */
	for (;;)
	{
		bool		res = rtnext(s, dir);

		if (res && s->ignore_killed_tuples)
		{
			offnum = ItemPointerGetOffsetNumber(&(s->currentItemData));
			page = BufferGetPage(so->curbuf);
			if (ItemIdDeleted(PageGetItemId(page, offnum)))
				continue;
		}

		PG_RETURN_BOOL(res);
	}
}

Datum
rtgetmulti(PG_FUNCTION_ARGS)
{
	IndexScanDesc s = (IndexScanDesc) PG_GETARG_POINTER(0);
	ItemPointer tids = (ItemPointer) PG_GETARG_POINTER(1);
	int32		max_tids = PG_GETARG_INT32(2);
	int32	   *returned_tids = (int32 *) PG_GETARG_POINTER(3);
	RTreeScanOpaque so = (RTreeScanOpaque) s->opaque;
	bool		res = true;
	int32		ntids = 0;

	/* XXX generic implementation: loop around guts of rtgettuple */
	while (ntids < max_tids)
	{
		res = rtnext(s, ForwardScanDirection);
		if (res && s->ignore_killed_tuples)
		{
			Page		page;
			OffsetNumber offnum;

			offnum = ItemPointerGetOffsetNumber(&(s->currentItemData));
			page = BufferGetPage(so->curbuf);
			if (ItemIdDeleted(PageGetItemId(page, offnum)))
				continue;
		}

		if (!res)
			break;
		tids[ntids] = s->xs_ctup.t_self;
		ntids++;
	}

	*returned_tids = ntids;
	PG_RETURN_BOOL(res);
}

static bool
rtnext(IndexScanDesc s, ScanDirection dir)
{
	Page		p;
	OffsetNumber n;
	RTreePageOpaque po;
	RTreeScanOpaque so;

	so = (RTreeScanOpaque) s->opaque;

	if (!ItemPointerIsValid(&(s->currentItemData)))
	{
		/* first call: start at the root */
		Assert(BufferIsValid(so->curbuf) == false);
		so->curbuf = ReadBuffer(s->indexRelation, P_ROOT);
		pgstat_count_index_scan(&s->xs_pgstat_info);
	}

	p = BufferGetPage(so->curbuf);
	po = (RTreePageOpaque) PageGetSpecialPointer(p);

	if (!ItemPointerIsValid(&(s->currentItemData)))
	{
		/* first call: start at first/last offset */
		if (ScanDirectionIsForward(dir))
			n = FirstOffsetNumber;
		else
			n = PageGetMaxOffsetNumber(p);
	}
	else
	{
		/* go on to the next offset */
		n = ItemPointerGetOffsetNumber(&(s->currentItemData));
		if (ScanDirectionIsForward(dir))
			n = OffsetNumberNext(n);
		else
			n = OffsetNumberPrev(n);
	}

	for (;;)
	{
		IndexTuple	it;
		RTSTACK    *stk;

		n = findnext(s, n, dir);

		/* no match on this page, so read in the next stack entry */
		if (n == InvalidOffsetNumber)
		{
			/* if out of stack entries, we're done */
			if (so->s_stack == NULL)
			{
				ReleaseBuffer(so->curbuf);
				so->curbuf = InvalidBuffer;
				return false;
			}

			stk = so->s_stack;
			so->curbuf = ReleaseAndReadBuffer(so->curbuf, s->indexRelation,
											  stk->rts_blk);
			p = BufferGetPage(so->curbuf);
			po = (RTreePageOpaque) PageGetSpecialPointer(p);

			if (ScanDirectionIsBackward(dir))
				n = OffsetNumberPrev(stk->rts_child);
			else
				n = OffsetNumberNext(stk->rts_child);
			so->s_stack = stk->rts_parent;
			pfree(stk);

			continue;
		}

		if (po->flags & F_LEAF)
		{
			ItemPointerSet(&(s->currentItemData),
						   BufferGetBlockNumber(so->curbuf),
						   n);
			it = (IndexTuple) PageGetItem(p, PageGetItemId(p, n));
			s->xs_ctup.t_self = it->t_tid;
			return true;
		}
		else
		{
			BlockNumber blk;

			stk = (RTSTACK *) palloc(sizeof(RTSTACK));
			stk->rts_child = n;
			stk->rts_blk = BufferGetBlockNumber(so->curbuf);
			stk->rts_parent = so->s_stack;
			so->s_stack = stk;

			it = (IndexTuple) PageGetItem(p, PageGetItemId(p, n));
			blk = ItemPointerGetBlockNumber(&(it->t_tid));

			/*
			 * Note that we release the pin on the page as we descend down the
			 * tree, even though there's a good chance we'll eventually need
			 * to re-read the buffer later in this scan. This may or may not
			 * be optimal, but it doesn't seem likely to make a huge
			 * performance difference either way.
			 */
			so->curbuf = ReleaseAndReadBuffer(so->curbuf, s->indexRelation, blk);
			p = BufferGetPage(so->curbuf);
			po = (RTreePageOpaque) PageGetSpecialPointer(p);

			if (ScanDirectionIsBackward(dir))
				n = PageGetMaxOffsetNumber(p);
			else
				n = FirstOffsetNumber;
		}
	}
}

/*
 * Return the offset of the next matching index entry. We begin the
 * search at offset "n" and search for matches in the direction
 * "dir". If no more matching entries are found on the page,
 * InvalidOffsetNumber is returned.
 */
static OffsetNumber
findnext(IndexScanDesc s, OffsetNumber n, ScanDirection dir)
{
	OffsetNumber maxoff;
	IndexTuple	it;
	RTreePageOpaque po;
	RTreeScanOpaque so;
	Page		p;

	so = (RTreeScanOpaque) s->opaque;
	p = BufferGetPage(so->curbuf);

	maxoff = PageGetMaxOffsetNumber(p);
	po = (RTreePageOpaque) PageGetSpecialPointer(p);

	/*
	 * If we modified the index during the scan, we may have a pointer to a
	 * ghost tuple, before the scan.  If this is the case, back up one.
	 */

	if (so->s_flags & RTS_CURBEFORE)
	{
		so->s_flags &= ~RTS_CURBEFORE;
		n = OffsetNumberPrev(n);
	}

	while (n >= FirstOffsetNumber && n <= maxoff)
	{
		it = (IndexTuple) PageGetItem(p, PageGetItemId(p, n));
		if (po->flags & F_LEAF)
		{
			if (index_keytest(it,
							  RelationGetDescr(s->indexRelation),
							  s->numberOfKeys, s->keyData))
				break;
		}
		else
		{
			if (index_keytest(it,
							  RelationGetDescr(s->indexRelation),
							  so->s_internalNKey, so->s_internalKey))
				break;
		}

		if (ScanDirectionIsBackward(dir))
			n = OffsetNumberPrev(n);
		else
			n = OffsetNumberNext(n);
	}

	if (n >= FirstOffsetNumber && n <= maxoff)
		return n;				/* found a match on this page */
	else
		return InvalidOffsetNumber;		/* no match, go to next page */
}
