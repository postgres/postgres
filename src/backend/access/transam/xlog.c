/*-------------------------------------------------------------------------
 *
 * xlog.c
 *		PostgreSQL write-ahead log manager
 *
 * The Write-Ahead Log (WAL) functionality is split into several source
 * files, in addition to this one:
 *
 * xloginsert.c - Functions for constructing WAL records
 * xlogrecovery.c - WAL recovery and standby code
 * xlogreader.c - Facility for reading WAL files and parsing WAL records
 * xlogutils.c - Helper functions for WAL redo routines
 *
 * This file contains functions for coordinating database startup and
 * checkpointing, and managing the write-ahead log buffers when the
 * system is running.
 *
 * StartupXLOG() is the main entry point of the startup process.  It
 * coordinates database startup, performing WAL recovery, and the
 * transition from WAL recovery into normal operations.
 *
 * XLogInsertRecord() inserts a WAL record into the WAL buffers.  Most
 * callers should not call this directly, but use the functions in
 * xloginsert.c to construct the WAL record.  XLogFlush() can be used
 * to force the WAL to disk.
 *
 * In addition to those, there are many other functions for interrogating
 * the current system state, and for starting/stopping backups.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "backup/basebackup.h"
#include "catalog/catversion.h"
#include "catalog/pg_control.h"
#include "catalog/pg_database.h"
#include "common/controldata_utils.h"
#include "common/file_utils.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "pgstat.h"
#include "port/atomics.h"
#include "postmaster/bgwriter.h"
#include "postmaster/startup.h"
#include "postmaster/walsummarizer.h"
#include "postmaster/walwriter.h"
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
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/reinit.h"
#include "storage/spin.h"
#include "storage/sync.h"
#include "utils/guc_hooks.h"
#include "utils/guc_tables.h"
#include "utils/injection_point.h"
#include "utils/ps_status.h"
#include "utils/relmapper.h"
#include "utils/snapmgr.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"
#include "utils/varlena.h"

#ifdef WAL_DEBUG
#include "utils/memutils.h"
#endif

/* timeline ID to be used when bootstrapping */
#define BootstrapTimeLineID		1

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
int			wal_compression = WAL_COMPRESSION_NONE;
char	   *wal_consistency_checking_string = NULL;
bool	   *wal_consistency_checking = NULL;
bool		wal_init_zero = true;
bool		wal_recycle = true;
bool		log_checkpoints = true;
int			wal_sync_method = DEFAULT_WAL_SYNC_METHOD;
int			wal_level = WAL_LEVEL_REPLICA;
int			CommitDelay = 0;	/* precommit delay in microseconds */
int			CommitSiblings = 5; /* # concurrent xacts needed to sleep */
int			wal_retrieve_retry_interval = 5000;
int			max_slot_wal_keep_size_mb = -1;
int			wal_decode_buffer_size = 512 * 1024;
bool		track_wal_io_timing = false;

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
 * Track whether there were any deferred checks for custom resource managers
 * specified in wal_consistency_checking.
 */
static bool check_wal_consistency_checking_deferred = false;

/*
 * GUC support
 */
const struct config_enum_entry wal_sync_method_options[] = {
	{"fsync", WAL_SYNC_METHOD_FSYNC, false},
#ifdef HAVE_FSYNC_WRITETHROUGH
	{"fsync_writethrough", WAL_SYNC_METHOD_FSYNC_WRITETHROUGH, false},
#endif
	{"fdatasync", WAL_SYNC_METHOD_FDATASYNC, false},
#ifdef O_SYNC
	{"open_sync", WAL_SYNC_METHOD_OPEN, false},
#endif
#ifdef O_DSYNC
	{"open_datasync", WAL_SYNC_METHOD_OPEN_DSYNC, false},
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

/*
 * Statistics for current checkpoint are collected in this global struct.
 * Because only the checkpointer or a stand-alone backend can perform
 * checkpoints, this will be unused in normal backends.
 */
CheckpointStatsData CheckpointStats;

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
 * see GetRedoRecPtr.
 *
 * NB: Code that uses this variable must be prepared not only for the
 * possibility that it may be arbitrarily out of date, but also for the
 * possibility that it might be set to InvalidXLogRecPtr. We used to
 * initialize it as a side effect of the first call to RecoveryInProgress(),
 * which meant that most code that might use it could assume that it had a
 * real if perhaps stale value. That's no longer the case.
 */
static XLogRecPtr RedoRecPtr;

/*
 * doPageWrites is this backend's local copy of (fullPageWrites ||
 * runningBackups > 0).  It is used together with RedoRecPtr to decide whether
 * a full-page image of a page need to be taken.
 *
 * NB: Initially this is false, and there's no guarantee that it will be
 * initialized to any other value before it is first used. Any code that
 * makes use of it must recheck the value after obtaining a WALInsertLock,
 * and respond appropriately if it turns out that the previous value wasn't
 * accurate.
 */
static bool doPageWrites;

/*----------
 * Shared-memory data structures for XLOG control
 *
 * LogwrtRqst indicates a byte position that we need to write and/or fsync
 * the log up to (all records before that point must be written or fsynced).
 * The positions already written/fsynced are maintained in logWriteResult
 * and logFlushResult using atomic access.
 * In addition to the shared variable, each backend has a private copy of
 * both in LogwrtResult, which is updated when convenient.
 *
 * The request bookkeeping is simpler: there is a shared XLogCtl->LogwrtRqst
 * (protected by info_lck), but we don't need to cache any copies of it.
 *
 * info_lck is only held long enough to read/update the protected variables,
 * so it's a plain spinlock.  The other locks are held longer (potentially
 * over I/O operations), so we use LWLocks for them.  These locks are:
 *
 * WALWriteLock: must be held to write WAL buffers to disk (XLogWrite or
 * XLogFlush).
 *
 * ControlFileLock: must be held to read/update control file or create
 * new log file.
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
	pg_atomic_uint64 insertingAt;
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
	 * fullPageWrites is the authoritative value used by all backends to
	 * determine whether to write full-page image to WAL. This shared value,
	 * instead of the process-local fullPageWrites, is required because, when
	 * full_page_writes is changed by SIGHUP, we must WAL-log it before it
	 * actually affects WAL-logging by backends.  Checkpointer sets at startup
	 * or after SIGHUP.
	 *
	 * To read these fields, you must hold an insertion lock. To modify them,
	 * you must hold ALL the locks.
	 */
	XLogRecPtr	RedoRecPtr;		/* current redo point for insertions */
	bool		fullPageWrites;

	/*
	 * runningBackups is a counter indicating the number of backups currently
	 * in progress. lastBackupStart is the latest checkpoint redo location
	 * used as a starting point for an online backup.
	 */
	int			runningBackups;
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
	XLogRecPtr	asyncXactLSN;	/* LSN of newest async commit/abort */
	XLogRecPtr	replicationSlotMinLSN;	/* oldest LSN needed by any slot */

	XLogSegNo	lastRemovedSegNo;	/* latest removed/recycled XLOG segment */

	/* Fake LSN counter, for unlogged relations. */
	pg_atomic_uint64 unloggedLSN;

	/* Time and LSN of last xlog segment switch. Protected by WALWriteLock. */
	pg_time_t	lastSegSwitchTime;
	XLogRecPtr	lastSegSwitchLSN;

	/* These are accessed using atomics -- info_lck not needed */
	pg_atomic_uint64 logInsertResult;	/* last byte + 1 inserted to buffers */
	pg_atomic_uint64 logWriteResult;	/* last byte + 1 written out */
	pg_atomic_uint64 logFlushResult;	/* last byte + 1 flushed */

	/*
	 * First initialized page in the cache (first byte position).
	 */
	XLogRecPtr	InitializedFrom;

	/*
	 * Latest reserved for initialization page in the cache (last byte
	 * position + 1).
	 *
	 * To change the identity of a buffer, you need to advance
	 * InitializeReserved first.  To change the identity of a buffer that's
	 * still dirty, the old page needs to be written out first, and for that
	 * you need WALWriteLock, and you need to ensure that there are no
	 * in-progress insertions to the page by calling
	 * WaitXLogInsertionsToFinish().
	 */
	pg_atomic_uint64 InitializeReserved;

	/*
	 * Latest initialized page in the cache (last byte position + 1).
	 *
	 * InitializedUpTo is updated after the buffer initialization.  After
	 * update, waiters got notification using InitializedUpToCondVar.
	 */
	pg_atomic_uint64 InitializedUpTo;
	ConditionVariable InitializedUpToCondVar;

	/*
	 * These values do not change after startup, although the pointed-to pages
	 * and xlblocks values certainly do.  xlblocks values are changed
	 * lock-free according to the check for the xlog write position and are
	 * accompanied by changes of InitializeReserved and InitializedUpTo.
	 */
	char	   *pages;			/* buffers for unwritten XLOG pages */
	pg_atomic_uint64 *xlblocks; /* 1st byte ptr-s + XLOG_BLCKSZ */
	int			XLogCacheBlck;	/* highest allocated xlog buffer index */

	/*
	 * InsertTimeLineID is the timeline into which new WAL is being inserted
	 * and flushed. It is zero during recovery, and does not change once set.
	 *
	 * If we create a new timeline when the system was started up,
	 * PrevTimeLineID is the old timeline's ID that we forked off from.
	 * Otherwise it's equal to InsertTimeLineID.
	 *
	 * We set these fields while holding info_lck. Most that reads these
	 * values knows that recovery is no longer in progress and so can safely
	 * read the value without a lock, but code that could be run either during
	 * or after recovery can take info_lck while reading these values.
	 */
	TimeLineID	InsertTimeLineID;
	TimeLineID	PrevTimeLineID;

	/*
	 * SharedRecoveryState indicates if we're still in crash or archive
	 * recovery.  Protected by info_lck.
	 */
	RecoveryState SharedRecoveryState;

	/*
	 * InstallXLogFileSegmentActive indicates whether the checkpointer should
	 * arrange for future segments by recycling and/or PreallocXlogFiles().
	 * Protected by ControlFileLock.  Only the startup process changes it.  If
	 * true, anyone can use InstallXLogFileSegment().  If false, the startup
	 * process owns the exclusive right to install segments, by reading from
	 * the archive and possibly replacing existing files.
	 */
	bool		InstallXLogFileSegmentActive;

	/*
	 * WalWriterSleeping indicates whether the WAL writer is currently in
	 * low-power mode (and hence should be nudged if an async commit occurs).
	 * Protected by info_lck.
	 */
	bool		WalWriterSleeping;

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
	 * lastFpwDisableRecPtr points to the start of the last replayed
	 * XLOG_FPW_CHANGE record that instructs full_page_writes is disabled.
	 */
	XLogRecPtr	lastFpwDisableRecPtr;

	slock_t		info_lck;		/* locks shared variables shown above */
} XLogCtlData;

/*
 * Classification of XLogInsertRecord operations.
 */
typedef enum
{
	WALINSERT_NORMAL,
	WALINSERT_SPECIAL_SWITCH,
	WALINSERT_SPECIAL_CHECKPOINT
} WalInsertClass;

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
 * Update local copy of shared XLogCtl->log{Write,Flush}Result
 *
 * It's critical that Flush always trails Write, so the order of the reads is
 * important, as is the barrier.  See also XLogWrite.
 */
#define RefreshXLogWriteResult(_target) \
	do { \
		_target.Flush = pg_atomic_read_u64(&XLogCtl->logFlushResult); \
		pg_read_barrier(); \
		_target.Write = pg_atomic_read_u64(&XLogCtl->logWriteResult); \
	} while (0)

/*
 * openLogFile is -1 or a kernel FD for an open log file segment.
 * openLogSegNo identifies the segment, and openLogTLI the corresponding TLI.
 * These variables are only used to write the XLOG, and so will normally refer
 * to the active segment.
 *
 * Note: call Reserve/ReleaseExternalFD to track consumption of this FD.
 */
static int	openLogFile = -1;
static XLogSegNo openLogSegNo = 0;
static TimeLineID openLogTLI = 0;

/*
 * Local copies of equivalent fields in the control file.  When running
 * crash recovery, LocalMinRecoveryPoint is set to InvalidXLogRecPtr as we
 * expect to replay all the WAL available, and updateMinRecoveryPoint is
 * switched to false to prevent any updates while replaying records.
 * Those values are kept consistent as long as crash recovery runs.
 */
static XLogRecPtr LocalMinRecoveryPoint;
static TimeLineID LocalMinRecoveryPointTLI;
static bool updateMinRecoveryPoint = true;

/* For WALInsertLockAcquire/Release functions */
static int	MyLockNo = 0;
static bool holdingAllLocks = false;

#ifdef WAL_DEBUG
static MemoryContext walDebugCxt = NULL;
#endif

static void CleanupAfterArchiveRecovery(TimeLineID EndOfLogTLI,
										XLogRecPtr EndOfLog,
										TimeLineID newTLI);
static void CheckRequiredParameterValues(void);
static void XLogReportParameters(void);
static int	LocalSetXLogInsertAllowed(void);
static void CreateEndOfRecoveryRecord(void);
static XLogRecPtr CreateOverwriteContrecordRecord(XLogRecPtr aborted_lsn,
												  XLogRecPtr pagePtr,
												  TimeLineID newTLI);
static void CheckPointGuts(XLogRecPtr checkPointRedo, int flags);
static void KeepLogSeg(XLogRecPtr recptr, XLogSegNo *logSegNo);
static XLogRecPtr XLogGetReplicationSlotMinimumLSN(void);

static void AdvanceXLInsertBuffer(XLogRecPtr upto, TimeLineID tli,
								  bool opportunistic);
static void XLogWrite(XLogwrtRqst WriteRqst, TimeLineID tli, bool flexible);
static bool InstallXLogFileSegment(XLogSegNo *segno, char *tmppath,
								   bool find_free, XLogSegNo max_segno,
								   TimeLineID tli);
static void XLogFileClose(void);
static void PreallocXlogFiles(XLogRecPtr endptr, TimeLineID tli);
static void RemoveTempXlogFiles(void);
static void RemoveOldXlogFiles(XLogSegNo segno, XLogRecPtr lastredoptr,
							   XLogRecPtr endptr, TimeLineID insertTLI);
static void RemoveXlogFile(const struct dirent *segment_de,
						   XLogSegNo recycleSegNo, XLogSegNo *endlogSegNo,
						   TimeLineID insertTLI);
static void UpdateLastRemovedPtr(char *filename);
static void ValidateXLOGDirectoryStructure(void);
static void CleanupBackupHistory(void);
static void UpdateMinRecoveryPoint(XLogRecPtr lsn, bool force);
static bool PerformRecoveryXLogAction(void);
static void InitControlFile(uint64 sysidentifier, uint32 data_checksum_version);
static void WriteControlFile(void);
static void ReadControlFile(void);
static void UpdateControlFile(void);
static char *str_time(pg_time_t tnow);

static int	get_sync_bit(int method);

static void CopyXLogRecordToWAL(int write_len, bool isLogSwitch,
								XLogRecData *rdata,
								XLogRecPtr StartPos, XLogRecPtr EndPos,
								TimeLineID tli);
static void ReserveXLogInsertLocation(int size, XLogRecPtr *StartPos,
									  XLogRecPtr *EndPos, XLogRecPtr *PrevPtr);
static bool ReserveXLogSwitch(XLogRecPtr *StartPos, XLogRecPtr *EndPos,
							  XLogRecPtr *PrevPtr);
