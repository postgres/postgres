/*-------------------------------------------------------------------------
 *
 * xlogreader.c
 *		Generic XLog reading facility
 *
 * Portions Copyright (c) 2013-2025, PostgreSQL Global Development Group
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
#ifdef USE_LZ4
#include <lz4.h>
#endif
#ifdef USE_ZSTD
#include <zstd.h>
#endif

#include "access/transam.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "access/xlogrecord.h"
#include "catalog/pg_control.h"
#include "common/pg_lzcompress.h"
#include "replication/origin.h"

#ifndef FRONTEND
#include "pgstat.h"
#include "storage/bufmgr.h"
#else
#include "common/logging.h"
#endif

static void report_invalid_record(XLogReaderState *state, const char *fmt,...)
			pg_attribute_printf(2, 3);
static void allocate_recordbuf(XLogReaderState *state, uint32 reclength);
static int	ReadPageInternal(XLogReaderState *state, XLogRecPtr pageptr,
							 int reqLen);
static void XLogReaderInvalReadState(XLogReaderState *state);
static XLogPageReadResult XLogDecodeNextRecord(XLogReaderState *state, bool nonblocking);
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
 * Default size; large enough that typical users of XLogReader won't often need
 * to use the 'oversized' memory allocation code path.
 */
#define DEFAULT_DECODE_BUFFER_SIZE (64 * 1024)

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

	state->errormsg_deferred = true;
}

/*
 * Set the size of the decoding buffer.  A pointer to a caller supplied memory
 * region may also be passed in, in which case non-oversized records will be
 * decoded there.
 */
void
XLogReaderSetDecodeBuffer(XLogReaderState *state, void *buffer, size_t size)
{
	Assert(state->decode_buffer == NULL);

	state->decode_buffer = buffer;
	state->decode_buffer_size = size;
	state->decode_buffer_tail = buffer;
	state->decode_buffer_head = buffer;
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
	allocate_recordbuf(state, 0);
	return state;
}

void
XLogReaderFree(XLogReaderState *state)
{
	if (state->seg.ws_file != -1)
		state->routine.segment_close(state);

	if (state->decode_buffer && state->free_decode_buffer)
		pfree(state->decode_buffer);

	pfree(state->errormsg_buf);
	if (state->readRecordBuf)
		pfree(state->readRecordBuf);
	pfree(state->readBuf);
	pfree(state);
}

/*
 * Allocate readRecordBuf to fit a record of at least the given length.
 *
 * readRecordBufSize is set to the new buffer size.
 *
 * To avoid useless small increases, round its size to a multiple of
 * XLOG_BLCKSZ, and make sure it's at least 5*Max(BLCKSZ, XLOG_BLCKSZ) to start
 * with.  (That is enough for all "normal" records, but very large commit or
 * abort records might need more space.)
 *
 * Note: This routine should *never* be called for xl_tot_len until the header
 * of the record has been fully validated.
 */
static void
allocate_recordbuf(XLogReaderState *state, uint32 reclength)
{
	uint32		newSize = reclength;

	newSize += XLOG_BLCKSZ - (newSize % XLOG_BLCKSZ);
	newSize = Max(newSize, 5 * Max(BLCKSZ, XLOG_BLCKSZ));

	if (state->readRecordBuf)
		pfree(state->readRecordBuf);
	state->readRecordBuf = (char *) palloc(newSize);
	state->readRecordBufSize = newSize;
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
 * 'RecPtr' should point to the beginning of a valid WAL record.  Pointing at
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
	state->NextRecPtr = RecPtr;
	state->ReadRecPtr = InvalidXLogRecPtr;
	state->DecodeRecPtr = InvalidXLogRecPtr;
}

/*
 * Release the last record that was returned by XLogNextRecord(), if any, to
 * free up space.  Returns the LSN past the end of the record.
 */
XLogRecPtr
XLogReleasePreviousRecord(XLogReaderState *state)
{
	DecodedXLogRecord *record;
	XLogRecPtr	next_lsn;

	if (!state->record)
		return InvalidXLogRecPtr;

	/*
	 * Remove it from the decoded record queue.  It must be the oldest item
	 * decoded, decode_queue_head.
	 */
	record = state->record;
	next_lsn = record->next_lsn;
	Assert(record == state->decode_queue_head);
	state->record = NULL;
	state->decode_queue_head = record->next;

	/* It might also be the newest item decoded, decode_queue_tail. */
	if (state->decode_queue_tail == record)
		state->decode_queue_tail = NULL;

	/* Release the space. */
	if (unlikely(record->oversized))
	{
		/* It's not in the decode buffer, so free it to release space. */
		pfree(record);
	}
	else
	{
		/* It must be the head (oldest) record in the decode buffer. */
		Assert(state->decode_buffer_head == (char *) record);

		/*
		 * We need to update head to point to the next record that is in the
		 * decode buffer, if any, being careful to skip oversized ones
		 * (they're not in the decode buffer).
		 */
		record = record->next;
		while (unlikely(record && record->oversized))
			record = record->next;

		if (record)
		{
			/* Adjust head to release space up to the next record. */
			state->decode_buffer_head = (char *) record;
		}
		else
		{
			/*
			 * Otherwise we might as well just reset head and tail to the
			 * start of the buffer space, because we're empty.  This means
			 * we'll keep overwriting the same piece of memory if we're not
			 * doing any prefetching.
			 */
			state->decode_buffer_head = state->decode_buffer;
			state->decode_buffer_tail = state->decode_buffer;
		}
	}

	return next_lsn;
}

