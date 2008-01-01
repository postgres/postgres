/*
 * xlog.h
 *
 * PostgreSQL transaction log manager
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/xlog.h,v 1.87 2008/01/01 19:45:56 momjian Exp $
 */
#ifndef XLOG_H
#define XLOG_H

#include "access/rmgr.h"
#include "access/xlogdefs.h"
#include "lib/stringinfo.h"
#include "storage/buf.h"
#include "utils/pg_crc.h"
#include "utils/timestamp.h"


/*
 * The overall layout of an XLOG record is:
 *		Fixed-size header (XLogRecord struct)
 *		rmgr-specific data
 *		BkpBlock
 *		backup block data
 *		BkpBlock
 *		backup block data
 *		...
 *
 * where there can be zero to three backup blocks (as signaled by xl_info flag
 * bits).  XLogRecord structs always start on MAXALIGN boundaries in the WAL
 * files, and we round up SizeOfXLogRecord so that the rmgr data is also
 * guaranteed to begin on a MAXALIGN boundary.	However, no padding is added
 * to align BkpBlock structs or backup block data.
 *
 * NOTE: xl_len counts only the rmgr data, not the XLogRecord header,
 * and also not any backup blocks.	xl_tot_len counts everything.  Neither
 * length field is rounded up to an alignment boundary.
 */
