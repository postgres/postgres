/*-------------------------------------------------------------------------
 *
 * xlogreader.c
 *		Generic XLog reading facility
 *
 * Portions Copyright (c) 2013-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/backend/access/transam/xlogreader.c
 *
 * NOTES
 *		See xlogreader.h for more notes on this facility.
 *
 *		This file is compiled as both front-end and backend code, so it
 *		may not use ereport, server-defined static variables, etc.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/transam.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "access/xlogrecord.h"
#include "catalog/pg_control.h"
#include "common/pg_lzcompress.h"
#include "replication/origin.h"

#ifndef FRONTEND
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/memutils.h"
#endif

static void report_invalid_record(XLogReaderState *state, const char *fmt,...)
			pg_attribute_printf(2, 3);
static bool allocate_recordbuf(XLogReaderState *state, uint32 reclength);
static int	ReadPageInternal(XLogReaderState *state, XLogRecPtr pageptr,
							 int reqLen);
static void XLogReaderInvalReadState(XLogReaderState *state);
static bool ValidXLogRecordHeader(XLogReaderState *state, XLogRecPtr RecPtr,
								  XLogRecPtr PrevRecPtr, XLogRecord *record, bool randAccess);
static bool ValidXLogRecord(XLogReaderState *state, XLogRecord *record,
							XLogRecPtr recptr);
static void ResetDecoder(XLogReaderState *state);
static void WALOpenSegmentInit(WALOpenSegment *seg, WALSegmentContext *segcxt,
							   int segsize, const char *waldir);

/* size of the buffer allocated for error message. */
#define MAX_ERRORMSG_LEN 1000

/*
 * Construct a string in state->errormsg_buf explaining what's wrong with
 * the current record being read.
 */
static void
report_invalid_record(XLogReaderState *state, const char *fmt,...)
{
	va_list		args;

	fmt = _(fmt);

	va_start(args, fmt);
	vsnprintf(state->errormsg_buf, MAX_ERRORMSG_LEN, fmt, args);
	va_end(args);
}

/*
 * Allocate and initialize a new XLogReader.
 *
 * Returns NULL if the xlogreader couldn't be allocated.
 */
XLogReaderState *
XLogReaderAllocate(int wal_segment_size, const char *waldir,
				   XLogReaderRoutine *routine, void *private_data)
{
	XLogReaderState *state;

	state = (XLogReaderState *)
		palloc_extended(sizeof(XLogReaderState),
						MCXT_ALLOC_NO_OOM | MCXT_ALLOC_ZERO);
	if (!state)
		return NULL;

	/* initialize caller-provided support functions */
	state->routine = *routine;

	state->max_block_id = -1;

	/*
	 * Permanently allocate readBuf.  We do it this way, rather than just
	 * making a static array, for two reasons: (1) no need to waste the
	 * storage in most instantiations of the backend; (2) a static char array
	 * isn't guaranteed to have any particular alignment, whereas
	 * palloc_extended() will provide MAXALIGN'd storage.
	 */
	state->readBuf = (char *) palloc_extended(XLOG_BLCKSZ,
											  MCXT_ALLOC_NO_OOM);
	if (!state->readBuf)
	{
		pfree(state);
		return NULL;
	}

	/* Initialize segment info. */
	WALOpenSegmentInit(&state->seg, &state->segcxt, wal_segment_size,
					   waldir);

	/* system_identifier initialized to zeroes above */
	state->private_data = private_data;
	/* ReadRecPtr, EndRecPtr and readLen initialized to zeroes above */
	state->errormsg_buf = palloc_extended(MAX_ERRORMSG_LEN + 1,
										  MCXT_ALLOC_NO_OOM);
	if (!state->errormsg_buf)
	{
		pfree(state->readBuf);
		pfree(state);
		return NULL;
	}
	state->errormsg_buf[0] = '\0';

	/*
	 * Allocate an initial readRecordBuf of minimal size, which can later be
	 * enlarged if necessary.
	 */
	if (!allocate_recordbuf(state, 0))
	{
		pfree(state->errormsg_buf);
		pfree(state->readBuf);
		pfree(state);
		return NULL;
	}

	return state;
}

void
XLogReaderFree(XLogReaderState *state)
{
	int			block_id;

	if (state->seg.ws_file != -1)
		state->routine.segment_close(state);

	for (block_id = 0; block_id <= XLR_MAX_BLOCK_ID; block_id++)
	{
		if (state->blocks[block_id].data)
			pfree(state->blocks[block_id].data);
	}
	if (state->main_data)
		pfree(state->main_data);

	pfree(state->errormsg_buf);
	if (state->readRecordBuf)
		pfree(state->readRecordBuf);
	pfree(state->readBuf);
	pfree(state);
}

/*
 * Allocate readRecordBuf to fit a record of at least the given length.
 * Returns true if successful, false if out of memory.
 *
 * readRecordBufSize is set to the new buffer size.
 *
 * To avoid useless small increases, round its size to a multiple of
 * XLOG_BLCKSZ, and make sure it's at least 5*Max(BLCKSZ, XLOG_BLCKSZ) to start
 * with.  (That is enough for all "normal" records, but very large commit or
 * abort records might need more space.)
 */
static bool
allocate_recordbuf(XLogReaderState *state, uint32 reclength)
{
	uint32		newSize = reclength;

	newSize += XLOG_BLCKSZ - (newSize % XLOG_BLCKSZ);
	newSize = Max(newSize, 5 * Max(BLCKSZ, XLOG_BLCKSZ));

#ifndef FRONTEND

	/*
	 * Note that in much unlucky circumstances, the random data read from a
	 * recycled segment can cause this routine to be called with a size
	 * causing a hard failure at allocation.  For a standby, this would cause
	 * the instance to stop suddenly with a hard failure, preventing it to
	 * retry fetching WAL from one of its sources which could allow it to move
	 * on with replay without a manual restart. If the data comes from a past
	 * recycled segment and is still valid, then the allocation may succeed
	 * but record checks are going to fail so this would be short-lived.  If
	 * the allocation fails because of a memory shortage, then this is not a
	 * hard failure either per the guarantee given by MCXT_ALLOC_NO_OOM.
	 */
	if (!AllocSizeIsValid(newSize))
		return false;

#endif

	if (state->readRecordBuf)
		pfree(state->readRecordBuf);
	state->readRecordBuf =
		(char *) palloc_extended(newSize, MCXT_ALLOC_NO_OOM);
	if (state->readRecordBuf == NULL)
	{
		state->readRecordBufSize = 0;
		return false;
	}
	state->readRecordBufSize = newSize;
	return true;
}

