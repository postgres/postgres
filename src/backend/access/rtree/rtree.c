/*-------------------------------------------------------------------------
 *
 * rtree.c
 *	  interface routines for the postgres rtree indexed access method.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/rtree/Attic/rtree.c,v 1.61 2001/03/22 03:59:16 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/rtree.h"
#include "access/xlogutils.h"
#include "catalog/index.h"
#include "executor/executor.h"
#include "miscadmin.h"


/*
 * XXX We assume that all datatypes indexable in rtrees are pass-by-reference.
 * To fix this, you'd need to improve the IndexTupleGetDatum() macro, and
 * do something with the various datum-pfreeing code.  However, it's not that
 * unreasonable an assumption in practice.
 */
#define IndexTupleGetDatum(itup)  \
	PointerGetDatum(((char *) (itup)) + sizeof(IndexTupleData))

/*
 * Space-allocation macros.  Note we count the item's line pointer in its size.
 */
#define RTPageAvailSpace  \
	(BLCKSZ - (sizeof(PageHeaderData) - sizeof(ItemIdData)) \
	 - MAXALIGN(sizeof(RTreePageOpaqueData)))
#define IndexTupleTotalSize(itup)  \
	(MAXALIGN(IndexTupleSize(itup)) + sizeof(ItemIdData))
#define IndexTupleAttSize(itup)  \
	(IndexTupleSize(itup) - sizeof(IndexTupleData))

/* results of rtpicksplit() */
typedef struct SPLITVEC
{
	OffsetNumber *spl_left;
	int			spl_nleft;
	Datum		spl_ldatum;
	OffsetNumber *spl_right;
	int			spl_nright;
	Datum		spl_rdatum;
} SPLITVEC;

typedef struct RTSTATE
{
	FmgrInfo	unionFn;		/* union function */
	FmgrInfo	sizeFn;			/* size function */
	FmgrInfo	interFn;		/* intersection function */
} RTSTATE;

/* non-export function prototypes */
static InsertIndexResult rtdoinsert(Relation r, IndexTuple itup,
		   RTSTATE *rtstate);
static void rttighten(Relation r, RTSTACK *stk, Datum datum, int att_size,
		  RTSTATE *rtstate);
static InsertIndexResult rtdosplit(Relation r, Buffer buffer, RTSTACK *stack,
		  IndexTuple itup, RTSTATE *rtstate);
static void rtintinsert(Relation r, RTSTACK *stk, IndexTuple ltup,
			IndexTuple rtup, RTSTATE *rtstate);
static void rtnewroot(Relation r, IndexTuple lt, IndexTuple rt);
static void rtpicksplit(Relation r, Page page, SPLITVEC *v, IndexTuple itup,
			RTSTATE *rtstate);
static void RTInitBuffer(Buffer b, uint32 f);
static OffsetNumber choose(Relation r, Page p, IndexTuple it,
	   RTSTATE *rtstate);
static int	nospace(Page p, IndexTuple it);
static void initRtstate(RTSTATE *rtstate, Relation index);


