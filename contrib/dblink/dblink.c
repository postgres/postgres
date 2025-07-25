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
 * Copyright (c) 2001-2025, PostgreSQL Global Development Group
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

#include "access/htup_details.h"
#include "access/relation.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_type.h"
#include "catalog/pg_user_mapping.h"
#include "commands/defrem.h"
#include "common/base64.h"
#include "executor/spi.h"
#include "foreign/foreign.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "libpq-fe.h"
#include "libpq/libpq-be.h"
#include "libpq/libpq-be-fe-helpers.h"
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
#include "utils/varlena.h"
#include "utils/wait_event.h"

PG_MODULE_MAGIC_EXT(
					.name = "dblink",
					.version = PG_VERSION
);

typedef struct remoteConn
{
	PGconn	   *conn;			/* Hold the remote connection */
	int			openCursorCount;	/* The number of open cursors */
	bool		newXactForCursor;	/* Opened a transaction for a cursor */
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
static PGresult *storeQueryResult(storeInfo *sinfo, PGconn *conn, const char *sql);
static void storeRow(storeInfo *sinfo, PGresult *res, bool first);
static remoteConn *getConnectionByName(const char *name);
static HTAB *createConnHash(void);
static remoteConn *createNewConnection(const char *name);
static void deleteConnection(const char *name);
static char **get_pkey_attnames(Relation rel, int16 *indnkeyatts);
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
static bool dblink_connstr_has_pw(const char *connstr);
static void dblink_security_check(PGconn *conn, const char *connname,
								  const char *connstr);
static void dblink_res_error(PGconn *conn, const char *conname, PGresult *res,
							 bool fail, const char *fmt,...) pg_attribute_printf(5, 6);
static char *get_connect_string(const char *servername);
static char *escape_param_str(const char *str);
static void validate_pkattnums(Relation rel,
							   int2vector *pkattnums_arg, int32 pknumatts_arg,
							   int **pkattnums, int *pknumatts);
static bool is_valid_dblink_option(const PQconninfoOption *options,
								   const char *option, Oid context);
static int	applyRemoteGucs(PGconn *conn);
static void restoreLocalGucs(int nestlevel);
static bool UseScramPassthrough(ForeignServer *foreign_server, UserMapping *user);
static void appendSCRAMKeysInfo(StringInfo buf);
static bool is_valid_dblink_fdw_option(const PQconninfoOption *options, const char *option,
									   Oid context);
static bool dblink_connstr_has_required_scram_options(const char *connstr);

/* Global */
static remoteConn *pconn = NULL;
static HTAB *remoteConnHash = NULL;

/* custom wait event values, retrieved from shared memory */
static uint32 dblink_we_connect = 0;
static uint32 dblink_we_get_conn = 0;
static uint32 dblink_we_get_result = 0;

/*
 *	Following is hash that holds multiple remote connections.
 *	Calling convention of each dblink function changes to accept
 *	connection name as the first parameter. The connection hash is
 *	much like ecpg e.g. a mapping between a name and a PGconn object.
 *
 *	To avoid potentially leaking a PGconn object in case of out-of-memory
 *	errors, we first create the hash entry, then open the PGconn.
 *	Hence, a hash entry whose rconn.conn pointer is NULL must be
 *	understood as a leftover from a failed create; it should be ignored
 *	by lookup operations, and silently replaced by create operations.
 */

typedef struct remoteConnHashEnt
{
	char		name[NAMEDATALEN];
	remoteConn	rconn;
} remoteConnHashEnt;

/* initial number of connection hashes */
#define NUMCONN 16

pg_noreturn static void
dblink_res_internalerror(PGconn *conn, PGresult *res, const char *p2)
{
	char	   *msg = pchomp(PQerrorMessage(conn));

	PQclear(res);
	elog(ERROR, "%s: %s", p2, msg);
}

pg_noreturn static void
dblink_conn_not_avail(const char *conname)
{
	if (conname)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST),
				 errmsg("connection \"%s\" not available", conname)));
	else
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST),
				 errmsg("connection not available")));
}

static void
dblink_get_conn(char *conname_or_str,
				PGconn *volatile *conn_p, char **conname_p, volatile bool *freeconn_p)
{
	remoteConn *rconn = getConnectionByName(conname_or_str);
	PGconn	   *conn;
	char	   *conname;
	bool		freeconn;

	if (rconn)
	{
		conn = rconn->conn;
		conname = conname_or_str;
		freeconn = false;
	}
	else
	{
		const char *connstr;

		connstr = get_connect_string(conname_or_str);
		if (connstr == NULL)
			connstr = conname_or_str;
		dblink_connstr_check(connstr);

		/* first time, allocate or get the custom wait event */
		if (dblink_we_get_conn == 0)
			dblink_we_get_conn = WaitEventExtensionNew("DblinkGetConnect");

		/* OK to make connection */
		conn = libpqsrv_connect(connstr, dblink_we_get_conn);

		if (PQstatus(conn) == CONNECTION_BAD)
		{
			char	   *msg = pchomp(PQerrorMessage(conn));

			libpqsrv_disconnect(conn);
			ereport(ERROR,
					(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
					 errmsg("could not establish connection"),
					 errdetail_internal("%s", msg)));
		}

		PQsetNoticeReceiver(conn, libpqsrv_notice_receiver,
							"received message via remote connection");

		dblink_security_check(conn, NULL, connstr);
		if (PQclientEncoding(conn) != GetDatabaseEncoding())
			PQsetClientEncoding(conn, GetDatabaseEncodingName());
		freeconn = true;
		conname = NULL;
	}

	*conn_p = conn;
	*conname_p = conname;
	*freeconn_p = freeconn;
}