/*
 * Initialize the passed segment structs.
 */
static void
WALOpenSegmentInit(WALOpenSegment *seg, WALSegmentContext *segcxt,
				   int segsize, const char *waldir)
{
	seg->ws_file = -1;
	seg->ws_segno = 0;
	seg->ws_tli = 0;

	segcxt->ws_segsize = segsize;
	if (waldir)
		snprintf(segcxt->ws_dir, MAXPGPATH, "%s", waldir);
}

/*
 * Begin reading WAL at 'RecPtr'.
 *
 * 'RecPtr' should point to the beginnning of a valid WAL record.  Pointing at
 * the beginning of a page is also OK, if there is a new record right after
 * the page header, i.e. not a continuation.
 *
 * This does not make any attempt to read the WAL yet, and hence cannot fail.
 * If the starting address is not correct, the first call to XLogReadRecord()
 * will error out.
 */
void
XLogBeginRead(XLogReaderState *state, XLogRecPtr RecPtr)
{
	Assert(!XLogRecPtrIsInvalid(RecPtr));

	ResetDecoder(state);

	/* Begin at the passed-in record pointer. */
	state->EndRecPtr = RecPtr;
	state->ReadRecPtr = InvalidXLogRecPtr;
}

/*
 * Attempt to read an XLOG record.
 *
 * XLogBeginRead() or XLogFindNextRecord() must be called before the first call
 * to XLogReadRecord().
 *
 * If the page_read callback fails to read the requested data, NULL is
 * returned.  The callback is expected to have reported the error; errormsg
 * is set to NULL.
 *
 * If the reading fails for some other reason, NULL is also returned, and
 * *errormsg is set to a string with details of the failure.
 *
 * The returned pointer (or *errormsg) points to an internal buffer that's
 * valid until the next call to XLogReadRecord.
 */