Datum
rtbuild(PG_FUNCTION_ARGS)
{
	Relation	heap = (Relation) PG_GETARG_POINTER(0);
	Relation	index = (Relation) PG_GETARG_POINTER(1);
	IndexInfo  *indexInfo = (IndexInfo *) PG_GETARG_POINTER(2);
	Node	   *oldPred = (Node *) PG_GETARG_POINTER(3);

#ifdef NOT_USED
	IndexStrategy istrat = (IndexStrategy) PG_GETARG_POINTER(4);

#endif
	HeapScanDesc hscan;
	HeapTuple	htup;
	IndexTuple	itup;
	TupleDesc	htupdesc,
				itupdesc;
	Datum		attdata[INDEX_MAX_KEYS];
	char		nulls[INDEX_MAX_KEYS];
	int			nhtups,
				nitups;
	Node	   *pred = indexInfo->ii_Predicate;

#ifndef OMIT_PARTIAL_INDEX
	TupleTable	tupleTable;
	TupleTableSlot *slot;

#endif
	ExprContext *econtext;
	InsertIndexResult res = NULL;
	Buffer		buffer = InvalidBuffer;
	RTSTATE		rtState;

	initRtstate(&rtState, index);

	/*
	 * We expect to be called exactly once for any index relation. If
	 * that's not the case, big trouble's what we have.
	 */
	if (oldPred == NULL && RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "%s already contains data", RelationGetRelationName(index));

	/* initialize the root page (if this is a new index) */
	if (oldPred == NULL)
	{
		buffer = ReadBuffer(index, P_NEW);
		RTInitBuffer(buffer, F_LEAF);
		WriteBuffer(buffer);
	}

	/* get tuple descriptors for heap and index relations */
	htupdesc = RelationGetDescr(heap);
	itupdesc = RelationGetDescr(index);

	/*
	 * If this is a predicate (partial) index, we will need to evaluate
	 * the predicate using ExecQual, which requires the current tuple to
	 * be in a slot of a TupleTable.  In addition, ExecQual must have an
	 * ExprContext referring to that slot.	Here, we initialize dummy
	 * TupleTable and ExprContext objects for this purpose. --Nels, Feb 92
	 *
	 * We construct the ExprContext anyway since we need a per-tuple
	 * temporary memory context for function evaluation -- tgl July 00
	 */
#ifndef OMIT_PARTIAL_INDEX
	if (pred != NULL || oldPred != NULL)
	{
		tupleTable = ExecCreateTupleTable(1);
		slot = ExecAllocTableSlot(tupleTable);
		ExecSetSlotDescriptor(slot, htupdesc, false);
	}
	else
	{
		tupleTable = NULL;
		slot = NULL;
	}
	econtext = MakeExprContext(slot, TransactionCommandContext);
#else
	econtext = MakeExprContext(NULL, TransactionCommandContext);
#endif	 /* OMIT_PARTIAL_INDEX */

	/* count the tuples as we insert them */
	nhtups = nitups = 0;

	/* start a heap scan */
	hscan = heap_beginscan(heap, 0, SnapshotNow, 0, (ScanKey) NULL);

	while (HeapTupleIsValid(htup = heap_getnext(hscan, 0)))
	{
		MemoryContextReset(econtext->ecxt_per_tuple_memory);

		nhtups++;

#ifndef OMIT_PARTIAL_INDEX

		/*
		 * If oldPred != NULL, this is an EXTEND INDEX command, so skip
		 * this tuple if it was already in the existing partial index
		 */
		if (oldPred != NULL)
		{
			slot->val = htup;
			if (ExecQual((List *) oldPred, econtext, false))
			{
				nitups++;
				continue;
			}
		}

		/*
		 * Skip this tuple if it doesn't satisfy the partial-index
		 * predicate
		 */
		if (pred != NULL)
		{
			slot->val = htup;
			if (!ExecQual((List *) pred, econtext, false))
				continue;
		}
#endif	 /* OMIT_PARTIAL_INDEX */

		nitups++;

		/*
		 * For the current heap tuple, extract all the attributes we use
		 * in this index, and note which are null.
		 */
		FormIndexDatum(indexInfo,
					   htup,
					   htupdesc,
					   econtext->ecxt_per_tuple_memory,
					   attdata,
					   nulls);

		/* form an index tuple and point it at the heap tuple */
		itup = index_formtuple(itupdesc, attdata, nulls);
		itup->t_tid = htup->t_self;

		/*
		 * Since we already have the index relation locked, we call
		 * rtdoinsert directly.  Normal access method calls dispatch
		 * through rtinsert, which locks the relation for write.  This is
		 * the right thing to do if you're inserting single tups, but not
		 * when you're initializing the whole index at once.
		 */

		res = rtdoinsert(index, itup, &rtState);
		pfree(itup);
		pfree(res);
	}

	/* okay, all heap tuples are indexed */
	heap_endscan(hscan);

#ifndef OMIT_PARTIAL_INDEX
	if (pred != NULL || oldPred != NULL)
		ExecDropTupleTable(tupleTable, true);
#endif	 /* OMIT_PARTIAL_INDEX */
	FreeExprContext(econtext);

	/*
	 * Since we just counted the tuples in the heap, we update its stats
	 * in pg_class to guarantee that the planner takes advantage of the
	 * index we just created.  But, only update statistics during normal
	 * index definitions, not for indices on system catalogs created
	 * during bootstrap processing.  We must close the relations before
	 * updating statistics to guarantee that the relcache entries are
	 * flushed when we increment the command counter in UpdateStats(). But
	 * we do not release any locks on the relations; those will be held
	 * until end of transaction.
	 */
	if (IsNormalProcessingMode())
	{
		Oid			hrelid = RelationGetRelid(heap);
		Oid			irelid = RelationGetRelid(index);

		heap_close(heap, NoLock);
		index_close(index);
		UpdateStats(hrelid, nhtups);
		UpdateStats(irelid, nitups);
		if (oldPred != NULL)
		{
			if (nitups == nhtups)
				pred = NULL;
			UpdateIndexPredicate(irelid, oldPred, pred);
		}
	}

	PG_RETURN_VOID();
}

/*
 *	rtinsert -- wrapper for rtree tuple insertion.
 *
 *	  This is the public interface routine for tuple insertion in rtrees.
 *	  It doesn't do any work; just locks the relation and passes the buck.
 */
Datum
rtinsert(PG_FUNCTION_ARGS)
{
	Relation	r = (Relation) PG_GETARG_POINTER(0);
	Datum	   *datum = (Datum *) PG_GETARG_POINTER(1);
	char	   *nulls = (char *) PG_GETARG_POINTER(2);
	ItemPointer ht_ctid = (ItemPointer) PG_GETARG_POINTER(3);

#ifdef NOT_USED
	Relation	heapRel = (Relation) PG_GETARG_POINTER(4);

#endif
	InsertIndexResult res;
	IndexTuple	itup;
	RTSTATE		rtState;

	/* generate an index tuple */
	itup = index_formtuple(RelationGetDescr(r), datum, nulls);
	itup->t_tid = *ht_ctid;
	initRtstate(&rtState, r);

	/*
	 * Notes in ExecUtils:ExecOpenIndices()
	 *
	 * RelationSetLockForWrite(r);
	 */

	res = rtdoinsert(r, itup, &rtState);

	PG_RETURN_POINTER(res);
}

