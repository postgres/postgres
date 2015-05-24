/*-------------------------------------------------------------------------
 *
 * gist.c
 *	  interface routines for the postgres GiST index access method.
 *
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/gist/gist.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/gist_private.h"
#include "access/xloginsert.h"
#include "catalog/index.h"
#include "catalog/pg_collation.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/* non-export function prototypes */
static void gistfixsplit(GISTInsertState *state, GISTSTATE *giststate);
static bool gistinserttuple(GISTInsertState *state, GISTInsertStack *stack,
			 GISTSTATE *giststate, IndexTuple tuple, OffsetNumber oldoffnum);
static bool gistinserttuples(GISTInsertState *state, GISTInsertStack *stack,
				 GISTSTATE *giststate,
				 IndexTuple *tuples, int ntup, OffsetNumber oldoffnum,
				 Buffer leftchild, Buffer rightchild,
				 bool unlockbuf, bool unlockleftchild);
static void gistfinishsplit(GISTInsertState *state, GISTInsertStack *stack,
				GISTSTATE *giststate, List *splitinfo, bool releasebuf);


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
 *	gistbuildempty() -- build an empty gist index in the initialization fork
 */
Datum
gistbuildempty(PG_FUNCTION_ARGS)
{
	Relation	index = (Relation) PG_GETARG_POINTER(0);
	Buffer		buffer;

	/* Initialize the root page */
	buffer = ReadBufferExtended(index, INIT_FORKNUM, P_NEW, RBM_NORMAL, NULL);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	/* Initialize and xlog buffer */
	START_CRIT_SECTION();
	GISTInitBuffer(buffer, F_LEAF);
	MarkBufferDirty(buffer);
	log_newpage_buffer(buffer, true);
	END_CRIT_SECTION();

	/* Unlock and release the buffer */
	UnlockReleaseBuffer(buffer);

	PG_RETURN_VOID();
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
	IndexUniqueCheck checkUnique = (IndexUniqueCheck) PG_GETARG_INT32(5);
#endif
	IndexTuple	itup;
	GISTSTATE  *giststate;
	MemoryContext oldCxt;

	giststate = initGISTstate(r);

	/*
	 * We use the giststate's scan context as temp context too.  This means
	 * that any memory leaked by the support functions is not reclaimed until
	 * end of insert.  In most cases, we aren't going to call the support
	 * functions very many times before finishing the insert, so this seems
	 * cheaper than resetting a temp context for each function call.
	 */
	oldCxt = MemoryContextSwitchTo(giststate->tempCxt);

	itup = gistFormTuple(giststate, r,
						 values, isnull, true /* size is currently bogus */ );
	itup->t_tid = *ht_ctid;

	gistdoinsert(r, itup, 0, giststate);

	/* cleanup */
	MemoryContextSwitchTo(oldCxt);
	freeGISTstate(giststate);

	PG_RETURN_BOOL(false);
}


/*
 * Place tuples from 'itup' to 'buffer'. If 'oldoffnum' is valid, the tuple
 * at that offset is atomically removed along with inserting the new tuples.
 * This is used to replace a tuple with a new one.
 *
 * If 'leftchildbuf' is valid, we're inserting the downlink for the page
 * to the right of 'leftchildbuf', or updating the downlink for 'leftchildbuf'.
 * F_FOLLOW_RIGHT flag on 'leftchildbuf' is cleared and NSN is set.
 *
 * If 'markfollowright' is true and the page is split, the left child is
 * marked with F_FOLLOW_RIGHT flag. That is the normal case. During buffered
 * index build, however, there is no concurrent access and the page splitting
 * is done in a slightly simpler fashion, and false is passed.
 *
 * If there is not enough room on the page, it is split. All the split
 * pages are kept pinned and locked and returned in *splitinfo, the caller
 * is responsible for inserting the downlinks for them. However, if
 * 'buffer' is the root page and it needs to be split, gistplacetopage()
 * performs the split as one atomic operation, and *splitinfo is set to NIL.
 * In that case, we continue to hold the root page locked, and the child
 * pages are released; note that new tuple(s) are *not* on the root page
 * but in one of the new child pages.
 *
 * If 'newblkno' is not NULL, returns the block number of page the first
 * new/updated tuple was inserted to. Usually it's the given page, but could
 * be its right sibling if the page was split.
 *
 * Returns 'true' if the page was split, 'false' otherwise.
 */