static XLogRecPtr WaitXLogInsertionsToFinish(XLogRecPtr upto);
static char *GetXLogBuffer(XLogRecPtr ptr, TimeLineID tli);
static XLogRecPtr XLogBytePosToRecPtr(uint64 bytepos);
static XLogRecPtr XLogBytePosToEndRecPtr(uint64 bytepos);
static uint64 XLogRecPtrToBytePos(XLogRecPtr ptr);

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
 * 'topxid_included' tells whether the top-transaction id is logged along with
 * current subtransaction. See XLogRecordAssemble().
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
				 int num_fpi,
				 bool topxid_included)
{
	XLogCtlInsert *Insert = &XLogCtl->Insert;
	pg_crc32c	rdata_crc;
	bool		inserted;
	XLogRecord *rechdr = (XLogRecord *) rdata->data;
	uint8		info = rechdr->xl_info & ~XLR_INFO_MASK;
	WalInsertClass class = WALINSERT_NORMAL;
	XLogRecPtr	StartPos;
	XLogRecPtr	EndPos;
	bool		prevDoPageWrites = doPageWrites;
	TimeLineID	insertTLI;

	/* Does this record type require special handling? */
	if (unlikely(rechdr->xl_rmid == RM_XLOG_ID))
	{
		if (info == XLOG_SWITCH)
			class = WALINSERT_SPECIAL_SWITCH;
		else if (info == XLOG_CHECKPOINT_REDO)
			class = WALINSERT_SPECIAL_CHECKPOINT;
	}

	/* we assume that all of the record header is in the first chunk */
	Assert(rdata->len >= SizeOfXLogRecord);

	/* cross-check on whether we should be here or not */
	if (!XLogInsertAllowed())
		elog(ERROR, "cannot make new WAL entries during recovery");

	/*
	 * Given that we're not in recovery, InsertTimeLineID is set and can't
	 * change, so we can read it without a lock.
	 */
	insertTLI = XLogCtl->InsertTimeLineID;

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
	 * page is not initialized yet, you have to go through AdvanceXLInsertBuffer,
	 * which will ensure it is initialized. But the WAL writer tries to do that
	 * ahead of insertions to avoid that from happening in the critical path.
	 *
	 *----------
	 */
	START_CRIT_SECTION();

	if (likely(class == WALINSERT_NORMAL))
	{
		WALInsertLockAcquire();

		/*
		 * Check to see if my copy of RedoRecPtr is out of date. If so, may
		 * have to go back and have the caller recompute everything. This can
		 * only happen just after a checkpoint, so it's better to be slow in
		 * this case and fast otherwise.
		 *
		 * Also check to see if fullPageWrites was just turned on or there's a
		 * running backup (which forces full-page writes); if we weren't
		 * already doing full-page writes then go back and recompute.
		 *
		 * If we aren't doing full-page writes then RedoRecPtr doesn't
		 * actually affect the contents of the XLOG record, so we'll update
		 * our local copy but not force a recomputation.  (If doPageWrites was
		 * just turned off, we could recompute the record without full pages,
		 * but we choose not to bother.)
		 */
		if (RedoRecPtr != Insert->RedoRecPtr)
		{
			Assert(RedoRecPtr < Insert->RedoRecPtr);
			RedoRecPtr = Insert->RedoRecPtr;
		}
		doPageWrites = (Insert->fullPageWrites || Insert->runningBackups > 0);

		if (doPageWrites &&
			(!prevDoPageWrites ||
			 (fpw_lsn != InvalidXLogRecPtr && fpw_lsn <= RedoRecPtr)))
		{
			/*
			 * Oops, some buffer now needs to be backed up that the caller
			 * didn't back up.  Start over.
			 */
			WALInsertLockRelease();
			END_CRIT_SECTION();
			return InvalidXLogRecPtr;
		}

		/*
		 * Reserve space for the record in the WAL. This also sets the xl_prev
		 * pointer.
		 */
		ReserveXLogInsertLocation(rechdr->xl_tot_len, &StartPos, &EndPos,
								  &rechdr->xl_prev);

		/* Normal records are always inserted. */
		inserted = true;
	}
	else if (class == WALINSERT_SPECIAL_SWITCH)
	{
		/*
		 * In order to insert an XLOG_SWITCH record, we need to hold all of
		 * the WAL insertion locks, not just one, so that no one else can
		 * begin inserting a record until we've figured out how much space
		 * remains in the current WAL segment and claimed all of it.
		 *
		 * Nonetheless, this case is simpler than the normal cases handled
		 * below, which must check for changes in doPageWrites and RedoRecPtr.
		 * Those checks are only needed for records that can contain buffer
		 * references, and an XLOG_SWITCH record never does.
		 */
		Assert(fpw_lsn == InvalidXLogRecPtr);
		WALInsertLockAcquireExclusive();
		inserted = ReserveXLogSwitch(&StartPos, &EndPos, &rechdr->xl_prev);
	}
	else
	{
		Assert(class == WALINSERT_SPECIAL_CHECKPOINT);

		/*
		 * We need to update both the local and shared copies of RedoRecPtr,
		 * which means that we need to hold all the WAL insertion locks.
		 * However, there can't be any buffer references, so as above, we need
		 * not check RedoRecPtr before inserting the record; we just need to
		 * update it afterwards.
		 */
		Assert(fpw_lsn == InvalidXLogRecPtr);
		WALInsertLockAcquireExclusive();
		ReserveXLogInsertLocation(rechdr->xl_tot_len, &StartPos, &EndPos,
								  &rechdr->xl_prev);
		RedoRecPtr = Insert->RedoRecPtr = StartPos;
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
		CopyXLogRecordToWAL(rechdr->xl_tot_len,
							class == WALINSERT_SPECIAL_SWITCH, rdata,
							StartPos, EndPos, insertTLI);

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

	END_CRIT_SECTION();

	MarkCurrentTransactionIdLoggedIfAny();

	/*
	 * Mark top transaction id is logged (if needed) so that we should not try
	 * to log it again with the next WAL record in the current subtransaction.
	 */
	if (topxid_included)
		MarkSubxactTopXidLogged();

	/*
	 * Update shared LogwrtRqst.Write, if we crossed page boundary.
	 */
	if (StartPos / XLOG_BLCKSZ != EndPos / XLOG_BLCKSZ)
	{
		SpinLockAcquire(&XLogCtl->info_lck);
		/* advance global request to include new block(s) */
		if (XLogCtl->LogwrtRqst.Write < EndPos)
			XLogCtl->LogwrtRqst.Write = EndPos;
		SpinLockRelease(&XLogCtl->info_lck);
		RefreshXLogWriteResult(LogwrtResult);
	}

	/*
	 * If this was an XLOG_SWITCH record, flush the record and the empty
	 * padding space that fills the rest of the segment, and perform
	 * end-of-segment actions (eg, notifying archiver).
	 */
	if (class == WALINSERT_SPECIAL_SWITCH)
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
		XLogRecord *record;
		DecodedXLogRecord *decoded;
		StringInfoData buf;
		StringInfoData recordBuf;
		char	   *errormsg = NULL;
		MemoryContext oldCxt;

		oldCxt = MemoryContextSwitchTo(walDebugCxt);

		initStringInfo(&buf);
		appendStringInfo(&buf, "INSERT @ %X/%08X: ", LSN_FORMAT_ARGS(EndPos));

		/*
		 * We have to piece together the WAL record data from the XLogRecData
		 * entries, so that we can pass it to the rm_desc function as one
		 * contiguous chunk.
		 */
		initStringInfo(&recordBuf);
		for (; rdata != NULL; rdata = rdata->next)
			appendBinaryStringInfo(&recordBuf, rdata->data, rdata->len);

		/* We also need temporary space to decode the record. */
		record = (XLogRecord *) recordBuf.data;
		decoded = (DecodedXLogRecord *)
			palloc(DecodeXLogRecordRequiredSpace(record->xl_tot_len));

		if (!debug_reader)
			debug_reader = XLogReaderAllocate(wal_segment_size, NULL,
											  XL_ROUTINE(.page_read = NULL,
														 .segment_open = NULL,
														 .segment_close = NULL),
											  NULL);
		if (!debug_reader)
		{
			appendStringInfoString(&buf, "error decoding record: out of memory while allocating a WAL reading processor");
		}
		else if (!DecodeXLogRecord(debug_reader,
								   decoded,
								   record,
								   EndPos,
								   &errormsg))
		{
			appendStringInfo(&buf, "error decoding record: %s",
							 errormsg ? errormsg : "no error message");
		}
		else
		{
			appendStringInfoString(&buf, " - ");

			debug_reader->record = decoded;
			xlog_outdesc(&buf, debug_reader);
			debug_reader->record = NULL;
		}
		elog(LOG, "%s", buf.data);

		pfree(decoded);
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
 *
 * NB: Testing shows that XLogInsertRecord runs faster if this code is inlined;
 * however, because there are two call sites, the compiler is reluctant to
 * inline. We use pg_attribute_always_inline here to try to convince it.
 */
static pg_attribute_always_inline void
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
 * Subroutine of XLogInsertRecord.  Copies a WAL record to an already-reserved
 * area in the WAL.
 */
static void
CopyXLogRecordToWAL(int write_len, bool isLogSwitch, XLogRecData *rdata,
					XLogRecPtr StartPos, XLogRecPtr EndPos, TimeLineID tli)
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
	currpos = GetXLogBuffer(CurrPos, tli);
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
		const char *rdata_data = rdata->data;
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
			currpos = GetXLogBuffer(CurrPos, tli);
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
			currpos = GetXLogBuffer(CurrPos, tli);
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
		ereport(PANIC,
				errcode(ERRCODE_DATA_CORRUPTED),
				errmsg_internal("space reserved for WAL record does not match what was written"));
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
		lockToTry = MyProcNumber % NUM_XLOGINSERT_LOCKS;
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
	XLogRecPtr	inserted;
	XLogRecPtr	reservedUpto;
	XLogRecPtr	finishedUpto;
	XLogCtlInsert *Insert = &XLogCtl->Insert;
	int			i;

	if (MyProc == NULL)
		elog(PANIC, "cannot wait without a PGPROC structure");

	/*
	 * Check if there's any work to do.  Use a barrier to ensure we get the
	 * freshest value.
	 */
	inserted = pg_atomic_read_membarrier_u64(&XLogCtl->logInsertResult);
	if (upto <= inserted)
		return inserted;

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
		ereport(LOG,
				errmsg("request to flush past end of generated WAL; request %X/%08X, current position %X/%08X",
					   LSN_FORMAT_ARGS(upto), LSN_FORMAT_ARGS(reservedUpto)));
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
			 *
			 * In this loop we are only waiting for insertions that started
			 * before WaitXLogInsertionsToFinish was called.  The lack of
			 * memory barriers in the loop means that we might see locks as
			 * "unused" that have since become used.  This is fine because
			 * they only can be used for later insertions that we would not
			 * want to wait on anyway.  Not taking a lock to acquire the
			 * current insertingAt value means that we might see older
			 * insertingAt values.  This is also fine, because if we read a
			 * value too old, we will add ourselves to the wait queue, which
			 * contains atomic operations.
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

	/*
	 * Advance the limit we know to have been inserted and return the freshest
	 * value we know of, which might be beyond what we requested if somebody
	 * is concurrently doing this with an 'upto' pointer ahead of us.
	 */
	finishedUpto = pg_atomic_monotonic_advance_u64(&XLogCtl->logInsertResult,
												   finishedUpto);

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
GetXLogBuffer(XLogRecPtr ptr, TimeLineID tli)
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
	 * We don't hold a lock while we read the value. If someone is just about
	 * to initialize or has just initialized the page, it's possible that we
	 * get InvalidXLogRecPtr. That's ok, we'll grab the mapping lock (in
	 * AdvanceXLInsertBuffer) and retry if we see anything other than the page
	 * we're looking for.
	 */
	expectedEndPtr = ptr;
	expectedEndPtr += XLOG_BLCKSZ - ptr % XLOG_BLCKSZ;

	endptr = pg_atomic_read_u64(&XLogCtl->xlblocks[idx]);
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

		AdvanceXLInsertBuffer(ptr, tli, false);
		endptr = pg_atomic_read_u64(&XLogCtl->xlblocks[idx]);

		if (expectedEndPtr != endptr)
			elog(PANIC, "could not find WAL buffer for %X/%08X",
				 LSN_FORMAT_ARGS(ptr));
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
 * Read WAL data directly from WAL buffers, if available. Returns the number
 * of bytes read successfully.
 *
 * Fewer than 'count' bytes may be read if some of the requested WAL data has
 * already been evicted.
 *
 * No locks are taken.
 *
 * Caller should ensure that it reads no further than LogwrtResult.Write
 * (which should have been updated by the caller when determining how far to
 * read). The 'tli' argument is only used as a convenient safety check so that
 * callers do not read from WAL buffers on a historical timeline.
 */
Size
WALReadFromBuffers(char *dstbuf, XLogRecPtr startptr, Size count,
				   TimeLineID tli)
{
	char	   *pdst = dstbuf;
	XLogRecPtr	recptr = startptr;
	XLogRecPtr	inserted;
	Size		nbytes = count;

	if (RecoveryInProgress() || tli != GetWALInsertionTimeLine())
		return 0;

	Assert(!XLogRecPtrIsInvalid(startptr));

	/*
	 * Caller should ensure that the requested data has been inserted into WAL
	 * buffers before we try to read it.
	 */
	inserted = pg_atomic_read_u64(&XLogCtl->logInsertResult);
	if (startptr + count > inserted)
		ereport(ERROR,
				errmsg("cannot read past end of generated WAL: requested %X/%08X, current position %X/%08X",
					   LSN_FORMAT_ARGS(startptr + count),
					   LSN_FORMAT_ARGS(inserted)));

	/*
	 * Loop through the buffers without a lock. For each buffer, atomically
	 * read and verify the end pointer, then copy the data out, and finally
	 * re-read and re-verify the end pointer.
	 *
	 * Once a page is evicted, it never returns to the WAL buffers, so if the
	 * end pointer matches the expected end pointer before and after we copy
	 * the data, then the right page must have been present during the data
	 * copy. Read barriers are necessary to ensure that the data copy actually
	 * happens between the two verification steps.
	 *
	 * If either verification fails, we simply terminate the loop and return
	 * with the data that had been already copied out successfully.
	 */
	while (nbytes > 0)
	{
		uint32		offset = recptr % XLOG_BLCKSZ;
		int			idx = XLogRecPtrToBufIdx(recptr);
		XLogRecPtr	expectedEndPtr;
		XLogRecPtr	endptr;
		const char *page;
		const char *psrc;
		Size		npagebytes;

		/*
		 * Calculate the end pointer we expect in the xlblocks array if the
		 * correct page is present.
		 */
		expectedEndPtr = recptr + (XLOG_BLCKSZ - offset);

		/*
		 * First verification step: check that the correct page is present in
		 * the WAL buffers.
		 */
		endptr = pg_atomic_read_u64(&XLogCtl->xlblocks[idx]);
		if (expectedEndPtr != endptr)
			break;

		/*
		 * The correct page is present (or was at the time the endptr was
		 * read; must re-verify later). Calculate pointer to source data and
		 * determine how much data to read from this page.
		 */
		page = XLogCtl->pages + idx * (Size) XLOG_BLCKSZ;
		psrc = page + offset;
		npagebytes = Min(nbytes, XLOG_BLCKSZ - offset);

		/*
		 * Ensure that the data copy and the first verification step are not
		 * reordered.
		 */
		pg_read_barrier();

		/* data copy */
		memcpy(pdst, psrc, npagebytes);

		/*
		 * Ensure that the data copy and the second verification step are not
		 * reordered.
		 */
		pg_read_barrier();

		/*
		 * Second verification step: check that the page we read from wasn't
		 * evicted while we were copying the data.
		 */
		endptr = pg_atomic_read_u64(&XLogCtl->xlblocks[idx]);
		if (expectedEndPtr != endptr)
			break;

		pdst += npagebytes;
		recptr += npagebytes;
		nbytes -= npagebytes;
	}

	Assert(pdst - dstbuf <= count);

	return pdst - dstbuf;
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
AdvanceXLInsertBuffer(XLogRecPtr upto, TimeLineID tli, bool opportunistic)
{
	XLogCtlInsert *Insert = &XLogCtl->Insert;
	int			nextidx;
	XLogRecPtr	OldPageRqstPtr;
	XLogwrtRqst WriteRqst;
	XLogRecPtr	NewPageEndPtr = InvalidXLogRecPtr;
	XLogRecPtr	NewPageBeginPtr;
	XLogPageHeader NewPage;
	XLogRecPtr	ReservedPtr;
	int			npages pg_attribute_unused() = 0;

	/*
	 * We must run the loop below inside the critical section as we expect
	 * XLogCtl->InitializedUpTo to eventually keep up.  The most of callers
	 * already run inside the critical section. Except for WAL writer, which
	 * passed 'opportunistic == true', and therefore we don't perform
	 * operations that could error out.
	 *
	 * Start an explicit critical section anyway though.
	 */
	Assert(CritSectionCount > 0 || opportunistic);
	START_CRIT_SECTION();

	/*--
	 * Loop till we get all the pages in WAL buffer before 'upto' reserved for
	 * initialization.  Multiple process can initialize different buffers with
	 * this loop in parallel as following.
	 *
	 * 1. Reserve page for initialization using XLogCtl->InitializeReserved.
	 * 2. Initialize the reserved page.
	 * 3. Attempt to advance XLogCtl->InitializedUpTo,
	 */
	ReservedPtr = pg_atomic_read_u64(&XLogCtl->InitializeReserved);
	while (upto >= ReservedPtr || opportunistic)
	{
		Assert(ReservedPtr % XLOG_BLCKSZ == 0);

		/*
		 * Get ending-offset of the buffer page we need to replace.
		 *
		 * We don't lookup into xlblocks, but rather calculate position we
		 * must wait to be written. If it was written, xlblocks will have this
		 * position (or uninitialized)
		 */
		if (ReservedPtr + XLOG_BLCKSZ > XLogCtl->InitializedFrom + XLOG_BLCKSZ * XLOGbuffers)
			OldPageRqstPtr = ReservedPtr + XLOG_BLCKSZ - (XLogRecPtr) XLOG_BLCKSZ * XLOGbuffers;
		else
			OldPageRqstPtr = InvalidXLogRecPtr;

		if (LogwrtResult.Write < OldPageRqstPtr && opportunistic)
		{
			/*
			 * If we just want to pre-initialize as much as we can without
			 * flushing, give up now.
			 */
			upto = ReservedPtr - 1;
			break;
		}

		/*
		 * Attempt to reserve the page for initialization.  Failure means that
		 * this page got reserved by another process.
		 */
		if (!pg_atomic_compare_exchange_u64(&XLogCtl->InitializeReserved,
											&ReservedPtr,
											ReservedPtr + XLOG_BLCKSZ))
			continue;

		/*
		 * Wait till page gets correctly initialized up to OldPageRqstPtr.
		 */
		nextidx = XLogRecPtrToBufIdx(ReservedPtr);
		while (pg_atomic_read_u64(&XLogCtl->InitializedUpTo) < OldPageRqstPtr)
			ConditionVariableSleep(&XLogCtl->InitializedUpToCondVar, WAIT_EVENT_WAL_BUFFER_INIT);
		ConditionVariableCancelSleep();
		Assert(pg_atomic_read_u64(&XLogCtl->xlblocks[nextidx]) == OldPageRqstPtr);

		/* Fall through if it's already written out. */
		if (LogwrtResult.Write < OldPageRqstPtr)
		{
			/* Nope, got work to do. */

			/* Advance shared memory write request position */
			SpinLockAcquire(&XLogCtl->info_lck);
			if (XLogCtl->LogwrtRqst.Write < OldPageRqstPtr)
				XLogCtl->LogwrtRqst.Write = OldPageRqstPtr;
			SpinLockRelease(&XLogCtl->info_lck);

			/*
			 * Acquire an up-to-date LogwrtResult value and see if we still
			 * need to write it or if someone else already did.
			 */
			RefreshXLogWriteResult(LogwrtResult);
			if (LogwrtResult.Write < OldPageRqstPtr)
			{
				WaitXLogInsertionsToFinish(OldPageRqstPtr);

				LWLockAcquire(WALWriteLock, LW_EXCLUSIVE);

				RefreshXLogWriteResult(LogwrtResult);
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
					XLogWrite(WriteRqst, tli, false);
					LWLockRelease(WALWriteLock);
					pgWalUsage.wal_buffers_full++;
					TRACE_POSTGRESQL_WAL_BUFFER_WRITE_DIRTY_DONE();
				}
			}
		}

		/*
		 * Now the next buffer slot is free and we can set it up to be the
		 * next output page.
		 */
		NewPageBeginPtr = ReservedPtr;
		NewPageEndPtr = NewPageBeginPtr + XLOG_BLCKSZ;

		NewPage = (XLogPageHeader) (XLogCtl->pages + nextidx * (Size) XLOG_BLCKSZ);

		/*
		 * Mark the xlblock with InvalidXLogRecPtr and issue a write barrier
		 * before initializing. Otherwise, the old page may be partially
		 * zeroed but look valid.
		 */
		pg_atomic_write_u64(&XLogCtl->xlblocks[nextidx], InvalidXLogRecPtr);
		pg_write_barrier();

		/*
		 * Be sure to re-zero the buffer so that bytes beyond what we've
		 * written will look like zeroes and not valid XLOG records...
		 */
		MemSet(NewPage, 0, XLOG_BLCKSZ);

		/*
		 * Fill the new page's header
		 */
		NewPage->xlp_magic = XLOG_PAGE_MAGIC;

		/* NewPage->xlp_info = 0; */	/* done by memset */
		NewPage->xlp_tli = tli;
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
		if (Insert->runningBackups == 0)
			NewPage->xlp_info |= XLP_BKP_REMOVABLE;

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

		/*-----
		 * Update the value of XLogCtl->xlblocks[nextidx] and try to advance
		 * XLogCtl->InitializedUpTo in a lock-less manner.
		 *
		 * First, let's provide a formal proof of the algorithm.  Let it be 'n'
		 * process with the following variables in shared memory:
		 *	f - an array of 'n' boolean flags,
		 *	v - atomic integer variable.
		 *
		 * Also, let
		 *	i - a number of a process,
		 *	j - local integer variable,
		 * CAS(var, oldval, newval) - compare-and-swap atomic operation
		 *							  returning true on success,
		 * write_barrier()/read_barrier() - memory barriers.
		 *
		 * The pseudocode for each process is the following.
		 *
		 *	j := i
		 *	f[i] := true
		 *	write_barrier()
		 *	while CAS(v, j, j + 1):
		 *		j := j + 1
		 *		read_barrier()
		 *		if not f[j]:
		 *			break
		 *
		 * Let's prove that v eventually reaches the value of n.
		 * 1. Prove by contradiction.  Assume v doesn't reach n and stucks
		 *	  on k, where k < n.
		 * 2. Process k attempts CAS(v, k, k + 1).  1). If, as we assumed, v
		 *	  gets stuck at k, then this CAS operation must fail.  Therefore,
		 *    v < k when process k attempts CAS(v, k, k + 1).
		 * 3. If, as we assumed, v gets stuck at k, then the value k of v
		 *	  must be achieved by some process m, where m < k.  The process
		 *	  m must observe f[k] == false.  Otherwise, it will later attempt
		 *	  CAS(v, k, k + 1) with success.
		 * 4. Therefore, corresponding read_barrier() (while j == k) on
		 *	  process m reached before write_barrier() of process k.  But then
		 *	  process k attempts CAS(v, k, k + 1) after process m successfully
		 *	  incremented v to k, and that CAS operation must succeed.
		 *	  That leads to a contradiction.  So, there is no such k (k < n)
		 *    where v gets stuck.  Q.E.D.
		 *
		 * To apply this proof to the code below, we assume
		 * XLogCtl->InitializedUpTo will play the role of v with XLOG_BLCKSZ
		 * granularity.  We also assume setting XLogCtl->xlblocks[nextidx] to
		 * NewPageEndPtr to play the role of setting f[i] to true.  Also, note
		 * that processes can't concurrently map different xlog locations to
		 * the same nextidx because we previously requested that
		 * XLogCtl->InitializedUpTo >= OldPageRqstPtr.  So, a xlog buffer can
		 * be taken for initialization only once the previous initialization
		 * takes effect on XLogCtl->InitializedUpTo.
		 */

		pg_atomic_write_u64(&XLogCtl->xlblocks[nextidx], NewPageEndPtr);

		pg_write_barrier();

		while (pg_atomic_compare_exchange_u64(&XLogCtl->InitializedUpTo, &NewPageBeginPtr, NewPageEndPtr))
		{
			NewPageBeginPtr = NewPageEndPtr;
			NewPageEndPtr = NewPageBeginPtr + XLOG_BLCKSZ;
			nextidx = XLogRecPtrToBufIdx(NewPageBeginPtr);

			pg_read_barrier();

			if (pg_atomic_read_u64(&XLogCtl->xlblocks[nextidx]) != NewPageEndPtr)
			{
				/*
				 * Page at nextidx wasn't initialized yet, so we can't move
				 * InitializedUpto further. It will be moved by backend which
				 * will initialize nextidx.
				 */
				ConditionVariableBroadcast(&XLogCtl->InitializedUpToCondVar);
				break;
			}
		}

		npages++;
	}

	END_CRIT_SECTION();

	/*
	 * All the pages in WAL buffer before 'upto' were reserved for
	 * initialization.  However, some pages might be reserved by concurrent
	 * processes.  Wait till they finish initialization.
	 */
	while (upto >= pg_atomic_read_u64(&XLogCtl->InitializedUpTo))
		ConditionVariableSleep(&XLogCtl->InitializedUpToCondVar, WAIT_EVENT_WAL_BUFFER_INIT);
	ConditionVariableCancelSleep();

	pg_read_barrier();

#ifdef WAL_DEBUG
	if (XLOG_DEBUG && npages > 0)
	{
		elog(DEBUG1, "initialized %d pages, up to %X/%08X",
			 npages, LSN_FORMAT_ARGS(NewPageEndPtr));
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
	 *    only did this on the primary anyway, not on standby. Keeping just
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

bool
check_wal_segment_size(int *newval, void **extra, GucSource source)
{
	if (!IsValidWalSegSize(*newval))
	{
		GUC_check_errdetail("The WAL segment size must be a power of two between 1 MB and 1 GB.");
		return false;
	}

	return true;
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
bool
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
XLogWrite(XLogwrtRqst WriteRqst, TimeLineID tli, bool flexible)
{
	bool		ispartialpage;
	bool		last_iteration;
	bool		finishing_seg;
	int			curridx;
	int			npages;
	int			startidx;
	uint32		startoffset;

	/* We should always be inside a critical section here */
	Assert(CritSectionCount > 0);

	/*
	 * Update local LogwrtResult (caller probably did this already, but...)
	 */
	RefreshXLogWriteResult(LogwrtResult);

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
		XLogRecPtr	EndPtr = pg_atomic_read_u64(&XLogCtl->xlblocks[curridx]);

		if (LogwrtResult.Write >= EndPtr)
			elog(PANIC, "xlog write request %X/%08X is past end of log %X/%08X",
				 LSN_FORMAT_ARGS(LogwrtResult.Write),
				 LSN_FORMAT_ARGS(EndPtr));

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
			openLogTLI = tli;

			/* create/use new log file */
			openLogFile = XLogFileInit(openLogSegNo, tli);
			ReserveExternalFD();
		}

		/* Make sure we have the current logfile open */
		if (openLogFile < 0)
		{
			XLByteToPrevSeg(LogwrtResult.Write, openLogSegNo,
							wal_segment_size);
			openLogTLI = tli;
			openLogFile = XLogFileOpen(openLogSegNo, tli);
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
			ssize_t		written;
			instr_time	start;

			/* OK to write the page(s) */
			from = XLogCtl->pages + startidx * (Size) XLOG_BLCKSZ;
			nbytes = npages * (Size) XLOG_BLCKSZ;
			nleft = nbytes;
			do
			{
				errno = 0;

				/*
				 * Measure I/O timing to write WAL data, for pg_stat_io.
				 */
				start = pgstat_prepare_io_time(track_wal_io_timing);

				pgstat_report_wait_start(WAIT_EVENT_WAL_WRITE);
				written = pg_pwrite(openLogFile, from, nleft, startoffset);
				pgstat_report_wait_end();

				pgstat_count_io_op_time(IOOBJECT_WAL, IOCONTEXT_NORMAL,
										IOOP_WRITE, start, 1, written);

				if (written <= 0)
				{
					char		xlogfname[MAXFNAMELEN];
					int			save_errno;

					if (errno == EINTR)
						continue;

					save_errno = errno;
					XLogFileName(xlogfname, tli, openLogSegNo,
								 wal_segment_size);
					errno = save_errno;
					ereport(PANIC,
							(errcode_for_file_access(),
							 errmsg("could not write to log file \"%s\" at offset %u, length %zu: %m",
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
				issue_xlog_fsync(openLogFile, openLogSegNo, tli);

				/* signal that we need to wakeup walsenders later */
				WalSndWakeupRequest();

				LogwrtResult.Flush = LogwrtResult.Write;	/* end of page */

				if (XLogArchivingActive())
					XLogArchiveNotifySeg(openLogSegNo, tli);

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
		if (wal_sync_method != WAL_SYNC_METHOD_OPEN &&
			wal_sync_method != WAL_SYNC_METHOD_OPEN_DSYNC)
		{
			if (openLogFile >= 0 &&
				!XLByteInPrevSeg(LogwrtResult.Write, openLogSegNo,
								 wal_segment_size))
				XLogFileClose();
			if (openLogFile < 0)
			{
				XLByteToPrevSeg(LogwrtResult.Write, openLogSegNo,
								wal_segment_size);
				openLogTLI = tli;
				openLogFile = XLogFileOpen(openLogSegNo, tli);
				ReserveExternalFD();
			}

			issue_xlog_fsync(openLogFile, openLogSegNo, tli);
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
	SpinLockAcquire(&XLogCtl->info_lck);
	if (XLogCtl->LogwrtRqst.Write < LogwrtResult.Write)
		XLogCtl->LogwrtRqst.Write = LogwrtResult.Write;
	if (XLogCtl->LogwrtRqst.Flush < LogwrtResult.Flush)
		XLogCtl->LogwrtRqst.Flush = LogwrtResult.Flush;
	SpinLockRelease(&XLogCtl->info_lck);

	/*
	 * We write Write first, bar, then Flush.  When reading, the opposite must
	 * be done (with a matching barrier in between), so that we always see a
	 * Flush value that trails behind the Write value seen.
	 */
	pg_atomic_write_u64(&XLogCtl->logWriteResult, LogwrtResult.Write);
	pg_write_barrier();
	pg_atomic_write_u64(&XLogCtl->logFlushResult, LogwrtResult.Flush);

#ifdef USE_ASSERT_CHECKING
	{
		XLogRecPtr	Flush;
		XLogRecPtr	Write;
		XLogRecPtr	Insert;

		Flush = pg_atomic_read_u64(&XLogCtl->logFlushResult);
		pg_read_barrier();
		Write = pg_atomic_read_u64(&XLogCtl->logWriteResult);
		pg_read_barrier();
		Insert = pg_atomic_read_u64(&XLogCtl->logInsertResult);

		/* WAL written to disk is always ahead of WAL flushed */
		Assert(Write >= Flush);

		/* WAL inserted to buffers is always ahead of WAL written */
		Assert(Insert >= Write);
	}
#endif
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
	bool		wakeup = false;
	XLogRecPtr	prevAsyncXactLSN;

	SpinLockAcquire(&XLogCtl->info_lck);
	sleeping = XLogCtl->WalWriterSleeping;
	prevAsyncXactLSN = XLogCtl->asyncXactLSN;
	if (XLogCtl->asyncXactLSN < asyncXactLSN)
		XLogCtl->asyncXactLSN = asyncXactLSN;
	SpinLockRelease(&XLogCtl->info_lck);

	/*
	 * If somebody else already called this function with a more aggressive
	 * LSN, they will have done what we needed (and perhaps more).
	 */
	if (asyncXactLSN <= prevAsyncXactLSN)
		return;

	/*
	 * If the WALWriter is sleeping, kick it to make it come out of low-power
	 * mode, so that this async commit will reach disk within the expected
	 * amount of time.  Otherwise, determine whether it has enough WAL
	 * available to flush, the same way that XLogBackgroundFlush() does.
	 */
	if (sleeping)
		wakeup = true;
	else
	{
		int			flushblocks;

		RefreshXLogWriteResult(LogwrtResult);

		flushblocks =
			WriteRqstPtr / XLOG_BLCKSZ - LogwrtResult.Flush / XLOG_BLCKSZ;

		if (WalWriterFlushAfter == 0 || flushblocks >= WalWriterFlushAfter)
			wakeup = true;
	}

	if (wakeup)
	{
		volatile PROC_HDR *procglobal = ProcGlobal;
		ProcNumber	walwriterProc = procglobal->walwriterProc;

		if (walwriterProc != INVALID_PROC_NUMBER)
			SetLatch(&GetPGProcByNumber(walwriterProc)->procLatch);
	}
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
	if (!updateMinRecoveryPoint || (!force && lsn <= LocalMinRecoveryPoint))
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
	if (XLogRecPtrIsInvalid(LocalMinRecoveryPoint) && InRecovery)
	{
		updateMinRecoveryPoint = false;
		return;
	}

	LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);

	/* update local copy */
	LocalMinRecoveryPoint = ControlFile->minRecoveryPoint;
	LocalMinRecoveryPointTLI = ControlFile->minRecoveryPointTLI;

	if (XLogRecPtrIsInvalid(LocalMinRecoveryPoint))
		updateMinRecoveryPoint = false;
	else if (force || LocalMinRecoveryPoint < lsn)
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
		newMinRecoveryPoint = GetCurrentReplayRecPtr(&newMinRecoveryPointTLI);
		if (!force && newMinRecoveryPoint < lsn)
			elog(WARNING,
				 "xlog min recovery request %X/%08X is past current point %X/%08X",
				 LSN_FORMAT_ARGS(lsn), LSN_FORMAT_ARGS(newMinRecoveryPoint));

		/* update control file */
		if (ControlFile->minRecoveryPoint < newMinRecoveryPoint)
		{
			ControlFile->minRecoveryPoint = newMinRecoveryPoint;
			ControlFile->minRecoveryPointTLI = newMinRecoveryPointTLI;
			UpdateControlFile();
			LocalMinRecoveryPoint = newMinRecoveryPoint;
			LocalMinRecoveryPointTLI = newMinRecoveryPointTLI;

			ereport(DEBUG2,
					errmsg_internal("updated min recovery point to %X/%08X on timeline %u",
									LSN_FORMAT_ARGS(newMinRecoveryPoint),
									newMinRecoveryPointTLI));
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
	TimeLineID	insertTLI = XLogCtl->InsertTimeLineID;

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
		elog(LOG, "xlog flush request %X/%08X; write %X/%08X; flush %X/%08X",
			 LSN_FORMAT_ARGS(record),
			 LSN_FORMAT_ARGS(LogwrtResult.Write),
			 LSN_FORMAT_ARGS(LogwrtResult.Flush));
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

		/* done already? */
		RefreshXLogWriteResult(LogwrtResult);
		if (record <= LogwrtResult.Flush)
			break;

		/*
		 * Before actually performing the write, wait for all in-flight
		 * insertions to the pages we're about to write to finish.
		 */
		SpinLockAcquire(&XLogCtl->info_lck);
		if (WriteRqstPtr < XLogCtl->LogwrtRqst.Write)
			WriteRqstPtr = XLogCtl->LogwrtRqst.Write;
		SpinLockRelease(&XLogCtl->info_lck);
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
		RefreshXLogWriteResult(LogwrtResult);
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

		XLogWrite(WriteRqst, insertTLI, false);

		LWLockRelease(WALWriteLock);
		/* done */
		break;
	}

	END_CRIT_SECTION();

	/* wake up walsenders now that we've released heavily contended locks */
	WalSndWakeupProcessRequests(true, !RecoveryInProgress());

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
			 "xlog flush request %X/%08X is not satisfied --- flushed only to %X/%08X",
			 LSN_FORMAT_ARGS(record),
			 LSN_FORMAT_ARGS(LogwrtResult.Flush));
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
	int			flushblocks;
	TimeLineID	insertTLI;

	/* XLOG doesn't need flushing during recovery */
	if (RecoveryInProgress())
		return false;

	/*
	 * Since we're not in recovery, InsertTimeLineID is set and can't change,
	 * so we can read it without a lock.
	 */
	insertTLI = XLogCtl->InsertTimeLineID;

	/* read updated LogwrtRqst */
	SpinLockAcquire(&XLogCtl->info_lck);
	WriteRqst = XLogCtl->LogwrtRqst;
	SpinLockRelease(&XLogCtl->info_lck);

	/* back off to last completed page boundary */
	WriteRqst.Write -= WriteRqst.Write % XLOG_BLCKSZ;

	/* if we have already flushed that far, consider async commit records */
	RefreshXLogWriteResult(LogwrtResult);
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
	 *
	 * Note that XLogSetAsyncXactLSN() performs similar calculation based on
	 * wal_writer_flush_after, to decide when to wake us up.  Make sure the
	 * logic is the same in both places if you change this.
	 */
	now = GetCurrentTimestamp();
	flushblocks =
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
	else if (flushblocks >= WalWriterFlushAfter)
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
		elog(LOG, "xlog bg flush request write %X/%08X; flush: %X/%08X, current is write %X/%08X; flush %X/%08X",
			 LSN_FORMAT_ARGS(WriteRqst.Write),
			 LSN_FORMAT_ARGS(WriteRqst.Flush),
			 LSN_FORMAT_ARGS(LogwrtResult.Write),
			 LSN_FORMAT_ARGS(LogwrtResult.Flush));
#endif

	START_CRIT_SECTION();

	/* now wait for any in-progress insertions to finish and get write lock */
	WaitXLogInsertionsToFinish(WriteRqst.Write);
	LWLockAcquire(WALWriteLock, LW_EXCLUSIVE);
	RefreshXLogWriteResult(LogwrtResult);
	if (WriteRqst.Write > LogwrtResult.Write ||
		WriteRqst.Flush > LogwrtResult.Flush)
	{
		XLogWrite(WriteRqst, insertTLI, flexible);
	}
	LWLockRelease(WALWriteLock);

	END_CRIT_SECTION();

	/* wake up walsenders now that we've released heavily contended locks */
	WalSndWakeupProcessRequests(true, !RecoveryInProgress());

	/*
	 * Great, done. To take some work off the critical path, try to initialize
	 * as many of the no-longer-needed WAL buffers for future use as we can.
	 */
	AdvanceXLInsertBuffer(InvalidXLogRecPtr, insertTLI, true);

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
		if (XLogRecPtrIsInvalid(LocalMinRecoveryPoint) && InRecovery)
			updateMinRecoveryPoint = false;

		/* Quick exit if already known to be updated or cannot be updated */
		if (record <= LocalMinRecoveryPoint || !updateMinRecoveryPoint)
			return false;

		/*
		 * Update local copy of minRecoveryPoint. But if the lock is busy,
		 * just return a conservative guess.
		 */
		if (!LWLockConditionalAcquire(ControlFileLock, LW_SHARED))
			return true;
		LocalMinRecoveryPoint = ControlFile->minRecoveryPoint;
		LocalMinRecoveryPointTLI = ControlFile->minRecoveryPointTLI;
		LWLockRelease(ControlFileLock);

		/*
		 * Check minRecoveryPoint for any other process than the startup
		 * process doing crash recovery, which should not update the control
		 * file value if crash recovery is still running.
		 */
		if (XLogRecPtrIsInvalid(LocalMinRecoveryPoint))
			updateMinRecoveryPoint = false;

		/* check again */
		if (record <= LocalMinRecoveryPoint || !updateMinRecoveryPoint)
			return false;
		else
			return true;
	}

	/* Quick exit if already known flushed */
	if (record <= LogwrtResult.Flush)
		return false;

	/* read LogwrtResult and update local state */
	RefreshXLogWriteResult(LogwrtResult);

	/* check again */
	if (record <= LogwrtResult.Flush)
		return false;

	return true;
}

/*
 * Try to make a given XLOG file segment exist.
 *
 * logsegno: identify segment.
 *
 * *added: on return, true if this call raised the number of extant segments.
 *
 * path: on return, this char[MAXPGPATH] has the path to the logsegno file.
 *
 * Returns -1 or FD of opened file.  A -1 here is not an error; a caller
 * wanting an open segment should attempt to open "path", which usually will
 * succeed.  (This is weird, but it's efficient for the callers.)
 */
static int
XLogFileInitInternal(XLogSegNo logsegno, TimeLineID logtli,
					 bool *added, char *path)
{
	char		tmppath[MAXPGPATH];
	XLogSegNo	installed_segno;
	XLogSegNo	max_segno;
	int			fd;
	int			save_errno;
	int			open_flags = O_RDWR | O_CREAT | O_EXCL | PG_BINARY;
	instr_time	io_start;

	Assert(logtli != 0);

	XLogFilePath(path, logtli, logsegno, wal_segment_size);

	/*
	 * Try to use existent file (checkpoint maker may have created it already)
	 */
	*added = false;
	fd = BasicOpenFile(path, O_RDWR | PG_BINARY | O_CLOEXEC |
					   get_sync_bit(wal_sync_method));
	if (fd < 0)
	{
		if (errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", path)));
	}
	else
		return fd;

	/*
	 * Initialize an empty (all zeroes) segment.  NOTE: it is possible that
	 * another process is doing the same thing.  If so, we will end up
	 * pre-creating an extra log segment.  That seems OK, and better than
	 * holding the lock throughout this lengthy process.
	 */
	elog(DEBUG2, "creating and filling new WAL file");

	snprintf(tmppath, MAXPGPATH, XLOGDIR "/xlogtemp.%d", (int) getpid());

	unlink(tmppath);

	if (io_direct_flags & IO_DIRECT_WAL_INIT)
		open_flags |= PG_O_DIRECT;

	/* do not use get_sync_bit() here --- want to fsync only at end of fill */
	fd = BasicOpenFile(tmppath, open_flags);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", tmppath)));

	/* Measure I/O timing when initializing segment */
	io_start = pgstat_prepare_io_time(track_wal_io_timing);

	pgstat_report_wait_start(WAIT_EVENT_WAL_INIT_WRITE);
	save_errno = 0;
	if (wal_init_zero)
	{
		ssize_t		rc;

		/*
		 * Zero-fill the file.  With this setting, we do this the hard way to
		 * ensure that all the file space has really been allocated.  On
		 * platforms that allow "holes" in files, just seeking to the end
		 * doesn't allocate intermediate space.  This way, we know that we
		 * have all the space and (after the fsync below) that all the
		 * indirect blocks are down on disk.  Therefore, fdatasync(2) or
		 * O_DSYNC will be sufficient to sync future writes to the log file.
		 */
		rc = pg_pwrite_zeros(fd, wal_segment_size, 0);

		if (rc < 0)
			save_errno = errno;
	}
	else
	{
		/*
		 * Otherwise, seeking to the end and writing a solitary byte is
		 * enough.
		 */
		errno = 0;
		if (pg_pwrite(fd, "\0", 1, wal_segment_size - 1) != 1)
		{
			/* if write didn't set errno, assume no disk space */
			save_errno = errno ? errno : ENOSPC;
		}
	}
	pgstat_report_wait_end();

	/*
	 * A full segment worth of data is written when using wal_init_zero. One
	 * byte is written when not using it.
	 */
	pgstat_count_io_op_time(IOOBJECT_WAL, IOCONTEXT_INIT, IOOP_WRITE,
							io_start, 1,
							wal_init_zero ? wal_segment_size : 1);

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

	/* Measure I/O timing when flushing segment */
	io_start = pgstat_prepare_io_time(track_wal_io_timing);

	pgstat_report_wait_start(WAIT_EVENT_WAL_INIT_SYNC);
	if (pg_fsync(fd) != 0)
	{
		save_errno = errno;
		close(fd);
		errno = save_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", tmppath)));
	}
	pgstat_report_wait_end();

	pgstat_count_io_op_time(IOOBJECT_WAL, IOCONTEXT_INIT,
							IOOP_FSYNC, io_start, 1, 0);

	if (close(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", tmppath)));

	/*
	 * Now move the segment into place with its final name.  Cope with
	 * possibility that someone else has created the file while we were
	 * filling ours: if so, use ours to pre-create a future log segment.
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
	if (InstallXLogFileSegment(&installed_segno, tmppath, true, max_segno,
							   logtli))
	{
		*added = true;
		elog(DEBUG2, "done creating and filling new WAL file");
	}
	else
	{
		/*
		 * No need for any more future segments, or InstallXLogFileSegment()
		 * failed to rename the file into place. If the rename failed, a
		 * caller opening the file may fail.
		 */
		unlink(tmppath);
		elog(DEBUG2, "abandoned new WAL file");
	}

	return -1;
}

/*
 * Create a new XLOG file segment, or open a pre-existing one.
 *
 * logsegno: identify segment to be created/opened.
 *
 * Returns FD of opened file.
 *
 * Note: errors here are ERROR not PANIC because we might or might not be
 * inside a critical section (eg, during checkpoint there is no reason to
 * take down the system on failure).  They will promote to PANIC if we are
 * in a critical section.
 */
int
XLogFileInit(XLogSegNo logsegno, TimeLineID logtli)
{
	bool		ignore_added;
	char		path[MAXPGPATH];
	int			fd;

	Assert(logtli != 0);

	fd = XLogFileInitInternal(logsegno, logtli, &ignore_added, path);
	if (fd >= 0)
		return fd;

	/* Now open original target segment (might not be file I just made) */
	fd = BasicOpenFile(path, O_RDWR | PG_BINARY | O_CLOEXEC |
					   get_sync_bit(wal_sync_method));
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
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
XLogFileCopy(TimeLineID destTLI, XLogSegNo destsegno,
			 TimeLineID srcTLI, XLogSegNo srcsegno,
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
	if (!InstallXLogFileSegment(&destsegno, tmppath, false, 0, destTLI))
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
 * tli: The timeline on which the new segment should be installed.
 *
 * Returns true if the file was installed successfully.  false indicates that
 * max_segno limit was exceeded, the startup process has disabled this
 * function for now, or an error occurred while renaming the file into place.
 */
static bool
InstallXLogFileSegment(XLogSegNo *segno, char *tmppath,
					   bool find_free, XLogSegNo max_segno, TimeLineID tli)
{
	char		path[MAXPGPATH];
	struct stat stat_buf;

	Assert(tli != 0);

	XLogFilePath(path, tli, *segno, wal_segment_size);

	LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
	if (!XLogCtl->InstallXLogFileSegmentActive)
	{
		LWLockRelease(ControlFileLock);
		return false;
	}

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
				LWLockRelease(ControlFileLock);
				return false;
			}
			(*segno)++;
			XLogFilePath(path, tli, *segno, wal_segment_size);
		}
	}

	Assert(access(path, F_OK) != 0 && errno == ENOENT);
	if (durable_rename(tmppath, path, LOG) != 0)
	{
		LWLockRelease(ControlFileLock);
		/* durable_rename already emitted log message */
		return false;
	}

	LWLockRelease(ControlFileLock);

	return true;
}

/*
 * Open a pre-existing logfile segment for writing.
 */
int
XLogFileOpen(XLogSegNo segno, TimeLineID tli)
{
	char		path[MAXPGPATH];
	int			fd;

	XLogFilePath(path, tli, segno, wal_segment_size);

	fd = BasicOpenFile(path, O_RDWR | PG_BINARY | O_CLOEXEC |
					   get_sync_bit(wal_sync_method));
	if (fd < 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));

	return fd;
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
	if (!XLogIsNeeded() && (io_direct_flags & IO_DIRECT_WAL) == 0)
		(void) posix_fadvise(openLogFile, 0, 0, POSIX_FADV_DONTNEED);
#endif

	if (close(openLogFile) != 0)
	{
		char		xlogfname[MAXFNAMELEN];
		int			save_errno = errno;

		XLogFileName(xlogfname, openLogTLI, openLogSegNo, wal_segment_size);
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
 *
 * XLogFileInitInternal() can ereport(ERROR).  All known causes indicate big
 * trouble; for example, a full filesystem is one cause.  The checkpoint WAL
 * and/or ControlFile updates already completed.  If a RequestCheckpoint()
 * initiated the present checkpoint and an ERROR ends this function, the
 * command that called RequestCheckpoint() fails.  That's not ideal, but it's
 * not worth contorting more functions to use caller-specified elevel values.
 * (With or without RequestCheckpoint(), an ERROR forestalls some inessential
 * reporting and resource reclamation.)
 */
static void
PreallocXlogFiles(XLogRecPtr endptr, TimeLineID tli)
{
	XLogSegNo	_logSegNo;
	int			lf;
	bool		added;
	char		path[MAXPGPATH];
	uint64		offset;

	if (!XLogCtl->InstallXLogFileSegmentActive)
		return;					/* unlocked check says no */

	XLByteToPrevSeg(endptr, _logSegNo, wal_segment_size);
	offset = XLogSegmentOffset(endptr - 1, wal_segment_size);
	if (offset >= (uint32) (0.75 * wal_segment_size))
	{
		_logSegNo++;
		lf = XLogFileInitInternal(_logSegNo, tli, &added, path);
		if (lf >= 0)
			close(lf);
		if (added)
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
 * Return the oldest WAL segment on the given TLI that still exists in
 * XLOGDIR, or 0 if none.
 */
XLogSegNo
XLogGetOldestSegno(TimeLineID tli)
{
	DIR		   *xldir;
	struct dirent *xlde;
	XLogSegNo	oldest_segno = 0;

	xldir = AllocateDir(XLOGDIR);
	while ((xlde = ReadDir(xldir, XLOGDIR)) != NULL)
	{
		TimeLineID	file_tli;
		XLogSegNo	file_segno;

		/* Ignore files that are not XLOG segments. */
		if (!IsXLogFileName(xlde->d_name))
			continue;

		/* Parse filename to get TLI and segno. */
		XLogFromFileName(xlde->d_name, &file_tli, &file_segno,
						 wal_segment_size);

		/* Ignore anything that's not from the TLI of interest. */
		if (tli != file_tli)
			continue;

		/* If it's the oldest so far, update oldest_segno. */
		if (oldest_segno == 0 || file_segno < oldest_segno)
			oldest_segno = file_segno;
	}

	FreeDir(xldir);
	return oldest_segno;
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
 *
 * insertTLI is the current timeline for XLOG insertion. Any recycled
 * segments should be reused for this timeline.
 */
static void
RemoveOldXlogFiles(XLogSegNo segno, XLogRecPtr lastredoptr, XLogRecPtr endptr,
				   TimeLineID insertTLI)
{
	DIR		   *xldir;
	struct dirent *xlde;
	char		lastoff[MAXFNAMELEN];
	XLogSegNo	endlogSegNo;
	XLogSegNo	recycleSegNo;

	/* Initialize info about where to try to recycle to */
	XLByteToSeg(endptr, endlogSegNo, wal_segment_size);
	recycleSegNo = XLOGfileslop(lastredoptr);

	/*
	 * Construct a filename of the last segment to be kept. The timeline ID
	 * doesn't matter, we ignore that in the comparison. (During recovery,
	 * InsertTimeLineID isn't set, so we can't use that.)
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

				RemoveXlogFile(xlde, recycleSegNo, &endlogSegNo, insertTLI);
			}
		}
	}

	FreeDir(xldir);
}

/*
 * Recycle or remove WAL files that are not part of the given timeline's
 * history.
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
void
RemoveNonParentXlogFiles(XLogRecPtr switchpoint, TimeLineID newTLI)
{
	DIR		   *xldir;
	struct dirent *xlde;
	char		switchseg[MAXFNAMELEN];
	XLogSegNo	endLogSegNo;
	XLogSegNo	switchLogSegNo;
	XLogSegNo	recycleSegNo;

	/*
	 * Initialize info about where to begin the work.  This will recycle,
	 * somewhat arbitrarily, 10 future segments.
	 */
	XLByteToPrevSeg(switchpoint, switchLogSegNo, wal_segment_size);
	XLByteToSeg(switchpoint, endLogSegNo, wal_segment_size);
	recycleSegNo = endLogSegNo + 10;

	/*
	 * Construct a filename of the last segment to be kept.
	 */
	XLogFileName(switchseg, newTLI, switchLogSegNo, wal_segment_size);

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
				RemoveXlogFile(xlde, recycleSegNo, &endLogSegNo, newTLI);
		}
	}

	FreeDir(xldir);
}

/*
 * Recycle or remove a log file that's no longer needed.
 *
 * segment_de is the dirent structure of the segment to recycle or remove.
 * recycleSegNo is the segment number to recycle up to.  endlogSegNo is
 * the segment number of the current (or recent) end of WAL.
 *
 * endlogSegNo gets incremented if the segment is recycled so as it is not
 * checked again with future callers of this function.
 *
 * insertTLI is the current timeline for XLOG insertion. Any recycled segments
 * should be used for this timeline.
 */
static void
RemoveXlogFile(const struct dirent *segment_de,
			   XLogSegNo recycleSegNo, XLogSegNo *endlogSegNo,
			   TimeLineID insertTLI)
{
	char		path[MAXPGPATH];
#ifdef WIN32
	char		newpath[MAXPGPATH];
#endif
	const char *segname = segment_de->d_name;

	snprintf(path, MAXPGPATH, XLOGDIR "/%s", segname);

	/*
	 * Before deleting the file, see if it can be recycled as a future log
	 * segment. Only recycle normal files, because we don't want to recycle
	 * symbolic links pointing to a separate archive directory.
	 */
	if (wal_recycle &&
		*endlogSegNo <= recycleSegNo &&
		XLogCtl->InstallXLogFileSegmentActive &&	/* callee rechecks this */
		get_dirent_type(path, segment_de, false, DEBUG2) == PGFILETYPE_REG &&
		InstallXLogFileSegment(endlogSegNo, path,
							   true, recycleSegNo, insertTLI))
	{
		ereport(DEBUG2,
				(errmsg_internal("recycled write-ahead log file \"%s\"",
								 segname)));
		CheckpointStats.ckpt_segs_recycled++;
		/* Needn't recheck that slot on future iterations */
		(*endlogSegNo)++;
	}
	else
	{
		/* No need for any more future segments, or recycling failed ... */
		int			rc;

		ereport(DEBUG2,
				(errmsg_internal("removing write-ahead log file \"%s\"",
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
 * Verify whether pg_wal, pg_wal/archive_status, and pg_wal/summaries exist.
 * If the latter do not exist, recreate them.
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
				(errcode_for_file_access(),
				 errmsg("required WAL directory \"%s\" does not exist",
						XLOGDIR)));

	/* Check for archive_status */
	snprintf(path, MAXPGPATH, XLOGDIR "/archive_status");
	if (stat(path, &stat_buf) == 0)
	{
		/* Check for weird cases where it exists but isn't a directory */
		if (!S_ISDIR(stat_buf.st_mode))
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("required WAL directory \"%s\" does not exist",
							path)));
	}
	else
	{
		ereport(LOG,
				(errmsg("creating missing WAL directory \"%s\"", path)));
		if (MakePGDirectory(path) < 0)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not create missing directory \"%s\": %m",
							path)));
	}

	/* Check for summaries */
	snprintf(path, MAXPGPATH, XLOGDIR "/summaries");
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
InitControlFile(uint64 sysidentifier, uint32 data_checksum_version)
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
	ControlFile->data_checksum_version = data_checksum_version;
}

static void
WriteControlFile(void)
{
	int			fd;
	char		buffer[PG_CONTROL_FILE_SIZE];	/* need not be aligned */

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

	/*
	 * Initialize the default 'char' signedness.
	 *
	 * The signedness of the char type is implementation-defined. For instance
	 * on x86 architecture CPUs, the char data type is typically treated as
	 * signed by default, whereas on aarch architecture CPUs, it is typically
	 * treated as unsigned by default. In v17 or earlier, we accidentally let
	 * C implementation signedness affect persistent data. This led to
	 * inconsistent results when comparing char data across different
	 * platforms.
	 *
	 * This flag can be used as a hint to ensure consistent behavior for
	 * pre-v18 data files that store data sorted by the 'char' type on disk,
	 * especially in cross-platform replication scenarios.
	 *
	 * Newly created database clusters unconditionally set the default char
	 * signedness to true. pg_upgrade changes this flag for clusters that were
	 * initialized on signedness=false platforms. As a result,
	 * signedness=false setting will become rare over time. If we had known
	 * about this problem during the last development cycle that forced initdb
	 * (v8.3), we would have made all clusters signed or all clusters
	 * unsigned. Making pg_upgrade the only source of signedness=false will
	 * cause the population of database clusters to converge toward that
	 * retrospective ideal.
	 */
	ControlFile->default_char_signedness = true;

	/* Contents are protected with a CRC */
	INIT_CRC32C(ControlFile->crc);
	COMP_CRC32C(ControlFile->crc,
				ControlFile,
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
	char		wal_segsz_str[20];
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
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with PG_CONTROL_VERSION %d (0x%08x),"
						   " but the server was compiled with PG_CONTROL_VERSION %d (0x%08x).",
						   ControlFile->pg_control_version, ControlFile->pg_control_version,
						   PG_CONTROL_VERSION, PG_CONTROL_VERSION),
				 errhint("This could be a problem of mismatched byte ordering.  It looks like you need to initdb.")));

	if (ControlFile->pg_control_version != PG_CONTROL_VERSION)
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with PG_CONTROL_VERSION %d,"
						   " but the server was compiled with PG_CONTROL_VERSION %d.",
						   ControlFile->pg_control_version, PG_CONTROL_VERSION),
				 errhint("It looks like you need to initdb.")));

	/* Now check the CRC. */
	INIT_CRC32C(crc);
	COMP_CRC32C(crc,
				ControlFile,
				offsetof(ControlFileData, crc));
	FIN_CRC32C(crc);

	if (!EQ_CRC32C(crc, ControlFile->crc))
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("incorrect checksum in control file")));

	/*
	 * Do compatibility checking immediately.  If the database isn't
	 * compatible with the backend executable, we want to abort before we can
	 * possibly do any damage.
	 */
	if (ControlFile->catalog_version_no != CATALOG_VERSION_NO)
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("database files are incompatible with server"),
		/* translator: %s is a variable name and %d is its value */
				 errdetail("The database cluster was initialized with %s %d,"
						   " but the server was compiled with %s %d.",
						   "CATALOG_VERSION_NO", ControlFile->catalog_version_no,
						   "CATALOG_VERSION_NO", CATALOG_VERSION_NO),
				 errhint("It looks like you need to initdb.")));
	if (ControlFile->maxAlign != MAXIMUM_ALIGNOF)
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("database files are incompatible with server"),
		/* translator: %s is a variable name and %d is its value */
				 errdetail("The database cluster was initialized with %s %d,"
						   " but the server was compiled with %s %d.",
						   "MAXALIGN", ControlFile->maxAlign,
						   "MAXALIGN", MAXIMUM_ALIGNOF),
				 errhint("It looks like you need to initdb.")));
	if (ControlFile->floatFormat != FLOATFORMAT_VALUE)
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("database files are incompatible with server"),
				 errdetail("The database cluster appears to use a different floating-point number format than the server executable."),
				 errhint("It looks like you need to initdb.")));
	if (ControlFile->blcksz != BLCKSZ)
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("database files are incompatible with server"),
		/* translator: %s is a variable name and %d is its value */
				 errdetail("The database cluster was initialized with %s %d,"
						   " but the server was compiled with %s %d.",
						   "BLCKSZ", ControlFile->blcksz,
						   "BLCKSZ", BLCKSZ),
				 errhint("It looks like you need to recompile or initdb.")));
	if (ControlFile->relseg_size != RELSEG_SIZE)
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("database files are incompatible with server"),
		/* translator: %s is a variable name and %d is its value */
				 errdetail("The database cluster was initialized with %s %d,"
						   " but the server was compiled with %s %d.",
						   "RELSEG_SIZE", ControlFile->relseg_size,
						   "RELSEG_SIZE", RELSEG_SIZE),
				 errhint("It looks like you need to recompile or initdb.")));
	if (ControlFile->xlog_blcksz != XLOG_BLCKSZ)
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("database files are incompatible with server"),
		/* translator: %s is a variable name and %d is its value */
				 errdetail("The database cluster was initialized with %s %d,"
						   " but the server was compiled with %s %d.",
						   "XLOG_BLCKSZ", ControlFile->xlog_blcksz,
						   "XLOG_BLCKSZ", XLOG_BLCKSZ),
				 errhint("It looks like you need to recompile or initdb.")));
	if (ControlFile->nameDataLen != NAMEDATALEN)
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("database files are incompatible with server"),
		/* translator: %s is a variable name and %d is its value */
				 errdetail("The database cluster was initialized with %s %d,"
						   " but the server was compiled with %s %d.",
						   "NAMEDATALEN", ControlFile->nameDataLen,
						   "NAMEDATALEN", NAMEDATALEN),
				 errhint("It looks like you need to recompile or initdb.")));
	if (ControlFile->indexMaxKeys != INDEX_MAX_KEYS)
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("database files are incompatible with server"),
		/* translator: %s is a variable name and %d is its value */
				 errdetail("The database cluster was initialized with %s %d,"
						   " but the server was compiled with %s %d.",
						   "INDEX_MAX_KEYS", ControlFile->indexMaxKeys,
						   "INDEX_MAX_KEYS", INDEX_MAX_KEYS),
				 errhint("It looks like you need to recompile or initdb.")));
	if (ControlFile->toast_max_chunk_size != TOAST_MAX_CHUNK_SIZE)
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("database files are incompatible with server"),
		/* translator: %s is a variable name and %d is its value */
				 errdetail("The database cluster was initialized with %s %d,"
						   " but the server was compiled with %s %d.",
						   "TOAST_MAX_CHUNK_SIZE", ControlFile->toast_max_chunk_size,
						   "TOAST_MAX_CHUNK_SIZE", (int) TOAST_MAX_CHUNK_SIZE),
				 errhint("It looks like you need to recompile or initdb.")));
	if (ControlFile->loblksize != LOBLKSIZE)
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("database files are incompatible with server"),
		/* translator: %s is a variable name and %d is its value */
				 errdetail("The database cluster was initialized with %s %d,"
						   " but the server was compiled with %s %d.",
						   "LOBLKSIZE", ControlFile->loblksize,
						   "LOBLKSIZE", (int) LOBLKSIZE),
				 errhint("It looks like you need to recompile or initdb.")));

