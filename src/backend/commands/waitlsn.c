/*-------------------------------------------------------------------------
 *
 * waitlsn.c
 *	  Implements waiting for the given replay LSN, which is used in
 *	  CALL pg_wal_replay_wait(target_lsn pg_lsn, timeout float8).
 *
 * Copyright (c) 2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/commands/waitlsn.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <float.h>
#include <math.h>

#include "pgstat.h"
#include "access/xlog.h"
#include "access/xlogrecovery.h"
#include "commands/waitlsn.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "utils/fmgrprotos.h"
#include "utils/pg_lsn.h"
#include "utils/snapmgr.h"
#include "utils/wait_event_types.h"

static int	waitlsn_cmp(const pairingheap_node *a, const pairingheap_node *b,
						void *arg);

struct WaitLSNState *waitLSNState = NULL;

/* Report the amount of shared memory space needed for WaitLSNState. */
Size
WaitLSNShmemSize(void)
{
	Size		size;

	size = offsetof(WaitLSNState, procInfos);
	size = add_size(size, mul_size(MaxBackends, sizeof(WaitLSNProcInfo)));
	return size;
}

/* Initialize the WaitLSNState in the shared memory. */
void
WaitLSNShmemInit(void)
{
	bool		found;

	waitLSNState = (WaitLSNState *) ShmemInitStruct("WaitLSNState",
													WaitLSNShmemSize(),
													&found);
	if (!found)
	{
		pg_atomic_init_u64(&waitLSNState->minWaitedLSN, PG_UINT64_MAX);
		pairingheap_initialize(&waitLSNState->waitersHeap, waitlsn_cmp, NULL);
		memset(&waitLSNState->procInfos, 0, MaxBackends * sizeof(WaitLSNProcInfo));
	}
}

/*
 * Comparison function for waitLSN->waitersHeap heap.  Waiting processes are
 * ordered by lsn, so that the waiter with smallest lsn is at the top.
 */
static int
waitlsn_cmp(const pairingheap_node *a, const pairingheap_node *b, void *arg)
{
	const WaitLSNProcInfo *aproc = pairingheap_const_container(WaitLSNProcInfo, phNode, a);
	const WaitLSNProcInfo *bproc = pairingheap_const_container(WaitLSNProcInfo, phNode, b);

	if (aproc->waitLSN < bproc->waitLSN)
		return 1;
	else if (aproc->waitLSN > bproc->waitLSN)
		return -1;
	else
		return 0;
}

/*
 * Update waitLSN->minWaitedLSN according to the current state of
 * waitLSN->waitersHeap.
 */
static void
updateMinWaitedLSN(void)
{
	XLogRecPtr	minWaitedLSN = PG_UINT64_MAX;

	if (!pairingheap_is_empty(&waitLSNState->waitersHeap))
	{
		pairingheap_node *node = pairingheap_first(&waitLSNState->waitersHeap);

		minWaitedLSN = pairingheap_container(WaitLSNProcInfo, phNode, node)->waitLSN;
	}

	pg_atomic_write_u64(&waitLSNState->minWaitedLSN, minWaitedLSN);
}

/*
 * Put the current process into the heap of LSN waiters.
 */
static void
addLSNWaiter(XLogRecPtr lsn)
{
	WaitLSNProcInfo *procInfo = &waitLSNState->procInfos[MyProcNumber];

	LWLockAcquire(WaitLSNLock, LW_EXCLUSIVE);

	Assert(!procInfo->inHeap);

	procInfo->latch = MyLatch;
	procInfo->waitLSN = lsn;

	pairingheap_add(&waitLSNState->waitersHeap, &procInfo->phNode);
	procInfo->inHeap = true;
	updateMinWaitedLSN();

	LWLockRelease(WaitLSNLock);
}

/*
 * Remove the current process from the heap of LSN waiters if it's there.
 */