bool
gistplacetopage(Relation rel, Size freespace, GISTSTATE *giststate,
				Buffer buffer,
				IndexTuple *itup, int ntup, OffsetNumber oldoffnum,
				BlockNumber *newblkno,
				Buffer leftchildbuf,
				List **splitinfo,
				bool markfollowright)
{
	BlockNumber blkno = BufferGetBlockNumber(buffer);
	Page		page = BufferGetPage(buffer);
	bool		is_leaf = (GistPageIsLeaf(page)) ? true : false;
	XLogRecPtr	recptr;
	int			i;
	bool		is_split;

	/*
	 * Refuse to modify a page that's incompletely split. This should not
	 * happen because we finish any incomplete splits while we walk down the
	 * tree. However, it's remotely possible that another concurrent inserter
	 * splits a parent page, and errors out before completing the split. We
	 * will just throw an error in that case, and leave any split we had in
	 * progress unfinished too. The next insert that comes along will clean up
	 * the mess.
	 */
	if (GistFollowRight(page))
		elog(ERROR, "concurrent GiST page split was incomplete");

	*splitinfo = NIL;

	/*
	 * if isupdate, remove old key: This node's key has been modified, either
	 * because a child split occurred or because we needed to adjust our key
	 * for an insert in a child node. Therefore, remove the old version of
	 * this node's key.
	 *
	 * for WAL replay, in the non-split case we handle this by setting up a
	 * one-element todelete array; in the split case, it's handled implicitly
	 * because the tuple vector passed to gistSplit won't include this tuple.
	 */
	is_split = gistnospace(page, itup, ntup, oldoffnum, freespace);
	if (is_split)
	{
		/* no space for insertion */
		IndexTuple *itvec;
		int			tlen;
		SplitedPageLayout *dist = NULL,
				   *ptr;
		BlockNumber oldrlink = InvalidBlockNumber;
		GistNSN		oldnsn = 0;
		SplitedPageLayout rootpg;
		bool		is_rootsplit;
		int			npage;

		is_rootsplit = (blkno == GIST_ROOT_BLKNO);

		/*
		 * Form index tuples vector to split. If we're replacing an old tuple,
		 * remove the old version from the vector.
		 */
		itvec = gistextractpage(page, &tlen);
		if (OffsetNumberIsValid(oldoffnum))
		{
			/* on inner page we should remove old tuple */
			int			pos = oldoffnum - FirstOffsetNumber;

			tlen--;
			if (pos != tlen)
				memmove(itvec + pos, itvec + pos + 1, sizeof(IndexTuple) * (tlen - pos));
		}
		itvec = gistjoinvector(itvec, &tlen, itup, ntup);
		dist = gistSplit(rel, page, itvec, tlen, giststate);

		/*
		 * Check that split didn't produce too many pages.
		 */
		npage = 0;
		for (ptr = dist; ptr; ptr = ptr->next)
			npage++;
		/* in a root split, we'll add one more page to the list below */
		if (is_rootsplit)
			npage++;
		if (npage > GIST_MAX_SPLIT_PAGES)
			elog(ERROR, "GiST page split into too many halves (%d, maximum %d)",
				 npage, GIST_MAX_SPLIT_PAGES);

		/*
		 * Set up pages to work with. Allocate new buffers for all but the
		 * leftmost page. The original page becomes the new leftmost page, and
		 * is just replaced with the new contents.
		 *
		 * For a root-split, allocate new buffers for all child pages, the
		 * original page is overwritten with new root page containing
		 * downlinks to the new child pages.
		 */
		ptr = dist;
		if (!is_rootsplit)
		{
			/* save old rightlink and NSN */
			oldrlink = GistPageGetOpaque(page)->rightlink;
			oldnsn = GistPageGetNSN(page);

			dist->buffer = buffer;
			dist->block.blkno = BufferGetBlockNumber(buffer);
			dist->page = PageGetTempPageCopySpecial(BufferGetPage(buffer));

			/* clean all flags except F_LEAF */
			GistPageGetOpaque(dist->page)->flags = (is_leaf) ? F_LEAF : 0;

			ptr = ptr->next;
		}
		for (; ptr; ptr = ptr->next)
		{
			/* Allocate new page */
			ptr->buffer = gistNewBuffer(rel);
			GISTInitBuffer(ptr->buffer, (is_leaf) ? F_LEAF : 0);
			ptr->page = BufferGetPage(ptr->buffer);
			ptr->block.blkno = BufferGetBlockNumber(ptr->buffer);
		}

		/*
		 * Now that we know which blocks the new pages go to, set up downlink
		 * tuples to point to them.
		 */
		for (ptr = dist; ptr; ptr = ptr->next)
		{
			ItemPointerSetBlockNumber(&(ptr->itup->t_tid), ptr->block.blkno);
			GistTupleSetValid(ptr->itup);
		}

		/*
		 * If this is a root split, we construct the new root page with the
		 * downlinks here directly, instead of requiring the caller to insert
		 * them. Add the new root page to the list along with the child pages.
		 */
		if (is_rootsplit)
		{
			IndexTuple *downlinks;
			int			ndownlinks = 0;
			int			i;

			rootpg.buffer = buffer;
			rootpg.page = PageGetTempPageCopySpecial(BufferGetPage(rootpg.buffer));
			GistPageGetOpaque(rootpg.page)->flags = 0;

			/* Prepare a vector of all the downlinks */
			for (ptr = dist; ptr; ptr = ptr->next)
				ndownlinks++;
			downlinks = palloc(sizeof(IndexTuple) * ndownlinks);
			for (i = 0, ptr = dist; ptr; ptr = ptr->next)
				downlinks[i++] = ptr->itup;

			rootpg.block.blkno = GIST_ROOT_BLKNO;
			rootpg.block.num = ndownlinks;
			rootpg.list = gistfillitupvec(downlinks, ndownlinks,
										  &(rootpg.lenlist));
			rootpg.itup = NULL;

			rootpg.next = dist;
			dist = &rootpg;
		}
		else
		{
			/* Prepare split-info to be returned to caller */
			for (ptr = dist; ptr; ptr = ptr->next)
			{
				GISTPageSplitInfo *si = palloc(sizeof(GISTPageSplitInfo));

				si->buf = ptr->buffer;
				si->downlink = ptr->itup;
				*splitinfo = lappend(*splitinfo, si);
			}
		}

		/*
		 * Fill all pages. All the pages are new, ie. freshly allocated empty
		 * pages, or a temporary copy of the old page.
		 */
		for (ptr = dist; ptr; ptr = ptr->next)
		{
			char	   *data = (char *) (ptr->list);

			for (i = 0; i < ptr->block.num; i++)
			{
				IndexTuple	thistup = (IndexTuple) data;

				if (PageAddItem(ptr->page, (Item) data, IndexTupleSize(thistup), i + FirstOffsetNumber, false, false) == InvalidOffsetNumber)
					elog(ERROR, "failed to add item to index page in \"%s\"", RelationGetRelationName(rel));

				/*
				 * If this is the first inserted/updated tuple, let the caller
				 * know which page it landed on.
				 */
				if (newblkno && ItemPointerEquals(&thistup->t_tid, &(*itup)->t_tid))
					*newblkno = ptr->block.blkno;

				data += IndexTupleSize(thistup);
			}

			/* Set up rightlinks */
			if (ptr->next && ptr->block.blkno != GIST_ROOT_BLKNO)
				GistPageGetOpaque(ptr->page)->rightlink =
					ptr->next->block.blkno;
			else
				GistPageGetOpaque(ptr->page)->rightlink = oldrlink;

			/*
			 * Mark the all but the right-most page with the follow-right
			 * flag. It will be cleared as soon as the downlink is inserted
			 * into the parent, but this ensures that if we error out before
			 * that, the index is still consistent. (in buffering build mode,
			 * any error will abort the index build anyway, so this is not
			 * needed.)
			 */
			if (ptr->next && !is_rootsplit && markfollowright)
				GistMarkFollowRight(ptr->page);
			else
				GistClearFollowRight(ptr->page);

			/*
			 * Copy the NSN of the original page to all pages. The
			 * F_FOLLOW_RIGHT flags ensure that scans will follow the
			 * rightlinks until the downlinks are inserted.
			 */
			GistPageSetNSN(ptr->page, oldnsn);
		}

		/*
		 * gistXLogSplit() needs to WAL log a lot of pages, prepare WAL
		 * insertion for that. NB: The number of pages and data segments
		 * specified here must match the calculations in gistXLogSplit()!
		 */
		if (RelationNeedsWAL(rel))
			XLogEnsureRecordSpace(npage, 1 + npage * 2);

		START_CRIT_SECTION();

		/*
		 * Must mark buffers dirty before XLogInsert, even though we'll still
		 * be changing their opaque fields below.
		 */
		for (ptr = dist; ptr; ptr = ptr->next)
			MarkBufferDirty(ptr->buffer);
		if (BufferIsValid(leftchildbuf))
			MarkBufferDirty(leftchildbuf);

		/*
		 * The first page in the chain was a temporary working copy meant to
		 * replace the old page. Copy it over the old page.
		 */
		PageRestoreTempPage(dist->page, BufferGetPage(dist->buffer));
		dist->page = BufferGetPage(dist->buffer);

		/* Write the WAL record */
		if (RelationNeedsWAL(rel))
			recptr = gistXLogSplit(rel->rd_node, blkno, is_leaf,
								   dist, oldrlink, oldnsn, leftchildbuf,
								   markfollowright);
		else
			recptr = gistGetFakeLSN(rel);

		for (ptr = dist; ptr; ptr = ptr->next)
		{
			PageSetLSN(ptr->page, recptr);
		}

		/*
		 * Return the new child buffers to the caller.
		 *
		 * If this was a root split, we've already inserted the downlink
		 * pointers, in the form of a new root page. Therefore we can release
		 * all the new buffers, and keep just the root page locked.
		 */
		if (is_rootsplit)
		{
			for (ptr = dist->next; ptr; ptr = ptr->next)
				UnlockReleaseBuffer(ptr->buffer);
		}
	}
	else
	{
		/*
		 * Enough space. We also get here if ntuples==0.
		 */
		START_CRIT_SECTION();

		if (OffsetNumberIsValid(oldoffnum))
			PageIndexTupleDelete(page, oldoffnum);
		gistfillbuffer(page, itup, ntup, InvalidOffsetNumber);

		MarkBufferDirty(buffer);

		if (BufferIsValid(leftchildbuf))
			MarkBufferDirty(leftchildbuf);

		if (RelationNeedsWAL(rel))
		{
			OffsetNumber ndeloffs = 0,
						deloffs[1];

			if (OffsetNumberIsValid(oldoffnum))
			{
				deloffs[0] = oldoffnum;
				ndeloffs = 1;
			}

			recptr = gistXLogUpdate(rel->rd_node, buffer,
									deloffs, ndeloffs, itup, ntup,
									leftchildbuf);

			PageSetLSN(page, recptr);
		}
		else
		{
			recptr = gistGetFakeLSN(rel);
			PageSetLSN(page, recptr);
		}

		if (newblkno)
			*newblkno = blkno;
	}

	/*
	 * If we inserted the downlink for a child page, set NSN and clear
	 * F_FOLLOW_RIGHT flag on the left child, so that concurrent scans know to
	 * follow the rightlink if and only if they looked at the parent page
	 * before we inserted the downlink.
	 *
	 * Note that we do this *after* writing the WAL record. That means that
	 * the possible full page image in the WAL record does not include these
	 * changes, and they must be replayed even if the page is restored from
	 * the full page image. There's a chicken-and-egg problem: if we updated
	 * the child pages first, we wouldn't know the recptr of the WAL record
	 * we're about to write.
	 */
	if (BufferIsValid(leftchildbuf))
	{
		Page		leftpg = BufferGetPage(leftchildbuf);

		GistPageSetNSN(leftpg, recptr);
		GistClearFollowRight(leftpg);

		PageSetLSN(leftpg, recptr);
	}

	END_CRIT_SECTION();

	return is_split;
}

