/*-------------------------------------------------------------------------
 *
 * misc.c
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/misc.c,v 1.35 2004/07/02 18:59:22 joe Exp $
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
 * Functions to terminate a backend or cancel a query running on
 * a different backend.
 */

static int pg_signal_backend(int pid, int sig) 
{
	if (!superuser()) 
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("only superuser can signal other backends"))));
	
	if (!IsBackendPid(pid))
	{
		/* This is just a warning so a loop-through-resultset will not abort
		 * if one backend terminated on it's own during the run */
		ereport(WARNING,
				(errmsg("pid %i is not a postgresql backend",pid)));
		return 0;
	}

	if (kill(pid, sig)) 
	{
		/* Again, just a warning to allow loops */
		ereport(WARNING,
				(errmsg("failed to send signal to backend %i: %m",pid)));
		return 0;
	}
	return 1;
}

Datum
pg_terminate_backend(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(pg_signal_backend(PG_GETARG_INT32(0),SIGTERM));
}

Datum
pg_cancel_backend(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(pg_signal_backend(PG_GETARG_INT32(0),SIGINT));
}


typedef struct 
{
	char *location;
	DIR *dirdesc;
} ts_db_fctx;

Datum pg_tablespace_databases(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	struct dirent *de;
	ts_db_fctx *fctx;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		Oid tablespaceOid=PG_GETARG_OID(0);

		funcctx=SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		fctx = palloc(sizeof(ts_db_fctx));

		/*
		 * size = path length + tablespace dirname length
		 *        + 2 dir sep chars + oid + terminator
		 */
		fctx->location = (char*) palloc(strlen(DataDir) + 11 + 10 + 1);
		if (tablespaceOid == GLOBALTABLESPACE_OID)
		{
			fctx->dirdesc = NULL;
			ereport(NOTICE,
					(errcode(ERRCODE_WARNING),
					 errmsg("global tablespace never has databases.")));
		}
		else
		{
			if (tablespaceOid == DEFAULTTABLESPACE_OID)
				sprintf(fctx->location, "%s/base", DataDir);
			else
				sprintf(fctx->location, "%s/pg_tblspc/%u", DataDir,
														   tablespaceOid);
		
			fctx->dirdesc = AllocateDir(fctx->location);

			if (!fctx->dirdesc)  /* not a tablespace */
				ereport(NOTICE,
						(errcode(ERRCODE_WARNING),
						 errmsg("%d is no tablespace oid.", tablespaceOid)));
		}
		funcctx->user_fctx = fctx;
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx=SRF_PERCALL_SETUP();
	fctx = (ts_db_fctx*) funcctx->user_fctx;

	if (!fctx->dirdesc)  /* not a tablespace */
		SRF_RETURN_DONE(funcctx);

	while ((de = readdir(fctx->dirdesc)) != NULL)
	{
		char *subdir;
		DIR *dirdesc;

		Oid datOid = atol(de->d_name);
		if (!datOid)
			continue;

		/* size = path length + dir sep char + file name + terminator */
		subdir = palloc(strlen(fctx->location) + 1 + strlen(de->d_name) + 1);
		sprintf(subdir, "%s/%s", fctx->location, de->d_name);
		dirdesc = AllocateDir(subdir);
		if (dirdesc)
		{
			while ((de = readdir(dirdesc)) != 0)
			{
				if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
					break;
			}
			pfree(subdir);
			FreeDir(dirdesc);

			if (!de)   /* database subdir is empty; don't report tablespace as used */
				continue;
		}

		SRF_RETURN_NEXT(funcctx, ObjectIdGetDatum(datOid));
	}

	FreeDir(fctx->dirdesc);
	SRF_RETURN_DONE(funcctx);
}
