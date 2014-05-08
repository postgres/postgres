/*-------------------------------------------------------------------------
 *
 * ginxlog.c
 *	  WAL replay logic for inverted index.
 *
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			 src/backend/access/gin/ginxlog.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gin_private.h"
#include "access/xlogutils.h"
#include "utils/memutils.h"

static MemoryContext opCtx;		/* working memory for operations */

static void
ginRedoClearIncompleteSplit(XLogRecPtr lsn, RelFileNode node, BlockNumber blkno)
{
	Buffer		buffer;
	Page		page;

	buffer = XLogReadBuffer(node, blkno, false);
	if (!BufferIsValid(buffer))
		return;					/* page was deleted, nothing to do */
	page = (Page) BufferGetPage(buffer);

	if (lsn > PageGetLSN(page))
	{
		GinPageGetOpaque(page)->flags &= ~GIN_INCOMPLETE_SPLIT;

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}

	UnlockReleaseBuffer(buffer);
}

static void
ginRedoCreateIndex(XLogRecPtr lsn, XLogRecord *record)
{
	RelFileNode *node = (RelFileNode *) XLogRecGetData(record);
	Buffer		RootBuffer,
				MetaBuffer;
	Page		page;

	/* Backup blocks are not used in create_index records */
	Assert(!(record->xl_info & XLR_BKP_BLOCK_MASK));

	MetaBuffer = XLogReadBuffer(*node, GIN_METAPAGE_BLKNO, true);
	Assert(BufferIsValid(MetaBuffer));
	page = (Page) BufferGetPage(MetaBuffer);

	GinInitMetabuffer(MetaBuffer);

	PageSetLSN(page, lsn);
	MarkBufferDirty(MetaBuffer);

	RootBuffer = XLogReadBuffer(*node, GIN_ROOT_BLKNO, true);
	Assert(BufferIsValid(RootBuffer));
	page = (Page) BufferGetPage(RootBuffer);

	GinInitBuffer(RootBuffer, GIN_LEAF);

	PageSetLSN(page, lsn);
	MarkBufferDirty(RootBuffer);

	UnlockReleaseBuffer(RootBuffer);
	UnlockReleaseBuffer(MetaBuffer);
}

static void
ginRedoCreatePTree(XLogRecPtr lsn, XLogRecord *record)
{
	ginxlogCreatePostingTree *data = (ginxlogCreatePostingTree *) XLogRecGetData(record);
	char	   *ptr;
	Buffer		buffer;
	Page		page;

	/* Backup blocks are not used in create_ptree records */
	Assert(!(record->xl_info & XLR_BKP_BLOCK_MASK));

	buffer = XLogReadBuffer(data->node, data->blkno, true);
	Assert(BufferIsValid(buffer));
	page = (Page) BufferGetPage(buffer);

	GinInitBuffer(buffer, GIN_DATA | GIN_LEAF | GIN_COMPRESSED);

	ptr = XLogRecGetData(record) + sizeof(ginxlogCreatePostingTree);

	/* Place page data */
	memcpy(GinDataLeafPageGetPostingList(page), ptr, data->size);

	GinDataPageSetDataSize(page, data->size);

	PageSetLSN(page, lsn);

	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
}

static void
ginRedoInsertEntry(Buffer buffer, bool isLeaf, BlockNumber rightblkno, void *rdata)
{
	Page		page = BufferGetPage(buffer);
	ginxlogInsertEntry *data = (ginxlogInsertEntry *) rdata;
	OffsetNumber offset = data->offset;
	IndexTuple	itup;

	if (rightblkno != InvalidBlockNumber)
	{
		/* update link to right page after split */
		Assert(!GinPageIsLeaf(page));
		Assert(offset >= FirstOffsetNumber && offset <= PageGetMaxOffsetNumber(page));
		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offset));
		GinSetDownlink(itup, rightblkno);
	}

	if (data->isDelete)
	{
		Assert(GinPageIsLeaf(page));
		Assert(offset >= FirstOffsetNumber && offset <= PageGetMaxOffsetNumber(page));
		PageIndexTupleDelete(page, offset);
	}

	itup = &data->tuple;

	if (PageAddItem(page, (Item) itup, IndexTupleSize(itup), offset, false, false) == InvalidOffsetNumber)
	{
		RelFileNode node;
		ForkNumber	forknum;
		BlockNumber blknum;

		BufferGetTag(buffer, &node, &forknum, &blknum);
		elog(ERROR, "failed to add item to index page in %u/%u/%u",
			 node.spcNode, node.dbNode, node.relNode);
	}
}

