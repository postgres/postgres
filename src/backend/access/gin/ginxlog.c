/*-------------------------------------------------------------------------
 *
 * ginxlog.c
 *	  WAL replay logic for inverted index.
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
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
ginRedoClearIncompleteSplit(XLogReaderState *record, uint8 block_id)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	Buffer		buffer;
	Page		page;

	if (XLogReadBufferForRedo(record, block_id, &buffer) == BLK_NEEDS_REDO)
	{
		page = (Page) BufferGetPage(buffer);
		GinPageGetOpaque(page)->flags &= ~GIN_INCOMPLETE_SPLIT;

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
ginRedoCreateIndex(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	Buffer		RootBuffer,
				MetaBuffer;
	Page		page;

	MetaBuffer = XLogInitBufferForRedo(record, 0);
	Assert(BufferGetBlockNumber(MetaBuffer) == GIN_METAPAGE_BLKNO);
	page = (Page) BufferGetPage(MetaBuffer);

	GinInitMetabuffer(MetaBuffer);

	PageSetLSN(page, lsn);
	MarkBufferDirty(MetaBuffer);

	RootBuffer = XLogInitBufferForRedo(record, 1);
	Assert(BufferGetBlockNumber(RootBuffer) == GIN_ROOT_BLKNO);
	page = (Page) BufferGetPage(RootBuffer);

	GinInitBuffer(RootBuffer, GIN_LEAF);

	PageSetLSN(page, lsn);
	MarkBufferDirty(RootBuffer);

	UnlockReleaseBuffer(RootBuffer);
	UnlockReleaseBuffer(MetaBuffer);
}

static void
ginRedoCreatePTree(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	ginxlogCreatePostingTree *data = (ginxlogCreatePostingTree *) XLogRecGetData(record);
	char	   *ptr;
	Buffer		buffer;
	Page		page;

	buffer = XLogInitBufferForRedo(record, 0);
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
ginRedoInsert(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	ginxlogInsert *data = (ginxlogInsert *) XLogRecGetData(record);
	Buffer		buffer;
#ifdef NOT_USED
	BlockNumber leftChildBlkno = InvalidBlockNumber;
#endif
	BlockNumber rightChildBlkno = InvalidBlockNumber;
	bool		isLeaf = (data->flags & GIN_INSERT_ISLEAF) != 0;

	/*
	 * First clear incomplete-split flag on child page if this finishes a
	 * split.
	 */
	if (!isLeaf)
	{
		char	   *payload = XLogRecGetData(record) + sizeof(ginxlogInsert);

#ifdef NOT_USED
		leftChildBlkno = BlockIdGetBlockNumber((BlockId) payload);
#endif
		payload += sizeof(BlockIdData);
		rightChildBlkno = BlockIdGetBlockNumber((BlockId) payload);
		payload += sizeof(BlockIdData);

		ginRedoClearIncompleteSplit(record, 1);
	}

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buffer);
		Size		len;
		char	   *payload = XLogRecGetBlockData(record, 0, &len);

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
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
ginRedoSplit(XLogReaderState *record)
{
	ginxlogSplit *data = (ginxlogSplit *) XLogRecGetData(record);
	Buffer		lbuffer,
				rbuffer,
				rootbuf;
	bool		isLeaf = (data->flags & GIN_INSERT_ISLEAF) != 0;
	bool		isRoot = (data->flags & GIN_SPLIT_ROOT) != 0;

	/*
	 * First clear incomplete-split flag on child page if this finishes a
	 * split
	 */
	if (!isLeaf)
		ginRedoClearIncompleteSplit(record, 3);

	if (XLogReadBufferForRedo(record, 0, &lbuffer) != BLK_RESTORED)
		elog(ERROR, "GIN split record did not contain a full-page image of left page");

	if (XLogReadBufferForRedo(record, 1, &rbuffer) != BLK_RESTORED)
		elog(ERROR, "GIN split record did not contain a full-page image of right page");

	if (isRoot)
	{
		if (XLogReadBufferForRedo(record, 2, &rootbuf) != BLK_RESTORED)
			elog(ERROR, "GIN split record did not contain a full-page image of root page");
		UnlockReleaseBuffer(rootbuf);
	}

	UnlockReleaseBuffer(rbuffer);
	UnlockReleaseBuffer(lbuffer);
}

