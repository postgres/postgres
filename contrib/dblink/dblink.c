/*
 * dblink.c
 *
 * Functions returning results from a remote database
 *
 * Joe Conway <mail@joeconway.com>
 *
 * Copyright (c) 2001, 2002 by PostgreSQL Global Development Group
 * ALL RIGHTS RESERVED;
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without a written agreement
 * is hereby granted, provided that the above copyright notice and this
 * paragraph and the following two paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTORS BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE AUTHOR OR DISTRIBUTORS HAVE BEEN ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * THE AUTHOR AND DISTRIBUTORS SPECIFICALLY DISCLAIMS ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE AUTHOR AND DISTRIBUTORS HAS NO OBLIGATIONS TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 */
#include "postgres.h"

#include <ctype.h>

#include "libpq-fe.h"

#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "access/tupdesc.h"
#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/namespace.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "nodes/nodes.h"
#include "nodes/execnodes.h"
#include "nodes/pg_list.h"
#include "parser/parse_type.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "dblink.h"

/*
 * Internal declarations
 */
static dblink_results *init_dblink_results(MemoryContext fn_mcxt);
static char **get_pkey_attnames(Oid relid, int16 *numatts);
static char *get_sql_insert(Oid relid, int16 *pkattnums, int16 pknumatts, char **src_pkattvals, char **tgt_pkattvals);
static char *get_sql_delete(Oid relid, int16 *pkattnums, int16 pknumatts, char **tgt_pkattvals);
static char *get_sql_update(Oid relid, int16 *pkattnums, int16 pknumatts, char **src_pkattvals, char **tgt_pkattvals);
static char *quote_literal_cstr(char *rawstr);
static char *quote_ident_cstr(char *rawstr);
static int16 get_attnum_pk_pos(int16 *pkattnums, int16 pknumatts, int16 key);
static HeapTuple get_tuple_of_interest(Oid relid, int16 *pkattnums, int16 pknumatts, char **src_pkattvals);
static Oid	get_relid_from_relname(text *relname_text);
static dblink_results *get_res_ptr(int32 res_id_index);
static void append_res_ptr(dblink_results * results);
static void remove_res_ptr(dblink_results * results);
static char *generate_relation_name(Oid relid);
static char *connstr_strip_password(const char *connstr);
static void dblink_security_check(PGconn *conn, const char *connstr);
static int get_nondropped_natts(Oid relid);

/* Global */
List	   *res_id = NIL;
int			res_id_index = 0;
PGconn	   *persistent_conn = NULL;

#define GET_TEXT(cstrp) DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(cstrp)))
#define GET_STR(textp) DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(textp)))
#define xpfree(var_) \
	do { \
		if (var_ != NULL) \
		{ \
			pfree(var_); \
			var_ = NULL; \
		} \
	} while (0)


/*
 * Create a persistent connection to another database
 */
PG_FUNCTION_INFO_V1(dblink_connect);
Datum
dblink_connect(PG_FUNCTION_ARGS)
{
	char	   *connstr = GET_STR(PG_GETARG_TEXT_P(0));
	char	   *msg;
	text	   *result_text;
	MemoryContext oldcontext;

	if (persistent_conn != NULL)
		PQfinish(persistent_conn);

	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	/* check password used if not superuser */
	dblink_security_check(persistent_conn, connstr);
	persistent_conn = PQconnectdb(connstr);

	MemoryContextSwitchTo(oldcontext);

	if (PQstatus(persistent_conn) == CONNECTION_BAD)
	{
		msg = pstrdup(PQerrorMessage(persistent_conn));
		PQfinish(persistent_conn);
		persistent_conn = NULL;
		elog(ERROR, "dblink_connect: connection error: %s", msg);
	}

	result_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum("OK")));
	PG_RETURN_TEXT_P(result_text);
}

/*
 * Clear a persistent connection to another database
 */
PG_FUNCTION_INFO_V1(dblink_disconnect);
Datum
dblink_disconnect(PG_FUNCTION_ARGS)
{
	text	   *result_text;

	if (persistent_conn != NULL)
		PQfinish(persistent_conn);

	persistent_conn = NULL;

	result_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum("OK")));
	PG_RETURN_TEXT_P(result_text);
}

/*
 * opens a cursor using a persistent connection
 */
PG_FUNCTION_INFO_V1(dblink_open);
Datum
dblink_open(PG_FUNCTION_ARGS)
{
	char	   *msg;
	PGresult   *res = NULL;
	PGconn	   *conn = NULL;
	text	   *result_text;
	char	   *curname = GET_STR(PG_GETARG_TEXT_P(0));
	char	   *sql = GET_STR(PG_GETARG_TEXT_P(1));
	StringInfo	str = makeStringInfo();

	if (persistent_conn != NULL)
		conn = persistent_conn;
	else
		elog(ERROR, "dblink_open: no connection available");

	res = PQexec(conn, "BEGIN");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		msg = pstrdup(PQerrorMessage(conn));
		PQclear(res);

		PQfinish(conn);
		persistent_conn = NULL;

		elog(ERROR, "dblink_open: begin error: %s", msg);
	}
	PQclear(res);

	appendStringInfo(str, "DECLARE %s CURSOR FOR %s", curname, sql);
	res = PQexec(conn, str->data);
	if (!res ||
		(PQresultStatus(res) != PGRES_COMMAND_OK &&
		 PQresultStatus(res) != PGRES_TUPLES_OK))
	{
		msg = pstrdup(PQerrorMessage(conn));

		PQclear(res);

		PQfinish(conn);
		persistent_conn = NULL;

		elog(ERROR, "dblink: sql error: %s", msg);
	}

	result_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum("OK")));
	PG_RETURN_TEXT_P(result_text);
}

/*
 * closes a cursor
 */
PG_FUNCTION_INFO_V1(dblink_close);
Datum
dblink_close(PG_FUNCTION_ARGS)
{
	PGconn	   *conn = NULL;
	PGresult   *res = NULL;
	char	   *curname = GET_STR(PG_GETARG_TEXT_P(0));
	StringInfo	str = makeStringInfo();
	text	   *result_text;
	char	   *msg;

	if (persistent_conn != NULL)
		conn = persistent_conn;
	else
		elog(ERROR, "dblink_close: no connection available");

	appendStringInfo(str, "CLOSE %s", curname);

	/* close the cursor */
	res = PQexec(conn, str->data);
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		msg = pstrdup(PQerrorMessage(conn));
		PQclear(res);

		PQfinish(persistent_conn);
		persistent_conn = NULL;

		elog(ERROR, "dblink_close: sql error: %s", msg);
	}

	PQclear(res);

	/* commit the transaction */
	res = PQexec(conn, "COMMIT");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		msg = pstrdup(PQerrorMessage(conn));
		PQclear(res);

		PQfinish(persistent_conn);
		persistent_conn = NULL;

		elog(ERROR, "dblink_close: commit error: %s", msg);
	}
	PQclear(res);

	result_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum("OK")));
	PG_RETURN_TEXT_P(result_text);
}

/*
 * Fetch results from an open cursor
 */