#ifdef USE_FLOAT8_BYVAL
	if (ControlFile->float8ByVal != true)
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized without USE_FLOAT8_BYVAL"
						   " but the server was compiled with USE_FLOAT8_BYVAL."),
				 errhint("It looks like you need to recompile or initdb.")));
#else
	if (ControlFile->float8ByVal != false)
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with USE_FLOAT8_BYVAL"
						   " but the server was compiled without USE_FLOAT8_BYVAL."),
				 errhint("It looks like you need to recompile or initdb.")));
#endif

	wal_segment_size = ControlFile->xlog_seg_size;

	if (!IsValidWalSegSize(wal_segment_size))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg_plural("invalid WAL segment size in control file (%d byte)",
									  "invalid WAL segment size in control file (%d bytes)",
									  wal_segment_size,
									  wal_segment_size),
						errdetail("The WAL segment size must be a power of two between 1 MB and 1 GB.")));

	snprintf(wal_segsz_str, sizeof(wal_segsz_str), "%d", wal_segment_size);
	SetConfigOption("wal_segment_size", wal_segsz_str, PGC_INTERNAL,
					PGC_S_DYNAMIC_DEFAULT);

	/* check and update variables dependent on wal_segment_size */
	if (ConvertToXSegs(min_wal_size_mb, wal_segment_size) < 2)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		/* translator: both %s are GUC names */
						errmsg("\"%s\" must be at least twice \"%s\"",
							   "min_wal_size", "wal_segment_size")));

	if (ConvertToXSegs(max_wal_size_mb, wal_segment_size) < 2)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		/* translator: both %s are GUC names */
						errmsg("\"%s\" must be at least twice \"%s\"",
							   "max_wal_size", "wal_segment_size")));

	UsableBytesInSegment =
		(wal_segment_size / XLOG_BLCKSZ * UsableBytesInPage) -
		(SizeOfXLogLongPHD - SizeOfXLogShortPHD);

	CalculateCheckpointSegments();

	/* Make the initdb settings visible as GUC variables, too */
	SetConfigOption("data_checksums", DataChecksumsEnabled() ? "yes" : "no",
					PGC_INTERNAL, PGC_S_DYNAMIC_DEFAULT);
}