static void
ginRedoRecompress(Page page, ginxlogRecompressDataLeaf *data)
{
	int			actionno;
	int			segno;
	GinPostingList *oldseg;
	Pointer		segmentend;
	char	   *walbuf;
	int			totalsize;

	/*
	 * If the page is in pre-9.4 format, convert to new format first.
	 */
	if (!GinPageIsCompressed(page))
	{
		ItemPointer uncompressed = (ItemPointer) GinDataPageGetData(page);
		int			nuncompressed = GinPageGetOpaque(page)->maxoff;
		int			npacked;
		GinPostingList *plist;

		plist = ginCompressPostingList(uncompressed, nuncompressed,
									   BLCKSZ, &npacked);
		Assert(npacked == nuncompressed);

		totalsize = SizeOfGinPostingList(plist);

		memcpy(GinDataLeafPageGetPostingList(page), plist, totalsize);
		GinDataPageSetDataSize(page, totalsize);
		GinPageSetCompressed(page);
		GinPageGetOpaque(page)->maxoff = InvalidOffsetNumber;
	}

	oldseg = GinDataLeafPageGetPostingList(page);
	segmentend = (Pointer) oldseg + GinDataLeafPageGetPostingListSize(page);
	segno = 0;

	walbuf = ((char *) data) + sizeof(ginxlogRecompressDataLeaf);
	for (actionno = 0; actionno < data->nactions; actionno++)
	{
		uint8		a_segno = *((uint8 *) (walbuf++));
		uint8		a_action = *((uint8 *) (walbuf++));
		GinPostingList *newseg = NULL;
		int			newsegsize = 0;
		ItemPointerData *items = NULL;
		uint16		nitems = 0;
		ItemPointerData *olditems;
		int			nolditems;
		ItemPointerData *newitems;
		int			nnewitems;
		int			segsize;
		Pointer		segptr;
		int			szleft;

		/* Extract all the information we need from the WAL record */
		if (a_action == GIN_SEGMENT_INSERT ||
			a_action == GIN_SEGMENT_REPLACE)
		{
			newseg = (GinPostingList *) walbuf;
			newsegsize = SizeOfGinPostingList(newseg);
			walbuf += SHORTALIGN(newsegsize);
		}

		if (a_action == GIN_SEGMENT_ADDITEMS)
		{
			memcpy(&nitems, walbuf, sizeof(uint16));
			walbuf += sizeof(uint16);
			items = (ItemPointerData *) walbuf;
			walbuf += nitems * sizeof(ItemPointerData);
		}

		/* Skip to the segment that this action concerns */
		Assert(segno <= a_segno);
		while (segno < a_segno)
		{
			oldseg = GinNextPostingListSegment(oldseg);
			segno++;
		}

		/*
		 * ADDITEMS action is handled like REPLACE, but the new segment to
		 * replace the old one is reconstructed using the old segment from
		 * disk and the new items from the WAL record.
		 */
		if (a_action == GIN_SEGMENT_ADDITEMS)
		{
			int			npacked;

			olditems = ginPostingListDecode(oldseg, &nolditems);

			newitems = ginMergeItemPointers(items, nitems,
											olditems, nolditems,
											&nnewitems);
			Assert(nnewitems == nolditems + nitems);

			newseg = ginCompressPostingList(newitems, nnewitems,
											BLCKSZ, &npacked);
			Assert(npacked == nnewitems);

			newsegsize = SizeOfGinPostingList(newseg);
			a_action = GIN_SEGMENT_REPLACE;
		}

		segptr = (Pointer) oldseg;
		if (segptr != segmentend)
			segsize = SizeOfGinPostingList(oldseg);
		else
		{
			/*
			 * Positioned after the last existing segment. Only INSERTs
			 * expected here.
			 */
			Assert(a_action == GIN_SEGMENT_INSERT);
			segsize = 0;
		}
		szleft = segmentend - segptr;

		switch (a_action)
		{
			case GIN_SEGMENT_DELETE:
				memmove(segptr, segptr + segsize, szleft - segsize);
				segmentend -= segsize;

				segno++;
				break;

			case GIN_SEGMENT_INSERT:
				/* make room for the new segment */
				memmove(segptr + newsegsize, segptr, szleft);
				/* copy the new segment in place */
				memcpy(segptr, newseg, newsegsize);
				segmentend += newsegsize;
				segptr += newsegsize;
				break;

			case GIN_SEGMENT_REPLACE:
				/* shift the segments that follow */
				memmove(segptr + newsegsize,
						segptr + segsize,
						szleft - segsize);
				/* copy the replacement segment in place */
				memcpy(segptr, newseg, newsegsize);
				segmentend -= segsize;
				segmentend += newsegsize;
				segptr += newsegsize;
				segno++;
				break;

			default:
				elog(ERROR, "unexpected GIN leaf action: %u", a_action);
		}
		oldseg = (GinPostingList *) segptr;
	}

	totalsize = segmentend - (Pointer) GinDataLeafPageGetPostingList(page);
	GinDataPageSetDataSize(page, totalsize);
}

