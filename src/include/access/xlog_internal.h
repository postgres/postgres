/*
 * xlog_internal.h
 *
 * PostgreSQL transaction log internal declarations
 *
 * NOTE: this file is intended to contain declarations useful for
 * manipulating the XLOG files directly, but it is not supposed to be
 * needed by rmgr routines (redo support for individual record types).
 * So the XLogRecord typedef and associated stuff appear in xlog.h.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/xlog_internal.h,v 1.17 2006/10/04 00:30:07 momjian Exp $
 */
#ifndef XLOG_INTERNAL_H
#define XLOG_INTERNAL_H

#include <time.h>

#include "access/xlog.h"
#include "fmgr.h"
#include "storage/block.h"
#include "storage/relfilenode.h"


/*
 * Header info for a backup block appended to an XLOG record.
 *
 * As a trivial form of data compression, the XLOG code is aware that
 * PG data pages usually contain an unused "hole" in the middle, which
 * contains only zero bytes.  If hole_length > 0 then we have removed
 * such a "hole" from the stored data (and it's not counted in the
 * XLOG record's CRC, either).  Hence, the amount of block data actually
 * present following the BkpBlock struct is BLCKSZ - hole_length bytes.
 *
 * Note that we don't attempt to align either the BkpBlock struct or the
 * block's data.  So, the struct must be copied to aligned local storage
 * before use.
 */
typedef struct BkpBlock
{
	RelFileNode node;			/* relation containing block */
	BlockNumber block;			/* block number */
	uint16		hole_offset;	/* number of bytes before "hole" */
	uint16		hole_length;	/* number of bytes in "hole" */

	/* ACTUAL BLOCK DATA FOLLOWS AT END OF STRUCT */
} BkpBlock;

/*
 * When there is not enough space on current page for whole record, we
 * continue on the next page with continuation record.	(However, the
 * XLogRecord header will never be split across pages; if there's less than
 * SizeOfXLogRecord space left at the end of a page, we just waste it.)
 *
 * Note that xl_rem_len includes backup-block data; that is, it tracks
 * xl_tot_len not xl_len in the initial header.  Also note that the
 * continuation data isn't necessarily aligned.
 */
typedef struct XLogContRecord
{
	uint32		xl_rem_len;		/* total len of remaining data for record */

	/* ACTUAL LOG DATA FOLLOWS AT END OF STRUCT */

} XLogContRecord;

#define SizeOfXLogContRecord	sizeof(XLogContRecord)

/*
 * Each page of XLOG file has a header like this:
 */
#define XLOG_PAGE_MAGIC 0xD05E	/* can be used as WAL version indicator */

typedef struct XLogPageHeaderData
{
	uint16		xlp_magic;		/* magic value for correctness checks */
	uint16		xlp_info;		/* flag bits, see below */
	TimeLineID	xlp_tli;		/* TimeLineID of first record on page */
	XLogRecPtr	xlp_pageaddr;	/* XLOG address of this page */
} XLogPageHeaderData;

#define SizeOfXLogShortPHD	MAXALIGN(sizeof(XLogPageHeaderData))

typedef XLogPageHeaderData *XLogPageHeader;

/*
 * When the XLP_LONG_HEADER flag is set, we store additional fields in the
 * page header.  (This is ordinarily done just in the first page of an
 * XLOG file.)	The additional fields serve to identify the file accurately.
 */
typedef struct XLogLongPageHeaderData
{
	XLogPageHeaderData std;		/* standard header fields */
	uint64		xlp_sysid;		/* system identifier from pg_control */
	uint32		xlp_seg_size;	/* just as a cross-check */
	uint32		xlp_xlog_blcksz;	/* just as a cross-check */
} XLogLongPageHeaderData;

#define SizeOfXLogLongPHD	MAXALIGN(sizeof(XLogLongPageHeaderData))

typedef XLogLongPageHeaderData *XLogLongPageHeader;

/* When record crosses page boundary, set this flag in new page's header */
#define XLP_FIRST_IS_CONTRECORD		0x0001
/* This flag indicates a "long" page header */
#define XLP_LONG_HEADER				0x0002
/* All defined flag bits in xlp_info (used for validity checking of header) */
#define XLP_ALL_FLAGS				0x0003

#define XLogPageHeaderSize(hdr)		\
	(((hdr)->xlp_info & XLP_LONG_HEADER) ? SizeOfXLogLongPHD : SizeOfXLogShortPHD)

/*
 * We break each logical log file (xlogid value) into segment files of the
 * size indicated by XLOG_SEG_SIZE.  One possible segment at the end of each
 * log file is wasted, to ensure that we don't have problems representing
 * last-byte-position-plus-1.
 */
#define XLogSegSize		((uint32) XLOG_SEG_SIZE)
#define XLogSegsPerFile (((uint32) 0xffffffff) / XLogSegSize)
#define XLogFileSize	(XLogSegsPerFile * XLogSegSize)


/*
 * Macros for manipulating XLOG pointers
 */

/* Increment an xlogid/segment pair */
#define NextLogSeg(logId, logSeg)	\
	do { \
		if ((logSeg) >= XLogSegsPerFile-1) \
		{ \
			(logId)++; \
			(logSeg) = 0; \
		} \
		else \
			(logSeg)++; \
	} while (0)