XLogRecord *
XLogReadRecord(XLogReaderState *state, char **errormsg)
{
	XLogRecPtr	RecPtr;
	XLogRecord *record;
	XLogRecPtr	targetPagePtr;
	bool		randAccess;
	uint32		len,
				total_len;
	uint32		targetRecOff;
	uint32		pageHeaderSize;
	bool		gotheader;
	int			readOff;

	/*
	 * randAccess indicates whether to verify the previous-record pointer of
	 * the record we're reading.  We only do this if we're reading
	 * sequentially, which is what we initially assume.
	 */
	randAccess = false;

	/* reset error state */
	*errormsg = NULL;
	state->errormsg_buf[0] = '\0';

	ResetDecoder(state);

	RecPtr = state->EndRecPtr;

	if (state->ReadRecPtr != InvalidXLogRecPtr)
	{
		/* read the record after the one we just read */

		/*
		 * EndRecPtr is pointing to end+1 of the previous WAL record.  If
		 * we're at a page boundary, no more records can fit on the current
		 * page. We must skip over the page header, but we can't do that until
		 * we've read in the page, since the header size is variable.
		 */
	}
	else
	{
		/*
		 * Caller supplied a position to start at.
		 *
		 * In this case, EndRecPtr should already be pointing to a valid
		 * record starting position.
		 */
		Assert(XRecOffIsValid(RecPtr));
		randAccess = true;
	}

	state->currRecPtr = RecPtr;

	targetPagePtr = RecPtr - (RecPtr % XLOG_BLCKSZ);
	targetRecOff = RecPtr % XLOG_BLCKSZ;

	/*
	 * Read the page containing the record into state->readBuf. Request enough
	 * byte to cover the whole record header, or at least the part of it that
	 * fits on the same page.
	 */
	readOff = ReadPageInternal(state, targetPagePtr,
							   Min(targetRecOff + SizeOfXLogRecord, XLOG_BLCKSZ));
	if (readOff < 0)
		goto err;

	/*
	 * ReadPageInternal always returns at least the page header, so we can
	 * examine it now.
	 */
	pageHeaderSize = XLogPageHeaderSize((XLogPageHeader) state->readBuf);
	if (targetRecOff == 0)
	{
		/*
		 * At page start, so skip over page header.
		 */
		RecPtr += pageHeaderSize;
		targetRecOff = pageHeaderSize;
	}
	else if (targetRecOff < pageHeaderSize)
	{
		report_invalid_record(state, "invalid record offset at %X/%X",
							  LSN_FORMAT_ARGS(RecPtr));
		goto err;
	}

	if ((((XLogPageHeader) state->readBuf)->xlp_info & XLP_FIRST_IS_CONTRECORD) &&
		targetRecOff == pageHeaderSize)
	{
		report_invalid_record(state, "contrecord is requested by %X/%X",
							  LSN_FORMAT_ARGS(RecPtr));
		goto err;
	}

	/* ReadPageInternal has verified the page header */
	Assert(pageHeaderSize <= readOff);

	/*
	 * Read the record length.
	 *
	 * NB: Even though we use an XLogRecord pointer here, the whole record
	 * header might not fit on this page. xl_tot_len is the first field of the
	 * struct, so it must be on this page (the records are MAXALIGNed), but we
	 * cannot access any other fields until we've verified that we got the
	 * whole header.
	 */
	record = (XLogRecord *) (state->readBuf + RecPtr % XLOG_BLCKSZ);
	total_len = record->xl_tot_len;

	/*
	 * If the whole record header is on this page, validate it immediately.
	 * Otherwise do just a basic sanity check on xl_tot_len, and validate the
	 * rest of the header after reading it from the next page.  The xl_tot_len
	 * check is necessary here to ensure that we enter the "Need to reassemble
	 * record" code path below; otherwise we might fail to apply
	 * ValidXLogRecordHeader at all.
	 */
	if (targetRecOff <= XLOG_BLCKSZ - SizeOfXLogRecord)
	{
		if (!ValidXLogRecordHeader(state, RecPtr, state->ReadRecPtr, record,
								   randAccess))
			goto err;
		gotheader = true;
	}
	else
	{
		/* XXX: more validation should be done here */
		if (total_len < SizeOfXLogRecord)
		{
			report_invalid_record(state,
								  "invalid record length at %X/%X: wanted %u, got %u",
								  LSN_FORMAT_ARGS(RecPtr),
								  (uint32) SizeOfXLogRecord, total_len);
			goto err;
		}
		gotheader = false;
	}

	len = XLOG_BLCKSZ - RecPtr % XLOG_BLCKSZ;
	if (total_len > len)
	{
		/* Need to reassemble record */
		char	   *contdata;
		XLogPageHeader pageHeader;
		char	   *buffer;
		uint32		gotlen;

		/*
		 * Enlarge readRecordBuf as needed.
		 */
		if (total_len > state->readRecordBufSize &&
			!allocate_recordbuf(state, total_len))
		{
			/* We treat this as a "bogus data" condition */
			report_invalid_record(state, "record length %u at %X/%X too long",
								  total_len, LSN_FORMAT_ARGS(RecPtr));
			goto err;
		}

		/* Copy the first fragment of the record from the first page. */
		memcpy(state->readRecordBuf,
			   state->readBuf + RecPtr % XLOG_BLCKSZ, len);
		buffer = state->readRecordBuf + len;
		gotlen = len;

		do
		{
			/* Calculate pointer to beginning of next page */
			targetPagePtr += XLOG_BLCKSZ;

			/* Wait for the next page to become available */
			readOff = ReadPageInternal(state, targetPagePtr,
									   Min(total_len - gotlen + SizeOfXLogShortPHD,
										   XLOG_BLCKSZ));

			if (readOff < 0)
				goto err;

			Assert(SizeOfXLogShortPHD <= readOff);

			/* Check that the continuation on next page looks valid */
			pageHeader = (XLogPageHeader) state->readBuf;
			if (!(pageHeader->xlp_info & XLP_FIRST_IS_CONTRECORD))
			{
				report_invalid_record(state,
									  "there is no contrecord flag at %X/%X",
									  LSN_FORMAT_ARGS(RecPtr));
				goto err;
			}

			/*
			 * Cross-check that xlp_rem_len agrees with how much of the record
			 * we expect there to be left.
			 */
			if (pageHeader->xlp_rem_len == 0 ||
				total_len != (pageHeader->xlp_rem_len + gotlen))
			{
				report_invalid_record(state,
									  "invalid contrecord length %u (expected %lld) at %X/%X",
									  pageHeader->xlp_rem_len,
									  ((long long) total_len) - gotlen,
									  LSN_FORMAT_ARGS(RecPtr));
				goto err;
			}

			/* Append the continuation from this page to the buffer */
			pageHeaderSize = XLogPageHeaderSize(pageHeader);

			if (readOff < pageHeaderSize)
				readOff = ReadPageInternal(state, targetPagePtr,
										   pageHeaderSize);

			Assert(pageHeaderSize <= readOff);

			contdata = (char *) state->readBuf + pageHeaderSize;
			len = XLOG_BLCKSZ - pageHeaderSize;
			if (pageHeader->xlp_rem_len < len)
				len = pageHeader->xlp_rem_len;

			if (readOff < pageHeaderSize + len)
				readOff = ReadPageInternal(state, targetPagePtr,
										   pageHeaderSize + len);

			memcpy(buffer, (char *) contdata, len);
			buffer += len;
			gotlen += len;

			/* If we just reassembled the record header, validate it. */
			if (!gotheader)
			{
				record = (XLogRecord *) state->readRecordBuf;
				if (!ValidXLogRecordHeader(state, RecPtr, state->ReadRecPtr,
										   record, randAccess))
					goto err;
				gotheader = true;
			}
		} while (gotlen < total_len);

		Assert(gotheader);

		record = (XLogRecord *) state->readRecordBuf;
		if (!ValidXLogRecord(state, record, RecPtr))
			goto err;

		pageHeaderSize = XLogPageHeaderSize((XLogPageHeader) state->readBuf);
		state->ReadRecPtr = RecPtr;
		state->EndRecPtr = targetPagePtr + pageHeaderSize
			+ MAXALIGN(pageHeader->xlp_rem_len);
	}
	else
	{
		/* Wait for the record data to become available */
		readOff = ReadPageInternal(state, targetPagePtr,
								   Min(targetRecOff + total_len, XLOG_BLCKSZ));
		if (readOff < 0)
			goto err;

		/* Record does not cross a page boundary */
		if (!ValidXLogRecord(state, record, RecPtr))
			goto err;

		state->EndRecPtr = RecPtr + MAXALIGN(total_len);

		state->ReadRecPtr = RecPtr;
	}

	/*
	 * Special processing if it's an XLOG SWITCH record
	 */
	if (record->xl_rmid == RM_XLOG_ID &&
		(record->xl_info & ~XLR_INFO_MASK) == XLOG_SWITCH)
	{
		/* Pretend it extends to end of segment */
		state->EndRecPtr += state->segcxt.ws_segsize - 1;
		state->EndRecPtr -= XLogSegmentOffset(state->EndRecPtr, state->segcxt.ws_segsize);
	}

	if (DecodeXLogRecord(state, record, errormsg))
		return record;
	else
		return NULL;

err:

	/*
	 * Invalidate the read state. We might read from a different source after
	 * failure.
	 */
	XLogReaderInvalReadState(state);

	if (state->errormsg_buf[0] != '\0')
		*errormsg = state->errormsg_buf;

	return NULL;
}

/*
 * Read a single xlog page including at least [pageptr, reqLen] of valid data
 * via the page_read() callback.
 *
 * Returns -1 if the required page cannot be read for some reason; errormsg_buf
 * is set in that case (unless the error occurs in the page_read callback).
 *
 * We fetch the page from a reader-local cache if we know we have the required
 * data and if there hasn't been any error since caching the data.
 */
