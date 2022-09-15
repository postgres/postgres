/*-------------------------------------------------------------------------
 *
 * xlogrecovery.c
 *		Functions for WAL recovery, standby mode
 *
 * This source file contains functions controlling WAL recovery.
 * InitWalRecovery() initializes the system for crash or archive recovery,
 * or standby mode, depending on configuration options and the state of
 * the control file and possible backup label file.  PerformWalRecovery()
 * performs the actual WAL replay, calling the rmgr-specific redo routines.
 * EndWalRecovery() performs end-of-recovery checks and cleanup actions,
 * and prepares information needed to initialize the WAL for writes.  In
 * addition to these three main functions, there are a bunch of functions
 * for interrogating recovery state and controlling the recovery process.
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/xlogrecovery.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "access/timeline.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "access/xlogarchive.h"
#include "access/xlogprefetcher.h"
#include "access/xlogreader.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "backup/basebackup.h"
#include "catalog/pg_control.h"
#include "commands/tablespace.h"
#include "common/file_utils.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgwriter.h"
#include "postmaster/startup.h"
#include "replication/walreceiver.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/spin.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/ps_status.h"
#include "utils/pg_rusage.h"

/* Unsupported old recovery command file names (relative to $PGDATA) */
#define RECOVERY_COMMAND_FILE	"recovery.conf"
#define RECOVERY_COMMAND_DONE	"recovery.done"

/*
 * GUC support
 */
const struct config_enum_entry recovery_target_action_options[] = {
	{"pause", RECOVERY_TARGET_ACTION_PAUSE, false},
	{"promote", RECOVERY_TARGET_ACTION_PROMOTE, false},
	{"shutdown", RECOVERY_TARGET_ACTION_SHUTDOWN, false},
	{NULL, 0, false}
};

/* options formerly taken from recovery.conf for archive recovery */
char	   *recoveryRestoreCommand = NULL;
char	   *recoveryEndCommand = NULL;
char	   *archiveCleanupCommand = NULL;
RecoveryTargetType recoveryTarget = RECOVERY_TARGET_UNSET;
bool		recoveryTargetInclusive = true;
int			recoveryTargetAction = RECOVERY_TARGET_ACTION_PAUSE;
TransactionId recoveryTargetXid;
char	   *recovery_target_time_string;
TimestampTz recoveryTargetTime;
const char *recoveryTargetName;
XLogRecPtr	recoveryTargetLSN;
int			recovery_min_apply_delay = 0;

/* options formerly taken from recovery.conf for XLOG streaming */
char	   *PrimaryConnInfo = NULL;
char	   *PrimarySlotName = NULL;
char	   *PromoteTriggerFile = NULL;
bool		wal_receiver_create_temp_slot = false;

/*
 * recoveryTargetTimeLineGoal: what the user requested, if any
 *
 * recoveryTargetTLIRequested: numeric value of requested timeline, if constant
 *
 * recoveryTargetTLI: the currently understood target timeline; changes
 *
 * expectedTLEs: a list of TimeLineHistoryEntries for recoveryTargetTLI and
 * the timelines of its known parents, newest first (so recoveryTargetTLI is
 * always the first list member).  Only these TLIs are expected to be seen in
 * the WAL segments we read, and indeed only these TLIs will be considered as
 * candidate WAL files to open at all.
 *
 * curFileTLI: the TLI appearing in the name of the current input WAL file.
 * (This is not necessarily the same as the timeline from which we are
 * replaying WAL, which StartupXLOG calls replayTLI, because we could be
 * scanning data that was copied from an ancestor timeline when the current
 * file was created.)  During a sequential scan we do not allow this value
 * to decrease.
 */
RecoveryTargetTimeLineGoal recoveryTargetTimeLineGoal = RECOVERY_TARGET_TIMELINE_LATEST;
TimeLineID	recoveryTargetTLIRequested = 0;
TimeLineID	recoveryTargetTLI = 0;
static List *expectedTLEs;
static TimeLineID curFileTLI;

/*
 * When ArchiveRecoveryRequested is set, archive recovery was requested,
 * ie. signal files were present.  When InArchiveRecovery is set, we are
 * currently recovering using offline XLOG archives.  These variables are only
 * valid in the startup process.
 *
 * When ArchiveRecoveryRequested is true, but InArchiveRecovery is false, we're
 * currently performing crash recovery using only XLOG files in pg_wal, but
 * will switch to using offline XLOG archives as soon as we reach the end of
 * WAL in pg_wal.
*/
bool		ArchiveRecoveryRequested = false;
bool		InArchiveRecovery = false;

/*
 * When StandbyModeRequested is set, standby mode was requested, i.e.
 * standby.signal file was present.  When StandbyMode is set, we are currently
 * in standby mode.  These variables are only valid in the startup process.
 * They work similarly to ArchiveRecoveryRequested and InArchiveRecovery.
 */
static bool StandbyModeRequested = false;
bool		StandbyMode = false;

/* was a signal file present at startup? */
static bool standby_signal_file_found = false;
static bool recovery_signal_file_found = false;

/*
 * CheckPointLoc is the position of the checkpoint record that determines
 * where to start the replay.  It comes from the backup label file or the
 * control file.
 *
 * RedoStartLSN is the checkpoint's REDO location, also from the backup label
 * file or the control file.  In standby mode, XLOG streaming usually starts
 * from the position where an invalid record was found.  But if we fail to
 * read even the initial checkpoint record, we use the REDO location instead
 * of the checkpoint location as the start position of XLOG streaming.
 * Otherwise we would have to jump backwards to the REDO location after
 * reading the checkpoint record, because the REDO record can precede the
 * checkpoint record.
 */
static XLogRecPtr CheckPointLoc = InvalidXLogRecPtr;
static TimeLineID CheckPointTLI = 0;
static XLogRecPtr RedoStartLSN = InvalidXLogRecPtr;
static TimeLineID RedoStartTLI = 0;

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

/* Has the recovery code requested a walreceiver wakeup? */
static bool doRequestWalReceiverReply;

/* XLogReader object used to parse the WAL records */
static XLogReaderState *xlogreader = NULL;

/* XLogPrefetcher object used to consume WAL records with read-ahead */
static XLogPrefetcher *xlogprefetcher = NULL;

/* Parameters passed down from ReadRecord to the XLogPageRead callback. */
typedef struct XLogPageReadPrivate
{
	int			emode;
	bool		fetching_ckpt;	/* are we fetching a checkpoint record? */
	bool		randAccess;
	TimeLineID	replayTLI;
} XLogPageReadPrivate;

/* flag to tell XLogPageRead that we have started replaying */
static bool InRedo = false;

/*
 * Codes indicating where we got a WAL file from during recovery, or where
 * to attempt to get one.
 */
typedef enum
{
	XLOG_FROM_ANY = 0,			/* request to read WAL from any source */
	XLOG_FROM_ARCHIVE,			/* restored using restore_command */
	XLOG_FROM_PG_WAL,			/* existing file in pg_wal */
	XLOG_FROM_STREAM			/* streamed from primary */
} XLogSource;

/* human-readable names for XLogSources, for debugging output */
static const char *const xlogSourceNames[] = {"any", "archive", "pg_wal", "stream"};

/*
 * readFile is -1 or a kernel FD for the log file segment that's currently
 * open for reading.  readSegNo identifies the segment.  readOff is the offset
 * of the page just read, readLen indicates how much of it has been read into
 * readBuf, and readSource indicates where we got the currently open file from.
 *
 * Note: we could use Reserve/ReleaseExternalFD to track consumption of this
 * FD too (like for openLogFile in xlog.c); but it doesn't currently seem
 * worthwhile, since the XLOG is not read by general-purpose sessions.
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

/* Local copy of WalRcv->flushedUpto */
static XLogRecPtr flushedUpto = 0;
static TimeLineID receiveTLI = 0;

/*
 * Copy of minRecoveryPoint and backupEndPoint from the control file.
 *
 * In order to reach consistency, we must replay the WAL up to
 * minRecoveryPoint.  If backupEndRequired is true, we must also reach
 * backupEndPoint, or if it's invalid, an end-of-backup record corresponding
 * to backupStartPoint.
 *
 * Note: In archive recovery, after consistency has been reached, the
 * functions in xlog.c will start updating minRecoveryPoint in the control
 * file.  But this copy of minRecoveryPoint variable reflects the value at the
 * beginning of recovery, and is *not* updated after consistency is reached.
 */
static XLogRecPtr minRecoveryPoint;
static TimeLineID minRecoveryPointTLI;

static XLogRecPtr backupStartPoint;
static XLogRecPtr backupEndPoint;
static bool backupEndRequired = false;

/*
 * Have we reached a consistent database state?  In crash recovery, we have
 * to replay all the WAL, so reachedConsistency is never set.  During archive
 * recovery, the database is consistent once minRecoveryPoint is reached.
 *
 * Consistent state means that the system is internally consistent, all
 * the WAL has been replayed up to a certain point, and importantly, there
 * is no trace of later actions on disk.
 */
bool		reachedConsistency = false;

/* Buffers dedicated to consistency checks of size BLCKSZ */
static char *replay_image_masked = NULL;
static char *primary_image_masked = NULL;


/*
 * Shared-memory state for WAL recovery.
 */