/*
 * Workhouse routine for doing insertion into a GiST index. Note that
 * this routine assumes it is invoked in a short-lived memory context,
 * so it does not bother releasing palloc'd allocations.
 */
void
gistdoinsert(Relation r, IndexTuple itup, Size freespace, GISTSTATE *giststate)
{
	ItemId		iid;
	IndexTuple	idxtuple;
	GISTInsertStack firststack;
	GISTInsertStack *stack;
	GISTInsertState state;
	bool		xlocked = false;

	memset(&state, 0, sizeof(GISTInsertState));
	state.freespace = freespace;
	state.r = r;

	/* Start from the root */
	firststack.blkno = GIST_ROOT_BLKNO;
	firststack.lsn = 0;
	firststack.parent = NULL;
	firststack.downlinkoffnum = InvalidOffsetNumber;
	state.stack = stack = &firststack;

	/*
	 * Walk down along the path of smallest penalty, updating the parent
	 * pointers with the key we're inserting as we go. If we crash in the
	 * middle, the tree is consistent, although the possible parent updates
	 * were a waste.
	 */
	for (;;)
	{
		if (XLogRecPtrIsInvalid(stack->lsn))
			stack->buffer = ReadBuffer(state.r, stack->blkno);

		/*
		 * Be optimistic and grab shared lock first. Swap it for an exclusive
		 * lock later if we need to update the page.
		 */
		if (!xlocked)
		{
			LockBuffer(stack->buffer, GIST_SHARE);
			gistcheckpage(state.r, stack->buffer);
		}

		stack->page = (Page) BufferGetPage(stack->buffer);
		stack->lsn = PageGetLSN(stack->page);
		Assert(!RelationNeedsWAL(state.r) || !XLogRecPtrIsInvalid(stack->lsn));

		/*
		 * If this page was split but the downlink was never inserted to the
		 * parent because the inserting backend crashed before doing that, fix
		 * that now.
		 */
		if (GistFollowRight(stack->page))
		{
			if (!xlocked)
			{
				LockBuffer(stack->buffer, GIST_UNLOCK);
				LockBuffer(stack->buffer, GIST_EXCLUSIVE);
				xlocked = true;
				/* someone might've completed the split when we unlocked */
				if (!GistFollowRight(stack->page))
					continue;
			}
			gistfixsplit(&state, giststate);

			UnlockReleaseBuffer(stack->buffer);
			xlocked = false;
			state.stack = stack = stack->parent;
			continue;
		}

		if (stack->blkno != GIST_ROOT_BLKNO &&
			stack->parent->lsn < GistPageGetNSN(stack->page))
		{
			/*
			 * Concurrent split detected. There's no guarantee that the
			 * downlink for this page is consistent with the tuple we're
			 * inserting anymore, so go back to parent and rechoose the best
			 * child.
			 */
			UnlockReleaseBuffer(stack->buffer);
			xlocked = false;
			state.stack = stack = stack->parent;
			continue;
		}

		if (!GistPageIsLeaf(stack->page))
		{
			/*
			 * This is an internal page so continue to walk down the tree.
			 * Find the child node that has the minimum insertion penalty.
			 */
			BlockNumber childblkno;
			IndexTuple	newtup;
			GISTInsertStack *item;
			OffsetNumber downlinkoffnum;

			downlinkoffnum = gistchoose(state.r, stack->page, itup, giststate);
			iid = PageGetItemId(stack->page, downlinkoffnum);
			idxtuple = (IndexTuple) PageGetItem(stack->page, iid);
			childblkno = ItemPointerGetBlockNumber(&(idxtuple->t_tid));

			/*
			 * Check that it's not a leftover invalid tuple from pre-9.1
			 */
			if (GistTupleIsInvalid(idxtuple))
				ereport(ERROR,
						(errmsg("index \"%s\" contains an inner tuple marked as invalid",
								RelationGetRelationName(r)),
						 errdetail("This is caused by an incomplete page split at crash recovery before upgrading to PostgreSQL 9.1."),
						 errhint("Please REINDEX it.")));

			/*
			 * Check that the key representing the target child node is
			 * consistent with the key we're inserting. Update it if it's not.
			 */
			newtup = gistgetadjusted(state.r, idxtuple, itup, giststate);
			if (newtup)
			{
				/*
				 * Swap shared lock for an exclusive one. Beware, the page may
				 * change while we unlock/lock the page...
				 */
				if (!xlocked)
				{
					LockBuffer(stack->buffer, GIST_UNLOCK);
					LockBuffer(stack->buffer, GIST_EXCLUSIVE);
					xlocked = true;
					stack->page = (Page) BufferGetPage(stack->buffer);

					if (PageGetLSN(stack->page) != stack->lsn)
					{
						/* the page was changed while we unlocked it, retry */
						continue;
					}
				}

				/*
				 * Update the tuple.
				 *
				 * We still hold the lock after gistinserttuple(), but it
				 * might have to split the page to make the updated tuple fit.
				 * In that case the updated tuple might migrate to the other
				 * half of the split, so we have to go back to the parent and
				 * descend back to the half that's a better fit for the new
				 * tuple.
				 */
				if (gistinserttuple(&state, stack, giststate, newtup,
									downlinkoffnum))
				{
					/*
					 * If this was a root split, the root page continues to be
					 * the parent and the updated tuple went to one of the
					 * child pages, so we just need to retry from the root
					 * page.
					 */
					if (stack->blkno != GIST_ROOT_BLKNO)
					{
						UnlockReleaseBuffer(stack->buffer);
						xlocked = false;
						state.stack = stack = stack->parent;
					}
					continue;
				}
			}
			LockBuffer(stack->buffer, GIST_UNLOCK);
			xlocked = false;

			/* descend to the chosen child */
			item = (GISTInsertStack *) palloc0(sizeof(GISTInsertStack));
			item->blkno = childblkno;
			item->parent = stack;
			item->downlinkoffnum = downlinkoffnum;
			state.stack = stack = item;
		}
		else
		{
			/*
			 * Leaf page. Insert the new key. We've already updated all the
			 * parents on the way down, but we might have to split the page if
			 * it doesn't fit. gistinserthere() will take care of that.
			 */

			/*
			 * Swap shared lock for an exclusive one. Be careful, the page may
			 * change while we unlock/lock the page...
			 */
			if (!xlocked)
			{
				LockBuffer(stack->buffer, GIST_UNLOCK);
				LockBuffer(stack->buffer, GIST_EXCLUSIVE);
				xlocked = true;
				stack->page = (Page) BufferGetPage(stack->buffer);
				stack->lsn = PageGetLSN(stack->page);

				if (stack->blkno == GIST_ROOT_BLKNO)
				{
					/*
					 * the only page that can become inner instead of leaf is
					 * the root page, so for root we should recheck it
					 */
					if (!GistPageIsLeaf(stack->page))
					{
						/*
						 * very rare situation: during unlock/lock index with
						 * number of pages = 1 was increased
						 */
						LockBuffer(stack->buffer, GIST_UNLOCK);
						xlocked = false;
						continue;
					}

					/*
					 * we don't need to check root split, because checking
					 * leaf/inner is enough to recognize split for root
					 */
				}
				else if (GistFollowRight(stack->page) ||
						 stack->parent->lsn < GistPageGetNSN(stack->page))
				{
					/*
					 * The page was split while we momentarily unlocked the
					 * page. Go back to parent.
					 */
					UnlockReleaseBuffer(stack->buffer);
					xlocked = false;
					state.stack = stack = stack->parent;
					continue;
				}
			}

			/* now state.stack->(page, buffer and blkno) points to leaf page */

			gistinserttuple(&state, stack, giststate, itup,
							InvalidOffsetNumber);
			LockBuffer(stack->buffer, GIST_UNLOCK);

			/* Release any pins we might still hold before exiting */
			for (; stack; stack = stack->parent)
				ReleaseBuffer(stack->buffer);
			break;
		}
	}
}

