/*-------------------------------------------------------------------------
 *
 * rtget.c
 *	  fetch tuples from an rtree scan.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/rtree/Attic/rtget.c,v 1.29 2003/08/04 02:39:57 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/iqual.h"
#include "access/relscan.h"
#include "access/rtree.h"

static OffsetNumber findnext(IndexScanDesc s, Page p, OffsetNumber n,
		 ScanDirection dir);
static bool rtscancache(IndexScanDesc s, ScanDirection dir);
static bool rtfirst(IndexScanDesc s, ScanDirection dir);
static bool rtnext(IndexScanDesc s, ScanDirection dir);


Datum
rtgettuple(PG_FUNCTION_ARGS)
{
	IndexScanDesc s = (IndexScanDesc) PG_GETARG_POINTER(0);
	ScanDirection dir = (ScanDirection) PG_GETARG_INT32(1);
	bool		res;

	/* if we have it cached in the scan desc, just return the value */
	if (rtscancache(s, dir))
		PG_RETURN_BOOL(true);

	/* not cached, so we'll have to do some work */
	if (ItemPointerIsValid(&(s->currentItemData)))
		res = rtnext(s, dir);
	else
		res = rtfirst(s, dir);
	PG_RETURN_BOOL(res);
}

static bool
rtfirst(IndexScanDesc s, ScanDirection dir)
{
	Buffer		b;
	Page		p;
	OffsetNumber n;
	OffsetNumber maxoff;
	RTreePageOpaque po;
	RTreeScanOpaque so;
	RTSTACK    *stk;
	BlockNumber blk;
	IndexTuple	it;

	b = ReadBuffer(s->indexRelation, P_ROOT);
	p = BufferGetPage(b);
	po = (RTreePageOpaque) PageGetSpecialPointer(p);
	so = (RTreeScanOpaque) s->opaque;

	for (;;)
	{
		maxoff = PageGetMaxOffsetNumber(p);
		if (ScanDirectionIsBackward(dir))
			n = findnext(s, p, maxoff, dir);
		else
			n = findnext(s, p, FirstOffsetNumber, dir);

		while (n < FirstOffsetNumber || n > maxoff)
		{
			ReleaseBuffer(b);
			if (so->s_stack == (RTSTACK *) NULL)
				return false;

			stk = so->s_stack;
			b = ReadBuffer(s->indexRelation, stk->rts_blk);
			p = BufferGetPage(b);
			po = (RTreePageOpaque) PageGetSpecialPointer(p);
			maxoff = PageGetMaxOffsetNumber(p);

			if (ScanDirectionIsBackward(dir))
				n = OffsetNumberPrev(stk->rts_child);
			else
				n = OffsetNumberNext(stk->rts_child);
			so->s_stack = stk->rts_parent;
			pfree(stk);

			n = findnext(s, p, n, dir);
		}
		if (po->flags & F_LEAF)
		{
			ItemPointerSet(&(s->currentItemData), BufferGetBlockNumber(b), n);

			it = (IndexTuple) PageGetItem(p, PageGetItemId(p, n));

			s->xs_ctup.t_self = it->t_tid;

			ReleaseBuffer(b);
			return true;
		}
		else
		{
			stk = (RTSTACK *) palloc(sizeof(RTSTACK));
			stk->rts_child = n;
			stk->rts_blk = BufferGetBlockNumber(b);
			stk->rts_parent = so->s_stack;
			so->s_stack = stk;

			it = (IndexTuple) PageGetItem(p, PageGetItemId(p, n));
			blk = ItemPointerGetBlockNumber(&(it->t_tid));

			ReleaseBuffer(b);
			b = ReadBuffer(s->indexRelation, blk);
			p = BufferGetPage(b);
			po = (RTreePageOpaque) PageGetSpecialPointer(p);
		}
	}
}

