/*-------------------------------------------------------------------------
 *
 * gistxlog.c
 *	  WAL replay logic for GiST.
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			 src/backend/access/gist/gistxlog.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gist_private.h"
#include "access/xlogutils.h"
#include "utils/memutils.h"

typedef struct
{
	gistxlogPage *header;
	IndexTuple *itup;
} NewPage;

typedef struct
{
	gistxlogPageSplit *data;
	NewPage    *page;
} PageSplitRecord;

static MemoryContext opCtx;		/* working memory for operations */

/*
 * Replay the clearing of F_FOLLOW_RIGHT flag on a child page.
 *
 * Even if the WAL record includes a full-page image, we have to update the
 * follow-right flag, because that change is not included in the full-page
 * image.  To be sure that the intermediate state with the wrong flag value is
 * not visible to concurrent Hot Standby queries, this function handles
 * restoring the full-page image as well as updating the flag.	(Note that
 * we never need to do anything else to the child page in the current WAL
 * action.)
 */
static void
gistRedoClearFollowRight(XLogRecPtr lsn, XLogRecord *record, int block_index,
						 RelFileNode node, BlockNumber childblkno)
{
	Buffer		buffer;
	Page		page;

	if (record->xl_info & XLR_BKP_BLOCK(block_index))
		buffer = RestoreBackupBlock(lsn, record, block_index, false, true);
	else
	{
		buffer = XLogReadBuffer(node, childblkno, false);
		if (!BufferIsValid(buffer))
			return;				/* page was deleted, nothing to do */
	}
	page = (Page) BufferGetPage(buffer);

	/*
	 * Note that we still update the page even if page LSN is equal to the LSN
	 * of this record, because the updated NSN is not included in the full
	 * page image.
	 */
	if (lsn >= PageGetLSN(page))
	{
		GistPageSetNSN(page, lsn);
		GistClearFollowRight(page);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	UnlockReleaseBuffer(buffer);
}

/*
 * redo any page update (except page split)
 */
static void
gistRedoPageUpdateRecord(XLogRecPtr lsn, XLogRecord *record)
{
	char	   *begin = XLogRecGetData(record);
	gistxlogPageUpdate *xldata = (gistxlogPageUpdate *) begin;
	Buffer		buffer;
	Page		page;
	char	   *data;

	/*
	 * We need to acquire and hold lock on target page while updating the left
	 * child page.	If we have a full-page image of target page, getting the
	 * lock is a side-effect of restoring that image.  Note that even if the
	 * target page no longer exists, we'll still attempt to replay the change
	 * on the child page.
	 */
	if (record->xl_info & XLR_BKP_BLOCK(0))
		buffer = RestoreBackupBlock(lsn, record, 0, false, true);
	else
		buffer = XLogReadBuffer(xldata->node, xldata->blkno, false);

	/* Fix follow-right data on left child page */
	if (BlockNumberIsValid(xldata->leftchild))
		gistRedoClearFollowRight(lsn, record, 1,
								 xldata->node, xldata->leftchild);

	/* Done if target page no longer exists */
	if (!BufferIsValid(buffer))
		return;

	/* nothing more to do if page was backed up (and no info to do it with) */
	if (record->xl_info & XLR_BKP_BLOCK(0))
	{
		UnlockReleaseBuffer(buffer);
		return;
	}

	page = (Page) BufferGetPage(buffer);

	/* nothing more to do if change already applied */
	if (lsn <= PageGetLSN(page))
	{
		UnlockReleaseBuffer(buffer);
		return;
	}

	data = begin + sizeof(gistxlogPageUpdate);

	/* Delete old tuples */
	if (xldata->ntodelete > 0)
	{
		int			i;
		OffsetNumber *todelete = (OffsetNumber *) data;

		data += sizeof(OffsetNumber) * xldata->ntodelete;

		for (i = 0; i < xldata->ntodelete; i++)
			PageIndexTupleDelete(page, todelete[i]);
		if (GistPageIsLeaf(page))
			GistMarkTuplesDeleted(page);
	}

	/* add tuples */
	if (data - begin < record->xl_len)
	{
		OffsetNumber off = (PageIsEmpty(page)) ? FirstOffsetNumber :
		OffsetNumberNext(PageGetMaxOffsetNumber(page));

		while (data - begin < record->xl_len)
		{
			IndexTuple	itup = (IndexTuple) data;
			Size		sz = IndexTupleSize(itup);
			OffsetNumber l;

			data += sz;

			l = PageAddItem(page, (Item) itup, sz, off, false, false);
			if (l == InvalidOffsetNumber)
				elog(ERROR, "failed to add item to GiST index page, size %d bytes",
					 (int) sz);
			off++;
		}
	}
	else
	{
		/*
		 * special case: leafpage, nothing to insert, nothing to delete, then
		 * vacuum marks page
		 */
		if (GistPageIsLeaf(page) && xldata->ntodelete == 0)
			GistClearTuplesDeleted(page);
	}

	if (!GistPageIsLeaf(page) &&
		PageGetMaxOffsetNumber(page) == InvalidOffsetNumber &&
		xldata->blkno == GIST_ROOT_BLKNO)
	{
		/*
		 * all links on non-leaf root page was deleted by vacuum full, so root
		 * page becomes a leaf
		 */
		GistPageSetLeaf(page);
	}

	GistPageGetOpaque(page)->rightlink = InvalidBlockNumber;
	PageSetLSN(page, lsn);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
}

static void
decodePageSplitRecord(PageSplitRecord *decoded, XLogRecord *record)
{
	char	   *begin = XLogRecGetData(record),
			   *ptr;
	int			j,
				i = 0;

	decoded->data = (gistxlogPageSplit *) begin;
	decoded->page = (NewPage *) palloc(sizeof(NewPage) * decoded->data->npage);

	ptr = begin + sizeof(gistxlogPageSplit);
	for (i = 0; i < decoded->data->npage; i++)
	{
		Assert(ptr - begin < record->xl_len);
		decoded->page[i].header = (gistxlogPage *) ptr;
		ptr += sizeof(gistxlogPage);

		decoded->page[i].itup = (IndexTuple *)
			palloc(sizeof(IndexTuple) * decoded->page[i].header->num);
		j = 0;
		while (j < decoded->page[i].header->num)
		{
			Assert(ptr - begin < record->xl_len);
			decoded->page[i].itup[j] = (IndexTuple) ptr;
			ptr += IndexTupleSize((IndexTuple) ptr);
			j++;
		}
	}
}

static void
gistRedoPageSplitRecord(XLogRecPtr lsn, XLogRecord *record)
{
	gistxlogPageSplit *xldata = (gistxlogPageSplit *) XLogRecGetData(record);
	PageSplitRecord xlrec;
	Buffer		firstbuffer = InvalidBuffer;
	Buffer		buffer;
	Page		page;
	int			i;
	bool		isrootsplit = false;

	decodePageSplitRecord(&xlrec, record);

	/*
	 * We must hold lock on the first-listed page throughout the action,
	 * including while updating the left child page (if any).  We can unlock
	 * remaining pages in the list as soon as they've been written, because
	 * there is no path for concurrent queries to reach those pages without
	 * first visiting the first-listed page.
	 */

	/* loop around all pages */
	for (i = 0; i < xlrec.data->npage; i++)
	{
		NewPage    *newpage = xlrec.page + i;
		int			flags;

		if (newpage->header->blkno == GIST_ROOT_BLKNO)
		{
			Assert(i == 0);
			isrootsplit = true;
		}

		buffer = XLogReadBuffer(xlrec.data->node, newpage->header->blkno, true);
		Assert(BufferIsValid(buffer));
		page = (Page) BufferGetPage(buffer);

		/* ok, clear buffer */
		if (xlrec.data->origleaf && newpage->header->blkno != GIST_ROOT_BLKNO)
			flags = F_LEAF;
		else
			flags = 0;
		GISTInitBuffer(buffer, flags);

		/* and fill it */
		gistfillbuffer(page, newpage->itup, newpage->header->num, FirstOffsetNumber);

		if (newpage->header->blkno == GIST_ROOT_BLKNO)
		{
			GistPageGetOpaque(page)->rightlink = InvalidBlockNumber;
			GistPageSetNSN(page, xldata->orignsn);
			GistClearFollowRight(page);
		}
		else
		{
			if (i < xlrec.data->npage - 1)
				GistPageGetOpaque(page)->rightlink = xlrec.page[i + 1].header->blkno;
			else
				GistPageGetOpaque(page)->rightlink = xldata->origrlink;
			GistPageSetNSN(page, xldata->orignsn);
			if (i < xlrec.data->npage - 1 && !isrootsplit &&
				xldata->markfollowright)
				GistMarkFollowRight(page);
			else
				GistClearFollowRight(page);
		}

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);

		if (i == 0)
			firstbuffer = buffer;
		else
			UnlockReleaseBuffer(buffer);
	}

	/* Fix follow-right data on left child page, if any */
	if (BlockNumberIsValid(xldata->leftchild))
		gistRedoClearFollowRight(lsn, record, 0,
								 xldata->node, xldata->leftchild);

	/* Finally, release lock on the first page */
	UnlockReleaseBuffer(firstbuffer);
}