/*
 * Utility wrapper to update the control file.  Note that the control
 * file gets flushed.
 */
static void
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
 * Return true if the cluster was initialized on a platform where the
 * default signedness of char is "signed". This function exists for code
 * that deals with pre-v18 data files that store data sorted by the 'char'
 * type on disk (e.g., GIN and GiST indexes). See the comments in
 * WriteControlFile() for details.
 */
bool
GetDefaultCharSignedness(void)
{
	return ControlFile->default_char_signedness;
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
	return pg_atomic_fetch_add_u64(&XLogCtl->unloggedLSN, 1);
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
 * GUC check_hook for wal_consistency_checking
 */
bool
check_wal_consistency_checking(char **newval, void **extra, GucSource source)
{
	char	   *rawstring;
	List	   *elemlist;
	ListCell   *l;
	bool		newwalconsistency[RM_MAX_ID + 1];

	/* Initialize the array */
	MemSet(newwalconsistency, 0, (RM_MAX_ID + 1) * sizeof(bool));

	/* Need a modifiable copy of string */
	rawstring = pstrdup(*newval);

	/* Parse string into list of identifiers */
	if (!SplitIdentifierString(rawstring, ',', &elemlist))
	{
		/* syntax error in list */
		GUC_check_errdetail("List syntax is invalid.");
		pfree(rawstring);
		list_free(elemlist);
		return false;
	}

	foreach(l, elemlist)
	{
		char	   *tok = (char *) lfirst(l);
		int			rmid;

		/* Check for 'all'. */
		if (pg_strcasecmp(tok, "all") == 0)
		{
			for (rmid = 0; rmid <= RM_MAX_ID; rmid++)
				if (RmgrIdExists(rmid) && GetRmgr(rmid).rm_mask != NULL)
					newwalconsistency[rmid] = true;
		}
		else
		{
			/* Check if the token matches any known resource manager. */
			bool		found = false;

			for (rmid = 0; rmid <= RM_MAX_ID; rmid++)
			{
				if (RmgrIdExists(rmid) && GetRmgr(rmid).rm_mask != NULL &&
					pg_strcasecmp(tok, GetRmgr(rmid).rm_name) == 0)
				{
					newwalconsistency[rmid] = true;
					found = true;
					break;
				}
			}
			if (!found)
			{
				/*
				 * During startup, it might be a not-yet-loaded custom
				 * resource manager.  Defer checking until
				 * InitializeWalConsistencyChecking().
				 */
				if (!process_shared_preload_libraries_done)
				{
					check_wal_consistency_checking_deferred = true;
				}
				else
				{
					GUC_check_errdetail("Unrecognized key word: \"%s\".", tok);
					pfree(rawstring);
					list_free(elemlist);
					return false;
				}
			}
		}
	}

	pfree(rawstring);
	list_free(elemlist);

	/* assign new value */
	*extra = guc_malloc(LOG, (RM_MAX_ID + 1) * sizeof(bool));
	if (!*extra)
		return false;
	memcpy(*extra, newwalconsistency, (RM_MAX_ID + 1) * sizeof(bool));
	return true;
}

/*
 * GUC assign_hook for wal_consistency_checking
 */
void
assign_wal_consistency_checking(const char *newval, void *extra)
{
	/*
	 * If some checks were deferred, it's possible that the checks will fail
	 * later during InitializeWalConsistencyChecking(). But in that case, the
	 * postmaster will exit anyway, so it's safe to proceed with the
	 * assignment.
	 *
	 * Any built-in resource managers specified are assigned immediately,
	 * which affects WAL created before shared_preload_libraries are
	 * processed. Any custom resource managers specified won't be assigned
	 * until after shared_preload_libraries are processed, but that's OK
	 * because WAL for a custom resource manager can't be written before the
	 * module is loaded anyway.
	 */
	wal_consistency_checking = extra;
}

/*
 * InitializeWalConsistencyChecking: run after loading custom resource managers
 *
 * If any unknown resource managers were specified in the
 * wal_consistency_checking GUC, processing was deferred.  Now that
 * shared_preload_libraries have been loaded, process wal_consistency_checking
 * again.
 */
void
InitializeWalConsistencyChecking(void)
{
	Assert(process_shared_preload_libraries_done);

	if (check_wal_consistency_checking_deferred)
	{
		struct config_generic *guc;

		guc = find_option("wal_consistency_checking", false, false, ERROR);

		check_wal_consistency_checking_deferred = false;

		set_config_option_ext("wal_consistency_checking",
							  wal_consistency_checking_string,
							  guc->scontext, guc->source, guc->srole,
							  GUC_ACTION_SET, true, ERROR, false);

		/* checking should not be deferred again */
		Assert(!check_wal_consistency_checking_deferred);
	}
}

/*
 * GUC show_hook for archive_command
 */
const char *
show_archive_command(void)
{
	if (XLogArchivingActive())
		return XLogArchiveCommand;
	else
		return "(disabled)";
}

/*
 * GUC show_hook for in_hot_standby
 */
const char *
show_in_hot_standby(void)
{
	/*
	 * We display the actual state based on shared memory, so that this GUC
	 * reports up-to-date state if examined intra-query.  The underlying
	 * variable (in_hot_standby_guc) changes only when we transmit a new value
	 * to the client.
	 */
	return RecoveryInProgress() ? "on" : "off";
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
 * Get the wal_level from the control file. For a standby, this value should be
 * considered as its active wal_level, because it may be different from what
 * was originally configured on standby.
 */
WalLevel
GetActiveWalLevelOnStandby(void)
{
	return ControlFile->wal_level;
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
	 *
	 * We prefer to report this value's source as PGC_S_DYNAMIC_DEFAULT.
	 * However, if the DBA explicitly set wal_buffers = -1 in the config file,
	 * then PGC_S_DYNAMIC_DEFAULT will fail to override that and we must force
	 * the matter with PGC_S_OVERRIDE.
	 */
	if (XLOGbuffers == -1)
	{
		char		buf[32];

		snprintf(buf, sizeof(buf), "%d", XLOGChooseNumBuffers());
		SetConfigOption("wal_buffers", buf, PGC_POSTMASTER,
						PGC_S_DYNAMIC_DEFAULT);
		if (XLOGbuffers == -1)	/* failed to apply it? */
			SetConfigOption("wal_buffers", buf, PGC_POSTMASTER,
							PGC_S_OVERRIDE);
	}
	Assert(XLOGbuffers > 0);

	/* XLogCtl */
	size = sizeof(XLogCtlData);

	/* WAL insertion locks, plus alignment */
	size = add_size(size, mul_size(sizeof(WALInsertLockPadded), NUM_XLOGINSERT_LOCKS + 1));
	/* xlblocks array */
	size = add_size(size, mul_size(sizeof(pg_atomic_uint64), XLOGbuffers));
	/* extra alignment padding for XLOG I/O buffers */
	size = add_size(size, Max(XLOG_BLCKSZ, PG_IO_ALIGN_SIZE));
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
	XLogCtl->xlblocks = (pg_atomic_uint64 *) allocptr;
	allocptr += sizeof(pg_atomic_uint64) * XLOGbuffers;

	for (i = 0; i < XLOGbuffers; i++)
	{
		pg_atomic_init_u64(&XLogCtl->xlblocks[i], InvalidXLogRecPtr);
	}

	/* WAL insertion locks. Ensure they're aligned to the full padded size */
	allocptr += sizeof(WALInsertLockPadded) -
		((uintptr_t) allocptr) % sizeof(WALInsertLockPadded);
	WALInsertLocks = XLogCtl->Insert.WALInsertLocks =
		(WALInsertLockPadded *) allocptr;
	allocptr += sizeof(WALInsertLockPadded) * NUM_XLOGINSERT_LOCKS;

	for (i = 0; i < NUM_XLOGINSERT_LOCKS; i++)
	{
		LWLockInitialize(&WALInsertLocks[i].l.lock, LWTRANCHE_WAL_INSERT);
		pg_atomic_init_u64(&WALInsertLocks[i].l.insertingAt, InvalidXLogRecPtr);
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
	XLogCtl->InstallXLogFileSegmentActive = false;
	XLogCtl->WalWriterSleeping = false;

	SpinLockInit(&XLogCtl->Insert.insertpos_lck);
	SpinLockInit(&XLogCtl->info_lck);
	pg_atomic_init_u64(&XLogCtl->logInsertResult, InvalidXLogRecPtr);
	pg_atomic_init_u64(&XLogCtl->logWriteResult, InvalidXLogRecPtr);
	pg_atomic_init_u64(&XLogCtl->logFlushResult, InvalidXLogRecPtr);
	pg_atomic_init_u64(&XLogCtl->unloggedLSN, InvalidXLogRecPtr);

	pg_atomic_init_u64(&XLogCtl->InitializeReserved, InvalidXLogRecPtr);
	pg_atomic_init_u64(&XLogCtl->InitializedUpTo, InvalidXLogRecPtr);
	ConditionVariableInit(&XLogCtl->InitializedUpToCondVar);
}

/*
 * This func must be called ONCE on system install.  It creates pg_control
 * and the initial XLOG segment.
 */
void
BootStrapXLOG(uint32 data_checksum_version)
{
	CheckPoint	checkPoint;
	char	   *buffer;
	XLogPageHeader page;
	XLogLongPageHeader longpage;
	XLogRecord *record;
	char	   *recptr;
	uint64		sysidentifier;
	struct timeval tv;
	pg_crc32c	crc;

	/* allow ordinary WAL segment creation, like StartupXLOG() would */
	SetInstallXLogFileSegmentActive();

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
	checkPoint.ThisTimeLineID = BootstrapTimeLineID;
	checkPoint.PrevTimeLineID = BootstrapTimeLineID;
	checkPoint.fullPageWrites = fullPageWrites;
	checkPoint.wal_level = wal_level;
	checkPoint.nextXid =
		FullTransactionIdFromEpochAndXid(0, FirstNormalTransactionId);
	checkPoint.nextOid = FirstGenbkiObjectId;
	checkPoint.nextMulti = FirstMultiXactId;
	checkPoint.nextMultiOffset = 0;
	checkPoint.oldestXid = FirstNormalTransactionId;
	checkPoint.oldestXidDB = Template1DbOid;
	checkPoint.oldestMulti = FirstMultiXactId;
	checkPoint.oldestMultiDB = Template1DbOid;
	checkPoint.oldestCommitTsXid = InvalidTransactionId;
	checkPoint.newestCommitTsXid = InvalidTransactionId;
	checkPoint.time = (pg_time_t) time(NULL);
	checkPoint.oldestActiveXid = InvalidTransactionId;

	TransamVariables->nextXid = checkPoint.nextXid;
	TransamVariables->nextOid = checkPoint.nextOid;
	TransamVariables->oidCount = 0;
	MultiXactSetNextMXact(checkPoint.nextMulti, checkPoint.nextMultiOffset);
	AdvanceOldestClogXid(checkPoint.oldestXid);
	SetTransactionIdLimit(checkPoint.oldestXid, checkPoint.oldestXidDB);
	SetMultiXactIdLimit(checkPoint.oldestMulti, checkPoint.oldestMultiDB, true);
	SetCommitTsLimit(InvalidTransactionId, InvalidTransactionId);

	/* Set up the XLOG page header */
	page->xlp_magic = XLOG_PAGE_MAGIC;
	page->xlp_info = XLP_LONG_HEADER;
	page->xlp_tli = BootstrapTimeLineID;
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
	openLogTLI = BootstrapTimeLineID;
	openLogFile = XLogFileInit(1, BootstrapTimeLineID);

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
	InitControlFile(sysidentifier, data_checksum_version);
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
	char	   *buf = palloc(128);

	pg_strftime(buf, 128,
				"%Y-%m-%d %H:%M:%S %Z",
				pg_localtime(&tnow, log_timezone));

	return buf;
}

/*
 * Initialize the first WAL segment on new timeline.
 */
static void
XLogInitNewTimeline(TimeLineID endTLI, XLogRecPtr endOfLog, TimeLineID newTLI)
{
	char		xlogfname[MAXFNAMELEN];
	XLogSegNo	endLogSegNo;
	XLogSegNo	startLogSegNo;

	/* we always switch to a new timeline after archive recovery */
	Assert(endTLI != newTLI);

	/*
	 * Update min recovery point one last time.
	 */
	UpdateMinRecoveryPoint(InvalidXLogRecPtr, true);

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
		XLogFileCopy(newTLI, endLogSegNo, endTLI, endLogSegNo,
					 XLogSegmentOffset(endOfLog, wal_segment_size));
	}
	else
	{
		/*
		 * The switch happened at a segment boundary, so just create the next
		 * segment on the new timeline.
		 */
		int			fd;

		fd = XLogFileInit(startLogSegNo, newTLI);

		if (close(fd) != 0)
		{
			int			save_errno = errno;

			XLogFileName(xlogfname, newTLI, startLogSegNo, wal_segment_size);
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
	XLogFileName(xlogfname, newTLI, startLogSegNo, wal_segment_size);
	XLogArchiveCleanup(xlogfname);
}

/*
 * Perform cleanup actions at the conclusion of archive recovery.
 */
static void
CleanupAfterArchiveRecovery(TimeLineID EndOfLogTLI, XLogRecPtr EndOfLog,
							TimeLineID newTLI)
{
	/*
	 * Execute the recovery_end_command, if any.
	 */
	if (recoveryEndCommand && strcmp(recoveryEndCommand, "") != 0)
		ExecuteRecoveryCommand(recoveryEndCommand,
							   "recovery_end_command",
							   true,
							   WAIT_EVENT_RECOVERY_END_COMMAND);

	/*
	 * We switched to a new timeline. Clean up segments on the old timeline.
	 *
	 * If there are any higher-numbered segments on the old timeline, remove
	 * them. They might contain valid WAL, but they might also be
	 * pre-allocated files containing garbage. In any case, they are not part
	 * of the new timeline's history so we don't need them.
	 */
	RemoveNonParentXlogFiles(EndOfLog, newTLI);

	/*
	 * If the switch happened in the middle of a segment, what to do with the
	 * last, partial segment on the old timeline? If we don't archive it, and
	 * the server that created the WAL never archives it either (e.g. because
	 * it was hit by a meteor), it will never make it to the archive. That's
	 * OK from our point of view, because the new segment that we created with
	 * the new TLI contains all the WAL from the old timeline up to the switch
	 * point. But if you later try to do PITR to the "missing" WAL on the old
	 * timeline, recovery won't find it in the archive. It's physically
	 * present in the new file with new TLI, but recovery won't look there
	 * when it's recovering to the older timeline. On the other hand, if we
	 * archive the partial segment, and the original server on that timeline
	 * is still running and archives the completed version of the same segment
	 * later, it will fail. (We used to do that in 9.4 and below, and it
	 * caused such problems).
	 *
	 * As a compromise, we rename the last segment with the .partial suffix,
	 * and archive it. Archive recovery will never try to read .partial
	 * segments, so they will normally go unused. But in the odd PITR case,
	 * the administrator can copy them manually to the pg_wal directory
	 * (removing the suffix). They can be useful in debugging, too.
	 *
	 * If a .done or .ready file already exists for the old timeline, however,
	 * we had already determined that the segment is complete, so we can let
	 * it be archived normally. (In particular, if it was restored from the
	 * archive to begin with, it's expected to have a .done file).
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

			/*
			 * If we're summarizing WAL, we can't rename the partial file
			 * until the summarizer finishes with it, else it will fail.
			 */
			if (summarize_wal)
				WaitForWalSummarization(EndOfLog);

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
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("WAL was generated with \"wal_level=minimal\", cannot continue recovering"),
				 errdetail("This happens if you temporarily set \"wal_level=minimal\" on the server."),
				 errhint("Use a backup taken after setting \"wal_level\" to higher than \"minimal\".")));
	}

	/*
	 * For Hot Standby, the WAL must be generated with 'replica' mode, and we
	 * must have at least as many backend slots as the primary.
	 */
	if (ArchiveRecoveryRequested && EnableHotStandby)
	{
		/* We ignore autovacuum_worker_slots when we make this test. */
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
	bool		didCrash;
	bool		haveTblspcMap;
	bool		haveBackupLabel;
	XLogRecPtr	EndOfLog;
	TimeLineID	EndOfLogTLI;
	TimeLineID	newTLI;
	bool		performedWalRecovery;
	EndOfWalRecoveryInfo *endOfRecoveryInfo;
	XLogRecPtr	abortedRecPtr;
	XLogRecPtr	missingContrecPtr;
	TransactionId oldestActiveXID;
	bool		promoted = false;

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
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("control file contains invalid checkpoint location")));

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
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("control file contains invalid database cluster state")));
	}

	/* This is just to allow attaching to startup process with a debugger */
