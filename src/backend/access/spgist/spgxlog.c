/*-------------------------------------------------------------------------
 *
 * spgxlog.c
 *	  WAL replay logic for SP-GiST
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			 src/backend/access/spgist/spgxlog.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/spgist_private.h"
#include "access/transam.h"
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
 * Add a leaf tuple, or replace an existing placeholder tuple.	This is used
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
spgRedoCreateIndex(XLogRecPtr lsn, XLogRecord *record)
{
	RelFileNode *node = (RelFileNode *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;

	buffer = XLogReadBuffer(*node, SPGIST_METAPAGE_BLKNO, true);
	Assert(BufferIsValid(buffer));
	page = (Page) BufferGetPage(buffer);
	SpGistInitMetapage(page);
	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);

	buffer = XLogReadBuffer(*node, SPGIST_ROOT_BLKNO, true);
	Assert(BufferIsValid(buffer));
	SpGistInitBuffer(buffer, SPGIST_LEAF);
	page = (Page) BufferGetPage(buffer);
	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);

	buffer = XLogReadBuffer(*node, SPGIST_NULL_BLKNO, true);
	Assert(BufferIsValid(buffer));
	SpGistInitBuffer(buffer, SPGIST_LEAF | SPGIST_NULLS);
	page = (Page) BufferGetPage(buffer);
	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
}

static void
spgRedoAddLeaf(XLogRecPtr lsn, XLogRecord *record)
{
	char	   *ptr = XLogRecGetData(record);
	spgxlogAddLeaf *xldata = (spgxlogAddLeaf *) ptr;
	SpGistLeafTuple leafTuple;
	Buffer		buffer;
	Page		page;

	/* we assume this is adequately aligned */
	ptr += sizeof(spgxlogAddLeaf);
	leafTuple = (SpGistLeafTuple) ptr;

	if (!(record->xl_info & XLR_BKP_BLOCK_1))
	{
		buffer = XLogReadBuffer(xldata->node, xldata->blknoLeaf,
								xldata->newPage);
		if (BufferIsValid(buffer))
		{
			page = BufferGetPage(buffer);

			if (xldata->newPage)
				SpGistInitBuffer(buffer,
					 SPGIST_LEAF | (xldata->storesNulls ? SPGIST_NULLS : 0));

			if (!XLByteLE(lsn, PageGetLSN(page)))
			{
				/* insert new tuple */
				if (xldata->offnumLeaf != xldata->offnumHeadLeaf)
				{
					/* normal cases, tuple was added by SpGistPageAddNewItem */
					addOrReplaceTuple(page, (Item) leafTuple, leafTuple->size,
									  xldata->offnumLeaf);

					/* update head tuple's chain link if needed */
					if (xldata->offnumHeadLeaf != InvalidOffsetNumber)
					{
						SpGistLeafTuple head;

						head = (SpGistLeafTuple) PageGetItem(page,
								PageGetItemId(page, xldata->offnumHeadLeaf));
						Assert(head->nextOffset == leafTuple->nextOffset);
						head->nextOffset = xldata->offnumLeaf;
					}
				}
				else
				{
					/* replacing a DEAD tuple */
					PageIndexTupleDelete(page, xldata->offnumLeaf);
					if (PageAddItem(page,
									(Item) leafTuple, leafTuple->size,
					 xldata->offnumLeaf, false, false) != xldata->offnumLeaf)
						elog(ERROR, "failed to add item of size %u to SPGiST index page",
							 leafTuple->size);
				}

				PageSetLSN(page, lsn);
				PageSetTLI(page, ThisTimeLineID);
				MarkBufferDirty(buffer);
			}
			UnlockReleaseBuffer(buffer);
		}
	}

	/* update parent downlink if necessary */
	if (xldata->blknoParent != InvalidBlockNumber &&
		!(record->xl_info & XLR_BKP_BLOCK_2))
	{
		buffer = XLogReadBuffer(xldata->node, xldata->blknoParent, false);
		if (BufferIsValid(buffer))
		{
			page = BufferGetPage(buffer);
			if (!XLByteLE(lsn, PageGetLSN(page)))
			{
				SpGistInnerTuple tuple;

				tuple = (SpGistInnerTuple) PageGetItem(page,
								  PageGetItemId(page, xldata->offnumParent));

				spgUpdateNodeLink(tuple, xldata->nodeI,
								  xldata->blknoLeaf, xldata->offnumLeaf);

				PageSetLSN(page, lsn);
				PageSetTLI(page, ThisTimeLineID);
				MarkBufferDirty(buffer);
			}
			UnlockReleaseBuffer(buffer);
		}
	}
}

