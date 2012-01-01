/*-------------------------------------------------------------------------
 *
 * gistxlog.c
 *	  WAL replay logic for GiST.
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
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
 * Replay the clearing of F_FOLLOW_RIGHT flag.
 */
static void
gistRedoClearFollowRight(RelFileNode node, XLogRecPtr lsn,
						 BlockNumber leftblkno)
{
	Buffer		buffer;

	buffer = XLogReadBuffer(node, leftblkno, false);
	if (BufferIsValid(buffer))
	{
		Page		page = (Page) BufferGetPage(buffer);

		/*
		 * Note that we still update the page even if page LSN is equal to the
		 * LSN of this record, because the updated NSN is not included in the
		 * full page image.
		 */
		if (!XLByteLT(lsn, PageGetLSN(page)))
		{
			GistPageGetOpaque(page)->nsn = lsn;
			GistClearFollowRight(page);

			PageSetLSN(page, lsn);
			PageSetTLI(page, ThisTimeLineID);
			MarkBufferDirty(buffer);
		}
		UnlockReleaseBuffer(buffer);
	}
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

	if (BlockNumberIsValid(xldata->leftchild))
		gistRedoClearFollowRight(xldata->node, lsn, xldata->leftchild);

	/* nothing more to do if page was backed up (and no info to do it with) */
	if (record->xl_info & XLR_BKP_BLOCK_1)
		return;

	buffer = XLogReadBuffer(xldata->node, xldata->blkno, false);
	if (!BufferIsValid(buffer))
		return;
	page = (Page) BufferGetPage(buffer);

	if (XLByteLE(lsn, PageGetLSN(page)))
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

	if (!GistPageIsLeaf(page) && PageGetMaxOffsetNumber(page) == InvalidOffsetNumber && xldata->blkno == GIST_ROOT_BLKNO)

		/*
		 * all links on non-leaf root page was deleted by vacuum full, so root
		 * page becomes a leaf
		 */
		GistPageSetLeaf(page);

	GistPageGetOpaque(page)->rightlink = InvalidBlockNumber;
	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
}

static void
gistRedoPageDeleteRecord(XLogRecPtr lsn, XLogRecord *record)
{
	gistxlogPageDelete *xldata = (gistxlogPageDelete *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;

	/* nothing else to do if page was backed up (and no info to do it with) */
	if (record->xl_info & XLR_BKP_BLOCK_1)
		return;

	buffer = XLogReadBuffer(xldata->node, xldata->blkno, false);
	if (!BufferIsValid(buffer))
		return;

	page = (Page) BufferGetPage(buffer);
	GistPageSetDeleted(page);

	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
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
	Buffer		buffer;
	Page		page;
	int			i;
	bool		isrootsplit = false;

	if (BlockNumberIsValid(xldata->leftchild))
		gistRedoClearFollowRight(xldata->node, lsn, xldata->leftchild);
	decodePageSplitRecord(&xlrec, record);

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
			GistPageGetOpaque(page)->nsn = xldata->orignsn;
			GistClearFollowRight(page);
		}
		else
		{
			if (i < xlrec.data->npage - 1)
				GistPageGetOpaque(page)->rightlink = xlrec.page[i + 1].header->blkno;
			else
				GistPageGetOpaque(page)->rightlink = xldata->origrlink;
			GistPageGetOpaque(page)->nsn = xldata->orignsn;
			if (i < xlrec.data->npage - 1 && !isrootsplit &&
				xldata->markfollowright)
				GistMarkFollowRight(page);
			else
				GistClearFollowRight(page);
		}

		PageSetLSN(page, lsn);
		PageSetTLI(page, ThisTimeLineID);
		MarkBufferDirty(buffer);
		UnlockReleaseBuffer(buffer);
	}
}

