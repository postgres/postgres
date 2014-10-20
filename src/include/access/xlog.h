/*
 * xlog.h
 *
 * PostgreSQL transaction log manager
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xlog.h
 */
#ifndef XLOG_H
#define XLOG_H

#include "access/rmgr.h"
#include "access/xlogdefs.h"
#include "datatype/timestamp.h"
#include "lib/stringinfo.h"
#include "storage/buf.h"
#include "utils/pg_crc.h"

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
 * where there can be zero to four backup blocks (as signaled by xl_info flag
 * bits).  XLogRecord structs always start on MAXALIGN boundaries in the WAL
 * files, and we round up SizeOfXLogRecord so that the rmgr data is also
 * guaranteed to begin on a MAXALIGN boundary.  However, no padding is added
 * to align BkpBlock structs or backup block data.
 *
 * NOTE: xl_len counts only the rmgr data, not the XLogRecord header,
 * and also not any backup blocks.  xl_tot_len counts everything.  Neither
 * length field is rounded up to an alignment boundary.
 */
typedef struct XLogRecord
{
	uint32		xl_tot_len;		/* total len of entire record */
	TransactionId xl_xid;		/* xact id */
	uint32		xl_len;			/* total len of rmgr data */
	uint8		xl_info;		/* flag bits, see below */
	RmgrId		xl_rmid;		/* resource manager for this record */
	/* 2 bytes of padding here, initialize to zero */
	XLogRecPtr	xl_prev;		/* ptr to previous record in log */
	pg_crc32	xl_crc;			/* CRC for this record */

	/* If MAXALIGN==8, there are 4 wasted bytes here */

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
 * xl_info to signal it.  We support backup of up to 4 disk blocks per XLOG
 * record.
 */
#define XLR_BKP_BLOCK_MASK		0x0F	/* all info bits used for bkp blocks */
#define XLR_MAX_BKP_BLOCKS		4
#define XLR_BKP_BLOCK(iblk)		(0x08 >> (iblk))		/* iblk in 0..3 */

/* Sync methods */
#define SYNC_METHOD_FSYNC		0
#define SYNC_METHOD_FDATASYNC	1
#define SYNC_METHOD_OPEN		2		/* for O_SYNC */
#define SYNC_METHOD_FSYNC_WRITETHROUGH	3
#define SYNC_METHOD_OPEN_DSYNC	4		/* for O_DSYNC */
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
 * sets XLR_BKP_BLOCK(N) bit in xl_info.  Note that the buffer must be pinned
 * and exclusive-locked by the caller, so that it won't change under us.
 * NB: when the buffer is backed up, we DO NOT insert the data pointed to by
 * this XLogRecData struct into the XLOG record, since we assume it's present
 * in the buffer.  Therefore, rmgr redo routines MUST pay attention to
 * XLR_BKP_BLOCK(N) to know what is actually stored in the XLOG record.
 * The N'th XLR_BKP_BLOCK bit corresponds to the N'th distinct buffer
 * value (ignoring InvalidBuffer) appearing in the rdata chain.
 *
 * When buffer is valid, caller must set buffer_std to indicate whether the
 * page uses standard pd_lower/pd_upper header fields.  If this is true, then
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

extern PGDLLIMPORT TimeLineID ThisTimeLineID;	/* current TLI */

/*
 * Prior to 8.4, all activity during recovery was carried out by the startup
 * process. This local variable continues to be used in many parts of the
 * code to indicate actions taken by RecoveryManagers. Other processes that
 * potentially perform work during recovery should check RecoveryInProgress().
 * See XLogCtl notes in xlog.c.
 */
extern bool InRecovery;

/*
 * Like InRecovery, standbyState is only valid in the startup process.
 * In all other processes it will have the value STANDBY_DISABLED (so
 * InHotStandby will read as FALSE).
 *
 * In DISABLED state, we're performing crash recovery or hot standby was
 * disabled in postgresql.conf.
 *
 * In INITIALIZED state, we've run InitRecoveryTransactionEnvironment, but
 * we haven't yet processed a RUNNING_XACTS or shutdown-checkpoint WAL record
 * to initialize our master-transaction tracking system.
 *
 * When the transaction tracking is initialized, we enter the SNAPSHOT_PENDING
 * state. The tracked information might still be incomplete, so we can't allow
 * connections yet, but redo functions must update the in-memory state when
 * appropriate.
 *
 * In SNAPSHOT_READY mode, we have full knowledge of transactions that are
 * (or were) running in the master at the current WAL location. Snapshots
 * can be taken, and read-only queries can be run.
 */
typedef enum
{
	STANDBY_DISABLED,
	STANDBY_INITIALIZED,
	STANDBY_SNAPSHOT_PENDING,
	STANDBY_SNAPSHOT_READY
} HotStandbyState;

extern HotStandbyState standbyState;

#define InHotStandby (standbyState >= STANDBY_SNAPSHOT_PENDING)

/*
 * Recovery target type.
 * Only set during a Point in Time recovery, not when standby_mode = on
 */
typedef enum
{
	RECOVERY_TARGET_UNSET,
	RECOVERY_TARGET_XID,
	RECOVERY_TARGET_TIME,
	RECOVERY_TARGET_NAME,
	RECOVERY_TARGET_IMMEDIATE
} RecoveryTargetType;

extern XLogRecPtr XactLastRecEnd;

extern bool reachedConsistency;

/* these variables are GUC parameters related to XLOG */
extern int	CheckPointSegments;
extern int	wal_keep_segments;
extern int	XLOGbuffers;
extern int	XLogArchiveTimeout;
extern bool XLogArchiveMode;
extern char *XLogArchiveCommand;
extern bool EnableHotStandby;
extern bool fullPageWrites;
extern bool wal_log_hints;
extern bool log_checkpoints;

/* WAL levels */
typedef enum WalLevel
{
	WAL_LEVEL_MINIMAL = 0,
	WAL_LEVEL_ARCHIVE,
	WAL_LEVEL_HOT_STANDBY,
	WAL_LEVEL_LOGICAL
} WalLevel;
extern int	wal_level;

#define XLogArchivingActive()	(XLogArchiveMode && wal_level >= WAL_LEVEL_ARCHIVE)
#define XLogArchiveCommandSet() (XLogArchiveCommand[0] != '\0')

/*
 * Is WAL-logging necessary for archival or log-shipping, or can we skip
 * WAL-logging if we fsync() the data before committing instead?
 */
#define XLogIsNeeded() (wal_level >= WAL_LEVEL_ARCHIVE)

/*
 * Is a full-page image needed for hint bit updates?
 *
 * Normally, we don't WAL-log hint bit updates, but if checksums are enabled,
 * we have to protect them against torn page writes.  When you only set
 * individual bits on a page, it's still consistent no matter what combination
 * of the bits make it to disk, but the checksum wouldn't match.  Also WAL-log
 * them if forced by wal_log_hints=on.
 */
#define XLogHintBitIsNeeded() (DataChecksumsEnabled() || wal_log_hints)

/* Do we need to WAL-log information required only for Hot Standby and logical replication? */
#define XLogStandbyInfoActive() (wal_level >= WAL_LEVEL_HOT_STANDBY)

/* Do we need to WAL-log information required only for logical replication? */
#define XLogLogicalInfoActive() (wal_level >= WAL_LEVEL_LOGICAL)

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
#define CHECKPOINT_END_OF_RECOVERY	0x0002		/* Like shutdown checkpoint,
												 * but issued at end of WAL
												 * recovery */
#define CHECKPOINT_IMMEDIATE	0x0004	/* Do it without delays */
#define CHECKPOINT_FORCE		0x0008	/* Force even if no activity */
/* These are important to RequestCheckpoint */
#define CHECKPOINT_WAIT			0x0010	/* Wait for completion */
/* These indicate the cause of a checkpoint request */
#define CHECKPOINT_CAUSE_XLOG	0x0020	/* XLOG consumption */
#define CHECKPOINT_CAUSE_TIME	0x0040	/* Elapsed time */
#define CHECKPOINT_FLUSH_ALL	0x0080	/* Flush all pages, including those
										 * belonging to unlogged tables */

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

	int			ckpt_sync_rels; /* # of relations synced */
	uint64		ckpt_longest_sync;		/* Longest sync for one relation */
	uint64		ckpt_agg_sync_time;		/* The sum of all the individual sync
										 * times, which is not necessarily the
										 * same as the total elapsed time for
										 * the entire sync phase. */
} CheckpointStatsData;

