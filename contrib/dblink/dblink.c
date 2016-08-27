/*
 * dblink.c
 *
 * Functions returning results from a remote database
 *
 * Joe Conway <mail@joeconway.com>
 * And contributors:
 * Darko Prenosil <Darko.Prenosil@finteh.hr>
 * Shridhar Daithankar <shridhar_daithankar@persistent.co.in>
 *
 * contrib/dblink/dblink.c
 * Copyright (c) 2001-2016, PostgreSQL Global Development Group
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

#include <limits.h>

#include "libpq-fe.h"

#include "access/htup_details.h"
#include "access/reloptions.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_type.h"
#include "catalog/pg_user_mapping.h"
#include "executor/spi.h"
#include "foreign/foreign.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "parser/scansup.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tqual.h"

#include "dblink.h"

PG_MODULE_MAGIC;

typedef struct remoteConn
{
	PGconn	   *conn;			/* Hold the remote connection */
	int			openCursorCount;	/* The number of open cursors */
	bool		newXactForCursor;		/* Opened a transaction for a cursor */
} remoteConn;

typedef struct storeInfo
{
	FunctionCallInfo fcinfo;
	Tuplestorestate *tuplestore;
	AttInMetadata *attinmeta;
	MemoryContext tmpcontext;
	char	  **cstrs;
	/* temp storage for results to avoid leaks on exception */
	PGresult   *last_res;
	PGresult   *cur_res;
} storeInfo;

/*
 * Internal declarations
 */
static Datum dblink_record_internal(FunctionCallInfo fcinfo, bool is_async);
static void prepTuplestoreResult(FunctionCallInfo fcinfo);
static void materializeResult(FunctionCallInfo fcinfo, PGconn *conn,
				  PGresult *res);
static void materializeQueryResult(FunctionCallInfo fcinfo,
					   PGconn *conn,
					   const char *conname,
					   const char *sql,
					   bool fail);
static PGresult *storeQueryResult(volatile storeInfo *sinfo, PGconn *conn, const char *sql);
static void storeRow(volatile storeInfo *sinfo, PGresult *res, bool first);
static remoteConn *getConnectionByName(const char *name);
static HTAB *createConnHash(void);
static void createNewConnection(const char *name, remoteConn *rconn);
static void deleteConnection(const char *name);
static char **get_pkey_attnames(Relation rel, int16 *numatts);
static char **get_text_array_contents(ArrayType *array, int *numitems);
static char *get_sql_insert(Relation rel, int *pkattnums, int pknumatts, char **src_pkattvals, char **tgt_pkattvals);
static char *get_sql_delete(Relation rel, int *pkattnums, int pknumatts, char **tgt_pkattvals);
static char *get_sql_update(Relation rel, int *pkattnums, int pknumatts, char **src_pkattvals, char **tgt_pkattvals);
static char *quote_ident_cstr(char *rawstr);
static int	get_attnum_pk_pos(int *pkattnums, int pknumatts, int key);
static HeapTuple get_tuple_of_interest(Relation rel, int *pkattnums, int pknumatts, char **src_pkattvals);
static Relation get_rel_from_relname(text *relname_text, LOCKMODE lockmode, AclMode aclmode);
static char *generate_relation_name(Relation rel);
static void dblink_connstr_check(const char *connstr);
static void dblink_security_check(PGconn *conn, remoteConn *rconn);
static void dblink_res_error(const char *conname, PGresult *res, const char *dblink_context_msg, bool fail);
static char *get_connect_string(const char *servername);
static char *escape_param_str(const char *from);
static void validate_pkattnums(Relation rel,
				   int2vector *pkattnums_arg, int32 pknumatts_arg,
				   int **pkattnums, int *pknumatts);
static bool is_valid_dblink_option(const PQconninfoOption *options,
					   const char *option, Oid context);
static int	applyRemoteGucs(PGconn *conn);
static void restoreLocalGucs(int nestlevel);

/* Global */
static remoteConn *pconn = NULL;
static HTAB *remoteConnHash = NULL;

/*
 *	Following is list that holds multiple remote connections.
 *	Calling convention of each dblink function changes to accept
 *	connection name as the first parameter. The connection list is
 *	much like ecpg e.g. a mapping between a name and a PGconn object.
 */

typedef struct remoteConnHashEnt
{
	char		name[NAMEDATALEN];
	remoteConn *rconn;
} remoteConnHashEnt;

/* initial number of connection hashes */
#define NUMCONN 16

/* general utility */
#define xpfree(var_) \
	do { \
		if (var_ != NULL) \
		{ \
			pfree(var_); \
			var_ = NULL; \
		} \
	} while (0)

#define xpstrdup(var_c, var_) \
	do { \
		if (var_ != NULL) \
			var_c = pstrdup(var_); \
		else \
			var_c = NULL; \
	} while (0)

#define DBLINK_RES_INTERNALERROR(p2) \
	do { \
			msg = pstrdup(PQerrorMessage(conn)); \
			if (res) \
				PQclear(res); \
			elog(ERROR, "%s: %s", p2, msg); \
	} while (0)

#define DBLINK_CONN_NOT_AVAIL \
	do { \
		if(conname) \
			ereport(ERROR, \
					(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST), \
					 errmsg("connection \"%s\" not available", conname))); \
		else \
			ereport(ERROR, \
					(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST), \
					 errmsg("connection not available"))); \
	} while (0)

#define DBLINK_GET_CONN \
	do { \
			char *conname_or_str = text_to_cstring(PG_GETARG_TEXT_PP(0)); \
			rconn = getConnectionByName(conname_or_str); \
			if (rconn) \
			{ \
				conn = rconn->conn; \
				conname = conname_or_str; \
			} \
			else \
			{ \
				connstr = get_connect_string(conname_or_str); \
				if (connstr == NULL) \
				{ \
					connstr = conname_or_str; \
				} \
				dblink_connstr_check(connstr); \
				conn = PQconnectdb(connstr); \
				if (PQstatus(conn) == CONNECTION_BAD) \
				{ \
					msg = pstrdup(PQerrorMessage(conn)); \
					PQfinish(conn); \
					ereport(ERROR, \
							(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION), \
							 errmsg("could not establish connection"), \
							 errdetail_internal("%s", msg))); \
				} \
				dblink_security_check(conn, rconn); \
				if (PQclientEncoding(conn) != GetDatabaseEncoding()) \
					PQsetClientEncoding(conn, GetDatabaseEncodingName()); \
				freeconn = true; \
			} \
	} while (0)

#define DBLINK_GET_NAMED_CONN \
	do { \
			conname = text_to_cstring(PG_GETARG_TEXT_PP(0)); \
			rconn = getConnectionByName(conname); \
			if (rconn) \
				conn = rconn->conn; \
			else \
				DBLINK_CONN_NOT_AVAIL; \
	} while (0)

#define DBLINK_INIT \
	do { \
			if (!pconn) \
			{ \
				pconn = (remoteConn *) MemoryContextAlloc(TopMemoryContext, sizeof(remoteConn)); \
				pconn->conn = NULL; \
				pconn->openCursorCount = 0; \
				pconn->newXactForCursor = FALSE; \
			} \
	} while (0)

/*
 * Create a persistent connection to another database
 */
PG_FUNCTION_INFO_V1(dblink_connect);
Datum
dblink_connect(PG_FUNCTION_ARGS)
{
	char	   *conname_or_str = NULL;
	char	   *connstr = NULL;
	char	   *connname = NULL;
	char	   *msg;
	PGconn	   *conn = NULL;
	remoteConn *rconn = NULL;

	DBLINK_INIT;

	if (PG_NARGS() == 2)
	{
		conname_or_str = text_to_cstring(PG_GETARG_TEXT_PP(1));
		connname = text_to_cstring(PG_GETARG_TEXT_PP(0));
	}
	else if (PG_NARGS() == 1)
		conname_or_str = text_to_cstring(PG_GETARG_TEXT_PP(0));

	if (connname)
		rconn = (remoteConn *) MemoryContextAlloc(TopMemoryContext,
												  sizeof(remoteConn));

	/* first check for valid foreign data server */
	connstr = get_connect_string(conname_or_str);
	if (connstr == NULL)
		connstr = conname_or_str;

	/* check password in connection string if not superuser */
	dblink_connstr_check(connstr);
	conn = PQconnectdb(connstr);

	if (PQstatus(conn) == CONNECTION_BAD)
	{
		msg = pstrdup(PQerrorMessage(conn));
		PQfinish(conn);
		if (rconn)
			pfree(rconn);

		ereport(ERROR,
				(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
				 errmsg("could not establish connection"),
				 errdetail_internal("%s", msg)));
	}

	/* check password actually used if not superuser */
	dblink_security_check(conn, rconn);

	/* attempt to set client encoding to match server encoding, if needed */
	if (PQclientEncoding(conn) != GetDatabaseEncoding())
		PQsetClientEncoding(conn, GetDatabaseEncodingName());

	if (connname)
	{
		rconn->conn = conn;
		createNewConnection(connname, rconn);
	}
	else
		pconn->conn = conn;

	PG_RETURN_TEXT_P(cstring_to_text("OK"));
}

/*
 * Clear a persistent connection to another database
 */