static int
ReadPageInternal(XLogReaderState *state, XLogRecPtr pageptr, int reqLen)
{
	int			readLen;
	uint32		targetPageOff;
	XLogSegNo	targetSegNo;
	XLogPageHeader hdr;

	Assert((pageptr % XLOG_BLCKSZ) == 0);

	XLByteToSeg(pageptr, targetSegNo, state->segcxt.ws_segsize);
	targetPageOff = XLogSegmentOffset(pageptr, state->segcxt.ws_segsize);

	/* check whether we have all the requested data already */
	if (targetSegNo == state->seg.ws_segno &&
		targetPageOff == state->segoff && reqLen <= state->readLen)
		return state->readLen;

	/*
	 * Data is not in our buffer.
	 *
	 * Every time we actually read the segment, even if we looked at parts of
	 * it before, we need to do verification as the page_read callback might
	 * now be rereading data from a different source.
	 *
	 * Whenever switching to a new WAL segment, we read the first page of the
	 * file and validate its header, even if that's not where the target
	 * record is.  This is so that we can check the additional identification
	 * info that is present in the first page's "long" header.
	 */
	if (targetSegNo != state->seg.ws_segno && targetPageOff != 0)
	{
		XLogRecPtr	targetSegmentPtr = pageptr - targetPageOff;

		readLen = state->routine.page_read(state, targetSegmentPtr, XLOG_BLCKSZ,
										   state->currRecPtr,
										   state->readBuf);
		if (readLen < 0)
			goto err;

		/* we can be sure to have enough WAL available, we scrolled back */
		Assert(readLen == XLOG_BLCKSZ);

		if (!XLogReaderValidatePageHeader(state, targetSegmentPtr,
										  state->readBuf))
			goto err;
	}

	/*
	 * First, read the requested data length, but at least a short page header
	 * so that we can validate it.
	 */
	readLen = state->routine.page_read(state, pageptr, Max(reqLen, SizeOfXLogShortPHD),
									   state->currRecPtr,
									   state->readBuf);
	if (readLen < 0)
		goto err;

	Assert(readLen <= XLOG_BLCKSZ);

	/* Do we have enough data to check the header length? */
	if (readLen <= SizeOfXLogShortPHD)
		goto err;

	Assert(readLen >= reqLen);

	hdr = (XLogPageHeader) state->readBuf;

	/* still not enough */
	if (readLen < XLogPageHeaderSize(hdr))
	{
		readLen = state->routine.page_read(state, pageptr, XLogPageHeaderSize(hdr),
										   state->currRecPtr,
										   state->readBuf);
		if (readLen < 0)
			goto err;
	}

	/*
	 * Now that we know we have the full header, validate it.
	 */
	if (!XLogReaderValidatePageHeader(state, pageptr, (char *) hdr))
		goto err;

	/* update read state information */
	state->seg.ws_segno = targetSegNo;
	state->segoff = targetPageOff;
	state->readLen = readLen;

	return readLen;

err:
	XLogReaderInvalReadState(state);
	return -1;
}

/*
 * Invalidate the xlogreader's read state to force a re-read.
 */
static void
XLogReaderInvalReadState(XLogReaderState *state)
{
	state->seg.ws_segno = 0;
	state->segoff = 0;
	state->readLen = 0;
}

/*
 * Validate an XLOG record header.
 *
 * This is just a convenience subroutine to avoid duplicated code in
 * XLogReadRecord.  It's not intended for use from anywhere else.
 */
static bool
ValidXLogRecordHeader(XLogReaderState *state, XLogRecPtr RecPtr,
					  XLogRecPtr PrevRecPtr, XLogRecord *record,
					  bool randAccess)
{
	if (record->xl_tot_len < SizeOfXLogRecord)
	{
		report_invalid_record(state,
							  "invalid record length at %X/%X: wanted %u, got %u",
							  LSN_FORMAT_ARGS(RecPtr),
							  (uint32) SizeOfXLogRecord, record->xl_tot_len);
		return false;
	}
	if (record->xl_rmid > RM_MAX_ID)
	{
		report_invalid_record(state,
							  "invalid resource manager ID %u at %X/%X",
							  record->xl_rmid, LSN_FORMAT_ARGS(RecPtr));
		return false;
	}
	if (randAccess)
	{
		/*
		 * We can't exactly verify the prev-link, but surely it should be less
		 * than the record's own address.
		 */
		if (!(record->xl_prev < RecPtr))
		{
			report_invalid_record(state,
								  "record with incorrect prev-link %X/%X at %X/%X",
								  LSN_FORMAT_ARGS(record->xl_prev),
								  LSN_FORMAT_ARGS(RecPtr));
			return false;
		}
	}
	else
	{
		/*
		 * Record's prev-link should exactly match our previous location. This
		 * check guards against torn WAL pages where a stale but valid-looking
		 * WAL record starts on a sector boundary.
		 */
		if (record->xl_prev != PrevRecPtr)
		{
			report_invalid_record(state,
								  "record with incorrect prev-link %X/%X at %X/%X",
								  LSN_FORMAT_ARGS(record->xl_prev),
								  LSN_FORMAT_ARGS(RecPtr));
			return false;
		}
	}

	return true;
}


/*
 * CRC-check an XLOG record.  We do not believe the contents of an XLOG
 * record (other than to the minimal extent of computing the amount of
 * data to read in) until we've checked the CRCs.
 *
 * We assume all of the record (that is, xl_tot_len bytes) has been read
 * into memory at *record.  Also, ValidXLogRecordHeader() has accepted the
 * record's header, which means in particular that xl_tot_len is at least
 * SizeOfXLogRecord.
 */
static bool
ValidXLogRecord(XLogReaderState *state, XLogRecord *record, XLogRecPtr recptr)
{
	pg_crc32c	crc;

	/* Calculate the CRC */
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, ((char *) record) + SizeOfXLogRecord, record->xl_tot_len - SizeOfXLogRecord);
	/* include the record header last */
	COMP_CRC32C(crc, (char *) record, offsetof(XLogRecord, xl_crc));
	FIN_CRC32C(crc);

	if (!EQ_CRC32C(record->xl_crc, crc))
	{
		report_invalid_record(state,
							  "incorrect resource manager data checksum in record at %X/%X",
							  LSN_FORMAT_ARGS(recptr));
		return false;
	}

	return true;
}

/*
 * Validate a page header.
 *
 * Check if 'phdr' is valid as the header of the XLog page at position
 * 'recptr'.
 */
