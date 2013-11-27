/*-------------------------------------------------------------------------
 *
 * ginbtree.c
 *	  page utilities routines for the postgres inverted index access method.
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/ginbtree.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin_private.h"
#include "miscadmin.h"
#include "utils/rel.h"

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
ginFindLeafPage(GinBtree btree, bool searchMode)
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

		access = ginTraverseLock(stack->buffer, searchMode);

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
void
ginFindParents(GinBtree btree, GinBtreeStack *stack)
{
	Page		page;
	Buffer		buffer;
	BlockNumber blkno,
				leftmostBlkno;
	OffsetNumber offset;
	GinBtreeStack *root = stack->parent;
	GinBtreeStack *ptr;

	if (!root)
	{
		/* XLog mode... */
		root = (GinBtreeStack *) palloc(sizeof(GinBtreeStack));
		root->blkno = btree->rootBlkno;
		root->buffer = ReadBuffer(btree->index, btree->rootBlkno);
		LockBuffer(root->buffer, GIN_EXCLUSIVE);
		root->parent = NULL;
	}
	else
	{
		/*
		 * find root, we should not release root page until update is
		 * finished!!
		 */
		while (root->parent)
		{
			ReleaseBuffer(root->buffer);
			root = root->parent;
		}

		Assert(root->blkno == btree->rootBlkno);
		Assert(BufferGetBlockNumber(root->buffer) == btree->rootBlkno);
		LockBuffer(root->buffer, GIN_EXCLUSIVE);
	}
	root->off = InvalidOffsetNumber;

	page = BufferGetPage(root->buffer);
	Assert(!GinPageIsLeaf(page));

	/* check trivial case */
	if ((root->off = btree->findChildPtr(btree, page, stack->blkno, InvalidOffsetNumber)) != InvalidOffsetNumber)
	{
		stack->parent = root;
		return;
	}

	leftmostBlkno = blkno = btree->getLeftMostChild(btree, page);
	LockBuffer(root->buffer, GIN_UNLOCK);
	Assert(blkno != InvalidBlockNumber);

	for (;;)
	{
		buffer = ReadBuffer(btree->index, blkno);
		LockBuffer(buffer, GIN_EXCLUSIVE);
		page = BufferGetPage(buffer);
		if (GinPageIsLeaf(page))
			elog(ERROR, "Lost path");

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
		}

		if (blkno != InvalidBlockNumber)
		{
			ptr = (GinBtreeStack *) palloc(sizeof(GinBtreeStack));
			ptr->blkno = blkno;
			ptr->buffer = buffer;
			ptr->parent = root; /* it may be wrong, but in next call we will
								 * correct */
			ptr->off = offset;
			stack->parent = ptr;
			return;
		}

		blkno = leftmostBlkno;
	}
}

/*
 * Insert a new item to a page.
 *
 * Returns true if the insertion was finished. On false, the page was split and
 * the parent needs to be updated. (a root split returns true as it doesn't
 * need any further action by the caller to complete)
 *
 * When inserting a downlink to a internal page, the existing item at the
 * given location is updated to point to 'updateblkno'.
 *
 * stack->buffer is locked on entry, and is kept locked.
 */