PG_FUNCTION_INFO_V1(dblink_fetch);
Datum
dblink_fetch(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	TupleDesc	tupdesc = NULL;
	int			call_cntr;
	int			max_calls;
	TupleTableSlot *slot;
	AttInMetadata *attinmeta;
	char	   *msg;
	PGresult   *res = NULL;
	MemoryContext oldcontext;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		Oid			functypeid;
		char		functyptype;
		Oid			funcid = fcinfo->flinfo->fn_oid;
		PGconn	   *conn = NULL;
		StringInfo	str = makeStringInfo();
		char	   *curname = GET_STR(PG_GETARG_TEXT_P(0));
		int			howmany = PG_GETARG_INT32(1);
		ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * switch to memory context appropriate for multiple function
		 * calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (persistent_conn != NULL)
			conn = persistent_conn;
		else
			elog(ERROR, "dblink_fetch: no connection available");

		appendStringInfo(str, "FETCH %d FROM %s", howmany, curname);

		res = PQexec(conn, str->data);
		if (!res ||
			(PQresultStatus(res) != PGRES_COMMAND_OK &&
			 PQresultStatus(res) != PGRES_TUPLES_OK))
		{
			msg = pstrdup(PQerrorMessage(conn));
			PQclear(res);

			PQfinish(persistent_conn);
			persistent_conn = NULL;

			elog(ERROR, "dblink_fetch: sql error: %s", msg);
		}
		else if (PQresultStatus(res) == PGRES_COMMAND_OK)
		{
			/* cursor does not exist - closed already or bad name */
			PQclear(res);
			elog(ERROR, "dblink_fetch: cursor %s does not exist", curname);
		}

		funcctx->max_calls = PQntuples(res);

		/* got results, keep track of them */
		funcctx->user_fctx = res;

		/* check typtype to see if we have a predetermined return type */
		functypeid = get_func_rettype(funcid);
		functyptype = get_typtype(functypeid);

		if (functyptype == 'c')
			tupdesc = TypeGetTupleDesc(functypeid, NIL);
		else if (functyptype == 'p' && functypeid == RECORDOID)
		{
			if (!rsinfo || !IsA(rsinfo, ReturnSetInfo))
				elog(ERROR, "function returning record called in context "
							"that cannot accept type record");

			/* get the requested return tuple description */
			tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);
		}
		else if (functyptype == 'b')
			elog(ERROR, "dblink_fetch: invalid kind of return type specified for function");
		else
			elog(ERROR, "dblink_fetch: unknown kind of return type specified for function");

		/* store needed metadata for subsequent calls */
		slot = TupleDescGetSlot(tupdesc);
		funcctx->slot = slot;

		/* check result and tuple descriptor have the same number of columns */
		if (PQnfields(res) != tupdesc->natts)
			elog(ERROR, "remote query result rowtype does not match "
						"the specified FROM clause rowtype");

		/* fast track when no results */
		if (funcctx->max_calls < 1)
		{
			if (res)
				PQclear(res);
			SRF_RETURN_DONE(funcctx);
		}

		attinmeta = TupleDescGetAttInMetadata(tupdesc);
		funcctx->attinmeta = attinmeta;

		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	/*
	 * initialize per-call variables
	 */
	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;

	slot = funcctx->slot;

	res = (PGresult *) funcctx->user_fctx;
	attinmeta = funcctx->attinmeta;
	tupdesc = attinmeta->tupdesc;

	if (call_cntr < max_calls)	/* do when there is more left to send */
	{
		char	  **values;
		HeapTuple	tuple;
		Datum		result;
		int			i;
		int			nfields = PQnfields(res);

		values = (char **) palloc(nfields * sizeof(char *));
		for (i = 0; i < nfields; i++)
		{
			if (PQgetisnull(res, call_cntr, i) == 0)
				values[i] = PQgetvalue(res, call_cntr, i);
			else
				values[i] = NULL;
		}

		/* build the tuple */
		tuple = BuildTupleFromCStrings(attinmeta, values);

		/* make the tuple into a datum */
		result = TupleGetDatum(slot, tuple);

		SRF_RETURN_NEXT(funcctx, result);
	}
	else
/* do when there is no more left */
	{
		PQclear(res);
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * Note: this is the new preferred version of dblink
 */
PG_FUNCTION_INFO_V1(dblink_record);
Datum
dblink_record(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	TupleDesc	tupdesc = NULL;
	int			call_cntr;
	int			max_calls;
	TupleTableSlot *slot;
	AttInMetadata *attinmeta;
	char	   *msg;
	PGresult   *res = NULL;
	bool		is_sql_cmd = false;
	char	   *sql_cmd_status = NULL;
	MemoryContext oldcontext;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		Oid			functypeid;
		char		functyptype;
		Oid			funcid = fcinfo->flinfo->fn_oid;
		PGconn	   *conn = NULL;
		char	   *connstr = NULL;
		char	   *sql = NULL;
		ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * switch to memory context appropriate for multiple function
		 * calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (fcinfo->nargs == 2)
		{
			connstr = GET_STR(PG_GETARG_TEXT_P(0));
			sql = GET_STR(PG_GETARG_TEXT_P(1));

			/* check password used if not superuser */
			dblink_security_check(conn, connstr);
			conn = PQconnectdb(connstr);
			if (PQstatus(conn) == CONNECTION_BAD)
			{
				msg = pstrdup(PQerrorMessage(conn));
				PQfinish(conn);
				elog(ERROR, "dblink: connection error: %s", msg);
			}
		}
		else if (fcinfo->nargs == 1)
		{
			sql = GET_STR(PG_GETARG_TEXT_P(0));

			if (persistent_conn != NULL)
				conn = persistent_conn;
			else
				elog(ERROR, "dblink: no connection available");
		}
		else
			elog(ERROR, "dblink: wrong number of arguments");

		res = PQexec(conn, sql);
		if (!res || (PQresultStatus(res) != PGRES_COMMAND_OK && PQresultStatus(res) != PGRES_TUPLES_OK))
		{
			msg = pstrdup(PQerrorMessage(conn));
			PQclear(res);
			PQfinish(conn);
			if (fcinfo->nargs == 1)
				persistent_conn = NULL;

			elog(ERROR, "dblink: sql error: %s", msg);
		}
		else
		{
			if (PQresultStatus(res) == PGRES_COMMAND_OK)
			{
				is_sql_cmd = true;

				/* need a tuple descriptor representing one TEXT column */
				tupdesc = CreateTemplateTupleDesc(1, false);
				TupleDescInitEntry(tupdesc, (AttrNumber) 1, "status",
								   TEXTOID, -1, 0, false);

				/*
				 * and save a copy of the command status string to return
				 * as our result tuple
				 */
				sql_cmd_status = PQcmdStatus(res);
				funcctx->max_calls = 1;
			}
			else
				funcctx->max_calls = PQntuples(res);

			/* got results, keep track of them */
			funcctx->user_fctx = res;

			/* if needed, close the connection to the database and cleanup */
			if (fcinfo->nargs == 2)
				PQfinish(conn);
		}

		/* check typtype to see if we have a predetermined return type */
		functypeid = get_func_rettype(funcid);
		functyptype = get_typtype(functypeid);

		if (!is_sql_cmd)
		{
			if (functyptype == 'c')
				tupdesc = TypeGetTupleDesc(functypeid, NIL);
			else if (functyptype == 'p' && functypeid == RECORDOID)
			{
				if (!rsinfo || !IsA(rsinfo, ReturnSetInfo))
					elog(ERROR, "function returning record called in context "
								"that cannot accept type record");

				/* get the requested return tuple description */
				tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);
			}
			else if (functyptype == 'b')
				elog(ERROR, "Invalid kind of return type specified for function");
			else
				elog(ERROR, "Unknown kind of return type specified for function");
		}

		/* store needed metadata for subsequent calls */
		slot = TupleDescGetSlot(tupdesc);
		funcctx->slot = slot;

		/* check result and tuple descriptor have the same number of columns */
		if (PQnfields(res) != tupdesc->natts)
			elog(ERROR, "remote query result rowtype does not match "
						"the specified FROM clause rowtype");

		/* fast track when no results */
		if (funcctx->max_calls < 1)
		{
			if (res)
				PQclear(res);
			SRF_RETURN_DONE(funcctx);
		}

		attinmeta = TupleDescGetAttInMetadata(tupdesc);
		funcctx->attinmeta = attinmeta;

		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	/*
	 * initialize per-call variables
	 */
	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;

	slot = funcctx->slot;

	res = (PGresult *) funcctx->user_fctx;
	attinmeta = funcctx->attinmeta;
	tupdesc = attinmeta->tupdesc;

	if (call_cntr < max_calls)	/* do when there is more left to send */
	{
		char	  **values;
		HeapTuple	tuple;
		Datum		result;

		if (!is_sql_cmd)
		{
			int			i;
			int			nfields = PQnfields(res);

			values = (char **) palloc(nfields * sizeof(char *));
			for (i = 0; i < nfields; i++)
			{
				if (PQgetisnull(res, call_cntr, i) == 0)
					values[i] = PQgetvalue(res, call_cntr, i);
				else
					values[i] = NULL;
			}
		}
		else
		{
			values = (char **) palloc(1 * sizeof(char *));
			values[0] = sql_cmd_status;
		}

		/* build the tuple */
		tuple = BuildTupleFromCStrings(attinmeta, values);

		/* make the tuple into a datum */
		result = TupleGetDatum(slot, tuple);

		SRF_RETURN_NEXT(funcctx, result);
	}
	else
/* do when there is no more left */
	{
		PQclear(res);
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * Execute an SQL non-SELECT command
 */
PG_FUNCTION_INFO_V1(dblink_exec);
Datum
dblink_exec(PG_FUNCTION_ARGS)
{
	char	   *msg;
	PGresult   *res = NULL;
	char	   *sql_cmd_status = NULL;
	TupleDesc	tupdesc = NULL;
	text	   *result_text;
	PGconn	   *conn = NULL;
	char	   *connstr = NULL;
	char	   *sql = NULL;

	if (fcinfo->nargs == 2)
	{
		connstr = GET_STR(PG_GETARG_TEXT_P(0));
		sql = GET_STR(PG_GETARG_TEXT_P(1));

		/* check password used if not superuser */
		dblink_security_check(conn, connstr);
		conn = PQconnectdb(connstr);
		if (PQstatus(conn) == CONNECTION_BAD)
		{
			msg = pstrdup(PQerrorMessage(conn));
			PQfinish(conn);
			elog(ERROR, "dblink_exec: connection error: %s", msg);
		}
	}
	else if (fcinfo->nargs == 1)
	{
		sql = GET_STR(PG_GETARG_TEXT_P(0));

		if (persistent_conn != NULL)
			conn = persistent_conn;
		else
			elog(ERROR, "dblink_exec: no connection available");
	}
	else
		elog(ERROR, "dblink_exec: wrong number of arguments");


	res = PQexec(conn, sql);
	if (!res || (PQresultStatus(res) != PGRES_COMMAND_OK && PQresultStatus(res) != PGRES_TUPLES_OK))
	{
		msg = pstrdup(PQerrorMessage(conn));
		PQclear(res);
		PQfinish(conn);
		if (fcinfo->nargs == 1)
			persistent_conn = NULL;

		elog(ERROR, "dblink_exec: sql error: %s", msg);
	}
	else
	{
		if (PQresultStatus(res) == PGRES_COMMAND_OK)
		{
			/* need a tuple descriptor representing one TEXT column */
			tupdesc = CreateTemplateTupleDesc(1, false);
			TupleDescInitEntry(tupdesc, (AttrNumber) 1, "status",
							   TEXTOID, -1, 0, false);

			/*
			 * and save a copy of the command status string to return as
			 * our result tuple
			 */
			sql_cmd_status = PQcmdStatus(res);
		}
		else
			elog(ERROR, "dblink_exec: queries returning results not allowed");
	}
	PQclear(res);

	/* if needed, close the connection to the database and cleanup */
	if (fcinfo->nargs == 2)
		PQfinish(conn);

	result_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(sql_cmd_status)));
	PG_RETURN_TEXT_P(result_text);
}

