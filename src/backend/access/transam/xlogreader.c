/*-------------------------------------------------------------------------
 *
 * xlogreader.c
 *		Generic XLog reading facility
 *
 * Portions Copyright (c) 2013, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/backend/access/transam/xlogreader.c
 *
 * NOTES
 *		See xlogreader.h for more notes on this facility.
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/transam.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "catalog/pg_control.h"

static bool allocate_recordbuf(XLogReaderState *state, uint32 reclength);

static bool ValidXLogPageHeader(XLogReaderState *state, XLogRecPtr recptr,
					XLogPageHeader hdr);
static bool ValidXLogRecordHeader(XLogReaderState *state, XLogRecPtr RecPtr,
				 XLogRecPtr PrevRecPtr, XLogRecord *record, bool randAccess);
static bool ValidXLogRecord(XLogReaderState *state, XLogRecord *record,
				XLogRecPtr recptr);
static int ReadPageInternal(XLogReaderState *state, XLogRecPtr pageptr,
				 int reqLen);
static void
report_invalid_record(XLogReaderState *state, const char *fmt,...)
/* This extension allows gcc to check the format string for consistency with
   the supplied arguments. */
__attribute__((format(PG_PRINTF_ATTRIBUTE, 2, 3)));

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
XLogReaderAllocate(XLogPageReadCB pagereadfunc, void *private_data)
{
	XLogReaderState *state;

	AssertArg(pagereadfunc != NULL);

	state = (XLogReaderState *) malloc(sizeof(XLogReaderState));
	if (!state)
		return NULL;
	MemSet(state, 0, sizeof(XLogReaderState));

	/*
	 * Permanently allocate readBuf.  We do it this way, rather than just
	 * making a static array, for two reasons: (1) no need to waste the
	 * storage in most instantiations of the backend; (2) a static char array
	 * isn't guaranteed to have any particular alignment, whereas malloc()
	 * will provide MAXALIGN'd storage.
	 */
	state->readBuf = (char *) malloc(XLOG_BLCKSZ);
	if (!state->readBuf)
	{
		free(state);
		return NULL;
	}

	state->read_page = pagereadfunc;
	/* system_identifier initialized to zeroes above */
	state->private_data = private_data;
	/* ReadRecPtr and EndRecPtr initialized to zeroes above */
	/* readSegNo, readOff, readLen, readPageTLI initialized to zeroes above */
	state->errormsg_buf = malloc(MAX_ERRORMSG_LEN + 1);
	if (!state->errormsg_buf)
	{
		free(state->readBuf);
		free(state);
		return NULL;
	}
	state->errormsg_buf[0] = '\0';

	/*
	 * Allocate an initial readRecordBuf of minimal size, which can later be
	 * enlarged if necessary.
	 */
	if (!allocate_recordbuf(state, 0))
	{
		free(state->errormsg_buf);
		free(state->readBuf);
		free(state);
		return NULL;
	}

	return state;
}

void
XLogReaderFree(XLogReaderState *state)
{
	free(state->errormsg_buf);
	if (state->readRecordBuf)
		free(state->readRecordBuf);
	free(state->readBuf);
	free(state);
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

	if (state->readRecordBuf)
		free(state->readRecordBuf);
	state->readRecordBuf = (char *) malloc(newSize);
	if (!state->readRecordBuf)
	{
		state->readRecordBufSize = 0;
		return false;
	}

	state->readRecordBufSize = newSize;
	return true;
}

