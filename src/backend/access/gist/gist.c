/*-------------------------------------------------------------------------
 *
 * gist.c
 *	  interface routines for the postgres GiST index access method.
 *
 *
 *
 * IDENTIFICATION
 *
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/gist.h"
#include "access/gistscan.h"
#include "access/heapam.h"
#include "catalog/index.h"
#include "catalog/pg_index.h"
#include "executor/executor.h"
#include "utils/syscache.h"

#ifndef HAVE_MEMMOVE
#include <regex/utils.h>
#else
#include <string.h>
#endif

/* non-export function prototypes */
static InsertIndexResult gistdoinsert(Relation r, IndexTuple itup,
			 GISTSTATE *GISTstate);
static InsertIndexResult gistentryinsert(Relation r, GISTSTACK *stk,
				IndexTuple tup,
				GISTSTATE *giststate);
static void gistentryinserttwo(Relation r, GISTSTACK *stk, IndexTuple ltup,
				   IndexTuple rtup, GISTSTATE *giststate);
static void gistAdjustKeys(Relation r, GISTSTACK *stk, BlockNumber blk,
			   char *datum, int att_size, GISTSTATE *giststate);
static void gistintinsert(Relation r, GISTSTACK *stk, IndexTuple ltup,
			  IndexTuple rtup, GISTSTATE *giststate);
static InsertIndexResult gistSplit(Relation r, Buffer buffer,
		  GISTSTACK *stack, IndexTuple itup,
		  GISTSTATE *giststate);
static void gistnewroot(GISTSTATE *giststate, Relation r, IndexTuple lt,
			IndexTuple rt);
static void GISTInitBuffer(Buffer b, uint32 f);
static BlockNumber gistChooseSubtree(Relation r, IndexTuple itup, int level,
				  GISTSTATE *giststate,
				  GISTSTACK **retstack, Buffer *leafbuf);
static OffsetNumber gistchoose(Relation r, Page p, IndexTuple it,
		   GISTSTATE *giststate);
static int	gistnospace(Page p, IndexTuple it);
void		gistdelete(Relation r, ItemPointer tid);
static IndexTuple gist_tuple_replacekey(Relation r, GISTENTRY entry, IndexTuple t);
static void gistcentryinit(GISTSTATE *giststate, GISTENTRY *e, char *pr,
			   Relation r, Page pg, OffsetNumber o, int b, bool l);
static char *int_range_out(INTRANGE *r);

/*
** routine to build an index.  Basically calls insert over and over
*/
void
gistbuild(Relation heap,
		  Relation index,
		  int natts,
		  AttrNumber *attnum,
		  IndexStrategy istrat,
		  uint16 pint,
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
	GISTSTATE	giststate;
	GISTENTRY	tmpcentry;
	Buffer		buffer = InvalidBuffer;
	bool	   *compvec;

	/* no locking is needed */

	setheapoverride(true);		/* so we can see the new pg_index tuple */
	initGISTstate(&giststate, index);
	setheapoverride(false);

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
		GISTInitBuffer(buffer, F_LEAF);
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
/* shut the compiler up */
	{
		tupleTable = NULL;
		slot = NULL;
		econtext = NULL;
	}
#endif	 /* OMIT_PARTIAL_INDEX */
	/* int the tuples as we insert them */
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
#endif	 /* OMIT_PARTIAL_INDEX */
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
#endif	 /* OMIT_PARTIAL_INDEX */
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

		/* immediately compress keys to normalize */
		compvec = (bool *) palloc(sizeof(bool) * natts);
		for (i = 0; i < natts; i++)
		{
			gistcentryinit(&giststate, &tmpcentry, (char *) d[i],
						   (Relation) NULL, (Page) NULL, (OffsetNumber) 0,
						   -1 /* size is currently bogus */ , TRUE);
			if (d[i] != (Datum) tmpcentry.pred && !(giststate.keytypbyval))
				compvec[i] = TRUE;
			else
				compvec[i] = FALSE;
			d[i] = (Datum) tmpcentry.pred;
		}

		/* form an index tuple and point it at the heap tuple */
		itup = index_formtuple(id, &d[0], nulls);
		itup->t_tid = htup->t_self;

		/*
		 * Since we already have the index relation locked, we call
		 * gistdoinsert directly.  Normal access method calls dispatch
		 * through gistinsert, which locks the relation for write.	This
		 * is the right thing to do if you're inserting single tups, but
		 * not when you're initializing the whole index at once.
		 */

		res = gistdoinsert(index, itup, &giststate);
		for (i = 0; i < natts; i++)
			if (compvec[i] == TRUE)
				pfree((char *) d[i]);
		pfree(itup);
		pfree(res);
		pfree(compvec);
	}

	/* okay, all heap tuples are indexed */
	heap_endscan(scan);

	if (pred != NULL || oldPred != NULL)
	{
#ifndef OMIT_PARTIAL_INDEX
		ExecDestroyTupleTable(tupleTable, true);
		pfree(econtext);
#endif	 /* OMIT_PARTIAL_INDEX */
	}

	/*
	 * Since we just inted the tuples in the heap, we update its stats in
	 * pg_relation to guarantee that the planner takes advantage of the
	 * index we just created.  UpdateStats() does a
	 * CommandinterIncrement(), which flushes changed entries from the
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
 *	gistinsert -- wrapper for GiST tuple insertion.
 *
 *	  This is the public interface routine for tuple insertion in GiSTs.
 *	  It doesn't do any work; just locks the relation and passes the buck.
 */
