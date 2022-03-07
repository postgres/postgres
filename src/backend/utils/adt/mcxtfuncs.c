/*-------------------------------------------------------------------------
 *
 * mcxtfuncs.c
 *	  Functions to show backend memory context.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/mcxtfuncs.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "funcapi.h"
#include "miscadmin.h"
#include "mb/pg_wchar.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/builtins.h"

/* ----------
 * The max bytes for showing identifiers of MemoryContext.
 * ----------
 */
#define MEMORY_CONTEXT_IDENT_DISPLAY_SIZE	1024

/*
 * PutMemoryContextsStatsTupleStore
 *		One recursion level for pg_get_backend_memory_contexts.
 */
static void
PutMemoryContextsStatsTupleStore(Tuplestorestate *tupstore,
								 TupleDesc tupdesc, MemoryContext context,
								 const char *parent, int level)
{
#define PG_GET_BACKEND_MEMORY_CONTEXTS_COLS	9

	Datum		values[PG_GET_BACKEND_MEMORY_CONTEXTS_COLS];
	bool		nulls[PG_GET_BACKEND_MEMORY_CONTEXTS_COLS];
	MemoryContextCounters stat;
	MemoryContext child;
	const char *name;
	const char *ident;

	AssertArg(MemoryContextIsValid(context));

	name = context->name;
	ident = context->ident;

	/*
	 * To be consistent with logging output, we label dynahash contexts with
	 * just the hash table name as with MemoryContextStatsPrint().
	 */
	if (ident && strcmp(name, "dynahash") == 0)
	{
		name = ident;
		ident = NULL;
	}

	/* Examine the context itself */
	memset(&stat, 0, sizeof(stat));
	(*context->methods->stats) (context, NULL, (void *) &level, &stat, true);

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));

	if (name)
		values[0] = CStringGetTextDatum(name);
	else
		nulls[0] = true;

	if (ident)
	{
		int			idlen = strlen(ident);
		char		clipped_ident[MEMORY_CONTEXT_IDENT_DISPLAY_SIZE];

		/*
		 * Some identifiers such as SQL query string can be very long,
		 * truncate oversize identifiers.
		 */
		if (idlen >= MEMORY_CONTEXT_IDENT_DISPLAY_SIZE)
			idlen = pg_mbcliplen(ident, idlen, MEMORY_CONTEXT_IDENT_DISPLAY_SIZE - 1);

		memcpy(clipped_ident, ident, idlen);
		clipped_ident[idlen] = '\0';
		values[1] = CStringGetTextDatum(clipped_ident);
	}
	else
		nulls[1] = true;

	if (parent)
		values[2] = CStringGetTextDatum(parent);
	else
		nulls[2] = true;

	values[3] = Int32GetDatum(level);
	values[4] = Int64GetDatum(stat.totalspace);
	values[5] = Int64GetDatum(stat.nblocks);
	values[6] = Int64GetDatum(stat.freespace);
	values[7] = Int64GetDatum(stat.freechunks);
	values[8] = Int64GetDatum(stat.totalspace - stat.freespace);
	tuplestore_putvalues(tupstore, tupdesc, values, nulls);

	for (child = context->firstchild; child != NULL; child = child->nextchild)
	{
		PutMemoryContextsStatsTupleStore(tupstore, tupdesc,
										 child, name, level + 1);
	}
}

/*
 * pg_get_backend_memory_contexts
 *		SQL SRF showing backend memory context.
 */
Datum
pg_get_backend_memory_contexts(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	SetSingleFuncCall(fcinfo, 0);
	PutMemoryContextsStatsTupleStore(rsinfo->setResult, rsinfo->setDesc,
									 TopMemoryContext, NULL, 0);

	return (Datum) 0;
}

/*
 * pg_log_backend_memory_contexts
 *		Signal a backend or an auxiliary process to log its memory contexts.
 *
 * By default, only superusers are allowed to signal to log the memory
 * contexts because allowing any users to issue this request at an unbounded
 * rate would cause lots of log messages and which can lead to denial of
 * service. Additional roles can be permitted with GRANT.
 *
 * On receipt of this signal, a backend or an auxiliary process sets the flag
 * in the signal handler, which causes the next CHECK_FOR_INTERRUPTS()
 * or process-specific interrupt handler to log the memory contexts.
 */
Datum
pg_log_backend_memory_contexts(PG_FUNCTION_ARGS)
{
	int			pid = PG_GETARG_INT32(0);
	PGPROC	   *proc;
	BackendId	backendId = InvalidBackendId;

	proc = BackendPidGetProc(pid);

	/*
	 * See if the process with given pid is a backend or an auxiliary process.
	 *
	 * If the given process is a backend, use its backend id in
	 * SendProcSignal() later to speed up the operation. Otherwise, don't do
	 * that because auxiliary processes (except the startup process) don't
	 * have a valid backend id.
	 */
	if (proc != NULL)
		backendId = proc->backendId;
	else
		proc = AuxiliaryPidGetProc(pid);

	/*
	 * BackendPidGetProc() and AuxiliaryPidGetProc() return NULL if the pid
	 * isn't valid; but by the time we reach kill(), a process for which we
	 * get a valid proc here might have terminated on its own.  There's no way
	 * to acquire a lock on an arbitrary process to prevent that. But since
	 * this mechanism is usually used to debug a backend or an auxiliary
	 * process running and consuming lots of memory, that it might end on its
	 * own first and its memory contexts are not logged is not a problem.
	 */
	if (proc == NULL)
	{
		/*
		 * This is just a warning so a loop-through-resultset will not abort
		 * if one backend terminated on its own during the run.
		 */
		ereport(WARNING,
				(errmsg("PID %d is not a PostgreSQL server process", pid)));
		PG_RETURN_BOOL(false);
	}

	if (SendProcSignal(pid, PROCSIG_LOG_MEMORY_CONTEXT, backendId) < 0)
	{
		/* Again, just a warning to allow loops */
		ereport(WARNING,
				(errmsg("could not send signal to process %d: %m", pid)));
		PG_RETURN_BOOL(false);
	}

	PG_RETURN_BOOL(true);
}