/*
 * VACUUM_PAGE record contains simply a full image of the page, similar to
 * an XLOG_FPI record.
 */
static void
ginRedoVacuumPage(XLogReaderState *record)
{
	Buffer		buffer;

	if (XLogReadBufferForRedo(record, 0, &buffer) != BLK_RESTORED)
	{
		elog(ERROR, "replay of gin entry tree page vacuum did not restore the page");
	}
	UnlockReleaseBuffer(buffer);
}

static void
ginRedoVacuumDataLeafPage(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	Buffer		buffer;

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buffer);
		Size		len;
		ginxlogVacuumDataLeafPage *xlrec;

		xlrec = (ginxlogVacuumDataLeafPage *) XLogRecGetBlockData(record, 0, &len);

		Assert(GinPageIsLeaf(page));
		Assert(GinPageIsData(page));

		ginRedoRecompress(page, &xlrec->data);
		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
ginRedoDeletePage(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	ginxlogDeletePage *data = (ginxlogDeletePage *) XLogRecGetData(record);
	Buffer		dbuffer;
	Buffer		pbuffer;
	Buffer		lbuffer;
	Page		page;

	if (XLogReadBufferForRedo(record, 0, &dbuffer) == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(dbuffer);
		Assert(GinPageIsData(page));
		GinPageGetOpaque(page)->flags = GIN_DELETED;
		PageSetLSN(page, lsn);
		MarkBufferDirty(dbuffer);
	}

	if (XLogReadBufferForRedo(record, 1, &pbuffer) == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(pbuffer);
		Assert(GinPageIsData(page));
		Assert(!GinPageIsLeaf(page));
		GinPageDeletePostingItem(page, data->parentOffset);
		PageSetLSN(page, lsn);
		MarkBufferDirty(pbuffer);
	}

	if (XLogReadBufferForRedo(record, 2, &lbuffer) == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(lbuffer);
		Assert(GinPageIsData(page));
		GinPageGetOpaque(page)->rightlink = data->rightLink;
		PageSetLSN(page, lsn);
		MarkBufferDirty(lbuffer);
	}

	if (BufferIsValid(lbuffer))
		UnlockReleaseBuffer(lbuffer);
	if (BufferIsValid(pbuffer))
		UnlockReleaseBuffer(pbuffer);
	if (BufferIsValid(dbuffer))
		UnlockReleaseBuffer(dbuffer);
}

static void
ginRedoUpdateMetapage(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	ginxlogUpdateMeta *data = (ginxlogUpdateMeta *) XLogRecGetData(record);
	Buffer		metabuffer;
	Page		metapage;
	Buffer		buffer;

	/*
	 * Restore the metapage. This is essentially the same as a full-page
	 * image, so restore the metapage unconditionally without looking at the
	 * LSN, to avoid torn page hazards.
	 */
	metabuffer = XLogInitBufferForRedo(record, 0);
	Assert(BufferGetBlockNumber(metabuffer) == GIN_METAPAGE_BLKNO);
	metapage = BufferGetPage(metabuffer);

	GinInitPage(metapage, GIN_META, BufferGetPageSize(metabuffer));
	memcpy(GinPageGetMeta(metapage), &data->metadata, sizeof(GinMetaPageData));
	PageSetLSN(metapage, lsn);
	MarkBufferDirty(metabuffer);

	if (data->ntuples > 0)
	{
		/*
		 * insert into tail page
		 */
		if (XLogReadBufferForRedo(record, 1, &buffer) == BLK_NEEDS_REDO)
		{
			Page		page = BufferGetPage(buffer);
			OffsetNumber off;
			int			i;
			Size		tupsize;
			char	   *payload;
			IndexTuple	tuples;
			Size		totaltupsize;

			payload = XLogRecGetBlockData(record, 1, &totaltupsize);
			tuples = (IndexTuple) payload;

			if (PageIsEmpty(page))
				off = FirstOffsetNumber;
			else
				off = OffsetNumberNext(PageGetMaxOffsetNumber(page));

			for (i = 0; i < data->ntuples; i++)
			{
				tupsize = IndexTupleSize(tuples);

				if (PageAddItem(page, (Item) tuples, tupsize, off,
								false, false) == InvalidOffsetNumber)
					elog(ERROR, "failed to add item to index page");

				tuples = (IndexTuple) (((char *) tuples) + tupsize);

				off++;
			}
			Assert(payload + totaltupsize == (char *) tuples);

			/*
			 * Increase counter of heap tuples
			 */
			GinPageGetOpaque(page)->maxoff++;

			PageSetLSN(page, lsn);
			MarkBufferDirty(buffer);
		}
		if (BufferIsValid(buffer))
			UnlockReleaseBuffer(buffer);
	}
	else if (data->prevTail != InvalidBlockNumber)
	{
		/*
		 * New tail
		 */
		if (XLogReadBufferForRedo(record, 1, &buffer) == BLK_NEEDS_REDO)
		{
			Page		page = BufferGetPage(buffer);

			GinPageGetOpaque(page)->rightlink = data->newRightlink;

			PageSetLSN(page, lsn);
			MarkBufferDirty(buffer);
		}
		if (BufferIsValid(buffer))
			UnlockReleaseBuffer(buffer);
	}

	UnlockReleaseBuffer(metabuffer);
}