/*
 * Attempt to read an XLOG record.
 *
 * XLogBeginRead() or XLogFindNextRecord() and then XLogReadAhead() must be
 * called before the first call to XLogNextRecord().  This functions returns
 * records and errors that were put into an internal queue by XLogReadAhead().
 *
 * On success, a record is returned.
 *
 * The returned record (or *errormsg) points to an internal buffer that's
 * valid until the next call to XLogNextRecord.
 */
DecodedXLogRecord *
XLogNextRecord(XLogReaderState *state, char **errormsg)
{
	/* Release the last record returned by XLogNextRecord(). */
	XLogReleasePreviousRecord(state);

	if (state->decode_queue_head == NULL)
	{
		*errormsg = NULL;
		if (state->errormsg_deferred)
		{
			if (state->errormsg_buf[0] != '\0')
				*errormsg = state->errormsg_buf;
			state->errormsg_deferred = false;
		}

		/*
		 * state->EndRecPtr is expected to have been set by the last call to
		 * XLogBeginRead() or XLogNextRecord(), and is the location of the
		 * error.
		 */
		Assert(!XLogRecPtrIsInvalid(state->EndRecPtr));

		return NULL;
	}

	/*
	 * Record this as the most recent record returned, so that we'll release
	 * it next time.  This also exposes it to the traditional
	 * XLogRecXXX(xlogreader) macros, which work with the decoder rather than
	 * the record for historical reasons.
	 */
	state->record = state->decode_queue_head;

	/*
	 * Update the pointers to the beginning and one-past-the-end of this
	 * record, again for the benefit of historical code that expected the
	 * decoder to track this rather than accessing these fields of the record
	 * itself.
	 */
	state->ReadRecPtr = state->record->lsn;
	state->EndRecPtr = state->record->next_lsn;

	*errormsg = NULL;

	return state->record;
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
	DecodedXLogRecord *decoded;

	/*
	 * Release last returned record, if there is one.  We need to do this so
	 * that we can check for empty decode queue accurately.
	 */
	XLogReleasePreviousRecord(state);

	/*
	 * Call XLogReadAhead() in blocking mode to make sure there is something
	 * in the queue, though we don't use the result.
	 */
	if (!XLogReaderHasQueuedRecordOrError(state))
		XLogReadAhead(state, false /* nonblocking */ );

	/* Consume the head record or error. */
	decoded = XLogNextRecord(state, errormsg);
	if (decoded)
	{
		/*
		 * This function returns a pointer to the record's header, not the
		 * actual decoded record.  The caller will access the decoded record
		 * through the XLogRecGetXXX() macros, which reach the decoded
		 * recorded as xlogreader->record.
		 */
		Assert(state->record == decoded);
		return &decoded->header;
	}

	return NULL;
}

/*
 * Allocate space for a decoded record.  The only member of the returned
 * object that is initialized is the 'oversized' flag, indicating that the
 * decoded record wouldn't fit in the decode buffer and must eventually be
 * freed explicitly.
 *
 * The caller is responsible for adjusting decode_buffer_tail with the real
 * size after successfully decoding a record into this space.  This way, if
 * decoding fails, then there is nothing to undo unless the 'oversized' flag
 * was set and pfree() must be called.
 *
 * Return NULL if there is no space in the decode buffer and allow_oversized
 * is false, or if memory allocation fails for an oversized buffer.
 */
static DecodedXLogRecord *
XLogReadRecordAlloc(XLogReaderState *state, size_t xl_tot_len, bool allow_oversized)
{
	size_t		required_space = DecodeXLogRecordRequiredSpace(xl_tot_len);
	DecodedXLogRecord *decoded = NULL;

	/* Allocate a circular decode buffer if we don't have one already. */
	if (unlikely(state->decode_buffer == NULL))
	{
		if (state->decode_buffer_size == 0)
			state->decode_buffer_size = DEFAULT_DECODE_BUFFER_SIZE;
		state->decode_buffer = palloc(state->decode_buffer_size);
		state->decode_buffer_head = state->decode_buffer;
		state->decode_buffer_tail = state->decode_buffer;
		state->free_decode_buffer = true;
	}

	/* Try to allocate space in the circular decode buffer. */
	if (state->decode_buffer_tail >= state->decode_buffer_head)
	{
		/* Empty, or tail is to the right of head. */
		if (required_space <=
			state->decode_buffer_size -
			(state->decode_buffer_tail - state->decode_buffer))
		{
			/*-
			 * There is space between tail and end.
			 *
			 * +-----+--------------------+-----+
			 * |     |////////////////////|here!|
			 * +-----+--------------------+-----+
			 *       ^                    ^
			 *       |                    |
			 *       h                    t
			 */
			decoded = (DecodedXLogRecord *) state->decode_buffer_tail;
			decoded->oversized = false;
			return decoded;
		}
		else if (required_space <
				 state->decode_buffer_head - state->decode_buffer)
		{
			/*-
			 * There is space between start and head.
			 *
			 * +-----+--------------------+-----+
			 * |here!|////////////////////|     |
			 * +-----+--------------------+-----+
			 *       ^                    ^
			 *       |                    |
			 *       h                    t
			 */
			decoded = (DecodedXLogRecord *) state->decode_buffer;
			decoded->oversized = false;
			return decoded;
		}
	}
	else
	{
		/* Tail is to the left of head. */
		if (required_space <
			state->decode_buffer_head - state->decode_buffer_tail)
		{
			/*-
			 * There is space between tail and head.
			 *
			 * +-----+--------------------+-----+
			 * |/////|here!               |/////|
			 * +-----+--------------------+-----+
			 *       ^                    ^
			 *       |                    |
			 *       t                    h
			 */
			decoded = (DecodedXLogRecord *) state->decode_buffer_tail;
			decoded->oversized = false;
			return decoded;
		}
	}

	/* Not enough space in the decode buffer.  Are we allowed to allocate? */
	if (allow_oversized)
	{
		decoded = palloc(required_space);
		decoded->oversized = true;
		return decoded;
	}

	return NULL;
}