static void
spgRedoMoveLeafs(XLogRecPtr lsn, XLogRecord *record)
{
	char	   *ptr = XLogRecGetData(record);
	spgxlogMoveLeafs *xldata = (spgxlogMoveLeafs *) ptr;
	SpGistState state;
	OffsetNumber *toDelete;
	OffsetNumber *toInsert;
	int			nInsert;
	Buffer		buffer;
	Page		page;

	fillFakeState(&state, xldata->stateSrc);

	nInsert = xldata->replaceDead ? 1 : xldata->nMoves + 1;

	ptr += MAXALIGN(sizeof(spgxlogMoveLeafs));
	toDelete = (OffsetNumber *) ptr;
	ptr += MAXALIGN(sizeof(OffsetNumber) * xldata->nMoves);
	toInsert = (OffsetNumber *) ptr;
	ptr += MAXALIGN(sizeof(OffsetNumber) * nInsert);

	/* now ptr points to the list of leaf tuples */

	/* Insert tuples on the dest page (do first, so redirect is valid) */
	if (!(record->xl_info & XLR_BKP_BLOCK_2))
	{
		buffer = XLogReadBuffer(xldata->node, xldata->blknoDst,
								xldata->newPage);
		if (BufferIsValid(buffer))
		{
			page = BufferGetPage(buffer);

			if (xldata->newPage)
				SpGistInitBuffer(buffer,
					 SPGIST_LEAF | (xldata->storesNulls ? SPGIST_NULLS : 0));

			if (!XLByteLE(lsn, PageGetLSN(page)))
			{
				int			i;

				for (i = 0; i < nInsert; i++)
				{
					SpGistLeafTuple lt = (SpGistLeafTuple) ptr;

					addOrReplaceTuple(page, (Item) lt, lt->size, toInsert[i]);
					ptr += lt->size;
				}

				PageSetLSN(page, lsn);
				PageSetTLI(page, ThisTimeLineID);
				MarkBufferDirty(buffer);
			}
			UnlockReleaseBuffer(buffer);
		}
	}

	/* Delete tuples from the source page, inserting a redirection pointer */
	if (!(record->xl_info & XLR_BKP_BLOCK_1))
	{
		buffer = XLogReadBuffer(xldata->node, xldata->blknoSrc, false);
		if (BufferIsValid(buffer))
		{
			page = BufferGetPage(buffer);
			if (!XLByteLE(lsn, PageGetLSN(page)))
			{
				spgPageIndexMultiDelete(&state, page, toDelete, xldata->nMoves,
						state.isBuild ? SPGIST_PLACEHOLDER : SPGIST_REDIRECT,
										SPGIST_PLACEHOLDER,
										xldata->blknoDst,
										toInsert[nInsert - 1]);

				PageSetLSN(page, lsn);
				PageSetTLI(page, ThisTimeLineID);
				MarkBufferDirty(buffer);
			}
			UnlockReleaseBuffer(buffer);
		}
	}

	/* And update the parent downlink */
	if (!(record->xl_info & XLR_BKP_BLOCK_3))
	{
		buffer = XLogReadBuffer(xldata->node, xldata->blknoParent, false);
		if (BufferIsValid(buffer))
		{
			page = BufferGetPage(buffer);
			if (!XLByteLE(lsn, PageGetLSN(page)))
			{
				SpGistInnerTuple tuple;

				tuple = (SpGistInnerTuple) PageGetItem(page,
								  PageGetItemId(page, xldata->offnumParent));

				spgUpdateNodeLink(tuple, xldata->nodeI,
								  xldata->blknoDst, toInsert[nInsert - 1]);

				PageSetLSN(page, lsn);
				PageSetTLI(page, ThisTimeLineID);
				MarkBufferDirty(buffer);
			}
			UnlockReleaseBuffer(buffer);
		}
	}
}

