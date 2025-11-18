/*-------------------------------------------------------------------------
 *
 * xlogwait.c
 *	  Implements waiting for WAL operations to reach specific LSNs.
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/access/transam/xlogwait.c
 *
 * NOTES
 *		This file implements waiting for WAL operations to reach specific LSNs
 *		on both physical standby and primary servers. The core idea is simple:
 *		every process that wants to wait publishes the LSN it needs to the
 *		shared memory, and the appropriate process (startup on standby, or
 *		WAL writer/backend on primary) wakes it once that LSN has been reached.
 *
 *		The shared memory used by this module comprises a procInfos
 *		per-backend array with the information of the awaited LSN for each
 *		of the backend processes.  The elements of that array are organized
 *		into a pairing heap waitersHeap, which allows for very fast finding
 *		of the least awaited LSN.
 *
 *		In addition, the least-awaited LSN is cached as minWaitedLSN.  The
 *		waiter process publishes information about itself to the shared
 *		memory and waits on the latch until it is woken up by the appropriate
 *		process, standby is promoted, or the postmaster	dies.  Then, it cleans
 *		information about itself in the shared memory.
 *
 *		On standby servers: After replaying a WAL record, the startup process
 *		first performs a fast path check minWaitedLSN > replayLSN.  If this
 *		check is negative, it checks waitersHeap and wakes up the backend
 *		whose awaited LSNs are reached.
 *
 *		On primary servers: After flushing WAL, the WAL writer or backend
 *		process performs a similar check against the flush LSN and wakes up
 *		waiters whose target flush LSNs have been reached.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <float.h>
#include <math.h>

#include "access/xlog.h"
#include "access/xlogrecovery.h"
#include "access/xlogwait.h"
#include "miscadmin.h"
#include "pgstat.h"
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
	size = add_size(size, mul_size(MaxBackends + NUM_AUXILIARY_PROCS, sizeof(WaitLSNProcInfo)));
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
		int			i;

		/* Initialize heaps and tracking */
		for (i = 0; i < WAIT_LSN_TYPE_COUNT; i++)
		{
			pg_atomic_init_u64(&waitLSNState->minWaitedLSN[i], PG_UINT64_MAX);
			pairingheap_initialize(&waitLSNState->waitersHeap[i], waitlsn_cmp, NULL);
		}

		/* Initialize process info array */
		memset(&waitLSNState->procInfos, 0,
			   (MaxBackends + NUM_AUXILIARY_PROCS) * sizeof(WaitLSNProcInfo));
	}
}

/*
 * Comparison function for LSN waiters heaps. Waiting processes are ordered by
 * LSN, so that the waiter with smallest LSN is at the top.
 */
static int
waitlsn_cmp(const pairingheap_node *a, const pairingheap_node *b, void *arg)
{
	const WaitLSNProcInfo *aproc = pairingheap_const_container(WaitLSNProcInfo, heapNode, a);
	const WaitLSNProcInfo *bproc = pairingheap_const_container(WaitLSNProcInfo, heapNode, b);

	if (aproc->waitLSN < bproc->waitLSN)
		return 1;
	else if (aproc->waitLSN > bproc->waitLSN)
		return -1;
	else
		return 0;
}

/*
 * Update minimum waited LSN for the specified LSN type
 */
static void
updateMinWaitedLSN(WaitLSNType lsnType)
{
	XLogRecPtr	minWaitedLSN = PG_UINT64_MAX;
	int			i = (int) lsnType;

	Assert(i >= 0 && i < (int) WAIT_LSN_TYPE_COUNT);

	if (!pairingheap_is_empty(&waitLSNState->waitersHeap[i]))
	{
		pairingheap_node *node = pairingheap_first(&waitLSNState->waitersHeap[i]);
		WaitLSNProcInfo *procInfo = pairingheap_container(WaitLSNProcInfo, heapNode, node);

		minWaitedLSN = procInfo->waitLSN;
	}
	pg_atomic_write_u64(&waitLSNState->minWaitedLSN[i], minWaitedLSN);
}

/*
 * Add current process to appropriate waiters heap based on LSN type
 */
static void
addLSNWaiter(XLogRecPtr lsn, WaitLSNType lsnType)
{
	WaitLSNProcInfo *procInfo = &waitLSNState->procInfos[MyProcNumber];
	int			i = (int) lsnType;

	Assert(i >= 0 && i < (int) WAIT_LSN_TYPE_COUNT);

	LWLockAcquire(WaitLSNLock, LW_EXCLUSIVE);

	procInfo->procno = MyProcNumber;
	procInfo->waitLSN = lsn;
	procInfo->lsnType = lsnType;

	Assert(!procInfo->inHeap);
	pairingheap_add(&waitLSNState->waitersHeap[i], &procInfo->heapNode);
	procInfo->inHeap = true;
	updateMinWaitedLSN(lsnType);

	LWLockRelease(WaitLSNLock);
}