static PGconn *
dblink_get_named_conn(const char *conname)
{
	remoteConn *rconn = getConnectionByName(conname);

	if (rconn)
		return rconn->conn;

	dblink_conn_not_avail(conname);
	return NULL;				/* keep compiler quiet */
}

static void
dblink_init(void)
{
	if (!pconn)
	{
		if (dblink_we_get_result == 0)
			dblink_we_get_result = WaitEventExtensionNew("DblinkGetResult");

		pconn = (remoteConn *) MemoryContextAlloc(TopMemoryContext, sizeof(remoteConn));
		pconn->conn = NULL;
		pconn->openCursorCount = 0;
		pconn->newXactForCursor = false;
	}
}

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

	dblink_init();

	if (PG_NARGS() == 2)
	{
		conname_or_str = text_to_cstring(PG_GETARG_TEXT_PP(1));
		connname = text_to_cstring(PG_GETARG_TEXT_PP(0));
	}
	else if (PG_NARGS() == 1)
		conname_or_str = text_to_cstring(PG_GETARG_TEXT_PP(0));

	/* first check for valid foreign data server */
	connstr = get_connect_string(conname_or_str);
	if (connstr == NULL)
		connstr = conname_or_str;

	/* check password in connection string if not superuser */
	dblink_connstr_check(connstr);

	/* first time, allocate or get the custom wait event */
	if (dblink_we_connect == 0)
		dblink_we_connect = WaitEventExtensionNew("DblinkConnect");

	/* if we need a hashtable entry, make that first, since it might fail */
	if (connname)
	{
		rconn = createNewConnection(connname);
		Assert(rconn->conn == NULL);
	}

	/* OK to make connection */
	conn = libpqsrv_connect(connstr, dblink_we_connect);

	if (PQstatus(conn) == CONNECTION_BAD)
	{
		msg = pchomp(PQerrorMessage(conn));
		libpqsrv_disconnect(conn);
		if (connname)
			deleteConnection(connname);

		ereport(ERROR,
				(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
				 errmsg("could not establish connection"),
				 errdetail_internal("%s", msg)));
	}

	PQsetNoticeReceiver(conn, libpqsrv_notice_receiver,
						"received message via remote connection");

	/* check password actually used if not superuser */
	dblink_security_check(conn, connname, connstr);

	/* attempt to set client encoding to match server encoding, if needed */
	if (PQclientEncoding(conn) != GetDatabaseEncoding())
		PQsetClientEncoding(conn, GetDatabaseEncodingName());

	/* all OK, save away the conn */
	if (connname)
	{
		rconn->conn = conn;
	}
	else
	{
		if (pconn->conn)
			libpqsrv_disconnect(pconn->conn);
		pconn->conn = conn;
	}

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

	dblink_init();

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
		dblink_conn_not_avail(conname);

	libpqsrv_disconnect(conn);
	if (rconn)
		deleteConnection(conname);
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
	PGresult   *res = NULL;
	PGconn	   *conn;
	char	   *curname = NULL;
	char	   *sql = NULL;
	char	   *conname = NULL;
	StringInfoData buf;
	remoteConn *rconn = NULL;
	bool		fail = true;	/* default to backward compatible behavior */

	dblink_init();
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
		dblink_conn_not_avail(conname);

	conn = rconn->conn;

	/* If we are not in a transaction, start one */
	if (PQtransactionStatus(conn) == PQTRANS_IDLE)
	{
		res = libpqsrv_exec(conn, "BEGIN", dblink_we_get_result);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			dblink_res_internalerror(conn, res, "begin error");
		PQclear(res);
		rconn->newXactForCursor = true;

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
	res = libpqsrv_exec(conn, buf.data, dblink_we_get_result);
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		dblink_res_error(conn, conname, res, fail,
						 "while opening cursor \"%s\"", curname);
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
	PGconn	   *conn;
	PGresult   *res = NULL;
	char	   *curname = NULL;
	char	   *conname = NULL;
	StringInfoData buf;
	remoteConn *rconn = NULL;
	bool		fail = true;	/* default to backward compatible behavior */

	dblink_init();
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
		dblink_conn_not_avail(conname);

	conn = rconn->conn;

	appendStringInfo(&buf, "CLOSE %s", curname);

	/* close the cursor */
	res = libpqsrv_exec(conn, buf.data, dblink_we_get_result);
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		dblink_res_error(conn, conname, res, fail,
						 "while closing cursor \"%s\"", curname);
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
			rconn->newXactForCursor = false;

			res = libpqsrv_exec(conn, "COMMIT", dblink_we_get_result);
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
				dblink_res_internalerror(conn, res, "commit error");
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

	dblink_init();

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
		dblink_conn_not_avail(conname);

	initStringInfo(&buf);
	appendStringInfo(&buf, "FETCH %d FROM %s", howmany, curname);

	/*
	 * Try to execute the query.  Note that since libpq uses malloc, the
	 * PGresult will be long-lived even though we are still in a short-lived
	 * memory context.
	 */
	res = libpqsrv_exec(conn, buf.data, dblink_we_get_result);
	if (!res ||
		(PQresultStatus(res) != PGRES_COMMAND_OK &&
		 PQresultStatus(res) != PGRES_TUPLES_OK))
	{
		dblink_res_error(conn, conname, res, fail,
						 "while fetching from cursor \"%s\"", curname);
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
	PGconn	   *conn;
	char	   *sql;
	int			retval;

	if (PG_NARGS() == 2)
	{
		conn = dblink_get_named_conn(text_to_cstring(PG_GETARG_TEXT_PP(0)));
		sql = text_to_cstring(PG_GETARG_TEXT_PP(1));
	}
	else
		/* shouldn't happen */
		elog(ERROR, "wrong number of arguments");

	/* async query send */
	retval = PQsendQuery(conn, sql);
	if (retval != 1)
		elog(NOTICE, "could not send query: %s", pchomp(PQerrorMessage(conn)));

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

	dblink_init();

	PG_TRY();
	{
		char	   *sql = NULL;
		char	   *conname = NULL;
		bool		fail = true;	/* default to backward compatible */

		if (!is_async)
		{
			if (PG_NARGS() == 3)
			{
				/* text,text,bool */
				conname = text_to_cstring(PG_GETARG_TEXT_PP(0));
				sql = text_to_cstring(PG_GETARG_TEXT_PP(1));
				fail = PG_GETARG_BOOL(2);
				dblink_get_conn(conname, &conn, &conname, &freeconn);
			}
			else if (PG_NARGS() == 2)
			{
				/* text,text or text,bool */
				if (get_fn_expr_argtype(fcinfo->flinfo, 1) == BOOLOID)
				{
					sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
					fail = PG_GETARG_BOOL(1);
					conn = pconn->conn;
				}
				else
				{
					conname = text_to_cstring(PG_GETARG_TEXT_PP(0));
					sql = text_to_cstring(PG_GETARG_TEXT_PP(1));
					dblink_get_conn(conname, &conn, &conname, &freeconn);
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
		else					/* is_async */
		{
			/* get async result */
			conname = text_to_cstring(PG_GETARG_TEXT_PP(0));

			if (PG_NARGS() == 2)
			{
				/* text,bool */
				fail = PG_GETARG_BOOL(1);
				conn = dblink_get_named_conn(conname);
			}
			else if (PG_NARGS() == 1)
			{
				/* text */
				conn = dblink_get_named_conn(conname);
			}
			else
				/* shouldn't happen */
				elog(ERROR, "wrong number of arguments");
		}

		if (!conn)
			dblink_conn_not_avail(conname);

		if (!is_async)
		{
			/* synchronous query, use efficient tuple collection method */
			materializeQueryResult(fcinfo, conn, conname, sql, fail);
		}
		else
		{
			/* async result retrieval, do it the old way */
			PGresult   *res = libpqsrv_get_result(conn, dblink_we_get_result);

			/* NULL means we're all done with the async results */
			if (res)
			{
				if (PQresultStatus(res) != PGRES_COMMAND_OK &&
					PQresultStatus(res) != PGRES_TUPLES_OK)
				{
					dblink_res_error(conn, conname, res, fail,
									 "while executing query");
					/* if fail isn't set, we'll return an empty query result */
				}
				else
				{
					materializeResult(fcinfo, conn, res);
				}
			}
		}
	}
	PG_FINALLY();
	{
		/* if needed, close the connection to the database */
		if (freeconn)
			libpqsrv_disconnect(conn);
	}
	PG_END_TRY();

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
	TupleDesc	tupdesc;
	bool		is_sql_cmd;
	int			ntuples;
	int			nfields;

	/* prepTuplestoreResult must have been called previously */
	Assert(rsinfo->returnMode == SFRM_Materialize);

	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		is_sql_cmd = true;

		/*
		 * need a tuple descriptor representing one TEXT column to return the
		 * command status string as our result tuple
		 */
		tupdesc = CreateTemplateTupleDesc(1);
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

		oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
		tupstore = tuplestore_begin_heap(true, false, work_mem);
		rsinfo->setResult = tupstore;
		rsinfo->setDesc = tupdesc;
		MemoryContextSwitchTo(oldcontext);

		values = palloc_array(char *, nfields);

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
	}

	PQclear(res);
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

	/* prepTuplestoreResult must have been called previously */
	Assert(rsinfo->returnMode == SFRM_Materialize);

	/* Use a PG_TRY block to ensure we pump libpq dry of results */
	PG_TRY();
	{
		storeInfo	sinfo = {0};
		PGresult   *res;

		sinfo.fcinfo = fcinfo;
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
			dblink_res_error(conn, conname, res, fail,
							 "while executing query");
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
			tupdesc = CreateTemplateTupleDesc(1);
			TupleDescInitEntry(tupdesc, (AttrNumber) 1, "status",
							   TEXTOID, -1, 0);
			attinmeta = TupleDescGetAttInMetadata(tupdesc);

			oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
			tupstore = tuplestore_begin_heap(true, false, work_mem);
			rsinfo->setResult = tupstore;
			rsinfo->setDesc = tupdesc;
			MemoryContextSwitchTo(oldcontext);

			values[0] = PQcmdStatus(res);

			/* build the tuple and put it into the tuplestore. */
			tuple = BuildTupleFromCStrings(attinmeta, values);
			tuplestore_puttuple(tupstore, tuple);

			PQclear(res);
		}
		else
		{
			Assert(PQresultStatus(res) == PGRES_TUPLES_OK);
			/* storeRow should have created a tuplestore */
			Assert(rsinfo->setResult != NULL);

			PQclear(res);
		}

		/* clean up data conversion short-lived memory context */
		if (sinfo.tmpcontext != NULL)
			MemoryContextDelete(sinfo.tmpcontext);

		PQclear(sinfo.last_res);
		PQclear(sinfo.cur_res);
	}
	PG_CATCH();
	{
		PGresult   *res;

		/* be sure to clear out any pending data in libpq */
		while ((res = libpqsrv_get_result(conn, dblink_we_get_result)) !=
			   NULL)
			PQclear(res);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * Execute query, and send any result rows to sinfo->tuplestore.
 */
static PGresult *
storeQueryResult(storeInfo *sinfo, PGconn *conn, const char *sql)
{
	bool		first = true;
	int			nestlevel = -1;
	PGresult   *res;

	if (!PQsendQuery(conn, sql))
		elog(ERROR, "could not send query: %s", pchomp(PQerrorMessage(conn)));

	if (!PQsetSingleRowMode(conn))	/* shouldn't fail */
		elog(ERROR, "failed to set single-row mode for dblink query");

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		sinfo->cur_res = libpqsrv_get_result(conn, dblink_we_get_result);
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
storeRow(storeInfo *sinfo, PGresult *res, bool first)
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
		sinfo->cstrs = palloc_array(char *, nfields);
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
			/* ignore it if it's not an open connection */
			if (hentry->rconn.conn == NULL)
				continue;
			/* stash away current value */
			astate = accumArrayResult(astate,
									  CStringGetTextDatum(hentry->name),
									  false, TEXTOID, CurrentMemoryContext);
		}
	}

	if (astate)
		PG_RETURN_DATUM(makeArrayResult(astate,
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
	PGconn	   *conn;

	dblink_init();
	conn = dblink_get_named_conn(text_to_cstring(PG_GETARG_TEXT_PP(0)));

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
	PGconn	   *conn;
	const char *msg;
	TimestampTz endtime;

	dblink_init();
	conn = dblink_get_named_conn(text_to_cstring(PG_GETARG_TEXT_PP(0)));
	endtime = TimestampTzPlusMilliseconds(GetCurrentTimestamp(),
										  30000);
	msg = libpqsrv_cancel(conn, endtime);
	if (msg == NULL)
		msg = "OK";

	PG_RETURN_TEXT_P(cstring_to_text(msg));
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
	PGconn	   *conn;

	dblink_init();
	conn = dblink_get_named_conn(text_to_cstring(PG_GETARG_TEXT_PP(0)));

	msg = PQerrorMessage(conn);
	if (msg == NULL || msg[0] == '\0')
		PG_RETURN_TEXT_P(cstring_to_text("OK"));
	else
		PG_RETURN_TEXT_P(cstring_to_text(pchomp(msg)));
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

	dblink_init();

	PG_TRY();
	{
		PGresult   *res = NULL;
		char	   *sql = NULL;
		char	   *conname = NULL;
		bool		fail = true;	/* default to backward compatible behavior */

		if (PG_NARGS() == 3)
		{
			/* must be text,text,bool */
			conname = text_to_cstring(PG_GETARG_TEXT_PP(0));
			sql = text_to_cstring(PG_GETARG_TEXT_PP(1));
			fail = PG_GETARG_BOOL(2);
			dblink_get_conn(conname, &conn, &conname, &freeconn);
		}
		else if (PG_NARGS() == 2)
		{
			/* might be text,text or text,bool */
			if (get_fn_expr_argtype(fcinfo->flinfo, 1) == BOOLOID)
			{
				sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
				fail = PG_GETARG_BOOL(1);
				conn = pconn->conn;
			}
			else
			{
				conname = text_to_cstring(PG_GETARG_TEXT_PP(0));
				sql = text_to_cstring(PG_GETARG_TEXT_PP(1));
				dblink_get_conn(conname, &conn, &conname, &freeconn);
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
			dblink_conn_not_avail(conname);

		res = libpqsrv_exec(conn, sql, dblink_we_get_result);
		if (!res ||
			(PQresultStatus(res) != PGRES_COMMAND_OK &&
			 PQresultStatus(res) != PGRES_TUPLES_OK))
		{
			dblink_res_error(conn, conname, res, fail,
							 "while executing command");

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
	PG_FINALLY();
	{
		/* if needed, close the connection to the database */
		if (freeconn)
			libpqsrv_disconnect(conn);
	}
	PG_END_TRY();

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
	int16		indnkeyatts;
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
		rel = get_rel_from_relname(PG_GETARG_TEXT_PP(0), AccessShareLock, ACL_SELECT);

		/* get the array of attnums */
		results = get_pkey_attnames(rel, &indnkeyatts);

		relation_close(rel, AccessShareLock);

		/*
		 * need a tuple descriptor representing one INT and one TEXT column
		 */
		tupdesc = CreateTemplateTupleDesc(2);
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

		if ((results != NULL) && (indnkeyatts > 0))
		{
			funcctx->max_calls = indnkeyatts;

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

		values = palloc_array(char *, 2);
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
	text	   *relname_text = PG_GETARG_TEXT_PP(0);
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
				 errmsg("source key array length must match number of key attributes")));

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
				 errmsg("target key array length must match number of key attributes")));

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
	text	   *relname_text = PG_GETARG_TEXT_PP(0);
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
				 errmsg("target key array length must match number of key attributes")));

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
	text	   *relname_text = PG_GETARG_TEXT_PP(0);
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
				 errmsg("source key array length must match number of key attributes")));

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
				 errmsg("target key array length must match number of key attributes")));

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
	PGconn	   *conn;
	PGnotify   *notify;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	dblink_init();
	if (PG_NARGS() == 1)
		conn = dblink_get_named_conn(text_to_cstring(PG_GETARG_TEXT_PP(0)));
	else
		conn = pconn->conn;

	InitMaterializedSRF(fcinfo, 0);

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

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

		PQfreemem(notify);
		PQconsumeInput(conn);
	}

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
					 errdetail("Could not get libpq's default connection options.")));
	}

	/* Validate each supplied option. */
	foreach(cell, options_list)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		if (!is_valid_dblink_fdw_option(options, def->defname, context))
		{
			/*
			 * Unknown option, or invalid option for the context specified, so
			 * complain about it.  Provide a hint with a valid option that
			 * looks similar, if there is one.
			 */
			const PQconninfoOption *opt;
			const char *closest_match;
			ClosestMatchState match_state;
			bool		has_valid_options = false;

			initClosestMatch(&match_state, def->defname, 4);
			for (opt = options; opt->keyword; opt++)
			{
				if (is_valid_dblink_option(options, opt->keyword, context))
				{
					has_valid_options = true;
					updateClosestMatch(&match_state, opt->keyword);
				}
			}

			closest_match = getClosestMatch(&match_state);
			ereport(ERROR,
					(errcode(ERRCODE_FDW_OPTION_NAME_NOT_FOUND),
					 errmsg("invalid option \"%s\"", def->defname),
					 has_valid_options ? closest_match ?
					 errhint("Perhaps you meant the option \"%s\".",
							 closest_match) : 0 :
					 errhint("There are no valid options in this context.")));
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
 * Return NULL, and set indnkeyatts = 0, if no primary key exists.
 */
static char **
get_pkey_attnames(Relation rel, int16 *indnkeyatts)
{
	Relation	indexRelation;
	ScanKeyData skey;
	SysScanDesc scan;
	HeapTuple	indexTuple;
	int			i;
	char	  **result = NULL;
	TupleDesc	tupdesc;

	/* initialize indnkeyatts to 0 in case no primary key exists */
	*indnkeyatts = 0;

	tupdesc = rel->rd_att;

	/* Prepare to scan pg_index for entries having indrelid = this rel. */
	indexRelation = table_open(IndexRelationId, AccessShareLock);
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
			*indnkeyatts = index->indnkeyatts;
			if (*indnkeyatts > 0)
			{
				result = palloc_array(char *, *indnkeyatts);

				for (i = 0; i < *indnkeyatts; i++)
					result[i] = SPI_fname(tupdesc, index->indkey.values[i]);
			}
			break;
		}
	}

	systable_endscan(scan);
	table_close(indexRelation, AccessShareLock);

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

	values = palloc_array(char *, nitems);

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
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;

		if (needComma)
			appendStringInfoChar(&buf, ',');

		appendStringInfoString(&buf,
							   quote_ident_cstr(NameStr(att->attname)));
		needComma = true;
	}

	appendStringInfoString(&buf, ") VALUES(");

	/*
	 * Note: i is physical column number (counting from 0).
	 */
	needComma = false;
	for (i = 0; i < natts; i++)
	{
		if (TupleDescAttr(tupdesc, i)->attisdropped)
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

	return buf.data;
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
		Form_pg_attribute attr = TupleDescAttr(tupdesc, pkattnum);

		if (i > 0)
			appendStringInfoString(&buf, " AND ");

		appendStringInfoString(&buf,
							   quote_ident_cstr(NameStr(attr->attname)));

		if (tgt_pkattvals[i] != NULL)
			appendStringInfo(&buf, " = %s",
							 quote_literal_cstr(tgt_pkattvals[i]));
		else
			appendStringInfoString(&buf, " IS NULL");
	}

	return buf.data;
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
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

		if (attr->attisdropped)
			continue;

		if (needComma)
			appendStringInfoString(&buf, ", ");

		appendStringInfo(&buf, "%s = ",
						 quote_ident_cstr(NameStr(attr->attname)));

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
		Form_pg_attribute attr = TupleDescAttr(tupdesc, pkattnum);

		if (i > 0)
			appendStringInfoString(&buf, " AND ");

		appendStringInfoString(&buf,
							   quote_ident_cstr(NameStr(attr->attname)));

		val = tgt_pkattvals[i];

		if (val != NULL)
			appendStringInfo(&buf, " = %s", quote_literal_cstr(val));
		else
			appendStringInfoString(&buf, " IS NULL");
	}

	return buf.data;
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
	result_text = DatumGetTextPP(DirectFunctionCall1(quote_ident,
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
	SPI_connect();

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
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

		if (i > 0)
			appendStringInfoString(&buf, ", ");

		if (attr->attisdropped)
			appendStringInfoString(&buf, "NULL");
		else
			appendStringInfoString(&buf,
								   quote_ident_cstr(NameStr(attr->attname)));
	}

	appendStringInfo(&buf, " FROM %s WHERE ", relname);

	for (i = 0; i < pknumatts; i++)
	{
		int			pkattnum = pkattnums[i];
		Form_pg_attribute attr = TupleDescAttr(tupdesc, pkattnum);

		if (i > 0)
			appendStringInfoString(&buf, " AND ");

		appendStringInfoString(&buf,
							   quote_ident_cstr(NameStr(attr->attname)));

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
	rel = table_openrv(relvar, lockmode);

	aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(),
								  aclmode);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_relkind_objtype(rel->rd_rel->relkind),
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

	if (hentry && hentry->rconn.conn != NULL)
		return &hentry->rconn;

	return NULL;
}