static InsertIndexResult
rtdoinsert(Relation r, IndexTuple itup, RTSTATE *rtstate)
{
	Page		page;
	Buffer		buffer;
	BlockNumber blk;
	IndexTuple	which;
	OffsetNumber l;
	RTSTACK    *stack;
	InsertIndexResult res;
	RTreePageOpaque opaque;
	Datum		datum;

	blk = P_ROOT;
	buffer = InvalidBuffer;
	stack = (RTSTACK *) NULL;

	do
	{
		/* let go of current buffer before getting next */
		if (buffer != InvalidBuffer)
			ReleaseBuffer(buffer);

		/* get next buffer */
		buffer = ReadBuffer(r, blk);
		page = (Page) BufferGetPage(buffer);

		opaque = (RTreePageOpaque) PageGetSpecialPointer(page);
		if (!(opaque->flags & F_LEAF))
		{
			RTSTACK    *n;
			ItemId		iid;

			n = (RTSTACK *) palloc(sizeof(RTSTACK));
			n->rts_parent = stack;
			n->rts_blk = blk;
			n->rts_child = choose(r, page, itup, rtstate);
			stack = n;

			iid = PageGetItemId(page, n->rts_child);
			which = (IndexTuple) PageGetItem(page, iid);
			blk = ItemPointerGetBlockNumber(&(which->t_tid));
		}
	} while (!(opaque->flags & F_LEAF));

	if (nospace(page, itup))
	{
		/* need to do a split */
		res = rtdosplit(r, buffer, stack, itup, rtstate);
		freestack(stack);
		WriteBuffer(buffer);	/* don't forget to release buffer! */
		return res;
	}

	/* add the item and write the buffer */
	if (PageIsEmpty(page))
	{
		l = PageAddItem(page, (Item) itup, IndexTupleSize(itup),
						FirstOffsetNumber,
						LP_USED);
	}
	else
	{
		l = PageAddItem(page, (Item) itup, IndexTupleSize(itup),
						OffsetNumberNext(PageGetMaxOffsetNumber(page)),
						LP_USED);
	}
	if (l == InvalidOffsetNumber)
		elog(ERROR, "rtdoinsert: failed to add index item to %s",
			 RelationGetRelationName(r));

	WriteBuffer(buffer);

	datum = IndexTupleGetDatum(itup);

	/* now expand the page boundary in the parent to include the new child */
	rttighten(r, stack, datum, IndexTupleAttSize(itup), rtstate);
	freestack(stack);

	/* build and return an InsertIndexResult for this insertion */
	res = (InsertIndexResult) palloc(sizeof(InsertIndexResultData));
	ItemPointerSet(&(res->pointerData), blk, l);

	return res;
}

static void
rttighten(Relation r,
		  RTSTACK *stk,
		  Datum datum,
		  int att_size,
		  RTSTATE *rtstate)
{
	Datum		oldud;
	Datum		tdatum;
	Page		p;
	float		old_size,
				newd_size;
	Buffer		b;

	if (stk == (RTSTACK *) NULL)
		return;

	b = ReadBuffer(r, stk->rts_blk);
	p = BufferGetPage(b);

	oldud = IndexTupleGetDatum(PageGetItem(p,
									  PageGetItemId(p, stk->rts_child)));

	FunctionCall2(&rtstate->sizeFn, oldud,
				  PointerGetDatum(&old_size));

	datum = FunctionCall2(&rtstate->unionFn, oldud, datum);

	FunctionCall2(&rtstate->sizeFn, datum,
				  PointerGetDatum(&newd_size));

	if (newd_size != old_size)
	{
		TupleDesc	td = RelationGetDescr(r);

		if (td->attrs[0]->attlen < 0)
		{

			/*
			 * This is an internal page, so 'oldud' had better be a union
			 * (constant-length) key, too.	(See comment below.)
			 */
			Assert(VARSIZE(DatumGetPointer(datum)) ==
				   VARSIZE(DatumGetPointer(oldud)));
			memmove(DatumGetPointer(oldud), DatumGetPointer(datum),
					VARSIZE(DatumGetPointer(datum)));
		}
		else
		{
			memmove(DatumGetPointer(oldud), DatumGetPointer(datum),
					att_size);
		}
		WriteBuffer(b);

		/*
		 * The user may be defining an index on variable-sized data (like
		 * polygons).  If so, we need to get a constant-sized datum for
		 * insertion on the internal page.	We do this by calling the
		 * union proc, which is required to return a rectangle.
		 */
		tdatum = FunctionCall2(&rtstate->unionFn, datum, datum);

		rttighten(r, stk->rts_parent, tdatum, att_size, rtstate);
		pfree(DatumGetPointer(tdatum));
	}
	else
		ReleaseBuffer(b);
	pfree(DatumGetPointer(datum));
}

