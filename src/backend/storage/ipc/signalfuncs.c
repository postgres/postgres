/*-------------------------------------------------------------------------
 *
 * signalfuncs.c
 *	  Functions for signaling backends
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/signalfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>

#include "catalog/pg_authid.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/syslogger.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/builtins.h"


/*
 * Send a signal to another backend.
 *
 * The signal is delivered if the user is either a superuser or the same
 * role as the backend being signaled. For "dangerous" signals, an explicit
 * check for superuser needs to be done prior to calling this function.
 *
 * Returns 0 on success, 1 on general failure, 2 on normal permission error
 * and 3 if the caller needs to be a superuser.
 *
 * In the event of a general failure (return code 1), a warning message will
 * be emitted. For permission errors, doing that is the responsibility of
 * the caller.
 */
#define SIGNAL_BACKEND_SUCCESS 0
#define SIGNAL_BACKEND_ERROR 1
#define SIGNAL_BACKEND_NOPERMISSION 2
#define SIGNAL_BACKEND_NOSUPERUSER 3
static int
pg_signal_backend(int pid, int sig)
{
	PGPROC	   *proc = BackendPidGetProc(pid);

	/*
	 * BackendPidGetProc returns NULL if the pid isn't valid; but by the time
	 * we reach kill(), a process for which we get a valid proc here might
	 * have terminated on its own.  There's no way to acquire a lock on an
	 * arbitrary process to prevent that. But since so far all the callers of
	 * this mechanism involve some request for ending the process anyway, that
	 * it might end on its own first is not a problem.
	 *
	 * Note that proc will also be NULL if the pid refers to an auxiliary
	 * process or the postmaster (neither of which can be signaled via
	 * pg_signal_backend()).
	 */
	if (proc == NULL)
	{
		/*
		 * This is just a warning so a loop-through-resultset will not abort
		 * if one backend terminated on its own during the run.
		 */
		ereport(WARNING,
				(errmsg("PID %d is not a PostgreSQL backend process", pid)));

		return SIGNAL_BACKEND_ERROR;
	}

	/*
	 * Only allow superusers to signal superuser-owned backends.  Any process
	 * not advertising a role might have the importance of a superuser-owned
	 * backend, so treat it that way.
	 */
	if ((!OidIsValid(proc->roleId) || superuser_arg(proc->roleId)) &&
		!superuser())
		return SIGNAL_BACKEND_NOSUPERUSER;

	/* Users can signal backends they have role membership in. */
	if (!has_privs_of_role(GetUserId(), proc->roleId) &&
		!has_privs_of_role(GetUserId(), ROLE_PG_SIGNAL_BACKEND))
		return SIGNAL_BACKEND_NOPERMISSION;

	/*
	 * Can the process we just validated above end, followed by the pid being
	 * recycled for a new process, before reaching here?  Then we'd be trying
	 * to kill the wrong thing.  Seems near impossible when sequential pid
	 * assignment and wraparound is used.  Perhaps it could happen on a system
	 * where pid re-use is randomized.  That race condition possibility seems
	 * too unlikely to worry about.
	 */

	/* If we have setsid(), signal the backend's whole process group */
#ifdef HAVE_SETSID
	if (kill(-pid, sig))
#else
	if (kill(pid, sig))
#endif
	{
		/* Again, just a warning to allow loops */
		ereport(WARNING,
				(errmsg("could not send signal to process %d: %m", pid)));
		return SIGNAL_BACKEND_ERROR;
	}
	return SIGNAL_BACKEND_SUCCESS;
}

/*
 * Signal to cancel a backend process.  This is allowed if you are a member of
 * the role whose process is being canceled.
 *
 * Note that only superusers can signal superuser-owned processes.
 */
Datum
pg_cancel_backend(PG_FUNCTION_ARGS)
{
	int			r = pg_signal_backend(PG_GETARG_INT32(0), SIGINT);

	if (r == SIGNAL_BACKEND_NOSUPERUSER)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to cancel query"),
				 errdetail("Only roles with the %s attribute may cancel queries of roles with the %s attribute.",
						   "SUPERUSER", "SUPERUSER")));

	if (r == SIGNAL_BACKEND_NOPERMISSION)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to cancel query"),
				 errdetail("Only roles with privileges of the role whose query is being canceled or with privileges of the \"%s\" role may cancel this query.",
						   "pg_signal_backend")));

	PG_RETURN_BOOL(r == SIGNAL_BACKEND_SUCCESS);
}

/*
 * Wait until there is no backend process with the given PID and return true.
 * On timeout, a warning is emitted and false is returned.
 */