static void
gistRedoCreateIndex(XLogRecPtr lsn, XLogRecord *record)
{
	RelFileNode *node = (RelFileNode *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;

	buffer = XLogReadBuffer(*node, GIST_ROOT_BLKNO, true);
	Assert(BufferIsValid(buffer));
	page = (Page) BufferGetPage(buffer);

	GISTInitBuffer(buffer, F_LEAF);

	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);

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
	RestoreBkpBlocks(lsn, record, false);

	oldCxt = MemoryContextSwitchTo(opCtx);
	switch (info)
	{
		case XLOG_GIST_PAGE_UPDATE:
			gistRedoPageUpdateRecord(lsn, record);
			break;
		case XLOG_GIST_PAGE_DELETE:
			gistRedoPageDeleteRecord(lsn, record);
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

static void
out_target(StringInfo buf, RelFileNode node)
{
	appendStringInfo(buf, "rel %u/%u/%u",
					 node.spcNode, node.dbNode, node.relNode);
}

static void
out_gistxlogPageUpdate(StringInfo buf, gistxlogPageUpdate *xlrec)
{
	out_target(buf, xlrec->node);
	appendStringInfo(buf, "; block number %u", xlrec->blkno);
}

static void
out_gistxlogPageDelete(StringInfo buf, gistxlogPageDelete *xlrec)
{
	appendStringInfo(buf, "page_delete: rel %u/%u/%u; blkno %u",
				xlrec->node.spcNode, xlrec->node.dbNode, xlrec->node.relNode,
					 xlrec->blkno);
}

static void
out_gistxlogPageSplit(StringInfo buf, gistxlogPageSplit *xlrec)
{
	appendStringInfo(buf, "page_split: ");
	out_target(buf, xlrec->node);
	appendStringInfo(buf, "; block number %u splits to %d pages",
					 xlrec->origblkno, xlrec->npage);
}

void
gist_desc(StringInfo buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_GIST_PAGE_UPDATE:
			appendStringInfo(buf, "page_update: ");
			out_gistxlogPageUpdate(buf, (gistxlogPageUpdate *) rec);
			break;
		case XLOG_GIST_PAGE_DELETE:
			out_gistxlogPageDelete(buf, (gistxlogPageDelete *) rec);
			break;
		case XLOG_GIST_PAGE_SPLIT:
			out_gistxlogPageSplit(buf, (gistxlogPageSplit *) rec);
			break;
		case XLOG_GIST_CREATE_INDEX:
			appendStringInfo(buf, "create_index: rel %u/%u/%u",
							 ((RelFileNode *) rec)->spcNode,
							 ((RelFileNode *) rec)->dbNode,
							 ((RelFileNode *) rec)->relNode);
			break;
		default:
			appendStringInfo(buf, "unknown gist op code %u", info);
			break;
	}
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
	gistxlogPageUpdate *xlrec;
	int			cur,
				i;
	XLogRecPtr	recptr;

	rdata = (XLogRecData *) palloc(sizeof(XLogRecData) * (4 + ituplen));
	xlrec = (gistxlogPageUpdate *) palloc(sizeof(gistxlogPageUpdate));

	xlrec->node = node;
	xlrec->blkno = BufferGetBlockNumber(buffer);
	xlrec->ntodelete = ntodelete;
	xlrec->leftchild =
		BufferIsValid(leftchildbuf) ? BufferGetBlockNumber(leftchildbuf) : InvalidBlockNumber;

	rdata[0].buffer = buffer;
	rdata[0].buffer_std = true;
	rdata[0].data = NULL;
	rdata[0].len = 0;
	rdata[0].next = &(rdata[1]);

	rdata[1].data = (char *) xlrec;
	rdata[1].len = sizeof(gistxlogPageUpdate);
	rdata[1].buffer = InvalidBuffer;
	rdata[1].next = &(rdata[2]);

	rdata[2].data = (char *) todelete;
	rdata[2].len = sizeof(OffsetNumber) * ntodelete;
	rdata[2].buffer = buffer;
	rdata[2].buffer_std = true;

	cur = 3;

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