/*
 * Attempt to read an XLOG record.
 *
 * If RecPtr is not NULL, try to read a record at that position.  Otherwise
 * try to read a record just after the last one previously read.
 *
 * If the read_page callback fails to read the requested data, NULL is
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
XLogReadRecord(XLogReaderState *state, XLogRecPtr RecPtr, char **errormsg)
{
	XLogRecord *record;
	XLogRecPtr	targetPagePtr;
	bool		randAccess = false;
	uint32		len,
				total_len;
	uint32		targetRecOff;
	uint32		pageHeaderSize;
	bool		gotheader;
	int			readOff;

	/* reset error state */
	*errormsg = NULL;
	state->errormsg_buf[0] = '\0';

	if (RecPtr == InvalidXLogRecPtr)
	{
		RecPtr = state->EndRecPtr;

		if (state->ReadRecPtr == InvalidXLogRecPtr)
			randAccess = true;

		/*
		 * RecPtr is pointing to end+1 of the previous WAL record.	If we're
		 * at a page boundary, no more records can fit on the current page. We
		 * must skip over the page header, but we can't do that until we've
		 * read in the page, since the header size is variable.
		 */
	}
	else
	{
		/*
		 * In this case, the passed-in record pointer should already be
		 * pointing to a valid record starting position.
		 */
		Assert(XRecOffIsValid(RecPtr));
		randAccess = true;		/* allow readPageTLI to go backwards too */
	}

	state->currRecPtr = RecPtr;

	targetPagePtr = RecPtr - (RecPtr % XLOG_BLCKSZ);
	targetRecOff = RecPtr % XLOG_BLCKSZ;

	/*
	 * Read the page containing the record into state->readBuf. Request enough
	 * byte to cover the whole record header, or at least the part of it that
	 * fits on the same page.
	 */
	readOff = ReadPageInternal(state,
							   targetPagePtr,
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
							  (uint32) (RecPtr >> 32), (uint32) RecPtr);
		goto err;
	}

	if ((((XLogPageHeader) state->readBuf)->xlp_info & XLP_FIRST_IS_CONTRECORD) &&
		targetRecOff == pageHeaderSize)
	{
		report_invalid_record(state, "contrecord is requested by %X/%X",
							  (uint32) (RecPtr >> 32), (uint32) RecPtr);
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
	 * rest of the header after reading it from the next page.	The xl_tot_len
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
			report_invalid_record(state, "invalid record length at %X/%X",
								  (uint32) (RecPtr >> 32), (uint32) RecPtr);
			goto err;
		}
		gotheader = false;
	}

	/*
	 * Enlarge readRecordBuf as needed.
	 */
	if (total_len > state->readRecordBufSize &&
		!allocate_recordbuf(state, total_len))
	{
		/* We treat this as a "bogus data" condition */
		report_invalid_record(state, "record length %u at %X/%X too long",
							  total_len,
							  (uint32) (RecPtr >> 32), (uint32) RecPtr);
		goto err;
	}

	len = XLOG_BLCKSZ - RecPtr % XLOG_BLCKSZ;
	if (total_len > len)
	{
		/* Need to reassemble record */
		char	   *contdata;
		XLogPageHeader pageHeader;
		char	   *buffer;
		uint32		gotlen;

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
								   (uint32) (RecPtr >> 32), (uint32) RecPtr);
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
									  "invalid contrecord length %u at %X/%X",
									  pageHeader->xlp_rem_len,
								   (uint32) (RecPtr >> 32), (uint32) RecPtr);
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
		memcpy(state->readRecordBuf, record, total_len);
	}

	/*
	 * Special processing if it's an XLOG SWITCH record
	 */
	if (record->xl_rmid == RM_XLOG_ID && record->xl_info == XLOG_SWITCH)
	{
		/* Pretend it extends to end of segment */
		state->EndRecPtr += XLogSegSize - 1;
		state->EndRecPtr -= state->EndRecPtr % XLogSegSize;
	}

	return record;

err:

	/*
	 * Invalidate the xlog page we've cached. We might read from a different
	 * source after failure.
	 */
	state->readSegNo = 0;
	state->readOff = 0;
	state->readLen = 0;

	if (state->errormsg_buf[0] != '\0')
		*errormsg = state->errormsg_buf;

	return NULL;
}

