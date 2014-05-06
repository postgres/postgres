/*-------------------------------------------------------------------------
 *
 * pgstatfuncs.c
 *	  Functions for accessing the statistics collector data
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/pgstatfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "libpq/ip.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/builtins.h"
#include "utils/inet.h"
#include "utils/timestamp.h"

/* bogus ... these externs should be in a header file */
extern Datum pg_stat_get_numscans(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_tuples_returned(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_tuples_fetched(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_tuples_inserted(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_tuples_updated(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_tuples_deleted(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_tuples_hot_updated(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_live_tuples(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_dead_tuples(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_mod_since_analyze(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_blocks_fetched(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_blocks_hit(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_last_vacuum_time(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_last_autovacuum_time(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_last_analyze_time(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_last_autoanalyze_time(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_vacuum_count(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_autovacuum_count(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_analyze_count(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_autoanalyze_count(PG_FUNCTION_ARGS);

extern Datum pg_stat_get_function_calls(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_function_total_time(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_function_self_time(PG_FUNCTION_ARGS);

extern Datum pg_stat_get_backend_idset(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_activity(PG_FUNCTION_ARGS);
extern Datum pg_backend_pid(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_backend_pid(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_backend_dbid(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_backend_userid(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_backend_activity(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_backend_waiting(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_backend_activity_start(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_backend_xact_start(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_backend_start(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_backend_client_addr(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_backend_client_port(PG_FUNCTION_ARGS);

extern Datum pg_stat_get_db_numbackends(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_db_xact_commit(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_db_xact_rollback(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_db_blocks_fetched(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_db_blocks_hit(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_db_tuples_returned(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_db_tuples_fetched(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_db_tuples_inserted(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_db_tuples_updated(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_db_tuples_deleted(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_db_conflict_tablespace(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_db_conflict_lock(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_db_conflict_snapshot(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_db_conflict_bufferpin(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_db_conflict_startup_deadlock(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_db_conflict_all(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_db_deadlocks(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_db_stat_reset_time(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_db_temp_files(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_db_temp_bytes(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_db_blk_read_time(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_db_blk_write_time(PG_FUNCTION_ARGS);

extern Datum pg_stat_get_archiver(PG_FUNCTION_ARGS);

extern Datum pg_stat_get_bgwriter_timed_checkpoints(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_bgwriter_requested_checkpoints(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_checkpoint_write_time(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_checkpoint_sync_time(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_bgwriter_buf_written_checkpoints(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_bgwriter_buf_written_clean(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_bgwriter_maxwritten_clean(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_bgwriter_stat_reset_time(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_buf_written_backend(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_buf_fsync_backend(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_buf_alloc(PG_FUNCTION_ARGS);

extern Datum pg_stat_get_xact_numscans(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_xact_tuples_returned(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_xact_tuples_fetched(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_xact_tuples_inserted(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_xact_tuples_updated(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_xact_tuples_deleted(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_xact_tuples_hot_updated(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_xact_blocks_fetched(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_xact_blocks_hit(PG_FUNCTION_ARGS);

extern Datum pg_stat_get_xact_function_calls(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_xact_function_total_time(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_xact_function_self_time(PG_FUNCTION_ARGS);

extern Datum pg_stat_clear_snapshot(PG_FUNCTION_ARGS);
extern Datum pg_stat_reset(PG_FUNCTION_ARGS);
extern Datum pg_stat_reset_shared(PG_FUNCTION_ARGS);
extern Datum pg_stat_reset_single_table_counters(PG_FUNCTION_ARGS);
extern Datum pg_stat_reset_single_function_counters(PG_FUNCTION_ARGS);

/* Global bgwriter statistics, from bgwriter.c */
extern PgStat_MsgBgWriter bgwriterStats;

Datum
pg_stat_get_numscans(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->numscans);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_tuples_returned(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->tuples_returned);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_tuples_fetched(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->tuples_fetched);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_tuples_inserted(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->tuples_inserted);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_tuples_updated(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->tuples_updated);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_tuples_deleted(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->tuples_deleted);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_tuples_hot_updated(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->tuples_hot_updated);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_live_tuples(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->n_live_tuples);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_dead_tuples(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->n_dead_tuples);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_mod_since_analyze(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->changes_since_analyze);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_blocks_fetched(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->blocks_fetched);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_blocks_hit(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->blocks_hit);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_last_vacuum_time(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	TimestampTz result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = tabentry->vacuum_timestamp;

	if (result == 0)
		PG_RETURN_NULL();
	else
		PG_RETURN_TIMESTAMPTZ(result);
}

Datum
pg_stat_get_last_autovacuum_time(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	TimestampTz result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = tabentry->autovac_vacuum_timestamp;

	if (result == 0)
		PG_RETURN_NULL();
	else
		PG_RETURN_TIMESTAMPTZ(result);
}

Datum
pg_stat_get_last_analyze_time(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	TimestampTz result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = tabentry->analyze_timestamp;

	if (result == 0)
		PG_RETURN_NULL();
	else
		PG_RETURN_TIMESTAMPTZ(result);
}

Datum
pg_stat_get_last_autoanalyze_time(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	TimestampTz result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = tabentry->autovac_analyze_timestamp;

	if (result == 0)
		PG_RETURN_NULL();
	else
		PG_RETURN_TIMESTAMPTZ(result);
}

Datum
pg_stat_get_vacuum_count(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->vacuum_count);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_autovacuum_count(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->autovac_vacuum_count);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_analyze_count(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->analyze_count);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_autoanalyze_count(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->autovac_analyze_count);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_function_calls(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	PgStat_StatFuncEntry *funcentry;

	if ((funcentry = pgstat_fetch_stat_funcentry(funcid)) == NULL)
		PG_RETURN_NULL();
	PG_RETURN_INT64(funcentry->f_numcalls);
}

Datum
pg_stat_get_function_total_time(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	PgStat_StatFuncEntry *funcentry;

	if ((funcentry = pgstat_fetch_stat_funcentry(funcid)) == NULL)
		PG_RETURN_NULL();
	/* convert counter from microsec to millisec for display */
	PG_RETURN_FLOAT8(((double) funcentry->f_total_time) / 1000.0);
}

Datum
pg_stat_get_function_self_time(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	PgStat_StatFuncEntry *funcentry;

	if ((funcentry = pgstat_fetch_stat_funcentry(funcid)) == NULL)
		PG_RETURN_NULL();
	/* convert counter from microsec to millisec for display */
	PG_RETURN_FLOAT8(((double) funcentry->f_self_time) / 1000.0);
}

Datum
pg_stat_get_backend_idset(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	int		   *fctx;
	int32		result;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		fctx = MemoryContextAlloc(funcctx->multi_call_memory_ctx,
								  2 * sizeof(int));
		funcctx->user_fctx = fctx;

		fctx[0] = 0;
		fctx[1] = pgstat_fetch_stat_numbackends();
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();
	fctx = funcctx->user_fctx;

	fctx[0] += 1;
	result = fctx[0];

	if (result <= fctx[1])
	{
		/* do when there is more left to send */
		SRF_RETURN_NEXT(funcctx, Int32GetDatum(result));
	}
	else
	{
		/* do when there is no more left */
		SRF_RETURN_DONE(funcctx);
	}
}

Datum
pg_stat_get_activity(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc	tupdesc;

		funcctx = SRF_FIRSTCALL_INIT();

		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		tupdesc = CreateTemplateTupleDesc(16, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "datid",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "pid",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "usesysid",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "application_name",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 5, "state",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 6, "query",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 7, "waiting",
						   BOOLOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 8, "act_start",
						   TIMESTAMPTZOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 9, "query_start",
						   TIMESTAMPTZOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 10, "backend_start",
						   TIMESTAMPTZOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 11, "state_change",
						   TIMESTAMPTZOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 12, "client_addr",
						   INETOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 13, "client_hostname",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 14, "client_port",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 15, "backend_xid",
						   XIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 16, "backend_xmin",
						   XIDOID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		funcctx->user_fctx = palloc0(sizeof(int));
		if (PG_ARGISNULL(0))
		{
			/* Get all backends */
			funcctx->max_calls = pgstat_fetch_stat_numbackends();
		}
		else
		{
			/*
			 * Get one backend - locate by pid.
			 *
			 * We lookup the backend early, so we can return zero rows if it
			 * doesn't exist, instead of returning a single row full of NULLs.
			 */
			int			pid = PG_GETARG_INT32(0);
			int			i;
			int			n = pgstat_fetch_stat_numbackends();

			for (i = 1; i <= n; i++)
			{
				PgBackendStatus *be = pgstat_fetch_stat_beentry(i);

				if (be)
				{
					if (be->st_procpid == pid)
					{
						*(int *) (funcctx->user_fctx) = i;
						break;
					}
				}
			}

			if (*(int *) (funcctx->user_fctx) == 0)
				/* Pid not found, return zero rows */
				funcctx->max_calls = 0;
			else
				funcctx->max_calls = 1;
		}

		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		/* for each row */
		Datum		values[16];
		bool		nulls[16];
		HeapTuple	tuple;
		LocalPgBackendStatus *local_beentry;
		PgBackendStatus *beentry;

		MemSet(values, 0, sizeof(values));
		MemSet(nulls, 0, sizeof(nulls));

		if (*(int *) (funcctx->user_fctx) > 0)
		{
			/* Get specific pid slot */
			local_beentry = pgstat_fetch_stat_local_beentry(*(int *) (funcctx->user_fctx));
			beentry = &local_beentry->backendStatus;
		}
		else
		{
			/* Get the next one in the list */
			local_beentry = pgstat_fetch_stat_local_beentry(funcctx->call_cntr + 1);	/* 1-based index */
			beentry = &local_beentry->backendStatus;
		}
		if (!beentry)
		{
			int			i;

			for (i = 0; i < sizeof(nulls) / sizeof(nulls[0]); i++)
				nulls[i] = true;

			nulls[5] = false;
			values[5] = CStringGetTextDatum("<backend information not available>");

			tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
			SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
		}

		/* Values available to all callers */
		values[0] = ObjectIdGetDatum(beentry->st_databaseid);
		values[1] = Int32GetDatum(beentry->st_procpid);
		values[2] = ObjectIdGetDatum(beentry->st_userid);
		if (beentry->st_appname)
			values[3] = CStringGetTextDatum(beentry->st_appname);
		else
			nulls[3] = true;

		if (TransactionIdIsValid(local_beentry->backend_xid))
			values[14] = TransactionIdGetDatum(local_beentry->backend_xid);
		else
			nulls[14] = true;

		if (TransactionIdIsValid(local_beentry->backend_xmin))
			values[15] = TransactionIdGetDatum(local_beentry->backend_xmin);
		else
			nulls[15] = true;

		/* Values only available to same user or superuser */
		if (superuser() || beentry->st_userid == GetUserId())
		{
			SockAddr	zero_clientaddr;

			switch (beentry->st_state)
			{
				case STATE_IDLE:
					values[4] = CStringGetTextDatum("idle");
					break;
				case STATE_RUNNING:
					values[4] = CStringGetTextDatum("active");
					break;
				case STATE_IDLEINTRANSACTION:
					values[4] = CStringGetTextDatum("idle in transaction");
					break;
				case STATE_FASTPATH:
					values[4] = CStringGetTextDatum("fastpath function call");
					break;
				case STATE_IDLEINTRANSACTION_ABORTED:
					values[4] = CStringGetTextDatum("idle in transaction (aborted)");
					break;
				case STATE_DISABLED:
					values[4] = CStringGetTextDatum("disabled");
					break;
				case STATE_UNDEFINED:
					nulls[4] = true;
					break;
			}

			values[5] = CStringGetTextDatum(beentry->st_activity);
			values[6] = BoolGetDatum(beentry->st_waiting);

			if (beentry->st_xact_start_timestamp != 0)
				values[7] = TimestampTzGetDatum(beentry->st_xact_start_timestamp);
			else
				nulls[7] = true;

			if (beentry->st_activity_start_timestamp != 0)
				values[8] = TimestampTzGetDatum(beentry->st_activity_start_timestamp);
			else
				nulls[8] = true;

			if (beentry->st_proc_start_timestamp != 0)
				values[9] = TimestampTzGetDatum(beentry->st_proc_start_timestamp);
			else
				nulls[9] = true;

			if (beentry->st_state_start_timestamp != 0)
				values[10] = TimestampTzGetDatum(beentry->st_state_start_timestamp);
			else
				nulls[10] = true;

			/* A zeroed client addr means we don't know */
			memset(&zero_clientaddr, 0, sizeof(zero_clientaddr));
			if (memcmp(&(beentry->st_clientaddr), &zero_clientaddr,
					   sizeof(zero_clientaddr)) == 0)
			{
				nulls[11] = true;
				nulls[12] = true;
				nulls[13] = true;
			}
			else
			{
				if (beentry->st_clientaddr.addr.ss_family == AF_INET
#ifdef HAVE_IPV6
					|| beentry->st_clientaddr.addr.ss_family == AF_INET6
#endif
					)
				{
					char		remote_host[NI_MAXHOST];
					char		remote_port[NI_MAXSERV];
					int			ret;

					remote_host[0] = '\0';
					remote_port[0] = '\0';
					ret = pg_getnameinfo_all(&beentry->st_clientaddr.addr,
											 beentry->st_clientaddr.salen,
											 remote_host, sizeof(remote_host),
											 remote_port, sizeof(remote_port),
											 NI_NUMERICHOST | NI_NUMERICSERV);
					if (ret == 0)
					{
						clean_ipv6_addr(beentry->st_clientaddr.addr.ss_family, remote_host);
						values[11] = DirectFunctionCall1(inet_in,
											   CStringGetDatum(remote_host));
						if (beentry->st_clienthostname &&
							beentry->st_clienthostname[0])
							values[12] = CStringGetTextDatum(beentry->st_clienthostname);
						else
							nulls[12] = true;
						values[13] = Int32GetDatum(atoi(remote_port));
					}
					else
					{
						nulls[11] = true;
						nulls[12] = true;
						nulls[13] = true;
					}
				}
				else if (beentry->st_clientaddr.addr.ss_family == AF_UNIX)
				{
					/*
					 * Unix sockets always reports NULL for host and -1 for
					 * port, so it's possible to tell the difference to
					 * connections we have no permissions to view, or with
					 * errors.
					 */
					nulls[11] = true;
					nulls[12] = true;
					values[13] = DatumGetInt32(-1);
				}
				else
				{
					/* Unknown address type, should never happen */
					nulls[11] = true;
					nulls[12] = true;
					nulls[13] = true;
				}
			}
		}
		else
		{
			/* No permissions to view data about this session */
			values[5] = CStringGetTextDatum("<insufficient privilege>");
			nulls[4] = true;
			nulls[6] = true;
			nulls[7] = true;
			nulls[8] = true;
			nulls[9] = true;
			nulls[10] = true;
			nulls[11] = true;
			nulls[12] = true;
			nulls[13] = true;
		}

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);

		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
	else
	{
		/* nothing left */
		SRF_RETURN_DONE(funcctx);
	}
}


Datum
pg_backend_pid(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(MyProcPid);
}


Datum
pg_stat_get_backend_pid(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	PG_RETURN_INT32(beentry->st_procpid);
}


Datum
pg_stat_get_backend_dbid(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	PG_RETURN_OID(beentry->st_databaseid);
}


Datum
pg_stat_get_backend_userid(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	PG_RETURN_OID(beentry->st_userid);
}


Datum
pg_stat_get_backend_activity(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;
	const char *activity;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		activity = "<backend information not available>";
	else if (!superuser() && beentry->st_userid != GetUserId())
		activity = "<insufficient privilege>";
	else if (*(beentry->st_activity) == '\0')
		activity = "<command string not enabled>";
	else
		activity = beentry->st_activity;

	PG_RETURN_TEXT_P(cstring_to_text(activity));
}


Datum
pg_stat_get_backend_waiting(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	bool		result;
	PgBackendStatus *beentry;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	if (!superuser() && beentry->st_userid != GetUserId())
		PG_RETURN_NULL();

	result = beentry->st_waiting;

	PG_RETURN_BOOL(result);
}


Datum
pg_stat_get_backend_activity_start(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	TimestampTz result;
	PgBackendStatus *beentry;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	if (!superuser() && beentry->st_userid != GetUserId())
		PG_RETURN_NULL();

	result = beentry->st_activity_start_timestamp;

	/*
	 * No time recorded for start of current query -- this is the case if the
	 * user hasn't enabled query-level stats collection.
	 */
	if (result == 0)
		PG_RETURN_NULL();

	PG_RETURN_TIMESTAMPTZ(result);
}


Datum
pg_stat_get_backend_xact_start(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	TimestampTz result;
	PgBackendStatus *beentry;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	if (!superuser() && beentry->st_userid != GetUserId())
		PG_RETURN_NULL();

	result = beentry->st_xact_start_timestamp;

	if (result == 0)			/* not in a transaction */
		PG_RETURN_NULL();

	PG_RETURN_TIMESTAMPTZ(result);
}


Datum
pg_stat_get_backend_start(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	TimestampTz result;
	PgBackendStatus *beentry;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	if (!superuser() && beentry->st_userid != GetUserId())
		PG_RETURN_NULL();

	result = beentry->st_proc_start_timestamp;

	if (result == 0)			/* probably can't happen? */
		PG_RETURN_NULL();

	PG_RETURN_TIMESTAMPTZ(result);
}


Datum
pg_stat_get_backend_client_addr(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;
	SockAddr	zero_clientaddr;
	char		remote_host[NI_MAXHOST];
	int			ret;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	if (!superuser() && beentry->st_userid != GetUserId())
		PG_RETURN_NULL();

	/* A zeroed client addr means we don't know */
	memset(&zero_clientaddr, 0, sizeof(zero_clientaddr));
	if (memcmp(&(beentry->st_clientaddr), &zero_clientaddr,
			   sizeof(zero_clientaddr)) == 0)
		PG_RETURN_NULL();

	switch (beentry->st_clientaddr.addr.ss_family)
	{
		case AF_INET:
#ifdef HAVE_IPV6
		case AF_INET6:
#endif
			break;
		default:
			PG_RETURN_NULL();
	}

	remote_host[0] = '\0';
	ret = pg_getnameinfo_all(&beentry->st_clientaddr.addr,
							 beentry->st_clientaddr.salen,
							 remote_host, sizeof(remote_host),
							 NULL, 0,
							 NI_NUMERICHOST | NI_NUMERICSERV);
	if (ret != 0)
		PG_RETURN_NULL();

	clean_ipv6_addr(beentry->st_clientaddr.addr.ss_family, remote_host);

	PG_RETURN_INET_P(DirectFunctionCall1(inet_in,
										 CStringGetDatum(remote_host)));
}

Datum
pg_stat_get_backend_client_port(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;
	SockAddr	zero_clientaddr;
	char		remote_port[NI_MAXSERV];
	int			ret;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	if (!superuser() && beentry->st_userid != GetUserId())
		PG_RETURN_NULL();

	/* A zeroed client addr means we don't know */
	memset(&zero_clientaddr, 0, sizeof(zero_clientaddr));
	if (memcmp(&(beentry->st_clientaddr), &zero_clientaddr,
			   sizeof(zero_clientaddr)) == 0)
		PG_RETURN_NULL();

	switch (beentry->st_clientaddr.addr.ss_family)
	{
		case AF_INET:
#ifdef HAVE_IPV6
		case AF_INET6:
#endif
			break;
		case AF_UNIX:
			PG_RETURN_INT32(-1);
		default:
			PG_RETURN_NULL();
	}

	remote_port[0] = '\0';
	ret = pg_getnameinfo_all(&beentry->st_clientaddr.addr,
							 beentry->st_clientaddr.salen,
							 NULL, 0,
							 remote_port, sizeof(remote_port),
							 NI_NUMERICHOST | NI_NUMERICSERV);
	if (ret != 0)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(DirectFunctionCall1(int4in,
										CStringGetDatum(remote_port)));
}


Datum
pg_stat_get_db_numbackends(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int32		result;
	int			tot_backends = pgstat_fetch_stat_numbackends();
	int			beid;

	result = 0;
	for (beid = 1; beid <= tot_backends; beid++)
	{
		PgBackendStatus *beentry = pgstat_fetch_stat_beentry(beid);

		if (beentry && beentry->st_databaseid == dbid)
			result++;
	}

	PG_RETURN_INT32(result);
}


Datum
pg_stat_get_db_xact_commit(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_xact_commit);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_db_xact_rollback(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_xact_rollback);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_db_blocks_fetched(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_blocks_fetched);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_db_blocks_hit(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_blocks_hit);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_db_tuples_returned(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_tuples_returned);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_db_tuples_fetched(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_tuples_fetched);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_db_tuples_inserted(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_tuples_inserted);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_db_tuples_updated(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_tuples_updated);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_db_tuples_deleted(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_tuples_deleted);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_db_stat_reset_time(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	TimestampTz result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = dbentry->stat_reset_timestamp;

	if (result == 0)
		PG_RETURN_NULL();
	else
		PG_RETURN_TIMESTAMPTZ(result);
}

Datum
pg_stat_get_db_temp_files(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = dbentry->n_temp_files;

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_db_temp_bytes(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = dbentry->n_temp_bytes;

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_db_conflict_tablespace(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_conflict_tablespace);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_db_conflict_lock(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_conflict_lock);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_db_conflict_snapshot(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_conflict_snapshot);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_db_conflict_bufferpin(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_conflict_bufferpin);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_db_conflict_startup_deadlock(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_conflict_startup_deadlock);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_db_conflict_all(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (
						  dbentry->n_conflict_tablespace +
						  dbentry->n_conflict_lock +
						  dbentry->n_conflict_snapshot +
						  dbentry->n_conflict_bufferpin +
						  dbentry->n_conflict_startup_deadlock);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_db_deadlocks(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_deadlocks);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_db_blk_read_time(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	double		result;
	PgStat_StatDBEntry *dbentry;

	/* convert counter from microsec to millisec for display */
	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = ((double) dbentry->n_block_read_time) / 1000.0;

	PG_RETURN_FLOAT8(result);
}

Datum
pg_stat_get_db_blk_write_time(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	double		result;
	PgStat_StatDBEntry *dbentry;

	/* convert counter from microsec to millisec for display */
	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = ((double) dbentry->n_block_write_time) / 1000.0;

	PG_RETURN_FLOAT8(result);
}

Datum
pg_stat_get_bgwriter_timed_checkpoints(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(pgstat_fetch_global()->timed_checkpoints);
}

Datum
pg_stat_get_bgwriter_requested_checkpoints(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(pgstat_fetch_global()->requested_checkpoints);
}

Datum
pg_stat_get_bgwriter_buf_written_checkpoints(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(pgstat_fetch_global()->buf_written_checkpoints);
}

Datum
pg_stat_get_bgwriter_buf_written_clean(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(pgstat_fetch_global()->buf_written_clean);
}

Datum
pg_stat_get_bgwriter_maxwritten_clean(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(pgstat_fetch_global()->maxwritten_clean);
}

Datum
pg_stat_get_checkpoint_write_time(PG_FUNCTION_ARGS)
{
	/* time is already in msec, just convert to double for presentation */
	PG_RETURN_FLOAT8((double) pgstat_fetch_global()->checkpoint_write_time);
}

Datum
pg_stat_get_checkpoint_sync_time(PG_FUNCTION_ARGS)
{
	/* time is already in msec, just convert to double for presentation */
	PG_RETURN_FLOAT8((double) pgstat_fetch_global()->checkpoint_sync_time);
}

Datum
pg_stat_get_bgwriter_stat_reset_time(PG_FUNCTION_ARGS)
{
	PG_RETURN_TIMESTAMPTZ(pgstat_fetch_global()->stat_reset_timestamp);
}

Datum
pg_stat_get_buf_written_backend(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(pgstat_fetch_global()->buf_written_backend);
}

Datum
pg_stat_get_buf_fsync_backend(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(pgstat_fetch_global()->buf_fsync_backend);
}

Datum
pg_stat_get_buf_alloc(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(pgstat_fetch_global()->buf_alloc);
}

Datum
pg_stat_get_xact_numscans(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_TableStatus *tabentry;

	if ((tabentry = find_tabstat_entry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->t_counts.t_numscans);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_xact_tuples_returned(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_TableStatus *tabentry;

	if ((tabentry = find_tabstat_entry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->t_counts.t_tuples_returned);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_xact_tuples_fetched(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_TableStatus *tabentry;

	if ((tabentry = find_tabstat_entry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->t_counts.t_tuples_fetched);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_xact_tuples_inserted(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_TableStatus *tabentry;
	PgStat_TableXactStatus *trans;

	if ((tabentry = find_tabstat_entry(relid)) == NULL)
		result = 0;
	else
	{
		result = tabentry->t_counts.t_tuples_inserted;
		/* live subtransactions' counts aren't in t_tuples_inserted yet */
		for (trans = tabentry->trans; trans != NULL; trans = trans->upper)
			result += trans->tuples_inserted;
	}

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_xact_tuples_updated(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_TableStatus *tabentry;
	PgStat_TableXactStatus *trans;

	if ((tabentry = find_tabstat_entry(relid)) == NULL)
		result = 0;
	else
	{
		result = tabentry->t_counts.t_tuples_updated;
		/* live subtransactions' counts aren't in t_tuples_updated yet */
		for (trans = tabentry->trans; trans != NULL; trans = trans->upper)
			result += trans->tuples_updated;
	}

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_xact_tuples_deleted(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_TableStatus *tabentry;
	PgStat_TableXactStatus *trans;

	if ((tabentry = find_tabstat_entry(relid)) == NULL)
		result = 0;
	else
	{
		result = tabentry->t_counts.t_tuples_deleted;
		/* live subtransactions' counts aren't in t_tuples_deleted yet */
		for (trans = tabentry->trans; trans != NULL; trans = trans->upper)
			result += trans->tuples_deleted;
	}

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_xact_tuples_hot_updated(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_TableStatus *tabentry;

	if ((tabentry = find_tabstat_entry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->t_counts.t_tuples_hot_updated);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_xact_blocks_fetched(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_TableStatus *tabentry;

	if ((tabentry = find_tabstat_entry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->t_counts.t_blocks_fetched);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_xact_blocks_hit(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_TableStatus *tabentry;

	if ((tabentry = find_tabstat_entry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->t_counts.t_blocks_hit);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_xact_function_calls(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	PgStat_BackendFunctionEntry *funcentry;

	if ((funcentry = find_funcstat_entry(funcid)) == NULL)
		PG_RETURN_NULL();
	PG_RETURN_INT64(funcentry->f_counts.f_numcalls);
}

Datum
pg_stat_get_xact_function_total_time(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	PgStat_BackendFunctionEntry *funcentry;

	if ((funcentry = find_funcstat_entry(funcid)) == NULL)
		PG_RETURN_NULL();
	PG_RETURN_FLOAT8(INSTR_TIME_GET_MILLISEC(funcentry->f_counts.f_total_time));
}

Datum
pg_stat_get_xact_function_self_time(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	PgStat_BackendFunctionEntry *funcentry;

	if ((funcentry = find_funcstat_entry(funcid)) == NULL)
		PG_RETURN_NULL();
	PG_RETURN_FLOAT8(INSTR_TIME_GET_MILLISEC(funcentry->f_counts.f_self_time));
}


/* Discard the active statistics snapshot */
Datum
pg_stat_clear_snapshot(PG_FUNCTION_ARGS)
{
	pgstat_clear_snapshot();

	PG_RETURN_VOID();
}


/* Reset all counters for the current database */
Datum
pg_stat_reset(PG_FUNCTION_ARGS)
{
	pgstat_reset_counters();

	PG_RETURN_VOID();
}

/* Reset some shared cluster-wide counters */
Datum
pg_stat_reset_shared(PG_FUNCTION_ARGS)
{
	char	   *target = text_to_cstring(PG_GETARG_TEXT_PP(0));

	pgstat_reset_shared_counters(target);

	PG_RETURN_VOID();
}

/* Reset a a single counter in the current database */
Datum
pg_stat_reset_single_table_counters(PG_FUNCTION_ARGS)
{
	Oid			taboid = PG_GETARG_OID(0);

	pgstat_reset_single_counter(taboid, RESET_TABLE);

	PG_RETURN_VOID();
}

Datum
pg_stat_reset_single_function_counters(PG_FUNCTION_ARGS)
{
	Oid			funcoid = PG_GETARG_OID(0);

	pgstat_reset_single_counter(funcoid, RESET_FUNCTION);

	PG_RETURN_VOID();
}

Datum
pg_stat_get_archiver(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[7];
	bool		nulls[7];
	PgStat_ArchiverStats *archiver_stats;

	/* Initialise values and NULL flags arrays */
	MemSet(values, 0, sizeof(values));
	MemSet(nulls, 0, sizeof(nulls));

	/* Initialise attributes information in the tuple descriptor */
	tupdesc = CreateTemplateTupleDesc(7, false);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "archived_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "last_archived_wal",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "last_archived_time",
					   TIMESTAMPTZOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "failed_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "last_failed_wal",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 6, "last_failed_time",
					   TIMESTAMPTZOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 7, "stats_reset",
					   TIMESTAMPTZOID, -1, 0);

	BlessTupleDesc(tupdesc);

	/* Get statistics about the archiver process */
	archiver_stats = pgstat_fetch_stat_archiver();

	/* Fill values and NULLs */
	values[0] = Int64GetDatum(archiver_stats->archived_count);
	if (*(archiver_stats->last_archived_wal) == '\0')
		nulls[1] = true;
	else
		values[1] = CStringGetTextDatum(archiver_stats->last_archived_wal);

	if (archiver_stats->last_archived_timestamp == 0)
		nulls[2] = true;
	else
		values[2] = TimestampTzGetDatum(archiver_stats->last_archived_timestamp);

	values[3] = Int64GetDatum(archiver_stats->failed_count);
	if (*(archiver_stats->last_failed_wal) == '\0')
		nulls[4] = true;
	else
		values[4] = CStringGetTextDatum(archiver_stats->last_failed_wal);

	if (archiver_stats->last_failed_timestamp == 0)
		nulls[5] = true;
	else
		values[5] = TimestampTzGetDatum(archiver_stats->last_failed_timestamp);

	if (archiver_stats->stat_reset_timestamp == 0)
		nulls[6] = true;
	else
		values[6] = TimestampTzGetDatum(archiver_stats->stat_reset_timestamp);

	/* Returns the record as Datum */
	PG_RETURN_DATUM(HeapTupleGetDatum(
								   heap_form_tuple(tupdesc, values, nulls)));
}