typedef struct XLogRecoveryCtlData
{
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
	 * recoveryWakeupLatch is used to wake up the startup process to continue
	 * WAL replay, if it is waiting for WAL to arrive or failover trigger file
	 * to appear.
	 *
	 * Note that the startup process also uses another latch, its procLatch,
	 * to wait for recovery conflict. If we get rid of recoveryWakeupLatch for
	 * signaling the startup process in favor of using its procLatch, which
	 * comports better with possible generic signal handlers using that latch.
	 * But we should not do that because the startup process doesn't assume
	 * that it's waken up by walreceiver process or SIGHUP signal handler
	 * while it's waiting for recovery conflict. The separate latches,
	 * recoveryWakeupLatch and procLatch, should be used for inter-process
	 * communication for WAL replay and recovery conflict, respectively.
	 */
	Latch		recoveryWakeupLatch;

	/*
	 * Last record successfully replayed.
	 */
	XLogRecPtr	lastReplayedReadRecPtr; /* start position */
	XLogRecPtr	lastReplayedEndRecPtr;	/* end+1 position */
	TimeLineID	lastReplayedTLI;	/* timeline */

	/*
	 * When we're currently replaying a record, ie. in a redo function,
	 * replayEndRecPtr points to the end+1 of the record being replayed,
	 * otherwise it's equal to lastReplayedEndRecPtr.
	 */
	XLogRecPtr	replayEndRecPtr;
	TimeLineID	replayEndTLI;
	/* timestamp of last COMMIT/ABORT record replayed (or being replayed) */
	TimestampTz recoveryLastXTime;

	/*
	 * timestamp of when we started replaying the current chunk of WAL data,
	 * only relevant for replication or archive recovery
	 */
	TimestampTz currentChunkStartTime;
	/* Recovery pause state */
	RecoveryPauseState recoveryPauseState;
	ConditionVariable recoveryNotPausedCV;

	slock_t		info_lck;		/* locks shared variables shown above */
} XLogRecoveryCtlData;

static XLogRecoveryCtlData *XLogRecoveryCtl = NULL;

/*
 * abortedRecPtr is the start pointer of a broken record at end of WAL when
 * recovery completes; missingContrecPtr is the location of the first
 * contrecord that went missing.  See CreateOverwriteContrecordRecord for
 * details.
 */
static XLogRecPtr abortedRecPtr;
static XLogRecPtr missingContrecPtr;

/*
 * if recoveryStopsBefore/After returns true, it saves information of the stop
 * point here
 */
static TransactionId recoveryStopXid;
static TimestampTz recoveryStopTime;
static XLogRecPtr recoveryStopLSN;
static char recoveryStopName[MAXFNAMELEN];
static bool recoveryStopAfter;

/* prototypes for local functions */
static void ApplyWalRecord(XLogReaderState *xlogreader, XLogRecord *record, TimeLineID *replayTLI);

static void readRecoverySignalFile(void);
static void validateRecoveryParameters(void);
static bool read_backup_label(XLogRecPtr *checkPointLoc,
							  TimeLineID *backupLabelTLI,
							  bool *backupEndRequired, bool *backupFromStandby);
static bool read_tablespace_map(List **tablespaces);

static void xlogrecovery_redo(XLogReaderState *record, TimeLineID replayTLI);
static void CheckRecoveryConsistency(void);
static void rm_redo_error_callback(void *arg);
#ifdef WAL_DEBUG
static void xlog_outrec(StringInfo buf, XLogReaderState *record);
#endif
static void xlog_block_info(StringInfo buf, XLogReaderState *record);
static void checkTimeLineSwitch(XLogRecPtr lsn, TimeLineID newTLI,
								TimeLineID prevTLI, TimeLineID replayTLI);
static bool getRecordTimestamp(XLogReaderState *record, TimestampTz *recordXtime);
static void verifyBackupPageConsistency(XLogReaderState *record);

static bool recoveryStopsBefore(XLogReaderState *record);
static bool recoveryStopsAfter(XLogReaderState *record);
static char *getRecoveryStopReason(void);
static void recoveryPausesHere(bool endOfRecovery);
static bool recoveryApplyDelay(XLogReaderState *record);
static void ConfirmRecoveryPaused(void);

static XLogRecord *ReadRecord(XLogPrefetcher *xlogprefetcher,
							  int emode, bool fetching_ckpt,
							  TimeLineID replayTLI);

static int	XLogPageRead(XLogReaderState *xlogreader, XLogRecPtr targetPagePtr,
						 int reqLen, XLogRecPtr targetRecPtr, char *readBuf);
static XLogPageReadResult WaitForWALToBecomeAvailable(XLogRecPtr RecPtr,
													  bool randAccess,
													  bool fetching_ckpt,
													  XLogRecPtr tliRecPtr,
													  TimeLineID replayTLI,
													  XLogRecPtr replayLSN,
													  bool nonblocking);
static int	emode_for_corrupt_record(int emode, XLogRecPtr RecPtr);
static XLogRecord *ReadCheckpointRecord(XLogPrefetcher *xlogprefetcher, XLogRecPtr RecPtr,
										int whichChkpt, bool report, TimeLineID replayTLI);
static bool rescanLatestTimeLine(TimeLineID replayTLI, XLogRecPtr replayLSN);
static int	XLogFileRead(XLogSegNo segno, int emode, TimeLineID tli,
						 XLogSource source, bool notfoundOk);
static int	XLogFileReadAnyTLI(XLogSegNo segno, int emode, XLogSource source);

static bool CheckForStandbyTrigger(void);
static void SetPromoteIsTriggered(void);
static bool HotStandbyActiveInReplay(void);

static void SetCurrentChunkStartTime(TimestampTz xtime);
static void SetLatestXTime(TimestampTz xtime);

/*
 * Initialization of shared memory for WAL recovery
 */
Size
XLogRecoveryShmemSize(void)
{
	Size		size;

	/* XLogRecoveryCtl */
	size = sizeof(XLogRecoveryCtlData);

	return size;
}

void
XLogRecoveryShmemInit(void)
{
	bool		found;

	XLogRecoveryCtl = (XLogRecoveryCtlData *)
		ShmemInitStruct("XLOG Recovery Ctl", XLogRecoveryShmemSize(), &found);
	if (found)
		return;
	memset(XLogRecoveryCtl, 0, sizeof(XLogRecoveryCtlData));

	SpinLockInit(&XLogRecoveryCtl->info_lck);
	InitSharedLatch(&XLogRecoveryCtl->recoveryWakeupLatch);
	ConditionVariableInit(&XLogRecoveryCtl->recoveryNotPausedCV);
}

/*
 * Prepare the system for WAL recovery, if needed.
 *
 * This is called by StartupXLOG() which coordinates the server startup
 * sequence.  This function analyzes the control file and the backup label
 * file, if any, and figures out whether we need to perform crash recovery or
 * archive recovery, and how far we need to replay the WAL to reach a
 * consistent state.
 *
 * This doesn't yet change the on-disk state, except for creating the symlinks
 * from table space map file if any, and for fetching WAL files needed to find
 * the checkpoint record.  On entry, the caller has already read the control
 * file into memory, and passes it as argument.  This function updates it to
 * reflect the recovery state, and the caller is expected to write it back to
 * disk does after initializing other subsystems, but before calling
 * PerformWalRecovery().
 *
 * This initializes some global variables like ArchiveModeRequested, and
 * StandbyModeRequested and InRecovery.
 */