static HTAB *
createConnHash(void)
{
	HASHCTL		ctl;

	ctl.keysize = NAMEDATALEN;
	ctl.entrysize = sizeof(remoteConnHashEnt);

	return hash_create("Remote Con hash", NUMCONN, &ctl,
					   HASH_ELEM | HASH_STRINGS);
}

static remoteConn *
createNewConnection(const char *name)
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

	if (found && hentry->rconn.conn != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("duplicate connection name")));

	/* New, or reusable, so initialize the rconn struct to zeroes */
	memset(&hentry->rconn, 0, sizeof(remoteConn));

	return &hentry->rconn;
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

 /*
  * Ensure that require_auth and SCRAM keys are correctly set on connstr.
  * SCRAM keys used to pass-through are coming from the initial connection
  * from the client with the server.
  *
  * All required SCRAM options are set by dblink, so we just need to ensure
  * that these options are not overwritten by the user.
  *
  * See appendSCRAMKeysInfo and its usage for more.
  */
bool
dblink_connstr_has_required_scram_options(const char *connstr)
{
	PQconninfoOption *options;
	bool		has_scram_server_key = false;
	bool		has_scram_client_key = false;
	bool		has_require_auth = false;
	bool		has_scram_keys = false;

	options = PQconninfoParse(connstr, NULL);
	if (options)
	{
		/*
		 * Continue iterating even if we found the keys that we need to
		 * validate to make sure that there is no other declaration of these
		 * keys that can overwrite the first.
		 */
		for (PQconninfoOption *option = options; option->keyword != NULL; option++)
		{
			if (strcmp(option->keyword, "require_auth") == 0)
			{
				if (option->val != NULL && strcmp(option->val, "scram-sha-256") == 0)
					has_require_auth = true;
				else
					has_require_auth = false;
			}

			if (strcmp(option->keyword, "scram_client_key") == 0)
			{
				if (option->val != NULL && option->val[0] != '\0')
					has_scram_client_key = true;
				else
					has_scram_client_key = false;
			}

			if (strcmp(option->keyword, "scram_server_key") == 0)
			{
				if (option->val != NULL && option->val[0] != '\0')
					has_scram_server_key = true;
				else
					has_scram_server_key = false;
			}
		}
		PQconninfoFree(options);
	}

	has_scram_keys = has_scram_client_key && has_scram_server_key && MyProcPort->has_scram_keys;

	return (has_scram_keys && has_require_auth);
}

