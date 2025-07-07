/*-------------------------------------------------------------------------
 *
 * standby.c
 *	  Misc functions used in Hot Standby mode.
 *
 *	All functions for handling RM_STANDBY_ID, which relate to
 *	AccessExclusiveLocks and starting snapshots for Hot Standby mode.
 *	Plus conflict recovery processing.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/standby.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "replication/slot.h"
#include "storage/bufmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/sinvaladt.h"
#include "storage/standby.h"
#include "utils/hsearch.h"
#include "utils/injection_point.h"
#include "utils/ps_status.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"

/* User-settable GUC parameters */
int			max_standby_archive_delay = 30 * 1000;
int			max_standby_streaming_delay = 30 * 1000;
bool		log_recovery_conflict_waits = false;

/*
 * Keep track of all the exclusive locks owned by original transactions.
 * For each known exclusive lock, there is a RecoveryLockEntry in the
 * RecoveryLockHash hash table.  All RecoveryLockEntrys belonging to a
 * given XID are chained together so that we can find them easily.
 * For each original transaction that is known to have any such locks,
 * there is a RecoveryLockXidEntry in the RecoveryLockXidHash hash table,
 * which stores the head of the chain of its locks.
 */
typedef struct RecoveryLockEntry
{
	xl_standby_lock key;		/* hash key: xid, dbOid, relOid */
	struct RecoveryLockEntry *next; /* chain link */
} RecoveryLockEntry;

typedef struct RecoveryLockXidEntry
{
	TransactionId xid;			/* hash key -- must be first */
	struct RecoveryLockEntry *head; /* chain head */
} RecoveryLockXidEntry;

static HTAB *RecoveryLockHash = NULL;
static HTAB *RecoveryLockXidHash = NULL;

/* Flags set by timeout handlers */
static volatile sig_atomic_t got_standby_deadlock_timeout = false;
static volatile sig_atomic_t got_standby_delay_timeout = false;
static volatile sig_atomic_t got_standby_lock_timeout = false;

static void ResolveRecoveryConflictWithVirtualXIDs(VirtualTransactionId *waitlist,
												   ProcSignalReason reason,
												   uint32 wait_event_info,
												   bool report_waiting);
static void SendRecoveryConflictWithBufferPin(ProcSignalReason reason);
static XLogRecPtr LogCurrentRunningXacts(RunningTransactions CurrRunningXacts);
static void LogAccessExclusiveLocks(int nlocks, xl_standby_lock *locks);
static const char *get_recovery_conflict_desc(ProcSignalReason reason);

/*
 * InitRecoveryTransactionEnvironment
 *		Initialize tracking of our primary's in-progress transactions.
 *
 * We need to issue shared invalidations and hold locks. Holding locks
 * means others may want to wait on us, so we need to make a lock table
 * vxact entry like a real transaction. We could create and delete
 * lock table entries for each transaction but its simpler just to create
 * one permanent entry and leave it there all the time. Locks are then
 * acquired and released as needed. Yes, this means you can see the
 * Startup process in pg_locks once we have run this.
 */
void
InitRecoveryTransactionEnvironment(void)
{
	VirtualTransactionId vxid;
	HASHCTL		hash_ctl;

	Assert(RecoveryLockHash == NULL);	/* don't run this twice */

	/*
	 * Initialize the hash tables for tracking the locks held by each
	 * transaction.
	 */
	hash_ctl.keysize = sizeof(xl_standby_lock);
	hash_ctl.entrysize = sizeof(RecoveryLockEntry);
	RecoveryLockHash = hash_create("RecoveryLockHash",
								   64,
								   &hash_ctl,
								   HASH_ELEM | HASH_BLOBS);
	hash_ctl.keysize = sizeof(TransactionId);
	hash_ctl.entrysize = sizeof(RecoveryLockXidEntry);
	RecoveryLockXidHash = hash_create("RecoveryLockXidHash",
									  64,
									  &hash_ctl,
									  HASH_ELEM | HASH_BLOBS);

	/*
	 * Initialize shared invalidation management for Startup process, being
	 * careful to register ourselves as a sendOnly process so we don't need to
	 * read messages, nor will we get signaled when the queue starts filling
	 * up.
	 */
	SharedInvalBackendInit(true);

	/*
	 * Lock a virtual transaction id for Startup process.
	 *
	 * We need to do GetNextLocalTransactionId() because
	 * SharedInvalBackendInit() leaves localTransactionId invalid and the lock
	 * manager doesn't like that at all.
	 *
	 * Note that we don't need to run XactLockTableInsert() because nobody
	 * needs to wait on xids. That sounds a little strange, but table locks
	 * are held by vxids and row level locks are held by xids. All queries
	 * hold AccessShareLocks so never block while we write or lock new rows.
	 */
	MyProc->vxid.procNumber = MyProcNumber;
	vxid.procNumber = MyProcNumber;
	vxid.localTransactionId = GetNextLocalTransactionId();
	VirtualXactLockTableInsert(vxid);

	standbyState = STANDBY_INITIALIZED;
}

/*
 * ShutdownRecoveryTransactionEnvironment
 *		Shut down transaction tracking
 *
 * Prepare to switch from hot standby mode to normal operation. Shut down
 * recovery-time transaction tracking.
 *
 * This must be called even in shutdown of startup process if transaction
 * tracking has been initialized. Otherwise some locks the tracked
 * transactions were holding will not be released and may interfere with
 * the processes still running (but will exit soon later) at the exit of
 * startup process.
 */
void
ShutdownRecoveryTransactionEnvironment(void)
{
	/*
	 * Do nothing if RecoveryLockHash is NULL because that means that
	 * transaction tracking has not yet been initialized or has already been
	 * shut down.  This makes it safe to have possibly-redundant calls of this
	 * function during process exit.
	 */
	if (RecoveryLockHash == NULL)
		return;

	/* Mark all tracked in-progress transactions as finished. */
	ExpireAllKnownAssignedTransactionIds();

	/* Release all locks the tracked transactions were holding */
	StandbyReleaseAllLocks();

	/* Destroy the lock hash tables. */
	hash_destroy(RecoveryLockHash);
	hash_destroy(RecoveryLockXidHash);
	RecoveryLockHash = NULL;
	RecoveryLockXidHash = NULL;

	/* Cleanup our VirtualTransaction */
	VirtualXactLockTableCleanup();
}


/*
 * -----------------------------------------------------
 *		Standby wait timers and backend cancel logic
 * -----------------------------------------------------
 */

/*
 * Determine the cutoff time at which we want to start canceling conflicting
 * transactions.  Returns zero (a time safely in the past) if we are willing
 * to wait forever.
 */