static void
gistRedoCreateIndex(XLogRecPtr lsn, XLogRecord *record)
{
	RelFileNode *node = (RelFileNode *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;

	/* Backup blocks are not used in create_index records */
	Assert(!(record->xl_info & XLR_BKP_BLOCK_MASK));

	buffer = XLogReadBuffer(*node, GIST_ROOT_BLKNO, true);
	Assert(BufferIsValid(buffer));
	page = (Page) BufferGetPage(buffer);

	GISTInitBuffer(buffer, F_LEAF);

	PageSetLSN(page, lsn);

	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
}

void
gist_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;
	MemoryContext oldCxt;

	/*
	 * GiST indexes do not require any conflict processing. NB: If we ever
	 * implement a similar optimization we have in b-tree, and remove killed
	 * tuples outside VACUUM, we'll need to handle that here.
	 */

	oldCxt = MemoryContextSwitchTo(opCtx);
	switch (info)
	{
		case XLOG_GIST_PAGE_UPDATE:
			gistRedoPageUpdateRecord(lsn, record);
			break;
		case XLOG_GIST_PAGE_SPLIT:
			gistRedoPageSplitRecord(lsn, record);
			break;
		case XLOG_GIST_CREATE_INDEX:
			gistRedoCreateIndex(lsn, record);
			break;
		default:
			elog(PANIC, "gist_redo: unknown op code %u", info);
	}

	MemoryContextSwitchTo(oldCxt);
	MemoryContextReset(opCtx);
}