void
InitWalRecovery(ControlFileData *ControlFile, bool *wasShutdown_ptr,
				bool *haveBackupLabel_ptr, bool *haveTblspcMap_ptr)
{
	XLogPageReadPrivate *private;
	struct stat st;
	bool		wasShutdown;
	XLogRecord *record;
	DBState		dbstate_at_startup;
	bool		haveTblspcMap = false;
	bool		haveBackupLabel = false;
	CheckPoint	checkPoint;
	bool		backupFromStandby = false;

	dbstate_at_startup = ControlFile->state;

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
							LSN_FORMAT_ARGS(recoveryTargetLSN))));
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
		OwnLatch(&XLogRecoveryCtl->recoveryWakeupLatch);

	private = palloc0(sizeof(XLogPageReadPrivate));
	xlogreader =
		XLogReaderAllocate(wal_segment_size, NULL,
						   XL_ROUTINE(.page_read = &XLogPageRead,
									  .segment_open = NULL,
									  .segment_close = wal_segment_close),
						   private);
	if (!xlogreader)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while allocating a WAL reading processor.")));
	xlogreader->system_identifier = ControlFile->system_identifier;

	/*
	 * Set the WAL decode buffer size.  This limits how far ahead we can read
	 * in the WAL.
	 */
	XLogReaderSetDecodeBuffer(xlogreader, NULL, wal_decode_buffer_size);

	/* Create a WAL prefetcher. */
	xlogprefetcher = XLogPrefetcherAllocate(xlogreader);

	/*
	 * Allocate two page buffers dedicated to WAL consistency checks.  We do
	 * it this way, rather than just making static arrays, for two reasons:
	 * (1) no need to waste the storage in most instantiations of the backend;
	 * (2) a static char array isn't guaranteed to have any particular
	 * alignment, whereas palloc() will provide MAXALIGN'd storage.
	 */
	replay_image_masked = (char *) palloc(BLCKSZ);
	primary_image_masked = (char *) palloc(BLCKSZ);

	if (read_backup_label(&CheckPointLoc, &CheckPointTLI, &backupEndRequired,
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
		record = ReadCheckpointRecord(xlogprefetcher, CheckPointLoc, 0, true,
									  CheckPointTLI);
		if (record != NULL)
		{
			memcpy(&checkPoint, XLogRecGetData(xlogreader), sizeof(CheckPoint));
			wasShutdown = ((record->xl_info & ~XLR_INFO_MASK) == XLOG_CHECKPOINT_SHUTDOWN);
			ereport(DEBUG1,
					(errmsg_internal("checkpoint record is at %X/%X",
									 LSN_FORMAT_ARGS(CheckPointLoc))));
			InRecovery = true;	/* force recovery even if SHUTDOWNED */

			/*
			 * Make sure that REDO location exists. This may not be the case
			 * if there was a crash during an online backup, which left a
			 * backup_label around that references a WAL segment that's
			 * already been archived.
			 */
			if (checkPoint.redo < CheckPointLoc)
			{
				XLogPrefetcherBeginRead(xlogprefetcher, checkPoint.redo);
				if (!ReadRecord(xlogprefetcher, LOG, false,
								checkPoint.ThisTimeLineID))
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

		/* Read the tablespace_map file if present and create symlinks. */
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

			/* tell the caller to delete it later */
			haveTblspcMap = true;
		}

		/* tell the caller to delete it later */
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
		 * pg_backup_start/stop. Or if you just kill a running primary server
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
		CheckPointLoc = ControlFile->checkPoint;
		CheckPointTLI = ControlFile->checkPointCopy.ThisTimeLineID;
		RedoStartLSN = ControlFile->checkPointCopy.redo;
		RedoStartTLI = ControlFile->checkPointCopy.ThisTimeLineID;
		record = ReadCheckpointRecord(xlogprefetcher, CheckPointLoc, 1, true,
									  CheckPointTLI);
		if (record != NULL)
		{
			ereport(DEBUG1,
					(errmsg_internal("checkpoint record is at %X/%X",
									 LSN_FORMAT_ARGS(CheckPointLoc))));
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
	 * If the location of the checkpoint record is not on the expected
	 * timeline in the history of the requested timeline, we cannot proceed:
	 * the backup is not part of the history of the requested timeline.
	 */
	Assert(expectedTLEs);		/* was initialized by reading checkpoint
								 * record */
	if (tliOfPointInHistory(CheckPointLoc, expectedTLEs) !=
		CheckPointTLI)
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
						   LSN_FORMAT_ARGS(ControlFile->checkPoint),
						   ControlFile->checkPointCopy.ThisTimeLineID,
						   LSN_FORMAT_ARGS(switchpoint))));
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
						LSN_FORMAT_ARGS(ControlFile->minRecoveryPoint),
						ControlFile->minRecoveryPointTLI)));

	ereport(DEBUG1,
			(errmsg_internal("redo record is at %X/%X; shutdown %s",
							 LSN_FORMAT_ARGS(checkPoint.redo),
							 wasShutdown ? "true" : "false")));
	ereport(DEBUG1,
			(errmsg_internal("next transaction ID: " UINT64_FORMAT "; next OID: %u",
							 U64FromFullTransactionId(checkPoint.nextXid),
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
	if (!TransactionIdIsNormal(XidFromFullTransactionId(checkPoint.nextXid)))
		ereport(PANIC,
				(errmsg("invalid next transaction ID")));

	/* sanity check */
	if (checkPoint.redo > CheckPointLoc)
		ereport(PANIC,
				(errmsg("invalid redo in checkpoint record")));

	/*
	 * Check whether we need to force recovery from WAL.  If it appears to
	 * have been a clean shutdown and we did not have a recovery signal file,
	 * then assume no recovery needed.
	 */
	if (checkPoint.redo < CheckPointLoc)
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
	 * If recovery is needed, update our in-memory copy of pg_control to show
	 * that we are recovering and to show the selected checkpoint as the place
	 * we are starting from. We also mark pg_control with any minimum recovery
	 * stop point obtained from a backup history file.
	 *
	 * We don't write the changes to disk yet, though. Only do that after
	 * initializing various subsystems.
	 */
	if (InRecovery)
	{
		if (InArchiveRecovery)
		{
			ControlFile->state = DB_IN_ARCHIVE_RECOVERY;
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
		}
		ControlFile->checkPoint = CheckPointLoc;
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
	}

	/* remember these, so that we know when we have reached consistency */
	backupStartPoint = ControlFile->backupStartPoint;
	backupEndRequired = ControlFile->backupEndRequired;
	backupEndPoint = ControlFile->backupEndPoint;
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
	 * Start recovery assuming that the final record isn't lost.
	 */
	abortedRecPtr = InvalidXLogRecPtr;
	missingContrecPtr = InvalidXLogRecPtr;

	*wasShutdown_ptr = wasShutdown;
	*haveBackupLabel_ptr = haveBackupLabel;
	*haveTblspcMap_ptr = haveTblspcMap;
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

		fd = BasicOpenFilePerm(STANDBY_SIGNAL_FILE, O_RDWR | PG_BINARY,
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

		fd = BasicOpenFilePerm(RECOVERY_SIGNAL_FILE, O_RDWR | PG_BINARY,
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
 * location and TLI into *checkPointLoc and *backupLabelTLI, respectively);
 * returns false if not. If this backup_label came from a streamed backup,
 * *backupEndRequired is set to true. If this backup_label was created during
 * recovery, *backupFromStandby is set to true.
 *
 * Also sets the global variables RedoStartLSN and RedoStartTLI with the LSN
 * and TLI read from the backup file.
 */
static bool
read_backup_label(XLogRecPtr *checkPointLoc, TimeLineID *backupLabelTLI,
				  bool *backupEndRequired, bool *backupFromStandby)
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

	/* suppress possible uninitialized-variable warnings */
	*checkPointLoc = InvalidXLogRecPtr;
	*backupLabelTLI = 0;
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
	RedoStartTLI = tli_from_walseg;
	if (fscanf(lfp, "CHECKPOINT LOCATION: %X/%X%c",
			   &hi, &lo, &ch) != 3 || ch != '\n')
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("invalid data in file \"%s\"", BACKUP_LABEL_FILE)));
	*checkPointLoc = ((uint64) hi) << 32 | lo;
	*backupLabelTLI = tli_from_walseg;

	/*
	 * BACKUP METHOD lets us know if this was a typical backup ("streamed",
	 * which could mean either pg_basebackup or the pg_backup_start/stop
	 * method was used) or if this label came from somewhere else (the only
	 * other option today being from pg_rewind).  If this was a streamed
	 * backup then we know that we need to play through until we get to the
	 * end of the WAL which was generated during the backup (at which point we
	 * will have reached consistency and backupEndRequired will be reset to be
	 * false).
	 */
	if (fscanf(lfp, "BACKUP METHOD: %19s\n", backuptype) == 1)
	{
		if (strcmp(backuptype, "streamed") == 0)
			*backupEndRequired = true;
	}

	/*
	 * BACKUP FROM lets us know if this was from a primary or a standby.  If
	 * it was from a standby, we'll double-check that the control file state
	 * matches that of a standby.
	 */
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
				(errmsg_internal("backup time %s in file \"%s\"",
								 backuptime, BACKUP_LABEL_FILE)));

	if (fscanf(lfp, "LABEL: %1023[^\n]\n", backuplabel) == 1)
		ereport(DEBUG1,
				(errmsg_internal("backup label %s in file \"%s\"",
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
				(errmsg_internal("backup timeline %u in file \"%s\"",
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
 * Returns true if a tablespace_map file was found (and fills *tablespaces
 * with a tablespaceinfo struct for each tablespace listed in the file);
 * returns false if not.
 */
static bool
read_tablespace_map(List **tablespaces)
{
	tablespaceinfo *ti;
	FILE	   *lfp;
	char		str[MAXPGPATH];
	int			ch,
				i,
				n;
	bool		was_backslash;

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
	 * the file format).  De-escape any backslashes that were inserted.
	 */
	i = 0;
	was_backslash = false;
	while ((ch = fgetc(lfp)) != EOF)
	{
		if (!was_backslash && (ch == '\n' || ch == '\r'))
		{
			if (i == 0)
				continue;		/* \r immediately followed by \n */

			/*
			 * The de-escaped line should contain an OID followed by exactly
			 * one space followed by a path.  The path might start with
			 * spaces, so don't be too liberal about parsing.
			 */
			str[i] = '\0';
			n = 0;
			while (str[n] && str[n] != ' ')
				n++;
			if (n < 1 || n >= i - 1)
				ereport(FATAL,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("invalid data in file \"%s\"", TABLESPACE_MAP)));
			str[n++] = '\0';

			ti = palloc0(sizeof(tablespaceinfo));
			ti->oid = pstrdup(str);
			ti->path = pstrdup(str + n);
			*tablespaces = lappend(*tablespaces, ti);

			i = 0;
			continue;
		}
		else if (!was_backslash && ch == '\\')
			was_backslash = true;
		else
		{
			if (i < sizeof(str) - 1)
				str[i++] = ch;
			was_backslash = false;
		}
	}

	if (i != 0 || was_backslash)	/* last line not terminated? */
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("invalid data in file \"%s\"", TABLESPACE_MAP)));

	if (ferror(lfp) || FreeFile(lfp))
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m",
						TABLESPACE_MAP)));

	return true;
}

/*
 * Finish WAL recovery.
 *
 * This does not close the 'xlogreader' yet, because in some cases the caller
 * still wants to re-read the last checkpoint record by calling
 * ReadCheckPointRecord().
 *
 * Returns the position of the last valid or applied record, after which new
 * WAL should be appended, information about why recovery was ended, and some
 * other things. See the WalRecoveryResult struct for details.
 */
