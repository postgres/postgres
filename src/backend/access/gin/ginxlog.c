/*-------------------------------------------------------------------------
 *
 * ginxlog.c
 *	  WAL replay logic for inverted index.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			 $PostgreSQL: pgsql/src/backend/access/gin/ginxlog.c,v 1.12 2008/01/01 19:45:46 momjian Exp $
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gin.h"
#include "access/heapam.h"
#include "utils/memutils.h"

static MemoryContext opCtx;		/* working memory for operations */
static MemoryContext topCtx;

typedef struct ginIncompleteSplit
{
	RelFileNode node;
	BlockNumber leftBlkno;
	BlockNumber rightBlkno;
	BlockNumber rootBlkno;
} ginIncompleteSplit;

static List *incomplete_splits;

static void
pushIncompleteSplit(RelFileNode node, BlockNumber leftBlkno, BlockNumber rightBlkno, BlockNumber rootBlkno)
{
	ginIncompleteSplit *split;

	MemoryContextSwitchTo(topCtx);

	split = palloc(sizeof(ginIncompleteSplit));

	split->node = node;
	split->leftBlkno = leftBlkno;
	split->rightBlkno = rightBlkno;
	split->rootBlkno = rootBlkno;

	incomplete_splits = lappend(incomplete_splits, split);

	MemoryContextSwitchTo(opCtx);
}

static void
forgetIncompleteSplit(RelFileNode node, BlockNumber leftBlkno, BlockNumber updateBlkno)
{
	ListCell   *l;

	foreach(l, incomplete_splits)
	{
		ginIncompleteSplit *split = (ginIncompleteSplit *) lfirst(l);

		if (RelFileNodeEquals(node, split->node) && leftBlkno == split->leftBlkno && updateBlkno == split->rightBlkno)
		{
			incomplete_splits = list_delete_ptr(incomplete_splits, split);
			break;
		}
	}
}

static void
ginRedoCreateIndex(XLogRecPtr lsn, XLogRecord *record)
{
	RelFileNode *node = (RelFileNode *) XLogRecGetData(record);
	Relation	reln;
	Buffer		buffer;
	Page		page;

	reln = XLogOpenRelation(*node);
	buffer = XLogReadBuffer(reln, GIN_ROOT_BLKNO, true);
	Assert(BufferIsValid(buffer));
	page = (Page) BufferGetPage(buffer);

	GinInitBuffer(buffer, GIN_LEAF);

	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);

	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
}

static void
ginRedoCreatePTree(XLogRecPtr lsn, XLogRecord *record)
{
	ginxlogCreatePostingTree *data = (ginxlogCreatePostingTree *) XLogRecGetData(record);
	ItemPointerData *items = (ItemPointerData *) (XLogRecGetData(record) + sizeof(ginxlogCreatePostingTree));
	Relation	reln;
	Buffer		buffer;
	Page		page;

	reln = XLogOpenRelation(data->node);
	buffer = XLogReadBuffer(reln, data->blkno, true);
	Assert(BufferIsValid(buffer));
	page = (Page) BufferGetPage(buffer);

	GinInitBuffer(buffer, GIN_DATA | GIN_LEAF);
	memcpy(GinDataPageGetData(page), items, sizeof(ItemPointerData) * data->nitem);
	GinPageGetOpaque(page)->maxoff = data->nitem;

	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);

	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
}

