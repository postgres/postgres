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
 * Copyright (c) 2001-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_function.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/pgstat_internal.h"
#include "utils/timestamp.h"


/* ----------
 * GUC parameters
 * ----------
 */
int			pgstat_track_functions = TRACK_FUNC_OFF;


/*
 * Indicates if backend has some function stats that it hasn't yet
 * sent to the collector.
 */
bool		have_function_stats = false;

/*
 * Backends store per-function info that's waiting to be sent to the collector
 * in this hash table (indexed by function OID).
 */
static HTAB *pgStatFunctions = NULL;

/*
 * Total time charged to functions so far in the current backend.
 * We use this to help separate "self" and "other" time charges.
 * (We assume this initializes to zero.)
 */
static instr_time total_func_time;


/*
 * Initialize function call usage data.
 * Called by the executor before invoking a function.
 */
void
pgstat_init_function_usage(FunctionCallInfo fcinfo,
						   PgStat_FunctionCallUsage *fcu)
{
	PgStat_BackendFunctionEntry *htabent;
	bool		found;

	if (pgstat_track_functions <= fcinfo->flinfo->fn_stats)
	{
		/* stats not wanted */
		fcu->fs = NULL;
		return;
	}

	if (!pgStatFunctions)
	{
		/* First time through - initialize function stat table */
		HASHCTL		hash_ctl;

		hash_ctl.keysize = sizeof(Oid);
		hash_ctl.entrysize = sizeof(PgStat_BackendFunctionEntry);
		pgStatFunctions = hash_create("Function stat entries",
									  PGSTAT_FUNCTION_HASH_SIZE,
									  &hash_ctl,
									  HASH_ELEM | HASH_BLOBS);
	}

	/* Get the stats entry for this function, create if necessary */
	htabent = hash_search(pgStatFunctions, &fcinfo->flinfo->fn_oid,
						  HASH_ENTER, &found);
	if (!found)
		MemSet(&htabent->f_counts, 0, sizeof(PgStat_FunctionCounts));

	fcu->fs = &htabent->f_counts;

	/* save stats for this function, later used to compensate for recursion */
	fcu->save_f_total_time = htabent->f_counts.f_total_time;

	/* save current backend-wide total time */
	fcu->save_total = total_func_time;

	/* get clock time as of function start */
	INSTR_TIME_SET_CURRENT(fcu->f_start);
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
	instr_time	f_total;
	instr_time	f_others;
	instr_time	f_self;

	/* stats not wanted? */
	if (fs == NULL)
		return;

	/* total elapsed time in this function call */
	INSTR_TIME_SET_CURRENT(f_total);
	INSTR_TIME_SUBTRACT(f_total, fcu->f_start);

	/* self usage: elapsed minus anything already charged to other calls */
	f_others = total_func_time;
	INSTR_TIME_SUBTRACT(f_others, fcu->save_total);
	f_self = f_total;
	INSTR_TIME_SUBTRACT(f_self, f_others);

	/* update backend-wide total time */
	INSTR_TIME_ADD(total_func_time, f_self);

	/*
	 * Compute the new f_total_time as the total elapsed time added to the
	 * pre-call value of f_total_time.  This is necessary to avoid
	 * double-counting any time taken by recursive calls of myself.  (We do
	 * not need any similar kluge for self time, since that already excludes
	 * any recursive calls.)
	 */
	INSTR_TIME_ADD(f_total, fcu->save_f_total_time);

	/* update counters in function stats table */
	if (finalize)
		fs->f_numcalls++;
	fs->f_total_time = f_total;
	INSTR_TIME_ADD(fs->f_self_time, f_self);

	/* indicate that we have something to send */
	have_function_stats = true;
}

/*
 * Subroutine for pgstat_report_stat: populate and send a function stat message
 */
void
pgstat_send_funcstats(void)
{
	/* we assume this inits to all zeroes: */
	static const PgStat_FunctionCounts all_zeroes;

	PgStat_MsgFuncstat msg;
	PgStat_BackendFunctionEntry *entry;
	HASH_SEQ_STATUS fstat;

	if (pgStatFunctions == NULL)
		return;

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_FUNCSTAT);
	msg.m_databaseid = MyDatabaseId;
	msg.m_nentries = 0;

	hash_seq_init(&fstat, pgStatFunctions);
	while ((entry = (PgStat_BackendFunctionEntry *) hash_seq_search(&fstat)) != NULL)
	{
		PgStat_FunctionEntry *m_ent;

		/* Skip it if no counts accumulated since last time */
		if (memcmp(&entry->f_counts, &all_zeroes,
				   sizeof(PgStat_FunctionCounts)) == 0)
			continue;

		/* need to convert format of time accumulators */
		m_ent = &msg.m_entry[msg.m_nentries];
		m_ent->f_id = entry->f_id;
		m_ent->f_numcalls = entry->f_counts.f_numcalls;
		m_ent->f_total_time = INSTR_TIME_GET_MICROSEC(entry->f_counts.f_total_time);
		m_ent->f_self_time = INSTR_TIME_GET_MICROSEC(entry->f_counts.f_self_time);

		if (++msg.m_nentries >= PGSTAT_NUM_FUNCENTRIES)
		{
			pgstat_send(&msg, offsetof(PgStat_MsgFuncstat, m_entry[0]) +
						msg.m_nentries * sizeof(PgStat_FunctionEntry));
			msg.m_nentries = 0;
		}

		/* reset the entry's counts */
		MemSet(&entry->f_counts, 0, sizeof(PgStat_FunctionCounts));
	}

	if (msg.m_nentries > 0)
		pgstat_send(&msg, offsetof(PgStat_MsgFuncstat, m_entry[0]) +
					msg.m_nentries * sizeof(PgStat_FunctionEntry));

	have_function_stats = false;
}

/*
 * find any existing PgStat_BackendFunctionEntry entry for specified function
 *
 * If no entry, return NULL, don't create a new one
 */
PgStat_BackendFunctionEntry *
find_funcstat_entry(Oid func_id)
{
	pgstat_assert_is_up();

	if (pgStatFunctions == NULL)
		return NULL;

	return (PgStat_BackendFunctionEntry *) hash_search(pgStatFunctions,
													   (void *) &func_id,
													   HASH_FIND, NULL);
}
