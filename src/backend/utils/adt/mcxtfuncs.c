/*-------------------------------------------------------------------------
 *
 * mcxtfuncs.c
 *	  Functions to show backend memory context.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "access/twophase.h"
#include "catalog/pg_authid_d.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/wait_event_types.h"

/* ----------
 * The max bytes for showing identifiers of MemoryContext.
 * ----------
 */
#define MEMORY_CONTEXT_IDENT_DISPLAY_SIZE	1024
struct MemoryStatsBackendState *memCxtState = NULL;
struct MemoryStatsCtl *memCxtArea = NULL;

/*
 * int_list_to_array
 *		Convert an IntList to an array of INT4OIDs.
 */
static Datum
int_list_to_array(const List *list)
{
	Datum	   *datum_array;
	int			length;
	ArrayType  *result_array;

	length = list_length(list);
	datum_array = (Datum *) palloc(length * sizeof(Datum));

	foreach_int(i, list)
		datum_array[foreach_current_index(i)] = Int32GetDatum(i);

	result_array = construct_array_builtin(datum_array, length, INT4OID);

	return PointerGetDatum(result_array);
}

/*
 * PutMemoryContextsStatsTupleStore
 *		Add details for the given MemoryContext to 'tupstore'.
 */
static void
PutMemoryContextsStatsTupleStore(Tuplestorestate *tupstore,
								 TupleDesc tupdesc, MemoryContext context,
								 HTAB *context_id_lookup)
{
#define PG_GET_BACKEND_MEMORY_CONTEXTS_COLS	10

	Datum		values[PG_GET_BACKEND_MEMORY_CONTEXTS_COLS];
	bool		nulls[PG_GET_BACKEND_MEMORY_CONTEXTS_COLS];
	MemoryContextCounters stat;
	List	   *path = NIL;
	const char *name;
	const char *ident;
	const char *type;

	Assert(MemoryContextIsValid(context));

	/*
	 * Figure out the transient context_id of this context and each of its
	 * ancestors.
	 */
	for (MemoryContext cur = context; cur != NULL; cur = cur->parent)
	{
		MemoryStatsContextId *entry;
		bool		found;

		entry = hash_search(context_id_lookup, &cur, HASH_FIND, &found);

		if (!found)
			elog(ERROR, "hash table corrupted");
		path = lcons_int(entry->context_id, path);
	}

	/* Examine the context itself */
	memset(&stat, 0, sizeof(stat));
	(*context->methods->stats) (context, NULL, NULL, &stat, true);

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));

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

	type = ContextTypeToString(context->type);

	values[2] = CStringGetTextDatum(type);
	values[3] = Int32GetDatum(list_length(path));	/* level */
	values[4] = int_list_to_array(path);
	values[5] = Int64GetDatum(stat.totalspace);
	values[6] = Int64GetDatum(stat.nblocks);
	values[7] = Int64GetDatum(stat.freespace);
	values[8] = Int64GetDatum(stat.freechunks);
	values[9] = Int64GetDatum(stat.totalspace - stat.freespace);

	tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	list_free(path);
}

/*
 * ContextTypeToString
 *		Returns a textual representation of a context type
 *
 * This should cover the same types as MemoryContextIsValid.
 */
const char *
ContextTypeToString(NodeTag type)
{
	const char *context_type;

	switch (type)
	{
		case T_AllocSetContext:
			context_type = "AllocSet";
			break;
		case T_GenerationContext:
			context_type = "Generation";
			break;
		case T_SlabContext:
			context_type = "Slab";
			break;
		case T_BumpContext:
			context_type = "Bump";
			break;
		default:
			context_type = "???";
			break;
	}
	return context_type;
}

/*
 * pg_get_backend_memory_contexts
 *		SQL SRF showing backend memory context.
 */
