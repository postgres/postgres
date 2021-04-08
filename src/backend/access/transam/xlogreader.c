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
static bool XLogNeedData(XLogReaderState *state, XLogRecPtr pageptr,
						 int reqLen, bool header_inclusive);
size_t DecodeXLogRecordRequiredSpace(size_t xl_tot_len);
static XLogReadRecordResult XLogDecodeOneRecord(XLogReaderState *state,
												bool allow_oversized);
static void XLogReaderInvalReadState(XLogReaderState *state);
static bool ValidXLogRecordHeader(XLogReaderState *state, XLogRecPtr RecPtr,
								  XLogRecPtr PrevRecPtr, XLogRecord *record);
static bool ValidXLogRecord(XLogReaderState *state, XLogRecord *record,
							XLogRecPtr recptr);
static void ResetDecoder(XLogReaderState *state);
static void WALOpenSegmentInit(WALOpenSegment *seg, WALSegmentContext *segcxt,
							   int segsize, const char *waldir);

/* size of the buffer allocated for error message. */
#define MAX_ERRORMSG_LEN 1000

#define DEFAULT_DECODE_BUFFER_SIZE 0x10000

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
 * Allocate and initialize a new XLogReader.
 *
 * Returns NULL if the xlogreader couldn't be allocated.
 */
XLogReaderState *
XLogReaderAllocate(int wal_segment_size, const char *waldir,
				   WALSegmentCleanupCB cleanup_cb)
{
	XLogReaderState *state;

	state = (XLogReaderState *)
		palloc_extended(sizeof(XLogReaderState),
						MCXT_ALLOC_NO_OOM | MCXT_ALLOC_ZERO);
	if (!state)
		return NULL;

	/* initialize caller-provided support functions */
	state->cleanup_cb = cleanup_cb;

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

	/* ReadRecPtr, EndRecPtr, reqLen and readLen initialized to zeroes above */
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
	if (state->seg.ws_file >= 0)
		state->cleanup_cb(state);

	if (state->decode_buffer && state->free_decode_buffer)
		pfree(state->decode_buffer);

	pfree(state->errormsg_buf);
	if (state->readRecordBuf)
		pfree(state->readRecordBuf);
	pfree(state->readBuf);
	pfree(state);
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
	state->decode_buffer_head = buffer;
	state->decode_buffer_tail = buffer;
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
	state->NextRecPtr = RecPtr;
	state->ReadRecPtr = InvalidXLogRecPtr;
	state->DecodeRecPtr = InvalidXLogRecPtr;
	state->readRecordState = XLREAD_NEXT_RECORD;
}

/*
 * See if we can release the last record that was returned by
 * XLogReadRecord(), to free up space.
 */
static void
XLogReleasePreviousRecord(XLogReaderState *state)
{
	DecodedXLogRecord *record;

	/*
	 * Remove it from the decoded record queue.  It must be the oldest
	 * item decoded, decode_queue_tail.
	 */
	record = state->record;
	Assert(record == state->decode_queue_tail);
	state->record = NULL;
	state->decode_queue_tail = record->next;

	/* It might also be the newest item decoded, decode_queue_head. */
	if (state->decode_queue_head == record)
		state->decode_queue_head = NULL;

	/* Release the space. */
	if (unlikely(record->oversized))
	{
		/* It's not in the the decode buffer, so free it to release space. */
		pfree(record);
	}
	else
	{
		/* It must be the tail record in the decode buffer. */
		Assert(state->decode_buffer_tail == (char *) record);

		/*
		 * We need to update tail to point to the next record that is in the
		 * decode buffer, if any, being careful to skip oversized ones
		 * (they're not in the decode buffer).
		 */
		record = record->next;
		while (unlikely(record && record->oversized))
			record = record->next;

		if (record)
		{
			/* Adjust tail to release space up to the next record. */
			state->decode_buffer_tail = (char *) record;
		}
		else if (state->decoding && !state->decoding->oversized)
		{
			/*
			 * We're releasing the last fully decoded record in
			 * XLogReadRecord(), but some time earlier we partially decoded a
			 * record in XLogReadAhead() and were unable to complete the job.
			 * We'll set the buffer head and tail to point to the record we
			 * started working on, so that we can continue (perhaps from a
			 * different source).
			 */
			state->decode_buffer_tail = (char *) state->decoding;
			state->decode_buffer_head = (char *) state->decoding;
		}
		else
		{
			/*
			 * Otherwise we might as well just reset head and tail to the
			 * start of the buffer space, because we're empty.  This means
			 * we'll keep overwriting the same piece of memory if we're not
			 * doing any prefetching.
			 */
			state->decode_buffer_tail = state->decode_buffer;
			state->decode_buffer_head = state->decode_buffer;
		}
	}
}

/*
 * Similar to XLogNextRecord(), but this traditional interface is for code
 * that just wants the header, not the decoded record.  Callers can access the
 * decoded record through the XLogRecGetXXX() macros.
 */