static TimestampTz
GetStandbyLimitTime(void)
{
	TimestampTz rtime;
	bool		fromStream;

	/*
	 * The cutoff time is the last WAL data receipt time plus the appropriate
	 * delay variable.  Delay of -1 means wait forever.
	 */
	GetXLogReceiptTime(&rtime, &fromStream);
	if (fromStream)
	{
		if (max_standby_streaming_delay < 0)
			return 0;			/* wait forever */
		return TimestampTzPlusMilliseconds(rtime, max_standby_streaming_delay);
	}
	else
	{
		if (max_standby_archive_delay < 0)
			return 0;			/* wait forever */
		return TimestampTzPlusMilliseconds(rtime, max_standby_archive_delay);
	}
}

#define STANDBY_INITIAL_WAIT_US  1000
static int	standbyWait_us = STANDBY_INITIAL_WAIT_US;

/*
 * Standby wait logic for ResolveRecoveryConflictWithVirtualXIDs.
 * We wait here for a while then return. If we decide we can't wait any
 * more then we return true, if we can wait some more return false.
 */
static bool
WaitExceedsMaxStandbyDelay(uint32 wait_event_info)
{
	TimestampTz ltime;

	CHECK_FOR_INTERRUPTS();

	/* Are we past the limit time? */
	ltime = GetStandbyLimitTime();
	if (ltime && GetCurrentTimestamp() >= ltime)
		return true;

	/*
	 * Sleep a bit (this is essential to avoid busy-waiting).
	 */
	pgstat_report_wait_start(wait_event_info);
	pg_usleep(standbyWait_us);
	pgstat_report_wait_end();

	/*
	 * Progressively increase the sleep times, but not to more than 1s, since
	 * pg_usleep isn't interruptible on some platforms.
	 */
	standbyWait_us *= 2;
	if (standbyWait_us > 1000000)
		standbyWait_us = 1000000;

	return false;
}

/*
 * Log the recovery conflict.
 *
 * wait_start is the timestamp when the caller started to wait.
 * now is the timestamp when this function has been called.
 * wait_list is the list of virtual transaction ids assigned to
 * conflicting processes. still_waiting indicates whether
 * the startup process is still waiting for the recovery conflict
 * to be resolved or not.
 */
void
LogRecoveryConflict(ProcSignalReason reason, TimestampTz wait_start,
					TimestampTz now, VirtualTransactionId *wait_list,
					bool still_waiting)
{
	long		secs;
	int			usecs;
	long		msecs;
	StringInfoData buf;
	int			nprocs = 0;

	/*
	 * There must be no conflicting processes when the recovery conflict has
	 * already been resolved.
	 */
	Assert(still_waiting || wait_list == NULL);

	TimestampDifference(wait_start, now, &secs, &usecs);
	msecs = secs * 1000 + usecs / 1000;
	usecs = usecs % 1000;

	if (wait_list)
	{
		VirtualTransactionId *vxids;

		/* Construct a string of list of the conflicting processes */
		vxids = wait_list;
		while (VirtualTransactionIdIsValid(*vxids))
		{
			PGPROC	   *proc = ProcNumberGetProc(vxids->procNumber);

			/* proc can be NULL if the target backend is not active */
			if (proc)
			{
				if (nprocs == 0)
				{
					initStringInfo(&buf);
					appendStringInfo(&buf, "%d", proc->pid);
				}
				else
					appendStringInfo(&buf, ", %d", proc->pid);

				nprocs++;
			}

			vxids++;
		}
	}

	/*
	 * If wait_list is specified, report the list of PIDs of active
	 * conflicting backends in a detail message. Note that if all the backends
	 * in the list are not active, no detail message is logged.
	 */
	if (still_waiting)
	{
		ereport(LOG,
				errmsg("recovery still waiting after %ld.%03d ms: %s",
					   msecs, usecs, get_recovery_conflict_desc(reason)),
				nprocs > 0 ? errdetail_log_plural("Conflicting process: %s.",
												  "Conflicting processes: %s.",
												  nprocs, buf.data) : 0);
	}
	else
	{
		ereport(LOG,
				errmsg("recovery finished waiting after %ld.%03d ms: %s",
					   msecs, usecs, get_recovery_conflict_desc(reason)));
	}

	if (nprocs > 0)
		pfree(buf.data);
}

/*
 * This is the main executioner for any query backend that conflicts with
 * recovery processing. Judgement has already been passed on it within
 * a specific rmgr. Here we just issue the orders to the procs. The procs
 * then throw the required error as instructed.
 *
 * If report_waiting is true, "waiting" is reported in PS display and the
 * wait for recovery conflict is reported in the log, if necessary. If
 * the caller is responsible for reporting them, report_waiting should be
 * false. Otherwise, both the caller and this function report the same
 * thing unexpectedly.
 */
static void
ResolveRecoveryConflictWithVirtualXIDs(VirtualTransactionId *waitlist,
									   ProcSignalReason reason, uint32 wait_event_info,
									   bool report_waiting)
{
	TimestampTz waitStart = 0;
	bool		waiting = false;
	bool		logged_recovery_conflict = false;

	/* Fast exit, to avoid a kernel call if there's no work to be done. */
	if (!VirtualTransactionIdIsValid(*waitlist))
		return;

	/* Set the wait start timestamp for reporting */
	if (report_waiting && (log_recovery_conflict_waits || update_process_title))
		waitStart = GetCurrentTimestamp();

	while (VirtualTransactionIdIsValid(*waitlist))
	{
		/* reset standbyWait_us for each xact we wait for */
		standbyWait_us = STANDBY_INITIAL_WAIT_US;

		/* wait until the virtual xid is gone */
		while (!VirtualXactLock(*waitlist, false))
		{
			/* Is it time to kill it? */
			if (WaitExceedsMaxStandbyDelay(wait_event_info))
			{
				pid_t		pid;

				/*
				 * Now find out who to throw out of the balloon.
				 */
				Assert(VirtualTransactionIdIsValid(*waitlist));
				pid = CancelVirtualTransaction(*waitlist, reason);

				/*
				 * Wait a little bit for it to die so that we avoid flooding
				 * an unresponsive backend when system is heavily loaded.
				 */
				if (pid != 0)
					pg_usleep(5000L);
			}

			if (waitStart != 0 && (!logged_recovery_conflict || !waiting))
			{
				TimestampTz now = 0;
				bool		maybe_log_conflict;
				bool		maybe_update_title;

				maybe_log_conflict = (log_recovery_conflict_waits && !logged_recovery_conflict);
				maybe_update_title = (update_process_title && !waiting);

				/* Get the current timestamp if not report yet */
				if (maybe_log_conflict || maybe_update_title)
					now = GetCurrentTimestamp();

				/*
				 * Report via ps if we have been waiting for more than 500
				 * msec (should that be configurable?)
				 */
				if (maybe_update_title &&
					TimestampDifferenceExceeds(waitStart, now, 500))
				{
					set_ps_display_suffix("waiting");
					waiting = true;
				}

				/*
				 * Emit the log message if the startup process is waiting
				 * longer than deadlock_timeout for recovery conflict.
				 */
				if (maybe_log_conflict &&
					TimestampDifferenceExceeds(waitStart, now, DeadlockTimeout))
				{
					LogRecoveryConflict(reason, waitStart, now, waitlist, true);
					logged_recovery_conflict = true;
				}
			}
		}

		/* The virtual transaction is gone now, wait for the next one */
		waitlist++;
	}

	/*
	 * Emit the log message if recovery conflict was resolved but the startup
	 * process waited longer than deadlock_timeout for it.
	 */
	if (logged_recovery_conflict)
		LogRecoveryConflict(reason, waitStart, GetCurrentTimestamp(),
							NULL, false);

	/* reset ps display to remove the suffix if we added one */
	if (waiting)
		set_ps_display_remove_suffix();

}