/*
 *	rtdosplit -- split a page in the tree.
 *
 *	  rtpicksplit does the interesting work of choosing the split.
 *	  This routine just does the bit-pushing.
 */
static InsertIndexResult
rtdosplit(Relation r,
		  Buffer buffer,
		  RTSTACK *stack,
		  IndexTuple itup,
		  RTSTATE *rtstate)
{
	Page		p;
	Buffer		leftbuf,
				rightbuf;
	Page		left,
				right;
	ItemId		itemid;
	IndexTuple	item;
	IndexTuple	ltup,
				rtup;
	OffsetNumber maxoff;
	OffsetNumber i;
	OffsetNumber leftoff,
				rightoff;
	BlockNumber lbknum,
				rbknum;
	BlockNumber bufblock;
	RTreePageOpaque opaque;
	int			blank;
	InsertIndexResult res;
	char	   *isnull;
	SPLITVEC	v;
	OffsetNumber *spl_left,
			   *spl_right;
	TupleDesc	tupDesc;

	p = (Page) BufferGetPage(buffer);
	opaque = (RTreePageOpaque) PageGetSpecialPointer(p);

	rtpicksplit(r, p, &v, itup, rtstate);

	/*
	 * The root of the tree is the first block in the relation.  If we're
	 * about to split the root, we need to do some hocus-pocus to enforce
	 * this guarantee.
	 */

	if (BufferGetBlockNumber(buffer) == P_ROOT)
	{
		leftbuf = ReadBuffer(r, P_NEW);
		RTInitBuffer(leftbuf, opaque->flags);
		lbknum = BufferGetBlockNumber(leftbuf);
		left = (Page) BufferGetPage(leftbuf);
	}
	else
	{
		leftbuf = buffer;
		IncrBufferRefCount(buffer);
		lbknum = BufferGetBlockNumber(buffer);
		left = (Page) PageGetTempPage(p, sizeof(RTreePageOpaqueData));
	}

	rightbuf = ReadBuffer(r, P_NEW);
	RTInitBuffer(rightbuf, opaque->flags);
	rbknum = BufferGetBlockNumber(rightbuf);
	right = (Page) BufferGetPage(rightbuf);

	spl_left = v.spl_left;
	spl_right = v.spl_right;
	leftoff = rightoff = FirstOffsetNumber;
	maxoff = PageGetMaxOffsetNumber(p);
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		itemid = PageGetItemId(p, i);
		item = (IndexTuple) PageGetItem(p, itemid);

		if (i == *spl_left)
		{
			if (PageAddItem(left, (Item) item, IndexTupleSize(item),
							leftoff, LP_USED) == InvalidOffsetNumber)
				elog(ERROR, "rtdosplit: failed to copy index item in %s",
					 RelationGetRelationName(r));
			leftoff = OffsetNumberNext(leftoff);
			spl_left++;			/* advance in left split vector */
		}
		else
		{
			Assert(i == *spl_right);
			if (PageAddItem(right, (Item) item, IndexTupleSize(item),
							rightoff, LP_USED) == InvalidOffsetNumber)
				elog(ERROR, "rtdosplit: failed to copy index item in %s",
					 RelationGetRelationName(r));
			rightoff = OffsetNumberNext(rightoff);
			spl_right++;		/* advance in right split vector */
		}
	}

	/* build an InsertIndexResult for this insertion */
	res = (InsertIndexResult) palloc(sizeof(InsertIndexResultData));

	/* now insert the new index tuple */
	if (*spl_left == maxoff + 1)
	{
		if (PageAddItem(left, (Item) itup, IndexTupleSize(itup),
						leftoff, LP_USED) == InvalidOffsetNumber)
			elog(ERROR, "rtdosplit: failed to add index item to %s",
				 RelationGetRelationName(r));
		leftoff = OffsetNumberNext(leftoff);
		ItemPointerSet(&(res->pointerData), lbknum, leftoff);
		spl_left++;
	}
	else
	{
		Assert(*spl_right == maxoff + 1);
		if (PageAddItem(right, (Item) itup, IndexTupleSize(itup),
						rightoff, LP_USED) == InvalidOffsetNumber)
			elog(ERROR, "rtdosplit: failed to add index item to %s",
				 RelationGetRelationName(r));
		rightoff = OffsetNumberNext(rightoff);
		ItemPointerSet(&(res->pointerData), rbknum, rightoff);
		spl_right++;
	}

	/* Make sure we consumed all of the split vectors, and release 'em */
	Assert(*spl_left == InvalidOffsetNumber);
	Assert(*spl_right == InvalidOffsetNumber);
	pfree(v.spl_left);
	pfree(v.spl_right);

	if ((bufblock = BufferGetBlockNumber(buffer)) != P_ROOT)
		PageRestoreTempPage(left, p);
	WriteBuffer(leftbuf);
	WriteBuffer(rightbuf);

	/*
	 * Okay, the page is split.  We have three things left to do:
	 *
	 * 1)  Adjust any active scans on this index to cope with changes we
	 * introduced in its structure by splitting this page.
	 *
	 * 2)  "Tighten" the bounding box of the pointer to the left page in the
	 * parent node in the tree, if any.  Since we moved a bunch of stuff
	 * off the left page, we expect it to get smaller.	This happens in
	 * the internal insertion routine.
	 *
	 * 3)  Insert a pointer to the right page in the parent.  This may cause
	 * the parent to split.  If it does, we need to repeat steps one and
	 * two for each split node in the tree.
	 */

	/* adjust active scans */
	rtadjscans(r, RTOP_SPLIT, bufblock, FirstOffsetNumber);

	tupDesc = r->rd_att;
	isnull = (char *) palloc(r->rd_rel->relnatts);
	for (blank = 0; blank < r->rd_rel->relnatts; blank++)
		isnull[blank] = ' ';

	ltup = (IndexTuple) index_formtuple(tupDesc,
										&(v.spl_ldatum), isnull);
	rtup = (IndexTuple) index_formtuple(tupDesc,
										&(v.spl_rdatum), isnull);
	pfree(isnull);

	/* set pointers to new child pages in the internal index tuples */
	ItemPointerSet(&(ltup->t_tid), lbknum, 1);
	ItemPointerSet(&(rtup->t_tid), rbknum, 1);

	rtintinsert(r, stack, ltup, rtup, rtstate);

	pfree(ltup);
	pfree(rtup);

	return res;
}