InsertIndexResult
gistinsert(Relation r, Datum *datum, char *nulls, ItemPointer ht_ctid, Relation heapRel)
{
	InsertIndexResult res;
	IndexTuple	itup;
	GISTSTATE	giststate;
	GISTENTRY	tmpentry;
	int			i;
	bool	   *compvec;

	initGISTstate(&giststate, r);

	/* immediately compress keys to normalize */
	compvec = (bool *) palloc(sizeof(bool) * r->rd_att->natts);
	for (i = 0; i < r->rd_att->natts; i++)
	{
		gistcentryinit(&giststate, &tmpentry, (char *) datum[i],
					   (Relation) NULL, (Page) NULL, (OffsetNumber) 0,
					   -1 /* size is currently bogus */ , TRUE);
		if (datum[i] != (Datum) tmpentry.pred && !(giststate.keytypbyval))
			compvec[i] = TRUE;
		else
			compvec[i] = FALSE;
		datum[i] = (Datum) tmpentry.pred;
	}
	itup = index_formtuple(RelationGetDescr(r), datum, nulls);
	itup->t_tid = *ht_ctid;

	/*
	 * Notes in ExecUtils:ExecOpenIndices()
	 *
	 * RelationSetLockForWrite(r);
	 */

	res = gistdoinsert(r, itup, &giststate);
	for (i = 0; i < r->rd_att->natts; i++)
		if (compvec[i] == TRUE)
			pfree((char *) datum[i]);
	pfree(itup);
	pfree(compvec);

	return res;
}

/*
** Take a compressed entry, and install it on a page.  Since we now know
** where the entry will live, we decompress it and recompress it using
** that knowledge (some compression routines may want to fish around
** on the page, for example, or do something special for leaf nodes.)
*/
static OffsetNumber
gistPageAddItem(GISTSTATE *giststate,
				Relation r,
				Page page,
				Item item,
				Size size,
				OffsetNumber offsetNumber,
				ItemIdFlags flags,
				GISTENTRY *dentry,
				IndexTuple *newtup)
{
	GISTENTRY	tmpcentry;
	IndexTuple	itup = (IndexTuple) item;

	/*
	 * recompress the item given that we now know the exact page and
	 * offset for insertion
	 */
	gistdentryinit(giststate, dentry,
				   (((char *) itup) + sizeof(IndexTupleData)),
			  (Relation) 0, (Page) 0, (OffsetNumber) InvalidOffsetNumber,
				   IndexTupleSize(itup) - sizeof(IndexTupleData), FALSE);
	gistcentryinit(giststate, &tmpcentry, dentry->pred, r, page,
				   offsetNumber, dentry->bytes, FALSE);
	*newtup = gist_tuple_replacekey(r, *dentry, itup);
	/* be tidy */
	if (tmpcentry.pred != dentry->pred
		&& tmpcentry.pred != (((char *) itup) + sizeof(IndexTupleData)))
		pfree(tmpcentry.pred);

	return (PageAddItem(page, (Item) *newtup, IndexTupleSize(*newtup),
						offsetNumber, flags));
}


static InsertIndexResult
gistdoinsert(Relation r,
			 IndexTuple itup,	/* itup contains compressed entry */
			 GISTSTATE *giststate)
{
	GISTENTRY	tmpdentry;
	InsertIndexResult res;
	OffsetNumber l;
	GISTSTACK  *stack;
	Buffer		buffer;
	BlockNumber blk;
	Page		page;
	OffsetNumber off;
	IndexTuple	newtup;

	/* 3rd arg is ignored for now */
	blk = gistChooseSubtree(r, itup, 0, giststate, &stack, &buffer);
	page = (Page) BufferGetPage(buffer);

	if (gistnospace(page, itup))
	{
		/* need to do a split */
		res = gistSplit(r, buffer, stack, itup, giststate);
		gistfreestack(stack);
		WriteBuffer(buffer);	/* don't forget to release buffer! */
		return res;
	}

	if (PageIsEmpty(page))
		off = FirstOffsetNumber;
	else
		off = OffsetNumberNext(PageGetMaxOffsetNumber(page));

	/* add the item and write the buffer */
	l = gistPageAddItem(giststate, r, page, (Item) itup, IndexTupleSize(itup),
						off, LP_USED, &tmpdentry, &newtup);
	WriteBuffer(buffer);

	/* now expand the page boundary in the parent to include the new child */
	gistAdjustKeys(r, stack, blk, tmpdentry.pred, tmpdentry.bytes, giststate);
	gistfreestack(stack);

	/* be tidy */
	if (itup != newtup)
		pfree(newtup);
	if (tmpdentry.pred != (((char *) itup) + sizeof(IndexTupleData)))
		pfree(tmpdentry.pred);

	/* build and return an InsertIndexResult for this insertion */
	res = (InsertIndexResult) palloc(sizeof(InsertIndexResultData));
	ItemPointerSet(&(res->pointerData), blk, l);

	return res;
}