/*
 * Generate whatever recovery conflicts are needed to eliminate snapshots that
 * might see XIDs <= snapshotConflictHorizon as still running.
 *
 * snapshotConflictHorizon cutoffs are our standard approach to generating
 * granular recovery conflicts.  Note that InvalidTransactionId values are
 * interpreted as "definitely don't need any conflicts" here, which is a
 * general convention that WAL records can (and often do) depend on.
 */
void
ResolveRecoveryConflictWithSnapshot(TransactionId snapshotConflictHorizon,
									bool isCatalogRel,
									RelFileLocator locator)
{
	VirtualTransactionId *backends;

	/*
	 * If we get passed InvalidTransactionId then we do nothing (no conflict).
	 *
	 * This can happen when replaying already-applied WAL records after a
	 * standby crash or restart, or when replaying an XLOG_HEAP2_VISIBLE
	 * record that marks as frozen a page which was already all-visible.  It's
	 * also quite common with records generated during index deletion
	 * (original execution of the deletion can reason that a recovery conflict
	 * which is sufficient for the deletion operation must take place before
	 * replay of the deletion record itself).
	 */
	if (!TransactionIdIsValid(snapshotConflictHorizon))
		return;

	Assert(TransactionIdIsNormal(snapshotConflictHorizon));
	backends = GetConflictingVirtualXIDs(snapshotConflictHorizon,
										 locator.dbOid);
	ResolveRecoveryConflictWithVirtualXIDs(backends,
										   PROCSIG_RECOVERY_CONFLICT_SNAPSHOT,
										   WAIT_EVENT_RECOVERY_CONFLICT_SNAPSHOT,
										   true);

	/*
	 * Note that WaitExceedsMaxStandbyDelay() is not taken into account here
	 * (as opposed to ResolveRecoveryConflictWithVirtualXIDs() above). That
	 * seems OK, given that this kind of conflict should not normally be
	 * reached, e.g. due to using a physical replication slot.
	 */
	if (wal_level >= WAL_LEVEL_LOGICAL && isCatalogRel)
		InvalidateObsoleteReplicationSlots(RS_INVAL_HORIZON, 0, locator.dbOid,
										   snapshotConflictHorizon);
}

/*
 * Variant of ResolveRecoveryConflictWithSnapshot that works with
 * FullTransactionId values
 */
void
ResolveRecoveryConflictWithSnapshotFullXid(FullTransactionId snapshotConflictHorizon,
										   bool isCatalogRel,
										   RelFileLocator locator)
{
	/*
	 * ResolveRecoveryConflictWithSnapshot operates on 32-bit TransactionIds,
	 * so truncate the logged FullTransactionId.  If the logged value is very
	 * old, so that XID wrap-around already happened on it, there can't be any
	 * snapshots that still see it.
	 */
	FullTransactionId nextXid = ReadNextFullTransactionId();
	uint64		diff;

	diff = U64FromFullTransactionId(nextXid) -
		U64FromFullTransactionId(snapshotConflictHorizon);
	if (diff < MaxTransactionId / 2)
	{
		TransactionId truncated;

		truncated = XidFromFullTransactionId(snapshotConflictHorizon);
		ResolveRecoveryConflictWithSnapshot(truncated,
											isCatalogRel,
											locator);
	}
}

void
ResolveRecoveryConflictWithTablespace(Oid tsid)
{
	VirtualTransactionId *temp_file_users;

	/*
	 * Standby users may be currently using this tablespace for their
	 * temporary files. We only care about current users because
	 * temp_tablespace parameter will just ignore tablespaces that no longer
	 * exist.
	 *
	 * Ask everybody to cancel their queries immediately so we can ensure no
	 * temp files remain and we can remove the tablespace. Nuke the entire
	 * site from orbit, it's the only way to be sure.
	 *
	 * XXX: We could work out the pids of active backends using this
	 * tablespace by examining the temp filenames in the directory. We would
	 * then convert the pids into VirtualXIDs before attempting to cancel
	 * them.
	 *
	 * We don't wait for commit because drop tablespace is non-transactional.
	 */
	temp_file_users = GetConflictingVirtualXIDs(InvalidTransactionId,
												InvalidOid);
	ResolveRecoveryConflictWithVirtualXIDs(temp_file_users,
										   PROCSIG_RECOVERY_CONFLICT_TABLESPACE,
										   WAIT_EVENT_RECOVERY_CONFLICT_TABLESPACE,
										   true);
}

void
ResolveRecoveryConflictWithDatabase(Oid dbid)
{
	/*
	 * We don't do ResolveRecoveryConflictWithVirtualXIDs() here since that
	 * only waits for transactions and completely idle sessions would block
	 * us. This is rare enough that we do this as simply as possible: no wait,
	 * just force them off immediately.
	 *
	 * No locking is required here because we already acquired
	 * AccessExclusiveLock. Anybody trying to connect while we do this will
	 * block during InitPostgres() and then disconnect when they see the
	 * database has been removed.
	 */
	while (CountDBBackends(dbid) > 0)
	{
		CancelDBBackends(dbid, PROCSIG_RECOVERY_CONFLICT_DATABASE, true);

		/*
		 * Wait awhile for them to die so that we avoid flooding an
		 * unresponsive backend when system is heavily loaded.
		 */
		pg_usleep(10000);
	}
}