EndOfWalRecoveryInfo *
FinishWalRecovery(void)
{
	EndOfWalRecoveryInfo *result = palloc(sizeof(EndOfWalRecoveryInfo));
	XLogRecPtr	lastRec;
	TimeLineID	lastRecTLI;
	XLogRecPtr	endOfLog;

	/*
	 * Kill WAL receiver, if it's still running, before we continue to write
	 * the startup checkpoint and aborted-contrecord records. It will trump
	 * over these records and subsequent ones if it's still alive when we
	 * start writing WAL.
	 */
	XLogShutdownWalRcv();

	/*
	 * We are now done reading the xlog from stream. Turn off streaming
	 * recovery to force fetching the files (which would be required at end of
	 * recovery, e.g., timeline history file) from archive or pg_wal.
	 *
	 * Note that standby mode must be turned off after killing WAL receiver,
	 * i.e., calling XLogShutdownWalRcv().
	 */
	Assert(!WalRcvStreaming());
	StandbyMode = false;

	/*
	 * Determine where to start writing WAL next.
	 *
	 * Re-fetch the last valid or last applied record, so we can identify the
	 * exact endpoint of what we consider the valid portion of WAL.  There may
	 * be an incomplete continuation record after that, in which case
	 * 'abortedRecPtr' and 'missingContrecPtr' are set and the caller will
	 * write a special OVERWRITE_CONTRECORD message to mark that the rest of
	 * it is intentionally missing.  See CreateOverwriteContrecordRecord().
	 *
	 * An important side-effect of this is to load the last page into
	 * xlogreader. The caller uses it to initialize the WAL for writing.
	 */
	if (!InRecovery)
	{
		lastRec = CheckPointLoc;
		lastRecTLI = CheckPointTLI;
	}
	else
	{
		lastRec = XLogRecoveryCtl->lastReplayedReadRecPtr;
		lastRecTLI = XLogRecoveryCtl->lastReplayedTLI;
	}
	XLogPrefetcherBeginRead(xlogprefetcher, lastRec);
	(void) ReadRecord(xlogprefetcher, PANIC, false, lastRecTLI);
	endOfLog = xlogreader->EndRecPtr;

	/*
	 * Remember the TLI in the filename of the XLOG segment containing the
	 * end-of-log.  It could be different from the timeline that endOfLog
	 * nominally belongs to, if there was a timeline switch in that segment,
	 * and we were reading the old WAL from a segment belonging to a higher
	 * timeline.
	 */
	result->endOfLogTLI = xlogreader->seg.ws_tli;

	if (ArchiveRecoveryRequested)
	{
		/*
		 * We are no longer in archive recovery state.
		 *
		 * We are now done reading the old WAL.  Turn off archive fetching if
		 * it was active.
		 */
		Assert(InArchiveRecovery);
		InArchiveRecovery = false;

		/*
		 * If the ending log segment is still open, close it (to avoid
		 * problems on Windows with trying to rename or delete an open file).
		 */
		if (readFile >= 0)
		{
			close(readFile);
			readFile = -1;
		}
	}

	/*
	 * Copy the last partial block to the caller, for initializing the WAL
	 * buffer for appending new WAL.
	 */
	if (endOfLog % XLOG_BLCKSZ != 0)
	{
		char	   *page;
		int			len;
		XLogRecPtr	pageBeginPtr;

		pageBeginPtr = endOfLog - (endOfLog % XLOG_BLCKSZ);
		Assert(readOff == XLogSegmentOffset(pageBeginPtr, wal_segment_size));

		/* Copy the valid part of the last block */
		len = endOfLog % XLOG_BLCKSZ;
		page = palloc(len);
		memcpy(page, xlogreader->readBuf, len);

		result->lastPageBeginPtr = pageBeginPtr;
		result->lastPage = page;
	}
	else
	{
		/* There is no partial block to copy. */
		result->lastPageBeginPtr = endOfLog;
		result->lastPage = NULL;
	}

	/*
	 * Create a comment for the history file to explain why and where timeline
	 * changed.
	 */
	result->recoveryStopReason = getRecoveryStopReason();

	result->lastRec = lastRec;
	result->lastRecTLI = lastRecTLI;
	result->endOfLog = endOfLog;

	result->abortedRecPtr = abortedRecPtr;
	result->missingContrecPtr = missingContrecPtr;

	result->standby_signal_file_found = standby_signal_file_found;
	result->recovery_signal_file_found = recovery_signal_file_found;

	return result;
}

/*
 * Clean up the WAL reader and leftovers from restoring WAL from archive
 */
