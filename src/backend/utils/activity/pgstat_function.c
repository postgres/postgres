/* -------------------------------------------------------------------------
 *
 * pgstat_function.c
 *	  Implementation of function statistics.
 *
 * This file contains the implementation of function statistics. It is kept
 * separate from pgstat.c to enforce the line between the statistics access /
 * storage implementation and the details about individual types of
 * statistics.
 *
 * Copyright (c) 2001-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_function.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "utils/inval.h"
#include "utils/pgstat_internal.h"
#include "utils/syscache.h"


/* ----------
 * GUC parameters
 * ----------
 */
int			pgstat_track_functions = TRACK_FUNC_OFF;


/*
 * Total time charged to functions so far in the current backend.
 * We use this to help separate "self" and "other" time charges.
 * (We assume this initializes to zero.)
 */
static instr_time total_func_time;


/*
 * Ensure that stats are dropped if transaction aborts.
 */
void
pgstat_create_function(Oid proid)
{
	pgstat_create_transactional(PGSTAT_KIND_FUNCTION,
								MyDatabaseId,
								proid);
}

/*
 * Ensure that stats are dropped if transaction commits.
 *
 * NB: This is only reliable because pgstat_init_function_usage() does some
 * extra work. If other places start emitting function stats they likely need
 * similar logic.
 */
void
pgstat_drop_function(Oid proid)
{
	pgstat_drop_transactional(PGSTAT_KIND_FUNCTION,
							  MyDatabaseId,
							  proid);
}

/*
 * Initialize function call usage data.
 * Called by the executor before invoking a function.
 */
void
pgstat_init_function_usage(FunctionCallInfo fcinfo,
						   PgStat_FunctionCallUsage *fcu)
{
	PgStat_EntryRef *entry_ref;
	PgStat_FunctionCounts *pending;
	bool		created_entry;

	if (pgstat_track_functions <= fcinfo->flinfo->fn_stats)
	{
		/* stats not wanted */
		fcu->fs = NULL;
		return;
	}

	entry_ref = pgstat_prep_pending_entry(PGSTAT_KIND_FUNCTION,
										  MyDatabaseId,
										  fcinfo->flinfo->fn_oid,
										  &created_entry);

	/*
	 * If no shared entry already exists, check if the function has been
	 * deleted concurrently. This can go unnoticed until here because
	 * executing a statement that just calls a function, does not trigger
	 * cache invalidation processing. The reason we care about this case is
	 * that otherwise we could create a new stats entry for an already dropped
	 * function (for relations etc this is not possible because emitting stats
	 * requires a lock for the relation to already have been acquired).
	 *
	 * It's somewhat ugly to have a behavioral difference based on
	 * track_functions being enabled/disabled. But it seems acceptable, given
	 * that there's already behavioral differences depending on whether the
	 * function is the caches etc.
	 *
	 * For correctness it'd be sufficient to set ->dropped to true. However,
	 * the accepted invalidation will commonly cause "low level" failures in
	 * PL code, with an OID in the error message. Making this harder to
	 * test...
	 */
	if (created_entry)
	{
		AcceptInvalidationMessages();
		if (!SearchSysCacheExists1(PROCOID, ObjectIdGetDatum(fcinfo->flinfo->fn_oid)))
		{
			pgstat_drop_entry(PGSTAT_KIND_FUNCTION, MyDatabaseId,
							  fcinfo->flinfo->fn_oid);
			ereport(ERROR, errcode(ERRCODE_UNDEFINED_FUNCTION),
					errmsg("function call to dropped function"));
		}
	}

	pending = entry_ref->pending;

	fcu->fs = pending;

	/* save stats for this function, later used to compensate for recursion */
	fcu->save_f_total_time = pending->total_time;

	/* save current backend-wide total time */
	fcu->save_total = total_func_time;

	/* get clock time as of function start */
	INSTR_TIME_SET_CURRENT(fcu->start);
}

/*
 * Calculate function call usage and update stat counters.
 * Called by the executor after invoking a function.
 *
 * In the case of a set-returning function that runs in value-per-call mode,
 * we will see multiple pgstat_init_function_usage/pgstat_end_function_usage
 * calls for what the user considers a single call of the function.  The
 * finalize flag should be TRUE on the last call.
 */
void
pgstat_end_function_usage(PgStat_FunctionCallUsage *fcu, bool finalize)
{
	PgStat_FunctionCounts *fs = fcu->fs;
	instr_time	total;
	instr_time	others;
	instr_time	self;

	/* stats not wanted? */
	if (fs == NULL)
		return;

	/* total elapsed time in this function call */
	INSTR_TIME_SET_CURRENT(total);
	INSTR_TIME_SUBTRACT(total, fcu->start);

	/* self usage: elapsed minus anything already charged to other calls */
	others = total_func_time;
	INSTR_TIME_SUBTRACT(others, fcu->save_total);
	self = total;
	INSTR_TIME_SUBTRACT(self, others);

	/* update backend-wide total time */
	INSTR_TIME_ADD(total_func_time, self);

	/*
	 * Compute the new total_time as the total elapsed time added to the
	 * pre-call value of total_time.  This is necessary to avoid
	 * double-counting any time taken by recursive calls of myself.  (We do
	 * not need any similar kluge for self time, since that already excludes
	 * any recursive calls.)
	 */
	INSTR_TIME_ADD(total, fcu->save_f_total_time);

	/* update counters in function stats table */
	if (finalize)
		fs->numcalls++;
	fs->total_time = total;
	INSTR_TIME_ADD(fs->self_time, self);
}

/*
 * Flush out pending stats for the entry
 *
 * If nowait is true, this function returns false if lock could not
 * immediately acquired, otherwise true is returned.
 */
bool
pgstat_function_flush_cb(PgStat_EntryRef *entry_ref, bool nowait)
{
	PgStat_FunctionCounts *localent;
	PgStatShared_Function *shfuncent;

	localent = (PgStat_FunctionCounts *) entry_ref->pending;
	shfuncent = (PgStatShared_Function *) entry_ref->shared_stats;

	/* localent always has non-zero content */

	if (!pgstat_lock_entry(entry_ref, nowait))
		return false;

	shfuncent->stats.numcalls += localent->numcalls;
	shfuncent->stats.total_time +=
		INSTR_TIME_GET_MICROSEC(localent->total_time);
	shfuncent->stats.self_time +=
		INSTR_TIME_GET_MICROSEC(localent->self_time);

	pgstat_unlock_entry(entry_ref);

	return true;
}

/*
 * find any existing PgStat_FunctionCounts entry for specified function
 *
 * If no entry, return NULL, don't create a new one
 */
PgStat_FunctionCounts *
find_funcstat_entry(Oid func_id)
{
	PgStat_EntryRef *entry_ref;

	entry_ref = pgstat_fetch_pending_entry(PGSTAT_KIND_FUNCTION, MyDatabaseId, func_id);

	if (entry_ref)
		return entry_ref->pending;
	return NULL;
}

/*
 * Support function for the SQL-callable pgstat* functions. Returns
 * the collected statistics for one function or NULL.
 */
PgStat_StatFuncEntry *
pgstat_fetch_stat_funcentry(Oid func_id)
{
	return (PgStat_StatFuncEntry *)
		pgstat_fetch_entry(PGSTAT_KIND_FUNCTION, MyDatabaseId, func_id);
}
