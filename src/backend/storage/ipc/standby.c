/*-------------------------------------------------------------------------
 *
 * standby.c
 *	  Misc functions used in Hot Standby mode.
 *
 *  All functions for handling RM_STANDBY_ID, which relate to
 *  AccessExclusiveLocks and starting snapshots for Hot Standby mode.
 *  Plus conflict recovery processing.
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/ipc/standby.c,v 1.13 2010/02/13 16:29:38 sriggs Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/sinvaladt.h"
#include "storage/standby.h"
#include "utils/ps_status.h"

int		vacuum_defer_cleanup_age;

static List *RecoveryLockList;

static void ResolveRecoveryConflictWithVirtualXIDs(VirtualTransactionId *waitlist,
									   ProcSignalReason reason);
static void ResolveRecoveryConflictWithLock(Oid dbOid, Oid relOid);
static void LogCurrentRunningXacts(RunningTransactions CurrRunningXacts);
static void LogAccessExclusiveLocks(int nlocks, xl_standby_lock *locks);

/*
 * InitRecoveryTransactionEnvironment
 *		Initiallize tracking of in-progress transactions in master
 *
 * We need to issue shared invalidations and hold locks. Holding locks
 * means others may want to wait on us, so we need to make lock table
 * inserts to appear like a transaction. We could create and delete
 * lock table entries for each transaction but its simpler just to create
 * one permanent entry and leave it there all the time. Locks are then
 * acquired and released as needed. Yes, this means you can see the
 * Startup process in pg_locks once we have run this.
 */