PG_FUNCTION_INFO_V1(dblink_disconnect);
Datum
dblink_disconnect(PG_FUNCTION_ARGS)
{
	char	   *conname = NULL;
	remoteConn *rconn = NULL;
	PGconn	   *conn = NULL;

	DBLINK_INIT;

	if (PG_NARGS() == 1)
	{
		conname = text_to_cstring(PG_GETARG_TEXT_PP(0));
		rconn = getConnectionByName(conname);
		if (rconn)
			conn = rconn->conn;
	}
	else
		conn = pconn->conn;

	if (!conn)
		DBLINK_CONN_NOT_AVAIL;

	PQfinish(conn);
	if (rconn)
	{
		deleteConnection(conname);
		pfree(rconn);
	}
	else
		pconn->conn = NULL;

	PG_RETURN_TEXT_P(cstring_to_text("OK"));
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
	char	   *curname = NULL;
	char	   *sql = NULL;
	char	   *conname = NULL;
	StringInfoData buf;
	remoteConn *rconn = NULL;
	bool		fail = true;	/* default to backward compatible behavior */

	DBLINK_INIT;
	initStringInfo(&buf);

	if (PG_NARGS() == 2)
	{
		/* text,text */
		curname = text_to_cstring(PG_GETARG_TEXT_PP(0));
		sql = text_to_cstring(PG_GETARG_TEXT_PP(1));
		rconn = pconn;
	}
	else if (PG_NARGS() == 3)
	{
		/* might be text,text,text or text,text,bool */
		if (get_fn_expr_argtype(fcinfo->flinfo, 2) == BOOLOID)
		{
			curname = text_to_cstring(PG_GETARG_TEXT_PP(0));
			sql = text_to_cstring(PG_GETARG_TEXT_PP(1));
			fail = PG_GETARG_BOOL(2);
			rconn = pconn;
		}
		else
		{
			conname = text_to_cstring(PG_GETARG_TEXT_PP(0));
			curname = text_to_cstring(PG_GETARG_TEXT_PP(1));
			sql = text_to_cstring(PG_GETARG_TEXT_PP(2));
			rconn = getConnectionByName(conname);
		}
	}
	else if (PG_NARGS() == 4)
	{
		/* text,text,text,bool */
		conname = text_to_cstring(PG_GETARG_TEXT_PP(0));
		curname = text_to_cstring(PG_GETARG_TEXT_PP(1));
		sql = text_to_cstring(PG_GETARG_TEXT_PP(2));
		fail = PG_GETARG_BOOL(3);
		rconn = getConnectionByName(conname);
	}

	if (!rconn || !rconn->conn)
		DBLINK_CONN_NOT_AVAIL;
	else
		conn = rconn->conn;

	/* If we are not in a transaction, start one */
	if (PQtransactionStatus(conn) == PQTRANS_IDLE)
	{
		res = PQexec(conn, "BEGIN");
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			DBLINK_RES_INTERNALERROR("begin error");
		PQclear(res);
		rconn->newXactForCursor = TRUE;

		/*
		 * Since transaction state was IDLE, we force cursor count to
		 * initially be 0. This is needed as a previous ABORT might have wiped
		 * out our transaction without maintaining the cursor count for us.
		 */
		rconn->openCursorCount = 0;
	}

	/* if we started a transaction, increment cursor count */
	if (rconn->newXactForCursor)
		(rconn->openCursorCount)++;

	appendStringInfo(&buf, "DECLARE %s CURSOR FOR %s", curname, sql);
	res = PQexec(conn, buf.data);
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		dblink_res_error(conname, res, "could not open cursor", fail);
		PG_RETURN_TEXT_P(cstring_to_text("ERROR"));
	}

	PQclear(res);
	PG_RETURN_TEXT_P(cstring_to_text("OK"));
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
	char	   *curname = NULL;
	char	   *conname = NULL;
	StringInfoData buf;
	char	   *msg;
	remoteConn *rconn = NULL;
	bool		fail = true;	/* default to backward compatible behavior */

	DBLINK_INIT;
	initStringInfo(&buf);

	if (PG_NARGS() == 1)
	{
		/* text */
		curname = text_to_cstring(PG_GETARG_TEXT_PP(0));
		rconn = pconn;
	}
	else if (PG_NARGS() == 2)
	{
		/* might be text,text or text,bool */
		if (get_fn_expr_argtype(fcinfo->flinfo, 1) == BOOLOID)
		{
			curname = text_to_cstring(PG_GETARG_TEXT_PP(0));
			fail = PG_GETARG_BOOL(1);
			rconn = pconn;
		}
		else
		{
			conname = text_to_cstring(PG_GETARG_TEXT_PP(0));
			curname = text_to_cstring(PG_GETARG_TEXT_PP(1));
			rconn = getConnectionByName(conname);
		}
	}
	if (PG_NARGS() == 3)
	{
		/* text,text,bool */
		conname = text_to_cstring(PG_GETARG_TEXT_PP(0));
		curname = text_to_cstring(PG_GETARG_TEXT_PP(1));
		fail = PG_GETARG_BOOL(2);
		rconn = getConnectionByName(conname);
	}

	if (!rconn || !rconn->conn)
		DBLINK_CONN_NOT_AVAIL;
	else
		conn = rconn->conn;

	appendStringInfo(&buf, "CLOSE %s", curname);

	/* close the cursor */
	res = PQexec(conn, buf.data);
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		dblink_res_error(conname, res, "could not close cursor", fail);
		PG_RETURN_TEXT_P(cstring_to_text("ERROR"));
	}

	PQclear(res);

	/* if we started a transaction, decrement cursor count */
	if (rconn->newXactForCursor)
	{
		(rconn->openCursorCount)--;

		/* if count is zero, commit the transaction */
		if (rconn->openCursorCount == 0)
		{
			rconn->newXactForCursor = FALSE;

			res = PQexec(conn, "COMMIT");
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
				DBLINK_RES_INTERNALERROR("commit error");
			PQclear(res);
		}
	}

	PG_RETURN_TEXT_P(cstring_to_text("OK"));
}

/*
 * Fetch results from an open cursor
 */
PG_FUNCTION_INFO_V1(dblink_fetch);
Datum
dblink_fetch(PG_FUNCTION_ARGS)
{
	PGresult   *res = NULL;
	char	   *conname = NULL;
	remoteConn *rconn = NULL;
	PGconn	   *conn = NULL;
	StringInfoData buf;
	char	   *curname = NULL;
	int			howmany = 0;
	bool		fail = true;	/* default to backward compatible */

	prepTuplestoreResult(fcinfo);

	DBLINK_INIT;

	if (PG_NARGS() == 4)
	{
		/* text,text,int,bool */
		conname = text_to_cstring(PG_GETARG_TEXT_PP(0));
		curname = text_to_cstring(PG_GETARG_TEXT_PP(1));
		howmany = PG_GETARG_INT32(2);
		fail = PG_GETARG_BOOL(3);

		rconn = getConnectionByName(conname);
		if (rconn)
			conn = rconn->conn;
	}
	else if (PG_NARGS() == 3)
	{
		/* text,text,int or text,int,bool */
		if (get_fn_expr_argtype(fcinfo->flinfo, 2) == BOOLOID)
		{
			curname = text_to_cstring(PG_GETARG_TEXT_PP(0));
			howmany = PG_GETARG_INT32(1);
			fail = PG_GETARG_BOOL(2);
			conn = pconn->conn;
		}
		else
		{
			conname = text_to_cstring(PG_GETARG_TEXT_PP(0));
			curname = text_to_cstring(PG_GETARG_TEXT_PP(1));
			howmany = PG_GETARG_INT32(2);

			rconn = getConnectionByName(conname);
			if (rconn)
				conn = rconn->conn;
		}
	}
	else if (PG_NARGS() == 2)
	{
		/* text,int */
		curname = text_to_cstring(PG_GETARG_TEXT_PP(0));
		howmany = PG_GETARG_INT32(1);
		conn = pconn->conn;
	}

	if (!conn)
		DBLINK_CONN_NOT_AVAIL;

	initStringInfo(&buf);
	appendStringInfo(&buf, "FETCH %d FROM %s", howmany, curname);

	/*
	 * Try to execute the query.  Note that since libpq uses malloc, the
	 * PGresult will be long-lived even though we are still in a short-lived
	 * memory context.
	 */
	res = PQexec(conn, buf.data);
	if (!res ||
		(PQresultStatus(res) != PGRES_COMMAND_OK &&
		 PQresultStatus(res) != PGRES_TUPLES_OK))
	{
		dblink_res_error(conname, res, "could not fetch from cursor", fail);
		return (Datum) 0;
	}
	else if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		/* cursor does not exist - closed already or bad name */
		PQclear(res);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_NAME),
				 errmsg("cursor \"%s\" does not exist", curname)));
	}

	materializeResult(fcinfo, conn, res);
	return (Datum) 0;
}

/*
 * Note: this is the new preferred version of dblink
 */
PG_FUNCTION_INFO_V1(dblink_record);
Datum
dblink_record(PG_FUNCTION_ARGS)
{
	return dblink_record_internal(fcinfo, false);
}

PG_FUNCTION_INFO_V1(dblink_send_query);
Datum
dblink_send_query(PG_FUNCTION_ARGS)
{
	char	   *conname = NULL;
	PGconn	   *conn = NULL;
	char	   *sql = NULL;
	remoteConn *rconn = NULL;
	int			retval;

	if (PG_NARGS() == 2)
	{
		DBLINK_GET_NAMED_CONN;
		sql = text_to_cstring(PG_GETARG_TEXT_PP(1));
	}
	else
		/* shouldn't happen */
		elog(ERROR, "wrong number of arguments");

	/* async query send */
	retval = PQsendQuery(conn, sql);
	if (retval != 1)
		elog(NOTICE, "could not send query: %s", PQerrorMessage(conn));

	PG_RETURN_INT32(retval);
}

PG_FUNCTION_INFO_V1(dblink_get_result);
Datum
dblink_get_result(PG_FUNCTION_ARGS)
{
	return dblink_record_internal(fcinfo, true);
}