/*
 * ResolveRecoveryConflictWithLock is called from ProcSleep()
 * to resolve conflicts with other backends holding relation locks.
 *
 * The WaitLatch sleep normally done in ProcSleep()
 * (when not InHotStandby) is performed here, for code clarity.
 *
 * We either resolve conflicts immediately or set a timeout to wake us at
 * the limit of our patience.
 *
 * Resolve conflicts by canceling to all backends holding a conflicting
 * lock.  As we are already queued to be granted the lock, no new lock
 * requests conflicting with ours will be granted in the meantime.
 *
 * We also must check for deadlocks involving the Startup process and
 * hot-standby backend processes. If deadlock_timeout is reached in
 * this function, all the backends holding the conflicting locks are
 * requested to check themselves for deadlocks.
 *
 * logging_conflict should be true if the recovery conflict has not been
 * logged yet even though logging is enabled. After deadlock_timeout is
 * reached and the request for deadlock check is sent, we wait again to
 * be signaled by the release of the lock if logging_conflict is false.
 * Otherwise we return without waiting again so that the caller can report
 * the recovery conflict. In this case, then, this function is called again
 * with logging_conflict=false (because the recovery conflict has already
 * been logged) and we will wait again for the lock to be released.
 */
void
ResolveRecoveryConflictWithLock(LOCKTAG locktag, bool logging_conflict)
{
	TimestampTz ltime;
	TimestampTz now;

	Assert(InHotStandby);

	ltime = GetStandbyLimitTime();
	now = GetCurrentTimestamp();

	/*
	 * Update waitStart if first time through after the startup process
	 * started waiting for the lock. It should not be updated every time
	 * ResolveRecoveryConflictWithLock() is called during the wait.
	 *
	 * Use the current time obtained for comparison with ltime as waitStart
	 * (i.e., the time when this process started waiting for the lock). Since
	 * getting the current time newly can cause overhead, we reuse the
	 * already-obtained time to avoid that overhead.
	 *
	 * Note that waitStart is updated without holding the lock table's
	 * partition lock, to avoid the overhead by additional lock acquisition.
	 * This can cause "waitstart" in pg_locks to become NULL for a very short
	 * period of time after the wait started even though "granted" is false.
	 * This is OK in practice because we can assume that users are likely to
	 * look at "waitstart" when waiting for the lock for a long time.
	 */
	if (pg_atomic_read_u64(&MyProc->waitStart) == 0)
		pg_atomic_write_u64(&MyProc->waitStart, now);

	if (now >= ltime && ltime != 0)
	{
		/*
		 * We're already behind, so clear a path as quickly as possible.
		 */
		VirtualTransactionId *backends;

		backends = GetLockConflicts(&locktag, AccessExclusiveLock, NULL);

		/*
		 * Prevent ResolveRecoveryConflictWithVirtualXIDs() from reporting
		 * "waiting" in PS display by disabling its argument report_waiting
		 * because the caller, WaitOnLock(), has already reported that.
		 */
		ResolveRecoveryConflictWithVirtualXIDs(backends,
											   PROCSIG_RECOVERY_CONFLICT_LOCK,
											   PG_WAIT_LOCK | locktag.locktag_type,
											   false);
	}
	else
	{
		/*
		 * Wait (or wait again) until ltime, and check for deadlocks as well
		 * if we will be waiting longer than deadlock_timeout
		 */
		EnableTimeoutParams timeouts[2];
		int			cnt = 0;

		if (ltime != 0)
		{
			got_standby_lock_timeout = false;
			timeouts[cnt].id = STANDBY_LOCK_TIMEOUT;
			timeouts[cnt].type = TMPARAM_AT;
			timeouts[cnt].fin_time = ltime;
			cnt++;
		}

		got_standby_deadlock_timeout = false;
		timeouts[cnt].id = STANDBY_DEADLOCK_TIMEOUT;
		timeouts[cnt].type = TMPARAM_AFTER;
		timeouts[cnt].delay_ms = DeadlockTimeout;
		cnt++;

		enable_timeouts(timeouts, cnt);
	}

	/* Wait to be signaled by the release of the Relation Lock */
	ProcWaitForSignal(PG_WAIT_LOCK | locktag.locktag_type);

	/*
	 * Exit if ltime is reached. Then all the backends holding conflicting
	 * locks will be canceled in the next ResolveRecoveryConflictWithLock()
	 * call.
	 */
	if (got_standby_lock_timeout)
		goto cleanup;

	if (got_standby_deadlock_timeout)
	{
		VirtualTransactionId *backends;

		backends = GetLockConflicts(&locktag, AccessExclusiveLock, NULL);

		/* Quick exit if there's no work to be done */
		if (!VirtualTransactionIdIsValid(*backends))
			goto cleanup;

		/*
		 * Send signals to all the backends holding the conflicting locks, to
		 * ask them to check themselves for deadlocks.
		 */
		while (VirtualTransactionIdIsValid(*backends))
		{
			SignalVirtualTransaction(*backends,
									 PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK,
									 false);
			backends++;
		}

		/*
		 * Exit if the recovery conflict has not been logged yet even though
		 * logging is enabled, so that the caller can log that. Then
		 * RecoveryConflictWithLock() is called again and we will wait again
		 * for the lock to be released.
		 */
		if (logging_conflict)
			goto cleanup;

		/*
		 * Wait again here to be signaled by the release of the Relation Lock,
		 * to prevent the subsequent RecoveryConflictWithLock() from causing
		 * deadlock_timeout and sending a request for deadlocks check again.
		 * Otherwise the request continues to be sent every deadlock_timeout
		 * until the relation locks are released or ltime is reached.
		 */
		got_standby_deadlock_timeout = false;
		ProcWaitForSignal(PG_WAIT_LOCK | locktag.locktag_type);
	}

cleanup:

	/*
	 * Clear any timeout requests established above.  We assume here that the
	 * Startup process doesn't have any other outstanding timeouts than those
	 * used by this function. If that stops being true, we could cancel the
	 * timeouts individually, but that'd be slower.
	 */
	disable_all_timeouts(false);
	got_standby_lock_timeout = false;
	got_standby_deadlock_timeout = false;
}

/*
 * ResolveRecoveryConflictWithBufferPin is called from LockBufferForCleanup()
 * to resolve conflicts with other backends holding buffer pins.
 *
 * The ProcWaitForSignal() sleep normally done in LockBufferForCleanup()
 * (when not InHotStandby) is performed here, for code clarity.
 *
 * We either resolve conflicts immediately or set a timeout to wake us at
 * the limit of our patience.
 *
 * Resolve conflicts by sending a PROCSIG signal to all backends to check if
 * they hold one of the buffer pins that is blocking Startup process. If so,
 * those backends will take an appropriate error action, ERROR or FATAL.
 *
 * We also must check for deadlocks.  Deadlocks occur because if queries
 * wait on a lock, that must be behind an AccessExclusiveLock, which can only
 * be cleared if the Startup process replays a transaction completion record.
 * If Startup process is also waiting then that is a deadlock. The deadlock
 * can occur if the query is waiting and then the Startup sleeps, or if
 * Startup is sleeping and the query waits on a lock. We protect against
 * only the former sequence here, the latter sequence is checked prior to
 * the query sleeping, in CheckRecoveryConflictDeadlock().
 *
 * Deadlocks are extremely rare, and relatively expensive to check for,
 * so we don't do a deadlock check right away ... only if we have had to wait
 * at least deadlock_timeout.
 */
