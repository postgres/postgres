/*
 * xlog.h
 *
 * PostgreSQL transaction log manager
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/xlog.h,v 1.59 2004/12/31 22:03:21 pgsql Exp $
 */
#ifndef XLOG_H
#define XLOG_H

#include "access/rmgr.h"
#include "access/transam.h"
#include "access/xlogdefs.h"
#include "storage/buf.h"
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
	TransactionId xl_xid;		/* xact id */
	uint32		xl_len;			/* total len of rmgr data */
	uint8		xl_info;		/* flag bits, see below */
	RmgrId		xl_rmid;		/* resource manager for this record */

	/* Depending on MAXALIGN, there are either 2 or 6 wasted bytes here */

	/* ACTUAL LOG DATA FOLLOWS AT END OF STRUCT */

} XLogRecord;

#define SizeOfXLogRecord	MAXALIGN(sizeof(XLogRecord))

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
 */
typedef struct XLogRecData
{
	Buffer		buffer;			/* buffer associated with this data */
	char	   *data;
	uint32		len;
	struct XLogRecData *next;
} XLogRecData;

extern TimeLineID ThisTimeLineID;		/* current TLI */
extern bool InRecovery;
extern XLogRecPtr MyLastRecPtr;
extern bool MyXactMadeXLogEntry;
extern bool MyXactMadeTempRelUpdate;
extern XLogRecPtr ProcLastRecEnd;

/* these variables are GUC parameters related to XLOG */
extern int	CheckPointSegments;
extern int	XLOGbuffers;
extern char *XLogArchiveCommand;
extern char *XLOG_sync_method;
extern const char XLOG_sync_method_default[];

#define XLogArchivingActive()	(XLogArchiveCommand[0] != '\0')

#ifdef WAL_DEBUG
extern bool XLOG_DEBUG;
#endif

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
extern void ShutdownXLOG(int code, Datum arg);
extern void InitXLOGAccess(void);
extern void CreateCheckPoint(bool shutdown, bool force);
extern void XLogPutNextOid(Oid nextOid);
extern XLogRecPtr GetRedoRecPtr(void);

#endif   /* XLOG_H */
