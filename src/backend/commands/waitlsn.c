/*-------------------------------------------------------------------------
 *
 * waitlsn.c
 *	  Implements waiting for the given LSN, which is used in
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
#include "fmgr.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlogdefs.h"
#include "access/xlogrecovery.h"
#include "catalog/pg_type.h"
#include "commands/waitlsn.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "storage/sinvaladt.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/fmgrprotos.h"

/* Add to / delete from shared memory array */
static void addLSNWaiter(XLogRecPtr lsn);
static void deleteLSNWaiter(void);

struct WaitLSNState *waitLSN = NULL;
static volatile sig_atomic_t haveShmemItem = false;

/*
 * Report the amount of shared memory space needed for WaitLSNState
 */
Size
WaitLSNShmemSize(void)
{
	Size		size;

	size = offsetof(WaitLSNState, procInfos);
	size = add_size(size, mul_size(MaxBackends, sizeof(WaitLSNProcInfo)));
	return size;
}

/* Initialize the WaitLSNState in the shared memory */
void
WaitLSNShmemInit(void)
{
	bool		found;

	waitLSN = (WaitLSNState *) ShmemInitStruct("WaitLSNState",
											   WaitLSNShmemSize(),
											   &found);
	if (!found)
	{
		SpinLockInit(&waitLSN->mutex);
		waitLSN->numWaitedProcs = 0;
		pg_atomic_init_u64(&waitLSN->minLSN, PG_UINT64_MAX);
	}
}

/*
 * Add the information about the LSN waiter backend to the shared memory
 * array.
 */
static void
addLSNWaiter(XLogRecPtr lsn)
{
	WaitLSNProcInfo cur;
	int			i;

	SpinLockAcquire(&waitLSN->mutex);

	cur.procnum = MyProcNumber;
	cur.waitLSN = lsn;

	for (i = 0; i < waitLSN->numWaitedProcs; i++)
	{
		if (waitLSN->procInfos[i].waitLSN >= cur.waitLSN)
		{
			WaitLSNProcInfo tmp;

			tmp = waitLSN->procInfos[i];
			waitLSN->procInfos[i] = cur;
			cur = tmp;
		}
	}
	waitLSN->procInfos[i] = cur;
	waitLSN->numWaitedProcs++;

	pg_atomic_write_u64(&waitLSN->minLSN, waitLSN->procInfos[i].waitLSN);
	SpinLockRelease(&waitLSN->mutex);
}

/*
 * Delete the information about the LSN waiter backend from the shared memory
 * array.
 */
static void
deleteLSNWaiter(void)
{
	int			i;
	bool		found = false;

	SpinLockAcquire(&waitLSN->mutex);

	for (i = 0; i < waitLSN->numWaitedProcs; i++)
	{
		if (waitLSN->procInfos[i].procnum == MyProcNumber)
			found = true;

		if (found && i < waitLSN->numWaitedProcs - 1)
		{
			waitLSN->procInfos[i] = waitLSN->procInfos[i + 1];
		}
	}

	if (!found)
	{
		SpinLockRelease(&waitLSN->mutex);
		return;
	}
	waitLSN->numWaitedProcs--;

	if (waitLSN->numWaitedProcs != 0)
		pg_atomic_write_u64(&waitLSN->minLSN, waitLSN->procInfos[i].waitLSN);
	else
		pg_atomic_write_u64(&waitLSN->minLSN, PG_UINT64_MAX);

	SpinLockRelease(&waitLSN->mutex);
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
	int			numWakeUpProcs;

	wakeUpProcNums = palloc(sizeof(int) * MaxBackends);

	SpinLockAcquire(&waitLSN->mutex);

	/*
	 * Remember processes, whose waited LSNs are already replayed.  We should
	 * set their latches later after spinlock release.
	 */
	for (i = 0; i < waitLSN->numWaitedProcs; i++)
	{
		if (!XLogRecPtrIsInvalid(currentLSN) &&
			waitLSN->procInfos[i].waitLSN > currentLSN)
			break;

		wakeUpProcNums[i] = waitLSN->procInfos[i].procnum;
	}

	/*
	 * Immediately remove those processes from the shmem array.  Otherwise,
	 * shmem array items will be here till corresponding processes wake up and
	 * delete themselves.
	 */
	numWakeUpProcs = i;
	for (i = 0; i < waitLSN->numWaitedProcs - numWakeUpProcs; i++)
		waitLSN->procInfos[i] = waitLSN->procInfos[i + numWakeUpProcs];
	waitLSN->numWaitedProcs -= numWakeUpProcs;

	if (waitLSN->numWaitedProcs != 0)
		pg_atomic_write_u64(&waitLSN->minLSN, waitLSN->procInfos[i].waitLSN);
	else
		pg_atomic_write_u64(&waitLSN->minLSN, PG_UINT64_MAX);

	SpinLockRelease(&waitLSN->mutex);

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
	if (haveShmemItem)
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
	TimestampTz endtime;

	/* Shouldn't be called when shmem isn't initialized */
	Assert(waitLSN);

	/* Should be only called by a backend */
	Assert(MyBackendType == B_BACKEND);

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
	haveShmemItem = true;

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
		haveShmemItem = false;
		ereport(ERROR,
				(errcode(ERRCODE_QUERY_CANCELED),
				 errmsg("timed out while waiting for target LSN %X/%X to be replayed; current replay LSN %X/%X",
						LSN_FORMAT_ARGS(targetLSN),
						LSN_FORMAT_ARGS(currentLSN))));
	}
	else
	{
		haveShmemItem = false;
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