void
ResolveRecoveryConflictWithBufferPin(void)
{
	TimestampTz ltime;

	Assert(InHotStandby);

	ltime = GetStandbyLimitTime();

	if (GetCurrentTimestamp() >= ltime && ltime != 0)
	{
		/*
		 * We're already behind, so clear a path as quickly as possible.
		 */
		SendRecoveryConflictWithBufferPin(PROCSIG_RECOVERY_CONFLICT_BUFFERPIN);
	}
	else
	{
		/*
		 * Wake up at ltime, and check for deadlocks as well if we will be
		 * waiting longer than deadlock_timeout
		 */
		EnableTimeoutParams timeouts[2];
		int			cnt = 0;

		if (ltime != 0)
		{
			timeouts[cnt].id = STANDBY_TIMEOUT;
			timeouts[cnt].type = TMPARAM_AT;
			timeouts[cnt].fin_time = ltime;
			cnt++;
		}

		got_standby_deadlock_timeout = false;
		timeouts[cnt].id = STANDBY_DEADLOCK_TIMEOUT;
		timeouts[cnt].type = TMPARAM_AFTER;
		timeouts[cnt].delay_ms = DeadlockTimeout;
		cnt++;

		enable_timeouts(timeouts, cnt);
	}

	/*
	 * Wait to be signaled by UnpinBuffer() or for the wait to be interrupted
	 * by one of the timeouts established above.
	 *
	 * We assume that only UnpinBuffer() and the timeout requests established
	 * above can wake us up here. WakeupRecovery() called by walreceiver or
	 * SIGHUP signal handler, etc cannot do that because it uses the different
	 * latch from that ProcWaitForSignal() waits on.
	 */
	ProcWaitForSignal(WAIT_EVENT_BUFFER_PIN);

	if (got_standby_delay_timeout)
		SendRecoveryConflictWithBufferPin(PROCSIG_RECOVERY_CONFLICT_BUFFERPIN);
	else if (got_standby_deadlock_timeout)
	{
		/*
		 * Send out a request for hot-standby backends to check themselves for
		 * deadlocks.
		 *
		 * XXX The subsequent ResolveRecoveryConflictWithBufferPin() will wait
		 * to be signaled by UnpinBuffer() again and send a request for
		 * deadlocks check if deadlock_timeout happens. This causes the
		 * request to continue to be sent every deadlock_timeout until the
		 * buffer is unpinned or ltime is reached. This would increase the
		 * workload in the startup process and backends. In practice it may
		 * not be so harmful because the period that the buffer is kept pinned
		 * is basically no so long. But we should fix this?
		 */
		SendRecoveryConflictWithBufferPin(PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK);
	}

	/*
	 * Clear any timeout requests established above.  We assume here that the
	 * Startup process doesn't have any other timeouts than what this function
	 * uses.  If that stops being true, we could cancel the timeouts
	 * individually, but that'd be slower.
	 */
	disable_all_timeouts(false);
	got_standby_delay_timeout = false;
	got_standby_deadlock_timeout = false;
}

static void
SendRecoveryConflictWithBufferPin(ProcSignalReason reason)
{
	Assert(reason == PROCSIG_RECOVERY_CONFLICT_BUFFERPIN ||
		   reason == PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK);

	/*
	 * We send signal to all backends to ask them if they are holding the
	 * buffer pin which is delaying the Startup process. We must not set the
	 * conflict flag yet, since most backends will be innocent. Let the
	 * SIGUSR1 handling in each backend decide their own fate.
	 */
	CancelDBBackends(InvalidOid, reason, false);
}

/*
 * In Hot Standby perform early deadlock detection.  We abort the lock
 * wait if we are about to sleep while holding the buffer pin that Startup
 * process is waiting for.
 *
 * Note: this code is pessimistic, because there is no way for it to
 * determine whether an actual deadlock condition is present: the lock we
 * need to wait for might be unrelated to any held by the Startup process.
 * Sooner or later, this mechanism should get ripped out in favor of somehow
 * accounting for buffer locks in DeadLockCheck().  However, errors here
 * seem to be very low-probability in practice, so for now it's not worth
 * the trouble.
 */
void
CheckRecoveryConflictDeadlock(void)
{
	Assert(!InRecovery);		/* do not call in Startup process */

	if (!HoldingBufferPinThatDelaysRecovery())
		return;

	/*
	 * Error message should match ProcessInterrupts() but we avoid calling
	 * that because we aren't handling an interrupt at this point. Note that
	 * we only cancel the current transaction here, so if we are in a
	 * subtransaction and the pin is held by a parent, then the Startup
	 * process will continue to wait even though we have avoided deadlock.
	 */
	ereport(ERROR,
			(errcode(ERRCODE_T_R_DEADLOCK_DETECTED),
			 errmsg("canceling statement due to conflict with recovery"),
			 errdetail("User transaction caused buffer deadlock with recovery.")));
}


/* --------------------------------
 *		timeout handler routines
 * --------------------------------
 */

/*
 * StandbyDeadLockHandler() will be called if STANDBY_DEADLOCK_TIMEOUT is
 * exceeded.
 */
void
StandbyDeadLockHandler(void)
{
	got_standby_deadlock_timeout = true;
}

/*
 * StandbyTimeoutHandler() will be called if STANDBY_TIMEOUT is exceeded.
 */
void
StandbyTimeoutHandler(void)
{
	got_standby_delay_timeout = true;
}

/*
 * StandbyLockTimeoutHandler() will be called if STANDBY_LOCK_TIMEOUT is exceeded.
 */
void
StandbyLockTimeoutHandler(void)
{
	got_standby_lock_timeout = true;
}

/*
 * -----------------------------------------------------
 * Locking in Recovery Mode
 * -----------------------------------------------------
 *
 * All locks are held by the Startup process using a single virtual
 * transaction. This implementation is both simpler and in some senses,
 * more correct. The locks held mean "some original transaction held
 * this lock, so query access is not allowed at this time". So the Startup
 * process is the proxy by which the original locks are implemented.
 *
 * We only keep track of AccessExclusiveLocks, which are only ever held by
 * one transaction on one relation.
 *
 * We keep a table of known locks in the RecoveryLockHash hash table.
 * The point of that table is to let us efficiently de-duplicate locks,
 * which is important because checkpoints will re-report the same locks
 * already held.  There is also a RecoveryLockXidHash table with one entry
 * per xid, which allows us to efficiently find all the locks held by a
 * given original transaction.
 *
 * We use session locks rather than normal locks so we don't need
 * ResourceOwners.
 */


