/*-------------------------------------------------------------------------
 *
 * gistget.c
 *	  fetch tuples from a GiST scan.
 *
 *
 *
 * IDENTIFICATION
 *	  /usr/local/devel/pglite/cvs/src/backend/access/gisr/gistget.c,v 1.9.1 1996/11/21 01:00:00 vadim Exp
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gist.h"
#include "executor/execdebug.h"



static OffsetNumber gistfindnext(IndexScanDesc s, Page p, OffsetNumber n,
			 ScanDirection dir);
static RetrieveIndexResult gistscancache(IndexScanDesc s, ScanDirection dir);
static RetrieveIndexResult gistfirst(IndexScanDesc s, ScanDirection dir);
static RetrieveIndexResult gistnext(IndexScanDesc s, ScanDirection dir);
static ItemPointer gistheapptr(Relation r, ItemPointer itemp);
static bool gistindex_keytest(IndexTuple tuple, TupleDesc tupdesc,
				  int scanKeySize, ScanKey key, GISTSTATE *giststate,
				  Relation r, Page p, OffsetNumber offset);


Datum
gistgettuple(PG_FUNCTION_ARGS)
{
	IndexScanDesc s = (IndexScanDesc) PG_GETARG_POINTER(0);
	ScanDirection dir = (ScanDirection) PG_GETARG_INT32(1);
	RetrieveIndexResult res;

	/* if we have it cached in the scan desc, just return the value */
	if ((res = gistscancache(s, dir)) != (RetrieveIndexResult) NULL)
		PG_RETURN_POINTER(res);

	/* not cached, so we'll have to do some work */
	if (ItemPointerIsValid(&(s->currentItemData)))
		res = gistnext(s, dir);
	else
		res = gistfirst(s, dir);
	PG_RETURN_POINTER(res);
}

static RetrieveIndexResult
gistfirst(IndexScanDesc s, ScanDirection dir)
{
	Buffer		b;
	Page		p;
	OffsetNumber n;
	OffsetNumber maxoff;
	RetrieveIndexResult res;
	GISTPageOpaque po;
	GISTScanOpaque so;
	GISTSTACK  *stk;
	BlockNumber blk;
	IndexTuple	it;

	b = ReadBuffer(s->relation, GISTP_ROOT);
	p = BufferGetPage(b);
	po = (GISTPageOpaque) PageGetSpecialPointer(p);
	so = (GISTScanOpaque) s->opaque;

	for (;;)
	{
		maxoff = PageGetMaxOffsetNumber(p);
		if (ScanDirectionIsBackward(dir))
			n = gistfindnext(s, p, maxoff, dir);
		else
			n = gistfindnext(s, p, FirstOffsetNumber, dir);

		while (n < FirstOffsetNumber || n > maxoff)
		{

			ReleaseBuffer(b);
			if (so->s_stack == (GISTSTACK *) NULL)
				return (RetrieveIndexResult) NULL;

			stk = so->s_stack;
			b = ReadBuffer(s->relation, stk->gs_blk);
			p = BufferGetPage(b);
			po = (GISTPageOpaque) PageGetSpecialPointer(p);
			maxoff = PageGetMaxOffsetNumber(p);

			if (ScanDirectionIsBackward(dir))
				n = OffsetNumberPrev(stk->gs_child);
			else
				n = OffsetNumberNext(stk->gs_child);
			so->s_stack = stk->gs_parent;
			pfree(stk);

			n = gistfindnext(s, p, n, dir);
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
			stk = (GISTSTACK *) palloc(sizeof(GISTSTACK));
			stk->gs_child = n;
			stk->gs_blk = BufferGetBlockNumber(b);
			stk->gs_parent = so->s_stack;
			so->s_stack = stk;

			it = (IndexTuple) PageGetItem(p, PageGetItemId(p, n));
			blk = ItemPointerGetBlockNumber(&(it->t_tid));

			ReleaseBuffer(b);
			b = ReadBuffer(s->relation, blk);
			p = BufferGetPage(b);
			po = (GISTPageOpaque) PageGetSpecialPointer(p);
		}
	}
}