void
InitRecoveryTransactionEnvironment(void)
{
	VirtualTransactionId vxid;

	/*
	 * Initialise shared invalidation management for Startup process,
	 * being careful to register ourselves as a sendOnly process so
	 * we don't need to read messages, nor will we get signalled
	 * when the queue starts filling up.
	 */
	SharedInvalBackendInit(true);

	/*
	 * Record the PID and PGPROC structure of the startup process.
	 */
	PublishStartupProcessInformation();

	/*
	 * Lock a virtual transaction id for Startup process.
	 *
	 * We need to do GetNextLocalTransactionId() because
	 * SharedInvalBackendInit() leaves localTransactionid invalid and
	 * the lock manager doesn't like that at all.
	 *
	 * Note that we don't need to run XactLockTableInsert() because nobody
	 * needs to wait on xids. That sounds a little strange, but table locks
	 * are held by vxids and row level locks are held by xids. All queries
	 * hold AccessShareLocks so never block while we write or lock new rows.
	 */
	vxid.backendId = MyBackendId;
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
 */
void
ShutdownRecoveryTransactionEnvironment(void)
{
	/* Mark all tracked in-progress transactions as finished. */
	ExpireAllKnownAssignedTransactionIds();

	/* Release all locks the tracked transactions were holding */
	StandbyReleaseAllLocks();
}


/*
 * -----------------------------------------------------
 * 		Standby wait timers and backend cancel logic
 * -----------------------------------------------------
 */

#define STANDBY_INITIAL_WAIT_US  1000
static int standbyWait_us = STANDBY_INITIAL_WAIT_US;

/*
 * Standby wait logic for ResolveRecoveryConflictWithVirtualXIDs.
 * We wait here for a while then return. If we decide we can't wait any
 * more then we return true, if we can wait some more return false.
 */
static bool
WaitExceedsMaxStandbyDelay(void)
{
	long	delay_secs;
	int		delay_usecs;

	if (MaxStandbyDelay == -1)
		return false;

	/* Are we past max_standby_delay? */
	TimestampDifference(GetLatestXLogTime(), GetCurrentTimestamp(),
						&delay_secs, &delay_usecs);
	if (delay_secs > MaxStandbyDelay)
		return true;

	/*
	 * Sleep, then do bookkeeping.
	 */
	pg_usleep(standbyWait_us);

	/*
	 * Progressively increase the sleep times.
	 */
	standbyWait_us *= 2;
	if (standbyWait_us > 1000000)
		standbyWait_us = 1000000;
	if (standbyWait_us > MaxStandbyDelay * 1000000 / 4)
		standbyWait_us = MaxStandbyDelay * 1000000 / 4;

	return false;
}

/*
 * This is the main executioner for any query backend that conflicts with
 * recovery processing. Judgement has already been passed on it within
 * a specific rmgr. Here we just issue the orders to the procs. The procs
 * then throw the required error as instructed.
 */
static void
ResolveRecoveryConflictWithVirtualXIDs(VirtualTransactionId *waitlist,
									   ProcSignalReason reason)
{
	char		waitactivitymsg[100];
	char		oldactivitymsg[101];

	while (VirtualTransactionIdIsValid(*waitlist))
	{
		long wait_s;
		int wait_us;			/* wait in microseconds (us) */
		TimestampTz waitStart;
		bool		logged;

		waitStart = GetCurrentTimestamp();
		standbyWait_us = STANDBY_INITIAL_WAIT_US;
		logged = false;

		/* wait until the virtual xid is gone */
		while(!ConditionalVirtualXactLockTableWait(*waitlist))
		{
			/*
			 * Report if we have been waiting for a while now...
			 */
			TimestampTz now = GetCurrentTimestamp();
			TimestampDifference(waitStart, now, &wait_s, &wait_us);
			if (!logged && (wait_s > 0 || wait_us > 500000))
			{
				const char *oldactivitymsgp;
				int			len;

				oldactivitymsgp = get_ps_display(&len);

				if (len > 100)
					len = 100;

				memcpy(oldactivitymsg, oldactivitymsgp, len);
				oldactivitymsg[len] = 0;

				snprintf(waitactivitymsg, sizeof(waitactivitymsg),
						 "waiting for max_standby_delay (%u s)",
						 MaxStandbyDelay);
				set_ps_display(waitactivitymsg, false);

				pgstat_report_waiting(true);

				logged = true;
			}

			/* Is it time to kill it? */
			if (WaitExceedsMaxStandbyDelay())
			{
				pid_t pid;

				/*
				 * Now find out who to throw out of the balloon.
				 */
				Assert(VirtualTransactionIdIsValid(*waitlist));
				pid = CancelVirtualTransaction(*waitlist, reason);

				/*
				 * Wait awhile for it to die so that we avoid flooding an
				 * unresponsive backend when system is heavily loaded.
				 */
				if (pid != 0)
					pg_usleep(5000);
			}
		}

		/* Reset ps display */
		if (logged)
		{
			set_ps_display(oldactivitymsg, false);
			pgstat_report_waiting(false);
		}

		/* The virtual transaction is gone now, wait for the next one */
		waitlist++;
    }
}

void
ResolveRecoveryConflictWithSnapshot(TransactionId latestRemovedXid, RelFileNode node)
{
	VirtualTransactionId *backends;

	backends = GetConflictingVirtualXIDs(latestRemovedXid,
										 node.dbNode);

	ResolveRecoveryConflictWithVirtualXIDs(backends,
										   PROCSIG_RECOVERY_CONFLICT_SNAPSHOT);
}

void
ResolveRecoveryConflictWithTablespace(Oid tsid)
{
	VirtualTransactionId *temp_file_users;

	/*
	 * Standby users may be currently using this tablespace for
	 * for their temporary files. We only care about current
	 * users because temp_tablespace parameter will just ignore
	 * tablespaces that no longer exist.
	 *
	 * Ask everybody to cancel their queries immediately so
	 * we can ensure no temp files remain and we can remove the
	 * tablespace. Nuke the entire site from orbit, it's the only
	 * way to be sure.
	 *
	 * XXX: We could work out the pids of active backends
	 * using this tablespace by examining the temp filenames in the
	 * directory. We would then convert the pids into VirtualXIDs
	 * before attempting to cancel them.
	 *
	 * We don't wait for commit because drop tablespace is
	 * non-transactional.
	 */
	temp_file_users = GetConflictingVirtualXIDs(InvalidTransactionId,
												InvalidOid);
	ResolveRecoveryConflictWithVirtualXIDs(temp_file_users,
										   PROCSIG_RECOVERY_CONFLICT_TABLESPACE);
}

void
ResolveRecoveryConflictWithDatabase(Oid dbid)
{
	/*
	 * We don't do ResolveRecoveryConflictWithVirutalXIDs() here since
	 * that only waits for transactions and completely idle sessions
	 * would block us. This is rare enough that we do this as simply
	 * as possible: no wait, just force them off immediately.
	 *
	 * No locking is required here because we already acquired
	 * AccessExclusiveLock. Anybody trying to connect while we do this
	 * will block during InitPostgres() and then disconnect when they
	 * see the database has been removed.
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

static void
ResolveRecoveryConflictWithLock(Oid dbOid, Oid relOid)
{
	VirtualTransactionId *backends;
	bool			report_memory_error = false;
	bool			lock_acquired = false;
	int				num_attempts = 0;
	LOCKTAG			locktag;

	SET_LOCKTAG_RELATION(locktag, dbOid, relOid);

	/*
	 * If blowing away everybody with conflicting locks doesn't work,
	 * after the first two attempts then we just start blowing everybody
	 * away until it does work. We do this because its likely that we
	 * either have too many locks and we just can't get one at all,
	 * or that there are many people crowding for the same table.
	 * Recovery must win; the end justifies the means.
	 */
	while (!lock_acquired)
	{
		if (++num_attempts < 3)
			backends = GetLockConflicts(&locktag, AccessExclusiveLock);
		else
		{
			backends = GetConflictingVirtualXIDs(InvalidTransactionId,
												 InvalidOid);
			report_memory_error = true;
		}

		ResolveRecoveryConflictWithVirtualXIDs(backends,
											   PROCSIG_RECOVERY_CONFLICT_LOCK);

		if (LockAcquireExtended(&locktag, AccessExclusiveLock, true, true, false)
											!= LOCKACQUIRE_NOT_AVAIL)
			lock_acquired = true;
	}
}

/*
 * ResolveRecoveryConflictWithBufferPin is called from LockBufferForCleanup()
 * to resolve conflicts with other backends holding buffer pins.
 *
 * We either resolve conflicts immediately or set a SIGALRM to wake us at
 * the limit of our patience. The sleep in LockBufferForCleanup() is
 * performed here, for code clarity.
 *
 * Resolve conflict by sending a SIGUSR1 reason to all backends to check if
 * they hold one of the buffer pins that is blocking Startup process. If so,
 * backends will take an appropriate error action, ERROR or FATAL.
 *
 * We also check for deadlocks before we wait, though applications that cause
 * these will be extremely rare.  Deadlocks occur because if queries
 * wait on a lock, that must be behind an AccessExclusiveLock, which can only
 * be cleared if the Startup process replays a transaction completion record.
 * If Startup process is also waiting then that is a deadlock. The deadlock
 * can occur if the query is waiting and then the Startup sleeps, or if
 * Startup is sleeping and the the query waits on a lock. We protect against
 * only the former sequence here, the latter sequence is checked prior to
 * the query sleeping, in CheckRecoveryConflictDeadlock().
 */
void
ResolveRecoveryConflictWithBufferPin(void)
{
	bool	sig_alarm_enabled = false;

	Assert(InHotStandby);

	if (MaxStandbyDelay == 0)
	{
		/*
		 * We don't want to wait, so just tell everybody holding the pin to 
		 * get out of town.
		 */
		SendRecoveryConflictWithBufferPin(PROCSIG_RECOVERY_CONFLICT_BUFFERPIN);
	}
	else if (MaxStandbyDelay == -1)
	{
		/*
		 * Send out a request to check for buffer pin deadlocks before we wait.
		 * This is fairly cheap, so no need to wait for deadlock timeout before
		 * trying to send it out.
		 */
		SendRecoveryConflictWithBufferPin(PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK);
	}
	else
	{
		TimestampTz now;
		long	standby_delay_secs;		/* How far Startup process is lagging */
		int		standby_delay_usecs;

		now = GetCurrentTimestamp();

		/* Are we past max_standby_delay? */
		TimestampDifference(GetLatestXLogTime(), now,
							&standby_delay_secs, &standby_delay_usecs);

		if (standby_delay_secs >= MaxStandbyDelay)
		{
			/*
			 * We're already behind, so clear a path as quickly as possible.
			 */
			SendRecoveryConflictWithBufferPin(PROCSIG_RECOVERY_CONFLICT_BUFFERPIN);
		}
		else
		{
			TimestampTz fin_time;			/* Expected wake-up time by timer */
			long	timer_delay_secs;		/* Amount of time we set timer for */
			int		timer_delay_usecs = 0;

			/*
			 * Send out a request to check for buffer pin deadlocks before we wait.
			 * This is fairly cheap, so no need to wait for deadlock timeout before
			 * trying to send it out.
			 */
			SendRecoveryConflictWithBufferPin(PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK);

			/*
			 * How much longer we should wait?
			 */
			timer_delay_secs = MaxStandbyDelay - standby_delay_secs;
			if (standby_delay_usecs > 0)
			{
				timer_delay_secs -= 1;
				timer_delay_usecs = 1000000 - standby_delay_usecs;
			}

			/*
			 * It's possible that the difference is less than a microsecond;
			 * ensure we don't cancel, rather than set, the interrupt.
			 */
			if (timer_delay_secs == 0 && timer_delay_usecs == 0)
				timer_delay_usecs = 1;

			/*
			 * When is the finish time? We recheck this if we are woken early.
			 */
			fin_time = TimestampTzPlusMilliseconds(now,
													(timer_delay_secs * 1000) +
													(timer_delay_usecs / 1000));

			if (enable_standby_sig_alarm(timer_delay_secs, timer_delay_usecs, fin_time))
				sig_alarm_enabled = true;
			else
				elog(FATAL, "could not set timer for process wakeup");
		}
	}

	/* Wait to be signaled by UnpinBuffer() */
	ProcWaitForSignal();

	if (sig_alarm_enabled)
	{
		if (!disable_standby_sig_alarm())
			elog(FATAL, "could not disable timer for process wakeup");
	}
}

void
SendRecoveryConflictWithBufferPin(ProcSignalReason reason)
{
	Assert(reason == PROCSIG_RECOVERY_CONFLICT_BUFFERPIN ||
		   reason == PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK);

	/*
	 * We send signal to all backends to ask them if they are holding
	 * the buffer pin which is delaying the Startup process. We must
	 * not set the conflict flag yet, since most backends will be innocent.
	 * Let the SIGUSR1 handling in each backend decide their own fate.
	 */
	CancelDBBackends(InvalidOid, reason, false);
}

/*
 * In Hot Standby perform early deadlock detection.  We abort the lock
 * wait if are about to sleep while holding the buffer pin that Startup
 * process is waiting for. The deadlock occurs because we can only be
 * waiting behind an AccessExclusiveLock, which can only clear when a
 * transaction completion record is replayed, which can only occur when
 * Startup process is not waiting. So if Startup process is waiting we
 * never will clear that lock, so if we wait we cause deadlock. If we
 * are the Startup process then no need to check for deadlocks.
 */
void
CheckRecoveryConflictDeadlock(LWLockId partitionLock)
{
	Assert(!InRecovery);

	if (!HoldingBufferPinThatDelaysRecovery())
		return;

	LWLockRelease(partitionLock);

	/*
	 * Error message should match ProcessInterrupts() but we avoid calling
	 * that because we aren't handling an interrupt at this point. Note
	 * that we only cancel the current transaction here, so if we are in a
	 * subtransaction and the pin is held by a parent, then the Startup
	 * process will continue to wait even though we have avoided deadlock.
	 */
	ereport(ERROR,
			(errcode(ERRCODE_QUERY_CANCELED),
			 errmsg("canceling statement due to conflict with recovery"),
			 errdetail("User transaction caused buffer deadlock with recovery.")));
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
 * one transaction on one relation, and don't worry about lock queuing.
 *
 * We keep a single dynamically expandible list of locks in local memory,
 * RelationLockList, so we can keep track of the various entried made by
 * the Startup process's virtual xid in the shared lock table.
 *
 * List elements use type xl_rel_lock, since the WAL record type exactly
 * matches the information that we need to keep track of.
 *
 * We use session locks rather than normal locks so we don't need
 * ResourceOwners.
 */


void
StandbyAcquireAccessExclusiveLock(TransactionId xid, Oid dbOid, Oid relOid)
{
	xl_standby_lock	*newlock;
	LOCKTAG			locktag;

	/* Already processed? */
	if (TransactionIdDidCommit(xid) || TransactionIdDidAbort(xid))
		return;

	elog(trace_recovery(DEBUG4),
		 "adding recovery lock: db %u rel %u", dbOid, relOid);

	/* dbOid is InvalidOid when we are locking a shared relation. */
	Assert(OidIsValid(relOid));

	newlock = palloc(sizeof(xl_standby_lock));
	newlock->xid = xid;
	newlock->dbOid = dbOid;
	newlock->relOid = relOid;
	RecoveryLockList = lappend(RecoveryLockList, newlock);

	/*
	 * Attempt to acquire the lock as requested, if not resolve conflict
	 */
	SET_LOCKTAG_RELATION(locktag, newlock->dbOid, newlock->relOid);

	if (LockAcquireExtended(&locktag, AccessExclusiveLock, true, true, false)
											== LOCKACQUIRE_NOT_AVAIL)
		ResolveRecoveryConflictWithLock(newlock->dbOid, newlock->relOid);
}

static void
StandbyReleaseLocks(TransactionId xid)
{
	ListCell   *cell,
			   *prev,
			   *next;

	/*
	 * Release all matching locks and remove them from list
	 */
	prev = NULL;
	for (cell = list_head(RecoveryLockList); cell; cell = next)
	{
		xl_standby_lock *lock = (xl_standby_lock *) lfirst(cell);
		next = lnext(cell);

		if (!TransactionIdIsValid(xid) || lock->xid == xid)
		{
			LOCKTAG		locktag;

			elog(trace_recovery(DEBUG4),
				 "releasing recovery lock: xid %u db %u rel %u",
				 lock->xid, lock->dbOid, lock->relOid);
			SET_LOCKTAG_RELATION(locktag, lock->dbOid, lock->relOid);
			if (!LockRelease(&locktag, AccessExclusiveLock, true))
				elog(trace_recovery(LOG),
					 "RecoveryLockList contains entry for lock no longer recorded by lock manager: xid %u database %u relation %u",
					 lock->xid, lock->dbOid, lock->relOid);

			RecoveryLockList = list_delete_cell(RecoveryLockList, cell, prev);
			pfree(lock);
		}
		else
			prev = cell;
	}
}

/*
 * Release locks for a transaction tree, starting at xid down, from
 * RecoveryLockList.
 *
 * Called during WAL replay of COMMIT/ROLLBACK when in hot standby mode,
 * to remove any AccessExclusiveLocks requested by a transaction.
 */
void
StandbyReleaseLockTree(TransactionId xid, int nsubxids, TransactionId *subxids)
{
	int i;

	StandbyReleaseLocks(xid);

	for (i = 0; i < nsubxids; i++)
		StandbyReleaseLocks(subxids[i]);
}

/*
 * StandbyReleaseOldLocks
 *		Release standby locks held by XIDs < removeXid
 *		In some cases, keep prepared transactions.
 */
static void
StandbyReleaseLocksMany(TransactionId removeXid, bool keepPreparedXacts)
{
	ListCell   *cell,
			   *prev,
			   *next;
	LOCKTAG		locktag;

	/*
	 * Release all matching locks.
	 */
	prev = NULL;
	for (cell = list_head(RecoveryLockList); cell; cell = next)
	{
		xl_standby_lock *lock = (xl_standby_lock *) lfirst(cell);
		next = lnext(cell);

		if (!TransactionIdIsValid(removeXid) || TransactionIdPrecedes(lock->xid, removeXid))
		{
			if (keepPreparedXacts && StandbyTransactionIdIsPrepared(lock->xid))
				continue;
			elog(trace_recovery(DEBUG4),
				 "releasing recovery lock: xid %u db %u rel %u",
				 lock->xid, lock->dbOid, lock->relOid);
			SET_LOCKTAG_RELATION(locktag, lock->dbOid, lock->relOid);
			if (!LockRelease(&locktag, AccessExclusiveLock, true))
				elog(trace_recovery(LOG),
					 "RecoveryLockList contains entry for lock no longer recorded by lock manager: xid %u database %u relation %u",
					 lock->xid, lock->dbOid, lock->relOid);
			RecoveryLockList = list_delete_cell(RecoveryLockList, cell, prev);
			pfree(lock);
		}
		else
			prev = cell;
	}
}

/*
 * Called at end of recovery and when we see a shutdown checkpoint.
 */
void
StandbyReleaseAllLocks(void)
{
	elog(trace_recovery(DEBUG2), "release all standby locks");
	StandbyReleaseLocksMany(InvalidTransactionId, false);
}

/*
 * StandbyReleaseOldLocks
 *		Release standby locks held by XIDs < removeXid, as long
 *		as their not prepared transactions.
 */
void
StandbyReleaseOldLocks(TransactionId removeXid)
{
	StandbyReleaseLocksMany(removeXid, true);
}

/*
 * --------------------------------------------------------------------
 * 		Recovery handling for Rmgr RM_STANDBY_ID
 *
 * These record types will only be created if XLogStandbyInfoActive()
 * --------------------------------------------------------------------
 */

void
standby_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	/* Do nothing if we're not in standby mode */
	if (standbyState == STANDBY_DISABLED)
		return;

	if (info == XLOG_STANDBY_LOCK)
	{
		xl_standby_locks *xlrec = (xl_standby_locks *) XLogRecGetData(record);
		int i;

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
		running.subxid_overflow = xlrec->subxid_overflow;
		running.nextXid = xlrec->nextXid;
		running.oldestRunningXid = xlrec->oldestRunningXid;
		running.xids = xlrec->xids;

		ProcArrayApplyRecoveryInfo(&running);
	}
	else
		elog(PANIC, "relation_redo: unknown op code %u", info);
}

static void
standby_desc_running_xacts(StringInfo buf, xl_running_xacts *xlrec)
{
	int			i;

	appendStringInfo(buf, " nextXid %u oldestRunningXid %u",
					 xlrec->nextXid,
					 xlrec->oldestRunningXid);
	if (xlrec->xcnt > 0)
	{
		appendStringInfo(buf, "; %d xacts:", xlrec->xcnt);
		for (i = 0; i < xlrec->xcnt; i++)
			appendStringInfo(buf, " %u", xlrec->xids[i]);
	}

	if (xlrec->subxid_overflow)
		appendStringInfo(buf, "; subxid ovf");
}

void
standby_desc(StringInfo buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_STANDBY_LOCK)
	{
		xl_standby_locks *xlrec = (xl_standby_locks *) rec;
		int i;

		appendStringInfo(buf, "AccessExclusive locks:");

		for (i = 0; i < xlrec->nlocks; i++)
			appendStringInfo(buf, " xid %u db %u rel %u",
							 xlrec->locks[i].xid, xlrec->locks[i].dbOid,
							 xlrec->locks[i].relOid);
	}
	else if (info == XLOG_RUNNING_XACTS)
	{
		xl_running_xacts *xlrec = (xl_running_xacts *) rec;

		appendStringInfo(buf, " running xacts:");
		standby_desc_running_xacts(buf, xlrec);
	}
	else
		appendStringInfo(buf, "UNKNOWN");
}

