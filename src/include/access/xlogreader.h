/*-------------------------------------------------------------------------
 *
 * xlogreader.h
 *		Definitions for the generic XLog reading facility
 *
 * Portions Copyright (c) 2013-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/include/access/xlogreader.h
 *
 * NOTES
 *		See the definition of the XLogReaderState struct for instructions on
 *		how to use the XLogReader infrastructure.
 *
 *		The basic idea is to allocate an XLogReaderState via
 *		XLogReaderAllocate(), position the reader to the first record with
 *		XLogBeginRead() or XLogFindNextRecord(), and call XLogReadRecord()
 *		until it returns NULL.
 *
 *		Callers supply a page_read callback if they want to call
 *		XLogReadRecord or XLogFindNextRecord; it can be passed in as NULL
 *		otherwise.  The WALRead function can be used as a helper to write
 *		page_read callbacks, but it is not mandatory; callers that use it,
 *		must supply segment_open callbacks.  The segment_close callback
 *		must always be supplied.
 *
 *		After reading a record with XLogReadRecord(), it's decomposed into
 *		the per-block and main data parts, and the parts can be accessed
 *		with the XLogRec* macros and functions. You can also decode a
 *		record that's already constructed in memory, without reading from
 *		disk, by calling the DecodeXLogRecord() function.
 *-------------------------------------------------------------------------
 */
#ifndef XLOGREADER_H
#define XLOGREADER_H

#ifndef FRONTEND
#include "access/transam.h"
#endif

#include "access/xlogrecord.h"

/* WALOpenSegment represents a WAL segment being read. */
typedef struct WALOpenSegment
{
	int			ws_file;		/* segment file descriptor */
	XLogSegNo	ws_segno;		/* segment number */
	TimeLineID	ws_tli;			/* timeline ID of the currently open file */
} WALOpenSegment;

/* WALSegmentContext carries context information about WAL segments to read */
typedef struct WALSegmentContext
{
	char		ws_dir[MAXPGPATH];
	int			ws_segsize;
} WALSegmentContext;

typedef struct XLogReaderState XLogReaderState;
typedef struct XLogFindNextRecordState XLogFindNextRecordState;

/* Function type definition for the segment cleanup callback */
typedef void (*WALSegmentCleanupCB) (XLogReaderState *xlogreader);

/* Function type definition for the open/close callbacks for WALRead() */
typedef void (*WALSegmentOpenCB) (XLogReaderState *xlogreader,
								  XLogSegNo nextSegNo,
								  TimeLineID *tli_p);
typedef void (*WALSegmentCloseCB) (XLogReaderState *xlogreader);

typedef struct
{
	/* Is this block ref in use? */
	bool		in_use;

	/* Identify the block this refers to */
	RelFileNode rnode;
	ForkNumber	forknum;
	BlockNumber blkno;

	/* copy of the fork_flags field from the XLogRecordBlockHeader */
	uint8		flags;

	/* Information on full-page image, if any */
	bool		has_image;		/* has image, even for consistency checking */
	bool		apply_image;	/* has image that should be restored */
	char	   *bkp_image;
	uint16		hole_offset;
	uint16		hole_length;
	uint16		bimg_len;
	uint8		bimg_info;

	/* Buffer holding the rmgr-specific data associated with this block */
	bool		has_data;
	char	   *data;
	uint16		data_len;
	uint16		data_bufsz;
} DecodedBkpBlock;

/* Return code from XLogReadRecord */
typedef enum XLogReadRecordResult
{
	XLREAD_SUCCESS,				/* record is successfully read */
	XLREAD_NEED_DATA,			/* need more data. see XLogReadRecord. */
	XLREAD_FULL,		/* cannot hold more data while reading ahead */
	XLREAD_FAIL					/* failed during reading a record */
}			XLogReadRecordResult;