static BlockNumber
gistChooseSubtree(Relation r, IndexTuple itup,	/* itup has compressed
												 * entry */
				  int level,
				  GISTSTATE *giststate,
				  GISTSTACK **retstack /* out */ ,
				  Buffer *leafbuf /* out */ )
{
	Buffer		buffer;
	BlockNumber blk;
	GISTSTACK  *stack;
	Page		page;
	GISTPageOpaque opaque;
	IndexTuple	which;

	blk = GISTP_ROOT;
	buffer = InvalidBuffer;
	stack = (GISTSTACK *) NULL;

	do
	{
		/* let go of current buffer before getting next */
		if (buffer != InvalidBuffer)
			ReleaseBuffer(buffer);

		/* get next buffer */
		buffer = ReadBuffer(r, blk);
		page = (Page) BufferGetPage(buffer);

		opaque = (GISTPageOpaque) PageGetSpecialPointer(page);
		if (!(opaque->flags & F_LEAF))
		{
			GISTSTACK  *n;
			ItemId		iid;

			n = (GISTSTACK *) palloc(sizeof(GISTSTACK));
			n->gs_parent = stack;
			n->gs_blk = blk;
			n->gs_child = gistchoose(r, page, itup, giststate);
			stack = n;

			iid = PageGetItemId(page, n->gs_child);
			which = (IndexTuple) PageGetItem(page, iid);
			blk = ItemPointerGetBlockNumber(&(which->t_tid));
		}
	} while (!(opaque->flags & F_LEAF));

	*retstack = stack;
	*leafbuf = buffer;

	return blk;
}


static void
gistAdjustKeys(Relation r,
			   GISTSTACK *stk,
			   BlockNumber blk,
			   char *datum,		/* datum is uncompressed */
			   int att_size,
			   GISTSTATE *giststate)
{
	char	   *oldud;
	Page		p;
	Buffer		b;
	bool		result;
	bytea	   *evec;
	GISTENTRY	centry,
			   *ev0p,
			   *ev1p;
	int			size,
				datumsize;
	IndexTuple	tid;

	if (stk == (GISTSTACK *) NULL)
		return;

	b = ReadBuffer(r, stk->gs_blk);
	p = BufferGetPage(b);

	oldud = (char *) PageGetItem(p, PageGetItemId(p, stk->gs_child));
	tid = (IndexTuple) oldud;
	size = IndexTupleSize((IndexTuple) oldud) - sizeof(IndexTupleData);
	oldud += sizeof(IndexTupleData);

	evec = (bytea *) palloc(2 * sizeof(GISTENTRY) + VARHDRSZ);
	VARSIZE(evec) = 2 * sizeof(GISTENTRY) + VARHDRSZ;

	/* insert decompressed oldud into entry vector */
	gistdentryinit(giststate, &((GISTENTRY *) VARDATA(evec))[0],
				   oldud, r, p, stk->gs_child,
				   size, FALSE);
	ev0p = &((GISTENTRY *) VARDATA(evec))[0];

	/* insert datum entry into entry vector */
	gistentryinit(((GISTENTRY *) VARDATA(evec))[1], datum,
		(Relation) NULL, (Page) NULL, (OffsetNumber) 0, att_size, FALSE);
	ev1p = &((GISTENTRY *) VARDATA(evec))[1];

	/* form union of decompressed entries */
	datum = (*fmgr_faddr(&giststate->unionFn)) (evec, &datumsize);

	/* did union leave decompressed version of oldud unchanged? */
	(*fmgr_faddr(&giststate->equalFn)) (ev0p->pred, datum, &result);
	if (!result)
	{
		TupleDesc	td = RelationGetDescr(r);

		/* compress datum for storage on page */
		gistcentryinit(giststate, &centry, datum, ev0p->rel, ev0p->page,
					   ev0p->offset, datumsize, FALSE);
		if (td->attrs[0]->attlen >= 0)
		{
			memmove(oldud, centry.pred, att_size);
			gistAdjustKeys(r, stk->gs_parent, stk->gs_blk, datum, att_size,
						   giststate);
		}
		else if (VARSIZE(centry.pred) == VARSIZE(oldud))
		{
			memmove(oldud, centry.pred, VARSIZE(centry.pred));
			gistAdjustKeys(r, stk->gs_parent, stk->gs_blk, datum, att_size,
						   giststate);
		}
		else
		{

			/*
			 * * new datum is not the same size as the old. * We have to
			 * delete the old entry and insert the new * one.  Note that
			 * this may cause a split here!
			 */
			IndexTuple	newtup;
			ItemPointerData oldtid;
			char	   *isnull;
			TupleDesc	tupDesc;
			InsertIndexResult res;

			/* delete old tuple */
			ItemPointerSet(&oldtid, stk->gs_blk, stk->gs_child);
			gistdelete(r, (ItemPointer) &oldtid);

			/* generate and insert new tuple */
			tupDesc = r->rd_att;
			isnull = (char *) palloc(r->rd_rel->relnatts);
			MemSet(isnull, ' ', r->rd_rel->relnatts);
			newtup = (IndexTuple) index_formtuple(tupDesc,
										 (Datum *) &centry.pred, isnull);
			pfree(isnull);
			/* set pointer in new tuple to point to current child */
			ItemPointerSet(&oldtid, blk, 1);
			newtup->t_tid = oldtid;

			/* inserting the new entry also adjust keys above */
			res = gistentryinsert(r, stk, newtup, giststate);

			/* in stack, set info to point to new tuple */
			stk->gs_blk = ItemPointerGetBlockNumber(&(res->pointerData));
			stk->gs_child = ItemPointerGetOffsetNumber(&(res->pointerData));

			pfree(res);
		}
		WriteBuffer(b);

		if (centry.pred != datum)
			pfree(datum);
	}
	else
		ReleaseBuffer(b);
	pfree(evec);
}