#ifdef XLOG_REPLAY_DELAY
	if (ControlFile->state != DB_SHUTDOWNED)
		pg_usleep(60000000L);
#endif

	/*
	 * Verify that pg_wal, pg_wal/archive_status, and pg_wal/summaries exist.
	 * In cases where someone has performed a copy for PITR, these directories
	 * may have been excluded and need to be re-created.
	 */
	ValidateXLOGDirectoryStructure();

	/* Set up timeout handler needed to report startup progress. */
	if (!IsBootstrapProcessingMode())
		RegisterTimeout(STARTUP_PROGRESS_TIMEOUT,
						startup_progress_timeout_handler);

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
		didCrash = true;
	}
	else
		didCrash = false;

	/*
	 * Prepare for WAL recovery if needed.
	 *
	 * InitWalRecovery analyzes the control file and the backup label file, if
	 * any.  It updates the in-memory ControlFile buffer according to the
	 * starting checkpoint, and sets InRecovery and ArchiveRecoveryRequested.
	 * It also applies the tablespace map file, if any.
	 */
	InitWalRecovery(ControlFile, &wasShutdown,
					&haveBackupLabel, &haveTblspcMap);
	checkPoint = ControlFile->checkPointCopy;

	/* initialize shared memory variables from the checkpoint record */
	TransamVariables->nextXid = checkPoint.nextXid;
	TransamVariables->nextOid = checkPoint.nextOid;
	TransamVariables->oidCount = 0;
	MultiXactSetNextMXact(checkPoint.nextMulti, checkPoint.nextMultiOffset);
	AdvanceOldestClogXid(checkPoint.oldestXid);
	SetTransactionIdLimit(checkPoint.oldestXid, checkPoint.oldestXidDB);
	SetMultiXactIdLimit(checkPoint.oldestMulti, checkPoint.oldestMultiDB, true);
	SetCommitTsLimit(checkPoint.oldestCommitTsXid,
					 checkPoint.newestCommitTsXid);

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
	 * Startup CLOG. This must be done after TransamVariables->nextXid has
	 * been initialized and before we accept connections or begin WAL replay.
	 */
	StartupCLOG();

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
		pg_atomic_write_membarrier_u64(&XLogCtl->unloggedLSN,
									   ControlFile->unloggedLSN);
	else
		pg_atomic_write_membarrier_u64(&XLogCtl->unloggedLSN,
									   FirstNormalUnloggedLSN);

	/*
	 * Copy any missing timeline history files between 'now' and the recovery
	 * target timeline from archive to pg_wal. While we don't need those files
	 * ourselves - the history file of the recovery target timeline covers all
	 * the previous timelines in the history too - a cascading standby server
	 * might be interested in them. Or, if you archive the WAL from this
	 * server to a different archive than the primary, it'd be good for all
	 * the history files to get archived there after failover, so that you can
	 * use one of the old timelines as a PITR target. Timeline history files
	 * are small, so it's better to copy them unnecessarily than not copy them
	 * and regret later.
	 */
	restoreTimeLineHistoryFiles(checkPoint.ThisTimeLineID, recoveryTargetTLI);

	/*
	 * Before running in recovery, scan pg_twophase and fill in its status to
	 * be able to work on entries generated by redo.  Doing a scan before
	 * taking any recovery action has the merit to discard any 2PC files that
	 * are newer than the first record to replay, saving from any conflicts at
	 * replay.  This avoids as well any subsequent scans when doing recovery
	 * of the on-disk two-phase data.
	 */
	restoreTwoPhaseData();

	/*
	 * When starting with crash recovery, reset pgstat data - it might not be
	 * valid. Otherwise restore pgstat data. It's safe to do this here,
	 * because postmaster will not yet have started any other processes.
	 *
	 * NB: Restoring replication slot stats relies on slot state to have
	 * already been restored from disk.
	 *
	 * TODO: With a bit of extra work we could just start with a pgstat file
	 * associated with the checkpoint redo location we're starting from.
	 */
	if (didCrash)
		pgstat_discard_stats();
	else
		pgstat_restore_stats();

	lastFullPageWrites = checkPoint.fullPageWrites;

	RedoRecPtr = XLogCtl->RedoRecPtr = XLogCtl->Insert.RedoRecPtr = checkPoint.redo;
	doPageWrites = lastFullPageWrites;

	/* REDO */
	if (InRecovery)
	{
		/* Initialize state for RecoveryInProgress() */
		SpinLockAcquire(&XLogCtl->info_lck);
		if (InArchiveRecovery)
			XLogCtl->SharedRecoveryState = RECOVERY_STATE_ARCHIVE;
		else
			XLogCtl->SharedRecoveryState = RECOVERY_STATE_CRASH;
		SpinLockRelease(&XLogCtl->info_lck);

		/*
		 * Update pg_control to show that we are recovering and to show the
		 * selected checkpoint as the place we are starting from. We also mark
		 * pg_control with any minimum recovery stop point obtained from a
		 * backup history file.
		 *
		 * No need to hold ControlFileLock yet, we aren't up far enough.
		 */
		UpdateControlFile();

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
			LocalMinRecoveryPoint = ControlFile->minRecoveryPoint;
			LocalMinRecoveryPointTLI = ControlFile->minRecoveryPointTLI;
		}
		else
		{
			LocalMinRecoveryPoint = InvalidXLogRecPtr;
			LocalMinRecoveryPointTLI = 0;
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
					(errmsg_internal("initializing for hot standby")));

			InitRecoveryTransactionEnvironment();

			if (wasShutdown)
				oldestActiveXID = PrescanPreparedTransactions(&xids, &nxids);
			else
				oldestActiveXID = checkPoint.oldestActiveXid;
			Assert(TransactionIdIsValid(oldestActiveXID));

			/* Tell procarray about the range of xids it has to deal with */
			ProcArrayInitRecovery(XidFromFullTransactionId(TransamVariables->nextXid));

			/*
			 * Startup subtrans only.  CLOG, MultiXact and commit timestamp
			 * have already been started up and other SLRUs are not maintained
			 * during recovery and need not be started yet.
			 */
			StartupSUBTRANS(oldestActiveXID);

			/*
			 * If we're beginning at a shutdown checkpoint, we know that
			 * nothing was running on the primary at this point. So fake-up an
			 * empty running-xacts record and use that here and now. Recover
			 * additional standby state for prepared transactions.
			 */
			if (wasShutdown)
			{
				RunningTransactionsData running;
				TransactionId latestCompletedXid;

				/* Update pg_subtrans entries for any prepared transactions */
				StandbyRecoverPreparedTransactions();

				/*
				 * Construct a RunningTransactions snapshot representing a
				 * shut down server, with only prepared transactions still
				 * alive. We're never overflowed at this point because all
				 * subxids are listed with their parent prepared transactions.
				 */
				running.xcnt = nxids;
				running.subxcnt = 0;
				running.subxid_status = SUBXIDS_IN_SUBTRANS;
				running.nextXid = XidFromFullTransactionId(checkPoint.nextXid);
				running.oldestRunningXid = oldestActiveXID;
				latestCompletedXid = XidFromFullTransactionId(checkPoint.nextXid);
				TransactionIdRetreat(latestCompletedXid);
				Assert(TransactionIdIsNormal(latestCompletedXid));
				running.latestCompletedXid = latestCompletedXid;
				running.xids = xids;

				ProcArrayApplyRecoveryInfo(&running);
			}
		}

		/*
		 * We're all set for replaying the WAL now. Do it.
		 */
		PerformWalRecovery();
		performedWalRecovery = true;
	}
	else
		performedWalRecovery = false;

	/*
	 * Finish WAL recovery.
	 */
	endOfRecoveryInfo = FinishWalRecovery();
	EndOfLog = endOfRecoveryInfo->endOfLog;
	EndOfLogTLI = endOfRecoveryInfo->endOfLogTLI;
	abortedRecPtr = endOfRecoveryInfo->abortedRecPtr;
	missingContrecPtr = endOfRecoveryInfo->missingContrecPtr;

	/*
	 * Reset ps status display, so as no information related to recovery shows
	 * up.
	 */
	set_ps_display("");

	/*
	 * When recovering from a backup (we are in recovery, and archive recovery
	 * was requested), complain if we did not roll forward far enough to reach
	 * the point where the database is consistent.  For regular online
	 * backup-from-primary, that means reaching the end-of-backup WAL record
	 * (at which point we reset backupStartPoint to be Invalid), for
	 * backup-from-replica (which can't inject records into the WAL stream),
	 * that point is when we reach the minRecoveryPoint in pg_control (which
	 * we purposefully copy last when backing up from a replica).  For
	 * pg_rewind (which creates a backup_label with a method of "pg_rewind")
	 * or snapshot-style backups (which don't), backupEndRequired will be set
	 * to false.
	 *
	 * Note: it is indeed okay to look at the local variable
	 * LocalMinRecoveryPoint here, even though ControlFile->minRecoveryPoint
	 * might be further ahead --- ControlFile->minRecoveryPoint cannot have
	 * been advanced beyond the WAL we processed.
	 */
	if (InRecovery &&
		(EndOfLog < LocalMinRecoveryPoint ||
		 !XLogRecPtrIsInvalid(ControlFile->backupStartPoint)))
	{
		/*
		 * Ran off end of WAL before reaching end-of-backup WAL record, or
		 * minRecoveryPoint. That's a bad sign, indicating that you tried to
		 * recover from an online backup but never called pg_backup_stop(), or
		 * you didn't archive all the WAL needed.
		 */
		if (ArchiveRecoveryRequested || ControlFile->backupEndRequired)
		{
			if (!XLogRecPtrIsInvalid(ControlFile->backupStartPoint) || ControlFile->backupEndRequired)
				ereport(FATAL,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("WAL ends before end of online backup"),
						 errhint("All WAL generated while online backup was taken must be available at recovery.")));
			else
				ereport(FATAL,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("WAL ends before consistent recovery point")));
		}
	}

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
	 * Pre-scan prepared transactions to find out the range of XIDs present.
	 * This information is not quite needed yet, but it is positioned here so
	 * as potential problems are detected before any on-disk change is done.
	 */
	oldestActiveXID = PrescanPreparedTransactions(NULL, NULL);

	/*
	 * Allow ordinary WAL segment creation before possibly switching to a new
	 * timeline, which creates a new segment, and after the last ReadRecord().
	 */
	SetInstallXLogFileSegmentActive();

	/*
	 * Consider whether we need to assign a new timeline ID.
	 *
	 * If we did archive recovery, we always assign a new ID.  This handles a
	 * couple of issues.  If we stopped short of the end of WAL during
	 * recovery, then we are clearly generating a new timeline and must assign
	 * it a unique new ID.  Even if we ran to the end, modifying the current
	 * last segment is problematic because it may result in trying to
	 * overwrite an already-archived copy of that segment, and we encourage
	 * DBAs to make their archive_commands reject that.  We can dodge the
	 * problem by making the new active segment have a new timeline ID.
	 *
	 * In a normal crash recovery, we can just extend the timeline we were in.
	 */
	newTLI = endOfRecoveryInfo->lastRecTLI;
	if (ArchiveRecoveryRequested)
	{
		newTLI = findNewestTimeLine(recoveryTargetTLI) + 1;
		ereport(LOG,
				(errmsg("selected new timeline ID: %u", newTLI)));

		/*
		 * Make a writable copy of the last WAL segment.  (Note that we also
		 * have a copy of the last block of the old WAL in
		 * endOfRecovery->lastPage; we will use that below.)
		 */
		XLogInitNewTimeline(EndOfLogTLI, EndOfLog, newTLI);

		/*
		 * Remove the signal files out of the way, so that we don't
		 * accidentally re-enter archive recovery mode in a subsequent crash.
		 */
		if (endOfRecoveryInfo->standby_signal_file_found)
			durable_unlink(STANDBY_SIGNAL_FILE, FATAL);

		if (endOfRecoveryInfo->recovery_signal_file_found)
			durable_unlink(RECOVERY_SIGNAL_FILE, FATAL);

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
		writeTimeLineHistory(newTLI, recoveryTargetTLI,
							 EndOfLog, endOfRecoveryInfo->recoveryStopReason);

		ereport(LOG,
				(errmsg("archive recovery complete")));
	}

	/* Save the selected TimeLineID in shared memory, too */
	SpinLockAcquire(&XLogCtl->info_lck);
	XLogCtl->InsertTimeLineID = newTLI;
	XLogCtl->PrevTimeLineID = endOfRecoveryInfo->lastRecTLI;
	SpinLockRelease(&XLogCtl->info_lck);

	/*
	 * Actually, if WAL ended in an incomplete record, skip the parts that
	 * made it through and start writing after the portion that persisted.
	 * (It's critical to first write an OVERWRITE_CONTRECORD message, which
	 * we'll do as soon as we're open for writing new WAL.)
	 */
	if (!XLogRecPtrIsInvalid(missingContrecPtr))
	{
		/*
		 * We should only have a missingContrecPtr if we're not switching to a
		 * new timeline. When a timeline switch occurs, WAL is copied from the
		 * old timeline to the new only up to the end of the last complete
		 * record, so there can't be an incomplete WAL record that we need to
		 * disregard.
		 */
		Assert(newTLI == endOfRecoveryInfo->lastRecTLI);
		Assert(!XLogRecPtrIsInvalid(abortedRecPtr));
		EndOfLog = missingContrecPtr;
	}

	/*
	 * Prepare to write WAL starting at EndOfLog location, and init xlog
	 * buffer cache using the block containing the last record from the
	 * previous incarnation.
	 */
	Insert = &XLogCtl->Insert;
	Insert->PrevBytePos = XLogRecPtrToBytePos(endOfRecoveryInfo->lastRec);
	Insert->CurrBytePos = XLogRecPtrToBytePos(EndOfLog);

	/*
	 * Tricky point here: lastPage contains the *last* block that the LastRec
	 * record spans, not the one it starts in.  The last block is indeed the
	 * one we want to use.
	 */
	if (EndOfLog % XLOG_BLCKSZ != 0)
	{
		char	   *page;
		int			len;
		int			firstIdx;

		firstIdx = XLogRecPtrToBufIdx(EndOfLog);
		len = EndOfLog - endOfRecoveryInfo->lastPageBeginPtr;
		Assert(len < XLOG_BLCKSZ);

		/* Copy the valid part of the last block, and zero the rest */
		page = &XLogCtl->pages[firstIdx * XLOG_BLCKSZ];
		memcpy(page, endOfRecoveryInfo->lastPage, len);
		memset(page + len, 0, XLOG_BLCKSZ - len);

		pg_atomic_write_u64(&XLogCtl->xlblocks[firstIdx], endOfRecoveryInfo->lastPageBeginPtr + XLOG_BLCKSZ);
		pg_atomic_write_u64(&XLogCtl->InitializedUpTo, endOfRecoveryInfo->lastPageBeginPtr + XLOG_BLCKSZ);
		XLogCtl->InitializedFrom = endOfRecoveryInfo->lastPageBeginPtr;
	}
	else
	{
		/*
		 * There is no partial block to copy. Just set InitializedUpTo, and
		 * let the first attempt to insert a log record to initialize the next
		 * buffer.
		 */
		pg_atomic_write_u64(&XLogCtl->InitializedUpTo, EndOfLog);
		XLogCtl->InitializedFrom = EndOfLog;
	}
	pg_atomic_write_u64(&XLogCtl->InitializeReserved, pg_atomic_read_u64(&XLogCtl->InitializedUpTo));

	/*
	 * Update local and shared status.  This is OK to do without any locks
	 * because no other process can be reading or writing WAL yet.
	 */
	LogwrtResult.Write = LogwrtResult.Flush = EndOfLog;
	pg_atomic_write_u64(&XLogCtl->logInsertResult, EndOfLog);
	pg_atomic_write_u64(&XLogCtl->logWriteResult, EndOfLog);
	pg_atomic_write_u64(&XLogCtl->logFlushResult, EndOfLog);
	XLogCtl->LogwrtRqst.Write = EndOfLog;
	XLogCtl->LogwrtRqst.Flush = EndOfLog;

	/*
	 * Preallocate additional log files, if wanted.
	 */
	PreallocXlogFiles(EndOfLog, newTLI);

	/*
	 * Okay, we're officially UP.
	 */
	InRecovery = false;

	/* start the archive_timeout timer and LSN running */
	XLogCtl->lastSegSwitchTime = (pg_time_t) time(NULL);
	XLogCtl->lastSegSwitchLSN = EndOfLog;

	/* also initialize latestCompletedXid, to nextXid - 1 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
	TransamVariables->latestCompletedXid = TransamVariables->nextXid;
	FullTransactionIdRetreat(&TransamVariables->latestCompletedXid);
	LWLockRelease(ProcArrayLock);

	/*
	 * Start up subtrans, if not already done for hot standby.  (commit
	 * timestamps are started below, if necessary.)
	 */
	if (standbyState == STANDBY_DISABLED)
		StartupSUBTRANS(oldestActiveXID);

	/*
	 * Perform end of recovery actions for any SLRUs that need it.
	 */
	TrimCLOG();
	TrimMultiXact();

	/*
	 * Reload shared-memory state for prepared transactions.  This needs to
	 * happen before renaming the last partial segment of the old timeline as
	 * it may be possible that we have to recover some transactions from it.
	 */
	RecoverPreparedTransactions();

	/* Shut down xlogreader */
	ShutdownWalRecovery();

	/* Enable WAL writes for this backend only. */
	LocalSetXLogInsertAllowed();

	/* If necessary, write overwrite-contrecord before doing anything else */
	if (!XLogRecPtrIsInvalid(abortedRecPtr))
	{
		Assert(!XLogRecPtrIsInvalid(missingContrecPtr));
		CreateOverwriteContrecordRecord(abortedRecPtr, missingContrecPtr, newTLI);
	}

	/*
	 * Update full_page_writes in shared memory and write an XLOG_FPW_CHANGE
	 * record before resource manager writes cleanup WAL records or checkpoint
	 * record is written.
	 */
	Insert->fullPageWrites = lastFullPageWrites;
	UpdateFullPageWrites();

	/*
	 * Emit checkpoint or end-of-recovery record in XLOG, if required.
	 */
	if (performedWalRecovery)
		promoted = PerformRecoveryXLogAction();

	/*
	 * If any of the critical GUCs have changed, log them before we allow
	 * backends to write WAL.
	 */
	XLogReportParameters();

	/* If this is archive recovery, perform post-recovery cleanup actions. */
	if (ArchiveRecoveryRequested)
		CleanupAfterArchiveRecovery(EndOfLogTLI, EndOfLog, newTLI);

	/*
	 * Local WAL inserts enabled, so it's time to finish initialization of
	 * commit timestamp.
	 */
	CompleteCommitTsInitialization();

	/*
	 * All done with end-of-recovery actions.
	 *
	 * Now allow backends to write WAL and update the control file status in
	 * consequence.  SharedRecoveryState, that controls if backends can write
	 * WAL, is updated while holding ControlFileLock to prevent other backends
	 * to look at an inconsistent state of the control file in shared memory.
	 * There is still a small window during which backends can write WAL and
	 * the control file is still referring to a system not in DB_IN_PRODUCTION
	 * state while looking at the on-disk control file.
	 *
	 * Also, we use info_lck to update SharedRecoveryState to ensure that
	 * there are no race conditions concerning visibility of other recent
	 * updates to shared memory.
	 */
	LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
	ControlFile->state = DB_IN_PRODUCTION;

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
	WalSndWakeup(true, true);

	/*
	 * If this was a promotion, request an (online) checkpoint now. This isn't
	 * required for consistency, but the last restartpoint might be far back,
	 * and in case of a crash, recovering from it might take a longer than is
	 * appropriate now that we're not in standby mode anymore.
	 */
	if (promoted)
		RequestCheckpoint(CHECKPOINT_FORCE);
}