static Datum
dblink_record_internal(FunctionCallInfo fcinfo, bool is_async)
{
	PGconn	   *volatile conn = NULL;
	volatile bool freeconn = false;

	prepTuplestoreResult(fcinfo);

	DBLINK_INIT;

	PG_TRY();
	{
		char	   *msg;
		char	   *connstr = NULL;
		char	   *sql = NULL;
		char	   *conname = NULL;
		remoteConn *rconn = NULL;
		bool		fail = true;	/* default to backward compatible */

		if (!is_async)
		{
			if (PG_NARGS() == 3)
			{
				/* text,text,bool */
				DBLINK_GET_CONN;
				sql = text_to_cstring(PG_GETARG_TEXT_PP(1));
				fail = PG_GETARG_BOOL(2);
			}
			else if (PG_NARGS() == 2)
			{
				/* text,text or text,bool */
				if (get_fn_expr_argtype(fcinfo->flinfo, 1) == BOOLOID)
				{
					conn = pconn->conn;
					sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
					fail = PG_GETARG_BOOL(1);
				}
				else
				{
					DBLINK_GET_CONN;
					sql = text_to_cstring(PG_GETARG_TEXT_PP(1));
				}
			}
			else if (PG_NARGS() == 1)
			{
				/* text */
				conn = pconn->conn;
				sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
			}
			else
				/* shouldn't happen */
				elog(ERROR, "wrong number of arguments");
		}
		else	/* is_async */
		{
			/* get async result */
			if (PG_NARGS() == 2)
			{
				/* text,bool */
				DBLINK_GET_NAMED_CONN;
				fail = PG_GETARG_BOOL(1);
			}
			else if (PG_NARGS() == 1)
			{
				/* text */
				DBLINK_GET_NAMED_CONN;
			}
			else
				/* shouldn't happen */
				elog(ERROR, "wrong number of arguments");
		}

		if (!conn)
			DBLINK_CONN_NOT_AVAIL;

		if (!is_async)
		{
			/* synchronous query, use efficient tuple collection method */
			materializeQueryResult(fcinfo, conn, conname, sql, fail);
		}
		else
		{
			/* async result retrieval, do it the old way */
			PGresult   *res = PQgetResult(conn);

			/* NULL means we're all done with the async results */
			if (res)
			{
				if (PQresultStatus(res) != PGRES_COMMAND_OK &&
					PQresultStatus(res) != PGRES_TUPLES_OK)
				{
					dblink_res_error(conname, res, "could not execute query",
									 fail);
					/* if fail isn't set, we'll return an empty query result */
				}
				else
				{
					materializeResult(fcinfo, conn, res);
				}
			}
		}
	}
	PG_CATCH();
	{
		/* if needed, close the connection to the database */
		if (freeconn)
			PQfinish(conn);
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* if needed, close the connection to the database */
	if (freeconn)
		PQfinish(conn);

	return (Datum) 0;
}

/*
 * Verify function caller can handle a tuplestore result, and set up for that.
 *
 * Note: if the caller returns without actually creating a tuplestore, the
 * executor will treat the function result as an empty set.
 */
static void
prepTuplestoreResult(FunctionCallInfo fcinfo)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	/* check to see if query supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/* let the executor know we're sending back a tuplestore */
	rsinfo->returnMode = SFRM_Materialize;

	/* caller must fill these to return a non-empty result */
	rsinfo->setResult = NULL;
	rsinfo->setDesc = NULL;
}

/*
 * Copy the contents of the PGresult into a tuplestore to be returned
 * as the result of the current function.
 * The PGresult will be released in this function.
 */
static void
materializeResult(FunctionCallInfo fcinfo, PGconn *conn, PGresult *res)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	/* prepTuplestoreResult must have been called previously */
	Assert(rsinfo->returnMode == SFRM_Materialize);

	PG_TRY();
	{
		TupleDesc	tupdesc;
		bool		is_sql_cmd;
		int			ntuples;
		int			nfields;

		if (PQresultStatus(res) == PGRES_COMMAND_OK)
		{
			is_sql_cmd = true;

			/*
			 * need a tuple descriptor representing one TEXT column to return
			 * the command status string as our result tuple
			 */
			tupdesc = CreateTemplateTupleDesc(1, false);
			TupleDescInitEntry(tupdesc, (AttrNumber) 1, "status",
							   TEXTOID, -1, 0);
			ntuples = 1;
			nfields = 1;
		}
		else
		{
			Assert(PQresultStatus(res) == PGRES_TUPLES_OK);

			is_sql_cmd = false;

			/* get a tuple descriptor for our result type */
			switch (get_call_result_type(fcinfo, NULL, &tupdesc))
			{
				case TYPEFUNC_COMPOSITE:
					/* success */
					break;
				case TYPEFUNC_RECORD:
					/* failed to determine actual type of RECORD */
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("function returning record called in context "
							   "that cannot accept type record")));
					break;
				default:
					/* result type isn't composite */
					elog(ERROR, "return type must be a row type");
					break;
			}

			/* make sure we have a persistent copy of the tupdesc */
			tupdesc = CreateTupleDescCopy(tupdesc);
			ntuples = PQntuples(res);
			nfields = PQnfields(res);
		}

		/*
		 * check result and tuple descriptor have the same number of columns
		 */
		if (nfields != tupdesc->natts)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("remote query result rowtype does not match "
							"the specified FROM clause rowtype")));

		if (ntuples > 0)
		{
			AttInMetadata *attinmeta;
			int			nestlevel = -1;
			Tuplestorestate *tupstore;
			MemoryContext oldcontext;
			int			row;
			char	  **values;

			attinmeta = TupleDescGetAttInMetadata(tupdesc);

			/* Set GUCs to ensure we read GUC-sensitive data types correctly */
			if (!is_sql_cmd)
				nestlevel = applyRemoteGucs(conn);

			oldcontext = MemoryContextSwitchTo(
									rsinfo->econtext->ecxt_per_query_memory);
			tupstore = tuplestore_begin_heap(true, false, work_mem);
			rsinfo->setResult = tupstore;
			rsinfo->setDesc = tupdesc;
			MemoryContextSwitchTo(oldcontext);

			values = (char **) palloc(nfields * sizeof(char *));

			/* put all tuples into the tuplestore */
			for (row = 0; row < ntuples; row++)
			{
				HeapTuple	tuple;

				if (!is_sql_cmd)
				{
					int			i;

					for (i = 0; i < nfields; i++)
					{
						if (PQgetisnull(res, row, i))
							values[i] = NULL;
						else
							values[i] = PQgetvalue(res, row, i);
					}
				}
				else
				{
					values[0] = PQcmdStatus(res);
				}

				/* build the tuple and put it into the tuplestore. */
				tuple = BuildTupleFromCStrings(attinmeta, values);
				tuplestore_puttuple(tupstore, tuple);
			}

			/* clean up GUC settings, if we changed any */
			restoreLocalGucs(nestlevel);

			/* clean up and return the tuplestore */
			tuplestore_donestoring(tupstore);
		}

		PQclear(res);
	}
	PG_CATCH();
	{
		/* be sure to release the libpq result */
		PQclear(res);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * Execute the given SQL command and store its results into a tuplestore
 * to be returned as the result of the current function.
 *
 * This is equivalent to PQexec followed by materializeResult, but we make
 * use of libpq's single-row mode to avoid accumulating the whole result
 * inside libpq before it gets transferred to the tuplestore.
 */
static void
materializeQueryResult(FunctionCallInfo fcinfo,
					   PGconn *conn,
					   const char *conname,
					   const char *sql,
					   bool fail)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	PGresult   *volatile res = NULL;
	volatile storeInfo sinfo;

	/* prepTuplestoreResult must have been called previously */
	Assert(rsinfo->returnMode == SFRM_Materialize);

	/* initialize storeInfo to empty */
	memset((void *) &sinfo, 0, sizeof(sinfo));
	sinfo.fcinfo = fcinfo;

	PG_TRY();
	{
		/* Create short-lived memory context for data conversions */
		sinfo.tmpcontext = AllocSetContextCreate(CurrentMemoryContext,
												 "dblink temporary context",
												 ALLOCSET_DEFAULT_SIZES);

		/* execute query, collecting any tuples into the tuplestore */
		res = storeQueryResult(&sinfo, conn, sql);

		if (!res ||
			(PQresultStatus(res) != PGRES_COMMAND_OK &&
			 PQresultStatus(res) != PGRES_TUPLES_OK))
		{
			/*
			 * dblink_res_error will clear the passed PGresult, so we need
			 * this ugly dance to avoid doing so twice during error exit
			 */
			PGresult   *res1 = res;

			res = NULL;
			dblink_res_error(conname, res1, "could not execute query", fail);
			/* if fail isn't set, we'll return an empty query result */
		}
		else if (PQresultStatus(res) == PGRES_COMMAND_OK)
		{
			/*
			 * storeRow didn't get called, so we need to convert the command
			 * status string to a tuple manually
			 */
			TupleDesc	tupdesc;
			AttInMetadata *attinmeta;
			Tuplestorestate *tupstore;
			HeapTuple	tuple;
			char	   *values[1];
			MemoryContext oldcontext;

			/*
			 * need a tuple descriptor representing one TEXT column to return
			 * the command status string as our result tuple
			 */
			tupdesc = CreateTemplateTupleDesc(1, false);
			TupleDescInitEntry(tupdesc, (AttrNumber) 1, "status",
							   TEXTOID, -1, 0);
			attinmeta = TupleDescGetAttInMetadata(tupdesc);

			oldcontext = MemoryContextSwitchTo(
									rsinfo->econtext->ecxt_per_query_memory);
			tupstore = tuplestore_begin_heap(true, false, work_mem);
			rsinfo->setResult = tupstore;
			rsinfo->setDesc = tupdesc;
			MemoryContextSwitchTo(oldcontext);

			values[0] = PQcmdStatus(res);

			/* build the tuple and put it into the tuplestore. */
			tuple = BuildTupleFromCStrings(attinmeta, values);
			tuplestore_puttuple(tupstore, tuple);

			PQclear(res);
			res = NULL;
		}
		else
		{
			Assert(PQresultStatus(res) == PGRES_TUPLES_OK);
			/* storeRow should have created a tuplestore */
			Assert(rsinfo->setResult != NULL);

			PQclear(res);
			res = NULL;
		}

		/* clean up data conversion short-lived memory context */
		if (sinfo.tmpcontext != NULL)
			MemoryContextDelete(sinfo.tmpcontext);
		sinfo.tmpcontext = NULL;

		PQclear(sinfo.last_res);
		sinfo.last_res = NULL;
		PQclear(sinfo.cur_res);
		sinfo.cur_res = NULL;
	}
	PG_CATCH();
	{
		/* be sure to release any libpq result we collected */
		PQclear(res);
		PQclear(sinfo.last_res);
		PQclear(sinfo.cur_res);
		/* and clear out any pending data in libpq */
		while ((res = PQgetResult(conn)) != NULL)
			PQclear(res);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * Execute query, and send any result rows to sinfo->tuplestore.
 */
static PGresult *
storeQueryResult(volatile storeInfo *sinfo, PGconn *conn, const char *sql)
{
	bool		first = true;
	int			nestlevel = -1;
	PGresult   *res;

	if (!PQsendQuery(conn, sql))
		elog(ERROR, "could not send query: %s", PQerrorMessage(conn));

	if (!PQsetSingleRowMode(conn))		/* shouldn't fail */
		elog(ERROR, "failed to set single-row mode for dblink query");

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		sinfo->cur_res = PQgetResult(conn);
		if (!sinfo->cur_res)
			break;

		if (PQresultStatus(sinfo->cur_res) == PGRES_SINGLE_TUPLE)
		{
			/* got one row from possibly-bigger resultset */

			/*
			 * Set GUCs to ensure we read GUC-sensitive data types correctly.
			 * We shouldn't do this until we have a row in hand, to ensure
			 * libpq has seen any earlier ParameterStatus protocol messages.
			 */
			if (first && nestlevel < 0)
				nestlevel = applyRemoteGucs(conn);

			storeRow(sinfo, sinfo->cur_res, first);

			PQclear(sinfo->cur_res);
			sinfo->cur_res = NULL;
			first = false;
		}
		else
		{
			/* if empty resultset, fill tuplestore header */
			if (first && PQresultStatus(sinfo->cur_res) == PGRES_TUPLES_OK)
				storeRow(sinfo, sinfo->cur_res, first);

			/* store completed result at last_res */
			PQclear(sinfo->last_res);
			sinfo->last_res = sinfo->cur_res;
			sinfo->cur_res = NULL;
			first = true;
		}
	}

	/* clean up GUC settings, if we changed any */
	restoreLocalGucs(nestlevel);

	/* return last_res */
	res = sinfo->last_res;
	sinfo->last_res = NULL;
	return res;
}

/*
 * Send single row to sinfo->tuplestore.
 *
 * If "first" is true, create the tuplestore using PGresult's metadata
 * (in this case the PGresult might contain either zero or one row).
 */
static void
storeRow(volatile storeInfo *sinfo, PGresult *res, bool first)
{
	int			nfields = PQnfields(res);
	HeapTuple	tuple;
	int			i;
	MemoryContext oldcontext;

	if (first)
	{
		/* Prepare for new result set */
		ReturnSetInfo *rsinfo = (ReturnSetInfo *) sinfo->fcinfo->resultinfo;
		TupleDesc	tupdesc;

		/*
		 * It's possible to get more than one result set if the query string
		 * contained multiple SQL commands.  In that case, we follow PQexec's
		 * traditional behavior of throwing away all but the last result.
		 */
		if (sinfo->tuplestore)
			tuplestore_end(sinfo->tuplestore);
		sinfo->tuplestore = NULL;

		/* get a tuple descriptor for our result type */
		switch (get_call_result_type(sinfo->fcinfo, NULL, &tupdesc))
		{
			case TYPEFUNC_COMPOSITE:
				/* success */
				break;
			case TYPEFUNC_RECORD:
				/* failed to determine actual type of RECORD */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("function returning record called in context "
								"that cannot accept type record")));
				break;
			default:
				/* result type isn't composite */
				elog(ERROR, "return type must be a row type");
				break;
		}

		/* make sure we have a persistent copy of the tupdesc */
		tupdesc = CreateTupleDescCopy(tupdesc);

		/* check result and tuple descriptor have the same number of columns */
		if (nfields != tupdesc->natts)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("remote query result rowtype does not match "
							"the specified FROM clause rowtype")));

		/* Prepare attinmeta for later data conversions */
		sinfo->attinmeta = TupleDescGetAttInMetadata(tupdesc);

		/* Create a new, empty tuplestore */
		oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
		sinfo->tuplestore = tuplestore_begin_heap(true, false, work_mem);
		rsinfo->setResult = sinfo->tuplestore;
		rsinfo->setDesc = tupdesc;
		MemoryContextSwitchTo(oldcontext);

		/* Done if empty resultset */
		if (PQntuples(res) == 0)
			return;

		/*
		 * Set up sufficiently-wide string pointers array; this won't change
		 * in size so it's easy to preallocate.
		 */
		if (sinfo->cstrs)
			pfree(sinfo->cstrs);
		sinfo->cstrs = (char **) palloc(nfields * sizeof(char *));
	}

	/* Should have a single-row result if we get here */
	Assert(PQntuples(res) == 1);

	/*
	 * Do the following work in a temp context that we reset after each tuple.
	 * This cleans up not only the data we have direct access to, but any
	 * cruft the I/O functions might leak.
	 */
	oldcontext = MemoryContextSwitchTo(sinfo->tmpcontext);

	/*
	 * Fill cstrs with null-terminated strings of column values.
	 */
	for (i = 0; i < nfields; i++)
	{
		if (PQgetisnull(res, 0, i))
			sinfo->cstrs[i] = NULL;
		else
			sinfo->cstrs[i] = PQgetvalue(res, 0, i);
	}

	/* Convert row to a tuple, and add it to the tuplestore */
	tuple = BuildTupleFromCStrings(sinfo->attinmeta, sinfo->cstrs);

	tuplestore_puttuple(sinfo->tuplestore, tuple);

	/* Clean up */
	MemoryContextSwitchTo(oldcontext);
	MemoryContextReset(sinfo->tmpcontext);
}