/*
 *	gistSplit -- split a page in the tree.
 *
 */
static InsertIndexResult
gistSplit(Relation r,
		  Buffer buffer,
		  GISTSTACK *stack,
		  IndexTuple itup,		/* contains compressed entry */
		  GISTSTATE *giststate)
{
	Page		p;
	Buffer		leftbuf,
				rightbuf;
	Page		left,
				right;
	ItemId		itemid;
	IndexTuple	item;
	IndexTuple	ltup,
				rtup,
				newtup;
	OffsetNumber maxoff;
	OffsetNumber i;
	OffsetNumber leftoff,
				rightoff;
	BlockNumber lbknum,
				rbknum;
	BlockNumber bufblock;
	GISTPageOpaque opaque;
	int			blank;
	InsertIndexResult res;
	char	   *isnull;
	GIST_SPLITVEC v;
	TupleDesc	tupDesc;
	bytea	   *entryvec;
	bool	   *decompvec;
	IndexTuple	item_1;
	GISTENTRY	tmpdentry,
				tmpentry;

	isnull = (char *) palloc(r->rd_rel->relnatts);
	for (blank = 0; blank < r->rd_rel->relnatts; blank++)
		isnull[blank] = ' ';
	p = (Page) BufferGetPage(buffer);
	opaque = (GISTPageOpaque) PageGetSpecialPointer(p);


	/*
	 * The root of the tree is the first block in the relation.  If we're
	 * about to split the root, we need to do some hocus-pocus to enforce
	 * this guarantee.
	 */

	if (BufferGetBlockNumber(buffer) == GISTP_ROOT)
	{
		leftbuf = ReadBuffer(r, P_NEW);
		GISTInitBuffer(leftbuf, opaque->flags);
		lbknum = BufferGetBlockNumber(leftbuf);
		left = (Page) BufferGetPage(leftbuf);
	}
	else
	{
		leftbuf = buffer;
		IncrBufferRefCount(buffer);
		lbknum = BufferGetBlockNumber(buffer);
		left = (Page) PageGetTempPage(p, sizeof(GISTPageOpaqueData));
	}

	rightbuf = ReadBuffer(r, P_NEW);
	GISTInitBuffer(rightbuf, opaque->flags);
	rbknum = BufferGetBlockNumber(rightbuf);
	right = (Page) BufferGetPage(rightbuf);

	/* generate the item array */
	maxoff = PageGetMaxOffsetNumber(p);
	entryvec = (bytea *) palloc(VARHDRSZ + (maxoff + 2) * sizeof(GISTENTRY));
	decompvec = (bool *) palloc(VARHDRSZ + (maxoff + 2) * sizeof(bool));
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		item_1 = (IndexTuple) PageGetItem(p, PageGetItemId(p, i));
		gistdentryinit(giststate, &((GISTENTRY *) VARDATA(entryvec))[i],
					   (((char *) item_1) + sizeof(IndexTupleData)),
					   r, p, i,
				 IndexTupleSize(item_1) - sizeof(IndexTupleData), FALSE);
		if ((char *) (((GISTENTRY *) VARDATA(entryvec))[i].pred)
			== (((char *) item_1) + sizeof(IndexTupleData)))
			decompvec[i] = FALSE;
		else
			decompvec[i] = TRUE;
	}

	/* add the new datum as the last entry */
	gistdentryinit(giststate, &(((GISTENTRY *) VARDATA(entryvec))[maxoff + 1]),
				   (((char *) itup) + sizeof(IndexTupleData)),
				   (Relation) NULL, (Page) NULL,
				   (OffsetNumber) 0, tmpentry.bytes, FALSE);
	if ((char *) (((GISTENTRY *) VARDATA(entryvec))[maxoff + 1]).pred !=
		(((char *) itup) + sizeof(IndexTupleData)))
		decompvec[maxoff + 1] = TRUE;
	else
		decompvec[maxoff + 1] = FALSE;

	VARSIZE(entryvec) = (maxoff + 2) * sizeof(GISTENTRY) + VARHDRSZ;

	/* now let the user-defined picksplit function set up the split vector */
	(*fmgr_faddr(&giststate->picksplitFn)) (entryvec, &v);

	/* compress ldatum and rdatum */
	gistcentryinit(giststate, &tmpentry, v.spl_ldatum, (Relation) NULL,
				   (Page) NULL, (OffsetNumber) 0,
				   ((GISTENTRY *) VARDATA(entryvec))[i].bytes, FALSE);
	if (v.spl_ldatum != tmpentry.pred)
		pfree(v.spl_ldatum);
	v.spl_ldatum = tmpentry.pred;

	gistcentryinit(giststate, &tmpentry, v.spl_rdatum, (Relation) NULL,
				   (Page) NULL, (OffsetNumber) 0,
				   ((GISTENTRY *) VARDATA(entryvec))[i].bytes, FALSE);
	if (v.spl_rdatum != tmpentry.pred)
		pfree(v.spl_rdatum);
	v.spl_rdatum = tmpentry.pred;

	/* clean up the entry vector: its preds need to be deleted, too */
	for (i = FirstOffsetNumber; i <= maxoff + 1; i = OffsetNumberNext(i))
		if (decompvec[i])
			pfree(((GISTENTRY *) VARDATA(entryvec))[i].pred);
	pfree(entryvec);
	pfree(decompvec);

	leftoff = rightoff = FirstOffsetNumber;
	maxoff = PageGetMaxOffsetNumber(p);
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		itemid = PageGetItemId(p, i);
		item = (IndexTuple) PageGetItem(p, itemid);

		if (i == *(v.spl_left))
		{
			gistPageAddItem(giststate, r, left, (Item) item,
							IndexTupleSize(item),
							leftoff, LP_USED, &tmpdentry, &newtup);
			leftoff = OffsetNumberNext(leftoff);
			v.spl_left++;		/* advance in left split vector */
			/* be tidy */
			if (tmpdentry.pred != (((char *) item) + sizeof(IndexTupleData)))
				pfree(tmpdentry.pred);
			if ((IndexTuple) item != newtup)
				pfree(newtup);
		}
		else
		{
			gistPageAddItem(giststate, r, right, (Item) item,
							IndexTupleSize(item),
							rightoff, LP_USED, &tmpdentry, &newtup);
			rightoff = OffsetNumberNext(rightoff);
			v.spl_right++;		/* advance in right split vector */
			/* be tidy */
			if (tmpdentry.pred != (((char *) item) + sizeof(IndexTupleData)))
				pfree(tmpdentry.pred);
			if (item != newtup)
				pfree(newtup);
		}
	}

	/* build an InsertIndexResult for this insertion */
	res = (InsertIndexResult) palloc(sizeof(InsertIndexResultData));

	/* now insert the new index tuple */
	if (*(v.spl_left) != FirstOffsetNumber)
	{
		gistPageAddItem(giststate, r, left, (Item) itup,
						IndexTupleSize(itup),
						leftoff, LP_USED, &tmpdentry, &newtup);
		leftoff = OffsetNumberNext(leftoff);
		ItemPointerSet(&(res->pointerData), lbknum, leftoff);
		/* be tidy */
		if (tmpdentry.pred != (((char *) itup) + sizeof(IndexTupleData)))
			pfree(tmpdentry.pred);
		if (itup != newtup)
			pfree(newtup);
	}
	else
	{
		gistPageAddItem(giststate, r, right, (Item) itup,
						IndexTupleSize(itup),
						rightoff, LP_USED, &tmpdentry, &newtup);
		rightoff = OffsetNumberNext(rightoff);
		ItemPointerSet(&(res->pointerData), rbknum, rightoff);
		/* be tidy */
		if (tmpdentry.pred != (((char *) itup) + sizeof(IndexTupleData)))
			pfree(tmpdentry.pred);
		if (itup != newtup)
			pfree(newtup);
	}

	if ((bufblock = BufferGetBlockNumber(buffer)) != GISTP_ROOT)
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
	gistadjscans(r, GISTOP_SPLIT, bufblock, FirstOffsetNumber);

	tupDesc = r->rd_att;

	ltup = (IndexTuple) index_formtuple(tupDesc,
									  (Datum *) &(v.spl_ldatum), isnull);
	rtup = (IndexTuple) index_formtuple(tupDesc,
									  (Datum *) &(v.spl_rdatum), isnull);
	pfree(isnull);

	/* set pointers to new child pages in the internal index tuples */
	ItemPointerSet(&(ltup->t_tid), lbknum, 1);
	ItemPointerSet(&(rtup->t_tid), rbknum, 1);

	gistintinsert(r, stack, ltup, rtup, giststate);

	pfree(ltup);
	pfree(rtup);

	return res;
}