void
StandbyAcquireAccessExclusiveLock(TransactionId xid, Oid dbOid, Oid relOid)
{
	RecoveryLockXidEntry *xidentry;
	RecoveryLockEntry *lockentry;
	xl_standby_lock key;
	LOCKTAG		locktag;
	bool		found;

	/* Already processed? */
	if (!TransactionIdIsValid(xid) ||
		TransactionIdDidCommit(xid) ||
		TransactionIdDidAbort(xid))
		return;

	elog(DEBUG4, "adding recovery lock: db %u rel %u", dbOid, relOid);

	/* dbOid is InvalidOid when we are locking a shared relation. */
	Assert(OidIsValid(relOid));

	/* Create a hash entry for this xid, if we don't have one already. */
	xidentry = hash_search(RecoveryLockXidHash, &xid, HASH_ENTER, &found);
	if (!found)
	{
		Assert(xidentry->xid == xid);	/* dynahash should have set this */
		xidentry->head = NULL;
	}

	/* Create a hash entry for this lock, unless we have one already. */
	key.xid = xid;
	key.dbOid = dbOid;
	key.relOid = relOid;
	lockentry = hash_search(RecoveryLockHash, &key, HASH_ENTER, &found);
	if (!found)
	{
		/* It's new, so link it into the XID's list ... */
		lockentry->next = xidentry->head;
		xidentry->head = lockentry;

		/* ... and acquire the lock locally. */
		SET_LOCKTAG_RELATION(locktag, dbOid, relOid);

		(void) LockAcquire(&locktag, AccessExclusiveLock, true, false);
	}
}

/*
 * Release all the locks associated with this RecoveryLockXidEntry.
 */
static void
StandbyReleaseXidEntryLocks(RecoveryLockXidEntry *xidentry)
{
	RecoveryLockEntry *entry;
	RecoveryLockEntry *next;

	for (entry = xidentry->head; entry != NULL; entry = next)
	{
		LOCKTAG		locktag;

		elog(DEBUG4,
			 "releasing recovery lock: xid %u db %u rel %u",
			 entry->key.xid, entry->key.dbOid, entry->key.relOid);
		/* Release the lock ... */
		SET_LOCKTAG_RELATION(locktag, entry->key.dbOid, entry->key.relOid);
		if (!LockRelease(&locktag, AccessExclusiveLock, true))
		{
			elog(LOG,
				 "RecoveryLockHash contains entry for lock no longer recorded by lock manager: xid %u database %u relation %u",
				 entry->key.xid, entry->key.dbOid, entry->key.relOid);
			Assert(false);
		}
		/* ... and remove the per-lock hash entry */
		next = entry->next;
		hash_search(RecoveryLockHash, entry, HASH_REMOVE, NULL);
	}

	xidentry->head = NULL;		/* just for paranoia */
}

/*
 * Release locks for specific XID, or all locks if it's InvalidXid.
 */
static void
StandbyReleaseLocks(TransactionId xid)
{
	RecoveryLockXidEntry *entry;

	if (TransactionIdIsValid(xid))
	{
		if ((entry = hash_search(RecoveryLockXidHash, &xid, HASH_FIND, NULL)))
		{
			StandbyReleaseXidEntryLocks(entry);
			hash_search(RecoveryLockXidHash, entry, HASH_REMOVE, NULL);
		}
	}
	else
		StandbyReleaseAllLocks();
}

/*
 * Release locks for a transaction tree, starting at xid down, from
 * RecoveryLockXidHash.
 *
 * Called during WAL replay of COMMIT/ROLLBACK when in hot standby mode,
 * to remove any AccessExclusiveLocks requested by a transaction.
 */
void
StandbyReleaseLockTree(TransactionId xid, int nsubxids, TransactionId *subxids)
{
	int			i;

	StandbyReleaseLocks(xid);

	for (i = 0; i < nsubxids; i++)
		StandbyReleaseLocks(subxids[i]);
}

/*
 * Called at end of recovery and when we see a shutdown checkpoint.
 */
void
StandbyReleaseAllLocks(void)
{
	HASH_SEQ_STATUS status;
	RecoveryLockXidEntry *entry;

	elog(DEBUG2, "release all standby locks");

	hash_seq_init(&status, RecoveryLockXidHash);
	while ((entry = hash_seq_search(&status)))
	{
		StandbyReleaseXidEntryLocks(entry);
		hash_search(RecoveryLockXidHash, entry, HASH_REMOVE, NULL);
	}
}

/*
 * StandbyReleaseOldLocks
 *		Release standby locks held by top-level XIDs that aren't running,
 *		as long as they're not prepared transactions.
 *
 * This is needed to prune the locks of crashed transactions, which didn't
 * write an ABORT/COMMIT record.
 */
void
StandbyReleaseOldLocks(TransactionId oldxid)
{
	HASH_SEQ_STATUS status;
	RecoveryLockXidEntry *entry;

	hash_seq_init(&status, RecoveryLockXidHash);
	while ((entry = hash_seq_search(&status)))
	{
		Assert(TransactionIdIsValid(entry->xid));

		/* Skip if prepared transaction. */
		if (StandbyTransactionIdIsPrepared(entry->xid))
			continue;

		/* Skip if >= oldxid. */
		if (!TransactionIdPrecedes(entry->xid, oldxid))
			continue;

		/* Remove all locks and hash table entry. */
		StandbyReleaseXidEntryLocks(entry);
		hash_search(RecoveryLockXidHash, entry, HASH_REMOVE, NULL);
	}
}

/*
 * --------------------------------------------------------------------
 *		Recovery handling for Rmgr RM_STANDBY_ID
 *
 * These record types will only be created if XLogStandbyInfoActive()
 * --------------------------------------------------------------------
 */