static XLogPageReadResult
XLogDecodeNextRecord(XLogReaderState *state, bool nonblocking)
{
	XLogRecPtr	RecPtr;
	XLogRecord *record;
	XLogRecPtr	targetPagePtr;
	bool		randAccess;
	uint32		len,
				total_len;
	uint32		targetRecOff;
	uint32		pageHeaderSize;
	bool		assembled;
	bool		gotheader;
	int			readOff;
	DecodedXLogRecord *decoded;
	char	   *errormsg;		/* not used */

	/*
	 * randAccess indicates whether to verify the previous-record pointer of
	 * the record we're reading.  We only do this if we're reading
	 * sequentially, which is what we initially assume.
	 */
	randAccess = false;

	/* reset error state */
	state->errormsg_buf[0] = '\0';
	decoded = NULL;

	state->abortedRecPtr = InvalidXLogRecPtr;
	state->missingContrecPtr = InvalidXLogRecPtr;

	RecPtr = state->NextRecPtr;

	if (state->DecodeRecPtr != InvalidXLogRecPtr)
	{
		/* read the record after the one we just read */

		/*
		 * NextRecPtr is pointing to end+1 of the previous WAL record.  If
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
		 * In this case, NextRecPtr should already be pointing either to a
		 * valid record starting position or alternatively to the beginning of
		 * a page. See the header comments for XLogBeginRead.
		 */
		Assert(RecPtr % XLOG_BLCKSZ == 0 || XRecOffIsValid(RecPtr));
		randAccess = true;
	}

restart:
	state->nonblocking = nonblocking;
	state->currRecPtr = RecPtr;
	assembled = false;

	targetPagePtr = RecPtr - (RecPtr % XLOG_BLCKSZ);
	targetRecOff = RecPtr % XLOG_BLCKSZ;

	/*
	 * Read the page containing the record into state->readBuf. Request enough
	 * byte to cover the whole record header, or at least the part of it that
	 * fits on the same page.
	 */
	readOff = ReadPageInternal(state, targetPagePtr,
							   Min(targetRecOff + SizeOfXLogRecord, XLOG_BLCKSZ));
	if (readOff == XLREAD_WOULDBLOCK)
		return XLREAD_WOULDBLOCK;
	else if (readOff < 0)
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
		report_invalid_record(state, "invalid record offset at %X/%08X: expected at least %u, got %u",
							  LSN_FORMAT_ARGS(RecPtr),
							  pageHeaderSize, targetRecOff);
		goto err;
	}

	if ((((XLogPageHeader) state->readBuf)->xlp_info & XLP_FIRST_IS_CONTRECORD) &&
		targetRecOff == pageHeaderSize)
	{
		report_invalid_record(state, "contrecord is requested by %X/%08X",
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
		if (!ValidXLogRecordHeader(state, RecPtr, state->DecodeRecPtr, record,
								   randAccess))
			goto err;
		gotheader = true;
	}
	else
	{
		/* There may be no next page if it's too small. */
		if (total_len < SizeOfXLogRecord)
		{
			report_invalid_record(state,
								  "invalid record length at %X/%08X: expected at least %u, got %u",
								  LSN_FORMAT_ARGS(RecPtr),
								  (uint32) SizeOfXLogRecord, total_len);
			goto err;
		}
		/* We'll validate the header once we have the next page. */
		gotheader = false;
	}

	/*
	 * Try to find space to decode this record, if we can do so without
	 * calling palloc.  If we can't, we'll try again below after we've
	 * validated that total_len isn't garbage bytes from a recycled WAL page.
	 */
	decoded = XLogReadRecordAlloc(state,
								  total_len,
								  false /* allow_oversized */ );
	if (decoded == NULL && nonblocking)
	{
		/*
		 * There is no space in the circular decode buffer, and the caller is
		 * only reading ahead.  The caller should consume existing records to
		 * make space.
		 */
		return XLREAD_WOULDBLOCK;
	}

	len = XLOG_BLCKSZ - RecPtr % XLOG_BLCKSZ;
	if (total_len > len)
	{
		/* Need to reassemble record */
		char	   *contdata;
		XLogPageHeader pageHeader;
		char	   *buffer;
		uint32		gotlen;

		assembled = true;

		/*
		 * We always have space for a couple of pages, enough to validate a
		 * boundary-spanning record header.
		 */
		Assert(state->readRecordBufSize >= XLOG_BLCKSZ * 2);
		Assert(state->readRecordBufSize >= len);

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

			if (readOff == XLREAD_WOULDBLOCK)
				return XLREAD_WOULDBLOCK;
			else if (readOff < 0)
				goto err;

			Assert(SizeOfXLogShortPHD <= readOff);

			pageHeader = (XLogPageHeader) state->readBuf;

			/*
			 * If we were expecting a continuation record and got an
			 * "overwrite contrecord" flag, that means the continuation record
			 * was overwritten with a different record.  Restart the read by
			 * assuming the address to read is the location where we found
			 * this flag; but keep track of the LSN of the record we were
			 * reading, for later verification.
			 */
			if (pageHeader->xlp_info & XLP_FIRST_IS_OVERWRITE_CONTRECORD)
			{
				state->overwrittenRecPtr = RecPtr;
				RecPtr = targetPagePtr;
				goto restart;
			}

			/* Check that the continuation on next page looks valid */
			if (!(pageHeader->xlp_info & XLP_FIRST_IS_CONTRECORD))
			{
				report_invalid_record(state,
									  "there is no contrecord flag at %X/%08X",
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
									  "invalid contrecord length %u (expected %lld) at %X/%08X",
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

			memcpy(buffer, contdata, len);
			buffer += len;
			gotlen += len;

			/* If we just reassembled the record header, validate it. */
			if (!gotheader)
			{
				record = (XLogRecord *) state->readRecordBuf;
				if (!ValidXLogRecordHeader(state, RecPtr, state->DecodeRecPtr,
										   record, randAccess))
					goto err;
				gotheader = true;
			}

			/*
			 * We might need a bigger buffer.  We have validated the record
			 * header, in the case that it split over a page boundary.  We've
			 * also cross-checked total_len against xlp_rem_len on the second
			 * page, and verified xlp_pageaddr on both.
			 */
			if (total_len > state->readRecordBufSize)
			{
				char		save_copy[XLOG_BLCKSZ * 2];

				/*
				 * Save and restore the data we already had.  It can't be more
				 * than two pages.
				 */
				Assert(gotlen <= lengthof(save_copy));
				Assert(gotlen <= state->readRecordBufSize);
				memcpy(save_copy, state->readRecordBuf, gotlen);
				allocate_recordbuf(state, total_len);
				memcpy(state->readRecordBuf, save_copy, gotlen);
				buffer = state->readRecordBuf + gotlen;
			}
		} while (gotlen < total_len);
		Assert(gotheader);

		record = (XLogRecord *) state->readRecordBuf;
		if (!ValidXLogRecord(state, record, RecPtr))
			goto err;

		pageHeaderSize = XLogPageHeaderSize((XLogPageHeader) state->readBuf);
		state->DecodeRecPtr = RecPtr;
		state->NextRecPtr = targetPagePtr + pageHeaderSize
			+ MAXALIGN(pageHeader->xlp_rem_len);
	}
	else
	{
		/* Wait for the record data to become available */
		readOff = ReadPageInternal(state, targetPagePtr,
								   Min(targetRecOff + total_len, XLOG_BLCKSZ));
		if (readOff == XLREAD_WOULDBLOCK)
			return XLREAD_WOULDBLOCK;
		else if (readOff < 0)
			goto err;

		/* Record does not cross a page boundary */
		if (!ValidXLogRecord(state, record, RecPtr))
			goto err;

		state->NextRecPtr = RecPtr + MAXALIGN(total_len);

		state->DecodeRecPtr = RecPtr;
	}

	/*
	 * Special processing if it's an XLOG SWITCH record
	 */
	if (record->xl_rmid == RM_XLOG_ID &&
		(record->xl_info & ~XLR_INFO_MASK) == XLOG_SWITCH)
	{
		/* Pretend it extends to end of segment */
		state->NextRecPtr += state->segcxt.ws_segsize - 1;
		state->NextRecPtr -= XLogSegmentOffset(state->NextRecPtr, state->segcxt.ws_segsize);
	}

	/*
	 * If we got here without a DecodedXLogRecord, it means we needed to
	 * validate total_len before trusting it, but by now we've done that.
	 */
	if (decoded == NULL)
	{
		Assert(!nonblocking);
		decoded = XLogReadRecordAlloc(state,
									  total_len,
									  true /* allow_oversized */ );
		/* allocation should always happen under allow_oversized */
		Assert(decoded != NULL);
	}

	if (DecodeXLogRecord(state, decoded, record, RecPtr, &errormsg))
	{
		/* Record the location of the next record. */
		decoded->next_lsn = state->NextRecPtr;

		/*
		 * If it's in the decode buffer, mark the decode buffer space as
		 * occupied.
		 */
		if (!decoded->oversized)
		{
			/* The new decode buffer head must be MAXALIGNed. */
			Assert(decoded->size == MAXALIGN(decoded->size));
			if ((char *) decoded == state->decode_buffer)
				state->decode_buffer_tail = state->decode_buffer + decoded->size;
			else
				state->decode_buffer_tail += decoded->size;
		}

		/* Insert it into the queue of decoded records. */
		Assert(state->decode_queue_tail != decoded);
		if (state->decode_queue_tail)
			state->decode_queue_tail->next = decoded;
		state->decode_queue_tail = decoded;
		if (!state->decode_queue_head)
			state->decode_queue_head = decoded;
		return XLREAD_SUCCESS;
	}

err:
	if (assembled)
	{
		/*
		 * We get here when a record that spans multiple pages needs to be
		 * assembled, but something went wrong -- perhaps a contrecord piece
		 * was lost.  If caller is WAL replay, it will know where the aborted
		 * record was and where to direct followup WAL to be written, marking
		 * the next piece with XLP_FIRST_IS_OVERWRITE_CONTRECORD, which will
		 * in turn signal downstream WAL consumers that the broken WAL record
		 * is to be ignored.
		 */
		state->abortedRecPtr = RecPtr;
		state->missingContrecPtr = targetPagePtr;

		/*
		 * If we got here without reporting an error, make sure an error is
		 * queued so that XLogPrefetcherReadRecord() doesn't bring us back a
		 * second time and clobber the above state.
		 */
		state->errormsg_deferred = true;
	}

	if (decoded && decoded->oversized)
		pfree(decoded);

	/*
	 * Invalidate the read state. We might read from a different source after
	 * failure.
	 */
	XLogReaderInvalReadState(state);

	/*
	 * If an error was written to errormsg_buf, it'll be returned to the
	 * caller of XLogReadRecord() after all successfully decoded records from
	 * the read queue.
	 */

	return XLREAD_FAIL;
}

/*
 * Try to decode the next available record, and return it.  The record will
 * also be returned to XLogNextRecord(), which must be called to 'consume'
 * each record.
 *
 * If nonblocking is true, may return NULL due to lack of data or WAL decoding
 * space.
 */
DecodedXLogRecord *
XLogReadAhead(XLogReaderState *state, bool nonblocking)
{
	XLogPageReadResult result;

	if (state->errormsg_deferred)
		return NULL;

	result = XLogDecodeNextRecord(state, nonblocking);
	if (result == XLREAD_SUCCESS)
	{
		Assert(state->decode_queue_tail != NULL);
		return state->decode_queue_tail;
	}

	return NULL;
}

/*
 * Read a single xlog page including at least [pageptr, reqLen] of valid data
 * via the page_read() callback.
 *
 * Returns XLREAD_FAIL if the required page cannot be read for some
 * reason; errormsg_buf is set in that case (unless the error occurs in the
 * page_read callback).
 *
 * Returns XLREAD_WOULDBLOCK if the requested data can't be read without
 * waiting.  This can be returned only if the installed page_read callback
 * respects the state->nonblocking flag, and cannot read the requested data
 * immediately.
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
	 * Invalidate contents of internal buffer before read attempt.  Just set
	 * the length to 0, rather than a full XLogReaderInvalReadState(), so we
	 * don't forget the segment we last successfully read.
	 */
	state->readLen = 0;

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
		if (readLen == XLREAD_WOULDBLOCK)
			return XLREAD_WOULDBLOCK;
		else if (readLen < 0)
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
	if (readLen == XLREAD_WOULDBLOCK)
		return XLREAD_WOULDBLOCK;
	else if (readLen < 0)
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
		if (readLen == XLREAD_WOULDBLOCK)
			return XLREAD_WOULDBLOCK;
		else if (readLen < 0)
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

	return XLREAD_FAIL;
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
							  "invalid record length at %X/%08X: expected at least %u, got %u",
							  LSN_FORMAT_ARGS(RecPtr),
							  (uint32) SizeOfXLogRecord, record->xl_tot_len);
		return false;
	}
	if (!RmgrIdIsValid(record->xl_rmid))
	{
		report_invalid_record(state,
							  "invalid resource manager ID %u at %X/%08X",
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
								  "record with incorrect prev-link %X/%08X at %X/%08X",
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
								  "record with incorrect prev-link %X/%08X at %X/%08X",
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

	Assert(record->xl_tot_len >= SizeOfXLogRecord);

	/* Calculate the CRC */
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, ((char *) record) + SizeOfXLogRecord, record->xl_tot_len - SizeOfXLogRecord);
	/* include the record header last */
	COMP_CRC32C(crc, (char *) record, offsetof(XLogRecord, xl_crc));
	FIN_CRC32C(crc);

	if (!EQ_CRC32C(record->xl_crc, crc))
	{
		report_invalid_record(state,
							  "incorrect resource manager data checksum in record at %X/%08X",
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
	XLogSegNo	segno;
	int32		offset;
	XLogPageHeader hdr = (XLogPageHeader) phdr;

	Assert((recptr % XLOG_BLCKSZ) == 0);

	XLByteToSeg(recptr, segno, state->segcxt.ws_segsize);
	offset = XLogSegmentOffset(recptr, state->segcxt.ws_segsize);

	if (hdr->xlp_magic != XLOG_PAGE_MAGIC)
	{
		char		fname[MAXFNAMELEN];

		XLogFileName(fname, state->seg.ws_tli, segno, state->segcxt.ws_segsize);

		report_invalid_record(state,
							  "invalid magic number %04X in WAL segment %s, LSN %X/%08X, offset %u",
							  hdr->xlp_magic,
							  fname,
							  LSN_FORMAT_ARGS(recptr),
							  offset);
		return false;
	}

	if ((hdr->xlp_info & ~XLP_ALL_FLAGS) != 0)
	{
		char		fname[MAXFNAMELEN];

		XLogFileName(fname, state->seg.ws_tli, segno, state->segcxt.ws_segsize);

		report_invalid_record(state,
							  "invalid info bits %04X in WAL segment %s, LSN %X/%08X, offset %u",
							  hdr->xlp_info,
							  fname,
							  LSN_FORMAT_ARGS(recptr),
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
								  "WAL file is from different database system: WAL file database system identifier is %" PRIu64 ", pg_control database system identifier is %" PRIu64,
								  longhdr->xlp_sysid,
								  state->system_identifier);
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
							  "invalid info bits %04X in WAL segment %s, LSN %X/%08X, offset %u",
							  hdr->xlp_info,
							  fname,
							  LSN_FORMAT_ARGS(recptr),
							  offset);
		return false;
	}

	/*
	 * Check that the address on the page agrees with what we expected. This
	 * check typically fails when an old WAL segment is recycled, and hasn't
	 * yet been overwritten with new data yet.
	 */
	if (hdr->xlp_pageaddr != recptr)
	{
		char		fname[MAXFNAMELEN];

		XLogFileName(fname, state->seg.ws_tli, segno, state->segcxt.ws_segsize);

		report_invalid_record(state,
							  "unexpected pageaddr %X/%08X in WAL segment %s, LSN %X/%08X, offset %u",
							  LSN_FORMAT_ARGS(hdr->xlp_pageaddr),
							  fname,
							  LSN_FORMAT_ARGS(recptr),
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
								  "out-of-sequence timeline ID %u (after %u) in WAL segment %s, LSN %X/%08X, offset %u",
								  hdr->xlp_tli,
								  state->latestPageTLI,
								  fname,
								  LSN_FORMAT_ARGS(recptr),
								  offset);
			return false;
		}
	}
	state->latestPagePtr = recptr;
	state->latestPageTLI = hdr->xlp_tli;

	return true;
}

