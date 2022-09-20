/*-------------------------------------------------------------------------
 *
 * gistxlog.c
 *	  WAL replay logic for GiST.
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			 src/backend/access/gist/gistxlog.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/bufmask.h"
#include "access/gist_private.h"
#include "access/gistxlog.h"
#include "access/heapam_xlog.h"
#include "access/transam.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "storage/procarray.h"
#include "utils/memutils.h"
#include "utils/rel.h"

static MemoryContext opCtx;		/* working memory for operations */

/*
 * Replay the clearing of F_FOLLOW_RIGHT flag on a child page.
 *
 * Even if the WAL record includes a full-page image, we have to update the
 * follow-right flag, because that change is not included in the full-page
 * image.  To be sure that the intermediate state with the wrong flag value is
 * not visible to concurrent Hot Standby queries, this function handles
 * restoring the full-page image as well as updating the flag.  (Note that
 * we never need to do anything else to the child page in the current WAL
 * action.)
 */
static void
gistRedoClearFollowRight(XLogReaderState *record, uint8 block_id)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	Buffer		buffer;
	Page		page;
	XLogRedoAction action;

	/*
	 * Note that we still update the page even if it was restored from a full
	 * page image, because the updated NSN is not included in the image.
	 */
	action = XLogReadBufferForRedo(record, block_id, &buffer);
	if (action == BLK_NEEDS_REDO || action == BLK_RESTORED)
	{
		page = BufferGetPage(buffer);

		GistPageSetNSN(page, lsn);
		GistClearFollowRight(page);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

/*
 * redo any page update (except page split)
 */
static void
gistRedoPageUpdateRecord(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	gistxlogPageUpdate *xldata = (gistxlogPageUpdate *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		char	   *begin;
		char	   *data;
		Size		datalen;
		int			ninserted PG_USED_FOR_ASSERTS_ONLY = 0;

		data = begin = XLogRecGetBlockData(record, 0, &datalen);

		page = (Page) BufferGetPage(buffer);

		if (xldata->ntodelete == 1 && xldata->ntoinsert == 1)
		{
			/*
			 * When replacing one tuple with one other tuple, we must use
			 * PageIndexTupleOverwrite for consistency with gistplacetopage.
			 */
			OffsetNumber offnum = *((OffsetNumber *) data);
			IndexTuple	itup;
			Size		itupsize;

			data += sizeof(OffsetNumber);
			itup = (IndexTuple) data;
			itupsize = IndexTupleSize(itup);
			if (!PageIndexTupleOverwrite(page, offnum, (Item) itup, itupsize))
				elog(ERROR, "failed to add item to GiST index page, size %d bytes",
					 (int) itupsize);
			data += itupsize;
			/* should be nothing left after consuming 1 tuple */
			Assert(data - begin == datalen);
			/* update insertion count for assert check below */
			ninserted++;
		}
		else if (xldata->ntodelete > 0)
		{
			/* Otherwise, delete old tuples if any */
			OffsetNumber *todelete = (OffsetNumber *) data;

			data += sizeof(OffsetNumber) * xldata->ntodelete;

			PageIndexMultiDelete(page, todelete, xldata->ntodelete);
			if (GistPageIsLeaf(page))
				GistMarkTuplesDeleted(page);
		}

		/* Add new tuples if any */
		if (data - begin < datalen)
		{
			OffsetNumber off = (PageIsEmpty(page)) ? FirstOffsetNumber :
			OffsetNumberNext(PageGetMaxOffsetNumber(page));

			while (data - begin < datalen)
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
				ninserted++;
			}
		}

		/* Check that XLOG record contained expected number of tuples */
		Assert(ninserted == xldata->ntoinsert);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}

	/*
	 * Fix follow-right data on left child page
	 *
	 * This must be done while still holding the lock on the target page. Note
	 * that even if the target page no longer exists, we still attempt to
	 * replay the change on the child page.
	 */
	if (XLogRecHasBlockRef(record, 1))
		gistRedoClearFollowRight(record, 1);

	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}


/*
 * redo delete on gist index page to remove tuples marked as DEAD during index
 * tuple insertion
 */
