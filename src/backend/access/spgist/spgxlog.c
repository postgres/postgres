/*-------------------------------------------------------------------------
 *
 * spgxlog.c
 *	  WAL replay logic for SP-GiST
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			 src/backend/access/spgist/spgxlog.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/bufmask.h"
#include "access/spgist_private.h"
#include "access/spgxlog.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "storage/standby.h"
#include "utils/memutils.h"


static MemoryContext opCtx;		/* working memory for operations */


/*
 * Prepare a dummy SpGistState, with just the minimum info needed for replay.
 *
 * At present, all we need is enough info to support spgFormDeadTuple(),
 * plus the isBuild flag.
 */
static void
fillFakeState(SpGistState *state, spgxlogState stateSrc)
{
	memset(state, 0, sizeof(*state));

	state->myXid = stateSrc.myXid;
	state->isBuild = stateSrc.isBuild;
	state->deadTupleStorage = palloc0(SGDTSIZE);
}

/*
 * Add a leaf tuple, or replace an existing placeholder tuple.  This is used
 * to replay SpGistPageAddNewItem() operations.  If the offset points at an
 * existing tuple, it had better be a placeholder tuple.
 */
static void
addOrReplaceTuple(Page page, Item tuple, int size, OffsetNumber offset)
{
	if (offset <= PageGetMaxOffsetNumber(page))
	{
		SpGistDeadTuple dt = (SpGistDeadTuple) PageGetItem(page,
														   PageGetItemId(page, offset));

		if (dt->tupstate != SPGIST_PLACEHOLDER)
			elog(ERROR, "SPGiST tuple to be replaced is not a placeholder");

		Assert(SpGistPageGetOpaque(page)->nPlaceholder > 0);
		SpGistPageGetOpaque(page)->nPlaceholder--;

		PageIndexTupleDelete(page, offset);
	}

	Assert(offset <= PageGetMaxOffsetNumber(page) + 1);

	if (PageAddItem(page, tuple, size, offset, false, false) != offset)
		elog(ERROR, "failed to add item of size %u to SPGiST index page",
			 size);
}