/*
 * We need to make sure that the connection made used credentials
 * which were provided by the user, so check what credentials were
 * used to connect and then make sure that they came from the user.
 *
 * On failure, we close "conn" and also delete the hashtable entry
 * identified by "connname" (if that's not NULL).
 */
static void
dblink_security_check(PGconn *conn, const char *connname, const char *connstr)
{
	/* Superuser bypasses security check */
	if (superuser())
		return;

	/* If password was used to connect, make sure it was one provided */
	if (PQconnectionUsedPassword(conn) && dblink_connstr_has_pw(connstr))
		return;

	/*
	 * Password was not used to connect, check if SCRAM pass-through is in
	 * use.
	 *
	 * If dblink_connstr_has_required_scram_options is true we assume that
	 * UseScramPassthrough is also true because the required SCRAM keys are
	 * only added if UseScramPassthrough is set, and the user is not allowed
	 * to add the SCRAM keys on fdw and user mapping options.
	 */
	if (MyProcPort->has_scram_keys && dblink_connstr_has_required_scram_options(connstr))
		return;

#ifdef ENABLE_GSS
	/* If GSSAPI creds used to connect, make sure it was one delegated */
	if (PQconnectionUsedGSSAPI(conn) && be_gssapi_get_delegation(MyProcPort))
		return;
#endif

	/* Otherwise, fail out */
	libpqsrv_disconnect(conn);
	if (connname)
		deleteConnection(connname);

	ereport(ERROR,
			(errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
			 errmsg("password or GSSAPI delegated credentials required"),
			 errdetail("Non-superusers may only connect using credentials they provide, eg: password in connection string or delegated GSSAPI credentials"),
			 errhint("Ensure provided credentials match target server's authentication method.")));
}