/*
 * Traverse the tree to find path from root page to specified "child" block.
 *
 * returns a new insertion stack, starting from the parent of "child", up
 * to the root. *downlinkoffnum is set to the offset of the downlink in the
 * direct parent of child.
 *
 * To prevent deadlocks, this should lock only one page at a time.
 */
static GISTInsertStack *
gistFindPath(Relation r, BlockNumber child, OffsetNumber *downlinkoffnum)
{
	Page		page;
	Buffer		buffer;
	OffsetNumber i,
				maxoff;
	ItemId		iid;
	IndexTuple	idxtuple;
	List	   *fifo;
	GISTInsertStack *top,
			   *ptr;
	BlockNumber blkno;

	top = (GISTInsertStack *) palloc0(sizeof(GISTInsertStack));
	top->blkno = GIST_ROOT_BLKNO;
	top->downlinkoffnum = InvalidOffsetNumber;

	fifo = list_make1(top);
	while (fifo != NIL)
	{
		/* Get next page to visit */
		top = linitial(fifo);
		fifo = list_delete_first(fifo);

		buffer = ReadBuffer(r, top->blkno);
		LockBuffer(buffer, GIST_SHARE);
		gistcheckpage(r, buffer);
		page = (Page) BufferGetPage(buffer);

		if (GistPageIsLeaf(page))
		{
			/*
			 * Because we scan the index top-down, all the rest of the pages
			 * in the queue must be leaf pages as well.
			 */
			UnlockReleaseBuffer(buffer);
			break;
		}

		top->lsn = PageGetLSN(page);

		/*
		 * If F_FOLLOW_RIGHT is set, the page to the right doesn't have a
		 * downlink. This should not normally happen..
		 */
		if (GistFollowRight(page))
			elog(ERROR, "concurrent GiST page split was incomplete");

		if (top->parent && top->parent->lsn < GistPageGetNSN(page) &&
			GistPageGetOpaque(page)->rightlink != InvalidBlockNumber /* sanity check */ )
		{
			/*
			 * Page was split while we looked elsewhere. We didn't see the
			 * downlink to the right page when we scanned the parent, so add
			 * it to the queue now.
			 *
			 * Put the right page ahead of the queue, so that we visit it
			 * next. That's important, because if this is the lowest internal
			 * level, just above leaves, we might already have queued up some
			 * leaf pages, and we assume that there can't be any non-leaf
			 * pages behind leaf pages.
			 */
			ptr = (GISTInsertStack *) palloc0(sizeof(GISTInsertStack));
			ptr->blkno = GistPageGetOpaque(page)->rightlink;
			ptr->downlinkoffnum = InvalidOffsetNumber;
			ptr->parent = top->parent;

			fifo = lcons(ptr, fifo);
		}

		maxoff = PageGetMaxOffsetNumber(page);

		for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
		{
			iid = PageGetItemId(page, i);
			idxtuple = (IndexTuple) PageGetItem(page, iid);
			blkno = ItemPointerGetBlockNumber(&(idxtuple->t_tid));
			if (blkno == child)
			{
				/* Found it! */
				UnlockReleaseBuffer(buffer);
				*downlinkoffnum = i;
				return top;
			}
			else
			{
				/* Append this child to the list of pages to visit later */
				ptr = (GISTInsertStack *) palloc0(sizeof(GISTInsertStack));
				ptr->blkno = blkno;
				ptr->downlinkoffnum = i;
				ptr->parent = top;

				fifo = lappend(fifo, ptr);
			}
		}

		UnlockReleaseBuffer(buffer);
	}

	elog(ERROR, "failed to re-find parent of a page in index \"%s\", block %u",
		 RelationGetRelationName(r), child);
	return NULL;				/* keep compiler quiet */
}

