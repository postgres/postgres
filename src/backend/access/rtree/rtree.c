/*-------------------------------------------------------------------------
 *
 * rtree.c--
 *	  interface routines for the postgres rtree indexed access method.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/rtree/Attic/rtree.c,v 1.27 1998/09/01 03:21:27 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <access/genam.h>
#include <catalog/index.h>
#include <access/rtscan.h>
#include <storage/lmgr.h>
#include <access/rtree.h>
#include <storage/bufmgr.h>
#include <utils/geo_decls.h>
#include <executor/executor.h>
#include <access/heapam.h>
#include <fmgr.h>
#include <storage/bufpage.h>

#ifndef HAVE_MEMMOVE
#include <regex/utils.h>
#else
#include <string.h>
#endif

typedef struct SPLITVEC
{
	OffsetNumber *spl_left;
	int			spl_nleft;
	char	   *spl_ldatum;
	OffsetNumber *spl_right;
	int			spl_nright;
	char	   *spl_rdatum;
} SPLITVEC;

typedef struct RTSTATE
{
	FmgrInfo	unionFn;		/* union function */
	FmgrInfo	sizeFn;			/* size function */
	FmgrInfo	interFn;		/* intersection function */
} RTSTATE;

/* non-export function prototypes */
static InsertIndexResult
rtdoinsert(Relation r, IndexTuple itup,
		   RTSTATE *rtstate);
static void
rttighten(Relation r, RTSTACK *stk, char *datum, int att_size,
		  RTSTATE *rtstate);
static InsertIndexResult
dosplit(Relation r, Buffer buffer, RTSTACK *stack,
		IndexTuple itup, RTSTATE *rtstate);
static void
rtintinsert(Relation r, RTSTACK *stk, IndexTuple ltup,
			IndexTuple rtup, RTSTATE *rtstate);
static void rtnewroot(Relation r, IndexTuple lt, IndexTuple rt);
static void
picksplit(Relation r, Page page, SPLITVEC *v, IndexTuple itup,
		  RTSTATE *rtstate);
static void RTInitBuffer(Buffer b, uint32 f);
static OffsetNumber
choose(Relation r, Page p, IndexTuple it,
	   RTSTATE *rtstate);
static int	nospace(Page p, IndexTuple it);
static void initRtstate(RTSTATE *rtstate, Relation index);