static RetrieveIndexResult
gistnext(IndexScanDesc s, ScanDirection dir)
{
	Buffer		b;
	Page		p;
	OffsetNumber n;
	OffsetNumber maxoff;
	RetrieveIndexResult res;
	GISTPageOpaque po;
	GISTScanOpaque so;
	GISTSTACK  *stk;
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
	po = (GISTPageOpaque) PageGetSpecialPointer(p);
	so = (GISTScanOpaque) s->opaque;

	for (;;)
	{
		maxoff = PageGetMaxOffsetNumber(p);
		n = gistfindnext(s, p, n, dir);

		while (n < FirstOffsetNumber || n > maxoff)
		{

			ReleaseBuffer(b);
			if (so->s_stack == (GISTSTACK *) NULL)
				return (RetrieveIndexResult) NULL;

			stk = so->s_stack;
			b = ReadBuffer(s->relation, stk->gs_blk);
			p = BufferGetPage(b);
			maxoff = PageGetMaxOffsetNumber(p);
			po = (GISTPageOpaque) PageGetSpecialPointer(p);

			if (ScanDirectionIsBackward(dir))
				n = OffsetNumberPrev(stk->gs_child);
			else
				n = OffsetNumberNext(stk->gs_child);
			so->s_stack = stk->gs_parent;
			pfree(stk);

			n = gistfindnext(s, p, n, dir);
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
			stk = (GISTSTACK *) palloc(sizeof(GISTSTACK));
			stk->gs_child = n;
			stk->gs_blk = BufferGetBlockNumber(b);
			stk->gs_parent = so->s_stack;
			so->s_stack = stk;

			it = (IndexTuple) PageGetItem(p, PageGetItemId(p, n));
			blk = ItemPointerGetBlockNumber(&(it->t_tid));

			ReleaseBuffer(b);
			b = ReadBuffer(s->relation, blk);
			p = BufferGetPage(b);
			po = (GISTPageOpaque) PageGetSpecialPointer(p);

			if (ScanDirectionIsBackward(dir))
				n = PageGetMaxOffsetNumber(p);
			else
				n = FirstOffsetNumber;
		}
	}
}

/* Similar to index_keytest, but decompresses the key in the IndexTuple */
static bool
gistindex_keytest(IndexTuple tuple,
				  TupleDesc tupdesc,
				  int scanKeySize,
				  ScanKey key,
				  GISTSTATE *giststate,
				  Relation r,
				  Page p,
				  OffsetNumber offset)
{
	bool		isNull;
	Datum		datum;
	Datum		test;
	GISTENTRY	de;

	IncrIndexProcessed();


	while (scanKeySize > 0)
	{
		datum = index_getattr(tuple,
							  1,
							  tupdesc,
							  &isNull);
		gistdentryinit(giststate, &de, (char *) datum, r, p, offset,
					   IndexTupleSize(tuple) - sizeof(IndexTupleData),
					   FALSE);

		if (isNull)
		{
			/* XXX eventually should check if SK_ISNULL */
			return false;
		}

		if (key[0].sk_flags & SK_COMMUTE)
		{
			test = FunctionCall3(&key[0].sk_func,
								 key[0].sk_argument,
								 PointerGetDatum(&de),
								 ObjectIdGetDatum(key[0].sk_procedure));
		}
		else
		{
			test = FunctionCall3(&key[0].sk_func,
								 PointerGetDatum(&de),
								 key[0].sk_argument,
								 ObjectIdGetDatum(key[0].sk_procedure));
		}

		if (DatumGetBool(test) == !!(key[0].sk_flags & SK_NEGATE))
			return false;

		scanKeySize -= 1;
		key++;
	}

	return true;
}


static OffsetNumber
gistfindnext(IndexScanDesc s, Page p, OffsetNumber n, ScanDirection dir)
{
	OffsetNumber maxoff;
	char	   *it;
	GISTPageOpaque po;
	GISTScanOpaque so;
	GISTSTATE  *giststate;

	maxoff = PageGetMaxOffsetNumber(p);
	po = (GISTPageOpaque) PageGetSpecialPointer(p);
	so = (GISTScanOpaque) s->opaque;
	giststate = so->giststate;

	/*
	 * If we modified the index during the scan, we may have a pointer to
	 * a ghost tuple, before the scan.	If this is the case, back up one.
	 */

	if (so->s_flags & GS_CURBEFORE)
	{
		so->s_flags &= ~GS_CURBEFORE;
		n = OffsetNumberPrev(n);
	}

	while (n >= FirstOffsetNumber && n <= maxoff)
	{
		it = (char *) PageGetItem(p, PageGetItemId(p, n));
		if (gistindex_keytest((IndexTuple) it,
							  RelationGetDescr(s->relation),
							  s->numberOfKeys, s->keyData, giststate,
							  s->relation, p, n))
			break;

		if (ScanDirectionIsBackward(dir))
			n = OffsetNumberPrev(n);
		else
			n = OffsetNumberNext(n);
	}

	return n;
}

static RetrieveIndexResult
gistscancache(IndexScanDesc s, ScanDirection dir)
{
	RetrieveIndexResult res;
	ItemPointer ip;

	if (!(ScanDirectionIsNoMovement(dir)
		  && ItemPointerIsValid(&(s->currentItemData))))
	{

		return (RetrieveIndexResult) NULL;
	}

	ip = gistheapptr(s->relation, &(s->currentItemData));

	if (ItemPointerIsValid(ip))
		res = FormRetrieveIndexResult(&(s->currentItemData), ip);
	else
		res = (RetrieveIndexResult) NULL;

	pfree(ip);

	return res;
}

/*
 *	gistheapptr returns the item pointer to the tuple in the heap relation
 *	for which itemp is the index relation item pointer.
 */
static ItemPointer
gistheapptr(Relation r, ItemPointer itemp)
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