XLogReadRecordResult
XLogReadRecord(XLogReaderState *state, XLogRecord **record, char **errormsg)
{
	XLogReadRecordResult result;
	DecodedXLogRecord *decoded;

	/* Consume the next decoded record. */
	result = XLogNextRecord(state, &decoded, errormsg);
	if (result == XLREAD_SUCCESS)
	{
		/*
		 * The traditional interface just returns the header, not the decoded
		 * record.  The caller will access the decoded record through the
		 * XLogRecGetXXX() macros.
		 */
		*record = &decoded->header;
	}
	else
		*record = NULL;
	return result;
}

/*
 * Consume the next record.  XLogBeginRead() or XLogFindNextRecord() must be
 * called before the first call to XLogNextRecord().
 *
 * This function may return XLREAD_NEED_DATA several times before returning a
 * result record. The caller shall read in some new data then call this
 * function again with the same parameters.
 *
 * When a record is successfully read, returns XLREAD_SUCCESS with result
 * record being stored in *record.  Otherwise *record is set to NULL.
 *
 * Returns XLREAD_NEED_DATA if more data is needed to finish decoding the
 * current record.  In that case, state->readPagePtr and state->reqLen inform
 * the desired position and minimum length of data needed.  The caller shall
 * read in the requested data and set state->readBuf to point to a buffer
 * containing it.  The caller must also set state->seg->ws_tli and
 * state->readLen to indicate the timeline that it was read from, and the
 * length of data that is now available (which must be >= given reqLen),
 * respectively.
 *
 * Returns XLREAD_FULL if allow_oversized is true, and no space is available.
 * This is intended for readahead.
 *
 * If invalid data is encountered, returns XLREAD_FAIL with *record being set
 * to NULL.  *errormsg is set to a string with details of the failure.  The
 * returned pointer (or *errormsg) points to an internal buffer that's valid
 * until the next call to XLogReadRecord.
 *
 */
XLogReadRecordResult
XLogNextRecord(XLogReaderState *state,
			   DecodedXLogRecord **record,
			   char **errormsg)
{
	/* Release the space occupied by the last record we returned. */
	if (state->record)
		XLogReleasePreviousRecord(state);

	for (;;)
	{
		XLogReadRecordResult result;

		/* We can now return the oldest item in the queue, if there is one. */
		if (state->decode_queue_tail)
		{
			/*
			 * Record this as the most recent record returned, so that we'll
			 * release it next time.  This also exposes it to the
			 * XLogRecXXX(decoder) macros, which pass in the decoder rather
			 * than the record for historical reasons.
			 */
			state->record = state->decode_queue_tail;

			/*
			 * It should be immediately after the last the record returned by
			 * XLogReadRecord(), or at the position set by XLogBeginRead() if
			 * XLogReadRecord() hasn't been called yet.  It may be after a
			 * page header, though.
			 */
			Assert(state->record->lsn == state->EndRecPtr ||
				   (state->EndRecPtr % XLOG_BLCKSZ == 0 &&
					(state->record->lsn == state->EndRecPtr + SizeOfXLogShortPHD ||
					 state->record->lsn == state->EndRecPtr + SizeOfXLogLongPHD)));

			/*
			 * Set ReadRecPtr and EndRecPtr to correspond to that
			 * record.
			 *
			 * Calling code could access these through the returned decoded
			 * record, but for now we'll update them directly here, for the
			 * benefit of all the existing code that accesses these variables
			 * directly.
			 */
			state->ReadRecPtr = state->record->lsn;
			state->EndRecPtr = state->record->next_lsn;

			*errormsg = NULL;
			*record = state->record;

			return XLREAD_SUCCESS;
		}
		else if (state->errormsg_deferred)
		{
			/*
			 * If we've run out of records, but we have a deferred error, now
			 * is the time to report it.
			 */
			state->errormsg_deferred = false;
			if (state->errormsg_buf[0] != '\0')
				*errormsg = state->errormsg_buf;
			else
				*errormsg = NULL;
			*record = NULL;
			state->EndRecPtr = state->DecodeRecPtr;

			return XLREAD_FAIL;
		}

		/* We need to get a decoded record into our queue first. */
		result = XLogDecodeOneRecord(state, true /* allow_oversized */ );
		switch(result)
		{
		case XLREAD_NEED_DATA:
			*errormsg = NULL;
			*record = NULL;
			return result;
		case XLREAD_SUCCESS:
			Assert(state->decode_queue_tail != NULL);
			break;
		case XLREAD_FULL:
			/* Not expected because we passed allow_oversized = true */
			Assert(false);
			break;
		case XLREAD_FAIL:
			/*
			 * If that produced neither a queued record nor a queued error,
			 * then we're at the end (for example, archive recovery with no
			 * more files available).
			 */
			Assert(state->decode_queue_tail == NULL);
			if (!state->errormsg_deferred)
			{
				state->EndRecPtr = state->DecodeRecPtr;
				*errormsg = NULL;
				*record = NULL;
				return result;
			}
			break;
		}
	}

	/* unreachable */
	return XLREAD_FAIL;
}