/* Decrement an xlogid/segment pair (assume it's not 0,0) */
#define PrevLogSeg(logId, logSeg)	\
	do { \
		if (logSeg) \
			(logSeg)--; \
		else \
		{ \
			(logId)--; \
			(logSeg) = XLogSegsPerFile-1; \
		} \
	} while (0)

/*
 * Compute ID and segment from an XLogRecPtr.
 *
 * For XLByteToSeg, do the computation at face value.  For XLByteToPrevSeg,
 * a boundary byte is taken to be in the previous segment.	This is suitable
 * for deciding which segment to write given a pointer to a record end,
 * for example.  (We can assume xrecoff is not zero, since no valid recptr
 * can have that.)
 */
#define XLByteToSeg(xlrp, logId, logSeg)	\
	( logId = (xlrp).xlogid, \
	  logSeg = (xlrp).xrecoff / XLogSegSize \
	)
#define XLByteToPrevSeg(xlrp, logId, logSeg)	\
	( logId = (xlrp).xlogid, \
	  logSeg = ((xlrp).xrecoff - 1) / XLogSegSize \
	)

/*
 * Is an XLogRecPtr within a particular XLOG segment?
 *
 * For XLByteInSeg, do the computation at face value.  For XLByteInPrevSeg,
 * a boundary byte is taken to be in the previous segment.
 */
#define XLByteInSeg(xlrp, logId, logSeg)	\
	((xlrp).xlogid == (logId) && \
	 (xlrp).xrecoff / XLogSegSize == (logSeg))

#define XLByteInPrevSeg(xlrp, logId, logSeg)	\
	((xlrp).xlogid == (logId) && \
	 ((xlrp).xrecoff - 1) / XLogSegSize == (logSeg))

/* Check if an xrecoff value is in a plausible range */
#define XRecOffIsValid(xrecoff) \
		((xrecoff) % XLOG_BLCKSZ >= SizeOfXLogShortPHD && \
		(XLOG_BLCKSZ - (xrecoff) % XLOG_BLCKSZ) >= SizeOfXLogRecord)

/*
 * The XLog directory and control file (relative to $PGDATA)
 */
#define XLOGDIR				"pg_xlog"
#define XLOG_CONTROL_FILE	"global/pg_control"

/*
 * These macros encapsulate knowledge about the exact layout of XLog file
 * names, timeline history file names, and archive-status file names.
 */
#define MAXFNAMELEN		64

#define XLogFileName(fname, tli, log, seg)	\
	snprintf(fname, MAXFNAMELEN, "%08X%08X%08X", tli, log, seg)

#define XLogFilePath(path, tli, log, seg)	\
	snprintf(path, MAXPGPATH, XLOGDIR "/%08X%08X%08X", tli, log, seg)

#define TLHistoryFileName(fname, tli)	\
	snprintf(fname, MAXFNAMELEN, "%08X.history", tli)

#define TLHistoryFilePath(path, tli)	\
	snprintf(path, MAXPGPATH, XLOGDIR "/%08X.history", tli)

#define StatusFilePath(path, xlog, suffix)	\
	snprintf(path, MAXPGPATH, XLOGDIR "/archive_status/%s%s", xlog, suffix)

#define BackupHistoryFileName(fname, tli, log, seg, offset) \
	snprintf(fname, MAXFNAMELEN, "%08X%08X%08X.%08X.backup", tli, log, seg, offset)

#define BackupHistoryFilePath(path, tli, log, seg, offset)	\
	snprintf(path, MAXPGPATH, XLOGDIR "/%08X%08X%08X.%08X.backup", tli, log, seg, offset)


/*
 * Method table for resource managers.
 *
 * RmgrTable[] is indexed by RmgrId values (see rmgr.h).
 */
typedef struct RmgrData
{
	const char *rm_name;
	void		(*rm_redo) (XLogRecPtr lsn, XLogRecord *rptr);
	void		(*rm_desc) (StringInfo buf, uint8 xl_info, char *rec);
	void		(*rm_startup) (void);
	void		(*rm_cleanup) (void);
	bool		(*rm_safe_restartpoint) (void);
} RmgrData;

extern const RmgrData RmgrTable[];

/*
 * Exported to support xlog switching from bgwriter
 */
extern time_t GetLastSegSwitchTime(void);
extern XLogRecPtr RequestXLogSwitch(void);

/*
 * These aren't in xlog.h because I'd rather not include fmgr.h there.
 */
extern Datum pg_start_backup(PG_FUNCTION_ARGS);
extern Datum pg_stop_backup(PG_FUNCTION_ARGS);
extern Datum pg_switch_xlog(PG_FUNCTION_ARGS);
extern Datum pg_current_xlog_location(PG_FUNCTION_ARGS);
extern Datum pg_current_xlog_insert_location(PG_FUNCTION_ARGS);
extern Datum pg_xlogfile_name_offset(PG_FUNCTION_ARGS);
extern Datum pg_xlogfile_name(PG_FUNCTION_ARGS);

#endif   /* XLOG_INTERNAL_H */