/*
 * Remove current process from appropriate waiters heap based on LSN type
 */
static void
deleteLSNWaiter(WaitLSNType lsnType)
{
	WaitLSNProcInfo *procInfo = &waitLSNState->procInfos[MyProcNumber];
	int			i = (int) lsnType;

	Assert(i >= 0 && i < (int) WAIT_LSN_TYPE_COUNT);

	LWLockAcquire(WaitLSNLock, LW_EXCLUSIVE);

	Assert(procInfo->lsnType == lsnType);

	if (procInfo->inHeap)
	{
		pairingheap_remove(&waitLSNState->waitersHeap[i], &procInfo->heapNode);
		procInfo->inHeap = false;
		updateMinWaitedLSN(lsnType);
	}

	LWLockRelease(WaitLSNLock);
}

/*
 * Size of a static array of procs to wakeup by WaitLSNWakeup() allocated
 * on the stack.  It should be enough to take single iteration for most cases.
 */
#define	WAKEUP_PROC_STATIC_ARRAY_SIZE (16)

/*
 * Remove waiters whose LSN has been reached from the heap and set their
 * latches.  If InvalidXLogRecPtr is given, remove all waiters from the heap
 * and set latches for all waiters.
 *
 * This function first accumulates waiters to wake up into an array, then
 * wakes them up without holding a WaitLSNLock.  The array size is static and
 * equal to WAKEUP_PROC_STATIC_ARRAY_SIZE.  That should be more than enough
 * to wake up all the waiters at once in the vast majority of cases.  However,
 * if there are more waiters, this function will loop to process them in
 * multiple chunks.
 */
static void
wakeupWaiters(WaitLSNType lsnType, XLogRecPtr currentLSN)
{
	ProcNumber	wakeUpProcs[WAKEUP_PROC_STATIC_ARRAY_SIZE];
	int			numWakeUpProcs;
	int			i = (int) lsnType;

	Assert(i >= 0 && i < (int) WAIT_LSN_TYPE_COUNT);

	do
	{
		numWakeUpProcs = 0;
		LWLockAcquire(WaitLSNLock, LW_EXCLUSIVE);

		/*
		 * Iterate the waiters heap until we find LSN not yet reached. Record
		 * process numbers to wake up, but send wakeups after releasing lock.
		 */
		while (!pairingheap_is_empty(&waitLSNState->waitersHeap[i]))
		{
			pairingheap_node *node = pairingheap_first(&waitLSNState->waitersHeap[i]);
			WaitLSNProcInfo *procInfo;

			/* Get procInfo using appropriate heap node */
			procInfo = pairingheap_container(WaitLSNProcInfo, heapNode, node);

			if (XLogRecPtrIsValid(currentLSN) && procInfo->waitLSN > currentLSN)
				break;

			Assert(numWakeUpProcs < WAKEUP_PROC_STATIC_ARRAY_SIZE);
			wakeUpProcs[numWakeUpProcs++] = procInfo->procno;
			(void) pairingheap_remove_first(&waitLSNState->waitersHeap[i]);

			/* Update appropriate flag */
			procInfo->inHeap = false;

			if (numWakeUpProcs == WAKEUP_PROC_STATIC_ARRAY_SIZE)
				break;
		}

		updateMinWaitedLSN(lsnType);
		LWLockRelease(WaitLSNLock);

		/*
		 * Set latches for processes whose waited LSNs have been reached.
		 * Since SetLatch() is a time-consuming operation, we do this outside
		 * of WaitLSNLock. This is safe because procLatch is never freed, so
		 * at worst we may set a latch for the wrong process or for no process
		 * at all, which is harmless.
		 */
		for (i = 0; i < numWakeUpProcs; i++)
			SetLatch(&GetPGProcByNumber(wakeUpProcs[i])->procLatch);

	} while (numWakeUpProcs == WAKEUP_PROC_STATIC_ARRAY_SIZE);
}

/*
 * Wake up processes waiting for LSN to reach currentLSN
 */