void
standby_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	/* Backup blocks are not used in standby records */
	Assert(!XLogRecHasAnyBlockRefs(record));

	/* Do nothing if we're not in hot standby mode */
	if (standbyState == STANDBY_DISABLED)
		return;

	if (info == XLOG_STANDBY_LOCK)
	{
		xl_standby_locks *xlrec = (xl_standby_locks *) XLogRecGetData(record);
		int			i;

		for (i = 0; i < xlrec->nlocks; i++)
			StandbyAcquireAccessExclusiveLock(xlrec->locks[i].xid,
											  xlrec->locks[i].dbOid,
											  xlrec->locks[i].relOid);
	}
	else if (info == XLOG_RUNNING_XACTS)
	{
		xl_running_xacts *xlrec = (xl_running_xacts *) XLogRecGetData(record);
		RunningTransactionsData running;

		running.xcnt = xlrec->xcnt;
		running.subxcnt = xlrec->subxcnt;
		running.subxid_status = xlrec->subxid_overflow ? SUBXIDS_MISSING : SUBXIDS_IN_ARRAY;
		running.nextXid = xlrec->nextXid;
		running.latestCompletedXid = xlrec->latestCompletedXid;
		running.oldestRunningXid = xlrec->oldestRunningXid;
		running.xids = xlrec->xids;

		ProcArrayApplyRecoveryInfo(&running);

		/*
		 * The startup process currently has no convenient way to schedule
		 * stats to be reported. XLOG_RUNNING_XACTS records issued at a
		 * regular cadence, making this a convenient location to report stats.
		 * While these records aren't generated with wal_level=minimal, stats
		 * also cannot be accessed during WAL replay.
		 */
		pgstat_report_stat(true);
	}
	else if (info == XLOG_INVALIDATIONS)
	{
		xl_invalidations *xlrec = (xl_invalidations *) XLogRecGetData(record);

		ProcessCommittedInvalidationMessages(xlrec->msgs,
											 xlrec->nmsgs,
											 xlrec->relcacheInitFileInval,
											 xlrec->dbId,
											 xlrec->tsId);
	}
	else
		elog(PANIC, "standby_redo: unknown op code %u", info);
}

/*
 * Log details of the current snapshot to WAL. This allows the snapshot state
 * to be reconstructed on the standby and for logical decoding.
 *
 * This is used for Hot Standby as follows:
 *
 * We can move directly to STANDBY_SNAPSHOT_READY at startup if we
 * start from a shutdown checkpoint because we know nothing was running
 * at that time and our recovery snapshot is known empty. In the more
 * typical case of an online checkpoint we need to jump through a few
 * hoops to get a correct recovery snapshot and this requires a two or
 * sometimes a three stage process.
 *
 * The initial snapshot must contain all running xids and all current
 * AccessExclusiveLocks at a point in time on the standby. Assembling
 * that information while the server is running requires many and
 * various LWLocks, so we choose to derive that information piece by
 * piece and then re-assemble that info on the standby. When that
 * information is fully assembled we move to STANDBY_SNAPSHOT_READY.
 *
 * Since locking on the primary when we derive the information is not
 * strict, we note that there is a time window between the derivation and
 * writing to WAL of the derived information. That allows race conditions
 * that we must resolve, since xids and locks may enter or leave the
 * snapshot during that window. This creates the issue that an xid or
 * lock may start *after* the snapshot has been derived yet *before* the
 * snapshot is logged in the running xacts WAL record. We resolve this by
 * starting to accumulate changes at a point just prior to when we derive
 * the snapshot on the primary, then ignore duplicates when we later apply
 * the snapshot from the running xacts record. This is implemented during
 * CreateCheckPoint() where we use the logical checkpoint location as
 * our starting point and then write the running xacts record immediately
 * before writing the main checkpoint WAL record. Since we always start
 * up from a checkpoint and are immediately at our starting point, we
 * unconditionally move to STANDBY_INITIALIZED. After this point we
 * must do 4 things:
 *	* move shared nextXid forwards as we see new xids
 *	* extend the clog and subtrans with each new xid
 *	* keep track of uncommitted known assigned xids
 *	* keep track of uncommitted AccessExclusiveLocks
 *
 * When we see a commit/abort we must remove known assigned xids and locks
 * from the completing transaction. Attempted removals that cannot locate
 * an entry are expected and must not cause an error when we are in state
 * STANDBY_INITIALIZED. This is implemented in StandbyReleaseLocks() and
 * KnownAssignedXidsRemove().
 *
 * Later, when we apply the running xact data we must be careful to ignore
 * transactions already committed, since those commits raced ahead when
 * making WAL entries.
 *
 * For logical decoding only the running xacts information is needed;
 * there's no need to look at the locking information, but it's logged anyway,
 * as there's no independent knob to just enable logical decoding. For
 * details of how this is used, check snapbuild.c's introductory comment.
 *
 *
 * Returns the RecPtr of the last inserted record.
 */
XLogRecPtr
LogStandbySnapshot(void)
{
	XLogRecPtr	recptr;
	RunningTransactions running;
	xl_standby_lock *locks;
	int			nlocks;

	Assert(XLogStandbyInfoActive());

#ifdef USE_INJECTION_POINTS
	if (IS_INJECTION_POINT_ATTACHED("skip-log-running-xacts"))
	{
		/*
		 * This record could move slot's xmin forward during decoding, leading
		 * to unpredictable results, so skip it when requested by the test.
		 */
		return GetInsertRecPtr();
	}
#endif

	/*
	 * Get details of any AccessExclusiveLocks being held at the moment.
	 */
	locks = GetRunningTransactionLocks(&nlocks);
	if (nlocks > 0)
		LogAccessExclusiveLocks(nlocks, locks);
	pfree(locks);

	/*
	 * Log details of all in-progress transactions. This should be the last
	 * record we write, because standby will open up when it sees this.
	 */
	running = GetRunningTransactionData();

	/*
	 * GetRunningTransactionData() acquired ProcArrayLock, we must release it.
	 * For Hot Standby this can be done before inserting the WAL record
	 * because ProcArrayApplyRecoveryInfo() rechecks the commit status using
	 * the clog. For logical decoding, though, the lock can't be released
	 * early because the clog might be "in the future" from the POV of the
	 * historic snapshot. This would allow for situations where we're waiting
	 * for the end of a transaction listed in the xl_running_xacts record
	 * which, according to the WAL, has committed before the xl_running_xacts
	 * record. Fortunately this routine isn't executed frequently, and it's
	 * only a shared lock.
	 */
	if (wal_level < WAL_LEVEL_LOGICAL)
		LWLockRelease(ProcArrayLock);

	recptr = LogCurrentRunningXacts(running);

	/* Release lock if we kept it longer ... */
	if (wal_level >= WAL_LEVEL_LOGICAL)
		LWLockRelease(ProcArrayLock);

	/* GetRunningTransactionData() acquired XidGenLock, we must release it */
	LWLockRelease(XidGenLock);

	return recptr;
}

/*
 * Record an enhanced snapshot of running transactions into WAL.
 *
 * The definitions of RunningTransactionsData and xl_running_xacts are
 * similar. We keep them separate because xl_running_xacts is a contiguous
 * chunk of memory and never exists fully until it is assembled in WAL.
 * The inserted records are marked as not being important for durability,
 * to avoid triggering superfluous checkpoint / archiving activity.
 */
