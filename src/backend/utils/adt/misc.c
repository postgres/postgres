/*-------------------------------------------------------------------------
 *
 * misc.c
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/misc.c,v 1.55 2006/11/21 20:59:53 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/file.h>
#include <signal.h>
#include <dirent.h>
#include <math.h>

#include "access/xact.h"
#include "catalog/pg_tablespace.h"
#include "commands/dbcommands.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "postmaster/syslogger.h"
#include "storage/fd.h"
#include "storage/pmsignal.h"
#include "storage/procarray.h"
#include "utils/builtins.h"

#define atooid(x)  ((Oid) strtoul((x), NULL, 10))


/*
 * Check if data is Null
 */
Datum
nullvalue(PG_FUNCTION_ARGS)
{
	if (PG_ARGISNULL(0))
		PG_RETURN_BOOL(true);
	PG_RETURN_BOOL(false);
}

/*
 * Check if data is not Null
 */
Datum
nonnullvalue(PG_FUNCTION_ARGS)
{
	if (PG_ARGISNULL(0))
		PG_RETURN_BOOL(false);
	PG_RETURN_BOOL(true);
}

/*
 * current_database()
 *	Expose the current database to the user
 */
Datum
current_database(PG_FUNCTION_ARGS)
{
	Name		db;

	db = (Name) palloc(NAMEDATALEN);

	namestrcpy(db, get_database_name(MyDatabaseId));
	PG_RETURN_NAME(db);
}


/*
 * Functions to send signals to other backends.
 */
static bool
pg_signal_backend(int pid, int sig)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			(errmsg("must be superuser to signal other server processes"))));

	if (!IsBackendPid(pid))
	{
		/*
		 * This is just a warning so a loop-through-resultset will not abort
		 * if one backend terminated on it's own during the run
		 */
		ereport(WARNING,
				(errmsg("PID %d is not a PostgreSQL server process", pid)));
		return false;
	}

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
		return false;
	}
	return true;
}

Datum
pg_cancel_backend(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(pg_signal_backend(PG_GETARG_INT32(0), SIGINT));
}

Datum
pg_reload_conf(PG_FUNCTION_ARGS)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to signal the postmaster"))));

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
 */
Datum
pg_rotate_logfile(PG_FUNCTION_ARGS)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to rotate log files"))));

	if (!Redirect_stderr)
	{
		ereport(WARNING,
		(errmsg("rotation not possible because log redirection not active")));
		PG_RETURN_BOOL(false);
	}

	SendPostmasterSignal(PMSIGNAL_ROTATE_LOGFILE);
	PG_RETURN_BOOL(true);
}

#ifdef NOT_USED

/* Disabled in 8.0 due to reliability concerns; FIXME someday */
Datum
pg_terminate_backend(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(pg_signal_backend(PG_GETARG_INT32(0), SIGTERM));
}
#endif


/* Function to find out which databases make use of a tablespace */

typedef struct
{
	char	   *location;
	DIR		   *dirdesc;
} ts_db_fctx;

Datum
pg_tablespace_databases(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	struct dirent *de;
	ts_db_fctx *fctx;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		Oid			tablespaceOid = PG_GETARG_OID(0);

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		fctx = palloc(sizeof(ts_db_fctx));

		/*
		 * size = tablespace dirname length + dir sep char + oid + terminator
		 */
		fctx->location = (char *) palloc(10 + 10 + 1);
		if (tablespaceOid == GLOBALTABLESPACE_OID)
		{
			fctx->dirdesc = NULL;
			ereport(WARNING,
					(errmsg("global tablespace never has databases")));
		}
		else
		{
			if (tablespaceOid == DEFAULTTABLESPACE_OID)
				sprintf(fctx->location, "base");
			else
				sprintf(fctx->location, "pg_tblspc/%u", tablespaceOid);

			fctx->dirdesc = AllocateDir(fctx->location);

			if (!fctx->dirdesc)
			{
				/* the only expected error is ENOENT */
				if (errno != ENOENT)
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not open directory \"%s\": %m",
									fctx->location)));
				ereport(WARNING,
					  (errmsg("%u is not a tablespace OID", tablespaceOid)));
			}
		}
		funcctx->user_fctx = fctx;
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	fctx = (ts_db_fctx *) funcctx->user_fctx;

	if (!fctx->dirdesc)			/* not a tablespace */
		SRF_RETURN_DONE(funcctx);

	while ((de = ReadDir(fctx->dirdesc, fctx->location)) != NULL)
	{
		char	   *subdir;
		DIR		   *dirdesc;
		Oid			datOid = atooid(de->d_name);

		/* this test skips . and .., but is awfully weak */
		if (!datOid)
			continue;

		/* if database subdir is empty, don't report tablespace as used */

		/* size = path length + dir sep char + file name + terminator */
		subdir = palloc(strlen(fctx->location) + 1 + strlen(de->d_name) + 1);
		sprintf(subdir, "%s/%s", fctx->location, de->d_name);
		dirdesc = AllocateDir(subdir);
		while ((de = ReadDir(dirdesc, subdir)) != NULL)
		{
			if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0)
				break;
		}
		FreeDir(dirdesc);
		pfree(subdir);

		if (!de)
			continue;			/* indeed, nothing in it */

		SRF_RETURN_NEXT(funcctx, ObjectIdGetDatum(datOid));
	}

	FreeDir(fctx->dirdesc);
	SRF_RETURN_DONE(funcctx);
}


/*
 * pg_sleep - delay for N seconds
 */
Datum
pg_sleep(PG_FUNCTION_ARGS)
{
	float8		secs = PG_GETARG_FLOAT8(0);
	float8		endtime;

	/*
	 * We break the requested sleep into segments of no more than 1 second, to
	 * put an upper bound on how long it will take us to respond to a cancel
	 * or die interrupt.  (Note that pg_usleep is interruptible by signals on
	 * some platforms but not others.)	Also, this method avoids exposing
	 * pg_usleep's upper bound on allowed delays.
	 *
	 * By computing the intended stop time initially, we avoid accumulation of
	 * extra delay across multiple sleeps.	This also ensures we won't delay
	 * less than the specified time if pg_usleep is interrupted by other
	 * signals such as SIGHUP.
	 */

#ifdef HAVE_INT64_TIMESTAMP
#define GetNowFloat()	((float8) GetCurrentTimestamp() / 1000000.0)
#else
#define GetNowFloat()	GetCurrentTimestamp()
#endif

	endtime = GetNowFloat() + secs;

	for (;;)
	{
		float8		delay;

		CHECK_FOR_INTERRUPTS();
		delay = endtime - GetNowFloat();
		if (delay >= 1.0)
			pg_usleep(1000000L);
		else if (delay > 0.0)
			pg_usleep((long) ceil(delay * 1000000.0));
		else
			break;
	}

	PG_RETURN_VOID();
}