extern CheckpointStatsData CheckpointStats;

extern XLogRecPtr XLogInsert(RmgrId rmid, uint8 info, XLogRecData *rdata);
extern bool XLogCheckBufferNeedsBackup(Buffer buffer);
extern void XLogFlush(XLogRecPtr RecPtr);
extern bool XLogBackgroundFlush(void);
extern bool XLogNeedsFlush(XLogRecPtr RecPtr);
extern int	XLogFileInit(XLogSegNo segno, bool *use_existent, bool use_lock);
extern int	XLogFileOpen(XLogSegNo segno);

extern XLogRecPtr XLogSaveBufferForHint(Buffer buffer, bool buffer_std);

extern void CheckXLogRemoved(XLogSegNo segno, TimeLineID tli);
extern XLogSegNo XLogGetLastRemovedSegno(void);
extern void XLogSetAsyncXactLSN(XLogRecPtr record);
extern void XLogSetReplicationSlotMinimumLSN(XLogRecPtr lsn);

extern Buffer RestoreBackupBlock(XLogRecPtr lsn, XLogRecord *record,
				   int block_index,
				   bool get_cleanup_lock, bool keep_buffer);

extern void xlog_redo(XLogRecPtr lsn, XLogRecord *record);
extern void xlog_desc(StringInfo buf, uint8 xl_info, char *rec);

