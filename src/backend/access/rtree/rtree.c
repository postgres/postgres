/*-------------------------------------------------------------------------
 *
 * rtree.c
 *	  interface routines for the postgres rtree indexed access method.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/rtree/Attic/rtree.c,v 1.80.2.1 2005/01/24 02:48:15 tgl Exp $
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

/* for sorting tuples by cost, for picking split */
typedef struct SPLITCOST
{
	OffsetNumber offset_number;
	float		cost_differential;
	bool		choose_left;
} SPLITCOST;

typedef struct RTSTATE
{
	FmgrInfo	unionFn;		/* union function */
	FmgrInfo	sizeFn;			/* size function */
	FmgrInfo	interFn;		/* intersection function */
} RTSTATE;

/* Working state for rtbuild and its callback */
typedef struct
{
	RTSTATE		rtState;
	double		indtuples;
} RTBuildState;

/* non-export function prototypes */
static void rtbuildCallback(Relation index,
				HeapTuple htup,
				Datum *attdata,
				char *nulls,
				bool tupleIsAlive,
				void *state);
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
static int	qsort_comp_splitcost(const void *a, const void *b);


/*
 * routine to build an index.  Basically calls insert over and over
 */
Datum
rtbuild(PG_FUNCTION_ARGS)
{
	Relation	heap = (Relation) PG_GETARG_POINTER(0);
	Relation	index = (Relation) PG_GETARG_POINTER(1);
	IndexInfo  *indexInfo = (IndexInfo *) PG_GETARG_POINTER(2);
	double		reltuples;
	RTBuildState buildstate;
	Buffer		buffer;

	/* no locking is needed */

	initRtstate(&buildstate.rtState, index);

	/*
	 * We expect to be called exactly once for any index relation. If
	 * that's not the case, big trouble's what we have.
	 */
	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	/* initialize the root page */
	buffer = ReadBuffer(index, P_NEW);
	RTInitBuffer(buffer, F_LEAF);
	WriteBuffer(buffer);

	/* build the index */
	buildstate.indtuples = 0;

	/* do the heap scan */
	reltuples = IndexBuildHeapScan(heap, index, indexInfo,
								   rtbuildCallback, (void *) &buildstate);

	/* okay, all heap tuples are indexed */

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
		UpdateStats(hrelid, reltuples);
		UpdateStats(irelid, buildstate.indtuples);
	}

	PG_RETURN_VOID();
}

/*
 * Per-tuple callback from IndexBuildHeapScan
 */