/*
** After a split, we need to overwrite the old entry's key in the parent,
** and install install an entry for the new key into the parent.
*/
static void
gistintinsert(Relation r,
			  GISTSTACK *stk,
			  IndexTuple ltup,	/* new version of entry for old page */
			  IndexTuple rtup,	/* entry for new page */
			  GISTSTATE *giststate)
{
	ItemPointerData ltid;

	if (stk == (GISTSTACK *) NULL)
	{
		gistnewroot(giststate, r, ltup, rtup);
		return;
	}

	/* remove old left pointer, insert the 2 new entries */
	ItemPointerSet(&ltid, stk->gs_blk, stk->gs_child);
	gistdelete(r, (ItemPointer) &ltid);
	gistentryinserttwo(r, stk, ltup, rtup, giststate);
}


/*
** Insert two entries onto one page, handling a split for either one!
*/
static void
gistentryinserttwo(Relation r, GISTSTACK *stk, IndexTuple ltup,
				   IndexTuple rtup, GISTSTATE *giststate)
{
	Buffer		b;
	Page		p;
	InsertIndexResult res;
	GISTENTRY	tmpentry;
	IndexTuple	newtup;

	b = ReadBuffer(r, stk->gs_blk);
	p = BufferGetPage(b);

	if (gistnospace(p, ltup))
	{
		res = gistSplit(r, b, stk->gs_parent, ltup, giststate);
		WriteBuffer(b);			/* don't forget to release buffer!  -
								 * 01/31/94 */
		pfree(res);
		gistdoinsert(r, rtup, giststate);
	}
	else
	{
		gistPageAddItem(giststate, r, p, (Item) ltup,
						IndexTupleSize(ltup), InvalidOffsetNumber,
						LP_USED, &tmpentry, &newtup);
		WriteBuffer(b);
		gistAdjustKeys(r, stk->gs_parent, stk->gs_blk, tmpentry.pred,
					   tmpentry.bytes, giststate);
		/* be tidy */
		if (tmpentry.pred != (((char *) ltup) + sizeof(IndexTupleData)))
			pfree(tmpentry.pred);
		if (ltup != newtup)
			pfree(newtup);
		gistentryinsert(r, stk, rtup, giststate);
	}
}