/*
 * Note: this original version of dblink is DEPRECATED;
 * it *will* be removed in favor of the new version on next release
 */
PG_FUNCTION_INFO_V1(dblink);
Datum
dblink(PG_FUNCTION_ARGS)
{
	PGconn	   *conn = NULL;
	PGresult   *res = NULL;
	dblink_results *results;
	char	   *optstr;
	char	   *sqlstatement;
	char	   *execstatement;
	char	   *msg;
	int			ntuples = 0;
	ReturnSetInfo *rsi;

	if (fcinfo->resultinfo == NULL || !IsA(fcinfo->resultinfo, ReturnSetInfo))
		elog(ERROR, "dblink: function called in context that does not accept a set result");

	optstr = DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(PG_GETARG_TEXT_P(0))));
	sqlstatement = DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(PG_GETARG_TEXT_P(1))));

	if (fcinfo->flinfo->fn_extra == NULL)
	{
		/* check password used if not superuser */
		dblink_security_check(conn, optstr);
		conn = PQconnectdb(optstr);
		if (PQstatus(conn) == CONNECTION_BAD)
		{
			msg = pstrdup(PQerrorMessage(conn));
			PQfinish(conn);
			elog(ERROR, "dblink: connection error: %s", msg);
		}

		execstatement = (char *) palloc(strlen(sqlstatement) + 1);
		if (execstatement != NULL)
		{
			strcpy(execstatement, sqlstatement);
			strcat(execstatement, "\0");
		}
		else
			elog(ERROR, "dblink: insufficient memory");

		res = PQexec(conn, execstatement);
		if (!res || (PQresultStatus(res) != PGRES_COMMAND_OK && PQresultStatus(res) != PGRES_TUPLES_OK))
		{
			msg = pstrdup(PQerrorMessage(conn));
			PQclear(res);
			PQfinish(conn);
			elog(ERROR, "dblink: sql error: %s", msg);
		}
		else
		{
			/*
			 * got results, start fetching them
			 */
			ntuples = PQntuples(res);

			/*
			 * increment resource index
			 */
			res_id_index++;

			results = init_dblink_results(fcinfo->flinfo->fn_mcxt);
			results->tup_num = 0;
			results->res_id_index = res_id_index;
			results->res = res;

			/*
			 * Append node to res_id to hold pointer to results. Needed by
			 * dblink_tok to access the data
			 */
			append_res_ptr(results);

			/*
			 * save pointer to results for the next function manager call
			 */
			fcinfo->flinfo->fn_extra = (void *) results;

			/* close the connection to the database and cleanup */
			PQfinish(conn);

			rsi = (ReturnSetInfo *) fcinfo->resultinfo;
			rsi->isDone = ExprMultipleResult;

			PG_RETURN_INT32(res_id_index);
		}
	}
	else
	{
		/*
		 * check for more results
		 */
		results = fcinfo->flinfo->fn_extra;

		results->tup_num++;
		res_id_index = results->res_id_index;
		ntuples = PQntuples(results->res);

		if (results->tup_num < ntuples)
		{
			/*
			 * fetch them if available
			 */

			rsi = (ReturnSetInfo *) fcinfo->resultinfo;
			rsi->isDone = ExprMultipleResult;

			PG_RETURN_INT32(res_id_index);
		}
		else
		{
			/*
			 * or if no more, clean things up
			 */
			results = fcinfo->flinfo->fn_extra;

			remove_res_ptr(results);
			PQclear(results->res);
			pfree(results);
			fcinfo->flinfo->fn_extra = NULL;

			rsi = (ReturnSetInfo *) fcinfo->resultinfo;
			rsi->isDone = ExprEndResult;

			PG_RETURN_NULL();
		}
	}
	PG_RETURN_NULL();
}

/*
 * Note: dblink_tok is DEPRECATED;
 * it *will* be removed in favor of the new version on next release
 *
 * dblink_tok
 * parse dblink output string
 * return fldnum item (0 based)
 * based on provided field separator
 */