static void
deleteLSNWaiter(void)
{
	WaitLSNProcInfo *procInfo = &waitLSNState->procInfos[MyProcNumber];

	LWLockAcquire(WaitLSNLock, LW_EXCLUSIVE);

	if (!procInfo->inHeap)
	{
		LWLockRelease(WaitLSNLock);
		return;
	}

	pairingheap_remove(&waitLSNState->waitersHeap, &procInfo->phNode);
	procInfo->inHeap = false;
	updateMinWaitedLSN();

	LWLockRelease(WaitLSNLock);
}

/*
 * Remove waiters whose LSN has been replayed from the heap and set their
 * latches.  If InvalidXLogRecPtr is given, remove all waiters from the heap
 * and set latches for all waiters.
 */
void
WaitLSNSetLatches(XLogRecPtr currentLSN)
{
	int			i;
	Latch	  **wakeUpProcLatches;
	int			numWakeUpProcs = 0;

	wakeUpProcLatches = palloc(sizeof(Latch *) * MaxBackends);

	LWLockAcquire(WaitLSNLock, LW_EXCLUSIVE);

	/*
	 * Iterate the pairing heap of waiting processes till we find LSN not yet
	 * replayed.  Record the process latches to set them later.
	 */
	while (!pairingheap_is_empty(&waitLSNState->waitersHeap))
	{
		pairingheap_node *node = pairingheap_first(&waitLSNState->waitersHeap);
		WaitLSNProcInfo *procInfo = pairingheap_container(WaitLSNProcInfo, phNode, node);

		if (!XLogRecPtrIsInvalid(currentLSN) &&
			procInfo->waitLSN > currentLSN)
			break;

		wakeUpProcLatches[numWakeUpProcs++] = procInfo->latch;
		(void) pairingheap_remove_first(&waitLSNState->waitersHeap);
		procInfo->inHeap = false;
	}

	updateMinWaitedLSN();

	LWLockRelease(WaitLSNLock);

	/*
	 * Set latches for processes, whose waited LSNs are already replayed. As
	 * the time consuming operations, we do it this outside of WaitLSNLock.
	 * This is  actually fine because procLatch isn't ever freed, so we just
	 * can potentially set the wrong process' (or no process') latch.
	 */
	for (i = 0; i < numWakeUpProcs; i++)
	{
		SetLatch(wakeUpProcLatches[i]);
	}
	pfree(wakeUpProcLatches);
}

/*
 * Delete our item from shmem array if any.
 */
void
WaitLSNCleanup(void)
{
	/*
	 * We do a fast-path check of the 'inHeap' flag without the lock.  This
	 * flag is set to true only by the process itself.  So, it's only possible
	 * to get a false positive.  But that will be eliminated by a recheck
	 * inside deleteLSNWaiter().
	 */
	if (waitLSNState->procInfos[MyProcNumber].inHeap)
		deleteLSNWaiter();
}

/*
 * Wait using MyLatch till the given LSN is replayed, the postmaster dies or
 * timeout happens.
 */