/*
 * List all open dblink connections by name.
 * Returns an array of all connection names.
 * Takes no params
 */
PG_FUNCTION_INFO_V1(dblink_get_connections);
Datum
dblink_get_connections(PG_FUNCTION_ARGS)
{
	HASH_SEQ_STATUS status;
	remoteConnHashEnt *hentry;
	ArrayBuildState *astate = NULL;

	if (remoteConnHash)
	{
		hash_seq_init(&status, remoteConnHash);
		while ((hentry = (remoteConnHashEnt *) hash_seq_search(&status)) != NULL)
		{
			/* stash away current value */
			astate = accumArrayResult(astate,
									  CStringGetTextDatum(hentry->name),
									  false, TEXTOID, CurrentMemoryContext);
		}
	}

	if (astate)
		PG_RETURN_ARRAYTYPE_P(makeArrayResult(astate,
											  CurrentMemoryContext));
	else
		PG_RETURN_NULL();
}

/*
 * Checks if a given remote connection is busy
 *
 * Returns 1 if the connection is busy, 0 otherwise
 * Params:
 *	text connection_name - name of the connection to check
 *
 */
PG_FUNCTION_INFO_V1(dblink_is_busy);
Datum
dblink_is_busy(PG_FUNCTION_ARGS)
{
	char	   *conname = NULL;
	PGconn	   *conn = NULL;
	remoteConn *rconn = NULL;

	DBLINK_INIT;
	DBLINK_GET_NAMED_CONN;

	PQconsumeInput(conn);
	PG_RETURN_INT32(PQisBusy(conn));
}

/*
 * Cancels a running request on a connection
 *
 * Returns text:
 *	"OK" if the cancel request has been sent correctly,
 *		an error message otherwise
 *
 * Params:
 *	text connection_name - name of the connection to check
 *
 */
PG_FUNCTION_INFO_V1(dblink_cancel_query);
Datum
dblink_cancel_query(PG_FUNCTION_ARGS)
{
	int			res = 0;
	char	   *conname = NULL;
	PGconn	   *conn = NULL;
	remoteConn *rconn = NULL;
	PGcancel   *cancel;
	char		errbuf[256];

	DBLINK_INIT;
	DBLINK_GET_NAMED_CONN;
	cancel = PQgetCancel(conn);

	res = PQcancel(cancel, errbuf, 256);
	PQfreeCancel(cancel);

	if (res == 1)
		PG_RETURN_TEXT_P(cstring_to_text("OK"));
	else
		PG_RETURN_TEXT_P(cstring_to_text(errbuf));
}


/*
 * Get error message from a connection
 *
 * Returns text:
 *	"OK" if no error, an error message otherwise
 *
 * Params:
 *	text connection_name - name of the connection to check
 *
 */
PG_FUNCTION_INFO_V1(dblink_error_message);
Datum
dblink_error_message(PG_FUNCTION_ARGS)
{
	char	   *msg;
	char	   *conname = NULL;
	PGconn	   *conn = NULL;
	remoteConn *rconn = NULL;

	DBLINK_INIT;
	DBLINK_GET_NAMED_CONN;

	msg = PQerrorMessage(conn);
	if (msg == NULL || msg[0] == '\0')
		PG_RETURN_TEXT_P(cstring_to_text("OK"));
	else
		PG_RETURN_TEXT_P(cstring_to_text(msg));
}

/*
 * Execute an SQL non-SELECT command
 */