void
gist_xlog_startup(void)
{
	opCtx = createTempGistContext();
}

void
gist_xlog_cleanup(void)
{
	MemoryContextDelete(opCtx);
}

/*
 * Write WAL record of a page split.
 */
XLogRecPtr
gistXLogSplit(RelFileNode node, BlockNumber blkno, bool page_is_leaf,
			  SplitedPageLayout *dist,
			  BlockNumber origrlink, GistNSN orignsn,
			  Buffer leftchildbuf, bool markfollowright)
{
	XLogRecData *rdata;
	gistxlogPageSplit xlrec;
	SplitedPageLayout *ptr;
	int			npage = 0,
				cur;
	XLogRecPtr	recptr;

	for (ptr = dist; ptr; ptr = ptr->next)
		npage++;

	rdata = (XLogRecData *) palloc(sizeof(XLogRecData) * (npage * 2 + 2));

	xlrec.node = node;
	xlrec.origblkno = blkno;
	xlrec.origrlink = origrlink;
	xlrec.orignsn = orignsn;
	xlrec.origleaf = page_is_leaf;
	xlrec.npage = (uint16) npage;
	xlrec.leftchild =
		BufferIsValid(leftchildbuf) ? BufferGetBlockNumber(leftchildbuf) : InvalidBlockNumber;
	xlrec.markfollowright = markfollowright;

	rdata[0].data = (char *) &xlrec;
	rdata[0].len = sizeof(gistxlogPageSplit);
	rdata[0].buffer = InvalidBuffer;

	cur = 1;

	/*
	 * Include a full page image of the child buf. (only necessary if a
	 * checkpoint happened since the child page was split)
	 */
	if (BufferIsValid(leftchildbuf))
	{
		rdata[cur - 1].next = &(rdata[cur]);
		rdata[cur].data = NULL;
		rdata[cur].len = 0;
		rdata[cur].buffer = leftchildbuf;
		rdata[cur].buffer_std = true;
		cur++;
	}

	for (ptr = dist; ptr; ptr = ptr->next)
	{
		rdata[cur - 1].next = &(rdata[cur]);
		rdata[cur].buffer = InvalidBuffer;
		rdata[cur].data = (char *) &(ptr->block);
		rdata[cur].len = sizeof(gistxlogPage);
		cur++;

		rdata[cur - 1].next = &(rdata[cur]);
		rdata[cur].buffer = InvalidBuffer;
		rdata[cur].data = (char *) (ptr->list);
		rdata[cur].len = ptr->lenlist;
		cur++;
	}
	rdata[cur - 1].next = NULL;

	recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_PAGE_SPLIT, rdata);

	pfree(rdata);
	return recptr;
}

