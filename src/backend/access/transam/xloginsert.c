/*-------------------------------------------------------------------------
 *
 * xloginsert.c
 *		Functions for constructing WAL records
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/xloginsert.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xloginsert.h"
#include "catalog/pg_control.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/proc.h"
#include "utils/memutils.h"
#include "pg_trace.h"

static XLogRecData *XLogRecordAssemble(RmgrId rmid, uint8 info,
				   XLogRecData *rdata,
				   XLogRecPtr RedoRecPtr, bool doPageWrites,
				   XLogRecPtr *fpw_lsn, XLogRecData **rdt_lastnormal);
static void XLogFillBkpBlock(Buffer buffer, bool buffer_std, BkpBlock *bkpb);

/*
 * Insert an XLOG record having the specified RMID and info bytes,
 * with the body of the record being the data chunk(s) described by
 * the rdata chain (see xloginsert.h for notes about rdata).
 *
 * Returns XLOG pointer to end of record (beginning of next record).
 * This can be used as LSN for data pages affected by the logged action.
 * (LSN is the XLOG point up to which the XLOG must be flushed to disk
 * before the data page can be written out.  This implements the basic
 * WAL rule "write the log before the data".)
 *
 * NB: this routine feels free to scribble on the XLogRecData structs,
 * though not on the data they reference.  This is OK since the XLogRecData
 * structs are always just temporaries in the calling code.
 */
XLogRecPtr
XLogInsert(RmgrId rmid, uint8 info, XLogRecData *rdata)
{
	XLogRecPtr	RedoRecPtr;
	bool		doPageWrites;
	XLogRecPtr	EndPos;
	XLogRecPtr	fpw_lsn;
	XLogRecData *rdt;
	XLogRecData *rdt_lastnormal;

	/* info's high bits are reserved for use by me */
	if (info & XLR_INFO_MASK)
		elog(PANIC, "invalid xlog info mask %02X", info);

	TRACE_POSTGRESQL_XLOG_INSERT(rmid, info);

	/*
	 * In bootstrap mode, we don't actually log anything but XLOG resources;
	 * return a phony record pointer.
	 */
	if (IsBootstrapProcessingMode() && rmid != RM_XLOG_ID)
	{
		EndPos = SizeOfXLogLongPHD;		/* start of 1st chkpt record */
		return EndPos;
	}

	/*
	 * Get values needed to decide whether to do full-page writes. Since we
	 * don't yet have an insertion lock, these could change under us, but
	 * XLogInsertRecord will recheck them once it has a lock.
	 */
	GetFullPageWriteInfo(&RedoRecPtr, &doPageWrites);

	/*
	 * Assemble an XLogRecData chain representing the WAL record, including
	 * any backup blocks needed.
	 *
	 * We may have to loop back to here if a race condition is detected in
	 * XLogInsertRecord.  We could prevent the race by doing all this work
	 * while holding an insertion lock, but it seems better to avoid doing CRC
	 * calculations while holding one.
	 */
retry:
	rdt = XLogRecordAssemble(rmid, info, rdata, RedoRecPtr, doPageWrites,
							 &fpw_lsn, &rdt_lastnormal);

	EndPos = XLogInsertRecord(rdt, fpw_lsn);

	if (EndPos == InvalidXLogRecPtr)
	{
		/*
		 * Undo the changes we made to the rdata chain, and retry.
		 *
		 * XXX: This doesn't undo *all* the changes; the XLogRecData
		 * entries for buffers that we had already decided to back up have
		 * had their data-pointers cleared. That's OK, as long as we
		 * decide to back them up on the next iteration as well. Hence,
		 * don't allow "doPageWrites" value to go from true to false after
		 * we've modified the rdata chain.
		 */
		bool		newDoPageWrites;

		GetFullPageWriteInfo(&RedoRecPtr, &newDoPageWrites);
		doPageWrites = doPageWrites || newDoPageWrites;
		rdt_lastnormal->next = NULL;

		goto retry;
	}

	return EndPos;
}