static void
gistRedoDeleteRecord(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	gistxlogDelete *xldata = (gistxlogDelete *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;

	/*
	 * If we have any conflict processing to do, it must happen before we
	 * update the page.
	 *
	 * GiST delete records can conflict with standby queries.  You might think
	 * that vacuum records would conflict as well, but we've handled that
	 * already.  XLOG_HEAP2_PRUNE records provide the highest xid cleaned by
	 * the vacuum of the heap and so we can resolve any conflicts just once
	 * when that arrives.  After that we know that no conflicts exist from
	 * individual gist vacuum records on that index.
	 */
	if (InHotStandby)
	{
		RelFileNode rnode;

		XLogRecGetBlockTag(record, 0, &rnode, NULL, NULL);

		ResolveRecoveryConflictWithSnapshot(xldata->latestRemovedXid, rnode);
	}

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		page = (Page) BufferGetPage(buffer);

		if (XLogRecGetDataLen(record) > SizeOfGistxlogDelete)
		{
			OffsetNumber *todelete;

			todelete = (OffsetNumber *) ((char *) xldata + SizeOfGistxlogDelete);

			PageIndexMultiDelete(page, todelete, xldata->ntodelete);
		}

		GistClearPageHasGarbage(page);
		GistMarkTuplesDeleted(page);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}

	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

/*
 * Returns an array of index pointers.
 */
static IndexTuple *
decodePageSplitRecord(char *begin, int len, int *n)
{
	char	   *ptr;
	int			i = 0;
	IndexTuple *tuples;

	/* extract the number of tuples */
	memcpy(n, begin, sizeof(int));
	ptr = begin + sizeof(int);

	tuples = palloc(*n * sizeof(IndexTuple));

	for (i = 0; i < *n; i++)
	{
		Assert(ptr - begin < len);
		tuples[i] = (IndexTuple) ptr;
		ptr += IndexTupleSize((IndexTuple) ptr);
	}
	Assert(ptr - begin == len);

	return tuples;
}

static void
gistRedoPageSplitRecord(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	gistxlogPageSplit *xldata = (gistxlogPageSplit *) XLogRecGetData(record);
	Buffer		firstbuffer = InvalidBuffer;
	Buffer		buffer;
	Page		page;
	int			i;
	bool		isrootsplit = false;

	/*
	 * We must hold lock on the first-listed page throughout the action,
	 * including while updating the left child page (if any).  We can unlock
	 * remaining pages in the list as soon as they've been written, because
	 * there is no path for concurrent queries to reach those pages without
	 * first visiting the first-listed page.
	 */

	/* loop around all pages */
	for (i = 0; i < xldata->npage; i++)
	{
		int			flags;
		char	   *data;
		Size		datalen;
		int			num;
		BlockNumber blkno;
		IndexTuple *tuples;

		XLogRecGetBlockTag(record, i + 1, NULL, NULL, &blkno);
		if (blkno == GIST_ROOT_BLKNO)
		{
			Assert(i == 0);
			isrootsplit = true;
		}

		buffer = XLogInitBufferForRedo(record, i + 1);
		page = (Page) BufferGetPage(buffer);
		data = XLogRecGetBlockData(record, i + 1, &datalen);

		tuples = decodePageSplitRecord(data, datalen, &num);

		/* ok, clear buffer */
		if (xldata->origleaf && blkno != GIST_ROOT_BLKNO)
			flags = F_LEAF;
		else
			flags = 0;
		GISTInitBuffer(buffer, flags);

		/* and fill it */
		gistfillbuffer(page, tuples, num, FirstOffsetNumber);

		if (blkno == GIST_ROOT_BLKNO)
		{
			GistPageGetOpaque(page)->rightlink = InvalidBlockNumber;
			GistPageSetNSN(page, xldata->orignsn);
			GistClearFollowRight(page);
		}
		else
		{
			if (i < xldata->npage - 1)
			{
				BlockNumber nextblkno;

				XLogRecGetBlockTag(record, i + 2, NULL, NULL, &nextblkno);
				GistPageGetOpaque(page)->rightlink = nextblkno;
			}
			else
				GistPageGetOpaque(page)->rightlink = xldata->origrlink;
			GistPageSetNSN(page, xldata->orignsn);
			if (i < xldata->npage - 1 && !isrootsplit &&
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
	if (XLogRecHasBlockRef(record, 0))
		gistRedoClearFollowRight(record, 0);

	/* Finally, release lock on the first page */
	UnlockReleaseBuffer(firstbuffer);
}

/* redo page deletion */
static void
gistRedoPageDelete(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	gistxlogPageDelete *xldata = (gistxlogPageDelete *) XLogRecGetData(record);
	Buffer		parentBuffer;
	Buffer		leafBuffer;

	if (XLogReadBufferForRedo(record, 0, &leafBuffer) == BLK_NEEDS_REDO)
	{
		Page		page = (Page) BufferGetPage(leafBuffer);

		GistPageSetDeleted(page, xldata->deleteXid);

		PageSetLSN(page, lsn);
		MarkBufferDirty(leafBuffer);
	}

	if (XLogReadBufferForRedo(record, 1, &parentBuffer) == BLK_NEEDS_REDO)
	{
		Page		page = (Page) BufferGetPage(parentBuffer);

		PageIndexTupleDelete(page, xldata->downlinkOffset);

		PageSetLSN(page, lsn);
		MarkBufferDirty(parentBuffer);
	}

	if (BufferIsValid(parentBuffer))
		UnlockReleaseBuffer(parentBuffer);
	if (BufferIsValid(leafBuffer))
		UnlockReleaseBuffer(leafBuffer);
}

static void
gistRedoPageReuse(XLogReaderState *record)
{
	gistxlogPageReuse *xlrec = (gistxlogPageReuse *) XLogRecGetData(record);

	/*
	 * PAGE_REUSE records exist to provide a conflict point when we reuse
	 * pages in the index via the FSM.  That's all they do though.
	 *
	 * latestRemovedXid was the page's deleteXid.  The
	 * GlobalVisCheckRemovableFullXid(deleteXid) test in gistPageRecyclable()
	 * conceptually mirrors the PGPROC->xmin > limitXmin test in
	 * GetConflictingVirtualXIDs().  Consequently, one XID value achieves the
	 * same exclusion effect on primary and standby.
	 */
	if (InHotStandby)
		ResolveRecoveryConflictWithSnapshotFullXid(xlrec->latestRemovedFullXid,
												   xlrec->node);
}

void
gist_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
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
			gistRedoPageUpdateRecord(record);
			break;
		case XLOG_GIST_DELETE:
			gistRedoDeleteRecord(record);
			break;
		case XLOG_GIST_PAGE_REUSE:
			gistRedoPageReuse(record);
			break;
		case XLOG_GIST_PAGE_SPLIT:
			gistRedoPageSplitRecord(record);
			break;
		case XLOG_GIST_PAGE_DELETE:
			gistRedoPageDelete(record);
			break;
		case XLOG_GIST_ASSIGN_LSN:
			/* nop. See gistGetFakeLSN(). */
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
 * Mask a Gist page before running consistency checks on it.
 */
void
gist_mask(char *pagedata, BlockNumber blkno)
{
	Page		page = (Page) pagedata;

	mask_page_lsn_and_checksum(page);

	mask_page_hint_bits(page);
	mask_unused_space(page);

	/*
	 * NSN is nothing but a special purpose LSN. Hence, mask it for the same
	 * reason as mask_page_lsn_and_checksum.
	 */
	GistPageSetNSN(page, (uint64) MASK_MARKER);

	/*
	 * We update F_FOLLOW_RIGHT flag on the left child after writing WAL
	 * record. Hence, mask this flag. See gistplacetopage() for details.
	 */
	GistMarkFollowRight(page);

	if (GistPageIsLeaf(page))
	{
		/*
		 * In gist leaf pages, it is possible to modify the LP_FLAGS without
		 * emitting any WAL record. Hence, mask the line pointer flags. See
		 * gistkillitems() for details.
		 */
		mask_lp_flags(page);
	}

	/*
	 * During gist redo, we never mark a page as garbage. Hence, mask it to
	 * ignore any differences.
	 */
	GistClearPageHasGarbage(page);
}

/*
 * Write WAL record of a page split.
 */
XLogRecPtr
gistXLogSplit(bool page_is_leaf,
			  SplitedPageLayout *dist,
			  BlockNumber origrlink, GistNSN orignsn,
			  Buffer leftchildbuf, bool markfollowright)
{
	gistxlogPageSplit xlrec;
	SplitedPageLayout *ptr;
	int			npage = 0;
	XLogRecPtr	recptr;
	int			i;

	for (ptr = dist; ptr; ptr = ptr->next)
		npage++;

	xlrec.origrlink = origrlink;
	xlrec.orignsn = orignsn;
	xlrec.origleaf = page_is_leaf;
	xlrec.npage = (uint16) npage;
	xlrec.markfollowright = markfollowright;

	XLogBeginInsert();

	/*
	 * Include a full page image of the child buf. (only necessary if a
	 * checkpoint happened since the child page was split)
	 */
	if (BufferIsValid(leftchildbuf))
		XLogRegisterBuffer(0, leftchildbuf, REGBUF_STANDARD);

	/*
	 * NOTE: We register a lot of data. The caller must've called
	 * XLogEnsureRecordSpace() to prepare for that. We cannot do it here,
	 * because we're already in a critical section. If you change the number
	 * of buffer or data registrations here, make sure you modify the
	 * XLogEnsureRecordSpace() calls accordingly!
	 */
	XLogRegisterData((char *) &xlrec, sizeof(gistxlogPageSplit));

	i = 1;
	for (ptr = dist; ptr; ptr = ptr->next)
	{
		XLogRegisterBuffer(i, ptr->buffer, REGBUF_WILL_INIT);
		XLogRegisterBufData(i, (char *) &(ptr->block.num), sizeof(int));
		XLogRegisterBufData(i, (char *) ptr->list, ptr->lenlist);
		i++;
	}

	recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_PAGE_SPLIT);

	return recptr;
}

/*
 * Write XLOG record describing a page deletion. This also includes removal of
 * downlink from the parent page.
 */
XLogRecPtr
gistXLogPageDelete(Buffer buffer, FullTransactionId xid,
				   Buffer parentBuffer, OffsetNumber downlinkOffset)
{
	gistxlogPageDelete xlrec;
	XLogRecPtr	recptr;

	xlrec.deleteXid = xid;
	xlrec.downlinkOffset = downlinkOffset;

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, SizeOfGistxlogPageDelete);

	XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
	XLogRegisterBuffer(1, parentBuffer, REGBUF_STANDARD);

	recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_PAGE_DELETE);

	return recptr;
}

/*
 * Write an empty XLOG record to assign a distinct LSN.
 */
XLogRecPtr
gistXLogAssignLSN(void)
{
	int			dummy = 0;

	/*
	 * Records other than SWITCH_WAL must have content. We use an integer 0 to
	 * follow the restriction.
	 */
	XLogBeginInsert();
	XLogSetRecordFlags(XLOG_MARK_UNIMPORTANT);
	XLogRegisterData((char *) &dummy, sizeof(dummy));
	return XLogInsert(RM_GIST_ID, XLOG_GIST_ASSIGN_LSN);
}

/*
 * Write XLOG record about reuse of a deleted page.
 */
void
gistXLogPageReuse(Relation rel, BlockNumber blkno, FullTransactionId latestRemovedXid)
{
	gistxlogPageReuse xlrec_reuse;

	/*
	 * Note that we don't register the buffer with the record, because this
	 * operation doesn't modify the page. This record only exists to provide a
	 * conflict point for Hot Standby.
	 */

	/* XLOG stuff */
	xlrec_reuse.node = rel->rd_node;
	xlrec_reuse.block = blkno;
	xlrec_reuse.latestRemovedFullXid = latestRemovedXid;

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec_reuse, SizeOfGistxlogPageReuse);

	XLogInsert(RM_GIST_ID, XLOG_GIST_PAGE_REUSE);
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
 * to log the whole buffer contents instead.
 */
XLogRecPtr
gistXLogUpdate(Buffer buffer,
			   OffsetNumber *todelete, int ntodelete,
			   IndexTuple *itup, int ituplen,
			   Buffer leftchildbuf)
{
	gistxlogPageUpdate xlrec;
	int			i;
	XLogRecPtr	recptr;

	xlrec.ntodelete = ntodelete;
	xlrec.ntoinsert = ituplen;

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, sizeof(gistxlogPageUpdate));

	XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
	XLogRegisterBufData(0, (char *) todelete, sizeof(OffsetNumber) * ntodelete);

	/* new tuples */
	for (i = 0; i < ituplen; i++)
		XLogRegisterBufData(0, (char *) (itup[i]), IndexTupleSize(itup[i]));

	/*
	 * Include a full page image of the child buf. (only necessary if a
	 * checkpoint happened since the child page was split)
	 */
	if (BufferIsValid(leftchildbuf))
		XLogRegisterBuffer(1, leftchildbuf, REGBUF_STANDARD);

	recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_PAGE_UPDATE);

	return recptr;
}

/*
 * Write XLOG record describing a delete of leaf index tuples marked as DEAD
 * during new tuple insertion.  One may think that this case is already covered
 * by gistXLogUpdate().  But deletion of index tuples might conflict with
 * standby queries and needs special handling.
 */
XLogRecPtr
gistXLogDelete(Buffer buffer, OffsetNumber *todelete, int ntodelete,
			   TransactionId latestRemovedXid)
{
	gistxlogDelete xlrec;
	XLogRecPtr	recptr;

	xlrec.latestRemovedXid = latestRemovedXid;
	xlrec.ntodelete = ntodelete;

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, SizeOfGistxlogDelete);

	/*
	 * We need the target-offsets array whether or not we store the whole
	 * buffer, to allow us to find the latestRemovedXid on a standby server.
	 */
	XLogRegisterData((char *) todelete, ntodelete * sizeof(OffsetNumber));

	XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

	recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_DELETE);

	return recptr;
}