/*
 * Function to check if the connection string includes an explicit
 * password, needed to ensure that non-superuser password-based auth
 * is using a provided password and not one picked up from the
 * environment.
 */
static bool
dblink_connstr_has_pw(const char *connstr)
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

	return connstr_gives_password;
}

/*
 * For non-superusers, insist that the connstr specify a password, except if
 * GSSAPI credentials have been delegated (and we check that they are used for
 * the connection in dblink_security_check later) or if SCRAM pass-through is
 * being used.  This prevents a password or GSSAPI credentials from being
 * picked up from .pgpass, a service file, the environment, etc.  We don't want
 * the postgres user's passwords or Kerberos credentials to be accessible to
 * non-superusers. In case of SCRAM pass-through insist that the connstr
 * has the required SCRAM pass-through options.
 */
static void
dblink_connstr_check(const char *connstr)
{
	if (superuser())
		return;

	if (dblink_connstr_has_pw(connstr))
		return;

	if (MyProcPort->has_scram_keys && dblink_connstr_has_required_scram_options(connstr))
		return;

#ifdef ENABLE_GSS
	if (be_gssapi_get_delegation(MyProcPort))
		return;
#endif

	ereport(ERROR,
			(errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
			 errmsg("password or GSSAPI delegated credentials required"),
			 errdetail("Non-superusers must provide a password in the connection string or send delegated GSSAPI credentials.")));
}