/*
 * Callback from PerformWalRecovery(), called when we switch from crash
 * recovery to archive recovery mode.  Updates the control file accordingly.
 */
void
SwitchIntoArchiveRecovery(XLogRecPtr EndRecPtr, TimeLineID replayTLI)
{
	/* initialize minRecoveryPoint to this record */
	LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
	ControlFile->state = DB_IN_ARCHIVE_RECOVERY;
	if (ControlFile->minRecoveryPoint < EndRecPtr)
	{
		ControlFile->minRecoveryPoint = EndRecPtr;
		ControlFile->minRecoveryPointTLI = replayTLI;
	}
	/* update local copy */
	LocalMinRecoveryPoint = ControlFile->minRecoveryPoint;
	LocalMinRecoveryPointTLI = ControlFile->minRecoveryPointTLI;

	/*
	 * The startup process can update its local copy of minRecoveryPoint from
	 * this point.
	 */
	updateMinRecoveryPoint = true;

	UpdateControlFile();

	/*
	 * We update SharedRecoveryState while holding the lock on ControlFileLock
	 * so both states are consistent in shared memory.
	 */
	SpinLockAcquire(&XLogCtl->info_lck);
	XLogCtl->SharedRecoveryState = RECOVERY_STATE_ARCHIVE;
	SpinLockRelease(&XLogCtl->info_lck);

	LWLockRelease(ControlFileLock);
}

/*
 * Callback from PerformWalRecovery(), called when we reach the end of backup.
 * Updates the control file accordingly.
 */
void
ReachedEndOfBackup(XLogRecPtr EndRecPtr, TimeLineID tli)
{
	/*
	 * We have reached the end of base backup, as indicated by pg_control. The
	 * data on disk is now consistent (unless minRecoveryPoint is further
	 * ahead, which can happen if we crashed during previous recovery).  Reset
	 * backupStartPoint and backupEndPoint, and update minRecoveryPoint to
	 * make sure we don't allow starting up at an earlier point even if
	 * recovery is stopped and restarted soon after this.
	 */
	LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);

	if (ControlFile->minRecoveryPoint < EndRecPtr)
	{
		ControlFile->minRecoveryPoint = EndRecPtr;
		ControlFile->minRecoveryPointTLI = tli;
	}

	ControlFile->backupStartPoint = InvalidXLogRecPtr;
	ControlFile->backupEndPoint = InvalidXLogRecPtr;
	ControlFile->backupEndRequired = false;
	UpdateControlFile();

	LWLockRelease(ControlFileLock);
}

/*
 * Perform whatever XLOG actions are necessary at end of REDO.
 *
 * The goal here is to make sure that we'll be able to recover properly if
 * we crash again. If we choose to write a checkpoint, we'll write a shutdown
 * checkpoint rather than an on-line one. This is not particularly critical,
 * but since we may be assigning a new TLI, using a shutdown checkpoint allows
 * us to have the rule that TLI only changes in shutdown checkpoints, which
 * allows some extra error checking in xlog_redo.
 */
static bool
PerformRecoveryXLogAction(void)
{
	bool		promoted = false;

	/*
	 * Perform a checkpoint to update all our recovery activity to disk.
	 *
	 * Note that we write a shutdown checkpoint rather than an on-line one.
	 * This is not particularly critical, but since we may be assigning a new
	 * TLI, using a shutdown checkpoint allows us to have the rule that TLI
	 * only changes in shutdown checkpoints, which allows some extra error
	 * checking in xlog_redo.
	 *
	 * In promotion, only create a lightweight end-of-recovery record instead
	 * of a full checkpoint. A checkpoint is requested later, after we're
	 * fully out of recovery mode and already accepting queries.
	 */
	if (ArchiveRecoveryRequested && IsUnderPostmaster &&
		PromoteIsTriggered())
	{
		promoted = true;

		/*
		 * Insert a special WAL record to mark the end of recovery, since we
		 * aren't doing a checkpoint. That means that the checkpointer process
		 * may likely be in the middle of a time-smoothed restartpoint and
		 * could continue to be for minutes after this.  That sounds strange,
		 * but the effect is roughly the same and it would be stranger to try
		 * to come out of the restartpoint and then checkpoint. We request a
		 * checkpoint later anyway, just for safety.
		 */
		CreateEndOfRecoveryRecord();
	}
	else
	{
		RequestCheckpoint(CHECKPOINT_END_OF_RECOVERY |
						  CHECKPOINT_FAST |
						  CHECKPOINT_WAIT);
	}

	return promoted;
}

/*
 * Is the system still in recovery?
 *
 * Unlike testing InRecovery, this works in any process that's connected to
 * shared memory.
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
 *
 * Returns the previous value of LocalXLogInsertAllowed.
 */