static void
ginRedoInsert(XLogRecPtr lsn, XLogRecord *record)
{
	ginxlogInsert *data = (ginxlogInsert *) XLogRecGetData(record);
	Relation	reln;
	Buffer		buffer;
	Page		page;

	/* nothing else to do if page was backed up */
	if (record->xl_info & XLR_BKP_BLOCK_1)
		return;

	reln = XLogOpenRelation(data->node);
	buffer = XLogReadBuffer(reln, data->blkno, false);
	Assert(BufferIsValid(buffer));
	page = (Page) BufferGetPage(buffer);

	if (data->isData)
	{
		Assert(data->isDelete == FALSE);
		Assert(GinPageIsData(page));

		if (!XLByteLE(lsn, PageGetLSN(page)))
		{
			if (data->isLeaf)
			{
				OffsetNumber i;
				ItemPointerData *items = (ItemPointerData *) (XLogRecGetData(record) + sizeof(ginxlogInsert));

				Assert(GinPageIsLeaf(page));
				Assert(data->updateBlkno == InvalidBlockNumber);

				for (i = 0; i < data->nitem; i++)
					GinDataPageAddItem(page, items + i, data->offset + i);
			}
			else
			{
				PostingItem *pitem;

				Assert(!GinPageIsLeaf(page));

				if (data->updateBlkno != InvalidBlockNumber)
				{
					/* update link to right page after split */
					pitem = (PostingItem *) GinDataPageGetItem(page, data->offset);
					PostingItemSetBlockNumber(pitem, data->updateBlkno);
				}

				pitem = (PostingItem *) (XLogRecGetData(record) + sizeof(ginxlogInsert));

				GinDataPageAddItem(page, pitem, data->offset);
			}
		}

		if (!data->isLeaf && data->updateBlkno != InvalidBlockNumber)
		{
			PostingItem *pitem = (PostingItem *) (XLogRecGetData(record) + sizeof(ginxlogInsert));

			forgetIncompleteSplit(data->node, PostingItemGetBlockNumber(pitem), data->updateBlkno);
		}

	}
	else
	{
		IndexTuple	itup;

		Assert(!GinPageIsData(page));

		if (!XLByteLE(lsn, PageGetLSN(page)))
		{
			if (data->updateBlkno != InvalidBlockNumber)
			{
				/* update link to right page after split */
				Assert(!GinPageIsLeaf(page));
				Assert(data->offset >= FirstOffsetNumber && data->offset <= PageGetMaxOffsetNumber(page));
				itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, data->offset));
				ItemPointerSet(&itup->t_tid, data->updateBlkno, InvalidOffsetNumber);
			}

			if (data->isDelete)
			{
				Assert(GinPageIsLeaf(page));
				Assert(data->offset >= FirstOffsetNumber && data->offset <= PageGetMaxOffsetNumber(page));
				PageIndexTupleDelete(page, data->offset);
			}

			itup = (IndexTuple) (XLogRecGetData(record) + sizeof(ginxlogInsert));

			if (PageAddItem(page, (Item) itup, IndexTupleSize(itup), data->offset, false, false) == InvalidOffsetNumber)
				elog(ERROR, "failed to add item to index page in %u/%u/%u",
				  data->node.spcNode, data->node.dbNode, data->node.relNode);
		}

		if (!data->isLeaf && data->updateBlkno != InvalidBlockNumber)
		{
			itup = (IndexTuple) (XLogRecGetData(record) + sizeof(ginxlogInsert));
			forgetIncompleteSplit(data->node, GinItemPointerGetBlockNumber(&itup->t_tid), data->updateBlkno);
		}
	}

	if (!XLByteLE(lsn, PageGetLSN(page)))
	{
		PageSetLSN(page, lsn);
		PageSetTLI(page, ThisTimeLineID);

		MarkBufferDirty(buffer);
	}
	UnlockReleaseBuffer(buffer);
}

