#include "postgres.h"

#include "fmgr.h"
#include "miscadmin.h"
#include "utils/hsearch.h"
#include "access/xact.h"
#include "catalog/pg_shadow.h"
#include "nodes/execnodes.h"

#include "pgstat.h"

extern Datum pg_stat_get_numscans(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_tuples_returned(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_tuples_fetched(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_tuples_inserted(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_tuples_updated(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_tuples_deleted(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_blocks_fetched(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_blocks_hit(PG_FUNCTION_ARGS);

extern Datum pg_stat_get_backend_idset(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_backend_pid(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_backend_dbid(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_backend_userid(PG_FUNCTION_ARGS);
extern Datum pg_stat_get_backend_activity(PG_FUNCTION_ARGS);

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
	FmgrInfo   *fmgr_info = fcinfo->flinfo;
	int32		result;

	if (fcinfo->resultinfo == NULL ||
		!IsA(fcinfo->resultinfo, ReturnSetInfo))
		elog(ERROR, "pg_stat_get_backend_idset: called in context that does not accept a set result");

	if (fmgr_info->fn_extra == NULL)
	{
		if (fmgr_info->fn_mcxt == NULL)
			elog(ERROR, "No function memory context in set-function");
		fmgr_info->fn_extra = MemoryContextAlloc(fmgr_info->fn_mcxt,
												 2 * sizeof(int));
		((int *) (fmgr_info->fn_extra))[0] = 0;
		((int *) (fmgr_info->fn_extra))[1] = pgstat_fetch_stat_numbackends();
	}

	((int *) (fmgr_info->fn_extra))[0] += 1;
	result = ((int *) (fmgr_info->fn_extra))[0];

	if (result > ((int *) (fmgr_info->fn_extra))[1])
	{
		pfree(fmgr_info->fn_extra);
		fmgr_info->fn_extra = NULL;
		((ReturnSetInfo *) (fcinfo->resultinfo))->isDone = ExprEndResult;
		PG_RETURN_NULL();
	}

	((ReturnSetInfo *) (fcinfo->resultinfo))->isDone = ExprMultipleResult;
	PG_RETURN_INT32(result);
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

	PG_RETURN_OID(beentry->userid);
}


Datum
pg_stat_get_backend_activity(PG_FUNCTION_ARGS)
{
	PgStat_StatBeEntry *beentry;
	int32		beid;
	int			len;
	text	   *result;

	if (!superuser())
		PG_RETURN_NULL();

	beid = PG_GETARG_INT32(0);

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	len = strlen(beentry->activity);
	result = palloc(VARHDRSZ + len);
	VARATT_SIZEP(result) = VARHDRSZ + len;
	memcpy(VARDATA(result), beentry->activity, len);

	PG_RETURN_TEXT_P(result);
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
