/*-------------------------------------------------------------------------
 *
 * gist.c
 *	  interface routines for the postgres GiST index access method.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/gist/gist.c,v 1.144 2006/11/12 06:55:53 neilc Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/gist_private.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "utils/memutils.h"

const XLogRecPtr XLogRecPtrForTemp = {1, 1};

/* Working state for gistbuild and its callback */
typedef struct
{
	GISTSTATE	giststate;
	int			numindexattrs;
	double		indtuples;
	MemoryContext tmpCtx;
} GISTBuildState;


/* non-export function prototypes */
static void gistbuildCallback(Relation index,
				  HeapTuple htup,
				  Datum *values,
				  bool *isnull,
				  bool tupleIsAlive,
				  void *state);
static void gistdoinsert(Relation r,
			 IndexTuple itup,
			 Size freespace,
			 GISTSTATE *GISTstate);
static void gistfindleaf(GISTInsertState *state,
			 GISTSTATE *giststate);


#define ROTATEDIST(d) do { \
	SplitedPageLayout *tmp=(SplitedPageLayout*)palloc(sizeof(SplitedPageLayout)); \
	memset(tmp,0,sizeof(SplitedPageLayout)); \
	tmp->block.blkno = InvalidBlockNumber;	\
	tmp->buffer = InvalidBuffer;	\
	tmp->next = (d); \
	(d)=tmp; \
} while(0)


/*
 * Create and return a temporary memory context for use by GiST. We
 * _always_ invoke user-provided methods in a temporary memory
 * context, so that memory leaks in those functions cannot cause
 * problems. Also, we use some additional temporary contexts in the
 * GiST code itself, to avoid the need to do some awkward manual
 * memory management.
 */
MemoryContext
createTempGistContext(void)
{
	return AllocSetContextCreate(CurrentMemoryContext,
								 "GiST temporary context",
								 ALLOCSET_DEFAULT_MINSIZE,
								 ALLOCSET_DEFAULT_INITSIZE,
								 ALLOCSET_DEFAULT_MAXSIZE);
}

/*
 * Routine to build an index.  Basically calls insert over and over.
 *
 * XXX: it would be nice to implement some sort of bulk-loading
 * algorithm, but it is not clear how to do that.
 */
Datum
gistbuild(PG_FUNCTION_ARGS)
{
	Relation	heap = (Relation) PG_GETARG_POINTER(0);
	Relation	index = (Relation) PG_GETARG_POINTER(1);
	IndexInfo  *indexInfo = (IndexInfo *) PG_GETARG_POINTER(2);
	IndexBuildResult *result;
	double		reltuples;
	GISTBuildState buildstate;
	Buffer		buffer;
	Page		page;

	/*
	 * We expect to be called exactly once for any index relation. If that's
	 * not the case, big trouble's what we have.
	 */
	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	/* no locking is needed */
	initGISTstate(&buildstate.giststate, index);

	/* initialize the root page */
	buffer = gistNewBuffer(index);
	Assert(BufferGetBlockNumber(buffer) == GIST_ROOT_BLKNO);
	page = BufferGetPage(buffer);

	START_CRIT_SECTION();

	GISTInitBuffer(buffer, F_LEAF);

	MarkBufferDirty(buffer);

	if (!index->rd_istemp)
	{
		XLogRecPtr	recptr;
		XLogRecData rdata;

		rdata.data = (char *) &(index->rd_node);
		rdata.len = sizeof(RelFileNode);
		rdata.buffer = InvalidBuffer;
		rdata.next = NULL;

		recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_CREATE_INDEX, &rdata);
		PageSetLSN(page, recptr);
		PageSetTLI(page, ThisTimeLineID);
	}
	else
		PageSetLSN(page, XLogRecPtrForTemp);

	UnlockReleaseBuffer(buffer);

	END_CRIT_SECTION();

	/* build the index */
	buildstate.numindexattrs = indexInfo->ii_NumIndexAttrs;
	buildstate.indtuples = 0;

	/*
	 * create a temporary memory context that is reset once for each tuple
	 * inserted into the index
	 */
	buildstate.tmpCtx = createTempGistContext();

	/* do the heap scan */
	reltuples = IndexBuildHeapScan(heap, index, indexInfo,
								   gistbuildCallback, (void *) &buildstate);

	/* okay, all heap tuples are indexed */
	MemoryContextDelete(buildstate.tmpCtx);

	freeGISTstate(&buildstate.giststate);

	/*
	 * Return statistics
	 */
	result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));

	result->heap_tuples = reltuples;
	result->index_tuples = buildstate.indtuples;

	PG_RETURN_POINTER(result);
}