static int
LocalSetXLogInsertAllowed(void)
{
	int			oldXLogAllowed = LocalXLogInsertAllowed;

	LocalXLogInsertAllowed = 1;

	return oldXLogAllowed;
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
	 * grabbed a WAL insertion lock to read the authoritative value in
	 * Insert->RedoRecPtr, someone might update it just after we've released
	 * the lock.
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
 * possibly out-of-date or, indeed, uninitialized, in which case they will
 * be InvalidXLogRecPtr and false, respectively.  XLogInsertRecord will
 * re-check them against up-to-date values, while holding the WAL insert lock.
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
 * position known to be fsync'd to disk. This should only be used on a
 * system that is known not to be in recovery.
 */
XLogRecPtr
GetFlushRecPtr(TimeLineID *insertTLI)
{
	Assert(XLogCtl->SharedRecoveryState == RECOVERY_STATE_DONE);

	RefreshXLogWriteResult(LogwrtResult);

	/*
	 * If we're writing and flushing WAL, the time line can't be changing, so
	 * no lock is required.
	 */
	if (insertTLI)
		*insertTLI = XLogCtl->InsertTimeLineID;

	return LogwrtResult.Flush;
}

/*
 * GetWALInsertionTimeLine -- Returns the current timeline of a system that
 * is not in recovery.
 */
TimeLineID
GetWALInsertionTimeLine(void)
{
	Assert(XLogCtl->SharedRecoveryState == RECOVERY_STATE_DONE);

	/* Since the value can't be changing, no lock is required. */
	return XLogCtl->InsertTimeLineID;
}

/*
 * GetWALInsertionTimeLineIfSet -- If the system is not in recovery, returns
 * the WAL insertion timeline; else, returns 0. Wherever possible, use
 * GetWALInsertionTimeLine() instead, since it's cheaper. Note that this
 * function decides recovery has ended as soon as the insert TLI is set, which
 * happens before we set XLogCtl->SharedRecoveryState to RECOVERY_STATE_DONE.
 */
TimeLineID
GetWALInsertionTimeLineIfSet(void)
{
	TimeLineID	insertTLI;

	SpinLockAcquire(&XLogCtl->info_lck);
	insertTLI = XLogCtl->InsertTimeLineID;
	SpinLockRelease(&XLogCtl->info_lck);

	return insertTLI;
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
		CreateRestartPoint(CHECKPOINT_IS_SHUTDOWN | CHECKPOINT_FAST);
	else
	{
		/*
		 * If archiving is enabled, rotate the last XLOG file so that all the
		 * remaining records are archived (postmaster wakes up the archiver
		 * process one more time at the end of shutdown). The checkpoint
		 * record will go to the next XLOG file and won't be archived (yet).
		 */
		if (XLogArchivingActive())
			RequestXLogSwitch(false);

		CreateCheckPoint(CHECKPOINT_IS_SHUTDOWN | CHECKPOINT_FAST);
	}
}

/*
 * Log start of a checkpoint.
 */
static void
LogCheckpointStart(int flags, bool restartpoint)
{
	if (restartpoint)
		ereport(LOG,
		/* translator: the placeholders show checkpoint options */
				(errmsg("restartpoint starting:%s%s%s%s%s%s%s%s",
						(flags & CHECKPOINT_IS_SHUTDOWN) ? " shutdown" : "",
						(flags & CHECKPOINT_END_OF_RECOVERY) ? " end-of-recovery" : "",
						(flags & CHECKPOINT_FAST) ? " fast" : "",
						(flags & CHECKPOINT_FORCE) ? " force" : "",
						(flags & CHECKPOINT_WAIT) ? " wait" : "",
						(flags & CHECKPOINT_CAUSE_XLOG) ? " wal" : "",
						(flags & CHECKPOINT_CAUSE_TIME) ? " time" : "",
						(flags & CHECKPOINT_FLUSH_UNLOGGED) ? " flush-unlogged" : "")));
	else
		ereport(LOG,
		/* translator: the placeholders show checkpoint options */
				(errmsg("checkpoint starting:%s%s%s%s%s%s%s%s",
						(flags & CHECKPOINT_IS_SHUTDOWN) ? " shutdown" : "",
						(flags & CHECKPOINT_END_OF_RECOVERY) ? " end-of-recovery" : "",
						(flags & CHECKPOINT_FAST) ? " fast" : "",
						(flags & CHECKPOINT_FORCE) ? " force" : "",
						(flags & CHECKPOINT_WAIT) ? " wait" : "",
						(flags & CHECKPOINT_CAUSE_XLOG) ? " wal" : "",
						(flags & CHECKPOINT_CAUSE_TIME) ? " time" : "",
						(flags & CHECKPOINT_FLUSH_UNLOGGED) ? " flush-unlogged" : "")));
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
	PendingCheckpointerStats.write_time += write_msecs;
	PendingCheckpointerStats.sync_time += sync_msecs;

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

	/*
	 * ControlFileLock is not required to see ControlFile->checkPoint and
	 * ->checkPointCopy here as we are the only updator of those variables at
	 * this moment.
	 */
	if (restartpoint)
		ereport(LOG,
				(errmsg("restartpoint complete: wrote %d buffers (%.1f%%), "
						"wrote %d SLRU buffers; %d WAL file(s) added, "
						"%d removed, %d recycled; write=%ld.%03d s, "
						"sync=%ld.%03d s, total=%ld.%03d s; sync files=%d, "
						"longest=%ld.%03d s, average=%ld.%03d s; distance=%d kB, "
						"estimate=%d kB; lsn=%X/%08X, redo lsn=%X/%08X",
						CheckpointStats.ckpt_bufs_written,
						(double) CheckpointStats.ckpt_bufs_written * 100 / NBuffers,
						CheckpointStats.ckpt_slru_written,
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
						(int) (CheckPointDistanceEstimate / 1024.0),
						LSN_FORMAT_ARGS(ControlFile->checkPoint),
						LSN_FORMAT_ARGS(ControlFile->checkPointCopy.redo))));
	else
		ereport(LOG,
				(errmsg("checkpoint complete: wrote %d buffers (%.1f%%), "
						"wrote %d SLRU buffers; %d WAL file(s) added, "
						"%d removed, %d recycled; write=%ld.%03d s, "
						"sync=%ld.%03d s, total=%ld.%03d s; sync files=%d, "
						"longest=%ld.%03d s, average=%ld.%03d s; distance=%d kB, "
						"estimate=%d kB; lsn=%X/%08X, redo lsn=%X/%08X",
						CheckpointStats.ckpt_bufs_written,
						(double) CheckpointStats.ckpt_bufs_written * 100 / NBuffers,
						CheckpointStats.ckpt_slru_written,
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
						(int) (CheckPointDistanceEstimate / 1024.0),
						LSN_FORMAT_ARGS(ControlFile->checkPoint),
						LSN_FORMAT_ARGS(ControlFile->checkPointCopy.redo))));
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
 * Update the ps display for a process running a checkpoint.  Note that
 * this routine should not do any allocations so as it can be called
 * from a critical section.
 */
static void
update_checkpoint_display(int flags, bool restartpoint, bool reset)
{
	/*
	 * The status is reported only for end-of-recovery and shutdown
	 * checkpoints or shutdown restartpoints.  Updating the ps display is
	 * useful in those situations as it may not be possible to rely on
	 * pg_stat_activity to see the status of the checkpointer or the startup
	 * process.
	 */
	if ((flags & (CHECKPOINT_END_OF_RECOVERY | CHECKPOINT_IS_SHUTDOWN)) == 0)
		return;

	if (reset)
		set_ps_display("");
	else
	{
		char		activitymsg[128];

		snprintf(activitymsg, sizeof(activitymsg), "performing %s%s%s",
				 (flags & CHECKPOINT_END_OF_RECOVERY) ? "end-of-recovery " : "",
				 (flags & CHECKPOINT_IS_SHUTDOWN) ? "shutdown " : "",
				 restartpoint ? "restartpoint" : "checkpoint");
		set_ps_display(activitymsg);
	}
}


/*
 * Perform a checkpoint --- either during shutdown, or on-the-fly
 *
 * flags is a bitwise OR of the following:
 *	CHECKPOINT_IS_SHUTDOWN: checkpoint is for database shutdown.
 *	CHECKPOINT_END_OF_RECOVERY: checkpoint is for end of WAL recovery.
 *	CHECKPOINT_FAST: finish the checkpoint ASAP, ignoring
 *		checkpoint_completion_target parameter.
 *	CHECKPOINT_FORCE: force a checkpoint even if no XLOG activity has occurred
 *		since the last one (implied by CHECKPOINT_IS_SHUTDOWN or
 *		CHECKPOINT_END_OF_RECOVERY).
 *	CHECKPOINT_FLUSH_UNLOGGED: also flush buffers of unlogged tables.
 *
 * Note: flags contains other bits, of interest here only for logging purposes.
 * In particular note that this routine is synchronous and does not pay
 * attention to CHECKPOINT_WAIT.
 *
 * If !shutdown then we are writing an online checkpoint. An XLOG_CHECKPOINT_REDO
 * record is inserted into WAL at the logical location of the checkpoint, before
 * flushing anything to disk, and when the checkpoint is eventually completed,
 * and it is from this point that WAL replay will begin in the case of a recovery
 * from this checkpoint. Once everything is written to disk, an
 * XLOG_CHECKPOINT_ONLINE record is written to complete the checkpoint, and
 * points back to the earlier XLOG_CHECKPOINT_REDO record. This mechanism allows
 * other write-ahead log records to be written while the checkpoint is in
 * progress, but we must be very careful about order of operations. This function
 * may take many minutes to execute on a busy system.
 *
 * On the other hand, when shutdown is true, concurrent insertion into the
 * write-ahead log is impossible, so there is no need for two separate records.
 * In this case, we only insert an XLOG_CHECKPOINT_SHUTDOWN record, and it's
 * both the record marking the completion of the checkpoint and the location
 * from which WAL replay would begin if needed.
 *
 * Returns true if a new checkpoint was performed, or false if it was skipped
 * because the system was idle.
 */
bool
CreateCheckPoint(int flags)
{
	bool		shutdown;
	CheckPoint	checkPoint;
	XLogRecPtr	recptr;
	XLogSegNo	_logSegNo;
	XLogCtlInsert *Insert = &XLogCtl->Insert;
	uint32		freespace;
	XLogRecPtr	PriorRedoPtr;
	XLogRecPtr	last_important_lsn;
	VirtualTransactionId *vxids;
	int			nvxids;
	int			oldXLogAllowed = 0;

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
	 * If this isn't a shutdown or forced checkpoint, and if there has been no
	 * WAL activity requiring a checkpoint, skip it.  The idea here is to
	 * avoid inserting duplicate checkpoints when the system is idle.
	 */
	if ((flags & (CHECKPOINT_IS_SHUTDOWN | CHECKPOINT_END_OF_RECOVERY |
				  CHECKPOINT_FORCE)) == 0)
	{
		if (last_important_lsn == ControlFile->checkPoint)
		{
			END_CRIT_SECTION();
			ereport(DEBUG1,
					(errmsg_internal("checkpoint skipped because system is idle")));
			return false;
		}
	}

	/*
	 * An end-of-recovery checkpoint is created before anyone is allowed to
	 * write WAL. To allow us to write the checkpoint record, temporarily
	 * enable XLogInsertAllowed.
	 */
	if (flags & CHECKPOINT_END_OF_RECOVERY)
		oldXLogAllowed = LocalSetXLogInsertAllowed();

	checkPoint.ThisTimeLineID = XLogCtl->InsertTimeLineID;
	if (flags & CHECKPOINT_END_OF_RECOVERY)
		checkPoint.PrevTimeLineID = XLogCtl->PrevTimeLineID;
	else
		checkPoint.PrevTimeLineID = checkPoint.ThisTimeLineID;

	/*
	 * We must block concurrent insertions while examining insert state.
	 */
	WALInsertLockAcquireExclusive();

	checkPoint.fullPageWrites = Insert->fullPageWrites;
	checkPoint.wal_level = wal_level;

	if (shutdown)
	{
		XLogRecPtr	curInsert = XLogBytePosToRecPtr(Insert->CurrBytePos);

		/*
		 * Compute new REDO record ptr = location of next XLOG record.
		 *
		 * Since this is a shutdown checkpoint, there can't be any concurrent
		 * WAL insertion.
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
		 * Here we update the shared RedoRecPtr for future XLogInsert calls;
		 * this must be done while holding all the insertion locks.
		 *
		 * Note: if we fail to complete the checkpoint, RedoRecPtr will be
		 * left pointing past where it really needs to point.  This is okay;
		 * the only consequence is that XLogInsert might back up whole buffers
		 * that it didn't really need to.  We can't postpone advancing
		 * RedoRecPtr because XLogInserts that happen while we are dumping
		 * buffers must assume that their buffer changes are not included in
		 * the checkpoint.
		 */
		RedoRecPtr = XLogCtl->Insert.RedoRecPtr = checkPoint.redo;
	}

	/*
	 * Now we can release the WAL insertion locks, allowing other xacts to
	 * proceed while we are flushing disk buffers.
	 */
	WALInsertLockRelease();

	/*
	 * If this is an online checkpoint, we have not yet determined the redo
	 * point. We do so now by inserting the special XLOG_CHECKPOINT_REDO
	 * record; the LSN at which it starts becomes the new redo pointer. We
	 * don't do this for a shutdown checkpoint, because in that case no WAL
	 * can be written between the redo point and the insertion of the
	 * checkpoint record itself, so the checkpoint record itself serves to
	 * mark the redo point.
	 */
	if (!shutdown)
	{
		/* Include WAL level in record for WAL summarizer's benefit. */
		XLogBeginInsert();
		XLogRegisterData(&wal_level, sizeof(wal_level));
		(void) XLogInsert(RM_XLOG_ID, XLOG_CHECKPOINT_REDO);

		/*
		 * XLogInsertRecord will have updated XLogCtl->Insert.RedoRecPtr in
		 * shared memory and RedoRecPtr in backend-local memory, but we need
		 * to copy that into the record that will be inserted when the
		 * checkpoint is complete.
		 */
		checkPoint.redo = RedoRecPtr;
	}

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

	/* Update the process title */
	update_checkpoint_display(flags, false, false);

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
	checkPoint.nextXid = TransamVariables->nextXid;
	checkPoint.oldestXid = TransamVariables->oldestXid;
	checkPoint.oldestXidDB = TransamVariables->oldestXidDB;
	LWLockRelease(XidGenLock);

	LWLockAcquire(CommitTsLock, LW_SHARED);
	checkPoint.oldestCommitTsXid = TransamVariables->oldestCommitTsXid;
	checkPoint.newestCommitTsXid = TransamVariables->newestCommitTsXid;
	LWLockRelease(CommitTsLock);

	LWLockAcquire(OidGenLock, LW_SHARED);
	checkPoint.nextOid = TransamVariables->nextOid;
	if (!shutdown)
		checkPoint.nextOid += TransamVariables->oidCount;
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
	 * A transaction that has not yet set delayChkptFlags when we look cannot
	 * be at risk, since it has not inserted its commit record yet; and one
	 * that's already cleared it is not at risk either, since it's done fixing
	 * clog and we will correctly flush the update below.  So we cannot miss
	 * any xacts we need to wait for.
	 */
	vxids = GetVirtualXIDsDelayingChkpt(&nvxids, DELAY_CHKPT_START);
	if (nvxids > 0)
	{
		do
		{
			/*
			 * Keep absorbing fsync requests while we wait. There could even
			 * be a deadlock if we don't, if the process that prevents the
			 * checkpoint is trying to add a request to the queue.
			 */
			AbsorbSyncRequests();

			pgstat_report_wait_start(WAIT_EVENT_CHECKPOINT_DELAY_START);
			pg_usleep(10000L);	/* wait for 10 msec */
			pgstat_report_wait_end();
		} while (HaveVirtualXIDsDelayingChkpt(vxids, nvxids,
											  DELAY_CHKPT_START));
	}
	pfree(vxids);

	CheckPointGuts(checkPoint.redo, flags);

	vxids = GetVirtualXIDsDelayingChkpt(&nvxids, DELAY_CHKPT_COMPLETE);
	if (nvxids > 0)
	{
		do
		{
			AbsorbSyncRequests();

			pgstat_report_wait_start(WAIT_EVENT_CHECKPOINT_DELAY_COMPLETE);
			pg_usleep(10000L);	/* wait for 10 msec */
			pgstat_report_wait_end();
		} while (HaveVirtualXIDsDelayingChkpt(vxids, nvxids,
											  DELAY_CHKPT_COMPLETE));
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
	XLogRegisterData(&checkPoint, sizeof(checkPoint));
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
			LocalXLogInsertAllowed = oldXLogAllowed;
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
	/* crash recovery should always recover to the end of WAL */
	ControlFile->minRecoveryPoint = InvalidXLogRecPtr;
	ControlFile->minRecoveryPointTLI = 0;

	/*
	 * Persist unloggedLSN value. It's reset on crash recovery, so this goes
	 * unused on non-shutdown checkpoints, but seems useful to store it always
	 * for debugging purposes.
	 */
	ControlFile->unloggedLSN = pg_atomic_read_membarrier_u64(&XLogCtl->unloggedLSN);

	UpdateControlFile();
	LWLockRelease(ControlFileLock);

	/*
	 * We are now done with critical updates; no need for system panic if we
	 * have trouble while fooling with old log segments.
	 */
	END_CRIT_SECTION();

	/*
	 * WAL summaries end when the next XLOG_CHECKPOINT_REDO or
	 * XLOG_CHECKPOINT_SHUTDOWN record is reached. This is the first point
	 * where (a) we're not inside of a critical section and (b) we can be
	 * certain that the relevant record has been flushed to disk, which must
	 * happen before it can be summarized.
	 *
	 * If this is a shutdown checkpoint, then this happens reasonably
	 * promptly: we've only just inserted and flushed the
	 * XLOG_CHECKPOINT_SHUTDOWN record. If this is not a shutdown checkpoint,
	 * then this might not be very prompt at all: the XLOG_CHECKPOINT_REDO
	 * record was written before we began flushing data to disk, and that
	 * could be many minutes ago at this point. However, we don't XLogFlush()
	 * after inserting that record, so we're not guaranteed that it's on disk
	 * until after the above call that flushes the XLOG_CHECKPOINT_ONLINE
	 * record.
	 */
	WakeupWalSummarizer();

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

#ifdef USE_INJECTION_POINTS
	INJECTION_POINT("checkpoint-before-old-wal-removal", NULL);
#endif

	/*
	 * Delete old log files, those no longer needed for last checkpoint to
	 * prevent the disk holding the xlog from growing full.
	 */
	XLByteToSeg(RedoRecPtr, _logSegNo, wal_segment_size);
	KeepLogSeg(recptr, &_logSegNo);
	if (InvalidateObsoleteReplicationSlots(RS_INVAL_WAL_REMOVED | RS_INVAL_IDLE_TIMEOUT,
										   _logSegNo, InvalidOid,
										   InvalidTransactionId))
	{
		/*
		 * Some slots have been invalidated; recalculate the old-segment
		 * horizon, starting again from RedoRecPtr.
		 */
		XLByteToSeg(RedoRecPtr, _logSegNo, wal_segment_size);
		KeepLogSeg(recptr, &_logSegNo);
	}
	_logSegNo--;
	RemoveOldXlogFiles(_logSegNo, RedoRecPtr, recptr,
					   checkPoint.ThisTimeLineID);

	/*
	 * Make more log segments if needed.  (Do this after recycling old log
	 * segments, since that may supply some of the needed files.)
	 */
	if (!shutdown)
		PreallocXlogFiles(recptr, checkPoint.ThisTimeLineID);

	/*
	 * Truncate pg_subtrans if possible.  We can throw away all data before
	 * the oldest XMIN of any running transaction.  No future transaction will
	 * attempt to reference any pg_subtrans entry older than that (see Asserts
	 * in subtrans.c).  During recovery, though, we mustn't do this because
	 * StartupSUBTRANS hasn't been called yet.
	 */
	if (!RecoveryInProgress())
		TruncateSUBTRANS(GetOldestTransactionIdConsideredRunning());

	/* Real work is done; log and update stats. */
	LogCheckpointEnd(false);

	/* Reset the process title */
	update_checkpoint_display(flags, false, true);

	TRACE_POSTGRESQL_CHECKPOINT_DONE(CheckpointStats.ckpt_bufs_written,
									 NBuffers,
									 CheckpointStats.ckpt_segs_added,
									 CheckpointStats.ckpt_segs_removed,
									 CheckpointStats.ckpt_segs_recycled);

	return true;
}

/*
 * Mark the end of recovery in WAL though without running a full checkpoint.
 * We can expect that a restartpoint is likely to be in progress as we
 * do this, though we are unwilling to wait for it to complete.
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
	xlrec.wal_level = wal_level;

	WALInsertLockAcquireExclusive();
	xlrec.ThisTimeLineID = XLogCtl->InsertTimeLineID;
	xlrec.PrevTimeLineID = XLogCtl->PrevTimeLineID;
	WALInsertLockRelease();

	START_CRIT_SECTION();

	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xl_end_of_recovery));
	recptr = XLogInsert(RM_XLOG_ID, XLOG_END_OF_RECOVERY);

	XLogFlush(recptr);

	/*
	 * Update the control file so that crash recovery can follow the timeline
	 * changes to this point.
	 */
	LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
	ControlFile->minRecoveryPoint = recptr;
	ControlFile->minRecoveryPointTLI = xlrec.ThisTimeLineID;
	UpdateControlFile();
	LWLockRelease(ControlFileLock);

	END_CRIT_SECTION();
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
 *
 * 'aborted_lsn' is the beginning position of the record that was incomplete.
 * It is included in the WAL record.  'pagePtr' and 'newTLI' point to the
 * beginning of the XLOG page where the record is to be inserted.  They must
 * match the current WAL insert position, they're passed here just so that we
 * can verify that.
 */
static XLogRecPtr
CreateOverwriteContrecordRecord(XLogRecPtr aborted_lsn, XLogRecPtr pagePtr,
								TimeLineID newTLI)
{
	xl_overwrite_contrecord xlrec;
	XLogRecPtr	recptr;
	XLogPageHeader pagehdr;
	XLogRecPtr	startPos;

	/* sanity checks */
	if (!RecoveryInProgress())
		elog(ERROR, "can only be used at end of recovery");
	if (pagePtr % XLOG_BLCKSZ != 0)
		elog(ERROR, "invalid position for missing continuation record %X/%08X",
			 LSN_FORMAT_ARGS(pagePtr));

	/* The current WAL insert position should be right after the page header */
	startPos = pagePtr;
	if (XLogSegmentOffset(startPos, wal_segment_size) == 0)
		startPos += SizeOfXLogLongPHD;
	else
		startPos += SizeOfXLogShortPHD;
	recptr = GetXLogInsertRecPtr();
	if (recptr != startPos)
		elog(ERROR, "invalid WAL insert position %X/%08X for OVERWRITE_CONTRECORD",
			 LSN_FORMAT_ARGS(recptr));

	START_CRIT_SECTION();

	/*
	 * Initialize the XLOG page header (by GetXLogBuffer), and set the
	 * XLP_FIRST_IS_OVERWRITE_CONTRECORD flag.
	 *
	 * No other backend is allowed to write WAL yet, so acquiring the WAL
	 * insertion lock is just pro forma.
	 */
	WALInsertLockAcquire();
	pagehdr = (XLogPageHeader) GetXLogBuffer(pagePtr, newTLI);
	pagehdr->xlp_info |= XLP_FIRST_IS_OVERWRITE_CONTRECORD;
	WALInsertLockRelease();

	/*
	 * Insert the XLOG_OVERWRITE_CONTRECORD record as the first record on the
	 * page.  We know it becomes the first record, because no other backend is
	 * allowed to write WAL yet.
	 */
	XLogBeginInsert();
	xlrec.overwritten_lsn = aborted_lsn;
	xlrec.overwrite_time = GetCurrentTimestamp();
	XLogRegisterData(&xlrec, sizeof(xl_overwrite_contrecord));
	recptr = XLogInsert(RM_XLOG_ID, XLOG_OVERWRITE_CONTRECORD);

	/* check that the record was inserted to the right place */
	if (ProcLastRecPtr != startPos)
		elog(ERROR, "OVERWRITE_CONTRECORD was inserted to unexpected position %X/%08X",
			 LSN_FORMAT_ARGS(ProcLastRecPtr));

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
	CheckPointRelationMap();
	CheckPointReplicationSlots(flags & CHECKPOINT_IS_SHUTDOWN);
	CheckPointSnapBuild();
	CheckPointLogicalRewriteHeap();
	CheckPointReplicationOrigin();

	/* Write out all dirty data in SLRUs and the main buffer pool */
	TRACE_POSTGRESQL_BUFFER_CHECKPOINT_START(flags);
	CheckpointStats.ckpt_write_t = GetCurrentTimestamp();
	CheckPointCLOG();
	CheckPointCommitTs();
	CheckPointSUBTRANS();
	CheckPointMultiXact();
	CheckPointPredicate();
	CheckPointBuffers(flags);

	/* Perform all queued up fsyncs */
	TRACE_POSTGRESQL_BUFFER_CHECKPOINT_SYNC_START();
	CheckpointStats.ckpt_sync_t = GetCurrentTimestamp();
	ProcessSyncRequests();
	CheckpointStats.ckpt_sync_end_t = GetCurrentTimestamp();
	TRACE_POSTGRESQL_BUFFER_CHECKPOINT_DONE();

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
RecoveryRestartPoint(const CheckPoint *checkPoint, XLogReaderState *record)
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
		elog(DEBUG2,
			 "could not record restart point at %X/%08X because there are unresolved references to invalid pages",
			 LSN_FORMAT_ARGS(checkPoint->redo));
		return;
	}

	/*
	 * Copy the checkpoint record to shared memory, so that checkpointer can
	 * work out the next time it wants to perform a restartpoint.
	 */
	SpinLockAcquire(&XLogCtl->info_lck);
	XLogCtl->lastCheckPointRecPtr = record->ReadRecPtr;
	XLogCtl->lastCheckPointEndPtr = record->EndRecPtr;
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

	/* Concurrent checkpoint/restartpoint cannot happen */
	Assert(!IsUnderPostmaster || MyBackendType == B_CHECKPOINTER);

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
				(errmsg_internal("skipping restartpoint, recovery has already ended")));
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
				errmsg_internal("skipping restartpoint, already performed at %X/%08X",
								LSN_FORMAT_ARGS(lastCheckPoint.redo)));

		UpdateMinRecoveryPoint(InvalidXLogRecPtr, true);
		if (flags & CHECKPOINT_IS_SHUTDOWN)
		{
			LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
			ControlFile->state = DB_SHUTDOWNED_IN_RECOVERY;
			UpdateControlFile();
			LWLockRelease(ControlFileLock);
		}
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

	/* Update the process title */
	update_checkpoint_display(flags, true, false);

	CheckPointGuts(lastCheckPoint.redo, flags);

	/*
	 * This location needs to be after CheckPointGuts() to ensure that some
	 * work has already happened during this checkpoint.
	 */
	INJECTION_POINT("create-restart-point", NULL);

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

		/*
		 * Ensure minRecoveryPoint is past the checkpoint record and update it
		 * if the control file still shows DB_IN_ARCHIVE_RECOVERY.  Normally,
		 * this will have happened already while writing out dirty buffers,
		 * but not necessarily - e.g. because no buffers were dirtied.  We do
		 * this because a backup performed in recovery uses minRecoveryPoint
		 * to determine which WAL files must be included in the backup, and
		 * the file (or files) containing the checkpoint record must be
		 * included, at a minimum.  Note that for an ordinary restart of
		 * recovery there's no value in having the minimum recovery point any
		 * earlier than this anyway, because redo will begin just after the
		 * checkpoint record.
		 */
		if (ControlFile->state == DB_IN_ARCHIVE_RECOVERY)
		{
			if (ControlFile->minRecoveryPoint < lastCheckPointEndPtr)
			{
				ControlFile->minRecoveryPoint = lastCheckPointEndPtr;
				ControlFile->minRecoveryPointTLI = lastCheckPoint.ThisTimeLineID;

				/* update local copy */
				LocalMinRecoveryPoint = ControlFile->minRecoveryPoint;
				LocalMinRecoveryPointTLI = ControlFile->minRecoveryPointTLI;
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
	if (InvalidateObsoleteReplicationSlots(RS_INVAL_WAL_REMOVED | RS_INVAL_IDLE_TIMEOUT,
										   _logSegNo, InvalidOid,
										   InvalidTransactionId))
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
	 * at end of recovery.  If we're still in recovery, use the timeline we're
	 * currently replaying.
	 *
	 * There is no guarantee that the WAL segments will be useful on the
	 * current timeline; if recovery proceeds to a new timeline right after
	 * this, the pre-allocated WAL segments on this timeline will not be used,
	 * and will go wasted until recycled on the next restartpoint. We'll live
	 * with that.
	 */
	if (!RecoveryInProgress())
		replayTLI = XLogCtl->InsertTimeLineID;

	RemoveOldXlogFiles(_logSegNo, RedoRecPtr, endptr, replayTLI);

	/*
	 * Make more log segments if needed.  (Do this after recycling old log
	 * segments, since that may supply some of the needed files.)
	 */
	PreallocXlogFiles(endptr, replayTLI);

	/*
	 * Truncate pg_subtrans if possible.  We can throw away all data before
	 * the oldest XMIN of any running transaction.  No future transaction will
	 * attempt to reference any pg_subtrans entry older than that (see Asserts
	 * in subtrans.c).  When hot standby is disabled, though, we mustn't do
	 * this because StartupSUBTRANS hasn't been called yet.
	 */
	if (EnableHotStandby)
		TruncateSUBTRANS(GetOldestTransactionIdConsideredRunning());

	/* Real work is done; log and update stats. */
	LogCheckpointEnd(true);

	/* Reset the process title */
	update_checkpoint_display(flags, true, true);

	xtime = GetLatestXTime();
	ereport((log_checkpoints ? LOG : DEBUG2),
			errmsg("recovery restart point at %X/%08X",
				   LSN_FORMAT_ARGS(lastCheckPoint.redo)),
			xtime ? errdetail("Last completed transaction was at log time %s.",
							  timestamptz_to_str(xtime)) : 0);

	/*
	 * Finally, execute archive_cleanup_command, if any.
	 */
	if (archiveCleanupCommand && strcmp(archiveCleanupCommand, "") != 0)
		ExecuteRecoveryCommand(archiveCleanupCommand,
							   "archive_cleanup_command",
							   false,
							   WAIT_EVENT_ARCHIVE_CLEANUP_COMMAND);

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
 *   a slot with this LSN cannot continue.  (Any associated walsender
 *   processes should have been terminated already.)
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

	/* Calculate how many segments are kept by slots. */
	keep = XLogGetReplicationSlotMinimumLSN();
	if (keep != InvalidXLogRecPtr && keep < recptr)
	{
		XLByteToSeg(keep, segno, wal_segment_size);

		/*
		 * Account for max_slot_wal_keep_size to avoid keeping more than
		 * configured.  However, don't do that during a binary upgrade: if
		 * slots were to be invalidated because of this, it would not be
		 * possible to preserve logical ones during the upgrade.
		 */
		if (max_slot_wal_keep_size_mb >= 0 && !IsBinaryUpgrade)
		{
			uint64		slot_keep_segs;

			slot_keep_segs =
				ConvertToXSegs(max_slot_wal_keep_size_mb, wal_segment_size);

			if (currSegNo - segno > slot_keep_segs)
				segno = currSegNo - slot_keep_segs;
		}
	}

	/*
	 * If WAL summarization is in use, don't remove WAL that has yet to be
	 * summarized.
	 */
	keep = GetOldestUnsummarizedLSN(NULL, NULL);
	if (keep != InvalidXLogRecPtr)
	{
		XLogSegNo	unsummarized_segno;

		XLByteToSeg(keep, unsummarized_segno, wal_segment_size);
		if (unsummarized_segno < segno)
			segno = unsummarized_segno;
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
	XLogRegisterData(&nextOid, sizeof(Oid));
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
	XLogRegisterData(&xlrec, sizeof(xl_restore_point));

	RecPtr = XLogInsert(RM_XLOG_ID, XLOG_RESTORE_POINT);

	ereport(LOG,
			errmsg("restore point \"%s\" created at %X/%08X",
				   rpName, LSN_FORMAT_ARGS(RecPtr)));

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
			XLogRegisterData(&xlrec, sizeof(xlrec));

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
		XLogRegisterData(&fullPageWrites, sizeof(bool));

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
 * XLOG resource manager's routines
 *
 * Definitions of info values are in include/catalog/pg_control.h, though
 * not all record types are related to control file updates.
 *
 * NOTE: Some XLOG record types that are directly related to WAL recovery
 * are handled in xlogrecovery_redo().
 */
void
xlog_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	XLogRecPtr	lsn = record->EndRecPtr;

	/*
	 * In XLOG rmgr, backup blocks are only used by XLOG_FPI and
	 * XLOG_FPI_FOR_HINT records.
	 */
	Assert(info == XLOG_FPI || info == XLOG_FPI_FOR_HINT ||
		   !XLogRecHasAnyBlockRefs(record));

	if (info == XLOG_NEXTOID)
	{
		Oid			nextOid;

		/*
		 * We used to try to take the maximum of TransamVariables->nextOid and
		 * the recorded nextOid, but that fails if the OID counter wraps
		 * around.  Since no OID allocation should be happening during replay
		 * anyway, better to just believe the record exactly.  We still take
		 * OidGenLock while setting the variable, just in case.
		 */
		memcpy(&nextOid, XLogRecGetData(record), sizeof(Oid));
		LWLockAcquire(OidGenLock, LW_EXCLUSIVE);
		TransamVariables->nextOid = nextOid;
		TransamVariables->oidCount = 0;
		LWLockRelease(OidGenLock);
	}
	else if (info == XLOG_CHECKPOINT_SHUTDOWN)
	{
		CheckPoint	checkPoint;
		TimeLineID	replayTLI;

		memcpy(&checkPoint, XLogRecGetData(record), sizeof(CheckPoint));
		/* In a SHUTDOWN checkpoint, believe the counters exactly */
		LWLockAcquire(XidGenLock, LW_EXCLUSIVE);
		TransamVariables->nextXid = checkPoint.nextXid;
		LWLockRelease(XidGenLock);
		LWLockAcquire(OidGenLock, LW_EXCLUSIVE);
		TransamVariables->nextOid = checkPoint.nextOid;
		TransamVariables->oidCount = 0;
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
		 * on the primary at this point. So fake-up an empty running-xacts
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

			/* Update pg_subtrans entries for any prepared transactions */
			StandbyRecoverPreparedTransactions();

			/*
			 * Construct a RunningTransactions snapshot representing a shut
			 * down server, with only prepared transactions still alive. We're
			 * never overflowed at this point because all subxids are listed
			 * with their parent prepared transactions.
			 */
			running.xcnt = nxids;
			running.subxcnt = 0;
			running.subxid_status = SUBXIDS_IN_SUBTRANS;
			running.nextXid = XidFromFullTransactionId(checkPoint.nextXid);
			running.oldestRunningXid = oldestActiveXID;
			latestCompletedXid = XidFromFullTransactionId(checkPoint.nextXid);
			TransactionIdRetreat(latestCompletedXid);
			Assert(TransactionIdIsNormal(latestCompletedXid));
			running.latestCompletedXid = latestCompletedXid;
			running.xids = xids;

			ProcArrayApplyRecoveryInfo(&running);
		}

		/* ControlFile->checkPointCopy always tracks the latest ckpt XID */
		LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
		ControlFile->checkPointCopy.nextXid = checkPoint.nextXid;
		LWLockRelease(ControlFileLock);

		/*
		 * We should've already switched to the new TLI before replaying this
		 * record.
		 */
		(void) GetCurrentReplayRecPtr(&replayTLI);
		if (checkPoint.ThisTimeLineID != replayTLI)
			ereport(PANIC,
					(errmsg("unexpected timeline ID %u (should be %u) in shutdown checkpoint record",
							checkPoint.ThisTimeLineID, replayTLI)));

		RecoveryRestartPoint(&checkPoint, record);
	}
	else if (info == XLOG_CHECKPOINT_ONLINE)
	{
		CheckPoint	checkPoint;
		TimeLineID	replayTLI;

		memcpy(&checkPoint, XLogRecGetData(record), sizeof(CheckPoint));
		/* In an ONLINE checkpoint, treat the XID counter as a minimum */
		LWLockAcquire(XidGenLock, LW_EXCLUSIVE);
		if (FullTransactionIdPrecedes(TransamVariables->nextXid,
									  checkPoint.nextXid))
			TransamVariables->nextXid = checkPoint.nextXid;
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
		if (TransactionIdPrecedes(TransamVariables->oldestXid,
								  checkPoint.oldestXid))
			SetTransactionIdLimit(checkPoint.oldestXid,
								  checkPoint.oldestXidDB);
		/* ControlFile->checkPointCopy always tracks the latest ckpt XID */
		LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
		ControlFile->checkPointCopy.nextXid = checkPoint.nextXid;
		LWLockRelease(ControlFileLock);

		/* TLI should not change in an on-line checkpoint */
		(void) GetCurrentReplayRecPtr(&replayTLI);
		if (checkPoint.ThisTimeLineID != replayTLI)
			ereport(PANIC,
					(errmsg("unexpected timeline ID %u (should be %u) in online checkpoint record",
							checkPoint.ThisTimeLineID, replayTLI)));

		RecoveryRestartPoint(&checkPoint, record);
	}
	else if (info == XLOG_OVERWRITE_CONTRECORD)
	{
		/* nothing to do here, handled in xlogrecovery_redo() */
	}
	else if (info == XLOG_END_OF_RECOVERY)
	{
		xl_end_of_recovery xlrec;
		TimeLineID	replayTLI;

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
		(void) GetCurrentReplayRecPtr(&replayTLI);
		if (xlrec.ThisTimeLineID != replayTLI)
			ereport(PANIC,
					(errmsg("unexpected timeline ID %u (should be %u) in end-of-recovery record",
							xlrec.ThisTimeLineID, replayTLI)));
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
		/* nothing to do here, handled in xlogrecovery.c */
	}
	else if (info == XLOG_FPI || info == XLOG_FPI_FOR_HINT)
	{
		/*
		 * XLOG_FPI records contain nothing else but one or more block
		 * references. Every block reference must include a full-page image
		 * even if full_page_writes was disabled when the record was generated
		 * - otherwise there would be no point in this record.
		 *
		 * XLOG_FPI_FOR_HINT records are generated when a page needs to be
		 * WAL-logged because of a hint bit update. They are only generated
		 * when checksums and/or wal_log_hints are enabled. They may include
		 * no full-page images if full_page_writes was disabled when they were
		 * generated. In this case there is nothing to do here.
		 *
		 * No recovery conflicts are generated by these generic records - if a
		 * resource manager needs to generate conflicts, it has to define a
		 * separate WAL record type and redo routine.
		 */
		for (uint8 block_id = 0; block_id <= XLogRecMaxBlockId(record); block_id++)
		{
			Buffer		buffer;

			if (!XLogRecHasBlockImage(record, block_id))
			{
				if (info == XLOG_FPI)
					elog(ERROR, "XLOG_FPI record did not contain a full-page image");
				continue;
			}

			if (XLogReadBufferForRedo(record, block_id, &buffer) != BLK_RESTORED)
				elog(ERROR, "unexpected XLogReadBufferForRedo result when restoring backup block");
			UnlockReleaseBuffer(buffer);
		}
	}
	else if (info == XLOG_BACKUP_END)
	{
		/* nothing to do here, handled in xlogrecovery_redo() */
	}
	else if (info == XLOG_PARAMETER_CHANGE)
	{
		xl_parameter_change xlrec;

		/* Update our copy of the parameters in pg_control */
		memcpy(&xlrec, XLogRecGetData(record), sizeof(xl_parameter_change));

		/*
		 * Invalidate logical slots if we are in hot standby and the primary
		 * does not have a WAL level sufficient for logical decoding. No need
		 * to search for potentially conflicting logically slots if standby is
		 * running with wal_level lower than logical, because in that case, we
		 * would have either disallowed creation of logical slots or
		 * invalidated existing ones.
		 */
		if (InRecovery && InHotStandby &&
			xlrec.wal_level < WAL_LEVEL_LOGICAL &&
			wal_level >= WAL_LEVEL_LOGICAL)
			InvalidateObsoleteReplicationSlots(RS_INVAL_WAL_LEVEL,
											   0, InvalidOid,
											   InvalidTransactionId);

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
			LocalMinRecoveryPoint = ControlFile->minRecoveryPoint;
			LocalMinRecoveryPointTLI = ControlFile->minRecoveryPointTLI;
		}
		if (LocalMinRecoveryPoint != InvalidXLogRecPtr && LocalMinRecoveryPoint < lsn)
		{
			TimeLineID	replayTLI;

			(void) GetCurrentReplayRecPtr(&replayTLI);
			ControlFile->minRecoveryPoint = lsn;
			ControlFile->minRecoveryPointTLI = replayTLI;
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
		 * do_pg_backup_start() and do_pg_backup_stop() can check whether
		 * full_page_writes has been disabled during online backup.
		 */
		if (!fpw)
		{
			SpinLockAcquire(&XLogCtl->info_lck);
			if (XLogCtl->lastFpwDisableRecPtr < record->ReadRecPtr)
				XLogCtl->lastFpwDisableRecPtr = record->ReadRecPtr;
			SpinLockRelease(&XLogCtl->info_lck);
		}

		/* Keep track of full_page_writes */
		lastFullPageWrites = fpw;
	}
	else if (info == XLOG_CHECKPOINT_REDO)
	{
		/* nothing to do here, just for informational purposes */
	}
}

/*
 * Return the extra open flags used for opening a file, depending on the
 * value of the GUCs wal_sync_method, fsync and debug_io_direct.
 */
static int
get_sync_bit(int method)
{
	int			o_direct_flag = 0;

	/*
	 * Use O_DIRECT if requested, except in walreceiver process.  The WAL
	 * written by walreceiver is normally read by the startup process soon
	 * after it's written.  Also, walreceiver performs unaligned writes, which
	 * don't work with O_DIRECT, so it is required for correctness too.
	 */
	if ((io_direct_flags & IO_DIRECT_WAL) && !AmWalReceiverProcess())
		o_direct_flag = PG_O_DIRECT;

	/* If fsync is disabled, never open in sync mode */
	if (!enableFsync)
		return o_direct_flag;

	switch (method)
	{
			/*
			 * enum values for all sync options are defined even if they are
			 * not supported on the current platform.  But if not, they are
			 * not included in the enum option array, and therefore will never
			 * be seen here.
			 */
		case WAL_SYNC_METHOD_FSYNC:
		case WAL_SYNC_METHOD_FSYNC_WRITETHROUGH:
		case WAL_SYNC_METHOD_FDATASYNC:
			return o_direct_flag;
#ifdef O_SYNC
		case WAL_SYNC_METHOD_OPEN:
			return O_SYNC | o_direct_flag;
#endif
#ifdef O_DSYNC
		case WAL_SYNC_METHOD_OPEN_DSYNC:
			return O_DSYNC | o_direct_flag;
#endif
		default:
			/* can't happen (unless we are out of sync with option array) */
			elog(ERROR, "unrecognized \"wal_sync_method\": %d", method);
			return 0;			/* silence warning */
	}
}

/*
 * GUC support
 */
void
assign_wal_sync_method(int new_wal_sync_method, void *extra)
{
	if (wal_sync_method != new_wal_sync_method)
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
				XLogFileName(xlogfname, openLogTLI, openLogSegNo,
							 wal_segment_size);
				errno = save_errno;
				ereport(PANIC,
						(errcode_for_file_access(),
						 errmsg("could not fsync file \"%s\": %m", xlogfname)));
			}

			pgstat_report_wait_end();
			if (get_sync_bit(wal_sync_method) != get_sync_bit(new_wal_sync_method))
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
issue_xlog_fsync(int fd, XLogSegNo segno, TimeLineID tli)
{
	char	   *msg = NULL;
	instr_time	start;

	Assert(tli != 0);

	/*
	 * Quick exit if fsync is disabled or write() has already synced the WAL
	 * file.
	 */
	if (!enableFsync ||
		wal_sync_method == WAL_SYNC_METHOD_OPEN ||
		wal_sync_method == WAL_SYNC_METHOD_OPEN_DSYNC)
		return;

	/*
	 * Measure I/O timing to sync the WAL file for pg_stat_io.
	 */
	start = pgstat_prepare_io_time(track_wal_io_timing);

	pgstat_report_wait_start(WAIT_EVENT_WAL_SYNC);
	switch (wal_sync_method)
	{
		case WAL_SYNC_METHOD_FSYNC:
			if (pg_fsync_no_writethrough(fd) != 0)
				msg = _("could not fsync file \"%s\": %m");
			break;
#ifdef HAVE_FSYNC_WRITETHROUGH
		case WAL_SYNC_METHOD_FSYNC_WRITETHROUGH:
			if (pg_fsync_writethrough(fd) != 0)
				msg = _("could not fsync write-through file \"%s\": %m");
			break;
#endif
		case WAL_SYNC_METHOD_FDATASYNC:
			if (pg_fdatasync(fd) != 0)
				msg = _("could not fdatasync file \"%s\": %m");
			break;
		case WAL_SYNC_METHOD_OPEN:
		case WAL_SYNC_METHOD_OPEN_DSYNC:
			/* not reachable */
			Assert(false);
			break;
		default:
			ereport(PANIC,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg_internal("unrecognized \"wal_sync_method\": %d", wal_sync_method));
			break;
	}

	/* PANIC if failed to fsync */
	if (msg)
	{
		char		xlogfname[MAXFNAMELEN];
		int			save_errno = errno;

		XLogFileName(xlogfname, tli, segno, wal_segment_size);
		errno = save_errno;
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg(msg, xlogfname)));
	}

	pgstat_report_wait_end();

	pgstat_count_io_op_time(IOOBJECT_WAL, IOCONTEXT_NORMAL, IOOP_FSYNC,
							start, 1, 0);
}

