/*-------------------------------------------------------------------------
 *
 * gist.c
 *	  interface routines for the postgres GiST index access method.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/gist/gist.c,v 1.127 2005/10/18 01:06:22 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/gist_private.h"
#include "access/gistscan.h"
#include "access/heapam.h"
#include "catalog/index.h"
#include "commands/vacuum.h"
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
			 GISTSTATE *GISTstate);
static void gistfindleaf(GISTInsertState *state,
			 GISTSTATE *giststate);


#define ROTATEDIST(d) do { \
	SplitedPageLayout *tmp=(SplitedPageLayout*)palloc(sizeof(SplitedPageLayout)); \
	memset(tmp,0,sizeof(SplitedPageLayout)); \
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
	double		reltuples;
	GISTBuildState buildstate;
	Buffer		buffer;

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
	GISTInitBuffer(buffer, F_LEAF);
	if (!index->rd_istemp)
	{
		XLogRecPtr	recptr;
		XLogRecData rdata;
		Page		page;

		rdata.buffer = InvalidBuffer;
		rdata.data = (char *) &(index->rd_node);
		rdata.len = sizeof(RelFileNode);
		rdata.next = NULL;

		page = BufferGetPage(buffer);

		START_CRIT_SECTION();

		recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_CREATE_INDEX, &rdata);
		PageSetLSN(page, recptr);
		PageSetTLI(page, ThisTimeLineID);

		END_CRIT_SECTION();
	}
	else
		PageSetLSN(BufferGetPage(buffer), XLogRecPtrForTemp);
	LockBuffer(buffer, GIST_UNLOCK);
	WriteBuffer(buffer);

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

	/* since we just counted the # of tuples, may as well update stats */
	IndexCloseAndUpdateStats(heap, reltuples, index, buildstate.indtuples);

	freeGISTstate(&buildstate.giststate);

	PG_RETURN_VOID();
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
	GISTENTRY	tmpcentry;
	int			i;
	MemoryContext oldCtx;

	/* GiST cannot index tuples with leading NULLs */
	if (isnull[0])
		return;

	oldCtx = MemoryContextSwitchTo(buildstate->tmpCtx);

	/* immediately compress keys to normalize */
	for (i = 0; i < buildstate->numindexattrs; i++)
	{
		if (isnull[i])
			values[i] = (Datum) 0;
		else
		{
			gistcentryinit(&buildstate->giststate, i, &tmpcentry, values[i],
						   NULL, NULL, (OffsetNumber) 0,
						   -1 /* size is currently bogus */ , TRUE, FALSE);
			values[i] = tmpcentry.key;
		}
	}

	/* form an index tuple and point it at the heap tuple */
	itup = index_form_tuple(buildstate->giststate.tupdesc, values, isnull);
	itup->t_tid = htup->t_self;

	/*
	 * Since we already have the index relation locked, we call gistdoinsert
	 * directly.  Normal access method calls dispatch through gistinsert,
	 * which locks the relation for write.	This is the right thing to do if
	 * you're inserting single tups, but not when you're initializing the
	 * whole index at once.
	 */
	gistdoinsert(index, itup, &buildstate->giststate);

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
	GISTENTRY	tmpentry;
	int			i;
	MemoryContext oldCtx;
	MemoryContext insertCtx;

	/* GiST cannot index tuples with leading NULLs */
	if (isnull[0])
		PG_RETURN_BOOL(false);

	insertCtx = createTempGistContext();
	oldCtx = MemoryContextSwitchTo(insertCtx);

	initGISTstate(&giststate, r);

	/* immediately compress keys to normalize */
	for (i = 0; i < r->rd_att->natts; i++)
	{
		if (isnull[i])
			values[i] = (Datum) 0;
		else
		{
			gistcentryinit(&giststate, i, &tmpentry, values[i],
						   NULL, NULL, (OffsetNumber) 0,
						   -1 /* size is currently bogus */ , TRUE, FALSE);
			values[i] = tmpentry.key;
		}
	}
	itup = index_form_tuple(giststate.tupdesc, values, isnull);
	itup->t_tid = *ht_ctid;

	gistdoinsert(r, itup, &giststate);

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
gistdoinsert(Relation r, IndexTuple itup, GISTSTATE *giststate)
{
	GISTInsertState state;

	memset(&state, 0, sizeof(GISTInsertState));

	state.itup = (IndexTuple *) palloc(sizeof(IndexTuple));
	state.itup[0] = (IndexTuple) palloc(IndexTupleSize(itup));
	memcpy(state.itup[0], itup, IndexTupleSize(itup));
	state.ituplen = 1;
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


	if (!is_leaf)

		/*
		 * This node's key has been modified, either because a child split
		 * occurred or because we needed to adjust our key for an insert in a
		 * child node. Therefore, remove the old version of this node's key.
		 */

		PageIndexTupleDelete(state->stack->page, state->stack->childoffnum);

	if (gistnospace(state->stack->page, state->itup, state->ituplen))
	{
		/* no space for insertion */
		IndexTuple *itvec,
				   *newitup;
		int			tlen;
		SplitedPageLayout *dist = NULL,
				   *ptr;

		is_splitted = true;
		itvec = gistextractbuffer(state->stack->buffer, &tlen);
		itvec = gistjoinvector(itvec, &tlen, state->itup, state->ituplen);
		newitup = gistSplit(state->r, state->stack->buffer, itvec, &tlen, &dist, giststate);

		if (!state->r->rd_istemp)
		{
			XLogRecPtr	recptr;
			XLogRecData *rdata;

			rdata = formSplitRdata(state->r->rd_node, state->stack->blkno,
								   &(state->key), dist);

			START_CRIT_SECTION();

			recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_PAGE_SPLIT, rdata);
			ptr = dist;
			while (ptr)
			{
				PageSetLSN(BufferGetPage(ptr->buffer), recptr);
				PageSetTLI(BufferGetPage(ptr->buffer), ThisTimeLineID);
				ptr = ptr->next;
			}

			END_CRIT_SECTION();
		}
		else
		{
			ptr = dist;
			while (ptr)
			{
				PageSetLSN(BufferGetPage(ptr->buffer), XLogRecPtrForTemp);
				ptr = ptr->next;
			}
		}

		state->itup = newitup;
		state->ituplen = tlen;	/* now tlen >= 2 */

		if (state->stack->blkno == GIST_ROOT_BLKNO)
		{
			gistnewroot(state->r, state->stack->buffer, state->itup, state->ituplen, &(state->key));
			state->needInsertComplete = false;
			ptr = dist;
			while (ptr)
			{
				Page		page = (Page) BufferGetPage(ptr->buffer);

				GistPageGetOpaque(page)->rightlink = (ptr->next) ?
					ptr->next->block.blkno : InvalidBlockNumber;
				GistPageGetOpaque(page)->nsn = PageGetLSN(page);
				LockBuffer(ptr->buffer, GIST_UNLOCK);
				WriteBuffer(ptr->buffer);
				ptr = ptr->next;
			}
		}
		else
		{
			Page		page;
			BlockNumber rightrightlink = InvalidBlockNumber;
			SplitedPageLayout *ourpage = NULL;
			GistNSN		oldnsn;
			GISTPageOpaque opaque;

			/* move origpage to first in chain */
			if (dist->block.blkno != state->stack->blkno)
			{
				ptr = dist;
				while (ptr->next)
				{
					if (ptr->next->block.blkno == state->stack->blkno)
					{
						ourpage = ptr->next;
						ptr->next = ptr->next->next;
						ourpage->next = dist;
						dist = ourpage;
						break;
					}
					ptr = ptr->next;
				}
				Assert(ourpage != NULL);
			}
			else
				ourpage = dist;


			/* now gets all needed data, and sets nsn's */
			page = (Page) BufferGetPage(ourpage->buffer);
			opaque = GistPageGetOpaque(page);
			rightrightlink = opaque->rightlink;
			oldnsn = opaque->nsn;
			opaque->nsn = PageGetLSN(page);
			opaque->rightlink = ourpage->next->block.blkno;

			/*
			 * fills and write all new pages. They isn't linked into tree yet
			 */

			ptr = ourpage->next;
			while (ptr)
			{
				page = (Page) BufferGetPage(ptr->buffer);
				GistPageGetOpaque(page)->rightlink = (ptr->next) ?
					ptr->next->block.blkno : rightrightlink;
				/* only for last set oldnsn */
				GistPageGetOpaque(page)->nsn = (ptr->next) ?
					opaque->nsn : oldnsn;

				LockBuffer(ptr->buffer, GIST_UNLOCK);
				WriteBuffer(ptr->buffer);
				ptr = ptr->next;
			}
		}
		WriteNoReleaseBuffer(state->stack->buffer);
	}
	else
	{
		/* enough space */
		XLogRecPtr	oldlsn;

		gistfillbuffer(state->r, state->stack->page, state->itup, state->ituplen, InvalidOffsetNumber);

		oldlsn = PageGetLSN(state->stack->page);
		if (!state->r->rd_istemp)
		{
			OffsetNumber noffs = 0,
						offs[MAXALIGN(sizeof(OffsetNumber)) / sizeof(OffsetNumber)];
			XLogRecPtr	recptr;
			XLogRecData *rdata;

			if (!is_leaf)
			{
				/* only on inner page we should delete previous version */
				offs[0] = state->stack->childoffnum;
				noffs = 1;
			}

			rdata = formUpdateRdata(state->r->rd_node, state->stack->blkno,
							 offs, noffs, false, state->itup, state->ituplen,
									&(state->key));

			START_CRIT_SECTION();

			recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_ENTRY_UPDATE, rdata);
			PageSetLSN(state->stack->page, recptr);
			PageSetTLI(state->stack->page, ThisTimeLineID);

			END_CRIT_SECTION();
		}
		else
			PageSetLSN(state->stack->page, XLogRecPtrForTemp);

		if (state->stack->blkno == GIST_ROOT_BLKNO)
			state->needInsertComplete = false;
		WriteNoReleaseBuffer(state->stack->buffer);

		if (!is_leaf)			/* small optimization: inform scan ablout
								 * deleting... */
			gistadjscans(state->r, GISTOP_DEL, state->stack->blkno,
						 state->stack->childoffnum, PageGetLSN(state->stack->page), oldlsn);

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
	while (true)
	{

		if (XLogRecPtrIsInvalid(state->stack->lsn))
			state->stack->buffer = ReadBuffer(state->r, state->stack->blkno);
		LockBuffer(state->stack->buffer, GIST_SHARE);

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
			LockBuffer(state->stack->buffer, GIST_UNLOCK);
			ReleaseBuffer(state->stack->buffer);
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
				LockBuffer(state->stack->buffer, GIST_UNLOCK);
				ReleaseBuffer(state->stack->buffer);

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
 * Should have the same interface as XLogReadBuffer
 */
static Buffer
gistReadAndLockBuffer(Relation r, BlockNumber blkno)
{
	Buffer		buffer = ReadBuffer(r, blkno);

	LockBuffer(buffer, GIST_SHARE);
	return buffer;
}

/*
 * Traverse the tree to find path from root page,
 * to prevent deadlocks, it should lock only one page simultaneously.
 * Function uses in recovery and usial mode, so should work with different
 * read functions (gistReadAndLockBuffer and XLogReadBuffer)
 * returns from the begining of closest parent;
 */
GISTInsertStack *
gistFindPath(Relation r, BlockNumber child, Buffer (*myReadBuffer) (Relation, BlockNumber))
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
		buffer = myReadBuffer(r, top->blkno);	/* buffer locked */
		page = (Page) BufferGetPage(buffer);

		if (GistPageIsLeaf(page))
		{
			/* we can safety go away, follows only leaf pages */
			LockBuffer(buffer, GIST_UNLOCK);
			ReleaseBuffer(buffer);
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
				LockBuffer(buffer, GIST_UNLOCK);
				ReleaseBuffer(buffer);
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

		LockBuffer(buffer, GIST_UNLOCK);
		ReleaseBuffer(buffer);
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
			LockBuffer(parent->buffer, GIST_UNLOCK);
			ReleaseBuffer(parent->buffer);
			if (parent->blkno == InvalidBlockNumber)

				/*
				 * end of chain and still didn't found parent, It's very-very
				 * rare situation when root splited
				 */
				break;
			parent->buffer = ReadBuffer(r, parent->blkno);
			LockBuffer(parent->buffer, GIST_EXCLUSIVE);
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
		ptr = parent = gistFindPath(r, child->blkno, gistReadAndLockBuffer);
		Assert(ptr != NULL);

		/* read all buffers as supposed in caller */
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
		LockBuffer(state->stack->buffer, GIST_UNLOCK);
		ReleaseBuffer(state->stack->buffer);

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

static void
gistToRealOffset(OffsetNumber *arr, int len, OffsetNumber *reasloffset)
{
	int			i;

	for (i = 0; i < len; i++)
		arr[i] = reasloffset[arr[i]];
}

/*
 *	gistSplit -- split a page in the tree.
 */
IndexTuple *
gistSplit(Relation r,
		  Buffer buffer,
		  IndexTuple *itup,		/* contains compressed entry */
		  int *len,
		  SplitedPageLayout **dist,
		  GISTSTATE *giststate)
{
	Page		p;
	Buffer		leftbuf,
				rightbuf;
	Page		left,
				right;
	IndexTuple *lvectup,
			   *rvectup,
			   *newtup;
	BlockNumber lbknum,
				rbknum;
	GISTPageOpaque opaque;
	GIST_SPLITVEC v;
	GistEntryVector *entryvec;
	int			i,
				fakeoffset,
				nlen;
	OffsetNumber *realoffset;
	IndexTuple *cleaneditup = itup;
	int			lencleaneditup = *len;

	p = (Page) BufferGetPage(buffer);
	opaque = GistPageGetOpaque(p);

	/*
	 * The root of the tree is the first block in the relation.  If we're
	 * about to split the root, we need to do some hocus-pocus to enforce this
	 * guarantee.
	 */
	if (BufferGetBlockNumber(buffer) == GIST_ROOT_BLKNO)
	{
		leftbuf = gistNewBuffer(r);
		GISTInitBuffer(leftbuf, opaque->flags & F_LEAF);
		lbknum = BufferGetBlockNumber(leftbuf);
		left = (Page) BufferGetPage(leftbuf);
	}
	else
	{
		leftbuf = buffer;
		/* IncrBufferRefCount(buffer); */
		lbknum = BufferGetBlockNumber(buffer);
		left = (Page) PageGetTempPage(p, sizeof(GISTPageOpaqueData));
	}

	rightbuf = gistNewBuffer(r);
	GISTInitBuffer(rightbuf, opaque->flags & F_LEAF);
	rbknum = BufferGetBlockNumber(rightbuf);
	right = (Page) BufferGetPage(rightbuf);

	/* generate the item array */
	realoffset = palloc((*len + 1) * sizeof(OffsetNumber));
	entryvec = palloc(GEVHDRSZ + (*len + 1) * sizeof(GISTENTRY));
	entryvec->n = *len + 1;

	fakeoffset = FirstOffsetNumber;
	for (i = 1; i <= *len; i++)
	{
		Datum		datum;
		bool		IsNull;

		if (!GistPageIsLeaf(p) && GistTupleIsInvalid(itup[i - 1]))
		{
			entryvec->n--;
			/* remember position of invalid tuple */
			realoffset[entryvec->n] = i;
			continue;
		}

		datum = index_getattr(itup[i - 1], 1, giststate->tupdesc, &IsNull);
		gistdentryinit(giststate, 0, &(entryvec->vector[fakeoffset]),
					   datum, r, p, i,
					   ATTSIZE(datum, giststate->tupdesc, 1, IsNull),
					   FALSE, IsNull);
		realoffset[fakeoffset] = i;
		fakeoffset++;
	}

	/*
	 * if it was invalid tuple then we need special processing. If it's
	 * possible, we move all invalid tuples on right page. We should remember,
	 * that union with invalid tuples is a invalid tuple.
	 */
	if (entryvec->n != *len + 1)
	{
		lencleaneditup = entryvec->n - 1;
		cleaneditup = (IndexTuple *) palloc(lencleaneditup * sizeof(IndexTuple));
		for (i = 1; i < entryvec->n; i++)
			cleaneditup[i - 1] = itup[realoffset[i] - 1];

		if (gistnospace(left, cleaneditup, lencleaneditup))
		{
			/* no space on left to put all good tuples, so picksplit */
			gistUserPicksplit(r, entryvec, &v, cleaneditup, lencleaneditup, giststate);
			v.spl_leftvalid = true;
			v.spl_rightvalid = false;
			gistToRealOffset(v.spl_left, v.spl_nleft, realoffset);
			gistToRealOffset(v.spl_right, v.spl_nright, realoffset);
		}
		else
		{
			/* we can try to store all valid tuples on one page */
			v.spl_right = (OffsetNumber *) palloc(entryvec->n * sizeof(OffsetNumber));
			v.spl_left = (OffsetNumber *) palloc(entryvec->n * sizeof(OffsetNumber));

			if (lencleaneditup == 0)
			{
				/* all tuples are invalid, so moves half of its to right */
				v.spl_leftvalid = v.spl_rightvalid = false;
				v.spl_nright = 0;
				v.spl_nleft = 0;
				for (i = 1; i <= *len; i++)
					if (i - 1 < *len / 2)
						v.spl_left[v.spl_nleft++] = i;
					else
						v.spl_right[v.spl_nright++] = i;
			}
			else
			{
				/*
				 * we will not call gistUserPicksplit, just put good tuples on
				 * left and invalid on right
				 */
				v.spl_nleft = lencleaneditup;
				v.spl_nright = 0;
				for (i = 1; i < entryvec->n; i++)
					v.spl_left[i - 1] = i;
				gistToRealOffset(v.spl_left, v.spl_nleft, realoffset);
				v.spl_lattr[0] = v.spl_ldatum = (Datum) 0;
				v.spl_rattr[0] = v.spl_rdatum = (Datum) 0;
				v.spl_lisnull[0] = true;
				v.spl_risnull[0] = true;
				gistunionsubkey(r, giststate, itup, &v, true);
				v.spl_leftvalid = true;
				v.spl_rightvalid = false;
			}
		}
	}
	else
	{
		/* there is no invalid tuples, so usial processing */
		gistUserPicksplit(r, entryvec, &v, itup, *len, giststate);
		v.spl_leftvalid = v.spl_rightvalid = true;
	}


	/* form left and right vector */
	lvectup = (IndexTuple *) palloc(sizeof(IndexTuple) * (*len + 1));
	rvectup = (IndexTuple *) palloc(sizeof(IndexTuple) * (*len + 1));

	for (i = 0; i < v.spl_nleft; i++)
		lvectup[i] = itup[v.spl_left[i] - 1];

	for (i = 0; i < v.spl_nright; i++)
		rvectup[i] = itup[v.spl_right[i] - 1];

	/* place invalid tuples on right page if itsn't done yet */
	for (fakeoffset = entryvec->n; fakeoffset < *len + 1 && lencleaneditup; fakeoffset++)
	{
		rvectup[v.spl_nright++] = itup[realoffset[fakeoffset] - 1];
	}

	/* write on disk (may need another split) */
	if (gistnospace(right, rvectup, v.spl_nright))
	{
		nlen = v.spl_nright;
		newtup = gistSplit(r, rightbuf, rvectup, &nlen, dist, giststate);
		/* ReleaseBuffer(rightbuf); */
	}
	else
	{
		char	   *ptr;

		gistfillbuffer(r, right, rvectup, v.spl_nright, FirstOffsetNumber);
		/* XLOG stuff */
		ROTATEDIST(*dist);
		(*dist)->block.blkno = BufferGetBlockNumber(rightbuf);
		(*dist)->block.num = v.spl_nright;
		(*dist)->list = (IndexTupleData *) palloc(BLCKSZ);
		ptr = (char *) ((*dist)->list);
		for (i = 0; i < v.spl_nright; i++)
		{
			memcpy(ptr, rvectup[i], IndexTupleSize(rvectup[i]));
			ptr += IndexTupleSize(rvectup[i]);
		}
		(*dist)->lenlist = ptr - ((char *) ((*dist)->list));
		(*dist)->buffer = rightbuf;

		nlen = 1;
		newtup = (IndexTuple *) palloc(sizeof(IndexTuple) * 1);
		newtup[0] = (v.spl_rightvalid) ? gistFormTuple(giststate, r, v.spl_rattr, v.spl_rattrsize, v.spl_risnull)
			: gist_form_invalid_tuple(rbknum);
		ItemPointerSetBlockNumber(&(newtup[0]->t_tid), rbknum);
	}

	if (gistnospace(left, lvectup, v.spl_nleft))
	{
		int			llen = v.spl_nleft;
		IndexTuple *lntup;

		lntup = gistSplit(r, leftbuf, lvectup, &llen, dist, giststate);
		/* ReleaseBuffer(leftbuf); */

		newtup = gistjoinvector(newtup, &nlen, lntup, llen);
	}
	else
	{
		char	   *ptr;

		gistfillbuffer(r, left, lvectup, v.spl_nleft, FirstOffsetNumber);
		/* XLOG stuff */
		ROTATEDIST(*dist);
		(*dist)->block.blkno = BufferGetBlockNumber(leftbuf);
		(*dist)->block.num = v.spl_nleft;
		(*dist)->list = (IndexTupleData *) palloc(BLCKSZ);
		ptr = (char *) ((*dist)->list);
		for (i = 0; i < v.spl_nleft; i++)
		{
			memcpy(ptr, lvectup[i], IndexTupleSize(lvectup[i]));
			ptr += IndexTupleSize(lvectup[i]);
		}
		(*dist)->lenlist = ptr - ((char *) ((*dist)->list));
		(*dist)->buffer = leftbuf;

		if (BufferGetBlockNumber(buffer) != GIST_ROOT_BLKNO)
			PageRestoreTempPage(left, p);

		nlen += 1;
		newtup = (IndexTuple *) repalloc(newtup, sizeof(IndexTuple) * nlen);
		newtup[nlen - 1] = (v.spl_leftvalid) ? gistFormTuple(giststate, r, v.spl_lattr, v.spl_lattrsize, v.spl_lisnull)
			: gist_form_invalid_tuple(lbknum);
		ItemPointerSetBlockNumber(&(newtup[nlen - 1]->t_tid), lbknum);
	}

	GistClearTuplesDeleted(p);

	*len = nlen;
	return newtup;
}

void
gistnewroot(Relation r, Buffer buffer, IndexTuple *itup, int len, ItemPointer key)
{
	Page		page;

	Assert(BufferGetBlockNumber(buffer) == GIST_ROOT_BLKNO);
	page = BufferGetPage(buffer);
	GISTInitBuffer(buffer, 0);

	gistfillbuffer(r, page, itup, len, FirstOffsetNumber);
	if (!r->rd_istemp)
	{
		XLogRecPtr	recptr;
		XLogRecData *rdata;

		rdata = formUpdateRdata(r->rd_node, GIST_ROOT_BLKNO,
								NULL, 0, false, itup, len, key);

		START_CRIT_SECTION();

		recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_NEW_ROOT, rdata);
		PageSetLSN(page, recptr);
		PageSetTLI(page, ThisTimeLineID);

		END_CRIT_SECTION();
	}
	else
		PageSetLSN(page, XLogRecPtrForTemp);
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
