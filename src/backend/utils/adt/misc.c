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
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/misc.c,v 1.34 2004/06/02 21:29:29 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/file.h>
#include <signal.h>

#include "commands/dbcommands.h"
#include "miscadmin.h"
#include "storage/sinval.h"
#include "utils/builtins.h"


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
