/*-------------------------------------------------------------------------
 *
 * xlog.c
 *		PostgreSQL write-ahead log manager
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/xlog.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>
#include <math.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "access/clog.h"
#include "access/commit_ts.h"
#include "access/heaptoast.h"
#include "access/multixact.h"
#include "access/rewriteheap.h"
#include "access/subtrans.h"
#include "access/timeline.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "access/xlogarchive.h"
#include "access/xloginsert.h"
#include "access/xlogreader.h"
#include "access/xlogutils.h"
#include "catalog/catversion.h"
#include "catalog/pg_control.h"
#include "catalog/pg_database.h"
#include "commands/progress.h"
#include "commands/tablespace.h"
#include "common/controldata_utils.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "pgstat.h"
#include "port/atomics.h"
#include "postmaster/bgwriter.h"
#include "postmaster/startup.h"
#include "postmaster/walwriter.h"
#include "replication/basebackup.h"
#include "replication/logical.h"
#include "replication/origin.h"
#include "replication/slot.h"
#include "replication/snapbuild.h"
#include "replication/walreceiver.h"
#include "replication/walsender.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/large_object.h"
#include "storage/latch.h"
#include "storage/pmsignal.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/reinit.h"
#include "storage/smgr.h"
#include "storage/spin.h"
#include "storage/sync.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/relmapper.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

extern uint32 bootstrap_data_checksum_version;

/* Unsupported old recovery command file names (relative to $PGDATA) */
#define RECOVERY_COMMAND_FILE	"recovery.conf"
#define RECOVERY_COMMAND_DONE	"recovery.done"

/* User-settable parameters */
int			max_wal_size_mb = 1024; /* 1 GB */
int			min_wal_size_mb = 80;	/* 80 MB */
int			wal_keep_size_mb = 0;
int			XLOGbuffers = -1;
int			XLogArchiveTimeout = 0;
int			XLogArchiveMode = ARCHIVE_MODE_OFF;
char	   *XLogArchiveCommand = NULL;
bool		EnableHotStandby = false;
bool		fullPageWrites = true;
bool		wal_log_hints = false;
bool		wal_compression = false;
char	   *wal_consistency_checking_string = NULL;
bool	   *wal_consistency_checking = NULL;
bool		wal_init_zero = true;
bool		wal_recycle = true;
bool		log_checkpoints = false;
int			sync_method = DEFAULT_SYNC_METHOD;
int			wal_level = WAL_LEVEL_MINIMAL;
int			CommitDelay = 0;	/* precommit delay in microseconds */
int			CommitSiblings = 5; /* # concurrent xacts needed to sleep */
int			wal_retrieve_retry_interval = 5000;
int			max_slot_wal_keep_size_mb = -1;

#ifdef WAL_DEBUG
bool		XLOG_DEBUG = false;
#endif

int			wal_segment_size = DEFAULT_XLOG_SEG_SIZE;

/*
 * Number of WAL insertion locks to use. A higher value allows more insertions
 * to happen concurrently, but adds some CPU overhead to flushing the WAL,
 * which needs to iterate all the locks.
 */
#define NUM_XLOGINSERT_LOCKS  8

/*
 * Max distance from last checkpoint, before triggering a new xlog-based
 * checkpoint.
 */
int			CheckPointSegments;

/* Estimated distance between checkpoints, in bytes */
static double CheckPointDistanceEstimate = 0;
static double PrevCheckPointDistance = 0;

/*
 * GUC support
 */
const struct config_enum_entry sync_method_options[] = {
	{"fsync", SYNC_METHOD_FSYNC, false},
#ifdef HAVE_FSYNC_WRITETHROUGH
	{"fsync_writethrough", SYNC_METHOD_FSYNC_WRITETHROUGH, false},
#endif
#ifdef HAVE_FDATASYNC
	{"fdatasync", SYNC_METHOD_FDATASYNC, false},
#endif
#ifdef OPEN_SYNC_FLAG
	{"open_sync", SYNC_METHOD_OPEN, false},
#endif
#ifdef OPEN_DATASYNC_FLAG
	{"open_datasync", SYNC_METHOD_OPEN_DSYNC, false},
#endif
	{NULL, 0, false}
};


/*
 * Although only "on", "off", and "always" are documented,
 * we accept all the likely variants of "on" and "off".
 */
const struct config_enum_entry archive_mode_options[] = {
	{"always", ARCHIVE_MODE_ALWAYS, false},
	{"on", ARCHIVE_MODE_ON, false},
	{"off", ARCHIVE_MODE_OFF, false},
	{"true", ARCHIVE_MODE_ON, true},
	{"false", ARCHIVE_MODE_OFF, true},
	{"yes", ARCHIVE_MODE_ON, true},
	{"no", ARCHIVE_MODE_OFF, true},
	{"1", ARCHIVE_MODE_ON, true},
	{"0", ARCHIVE_MODE_OFF, true},
	{NULL, 0, false}
};

const struct config_enum_entry recovery_target_action_options[] = {
	{"pause", RECOVERY_TARGET_ACTION_PAUSE, false},
	{"promote", RECOVERY_TARGET_ACTION_PROMOTE, false},
	{"shutdown", RECOVERY_TARGET_ACTION_SHUTDOWN, false},
	{NULL, 0, false}
};

/*
 * Statistics for current checkpoint are collected in this global struct.
 * Because only the checkpointer or a stand-alone backend can perform
 * checkpoints, this will be unused in normal backends.
 */
CheckpointStatsData CheckpointStats;

/*
 * ThisTimeLineID will be same in all backends --- it identifies current
 * WAL timeline for the database system.
 */
TimeLineID	ThisTimeLineID = 0;

/*
 * Are we doing recovery from XLOG?
 *
 * This is only ever true in the startup process; it should be read as meaning
 * "this process is replaying WAL records", rather than "the system is in
 * recovery mode".  It should be examined primarily by functions that need
 * to act differently when called from a WAL redo function (e.g., to skip WAL
 * logging).  To check whether the system is in recovery regardless of which
 * process you're running in, use RecoveryInProgress() but only after shared
 * memory startup and lock initialization.
 */
bool		InRecovery = false;

/* Are we in Hot Standby mode? Only valid in startup process, see xlog.h */
HotStandbyState standbyState = STANDBY_DISABLED;

static XLogRecPtr LastRec;

/* Local copy of WalRcv->flushedUpto */
static XLogRecPtr flushedUpto = 0;
static TimeLineID receiveTLI = 0;

/*
 * abortedRecPtr is the start pointer of a broken record at end of WAL when
 * recovery completes; missingContrecPtr is the location of the first
 * contrecord that went missing.  See CreateOverwriteContrecordRecord for
 * details.
 */
static XLogRecPtr abortedRecPtr;
static XLogRecPtr missingContrecPtr;

/*
 * During recovery, lastFullPageWrites keeps track of full_page_writes that
 * the replayed WAL records indicate. It's initialized with full_page_writes
 * that the recovery starting checkpoint record indicates, and then updated
 * each time XLOG_FPW_CHANGE record is replayed.
 */
static bool lastFullPageWrites;

/*
 * Local copy of the state tracked by SharedRecoveryState in shared memory,
 * It is false if SharedRecoveryState is RECOVERY_STATE_DONE.  True actually
 * means "not known, need to check the shared state".
 */
static bool LocalRecoveryInProgress = true;

/*
 * Local copy of SharedHotStandbyActive variable. False actually means "not
 * known, need to check the shared state".
 */
static bool LocalHotStandbyActive = false;

/*
 * Local copy of SharedPromoteIsTriggered variable. False actually means "not
 * known, need to check the shared state".
 */
static bool LocalPromoteIsTriggered = false;

/*
 * Local state for XLogInsertAllowed():
 *		1: unconditionally allowed to insert XLOG
 *		0: unconditionally not allowed to insert XLOG
 *		-1: must check RecoveryInProgress(); disallow until it is false
 * Most processes start with -1 and transition to 1 after seeing that recovery
 * is not in progress.  But we can also force the value for special cases.
 * The coding in XLogInsertAllowed() depends on the first two of these states
 * being numerically the same as bool true and false.
 */
static int	LocalXLogInsertAllowed = -1;

/*
 * When ArchiveRecoveryRequested is set, archive recovery was requested,
 * ie. signal files were present. When InArchiveRecovery is set, we are
 * currently recovering using offline XLOG archives. These variables are only
 * valid in the startup process.
 *
 * When ArchiveRecoveryRequested is true, but InArchiveRecovery is false, we're
 * currently performing crash recovery using only XLOG files in pg_wal, but
 * will switch to using offline XLOG archives as soon as we reach the end of
 * WAL in pg_wal.
*/
bool		ArchiveRecoveryRequested = false;
bool		InArchiveRecovery = false;

static bool standby_signal_file_found = false;
static bool recovery_signal_file_found = false;

/* Was the last xlog file restored from archive, or local? */
static bool restoredFromArchive = false;

/* Buffers dedicated to consistency checks of size BLCKSZ */
static char *replay_image_masked = NULL;
static char *master_image_masked = NULL;

/* options formerly taken from recovery.conf for archive recovery */
char	   *recoveryRestoreCommand = NULL;
char	   *recoveryEndCommand = NULL;
char	   *archiveCleanupCommand = NULL;
RecoveryTargetType recoveryTarget = RECOVERY_TARGET_UNSET;
bool		recoveryTargetInclusive = true;
int			recoveryTargetAction = RECOVERY_TARGET_ACTION_PAUSE;
TransactionId recoveryTargetXid;
char	   *recovery_target_time_string;
static TimestampTz recoveryTargetTime;
const char *recoveryTargetName;
XLogRecPtr	recoveryTargetLSN;
int			recovery_min_apply_delay = 0;

/* options formerly taken from recovery.conf for XLOG streaming */
bool		StandbyModeRequested = false;
char	   *PrimaryConnInfo = NULL;
char	   *PrimarySlotName = NULL;
char	   *PromoteTriggerFile = NULL;
bool		wal_receiver_create_temp_slot = false;

/* are we currently in standby mode? */
bool		StandbyMode = false;

/* whether request for fast promotion has been made yet */
static bool fast_promote = false;

/*
 * if recoveryStopsBefore/After returns true, it saves information of the stop
 * point here
 */
static TransactionId recoveryStopXid;
static TimestampTz recoveryStopTime;
static XLogRecPtr recoveryStopLSN;
static char recoveryStopName[MAXFNAMELEN];
static bool recoveryStopAfter;

/*
 * During normal operation, the only timeline we care about is ThisTimeLineID.
 * During recovery, however, things are more complicated.  To simplify life
 * for rmgr code, we keep ThisTimeLineID set to the "current" timeline as we
 * scan through the WAL history (that is, it is the line that was active when
 * the currently-scanned WAL record was generated).  We also need these
 * timeline values:
 *
 * recoveryTargetTimeLineGoal: what the user requested, if any
 *
 * recoveryTargetTLIRequested: numeric value of requested timeline, if constant
 *
 * recoveryTargetTLI: the currently understood target timeline; changes
 *
 * expectedTLEs: a list of TimeLineHistoryEntries for recoveryTargetTLI and the timelines of
 * its known parents, newest first (so recoveryTargetTLI is always the
 * first list member).  Only these TLIs are expected to be seen in the WAL
 * segments we read, and indeed only these TLIs will be considered as
 * candidate WAL files to open at all.
 *
 * curFileTLI: the TLI appearing in the name of the current input WAL file.
 * (This is not necessarily the same as ThisTimeLineID, because we could
 * be scanning data that was copied from an ancestor timeline when the current
 * file was created.)  During a sequential scan we do not allow this value
 * to decrease.
 */
RecoveryTargetTimeLineGoal recoveryTargetTimeLineGoal = RECOVERY_TARGET_TIMELINE_LATEST;
TimeLineID	recoveryTargetTLIRequested = 0;
TimeLineID	recoveryTargetTLI = 0;
static List *expectedTLEs;
static TimeLineID curFileTLI;

/*
 * ProcLastRecPtr points to the start of the last XLOG record inserted by the
 * current backend.  It is updated for all inserts.  XactLastRecEnd points to
 * end+1 of the last record, and is reset when we end a top-level transaction,
 * or start a new one; so it can be used to tell if the current transaction has
 * created any XLOG records.
 *
 * While in parallel mode, this may not be fully up to date.  When committing,
 * a transaction can assume this covers all xlog records written either by the
 * user backend or by any parallel worker which was present at any point during
 * the transaction.  But when aborting, or when still in parallel mode, other
 * parallel backends may have written WAL records at later LSNs than the value
 * stored here.  The parallel leader advances its own copy, when necessary,
 * in WaitForParallelWorkersToFinish.
 */
XLogRecPtr	ProcLastRecPtr = InvalidXLogRecPtr;
XLogRecPtr	XactLastRecEnd = InvalidXLogRecPtr;
XLogRecPtr	XactLastCommitEnd = InvalidXLogRecPtr;

/*
 * RedoRecPtr is this backend's local copy of the REDO record pointer
 * (which is almost but not quite the same as a pointer to the most recent
 * CHECKPOINT record).  We update this from the shared-memory copy,
 * XLogCtl->Insert.RedoRecPtr, whenever we can safely do so (ie, when we
 * hold an insertion lock).  See XLogInsertRecord for details.  We are also
 * allowed to update from XLogCtl->RedoRecPtr if we hold the info_lck;
 * see GetRedoRecPtr.  A freshly spawned backend obtains the value during
 * InitXLOGAccess.
 */
static XLogRecPtr RedoRecPtr;

/*
 * doPageWrites is this backend's local copy of (forcePageWrites ||
 * fullPageWrites).  It is used together with RedoRecPtr to decide whether
 * a full-page image of a page need to be taken.
 */
static bool doPageWrites;

/* Has the recovery code requested a walreceiver wakeup? */
static bool doRequestWalReceiverReply;

/*
 * RedoStartLSN points to the checkpoint's REDO location which is specified
 * in a backup label file, backup history file or control file. In standby
 * mode, XLOG streaming usually starts from the position where an invalid
 * record was found. But if we fail to read even the initial checkpoint
 * record, we use the REDO location instead of the checkpoint location as
 * the start position of XLOG streaming. Otherwise we would have to jump
 * backwards to the REDO location after reading the checkpoint record,
 * because the REDO record can precede the checkpoint record.
 */
static XLogRecPtr RedoStartLSN = InvalidXLogRecPtr;

/*----------
 * Shared-memory data structures for XLOG control
 *
 * LogwrtRqst indicates a byte position that we need to write and/or fsync
 * the log up to (all records before that point must be written or fsynced).
 * LogwrtResult indicates the byte positions we have already written/fsynced.
 * These structs are identical but are declared separately to indicate their
 * slightly different functions.
 *
 * To read XLogCtl->LogwrtResult, you must hold either info_lck or
 * WALWriteLock.  To update it, you need to hold both locks.  The point of
 * this arrangement is that the value can be examined by code that already
 * holds WALWriteLock without needing to grab info_lck as well.  In addition
 * to the shared variable, each backend has a private copy of LogwrtResult,
 * which is updated when convenient.
 *
 * The request bookkeeping is simpler: there is a shared XLogCtl->LogwrtRqst
 * (protected by info_lck), but we don't need to cache any copies of it.
 *
 * info_lck is only held long enough to read/update the protected variables,
 * so it's a plain spinlock.  The other locks are held longer (potentially
 * over I/O operations), so we use LWLocks for them.  These locks are:
 *
 * WALBufMappingLock: must be held to replace a page in the WAL buffer cache.
 * It is only held while initializing and changing the mapping.  If the
 * contents of the buffer being replaced haven't been written yet, the mapping
 * lock is released while the write is done, and reacquired afterwards.
 *
 * WALWriteLock: must be held to write WAL buffers to disk (XLogWrite or
 * XLogFlush).
 *
 * ControlFileLock: must be held to read/update control file or create
 * new log file.
 *
 * CheckpointLock: must be held to do a checkpoint or restartpoint (ensures
 * only one checkpointer at a time; currently, with all checkpoints done by
 * the checkpointer, this is just pro forma).
 *
 *----------
 */

typedef struct XLogwrtRqst
{
	XLogRecPtr	Write;			/* last byte + 1 to write out */
	XLogRecPtr	Flush;			/* last byte + 1 to flush */
} XLogwrtRqst;

typedef struct XLogwrtResult
{
	XLogRecPtr	Write;			/* last byte + 1 written out */
	XLogRecPtr	Flush;			/* last byte + 1 flushed */
} XLogwrtResult;

/*
 * Inserting to WAL is protected by a small fixed number of WAL insertion
 * locks. To insert to the WAL, you must hold one of the locks - it doesn't
 * matter which one. To lock out other concurrent insertions, you must hold
 * of them. Each WAL insertion lock consists of a lightweight lock, plus an
 * indicator of how far the insertion has progressed (insertingAt).
 *
 * The insertingAt values are read when a process wants to flush WAL from
 * the in-memory buffers to disk, to check that all the insertions to the
 * region the process is about to write out have finished. You could simply
 * wait for all currently in-progress insertions to finish, but the
 * insertingAt indicator allows you to ignore insertions to later in the WAL,
 * so that you only wait for the insertions that are modifying the buffers
 * you're about to write out.
 *
 * This isn't just an optimization. If all the WAL buffers are dirty, an
 * inserter that's holding a WAL insert lock might need to evict an old WAL
 * buffer, which requires flushing the WAL. If it's possible for an inserter
 * to block on another inserter unnecessarily, deadlock can arise when two
 * inserters holding a WAL insert lock wait for each other to finish their
 * insertion.
 *
 * Small WAL records that don't cross a page boundary never update the value,
 * the WAL record is just copied to the page and the lock is released. But
 * to avoid the deadlock-scenario explained above, the indicator is always
 * updated before sleeping while holding an insertion lock.
 *
 * lastImportantAt contains the LSN of the last important WAL record inserted
 * using a given lock. This value is used to detect if there has been
 * important WAL activity since the last time some action, like a checkpoint,
 * was performed - allowing to not repeat the action if not. The LSN is
 * updated for all insertions, unless the XLOG_MARK_UNIMPORTANT flag was
 * set. lastImportantAt is never cleared, only overwritten by the LSN of newer
 * records.  Tracking the WAL activity directly in WALInsertLock has the
 * advantage of not needing any additional locks to update the value.
 */
typedef struct
{
	LWLock		lock;
	XLogRecPtr	insertingAt;
	XLogRecPtr	lastImportantAt;
} WALInsertLock;

/*
 * All the WAL insertion locks are allocated as an array in shared memory. We
 * force the array stride to be a power of 2, which saves a few cycles in
 * indexing, but more importantly also ensures that individual slots don't
 * cross cache line boundaries. (Of course, we have to also ensure that the
 * array start address is suitably aligned.)
 */
typedef union WALInsertLockPadded
{
	WALInsertLock l;
	char		pad[PG_CACHE_LINE_SIZE];
} WALInsertLockPadded;

/*
 * State of an exclusive backup, necessary to control concurrent activities
 * across sessions when working on exclusive backups.
 *
 * EXCLUSIVE_BACKUP_NONE means that there is no exclusive backup actually
 * running, to be more precise pg_start_backup() is not being executed for
 * an exclusive backup and there is no exclusive backup in progress.
 * EXCLUSIVE_BACKUP_STARTING means that pg_start_backup() is starting an
 * exclusive backup.
 * EXCLUSIVE_BACKUP_IN_PROGRESS means that pg_start_backup() has finished
 * running and an exclusive backup is in progress. pg_stop_backup() is
 * needed to finish it.
 * EXCLUSIVE_BACKUP_STOPPING means that pg_stop_backup() is stopping an
 * exclusive backup.
 */
typedef enum ExclusiveBackupState
{
	EXCLUSIVE_BACKUP_NONE = 0,
	EXCLUSIVE_BACKUP_STARTING,
	EXCLUSIVE_BACKUP_IN_PROGRESS,
	EXCLUSIVE_BACKUP_STOPPING
} ExclusiveBackupState;

/*
 * Session status of running backup, used for sanity checks in SQL-callable
 * functions to start and stop backups.
 */
static SessionBackupState sessionBackupState = SESSION_BACKUP_NONE;

/*
 * Shared state data for WAL insertion.
 */
typedef struct XLogCtlInsert
{
	slock_t		insertpos_lck;	/* protects CurrBytePos and PrevBytePos */

	/*
	 * CurrBytePos is the end of reserved WAL. The next record will be
	 * inserted at that position. PrevBytePos is the start position of the
	 * previously inserted (or rather, reserved) record - it is copied to the
	 * prev-link of the next record. These are stored as "usable byte
	 * positions" rather than XLogRecPtrs (see XLogBytePosToRecPtr()).
	 */
	uint64		CurrBytePos;
	uint64		PrevBytePos;

	/*
	 * Make sure the above heavily-contended spinlock and byte positions are
	 * on their own cache line. In particular, the RedoRecPtr and full page
	 * write variables below should be on a different cache line. They are
	 * read on every WAL insertion, but updated rarely, and we don't want
	 * those reads to steal the cache line containing Curr/PrevBytePos.
	 */
	char		pad[PG_CACHE_LINE_SIZE];

	/*
	 * fullPageWrites is the master copy used by all backends to determine
	 * whether to write full-page to WAL, instead of using process-local one.
	 * This is required because, when full_page_writes is changed by SIGHUP,
	 * we must WAL-log it before it actually affects WAL-logging by backends.
	 * Checkpointer sets at startup or after SIGHUP.
	 *
	 * To read these fields, you must hold an insertion lock. To modify them,
	 * you must hold ALL the locks.
	 */
	XLogRecPtr	RedoRecPtr;		/* current redo point for insertions */
	bool		forcePageWrites;	/* forcing full-page writes for PITR? */
	bool		fullPageWrites;

	/*
	 * exclusiveBackupState indicates the state of an exclusive backup (see
	 * comments of ExclusiveBackupState for more details). nonExclusiveBackups
	 * is a counter indicating the number of streaming base backups currently
	 * in progress. forcePageWrites is set to true when either of these is
	 * non-zero. lastBackupStart is the latest checkpoint redo location used
	 * as a starting point for an online backup.
	 */
	ExclusiveBackupState exclusiveBackupState;
	int			nonExclusiveBackups;
	XLogRecPtr	lastBackupStart;

	/*
	 * WAL insertion locks.
	 */
	WALInsertLockPadded *WALInsertLocks;
} XLogCtlInsert;

/*
 * Total shared-memory state for XLOG.
 */
typedef struct XLogCtlData
{
	XLogCtlInsert Insert;

	/* Protected by info_lck: */
	XLogwrtRqst LogwrtRqst;
	XLogRecPtr	RedoRecPtr;		/* a recent copy of Insert->RedoRecPtr */
	FullTransactionId ckptFullXid;	/* nextFullXid of latest checkpoint */
	XLogRecPtr	asyncXactLSN;	/* LSN of newest async commit/abort */
	XLogRecPtr	replicationSlotMinLSN;	/* oldest LSN needed by any slot */

	XLogSegNo	lastRemovedSegNo;	/* latest removed/recycled XLOG segment */

	/* Fake LSN counter, for unlogged relations. Protected by ulsn_lck. */
	XLogRecPtr	unloggedLSN;
	slock_t		ulsn_lck;

	/* Time and LSN of last xlog segment switch. Protected by WALWriteLock. */
	pg_time_t	lastSegSwitchTime;
	XLogRecPtr	lastSegSwitchLSN;

	/*
	 * Protected by info_lck and WALWriteLock (you must hold either lock to
	 * read it, but both to update)
	 */
	XLogwrtResult LogwrtResult;

	/*
	 * Latest initialized page in the cache (last byte position + 1).
	 *
	 * To change the identity of a buffer (and InitializedUpTo), you need to
	 * hold WALBufMappingLock.  To change the identity of a buffer that's
	 * still dirty, the old page needs to be written out first, and for that
	 * you need WALWriteLock, and you need to ensure that there are no
	 * in-progress insertions to the page by calling
	 * WaitXLogInsertionsToFinish().
	 */
	XLogRecPtr	InitializedUpTo;

	/*
	 * These values do not change after startup, although the pointed-to pages
	 * and xlblocks values certainly do.  xlblocks values are protected by
	 * WALBufMappingLock.
	 */
	char	   *pages;			/* buffers for unwritten XLOG pages */
	XLogRecPtr *xlblocks;		/* 1st byte ptr-s + XLOG_BLCKSZ */
	int			XLogCacheBlck;	/* highest allocated xlog buffer index */

	/*
	 * Shared copy of ThisTimeLineID. Does not change after end-of-recovery.
	 * If we created a new timeline when the system was started up,
	 * PrevTimeLineID is the old timeline's ID that we forked off from.
	 * Otherwise it's equal to ThisTimeLineID.
	 */
	TimeLineID	ThisTimeLineID;
	TimeLineID	PrevTimeLineID;

	/*
	 * SharedRecoveryState indicates if we're still in crash or archive
	 * recovery.  Protected by info_lck.
	 */
	RecoveryState SharedRecoveryState;

	/*
	 * SharedHotStandbyActive indicates if we allow hot standby queries to be
	 * run.  Protected by info_lck.
	 */
	bool		SharedHotStandbyActive;

	/*
	 * SharedPromoteIsTriggered indicates if a standby promotion has been
	 * triggered.  Protected by info_lck.
	 */
	bool		SharedPromoteIsTriggered;

	/*
	 * WalWriterSleeping indicates whether the WAL writer is currently in
	 * low-power mode (and hence should be nudged if an async commit occurs).
	 * Protected by info_lck.
	 */
	bool		WalWriterSleeping;

	/*
	 * recoveryWakeupLatch is used to wake up the startup process to continue
	 * WAL replay, if it is waiting for WAL to arrive or failover trigger file
	 * to appear.
	 */
	Latch		recoveryWakeupLatch;

	/*
	 * During recovery, we keep a copy of the latest checkpoint record here.
	 * lastCheckPointRecPtr points to start of checkpoint record and
	 * lastCheckPointEndPtr points to end+1 of checkpoint record.  Used by the
	 * checkpointer when it wants to create a restartpoint.
	 *
	 * Protected by info_lck.
	 */
	XLogRecPtr	lastCheckPointRecPtr;
	XLogRecPtr	lastCheckPointEndPtr;
	CheckPoint	lastCheckPoint;

	/*
	 * lastReplayedEndRecPtr points to end+1 of the last record successfully
	 * replayed. When we're currently replaying a record, ie. in a redo
	 * function, replayEndRecPtr points to the end+1 of the record being
	 * replayed, otherwise it's equal to lastReplayedEndRecPtr.
	 */
	XLogRecPtr	lastReplayedEndRecPtr;
	TimeLineID	lastReplayedTLI;
	XLogRecPtr	replayEndRecPtr;
	TimeLineID	replayEndTLI;
	/* timestamp of last COMMIT/ABORT record replayed (or being replayed) */
	TimestampTz recoveryLastXTime;

	/*
	 * timestamp of when we started replaying the current chunk of WAL data,
	 * only relevant for replication or archive recovery
	 */
	TimestampTz currentChunkStartTime;
	/* Are we requested to pause recovery? */
	bool		recoveryPause;

	/*
	 * lastFpwDisableRecPtr points to the start of the last replayed
	 * XLOG_FPW_CHANGE record that instructs full_page_writes is disabled.
	 */
	XLogRecPtr	lastFpwDisableRecPtr;

	slock_t		info_lck;		/* locks shared variables shown above */
} XLogCtlData;

static XLogCtlData *XLogCtl = NULL;

/* a private copy of XLogCtl->Insert.WALInsertLocks, for convenience */
static WALInsertLockPadded *WALInsertLocks = NULL;

/*
 * We maintain an image of pg_control in shared memory.
 */
static ControlFileData *ControlFile = NULL;

/*
 * Calculate the amount of space left on the page after 'endptr'. Beware
 * multiple evaluation!
 */
#define INSERT_FREESPACE(endptr)	\
	(((endptr) % XLOG_BLCKSZ == 0) ? 0 : (XLOG_BLCKSZ - (endptr) % XLOG_BLCKSZ))

/* Macro to advance to next buffer index. */
#define NextBufIdx(idx)		\
		(((idx) == XLogCtl->XLogCacheBlck) ? 0 : ((idx) + 1))

/*
 * XLogRecPtrToBufIdx returns the index of the WAL buffer that holds, or
 * would hold if it was in cache, the page containing 'recptr'.
 */
#define XLogRecPtrToBufIdx(recptr)	\
	(((recptr) / XLOG_BLCKSZ) % (XLogCtl->XLogCacheBlck + 1))

/*
 * These are the number of bytes in a WAL page usable for WAL data.
 */
#define UsableBytesInPage (XLOG_BLCKSZ - SizeOfXLogShortPHD)

/*
 * Convert values of GUCs measured in megabytes to equiv. segment count.
 * Rounds down.
 */
#define ConvertToXSegs(x, segsize)	XLogMBVarToSegs((x), (segsize))

/* The number of bytes in a WAL segment usable for WAL data. */
static int	UsableBytesInSegment;

/*
 * Private, possibly out-of-date copy of shared LogwrtResult.
 * See discussion above.
 */
static XLogwrtResult LogwrtResult = {0, 0};

/*
 * Codes indicating where we got a WAL file from during recovery, or where
 * to attempt to get one.
 */
typedef enum
{
	XLOG_FROM_ANY = 0,			/* request to read WAL from any source */
	XLOG_FROM_ARCHIVE,			/* restored using restore_command */
	XLOG_FROM_PG_WAL,			/* existing file in pg_wal */
	XLOG_FROM_STREAM			/* streamed from master */
} XLogSource;

/* human-readable names for XLogSources, for debugging output */
static const char *const xlogSourceNames[] = {"any", "archive", "pg_wal", "stream"};

/*
 * openLogFile is -1 or a kernel FD for an open log file segment.
 * openLogSegNo identifies the segment.  These variables are only used to
 * write the XLOG, and so will normally refer to the active segment.
 * Note: call Reserve/ReleaseExternalFD to track consumption of this FD.
 */
static int	openLogFile = -1;
static XLogSegNo openLogSegNo = 0;

/*
 * These variables are used similarly to the ones above, but for reading
 * the XLOG.  readOff is the offset of the page just read, readLen
 * indicates how much of it has been read into readBuf, and readSource
 * indicates where we got the currently open file from.
 * Note: we could use Reserve/ReleaseExternalFD to track consumption of
 * this FD too; but it doesn't currently seem worthwhile, since the XLOG is
 * not read by general-purpose sessions.
 */
static int	readFile = -1;
static XLogSegNo readSegNo = 0;
static uint32 readOff = 0;
static uint32 readLen = 0;
static XLogSource readSource = XLOG_FROM_ANY;

/*
 * Keeps track of which source we're currently reading from. This is
 * different from readSource in that this is always set, even when we don't
 * currently have a WAL file open. If lastSourceFailed is set, our last
 * attempt to read from currentSource failed, and we should try another source
 * next.
 *
 * pendingWalRcvRestart is set when a config change occurs that requires a
 * walreceiver restart.  This is only valid in XLOG_FROM_STREAM state.
 */
static XLogSource currentSource = XLOG_FROM_ANY;
static bool lastSourceFailed = false;
static bool pendingWalRcvRestart = false;

typedef struct XLogPageReadPrivate
{
	int			emode;
	bool		fetching_ckpt;	/* are we fetching a checkpoint record? */
	bool		randAccess;
} XLogPageReadPrivate;

/*
 * These variables track when we last obtained some WAL data to process,
 * and where we got it from.  (XLogReceiptSource is initially the same as
 * readSource, but readSource gets reset to zero when we don't have data
 * to process right now.  It is also different from currentSource, which
 * also changes when we try to read from a source and fail, while
 * XLogReceiptSource tracks where we last successfully read some WAL.)
 */
static TimestampTz XLogReceiptTime = 0;
static XLogSource XLogReceiptSource = XLOG_FROM_ANY;

/* State information for XLOG reading */
static XLogRecPtr ReadRecPtr;	/* start of last record read */
static XLogRecPtr EndRecPtr;	/* end+1 of last record read */

/*
 * Local copies of equivalent fields in the control file.  When running
 * crash recovery, minRecoveryPoint is set to InvalidXLogRecPtr as we
 * expect to replay all the WAL available, and updateMinRecoveryPoint is
 * switched to false to prevent any updates while replaying records.
 * Those values are kept consistent as long as crash recovery runs.
 */
static XLogRecPtr minRecoveryPoint;
static TimeLineID minRecoveryPointTLI;
static bool updateMinRecoveryPoint = true;

/*
 * Have we reached a consistent database state? In crash recovery, we have
 * to replay all the WAL, so reachedConsistency is never set. During archive
 * recovery, the database is consistent once minRecoveryPoint is reached.
 */
bool		reachedConsistency = false;

static bool InRedo = false;

/* Have we launched bgwriter during recovery? */
static bool bgwriterLaunched = false;

/* For WALInsertLockAcquire/Release functions */
static int	MyLockNo = 0;
static bool holdingAllLocks = false;

#ifdef WAL_DEBUG
static MemoryContext walDebugCxt = NULL;
#endif

static void readRecoverySignalFile(void);
static void validateRecoveryParameters(void);
static void exitArchiveRecovery(TimeLineID endTLI, XLogRecPtr endOfLog);
static bool recoveryStopsBefore(XLogReaderState *record);
static bool recoveryStopsAfter(XLogReaderState *record);
static void recoveryPausesHere(bool endOfRecovery);
static bool recoveryApplyDelay(XLogReaderState *record);
static void SetLatestXTime(TimestampTz xtime);
static void SetCurrentChunkStartTime(TimestampTz xtime);
static void CheckRequiredParameterValues(void);
static void XLogReportParameters(void);
static void checkTimeLineSwitch(XLogRecPtr lsn, TimeLineID newTLI,
								TimeLineID prevTLI);
static void VerifyOverwriteContrecord(xl_overwrite_contrecord *xlrec,
									  XLogReaderState *state);
static void LocalSetXLogInsertAllowed(void);
static void CreateEndOfRecoveryRecord(void);
static XLogRecPtr CreateOverwriteContrecordRecord(XLogRecPtr aborted_lsn);
static void CheckPointGuts(XLogRecPtr checkPointRedo, int flags);
static void KeepLogSeg(XLogRecPtr recptr, XLogSegNo *logSegNo);
static XLogRecPtr XLogGetReplicationSlotMinimumLSN(void);

static void AdvanceXLInsertBuffer(XLogRecPtr upto, bool opportunistic);
static bool XLogCheckpointNeeded(XLogSegNo new_segno);
static void XLogWrite(XLogwrtRqst WriteRqst, bool flexible);
static bool InstallXLogFileSegment(XLogSegNo *segno, char *tmppath,
								   bool find_free, XLogSegNo max_segno,
								   bool use_lock);
static int	XLogFileRead(XLogSegNo segno, int emode, TimeLineID tli,
						 XLogSource source, bool notfoundOk);
static int	XLogFileReadAnyTLI(XLogSegNo segno, int emode, XLogSource source);
static int	XLogPageRead(XLogReaderState *xlogreader, XLogRecPtr targetPagePtr,
						 int reqLen, XLogRecPtr targetRecPtr, char *readBuf);
static bool WaitForWALToBecomeAvailable(XLogRecPtr RecPtr, bool randAccess,
										bool fetching_ckpt, XLogRecPtr tliRecPtr);
static int	emode_for_corrupt_record(int emode, XLogRecPtr RecPtr);
static void XLogFileClose(void);
static void PreallocXlogFiles(XLogRecPtr endptr);
static void RemoveTempXlogFiles(void);
static void RemoveOldXlogFiles(XLogSegNo segno, XLogRecPtr lastredoptr, XLogRecPtr endptr);
static void RemoveXlogFile(const char *segname, XLogRecPtr lastredoptr, XLogRecPtr endptr);
static void UpdateLastRemovedPtr(char *filename);
static void ValidateXLOGDirectoryStructure(void);
static void CleanupBackupHistory(void);
static void UpdateMinRecoveryPoint(XLogRecPtr lsn, bool force);
static XLogRecord *ReadRecord(XLogReaderState *xlogreader,
							  int emode, bool fetching_ckpt);
static void CheckRecoveryConsistency(void);
static XLogRecord *ReadCheckpointRecord(XLogReaderState *xlogreader,
										XLogRecPtr RecPtr, int whichChkpt, bool report);
static bool rescanLatestTimeLine(void);
static void InitControlFile(uint64 sysidentifier);
static void WriteControlFile(void);
static void ReadControlFile(void);
static char *str_time(pg_time_t tnow);
static void SetPromoteIsTriggered(void);
static bool CheckForStandbyTrigger(void);

#ifdef WAL_DEBUG
static void xlog_outrec(StringInfo buf, XLogReaderState *record);
#endif
static void xlog_outdesc(StringInfo buf, XLogReaderState *record);
static void pg_start_backup_callback(int code, Datum arg);
static void pg_stop_backup_callback(int code, Datum arg);
static bool read_backup_label(XLogRecPtr *checkPointLoc,
							  bool *backupEndRequired, bool *backupFromStandby);
static bool read_tablespace_map(List **tablespaces);

static void rm_redo_error_callback(void *arg);
static int	get_sync_bit(int method);

static void CopyXLogRecordToWAL(int write_len, bool isLogSwitch,
								XLogRecData *rdata,
								XLogRecPtr StartPos, XLogRecPtr EndPos);
static void ReserveXLogInsertLocation(int size, XLogRecPtr *StartPos,
									  XLogRecPtr *EndPos, XLogRecPtr *PrevPtr);
static bool ReserveXLogSwitch(XLogRecPtr *StartPos, XLogRecPtr *EndPos,
							  XLogRecPtr *PrevPtr);
static XLogRecPtr WaitXLogInsertionsToFinish(XLogRecPtr upto);
static char *GetXLogBuffer(XLogRecPtr ptr);
static XLogRecPtr XLogBytePosToRecPtr(uint64 bytepos);
static XLogRecPtr XLogBytePosToEndRecPtr(uint64 bytepos);
static uint64 XLogRecPtrToBytePos(XLogRecPtr ptr);
static void checkXLogConsistency(XLogReaderState *record);

static void WALInsertLockAcquire(void);
static void WALInsertLockAcquireExclusive(void);
static void WALInsertLockRelease(void);
static void WALInsertLockUpdateInsertingAt(XLogRecPtr insertingAt);

/*
 * Insert an XLOG record represented by an already-constructed chain of data
 * chunks.  This is a low-level routine; to construct the WAL record header
 * and data, use the higher-level routines in xloginsert.c.
 *
 * If 'fpw_lsn' is valid, it is the oldest LSN among the pages that this
 * WAL record applies to, that were not included in the record as full page
 * images.  If fpw_lsn <= RedoRecPtr, the function does not perform the
 * insertion and returns InvalidXLogRecPtr.  The caller can then recalculate
 * which pages need a full-page image, and retry.  If fpw_lsn is invalid, the
 * record is always inserted.
 *
 * 'flags' gives more in-depth control on the record being inserted. See
 * XLogSetRecordFlags() for details.
 *
 * The first XLogRecData in the chain must be for the record header, and its
 * data must be MAXALIGNed.  XLogInsertRecord fills in the xl_prev and
 * xl_crc fields in the header, the rest of the header must already be filled
 * by the caller.
 *
 * Returns XLOG pointer to end of record (beginning of next record).
 * This can be used as LSN for data pages affected by the logged action.
 * (LSN is the XLOG point up to which the XLOG must be flushed to disk
 * before the data page can be written out.  This implements the basic
 * WAL rule "write the log before the data".)
 */
XLogRecPtr
XLogInsertRecord(XLogRecData *rdata,
				 XLogRecPtr fpw_lsn,
				 uint8 flags,
				 int num_fpi)
{
	XLogCtlInsert *Insert = &XLogCtl->Insert;
	pg_crc32c	rdata_crc;
	bool		inserted;
	XLogRecord *rechdr = (XLogRecord *) rdata->data;
	uint8		info = rechdr->xl_info & ~XLR_INFO_MASK;
	bool		isLogSwitch = (rechdr->xl_rmid == RM_XLOG_ID &&
							   info == XLOG_SWITCH);
	XLogRecPtr	StartPos;
	XLogRecPtr	EndPos;
	bool		prevDoPageWrites = doPageWrites;

	/* we assume that all of the record header is in the first chunk */
	Assert(rdata->len >= SizeOfXLogRecord);

	/* cross-check on whether we should be here or not */
	if (!XLogInsertAllowed())
		elog(ERROR, "cannot make new WAL entries during recovery");

	/*----------
	 *
	 * We have now done all the preparatory work we can without holding a
	 * lock or modifying shared state. From here on, inserting the new WAL
	 * record to the shared WAL buffer cache is a two-step process:
	 *
	 * 1. Reserve the right amount of space from the WAL. The current head of
	 *	  reserved space is kept in Insert->CurrBytePos, and is protected by
	 *	  insertpos_lck.
	 *
	 * 2. Copy the record to the reserved WAL space. This involves finding the
	 *	  correct WAL buffer containing the reserved space, and copying the
	 *	  record in place. This can be done concurrently in multiple processes.
	 *
	 * To keep track of which insertions are still in-progress, each concurrent
	 * inserter acquires an insertion lock. In addition to just indicating that
	 * an insertion is in progress, the lock tells others how far the inserter
	 * has progressed. There is a small fixed number of insertion locks,
	 * determined by NUM_XLOGINSERT_LOCKS. When an inserter crosses a page
	 * boundary, it updates the value stored in the lock to the how far it has
	 * inserted, to allow the previous buffer to be flushed.
	 *
	 * Holding onto an insertion lock also protects RedoRecPtr and
	 * fullPageWrites from changing until the insertion is finished.
	 *
	 * Step 2 can usually be done completely in parallel. If the required WAL
	 * page is not initialized yet, you have to grab WALBufMappingLock to
	 * initialize it, but the WAL writer tries to do that ahead of insertions
	 * to avoid that from happening in the critical path.
	 *
	 *----------
	 */
	START_CRIT_SECTION();
	if (isLogSwitch)
		WALInsertLockAcquireExclusive();
	else
		WALInsertLockAcquire();

	/*
	 * Check to see if my copy of RedoRecPtr is out of date. If so, may have
	 * to go back and have the caller recompute everything. This can only
	 * happen just after a checkpoint, so it's better to be slow in this case
	 * and fast otherwise.
	 *
	 * Also check to see if fullPageWrites or forcePageWrites was just turned
	 * on; if we weren't already doing full-page writes then go back and
	 * recompute.
	 *
	 * If we aren't doing full-page writes then RedoRecPtr doesn't actually
	 * affect the contents of the XLOG record, so we'll update our local copy
	 * but not force a recomputation.  (If doPageWrites was just turned off,
	 * we could recompute the record without full pages, but we choose not to
	 * bother.)
	 */
	if (RedoRecPtr != Insert->RedoRecPtr)
	{
		Assert(RedoRecPtr < Insert->RedoRecPtr);
		RedoRecPtr = Insert->RedoRecPtr;
	}
	doPageWrites = (Insert->fullPageWrites || Insert->forcePageWrites);

	if (doPageWrites &&
		(!prevDoPageWrites ||
		 (fpw_lsn != InvalidXLogRecPtr && fpw_lsn <= RedoRecPtr)))
	{
		/*
		 * Oops, some buffer now needs to be backed up that the caller didn't
		 * back up.  Start over.
		 */
		WALInsertLockRelease();
		END_CRIT_SECTION();
		return InvalidXLogRecPtr;
	}

	/*
	 * Reserve space for the record in the WAL. This also sets the xl_prev
	 * pointer.
	 */
	if (isLogSwitch)
		inserted = ReserveXLogSwitch(&StartPos, &EndPos, &rechdr->xl_prev);
	else
	{
		ReserveXLogInsertLocation(rechdr->xl_tot_len, &StartPos, &EndPos,
								  &rechdr->xl_prev);
		inserted = true;
	}

	if (inserted)
	{
		/*
		 * Now that xl_prev has been filled in, calculate CRC of the record
		 * header.
		 */
		rdata_crc = rechdr->xl_crc;
		COMP_CRC32C(rdata_crc, rechdr, offsetof(XLogRecord, xl_crc));
		FIN_CRC32C(rdata_crc);
		rechdr->xl_crc = rdata_crc;

		/*
		 * All the record data, including the header, is now ready to be
		 * inserted. Copy the record in the space reserved.
		 */
		CopyXLogRecordToWAL(rechdr->xl_tot_len, isLogSwitch, rdata,
							StartPos, EndPos);

		/*
		 * Unless record is flagged as not important, update LSN of last
		 * important record in the current slot. When holding all locks, just
		 * update the first one.
		 */
		if ((flags & XLOG_MARK_UNIMPORTANT) == 0)
		{
			int			lockno = holdingAllLocks ? 0 : MyLockNo;

			WALInsertLocks[lockno].l.lastImportantAt = StartPos;
		}
	}
	else
	{
		/*
		 * This was an xlog-switch record, but the current insert location was
		 * already exactly at the beginning of a segment, so there was no need
		 * to do anything.
		 */
	}

	/*
	 * Done! Let others know that we're finished.
	 */
	WALInsertLockRelease();

	MarkCurrentTransactionIdLoggedIfAny();

	END_CRIT_SECTION();

	/*
	 * Update shared LogwrtRqst.Write, if we crossed page boundary.
	 */
	if (StartPos / XLOG_BLCKSZ != EndPos / XLOG_BLCKSZ)
	{
		SpinLockAcquire(&XLogCtl->info_lck);
		/* advance global request to include new block(s) */
		if (XLogCtl->LogwrtRqst.Write < EndPos)
			XLogCtl->LogwrtRqst.Write = EndPos;
		/* update local result copy while I have the chance */
		LogwrtResult = XLogCtl->LogwrtResult;
		SpinLockRelease(&XLogCtl->info_lck);
	}

	/*
	 * If this was an XLOG_SWITCH record, flush the record and the empty
	 * padding space that fills the rest of the segment, and perform
	 * end-of-segment actions (eg, notifying archiver).
	 */
	if (isLogSwitch)
	{
		TRACE_POSTGRESQL_WAL_SWITCH();
		XLogFlush(EndPos);

		/*
		 * Even though we reserved the rest of the segment for us, which is
		 * reflected in EndPos, we return a pointer to just the end of the
		 * xlog-switch record.
		 */
		if (inserted)
		{
			EndPos = StartPos + SizeOfXLogRecord;
			if (StartPos / XLOG_BLCKSZ != EndPos / XLOG_BLCKSZ)
			{
				uint64		offset = XLogSegmentOffset(EndPos, wal_segment_size);

				if (offset == EndPos % XLOG_BLCKSZ)
					EndPos += SizeOfXLogLongPHD;
				else
					EndPos += SizeOfXLogShortPHD;
			}
		}
	}

#ifdef WAL_DEBUG
	if (XLOG_DEBUG)
	{
		static XLogReaderState *debug_reader = NULL;
		StringInfoData buf;
		StringInfoData recordBuf;
		char	   *errormsg = NULL;
		MemoryContext oldCxt;

		oldCxt = MemoryContextSwitchTo(walDebugCxt);

		initStringInfo(&buf);
		appendStringInfo(&buf, "INSERT @ %X/%X: ",
						 (uint32) (EndPos >> 32), (uint32) EndPos);

		/*
		 * We have to piece together the WAL record data from the XLogRecData
		 * entries, so that we can pass it to the rm_desc function as one
		 * contiguous chunk.
		 */
		initStringInfo(&recordBuf);
		for (; rdata != NULL; rdata = rdata->next)
			appendBinaryStringInfo(&recordBuf, rdata->data, rdata->len);

		if (!debug_reader)
			debug_reader = XLogReaderAllocate(wal_segment_size, NULL,
											  XL_ROUTINE(), NULL);

		if (!debug_reader)
		{
			appendStringInfoString(&buf, "error decoding record: out of memory");
		}
		else if (!DecodeXLogRecord(debug_reader, (XLogRecord *) recordBuf.data,
								   &errormsg))
		{
			appendStringInfo(&buf, "error decoding record: %s",
							 errormsg ? errormsg : "no error message");
		}
		else
		{
			appendStringInfoString(&buf, " - ");
			xlog_outdesc(&buf, debug_reader);
		}
		elog(LOG, "%s", buf.data);

		pfree(buf.data);
		pfree(recordBuf.data);
		MemoryContextSwitchTo(oldCxt);
	}
#endif

	/*
	 * Update our global variables
	 */
	ProcLastRecPtr = StartPos;
	XactLastRecEnd = EndPos;

	/* Report WAL traffic to the instrumentation. */
	if (inserted)
	{
		pgWalUsage.wal_bytes += rechdr->xl_tot_len;
		pgWalUsage.wal_records++;
		pgWalUsage.wal_fpi += num_fpi;
	}

	return EndPos;
}

/*
 * Reserves the right amount of space for a record of given size from the WAL.
 * *StartPos is set to the beginning of the reserved section, *EndPos to
 * its end+1. *PrevPtr is set to the beginning of the previous record; it is
 * used to set the xl_prev of this record.
 *
 * This is the performance critical part of XLogInsert that must be serialized
 * across backends. The rest can happen mostly in parallel. Try to keep this
 * section as short as possible, insertpos_lck can be heavily contended on a
 * busy system.
 *
 * NB: The space calculation here must match the code in CopyXLogRecordToWAL,
 * where we actually copy the record to the reserved space.
 */
static void
ReserveXLogInsertLocation(int size, XLogRecPtr *StartPos, XLogRecPtr *EndPos,
						  XLogRecPtr *PrevPtr)
{
	XLogCtlInsert *Insert = &XLogCtl->Insert;
	uint64		startbytepos;
	uint64		endbytepos;
	uint64		prevbytepos;

	size = MAXALIGN(size);

	/* All (non xlog-switch) records should contain data. */
	Assert(size > SizeOfXLogRecord);

	/*
	 * The duration the spinlock needs to be held is minimized by minimizing
	 * the calculations that have to be done while holding the lock. The
	 * current tip of reserved WAL is kept in CurrBytePos, as a byte position
	 * that only counts "usable" bytes in WAL, that is, it excludes all WAL
	 * page headers. The mapping between "usable" byte positions and physical
	 * positions (XLogRecPtrs) can be done outside the locked region, and
	 * because the usable byte position doesn't include any headers, reserving
	 * X bytes from WAL is almost as simple as "CurrBytePos += X".
	 */
	SpinLockAcquire(&Insert->insertpos_lck);

	startbytepos = Insert->CurrBytePos;
	endbytepos = startbytepos + size;
	prevbytepos = Insert->PrevBytePos;
	Insert->CurrBytePos = endbytepos;
	Insert->PrevBytePos = startbytepos;

	SpinLockRelease(&Insert->insertpos_lck);

	*StartPos = XLogBytePosToRecPtr(startbytepos);
	*EndPos = XLogBytePosToEndRecPtr(endbytepos);
	*PrevPtr = XLogBytePosToRecPtr(prevbytepos);

	/*
	 * Check that the conversions between "usable byte positions" and
	 * XLogRecPtrs work consistently in both directions.
	 */
	Assert(XLogRecPtrToBytePos(*StartPos) == startbytepos);
	Assert(XLogRecPtrToBytePos(*EndPos) == endbytepos);
	Assert(XLogRecPtrToBytePos(*PrevPtr) == prevbytepos);
}

/*
 * Like ReserveXLogInsertLocation(), but for an xlog-switch record.
 *
 * A log-switch record is handled slightly differently. The rest of the
 * segment will be reserved for this insertion, as indicated by the returned
 * *EndPos value. However, if we are already at the beginning of the current
 * segment, *StartPos and *EndPos are set to the current location without
 * reserving any space, and the function returns false.
*/
static bool
ReserveXLogSwitch(XLogRecPtr *StartPos, XLogRecPtr *EndPos, XLogRecPtr *PrevPtr)
{
	XLogCtlInsert *Insert = &XLogCtl->Insert;
	uint64		startbytepos;
	uint64		endbytepos;
	uint64		prevbytepos;
	uint32		size = MAXALIGN(SizeOfXLogRecord);
	XLogRecPtr	ptr;
	uint32		segleft;

	/*
	 * These calculations are a bit heavy-weight to be done while holding a
	 * spinlock, but since we're holding all the WAL insertion locks, there
	 * are no other inserters competing for it. GetXLogInsertRecPtr() does
	 * compete for it, but that's not called very frequently.
	 */
	SpinLockAcquire(&Insert->insertpos_lck);

	startbytepos = Insert->CurrBytePos;

	ptr = XLogBytePosToEndRecPtr(startbytepos);
	if (XLogSegmentOffset(ptr, wal_segment_size) == 0)
	{
		SpinLockRelease(&Insert->insertpos_lck);
		*EndPos = *StartPos = ptr;
		return false;
	}

	endbytepos = startbytepos + size;
	prevbytepos = Insert->PrevBytePos;

	*StartPos = XLogBytePosToRecPtr(startbytepos);
	*EndPos = XLogBytePosToEndRecPtr(endbytepos);

	segleft = wal_segment_size - XLogSegmentOffset(*EndPos, wal_segment_size);
	if (segleft != wal_segment_size)
	{
		/* consume the rest of the segment */
		*EndPos += segleft;
		endbytepos = XLogRecPtrToBytePos(*EndPos);
	}
	Insert->CurrBytePos = endbytepos;
	Insert->PrevBytePos = startbytepos;

	SpinLockRelease(&Insert->insertpos_lck);

	*PrevPtr = XLogBytePosToRecPtr(prevbytepos);

	Assert(XLogSegmentOffset(*EndPos, wal_segment_size) == 0);
	Assert(XLogRecPtrToBytePos(*EndPos) == endbytepos);
	Assert(XLogRecPtrToBytePos(*StartPos) == startbytepos);
	Assert(XLogRecPtrToBytePos(*PrevPtr) == prevbytepos);

	return true;
}

/*
 * Checks whether the current buffer page and backup page stored in the
 * WAL record are consistent or not. Before comparing the two pages, a
 * masking can be applied to the pages to ignore certain areas like hint bits,
 * unused space between pd_lower and pd_upper among other things. This
 * function should be called once WAL replay has been completed for a
 * given record.
 */
static void
checkXLogConsistency(XLogReaderState *record)
{
	RmgrId		rmid = XLogRecGetRmid(record);
	RelFileNode rnode;
	ForkNumber	forknum;
	BlockNumber blkno;
	int			block_id;

	/* Records with no backup blocks have no need for consistency checks. */
	if (!XLogRecHasAnyBlockRefs(record))
		return;

	Assert((XLogRecGetInfo(record) & XLR_CHECK_CONSISTENCY) != 0);

	for (block_id = 0; block_id <= record->max_block_id; block_id++)
	{
		Buffer		buf;
		Page		page;

		if (!XLogRecGetBlockTag(record, block_id, &rnode, &forknum, &blkno))
		{
			/*
			 * WAL record doesn't contain a block reference with the given id.
			 * Do nothing.
			 */
			continue;
		}

		Assert(XLogRecHasBlockImage(record, block_id));

		if (XLogRecBlockImageApply(record, block_id))
		{
			/*
			 * WAL record has already applied the page, so bypass the
			 * consistency check as that would result in comparing the full
			 * page stored in the record with itself.
			 */
			continue;
		}

		/*
		 * Read the contents from the current buffer and store it in a
		 * temporary page.
		 */
		buf = XLogReadBufferExtended(rnode, forknum, blkno,
									 RBM_NORMAL_NO_LOG);
		if (!BufferIsValid(buf))
			continue;

		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);

		/*
		 * Take a copy of the local page where WAL has been applied to have a
		 * comparison base before masking it...
		 */
		memcpy(replay_image_masked, page, BLCKSZ);

		/* No need for this page anymore now that a copy is in. */
		UnlockReleaseBuffer(buf);

		/*
		 * If the block LSN is already ahead of this WAL record, we can't
		 * expect contents to match.  This can happen if recovery is
		 * restarted.
		 */
		if (PageGetLSN(replay_image_masked) > record->EndRecPtr)
			continue;

		/*
		 * Read the contents from the backup copy, stored in WAL record and
		 * store it in a temporary page. There is no need to allocate a new
		 * page here, a local buffer is fine to hold its contents and a mask
		 * can be directly applied on it.
		 */
		if (!RestoreBlockImage(record, block_id, master_image_masked))
			elog(ERROR, "failed to restore block image");

		/*
		 * If masking function is defined, mask both the master and replay
		 * images
		 */
		if (RmgrTable[rmid].rm_mask != NULL)
		{
			RmgrTable[rmid].rm_mask(replay_image_masked, blkno);
			RmgrTable[rmid].rm_mask(master_image_masked, blkno);
		}

		/* Time to compare the master and replay images. */
		if (memcmp(replay_image_masked, master_image_masked, BLCKSZ) != 0)
		{
			elog(FATAL,
				 "inconsistent page found, rel %u/%u/%u, forknum %u, blkno %u",
				 rnode.spcNode, rnode.dbNode, rnode.relNode,
				 forknum, blkno);
		}
	}
}

/*
 * Subroutine of XLogInsertRecord.  Copies a WAL record to an already-reserved
 * area in the WAL.
 */
static void
CopyXLogRecordToWAL(int write_len, bool isLogSwitch, XLogRecData *rdata,
					XLogRecPtr StartPos, XLogRecPtr EndPos)
{
	char	   *currpos;
	int			freespace;
	int			written;
	XLogRecPtr	CurrPos;
	XLogPageHeader pagehdr;

	/*
	 * Get a pointer to the right place in the right WAL buffer to start
	 * inserting to.
	 */
	CurrPos = StartPos;
	currpos = GetXLogBuffer(CurrPos);
	freespace = INSERT_FREESPACE(CurrPos);

	/*
	 * there should be enough space for at least the first field (xl_tot_len)
	 * on this page.
	 */
	Assert(freespace >= sizeof(uint32));

	/* Copy record data */
	written = 0;
	while (rdata != NULL)
	{
		char	   *rdata_data = rdata->data;
		int			rdata_len = rdata->len;

		while (rdata_len > freespace)
		{
			/*
			 * Write what fits on this page, and continue on the next page.
			 */
			Assert(CurrPos % XLOG_BLCKSZ >= SizeOfXLogShortPHD || freespace == 0);
			memcpy(currpos, rdata_data, freespace);
			rdata_data += freespace;
			rdata_len -= freespace;
			written += freespace;
			CurrPos += freespace;

			/*
			 * Get pointer to beginning of next page, and set the xlp_rem_len
			 * in the page header. Set XLP_FIRST_IS_CONTRECORD.
			 *
			 * It's safe to set the contrecord flag and xlp_rem_len without a
			 * lock on the page. All the other flags were already set when the
			 * page was initialized, in AdvanceXLInsertBuffer, and we're the
			 * only backend that needs to set the contrecord flag.
			 */
			currpos = GetXLogBuffer(CurrPos);
			pagehdr = (XLogPageHeader) currpos;
			pagehdr->xlp_rem_len = write_len - written;
			pagehdr->xlp_info |= XLP_FIRST_IS_CONTRECORD;

			/* skip over the page header */
			if (XLogSegmentOffset(CurrPos, wal_segment_size) == 0)
			{
				CurrPos += SizeOfXLogLongPHD;
				currpos += SizeOfXLogLongPHD;
			}
			else
			{
				CurrPos += SizeOfXLogShortPHD;
				currpos += SizeOfXLogShortPHD;
			}
			freespace = INSERT_FREESPACE(CurrPos);
		}

		Assert(CurrPos % XLOG_BLCKSZ >= SizeOfXLogShortPHD || rdata_len == 0);
		memcpy(currpos, rdata_data, rdata_len);
		currpos += rdata_len;
		CurrPos += rdata_len;
		freespace -= rdata_len;
		written += rdata_len;

		rdata = rdata->next;
	}
	Assert(written == write_len);

	/*
	 * If this was an xlog-switch, it's not enough to write the switch record,
	 * we also have to consume all the remaining space in the WAL segment.  We
	 * have already reserved that space, but we need to actually fill it.
	 */
	if (isLogSwitch && XLogSegmentOffset(CurrPos, wal_segment_size) != 0)
	{
		/* An xlog-switch record doesn't contain any data besides the header */
		Assert(write_len == SizeOfXLogRecord);

		/* Assert that we did reserve the right amount of space */
		Assert(XLogSegmentOffset(EndPos, wal_segment_size) == 0);

		/* Use up all the remaining space on the current page */
		CurrPos += freespace;

		/*
		 * Cause all remaining pages in the segment to be flushed, leaving the
		 * XLog position where it should be, at the start of the next segment.
		 * We do this one page at a time, to make sure we don't deadlock
		 * against ourselves if wal_buffers < wal_segment_size.
		 */
		while (CurrPos < EndPos)
		{
			/*
			 * The minimal action to flush the page would be to call
			 * WALInsertLockUpdateInsertingAt(CurrPos) followed by
			 * AdvanceXLInsertBuffer(...).  The page would be left initialized
			 * mostly to zeros, except for the page header (always the short
			 * variant, as this is never a segment's first page).
			 *
			 * The large vistas of zeros are good for compressibility, but the
			 * headers interrupting them every XLOG_BLCKSZ (with values that
			 * differ from page to page) are not.  The effect varies with
			 * compression tool, but bzip2 for instance compresses about an
			 * order of magnitude worse if those headers are left in place.
			 *
			 * Rather than complicating AdvanceXLInsertBuffer itself (which is
			 * called in heavily-loaded circumstances as well as this lightly-
			 * loaded one) with variant behavior, we just use GetXLogBuffer
			 * (which itself calls the two methods we need) to get the pointer
			 * and zero most of the page.  Then we just zero the page header.
			 */
			currpos = GetXLogBuffer(CurrPos);
			MemSet(currpos, 0, SizeOfXLogShortPHD);

			CurrPos += XLOG_BLCKSZ;
		}
	}
	else
	{
		/* Align the end position, so that the next record starts aligned */
		CurrPos = MAXALIGN64(CurrPos);
	}

	if (CurrPos != EndPos)
		elog(PANIC, "space reserved for WAL record does not match what was written");
}

/*
 * Acquire a WAL insertion lock, for inserting to WAL.
 */
static void
WALInsertLockAcquire(void)
{
	bool		immed;

	/*
	 * It doesn't matter which of the WAL insertion locks we acquire, so try
	 * the one we used last time.  If the system isn't particularly busy, it's
	 * a good bet that it's still available, and it's good to have some
	 * affinity to a particular lock so that you don't unnecessarily bounce
	 * cache lines between processes when there's no contention.
	 *
	 * If this is the first time through in this backend, pick a lock
	 * (semi-)randomly.  This allows the locks to be used evenly if you have a
	 * lot of very short connections.
	 */
	static int	lockToTry = -1;

	if (lockToTry == -1)
		lockToTry = MyProc->pgprocno % NUM_XLOGINSERT_LOCKS;
	MyLockNo = lockToTry;

	/*
	 * The insertingAt value is initially set to 0, as we don't know our
	 * insert location yet.
	 */
	immed = LWLockAcquire(&WALInsertLocks[MyLockNo].l.lock, LW_EXCLUSIVE);
	if (!immed)
	{
		/*
		 * If we couldn't get the lock immediately, try another lock next
		 * time.  On a system with more insertion locks than concurrent
		 * inserters, this causes all the inserters to eventually migrate to a
		 * lock that no-one else is using.  On a system with more inserters
		 * than locks, it still helps to distribute the inserters evenly
		 * across the locks.
		 */
		lockToTry = (lockToTry + 1) % NUM_XLOGINSERT_LOCKS;
	}
}

/*
 * Acquire all WAL insertion locks, to prevent other backends from inserting
 * to WAL.
 */
static void
WALInsertLockAcquireExclusive(void)
{
	int			i;

	/*
	 * When holding all the locks, all but the last lock's insertingAt
	 * indicator is set to 0xFFFFFFFFFFFFFFFF, which is higher than any real
	 * XLogRecPtr value, to make sure that no-one blocks waiting on those.
	 */
	for (i = 0; i < NUM_XLOGINSERT_LOCKS - 1; i++)
	{
		LWLockAcquire(&WALInsertLocks[i].l.lock, LW_EXCLUSIVE);
		LWLockUpdateVar(&WALInsertLocks[i].l.lock,
						&WALInsertLocks[i].l.insertingAt,
						PG_UINT64_MAX);
	}
	/* Variable value reset to 0 at release */
	LWLockAcquire(&WALInsertLocks[i].l.lock, LW_EXCLUSIVE);

	holdingAllLocks = true;
}

/*
 * Release our insertion lock (or locks, if we're holding them all).
 *
 * NB: Reset all variables to 0, so they cause LWLockWaitForVar to block the
 * next time the lock is acquired.
 */
static void
WALInsertLockRelease(void)
{
	if (holdingAllLocks)
	{
		int			i;

		for (i = 0; i < NUM_XLOGINSERT_LOCKS; i++)
			LWLockReleaseClearVar(&WALInsertLocks[i].l.lock,
								  &WALInsertLocks[i].l.insertingAt,
								  0);

		holdingAllLocks = false;
	}
	else
	{
		LWLockReleaseClearVar(&WALInsertLocks[MyLockNo].l.lock,
							  &WALInsertLocks[MyLockNo].l.insertingAt,
							  0);
	}
}

/*
 * Update our insertingAt value, to let others know that we've finished
 * inserting up to that point.
 */
static void
WALInsertLockUpdateInsertingAt(XLogRecPtr insertingAt)
{
	if (holdingAllLocks)
	{
		/*
		 * We use the last lock to mark our actual position, see comments in
		 * WALInsertLockAcquireExclusive.
		 */
		LWLockUpdateVar(&WALInsertLocks[NUM_XLOGINSERT_LOCKS - 1].l.lock,
						&WALInsertLocks[NUM_XLOGINSERT_LOCKS - 1].l.insertingAt,
						insertingAt);
	}
	else
		LWLockUpdateVar(&WALInsertLocks[MyLockNo].l.lock,
						&WALInsertLocks[MyLockNo].l.insertingAt,
						insertingAt);
}

/*
 * Wait for any WAL insertions < upto to finish.
 *
 * Returns the location of the oldest insertion that is still in-progress.
 * Any WAL prior to that point has been fully copied into WAL buffers, and
 * can be flushed out to disk. Because this waits for any insertions older
 * than 'upto' to finish, the return value is always >= 'upto'.
 *
 * Note: When you are about to write out WAL, you must call this function
 * *before* acquiring WALWriteLock, to avoid deadlocks. This function might
 * need to wait for an insertion to finish (or at least advance to next
 * uninitialized page), and the inserter might need to evict an old WAL buffer
 * to make room for a new one, which in turn requires WALWriteLock.
 */
static XLogRecPtr
WaitXLogInsertionsToFinish(XLogRecPtr upto)
{
	uint64		bytepos;
	XLogRecPtr	reservedUpto;
	XLogRecPtr	finishedUpto;
	XLogCtlInsert *Insert = &XLogCtl->Insert;
	int			i;

	if (MyProc == NULL)
		elog(PANIC, "cannot wait without a PGPROC structure");

	/* Read the current insert position */
	SpinLockAcquire(&Insert->insertpos_lck);
	bytepos = Insert->CurrBytePos;
	SpinLockRelease(&Insert->insertpos_lck);
	reservedUpto = XLogBytePosToEndRecPtr(bytepos);

	/*
	 * No-one should request to flush a piece of WAL that hasn't even been
	 * reserved yet. However, it can happen if there is a block with a bogus
	 * LSN on disk, for example. XLogFlush checks for that situation and
	 * complains, but only after the flush. Here we just assume that to mean
	 * that all WAL that has been reserved needs to be finished. In this
	 * corner-case, the return value can be smaller than 'upto' argument.
	 */
	if (upto > reservedUpto)
	{
		elog(LOG, "request to flush past end of generated WAL; request %X/%X, currpos %X/%X",
			 (uint32) (upto >> 32), (uint32) upto,
			 (uint32) (reservedUpto >> 32), (uint32) reservedUpto);
		upto = reservedUpto;
	}

	/*
	 * Loop through all the locks, sleeping on any in-progress insert older
	 * than 'upto'.
	 *
	 * finishedUpto is our return value, indicating the point upto which all
	 * the WAL insertions have been finished. Initialize it to the head of
	 * reserved WAL, and as we iterate through the insertion locks, back it
	 * out for any insertion that's still in progress.
	 */
	finishedUpto = reservedUpto;
	for (i = 0; i < NUM_XLOGINSERT_LOCKS; i++)
	{
		XLogRecPtr	insertingat = InvalidXLogRecPtr;

		do
		{
			/*
			 * See if this insertion is in progress.  LWLockWaitForVar will
			 * wait for the lock to be released, or for the 'value' to be set
			 * by a LWLockUpdateVar call.  When a lock is initially acquired,
			 * its value is 0 (InvalidXLogRecPtr), which means that we don't
			 * know where it's inserting yet.  We will have to wait for it. If
			 * it's a small insertion, the record will most likely fit on the
			 * same page and the inserter will release the lock without ever
			 * calling LWLockUpdateVar.  But if it has to sleep, it will
			 * advertise the insertion point with LWLockUpdateVar before
			 * sleeping.
			 */
			if (LWLockWaitForVar(&WALInsertLocks[i].l.lock,
								 &WALInsertLocks[i].l.insertingAt,
								 insertingat, &insertingat))
			{
				/* the lock was free, so no insertion in progress */
				insertingat = InvalidXLogRecPtr;
				break;
			}

			/*
			 * This insertion is still in progress. Have to wait, unless the
			 * inserter has proceeded past 'upto'.
			 */
		} while (insertingat < upto);

		if (insertingat != InvalidXLogRecPtr && insertingat < finishedUpto)
			finishedUpto = insertingat;
	}
	return finishedUpto;
}

/*
 * Get a pointer to the right location in the WAL buffer containing the
 * given XLogRecPtr.
 *
 * If the page is not initialized yet, it is initialized. That might require
 * evicting an old dirty buffer from the buffer cache, which means I/O.
 *
 * The caller must ensure that the page containing the requested location
 * isn't evicted yet, and won't be evicted. The way to ensure that is to
 * hold onto a WAL insertion lock with the insertingAt position set to
 * something <= ptr. GetXLogBuffer() will update insertingAt if it needs
 * to evict an old page from the buffer. (This means that once you call
 * GetXLogBuffer() with a given 'ptr', you must not access anything before
 * that point anymore, and must not call GetXLogBuffer() with an older 'ptr'
 * later, because older buffers might be recycled already)
 */
static char *
GetXLogBuffer(XLogRecPtr ptr)
{
	int			idx;
	XLogRecPtr	endptr;
	static uint64 cachedPage = 0;
	static char *cachedPos = NULL;
	XLogRecPtr	expectedEndPtr;

	/*
	 * Fast path for the common case that we need to access again the same
	 * page as last time.
	 */
	if (ptr / XLOG_BLCKSZ == cachedPage)
	{
		Assert(((XLogPageHeader) cachedPos)->xlp_magic == XLOG_PAGE_MAGIC);
		Assert(((XLogPageHeader) cachedPos)->xlp_pageaddr == ptr - (ptr % XLOG_BLCKSZ));
		return cachedPos + ptr % XLOG_BLCKSZ;
	}

	/*
	 * The XLog buffer cache is organized so that a page is always loaded to a
	 * particular buffer.  That way we can easily calculate the buffer a given
	 * page must be loaded into, from the XLogRecPtr alone.
	 */
	idx = XLogRecPtrToBufIdx(ptr);

	/*
	 * See what page is loaded in the buffer at the moment. It could be the
	 * page we're looking for, or something older. It can't be anything newer
	 * - that would imply the page we're looking for has already been written
	 * out to disk and evicted, and the caller is responsible for making sure
	 * that doesn't happen.
	 *
	 * However, we don't hold a lock while we read the value. If someone has
	 * just initialized the page, it's possible that we get a "torn read" of
	 * the XLogRecPtr if 64-bit fetches are not atomic on this platform. In
	 * that case we will see a bogus value. That's ok, we'll grab the mapping
	 * lock (in AdvanceXLInsertBuffer) and retry if we see anything else than
	 * the page we're looking for. But it means that when we do this unlocked
	 * read, we might see a value that appears to be ahead of the page we're
	 * looking for. Don't PANIC on that, until we've verified the value while
	 * holding the lock.
	 */
	expectedEndPtr = ptr;
	expectedEndPtr += XLOG_BLCKSZ - ptr % XLOG_BLCKSZ;

	endptr = XLogCtl->xlblocks[idx];
	if (expectedEndPtr != endptr)
	{
		XLogRecPtr	initializedUpto;

		/*
		 * Before calling AdvanceXLInsertBuffer(), which can block, let others
		 * know how far we're finished with inserting the record.
		 *
		 * NB: If 'ptr' points to just after the page header, advertise a
		 * position at the beginning of the page rather than 'ptr' itself. If
		 * there are no other insertions running, someone might try to flush
		 * up to our advertised location. If we advertised a position after
		 * the page header, someone might try to flush the page header, even
		 * though page might actually not be initialized yet. As the first
		 * inserter on the page, we are effectively responsible for making
		 * sure that it's initialized, before we let insertingAt to move past
		 * the page header.
		 */
		if (ptr % XLOG_BLCKSZ == SizeOfXLogShortPHD &&
			XLogSegmentOffset(ptr, wal_segment_size) > XLOG_BLCKSZ)
			initializedUpto = ptr - SizeOfXLogShortPHD;
		else if (ptr % XLOG_BLCKSZ == SizeOfXLogLongPHD &&
				 XLogSegmentOffset(ptr, wal_segment_size) < XLOG_BLCKSZ)
			initializedUpto = ptr - SizeOfXLogLongPHD;
		else
			initializedUpto = ptr;

		WALInsertLockUpdateInsertingAt(initializedUpto);

		AdvanceXLInsertBuffer(ptr, false);
		endptr = XLogCtl->xlblocks[idx];

		if (expectedEndPtr != endptr)
			elog(PANIC, "could not find WAL buffer for %X/%X",
				 (uint32) (ptr >> 32), (uint32) ptr);
	}
	else
	{
		/*
		 * Make sure the initialization of the page is visible to us, and
		 * won't arrive later to overwrite the WAL data we write on the page.
		 */
		pg_memory_barrier();
	}

	/*
	 * Found the buffer holding this page. Return a pointer to the right
	 * offset within the page.
	 */
	cachedPage = ptr / XLOG_BLCKSZ;
	cachedPos = XLogCtl->pages + idx * (Size) XLOG_BLCKSZ;

	Assert(((XLogPageHeader) cachedPos)->xlp_magic == XLOG_PAGE_MAGIC);
	Assert(((XLogPageHeader) cachedPos)->xlp_pageaddr == ptr - (ptr % XLOG_BLCKSZ));

	return cachedPos + ptr % XLOG_BLCKSZ;
}

/*
 * Converts a "usable byte position" to XLogRecPtr. A usable byte position
 * is the position starting from the beginning of WAL, excluding all WAL
 * page headers.
 */
static XLogRecPtr
XLogBytePosToRecPtr(uint64 bytepos)
{
	uint64		fullsegs;
	uint64		fullpages;
	uint64		bytesleft;
	uint32		seg_offset;
	XLogRecPtr	result;

	fullsegs = bytepos / UsableBytesInSegment;
	bytesleft = bytepos % UsableBytesInSegment;

	if (bytesleft < XLOG_BLCKSZ - SizeOfXLogLongPHD)
	{
		/* fits on first page of segment */
		seg_offset = bytesleft + SizeOfXLogLongPHD;
	}
	else
	{
		/* account for the first page on segment with long header */
		seg_offset = XLOG_BLCKSZ;
		bytesleft -= XLOG_BLCKSZ - SizeOfXLogLongPHD;

		fullpages = bytesleft / UsableBytesInPage;
		bytesleft = bytesleft % UsableBytesInPage;

		seg_offset += fullpages * XLOG_BLCKSZ + bytesleft + SizeOfXLogShortPHD;
	}

	XLogSegNoOffsetToRecPtr(fullsegs, seg_offset, wal_segment_size, result);

	return result;
}

/*
 * Like XLogBytePosToRecPtr, but if the position is at a page boundary,
 * returns a pointer to the beginning of the page (ie. before page header),
 * not to where the first xlog record on that page would go to. This is used
 * when converting a pointer to the end of a record.
 */
static XLogRecPtr
XLogBytePosToEndRecPtr(uint64 bytepos)
{
	uint64		fullsegs;
	uint64		fullpages;
	uint64		bytesleft;
	uint32		seg_offset;
	XLogRecPtr	result;

	fullsegs = bytepos / UsableBytesInSegment;
	bytesleft = bytepos % UsableBytesInSegment;

	if (bytesleft < XLOG_BLCKSZ - SizeOfXLogLongPHD)
	{
		/* fits on first page of segment */
		if (bytesleft == 0)
			seg_offset = 0;
		else
			seg_offset = bytesleft + SizeOfXLogLongPHD;
	}
	else
	{
		/* account for the first page on segment with long header */
		seg_offset = XLOG_BLCKSZ;
		bytesleft -= XLOG_BLCKSZ - SizeOfXLogLongPHD;

		fullpages = bytesleft / UsableBytesInPage;
		bytesleft = bytesleft % UsableBytesInPage;

		if (bytesleft == 0)
			seg_offset += fullpages * XLOG_BLCKSZ + bytesleft;
		else
			seg_offset += fullpages * XLOG_BLCKSZ + bytesleft + SizeOfXLogShortPHD;
	}

	XLogSegNoOffsetToRecPtr(fullsegs, seg_offset, wal_segment_size, result);

	return result;
}

/*
 * Convert an XLogRecPtr to a "usable byte position".
 */
static uint64
XLogRecPtrToBytePos(XLogRecPtr ptr)
{
	uint64		fullsegs;
	uint32		fullpages;
	uint32		offset;
	uint64		result;

	XLByteToSeg(ptr, fullsegs, wal_segment_size);

	fullpages = (XLogSegmentOffset(ptr, wal_segment_size)) / XLOG_BLCKSZ;
	offset = ptr % XLOG_BLCKSZ;

	if (fullpages == 0)
	{
		result = fullsegs * UsableBytesInSegment;
		if (offset > 0)
		{
			Assert(offset >= SizeOfXLogLongPHD);
			result += offset - SizeOfXLogLongPHD;
		}
	}
	else
	{
		result = fullsegs * UsableBytesInSegment +
			(XLOG_BLCKSZ - SizeOfXLogLongPHD) + /* account for first page */
			(fullpages - 1) * UsableBytesInPage;	/* full pages */
		if (offset > 0)
		{
			Assert(offset >= SizeOfXLogShortPHD);
			result += offset - SizeOfXLogShortPHD;
		}
	}

	return result;
}

/*
 * Initialize XLOG buffers, writing out old buffers if they still contain
 * unwritten data, upto the page containing 'upto'. Or if 'opportunistic' is
 * true, initialize as many pages as we can without having to write out
 * unwritten data. Any new pages are initialized to zeros, with pages headers
 * initialized properly.
 */
static void
AdvanceXLInsertBuffer(XLogRecPtr upto, bool opportunistic)
{
	XLogCtlInsert *Insert = &XLogCtl->Insert;
	int			nextidx;
	XLogRecPtr	OldPageRqstPtr;
	XLogwrtRqst WriteRqst;
	XLogRecPtr	NewPageEndPtr = InvalidXLogRecPtr;
	XLogRecPtr	NewPageBeginPtr;
	XLogPageHeader NewPage;
	int			npages pg_attribute_unused() = 0;

	LWLockAcquire(WALBufMappingLock, LW_EXCLUSIVE);

	/*
	 * Now that we have the lock, check if someone initialized the page
	 * already.
	 */
	while (upto >= XLogCtl->InitializedUpTo || opportunistic)
	{
		nextidx = XLogRecPtrToBufIdx(XLogCtl->InitializedUpTo);

		/*
		 * Get ending-offset of the buffer page we need to replace (this may
		 * be zero if the buffer hasn't been used yet).  Fall through if it's
		 * already written out.
		 */
		OldPageRqstPtr = XLogCtl->xlblocks[nextidx];
		if (LogwrtResult.Write < OldPageRqstPtr)
		{
			/*
			 * Nope, got work to do. If we just want to pre-initialize as much
			 * as we can without flushing, give up now.
			 */
			if (opportunistic)
				break;

			/* Before waiting, get info_lck and update LogwrtResult */
			SpinLockAcquire(&XLogCtl->info_lck);
			if (XLogCtl->LogwrtRqst.Write < OldPageRqstPtr)
				XLogCtl->LogwrtRqst.Write = OldPageRqstPtr;
			LogwrtResult = XLogCtl->LogwrtResult;
			SpinLockRelease(&XLogCtl->info_lck);

			/*
			 * Now that we have an up-to-date LogwrtResult value, see if we
			 * still need to write it or if someone else already did.
			 */
			if (LogwrtResult.Write < OldPageRqstPtr)
			{
				/*
				 * Must acquire write lock. Release WALBufMappingLock first,
				 * to make sure that all insertions that we need to wait for
				 * can finish (up to this same position). Otherwise we risk
				 * deadlock.
				 */
				LWLockRelease(WALBufMappingLock);

				WaitXLogInsertionsToFinish(OldPageRqstPtr);

				LWLockAcquire(WALWriteLock, LW_EXCLUSIVE);

				LogwrtResult = XLogCtl->LogwrtResult;
				if (LogwrtResult.Write >= OldPageRqstPtr)
				{
					/* OK, someone wrote it already */
					LWLockRelease(WALWriteLock);
				}
				else
				{
					/* Have to write it ourselves */
					TRACE_POSTGRESQL_WAL_BUFFER_WRITE_DIRTY_START();
					WriteRqst.Write = OldPageRqstPtr;
					WriteRqst.Flush = 0;
					XLogWrite(WriteRqst, false);
					LWLockRelease(WALWriteLock);
					TRACE_POSTGRESQL_WAL_BUFFER_WRITE_DIRTY_DONE();
				}
				/* Re-acquire WALBufMappingLock and retry */
				LWLockAcquire(WALBufMappingLock, LW_EXCLUSIVE);
				continue;
			}
		}

		/*
		 * Now the next buffer slot is free and we can set it up to be the
		 * next output page.
		 */
		NewPageBeginPtr = XLogCtl->InitializedUpTo;
		NewPageEndPtr = NewPageBeginPtr + XLOG_BLCKSZ;

		Assert(XLogRecPtrToBufIdx(NewPageBeginPtr) == nextidx);

		NewPage = (XLogPageHeader) (XLogCtl->pages + nextidx * (Size) XLOG_BLCKSZ);

		/*
		 * Be sure to re-zero the buffer so that bytes beyond what we've
		 * written will look like zeroes and not valid XLOG records...
		 */
		MemSet((char *) NewPage, 0, XLOG_BLCKSZ);

		/*
		 * Fill the new page's header
		 */
		NewPage->xlp_magic = XLOG_PAGE_MAGIC;

		/* NewPage->xlp_info = 0; */	/* done by memset */
		NewPage->xlp_tli = ThisTimeLineID;
		NewPage->xlp_pageaddr = NewPageBeginPtr;

		/* NewPage->xlp_rem_len = 0; */	/* done by memset */

		/*
		 * If online backup is not in progress, mark the header to indicate
		 * that WAL records beginning in this page have removable backup
		 * blocks.  This allows the WAL archiver to know whether it is safe to
		 * compress archived WAL data by transforming full-block records into
		 * the non-full-block format.  It is sufficient to record this at the
		 * page level because we force a page switch (in fact a segment
		 * switch) when starting a backup, so the flag will be off before any
		 * records can be written during the backup.  At the end of a backup,
		 * the last page will be marked as all unsafe when perhaps only part
		 * is unsafe, but at worst the archiver would miss the opportunity to
		 * compress a few records.
		 */
		if (!Insert->forcePageWrites)
			NewPage->xlp_info |= XLP_BKP_REMOVABLE;

		/*
		 * If a record was found to be broken at the end of recovery, and
		 * we're going to write on the page where its first contrecord was
		 * lost, set the XLP_FIRST_IS_OVERWRITE_CONTRECORD flag on the page
		 * header.  See CreateOverwriteContrecordRecord().
		 */
		if (missingContrecPtr == NewPageBeginPtr)
		{
			NewPage->xlp_info |= XLP_FIRST_IS_OVERWRITE_CONTRECORD;
			missingContrecPtr = InvalidXLogRecPtr;
		}

		/*
		 * If first page of an XLOG segment file, make it a long header.
		 */
		if ((XLogSegmentOffset(NewPage->xlp_pageaddr, wal_segment_size)) == 0)
		{
			XLogLongPageHeader NewLongPage = (XLogLongPageHeader) NewPage;

			NewLongPage->xlp_sysid = ControlFile->system_identifier;
			NewLongPage->xlp_seg_size = wal_segment_size;
			NewLongPage->xlp_xlog_blcksz = XLOG_BLCKSZ;
			NewPage->xlp_info |= XLP_LONG_HEADER;
		}

		/*
		 * Make sure the initialization of the page becomes visible to others
		 * before the xlblocks update. GetXLogBuffer() reads xlblocks without
		 * holding a lock.
		 */
		pg_write_barrier();

		*((volatile XLogRecPtr *) &XLogCtl->xlblocks[nextidx]) = NewPageEndPtr;

		XLogCtl->InitializedUpTo = NewPageEndPtr;

		npages++;
	}
	LWLockRelease(WALBufMappingLock);

#ifdef WAL_DEBUG
	if (XLOG_DEBUG && npages > 0)
	{
		elog(DEBUG1, "initialized %d pages, up to %X/%X",
			 npages, (uint32) (NewPageEndPtr >> 32), (uint32) NewPageEndPtr);
	}
#endif
}

/*
 * Calculate CheckPointSegments based on max_wal_size_mb and
 * checkpoint_completion_target.
 */
static void
CalculateCheckpointSegments(void)
{
	double		target;

	/*-------
	 * Calculate the distance at which to trigger a checkpoint, to avoid
	 * exceeding max_wal_size_mb. This is based on two assumptions:
	 *
	 * a) we keep WAL for only one checkpoint cycle (prior to PG11 we kept
	 *    WAL for two checkpoint cycles to allow us to recover from the
	 *    secondary checkpoint if the first checkpoint failed, though we
	 *    only did this on the master anyway, not on standby. Keeping just
	 *    one checkpoint simplifies processing and reduces disk space in
	 *    many smaller databases.)
	 * b) during checkpoint, we consume checkpoint_completion_target *
	 *	  number of segments consumed between checkpoints.
	 *-------
	 */
	target = (double) ConvertToXSegs(max_wal_size_mb, wal_segment_size) /
		(1.0 + CheckPointCompletionTarget);

	/* round down */
	CheckPointSegments = (int) target;

	if (CheckPointSegments < 1)
		CheckPointSegments = 1;
}

void
assign_max_wal_size(int newval, void *extra)
{
	max_wal_size_mb = newval;
	CalculateCheckpointSegments();
}

void
assign_checkpoint_completion_target(double newval, void *extra)
{
	CheckPointCompletionTarget = newval;
	CalculateCheckpointSegments();
}

/*
 * At a checkpoint, how many WAL segments to recycle as preallocated future
 * XLOG segments? Returns the highest segment that should be preallocated.
 */
static XLogSegNo
XLOGfileslop(XLogRecPtr lastredoptr)
{
	XLogSegNo	minSegNo;
	XLogSegNo	maxSegNo;
	double		distance;
	XLogSegNo	recycleSegNo;

	/*
	 * Calculate the segment numbers that min_wal_size_mb and max_wal_size_mb
	 * correspond to. Always recycle enough segments to meet the minimum, and
	 * remove enough segments to stay below the maximum.
	 */
	minSegNo = lastredoptr / wal_segment_size +
		ConvertToXSegs(min_wal_size_mb, wal_segment_size) - 1;
	maxSegNo = lastredoptr / wal_segment_size +
		ConvertToXSegs(max_wal_size_mb, wal_segment_size) - 1;

	/*
	 * Between those limits, recycle enough segments to get us through to the
	 * estimated end of next checkpoint.
	 *
	 * To estimate where the next checkpoint will finish, assume that the
	 * system runs steadily consuming CheckPointDistanceEstimate bytes between
	 * every checkpoint.
	 */
	distance = (1.0 + CheckPointCompletionTarget) * CheckPointDistanceEstimate;
	/* add 10% for good measure. */
	distance *= 1.10;

	recycleSegNo = (XLogSegNo) ceil(((double) lastredoptr + distance) /
									wal_segment_size);

	if (recycleSegNo < minSegNo)
		recycleSegNo = minSegNo;
	if (recycleSegNo > maxSegNo)
		recycleSegNo = maxSegNo;

	return recycleSegNo;
}

/*
 * Check whether we've consumed enough xlog space that a checkpoint is needed.
 *
 * new_segno indicates a log file that has just been filled up (or read
 * during recovery). We measure the distance from RedoRecPtr to new_segno
 * and see if that exceeds CheckPointSegments.
 *
 * Note: it is caller's responsibility that RedoRecPtr is up-to-date.
 */
static bool
XLogCheckpointNeeded(XLogSegNo new_segno)
{
	XLogSegNo	old_segno;

	XLByteToSeg(RedoRecPtr, old_segno, wal_segment_size);

	if (new_segno >= old_segno + (uint64) (CheckPointSegments - 1))
		return true;
	return false;
}

/*
 * Write and/or fsync the log at least as far as WriteRqst indicates.
 *
 * If flexible == true, we don't have to write as far as WriteRqst, but
 * may stop at any convenient boundary (such as a cache or logfile boundary).
 * This option allows us to avoid uselessly issuing multiple writes when a
 * single one would do.
 *
 * Must be called with WALWriteLock held. WaitXLogInsertionsToFinish(WriteRqst)
 * must be called before grabbing the lock, to make sure the data is ready to
 * write.
 */
static void
XLogWrite(XLogwrtRqst WriteRqst, bool flexible)
{
	bool		ispartialpage;
	bool		last_iteration;
	bool		finishing_seg;
	bool		use_existent;
	int			curridx;
	int			npages;
	int			startidx;
	uint32		startoffset;

	/* We should always be inside a critical section here */
	Assert(CritSectionCount > 0);

	/*
	 * Update local LogwrtResult (caller probably did this already, but...)
	 */
	LogwrtResult = XLogCtl->LogwrtResult;

	/*
	 * Since successive pages in the xlog cache are consecutively allocated,
	 * we can usually gather multiple pages together and issue just one
	 * write() call.  npages is the number of pages we have determined can be
	 * written together; startidx is the cache block index of the first one,
	 * and startoffset is the file offset at which it should go. The latter
	 * two variables are only valid when npages > 0, but we must initialize
	 * all of them to keep the compiler quiet.
	 */
	npages = 0;
	startidx = 0;
	startoffset = 0;

	/*
	 * Within the loop, curridx is the cache block index of the page to
	 * consider writing.  Begin at the buffer containing the next unwritten
	 * page, or last partially written page.
	 */
	curridx = XLogRecPtrToBufIdx(LogwrtResult.Write);

	while (LogwrtResult.Write < WriteRqst.Write)
	{
		/*
		 * Make sure we're not ahead of the insert process.  This could happen
		 * if we're passed a bogus WriteRqst.Write that is past the end of the
		 * last page that's been initialized by AdvanceXLInsertBuffer.
		 */
		XLogRecPtr	EndPtr = XLogCtl->xlblocks[curridx];

		if (LogwrtResult.Write >= EndPtr)
			elog(PANIC, "xlog write request %X/%X is past end of log %X/%X",
				 (uint32) (LogwrtResult.Write >> 32),
				 (uint32) LogwrtResult.Write,
				 (uint32) (EndPtr >> 32), (uint32) EndPtr);

		/* Advance LogwrtResult.Write to end of current buffer page */
		LogwrtResult.Write = EndPtr;
		ispartialpage = WriteRqst.Write < LogwrtResult.Write;

		if (!XLByteInPrevSeg(LogwrtResult.Write, openLogSegNo,
							 wal_segment_size))
		{
			/*
			 * Switch to new logfile segment.  We cannot have any pending
			 * pages here (since we dump what we have at segment end).
			 */
			Assert(npages == 0);
			if (openLogFile >= 0)
				XLogFileClose();
			XLByteToPrevSeg(LogwrtResult.Write, openLogSegNo,
							wal_segment_size);

			/* create/use new log file */
			use_existent = true;
			openLogFile = XLogFileInit(openLogSegNo, &use_existent, true);
			ReserveExternalFD();
		}

		/* Make sure we have the current logfile open */
		if (openLogFile < 0)
		{
			XLByteToPrevSeg(LogwrtResult.Write, openLogSegNo,
							wal_segment_size);
			openLogFile = XLogFileOpen(openLogSegNo);
			ReserveExternalFD();
		}

		/* Add current page to the set of pending pages-to-dump */
		if (npages == 0)
		{
			/* first of group */
			startidx = curridx;
			startoffset = XLogSegmentOffset(LogwrtResult.Write - XLOG_BLCKSZ,
											wal_segment_size);
		}
		npages++;

		/*
		 * Dump the set if this will be the last loop iteration, or if we are
		 * at the last page of the cache area (since the next page won't be
		 * contiguous in memory), or if we are at the end of the logfile
		 * segment.
		 */
		last_iteration = WriteRqst.Write <= LogwrtResult.Write;

		finishing_seg = !ispartialpage &&
			(startoffset + npages * XLOG_BLCKSZ) >= wal_segment_size;

		if (last_iteration ||
			curridx == XLogCtl->XLogCacheBlck ||
			finishing_seg)
		{
			char	   *from;
			Size		nbytes;
			Size		nleft;
			int			written;

			/* OK to write the page(s) */
			from = XLogCtl->pages + startidx * (Size) XLOG_BLCKSZ;
			nbytes = npages * (Size) XLOG_BLCKSZ;
			nleft = nbytes;
			do
			{
				errno = 0;
				pgstat_report_wait_start(WAIT_EVENT_WAL_WRITE);
				written = pg_pwrite(openLogFile, from, nleft, startoffset);
				pgstat_report_wait_end();
				if (written <= 0)
				{
					char		xlogfname[MAXFNAMELEN];
					int			save_errno;

					if (errno == EINTR)
						continue;

					save_errno = errno;
					XLogFileName(xlogfname, ThisTimeLineID, openLogSegNo,
								 wal_segment_size);
					errno = save_errno;
					ereport(PANIC,
							(errcode_for_file_access(),
							 errmsg("could not write to log file %s "
									"at offset %u, length %zu: %m",
									xlogfname, startoffset, nleft)));
				}
				nleft -= written;
				from += written;
				startoffset += written;
			} while (nleft > 0);

			npages = 0;

			/*
			 * If we just wrote the whole last page of a logfile segment,
			 * fsync the segment immediately.  This avoids having to go back
			 * and re-open prior segments when an fsync request comes along
			 * later. Doing it here ensures that one and only one backend will
			 * perform this fsync.
			 *
			 * This is also the right place to notify the Archiver that the
			 * segment is ready to copy to archival storage, and to update the
			 * timer for archive_timeout, and to signal for a checkpoint if
			 * too many logfile segments have been used since the last
			 * checkpoint.
			 */
			if (finishing_seg)
			{
				issue_xlog_fsync(openLogFile, openLogSegNo);

				/* signal that we need to wakeup walsenders later */
				WalSndWakeupRequest();

				LogwrtResult.Flush = LogwrtResult.Write;	/* end of page */

				if (XLogArchivingActive())
					XLogArchiveNotifySeg(openLogSegNo);

				XLogCtl->lastSegSwitchTime = (pg_time_t) time(NULL);
				XLogCtl->lastSegSwitchLSN = LogwrtResult.Flush;

				/*
				 * Request a checkpoint if we've consumed too much xlog since
				 * the last one.  For speed, we first check using the local
				 * copy of RedoRecPtr, which might be out of date; if it looks
				 * like a checkpoint is needed, forcibly update RedoRecPtr and
				 * recheck.
				 */
				if (IsUnderPostmaster && XLogCheckpointNeeded(openLogSegNo))
				{
					(void) GetRedoRecPtr();
					if (XLogCheckpointNeeded(openLogSegNo))
						RequestCheckpoint(CHECKPOINT_CAUSE_XLOG);
				}
			}
		}

		if (ispartialpage)
		{
			/* Only asked to write a partial page */
			LogwrtResult.Write = WriteRqst.Write;
			break;
		}
		curridx = NextBufIdx(curridx);

		/* If flexible, break out of loop as soon as we wrote something */
		if (flexible && npages == 0)
			break;
	}

	Assert(npages == 0);

	/*
	 * If asked to flush, do so
	 */
	if (LogwrtResult.Flush < WriteRqst.Flush &&
		LogwrtResult.Flush < LogwrtResult.Write)

	{
		/*
		 * Could get here without iterating above loop, in which case we might
		 * have no open file or the wrong one.  However, we do not need to
		 * fsync more than one file.
		 */
		if (sync_method != SYNC_METHOD_OPEN &&
			sync_method != SYNC_METHOD_OPEN_DSYNC)
		{
			if (openLogFile >= 0 &&
				!XLByteInPrevSeg(LogwrtResult.Write, openLogSegNo,
								 wal_segment_size))
				XLogFileClose();
			if (openLogFile < 0)
			{
				XLByteToPrevSeg(LogwrtResult.Write, openLogSegNo,
								wal_segment_size);
				openLogFile = XLogFileOpen(openLogSegNo);
				ReserveExternalFD();
			}

			issue_xlog_fsync(openLogFile, openLogSegNo);
		}

		/* signal that we need to wakeup walsenders later */
		WalSndWakeupRequest();

		LogwrtResult.Flush = LogwrtResult.Write;
	}

	/*
	 * Update shared-memory status
	 *
	 * We make sure that the shared 'request' values do not fall behind the
	 * 'result' values.  This is not absolutely essential, but it saves some
	 * code in a couple of places.
	 */
	{
		SpinLockAcquire(&XLogCtl->info_lck);
		XLogCtl->LogwrtResult = LogwrtResult;
		if (XLogCtl->LogwrtRqst.Write < LogwrtResult.Write)
			XLogCtl->LogwrtRqst.Write = LogwrtResult.Write;
		if (XLogCtl->LogwrtRqst.Flush < LogwrtResult.Flush)
			XLogCtl->LogwrtRqst.Flush = LogwrtResult.Flush;
		SpinLockRelease(&XLogCtl->info_lck);
	}
}

/*
 * Record the LSN for an asynchronous transaction commit/abort
 * and nudge the WALWriter if there is work for it to do.
 * (This should not be called for synchronous commits.)
 */
void
XLogSetAsyncXactLSN(XLogRecPtr asyncXactLSN)
{
	XLogRecPtr	WriteRqstPtr = asyncXactLSN;
	bool		sleeping;

	SpinLockAcquire(&XLogCtl->info_lck);
	LogwrtResult = XLogCtl->LogwrtResult;
	sleeping = XLogCtl->WalWriterSleeping;
	if (XLogCtl->asyncXactLSN < asyncXactLSN)
		XLogCtl->asyncXactLSN = asyncXactLSN;
	SpinLockRelease(&XLogCtl->info_lck);

	/*
	 * If the WALWriter is sleeping, we should kick it to make it come out of
	 * low-power mode.  Otherwise, determine whether there's a full page of
	 * WAL available to write.
	 */
	if (!sleeping)
	{
		/* back off to last completed page boundary */
		WriteRqstPtr -= WriteRqstPtr % XLOG_BLCKSZ;

		/* if we have already flushed that far, we're done */
		if (WriteRqstPtr <= LogwrtResult.Flush)
			return;
	}

	/*
	 * Nudge the WALWriter: it has a full page of WAL to write, or we want it
	 * to come out of low-power mode so that this async commit will reach disk
	 * within the expected amount of time.
	 */
	if (ProcGlobal->walwriterLatch)
		SetLatch(ProcGlobal->walwriterLatch);
}

/*
 * Record the LSN up to which we can remove WAL because it's not required by
 * any replication slot.
 */
void
XLogSetReplicationSlotMinimumLSN(XLogRecPtr lsn)
{
	SpinLockAcquire(&XLogCtl->info_lck);
	XLogCtl->replicationSlotMinLSN = lsn;
	SpinLockRelease(&XLogCtl->info_lck);
}


/*
 * Return the oldest LSN we must retain to satisfy the needs of some
 * replication slot.
 */
static XLogRecPtr
XLogGetReplicationSlotMinimumLSN(void)
{
	XLogRecPtr	retval;

	SpinLockAcquire(&XLogCtl->info_lck);
	retval = XLogCtl->replicationSlotMinLSN;
	SpinLockRelease(&XLogCtl->info_lck);

	return retval;
}

/*
 * Advance minRecoveryPoint in control file.
 *
 * If we crash during recovery, we must reach this point again before the
 * database is consistent.
 *
 * If 'force' is true, 'lsn' argument is ignored. Otherwise, minRecoveryPoint
 * is only updated if it's not already greater than or equal to 'lsn'.
 */
static void
UpdateMinRecoveryPoint(XLogRecPtr lsn, bool force)
{
	/* Quick check using our local copy of the variable */
	if (!updateMinRecoveryPoint || (!force && lsn <= minRecoveryPoint))
		return;

	/*
	 * An invalid minRecoveryPoint means that we need to recover all the WAL,
	 * i.e., we're doing crash recovery.  We never modify the control file's
	 * value in that case, so we can short-circuit future checks here too. The
	 * local values of minRecoveryPoint and minRecoveryPointTLI should not be
	 * updated until crash recovery finishes.  We only do this for the startup
	 * process as it should not update its own reference of minRecoveryPoint
	 * until it has finished crash recovery to make sure that all WAL
	 * available is replayed in this case.  This also saves from extra locks
	 * taken on the control file from the startup process.
	 */
	if (XLogRecPtrIsInvalid(minRecoveryPoint) && InRecovery)
	{
		updateMinRecoveryPoint = false;
		return;
	}

	LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);

	/* update local copy */
	minRecoveryPoint = ControlFile->minRecoveryPoint;
	minRecoveryPointTLI = ControlFile->minRecoveryPointTLI;

	if (XLogRecPtrIsInvalid(minRecoveryPoint))
		updateMinRecoveryPoint = false;
	else if (force || minRecoveryPoint < lsn)
	{
		XLogRecPtr	newMinRecoveryPoint;
		TimeLineID	newMinRecoveryPointTLI;

		/*
		 * To avoid having to update the control file too often, we update it
		 * all the way to the last record being replayed, even though 'lsn'
		 * would suffice for correctness.  This also allows the 'force' case
		 * to not need a valid 'lsn' value.
		 *
		 * Another important reason for doing it this way is that the passed
		 * 'lsn' value could be bogus, i.e., past the end of available WAL, if
		 * the caller got it from a corrupted heap page.  Accepting such a
		 * value as the min recovery point would prevent us from coming up at
		 * all.  Instead, we just log a warning and continue with recovery.
		 * (See also the comments about corrupt LSNs in XLogFlush.)
		 */
		SpinLockAcquire(&XLogCtl->info_lck);
		newMinRecoveryPoint = XLogCtl->replayEndRecPtr;
		newMinRecoveryPointTLI = XLogCtl->replayEndTLI;
		SpinLockRelease(&XLogCtl->info_lck);

		if (!force && newMinRecoveryPoint < lsn)
			elog(WARNING,
				 "xlog min recovery request %X/%X is past current point %X/%X",
				 (uint32) (lsn >> 32), (uint32) lsn,
				 (uint32) (newMinRecoveryPoint >> 32),
				 (uint32) newMinRecoveryPoint);

		/* update control file */
		if (ControlFile->minRecoveryPoint < newMinRecoveryPoint)
		{
			ControlFile->minRecoveryPoint = newMinRecoveryPoint;
			ControlFile->minRecoveryPointTLI = newMinRecoveryPointTLI;
			UpdateControlFile();
			minRecoveryPoint = newMinRecoveryPoint;
			minRecoveryPointTLI = newMinRecoveryPointTLI;

			ereport(DEBUG2,
					(errmsg("updated min recovery point to %X/%X on timeline %u",
							(uint32) (minRecoveryPoint >> 32),
							(uint32) minRecoveryPoint,
							newMinRecoveryPointTLI)));
		}
	}
	LWLockRelease(ControlFileLock);
}

/*
 * Ensure that all XLOG data through the given position is flushed to disk.
 *
 * NOTE: this differs from XLogWrite mainly in that the WALWriteLock is not
 * already held, and we try to avoid acquiring it if possible.
 */
void
XLogFlush(XLogRecPtr record)
{
	XLogRecPtr	WriteRqstPtr;
	XLogwrtRqst WriteRqst;

	/*
	 * During REDO, we are reading not writing WAL.  Therefore, instead of
	 * trying to flush the WAL, we should update minRecoveryPoint instead. We
	 * test XLogInsertAllowed(), not InRecovery, because we need checkpointer
	 * to act this way too, and because when it tries to write the
	 * end-of-recovery checkpoint, it should indeed flush.
	 */
	if (!XLogInsertAllowed())
	{
		UpdateMinRecoveryPoint(record, false);
		return;
	}

	/* Quick exit if already known flushed */
	if (record <= LogwrtResult.Flush)
		return;

#ifdef WAL_DEBUG
	if (XLOG_DEBUG)
		elog(LOG, "xlog flush request %X/%X; write %X/%X; flush %X/%X",
			 (uint32) (record >> 32), (uint32) record,
			 (uint32) (LogwrtResult.Write >> 32), (uint32) LogwrtResult.Write,
			 (uint32) (LogwrtResult.Flush >> 32), (uint32) LogwrtResult.Flush);
#endif

	START_CRIT_SECTION();

	/*
	 * Since fsync is usually a horribly expensive operation, we try to
	 * piggyback as much data as we can on each fsync: if we see any more data
	 * entered into the xlog buffer, we'll write and fsync that too, so that
	 * the final value of LogwrtResult.Flush is as large as possible. This
	 * gives us some chance of avoiding another fsync immediately after.
	 */

	/* initialize to given target; may increase below */
	WriteRqstPtr = record;

	/*
	 * Now wait until we get the write lock, or someone else does the flush
	 * for us.
	 */
	for (;;)
	{
		XLogRecPtr	insertpos;

		/* read LogwrtResult and update local state */
		SpinLockAcquire(&XLogCtl->info_lck);
		if (WriteRqstPtr < XLogCtl->LogwrtRqst.Write)
			WriteRqstPtr = XLogCtl->LogwrtRqst.Write;
		LogwrtResult = XLogCtl->LogwrtResult;
		SpinLockRelease(&XLogCtl->info_lck);

		/* done already? */
		if (record <= LogwrtResult.Flush)
			break;

		/*
		 * Before actually performing the write, wait for all in-flight
		 * insertions to the pages we're about to write to finish.
		 */
		insertpos = WaitXLogInsertionsToFinish(WriteRqstPtr);

		/*
		 * Try to get the write lock. If we can't get it immediately, wait
		 * until it's released, and recheck if we still need to do the flush
		 * or if the backend that held the lock did it for us already. This
		 * helps to maintain a good rate of group committing when the system
		 * is bottlenecked by the speed of fsyncing.
		 */
		if (!LWLockAcquireOrWait(WALWriteLock, LW_EXCLUSIVE))
		{
			/*
			 * The lock is now free, but we didn't acquire it yet. Before we
			 * do, loop back to check if someone else flushed the record for
			 * us already.
			 */
			continue;
		}

		/* Got the lock; recheck whether request is satisfied */
		LogwrtResult = XLogCtl->LogwrtResult;
		if (record <= LogwrtResult.Flush)
		{
			LWLockRelease(WALWriteLock);
			break;
		}

		/*
		 * Sleep before flush! By adding a delay here, we may give further
		 * backends the opportunity to join the backlog of group commit
		 * followers; this can significantly improve transaction throughput,
		 * at the risk of increasing transaction latency.
		 *
		 * We do not sleep if enableFsync is not turned on, nor if there are
		 * fewer than CommitSiblings other backends with active transactions.
		 */
		if (CommitDelay > 0 && enableFsync &&
			MinimumActiveBackends(CommitSiblings))
		{
			pg_usleep(CommitDelay);

			/*
			 * Re-check how far we can now flush the WAL. It's generally not
			 * safe to call WaitXLogInsertionsToFinish while holding
			 * WALWriteLock, because an in-progress insertion might need to
			 * also grab WALWriteLock to make progress. But we know that all
			 * the insertions up to insertpos have already finished, because
			 * that's what the earlier WaitXLogInsertionsToFinish() returned.
			 * We're only calling it again to allow insertpos to be moved
			 * further forward, not to actually wait for anyone.
			 */
			insertpos = WaitXLogInsertionsToFinish(insertpos);
		}

		/* try to write/flush later additions to XLOG as well */
		WriteRqst.Write = insertpos;
		WriteRqst.Flush = insertpos;

		XLogWrite(WriteRqst, false);

		LWLockRelease(WALWriteLock);
		/* done */
		break;
	}

	END_CRIT_SECTION();

	/* wake up walsenders now that we've released heavily contended locks */
	WalSndWakeupProcessRequests();

	/*
	 * If we still haven't flushed to the request point then we have a
	 * problem; most likely, the requested flush point is past end of XLOG.
	 * This has been seen to occur when a disk page has a corrupted LSN.
	 *
	 * Formerly we treated this as a PANIC condition, but that hurts the
	 * system's robustness rather than helping it: we do not want to take down
	 * the whole system due to corruption on one data page.  In particular, if
	 * the bad page is encountered again during recovery then we would be
	 * unable to restart the database at all!  (This scenario actually
	 * happened in the field several times with 7.1 releases.)	As of 8.4, bad
	 * LSNs encountered during recovery are UpdateMinRecoveryPoint's problem;
	 * the only time we can reach here during recovery is while flushing the
	 * end-of-recovery checkpoint record, and we don't expect that to have a
	 * bad LSN.
	 *
	 * Note that for calls from xact.c, the ERROR will be promoted to PANIC
	 * since xact.c calls this routine inside a critical section.  However,
	 * calls from bufmgr.c are not within critical sections and so we will not
	 * force a restart for a bad LSN on a data page.
	 */
	if (LogwrtResult.Flush < record)
		elog(ERROR,
			 "xlog flush request %X/%X is not satisfied --- flushed only to %X/%X",
			 (uint32) (record >> 32), (uint32) record,
			 (uint32) (LogwrtResult.Flush >> 32), (uint32) LogwrtResult.Flush);
}

/*
 * Write & flush xlog, but without specifying exactly where to.
 *
 * We normally write only completed blocks; but if there is nothing to do on
 * that basis, we check for unwritten async commits in the current incomplete
 * block, and write through the latest one of those.  Thus, if async commits
 * are not being used, we will write complete blocks only.
 *
 * If, based on the above, there's anything to write we do so immediately. But
 * to avoid calling fsync, fdatasync et. al. at a rate that'd impact
 * concurrent IO, we only flush WAL every wal_writer_delay ms, or if there's
 * more than wal_writer_flush_after unflushed blocks.
 *
 * We can guarantee that async commits reach disk after at most three
 * wal_writer_delay cycles. (When flushing complete blocks, we allow XLogWrite
 * to write "flexibly", meaning it can stop at the end of the buffer ring;
 * this makes a difference only with very high load or long wal_writer_delay,
 * but imposes one extra cycle for the worst case for async commits.)
 *
 * This routine is invoked periodically by the background walwriter process.
 *
 * Returns true if there was any work to do, even if we skipped flushing due
 * to wal_writer_delay/wal_writer_flush_after.
 */
bool
XLogBackgroundFlush(void)
{
	XLogwrtRqst WriteRqst;
	bool		flexible = true;
	static TimestampTz lastflush;
	TimestampTz now;
	int			flushbytes;

	/* XLOG doesn't need flushing during recovery */
	if (RecoveryInProgress())
		return false;

	/* read LogwrtResult and update local state */
	SpinLockAcquire(&XLogCtl->info_lck);
	LogwrtResult = XLogCtl->LogwrtResult;
	WriteRqst = XLogCtl->LogwrtRqst;
	SpinLockRelease(&XLogCtl->info_lck);

	/* back off to last completed page boundary */
	WriteRqst.Write -= WriteRqst.Write % XLOG_BLCKSZ;

	/* if we have already flushed that far, consider async commit records */
	if (WriteRqst.Write <= LogwrtResult.Flush)
	{
		SpinLockAcquire(&XLogCtl->info_lck);
		WriteRqst.Write = XLogCtl->asyncXactLSN;
		SpinLockRelease(&XLogCtl->info_lck);
		flexible = false;		/* ensure it all gets written */
	}

	/*
	 * If already known flushed, we're done. Just need to check if we are
	 * holding an open file handle to a logfile that's no longer in use,
	 * preventing the file from being deleted.
	 */
	if (WriteRqst.Write <= LogwrtResult.Flush)
	{
		if (openLogFile >= 0)
		{
			if (!XLByteInPrevSeg(LogwrtResult.Write, openLogSegNo,
								 wal_segment_size))
			{
				XLogFileClose();
			}
		}
		return false;
	}

	/*
	 * Determine how far to flush WAL, based on the wal_writer_delay and
	 * wal_writer_flush_after GUCs.
	 */
	now = GetCurrentTimestamp();
	flushbytes =
		WriteRqst.Write / XLOG_BLCKSZ - LogwrtResult.Flush / XLOG_BLCKSZ;

	if (WalWriterFlushAfter == 0 || lastflush == 0)
	{
		/* first call, or block based limits disabled */
		WriteRqst.Flush = WriteRqst.Write;
		lastflush = now;
	}
	else if (TimestampDifferenceExceeds(lastflush, now, WalWriterDelay))
	{
		/*
		 * Flush the writes at least every WalWriterDelay ms. This is
		 * important to bound the amount of time it takes for an asynchronous
		 * commit to hit disk.
		 */
		WriteRqst.Flush = WriteRqst.Write;
		lastflush = now;
	}
	else if (flushbytes >= WalWriterFlushAfter)
	{
		/* exceeded wal_writer_flush_after blocks, flush */
		WriteRqst.Flush = WriteRqst.Write;
		lastflush = now;
	}
	else
	{
		/* no flushing, this time round */
		WriteRqst.Flush = 0;
	}

#ifdef WAL_DEBUG
	if (XLOG_DEBUG)
		elog(LOG, "xlog bg flush request write %X/%X; flush: %X/%X, current is write %X/%X; flush %X/%X",
			 (uint32) (WriteRqst.Write >> 32), (uint32) WriteRqst.Write,
			 (uint32) (WriteRqst.Flush >> 32), (uint32) WriteRqst.Flush,
			 (uint32) (LogwrtResult.Write >> 32), (uint32) LogwrtResult.Write,
			 (uint32) (LogwrtResult.Flush >> 32), (uint32) LogwrtResult.Flush);
#endif

	START_CRIT_SECTION();

	/* now wait for any in-progress insertions to finish and get write lock */
	WaitXLogInsertionsToFinish(WriteRqst.Write);
	LWLockAcquire(WALWriteLock, LW_EXCLUSIVE);
	LogwrtResult = XLogCtl->LogwrtResult;
	if (WriteRqst.Write > LogwrtResult.Write ||
		WriteRqst.Flush > LogwrtResult.Flush)
	{
		XLogWrite(WriteRqst, flexible);
	}
	LWLockRelease(WALWriteLock);

	END_CRIT_SECTION();

	/* wake up walsenders now that we've released heavily contended locks */
	WalSndWakeupProcessRequests();

	/*
	 * Great, done. To take some work off the critical path, try to initialize
	 * as many of the no-longer-needed WAL buffers for future use as we can.
	 */
	AdvanceXLInsertBuffer(InvalidXLogRecPtr, true);

	/*
	 * If we determined that we need to write data, but somebody else
	 * wrote/flushed already, it should be considered as being active, to
	 * avoid hibernating too early.
	 */
	return true;
}

/*
 * Test whether XLOG data has been flushed up to (at least) the given position.
 *
 * Returns true if a flush is still needed.  (It may be that someone else
 * is already in process of flushing that far, however.)
 */
bool
XLogNeedsFlush(XLogRecPtr record)
{
	/*
	 * During recovery, we don't flush WAL but update minRecoveryPoint
	 * instead. So "needs flush" is taken to mean whether minRecoveryPoint
	 * would need to be updated.
	 */
	if (RecoveryInProgress())
	{
		/*
		 * An invalid minRecoveryPoint means that we need to recover all the
		 * WAL, i.e., we're doing crash recovery.  We never modify the control
		 * file's value in that case, so we can short-circuit future checks
		 * here too.  This triggers a quick exit path for the startup process,
		 * which cannot update its local copy of minRecoveryPoint as long as
		 * it has not replayed all WAL available when doing crash recovery.
		 */
		if (XLogRecPtrIsInvalid(minRecoveryPoint) && InRecovery)
			updateMinRecoveryPoint = false;

		/* Quick exit if already known to be updated or cannot be updated */
		if (record <= minRecoveryPoint || !updateMinRecoveryPoint)
			return false;

		/*
		 * Update local copy of minRecoveryPoint. But if the lock is busy,
		 * just return a conservative guess.
		 */
		if (!LWLockConditionalAcquire(ControlFileLock, LW_SHARED))
			return true;
		minRecoveryPoint = ControlFile->minRecoveryPoint;
		minRecoveryPointTLI = ControlFile->minRecoveryPointTLI;
		LWLockRelease(ControlFileLock);

		/*
		 * Check minRecoveryPoint for any other process than the startup
		 * process doing crash recovery, which should not update the control
		 * file value if crash recovery is still running.
		 */
		if (XLogRecPtrIsInvalid(minRecoveryPoint))
			updateMinRecoveryPoint = false;

		/* check again */
		if (record <= minRecoveryPoint || !updateMinRecoveryPoint)
			return false;
		else
			return true;
	}

	/* Quick exit if already known flushed */
	if (record <= LogwrtResult.Flush)
		return false;

	/* read LogwrtResult and update local state */
	SpinLockAcquire(&XLogCtl->info_lck);
	LogwrtResult = XLogCtl->LogwrtResult;
	SpinLockRelease(&XLogCtl->info_lck);

	/* check again */
	if (record <= LogwrtResult.Flush)
		return false;

	return true;
}

/*
 * Create a new XLOG file segment, or open a pre-existing one.
 *
 * logsegno: identify segment to be created/opened.
 *
 * *use_existent: if true, OK to use a pre-existing file (else, any
 * pre-existing file will be deleted).  On return, true if a pre-existing
 * file was used.
 *
 * use_lock: if true, acquire ControlFileLock while moving file into
 * place.  This should be true except during bootstrap log creation.  The
 * caller must *not* hold the lock at call.
 *
 * Returns FD of opened file.
 *
 * Note: errors here are ERROR not PANIC because we might or might not be
 * inside a critical section (eg, during checkpoint there is no reason to
 * take down the system on failure).  They will promote to PANIC if we are
 * in a critical section.
 */
int
XLogFileInit(XLogSegNo logsegno, bool *use_existent, bool use_lock)
{
	char		path[MAXPGPATH];
	char		tmppath[MAXPGPATH];
	PGAlignedXLogBlock zbuffer;
	XLogSegNo	installed_segno;
	XLogSegNo	max_segno;
	int			fd;
	int			nbytes;
	int			save_errno;

	XLogFilePath(path, ThisTimeLineID, logsegno, wal_segment_size);

	/*
	 * Try to use existent file (checkpoint maker may have created it already)
	 */
	if (*use_existent)
	{
		fd = BasicOpenFile(path, O_RDWR | PG_BINARY | get_sync_bit(sync_method));
		if (fd < 0)
		{
			if (errno != ENOENT)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\": %m", path)));
		}
		else
			return fd;
	}

	/*
	 * Initialize an empty (all zeroes) segment.  NOTE: it is possible that
	 * another process is doing the same thing.  If so, we will end up
	 * pre-creating an extra log segment.  That seems OK, and better than
	 * holding the lock throughout this lengthy process.
	 */
	elog(DEBUG2, "creating and filling new WAL file");

	snprintf(tmppath, MAXPGPATH, XLOGDIR "/xlogtemp.%d", (int) getpid());

	unlink(tmppath);

	/* do not use get_sync_bit() here --- want to fsync only at end of fill */
	fd = BasicOpenFile(tmppath, O_RDWR | O_CREAT | O_EXCL | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", tmppath)));

	memset(zbuffer.data, 0, XLOG_BLCKSZ);

	pgstat_report_wait_start(WAIT_EVENT_WAL_INIT_WRITE);
	save_errno = 0;
	if (wal_init_zero)
	{
		/*
		 * Zero-fill the file.  With this setting, we do this the hard way to
		 * ensure that all the file space has really been allocated.  On
		 * platforms that allow "holes" in files, just seeking to the end
		 * doesn't allocate intermediate space.  This way, we know that we
		 * have all the space and (after the fsync below) that all the
		 * indirect blocks are down on disk.  Therefore, fdatasync(2) or
		 * O_DSYNC will be sufficient to sync future writes to the log file.
		 */
		for (nbytes = 0; nbytes < wal_segment_size; nbytes += XLOG_BLCKSZ)
		{
			errno = 0;
			if (write(fd, zbuffer.data, XLOG_BLCKSZ) != XLOG_BLCKSZ)
			{
				/* if write didn't set errno, assume no disk space */
				save_errno = errno ? errno : ENOSPC;
				break;
			}
		}
	}
	else
	{
		/*
		 * Otherwise, seeking to the end and writing a solitary byte is
		 * enough.
		 */
		errno = 0;
		if (pg_pwrite(fd, zbuffer.data, 1, wal_segment_size - 1) != 1)
		{
			/* if write didn't set errno, assume no disk space */
			save_errno = errno ? errno : ENOSPC;
		}
	}
	pgstat_report_wait_end();

	if (save_errno)
	{
		/*
		 * If we fail to make the file, delete it to release disk space
		 */
		unlink(tmppath);

		close(fd);

		errno = save_errno;

		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", tmppath)));
	}

	pgstat_report_wait_start(WAIT_EVENT_WAL_INIT_SYNC);
	if (pg_fsync(fd) != 0)
	{
		int			save_errno = errno;

		close(fd);
		errno = save_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", tmppath)));
	}
	pgstat_report_wait_end();

	if (close(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", tmppath)));

	/*
	 * Now move the segment into place with its final name.
	 *
	 * If caller didn't want to use a pre-existing file, get rid of any
	 * pre-existing file.  Otherwise, cope with possibility that someone else
	 * has created the file while we were filling ours: if so, use ours to
	 * pre-create a future log segment.
	 */
	installed_segno = logsegno;

	/*
	 * XXX: What should we use as max_segno? We used to use XLOGfileslop when
	 * that was a constant, but that was always a bit dubious: normally, at a
	 * checkpoint, XLOGfileslop was the offset from the checkpoint record, but
	 * here, it was the offset from the insert location. We can't do the
	 * normal XLOGfileslop calculation here because we don't have access to
	 * the prior checkpoint's redo location. So somewhat arbitrarily, just use
	 * CheckPointSegments.
	 */
	max_segno = logsegno + CheckPointSegments;
	if (!InstallXLogFileSegment(&installed_segno, tmppath,
								*use_existent, max_segno,
								use_lock))
	{
		/*
		 * No need for any more future segments, or InstallXLogFileSegment()
		 * failed to rename the file into place. If the rename failed, opening
		 * the file below will fail.
		 */
		unlink(tmppath);
	}

	/* Set flag to tell caller there was no existent file */
	*use_existent = false;

	/* Now open original target segment (might not be file I just made) */
	fd = BasicOpenFile(path, O_RDWR | PG_BINARY | get_sync_bit(sync_method));
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path),
				 (AmCheckpointerProcess() ?
				  errhint("This is known to fail occasionally during archive recovery, where it is harmless.") :
				  0)));

	elog(DEBUG2, "done creating and filling new WAL file");

	return fd;
}

/*
 * Create a new XLOG file segment by copying a pre-existing one.
 *
 * destsegno: identify segment to be created.
 *
 * srcTLI, srcsegno: identify segment to be copied (could be from
 *		a different timeline)
 *
 * upto: how much of the source file to copy (the rest is filled with
 *		zeros)
 *
 * Currently this is only used during recovery, and so there are no locking
 * considerations.  But we should be just as tense as XLogFileInit to avoid
 * emplacing a bogus file.
 */
static void
XLogFileCopy(XLogSegNo destsegno, TimeLineID srcTLI, XLogSegNo srcsegno,
			 int upto)
{
	char		path[MAXPGPATH];
	char		tmppath[MAXPGPATH];
	PGAlignedXLogBlock buffer;
	int			srcfd;
	int			fd;
	int			nbytes;

	/*
	 * Open the source file
	 */
	XLogFilePath(path, srcTLI, srcsegno, wal_segment_size);
	srcfd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (srcfd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));

	/*
	 * Copy into a temp file name.
	 */
	snprintf(tmppath, MAXPGPATH, XLOGDIR "/xlogtemp.%d", (int) getpid());

	unlink(tmppath);

	/* do not use get_sync_bit() here --- want to fsync only at end of fill */
	fd = OpenTransientFile(tmppath, O_RDWR | O_CREAT | O_EXCL | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", tmppath)));

	/*
	 * Do the data copying.
	 */
	for (nbytes = 0; nbytes < wal_segment_size; nbytes += sizeof(buffer))
	{
		int			nread;

		nread = upto - nbytes;

		/*
		 * The part that is not read from the source file is filled with
		 * zeros.
		 */
		if (nread < sizeof(buffer))
			memset(buffer.data, 0, sizeof(buffer));

		if (nread > 0)
		{
			int			r;

			if (nread > sizeof(buffer))
				nread = sizeof(buffer);
			pgstat_report_wait_start(WAIT_EVENT_WAL_COPY_READ);
			r = read(srcfd, buffer.data, nread);
			if (r != nread)
			{
				if (r < 0)
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not read file \"%s\": %m",
									path)));
				else
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("could not read file \"%s\": read %d of %zu",
									path, r, (Size) nread)));
			}
			pgstat_report_wait_end();
		}
		errno = 0;
		pgstat_report_wait_start(WAIT_EVENT_WAL_COPY_WRITE);
		if ((int) write(fd, buffer.data, sizeof(buffer)) != (int) sizeof(buffer))
		{
			int			save_errno = errno;

			/*
			 * If we fail to make the file, delete it to release disk space
			 */
			unlink(tmppath);
			/* if write didn't set errno, assume problem is no disk space */
			errno = save_errno ? save_errno : ENOSPC;

			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\": %m", tmppath)));
		}
		pgstat_report_wait_end();
	}

	pgstat_report_wait_start(WAIT_EVENT_WAL_COPY_SYNC);
	if (pg_fsync(fd) != 0)
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", tmppath)));
	pgstat_report_wait_end();

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", tmppath)));

	if (CloseTransientFile(srcfd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));

	/*
	 * Now move the segment into place with its final name.
	 */
	if (!InstallXLogFileSegment(&destsegno, tmppath, false, 0, false))
		elog(ERROR, "InstallXLogFileSegment should not have failed");
}

/*
 * Install a new XLOG segment file as a current or future log segment.
 *
 * This is used both to install a newly-created segment (which has a temp
 * filename while it's being created) and to recycle an old segment.
 *
 * *segno: identify segment to install as (or first possible target).
 * When find_free is true, this is modified on return to indicate the
 * actual installation location or last segment searched.
 *
 * tmppath: initial name of file to install.  It will be renamed into place.
 *
 * find_free: if true, install the new segment at the first empty segno
 * number at or after the passed numbers.  If false, install the new segment
 * exactly where specified, deleting any existing segment file there.
 *
 * max_segno: maximum segment number to install the new file as.  Fail if no
 * free slot is found between *segno and max_segno. (Ignored when find_free
 * is false.)
 *
 * use_lock: if true, acquire ControlFileLock while moving file into
 * place.  This should be true except during bootstrap log creation.  The
 * caller must *not* hold the lock at call.
 *
 * Returns true if the file was installed successfully.  false indicates that
 * max_segno limit was exceeded, or an error occurred while renaming the
 * file into place.
 */
static bool
InstallXLogFileSegment(XLogSegNo *segno, char *tmppath,
					   bool find_free, XLogSegNo max_segno,
					   bool use_lock)
{
	char		path[MAXPGPATH];
	struct stat stat_buf;

	XLogFilePath(path, ThisTimeLineID, *segno, wal_segment_size);

	/*
	 * We want to be sure that only one process does this at a time.
	 */
	if (use_lock)
		LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);

	if (!find_free)
	{
		/* Force installation: get rid of any pre-existing segment file */
		durable_unlink(path, DEBUG1);
	}
	else
	{
		/* Find a free slot to put it in */
		while (stat(path, &stat_buf) == 0)
		{
			if ((*segno) >= max_segno)
			{
				/* Failed to find a free slot within specified range */
				if (use_lock)
					LWLockRelease(ControlFileLock);
				return false;
			}
			(*segno)++;
			XLogFilePath(path, ThisTimeLineID, *segno, wal_segment_size);
		}
	}

	/*
	 * Perform the rename using link if available, paranoidly trying to avoid
	 * overwriting an existing file (there shouldn't be one).
	 */
	if (durable_rename_excl(tmppath, path, LOG) != 0)
	{
		if (use_lock)
			LWLockRelease(ControlFileLock);
		/* durable_rename_excl already emitted log message */
		return false;
	}

	if (use_lock)
		LWLockRelease(ControlFileLock);

	return true;
}

/*
 * Open a pre-existing logfile segment for writing.
 */
int
XLogFileOpen(XLogSegNo segno)
{
	char		path[MAXPGPATH];
	int			fd;

	XLogFilePath(path, ThisTimeLineID, segno, wal_segment_size);

	fd = BasicOpenFile(path, O_RDWR | PG_BINARY | get_sync_bit(sync_method));
	if (fd < 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));

	return fd;
}

/*
 * Open a logfile segment for reading (during recovery).
 *
 * If source == XLOG_FROM_ARCHIVE, the segment is retrieved from archive.
 * Otherwise, it's assumed to be already available in pg_wal.
 */
static int
XLogFileRead(XLogSegNo segno, int emode, TimeLineID tli,
			 XLogSource source, bool notfoundOk)
{
	char		xlogfname[MAXFNAMELEN];
	char		activitymsg[MAXFNAMELEN + 16];
	char		path[MAXPGPATH];
	int			fd;

	XLogFileName(xlogfname, tli, segno, wal_segment_size);

	switch (source)
	{
		case XLOG_FROM_ARCHIVE:
			/* Report recovery progress in PS display */
			snprintf(activitymsg, sizeof(activitymsg), "waiting for %s",
					 xlogfname);
			set_ps_display(activitymsg);

			restoredFromArchive = RestoreArchivedFile(path, xlogfname,
													  "RECOVERYXLOG",
													  wal_segment_size,
													  InRedo);
			if (!restoredFromArchive)
				return -1;
			break;

		case XLOG_FROM_PG_WAL:
		case XLOG_FROM_STREAM:
			XLogFilePath(path, tli, segno, wal_segment_size);
			restoredFromArchive = false;
			break;

		default:
			elog(ERROR, "invalid XLogFileRead source %d", source);
	}

	/*
	 * If the segment was fetched from archival storage, replace the existing
	 * xlog segment (if any) with the archival version.
	 */
	if (source == XLOG_FROM_ARCHIVE)
	{
		KeepFileRestoredFromArchive(path, xlogfname);

		/*
		 * Set path to point at the new file in pg_wal.
		 */
		snprintf(path, MAXPGPATH, XLOGDIR "/%s", xlogfname);
	}

	fd = BasicOpenFile(path, O_RDONLY | PG_BINARY);
	if (fd >= 0)
	{
		/* Success! */
		curFileTLI = tli;

		/* Report recovery progress in PS display */
		snprintf(activitymsg, sizeof(activitymsg), "recovering %s",
				 xlogfname);
		set_ps_display(activitymsg);

		/* Track source of data in assorted state variables */
		readSource = source;
		XLogReceiptSource = source;
		/* In FROM_STREAM case, caller tracks receipt time, not me */
		if (source != XLOG_FROM_STREAM)
			XLogReceiptTime = GetCurrentTimestamp();

		return fd;
	}
	if (errno != ENOENT || !notfoundOk) /* unexpected failure? */
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	return -1;
}

/*
 * Open a logfile segment for reading (during recovery).
 *
 * This version searches for the segment with any TLI listed in expectedTLEs.
 */
static int
XLogFileReadAnyTLI(XLogSegNo segno, int emode, XLogSource source)
{
	char		path[MAXPGPATH];
	ListCell   *cell;
	int			fd;
	List	   *tles;

	/*
	 * Loop looking for a suitable timeline ID: we might need to read any of
	 * the timelines listed in expectedTLEs.
	 *
	 * We expect curFileTLI on entry to be the TLI of the preceding file in
	 * sequence, or 0 if there was no predecessor.  We do not allow curFileTLI
	 * to go backwards; this prevents us from picking up the wrong file when a
	 * parent timeline extends to higher segment numbers than the child we
	 * want to read.
	 *
	 * If we haven't read the timeline history file yet, read it now, so that
	 * we know which TLIs to scan.  We don't save the list in expectedTLEs,
	 * however, unless we actually find a valid segment.  That way if there is
	 * neither a timeline history file nor a WAL segment in the archive, and
	 * streaming replication is set up, we'll read the timeline history file
	 * streamed from the master when we start streaming, instead of recovering
	 * with a dummy history generated here.
	 */
	if (expectedTLEs)
		tles = expectedTLEs;
	else
		tles = readTimeLineHistory(recoveryTargetTLI);

	foreach(cell, tles)
	{
		TimeLineHistoryEntry *hent = (TimeLineHistoryEntry *) lfirst(cell);
		TimeLineID	tli = hent->tli;

		if (tli < curFileTLI)
			break;				/* don't bother looking at too-old TLIs */

		/*
		 * Skip scanning the timeline ID that the logfile segment to read
		 * doesn't belong to
		 */
		if (hent->begin != InvalidXLogRecPtr)
		{
			XLogSegNo	beginseg = 0;

			XLByteToSeg(hent->begin, beginseg, wal_segment_size);

			/*
			 * The logfile segment that doesn't belong to the timeline is
			 * older or newer than the segment that the timeline started or
			 * ended at, respectively. It's sufficient to check only the
			 * starting segment of the timeline here. Since the timelines are
			 * scanned in descending order in this loop, any segments newer
			 * than the ending segment should belong to newer timeline and
			 * have already been read before. So it's not necessary to check
			 * the ending segment of the timeline here.
			 */
			if (segno < beginseg)
				continue;
		}

		if (source == XLOG_FROM_ANY || source == XLOG_FROM_ARCHIVE)
		{
			fd = XLogFileRead(segno, emode, tli,
							  XLOG_FROM_ARCHIVE, true);
			if (fd != -1)
			{
				elog(DEBUG1, "got WAL segment from archive");
				if (!expectedTLEs)
					expectedTLEs = tles;
				return fd;
			}
		}

		if (source == XLOG_FROM_ANY || source == XLOG_FROM_PG_WAL)
		{
			fd = XLogFileRead(segno, emode, tli,
							  XLOG_FROM_PG_WAL, true);
			if (fd != -1)
			{
				if (!expectedTLEs)
					expectedTLEs = tles;
				return fd;
			}
		}
	}

	/* Couldn't find it.  For simplicity, complain about front timeline */
	XLogFilePath(path, recoveryTargetTLI, segno, wal_segment_size);
	errno = ENOENT;
	ereport(emode,
			(errcode_for_file_access(),
			 errmsg("could not open file \"%s\": %m", path)));
	return -1;
}

/*
 * Close the current logfile segment for writing.
 */
static void
XLogFileClose(void)
{
	Assert(openLogFile >= 0);

	/*
	 * WAL segment files will not be re-read in normal operation, so we advise
	 * the OS to release any cached pages.  But do not do so if WAL archiving
	 * or streaming is active, because archiver and walsender process could
	 * use the cache to read the WAL segment.
	 */
#if defined(USE_POSIX_FADVISE) && defined(POSIX_FADV_DONTNEED)
	if (!XLogIsNeeded())
		(void) posix_fadvise(openLogFile, 0, 0, POSIX_FADV_DONTNEED);
#endif

	if (close(openLogFile) != 0)
	{
		char		xlogfname[MAXFNAMELEN];
		int			save_errno = errno;

		XLogFileName(xlogfname, ThisTimeLineID, openLogSegNo, wal_segment_size);
		errno = save_errno;
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", xlogfname)));
	}

	openLogFile = -1;
	ReleaseExternalFD();
}

/*
 * Preallocate log files beyond the specified log endpoint.
 *
 * XXX this is currently extremely conservative, since it forces only one
 * future log segment to exist, and even that only if we are 75% done with
 * the current one.  This is only appropriate for very low-WAL-volume systems.
 * High-volume systems will be OK once they've built up a sufficient set of
 * recycled log segments, but the startup transient is likely to include
 * a lot of segment creations by foreground processes, which is not so good.
 */
static void
PreallocXlogFiles(XLogRecPtr endptr)
{
	XLogSegNo	_logSegNo;
	int			lf;
	bool		use_existent;
	uint64		offset;

	XLByteToPrevSeg(endptr, _logSegNo, wal_segment_size);
	offset = XLogSegmentOffset(endptr - 1, wal_segment_size);
	if (offset >= (uint32) (0.75 * wal_segment_size))
	{
		_logSegNo++;
		use_existent = true;
		lf = XLogFileInit(_logSegNo, &use_existent, true);
		close(lf);
		if (!use_existent)
			CheckpointStats.ckpt_segs_added++;
	}
}

/*
 * Throws an error if the given log segment has already been removed or
 * recycled. The caller should only pass a segment that it knows to have
 * existed while the server has been running, as this function always
 * succeeds if no WAL segments have been removed since startup.
 * 'tli' is only used in the error message.
 *
 * Note: this function guarantees to keep errno unchanged on return.
 * This supports callers that use this to possibly deliver a better
 * error message about a missing file, while still being able to throw
 * a normal file-access error afterwards, if this does return.
 */
void
CheckXLogRemoved(XLogSegNo segno, TimeLineID tli)
{
	int			save_errno = errno;
	XLogSegNo	lastRemovedSegNo;

	SpinLockAcquire(&XLogCtl->info_lck);
	lastRemovedSegNo = XLogCtl->lastRemovedSegNo;
	SpinLockRelease(&XLogCtl->info_lck);

	if (segno <= lastRemovedSegNo)
	{
		char		filename[MAXFNAMELEN];

		XLogFileName(filename, tli, segno, wal_segment_size);
		errno = save_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("requested WAL segment %s has already been removed",
						filename)));
	}
	errno = save_errno;
}

/*
 * Return the last WAL segment removed, or 0 if no segment has been removed
 * since startup.
 *
 * NB: the result can be out of date arbitrarily fast, the caller has to deal
 * with that.
 */
XLogSegNo
XLogGetLastRemovedSegno(void)
{
	XLogSegNo	lastRemovedSegNo;

	SpinLockAcquire(&XLogCtl->info_lck);
	lastRemovedSegNo = XLogCtl->lastRemovedSegNo;
	SpinLockRelease(&XLogCtl->info_lck);

	return lastRemovedSegNo;
}


/*
 * Update the last removed segno pointer in shared memory, to reflect that the
 * given XLOG file has been removed.
 */
static void
UpdateLastRemovedPtr(char *filename)
{
	uint32		tli;
	XLogSegNo	segno;

	XLogFromFileName(filename, &tli, &segno, wal_segment_size);

	SpinLockAcquire(&XLogCtl->info_lck);
	if (segno > XLogCtl->lastRemovedSegNo)
		XLogCtl->lastRemovedSegNo = segno;
	SpinLockRelease(&XLogCtl->info_lck);
}

/*
 * Remove all temporary log files in pg_wal
 *
 * This is called at the beginning of recovery after a previous crash,
 * at a point where no other processes write fresh WAL data.
 */
static void
RemoveTempXlogFiles(void)
{
	DIR		   *xldir;
	struct dirent *xlde;

	elog(DEBUG2, "removing all temporary WAL segments");

	xldir = AllocateDir(XLOGDIR);
	while ((xlde = ReadDir(xldir, XLOGDIR)) != NULL)
	{
		char		path[MAXPGPATH];

		if (strncmp(xlde->d_name, "xlogtemp.", 9) != 0)
			continue;

		snprintf(path, MAXPGPATH, XLOGDIR "/%s", xlde->d_name);
		unlink(path);
		elog(DEBUG2, "removed temporary WAL segment \"%s\"", path);
	}
	FreeDir(xldir);
}

/*
 * Recycle or remove all log files older or equal to passed segno.
 *
 * endptr is current (or recent) end of xlog, and lastredoptr is the
 * redo pointer of the last checkpoint. These are used to determine
 * whether we want to recycle rather than delete no-longer-wanted log files.
 */
static void
RemoveOldXlogFiles(XLogSegNo segno, XLogRecPtr lastredoptr, XLogRecPtr endptr)
{
	DIR		   *xldir;
	struct dirent *xlde;
	char		lastoff[MAXFNAMELEN];

	/*
	 * Construct a filename of the last segment to be kept. The timeline ID
	 * doesn't matter, we ignore that in the comparison. (During recovery,
	 * ThisTimeLineID isn't set, so we can't use that.)
	 */
	XLogFileName(lastoff, 0, segno, wal_segment_size);

	elog(DEBUG2, "attempting to remove WAL segments older than log file %s",
		 lastoff);

	xldir = AllocateDir(XLOGDIR);

	while ((xlde = ReadDir(xldir, XLOGDIR)) != NULL)
	{
		/* Ignore files that are not XLOG segments */
		if (!IsXLogFileName(xlde->d_name) &&
			!IsPartialXLogFileName(xlde->d_name))
			continue;

		/*
		 * We ignore the timeline part of the XLOG segment identifiers in
		 * deciding whether a segment is still needed.  This ensures that we
		 * won't prematurely remove a segment from a parent timeline. We could
		 * probably be a little more proactive about removing segments of
		 * non-parent timelines, but that would be a whole lot more
		 * complicated.
		 *
		 * We use the alphanumeric sorting property of the filenames to decide
		 * which ones are earlier than the lastoff segment.
		 */
		if (strcmp(xlde->d_name + 8, lastoff + 8) <= 0)
		{
			if (XLogArchiveCheckDone(xlde->d_name))
			{
				/* Update the last removed location in shared memory first */
				UpdateLastRemovedPtr(xlde->d_name);

				RemoveXlogFile(xlde->d_name, lastredoptr, endptr);
			}
		}
	}

	FreeDir(xldir);
}

/*
 * Remove WAL files that are not part of the given timeline's history.
 *
 * This is called during recovery, whenever we switch to follow a new
 * timeline, and at the end of recovery when we create a new timeline. We
 * wouldn't otherwise care about extra WAL files lying in pg_wal, but they
 * might be leftover pre-allocated or recycled WAL segments on the old timeline
 * that we haven't used yet, and contain garbage. If we just leave them in
 * pg_wal, they will eventually be archived, and we can't let that happen.
 * Files that belong to our timeline history are valid, because we have
 * successfully replayed them, but from others we can't be sure.
 *
 * 'switchpoint' is the current point in WAL where we switch to new timeline,
 * and 'newTLI' is the new timeline we switch to.
 */
static void
RemoveNonParentXlogFiles(XLogRecPtr switchpoint, TimeLineID newTLI)
{
	DIR		   *xldir;
	struct dirent *xlde;
	char		switchseg[MAXFNAMELEN];
	XLogSegNo	endLogSegNo;

	XLByteToPrevSeg(switchpoint, endLogSegNo, wal_segment_size);

	/*
	 * Construct a filename of the last segment to be kept.
	 */
	XLogFileName(switchseg, newTLI, endLogSegNo, wal_segment_size);

	elog(DEBUG2, "attempting to remove WAL segments newer than log file %s",
		 switchseg);

	xldir = AllocateDir(XLOGDIR);

	while ((xlde = ReadDir(xldir, XLOGDIR)) != NULL)
	{
		/* Ignore files that are not XLOG segments */
		if (!IsXLogFileName(xlde->d_name))
			continue;

		/*
		 * Remove files that are on a timeline older than the new one we're
		 * switching to, but with a segment number >= the first segment on the
		 * new timeline.
		 */
		if (strncmp(xlde->d_name, switchseg, 8) < 0 &&
			strcmp(xlde->d_name + 8, switchseg + 8) > 0)
		{
			/*
			 * If the file has already been marked as .ready, however, don't
			 * remove it yet. It should be OK to remove it - files that are
			 * not part of our timeline history are not required for recovery
			 * - but seems safer to let them be archived and removed later.
			 */
			if (!XLogArchiveIsReady(xlde->d_name))
				RemoveXlogFile(xlde->d_name, InvalidXLogRecPtr, switchpoint);
		}
	}

	FreeDir(xldir);
}

/*
 * Recycle or remove a log file that's no longer needed.
 *
 * endptr is current (or recent) end of xlog, and lastredoptr is the
 * redo pointer of the last checkpoint. These are used to determine
 * whether we want to recycle rather than delete no-longer-wanted log files.
 * If lastredoptr is not known, pass invalid, and the function will recycle,
 * somewhat arbitrarily, 10 future segments.
 */
static void
RemoveXlogFile(const char *segname, XLogRecPtr lastredoptr, XLogRecPtr endptr)
{
	char		path[MAXPGPATH];
#ifdef WIN32
	char		newpath[MAXPGPATH];
#endif
	struct stat statbuf;
	XLogSegNo	endlogSegNo;
	XLogSegNo	recycleSegNo;

	if (wal_recycle)
	{
		/*
		 * Initialize info about where to try to recycle to.
		 */
		XLByteToSeg(endptr, endlogSegNo, wal_segment_size);
		if (lastredoptr == InvalidXLogRecPtr)
			recycleSegNo = endlogSegNo + 10;
		else
			recycleSegNo = XLOGfileslop(lastredoptr);
	}
	else
		recycleSegNo = 0;		/* keep compiler quiet */

	snprintf(path, MAXPGPATH, XLOGDIR "/%s", segname);

	/*
	 * Before deleting the file, see if it can be recycled as a future log
	 * segment. Only recycle normal files, pg_standby for example can create
	 * symbolic links pointing to a separate archive directory.
	 */
	if (wal_recycle &&
		endlogSegNo <= recycleSegNo &&
		lstat(path, &statbuf) == 0 && S_ISREG(statbuf.st_mode) &&
		InstallXLogFileSegment(&endlogSegNo, path,
							   true, recycleSegNo, true))
	{
		ereport(DEBUG2,
				(errmsg("recycled write-ahead log file \"%s\"",
						segname)));
		CheckpointStats.ckpt_segs_recycled++;
		/* Needn't recheck that slot on future iterations */
		endlogSegNo++;
	}
	else
	{
		/* No need for any more future segments... */
		int			rc;

		ereport(DEBUG2,
				(errmsg("removing write-ahead log file \"%s\"",
						segname)));

#ifdef WIN32

		/*
		 * On Windows, if another process (e.g another backend) holds the file
		 * open in FILE_SHARE_DELETE mode, unlink will succeed, but the file
		 * will still show up in directory listing until the last handle is
		 * closed. To avoid confusing the lingering deleted file for a live
		 * WAL file that needs to be archived, rename it before deleting it.
		 *
		 * If another process holds the file open without FILE_SHARE_DELETE
		 * flag, rename will fail. We'll try again at the next checkpoint.
		 */
		snprintf(newpath, MAXPGPATH, "%s.deleted", path);
		if (rename(path, newpath) != 0)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not rename file \"%s\": %m",
							path)));
			return;
		}
		rc = durable_unlink(newpath, LOG);
#else
		rc = durable_unlink(path, LOG);
#endif
		if (rc != 0)
		{
			/* Message already logged by durable_unlink() */
			return;
		}
		CheckpointStats.ckpt_segs_removed++;
	}

	XLogArchiveCleanup(segname);
}

/*
 * Verify whether pg_wal and pg_wal/archive_status exist.
 * If the latter does not exist, recreate it.
 *
 * It is not the goal of this function to verify the contents of these
 * directories, but to help in cases where someone has performed a cluster
 * copy for PITR purposes but omitted pg_wal from the copy.
 *
 * We could also recreate pg_wal if it doesn't exist, but a deliberate
 * policy decision was made not to.  It is fairly common for pg_wal to be
 * a symlink, and if that was the DBA's intent then automatically making a
 * plain directory would result in degraded performance with no notice.
 */
static void
ValidateXLOGDirectoryStructure(void)
{
	char		path[MAXPGPATH];
	struct stat stat_buf;

	/* Check for pg_wal; if it doesn't exist, error out */
	if (stat(XLOGDIR, &stat_buf) != 0 ||
		!S_ISDIR(stat_buf.st_mode))
		ereport(FATAL,
				(errmsg("required WAL directory \"%s\" does not exist",
						XLOGDIR)));

	/* Check for archive_status */
	snprintf(path, MAXPGPATH, XLOGDIR "/archive_status");
	if (stat(path, &stat_buf) == 0)
	{
		/* Check for weird cases where it exists but isn't a directory */
		if (!S_ISDIR(stat_buf.st_mode))
			ereport(FATAL,
					(errmsg("required WAL directory \"%s\" does not exist",
							path)));
	}
	else
	{
		ereport(LOG,
				(errmsg("creating missing WAL directory \"%s\"", path)));
		if (MakePGDirectory(path) < 0)
			ereport(FATAL,
					(errmsg("could not create missing directory \"%s\": %m",
							path)));
	}
}

/*
 * Remove previous backup history files.  This also retries creation of
 * .ready files for any backup history files for which XLogArchiveNotify
 * failed earlier.
 */
static void
CleanupBackupHistory(void)
{
	DIR		   *xldir;
	struct dirent *xlde;
	char		path[MAXPGPATH + sizeof(XLOGDIR)];

	xldir = AllocateDir(XLOGDIR);

	while ((xlde = ReadDir(xldir, XLOGDIR)) != NULL)
	{
		if (IsBackupHistoryFileName(xlde->d_name))
		{
			if (XLogArchiveCheckDone(xlde->d_name))
			{
				elog(DEBUG2, "removing WAL backup history file \"%s\"",
					 xlde->d_name);
				snprintf(path, sizeof(path), XLOGDIR "/%s", xlde->d_name);
				unlink(path);
				XLogArchiveCleanup(xlde->d_name);
			}
		}
	}

	FreeDir(xldir);
}

/*
 * Attempt to read the next XLOG record.
 *
 * Before first call, the reader needs to be positioned to the first record
 * by calling XLogBeginRead().
 *
 * If no valid record is available, returns NULL, or fails if emode is PANIC.
 * (emode must be either PANIC, LOG). In standby mode, retries until a valid
 * record is available.
 */
static XLogRecord *
ReadRecord(XLogReaderState *xlogreader, int emode,
		   bool fetching_ckpt)
{
	XLogRecord *record;
	XLogPageReadPrivate *private = (XLogPageReadPrivate *) xlogreader->private_data;

	/* Pass through parameters to XLogPageRead */
	private->fetching_ckpt = fetching_ckpt;
	private->emode = emode;
	private->randAccess = (xlogreader->ReadRecPtr == InvalidXLogRecPtr);

	/* This is the first attempt to read this page. */
	lastSourceFailed = false;

	for (;;)
	{
		char	   *errormsg;

		record = XLogReadRecord(xlogreader, &errormsg);
		ReadRecPtr = xlogreader->ReadRecPtr;
		EndRecPtr = xlogreader->EndRecPtr;
		if (record == NULL)
		{
			/*
			 * When we find that WAL ends in an incomplete record, keep track
			 * of that record.  After recovery is done, we'll write a record to
			 * indicate to downstream WAL readers that that portion is to be
			 * ignored.
			 *
			 * However, when ArchiveRecoveryRequested = true, we're going to
			 * switch to a new timeline at the end of recovery. We will only
			 * copy WAL over to the new timeline up to the end of the last
			 * complete record, so if we did this, we would later create an
			 * overwrite contrecord in the wrong place, breaking everything.
			 */
			if (!ArchiveRecoveryRequested &&
				!XLogRecPtrIsInvalid(xlogreader->abortedRecPtr))
			{
				abortedRecPtr = xlogreader->abortedRecPtr;
				missingContrecPtr = xlogreader->missingContrecPtr;
			}

			if (readFile >= 0)
			{
				close(readFile);
				readFile = -1;
			}

			/*
			 * We only end up here without a message when XLogPageRead()
			 * failed - in that case we already logged something. In
			 * StandbyMode that only happens if we have been triggered, so we
			 * shouldn't loop anymore in that case.
			 */
			if (errormsg)
				ereport(emode_for_corrupt_record(emode, EndRecPtr),
						(errmsg_internal("%s", errormsg) /* already translated */ ));
		}

		/*
		 * Check page TLI is one of the expected values.
		 */
		else if (!tliInHistory(xlogreader->latestPageTLI, expectedTLEs))
		{
			char		fname[MAXFNAMELEN];
			XLogSegNo	segno;
			int32		offset;

			XLByteToSeg(xlogreader->latestPagePtr, segno, wal_segment_size);
			offset = XLogSegmentOffset(xlogreader->latestPagePtr,
									   wal_segment_size);
			XLogFileName(fname, xlogreader->seg.ws_tli, segno,
						 wal_segment_size);
			ereport(emode_for_corrupt_record(emode, EndRecPtr),
					(errmsg("unexpected timeline ID %u in log segment %s, offset %u",
							xlogreader->latestPageTLI,
							fname,
							offset)));
			record = NULL;
		}

		if (record)
		{
			/* Great, got a record */
			return record;
		}
		else
		{
			/* No valid record available from this source */
			lastSourceFailed = true;

			/*
			 * If archive recovery was requested, but we were still doing
			 * crash recovery, switch to archive recovery and retry using the
			 * offline archive. We have now replayed all the valid WAL in
			 * pg_wal, so we are presumably now consistent.
			 *
			 * We require that there's at least some valid WAL present in
			 * pg_wal, however (!fetching_ckpt).  We could recover using the
			 * WAL from the archive, even if pg_wal is completely empty, but
			 * we'd have no idea how far we'd have to replay to reach
			 * consistency.  So err on the safe side and give up.
			 */
			if (!InArchiveRecovery && ArchiveRecoveryRequested &&
				!fetching_ckpt)
			{
				ereport(DEBUG1,
						(errmsg_internal("reached end of WAL in pg_wal, entering archive recovery")));
				InArchiveRecovery = true;
				if (StandbyModeRequested)
					StandbyMode = true;

				/* initialize minRecoveryPoint to this record */
				LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
				ControlFile->state = DB_IN_ARCHIVE_RECOVERY;
				if (ControlFile->minRecoveryPoint < EndRecPtr)
				{
					ControlFile->minRecoveryPoint = EndRecPtr;
					ControlFile->minRecoveryPointTLI = ThisTimeLineID;
				}
				/* update local copy */
				minRecoveryPoint = ControlFile->minRecoveryPoint;
				minRecoveryPointTLI = ControlFile->minRecoveryPointTLI;

				/*
				 * The startup process can update its local copy of
				 * minRecoveryPoint from this point.
				 */
				updateMinRecoveryPoint = true;

				UpdateControlFile();

				/*
				 * We update SharedRecoveryState while holding the lock on
				 * ControlFileLock so both states are consistent in shared
				 * memory.
				 */
				SpinLockAcquire(&XLogCtl->info_lck);
				XLogCtl->SharedRecoveryState = RECOVERY_STATE_ARCHIVE;
				SpinLockRelease(&XLogCtl->info_lck);

				LWLockRelease(ControlFileLock);

				CheckRecoveryConsistency();

				/*
				 * Before we retry, reset lastSourceFailed and currentSource
				 * so that we will check the archive next.
				 */
				lastSourceFailed = false;
				currentSource = XLOG_FROM_ANY;

				continue;
			}

			/* In standby mode, loop back to retry. Otherwise, give up. */
			if (StandbyMode && !CheckForStandbyTrigger())
				continue;
			else
				return NULL;
		}
	}
}

/*
 * Scan for new timelines that might have appeared in the archive since we
 * started recovery.
 *
 * If there are any, the function changes recovery target TLI to the latest
 * one and returns 'true'.
 */
static bool
rescanLatestTimeLine(void)
{
	List	   *newExpectedTLEs;
	bool		found;
	ListCell   *cell;
	TimeLineID	newtarget;
	TimeLineID	oldtarget = recoveryTargetTLI;
	TimeLineHistoryEntry *currentTle = NULL;

	newtarget = findNewestTimeLine(recoveryTargetTLI);
	if (newtarget == recoveryTargetTLI)
	{
		/* No new timelines found */
		return false;
	}

	/*
	 * Determine the list of expected TLIs for the new TLI
	 */

	newExpectedTLEs = readTimeLineHistory(newtarget);

	/*
	 * If the current timeline is not part of the history of the new timeline,
	 * we cannot proceed to it.
	 */
	found = false;
	foreach(cell, newExpectedTLEs)
	{
		currentTle = (TimeLineHistoryEntry *) lfirst(cell);

		if (currentTle->tli == recoveryTargetTLI)
		{
			found = true;
			break;
		}
	}
	if (!found)
	{
		ereport(LOG,
				(errmsg("new timeline %u is not a child of database system timeline %u",
						newtarget,
						ThisTimeLineID)));
		return false;
	}

	/*
	 * The current timeline was found in the history file, but check that the
	 * next timeline was forked off from it *after* the current recovery
	 * location.
	 */
	if (currentTle->end < EndRecPtr)
	{
		ereport(LOG,
				(errmsg("new timeline %u forked off current database system timeline %u before current recovery point %X/%X",
						newtarget,
						ThisTimeLineID,
						(uint32) (EndRecPtr >> 32), (uint32) EndRecPtr)));
		return false;
	}

	/* The new timeline history seems valid. Switch target */
	recoveryTargetTLI = newtarget;
	list_free_deep(expectedTLEs);
	expectedTLEs = newExpectedTLEs;

	/*
	 * As in StartupXLOG(), try to ensure we have all the history files
	 * between the old target and new target in pg_wal.
	 */
	restoreTimeLineHistoryFiles(oldtarget + 1, newtarget);

	ereport(LOG,
			(errmsg("new target timeline is %u",
					recoveryTargetTLI)));

	return true;
}

/*
 * I/O routines for pg_control
 *
 * *ControlFile is a buffer in shared memory that holds an image of the
 * contents of pg_control.  WriteControlFile() initializes pg_control
 * given a preloaded buffer, ReadControlFile() loads the buffer from
 * the pg_control file (during postmaster or standalone-backend startup),
 * and UpdateControlFile() rewrites pg_control after we modify xlog state.
 * InitControlFile() fills the buffer with initial values.
 *
 * For simplicity, WriteControlFile() initializes the fields of pg_control
 * that are related to checking backend/database compatibility, and
 * ReadControlFile() verifies they are correct.  We could split out the
 * I/O and compatibility-check functions, but there seems no need currently.
 */

static void
InitControlFile(uint64 sysidentifier)
{
	char		mock_auth_nonce[MOCK_AUTH_NONCE_LEN];

	/*
	 * Generate a random nonce. This is used for authentication requests that
	 * will fail because the user does not exist. The nonce is used to create
	 * a genuine-looking password challenge for the non-existent user, in lieu
	 * of an actual stored password.
	 */
	if (!pg_strong_random(mock_auth_nonce, MOCK_AUTH_NONCE_LEN))
		ereport(PANIC,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not generate secret authorization token")));

	memset(ControlFile, 0, sizeof(ControlFileData));
	/* Initialize pg_control status fields */
	ControlFile->system_identifier = sysidentifier;
	memcpy(ControlFile->mock_authentication_nonce, mock_auth_nonce, MOCK_AUTH_NONCE_LEN);
	ControlFile->state = DB_SHUTDOWNED;
	ControlFile->unloggedLSN = FirstNormalUnloggedLSN;

	/* Set important parameter values for use when replaying WAL */
	ControlFile->MaxConnections = MaxConnections;
	ControlFile->max_worker_processes = max_worker_processes;
	ControlFile->max_wal_senders = max_wal_senders;
	ControlFile->max_prepared_xacts = max_prepared_xacts;
	ControlFile->max_locks_per_xact = max_locks_per_xact;
	ControlFile->wal_level = wal_level;
	ControlFile->wal_log_hints = wal_log_hints;
	ControlFile->track_commit_timestamp = track_commit_timestamp;
	ControlFile->data_checksum_version = bootstrap_data_checksum_version;
}

static void
WriteControlFile(void)
{
	int			fd;
	char		buffer[PG_CONTROL_FILE_SIZE];	/* need not be aligned */

	/*
	 * Ensure that the size of the pg_control data structure is sane.  See the
	 * comments for these symbols in pg_control.h.
	 */
	StaticAssertStmt(sizeof(ControlFileData) <= PG_CONTROL_MAX_SAFE_SIZE,
					 "pg_control is too large for atomic disk writes");
	StaticAssertStmt(sizeof(ControlFileData) <= PG_CONTROL_FILE_SIZE,
					 "sizeof(ControlFileData) exceeds PG_CONTROL_FILE_SIZE");

	/*
	 * Initialize version and compatibility-check fields
	 */
	ControlFile->pg_control_version = PG_CONTROL_VERSION;
	ControlFile->catalog_version_no = CATALOG_VERSION_NO;

	ControlFile->maxAlign = MAXIMUM_ALIGNOF;
	ControlFile->floatFormat = FLOATFORMAT_VALUE;

	ControlFile->blcksz = BLCKSZ;
	ControlFile->relseg_size = RELSEG_SIZE;
	ControlFile->xlog_blcksz = XLOG_BLCKSZ;
	ControlFile->xlog_seg_size = wal_segment_size;

	ControlFile->nameDataLen = NAMEDATALEN;
	ControlFile->indexMaxKeys = INDEX_MAX_KEYS;

	ControlFile->toast_max_chunk_size = TOAST_MAX_CHUNK_SIZE;
	ControlFile->loblksize = LOBLKSIZE;

	ControlFile->float8ByVal = FLOAT8PASSBYVAL;

	/* Contents are protected with a CRC */
	INIT_CRC32C(ControlFile->crc);
	COMP_CRC32C(ControlFile->crc,
				(char *) ControlFile,
				offsetof(ControlFileData, crc));
	FIN_CRC32C(ControlFile->crc);

	/*
	 * We write out PG_CONTROL_FILE_SIZE bytes into pg_control, zero-padding
	 * the excess over sizeof(ControlFileData).  This reduces the odds of
	 * premature-EOF errors when reading pg_control.  We'll still fail when we
	 * check the contents of the file, but hopefully with a more specific
	 * error than "couldn't read pg_control".
	 */
	memset(buffer, 0, PG_CONTROL_FILE_SIZE);
	memcpy(buffer, ControlFile, sizeof(ControlFileData));

	fd = BasicOpenFile(XLOG_CONTROL_FILE,
					   O_RDWR | O_CREAT | O_EXCL | PG_BINARY);
	if (fd < 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m",
						XLOG_CONTROL_FILE)));

	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_CONTROL_FILE_WRITE);
	if (write(fd, buffer, PG_CONTROL_FILE_SIZE) != PG_CONTROL_FILE_SIZE)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						XLOG_CONTROL_FILE)));
	}
	pgstat_report_wait_end();

	pgstat_report_wait_start(WAIT_EVENT_CONTROL_FILE_SYNC);
	if (pg_fsync(fd) != 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m",
						XLOG_CONTROL_FILE)));
	pgstat_report_wait_end();

	if (close(fd) != 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m",
						XLOG_CONTROL_FILE)));
}

static void
ReadControlFile(void)
{
	pg_crc32c	crc;
	int			fd;
	static char wal_segsz_str[20];
	int			r;

	/*
	 * Read data...
	 */
	fd = BasicOpenFile(XLOG_CONTROL_FILE,
					   O_RDWR | PG_BINARY);
	if (fd < 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						XLOG_CONTROL_FILE)));

	pgstat_report_wait_start(WAIT_EVENT_CONTROL_FILE_READ);
	r = read(fd, ControlFile, sizeof(ControlFileData));
	if (r != sizeof(ControlFileData))
	{
		if (r < 0)
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m",
							XLOG_CONTROL_FILE)));
		else
			ereport(PANIC,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("could not read file \"%s\": read %d of %zu",
							XLOG_CONTROL_FILE, r, sizeof(ControlFileData))));
	}
	pgstat_report_wait_end();

	close(fd);

	/*
	 * Check for expected pg_control format version.  If this is wrong, the
	 * CRC check will likely fail because we'll be checking the wrong number
	 * of bytes.  Complaining about wrong version will probably be more
	 * enlightening than complaining about wrong CRC.
	 */

	if (ControlFile->pg_control_version != PG_CONTROL_VERSION && ControlFile->pg_control_version % 65536 == 0 && ControlFile->pg_control_version / 65536 != 0)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with PG_CONTROL_VERSION %d (0x%08x),"
						   " but the server was compiled with PG_CONTROL_VERSION %d (0x%08x).",
						   ControlFile->pg_control_version, ControlFile->pg_control_version,
						   PG_CONTROL_VERSION, PG_CONTROL_VERSION),
				 errhint("This could be a problem of mismatched byte ordering.  It looks like you need to initdb.")));

	if (ControlFile->pg_control_version != PG_CONTROL_VERSION)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with PG_CONTROL_VERSION %d,"
						   " but the server was compiled with PG_CONTROL_VERSION %d.",
						   ControlFile->pg_control_version, PG_CONTROL_VERSION),
				 errhint("It looks like you need to initdb.")));

	/* Now check the CRC. */
	INIT_CRC32C(crc);
	COMP_CRC32C(crc,
				(char *) ControlFile,
				offsetof(ControlFileData, crc));
	FIN_CRC32C(crc);

	if (!EQ_CRC32C(crc, ControlFile->crc))
		ereport(FATAL,
				(errmsg("incorrect checksum in control file")));

	/*
	 * Do compatibility checking immediately.  If the database isn't
	 * compatible with the backend executable, we want to abort before we can
	 * possibly do any damage.
	 */
	if (ControlFile->catalog_version_no != CATALOG_VERSION_NO)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with CATALOG_VERSION_NO %d,"
						   " but the server was compiled with CATALOG_VERSION_NO %d.",
						   ControlFile->catalog_version_no, CATALOG_VERSION_NO),
				 errhint("It looks like you need to initdb.")));
	if (ControlFile->maxAlign != MAXIMUM_ALIGNOF)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with MAXALIGN %d,"
						   " but the server was compiled with MAXALIGN %d.",
						   ControlFile->maxAlign, MAXIMUM_ALIGNOF),
				 errhint("It looks like you need to initdb.")));
	if (ControlFile->floatFormat != FLOATFORMAT_VALUE)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster appears to use a different floating-point number format than the server executable."),
				 errhint("It looks like you need to initdb.")));
	if (ControlFile->blcksz != BLCKSZ)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with BLCKSZ %d,"
						   " but the server was compiled with BLCKSZ %d.",
						   ControlFile->blcksz, BLCKSZ),
				 errhint("It looks like you need to recompile or initdb.")));
	if (ControlFile->relseg_size != RELSEG_SIZE)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with RELSEG_SIZE %d,"
						   " but the server was compiled with RELSEG_SIZE %d.",
						   ControlFile->relseg_size, RELSEG_SIZE),
				 errhint("It looks like you need to recompile or initdb.")));
	if (ControlFile->xlog_blcksz != XLOG_BLCKSZ)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with XLOG_BLCKSZ %d,"
						   " but the server was compiled with XLOG_BLCKSZ %d.",
						   ControlFile->xlog_blcksz, XLOG_BLCKSZ),
				 errhint("It looks like you need to recompile or initdb.")));
	if (ControlFile->nameDataLen != NAMEDATALEN)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with NAMEDATALEN %d,"
						   " but the server was compiled with NAMEDATALEN %d.",
						   ControlFile->nameDataLen, NAMEDATALEN),
				 errhint("It looks like you need to recompile or initdb.")));
	if (ControlFile->indexMaxKeys != INDEX_MAX_KEYS)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with INDEX_MAX_KEYS %d,"
						   " but the server was compiled with INDEX_MAX_KEYS %d.",
						   ControlFile->indexMaxKeys, INDEX_MAX_KEYS),
				 errhint("It looks like you need to recompile or initdb.")));
	if (ControlFile->toast_max_chunk_size != TOAST_MAX_CHUNK_SIZE)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with TOAST_MAX_CHUNK_SIZE %d,"
						   " but the server was compiled with TOAST_MAX_CHUNK_SIZE %d.",
						   ControlFile->toast_max_chunk_size, (int) TOAST_MAX_CHUNK_SIZE),
				 errhint("It looks like you need to recompile or initdb.")));
	if (ControlFile->loblksize != LOBLKSIZE)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with LOBLKSIZE %d,"
						   " but the server was compiled with LOBLKSIZE %d.",
						   ControlFile->loblksize, (int) LOBLKSIZE),
				 errhint("It looks like you need to recompile or initdb.")));

#ifdef USE_FLOAT8_BYVAL
	if (ControlFile->float8ByVal != true)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized without USE_FLOAT8_BYVAL"
						   " but the server was compiled with USE_FLOAT8_BYVAL."),
				 errhint("It looks like you need to recompile or initdb.")));
#else
	if (ControlFile->float8ByVal != false)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with USE_FLOAT8_BYVAL"
						   " but the server was compiled without USE_FLOAT8_BYVAL."),
				 errhint("It looks like you need to recompile or initdb.")));
#endif

	wal_segment_size = ControlFile->xlog_seg_size;

	if (!IsValidWalSegSize(wal_segment_size))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg_plural("WAL segment size must be a power of two between 1 MB and 1 GB, but the control file specifies %d byte",
									  "WAL segment size must be a power of two between 1 MB and 1 GB, but the control file specifies %d bytes",
									  wal_segment_size,
									  wal_segment_size)));

	snprintf(wal_segsz_str, sizeof(wal_segsz_str), "%d", wal_segment_size);
	SetConfigOption("wal_segment_size", wal_segsz_str, PGC_INTERNAL,
					PGC_S_OVERRIDE);

	/* check and update variables dependent on wal_segment_size */
	if (ConvertToXSegs(min_wal_size_mb, wal_segment_size) < 2)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("\"min_wal_size\" must be at least twice \"wal_segment_size\"")));

	if (ConvertToXSegs(max_wal_size_mb, wal_segment_size) < 2)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("\"max_wal_size\" must be at least twice \"wal_segment_size\"")));

	UsableBytesInSegment =
		(wal_segment_size / XLOG_BLCKSZ * UsableBytesInPage) -
		(SizeOfXLogLongPHD - SizeOfXLogShortPHD);

	CalculateCheckpointSegments();

	/* Make the initdb settings visible as GUC variables, too */
	SetConfigOption("data_checksums", DataChecksumsEnabled() ? "yes" : "no",
					PGC_INTERNAL, PGC_S_OVERRIDE);
}

/*
 * Utility wrapper to update the control file.  Note that the control
 * file gets flushed.
 */
void
UpdateControlFile(void)
{
	update_controlfile(DataDir, ControlFile, true);
}

/*
 * Returns the unique system identifier from control file.
 */
uint64
GetSystemIdentifier(void)
{
	Assert(ControlFile != NULL);
	return ControlFile->system_identifier;
}

/*
 * Returns the random nonce from control file.
 */
char *
GetMockAuthenticationNonce(void)
{
	Assert(ControlFile != NULL);
	return ControlFile->mock_authentication_nonce;
}

/*
 * Are checksums enabled for data pages?
 */
bool
DataChecksumsEnabled(void)
{
	Assert(ControlFile != NULL);
	return (ControlFile->data_checksum_version > 0);
}

/*
 * Returns a fake LSN for unlogged relations.
 *
 * Each call generates an LSN that is greater than any previous value
 * returned. The current counter value is saved and restored across clean
 * shutdowns, but like unlogged relations, does not survive a crash. This can
 * be used in lieu of real LSN values returned by XLogInsert, if you need an
 * LSN-like increasing sequence of numbers without writing any WAL.
 */
XLogRecPtr
GetFakeLSNForUnloggedRel(void)
{
	XLogRecPtr	nextUnloggedLSN;

	/* increment the unloggedLSN counter, need SpinLock */
	SpinLockAcquire(&XLogCtl->ulsn_lck);
	nextUnloggedLSN = XLogCtl->unloggedLSN++;
	SpinLockRelease(&XLogCtl->ulsn_lck);

	return nextUnloggedLSN;
}

/*
 * Auto-tune the number of XLOG buffers.
 *
 * The preferred setting for wal_buffers is about 3% of shared_buffers, with
 * a maximum of one XLOG segment (there is little reason to think that more
 * is helpful, at least so long as we force an fsync when switching log files)
 * and a minimum of 8 blocks (which was the default value prior to PostgreSQL
 * 9.1, when auto-tuning was added).
 *
 * This should not be called until NBuffers has received its final value.
 */
static int
XLOGChooseNumBuffers(void)
{
	int			xbuffers;

	xbuffers = NBuffers / 32;
	if (xbuffers > (wal_segment_size / XLOG_BLCKSZ))
		xbuffers = (wal_segment_size / XLOG_BLCKSZ);
	if (xbuffers < 8)
		xbuffers = 8;
	return xbuffers;
}

/*
 * GUC check_hook for wal_buffers
 */
bool
check_wal_buffers(int *newval, void **extra, GucSource source)
{
	/*
	 * -1 indicates a request for auto-tune.
	 */
	if (*newval == -1)
	{
		/*
		 * If we haven't yet changed the boot_val default of -1, just let it
		 * be.  We'll fix it when XLOGShmemSize is called.
		 */
		if (XLOGbuffers == -1)
			return true;

		/* Otherwise, substitute the auto-tune value */
		*newval = XLOGChooseNumBuffers();
	}

	/*
	 * We clamp manually-set values to at least 4 blocks.  Prior to PostgreSQL
	 * 9.1, a minimum of 4 was enforced by guc.c, but since that is no longer
	 * the case, we just silently treat such values as a request for the
	 * minimum.  (We could throw an error instead, but that doesn't seem very
	 * helpful.)
	 */
	if (*newval < 4)
		*newval = 4;

	return true;
}

/*
 * Read the control file, set respective GUCs.
 *
 * This is to be called during startup, including a crash recovery cycle,
 * unless in bootstrap mode, where no control file yet exists.  As there's no
 * usable shared memory yet (its sizing can depend on the contents of the
 * control file!), first store the contents in local memory. XLOGShmemInit()
 * will then copy it to shared memory later.
 *
 * reset just controls whether previous contents are to be expected (in the
 * reset case, there's a dangling pointer into old shared memory), or not.
 */
void
LocalProcessControlFile(bool reset)
{
	Assert(reset || ControlFile == NULL);
	ControlFile = palloc(sizeof(ControlFileData));
	ReadControlFile();
}

/*
 * Initialization of shared memory for XLOG
 */
Size
XLOGShmemSize(void)
{
	Size		size;

	/*
	 * If the value of wal_buffers is -1, use the preferred auto-tune value.
	 * This isn't an amazingly clean place to do this, but we must wait till
	 * NBuffers has received its final value, and must do it before using the
	 * value of XLOGbuffers to do anything important.
	 */
	if (XLOGbuffers == -1)
	{
		char		buf[32];

		snprintf(buf, sizeof(buf), "%d", XLOGChooseNumBuffers());
		SetConfigOption("wal_buffers", buf, PGC_POSTMASTER, PGC_S_OVERRIDE);
	}
	Assert(XLOGbuffers > 0);

	/* XLogCtl */
	size = sizeof(XLogCtlData);

	/* WAL insertion locks, plus alignment */
	size = add_size(size, mul_size(sizeof(WALInsertLockPadded), NUM_XLOGINSERT_LOCKS + 1));
	/* xlblocks array */
	size = add_size(size, mul_size(sizeof(XLogRecPtr), XLOGbuffers));
	/* extra alignment padding for XLOG I/O buffers */
	size = add_size(size, XLOG_BLCKSZ);
	/* and the buffers themselves */
	size = add_size(size, mul_size(XLOG_BLCKSZ, XLOGbuffers));

	/*
	 * Note: we don't count ControlFileData, it comes out of the "slop factor"
	 * added by CreateSharedMemoryAndSemaphores.  This lets us use this
	 * routine again below to compute the actual allocation size.
	 */

	return size;
}

void
XLOGShmemInit(void)
{
	bool		foundCFile,
				foundXLog;
	char	   *allocptr;
	int			i;
	ControlFileData *localControlFile;

#ifdef WAL_DEBUG

	/*
	 * Create a memory context for WAL debugging that's exempt from the normal
	 * "no pallocs in critical section" rule. Yes, that can lead to a PANIC if
	 * an allocation fails, but wal_debug is not for production use anyway.
	 */
	if (walDebugCxt == NULL)
	{
		walDebugCxt = AllocSetContextCreate(TopMemoryContext,
											"WAL Debug",
											ALLOCSET_DEFAULT_SIZES);
		MemoryContextAllowInCriticalSection(walDebugCxt, true);
	}
#endif


	XLogCtl = (XLogCtlData *)
		ShmemInitStruct("XLOG Ctl", XLOGShmemSize(), &foundXLog);

	localControlFile = ControlFile;
	ControlFile = (ControlFileData *)
		ShmemInitStruct("Control File", sizeof(ControlFileData), &foundCFile);

	if (foundCFile || foundXLog)
	{
		/* both should be present or neither */
		Assert(foundCFile && foundXLog);

		/* Initialize local copy of WALInsertLocks */
		WALInsertLocks = XLogCtl->Insert.WALInsertLocks;

		if (localControlFile)
			pfree(localControlFile);
		return;
	}
	memset(XLogCtl, 0, sizeof(XLogCtlData));

	/*
	 * Already have read control file locally, unless in bootstrap mode. Move
	 * contents into shared memory.
	 */
	if (localControlFile)
	{
		memcpy(ControlFile, localControlFile, sizeof(ControlFileData));
		pfree(localControlFile);
	}

	/*
	 * Since XLogCtlData contains XLogRecPtr fields, its sizeof should be a
	 * multiple of the alignment for same, so no extra alignment padding is
	 * needed here.
	 */
	allocptr = ((char *) XLogCtl) + sizeof(XLogCtlData);
	XLogCtl->xlblocks = (XLogRecPtr *) allocptr;
	memset(XLogCtl->xlblocks, 0, sizeof(XLogRecPtr) * XLOGbuffers);
	allocptr += sizeof(XLogRecPtr) * XLOGbuffers;


	/* WAL insertion locks. Ensure they're aligned to the full padded size */
	allocptr += sizeof(WALInsertLockPadded) -
		((uintptr_t) allocptr) % sizeof(WALInsertLockPadded);
	WALInsertLocks = XLogCtl->Insert.WALInsertLocks =
		(WALInsertLockPadded *) allocptr;
	allocptr += sizeof(WALInsertLockPadded) * NUM_XLOGINSERT_LOCKS;

	for (i = 0; i < NUM_XLOGINSERT_LOCKS; i++)
	{
		LWLockInitialize(&WALInsertLocks[i].l.lock, LWTRANCHE_WAL_INSERT);
		WALInsertLocks[i].l.insertingAt = InvalidXLogRecPtr;
		WALInsertLocks[i].l.lastImportantAt = InvalidXLogRecPtr;
	}

	/*
	 * Align the start of the page buffers to a full xlog block size boundary.
	 * This simplifies some calculations in XLOG insertion. It is also
	 * required for O_DIRECT.
	 */
	allocptr = (char *) TYPEALIGN(XLOG_BLCKSZ, allocptr);
	XLogCtl->pages = allocptr;
	memset(XLogCtl->pages, 0, (Size) XLOG_BLCKSZ * XLOGbuffers);

	/*
	 * Do basic initialization of XLogCtl shared data. (StartupXLOG will fill
	 * in additional info.)
	 */
	XLogCtl->XLogCacheBlck = XLOGbuffers - 1;
	XLogCtl->SharedRecoveryState = RECOVERY_STATE_CRASH;
	XLogCtl->SharedHotStandbyActive = false;
	XLogCtl->SharedPromoteIsTriggered = false;
	XLogCtl->WalWriterSleeping = false;

	SpinLockInit(&XLogCtl->Insert.insertpos_lck);
	SpinLockInit(&XLogCtl->info_lck);
	SpinLockInit(&XLogCtl->ulsn_lck);
	InitSharedLatch(&XLogCtl->recoveryWakeupLatch);
}

/*
 * This func must be called ONCE on system install.  It creates pg_control
 * and the initial XLOG segment.
 */
void
BootStrapXLOG(void)
{
	CheckPoint	checkPoint;
	char	   *buffer;
	XLogPageHeader page;
	XLogLongPageHeader longpage;
	XLogRecord *record;
	char	   *recptr;
	bool		use_existent;
	uint64		sysidentifier;
	struct timeval tv;
	pg_crc32c	crc;

	/*
	 * Select a hopefully-unique system identifier code for this installation.
	 * We use the result of gettimeofday(), including the fractional seconds
	 * field, as being about as unique as we can easily get.  (Think not to
	 * use random(), since it hasn't been seeded and there's no portable way
	 * to seed it other than the system clock value...)  The upper half of the
	 * uint64 value is just the tv_sec part, while the lower half contains the
	 * tv_usec part (which must fit in 20 bits), plus 12 bits from our current
	 * PID for a little extra uniqueness.  A person knowing this encoding can
	 * determine the initialization time of the installation, which could
	 * perhaps be useful sometimes.
	 */
	gettimeofday(&tv, NULL);
	sysidentifier = ((uint64) tv.tv_sec) << 32;
	sysidentifier |= ((uint64) tv.tv_usec) << 12;
	sysidentifier |= getpid() & 0xFFF;

	/* First timeline ID is always 1 */
	ThisTimeLineID = 1;

	/* page buffer must be aligned suitably for O_DIRECT */
	buffer = (char *) palloc(XLOG_BLCKSZ + XLOG_BLCKSZ);
	page = (XLogPageHeader) TYPEALIGN(XLOG_BLCKSZ, buffer);
	memset(page, 0, XLOG_BLCKSZ);

	/*
	 * Set up information for the initial checkpoint record
	 *
	 * The initial checkpoint record is written to the beginning of the WAL
	 * segment with logid=0 logseg=1. The very first WAL segment, 0/0, is not
	 * used, so that we can use 0/0 to mean "before any valid WAL segment".
	 */
	checkPoint.redo = wal_segment_size + SizeOfXLogLongPHD;
	checkPoint.ThisTimeLineID = ThisTimeLineID;
	checkPoint.PrevTimeLineID = ThisTimeLineID;
	checkPoint.fullPageWrites = fullPageWrites;
	checkPoint.nextFullXid =
		FullTransactionIdFromEpochAndXid(0, FirstNormalTransactionId);
	checkPoint.nextOid = FirstBootstrapObjectId;
	checkPoint.nextMulti = FirstMultiXactId;
	checkPoint.nextMultiOffset = 0;
	checkPoint.oldestXid = FirstNormalTransactionId;
	checkPoint.oldestXidDB = TemplateDbOid;
	checkPoint.oldestMulti = FirstMultiXactId;
	checkPoint.oldestMultiDB = TemplateDbOid;
	checkPoint.oldestCommitTsXid = InvalidTransactionId;
	checkPoint.newestCommitTsXid = InvalidTransactionId;
	checkPoint.time = (pg_time_t) time(NULL);
	checkPoint.oldestActiveXid = InvalidTransactionId;

	ShmemVariableCache->nextFullXid = checkPoint.nextFullXid;
	ShmemVariableCache->nextOid = checkPoint.nextOid;
	ShmemVariableCache->oidCount = 0;
	MultiXactSetNextMXact(checkPoint.nextMulti, checkPoint.nextMultiOffset);
	AdvanceOldestClogXid(checkPoint.oldestXid);
	SetTransactionIdLimit(checkPoint.oldestXid, checkPoint.oldestXidDB);
	SetMultiXactIdLimit(checkPoint.oldestMulti, checkPoint.oldestMultiDB, true);
	SetCommitTsLimit(InvalidTransactionId, InvalidTransactionId);

	/* Set up the XLOG page header */
	page->xlp_magic = XLOG_PAGE_MAGIC;
	page->xlp_info = XLP_LONG_HEADER;
	page->xlp_tli = ThisTimeLineID;
	page->xlp_pageaddr = wal_segment_size;
	longpage = (XLogLongPageHeader) page;
	longpage->xlp_sysid = sysidentifier;
	longpage->xlp_seg_size = wal_segment_size;
	longpage->xlp_xlog_blcksz = XLOG_BLCKSZ;

	/* Insert the initial checkpoint record */
	recptr = ((char *) page + SizeOfXLogLongPHD);
	record = (XLogRecord *) recptr;
	record->xl_prev = 0;
	record->xl_xid = InvalidTransactionId;
	record->xl_tot_len = SizeOfXLogRecord + SizeOfXLogRecordDataHeaderShort + sizeof(checkPoint);
	record->xl_info = XLOG_CHECKPOINT_SHUTDOWN;
	record->xl_rmid = RM_XLOG_ID;
	recptr += SizeOfXLogRecord;
	/* fill the XLogRecordDataHeaderShort struct */
	*(recptr++) = (char) XLR_BLOCK_ID_DATA_SHORT;
	*(recptr++) = sizeof(checkPoint);
	memcpy(recptr, &checkPoint, sizeof(checkPoint));
	recptr += sizeof(checkPoint);
	Assert(recptr - (char *) record == record->xl_tot_len);

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, ((char *) record) + SizeOfXLogRecord, record->xl_tot_len - SizeOfXLogRecord);
	COMP_CRC32C(crc, (char *) record, offsetof(XLogRecord, xl_crc));
	FIN_CRC32C(crc);
	record->xl_crc = crc;

	/* Create first XLOG segment file */
	use_existent = false;
	openLogFile = XLogFileInit(1, &use_existent, false);

	/*
	 * We needn't bother with Reserve/ReleaseExternalFD here, since we'll
	 * close the file again in a moment.
	 */

	/* Write the first page with the initial record */
	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_WAL_BOOTSTRAP_WRITE);
	if (write(openLogFile, page, XLOG_BLCKSZ) != XLOG_BLCKSZ)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not write bootstrap write-ahead log file: %m")));
	}
	pgstat_report_wait_end();

	pgstat_report_wait_start(WAIT_EVENT_WAL_BOOTSTRAP_SYNC);
	if (pg_fsync(openLogFile) != 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not fsync bootstrap write-ahead log file: %m")));
	pgstat_report_wait_end();

	if (close(openLogFile) != 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not close bootstrap write-ahead log file: %m")));

	openLogFile = -1;

	/* Now create pg_control */
	InitControlFile(sysidentifier);
	ControlFile->time = checkPoint.time;
	ControlFile->checkPoint = checkPoint.redo;
	ControlFile->checkPointCopy = checkPoint;

	/* some additional ControlFile fields are set in WriteControlFile() */
	WriteControlFile();

	/* Bootstrap the commit log, too */
	BootStrapCLOG();
	BootStrapCommitTs();
	BootStrapSUBTRANS();
	BootStrapMultiXact();

	pfree(buffer);

	/*
	 * Force control file to be read - in contrast to normal processing we'd
	 * otherwise never run the checks and GUC related initializations therein.
	 */
	ReadControlFile();
}

static char *
str_time(pg_time_t tnow)
{
	static char buf[128];

	pg_strftime(buf, sizeof(buf),
				"%Y-%m-%d %H:%M:%S %Z",
				pg_localtime(&tnow, log_timezone));

	return buf;
}

/*
 * See if there are any recovery signal files and if so, set state for
 * recovery.
 *
 * See if there is a recovery command file (recovery.conf), and if so
 * throw an ERROR since as of PG12 we no longer recognize that.
 */
static void
readRecoverySignalFile(void)
{
	struct stat stat_buf;

	if (IsBootstrapProcessingMode())
		return;

	/*
	 * Check for old recovery API file: recovery.conf
	 */
	if (stat(RECOVERY_COMMAND_FILE, &stat_buf) == 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("using recovery command file \"%s\" is not supported",
						RECOVERY_COMMAND_FILE)));

	/*
	 * Remove unused .done file, if present. Ignore if absent.
	 */
	unlink(RECOVERY_COMMAND_DONE);

	/*
	 * Check for recovery signal files and if found, fsync them since they
	 * represent server state information.  We don't sweat too much about the
	 * possibility of fsync failure, however.
	 *
	 * If present, standby signal file takes precedence. If neither is present
	 * then we won't enter archive recovery.
	 */
	if (stat(STANDBY_SIGNAL_FILE, &stat_buf) == 0)
	{
		int			fd;

		fd = BasicOpenFilePerm(STANDBY_SIGNAL_FILE, O_RDWR | PG_BINARY | get_sync_bit(sync_method),
							   S_IRUSR | S_IWUSR);
		if (fd >= 0)
		{
			(void) pg_fsync(fd);
			close(fd);
		}
		standby_signal_file_found = true;
	}
	else if (stat(RECOVERY_SIGNAL_FILE, &stat_buf) == 0)
	{
		int			fd;

		fd = BasicOpenFilePerm(RECOVERY_SIGNAL_FILE, O_RDWR | PG_BINARY | get_sync_bit(sync_method),
							   S_IRUSR | S_IWUSR);
		if (fd >= 0)
		{
			(void) pg_fsync(fd);
			close(fd);
		}
		recovery_signal_file_found = true;
	}

	StandbyModeRequested = false;
	ArchiveRecoveryRequested = false;
	if (standby_signal_file_found)
	{
		StandbyModeRequested = true;
		ArchiveRecoveryRequested = true;
	}
	else if (recovery_signal_file_found)
	{
		StandbyModeRequested = false;
		ArchiveRecoveryRequested = true;
	}
	else
		return;

	/*
	 * We don't support standby mode in standalone backends; that requires
	 * other processes such as the WAL receiver to be alive.
	 */
	if (StandbyModeRequested && !IsUnderPostmaster)
		ereport(FATAL,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("standby mode is not supported by single-user servers")));
}

static void
validateRecoveryParameters(void)
{
	if (!ArchiveRecoveryRequested)
		return;

	/*
	 * Check for compulsory parameters
	 */
	if (StandbyModeRequested)
	{
		if ((PrimaryConnInfo == NULL || strcmp(PrimaryConnInfo, "") == 0) &&
			(recoveryRestoreCommand == NULL || strcmp(recoveryRestoreCommand, "") == 0))
			ereport(WARNING,
					(errmsg("specified neither primary_conninfo nor restore_command"),
					 errhint("The database server will regularly poll the pg_wal subdirectory to check for files placed there.")));
	}
	else
	{
		if (recoveryRestoreCommand == NULL ||
			strcmp(recoveryRestoreCommand, "") == 0)
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("must specify restore_command when standby mode is not enabled")));
	}

	/*
	 * Override any inconsistent requests. Note that this is a change of
	 * behaviour in 9.5; prior to this we simply ignored a request to pause if
	 * hot_standby = off, which was surprising behaviour.
	 */
	if (recoveryTargetAction == RECOVERY_TARGET_ACTION_PAUSE &&
		!EnableHotStandby)
		recoveryTargetAction = RECOVERY_TARGET_ACTION_SHUTDOWN;

	/*
	 * Final parsing of recovery_target_time string; see also
	 * check_recovery_target_time().
	 */
	if (recoveryTarget == RECOVERY_TARGET_TIME)
	{
		recoveryTargetTime = DatumGetTimestampTz(DirectFunctionCall3(timestamptz_in,
																	 CStringGetDatum(recovery_target_time_string),
																	 ObjectIdGetDatum(InvalidOid),
																	 Int32GetDatum(-1)));
	}

	/*
	 * If user specified recovery_target_timeline, validate it or compute the
	 * "latest" value.  We can't do this until after we've gotten the restore
	 * command and set InArchiveRecovery, because we need to fetch timeline
	 * history files from the archive.
	 */
	if (recoveryTargetTimeLineGoal == RECOVERY_TARGET_TIMELINE_NUMERIC)
	{
		TimeLineID	rtli = recoveryTargetTLIRequested;

		/* Timeline 1 does not have a history file, all else should */
		if (rtli != 1 && !existsTimeLineHistory(rtli))
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("recovery target timeline %u does not exist",
							rtli)));
		recoveryTargetTLI = rtli;
	}
	else if (recoveryTargetTimeLineGoal == RECOVERY_TARGET_TIMELINE_LATEST)
	{
		/* We start the "latest" search from pg_control's timeline */
		recoveryTargetTLI = findNewestTimeLine(recoveryTargetTLI);
	}
	else
	{
		/*
		 * else we just use the recoveryTargetTLI as already read from
		 * ControlFile
		 */
		Assert(recoveryTargetTimeLineGoal == RECOVERY_TARGET_TIMELINE_CONTROLFILE);
	}
}

/*
 * Exit archive-recovery state
 */
static void
exitArchiveRecovery(TimeLineID endTLI, XLogRecPtr endOfLog)
{
	char		xlogfname[MAXFNAMELEN];
	XLogSegNo	endLogSegNo;
	XLogSegNo	startLogSegNo;

	/* we always switch to a new timeline after archive recovery */
	Assert(endTLI != ThisTimeLineID);

	/*
	 * We are no longer in archive recovery state.
	 */
	InArchiveRecovery = false;

	/*
	 * Update min recovery point one last time.
	 */
	UpdateMinRecoveryPoint(InvalidXLogRecPtr, true);

	/*
	 * If the ending log segment is still open, close it (to avoid problems on
	 * Windows with trying to rename or delete an open file).
	 */
	if (readFile >= 0)
	{
		close(readFile);
		readFile = -1;
	}

	/*
	 * Calculate the last segment on the old timeline, and the first segment
	 * on the new timeline. If the switch happens in the middle of a segment,
	 * they are the same, but if the switch happens exactly at a segment
	 * boundary, startLogSegNo will be endLogSegNo + 1.
	 */
	XLByteToPrevSeg(endOfLog, endLogSegNo, wal_segment_size);
	XLByteToSeg(endOfLog, startLogSegNo, wal_segment_size);

	/*
	 * Initialize the starting WAL segment for the new timeline. If the switch
	 * happens in the middle of a segment, copy data from the last WAL segment
	 * of the old timeline up to the switch point, to the starting WAL segment
	 * on the new timeline.
	 */
	if (endLogSegNo == startLogSegNo)
	{
		/*
		 * Make a copy of the file on the new timeline.
		 *
		 * Writing WAL isn't allowed yet, so there are no locking
		 * considerations. But we should be just as tense as XLogFileInit to
		 * avoid emplacing a bogus file.
		 */
		XLogFileCopy(endLogSegNo, endTLI, endLogSegNo,
					 XLogSegmentOffset(endOfLog, wal_segment_size));
	}
	else
	{
		/*
		 * The switch happened at a segment boundary, so just create the next
		 * segment on the new timeline.
		 */
		bool		use_existent = true;
		int			fd;

		fd = XLogFileInit(startLogSegNo, &use_existent, true);

		if (close(fd) != 0)
		{
			char		xlogfname[MAXFNAMELEN];
			int			save_errno = errno;

			XLogFileName(xlogfname, ThisTimeLineID, startLogSegNo,
						 wal_segment_size);
			errno = save_errno;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not close file \"%s\": %m", xlogfname)));
		}
	}

	/*
	 * Let's just make real sure there are not .ready or .done flags posted
	 * for the new segment.
	 */
	XLogFileName(xlogfname, ThisTimeLineID, startLogSegNo, wal_segment_size);
	XLogArchiveCleanup(xlogfname);

	/*
	 * Remove the signal files out of the way, so that we don't accidentally
	 * re-enter archive recovery mode in a subsequent crash.
	 */
	if (standby_signal_file_found)
		durable_unlink(STANDBY_SIGNAL_FILE, FATAL);

	if (recovery_signal_file_found)
		durable_unlink(RECOVERY_SIGNAL_FILE, FATAL);

	ereport(LOG,
			(errmsg("archive recovery complete")));
}

/*
 * Extract timestamp from WAL record.
 *
 * If the record contains a timestamp, returns true, and saves the timestamp
 * in *recordXtime. If the record type has no timestamp, returns false.
 * Currently, only transaction commit/abort records and restore points contain
 * timestamps.
 */
static bool
getRecordTimestamp(XLogReaderState *record, TimestampTz *recordXtime)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	uint8		xact_info = info & XLOG_XACT_OPMASK;
	uint8		rmid = XLogRecGetRmid(record);

	if (rmid == RM_XLOG_ID && info == XLOG_RESTORE_POINT)
	{
		*recordXtime = ((xl_restore_point *) XLogRecGetData(record))->rp_time;
		return true;
	}
	if (rmid == RM_XACT_ID && (xact_info == XLOG_XACT_COMMIT ||
							   xact_info == XLOG_XACT_COMMIT_PREPARED))
	{
		*recordXtime = ((xl_xact_commit *) XLogRecGetData(record))->xact_time;
		return true;
	}
	if (rmid == RM_XACT_ID && (xact_info == XLOG_XACT_ABORT ||
							   xact_info == XLOG_XACT_ABORT_PREPARED))
	{
		*recordXtime = ((xl_xact_abort *) XLogRecGetData(record))->xact_time;
		return true;
	}
	return false;
}

/*
 * For point-in-time recovery, this function decides whether we want to
 * stop applying the XLOG before the current record.
 *
 * Returns true if we are stopping, false otherwise. If stopping, some
 * information is saved in recoveryStopXid et al for use in annotating the
 * new timeline's history file.
 */
static bool
recoveryStopsBefore(XLogReaderState *record)
{
	bool		stopsHere = false;
	uint8		xact_info;
	bool		isCommit;
	TimestampTz recordXtime = 0;
	TransactionId recordXid;

	/*
	 * Ignore recovery target settings when not in archive recovery (meaning
	 * we are in crash recovery).
	 */
	if (!ArchiveRecoveryRequested)
		return false;

	/* Check if we should stop as soon as reaching consistency */
	if (recoveryTarget == RECOVERY_TARGET_IMMEDIATE && reachedConsistency)
	{
		ereport(LOG,
				(errmsg("recovery stopping after reaching consistency")));

		recoveryStopAfter = false;
		recoveryStopXid = InvalidTransactionId;
		recoveryStopLSN = InvalidXLogRecPtr;
		recoveryStopTime = 0;
		recoveryStopName[0] = '\0';
		return true;
	}

	/* Check if target LSN has been reached */
	if (recoveryTarget == RECOVERY_TARGET_LSN &&
		!recoveryTargetInclusive &&
		record->ReadRecPtr >= recoveryTargetLSN)
	{
		recoveryStopAfter = false;
		recoveryStopXid = InvalidTransactionId;
		recoveryStopLSN = record->ReadRecPtr;
		recoveryStopTime = 0;
		recoveryStopName[0] = '\0';
		ereport(LOG,
				(errmsg("recovery stopping before WAL location (LSN) \"%X/%X\"",
						(uint32) (recoveryStopLSN >> 32),
						(uint32) recoveryStopLSN)));
		return true;
	}

	/* Otherwise we only consider stopping before COMMIT or ABORT records. */
	if (XLogRecGetRmid(record) != RM_XACT_ID)
		return false;

	xact_info = XLogRecGetInfo(record) & XLOG_XACT_OPMASK;

	if (xact_info == XLOG_XACT_COMMIT)
	{
		isCommit = true;
		recordXid = XLogRecGetXid(record);
	}
	else if (xact_info == XLOG_XACT_COMMIT_PREPARED)
	{
		xl_xact_commit *xlrec = (xl_xact_commit *) XLogRecGetData(record);
		xl_xact_parsed_commit parsed;

		isCommit = true;
		ParseCommitRecord(XLogRecGetInfo(record),
						  xlrec,
						  &parsed);
		recordXid = parsed.twophase_xid;
	}
	else if (xact_info == XLOG_XACT_ABORT)
	{
		isCommit = false;
		recordXid = XLogRecGetXid(record);
	}
	else if (xact_info == XLOG_XACT_ABORT_PREPARED)
	{
		xl_xact_abort *xlrec = (xl_xact_abort *) XLogRecGetData(record);
		xl_xact_parsed_abort parsed;

		isCommit = false;
		ParseAbortRecord(XLogRecGetInfo(record),
						 xlrec,
						 &parsed);
		recordXid = parsed.twophase_xid;
	}
	else
		return false;

	if (recoveryTarget == RECOVERY_TARGET_XID && !recoveryTargetInclusive)
	{
		/*
		 * There can be only one transaction end record with this exact
		 * transactionid
		 *
		 * when testing for an xid, we MUST test for equality only, since
		 * transactions are numbered in the order they start, not the order
		 * they complete. A higher numbered xid will complete before you about
		 * 50% of the time...
		 */
		stopsHere = (recordXid == recoveryTargetXid);
	}

	/*
	 * Note: we must fetch recordXtime regardless of recoveryTarget setting.
	 * We don't expect getRecordTimestamp ever to fail, since we already know
	 * this is a commit or abort record; but test its result anyway.
	 */
	if (getRecordTimestamp(record, &recordXtime) &&
		recoveryTarget == RECOVERY_TARGET_TIME)
	{
		/*
		 * There can be many transactions that share the same commit time, so
		 * we stop after the last one, if we are inclusive, or stop at the
		 * first one if we are exclusive
		 */
		if (recoveryTargetInclusive)
			stopsHere = (recordXtime > recoveryTargetTime);
		else
			stopsHere = (recordXtime >= recoveryTargetTime);
	}

	if (stopsHere)
	{
		recoveryStopAfter = false;
		recoveryStopXid = recordXid;
		recoveryStopTime = recordXtime;
		recoveryStopLSN = InvalidXLogRecPtr;
		recoveryStopName[0] = '\0';

		if (isCommit)
		{
			ereport(LOG,
					(errmsg("recovery stopping before commit of transaction %u, time %s",
							recoveryStopXid,
							timestamptz_to_str(recoveryStopTime))));
		}
		else
		{
			ereport(LOG,
					(errmsg("recovery stopping before abort of transaction %u, time %s",
							recoveryStopXid,
							timestamptz_to_str(recoveryStopTime))));
		}
	}

	return stopsHere;
}

/*
 * Same as recoveryStopsBefore, but called after applying the record.
 *
 * We also track the timestamp of the latest applied COMMIT/ABORT
 * record in XLogCtl->recoveryLastXTime.
 */
static bool
recoveryStopsAfter(XLogReaderState *record)
{
	uint8		info;
	uint8		xact_info;
	uint8		rmid;
	TimestampTz recordXtime = 0;

	/*
	 * Ignore recovery target settings when not in archive recovery (meaning
	 * we are in crash recovery).
	 */
	if (!ArchiveRecoveryRequested)
		return false;

	info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	rmid = XLogRecGetRmid(record);

	/*
	 * There can be many restore points that share the same name; we stop at
	 * the first one.
	 */
	if (recoveryTarget == RECOVERY_TARGET_NAME &&
		rmid == RM_XLOG_ID && info == XLOG_RESTORE_POINT)
	{
		xl_restore_point *recordRestorePointData;

		recordRestorePointData = (xl_restore_point *) XLogRecGetData(record);

		if (strcmp(recordRestorePointData->rp_name, recoveryTargetName) == 0)
		{
			recoveryStopAfter = true;
			recoveryStopXid = InvalidTransactionId;
			recoveryStopLSN = InvalidXLogRecPtr;
			(void) getRecordTimestamp(record, &recoveryStopTime);
			strlcpy(recoveryStopName, recordRestorePointData->rp_name, MAXFNAMELEN);

			ereport(LOG,
					(errmsg("recovery stopping at restore point \"%s\", time %s",
							recoveryStopName,
							timestamptz_to_str(recoveryStopTime))));
			return true;
		}
	}

	/* Check if the target LSN has been reached */
	if (recoveryTarget == RECOVERY_TARGET_LSN &&
		recoveryTargetInclusive &&
		record->ReadRecPtr >= recoveryTargetLSN)
	{
		recoveryStopAfter = true;
		recoveryStopXid = InvalidTransactionId;
		recoveryStopLSN = record->ReadRecPtr;
		recoveryStopTime = 0;
		recoveryStopName[0] = '\0';
		ereport(LOG,
				(errmsg("recovery stopping after WAL location (LSN) \"%X/%X\"",
						(uint32) (recoveryStopLSN >> 32),
						(uint32) recoveryStopLSN)));
		return true;
	}

	if (rmid != RM_XACT_ID)
		return false;

	xact_info = info & XLOG_XACT_OPMASK;

	if (xact_info == XLOG_XACT_COMMIT ||
		xact_info == XLOG_XACT_COMMIT_PREPARED ||
		xact_info == XLOG_XACT_ABORT ||
		xact_info == XLOG_XACT_ABORT_PREPARED)
	{
		TransactionId recordXid;

		/* Update the last applied transaction timestamp */
		if (getRecordTimestamp(record, &recordXtime))
			SetLatestXTime(recordXtime);

		/* Extract the XID of the committed/aborted transaction */
		if (xact_info == XLOG_XACT_COMMIT_PREPARED)
		{
			xl_xact_commit *xlrec = (xl_xact_commit *) XLogRecGetData(record);
			xl_xact_parsed_commit parsed;

			ParseCommitRecord(XLogRecGetInfo(record),
							  xlrec,
							  &parsed);
			recordXid = parsed.twophase_xid;
		}
		else if (xact_info == XLOG_XACT_ABORT_PREPARED)
		{
			xl_xact_abort *xlrec = (xl_xact_abort *) XLogRecGetData(record);
			xl_xact_parsed_abort parsed;

			ParseAbortRecord(XLogRecGetInfo(record),
							 xlrec,
							 &parsed);
			recordXid = parsed.twophase_xid;
		}
		else
			recordXid = XLogRecGetXid(record);

		/*
		 * There can be only one transaction end record with this exact
		 * transactionid
		 *
		 * when testing for an xid, we MUST test for equality only, since
		 * transactions are numbered in the order they start, not the order
		 * they complete. A higher numbered xid will complete before you about
		 * 50% of the time...
		 */
		if (recoveryTarget == RECOVERY_TARGET_XID && recoveryTargetInclusive &&
			recordXid == recoveryTargetXid)
		{
			recoveryStopAfter = true;
			recoveryStopXid = recordXid;
			recoveryStopTime = recordXtime;
			recoveryStopLSN = InvalidXLogRecPtr;
			recoveryStopName[0] = '\0';

			if (xact_info == XLOG_XACT_COMMIT ||
				xact_info == XLOG_XACT_COMMIT_PREPARED)
			{
				ereport(LOG,
						(errmsg("recovery stopping after commit of transaction %u, time %s",
								recoveryStopXid,
								timestamptz_to_str(recoveryStopTime))));
			}
			else if (xact_info == XLOG_XACT_ABORT ||
					 xact_info == XLOG_XACT_ABORT_PREPARED)
			{
				ereport(LOG,
						(errmsg("recovery stopping after abort of transaction %u, time %s",
								recoveryStopXid,
								timestamptz_to_str(recoveryStopTime))));
			}
			return true;
		}
	}

	/* Check if we should stop as soon as reaching consistency */
	if (recoveryTarget == RECOVERY_TARGET_IMMEDIATE && reachedConsistency)
	{
		ereport(LOG,
				(errmsg("recovery stopping after reaching consistency")));

		recoveryStopAfter = true;
		recoveryStopXid = InvalidTransactionId;
		recoveryStopTime = 0;
		recoveryStopLSN = InvalidXLogRecPtr;
		recoveryStopName[0] = '\0';
		return true;
	}

	return false;
}

/*
 * Wait until shared recoveryPause flag is cleared.
 *
 * endOfRecovery is true if the recovery target is reached and
 * the paused state starts at the end of recovery because of
 * recovery_target_action=pause, and false otherwise.
 *
 * XXX Could also be done with shared latch, avoiding the pg_usleep loop.
 * Probably not worth the trouble though.  This state shouldn't be one that
 * anyone cares about server power consumption in.
 */
static void
recoveryPausesHere(bool endOfRecovery)
{
	/* Don't pause unless users can connect! */
	if (!LocalHotStandbyActive)
		return;

	/* Don't pause after standby promotion has been triggered */
	if (LocalPromoteIsTriggered)
		return;

	if (endOfRecovery)
		ereport(LOG,
				(errmsg("pausing at the end of recovery"),
				 errhint("Execute pg_wal_replay_resume() to promote.")));
	else
		ereport(LOG,
				(errmsg("recovery has paused"),
				 errhint("Execute pg_wal_replay_resume() to continue.")));

	while (RecoveryIsPaused())
	{
		HandleStartupProcInterrupts();
		if (CheckForStandbyTrigger())
			return;
		pgstat_report_wait_start(WAIT_EVENT_RECOVERY_PAUSE);
		pg_usleep(1000000L);	/* 1000 ms */
		pgstat_report_wait_end();
	}
}

bool
RecoveryIsPaused(void)
{
	bool		recoveryPause;

	SpinLockAcquire(&XLogCtl->info_lck);
	recoveryPause = XLogCtl->recoveryPause;
	SpinLockRelease(&XLogCtl->info_lck);

	return recoveryPause;
}

void
SetRecoveryPause(bool recoveryPause)
{
	SpinLockAcquire(&XLogCtl->info_lck);
	XLogCtl->recoveryPause = recoveryPause;
	SpinLockRelease(&XLogCtl->info_lck);
}

/*
 * When recovery_min_apply_delay is set, we wait long enough to make sure
 * certain record types are applied at least that interval behind the master.
 *
 * Returns true if we waited.
 *
 * Note that the delay is calculated between the WAL record log time and
 * the current time on standby. We would prefer to keep track of when this
 * standby received each WAL record, which would allow a more consistent
 * approach and one not affected by time synchronisation issues, but that
 * is significantly more effort and complexity for little actual gain in
 * usability.
 */
static bool
recoveryApplyDelay(XLogReaderState *record)
{
	uint8		xact_info;
	TimestampTz xtime;
	TimestampTz delayUntil;
	long		msecs;

	/* nothing to do if no delay configured */
	if (recovery_min_apply_delay <= 0)
		return false;

	/* no delay is applied on a database not yet consistent */
	if (!reachedConsistency)
		return false;

	/* nothing to do if crash recovery is requested */
	if (!ArchiveRecoveryRequested)
		return false;

	/*
	 * Is it a COMMIT record?
	 *
	 * We deliberately choose not to delay aborts since they have no effect on
	 * MVCC. We already allow replay of records that don't have a timestamp,
	 * so there is already opportunity for issues caused by early conflicts on
	 * standbys.
	 */
	if (XLogRecGetRmid(record) != RM_XACT_ID)
		return false;

	xact_info = XLogRecGetInfo(record) & XLOG_XACT_OPMASK;

	if (xact_info != XLOG_XACT_COMMIT &&
		xact_info != XLOG_XACT_COMMIT_PREPARED)
		return false;

	if (!getRecordTimestamp(record, &xtime))
		return false;

	delayUntil = TimestampTzPlusMilliseconds(xtime, recovery_min_apply_delay);

	/*
	 * Exit without arming the latch if it's already past time to apply this
	 * record
	 */
	msecs = TimestampDifferenceMilliseconds(GetCurrentTimestamp(), delayUntil);
	if (msecs <= 0)
		return false;

	while (true)
	{
		ResetLatch(&XLogCtl->recoveryWakeupLatch);

		/*
		 * This might change recovery_min_apply_delay or the trigger file's
		 * location.
		 */
		HandleStartupProcInterrupts();

		if (CheckForStandbyTrigger())
			break;

		/*
		 * Recalculate delayUntil as recovery_min_apply_delay could have
		 * changed while waiting in this loop.
		 */
		delayUntil = TimestampTzPlusMilliseconds(xtime, recovery_min_apply_delay);

		/*
		 * Wait for difference between GetCurrentTimestamp() and delayUntil.
		 */
		msecs = TimestampDifferenceMilliseconds(GetCurrentTimestamp(),
												delayUntil);

		if (msecs <= 0)
			break;

		elog(DEBUG2, "recovery apply delay %ld milliseconds", msecs);

		(void) WaitLatch(&XLogCtl->recoveryWakeupLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 msecs,
						 WAIT_EVENT_RECOVERY_APPLY_DELAY);
	}
	return true;
}

/*
 * Save timestamp of latest processed commit/abort record.
 *
 * We keep this in XLogCtl, not a simple static variable, so that it can be
 * seen by processes other than the startup process.  Note in particular
 * that CreateRestartPoint is executed in the checkpointer.
 */
static void
SetLatestXTime(TimestampTz xtime)
{
	SpinLockAcquire(&XLogCtl->info_lck);
	XLogCtl->recoveryLastXTime = xtime;
	SpinLockRelease(&XLogCtl->info_lck);
}

/*
 * Fetch timestamp of latest processed commit/abort record.
 */
TimestampTz
GetLatestXTime(void)
{
	TimestampTz xtime;

	SpinLockAcquire(&XLogCtl->info_lck);
	xtime = XLogCtl->recoveryLastXTime;
	SpinLockRelease(&XLogCtl->info_lck);

	return xtime;
}

/*
 * Save timestamp of the next chunk of WAL records to apply.
 *
 * We keep this in XLogCtl, not a simple static variable, so that it can be
 * seen by all backends.
 */
static void
SetCurrentChunkStartTime(TimestampTz xtime)
{
	SpinLockAcquire(&XLogCtl->info_lck);
	XLogCtl->currentChunkStartTime = xtime;
	SpinLockRelease(&XLogCtl->info_lck);
}

/*
 * Fetch timestamp of latest processed commit/abort record.
 * Startup process maintains an accurate local copy in XLogReceiptTime
 */
TimestampTz
GetCurrentChunkReplayStartTime(void)
{
	TimestampTz xtime;

	SpinLockAcquire(&XLogCtl->info_lck);
	xtime = XLogCtl->currentChunkStartTime;
	SpinLockRelease(&XLogCtl->info_lck);

	return xtime;
}

/*
 * Returns time of receipt of current chunk of XLOG data, as well as
 * whether it was received from streaming replication or from archives.
 */
void
GetXLogReceiptTime(TimestampTz *rtime, bool *fromStream)
{
	/*
	 * This must be executed in the startup process, since we don't export the
	 * relevant state to shared memory.
	 */
	Assert(InRecovery);

	*rtime = XLogReceiptTime;
	*fromStream = (XLogReceiptSource == XLOG_FROM_STREAM);
}

/*
 * Note that text field supplied is a parameter name and does not require
 * translation
 */
#define RecoveryRequiresIntParameter(param_name, currValue, minValue) \
do { \
	if ((currValue) < (minValue)) \
		ereport(ERROR, \
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE), \
				 errmsg("hot standby is not possible because %s = %d is a lower setting than on the master server (its value was %d)", \
						param_name, \
						currValue, \
						minValue))); \
} while(0)

/*
 * Check to see if required parameters are set high enough on this server
 * for various aspects of recovery operation.
 *
 * Note that all the parameters which this function tests need to be
 * listed in Administrator's Overview section in high-availability.sgml.
 * If you change them, don't forget to update the list.
 */
static void
CheckRequiredParameterValues(void)
{
	/*
	 * For archive recovery, the WAL must be generated with at least 'replica'
	 * wal_level.
	 */
	if (ArchiveRecoveryRequested && ControlFile->wal_level == WAL_LEVEL_MINIMAL)
	{
		ereport(WARNING,
				(errmsg("WAL was generated with wal_level=minimal, data may be missing"),
				 errhint("This happens if you temporarily set wal_level=minimal without taking a new base backup.")));
	}

	/*
	 * For Hot Standby, the WAL must be generated with 'replica' mode, and we
	 * must have at least as many backend slots as the primary.
	 */
	if (ArchiveRecoveryRequested && EnableHotStandby)
	{
		if (ControlFile->wal_level < WAL_LEVEL_REPLICA)
			ereport(ERROR,
					(errmsg("hot standby is not possible because wal_level was not set to \"replica\" or higher on the master server"),
					 errhint("Either set wal_level to \"replica\" on the master, or turn off hot_standby here.")));

		/* We ignore autovacuum_max_workers when we make this test. */
		RecoveryRequiresIntParameter("max_connections",
									 MaxConnections,
									 ControlFile->MaxConnections);
		RecoveryRequiresIntParameter("max_worker_processes",
									 max_worker_processes,
									 ControlFile->max_worker_processes);
		RecoveryRequiresIntParameter("max_wal_senders",
									 max_wal_senders,
									 ControlFile->max_wal_senders);
		RecoveryRequiresIntParameter("max_prepared_transactions",
									 max_prepared_xacts,
									 ControlFile->max_prepared_xacts);
		RecoveryRequiresIntParameter("max_locks_per_transaction",
									 max_locks_per_xact,
									 ControlFile->max_locks_per_xact);
	}
}

/*
 * This must be called ONCE during postmaster or standalone-backend startup
 */
void
StartupXLOG(void)
{
	XLogCtlInsert *Insert;
	CheckPoint	checkPoint;
	bool		wasShutdown;
	bool		reachedRecoveryTarget = false;
	bool		haveBackupLabel = false;
	bool		haveTblspcMap = false;
	XLogRecPtr	RecPtr,
				checkPointLoc,
				EndOfLog;
	TimeLineID	EndOfLogTLI;
	TimeLineID	PrevTimeLineID;
	XLogRecord *record;
	TransactionId oldestActiveXID;
	bool		backupEndRequired = false;
	bool		backupFromStandby = false;
	DBState		dbstate_at_startup;
	XLogReaderState *xlogreader;
	XLogPageReadPrivate private;
	bool		fast_promoted = false;
	struct stat st;

	/*
	 * We should have an aux process resource owner to use, and we should not
	 * be in a transaction that's installed some other resowner.
	 */
	Assert(AuxProcessResourceOwner != NULL);
	Assert(CurrentResourceOwner == NULL ||
		   CurrentResourceOwner == AuxProcessResourceOwner);
	CurrentResourceOwner = AuxProcessResourceOwner;

	/*
	 * Check that contents look valid.
	 */
	if (!XRecOffIsValid(ControlFile->checkPoint))
		ereport(FATAL,
				(errmsg("control file contains invalid checkpoint location")));

	switch (ControlFile->state)
	{
		case DB_SHUTDOWNED:

			/*
			 * This is the expected case, so don't be chatty in standalone
			 * mode
			 */
			ereport(IsPostmasterEnvironment ? LOG : NOTICE,
					(errmsg("database system was shut down at %s",
							str_time(ControlFile->time))));
			break;

		case DB_SHUTDOWNED_IN_RECOVERY:
			ereport(LOG,
					(errmsg("database system was shut down in recovery at %s",
							str_time(ControlFile->time))));
			break;

		case DB_SHUTDOWNING:
			ereport(LOG,
					(errmsg("database system shutdown was interrupted; last known up at %s",
							str_time(ControlFile->time))));
			break;

		case DB_IN_CRASH_RECOVERY:
			ereport(LOG,
					(errmsg("database system was interrupted while in recovery at %s",
							str_time(ControlFile->time)),
					 errhint("This probably means that some data is corrupted and"
							 " you will have to use the last backup for recovery.")));
			break;

		case DB_IN_ARCHIVE_RECOVERY:
			ereport(LOG,
					(errmsg("database system was interrupted while in recovery at log time %s",
							str_time(ControlFile->checkPointCopy.time)),
					 errhint("If this has occurred more than once some data might be corrupted"
							 " and you might need to choose an earlier recovery target.")));
			break;

		case DB_IN_PRODUCTION:
			ereport(LOG,
					(errmsg("database system was interrupted; last known up at %s",
							str_time(ControlFile->time))));
			break;

		default:
			ereport(FATAL,
					(errmsg("control file contains invalid database cluster state")));
	}

	/* This is just to allow attaching to startup process with a debugger */
#ifdef XLOG_REPLAY_DELAY
	if (ControlFile->state != DB_SHUTDOWNED)
		pg_usleep(60000000L);
#endif

	/*
	 * Verify that pg_wal and pg_wal/archive_status exist.  In cases where
	 * someone has performed a copy for PITR, these directories may have been
	 * excluded and need to be re-created.
	 */
	ValidateXLOGDirectoryStructure();

	/*----------
	 * If we previously crashed, perform a couple of actions:
	 *
	 * - The pg_wal directory may still include some temporary WAL segments
	 *   used when creating a new segment, so perform some clean up to not
	 *   bloat this path.  This is done first as there is no point to sync
	 *   this temporary data.
	 *
	 * - There might be data which we had written, intending to fsync it, but
	 *   which we had not actually fsync'd yet.  Therefore, a power failure in
	 *   the near future might cause earlier unflushed writes to be lost, even
	 *   though more recent data written to disk from here on would be
	 *   persisted.  To avoid that, fsync the entire data directory.
	 */
	if (ControlFile->state != DB_SHUTDOWNED &&
		ControlFile->state != DB_SHUTDOWNED_IN_RECOVERY)
	{
		RemoveTempXlogFiles();
		SyncDataDirectory();
	}

	/*
	 * Initialize on the assumption we want to recover to the latest timeline
	 * that's active according to pg_control.
	 */
	if (ControlFile->minRecoveryPointTLI >
		ControlFile->checkPointCopy.ThisTimeLineID)
		recoveryTargetTLI = ControlFile->minRecoveryPointTLI;
	else
		recoveryTargetTLI = ControlFile->checkPointCopy.ThisTimeLineID;

	/*
	 * Check for signal files, and if so set up state for offline recovery
	 */
	readRecoverySignalFile();
	validateRecoveryParameters();

	if (ArchiveRecoveryRequested)
	{
		if (StandbyModeRequested)
			ereport(LOG,
					(errmsg("entering standby mode")));
		else if (recoveryTarget == RECOVERY_TARGET_XID)
			ereport(LOG,
					(errmsg("starting point-in-time recovery to XID %u",
							recoveryTargetXid)));
		else if (recoveryTarget == RECOVERY_TARGET_TIME)
			ereport(LOG,
					(errmsg("starting point-in-time recovery to %s",
							timestamptz_to_str(recoveryTargetTime))));
		else if (recoveryTarget == RECOVERY_TARGET_NAME)
			ereport(LOG,
					(errmsg("starting point-in-time recovery to \"%s\"",
							recoveryTargetName)));
		else if (recoveryTarget == RECOVERY_TARGET_LSN)
			ereport(LOG,
					(errmsg("starting point-in-time recovery to WAL location (LSN) \"%X/%X\"",
							(uint32) (recoveryTargetLSN >> 32),
							(uint32) recoveryTargetLSN)));
		else if (recoveryTarget == RECOVERY_TARGET_IMMEDIATE)
			ereport(LOG,
					(errmsg("starting point-in-time recovery to earliest consistent point")));
		else
			ereport(LOG,
					(errmsg("starting archive recovery")));
	}

	/*
	 * Take ownership of the wakeup latch if we're going to sleep during
	 * recovery.
	 */
	if (ArchiveRecoveryRequested)
		OwnLatch(&XLogCtl->recoveryWakeupLatch);

	/* Set up XLOG reader facility */
	MemSet(&private, 0, sizeof(XLogPageReadPrivate));
	xlogreader =
		XLogReaderAllocate(wal_segment_size, NULL,
						   XL_ROUTINE(.page_read = &XLogPageRead,
									  .segment_open = NULL,
									  .segment_close = wal_segment_close),
						   &private);
	if (!xlogreader)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while allocating a WAL reading processor.")));
	xlogreader->system_identifier = ControlFile->system_identifier;

	/*
	 * Allocate two page buffers dedicated to WAL consistency checks.  We do
	 * it this way, rather than just making static arrays, for two reasons:
	 * (1) no need to waste the storage in most instantiations of the backend;
	 * (2) a static char array isn't guaranteed to have any particular
	 * alignment, whereas palloc() will provide MAXALIGN'd storage.
	 */
	replay_image_masked = (char *) palloc(BLCKSZ);
	master_image_masked = (char *) palloc(BLCKSZ);

	if (read_backup_label(&checkPointLoc, &backupEndRequired,
						  &backupFromStandby))
	{
		List	   *tablespaces = NIL;

		/*
		 * Archive recovery was requested, and thanks to the backup label
		 * file, we know how far we need to replay to reach consistency. Enter
		 * archive recovery directly.
		 */
		InArchiveRecovery = true;
		if (StandbyModeRequested)
			StandbyMode = true;

		/*
		 * When a backup_label file is present, we want to roll forward from
		 * the checkpoint it identifies, rather than using pg_control.
		 */
		record = ReadCheckpointRecord(xlogreader, checkPointLoc, 0, true);
		if (record != NULL)
		{
			memcpy(&checkPoint, XLogRecGetData(xlogreader), sizeof(CheckPoint));
			wasShutdown = ((record->xl_info & ~XLR_INFO_MASK) == XLOG_CHECKPOINT_SHUTDOWN);
			ereport(DEBUG1,
					(errmsg("checkpoint record is at %X/%X",
							(uint32) (checkPointLoc >> 32), (uint32) checkPointLoc)));
			InRecovery = true;	/* force recovery even if SHUTDOWNED */

			/*
			 * Make sure that REDO location exists. This may not be the case
			 * if there was a crash during an online backup, which left a
			 * backup_label around that references a WAL segment that's
			 * already been archived.
			 */
			if (checkPoint.redo < checkPointLoc)
			{
				XLogBeginRead(xlogreader, checkPoint.redo);
				if (!ReadRecord(xlogreader, LOG, false))
					ereport(FATAL,
							(errmsg("could not find redo location referenced by checkpoint record"),
							 errhint("If you are restoring from a backup, touch \"%s/recovery.signal\" and add required recovery options.\n"
									 "If you are not restoring from a backup, try removing the file \"%s/backup_label\".\n"
									 "Be careful: removing \"%s/backup_label\" will result in a corrupt cluster if restoring from a backup.",
									 DataDir, DataDir, DataDir)));
			}
		}
		else
		{
			ereport(FATAL,
					(errmsg("could not locate required checkpoint record"),
					 errhint("If you are restoring from a backup, touch \"%s/recovery.signal\" and add required recovery options.\n"
							 "If you are not restoring from a backup, try removing the file \"%s/backup_label\".\n"
							 "Be careful: removing \"%s/backup_label\" will result in a corrupt cluster if restoring from a backup.",
							 DataDir, DataDir, DataDir)));
			wasShutdown = false;	/* keep compiler quiet */
		}

		/* read the tablespace_map file if present and create symlinks. */
		if (read_tablespace_map(&tablespaces))
		{
			ListCell   *lc;

			foreach(lc, tablespaces)
			{
				tablespaceinfo *ti = lfirst(lc);
				char	   *linkloc;

				linkloc = psprintf("pg_tblspc/%s", ti->oid);

				/*
				 * Remove the existing symlink if any and Create the symlink
				 * under PGDATA.
				 */
				remove_tablespace_symlink(linkloc);

				if (symlink(ti->path, linkloc) < 0)
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not create symbolic link \"%s\": %m",
									linkloc)));

				pfree(ti->oid);
				pfree(ti->path);
				pfree(ti);
			}

			/* set flag to delete it later */
			haveTblspcMap = true;
		}

		/* set flag to delete it later */
		haveBackupLabel = true;
	}
	else
	{
		/*
		 * If tablespace_map file is present without backup_label file, there
		 * is no use of such file.  There is no harm in retaining it, but it
		 * is better to get rid of the map file so that we don't have any
		 * redundant file in data directory and it will avoid any sort of
		 * confusion.  It seems prudent though to just rename the file out of
		 * the way rather than delete it completely, also we ignore any error
		 * that occurs in rename operation as even if map file is present
		 * without backup_label file, it is harmless.
		 */
		if (stat(TABLESPACE_MAP, &st) == 0)
		{
			unlink(TABLESPACE_MAP_OLD);
			if (durable_rename(TABLESPACE_MAP, TABLESPACE_MAP_OLD, DEBUG1) == 0)
				ereport(LOG,
						(errmsg("ignoring file \"%s\" because no file \"%s\" exists",
								TABLESPACE_MAP, BACKUP_LABEL_FILE),
						 errdetail("File \"%s\" was renamed to \"%s\".",
								   TABLESPACE_MAP, TABLESPACE_MAP_OLD)));
			else
				ereport(LOG,
						(errmsg("ignoring file \"%s\" because no file \"%s\" exists",
								TABLESPACE_MAP, BACKUP_LABEL_FILE),
						 errdetail("Could not rename file \"%s\" to \"%s\": %m.",
								   TABLESPACE_MAP, TABLESPACE_MAP_OLD)));
		}

		/*
		 * It's possible that archive recovery was requested, but we don't
		 * know how far we need to replay the WAL before we reach consistency.
		 * This can happen for example if a base backup is taken from a
		 * running server using an atomic filesystem snapshot, without calling
		 * pg_start/stop_backup. Or if you just kill a running master server
		 * and put it into archive recovery by creating a recovery signal
		 * file.
		 *
		 * Our strategy in that case is to perform crash recovery first,
		 * replaying all the WAL present in pg_wal, and only enter archive
		 * recovery after that.
		 *
		 * But usually we already know how far we need to replay the WAL (up
		 * to minRecoveryPoint, up to backupEndPoint, or until we see an
		 * end-of-backup record), and we can enter archive recovery directly.
		 */
		if (ArchiveRecoveryRequested &&
			(ControlFile->minRecoveryPoint != InvalidXLogRecPtr ||
			 ControlFile->backupEndRequired ||
			 ControlFile->backupEndPoint != InvalidXLogRecPtr ||
			 ControlFile->state == DB_SHUTDOWNED))
		{
			InArchiveRecovery = true;
			if (StandbyModeRequested)
				StandbyMode = true;
		}

		/* Get the last valid checkpoint record. */
		checkPointLoc = ControlFile->checkPoint;
		RedoStartLSN = ControlFile->checkPointCopy.redo;
		record = ReadCheckpointRecord(xlogreader, checkPointLoc, 1, true);
		if (record != NULL)
		{
			ereport(DEBUG1,
					(errmsg("checkpoint record is at %X/%X",
							(uint32) (checkPointLoc >> 32), (uint32) checkPointLoc)));
		}
		else
		{
			/*
			 * We used to attempt to go back to a secondary checkpoint record
			 * here, but only when not in standby mode. We now just fail if we
			 * can't read the last checkpoint because this allows us to
			 * simplify processing around checkpoints.
			 */
			ereport(PANIC,
					(errmsg("could not locate a valid checkpoint record")));
		}
		memcpy(&checkPoint, XLogRecGetData(xlogreader), sizeof(CheckPoint));
		wasShutdown = ((record->xl_info & ~XLR_INFO_MASK) == XLOG_CHECKPOINT_SHUTDOWN);
	}

	/*
	 * Clear out any old relcache cache files.  This is *necessary* if we do
	 * any WAL replay, since that would probably result in the cache files
	 * being out of sync with database reality.  In theory we could leave them
	 * in place if the database had been cleanly shut down, but it seems
	 * safest to just remove them always and let them be rebuilt during the
	 * first backend startup.  These files needs to be removed from all
	 * directories including pg_tblspc, however the symlinks are created only
	 * after reading tablespace_map file in case of archive recovery from
	 * backup, so needs to clear old relcache files here after creating
	 * symlinks.
	 */
	RelationCacheInitFileRemove();

	/*
	 * If the location of the checkpoint record is not on the expected
	 * timeline in the history of the requested timeline, we cannot proceed:
	 * the backup is not part of the history of the requested timeline.
	 */
	Assert(expectedTLEs);		/* was initialized by reading checkpoint
								 * record */
	if (tliOfPointInHistory(checkPointLoc, expectedTLEs) !=
		checkPoint.ThisTimeLineID)
	{
		XLogRecPtr	switchpoint;

		/*
		 * tliSwitchPoint will throw an error if the checkpoint's timeline is
		 * not in expectedTLEs at all.
		 */
		switchpoint = tliSwitchPoint(ControlFile->checkPointCopy.ThisTimeLineID, expectedTLEs, NULL);
		ereport(FATAL,
				(errmsg("requested timeline %u is not a child of this server's history",
						recoveryTargetTLI),
				 errdetail("Latest checkpoint is at %X/%X on timeline %u, but in the history of the requested timeline, the server forked off from that timeline at %X/%X.",
						   (uint32) (ControlFile->checkPoint >> 32),
						   (uint32) ControlFile->checkPoint,
						   ControlFile->checkPointCopy.ThisTimeLineID,
						   (uint32) (switchpoint >> 32),
						   (uint32) switchpoint)));
	}

	/*
	 * The min recovery point should be part of the requested timeline's
	 * history, too.
	 */
	if (!XLogRecPtrIsInvalid(ControlFile->minRecoveryPoint) &&
		tliOfPointInHistory(ControlFile->minRecoveryPoint - 1, expectedTLEs) !=
		ControlFile->minRecoveryPointTLI)
		ereport(FATAL,
				(errmsg("requested timeline %u does not contain minimum recovery point %X/%X on timeline %u",
						recoveryTargetTLI,
						(uint32) (ControlFile->minRecoveryPoint >> 32),
						(uint32) ControlFile->minRecoveryPoint,
						ControlFile->minRecoveryPointTLI)));

	LastRec = RecPtr = checkPointLoc;

	ereport(DEBUG1,
			(errmsg_internal("redo record is at %X/%X; shutdown %s",
							 (uint32) (checkPoint.redo >> 32), (uint32) checkPoint.redo,
							 wasShutdown ? "true" : "false")));
	ereport(DEBUG1,
			(errmsg_internal("next transaction ID: " UINT64_FORMAT "; next OID: %u",
							 U64FromFullTransactionId(checkPoint.nextFullXid),
							 checkPoint.nextOid)));
	ereport(DEBUG1,
			(errmsg_internal("next MultiXactId: %u; next MultiXactOffset: %u",
							 checkPoint.nextMulti, checkPoint.nextMultiOffset)));
	ereport(DEBUG1,
			(errmsg_internal("oldest unfrozen transaction ID: %u, in database %u",
							 checkPoint.oldestXid, checkPoint.oldestXidDB)));
	ereport(DEBUG1,
			(errmsg_internal("oldest MultiXactId: %u, in database %u",
							 checkPoint.oldestMulti, checkPoint.oldestMultiDB)));
	ereport(DEBUG1,
			(errmsg_internal("commit timestamp Xid oldest/newest: %u/%u",
							 checkPoint.oldestCommitTsXid,
							 checkPoint.newestCommitTsXid)));
	if (!TransactionIdIsNormal(XidFromFullTransactionId(checkPoint.nextFullXid)))
		ereport(PANIC,
				(errmsg("invalid next transaction ID")));

	/* initialize shared memory variables from the checkpoint record */
	ShmemVariableCache->nextFullXid = checkPoint.nextFullXid;
	ShmemVariableCache->nextOid = checkPoint.nextOid;
	ShmemVariableCache->oidCount = 0;
	MultiXactSetNextMXact(checkPoint.nextMulti, checkPoint.nextMultiOffset);
	AdvanceOldestClogXid(checkPoint.oldestXid);
	SetTransactionIdLimit(checkPoint.oldestXid, checkPoint.oldestXidDB);
	SetMultiXactIdLimit(checkPoint.oldestMulti, checkPoint.oldestMultiDB, true);
	SetCommitTsLimit(checkPoint.oldestCommitTsXid,
					 checkPoint.newestCommitTsXid);
	XLogCtl->ckptFullXid = checkPoint.nextFullXid;

	/*
	 * Initialize replication slots, before there's a chance to remove
	 * required resources.
	 */
	StartupReplicationSlots();

	/*
	 * Startup logical state, needs to be setup now so we have proper data
	 * during crash recovery.
	 */
	StartupReorderBuffer();

	/*
	 * Startup MultiXact. We need to do this early to be able to replay
	 * truncations.
	 */
	StartupMultiXact();

	/*
	 * Ditto for commit timestamps.  Activate the facility if the setting is
	 * enabled in the control file, as there should be no tracking of commit
	 * timestamps done when the setting was disabled.  This facility can be
	 * started or stopped when replaying a XLOG_PARAMETER_CHANGE record.
	 */
	if (ControlFile->track_commit_timestamp)
		StartupCommitTs();

	/*
	 * Recover knowledge about replay progress of known replication partners.
	 */
	StartupReplicationOrigin();

	/*
	 * Initialize unlogged LSN. On a clean shutdown, it's restored from the
	 * control file. On recovery, all unlogged relations are blown away, so
	 * the unlogged LSN counter can be reset too.
	 */
	if (ControlFile->state == DB_SHUTDOWNED)
		XLogCtl->unloggedLSN = ControlFile->unloggedLSN;
	else
		XLogCtl->unloggedLSN = FirstNormalUnloggedLSN;

	/*
	 * We must replay WAL entries using the same TimeLineID they were created
	 * under, so temporarily adopt the TLI indicated by the checkpoint (see
	 * also xlog_redo()).
	 */
	ThisTimeLineID = checkPoint.ThisTimeLineID;

	/*
	 * Copy any missing timeline history files between 'now' and the recovery
	 * target timeline from archive to pg_wal. While we don't need those files
	 * ourselves - the history file of the recovery target timeline covers all
	 * the previous timelines in the history too - a cascading standby server
	 * might be interested in them. Or, if you archive the WAL from this
	 * server to a different archive than the master, it'd be good for all the
	 * history files to get archived there after failover, so that you can use
	 * one of the old timelines as a PITR target. Timeline history files are
	 * small, so it's better to copy them unnecessarily than not copy them and
	 * regret later.
	 */
	restoreTimeLineHistoryFiles(ThisTimeLineID, recoveryTargetTLI);

	/*
	 * Before running in recovery, scan pg_twophase and fill in its status to
	 * be able to work on entries generated by redo.  Doing a scan before
	 * taking any recovery action has the merit to discard any 2PC files that
	 * are newer than the first record to replay, saving from any conflicts at
	 * replay.  This avoids as well any subsequent scans when doing recovery
	 * of the on-disk two-phase data.
	 */
	restoreTwoPhaseData();

	lastFullPageWrites = checkPoint.fullPageWrites;

	RedoRecPtr = XLogCtl->RedoRecPtr = XLogCtl->Insert.RedoRecPtr = checkPoint.redo;
	doPageWrites = lastFullPageWrites;

	if (RecPtr < checkPoint.redo)
		ereport(PANIC,
				(errmsg("invalid redo in checkpoint record")));

	/*
	 * Check whether we need to force recovery from WAL.  If it appears to
	 * have been a clean shutdown and we did not have a recovery signal file,
	 * then assume no recovery needed.
	 */
	if (checkPoint.redo < RecPtr)
	{
		if (wasShutdown)
			ereport(PANIC,
					(errmsg("invalid redo record in shutdown checkpoint")));
		InRecovery = true;
	}
	else if (ControlFile->state != DB_SHUTDOWNED)
		InRecovery = true;
	else if (ArchiveRecoveryRequested)
	{
		/* force recovery due to presence of recovery signal file */
		InRecovery = true;
	}

	/*
	 * Start recovery assuming that the final record isn't lost.
	 */
	abortedRecPtr = InvalidXLogRecPtr;
	missingContrecPtr = InvalidXLogRecPtr;

	/* REDO */
	if (InRecovery)
	{
		int			rmid;

		/*
		 * Update pg_control to show that we are recovering and to show the
		 * selected checkpoint as the place we are starting from. We also mark
		 * pg_control with any minimum recovery stop point obtained from a
		 * backup history file.
		 */
		dbstate_at_startup = ControlFile->state;
		if (InArchiveRecovery)
		{
			ControlFile->state = DB_IN_ARCHIVE_RECOVERY;

			SpinLockAcquire(&XLogCtl->info_lck);
			XLogCtl->SharedRecoveryState = RECOVERY_STATE_ARCHIVE;
			SpinLockRelease(&XLogCtl->info_lck);
		}
		else
		{
			ereport(LOG,
					(errmsg("database system was not properly shut down; "
							"automatic recovery in progress")));
			if (recoveryTargetTLI > ControlFile->checkPointCopy.ThisTimeLineID)
				ereport(LOG,
						(errmsg("crash recovery starts in timeline %u "
								"and has target timeline %u",
								ControlFile->checkPointCopy.ThisTimeLineID,
								recoveryTargetTLI)));
			ControlFile->state = DB_IN_CRASH_RECOVERY;

			SpinLockAcquire(&XLogCtl->info_lck);
			XLogCtl->SharedRecoveryState = RECOVERY_STATE_CRASH;
			SpinLockRelease(&XLogCtl->info_lck);
		}
		ControlFile->checkPoint = checkPointLoc;
		ControlFile->checkPointCopy = checkPoint;
		if (InArchiveRecovery)
		{
			/* initialize minRecoveryPoint if not set yet */
			if (ControlFile->minRecoveryPoint < checkPoint.redo)
			{
				ControlFile->minRecoveryPoint = checkPoint.redo;
				ControlFile->minRecoveryPointTLI = checkPoint.ThisTimeLineID;
			}
		}

		/*
		 * Set backupStartPoint if we're starting recovery from a base backup.
		 *
		 * Also set backupEndPoint and use minRecoveryPoint as the backup end
		 * location if we're starting recovery from a base backup which was
		 * taken from a standby. In this case, the database system status in
		 * pg_control must indicate that the database was already in recovery.
		 * Usually that will be DB_IN_ARCHIVE_RECOVERY but also can be
		 * DB_SHUTDOWNED_IN_RECOVERY if recovery previously was interrupted
		 * before reaching this point; e.g. because restore_command or
		 * primary_conninfo were faulty.
		 *
		 * Any other state indicates that the backup somehow became corrupted
		 * and we can't sensibly continue with recovery.
		 */
		if (haveBackupLabel)
		{
			ControlFile->backupStartPoint = checkPoint.redo;
			ControlFile->backupEndRequired = backupEndRequired;

			if (backupFromStandby)
			{
				if (dbstate_at_startup != DB_IN_ARCHIVE_RECOVERY &&
					dbstate_at_startup != DB_SHUTDOWNED_IN_RECOVERY)
					ereport(FATAL,
							(errmsg("backup_label contains data inconsistent with control file"),
							 errhint("This means that the backup is corrupted and you will "
									 "have to use another backup for recovery.")));
				ControlFile->backupEndPoint = ControlFile->minRecoveryPoint;
			}
		}
		ControlFile->time = (pg_time_t) time(NULL);
		/* No need to hold ControlFileLock yet, we aren't up far enough */
		UpdateControlFile();

		/*
		 * Initialize our local copy of minRecoveryPoint.  When doing crash
		 * recovery we want to replay up to the end of WAL.  Particularly, in
		 * the case of a promoted standby minRecoveryPoint value in the
		 * control file is only updated after the first checkpoint.  However,
		 * if the instance crashes before the first post-recovery checkpoint
		 * is completed then recovery will use a stale location causing the
		 * startup process to think that there are still invalid page
		 * references when checking for data consistency.
		 */
		if (InArchiveRecovery)
		{
			minRecoveryPoint = ControlFile->minRecoveryPoint;
			minRecoveryPointTLI = ControlFile->minRecoveryPointTLI;
		}
		else
		{
			minRecoveryPoint = InvalidXLogRecPtr;
			minRecoveryPointTLI = 0;
		}

		/*
		 * Reset pgstat data, because it may be invalid after recovery.
		 */
		pgstat_reset_all();

		/*
		 * If there was a backup label file, it's done its job and the info
		 * has now been propagated into pg_control.  We must get rid of the
		 * label file so that if we crash during recovery, we'll pick up at
		 * the latest recovery restartpoint instead of going all the way back
		 * to the backup start point.  It seems prudent though to just rename
		 * the file out of the way rather than delete it completely.
		 */
		if (haveBackupLabel)
		{
			unlink(BACKUP_LABEL_OLD);
			durable_rename(BACKUP_LABEL_FILE, BACKUP_LABEL_OLD, FATAL);
		}

		/*
		 * If there was a tablespace_map file, it's done its job and the
		 * symlinks have been created.  We must get rid of the map file so
		 * that if we crash during recovery, we don't create symlinks again.
		 * It seems prudent though to just rename the file out of the way
		 * rather than delete it completely.
		 */
		if (haveTblspcMap)
		{
			unlink(TABLESPACE_MAP_OLD);
			durable_rename(TABLESPACE_MAP, TABLESPACE_MAP_OLD, FATAL);
		}

		/* Check that the GUCs used to generate the WAL allow recovery */
		CheckRequiredParameterValues();

		/*
		 * We're in recovery, so unlogged relations may be trashed and must be
		 * reset.  This should be done BEFORE allowing Hot Standby
		 * connections, so that read-only backends don't try to read whatever
		 * garbage is left over from before.
		 */
		ResetUnloggedRelations(UNLOGGED_RELATION_CLEANUP);

		/*
		 * Likewise, delete any saved transaction snapshot files that got left
		 * behind by crashed backends.
		 */
		DeleteAllExportedSnapshotFiles();

		/*
		 * Initialize for Hot Standby, if enabled. We won't let backends in
		 * yet, not until we've reached the min recovery point specified in
		 * control file and we've established a recovery snapshot from a
		 * running-xacts WAL record.
		 */
		if (ArchiveRecoveryRequested && EnableHotStandby)
		{
			TransactionId *xids;
			int			nxids;

			ereport(DEBUG1,
					(errmsg("initializing for hot standby")));

			InitRecoveryTransactionEnvironment();

			if (wasShutdown)
				oldestActiveXID = PrescanPreparedTransactions(&xids, &nxids);
			else
				oldestActiveXID = checkPoint.oldestActiveXid;
			Assert(TransactionIdIsValid(oldestActiveXID));

			/* Tell procarray about the range of xids it has to deal with */
			ProcArrayInitRecovery(XidFromFullTransactionId(ShmemVariableCache->nextFullXid));

			/*
			 * Startup commit log and subtrans only.  MultiXact and commit
			 * timestamp have already been started up and other SLRUs are not
			 * maintained during recovery and need not be started yet.
			 */
			StartupCLOG();
			StartupSUBTRANS(oldestActiveXID);

			/*
			 * If we're beginning at a shutdown checkpoint, we know that
			 * nothing was running on the master at this point. So fake-up an
			 * empty running-xacts record and use that here and now. Recover
			 * additional standby state for prepared transactions.
			 */
			if (wasShutdown)
			{
				RunningTransactionsData running;
				TransactionId latestCompletedXid;

				/*
				 * Construct a RunningTransactions snapshot representing a
				 * shut down server, with only prepared transactions still
				 * alive. We're never overflowed at this point because all
				 * subxids are listed with their parent prepared transactions.
				 */
				running.xcnt = nxids;
				running.subxcnt = 0;
				running.subxid_overflow = false;
				running.nextXid = XidFromFullTransactionId(checkPoint.nextFullXid);
				running.oldestRunningXid = oldestActiveXID;
				latestCompletedXid = XidFromFullTransactionId(checkPoint.nextFullXid);
				TransactionIdRetreat(latestCompletedXid);
				Assert(TransactionIdIsNormal(latestCompletedXid));
				running.latestCompletedXid = latestCompletedXid;
				running.xids = xids;

				ProcArrayApplyRecoveryInfo(&running);

				StandbyRecoverPreparedTransactions();
			}
		}

		/* Initialize resource managers */
		for (rmid = 0; rmid <= RM_MAX_ID; rmid++)
		{
			if (RmgrTable[rmid].rm_startup != NULL)
				RmgrTable[rmid].rm_startup();
		}

		/*
		 * Initialize shared variables for tracking progress of WAL replay, as
		 * if we had just replayed the record before the REDO location (or the
		 * checkpoint record itself, if it's a shutdown checkpoint).
		 */
		SpinLockAcquire(&XLogCtl->info_lck);
		if (checkPoint.redo < RecPtr)
			XLogCtl->replayEndRecPtr = checkPoint.redo;
		else
			XLogCtl->replayEndRecPtr = EndRecPtr;
		XLogCtl->replayEndTLI = ThisTimeLineID;
		XLogCtl->lastReplayedEndRecPtr = XLogCtl->replayEndRecPtr;
		XLogCtl->lastReplayedTLI = XLogCtl->replayEndTLI;
		XLogCtl->recoveryLastXTime = 0;
		XLogCtl->currentChunkStartTime = 0;
		XLogCtl->recoveryPause = false;
		SpinLockRelease(&XLogCtl->info_lck);

		/* Also ensure XLogReceiptTime has a sane value */
		XLogReceiptTime = GetCurrentTimestamp();

		/*
		 * Let postmaster know we've started redo now, so that it can launch
		 * checkpointer to perform restartpoints.  We don't bother during
		 * crash recovery as restartpoints can only be performed during
		 * archive recovery.  And we'd like to keep crash recovery simple, to
		 * avoid introducing bugs that could affect you when recovering after
		 * crash.
		 *
		 * After this point, we can no longer assume that we're the only
		 * process in addition to postmaster!  Also, fsync requests are
		 * subsequently to be handled by the checkpointer, not locally.
		 */
		if (ArchiveRecoveryRequested && IsUnderPostmaster)
		{
			PublishStartupProcessInformation();
			EnableSyncRequestForwarding();
			SendPostmasterSignal(PMSIGNAL_RECOVERY_STARTED);
			bgwriterLaunched = true;
		}

		/*
		 * Allow read-only connections immediately if we're consistent
		 * already.
		 */
		CheckRecoveryConsistency();

		/*
		 * Find the first record that logically follows the checkpoint --- it
		 * might physically precede it, though.
		 */
		if (checkPoint.redo < RecPtr)
		{
			/* back up to find the record */
			XLogBeginRead(xlogreader, checkPoint.redo);
			record = ReadRecord(xlogreader, PANIC, false);
		}
		else
		{
			/* just have to read next record after CheckPoint */
			record = ReadRecord(xlogreader, LOG, false);
		}

		if (record != NULL)
		{
			ErrorContextCallback errcallback;
			TimestampTz xtime;

			InRedo = true;

			ereport(LOG,
					(errmsg("redo starts at %X/%X",
							(uint32) (ReadRecPtr >> 32), (uint32) ReadRecPtr)));

			/*
			 * main redo apply loop
			 */
			do
			{
				bool		switchedTLI = false;

#ifdef WAL_DEBUG
				if (XLOG_DEBUG ||
					(rmid == RM_XACT_ID && trace_recovery_messages <= DEBUG2) ||
					(rmid != RM_XACT_ID && trace_recovery_messages <= DEBUG3))
				{
					StringInfoData buf;

					initStringInfo(&buf);
					appendStringInfo(&buf, "REDO @ %X/%X; LSN %X/%X: ",
									 (uint32) (ReadRecPtr >> 32), (uint32) ReadRecPtr,
									 (uint32) (EndRecPtr >> 32), (uint32) EndRecPtr);
					xlog_outrec(&buf, xlogreader);
					appendStringInfoString(&buf, " - ");
					xlog_outdesc(&buf, xlogreader);
					elog(LOG, "%s", buf.data);
					pfree(buf.data);
				}
#endif

				/* Handle interrupt signals of startup process */
				HandleStartupProcInterrupts();

				/*
				 * Pause WAL replay, if requested by a hot-standby session via
				 * SetRecoveryPause().
				 *
				 * Note that we intentionally don't take the info_lck spinlock
				 * here.  We might therefore read a slightly stale value of
				 * the recoveryPause flag, but it can't be very stale (no
				 * worse than the last spinlock we did acquire).  Since a
				 * pause request is a pretty asynchronous thing anyway,
				 * possibly responding to it one WAL record later than we
				 * otherwise would is a minor issue, so it doesn't seem worth
				 * adding another spinlock cycle to prevent that.
				 */
				if (((volatile XLogCtlData *) XLogCtl)->recoveryPause)
					recoveryPausesHere(false);

				/*
				 * Have we reached our recovery target?
				 */
				if (recoveryStopsBefore(xlogreader))
				{
					reachedRecoveryTarget = true;
					break;
				}

				/*
				 * If we've been asked to lag the master, wait on latch until
				 * enough time has passed.
				 */
				if (recoveryApplyDelay(xlogreader))
				{
					/*
					 * We test for paused recovery again here. If user sets
					 * delayed apply, it may be because they expect to pause
					 * recovery in case of problems, so we must test again
					 * here otherwise pausing during the delay-wait wouldn't
					 * work.
					 */
					if (((volatile XLogCtlData *) XLogCtl)->recoveryPause)
						recoveryPausesHere(false);
				}

				/* Setup error traceback support for ereport() */
				errcallback.callback = rm_redo_error_callback;
				errcallback.arg = (void *) xlogreader;
				errcallback.previous = error_context_stack;
				error_context_stack = &errcallback;

				/*
				 * ShmemVariableCache->nextFullXid must be beyond record's
				 * xid.
				 */
				AdvanceNextFullTransactionIdPastXid(record->xl_xid);

				/*
				 * Before replaying this record, check if this record causes
				 * the current timeline to change. The record is already
				 * considered to be part of the new timeline, so we update
				 * ThisTimeLineID before replaying it. That's important so
				 * that replayEndTLI, which is recorded as the minimum
				 * recovery point's TLI if recovery stops after this record,
				 * is set correctly.
				 */
				if (record->xl_rmid == RM_XLOG_ID)
				{
					TimeLineID	newTLI = ThisTimeLineID;
					TimeLineID	prevTLI = ThisTimeLineID;
					uint8		info = record->xl_info & ~XLR_INFO_MASK;

					if (info == XLOG_CHECKPOINT_SHUTDOWN)
					{
						CheckPoint	checkPoint;

						memcpy(&checkPoint, XLogRecGetData(xlogreader), sizeof(CheckPoint));
						newTLI = checkPoint.ThisTimeLineID;
						prevTLI = checkPoint.PrevTimeLineID;
					}
					else if (info == XLOG_END_OF_RECOVERY)
					{
						xl_end_of_recovery xlrec;

						memcpy(&xlrec, XLogRecGetData(xlogreader), sizeof(xl_end_of_recovery));
						newTLI = xlrec.ThisTimeLineID;
						prevTLI = xlrec.PrevTimeLineID;
					}

					if (newTLI != ThisTimeLineID)
					{
						/* Check that it's OK to switch to this TLI */
						checkTimeLineSwitch(EndRecPtr, newTLI, prevTLI);

						/* Following WAL records should be run with new TLI */
						ThisTimeLineID = newTLI;
						switchedTLI = true;
					}
				}

				/*
				 * Update shared replayEndRecPtr before replaying this record,
				 * so that XLogFlush will update minRecoveryPoint correctly.
				 */
				SpinLockAcquire(&XLogCtl->info_lck);
				XLogCtl->replayEndRecPtr = EndRecPtr;
				XLogCtl->replayEndTLI = ThisTimeLineID;
				SpinLockRelease(&XLogCtl->info_lck);

				/*
				 * If we are attempting to enter Hot Standby mode, process
				 * XIDs we see
				 */
				if (standbyState >= STANDBY_INITIALIZED &&
					TransactionIdIsValid(record->xl_xid))
					RecordKnownAssignedTransactionIds(record->xl_xid);

				/* Now apply the WAL record itself */
				RmgrTable[record->xl_rmid].rm_redo(xlogreader);

				/*
				 * After redo, check whether the backup pages associated with
				 * the WAL record are consistent with the existing pages. This
				 * check is done only if consistency check is enabled for this
				 * record.
				 */
				if ((record->xl_info & XLR_CHECK_CONSISTENCY) != 0)
					checkXLogConsistency(xlogreader);

				/* Pop the error context stack */
				error_context_stack = errcallback.previous;

				/*
				 * Update lastReplayedEndRecPtr after this record has been
				 * successfully replayed.
				 */
				SpinLockAcquire(&XLogCtl->info_lck);
				XLogCtl->lastReplayedEndRecPtr = EndRecPtr;
				XLogCtl->lastReplayedTLI = ThisTimeLineID;
				SpinLockRelease(&XLogCtl->info_lck);

				/*
				 * If rm_redo called XLogRequestWalReceiverReply, then we wake
				 * up the receiver so that it notices the updated
				 * lastReplayedEndRecPtr and sends a reply to the master.
				 */
				if (doRequestWalReceiverReply)
				{
					doRequestWalReceiverReply = false;
					WalRcvForceReply();
				}

				/* Remember this record as the last-applied one */
				LastRec = ReadRecPtr;

				/* Allow read-only connections if we're consistent now */
				CheckRecoveryConsistency();

				/* Is this a timeline switch? */
				if (switchedTLI)
				{
					/*
					 * Before we continue on the new timeline, clean up any
					 * (possibly bogus) future WAL segments on the old
					 * timeline.
					 */
					RemoveNonParentXlogFiles(EndRecPtr, ThisTimeLineID);

					/*
					 * Wake up any walsenders to notice that we are on a new
					 * timeline.
					 */
					if (switchedTLI && AllowCascadeReplication())
						WalSndWakeup();
				}

				/* Exit loop if we reached inclusive recovery target */
				if (recoveryStopsAfter(xlogreader))
				{
					reachedRecoveryTarget = true;
					break;
				}

				/* Else, try to fetch the next WAL record */
				record = ReadRecord(xlogreader, LOG, false);
			} while (record != NULL);

			/*
			 * end of main redo apply loop
			 */

			if (reachedRecoveryTarget)
			{
				if (!reachedConsistency)
					ereport(FATAL,
							(errmsg("requested recovery stop point is before consistent recovery point")));

				/*
				 * This is the last point where we can restart recovery with a
				 * new recovery target, if we shutdown and begin again. After
				 * this, Resource Managers may choose to do permanent
				 * corrective actions at end of recovery.
				 */
				switch (recoveryTargetAction)
				{
					case RECOVERY_TARGET_ACTION_SHUTDOWN:

						/*
						 * exit with special return code to request shutdown
						 * of postmaster.  Log messages issued from
						 * postmaster.
						 */
						proc_exit(3);

					case RECOVERY_TARGET_ACTION_PAUSE:
						SetRecoveryPause(true);
						recoveryPausesHere(true);

						/* drop into promote */

					case RECOVERY_TARGET_ACTION_PROMOTE:
						break;
				}
			}

			/* Allow resource managers to do any required cleanup. */
			for (rmid = 0; rmid <= RM_MAX_ID; rmid++)
			{
				if (RmgrTable[rmid].rm_cleanup != NULL)
					RmgrTable[rmid].rm_cleanup();
			}

			ereport(LOG,
					(errmsg("redo done at %X/%X",
							(uint32) (ReadRecPtr >> 32), (uint32) ReadRecPtr)));
			xtime = GetLatestXTime();
			if (xtime)
				ereport(LOG,
						(errmsg("last completed transaction was at log time %s",
								timestamptz_to_str(xtime))));

			InRedo = false;
		}
		else
		{
			/* there are no WAL records following the checkpoint */
			ereport(LOG,
					(errmsg("redo is not required")));

		}

		/*
		 * This check is intentionally after the above log messages that
		 * indicate how far recovery went.
		 */
		if (ArchiveRecoveryRequested &&
			recoveryTarget != RECOVERY_TARGET_UNSET &&
			!reachedRecoveryTarget)
			ereport(FATAL,
					(errmsg("recovery ended before configured recovery target was reached")));
	}

	/*
	 * Kill WAL receiver, if it's still running, before we continue to write
	 * the startup checkpoint and aborted-contrecord records. It will trump
	 * over these records and subsequent ones if it's still alive when we
	 * start writing WAL.
	 */
	ShutdownWalRcv();

	/*
	 * Reset unlogged relations to the contents of their INIT fork. This is
	 * done AFTER recovery is complete so as to include any unlogged relations
	 * created during recovery, but BEFORE recovery is marked as having
	 * completed successfully. Otherwise we'd not retry if any of the post
	 * end-of-recovery steps fail.
	 */
	if (InRecovery)
		ResetUnloggedRelations(UNLOGGED_RELATION_INIT);

	/*
	 * We don't need the latch anymore. It's not strictly necessary to disown
	 * it, but let's do it for the sake of tidiness.
	 */
	if (ArchiveRecoveryRequested)
		DisownLatch(&XLogCtl->recoveryWakeupLatch);

	/*
	 * We are now done reading the xlog from stream. Turn off streaming
	 * recovery to force fetching the files (which would be required at end of
	 * recovery, e.g., timeline history file) from archive or pg_wal.
	 *
	 * Note that standby mode must be turned off after killing WAL receiver,
	 * i.e., calling ShutdownWalRcv().
	 */
	Assert(!WalRcvStreaming());
	StandbyMode = false;

	/*
	 * Determine where to start writing WAL next.
	 *
	 * When recovery ended in an incomplete record, write a WAL record about
	 * that and continue after it.  In all other cases, re-fetch the last
	 * valid or last applied record, so we can identify the exact endpoint of
	 * what we consider the valid portion of WAL.
	 */
	XLogBeginRead(xlogreader, LastRec);
	record = ReadRecord(xlogreader, PANIC, false);
	EndOfLog = EndRecPtr;

	/*
	 * EndOfLogTLI is the TLI in the filename of the XLOG segment containing
	 * the end-of-log. It could be different from the timeline that EndOfLog
	 * nominally belongs to, if there was a timeline switch in that segment,
	 * and we were reading the old WAL from a segment belonging to a higher
	 * timeline.
	 */
	EndOfLogTLI = xlogreader->seg.ws_tli;

	/*
	 * Complain if we did not roll forward far enough to render the backup
	 * dump consistent.  Note: it is indeed okay to look at the local variable
	 * minRecoveryPoint here, even though ControlFile->minRecoveryPoint might
	 * be further ahead --- ControlFile->minRecoveryPoint cannot have been
	 * advanced beyond the WAL we processed.
	 */
	if (InRecovery &&
		(EndOfLog < minRecoveryPoint ||
		 !XLogRecPtrIsInvalid(ControlFile->backupStartPoint)))
	{
		/*
		 * Ran off end of WAL before reaching end-of-backup WAL record, or
		 * minRecoveryPoint. That's usually a bad sign, indicating that you
		 * tried to recover from an online backup but never called
		 * pg_stop_backup(), or you didn't archive all the WAL up to that
		 * point. However, this also happens in crash recovery, if the system
		 * crashes while an online backup is in progress. We must not treat
		 * that as an error, or the database will refuse to start up.
		 */
		if (ArchiveRecoveryRequested || ControlFile->backupEndRequired)
		{
			if (ControlFile->backupEndRequired)
				ereport(FATAL,
						(errmsg("WAL ends before end of online backup"),
						 errhint("All WAL generated while online backup was taken must be available at recovery.")));
			else if (!XLogRecPtrIsInvalid(ControlFile->backupStartPoint))
				ereport(FATAL,
						(errmsg("WAL ends before end of online backup"),
						 errhint("Online backup started with pg_start_backup() must be ended with pg_stop_backup(), and all WAL up to that point must be available at recovery.")));
			else
				ereport(FATAL,
						(errmsg("WAL ends before consistent recovery point")));
		}
	}

	/*
	 * Pre-scan prepared transactions to find out the range of XIDs present.
	 * This information is not quite needed yet, but it is positioned here so
	 * as potential problems are detected before any on-disk change is done.
	 */
	oldestActiveXID = PrescanPreparedTransactions(NULL, NULL);

	/*
	 * Consider whether we need to assign a new timeline ID.
	 *
	 * If we are doing an archive recovery, we always assign a new ID.  This
	 * handles a couple of issues.  If we stopped short of the end of WAL
	 * during recovery, then we are clearly generating a new timeline and must
	 * assign it a unique new ID.  Even if we ran to the end, modifying the
	 * current last segment is problematic because it may result in trying to
	 * overwrite an already-archived copy of that segment, and we encourage
	 * DBAs to make their archive_commands reject that.  We can dodge the
	 * problem by making the new active segment have a new timeline ID.
	 *
	 * In a normal crash recovery, we can just extend the timeline we were in.
	 */
	PrevTimeLineID = ThisTimeLineID;
	if (ArchiveRecoveryRequested)
	{
		char		reason[200];
		char		recoveryPath[MAXPGPATH];

		Assert(InArchiveRecovery);

		ThisTimeLineID = findNewestTimeLine(recoveryTargetTLI) + 1;
		ereport(LOG,
				(errmsg("selected new timeline ID: %u", ThisTimeLineID)));

		/*
		 * Create a comment for the history file to explain why and where
		 * timeline changed.
		 */
		if (recoveryTarget == RECOVERY_TARGET_XID)
			snprintf(reason, sizeof(reason),
					 "%s transaction %u",
					 recoveryStopAfter ? "after" : "before",
					 recoveryStopXid);
		else if (recoveryTarget == RECOVERY_TARGET_TIME)
			snprintf(reason, sizeof(reason),
					 "%s %s\n",
					 recoveryStopAfter ? "after" : "before",
					 timestamptz_to_str(recoveryStopTime));
		else if (recoveryTarget == RECOVERY_TARGET_LSN)
			snprintf(reason, sizeof(reason),
					 "%s LSN %X/%X\n",
					 recoveryStopAfter ? "after" : "before",
					 (uint32) (recoveryStopLSN >> 32),
					 (uint32) recoveryStopLSN);
		else if (recoveryTarget == RECOVERY_TARGET_NAME)
			snprintf(reason, sizeof(reason),
					 "at restore point \"%s\"",
					 recoveryStopName);
		else if (recoveryTarget == RECOVERY_TARGET_IMMEDIATE)
			snprintf(reason, sizeof(reason), "reached consistency");
		else
			snprintf(reason, sizeof(reason), "no recovery target specified");

		/*
		 * We are now done reading the old WAL.  Turn off archive fetching if
		 * it was active, and make a writable copy of the last WAL segment.
		 * (Note that we also have a copy of the last block of the old WAL in
		 * readBuf; we will use that below.)
		 */
		exitArchiveRecovery(EndOfLogTLI, EndOfLog);

		/*
		 * Write the timeline history file, and have it archived. After this
		 * point (or rather, as soon as the file is archived), the timeline
		 * will appear as "taken" in the WAL archive and to any standby
		 * servers.  If we crash before actually switching to the new
		 * timeline, standby servers will nevertheless think that we switched
		 * to the new timeline, and will try to connect to the new timeline.
		 * To minimize the window for that, try to do as little as possible
		 * between here and writing the end-of-recovery record.
		 */
		writeTimeLineHistory(ThisTimeLineID, recoveryTargetTLI,
							 EndRecPtr, reason);

		/*
		 * Since there might be a partial WAL segment named RECOVERYXLOG, get
		 * rid of it.
		 */
		snprintf(recoveryPath, MAXPGPATH, XLOGDIR "/RECOVERYXLOG");
		unlink(recoveryPath);	/* ignore any error */

		/* Get rid of any remaining recovered timeline-history file, too */
		snprintf(recoveryPath, MAXPGPATH, XLOGDIR "/RECOVERYHISTORY");
		unlink(recoveryPath);	/* ignore any error */
	}

	/* Save the selected TimeLineID in shared memory, too */
	XLogCtl->ThisTimeLineID = ThisTimeLineID;
	XLogCtl->PrevTimeLineID = PrevTimeLineID;

	/*
	 * Actually, if WAL ended in an incomplete record, skip the parts that
	 * made it through and start writing after the portion that persisted.
	 * (It's critical to first write an OVERWRITE_CONTRECORD message, which
	 * we'll do as soon as we're open for writing new WAL.)
	 */
	if (!XLogRecPtrIsInvalid(missingContrecPtr))
	{
		/*
		 * We should only have a missingContrecPtr if we're not switching to
		 * a new timeline. When a timeline switch occurs, WAL is copied from
		 * the old timeline to the new only up to the end of the last complete
		 * record, so there can't be an incomplete WAL record that we need to
		 * disregard.
		 */
		Assert(ThisTimeLineID == PrevTimeLineID);
		Assert(!XLogRecPtrIsInvalid(abortedRecPtr));
		EndOfLog = missingContrecPtr;
	}

	/*
	 * Prepare to write WAL starting at EndOfLog location, and init xlog
	 * buffer cache using the block containing the last record from the
	 * previous incarnation.
	 */
	Insert = &XLogCtl->Insert;
	Insert->PrevBytePos = XLogRecPtrToBytePos(LastRec);
	Insert->CurrBytePos = XLogRecPtrToBytePos(EndOfLog);

	/*
	 * Tricky point here: readBuf contains the *last* block that the LastRec
	 * record spans, not the one it starts in.  The last block is indeed the
	 * one we want to use.
	 */
	if (EndOfLog % XLOG_BLCKSZ != 0)
	{
		char	   *page;
		int			len;
		int			firstIdx;
		XLogRecPtr	pageBeginPtr;

		pageBeginPtr = EndOfLog - (EndOfLog % XLOG_BLCKSZ);
		Assert(readOff == XLogSegmentOffset(pageBeginPtr, wal_segment_size));

		firstIdx = XLogRecPtrToBufIdx(EndOfLog);

		/* Copy the valid part of the last block, and zero the rest */
		page = &XLogCtl->pages[firstIdx * XLOG_BLCKSZ];
		len = EndOfLog % XLOG_BLCKSZ;
		memcpy(page, xlogreader->readBuf, len);
		memset(page + len, 0, XLOG_BLCKSZ - len);

		XLogCtl->xlblocks[firstIdx] = pageBeginPtr + XLOG_BLCKSZ;
		XLogCtl->InitializedUpTo = pageBeginPtr + XLOG_BLCKSZ;
	}
	else
	{
		/*
		 * There is no partial block to copy. Just set InitializedUpTo, and
		 * let the first attempt to insert a log record to initialize the next
		 * buffer.
		 */
		XLogCtl->InitializedUpTo = EndOfLog;
	}

	LogwrtResult.Write = LogwrtResult.Flush = EndOfLog;

	XLogCtl->LogwrtResult = LogwrtResult;

	XLogCtl->LogwrtRqst.Write = EndOfLog;
	XLogCtl->LogwrtRqst.Flush = EndOfLog;

	LocalSetXLogInsertAllowed();

	/* If necessary, write overwrite-contrecord before doing anything else */
	if (!XLogRecPtrIsInvalid(abortedRecPtr))
	{
		Assert(!XLogRecPtrIsInvalid(missingContrecPtr));
		CreateOverwriteContrecordRecord(abortedRecPtr);
		abortedRecPtr = InvalidXLogRecPtr;
		missingContrecPtr = InvalidXLogRecPtr;
	}

	/*
	 * Update full_page_writes in shared memory and write an XLOG_FPW_CHANGE
	 * record before resource manager writes cleanup WAL records or checkpoint
	 * record is written.
	 */
	Insert->fullPageWrites = lastFullPageWrites;
	UpdateFullPageWrites();
	LocalXLogInsertAllowed = -1;

	if (InRecovery)
	{
		/*
		 * Perform a checkpoint to update all our recovery activity to disk.
		 *
		 * Note that we write a shutdown checkpoint rather than an on-line
		 * one. This is not particularly critical, but since we may be
		 * assigning a new TLI, using a shutdown checkpoint allows us to have
		 * the rule that TLI only changes in shutdown checkpoints, which
		 * allows some extra error checking in xlog_redo.
		 *
		 * In fast promotion, only create a lightweight end-of-recovery record
		 * instead of a full checkpoint. A checkpoint is requested later,
		 * after we're fully out of recovery mode and already accepting
		 * queries.
		 */
		if (bgwriterLaunched)
		{
			if (fast_promote)
			{
				checkPointLoc = ControlFile->checkPoint;

				/*
				 * Confirm the last checkpoint is available for us to recover
				 * from if we fail.
				 */
				record = ReadCheckpointRecord(xlogreader, checkPointLoc, 1, false);
				if (record != NULL)
				{
					fast_promoted = true;

					/*
					 * Insert a special WAL record to mark the end of
					 * recovery, since we aren't doing a checkpoint. That
					 * means that the checkpointer process may likely be in
					 * the middle of a time-smoothed restartpoint and could
					 * continue to be for minutes after this. That sounds
					 * strange, but the effect is roughly the same and it
					 * would be stranger to try to come out of the
					 * restartpoint and then checkpoint. We request a
					 * checkpoint later anyway, just for safety.
					 */
					CreateEndOfRecoveryRecord();
				}
			}

			if (!fast_promoted)
				RequestCheckpoint(CHECKPOINT_END_OF_RECOVERY |
								  CHECKPOINT_IMMEDIATE |
								  CHECKPOINT_WAIT);
		}
		else
			CreateCheckPoint(CHECKPOINT_END_OF_RECOVERY | CHECKPOINT_IMMEDIATE);
	}

	/*
	 * Preallocate additional log files, if wanted.
	 */
	PreallocXlogFiles(EndOfLog);

	/*
	 * Okay, we're officially UP.
	 */
	InRecovery = false;

	/* start the archive_timeout timer and LSN running */
	XLogCtl->lastSegSwitchTime = (pg_time_t) time(NULL);
	XLogCtl->lastSegSwitchLSN = EndOfLog;

	/* also initialize latestCompletedXid, to nextXid - 1 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
	ShmemVariableCache->latestCompletedXid = XidFromFullTransactionId(ShmemVariableCache->nextFullXid);
	TransactionIdRetreat(ShmemVariableCache->latestCompletedXid);
	LWLockRelease(ProcArrayLock);

	/*
	 * Start up the commit log and subtrans, if not already done for hot
	 * standby.  (commit timestamps are started below, if necessary.)
	 */
	if (standbyState == STANDBY_DISABLED)
	{
		StartupCLOG();
		StartupSUBTRANS(oldestActiveXID);
	}

	/*
	 * Perform end of recovery actions for any SLRUs that need it.
	 */
	TrimCLOG();
	TrimMultiXact();

	/*
	 * Reload shared-memory state for prepared transactions.  This needs to
	 * happen before renaming the last partial segment of the old timeline as
	 * it may be possible that we have to recovery some transactions from it.
	 */
	RecoverPreparedTransactions();

	/* Shut down xlogreader */
	if (readFile >= 0)
	{
		close(readFile);
		readFile = -1;
	}
	XLogReaderFree(xlogreader);

	/*
	 * If any of the critical GUCs have changed, log them before we allow
	 * backends to write WAL.
	 */
	LocalSetXLogInsertAllowed();
	XLogReportParameters();

	if (ArchiveRecoveryRequested)
	{
		/*
		 * And finally, execute the recovery_end_command, if any.
		 */
		if (recoveryEndCommand && strcmp(recoveryEndCommand, "") != 0)
			ExecuteRecoveryCommand(recoveryEndCommand,
								   "recovery_end_command",
								   true);

		/*
		 * We switched to a new timeline. Clean up segments on the old
		 * timeline.
		 *
		 * If there are any higher-numbered segments on the old timeline,
		 * remove them. They might contain valid WAL, but they might also be
		 * pre-allocated files containing garbage. In any case, they are not
		 * part of the new timeline's history so we don't need them.
		 */
		RemoveNonParentXlogFiles(EndOfLog, ThisTimeLineID);

		/*
		 * If the switch happened in the middle of a segment, what to do with
		 * the last, partial segment on the old timeline? If we don't archive
		 * it, and the server that created the WAL never archives it either
		 * (e.g. because it was hit by a meteor), it will never make it to the
		 * archive. That's OK from our point of view, because the new segment
		 * that we created with the new TLI contains all the WAL from the old
		 * timeline up to the switch point. But if you later try to do PITR to
		 * the "missing" WAL on the old timeline, recovery won't find it in
		 * the archive. It's physically present in the new file with new TLI,
		 * but recovery won't look there when it's recovering to the older
		 * timeline. On the other hand, if we archive the partial segment, and
		 * the original server on that timeline is still running and archives
		 * the completed version of the same segment later, it will fail. (We
		 * used to do that in 9.4 and below, and it caused such problems).
		 *
		 * As a compromise, we rename the last segment with the .partial
		 * suffix, and archive it. Archive recovery will never try to read
		 * .partial segments, so they will normally go unused. But in the odd
		 * PITR case, the administrator can copy them manually to the pg_wal
		 * directory (removing the suffix). They can be useful in debugging,
		 * too.
		 *
		 * If a .done or .ready file already exists for the old timeline,
		 * however, we had already determined that the segment is complete, so
		 * we can let it be archived normally. (In particular, if it was
		 * restored from the archive to begin with, it's expected to have a
		 * .done file).
		 */
		if (XLogSegmentOffset(EndOfLog, wal_segment_size) != 0 &&
			XLogArchivingActive())
		{
			char		origfname[MAXFNAMELEN];
			XLogSegNo	endLogSegNo;

			XLByteToPrevSeg(EndOfLog, endLogSegNo, wal_segment_size);
			XLogFileName(origfname, EndOfLogTLI, endLogSegNo, wal_segment_size);

			if (!XLogArchiveIsReadyOrDone(origfname))
			{
				char		origpath[MAXPGPATH];
				char		partialfname[MAXFNAMELEN];
				char		partialpath[MAXPGPATH];

				XLogFilePath(origpath, EndOfLogTLI, endLogSegNo, wal_segment_size);
				snprintf(partialfname, MAXFNAMELEN, "%s.partial", origfname);
				snprintf(partialpath, MAXPGPATH, "%s.partial", origpath);

				/*
				 * Make sure there's no .done or .ready file for the .partial
				 * file.
				 */
				XLogArchiveCleanup(partialfname);

				durable_rename(origpath, partialpath, ERROR);
				XLogArchiveNotify(partialfname);
			}
		}
	}

	/*
	 * Local WAL inserts enabled, so it's time to finish initialization of
	 * commit timestamp.
	 */
	CompleteCommitTsInitialization();

	/*
	 * All done with end-of-recovery actions.
	 *
	 * Now allow backends to write WAL and update the control file status in
	 * consequence.  The boolean flag allowing backends to write WAL is
	 * updated while holding ControlFileLock to prevent other backends to look
	 * at an inconsistent state of the control file in shared memory.  There
	 * is still a small window during which backends can write WAL and the
	 * control file is still referring to a system not in DB_IN_PRODUCTION
	 * state while looking at the on-disk control file.
	 *
	 * Also, although the boolean flag to allow WAL is probably atomic in
	 * itself, we use the info_lck here to ensure that there are no race
	 * conditions concerning visibility of other recent updates to shared
	 * memory.
	 */
	LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
	ControlFile->state = DB_IN_PRODUCTION;
	ControlFile->time = (pg_time_t) time(NULL);

	SpinLockAcquire(&XLogCtl->info_lck);
	XLogCtl->SharedRecoveryState = RECOVERY_STATE_DONE;
	SpinLockRelease(&XLogCtl->info_lck);

	UpdateControlFile();
	LWLockRelease(ControlFileLock);

	/*
	 * Shutdown the recovery environment.  This must occur after
	 * RecoverPreparedTransactions() (see notes in lock_twophase_recover())
	 * and after switching SharedRecoveryState to RECOVERY_STATE_DONE so as
	 * any session building a snapshot will not rely on KnownAssignedXids as
	 * RecoveryInProgress() would return false at this stage.  This is
	 * particularly critical for prepared 2PC transactions, that would still
	 * need to be included in snapshots once recovery has ended.
	 */
	if (standbyState != STANDBY_DISABLED)
		ShutdownRecoveryTransactionEnvironment();

	/*
	 * If there were cascading standby servers connected to us, nudge any wal
	 * sender processes to notice that we've been promoted.
	 */
	WalSndWakeup();

	/*
	 * If this was a fast promotion, request an (online) checkpoint now. This
	 * isn't required for consistency, but the last restartpoint might be far
	 * back, and in case of a crash, recovering from it might take a longer
	 * than is appropriate now that we're not in standby mode anymore.
	 */
	if (fast_promoted)
		RequestCheckpoint(CHECKPOINT_FORCE);
}

/*
 * Verify that, in non-test mode, ./pg_tblspc doesn't contain any real
 * directories.
 *
 * Replay of database creation XLOG records for databases that were later
 * dropped can create fake directories in pg_tblspc.  By the time consistency
 * is reached these directories should have been removed; here we verify
 * that this did indeed happen.  This is to be called at the point where
 * consistent state is reached.
 *
 * allow_in_place_tablespaces turns the PANIC into a WARNING, which is
 * useful for testing purposes, and also allows for an escape hatch in case
 * things go south.
 */
static void
CheckTablespaceDirectory(void)
{
	DIR        *dir;
	struct dirent *de;

	dir = AllocateDir("pg_tblspc");
	while ((de = ReadDir(dir, "pg_tblspc")) != NULL)
	{
		char        path[MAXPGPATH + 10];
#ifndef WIN32
		struct stat st;
#endif

		/* Skip entries of non-oid names */
		if (strspn(de->d_name, "0123456789") != strlen(de->d_name))
			continue;

		snprintf(path, sizeof(path), "pg_tblspc/%s", de->d_name);

#ifndef WIN32
		if (lstat(path, &st) < 0)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m",
							path)));
		else if (!S_ISLNK(st.st_mode))
#else		/* WIN32 */
		if (!pgwin32_is_junction(path))
#endif
			ereport(allow_in_place_tablespaces ? WARNING : PANIC,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("unexpected directory entry \"%s\" found in %s",
							de->d_name, "pg_tblspc/"),
					 errdetail("All directory entries in pg_tblspc/ should be symbolic links."),
					 errhint("Remove those directories, or set allow_in_place_tablespaces to ON transiently to let recovery complete.")));
	}
}

/*
 * Checks if recovery has reached a consistent state. When consistency is
 * reached and we have a valid starting standby snapshot, tell postmaster
 * that it can start accepting read-only connections.
 */
static void
CheckRecoveryConsistency(void)
{
	XLogRecPtr	lastReplayedEndRecPtr;

	/*
	 * During crash recovery, we don't reach a consistent state until we've
	 * replayed all the WAL.
	 */
	if (XLogRecPtrIsInvalid(minRecoveryPoint))
		return;

	Assert(InArchiveRecovery);

	/*
	 * assume that we are called in the startup process, and hence don't need
	 * a lock to read lastReplayedEndRecPtr
	 */
	lastReplayedEndRecPtr = XLogCtl->lastReplayedEndRecPtr;

	/*
	 * Have we reached the point where our base backup was completed?
	 */
	if (!XLogRecPtrIsInvalid(ControlFile->backupEndPoint) &&
		ControlFile->backupEndPoint <= lastReplayedEndRecPtr)
	{
		/*
		 * We have reached the end of base backup, as indicated by pg_control.
		 * The data on disk is now consistent. Reset backupStartPoint and
		 * backupEndPoint, and update minRecoveryPoint to make sure we don't
		 * allow starting up at an earlier point even if recovery is stopped
		 * and restarted soon after this.
		 */
		elog(DEBUG1, "end of backup reached");

		LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);

		if (ControlFile->minRecoveryPoint < lastReplayedEndRecPtr)
			ControlFile->minRecoveryPoint = lastReplayedEndRecPtr;

		ControlFile->backupStartPoint = InvalidXLogRecPtr;
		ControlFile->backupEndPoint = InvalidXLogRecPtr;
		ControlFile->backupEndRequired = false;
		UpdateControlFile();

		LWLockRelease(ControlFileLock);
	}

	/*
	 * Have we passed our safe starting point? Note that minRecoveryPoint is
	 * known to be incorrectly set if ControlFile->backupEndRequired, until
	 * the XLOG_BACKUP_END arrives to advise us of the correct
	 * minRecoveryPoint. All we know prior to that is that we're not
	 * consistent yet.
	 */
	if (!reachedConsistency && !ControlFile->backupEndRequired &&
		minRecoveryPoint <= lastReplayedEndRecPtr &&
		XLogRecPtrIsInvalid(ControlFile->backupStartPoint))
	{
		/*
		 * Check to see if the XLOG sequence contained any unresolved
		 * references to uninitialized pages.
		 */
		XLogCheckInvalidPages();

		/*
		 * Check that pg_tblspc doesn't contain any real directories. Replay
		 * of Database/CREATE_* records may have created ficticious tablespace
		 * directories that should have been removed by the time consistency
		 * was reached.
		 */
		CheckTablespaceDirectory();

		reachedConsistency = true;
		ereport(LOG,
				(errmsg("consistent recovery state reached at %X/%X",
						(uint32) (lastReplayedEndRecPtr >> 32),
						(uint32) lastReplayedEndRecPtr)));
	}

	/*
	 * Have we got a valid starting snapshot that will allow queries to be
	 * run? If so, we can tell postmaster that the database is consistent now,
	 * enabling connections.
	 */
	if (standbyState == STANDBY_SNAPSHOT_READY &&
		!LocalHotStandbyActive &&
		reachedConsistency &&
		IsUnderPostmaster)
	{
		SpinLockAcquire(&XLogCtl->info_lck);
		XLogCtl->SharedHotStandbyActive = true;
		SpinLockRelease(&XLogCtl->info_lck);

		LocalHotStandbyActive = true;

		SendPostmasterSignal(PMSIGNAL_BEGIN_HOT_STANDBY);
	}
}

/*
 * Is the system still in recovery?
 *
 * Unlike testing InRecovery, this works in any process that's connected to
 * shared memory.
 *
 * As a side-effect, we initialize the local TimeLineID and RedoRecPtr
 * variables the first time we see that recovery is finished.
 */
bool
RecoveryInProgress(void)
{
	/*
	 * We check shared state each time only until we leave recovery mode. We
	 * can't re-enter recovery, so there's no need to keep checking after the
	 * shared variable has once been seen false.
	 */
	if (!LocalRecoveryInProgress)
		return false;
	else
	{
		/*
		 * use volatile pointer to make sure we make a fresh read of the
		 * shared variable.
		 */
		volatile XLogCtlData *xlogctl = XLogCtl;

		LocalRecoveryInProgress = (xlogctl->SharedRecoveryState != RECOVERY_STATE_DONE);

		/*
		 * Initialize TimeLineID and RedoRecPtr when we discover that recovery
		 * is finished. InitPostgres() relies upon this behaviour to ensure
		 * that InitXLOGAccess() is called at backend startup.  (If you change
		 * this, see also LocalSetXLogInsertAllowed.)
		 */
		if (!LocalRecoveryInProgress)
		{
			/*
			 * If we just exited recovery, make sure we read TimeLineID and
			 * RedoRecPtr after SharedRecoveryState (for machines with weak
			 * memory ordering).
			 */
			pg_memory_barrier();
			InitXLOGAccess();
		}

		/*
		 * Note: We don't need a memory barrier when we're still in recovery.
		 * We might exit recovery immediately after return, so the caller
		 * can't rely on 'true' meaning that we're still in recovery anyway.
		 */

		return LocalRecoveryInProgress;
	}
}

/*
 * Returns current recovery state from shared memory.
 *
 * This returned state is kept consistent with the contents of the control
 * file.  See details about the possible values of RecoveryState in xlog.h.
 */
RecoveryState
GetRecoveryState(void)
{
	RecoveryState retval;

	SpinLockAcquire(&XLogCtl->info_lck);
	retval = XLogCtl->SharedRecoveryState;
	SpinLockRelease(&XLogCtl->info_lck);

	return retval;
}

/*
 * Is HotStandby active yet? This is only important in special backends
 * since normal backends won't ever be able to connect until this returns
 * true. Postmaster knows this by way of signal, not via shared memory.
 *
 * Unlike testing standbyState, this works in any process that's connected to
 * shared memory.  (And note that standbyState alone doesn't tell the truth
 * anyway.)
 */
bool
HotStandbyActive(void)
{
	/*
	 * We check shared state each time only until Hot Standby is active. We
	 * can't de-activate Hot Standby, so there's no need to keep checking
	 * after the shared variable has once been seen true.
	 */
	if (LocalHotStandbyActive)
		return true;
	else
	{
		/* spinlock is essential on machines with weak memory ordering! */
		SpinLockAcquire(&XLogCtl->info_lck);
		LocalHotStandbyActive = XLogCtl->SharedHotStandbyActive;
		SpinLockRelease(&XLogCtl->info_lck);

		return LocalHotStandbyActive;
	}
}

/*
 * Like HotStandbyActive(), but to be used only in WAL replay code,
 * where we don't need to ask any other process what the state is.
 */
bool
HotStandbyActiveInReplay(void)
{
	Assert(AmStartupProcess() || !IsPostmasterEnvironment);
	return LocalHotStandbyActive;
}

/*
 * Is this process allowed to insert new WAL records?
 *
 * Ordinarily this is essentially equivalent to !RecoveryInProgress().
 * But we also have provisions for forcing the result "true" or "false"
 * within specific processes regardless of the global state.
 */
bool
XLogInsertAllowed(void)
{
	/*
	 * If value is "unconditionally true" or "unconditionally false", just
	 * return it.  This provides the normal fast path once recovery is known
	 * done.
	 */
	if (LocalXLogInsertAllowed >= 0)
		return (bool) LocalXLogInsertAllowed;

	/*
	 * Else, must check to see if we're still in recovery.
	 */
	if (RecoveryInProgress())
		return false;

	/*
	 * On exit from recovery, reset to "unconditionally true", since there is
	 * no need to keep checking.
	 */
	LocalXLogInsertAllowed = 1;
	return true;
}

/*
 * Make XLogInsertAllowed() return true in the current process only.
 *
 * Note: it is allowed to switch LocalXLogInsertAllowed back to -1 later,
 * and even call LocalSetXLogInsertAllowed() again after that.
 */
static void
LocalSetXLogInsertAllowed(void)
{
	Assert(LocalXLogInsertAllowed == -1);
	LocalXLogInsertAllowed = 1;

	/* Initialize as RecoveryInProgress() would do when switching state */
	InitXLOGAccess();
}

/*
 * Subroutine to try to fetch and validate a prior checkpoint record.
 *
 * whichChkpt identifies the checkpoint (merely for reporting purposes).
 * 1 for "primary", 0 for "other" (backup_label)
 */
static XLogRecord *
ReadCheckpointRecord(XLogReaderState *xlogreader, XLogRecPtr RecPtr,
					 int whichChkpt, bool report)
{
	XLogRecord *record;
	uint8		info;

	if (!XRecOffIsValid(RecPtr))
	{
		if (!report)
			return NULL;

		switch (whichChkpt)
		{
			case 1:
				ereport(LOG,
						(errmsg("invalid primary checkpoint link in control file")));
				break;
			default:
				ereport(LOG,
						(errmsg("invalid checkpoint link in backup_label file")));
				break;
		}
		return NULL;
	}

	XLogBeginRead(xlogreader, RecPtr);
	record = ReadRecord(xlogreader, LOG, true);

	if (record == NULL)
	{
		if (!report)
			return NULL;

		switch (whichChkpt)
		{
			case 1:
				ereport(LOG,
						(errmsg("invalid primary checkpoint record")));
				break;
			default:
				ereport(LOG,
						(errmsg("invalid checkpoint record")));
				break;
		}
		return NULL;
	}
	if (record->xl_rmid != RM_XLOG_ID)
	{
		switch (whichChkpt)
		{
			case 1:
				ereport(LOG,
						(errmsg("invalid resource manager ID in primary checkpoint record")));
				break;
			default:
				ereport(LOG,
						(errmsg("invalid resource manager ID in checkpoint record")));
				break;
		}
		return NULL;
	}
	info = record->xl_info & ~XLR_INFO_MASK;
	if (info != XLOG_CHECKPOINT_SHUTDOWN &&
		info != XLOG_CHECKPOINT_ONLINE)
	{
		switch (whichChkpt)
		{
			case 1:
				ereport(LOG,
						(errmsg("invalid xl_info in primary checkpoint record")));
				break;
			default:
				ereport(LOG,
						(errmsg("invalid xl_info in checkpoint record")));
				break;
		}
		return NULL;
	}
	if (record->xl_tot_len != SizeOfXLogRecord + SizeOfXLogRecordDataHeaderShort + sizeof(CheckPoint))
	{
		switch (whichChkpt)
		{
			case 1:
				ereport(LOG,
						(errmsg("invalid length of primary checkpoint record")));
				break;
			default:
				ereport(LOG,
						(errmsg("invalid length of checkpoint record")));
				break;
		}
		return NULL;
	}
	return record;
}

/*
 * This must be called in a backend process before creating WAL records
 * (except in a standalone backend, which does StartupXLOG instead).  We need
 * to initialize the local copies of ThisTimeLineID and RedoRecPtr.
 *
 * Note: before Postgres 8.0, we went to some effort to keep the postmaster
 * process's copies of ThisTimeLineID and RedoRecPtr valid too.  This was
 * unnecessary however, since the postmaster itself never touches XLOG anyway.
 */
void
InitXLOGAccess(void)
{
	XLogCtlInsert *Insert = &XLogCtl->Insert;

	/* ThisTimeLineID doesn't change so we need no lock to copy it */
	ThisTimeLineID = XLogCtl->ThisTimeLineID;
	Assert(ThisTimeLineID != 0 || IsBootstrapProcessingMode());

	/* set wal_segment_size */
	wal_segment_size = ControlFile->xlog_seg_size;

	/* Use GetRedoRecPtr to copy the RedoRecPtr safely */
	(void) GetRedoRecPtr();
	/* Also update our copy of doPageWrites. */
	doPageWrites = (Insert->fullPageWrites || Insert->forcePageWrites);

	/* Also initialize the working areas for constructing WAL records */
	InitXLogInsert();
}

/*
 * Return the current Redo pointer from shared memory.
 *
 * As a side-effect, the local RedoRecPtr copy is updated.
 */
XLogRecPtr
GetRedoRecPtr(void)
{
	XLogRecPtr	ptr;

	/*
	 * The possibly not up-to-date copy in XlogCtl is enough. Even if we
	 * grabbed a WAL insertion lock to read the master copy, someone might
	 * update it just after we've released the lock.
	 */
	SpinLockAcquire(&XLogCtl->info_lck);
	ptr = XLogCtl->RedoRecPtr;
	SpinLockRelease(&XLogCtl->info_lck);

	if (RedoRecPtr < ptr)
		RedoRecPtr = ptr;

	return RedoRecPtr;
}

/*
 * Return information needed to decide whether a modified block needs a
 * full-page image to be included in the WAL record.
 *
 * The returned values are cached copies from backend-private memory, and
 * possibly out-of-date.  XLogInsertRecord will re-check them against
 * up-to-date values, while holding the WAL insert lock.
 */
void
GetFullPageWriteInfo(XLogRecPtr *RedoRecPtr_p, bool *doPageWrites_p)
{
	*RedoRecPtr_p = RedoRecPtr;
	*doPageWrites_p = doPageWrites;
}

/*
 * GetInsertRecPtr -- Returns the current insert position.
 *
 * NOTE: The value *actually* returned is the position of the last full
 * xlog page. It lags behind the real insert position by at most 1 page.
 * For that, we don't need to scan through WAL insertion locks, and an
 * approximation is enough for the current usage of this function.
 */
XLogRecPtr
GetInsertRecPtr(void)
{
	XLogRecPtr	recptr;

	SpinLockAcquire(&XLogCtl->info_lck);
	recptr = XLogCtl->LogwrtRqst.Write;
	SpinLockRelease(&XLogCtl->info_lck);

	return recptr;
}

/*
 * GetFlushRecPtr -- Returns the current flush position, ie, the last WAL
 * position known to be fsync'd to disk.
 */
XLogRecPtr
GetFlushRecPtr(void)
{
	SpinLockAcquire(&XLogCtl->info_lck);
	LogwrtResult = XLogCtl->LogwrtResult;
	SpinLockRelease(&XLogCtl->info_lck);

	return LogwrtResult.Flush;
}

/*
 * GetLastImportantRecPtr -- Returns the LSN of the last important record
 * inserted. All records not explicitly marked as unimportant are considered
 * important.
 *
 * The LSN is determined by computing the maximum of
 * WALInsertLocks[i].lastImportantAt.
 */
XLogRecPtr
GetLastImportantRecPtr(void)
{
	XLogRecPtr	res = InvalidXLogRecPtr;
	int			i;

	for (i = 0; i < NUM_XLOGINSERT_LOCKS; i++)
	{
		XLogRecPtr	last_important;

		/*
		 * Need to take a lock to prevent torn reads of the LSN, which are
		 * possible on some of the supported platforms. WAL insert locks only
		 * support exclusive mode, so we have to use that.
		 */
		LWLockAcquire(&WALInsertLocks[i].l.lock, LW_EXCLUSIVE);
		last_important = WALInsertLocks[i].l.lastImportantAt;
		LWLockRelease(&WALInsertLocks[i].l.lock);

		if (res < last_important)
			res = last_important;
	}

	return res;
}

/*
 * Get the time and LSN of the last xlog segment switch
 */
pg_time_t
GetLastSegSwitchData(XLogRecPtr *lastSwitchLSN)
{
	pg_time_t	result;

	/* Need WALWriteLock, but shared lock is sufficient */
	LWLockAcquire(WALWriteLock, LW_SHARED);
	result = XLogCtl->lastSegSwitchTime;
	*lastSwitchLSN = XLogCtl->lastSegSwitchLSN;
	LWLockRelease(WALWriteLock);

	return result;
}

/*
 * This must be called ONCE during postmaster or standalone-backend shutdown
 */
void
ShutdownXLOG(int code, Datum arg)
{
	/*
	 * We should have an aux process resource owner to use, and we should not
	 * be in a transaction that's installed some other resowner.
	 */
	Assert(AuxProcessResourceOwner != NULL);
	Assert(CurrentResourceOwner == NULL ||
		   CurrentResourceOwner == AuxProcessResourceOwner);
	CurrentResourceOwner = AuxProcessResourceOwner;

	/* Don't be chatty in standalone mode */
	ereport(IsPostmasterEnvironment ? LOG : NOTICE,
			(errmsg("shutting down")));

	/*
	 * Signal walsenders to move to stopping state.
	 */
	WalSndInitStopping();

	/*
	 * Wait for WAL senders to be in stopping state.  This prevents commands
	 * from writing new WAL.
	 */
	WalSndWaitStopping();

	if (RecoveryInProgress())
		CreateRestartPoint(CHECKPOINT_IS_SHUTDOWN | CHECKPOINT_IMMEDIATE);
	else
	{
		/*
		 * If archiving is enabled, rotate the last XLOG file so that all the
		 * remaining records are archived (postmaster wakes up the archiver
		 * process one more time at the end of shutdown). The checkpoint
		 * record will go to the next XLOG file and won't be archived (yet).
		 */
		if (XLogArchivingActive() && XLogArchiveCommandSet())
			RequestXLogSwitch(false);

		CreateCheckPoint(CHECKPOINT_IS_SHUTDOWN | CHECKPOINT_IMMEDIATE);
	}
	ShutdownCLOG();
	ShutdownCommitTs();
	ShutdownSUBTRANS();
	ShutdownMultiXact();
}

/*
 * Log start of a checkpoint.
 */
static void
LogCheckpointStart(int flags, bool restartpoint)
{
	elog(LOG, "%s starting:%s%s%s%s%s%s%s%s",
		 restartpoint ? "restartpoint" : "checkpoint",
		 (flags & CHECKPOINT_IS_SHUTDOWN) ? " shutdown" : "",
		 (flags & CHECKPOINT_END_OF_RECOVERY) ? " end-of-recovery" : "",
		 (flags & CHECKPOINT_IMMEDIATE) ? " immediate" : "",
		 (flags & CHECKPOINT_FORCE) ? " force" : "",
		 (flags & CHECKPOINT_WAIT) ? " wait" : "",
		 (flags & CHECKPOINT_CAUSE_XLOG) ? " wal" : "",
		 (flags & CHECKPOINT_CAUSE_TIME) ? " time" : "",
		 (flags & CHECKPOINT_FLUSH_ALL) ? " flush-all" : "");
}

/*
 * Log end of a checkpoint.
 */
static void
LogCheckpointEnd(bool restartpoint)
{
	long		write_msecs,
				sync_msecs,
				total_msecs,
				longest_msecs,
				average_msecs;
	uint64		average_sync_time;

	CheckpointStats.ckpt_end_t = GetCurrentTimestamp();

	write_msecs = TimestampDifferenceMilliseconds(CheckpointStats.ckpt_write_t,
												  CheckpointStats.ckpt_sync_t);

	sync_msecs = TimestampDifferenceMilliseconds(CheckpointStats.ckpt_sync_t,
												 CheckpointStats.ckpt_sync_end_t);

	/* Accumulate checkpoint timing summary data, in milliseconds. */
	BgWriterStats.m_checkpoint_write_time += write_msecs;
	BgWriterStats.m_checkpoint_sync_time += sync_msecs;

	/*
	 * All of the published timing statistics are accounted for.  Only
	 * continue if a log message is to be written.
	 */
	if (!log_checkpoints)
		return;

	total_msecs = TimestampDifferenceMilliseconds(CheckpointStats.ckpt_start_t,
												  CheckpointStats.ckpt_end_t);

	/*
	 * Timing values returned from CheckpointStats are in microseconds.
	 * Convert to milliseconds for consistent printing.
	 */
	longest_msecs = (long) ((CheckpointStats.ckpt_longest_sync + 999) / 1000);

	average_sync_time = 0;
	if (CheckpointStats.ckpt_sync_rels > 0)
		average_sync_time = CheckpointStats.ckpt_agg_sync_time /
			CheckpointStats.ckpt_sync_rels;
	average_msecs = (long) ((average_sync_time + 999) / 1000);

	elog(LOG, "%s complete: wrote %d buffers (%.1f%%); "
		 "%d WAL file(s) added, %d removed, %d recycled; "
		 "write=%ld.%03d s, sync=%ld.%03d s, total=%ld.%03d s; "
		 "sync files=%d, longest=%ld.%03d s, average=%ld.%03d s; "
		 "distance=%d kB, estimate=%d kB",
		 restartpoint ? "restartpoint" : "checkpoint",
		 CheckpointStats.ckpt_bufs_written,
		 (double) CheckpointStats.ckpt_bufs_written * 100 / NBuffers,
		 CheckpointStats.ckpt_segs_added,
		 CheckpointStats.ckpt_segs_removed,
		 CheckpointStats.ckpt_segs_recycled,
		 write_msecs / 1000, (int) (write_msecs % 1000),
		 sync_msecs / 1000, (int) (sync_msecs % 1000),
		 total_msecs / 1000, (int) (total_msecs % 1000),
		 CheckpointStats.ckpt_sync_rels,
		 longest_msecs / 1000, (int) (longest_msecs % 1000),
		 average_msecs / 1000, (int) (average_msecs % 1000),
		 (int) (PrevCheckPointDistance / 1024.0),
		 (int) (CheckPointDistanceEstimate / 1024.0));
}

/*
 * Update the estimate of distance between checkpoints.
 *
 * The estimate is used to calculate the number of WAL segments to keep
 * preallocated, see XLOGfileslop().
 */
static void
UpdateCheckPointDistanceEstimate(uint64 nbytes)
{
	/*
	 * To estimate the number of segments consumed between checkpoints, keep a
	 * moving average of the amount of WAL generated in previous checkpoint
	 * cycles. However, if the load is bursty, with quiet periods and busy
	 * periods, we want to cater for the peak load. So instead of a plain
	 * moving average, let the average decline slowly if the previous cycle
	 * used less WAL than estimated, but bump it up immediately if it used
	 * more.
	 *
	 * When checkpoints are triggered by max_wal_size, this should converge to
	 * CheckpointSegments * wal_segment_size,
	 *
	 * Note: This doesn't pay any attention to what caused the checkpoint.
	 * Checkpoints triggered manually with CHECKPOINT command, or by e.g.
	 * starting a base backup, are counted the same as those created
	 * automatically. The slow-decline will largely mask them out, if they are
	 * not frequent. If they are frequent, it seems reasonable to count them
	 * in as any others; if you issue a manual checkpoint every 5 minutes and
	 * never let a timed checkpoint happen, it makes sense to base the
	 * preallocation on that 5 minute interval rather than whatever
	 * checkpoint_timeout is set to.
	 */
	PrevCheckPointDistance = nbytes;
	if (CheckPointDistanceEstimate < nbytes)
		CheckPointDistanceEstimate = nbytes;
	else
		CheckPointDistanceEstimate =
			(0.90 * CheckPointDistanceEstimate + 0.10 * (double) nbytes);
}

/*
 * Perform a checkpoint --- either during shutdown, or on-the-fly
 *
 * flags is a bitwise OR of the following:
 *	CHECKPOINT_IS_SHUTDOWN: checkpoint is for database shutdown.
 *	CHECKPOINT_END_OF_RECOVERY: checkpoint is for end of WAL recovery.
 *	CHECKPOINT_IMMEDIATE: finish the checkpoint ASAP,
 *		ignoring checkpoint_completion_target parameter.
 *	CHECKPOINT_FORCE: force a checkpoint even if no XLOG activity has occurred
 *		since the last one (implied by CHECKPOINT_IS_SHUTDOWN or
 *		CHECKPOINT_END_OF_RECOVERY).
 *	CHECKPOINT_FLUSH_ALL: also flush buffers of unlogged tables.
 *
 * Note: flags contains other bits, of interest here only for logging purposes.
 * In particular note that this routine is synchronous and does not pay
 * attention to CHECKPOINT_WAIT.
 *
 * If !shutdown then we are writing an online checkpoint. This is a very special
 * kind of operation and WAL record because the checkpoint action occurs over
 * a period of time yet logically occurs at just a single LSN. The logical
 * position of the WAL record (redo ptr) is the same or earlier than the
 * physical position. When we replay WAL we locate the checkpoint via its
 * physical position then read the redo ptr and actually start replay at the
 * earlier logical position. Note that we don't write *anything* to WAL at
 * the logical position, so that location could be any other kind of WAL record.
 * All of this mechanism allows us to continue working while we checkpoint.
 * As a result, timing of actions is critical here and be careful to note that
 * this function will likely take minutes to execute on a busy system.
 */
void
CreateCheckPoint(int flags)
{
	bool		shutdown;
	CheckPoint	checkPoint;
	XLogRecPtr	recptr;
	XLogSegNo	_logSegNo;
	XLogCtlInsert *Insert = &XLogCtl->Insert;
	uint32		freespace;
	XLogRecPtr	PriorRedoPtr;
	XLogRecPtr	curInsert;
	XLogRecPtr	last_important_lsn;
	VirtualTransactionId *vxids;
	int			nvxids;

	/*
	 * An end-of-recovery checkpoint is really a shutdown checkpoint, just
	 * issued at a different time.
	 */
	if (flags & (CHECKPOINT_IS_SHUTDOWN | CHECKPOINT_END_OF_RECOVERY))
		shutdown = true;
	else
		shutdown = false;

	/* sanity check */
	if (RecoveryInProgress() && (flags & CHECKPOINT_END_OF_RECOVERY) == 0)
		elog(ERROR, "can't create a checkpoint during recovery");

	/*
	 * Initialize InitXLogInsert working areas before entering the critical
	 * section.  Normally, this is done by the first call to
	 * RecoveryInProgress() or LocalSetXLogInsertAllowed(), but when creating
	 * an end-of-recovery checkpoint, the LocalSetXLogInsertAllowed call is
	 * done below in a critical section, and InitXLogInsert cannot be called
	 * in a critical section.
	 */
	InitXLogInsert();

	/*
	 * Acquire CheckpointLock to ensure only one checkpoint happens at a time.
	 * (This is just pro forma, since in the present system structure there is
	 * only one process that is allowed to issue checkpoints at any given
	 * time.)
	 */
	LWLockAcquire(CheckpointLock, LW_EXCLUSIVE);

	/*
	 * Prepare to accumulate statistics.
	 *
	 * Note: because it is possible for log_checkpoints to change while a
	 * checkpoint proceeds, we always accumulate stats, even if
	 * log_checkpoints is currently off.
	 */
	MemSet(&CheckpointStats, 0, sizeof(CheckpointStats));
	CheckpointStats.ckpt_start_t = GetCurrentTimestamp();

	/*
	 * Let smgr prepare for checkpoint; this has to happen outside the
	 * critical section and before we determine the REDO pointer.  Note that
	 * smgr must not do anything that'd have to be undone if we decide no
	 * checkpoint is needed.
	 */
	SyncPreCheckpoint();

	/*
	 * Use a critical section to force system panic if we have trouble.
	 */
	START_CRIT_SECTION();

	if (shutdown)
	{
		LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
		ControlFile->state = DB_SHUTDOWNING;
		ControlFile->time = (pg_time_t) time(NULL);
		UpdateControlFile();
		LWLockRelease(ControlFileLock);
	}

	/* Begin filling in the checkpoint WAL record */
	MemSet(&checkPoint, 0, sizeof(checkPoint));
	checkPoint.time = (pg_time_t) time(NULL);

	/*
	 * For Hot Standby, derive the oldestActiveXid before we fix the redo
	 * pointer. This allows us to begin accumulating changes to assemble our
	 * starting snapshot of locks and transactions.
	 */
	if (!shutdown && XLogStandbyInfoActive())
		checkPoint.oldestActiveXid = GetOldestActiveTransactionId();
	else
		checkPoint.oldestActiveXid = InvalidTransactionId;

	/*
	 * Get location of last important record before acquiring insert locks (as
	 * GetLastImportantRecPtr() also locks WAL locks).
	 */
	last_important_lsn = GetLastImportantRecPtr();

	/*
	 * We must block concurrent insertions while examining insert state to
	 * determine the checkpoint REDO pointer.
	 */
	WALInsertLockAcquireExclusive();
	curInsert = XLogBytePosToRecPtr(Insert->CurrBytePos);

	/*
	 * If this isn't a shutdown or forced checkpoint, and if there has been no
	 * WAL activity requiring a checkpoint, skip it.  The idea here is to
	 * avoid inserting duplicate checkpoints when the system is idle.
	 */
	if ((flags & (CHECKPOINT_IS_SHUTDOWN | CHECKPOINT_END_OF_RECOVERY |
				  CHECKPOINT_FORCE)) == 0)
	{
		if (last_important_lsn == ControlFile->checkPoint)
		{
			WALInsertLockRelease();
			LWLockRelease(CheckpointLock);
			END_CRIT_SECTION();
			ereport(DEBUG1,
					(errmsg("checkpoint skipped because system is idle")));
			return;
		}
	}

	/*
	 * An end-of-recovery checkpoint is created before anyone is allowed to
	 * write WAL. To allow us to write the checkpoint record, temporarily
	 * enable XLogInsertAllowed.  (This also ensures ThisTimeLineID is
	 * initialized, which we need here and in AdvanceXLInsertBuffer.)
	 */
	if (flags & CHECKPOINT_END_OF_RECOVERY)
		LocalSetXLogInsertAllowed();

	checkPoint.ThisTimeLineID = ThisTimeLineID;
	if (flags & CHECKPOINT_END_OF_RECOVERY)
		checkPoint.PrevTimeLineID = XLogCtl->PrevTimeLineID;
	else
		checkPoint.PrevTimeLineID = ThisTimeLineID;

	checkPoint.fullPageWrites = Insert->fullPageWrites;

	/*
	 * Compute new REDO record ptr = location of next XLOG record.
	 *
	 * NB: this is NOT necessarily where the checkpoint record itself will be,
	 * since other backends may insert more XLOG records while we're off doing
	 * the buffer flush work.  Those XLOG records are logically after the
	 * checkpoint, even though physically before it.  Got that?
	 */
	freespace = INSERT_FREESPACE(curInsert);
	if (freespace == 0)
	{
		if (XLogSegmentOffset(curInsert, wal_segment_size) == 0)
			curInsert += SizeOfXLogLongPHD;
		else
			curInsert += SizeOfXLogShortPHD;
	}
	checkPoint.redo = curInsert;

	/*
	 * Here we update the shared RedoRecPtr for future XLogInsert calls; this
	 * must be done while holding all the insertion locks.
	 *
	 * Note: if we fail to complete the checkpoint, RedoRecPtr will be left
	 * pointing past where it really needs to point.  This is okay; the only
	 * consequence is that XLogInsert might back up whole buffers that it
	 * didn't really need to.  We can't postpone advancing RedoRecPtr because
	 * XLogInserts that happen while we are dumping buffers must assume that
	 * their buffer changes are not included in the checkpoint.
	 */
	RedoRecPtr = XLogCtl->Insert.RedoRecPtr = checkPoint.redo;

	/*
	 * Now we can release the WAL insertion locks, allowing other xacts to
	 * proceed while we are flushing disk buffers.
	 */
	WALInsertLockRelease();

	/* Update the info_lck-protected copy of RedoRecPtr as well */
	SpinLockAcquire(&XLogCtl->info_lck);
	XLogCtl->RedoRecPtr = checkPoint.redo;
	SpinLockRelease(&XLogCtl->info_lck);

	/*
	 * If enabled, log checkpoint start.  We postpone this until now so as not
	 * to log anything if we decided to skip the checkpoint.
	 */
	if (log_checkpoints)
		LogCheckpointStart(flags, false);

	TRACE_POSTGRESQL_CHECKPOINT_START(flags);

	/*
	 * Get the other info we need for the checkpoint record.
	 *
	 * We don't need to save oldestClogXid in the checkpoint, it only matters
	 * for the short period in which clog is being truncated, and if we crash
	 * during that we'll redo the clog truncation and fix up oldestClogXid
	 * there.
	 */
	LWLockAcquire(XidGenLock, LW_SHARED);
	checkPoint.nextFullXid = ShmemVariableCache->nextFullXid;
	checkPoint.oldestXid = ShmemVariableCache->oldestXid;
	checkPoint.oldestXidDB = ShmemVariableCache->oldestXidDB;
	LWLockRelease(XidGenLock);

	LWLockAcquire(CommitTsLock, LW_SHARED);
	checkPoint.oldestCommitTsXid = ShmemVariableCache->oldestCommitTsXid;
	checkPoint.newestCommitTsXid = ShmemVariableCache->newestCommitTsXid;
	LWLockRelease(CommitTsLock);

	LWLockAcquire(OidGenLock, LW_SHARED);
	checkPoint.nextOid = ShmemVariableCache->nextOid;
	if (!shutdown)
		checkPoint.nextOid += ShmemVariableCache->oidCount;
	LWLockRelease(OidGenLock);

	MultiXactGetCheckptMulti(shutdown,
							 &checkPoint.nextMulti,
							 &checkPoint.nextMultiOffset,
							 &checkPoint.oldestMulti,
							 &checkPoint.oldestMultiDB);

	/*
	 * Having constructed the checkpoint record, ensure all shmem disk buffers
	 * and commit-log buffers are flushed to disk.
	 *
	 * This I/O could fail for various reasons.  If so, we will fail to
	 * complete the checkpoint, but there is no reason to force a system
	 * panic. Accordingly, exit critical section while doing it.
	 */
	END_CRIT_SECTION();

	/*
	 * In some cases there are groups of actions that must all occur on one
	 * side or the other of a checkpoint record. Before flushing the
	 * checkpoint record we must explicitly wait for any backend currently
	 * performing those groups of actions.
	 *
	 * One example is end of transaction, so we must wait for any transactions
	 * that are currently in commit critical sections.  If an xact inserted
	 * its commit record into XLOG just before the REDO point, then a crash
	 * restart from the REDO point would not replay that record, which means
	 * that our flushing had better include the xact's update of pg_xact.  So
	 * we wait till he's out of his commit critical section before proceeding.
	 * See notes in RecordTransactionCommit().
	 *
	 * Because we've already released the insertion locks, this test is a bit
	 * fuzzy: it is possible that we will wait for xacts we didn't really need
	 * to wait for.  But the delay should be short and it seems better to make
	 * checkpoint take a bit longer than to hold off insertions longer than
	 * necessary. (In fact, the whole reason we have this issue is that xact.c
	 * does commit record XLOG insertion and clog update as two separate steps
	 * protected by different locks, but again that seems best on grounds of
	 * minimizing lock contention.)
	 *
	 * A transaction that has not yet set delayChkpt when we look cannot be at
	 * risk, since he's not inserted his commit record yet; and one that's
	 * already cleared it is not at risk either, since he's done fixing clog
	 * and we will correctly flush the update below.  So we cannot miss any
	 * xacts we need to wait for.
	 */
	vxids = GetVirtualXIDsDelayingChkpt(&nvxids);
	if (nvxids > 0)
	{
		do
		{
			pg_usleep(10000L);	/* wait for 10 msec */
		} while (HaveVirtualXIDsDelayingChkpt(vxids, nvxids));
	}
	pfree(vxids);

	CheckPointGuts(checkPoint.redo, flags);

	vxids = GetVirtualXIDsDelayingChkptEnd(&nvxids);
	if (nvxids > 0)
	{
		do
		{
			pg_usleep(10000L);	/* wait for 10 msec */
		} while (HaveVirtualXIDsDelayingChkptEnd(vxids, nvxids));
	}
	pfree(vxids);

	/*
	 * Take a snapshot of running transactions and write this to WAL. This
	 * allows us to reconstruct the state of running transactions during
	 * archive recovery, if required. Skip, if this info disabled.
	 *
	 * If we are shutting down, or Startup process is completing crash
	 * recovery we don't need to write running xact data.
	 */
	if (!shutdown && XLogStandbyInfoActive())
		LogStandbySnapshot();

	START_CRIT_SECTION();

	/*
	 * Now insert the checkpoint record into XLOG.
	 */
	XLogBeginInsert();
	XLogRegisterData((char *) (&checkPoint), sizeof(checkPoint));
	recptr = XLogInsert(RM_XLOG_ID,
						shutdown ? XLOG_CHECKPOINT_SHUTDOWN :
						XLOG_CHECKPOINT_ONLINE);

	XLogFlush(recptr);

	/*
	 * We mustn't write any new WAL after a shutdown checkpoint, or it will be
	 * overwritten at next startup.  No-one should even try, this just allows
	 * sanity-checking.  In the case of an end-of-recovery checkpoint, we want
	 * to just temporarily disable writing until the system has exited
	 * recovery.
	 */
	if (shutdown)
	{
		if (flags & CHECKPOINT_END_OF_RECOVERY)
			LocalXLogInsertAllowed = -1;	/* return to "check" state */
		else
			LocalXLogInsertAllowed = 0; /* never again write WAL */
	}

	/*
	 * We now have ProcLastRecPtr = start of actual checkpoint record, recptr
	 * = end of actual checkpoint record.
	 */
	if (shutdown && checkPoint.redo != ProcLastRecPtr)
		ereport(PANIC,
				(errmsg("concurrent write-ahead log activity while database system is shutting down")));

	/*
	 * Remember the prior checkpoint's redo ptr for
	 * UpdateCheckPointDistanceEstimate()
	 */
	PriorRedoPtr = ControlFile->checkPointCopy.redo;

	/*
	 * Update the control file.
	 */
	LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
	if (shutdown)
		ControlFile->state = DB_SHUTDOWNED;
	ControlFile->checkPoint = ProcLastRecPtr;
	ControlFile->checkPointCopy = checkPoint;
	ControlFile->time = (pg_time_t) time(NULL);
	/* crash recovery should always recover to the end of WAL */
	ControlFile->minRecoveryPoint = InvalidXLogRecPtr;
	ControlFile->minRecoveryPointTLI = 0;

	/*
	 * Persist unloggedLSN value. It's reset on crash recovery, so this goes
	 * unused on non-shutdown checkpoints, but seems useful to store it always
	 * for debugging purposes.
	 */
	SpinLockAcquire(&XLogCtl->ulsn_lck);
	ControlFile->unloggedLSN = XLogCtl->unloggedLSN;
	SpinLockRelease(&XLogCtl->ulsn_lck);

	UpdateControlFile();
	LWLockRelease(ControlFileLock);

	/* Update shared-memory copy of checkpoint XID/epoch */
	SpinLockAcquire(&XLogCtl->info_lck);
	XLogCtl->ckptFullXid = checkPoint.nextFullXid;
	SpinLockRelease(&XLogCtl->info_lck);

	/*
	 * We are now done with critical updates; no need for system panic if we
	 * have trouble while fooling with old log segments.
	 */
	END_CRIT_SECTION();

	/*
	 * Let smgr do post-checkpoint cleanup (eg, deleting old files).
	 */
	SyncPostCheckpoint();

	/*
	 * Update the average distance between checkpoints if the prior checkpoint
	 * exists.
	 */
	if (PriorRedoPtr != InvalidXLogRecPtr)
		UpdateCheckPointDistanceEstimate(RedoRecPtr - PriorRedoPtr);

	/*
	 * Delete old log files, those no longer needed for last checkpoint to
	 * prevent the disk holding the xlog from growing full.
	 */
	XLByteToSeg(RedoRecPtr, _logSegNo, wal_segment_size);
	KeepLogSeg(recptr, &_logSegNo);
	if (InvalidateObsoleteReplicationSlots(_logSegNo))
	{
		/*
		 * Some slots have been invalidated; recalculate the old-segment
		 * horizon, starting again from RedoRecPtr.
		 */
		XLByteToSeg(RedoRecPtr, _logSegNo, wal_segment_size);
		KeepLogSeg(recptr, &_logSegNo);
	}
	_logSegNo--;
	RemoveOldXlogFiles(_logSegNo, RedoRecPtr, recptr);

	/*
	 * Make more log segments if needed.  (Do this after recycling old log
	 * segments, since that may supply some of the needed files.)
	 */
	if (!shutdown)
		PreallocXlogFiles(recptr);

	/*
	 * Truncate pg_subtrans if possible.  We can throw away all data before
	 * the oldest XMIN of any running transaction.  No future transaction will
	 * attempt to reference any pg_subtrans entry older than that (see Asserts
	 * in subtrans.c).  During recovery, though, we mustn't do this because
	 * StartupSUBTRANS hasn't been called yet.
	 */
	if (!RecoveryInProgress())
		TruncateSUBTRANS(GetOldestXmin(NULL, PROCARRAY_FLAGS_DEFAULT));

	/* Real work is done, but log and update stats before releasing lock. */
	LogCheckpointEnd(false);

	TRACE_POSTGRESQL_CHECKPOINT_DONE(CheckpointStats.ckpt_bufs_written,
									 NBuffers,
									 CheckpointStats.ckpt_segs_added,
									 CheckpointStats.ckpt_segs_removed,
									 CheckpointStats.ckpt_segs_recycled);

	LWLockRelease(CheckpointLock);
}

/*
 * Mark the end of recovery in WAL though without running a full checkpoint.
 * We can expect that a restartpoint is likely to be in progress as we
 * do this, though we are unwilling to wait for it to complete. So be
 * careful to avoid taking the CheckpointLock anywhere here.
 *
 * CreateRestartPoint() allows for the case where recovery may end before
 * the restartpoint completes so there is no concern of concurrent behaviour.
 */
static void
CreateEndOfRecoveryRecord(void)
{
	xl_end_of_recovery xlrec;
	XLogRecPtr	recptr;

	/* sanity check */
	if (!RecoveryInProgress())
		elog(ERROR, "can only be used to end recovery");

	xlrec.end_time = GetCurrentTimestamp();

	WALInsertLockAcquireExclusive();
	xlrec.ThisTimeLineID = ThisTimeLineID;
	xlrec.PrevTimeLineID = XLogCtl->PrevTimeLineID;
	WALInsertLockRelease();

	LocalSetXLogInsertAllowed();

	START_CRIT_SECTION();

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, sizeof(xl_end_of_recovery));
	recptr = XLogInsert(RM_XLOG_ID, XLOG_END_OF_RECOVERY);

	XLogFlush(recptr);

	/*
	 * Update the control file so that crash recovery can follow the timeline
	 * changes to this point.
	 */
	LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
	ControlFile->time = (pg_time_t) time(NULL);
	ControlFile->minRecoveryPoint = recptr;
	ControlFile->minRecoveryPointTLI = ThisTimeLineID;
	UpdateControlFile();
	LWLockRelease(ControlFileLock);

	END_CRIT_SECTION();

	LocalXLogInsertAllowed = -1;	/* return to "check" state */
}

/*
 * Write an OVERWRITE_CONTRECORD message.
 *
 * When on WAL replay we expect a continuation record at the start of a page
 * that is not there, recovery ends and WAL writing resumes at that point.
 * But it's wrong to resume writing new WAL back at the start of the record
 * that was broken, because downstream consumers of that WAL (physical
 * replicas) are not prepared to "rewind".  So the first action after
 * finishing replay of all valid WAL must be to write a record of this type
 * at the point where the contrecord was missing; to support xlogreader
 * detecting the special case, XLP_FIRST_IS_OVERWRITE_CONTRECORD is also added
 * to the page header where the record occurs.  xlogreader has an ad-hoc
 * mechanism to report metadata about the broken record, which is what we
 * use here.
 *
 * At replay time, XLP_FIRST_IS_OVERWRITE_CONTRECORD instructs xlogreader to
 * skip the record it was reading, and pass back the LSN of the skipped
 * record, so that its caller can verify (on "replay" of that record) that the
 * XLOG_OVERWRITE_CONTRECORD matches what was effectively overwritten.
 */
static XLogRecPtr
CreateOverwriteContrecordRecord(XLogRecPtr aborted_lsn)
{
	xl_overwrite_contrecord xlrec;
	XLogRecPtr	recptr;

	/* sanity check */
	if (!RecoveryInProgress())
		elog(ERROR, "can only be used at end of recovery");

	xlrec.overwritten_lsn = aborted_lsn;
	xlrec.overwrite_time = GetCurrentTimestamp();

	START_CRIT_SECTION();

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, sizeof(xl_overwrite_contrecord));

	recptr = XLogInsert(RM_XLOG_ID, XLOG_OVERWRITE_CONTRECORD);

	XLogFlush(recptr);

	END_CRIT_SECTION();

	return recptr;
}

/*
 * Flush all data in shared memory to disk, and fsync
 *
 * This is the common code shared between regular checkpoints and
 * recovery restartpoints.
 */
static void
CheckPointGuts(XLogRecPtr checkPointRedo, int flags)
{
	CheckPointCLOG();
	CheckPointCommitTs();
	CheckPointSUBTRANS();
	CheckPointMultiXact();
	CheckPointPredicate();
	CheckPointRelationMap();
	CheckPointReplicationSlots();
	CheckPointSnapBuild();
	CheckPointLogicalRewriteHeap();
	CheckPointBuffers(flags);	/* performs all required fsyncs */
	CheckPointReplicationOrigin();
	/* We deliberately delay 2PC checkpointing as long as possible */
	CheckPointTwoPhase(checkPointRedo);
}

/*
 * Save a checkpoint for recovery restart if appropriate
 *
 * This function is called each time a checkpoint record is read from XLOG.
 * It must determine whether the checkpoint represents a safe restartpoint or
 * not.  If so, the checkpoint record is stashed in shared memory so that
 * CreateRestartPoint can consult it.  (Note that the latter function is
 * executed by the checkpointer, while this one will be executed by the
 * startup process.)
 */
static void
RecoveryRestartPoint(const CheckPoint *checkPoint)
{
	/*
	 * Also refrain from creating a restartpoint if we have seen any
	 * references to non-existent pages. Restarting recovery from the
	 * restartpoint would not see the references, so we would lose the
	 * cross-check that the pages belonged to a relation that was dropped
	 * later.
	 */
	if (XLogHaveInvalidPages())
	{
		elog(trace_recovery(DEBUG2),
			 "could not record restart point at %X/%X because there "
			 "are unresolved references to invalid pages",
			 (uint32) (checkPoint->redo >> 32),
			 (uint32) checkPoint->redo);
		return;
	}

	/*
	 * Copy the checkpoint record to shared memory, so that checkpointer can
	 * work out the next time it wants to perform a restartpoint.
	 */
	SpinLockAcquire(&XLogCtl->info_lck);
	XLogCtl->lastCheckPointRecPtr = ReadRecPtr;
	XLogCtl->lastCheckPointEndPtr = EndRecPtr;
	XLogCtl->lastCheckPoint = *checkPoint;
	SpinLockRelease(&XLogCtl->info_lck);
}

/*
 * Establish a restartpoint if possible.
 *
 * This is similar to CreateCheckPoint, but is used during WAL recovery
 * to establish a point from which recovery can roll forward without
 * replaying the entire recovery log.
 *
 * Returns true if a new restartpoint was established. We can only establish
 * a restartpoint if we have replayed a safe checkpoint record since last
 * restartpoint.
 */
bool
CreateRestartPoint(int flags)
{
	XLogRecPtr	lastCheckPointRecPtr;
	XLogRecPtr	lastCheckPointEndPtr;
	CheckPoint	lastCheckPoint;
	XLogRecPtr	PriorRedoPtr;
	XLogRecPtr	receivePtr;
	XLogRecPtr	replayPtr;
	TimeLineID	replayTLI;
	XLogRecPtr	endptr;
	XLogSegNo	_logSegNo;
	TimestampTz xtime;

	/*
	 * Acquire CheckpointLock to ensure only one restartpoint or checkpoint
	 * happens at a time.
	 */
	LWLockAcquire(CheckpointLock, LW_EXCLUSIVE);

	/* Get a local copy of the last safe checkpoint record. */
	SpinLockAcquire(&XLogCtl->info_lck);
	lastCheckPointRecPtr = XLogCtl->lastCheckPointRecPtr;
	lastCheckPointEndPtr = XLogCtl->lastCheckPointEndPtr;
	lastCheckPoint = XLogCtl->lastCheckPoint;
	SpinLockRelease(&XLogCtl->info_lck);

	/*
	 * Check that we're still in recovery mode. It's ok if we exit recovery
	 * mode after this check, the restart point is valid anyway.
	 */
	if (!RecoveryInProgress())
	{
		ereport(DEBUG2,
				(errmsg("skipping restartpoint, recovery has already ended")));
		LWLockRelease(CheckpointLock);
		return false;
	}

	/*
	 * If the last checkpoint record we've replayed is already our last
	 * restartpoint, we can't perform a new restart point. We still update
	 * minRecoveryPoint in that case, so that if this is a shutdown restart
	 * point, we won't start up earlier than before. That's not strictly
	 * necessary, but when hot standby is enabled, it would be rather weird if
	 * the database opened up for read-only connections at a point-in-time
	 * before the last shutdown. Such time travel is still possible in case of
	 * immediate shutdown, though.
	 *
	 * We don't explicitly advance minRecoveryPoint when we do create a
	 * restartpoint. It's assumed that flushing the buffers will do that as a
	 * side-effect.
	 */
	if (XLogRecPtrIsInvalid(lastCheckPointRecPtr) ||
		lastCheckPoint.redo <= ControlFile->checkPointCopy.redo)
	{
		ereport(DEBUG2,
				(errmsg("skipping restartpoint, already performed at %X/%X",
						(uint32) (lastCheckPoint.redo >> 32),
						(uint32) lastCheckPoint.redo)));

		UpdateMinRecoveryPoint(InvalidXLogRecPtr, true);
		if (flags & CHECKPOINT_IS_SHUTDOWN)
		{
			LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
			ControlFile->state = DB_SHUTDOWNED_IN_RECOVERY;
			ControlFile->time = (pg_time_t) time(NULL);
			UpdateControlFile();
			LWLockRelease(ControlFileLock);
		}
		LWLockRelease(CheckpointLock);
		return false;
	}

	/*
	 * Update the shared RedoRecPtr so that the startup process can calculate
	 * the number of segments replayed since last restartpoint, and request a
	 * restartpoint if it exceeds CheckPointSegments.
	 *
	 * Like in CreateCheckPoint(), hold off insertions to update it, although
	 * during recovery this is just pro forma, because no WAL insertions are
	 * happening.
	 */
	WALInsertLockAcquireExclusive();
	RedoRecPtr = XLogCtl->Insert.RedoRecPtr = lastCheckPoint.redo;
	WALInsertLockRelease();

	/* Also update the info_lck-protected copy */
	SpinLockAcquire(&XLogCtl->info_lck);
	XLogCtl->RedoRecPtr = lastCheckPoint.redo;
	SpinLockRelease(&XLogCtl->info_lck);

	/*
	 * Prepare to accumulate statistics.
	 *
	 * Note: because it is possible for log_checkpoints to change while a
	 * checkpoint proceeds, we always accumulate stats, even if
	 * log_checkpoints is currently off.
	 */
	MemSet(&CheckpointStats, 0, sizeof(CheckpointStats));
	CheckpointStats.ckpt_start_t = GetCurrentTimestamp();

	if (log_checkpoints)
		LogCheckpointStart(flags, true);

	CheckPointGuts(lastCheckPoint.redo, flags);

	/*
	 * Remember the prior checkpoint's redo ptr for
	 * UpdateCheckPointDistanceEstimate()
	 */
	PriorRedoPtr = ControlFile->checkPointCopy.redo;

	/*
	 * Update pg_control, using current time.  Check that it still shows an
	 * older checkpoint, else do nothing; this is a quick hack to make sure
	 * nothing really bad happens if somehow we get here after the
	 * end-of-recovery checkpoint.
	 */
	LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
	if (ControlFile->checkPointCopy.redo < lastCheckPoint.redo)
	{
		/*
		 * Update the checkpoint information.  We do this even if the cluster
		 * does not show DB_IN_ARCHIVE_RECOVERY to match with the set of WAL
		 * segments recycled below.
		 */
		ControlFile->checkPoint = lastCheckPointRecPtr;
		ControlFile->checkPointCopy = lastCheckPoint;
		ControlFile->time = (pg_time_t) time(NULL);

		/*
		 * Ensure minRecoveryPoint is past the checkpoint record and update it
		 * if the control file still shows DB_IN_ARCHIVE_RECOVERY.  Normally,
		 * this will have happened already while writing out dirty buffers,
		 * but not necessarily - e.g. because no buffers were dirtied.  We do
		 * this because a non-exclusive base backup uses minRecoveryPoint to
		 * determine which WAL files must be included in the backup, and the
		 * file (or files) containing the checkpoint record must be included,
		 * at a minimum. Note that for an ordinary restart of recovery there's
		 * no value in having the minimum recovery point any earlier than this
		 * anyway, because redo will begin just after the checkpoint record.
		 * this because a non-exclusive base backup uses minRecoveryPoint to
		 * determine which WAL files must be included in the backup, and the
		 * file (or files) containing the checkpoint record must be included,
		 * at a minimum. Note that for an ordinary restart of recovery there's
		 * no value in having the minimum recovery point any earlier than this
		 * anyway, because redo will begin just after the checkpoint record.
		 */
		if (ControlFile->state == DB_IN_ARCHIVE_RECOVERY)
		{
			if (ControlFile->minRecoveryPoint < lastCheckPointEndPtr)
			{
				ControlFile->minRecoveryPoint = lastCheckPointEndPtr;
				ControlFile->minRecoveryPointTLI = lastCheckPoint.ThisTimeLineID;

				/* update local copy */
				minRecoveryPoint = ControlFile->minRecoveryPoint;
				minRecoveryPointTLI = ControlFile->minRecoveryPointTLI;
			}
			if (flags & CHECKPOINT_IS_SHUTDOWN)
				ControlFile->state = DB_SHUTDOWNED_IN_RECOVERY;
		}
		UpdateControlFile();
	}
	LWLockRelease(ControlFileLock);

	/*
	 * Update the average distance between checkpoints/restartpoints if the
	 * prior checkpoint exists.
	 */
	if (PriorRedoPtr != InvalidXLogRecPtr)
		UpdateCheckPointDistanceEstimate(RedoRecPtr - PriorRedoPtr);

	/*
	 * Delete old log files, those no longer needed for last restartpoint to
	 * prevent the disk holding the xlog from growing full.
	 */
	XLByteToSeg(RedoRecPtr, _logSegNo, wal_segment_size);

	/*
	 * Retreat _logSegNo using the current end of xlog replayed or received,
	 * whichever is later.
	 */
	receivePtr = GetWalRcvFlushRecPtr(NULL, NULL);
	replayPtr = GetXLogReplayRecPtr(&replayTLI);
	endptr = (receivePtr < replayPtr) ? replayPtr : receivePtr;
	KeepLogSeg(endptr, &_logSegNo);
	if (InvalidateObsoleteReplicationSlots(_logSegNo))
	{
		/*
		 * Some slots have been invalidated; recalculate the old-segment
		 * horizon, starting again from RedoRecPtr.
		 */
		XLByteToSeg(RedoRecPtr, _logSegNo, wal_segment_size);
		KeepLogSeg(endptr, &_logSegNo);
	}
	_logSegNo--;

	/*
	 * Try to recycle segments on a useful timeline. If we've been promoted
	 * since the beginning of this restartpoint, use the new timeline chosen
	 * at end of recovery (RecoveryInProgress() sets ThisTimeLineID in that
	 * case). If we're still in recovery, use the timeline we're currently
	 * replaying.
	 *
	 * There is no guarantee that the WAL segments will be useful on the
	 * current timeline; if recovery proceeds to a new timeline right after
	 * this, the pre-allocated WAL segments on this timeline will not be used,
	 * and will go wasted until recycled on the next restartpoint. We'll live
	 * with that.
	 */
	if (RecoveryInProgress())
		ThisTimeLineID = replayTLI;

	RemoveOldXlogFiles(_logSegNo, RedoRecPtr, endptr);

	/*
	 * Make more log segments if needed.  (Do this after recycling old log
	 * segments, since that may supply some of the needed files.)
	 */
	PreallocXlogFiles(endptr);

	/*
	 * ThisTimeLineID is normally not set when we're still in recovery.
	 * However, recycling/preallocating segments above needed ThisTimeLineID
	 * to determine which timeline to install the segments on. Reset it now,
	 * to restore the normal state of affairs for debugging purposes.
	 */
	if (RecoveryInProgress())
		ThisTimeLineID = 0;

	/*
	 * Truncate pg_subtrans if possible.  We can throw away all data before
	 * the oldest XMIN of any running transaction.  No future transaction will
	 * attempt to reference any pg_subtrans entry older than that (see Asserts
	 * in subtrans.c).  When hot standby is disabled, though, we mustn't do
	 * this because StartupSUBTRANS hasn't been called yet.
	 */
	if (EnableHotStandby)
		TruncateSUBTRANS(GetOldestXmin(NULL, PROCARRAY_FLAGS_DEFAULT));

	/* Real work is done, but log and update before releasing lock. */
	LogCheckpointEnd(true);

	xtime = GetLatestXTime();
	ereport((log_checkpoints ? LOG : DEBUG2),
			(errmsg("recovery restart point at %X/%X",
					(uint32) (lastCheckPoint.redo >> 32), (uint32) lastCheckPoint.redo),
			 xtime ? errdetail("Last completed transaction was at log time %s.",
							   timestamptz_to_str(xtime)) : 0));

	LWLockRelease(CheckpointLock);

	/*
	 * Finally, execute archive_cleanup_command, if any.
	 */
	if (archiveCleanupCommand && strcmp(archiveCleanupCommand, "") != 0)
		ExecuteRecoveryCommand(archiveCleanupCommand,
							   "archive_cleanup_command",
							   false);

	return true;
}

/*
 * Report availability of WAL for the given target LSN
 *		(typically a slot's restart_lsn)
 *
 * Returns one of the following enum values:
 *
 * * WALAVAIL_RESERVED means targetLSN is available and it is in the range of
 *   max_wal_size.
 *
 * * WALAVAIL_EXTENDED means it is still available by preserving extra
 *   segments beyond max_wal_size. If max_slot_wal_keep_size is smaller
 *   than max_wal_size, this state is not returned.
 *
 * * WALAVAIL_UNRESERVED means it is being lost and the next checkpoint will
 *   remove reserved segments. The walsender using this slot may return to the
 *   above.
 *
 * * WALAVAIL_REMOVED means it has been removed. A replication stream on
 *   a slot with this LSN cannot continue after a restart.
 *
 * * WALAVAIL_INVALID_LSN means the slot hasn't been set to reserve WAL.
 */
WALAvailability
GetWALAvailability(XLogRecPtr targetLSN)
{
	XLogRecPtr	currpos;		/* current write LSN */
	XLogSegNo	currSeg;		/* segid of currpos */
	XLogSegNo	targetSeg;		/* segid of targetLSN */
	XLogSegNo	oldestSeg;		/* actual oldest segid */
	XLogSegNo	oldestSegMaxWalSize;	/* oldest segid kept by max_wal_size */
	XLogSegNo	oldestSlotSeg;	/* oldest segid kept by slot */
	uint64		keepSegs;

	/*
	 * slot does not reserve WAL. Either deactivated, or has never been active
	 */
	if (XLogRecPtrIsInvalid(targetLSN))
		return WALAVAIL_INVALID_LSN;

	/*
	 * Calculate the oldest segment currently reserved by all slots,
	 * considering wal_keep_size and max_slot_wal_keep_size.  Initialize
	 * oldestSlotSeg to the current segment.
	 */
	currpos = GetXLogWriteRecPtr();
	XLByteToSeg(currpos, oldestSlotSeg, wal_segment_size);
	KeepLogSeg(currpos, &oldestSlotSeg);

	/*
	 * Find the oldest extant segment file. We get 1 until checkpoint removes
	 * the first WAL segment file since startup, which causes the status being
	 * wrong under certain abnormal conditions but that doesn't actually harm.
	 */
	oldestSeg = XLogGetLastRemovedSegno() + 1;

	/* calculate oldest segment by max_wal_size */
	XLByteToSeg(currpos, currSeg, wal_segment_size);
	keepSegs = ConvertToXSegs(max_wal_size_mb, wal_segment_size) + 1;

	if (currSeg > keepSegs)
		oldestSegMaxWalSize = currSeg - keepSegs;
	else
		oldestSegMaxWalSize = 1;

	/* the segment we care about */
	XLByteToSeg(targetLSN, targetSeg, wal_segment_size);

	/*
	 * No point in returning reserved or extended status values if the
	 * targetSeg is known to be lost.
	 */
	if (targetSeg >= oldestSlotSeg)
	{
		/* show "reserved" when targetSeg is within max_wal_size */
		if (targetSeg >= oldestSegMaxWalSize)
			return WALAVAIL_RESERVED;

		/* being retained by slots exceeding max_wal_size */
		return WALAVAIL_EXTENDED;
	}

	/* WAL segments are no longer retained but haven't been removed yet */
	if (targetSeg >= oldestSeg)
		return WALAVAIL_UNRESERVED;

	/* Definitely lost */
	return WALAVAIL_REMOVED;
}


/*
 * Retreat *logSegNo to the last segment that we need to retain because of
 * either wal_keep_size or replication slots.
 *
 * This is calculated by subtracting wal_keep_size from the given xlog
 * location, recptr and by making sure that that result is below the
 * requirement of replication slots.  For the latter criterion we do consider
 * the effects of max_slot_wal_keep_size: reserve at most that much space back
 * from recptr.
 *
 * Note about replication slots: if this function calculates a value
 * that's further ahead than what slots need reserved, then affected
 * slots need to be invalidated and this function invoked again.
 * XXX it might be a good idea to rewrite this function so that
 * invalidation is optionally done here, instead.
 */
static void
KeepLogSeg(XLogRecPtr recptr, XLogSegNo *logSegNo)
{
	XLogSegNo	currSegNo;
	XLogSegNo	segno;
	XLogRecPtr	keep;

	XLByteToSeg(recptr, currSegNo, wal_segment_size);
	segno = currSegNo;

	/*
	 * Calculate how many segments are kept by slots first, adjusting for
	 * max_slot_wal_keep_size.
	 */
	keep = XLogGetReplicationSlotMinimumLSN();
	if (keep != InvalidXLogRecPtr && keep < recptr)
	{
		XLByteToSeg(keep, segno, wal_segment_size);

		/* Cap by max_slot_wal_keep_size ... */
		if (max_slot_wal_keep_size_mb >= 0)
		{
			uint64		slot_keep_segs;

			slot_keep_segs =
				ConvertToXSegs(max_slot_wal_keep_size_mb, wal_segment_size);

			if (currSegNo - segno > slot_keep_segs)
				segno = currSegNo - slot_keep_segs;
		}
	}

	/* but, keep at least wal_keep_size if that's set */
	if (wal_keep_size_mb > 0)
	{
		uint64		keep_segs;

		keep_segs = ConvertToXSegs(wal_keep_size_mb, wal_segment_size);
		if (currSegNo - segno < keep_segs)
		{
			/* avoid underflow, don't go below 1 */
			if (currSegNo <= keep_segs)
				segno = 1;
			else
				segno = currSegNo - keep_segs;
		}
	}

	/* don't delete WAL segments newer than the calculated segment */
	if (segno < *logSegNo)
		*logSegNo = segno;
}

/*
 * Write a NEXTOID log record
 */
void
XLogPutNextOid(Oid nextOid)
{
	XLogBeginInsert();
	XLogRegisterData((char *) (&nextOid), sizeof(Oid));
	(void) XLogInsert(RM_XLOG_ID, XLOG_NEXTOID);

	/*
	 * We need not flush the NEXTOID record immediately, because any of the
	 * just-allocated OIDs could only reach disk as part of a tuple insert or
	 * update that would have its own XLOG record that must follow the NEXTOID
	 * record.  Therefore, the standard buffer LSN interlock applied to those
	 * records will ensure no such OID reaches disk before the NEXTOID record
	 * does.
	 *
	 * Note, however, that the above statement only covers state "within" the
	 * database.  When we use a generated OID as a file or directory name, we
	 * are in a sense violating the basic WAL rule, because that filesystem
	 * change may reach disk before the NEXTOID WAL record does.  The impact
	 * of this is that if a database crash occurs immediately afterward, we
	 * might after restart re-generate the same OID and find that it conflicts
	 * with the leftover file or directory.  But since for safety's sake we
	 * always loop until finding a nonconflicting filename, this poses no real
	 * problem in practice. See pgsql-hackers discussion 27-Sep-2006.
	 */
}

/*
 * Write an XLOG SWITCH record.
 *
 * Here we just blindly issue an XLogInsert request for the record.
 * All the magic happens inside XLogInsert.
 *
 * The return value is either the end+1 address of the switch record,
 * or the end+1 address of the prior segment if we did not need to
 * write a switch record because we are already at segment start.
 */
XLogRecPtr
RequestXLogSwitch(bool mark_unimportant)
{
	XLogRecPtr	RecPtr;

	/* XLOG SWITCH has no data */
	XLogBeginInsert();

	if (mark_unimportant)
		XLogSetRecordFlags(XLOG_MARK_UNIMPORTANT);
	RecPtr = XLogInsert(RM_XLOG_ID, XLOG_SWITCH);

	return RecPtr;
}

/*
 * Write a RESTORE POINT record
 */
XLogRecPtr
XLogRestorePoint(const char *rpName)
{
	XLogRecPtr	RecPtr;
	xl_restore_point xlrec;

	xlrec.rp_time = GetCurrentTimestamp();
	strlcpy(xlrec.rp_name, rpName, MAXFNAMELEN);

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, sizeof(xl_restore_point));

	RecPtr = XLogInsert(RM_XLOG_ID, XLOG_RESTORE_POINT);

	ereport(LOG,
			(errmsg("restore point \"%s\" created at %X/%X",
					rpName, (uint32) (RecPtr >> 32), (uint32) RecPtr)));

	return RecPtr;
}

/*
 * Check if any of the GUC parameters that are critical for hot standby
 * have changed, and update the value in pg_control file if necessary.
 */
static void
XLogReportParameters(void)
{
	if (wal_level != ControlFile->wal_level ||
		wal_log_hints != ControlFile->wal_log_hints ||
		MaxConnections != ControlFile->MaxConnections ||
		max_worker_processes != ControlFile->max_worker_processes ||
		max_wal_senders != ControlFile->max_wal_senders ||
		max_prepared_xacts != ControlFile->max_prepared_xacts ||
		max_locks_per_xact != ControlFile->max_locks_per_xact ||
		track_commit_timestamp != ControlFile->track_commit_timestamp)
	{
		/*
		 * The change in number of backend slots doesn't need to be WAL-logged
		 * if archiving is not enabled, as you can't start archive recovery
		 * with wal_level=minimal anyway. We don't really care about the
		 * values in pg_control either if wal_level=minimal, but seems better
		 * to keep them up-to-date to avoid confusion.
		 */
		if (wal_level != ControlFile->wal_level || XLogIsNeeded())
		{
			xl_parameter_change xlrec;
			XLogRecPtr	recptr;

			xlrec.MaxConnections = MaxConnections;
			xlrec.max_worker_processes = max_worker_processes;
			xlrec.max_wal_senders = max_wal_senders;
			xlrec.max_prepared_xacts = max_prepared_xacts;
			xlrec.max_locks_per_xact = max_locks_per_xact;
			xlrec.wal_level = wal_level;
			xlrec.wal_log_hints = wal_log_hints;
			xlrec.track_commit_timestamp = track_commit_timestamp;

			XLogBeginInsert();
			XLogRegisterData((char *) &xlrec, sizeof(xlrec));

			recptr = XLogInsert(RM_XLOG_ID, XLOG_PARAMETER_CHANGE);
			XLogFlush(recptr);
		}

		LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);

		ControlFile->MaxConnections = MaxConnections;
		ControlFile->max_worker_processes = max_worker_processes;
		ControlFile->max_wal_senders = max_wal_senders;
		ControlFile->max_prepared_xacts = max_prepared_xacts;
		ControlFile->max_locks_per_xact = max_locks_per_xact;
		ControlFile->wal_level = wal_level;
		ControlFile->wal_log_hints = wal_log_hints;
		ControlFile->track_commit_timestamp = track_commit_timestamp;
		UpdateControlFile();

		LWLockRelease(ControlFileLock);
	}
}

/*
 * Update full_page_writes in shared memory, and write an
 * XLOG_FPW_CHANGE record if necessary.
 *
 * Note: this function assumes there is no other process running
 * concurrently that could update it.
 */
void
UpdateFullPageWrites(void)
{
	XLogCtlInsert *Insert = &XLogCtl->Insert;
	bool		recoveryInProgress;

	/*
	 * Do nothing if full_page_writes has not been changed.
	 *
	 * It's safe to check the shared full_page_writes without the lock,
	 * because we assume that there is no concurrently running process which
	 * can update it.
	 */
	if (fullPageWrites == Insert->fullPageWrites)
		return;

	/*
	 * Perform this outside critical section so that the WAL insert
	 * initialization done by RecoveryInProgress() doesn't trigger an
	 * assertion failure.
	 */
	recoveryInProgress = RecoveryInProgress();

	START_CRIT_SECTION();

	/*
	 * It's always safe to take full page images, even when not strictly
	 * required, but not the other round. So if we're setting full_page_writes
	 * to true, first set it true and then write the WAL record. If we're
	 * setting it to false, first write the WAL record and then set the global
	 * flag.
	 */
	if (fullPageWrites)
	{
		WALInsertLockAcquireExclusive();
		Insert->fullPageWrites = true;
		WALInsertLockRelease();
	}

	/*
	 * Write an XLOG_FPW_CHANGE record. This allows us to keep track of
	 * full_page_writes during archive recovery, if required.
	 */
	if (XLogStandbyInfoActive() && !recoveryInProgress)
	{
		XLogBeginInsert();
		XLogRegisterData((char *) (&fullPageWrites), sizeof(bool));

		XLogInsert(RM_XLOG_ID, XLOG_FPW_CHANGE);
	}

	if (!fullPageWrites)
	{
		WALInsertLockAcquireExclusive();
		Insert->fullPageWrites = false;
		WALInsertLockRelease();
	}
	END_CRIT_SECTION();
}

/*
 * Check that it's OK to switch to new timeline during recovery.
 *
 * 'lsn' is the address of the shutdown checkpoint record we're about to
 * replay. (Currently, timeline can only change at a shutdown checkpoint).
 */
static void
checkTimeLineSwitch(XLogRecPtr lsn, TimeLineID newTLI, TimeLineID prevTLI)
{
	/* Check that the record agrees on what the current (old) timeline is */
	if (prevTLI != ThisTimeLineID)
		ereport(PANIC,
				(errmsg("unexpected previous timeline ID %u (current timeline ID %u) in checkpoint record",
						prevTLI, ThisTimeLineID)));

	/*
	 * The new timeline better be in the list of timelines we expect to see,
	 * according to the timeline history. It should also not decrease.
	 */
	if (newTLI < ThisTimeLineID || !tliInHistory(newTLI, expectedTLEs))
		ereport(PANIC,
				(errmsg("unexpected timeline ID %u (after %u) in checkpoint record",
						newTLI, ThisTimeLineID)));

	/*
	 * If we have not yet reached min recovery point, and we're about to
	 * switch to a timeline greater than the timeline of the min recovery
	 * point: trouble. After switching to the new timeline, we could not
	 * possibly visit the min recovery point on the correct timeline anymore.
	 * This can happen if there is a newer timeline in the archive that
	 * branched before the timeline the min recovery point is on, and you
	 * attempt to do PITR to the new timeline.
	 */
	if (!XLogRecPtrIsInvalid(minRecoveryPoint) &&
		lsn < minRecoveryPoint &&
		newTLI > minRecoveryPointTLI)
		ereport(PANIC,
				(errmsg("unexpected timeline ID %u in checkpoint record, before reaching minimum recovery point %X/%X on timeline %u",
						newTLI,
						(uint32) (minRecoveryPoint >> 32),
						(uint32) minRecoveryPoint,
						minRecoveryPointTLI)));

	/* Looks good */
}

/*
 * XLOG resource manager's routines
 *
 * Definitions of info values are in include/catalog/pg_control.h, though
 * not all record types are related to control file updates.
 */
void
xlog_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	XLogRecPtr	lsn = record->EndRecPtr;

	/* in XLOG rmgr, backup blocks are only used by XLOG_FPI records */
	Assert(info == XLOG_FPI || info == XLOG_FPI_FOR_HINT ||
		   !XLogRecHasAnyBlockRefs(record));

	if (info == XLOG_NEXTOID)
	{
		Oid			nextOid;

		/*
		 * We used to try to take the maximum of ShmemVariableCache->nextOid
		 * and the recorded nextOid, but that fails if the OID counter wraps
		 * around.  Since no OID allocation should be happening during replay
		 * anyway, better to just believe the record exactly.  We still take
		 * OidGenLock while setting the variable, just in case.
		 */
		memcpy(&nextOid, XLogRecGetData(record), sizeof(Oid));
		LWLockAcquire(OidGenLock, LW_EXCLUSIVE);
		ShmemVariableCache->nextOid = nextOid;
		ShmemVariableCache->oidCount = 0;
		LWLockRelease(OidGenLock);
	}
	else if (info == XLOG_CHECKPOINT_SHUTDOWN)
	{
		CheckPoint	checkPoint;

		memcpy(&checkPoint, XLogRecGetData(record), sizeof(CheckPoint));
		/* In a SHUTDOWN checkpoint, believe the counters exactly */
		LWLockAcquire(XidGenLock, LW_EXCLUSIVE);
		ShmemVariableCache->nextFullXid = checkPoint.nextFullXid;
		LWLockRelease(XidGenLock);
		LWLockAcquire(OidGenLock, LW_EXCLUSIVE);
		ShmemVariableCache->nextOid = checkPoint.nextOid;
		ShmemVariableCache->oidCount = 0;
		LWLockRelease(OidGenLock);
		MultiXactSetNextMXact(checkPoint.nextMulti,
							  checkPoint.nextMultiOffset);

		MultiXactAdvanceOldest(checkPoint.oldestMulti,
							   checkPoint.oldestMultiDB);

		/*
		 * No need to set oldestClogXid here as well; it'll be set when we
		 * redo an xl_clog_truncate if it changed since initialization.
		 */
		SetTransactionIdLimit(checkPoint.oldestXid, checkPoint.oldestXidDB);

		/*
		 * If we see a shutdown checkpoint while waiting for an end-of-backup
		 * record, the backup was canceled and the end-of-backup record will
		 * never arrive.
		 */
		if (ArchiveRecoveryRequested &&
			!XLogRecPtrIsInvalid(ControlFile->backupStartPoint) &&
			XLogRecPtrIsInvalid(ControlFile->backupEndPoint))
			ereport(PANIC,
					(errmsg("online backup was canceled, recovery cannot continue")));

		/*
		 * If we see a shutdown checkpoint, we know that nothing was running
		 * on the master at this point. So fake-up an empty running-xacts
		 * record and use that here and now. Recover additional standby state
		 * for prepared transactions.
		 */
		if (standbyState >= STANDBY_INITIALIZED)
		{
			TransactionId *xids;
			int			nxids;
			TransactionId oldestActiveXID;
			TransactionId latestCompletedXid;
			RunningTransactionsData running;

			oldestActiveXID = PrescanPreparedTransactions(&xids, &nxids);

			/*
			 * Construct a RunningTransactions snapshot representing a shut
			 * down server, with only prepared transactions still alive. We're
			 * never overflowed at this point because all subxids are listed
			 * with their parent prepared transactions.
			 */
			running.xcnt = nxids;
			running.subxcnt = 0;
			running.subxid_overflow = false;
			running.nextXid = XidFromFullTransactionId(checkPoint.nextFullXid);
			running.oldestRunningXid = oldestActiveXID;
			latestCompletedXid = XidFromFullTransactionId(checkPoint.nextFullXid);
			TransactionIdRetreat(latestCompletedXid);
			Assert(TransactionIdIsNormal(latestCompletedXid));
			running.latestCompletedXid = latestCompletedXid;
			running.xids = xids;

			ProcArrayApplyRecoveryInfo(&running);

			StandbyRecoverPreparedTransactions();
		}

		/* ControlFile->checkPointCopy always tracks the latest ckpt XID */
		LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
		ControlFile->checkPointCopy.nextFullXid = checkPoint.nextFullXid;
		LWLockRelease(ControlFileLock);

		/* Update shared-memory copy of checkpoint XID/epoch */
		SpinLockAcquire(&XLogCtl->info_lck);
		XLogCtl->ckptFullXid = checkPoint.nextFullXid;
		SpinLockRelease(&XLogCtl->info_lck);

		/*
		 * We should've already switched to the new TLI before replaying this
		 * record.
		 */
		if (checkPoint.ThisTimeLineID != ThisTimeLineID)
			ereport(PANIC,
					(errmsg("unexpected timeline ID %u (should be %u) in checkpoint record",
							checkPoint.ThisTimeLineID, ThisTimeLineID)));

		RecoveryRestartPoint(&checkPoint);
	}
	else if (info == XLOG_CHECKPOINT_ONLINE)
	{
		CheckPoint	checkPoint;

		memcpy(&checkPoint, XLogRecGetData(record), sizeof(CheckPoint));
		/* In an ONLINE checkpoint, treat the XID counter as a minimum */
		LWLockAcquire(XidGenLock, LW_EXCLUSIVE);
		if (FullTransactionIdPrecedes(ShmemVariableCache->nextFullXid,
									  checkPoint.nextFullXid))
			ShmemVariableCache->nextFullXid = checkPoint.nextFullXid;
		LWLockRelease(XidGenLock);

		/*
		 * We ignore the nextOid counter in an ONLINE checkpoint, preferring
		 * to track OID assignment through XLOG_NEXTOID records.  The nextOid
		 * counter is from the start of the checkpoint and might well be stale
		 * compared to later XLOG_NEXTOID records.  We could try to take the
		 * maximum of the nextOid counter and our latest value, but since
		 * there's no particular guarantee about the speed with which the OID
		 * counter wraps around, that's a risky thing to do.  In any case,
		 * users of the nextOid counter are required to avoid assignment of
		 * duplicates, so that a somewhat out-of-date value should be safe.
		 */

		/* Handle multixact */
		MultiXactAdvanceNextMXact(checkPoint.nextMulti,
								  checkPoint.nextMultiOffset);

		/*
		 * NB: This may perform multixact truncation when replaying WAL
		 * generated by an older primary.
		 */
		MultiXactAdvanceOldest(checkPoint.oldestMulti,
							   checkPoint.oldestMultiDB);
		if (TransactionIdPrecedes(ShmemVariableCache->oldestXid,
								  checkPoint.oldestXid))
			SetTransactionIdLimit(checkPoint.oldestXid,
								  checkPoint.oldestXidDB);
		/* ControlFile->checkPointCopy always tracks the latest ckpt XID */
		LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
		ControlFile->checkPointCopy.nextFullXid = checkPoint.nextFullXid;
		LWLockRelease(ControlFileLock);

		/* Update shared-memory copy of checkpoint XID/epoch */
		SpinLockAcquire(&XLogCtl->info_lck);
		XLogCtl->ckptFullXid = checkPoint.nextFullXid;
		SpinLockRelease(&XLogCtl->info_lck);

		/* TLI should not change in an on-line checkpoint */
		if (checkPoint.ThisTimeLineID != ThisTimeLineID)
			ereport(PANIC,
					(errmsg("unexpected timeline ID %u (should be %u) in checkpoint record",
							checkPoint.ThisTimeLineID, ThisTimeLineID)));

		RecoveryRestartPoint(&checkPoint);
	}
	else if (info == XLOG_OVERWRITE_CONTRECORD)
	{
		xl_overwrite_contrecord xlrec;

		memcpy(&xlrec, XLogRecGetData(record), sizeof(xl_overwrite_contrecord));
		VerifyOverwriteContrecord(&xlrec, record);
	}
	else if (info == XLOG_END_OF_RECOVERY)
	{
		xl_end_of_recovery xlrec;

		memcpy(&xlrec, XLogRecGetData(record), sizeof(xl_end_of_recovery));

		/*
		 * For Hot Standby, we could treat this like a Shutdown Checkpoint,
		 * but this case is rarer and harder to test, so the benefit doesn't
		 * outweigh the potential extra cost of maintenance.
		 */

		/*
		 * We should've already switched to the new TLI before replaying this
		 * record.
		 */
		if (xlrec.ThisTimeLineID != ThisTimeLineID)
			ereport(PANIC,
					(errmsg("unexpected timeline ID %u (should be %u) in checkpoint record",
							xlrec.ThisTimeLineID, ThisTimeLineID)));
	}
	else if (info == XLOG_NOOP)
	{
		/* nothing to do here */
	}
	else if (info == XLOG_SWITCH)
	{
		/* nothing to do here */
	}
	else if (info == XLOG_RESTORE_POINT)
	{
		/* nothing to do here */
	}
	else if (info == XLOG_FPI || info == XLOG_FPI_FOR_HINT)
	{
		/*
		 * Full-page image (FPI) records contain nothing else but a backup
		 * block (or multiple backup blocks). Every block reference must
		 * include a full-page image - otherwise there would be no point in
		 * this record.
		 *
		 * No recovery conflicts are generated by these generic records - if a
		 * resource manager needs to generate conflicts, it has to define a
		 * separate WAL record type and redo routine.
		 *
		 * XLOG_FPI_FOR_HINT records are generated when a page needs to be
		 * WAL- logged because of a hint bit update. They are only generated
		 * when checksums are enabled. There is no difference in handling
		 * XLOG_FPI and XLOG_FPI_FOR_HINT records, they use a different info
		 * code just to distinguish them for statistics purposes.
		 */
		for (uint8 block_id = 0; block_id <= record->max_block_id; block_id++)
		{
			Buffer		buffer;

			if (XLogReadBufferForRedo(record, block_id, &buffer) != BLK_RESTORED)
				elog(ERROR, "unexpected XLogReadBufferForRedo result when restoring backup block");
			UnlockReleaseBuffer(buffer);
		}
	}
	else if (info == XLOG_BACKUP_END)
	{
		XLogRecPtr	startpoint;

		memcpy(&startpoint, XLogRecGetData(record), sizeof(startpoint));

		if (ControlFile->backupStartPoint == startpoint)
		{
			/*
			 * We have reached the end of base backup, the point where
			 * pg_stop_backup() was done. The data on disk is now consistent.
			 * Reset backupStartPoint, and update minRecoveryPoint to make
			 * sure we don't allow starting up at an earlier point even if
			 * recovery is stopped and restarted soon after this.
			 */
			elog(DEBUG1, "end of backup reached");

			LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);

			if (ControlFile->minRecoveryPoint < lsn)
			{
				ControlFile->minRecoveryPoint = lsn;
				ControlFile->minRecoveryPointTLI = ThisTimeLineID;
			}
			ControlFile->backupStartPoint = InvalidXLogRecPtr;
			ControlFile->backupEndRequired = false;
			UpdateControlFile();

			LWLockRelease(ControlFileLock);
		}
	}
	else if (info == XLOG_PARAMETER_CHANGE)
	{
		xl_parameter_change xlrec;

		/* Update our copy of the parameters in pg_control */
		memcpy(&xlrec, XLogRecGetData(record), sizeof(xl_parameter_change));

		LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
		ControlFile->MaxConnections = xlrec.MaxConnections;
		ControlFile->max_worker_processes = xlrec.max_worker_processes;
		ControlFile->max_wal_senders = xlrec.max_wal_senders;
		ControlFile->max_prepared_xacts = xlrec.max_prepared_xacts;
		ControlFile->max_locks_per_xact = xlrec.max_locks_per_xact;
		ControlFile->wal_level = xlrec.wal_level;
		ControlFile->wal_log_hints = xlrec.wal_log_hints;

		/*
		 * Update minRecoveryPoint to ensure that if recovery is aborted, we
		 * recover back up to this point before allowing hot standby again.
		 * This is important if the max_* settings are decreased, to ensure
		 * you don't run queries against the WAL preceding the change. The
		 * local copies cannot be updated as long as crash recovery is
		 * happening and we expect all the WAL to be replayed.
		 */
		if (InArchiveRecovery)
		{
			minRecoveryPoint = ControlFile->minRecoveryPoint;
			minRecoveryPointTLI = ControlFile->minRecoveryPointTLI;
		}
		if (minRecoveryPoint != InvalidXLogRecPtr && minRecoveryPoint < lsn)
		{
			ControlFile->minRecoveryPoint = lsn;
			ControlFile->minRecoveryPointTLI = ThisTimeLineID;
		}

		CommitTsParameterChange(xlrec.track_commit_timestamp,
								ControlFile->track_commit_timestamp);
		ControlFile->track_commit_timestamp = xlrec.track_commit_timestamp;

		UpdateControlFile();
		LWLockRelease(ControlFileLock);

		/* Check to see if any parameter change gives a problem on recovery */
		CheckRequiredParameterValues();
	}
	else if (info == XLOG_FPW_CHANGE)
	{
		bool		fpw;

		memcpy(&fpw, XLogRecGetData(record), sizeof(bool));

		/*
		 * Update the LSN of the last replayed XLOG_FPW_CHANGE record so that
		 * do_pg_start_backup() and do_pg_stop_backup() can check whether
		 * full_page_writes has been disabled during online backup.
		 */
		if (!fpw)
		{
			SpinLockAcquire(&XLogCtl->info_lck);
			if (XLogCtl->lastFpwDisableRecPtr < ReadRecPtr)
				XLogCtl->lastFpwDisableRecPtr = ReadRecPtr;
			SpinLockRelease(&XLogCtl->info_lck);
		}

		/* Keep track of full_page_writes */
		lastFullPageWrites = fpw;
	}
}

/*
 * Verify the payload of a XLOG_OVERWRITE_CONTRECORD record.
 */
static void
VerifyOverwriteContrecord(xl_overwrite_contrecord *xlrec, XLogReaderState *state)
{
	if (xlrec->overwritten_lsn != state->overwrittenRecPtr)
		elog(FATAL, "mismatching overwritten LSN %X/%X -> %X/%X",
			 (uint32) (xlrec->overwritten_lsn >> 32),
			 (uint32) xlrec->overwritten_lsn,
			 (uint32) (state->overwrittenRecPtr >> 32),
			 (uint32) state->overwrittenRecPtr);

	/* We have safely skipped the aborted record */
	abortedRecPtr = InvalidXLogRecPtr;
	missingContrecPtr = InvalidXLogRecPtr;

	ereport(LOG,
			(errmsg("successfully skipped missing contrecord at %X/%X, overwritten at %s",
					(uint32) (xlrec->overwritten_lsn >> 32),
					(uint32) xlrec->overwritten_lsn,
					timestamptz_to_str(xlrec->overwrite_time))));

	/* Verifying the record should only happen once */
	state->overwrittenRecPtr = InvalidXLogRecPtr;
}

#ifdef WAL_DEBUG

static void
xlog_outrec(StringInfo buf, XLogReaderState *record)
{
	int			block_id;

	appendStringInfo(buf, "prev %X/%X; xid %u",
					 (uint32) (XLogRecGetPrev(record) >> 32),
					 (uint32) XLogRecGetPrev(record),
					 XLogRecGetXid(record));

	appendStringInfo(buf, "; len %u",
					 XLogRecGetDataLen(record));

	/* decode block references */
	for (block_id = 0; block_id <= record->max_block_id; block_id++)
	{
		RelFileNode rnode;
		ForkNumber	forknum;
		BlockNumber blk;

		if (!XLogRecHasBlockRef(record, block_id))
			continue;

		XLogRecGetBlockTag(record, block_id, &rnode, &forknum, &blk);
		if (forknum != MAIN_FORKNUM)
			appendStringInfo(buf, "; blkref #%u: rel %u/%u/%u, fork %u, blk %u",
							 block_id,
							 rnode.spcNode, rnode.dbNode, rnode.relNode,
							 forknum,
							 blk);
		else
			appendStringInfo(buf, "; blkref #%u: rel %u/%u/%u, blk %u",
							 block_id,
							 rnode.spcNode, rnode.dbNode, rnode.relNode,
							 blk);
		if (XLogRecHasBlockImage(record, block_id))
			appendStringInfoString(buf, " FPW");
	}
}
#endif							/* WAL_DEBUG */

/*
 * Returns a string describing an XLogRecord, consisting of its identity
 * optionally followed by a colon, a space, and a further description.
 */
static void
xlog_outdesc(StringInfo buf, XLogReaderState *record)
{
	RmgrId		rmid = XLogRecGetRmid(record);
	uint8		info = XLogRecGetInfo(record);
	const char *id;

	appendStringInfoString(buf, RmgrTable[rmid].rm_name);
	appendStringInfoChar(buf, '/');

	id = RmgrTable[rmid].rm_identify(info);
	if (id == NULL)
		appendStringInfo(buf, "UNKNOWN (%X): ", info & ~XLR_INFO_MASK);
	else
		appendStringInfo(buf, "%s: ", id);

	RmgrTable[rmid].rm_desc(buf, record);
}


/*
 * Return the (possible) sync flag used for opening a file, depending on the
 * value of the GUC wal_sync_method.
 */
static int
get_sync_bit(int method)
{
	int			o_direct_flag = 0;

	/* If fsync is disabled, never open in sync mode */
	if (!enableFsync)
		return 0;

	/*
	 * Optimize writes by bypassing kernel cache with O_DIRECT when using
	 * O_SYNC/O_FSYNC and O_DSYNC.  But only if archiving and streaming are
	 * disabled, otherwise the archive command or walsender process will read
	 * the WAL soon after writing it, which is guaranteed to cause a physical
	 * read if we bypassed the kernel cache. We also skip the
	 * posix_fadvise(POSIX_FADV_DONTNEED) call in XLogFileClose() for the same
	 * reason.
	 *
	 * Never use O_DIRECT in walreceiver process for similar reasons; the WAL
	 * written by walreceiver is normally read by the startup process soon
	 * after its written. Also, walreceiver performs unaligned writes, which
	 * don't work with O_DIRECT, so it is required for correctness too.
	 */
	if (!XLogIsNeeded() && !AmWalReceiverProcess())
		o_direct_flag = PG_O_DIRECT;

	switch (method)
	{
			/*
			 * enum values for all sync options are defined even if they are
			 * not supported on the current platform.  But if not, they are
			 * not included in the enum option array, and therefore will never
			 * be seen here.
			 */
		case SYNC_METHOD_FSYNC:
		case SYNC_METHOD_FSYNC_WRITETHROUGH:
		case SYNC_METHOD_FDATASYNC:
			return 0;
#ifdef OPEN_SYNC_FLAG
		case SYNC_METHOD_OPEN:
			return OPEN_SYNC_FLAG | o_direct_flag;
#endif
#ifdef OPEN_DATASYNC_FLAG
		case SYNC_METHOD_OPEN_DSYNC:
			return OPEN_DATASYNC_FLAG | o_direct_flag;
#endif
		default:
			/* can't happen (unless we are out of sync with option array) */
			elog(ERROR, "unrecognized wal_sync_method: %d", method);
			return 0;			/* silence warning */
	}
}

/*
 * GUC support
 */
void
assign_xlog_sync_method(int new_sync_method, void *extra)
{
	if (sync_method != new_sync_method)
	{
		/*
		 * To ensure that no blocks escape unsynced, force an fsync on the
		 * currently open log segment (if any).  Also, if the open flag is
		 * changing, close the log file so it will be reopened (with new flag
		 * bit) at next use.
		 */
		if (openLogFile >= 0)
		{
			pgstat_report_wait_start(WAIT_EVENT_WAL_SYNC_METHOD_ASSIGN);
			if (pg_fsync(openLogFile) != 0)
			{
				char		xlogfname[MAXFNAMELEN];
				int			save_errno;

				save_errno = errno;
				XLogFileName(xlogfname, ThisTimeLineID, openLogSegNo,
							 wal_segment_size);
				errno = save_errno;
				ereport(PANIC,
						(errcode_for_file_access(),
						 errmsg("could not fsync file \"%s\": %m", xlogfname)));
			}

			pgstat_report_wait_end();
			if (get_sync_bit(sync_method) != get_sync_bit(new_sync_method))
				XLogFileClose();
		}
	}
}


/*
 * Issue appropriate kind of fsync (if any) for an XLOG output file.
 *
 * 'fd' is a file descriptor for the XLOG file to be fsync'd.
 * 'segno' is for error reporting purposes.
 */
void
issue_xlog_fsync(int fd, XLogSegNo segno)
{
	char	   *msg = NULL;

	pgstat_report_wait_start(WAIT_EVENT_WAL_SYNC);
	switch (sync_method)
	{
		case SYNC_METHOD_FSYNC:
			if (pg_fsync_no_writethrough(fd) != 0)
				msg = _("could not fsync file \"%s\": %m");
			break;
#ifdef HAVE_FSYNC_WRITETHROUGH
		case SYNC_METHOD_FSYNC_WRITETHROUGH:
			if (pg_fsync_writethrough(fd) != 0)
				msg = _("could not fsync write-through file \"%s\": %m");
			break;
#endif
#ifdef HAVE_FDATASYNC
		case SYNC_METHOD_FDATASYNC:
			if (pg_fdatasync(fd) != 0)
				msg = _("could not fdatasync file \"%s\": %m");
			break;
#endif
		case SYNC_METHOD_OPEN:
		case SYNC_METHOD_OPEN_DSYNC:
			/* write synced it already */
			break;
		default:
			elog(PANIC, "unrecognized wal_sync_method: %d", sync_method);
			break;
	}

	/* PANIC if failed to fsync */
	if (msg)
	{
		char		xlogfname[MAXFNAMELEN];
		int			save_errno = errno;

		XLogFileName(xlogfname, ThisTimeLineID, segno,
					 wal_segment_size);
		errno = save_errno;
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg(msg, xlogfname)));
	}

	pgstat_report_wait_end();
}

/*
 * do_pg_start_backup
 *
 * Utility function called at the start of an online backup. It creates the
 * necessary starting checkpoint and constructs the backup label file.
 *
 * There are two kind of backups: exclusive and non-exclusive. An exclusive
 * backup is started with pg_start_backup(), and there can be only one active
 * at a time. The backup and tablespace map files of an exclusive backup are
 * written to $PGDATA/backup_label and $PGDATA/tablespace_map, and they are
 * removed by pg_stop_backup().
 *
 * A non-exclusive backup is used for the streaming base backups (see
 * src/backend/replication/basebackup.c). The difference to exclusive backups
 * is that the backup label and tablespace map files are not written to disk.
 * Instead, their would-be contents are returned in *labelfile and *tblspcmapfile,
 * and the caller is responsible for including them in the backup archive as
 * 'backup_label' and 'tablespace_map'. There can be many non-exclusive backups
 * active at the same time, and they don't conflict with an exclusive backup
 * either.
 *
 * tablespaces is required only when this function is called while
 * the streaming base backup requested by pg_basebackup is running.
 * NULL should be specified otherwise.
 *
 * tblspcmapfile is required mainly for tar format in windows as native windows
 * utilities are not able to create symlinks while extracting files from tar.
 * However for consistency, the same is used for all platforms.
 *
 * needtblspcmapfile is true for the cases (exclusive backup and for
 * non-exclusive backup only when tar format is used for taking backup)
 * when backup needs to generate tablespace_map file, it is used to
 * embed escape character before newline character in tablespace path.
 *
 * Returns the minimum WAL location that must be present to restore from this
 * backup, and the corresponding timeline ID in *starttli_p.
 *
 * Every successfully started non-exclusive backup must be stopped by calling
 * do_pg_stop_backup() or do_pg_abort_backup().
 *
 * It is the responsibility of the caller of this function to verify the
 * permissions of the calling user!
 */
XLogRecPtr
do_pg_start_backup(const char *backupidstr, bool fast, TimeLineID *starttli_p,
				   StringInfo labelfile, List **tablespaces,
				   StringInfo tblspcmapfile, bool infotbssize,
				   bool needtblspcmapfile)
{
	bool		exclusive = (labelfile == NULL);
	bool		backup_started_in_recovery = false;
	XLogRecPtr	checkpointloc;
	XLogRecPtr	startpoint;
	TimeLineID	starttli;
	pg_time_t	stamp_time;
	char		strfbuf[128];
	char		xlogfilename[MAXFNAMELEN];
	XLogSegNo	_logSegNo;
	struct stat stat_buf;
	FILE	   *fp;

	backup_started_in_recovery = RecoveryInProgress();

	/*
	 * Currently only non-exclusive backup can be taken during recovery.
	 */
	if (backup_started_in_recovery && exclusive)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("WAL control functions cannot be executed during recovery.")));

	/*
	 * During recovery, we don't need to check WAL level. Because, if WAL
	 * level is not sufficient, it's impossible to get here during recovery.
	 */
	if (!backup_started_in_recovery && !XLogIsNeeded())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("WAL level not sufficient for making an online backup"),
				 errhint("wal_level must be set to \"replica\" or \"logical\" at server start.")));

	if (strlen(backupidstr) > MAXPGPATH)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("backup label too long (max %d bytes)",
						MAXPGPATH)));

	/*
	 * Mark backup active in shared memory.  We must do full-page WAL writes
	 * during an on-line backup even if not doing so at other times, because
	 * it's quite possible for the backup dump to obtain a "torn" (partially
	 * written) copy of a database page if it reads the page concurrently with
	 * our write to the same page.  This can be fixed as long as the first
	 * write to the page in the WAL sequence is a full-page write. Hence, we
	 * turn on forcePageWrites and then force a CHECKPOINT, to ensure there
	 * are no dirty pages in shared memory that might get dumped while the
	 * backup is in progress without having a corresponding WAL record.  (Once
	 * the backup is complete, we need not force full-page writes anymore,
	 * since we expect that any pages not modified during the backup interval
	 * must have been correctly captured by the backup.)
	 *
	 * Note that forcePageWrites has no effect during an online backup from
	 * the standby.
	 *
	 * We must hold all the insertion locks to change the value of
	 * forcePageWrites, to ensure adequate interlocking against
	 * XLogInsertRecord().
	 */
	WALInsertLockAcquireExclusive();
	if (exclusive)
	{
		/*
		 * At first, mark that we're now starting an exclusive backup, to
		 * ensure that there are no other sessions currently running
		 * pg_start_backup() or pg_stop_backup().
		 */
		if (XLogCtl->Insert.exclusiveBackupState != EXCLUSIVE_BACKUP_NONE)
		{
			WALInsertLockRelease();
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("a backup is already in progress"),
					 errhint("Run pg_stop_backup() and try again.")));
		}
		XLogCtl->Insert.exclusiveBackupState = EXCLUSIVE_BACKUP_STARTING;
	}
	else
		XLogCtl->Insert.nonExclusiveBackups++;
	XLogCtl->Insert.forcePageWrites = true;
	WALInsertLockRelease();

	/* Ensure we release forcePageWrites if fail below */
	PG_ENSURE_ERROR_CLEANUP(pg_start_backup_callback, (Datum) BoolGetDatum(exclusive));
	{
		bool		gotUniqueStartpoint = false;
		DIR		   *tblspcdir;
		struct dirent *de;
		tablespaceinfo *ti;
		int			datadirpathlen;

		/*
		 * Force an XLOG file switch before the checkpoint, to ensure that the
		 * WAL segment the checkpoint is written to doesn't contain pages with
		 * old timeline IDs.  That would otherwise happen if you called
		 * pg_start_backup() right after restoring from a PITR archive: the
		 * first WAL segment containing the startup checkpoint has pages in
		 * the beginning with the old timeline ID.  That can cause trouble at
		 * recovery: we won't have a history file covering the old timeline if
		 * pg_wal directory was not included in the base backup and the WAL
		 * archive was cleared too before starting the backup.
		 *
		 * This also ensures that we have emitted a WAL page header that has
		 * XLP_BKP_REMOVABLE off before we emit the checkpoint record.
		 * Therefore, if a WAL archiver (such as pglesslog) is trying to
		 * compress out removable backup blocks, it won't remove any that
		 * occur after this point.
		 *
		 * During recovery, we skip forcing XLOG file switch, which means that
		 * the backup taken during recovery is not available for the special
		 * recovery case described above.
		 */
		if (!backup_started_in_recovery)
			RequestXLogSwitch(false);

		do
		{
			bool		checkpointfpw;

			/*
			 * Force a CHECKPOINT.  Aside from being necessary to prevent torn
			 * page problems, this guarantees that two successive backup runs
			 * will have different checkpoint positions and hence different
			 * history file names, even if nothing happened in between.
			 *
			 * During recovery, establish a restartpoint if possible. We use
			 * the last restartpoint as the backup starting checkpoint. This
			 * means that two successive backup runs can have same checkpoint
			 * positions.
			 *
			 * Since the fact that we are executing do_pg_start_backup()
			 * during recovery means that checkpointer is running, we can use
			 * RequestCheckpoint() to establish a restartpoint.
			 *
			 * We use CHECKPOINT_IMMEDIATE only if requested by user (via
			 * passing fast = true).  Otherwise this can take awhile.
			 */
			RequestCheckpoint(CHECKPOINT_FORCE | CHECKPOINT_WAIT |
							  (fast ? CHECKPOINT_IMMEDIATE : 0));

			/*
			 * Now we need to fetch the checkpoint record location, and also
			 * its REDO pointer.  The oldest point in WAL that would be needed
			 * to restore starting from the checkpoint is precisely the REDO
			 * pointer.
			 */
			LWLockAcquire(ControlFileLock, LW_SHARED);
			checkpointloc = ControlFile->checkPoint;
			startpoint = ControlFile->checkPointCopy.redo;
			starttli = ControlFile->checkPointCopy.ThisTimeLineID;
			checkpointfpw = ControlFile->checkPointCopy.fullPageWrites;
			LWLockRelease(ControlFileLock);

			if (backup_started_in_recovery)
			{
				XLogRecPtr	recptr;

				/*
				 * Check to see if all WAL replayed during online backup
				 * (i.e., since last restartpoint used as backup starting
				 * checkpoint) contain full-page writes.
				 */
				SpinLockAcquire(&XLogCtl->info_lck);
				recptr = XLogCtl->lastFpwDisableRecPtr;
				SpinLockRelease(&XLogCtl->info_lck);

				if (!checkpointfpw || startpoint <= recptr)
					ereport(ERROR,
							(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							 errmsg("WAL generated with full_page_writes=off was replayed "
									"since last restartpoint"),
							 errhint("This means that the backup being taken on the standby "
									 "is corrupt and should not be used. "
									 "Enable full_page_writes and run CHECKPOINT on the master, "
									 "and then try an online backup again.")));

				/*
				 * During recovery, since we don't use the end-of-backup WAL
				 * record and don't write the backup history file, the
				 * starting WAL location doesn't need to be unique. This means
				 * that two base backups started at the same time might use
				 * the same checkpoint as starting locations.
				 */
				gotUniqueStartpoint = true;
			}

			/*
			 * If two base backups are started at the same time (in WAL sender
			 * processes), we need to make sure that they use different
			 * checkpoints as starting locations, because we use the starting
			 * WAL location as a unique identifier for the base backup in the
			 * end-of-backup WAL record and when we write the backup history
			 * file. Perhaps it would be better generate a separate unique ID
			 * for each backup instead of forcing another checkpoint, but
			 * taking a checkpoint right after another is not that expensive
			 * either because only few buffers have been dirtied yet.
			 */
			WALInsertLockAcquireExclusive();
			if (XLogCtl->Insert.lastBackupStart < startpoint)
			{
				XLogCtl->Insert.lastBackupStart = startpoint;
				gotUniqueStartpoint = true;
			}
			WALInsertLockRelease();
		} while (!gotUniqueStartpoint);

		XLByteToSeg(startpoint, _logSegNo, wal_segment_size);
		XLogFileName(xlogfilename, starttli, _logSegNo, wal_segment_size);

		/*
		 * Construct tablespace_map file
		 */
		if (exclusive)
			tblspcmapfile = makeStringInfo();

		datadirpathlen = strlen(DataDir);

		/*
		 * Report that we are now estimating the total backup size if we're
		 * streaming base backup as requested by pg_basebackup
		 */
		if (tablespaces)
			pgstat_progress_update_param(PROGRESS_BASEBACKUP_PHASE,
										 PROGRESS_BASEBACKUP_PHASE_ESTIMATE_BACKUP_SIZE);

		/* Collect information about all tablespaces */
		tblspcdir = AllocateDir("pg_tblspc");
		while ((de = ReadDir(tblspcdir, "pg_tblspc")) != NULL)
		{
			char		fullpath[MAXPGPATH + 10];
			char		linkpath[MAXPGPATH];
			char	   *relpath = NULL;
			int			rllen;
			StringInfoData buflinkpath;
			char	   *s = linkpath;
#ifndef WIN32
			struct stat st;
#endif

			/* Skip special stuff */
			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
				continue;

			snprintf(fullpath, sizeof(fullpath), "pg_tblspc/%s", de->d_name);

			/*
			 * Skip anything that isn't a symlink/junction.  For testing only,
			 * we sometimes use allow_in_place_tablespaces to create
			 * directories directly under pg_tblspc, which would fail below.
			 */
#ifndef WIN32
			if (lstat(fullpath, &st) < 0)
				ereport(LOG,
						(errcode_for_file_access(),
						 errmsg("could not stat file \"%s\": %m",
								fullpath)));
			else if (!S_ISLNK(st.st_mode))
#else			/* WIN32 */
			if (!pgwin32_is_junction(fullpath))
#endif
				continue;

#if defined(HAVE_READLINK) || defined(WIN32)
			rllen = readlink(fullpath, linkpath, sizeof(linkpath));
			if (rllen < 0)
			{
				ereport(WARNING,
						(errmsg("could not read symbolic link \"%s\": %m",
								fullpath)));
				continue;
			}
			else if (rllen >= sizeof(linkpath))
			{
				ereport(WARNING,
						(errmsg("symbolic link \"%s\" target is too long",
								fullpath)));
				continue;
			}
			linkpath[rllen] = '\0';

			/*
			 * Add the escape character '\\' before newline in a string to
			 * ensure that we can distinguish between the newline in the
			 * tablespace path and end of line while reading tablespace_map
			 * file during archive recovery.
			 */
			initStringInfo(&buflinkpath);

			while (*s)
			{
				if ((*s == '\n' || *s == '\r') && needtblspcmapfile)
					appendStringInfoChar(&buflinkpath, '\\');
				appendStringInfoChar(&buflinkpath, *s++);
			}

			/*
			 * Relpath holds the relative path of the tablespace directory
			 * when it's located within PGDATA, or NULL if it's located
			 * elsewhere.
			 */
			if (rllen > datadirpathlen &&
				strncmp(linkpath, DataDir, datadirpathlen) == 0 &&
				IS_DIR_SEP(linkpath[datadirpathlen]))
				relpath = linkpath + datadirpathlen + 1;

			ti = palloc(sizeof(tablespaceinfo));
			ti->oid = pstrdup(de->d_name);
			ti->path = pstrdup(buflinkpath.data);
			ti->rpath = relpath ? pstrdup(relpath) : NULL;
			ti->size = infotbssize ?
				sendTablespace(fullpath, ti->oid, true, NULL) : -1;

			if (tablespaces)
				*tablespaces = lappend(*tablespaces, ti);

			appendStringInfo(tblspcmapfile, "%s %s\n", ti->oid, ti->path);

			pfree(buflinkpath.data);
#else

			/*
			 * If the platform does not have symbolic links, it should not be
			 * possible to have tablespaces - clearly somebody else created
			 * them. Warn about it and ignore.
			 */
			ereport(WARNING,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("tablespaces are not supported on this platform")));
#endif
		}
		FreeDir(tblspcdir);

		/*
		 * Construct backup label file
		 */
		if (exclusive)
			labelfile = makeStringInfo();

		/* Use the log timezone here, not the session timezone */
		stamp_time = (pg_time_t) time(NULL);
		pg_strftime(strfbuf, sizeof(strfbuf),
					"%Y-%m-%d %H:%M:%S %Z",
					pg_localtime(&stamp_time, log_timezone));
		appendStringInfo(labelfile, "START WAL LOCATION: %X/%X (file %s)\n",
						 (uint32) (startpoint >> 32), (uint32) startpoint, xlogfilename);
		appendStringInfo(labelfile, "CHECKPOINT LOCATION: %X/%X\n",
						 (uint32) (checkpointloc >> 32), (uint32) checkpointloc);
		appendStringInfo(labelfile, "BACKUP METHOD: %s\n",
						 exclusive ? "pg_start_backup" : "streamed");
		appendStringInfo(labelfile, "BACKUP FROM: %s\n",
						 backup_started_in_recovery ? "standby" : "master");
		appendStringInfo(labelfile, "START TIME: %s\n", strfbuf);
		appendStringInfo(labelfile, "LABEL: %s\n", backupidstr);
		appendStringInfo(labelfile, "START TIMELINE: %u\n", starttli);

		/*
		 * Okay, write the file, or return its contents to caller.
		 */
		if (exclusive)
		{
			/*
			 * Check for existing backup label --- implies a backup is already
			 * running.  (XXX given that we checked exclusiveBackupState
			 * above, maybe it would be OK to just unlink any such label
			 * file?)
			 */
			if (stat(BACKUP_LABEL_FILE, &stat_buf) != 0)
			{
				if (errno != ENOENT)
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not stat file \"%s\": %m",
									BACKUP_LABEL_FILE)));
			}
			else
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("a backup is already in progress"),
						 errhint("If you're sure there is no backup in progress, remove file \"%s\" and try again.",
								 BACKUP_LABEL_FILE)));

			fp = AllocateFile(BACKUP_LABEL_FILE, "w");

			if (!fp)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not create file \"%s\": %m",
								BACKUP_LABEL_FILE)));
			if (fwrite(labelfile->data, labelfile->len, 1, fp) != 1 ||
				fflush(fp) != 0 ||
				pg_fsync(fileno(fp)) != 0 ||
				ferror(fp) ||
				FreeFile(fp))
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not write file \"%s\": %m",
								BACKUP_LABEL_FILE)));
			/* Allocated locally for exclusive backups, so free separately */
			pfree(labelfile->data);
			pfree(labelfile);

			/* Write backup tablespace_map file. */
			if (tblspcmapfile->len > 0)
			{
				if (stat(TABLESPACE_MAP, &stat_buf) != 0)
				{
					if (errno != ENOENT)
						ereport(ERROR,
								(errcode_for_file_access(),
								 errmsg("could not stat file \"%s\": %m",
										TABLESPACE_MAP)));
				}
				else
					ereport(ERROR,
							(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							 errmsg("a backup is already in progress"),
							 errhint("If you're sure there is no backup in progress, remove file \"%s\" and try again.",
									 TABLESPACE_MAP)));

				fp = AllocateFile(TABLESPACE_MAP, "w");

				if (!fp)
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not create file \"%s\": %m",
									TABLESPACE_MAP)));
				if (fwrite(tblspcmapfile->data, tblspcmapfile->len, 1, fp) != 1 ||
					fflush(fp) != 0 ||
					pg_fsync(fileno(fp)) != 0 ||
					ferror(fp) ||
					FreeFile(fp))
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not write file \"%s\": %m",
									TABLESPACE_MAP)));
			}

			/* Allocated locally for exclusive backups, so free separately */
			pfree(tblspcmapfile->data);
			pfree(tblspcmapfile);
		}
	}
	PG_END_ENSURE_ERROR_CLEANUP(pg_start_backup_callback, (Datum) BoolGetDatum(exclusive));

	/*
	 * Mark that start phase has correctly finished for an exclusive backup.
	 * Session-level locks are updated as well to reflect that state.
	 *
	 * Note that CHECK_FOR_INTERRUPTS() must not occur while updating backup
	 * counters and session-level lock. Otherwise they can be updated
	 * inconsistently, and which might cause do_pg_abort_backup() to fail.
	 */
	if (exclusive)
	{
		WALInsertLockAcquireExclusive();
		XLogCtl->Insert.exclusiveBackupState = EXCLUSIVE_BACKUP_IN_PROGRESS;

		/* Set session-level lock */
		sessionBackupState = SESSION_BACKUP_EXCLUSIVE;
		WALInsertLockRelease();
	}
	else
		sessionBackupState = SESSION_BACKUP_NON_EXCLUSIVE;

	/*
	 * We're done.  As a convenience, return the starting WAL location.
	 */
	if (starttli_p)
		*starttli_p = starttli;
	return startpoint;
}

/* Error cleanup callback for pg_start_backup */
static void
pg_start_backup_callback(int code, Datum arg)
{
	bool		exclusive = DatumGetBool(arg);

	/* Update backup counters and forcePageWrites on failure */
	WALInsertLockAcquireExclusive();
	if (exclusive)
	{
		Assert(XLogCtl->Insert.exclusiveBackupState == EXCLUSIVE_BACKUP_STARTING);
		XLogCtl->Insert.exclusiveBackupState = EXCLUSIVE_BACKUP_NONE;
	}
	else
	{
		Assert(XLogCtl->Insert.nonExclusiveBackups > 0);
		XLogCtl->Insert.nonExclusiveBackups--;
	}

	if (XLogCtl->Insert.exclusiveBackupState == EXCLUSIVE_BACKUP_NONE &&
		XLogCtl->Insert.nonExclusiveBackups == 0)
	{
		XLogCtl->Insert.forcePageWrites = false;
	}
	WALInsertLockRelease();
}

/*
 * Error cleanup callback for pg_stop_backup
 */
static void
pg_stop_backup_callback(int code, Datum arg)
{
	bool		exclusive = DatumGetBool(arg);

	/* Update backup status on failure */
	WALInsertLockAcquireExclusive();
	if (exclusive)
	{
		Assert(XLogCtl->Insert.exclusiveBackupState == EXCLUSIVE_BACKUP_STOPPING);
		XLogCtl->Insert.exclusiveBackupState = EXCLUSIVE_BACKUP_IN_PROGRESS;
	}
	WALInsertLockRelease();
}

/*
 * Utility routine to fetch the session-level status of a backup running.
 */
SessionBackupState
get_backup_status(void)
{
	return sessionBackupState;
}

/*
 * do_pg_stop_backup
 *
 * Utility function called at the end of an online backup. It cleans up the
 * backup state and can optionally wait for WAL segments to be archived.
 *
 * If labelfile is NULL, this stops an exclusive backup. Otherwise this stops
 * the non-exclusive backup specified by 'labelfile'.
 *
 * Returns the last WAL location that must be present to restore from this
 * backup, and the corresponding timeline ID in *stoptli_p.
 *
 * It is the responsibility of the caller of this function to verify the
 * permissions of the calling user!
 */
XLogRecPtr
do_pg_stop_backup(char *labelfile, bool waitforarchive, TimeLineID *stoptli_p)
{
	bool		exclusive = (labelfile == NULL);
	bool		backup_started_in_recovery = false;
	XLogRecPtr	startpoint;
	XLogRecPtr	stoppoint;
	TimeLineID	stoptli;
	pg_time_t	stamp_time;
	char		strfbuf[128];
	char		histfilepath[MAXPGPATH];
	char		startxlogfilename[MAXFNAMELEN];
	char		stopxlogfilename[MAXFNAMELEN];
	char		lastxlogfilename[MAXFNAMELEN];
	char		histfilename[MAXFNAMELEN];
	char		backupfrom[20];
	XLogSegNo	_logSegNo;
	FILE	   *lfp;
	FILE	   *fp;
	char		ch;
	int			seconds_before_warning;
	int			waits = 0;
	bool		reported_waiting = false;
	char	   *remaining;
	char	   *ptr;
	uint32		hi,
				lo;

	backup_started_in_recovery = RecoveryInProgress();

	/*
	 * Currently only non-exclusive backup can be taken during recovery.
	 */
	if (backup_started_in_recovery && exclusive)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("WAL control functions cannot be executed during recovery.")));

	/*
	 * During recovery, we don't need to check WAL level. Because, if WAL
	 * level is not sufficient, it's impossible to get here during recovery.
	 */
	if (!backup_started_in_recovery && !XLogIsNeeded())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("WAL level not sufficient for making an online backup"),
				 errhint("wal_level must be set to \"replica\" or \"logical\" at server start.")));

	if (exclusive)
	{
		/*
		 * At first, mark that we're now stopping an exclusive backup, to
		 * ensure that there are no other sessions currently running
		 * pg_start_backup() or pg_stop_backup().
		 */
		WALInsertLockAcquireExclusive();
		if (XLogCtl->Insert.exclusiveBackupState != EXCLUSIVE_BACKUP_IN_PROGRESS)
		{
			WALInsertLockRelease();
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("exclusive backup not in progress")));
		}
		XLogCtl->Insert.exclusiveBackupState = EXCLUSIVE_BACKUP_STOPPING;
		WALInsertLockRelease();

		/*
		 * Remove backup_label. In case of failure, the state for an exclusive
		 * backup is switched back to in-progress.
		 */
		PG_ENSURE_ERROR_CLEANUP(pg_stop_backup_callback, (Datum) BoolGetDatum(exclusive));
		{
			/*
			 * Read the existing label file into memory.
			 */
			struct stat statbuf;
			int			r;

			if (stat(BACKUP_LABEL_FILE, &statbuf))
			{
				/* should not happen per the upper checks */
				if (errno != ENOENT)
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not stat file \"%s\": %m",
									BACKUP_LABEL_FILE)));
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("a backup is not in progress")));
			}

			lfp = AllocateFile(BACKUP_LABEL_FILE, "r");
			if (!lfp)
			{
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read file \"%s\": %m",
								BACKUP_LABEL_FILE)));
			}
			labelfile = palloc(statbuf.st_size + 1);
			r = fread(labelfile, statbuf.st_size, 1, lfp);
			labelfile[statbuf.st_size] = '\0';

			/*
			 * Close and remove the backup label file
			 */
			if (r != 1 || ferror(lfp) || FreeFile(lfp))
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read file \"%s\": %m",
								BACKUP_LABEL_FILE)));
			durable_unlink(BACKUP_LABEL_FILE, ERROR);

			/*
			 * Remove tablespace_map file if present, it is created only if
			 * there are tablespaces.
			 */
			durable_unlink(TABLESPACE_MAP, DEBUG1);
		}
		PG_END_ENSURE_ERROR_CLEANUP(pg_stop_backup_callback, (Datum) BoolGetDatum(exclusive));
	}

	/*
	 * OK to update backup counters, forcePageWrites and session-level lock.
	 *
	 * Note that CHECK_FOR_INTERRUPTS() must not occur while updating them.
	 * Otherwise they can be updated inconsistently, and which might cause
	 * do_pg_abort_backup() to fail.
	 */
	WALInsertLockAcquireExclusive();
	if (exclusive)
	{
		XLogCtl->Insert.exclusiveBackupState = EXCLUSIVE_BACKUP_NONE;
	}
	else
	{
		/*
		 * The user-visible pg_start/stop_backup() functions that operate on
		 * exclusive backups can be called at any time, but for non-exclusive
		 * backups, it is expected that each do_pg_start_backup() call is
		 * matched by exactly one do_pg_stop_backup() call.
		 */
		Assert(XLogCtl->Insert.nonExclusiveBackups > 0);
		XLogCtl->Insert.nonExclusiveBackups--;
	}

	if (XLogCtl->Insert.exclusiveBackupState == EXCLUSIVE_BACKUP_NONE &&
		XLogCtl->Insert.nonExclusiveBackups == 0)
	{
		XLogCtl->Insert.forcePageWrites = false;
	}

	/*
	 * Clean up session-level lock.
	 *
	 * You might think that WALInsertLockRelease() can be called before
	 * cleaning up session-level lock because session-level lock doesn't need
	 * to be protected with WAL insertion lock. But since
	 * CHECK_FOR_INTERRUPTS() can occur in it, session-level lock must be
	 * cleaned up before it.
	 */
	sessionBackupState = SESSION_BACKUP_NONE;

	WALInsertLockRelease();

	/*
	 * Read and parse the START WAL LOCATION line (this code is pretty crude,
	 * but we are not expecting any variability in the file format).
	 */
	if (sscanf(labelfile, "START WAL LOCATION: %X/%X (file %24s)%c",
			   &hi, &lo, startxlogfilename,
			   &ch) != 4 || ch != '\n')
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("invalid data in file \"%s\"", BACKUP_LABEL_FILE)));
	startpoint = ((uint64) hi) << 32 | lo;
	remaining = strchr(labelfile, '\n') + 1;	/* %n is not portable enough */

	/*
	 * Parse the BACKUP FROM line. If we are taking an online backup from the
	 * standby, we confirm that the standby has not been promoted during the
	 * backup.
	 */
	ptr = strstr(remaining, "BACKUP FROM:");
	if (!ptr || sscanf(ptr, "BACKUP FROM: %19s\n", backupfrom) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("invalid data in file \"%s\"", BACKUP_LABEL_FILE)));
	if (strcmp(backupfrom, "standby") == 0 && !backup_started_in_recovery)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the standby was promoted during online backup"),
				 errhint("This means that the backup being taken is corrupt "
						 "and should not be used. "
						 "Try taking another online backup.")));

	/*
	 * During recovery, we don't write an end-of-backup record. We assume that
	 * pg_control was backed up last and its minimum recovery point can be
	 * available as the backup end location. Since we don't have an
	 * end-of-backup record, we use the pg_control value to check whether
	 * we've reached the end of backup when starting recovery from this
	 * backup. We have no way of checking if pg_control wasn't backed up last
	 * however.
	 *
	 * We don't force a switch to new WAL file but it is still possible to
	 * wait for all the required files to be archived if waitforarchive is
	 * true. This is okay if we use the backup to start a standby and fetch
	 * the missing WAL using streaming replication. But in the case of an
	 * archive recovery, a user should set waitforarchive to true and wait for
	 * them to be archived to ensure that all the required files are
	 * available.
	 *
	 * We return the current minimum recovery point as the backup end
	 * location. Note that it can be greater than the exact backup end
	 * location if the minimum recovery point is updated after the backup of
	 * pg_control. This is harmless for current uses.
	 *
	 * XXX currently a backup history file is for informational and debug
	 * purposes only. It's not essential for an online backup. Furthermore,
	 * even if it's created, it will not be archived during recovery because
	 * an archiver is not invoked. So it doesn't seem worthwhile to write a
	 * backup history file during recovery.
	 */
	if (backup_started_in_recovery)
	{
		XLogRecPtr	recptr;

		/*
		 * Check to see if all WAL replayed during online backup contain
		 * full-page writes.
		 */
		SpinLockAcquire(&XLogCtl->info_lck);
		recptr = XLogCtl->lastFpwDisableRecPtr;
		SpinLockRelease(&XLogCtl->info_lck);

		if (startpoint <= recptr)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("WAL generated with full_page_writes=off was replayed "
							"during online backup"),
					 errhint("This means that the backup being taken on the standby "
							 "is corrupt and should not be used. "
							 "Enable full_page_writes and run CHECKPOINT on the master, "
							 "and then try an online backup again.")));


		LWLockAcquire(ControlFileLock, LW_SHARED);
		stoppoint = ControlFile->minRecoveryPoint;
		stoptli = ControlFile->minRecoveryPointTLI;
		LWLockRelease(ControlFileLock);
	}
	else
	{
		/*
		 * Write the backup-end xlog record
		 */
		XLogBeginInsert();
		XLogRegisterData((char *) (&startpoint), sizeof(startpoint));
		stoppoint = XLogInsert(RM_XLOG_ID, XLOG_BACKUP_END);
		stoptli = ThisTimeLineID;

		/*
		 * Force a switch to a new xlog segment file, so that the backup is
		 * valid as soon as archiver moves out the current segment file.
		 */
		RequestXLogSwitch(false);

		XLByteToPrevSeg(stoppoint, _logSegNo, wal_segment_size);
		XLogFileName(stopxlogfilename, stoptli, _logSegNo, wal_segment_size);

		/* Use the log timezone here, not the session timezone */
		stamp_time = (pg_time_t) time(NULL);
		pg_strftime(strfbuf, sizeof(strfbuf),
					"%Y-%m-%d %H:%M:%S %Z",
					pg_localtime(&stamp_time, log_timezone));

		/*
		 * Write the backup history file
		 */
		XLByteToSeg(startpoint, _logSegNo, wal_segment_size);
		BackupHistoryFilePath(histfilepath, stoptli, _logSegNo,
							  startpoint, wal_segment_size);
		fp = AllocateFile(histfilepath, "w");
		if (!fp)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create file \"%s\": %m",
							histfilepath)));
		fprintf(fp, "START WAL LOCATION: %X/%X (file %s)\n",
				(uint32) (startpoint >> 32), (uint32) startpoint, startxlogfilename);
		fprintf(fp, "STOP WAL LOCATION: %X/%X (file %s)\n",
				(uint32) (stoppoint >> 32), (uint32) stoppoint, stopxlogfilename);

		/*
		 * Transfer remaining lines including label and start timeline to
		 * history file.
		 */
		fprintf(fp, "%s", remaining);
		fprintf(fp, "STOP TIME: %s\n", strfbuf);
		fprintf(fp, "STOP TIMELINE: %u\n", stoptli);
		if (fflush(fp) || ferror(fp) || FreeFile(fp))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m",
							histfilepath)));

		/*
		 * Clean out any no-longer-needed history files.  As a side effect,
		 * this will post a .ready file for the newly created history file,
		 * notifying the archiver that history file may be archived
		 * immediately.
		 */
		CleanupBackupHistory();
	}

	/*
	 * If archiving is enabled, wait for all the required WAL files to be
	 * archived before returning. If archiving isn't enabled, the required WAL
	 * needs to be transported via streaming replication (hopefully with
	 * wal_keep_size set high enough), or some more exotic mechanism like
	 * polling and copying files from pg_wal with script. We have no knowledge
	 * of those mechanisms, so it's up to the user to ensure that he gets all
	 * the required WAL.
	 *
	 * We wait until both the last WAL file filled during backup and the
	 * history file have been archived, and assume that the alphabetic sorting
	 * property of the WAL files ensures any earlier WAL files are safely
	 * archived as well.
	 *
	 * We wait forever, since archive_command is supposed to work and we
	 * assume the admin wanted his backup to work completely. If you don't
	 * wish to wait, then either waitforarchive should be passed in as false,
	 * or you can set statement_timeout.  Also, some notices are issued to
	 * clue in anyone who might be doing this interactively.
	 */

	if (waitforarchive &&
		((!backup_started_in_recovery && XLogArchivingActive()) ||
		 (backup_started_in_recovery && XLogArchivingAlways())))
	{
		XLByteToPrevSeg(stoppoint, _logSegNo, wal_segment_size);
		XLogFileName(lastxlogfilename, stoptli, _logSegNo, wal_segment_size);

		XLByteToSeg(startpoint, _logSegNo, wal_segment_size);
		BackupHistoryFileName(histfilename, stoptli, _logSegNo,
							  startpoint, wal_segment_size);

		seconds_before_warning = 60;
		waits = 0;

		while (XLogArchiveIsBusy(lastxlogfilename) ||
			   XLogArchiveIsBusy(histfilename))
		{
			CHECK_FOR_INTERRUPTS();

			if (!reported_waiting && waits > 5)
			{
				ereport(NOTICE,
						(errmsg("base backup done, waiting for required WAL segments to be archived")));
				reported_waiting = true;
			}

			pgstat_report_wait_start(WAIT_EVENT_BACKUP_WAIT_WAL_ARCHIVE);
			pg_usleep(1000000L);
			pgstat_report_wait_end();

			if (++waits >= seconds_before_warning)
			{
				seconds_before_warning *= 2;	/* This wraps in >10 years... */
				ereport(WARNING,
						(errmsg("still waiting for all required WAL segments to be archived (%d seconds elapsed)",
								waits),
						 errhint("Check that your archive_command is executing properly.  "
								 "You can safely cancel this backup, "
								 "but the database backup will not be usable without all the WAL segments.")));
			}
		}

		ereport(NOTICE,
				(errmsg("all required WAL segments have been archived")));
	}
	else if (waitforarchive)
		ereport(NOTICE,
				(errmsg("WAL archiving is not enabled; you must ensure that all required WAL segments are copied through other means to complete the backup")));

	/*
	 * We're done.  As a convenience, return the ending WAL location.
	 */
	if (stoptli_p)
		*stoptli_p = stoptli;
	return stoppoint;
}


/*
 * do_pg_abort_backup: abort a running backup
 *
 * This does just the most basic steps of do_pg_stop_backup(), by taking the
 * system out of backup mode, thus making it a lot more safe to call from
 * an error handler.
 *
 * The caller can pass 'arg' as 'true' or 'false' to control whether a warning
 * is emitted.
 *
 * NB: This is only for aborting a non-exclusive backup that doesn't write
 * backup_label. A backup started with pg_start_backup() needs to be finished
 * with pg_stop_backup().
 *
 * NB: This gets used as a before_shmem_exit handler, hence the odd-looking
 * signature.
 */
void
do_pg_abort_backup(int code, Datum arg)
{
	bool		emit_warning = DatumGetBool(arg);

	/*
	 * Quick exit if session is not keeping around a non-exclusive backup
	 * already started.
	 */
	if (sessionBackupState != SESSION_BACKUP_NON_EXCLUSIVE)
		return;

	WALInsertLockAcquireExclusive();
	Assert(XLogCtl->Insert.nonExclusiveBackups > 0);
	XLogCtl->Insert.nonExclusiveBackups--;

	if (XLogCtl->Insert.exclusiveBackupState == EXCLUSIVE_BACKUP_NONE &&
		XLogCtl->Insert.nonExclusiveBackups == 0)
	{
		XLogCtl->Insert.forcePageWrites = false;
	}

	sessionBackupState = SESSION_BACKUP_NONE;
	WALInsertLockRelease();

	if (emit_warning)
		ereport(WARNING,
				(errmsg("aborting backup due to backend exiting before pg_stop_backup was called")));
}

/*
 * Register a handler that will warn about unterminated backups at end of
 * session, unless this has already been done.
 */
void
register_persistent_abort_backup_handler(void)
{
	static bool already_done = false;

	if (already_done)
		return;
	before_shmem_exit(do_pg_abort_backup, DatumGetBool(true));
	already_done = true;
}

/*
 * Get latest redo apply position.
 *
 * Exported to allow WALReceiver to read the pointer directly.
 */
XLogRecPtr
GetXLogReplayRecPtr(TimeLineID *replayTLI)
{
	XLogRecPtr	recptr;
	TimeLineID	tli;

	SpinLockAcquire(&XLogCtl->info_lck);
	recptr = XLogCtl->lastReplayedEndRecPtr;
	tli = XLogCtl->lastReplayedTLI;
	SpinLockRelease(&XLogCtl->info_lck);

	if (replayTLI)
		*replayTLI = tli;
	return recptr;
}

/*
 * Get latest WAL insert pointer
 */
XLogRecPtr
GetXLogInsertRecPtr(void)
{
	XLogCtlInsert *Insert = &XLogCtl->Insert;
	uint64		current_bytepos;

	SpinLockAcquire(&Insert->insertpos_lck);
	current_bytepos = Insert->CurrBytePos;
	SpinLockRelease(&Insert->insertpos_lck);

	return XLogBytePosToRecPtr(current_bytepos);
}

/*
 * Get latest WAL write pointer
 */
XLogRecPtr
GetXLogWriteRecPtr(void)
{
	SpinLockAcquire(&XLogCtl->info_lck);
	LogwrtResult = XLogCtl->LogwrtResult;
	SpinLockRelease(&XLogCtl->info_lck);

	return LogwrtResult.Write;
}

/*
 * Returns the redo pointer of the last checkpoint or restartpoint. This is
 * the oldest point in WAL that we still need, if we have to restart recovery.
 */
void
GetOldestRestartPoint(XLogRecPtr *oldrecptr, TimeLineID *oldtli)
{
	LWLockAcquire(ControlFileLock, LW_SHARED);
	*oldrecptr = ControlFile->checkPointCopy.redo;
	*oldtli = ControlFile->checkPointCopy.ThisTimeLineID;
	LWLockRelease(ControlFileLock);
}

/*
 * read_backup_label: check to see if a backup_label file is present
 *
 * If we see a backup_label during recovery, we assume that we are recovering
 * from a backup dump file, and we therefore roll forward from the checkpoint
 * identified by the label file, NOT what pg_control says.  This avoids the
 * problem that pg_control might have been archived one or more checkpoints
 * later than the start of the dump, and so if we rely on it as the start
 * point, we will fail to restore a consistent database state.
 *
 * Returns true if a backup_label was found (and fills the checkpoint
 * location and its REDO location into *checkPointLoc and RedoStartLSN,
 * respectively); returns false if not. If this backup_label came from a
 * streamed backup, *backupEndRequired is set to true. If this backup_label
 * was created during recovery, *backupFromStandby is set to true.
 */
static bool
read_backup_label(XLogRecPtr *checkPointLoc, bool *backupEndRequired,
				  bool *backupFromStandby)
{
	char		startxlogfilename[MAXFNAMELEN];
	TimeLineID	tli_from_walseg,
				tli_from_file;
	FILE	   *lfp;
	char		ch;
	char		backuptype[20];
	char		backupfrom[20];
	char		backuplabel[MAXPGPATH];
	char		backuptime[128];
	uint32		hi,
				lo;

	*backupEndRequired = false;
	*backupFromStandby = false;

	/*
	 * See if label file is present
	 */
	lfp = AllocateFile(BACKUP_LABEL_FILE, "r");
	if (!lfp)
	{
		if (errno != ENOENT)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m",
							BACKUP_LABEL_FILE)));
		return false;			/* it's not there, all is fine */
	}

	/*
	 * Read and parse the START WAL LOCATION and CHECKPOINT lines (this code
	 * is pretty crude, but we are not expecting any variability in the file
	 * format).
	 */
	if (fscanf(lfp, "START WAL LOCATION: %X/%X (file %08X%16s)%c",
			   &hi, &lo, &tli_from_walseg, startxlogfilename, &ch) != 5 || ch != '\n')
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("invalid data in file \"%s\"", BACKUP_LABEL_FILE)));
	RedoStartLSN = ((uint64) hi) << 32 | lo;
	if (fscanf(lfp, "CHECKPOINT LOCATION: %X/%X%c",
			   &hi, &lo, &ch) != 3 || ch != '\n')
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("invalid data in file \"%s\"", BACKUP_LABEL_FILE)));
	*checkPointLoc = ((uint64) hi) << 32 | lo;

	/*
	 * BACKUP METHOD and BACKUP FROM lines are new in 9.2. We can't restore
	 * from an older backup anyway, but since the information on it is not
	 * strictly required, don't error out if it's missing for some reason.
	 */
	if (fscanf(lfp, "BACKUP METHOD: %19s\n", backuptype) == 1)
	{
		if (strcmp(backuptype, "streamed") == 0)
			*backupEndRequired = true;
	}

	if (fscanf(lfp, "BACKUP FROM: %19s\n", backupfrom) == 1)
	{
		if (strcmp(backupfrom, "standby") == 0)
			*backupFromStandby = true;
	}

	/*
	 * Parse START TIME and LABEL. Those are not mandatory fields for recovery
	 * but checking for their presence is useful for debugging and the next
	 * sanity checks. Cope also with the fact that the result buffers have a
	 * pre-allocated size, hence if the backup_label file has been generated
	 * with strings longer than the maximum assumed here an incorrect parsing
	 * happens. That's fine as only minor consistency checks are done
	 * afterwards.
	 */
	if (fscanf(lfp, "START TIME: %127[^\n]\n", backuptime) == 1)
		ereport(DEBUG1,
				(errmsg("backup time %s in file \"%s\"",
						backuptime, BACKUP_LABEL_FILE)));

	if (fscanf(lfp, "LABEL: %1023[^\n]\n", backuplabel) == 1)
		ereport(DEBUG1,
				(errmsg("backup label %s in file \"%s\"",
						backuplabel, BACKUP_LABEL_FILE)));

	/*
	 * START TIMELINE is new as of 11. Its parsing is not mandatory, still use
	 * it as a sanity check if present.
	 */
	if (fscanf(lfp, "START TIMELINE: %u\n", &tli_from_file) == 1)
	{
		if (tli_from_walseg != tli_from_file)
			ereport(FATAL,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("invalid data in file \"%s\"", BACKUP_LABEL_FILE),
					 errdetail("Timeline ID parsed is %u, but expected %u.",
							   tli_from_file, tli_from_walseg)));

		ereport(DEBUG1,
				(errmsg("backup timeline %u in file \"%s\"",
						tli_from_file, BACKUP_LABEL_FILE)));
	}

	if (ferror(lfp) || FreeFile(lfp))
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m",
						BACKUP_LABEL_FILE)));

	return true;
}

/*
 * read_tablespace_map: check to see if a tablespace_map file is present
 *
 * If we see a tablespace_map file during recovery, we assume that we are
 * recovering from a backup dump file, and we therefore need to create symlinks
 * as per the information present in tablespace_map file.
 *
 * Returns true if a tablespace_map file was found (and fills the link
 * information for all the tablespace links present in file); returns false
 * if not.
 */
static bool
read_tablespace_map(List **tablespaces)
{
	tablespaceinfo *ti;
	FILE	   *lfp;
	char		tbsoid[MAXPGPATH];
	char	   *tbslinkpath;
	char		str[MAXPGPATH];
	int			ch,
				prev_ch = -1,
				i = 0,
				n;

	/*
	 * See if tablespace_map file is present
	 */
	lfp = AllocateFile(TABLESPACE_MAP, "r");
	if (!lfp)
	{
		if (errno != ENOENT)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m",
							TABLESPACE_MAP)));
		return false;			/* it's not there, all is fine */
	}

	/*
	 * Read and parse the link name and path lines from tablespace_map file
	 * (this code is pretty crude, but we are not expecting any variability in
	 * the file format).  While taking backup we embed escape character '\\'
	 * before newline in tablespace path, so that during reading of
	 * tablespace_map file, we could distinguish newline in tablespace path
	 * and end of line.  Now while reading tablespace_map file, remove the
	 * escape character that has been added in tablespace path during backup.
	 */
	while ((ch = fgetc(lfp)) != EOF)
	{
		if ((ch == '\n' || ch == '\r') && prev_ch != '\\')
		{
			str[i] = '\0';
			if (sscanf(str, "%s %n", tbsoid, &n) != 1)
				ereport(FATAL,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("invalid data in file \"%s\"", TABLESPACE_MAP)));
			tbslinkpath = str + n;
			i = 0;

			ti = palloc(sizeof(tablespaceinfo));
			ti->oid = pstrdup(tbsoid);
			ti->path = pstrdup(tbslinkpath);

			*tablespaces = lappend(*tablespaces, ti);
			continue;
		}
		else if ((ch == '\n' || ch == '\r') && prev_ch == '\\')
			str[i - 1] = ch;
		else if (i < sizeof(str) - 1)
			str[i++] = ch;
		prev_ch = ch;
	}

	if (ferror(lfp) || FreeFile(lfp))
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m",
						TABLESPACE_MAP)));

	return true;
}

/*
 * Error context callback for errors occurring during rm_redo().
 */
static void
rm_redo_error_callback(void *arg)
{
	XLogReaderState *record = (XLogReaderState *) arg;
	StringInfoData buf;

	initStringInfo(&buf);
	xlog_outdesc(&buf, record);

	/* translator: %s is a WAL record description */
	errcontext("WAL redo at %X/%X for %s",
			   (uint32) (record->ReadRecPtr >> 32),
			   (uint32) record->ReadRecPtr,
			   buf.data);

	pfree(buf.data);
}

/*
 * BackupInProgress: check if online backup mode is active
 *
 * This is done by checking for existence of the "backup_label" file.
 */
bool
BackupInProgress(void)
{
	struct stat stat_buf;

	return (stat(BACKUP_LABEL_FILE, &stat_buf) == 0);
}

/*
 * CancelBackup: rename the "backup_label" and "tablespace_map"
 *				 files to cancel backup mode
 *
 * If the "backup_label" file exists, it will be renamed to "backup_label.old".
 * Similarly, if the "tablespace_map" file exists, it will be renamed to
 * "tablespace_map.old".
 *
 * Note that this will render an online backup in progress
 * useless. To correctly finish an online backup, pg_stop_backup must be
 * called.
 */
void
CancelBackup(void)
{
	struct stat stat_buf;

	/* if the backup_label file is not there, return */
	if (stat(BACKUP_LABEL_FILE, &stat_buf) < 0)
		return;

	/* remove leftover file from previously canceled backup if it exists */
	unlink(BACKUP_LABEL_OLD);

	if (durable_rename(BACKUP_LABEL_FILE, BACKUP_LABEL_OLD, DEBUG1) != 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("online backup mode was not canceled"),
				 errdetail("File \"%s\" could not be renamed to \"%s\": %m.",
						   BACKUP_LABEL_FILE, BACKUP_LABEL_OLD)));
		return;
	}

	/* if the tablespace_map file is not there, return */
	if (stat(TABLESPACE_MAP, &stat_buf) < 0)
	{
		ereport(LOG,
				(errmsg("online backup mode canceled"),
				 errdetail("File \"%s\" was renamed to \"%s\".",
						   BACKUP_LABEL_FILE, BACKUP_LABEL_OLD)));
		return;
	}

	/* remove leftover file from previously canceled backup if it exists */
	unlink(TABLESPACE_MAP_OLD);

	if (durable_rename(TABLESPACE_MAP, TABLESPACE_MAP_OLD, DEBUG1) == 0)
	{
		ereport(LOG,
				(errmsg("online backup mode canceled"),
				 errdetail("Files \"%s\" and \"%s\" were renamed to "
						   "\"%s\" and \"%s\", respectively.",
						   BACKUP_LABEL_FILE, TABLESPACE_MAP,
						   BACKUP_LABEL_OLD, TABLESPACE_MAP_OLD)));
	}
	else
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("online backup mode canceled"),
				 errdetail("File \"%s\" was renamed to \"%s\", but "
						   "file \"%s\" could not be renamed to \"%s\": %m.",
						   BACKUP_LABEL_FILE, BACKUP_LABEL_OLD,
						   TABLESPACE_MAP, TABLESPACE_MAP_OLD)));
	}
}

/*
 * Read the XLOG page containing RecPtr into readBuf (if not read already).
 * Returns number of bytes read, if the page is read successfully, or -1
 * in case of errors.  When errors occur, they are ereport'ed, but only
 * if they have not been previously reported.
 *
 * This is responsible for restoring files from archive as needed, as well
 * as for waiting for the requested WAL record to arrive in standby mode.
 *
 * 'emode' specifies the log level used for reporting "file not found" or
 * "end of WAL" situations in archive recovery, or in standby mode when a
 * trigger file is found. If set to WARNING or below, XLogPageRead() returns
 * false in those situations, on higher log levels the ereport() won't
 * return.
 *
 * In standby mode, if after a successful return of XLogPageRead() the
 * caller finds the record it's interested in to be broken, it should
 * ereport the error with the level determined by
 * emode_for_corrupt_record(), and then set lastSourceFailed
 * and call XLogPageRead() again with the same arguments. This lets
 * XLogPageRead() to try fetching the record from another source, or to
 * sleep and retry.
 */
static int
XLogPageRead(XLogReaderState *xlogreader, XLogRecPtr targetPagePtr, int reqLen,
			 XLogRecPtr targetRecPtr, char *readBuf)
{
	XLogPageReadPrivate *private =
	(XLogPageReadPrivate *) xlogreader->private_data;
	int			emode = private->emode;
	uint32		targetPageOff;
	XLogSegNo	targetSegNo PG_USED_FOR_ASSERTS_ONLY;
	int			r;

	XLByteToSeg(targetPagePtr, targetSegNo, wal_segment_size);
	targetPageOff = XLogSegmentOffset(targetPagePtr, wal_segment_size);

	/*
	 * See if we need to switch to a new segment because the requested record
	 * is not in the currently open one.
	 */
	if (readFile >= 0 &&
		!XLByteInSeg(targetPagePtr, readSegNo, wal_segment_size))
	{
		/*
		 * Request a restartpoint if we've replayed too much xlog since the
		 * last one.
		 */
		if (bgwriterLaunched)
		{
			if (XLogCheckpointNeeded(readSegNo))
			{
				(void) GetRedoRecPtr();
				if (XLogCheckpointNeeded(readSegNo))
					RequestCheckpoint(CHECKPOINT_CAUSE_XLOG);
			}
		}

		close(readFile);
		readFile = -1;
		readSource = XLOG_FROM_ANY;
	}

	XLByteToSeg(targetPagePtr, readSegNo, wal_segment_size);

retry:
	/* See if we need to retrieve more data */
	if (readFile < 0 ||
		(readSource == XLOG_FROM_STREAM &&
		 flushedUpto < targetPagePtr + reqLen))
	{
		if (!WaitForWALToBecomeAvailable(targetPagePtr + reqLen,
										 private->randAccess,
										 private->fetching_ckpt,
										 targetRecPtr))
		{
			if (readFile >= 0)
				close(readFile);
			readFile = -1;
			readLen = 0;
			readSource = XLOG_FROM_ANY;

			return -1;
		}
	}

	/*
	 * At this point, we have the right segment open and if we're streaming we
	 * know the requested record is in it.
	 */
	Assert(readFile != -1);

	/*
	 * If the current segment is being streamed from master, calculate how
	 * much of the current page we have received already. We know the
	 * requested record has been received, but this is for the benefit of
	 * future calls, to allow quick exit at the top of this function.
	 */
	if (readSource == XLOG_FROM_STREAM)
	{
		if (((targetPagePtr) / XLOG_BLCKSZ) != (flushedUpto / XLOG_BLCKSZ))
			readLen = XLOG_BLCKSZ;
		else
			readLen = XLogSegmentOffset(flushedUpto, wal_segment_size) -
				targetPageOff;
	}
	else
		readLen = XLOG_BLCKSZ;

	/* Read the requested page */
	readOff = targetPageOff;

	pgstat_report_wait_start(WAIT_EVENT_WAL_READ);
	r = pg_pread(readFile, readBuf, XLOG_BLCKSZ, (off_t) readOff);
	if (r != XLOG_BLCKSZ)
	{
		char		fname[MAXFNAMELEN];
		int			save_errno = errno;

		pgstat_report_wait_end();
		XLogFileName(fname, curFileTLI, readSegNo, wal_segment_size);
		if (r < 0)
		{
			errno = save_errno;
			ereport(emode_for_corrupt_record(emode, targetPagePtr + reqLen),
					(errcode_for_file_access(),
					 errmsg("could not read from log segment %s, offset %u: %m",
							fname, readOff)));
		}
		else
			ereport(emode_for_corrupt_record(emode, targetPagePtr + reqLen),
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("could not read from log segment %s, offset %u: read %d of %zu",
							fname, readOff, r, (Size) XLOG_BLCKSZ)));
		goto next_record_is_invalid;
	}
	pgstat_report_wait_end();

	Assert(targetSegNo == readSegNo);
	Assert(targetPageOff == readOff);
	Assert(reqLen <= readLen);

	xlogreader->seg.ws_tli = curFileTLI;

	/*
	 * Check the page header immediately, so that we can retry immediately if
	 * it's not valid. This may seem unnecessary, because XLogReadRecord()
	 * validates the page header anyway, and would propagate the failure up to
	 * ReadRecord(), which would retry. However, there's a corner case with
	 * continuation records, if a record is split across two pages such that
	 * we would need to read the two pages from different sources. For
	 * example, imagine a scenario where a streaming replica is started up,
	 * and replay reaches a record that's split across two WAL segments. The
	 * first page is only available locally, in pg_wal, because it's already
	 * been recycled in the master. The second page, however, is not present
	 * in pg_wal, and we should stream it from the master. There is a recycled
	 * WAL segment present in pg_wal, with garbage contents, however. We would
	 * read the first page from the local WAL segment, but when reading the
	 * second page, we would read the bogus, recycled, WAL segment. If we
	 * didn't catch that case here, we would never recover, because
	 * ReadRecord() would retry reading the whole record from the beginning.
	 *
	 * Of course, this only catches errors in the page header, which is what
	 * happens in the case of a recycled WAL segment. Other kinds of errors or
	 * corruption still has the same problem. But this at least fixes the
	 * common case, which can happen as part of normal operation.
	 *
	 * Validating the page header is cheap enough that doing it twice
	 * shouldn't be a big deal from a performance point of view.
	 */
	if (!XLogReaderValidatePageHeader(xlogreader, targetPagePtr, readBuf))
	{
		/* reset any error XLogReaderValidatePageHeader() might have set */
		xlogreader->errormsg_buf[0] = '\0';
		goto next_record_is_invalid;
	}

	return readLen;

next_record_is_invalid:
	lastSourceFailed = true;

	if (readFile >= 0)
		close(readFile);
	readFile = -1;
	readLen = 0;
	readSource = XLOG_FROM_ANY;

	/* In standby-mode, keep trying */
	if (StandbyMode)
		goto retry;
	else
		return -1;
}

/*
 * Open the WAL segment containing WAL location 'RecPtr'.
 *
 * The segment can be fetched via restore_command, or via walreceiver having
 * streamed the record, or it can already be present in pg_wal. Checking
 * pg_wal is mainly for crash recovery, but it will be polled in standby mode
 * too, in case someone copies a new segment directly to pg_wal. That is not
 * documented or recommended, though.
 *
 * If 'fetching_ckpt' is true, we're fetching a checkpoint record, and should
 * prepare to read WAL starting from RedoStartLSN after this.
 *
 * 'RecPtr' might not point to the beginning of the record we're interested
 * in, it might also point to the page or segment header. In that case,
 * 'tliRecPtr' is the position of the WAL record we're interested in. It is
 * used to decide which timeline to stream the requested WAL from.
 *
 * If the record is not immediately available, the function returns false
 * if we're not in standby mode. In standby mode, waits for it to become
 * available.
 *
 * When the requested record becomes available, the function opens the file
 * containing it (if not open already), and returns true. When end of standby
 * mode is triggered by the user, and there is no more WAL available, returns
 * false.
 */
static bool
WaitForWALToBecomeAvailable(XLogRecPtr RecPtr, bool randAccess,
							bool fetching_ckpt, XLogRecPtr tliRecPtr)
{
	static TimestampTz last_fail_time = 0;
	TimestampTz now;
	bool		streaming_reply_sent = false;

	/*-------
	 * Standby mode is implemented by a state machine:
	 *
	 * 1. Read from either archive or pg_wal (XLOG_FROM_ARCHIVE), or just
	 *	  pg_wal (XLOG_FROM_PG_WAL)
	 * 2. Check trigger file
	 * 3. Read from primary server via walreceiver (XLOG_FROM_STREAM)
	 * 4. Rescan timelines
	 * 5. Sleep wal_retrieve_retry_interval milliseconds, and loop back to 1.
	 *
	 * Failure to read from the current source advances the state machine to
	 * the next state.
	 *
	 * 'currentSource' indicates the current state. There are no currentSource
	 * values for "check trigger", "rescan timelines", and "sleep" states,
	 * those actions are taken when reading from the previous source fails, as
	 * part of advancing to the next state.
	 *
	 * If standby mode is turned off while reading WAL from stream, we move
	 * to XLOG_FROM_ARCHIVE and reset lastSourceFailed, to force fetching
	 * the files (which would be required at end of recovery, e.g., timeline
	 * history file) from archive or pg_wal. We don't need to kill WAL receiver
	 * here because it's already stopped when standby mode is turned off at
	 * the end of recovery.
	 *-------
	 */
	if (!InArchiveRecovery)
		currentSource = XLOG_FROM_PG_WAL;
	else if (currentSource == XLOG_FROM_ANY ||
			 (!StandbyMode && currentSource == XLOG_FROM_STREAM))
	{
		lastSourceFailed = false;
		currentSource = XLOG_FROM_ARCHIVE;
	}

	for (;;)
	{
		XLogSource	oldSource = currentSource;
		bool		startWalReceiver = false;

		/*
		 * First check if we failed to read from the current source, and
		 * advance the state machine if so. The failure to read might've
		 * happened outside this function, e.g when a CRC check fails on a
		 * record, or within this loop.
		 */
		if (lastSourceFailed)
		{
			switch (currentSource)
			{
				case XLOG_FROM_ARCHIVE:
				case XLOG_FROM_PG_WAL:

					/*
					 * Check to see if the trigger file exists. Note that we
					 * do this only after failure, so when you create the
					 * trigger file, we still finish replaying as much as we
					 * can from archive and pg_wal before failover.
					 */
					if (StandbyMode && CheckForStandbyTrigger())
					{
						ShutdownWalRcv();
						return false;
					}

					/*
					 * Not in standby mode, and we've now tried the archive
					 * and pg_wal.
					 */
					if (!StandbyMode)
						return false;

					/*
					 * Move to XLOG_FROM_STREAM state, and set to start a
					 * walreceiver if necessary.
					 */
					currentSource = XLOG_FROM_STREAM;
					startWalReceiver = true;
					break;

				case XLOG_FROM_STREAM:

					/*
					 * Failure while streaming. Most likely, we got here
					 * because streaming replication was terminated, or
					 * promotion was triggered. But we also get here if we
					 * find an invalid record in the WAL streamed from master,
					 * in which case something is seriously wrong. There's
					 * little chance that the problem will just go away, but
					 * PANIC is not good for availability either, especially
					 * in hot standby mode. So, we treat that the same as
					 * disconnection, and retry from archive/pg_wal again. The
					 * WAL in the archive should be identical to what was
					 * streamed, so it's unlikely that it helps, but one can
					 * hope...
					 */

					/*
					 * We should be able to move to XLOG_FROM_STREAM only in
					 * standby mode.
					 */
					Assert(StandbyMode);

					/*
					 * Before we leave XLOG_FROM_STREAM state, make sure that
					 * walreceiver is not active, so that it won't overwrite
					 * WAL that we restore from archive.
					 */
					if (WalRcvStreaming())
						ShutdownWalRcv();

					/*
					 * Before we sleep, re-scan for possible new timelines if
					 * we were requested to recover to the latest timeline.
					 */
					if (recoveryTargetTimeLineGoal == RECOVERY_TARGET_TIMELINE_LATEST)
					{
						if (rescanLatestTimeLine())
						{
							currentSource = XLOG_FROM_ARCHIVE;
							break;
						}
					}

					/*
					 * XLOG_FROM_STREAM is the last state in our state
					 * machine, so we've exhausted all the options for
					 * obtaining the requested WAL. We're going to loop back
					 * and retry from the archive, but if it hasn't been long
					 * since last attempt, sleep wal_retrieve_retry_interval
					 * milliseconds to avoid busy-waiting.
					 */
					now = GetCurrentTimestamp();
					if (!TimestampDifferenceExceeds(last_fail_time, now,
													wal_retrieve_retry_interval))
					{
						long		wait_time;

						wait_time = wal_retrieve_retry_interval -
							TimestampDifferenceMilliseconds(last_fail_time, now);

						/* Do background tasks that might benefit us later. */
						KnownAssignedTransactionIdsIdleMaintenance();

						(void) WaitLatch(&XLogCtl->recoveryWakeupLatch,
										 WL_LATCH_SET | WL_TIMEOUT |
										 WL_EXIT_ON_PM_DEATH,
										 wait_time,
										 WAIT_EVENT_RECOVERY_RETRIEVE_RETRY_INTERVAL);
						ResetLatch(&XLogCtl->recoveryWakeupLatch);
						now = GetCurrentTimestamp();

						/* Handle interrupt signals of startup process */
						HandleStartupProcInterrupts();
					}
					last_fail_time = now;
					currentSource = XLOG_FROM_ARCHIVE;
					break;

				default:
					elog(ERROR, "unexpected WAL source %d", currentSource);
			}
		}
		else if (currentSource == XLOG_FROM_PG_WAL)
		{
			/*
			 * We just successfully read a file in pg_wal. We prefer files in
			 * the archive over ones in pg_wal, so try the next file again
			 * from the archive first.
			 */
			if (InArchiveRecovery)
				currentSource = XLOG_FROM_ARCHIVE;
		}

		if (currentSource != oldSource)
			elog(DEBUG2, "switched WAL source from %s to %s after %s",
				 xlogSourceNames[oldSource], xlogSourceNames[currentSource],
				 lastSourceFailed ? "failure" : "success");

		/*
		 * We've now handled possible failure. Try to read from the chosen
		 * source.
		 */
		lastSourceFailed = false;

		switch (currentSource)
		{
			case XLOG_FROM_ARCHIVE:
			case XLOG_FROM_PG_WAL:

				/*
				 * WAL receiver must not be running when reading WAL from
				 * archive or pg_wal.
				 */
				Assert(!WalRcvStreaming());

				/* Close any old file we might have open. */
				if (readFile >= 0)
				{
					close(readFile);
					readFile = -1;
				}
				/* Reset curFileTLI if random fetch. */
				if (randAccess)
					curFileTLI = 0;

				/*
				 * Try to restore the file from archive, or read an existing
				 * file from pg_wal.
				 */
				readFile = XLogFileReadAnyTLI(readSegNo, DEBUG2,
											  currentSource == XLOG_FROM_ARCHIVE ? XLOG_FROM_ANY :
											  currentSource);
				if (readFile >= 0)
					return true;	/* success! */

				/*
				 * Nope, not found in archive or pg_wal.
				 */
				lastSourceFailed = true;
				break;

			case XLOG_FROM_STREAM:
				{
					bool		havedata;

					/*
					 * We should be able to move to XLOG_FROM_STREAM only in
					 * standby mode.
					 */
					Assert(StandbyMode);

					/*
					 * First, shutdown walreceiver if its restart has been
					 * requested -- but no point if we're already slated for
					 * starting it.
					 */
					if (pendingWalRcvRestart && !startWalReceiver)
					{
						ShutdownWalRcv();

						/*
						 * Re-scan for possible new timelines if we were
						 * requested to recover to the latest timeline.
						 */
						if (recoveryTargetTimeLineGoal ==
							RECOVERY_TARGET_TIMELINE_LATEST)
							rescanLatestTimeLine();

						startWalReceiver = true;
					}
					pendingWalRcvRestart = false;

					/*
					 * Launch walreceiver if needed.
					 *
					 * If fetching_ckpt is true, RecPtr points to the initial
					 * checkpoint location. In that case, we use RedoStartLSN
					 * as the streaming start position instead of RecPtr, so
					 * that when we later jump backwards to start redo at
					 * RedoStartLSN, we will have the logs streamed already.
					 */
					if (startWalReceiver &&
						PrimaryConnInfo && strcmp(PrimaryConnInfo, "") != 0)
					{
						XLogRecPtr	ptr;
						TimeLineID	tli;

						if (fetching_ckpt)
						{
							ptr = RedoStartLSN;
							tli = ControlFile->checkPointCopy.ThisTimeLineID;
						}
						else
						{
							ptr = RecPtr;

							/*
							 * Use the record begin position to determine the
							 * TLI, rather than the position we're reading.
							 */
							tli = tliOfPointInHistory(tliRecPtr, expectedTLEs);

							if (curFileTLI > 0 && tli < curFileTLI)
								elog(ERROR, "according to history file, WAL location %X/%X belongs to timeline %u, but previous recovered WAL file came from timeline %u",
									 (uint32) (tliRecPtr >> 32),
									 (uint32) tliRecPtr,
									 tli, curFileTLI);
						}
						curFileTLI = tli;
						RequestXLogStreaming(tli, ptr, PrimaryConnInfo,
											 PrimarySlotName,
											 wal_receiver_create_temp_slot);
						flushedUpto = 0;
					}

					/*
					 * Check if WAL receiver is active or wait to start up.
					 */
					if (!WalRcvStreaming())
					{
						lastSourceFailed = true;
						break;
					}

					/*
					 * Walreceiver is active, so see if new data has arrived.
					 *
					 * We only advance XLogReceiptTime when we obtain fresh
					 * WAL from walreceiver and observe that we had already
					 * processed everything before the most recent "chunk"
					 * that it flushed to disk.  In steady state where we are
					 * keeping up with the incoming data, XLogReceiptTime will
					 * be updated on each cycle. When we are behind,
					 * XLogReceiptTime will not advance, so the grace time
					 * allotted to conflicting queries will decrease.
					 */
					if (RecPtr < flushedUpto)
						havedata = true;
					else
					{
						XLogRecPtr	latestChunkStart;

						flushedUpto = GetWalRcvFlushRecPtr(&latestChunkStart, &receiveTLI);
						if (RecPtr < flushedUpto && receiveTLI == curFileTLI)
						{
							havedata = true;
							if (latestChunkStart <= RecPtr)
							{
								XLogReceiptTime = GetCurrentTimestamp();
								SetCurrentChunkStartTime(XLogReceiptTime);
							}
						}
						else
							havedata = false;
					}
					if (havedata)
					{
						/*
						 * Great, streamed far enough.  Open the file if it's
						 * not open already.  Also read the timeline history
						 * file if we haven't initialized timeline history
						 * yet; it should be streamed over and present in
						 * pg_wal by now.  Use XLOG_FROM_STREAM so that source
						 * info is set correctly and XLogReceiptTime isn't
						 * changed.
						 *
						 * NB: We must set readTimeLineHistory based on
						 * recoveryTargetTLI, not receiveTLI. Normally they'll
						 * be the same, but if recovery_target_timeline is
						 * 'latest' and archiving is configured, then it's
						 * possible that we managed to retrieve one or more
						 * new timeline history files from the archive,
						 * updating recoveryTargetTLI.
						 */
						if (readFile < 0)
						{
							if (!expectedTLEs)
								expectedTLEs = readTimeLineHistory(recoveryTargetTLI);
							readFile = XLogFileRead(readSegNo, PANIC,
													receiveTLI,
													XLOG_FROM_STREAM, false);
							Assert(readFile >= 0);
						}
						else
						{
							/* just make sure source info is correct... */
							readSource = XLOG_FROM_STREAM;
							XLogReceiptSource = XLOG_FROM_STREAM;
							return true;
						}
						break;
					}

					/*
					 * Data not here yet. Check for trigger, then wait for
					 * walreceiver to wake us up when new WAL arrives.
					 */
					if (CheckForStandbyTrigger())
					{
						/*
						 * Note that we don't "return false" immediately here.
						 * After being triggered, we still want to replay all
						 * the WAL that was already streamed. It's in pg_wal
						 * now, so we just treat this as a failure, and the
						 * state machine will move on to replay the streamed
						 * WAL from pg_wal, and then recheck the trigger and
						 * exit replay.
						 */
						lastSourceFailed = true;
						break;
					}

					/*
					 * Since we have replayed everything we have received so
					 * far and are about to start waiting for more WAL, let's
					 * tell the upstream server our replay location now so
					 * that pg_stat_replication doesn't show stale
					 * information.
					 */
					if (!streaming_reply_sent)
					{
						WalRcvForceReply();
						streaming_reply_sent = true;
					}

					/* Do any background tasks that might benefit us later. */
					KnownAssignedTransactionIdsIdleMaintenance();

					/*
					 * Wait for more WAL to arrive. Time out after 5 seconds
					 * to react to a trigger file promptly and to check if the
					 * WAL receiver is still active.
					 */
					(void) WaitLatch(&XLogCtl->recoveryWakeupLatch,
									 WL_LATCH_SET | WL_TIMEOUT |
									 WL_EXIT_ON_PM_DEATH,
									 5000L, WAIT_EVENT_RECOVERY_WAL_STREAM);
					ResetLatch(&XLogCtl->recoveryWakeupLatch);
					break;
				}

			default:
				elog(ERROR, "unexpected WAL source %d", currentSource);
		}

		/*
		 * This possibly-long loop needs to handle interrupts of startup
		 * process.
		 */
		HandleStartupProcInterrupts();
	}

	return false;				/* not reached */
}

/*
 * Set flag to signal the walreceiver to restart.  (The startup process calls
 * this on noticing a relevant configuration change.)
 */
void
StartupRequestWalReceiverRestart(void)
{
	if (currentSource == XLOG_FROM_STREAM && WalRcvRunning())
	{
		ereport(LOG,
				(errmsg("WAL receiver process shutdown requested")));

		pendingWalRcvRestart = true;
	}
}

/*
 * Determine what log level should be used to report a corrupt WAL record
 * in the current WAL page, previously read by XLogPageRead().
 *
 * 'emode' is the error mode that would be used to report a file-not-found
 * or legitimate end-of-WAL situation.   Generally, we use it as-is, but if
 * we're retrying the exact same record that we've tried previously, only
 * complain the first time to keep the noise down.  However, we only do when
 * reading from pg_wal, because we don't expect any invalid records in archive
 * or in records streamed from master. Files in the archive should be complete,
 * and we should never hit the end of WAL because we stop and wait for more WAL
 * to arrive before replaying it.
 *
 * NOTE: This function remembers the RecPtr value it was last called with,
 * to suppress repeated messages about the same record. Only call this when
 * you are about to ereport(), or you might cause a later message to be
 * erroneously suppressed.
 */
static int
emode_for_corrupt_record(int emode, XLogRecPtr RecPtr)
{
	static XLogRecPtr lastComplaint = 0;

	if (readSource == XLOG_FROM_PG_WAL && emode == LOG)
	{
		if (RecPtr == lastComplaint)
			emode = DEBUG1;
		else
			lastComplaint = RecPtr;
	}
	return emode;
}

/*
 * Has a standby promotion already been triggered?
 *
 * Unlike CheckForStandbyTrigger(), this works in any process
 * that's connected to shared memory.
 */
bool
PromoteIsTriggered(void)
{
	/*
	 * We check shared state each time only until a standby promotion is
	 * triggered. We can't trigger a promotion again, so there's no need to
	 * keep checking after the shared variable has once been seen true.
	 */
	if (LocalPromoteIsTriggered)
		return true;

	SpinLockAcquire(&XLogCtl->info_lck);
	LocalPromoteIsTriggered = XLogCtl->SharedPromoteIsTriggered;
	SpinLockRelease(&XLogCtl->info_lck);

	return LocalPromoteIsTriggered;
}

static void
SetPromoteIsTriggered(void)
{
	SpinLockAcquire(&XLogCtl->info_lck);
	XLogCtl->SharedPromoteIsTriggered = true;
	SpinLockRelease(&XLogCtl->info_lck);

	LocalPromoteIsTriggered = true;
}

/*
 * Check to see whether the user-specified trigger file exists and whether a
 * promote request has arrived.  If either condition holds, return true.
 */
static bool
CheckForStandbyTrigger(void)
{
	struct stat stat_buf;

	if (LocalPromoteIsTriggered)
		return true;

	if (IsPromoteSignaled())
	{
		/*
		 * In 9.1 and 9.2 the postmaster unlinked the promote file inside the
		 * signal handler. It now leaves the file in place and lets the
		 * Startup process do the unlink. This allows Startup to know whether
		 * it should create a full checkpoint before starting up (fallback
		 * mode). Fast promotion takes precedence.
		 */
		if (stat(PROMOTE_SIGNAL_FILE, &stat_buf) == 0)
		{
			unlink(PROMOTE_SIGNAL_FILE);
			unlink(FALLBACK_PROMOTE_SIGNAL_FILE);
			fast_promote = true;
		}
		else if (stat(FALLBACK_PROMOTE_SIGNAL_FILE, &stat_buf) == 0)
		{
			unlink(FALLBACK_PROMOTE_SIGNAL_FILE);
			fast_promote = false;
		}

		ereport(LOG, (errmsg("received promote request")));

		ResetPromoteSignaled();
		SetPromoteIsTriggered();
		return true;
	}

	if (PromoteTriggerFile == NULL || strcmp(PromoteTriggerFile, "") == 0)
		return false;

	if (stat(PromoteTriggerFile, &stat_buf) == 0)
	{
		ereport(LOG,
				(errmsg("promote trigger file found: %s", PromoteTriggerFile)));
		unlink(PromoteTriggerFile);
		SetPromoteIsTriggered();
		fast_promote = true;
		return true;
	}
	else if (errno != ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat promote trigger file \"%s\": %m",
						PromoteTriggerFile)));

	return false;
}

/*
 * Remove the files signaling a standby promotion request.
 */
void
RemovePromoteSignalFiles(void)
{
	unlink(PROMOTE_SIGNAL_FILE);
	unlink(FALLBACK_PROMOTE_SIGNAL_FILE);
}

/*
 * Check to see if a promote request has arrived. Should be
 * called by postmaster after receiving SIGUSR1.
 */
bool
CheckPromoteSignal(void)
{
	struct stat stat_buf;

	if (stat(PROMOTE_SIGNAL_FILE, &stat_buf) == 0 ||
		stat(FALLBACK_PROMOTE_SIGNAL_FILE, &stat_buf) == 0)
		return true;

	return false;
}

/*
 * Wake up startup process to replay newly arrived WAL, or to notice that
 * failover has been requested.
 */
void
WakeupRecovery(void)
{
	SetLatch(&XLogCtl->recoveryWakeupLatch);
}

/*
 * Update the WalWriterSleeping flag.
 */
void
SetWalWriterSleeping(bool sleeping)
{
	SpinLockAcquire(&XLogCtl->info_lck);
	XLogCtl->WalWriterSleeping = sleeping;
	SpinLockRelease(&XLogCtl->info_lck);
}

/*
 * Schedule a walreceiver wakeup in the main recovery loop.
 */
void
XLogRequestWalReceiverReply(void)
{
	doRequestWalReceiverReply = true;
}