static void
rtintinsert(Relation r,
			RTSTACK *stk,
			IndexTuple ltup,
			IndexTuple rtup,
			RTSTATE *rtstate)
{
	IndexTuple	old;
	Buffer		b;
	Page		p;
	Datum		ldatum,
				rdatum,
				newdatum;
	InsertIndexResult res;

	if (stk == (RTSTACK *) NULL)
	{
		rtnewroot(r, ltup, rtup);
		return;
	}

	b = ReadBuffer(r, stk->rts_blk);
	p = BufferGetPage(b);
	old = (IndexTuple) PageGetItem(p, PageGetItemId(p, stk->rts_child));

	/*
	 * This is a hack.	Right now, we force rtree internal keys to be
	 * constant size. To fix this, need delete the old key and add both
	 * left and right for the two new pages.  The insertion of left may
	 * force a split if the new left key is bigger than the old key.
	 */

	if (IndexTupleSize(old) != IndexTupleSize(ltup))
		elog(ERROR, "Variable-length rtree keys are not supported.");

	/* install pointer to left child */
	memmove(old, ltup, IndexTupleSize(ltup));

	if (nospace(p, rtup))
	{
		newdatum = IndexTupleGetDatum(ltup);
		rttighten(r, stk->rts_parent, newdatum,
				  IndexTupleAttSize(ltup), rtstate);
		res = rtdosplit(r, b, stk->rts_parent, rtup, rtstate);
		WriteBuffer(b);			/* don't forget to release buffer!  -
								 * 01/31/94 */
		pfree(res);
	}
	else
	{
		if (PageAddItem(p, (Item) rtup, IndexTupleSize(rtup),
						PageGetMaxOffsetNumber(p),
						LP_USED) == InvalidOffsetNumber)
			elog(ERROR, "rtintinsert: failed to add index item to %s",
				 RelationGetRelationName(r));
		WriteBuffer(b);
		ldatum = IndexTupleGetDatum(ltup);
		rdatum = IndexTupleGetDatum(rtup);
		newdatum = FunctionCall2(&rtstate->unionFn, ldatum, rdatum);

		rttighten(r, stk->rts_parent, newdatum,
				  IndexTupleAttSize(rtup), rtstate);

		pfree(DatumGetPointer(newdatum));
	}
}

static void
rtnewroot(Relation r, IndexTuple lt, IndexTuple rt)
{
	Buffer		b;
	Page		p;

	b = ReadBuffer(r, P_ROOT);
	RTInitBuffer(b, 0);
	p = BufferGetPage(b);
	if (PageAddItem(p, (Item) lt, IndexTupleSize(lt),
					FirstOffsetNumber,
					LP_USED) == InvalidOffsetNumber)
		elog(ERROR, "rtnewroot: failed to add index item to %s",
			 RelationGetRelationName(r));
	if (PageAddItem(p, (Item) rt, IndexTupleSize(rt),
					OffsetNumberNext(FirstOffsetNumber),
					LP_USED) == InvalidOffsetNumber)
		elog(ERROR, "rtnewroot: failed to add index item to %s",
			 RelationGetRelationName(r));
	WriteBuffer(b);
}

/*
 * Choose how to split an rtree page into two pages.
 *
 * We return two vectors of index item numbers, one for the items to be
 * put on the left page, one for the items to be put on the right page.
 * In addition, the item to be added (itup) is listed in the appropriate
 * vector.	It is represented by item number N+1 (N = # of items on page).
 *
 * Both vectors appear in sequence order with a terminating sentinel value
 * of InvalidOffsetNumber.
 *
 * The bounding-box datums for the two new pages are also returned in *v.
 *
 * This is the quadratic-cost split algorithm Guttman describes in
 * his paper.  The reason we chose it is that you can implement this
 * with less information about the data types on which you're operating.
 *
 * We must also deal with a consideration not found in Guttman's algorithm:
 * variable-length data.  In particular, the incoming item might be
 * large enough that not just any split will work.	In the worst case,
 * our "split" may have to be the new item on one page and all the existing
 * items on the other.	Short of that, we have to take care that we do not
 * make a split that leaves both pages too full for the new item.
 */