static void
ginRedoInsertListPage(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	ginxlogInsertListPage *data = (ginxlogInsertListPage *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	OffsetNumber l,
				off = FirstOffsetNumber;
	int			i,
				tupsize;
	char	   *payload;
	IndexTuple	tuples;
	Size		totaltupsize;

	/* We always re-initialize the page. */
	buffer = XLogInitBufferForRedo(record, 0);
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

	payload = XLogRecGetBlockData(record, 0, &totaltupsize);

	tuples = (IndexTuple) payload;
	for (i = 0; i < data->ntuples; i++)
	{
		tupsize = IndexTupleSize(tuples);

		l = PageAddItem(page, (Item) tuples, tupsize, off, false, false);

		if (l == InvalidOffsetNumber)
			elog(ERROR, "failed to add item to index page");

		tuples = (IndexTuple) (((char *) tuples) + tupsize);
		off++;
	}
	Assert((char *) tuples == payload + totaltupsize);

	PageSetLSN(page, lsn);
	MarkBufferDirty(buffer);

	UnlockReleaseBuffer(buffer);
}

static void
ginRedoDeleteListPages(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	ginxlogDeleteListPages *data = (ginxlogDeleteListPages *) XLogRecGetData(record);
	Buffer		metabuffer;
	Page		metapage;
	int			i;

	metabuffer = XLogInitBufferForRedo(record, 0);
	Assert(BufferGetBlockNumber(metabuffer) == GIN_METAPAGE_BLKNO);
	metapage = BufferGetPage(metabuffer);

	GinInitPage(metapage, GIN_META, BufferGetPageSize(metabuffer));

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

		buffer = XLogInitBufferForRedo(record, i + 1);
		page = BufferGetPage(buffer);
		GinInitBuffer(buffer, GIN_DELETED);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);

		UnlockReleaseBuffer(buffer);
	}
	UnlockReleaseBuffer(metabuffer);
}

void
gin_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
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
			ginRedoCreateIndex(record);
			break;
		case XLOG_GIN_CREATE_PTREE:
			ginRedoCreatePTree(record);
			break;
		case XLOG_GIN_INSERT:
			ginRedoInsert(record);
			break;
		case XLOG_GIN_SPLIT:
			ginRedoSplit(record);
			break;
		case XLOG_GIN_VACUUM_PAGE:
			ginRedoVacuumPage(record);
			break;
		case XLOG_GIN_VACUUM_DATA_LEAF_PAGE:
			ginRedoVacuumDataLeafPage(record);
			break;
		case XLOG_GIN_DELETE_PAGE:
			ginRedoDeletePage(record);
			break;
		case XLOG_GIN_UPDATE_META_PAGE:
			ginRedoUpdateMetapage(record);
			break;
		case XLOG_GIN_INSERT_LISTPAGE:
			ginRedoInsertListPage(record);
			break;
		case XLOG_GIN_DELETE_LISTPAGE:
			ginRedoDeleteListPages(record);
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
								  ALLOCSET_DEFAULT_SIZES);
}

void
gin_xlog_cleanup(void)
{
	MemoryContextDelete(opCtx);
	opCtx = NULL;
}