PG_FUNCTION_INFO_V1(dblink_tok);
Datum
dblink_tok(PG_FUNCTION_ARGS)
{
	dblink_results *results;
	int			fldnum;
	text	   *result_text;
	char	   *result;
	int			nfields = 0;
	int			text_len = 0;

	results = get_res_ptr(PG_GETARG_INT32(0));
	if (results == NULL)
	{
		if (res_id != NIL)
		{
			freeList(res_id);
			res_id = NIL;
			res_id_index = 0;
		}

		elog(ERROR, "dblink_tok: function called with invalid resource id");
	}

	fldnum = PG_GETARG_INT32(1);
	if (fldnum < 0)
		elog(ERROR, "dblink_tok: field number < 0 not permitted");

	nfields = PQnfields(results->res);
	if (fldnum > (nfields - 1))
		elog(ERROR, "dblink_tok: field number %d does not exist", fldnum);

	if (PQgetisnull(results->res, results->tup_num, fldnum) == 1)
		PG_RETURN_NULL();
	else
	{
		text_len = PQgetlength(results->res, results->tup_num, fldnum);

		result = (char *) palloc(text_len + 1);

		if (result != NULL)
		{
			strcpy(result, PQgetvalue(results->res, results->tup_num, fldnum));
			strcat(result, "\0");
		}
		else
			elog(ERROR, "dblink: insufficient memory");

		result_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(result)));

		PG_RETURN_TEXT_P(result_text);
	}
}

/*
 * dblink_get_pkey
 *
 * Return list of primary key fields for the supplied relation,
 * or NULL if none exists.
 */
PG_FUNCTION_INFO_V1(dblink_get_pkey);
Datum
dblink_get_pkey(PG_FUNCTION_ARGS)
{
	int16		numatts;
	Oid			relid;
	char	  **results;
	FuncCallContext *funcctx;
	int32		call_cntr;
	int32		max_calls;
	TupleTableSlot *slot;
	AttInMetadata *attinmeta;
	MemoryContext oldcontext;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc = NULL;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * switch to memory context appropriate for multiple function
		 * calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* convert relname to rel Oid */
		relid = get_relid_from_relname(PG_GETARG_TEXT_P(0));
		if (!OidIsValid(relid))
			elog(ERROR, "dblink_get_pkey: relation does not exist");

		/*
		 * need a tuple descriptor representing one INT and one TEXT
		 * column
		 */
		tupdesc = CreateTemplateTupleDesc(2, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "position",
						   INT4OID, -1, 0, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "colname",
						   TEXTOID, -1, 0, false);

		/* allocate a slot for a tuple with this tupdesc */
		slot = TupleDescGetSlot(tupdesc);

		/* assign slot to function context */
		funcctx->slot = slot;

		/*
		 * Generate attribute metadata needed later to produce tuples from
		 * raw C strings
		 */
		attinmeta = TupleDescGetAttInMetadata(tupdesc);
		funcctx->attinmeta = attinmeta;

		/* get an array of attnums */
		results = get_pkey_attnames(relid, &numatts);

		if ((results != NULL) && (numatts > 0))
		{
			funcctx->max_calls = numatts;

			/* got results, keep track of them */
			funcctx->user_fctx = results;
		}
		else
/* fast track when no results */
			SRF_RETURN_DONE(funcctx);

		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	/*
	 * initialize per-call variables
	 */
	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;

	slot = funcctx->slot;

	results = (char **) funcctx->user_fctx;
	attinmeta = funcctx->attinmeta;

	if (call_cntr < max_calls)	/* do when there is more left to send */
	{
		char	  **values;
		HeapTuple	tuple;
		Datum		result;

		values = (char **) palloc(2 * sizeof(char *));
		values[0] = (char *) palloc(12);		/* sign, 10 digits, '\0' */

		sprintf(values[0], "%d", call_cntr + 1);

		values[1] = results[call_cntr];

		/* build the tuple */
		tuple = BuildTupleFromCStrings(attinmeta, values);

		/* make the tuple into a datum */
		result = TupleGetDatum(slot, tuple);

		SRF_RETURN_NEXT(funcctx, result);
	}
	else
/* do when there is no more left */
		SRF_RETURN_DONE(funcctx);
}

/*
 * Note: dblink_last_oid is DEPRECATED;
 * it *will* be removed on next release
 *
 * dblink_last_oid
 * return last inserted oid
 */
PG_FUNCTION_INFO_V1(dblink_last_oid);
Datum
dblink_last_oid(PG_FUNCTION_ARGS)
{
	dblink_results *results;

	results = get_res_ptr(PG_GETARG_INT32(0));
	if (results == NULL)
	{
		if (res_id != NIL)
		{
			freeList(res_id);
			res_id = NIL;
			res_id_index = 0;
		}

		elog(ERROR, "dblink_tok: function called with invalid resource id");
	}

	PG_RETURN_OID(PQoidValue(results->res));
}


#ifndef SHRT_MAX
#define SHRT_MAX (0x7FFF)
#endif
/*
 * dblink_build_sql_insert
 *
 * Used to generate an SQL insert statement
 * based on an existing tuple in a local relation.
 * This is useful for selectively replicating data
 * to another server via dblink.
 *
 * API:
 * <relname> - name of local table of interest
 * <pkattnums> - an int2vector of attnums which will be used
 * to identify the local tuple of interest
 * <pknumatts> - number of attnums in pkattnums
 * <src_pkattvals_arry> - text array of key values which will be used
 * to identify the local tuple of interest
 * <tgt_pkattvals_arry> - text array of key values which will be used
 * to build the string for execution remotely. These are substituted
 * for their counterparts in src_pkattvals_arry
 */