static void
rtpicksplit(Relation r,
			Page page,
			SPLITVEC *v,
			IndexTuple itup,
			RTSTATE *rtstate)
{
	OffsetNumber maxoff,
				newitemoff;
	OffsetNumber i,
				j;
	IndexTuple	item_1,
				item_2;
	Datum		datum_alpha,
				datum_beta;
	Datum		datum_l,
				datum_r;
	Datum		union_d,
				union_dl,
				union_dr;
	Datum		inter_d;
	bool		firsttime;
	float		size_alpha,
				size_beta,
				size_union,
				size_inter;
	float		size_waste,
				waste;
	float		size_l,
				size_r;
	int			nbytes;
	OffsetNumber seed_1 = 0,
				seed_2 = 0;
	OffsetNumber *left,
			   *right;
	Size		newitemsz,
				item_1_sz,
				item_2_sz,
				left_avail_space,
				right_avail_space;

	/*
	 * First, make sure the new item is not so large that we can't
	 * possibly fit it on a page, even by itself.  (It's sufficient to
	 * make this test here, since any oversize tuple must lead to a page
	 * split attempt.)
	 */
	newitemsz = IndexTupleTotalSize(itup);
	if (newitemsz > RTPageAvailSpace)
		elog(ERROR, "rtree: index item size %lu exceeds maximum %lu",
			 (unsigned long) newitemsz, (unsigned long) RTPageAvailSpace);

	maxoff = PageGetMaxOffsetNumber(page);
	newitemoff = OffsetNumberNext(maxoff);		/* phony index for new
												 * item */

	/* Make arrays big enough for worst case, including sentinel */
	nbytes = (maxoff + 2) * sizeof(OffsetNumber);
	v->spl_left = (OffsetNumber *) palloc(nbytes);
	v->spl_right = (OffsetNumber *) palloc(nbytes);

	firsttime = true;
	waste = 0.0;

	for (i = FirstOffsetNumber; i < maxoff; i = OffsetNumberNext(i))
	{
		item_1 = (IndexTuple) PageGetItem(page, PageGetItemId(page, i));
		datum_alpha = IndexTupleGetDatum(item_1);
		item_1_sz = IndexTupleTotalSize(item_1);

		for (j = OffsetNumberNext(i); j <= maxoff; j = OffsetNumberNext(j))
		{
			item_2 = (IndexTuple) PageGetItem(page, PageGetItemId(page, j));
			datum_beta = IndexTupleGetDatum(item_2);
			item_2_sz = IndexTupleTotalSize(item_2);

			/*
			 * Ignore seed pairs that don't leave room for the new item on
			 * either split page.
			 */
			if (newitemsz + item_1_sz > RTPageAvailSpace &&
				newitemsz + item_2_sz > RTPageAvailSpace)
				continue;

			/* compute the wasted space by unioning these guys */
			union_d = FunctionCall2(&rtstate->unionFn,
									datum_alpha, datum_beta);
			FunctionCall2(&rtstate->sizeFn, union_d,
						  PointerGetDatum(&size_union));
			inter_d = FunctionCall2(&rtstate->interFn,
									datum_alpha, datum_beta);

			/*
			 * The interFn may return a NULL pointer (not an SQL null!) to
			 * indicate no intersection.  sizeFn must cope with this.
			 */
			FunctionCall2(&rtstate->sizeFn, inter_d,
						  PointerGetDatum(&size_inter));
			size_waste = size_union - size_inter;

			if (DatumGetPointer(union_d) != NULL)
				pfree(DatumGetPointer(union_d));
			if (DatumGetPointer(inter_d) != NULL)
				pfree(DatumGetPointer(inter_d));

			/*
			 * are these a more promising split that what we've already
			 * seen?
			 */
			if (size_waste > waste || firsttime)
			{
				waste = size_waste;
				seed_1 = i;
				seed_2 = j;
				firsttime = false;
			}
		}
	}

	if (firsttime)
	{

		/*
		 * There is no possible split except to put the new item on its
		 * own page.  Since we still have to compute the union rectangles,
		 * we play dumb and run through the split algorithm anyway,
		 * setting seed_1 = first item on page and seed_2 = new item.
		 */
		seed_1 = FirstOffsetNumber;
		seed_2 = newitemoff;
	}

	item_1 = (IndexTuple) PageGetItem(page, PageGetItemId(page, seed_1));
	datum_alpha = IndexTupleGetDatum(item_1);
	datum_l = FunctionCall2(&rtstate->unionFn, datum_alpha, datum_alpha);
	FunctionCall2(&rtstate->sizeFn, datum_l, PointerGetDatum(&size_l));
	left_avail_space = RTPageAvailSpace - IndexTupleTotalSize(item_1);

	if (seed_2 == newitemoff)
	{
		item_2 = itup;
		/* Needn't leave room for new item in calculations below */
		newitemsz = 0;
	}
	else
		item_2 = (IndexTuple) PageGetItem(page, PageGetItemId(page, seed_2));
	datum_beta = IndexTupleGetDatum(item_2);
	datum_r = FunctionCall2(&rtstate->unionFn, datum_beta, datum_beta);
	FunctionCall2(&rtstate->sizeFn, datum_r, PointerGetDatum(&size_r));
	right_avail_space = RTPageAvailSpace - IndexTupleTotalSize(item_2);

	/*
	 * Now split up the regions between the two seeds.	An important
	 * property of this split algorithm is that the split vector v has the
	 * indices of items to be split in order in its left and right
	 * vectors.  We exploit this property by doing a merge in the code
	 * that actually splits the page.
	 *
	 * For efficiency, we also place the new index tuple in this loop. This
	 * is handled at the very end, when we have placed all the existing
	 * tuples and i == maxoff + 1.
	 */
	left = v->spl_left;
	v->spl_nleft = 0;
	right = v->spl_right;
	v->spl_nright = 0;

	for (i = FirstOffsetNumber; i <= newitemoff; i = OffsetNumberNext(i))
	{
		bool		left_feasible,
					right_feasible,
					choose_left;

		/*
		 * If we've already decided where to place this item, just put it
		 * on the correct list.  Otherwise, we need to figure out which
		 * page needs the least enlargement in order to store the item.
		 */

		if (i == seed_1)
		{
			*left++ = i;
			v->spl_nleft++;
			/* left avail_space & union already includes this one */
			continue;
		}
		if (i == seed_2)
		{
			*right++ = i;
			v->spl_nright++;
			/* right avail_space & union already includes this one */
			continue;
		}

		/* Compute new union datums and sizes for both possible additions */
		if (i == newitemoff)
		{
			item_1 = itup;
			/* Needn't leave room for new item anymore */
			newitemsz = 0;
		}
		else
			item_1 = (IndexTuple) PageGetItem(page, PageGetItemId(page, i));
		item_1_sz = IndexTupleTotalSize(item_1);

		datum_alpha = IndexTupleGetDatum(item_1);
		union_dl = FunctionCall2(&rtstate->unionFn, datum_l, datum_alpha);
		union_dr = FunctionCall2(&rtstate->unionFn, datum_r, datum_alpha);
		FunctionCall2(&rtstate->sizeFn, union_dl,
					  PointerGetDatum(&size_alpha));
		FunctionCall2(&rtstate->sizeFn, union_dr,
					  PointerGetDatum(&size_beta));

		/*
		 * We prefer the page that shows smaller enlargement of its union
		 * area (Guttman's algorithm), but we must take care that at least
		 * one page will still have room for the new item after this one
		 * is added.
		 *
		 * (We know that all the old items together can fit on one page, so
		 * we need not worry about any other problem than failing to fit
		 * the new item.)
		 */
		left_feasible = (left_avail_space >= item_1_sz &&
						 ((left_avail_space - item_1_sz) >= newitemsz ||
						  right_avail_space >= newitemsz));
		right_feasible = (right_avail_space >= item_1_sz &&
						  ((right_avail_space - item_1_sz) >= newitemsz ||
						   left_avail_space >= newitemsz));
		if (left_feasible && right_feasible)
		{
			/* Both feasible, use Guttman's algorithm */
			choose_left = (size_alpha - size_l < size_beta - size_r);
		}
		else if (left_feasible)
			choose_left = true;
		else if (right_feasible)
			choose_left = false;
		else
		{
			elog(ERROR, "rtpicksplit: failed to find a workable page split");
			choose_left = false;/* keep compiler quiet */
		}

		if (choose_left)
		{
			pfree(DatumGetPointer(datum_l));
			pfree(DatumGetPointer(union_dr));
			datum_l = union_dl;
			size_l = size_alpha;
			left_avail_space -= item_1_sz;
			*left++ = i;
			v->spl_nleft++;
		}
		else
		{
			pfree(DatumGetPointer(datum_r));
			pfree(DatumGetPointer(union_dl));
			datum_r = union_dr;
			size_r = size_beta;
			right_avail_space -= item_1_sz;
			*right++ = i;
			v->spl_nright++;
		}
	}

	*left = *right = InvalidOffsetNumber;		/* add ending sentinels */

	v->spl_ldatum = datum_l;
	v->spl_rdatum = datum_r;
}