void
ShutdownWalRecovery(void)
{
	char		recoveryPath[MAXPGPATH];

	/* Final update of pg_stat_recovery_prefetch. */
	XLogPrefetcherComputeStats(xlogprefetcher);

	/* Shut down xlogreader */
	if (readFile >= 0)
	{
		close(readFile);
		readFile = -1;
	}
	XLogReaderFree(xlogreader);
	XLogPrefetcherFree(xlogprefetcher);

	if (ArchiveRecoveryRequested)
	{
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

	/*
	 * We don't need the latch anymore. It's not strictly necessary to disown
	 * it, but let's do it for the sake of tidiness.
	 */
	if (ArchiveRecoveryRequested)
		DisownLatch(&XLogRecoveryCtl->recoveryWakeupLatch);
}

/*
 * Perform WAL recovery.
 *
 * If the system was shut down cleanly, this is never called.
 */
void
PerformWalRecovery(void)
{
	XLogRecord *record;
	bool		reachedRecoveryTarget = false;
	TimeLineID	replayTLI;

	/*
	 * Initialize shared variables for tracking progress of WAL replay, as if
	 * we had just replayed the record before the REDO location (or the
	 * checkpoint record itself, if it's a shutdown checkpoint).
	 */
	SpinLockAcquire(&XLogRecoveryCtl->info_lck);
	if (RedoStartLSN < CheckPointLoc)
	{
		XLogRecoveryCtl->lastReplayedReadRecPtr = InvalidXLogRecPtr;
		XLogRecoveryCtl->lastReplayedEndRecPtr = RedoStartLSN;
		XLogRecoveryCtl->lastReplayedTLI = RedoStartTLI;
	}
	else
	{
		XLogRecoveryCtl->lastReplayedReadRecPtr = xlogreader->ReadRecPtr;
		XLogRecoveryCtl->lastReplayedEndRecPtr = xlogreader->EndRecPtr;
		XLogRecoveryCtl->lastReplayedTLI = CheckPointTLI;
	}
	XLogRecoveryCtl->replayEndRecPtr = XLogRecoveryCtl->lastReplayedEndRecPtr;
	XLogRecoveryCtl->replayEndTLI = XLogRecoveryCtl->lastReplayedTLI;
	XLogRecoveryCtl->recoveryLastXTime = 0;
	XLogRecoveryCtl->currentChunkStartTime = 0;
	XLogRecoveryCtl->recoveryPauseState = RECOVERY_NOT_PAUSED;
	SpinLockRelease(&XLogRecoveryCtl->info_lck);

	/* Also ensure XLogReceiptTime has a sane value */
	XLogReceiptTime = GetCurrentTimestamp();

	/*
	 * Let postmaster know we've started redo now, so that it can launch the
	 * archiver if necessary.
	 */
	if (IsUnderPostmaster)
		SendPostmasterSignal(PMSIGNAL_RECOVERY_STARTED);

	/*
	 * Allow read-only connections immediately if we're consistent already.
	 */
	CheckRecoveryConsistency();

	/*
	 * Find the first record that logically follows the checkpoint --- it
	 * might physically precede it, though.
	 */
	if (RedoStartLSN < CheckPointLoc)
	{
		/* back up to find the record */
		replayTLI = RedoStartTLI;
		XLogPrefetcherBeginRead(xlogprefetcher, RedoStartLSN);
		record = ReadRecord(xlogprefetcher, PANIC, false, replayTLI);
	}
	else
	{
		/* just have to read next record after CheckPoint */
		Assert(xlogreader->ReadRecPtr == CheckPointLoc);
		replayTLI = CheckPointTLI;
		record = ReadRecord(xlogprefetcher, LOG, false, replayTLI);
	}

	if (record != NULL)
	{
		TimestampTz xtime;
		PGRUsage	ru0;

		pg_rusage_init(&ru0);

		InRedo = true;

		RmgrStartup();

		ereport(LOG,
				(errmsg("redo starts at %X/%X",
						LSN_FORMAT_ARGS(xlogreader->ReadRecPtr))));

		/* Prepare to report progress of the redo phase. */
		if (!StandbyMode)
			begin_startup_progress_phase();

		/*
		 * main redo apply loop
		 */
		do
		{
			if (!StandbyMode)
				ereport_startup_progress("redo in progress, elapsed time: %ld.%02d s, current LSN: %X/%X",
										 LSN_FORMAT_ARGS(xlogreader->ReadRecPtr));

#ifdef WAL_DEBUG
			if (XLOG_DEBUG ||
				(record->xl_rmid == RM_XACT_ID && trace_recovery_messages <= DEBUG2) ||
				(record->xl_rmid != RM_XACT_ID && trace_recovery_messages <= DEBUG3))
			{
				StringInfoData buf;

				initStringInfo(&buf);
				appendStringInfo(&buf, "REDO @ %X/%X; LSN %X/%X: ",
								 LSN_FORMAT_ARGS(xlogreader->ReadRecPtr),
								 LSN_FORMAT_ARGS(xlogreader->EndRecPtr));
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
			 * here.  We might therefore read a slightly stale value of the
			 * recoveryPause flag, but it can't be very stale (no worse than
			 * the last spinlock we did acquire).  Since a pause request is a
			 * pretty asynchronous thing anyway, possibly responding to it one
			 * WAL record later than we otherwise would is a minor issue, so
			 * it doesn't seem worth adding another spinlock cycle to prevent
			 * that.
			 */
			if (((volatile XLogRecoveryCtlData *) XLogRecoveryCtl)->recoveryPauseState !=
				RECOVERY_NOT_PAUSED)
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
			 * If we've been asked to lag the primary, wait on latch until
			 * enough time has passed.
			 */
			if (recoveryApplyDelay(xlogreader))
			{
				/*
				 * We test for paused recovery again here. If user sets
				 * delayed apply, it may be because they expect to pause
				 * recovery in case of problems, so we must test again here
				 * otherwise pausing during the delay-wait wouldn't work.
				 */
				if (((volatile XLogRecoveryCtlData *) XLogRecoveryCtl)->recoveryPauseState !=
					RECOVERY_NOT_PAUSED)
					recoveryPausesHere(false);
			}

			/*
			 * Apply the record
			 */
			ApplyWalRecord(xlogreader, record, &replayTLI);

			/* Exit loop if we reached inclusive recovery target */
			if (recoveryStopsAfter(xlogreader))
			{
				reachedRecoveryTarget = true;
				break;
			}

			/* Else, try to fetch the next WAL record */
			record = ReadRecord(xlogprefetcher, LOG, false, replayTLI);
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
			 * This is the last point where we can restart recovery with a new
			 * recovery target, if we shutdown and begin again. After this,
			 * Resource Managers may choose to do permanent corrective actions
			 * at end of recovery.
			 */
			switch (recoveryTargetAction)
			{
				case RECOVERY_TARGET_ACTION_SHUTDOWN:

					/*
					 * exit with special return code to request shutdown of
					 * postmaster.  Log messages issued from postmaster.
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

		RmgrCleanup();

		ereport(LOG,
				(errmsg("redo done at %X/%X system usage: %s",
						LSN_FORMAT_ARGS(xlogreader->ReadRecPtr),
						pg_rusage_show(&ru0))));
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
	 * This check is intentionally after the above log messages that indicate
	 * how far recovery went.
	 */
	if (ArchiveRecoveryRequested &&
		recoveryTarget != RECOVERY_TARGET_UNSET &&
		!reachedRecoveryTarget)
		ereport(FATAL,
				(errmsg("recovery ended before configured recovery target was reached")));
}

/*
 * Subroutine of PerformWalRecovery, to apply one WAL record.
 */
static void
ApplyWalRecord(XLogReaderState *xlogreader, XLogRecord *record, TimeLineID *replayTLI)
{
	ErrorContextCallback errcallback;
	bool		switchedTLI = false;

	/* Setup error traceback support for ereport() */
	errcallback.callback = rm_redo_error_callback;
	errcallback.arg = (void *) xlogreader;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/*
	 * ShmemVariableCache->nextXid must be beyond record's xid.
	 */
	AdvanceNextFullTransactionIdPastXid(record->xl_xid);

	/*
	 * Before replaying this record, check if this record causes the current
	 * timeline to change. The record is already considered to be part of the
	 * new timeline, so we update replayTLI before replaying it. That's
	 * important so that replayEndTLI, which is recorded as the minimum
	 * recovery point's TLI if recovery stops after this record, is set
	 * correctly.
	 */
	if (record->xl_rmid == RM_XLOG_ID)
	{
		TimeLineID	newReplayTLI = *replayTLI;
		TimeLineID	prevReplayTLI = *replayTLI;
		uint8		info = record->xl_info & ~XLR_INFO_MASK;

		if (info == XLOG_CHECKPOINT_SHUTDOWN)
		{
			CheckPoint	checkPoint;

			memcpy(&checkPoint, XLogRecGetData(xlogreader), sizeof(CheckPoint));
			newReplayTLI = checkPoint.ThisTimeLineID;
			prevReplayTLI = checkPoint.PrevTimeLineID;
		}
		else if (info == XLOG_END_OF_RECOVERY)
		{
			xl_end_of_recovery xlrec;

			memcpy(&xlrec, XLogRecGetData(xlogreader), sizeof(xl_end_of_recovery));
			newReplayTLI = xlrec.ThisTimeLineID;
			prevReplayTLI = xlrec.PrevTimeLineID;
		}

		if (newReplayTLI != *replayTLI)
		{
			/* Check that it's OK to switch to this TLI */
			checkTimeLineSwitch(xlogreader->EndRecPtr,
								newReplayTLI, prevReplayTLI, *replayTLI);

			/* Following WAL records should be run with new TLI */
			*replayTLI = newReplayTLI;
			switchedTLI = true;
		}
	}

	/*
	 * Update shared replayEndRecPtr before replaying this record, so that
	 * XLogFlush will update minRecoveryPoint correctly.
	 */
	SpinLockAcquire(&XLogRecoveryCtl->info_lck);
	XLogRecoveryCtl->replayEndRecPtr = xlogreader->EndRecPtr;
	XLogRecoveryCtl->replayEndTLI = *replayTLI;
	SpinLockRelease(&XLogRecoveryCtl->info_lck);

	/*
	 * If we are attempting to enter Hot Standby mode, process XIDs we see
	 */
	if (standbyState >= STANDBY_INITIALIZED &&
		TransactionIdIsValid(record->xl_xid))
		RecordKnownAssignedTransactionIds(record->xl_xid);

	/*
	 * Some XLOG record types that are related to recovery are processed
	 * directly here, rather than in xlog_redo()
	 */
	if (record->xl_rmid == RM_XLOG_ID)
		xlogrecovery_redo(xlogreader, *replayTLI);

	/* Now apply the WAL record itself */
	GetRmgr(record->xl_rmid).rm_redo(xlogreader);

	/*
	 * After redo, check whether the backup pages associated with the WAL
	 * record are consistent with the existing pages. This check is done only
	 * if consistency check is enabled for this record.
	 */
	if ((record->xl_info & XLR_CHECK_CONSISTENCY) != 0)
		verifyBackupPageConsistency(xlogreader);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;

	/*
	 * Update lastReplayedEndRecPtr after this record has been successfully
	 * replayed.
	 */
	SpinLockAcquire(&XLogRecoveryCtl->info_lck);
	XLogRecoveryCtl->lastReplayedReadRecPtr = xlogreader->ReadRecPtr;
	XLogRecoveryCtl->lastReplayedEndRecPtr = xlogreader->EndRecPtr;
	XLogRecoveryCtl->lastReplayedTLI = *replayTLI;
	SpinLockRelease(&XLogRecoveryCtl->info_lck);

	/*
	 * If rm_redo called XLogRequestWalReceiverReply, then we wake up the
	 * receiver so that it notices the updated lastReplayedEndRecPtr and sends
	 * a reply to the primary.
	 */
	if (doRequestWalReceiverReply)
	{
		doRequestWalReceiverReply = false;
		WalRcvForceReply();
	}

	/* Allow read-only connections if we're consistent now */
	CheckRecoveryConsistency();

	/* Is this a timeline switch? */
	if (switchedTLI)
	{
		/*
		 * Before we continue on the new timeline, clean up any (possibly
		 * bogus) future WAL segments on the old timeline.
		 */
		RemoveNonParentXlogFiles(xlogreader->EndRecPtr, *replayTLI);

		/*
		 * Wake up any walsenders to notice that we are on a new timeline.
		 */
		if (AllowCascadeReplication())
			WalSndWakeup();

		/* Reset the prefetcher. */
		XLogPrefetchReconfigure();
	}
}

/*
 * Some XLOG RM record types that are directly related to WAL recovery are
 * handled here rather than in the xlog_redo()
 */
static void
xlogrecovery_redo(XLogReaderState *record, TimeLineID replayTLI)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	XLogRecPtr	lsn = record->EndRecPtr;

	Assert(XLogRecGetRmid(record) == RM_XLOG_ID);

	if (info == XLOG_OVERWRITE_CONTRECORD)
	{
		/* Verify the payload of a XLOG_OVERWRITE_CONTRECORD record. */
		xl_overwrite_contrecord xlrec;

		memcpy(&xlrec, XLogRecGetData(record), sizeof(xl_overwrite_contrecord));
		if (xlrec.overwritten_lsn != record->overwrittenRecPtr)
			elog(FATAL, "mismatching overwritten LSN %X/%X -> %X/%X",
				 LSN_FORMAT_ARGS(xlrec.overwritten_lsn),
				 LSN_FORMAT_ARGS(record->overwrittenRecPtr));

		/* We have safely skipped the aborted record */
		abortedRecPtr = InvalidXLogRecPtr;
		missingContrecPtr = InvalidXLogRecPtr;

		ereport(LOG,
				(errmsg("successfully skipped missing contrecord at %X/%X, overwritten at %s",
						LSN_FORMAT_ARGS(xlrec.overwritten_lsn),
						timestamptz_to_str(xlrec.overwrite_time))));

		/* Verifying the record should only happen once */
		record->overwrittenRecPtr = InvalidXLogRecPtr;
	}
	else if (info == XLOG_BACKUP_END)
	{
		XLogRecPtr	startpoint;

		memcpy(&startpoint, XLogRecGetData(record), sizeof(startpoint));

		if (backupStartPoint == startpoint)
		{
			/*
			 * We have reached the end of base backup, the point where
			 * pg_backup_stop() was done.  The data on disk is now consistent
			 * (assuming we have also reached minRecoveryPoint).  Set
			 * backupEndPoint to the current LSN, so that the next call to
			 * CheckRecoveryConsistency() will notice it and do the
			 * end-of-backup processing.
			 */
			elog(DEBUG1, "end of backup record reached");

			backupEndPoint = lsn;
		}
		else
			elog(DEBUG1, "saw end-of-backup record for backup starting at %X/%X, waiting for %X/%X",
				 LSN_FORMAT_ARGS(startpoint), LSN_FORMAT_ARGS(backupStartPoint));
	}
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
	DIR		   *dir;
	struct dirent *de;

	dir = AllocateDir("pg_tblspc");
	while ((de = ReadDir(dir, "pg_tblspc")) != NULL)
	{
		char		path[MAXPGPATH + 10];

		/* Skip entries of non-oid names */
		if (strspn(de->d_name, "0123456789") != strlen(de->d_name))
			continue;

		snprintf(path, sizeof(path), "pg_tblspc/%s", de->d_name);

		if (get_dirent_type(path, de, false, ERROR) != PGFILETYPE_LNK)
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
	TimeLineID	lastReplayedTLI;

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
	lastReplayedEndRecPtr = XLogRecoveryCtl->lastReplayedEndRecPtr;
	lastReplayedTLI = XLogRecoveryCtl->lastReplayedTLI;

	/*
	 * Have we reached the point where our base backup was completed?
	 */
	if (!XLogRecPtrIsInvalid(backupEndPoint) &&
		backupEndPoint <= lastReplayedEndRecPtr)
	{
		elog(DEBUG1, "end of backup reached");

		/*
		 * We have reached the end of base backup, as indicated by pg_control.
		 * Update the control file accordingly.
		 */
		ReachedEndOfBackup(lastReplayedEndRecPtr, lastReplayedTLI);
		backupStartPoint = InvalidXLogRecPtr;
		backupEndPoint = InvalidXLogRecPtr;
		backupEndRequired = false;
	}

	/*
	 * Have we passed our safe starting point? Note that minRecoveryPoint is
	 * known to be incorrectly set if recovering from a backup, until the
	 * XLOG_BACKUP_END arrives to advise us of the correct minRecoveryPoint.
	 * All we know prior to that is that we're not consistent yet.
	 */
	if (!reachedConsistency && !backupEndRequired &&
		minRecoveryPoint <= lastReplayedEndRecPtr)
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
						LSN_FORMAT_ARGS(lastReplayedEndRecPtr))));
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
		SpinLockAcquire(&XLogRecoveryCtl->info_lck);
		XLogRecoveryCtl->SharedHotStandbyActive = true;
		SpinLockRelease(&XLogRecoveryCtl->info_lck);

		LocalHotStandbyActive = true;

		SendPostmasterSignal(PMSIGNAL_BEGIN_HOT_STANDBY);
	}
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
	xlog_block_info(&buf, record);

	/* translator: %s is a WAL record description */
	errcontext("WAL redo at %X/%X for %s",
			   LSN_FORMAT_ARGS(record->ReadRecPtr),
			   buf.data);

	pfree(buf.data);
}

/*
 * Returns a string describing an XLogRecord, consisting of its identity
 * optionally followed by a colon, a space, and a further description.
 */