PG_FUNCTION_INFO_V1(dblink_exec);
Datum
dblink_exec(PG_FUNCTION_ARGS)
{
	text	   *volatile sql_cmd_status = NULL;
	PGconn	   *volatile conn = NULL;
	volatile bool freeconn = false;

	DBLINK_INIT;

	PG_TRY();
	{
		char	   *msg;
		PGresult   *res = NULL;
		char	   *connstr = NULL;
		char	   *sql = NULL;
		char	   *conname = NULL;
		remoteConn *rconn = NULL;
		bool		fail = true;	/* default to backward compatible behavior */

		if (PG_NARGS() == 3)
		{
			/* must be text,text,bool */
			DBLINK_GET_CONN;
			sql = text_to_cstring(PG_GETARG_TEXT_PP(1));
			fail = PG_GETARG_BOOL(2);
		}
		else if (PG_NARGS() == 2)
		{
			/* might be text,text or text,bool */
			if (get_fn_expr_argtype(fcinfo->flinfo, 1) == BOOLOID)
			{
				conn = pconn->conn;
				sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
				fail = PG_GETARG_BOOL(1);
			}
			else
			{
				DBLINK_GET_CONN;
				sql = text_to_cstring(PG_GETARG_TEXT_PP(1));
			}
		}
		else if (PG_NARGS() == 1)
		{
			/* must be single text argument */
			conn = pconn->conn;
			sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
		}
		else
			/* shouldn't happen */
			elog(ERROR, "wrong number of arguments");

		if (!conn)
			DBLINK_CONN_NOT_AVAIL;

		res = PQexec(conn, sql);
		if (!res ||
			(PQresultStatus(res) != PGRES_COMMAND_OK &&
			 PQresultStatus(res) != PGRES_TUPLES_OK))
		{
			dblink_res_error(conname, res, "could not execute command", fail);

			/*
			 * and save a copy of the command status string to return as our
			 * result tuple
			 */
			sql_cmd_status = cstring_to_text("ERROR");
		}
		else if (PQresultStatus(res) == PGRES_COMMAND_OK)
		{
			/*
			 * and save a copy of the command status string to return as our
			 * result tuple
			 */
			sql_cmd_status = cstring_to_text(PQcmdStatus(res));
			PQclear(res);
		}
		else
		{
			PQclear(res);
			ereport(ERROR,
				  (errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
				   errmsg("statement returning results not allowed")));
		}
	}
	PG_CATCH();
	{
		/* if needed, close the connection to the database */
		if (freeconn)
			PQfinish(conn);
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* if needed, close the connection to the database */
	if (freeconn)
		PQfinish(conn);

	PG_RETURN_TEXT_P(sql_cmd_status);
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
	char	  **results;
	FuncCallContext *funcctx;
	int32		call_cntr;
	int32		max_calls;
	AttInMetadata *attinmeta;
	MemoryContext oldcontext;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		Relation	rel;
		TupleDesc	tupdesc;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * switch to memory context appropriate for multiple function calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* open target relation */
		rel = get_rel_from_relname(PG_GETARG_TEXT_P(0), AccessShareLock, ACL_SELECT);

		/* get the array of attnums */
		results = get_pkey_attnames(rel, &numatts);

		relation_close(rel, AccessShareLock);

		/*
		 * need a tuple descriptor representing one INT and one TEXT column
		 */
		tupdesc = CreateTemplateTupleDesc(2, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "position",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "colname",
						   TEXTOID, -1, 0);

		/*
		 * Generate attribute metadata needed later to produce tuples from raw
		 * C strings
		 */
		attinmeta = TupleDescGetAttInMetadata(tupdesc);
		funcctx->attinmeta = attinmeta;

		if ((results != NULL) && (numatts > 0))
		{
			funcctx->max_calls = numatts;

			/* got results, keep track of them */
			funcctx->user_fctx = results;
		}
		else
		{
			/* fast track when no results */
			MemoryContextSwitchTo(oldcontext);
			SRF_RETURN_DONE(funcctx);
		}

		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	/*
	 * initialize per-call variables
	 */
	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;

	results = (char **) funcctx->user_fctx;
	attinmeta = funcctx->attinmeta;

	if (call_cntr < max_calls)	/* do when there is more left to send */
	{
		char	  **values;
		HeapTuple	tuple;
		Datum		result;

		values = (char **) palloc(2 * sizeof(char *));
		values[0] = psprintf("%d", call_cntr + 1);
		values[1] = results[call_cntr];

		/* build the tuple */
		tuple = BuildTupleFromCStrings(attinmeta, values);

		/* make the tuple into a datum */
		result = HeapTupleGetDatum(tuple);

		SRF_RETURN_NEXT(funcctx, result);
	}
	else
	{
		/* do when there is no more left */
		SRF_RETURN_DONE(funcctx);
	}
}


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
	text	   *relname_text = PG_GETARG_TEXT_P(0);
	int2vector *pkattnums_arg = (int2vector *) PG_GETARG_POINTER(1);
	int32		pknumatts_arg = PG_GETARG_INT32(2);
	ArrayType  *src_pkattvals_arry = PG_GETARG_ARRAYTYPE_P(3);
	ArrayType  *tgt_pkattvals_arry = PG_GETARG_ARRAYTYPE_P(4);
	Relation	rel;
	int		   *pkattnums;
	int			pknumatts;
	char	  **src_pkattvals;
	char	  **tgt_pkattvals;
	int			src_nitems;
	int			tgt_nitems;
	char	   *sql;

	/*
	 * Open target relation.
	 */
	rel = get_rel_from_relname(relname_text, AccessShareLock, ACL_SELECT);

	/*
	 * Process pkattnums argument.
	 */
	validate_pkattnums(rel, pkattnums_arg, pknumatts_arg,
					   &pkattnums, &pknumatts);

	/*
	 * Source array is made up of key values that will be used to locate the
	 * tuple of interest from the local system.
	 */
	src_pkattvals = get_text_array_contents(src_pkattvals_arry, &src_nitems);

	/*
	 * There should be one source array key value for each key attnum
	 */
	if (src_nitems != pknumatts)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("source key array length must match number of key " \
						"attributes")));

	/*
	 * Target array is made up of key values that will be used to build the
	 * SQL string for use on the remote system.
	 */
	tgt_pkattvals = get_text_array_contents(tgt_pkattvals_arry, &tgt_nitems);

	/*
	 * There should be one target array key value for each key attnum
	 */
	if (tgt_nitems != pknumatts)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("target key array length must match number of key " \
						"attributes")));

	/*
	 * Prep work is finally done. Go get the SQL string.
	 */
	sql = get_sql_insert(rel, pkattnums, pknumatts, src_pkattvals, tgt_pkattvals);

	/*
	 * Now we can close the relation.
	 */
	relation_close(rel, AccessShareLock);

	/*
	 * And send it
	 */
	PG_RETURN_TEXT_P(cstring_to_text(sql));
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
	text	   *relname_text = PG_GETARG_TEXT_P(0);
	int2vector *pkattnums_arg = (int2vector *) PG_GETARG_POINTER(1);
	int32		pknumatts_arg = PG_GETARG_INT32(2);
	ArrayType  *tgt_pkattvals_arry = PG_GETARG_ARRAYTYPE_P(3);
	Relation	rel;
	int		   *pkattnums;
	int			pknumatts;
	char	  **tgt_pkattvals;
	int			tgt_nitems;
	char	   *sql;

	/*
	 * Open target relation.
	 */
	rel = get_rel_from_relname(relname_text, AccessShareLock, ACL_SELECT);

	/*
	 * Process pkattnums argument.
	 */
	validate_pkattnums(rel, pkattnums_arg, pknumatts_arg,
					   &pkattnums, &pknumatts);

	/*
	 * Target array is made up of key values that will be used to build the
	 * SQL string for use on the remote system.
	 */
	tgt_pkattvals = get_text_array_contents(tgt_pkattvals_arry, &tgt_nitems);

	/*
	 * There should be one target array key value for each key attnum
	 */
	if (tgt_nitems != pknumatts)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("target key array length must match number of key " \
						"attributes")));

	/*
	 * Prep work is finally done. Go get the SQL string.
	 */
	sql = get_sql_delete(rel, pkattnums, pknumatts, tgt_pkattvals);

	/*
	 * Now we can close the relation.
	 */
	relation_close(rel, AccessShareLock);

	/*
	 * And send it
	 */
	PG_RETURN_TEXT_P(cstring_to_text(sql));
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
	text	   *relname_text = PG_GETARG_TEXT_P(0);
	int2vector *pkattnums_arg = (int2vector *) PG_GETARG_POINTER(1);
	int32		pknumatts_arg = PG_GETARG_INT32(2);
	ArrayType  *src_pkattvals_arry = PG_GETARG_ARRAYTYPE_P(3);
	ArrayType  *tgt_pkattvals_arry = PG_GETARG_ARRAYTYPE_P(4);
	Relation	rel;
	int		   *pkattnums;
	int			pknumatts;
	char	  **src_pkattvals;
	char	  **tgt_pkattvals;
	int			src_nitems;
	int			tgt_nitems;
	char	   *sql;

	/*
	 * Open target relation.
	 */
	rel = get_rel_from_relname(relname_text, AccessShareLock, ACL_SELECT);

	/*
	 * Process pkattnums argument.
	 */
	validate_pkattnums(rel, pkattnums_arg, pknumatts_arg,
					   &pkattnums, &pknumatts);

	/*
	 * Source array is made up of key values that will be used to locate the
	 * tuple of interest from the local system.
	 */
	src_pkattvals = get_text_array_contents(src_pkattvals_arry, &src_nitems);

	/*
	 * There should be one source array key value for each key attnum
	 */
	if (src_nitems != pknumatts)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("source key array length must match number of key " \
						"attributes")));

	/*
	 * Target array is made up of key values that will be used to build the
	 * SQL string for use on the remote system.
	 */
	tgt_pkattvals = get_text_array_contents(tgt_pkattvals_arry, &tgt_nitems);

	/*
	 * There should be one target array key value for each key attnum
	 */
	if (tgt_nitems != pknumatts)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("target key array length must match number of key " \
						"attributes")));

	/*
	 * Prep work is finally done. Go get the SQL string.
	 */
	sql = get_sql_update(rel, pkattnums, pknumatts, src_pkattvals, tgt_pkattvals);

	/*
	 * Now we can close the relation.
	 */
	relation_close(rel, AccessShareLock);

	/*
	 * And send it
	 */
	PG_RETURN_TEXT_P(cstring_to_text(sql));
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
	/* This is now just an alias for the built-in function current_query() */
	PG_RETURN_DATUM(current_query(fcinfo));
}

