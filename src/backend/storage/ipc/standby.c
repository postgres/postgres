/*-------------------------------------------------------------------------
 *
 * standby.c
 *	  Misc functions used in Hot Standby mode.
 *
 *	InitRecoveryTransactionEnvironment()
 *  ShutdownRecoveryTransactionEnvironment()
 *
 *  ResolveRecoveryConflictWithVirtualXIDs()
 *
 *  All functions for handling RM_STANDBY_ID, which relate to
 *  AccessExclusiveLocks and starting snapshots for Hot Standby mode.
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/ipc/standby.c,v 1.1 2009/12/19 01:32:35 sriggs Exp $
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
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/sinvaladt.h"
#include "storage/standby.h"
#include "utils/ps_status.h"

int		vacuum_defer_cleanup_age;

static List *RecoveryLockList;

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

	/* max_standby_delay = -1 means wait forever, if necessary */
	if (MaxStandbyDelay < 0)
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
 *
 * We may ask for a specific cancel_mode, typically ERROR or FATAL.
 */
void
ResolveRecoveryConflictWithVirtualXIDs(VirtualTransactionId *waitlist,
									   char *reason, int cancel_mode)
{
	char		waitactivitymsg[100];

	Assert(cancel_mode > 0);

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
				const char *oldactivitymsg;
				int			len;

				oldactivitymsg = get_ps_display(&len);
				snprintf(waitactivitymsg, sizeof(waitactivitymsg),
						 "waiting for max_standby_delay (%u ms)",
						 MaxStandbyDelay);
				set_ps_display(waitactivitymsg, false);
				if (len > 100)
					len = 100;
				memcpy(waitactivitymsg, oldactivitymsg, len);

				ereport(trace_recovery(DEBUG5),
						(errmsg("virtual transaction %u/%u is blocking %s",
								waitlist->backendId,
								waitlist->localTransactionId,
								reason)));

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
				pid = CancelVirtualTransaction(*waitlist, cancel_mode);

				if (pid != 0)
				{
					/*
					 * Startup process debug messages
					 */
					switch (cancel_mode)
					{
						case CONFLICT_MODE_FATAL:
							elog(trace_recovery(DEBUG1),
									"recovery disconnects session with pid %d because of conflict with %s",
											pid,
											reason);
							break;
						case CONFLICT_MODE_ERROR:
							elog(trace_recovery(DEBUG1),
									"recovery cancels virtual transaction %u/%u pid %d because of conflict with %s",
											waitlist->backendId,
											waitlist->localTransactionId,
											pid,
											reason);
							break;
						default:
							/* No conflict pending, so fall through */
							break;
					}

					/*
					 * Wait awhile for it to die so that we avoid flooding an
					 * unresponsive backend when system is heavily loaded.
					 */
					pg_usleep(5000);
				}
			}
		}

		/* Reset ps display */
		if (logged)
		{
			set_ps_display(waitactivitymsg, false);
			pgstat_report_waiting(false);
		}

		/* The virtual transaction is gone now, wait for the next one */
		waitlist++;
    }
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
	bool			report_memory_error = false;
	int				num_attempts = 0;

	/* Already processed? */
	if (TransactionIdDidCommit(xid) || TransactionIdDidAbort(xid))
		return;

	elog(trace_recovery(DEBUG4),
		 "adding recovery lock: db %d rel %d", dbOid, relOid);

	/* dbOid is InvalidOid when we are locking a shared relation. */
	Assert(OidIsValid(relOid));

	newlock = palloc(sizeof(xl_standby_lock));
	newlock->xid = xid;
	newlock->dbOid = dbOid;
	newlock->relOid = relOid;
	RecoveryLockList = lappend(RecoveryLockList, newlock);

	/*
	 * Attempt to acquire the lock as requested.
	 */
	SET_LOCKTAG_RELATION(locktag, newlock->dbOid, newlock->relOid);

	/*
	 * Wait for lock to clear or kill anyone in our way.
	 */
	while (LockAcquireExtended(&locktag, AccessExclusiveLock,
								true, true, report_memory_error)
											== LOCKACQUIRE_NOT_AVAIL)
	{
		VirtualTransactionId *backends;

		/*
		 * If blowing away everybody with conflicting locks doesn't work,
		 * after the first two attempts then we just start blowing everybody
		 * away until it does work. We do this because its likely that we
		 * either have too many locks and we just can't get one at all,
		 * or that there are many people crowding for the same table.
		 * Recovery must win; the end justifies the means.
		 */
		if (++num_attempts < 3)
			backends = GetLockConflicts(&locktag, AccessExclusiveLock);
		else
		{
			backends = GetConflictingVirtualXIDs(InvalidTransactionId,
												 InvalidOid,
												 true);
			report_memory_error = true;
		}

		ResolveRecoveryConflictWithVirtualXIDs(backends,
											   "exclusive lock",
											   CONFLICT_MODE_ERROR);
	}
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
					"releasing recovery lock: xid %u db %d rel %d",
							lock->xid, lock->dbOid, lock->relOid);
			SET_LOCKTAG_RELATION(locktag, lock->dbOid, lock->relOid);
			if (!LockRelease(&locktag, AccessExclusiveLock, true))
				elog(trace_recovery(LOG),
					"RecoveryLockList contains entry for lock "
					"no longer recorded by lock manager "
					"xid %u database %d relation %d",
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
				 "releasing recovery lock: xid %u db %d rel %d",
				 lock->xid, lock->dbOid, lock->relOid);
			SET_LOCKTAG_RELATION(locktag, lock->dbOid, lock->relOid);
			if (!LockRelease(&locktag, AccessExclusiveLock, true))
				elog(trace_recovery(LOG),
					 "RecoveryLockList contains entry for lock "
					 "no longer recorded by lock manager "
					 "xid %u database %d relation %d",
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

	appendStringInfo(buf,
					 " nextXid %u oldestRunningXid %u",
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
			appendStringInfo(buf, " xid %u db %d rel %d",
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