/*
 * Forget about an error produced by XLogReaderValidatePageHeader().
 */
void
XLogReaderResetError(XLogReaderState *state)
{
	state->errormsg_buf[0] = '\0';
	state->errormsg_deferred = false;
}

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

	/* Make sure ReadPageInternal() can't return XLREAD_WOULDBLOCK. */
	state->nonblocking = false;

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

/*
 * Helper function to ease writing of XLogReaderRoutine->page_read callbacks.
 * If this function is used, caller must supply a segment_open callback in
 * 'state', as that is used here.
 *
 * Read 'count' bytes into 'buf', starting at location 'startptr', from WAL
 * fetched from timeline 'tli'.
 *
 * Returns true if succeeded, false if an error occurs, in which case
 * 'errinfo' receives error details.
 */
bool
WALRead(XLogReaderState *state,
		char *buf, XLogRecPtr startptr, Size count, TimeLineID tli,
		WALReadError *errinfo)
{
	char	   *p;
	XLogRecPtr	recptr;
	Size		nbytes;
#ifndef FRONTEND
	instr_time	io_start;
#endif

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
		 * provided segment_open callback.
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
		/* Measure I/O timing when reading segment */
		io_start = pgstat_prepare_io_time(track_wal_io_timing);

		pgstat_report_wait_start(WAIT_EVENT_WAL_READ);
#endif

		/* Reset errno first; eases reporting non-errno-affecting errors */
		errno = 0;
		readbytes = pg_pread(state->seg.ws_file, p, segbytes, (off_t) startoff);

#ifndef FRONTEND
		pgstat_report_wait_end();

		pgstat_count_io_op_time(IOOBJECT_WAL, IOCONTEXT_NORMAL, IOOP_READ,
								io_start, 1, readbytes);
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

/*
 * Private function to reset the state, forgetting all decoded records, if we
 * are asked to move to a new read position.
 */
static void
ResetDecoder(XLogReaderState *state)
{
	DecodedXLogRecord *r;

	/* Reset the decoded record queue, freeing any oversized records. */
	while ((r = state->decode_queue_head) != NULL)
	{
		state->decode_queue_head = r->next;
		if (r->oversized)
			pfree(r);
	}
	state->decode_queue_tail = NULL;
	state->decode_queue_head = NULL;
	state->record = NULL;

	/* Reset the decode buffer to empty. */
	state->decode_buffer_tail = state->decode_buffer;
	state->decode_buffer_head = state->decode_buffer;

	/* Clear error state. */
	state->errormsg_buf[0] = '\0';
	state->errormsg_deferred = false;
}

/*
 * Compute the maximum possible amount of padding that could be required to
 * decode a record, given xl_tot_len from the record's header.  This is the
 * amount of output buffer space that we need to decode a record, though we
 * might not finish up using it all.
 *
 * This computation is pessimistic and assumes the maximum possible number of
 * blocks, due to lack of better information.
 */
size_t
DecodeXLogRecordRequiredSpace(size_t xl_tot_len)
{
	size_t		size = 0;

	/* Account for the fixed size part of the decoded record struct. */
	size += offsetof(DecodedXLogRecord, blocks[0]);
	/* Account for the flexible blocks array of maximum possible size. */
	size += sizeof(DecodedBkpBlock) * (XLR_MAX_BLOCK_ID + 1);
	/* Account for all the raw main and block data. */
	size += xl_tot_len;
	/* We might insert padding before main_data. */
	size += (MAXIMUM_ALIGNOF - 1);
	/* We might insert padding before each block's data. */
	size += (MAXIMUM_ALIGNOF - 1) * (XLR_MAX_BLOCK_ID + 1);
	/* We might insert padding at the end. */
	size += (MAXIMUM_ALIGNOF - 1);

	return size;
}

/*
 * Decode a record.  "decoded" must point to a MAXALIGNed memory area that has
 * space for at least DecodeXLogRecordRequiredSpace(record) bytes.  On
 * success, decoded->size contains the actual space occupied by the decoded
 * record, which may turn out to be less.
 *
 * Only decoded->oversized member must be initialized already, and will not be
 * modified.  Other members will be initialized as required.
 *
 * On error, a human-readable error message is returned in *errormsg, and
 * the return value is false.
 */
bool
DecodeXLogRecord(XLogReaderState *state,
				 DecodedXLogRecord *decoded,
				 XLogRecord *record,
				 XLogRecPtr lsn,
				 char **errormsg)
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
	char	   *out;
	uint32		remaining;
	uint32		datatotal;
	RelFileLocator *rlocator = NULL;
	uint8		block_id;

	decoded->header = *record;
	decoded->lsn = lsn;
	decoded->next = NULL;
	decoded->record_origin = InvalidRepOriginId;
	decoded->toplevel_xid = InvalidTransactionId;
	decoded->main_data = NULL;
	decoded->main_data_len = 0;
	decoded->max_block_id = -1;
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

			decoded->main_data_len = main_data_len;
			datatotal += main_data_len;
			break;				/* by convention, the main data fragment is
								 * always last */
		}
		else if (block_id == XLR_BLOCK_ID_DATA_LONG)
		{
			/* XLogRecordDataHeaderLong */
			uint32		main_data_len;

			COPY_HEADER_FIELD(&main_data_len, sizeof(uint32));
			decoded->main_data_len = main_data_len;
			datatotal += main_data_len;
			break;				/* by convention, the main data fragment is
								 * always last */
		}
		else if (block_id == XLR_BLOCK_ID_ORIGIN)
		{
			COPY_HEADER_FIELD(&decoded->record_origin, sizeof(RepOriginId));
		}
		else if (block_id == XLR_BLOCK_ID_TOPLEVEL_XID)
		{
			COPY_HEADER_FIELD(&decoded->toplevel_xid, sizeof(TransactionId));
		}
		else if (block_id <= XLR_MAX_BLOCK_ID)
		{
			/* XLogRecordBlockHeader */
			DecodedBkpBlock *blk;
			uint8		fork_flags;

			/* mark any intervening block IDs as not in use */
			for (int i = decoded->max_block_id + 1; i < block_id; ++i)
				decoded->blocks[i].in_use = false;

			if (block_id <= decoded->max_block_id)
			{
				report_invalid_record(state,
									  "out-of-order block_id %u at %X/%08X",
									  block_id,
									  LSN_FORMAT_ARGS(state->ReadRecPtr));
				goto err;
			}
			decoded->max_block_id = block_id;

			blk = &decoded->blocks[block_id];
			blk->in_use = true;
			blk->apply_image = false;

			COPY_HEADER_FIELD(&fork_flags, sizeof(uint8));
			blk->forknum = fork_flags & BKPBLOCK_FORK_MASK;
			blk->flags = fork_flags;
			blk->has_image = ((fork_flags & BKPBLOCK_HAS_IMAGE) != 0);
			blk->has_data = ((fork_flags & BKPBLOCK_HAS_DATA) != 0);

			blk->prefetch_buffer = InvalidBuffer;

			COPY_HEADER_FIELD(&blk->data_len, sizeof(uint16));
			/* cross-check that the HAS_DATA flag is set iff data_length > 0 */
			if (blk->has_data && blk->data_len == 0)
			{
				report_invalid_record(state,
									  "BKPBLOCK_HAS_DATA set, but no data included at %X/%08X",
									  LSN_FORMAT_ARGS(state->ReadRecPtr));
				goto err;
			}
			if (!blk->has_data && blk->data_len != 0)
			{
				report_invalid_record(state,
									  "BKPBLOCK_HAS_DATA not set, but data length is %u at %X/%08X",
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

				if (BKPIMAGE_COMPRESSED(blk->bimg_info))
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
										  "BKPIMAGE_HAS_HOLE set, but hole offset %u length %u block image length %u at %X/%08X",
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
										  "BKPIMAGE_HAS_HOLE not set, but hole offset %u length %u at %X/%08X",
										  (unsigned int) blk->hole_offset,
										  (unsigned int) blk->hole_length,
										  LSN_FORMAT_ARGS(state->ReadRecPtr));
					goto err;
				}

				/*
				 * Cross-check that bimg_len < BLCKSZ if it is compressed.
				 */
				if (BKPIMAGE_COMPRESSED(blk->bimg_info) &&
					blk->bimg_len == BLCKSZ)
				{
					report_invalid_record(state,
										  "BKPIMAGE_COMPRESSED set, but block image length %u at %X/%08X",
										  (unsigned int) blk->bimg_len,
										  LSN_FORMAT_ARGS(state->ReadRecPtr));
					goto err;
				}

				/*
				 * cross-check that bimg_len = BLCKSZ if neither HAS_HOLE is
				 * set nor COMPRESSED().
				 */
				if (!(blk->bimg_info & BKPIMAGE_HAS_HOLE) &&
					!BKPIMAGE_COMPRESSED(blk->bimg_info) &&
					blk->bimg_len != BLCKSZ)
				{
					report_invalid_record(state,
										  "neither BKPIMAGE_HAS_HOLE nor BKPIMAGE_COMPRESSED set, but block image length is %u at %X/%08X",
										  (unsigned int) blk->data_len,
										  LSN_FORMAT_ARGS(state->ReadRecPtr));
					goto err;
				}
			}
			if (!(fork_flags & BKPBLOCK_SAME_REL))
			{
				COPY_HEADER_FIELD(&blk->rlocator, sizeof(RelFileLocator));
				rlocator = &blk->rlocator;
			}
			else
			{
				if (rlocator == NULL)
				{
					report_invalid_record(state,
										  "BKPBLOCK_SAME_REL set but no previous rel at %X/%08X",
										  LSN_FORMAT_ARGS(state->ReadRecPtr));
					goto err;
				}

				blk->rlocator = *rlocator;
			}
			COPY_HEADER_FIELD(&blk->blkno, sizeof(BlockNumber));
		}
		else
		{
			report_invalid_record(state,
								  "invalid block_id %u at %X/%08X",
								  block_id, LSN_FORMAT_ARGS(state->ReadRecPtr));
			goto err;
		}
	}

	if (remaining != datatotal)
		goto shortdata_err;

	/*
	 * Ok, we've parsed the fragment headers, and verified that the total
	 * length of the payload in the fragments is equal to the amount of data
	 * left.  Copy the data of each fragment to contiguous space after the
	 * blocks array, inserting alignment padding before the data fragments so
	 * they can be cast to struct pointers by REDO routines.
	 */
	out = ((char *) decoded) +
		offsetof(DecodedXLogRecord, blocks) +
		sizeof(decoded->blocks[0]) * (decoded->max_block_id + 1);

	/* block data first */
	for (block_id = 0; block_id <= decoded->max_block_id; block_id++)
	{
		DecodedBkpBlock *blk = &decoded->blocks[block_id];

		if (!blk->in_use)
			continue;

		Assert(blk->has_image || !blk->apply_image);

		if (blk->has_image)
		{
			/* no need to align image */
			blk->bkp_image = out;
			memcpy(out, ptr, blk->bimg_len);
			ptr += blk->bimg_len;
			out += blk->bimg_len;
		}
		if (blk->has_data)
		{
			out = (char *) MAXALIGN(out);
			blk->data = out;
			memcpy(blk->data, ptr, blk->data_len);
			ptr += blk->data_len;
			out += blk->data_len;
		}
	}

	/* and finally, the main data */
	if (decoded->main_data_len > 0)
	{
		out = (char *) MAXALIGN(out);
		decoded->main_data = out;
		memcpy(decoded->main_data, ptr, decoded->main_data_len);
		ptr += decoded->main_data_len;
		out += decoded->main_data_len;
	}

	/* Report the actual size we used. */
	decoded->size = MAXALIGN(out - (char *) decoded);
	Assert(DecodeXLogRecordRequiredSpace(record->xl_tot_len) >=
		   decoded->size);

	return true;