/*
 * Log details of the current snapshot to WAL. This allows the snapshot state
 * to be reconstructed on the standby.
 */
void
LogStandbySnapshot(TransactionId *oldestActiveXid, TransactionId *nextXid)
{
	RunningTransactions running;
	xl_standby_lock *locks;
	int nlocks;

	Assert(XLogStandbyInfoActive());

	/*
	 * Get details of any AccessExclusiveLocks being held at the moment.
	 */
	locks = GetRunningTransactionLocks(&nlocks);
	if (nlocks > 0)
		LogAccessExclusiveLocks(nlocks, locks);

	/*
	 * Log details of all in-progress transactions. This should be the last
	 * record we write, because standby will open up when it sees this.
	 */
	running = GetRunningTransactionData();
	LogCurrentRunningXacts(running);

	*oldestActiveXid = running->oldestRunningXid;
	*nextXid = running->nextXid;
}

/*
 * Record an enhanced snapshot of running transactions into WAL.
 *
 * The definitions of RunningTransactionData and xl_xact_running_xacts
 * are similar. We keep them separate because xl_xact_running_xacts
 * is a contiguous chunk of memory and never exists fully until it is
 * assembled in WAL.
 */
static void
LogCurrentRunningXacts(RunningTransactions CurrRunningXacts)
{
	xl_running_xacts	xlrec;
	XLogRecData 			rdata[2];
	int						lastrdata = 0;
	XLogRecPtr	recptr;

	xlrec.xcnt = CurrRunningXacts->xcnt;
	xlrec.subxid_overflow = CurrRunningXacts->subxid_overflow;
	xlrec.nextXid = CurrRunningXacts->nextXid;
	xlrec.oldestRunningXid = CurrRunningXacts->oldestRunningXid;

	/* Header */
	rdata[0].data = (char *) (&xlrec);
	rdata[0].len = MinSizeOfXactRunningXacts;
	rdata[0].buffer = InvalidBuffer;

	/* array of TransactionIds */
	if (xlrec.xcnt > 0)
	{
		rdata[0].next = &(rdata[1]);
		rdata[1].data = (char *) CurrRunningXacts->xids;
		rdata[1].len = xlrec.xcnt * sizeof(TransactionId);
		rdata[1].buffer = InvalidBuffer;
		lastrdata = 1;
	}

	rdata[lastrdata].next = NULL;

	recptr = XLogInsert(RM_STANDBY_ID, XLOG_RUNNING_XACTS, rdata);

	if (CurrRunningXacts->subxid_overflow)
		ereport(trace_recovery(DEBUG2),
				(errmsg("snapshot of %u running transactions overflowed (lsn %X/%X oldest xid %u next xid %u)",
						CurrRunningXacts->xcnt,
						recptr.xlogid, recptr.xrecoff,
						CurrRunningXacts->oldestRunningXid,
						CurrRunningXacts->nextXid)));
	else
		ereport(trace_recovery(DEBUG2),
				(errmsg("snapshot of %u running transaction ids (lsn %X/%X oldest xid %u next xid %u)",
						CurrRunningXacts->xcnt,
						recptr.xlogid, recptr.xrecoff,
						CurrRunningXacts->oldestRunningXid,
						CurrRunningXacts->nextXid)));

}