/*
 * Per-tuple callback from IndexBuildHeapScan
 */
static void
gistbuildCallback(Relation index,
				  HeapTuple htup,
				  Datum *values,
				  bool *isnull,
				  bool tupleIsAlive,
				  void *state)
{
	GISTBuildState *buildstate = (GISTBuildState *) state;
	IndexTuple	itup;
	MemoryContext oldCtx;

	oldCtx = MemoryContextSwitchTo(buildstate->tmpCtx);

	/* form an index tuple and point it at the heap tuple */
	itup = gistFormTuple(&buildstate->giststate, index,
						 values, isnull, true /* size is currently bogus */ );
	itup->t_tid = htup->t_self;

	/*
	 * Since we already have the index relation locked, we call gistdoinsert
	 * directly.  Normal access method calls dispatch through gistinsert,
	 * which locks the relation for write.	This is the right thing to do if
	 * you're inserting single tups, but not when you're initializing the
	 * whole index at once.
	 *
	 * In this path we respect the fillfactor setting, whereas insertions
	 * after initial build do not.
	 */
	gistdoinsert(index, itup,
			  RelationGetTargetPageFreeSpace(index, GIST_DEFAULT_FILLFACTOR),
				 &buildstate->giststate);

	buildstate->indtuples += 1;
	MemoryContextSwitchTo(oldCtx);
	MemoryContextReset(buildstate->tmpCtx);
}

/*
 *	gistinsert -- wrapper for GiST tuple insertion.
 *
 *	  This is the public interface routine for tuple insertion in GiSTs.
 *	  It doesn't do any work; just locks the relation and passes the buck.
 */
Datum
gistinsert(PG_FUNCTION_ARGS)
{
	Relation	r = (Relation) PG_GETARG_POINTER(0);
	Datum	   *values = (Datum *) PG_GETARG_POINTER(1);
	bool	   *isnull = (bool *) PG_GETARG_POINTER(2);
	ItemPointer ht_ctid = (ItemPointer) PG_GETARG_POINTER(3);

#ifdef NOT_USED
	Relation	heapRel = (Relation) PG_GETARG_POINTER(4);
	bool		checkUnique = PG_GETARG_BOOL(5);
#endif
	IndexTuple	itup;
	GISTSTATE	giststate;
	MemoryContext oldCtx;
	MemoryContext insertCtx;

	insertCtx = createTempGistContext();
	oldCtx = MemoryContextSwitchTo(insertCtx);

	initGISTstate(&giststate, r);

	itup = gistFormTuple(&giststate, r,
						 values, isnull, true /* size is currently bogus */ );
	itup->t_tid = *ht_ctid;

	gistdoinsert(r, itup, 0, &giststate);

	/* cleanup */
	freeGISTstate(&giststate);
	MemoryContextSwitchTo(oldCtx);
	MemoryContextDelete(insertCtx);

	PG_RETURN_BOOL(true);
}


/*
 * Workhouse routine for doing insertion into a GiST index. Note that
 * this routine assumes it is invoked in a short-lived memory context,
 * so it does not bother releasing palloc'd allocations.
 */
static void
gistdoinsert(Relation r, IndexTuple itup, Size freespace, GISTSTATE *giststate)
{
	GISTInsertState state;

	memset(&state, 0, sizeof(GISTInsertState));

	state.itup = (IndexTuple *) palloc(sizeof(IndexTuple));
	state.itup[0] = (IndexTuple) palloc(IndexTupleSize(itup));
	memcpy(state.itup[0], itup, IndexTupleSize(itup));
	state.ituplen = 1;
	state.freespace = freespace;
	state.r = r;
	state.key = itup->t_tid;
	state.needInsertComplete = true;

	state.stack = (GISTInsertStack *) palloc0(sizeof(GISTInsertStack));
	state.stack->blkno = GIST_ROOT_BLKNO;

	gistfindleaf(&state, giststate);
	gistmakedeal(&state, giststate);
}