void
rtbuild(Relation heap,
		Relation index,
		int natts,
		AttrNumber *attnum,
		IndexStrategy istrat,
		uint16 pcount,
		Datum *params,
		FuncIndexInfo *finfo,
		PredInfo *predInfo)
{
	HeapScanDesc scan;
	AttrNumber	i;
	HeapTuple	htup;
	IndexTuple	itup;
	TupleDesc	hd,
				id;
	InsertIndexResult res;
	Datum	   *d;
	bool	   *nulls;
	Buffer		buffer = InvalidBuffer;
	int			nb,
				nh,
				ni;

#ifndef OMIT_PARTIAL_INDEX
	ExprContext *econtext;
	TupleTable	tupleTable;
	TupleTableSlot *slot;

#endif
	Oid			hrelid,
				irelid;
	Node	   *pred,
			   *oldPred;
	RTSTATE		rtState;

	initRtstate(&rtState, index);

	/* rtrees only know how to do stupid locking now */
	RelationSetLockForWrite(index);

	pred = predInfo->pred;
	oldPred = predInfo->oldPred;

	/*
	 * We expect to be called exactly once for any index relation. If
	 * that's not the case, big trouble's what we have.
	 */

	if (oldPred == NULL && (nb = RelationGetNumberOfBlocks(index)) != 0)
		elog(ERROR, "%s already contains data", index->rd_rel->relname.data);

	/* initialize the root page (if this is a new index) */
	if (oldPred == NULL)
	{
		buffer = ReadBuffer(index, P_NEW);
		RTInitBuffer(buffer, F_LEAF);
		WriteBuffer(buffer);
	}

	/* init the tuple descriptors and get set for a heap scan */
	hd = RelationGetDescr(heap);
	id = RelationGetDescr(index);
	d = (Datum *) palloc(natts * sizeof(*d));
	nulls = (bool *) palloc(natts * sizeof(*nulls));

	/*
	 * If this is a predicate (partial) index, we will need to evaluate
	 * the predicate using ExecQual, which requires the current tuple to
	 * be in a slot of a TupleTable.  In addition, ExecQual must have an
	 * ExprContext referring to that slot.	Here, we initialize dummy
	 * TupleTable and ExprContext objects for this purpose. --Nels, Feb
	 * '92
	 */
#ifndef OMIT_PARTIAL_INDEX
	if (pred != NULL || oldPred != NULL)
	{
		tupleTable = ExecCreateTupleTable(1);
		slot = ExecAllocTableSlot(tupleTable);
		econtext = makeNode(ExprContext);
		FillDummyExprContext(econtext, slot, hd, buffer);
	}
	else
	{
		econtext = NULL;
		tupleTable = NULL;
		slot = NULL;
	}
#endif							/* OMIT_PARTIAL_INDEX */

	/* count the tuples as we insert them */
	nh = ni = 0;

 	scan = heap_beginscan(heap, 0, SnapshotNow, 0, (ScanKey) NULL);

	while (HeapTupleIsValid(htup = heap_getnext(scan, 0)))
	{
		nh++;

		/*
		 * If oldPred != NULL, this is an EXTEND INDEX command, so skip
		 * this tuple if it was already in the existing partial index
		 */
		if (oldPred != NULL)
		{
#ifndef OMIT_PARTIAL_INDEX
			/* SetSlotContents(slot, htup); */
			slot->val = htup;
			if (ExecQual((List *) oldPred, econtext) == true)
			{
				ni++;
				continue;
			}
#endif							/* OMIT_PARTIAL_INDEX */
		}

		/*
		 * Skip this tuple if it doesn't satisfy the partial-index
		 * predicate
		 */
		if (pred != NULL)
		{
#ifndef OMIT_PARTIAL_INDEX
			/* SetSlotContents(slot, htup); */
			slot->val = htup;
			if (ExecQual((List *) pred, econtext) == false)
				continue;
#endif							/* OMIT_PARTIAL_INDEX */
		}

		ni++;

		/*
		 * For the current heap tuple, extract all the attributes we use
		 * in this index, and note which are null.
		 */

		for (i = 1; i <= natts; i++)
		{
			int			attoff;
			bool		attnull;

			/*
			 * Offsets are from the start of the tuple, and are
			 * zero-based; indices are one-based.  The next call returns i
			 * - 1.  That's data hiding for you.
			 */

			attoff = AttrNumberGetAttrOffset(i);

			/*
			 * d[attoff] = HeapTupleGetAttributeValue(htup, buffer,
			 */
			d[attoff] = GetIndexValue(htup,
									  hd,
									  attoff,
									  attnum,
									  finfo,
									  &attnull);
			nulls[attoff] = (attnull ? 'n' : ' ');
		}

		/* form an index tuple and point it at the heap tuple */
		itup = index_formtuple(id, &d[0], nulls);
		itup->t_tid = htup->t_ctid;

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
	heap_endscan(scan);
	RelationUnsetLockForWrite(index);

	if (pred != NULL || oldPred != NULL)
	{
#ifndef OMIT_PARTIAL_INDEX
		ExecDestroyTupleTable(tupleTable, true);
		pfree(econtext);
#endif							/* OMIT_PARTIAL_INDEX */
	}

	/*
	 * Since we just counted the tuples in the heap, we update its stats
	 * in pg_relation to guarantee that the planner takes advantage of the
	 * index we just created.  UpdateStats() does a
	 * CommandCounterIncrement(), which flushes changed entries from the
	 * system relcache.  The act of constructing an index changes these
	 * heap and index tuples in the system catalogs, so they need to be
	 * flushed.  We close them to guarantee that they will be.
	 */

	hrelid = RelationGetRelid(heap);
	irelid = RelationGetRelid(index);
	heap_close(heap);
	index_close(index);

	UpdateStats(hrelid, nh, true);
	UpdateStats(irelid, ni, false);

	if (oldPred != NULL)
	{
		if (ni == nh)
			pred = NULL;
		UpdateIndexPredicate(irelid, oldPred, pred);
	}

	/* be tidy */
	pfree(nulls);
	pfree(d);
}

/*
 *	rtinsert -- wrapper for rtree tuple insertion.
 *
 *	  This is the public interface routine for tuple insertion in rtrees.
 *	  It doesn't do any work; just locks the relation and passes the buck.
 */
InsertIndexResult
rtinsert(Relation r, Datum *datum, char *nulls, ItemPointer ht_ctid, Relation heapRel)
{
	InsertIndexResult res;
	IndexTuple	itup;
	RTSTATE		rtState;

	/* generate an index tuple */
	itup = index_formtuple(RelationGetDescr(r), datum, nulls);
	itup->t_tid = *ht_ctid;
	initRtstate(&rtState, r);

	RelationSetLockForWrite(r);
	res = rtdoinsert(r, itup, &rtState);

	/* XXX two-phase locking -- don't unlock the relation until EOT */
	return res;
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
	char	   *datum;

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
		res = dosplit(r, buffer, stack, itup, rtstate);
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

	WriteBuffer(buffer);

	datum = (((char *) itup) + sizeof(IndexTupleData));

	/* now expand the page boundary in the parent to include the new child */
	rttighten(r, stack, datum,
			  (IndexTupleSize(itup) - sizeof(IndexTupleData)), rtstate);
	freestack(stack);

	/* build and return an InsertIndexResult for this insertion */
	res = (InsertIndexResult) palloc(sizeof(InsertIndexResultData));
	ItemPointerSet(&(res->pointerData), blk, l);

	return res;
}

static void
rttighten(Relation r,
		  RTSTACK *stk,
		  char *datum,
		  int att_size,
		  RTSTATE *rtstate)
{
	char	   *oldud;
	char	   *tdatum;
	Page		p;
	float		old_size,
				newd_size;
	Buffer		b;

	if (stk == (RTSTACK *) NULL)
		return;

	b = ReadBuffer(r, stk->rts_blk);
	p = BufferGetPage(b);

	oldud = (char *) PageGetItem(p, PageGetItemId(p, stk->rts_child));
	oldud += sizeof(IndexTupleData);

	(*fmgr_faddr(&rtstate->sizeFn)) (oldud, &old_size);
	datum = (char *) (*fmgr_faddr(&rtstate->unionFn)) (oldud, datum);

	(*fmgr_faddr(&rtstate->sizeFn)) (datum, &newd_size);

	if (newd_size != old_size)
	{
		TupleDesc	td = RelationGetDescr(r);

		if (td->attrs[0]->attlen < 0)
		{

			/*
			 * This is an internal page, so 'oldud' had better be a union
			 * (constant-length) key, too.	(See comment below.)
			 */
			Assert(VARSIZE(datum) == VARSIZE(oldud));
			memmove(oldud, datum, VARSIZE(datum));
		}
		else
			memmove(oldud, datum, att_size);
		WriteBuffer(b);

		/*
		 * The user may be defining an index on variable-sized data (like
		 * polygons).  If so, we need to get a constant-sized datum for
		 * insertion on the internal page.	We do this by calling the
		 * union proc, which is guaranteed to return a rectangle.
		 */

		tdatum = (char *) (*fmgr_faddr(&rtstate->unionFn)) (datum, datum);
		rttighten(r, stk->rts_parent, tdatum, att_size, rtstate);
		pfree(tdatum);
	}
	else
		ReleaseBuffer(b);
	pfree(datum);
}

/*
 *	dosplit -- split a page in the tree.
 *
 *	  This is the quadratic-cost split algorithm Guttman describes in
 *	  his paper.  The reason we chose it is that you can implement this
 *	  with less information about the data types on which you're operating.
 */
static InsertIndexResult
dosplit(Relation r,
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
	TupleDesc	tupDesc;

	isnull = (char *) palloc(r->rd_rel->relnatts);
	for (blank = 0; blank < r->rd_rel->relnatts; blank++)
		isnull[blank] = ' ';
	p = (Page) BufferGetPage(buffer);
	opaque = (RTreePageOpaque) PageGetSpecialPointer(p);

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

	picksplit(r, p, &v, itup, rtstate);

	leftoff = rightoff = FirstOffsetNumber;
	maxoff = PageGetMaxOffsetNumber(p);
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		itemid = PageGetItemId(p, i);
		item = (IndexTuple) PageGetItem(p, itemid);

		if (i == *(v.spl_left))
		{
			PageAddItem(left, (Item) item, IndexTupleSize(item),
						leftoff, LP_USED);
			leftoff = OffsetNumberNext(leftoff);
			v.spl_left++;		/* advance in left split vector */
		}
		else
		{
			PageAddItem(right, (Item) item, IndexTupleSize(item),
						rightoff, LP_USED);
			rightoff = OffsetNumberNext(rightoff);
			v.spl_right++;		/* advance in right split vector */
		}
	}

	/* build an InsertIndexResult for this insertion */
	res = (InsertIndexResult) palloc(sizeof(InsertIndexResultData));

	/* now insert the new index tuple */
	if (*(v.spl_left) != FirstOffsetNumber)
	{
		PageAddItem(left, (Item) itup, IndexTupleSize(itup),
					leftoff, LP_USED);
		leftoff = OffsetNumberNext(leftoff);
		ItemPointerSet(&(res->pointerData), lbknum, leftoff);
	}
	else
	{
		PageAddItem(right, (Item) itup, IndexTupleSize(itup),
					rightoff, LP_USED);
		rightoff = OffsetNumberNext(rightoff);
		ItemPointerSet(&(res->pointerData), rbknum, rightoff);
	}

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
	ltup = (IndexTuple) index_formtuple(tupDesc,
									  (Datum *) &(v.spl_ldatum), isnull);
	rtup = (IndexTuple) index_formtuple(tupDesc,
									  (Datum *) &(v.spl_rdatum), isnull);
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
	char	   *ldatum,
			   *rdatum,
			   *newdatum;
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
	 * This is a hack.	Right now, we force rtree keys to be constant
	 * size. To fix this, need delete the old key and add both left and
	 * right for the two new pages.  The insertion of left may force a
	 * split if the new left key is bigger than the old key.
	 */

	if (IndexTupleSize(old) != IndexTupleSize(ltup))
		elog(ERROR, "Variable-length rtree keys are not supported.");

	/* install pointer to left child */
	memmove(old, ltup, IndexTupleSize(ltup));

	if (nospace(p, rtup))
	{
		newdatum = (((char *) ltup) + sizeof(IndexTupleData));
		rttighten(r, stk->rts_parent, newdatum,
			   (IndexTupleSize(ltup) - sizeof(IndexTupleData)), rtstate);
		res = dosplit(r, b, stk->rts_parent, rtup, rtstate);
		WriteBuffer(b);			/* don't forget to release buffer!  -
								 * 01/31/94 */
		pfree(res);
	}
	else
	{
		PageAddItem(p, (Item) rtup, IndexTupleSize(rtup),
					PageGetMaxOffsetNumber(p), LP_USED);
		WriteBuffer(b);
		ldatum = (((char *) ltup) + sizeof(IndexTupleData));
		rdatum = (((char *) rtup) + sizeof(IndexTupleData));
		newdatum = (char *) (*fmgr_faddr(&rtstate->unionFn)) (ldatum, rdatum);

		rttighten(r, stk->rts_parent, newdatum,
			   (IndexTupleSize(rtup) - sizeof(IndexTupleData)), rtstate);

		pfree(newdatum);
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
	PageAddItem(p, (Item) lt, IndexTupleSize(lt),
				FirstOffsetNumber, LP_USED);
	PageAddItem(p, (Item) rt, IndexTupleSize(rt),
				OffsetNumberNext(FirstOffsetNumber), LP_USED);
	WriteBuffer(b);
}

static void
picksplit(Relation r,
		  Page page,
		  SPLITVEC *v,
		  IndexTuple itup,
		  RTSTATE *rtstate)
{
	OffsetNumber maxoff;
	OffsetNumber i,
				j;
	IndexTuple	item_1,
				item_2;
	char	   *datum_alpha,
			   *datum_beta;
	char	   *datum_l,
			   *datum_r;
	char	   *union_d,
			   *union_dl,
			   *union_dr;
	char	   *inter_d;
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

	maxoff = PageGetMaxOffsetNumber(page);

	nbytes = (maxoff + 2) * sizeof(OffsetNumber);
	v->spl_left = (OffsetNumber *) palloc(nbytes);
	v->spl_right = (OffsetNumber *) palloc(nbytes);

	firsttime = true;
	waste = 0.0;

	for (i = FirstOffsetNumber; i < maxoff; i = OffsetNumberNext(i))
	{
		item_1 = (IndexTuple) PageGetItem(page, PageGetItemId(page, i));
		datum_alpha = ((char *) item_1) + sizeof(IndexTupleData);
		for (j = OffsetNumberNext(i); j <= maxoff; j = OffsetNumberNext(j))
		{
			item_2 = (IndexTuple) PageGetItem(page, PageGetItemId(page, j));
			datum_beta = ((char *) item_2) + sizeof(IndexTupleData);

			/* compute the wasted space by unioning these guys */
			union_d = (char *) (*fmgr_faddr(&rtstate->unionFn)) (datum_alpha, datum_beta);
			(*fmgr_faddr(&rtstate->sizeFn)) (union_d, &size_union);
			inter_d = (char *) (*fmgr_faddr(&rtstate->interFn)) (datum_alpha, datum_beta);
			(*fmgr_faddr(&rtstate->sizeFn)) (inter_d, &size_inter);
			size_waste = size_union - size_inter;

			pfree(union_d);

			if (inter_d != (char *) NULL)
				pfree(inter_d);

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

	left = v->spl_left;
	v->spl_nleft = 0;
	right = v->spl_right;
	v->spl_nright = 0;

	item_1 = (IndexTuple) PageGetItem(page, PageGetItemId(page, seed_1));
	datum_alpha = ((char *) item_1) + sizeof(IndexTupleData);
	datum_l = (char *) (*fmgr_faddr(&rtstate->unionFn)) (datum_alpha, datum_alpha);
	(*fmgr_faddr(&rtstate->sizeFn)) (datum_l, &size_l);
	item_2 = (IndexTuple) PageGetItem(page, PageGetItemId(page, seed_2));
	datum_beta = ((char *) item_2) + sizeof(IndexTupleData);
	datum_r = (char *) (*fmgr_faddr(&rtstate->unionFn)) (datum_beta, datum_beta);
	(*fmgr_faddr(&rtstate->sizeFn)) (datum_r, &size_r);

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

	maxoff = OffsetNumberNext(maxoff);
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{

		/*
		 * If we've already decided where to place this item, just put it
		 * on the right list.  Otherwise, we need to figure out which page
		 * needs the least enlargement in order to store the item.
		 */

		if (i == seed_1)
		{
			*left++ = i;
			v->spl_nleft++;
			continue;
		}
		else if (i == seed_2)
		{
			*right++ = i;
			v->spl_nright++;
			continue;
		}

		/* okay, which page needs least enlargement? */
		if (i == maxoff)
			item_1 = itup;
		else
			item_1 = (IndexTuple) PageGetItem(page, PageGetItemId(page, i));

		datum_alpha = ((char *) item_1) + sizeof(IndexTupleData);
		union_dl = (char *) (*fmgr_faddr(&rtstate->unionFn)) (datum_l, datum_alpha);
		union_dr = (char *) (*fmgr_faddr(&rtstate->unionFn)) (datum_r, datum_alpha);
		(*fmgr_faddr(&rtstate->sizeFn)) (union_dl, &size_alpha);
		(*fmgr_faddr(&rtstate->sizeFn)) (union_dr, &size_beta);

		/* pick which page to add it to */
		if (size_alpha - size_l < size_beta - size_r)
		{
			pfree(datum_l);
			pfree(union_dr);
			datum_l = union_dl;
			size_l = size_alpha;
			*left++ = i;
			v->spl_nleft++;
		}
		else
		{
			pfree(datum_r);
			pfree(union_dl);
			datum_r = union_dr;
			size_r = size_alpha;
			*right++ = i;
			v->spl_nright++;
		}
	}
	*left = *right = FirstOffsetNumber; /* sentinel value, see dosplit() */

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
	char	   *ud,
			   *id;
	char	   *datum;
	float		usize,
				dsize;
	OffsetNumber which;
	float		which_grow;

	id = ((char *) it) + sizeof(IndexTupleData);
	maxoff = PageGetMaxOffsetNumber(p);
	which_grow = -1.0;
	which = -1;

	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		datum = (char *) PageGetItem(p, PageGetItemId(p, i));
		datum += sizeof(IndexTupleData);
		(*fmgr_faddr(&rtstate->sizeFn)) (datum, &dsize);
		ud = (char *) (*fmgr_faddr(&rtstate->unionFn)) (datum, id);
		(*fmgr_faddr(&rtstate->sizeFn)) (ud, &usize);
		pfree(ud);
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

char *
rtdelete(Relation r, ItemPointer tid)
{
	BlockNumber blkno;
	OffsetNumber offnum;
	Buffer		buf;
	Page		page;

	/* must write-lock on delete */
	RelationSetLockForWrite(r);

	blkno = ItemPointerGetBlockNumber(tid);
	offnum = ItemPointerGetOffsetNumber(tid);

	/* adjust any scans that will be affected by this deletion */
	rtadjscans(r, RTOP_DEL, blkno, offnum);

	/* delete the index tuple */
	buf = ReadBuffer(r, blkno);
	page = BufferGetPage(buf);

	PageIndexTupleDelete(page, offnum);

	WriteBuffer(buf);

	/* XXX -- two-phase locking, don't release the write lock */
	return (char *) NULL;
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
	char	   *datum;
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
			datum = ((char *) itup);
			datum += sizeof(IndexTupleData);
			itkey = (char *) box_out((BOX *) datum);
			printf("\t[%d] size %d heap <%d,%d> key:%s\n",
				   offnum, IndexTupleSize(itup), itblkno, itoffno, itkey);
			pfree(itkey);
		}

		ReleaseBuffer(buf);
	}
}

#endif							/* defined RTDEBUG */