/*
 * Assemble a full WAL record, including backup blocks, from an XLogRecData
 * chain, ready for insertion with XLogInsertRecord(). The record header
 * fields are filled in, except for the xl_prev field and CRC.
 *
 * The rdata chain is modified, adding entries for full-page images.
 * *rdt_lastnormal is set to point to the last normal (ie. not added by
 * this function) entry. It can be used to reset the chain to its original
 * state.
 *
 * If the rdata chain contains any buffer references, and a full-page image
 * was not taken of all the buffers, *fpw_lsn is set to the lowest LSN among
 * such pages. This signals that the assembled record is only good for
 * insertion on the assumption that the RedoRecPtr and doPageWrites values
 * were up-to-date.
 */
static XLogRecData *
XLogRecordAssemble(RmgrId rmid, uint8 info, XLogRecData *rdata,
				   XLogRecPtr RedoRecPtr, bool doPageWrites,
				   XLogRecPtr *fpw_lsn, XLogRecData **rdt_lastnormal)
{
	bool		isLogSwitch = (rmid == RM_XLOG_ID && info == XLOG_SWITCH);
	XLogRecData *rdt;
	Buffer		dtbuf[XLR_MAX_BKP_BLOCKS];
	bool		dtbuf_bkp[XLR_MAX_BKP_BLOCKS];
	uint32		len,
				total_len;
	unsigned	i;

	/*
	 * These need to be static because they are returned to the caller as part
	 * of the XLogRecData chain.
	 */
	static BkpBlock dtbuf_xlg[XLR_MAX_BKP_BLOCKS];
	static XLogRecData dtbuf_rdt1[XLR_MAX_BKP_BLOCKS];
	static XLogRecData dtbuf_rdt2[XLR_MAX_BKP_BLOCKS];
	static XLogRecData dtbuf_rdt3[XLR_MAX_BKP_BLOCKS];
	static XLogRecData hdr_rdt;
	static XLogRecord *rechdr;

	if (rechdr == NULL)
	{
		static char rechdrbuf[SizeOfXLogRecord + MAXIMUM_ALIGNOF];

		rechdr = (XLogRecord *) MAXALIGN(&rechdrbuf);
		MemSet(rechdr, 0, SizeOfXLogRecord);
	}

	/* The record begins with the header */
	hdr_rdt.data = (char *) rechdr;
	hdr_rdt.len = SizeOfXLogRecord;
	hdr_rdt.next = rdata;
	total_len = SizeOfXLogRecord;

	/*
	 * Here we scan the rdata chain, to determine which buffers must be backed
	 * up.
	 *
	 * We add entries for backup blocks to the chain, so that they don't need
	 * any special treatment in the critical section where the chunks are
	 * copied into the WAL buffers. Those entries have to be unlinked from the
	 * chain if we have to loop back here.
	 */
	for (i = 0; i < XLR_MAX_BKP_BLOCKS; i++)
	{
		dtbuf[i] = InvalidBuffer;
		dtbuf_bkp[i] = false;
	}

	*fpw_lsn = InvalidXLogRecPtr;
	len = 0;
	for (rdt = rdata;;)
	{
		if (rdt->buffer == InvalidBuffer)
		{
			/* Simple data, just include it */
			len += rdt->len;
		}
		else
		{
			/* Find info for buffer */
			for (i = 0; i < XLR_MAX_BKP_BLOCKS; i++)
			{
				if (rdt->buffer == dtbuf[i])
				{
					/* Buffer already referenced by earlier chain item */
					if (dtbuf_bkp[i])
					{
						rdt->data = NULL;
						rdt->len = 0;
					}
					else if (rdt->data)
						len += rdt->len;
					break;
				}
				if (dtbuf[i] == InvalidBuffer)
				{
					/* OK, put it in this slot */
					XLogRecPtr	page_lsn;
					bool		needs_backup;

					dtbuf[i] = rdt->buffer;

					/*
					 * Determine whether the buffer has to be backed up.
					 *
					 * We assume page LSN is first data on *every* page that
					 * can be passed to XLogInsert, whether it has the
					 * standard page layout or not. We don't need to take the
					 * buffer header lock for PageGetLSN because we hold an
					 * exclusive lock on the page and/or the relation.
					 */
					page_lsn = PageGetLSN(BufferGetPage(rdt->buffer));
					if (!doPageWrites)
						needs_backup = false;
					else if (page_lsn <= RedoRecPtr)
						needs_backup = true;
					else
						needs_backup = false;

					if (needs_backup)
					{
						/*
						 * The page needs to be backed up, so set up BkpBlock
						 */
						XLogFillBkpBlock(rdt->buffer, rdt->buffer_std,
										 &(dtbuf_xlg[i]));
						dtbuf_bkp[i] = true;
						rdt->data = NULL;
						rdt->len = 0;
					}
					else
					{
						if (rdt->data)
							len += rdt->len;
						if (*fpw_lsn == InvalidXLogRecPtr ||
							page_lsn < *fpw_lsn)
						{
							*fpw_lsn = page_lsn;
						}
					}
					break;
				}
			}
			if (i >= XLR_MAX_BKP_BLOCKS)
				elog(PANIC, "can backup at most %d blocks per xlog record",
					 XLR_MAX_BKP_BLOCKS);
		}
		/* Break out of loop when rdt points to last chain item */
		if (rdt->next == NULL)
			break;
		rdt = rdt->next;
	}
	total_len += len;

	/*
	 * Make additional rdata chain entries for the backup blocks, so that we
	 * don't need to special-case them in the write loop.  This modifies the
	 * original rdata chain, but we keep a pointer to the last regular entry,
	 * rdt_lastnormal, so that we can undo this if we have to start over.
	 *
	 * At the exit of this loop, total_len includes the backup block data.
	 *
	 * Also set the appropriate info bits to show which buffers were backed
	 * up. The XLR_BKP_BLOCK(N) bit corresponds to the N'th distinct buffer
	 * value (ignoring InvalidBuffer) appearing in the rdata chain.
	 */
	*rdt_lastnormal = rdt;
	for (i = 0; i < XLR_MAX_BKP_BLOCKS; i++)
	{
		BkpBlock   *bkpb;
		char	   *page;

		if (!dtbuf_bkp[i])
			continue;

		info |= XLR_BKP_BLOCK(i);

		bkpb = &(dtbuf_xlg[i]);
		page = (char *) BufferGetBlock(dtbuf[i]);

		rdt->next = &(dtbuf_rdt1[i]);
		rdt = rdt->next;

		rdt->data = (char *) bkpb;
		rdt->len = sizeof(BkpBlock);
		total_len += sizeof(BkpBlock);

		rdt->next = &(dtbuf_rdt2[i]);
		rdt = rdt->next;

		if (bkpb->hole_length == 0)
		{
			rdt->data = page;
			rdt->len = BLCKSZ;
			total_len += BLCKSZ;
			rdt->next = NULL;
		}
		else
		{
			/* must skip the hole */
			rdt->data = page;
			rdt->len = bkpb->hole_offset;
			total_len += bkpb->hole_offset;

			rdt->next = &(dtbuf_rdt3[i]);
			rdt = rdt->next;

			rdt->data = page + (bkpb->hole_offset + bkpb->hole_length);
			rdt->len = BLCKSZ - (bkpb->hole_offset + bkpb->hole_length);
			total_len += rdt->len;
			rdt->next = NULL;
		}
	}

	/*
	 * We disallow len == 0 because it provides a useful bit of extra error
	 * checking in ReadRecord.  This means that all callers of XLogInsert
	 * must supply at least some not-in-a-buffer data.  However, we make an
	 * exception for XLOG SWITCH records because we don't want them to ever
	 * cross a segment boundary.
	 */
	if (len == 0 && !isLogSwitch)
		elog(PANIC, "invalid xlog record length %u", rechdr->xl_len);

	/*
	 * Fill in the fields in the record header. Prev-link is filled in later,
	 * once we know where in the WAL the record will be inserted. CRC is also
	 * not calculated yet.
	 */
	rechdr->xl_xid = GetCurrentTransactionIdIfAny();
	rechdr->xl_tot_len = total_len;
	rechdr->xl_len = len;		/* doesn't include backup blocks */
	rechdr->xl_info = info;
	rechdr->xl_rmid = rmid;
	rechdr->xl_prev = InvalidXLogRecPtr;

	return &hdr_rdt;
}