/*
** Insert an entry onto a page
*/
static InsertIndexResult
gistentryinsert(Relation r, GISTSTACK *stk, IndexTuple tup,
				GISTSTATE *giststate)
{
	Buffer		b;
	Page		p;
	InsertIndexResult res;
	OffsetNumber off;
	GISTENTRY	tmpentry;
	IndexTuple	newtup;

	b = ReadBuffer(r, stk->gs_blk);
	p = BufferGetPage(b);

	if (gistnospace(p, tup))
	{
		res = gistSplit(r, b, stk->gs_parent, tup, giststate);
		WriteBuffer(b);			/* don't forget to release buffer!  -
								 * 01/31/94 */
		return res;
	}
	else
	{
		res = (InsertIndexResult) palloc(sizeof(InsertIndexResultData));
		off = gistPageAddItem(giststate, r, p, (Item) tup, IndexTupleSize(tup),
					   InvalidOffsetNumber, LP_USED, &tmpentry, &newtup);
		WriteBuffer(b);
		ItemPointerSet(&(res->pointerData), stk->gs_blk, off);
		gistAdjustKeys(r, stk->gs_parent, stk->gs_blk, tmpentry.pred,
					   tmpentry.bytes, giststate);
		/* be tidy */
		if (tmpentry.pred != (((char *) tup) + sizeof(IndexTupleData)))
			pfree(tmpentry.pred);
		if (tup != newtup)
			pfree(newtup);
		return res;
	}
}


static void
gistnewroot(GISTSTATE *giststate, Relation r, IndexTuple lt, IndexTuple rt)
{
	Buffer		b;
	Page		p;
	GISTENTRY	tmpentry;
	IndexTuple	newtup;

	b = ReadBuffer(r, GISTP_ROOT);
	GISTInitBuffer(b, 0);
	p = BufferGetPage(b);
	gistPageAddItem(giststate, r, p, (Item) lt, IndexTupleSize(lt),
					FirstOffsetNumber,
					LP_USED, &tmpentry, &newtup);
	/* be tidy */
	if (tmpentry.pred != (((char *) lt) + sizeof(IndexTupleData)))
		pfree(tmpentry.pred);
	if (lt != newtup)
		pfree(newtup);
	gistPageAddItem(giststate, r, p, (Item) rt, IndexTupleSize(rt),
					OffsetNumberNext(FirstOffsetNumber), LP_USED,
					&tmpentry, &newtup);
	/* be tidy */
	if (tmpentry.pred != (((char *) rt) + sizeof(IndexTupleData)))
		pfree(tmpentry.pred);
	if (rt != newtup)
		pfree(newtup);
	WriteBuffer(b);
}