static bool
gistplacetopage(GISTInsertState *state, GISTSTATE *giststate)
{
	bool		is_splitted = false;
	bool		is_leaf = (GistPageIsLeaf(state->stack->page)) ? true : false;

	/*
	 * if (!is_leaf) remove old key: This node's key has been modified, either
	 * because a child split occurred or because we needed to adjust our key
	 * for an insert in a child node. Therefore, remove the old version of
	 * this node's key.
	 *
	 * for WAL replay, in the non-split case we handle this by setting up a
	 * one-element todelete array; in the split case, it's handled implicitly
	 * because the tuple vector passed to gistSplit won't include this tuple.
	 *
	 * XXX: If we want to change fillfactors between node and leaf, fillfactor
	 * = (is_leaf ? state->leaf_fillfactor : state->node_fillfactor)
	 */
	if (gistnospace(state->stack->page, state->itup, state->ituplen,
					is_leaf ? InvalidOffsetNumber : state->stack->childoffnum,
					state->freespace))
	{
		/* no space for insertion */
		IndexTuple *itvec;
		int			tlen;
		SplitedPageLayout *dist = NULL,
				   *ptr;
		BlockNumber rrlink = InvalidBlockNumber;
		GistNSN		oldnsn;

		is_splitted = true;

		/*
		 * Form index tuples vector to split: remove old tuple if t's needed
		 * and add new tuples to vector
		 */
		itvec = gistextractpage(state->stack->page, &tlen);
		if (!is_leaf)
		{
			/* on inner page we should remove old tuple */
			int			pos = state->stack->childoffnum - FirstOffsetNumber;

			tlen--;
			if (pos != tlen)
				memmove(itvec + pos, itvec + pos + 1, sizeof(IndexTuple) * (tlen - pos));
		}
		itvec = gistjoinvector(itvec, &tlen, state->itup, state->ituplen);
		dist = gistSplit(state->r, state->stack->page, itvec, tlen, giststate);

		state->itup = (IndexTuple *) palloc(sizeof(IndexTuple) * tlen);
		state->ituplen = 0;

		if (state->stack->blkno != GIST_ROOT_BLKNO)
		{
			/*
			 * if non-root split then we should not allocate new buffer, but
			 * we must create temporary page to operate
			 */
			dist->buffer = state->stack->buffer;
			dist->page = PageGetTempPage(BufferGetPage(dist->buffer), sizeof(GISTPageOpaqueData));

			/* clean all flags except F_LEAF */
			GistPageGetOpaque(dist->page)->flags = (is_leaf) ? F_LEAF : 0;
		}

		/* make new pages and fills them */
		for (ptr = dist; ptr; ptr = ptr->next)
		{
			int			i;
			char	   *data;

			/* get new page */
			if (ptr->buffer == InvalidBuffer)
			{
				ptr->buffer = gistNewBuffer(state->r);
				GISTInitBuffer(ptr->buffer, (is_leaf) ? F_LEAF : 0);
				ptr->page = BufferGetPage(ptr->buffer);
			}
			ptr->block.blkno = BufferGetBlockNumber(ptr->buffer);

			/*
			 * fill page, we can do it because all these pages are new
			 * (ie not linked in tree or masked by temp page
			 */
			data = (char *) (ptr->list);
			for (i = 0; i < ptr->block.num; i++)
			{
				if (PageAddItem(ptr->page, (Item) data, IndexTupleSize((IndexTuple) data), i + FirstOffsetNumber, LP_USED) == InvalidOffsetNumber)
					elog(ERROR, "failed to add item to index page in \"%s\"", RelationGetRelationName(state->r));
				data += IndexTupleSize((IndexTuple) data);
			}

			/* set up ItemPointer and remember it for parent */
			ItemPointerSetBlockNumber(&(ptr->itup->t_tid), ptr->block.blkno);
			state->itup[state->ituplen] = ptr->itup;
			state->ituplen++;
		}

		/* saves old rightlink */
		if (state->stack->blkno != GIST_ROOT_BLKNO)
			rrlink = GistPageGetOpaque(dist->page)->rightlink;

		START_CRIT_SECTION();

		/*
		 * must mark buffers dirty before XLogInsert, even though we'll still
		 * be changing their opaque fields below. set up right links.
		 */
		for (ptr = dist; ptr; ptr = ptr->next)
		{
			MarkBufferDirty(ptr->buffer);
			GistPageGetOpaque(ptr->page)->rightlink = (ptr->next) ?
				ptr->next->block.blkno : rrlink;
		}

		/* restore splitted non-root page */
		if (state->stack->blkno != GIST_ROOT_BLKNO)
		{
			PageRestoreTempPage(dist->page, BufferGetPage(dist->buffer));
			dist->page = BufferGetPage(dist->buffer);
		}

		if (!state->r->rd_istemp)
		{
			XLogRecPtr	recptr;
			XLogRecData *rdata;

			rdata = formSplitRdata(state->r->rd_node, state->stack->blkno,
								   is_leaf, &(state->key), dist);

			recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_PAGE_SPLIT, rdata);

			for (ptr = dist; ptr; ptr = ptr->next)
			{
				PageSetLSN(ptr->page, recptr);
				PageSetTLI(ptr->page, ThisTimeLineID);
			}
		}
		else
		{
			for (ptr = dist; ptr; ptr = ptr->next)
			{
				PageSetLSN(ptr->page, XLogRecPtrForTemp);
			}
		}

		/* set up NSN */
		oldnsn = GistPageGetOpaque(dist->page)->nsn;
		if (state->stack->blkno == GIST_ROOT_BLKNO)
			/* if root split we should put initial value */
			oldnsn = PageGetLSN(dist->page);

		for (ptr = dist; ptr; ptr = ptr->next)
		{
			/* only for last set oldnsn */
			GistPageGetOpaque(ptr->page)->nsn = (ptr->next) ?
				PageGetLSN(ptr->page) : oldnsn;
		}

		/*
		 * release buffers, if it was a root split then release all buffers
		 * because we create all buffers
		 */
		ptr = (state->stack->blkno == GIST_ROOT_BLKNO) ? dist : dist->next;
		for (; ptr; ptr = ptr->next)
			UnlockReleaseBuffer(ptr->buffer);

		if (state->stack->blkno == GIST_ROOT_BLKNO)
		{
			gistnewroot(state->r, state->stack->buffer, state->itup, state->ituplen, &(state->key));
			state->needInsertComplete = false;
		}

		END_CRIT_SECTION();
	}
	else
	{
		/* enough space */
		START_CRIT_SECTION();

		if (!is_leaf)
			PageIndexTupleDelete(state->stack->page, state->stack->childoffnum);
		gistfillbuffer(state->r, state->stack->page, state->itup, state->ituplen, InvalidOffsetNumber);

		MarkBufferDirty(state->stack->buffer);

		if (!state->r->rd_istemp)
		{
			OffsetNumber noffs = 0,
						offs[1];
			XLogRecPtr	recptr;
			XLogRecData *rdata;

			if (!is_leaf)
			{
				/* only on inner page we should delete previous version */
				offs[0] = state->stack->childoffnum;
				noffs = 1;
			}

			rdata = formUpdateRdata(state->r->rd_node, state->stack->buffer,
									offs, noffs,
									state->itup, state->ituplen,
									&(state->key));

			recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_PAGE_UPDATE, rdata);
			PageSetLSN(state->stack->page, recptr);
			PageSetTLI(state->stack->page, ThisTimeLineID);
		}
		else
			PageSetLSN(state->stack->page, XLogRecPtrForTemp);

		if (state->stack->blkno == GIST_ROOT_BLKNO)
			state->needInsertComplete = false;

		END_CRIT_SECTION();

		if (state->ituplen > 1)
		{						/* previous is_splitted==true */

			/*
			 * child was splited, so we must form union for insertion in
			 * parent
			 */
			IndexTuple	newtup = gistunion(state->r, state->itup, state->ituplen, giststate);

			ItemPointerSetBlockNumber(&(newtup->t_tid), state->stack->blkno);
			state->itup[0] = newtup;
			state->ituplen = 1;
		}
		else if (is_leaf)
		{
			/*
			 * itup[0] store key to adjust parent, we set it to valid to
			 * correct check by GistTupleIsInvalid macro in gistgetadjusted()
			 */
			ItemPointerSetBlockNumber(&(state->itup[0]->t_tid), state->stack->blkno);
			GistTupleSetValid(state->itup[0]);
		}
	}
	return is_splitted;
}

