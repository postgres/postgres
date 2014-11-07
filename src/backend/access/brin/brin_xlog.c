/*
 * brin_xlog.c
 *		XLog replay routines for BRIN indexes
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/brin/brin_xlog.c
 */
#include "postgres.h"

#include "access/brin_page.h"
#include "access/brin_pageops.h"
#include "access/brin_xlog.h"
#include "access/xlogutils.h"


/*
 * xlog replay routines
 */
static void
brin_xlog_createidx(XLogRecPtr lsn, XLogRecord *record)
{
	xl_brin_createidx *xlrec = (xl_brin_createidx *) XLogRecGetData(record);
	Buffer		buf;
	Page		page;

	/* Backup blocks are not used in create_index records */
	Assert(!(record->xl_info & XLR_BKP_BLOCK_MASK));

	/* create the index' metapage */
	buf = XLogReadBuffer(xlrec->node, BRIN_METAPAGE_BLKNO, true);
	Assert(BufferIsValid(buf));
	page = (Page) BufferGetPage(buf);
	brin_metapage_init(page, xlrec->pagesPerRange, xlrec->version);
	PageSetLSN(page, lsn);
	MarkBufferDirty(buf);
	UnlockReleaseBuffer(buf);
}

/*
 * Common part of an insert or update. Inserts the new tuple and updates the
 * revmap.
 */