static void
GISTInitBuffer(Buffer b, uint32 f)
{
	GISTPageOpaque opaque;
	Page		page;
	Size		pageSize;

	pageSize = BufferGetPageSize(b);

	page = BufferGetPage(b);
	MemSet(page, 0, (int) pageSize);
	PageInit(page, pageSize, sizeof(GISTPageOpaqueData));

	opaque = (GISTPageOpaque) PageGetSpecialPointer(page);
	opaque->flags = f;
}


/*
** find entry with lowest penalty
*/
static OffsetNumber
gistchoose(Relation r, Page p, IndexTuple it,	/* it has compressed entry */
		   GISTSTATE *giststate)
{
	OffsetNumber maxoff;
	OffsetNumber i;
	char	   *id;
	char	   *datum;
	float		usize;
	OffsetNumber which;
	float		which_grow;
	GISTENTRY	entry,
				identry;
	int			size,
				idsize;

	idsize = IndexTupleSize(it) - sizeof(IndexTupleData);
	id = ((char *) it) + sizeof(IndexTupleData);
	maxoff = PageGetMaxOffsetNumber(p);
	which_grow = -1.0;
	which = -1;

	gistdentryinit(giststate, &identry, id, (Relation) NULL, (Page) NULL,
				   (OffsetNumber) 0, idsize, FALSE);

	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		datum = (char *) PageGetItem(p, PageGetItemId(p, i));
		size = IndexTupleSize(datum) - sizeof(IndexTupleData);
		datum += sizeof(IndexTupleData);
		gistdentryinit(giststate, &entry, datum, r, p, i, size, FALSE);
		(*fmgr_faddr(&giststate->penaltyFn)) (&entry, &identry, &usize);
		if (which_grow < 0 || usize < which_grow)
		{
			which = i;
			which_grow = usize;
			if (which_grow == 0)
				break;
		}
		if (entry.pred != datum)
			pfree(entry.pred);
	}
	if (identry.pred != id)
		pfree(identry.pred);

	return which;
}

static int
gistnospace(Page p, IndexTuple it)
{
	return PageGetFreeSpace(p) < IndexTupleSize(it);
}

void
gistfreestack(GISTSTACK *s)
{
	GISTSTACK  *p;

	while (s != (GISTSTACK *) NULL)
	{
		p = s->gs_parent;
		pfree(s);
		s = p;
	}
}


/*
** remove an entry from a page
*/
void
gistdelete(Relation r, ItemPointer tid)
{
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
	gistadjscans(r, GISTOP_DEL, blkno, offnum);

	/* delete the index tuple */
	buf = ReadBuffer(r, blkno);
	page = BufferGetPage(buf);

	PageIndexTupleDelete(page, offnum);

	WriteBuffer(buf);

}

void
initGISTstate(GISTSTATE *giststate, Relation index)
{
	RegProcedure consistent_proc,
				union_proc,
				compress_proc,
				decompress_proc;
	RegProcedure penalty_proc,
				picksplit_proc,
				equal_proc;
	HeapTuple	htup;
	Form_pg_index itupform;

	consistent_proc = index_getprocid(index, 1, GIST_CONSISTENT_PROC);
	union_proc = index_getprocid(index, 1, GIST_UNION_PROC);
	compress_proc = index_getprocid(index, 1, GIST_COMPRESS_PROC);
	decompress_proc = index_getprocid(index, 1, GIST_DECOMPRESS_PROC);
	penalty_proc = index_getprocid(index, 1, GIST_PENALTY_PROC);
	picksplit_proc = index_getprocid(index, 1, GIST_PICKSPLIT_PROC);
	equal_proc = index_getprocid(index, 1, GIST_EQUAL_PROC);
	fmgr_info(consistent_proc, &giststate->consistentFn);
	fmgr_info(union_proc, &giststate->unionFn);
	fmgr_info(compress_proc, &giststate->compressFn);
	fmgr_info(decompress_proc, &giststate->decompressFn);
	fmgr_info(penalty_proc, &giststate->penaltyFn);
	fmgr_info(picksplit_proc, &giststate->picksplitFn);
	fmgr_info(equal_proc, &giststate->equalFn);

	/* see if key type is different from type of attribute being indexed */
	htup = SearchSysCacheTuple(INDEXRELID,
							   ObjectIdGetDatum(RelationGetRelid(index)),
							   0, 0, 0);
	itupform = (Form_pg_index) GETSTRUCT(htup);
	if (!HeapTupleIsValid(htup))
		elog(ERROR, "initGISTstate: index %u not found",
			 RelationGetRelid(index));
	giststate->haskeytype = itupform->indhaskeytype;
	if (giststate->haskeytype)
	{
		/* key type is different -- is it byval? */
		htup = SearchSysCacheTuple(ATTNUM,
								   ObjectIdGetDatum(itupform->indexrelid),
								   UInt16GetDatum(FirstOffsetNumber),
								   0, 0);
		if (!HeapTupleIsValid(htup))
		{
			elog(ERROR, "initGISTstate: no attribute tuple %u %d",
				 itupform->indexrelid, FirstOffsetNumber);
			return;
		}
		giststate->keytypbyval = (((Form_pg_attribute) htup)->attbyval);
	}
	else
		giststate->keytypbyval = FALSE;
	return;
}