/*
 * Read a single xlog page including at least [pageptr, reqLen] of valid data
 * via the read_page() callback.
 *
 * Returns -1 if the required page cannot be read for some reason; errormsg_buf
 * is set in that case (unless the error occurs in the read_page callback).
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

	XLByteToSeg(pageptr, targetSegNo);
	targetPageOff = (pageptr % XLogSegSize);

	/* check whether we have all the requested data already */
	if (targetSegNo == state->readSegNo && targetPageOff == state->readOff &&
		reqLen < state->readLen)
		return state->readLen;

	/*
	 * Data is not in our buffer.
	 *
	 * Every time we actually read the page, even if we looked at parts of it
	 * before, we need to do verification as the read_page callback might now
	 * be rereading data from a different source.
	 *
	 * Whenever switching to a new WAL segment, we read the first page of the
	 * file and validate its header, even if that's not where the target
	 * record is.  This is so that we can check the additional identification
	 * info that is present in the first page's "long" header.
	 */
	if (targetSegNo != state->readSegNo && targetPageOff != 0)
	{
		XLogPageHeader hdr;
		XLogRecPtr	targetSegmentPtr = pageptr - targetPageOff;

		readLen = state->read_page(state, targetSegmentPtr, XLOG_BLCKSZ,
								   state->currRecPtr,
								   state->readBuf, &state->readPageTLI);
		if (readLen < 0)
			goto err;

		/* we can be sure to have enough WAL available, we scrolled back */
		Assert(readLen == XLOG_BLCKSZ);

		hdr = (XLogPageHeader) state->readBuf;

		if (!ValidXLogPageHeader(state, targetSegmentPtr, hdr))
			goto err;
	}

	/*
	 * First, read the requested data length, but at least a short page header
	 * so that we can validate it.
	 */
	readLen = state->read_page(state, pageptr, Max(reqLen, SizeOfXLogShortPHD),
							   state->currRecPtr,
							   state->readBuf, &state->readPageTLI);
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
		readLen = state->read_page(state, pageptr, XLogPageHeaderSize(hdr),
								   state->currRecPtr,
								   state->readBuf, &state->readPageTLI);
		if (readLen < 0)
			goto err;
	}

	/*
	 * Now that we know we have the full header, validate it.
	 */
	if (!ValidXLogPageHeader(state, pageptr, hdr))
		goto err;

	/* update cache information */
	state->readSegNo = targetSegNo;
	state->readOff = targetPageOff;
	state->readLen = readLen;

	return readLen;

err:
	state->readSegNo = 0;
	state->readOff = 0;
	state->readLen = 0;
	return -1;
}

/*
 * Validate an XLOG record header.
 *
 * This is just a convenience subroutine to avoid duplicated code in
 * XLogReadRecord.	It's not intended for use from anywhere else.
 */
static bool
ValidXLogRecordHeader(XLogReaderState *state, XLogRecPtr RecPtr,
					  XLogRecPtr PrevRecPtr, XLogRecord *record,
					  bool randAccess)
{
	/*
	 * xl_len == 0 is bad data for everything except XLOG SWITCH, where it is
	 * required.
	 */
	if (record->xl_rmid == RM_XLOG_ID && record->xl_info == XLOG_SWITCH)
	{
		if (record->xl_len != 0)
		{
			report_invalid_record(state,
								  "invalid xlog switch record at %X/%X",
								  (uint32) (RecPtr >> 32), (uint32) RecPtr);
			return false;
		}
	}
	else if (record->xl_len == 0)
	{
		report_invalid_record(state,
							  "record with zero length at %X/%X",
							  (uint32) (RecPtr >> 32), (uint32) RecPtr);
		return false;
	}
	if (record->xl_tot_len < SizeOfXLogRecord + record->xl_len ||
		record->xl_tot_len > SizeOfXLogRecord + record->xl_len +
		XLR_MAX_BKP_BLOCKS * (sizeof(BkpBlock) + BLCKSZ))
	{
		report_invalid_record(state,
							  "invalid record length at %X/%X",
							  (uint32) (RecPtr >> 32), (uint32) RecPtr);
		return false;
	}
	if (record->xl_rmid > RM_MAX_ID)
	{
		report_invalid_record(state,
							  "invalid resource manager ID %u at %X/%X",
							  record->xl_rmid, (uint32) (RecPtr >> 32),
							  (uint32) RecPtr);
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
								  (uint32) (record->xl_prev >> 32),
								  (uint32) record->xl_prev,
								  (uint32) (RecPtr >> 32), (uint32) RecPtr);
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
								  (uint32) (record->xl_prev >> 32),
								  (uint32) record->xl_prev,
								  (uint32) (RecPtr >> 32), (uint32) RecPtr);
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
 * into memory at *record.	Also, ValidXLogRecordHeader() has accepted the
 * record's header, which means in particular that xl_tot_len is at least
 * SizeOfXlogRecord, so it is safe to fetch xl_len.
 */