/*
 * do_pg_backup_start is the workhorse of the user-visible pg_backup_start()
 * function. It creates the necessary starting checkpoint and constructs the
 * backup state and tablespace map.
 *
 * Input parameters are "state" (the backup state), "fast" (if true, we do
 * the checkpoint in fast mode), and "tablespaces" (if non-NULL, indicates a
 * list of tablespaceinfo structs describing the cluster's tablespaces.).
 *
 * The tablespace map contents are appended to passed-in parameter
 * tablespace_map and the caller is responsible for including it in the backup
 * archive as 'tablespace_map'. The tablespace_map file is required mainly for
 * tar format in windows as native windows utilities are not able to create
 * symlinks while extracting files from tar. However for consistency and
 * platform-independence, we do it the same way everywhere.
 *
 * It fills in "state" with the information required for the backup, such
 * as the minimum WAL location that must be present to restore from this
 * backup (starttli) and the corresponding timeline ID (starttli).
 *
 * Every successfully started backup must be stopped by calling
 * do_pg_backup_stop() or do_pg_abort_backup(). There can be many
 * backups active at the same time.
 *
 * It is the responsibility of the caller of this function to verify the
 * permissions of the calling user!
 */
void
do_pg_backup_start(const char *backupidstr, bool fast, List **tablespaces,
				   BackupState *state, StringInfo tblspcmapfile)
{
	bool		backup_started_in_recovery;

	Assert(state != NULL);
	backup_started_in_recovery = RecoveryInProgress();

	/*
	 * During recovery, we don't need to check WAL level. Because, if WAL
	 * level is not sufficient, it's impossible to get here during recovery.
	 */
	if (!backup_started_in_recovery && !XLogIsNeeded())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("WAL level not sufficient for making an online backup"),
				 errhint("\"wal_level\" must be set to \"replica\" or \"logical\" at server start.")));

	if (strlen(backupidstr) > MAXPGPATH)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("backup label too long (max %d bytes)",
						MAXPGPATH)));

	strlcpy(state->name, backupidstr, sizeof(state->name));

	/*
	 * Mark backup active in shared memory.  We must do full-page WAL writes
	 * during an on-line backup even if not doing so at other times, because
	 * it's quite possible for the backup dump to obtain a "torn" (partially
	 * written) copy of a database page if it reads the page concurrently with
	 * our write to the same page.  This can be fixed as long as the first
	 * write to the page in the WAL sequence is a full-page write. Hence, we
	 * increment runningBackups then force a CHECKPOINT, to ensure there are
	 * no dirty pages in shared memory that might get dumped while the backup
	 * is in progress without having a corresponding WAL record.  (Once the
	 * backup is complete, we need not force full-page writes anymore, since
	 * we expect that any pages not modified during the backup interval must
	 * have been correctly captured by the backup.)
	 *
	 * Note that forcing full-page writes has no effect during an online
	 * backup from the standby.
	 *
	 * We must hold all the insertion locks to change the value of
	 * runningBackups, to ensure adequate interlocking against
	 * XLogInsertRecord().
	 */
	WALInsertLockAcquireExclusive();
	XLogCtl->Insert.runningBackups++;
	WALInsertLockRelease();

	/*
	 * Ensure we decrement runningBackups if we fail below. NB -- for this to
	 * work correctly, it is critical that sessionBackupState is only updated
	 * after this block is over.
	 */
	PG_ENSURE_ERROR_CLEANUP(do_pg_abort_backup, DatumGetBool(true));
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
		 * pg_backup_start() right after restoring from a PITR archive: the
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
			 * Since the fact that we are executing do_pg_backup_start()
			 * during recovery means that checkpointer is running, we can use
			 * RequestCheckpoint() to establish a restartpoint.
			 *
			 * We use CHECKPOINT_FAST only if requested by user (via passing
			 * fast = true).  Otherwise this can take awhile.
			 */
			RequestCheckpoint(CHECKPOINT_FORCE | CHECKPOINT_WAIT |
							  (fast ? CHECKPOINT_FAST : 0));

			/*
			 * Now we need to fetch the checkpoint record location, and also
			 * its REDO pointer.  The oldest point in WAL that would be needed
			 * to restore starting from the checkpoint is precisely the REDO
			 * pointer.
			 */
			LWLockAcquire(ControlFileLock, LW_SHARED);
			state->checkpointloc = ControlFile->checkPoint;
			state->startpoint = ControlFile->checkPointCopy.redo;
			state->starttli = ControlFile->checkPointCopy.ThisTimeLineID;
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

				if (!checkpointfpw || state->startpoint <= recptr)
					ereport(ERROR,
							(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							 errmsg("WAL generated with \"full_page_writes=off\" was replayed "
									"since last restartpoint"),
							 errhint("This means that the backup being taken on the standby "
									 "is corrupt and should not be used. "
									 "Enable \"full_page_writes\" and run CHECKPOINT on the primary, "
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
			if (XLogCtl->Insert.lastBackupStart < state->startpoint)
			{
				XLogCtl->Insert.lastBackupStart = state->startpoint;
				gotUniqueStartpoint = true;
			}
			WALInsertLockRelease();
		} while (!gotUniqueStartpoint);

		/*
		 * Construct tablespace_map file.
		 */
		datadirpathlen = strlen(DataDir);

		/* Collect information about all tablespaces */
		tblspcdir = AllocateDir(PG_TBLSPC_DIR);
		while ((de = ReadDir(tblspcdir, PG_TBLSPC_DIR)) != NULL)
		{
			char		fullpath[MAXPGPATH + sizeof(PG_TBLSPC_DIR)];
			char		linkpath[MAXPGPATH];
			char	   *relpath = NULL;
			char	   *s;
			PGFileType	de_type;
			char	   *badp;
			Oid			tsoid;

			/*
			 * Try to parse the directory name as an unsigned integer.
			 *
			 * Tablespace directories should be positive integers that can be
			 * represented in 32 bits, with no leading zeroes or trailing
			 * garbage. If we come across a name that doesn't meet those
			 * criteria, skip it.
			 */
			if (de->d_name[0] < '1' || de->d_name[1] > '9')
				continue;
			errno = 0;
			tsoid = strtoul(de->d_name, &badp, 10);
			if (*badp != '\0' || errno == EINVAL || errno == ERANGE)
				continue;

			snprintf(fullpath, sizeof(fullpath), "%s/%s", PG_TBLSPC_DIR, de->d_name);

			de_type = get_dirent_type(fullpath, de, false, ERROR);

			if (de_type == PGFILETYPE_LNK)
			{
				StringInfoData escapedpath;
				int			rllen;

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
				 * Relpath holds the relative path of the tablespace directory
				 * when it's located within PGDATA, or NULL if it's located
				 * elsewhere.
				 */
				if (rllen > datadirpathlen &&
					strncmp(linkpath, DataDir, datadirpathlen) == 0 &&
					IS_DIR_SEP(linkpath[datadirpathlen]))
					relpath = pstrdup(linkpath + datadirpathlen + 1);

				/*
				 * Add a backslash-escaped version of the link path to the
				 * tablespace map file.
				 */
				initStringInfo(&escapedpath);
				for (s = linkpath; *s; s++)
				{
					if (*s == '\n' || *s == '\r' || *s == '\\')
						appendStringInfoChar(&escapedpath, '\\');
					appendStringInfoChar(&escapedpath, *s);
				}
				appendStringInfo(tblspcmapfile, "%s %s\n",
								 de->d_name, escapedpath.data);
				pfree(escapedpath.data);
			}
			else if (de_type == PGFILETYPE_DIR)
			{
				/*
				 * It's possible to use allow_in_place_tablespaces to create
				 * directories directly under pg_tblspc, for testing purposes
				 * only.
				 *
				 * In this case, we store a relative path rather than an
				 * absolute path into the tablespaceinfo.
				 */
				snprintf(linkpath, sizeof(linkpath), "%s/%s",
						 PG_TBLSPC_DIR, de->d_name);
				relpath = pstrdup(linkpath);
			}
			else
			{
				/* Skip any other file type that appears here. */
				continue;
			}

			ti = palloc(sizeof(tablespaceinfo));
			ti->oid = tsoid;
			ti->path = pstrdup(linkpath);
			ti->rpath = relpath;
			ti->size = -1;

			if (tablespaces)
				*tablespaces = lappend(*tablespaces, ti);
		}
		FreeDir(tblspcdir);

		state->starttime = (pg_time_t) time(NULL);
	}
	PG_END_ENSURE_ERROR_CLEANUP(do_pg_abort_backup, DatumGetBool(true));

	state->started_in_recovery = backup_started_in_recovery;

	/*
	 * Mark that the start phase has correctly finished for the backup.
	 */
	sessionBackupState = SESSION_BACKUP_RUNNING;
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
 * do_pg_backup_stop
 *
 * Utility function called at the end of an online backup.  It creates history
 * file (if required), resets sessionBackupState and so on.  It can optionally
 * wait for WAL segments to be archived.
 *
 * "state" is filled with the information necessary to restore from this
 * backup with its stop LSN (stoppoint), its timeline ID (stoptli), etc.
 *
 * It is the responsibility of the caller of this function to verify the
 * permissions of the calling user!
 */
void
do_pg_backup_stop(BackupState *state, bool waitforarchive)
{
	bool		backup_stopped_in_recovery = false;
	char		histfilepath[MAXPGPATH];
	char		lastxlogfilename[MAXFNAMELEN];
	char		histfilename[MAXFNAMELEN];
	XLogSegNo	_logSegNo;
	FILE	   *fp;
	int			seconds_before_warning;
	int			waits = 0;
	bool		reported_waiting = false;

	Assert(state != NULL);

	backup_stopped_in_recovery = RecoveryInProgress();

	/*
	 * During recovery, we don't need to check WAL level. Because, if WAL
	 * level is not sufficient, it's impossible to get here during recovery.
	 */
	if (!backup_stopped_in_recovery && !XLogIsNeeded())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("WAL level not sufficient for making an online backup"),
				 errhint("\"wal_level\" must be set to \"replica\" or \"logical\" at server start.")));

	/*
	 * OK to update backup counter and session-level lock.
	 *
	 * Note that CHECK_FOR_INTERRUPTS() must not occur while updating them,
	 * otherwise they can be updated inconsistently, which might cause
	 * do_pg_abort_backup() to fail.
	 */
	WALInsertLockAcquireExclusive();

	/*
	 * It is expected that each do_pg_backup_start() call is matched by
	 * exactly one do_pg_backup_stop() call.
	 */
	Assert(XLogCtl->Insert.runningBackups > 0);
	XLogCtl->Insert.runningBackups--;

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
	 * If we are taking an online backup from the standby, we confirm that the
	 * standby has not been promoted during the backup.
	 */
	if (state->started_in_recovery && !backup_stopped_in_recovery)
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
	if (backup_stopped_in_recovery)
	{
		XLogRecPtr	recptr;

		/*
		 * Check to see if all WAL replayed during online backup contain
		 * full-page writes.
		 */
		SpinLockAcquire(&XLogCtl->info_lck);
		recptr = XLogCtl->lastFpwDisableRecPtr;
		SpinLockRelease(&XLogCtl->info_lck);

		if (state->startpoint <= recptr)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("WAL generated with \"full_page_writes=off\" was replayed "
							"during online backup"),
					 errhint("This means that the backup being taken on the standby "
							 "is corrupt and should not be used. "
							 "Enable \"full_page_writes\" and run CHECKPOINT on the primary, "
							 "and then try an online backup again.")));


		LWLockAcquire(ControlFileLock, LW_SHARED);
		state->stoppoint = ControlFile->minRecoveryPoint;
		state->stoptli = ControlFile->minRecoveryPointTLI;
		LWLockRelease(ControlFileLock);
	}
	else
	{
		char	   *history_file;

		/*
		 * Write the backup-end xlog record
		 */
		XLogBeginInsert();
		XLogRegisterData(&state->startpoint,
						 sizeof(state->startpoint));
		state->stoppoint = XLogInsert(RM_XLOG_ID, XLOG_BACKUP_END);

		/*
		 * Given that we're not in recovery, InsertTimeLineID is set and can't
		 * change, so we can read it without a lock.
		 */
		state->stoptli = XLogCtl->InsertTimeLineID;

		/*
		 * Force a switch to a new xlog segment file, so that the backup is
		 * valid as soon as archiver moves out the current segment file.
		 */
		RequestXLogSwitch(false);

		state->stoptime = (pg_time_t) time(NULL);

		/*
		 * Write the backup history file
		 */
		XLByteToSeg(state->startpoint, _logSegNo, wal_segment_size);
		BackupHistoryFilePath(histfilepath, state->stoptli, _logSegNo,
							  state->startpoint, wal_segment_size);
		fp = AllocateFile(histfilepath, "w");
		if (!fp)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create file \"%s\": %m",
							histfilepath)));

		/* Build and save the contents of the backup history file */
		history_file = build_backup_content(state, true);
		fprintf(fp, "%s", history_file);
		pfree(history_file);

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
		((!backup_stopped_in_recovery && XLogArchivingActive()) ||
		 (backup_stopped_in_recovery && XLogArchivingAlways())))
	{
		XLByteToPrevSeg(state->stoppoint, _logSegNo, wal_segment_size);
		XLogFileName(lastxlogfilename, state->stoptli, _logSegNo,
					 wal_segment_size);

		XLByteToSeg(state->startpoint, _logSegNo, wal_segment_size);
		BackupHistoryFileName(histfilename, state->stoptli, _logSegNo,
							  state->startpoint, wal_segment_size);

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

			(void) WaitLatch(MyLatch,
							 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
							 1000L,
							 WAIT_EVENT_BACKUP_WAIT_WAL_ARCHIVE);
			ResetLatch(MyLatch);

			if (++waits >= seconds_before_warning)
			{
				seconds_before_warning *= 2;	/* This wraps in >10 years... */
				ereport(WARNING,
						(errmsg("still waiting for all required WAL segments to be archived (%d seconds elapsed)",
								waits),
						 errhint("Check that your \"archive_command\" is executing properly.  "
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
}


/*
 * do_pg_abort_backup: abort a running backup
 *
 * This does just the most basic steps of do_pg_backup_stop(), by taking the
 * system out of backup mode, thus making it a lot more safe to call from
 * an error handler.
 *
 * 'arg' indicates that it's being called during backup setup; so
 * sessionBackupState has not been modified yet, but runningBackups has
 * already been incremented.  When it's false, then it's invoked as a
 * before_shmem_exit handler, and therefore we must not change state
 * unless sessionBackupState indicates that a backup is actually running.
 *
 * NB: This gets used as a PG_ENSURE_ERROR_CLEANUP callback and
 * before_shmem_exit handler, hence the odd-looking signature.
 */
void
do_pg_abort_backup(int code, Datum arg)
{
	bool		during_backup_start = DatumGetBool(arg);

	/* If called during backup start, there shouldn't be one already running */
	Assert(!during_backup_start || sessionBackupState == SESSION_BACKUP_NONE);

	if (during_backup_start || sessionBackupState != SESSION_BACKUP_NONE)
	{
		WALInsertLockAcquireExclusive();
		Assert(XLogCtl->Insert.runningBackups > 0);
		XLogCtl->Insert.runningBackups--;

		sessionBackupState = SESSION_BACKUP_NONE;
		WALInsertLockRelease();

		if (!during_backup_start)
			ereport(WARNING,
					errmsg("aborting backup due to backend exiting before pg_backup_stop was called"));
	}
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
	before_shmem_exit(do_pg_abort_backup, DatumGetBool(false));
	already_done = true;
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
	RefreshXLogWriteResult(LogwrtResult);

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

/* Thin wrapper around ShutdownWalRcv(). */
void
XLogShutdownWalRcv(void)
{
	ShutdownWalRcv();

	LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
	XLogCtl->InstallXLogFileSegmentActive = false;
	LWLockRelease(ControlFileLock);
}

/* Enable WAL file recycling and preallocation. */
void
SetInstallXLogFileSegmentActive(void)
{
	LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
	XLogCtl->InstallXLogFileSegmentActive = true;
	LWLockRelease(ControlFileLock);
}

bool
IsInstallXLogFileSegmentActive(void)
{
	bool		result;

	LWLockAcquire(ControlFileLock, LW_SHARED);
	result = XLogCtl->InstallXLogFileSegmentActive;
	LWLockRelease(ControlFileLock);

	return result;
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