static void
spgRedoAddLeaf(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	char	   *ptr = XLogRecGetData(record);
	spgxlogAddLeaf *xldata = (spgxlogAddLeaf *) ptr;
	char	   *leafTuple;
	SpGistLeafTupleData leafTupleHdr;
	Buffer		buffer;
	Page		page;
	XLogRedoAction action;

	ptr += sizeof(spgxlogAddLeaf);
	leafTuple = ptr;
	/* the leaf tuple is unaligned, so make a copy to access its header */
	memcpy(&leafTupleHdr, leafTuple, sizeof(SpGistLeafTupleData));

	/*
	 * In normal operation we would have both current and parent pages locked
	 * simultaneously; but in WAL replay it should be safe to update the leaf
	 * page before updating the parent.
	 */
	if (xldata->newPage)
	{
		buffer = XLogInitBufferForRedo(record, 0);
		SpGistInitBuffer(buffer,
						 SPGIST_LEAF | (xldata->storesNulls ? SPGIST_NULLS : 0));
		action = BLK_NEEDS_REDO;
	}
	else
		action = XLogReadBufferForRedo(record, 0, &buffer);

	if (action == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(buffer);

		/* insert new tuple */
		if (xldata->offnumLeaf != xldata->offnumHeadLeaf)
		{
			/* normal cases, tuple was added by SpGistPageAddNewItem */
			addOrReplaceTuple(page, (Item) leafTuple, leafTupleHdr.size,
							  xldata->offnumLeaf);

			/* update head tuple's chain link if needed */
			if (xldata->offnumHeadLeaf != InvalidOffsetNumber)
			{
				SpGistLeafTuple head;

				head = (SpGistLeafTuple) PageGetItem(page,
													 PageGetItemId(page, xldata->offnumHeadLeaf));
				Assert(head->nextOffset == leafTupleHdr.nextOffset);
				head->nextOffset = xldata->offnumLeaf;
			}
		}
		else
		{
			/* replacing a DEAD tuple */
			PageIndexTupleDelete(page, xldata->offnumLeaf);
			if (PageAddItem(page,
							(Item) leafTuple, leafTupleHdr.size,
							xldata->offnumLeaf, false, false) != xldata->offnumLeaf)
				elog(ERROR, "failed to add item of size %u to SPGiST index page",
					 leafTupleHdr.size);
		}

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	/* update parent downlink if necessary */
	if (xldata->offnumParent != InvalidOffsetNumber)
	{
		if (XLogReadBufferForRedo(record, 1, &buffer) == BLK_NEEDS_REDO)
		{
			SpGistInnerTuple tuple;
			BlockNumber blknoLeaf;

			XLogRecGetBlockTag(record, 0, NULL, NULL, &blknoLeaf);

			page = BufferGetPage(buffer);

			tuple = (SpGistInnerTuple) PageGetItem(page,
												   PageGetItemId(page, xldata->offnumParent));

			spgUpdateNodeLink(tuple, xldata->nodeI,
							  blknoLeaf, xldata->offnumLeaf);

			PageSetLSN(page, lsn);
			MarkBufferDirty(buffer);
		}
		if (BufferIsValid(buffer))
			UnlockReleaseBuffer(buffer);
	}
}

static void
spgRedoMoveLeafs(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	char	   *ptr = XLogRecGetData(record);
	spgxlogMoveLeafs *xldata = (spgxlogMoveLeafs *) ptr;
	SpGistState state;
	OffsetNumber *toDelete;
	OffsetNumber *toInsert;
	int			nInsert;
	Buffer		buffer;
	Page		page;
	XLogRedoAction action;
	BlockNumber blknoDst;

	XLogRecGetBlockTag(record, 1, NULL, NULL, &blknoDst);

	fillFakeState(&state, xldata->stateSrc);

	nInsert = xldata->replaceDead ? 1 : xldata->nMoves + 1;

	ptr += SizeOfSpgxlogMoveLeafs;
	toDelete = (OffsetNumber *) ptr;
	ptr += sizeof(OffsetNumber) * xldata->nMoves;
	toInsert = (OffsetNumber *) ptr;
	ptr += sizeof(OffsetNumber) * nInsert;

	/* now ptr points to the list of leaf tuples */

	/*
	 * In normal operation we would have all three pages (source, dest, and
	 * parent) locked simultaneously; but in WAL replay it should be safe to
	 * update them one at a time, as long as we do it in the right order.
	 */

	/* Insert tuples on the dest page (do first, so redirect is valid) */
	if (xldata->newPage)
	{
		buffer = XLogInitBufferForRedo(record, 1);
		SpGistInitBuffer(buffer,
						 SPGIST_LEAF | (xldata->storesNulls ? SPGIST_NULLS : 0));
		action = BLK_NEEDS_REDO;
	}
	else
		action = XLogReadBufferForRedo(record, 1, &buffer);

	if (action == BLK_NEEDS_REDO)
	{
		int			i;

		page = BufferGetPage(buffer);

		for (i = 0; i < nInsert; i++)
		{
			char	   *leafTuple;
			SpGistLeafTupleData leafTupleHdr;

			/*
			 * the tuples are not aligned, so must copy to access the size
			 * field.
			 */
			leafTuple = ptr;
			memcpy(&leafTupleHdr, leafTuple,
				   sizeof(SpGistLeafTupleData));

			addOrReplaceTuple(page, (Item) leafTuple,
							  leafTupleHdr.size, toInsert[i]);
			ptr += leafTupleHdr.size;
		}

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	/* Delete tuples from the source page, inserting a redirection pointer */
	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(buffer);

		spgPageIndexMultiDelete(&state, page, toDelete, xldata->nMoves,
								state.isBuild ? SPGIST_PLACEHOLDER : SPGIST_REDIRECT,
								SPGIST_PLACEHOLDER,
								blknoDst,
								toInsert[nInsert - 1]);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	/* And update the parent downlink */
	if (XLogReadBufferForRedo(record, 2, &buffer) == BLK_NEEDS_REDO)
	{
		SpGistInnerTuple tuple;

		page = BufferGetPage(buffer);

		tuple = (SpGistInnerTuple) PageGetItem(page,
											   PageGetItemId(page, xldata->offnumParent));

		spgUpdateNodeLink(tuple, xldata->nodeI,
						  blknoDst, toInsert[nInsert - 1]);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
spgRedoAddNode(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	char	   *ptr = XLogRecGetData(record);
	spgxlogAddNode *xldata = (spgxlogAddNode *) ptr;
	char	   *innerTuple;
	SpGistInnerTupleData innerTupleHdr;
	SpGistState state;
	Buffer		buffer;
	Page		page;
	XLogRedoAction action;

	ptr += sizeof(spgxlogAddNode);
	innerTuple = ptr;
	/* the tuple is unaligned, so make a copy to access its header */
	memcpy(&innerTupleHdr, innerTuple, sizeof(SpGistInnerTupleData));

	fillFakeState(&state, xldata->stateSrc);

	if (!XLogRecHasBlockRef(record, 1))
	{
		/* update in place */
		Assert(xldata->parentBlk == -1);
		if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
		{
			page = BufferGetPage(buffer);

			PageIndexTupleDelete(page, xldata->offnum);
			if (PageAddItem(page, (Item) innerTuple, innerTupleHdr.size,
							xldata->offnum,
							false, false) != xldata->offnum)
				elog(ERROR, "failed to add item of size %u to SPGiST index page",
					 innerTupleHdr.size);

			PageSetLSN(page, lsn);
			MarkBufferDirty(buffer);
		}
		if (BufferIsValid(buffer))
			UnlockReleaseBuffer(buffer);
	}
	else
	{
		BlockNumber blkno;
		BlockNumber blknoNew;

		XLogRecGetBlockTag(record, 0, NULL, NULL, &blkno);
		XLogRecGetBlockTag(record, 1, NULL, NULL, &blknoNew);

		/*
		 * In normal operation we would have all three pages (source, dest,
		 * and parent) locked simultaneously; but in WAL replay it should be
		 * safe to update them one at a time, as long as we do it in the right
		 * order. We must insert the new tuple before replacing the old tuple
		 * with the redirect tuple.
		 */

		/* Install new tuple first so redirect is valid */
		if (xldata->newPage)
		{
			/* AddNode is not used for nulls pages */
			buffer = XLogInitBufferForRedo(record, 1);
			SpGistInitBuffer(buffer, 0);
			action = BLK_NEEDS_REDO;
		}
		else
			action = XLogReadBufferForRedo(record, 1, &buffer);
		if (action == BLK_NEEDS_REDO)
		{
			page = BufferGetPage(buffer);

			addOrReplaceTuple(page, (Item) innerTuple,
							  innerTupleHdr.size, xldata->offnumNew);

			/*
			 * If parent is in this same page, update it now.
			 */
			if (xldata->parentBlk == 1)
			{
				SpGistInnerTuple parentTuple;

				parentTuple = (SpGistInnerTuple) PageGetItem(page,
															 PageGetItemId(page, xldata->offnumParent));

				spgUpdateNodeLink(parentTuple, xldata->nodeI,
								  blknoNew, xldata->offnumNew);
			}
			PageSetLSN(page, lsn);
			MarkBufferDirty(buffer);
		}
		if (BufferIsValid(buffer))
			UnlockReleaseBuffer(buffer);

		/* Delete old tuple, replacing it with redirect or placeholder tuple */
		if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
		{
			SpGistDeadTuple dt;

			page = BufferGetPage(buffer);

			if (state.isBuild)
				dt = spgFormDeadTuple(&state, SPGIST_PLACEHOLDER,
									  InvalidBlockNumber,
									  InvalidOffsetNumber);
			else
				dt = spgFormDeadTuple(&state, SPGIST_REDIRECT,
									  blknoNew,
									  xldata->offnumNew);

			PageIndexTupleDelete(page, xldata->offnum);
			if (PageAddItem(page, (Item) dt, dt->size,
							xldata->offnum,
							false, false) != xldata->offnum)
				elog(ERROR, "failed to add item of size %u to SPGiST index page",
					 dt->size);

			if (state.isBuild)
				SpGistPageGetOpaque(page)->nPlaceholder++;
			else
				SpGistPageGetOpaque(page)->nRedirection++;

			/*
			 * If parent is in this same page, update it now.
			 */
			if (xldata->parentBlk == 0)
			{
				SpGistInnerTuple parentTuple;

				parentTuple = (SpGistInnerTuple) PageGetItem(page,
															 PageGetItemId(page, xldata->offnumParent));

				spgUpdateNodeLink(parentTuple, xldata->nodeI,
								  blknoNew, xldata->offnumNew);
			}
			PageSetLSN(page, lsn);
			MarkBufferDirty(buffer);
		}
		if (BufferIsValid(buffer))
			UnlockReleaseBuffer(buffer);

		/*
		 * Update parent downlink (if we didn't do it as part of the source or
		 * destination page update already).
		 */
		if (xldata->parentBlk == 2)
		{
			if (XLogReadBufferForRedo(record, 2, &buffer) == BLK_NEEDS_REDO)
			{
				SpGistInnerTuple parentTuple;

				page = BufferGetPage(buffer);

				parentTuple = (SpGistInnerTuple) PageGetItem(page,
															 PageGetItemId(page, xldata->offnumParent));

				spgUpdateNodeLink(parentTuple, xldata->nodeI,
								  blknoNew, xldata->offnumNew);

				PageSetLSN(page, lsn);
				MarkBufferDirty(buffer);
			}
			if (BufferIsValid(buffer))
				UnlockReleaseBuffer(buffer);
		}
	}
}

static void
spgRedoSplitTuple(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	char	   *ptr = XLogRecGetData(record);
	spgxlogSplitTuple *xldata = (spgxlogSplitTuple *) ptr;
	char	   *prefixTuple;
	SpGistInnerTupleData prefixTupleHdr;
	char	   *postfixTuple;
	SpGistInnerTupleData postfixTupleHdr;
	Buffer		buffer;
	Page		page;
	XLogRedoAction action;

	ptr += sizeof(spgxlogSplitTuple);
	prefixTuple = ptr;
	/* the prefix tuple is unaligned, so make a copy to access its header */
	memcpy(&prefixTupleHdr, prefixTuple, sizeof(SpGistInnerTupleData));
	ptr += prefixTupleHdr.size;
	postfixTuple = ptr;
	/* postfix tuple is also unaligned */
	memcpy(&postfixTupleHdr, postfixTuple, sizeof(SpGistInnerTupleData));

	/*
	 * In normal operation we would have both pages locked simultaneously; but
	 * in WAL replay it should be safe to update them one at a time, as long
	 * as we do it in the right order.
	 */

	/* insert postfix tuple first to avoid dangling link */
	if (!xldata->postfixBlkSame)
	{
		if (xldata->newPage)
		{
			buffer = XLogInitBufferForRedo(record, 1);
			/* SplitTuple is not used for nulls pages */
			SpGistInitBuffer(buffer, 0);
			action = BLK_NEEDS_REDO;
		}
		else
			action = XLogReadBufferForRedo(record, 1, &buffer);
		if (action == BLK_NEEDS_REDO)
		{
			page = BufferGetPage(buffer);

			addOrReplaceTuple(page, (Item) postfixTuple,
							  postfixTupleHdr.size, xldata->offnumPostfix);

			PageSetLSN(page, lsn);
			MarkBufferDirty(buffer);
		}
		if (BufferIsValid(buffer))
			UnlockReleaseBuffer(buffer);
	}

	/* now handle the original page */
	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(buffer);

		PageIndexTupleDelete(page, xldata->offnumPrefix);
		if (PageAddItem(page, (Item) prefixTuple, prefixTupleHdr.size,
						xldata->offnumPrefix, false, false) != xldata->offnumPrefix)
			elog(ERROR, "failed to add item of size %u to SPGiST index page",
				 prefixTupleHdr.size);

		if (xldata->postfixBlkSame)
			addOrReplaceTuple(page, (Item) postfixTuple,
							  postfixTupleHdr.size,
							  xldata->offnumPostfix);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
spgRedoPickSplit(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	char	   *ptr = XLogRecGetData(record);
	spgxlogPickSplit *xldata = (spgxlogPickSplit *) ptr;
	char	   *innerTuple;
	SpGistInnerTupleData innerTupleHdr;
	SpGistState state;
	OffsetNumber *toDelete;
	OffsetNumber *toInsert;
	uint8	   *leafPageSelect;
	Buffer		srcBuffer;
	Buffer		destBuffer;
	Buffer		innerBuffer;
	Page		srcPage;
	Page		destPage;
	Page		page;
	int			i;
	BlockNumber blknoInner;
	XLogRedoAction action;

	XLogRecGetBlockTag(record, 2, NULL, NULL, &blknoInner);

	fillFakeState(&state, xldata->stateSrc);

	ptr += SizeOfSpgxlogPickSplit;
	toDelete = (OffsetNumber *) ptr;
	ptr += sizeof(OffsetNumber) * xldata->nDelete;
	toInsert = (OffsetNumber *) ptr;
	ptr += sizeof(OffsetNumber) * xldata->nInsert;
	leafPageSelect = (uint8 *) ptr;
	ptr += sizeof(uint8) * xldata->nInsert;

	innerTuple = ptr;
	/* the inner tuple is unaligned, so make a copy to access its header */
	memcpy(&innerTupleHdr, innerTuple, sizeof(SpGistInnerTupleData));
	ptr += innerTupleHdr.size;

	/* now ptr points to the list of leaf tuples */

	if (xldata->isRootSplit)
	{
		/* when splitting root, we touch it only in the guise of new inner */
		srcBuffer = InvalidBuffer;
		srcPage = NULL;
	}
	else if (xldata->initSrc)
	{
		/* just re-init the source page */
		srcBuffer = XLogInitBufferForRedo(record, 0);
		srcPage = (Page) BufferGetPage(srcBuffer);

		SpGistInitBuffer(srcBuffer,
						 SPGIST_LEAF | (xldata->storesNulls ? SPGIST_NULLS : 0));
		/* don't update LSN etc till we're done with it */
	}
	else
	{
		/*
		 * Delete the specified tuples from source page.  (In case we're in
		 * Hot Standby, we need to hold lock on the page till we're done
		 * inserting leaf tuples and the new inner tuple, else the added
		 * redirect tuple will be a dangling link.)
		 */
		srcPage = NULL;
		if (XLogReadBufferForRedo(record, 0, &srcBuffer) == BLK_NEEDS_REDO)
		{
			srcPage = BufferGetPage(srcBuffer);

			/*
			 * We have it a bit easier here than in doPickSplit(), because we
			 * know the inner tuple's location already, so we can inject the
			 * correct redirection tuple now.
			 */
			if (!state.isBuild)
				spgPageIndexMultiDelete(&state, srcPage,
										toDelete, xldata->nDelete,
										SPGIST_REDIRECT,
										SPGIST_PLACEHOLDER,
										blknoInner,
										xldata->offnumInner);
			else
				spgPageIndexMultiDelete(&state, srcPage,
										toDelete, xldata->nDelete,
										SPGIST_PLACEHOLDER,
										SPGIST_PLACEHOLDER,
										InvalidBlockNumber,
										InvalidOffsetNumber);

			/* don't update LSN etc till we're done with it */
		}
	}

	/* try to access dest page if any */
	if (!XLogRecHasBlockRef(record, 1))
	{
		destBuffer = InvalidBuffer;
		destPage = NULL;
	}
	else if (xldata->initDest)
	{
		/* just re-init the dest page */
		destBuffer = XLogInitBufferForRedo(record, 1);
		destPage = (Page) BufferGetPage(destBuffer);

		SpGistInitBuffer(destBuffer,
						 SPGIST_LEAF | (xldata->storesNulls ? SPGIST_NULLS : 0));
		/* don't update LSN etc till we're done with it */
	}
	else
	{
		/*
		 * We could probably release the page lock immediately in the
		 * full-page-image case, but for safety let's hold it till later.
		 */
		if (XLogReadBufferForRedo(record, 1, &destBuffer) == BLK_NEEDS_REDO)
			destPage = (Page) BufferGetPage(destBuffer);
		else
			destPage = NULL;	/* don't do any page updates */
	}

	/* restore leaf tuples to src and/or dest page */
	for (i = 0; i < xldata->nInsert; i++)
	{
		char	   *leafTuple;
		SpGistLeafTupleData leafTupleHdr;

		/* the tuples are not aligned, so must copy to access the size field. */
		leafTuple = ptr;
		memcpy(&leafTupleHdr, leafTuple, sizeof(SpGistLeafTupleData));
		ptr += leafTupleHdr.size;

		page = leafPageSelect[i] ? destPage : srcPage;
		if (page == NULL)
			continue;			/* no need to touch this page */

		addOrReplaceTuple(page, (Item) leafTuple, leafTupleHdr.size,
						  toInsert[i]);
	}

	/* Now update src and dest page LSNs if needed */
	if (srcPage != NULL)
	{
		PageSetLSN(srcPage, lsn);
		MarkBufferDirty(srcBuffer);
	}
	if (destPage != NULL)
	{
		PageSetLSN(destPage, lsn);
		MarkBufferDirty(destBuffer);
	}

	/* restore new inner tuple */
	if (xldata->initInner)
	{
		innerBuffer = XLogInitBufferForRedo(record, 2);
		SpGistInitBuffer(innerBuffer, (xldata->storesNulls ? SPGIST_NULLS : 0));
		action = BLK_NEEDS_REDO;
	}
	else
		action = XLogReadBufferForRedo(record, 2, &innerBuffer);

	if (action == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(innerBuffer);

		addOrReplaceTuple(page, (Item) innerTuple, innerTupleHdr.size,
						  xldata->offnumInner);

		/* if inner is also parent, update link while we're here */
		if (xldata->innerIsParent)
		{
			SpGistInnerTuple parent;

			parent = (SpGistInnerTuple) PageGetItem(page,
													PageGetItemId(page, xldata->offnumParent));
			spgUpdateNodeLink(parent, xldata->nodeI,
							  blknoInner, xldata->offnumInner);
		}

		PageSetLSN(page, lsn);
		MarkBufferDirty(innerBuffer);
	}
	if (BufferIsValid(innerBuffer))
		UnlockReleaseBuffer(innerBuffer);

	/*
	 * Now we can release the leaf-page locks.  It's okay to do this before
	 * updating the parent downlink.
	 */
	if (BufferIsValid(srcBuffer))
		UnlockReleaseBuffer(srcBuffer);
	if (BufferIsValid(destBuffer))
		UnlockReleaseBuffer(destBuffer);

	/* update parent downlink, unless we did it above */
	if (XLogRecHasBlockRef(record, 3))
	{
		Buffer		parentBuffer;

		if (XLogReadBufferForRedo(record, 3, &parentBuffer) == BLK_NEEDS_REDO)
		{
			SpGistInnerTuple parent;

			page = BufferGetPage(parentBuffer);

			parent = (SpGistInnerTuple) PageGetItem(page,
													PageGetItemId(page, xldata->offnumParent));
			spgUpdateNodeLink(parent, xldata->nodeI,
							  blknoInner, xldata->offnumInner);

			PageSetLSN(page, lsn);
			MarkBufferDirty(parentBuffer);
		}
		if (BufferIsValid(parentBuffer))
			UnlockReleaseBuffer(parentBuffer);
	}
	else
		Assert(xldata->innerIsParent || xldata->isRootSplit);
}

static void
spgRedoVacuumLeaf(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	char	   *ptr = XLogRecGetData(record);
	spgxlogVacuumLeaf *xldata = (spgxlogVacuumLeaf *) ptr;
	OffsetNumber *toDead;
	OffsetNumber *toPlaceholder;
	OffsetNumber *moveSrc;
	OffsetNumber *moveDest;
	OffsetNumber *chainSrc;
	OffsetNumber *chainDest;
	SpGistState state;
	Buffer		buffer;
	Page		page;
	int			i;

	fillFakeState(&state, xldata->stateSrc);

	ptr += SizeOfSpgxlogVacuumLeaf;
	toDead = (OffsetNumber *) ptr;
	ptr += sizeof(OffsetNumber) * xldata->nDead;
	toPlaceholder = (OffsetNumber *) ptr;
	ptr += sizeof(OffsetNumber) * xldata->nPlaceholder;
	moveSrc = (OffsetNumber *) ptr;
	ptr += sizeof(OffsetNumber) * xldata->nMove;
	moveDest = (OffsetNumber *) ptr;
	ptr += sizeof(OffsetNumber) * xldata->nMove;
	chainSrc = (OffsetNumber *) ptr;
	ptr += sizeof(OffsetNumber) * xldata->nChain;
	chainDest = (OffsetNumber *) ptr;

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(buffer);

		spgPageIndexMultiDelete(&state, page,
								toDead, xldata->nDead,
								SPGIST_DEAD, SPGIST_DEAD,
								InvalidBlockNumber,
								InvalidOffsetNumber);

		spgPageIndexMultiDelete(&state, page,
								toPlaceholder, xldata->nPlaceholder,
								SPGIST_PLACEHOLDER, SPGIST_PLACEHOLDER,
								InvalidBlockNumber,
								InvalidOffsetNumber);

		/* see comments in vacuumLeafPage() */
		for (i = 0; i < xldata->nMove; i++)
		{
			ItemId		idSrc = PageGetItemId(page, moveSrc[i]);
			ItemId		idDest = PageGetItemId(page, moveDest[i]);
			ItemIdData	tmp;

			tmp = *idSrc;
			*idSrc = *idDest;
			*idDest = tmp;
		}

		spgPageIndexMultiDelete(&state, page,
								moveSrc, xldata->nMove,
								SPGIST_PLACEHOLDER, SPGIST_PLACEHOLDER,
								InvalidBlockNumber,
								InvalidOffsetNumber);

		for (i = 0; i < xldata->nChain; i++)
		{
			SpGistLeafTuple lt;

			lt = (SpGistLeafTuple) PageGetItem(page,
											   PageGetItemId(page, chainSrc[i]));
			Assert(lt->tupstate == SPGIST_LIVE);
			lt->nextOffset = chainDest[i];
		}

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
spgRedoVacuumRoot(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	char	   *ptr = XLogRecGetData(record);
	spgxlogVacuumRoot *xldata = (spgxlogVacuumRoot *) ptr;
	OffsetNumber *toDelete;
	Buffer		buffer;
	Page		page;

	toDelete = xldata->offsets;

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(buffer);

		/* The tuple numbers are in order */
		PageIndexMultiDelete(page, toDelete, xldata->nDelete);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
spgRedoVacuumRedirect(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	char	   *ptr = XLogRecGetData(record);
	spgxlogVacuumRedirect *xldata = (spgxlogVacuumRedirect *) ptr;
	OffsetNumber *itemToPlaceholder;
	Buffer		buffer;

	itemToPlaceholder = xldata->offsets;

	/*
	 * If any redirection tuples are being removed, make sure there are no
	 * live Hot Standby transactions that might need to see them.
	 */
	if (InHotStandby)
	{
		if (TransactionIdIsValid(xldata->newestRedirectXid))
		{
			RelFileNode node;

			XLogRecGetBlockTag(record, 0, &node, NULL, NULL);
			ResolveRecoveryConflictWithSnapshot(xldata->newestRedirectXid,
												node);
		}
	}

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buffer);
		SpGistPageOpaque opaque = SpGistPageGetOpaque(page);
		int			i;

		/* Convert redirect pointers to plain placeholders */
		for (i = 0; i < xldata->nToPlaceholder; i++)
		{
			SpGistDeadTuple dt;

			dt = (SpGistDeadTuple) PageGetItem(page,
											   PageGetItemId(page, itemToPlaceholder[i]));
			Assert(dt->tupstate == SPGIST_REDIRECT);
			dt->tupstate = SPGIST_PLACEHOLDER;
			ItemPointerSetInvalid(&dt->pointer);
		}

		Assert(opaque->nRedirection >= xldata->nToPlaceholder);
		opaque->nRedirection -= xldata->nToPlaceholder;
		opaque->nPlaceholder += xldata->nToPlaceholder;

		/* Remove placeholder tuples at end of page */
		if (xldata->firstPlaceholder != InvalidOffsetNumber)
		{
			int			max = PageGetMaxOffsetNumber(page);
			OffsetNumber *toDelete;

			toDelete = palloc(sizeof(OffsetNumber) * max);

			for (i = xldata->firstPlaceholder; i <= max; i++)
				toDelete[i - xldata->firstPlaceholder] = i;

			i = max - xldata->firstPlaceholder + 1;
			Assert(opaque->nPlaceholder >= i);
			opaque->nPlaceholder -= i;

			/* The array is sorted, so can use PageIndexMultiDelete */
			PageIndexMultiDelete(page, toDelete, i);

			pfree(toDelete);
		}

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

void
spg_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	MemoryContext oldCxt;

	oldCxt = MemoryContextSwitchTo(opCtx);
	switch (info)
	{
		case XLOG_SPGIST_ADD_LEAF:
			spgRedoAddLeaf(record);
			break;
		case XLOG_SPGIST_MOVE_LEAFS:
			spgRedoMoveLeafs(record);
			break;
		case XLOG_SPGIST_ADD_NODE:
			spgRedoAddNode(record);
			break;
		case XLOG_SPGIST_SPLIT_TUPLE:
			spgRedoSplitTuple(record);
			break;
		case XLOG_SPGIST_PICKSPLIT:
			spgRedoPickSplit(record);
			break;
		case XLOG_SPGIST_VACUUM_LEAF:
			spgRedoVacuumLeaf(record);
			break;
		case XLOG_SPGIST_VACUUM_ROOT:
			spgRedoVacuumRoot(record);
			break;
		case XLOG_SPGIST_VACUUM_REDIRECT:
			spgRedoVacuumRedirect(record);
			break;
		default:
			elog(PANIC, "spg_redo: unknown op code %u", info);
	}

	MemoryContextSwitchTo(oldCxt);
	MemoryContextReset(opCtx);
}

void
spg_xlog_startup(void)
{
	opCtx = AllocSetContextCreate(CurrentMemoryContext,
								  "SP-GiST temporary context",
								  ALLOCSET_DEFAULT_SIZES);
}

void
spg_xlog_cleanup(void)
{
	MemoryContextDelete(opCtx);
	opCtx = NULL;
}

/*
 * Mask a SpGist page before performing consistency checks on it.
 */
void
spg_mask(char *pagedata, BlockNumber blkno)
{
	Page		page = (Page) pagedata;
	PageHeader	pagehdr = (PageHeader) page;

	mask_page_lsn_and_checksum(page);

	mask_page_hint_bits(page);

	/*
	 * Mask the unused space, but only if the page's pd_lower appears to have
	 * been set correctly.
	 */
	if (pagehdr->pd_lower >= SizeOfPageHeaderData)
		mask_unused_space(page);
}
