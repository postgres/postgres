/*--------------------------------------------------------------------------
 *
 * test_wait_lsn.c
 *		Test support for WAIT FOR LSN.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_wait_lsn/test_wait_lsn.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlogwait.h"
#include "fmgr.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_wait_lsn_wakeup);
PG_FUNCTION_INFO_V1(test_wait_lsn_waiter_is_registered);

static WaitLSNType
parse_wait_lsn_type(text *mode_text)
{
	char	   *mode = text_to_cstring(mode_text);
	WaitLSNType lsn_type;

	if (pg_strcasecmp(mode, "standby_replay") == 0)
		lsn_type = WAIT_LSN_TYPE_STANDBY_REPLAY;
	else if (pg_strcasecmp(mode, "standby_write") == 0)
		lsn_type = WAIT_LSN_TYPE_STANDBY_WRITE;
	else if (pg_strcasecmp(mode, "standby_flush") == 0)
		lsn_type = WAIT_LSN_TYPE_STANDBY_FLUSH;
	else if (pg_strcasecmp(mode, "primary_flush") == 0)
		lsn_type = WAIT_LSN_TYPE_PRIMARY_FLUSH;
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unrecognized WAIT FOR LSN mode \"%s\"", mode)));

	pfree(mode);
	return lsn_type;
}

/*
 * Wake all waiters of the supplied type through the supplied LSN without
 * advancing the underlying WAL position.
 */
Datum
test_wait_lsn_wakeup(PG_FUNCTION_ARGS)
{
	WaitLSNType lsn_type = parse_wait_lsn_type(PG_GETARG_TEXT_PP(0));
	XLogRecPtr	upto_lsn = PG_GETARG_LSN(1);

	WaitLSNWakeup(lsn_type, upto_lsn);

	PG_RETURN_VOID();
}

/*
 * Check whether the backend with the supplied PID is registered for the
 * supplied mode and target.  ProcArrayLock stabilizes the PID mapping, while
 * WaitLSNLock protects the registration state.
 */
Datum
test_wait_lsn_waiter_is_registered(PG_FUNCTION_ARGS)
{
	int			pid = PG_GETARG_INT32(0);
	WaitLSNType lsn_type = parse_wait_lsn_type(PG_GETARG_TEXT_PP(1));
	XLogRecPtr	target_lsn = PG_GETARG_LSN(2);
	bool		registered = false;
	PGPROC	   *proc;

	LWLockAcquire(ProcArrayLock, LW_SHARED);
	proc = BackendPidGetProcWithLock(pid);

	if (proc != NULL)
	{
		ProcNumber	procno = GetNumberFromPGProc(proc);
		WaitLSNProcInfo *proc_info = &waitLSNState->procInfos[procno];

		LWLockAcquire(WaitLSNLock, LW_SHARED);
		registered = proc_info->inHeap &&
			proc_info->procno == procno &&
			proc_info->lsnType == lsn_type &&
			proc_info->waitLSN == target_lsn;
		LWLockRelease(WaitLSNLock);
	}

	LWLockRelease(ProcArrayLock);

	PG_RETURN_BOOL(registered);
}