/*
 * Determine whether the buffer referenced has to be backed up.
 *
 * Since we don't yet have the insert lock, fullPageWrites and forcePageWrites
 * could change later, so the result should be used for optimization purposes
 * only.
 */
bool
XLogCheckBufferNeedsBackup(Buffer buffer)
{
	XLogRecPtr	RedoRecPtr;
	bool		doPageWrites;
	Page		page;

	GetFullPageWriteInfo(&RedoRecPtr, &doPageWrites);

	page = BufferGetPage(buffer);

	if (doPageWrites && PageGetLSN(page) <= RedoRecPtr)
		return true;			/* buffer requires backup */

	return false;				/* buffer does not need to be backed up */
}

/*
 * Write a backup block if needed when we are setting a hint. Note that
 * this may be called for a variety of page types, not just heaps.
 *
 * Callable while holding just share lock on the buffer content.
 *
 * We can't use the plain backup block mechanism since that relies on the
 * Buffer being exclusively locked. Since some modifications (setting LSN, hint
 * bits) are allowed in a sharelocked buffer that can lead to wal checksum
 * failures. So instead we copy the page and insert the copied data as normal
 * record data.
 *
 * We only need to do something if page has not yet been full page written in
 * this checkpoint round. The LSN of the inserted wal record is returned if we
 * had to write, InvalidXLogRecPtr otherwise.
 *
 * It is possible that multiple concurrent backends could attempt to write WAL
 * records. In that case, multiple copies of the same block would be recorded
 * in separate WAL records by different backends, though that is still OK from
 * a correctness perspective.
 */