shortdata_err:
	report_invalid_record(state,
						  "record with invalid length at %X/%08X",
						  LSN_FORMAT_ARGS(state->ReadRecPtr));
err:
	*errormsg = state->errormsg_buf;

	return false;
}

/*
 * Returns information about the block that a block reference refers to.
 *
 * This is like XLogRecGetBlockTagExtended, except that the block reference
 * must exist and there's no access to prefetch_buffer.
 */
void
XLogRecGetBlockTag(XLogReaderState *record, uint8 block_id,
				   RelFileLocator *rlocator, ForkNumber *forknum,
				   BlockNumber *blknum)
{
	if (!XLogRecGetBlockTagExtended(record, block_id, rlocator, forknum,
									blknum, NULL))
	{
#ifndef FRONTEND
		elog(ERROR, "could not locate backup block with ID %d in WAL record",
			 block_id);
#else
		pg_fatal("could not locate backup block with ID %d in WAL record",
				 block_id);
#endif
	}
}

/*
 * Returns information about the block that a block reference refers to,
 * optionally including the buffer that the block may already be in.
 *
 * If the WAL record contains a block reference with the given ID, *rlocator,
 * *forknum, *blknum and *prefetch_buffer are filled in (if not NULL), and
 * returns true.  Otherwise returns false.
 */