/*
 * returns stack of pages, all pages in stack are pinned, and
 * leaf is X-locked
 */

static void
gistfindleaf(GISTInsertState *state, GISTSTATE *giststate)
{
	ItemId		iid;
	IndexTuple	idxtuple;
	GISTPageOpaque opaque;

	/*
	 * walk down, We don't lock page for a long time, but so we should be
	 * ready to recheck path in a bad case... We remember, that page->lsn
	 * should never be invalid.
	 */
	for (;;)
	{
		if (XLogRecPtrIsInvalid(state->stack->lsn))
			state->stack->buffer = ReadBuffer(state->r, state->stack->blkno);
		LockBuffer(state->stack->buffer, GIST_SHARE);
		gistcheckpage(state->r, state->stack->buffer);

		state->stack->page = (Page) BufferGetPage(state->stack->buffer);
		opaque = GistPageGetOpaque(state->stack->page);

		state->stack->lsn = PageGetLSN(state->stack->page);
		Assert(state->r->rd_istemp || !XLogRecPtrIsInvalid(state->stack->lsn));

		if (state->stack->blkno != GIST_ROOT_BLKNO &&
			XLByteLT(state->stack->parent->lsn, opaque->nsn))
		{
			/*
			 * caused split non-root page is detected, go up to parent to
			 * choose best child
			 */
			UnlockReleaseBuffer(state->stack->buffer);
			state->stack = state->stack->parent;
			continue;
		}

		if (!GistPageIsLeaf(state->stack->page))
		{
			/*
			 * This is an internal page, so continue to walk down the tree. We
			 * find the child node that has the minimum insertion penalty and
			 * recursively invoke ourselves to modify that node. Once the
			 * recursive call returns, we may need to adjust the parent node
			 * for two reasons: the child node split, or the key in this node
			 * needs to be adjusted for the newly inserted key below us.
			 */
			GISTInsertStack *item = (GISTInsertStack *) palloc0(sizeof(GISTInsertStack));

			state->stack->childoffnum = gistchoose(state->r, state->stack->page, state->itup[0], giststate);

			iid = PageGetItemId(state->stack->page, state->stack->childoffnum);
			idxtuple = (IndexTuple) PageGetItem(state->stack->page, iid);
			item->blkno = ItemPointerGetBlockNumber(&(idxtuple->t_tid));
			LockBuffer(state->stack->buffer, GIST_UNLOCK);

			item->parent = state->stack;
			item->child = NULL;
			if (state->stack)
				state->stack->child = item;
			state->stack = item;
		}
		else
		{
			/* be carefull, during unlock/lock page may be changed... */
			LockBuffer(state->stack->buffer, GIST_UNLOCK);
			LockBuffer(state->stack->buffer, GIST_EXCLUSIVE);
			state->stack->page = (Page) BufferGetPage(state->stack->buffer);
			opaque = GistPageGetOpaque(state->stack->page);

			if (state->stack->blkno == GIST_ROOT_BLKNO)
			{
				/*
				 * the only page can become inner instead of leaf is a root
				 * page, so for root we should recheck it
				 */
				if (!GistPageIsLeaf(state->stack->page))
				{
					/*
					 * very rarely situation: during unlock/lock index with
					 * number of pages = 1 was increased
					 */
					LockBuffer(state->stack->buffer, GIST_UNLOCK);
					continue;
				}

				/*
				 * we don't need to check root split, because checking
				 * leaf/inner is enough to recognize split for root
				 */

			}
			else if (XLByteLT(state->stack->parent->lsn, opaque->nsn))
			{
				/*
				 * detecting split during unlock/lock, so we should find
				 * better child on parent
				 */

				/* forget buffer */
				UnlockReleaseBuffer(state->stack->buffer);

				state->stack = state->stack->parent;
				continue;
			}

			state->stack->lsn = PageGetLSN(state->stack->page);

			/* ok we found a leaf page and it X-locked */
			break;
		}
	}

	/* now state->stack->(page, buffer and blkno) points to leaf page */
}