static bool
pg_wait_until_termination(int pid, int64 timeout)
{
	/*
	 * Wait in steps of waittime milliseconds until this function exits or
	 * timeout.
	 */
	int64		waittime = 100;

	/*
	 * Initially remaining time is the entire timeout specified by the user.
	 */
	int64		remainingtime = timeout;

	/*
	 * Check existence of the backend. If the backend still exists, then wait
	 * for waittime milliseconds, again check for the existence. Repeat this
	 * until timeout or an error occurs or a pending interrupt such as query
	 * cancel gets processed.
	 */
	do
	{
		if (remainingtime < waittime)
			waittime = remainingtime;

		if (kill(pid, 0) == -1)
		{
			if (errno == ESRCH)
				return true;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("could not check the existence of the backend with PID %d: %m",
								pid)));
		}

		/* Process interrupts, if any, before waiting */
		CHECK_FOR_INTERRUPTS();

		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 waittime,
						 WAIT_EVENT_BACKEND_TERMINATION);

		ResetLatch(MyLatch);

		remainingtime -= waittime;
	} while (remainingtime > 0);

	ereport(WARNING,
			(errmsg_plural("backend with PID %d did not terminate within %lld millisecond",
						   "backend with PID %d did not terminate within %lld milliseconds",
						   timeout,
						   pid, (long long int) timeout)));

	return false;
}

/*
 * Send a signal to terminate a backend process. This is allowed if you are a
 * member of the role whose process is being terminated. If the timeout input
 * argument is 0, then this function just signals the backend and returns
 * true.  If timeout is nonzero, then it waits until no process has the given
 * PID; if the process ends within the timeout, true is returned, and if the
 * timeout is exceeded, a warning is emitted and false is returned.
 *
 * Note that only superusers can signal superuser-owned processes.
 */
Datum
pg_terminate_backend(PG_FUNCTION_ARGS)
{
	int			pid;
	int			r;
	int			timeout;		/* milliseconds */

	pid = PG_GETARG_INT32(0);
	timeout = PG_GETARG_INT64(1);

	if (timeout < 0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("\"timeout\" must not be negative")));

	r = pg_signal_backend(pid, SIGTERM);

	if (r == SIGNAL_BACKEND_NOSUPERUSER)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to terminate process"),
				 errdetail("Only roles with the %s attribute may terminate processes of roles with the %s attribute.",
						   "SUPERUSER", "SUPERUSER")));

	if (r == SIGNAL_BACKEND_NOPERMISSION)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to terminate process"),
				 errdetail("Only roles with privileges of the role whose process is being terminated or with privileges of the \"%s\" role may terminate this process.",
						   "pg_signal_backend")));

	/* Wait only on success and if actually requested */
	if (r == SIGNAL_BACKEND_SUCCESS && timeout > 0)
		PG_RETURN_BOOL(pg_wait_until_termination(pid, timeout));
	else
		PG_RETURN_BOOL(r == SIGNAL_BACKEND_SUCCESS);
}

/*
 * Signal to reload the database configuration
 *
 * Permission checking for this function is managed through the normal
 * GRANT system.
 */
Datum
pg_reload_conf(PG_FUNCTION_ARGS)
{
	if (kill(PostmasterPid, SIGHUP))
	{
		ereport(WARNING,
				(errmsg("failed to send signal to postmaster: %m")));
		PG_RETURN_BOOL(false);
	}

	PG_RETURN_BOOL(true);
}


/*
 * Rotate log file
 *
 * This function is kept to support adminpack 1.0.
 */
Datum
pg_rotate_logfile(PG_FUNCTION_ARGS)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to rotate log files with adminpack 1.0"),
		/* translator: %s is a SQL function name */
				 errhint("Consider using %s, which is part of core, instead.",
						 "pg_logfile_rotate()")));

	if (!Logging_collector)
	{
		ereport(WARNING,
				(errmsg("rotation not possible because log collection not active")));
		PG_RETURN_BOOL(false);
	}

	SendPostmasterSignal(PMSIGNAL_ROTATE_LOGFILE);
	PG_RETURN_BOOL(true);
}

/*
 * Rotate log file
 *
 * Permission checking for this function is managed through the normal
 * GRANT system.
 */
Datum
pg_rotate_logfile_v2(PG_FUNCTION_ARGS)
{
	if (!Logging_collector)
	{
		ereport(WARNING,
				(errmsg("rotation not possible because log collection not active")));
		PG_RETURN_BOOL(false);
	}

	SendPostmasterSignal(PMSIGNAL_ROTATE_LOGFILE);
	PG_RETURN_BOOL(true);
}
