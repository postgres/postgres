/*-------------------------------------------------------------------------
 *
 * wait.c
 *	  Implements WAIT FOR clause for BEGIN and START TRANSACTION commands.
 *	  This clause allows waiting for given LSN to be replayed on standby.
 *
 * Copyright (c) 2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/commands/wait.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/xlog.h"
#include "access/xlogdefs.h"
#include "commands/wait.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/backendid.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "storage/sinvaladt.h"
#include "storage/spin.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"
#include "utils/timestamp.h"

/*
 * Shared memory structure representing information about LSNs, which backends
 * are waiting for replay.
 */
typedef struct
{
	slock_t		mutex;			/* mutex protecting the fields below */
	int			max_backend_id; /* max backend_id present in lsns[] */
	pg_atomic_uint64 min_lsn;	/* minimal waited LSN */
	/* per-backend array of waited LSNs */
	XLogRecPtr	lsns[FLEXIBLE_ARRAY_MEMBER];
}			WaitLSNState;

static WaitLSNState * state;

/*
 * Add the wait event of the current backend to shared memory array
 */
static void
WaitLSNAdd(XLogRecPtr lsn_to_wait)
{
	SpinLockAcquire(&state->mutex);
	if (state->max_backend_id < MyBackendId)
		state->max_backend_id = MyBackendId;

	state->lsns[MyBackendId] = lsn_to_wait;

	if (lsn_to_wait < state->min_lsn.value)
		state->min_lsn.value = lsn_to_wait;
	SpinLockRelease(&state->mutex);
}

/*
 * Delete wait event of the current backend from the shared memory array.
 */
void
WaitLSNDelete(void)
{
	int			i;
	XLogRecPtr	deleted_lsn;

	SpinLockAcquire(&state->mutex);

	deleted_lsn = state->lsns[MyBackendId];
	state->lsns[MyBackendId] = InvalidXLogRecPtr;

	/* If we are deleting the minimal LSN, then choose the next min_lsn */
	if (!XLogRecPtrIsInvalid(deleted_lsn) &&
		deleted_lsn == state->min_lsn.value)
	{
		state->min_lsn.value = InvalidXLogRecPtr;
		for (i = 2; i <= state->max_backend_id; i++)
		{
			if (!XLogRecPtrIsInvalid(state->lsns[i]) &&
				(state->lsns[i] < state->min_lsn.value ||
				 XLogRecPtrIsInvalid(state->min_lsn.value)))
			{
				state->min_lsn.value = state->lsns[i];
			}
		}
	}

	/* If deleting from the end of the array, shorten the array's used part */
	if (state->max_backend_id == MyBackendId)
	{
		for (i = (MyBackendId); i >= 2; i--)
			if (!XLogRecPtrIsInvalid(state->lsns[i]))
			{
				state->max_backend_id = i;
				break;
			}
	}

	SpinLockRelease(&state->mutex);
}

/*
 * Report amount of shared memory space needed for WaitLSNState
 */
Size
WaitLSNShmemSize(void)
{
	Size		size;

	size = offsetof(WaitLSNState, lsns);
	size = add_size(size, mul_size(MaxBackends + 1, sizeof(XLogRecPtr)));
	return size;
}

/*
 * Initialize an shared memory structure for waiting for LSN
 */
void
WaitLSNShmemInit(void)
{
	bool		found;
	uint32		i;

	state = (WaitLSNState *) ShmemInitStruct("pg_wait_lsn",
											 WaitLSNShmemSize(),
											 &found);
	if (!found)
	{
		SpinLockInit(&state->mutex);

		for (i = 0; i < (MaxBackends + 1); i++)
			state->lsns[i] = InvalidXLogRecPtr;

		state->max_backend_id = 0;
		pg_atomic_init_u64(&state->min_lsn, InvalidXLogRecPtr);
	}
}

/*
 * Set latches in shared memory to signal that new LSN has been replayed
 */