/*
 * Wholesale logging of AccessExclusiveLocks. Other lock types need not be
 * logged, as described in backend/storage/lmgr/README.
 */
static void
LogAccessExclusiveLocks(int nlocks, xl_standby_lock *locks)
{
	XLogRecData		rdata[2];
	xl_standby_locks	xlrec;

	xlrec.nlocks = nlocks;

	rdata[0].data = (char *) &xlrec;
	rdata[0].len = offsetof(xl_standby_locks, locks);
	rdata[0].buffer = InvalidBuffer;
	rdata[0].next = &rdata[1];

	rdata[1].data = (char *) locks;
	rdata[1].len = nlocks * sizeof(xl_standby_lock);
	rdata[1].buffer = InvalidBuffer;
	rdata[1].next = NULL;

	(void) XLogInsert(RM_STANDBY_ID, XLOG_STANDBY_LOCK, rdata);
}

/*
 * Individual logging of AccessExclusiveLocks for use during LockAcquire()
 */
void
LogAccessExclusiveLock(Oid dbOid, Oid relOid)
{
	xl_standby_lock		xlrec;

	/*
	 * Ensure that a TransactionId has been assigned to this transaction.
	 * We don't actually need the xid yet but if we don't do this then
	 * RecordTransactionCommit() and RecordTransactionAbort() will optimise
	 * away the transaction completion record which recovery relies upon to
	 * release locks. It's a hack, but for a corner case not worth adding
	 * code for into the main commit path.
	 */
	xlrec.xid = GetTopTransactionId();

	/*
	 * Decode the locktag back to the original values, to avoid
	 * sending lots of empty bytes with every message.  See
	 * lock.h to check how a locktag is defined for LOCKTAG_RELATION
	 */
	xlrec.dbOid = dbOid;
	xlrec.relOid = relOid;

	LogAccessExclusiveLocks(1, &xlrec);
}