static void
brin_xlog_insert_update(XLogRecPtr lsn, XLogRecord *record,
						xl_brin_insert *xlrec, BrinTuple *tuple)
{
	BlockNumber blkno;
	Buffer		buffer;
	Page		page;
	XLogRedoAction action;

	blkno = ItemPointerGetBlockNumber(&xlrec->tid);

	/*
	 * If we inserted the first and only tuple on the page, re-initialize the
	 * page from scratch.
	 */
	if (record->xl_info & XLOG_BRIN_INIT_PAGE)
	{
		XLogReadBufferForRedoExtended(lsn, record, 0,
									  xlrec->node, MAIN_FORKNUM, blkno,
									  RBM_ZERO, false, &buffer);
		page = BufferGetPage(buffer);
		brin_page_init(page, BRIN_PAGETYPE_REGULAR);
		action = BLK_NEEDS_REDO;
	}
	else
	{
		action = XLogReadBufferForRedo(lsn, record, 0,
									   xlrec->node, blkno, &buffer);
	}

	/* insert the index item into the page */
	if (action == BLK_NEEDS_REDO)
	{
		OffsetNumber offnum;

		Assert(tuple->bt_blkno == xlrec->heapBlk);

		page = (Page) BufferGetPage(buffer);
		offnum = ItemPointerGetOffsetNumber(&(xlrec->tid));
		if (PageGetMaxOffsetNumber(page) + 1 < offnum)
			elog(PANIC, "brin_xlog_insert_update: invalid max offset number");

		offnum = PageAddItem(page, (Item) tuple, xlrec->tuplen, offnum, true,
							 false);
		if (offnum == InvalidOffsetNumber)
			elog(PANIC, "brin_xlog_insert_update: failed to add tuple");

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	/* update the revmap */
	action = XLogReadBufferForRedo(lsn, record, 1, xlrec->node,
								   xlrec->revmapBlk, &buffer);
	if (action == BLK_NEEDS_REDO)
	{
		page = (Page) BufferGetPage(buffer);

		brinSetHeapBlockItemptr(buffer, xlrec->pagesPerRange, xlrec->heapBlk,
								xlrec->tid);
		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	/* XXX no FSM updates here ... */
}

/*
 * replay a BRIN index insertion
 */
static void
brin_xlog_insert(XLogRecPtr lsn, XLogRecord *record)
{
	xl_brin_insert *xlrec = (xl_brin_insert *) XLogRecGetData(record);
	BrinTuple  *newtup;

	newtup = (BrinTuple *) ((char *) xlrec + SizeOfBrinInsert);

	brin_xlog_insert_update(lsn, record, xlrec, newtup);
}

/*
 * replay a BRIN index update
 */
static void
brin_xlog_update(XLogRecPtr lsn, XLogRecord *record)
{
	xl_brin_update *xlrec = (xl_brin_update *) XLogRecGetData(record);
	BlockNumber blkno;
	Buffer		buffer;
	BrinTuple  *newtup;
	XLogRedoAction action;

	newtup = (BrinTuple *) ((char *) xlrec + SizeOfBrinUpdate);

	/* First remove the old tuple */
	blkno = ItemPointerGetBlockNumber(&(xlrec->oldtid));
	action = XLogReadBufferForRedo(lsn, record, 2, xlrec->new.node,
								   blkno, &buffer);
	if (action == BLK_NEEDS_REDO)
	{
		Page		page;
		OffsetNumber offnum;

		page = (Page) BufferGetPage(buffer);

		offnum = ItemPointerGetOffsetNumber(&(xlrec->oldtid));
		if (PageGetMaxOffsetNumber(page) + 1 < offnum)
			elog(PANIC, "brin_xlog_update: invalid max offset number");

		PageIndexDeleteNoCompact(page, &offnum, 1);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}

	/* Then insert the new tuple and update revmap, like in an insertion. */
	brin_xlog_insert_update(lsn, record, &xlrec->new, newtup);

	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

/*
 * Update a tuple on a single page.
 */
static void
brin_xlog_samepage_update(XLogRecPtr lsn, XLogRecord *record)
{
	xl_brin_samepage_update *xlrec;
	BlockNumber blkno;
	Buffer		buffer;
	XLogRedoAction action;

	xlrec = (xl_brin_samepage_update *) XLogRecGetData(record);
	blkno = ItemPointerGetBlockNumber(&(xlrec->tid));
	action = XLogReadBufferForRedo(lsn, record, 0, xlrec->node, blkno,
								   &buffer);
	if (action == BLK_NEEDS_REDO)
	{
		int			tuplen;
		BrinTuple  *mmtuple;
		Page		page;
		OffsetNumber offnum;

		tuplen = record->xl_len - SizeOfBrinSamepageUpdate;
		mmtuple = (BrinTuple *) ((char *) xlrec + SizeOfBrinSamepageUpdate);

		page = (Page) BufferGetPage(buffer);

		offnum = ItemPointerGetOffsetNumber(&(xlrec->tid));
		if (PageGetMaxOffsetNumber(page) + 1 < offnum)
			elog(PANIC, "brin_xlog_samepage_update: invalid max offset number");

		PageIndexDeleteNoCompact(page, &offnum, 1);
		offnum = PageAddItem(page, (Item) mmtuple, tuplen, offnum, true, false);
		if (offnum == InvalidOffsetNumber)
			elog(PANIC, "brin_xlog_samepage_update: failed to add tuple");

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	/* XXX no FSM updates here ... */
}

/*
 * Replay a revmap page extension
 */
static void
brin_xlog_revmap_extend(XLogRecPtr lsn, XLogRecord *record)
{
	xl_brin_revmap_extend *xlrec;
	Buffer		metabuf;
	Buffer		buf;
	Page		page;
	XLogRedoAction action;

	xlrec = (xl_brin_revmap_extend *) XLogRecGetData(record);
	/* Update the metapage */
	action = XLogReadBufferForRedo(lsn, record, 0, xlrec->node,
								   BRIN_METAPAGE_BLKNO, &metabuf);
	if (action == BLK_NEEDS_REDO)
	{
		Page		metapg;
		BrinMetaPageData *metadata;

		metapg = BufferGetPage(metabuf);
		metadata = (BrinMetaPageData *) PageGetContents(metapg);

		Assert(metadata->lastRevmapPage == xlrec->targetBlk - 1);
		metadata->lastRevmapPage = xlrec->targetBlk;

		PageSetLSN(metapg, lsn);
		MarkBufferDirty(metabuf);
	}

	/*
	 * Re-init the target block as a revmap page.  There's never a full- page
	 * image here.
	 */

	buf = XLogReadBuffer(xlrec->node, xlrec->targetBlk, true);
	page = (Page) BufferGetPage(buf);
	brin_page_init(page, BRIN_PAGETYPE_REVMAP);

	PageSetLSN(page, lsn);
	MarkBufferDirty(buf);

	UnlockReleaseBuffer(buf);
	if (BufferIsValid(metabuf))
		UnlockReleaseBuffer(metabuf);
}

void
brin_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	switch (info & XLOG_BRIN_OPMASK)
	{
		case XLOG_BRIN_CREATE_INDEX:
			brin_xlog_createidx(lsn, record);
			break;
		case XLOG_BRIN_INSERT:
			brin_xlog_insert(lsn, record);
			break;
		case XLOG_BRIN_UPDATE:
			brin_xlog_update(lsn, record);
			break;
		case XLOG_BRIN_SAMEPAGE_UPDATE:
			brin_xlog_samepage_update(lsn, record);
			break;
		case XLOG_BRIN_REVMAP_EXTEND:
			brin_xlog_revmap_extend(lsn, record);
			break;
		default:
			elog(PANIC, "brin_redo: unknown op code %u", info);
	}
}