/*
 * Traverse the tree to find path from root page to specified "child" block.
 *
 * returns from the beginning of closest parent;
 *
 * To prevent deadlocks, this should lock only one page simultaneously.
 */
GISTInsertStack *
gistFindPath(Relation r, BlockNumber child)
{
	Page		page;
	Buffer		buffer;
	OffsetNumber i,
				maxoff;
	ItemId		iid;
	IndexTuple	idxtuple;
	GISTInsertStack *top,
			   *tail,
			   *ptr;
	BlockNumber blkno;

	top = tail = (GISTInsertStack *) palloc0(sizeof(GISTInsertStack));
	top->blkno = GIST_ROOT_BLKNO;

	while (top && top->blkno != child)
	{
		buffer = ReadBuffer(r, top->blkno);
		LockBuffer(buffer, GIST_SHARE);
		gistcheckpage(r, buffer);
		page = (Page) BufferGetPage(buffer);

		if (GistPageIsLeaf(page))
		{
			/* we can safety go away, follows only leaf pages */
			UnlockReleaseBuffer(buffer);
			return NULL;
		}

		top->lsn = PageGetLSN(page);

		if (top->parent && XLByteLT(top->parent->lsn, GistPageGetOpaque(page)->nsn) &&
			GistPageGetOpaque(page)->rightlink != InvalidBlockNumber /* sanity check */ )
		{
			/* page splited while we thinking of... */
			ptr = (GISTInsertStack *) palloc0(sizeof(GISTInsertStack));
			ptr->blkno = GistPageGetOpaque(page)->rightlink;
			ptr->childoffnum = InvalidOffsetNumber;
			ptr->parent = top;
			ptr->next = NULL;
			tail->next = ptr;
			tail = ptr;
		}

		maxoff = PageGetMaxOffsetNumber(page);

		for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
		{
			iid = PageGetItemId(page, i);
			idxtuple = (IndexTuple) PageGetItem(page, iid);
			blkno = ItemPointerGetBlockNumber(&(idxtuple->t_tid));
			if (blkno == child)
			{
				OffsetNumber poff = InvalidOffsetNumber;

				/* make childs links */
				ptr = top;
				while (ptr->parent)
				{
					/* set child link */
					ptr->parent->child = ptr;
					/* move childoffnum.. */
					if (ptr == top)
					{
						/* first iteration */
						poff = ptr->parent->childoffnum;
						ptr->parent->childoffnum = ptr->childoffnum;
					}
					else
					{
						OffsetNumber tmp = ptr->parent->childoffnum;

						ptr->parent->childoffnum = poff;
						poff = tmp;
					}
					ptr = ptr->parent;
				}
				top->childoffnum = i;
				UnlockReleaseBuffer(buffer);
				return top;
			}
			else
			{
				/* Install next inner page to the end of stack */
				ptr = (GISTInsertStack *) palloc0(sizeof(GISTInsertStack));
				ptr->blkno = blkno;
				ptr->childoffnum = i;	/* set offsetnumber of child to child
										 * !!! */
				ptr->parent = top;
				ptr->next = NULL;
				tail->next = ptr;
				tail = ptr;
			}
		}

		UnlockReleaseBuffer(buffer);
		top = top->next;
	}

	return NULL;
}