typedef struct XLogRecord
{
	pg_crc32	xl_crc;			/* CRC for this record */
	XLogRecPtr	xl_prev;		/* ptr to previous record in log */
	TransactionId xl_xid;		/* xact id */
	uint32		xl_tot_len;		/* total len of entire record */
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
 * record.
 */
#define XLR_BKP_BLOCK_MASK		0x0E	/* all info bits used for bkp blocks */
#define XLR_MAX_BKP_BLOCKS		3
#define XLR_SET_BKP_BLOCK(iblk) (0x08 >> (iblk))
#define XLR_BKP_BLOCK_1			XLR_SET_BKP_BLOCK(0)	/* 0x08 */
#define XLR_BKP_BLOCK_2			XLR_SET_BKP_BLOCK(1)	/* 0x04 */
#define XLR_BKP_BLOCK_3			XLR_SET_BKP_BLOCK(2)	/* 0x02 */

/*
 * Bit 0 of xl_info is set if the backed-up blocks could safely be removed
 * from a compressed version of XLOG (that is, they are backed up only to
 * prevent partial-page-write problems, and not to ensure consistency of PITR
 * recovery).  The compression algorithm would need to extract data from the
 * blocks to create an equivalent non-full-page XLOG record.
 */
#define XLR_BKP_REMOVABLE		0x01

/* Sync methods */
#define SYNC_METHOD_FSYNC		0
#define SYNC_METHOD_FDATASYNC	1
#define SYNC_METHOD_OPEN		2		/* for O_SYNC and O_DSYNC */
#define SYNC_METHOD_FSYNC_WRITETHROUGH	3
extern int	sync_method;

/*
 * The rmgr data to be written by XLogInsert() is defined by a chain of
 * one or more XLogRecData structs.  (Multiple structs would be used when
 * parts of the source data aren't physically adjacent in memory, or when
 * multiple associated buffers need to be specified.)
 *
 * If buffer is valid then XLOG will check if buffer must be backed up
 * (ie, whether this is first change of that page since last checkpoint).
 * If so, the whole page contents are attached to the XLOG record, and XLOG
 * sets XLR_BKP_BLOCK_X bit in xl_info.  Note that the buffer must be pinned
 * and exclusive-locked by the caller, so that it won't change under us.
 * NB: when the buffer is backed up, we DO NOT insert the data pointed to by
 * this XLogRecData struct into the XLOG record, since we assume it's present
 * in the buffer.  Therefore, rmgr redo routines MUST pay attention to
 * XLR_BKP_BLOCK_X to know what is actually stored in the XLOG record.
 * The i'th XLR_BKP_BLOCK bit corresponds to the i'th distinct buffer
 * value (ignoring InvalidBuffer) appearing in the rdata chain.
 *
 * When buffer is valid, caller must set buffer_std to indicate whether the
 * page uses standard pd_lower/pd_upper header fields.	If this is true, then
 * XLOG is allowed to omit the free space between pd_lower and pd_upper from
 * the backed-up page image.  Note that even when buffer_std is false, the
 * page MUST have an LSN field as its first eight bytes!
 *
 * Note: data can be NULL to indicate no rmgr data associated with this chain
 * entry.  This can be sensible (ie, not a wasted entry) if buffer is valid.
 * The implication is that the buffer has been changed by the operation being
 * logged, and so may need to be backed up, but the change can be redone using
 * only information already present elsewhere in the XLOG entry.
 */
typedef struct XLogRecData
{
	char	   *data;			/* start of rmgr data to include */
	uint32		len;			/* length of rmgr data to include */
	Buffer		buffer;			/* buffer associated with data, if any */
	bool		buffer_std;		/* buffer has standard pd_lower/pd_upper */
	struct XLogRecData *next;	/* next struct in chain, or NULL */
} XLogRecData;

extern TimeLineID ThisTimeLineID;		/* current TLI */
extern bool InRecovery;
extern XLogRecPtr XactLastRecEnd;

/* these variables are GUC parameters related to XLOG */
extern int	CheckPointSegments;
extern int	XLOGbuffers;
extern bool XLogArchiveMode;
extern char *XLogArchiveCommand;
extern int	XLogArchiveTimeout;
extern char *XLOG_sync_method;
extern const char XLOG_sync_method_default[];
extern bool log_checkpoints;

#define XLogArchivingActive()	(XLogArchiveMode)
#define XLogArchiveCommandSet() (XLogArchiveCommand[0] != '\0')

#ifdef WAL_DEBUG
extern bool XLOG_DEBUG;
#endif

/*
 * OR-able request flag bits for checkpoints.  The "cause" bits are used only
 * for logging purposes.  Note: the flags must be defined so that it's
 * sensible to OR together request flags arising from different requestors.
 */

/* These directly affect the behavior of CreateCheckPoint and subsidiaries */
#define CHECKPOINT_IS_SHUTDOWN	0x0001	/* Checkpoint is for shutdown */
#define CHECKPOINT_IMMEDIATE	0x0002	/* Do it without delays */
#define CHECKPOINT_FORCE		0x0004	/* Force even if no activity */
/* These are important to RequestCheckpoint */
#define CHECKPOINT_WAIT			0x0008	/* Wait for completion */
/* These indicate the cause of a checkpoint request */
#define CHECKPOINT_CAUSE_XLOG	0x0010	/* XLOG consumption */
#define CHECKPOINT_CAUSE_TIME	0x0020	/* Elapsed time */

/* Checkpoint statistics */
typedef struct CheckpointStatsData
{
	TimestampTz ckpt_start_t;	/* start of checkpoint */
	TimestampTz ckpt_write_t;	/* start of flushing buffers */
	TimestampTz ckpt_sync_t;	/* start of fsyncs */
	TimestampTz ckpt_sync_end_t;	/* end of fsyncs */
	TimestampTz ckpt_end_t;		/* end of checkpoint */

	int			ckpt_bufs_written;		/* # of buffers written */

	int			ckpt_segs_added;	/* # of new xlog segments created */
	int			ckpt_segs_removed;		/* # of xlog segments deleted */
	int			ckpt_segs_recycled;		/* # of xlog segments recycled */
} CheckpointStatsData;

extern CheckpointStatsData CheckpointStats;


extern XLogRecPtr XLogInsert(RmgrId rmid, uint8 info, XLogRecData *rdata);
extern void XLogFlush(XLogRecPtr RecPtr);
extern void XLogBackgroundFlush(void);
extern void XLogAsyncCommitFlush(void);
extern bool XLogNeedsFlush(XLogRecPtr RecPtr);

extern void XLogSetAsyncCommitLSN(XLogRecPtr record);

extern void xlog_redo(XLogRecPtr lsn, XLogRecord *record);
extern void xlog_desc(StringInfo buf, uint8 xl_info, char *rec);

extern void UpdateControlFile(void);
extern Size XLOGShmemSize(void);
extern void XLOGShmemInit(void);
extern void BootStrapXLOG(void);
extern void StartupXLOG(void);
extern void ShutdownXLOG(int code, Datum arg);
extern void InitXLOGAccess(void);
extern void CreateCheckPoint(int flags);
extern void XLogPutNextOid(Oid nextOid);
extern XLogRecPtr GetRedoRecPtr(void);
extern XLogRecPtr GetInsertRecPtr(void);
extern void GetNextXidAndEpoch(TransactionId *xid, uint32 *epoch);

#endif   /* XLOG_H */