/*
 * Report an error received from the remote server
 *
 * res: the received error result
 * fail: true for ERROR ereport, false for NOTICE
 * fmt and following args: sprintf-style format and values for errcontext;
 * the resulting string should be worded like "while <some action>"
 *
 * If "res" is not NULL, it'll be PQclear'ed here (unless we throw error,
 * in which case memory context cleanup will clear it eventually).
 */
static void
dblink_res_error(PGconn *conn, const char *conname, PGresult *res,
				 bool fail, const char *fmt,...)
{
	int			level;
	char	   *pg_diag_sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
	char	   *message_primary = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
	char	   *message_detail = PQresultErrorField(res, PG_DIAG_MESSAGE_DETAIL);
	char	   *message_hint = PQresultErrorField(res, PG_DIAG_MESSAGE_HINT);
	char	   *message_context = PQresultErrorField(res, PG_DIAG_CONTEXT);
	int			sqlstate;
	va_list		ap;
	char		dblink_context_msg[512];

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

	/*
	 * If we don't get a message from the PGresult, try the PGconn.  This is
	 * needed because for connection-level failures, PQgetResult may just
	 * return NULL, not a PGresult at all.
	 */
	if (message_primary == NULL)
		message_primary = pchomp(PQerrorMessage(conn));

	/*
	 * Format the basic errcontext string.  Below, we'll add on something
	 * about the connection name.  That's a violation of the translatability
	 * guidelines about constructing error messages out of parts, but since
	 * there's no translation support for dblink, there's no need to worry
	 * about that (yet).
	 */
	va_start(ap, fmt);
	vsnprintf(dblink_context_msg, sizeof(dblink_context_msg), fmt, ap);
	va_end(ap);

	ereport(level,
			(errcode(sqlstate),
			 (message_primary != NULL && message_primary[0] != '\0') ?
			 errmsg_internal("%s", message_primary) :
			 errmsg("could not obtain message string for remote error"),
			 message_detail ? errdetail_internal("%s", message_detail) : 0,
			 message_hint ? errhint("%s", message_hint) : 0,
			 message_context ? (errcontext("%s", message_context)) : 0,
			 conname ?
			 (errcontext("%s on dblink connection named \"%s\"",
						 dblink_context_msg, conname)) :
			 (errcontext("%s on unnamed dblink connection",
						 dblink_context_msg))));
	PQclear(res);
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
	StringInfoData buf;
	ForeignDataWrapper *fdw;
	AclResult	aclresult;
	char	   *srvname;

	static const PQconninfoOption *options = NULL;

	initStringInfo(&buf);

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
					 errdetail("Could not get libpq's default connection options.")));
	}

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
		aclresult = object_aclcheck(ForeignServerRelationId, serverid, userid, ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_FOREIGN_SERVER, foreign_server->servername);

		/*
		 * First append hardcoded options needed for SCRAM pass-through, so if
		 * the user overwrites these options we can ereport on
		 * dblink_connstr_check and dblink_security_check.
		 */
		if (MyProcPort->has_scram_keys && UseScramPassthrough(foreign_server, user_mapping))
			appendSCRAMKeysInfo(&buf);

		foreach(cell, fdw->options)
		{
			DefElem    *def = lfirst(cell);

			if (is_valid_dblink_option(options, def->defname, ForeignDataWrapperRelationId))
				appendStringInfo(&buf, "%s='%s' ", def->defname,
								 escape_param_str(strVal(def->arg)));
		}

		foreach(cell, foreign_server->options)
		{
			DefElem    *def = lfirst(cell);

			if (is_valid_dblink_option(options, def->defname, ForeignServerRelationId))
				appendStringInfo(&buf, "%s='%s' ", def->defname,
								 escape_param_str(strVal(def->arg)));
		}

		foreach(cell, user_mapping->options)
		{

			DefElem    *def = lfirst(cell);

			if (is_valid_dblink_option(options, def->defname, UserMappingRelationId))
				appendStringInfo(&buf, "%s='%s' ", def->defname,
								 escape_param_str(strVal(def->arg)));
		}

		return buf.data;
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
	StringInfoData buf;

	initStringInfo(&buf);

	for (cp = str; *cp; cp++)
	{
		if (*cp == '\\' || *cp == '\'')
			appendStringInfoChar(&buf, '\\');
		appendStringInfoChar(&buf, *cp);
	}

	return buf.data;
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
	*pkattnums = palloc_array(int, pknumatts_arg);
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
			if (TupleDescAttr(tupdesc, j)->attisdropped)
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
	 * Disallow OAuth options for now, since the builtin flow communicates on
	 * stderr by default and can't cache tokens yet.
	 */
	if (strncmp(opt->keyword, "oauth_", strlen("oauth_")) == 0)
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
 * Same as is_valid_dblink_option but also check for only dblink_fdw specific
 * options.
 */