/*
 * Returns X-locked parent of stack page
 */

static void
gistFindCorrectParent(Relation r, GISTInsertStack *child)
{
	GISTInsertStack *parent = child->parent;

	LockBuffer(parent->buffer, GIST_EXCLUSIVE);
	gistcheckpage(r, parent->buffer);
	parent->page = (Page) BufferGetPage(parent->buffer);

	/* here we don't need to distinguish between split and page update */
	if (parent->childoffnum == InvalidOffsetNumber || !XLByteEQ(parent->lsn, PageGetLSN(parent->page)))
	{
		/* parent is changed, look child in right links until found */
		OffsetNumber i,
					maxoff;
		ItemId		iid;
		IndexTuple	idxtuple;
		GISTInsertStack *ptr;

		while (true)
		{
			maxoff = PageGetMaxOffsetNumber(parent->page);
			for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
			{
				iid = PageGetItemId(parent->page, i);
				idxtuple = (IndexTuple) PageGetItem(parent->page, iid);
				if (ItemPointerGetBlockNumber(&(idxtuple->t_tid)) == child->blkno)
				{
					/* yes!!, found */
					parent->childoffnum = i;
					return;
				}
			}

			parent->blkno = GistPageGetOpaque(parent->page)->rightlink;
			UnlockReleaseBuffer(parent->buffer);
			if (parent->blkno == InvalidBlockNumber)

				/*
				 * end of chain and still didn't found parent, It's very-very
				 * rare situation when root splited
				 */
				break;
			parent->buffer = ReadBuffer(r, parent->blkno);
			LockBuffer(parent->buffer, GIST_EXCLUSIVE);
			gistcheckpage(r, parent->buffer);
			parent->page = (Page) BufferGetPage(parent->buffer);
		}

		/*
		 * awful!!, we need search tree to find parent ... , but before we
		 * should release all old parent
		 */

		ptr = child->parent->parent;	/* child->parent already released
										 * above */
		while (ptr)
		{
			ReleaseBuffer(ptr->buffer);
			ptr = ptr->parent;
		}

		/* ok, find new path */
		ptr = parent = gistFindPath(r, child->blkno);
		Assert(ptr != NULL);

		/* read all buffers as expected by caller */
		/* note we don't lock them or gistcheckpage them here! */
		while (ptr)
		{
			ptr->buffer = ReadBuffer(r, ptr->blkno);
			ptr->page = (Page) BufferGetPage(ptr->buffer);
			ptr = ptr->parent;
		}

		/* install new chain of parents to stack */
		child->parent = parent;
		parent->child = child;

		/* make recursive call to normal processing */
		gistFindCorrectParent(r, child);
	}

	return;
}