/*
 * Retrieve async notifications for a connection.
 *
 * Returns a setof record of notifications, or an empty set if none received.
 * Can optionally take a named connection as parameter, but uses the unnamed
 * connection per default.
 *
 */
#define DBLINK_NOTIFY_COLS		3

PG_FUNCTION_INFO_V1(dblink_get_notify);
Datum
dblink_get_notify(PG_FUNCTION_ARGS)
{
	char	   *conname = NULL;
	PGconn	   *conn = NULL;
	remoteConn *rconn = NULL;
	PGnotify   *notify;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;

	prepTuplestoreResult(fcinfo);

	DBLINK_INIT;
	if (PG_NARGS() == 1)
		DBLINK_GET_NAMED_CONN;
	else
		conn = pconn->conn;

	/* create the tuplestore in per-query memory */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupdesc = CreateTemplateTupleDesc(DBLINK_NOTIFY_COLS, false);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "notify_name",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "be_pid",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "extra",
					   TEXTOID, -1, 0);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	PQconsumeInput(conn);
	while ((notify = PQnotifies(conn)) != NULL)
	{
		Datum		values[DBLINK_NOTIFY_COLS];
		bool		nulls[DBLINK_NOTIFY_COLS];

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		if (notify->relname != NULL)
			values[0] = CStringGetTextDatum(notify->relname);
		else
			nulls[0] = true;

		values[1] = Int32GetDatum(notify->be_pid);

		if (notify->extra != NULL)
			values[2] = CStringGetTextDatum(notify->extra);
		else
			nulls[2] = true;

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);

		PQfreemem(notify);
		PQconsumeInput(conn);
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

/*
 * Validate the options given to a dblink foreign server or user mapping.
 * Raise an error if any option is invalid.
 *
 * We just check the names of options here, so semantic errors in options,
 * such as invalid numeric format, will be detected at the attempt to connect.
 */
PG_FUNCTION_INFO_V1(dblink_fdw_validator);
Datum
dblink_fdw_validator(PG_FUNCTION_ARGS)
{
	List	   *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			context = PG_GETARG_OID(1);
	ListCell   *cell;

	static const PQconninfoOption *options = NULL;

	/*
	 * Get list of valid libpq options.
	 *
	 * To avoid unnecessary work, we get the list once and use it throughout
	 * the lifetime of this backend process.  We don't need to care about
	 * memory context issues, because PQconndefaults allocates with malloc.
	 */
	if (!options)
	{
		options = PQconndefaults();
		if (!options)			/* assume reason for failure is OOM */
			ereport(ERROR,
					(errcode(ERRCODE_FDW_OUT_OF_MEMORY),
					 errmsg("out of memory"),
			 errdetail("could not get libpq's default connection options")));
	}

	/* Validate each supplied option. */
	foreach(cell, options_list)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		if (!is_valid_dblink_option(options, def->defname, context))
		{
			/*
			 * Unknown option, or invalid option for the context specified, so
			 * complain about it.  Provide a hint with list of valid options
			 * for the context.
			 */
			StringInfoData buf;
			const PQconninfoOption *opt;

			initStringInfo(&buf);
			for (opt = options; opt->keyword; opt++)
			{
				if (is_valid_dblink_option(options, opt->keyword, context))
					appendStringInfo(&buf, "%s%s",
									 (buf.len > 0) ? ", " : "",
									 opt->keyword);
			}
			ereport(ERROR,
					(errcode(ERRCODE_FDW_OPTION_NAME_NOT_FOUND),
					 errmsg("invalid option \"%s\"", def->defname),
					 errhint("Valid options in this context are: %s",
							 buf.data)));
		}
	}

	PG_RETURN_VOID();
}


/*************************************************************
 * internal functions
 */


/*
 * get_pkey_attnames
 *
 * Get the primary key attnames for the given relation.
 * Return NULL, and set numatts = 0, if no primary key exists.
 */
static char **
get_pkey_attnames(Relation rel, int16 *numatts)
{
	Relation	indexRelation;
	ScanKeyData skey;
	SysScanDesc scan;
	HeapTuple	indexTuple;
	int			i;
	char	  **result = NULL;
	TupleDesc	tupdesc;

	/* initialize numatts to 0 in case no primary key exists */
	*numatts = 0;

	tupdesc = rel->rd_att;

	/* Prepare to scan pg_index for entries having indrelid = this rel. */
	indexRelation = heap_open(IndexRelationId, AccessShareLock);
	ScanKeyInit(&skey,
				Anum_pg_index_indrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));

	scan = systable_beginscan(indexRelation, IndexIndrelidIndexId, true,
							  NULL, 1, &skey);

	while (HeapTupleIsValid(indexTuple = systable_getnext(scan)))
	{
		Form_pg_index index = (Form_pg_index) GETSTRUCT(indexTuple);

		/* we're only interested if it is the primary key */
		if (index->indisprimary)
		{
			*numatts = index->indnatts;
			if (*numatts > 0)
			{
				result = (char **) palloc(*numatts * sizeof(char *));

				for (i = 0; i < *numatts; i++)
					result[i] = SPI_fname(tupdesc, index->indkey.values[i]);
			}
			break;
		}
	}

	systable_endscan(scan);
	heap_close(indexRelation, AccessShareLock);

	return result;
}

/*
 * Deconstruct a text[] into C-strings (note any NULL elements will be
 * returned as NULL pointers)
 */
static char **
get_text_array_contents(ArrayType *array, int *numitems)
{
	int			ndim = ARR_NDIM(array);
	int		   *dims = ARR_DIMS(array);
	int			nitems;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	char	  **values;
	char	   *ptr;
	bits8	   *bitmap;
	int			bitmask;
	int			i;

	Assert(ARR_ELEMTYPE(array) == TEXTOID);

	*numitems = nitems = ArrayGetNItems(ndim, dims);

	get_typlenbyvalalign(ARR_ELEMTYPE(array),
						 &typlen, &typbyval, &typalign);

	values = (char **) palloc(nitems * sizeof(char *));

	ptr = ARR_DATA_PTR(array);
	bitmap = ARR_NULLBITMAP(array);
	bitmask = 1;

	for (i = 0; i < nitems; i++)
	{
		if (bitmap && (*bitmap & bitmask) == 0)
		{
			values[i] = NULL;
		}
		else
		{
			values[i] = TextDatumGetCString(PointerGetDatum(ptr));
			ptr = att_addlength_pointer(ptr, typlen, ptr);
			ptr = (char *) att_align_nominal(ptr, typalign);
		}

		/* advance bitmap pointer if any */
		if (bitmap)
		{
			bitmask <<= 1;
			if (bitmask == 0x100)
			{
				bitmap++;
				bitmask = 1;
			}
		}
	}

	return values;
}

static char *
get_sql_insert(Relation rel, int *pkattnums, int pknumatts, char **src_pkattvals, char **tgt_pkattvals)
{
	char	   *relname;
	HeapTuple	tuple;
	TupleDesc	tupdesc;
	int			natts;
	StringInfoData buf;
	char	   *val;
	int			key;
	int			i;
	bool		needComma;

	initStringInfo(&buf);

	/* get relation name including any needed schema prefix and quoting */
	relname = generate_relation_name(rel);

	tupdesc = rel->rd_att;
	natts = tupdesc->natts;

	tuple = get_tuple_of_interest(rel, pkattnums, pknumatts, src_pkattvals);
	if (!tuple)
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("source row not found")));

	appendStringInfo(&buf, "INSERT INTO %s(", relname);

	needComma = false;
	for (i = 0; i < natts; i++)
	{
		if (tupdesc->attrs[i]->attisdropped)
			continue;

		if (needComma)
			appendStringInfoChar(&buf, ',');

		appendStringInfoString(&buf,
					  quote_ident_cstr(NameStr(tupdesc->attrs[i]->attname)));
		needComma = true;
	}

	appendStringInfoString(&buf, ") VALUES(");

	/*
	 * Note: i is physical column number (counting from 0).
	 */
	needComma = false;
	for (i = 0; i < natts; i++)
	{
		if (tupdesc->attrs[i]->attisdropped)
			continue;

		if (needComma)
			appendStringInfoChar(&buf, ',');

		key = get_attnum_pk_pos(pkattnums, pknumatts, i);

		if (key >= 0)
			val = tgt_pkattvals[key] ? pstrdup(tgt_pkattvals[key]) : NULL;
		else
			val = SPI_getvalue(tuple, tupdesc, i + 1);

		if (val != NULL)
		{
			appendStringInfoString(&buf, quote_literal_cstr(val));
			pfree(val);
		}
		else
			appendStringInfoString(&buf, "NULL");
		needComma = true;
	}
	appendStringInfoChar(&buf, ')');

	return (buf.data);
}

static char *
get_sql_delete(Relation rel, int *pkattnums, int pknumatts, char **tgt_pkattvals)
{
	char	   *relname;
	TupleDesc	tupdesc;
	StringInfoData buf;
	int			i;

	initStringInfo(&buf);

	/* get relation name including any needed schema prefix and quoting */
	relname = generate_relation_name(rel);

	tupdesc = rel->rd_att;

	appendStringInfo(&buf, "DELETE FROM %s WHERE ", relname);
	for (i = 0; i < pknumatts; i++)
	{
		int			pkattnum = pkattnums[i];

		if (i > 0)
			appendStringInfoString(&buf, " AND ");

		appendStringInfoString(&buf,
			   quote_ident_cstr(NameStr(tupdesc->attrs[pkattnum]->attname)));

		if (tgt_pkattvals[i] != NULL)
			appendStringInfo(&buf, " = %s",
							 quote_literal_cstr(tgt_pkattvals[i]));
		else
			appendStringInfoString(&buf, " IS NULL");
	}

	return (buf.data);
}