static void
ginRedoSplit(XLogRecPtr lsn, XLogRecord *record)
{
	ginxlogSplit *data = (ginxlogSplit *) XLogRecGetData(record);
	Relation	reln;
	Buffer		lbuffer,
				rbuffer;
	Page		lpage,
				rpage;
	uint32		flags = 0;

	reln = XLogOpenRelation(data->node);

	if (data->isLeaf)
		flags |= GIN_LEAF;
	if (data->isData)
		flags |= GIN_DATA;

	lbuffer = XLogReadBuffer(reln, data->lblkno, data->isRootSplit);
	Assert(BufferIsValid(lbuffer));
	lpage = (Page) BufferGetPage(lbuffer);
	GinInitBuffer(lbuffer, flags);

	rbuffer = XLogReadBuffer(reln, data->rblkno, true);
	Assert(BufferIsValid(rbuffer));
	rpage = (Page) BufferGetPage(rbuffer);
	GinInitBuffer(rbuffer, flags);

	GinPageGetOpaque(lpage)->rightlink = BufferGetBlockNumber(rbuffer);
	GinPageGetOpaque(rpage)->rightlink = data->rrlink;

	if (data->isData)
	{
		char	   *ptr = XLogRecGetData(record) + sizeof(ginxlogSplit);
		Size		sizeofitem = GinSizeOfItem(lpage);
		OffsetNumber i;
		ItemPointer bound;

		for (i = 0; i < data->separator; i++)
		{
			GinDataPageAddItem(lpage, ptr, InvalidOffsetNumber);
			ptr += sizeofitem;
		}

		for (i = data->separator; i < data->nitem; i++)
		{
			GinDataPageAddItem(rpage, ptr, InvalidOffsetNumber);
			ptr += sizeofitem;
		}

		/* set up right key */
		bound = GinDataPageGetRightBound(lpage);
		if (data->isLeaf)
			*bound = *(ItemPointerData *) GinDataPageGetItem(lpage, GinPageGetOpaque(lpage)->maxoff);
		else
			*bound = ((PostingItem *) GinDataPageGetItem(lpage, GinPageGetOpaque(lpage)->maxoff))->key;

		bound = GinDataPageGetRightBound(rpage);
		*bound = data->rightbound;
	}
	else
	{
		IndexTuple	itup = (IndexTuple) (XLogRecGetData(record) + sizeof(ginxlogSplit));
		OffsetNumber i;

		for (i = 0; i < data->separator; i++)
		{
			if (PageAddItem(lpage, (Item) itup, IndexTupleSize(itup), InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
				elog(ERROR, "failed to add item to index page in %u/%u/%u",
				  data->node.spcNode, data->node.dbNode, data->node.relNode);
			itup = (IndexTuple) (((char *) itup) + MAXALIGN(IndexTupleSize(itup)));
		}

		for (i = data->separator; i < data->nitem; i++)
		{
			if (PageAddItem(rpage, (Item) itup, IndexTupleSize(itup), InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
				elog(ERROR, "failed to add item to index page in %u/%u/%u",
				  data->node.spcNode, data->node.dbNode, data->node.relNode);
			itup = (IndexTuple) (((char *) itup) + MAXALIGN(IndexTupleSize(itup)));
		}
	}

	PageSetLSN(rpage, lsn);
	PageSetTLI(rpage, ThisTimeLineID);
	MarkBufferDirty(rbuffer);

	PageSetLSN(lpage, lsn);
	PageSetTLI(lpage, ThisTimeLineID);
	MarkBufferDirty(lbuffer);

	if (!data->isLeaf && data->updateBlkno != InvalidBlockNumber)
		forgetIncompleteSplit(data->node, data->leftChildBlkno, data->updateBlkno);

	if (data->isRootSplit)
	{
		Buffer		rootBuf = XLogReadBuffer(reln, data->rootBlkno, false);
		Page		rootPage = BufferGetPage(rootBuf);

		GinInitBuffer(rootBuf, flags & ~GIN_LEAF);

		if (data->isData)
		{
			Assert(data->rootBlkno != GIN_ROOT_BLKNO);
			dataFillRoot(NULL, rootBuf, lbuffer, rbuffer);
		}
		else
		{
			Assert(data->rootBlkno == GIN_ROOT_BLKNO);
			entryFillRoot(NULL, rootBuf, lbuffer, rbuffer);
		}

		PageSetLSN(rootPage, lsn);
		PageSetTLI(rootPage, ThisTimeLineID);

		MarkBufferDirty(rootBuf);
		UnlockReleaseBuffer(rootBuf);
	}
	else
		pushIncompleteSplit(data->node, data->lblkno, data->rblkno, data->rootBlkno);

	UnlockReleaseBuffer(rbuffer);
	UnlockReleaseBuffer(lbuffer);
}

static void
ginRedoVacuumPage(XLogRecPtr lsn, XLogRecord *record)
{
	ginxlogVacuumPage *data = (ginxlogVacuumPage *) XLogRecGetData(record);
	Relation	reln;
	Buffer		buffer;
	Page		page;

	/* nothing else to do if page was backed up (and no info to do it with) */
	if (record->xl_info & XLR_BKP_BLOCK_1)
		return;

	reln = XLogOpenRelation(data->node);
	buffer = XLogReadBuffer(reln, data->blkno, false);
	Assert(BufferIsValid(buffer));
	page = (Page) BufferGetPage(buffer);

	if (GinPageIsData(page))
	{
		memcpy(GinDataPageGetData(page), XLogRecGetData(record) + sizeof(ginxlogVacuumPage),
			   GinSizeOfItem(page) *data->nitem);
		GinPageGetOpaque(page)->maxoff = data->nitem;
	}
	else
	{
		OffsetNumber i,
				   *tod;
		IndexTuple	itup = (IndexTuple) (XLogRecGetData(record) + sizeof(ginxlogVacuumPage));

		tod = (OffsetNumber *) palloc(sizeof(OffsetNumber) * PageGetMaxOffsetNumber(page));
		for (i = FirstOffsetNumber; i <= PageGetMaxOffsetNumber(page); i++)
			tod[i - 1] = i;

		PageIndexMultiDelete(page, tod, PageGetMaxOffsetNumber(page));

		for (i = 0; i < data->nitem; i++)
		{
			if (PageAddItem(page, (Item) itup, IndexTupleSize(itup), InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
				elog(ERROR, "failed to add item to index page in %u/%u/%u",
				  data->node.spcNode, data->node.dbNode, data->node.relNode);
			itup = (IndexTuple) (((char *) itup) + MAXALIGN(IndexTupleSize(itup)));
		}
	}

	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);

	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
}

static void
ginRedoDeletePage(XLogRecPtr lsn, XLogRecord *record)
{
	ginxlogDeletePage *data = (ginxlogDeletePage *) XLogRecGetData(record);
	Relation	reln;
	Buffer		buffer;
	Page		page;

	reln = XLogOpenRelation(data->node);

	if (!(record->xl_info & XLR_BKP_BLOCK_1))
	{
		buffer = XLogReadBuffer(reln, data->blkno, false);
		page = BufferGetPage(buffer);
		Assert(GinPageIsData(page));
		GinPageGetOpaque(page)->flags = GIN_DELETED;
		PageSetLSN(page, lsn);
		PageSetTLI(page, ThisTimeLineID);
		MarkBufferDirty(buffer);
		UnlockReleaseBuffer(buffer);
	}

	if (!(record->xl_info & XLR_BKP_BLOCK_2))
	{
		buffer = XLogReadBuffer(reln, data->parentBlkno, false);
		page = BufferGetPage(buffer);
		Assert(GinPageIsData(page));
		Assert(!GinPageIsLeaf(page));
		PageDeletePostingItem(page, data->parentOffset);
		PageSetLSN(page, lsn);
		PageSetTLI(page, ThisTimeLineID);
		MarkBufferDirty(buffer);
		UnlockReleaseBuffer(buffer);
	}

	if (!(record->xl_info & XLR_BKP_BLOCK_3) && data->leftBlkno != InvalidBlockNumber)
	{
		buffer = XLogReadBuffer(reln, data->leftBlkno, false);
		page = BufferGetPage(buffer);
		Assert(GinPageIsData(page));
		GinPageGetOpaque(page)->rightlink = data->rightLink;
		PageSetLSN(page, lsn);
		PageSetTLI(page, ThisTimeLineID);
		MarkBufferDirty(buffer);
		UnlockReleaseBuffer(buffer);
	}
}

void
gin_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	topCtx = MemoryContextSwitchTo(opCtx);
	switch (info)
	{
		case XLOG_GIN_CREATE_INDEX:
			ginRedoCreateIndex(lsn, record);
			break;
		case XLOG_GIN_CREATE_PTREE:
			ginRedoCreatePTree(lsn, record);
			break;
		case XLOG_GIN_INSERT:
			ginRedoInsert(lsn, record);
			break;
		case XLOG_GIN_SPLIT:
			ginRedoSplit(lsn, record);
			break;
		case XLOG_GIN_VACUUM_PAGE:
			ginRedoVacuumPage(lsn, record);
			break;
		case XLOG_GIN_DELETE_PAGE:
			ginRedoDeletePage(lsn, record);
			break;
		default:
			elog(PANIC, "gin_redo: unknown op code %u", info);
	}
	MemoryContextSwitchTo(topCtx);
	MemoryContextReset(opCtx);
}

static void
desc_node(StringInfo buf, RelFileNode node, BlockNumber blkno)
{
	appendStringInfo(buf, "node: %u/%u/%u blkno: %u",
					 node.spcNode, node.dbNode, node.relNode, blkno);
}

void
gin_desc(StringInfo buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_GIN_CREATE_INDEX:
			appendStringInfo(buf, "Create index, ");
			desc_node(buf, *(RelFileNode *) rec, GIN_ROOT_BLKNO);
			break;
		case XLOG_GIN_CREATE_PTREE:
			appendStringInfo(buf, "Create posting tree, ");
			desc_node(buf, ((ginxlogCreatePostingTree *) rec)->node, ((ginxlogCreatePostingTree *) rec)->blkno);
			break;
		case XLOG_GIN_INSERT:
			appendStringInfo(buf, "Insert item, ");
			desc_node(buf, ((ginxlogInsert *) rec)->node, ((ginxlogInsert *) rec)->blkno);
			appendStringInfo(buf, " offset: %u nitem: %u isdata: %c isleaf %c isdelete %c updateBlkno:%u",
							 ((ginxlogInsert *) rec)->offset,
							 ((ginxlogInsert *) rec)->nitem,
							 (((ginxlogInsert *) rec)->isData) ? 'T' : 'F',
							 (((ginxlogInsert *) rec)->isLeaf) ? 'T' : 'F',
							 (((ginxlogInsert *) rec)->isDelete) ? 'T' : 'F',
							 ((ginxlogInsert *) rec)->updateBlkno
				);

			break;
		case XLOG_GIN_SPLIT:
			appendStringInfo(buf, "Page split, ");
			desc_node(buf, ((ginxlogSplit *) rec)->node, ((ginxlogSplit *) rec)->lblkno);
			appendStringInfo(buf, " isrootsplit: %c", (((ginxlogSplit *) rec)->isRootSplit) ? 'T' : 'F');
			break;
		case XLOG_GIN_VACUUM_PAGE:
			appendStringInfo(buf, "Vacuum page, ");
			desc_node(buf, ((ginxlogVacuumPage *) rec)->node, ((ginxlogVacuumPage *) rec)->blkno);
			break;
		case XLOG_GIN_DELETE_PAGE:
			appendStringInfo(buf, "Delete page, ");
			desc_node(buf, ((ginxlogDeletePage *) rec)->node, ((ginxlogDeletePage *) rec)->blkno);
			break;
		default:
			elog(PANIC, "gin_desc: unknown op code %u", info);
	}
}

void
gin_xlog_startup(void)
{
	incomplete_splits = NIL;

	opCtx = AllocSetContextCreate(CurrentMemoryContext,
								  "GIN recovery temporary context",
								  ALLOCSET_DEFAULT_MINSIZE,
								  ALLOCSET_DEFAULT_INITSIZE,
								  ALLOCSET_DEFAULT_MAXSIZE);
}

static void
ginContinueSplit(ginIncompleteSplit *split)
{
	GinBtreeData btree;
	Relation	reln;
	Buffer		buffer;
	GinBtreeStack stack;

	/*
	 * elog(NOTICE,"ginContinueSplit root:%u l:%u r:%u",  split->rootBlkno,
	 * split->leftBlkno, split->rightBlkno);
	 */
	reln = XLogOpenRelation(split->node);

	buffer = XLogReadBuffer(reln, split->leftBlkno, false);

	if (split->rootBlkno == GIN_ROOT_BLKNO)
	{
		prepareEntryScan(&btree, reln, (Datum) 0, NULL);
		btree.entry = ginPageGetLinkItup(buffer);
	}
	else
	{
		Page		page = BufferGetPage(buffer);

		prepareDataScan(&btree, reln);

		PostingItemSetBlockNumber(&(btree.pitem), split->leftBlkno);
		if (GinPageIsLeaf(page))
			btree.pitem.key = *(ItemPointerData *) GinDataPageGetItem(page,
											 GinPageGetOpaque(page)->maxoff);
		else
			btree.pitem.key = ((PostingItem *) GinDataPageGetItem(page,
									   GinPageGetOpaque(page)->maxoff))->key;
	}

	btree.rightblkno = split->rightBlkno;

	stack.blkno = split->leftBlkno;
	stack.buffer = buffer;
	stack.off = InvalidOffsetNumber;
	stack.parent = NULL;

	findParents(&btree, &stack, split->rootBlkno);
	ginInsertValue(&btree, stack.parent);

	UnlockReleaseBuffer(buffer);
}

void
gin_xlog_cleanup(void)
{
	ListCell   *l;
	MemoryContext topCtx;

	topCtx = MemoryContextSwitchTo(opCtx);

	foreach(l, incomplete_splits)
	{
		ginIncompleteSplit *split = (ginIncompleteSplit *) lfirst(l);

		ginContinueSplit(split);
		MemoryContextReset(opCtx);
	}

	MemoryContextSwitchTo(topCtx);
	MemoryContextDelete(opCtx);
	incomplete_splits = NIL;
}

bool
gin_safe_restartpoint(void)
{
	if (incomplete_splits)
		return false;
	return true;
}