static bool
ginPlaceToPage(GinBtree btree, GinBtreeStack *stack,
			   void *insertdata, BlockNumber updateblkno,
			   GinStatsData *buildStats)
{
	Page		page = BufferGetPage(stack->buffer);
	XLogRecData *rdata;
	bool		fit;

	/*
	 * Try to put the incoming tuple on the page. If it doesn't fit,
	 * placeToPage method will return false and leave the page unmodified, and
	 * we'll have to split the page.
	 */
	START_CRIT_SECTION();
	fit = btree->placeToPage(btree, stack->buffer, stack->off,
							 insertdata, updateblkno,
							 &rdata);
	if (fit)
	{
		MarkBufferDirty(stack->buffer);

		if (RelationNeedsWAL(btree->index))
		{
			XLogRecPtr	recptr;

			recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_INSERT, rdata);
			PageSetLSN(page, recptr);
		}

		END_CRIT_SECTION();

		return true;
	}
	else
	{
		/* Didn't fit, have to split */
		Buffer		rbuffer;
		Page		newlpage;
		BlockNumber savedRightLink;
		GinBtreeStack *parent;
		Page		lpage,
					rpage;

		END_CRIT_SECTION();

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

		/*
		 * newlpage is a pointer to memory page, it is not associated with a
		 * buffer. stack->buffer is not touched yet.
		 */
		newlpage = btree->splitPage(btree, stack->buffer, rbuffer, stack->off,
									insertdata, updateblkno,
									&rdata);

		((ginxlogSplit *) (rdata->data))->rootBlkno = btree->rootBlkno;

		parent = stack->parent;

		if (parent == NULL)
		{
			/*
			 * split root, so we need to allocate new left page and place
			 * pointer on root to left and right page
			 */
			Buffer		lbuffer = GinNewBuffer(btree->index);

			/* During index build, count the new page */
			if (buildStats)
			{
				if (btree->isData)
					buildStats->nDataPages++;
				else
					buildStats->nEntryPages++;
			}

			((ginxlogSplit *) (rdata->data))->isRootSplit = TRUE;
			((ginxlogSplit *) (rdata->data))->rrlink = InvalidBlockNumber;

			lpage = BufferGetPage(lbuffer);
			rpage = BufferGetPage(rbuffer);

			GinPageGetOpaque(rpage)->rightlink = InvalidBlockNumber;
			GinPageGetOpaque(newlpage)->rightlink = BufferGetBlockNumber(rbuffer);
			((ginxlogSplit *) (rdata->data))->lblkno = BufferGetBlockNumber(lbuffer);

			START_CRIT_SECTION();

			GinInitBuffer(stack->buffer, GinPageGetOpaque(newlpage)->flags & ~GIN_LEAF);
			PageRestoreTempPage(newlpage, lpage);
			btree->fillRoot(btree, stack->buffer, lbuffer, rbuffer);

			MarkBufferDirty(rbuffer);
			MarkBufferDirty(lbuffer);
			MarkBufferDirty(stack->buffer);

			if (RelationNeedsWAL(btree->index))
			{
				XLogRecPtr	recptr;

				recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_SPLIT, rdata);
				PageSetLSN(page, recptr);
				PageSetLSN(lpage, recptr);
				PageSetLSN(rpage, recptr);
			}

			UnlockReleaseBuffer(rbuffer);
			UnlockReleaseBuffer(lbuffer);
			END_CRIT_SECTION();

			/* During index build, count the newly-added root page */
			if (buildStats)
			{
				if (btree->isData)
					buildStats->nDataPages++;
				else
					buildStats->nEntryPages++;
			}

			return true;
		}
		else
		{
			/* split non-root page */
			((ginxlogSplit *) (rdata->data))->isRootSplit = FALSE;
			((ginxlogSplit *) (rdata->data))->rrlink = savedRightLink;

			lpage = BufferGetPage(stack->buffer);
			rpage = BufferGetPage(rbuffer);

			GinPageGetOpaque(rpage)->rightlink = savedRightLink;
			GinPageGetOpaque(newlpage)->rightlink = BufferGetBlockNumber(rbuffer);

			START_CRIT_SECTION();
			PageRestoreTempPage(newlpage, lpage);

			MarkBufferDirty(rbuffer);
			MarkBufferDirty(stack->buffer);

			if (RelationNeedsWAL(btree->index))
			{
				XLogRecPtr	recptr;

				recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_SPLIT, rdata);
				PageSetLSN(lpage, recptr);
				PageSetLSN(rpage, recptr);
			}
			UnlockReleaseBuffer(rbuffer);
			END_CRIT_SECTION();

			return false;
		}
	}
}

/*
 * Finish a split by inserting the downlink for the new page to parent.
 *
 * On entry, stack->buffer is exclusively locked.
 *
 * NB: the passed-in stack is freed, as though by freeGinBtreeStack.
 */
void
ginFinishSplit(GinBtree btree, GinBtreeStack *stack, GinStatsData *buildStats)
{
	Page		page;
	bool		done;

	/* this loop crawls up the stack until the insertion is complete */
	do
	{
		GinBtreeStack *parent = stack->parent;
		void	   *insertdata;
		BlockNumber updateblkno;

		insertdata = btree->prepareDownlink(btree, stack->buffer);
		updateblkno = GinPageGetOpaque(BufferGetPage(stack->buffer))->rightlink;

		/* search parent to lock */
		LockBuffer(parent->buffer, GIN_EXCLUSIVE);

		/* move right if it's needed */
		page = BufferGetPage(parent->buffer);
		while ((parent->off = btree->findChildPtr(btree, page, stack->blkno, parent->off)) == InvalidOffsetNumber)
		{
			BlockNumber rightlink = GinPageGetOpaque(page)->rightlink;

			if (rightlink == InvalidBlockNumber)
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
			parent->blkno = rightlink;
			page = BufferGetPage(parent->buffer);
		}

		/* release the child */
		UnlockReleaseBuffer(stack->buffer);
		pfree(stack);
		stack = parent;

		/* insert the downlink to parent */
		done = ginPlaceToPage(btree, stack,
							  insertdata, updateblkno,
							  buildStats);
		pfree(insertdata);
	} while (!done);
	LockBuffer(stack->buffer, GIN_UNLOCK);

	/* free the rest of the stack */
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

	done = ginPlaceToPage(btree, stack,
						  insertdata, InvalidBlockNumber,
						  buildStats);
	if (done)
	{
		LockBuffer(stack->buffer, GIN_UNLOCK);
		freeGinBtreeStack(stack);
	}
	else
		ginFinishSplit(btree, stack, buildStats);
}