void
xlog_outdesc(StringInfo buf, XLogReaderState *record)
{
	RmgrData	rmgr = GetRmgr(XLogRecGetRmid(record));
	uint8		info = XLogRecGetInfo(record);
	const char *id;

	appendStringInfoString(buf, rmgr.rm_name);
	appendStringInfoChar(buf, '/');

	id = rmgr.rm_identify(info);
	if (id == NULL)
		appendStringInfo(buf, "UNKNOWN (%X): ", info & ~XLR_INFO_MASK);
	else
		appendStringInfo(buf, "%s: ", id);

	rmgr.rm_desc(buf, record);
}

#ifdef WAL_DEBUG

static void
xlog_outrec(StringInfo buf, XLogReaderState *record)
{
	appendStringInfo(buf, "prev %X/%X; xid %u",
					 LSN_FORMAT_ARGS(XLogRecGetPrev(record)),
					 XLogRecGetXid(record));

	appendStringInfo(buf, "; len %u",
					 XLogRecGetDataLen(record));

	xlog_block_info(buf, record);
}
#endif							/* WAL_DEBUG */

/*
 * Returns a string giving information about all the blocks in an
 * XLogRecord.
 */
static void
xlog_block_info(StringInfo buf, XLogReaderState *record)
{
	int			block_id;

	/* decode block references */
	for (block_id = 0; block_id <= XLogRecMaxBlockId(record); block_id++)
	{
		RelFileNode rnode;
		ForkNumber	forknum;
		BlockNumber blk;

		if (!XLogRecGetBlockTagExtended(record, block_id,
										&rnode, &forknum, &blk, NULL))
			continue;

		if (forknum != MAIN_FORKNUM)
			appendStringInfo(buf, "; blkref #%d: rel %u/%u/%u, fork %u, blk %u",
							 block_id,
							 rnode.spcNode, rnode.dbNode, rnode.relNode,
							 forknum,
							 blk);
		else
			appendStringInfo(buf, "; blkref #%d: rel %u/%u/%u, blk %u",
							 block_id,
							 rnode.spcNode, rnode.dbNode, rnode.relNode,
							 blk);
		if (XLogRecHasBlockImage(record, block_id))
			appendStringInfoString(buf, " FPW");
	}
}


/*
 * Check that it's OK to switch to new timeline during recovery.
 *
 * 'lsn' is the address of the shutdown checkpoint record we're about to
 * replay. (Currently, timeline can only change at a shutdown checkpoint).
 */
static void
checkTimeLineSwitch(XLogRecPtr lsn, TimeLineID newTLI, TimeLineID prevTLI,
					TimeLineID replayTLI)
{
	/* Check that the record agrees on what the current (old) timeline is */
	if (prevTLI != replayTLI)
		ereport(PANIC,
				(errmsg("unexpected previous timeline ID %u (current timeline ID %u) in checkpoint record",
						prevTLI, replayTLI)));

	/*
	 * The new timeline better be in the list of timelines we expect to see,
	 * according to the timeline history. It should also not decrease.
	 */
	if (newTLI < replayTLI || !tliInHistory(newTLI, expectedTLEs))
		ereport(PANIC,
				(errmsg("unexpected timeline ID %u (after %u) in checkpoint record",
						newTLI, replayTLI)));

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
						LSN_FORMAT_ARGS(minRecoveryPoint),
						minRecoveryPointTLI)));

	/* Looks good */
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
 * Checks whether the current buffer page and backup page stored in the
 * WAL record are consistent or not. Before comparing the two pages, a
 * masking can be applied to the pages to ignore certain areas like hint bits,
 * unused space between pd_lower and pd_upper among other things. This
 * function should be called once WAL replay has been completed for a
 * given record.
 */
static void
verifyBackupPageConsistency(XLogReaderState *record)
{
	RmgrData	rmgr = GetRmgr(XLogRecGetRmid(record));
	RelFileNode rnode;
	ForkNumber	forknum;
	BlockNumber blkno;
	int			block_id;

	/* Records with no backup blocks have no need for consistency checks. */
	if (!XLogRecHasAnyBlockRefs(record))
		return;

	Assert((XLogRecGetInfo(record) & XLR_CHECK_CONSISTENCY) != 0);

	for (block_id = 0; block_id <= XLogRecMaxBlockId(record); block_id++)
	{
		Buffer		buf;
		Page		page;

		if (!XLogRecGetBlockTagExtended(record, block_id,
										&rnode, &forknum, &blkno, NULL))
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
									 RBM_NORMAL_NO_LOG,
									 InvalidBuffer);
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
		if (!RestoreBlockImage(record, block_id, primary_image_masked))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg_internal("%s", record->errormsg_buf)));

		/*
		 * If masking function is defined, mask both the primary and replay
		 * images
		 */
		if (rmgr.rm_mask != NULL)
		{
			rmgr.rm_mask(replay_image_masked, blkno);
			rmgr.rm_mask(primary_image_masked, blkno);
		}

		/* Time to compare the primary and replay images. */
		if (memcmp(replay_image_masked, primary_image_masked, BLCKSZ) != 0)
		{
			elog(FATAL,
				 "inconsistent page found, rel %u/%u/%u, forknum %u, blkno %u",
				 rnode.spcNode, rnode.dbNode, rnode.relNode,
				 forknum, blkno);
		}
	}
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
						LSN_FORMAT_ARGS(recoveryStopLSN))));
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

	if (recoveryTarget == RECOVERY_TARGET_TIME &&
		getRecordTimestamp(record, &recordXtime))
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
 * record in XLogRecoveryCtl->recoveryLastXTime.
 */
static bool
recoveryStopsAfter(XLogReaderState *record)
{
	uint8		info;
	uint8		xact_info;
	uint8		rmid;
	TimestampTz recordXtime;

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
						LSN_FORMAT_ARGS(recoveryStopLSN))));
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
 * Create a comment for the history file to explain why and where
 * timeline changed.
 */
static char *
getRecoveryStopReason(void)
{
	char		reason[200];

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
				 LSN_FORMAT_ARGS(recoveryStopLSN));
	else if (recoveryTarget == RECOVERY_TARGET_NAME)
		snprintf(reason, sizeof(reason),
				 "at restore point \"%s\"",
				 recoveryStopName);
	else if (recoveryTarget == RECOVERY_TARGET_IMMEDIATE)
		snprintf(reason, sizeof(reason), "reached consistency");
	else
		snprintf(reason, sizeof(reason), "no recovery target specified");

	return pstrdup(reason);
}

/*
 * Wait until shared recoveryPauseState is set to RECOVERY_NOT_PAUSED.
 *
 * endOfRecovery is true if the recovery target is reached and
 * the paused state starts at the end of recovery because of
 * recovery_target_action=pause, and false otherwise.
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

	/* loop until recoveryPauseState is set to RECOVERY_NOT_PAUSED */
	while (GetRecoveryPauseState() != RECOVERY_NOT_PAUSED)
	{
		HandleStartupProcInterrupts();
		if (CheckForStandbyTrigger())
			return;

		/*
		 * If recovery pause is requested then set it paused.  While we are in
		 * the loop, user might resume and pause again so set this every time.
		 */
		ConfirmRecoveryPaused();

		/*
		 * We wait on a condition variable that will wake us as soon as the
		 * pause ends, but we use a timeout so we can check the above exit
		 * condition periodically too.
		 */
		ConditionVariableTimedSleep(&XLogRecoveryCtl->recoveryNotPausedCV, 1000,
									WAIT_EVENT_RECOVERY_PAUSE);
	}
	ConditionVariableCancelSleep();
}

/*
 * When recovery_min_apply_delay is set, we wait long enough to make sure
 * certain record types are applied at least that interval behind the primary.
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
		ResetLatch(&XLogRecoveryCtl->recoveryWakeupLatch);

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

		(void) WaitLatch(&XLogRecoveryCtl->recoveryWakeupLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 msecs,
						 WAIT_EVENT_RECOVERY_APPLY_DELAY);
	}
	return true;
}

/*
 * Get the current state of the recovery pause request.
 */
RecoveryPauseState
GetRecoveryPauseState(void)
{
	RecoveryPauseState state;

	SpinLockAcquire(&XLogRecoveryCtl->info_lck);
	state = XLogRecoveryCtl->recoveryPauseState;
	SpinLockRelease(&XLogRecoveryCtl->info_lck);

	return state;
}

/*
 * Set the recovery pause state.
 *
 * If recovery pause is requested then sets the recovery pause state to
 * 'pause requested' if it is not already 'paused'.  Otherwise, sets it
 * to 'not paused' to resume the recovery.  The recovery pause will be
 * confirmed by the ConfirmRecoveryPaused.
 */
void
SetRecoveryPause(bool recoveryPause)
{
	SpinLockAcquire(&XLogRecoveryCtl->info_lck);

	if (!recoveryPause)
		XLogRecoveryCtl->recoveryPauseState = RECOVERY_NOT_PAUSED;
	else if (XLogRecoveryCtl->recoveryPauseState == RECOVERY_NOT_PAUSED)
		XLogRecoveryCtl->recoveryPauseState = RECOVERY_PAUSE_REQUESTED;

	SpinLockRelease(&XLogRecoveryCtl->info_lck);

	if (!recoveryPause)
		ConditionVariableBroadcast(&XLogRecoveryCtl->recoveryNotPausedCV);
}

/*
 * Confirm the recovery pause by setting the recovery pause state to
 * RECOVERY_PAUSED.
 */
static void
ConfirmRecoveryPaused(void)
{
	/* If recovery pause is requested then set it paused */
	SpinLockAcquire(&XLogRecoveryCtl->info_lck);
	if (XLogRecoveryCtl->recoveryPauseState == RECOVERY_PAUSE_REQUESTED)
		XLogRecoveryCtl->recoveryPauseState = RECOVERY_PAUSED;
	SpinLockRelease(&XLogRecoveryCtl->info_lck);
}


/*
 * Attempt to read the next XLOG record.
 *
 * Before first call, the reader needs to be positioned to the first record
 * by calling XLogPrefetcherBeginRead().
 *
 * If no valid record is available, returns NULL, or fails if emode is PANIC.
 * (emode must be either PANIC, LOG). In standby mode, retries until a valid
 * record is available.
 */
