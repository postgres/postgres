/*-------------------------------------------------------------------------
 *
 * pgstatfuncs.c
 *	  Functions for accessing the statistics collector data
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/pgstatfuncs.c,v 1.17.2.1 2004/10/01 21:09:46 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_shadow.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "pgstat.h"
#include "utils/hsearch.h"

/* bogus ... these externs should be in a header file */
extern Datum pg_stat_get_numscans(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_tuples_returned(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_tuples_fetched(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_tuples_inserted(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_tuples_updated(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_tuples_deleted(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_blocks_fetched(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_blocks_hit(PG_FUNCTION_ARGS);

extern Datum pg_stat_get_backend_idset(PG_FUNCTION_ARGS);
extern Datum pg_backend_pid(PG_FUNCTION_ARGS);
extern Datum pg_stat_reset(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_backend_pid(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_backend_dbid(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_backend_userid(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_backend_activity(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_backend_activity_start(PG_FUNCTION_ARGS);

extern Datum pg_stat_get_db_numbackends(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_db_xact_commit(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_db_xact_rollback(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_db_blocks_fetched(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_db_blocks_hit(PG_FUNCTION_ARGS);


Datum
pg_stat_get_numscans(PG_FUNCTION_ARGS)
{
	PgStat_StatTabEntry *tabentry;
	Oid			relid;
	int64		result;

	relid = PG_GETARG_OID(0);

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->numscans);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_tuples_returned(PG_FUNCTION_ARGS)
{
	PgStat_StatTabEntry *tabentry;
	Oid			relid;
	int64		result;

	relid = PG_GETARG_OID(0);

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->tuples_returned);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_tuples_fetched(PG_FUNCTION_ARGS)
{
	PgStat_StatTabEntry *tabentry;
	Oid			relid;
	int64		result;

	relid = PG_GETARG_OID(0);

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->tuples_fetched);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_tuples_inserted(PG_FUNCTION_ARGS)
{
	PgStat_StatTabEntry *tabentry;
	Oid			relid;
	int64		result;

	relid = PG_GETARG_OID(0);

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->tuples_inserted);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_tuples_updated(PG_FUNCTION_ARGS)
{
	PgStat_StatTabEntry *tabentry;
	Oid			relid;
	int64		result;

	relid = PG_GETARG_OID(0);

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->tuples_updated);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_tuples_deleted(PG_FUNCTION_ARGS)
{
	PgStat_StatTabEntry *tabentry;
	Oid			relid;
	int64		result;

	relid = PG_GETARG_OID(0);

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->tuples_deleted);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_blocks_fetched(PG_FUNCTION_ARGS)
{
	PgStat_StatTabEntry *tabentry;
	Oid			relid;
	int64		result;

	relid = PG_GETARG_OID(0);

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->blocks_fetched);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_blocks_hit(PG_FUNCTION_ARGS)
{
	PgStat_StatTabEntry *tabentry;
	Oid			relid;
	int64		result;

	relid = PG_GETARG_OID(0);

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->blocks_hit);

	PG_RETURN_INT64(result);
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
pg_backend_pid(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(MyProcPid);
}

/*
 * Built-in function for resetting the counters
 */
Datum
pg_stat_reset(PG_FUNCTION_ARGS)
{
	pgstat_reset_counters();

	PG_RETURN_BOOL(true);
}

Datum
pg_stat_get_backend_pid(PG_FUNCTION_ARGS)
{
	PgStat_StatBeEntry *beentry;
	int32		beid;

	beid = PG_GETARG_INT32(0);

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	PG_RETURN_INT32(beentry->procpid);
}


Datum
pg_stat_get_backend_dbid(PG_FUNCTION_ARGS)
{
	PgStat_StatBeEntry *beentry;
	int32		beid;

	beid = PG_GETARG_INT32(0);

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	PG_RETURN_OID(beentry->databaseid);
}


Datum
pg_stat_get_backend_userid(PG_FUNCTION_ARGS)
{
	PgStat_StatBeEntry *beentry;
	int32		beid;

	beid = PG_GETARG_INT32(0);

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	PG_RETURN_INT32(beentry->userid);
}


Datum
pg_stat_get_backend_activity(PG_FUNCTION_ARGS)
{
	PgStat_StatBeEntry *beentry;
	int32		beid;
	int			len;
	text	   *result;

	beid = PG_GETARG_INT32(0);

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	if (!superuser() && beentry->userid != GetUserId())
		PG_RETURN_NULL();

	len = strlen(beentry->activity);
	result = palloc(VARHDRSZ + len);
	VARATT_SIZEP(result) = VARHDRSZ + len;
	memcpy(VARDATA(result), beentry->activity, len);

	PG_RETURN_TEXT_P(result);
}


Datum
pg_stat_get_backend_activity_start(PG_FUNCTION_ARGS)
{
	PgStat_StatBeEntry *beentry;
	int32		beid;
	AbsoluteTime sec;
	int			usec;
	TimestampTz result;

	beid = PG_GETARG_INT32(0);

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	if (!superuser() && beentry->userid != GetUserId())
		PG_RETURN_NULL();

	sec = beentry->activity_start_sec;
	usec = beentry->activity_start_usec;

	/*
	 * No time recorded for start of current query -- this is the case if
	 * the user hasn't enabled query-level stats collection.
	 */
	if (sec == 0 && usec == 0)
		PG_RETURN_NULL();

	result = AbsoluteTimeUsecToTimestampTz(sec, usec);

	PG_RETURN_TIMESTAMPTZ(result);
}


Datum
pg_stat_get_db_numbackends(PG_FUNCTION_ARGS)
{
	PgStat_StatDBEntry *dbentry;
	Oid			dbid;
	int32		result;

	dbid = PG_GETARG_OID(0);

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int32) (dbentry->n_backends);

	PG_RETURN_INT32(result);
}


Datum
pg_stat_get_db_xact_commit(PG_FUNCTION_ARGS)
{
	PgStat_StatDBEntry *dbentry;
	Oid			dbid;
	int64		result;

	dbid = PG_GETARG_OID(0);

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_xact_commit);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_db_xact_rollback(PG_FUNCTION_ARGS)
{
	PgStat_StatDBEntry *dbentry;
	Oid			dbid;
	int64		result;

	dbid = PG_GETARG_OID(0);

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_xact_rollback);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_db_blocks_fetched(PG_FUNCTION_ARGS)
{
	PgStat_StatDBEntry *dbentry;
	Oid			dbid;
	int64		result;

	dbid = PG_GETARG_OID(0);

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_blocks_fetched);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_db_blocks_hit(PG_FUNCTION_ARGS)
{
	PgStat_StatDBEntry *dbentry;
	Oid			dbid;
	int64		result;

	dbid = PG_GETARG_OID(0);

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_blocks_hit);

	PG_RETURN_INT64(result);
}