static bool
is_valid_dblink_fdw_option(const PQconninfoOption *options, const char *option,
						   Oid context)
{
	if (strcmp(option, "use_scram_passthrough") == 0)
		return true;

	return is_valid_dblink_option(options, option, context);
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

/*
 * Append SCRAM client key and server key information from the global
 * MyProcPort into the given StringInfo buffer.
 */
static void
appendSCRAMKeysInfo(StringInfo buf)
{
	int			len;
	int			encoded_len;
	char	   *client_key;
	char	   *server_key;

	len = pg_b64_enc_len(sizeof(MyProcPort->scram_ClientKey));
	/* don't forget the zero-terminator */
	client_key = palloc0(len + 1);
	encoded_len = pg_b64_encode(MyProcPort->scram_ClientKey,
								sizeof(MyProcPort->scram_ClientKey),
								client_key, len);
	if (encoded_len < 0)
		elog(ERROR, "could not encode SCRAM client key");

	len = pg_b64_enc_len(sizeof(MyProcPort->scram_ServerKey));
	/* don't forget the zero-terminator */
	server_key = palloc0(len + 1);
	encoded_len = pg_b64_encode(MyProcPort->scram_ServerKey,
								sizeof(MyProcPort->scram_ServerKey),
								server_key, len);
	if (encoded_len < 0)
		elog(ERROR, "could not encode SCRAM server key");

	appendStringInfo(buf, "scram_client_key='%s' ", client_key);
	appendStringInfo(buf, "scram_server_key='%s' ", server_key);
	appendStringInfoString(buf, "require_auth='scram-sha-256' ");

	pfree(client_key);
	pfree(server_key);
}


static bool
UseScramPassthrough(ForeignServer *foreign_server, UserMapping *user)
{
	ListCell   *cell;

	foreach(cell, foreign_server->options)
	{
		DefElem    *def = lfirst(cell);

		if (strcmp(def->defname, "use_scram_passthrough") == 0)
			return defGetBoolean(def);
	}

	foreach(cell, user->options)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		if (strcmp(def->defname, "use_scram_passthrough") == 0)
			return defGetBoolean(def);
	}

	return false;
}
