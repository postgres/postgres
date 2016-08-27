/*-------------------------------------------------------------------------
 *
 * ginbtree.c
 *	  page utilities routines for the postgres inverted index access method.
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/ginbtree.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin_private.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/rel.h"

static void ginFindParents(GinBtree btree, GinBtreeStack *stack);
static bool ginPlaceToPage(GinBtree btree, GinBtreeStack *stack,
			   void *insertdata, BlockNumber updateblkno,
			   Buffer childbuf, GinStatsData *buildStats);
static void ginFinishSplit(GinBtree btree, GinBtreeStack *stack,
			   bool freestack, GinStatsData *buildStats);

/*
 * Lock buffer by needed method for search.
 */
static int
ginTraverseLock(Buffer buffer, bool searchMode)
{
	Page		page;
	int			access = GIN_SHARE;

	LockBuffer(buffer, GIN_SHARE);
	page = BufferGetPage(buffer);
	if (GinPageIsLeaf(page))
	{
		if (searchMode == FALSE)
		{
			/* we should relock our page */
			LockBuffer(buffer, GIN_UNLOCK);
			LockBuffer(buffer, GIN_EXCLUSIVE);

			/* But root can become non-leaf during relock */
			if (!GinPageIsLeaf(page))
			{
				/* restore old lock type (very rare) */
				LockBuffer(buffer, GIN_UNLOCK);
				LockBuffer(buffer, GIN_SHARE);
			}
			else
				access = GIN_EXCLUSIVE;
		}
	}

	return access;
}

/*
 * Descend the tree to the leaf page that contains or would contain the key
 * we're searching for. The key should already be filled in 'btree', in
 * tree-type specific manner. If btree->fullScan is true, descends to the
 * leftmost leaf page.
 *
 * If 'searchmode' is false, on return stack->buffer is exclusively locked,
 * and the stack represents the full path to the root. Otherwise stack->buffer
 * is share-locked, and stack->parent is NULL.
 */
GinBtreeStack *
ginFindLeafPage(GinBtree btree, bool searchMode, Snapshot snapshot)
{
	GinBtreeStack *stack;

	stack = (GinBtreeStack *) palloc(sizeof(GinBtreeStack));
	stack->blkno = btree->rootBlkno;
	stack->buffer = ReadBuffer(btree->index, btree->rootBlkno);
	stack->parent = NULL;
	stack->predictNumber = 1;

	for (;;)
	{
		Page		page;
		BlockNumber child;
		int			access;

		stack->off = InvalidOffsetNumber;

		page = BufferGetPage(stack->buffer);
		TestForOldSnapshot(snapshot, btree->index, page);

		access = ginTraverseLock(stack->buffer, searchMode);

		/*
		 * If we're going to modify the tree, finish any incomplete splits we
		 * encounter on the way.
		 */
		if (!searchMode && GinPageIsIncompleteSplit(page))
			ginFinishSplit(btree, stack, false, NULL);

		/*
		 * ok, page is correctly locked, we should check to move right ..,
		 * root never has a right link, so small optimization
		 */
		while (btree->fullScan == FALSE && stack->blkno != btree->rootBlkno &&
			   btree->isMoveRight(btree, page))
		{
			BlockNumber rightlink = GinPageGetOpaque(page)->rightlink;

			if (rightlink == InvalidBlockNumber)
				/* rightmost page */
				break;

			stack->buffer = ginStepRight(stack->buffer, btree->index, access);
			stack->blkno = rightlink;
			page = BufferGetPage(stack->buffer);
			TestForOldSnapshot(snapshot, btree->index, page);

			if (!searchMode && GinPageIsIncompleteSplit(page))
				ginFinishSplit(btree, stack, false, NULL);
		}

		if (GinPageIsLeaf(page))	/* we found, return locked page */
			return stack;

		/* now we have correct buffer, try to find child */
		child = btree->findChildPage(btree, stack);

		LockBuffer(stack->buffer, GIN_UNLOCK);
		Assert(child != InvalidBlockNumber);
		Assert(stack->blkno != child);

		if (searchMode)
		{
			/* in search mode we may forget path to leaf */
			stack->blkno = child;
			stack->buffer = ReleaseAndReadBuffer(stack->buffer, btree->index, stack->blkno);
		}
		else
		{
			GinBtreeStack *ptr = (GinBtreeStack *) palloc(sizeof(GinBtreeStack));

			ptr->parent = stack;
			stack = ptr;
			stack->blkno = child;
			stack->buffer = ReadBuffer(btree->index, stack->blkno);
			stack->predictNumber = 1;
		}
	}
}