/*
 * Write XLOG record describing a page update. The update can include any
 * number of deletions and/or insertions of tuples on a single index page.
 *
 * If this update inserts a downlink for a split page, also record that
 * the F_FOLLOW_RIGHT flag on the child page is cleared and NSN set.
 *
 * Note that both the todelete array and the tuples are marked as belonging
 * to the target buffer; they need not be stored in XLOG if XLogInsert decides
 * to log the whole buffer contents instead.  Also, we take care that there's
 * at least one rdata item referencing the buffer, even when ntodelete and
 * ituplen are both zero; this ensures that XLogInsert knows about the buffer.
 */
XLogRecPtr
gistXLogUpdate(RelFileNode node, Buffer buffer,
			   OffsetNumber *todelete, int ntodelete,
			   IndexTuple *itup, int ituplen,
			   Buffer leftchildbuf)
{
	XLogRecData *rdata;
	gistxlogPageUpdate xlrec;
	int			cur,
				i;
	XLogRecPtr	recptr;

	rdata = (XLogRecData *) palloc(sizeof(XLogRecData) * (3 + ituplen));

	xlrec.node = node;
	xlrec.blkno = BufferGetBlockNumber(buffer);
	xlrec.ntodelete = ntodelete;
	xlrec.leftchild =
		BufferIsValid(leftchildbuf) ? BufferGetBlockNumber(leftchildbuf) : InvalidBlockNumber;

	rdata[0].data = (char *) &xlrec;
	rdata[0].len = sizeof(gistxlogPageUpdate);
	rdata[0].buffer = InvalidBuffer;
	rdata[0].next = &(rdata[1]);

	rdata[1].data = (char *) todelete;
	rdata[1].len = sizeof(OffsetNumber) * ntodelete;
	rdata[1].buffer = buffer;
	rdata[1].buffer_std = true;

	cur = 2;

	/* new tuples */
	for (i = 0; i < ituplen; i++)
	{
		rdata[cur - 1].next = &(rdata[cur]);
		rdata[cur].data = (char *) (itup[i]);
		rdata[cur].len = IndexTupleSize(itup[i]);
		rdata[cur].buffer = buffer;
		rdata[cur].buffer_std = true;
		cur++;
	}

	/*
	 * Include a full page image of the child buf. (only necessary if a
	 * checkpoint happened since the child page was split)
	 */
	if (BufferIsValid(leftchildbuf))
	{
		rdata[cur - 1].next = &(rdata[cur]);
		rdata[cur].data = NULL;
		rdata[cur].len = 0;
		rdata[cur].buffer = leftchildbuf;
		rdata[cur].buffer_std = true;
		cur++;
	}
	rdata[cur - 1].next = NULL;

	recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_PAGE_UPDATE, rdata);

	pfree(rdata);
	return recptr;
}