bool
XLogRecGetBlockTagExtended(XLogReaderState *record, uint8 block_id,
						   RelFileLocator *rlocator, ForkNumber *forknum,
						   BlockNumber *blknum,
						   Buffer *prefetch_buffer)
{
	DecodedBkpBlock *bkpb;

	if (!XLogRecHasBlockRef(record, block_id))
		return false;

	bkpb = &record->record->blocks[block_id];
	if (rlocator)
		*rlocator = bkpb->rlocator;
	if (forknum)
		*forknum = bkpb->forknum;
	if (blknum)
		*blknum = bkpb->blkno;
	if (prefetch_buffer)
		*prefetch_buffer = bkpb->prefetch_buffer;
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

	if (block_id > record->record->max_block_id ||
		!record->record->blocks[block_id].in_use)
		return NULL;

	bkpb = &record->record->blocks[block_id];

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
 * Returns true if a full-page image is restored, and false on failure with
 * an error to be consumed by the caller.
 */
bool
RestoreBlockImage(XLogReaderState *record, uint8 block_id, char *page)
{
	DecodedBkpBlock *bkpb;
	char	   *ptr;
	PGAlignedBlock tmp;

	if (block_id > record->record->max_block_id ||
		!record->record->blocks[block_id].in_use)
	{
		report_invalid_record(record,
							  "could not restore image at %X/%08X with invalid block %d specified",
							  LSN_FORMAT_ARGS(record->ReadRecPtr),
							  block_id);
		return false;
	}
	if (!record->record->blocks[block_id].has_image)
	{
		report_invalid_record(record, "could not restore image at %X/%08X with invalid state, block %d",
							  LSN_FORMAT_ARGS(record->ReadRecPtr),
							  block_id);
		return false;
	}

	bkpb = &record->record->blocks[block_id];
	ptr = bkpb->bkp_image;

	if (BKPIMAGE_COMPRESSED(bkpb->bimg_info))
	{
		/* If a backup block image is compressed, decompress it */
		bool		decomp_success = true;

		if ((bkpb->bimg_info & BKPIMAGE_COMPRESS_PGLZ) != 0)
		{
			if (pglz_decompress(ptr, bkpb->bimg_len, tmp.data,
								BLCKSZ - bkpb->hole_length, true) < 0)
				decomp_success = false;
		}
		else if ((bkpb->bimg_info & BKPIMAGE_COMPRESS_LZ4) != 0)
		{
#ifdef USE_LZ4
			if (LZ4_decompress_safe(ptr, tmp.data,
									bkpb->bimg_len, BLCKSZ - bkpb->hole_length) <= 0)
				decomp_success = false;
#else
			report_invalid_record(record, "could not restore image at %X/%08X compressed with %s not supported by build, block %d",
								  LSN_FORMAT_ARGS(record->ReadRecPtr),
								  "LZ4",
								  block_id);
			return false;
#endif
		}
		else if ((bkpb->bimg_info & BKPIMAGE_COMPRESS_ZSTD) != 0)
		{
#ifdef USE_ZSTD
			size_t		decomp_result = ZSTD_decompress(tmp.data,
														BLCKSZ - bkpb->hole_length,
														ptr, bkpb->bimg_len);

			if (ZSTD_isError(decomp_result))
				decomp_success = false;
#else
			report_invalid_record(record, "could not restore image at %X/%08X compressed with %s not supported by build, block %d",
								  LSN_FORMAT_ARGS(record->ReadRecPtr),
								  "zstd",
								  block_id);
			return false;
#endif
		}
		else
		{
			report_invalid_record(record, "could not restore image at %X/%08X compressed with unknown method, block %d",
								  LSN_FORMAT_ARGS(record->ReadRecPtr),
								  block_id);
			return false;
		}

		if (!decomp_success)
		{
			report_invalid_record(record, "could not decompress image at %X/%08X, block %d",
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
	/*
	 * This function is only safe during replay, because it depends on the
	 * replay state.  See AdvanceNextFullTransactionIdPastXid() for more.
	 */
	Assert(AmStartupProcess() || !IsUnderPostmaster);

	return FullTransactionIdFromAllowableAt(TransamVariables->nextXid,
											XLogRecGetXid(record));
}

#endif