static bool
ValidXLogRecord(XLogReaderState *state, XLogRecord *record, XLogRecPtr recptr)
{
	pg_crc32	crc;
	int			i;
	uint32		len = record->xl_len;
	BkpBlock	bkpb;
	char	   *blk;
	size_t		remaining = record->xl_tot_len;

	/* First the rmgr data */
	if (remaining < SizeOfXLogRecord + len)
	{
		/* ValidXLogRecordHeader() should've caught this already... */
		report_invalid_record(state, "invalid record length at %X/%X",
							  (uint32) (recptr >> 32), (uint32) recptr);
		return false;
	}
	remaining -= SizeOfXLogRecord + len;
	INIT_CRC32(crc);
	COMP_CRC32(crc, XLogRecGetData(record), len);

	/* Add in the backup blocks, if any */
	blk = (char *) XLogRecGetData(record) + len;
	for (i = 0; i < XLR_MAX_BKP_BLOCKS; i++)
	{
		uint32		blen;

		if (!(record->xl_info & XLR_BKP_BLOCK(i)))
			continue;

		if (remaining < sizeof(BkpBlock))
		{
			report_invalid_record(state,
							  "invalid backup block size in record at %X/%X",
								  (uint32) (recptr >> 32), (uint32) recptr);
			return false;
		}
		memcpy(&bkpb, blk, sizeof(BkpBlock));

		if (bkpb.hole_offset + bkpb.hole_length > BLCKSZ)
		{
			report_invalid_record(state,
								  "incorrect hole size in record at %X/%X",
								  (uint32) (recptr >> 32), (uint32) recptr);
			return false;
		}
		blen = sizeof(BkpBlock) + BLCKSZ - bkpb.hole_length;

		if (remaining < blen)
		{
			report_invalid_record(state,
							  "invalid backup block size in record at %X/%X",
								  (uint32) (recptr >> 32), (uint32) recptr);
			return false;
		}
		remaining -= blen;
		COMP_CRC32(crc, blk, blen);
		blk += blen;
	}

	/* Check that xl_tot_len agrees with our calculation */
	if (remaining != 0)
	{
		report_invalid_record(state,
							  "incorrect total length in record at %X/%X",
							  (uint32) (recptr >> 32), (uint32) recptr);
		return false;
	}

	/* Finally include the record header */
	COMP_CRC32(crc, (char *) record, offsetof(XLogRecord, xl_crc));
	FIN_CRC32(crc);

	if (!EQ_CRC32(record->xl_crc, crc))
	{
		report_invalid_record(state,
			   "incorrect resource manager data checksum in record at %X/%X",
							  (uint32) (recptr >> 32), (uint32) recptr);
		return false;
	}

	return true;
}

/*
 * Validate a page header
 */