/*
 * Updates the stack so that child->parent is the correct parent of the
 * child. child->parent must be exclusively locked on entry, and will
 * remain so at exit, but it might not be the same page anymore.
 */
static void
gistFindCorrectParent(Relation r, GISTInsertStack *child)
{
	GISTInsertStack *parent = child->parent;

	gistcheckpage(r, parent->buffer);
	parent->page = (Page) BufferGetPage(parent->buffer);

	/* here we don't need to distinguish between split and page update */
	if (child->downlinkoffnum == InvalidOffsetNumber ||
		parent->lsn != PageGetLSN(parent->page))
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
					child->downlinkoffnum = i;
					return;
				}
			}

			parent->blkno = GistPageGetOpaque(parent->page)->rightlink;
			UnlockReleaseBuffer(parent->buffer);
			if (parent->blkno == InvalidBlockNumber)
			{
				/*
				 * End of chain and still didn't find parent. It's a very-very
				 * rare situation when root splited.
				 */
				break;
			}
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
		ptr = parent = gistFindPath(r, child->blkno, &child->downlinkoffnum);

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

		/* make recursive call to normal processing */
		LockBuffer(child->parent->buffer, GIST_EXCLUSIVE);
		gistFindCorrectParent(r, child);
	}

	return;
}

/*
 * Form a downlink pointer for the page in 'buf'.
 */