bool
XLogReaderValidatePageHeader(XLogReaderState *state, XLogRecPtr recptr,
							 char *phdr)
{
	XLogRecPtr	recaddr;
	XLogSegNo	segno;
	int32		offset;
	XLogPageHeader hdr = (XLogPageHeader) phdr;

	Assert((recptr % XLOG_BLCKSZ) == 0);

	XLByteToSeg(recptr, segno, state->segcxt.ws_segsize);
	offset = XLogSegmentOffset(recptr, state->segcxt.ws_segsize);

	XLogSegNoOffsetToRecPtr(segno, offset, state->segcxt.ws_segsize, recaddr);

	if (hdr->xlp_magic != XLOG_PAGE_MAGIC)
	{
		char		fname[MAXFNAMELEN];

		XLogFileName(fname, state->seg.ws_tli, segno, state->segcxt.ws_segsize);

		report_invalid_record(state,
							  "invalid magic number %04X in log segment %s, offset %u",
							  hdr->xlp_magic,
							  fname,
							  offset);
		return false;
	}

	if ((hdr->xlp_info & ~XLP_ALL_FLAGS) != 0)
	{
		char		fname[MAXFNAMELEN];

		XLogFileName(fname, state->seg.ws_tli, segno, state->segcxt.ws_segsize);

		report_invalid_record(state,
							  "invalid info bits %04X in log segment %s, offset %u",
							  hdr->xlp_info,
							  fname,
							  offset);
		return false;
	}

	if (hdr->xlp_info & XLP_LONG_HEADER)
	{
		XLogLongPageHeader longhdr = (XLogLongPageHeader) hdr;

		if (state->system_identifier &&
			longhdr->xlp_sysid != state->system_identifier)
		{
			report_invalid_record(state,
								  "WAL file is from different database system: WAL file database system identifier is %llu, pg_control database system identifier is %llu",
								  (unsigned long long) longhdr->xlp_sysid,
								  (unsigned long long) state->system_identifier);
			return false;
		}
		else if (longhdr->xlp_seg_size != state->segcxt.ws_segsize)
		{
			report_invalid_record(state,
								  "WAL file is from different database system: incorrect segment size in page header");
			return false;
		}
		else if (longhdr->xlp_xlog_blcksz != XLOG_BLCKSZ)
		{
			report_invalid_record(state,
								  "WAL file is from different database system: incorrect XLOG_BLCKSZ in page header");
			return false;
		}
	}
	else if (offset == 0)
	{
		char		fname[MAXFNAMELEN];

		XLogFileName(fname, state->seg.ws_tli, segno, state->segcxt.ws_segsize);

		/* hmm, first page of file doesn't have a long header? */
		report_invalid_record(state,
							  "invalid info bits %04X in log segment %s, offset %u",
							  hdr->xlp_info,
							  fname,
							  offset);
		return false;
	}

	/*
	 * Check that the address on the page agrees with what we expected. This
	 * check typically fails when an old WAL segment is recycled, and hasn't
	 * yet been overwritten with new data yet.
	 */
	if (hdr->xlp_pageaddr != recaddr)
	{
		char		fname[MAXFNAMELEN];

		XLogFileName(fname, state->seg.ws_tli, segno, state->segcxt.ws_segsize);

		report_invalid_record(state,
							  "unexpected pageaddr %X/%X in log segment %s, offset %u",
							  LSN_FORMAT_ARGS(hdr->xlp_pageaddr),
							  fname,
							  offset);
		return false;
	}

	/*
	 * Since child timelines are always assigned a TLI greater than their
	 * immediate parent's TLI, we should never see TLI go backwards across
	 * successive pages of a consistent WAL sequence.
	 *
	 * Sometimes we re-read a segment that's already been (partially) read. So
	 * we only verify TLIs for pages that are later than the last remembered
	 * LSN.
	 */
	if (recptr > state->latestPagePtr)
	{
		if (hdr->xlp_tli < state->latestPageTLI)
		{
			char		fname[MAXFNAMELEN];

			XLogFileName(fname, state->seg.ws_tli, segno, state->segcxt.ws_segsize);

			report_invalid_record(state,
								  "out-of-sequence timeline ID %u (after %u) in log segment %s, offset %u",
								  hdr->xlp_tli,
								  state->latestPageTLI,
								  fname,
								  offset);
			return false;
		}
	}
	state->latestPagePtr = recptr;
	state->latestPageTLI = hdr->xlp_tli;

	return true;
}

#ifdef FRONTEND
/*
 * Functions that are currently not needed in the backend, but are better
 * implemented inside xlogreader.c because of the internal facilities available
 * here.
 */

/*
 * Find the first record with an lsn >= RecPtr.
 *
 * This is different from XLogBeginRead() in that RecPtr doesn't need to point
 * to a valid record boundary.  Useful for checking whether RecPtr is a valid
 * xlog address for reading, and to find the first valid address after some
 * address when dumping records for debugging purposes.
 *
 * This positions the reader, like XLogBeginRead(), so that the next call to
 * XLogReadRecord() will read the next valid record.
 */
XLogRecPtr
XLogFindNextRecord(XLogReaderState *state, XLogRecPtr RecPtr)
{
	XLogRecPtr	tmpRecPtr;
	XLogRecPtr	found = InvalidXLogRecPtr;
	XLogPageHeader header;
	char	   *errormsg;

	Assert(!XLogRecPtrIsInvalid(RecPtr));

	/*
	 * skip over potential continuation data, keeping in mind that it may span
	 * multiple pages
	 */
	tmpRecPtr = RecPtr;
	while (true)
	{
		XLogRecPtr	targetPagePtr;
		int			targetRecOff;
		uint32		pageHeaderSize;
		int			readLen;

		/*
		 * Compute targetRecOff. It should typically be equal or greater than
		 * short page-header since a valid record can't start anywhere before
		 * that, except when caller has explicitly specified the offset that
		 * falls somewhere there or when we are skipping multi-page
		 * continuation record. It doesn't matter though because
		 * ReadPageInternal() is prepared to handle that and will read at
		 * least short page-header worth of data
		 */
		targetRecOff = tmpRecPtr % XLOG_BLCKSZ;

		/* scroll back to page boundary */
		targetPagePtr = tmpRecPtr - targetRecOff;

		/* Read the page containing the record */
		readLen = ReadPageInternal(state, targetPagePtr, targetRecOff);
		if (readLen < 0)
			goto err;

		header = (XLogPageHeader) state->readBuf;

		pageHeaderSize = XLogPageHeaderSize(header);

		/* make sure we have enough data for the page header */
		readLen = ReadPageInternal(state, targetPagePtr, pageHeaderSize);
		if (readLen < 0)
			goto err;

		/* skip over potential continuation data */
		if (header->xlp_info & XLP_FIRST_IS_CONTRECORD)
		{
			/*
			 * If the length of the remaining continuation data is more than
			 * what can fit in this page, the continuation record crosses over
			 * this page. Read the next page and try again. xlp_rem_len in the
			 * next page header will contain the remaining length of the
			 * continuation data
			 *
			 * Note that record headers are MAXALIGN'ed
			 */
			if (MAXALIGN(header->xlp_rem_len) >= (XLOG_BLCKSZ - pageHeaderSize))
				tmpRecPtr = targetPagePtr + XLOG_BLCKSZ;
			else
			{
				/*
				 * The previous continuation record ends in this page. Set
				 * tmpRecPtr to point to the first valid record
				 */
				tmpRecPtr = targetPagePtr + pageHeaderSize
					+ MAXALIGN(header->xlp_rem_len);
				break;
			}
		}
		else
		{
			tmpRecPtr = targetPagePtr + pageHeaderSize;
			break;
		}
	}

	/*
	 * we know now that tmpRecPtr is an address pointing to a valid XLogRecord
	 * because either we're at the first record after the beginning of a page
	 * or we just jumped over the remaining data of a continuation.
	 */
	XLogBeginRead(state, tmpRecPtr);
	while (XLogReadRecord(state, &errormsg) != NULL)
	{
		/* past the record we've found, break out */
		if (RecPtr <= state->ReadRecPtr)
		{
			/* Rewind the reader to the beginning of the last record. */
			found = state->ReadRecPtr;
			XLogBeginRead(state, found);
			return found;
		}
	}

err:
	XLogReaderInvalReadState(state);

	return InvalidXLogRecPtr;
}