static bool
ValidXLogPageHeader(XLogReaderState *state, XLogRecPtr recptr,
					XLogPageHeader hdr)
{
	XLogRecPtr	recaddr;
	XLogSegNo	segno;
	int32		offset;

	Assert((recptr % XLOG_BLCKSZ) == 0);

	XLByteToSeg(recptr, segno);
	offset = recptr % XLogSegSize;

	XLogSegNoOffsetToRecPtr(segno, offset, recaddr);

	if (hdr->xlp_magic != XLOG_PAGE_MAGIC)
	{
		char		fname[MAXFNAMELEN];

		XLogFileName(fname, state->readPageTLI, segno);

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

		XLogFileName(fname, state->readPageTLI, segno);

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
			char		fhdrident_str[32];
			char		sysident_str[32];

			/*
			 * Format sysids separately to keep platform-dependent format code
			 * out of the translatable message string.
			 */
			snprintf(fhdrident_str, sizeof(fhdrident_str), UINT64_FORMAT,
					 longhdr->xlp_sysid);
			snprintf(sysident_str, sizeof(sysident_str), UINT64_FORMAT,
					 state->system_identifier);
			report_invalid_record(state,
								  "WAL file is from different database system: WAL file database system identifier is %s, pg_control database system identifier is %s.",
								  fhdrident_str, sysident_str);
			return false;
		}
		else if (longhdr->xlp_seg_size != XLogSegSize)
		{
			report_invalid_record(state,
								  "WAL file is from different database system: Incorrect XLOG_SEG_SIZE in page header.");
			return false;
		}
		else if (longhdr->xlp_xlog_blcksz != XLOG_BLCKSZ)
		{
			report_invalid_record(state,
								  "WAL file is from different database system: Incorrect XLOG_BLCKSZ in page header.");
			return false;
		}
	}
	else if (offset == 0)
	{
		char		fname[MAXFNAMELEN];

		XLogFileName(fname, state->readPageTLI, segno);

		/* hmm, first page of file doesn't have a long header? */
		report_invalid_record(state,
					   "invalid info bits %04X in log segment %s, offset %u",
							  hdr->xlp_info,
							  fname,
							  offset);
		return false;
	}

	if (hdr->xlp_pageaddr != recaddr)
	{
		char		fname[MAXFNAMELEN];

		XLogFileName(fname, state->readPageTLI, segno);

		report_invalid_record(state,
					"unexpected pageaddr %X/%X in log segment %s, offset %u",
			  (uint32) (hdr->xlp_pageaddr >> 32), (uint32) hdr->xlp_pageaddr,
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

			XLogFileName(fname, state->readPageTLI, segno);

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
 * Find the first record with at an lsn >= RecPtr.
 *
 * Useful for checking whether RecPtr is a valid xlog address for reading and to
 * find the first valid address after some address when dumping records for
 * debugging purposes.
 */
XLogRecPtr
XLogFindNextRecord(XLogReaderState *state, XLogRecPtr RecPtr)
{
	XLogReaderState saved_state = *state;
	XLogRecPtr	targetPagePtr;
	XLogRecPtr	tmpRecPtr;
	int			targetRecOff;
	XLogRecPtr	found = InvalidXLogRecPtr;
	uint32		pageHeaderSize;
	XLogPageHeader header;
	XLogRecord *record;
	int			readLen;
	char	   *errormsg;

	Assert(!XLogRecPtrIsInvalid(RecPtr));

	targetRecOff = RecPtr % XLOG_BLCKSZ;

	/* scroll back to page boundary */
	targetPagePtr = RecPtr - targetRecOff;

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
		/* record headers are MAXALIGN'ed */
		tmpRecPtr = targetPagePtr + pageHeaderSize
			+ MAXALIGN(header->xlp_rem_len);
	}
	else
	{
		tmpRecPtr = targetPagePtr + pageHeaderSize;
	}

	/*
	 * we know now that tmpRecPtr is an address pointing to a valid XLogRecord
	 * because either we're at the first record after the beginning of a page
	 * or we just jumped over the remaining data of a continuation.
	 */
	while ((record = XLogReadRecord(state, tmpRecPtr, &errormsg)))
	{
		/* continue after the record */
		tmpRecPtr = InvalidXLogRecPtr;

		/* past the record we've found, break out */
		if (RecPtr <= state->ReadRecPtr)
		{
			found = state->ReadRecPtr;
			goto out;
		}
	}

err:
out:
	/* Reset state to what we had before finding the record */
	state->readSegNo = 0;
	state->readOff = 0;
	state->readLen = 0;
	state->ReadRecPtr = saved_state.ReadRecPtr;
	state->EndRecPtr = saved_state.EndRecPtr;

	return found;
}

#endif   /* FRONTEND */