XLogRecPtr
XLogSaveBufferForHint(Buffer buffer, bool buffer_std)
{
	XLogRecPtr	recptr = InvalidXLogRecPtr;
	XLogRecPtr	lsn;
	XLogRecPtr	RedoRecPtr;

	/*
	 * Ensure no checkpoint can change our view of RedoRecPtr.
	 */
	Assert(MyPgXact->delayChkpt);

	/*
	 * Update RedoRecPtr so that we can make the right decision
	 */
	RedoRecPtr = GetRedoRecPtr();

	/*
	 * We assume page LSN is first data on *every* page that can be passed to
	 * XLogInsert, whether it has the standard page layout or not. Since we're
	 * only holding a share-lock on the page, we must take the buffer header
	 * lock when we look at the LSN.
	 */
	lsn = BufferGetLSNAtomic(buffer);

	if (lsn <= RedoRecPtr)
	{
		XLogRecData rdata[2];
		BkpBlock	bkpb;
		char		copied_buffer[BLCKSZ];
		char	   *origdata = (char *) BufferGetBlock(buffer);

		/* Make a BkpBlock struct representing the buffer */
		XLogFillBkpBlock(buffer, buffer_std, &bkpb);

		/*
		 * Copy buffer so we don't have to worry about concurrent hint bit or
		 * lsn updates. We assume pd_lower/upper cannot be changed without an
		 * exclusive lock, so the contents bkp are not racy.
		 *
		 * With buffer_std set to false, XLogFillBkpBlock() sets hole_length
		 * and hole_offset to 0; so the following code is safe for either
		 * case.
		 */
		memcpy(copied_buffer, origdata, bkpb.hole_offset);
		memcpy(copied_buffer + bkpb.hole_offset,
			   origdata + bkpb.hole_offset + bkpb.hole_length,
			   BLCKSZ - bkpb.hole_offset - bkpb.hole_length);

		/*
		 * Header for backup block.
		 */
		rdata[0].data = (char *) &bkpb;
		rdata[0].len = sizeof(BkpBlock);
		rdata[0].buffer = InvalidBuffer;
		rdata[0].next = &(rdata[1]);

		/*
		 * Save copy of the buffer.
		 */
		rdata[1].data = copied_buffer;
		rdata[1].len = BLCKSZ - bkpb.hole_length;
		rdata[1].buffer = InvalidBuffer;
		rdata[1].next = NULL;

		recptr = XLogInsert(RM_XLOG_ID, XLOG_FPI, rdata);
	}

	return recptr;
}

/*
 * Write a WAL record containing a full image of a page. Caller is responsible
 * for writing the page to disk after calling this routine.
 *
 * Note: If you're using this function, you should be building pages in private
 * memory and writing them directly to smgr.  If you're using buffers, call
 * log_newpage_buffer instead.
 *
 * If the page follows the standard page layout, with a PageHeader and unused
 * space between pd_lower and pd_upper, set 'page_std' to TRUE. That allows
 * the unused space to be left out from the WAL record, making it smaller.
 */