void
WaitLSNWakeup(WaitLSNType lsnType, XLogRecPtr currentLSN)
{
	int			i = (int) lsnType;

	Assert(i >= 0 && i < (int) WAIT_LSN_TYPE_COUNT);

	/*
	 * Fast path check.  Skip if currentLSN is InvalidXLogRecPtr, which means
	 * "wake all waiters" (e.g., during promotion when recovery ends).
	 */
	if (XLogRecPtrIsValid(currentLSN) &&
		pg_atomic_read_u64(&waitLSNState->minWaitedLSN[i]) > currentLSN)
		return;

	wakeupWaiters(lsnType, currentLSN);
}

/*
 * Clean up LSN waiters for exiting process
 */
void
WaitLSNCleanup(void)
{
	if (waitLSNState)
	{
		/*
		 * We do a fast-path check of the inHeap flag without the lock.  This
		 * flag is set to true only by the process itself.  So, it's only
		 * possible to get a false positive.  But that will be eliminated by a
		 * recheck inside deleteLSNWaiter().
		 */
		if (waitLSNState->procInfos[MyProcNumber].inHeap)
			deleteLSNWaiter(waitLSNState->procInfos[MyProcNumber].lsnType);
	}
}

/*
 * Wait using MyLatch till the given LSN is reached, the replica gets
 * promoted, or the postmaster dies.
 *
 * Returns WAIT_LSN_RESULT_SUCCESS if target LSN was reached.
 * Returns WAIT_LSN_RESULT_NOT_IN_RECOVERY if run not in recovery,
 * or replica got promoted before the target LSN reached.
 */
WaitLSNResult
WaitForLSN(WaitLSNType lsnType, XLogRecPtr targetLSN, int64 timeout)
{
	XLogRecPtr	currentLSN;
	TimestampTz endtime = 0;
	int			wake_events = WL_LATCH_SET | WL_POSTMASTER_DEATH;

	/* Shouldn't be called when shmem isn't initialized */
	Assert(waitLSNState);

	/* Should have a valid proc number */
	Assert(MyProcNumber >= 0 && MyProcNumber < MaxBackends);

	if (timeout > 0)
	{
		endtime = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), timeout);
		wake_events |= WL_TIMEOUT;
	}

	/*
	 * Add our process to the waiters heap.  It might happen that target LSN
	 * gets reached before we do.  The check at the beginning of the loop
	 * below prevents the race condition.
	 */
	addLSNWaiter(targetLSN, lsnType);

	for (;;)
	{
		int			rc;
		long		delay_ms = -1;

		if (lsnType == WAIT_LSN_TYPE_REPLAY)
			currentLSN = GetXLogReplayRecPtr(NULL);
		else
			currentLSN = GetFlushRecPtr(NULL);

		/* Check that recovery is still in-progress */
		if (lsnType == WAIT_LSN_TYPE_REPLAY && !RecoveryInProgress())
		{
			/*
			 * Recovery was ended, but check if target LSN was already
			 * reached.
			 */
			deleteLSNWaiter(lsnType);

			if (PromoteIsTriggered() && targetLSN <= currentLSN)
				return WAIT_LSN_RESULT_SUCCESS;
			return WAIT_LSN_RESULT_NOT_IN_RECOVERY;
		}
		else
		{
			/* Check if the waited LSN has been reached */
			if (targetLSN <= currentLSN)
				break;
		}

		if (timeout > 0)
		{
			delay_ms = TimestampDifferenceMilliseconds(GetCurrentTimestamp(), endtime);
			if (delay_ms <= 0)
				break;
		}

		CHECK_FOR_INTERRUPTS();

		rc = WaitLatch(MyLatch, wake_events, delay_ms,
					   (lsnType == WAIT_LSN_TYPE_REPLAY) ? WAIT_EVENT_WAIT_FOR_WAL_REPLAY : WAIT_EVENT_WAIT_FOR_WAL_FLUSH);

		/*
		 * Emergency bailout if postmaster has died.  This is to avoid the
		 * necessity for manual cleanup of all postmaster children.
		 */
		if (rc & WL_POSTMASTER_DEATH)
			ereport(FATAL,
					errcode(ERRCODE_ADMIN_SHUTDOWN),
					errmsg("terminating connection due to unexpected postmaster exit"),
					errcontext("while waiting for LSN"));

		if (rc & WL_LATCH_SET)
			ResetLatch(MyLatch);
	}

	/*
	 * Delete our process from the shared memory heap.  We might already be
	 * deleted by the startup process.  The 'inHeap' flags prevents us from
	 * the double deletion.
	 */
	deleteLSNWaiter(lsnType);

	/*
	 * If we didn't reach the target LSN, we must be exited by timeout.
	 */
	if (targetLSN > currentLSN)
		return WAIT_LSN_RESULT_TIMEOUT;

	return WAIT_LSN_RESULT_SUCCESS;
}