extern void issue_xlog_fsync(int fd, XLogSegNo segno);

extern bool RecoveryInProgress(void);
extern bool HotStandbyActive(void);
extern bool HotStandbyActiveInReplay(void);
extern bool XLogInsertAllowed(void);
extern void GetXLogReceiptTime(TimestampTz *rtime, bool *fromStream);
extern XLogRecPtr GetXLogReplayRecPtr(TimeLineID *replayTLI);
extern XLogRecPtr GetXLogInsertRecPtr(void);
extern XLogRecPtr GetXLogWriteRecPtr(void);
extern bool RecoveryIsPaused(void);
extern void SetRecoveryPause(bool recoveryPause);
extern TimestampTz GetLatestXTime(void);
extern TimestampTz GetCurrentChunkReplayStartTime(void);
extern char *XLogFileNameP(TimeLineID tli, XLogSegNo segno);

extern void UpdateControlFile(void);
extern uint64 GetSystemIdentifier(void);
extern bool DataChecksumsEnabled(void);
extern XLogRecPtr GetFakeLSNForUnloggedRel(void);
extern Size XLOGShmemSize(void);
extern void XLOGShmemInit(void);
extern void BootStrapXLOG(void);
extern void StartupXLOG(void);
extern void ShutdownXLOG(int code, Datum arg);
extern void InitXLOGAccess(void);
extern void CreateCheckPoint(int flags);
extern bool CreateRestartPoint(int flags);
extern void XLogPutNextOid(Oid nextOid);
extern XLogRecPtr XLogRestorePoint(const char *rpName);
extern void UpdateFullPageWrites(void);
extern XLogRecPtr GetRedoRecPtr(void);
extern XLogRecPtr GetInsertRecPtr(void);
extern XLogRecPtr GetFlushRecPtr(void);
extern void GetNextXidAndEpoch(TransactionId *xid, uint32 *epoch);

extern bool CheckPromoteSignal(void);
extern void WakeupRecovery(void);
extern void SetWalWriterSleeping(bool sleeping);

/*
 * Starting/stopping a base backup
 */
extern XLogRecPtr do_pg_start_backup(const char *backupidstr, bool fast,
				   TimeLineID *starttli_p, char **labelfile);
extern XLogRecPtr do_pg_stop_backup(char *labelfile, bool waitforarchive,
				  TimeLineID *stoptli_p);
extern void do_pg_abort_backup(void);

/* File path names (all relative to $PGDATA) */
#define BACKUP_LABEL_FILE		"backup_label"
#define BACKUP_LABEL_OLD		"backup_label.old"

#endif   /* XLOG_H */