static void
RTInitBuffer(Buffer b, uint32 f)
{
	RTreePageOpaque opaque;
	Page		page;
	Size		pageSize;

	pageSize = BufferGetPageSize(b);

	page = BufferGetPage(b);
	MemSet(page, 0, (int) pageSize);
	PageInit(page, pageSize, sizeof(RTreePageOpaqueData));

	opaque = (RTreePageOpaque) PageGetSpecialPointer(page);
	opaque->flags = f;
}

static OffsetNumber
choose(Relation r, Page p, IndexTuple it, RTSTATE *rtstate)
{
	OffsetNumber maxoff;
	OffsetNumber i;
	Datum		ud,
				id;
	Datum		datum;
	float		usize,
				dsize;
	OffsetNumber which;
	float		which_grow;

	id = IndexTupleGetDatum(it);
	maxoff = PageGetMaxOffsetNumber(p);
	which_grow = -1.0;
	which = -1;

	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		datum = IndexTupleGetDatum(PageGetItem(p, PageGetItemId(p, i)));
		FunctionCall2(&rtstate->sizeFn, datum,
					  PointerGetDatum(&dsize));
		ud = FunctionCall2(&rtstate->unionFn, datum, id);
		FunctionCall2(&rtstate->sizeFn, ud,
					  PointerGetDatum(&usize));
		pfree(DatumGetPointer(ud));
		if (which_grow < 0 || usize - dsize < which_grow)
		{
			which = i;
			which_grow = usize - dsize;
			if (which_grow == 0)
				break;
		}
	}

	return which;
}