#endif							/* FRONTEND */

/*
 * Helper function to ease writing of XLogRoutine->page_read callbacks.
 * If this function is used, caller must supply a segment_open callback in
 * 'state', as that is used here.
 *
 * Read 'count' bytes into 'buf', starting at location 'startptr', from WAL
 * fetched from timeline 'tli'.
 *
 * Returns true if succeeded, false if an error occurs, in which case
 * 'errinfo' receives error details.
 *
 * XXX probably this should be improved to suck data directly from the
 * WAL buffers when possible.
 */
bool
WALRead(XLogReaderState *state,
		char *buf, XLogRecPtr startptr, Size count, TimeLineID tli,
		WALReadError *errinfo)
{
	char	   *p;
	XLogRecPtr	recptr;
	Size		nbytes;

	p = buf;
	recptr = startptr;
	nbytes = count;

	while (nbytes > 0)
	{
		uint32		startoff;
		int			segbytes;
		int			readbytes;

		startoff = XLogSegmentOffset(recptr, state->segcxt.ws_segsize);

		/*
		 * If the data we want is not in a segment we have open, close what we
		 * have (if anything) and open the next one, using the caller's
		 * provided openSegment callback.
		 */
		if (state->seg.ws_file < 0 ||
			!XLByteInSeg(recptr, state->seg.ws_segno, state->segcxt.ws_segsize) ||
			tli != state->seg.ws_tli)
		{
			XLogSegNo	nextSegNo;

			if (state->seg.ws_file >= 0)
				state->routine.segment_close(state);

			XLByteToSeg(recptr, nextSegNo, state->segcxt.ws_segsize);
			state->routine.segment_open(state, nextSegNo, &tli);

			/* This shouldn't happen -- indicates a bug in segment_open */
			Assert(state->seg.ws_file >= 0);

			/* Update the current segment info. */
			state->seg.ws_tli = tli;
			state->seg.ws_segno = nextSegNo;
		}

		/* How many bytes are within this segment? */
		if (nbytes > (state->segcxt.ws_segsize - startoff))
			segbytes = state->segcxt.ws_segsize - startoff;
		else
			segbytes = nbytes;

#ifndef FRONTEND
		pgstat_report_wait_start(WAIT_EVENT_WAL_READ);
#endif

		/* Reset errno first; eases reporting non-errno-affecting errors */
		errno = 0;
		readbytes = pg_pread(state->seg.ws_file, p, segbytes, (off_t) startoff);

#ifndef FRONTEND
		pgstat_report_wait_end();
#endif

		if (readbytes <= 0)
		{
			errinfo->wre_errno = errno;
			errinfo->wre_req = segbytes;
			errinfo->wre_read = readbytes;
			errinfo->wre_off = startoff;
			errinfo->wre_seg = state->seg;
			return false;
		}

		/* Update state for read */
		recptr += readbytes;
		nbytes -= readbytes;
		p += readbytes;
	}

	return true;
}

/* ----------------------------------------
 * Functions for decoding the data and block references in a record.
 * ----------------------------------------
 */

/* private function to reset the state between records */
static void
ResetDecoder(XLogReaderState *state)
{
	int			block_id;

	state->decoded_record = NULL;

	state->main_data_len = 0;

	for (block_id = 0; block_id <= state->max_block_id; block_id++)
	{
		state->blocks[block_id].in_use = false;
		state->blocks[block_id].has_image = false;
		state->blocks[block_id].has_data = false;
		state->blocks[block_id].apply_image = false;
	}
	state->max_block_id = -1;
}

/*
 * Decode the previously read record.
 *
 * On error, a human-readable error message is returned in *errormsg, and
 * the return value is false.
 */