static IndexTuple
gistformdownlink(Relation rel, Buffer buf, GISTSTATE *giststate,
				 GISTInsertStack *stack)
{
	Page		page = BufferGetPage(buf);
	OffsetNumber maxoff;
	OffsetNumber offset;
	IndexTuple	downlink = NULL;

	maxoff = PageGetMaxOffsetNumber(page);
	for (offset = FirstOffsetNumber; offset <= maxoff; offset = OffsetNumberNext(offset))
	{
		IndexTuple	ituple = (IndexTuple)
		PageGetItem(page, PageGetItemId(page, offset));

		if (downlink == NULL)
			downlink = CopyIndexTuple(ituple);
		else
		{
			IndexTuple	newdownlink;

			newdownlink = gistgetadjusted(rel, downlink, ituple,
										  giststate);
			if (newdownlink)
				downlink = newdownlink;
		}
	}

	/*
	 * If the page is completely empty, we can't form a meaningful downlink
	 * for it. But we have to insert a downlink for the page. Any key will do,
	 * as long as its consistent with the downlink of parent page, so that we
	 * can legally insert it to the parent. A minimal one that matches as few
	 * scans as possible would be best, to keep scans from doing useless work,
	 * but we don't know how to construct that. So we just use the downlink of
	 * the original page that was split - that's as far from optimal as it can
	 * get but will do..
	 */
	if (!downlink)
	{
		ItemId		iid;

		LockBuffer(stack->parent->buffer, GIST_EXCLUSIVE);
		gistFindCorrectParent(rel, stack);
		iid = PageGetItemId(stack->parent->page, stack->downlinkoffnum);
		downlink = (IndexTuple) PageGetItem(stack->parent->page, iid);
		downlink = CopyIndexTuple(downlink);
		LockBuffer(stack->parent->buffer, GIST_UNLOCK);
	}

	ItemPointerSetBlockNumber(&(downlink->t_tid), BufferGetBlockNumber(buf));
	GistTupleSetValid(downlink);

	return downlink;
}


/*
 * Complete the incomplete split of state->stack->page.
 */
static void
gistfixsplit(GISTInsertState *state, GISTSTATE *giststate)
{
	GISTInsertStack *stack = state->stack;
	Buffer		buf;
	Page		page;
	List	   *splitinfo = NIL;

	elog(LOG, "fixing incomplete split in index \"%s\", block %u",
		 RelationGetRelationName(state->r), stack->blkno);

	Assert(GistFollowRight(stack->page));
	Assert(OffsetNumberIsValid(stack->downlinkoffnum));

	buf = stack->buffer;

	/*
	 * Read the chain of split pages, following the rightlinks. Construct a
	 * downlink tuple for each page.
	 */
	for (;;)
	{
		GISTPageSplitInfo *si = palloc(sizeof(GISTPageSplitInfo));
		IndexTuple	downlink;

		page = BufferGetPage(buf);

		/* Form the new downlink tuples to insert to parent */
		downlink = gistformdownlink(state->r, buf, giststate, stack);

		si->buf = buf;
		si->downlink = downlink;

		splitinfo = lappend(splitinfo, si);

		if (GistFollowRight(page))
		{
			/* lock next page */
			buf = ReadBuffer(state->r, GistPageGetOpaque(page)->rightlink);
			LockBuffer(buf, GIST_EXCLUSIVE);
		}
		else
			break;
	}

	/* Insert the downlinks */
	gistfinishsplit(state, stack, giststate, splitinfo, false);
}

/*
 * Insert or replace a tuple in stack->buffer. If 'oldoffnum' is valid, the
 * tuple at 'oldoffnum' is replaced, otherwise the tuple is inserted as new.
 * 'stack' represents the path from the root to the page being updated.
 *
 * The caller must hold an exclusive lock on stack->buffer.  The lock is still
 * held on return, but the page might not contain the inserted tuple if the
 * page was split. The function returns true if the page was split, false
 * otherwise.
 */