static void
spgRedoAddNode(XLogRecPtr lsn, XLogRecord *record)
{
	char	   *ptr = XLogRecGetData(record);
	spgxlogAddNode *xldata = (spgxlogAddNode *) ptr;
	SpGistInnerTuple innerTuple;
	SpGistState state;
	Buffer		buffer;
	Page		page;
	int			bbi;

	/* we assume this is adequately aligned */
	ptr += sizeof(spgxlogAddNode);
	innerTuple = (SpGistInnerTuple) ptr;

	fillFakeState(&state, xldata->stateSrc);

	if (xldata->blknoNew == InvalidBlockNumber)
	{
		/* update in place */
		Assert(xldata->blknoParent == InvalidBlockNumber);
		if (!(record->xl_info & XLR_BKP_BLOCK_1))
		{
			buffer = XLogReadBuffer(xldata->node, xldata->blkno, false);
			if (BufferIsValid(buffer))
			{
				page = BufferGetPage(buffer);
				if (!XLByteLE(lsn, PageGetLSN(page)))
				{
					PageIndexTupleDelete(page, xldata->offnum);
					if (PageAddItem(page, (Item) innerTuple, innerTuple->size,
									xldata->offnum,
									false, false) != xldata->offnum)
						elog(ERROR, "failed to add item of size %u to SPGiST index page",
							 innerTuple->size);

					PageSetLSN(page, lsn);
					PageSetTLI(page, ThisTimeLineID);
					MarkBufferDirty(buffer);
				}
				UnlockReleaseBuffer(buffer);
			}
		}
	}
	else
	{
		/* Install new tuple first so redirect is valid */
		if (!(record->xl_info & XLR_BKP_BLOCK_2))
		{
			buffer = XLogReadBuffer(xldata->node, xldata->blknoNew,
									xldata->newPage);
			if (BufferIsValid(buffer))
			{
				page = BufferGetPage(buffer);

				/* AddNode is not used for nulls pages */
				if (xldata->newPage)
					SpGistInitBuffer(buffer, 0);

				if (!XLByteLE(lsn, PageGetLSN(page)))
				{
					addOrReplaceTuple(page, (Item) innerTuple,
									  innerTuple->size, xldata->offnumNew);

					PageSetLSN(page, lsn);
					PageSetTLI(page, ThisTimeLineID);
					MarkBufferDirty(buffer);
				}
				UnlockReleaseBuffer(buffer);
			}
		}

		/* Delete old tuple, replacing it with redirect or placeholder tuple */
		if (!(record->xl_info & XLR_BKP_BLOCK_1))
		{
			buffer = XLogReadBuffer(xldata->node, xldata->blkno, false);
			if (BufferIsValid(buffer))
			{
				page = BufferGetPage(buffer);
				if (!XLByteLE(lsn, PageGetLSN(page)))
				{
					SpGistDeadTuple dt;

					if (state.isBuild)
						dt = spgFormDeadTuple(&state, SPGIST_PLACEHOLDER,
											  InvalidBlockNumber,
											  InvalidOffsetNumber);
					else
						dt = spgFormDeadTuple(&state, SPGIST_REDIRECT,
											  xldata->blknoNew,
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

					PageSetLSN(page, lsn);
					PageSetTLI(page, ThisTimeLineID);
					MarkBufferDirty(buffer);
				}
				UnlockReleaseBuffer(buffer);
			}
		}

		/*
		 * Update parent downlink.	Since parent could be in either of the
		 * previous two buffers, it's a bit tricky to determine which BKP bit
		 * applies.
		 */
		if (xldata->blknoParent == xldata->blkno)
			bbi = 0;
		else if (xldata->blknoParent == xldata->blknoNew)
			bbi = 1;
		else
			bbi = 2;

		if (!(record->xl_info & XLR_SET_BKP_BLOCK(bbi)))
		{
			buffer = XLogReadBuffer(xldata->node, xldata->blknoParent, false);
			if (BufferIsValid(buffer))
			{
				page = BufferGetPage(buffer);
				if (!XLByteLE(lsn, PageGetLSN(page)))
				{
					SpGistInnerTuple innerTuple;

					innerTuple = (SpGistInnerTuple) PageGetItem(page,
								  PageGetItemId(page, xldata->offnumParent));

					spgUpdateNodeLink(innerTuple, xldata->nodeI,
									  xldata->blknoNew, xldata->offnumNew);

					PageSetLSN(page, lsn);
					PageSetTLI(page, ThisTimeLineID);
					MarkBufferDirty(buffer);
				}
				UnlockReleaseBuffer(buffer);
			}
		}
	}
}

static void
spgRedoSplitTuple(XLogRecPtr lsn, XLogRecord *record)
{
	char	   *ptr = XLogRecGetData(record);
	spgxlogSplitTuple *xldata = (spgxlogSplitTuple *) ptr;
	SpGistInnerTuple prefixTuple;
	SpGistInnerTuple postfixTuple;
	Buffer		buffer;
	Page		page;

	/* we assume this is adequately aligned */
	ptr += sizeof(spgxlogSplitTuple);
	prefixTuple = (SpGistInnerTuple) ptr;
	ptr += prefixTuple->size;
	postfixTuple = (SpGistInnerTuple) ptr;

	/* insert postfix tuple first to avoid dangling link */
	if (xldata->blknoPostfix != xldata->blknoPrefix &&
		!(record->xl_info & XLR_BKP_BLOCK_2))
	{
		buffer = XLogReadBuffer(xldata->node, xldata->blknoPostfix,
								xldata->newPage);
		if (BufferIsValid(buffer))
		{
			page = BufferGetPage(buffer);

			/* SplitTuple is not used for nulls pages */
			if (xldata->newPage)
				SpGistInitBuffer(buffer, 0);

			if (!XLByteLE(lsn, PageGetLSN(page)))
			{
				addOrReplaceTuple(page, (Item) postfixTuple,
								  postfixTuple->size, xldata->offnumPostfix);

				PageSetLSN(page, lsn);
				PageSetTLI(page, ThisTimeLineID);
				MarkBufferDirty(buffer);
			}
			UnlockReleaseBuffer(buffer);
		}
	}

	/* now handle the original page */
	if (!(record->xl_info & XLR_BKP_BLOCK_1))
	{
		buffer = XLogReadBuffer(xldata->node, xldata->blknoPrefix, false);
		if (BufferIsValid(buffer))
		{
			page = BufferGetPage(buffer);
			if (!XLByteLE(lsn, PageGetLSN(page)))
			{
				PageIndexTupleDelete(page, xldata->offnumPrefix);
				if (PageAddItem(page, (Item) prefixTuple, prefixTuple->size,
				 xldata->offnumPrefix, false, false) != xldata->offnumPrefix)
					elog(ERROR, "failed to add item of size %u to SPGiST index page",
						 prefixTuple->size);

				if (xldata->blknoPostfix == xldata->blknoPrefix)
					addOrReplaceTuple(page, (Item) postfixTuple,
									  postfixTuple->size,
									  xldata->offnumPostfix);

				PageSetLSN(page, lsn);
				PageSetTLI(page, ThisTimeLineID);
				MarkBufferDirty(buffer);
			}
			UnlockReleaseBuffer(buffer);
		}
	}
}

static void
spgRedoPickSplit(XLogRecPtr lsn, XLogRecord *record)
{
	char	   *ptr = XLogRecGetData(record);
	spgxlogPickSplit *xldata = (spgxlogPickSplit *) ptr;
	SpGistInnerTuple innerTuple;
	SpGistState state;
	OffsetNumber *toDelete;
	OffsetNumber *toInsert;
	uint8	   *leafPageSelect;
	Buffer		srcBuffer;
	Buffer		destBuffer;
	Page		page;
	int			bbi;
	int			i;

	fillFakeState(&state, xldata->stateSrc);

	ptr += MAXALIGN(sizeof(spgxlogPickSplit));
	innerTuple = (SpGistInnerTuple) ptr;
	ptr += innerTuple->size;
	toDelete = (OffsetNumber *) ptr;
	ptr += MAXALIGN(sizeof(OffsetNumber) * xldata->nDelete);
	toInsert = (OffsetNumber *) ptr;
	ptr += MAXALIGN(sizeof(OffsetNumber) * xldata->nInsert);
	leafPageSelect = (uint8 *) ptr;
	ptr += MAXALIGN(sizeof(uint8) * xldata->nInsert);

	/* now ptr points to the list of leaf tuples */

	/*
	 * It's a bit tricky to identify which pages have been handled as
	 * full-page images, so we explicitly count each referenced buffer.
	 */
	bbi = 0;

	if (SpGistBlockIsRoot(xldata->blknoSrc))
	{
		/* when splitting root, we touch it only in the guise of new inner */
		srcBuffer = InvalidBuffer;
	}
	else if (xldata->initSrc)
	{
		/* just re-init the source page */
		srcBuffer = XLogReadBuffer(xldata->node, xldata->blknoSrc, true);
		Assert(BufferIsValid(srcBuffer));
		page = (Page) BufferGetPage(srcBuffer);

		SpGistInitBuffer(srcBuffer,
					 SPGIST_LEAF | (xldata->storesNulls ? SPGIST_NULLS : 0));
		/* don't update LSN etc till we're done with it */
	}
	else
	{
		/* delete the specified tuples from source page */
		if (!(record->xl_info & XLR_SET_BKP_BLOCK(bbi)))
		{
			srcBuffer = XLogReadBuffer(xldata->node, xldata->blknoSrc, false);
			if (BufferIsValid(srcBuffer))
			{
				page = BufferGetPage(srcBuffer);
				if (!XLByteLE(lsn, PageGetLSN(page)))
				{
					/*
					 * We have it a bit easier here than in doPickSplit(),
					 * because we know the inner tuple's location already, so
					 * we can inject the correct redirection tuple now.
					 */
					if (!state.isBuild)
						spgPageIndexMultiDelete(&state, page,
												toDelete, xldata->nDelete,
												SPGIST_REDIRECT,
												SPGIST_PLACEHOLDER,
												xldata->blknoInner,
												xldata->offnumInner);
					else
						spgPageIndexMultiDelete(&state, page,
												toDelete, xldata->nDelete,
												SPGIST_PLACEHOLDER,
												SPGIST_PLACEHOLDER,
												InvalidBlockNumber,
												InvalidOffsetNumber);

					/* don't update LSN etc till we're done with it */
				}
			}
		}
		else
			srcBuffer = InvalidBuffer;
		bbi++;
	}

	/* try to access dest page if any */
	if (xldata->blknoDest == InvalidBlockNumber)
	{
		destBuffer = InvalidBuffer;
	}
	else if (xldata->initDest)
	{
		/* just re-init the dest page */
		destBuffer = XLogReadBuffer(xldata->node, xldata->blknoDest, true);
		Assert(BufferIsValid(destBuffer));
		page = (Page) BufferGetPage(destBuffer);

		SpGistInitBuffer(destBuffer,
					 SPGIST_LEAF | (xldata->storesNulls ? SPGIST_NULLS : 0));
		/* don't update LSN etc till we're done with it */
	}
	else
	{
		if (!(record->xl_info & XLR_SET_BKP_BLOCK(bbi)))
			destBuffer = XLogReadBuffer(xldata->node, xldata->blknoDest, false);
		else
			destBuffer = InvalidBuffer;
		bbi++;
	}

	/* restore leaf tuples to src and/or dest page */
	for (i = 0; i < xldata->nInsert; i++)
	{
		SpGistLeafTuple lt = (SpGistLeafTuple) ptr;
		Buffer		leafBuffer;

		ptr += lt->size;

		leafBuffer = leafPageSelect[i] ? destBuffer : srcBuffer;
		if (!BufferIsValid(leafBuffer))
			continue;			/* no need to touch this page */
		page = BufferGetPage(leafBuffer);

		if (!XLByteLE(lsn, PageGetLSN(page)))
		{
			addOrReplaceTuple(page, (Item) lt, lt->size, toInsert[i]);
		}
	}

	/* Now update src and dest page LSNs */
	if (BufferIsValid(srcBuffer))
	{
		page = BufferGetPage(srcBuffer);
		if (!XLByteLE(lsn, PageGetLSN(page)))
		{
			PageSetLSN(page, lsn);
			PageSetTLI(page, ThisTimeLineID);
			MarkBufferDirty(srcBuffer);
		}
		UnlockReleaseBuffer(srcBuffer);
	}
	if (BufferIsValid(destBuffer))
	{
		page = BufferGetPage(destBuffer);
		if (!XLByteLE(lsn, PageGetLSN(page)))
		{
			PageSetLSN(page, lsn);
			PageSetTLI(page, ThisTimeLineID);
			MarkBufferDirty(destBuffer);
		}
		UnlockReleaseBuffer(destBuffer);
	}

	/* restore new inner tuple */
	if (!(record->xl_info & XLR_SET_BKP_BLOCK(bbi)))
	{
		Buffer		buffer = XLogReadBuffer(xldata->node, xldata->blknoInner,
											xldata->initInner);

		if (BufferIsValid(buffer))
		{
			page = BufferGetPage(buffer);

			if (xldata->initInner)
				SpGistInitBuffer(buffer,
								 (xldata->storesNulls ? SPGIST_NULLS : 0));

			if (!XLByteLE(lsn, PageGetLSN(page)))
			{
				addOrReplaceTuple(page, (Item) innerTuple, innerTuple->size,
								  xldata->offnumInner);

				/* if inner is also parent, update link while we're here */
				if (xldata->blknoInner == xldata->blknoParent)
				{
					SpGistInnerTuple parent;

					parent = (SpGistInnerTuple) PageGetItem(page,
								  PageGetItemId(page, xldata->offnumParent));
					spgUpdateNodeLink(parent, xldata->nodeI,
									xldata->blknoInner, xldata->offnumInner);
				}

				PageSetLSN(page, lsn);
				PageSetTLI(page, ThisTimeLineID);
				MarkBufferDirty(buffer);
			}
			UnlockReleaseBuffer(buffer);
		}
	}
	bbi++;

	/* update parent downlink, unless we did it above */
	if (xldata->blknoParent == InvalidBlockNumber)
	{
		/* no parent cause we split the root */
		Assert(SpGistBlockIsRoot(xldata->blknoInner));
	}
	else if (xldata->blknoInner != xldata->blknoParent)
	{
		if (!(record->xl_info & XLR_SET_BKP_BLOCK(bbi)))
		{
			Buffer		buffer = XLogReadBuffer(xldata->node, xldata->blknoParent, false);

			if (BufferIsValid(buffer))
			{
				page = BufferGetPage(buffer);

				if (!XLByteLE(lsn, PageGetLSN(page)))
				{
					SpGistInnerTuple parent;

					parent = (SpGistInnerTuple) PageGetItem(page,
								  PageGetItemId(page, xldata->offnumParent));
					spgUpdateNodeLink(parent, xldata->nodeI,
									xldata->blknoInner, xldata->offnumInner);

					PageSetLSN(page, lsn);
					PageSetTLI(page, ThisTimeLineID);
					MarkBufferDirty(buffer);
				}
				UnlockReleaseBuffer(buffer);
			}
		}
	}
}

static void
spgRedoVacuumLeaf(XLogRecPtr lsn, XLogRecord *record)
{
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

	ptr += sizeof(spgxlogVacuumLeaf);
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

	if (!(record->xl_info & XLR_BKP_BLOCK_1))
	{
		buffer = XLogReadBuffer(xldata->node, xldata->blkno, false);
		if (BufferIsValid(buffer))
		{
			page = BufferGetPage(buffer);
			if (!XLByteLE(lsn, PageGetLSN(page)))
			{
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
				PageSetTLI(page, ThisTimeLineID);
				MarkBufferDirty(buffer);
			}
			UnlockReleaseBuffer(buffer);
		}
	}
}

static void
spgRedoVacuumRoot(XLogRecPtr lsn, XLogRecord *record)
{
	char	   *ptr = XLogRecGetData(record);
	spgxlogVacuumRoot *xldata = (spgxlogVacuumRoot *) ptr;
	OffsetNumber *toDelete;
	Buffer		buffer;
	Page		page;

	ptr += sizeof(spgxlogVacuumRoot);
	toDelete = (OffsetNumber *) ptr;

	if (!(record->xl_info & XLR_BKP_BLOCK_1))
	{
		buffer = XLogReadBuffer(xldata->node, xldata->blkno, false);
		if (BufferIsValid(buffer))
		{
			page = BufferGetPage(buffer);
			if (!XLByteLE(lsn, PageGetLSN(page)))
			{
				/* The tuple numbers are in order */
				PageIndexMultiDelete(page, toDelete, xldata->nDelete);

				PageSetLSN(page, lsn);
				PageSetTLI(page, ThisTimeLineID);
				MarkBufferDirty(buffer);
			}
			UnlockReleaseBuffer(buffer);
		}
	}
}

static void
spgRedoVacuumRedirect(XLogRecPtr lsn, XLogRecord *record)
{
	char	   *ptr = XLogRecGetData(record);
	spgxlogVacuumRedirect *xldata = (spgxlogVacuumRedirect *) ptr;
	OffsetNumber *itemToPlaceholder;
	Buffer		buffer;
	Page		page;

	ptr += sizeof(spgxlogVacuumRedirect);
	itemToPlaceholder = (OffsetNumber *) ptr;

	if (!(record->xl_info & XLR_BKP_BLOCK_1))
	{
		buffer = XLogReadBuffer(xldata->node, xldata->blkno, false);

		if (BufferIsValid(buffer))
		{
			page = BufferGetPage(buffer);
			if (!XLByteLE(lsn, PageGetLSN(page)))
			{
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
				PageSetTLI(page, ThisTimeLineID);
				MarkBufferDirty(buffer);
			}

			UnlockReleaseBuffer(buffer);
		}
	}
}

void
spg_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;
	MemoryContext oldCxt;

	/*
	 * If we have any conflict processing to do, it must happen before we
	 * update the page.
	 */
	if (InHotStandby)
	{
		switch (info)
		{
			case XLOG_SPGIST_VACUUM_REDIRECT:
				{
					spgxlogVacuumRedirect *xldata =
					(spgxlogVacuumRedirect *) XLogRecGetData(record);

					/*
					 * If any redirection tuples are being removed, make sure
					 * there are no live Hot Standby transactions that might
					 * need to see them.
					 */
					if (TransactionIdIsValid(xldata->newestRedirectXid))
						ResolveRecoveryConflictWithSnapshot(xldata->newestRedirectXid,
															xldata->node);
					break;
				}
			default:
				break;
		}
	}

	RestoreBkpBlocks(lsn, record, false);

	oldCxt = MemoryContextSwitchTo(opCtx);
	switch (info)
	{
		case XLOG_SPGIST_CREATE_INDEX:
			spgRedoCreateIndex(lsn, record);
			break;
		case XLOG_SPGIST_ADD_LEAF:
			spgRedoAddLeaf(lsn, record);
			break;
		case XLOG_SPGIST_MOVE_LEAFS:
			spgRedoMoveLeafs(lsn, record);
			break;
		case XLOG_SPGIST_ADD_NODE:
			spgRedoAddNode(lsn, record);
			break;
		case XLOG_SPGIST_SPLIT_TUPLE:
			spgRedoSplitTuple(lsn, record);
			break;
		case XLOG_SPGIST_PICKSPLIT:
			spgRedoPickSplit(lsn, record);
			break;
		case XLOG_SPGIST_VACUUM_LEAF:
			spgRedoVacuumLeaf(lsn, record);
			break;
		case XLOG_SPGIST_VACUUM_ROOT:
			spgRedoVacuumRoot(lsn, record);
			break;
		case XLOG_SPGIST_VACUUM_REDIRECT:
			spgRedoVacuumRedirect(lsn, record);
			break;
		default:
			elog(PANIC, "spg_redo: unknown op code %u", info);
	}

	MemoryContextSwitchTo(oldCxt);
	MemoryContextReset(opCtx);
}