static XLogRecPtr
LogCurrentRunningXacts(RunningTransactions CurrRunningXacts)
{
	xl_running_xacts xlrec;
	XLogRecPtr	recptr;

	xlrec.xcnt = CurrRunningXacts->xcnt;
	xlrec.subxcnt = CurrRunningXacts->subxcnt;
	xlrec.subxid_overflow = (CurrRunningXacts->subxid_status != SUBXIDS_IN_ARRAY);
	xlrec.nextXid = CurrRunningXacts->nextXid;
	xlrec.oldestRunningXid = CurrRunningXacts->oldestRunningXid;
	xlrec.latestCompletedXid = CurrRunningXacts->latestCompletedXid;

	/* Header */
	XLogBeginInsert();
	XLogSetRecordFlags(XLOG_MARK_UNIMPORTANT);
	XLogRegisterData(&xlrec, MinSizeOfXactRunningXacts);

	/* array of TransactionIds */
	if (xlrec.xcnt > 0)
		XLogRegisterData(CurrRunningXacts->xids,
						 (xlrec.xcnt + xlrec.subxcnt) * sizeof(TransactionId));

	recptr = XLogInsert(RM_STANDBY_ID, XLOG_RUNNING_XACTS);

	if (xlrec.subxid_overflow)
		elog(DEBUG2,
			 "snapshot of %d running transactions overflowed (lsn %X/%08X oldest xid %u latest complete %u next xid %u)",
			 CurrRunningXacts->xcnt,
			 LSN_FORMAT_ARGS(recptr),
			 CurrRunningXacts->oldestRunningXid,
			 CurrRunningXacts->latestCompletedXid,
			 CurrRunningXacts->nextXid);
	else
		elog(DEBUG2,
			 "snapshot of %d+%d running transaction ids (lsn %X/%08X oldest xid %u latest complete %u next xid %u)",
			 CurrRunningXacts->xcnt, CurrRunningXacts->subxcnt,
			 LSN_FORMAT_ARGS(recptr),
			 CurrRunningXacts->oldestRunningXid,
			 CurrRunningXacts->latestCompletedXid,
			 CurrRunningXacts->nextXid);

	/*
	 * Ensure running_xacts information is synced to disk not too far in the
	 * future. We don't want to stall anything though (i.e. use XLogFlush()),
	 * so we let the wal writer do it during normal operation.
	 * XLogSetAsyncXactLSN() conveniently will mark the LSN as to-be-synced
	 * and nudge the WALWriter into action if sleeping. Check
	 * XLogBackgroundFlush() for details why a record might not be flushed
	 * without it.
	 */
	XLogSetAsyncXactLSN(recptr);

	return recptr;
}

/*
 * Wholesale logging of AccessExclusiveLocks. Other lock types need not be
 * logged, as described in backend/storage/lmgr/README.
 */
static void
LogAccessExclusiveLocks(int nlocks, xl_standby_lock *locks)
{
	xl_standby_locks xlrec;

	xlrec.nlocks = nlocks;

	XLogBeginInsert();
	XLogRegisterData(&xlrec, offsetof(xl_standby_locks, locks));
	XLogRegisterData(locks, nlocks * sizeof(xl_standby_lock));
	XLogSetRecordFlags(XLOG_MARK_UNIMPORTANT);

	(void) XLogInsert(RM_STANDBY_ID, XLOG_STANDBY_LOCK);
}

/*
 * Individual logging of AccessExclusiveLocks for use during LockAcquire()
 */
void
LogAccessExclusiveLock(Oid dbOid, Oid relOid)
{
	xl_standby_lock xlrec;

	xlrec.xid = GetCurrentTransactionId();

	xlrec.dbOid = dbOid;
	xlrec.relOid = relOid;

	LogAccessExclusiveLocks(1, &xlrec);
	MyXactFlags |= XACT_FLAGS_ACQUIREDACCESSEXCLUSIVELOCK;
}

/*
 * Prepare to log an AccessExclusiveLock, for use during LockAcquire()
 */
void
LogAccessExclusiveLockPrepare(void)
{
	/*
	 * Ensure that a TransactionId has been assigned to this transaction, for
	 * two reasons, both related to lock release on the standby. First, we
	 * must assign an xid so that RecordTransactionCommit() and
	 * RecordTransactionAbort() do not optimise away the transaction
	 * completion record which recovery relies upon to release locks. It's a
	 * hack, but for a corner case not worth adding code for into the main
	 * commit path. Second, we must assign an xid before the lock is recorded
	 * in shared memory, otherwise a concurrently executing
	 * GetRunningTransactionLocks() might see a lock associated with an
	 * InvalidTransactionId which we later assert cannot happen.
	 */
	(void) GetCurrentTransactionId();
}

/*
 * Emit WAL for invalidations. This currently is only used for commits without
 * an xid but which contain invalidations.
 */
void
LogStandbyInvalidations(int nmsgs, SharedInvalidationMessage *msgs,
						bool relcacheInitFileInval)
{
	xl_invalidations xlrec;

	/* prepare record */
	memset(&xlrec, 0, sizeof(xlrec));
	xlrec.dbId = MyDatabaseId;
	xlrec.tsId = MyDatabaseTableSpace;
	xlrec.relcacheInitFileInval = relcacheInitFileInval;
	xlrec.nmsgs = nmsgs;

	/* perform insertion */
	XLogBeginInsert();
	XLogRegisterData(&xlrec, MinSizeOfInvalidations);
	XLogRegisterData(msgs,
					 nmsgs * sizeof(SharedInvalidationMessage));
	XLogInsert(RM_STANDBY_ID, XLOG_INVALIDATIONS);
}

/* Return the description of recovery conflict */
static const char *
get_recovery_conflict_desc(ProcSignalReason reason)
{
	const char *reasonDesc = _("unknown reason");

	switch (reason)
	{
		case PROCSIG_RECOVERY_CONFLICT_BUFFERPIN:
			reasonDesc = _("recovery conflict on buffer pin");
			break;
		case PROCSIG_RECOVERY_CONFLICT_LOCK:
			reasonDesc = _("recovery conflict on lock");
			break;
		case PROCSIG_RECOVERY_CONFLICT_TABLESPACE:
			reasonDesc = _("recovery conflict on tablespace");
			break;
		case PROCSIG_RECOVERY_CONFLICT_SNAPSHOT:
			reasonDesc = _("recovery conflict on snapshot");
			break;
		case PROCSIG_RECOVERY_CONFLICT_LOGICALSLOT:
			reasonDesc = _("recovery conflict on replication slot");
			break;
		case PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK:
			reasonDesc = _("recovery conflict on buffer deadlock");
			break;
		case PROCSIG_RECOVERY_CONFLICT_DATABASE:
			reasonDesc = _("recovery conflict on database");
			break;
		default:
			break;
	}

	return reasonDesc;
}