/*
 * internal state of XLogReadRecord
 *
 * XLogReadState runs a state machine while reading a record. Theses states
 * are not seen outside the function. Each state may repeat several times
 * exiting requesting caller for new data. See the comment of XLogReadRecrod
 * for details.
 */
typedef enum XLogReadRecordState
{
	XLREAD_NEXT_RECORD,
	XLREAD_TOT_LEN,
	XLREAD_FIRST_FRAGMENT,
	XLREAD_CONTINUATION
}			XLogReadRecordState;

/*
 * The decoded contents of a record.  This occupies a contiguous region of
 * memory, with main_data and blocks[n].data pointing to memory after the
 * members declared here.
 */
typedef struct DecodedXLogRecord
{
	/* Private member used for resource management. */
	size_t		size;			/* total size of decoded record */
	bool		oversized;		/* outside the regular decode buffer? */
	struct DecodedXLogRecord *next;	/* decoded record queue  link */

	/* Public members. */
	XLogRecPtr	lsn;			/* location */
	XLogRecPtr	next_lsn;		/* location of next record */
	XLogRecord	header;			/* header */
	RepOriginId record_origin;
	TransactionId toplevel_xid; /* XID of top-level transaction */
	char	   *main_data;		/* record's main data portion */
	uint32		main_data_len;	/* main data portion's length */
	int			max_block_id;	/* highest block_id in use (-1 if none) */
	DecodedBkpBlock blocks[FLEXIBLE_ARRAY_MEMBER];
} DecodedXLogRecord;

struct XLogReaderState
{
	/*
	 * Operational callbacks
	 */
	WALSegmentCleanupCB cleanup_cb;

	/* ----------------------------------------
	 * Public parameters
	 * ----------------------------------------
	 */

	/*
	 * System identifier of the xlog files we're about to read.  Set to zero
	 * (the default value) if unknown or unimportant.
	 */
	uint64		system_identifier;

	/*
	 * Start and end point of last record read.  EndRecPtr is also used as the
	 * position to read next.  Calling XLogBeginRead() sets EndRecPtr to the
	 * starting position and ReadRecPtr to invalid.
	 *
	 * Start and end point of last record returned by XLogReadRecord().  These
	 * are also available as record->lsn and record->next_lsn.
	 */
	XLogRecPtr	ReadRecPtr;		/* start of last record read or being read */
	XLogRecPtr	EndRecPtr;		/* end+1 of last record read */

	/* ----------------------------------------
	 * Communication with page reader
	 * readBuf is XLOG_BLCKSZ bytes, valid up to at least reqLen bytes.
	 * ----------------------------------------
	 */
	/* variables the clients of xlogreader can examine */
	XLogRecPtr	readPagePtr;	/* page pointer to read */
	int32		reqLen;			/* bytes requested to the caller */
	char	   *readBuf;		/* buffer to store data */
	bool		page_verified;	/* is the page header on the buffer verified? */
	bool		record_verified;/* is the current record header verified? */

	/* variables set by the client of xlogreader */
	int32		readLen;		/* actual bytes copied into readBuf by client,
								 * which should be >= reqLen.  Client should
								 * use XLogReaderSetInputData() to set. */

	/* ----------------------------------------
	 * Decoded representation of current record
	 *
	 * Use XLogRecGet* functions to investigate the record; these fields
	 * should not be accessed directly.
	 * ----------------------------------------
	 * Start and end point of the last record read and decoded by
	 * XLogReadRecordInternal().  NextRecPtr is also used as the position to
	 * decode next.  Calling XLogBeginRead() sets NextRecPtr and EndRecPtr to
	 * the requested starting position.
	 */
	XLogRecPtr	DecodeRecPtr;	/* start of last record decoded */
	XLogRecPtr	NextRecPtr;		/* end+1 of last record decoded */
	XLogRecPtr	PrevRecPtr;		/* start of previous record decoded */

	/* Last record returned by XLogReadRecord(). */
	DecodedXLogRecord *record;

