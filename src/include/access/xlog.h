/*
 * xlog.h
 *
 * PostgreSQL transaction log manager
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: xlog.h,v 1.44 2003/08/04 02:40:10 momjian Exp $
 */
#ifndef XLOG_H
#define XLOG_H

#include "access/rmgr.h"
#include "access/transam.h"
#include "access/xlogdefs.h"
#include "storage/bufmgr.h"
#include "utils/pg_crc.h"


/*
 * Header for each record in XLOG
 *
 * NOTE: xl_len counts only the rmgr data, not the XLogRecord header,
 * and also not any backup blocks appended to the record (which are signaled
 * by xl_info flag bits).  The total space needed for an XLOG record is
 * really:
 *
 * SizeOfXLogRecord + xl_len + n_backup_blocks * (sizeof(BkpBlock) + BLCKSZ)
 *
 * rounded up to a MAXALIGN boundary (so that all xlog records start on
 * MAXALIGN boundaries).
 */
typedef struct XLogRecord
{
	crc64		xl_crc;			/* CRC for this record */
	XLogRecPtr	xl_prev;		/* ptr to previous record in log */
	XLogRecPtr	xl_xact_prev;	/* ptr to previous record of this xact */
	TransactionId xl_xid;		/* xact id */
	uint16		xl_len;			/* total len of rmgr data */
	uint8		xl_info;		/* flag bits, see below */
	RmgrId		xl_rmid;		/* resource manager for this record */

	/* ACTUAL LOG DATA FOLLOWS AT END OF STRUCT */

} XLogRecord;

#define SizeOfXLogRecord	MAXALIGN(sizeof(XLogRecord))
#define MAXLOGRECSZ			65535		/* the most that'll fit in xl_len */

#define XLogRecGetData(record)	((char*) (record) + SizeOfXLogRecord)

/*
 * XLOG uses only low 4 bits of xl_info.  High 4 bits may be used by rmgr.
 */
#define XLR_INFO_MASK			0x0F

/*
 * If we backed up any disk blocks with the XLOG record, we use flag bits in
 * xl_info to signal it.  We support backup of up to 3 disk blocks per XLOG
 * record.	(Could support 4 if we cared to dedicate all the xl_info bits for
 * this purpose; currently bit 0 of xl_info is unused and available.)
 */
#define XLR_BKP_BLOCK_MASK		0x0E	/* all info bits used for bkp
										 * blocks */
#define XLR_MAX_BKP_BLOCKS		3
#define XLR_SET_BKP_BLOCK(iblk) (0x08 >> (iblk))
#define XLR_BKP_BLOCK_1			XLR_SET_BKP_BLOCK(0)	/* 0x08 */
#define XLR_BKP_BLOCK_2			XLR_SET_BKP_BLOCK(1)	/* 0x04 */
#define XLR_BKP_BLOCK_3			XLR_SET_BKP_BLOCK(2)	/* 0x02 */

/*
 * Sometimes we log records which are out of transaction control.
 * Rmgr may "or" XLOG_NO_TRAN into info passed to XLogInsert to indicate this.
 */
#define XLOG_NO_TRAN			XLR_INFO_MASK

/*
 * Header info for a backup block appended to an XLOG record.
 *
 * Note that the backup block has its own CRC, and is not covered by
 * the CRC of the XLOG record proper.  Also note that we don't attempt
 * to align either the BkpBlock struct or the block's data.
 */
typedef struct BkpBlock
{
	crc64		crc;
	RelFileNode node;
	BlockNumber block;
} BkpBlock;

/*
 * When there is not enough space on current page for whole record, we
 * continue on the next page with continuation record.	(However, the
 * XLogRecord header will never be split across pages; if there's less than
 * SizeOfXLogRecord space left at the end of a page, we just waste it.)
 *
 * Note that xl_rem_len includes backup-block data, unlike xl_len in the
 * initial header.
 */
typedef struct XLogContRecord
{
	uint32		xl_rem_len;		/* total len of remaining data for record */

	/* ACTUAL LOG DATA FOLLOWS AT END OF STRUCT */

} XLogContRecord;

#define SizeOfXLogContRecord	MAXALIGN(sizeof(XLogContRecord))

/*
 * Each page of XLOG file has a header like this:
 */
#define XLOG_PAGE_MAGIC 0xD05A	/* can be used as WAL version indicator */