static int
nospace(Page p, IndexTuple it)
{
	return PageGetFreeSpace(p) < IndexTupleSize(it);
}

void
freestack(RTSTACK *s)
{
	RTSTACK    *p;

	while (s != (RTSTACK *) NULL)
	{
		p = s->rts_parent;
		pfree(s);
		s = p;
	}
}

Datum
rtdelete(PG_FUNCTION_ARGS)
{
	Relation	r = (Relation) PG_GETARG_POINTER(0);
	ItemPointer tid = (ItemPointer) PG_GETARG_POINTER(1);
	BlockNumber blkno;
	OffsetNumber offnum;
	Buffer		buf;
	Page		page;

	/*
	 * Notes in ExecUtils:ExecOpenIndices() Also note that only vacuum
	 * deletes index tuples now...
	 *
	 * RelationSetLockForWrite(r);
	 */

	blkno = ItemPointerGetBlockNumber(tid);
	offnum = ItemPointerGetOffsetNumber(tid);

	/* adjust any scans that will be affected by this deletion */
	rtadjscans(r, RTOP_DEL, blkno, offnum);

	/* delete the index tuple */
	buf = ReadBuffer(r, blkno);
	page = BufferGetPage(buf);

	PageIndexTupleDelete(page, offnum);

	WriteBuffer(buf);

	PG_RETURN_VOID();
}

static void
initRtstate(RTSTATE *rtstate, Relation index)
{
	RegProcedure union_proc,
				size_proc,
				inter_proc;

	union_proc = index_getprocid(index, 1, RT_UNION_PROC);
	size_proc = index_getprocid(index, 1, RT_SIZE_PROC);
	inter_proc = index_getprocid(index, 1, RT_INTER_PROC);
	fmgr_info(union_proc, &rtstate->unionFn);
	fmgr_info(size_proc, &rtstate->sizeFn);
	fmgr_info(inter_proc, &rtstate->interFn);
	return;
}

#ifdef RTDEBUG

void
_rtdump(Relation r)
{
	Buffer		buf;
	Page		page;
	OffsetNumber offnum,
				maxoff;
	BlockNumber blkno;
	BlockNumber nblocks;
	RTreePageOpaque po;
	IndexTuple	itup;
	BlockNumber itblkno;
	OffsetNumber itoffno;
	Datum		datum;
	char	   *itkey;

	nblocks = RelationGetNumberOfBlocks(r);
	for (blkno = 0; blkno < nblocks; blkno++)
	{
		buf = ReadBuffer(r, blkno);
		page = BufferGetPage(buf);
		po = (RTreePageOpaque) PageGetSpecialPointer(page);
		maxoff = PageGetMaxOffsetNumber(page);
		printf("Page %d maxoff %d <%s>\n", blkno, maxoff,
			   (po->flags & F_LEAF ? "LEAF" : "INTERNAL"));

		if (PageIsEmpty(page))
		{
			ReleaseBuffer(buf);
			continue;
		}

		for (offnum = FirstOffsetNumber;
			 offnum <= maxoff;
			 offnum = OffsetNumberNext(offnum))
		{
			itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offnum));
			itblkno = ItemPointerGetBlockNumber(&(itup->t_tid));
			itoffno = ItemPointerGetOffsetNumber(&(itup->t_tid));
			datum = IndexTupleGetDatum(itup);
			itkey = DatumGetCString(DirectFunctionCall1(box_out,
														datum));
			printf("\t[%d] size %d heap <%d,%d> key:%s\n",
				   offnum, IndexTupleSize(itup), itblkno, itoffno, itkey);
			pfree(itkey);
		}

		ReleaseBuffer(buf);
	}
}

#endif	 /* defined RTDEBUG */

void
rtree_redo(XLogRecPtr lsn, XLogRecord *record)
{
	elog(STOP, "rtree_redo: unimplemented");
}

void
rtree_undo(XLogRecPtr lsn, XLogRecord *record)
{
	elog(STOP, "rtree_undo: unimplemented");
}

void
rtree_desc(char *buf, uint8 xl_info, char *rec)
{
}