/*
 * Try to decode the next available record.  The next record will also be
 * returned to XLogRecordRead().
 *
 * In addition to the values that XLogReadRecord() can return, XLogReadAhead()
 * can also return XLREAD_FULL to indicate that further readahead is not
 * possible yet due to lack of space.
 */
XLogReadRecordResult
XLogReadAhead(XLogReaderState *state, DecodedXLogRecord **record, char **errormsg)
{
	XLogReadRecordResult result;

	/* We stop trying after encountering an error. */
	if (unlikely(state->errormsg_deferred))
	{
		/* We only report the error message the first time, see below. */
		*errormsg = NULL;
		return XLREAD_FAIL;
	}

	/*
	 * Try to decode one more record, if we have space.  Pass allow_oversized
	 * = false, so that this call returns fast if the decode buffer is full.
	 */
	result = XLogDecodeOneRecord(state, false);
	switch (result)
	{
	case XLREAD_SUCCESS:
		/* New record at head of decode record queue. */
		Assert(state->decode_queue_head != NULL);
		*record = state->decode_queue_head;
		return result;
	case XLREAD_FULL:
		/* No space in circular decode buffer. */
		return result;
	case XLREAD_NEED_DATA:
		/* The caller needs to insert more data. */
		return result;
	case XLREAD_FAIL:
		/* Report the error.  XLogReadRecord() will also report it. */
		Assert(state->errormsg_deferred);
		if (state->errormsg_buf[0] != '\0')
			*errormsg = state->errormsg_buf;
		return result;
	}

	/* Unreachable. */
	return XLREAD_FAIL;
}

/*
 * Allocate space for a decoded record.  The only member of the returned
 * object that is initialized is the 'oversized' flag, indicating that the
 * decoded record wouldn't fit in the decode buffer and must eventually be
 * freed explicitly.
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
	if (state->decode_buffer_head >= state->decode_buffer_tail)
	{
		/* Empty, or head is to the right of tail. */
		if (state->decode_buffer_head + required_space <=
			state->decode_buffer + state->decode_buffer_size)
		{
			/* There is space between head and end. */
			decoded = (DecodedXLogRecord *) state->decode_buffer_head;
			decoded->oversized = false;
			return decoded;
		}
		else if (state->decode_buffer + required_space <
				 state->decode_buffer_tail)
		{
			/* There is space between start and tail. */
			decoded = (DecodedXLogRecord *) state->decode_buffer;
			decoded->oversized = false;
			return decoded;
		}
	}
	else
	{
		/* Head is to the left of tail. */
		if (state->decode_buffer_head + required_space <
			state->decode_buffer_tail)
		{
			/* There is space between head and tail. */
			decoded = (DecodedXLogRecord *) state->decode_buffer_head;
			decoded->oversized = false;
			return decoded;
		}
	}

	/* Not enough space in the decode buffer.  Are we allowed to allocate? */
	if (allow_oversized)
	{
		decoded = palloc_extended(required_space, MCXT_ALLOC_NO_OOM);
		if (decoded == NULL)
			return NULL;
		decoded->oversized = true;
		return decoded;
	}

	return decoded;
}

/*
 * Try to read and decode the next record and add it to the head of the
 * decoded record queue.  If 'allow_oversized' is false, then XLREAD_FULL can
 * be returned to indicate the decoding buffer is full.  XLogBeginRead() or
 * XLogFindNextRecord() must be called before the first call to
 * XLogReadRecord().
 *
 * This function runs a state machine consisting of the following states.
 *
 * XLREAD_NEXT_RECORD:
 *    The initial state.  If called with a valid XLogRecPtr, try to read a
 *    record at that position.  If invalid RecPtr is given try to read a record
 *    just after the last one read.  The next state is XLREAD_TOT_LEN.
 *
 * XLREAD_TOT_LEN:
 *    Examining record header.  Ends after reading record length.
 *    recordRemainLen and recordGotLen are initialized.  The next state is
 *    XLREAD_FIRST_FRAGMENT.
 *
 * XLREAD_FIRST_FRAGMENT:
 *    Reading the first fragment.  Goes to XLREAD_NEXT_RECORD if that's all or
 *    XLREAD_CONTINUATION if we need more data.

 * XLREAD_CONTINUATION:
 *    Reading continuation of record.  If the whole record is now decoded, goes
 *    to XLREAD_NEXT_RECORD.  During this state, recordRemainLen indicates how
 *    much is left.
 *
 * If invalid data is found in any state, the state machine stays at the
 * current state.  This behavior allows us to continue reading a record
 * after switching to a different source, during streaming replication.
 */