PG_FUNCTION_INFO_V1(dblink_build_sql_insert);
Datum
dblink_build_sql_insert(PG_FUNCTION_ARGS)
{
	Oid			relid;
	text	   *relname_text;
	int16	   *pkattnums;
	int			pknumatts_tmp;
	int16		pknumatts = 0;
	char	  **src_pkattvals;
	char	  **tgt_pkattvals;
	ArrayType  *src_pkattvals_arry;
	ArrayType  *tgt_pkattvals_arry;
	int			src_ndim;
	int		   *src_dim;
	int			src_nitems;
	int			tgt_ndim;
	int		   *tgt_dim;
	int			tgt_nitems;
	int			i;
	char	   *ptr;
	char	   *sql;
	text	   *sql_text;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	int			nondropped_natts;

	relname_text = PG_GETARG_TEXT_P(0);

	/*
	 * Convert relname to rel OID.
	 */
	relid = get_relid_from_relname(relname_text);
	if (!OidIsValid(relid))
		elog(ERROR, "dblink_build_sql_insert: relation does not exist");

	pkattnums = (int16 *) PG_GETARG_POINTER(1);
	pknumatts_tmp = PG_GETARG_INT32(2);
	if (pknumatts_tmp <= SHRT_MAX)
		pknumatts = pknumatts_tmp;
	else
		elog(ERROR, "Bad input value for pknumatts; too large");

	/*
	 * There should be at least one key attribute
	 */
	if (pknumatts == 0)
		elog(ERROR, "dblink_build_sql_insert: number of key attributes must be > 0.");

	/*
	 * ensure we don't ask for more pk attributes than we have
	 * non-dropped columns
	 */
	nondropped_natts = get_nondropped_natts(relid);
	if (pknumatts > nondropped_natts)
		elog(ERROR, "number of primary key fields exceeds number of specified relation attributes");

	src_pkattvals_arry = PG_GETARG_ARRAYTYPE_P(3);
	tgt_pkattvals_arry = PG_GETARG_ARRAYTYPE_P(4);

	/*
	 * Source array is made up of key values that will be used to locate
	 * the tuple of interest from the local system.
	 */
	src_ndim = ARR_NDIM(src_pkattvals_arry);
	src_dim = ARR_DIMS(src_pkattvals_arry);
	src_nitems = ArrayGetNItems(src_ndim, src_dim);

	/*
	 * There should be one source array key value for each key attnum
	 */
	if (src_nitems != pknumatts)
		elog(ERROR, "dblink_build_sql_insert: source key array length does not match number of key attributes.");

	/*
	 * get array of pointers to c-strings from the input source array
	 */
	Assert(ARR_ELEMTYPE(src_pkattvals_arry) == TEXTOID);
	get_typlenbyvalalign(ARR_ELEMTYPE(src_pkattvals_arry),
						 &typlen, &typbyval, &typalign);

	src_pkattvals = (char **) palloc(src_nitems * sizeof(char *));
	ptr = ARR_DATA_PTR(src_pkattvals_arry);
	for (i = 0; i < src_nitems; i++)
	{
		src_pkattvals[i] = DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(ptr)));
		ptr = att_addlength(ptr, typlen, PointerGetDatum(ptr));
		ptr = (char *) att_align(ptr, typalign);
	}

	/*
	 * Target array is made up of key values that will be used to build
	 * the SQL string for use on the remote system.
	 */
	tgt_ndim = ARR_NDIM(tgt_pkattvals_arry);
	tgt_dim = ARR_DIMS(tgt_pkattvals_arry);
	tgt_nitems = ArrayGetNItems(tgt_ndim, tgt_dim);

	/*
	 * There should be one target array key value for each key attnum
	 */
	if (tgt_nitems != pknumatts)
		elog(ERROR, "dblink_build_sql_insert: target key array length does not match number of key attributes.");

	/*
	 * get array of pointers to c-strings from the input target array
	 */
	Assert(ARR_ELEMTYPE(tgt_pkattvals_arry) == TEXTOID);
	get_typlenbyvalalign(ARR_ELEMTYPE(tgt_pkattvals_arry),
						 &typlen, &typbyval, &typalign);

	tgt_pkattvals = (char **) palloc(tgt_nitems * sizeof(char *));
	ptr = ARR_DATA_PTR(tgt_pkattvals_arry);
	for (i = 0; i < tgt_nitems; i++)
	{
		tgt_pkattvals[i] = DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(ptr)));
		ptr = att_addlength(ptr, typlen, PointerGetDatum(ptr));
		ptr = (char *) att_align(ptr, typalign);
	}

	/*
	 * Prep work is finally done. Go get the SQL string.
	 */
	sql = get_sql_insert(relid, pkattnums, pknumatts, src_pkattvals, tgt_pkattvals);

	/*
	 * Make it into TEXT for return to the client
	 */
	sql_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(sql)));

	/*
	 * And send it
	 */
	PG_RETURN_TEXT_P(sql_text);
}


/*
 * dblink_build_sql_delete
 *
 * Used to generate an SQL delete statement.
 * This is useful for selectively replicating a
 * delete to another server via dblink.
 *
 * API:
 * <relname> - name of remote table of interest
 * <pkattnums> - an int2vector of attnums which will be used
 * to identify the remote tuple of interest
 * <pknumatts> - number of attnums in pkattnums
 * <tgt_pkattvals_arry> - text array of key values which will be used
 * to build the string for execution remotely.
 */
PG_FUNCTION_INFO_V1(dblink_build_sql_delete);
Datum
dblink_build_sql_delete(PG_FUNCTION_ARGS)
{
	Oid			relid;
	text	   *relname_text;
	int16	   *pkattnums;
	int			pknumatts_tmp;
	int16		pknumatts = 0;
	char	  **tgt_pkattvals;
	ArrayType  *tgt_pkattvals_arry;
	int			tgt_ndim;
	int		   *tgt_dim;
	int			tgt_nitems;
	int			i;
	char	   *ptr;
	char	   *sql;
	text	   *sql_text;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	int			nondropped_natts;

	relname_text = PG_GETARG_TEXT_P(0);

	/*
	 * Convert relname to rel OID.
	 */
	relid = get_relid_from_relname(relname_text);
	if (!OidIsValid(relid))
		elog(ERROR, "dblink_build_sql_delete: relation does not exist");

	pkattnums = (int16 *) PG_GETARG_POINTER(1);
	pknumatts_tmp = PG_GETARG_INT32(2);
	if (pknumatts_tmp <= SHRT_MAX)
		pknumatts = pknumatts_tmp;
	else
		elog(ERROR, "Bad input value for pknumatts; too large");

	/*
	 * There should be at least one key attribute
	 */
	if (pknumatts == 0)
		elog(ERROR, "dblink_build_sql_insert: number of key attributes must be > 0.");

	/*
	 * ensure we don't ask for more pk attributes than we have
	 * non-dropped columns
	 */
	nondropped_natts = get_nondropped_natts(relid);
	if (pknumatts > nondropped_natts)
		elog(ERROR, "number of primary key fields exceeds number of specified relation attributes");

	tgt_pkattvals_arry = PG_GETARG_ARRAYTYPE_P(3);

	/*
	 * Target array is made up of key values that will be used to build
	 * the SQL string for use on the remote system.
	 */
	tgt_ndim = ARR_NDIM(tgt_pkattvals_arry);
	tgt_dim = ARR_DIMS(tgt_pkattvals_arry);
	tgt_nitems = ArrayGetNItems(tgt_ndim, tgt_dim);

	/*
	 * There should be one target array key value for each key attnum
	 */
	if (tgt_nitems != pknumatts)
		elog(ERROR, "dblink_build_sql_insert: target key array length does not match number of key attributes.");

	/*
	 * get array of pointers to c-strings from the input target array
	 */
	Assert(ARR_ELEMTYPE(tgt_pkattvals_arry) == TEXTOID);
	get_typlenbyvalalign(ARR_ELEMTYPE(tgt_pkattvals_arry),
						 &typlen, &typbyval, &typalign);

	tgt_pkattvals = (char **) palloc(tgt_nitems * sizeof(char *));
	ptr = ARR_DATA_PTR(tgt_pkattvals_arry);
	for (i = 0; i < tgt_nitems; i++)
	{
		tgt_pkattvals[i] = DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(ptr)));
		ptr = att_addlength(ptr, typlen, PointerGetDatum(ptr));
		ptr = (char *) att_align(ptr, typalign);
	}

	/*
	 * Prep work is finally done. Go get the SQL string.
	 */
	sql = get_sql_delete(relid, pkattnums, pknumatts, tgt_pkattvals);

	/*
	 * Make it into TEXT for return to the client
	 */
	sql_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(sql)));

	/*
	 * And send it
	 */
	PG_RETURN_TEXT_P(sql_text);
}


/*
 * dblink_build_sql_update
 *
 * Used to generate an SQL update statement
 * based on an existing tuple in a local relation.
 * This is useful for selectively replicating data
 * to another server via dblink.
 *
 * API:
 * <relname> - name of local table of interest
 * <pkattnums> - an int2vector of attnums which will be used
 * to identify the local tuple of interest
 * <pknumatts> - number of attnums in pkattnums
 * <src_pkattvals_arry> - text array of key values which will be used
 * to identify the local tuple of interest
 * <tgt_pkattvals_arry> - text array of key values which will be used
 * to build the string for execution remotely. These are substituted
 * for their counterparts in src_pkattvals_arry
 */