void
gistmakedeal(GISTInsertState *state, GISTSTATE *giststate)
{
	int			is_splitted;
	ItemId		iid;
	IndexTuple	oldtup,
				newtup;

	/* walk up */
	while (true)
	{
		/*
		 * After this call: 1. if child page was splited, then itup contains
		 * keys for each page 2. if  child page wasn't splited, then itup
		 * contains additional for adjustment of current key
		 */

		if (state->stack->parent)
		{
			/*
			 * X-lock parent page before proceed child, gistFindCorrectParent
			 * should find and lock it
			 */
			gistFindCorrectParent(state->r, state->stack);
		}
		is_splitted = gistplacetopage(state, giststate);

		/* parent locked above, so release child buffer */
		UnlockReleaseBuffer(state->stack->buffer);

		/* pop parent page from stack */
		state->stack = state->stack->parent;

		/* stack is void */
		if (!state->stack)
			break;

		/*
		 * child did not split, so we can check is it needed to update parent
		 * tuple
		 */
		if (!is_splitted)
		{
			/* parent's tuple */
			iid = PageGetItemId(state->stack->page, state->stack->childoffnum);
			oldtup = (IndexTuple) PageGetItem(state->stack->page, iid);
			newtup = gistgetadjusted(state->r, oldtup, state->itup[0], giststate);

			if (!newtup)
			{					/* not need to update key */
				LockBuffer(state->stack->buffer, GIST_UNLOCK);
				break;
			}

			state->itup[0] = newtup;
		}
	}							/* while */

	/* release all parent buffers */
	while (state->stack)
	{
		ReleaseBuffer(state->stack->buffer);
		state->stack = state->stack->parent;
	}

	/* say to xlog that insert is completed */
	if (state->needInsertComplete && !state->r->rd_istemp)
		gistxlogInsertCompletion(state->r->rd_node, &(state->key), 1);
}

/*
 * gistSplit -- split a page in the tree and fill struct
 * used for XLOG and real writes buffers. Function is recursive, ie
 * it will split page until keys will fit in every page.
 */