typedef struct XLogPageHeaderData
{
	uint16		xlp_magic;		/* magic value for correctness checks */
	uint16		xlp_info;		/* flag bits, see below */
	StartUpID	xlp_sui;		/* StartUpID of first record on page */
	XLogRecPtr	xlp_pageaddr;	/* XLOG address of this page */
} XLogPageHeaderData;

#define SizeOfXLogPHD	MAXALIGN(sizeof(XLogPageHeaderData))

typedef XLogPageHeaderData *XLogPageHeader;

/* When record crosses page boundary, set this flag in new page's header */
#define XLP_FIRST_IS_CONTRECORD		0x0001
/* All defined flag bits in xlp_info (used for validity checking of header) */
#define XLP_ALL_FLAGS				0x0001

/*
 * We break each logical log file (xlogid value) into 16Mb segments.
 * One possible segment at the end of each log file is wasted, to ensure
 * that we don't have problems representing last-byte-position-plus-1.
 */
#define XLogSegSize		((uint32) (16*1024*1024))
#define XLogSegsPerFile (((uint32) 0xffffffff) / XLogSegSize)
#define XLogFileSize	(XLogSegsPerFile * XLogSegSize)

/*
 * Method table for resource managers.
 *
 * RmgrTable[] is indexed by RmgrId values (see rmgr.h).
 */
typedef struct RmgrData
{
	const char *rm_name;
	void		(*rm_redo) (XLogRecPtr lsn, XLogRecord *rptr);
	void		(*rm_undo) (XLogRecPtr lsn, XLogRecord *rptr);
	void		(*rm_desc) (char *buf, uint8 xl_info, char *rec);
	void		(*rm_startup) (void);
	void		(*rm_cleanup) (void);
} RmgrData;

extern RmgrData RmgrTable[];

/*--------------------
 * List of these structs is used to pass data to XLogInsert().
 *
 * If buffer is valid then XLOG will check if buffer must be backed up
 * (ie, whether this is first change of that page since last checkpoint).
 * If so, the whole page contents are attached to the XLOG record, and XLOG
 * sets XLR_BKP_BLOCK_X bit in xl_info.  Note that the buffer must be pinned
 * and locked while this is going on, so that it won't change under us.
 * NB: when this happens, we do not bother to insert the associated data into
 * the XLOG record, since we assume it's present in the buffer.  Therefore,
 * rmgr redo routines MUST pay attention to XLR_BKP_BLOCK_X to know what
 * is actually stored in the XLOG record.
 *--------------------
 */
typedef struct XLogRecData
{
	Buffer		buffer;			/* buffer associated with this data */
	char	   *data;
	uint32		len;
	struct XLogRecData *next;
} XLogRecData;

extern StartUpID ThisStartUpID; /* current SUI */
extern bool InRecovery;
extern XLogRecPtr MyLastRecPtr;
extern bool MyXactMadeXLogEntry;
extern bool MyXactMadeTempRelUpdate;
extern XLogRecPtr ProcLastRecEnd;

/* these variables are GUC parameters related to XLOG */
extern int	CheckPointSegments;
extern int	CheckPointWarning;
extern int	XLOGbuffers;
extern int	XLOG_DEBUG;
extern char *XLOG_sync_method;
extern const char XLOG_sync_method_default[];


extern XLogRecPtr XLogInsert(RmgrId rmid, uint8 info, XLogRecData *rdata);
extern void XLogFlush(XLogRecPtr RecPtr);

extern void xlog_redo(XLogRecPtr lsn, XLogRecord *record);
extern void xlog_undo(XLogRecPtr lsn, XLogRecord *record);
extern void xlog_desc(char *buf, uint8 xl_info, char *rec);

extern void UpdateControlFile(void);
extern int	XLOGShmemSize(void);
extern void XLOGShmemInit(void);
extern void XLOGPathInit(void);
extern void BootStrapXLOG(void);
extern void StartupXLOG(void);
extern void ShutdownXLOG(void);
extern void CreateCheckPoint(bool shutdown, bool force);
extern void SetThisStartUpID(void);
extern void XLogPutNextOid(Oid nextOid);
extern void SetSavedRedoRecPtr(void);
extern void GetSavedRedoRecPtr(void);
extern XLogRecPtr GetRedoRecPtr(void);

/* in storage/ipc/sinval.c, but don't want to declare in sinval.h because
 * we'd have to include xlog.h into that ...
 */
extern XLogRecPtr GetUndoRecPtr(void);

extern const char *assign_xlog_sync_method(const char *method,
						bool doit, bool interactive);

#endif   /* XLOG_H */