PG_FUNCTION_INFO_V1(dblink_build_sql_update);
Datum
dblink_build_sql_update(PG_FUNCTION_ARGS)
{
	Oid			relid;
	text	   *relname_text;
	int16	   *pkattnums;
	int			pknumatts_tmp;
	int16		pknumatts = 0;
	char	  **src_pkattvals;
	char	  **tgt_pkattvals;
	ArrayType  *src_pkattvals_arry;
	ArrayType  *tgt_pkattvals_arry;
	int			src_ndim;
	int		   *src_dim;
	int			src_nitems;
	int			tgt_ndim;
	int		   *tgt_dim;
	int			tgt_nitems;
	int			i;
	char	   *ptr;
	char	   *sql;
	text	   *sql_text;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	int			nondropped_natts;

	relname_text = PG_GETARG_TEXT_P(0);

	/*
	 * Convert relname to rel OID.
	 */
	relid = get_relid_from_relname(relname_text);
	if (!OidIsValid(relid))
		elog(ERROR, "dblink_build_sql_update: relation does not exist");

	pkattnums = (int16 *) PG_GETARG_POINTER(1);
	pknumatts_tmp = PG_GETARG_INT32(2);
	if (pknumatts_tmp <= SHRT_MAX)
		pknumatts = pknumatts_tmp;
	else
		elog(ERROR, "Bad input value for pknumatts; too large");

	/*
	 * There should be one source array key values for each key attnum
	 */
	if (pknumatts == 0)
		elog(ERROR, "dblink_build_sql_insert: number of key attributes must be > 0.");

	/*
	 * ensure we don't ask for more pk attributes than we have
	 * non-dropped columns
	 */
	nondropped_natts = get_nondropped_natts(relid);
	if (pknumatts > nondropped_natts)
		elog(ERROR, "number of primary key fields exceeds number of specified relation attributes");

	src_pkattvals_arry = PG_GETARG_ARRAYTYPE_P(3);
	tgt_pkattvals_arry = PG_GETARG_ARRAYTYPE_P(4);

	/*
	 * Source array is made up of key values that will be used to locate
	 * the tuple of interest from the local system.
	 */
	src_ndim = ARR_NDIM(src_pkattvals_arry);
	src_dim = ARR_DIMS(src_pkattvals_arry);
	src_nitems = ArrayGetNItems(src_ndim, src_dim);

	/*
	 * There should be one source array key value for each key attnum
	 */
	if (src_nitems != pknumatts)
		elog(ERROR, "dblink_build_sql_insert: source key array length does not match number of key attributes.");

	/*
	 * get array of pointers to c-strings from the input source array
	 */
	Assert(ARR_ELEMTYPE(src_pkattvals_arry) == TEXTOID);
	get_typlenbyvalalign(ARR_ELEMTYPE(src_pkattvals_arry),
						 &typlen, &typbyval, &typalign);

	src_pkattvals = (char **) palloc(src_nitems * sizeof(char *));
	ptr = ARR_DATA_PTR(src_pkattvals_arry);
	for (i = 0; i < src_nitems; i++)
	{
		src_pkattvals[i] = DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(ptr)));
		ptr = att_addlength(ptr, typlen, PointerGetDatum(ptr));
		ptr = (char *) att_align(ptr, typalign);
	}

	/*
	 * Target array is made up of key values that will be used to build
	 * the SQL string for use on the remote system.
	 */
	tgt_ndim = ARR_NDIM(tgt_pkattvals_arry);
	tgt_dim = ARR_DIMS(tgt_pkattvals_arry);
	tgt_nitems = ArrayGetNItems(tgt_ndim, tgt_dim);

	/*
	 * There should be one target array key value for each key attnum
	 */
	if (tgt_nitems != pknumatts)
		elog(ERROR, "dblink_build_sql_insert: target key array length does not match number of key attributes.");

	/*
	 * get array of pointers to c-strings from the input target array
	 */
	Assert(ARR_ELEMTYPE(tgt_pkattvals_arry) == TEXTOID);
	get_typlenbyvalalign(ARR_ELEMTYPE(tgt_pkattvals_arry),
						 &typlen, &typbyval, &typalign);

	tgt_pkattvals = (char **) palloc(tgt_nitems * sizeof(char *));
	ptr = ARR_DATA_PTR(tgt_pkattvals_arry);
	for (i = 0; i < tgt_nitems; i++)
	{
		tgt_pkattvals[i] = DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(ptr)));
		ptr = att_addlength(ptr, typlen, PointerGetDatum(ptr));
		ptr = (char *) att_align(ptr, typalign);
	}

	/*
	 * Prep work is finally done. Go get the SQL string.
	 */
	sql = get_sql_update(relid, pkattnums, pknumatts, src_pkattvals, tgt_pkattvals);

	/*
	 * Make it into TEXT for return to the client
	 */
	sql_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(sql)));

	/*
	 * And send it
	 */
	PG_RETURN_TEXT_P(sql_text);
}

/*
 * dblink_current_query
 * return the current query string
 * to allow its use in (among other things)
 * rewrite rules
 */
PG_FUNCTION_INFO_V1(dblink_current_query);
Datum
dblink_current_query(PG_FUNCTION_ARGS)
{
	text	   *result_text;

	result_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(debug_query_string)));
	PG_RETURN_TEXT_P(result_text);
}


/*************************************************************
 * internal functions
 */


/*
 * init_dblink_results
 *	 - create an empty dblink_results data structure
 */
static dblink_results *
init_dblink_results(MemoryContext fn_mcxt)
{
	MemoryContext oldcontext;
	dblink_results *retval;

	oldcontext = MemoryContextSwitchTo(fn_mcxt);

	retval = (dblink_results *) palloc(sizeof(dblink_results));
	MemSet(retval, 0, sizeof(dblink_results));

	retval->tup_num = -1;
	retval->res_id_index = -1;
	retval->res = NULL;

	MemoryContextSwitchTo(oldcontext);

	return retval;
}

/*
 * get_pkey_attnames
 *
 * Get the primary key attnames for the given relation.
 * Return NULL, and set numatts = 0, if no primary key exists.
 */