/*
** Given an IndexTuple to be inserted on a page, this routine replaces
** the key with another key, which may involve generating a new IndexTuple
** if the sizes don't match
*/
static IndexTuple
gist_tuple_replacekey(Relation r, GISTENTRY entry, IndexTuple t)
{
	char	   *datum = (((char *) t) + sizeof(IndexTupleData));

	/* if new entry fits in index tuple, copy it in */
	if (entry.bytes < IndexTupleSize(t) - sizeof(IndexTupleData))
	{
		memcpy(datum, entry.pred, entry.bytes);
		/* clear out old size */
		t->t_info &= 0xe000;
		/* or in new size */
		t->t_info |= MAXALIGN(entry.bytes + sizeof(IndexTupleData));

		return t;
	}
	else
	{
		/* generate a new index tuple for the compressed entry */
		TupleDesc	tupDesc = r->rd_att;
		IndexTuple	newtup;
		char	   *isnull;
		int			blank;

		isnull = (char *) palloc(r->rd_rel->relnatts);
		for (blank = 0; blank < r->rd_rel->relnatts; blank++)
			isnull[blank] = ' ';
		newtup = (IndexTuple) index_formtuple(tupDesc,
											  (Datum *) &(entry.pred),
											  isnull);
		newtup->t_tid = t->t_tid;
		pfree(isnull);
		return newtup;
	}
}


/*
** initialize a GiST entry with a decompressed version of pred
*/
void
gistdentryinit(GISTSTATE *giststate, GISTENTRY *e, char *pr, Relation r,
			   Page pg, OffsetNumber o, int b, bool l)
{
	GISTENTRY  *dep;

	gistentryinit(*e, pr, r, pg, o, b, l);
	if (giststate->haskeytype)
	{
		dep = (GISTENTRY *) ((*fmgr_faddr(&giststate->decompressFn)) (e));
		gistentryinit(*e, dep->pred, dep->rel, dep->page, dep->offset, dep->bytes,
					  dep->leafkey);
		if (dep != e)
			pfree(dep);
	}
}


/*
** initialize a GiST entry with a compressed version of pred
*/
static void
gistcentryinit(GISTSTATE *giststate, GISTENTRY *e, char *pr, Relation r,
			   Page pg, OffsetNumber o, int b, bool l)
{
	GISTENTRY  *cep;

	gistentryinit(*e, pr, r, pg, o, b, l);
	if (giststate->haskeytype)
	{
		cep = (GISTENTRY *) ((*fmgr_faddr(&giststate->compressFn)) (e));
		gistentryinit(*e, cep->pred, cep->rel, cep->page, cep->offset, cep->bytes,
					  cep->leafkey);
		if (cep != e)
			pfree(cep);
	}
}



#ifdef GISTDEBUG

/*
** sloppy debugging support routine, requires recompilation with appropriate
** "out" method for the index keys.  Could be fixed to find that info
** in the catalogs...
*/
void
_gistdump(Relation r)
{
	Buffer		buf;
	Page		page;
	OffsetNumber offnum,
				maxoff;
	BlockNumber blkno;
	BlockNumber nblocks;
	GISTPageOpaque po;
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
		po = (GISTPageOpaque) PageGetSpecialPointer(page);
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
			/* get out function for type of key, and out it! */
			itkey = (char *) int_range_out((INTRANGE *) datum);
			/* itkey = " unable to print"; */
			printf("\t[%d] size %d heap <%d,%d> key:%s\n",
				   offnum, IndexTupleSize(itup), itblkno, itoffno, itkey);
			pfree(itkey);
		}

		ReleaseBuffer(buf);
	}
}

static char *
int_range_out(INTRANGE *r)
{
	char	   *result;

	if (r == NULL)
		return NULL;
	result = (char *) palloc(80);
	snprintf(result, 80, "[%d,%d): %d", r->lower, r->upper, r->flag);

	return result;
}

#endif	 /* defined GISTDEBUG */
