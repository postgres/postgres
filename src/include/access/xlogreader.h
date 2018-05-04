/*-------------------------------------------------------------------------
 *
 * xlogreader.h
 *		Definitions for the generic XLog reading facility
 *
 * Portions Copyright (c) 2013-2014, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/include/access/xlogreader.h
 *
 * NOTES
 *		See the definition of the XLogReaderState struct for instructions on
 *		how to use the XLogReader infrastructure.
 *
 *		The basic idea is to allocate an XLogReaderState via
 *		XLogReaderAllocate(), and call XLogReadRecord() until it returns NULL.
 *-------------------------------------------------------------------------
 */
#ifndef XLOGREADER_H
#define XLOGREADER_H

#include "access/xlog_internal.h"

typedef struct XLogReaderState XLogReaderState;

/* Function type definition for the read_page callback */
typedef int (*XLogPageReadCB) (XLogReaderState *xlogreader,
										   XLogRecPtr targetPagePtr,
										   int reqLen,
										   XLogRecPtr targetRecPtr,
										   char *readBuf,
										   TimeLineID *pageTLI);

struct XLogReaderState
{
	/* ----------------------------------------
	 * Public parameters
	 * ----------------------------------------
	 */

	/*
	 * Data input callback (mandatory).
	 *
	 * This callback shall read at least reqLen valid bytes of the xlog page
	 * starting at targetPagePtr, and store them in readBuf.  The callback
	 * shall return the number of bytes read (never more than XLOG_BLCKSZ), or
	 * -1 on failure.  The callback shall sleep, if necessary, to wait for the
	 * requested bytes to become available.  The callback will not be invoked
	 * again for the same page unless more than the returned number of bytes
	 * are needed.
	 *
	 * targetRecPtr is the position of the WAL record we're reading.  Usually
	 * it is equal to targetPagePtr + reqLen, but sometimes xlogreader needs
	 * to read and verify the page or segment header, before it reads the
	 * actual WAL record it's interested in.  In that case, targetRecPtr can
	 * be used to determine which timeline to read the page from.
	 *
	 * The callback shall set *pageTLI to the TLI of the file the page was
	 * read from.  It is currently used only for error reporting purposes, to
	 * reconstruct the name of the WAL file where an error occurred.
	 */
	XLogPageReadCB read_page;

	/*
	 * System identifier of the xlog files we're about to read.  Set to zero
	 * (the default value) if unknown or unimportant.
	 */
	uint64		system_identifier;

	/*
	 * Opaque data for callbacks to use.  Not used by XLogReader.
	 */
	void	   *private_data;

	/*
	 * Start and end point of last record read.  EndRecPtr is also used as the
	 * position to read next, if XLogReadRecord receives an invalid recptr.
	 */
	XLogRecPtr	ReadRecPtr;		/* start of last record read */
	XLogRecPtr	EndRecPtr;		/* end+1 of last record read */

	/* ----------------------------------------
	 * private/internal state
	 * ----------------------------------------
	 */

	/* Buffer for currently read page (XLOG_BLCKSZ bytes) */
	char	   *readBuf;

	/* last read segment, segment offset, read length, TLI */
	XLogSegNo	readSegNo;
	uint32		readOff;
	uint32		readLen;
	TimeLineID	readPageTLI;

	/* beginning of last page read, and its TLI  */
	XLogRecPtr	latestPagePtr;
	TimeLineID	latestPageTLI;

	/* beginning of the WAL record being read. */
	XLogRecPtr	currRecPtr;

	/* Buffer for current ReadRecord result (expandable) */
	char	   *readRecordBuf;
	uint32		readRecordBufSize;

	/* Buffer to hold error message */
	char	   *errormsg_buf;
};

/* Get a new XLogReader */
extern XLogReaderState *XLogReaderAllocate(XLogPageReadCB pagereadfunc,
				   void *private_data);

/* Free an XLogReader */
extern void XLogReaderFree(XLogReaderState *state);

/* Read the next XLog record. Returns NULL on end-of-WAL or failure */
extern struct XLogRecord *XLogReadRecord(XLogReaderState *state,
			   XLogRecPtr recptr, char **errormsg);

/* Validate a page */
extern bool XLogReaderValidatePageHeader(XLogReaderState *state,
					XLogRecPtr recptr, char *phdr);

#ifdef FRONTEND
extern XLogRecPtr XLogFindNextRecord(XLogReaderState *state, XLogRecPtr RecPtr);
#endif   /* FRONTEND */

#endif   /* XLOGREADER_H */