static void
rtbuildCallback(Relation index,
				HeapTuple htup,
				Datum *attdata,
				char *nulls,
				bool tupleIsAlive,
				void *state)
{
	RTBuildState *buildstate = (RTBuildState *) state;
	IndexTuple	itup;
	InsertIndexResult res;

	/* form an index tuple and point it at the heap tuple */
	itup = index_formtuple(RelationGetDescr(index), attdata, nulls);
	itup->t_tid = htup->t_self;

	/* rtree indexes don't index nulls, see notes in rtinsert */
	if (IndexTupleHasNulls(itup))
	{
		pfree(itup);
		return;
	}

	/*
	 * Since we already have the index relation locked, we call rtdoinsert
	 * directly.  Normal access method calls dispatch through rtinsert,
	 * which locks the relation for write.	This is the right thing to do
	 * if you're inserting single tups, but not when you're initializing
	 * the whole index at once.
	 */
	res = rtdoinsert(index, itup, &buildstate->rtState);

	if (res)
		pfree(res);

	buildstate->indtuples += 1;

	pfree(itup);
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
	bool		checkUnique = PG_GETARG_BOOL(5);
#endif
	InsertIndexResult res;
	IndexTuple	itup;
	RTSTATE		rtState;

	/* generate an index tuple */
	itup = index_formtuple(RelationGetDescr(r), datum, nulls);
	itup->t_tid = *ht_ctid;

	/*
	 * Currently, rtrees do not support indexing NULLs; considerable
	 * infrastructure work would have to be done to do anything reasonable
	 * with a NULL.
	 */
	if (IndexTupleHasNulls(itup))
	{
		pfree(itup);
		PG_RETURN_POINTER((InsertIndexResult) NULL);
	}

	initRtstate(&rtState, r);

	/*
	 * Since rtree is not marked "amconcurrent" in pg_am, caller should
	 * have acquired exclusive lock on index relation.	We need no locking
	 * here.
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
		elog(ERROR, "failed to add index item to \"%s\"",
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

	/*
	 * If newd_size == 0 we have degenerate rectangles, so we don't know
	 * if there was any change, so we have to assume there was.
	 */
	if ((newd_size == 0) || (newd_size != old_size))
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
	int			n;
	OffsetNumber newitemoff;

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
	newitemoff = OffsetNumberNext(maxoff);

	/* build an InsertIndexResult for this insertion */
	res = (InsertIndexResult) palloc(sizeof(InsertIndexResultData));

	/*
	 * spl_left contains a list of the offset numbers of the tuples that
	 * will go to the left page.  For each offset number, get the tuple
	 * item, then add the item to the left page.  Similarly for the right
	 * side.
	 */

	/* fill left node */
	for (n = 0; n < v.spl_nleft; n++)
	{
		i = *spl_left;
		if (i == newitemoff)
			item = itup;
		else
		{
			itemid = PageGetItemId(p, i);
			item = (IndexTuple) PageGetItem(p, itemid);
		}

		if (PageAddItem(left, (Item) item, IndexTupleSize(item),
						leftoff, LP_USED) == InvalidOffsetNumber)
			elog(ERROR, "failed to add index item to \"%s\"",
				 RelationGetRelationName(r));
		leftoff = OffsetNumberNext(leftoff);

		if (i == newitemoff)
			ItemPointerSet(&(res->pointerData), lbknum, leftoff);

		spl_left++;				/* advance in left split vector */
	}

	/* fill right node */
	for (n = 0; n < v.spl_nright; n++)
	{
		i = *spl_right;
		if (i == newitemoff)
			item = itup;
		else
		{
			itemid = PageGetItemId(p, i);
			item = (IndexTuple) PageGetItem(p, itemid);
		}

		if (PageAddItem(right, (Item) item, IndexTupleSize(item),
						rightoff, LP_USED) == InvalidOffsetNumber)
			elog(ERROR, "failed to add index item to \"%s\"",
				 RelationGetRelationName(r));
		rightoff = OffsetNumberNext(rightoff);

		if (i == newitemoff)
			ItemPointerSet(&(res->pointerData), rbknum, rightoff);

		spl_right++;			/* advance in right split vector */
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
	pfree(DatumGetPointer(v.spl_ldatum));
	pfree(DatumGetPointer(v.spl_rdatum));

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
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("variable-length rtree keys are not supported")));

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
			elog(ERROR, "failed to add index item to \"%s\"",
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
		elog(ERROR, "failed to add index item to \"%s\"",
			 RelationGetRelationName(r));
	if (PageAddItem(p, (Item) rt, IndexTupleSize(rt),
					OffsetNumberNext(FirstOffsetNumber),
					LP_USED) == InvalidOffsetNumber)
		elog(ERROR, "failed to add index item to \"%s\"",
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
 * Both vectors have a terminating sentinel value of InvalidOffsetNumber,
 * but the sentinal value is no longer used, because the SPLITVEC
 * vector also contains the length of each vector, and that information
 * is now used to iterate over them in rtdosplit(). --kbb, 21 Sept 2001
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
	int			total_num_tuples,
				num_tuples_without_seeds,
				max_after_split;	/* in Guttman's lingo, (M - m) */
	float		diff;			/* diff between cost of putting tuple left
								 * or right */
	SPLITCOST  *cost_vector;
	int			n;

	/*
	 * First, make sure the new item is not so large that we can't
	 * possibly fit it on a page, even by itself.  (It's sufficient to
	 * make this test here, since any oversize tuple must lead to a page
	 * split attempt.)
	 */
	newitemsz = IndexTupleTotalSize(itup);
	if (newitemsz > RTPageAvailSpace)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("index row size %lu exceeds rtree maximum, %lu",
						(unsigned long) newitemsz,
						(unsigned long) RTPageAvailSpace)));

	maxoff = PageGetMaxOffsetNumber(page);
	newitemoff = OffsetNumberNext(maxoff);		/* phony index for new
												 * item */
	total_num_tuples = newitemoff;
	num_tuples_without_seeds = total_num_tuples - 2;
	max_after_split = total_num_tuples / 2;		/* works for m = M/2 */

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
	 * Now split up the regions between the two seeds.
	 *
	 * The cost_vector array will contain hints for determining where each
	 * tuple should go.  Each record in the array will contain a boolean,
	 * choose_left, that indicates which node the tuple prefers to be on,
	 * and the absolute difference in cost between putting the tuple in
	 * its favored node and in the other node.
	 *
	 * Later, we will sort the cost_vector in descending order by cost
	 * difference, and consider the tuples in that order for placement.
	 * That way, the tuples that *really* want to be in one node or the
	 * other get to choose first, and the tuples that don't really care
	 * choose last.
	 *
	 * First, build the cost_vector array.	The new index tuple will also be
	 * handled in this loop, and represented in the array, with
	 * i==newitemoff.
	 *
	 * In the case of variable size tuples it is possible that we only have
	 * the two seeds and no other tuples, in which case we don't do any of
	 * this cost_vector stuff.
	 */

	/* to keep compiler quiet */
	cost_vector = (SPLITCOST *) NULL;

	if (num_tuples_without_seeds > 0)
	{
		cost_vector =
			(SPLITCOST *) palloc(num_tuples_without_seeds * sizeof(SPLITCOST));
		n = 0;
		for (i = FirstOffsetNumber; i <= newitemoff; i = OffsetNumberNext(i))
		{
			/* Compute new union datums and sizes for both choices */

			if ((i == seed_1) || (i == seed_2))
				continue;
			else if (i == newitemoff)
				item_1 = itup;
			else
				item_1 = (IndexTuple) PageGetItem(page, PageGetItemId(page, i));

			datum_alpha = IndexTupleGetDatum(item_1);
			union_dl = FunctionCall2(&rtstate->unionFn, datum_l, datum_alpha);
			union_dr = FunctionCall2(&rtstate->unionFn, datum_r, datum_alpha);
			FunctionCall2(&rtstate->sizeFn, union_dl,
						  PointerGetDatum(&size_alpha));
			FunctionCall2(&rtstate->sizeFn, union_dr,
						  PointerGetDatum(&size_beta));
			pfree(DatumGetPointer(union_dl));
			pfree(DatumGetPointer(union_dr));

			diff = (size_alpha - size_l) - (size_beta - size_r);

			cost_vector[n].offset_number = i;
			cost_vector[n].cost_differential = fabs(diff);
			cost_vector[n].choose_left = (diff < 0);

			n++;
		}

		/*
		 * Sort the array.	The function qsort_comp_splitcost is set up
		 * "backwards", to provided descending order.
		 */
		qsort(cost_vector, num_tuples_without_seeds, sizeof(SPLITCOST),
			  &qsort_comp_splitcost);
	}

	/*
	 * Now make the final decisions about where each tuple will go, and
	 * build the vectors to return in the SPLITVEC record.
	 *
	 * The cost_vector array contains (descriptions of) all the tuples, in
	 * the order that we want to consider them, so we we just iterate
	 * through it and place each tuple in left or right nodes, according
	 * to the criteria described below.
	 */

	left = v->spl_left;
	v->spl_nleft = 0;
	right = v->spl_right;
	v->spl_nright = 0;

	/*
	 * Place the seeds first. left avail space, left union, right avail
	 * space, and right union have already been adjusted for the seeds.
	 */

	*left++ = seed_1;
	v->spl_nleft++;

	*right++ = seed_2;
	v->spl_nright++;

	for (n = 0; n < num_tuples_without_seeds; n++)
	{
		bool		left_feasible,
					right_feasible,
					choose_left;

		/*
		 * We need to figure out which page needs the least enlargement in
		 * order to store the item.
		 */

		i = cost_vector[n].offset_number;

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
		 *
		 * Guttman's algorithm actually has two factors to consider (in
		 * order):	1. if one node has so many tuples already assigned to
		 * it that the other needs all the rest in order to satisfy the
		 * condition that neither node has fewer than m tuples, then that
		 * is decisive; 2. otherwise, choose the page that shows the
		 * smaller enlargement of its union area.
		 *
		 * I have chosen m = M/2, where M is the maximum number of tuples on
		 * a page.	(Actually, this is only strictly true for fixed size
		 * tuples.	For variable size tuples, there still might have to be
		 * only one tuple on a page, if it is really big.  But even with
		 * variable size tuples we still try to get m as close as possible
		 * to M/2.)
		 *
		 * The question of which page shows the smaller enlargement of its
		 * union area has already been answered, and the answer stored in
		 * the choose_left field of the SPLITCOST record.
		 */
		left_feasible = (left_avail_space >= item_1_sz &&
						 ((left_avail_space - item_1_sz) >= newitemsz ||
						  right_avail_space >= newitemsz));
		right_feasible = (right_avail_space >= item_1_sz &&
						  ((right_avail_space - item_1_sz) >= newitemsz ||
						   left_avail_space >= newitemsz));
		if (left_feasible && right_feasible)
		{
			/*
			 * Both feasible, use Guttman's algorithm. First check the m
			 * condition described above, and if that doesn't apply,
			 * choose the page with the smaller enlargement of its union
			 * area.
			 */
			if (v->spl_nleft > max_after_split)
				choose_left = false;
			else if (v->spl_nright > max_after_split)
				choose_left = true;
			else
				choose_left = cost_vector[n].choose_left;
		}
		else if (left_feasible)
			choose_left = true;
		else if (right_feasible)
			choose_left = false;
		else
		{
			elog(ERROR, "failed to find a workable rtree page split");
			choose_left = false;	/* keep compiler quiet */
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

	if (num_tuples_without_seeds > 0)
		pfree(cost_vector);

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

/*
 * Bulk deletion of all index entries pointing to a set of heap tuples.
 * The set of target tuples is specified via a callback routine that tells
 * whether any given heap tuple (identified by ItemPointer) is being deleted.
 *
 * Result: a palloc'd struct containing statistical info for VACUUM displays.
 */
Datum
rtbulkdelete(PG_FUNCTION_ARGS)
{
	Relation	rel = (Relation) PG_GETARG_POINTER(0);
	IndexBulkDeleteCallback callback = (IndexBulkDeleteCallback) PG_GETARG_POINTER(1);
	void	   *callback_state = (void *) PG_GETARG_POINTER(2);
	IndexBulkDeleteResult *result;
	BlockNumber num_pages;
	double		tuples_removed;
	double		num_index_tuples;
	IndexScanDesc iscan;

	tuples_removed = 0;
	num_index_tuples = 0;

	/*
	 * Since rtree is not marked "amconcurrent" in pg_am, caller should
	 * have acquired exclusive lock on index relation.	We need no locking
	 * here.
	 */

	/*
	 * XXX generic implementation --- should be improved!
	 */

	/* walk through the entire index */
	iscan = index_beginscan(NULL, rel, SnapshotAny, 0, (ScanKey) NULL);
	/* including killed tuples */
	iscan->ignore_killed_tuples = false;

	while (index_getnext_indexitem(iscan, ForwardScanDirection))
	{
		if (callback(&iscan->xs_ctup.t_self, callback_state))
		{
			ItemPointerData indextup = iscan->currentItemData;
			BlockNumber blkno;
			OffsetNumber offnum;
			Buffer		buf;
			Page		page;

			blkno = ItemPointerGetBlockNumber(&indextup);
			offnum = ItemPointerGetOffsetNumber(&indextup);

			/* adjust any scans that will be affected by this deletion */
			/* (namely, my own scan) */
			rtadjscans(rel, RTOP_DEL, blkno, offnum);

			/* delete the index tuple */
			buf = ReadBuffer(rel, blkno);
			page = BufferGetPage(buf);

			PageIndexTupleDelete(page, offnum);

			WriteBuffer(buf);

			tuples_removed += 1;
		}
		else
			num_index_tuples += 1;
	}

	index_endscan(iscan);

	/* return statistics */
	num_pages = RelationGetNumberOfBlocks(rel);

	result = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
	result->num_pages = num_pages;
	result->num_index_tuples = num_index_tuples;
	result->tuples_removed = tuples_removed;

	PG_RETURN_POINTER(result);
}


static void
initRtstate(RTSTATE *rtstate, Relation index)
{
	fmgr_info_copy(&rtstate->unionFn,
				   index_getprocinfo(index, 1, RT_UNION_PROC),
				   CurrentMemoryContext);
	fmgr_info_copy(&rtstate->sizeFn,
				   index_getprocinfo(index, 1, RT_SIZE_PROC),
				   CurrentMemoryContext);
	fmgr_info_copy(&rtstate->interFn,
				   index_getprocinfo(index, 1, RT_INTER_PROC),
				   CurrentMemoryContext);
}

/* for sorting SPLITCOST records in descending order */
static int
qsort_comp_splitcost(const void *a, const void *b)
{
	float		diff =
	((SPLITCOST *) a)->cost_differential -
	((SPLITCOST *) b)->cost_differential;

	if (diff < 0)
		return 1;
	else if (diff > 0)
		return -1;
	else
		return 0;
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
#endif   /* defined RTDEBUG */

void
rtree_redo(XLogRecPtr lsn, XLogRecord *record)
{
	elog(PANIC, "rtree_redo: unimplemented");
}

void
rtree_undo(XLogRecPtr lsn, XLogRecord *record)
{
	elog(PANIC, "rtree_undo: unimplemented");
}

void
rtree_desc(char *buf, uint8 xl_info, char *rec)
{
}