	/* ----------------------------------------
	 * private/internal state
	 * ----------------------------------------
	 */

	/*
	 * Buffer for decoded records.  This is a circular buffer, though
	 * individual records can't be split in the middle, so some space is often
	 * wasted at the end.  Oversized records that don't fit in this space are
	 * allocated separately.
	 */
	char	   *decode_buffer;
	size_t		decode_buffer_size;
	bool		free_decode_buffer;		/* need to free? */
	char	   *decode_buffer_head;		/* write head */
	char	   *decode_buffer_tail;		/* read head */

	/*
	 * Queue of records that have been decoded.  This is a linked list that
	 * usually consists of consecutive records in decode_buffer, but may also
	 * contain oversized records allocated with palloc().
	 */
	DecodedXLogRecord *decode_queue_head;	/* newest decoded record */
	DecodedXLogRecord *decode_queue_tail;	/* oldest decoded record */

	/* last read XLOG position for data currently in readBuf */
	WALSegmentContext segcxt;
	WALOpenSegment seg;
	uint32		segoff;

	/*
	 * beginning of prior page read, and its TLI.  Doesn't necessarily
	 * correspond to what's in readBuf; used for timeline sanity checks.
	 */
	XLogRecPtr	latestPagePtr;
	TimeLineID	latestPageTLI;

	/* timeline to read it from, 0 if a lookup is required */
	TimeLineID	currTLI;

	/*
	 * Safe point to read to in currTLI if current TLI is historical
	 * (tliSwitchPoint) or InvalidXLogRecPtr if on current timeline.
	 *
	 * Actually set to the start of the segment containing the timeline switch
	 * that ends currTLI's validity, not the LSN of the switch its self, since
	 * we can't assume the old segment will be present.
	 */
	XLogRecPtr	currTLIValidUntil;

	/*
	 * If currTLI is not the most recent known timeline, the next timeline to
	 * read from when currTLIValidUntil is reached.
	 */
	TimeLineID	nextTLI;

	/*
	 * Buffer for current ReadRecord result (expandable), used when a record
	 * crosses a page boundary.
	 */
	char	   *readRecordBuf;
	uint32		readRecordBufSize;

	/*
	 * XLogReadRecordInternal() state
	 */
	XLogReadRecordState readRecordState;	/* state machine state */
	int			recordGotLen;	/* amount of current record that has already
								 * been read */
	int			recordRemainLen;	/* length of current record that remains */
	XLogRecPtr	recordContRecPtr;	/* where the current record continues */

	DecodedXLogRecord *decoding;	/* record currently being decoded */

	/* Buffer to hold error message */
	char	   *errormsg_buf;
	bool		errormsg_deferred;
};

struct XLogFindNextRecordState
{
	XLogReaderState *reader_state;
	XLogRecPtr		targetRecPtr;
	XLogRecPtr		currRecPtr;
};

/* Report that data is available for decoding. */
static inline void
XLogReaderSetInputData(XLogReaderState *state, int32 len)
{
	state->readLen = len;
}

/* Get a new XLogReader */
extern XLogReaderState *XLogReaderAllocate(int wal_segment_size,
										   const char *waldir,
										   WALSegmentCleanupCB cleanup_cb);

/* Free an XLogReader */
extern void XLogReaderFree(XLogReaderState *state);

/* Optionally provide a circular decoding buffer to allow readahead. */
extern void XLogReaderSetDecodeBuffer(XLogReaderState *state,
									  void *buffer,
									  size_t size);

/* Position the XLogReader to given record */
extern void XLogBeginRead(XLogReaderState *state, XLogRecPtr RecPtr);
#ifdef FRONTEND
extern XLogFindNextRecordState *InitXLogFindNextRecord(XLogReaderState *reader_state, XLogRecPtr start_ptr);
extern bool XLogFindNextRecord(XLogFindNextRecordState *state);
#endif							/* FRONTEND */