/*
 * Step right from current page.
 *
 * The next page is locked first, before releasing the current page. This is
 * crucial to protect from concurrent page deletion (see comment in
 * ginDeletePage).
 */
Buffer
ginStepRight(Buffer buffer, Relation index, int lockmode)
{
	Buffer		nextbuffer;
	Page		page = BufferGetPage(buffer);
	bool		isLeaf = GinPageIsLeaf(page);
	bool		isData = GinPageIsData(page);
	BlockNumber blkno = GinPageGetOpaque(page)->rightlink;

	nextbuffer = ReadBuffer(index, blkno);
	LockBuffer(nextbuffer, lockmode);
	UnlockReleaseBuffer(buffer);

	/* Sanity check that the page we stepped to is of similar kind. */
	page = BufferGetPage(nextbuffer);
	if (isLeaf != GinPageIsLeaf(page) || isData != GinPageIsData(page))
		elog(ERROR, "right sibling of GIN page is of different type");

	/*
	 * Given the proper lock sequence above, we should never land on a deleted
	 * page.
	 */
	if (GinPageIsDeleted(page))
		elog(ERROR, "right sibling of GIN page was deleted");

	return nextbuffer;
}

void
freeGinBtreeStack(GinBtreeStack *stack)
{
	while (stack)
	{
		GinBtreeStack *tmp = stack->parent;

		if (stack->buffer != InvalidBuffer)
			ReleaseBuffer(stack->buffer);

		pfree(stack);
		stack = tmp;
	}
}

/*
 * Try to find parent for current stack position. Returns correct parent and
 * child's offset in stack->parent. The root page is never released, to
 * to prevent conflict with vacuum process.
 */
static void
ginFindParents(GinBtree btree, GinBtreeStack *stack)
{
	Page		page;
	Buffer		buffer;
	BlockNumber blkno,
				leftmostBlkno;
	OffsetNumber offset;
	GinBtreeStack *root;
	GinBtreeStack *ptr;

	/*
	 * Unwind the stack all the way up to the root, leaving only the root
	 * item.
	 *
	 * Be careful not to release the pin on the root page! The pin on root
	 * page is required to lock out concurrent vacuums on the tree.
	 */
	root = stack->parent;
	while (root->parent)
	{
		ReleaseBuffer(root->buffer);
		root = root->parent;
	}

	Assert(root->blkno == btree->rootBlkno);
	Assert(BufferGetBlockNumber(root->buffer) == btree->rootBlkno);
	root->off = InvalidOffsetNumber;

	blkno = root->blkno;
	buffer = root->buffer;
	offset = InvalidOffsetNumber;

	ptr = (GinBtreeStack *) palloc(sizeof(GinBtreeStack));

	for (;;)
	{
		LockBuffer(buffer, GIN_EXCLUSIVE);
		page = BufferGetPage(buffer);
		if (GinPageIsLeaf(page))
			elog(ERROR, "Lost path");

		if (GinPageIsIncompleteSplit(page))
		{
			Assert(blkno != btree->rootBlkno);
			ptr->blkno = blkno;
			ptr->buffer = buffer;

			/*
			 * parent may be wrong, but if so, the ginFinishSplit call will
			 * recurse to call ginFindParents again to fix it.
			 */
			ptr->parent = root;
			ptr->off = InvalidOffsetNumber;

			ginFinishSplit(btree, ptr, false, NULL);
		}

		leftmostBlkno = btree->getLeftMostChild(btree, page);

		while ((offset = btree->findChildPtr(btree, page, stack->blkno, InvalidOffsetNumber)) == InvalidOffsetNumber)
		{
			blkno = GinPageGetOpaque(page)->rightlink;
			if (blkno == InvalidBlockNumber)
			{
				UnlockReleaseBuffer(buffer);
				break;
			}
			buffer = ginStepRight(buffer, btree->index, GIN_EXCLUSIVE);
			page = BufferGetPage(buffer);

			/* finish any incomplete splits, as above */
			if (GinPageIsIncompleteSplit(page))
			{
				Assert(blkno != btree->rootBlkno);
				ptr->blkno = blkno;
				ptr->buffer = buffer;
				ptr->parent = root;
				ptr->off = InvalidOffsetNumber;

				ginFinishSplit(btree, ptr, false, NULL);
			}
		}

		if (blkno != InvalidBlockNumber)
		{
			ptr->blkno = blkno;
			ptr->buffer = buffer;
			ptr->parent = root; /* it may be wrong, but in next call we will
								 * correct */
			ptr->off = offset;
			stack->parent = ptr;
			return;
		}

		/* Descend down to next level */
		blkno = leftmostBlkno;
		buffer = ReadBuffer(btree->index, blkno);
	}
}

