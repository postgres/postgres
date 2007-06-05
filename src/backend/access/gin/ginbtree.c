/*-------------------------------------------------------------------------
 *
 * ginbtree.c
 *	  page utilities routines for the postgres inverted index access method.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			$PostgreSQL: pgsql/src/backend/access/gin/ginbtree.c,v 1.6.2.1 2007/06/05 12:48:21 teodor Exp $
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/gin.h"
#include "miscadmin.h"

/*
 * Locks buffer by needed method for search.
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

GinBtreeStack *
ginPrepareFindLeafPage(GinBtree btree, BlockNumber blkno)
{
	GinBtreeStack *stack = (GinBtreeStack *) palloc(sizeof(GinBtreeStack));

	stack->blkno = blkno;
	stack->buffer = ReadBuffer(btree->index, stack->blkno);
	stack->parent = NULL;
	stack->predictNumber = 1;

	ginTraverseLock(stack->buffer, btree->searchMode);

	return stack;
}

/*
 * Locates leaf page contained tuple
 */
GinBtreeStack *
ginFindLeafPage(GinBtree btree, GinBtreeStack *stack)
{
	bool		isfirst = TRUE;
	BlockNumber rootBlkno;

	if (!stack)
		stack = ginPrepareFindLeafPage(btree, GIN_ROOT_BLKNO);
	rootBlkno = stack->blkno;

	for (;;)
	{
		Page		page;
		BlockNumber child;
		int			access = GIN_SHARE;

		stack->off = InvalidOffsetNumber;

		page = BufferGetPage(stack->buffer);

		if (isfirst)
		{
			if (GinPageIsLeaf(page) && !btree->searchMode)
				access = GIN_EXCLUSIVE;
			isfirst = FALSE;
		}
		else
			access = ginTraverseLock(stack->buffer, btree->searchMode);

		/*
		 * ok, page is correctly locked, we should check to move right ..,
		 * root never has a right link, so small optimization
		 */
		while (btree->fullScan == FALSE && stack->blkno != rootBlkno && btree->isMoveRight(btree, page))
		{
			BlockNumber rightlink = GinPageGetOpaque(page)->rightlink;

			if (rightlink == InvalidBlockNumber)
				/* rightmost page */
				break;

			stack->blkno = rightlink;
			LockBuffer(stack->buffer, GIN_UNLOCK);
			stack->buffer = ReleaseAndReadBuffer(stack->buffer, btree->index, stack->blkno);
			LockBuffer(stack->buffer, access);
			page = BufferGetPage(stack->buffer);
		}

		if (GinPageIsLeaf(page))	/* we found, return locked page */
			return stack;

		/* now we have correct buffer, try to find child */
		child = btree->findChildPage(btree, stack);

		LockBuffer(stack->buffer, GIN_UNLOCK);
		Assert(child != InvalidBlockNumber);
		Assert(stack->blkno != child);

		if (btree->searchMode)
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

	/* keep compiler happy */
	return NULL;
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
 * Try to find parent for current stack position, returns correct
 * parent and child's offset in  stack->parent.
 * Function should never release root page to prevent conflicts
 * with vacuum process
 */
void
findParents(GinBtree btree, GinBtreeStack *stack,
			BlockNumber rootBlkno)
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
		root->blkno = rootBlkno;
		root->buffer = ReadBuffer(btree->index, rootBlkno);
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

		Assert(root->blkno == rootBlkno);
		Assert(BufferGetBlockNumber(root->buffer) == rootBlkno);
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

	leftmostBlkno = blkno = btree->getLeftMostPage(btree, page);
	LockBuffer(root->buffer, GIN_UNLOCK);
	Assert(blkno != InvalidBlockNumber);


	for (;;)
	{
		buffer = ReadBuffer(btree->index, blkno);
		LockBuffer(buffer, GIN_EXCLUSIVE);
		page = BufferGetPage(buffer);
		if (GinPageIsLeaf(page))
			elog(ERROR, "Lost path");

		leftmostBlkno = btree->getLeftMostPage(btree, page);

		while ((offset = btree->findChildPtr(btree, page, stack->blkno, InvalidOffsetNumber)) == InvalidOffsetNumber)
		{
			blkno = GinPageGetOpaque(page)->rightlink;
			LockBuffer(buffer, GIN_UNLOCK);
			ReleaseBuffer(buffer);
			if (blkno == InvalidBlockNumber)
				break;
			buffer = ReadBuffer(btree->index, blkno);
			LockBuffer(buffer, GIN_EXCLUSIVE);
			page = BufferGetPage(buffer);
		}

		if (blkno != InvalidBlockNumber)
		{
			ptr = (GinBtreeStack *) palloc(sizeof(GinBtreeStack));
			ptr->blkno = blkno;
			ptr->buffer = buffer;
			ptr->parent = root; /* it's may be wrong, but in next call we will
								 * correct */
			ptr->off = offset;
			stack->parent = ptr;
			return;
		}

		blkno = leftmostBlkno;
	}
}

/*
 * Insert value (stored in GinBtree) to tree described by stack
 */
void
ginInsertValue(GinBtree btree, GinBtreeStack *stack)
{
	GinBtreeStack *parent = stack;
	BlockNumber rootBlkno = InvalidBuffer;
	Page		page,
				rpage,
				lpage;

	/* remember root BlockNumber */
	while (parent)
	{
		rootBlkno = parent->blkno;
		parent = parent->parent;
	}

	while (stack)
	{
		XLogRecData *rdata;
		BlockNumber savedRightLink;

		page = BufferGetPage(stack->buffer);
		savedRightLink = GinPageGetOpaque(page)->rightlink;

		if (btree->isEnoughSpace(btree, stack->buffer, stack->off))
		{
			START_CRIT_SECTION();
			btree->placeToPage(btree, stack->buffer, stack->off, &rdata);

			MarkBufferDirty(stack->buffer);

			if (!btree->index->rd_istemp)
			{
				XLogRecPtr	recptr;

				recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_INSERT, rdata);
				PageSetLSN(page, recptr);
				PageSetTLI(page, ThisTimeLineID);
			}

			UnlockReleaseBuffer(stack->buffer);
			END_CRIT_SECTION();

			freeGinBtreeStack(stack->parent);
			return;
		}
		else
		{
			Buffer		rbuffer = GinNewBuffer(btree->index);
			Page		newlpage;

			/*
			 * newlpage is a pointer to memory page, it doesn't associate
			 * with buffer, stack->buffer shoud be untouched
			 */
			newlpage = btree->splitPage(btree, stack->buffer, rbuffer, stack->off, &rdata);


			((ginxlogSplit *) (rdata->data))->rootBlkno = rootBlkno;

			parent = stack->parent;

			if (parent == NULL)
			{
				/*
				 * split root, so we need to allocate new left page and place
				 * pointer on root to left and right page
				 */
				Buffer		lbuffer = GinNewBuffer(btree->index);

				((ginxlogSplit *) (rdata->data))->isRootSplit = TRUE;
				((ginxlogSplit *) (rdata->data))->rrlink = InvalidBlockNumber;


				page = BufferGetPage(stack->buffer);
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

				if (!btree->index->rd_istemp)
				{
					XLogRecPtr	recptr;

					recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_SPLIT, rdata);
					PageSetLSN(page, recptr);
					PageSetTLI(page, ThisTimeLineID);
					PageSetLSN(lpage, recptr);
					PageSetTLI(lpage, ThisTimeLineID);
					PageSetLSN(rpage, recptr);
					PageSetTLI(rpage, ThisTimeLineID);
				}

				UnlockReleaseBuffer(rbuffer);
				UnlockReleaseBuffer(lbuffer);
				UnlockReleaseBuffer(stack->buffer);

				END_CRIT_SECTION();

				return;
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

				if (!btree->index->rd_istemp)
				{
					XLogRecPtr	recptr;

					recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_SPLIT, rdata);
					PageSetLSN(lpage, recptr);
					PageSetTLI(lpage, ThisTimeLineID);
					PageSetLSN(rpage, recptr);
					PageSetTLI(rpage, ThisTimeLineID);
				}
				UnlockReleaseBuffer(rbuffer);
				END_CRIT_SECTION();
			}
		}

		btree->isDelete = FALSE;

		/* search parent to lock */
		LockBuffer(parent->buffer, GIN_EXCLUSIVE);

		/* move right if it's needed */
		page = BufferGetPage(parent->buffer);
		while ((parent->off = btree->findChildPtr(btree, page, stack->blkno, parent->off)) == InvalidOffsetNumber)
		{
			BlockNumber rightlink = GinPageGetOpaque(page)->rightlink;

			LockBuffer(parent->buffer, GIN_UNLOCK);

			if (rightlink == InvalidBlockNumber)
			{
				/*
				 * rightmost page, but we don't find parent, we should use
				 * plain search...
				 */
				findParents(btree, stack, rootBlkno);
				parent = stack->parent;
				page = BufferGetPage(parent->buffer);
				break;
			}

			parent->blkno = rightlink;
			parent->buffer = ReleaseAndReadBuffer(parent->buffer, btree->index, parent->blkno);
			LockBuffer(parent->buffer, GIN_EXCLUSIVE);
			page = BufferGetPage(parent->buffer);
		}

		UnlockReleaseBuffer(stack->buffer);
		pfree(stack);
		stack = parent;
	}
}
