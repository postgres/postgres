/*-------------------------------------------------------------------------
 *
 * rtget.c--
 *	  fetch tuples from an rtree scan.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/rtree/Attic/rtget.c,v 1.12 1998/09/01 04:27:09 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <storage/bufmgr.h>
#include <access/sdir.h>
#include <access/relscan.h>
#include <access/iqual.h>
#include <access/rtree.h>
#include <storage/bufpage.h>
#ifndef HAVE_MEMMOVE
#include <regex/utils.h>
#else
#include <string.h>
#endif


static OffsetNumber findnext(IndexScanDesc s, Page p, OffsetNumber n,
		 ScanDirection dir);
static RetrieveIndexResult rtscancache(IndexScanDesc s, ScanDirection dir);
static RetrieveIndexResult rtfirst(IndexScanDesc s, ScanDirection dir);
static RetrieveIndexResult rtnext(IndexScanDesc s, ScanDirection dir);
static ItemPointer rtheapptr(Relation r, ItemPointer itemp);


RetrieveIndexResult
rtgettuple(IndexScanDesc s, ScanDirection dir)
{
	RetrieveIndexResult res;

	/* if we have it cached in the scan desc, just return the value */
	if ((res = rtscancache(s, dir)) != (RetrieveIndexResult) NULL)
		return res;

	/* not cached, so we'll have to do some work */
	if (ItemPointerIsValid(&(s->currentItemData)))
		res = rtnext(s, dir);
	else
		res = rtfirst(s, dir);
	return res;
}

static RetrieveIndexResult
rtfirst(IndexScanDesc s, ScanDirection dir)
{
	Buffer		b;
	Page		p;
	OffsetNumber n;
	OffsetNumber maxoff;
	RetrieveIndexResult res;
	RTreePageOpaque po;
	RTreeScanOpaque so;
	RTSTACK    *stk;
	BlockNumber blk;
	IndexTuple	it;

	b = ReadBuffer(s->relation, P_ROOT);
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
				return (RetrieveIndexResult) NULL;

			stk = so->s_stack;
			b = ReadBuffer(s->relation, stk->rts_blk);
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

			res = FormRetrieveIndexResult(&(s->currentItemData), &(it->t_tid));

			ReleaseBuffer(b);
			return res;
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
			b = ReadBuffer(s->relation, blk);
			p = BufferGetPage(b);
			po = (RTreePageOpaque) PageGetSpecialPointer(p);
		}
	}
}

static RetrieveIndexResult
rtnext(IndexScanDesc s, ScanDirection dir)
{
	Buffer		b;
	Page		p;
	OffsetNumber n;
	OffsetNumber maxoff;
	RetrieveIndexResult res;
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

	b = ReadBuffer(s->relation, blk);
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
				return (RetrieveIndexResult) NULL;

			stk = so->s_stack;
			b = ReadBuffer(s->relation, stk->rts_blk);
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

			res = FormRetrieveIndexResult(&(s->currentItemData), &(it->t_tid));

			ReleaseBuffer(b);
			return res;
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
			b = ReadBuffer(s->relation, blk);
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
							  RelationGetDescr(s->relation),
							  s->numberOfKeys, s->keyData))
				break;
		}
		else
		{
			if (index_keytest(it,
							  RelationGetDescr(s->relation),
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

static RetrieveIndexResult
rtscancache(IndexScanDesc s, ScanDirection dir)
{
	RetrieveIndexResult res;
	ItemPointer ip;

	if (!(ScanDirectionIsNoMovement(dir)
		  && ItemPointerIsValid(&(s->currentItemData))))
	{

		return (RetrieveIndexResult) NULL;
	}

	ip = rtheapptr(s->relation, &(s->currentItemData));

	if (ItemPointerIsValid(ip))
		res = FormRetrieveIndexResult(&(s->currentItemData), ip);
	else
		res = (RetrieveIndexResult) NULL;

	pfree(ip);

	return res;
}

/*
 *	rtheapptr returns the item pointer to the tuple in the heap relation
 *	for which itemp is the index relation item pointer.
 */
static ItemPointer
rtheapptr(Relation r, ItemPointer itemp)
{
	Buffer		b;
	Page		p;
	IndexTuple	it;
	ItemPointer ip;
	OffsetNumber n;

	ip = (ItemPointer) palloc(sizeof(ItemPointerData));
	if (ItemPointerIsValid(itemp))
	{
		b = ReadBuffer(r, ItemPointerGetBlockNumber(itemp));
		p = BufferGetPage(b);
		n = ItemPointerGetOffsetNumber(itemp);
		it = (IndexTuple) PageGetItem(p, PageGetItemId(p, n));
		memmove((char *) ip, (char *) &(it->t_tid),
				sizeof(ItemPointerData));
		ReleaseBuffer(b);
	}
	else
		ItemPointerSetInvalid(ip);

	return ip;
}
