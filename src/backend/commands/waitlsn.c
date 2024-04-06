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

static int	lsn_cmp(const pairingheap_node *a, const pairingheap_node *b,
					void *arg);

struct WaitLSNState *waitLSN = NULL;

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

	waitLSN = (WaitLSNState *) ShmemInitStruct("WaitLSNState",
											   WaitLSNShmemSize(),
											   &found);
	if (!found)
	{
		pg_atomic_init_u64(&waitLSN->minWaitedLSN, PG_UINT64_MAX);
		pairingheap_initialize(&waitLSN->waitersHeap, lsn_cmp, NULL);
		memset(&waitLSN->procInfos, 0, MaxBackends * sizeof(WaitLSNProcInfo));
	}
}

/*
 * Comparison function for waitLSN->waitersHeap heap.  Waiting processes are
 * ordered by lsn, so that the waiter with smallest lsn is at the top.
 */
static int
lsn_cmp(const pairingheap_node *a, const pairingheap_node *b, void *arg)
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

	if (!pairingheap_is_empty(&waitLSN->waitersHeap))
	{
		pairingheap_node *node = pairingheap_first(&waitLSN->waitersHeap);

		minWaitedLSN = pairingheap_container(WaitLSNProcInfo, phNode, node)->waitLSN;
	}

	pg_atomic_write_u64(&waitLSN->minWaitedLSN, minWaitedLSN);
}

/*
 * Put the current process into the heap of LSN waiters.
 */
static void
addLSNWaiter(XLogRecPtr lsn)
{
	WaitLSNProcInfo *procInfo = &waitLSN->procInfos[MyProcNumber];

	Assert(!procInfo->inHeap);

	procInfo->procnum = MyProcNumber;
	procInfo->waitLSN = lsn;

	LWLockAcquire(WaitLSNLock, LW_EXCLUSIVE);

	pairingheap_add(&waitLSN->waitersHeap, &procInfo->phNode);
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
	WaitLSNProcInfo *procInfo = &waitLSN->procInfos[MyProcNumber];

	LWLockAcquire(WaitLSNLock, LW_EXCLUSIVE);

	if (!procInfo->inHeap)
	{
		LWLockRelease(WaitLSNLock);
		return;
	}

	pairingheap_remove(&waitLSN->waitersHeap, &procInfo->phNode);
	procInfo->inHeap = false;
	updateMinWaitedLSN();

	LWLockRelease(WaitLSNLock);
}

/*
 * Set latches of LSN waiters whose LSN has been replayed.  Set latches of all
 * LSN waiters when InvalidXLogRecPtr is given.
 */
void
WaitLSNSetLatches(XLogRecPtr currentLSN)
{
	int			i;
	int		   *wakeUpProcNums;
	int			numWakeUpProcs = 0;

	wakeUpProcNums = palloc(sizeof(int) * MaxBackends);

	LWLockAcquire(WaitLSNLock, LW_EXCLUSIVE);

	/*
	 * Iterate the pairing heap of waiting processes till we find LSN not yet
	 * replayed.  Record the process numbers to set their latches later.
	 */
	while (!pairingheap_is_empty(&waitLSN->waitersHeap))
	{
		pairingheap_node *node = pairingheap_first(&waitLSN->waitersHeap);
		WaitLSNProcInfo *procInfo = pairingheap_container(WaitLSNProcInfo, phNode, node);

		if (!XLogRecPtrIsInvalid(currentLSN) &&
			procInfo->waitLSN > currentLSN)
			break;

		wakeUpProcNums[numWakeUpProcs++] = procInfo->procnum;
		(void) pairingheap_remove_first(&waitLSN->waitersHeap);
		procInfo->inHeap = false;
	}

	updateMinWaitedLSN();

	LWLockRelease(WaitLSNLock);

	/*
	 * Set latches for processes, whose waited LSNs are already replayed. This
	 * involves spinlocks.  So, we shouldn't do this under a spinlock.
	 */
	for (i = 0; i < numWakeUpProcs; i++)
	{
		PGPROC	   *backend;

		backend = GetPGProcByNumber(wakeUpProcNums[i]);
		SetLatch(&backend->procLatch);
	}
	pfree(wakeUpProcNums);
}

/*
 * Delete our item from shmem array if any.
 */
void
WaitLSNCleanup(void)
{
	if (waitLSN->procInfos[MyProcNumber].inHeap)
		deleteLSNWaiter();
}

/*
 * Wait using MyLatch till the given LSN is replayed, the postmaster dies or
 * timeout happens.
 */
void
WaitForLSN(XLogRecPtr targetLSN, int64 timeout)
{
	XLogRecPtr	currentLSN;
	TimestampTz endtime = 0;

	/* Shouldn't be called when shmem isn't initialized */
	Assert(waitLSN);

	/* Should be only called by a backend */
	Assert(MyBackendType == B_BACKEND && MyProcNumber <= MaxBackends);

	if (!RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is not in progress"),
				 errhint("Waiting for LSN can only be executed during recovery.")));

	/* If target LSN is already replayed, exit immediately */
	if (targetLSN <= GetXLogReplayRecPtr(NULL))
		return;

	if (timeout > 0)
		endtime = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), timeout);

	addLSNWaiter(targetLSN);

	for (;;)
	{
		int			rc;
		int			latch_events = WL_LATCH_SET | WL_EXIT_ON_PM_DEATH;
		long		delay_ms = 0;

		/* Check if the waited LSN has been replayed */
		currentLSN = GetXLogReplayRecPtr(NULL);
		if (targetLSN <= currentLSN)
			break;

		/* Recheck that recovery is still in-progress */
		if (!RecoveryInProgress())
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("recovery is not in progress"),
					 errdetail("Recovery ended before replaying the target LSN %X/%X; last replay LSN %X/%X.",
							   LSN_FORMAT_ARGS(targetLSN),
							   LSN_FORMAT_ARGS(currentLSN))));

		if (timeout > 0)
		{
			delay_ms = (endtime - GetCurrentTimestamp()) / 1000;
			latch_events |= WL_TIMEOUT;
			if (delay_ms <= 0)
				break;
		}

		CHECK_FOR_INTERRUPTS();

		rc = WaitLatch(MyLatch, latch_events, delay_ms,
					   WAIT_EVENT_WAIT_FOR_WAL_REPLAY);

		if (rc & WL_LATCH_SET)
			ResetLatch(MyLatch);
	}

	if (targetLSN > currentLSN)
	{
		deleteLSNWaiter();
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
	CallContext *context = (CallContext *) fcinfo->context;

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
	 * At first, we check that pg_wal_replay_wait() is called in a non-atomic
	 * context.  That is, a procedure call isn't wrapped into a transaction,
	 * another procedure call, or a function call.
	 *
	 * Secondly, according to PlannedStmtRequiresSnapshot(), even in an atomic
	 * context, CallStmt is processed with a snapshot.  Thankfully, we can pop
	 * this snapshot, because PortalRunUtility() can tolerate this.
	 */
	if (context->atomic)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_wal_replay_wait() must be only called in non-atomic context"),
				 errdetail("Make sure pg_wal_replay_wait() isn't called within a transaction, another procedure, or a function.")));

	if (ActiveSnapshotSet())
		PopActiveSnapshot();
	Assert(!ActiveSnapshotSet());
	InvalidateCatalogSnapshot();
	Assert(MyProc->xmin == InvalidTransactionId);

	(void) WaitForLSN(target_lsn, timeout);

	PG_RETURN_VOID();
}