bool
DecodeXLogRecord(XLogReaderState *state, XLogRecord *record, char **errormsg)
{
	/*
	 * read next _size bytes from record buffer, but check for overrun first.
	 */
#define COPY_HEADER_FIELD(_dst, _size)			\
	do {										\
		if (remaining < _size)					\
			goto shortdata_err;					\
		memcpy(_dst, ptr, _size);				\
		ptr += _size;							\
		remaining -= _size;						\
	} while(0)

	char	   *ptr;
	uint32		remaining;
	uint32		datatotal;
	RelFileNode *rnode = NULL;
	uint8		block_id;

	ResetDecoder(state);

	state->decoded_record = record;
	state->record_origin = InvalidRepOriginId;
	state->toplevel_xid = InvalidTransactionId;

	ptr = (char *) record;
	ptr += SizeOfXLogRecord;
	remaining = record->xl_tot_len - SizeOfXLogRecord;

	/* Decode the headers */
	datatotal = 0;
	while (remaining > datatotal)
	{
		COPY_HEADER_FIELD(&block_id, sizeof(uint8));

		if (block_id == XLR_BLOCK_ID_DATA_SHORT)
		{
			/* XLogRecordDataHeaderShort */
			uint8		main_data_len;

			COPY_HEADER_FIELD(&main_data_len, sizeof(uint8));

			state->main_data_len = main_data_len;
			datatotal += main_data_len;
			break;				/* by convention, the main data fragment is
								 * always last */
		}
		else if (block_id == XLR_BLOCK_ID_DATA_LONG)
		{
			/* XLogRecordDataHeaderLong */
			uint32		main_data_len;

			COPY_HEADER_FIELD(&main_data_len, sizeof(uint32));
			state->main_data_len = main_data_len;
			datatotal += main_data_len;
			break;				/* by convention, the main data fragment is
								 * always last */
		}
		else if (block_id == XLR_BLOCK_ID_ORIGIN)
		{
			COPY_HEADER_FIELD(&state->record_origin, sizeof(RepOriginId));
		}
		else if (block_id == XLR_BLOCK_ID_TOPLEVEL_XID)
		{
			COPY_HEADER_FIELD(&state->toplevel_xid, sizeof(TransactionId));
		}
		else if (block_id <= XLR_MAX_BLOCK_ID)
		{
			/* XLogRecordBlockHeader */
			DecodedBkpBlock *blk;
			uint8		fork_flags;

			if (block_id <= state->max_block_id)
			{
				report_invalid_record(state,
									  "out-of-order block_id %u at %X/%X",
									  block_id,
									  LSN_FORMAT_ARGS(state->ReadRecPtr));
				goto err;
			}
			state->max_block_id = block_id;

			blk = &state->blocks[block_id];
			blk->in_use = true;
			blk->apply_image = false;

			COPY_HEADER_FIELD(&fork_flags, sizeof(uint8));
			blk->forknum = fork_flags & BKPBLOCK_FORK_MASK;
			blk->flags = fork_flags;
			blk->has_image = ((fork_flags & BKPBLOCK_HAS_IMAGE) != 0);
			blk->has_data = ((fork_flags & BKPBLOCK_HAS_DATA) != 0);

			COPY_HEADER_FIELD(&blk->data_len, sizeof(uint16));
			/* cross-check that the HAS_DATA flag is set iff data_length > 0 */
			if (blk->has_data && blk->data_len == 0)
			{
				report_invalid_record(state,
									  "BKPBLOCK_HAS_DATA set, but no data included at %X/%X",
									  LSN_FORMAT_ARGS(state->ReadRecPtr));
				goto err;
			}
			if (!blk->has_data && blk->data_len != 0)
			{
				report_invalid_record(state,
									  "BKPBLOCK_HAS_DATA not set, but data length is %u at %X/%X",
									  (unsigned int) blk->data_len,
									  LSN_FORMAT_ARGS(state->ReadRecPtr));
				goto err;
			}
			datatotal += blk->data_len;

			if (blk->has_image)
			{
				COPY_HEADER_FIELD(&blk->bimg_len, sizeof(uint16));
				COPY_HEADER_FIELD(&blk->hole_offset, sizeof(uint16));
				COPY_HEADER_FIELD(&blk->bimg_info, sizeof(uint8));

				blk->apply_image = ((blk->bimg_info & BKPIMAGE_APPLY) != 0);

				if (blk->bimg_info & BKPIMAGE_IS_COMPRESSED)
				{
					if (blk->bimg_info & BKPIMAGE_HAS_HOLE)
						COPY_HEADER_FIELD(&blk->hole_length, sizeof(uint16));
					else
						blk->hole_length = 0;
				}
				else
					blk->hole_length = BLCKSZ - blk->bimg_len;
				datatotal += blk->bimg_len;

				/*
				 * cross-check that hole_offset > 0, hole_length > 0 and
				 * bimg_len < BLCKSZ if the HAS_HOLE flag is set.
				 */
				if ((blk->bimg_info & BKPIMAGE_HAS_HOLE) &&
					(blk->hole_offset == 0 ||
					 blk->hole_length == 0 ||
					 blk->bimg_len == BLCKSZ))
				{
					report_invalid_record(state,
										  "BKPIMAGE_HAS_HOLE set, but hole offset %u length %u block image length %u at %X/%X",
										  (unsigned int) blk->hole_offset,
										  (unsigned int) blk->hole_length,
										  (unsigned int) blk->bimg_len,
										  LSN_FORMAT_ARGS(state->ReadRecPtr));
					goto err;
				}

				/*
				 * cross-check that hole_offset == 0 and hole_length == 0 if
				 * the HAS_HOLE flag is not set.
				 */
				if (!(blk->bimg_info & BKPIMAGE_HAS_HOLE) &&
					(blk->hole_offset != 0 || blk->hole_length != 0))
				{
					report_invalid_record(state,
										  "BKPIMAGE_HAS_HOLE not set, but hole offset %u length %u at %X/%X",
										  (unsigned int) blk->hole_offset,
										  (unsigned int) blk->hole_length,
										  LSN_FORMAT_ARGS(state->ReadRecPtr));
					goto err;
				}

				/*
				 * cross-check that bimg_len < BLCKSZ if the IS_COMPRESSED
				 * flag is set.
				 */
				if ((blk->bimg_info & BKPIMAGE_IS_COMPRESSED) &&
					blk->bimg_len == BLCKSZ)
				{
					report_invalid_record(state,
										  "BKPIMAGE_IS_COMPRESSED set, but block image length %u at %X/%X",
										  (unsigned int) blk->bimg_len,
										  LSN_FORMAT_ARGS(state->ReadRecPtr));
					goto err;
				}

				/*
				 * cross-check that bimg_len = BLCKSZ if neither HAS_HOLE nor
				 * IS_COMPRESSED flag is set.
				 */
				if (!(blk->bimg_info & BKPIMAGE_HAS_HOLE) &&
					!(blk->bimg_info & BKPIMAGE_IS_COMPRESSED) &&
					blk->bimg_len != BLCKSZ)
				{
					report_invalid_record(state,
										  "neither BKPIMAGE_HAS_HOLE nor BKPIMAGE_IS_COMPRESSED set, but block image length is %u at %X/%X",
										  (unsigned int) blk->data_len,
										  LSN_FORMAT_ARGS(state->ReadRecPtr));
					goto err;
				}
			}
			if (!(fork_flags & BKPBLOCK_SAME_REL))
			{
				COPY_HEADER_FIELD(&blk->rnode, sizeof(RelFileNode));
				rnode = &blk->rnode;
			}
			else
			{
				if (rnode == NULL)
				{
					report_invalid_record(state,
										  "BKPBLOCK_SAME_REL set but no previous rel at %X/%X",
										  LSN_FORMAT_ARGS(state->ReadRecPtr));
					goto err;
				}

				blk->rnode = *rnode;
			}
			COPY_HEADER_FIELD(&blk->blkno, sizeof(BlockNumber));
		}
		else
		{
			report_invalid_record(state,
								  "invalid block_id %u at %X/%X",
								  block_id, LSN_FORMAT_ARGS(state->ReadRecPtr));
			goto err;
		}
	}

	if (remaining != datatotal)
		goto shortdata_err;

	/*
	 * Ok, we've parsed the fragment headers, and verified that the total
	 * length of the payload in the fragments is equal to the amount of data
	 * left. Copy the data of each fragment to a separate buffer.
	 *
	 * We could just set up pointers into readRecordBuf, but we want to align
	 * the data for the convenience of the callers. Backup images are not
	 * copied, however; they don't need alignment.
	 */

	/* block data first */
	for (block_id = 0; block_id <= state->max_block_id; block_id++)
	{
		DecodedBkpBlock *blk = &state->blocks[block_id];

		if (!blk->in_use)
			continue;

		Assert(blk->has_image || !blk->apply_image);

		if (blk->has_image)
		{
			blk->bkp_image = ptr;
			ptr += blk->bimg_len;
		}
		if (blk->has_data)
		{
			if (!blk->data || blk->data_len > blk->data_bufsz)
			{
				if (blk->data)
					pfree(blk->data);

				/*
				 * Force the initial request to be BLCKSZ so that we don't
				 * waste time with lots of trips through this stanza as a
				 * result of WAL compression.
				 */
				blk->data_bufsz = MAXALIGN(Max(blk->data_len, BLCKSZ));
				blk->data = palloc(blk->data_bufsz);
			}
			memcpy(blk->data, ptr, blk->data_len);
			ptr += blk->data_len;
		}
	}

	/* and finally, the main data */
	if (state->main_data_len > 0)
	{
		if (!state->main_data || state->main_data_len > state->main_data_bufsz)
		{
			if (state->main_data)
				pfree(state->main_data);

			/*
			 * main_data_bufsz must be MAXALIGN'ed.  In many xlog record
			 * types, we omit trailing struct padding on-disk to save a few
			 * bytes; but compilers may generate accesses to the xlog struct
			 * that assume that padding bytes are present.  If the palloc
			 * request is not large enough to include such padding bytes then
			 * we'll get valgrind complaints due to otherwise-harmless fetches
			 * of the padding bytes.
			 *
			 * In addition, force the initial request to be reasonably large
			 * so that we don't waste time with lots of trips through this
			 * stanza.  BLCKSZ / 2 seems like a good compromise choice.
			 */
			state->main_data_bufsz = MAXALIGN(Max(state->main_data_len,
												  BLCKSZ / 2));
			state->main_data = palloc(state->main_data_bufsz);
		}
		memcpy(state->main_data, ptr, state->main_data_len);
		ptr += state->main_data_len;
	}

	return true;

shortdata_err:
	report_invalid_record(state,
						  "record with invalid length at %X/%X",
						  LSN_FORMAT_ARGS(state->ReadRecPtr));
err:
	*errormsg = state->errormsg_buf;

	return false;
}