static bool
gistinserttuple(GISTInsertState *state, GISTInsertStack *stack,
			  GISTSTATE *giststate, IndexTuple tuple, OffsetNumber oldoffnum)
{
	return gistinserttuples(state, stack, giststate, &tuple, 1, oldoffnum,
							InvalidBuffer, InvalidBuffer, false, false);
}

/* ----------------
 * An extended workhorse version of gistinserttuple(). This version allows
 * inserting multiple tuples, or replacing a single tuple with multiple tuples.
 * This is used to recursively update the downlinks in the parent when a page
 * is split.
 *
 * If leftchild and rightchild are valid, we're inserting/replacing the
 * downlink for rightchild, and leftchild is its left sibling. We clear the
 * F_FOLLOW_RIGHT flag and update NSN on leftchild, atomically with the
 * insertion of the downlink.
 *
 * To avoid holding locks for longer than necessary, when recursing up the
 * tree to update the parents, the locking is a bit peculiar here. On entry,
 * the caller must hold an exclusive lock on stack->buffer, as well as
 * leftchild and rightchild if given. On return:
 *
 *	- Lock on stack->buffer is released, if 'unlockbuf' is true. The page is
 *	  always kept pinned, however.
 *	- Lock on 'leftchild' is released, if 'unlockleftchild' is true. The page
 *	  is kept pinned.
 *	- Lock and pin on 'rightchild' are always released.
 *
 * Returns 'true' if the page had to be split. Note that if the page was
 * split, the inserted/updated tuples might've been inserted to a right
 * sibling of stack->buffer instead of stack->buffer itself.
 */
static bool
gistinserttuples(GISTInsertState *state, GISTInsertStack *stack,
				 GISTSTATE *giststate,
				 IndexTuple *tuples, int ntup, OffsetNumber oldoffnum,
				 Buffer leftchild, Buffer rightchild,
				 bool unlockbuf, bool unlockleftchild)
{
	List	   *splitinfo;
	bool		is_split;

	/* Insert the tuple(s) to the page, splitting the page if necessary */
	is_split = gistplacetopage(state->r, state->freespace, giststate,
							   stack->buffer,
							   tuples, ntup,
							   oldoffnum, NULL,
							   leftchild,
							   &splitinfo,
							   true);

	/*
	 * Before recursing up in case the page was split, release locks on the
	 * child pages. We don't need to keep them locked when updating the
	 * parent.
	 */
	if (BufferIsValid(rightchild))
		UnlockReleaseBuffer(rightchild);
	if (BufferIsValid(leftchild) && unlockleftchild)
		LockBuffer(leftchild, GIST_UNLOCK);

	/*
	 * If we had to split, insert/update the downlinks in the parent. If the
	 * caller requested us to release the lock on stack->buffer, tell
	 * gistfinishsplit() to do that as soon as it's safe to do so. If we
	 * didn't have to split, release it ourselves.
	 */
	if (splitinfo)
		gistfinishsplit(state, stack, giststate, splitinfo, unlockbuf);
	else if (unlockbuf)
		LockBuffer(stack->buffer, GIST_UNLOCK);

	return is_split;
}

/*
 * Finish an incomplete split by inserting/updating the downlinks in parent
 * page. 'splitinfo' contains all the child pages involved in the split,
 * from left-to-right.
 *
 * On entry, the caller must hold a lock on stack->buffer and all the child
 * pages in 'splitinfo'. If 'unlockbuf' is true, the lock on stack->buffer is
 * released on return. The child pages are always unlocked and unpinned.
 */
static void
gistfinishsplit(GISTInsertState *state, GISTInsertStack *stack,
				GISTSTATE *giststate, List *splitinfo, bool unlockbuf)
{
	ListCell   *lc;
	List	   *reversed;
	GISTPageSplitInfo *right;
	GISTPageSplitInfo *left;
	IndexTuple	tuples[2];

	/* A split always contains at least two halves */
	Assert(list_length(splitinfo) >= 2);

	/*
	 * We need to insert downlinks for each new page, and update the downlink
	 * for the original (leftmost) page in the split. Begin at the rightmost
	 * page, inserting one downlink at a time until there's only two pages
	 * left. Finally insert the downlink for the last new page and update the
	 * downlink for the original page as one operation.
	 */

	/* for convenience, create a copy of the list in reverse order */
	reversed = NIL;
	foreach(lc, splitinfo)
	{
		reversed = lcons(lfirst(lc), reversed);
	}

	LockBuffer(stack->parent->buffer, GIST_EXCLUSIVE);
	gistFindCorrectParent(state->r, stack);

	/*
	 * insert downlinks for the siblings from right to left, until there are
	 * only two siblings left.
	 */
	while (list_length(reversed) > 2)
	{
		right = (GISTPageSplitInfo *) linitial(reversed);
		left = (GISTPageSplitInfo *) lsecond(reversed);

		if (gistinserttuples(state, stack->parent, giststate,
							 &right->downlink, 1,
							 InvalidOffsetNumber,
							 left->buf, right->buf, false, false))
		{
			/*
			 * If the parent page was split, need to relocate the original
			 * parent pointer.
			 */
			gistFindCorrectParent(state->r, stack);
		}
		/* gistinserttuples() released the lock on right->buf. */
		reversed = list_delete_first(reversed);
	}

	right = (GISTPageSplitInfo *) linitial(reversed);
	left = (GISTPageSplitInfo *) lsecond(reversed);

	/*
	 * Finally insert downlink for the remaining right page and update the
	 * downlink for the original page to not contain the tuples that were
	 * moved to the new pages.
	 */
	tuples[0] = left->downlink;
	tuples[1] = right->downlink;
	gistinserttuples(state, stack->parent, giststate,
					 tuples, 2,
					 stack->downlinkoffnum,
					 left->buf, right->buf,
					 true,		/* Unlock parent */
					 unlockbuf	/* Unlock stack->buffer if caller wants that */
		);
	Assert(left->buf == stack->buffer);
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
	int			i;
	SplitedPageLayout *res = NULL;

	/* this should never recurse very deeply, but better safe than sorry */
	check_stack_depth();

	/* there's no point in splitting an empty page */
	Assert(len > 0);

	/*
	 * If a single tuple doesn't fit on a page, no amount of splitting will
	 * help.
	 */
	if (len == 1)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			errmsg("index row size %zu exceeds maximum %zu for index \"%s\"",
				   IndexTupleSize(itup[0]), GiSTPageSize,
				   RelationGetRelationName(r))));

	memset(v.spl_lisnull, TRUE, sizeof(bool) * giststate->tupdesc->natts);
	memset(v.spl_risnull, TRUE, sizeof(bool) * giststate->tupdesc->natts);
	gistSplitByKey(r, page, itup, len, giststate, &v, 0);

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
		res->itup = gistFormTuple(giststate, r, v.spl_rattr, v.spl_risnull, false);
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
		res->itup = gistFormTuple(giststate, r, v.spl_lattr, v.spl_lisnull, false);
	}

	return res;
}