static XLogRecord *
ReadRecord(XLogPrefetcher *xlogprefetcher, int emode,
		   bool fetching_ckpt, TimeLineID replayTLI)
{
	XLogRecord *record;
	XLogReaderState *xlogreader = XLogPrefetcherGetReader(xlogprefetcher);
	XLogPageReadPrivate *private = (XLogPageReadPrivate *) xlogreader->private_data;

	/* Pass through parameters to XLogPageRead */
	private->fetching_ckpt = fetching_ckpt;
	private->emode = emode;
	private->randAccess = (xlogreader->ReadRecPtr == InvalidXLogRecPtr);
	private->replayTLI = replayTLI;

	/* This is the first attempt to read this page. */
	lastSourceFailed = false;

	for (;;)
	{
		char	   *errormsg;

		record = XLogPrefetcherReadRecord(xlogprefetcher, &errormsg);
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
				ereport(emode_for_corrupt_record(emode, xlogreader->EndRecPtr),
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
			ereport(emode_for_corrupt_record(emode, xlogreader->EndRecPtr),
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

				SwitchIntoArchiveRecovery(xlogreader->EndRecPtr, replayTLI);
				minRecoveryPoint = xlogreader->EndRecPtr;
				minRecoveryPointTLI = replayTLI;

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
 * Read the XLOG page containing RecPtr into readBuf (if not read already).
 * Returns number of bytes read, if the page is read successfully, or
 * XLREAD_FAIL in case of errors.  When errors occur, they are ereport'ed, but
 * only if they have not been previously reported.
 *
 * While prefetching, xlogreader->nonblocking may be set.  In that case,
 * returns XLREAD_WOULDBLOCK if we'd otherwise have to wait for more WAL.
 *
 * This is responsible for restoring files from archive as needed, as well
 * as for waiting for the requested WAL record to arrive in standby mode.
 *
 * 'emode' specifies the log level used for reporting "file not found" or
 * "end of WAL" situations in archive recovery, or in standby mode when a
 * trigger file is found. If set to WARNING or below, XLogPageRead() returns
 * XLREAD_FAIL in those situations, on higher log levels the ereport() won't
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
		if (ArchiveRecoveryRequested && IsUnderPostmaster)
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
		if (readFile >= 0 &&
			xlogreader->nonblocking &&
			readSource == XLOG_FROM_STREAM &&
			flushedUpto < targetPagePtr + reqLen)
			return XLREAD_WOULDBLOCK;

		switch (WaitForWALToBecomeAvailable(targetPagePtr + reqLen,
											private->randAccess,
											private->fetching_ckpt,
											targetRecPtr,
											private->replayTLI,
											xlogreader->EndRecPtr,
											xlogreader->nonblocking))
		{
			case XLREAD_WOULDBLOCK:
				return XLREAD_WOULDBLOCK;
			case XLREAD_FAIL:
				if (readFile >= 0)
					close(readFile);
				readFile = -1;
				readLen = 0;
				readSource = XLOG_FROM_ANY;
				return XLREAD_FAIL;
			case XLREAD_SUCCESS:
				break;
		}
	}

	/*
	 * At this point, we have the right segment open and if we're streaming we
	 * know the requested record is in it.
	 */
	Assert(readFile != -1);

	/*
	 * If the current segment is being streamed from the primary, calculate
	 * how much of the current page we have received already. We know the
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
	 * it's not valid. This may seem unnecessary, because ReadPageInternal()
	 * validates the page header anyway, and would propagate the failure up to
	 * ReadRecord(), which would retry. However, there's a corner case with
	 * continuation records, if a record is split across two pages such that
	 * we would need to read the two pages from different sources. For
	 * example, imagine a scenario where a streaming replica is started up,
	 * and replay reaches a record that's split across two WAL segments. The
	 * first page is only available locally, in pg_wal, because it's already
	 * been recycled on the primary. The second page, however, is not present
	 * in pg_wal, and we should stream it from the primary. There is a
	 * recycled WAL segment present in pg_wal, with garbage contents, however.
	 * We would read the first page from the local WAL segment, but when
	 * reading the second page, we would read the bogus, recycled, WAL
	 * segment. If we didn't catch that case here, we would never recover,
	 * because ReadRecord() would retry reading the whole record from the
	 * beginning.
	 *
	 * Of course, this only catches errors in the page header, which is what
	 * happens in the case of a recycled WAL segment. Other kinds of errors or
	 * corruption still has the same problem. But this at least fixes the
	 * common case, which can happen as part of normal operation.
	 *
	 * Validating the page header is cheap enough that doing it twice
	 * shouldn't be a big deal from a performance point of view.
	 *
	 * When not in standby mode, an invalid page header should cause recovery
	 * to end, not retry reading the page, so we don't need to validate the
	 * page header here for the retry. Instead, ReadPageInternal() is
	 * responsible for the validation.
	 */
	if (StandbyMode &&
		!XLogReaderValidatePageHeader(xlogreader, targetPagePtr, readBuf))
	{
		/*
		 * Emit this error right now then retry this page immediately. Use
		 * errmsg_internal() because the message was already translated.
		 */
		if (xlogreader->errormsg_buf[0])
			ereport(emode_for_corrupt_record(emode, xlogreader->EndRecPtr),
					(errmsg_internal("%s", xlogreader->errormsg_buf)));

		/* reset any error XLogReaderValidatePageHeader() might have set */
		XLogReaderResetError(xlogreader);
		goto next_record_is_invalid;
	}

	return readLen;

next_record_is_invalid:

	/*
	 * If we're reading ahead, give up fast.  Retries and error reporting will
	 * be handled by a later read when recovery catches up to this point.
	 */
	if (xlogreader->nonblocking)
		return XLREAD_WOULDBLOCK;

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
		return XLREAD_FAIL;
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
 * 'replayLSN' is the current replay LSN, so that if we scan for new
 * timelines, we can reject a switch to a timeline that branched off before
 * this point.
 *
 * If the record is not immediately available, the function returns false
 * if we're not in standby mode. In standby mode, waits for it to become
 * available.
 *
 * When the requested record becomes available, the function opens the file
 * containing it (if not open already), and returns XLREAD_SUCCESS. When end
 * of standby mode is triggered by the user, and there is no more WAL
 * available, returns XLREAD_FAIL.
 *
 * If nonblocking is true, then give up immediately if we can't satisfy the
 * request, returning XLREAD_WOULDBLOCK instead of waiting.
 */
static XLogPageReadResult
WaitForWALToBecomeAvailable(XLogRecPtr RecPtr, bool randAccess,
							bool fetching_ckpt, XLogRecPtr tliRecPtr,
							TimeLineID replayTLI, XLogRecPtr replayLSN,
							bool nonblocking)
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
			/*
			 * Don't allow any retry loops to occur during nonblocking
			 * readahead.  Let the caller process everything that has been
			 * decoded already first.
			 */
			if (nonblocking)
				return XLREAD_WOULDBLOCK;

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
						XLogShutdownWalRcv();
						return XLREAD_FAIL;
					}

					/*
					 * Not in standby mode, and we've now tried the archive
					 * and pg_wal.
					 */
					if (!StandbyMode)
						return XLREAD_FAIL;

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
					 * find an invalid record in the WAL streamed from the
					 * primary, in which case something is seriously wrong.
					 * There's little chance that the problem will just go
					 * away, but PANIC is not good for availability either,
					 * especially in hot standby mode. So, we treat that the
					 * same as disconnection, and retry from archive/pg_wal
					 * again. The WAL in the archive should be identical to
					 * what was streamed, so it's unlikely that it helps, but
					 * one can hope...
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
					XLogShutdownWalRcv();

					/*
					 * Before we sleep, re-scan for possible new timelines if
					 * we were requested to recover to the latest timeline.
					 */
					if (recoveryTargetTimeLineGoal == RECOVERY_TARGET_TIMELINE_LATEST)
					{
						if (rescanLatestTimeLine(replayTLI, replayLSN))
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

						elog(LOG, "waiting for WAL to become available at %X/%X",
							 LSN_FORMAT_ARGS(RecPtr));

						(void) WaitLatch(&XLogRecoveryCtl->recoveryWakeupLatch,
										 WL_LATCH_SET | WL_TIMEOUT |
										 WL_EXIT_ON_PM_DEATH,
										 wait_time,
										 WAIT_EVENT_RECOVERY_RETRIEVE_RETRY_INTERVAL);
						ResetLatch(&XLogRecoveryCtl->recoveryWakeupLatch);
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
					return XLREAD_SUCCESS;	/* success! */

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
						XLogShutdownWalRcv();

						/*
						 * Re-scan for possible new timelines if we were
						 * requested to recover to the latest timeline.
						 */
						if (recoveryTargetTimeLineGoal ==
							RECOVERY_TARGET_TIMELINE_LATEST)
							rescanLatestTimeLine(replayTLI, replayLSN);

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
							tli = RedoStartTLI;
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
									 LSN_FORMAT_ARGS(tliRecPtr),
									 tli, curFileTLI);
						}
						curFileTLI = tli;
						SetInstallXLogFileSegmentActive();
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
							return XLREAD_SUCCESS;
						}
						break;
					}

					/* In nonblocking mode, return rather than sleeping. */
					if (nonblocking)
						return XLREAD_WOULDBLOCK;

					/*
					 * Data not here yet. Check for trigger, then wait for
					 * walreceiver to wake us up when new WAL arrives.
					 */
					if (CheckForStandbyTrigger())
					{
						/*
						 * Note that we don't return XLREAD_FAIL immediately
						 * here. After being triggered, we still want to
						 * replay all the WAL that was already streamed. It's
						 * in pg_wal now, so we just treat this as a failure,
						 * and the state machine will move on to replay the
						 * streamed WAL from pg_wal, and then recheck the
						 * trigger and exit replay.
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

					/* Update pg_stat_recovery_prefetch before sleeping. */
					XLogPrefetcherComputeStats(xlogprefetcher);

					/*
					 * Wait for more WAL to arrive. Time out after 5 seconds
					 * to react to a trigger file promptly and to check if the
					 * WAL receiver is still active.
					 */
					(void) WaitLatch(&XLogRecoveryCtl->recoveryWakeupLatch,
									 WL_LATCH_SET | WL_TIMEOUT |
									 WL_EXIT_ON_PM_DEATH,
									 5000L, WAIT_EVENT_RECOVERY_WAL_STREAM);
					ResetLatch(&XLogRecoveryCtl->recoveryWakeupLatch);
					break;
				}

			default:
				elog(ERROR, "unexpected WAL source %d", currentSource);
		}

		/*
		 * Check for recovery pause here so that we can confirm more quickly
		 * that a requested pause has actually taken effect.
		 */
		if (((volatile XLogRecoveryCtlData *) XLogRecoveryCtl)->recoveryPauseState !=
			RECOVERY_NOT_PAUSED)
			recoveryPausesHere(false);

		/*
		 * This possibly-long loop needs to handle interrupts of startup
		 * process.
		 */
		HandleStartupProcInterrupts();
	}

	return XLREAD_FAIL;			/* not reached */
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
 * or in records streamed from the primary. Files in the archive should be complete,
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
 * Subroutine to try to fetch and validate a prior checkpoint record.
 *
 * whichChkpt identifies the checkpoint (merely for reporting purposes).
 * 1 for "primary", 0 for "other" (backup_label)
 */
