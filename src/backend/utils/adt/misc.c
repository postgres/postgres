/*-------------------------------------------------------------------------
 *
 * misc.c
 *
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/misc.c,v 1.38 2004/08/29 05:06:49 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/file.h>
#include <signal.h>
#include <dirent.h>

#include "commands/dbcommands.h"
#include "miscadmin.h"
#include "storage/sinval.h"
#include "storage/fd.h"
#include "utils/builtins.h"
#include "funcapi.h"
#include "catalog/pg_type.h"
#include "catalog/pg_tablespace.h"

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

static int
pg_signal_backend(int pid, int sig)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
		(errmsg("must be superuser to signal other server processes"))));

	if (!IsBackendPid(pid))
	{
		/*
		 * This is just a warning so a loop-through-resultset will not
		 * abort if one backend terminated on it's own during the run
		 */
		ereport(WARNING,
			 (errmsg("PID %d is not a PostgreSQL server process", pid)));
		return 0;
	}

	if (kill(pid, sig))
	{
		/* Again, just a warning to allow loops */
		ereport(WARNING,
				(errmsg("could not send signal to process %d: %m", pid)));
		return 0;
	}
	return 1;
}

Datum
pg_cancel_backend(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(pg_signal_backend(PG_GETARG_INT32(0), SIGINT));
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
		 * size = path length + tablespace dirname length + 2 dir sep
		 * chars + oid + terminator
		 */
		fctx->location = (char *) palloc(strlen(DataDir) + 11 + 10 + 1);
		if (tablespaceOid == GLOBALTABLESPACE_OID)
		{
			fctx->dirdesc = NULL;
			ereport(WARNING,
					(errmsg("global tablespace never has databases")));
		}
		else
		{
			if (tablespaceOid == DEFAULTTABLESPACE_OID)
				sprintf(fctx->location, "%s/base", DataDir);
			else
				sprintf(fctx->location, "%s/pg_tblspc/%u", DataDir,
						tablespaceOid);

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
				  (errmsg("%u is not a tablespace oid", tablespaceOid)));
			}
		}
		funcctx->user_fctx = fctx;
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	fctx = (ts_db_fctx *) funcctx->user_fctx;

	if (!fctx->dirdesc)			/* not a tablespace */
		SRF_RETURN_DONE(funcctx);

	while ((de = readdir(fctx->dirdesc)) != NULL)
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
		pfree(subdir);
		if (!dirdesc)
			continue;			/* XXX more sloppiness */

		while ((de = readdir(dirdesc)) != 0)
		{
			if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0)
				break;
		}
		FreeDir(dirdesc);

		if (!de)
			continue;			/* indeed, nothing in it */

		SRF_RETURN_NEXT(funcctx, ObjectIdGetDatum(datOid));
	}

	FreeDir(fctx->dirdesc);
	SRF_RETURN_DONE(funcctx);
}