static bool
rtnext(IndexScanDesc s, ScanDirection dir)
{
	Buffer		b;
	Page		p;
	OffsetNumber n;
	OffsetNumber maxoff;
	RTreePageOpaque po;
	RTreeScanOpaque so;
	RTSTACK    *stk;
	BlockNumber blk;
	IndexTuple	it;

	blk = ItemPointerGetBlockNumber(&(s->currentItemData));
	n = ItemPointerGetOffsetNumber(&(s->currentItemData));

	if (ScanDirectionIsForward(dir))
		n = OffsetNumberNext(n);
	else
		n = OffsetNumberPrev(n);

	b = ReadBuffer(s->indexRelation, blk);
	p = BufferGetPage(b);
	po = (RTreePageOpaque) PageGetSpecialPointer(p);
	so = (RTreeScanOpaque) s->opaque;

	for (;;)
	{
		maxoff = PageGetMaxOffsetNumber(p);
		n = findnext(s, p, n, dir);

		while (n < FirstOffsetNumber || n > maxoff)
		{
			ReleaseBuffer(b);
			if (so->s_stack == (RTSTACK *) NULL)
				return false;

			stk = so->s_stack;
			b = ReadBuffer(s->indexRelation, stk->rts_blk);
			p = BufferGetPage(b);
			maxoff = PageGetMaxOffsetNumber(p);
			po = (RTreePageOpaque) PageGetSpecialPointer(p);

			if (ScanDirectionIsBackward(dir))
				n = OffsetNumberPrev(stk->rts_child);
			else
				n = OffsetNumberNext(stk->rts_child);
			so->s_stack = stk->rts_parent;
			pfree(stk);

			n = findnext(s, p, n, dir);
		}
		if (po->flags & F_LEAF)
		{
			ItemPointerSet(&(s->currentItemData), BufferGetBlockNumber(b), n);

			it = (IndexTuple) PageGetItem(p, PageGetItemId(p, n));

			s->xs_ctup.t_self = it->t_tid;

			ReleaseBuffer(b);
			return true;
		}
		else
		{
			stk = (RTSTACK *) palloc(sizeof(RTSTACK));
			stk->rts_child = n;
			stk->rts_blk = BufferGetBlockNumber(b);
			stk->rts_parent = so->s_stack;
			so->s_stack = stk;

			it = (IndexTuple) PageGetItem(p, PageGetItemId(p, n));
			blk = ItemPointerGetBlockNumber(&(it->t_tid));

			ReleaseBuffer(b);
			b = ReadBuffer(s->indexRelation, blk);
			p = BufferGetPage(b);
			po = (RTreePageOpaque) PageGetSpecialPointer(p);

			if (ScanDirectionIsBackward(dir))
				n = PageGetMaxOffsetNumber(p);
			else
				n = FirstOffsetNumber;
		}
	}
}

static OffsetNumber
findnext(IndexScanDesc s, Page p, OffsetNumber n, ScanDirection dir)
{
	OffsetNumber maxoff;
	IndexTuple	it;
	RTreePageOpaque po;
	RTreeScanOpaque so;

	maxoff = PageGetMaxOffsetNumber(p);
	po = (RTreePageOpaque) PageGetSpecialPointer(p);
	so = (RTreeScanOpaque) s->opaque;

	/*
	 * If we modified the index during the scan, we may have a pointer to
	 * a ghost tuple, before the scan.	If this is the case, back up one.
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

	return n;
}

static bool
rtscancache(IndexScanDesc s, ScanDirection dir)
{
	Buffer		b;
	Page		p;
	OffsetNumber n;
	IndexTuple	it;

	if (!(ScanDirectionIsNoMovement(dir)
		  && ItemPointerIsValid(&(s->currentItemData))))
		return false;

	b = ReadBuffer(s->indexRelation,
				   ItemPointerGetBlockNumber(&(s->currentItemData)));
	p = BufferGetPage(b);
	n = ItemPointerGetOffsetNumber(&(s->currentItemData));
	it = (IndexTuple) PageGetItem(p, PageGetItemId(p, n));
	s->xs_ctup.t_self = it->t_tid;
	ReleaseBuffer(b);

	return true;
}