XLogRecPtr
log_newpage(RelFileNode *rnode, ForkNumber forkNum, BlockNumber blkno,
			Page page, bool page_std)
{
	BkpBlock	bkpb;
	XLogRecPtr	recptr;
	XLogRecData rdata[3];

	/* NO ELOG(ERROR) from here till newpage op is logged */
	START_CRIT_SECTION();

	bkpb.node = *rnode;
	bkpb.fork = forkNum;
	bkpb.block = blkno;

	if (page_std)
	{
		/* Assume we can omit data between pd_lower and pd_upper */
		uint16		lower = ((PageHeader) page)->pd_lower;
		uint16		upper = ((PageHeader) page)->pd_upper;

		if (lower >= SizeOfPageHeaderData &&
			upper > lower &&
			upper <= BLCKSZ)
		{
			bkpb.hole_offset = lower;
			bkpb.hole_length = upper - lower;
		}
		else
		{
			/* No "hole" to compress out */
			bkpb.hole_offset = 0;
			bkpb.hole_length = 0;
		}
	}
	else
	{
		/* Not a standard page header, don't try to eliminate "hole" */
		bkpb.hole_offset = 0;
		bkpb.hole_length = 0;
	}

	rdata[0].data = (char *) &bkpb;
	rdata[0].len = sizeof(BkpBlock);
	rdata[0].buffer = InvalidBuffer;
	rdata[0].next = &(rdata[1]);

	if (bkpb.hole_length == 0)
	{
		rdata[1].data = (char *) page;
		rdata[1].len = BLCKSZ;
		rdata[1].buffer = InvalidBuffer;
		rdata[1].next = NULL;
	}
	else
	{
		/* must skip the hole */
		rdata[1].data = (char *) page;
		rdata[1].len = bkpb.hole_offset;
		rdata[1].buffer = InvalidBuffer;
		rdata[1].next = &rdata[2];

		rdata[2].data = (char *) page + (bkpb.hole_offset + bkpb.hole_length);
		rdata[2].len = BLCKSZ - (bkpb.hole_offset + bkpb.hole_length);
		rdata[2].buffer = InvalidBuffer;
		rdata[2].next = NULL;
	}

	recptr = XLogInsert(RM_XLOG_ID, XLOG_FPI, rdata);

	/*
	 * The page may be uninitialized. If so, we can't set the LSN because that
	 * would corrupt the page.
	 */
	if (!PageIsNew(page))
	{
		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	return recptr;
}

/*
 * Write a WAL record containing a full image of a page.
 *
 * Caller should initialize the buffer and mark it dirty before calling this
 * function.  This function will set the page LSN.
 *
 * If the page follows the standard page layout, with a PageHeader and unused
 * space between pd_lower and pd_upper, set 'page_std' to TRUE. That allows
 * the unused space to be left out from the WAL record, making it smaller.
 */
XLogRecPtr
log_newpage_buffer(Buffer buffer, bool page_std)
{
	Page		page = BufferGetPage(buffer);
	RelFileNode rnode;
	ForkNumber	forkNum;
	BlockNumber blkno;

	/* Shared buffers should be modified in a critical section. */
	Assert(CritSectionCount > 0);

	BufferGetTag(buffer, &rnode, &forkNum, &blkno);

	return log_newpage(&rnode, forkNum, blkno, page, page_std);
}

/*
 * Fill a BkpBlock for a buffer.
 */
static void
XLogFillBkpBlock(Buffer buffer, bool buffer_std, BkpBlock *bkpb)
{
	BufferGetTag(buffer, &bkpb->node, &bkpb->fork, &bkpb->block);

	if (buffer_std)
	{
		/* Assume we can omit data between pd_lower and pd_upper */
		Page		page = BufferGetPage(buffer);
		uint16		lower = ((PageHeader) page)->pd_lower;
		uint16		upper = ((PageHeader) page)->pd_upper;

		if (lower >= SizeOfPageHeaderData &&
			upper > lower &&
			upper <= BLCKSZ)
		{
			bkpb->hole_offset = lower;
			bkpb->hole_length = upper - lower;
		}
		else
		{
			/* No "hole" to compress out */
			bkpb->hole_offset = 0;
			bkpb->hole_length = 0;
		}
	}
	else
	{
		/* Not a standard page header, don't try to eliminate "hole" */
		bkpb->hole_offset = 0;
		bkpb->hole_length = 0;
	}
}