/*
 * Create a GISTSTATE and fill it with information about the index
 */
GISTSTATE *
initGISTstate(Relation index)
{
	GISTSTATE  *giststate;
	MemoryContext scanCxt;
	MemoryContext oldCxt;
	int			i;

	/* safety check to protect fixed-size arrays in GISTSTATE */
	if (index->rd_att->natts > INDEX_MAX_KEYS)
		elog(ERROR, "numberOfAttributes %d > %d",
			 index->rd_att->natts, INDEX_MAX_KEYS);

	/* Create the memory context that will hold the GISTSTATE */
	scanCxt = AllocSetContextCreate(CurrentMemoryContext,
									"GiST scan context",
									ALLOCSET_DEFAULT_MINSIZE,
									ALLOCSET_DEFAULT_INITSIZE,
									ALLOCSET_DEFAULT_MAXSIZE);
	oldCxt = MemoryContextSwitchTo(scanCxt);

	/* Create and fill in the GISTSTATE */
	giststate = (GISTSTATE *) palloc(sizeof(GISTSTATE));

	giststate->scanCxt = scanCxt;
	giststate->tempCxt = scanCxt;		/* caller must change this if needed */
	giststate->tupdesc = index->rd_att;

	for (i = 0; i < index->rd_att->natts; i++)
	{
		fmgr_info_copy(&(giststate->consistentFn[i]),
					   index_getprocinfo(index, i + 1, GIST_CONSISTENT_PROC),
					   scanCxt);
		fmgr_info_copy(&(giststate->unionFn[i]),
					   index_getprocinfo(index, i + 1, GIST_UNION_PROC),
					   scanCxt);
		fmgr_info_copy(&(giststate->compressFn[i]),
					   index_getprocinfo(index, i + 1, GIST_COMPRESS_PROC),
					   scanCxt);
		fmgr_info_copy(&(giststate->decompressFn[i]),
					   index_getprocinfo(index, i + 1, GIST_DECOMPRESS_PROC),
					   scanCxt);
		fmgr_info_copy(&(giststate->penaltyFn[i]),
					   index_getprocinfo(index, i + 1, GIST_PENALTY_PROC),
					   scanCxt);
		fmgr_info_copy(&(giststate->picksplitFn[i]),
					   index_getprocinfo(index, i + 1, GIST_PICKSPLIT_PROC),
					   scanCxt);
		fmgr_info_copy(&(giststate->equalFn[i]),
					   index_getprocinfo(index, i + 1, GIST_EQUAL_PROC),
					   scanCxt);
		/* opclasses are not required to provide a Distance method */
		if (OidIsValid(index_getprocid(index, i + 1, GIST_DISTANCE_PROC)))
			fmgr_info_copy(&(giststate->distanceFn[i]),
						 index_getprocinfo(index, i + 1, GIST_DISTANCE_PROC),
						   scanCxt);
		else
			giststate->distanceFn[i].fn_oid = InvalidOid;

		/* opclasses are not required to provide a Fetch method */
		if (OidIsValid(index_getprocid(index, i + 1, GIST_FETCH_PROC)))
			fmgr_info_copy(&(giststate->fetchFn[i]),
						   index_getprocinfo(index, i + 1, GIST_FETCH_PROC),
						   scanCxt);
		else
			giststate->fetchFn[i].fn_oid = InvalidOid;

		/*
		 * If the index column has a specified collation, we should honor that
		 * while doing comparisons.  However, we may have a collatable storage
		 * type for a noncollatable indexed data type.  If there's no index
		 * collation then specify default collation in case the support
		 * functions need collation.  This is harmless if the support
		 * functions don't care about collation, so we just do it
		 * unconditionally.  (We could alternatively call get_typcollation,
		 * but that seems like expensive overkill --- there aren't going to be
		 * any cases where a GiST storage type has a nondefault collation.)
		 */
		if (OidIsValid(index->rd_indcollation[i]))
			giststate->supportCollation[i] = index->rd_indcollation[i];
		else
			giststate->supportCollation[i] = DEFAULT_COLLATION_OID;
	}

	MemoryContextSwitchTo(oldCxt);

	return giststate;
}

void
freeGISTstate(GISTSTATE *giststate)
{
	/* It's sufficient to delete the scanCxt */
	MemoryContextDelete(giststate->scanCxt);
}