static void
WaitForLSNReplay(XLogRecPtr targetLSN, int64 timeout)
{
	XLogRecPtr	currentLSN;
	TimestampTz endtime = 0;
	int			wake_events = WL_LATCH_SET | WL_EXIT_ON_PM_DEATH;

	/* Shouldn't be called when shmem isn't initialized */
	Assert(waitLSNState);

	/* Should have a valid proc number */
	Assert(MyProcNumber >= 0 && MyProcNumber < MaxBackends);

	if (!RecoveryInProgress())
	{
		/*
		 * Recovery is not in progress.  Given that we detected this in the
		 * very first check, this procedure was mistakenly called on primary.
		 * However, it's possible that standby was promoted concurrently to
		 * the procedure call, while target LSN is replayed.  So, we still
		 * check the last replay LSN before reporting an error.
		 */
		if (targetLSN <= GetXLogReplayRecPtr(NULL))
			return;
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is not in progress"),
				 errhint("Waiting for LSN can only be executed during recovery.")));
	}
	else
	{
		/* If target LSN is already replayed, exit immediately */
		if (targetLSN <= GetXLogReplayRecPtr(NULL))
			return;
	}

	if (timeout > 0)
	{
		endtime = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), timeout);
		wake_events |= WL_TIMEOUT;
	}

	/*
	 * Add our process to the pairing heap of waiters.  It might happen that
	 * target LSN gets replayed before we do.  Another check at the beginning
	 * of the loop below prevents the race condition.
	 */
	addLSNWaiter(targetLSN);

	for (;;)
	{
		int			rc;
		long		delay_ms = 0;

		/* Recheck that recovery is still in-progress */
		if (!RecoveryInProgress())
		{
			/*
			 * Recovery was ended, but recheck if target LSN was already
			 * replayed.
			 */
			currentLSN = GetXLogReplayRecPtr(NULL);
			if (targetLSN <= currentLSN)
				return;
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("recovery is not in progress"),
					 errdetail("Recovery ended before replaying target LSN %X/%X; last replay LSN %X/%X.",
							   LSN_FORMAT_ARGS(targetLSN),
							   LSN_FORMAT_ARGS(currentLSN))));
		}
		else
		{
			/* Check if the waited LSN has been replayed */
			currentLSN = GetXLogReplayRecPtr(NULL);
			if (targetLSN <= currentLSN)
				break;
		}

		/*
		 * If the timeout value is specified, calculate the number of
		 * milliseconds before the timeout.  Exit if the timeout is already
		 * achieved.
		 */
		if (timeout > 0)
		{
			delay_ms = TimestampDifferenceMilliseconds(GetCurrentTimestamp(), endtime);
			if (delay_ms <= 0)
				break;
		}

		CHECK_FOR_INTERRUPTS();

		rc = WaitLatch(MyLatch, wake_events, delay_ms,
					   WAIT_EVENT_WAIT_FOR_WAL_REPLAY);

		if (rc & WL_LATCH_SET)
			ResetLatch(MyLatch);
	}

	/*
	 * Delete our process from the shared memory pairing heap.  We might
	 * already be deleted by the startup process.  The 'inHeap' flag prevents
	 * us from the double deletion.
	 */
	deleteLSNWaiter();

	/*
	 * If we didn't achieve the target LSN, we must be exited by timeout.
	 */
	if (targetLSN > currentLSN)
	{
		ereport(ERROR,
				(errcode(ERRCODE_QUERY_CANCELED),
				 errmsg("timed out while waiting for target LSN %X/%X to be replayed; current replay LSN %X/%X",
						LSN_FORMAT_ARGS(targetLSN),
						LSN_FORMAT_ARGS(currentLSN))));
	}
}

Datum
pg_wal_replay_wait(PG_FUNCTION_ARGS)
{
	XLogRecPtr	target_lsn = PG_GETARG_LSN(0);
	int64		timeout = PG_GETARG_INT64(1);

	if (timeout < 0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("\"timeout\" must not be negative")));

	/*
	 * We are going to wait for the LSN replay.  We should first care that we
	 * don't hold a snapshot and correspondingly our MyProc->xmin is invalid.
	 * Otherwise, our snapshot could prevent the replay of WAL records
	 * implying a kind of self-deadlock.  This is the reason why
	 * pg_wal_replay_wait() is a procedure, not a function.
	 *
	 * At first, we should check there is no active snapshot.  According to
	 * PlannedStmtRequiresSnapshot(), even in an atomic context, CallStmt is
	 * processed with a snapshot.  Thankfully, we can pop this snapshot,
	 * because PortalRunUtility() can tolerate this.
	 */
	if (ActiveSnapshotSet())
		PopActiveSnapshot();

	/*
	 * At second, invalidate a catalog snapshot if any.  And we should be done
	 * with the preparation.
	 */
	InvalidateCatalogSnapshot();

	/* Give up if there is still an active or registered sanpshot. */
	if (GetOldestSnapshot())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_wal_replay_wait() must be only called without an active or registered snapshot"),
				 errdetail("Make sure pg_wal_replay_wait() isn't called within a transaction with an isolation level higher than READ COMMITTED, another procedure, or a function.")));

	/*
	 * As the result we should hold no snapshot, and correspondingly our xmin
	 * should be unset.
	 */
	Assert(MyProc->xmin == InvalidTransactionId);

	(void) WaitForLSNReplay(target_lsn, timeout);

	PG_RETURN_VOID();
}