void
WaitLSNSetLatch(XLogRecPtr cur_lsn)
{
	uint32		i;
	int			max_backend_id;
	PGPROC	   *backend;

	SpinLockAcquire(&state->mutex);
	max_backend_id = state->max_backend_id;

	for (i = 2; i <= max_backend_id; i++)
	{
		backend = BackendIdGetProc(i);

		if (backend && state->lsns[i] != 0 &&
			state->lsns[i] <= cur_lsn)
		{
			SetLatch(&backend->procLatch);
		}
	}
	SpinLockRelease(&state->mutex);
}

/*
 * Get minimal LSN that some backend is waiting for
 */
XLogRecPtr
WaitLSNGetMin(void)
{
	return state->min_lsn.value;
}

/*
 * On WAIT use a latch to wait till LSN is replayed, postmaster dies or timeout
 * happens. Timeout is specified in milliseconds.  Returns true if LSN was
 * reached and false otherwise.
 */
bool
WaitLSNUtility(XLogRecPtr target_lsn, const int timeout_ms)
{
	XLogRecPtr	cur_lsn;
	int			latch_events;
	float8		endtime;
	bool		res = false;
	bool		wait_forever = (timeout_ms <= 0);

	endtime = GetNowFloat() + timeout_ms / 1000.0;

	latch_events = WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH;

	/* Check if we already reached the needed LSN */
	cur_lsn = GetXLogReplayRecPtr(NULL);
	if (cur_lsn >= target_lsn)
		return true;

	WaitLSNAdd(target_lsn);
	ResetLatch(MyLatch);

	/* Recheck if LSN was reached while WaitLSNAdd() and ResetLatch() */
	cur_lsn = GetXLogReplayRecPtr(NULL);
	if (cur_lsn >= target_lsn)
		return true;

	for (;;)
	{
		int			rc;
		float8		time_left = 0;
		long		time_left_ms = 0;

		time_left = endtime - GetNowFloat();

		/* Use 1 second as the default timeout to check for interrupts */
		if (wait_forever || time_left < 0 || time_left > 1.0)
			time_left_ms = 1000;
		else
			time_left_ms = (long) ceil(time_left * 1000.0);

		/* If interrupt, LockErrorCleanup() will do WaitLSNDelete() for us */
		CHECK_FOR_INTERRUPTS();

		/* If postmaster dies, finish immediately */
		if (!PostmasterIsAlive())
			break;

		rc = WaitLatch(MyLatch, latch_events, time_left_ms,
					   WAIT_EVENT_CLIENT_READ);

		ResetLatch(MyLatch);

		if (rc & WL_LATCH_SET)
			cur_lsn = GetXLogReplayRecPtr(NULL);

		if (rc & WL_TIMEOUT)
		{
			time_left = endtime - GetNowFloat();
			/* If the time specified by user has passed, stop waiting */
			if (!wait_forever && time_left <= 0.0)
				break;
			cur_lsn = GetXLogReplayRecPtr(NULL);
		}

		/* If LSN has been replayed */
		if (target_lsn <= cur_lsn)
			break;
	}

	WaitLSNDelete();

	if (cur_lsn < target_lsn)
		ereport(WARNING,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("didn't start transaction because LSN was not reached"),
				 errhint("Try to increase wait timeout.")));
	else
		res = true;

	return res;
}

/*
 * Implementation of WAIT FOR clause for BEGIN and START TRANSACTION commands
 */
int
WaitLSNMain(WaitClause *stmt, DestReceiver *dest)
{
	TupleDesc	tupdesc;
	TupOutputState *tstate;
	XLogRecPtr	target_lsn;
	bool		res = false;

	target_lsn = DatumGetLSN(DirectFunctionCall1(pg_lsn_in,
												 CStringGetDatum(stmt->lsn)));
	res = WaitLSNUtility(target_lsn, stmt->timeout);

	/* Need a tuple descriptor representing a single TEXT column */
	tupdesc = CreateTemplateTupleDesc(1);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "LSN reached", TEXTOID, -1, 0);

	/* Prepare for projection of tuples */
	tstate = begin_tup_output_tupdesc(dest, tupdesc, &TTSOpsMinimalTuple);

	/* Send the result */
	do_text_output_oneline(tstate, res ? "t" : "f");
	end_tup_output(tstate);
	return res;
}