static XLogRecord *
ReadCheckpointRecord(XLogPrefetcher *xlogprefetcher, XLogRecPtr RecPtr,
					 int whichChkpt, bool report, TimeLineID replayTLI)
{
	XLogRecord *record;
	uint8		info;

	Assert(xlogreader != NULL);

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

	XLogPrefetcherBeginRead(xlogprefetcher, RecPtr);
	record = ReadRecord(xlogprefetcher, LOG, true, replayTLI);

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
 * Scan for new timelines that might have appeared in the archive since we
 * started recovery.
 *
 * If there are any, the function changes recovery target TLI to the latest
 * one and returns 'true'.
 */
static bool
rescanLatestTimeLine(TimeLineID replayTLI, XLogRecPtr replayLSN)
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
						replayTLI)));
		return false;
	}

	/*
	 * The current timeline was found in the history file, but check that the
	 * next timeline was forked off from it *after* the current recovery
	 * location.
	 */
	if (currentTle->end < replayLSN)
	{
		ereport(LOG,
				(errmsg("new timeline %u forked off current database system timeline %u before current recovery point %X/%X",
						newtarget,
						replayTLI,
						LSN_FORMAT_ARGS(replayLSN))));
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

			if (!RestoreArchivedFile(path, xlogfname,
									 "RECOVERYXLOG",
									 wal_segment_size,
									 InRedo))
				return -1;
			break;

		case XLOG_FROM_PG_WAL:
		case XLOG_FROM_STREAM:
			XLogFilePath(path, tli, segno, wal_segment_size);
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
		Assert(!IsInstallXLogFileSegmentActive());
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
	 * streamed from the primary when we start streaming, instead of
	 * recovering with a dummy history generated here.
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

	SpinLockAcquire(&XLogRecoveryCtl->info_lck);
	LocalPromoteIsTriggered = XLogRecoveryCtl->SharedPromoteIsTriggered;
	SpinLockRelease(&XLogRecoveryCtl->info_lck);

	return LocalPromoteIsTriggered;
}

static void
SetPromoteIsTriggered(void)
{
	SpinLockAcquire(&XLogRecoveryCtl->info_lck);
	XLogRecoveryCtl->SharedPromoteIsTriggered = true;
	SpinLockRelease(&XLogRecoveryCtl->info_lck);

	/*
	 * Mark the recovery pause state as 'not paused' because the paused state
	 * ends and promotion continues if a promotion is triggered while recovery
	 * is paused. Otherwise pg_get_wal_replay_pause_state() can mistakenly
	 * return 'paused' while a promotion is ongoing.
	 */
	SetRecoveryPause(false);

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

	if (IsPromoteSignaled() && CheckPromoteSignal())
	{
		ereport(LOG, (errmsg("received promote request")));
		RemovePromoteSignalFiles();
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
}

/*
 * Check to see if a promote request has arrived.
 */
bool
CheckPromoteSignal(void)
{
	struct stat stat_buf;

	if (stat(PROMOTE_SIGNAL_FILE, &stat_buf) == 0)
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
	SetLatch(&XLogRecoveryCtl->recoveryWakeupLatch);
}

/*
 * Schedule a walreceiver wakeup in the main recovery loop.
 */
void
XLogRequestWalReceiverReply(void)
{
	doRequestWalReceiverReply = true;
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
		SpinLockAcquire(&XLogRecoveryCtl->info_lck);
		LocalHotStandbyActive = XLogRecoveryCtl->SharedHotStandbyActive;
		SpinLockRelease(&XLogRecoveryCtl->info_lck);

		return LocalHotStandbyActive;
	}
}

/*
 * Like HotStandbyActive(), but to be used only in WAL replay code,
 * where we don't need to ask any other process what the state is.
 */
static bool
HotStandbyActiveInReplay(void)
{
	Assert(AmStartupProcess() || !IsPostmasterEnvironment);
	return LocalHotStandbyActive;
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

	SpinLockAcquire(&XLogRecoveryCtl->info_lck);
	recptr = XLogRecoveryCtl->lastReplayedEndRecPtr;
	tli = XLogRecoveryCtl->lastReplayedTLI;
	SpinLockRelease(&XLogRecoveryCtl->info_lck);

	if (replayTLI)
		*replayTLI = tli;
	return recptr;
}


/*
 * Get position of last applied, or the record being applied.
 *
 * This is different from GetXLogReplayRecPtr() in that if a WAL
 * record is currently being applied, this includes that record.
 */
XLogRecPtr
GetCurrentReplayRecPtr(TimeLineID *replayEndTLI)
{
	XLogRecPtr	recptr;
	TimeLineID	tli;

	SpinLockAcquire(&XLogRecoveryCtl->info_lck);
	recptr = XLogRecoveryCtl->replayEndRecPtr;
	tli = XLogRecoveryCtl->replayEndTLI;
	SpinLockRelease(&XLogRecoveryCtl->info_lck);

	if (replayEndTLI)
		*replayEndTLI = tli;
	return recptr;
}

/*
 * Save timestamp of latest processed commit/abort record.
 *
 * We keep this in XLogRecoveryCtl, not a simple static variable, so that it can be
 * seen by processes other than the startup process.  Note in particular
 * that CreateRestartPoint is executed in the checkpointer.
 */
static void
SetLatestXTime(TimestampTz xtime)
{
	SpinLockAcquire(&XLogRecoveryCtl->info_lck);
	XLogRecoveryCtl->recoveryLastXTime = xtime;
	SpinLockRelease(&XLogRecoveryCtl->info_lck);
}

/*
 * Fetch timestamp of latest processed commit/abort record.
 */
TimestampTz
GetLatestXTime(void)
{
	TimestampTz xtime;

	SpinLockAcquire(&XLogRecoveryCtl->info_lck);
	xtime = XLogRecoveryCtl->recoveryLastXTime;
	SpinLockRelease(&XLogRecoveryCtl->info_lck);

	return xtime;
}

/*
 * Save timestamp of the next chunk of WAL records to apply.
 *
 * We keep this in XLogRecoveryCtl, not a simple static variable, so that it can be
 * seen by all backends.
 */
static void
SetCurrentChunkStartTime(TimestampTz xtime)
{
	SpinLockAcquire(&XLogRecoveryCtl->info_lck);
	XLogRecoveryCtl->currentChunkStartTime = xtime;
	SpinLockRelease(&XLogRecoveryCtl->info_lck);
}

/*
 * Fetch timestamp of latest processed commit/abort record.
 * Startup process maintains an accurate local copy in XLogReceiptTime
 */
TimestampTz
GetCurrentChunkReplayStartTime(void)
{
	TimestampTz xtime;

	SpinLockAcquire(&XLogRecoveryCtl->info_lck);
	xtime = XLogRecoveryCtl->currentChunkStartTime;
	SpinLockRelease(&XLogRecoveryCtl->info_lck);

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
void
RecoveryRequiresIntParameter(const char *param_name, int currValue, int minValue)
{
	if (currValue < minValue)
	{
		if (HotStandbyActiveInReplay())
		{
			bool		warned_for_promote = false;

			ereport(WARNING,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("hot standby is not possible because of insufficient parameter settings"),
					 errdetail("%s = %d is a lower setting than on the primary server, where its value was %d.",
							   param_name,
							   currValue,
							   minValue)));

			SetRecoveryPause(true);

			ereport(LOG,
					(errmsg("recovery has paused"),
					 errdetail("If recovery is unpaused, the server will shut down."),
					 errhint("You can then restart the server after making the necessary configuration changes.")));

			while (GetRecoveryPauseState() != RECOVERY_NOT_PAUSED)
			{
				HandleStartupProcInterrupts();

				if (CheckForStandbyTrigger())
				{
					if (!warned_for_promote)
						ereport(WARNING,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("promotion is not possible because of insufficient parameter settings"),

						/*
						 * Repeat the detail from above so it's easy to find
						 * in the log.
						 */
								 errdetail("%s = %d is a lower setting than on the primary server, where its value was %d.",
										   param_name,
										   currValue,
										   minValue),
								 errhint("Restart the server after making the necessary configuration changes.")));
					warned_for_promote = true;
				}

				/*
				 * If recovery pause is requested then set it paused.  While
				 * we are in the loop, user might resume and pause again so
				 * set this every time.
				 */
				ConfirmRecoveryPaused();

				/*
				 * We wait on a condition variable that will wake us as soon
				 * as the pause ends, but we use a timeout so we can check the
				 * above conditions periodically too.
				 */
				ConditionVariableTimedSleep(&XLogRecoveryCtl->recoveryNotPausedCV, 1000,
											WAIT_EVENT_RECOVERY_PAUSE);
			}
			ConditionVariableCancelSleep();
		}

		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("recovery aborted because of insufficient parameter settings"),
		/* Repeat the detail from above so it's easy to find in the log. */
				 errdetail("%s = %d is a lower setting than on the primary server, where its value was %d.",
						   param_name,
						   currValue,
						   minValue),
				 errhint("You can restart the server after making the necessary configuration changes.")));
	}
}