Datum
pg_get_backend_memory_contexts(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	int			context_id;
	List	   *contexts;
	HASHCTL		ctl;
	HTAB	   *context_id_lookup;

	ctl.keysize = sizeof(MemoryContext);
	ctl.entrysize = sizeof(MemoryStatsContextId);
	ctl.hcxt = CurrentMemoryContext;

	context_id_lookup = hash_create("pg_get_backend_memory_contexts",
									256,
									&ctl,
									HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	InitMaterializedSRF(fcinfo, 0);

	/*
	 * Here we use a non-recursive algorithm to visit all MemoryContexts
	 * starting with TopMemoryContext.  The reason we avoid using a recursive
	 * algorithm is because we want to assign the context_id breadth-first.
	 * I.e. all contexts at level 1 are assigned IDs before contexts at level
	 * 2.  Because contexts closer to TopMemoryContext are less likely to
	 * change, this makes the assigned context_id more stable.  Otherwise, if
	 * the first child of TopMemoryContext obtained an additional grandchild,
	 * the context_id for the second child of TopMemoryContext would change.
	 */
	contexts = list_make1(TopMemoryContext);

	/* TopMemoryContext will always have a context_id of 1 */
	context_id = 1;

	foreach_ptr(MemoryContextData, cur, contexts)
	{
		MemoryStatsContextId *entry;
		bool		found;

		/*
		 * Record the context_id that we've assigned to each MemoryContext.
		 * PutMemoryContextsStatsTupleStore needs this to populate the "path"
		 * column with the parent context_ids.
		 */
		entry = (MemoryStatsContextId *) hash_search(context_id_lookup, &cur,
													 HASH_ENTER, &found);
		entry->context_id = context_id++;
		Assert(!found);

		PutMemoryContextsStatsTupleStore(rsinfo->setResult,
										 rsinfo->setDesc,
										 cur,
										 context_id_lookup);

		/*
		 * Append all children onto the contexts list so they're processed by
		 * subsequent iterations.
		 */
		for (MemoryContext c = cur->firstchild; c != NULL; c = c->nextchild)
			contexts = lappend(contexts, c);
	}

	hash_destroy(context_id_lookup);

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
	ProcNumber	procNumber = INVALID_PROC_NUMBER;

	/*
	 * See if the process with given pid is a backend or an auxiliary process.
	 */
	proc = BackendPidGetProc(pid);
	if (proc == NULL)
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

	procNumber = GetNumberFromPGProc(proc);
	if (SendProcSignal(pid, PROCSIG_LOG_MEMORY_CONTEXT, procNumber) < 0)
	{
		/* Again, just a warning to allow loops */
		ereport(WARNING,
				(errmsg("could not send signal to process %d: %m", pid)));
		PG_RETURN_BOOL(false);
	}

	PG_RETURN_BOOL(true);
}

/*
 * pg_get_process_memory_contexts
 *		Signal a backend or an auxiliary process to send its memory contexts,
 *		wait for the results and display them.
 *
 * By default, only superusers or users with ROLE_PG_READ_ALL_STATS are allowed
 * to signal a process to return the memory contexts. This is because allowing
 * any users to issue this request at an unbounded rate would cause lots of
 * requests to be sent, which can lead to denial of service. Additional roles
 * can be permitted with GRANT.
 *
 * On receipt of this signal, a backend or an auxiliary process sets the flag
 * in the signal handler, which causes the next CHECK_FOR_INTERRUPTS()
 * or process-specific interrupt handler to copy the memory context details
 * to a dynamic shared memory space.
 *
 * We have defined a limit on DSA memory that could be allocated per process -
 * if the process has more memory contexts than what can fit in the allocated
 * size, the excess contexts are summarized and represented as cumulative total
 * at the end of the buffer.
 *
 * After sending the signal, wait on a condition variable. The publishing
 * backend, after copying the data to shared memory, sends signal on that
 * condition variable. There is one condition variable per publishing backend.
 * Once the condition variable is signalled, check if the latest memory context
 * information is available and display.
 *
 * If the publishing backend does not respond before the condition variable
 * times out, which is set to MEMSTATS_WAIT_TIMEOUT, retry given that there is
 * time left within the timeout specified by the user, before giving up and
 * returning previously published statistics, if any. If no previous statistics
 * exist, return NULL.
 */
#define MEMSTATS_WAIT_TIMEOUT 100
Datum
pg_get_process_memory_contexts(PG_FUNCTION_ARGS)
{
	int			pid = PG_GETARG_INT32(0);
	bool		summary = PG_GETARG_BOOL(1);
	double		timeout = PG_GETARG_FLOAT8(2);
	PGPROC	   *proc;
	ProcNumber	procNumber = INVALID_PROC_NUMBER;
	bool		proc_is_aux = false;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryStatsEntry *memcxt_info;
	TimestampTz start_timestamp;

	/*
	 * See if the process with given pid is a backend or an auxiliary process
	 * and remember the type for when we requery the process later.
	 */
	proc = BackendPidGetProc(pid);
	if (proc == NULL)
	{
		proc = AuxiliaryPidGetProc(pid);
		proc_is_aux = true;
	}

	/*
	 * BackendPidGetProc() and AuxiliaryPidGetProc() return NULL if the pid
	 * isn't valid; this is however not a problem and leave with a WARNING.
	 * See comment in pg_log_backend_memory_contexts for a discussion on this.
	 */
	if (proc == NULL)
	{
		/*
		 * This is just a warning so a loop-through-resultset will not abort
		 * if one backend terminated on its own during the run.
		 */
		ereport(WARNING,
				errmsg("PID %d is not a PostgreSQL server process", pid));
		PG_RETURN_NULL();
	}

	InitMaterializedSRF(fcinfo, 0);

	procNumber = GetNumberFromPGProc(proc);

	LWLockAcquire(&memCxtState[procNumber].lw_lock, LW_EXCLUSIVE);
	memCxtState[procNumber].summary = summary;
	LWLockRelease(&memCxtState[procNumber].lw_lock);

	start_timestamp = GetCurrentTimestamp();

	/*
	 * Send a signal to a PostgreSQL process, informing it we want it to
	 * produce information about its memory contexts.
	 */
	if (SendProcSignal(pid, PROCSIG_GET_MEMORY_CONTEXT, procNumber) < 0)
	{
		ereport(WARNING,
				errmsg("could not send signal to process %d: %m", pid));
		PG_RETURN_NULL();
	}

	/*
	 * Even if the proc has published statistics, the may not be due to the
	 * current request, but previously published stats.  Check if the stats
	 * are updated by comparing the timestamp, if the stats are newer than our
	 * previously recorded timestamp from before sending the procsignal, they
	 * must by definition be updated. Wait for the timeout specified by the
	 * user, following which display old statistics if available or return
	 * NULL.
	 */
	while (1)
	{
		long		msecs;

		/*
		 * We expect to come out of sleep when the requested process has
		 * finished publishing the statistics, verified using the valid DSA
		 * pointer.
		 *
		 * Make sure that the information belongs to pid we requested
		 * information for, Otherwise loop back and wait for the server
		 * process to finish publishing statistics.
		 */
		LWLockAcquire(&memCxtState[procNumber].lw_lock, LW_EXCLUSIVE);

		/*
		 * Note in procnumber.h file says that a procNumber can be re-used for
		 * a different backend immediately after a backend exits. In case an
		 * old process' data was there and not updated by the current process
		 * in the slot identified by the procNumber, the pid of the requested
		 * process and the proc_id might not match.
		 */
		if (memCxtState[procNumber].proc_id == pid)
		{
			/*
			 * Break if the latest stats have been read, indicated by
			 * statistics timestamp being newer than the current request
			 * timestamp.
			 */
			msecs = TimestampDifferenceMilliseconds(start_timestamp,
													memCxtState[procNumber].stats_timestamp);

			if (DsaPointerIsValid(memCxtState[procNumber].memstats_dsa_pointer)
				&& msecs > 0)
				break;
		}
		LWLockRelease(&memCxtState[procNumber].lw_lock);

		/*
		 * Recheck the state of the backend before sleeping on the condition
		 * variable to ensure the process is still alive.  Only check the
		 * relevant process type based on the earlier PID check.
		 */
		if (proc_is_aux)
			proc = AuxiliaryPidGetProc(pid);
		else
			proc = BackendPidGetProc(pid);

		/*
		 * The process ending during memory context processing is not an
		 * error.
		 */
		if (proc == NULL)
		{
			ereport(WARNING,
					errmsg("PID %d is no longer a PostgreSQL server process",
						   pid));
			PG_RETURN_NULL();
		}

		msecs = TimestampDifferenceMilliseconds(start_timestamp, GetCurrentTimestamp());

		/*
		 * If we haven't already exceeded the timeout value, sleep for the
		 * remainder of the timeout on the condition variable.
		 */
		if (msecs > 0 && msecs < (timeout * 1000))
		{
			/*
			 * Wait for the timeout as defined by the user. If no updated
			 * statistics are available within the allowed time then display
			 * previously published statistics if there are any. If no
			 * previous statistics are available then return NULL.  The timer
			 * is defined in milliseconds since that's what the condition
			 * variable sleep uses.
			 */
			if (ConditionVariableTimedSleep(&memCxtState[procNumber].memcxt_cv,
											((timeout * 1000) - msecs), WAIT_EVENT_MEM_CXT_PUBLISH))
			{
				LWLockAcquire(&memCxtState[procNumber].lw_lock, LW_EXCLUSIVE);
				/* Displaying previously published statistics if available */
				if (DsaPointerIsValid(memCxtState[procNumber].memstats_dsa_pointer))
					break;
				else
				{
					LWLockRelease(&memCxtState[procNumber].lw_lock);
					PG_RETURN_NULL();
				}
			}
		}
		else
		{
			LWLockAcquire(&memCxtState[procNumber].lw_lock, LW_EXCLUSIVE);
			/* Displaying previously published statistics if available */
			if (DsaPointerIsValid(memCxtState[procNumber].memstats_dsa_pointer))
				break;
			else
			{
				LWLockRelease(&memCxtState[procNumber].lw_lock);
				PG_RETURN_NULL();
			}
		}
	}

	/*
	 * We should only reach here with a valid DSA handle, either containing
	 * updated statistics or previously published statistics (identified by
	 * the timestamp.
	 */
	Assert(memCxtArea->memstats_dsa_handle != DSA_HANDLE_INVALID);
	/* Attach to the dsa area if we have not already done so */
	if (MemoryStatsDsaArea == NULL)
	{
		MemoryContext oldcontext = CurrentMemoryContext;

		MemoryContextSwitchTo(TopMemoryContext);
		MemoryStatsDsaArea = dsa_attach(memCxtArea->memstats_dsa_handle);
		MemoryContextSwitchTo(oldcontext);
		dsa_pin_mapping(MemoryStatsDsaArea);
	}

	/*
	 * Backend has finished publishing the stats, project them.
	 */
	memcxt_info = (MemoryStatsEntry *)
		dsa_get_address(MemoryStatsDsaArea, memCxtState[procNumber].memstats_dsa_pointer);

#define PG_GET_PROCESS_MEMORY_CONTEXTS_COLS	12
	for (int i = 0; i < memCxtState[procNumber].total_stats; i++)
	{
		ArrayType  *path_array;
		int			path_length;
		Datum		values[PG_GET_PROCESS_MEMORY_CONTEXTS_COLS];
		bool		nulls[PG_GET_PROCESS_MEMORY_CONTEXTS_COLS];
		char	   *name;
		char	   *ident;
		Datum	   *path_datum = NULL;
		int		   *path_int = NULL;

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		if (DsaPointerIsValid(memcxt_info[i].name))
		{
			name = (char *) dsa_get_address(MemoryStatsDsaArea, memcxt_info[i].name);
			values[0] = CStringGetTextDatum(name);
		}
		else
			nulls[0] = true;

		if (DsaPointerIsValid(memcxt_info[i].ident))
		{
			ident = (char *) dsa_get_address(MemoryStatsDsaArea, memcxt_info[i].ident);
			values[1] = CStringGetTextDatum(ident);
		}
		else
			nulls[1] = true;

		values[2] = CStringGetTextDatum(ContextTypeToString(memcxt_info[i].type));

		path_length = memcxt_info[i].path_length;
		path_datum = (Datum *) palloc(path_length * sizeof(Datum));
		if (DsaPointerIsValid(memcxt_info[i].path))
		{
			path_int = (int *) dsa_get_address(MemoryStatsDsaArea, memcxt_info[i].path);
			for (int j = 0; j < path_length; j++)
				path_datum[j] = Int32GetDatum(path_int[j]);
			path_array = construct_array_builtin(path_datum, path_length, INT4OID);
			values[3] = PointerGetDatum(path_array);
		}
		else
			nulls[3] = true;

		values[4] = Int32GetDatum(memcxt_info[i].levels);
		values[5] = Int64GetDatum(memcxt_info[i].totalspace);
		values[6] = Int64GetDatum(memcxt_info[i].nblocks);
		values[7] = Int64GetDatum(memcxt_info[i].freespace);
		values[8] = Int64GetDatum(memcxt_info[i].freechunks);
		values[9] = Int64GetDatum(memcxt_info[i].totalspace -
								  memcxt_info[i].freespace);
		values[10] = Int32GetDatum(memcxt_info[i].num_agg_stats);
		values[11] = TimestampTzGetDatum(memCxtState[procNumber].stats_timestamp);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}
	LWLockRelease(&memCxtState[procNumber].lw_lock);

	ConditionVariableCancelSleep();

	PG_RETURN_NULL();
}

Size
MemoryContextReportingShmemSize(void)
{
	Size		sz = 0;
	Size		TotalProcs = 0;

	TotalProcs = add_size(TotalProcs, NUM_AUXILIARY_PROCS);
	TotalProcs = add_size(TotalProcs, MaxBackends);
	sz = add_size(sz, mul_size(TotalProcs, sizeof(MemoryStatsBackendState)));

	sz = add_size(sz, sizeof(MemoryStatsCtl));

	return sz;
}

/*
 * Initialize shared memory for displaying memory context statistics
 */
void
MemoryContextReportingShmemInit(void)
{
	bool		found;

	memCxtArea = (MemoryStatsCtl *)
		ShmemInitStruct("MemoryStatsCtl",
						sizeof(MemoryStatsCtl), &found);

	if (!found)
	{
		LWLockInitialize(&memCxtArea->lw_lock, LWTRANCHE_MEMORY_CONTEXT_REPORTING_STATE);
		memCxtArea->memstats_dsa_handle = DSA_HANDLE_INVALID;
	}

	memCxtState = (MemoryStatsBackendState *)
		ShmemInitStruct("MemoryStatsBackendState",
						((MaxBackends + NUM_AUXILIARY_PROCS) * sizeof(MemoryStatsBackendState)),
						&found);

	if (found)
		return;

	for (int i = 0; i < (MaxBackends + NUM_AUXILIARY_PROCS); i++)
	{
		ConditionVariableInit(&memCxtState[i].memcxt_cv);
		LWLockInitialize(&memCxtState[i].lw_lock, LWTRANCHE_MEMORY_CONTEXT_REPORTING_PROC);
		memCxtState[i].memstats_dsa_pointer = InvalidDsaPointer;
	}
}