static XLogReadRecordResult
XLogDecodeOneRecord(XLogReaderState *state, bool allow_oversized)
{
	XLogRecord *record;
	char	   *errormsg; /* not used */
	XLogRecord *prec;

	/* reset error state */
	state->errormsg_buf[0] = '\0';
	record = NULL;

	switch (state->readRecordState)
	{
		case XLREAD_NEXT_RECORD:
			Assert(!state->decoding);

			if (state->DecodeRecPtr != InvalidXLogRecPtr)
			{
				/* read the record after the one we just read */

				/*
				 * NextRecPtr is pointing to end+1 of the previous WAL record.
				 * If we're at a page boundary, no more records can fit on the
				 * current page. We must skip over the page header, but we
				 * can't do that until we've read in the page, since the
				 * header size is variable.
				 */
				state->PrevRecPtr = state->DecodeRecPtr;
				state->DecodeRecPtr = state->NextRecPtr;
			}
			else
			{
				/*
				 * Caller supplied a position to start at.
				 *
				 * In this case, EndRecPtr should already be pointing to a
				 * valid record starting position.
				 */
				Assert(XRecOffIsValid(state->NextRecPtr));
				state->DecodeRecPtr = state->NextRecPtr;

				/*
				 * We cannot verify the previous-record pointer when we're
				 * seeking to a particular record. Reset PrevRecPtr so that we
				 * won't try doing that.
				 */
				state->PrevRecPtr = InvalidXLogRecPtr;
			}

			state->record_verified = false;
			state->readRecordState = XLREAD_TOT_LEN;
			/* fall through */

		case XLREAD_TOT_LEN:
			{
				uint32		total_len;
				uint32		pageHeaderSize;
				XLogRecPtr	targetPagePtr;
				uint32		targetRecOff;
				XLogPageHeader pageHeader;

				Assert(!state->decoding);

				targetPagePtr =
					state->DecodeRecPtr - (state->DecodeRecPtr % XLOG_BLCKSZ);
				targetRecOff = state->DecodeRecPtr % XLOG_BLCKSZ;

				/*
				 * Check if we have enough data. For the first record in the
				 * page, the requesting length doesn't contain page header.
				 */
				if (XLogNeedData(state, targetPagePtr,
								 Min(targetRecOff + SizeOfXLogRecord, XLOG_BLCKSZ),
								 targetRecOff != 0))
					return XLREAD_NEED_DATA;

				/* error out if caller supplied bogus page */
				if (!state->page_verified)
					goto err;

				/* examine page header now. */
				pageHeaderSize =
					XLogPageHeaderSize((XLogPageHeader) state->readBuf);
				if (targetRecOff == 0)
				{
					/* At page start, so skip over page header. */
					state->DecodeRecPtr += pageHeaderSize;
					targetRecOff = pageHeaderSize;
				}
				else if (targetRecOff < pageHeaderSize)
				{
					report_invalid_record(state, "invalid record offset at %X/%X",
									  LSN_FORMAT_ARGS(state->DecodeRecPtr));
					goto err;
				}

				pageHeader = (XLogPageHeader) state->readBuf;
				if ((pageHeader->xlp_info & XLP_FIRST_IS_CONTRECORD) &&
					targetRecOff == pageHeaderSize)
				{
					report_invalid_record(state, "contrecord is requested by %X/%X",
										  (uint32) (state->DecodeRecPtr >> 32),
										  (uint32) state->DecodeRecPtr);
					goto err;
				}

				/* XLogNeedData has verified the page header */
				Assert(pageHeaderSize <= state->readLen);

				/*
				 * Read the record length.
				 *
				 * NB: Even though we use an XLogRecord pointer here, the
				 * whole record header might not fit on this page. xl_tot_len
				 * is the first field of the struct, so it must be on this
				 * page (the records are MAXALIGNed), but we cannot access any
				 * other fields until we've verified that we got the whole
				 * header.
				 */
				prec = (XLogRecord *) (state->readBuf +
									   state->DecodeRecPtr % XLOG_BLCKSZ);
				total_len = prec->xl_tot_len;

				/* Find space to decode this record. */
				Assert(state->decoding == NULL);
				state->decoding = XLogReadRecordAlloc(state, total_len,
												  allow_oversized);
				if (state->decoding == NULL)
				{
					/*
					 * We couldn't get space.  If allow_oversized was true,
					 * then palloc() must have failed.  Otherwise, report that
					 * our decoding buffer is full.  This means that weare
					 * trying to read too far ahead.
					 */
					if (allow_oversized)
						goto err;
					return XLREAD_FULL;
				}

				/*
				 * If the whole record header is on this page, validate it
				 * immediately.  Otherwise do just a basic sanity check on
				 * xl_tot_len, and validate the rest of the header after
				 * reading it from the next page.  The xl_tot_len check is
				 * necessary here to ensure that we enter the
				 * XLREAD_CONTINUATION state below; otherwise we might fail to
				 * apply ValidXLogRecordHeader at all.
				 */
				if (targetRecOff <= XLOG_BLCKSZ - SizeOfXLogRecord)
				{
					if (!ValidXLogRecordHeader(state, state->DecodeRecPtr,
											   state->PrevRecPtr, prec))
						goto err;

					state->record_verified = true;
				}
				else
				{
					/* XXX: more validation should be done here */
					if (total_len < SizeOfXLogRecord)
					{
						report_invalid_record(state,
											  "invalid record length at %X/%X: wanted %u, got %u",
										  LSN_FORMAT_ARGS(state->DecodeRecPtr),
											  (uint32) SizeOfXLogRecord, total_len);
						goto err;
					}
				}

				/*
				 * Wait for the rest of the record, or the part of the record
				 * that fit on the first page if crossed a page boundary, to
				 * become available.
				 */
				state->recordGotLen = 0;
				state->recordRemainLen = total_len;
				state->readRecordState = XLREAD_FIRST_FRAGMENT;
			}
			/* fall through */

		case XLREAD_FIRST_FRAGMENT:
			{
				uint32		total_len = state->recordRemainLen;
				uint32		request_len;
				uint32		record_len;
				XLogRecPtr	targetPagePtr;
				uint32		targetRecOff;

				Assert(state->decoding);

				/*
				 * Wait for the rest of the record on the first page to become
				 * available
				 */
				targetPagePtr =
					state->DecodeRecPtr - (state->DecodeRecPtr % XLOG_BLCKSZ);
				targetRecOff = state->DecodeRecPtr % XLOG_BLCKSZ;

				request_len = Min(targetRecOff + total_len, XLOG_BLCKSZ);
				record_len = request_len - targetRecOff;

				/* ReadRecPtr contains page header */
				Assert(targetRecOff != 0);
				if (XLogNeedData(state, targetPagePtr, request_len, true))
					return XLREAD_NEED_DATA;

				/* error out if caller supplied bogus page */
				if (!state->page_verified)
					goto err;

				prec = (XLogRecord *) (state->readBuf + targetRecOff);

				/* validate record header if not yet */
				if (!state->record_verified && record_len >= SizeOfXLogRecord)
				{
				if (!ValidXLogRecordHeader(state, state->DecodeRecPtr,
											   state->PrevRecPtr, prec))
						goto err;

					state->record_verified = true;
				}


				if (total_len == record_len)
				{
					/* Record does not cross a page boundary */
					Assert(state->record_verified);

					if (!ValidXLogRecord(state, prec, state->DecodeRecPtr))
						goto err;

					state->record_verified = true;	/* to be tidy */

					/* We already checked the header earlier */
					state->NextRecPtr = state->DecodeRecPtr + MAXALIGN(record_len);

					record = prec;
					state->readRecordState = XLREAD_NEXT_RECORD;
					break;
				}

				/*
				 * The record continues on the next page. Need to reassemble
				 * record
				 */
				Assert(total_len > record_len);

				/* Enlarge readRecordBuf as needed. */
				if (total_len > state->readRecordBufSize &&
					!allocate_recordbuf(state, total_len))
				{
					/* We treat this as a "bogus data" condition */
					report_invalid_record(state,
										  "record length %u at %X/%X too long",
										  total_len,
										  LSN_FORMAT_ARGS(state->DecodeRecPtr));
					goto err;
				}

				/* Copy the first fragment of the record from the first page. */
				memcpy(state->readRecordBuf, state->readBuf + targetRecOff,
					   record_len);
				state->recordGotLen += record_len;
				state->recordRemainLen -= record_len;

				/* Calculate pointer to beginning of next page */
				state->recordContRecPtr = state->DecodeRecPtr + record_len;
				Assert(state->recordContRecPtr % XLOG_BLCKSZ == 0);

				state->readRecordState = XLREAD_CONTINUATION;
			}
			/* fall through */

		case XLREAD_CONTINUATION:
			{
				XLogPageHeader pageHeader = NULL;
				uint32		pageHeaderSize;
				XLogRecPtr	targetPagePtr = InvalidXLogRecPtr;

				/*
				 * we enter this state only if we haven't read the whole
				 * record.
				 */
				Assert(state->decoding);
				Assert(state->recordRemainLen > 0);

				while (state->recordRemainLen > 0)
				{
					char	   *contdata;
					uint32		request_len PG_USED_FOR_ASSERTS_ONLY;
					uint32		record_len;

					/* Wait for the next page to become available */
					targetPagePtr = state->recordContRecPtr;

					/* this request contains page header */
					Assert(targetPagePtr != 0);
					if (XLogNeedData(state, targetPagePtr,
									 Min(state->recordRemainLen, XLOG_BLCKSZ),
									 false))
						return XLREAD_NEED_DATA;

					if (!state->page_verified)
					goto err_continue;

					Assert(SizeOfXLogShortPHD <= state->readLen);

					/* Check that the continuation on next page looks valid */
					pageHeader = (XLogPageHeader) state->readBuf;
					if (!(pageHeader->xlp_info & XLP_FIRST_IS_CONTRECORD))
					{
						report_invalid_record(
											  state,
											  "there is no contrecord flag at %X/%X reading %X/%X",
											  (uint32) (state->recordContRecPtr >> 32),
											  (uint32) state->recordContRecPtr,
											  (uint32) (state->DecodeRecPtr >> 32),
											  (uint32) state->DecodeRecPtr);
						goto err;
					}

					/*
					 * Cross-check that xlp_rem_len agrees with how much of
					 * the record we expect there to be left.
					 */
					if (pageHeader->xlp_rem_len == 0 ||
						pageHeader->xlp_rem_len != state->recordRemainLen)
					{
						report_invalid_record(
											  state,
											  "invalid contrecord length %u at %X/%X reading %X/%X, expected %u",
											  pageHeader->xlp_rem_len,
											  (uint32) (state->recordContRecPtr >> 32),
											  (uint32) state->recordContRecPtr,
											  (uint32) (state->DecodeRecPtr >> 32),
											  (uint32) state->DecodeRecPtr,
											  state->recordRemainLen);
						goto err;
					}

					/* Append the continuation from this page to the buffer */
					pageHeaderSize = XLogPageHeaderSize(pageHeader);

					/*
					 * XLogNeedData should have ensured that the whole page
					 * header was read
					 */
					Assert(pageHeaderSize <= state->readLen);

					contdata = (char *) state->readBuf + pageHeaderSize;
					record_len = XLOG_BLCKSZ - pageHeaderSize;
					if (pageHeader->xlp_rem_len < record_len)
						record_len = pageHeader->xlp_rem_len;

					request_len = record_len + pageHeaderSize;

					/*
					 * XLogNeedData should have ensured all needed data was
					 * read
					 */
					Assert(request_len <= state->readLen);

					memcpy(state->readRecordBuf + state->recordGotLen,
						   (char *) contdata, record_len);
					state->recordGotLen += record_len;
					state->recordRemainLen -= record_len;

					/* If we just reassembled the record header, validate it. */
					if (!state->record_verified)
					{
						Assert(state->recordGotLen >= SizeOfXLogRecord);
						if (!ValidXLogRecordHeader(state, state->DecodeRecPtr,
												   state->PrevRecPtr,
												   (XLogRecord *) state->readRecordBuf))
							goto err;

						state->record_verified = true;
					}

					/*
					 * Calculate pointer to beginning of next page, and
					 * continue
					 */
					state->recordContRecPtr += XLOG_BLCKSZ;
				}

				/* targetPagePtr is pointing the last-read page here */
				prec = (XLogRecord *) state->readRecordBuf;
				if (!ValidXLogRecord(state, prec, state->DecodeRecPtr))
					goto err;

				pageHeaderSize =
					XLogPageHeaderSize((XLogPageHeader) state->readBuf);
				state->NextRecPtr = targetPagePtr + pageHeaderSize
					+ MAXALIGN(pageHeader->xlp_rem_len);

				record = prec;
				state->readRecordState = XLREAD_NEXT_RECORD;

				break;
			}
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

	Assert(!record || state->readLen >= 0);
	if (DecodeXLogRecord(state, state->decoding, record, state->DecodeRecPtr, &errormsg))
	{
		/* Record the location of the next record. */
		state->decoding->next_lsn = state->NextRecPtr;

		/*
		 * If it's in the decode buffer (not an "oversized" record allocated
		 * with palloc()), mark the decode buffer space as occupied.
		 */
		if (!state->decoding->oversized)
		{
			/* The new decode buffer head must be MAXALIGNed. */
			Assert(state->decoding->size == MAXALIGN(state->decoding->size));
			if ((char *) state->decoding == state->decode_buffer)
				state->decode_buffer_head = state->decode_buffer +
					state->decoding->size;
			else
				state->decode_buffer_head += state->decoding->size;
		}

		/* Insert it into the queue of decoded records. */
		Assert(state->decode_queue_head != state->decoding);
		if (state->decode_queue_head)
			state->decode_queue_head->next = state->decoding;
		state->decode_queue_head = state->decoding;
		if (!state->decode_queue_tail)
			state->decode_queue_tail = state->decoding;
		state->decoding = NULL;

		return XLREAD_SUCCESS;
	}

err:
	if (state->decoding && state->decoding->oversized)
		pfree(state->decoding);
	state->decoding = NULL;

err_continue:
	/*
	 * Invalidate the read page. We might read from a different source after
	 * failure.
	 */
	XLogReaderInvalReadState(state);

	/*
	 * If an error was written to errmsg_buf, it'll be returned to the caller
	 * of XLogReadRecord() after all successfully decoded records from the
	 * read queue.
	 */

	return XLREAD_FAIL;
}

/*
 * Checks that an xlog page loaded in state->readBuf is including at least
 * [pageptr, reqLen] and the page is valid. header_inclusive indicates that
 * reqLen is calculated including page header length.
 *
 * Returns false if the buffer already contains the requested data, or found
 * error. state->page_verified is set to true for the former and false for the
 * latter.
 *
 * Otherwise returns true and requests data loaded onto state->readBuf by
 * state->readPagePtr and state->readLen. The caller shall call this function
 * again after filling the buffer at least with that portion of data and set
 * state->readLen to the length of actually loaded data.
 *
 * If header_inclusive is false, corrects reqLen internally by adding the
 * actual page header length and may request caller for new data.
 */
static bool
XLogNeedData(XLogReaderState *state, XLogRecPtr pageptr, int reqLen,
			 bool header_inclusive)
{
	uint32		targetPageOff;
	XLogSegNo	targetSegNo;
	uint32		addLen = 0;

	/* Some data is loaded, but page header is not verified yet. */
	if (!state->page_verified &&
		!XLogRecPtrIsInvalid(state->readPagePtr) && state->readLen >= 0)
	{
		uint32		pageHeaderSize;

		/* just loaded new data so needs to verify page header */

		/* The caller must have loaded at least page header */
		Assert(state->readLen >= SizeOfXLogShortPHD);

		/*
		 * We have enough data to check the header length. Recheck the loaded
		 * length against the actual header length.
		 */
		pageHeaderSize = XLogPageHeaderSize((XLogPageHeader) state->readBuf);

		/* Request more data if we don't have the full header. */
		if (state->readLen < pageHeaderSize)
		{
			state->reqLen = pageHeaderSize;
			return true;
		}

		/* Now that we know we have the full header, validate it. */
		if (!XLogReaderValidatePageHeader(state, state->readPagePtr,
										  (char *) state->readBuf))
		{
			/* That's bad. Force reading the page again. */
			XLogReaderInvalReadState(state);

			return false;
		}

		state->page_verified = true;

		XLByteToSeg(state->readPagePtr, state->seg.ws_segno,
					state->segcxt.ws_segsize);
	}

	/*
	 * The loaded page may not be the one caller is supposing to read when we
	 * are verifying the first page of new segment. In that case, skip further
	 * verification and immediately load the target page.
	 */
	if (state->page_verified && pageptr == state->readPagePtr)
	{
		/*
		 * calculate additional length for page header keeping the total
		 * length within the block size.
		 */
		if (!header_inclusive)
		{
			uint32		pageHeaderSize =
			XLogPageHeaderSize((XLogPageHeader) state->readBuf);

			addLen = pageHeaderSize;
			if (reqLen + pageHeaderSize <= XLOG_BLCKSZ)
				addLen = pageHeaderSize;
			else
				addLen = XLOG_BLCKSZ - reqLen;
		}

		/* Return if we already have it. */
		if (reqLen + addLen <= state->readLen)
			return false;
	}

	/* Data is not in our buffer, request the caller for it. */
	XLByteToSeg(pageptr, targetSegNo, state->segcxt.ws_segsize);
	targetPageOff = XLogSegmentOffset(pageptr, state->segcxt.ws_segsize);
	Assert((pageptr % XLOG_BLCKSZ) == 0);

	/*
	 * Every time we request to load new data of a page to the caller, even if
	 * we looked at a part of it before, we need to do verification on the
	 * next invocation as the caller might now be rereading data from a
	 * different source.
	 */
	state->page_verified = false;

	/*
	 * Whenever switching to a new WAL segment, we read the first page of the
	 * file and validate its header, even if that's not where the target
	 * record is.  This is so that we can check the additional identification
	 * info that is present in the first page's "long" header. Don't do this
	 * if the caller requested the first page in the segment.
	 */
	if (targetSegNo != state->seg.ws_segno && targetPageOff != 0)
	{
		/*
		 * Then we'll see that the targetSegNo now matches the ws_segno, and
		 * will not come back here, but will request the actual target page.
		 */
		state->readPagePtr = pageptr - targetPageOff;
		state->reqLen = XLOG_BLCKSZ;
		return true;
	}

	/*
	 * Request the caller to load the page. We need at least a short page
	 * header so that we can validate it.
	 */
	state->readPagePtr = pageptr;
	state->reqLen = Max(reqLen + addLen, SizeOfXLogShortPHD);
	return true;
}

/*
 * Invalidate the xlogreader's read state to force a re-read.
 */
static void
XLogReaderInvalReadState(XLogReaderState *state)
{
	state->readPagePtr = InvalidXLogRecPtr;
}

/*
 * Validate an XLOG record header.
 *
 * This is just a convenience subroutine to avoid duplicated code in
 * XLogReadRecord.  It's not intended for use from anywhere else.
 *
 * If PrevRecPtr is valid, the xl_prev is is cross-checked with it.
 */
static bool
ValidXLogRecordHeader(XLogReaderState *state, XLogRecPtr RecPtr,
					  XLogRecPtr PrevRecPtr, XLogRecord *record)
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
	if (PrevRecPtr == InvalidXLogRecPtr)
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

XLogFindNextRecordState *
InitXLogFindNextRecord(XLogReaderState *reader_state, XLogRecPtr start_ptr)
{
	XLogFindNextRecordState *state = (XLogFindNextRecordState *)
		palloc_extended(sizeof(XLogFindNextRecordState),
						MCXT_ALLOC_NO_OOM | MCXT_ALLOC_ZERO);
	if (!state)
		return NULL;

	state->reader_state = reader_state;
	state->targetRecPtr = start_ptr;
	state->currRecPtr = start_ptr;

	return state;
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
bool
XLogFindNextRecord(XLogFindNextRecordState *state)
{
	XLogPageHeader header;
	XLogRecord *record;
	XLogReadRecordResult result;
	char	   *errormsg;

	Assert(!XLogRecPtrIsInvalid(state->currRecPtr));

	/*
	 * skip over potential continuation data, keeping in mind that it may span
	 * multiple pages
	 */
	while (true)
	{
		XLogRecPtr	targetPagePtr;
		int			targetRecOff;
		uint32		pageHeaderSize;

		/*
		 * Compute targetRecOff. It should typically be equal or greater than
		 * short page-header since a valid record can't start anywhere before
		 * that, except when caller has explicitly specified the offset that
		 * falls somewhere there or when we are skipping multi-page
		 * continuation record. It doesn't matter though because
		 * XLogNeedData() is prepared to handle that and will read at least
		 * short page-header worth of data
		 */
		targetRecOff = state->currRecPtr % XLOG_BLCKSZ;

		/* scroll back to page boundary */
		targetPagePtr = state->currRecPtr - targetRecOff;

		if (XLogNeedData(state->reader_state, targetPagePtr, targetRecOff,
							targetRecOff != 0))
			return true;

		if (!state->reader_state->page_verified)
			goto err;

		header = (XLogPageHeader) state->reader_state->readBuf;

		pageHeaderSize = XLogPageHeaderSize(header);

		/* we should have read the page header */
		Assert(state->reader_state->readLen >= pageHeaderSize);

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
				state->currRecPtr = targetPagePtr + XLOG_BLCKSZ;
			else
			{
				/*
				 * The previous continuation record ends in this page. Set
				 * state->currRecPtr to point to the first valid record
				 */
				state->currRecPtr = targetPagePtr + pageHeaderSize
					+ MAXALIGN(header->xlp_rem_len);
				break;
			}
		}
		else
		{
			state->currRecPtr = targetPagePtr + pageHeaderSize;
			break;
		}
	}

	/*
	 * we know now that tmpRecPtr is an address pointing to a valid XLogRecord
	 * because either we're at the first record after the beginning of a page
	 * or we just jumped over the remaining data of a continuation.
	 */
	XLogBeginRead(state->reader_state, state->currRecPtr);
	while ((result = XLogReadRecord(state->reader_state, &record, &errormsg)) !=
		   XLREAD_FAIL)
	{
		if (result == XLREAD_NEED_DATA)
			return true;

		/* past the record we've found, break out */
		if (state->targetRecPtr <= state->reader_state->ReadRecPtr)
		{
			/* Rewind the reader to the beginning of the last record. */
			state->currRecPtr = state->reader_state->ReadRecPtr;
			XLogBeginRead(state->reader_state, state->currRecPtr);
			return false;
		}
	}

err:
	XLogReaderInvalReadState(state->reader_state);

	state->currRecPtr = InvalidXLogRecPtr;;
	return false;
}

#endif							/* FRONTEND */

/*
 * Helper function to ease writing of routines that read raw WAL data.
 * If this function is used, caller must supply a segment_open callback and
 * segment_close callback as that is used here.
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
		WALSegmentOpenCB segopenfn, WALSegmentCloseCB segclosefn,
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
				segclosefn(state);

			XLByteToSeg(recptr, nextSegNo, state->segcxt.ws_segsize);
			segopenfn(state, nextSegNo, &tli);

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

/*
 * Private function to reset the state, forgetting all decoded records, if we
 * are asked to move to a new read position.
 */
static void
ResetDecoder(XLogReaderState *state)
{
	DecodedXLogRecord *r;

	/* Reset the decoded record queue, freeing any oversized records. */
	while ((r = state->decode_queue_tail))
	{
		state->decode_queue_tail = r->next;
		if (r->oversized)
			pfree(r);
	}
	state->decode_queue_head = NULL;
	state->decode_queue_tail = NULL;
	state->record = NULL;
	state->decoding = NULL;

	/* Reset the decode buffer to empty. */
	state->decode_buffer_head = state->decode_buffer;
	state->decode_buffer_tail = state->decode_buffer;

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
	size_t size = 0;

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
	RelFileNode *rnode = NULL;
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
									  "out-of-order block_id %u at %X/%X",
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

			blk->recent_buffer = InvalidBuffer;

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
	return XLogRecGetRecentBuffer(record, block_id, rnode, forknum, blknum,
								  NULL);
}

bool
XLogRecGetRecentBuffer(XLogReaderState *record, uint8 block_id,
					   RelFileNode *rnode, ForkNumber *forknum,
					   BlockNumber *blknum, Buffer *recent_buffer)
{
	DecodedBkpBlock *bkpb;

	if (block_id > record->record->max_block_id ||
		!record->record->blocks[block_id].in_use)
		return false;

	bkpb = &record->record->blocks[block_id];
	if (rnode)
		*rnode = bkpb->rnode;
	if (forknum)
		*forknum = bkpb->forknum;
	if (blknum)
		*blknum = bkpb->blkno;
	if (recent_buffer)
		*recent_buffer = bkpb->recent_buffer;
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
 * Returns true if a full-page image is restored.
 */
bool
RestoreBlockImage(XLogReaderState *record, uint8 block_id, char *page)
{
	DecodedBkpBlock *bkpb;
	char	   *ptr;
	PGAlignedBlock tmp;

	if (block_id > record->record->max_block_id ||
		!record->record->blocks[block_id].in_use)
		return false;
	if (!record->record->blocks[block_id].has_image)
		return false;

	bkpb = &record->record->blocks[block_id];
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