static char *
get_sql_update(Relation rel, int *pkattnums, int pknumatts, char **src_pkattvals, char **tgt_pkattvals)
{
	char	   *relname;
	HeapTuple	tuple;
	TupleDesc	tupdesc;
	int			natts;
	StringInfoData buf;
	char	   *val;
	int			key;
	int			i;
	bool		needComma;

	initStringInfo(&buf);

	/* get relation name including any needed schema prefix and quoting */
	relname = generate_relation_name(rel);

	tupdesc = rel->rd_att;
	natts = tupdesc->natts;

	tuple = get_tuple_of_interest(rel, pkattnums, pknumatts, src_pkattvals);
	if (!tuple)
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("source row not found")));

	appendStringInfo(&buf, "UPDATE %s SET ", relname);

	/*
	 * Note: i is physical column number (counting from 0).
	 */
	needComma = false;
	for (i = 0; i < natts; i++)
	{
		if (tupdesc->attrs[i]->attisdropped)
			continue;

		if (needComma)
			appendStringInfoString(&buf, ", ");

		appendStringInfo(&buf, "%s = ",
					  quote_ident_cstr(NameStr(tupdesc->attrs[i]->attname)));

		key = get_attnum_pk_pos(pkattnums, pknumatts, i);

		if (key >= 0)
			val = tgt_pkattvals[key] ? pstrdup(tgt_pkattvals[key]) : NULL;
		else
			val = SPI_getvalue(tuple, tupdesc, i + 1);

		if (val != NULL)
		{
			appendStringInfoString(&buf, quote_literal_cstr(val));
			pfree(val);
		}
		else
			appendStringInfoString(&buf, "NULL");
		needComma = true;
	}

	appendStringInfoString(&buf, " WHERE ");

	for (i = 0; i < pknumatts; i++)
	{
		int			pkattnum = pkattnums[i];

		if (i > 0)
			appendStringInfoString(&buf, " AND ");

		appendStringInfoString(&buf,
			   quote_ident_cstr(NameStr(tupdesc->attrs[pkattnum]->attname)));

		val = tgt_pkattvals[i];

		if (val != NULL)
			appendStringInfo(&buf, " = %s", quote_literal_cstr(val));
		else
			appendStringInfoString(&buf, " IS NULL");
	}

	return (buf.data);
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

	rawstr_text = cstring_to_text(rawstr);
	result_text = DatumGetTextP(DirectFunctionCall1(quote_ident,
											  PointerGetDatum(rawstr_text)));
	result = text_to_cstring(result_text);

	return result;
}

static int
get_attnum_pk_pos(int *pkattnums, int pknumatts, int key)
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
get_tuple_of_interest(Relation rel, int *pkattnums, int pknumatts, char **src_pkattvals)
{
	char	   *relname;
	TupleDesc	tupdesc;
	int			natts;
	StringInfoData buf;
	int			ret;
	HeapTuple	tuple;
	int			i;

	/*
	 * Connect to SPI manager
	 */
	if ((ret = SPI_connect()) < 0)
		/* internal error */
		elog(ERROR, "SPI connect failure - returned %d", ret);

	initStringInfo(&buf);

	/* get relation name including any needed schema prefix and quoting */
	relname = generate_relation_name(rel);

	tupdesc = rel->rd_att;
	natts = tupdesc->natts;

	/*
	 * Build sql statement to look up tuple of interest, ie, the one matching
	 * src_pkattvals.  We used to use "SELECT *" here, but it's simpler to
	 * generate a result tuple that matches the table's physical structure,
	 * with NULLs for any dropped columns.  Otherwise we have to deal with two
	 * different tupdescs and everything's very confusing.
	 */
	appendStringInfoString(&buf, "SELECT ");

	for (i = 0; i < natts; i++)
	{
		if (i > 0)
			appendStringInfoString(&buf, ", ");

		if (tupdesc->attrs[i]->attisdropped)
			appendStringInfoString(&buf, "NULL");
		else
			appendStringInfoString(&buf,
					  quote_ident_cstr(NameStr(tupdesc->attrs[i]->attname)));
	}

	appendStringInfo(&buf, " FROM %s WHERE ", relname);

	for (i = 0; i < pknumatts; i++)
	{
		int			pkattnum = pkattnums[i];

		if (i > 0)
			appendStringInfoString(&buf, " AND ");

		appendStringInfoString(&buf,
			   quote_ident_cstr(NameStr(tupdesc->attrs[pkattnum]->attname)));

		if (src_pkattvals[i] != NULL)
			appendStringInfo(&buf, " = %s",
							 quote_literal_cstr(src_pkattvals[i]));
		else
			appendStringInfoString(&buf, " IS NULL");
	}

	/*
	 * Retrieve the desired tuple
	 */
	ret = SPI_exec(buf.data, 0);
	pfree(buf.data);

	/*
	 * Only allow one qualifying tuple
	 */
	if ((ret == SPI_OK_SELECT) && (SPI_processed > 1))
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("source criteria matched more than one record")));

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

/*
 * Open the relation named by relname_text, acquire specified type of lock,
 * verify we have specified permissions.
 * Caller must close rel when done with it.
 */
static Relation
get_rel_from_relname(text *relname_text, LOCKMODE lockmode, AclMode aclmode)
{
	RangeVar   *relvar;
	Relation	rel;
	AclResult	aclresult;

	relvar = makeRangeVarFromNameList(textToQualifiedNameList(relname_text));
	rel = heap_openrv(relvar, lockmode);

	aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(),
								  aclmode);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_CLASS,
					   RelationGetRelationName(rel));

	return rel;
}

/*
 * generate_relation_name - copied from ruleutils.c
 *		Compute the name to display for a relation
 *
 * The result includes all necessary quoting and schema-prefixing.
 */
static char *
generate_relation_name(Relation rel)
{
	char	   *nspname;
	char	   *result;

	/* Qualify the name if not visible in search path */
	if (RelationIsVisible(RelationGetRelid(rel)))
		nspname = NULL;
	else
		nspname = get_namespace_name(rel->rd_rel->relnamespace);

	result = quote_qualified_identifier(nspname, RelationGetRelationName(rel));

	return result;
}


static remoteConn *
getConnectionByName(const char *name)
{
	remoteConnHashEnt *hentry;
	char	   *key;

	if (!remoteConnHash)
		remoteConnHash = createConnHash();

	key = pstrdup(name);
	truncate_identifier(key, strlen(key), false);
	hentry = (remoteConnHashEnt *) hash_search(remoteConnHash,
											   key, HASH_FIND, NULL);

	if (hentry)
		return (hentry->rconn);

	return (NULL);
}

static HTAB *
createConnHash(void)
{
	HASHCTL		ctl;

	ctl.keysize = NAMEDATALEN;
	ctl.entrysize = sizeof(remoteConnHashEnt);

	return hash_create("Remote Con hash", NUMCONN, &ctl, HASH_ELEM);
}

static void
createNewConnection(const char *name, remoteConn *rconn)
{
	remoteConnHashEnt *hentry;
	bool		found;
	char	   *key;

	if (!remoteConnHash)
		remoteConnHash = createConnHash();

	key = pstrdup(name);
	truncate_identifier(key, strlen(key), true);
	hentry = (remoteConnHashEnt *) hash_search(remoteConnHash, key,
											   HASH_ENTER, &found);

	if (found)
	{
		PQfinish(rconn->conn);
		pfree(rconn);

		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("duplicate connection name")));
	}

	hentry->rconn = rconn;
	strlcpy(hentry->name, name, sizeof(hentry->name));
}

static void
deleteConnection(const char *name)
{
	remoteConnHashEnt *hentry;
	bool		found;
	char	   *key;

	if (!remoteConnHash)
		remoteConnHash = createConnHash();

	key = pstrdup(name);
	truncate_identifier(key, strlen(key), false);
	hentry = (remoteConnHashEnt *) hash_search(remoteConnHash,
											   key, HASH_REMOVE, &found);

	if (!hentry)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("undefined connection name")));

}

static void
dblink_security_check(PGconn *conn, remoteConn *rconn)
{
	if (!superuser())
	{
		if (!PQconnectionUsedPassword(conn))
		{
			PQfinish(conn);
			if (rconn)
				pfree(rconn);

			ereport(ERROR,
				  (errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
				   errmsg("password is required"),
				   errdetail("Non-superuser cannot connect if the server does not request a password."),
				   errhint("Target server's authentication method must be changed.")));
		}
	}
}

/*
 * For non-superusers, insist that the connstr specify a password.  This
 * prevents a password from being picked up from .pgpass, a service file,
 * the environment, etc.  We don't want the postgres user's passwords
 * to be accessible to non-superusers.
 */
static void
dblink_connstr_check(const char *connstr)
{
	if (!superuser())
	{
		PQconninfoOption *options;
		PQconninfoOption *option;
		bool		connstr_gives_password = false;

		options = PQconninfoParse(connstr, NULL);
		if (options)
		{
			for (option = options; option->keyword != NULL; option++)
			{
				if (strcmp(option->keyword, "password") == 0)
				{
					if (option->val != NULL && option->val[0] != '\0')
					{
						connstr_gives_password = true;
						break;
					}
				}
			}
			PQconninfoFree(options);
		}

		if (!connstr_gives_password)
			ereport(ERROR,
				  (errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
				   errmsg("password is required"),
				   errdetail("Non-superusers must provide a password in the connection string.")));
	}
}