/* Read the next record's header.  Returns NULL on end-of-WAL or failure. */
extern XLogReadRecordResult XLogReadRecord(XLogReaderState *state,
										   XLogRecord **record,
										   char **errormsg);

/* Read the next decoded record.  Returns NULL on end-of-WAL or failure. */
extern XLogReadRecordResult XLogNextRecord(XLogReaderState *state,
										   DecodedXLogRecord **record,
										   char **errormsg);

/* Try to read ahead, if there is space in the decoding buffer. */
extern XLogReadRecordResult XLogReadAhead(XLogReaderState *state,
										  DecodedXLogRecord **record,
										  char **errormsg);

/* Validate a page */
extern bool XLogReaderValidatePageHeader(XLogReaderState *state,
										 XLogRecPtr recptr, char *phdr);

/*
 * Error information from WALRead that both backend and frontend caller can
 * process.  Currently only errors from pg_pread can be reported.
 */
typedef struct WALReadError
{
	int			wre_errno;		/* errno set by the last pg_pread() */
	int			wre_off;		/* Offset we tried to read from. */
	int			wre_req;		/* Bytes requested to be read. */
	int			wre_read;		/* Bytes read by the last read(). */
	WALOpenSegment wre_seg;		/* Segment we tried to read from. */
} WALReadError;

extern bool WALRead(XLogReaderState *state,
					WALSegmentOpenCB segopenfn, WALSegmentCloseCB sgclosefn,
					char *buf, XLogRecPtr startptr, Size count,
					TimeLineID tli, WALReadError *errinfo);

/* Functions for decoding an XLogRecord */

extern size_t DecodeXLogRecordRequiredSpace(size_t xl_tot_len);
extern bool DecodeXLogRecord(XLogReaderState *state,
							 DecodedXLogRecord *decoded,
							 XLogRecord *record,
							 XLogRecPtr lsn,
							 char **errmsg);

#define XLogRecGetTotalLen(decoder) ((decoder)->record->header.xl_tot_len)
#define XLogRecGetPrev(decoder) ((decoder)->record->header.xl_prev)
#define XLogRecGetInfo(decoder) ((decoder)->record->header.xl_info)
#define XLogRecGetRmid(decoder) ((decoder)->record->header.xl_rmid)
#define XLogRecGetXid(decoder) ((decoder)->record->header.xl_xid)
#define XLogRecGetOrigin(decoder) ((decoder)->record->record_origin)
#define XLogRecGetTopXid(decoder) ((decoder)->record->toplevel_xid)
#define XLogRecGetData(decoder) ((decoder)->record->main_data)
#define XLogRecGetDataLen(decoder) ((decoder)->record->main_data_len)
#define XLogRecHasAnyBlockRefs(decoder) ((decoder)->record->max_block_id >= 0)
#define XLogRecMaxBlockId(decoder) ((decoder)->record->max_block_id)
#define XLogRecGetBlock(decoder, i) (&(decoder)->record->blocks[(i)])
#define XLogRecHasBlockRef(decoder, block_id) \
	((decoder)->record->max_block_id >= (block_id))  && \
	((decoder)->record->blocks[block_id].in_use)
#define XLogRecHasBlockImage(decoder, block_id) \
	((decoder)->record->blocks[block_id].has_image)
#define XLogRecBlockImageApply(decoder, block_id) \
	((decoder)->record->blocks[block_id].apply_image)

#ifndef FRONTEND
extern FullTransactionId XLogRecGetFullXid(XLogReaderState *record);
#endif

extern bool RestoreBlockImage(XLogReaderState *record, uint8 block_id, char *page);
extern char *XLogRecGetBlockData(XLogReaderState *record, uint8 block_id, Size *len);
extern bool XLogRecGetBlockTag(XLogReaderState *record, uint8 block_id,
							   RelFileNode *rnode, ForkNumber *forknum,
							   BlockNumber *blknum);

#endif							/* XLOGREADER_H */