/*
 * Returns information about the block that a block reference refers to.
 *
 * If the WAL record contains a block reference with the given ID, *rnode,
 * *forknum, and *blknum are filled in (if not NULL), and returns true.
 * Otherwise returns false.
 */
bool
XLogRecGetBlockTag(XLogReaderState *record, uint8 block_id,
				   RelFileNode *rnode, ForkNumber *forknum, BlockNumber *blknum)
{
	DecodedBkpBlock *bkpb;

	if (!record->blocks[block_id].in_use)
		return false;

	bkpb = &record->blocks[block_id];
	if (rnode)
		*rnode = bkpb->rnode;
	if (forknum)
		*forknum = bkpb->forknum;
	if (blknum)
		*blknum = bkpb->blkno;
	return true;
}

/*
 * Returns the data associated with a block reference, or NULL if there is
 * no data (e.g. because a full-page image was taken instead). The returned
 * pointer points to a MAXALIGNed buffer.
 */
char *
XLogRecGetBlockData(XLogReaderState *record, uint8 block_id, Size *len)
{
	DecodedBkpBlock *bkpb;

	if (!record->blocks[block_id].in_use)
		return NULL;

	bkpb = &record->blocks[block_id];

	if (!bkpb->has_data)
	{
		if (len)
			*len = 0;
		return NULL;
	}
	else
	{
		if (len)
			*len = bkpb->data_len;
		return bkpb->data;
	}
}

/*
 * Restore a full-page image from a backup block attached to an XLOG record.
 *
 * Returns true if a full-page image is restored.
 */
bool
RestoreBlockImage(XLogReaderState *record, uint8 block_id, char *page)
{
	DecodedBkpBlock *bkpb;
	char	   *ptr;
	PGAlignedBlock tmp;

	if (!record->blocks[block_id].in_use)
		return false;
	if (!record->blocks[block_id].has_image)
		return false;

	bkpb = &record->blocks[block_id];
	ptr = bkpb->bkp_image;

	if (bkpb->bimg_info & BKPIMAGE_IS_COMPRESSED)
	{
		/* If a backup block image is compressed, decompress it */
		if (pglz_decompress(ptr, bkpb->bimg_len, tmp.data,
							BLCKSZ - bkpb->hole_length, true) < 0)
		{
			report_invalid_record(record, "invalid compressed image at %X/%X, block %d",
								  LSN_FORMAT_ARGS(record->ReadRecPtr),
								  block_id);
			return false;
		}
		ptr = tmp.data;
	}

	/* generate page, taking into account hole if necessary */
	if (bkpb->hole_length == 0)
	{
		memcpy(page, ptr, BLCKSZ);
	}
	else
	{
		memcpy(page, ptr, bkpb->hole_offset);
		/* must zero-fill the hole */
		MemSet(page + bkpb->hole_offset, 0, bkpb->hole_length);
		memcpy(page + (bkpb->hole_offset + bkpb->hole_length),
			   ptr + bkpb->hole_offset,
			   BLCKSZ - (bkpb->hole_offset + bkpb->hole_length));
	}

	return true;
}

#ifndef FRONTEND

/*
 * Extract the FullTransactionId from a WAL record.
 */
FullTransactionId
XLogRecGetFullXid(XLogReaderState *record)
{
	TransactionId xid,
				next_xid;
	uint32		epoch;

	/*
	 * This function is only safe during replay, because it depends on the
	 * replay state.  See AdvanceNextFullTransactionIdPastXid() for more.
	 */
	Assert(AmStartupProcess() || !IsUnderPostmaster);

	xid = XLogRecGetXid(record);
	next_xid = XidFromFullTransactionId(ShmemVariableCache->nextXid);
	epoch = EpochFromFullTransactionId(ShmemVariableCache->nextXid);

	/*
	 * If xid is numerically greater than next_xid, it has to be from the last
	 * epoch.
	 */
	if (unlikely(xid > next_xid))
		--epoch;

	return FullTransactionIdFromEpochAndXid(epoch, xid);
}

#endif