static void
dblink_res_error(const char *conname, PGresult *res, const char *dblink_context_msg, bool fail)
{
	int			level;
	char	   *pg_diag_sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
	char	   *pg_diag_message_primary = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
	char	   *pg_diag_message_detail = PQresultErrorField(res, PG_DIAG_MESSAGE_DETAIL);
	char	   *pg_diag_message_hint = PQresultErrorField(res, PG_DIAG_MESSAGE_HINT);
	char	   *pg_diag_context = PQresultErrorField(res, PG_DIAG_CONTEXT);
	int			sqlstate;
	char	   *message_primary;
	char	   *message_detail;
	char	   *message_hint;
	char	   *message_context;
	const char *dblink_context_conname = "unnamed";

	if (fail)
		level = ERROR;
	else
		level = NOTICE;

	if (pg_diag_sqlstate)
		sqlstate = MAKE_SQLSTATE(pg_diag_sqlstate[0],
								 pg_diag_sqlstate[1],
								 pg_diag_sqlstate[2],
								 pg_diag_sqlstate[3],
								 pg_diag_sqlstate[4]);
	else
		sqlstate = ERRCODE_CONNECTION_FAILURE;

	xpstrdup(message_primary, pg_diag_message_primary);
	xpstrdup(message_detail, pg_diag_message_detail);
	xpstrdup(message_hint, pg_diag_message_hint);
	xpstrdup(message_context, pg_diag_context);

	if (res)
		PQclear(res);

	if (conname)
		dblink_context_conname = conname;

	ereport(level,
			(errcode(sqlstate),
			 message_primary ? errmsg_internal("%s", message_primary) :
			 errmsg("unknown error"),
			 message_detail ? errdetail_internal("%s", message_detail) : 0,
			 message_hint ? errhint("%s", message_hint) : 0,
			 message_context ? errcontext("%s", message_context) : 0,
		  errcontext("Error occurred on dblink connection named \"%s\": %s.",
					 dblink_context_conname, dblink_context_msg)));
}

/*
 * Obtain connection string for a foreign server
 */
static char *
get_connect_string(const char *servername)
{
	ForeignServer *foreign_server = NULL;
	UserMapping *user_mapping;
	ListCell   *cell;
	StringInfo	buf = makeStringInfo();
	ForeignDataWrapper *fdw;
	AclResult	aclresult;
	char	   *srvname;

	/* first gather the server connstr options */
	srvname = pstrdup(servername);
	truncate_identifier(srvname, strlen(srvname), false);
	foreign_server = GetForeignServerByName(srvname, true);

	if (foreign_server)
	{
		Oid			serverid = foreign_server->serverid;
		Oid			fdwid = foreign_server->fdwid;
		Oid			userid = GetUserId();

		user_mapping = GetUserMapping(userid, serverid);
		fdw = GetForeignDataWrapper(fdwid);

		/* Check permissions, user must have usage on the server. */
		aclresult = pg_foreign_server_aclcheck(serverid, userid, ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_FOREIGN_SERVER, foreign_server->servername);

		foreach(cell, fdw->options)
		{
			DefElem    *def = lfirst(cell);

			appendStringInfo(buf, "%s='%s' ", def->defname,
							 escape_param_str(strVal(def->arg)));
		}

		foreach(cell, foreign_server->options)
		{
			DefElem    *def = lfirst(cell);

			appendStringInfo(buf, "%s='%s' ", def->defname,
							 escape_param_str(strVal(def->arg)));
		}

		foreach(cell, user_mapping->options)
		{

			DefElem    *def = lfirst(cell);

			appendStringInfo(buf, "%s='%s' ", def->defname,
							 escape_param_str(strVal(def->arg)));
		}

		return buf->data;
	}
	else
		return NULL;
}

/*
 * Escaping libpq connect parameter strings.
 *
 * Replaces "'" with "\'" and "\" with "\\".
 */
static char *
escape_param_str(const char *str)
{
	const char *cp;
	StringInfo	buf = makeStringInfo();

	for (cp = str; *cp; cp++)
	{
		if (*cp == '\\' || *cp == '\'')
			appendStringInfoChar(buf, '\\');
		appendStringInfoChar(buf, *cp);
	}

	return buf->data;
}

/*
 * Validate the PK-attnums argument for dblink_build_sql_insert() and related
 * functions, and translate to the internal representation.
 *
 * The user supplies an int2vector of 1-based logical attnums, plus a count
 * argument (the need for the separate count argument is historical, but we
 * still check it).  We check that each attnum corresponds to a valid,
 * non-dropped attribute of the rel.  We do *not* prevent attnums from being
 * listed twice, though the actual use-case for such things is dubious.
 * Note that before Postgres 9.0, the user's attnums were interpreted as
 * physical not logical column numbers; this was changed for future-proofing.
 *
 * The internal representation is a palloc'd int array of 0-based physical
 * attnums.
 */
static void
validate_pkattnums(Relation rel,
				   int2vector *pkattnums_arg, int32 pknumatts_arg,
				   int **pkattnums, int *pknumatts)
{
	TupleDesc	tupdesc = rel->rd_att;
	int			natts = tupdesc->natts;
	int			i;

	/* Don't take more array elements than there are */
	pknumatts_arg = Min(pknumatts_arg, pkattnums_arg->dim1);

	/* Must have at least one pk attnum selected */
	if (pknumatts_arg <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("number of key attributes must be > 0")));

	/* Allocate output array */
	*pkattnums = (int *) palloc(pknumatts_arg * sizeof(int));
	*pknumatts = pknumatts_arg;

	/* Validate attnums and convert to internal form */
	for (i = 0; i < pknumatts_arg; i++)
	{
		int			pkattnum = pkattnums_arg->values[i];
		int			lnum;
		int			j;

		/* Can throw error immediately if out of range */
		if (pkattnum <= 0 || pkattnum > natts)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid attribute number %d", pkattnum)));

		/* Identify which physical column has this logical number */
		lnum = 0;
		for (j = 0; j < natts; j++)
		{
			/* dropped columns don't count */
			if (tupdesc->attrs[j]->attisdropped)
				continue;

			if (++lnum == pkattnum)
				break;
		}

		if (j < natts)
			(*pkattnums)[i] = j;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid attribute number %d", pkattnum)));
	}
}

/*
 * Check if the specified connection option is valid.
 *
 * We basically allow whatever libpq thinks is an option, with these
 * restrictions:
 *		debug options: disallowed
 *		"client_encoding": disallowed
 *		"user": valid only in USER MAPPING options
 *		secure options (eg password): valid only in USER MAPPING options
 *		others: valid only in FOREIGN SERVER options
 *
 * We disallow client_encoding because it would be overridden anyway via
 * PQclientEncoding; allowing it to be specified would merely promote
 * confusion.
 */
static bool
is_valid_dblink_option(const PQconninfoOption *options, const char *option,
					   Oid context)
{
	const PQconninfoOption *opt;

	/* Look up the option in libpq result */
	for (opt = options; opt->keyword; opt++)
	{
		if (strcmp(opt->keyword, option) == 0)
			break;
	}
	if (opt->keyword == NULL)
		return false;

	/* Disallow debug options (particularly "replication") */
	if (strchr(opt->dispchar, 'D'))
		return false;

	/* Disallow "client_encoding" */
	if (strcmp(opt->keyword, "client_encoding") == 0)
		return false;

	/*
	 * If the option is "user" or marked secure, it should be specified only
	 * in USER MAPPING.  Others should be specified only in SERVER.
	 */
	if (strcmp(opt->keyword, "user") == 0 || strchr(opt->dispchar, '*'))
	{
		if (context != UserMappingRelationId)
			return false;
	}
	else
	{
		if (context != ForeignServerRelationId)
			return false;
	}

	return true;
}

/*
 * Copy the remote session's values of GUCs that affect datatype I/O
 * and apply them locally in a new GUC nesting level.  Returns the new
 * nestlevel (which is needed by restoreLocalGucs to undo the settings),
 * or -1 if no new nestlevel was needed.
 *
 * We use the equivalent of a function SET option to allow the settings to
 * persist only until the caller calls restoreLocalGucs.  If an error is
 * thrown in between, guc.c will take care of undoing the settings.
 */
static int
applyRemoteGucs(PGconn *conn)
{
	static const char *const GUCsAffectingIO[] = {
		"DateStyle",
		"IntervalStyle"
	};

	int			nestlevel = -1;
	int			i;

	for (i = 0; i < lengthof(GUCsAffectingIO); i++)
	{
		const char *gucName = GUCsAffectingIO[i];
		const char *remoteVal = PQparameterStatus(conn, gucName);
		const char *localVal;

		/*
		 * If the remote server is pre-8.4, it won't have IntervalStyle, but
		 * that's okay because its output format won't be ambiguous.  So just
		 * skip the GUC if we don't get a value for it.  (We might eventually
		 * need more complicated logic with remote-version checks here.)
		 */
		if (remoteVal == NULL)
			continue;

		/*
		 * Avoid GUC-setting overhead if the remote and local GUCs already
		 * have the same value.
		 */
		localVal = GetConfigOption(gucName, false, false);
		Assert(localVal != NULL);

		if (strcmp(remoteVal, localVal) == 0)
			continue;

		/* Create new GUC nest level if we didn't already */
		if (nestlevel < 0)
			nestlevel = NewGUCNestLevel();

		/* Apply the option (this will throw error on failure) */
		(void) set_config_option(gucName, remoteVal,
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, 0, false);
	}

	return nestlevel;
}

/*
 * Restore local GUCs after they have been overlaid with remote settings.
 */
static void
restoreLocalGucs(int nestlevel)
{
	/* Do nothing if no new nestlevel was created */
	if (nestlevel > 0)
		AtEOXact_GUC(true, nestlevel);
}