/*
 * Insert a new item to a page.
 *
 * Returns true if the insertion was finished. On false, the page was split and
 * the parent needs to be updated. (A root split returns true as it doesn't
 * need any further action by the caller to complete.)
 *
 * When inserting a downlink to an internal page, 'childbuf' contains the
 * child page that was split. Its GIN_INCOMPLETE_SPLIT flag will be cleared
 * atomically with the insert. Also, the existing item at offset stack->off
 * in the target page is updated to point to updateblkno.
 *
 * stack->buffer is locked on entry, and is kept locked.
 * Likewise for childbuf, if given.
 */
static bool
ginPlaceToPage(GinBtree btree, GinBtreeStack *stack,
			   void *insertdata, BlockNumber updateblkno,
			   Buffer childbuf, GinStatsData *buildStats)
{
	Page		page = BufferGetPage(stack->buffer);
	bool		result;
	GinPlaceToPageRC rc;
	uint16		xlflags = 0;
	Page		childpage = NULL;
	Page		newlpage = NULL,
				newrpage = NULL;
	void	   *ptp_workspace = NULL;
	MemoryContext tmpCxt;
	MemoryContext oldCxt;

	/*
	 * We do all the work of this function and its subfunctions in a temporary
	 * memory context.  This avoids leakages and simplifies APIs, since some
	 * subfunctions allocate storage that has to survive until we've finished
	 * the WAL insertion.
	 */
	tmpCxt = AllocSetContextCreate(CurrentMemoryContext,
								   "ginPlaceToPage temporary context",
								   ALLOCSET_DEFAULT_SIZES);
	oldCxt = MemoryContextSwitchTo(tmpCxt);

	if (GinPageIsData(page))
		xlflags |= GIN_INSERT_ISDATA;
	if (GinPageIsLeaf(page))
	{
		xlflags |= GIN_INSERT_ISLEAF;
		Assert(!BufferIsValid(childbuf));
		Assert(updateblkno == InvalidBlockNumber);
	}
	else
	{
		Assert(BufferIsValid(childbuf));
		Assert(updateblkno != InvalidBlockNumber);
		childpage = BufferGetPage(childbuf);
	}

	/*
	 * See if the incoming tuple will fit on the page.  beginPlaceToPage will
	 * decide if the page needs to be split, and will compute the split
	 * contents if so.  See comments for beginPlaceToPage and execPlaceToPage
	 * functions for more details of the API here.
	 */
	rc = btree->beginPlaceToPage(btree, stack->buffer, stack,
								 insertdata, updateblkno,
								 &ptp_workspace,
								 &newlpage, &newrpage);

	if (rc == GPTP_NO_WORK)
	{
		/* Nothing to do */
		result = true;
	}
	else if (rc == GPTP_INSERT)
	{
		/* It will fit, perform the insertion */
		START_CRIT_SECTION();

		if (RelationNeedsWAL(btree->index))
		{
			XLogBeginInsert();
			XLogRegisterBuffer(0, stack->buffer, REGBUF_STANDARD);
			if (BufferIsValid(childbuf))
				XLogRegisterBuffer(1, childbuf, REGBUF_STANDARD);
		}

		/* Perform the page update, and register any extra WAL data */
		btree->execPlaceToPage(btree, stack->buffer, stack,
							   insertdata, updateblkno, ptp_workspace);

		MarkBufferDirty(stack->buffer);

		/* An insert to an internal page finishes the split of the child. */
		if (BufferIsValid(childbuf))
		{
			GinPageGetOpaque(childpage)->flags &= ~GIN_INCOMPLETE_SPLIT;
			MarkBufferDirty(childbuf);
		}

		if (RelationNeedsWAL(btree->index))
		{
			XLogRecPtr	recptr;
			ginxlogInsert xlrec;
			BlockIdData childblknos[2];

			xlrec.flags = xlflags;

			XLogRegisterData((char *) &xlrec, sizeof(ginxlogInsert));

			/*
			 * Log information about child if this was an insertion of a
			 * downlink.
			 */
			if (BufferIsValid(childbuf))
			{
				BlockIdSet(&childblknos[0], BufferGetBlockNumber(childbuf));
				BlockIdSet(&childblknos[1], GinPageGetOpaque(childpage)->rightlink);
				XLogRegisterData((char *) childblknos,
								 sizeof(BlockIdData) * 2);
			}

			recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_INSERT);
			PageSetLSN(page, recptr);
			if (BufferIsValid(childbuf))
				PageSetLSN(childpage, recptr);
		}

		END_CRIT_SECTION();

		/* Insertion is complete. */
		result = true;
	}
	else if (rc == GPTP_SPLIT)
	{
		/*
		 * Didn't fit, need to split.  The split has been computed in newlpage
		 * and newrpage, which are pointers to palloc'd pages, not associated
		 * with buffers.  stack->buffer is not touched yet.
		 */
		Buffer		rbuffer;
		BlockNumber savedRightLink;
		ginxlogSplit data;
		Buffer		lbuffer = InvalidBuffer;
		Page		newrootpg = NULL;

		/* Get a new index page to become the right page */
		rbuffer = GinNewBuffer(btree->index);

		/* During index build, count the new page */
		if (buildStats)
		{
			if (btree->isData)
				buildStats->nDataPages++;
			else
				buildStats->nEntryPages++;
		}

		savedRightLink = GinPageGetOpaque(page)->rightlink;

		/* Begin setting up WAL record */
		data.node = btree->index->rd_node;
		data.flags = xlflags;
		if (BufferIsValid(childbuf))
		{
			data.leftChildBlkno = BufferGetBlockNumber(childbuf);
			data.rightChildBlkno = GinPageGetOpaque(childpage)->rightlink;
		}
		else
			data.leftChildBlkno = data.rightChildBlkno = InvalidBlockNumber;

		if (stack->parent == NULL)
		{
			/*
			 * splitting the root, so we need to allocate new left page and
			 * place pointers to left and right page on root page.
			 */
			lbuffer = GinNewBuffer(btree->index);

			/* During index build, count the new left page */
			if (buildStats)
			{
				if (btree->isData)
					buildStats->nDataPages++;
				else
					buildStats->nEntryPages++;
			}

			data.rrlink = InvalidBlockNumber;
			data.flags |= GIN_SPLIT_ROOT;

			GinPageGetOpaque(newrpage)->rightlink = InvalidBlockNumber;
			GinPageGetOpaque(newlpage)->rightlink = BufferGetBlockNumber(rbuffer);

			/*
			 * Construct a new root page containing downlinks to the new left
			 * and right pages.  (Do this in a temporary copy rather than
			 * overwriting the original page directly, since we're not in the
			 * critical section yet.)
			 */
			newrootpg = PageGetTempPage(newrpage);
			GinInitPage(newrootpg, GinPageGetOpaque(newlpage)->flags & ~(GIN_LEAF | GIN_COMPRESSED), BLCKSZ);

			btree->fillRoot(btree, newrootpg,
							BufferGetBlockNumber(lbuffer), newlpage,
							BufferGetBlockNumber(rbuffer), newrpage);
		}
		else
		{
			/* splitting a non-root page */
			data.rrlink = savedRightLink;

			GinPageGetOpaque(newrpage)->rightlink = savedRightLink;
			GinPageGetOpaque(newlpage)->flags |= GIN_INCOMPLETE_SPLIT;
			GinPageGetOpaque(newlpage)->rightlink = BufferGetBlockNumber(rbuffer);
		}

		/*
		 * OK, we have the new contents of the left page in a temporary copy
		 * now (newlpage), and likewise for the new contents of the
		 * newly-allocated right block. The original page is still unchanged.
		 *
		 * If this is a root split, we also have a temporary page containing
		 * the new contents of the root.
		 */

		START_CRIT_SECTION();

		MarkBufferDirty(rbuffer);
		MarkBufferDirty(stack->buffer);

		/*
		 * Restore the temporary copies over the real buffers.
		 */
		if (stack->parent == NULL)
		{
			/* Splitting the root, three pages to update */
			MarkBufferDirty(lbuffer);
			memcpy(page, newrootpg, BLCKSZ);
			memcpy(BufferGetPage(lbuffer), newlpage, BLCKSZ);
			memcpy(BufferGetPage(rbuffer), newrpage, BLCKSZ);
		}
		else
		{
			/* Normal split, only two pages to update */
			memcpy(page, newlpage, BLCKSZ);
			memcpy(BufferGetPage(rbuffer), newrpage, BLCKSZ);
		}

		/* We also clear childbuf's INCOMPLETE_SPLIT flag, if passed */
		if (BufferIsValid(childbuf))
		{
			GinPageGetOpaque(childpage)->flags &= ~GIN_INCOMPLETE_SPLIT;
			MarkBufferDirty(childbuf);
		}

		/* write WAL record */
		if (RelationNeedsWAL(btree->index))
		{
			XLogRecPtr	recptr;

			XLogBeginInsert();

			/*
			 * We just take full page images of all the split pages. Splits
			 * are uncommon enough that it's not worth complicating the code
			 * to be more efficient.
			 */
			if (stack->parent == NULL)
			{
				XLogRegisterBuffer(0, lbuffer, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
				XLogRegisterBuffer(1, rbuffer, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
				XLogRegisterBuffer(2, stack->buffer, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
			}
			else
			{
				XLogRegisterBuffer(0, stack->buffer, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
				XLogRegisterBuffer(1, rbuffer, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
			}
			if (BufferIsValid(childbuf))
				XLogRegisterBuffer(3, childbuf, REGBUF_STANDARD);

			XLogRegisterData((char *) &data, sizeof(ginxlogSplit));

			recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_SPLIT);

			PageSetLSN(page, recptr);
			PageSetLSN(BufferGetPage(rbuffer), recptr);
			if (stack->parent == NULL)
				PageSetLSN(BufferGetPage(lbuffer), recptr);
			if (BufferIsValid(childbuf))
				PageSetLSN(childpage, recptr);
		}
		END_CRIT_SECTION();

		/*
		 * We can release the locks/pins on the new pages now, but keep
		 * stack->buffer locked.  childbuf doesn't get unlocked either.
		 */
		UnlockReleaseBuffer(rbuffer);
		if (stack->parent == NULL)
			UnlockReleaseBuffer(lbuffer);

		/*
		 * If we split the root, we're done. Otherwise the split is not
		 * complete until the downlink for the new page has been inserted to
		 * the parent.
		 */
		result = (stack->parent == NULL);
	}
	else
	{
		elog(ERROR, "invalid return code from GIN placeToPage method: %d", rc);
		result = false;			/* keep compiler quiet */
	}

	/* Clean up temp context */
	MemoryContextSwitchTo(oldCxt);
	MemoryContextDelete(tmpCxt);

	return result;
}

/*
 * Finish a split by inserting the downlink for the new page to parent.
 *
 * On entry, stack->buffer is exclusively locked.
 *
 * If freestack is true, all the buffers are released and unlocked as we
 * crawl up the tree, and 'stack' is freed. Otherwise stack->buffer is kept
 * locked, and stack is unmodified, except for possibly moving right to find
 * the correct parent of page.
 */
static void
ginFinishSplit(GinBtree btree, GinBtreeStack *stack, bool freestack,
			   GinStatsData *buildStats)
{
	Page		page;
	bool		done;
	bool		first = true;

	/*
	 * freestack == false when we encounter an incompletely split page during
	 * a scan, while freestack == true is used in the normal scenario that a
	 * split is finished right after the initial insert.
	 */
	if (!freestack)
		elog(DEBUG1, "finishing incomplete split of block %u in gin index \"%s\"",
			 stack->blkno, RelationGetRelationName(btree->index));

	/* this loop crawls up the stack until the insertion is complete */
	do
	{
		GinBtreeStack *parent = stack->parent;
		void	   *insertdata;
		BlockNumber updateblkno;

		/* search parent to lock */
		LockBuffer(parent->buffer, GIN_EXCLUSIVE);

		/*
		 * If the parent page was incompletely split, finish that split first,
		 * then continue with the current one.
		 *
		 * Note: we have to finish *all* incomplete splits we encounter, even
		 * if we have to move right. Otherwise we might choose as the target a
		 * page that has no downlink in the parent, and splitting it further
		 * would fail.
		 */
		if (GinPageIsIncompleteSplit(BufferGetPage(parent->buffer)))
			ginFinishSplit(btree, parent, false, buildStats);

		/* move right if it's needed */
		page = BufferGetPage(parent->buffer);
		while ((parent->off = btree->findChildPtr(btree, page, stack->blkno, parent->off)) == InvalidOffsetNumber)
		{
			if (GinPageRightMost(page))
			{
				/*
				 * rightmost page, but we don't find parent, we should use
				 * plain search...
				 */
				LockBuffer(parent->buffer, GIN_UNLOCK);
				ginFindParents(btree, stack);
				parent = stack->parent;
				Assert(parent != NULL);
				break;
			}

			parent->buffer = ginStepRight(parent->buffer, btree->index, GIN_EXCLUSIVE);
			parent->blkno = BufferGetBlockNumber(parent->buffer);
			page = BufferGetPage(parent->buffer);

			if (GinPageIsIncompleteSplit(BufferGetPage(parent->buffer)))
				ginFinishSplit(btree, parent, false, buildStats);
		}

		/* insert the downlink */
		insertdata = btree->prepareDownlink(btree, stack->buffer);
		updateblkno = GinPageGetOpaque(BufferGetPage(stack->buffer))->rightlink;
		done = ginPlaceToPage(btree, parent,
							  insertdata, updateblkno,
							  stack->buffer, buildStats);
		pfree(insertdata);

		/*
		 * If the caller requested to free the stack, unlock and release the
		 * child buffer now. Otherwise keep it pinned and locked, but if we
		 * have to recurse up the tree, we can unlock the upper pages, only
		 * keeping the page at the bottom of the stack locked.
		 */
		if (!first || freestack)
			LockBuffer(stack->buffer, GIN_UNLOCK);
		if (freestack)
		{
			ReleaseBuffer(stack->buffer);
			pfree(stack);
		}
		stack = parent;

		first = false;
	} while (!done);

	/* unlock the parent */
	LockBuffer(stack->buffer, GIN_UNLOCK);

	if (freestack)
		freeGinBtreeStack(stack);
}

/*
 * Insert a value to tree described by stack.
 *
 * The value to be inserted is given in 'insertdata'. Its format depends
 * on whether this is an entry or data tree, ginInsertValue just passes it
 * through to the tree-specific callback function.
 *
 * During an index build, buildStats is non-null and the counters it contains
 * are incremented as needed.
 *
 * NB: the passed-in stack is freed, as though by freeGinBtreeStack.
 */
void
ginInsertValue(GinBtree btree, GinBtreeStack *stack, void *insertdata,
			   GinStatsData *buildStats)
{
	bool		done;

	/* If the leaf page was incompletely split, finish the split first */
	if (GinPageIsIncompleteSplit(BufferGetPage(stack->buffer)))
		ginFinishSplit(btree, stack, false, buildStats);

	done = ginPlaceToPage(btree, stack,
						  insertdata, InvalidBlockNumber,
						  InvalidBuffer, buildStats);
	if (done)
	{
		LockBuffer(stack->buffer, GIN_UNLOCK);
		freeGinBtreeStack(stack);
	}
	else
		ginFinishSplit(btree, stack, true, buildStats);
}