static void
ginRedoInsertData(Buffer buffer, bool isLeaf, BlockNumber rightblkno, void *rdata)
{
	Page		page = BufferGetPage(buffer);

	if (isLeaf)
	{
		ginxlogRecompressDataLeaf *data = (ginxlogRecompressDataLeaf *) rdata;

		Assert(GinPageIsLeaf(page));

		ginRedoRecompress(page, data);
	}
	else
	{
		ginxlogInsertDataInternal *data = (ginxlogInsertDataInternal *) rdata;
		PostingItem *oldpitem;

		Assert(!GinPageIsLeaf(page));

		/* update link to right page after split */
		oldpitem = GinDataPageGetPostingItem(page, data->offset);
		PostingItemSetBlockNumber(oldpitem, rightblkno);

		GinDataPageAddPostingItem(page, &data->newitem, data->offset);
	}
}

static void
ginRedoInsert(XLogRecPtr lsn, XLogRecord *record)
{
	ginxlogInsert *data = (ginxlogInsert *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	char	   *payload;
	BlockNumber leftChildBlkno = InvalidBlockNumber;
	BlockNumber rightChildBlkno = InvalidBlockNumber;
	bool		isLeaf = (data->flags & GIN_INSERT_ISLEAF) != 0;

	payload = XLogRecGetData(record) + sizeof(ginxlogInsert);

	/*
	 * First clear incomplete-split flag on child page if this finishes a
	 * split.
	 */
	if (!isLeaf)
	{
		leftChildBlkno = BlockIdGetBlockNumber((BlockId) payload);
		payload += sizeof(BlockIdData);
		rightChildBlkno = BlockIdGetBlockNumber((BlockId) payload);
		payload += sizeof(BlockIdData);

		if (record->xl_info & XLR_BKP_BLOCK(0))
			(void) RestoreBackupBlock(lsn, record, 0, false, false);
		else
			ginRedoClearIncompleteSplit(lsn, data->node, leftChildBlkno);
	}

	/* If we have a full-page image, restore it and we're done */
	if (record->xl_info & XLR_BKP_BLOCK(isLeaf ? 0 : 1))
	{
		(void) RestoreBackupBlock(lsn, record, isLeaf ? 0 : 1, false, false);
		return;
	}

	buffer = XLogReadBuffer(data->node, data->blkno, false);
	if (!BufferIsValid(buffer))
		return;					/* page was deleted, nothing to do */
	page = (Page) BufferGetPage(buffer);

	if (lsn > PageGetLSN(page))
	{
		/* How to insert the payload is tree-type specific */
		if (data->flags & GIN_INSERT_ISDATA)
		{
			Assert(GinPageIsData(page));
			ginRedoInsertData(buffer, isLeaf, rightChildBlkno, payload);
		}
		else
		{
			Assert(!GinPageIsData(page));
			ginRedoInsertEntry(buffer, isLeaf, rightChildBlkno, payload);
		}

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}

	UnlockReleaseBuffer(buffer);
}

static void
ginRedoSplitEntry(Page lpage, Page rpage, void *rdata)
{
	ginxlogSplitEntry *data = (ginxlogSplitEntry *) rdata;
	IndexTuple	itup = (IndexTuple) ((char *) rdata + sizeof(ginxlogSplitEntry));
	OffsetNumber i;

	for (i = 0; i < data->separator; i++)
	{
		if (PageAddItem(lpage, (Item) itup, IndexTupleSize(itup), InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
			elog(ERROR, "failed to add item to gin index page");
		itup = (IndexTuple) (((char *) itup) + MAXALIGN(IndexTupleSize(itup)));
	}

	for (i = data->separator; i < data->nitem; i++)
	{
		if (PageAddItem(rpage, (Item) itup, IndexTupleSize(itup), InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
			elog(ERROR, "failed to add item to gin index page");
		itup = (IndexTuple) (((char *) itup) + MAXALIGN(IndexTupleSize(itup)));
	}
}

static void
ginRedoSplitData(Page lpage, Page rpage, void *rdata)
{
	bool		isleaf = GinPageIsLeaf(lpage);

	if (isleaf)
	{
		ginxlogSplitDataLeaf *data = (ginxlogSplitDataLeaf *) rdata;
		Pointer		lptr = (Pointer) rdata + sizeof(ginxlogSplitDataLeaf);
		Pointer		rptr = lptr + data->lsize;

		Assert(data->lsize > 0 && data->lsize <= GinDataPageMaxDataSize);
		Assert(data->rsize > 0 && data->rsize <= GinDataPageMaxDataSize);

		memcpy(GinDataLeafPageGetPostingList(lpage), lptr, data->lsize);
		memcpy(GinDataLeafPageGetPostingList(rpage), rptr, data->rsize);

		GinDataPageSetDataSize(lpage, data->lsize);
		GinDataPageSetDataSize(rpage, data->rsize);
		*GinDataPageGetRightBound(lpage) = data->lrightbound;
		*GinDataPageGetRightBound(rpage) = data->rrightbound;
	}
	else
	{
		ginxlogSplitDataInternal *data = (ginxlogSplitDataInternal *) rdata;
		PostingItem *items = (PostingItem *) ((char *) rdata + sizeof(ginxlogSplitDataInternal));
		OffsetNumber i;
		OffsetNumber maxoff;

		for (i = 0; i < data->separator; i++)
			GinDataPageAddPostingItem(lpage, &items[i], InvalidOffsetNumber);
		for (i = data->separator; i < data->nitem; i++)
			GinDataPageAddPostingItem(rpage, &items[i], InvalidOffsetNumber);

		/* set up right key */
		maxoff = GinPageGetOpaque(lpage)->maxoff;
		*GinDataPageGetRightBound(lpage) = GinDataPageGetPostingItem(lpage, maxoff)->key;
		*GinDataPageGetRightBound(rpage) = data->rightbound;
	}
}

static void
ginRedoSplit(XLogRecPtr lsn, XLogRecord *record)
{
	ginxlogSplit *data = (ginxlogSplit *) XLogRecGetData(record);
	Buffer		lbuffer,
				rbuffer;
	Page		lpage,
				rpage;
	uint32		flags;
	uint32		lflags,
				rflags;
	char	   *payload;
	bool		isLeaf = (data->flags & GIN_INSERT_ISLEAF) != 0;
	bool		isData = (data->flags & GIN_INSERT_ISDATA) != 0;
	bool		isRoot = (data->flags & GIN_SPLIT_ROOT) != 0;

	payload = XLogRecGetData(record) + sizeof(ginxlogSplit);

	/*
	 * First clear incomplete-split flag on child page if this finishes a
	 * split
	 */
	if (!isLeaf)
	{
		if (record->xl_info & XLR_BKP_BLOCK(0))
			(void) RestoreBackupBlock(lsn, record, 0, false, false);
		else
			ginRedoClearIncompleteSplit(lsn, data->node, data->leftChildBlkno);
	}

	flags = 0;
	if (isLeaf)
		flags |= GIN_LEAF;
	if (isData)
		flags |= GIN_DATA;
	if (isLeaf && isData)
		flags |= GIN_COMPRESSED;

	lflags = rflags = flags;
	if (!isRoot)
		lflags |= GIN_INCOMPLETE_SPLIT;

	lbuffer = XLogReadBuffer(data->node, data->lblkno, true);
	Assert(BufferIsValid(lbuffer));
	lpage = (Page) BufferGetPage(lbuffer);
	GinInitBuffer(lbuffer, lflags);

	rbuffer = XLogReadBuffer(data->node, data->rblkno, true);
	Assert(BufferIsValid(rbuffer));
	rpage = (Page) BufferGetPage(rbuffer);
	GinInitBuffer(rbuffer, rflags);

	GinPageGetOpaque(lpage)->rightlink = BufferGetBlockNumber(rbuffer);
	GinPageGetOpaque(rpage)->rightlink = isRoot ? InvalidBlockNumber : data->rrlink;

	/* Do the tree-type specific portion to restore the page contents */
	if (isData)
		ginRedoSplitData(lpage, rpage, payload);
	else
		ginRedoSplitEntry(lpage, rpage, payload);

	PageSetLSN(rpage, lsn);
	MarkBufferDirty(rbuffer);

	PageSetLSN(lpage, lsn);
	MarkBufferDirty(lbuffer);

	if (isRoot)
	{
		BlockNumber rootBlkno = data->rrlink;
		Buffer		rootBuf = XLogReadBuffer(data->node, rootBlkno, true);
		Page		rootPage = BufferGetPage(rootBuf);

		GinInitBuffer(rootBuf, flags & ~GIN_LEAF & ~GIN_COMPRESSED);

		if (isData)
		{
			Assert(rootBlkno != GIN_ROOT_BLKNO);
			ginDataFillRoot(NULL, BufferGetPage(rootBuf),
							BufferGetBlockNumber(lbuffer),
							BufferGetPage(lbuffer),
							BufferGetBlockNumber(rbuffer),
							BufferGetPage(rbuffer));
		}
		else
		{
			Assert(rootBlkno == GIN_ROOT_BLKNO);
			ginEntryFillRoot(NULL, BufferGetPage(rootBuf),
							 BufferGetBlockNumber(lbuffer),
							 BufferGetPage(lbuffer),
							 BufferGetBlockNumber(rbuffer),
							 BufferGetPage(rbuffer));
		}

		PageSetLSN(rootPage, lsn);

		MarkBufferDirty(rootBuf);
		UnlockReleaseBuffer(rootBuf);
	}

	UnlockReleaseBuffer(rbuffer);
	UnlockReleaseBuffer(lbuffer);
}

/*
 * This is functionally the same as heap_xlog_newpage.
 */
static void
ginRedoVacuumPage(XLogRecPtr lsn, XLogRecord *record)
{
	ginxlogVacuumPage *xlrec = (ginxlogVacuumPage *) XLogRecGetData(record);
	char	   *blk = ((char *) xlrec) + sizeof(ginxlogVacuumPage);
	Buffer		buffer;
	Page		page;

	Assert(xlrec->hole_offset < BLCKSZ);
	Assert(xlrec->hole_length < BLCKSZ);

	/* Backup blocks are not used, we'll re-initialize the page always. */
	Assert(!(record->xl_info & XLR_BKP_BLOCK_MASK));

	buffer = XLogReadBuffer(xlrec->node, xlrec->blkno, true);
	if (!BufferIsValid(buffer))
		return;
	page = (Page) BufferGetPage(buffer);

	if (xlrec->hole_length == 0)
	{
		memcpy((char *) page, blk, BLCKSZ);
	}
	else
	{
		memcpy((char *) page, blk, xlrec->hole_offset);
		/* must zero-fill the hole */
		MemSet((char *) page + xlrec->hole_offset, 0, xlrec->hole_length);
		memcpy((char *) page + (xlrec->hole_offset + xlrec->hole_length),
			   blk + xlrec->hole_offset,
			   BLCKSZ - (xlrec->hole_offset + xlrec->hole_length));
	}

	PageSetLSN(page, lsn);

	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
}

static void
ginRedoVacuumDataLeafPage(XLogRecPtr lsn, XLogRecord *record)
{
	ginxlogVacuumDataLeafPage *xlrec = (ginxlogVacuumDataLeafPage *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;

	/* If we have a full-page image, restore it and we're done */
	if (record->xl_info & XLR_BKP_BLOCK(0))
	{
		(void) RestoreBackupBlock(lsn, record, 0, false, false);
		return;
	}

	buffer = XLogReadBuffer(xlrec->node, xlrec->blkno, false);
	if (!BufferIsValid(buffer))
		return;
	page = (Page) BufferGetPage(buffer);

	Assert(GinPageIsLeaf(page));
	Assert(GinPageIsData(page));

	if (lsn > PageGetLSN(page))
	{
		ginRedoRecompress(page, &xlrec->data);
		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}

	UnlockReleaseBuffer(buffer);
}

static void
ginRedoDeletePage(XLogRecPtr lsn, XLogRecord *record)
{
	ginxlogDeletePage *data = (ginxlogDeletePage *) XLogRecGetData(record);
	Buffer		dbuffer;
	Buffer		pbuffer;
	Buffer		lbuffer;
	Page		page;

	if (record->xl_info & XLR_BKP_BLOCK(0))
		dbuffer = RestoreBackupBlock(lsn, record, 0, false, true);
	else
	{
		dbuffer = XLogReadBuffer(data->node, data->blkno, false);
		if (BufferIsValid(dbuffer))
		{
			page = BufferGetPage(dbuffer);
			if (lsn > PageGetLSN(page))
			{
				Assert(GinPageIsData(page));
				GinPageGetOpaque(page)->flags = GIN_DELETED;
				PageSetLSN(page, lsn);
				MarkBufferDirty(dbuffer);
			}
		}
	}

	if (record->xl_info & XLR_BKP_BLOCK(1))
		pbuffer = RestoreBackupBlock(lsn, record, 1, false, true);
	else
	{
		pbuffer = XLogReadBuffer(data->node, data->parentBlkno, false);
		if (BufferIsValid(pbuffer))
		{
			page = BufferGetPage(pbuffer);
			if (lsn > PageGetLSN(page))
			{
				Assert(GinPageIsData(page));
				Assert(!GinPageIsLeaf(page));
				GinPageDeletePostingItem(page, data->parentOffset);
				PageSetLSN(page, lsn);
				MarkBufferDirty(pbuffer);
			}
		}
	}

	if (record->xl_info & XLR_BKP_BLOCK(2))
		(void) RestoreBackupBlock(lsn, record, 2, false, false);
	else if (data->leftBlkno != InvalidBlockNumber)
	{
		lbuffer = XLogReadBuffer(data->node, data->leftBlkno, false);
		if (BufferIsValid(lbuffer))
		{
			page = BufferGetPage(lbuffer);
			if (lsn > PageGetLSN(page))
			{
				Assert(GinPageIsData(page));
				GinPageGetOpaque(page)->rightlink = data->rightLink;
				PageSetLSN(page, lsn);
				MarkBufferDirty(lbuffer);
			}
			UnlockReleaseBuffer(lbuffer);
		}
	}

	if (BufferIsValid(pbuffer))
		UnlockReleaseBuffer(pbuffer);
	if (BufferIsValid(dbuffer))
		UnlockReleaseBuffer(dbuffer);
}

static void
ginRedoUpdateMetapage(XLogRecPtr lsn, XLogRecord *record)
{
	ginxlogUpdateMeta *data = (ginxlogUpdateMeta *) XLogRecGetData(record);
	Buffer		metabuffer;
	Page		metapage;
	Buffer		buffer;

	/*
	 * Restore the metapage. This is essentially the same as a full-page
	 * image, so restore the metapage unconditionally without looking at the
	 * LSN, to avoid torn page hazards.
	 */
	metabuffer = XLogReadBuffer(data->node, GIN_METAPAGE_BLKNO, false);
	if (!BufferIsValid(metabuffer))
		return;					/* assume index was deleted, nothing to do */
	metapage = BufferGetPage(metabuffer);

	memcpy(GinPageGetMeta(metapage), &data->metadata, sizeof(GinMetaPageData));
	PageSetLSN(metapage, lsn);
	MarkBufferDirty(metabuffer);

	if (data->ntuples > 0)
	{
		/*
		 * insert into tail page
		 */
		if (record->xl_info & XLR_BKP_BLOCK(0))
			(void) RestoreBackupBlock(lsn, record, 0, false, false);
		else
		{
			buffer = XLogReadBuffer(data->node, data->metadata.tail, false);
			if (BufferIsValid(buffer))
			{
				Page		page = BufferGetPage(buffer);

				if (lsn > PageGetLSN(page))
				{
					OffsetNumber l,
								off = (PageIsEmpty(page)) ? FirstOffsetNumber :
					OffsetNumberNext(PageGetMaxOffsetNumber(page));
					int			i,
								tupsize;
					IndexTuple	tuples = (IndexTuple) (XLogRecGetData(record) + sizeof(ginxlogUpdateMeta));

					for (i = 0; i < data->ntuples; i++)
					{
						tupsize = IndexTupleSize(tuples);

						l = PageAddItem(page, (Item) tuples, tupsize, off, false, false);

						if (l == InvalidOffsetNumber)
							elog(ERROR, "failed to add item to index page");

						tuples = (IndexTuple) (((char *) tuples) + tupsize);

						off++;
					}

					/*
					 * Increase counter of heap tuples
					 */
					GinPageGetOpaque(page)->maxoff++;

					PageSetLSN(page, lsn);
					MarkBufferDirty(buffer);
				}
				UnlockReleaseBuffer(buffer);
			}
		}
	}
	else if (data->prevTail != InvalidBlockNumber)
	{
		/*
		 * New tail
		 */
		if (record->xl_info & XLR_BKP_BLOCK(0))
			(void) RestoreBackupBlock(lsn, record, 0, false, false);
		else
		{
			buffer = XLogReadBuffer(data->node, data->prevTail, false);
			if (BufferIsValid(buffer))
			{
				Page		page = BufferGetPage(buffer);

				if (lsn > PageGetLSN(page))
				{
					GinPageGetOpaque(page)->rightlink = data->newRightlink;

					PageSetLSN(page, lsn);
					MarkBufferDirty(buffer);
				}
				UnlockReleaseBuffer(buffer);
			}
		}
	}

	UnlockReleaseBuffer(metabuffer);
}

static void
ginRedoInsertListPage(XLogRecPtr lsn, XLogRecord *record)
{
	ginxlogInsertListPage *data = (ginxlogInsertListPage *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	OffsetNumber l,
				off = FirstOffsetNumber;
	int			i,
				tupsize;
	IndexTuple	tuples = (IndexTuple) (XLogRecGetData(record) + sizeof(ginxlogInsertListPage));

	/*
	 * Backup blocks are not used, we always re-initialize the page.
	 */
	Assert(!(record->xl_info & XLR_BKP_BLOCK_MASK));

	buffer = XLogReadBuffer(data->node, data->blkno, true);
	Assert(BufferIsValid(buffer));
	page = BufferGetPage(buffer);

	GinInitBuffer(buffer, GIN_LIST);
	GinPageGetOpaque(page)->rightlink = data->rightlink;
	if (data->rightlink == InvalidBlockNumber)
	{
		/* tail of sublist */
		GinPageSetFullRow(page);
		GinPageGetOpaque(page)->maxoff = 1;
	}
	else
	{
		GinPageGetOpaque(page)->maxoff = 0;
	}

	for (i = 0; i < data->ntuples; i++)
	{
		tupsize = IndexTupleSize(tuples);

		l = PageAddItem(page, (Item) tuples, tupsize, off, false, false);

		if (l == InvalidOffsetNumber)
			elog(ERROR, "failed to add item to index page");

		tuples = (IndexTuple) (((char *) tuples) + tupsize);
		off++;
	}

	PageSetLSN(page, lsn);
	MarkBufferDirty(buffer);

	UnlockReleaseBuffer(buffer);
}

static void
ginRedoDeleteListPages(XLogRecPtr lsn, XLogRecord *record)
{
	ginxlogDeleteListPages *data = (ginxlogDeleteListPages *) XLogRecGetData(record);
	Buffer		metabuffer;
	Page		metapage;
	int			i;

	/* Backup blocks are not used in delete_listpage records */
	Assert(!(record->xl_info & XLR_BKP_BLOCK_MASK));

	metabuffer = XLogReadBuffer(data->node, GIN_METAPAGE_BLKNO, false);
	if (!BufferIsValid(metabuffer))
		return;					/* assume index was deleted, nothing to do */
	metapage = BufferGetPage(metabuffer);

	memcpy(GinPageGetMeta(metapage), &data->metadata, sizeof(GinMetaPageData));
	PageSetLSN(metapage, lsn);
	MarkBufferDirty(metabuffer);

	/*
	 * In normal operation, shiftList() takes exclusive lock on all the
	 * pages-to-be-deleted simultaneously.  During replay, however, it should
	 * be all right to lock them one at a time.  This is dependent on the fact
	 * that we are deleting pages from the head of the list, and that readers
	 * share-lock the next page before releasing the one they are on. So we
	 * cannot get past a reader that is on, or due to visit, any page we are
	 * going to delete.  New incoming readers will block behind our metapage
	 * lock and then see a fully updated page list.
	 *
	 * No full-page images are taken of the deleted pages. Instead, they are
	 * re-initialized as empty, deleted pages. Their right-links don't need to
	 * be preserved, because no new readers can see the pages, as explained
	 * above.
	 */
	for (i = 0; i < data->ndeleted; i++)
	{
		Buffer		buffer;
		Page		page;

		buffer = XLogReadBuffer(data->node, data->toDelete[i], true);
		page = BufferGetPage(buffer);
		GinInitBuffer(buffer, GIN_DELETED);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);

		UnlockReleaseBuffer(buffer);
	}
	UnlockReleaseBuffer(metabuffer);
}

void
gin_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;
	MemoryContext oldCtx;

	/*
	 * GIN indexes do not require any conflict processing. NB: If we ever
	 * implement a similar optimization as we have in b-tree, and remove
	 * killed tuples outside VACUUM, we'll need to handle that here.
	 */

	oldCtx = MemoryContextSwitchTo(opCtx);
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
		case XLOG_GIN_VACUUM_DATA_LEAF_PAGE:
			ginRedoVacuumDataLeafPage(lsn, record);
			break;
		case XLOG_GIN_DELETE_PAGE:
			ginRedoDeletePage(lsn, record);
			break;
		case XLOG_GIN_UPDATE_META_PAGE:
			ginRedoUpdateMetapage(lsn, record);
			break;
		case XLOG_GIN_INSERT_LISTPAGE:
			ginRedoInsertListPage(lsn, record);
			break;
		case XLOG_GIN_DELETE_LISTPAGE:
			ginRedoDeleteListPages(lsn, record);
			break;
		default:
			elog(PANIC, "gin_redo: unknown op code %u", info);
	}
	MemoryContextSwitchTo(oldCtx);
	MemoryContextReset(opCtx);
}

void
gin_xlog_startup(void)
{
	opCtx = AllocSetContextCreate(CurrentMemoryContext,
								  "GIN recovery temporary context",
								  ALLOCSET_DEFAULT_MINSIZE,
								  ALLOCSET_DEFAULT_INITSIZE,
								  ALLOCSET_DEFAULT_MAXSIZE);
}

void
gin_xlog_cleanup(void)
{
	MemoryContextDelete(opCtx);
}