SplitedPageLayout *
gistSplit(Relation r,
		  Page page,
		  IndexTuple *itup,		/* contains compressed entry */
		  int len,
		  GISTSTATE *giststate)
{
	IndexTuple *lvectup,
			   *rvectup;
	GistSplitVector v;
	GistEntryVector *entryvec;
	int			i;
	SplitedPageLayout *res = NULL;

	/* generate the item array */
	entryvec = palloc(GEVHDRSZ + (len + 1) * sizeof(GISTENTRY));
	entryvec->n = len + 1;

	memset(v.spl_lisnull, TRUE, sizeof(bool) * giststate->tupdesc->natts);
	memset(v.spl_risnull, TRUE, sizeof(bool) * giststate->tupdesc->natts);
	gistSplitByKey(r, page, itup, len, giststate,
				   &v, entryvec, 0);

	/* form left and right vector */
	lvectup = (IndexTuple *) palloc(sizeof(IndexTuple) * (len + 1));
	rvectup = (IndexTuple *) palloc(sizeof(IndexTuple) * (len + 1));

	for (i = 0; i < v.splitVector.spl_nleft; i++)
		lvectup[i] = itup[v.splitVector.spl_left[i] - 1];

	for (i = 0; i < v.splitVector.spl_nright; i++)
		rvectup[i] = itup[v.splitVector.spl_right[i] - 1];

	/* finalize splitting (may need another split) */
	if (!gistfitpage(rvectup, v.splitVector.spl_nright))
	{
		res = gistSplit(r, page, rvectup, v.splitVector.spl_nright, giststate);
	}
	else
	{
		ROTATEDIST(res);
		res->block.num = v.splitVector.spl_nright;
		res->list = gistfillitupvec(rvectup, v.splitVector.spl_nright, &(res->lenlist));
		res->itup = (v.spl_rightvalid) ? gistFormTuple(giststate, r, v.spl_rattr, v.spl_risnull, false)
			: gist_form_invalid_tuple(GIST_ROOT_BLKNO);
	}

	if (!gistfitpage(lvectup, v.splitVector.spl_nleft))
	{
		SplitedPageLayout *resptr,
				   *subres;

		resptr = subres = gistSplit(r, page, lvectup, v.splitVector.spl_nleft, giststate);

		/* install on list's tail */
		while (resptr->next)
			resptr = resptr->next;

		resptr->next = res;
		res = subres;
	}
	else
	{
		ROTATEDIST(res);
		res->block.num = v.splitVector.spl_nleft;
		res->list = gistfillitupvec(lvectup, v.splitVector.spl_nleft, &(res->lenlist));
		res->itup = (v.spl_leftvalid) ? gistFormTuple(giststate, r, v.spl_lattr, v.spl_lisnull, false)
			: gist_form_invalid_tuple(GIST_ROOT_BLKNO);
	}

	return res;
}

/*
 * buffer must be pinned and locked by caller
 */
void
gistnewroot(Relation r, Buffer buffer, IndexTuple *itup, int len, ItemPointer key)
{
	Page		page;

	Assert(BufferGetBlockNumber(buffer) == GIST_ROOT_BLKNO);
	page = BufferGetPage(buffer);

	START_CRIT_SECTION();

	GISTInitBuffer(buffer, 0);
	gistfillbuffer(r, page, itup, len, FirstOffsetNumber);

	MarkBufferDirty(buffer);

	if (!r->rd_istemp)
	{
		XLogRecPtr	recptr;
		XLogRecData *rdata;

		rdata = formUpdateRdata(r->rd_node, buffer,
								NULL, 0,
								itup, len, key);

		recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_NEW_ROOT, rdata);
		PageSetLSN(page, recptr);
		PageSetTLI(page, ThisTimeLineID);
	}
	else
		PageSetLSN(page, XLogRecPtrForTemp);

	END_CRIT_SECTION();
}

void
initGISTstate(GISTSTATE *giststate, Relation index)
{
	int			i;

	if (index->rd_att->natts > INDEX_MAX_KEYS)
		elog(ERROR, "numberOfAttributes %d > %d",
			 index->rd_att->natts, INDEX_MAX_KEYS);

	giststate->tupdesc = index->rd_att;

	for (i = 0; i < index->rd_att->natts; i++)
	{
		fmgr_info_copy(&(giststate->consistentFn[i]),
					   index_getprocinfo(index, i + 1, GIST_CONSISTENT_PROC),
					   CurrentMemoryContext);
		fmgr_info_copy(&(giststate->unionFn[i]),
					   index_getprocinfo(index, i + 1, GIST_UNION_PROC),
					   CurrentMemoryContext);
		fmgr_info_copy(&(giststate->compressFn[i]),
					   index_getprocinfo(index, i + 1, GIST_COMPRESS_PROC),
					   CurrentMemoryContext);
		fmgr_info_copy(&(giststate->decompressFn[i]),
					   index_getprocinfo(index, i + 1, GIST_DECOMPRESS_PROC),
					   CurrentMemoryContext);
		fmgr_info_copy(&(giststate->penaltyFn[i]),
					   index_getprocinfo(index, i + 1, GIST_PENALTY_PROC),
					   CurrentMemoryContext);
		fmgr_info_copy(&(giststate->picksplitFn[i]),
					   index_getprocinfo(index, i + 1, GIST_PICKSPLIT_PROC),
					   CurrentMemoryContext);
		fmgr_info_copy(&(giststate->equalFn[i]),
					   index_getprocinfo(index, i + 1, GIST_EQUAL_PROC),
					   CurrentMemoryContext);
	}
}

void
freeGISTstate(GISTSTATE *giststate)
{
	/* no work */
}