static void
out_target(StringInfo buf, RelFileNode node)
{
	appendStringInfo(buf, "rel %u/%u/%u ",
					 node.spcNode, node.dbNode, node.relNode);
}

void
spg_desc(StringInfo buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_SPGIST_CREATE_INDEX:
			appendStringInfo(buf, "create_index: rel %u/%u/%u",
							 ((RelFileNode *) rec)->spcNode,
							 ((RelFileNode *) rec)->dbNode,
							 ((RelFileNode *) rec)->relNode);
			break;
		case XLOG_SPGIST_ADD_LEAF:
			out_target(buf, ((spgxlogAddLeaf *) rec)->node);
			appendStringInfo(buf, "add leaf to page: %u",
							 ((spgxlogAddLeaf *) rec)->blknoLeaf);
			break;
		case XLOG_SPGIST_MOVE_LEAFS:
			out_target(buf, ((spgxlogMoveLeafs *) rec)->node);
			appendStringInfo(buf, "move %u leafs from page %u to page %u",
							 ((spgxlogMoveLeafs *) rec)->nMoves,
							 ((spgxlogMoveLeafs *) rec)->blknoSrc,
							 ((spgxlogMoveLeafs *) rec)->blknoDst);
			break;
		case XLOG_SPGIST_ADD_NODE:
			out_target(buf, ((spgxlogAddNode *) rec)->node);
			appendStringInfo(buf, "add node to %u:%u",
							 ((spgxlogAddNode *) rec)->blkno,
							 ((spgxlogAddNode *) rec)->offnum);
			break;
		case XLOG_SPGIST_SPLIT_TUPLE:
			out_target(buf, ((spgxlogSplitTuple *) rec)->node);
			appendStringInfo(buf, "split node %u:%u to %u:%u",
							 ((spgxlogSplitTuple *) rec)->blknoPrefix,
							 ((spgxlogSplitTuple *) rec)->offnumPrefix,
							 ((spgxlogSplitTuple *) rec)->blknoPostfix,
							 ((spgxlogSplitTuple *) rec)->offnumPostfix);
			break;
		case XLOG_SPGIST_PICKSPLIT:
			out_target(buf, ((spgxlogPickSplit *) rec)->node);
			appendStringInfo(buf, "split leaf page");
			break;
		case XLOG_SPGIST_VACUUM_LEAF:
			out_target(buf, ((spgxlogVacuumLeaf *) rec)->node);
			appendStringInfo(buf, "vacuum leaf tuples on page %u",
							 ((spgxlogVacuumLeaf *) rec)->blkno);
			break;
		case XLOG_SPGIST_VACUUM_ROOT:
			out_target(buf, ((spgxlogVacuumRoot *) rec)->node);
			appendStringInfo(buf, "vacuum leaf tuples on root page %u",
							 ((spgxlogVacuumRoot *) rec)->blkno);
			break;
		case XLOG_SPGIST_VACUUM_REDIRECT:
			out_target(buf, ((spgxlogVacuumRedirect *) rec)->node);
			appendStringInfo(buf, "vacuum redirect tuples on page %u, newest XID %u",
							 ((spgxlogVacuumRedirect *) rec)->blkno,
						 ((spgxlogVacuumRedirect *) rec)->newestRedirectXid);
			break;
		default:
			appendStringInfo(buf, "unknown spgist op code %u", info);
			break;
	}
}

void
spg_xlog_startup(void)
{
	opCtx = AllocSetContextCreate(CurrentMemoryContext,
								  "SP-GiST temporary context",
								  ALLOCSET_DEFAULT_MINSIZE,
								  ALLOCSET_DEFAULT_INITSIZE,
								  ALLOCSET_DEFAULT_MAXSIZE);
}

void
spg_xlog_cleanup(void)
{
	MemoryContextDelete(opCtx);
	opCtx = NULL;
}
