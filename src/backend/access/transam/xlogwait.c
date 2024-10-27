/*-------------------------------------------------------------------------
 *
 * xlogwait.c
 *	  Implements waiting for the given replay LSN, which is used in
 *	  CALL pg_wal_replay_wait(target_lsn pg_lsn,
 *							  timeout float8, no_error bool).
 *
 * Copyright (c) 2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/access/transam/xlogwait.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <float.h>
#include <math.h>

#include "pgstat.h"
#include "access/xlog.h"
#include "access/xlogrecovery.h"
#include "access/xlogwait.h"
#include "miscadmin.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "utils/fmgrprotos.h"
#include "utils/pg_lsn.h"
#include "utils/snapmgr.h"

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
WaitLSNResult
WaitForLSNReplay(XLogRecPtr targetLSN, int64 timeout)
{
	XLogRecPtr	currentLSN;
	TimestampTz endtime = 0;
	int			wake_events = WL_LATCH_SET | WL_POSTMASTER_DEATH;

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
			return WAIT_LSN_RESULT_SUCCESS;
		return WAIT_LSN_RESULT_NOT_IN_RECOVERY;
	}
	else
	{
		/* If target LSN is already replayed, exit immediately */
		if (targetLSN <= GetXLogReplayRecPtr(NULL))
			return WAIT_LSN_RESULT_SUCCESS;
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
			 * replayed.  See the comment regarding deleteLSNWaiter() below.
			 */
			deleteLSNWaiter();
			currentLSN = GetXLogReplayRecPtr(NULL);
			if (targetLSN <= currentLSN)
				return WAIT_LSN_RESULT_SUCCESS;
			return WAIT_LSN_RESULT_NOT_IN_RECOVERY;
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
		 * reached.
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

		/*
		 * Emergency bailout if postmaster has died.  This is to avoid the
		 * necessity for manual cleanup of all postmaster children.
		 */
		if (rc & WL_POSTMASTER_DEATH)
			ereport(FATAL,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("terminating connection due to unexpected postmaster exit"),
					 errcontext("while waiting for LSN replay")));

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
	 * If we didn't reach the target LSN, we must be exited by timeout.
	 */
	if (targetLSN > currentLSN)
		return WAIT_LSN_RESULT_TIMEOUT;

	return WAIT_LSN_RESULT_SUCCESS;
}