static char **
get_pkey_attnames(Oid relid, int16 *numatts)
{
	Relation	indexRelation;
	ScanKeyData entry;
	HeapScanDesc scan;
	HeapTuple	indexTuple;
	int			i;
	char	  **result = NULL;
	Relation	rel;
	TupleDesc	tupdesc;

	/* open relation using relid, get tupdesc */
	rel = relation_open(relid, AccessShareLock);
	tupdesc = rel->rd_att;

	/* initialize numatts to 0 in case no primary key exists */
	*numatts = 0;

	/* use relid to get all related indexes */
	indexRelation = heap_openr(IndexRelationName, AccessShareLock);
	ScanKeyEntryInitialize(&entry, 0, Anum_pg_index_indrelid,
						   F_OIDEQ, ObjectIdGetDatum(relid));
	scan = heap_beginscan(indexRelation, SnapshotNow, 1, &entry);

	while ((indexTuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_index index = (Form_pg_index) GETSTRUCT(indexTuple);

		/* we're only interested if it is the primary key */
		if (index->indisprimary == TRUE)
		{
			i = 0;
			while (index->indkey[i++] != 0)
				(*numatts)++;

			if (*numatts > 0)
			{
				result = (char **) palloc(*numatts * sizeof(char *));

				for (i = 0; i < *numatts; i++)
					result[i] = SPI_fname(tupdesc, index->indkey[i]);
			}
			break;
		}
	}
	heap_endscan(scan);
	heap_close(indexRelation, AccessShareLock);
	relation_close(rel, AccessShareLock);

	return result;
}

static char *
get_sql_insert(Oid relid, int16 *pkattnums, int16 pknumatts, char **src_pkattvals, char **tgt_pkattvals)
{
	Relation	rel;
	char	   *relname;
	HeapTuple	tuple;
	TupleDesc	tupdesc;
	int			natts;
	StringInfo	str = makeStringInfo();
	char	   *sql;
	char	   *val;
	int16		key;
	int			i;
	bool		needComma;

	/* get relation name including any needed schema prefix and quoting */
	relname = generate_relation_name(relid);

	/*
	 * Open relation using relid
	 */
	rel = relation_open(relid, AccessShareLock);
	tupdesc = rel->rd_att;
	natts = tupdesc->natts;

	tuple = get_tuple_of_interest(relid, pkattnums, pknumatts, src_pkattvals);
	if (!tuple)
		elog(ERROR, "dblink_build_sql_insert: row not found");

	appendStringInfo(str, "INSERT INTO %s(", relname);

	needComma = false;
	for (i = 0; i < natts; i++)
	{
		if (tupdesc->attrs[i]->attisdropped)
			continue;

		if (needComma)
			appendStringInfo(str, ",");

		appendStringInfo(str, "%s",
				  quote_ident_cstr(NameStr(tupdesc->attrs[i]->attname)));
		needComma = true;
	}

	appendStringInfo(str, ") VALUES(");

	/*
	 * remember attvals are 1 based
	 */
	needComma = false;
	for (i = 0; i < natts; i++)
	{
		if (tupdesc->attrs[i]->attisdropped)
			continue;

		if (needComma)
			appendStringInfo(str, ",");

		if (tgt_pkattvals != NULL)
			key = get_attnum_pk_pos(pkattnums, pknumatts, i + 1);
		else
			key = -1;

		if (key > -1)
			val = pstrdup(tgt_pkattvals[key]);
		else
			val = SPI_getvalue(tuple, tupdesc, i + 1);

		if (val != NULL)
		{
			appendStringInfo(str, "%s", quote_literal_cstr(val));
			pfree(val);
		}
		else
			appendStringInfo(str, "NULL");
		needComma = true;
	}
	appendStringInfo(str, ")");

	sql = pstrdup(str->data);
	pfree(str->data);
	pfree(str);
	relation_close(rel, AccessShareLock);

	return (sql);
}

static char *
get_sql_delete(Oid relid, int16 *pkattnums, int16 pknumatts, char **tgt_pkattvals)
{
	Relation	rel;
	char	   *relname;
	TupleDesc	tupdesc;
	int			natts;
	StringInfo	str = makeStringInfo();
	char	   *sql;
	char	   *val;
	int			i;

	/* get relation name including any needed schema prefix and quoting */
	relname = generate_relation_name(relid);

	/*
	 * Open relation using relid
	 */
	rel = relation_open(relid, AccessShareLock);
	tupdesc = rel->rd_att;
	natts = tupdesc->natts;

	appendStringInfo(str, "DELETE FROM %s WHERE ", relname);
	for (i = 0; i < pknumatts; i++)
	{
		int16		pkattnum = pkattnums[i];

		if (i > 0)
			appendStringInfo(str, " AND ");

		appendStringInfo(str, "%s",
		quote_ident_cstr(NameStr(tupdesc->attrs[pkattnum - 1]->attname)));

		if (tgt_pkattvals != NULL)
			val = pstrdup(tgt_pkattvals[i]);
		else
		{
			elog(ERROR, "Target key array must not be NULL");
			val = NULL;			/* keep compiler quiet */
		}

		if (val != NULL)
		{
			appendStringInfo(str, " = %s", quote_literal_cstr(val));
			pfree(val);
		}
		else
			appendStringInfo(str, " IS NULL");
	}

	sql = pstrdup(str->data);
	pfree(str->data);
	pfree(str);
	relation_close(rel, AccessShareLock);

	return (sql);
}

static char *
get_sql_update(Oid relid, int16 *pkattnums, int16 pknumatts, char **src_pkattvals, char **tgt_pkattvals)
{
	Relation	rel;
	char	   *relname;
	HeapTuple	tuple;
	TupleDesc	tupdesc;
	int			natts;
	StringInfo	str = makeStringInfo();
	char	   *sql;
	char	   *val;
	int16		key;
	int			i;
	bool		needComma;

	/* get relation name including any needed schema prefix and quoting */
	relname = generate_relation_name(relid);

	/*
	 * Open relation using relid
	 */
	rel = relation_open(relid, AccessShareLock);
	tupdesc = rel->rd_att;
	natts = tupdesc->natts;

	tuple = get_tuple_of_interest(relid, pkattnums, pknumatts, src_pkattvals);
	if (!tuple)
		elog(ERROR, "dblink_build_sql_update: row not found");

	appendStringInfo(str, "UPDATE %s SET ", relname);

	needComma = false;
	for (i = 0; i < natts; i++)
	{
		if (tupdesc->attrs[i]->attisdropped)
			continue;

		if (needComma)
			appendStringInfo(str, ", ");

		appendStringInfo(str, "%s = ",
				  quote_ident_cstr(NameStr(tupdesc->attrs[i]->attname)));

		if (tgt_pkattvals != NULL)
			key = get_attnum_pk_pos(pkattnums, pknumatts, i + 1);
		else
			key = -1;

		if (key > -1)
			val = pstrdup(tgt_pkattvals[key]);
		else
			val = SPI_getvalue(tuple, tupdesc, i + 1);

		if (val != NULL)
		{
			appendStringInfo(str, "%s", quote_literal_cstr(val));
			pfree(val);
		}
		else
			appendStringInfo(str, "NULL");
		needComma = true;
	}

	appendStringInfo(str, " WHERE ");

	for (i = 0; i < pknumatts; i++)
	{
		int16		pkattnum = pkattnums[i];

		if (i > 0)
			appendStringInfo(str, " AND ");

		appendStringInfo(str, "%s",
		quote_ident_cstr(NameStr(tupdesc->attrs[pkattnum - 1]->attname)));

		if (tgt_pkattvals != NULL)
			val = pstrdup(tgt_pkattvals[i]);
		else
			val = SPI_getvalue(tuple, tupdesc, pkattnum);

		if (val != NULL)
		{
			appendStringInfo(str, " = %s", quote_literal_cstr(val));
			pfree(val);
		}
		else
			appendStringInfo(str, " IS NULL");
	}

	sql = pstrdup(str->data);
	pfree(str->data);
	pfree(str);
	relation_close(rel, AccessShareLock);

	return (sql);
}

/*
 * Return a properly quoted literal value.
 * Uses quote_literal in quote.c
 */
static char *
quote_literal_cstr(char *rawstr)
{
	text	   *rawstr_text;
	text	   *result_text;
	char	   *result;

	rawstr_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(rawstr)));
	result_text = DatumGetTextP(DirectFunctionCall1(quote_literal, PointerGetDatum(rawstr_text)));
	result = DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(result_text)));

	return result;
}

/*
 * Return a properly quoted identifier.
 * Uses quote_ident in quote.c
 */
static char *
quote_ident_cstr(char *rawstr)
{
	text	   *rawstr_text;
	text	   *result_text;
	char	   *result;

	rawstr_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(rawstr)));
	result_text = DatumGetTextP(DirectFunctionCall1(quote_ident, PointerGetDatum(rawstr_text)));
	result = DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(result_text)));

	return result;
}

static int16
get_attnum_pk_pos(int16 *pkattnums, int16 pknumatts, int16 key)
{
	int			i;

	/*
	 * Not likely a long list anyway, so just scan for the value
	 */
	for (i = 0; i < pknumatts; i++)
		if (key == pkattnums[i])
			return i;

	return -1;
}

static HeapTuple
get_tuple_of_interest(Oid relid, int16 *pkattnums, int16 pknumatts, char **src_pkattvals)
{
	Relation	rel;
	char	   *relname;
	TupleDesc	tupdesc;
	StringInfo	str = makeStringInfo();
	char	   *sql = NULL;
	int			ret;
	HeapTuple	tuple;
	int			i;
	char	   *val = NULL;

	/* get relation name including any needed schema prefix and quoting */
	relname = generate_relation_name(relid);

	/*
	 * Open relation using relid
	 */
	rel = relation_open(relid, AccessShareLock);
	tupdesc = CreateTupleDescCopy(rel->rd_att);
	relation_close(rel, AccessShareLock);

	/*
	 * Connect to SPI manager
	 */
	if ((ret = SPI_connect()) < 0)
		elog(ERROR, "get_tuple_of_interest: SPI_connect returned %d", ret);

	/*
	 * Build sql statement to look up tuple of interest Use src_pkattvals
	 * as the criteria.
	 */
	appendStringInfo(str, "SELECT * FROM %s WHERE ", relname);

	for (i = 0; i < pknumatts; i++)
	{
		int16		pkattnum = pkattnums[i];

		if (i > 0)
			appendStringInfo(str, " AND ");

		appendStringInfo(str, "%s",
		quote_ident_cstr(NameStr(tupdesc->attrs[pkattnum - 1]->attname)));

		val = pstrdup(src_pkattvals[i]);
		if (val != NULL)
		{
			appendStringInfo(str, " = %s", quote_literal_cstr(val));
			pfree(val);
		}
		else
			appendStringInfo(str, " IS NULL");
	}

	sql = pstrdup(str->data);
	pfree(str->data);
	pfree(str);

	/*
	 * Retrieve the desired tuple
	 */
	ret = SPI_exec(sql, 0);
	pfree(sql);

	/*
	 * Only allow one qualifying tuple
	 */
	if ((ret == SPI_OK_SELECT) && (SPI_processed > 1))
		elog(ERROR, "get_tuple_of_interest: Source criteria may not match more than one record.");
	else if (ret == SPI_OK_SELECT && SPI_processed == 1)
	{
		SPITupleTable *tuptable = SPI_tuptable;

		tuple = SPI_copytuple(tuptable->vals[0]);
		SPI_finish();

		return tuple;
	}
	else
	{
		/*
		 * no qualifying tuples
		 */
		SPI_finish();

		return NULL;
	}

	/*
	 * never reached, but keep compiler quiet
	 */
	return NULL;
}

static Oid
get_relid_from_relname(text *relname_text)
{
	RangeVar   *relvar;
	Relation	rel;
	Oid			relid;

	relvar = makeRangeVarFromNameList(textToQualifiedNameList(relname_text, "get_relid_from_relname"));
	rel = heap_openrv(relvar, AccessShareLock);
	relid = RelationGetRelid(rel);
	relation_close(rel, AccessShareLock);

	return relid;
}

static dblink_results *
get_res_ptr(int32 res_id_index)
{
	List	   *ptr;

	/*
	 * short circuit empty list
	 */
	if (res_id == NIL)
		return NULL;

	/*
	 * OK, should be good to go
	 */
	foreach(ptr, res_id)
	{
		dblink_results *this_res_id = (dblink_results *) lfirst(ptr);

		if (this_res_id->res_id_index == res_id_index)
			return this_res_id;
	}
	return NULL;
}

/*
 * Add node to global List res_id
 */
static void
append_res_ptr(dblink_results * results)
{
	res_id = lappend(res_id, results);
}

/*
 * Remove node from global List
 * using res_id_index
 */
static void
remove_res_ptr(dblink_results * results)
{
	res_id = lremove(results, res_id);

	if (res_id == NIL)
		res_id_index = 0;
}

/*
 * generate_relation_name - copied from ruleutils.c
 *		Compute the name to display for a relation specified by OID
 *
 * The result includes all necessary quoting and schema-prefixing.
 */
static char *
generate_relation_name(Oid relid)
{
	HeapTuple	tp;
	Form_pg_class reltup;
	char	   *nspname;
	char	   *result;

	tp = SearchSysCache(RELOID,
						ObjectIdGetDatum(relid),
						0, 0, 0);
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup of relation %u failed", relid);
	reltup = (Form_pg_class) GETSTRUCT(tp);

	/* Qualify the name if not visible in search path */
	if (RelationIsVisible(relid))
		nspname = NULL;
	else
		nspname = get_namespace_name(reltup->relnamespace);

	result = quote_qualified_identifier(nspname, NameStr(reltup->relname));

	ReleaseSysCache(tp);

	return result;
}

/*
 * Modified version of conninfo_parse() from fe-connect.c
 * Used to remove any password from the connection string
 * in order to test whether the server auth method will
 * require it.
 */
static char *
connstr_strip_password(const char *connstr)
{
	char		   *pname;
	char		   *pval;
	char		   *buf;
	char		   *cp;
	char		   *cp2;
	StringInfoData	result;

	/* initialize return value */
	initStringInfo(&result);

	/* Need a modifiable copy of the input string */
	buf = pstrdup(connstr);
	cp = buf;

	while (*cp)
	{
		/* Skip blanks before the parameter name */
		if (isspace((unsigned char) *cp))
		{
			cp++;
			continue;
		}

		/* Get the parameter name */
		pname = cp;
		while (*cp)
		{
			if (*cp == '=')
				break;
			if (isspace((unsigned char) *cp))
			{
				*cp++ = '\0';
				while (*cp)
				{
					if (!isspace((unsigned char) *cp))
						break;
					cp++;
				}
				break;
			}
			cp++;
		}

		/* Check that there is a following '=' */
		if (*cp != '=')
			elog(ERROR, "missing \"=\" after \"%s\" in connection string", pname);
		*cp++ = '\0';

		/* Skip blanks after the '=' */
		while (*cp)
		{
			if (!isspace((unsigned char) *cp))
				break;
			cp++;
		}

		/* Get the parameter value */
		pval = cp;

		if (*cp != '\'')
		{
			cp2 = pval;
			while (*cp)
			{
				if (isspace((unsigned char) *cp))
				{
					*cp++ = '\0';
					break;
				}
				if (*cp == '\\')
				{
					cp++;
					if (*cp != '\0')
						*cp2++ = *cp++;
				}
				else
					*cp2++ = *cp++;
			}
			*cp2 = '\0';
		}
		else
		{
			cp2 = pval;
			cp++;
			for (;;)
			{
				if (*cp == '\0')
					elog(ERROR, "unterminated quoted string in connection string");
				if (*cp == '\\')
				{
					cp++;
					if (*cp != '\0')
						*cp2++ = *cp++;
					continue;
				}
				if (*cp == '\'')
				{
					*cp2 = '\0';
					cp++;
					break;
				}
				*cp2++ = *cp++;
			}
		}

		/*
		 * Now we have the name and the value. If it is not a password,
		 * append to the return connstr.
		 */
		if (strcmp("password", pname) != 0)
			/* append the value */
			appendStringInfo(&result, " %s='%s'", pname, pval);
	}

	return result.data;
}

static void
dblink_security_check(PGconn *conn, const char *connstr)
{
	if (!superuser())
	{
		/* this attempt must fail */
		conn = PQconnectdb(connstr_strip_password(connstr));

		if (PQstatus(conn) == CONNECTION_OK)
		{
			PQfinish(conn);
			conn = NULL;
			elog(ERROR, "Non-superuser cannot connect if the server does not request a password.");
		}
		else
			PQfinish(conn);
	}
}

static int
get_nondropped_natts(Oid relid)
{
	int			nondropped_natts = 0;
	TupleDesc	tupdesc;
	Relation	rel;
	int			natts;
	int			i;

	rel = relation_open(relid, AccessShareLock);
	tupdesc = rel->rd_att;
	natts = tupdesc->natts;

	for (i = 0; i < natts; i++)
	{
		if (tupdesc->attrs[i]->attisdropped)
			continue;
		nondropped_natts++;
	}

	relation_close(rel, AccessShareLock);
	return nondropped_natts;
}
