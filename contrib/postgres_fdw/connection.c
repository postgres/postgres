/*-------------------------------------------------------------------------
 *
 * connection.c
 *		  Connection management functions for postgres_fdw
 *
 * Portions Copyright (c) 2012-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  contrib/postgres_fdw/connection.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#if HAVE_POLL_H
#include <poll.h>
#endif

#include "access/xact.h"
#include "catalog/pg_user_mapping.h"
#include "commands/defrem.h"
#include "common/base64.h"
#include "funcapi.h"
#include "libpq/libpq-be.h"
#include "libpq/libpq-be-fe-helpers.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postgres_fdw.h"
#include "storage/latch.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/syscache.h"

/*
 * Connection cache hash table entry
 *
 * The lookup key in this hash table is the user mapping OID. We use just one
 * connection per user mapping ID, which ensures that all the scans use the
 * same snapshot during a query.  Using the user mapping OID rather than
 * the foreign server OID + user OID avoids creating multiple connections when
 * the public user mapping applies to all user OIDs.
 *
 * The "conn" pointer can be NULL if we don't currently have a live connection.
 * When we do have a connection, xact_depth tracks the current depth of
 * transactions and subtransactions open on the remote side.  We need to issue
 * commands at the same nesting depth on the remote as we're executing at
 * ourselves, so that rolling back a subtransaction will kill the right
 * queries and not the wrong ones.
 */
typedef Oid ConnCacheKey;

typedef struct ConnCacheEntry
{
	ConnCacheKey key;			/* hash key (must be first) */
	PGconn	   *conn;			/* connection to foreign server, or NULL */
	/* Remaining fields are invalid when conn is NULL: */
	int			xact_depth;		/* 0 = no xact open, 1 = main xact open, 2 =
								 * one level of subxact open, etc */
	bool		have_prep_stmt; /* have we prepared any stmts in this xact? */
	bool		have_error;		/* have any subxacts aborted in this xact? */
	bool		changing_xact_state;	/* xact state change in process */
	bool		parallel_commit;	/* do we commit (sub)xacts in parallel? */
	bool		parallel_abort; /* do we abort (sub)xacts in parallel? */
	bool		invalidated;	/* true if reconnect is pending */
	bool		keep_connections;	/* setting value of keep_connections
									 * server option */
	Oid			serverid;		/* foreign server OID used to get server name */
	uint32		server_hashvalue;	/* hash value of foreign server OID */
	uint32		mapping_hashvalue;	/* hash value of user mapping OID */
	PgFdwConnState state;		/* extra per-connection state */
} ConnCacheEntry;

/*
 * Connection cache (initialized on first use)
 */
static HTAB *ConnectionHash = NULL;

/* for assigning cursor numbers and prepared statement numbers */
static unsigned int cursor_number = 0;
static unsigned int prep_stmt_number = 0;

/* tracks whether any work is needed in callback functions */
static bool xact_got_connection = false;

/* custom wait event values, retrieved from shared memory */
static uint32 pgfdw_we_cleanup_result = 0;
static uint32 pgfdw_we_connect = 0;
static uint32 pgfdw_we_get_result = 0;

/*
 * Milliseconds to wait to cancel an in-progress query or execute a cleanup
 * query; if it takes longer than 30 seconds to do these, we assume the
 * connection is dead.
 */
#define CONNECTION_CLEANUP_TIMEOUT	30000

/*
 * Milliseconds to wait before issuing another cancel request.  This covers
 * the race condition where the remote session ignored our cancel request
 * because it arrived while idle.
 */
#define RETRY_CANCEL_TIMEOUT	1000

/* Macro for constructing abort command to be sent */
#define CONSTRUCT_ABORT_COMMAND(sql, entry, toplevel) \
	do { \
		if (toplevel) \
			snprintf((sql), sizeof(sql), \
					 "ABORT TRANSACTION"); \
		else \
			snprintf((sql), sizeof(sql), \
					 "ROLLBACK TO SAVEPOINT s%d; RELEASE SAVEPOINT s%d", \
					 (entry)->xact_depth, (entry)->xact_depth); \
	} while(0)

/*
 * Extension version number, for supporting older extension versions' objects
 */
enum pgfdwVersion
{
	PGFDW_V1_1 = 0,
	PGFDW_V1_2,
};

/*
 * SQL functions
 */
PG_FUNCTION_INFO_V1(postgres_fdw_get_connections);
PG_FUNCTION_INFO_V1(postgres_fdw_get_connections_1_2);
PG_FUNCTION_INFO_V1(postgres_fdw_disconnect);
PG_FUNCTION_INFO_V1(postgres_fdw_disconnect_all);

/* prototypes of private functions */
static void make_new_connection(ConnCacheEntry *entry, UserMapping *user);
static PGconn *connect_pg_server(ForeignServer *server, UserMapping *user);
static void disconnect_pg_server(ConnCacheEntry *entry);
static void check_conn_params(const char **keywords, const char **values, UserMapping *user);
static void configure_remote_session(PGconn *conn);
static void do_sql_command_begin(PGconn *conn, const char *sql);
static void do_sql_command_end(PGconn *conn, const char *sql,
							   bool consume_input);
static void begin_remote_xact(ConnCacheEntry *entry);
static void pgfdw_xact_callback(XactEvent event, void *arg);
static void pgfdw_subxact_callback(SubXactEvent event,
								   SubTransactionId mySubid,
								   SubTransactionId parentSubid,
								   void *arg);
static void pgfdw_inval_callback(Datum arg, int cacheid, uint32 hashvalue);
static void pgfdw_reject_incomplete_xact_state_change(ConnCacheEntry *entry);
static void pgfdw_reset_xact_state(ConnCacheEntry *entry, bool toplevel);
static bool pgfdw_cancel_query(PGconn *conn);
static bool pgfdw_cancel_query_begin(PGconn *conn, TimestampTz endtime);
static bool pgfdw_cancel_query_end(PGconn *conn, TimestampTz endtime,
								   TimestampTz retrycanceltime,
								   bool consume_input);
static bool pgfdw_exec_cleanup_query(PGconn *conn, const char *query,
									 bool ignore_errors);
static bool pgfdw_exec_cleanup_query_begin(PGconn *conn, const char *query);
static bool pgfdw_exec_cleanup_query_end(PGconn *conn, const char *query,
										 TimestampTz endtime,
										 bool consume_input,
										 bool ignore_errors);
static bool pgfdw_get_cleanup_result(PGconn *conn, TimestampTz endtime,
									 TimestampTz retrycanceltime,
									 PGresult **result, bool *timed_out);
static void pgfdw_abort_cleanup(ConnCacheEntry *entry, bool toplevel);
static bool pgfdw_abort_cleanup_begin(ConnCacheEntry *entry, bool toplevel,
									  List **pending_entries,
									  List **cancel_requested);
static void pgfdw_finish_pre_commit_cleanup(List *pending_entries);
static void pgfdw_finish_pre_subcommit_cleanup(List *pending_entries,
											   int curlevel);
static void pgfdw_finish_abort_cleanup(List *pending_entries,
									   List *cancel_requested,
									   bool toplevel);
static void pgfdw_security_check(const char **keywords, const char **values,
								 UserMapping *user, PGconn *conn);
static bool UserMappingPasswordRequired(UserMapping *user);
static bool UseScramPassthrough(ForeignServer *server, UserMapping *user);
static bool disconnect_cached_connections(Oid serverid);
static void postgres_fdw_get_connections_internal(FunctionCallInfo fcinfo,
												  enum pgfdwVersion api_version);
static int	pgfdw_conn_check(PGconn *conn);
static bool pgfdw_conn_checkable(void);
static bool pgfdw_has_required_scram_options(const char **keywords, const char **values);

/*
 * Get a PGconn which can be used to execute queries on the remote PostgreSQL
 * server with the user's authorization.  A new connection is established
 * if we don't already have a suitable one, and a transaction is opened at
 * the right subtransaction nesting depth if we didn't do that already.
 *
 * will_prep_stmt must be true if caller intends to create any prepared
 * statements.  Since those don't go away automatically at transaction end
 * (not even on error), we need this flag to cue manual cleanup.
 *
 * If state is not NULL, *state receives the per-connection state associated
 * with the PGconn.
 */
PGconn *
GetConnection(UserMapping *user, bool will_prep_stmt, PgFdwConnState **state)
{
	bool		found;
	bool		retry = false;
	ConnCacheEntry *entry;
	ConnCacheKey key;
	MemoryContext ccxt = CurrentMemoryContext;

	/* First time through, initialize connection cache hashtable */
	if (ConnectionHash == NULL)
	{
		HASHCTL		ctl;

		if (pgfdw_we_get_result == 0)
			pgfdw_we_get_result =
				WaitEventExtensionNew("PostgresFdwGetResult");

		ctl.keysize = sizeof(ConnCacheKey);
		ctl.entrysize = sizeof(ConnCacheEntry);
		ConnectionHash = hash_create("postgres_fdw connections", 8,
									 &ctl,
									 HASH_ELEM | HASH_BLOBS);

		/*
		 * Register some callback functions that manage connection cleanup.
		 * This should be done just once in each backend.
		 */
		RegisterXactCallback(pgfdw_xact_callback, NULL);
		RegisterSubXactCallback(pgfdw_subxact_callback, NULL);
		CacheRegisterSyscacheCallback(FOREIGNSERVEROID,
									  pgfdw_inval_callback, (Datum) 0);
		CacheRegisterSyscacheCallback(USERMAPPINGOID,
									  pgfdw_inval_callback, (Datum) 0);
	}

	/* Set flag that we did GetConnection during the current transaction */
	xact_got_connection = true;

	/* Create hash key for the entry.  Assume no pad bytes in key struct */
	key = user->umid;

	/*
	 * Find or create cached entry for requested connection.
	 */
	entry = hash_search(ConnectionHash, &key, HASH_ENTER, &found);
	if (!found)
	{
		/*
		 * We need only clear "conn" here; remaining fields will be filled
		 * later when "conn" is set.
		 */
		entry->conn = NULL;
	}

	/* Reject further use of connections which failed abort cleanup. */
	pgfdw_reject_incomplete_xact_state_change(entry);

	/*
	 * If the connection needs to be remade due to invalidation, disconnect as
	 * soon as we're out of all transactions.
	 */
	if (entry->conn != NULL && entry->invalidated && entry->xact_depth == 0)
	{
		elog(DEBUG3, "closing connection %p for option changes to take effect",
			 entry->conn);
		disconnect_pg_server(entry);
	}

	/*
	 * If cache entry doesn't have a connection, we have to establish a new
	 * connection.  (If connect_pg_server throws an error, the cache entry
	 * will remain in a valid empty state, ie conn == NULL.)
	 */
	if (entry->conn == NULL)
		make_new_connection(entry, user);

	/*
	 * We check the health of the cached connection here when using it.  In
	 * cases where we're out of all transactions, if a broken connection is
	 * detected, we try to reestablish a new connection later.
	 */
	PG_TRY();
	{
		/* Process a pending asynchronous request if any. */
		if (entry->state.pendingAreq)
			process_pending_request(entry->state.pendingAreq);
		/* Start a new transaction or subtransaction if needed. */
		begin_remote_xact(entry);
	}
	PG_CATCH();
	{
		MemoryContext ecxt = MemoryContextSwitchTo(ccxt);
		ErrorData  *errdata = CopyErrorData();

		/*
		 * Determine whether to try to reestablish the connection.
		 *
		 * After a broken connection is detected in libpq, any error other
		 * than connection failure (e.g., out-of-memory) can be thrown
		 * somewhere between return from libpq and the expected ereport() call
		 * in pgfdw_report_error(). In this case, since PQstatus() indicates
		 * CONNECTION_BAD, checking only PQstatus() causes the false detection
		 * of connection failure. To avoid this, we also verify that the
		 * error's sqlstate is ERRCODE_CONNECTION_FAILURE. Note that also
		 * checking only the sqlstate can cause another false detection
		 * because pgfdw_report_error() may report ERRCODE_CONNECTION_FAILURE
		 * for any libpq-originated error condition.
		 */
		if (errdata->sqlerrcode != ERRCODE_CONNECTION_FAILURE ||
			PQstatus(entry->conn) != CONNECTION_BAD ||
			entry->xact_depth > 0)
		{
			MemoryContextSwitchTo(ecxt);
			PG_RE_THROW();
		}

		/* Clean up the error state */
		FlushErrorState();
		FreeErrorData(errdata);
		errdata = NULL;

		retry = true;
	}
	PG_END_TRY();

	/*
	 * If a broken connection is detected, disconnect it, reestablish a new
	 * connection and retry a new remote transaction. If connection failure is
	 * reported again, we give up getting a connection.
	 */
	if (retry)
	{
		Assert(entry->xact_depth == 0);

		ereport(DEBUG3,
				(errmsg_internal("could not start remote transaction on connection %p",
								 entry->conn)),
				errdetail_internal("%s", pchomp(PQerrorMessage(entry->conn))));

		elog(DEBUG3, "closing connection %p to reestablish a new one",
			 entry->conn);
		disconnect_pg_server(entry);

		make_new_connection(entry, user);

		begin_remote_xact(entry);
	}

	/* Remember if caller will prepare statements */
	entry->have_prep_stmt |= will_prep_stmt;

	/* If caller needs access to the per-connection state, return it. */
	if (state)
		*state = &entry->state;

	return entry->conn;
}

/*
 * Reset all transient state fields in the cached connection entry and
 * establish new connection to the remote server.
 */
static void
make_new_connection(ConnCacheEntry *entry, UserMapping *user)
{
	ForeignServer *server = GetForeignServer(user->serverid);
	ListCell   *lc;

	Assert(entry->conn == NULL);

	/* Reset all transient state fields, to be sure all are clean */
	entry->xact_depth = 0;
	entry->have_prep_stmt = false;
	entry->have_error = false;
	entry->changing_xact_state = false;
	entry->invalidated = false;
	entry->serverid = server->serverid;
	entry->server_hashvalue =
		GetSysCacheHashValue1(FOREIGNSERVEROID,
							  ObjectIdGetDatum(server->serverid));
	entry->mapping_hashvalue =
		GetSysCacheHashValue1(USERMAPPINGOID,
							  ObjectIdGetDatum(user->umid));
	memset(&entry->state, 0, sizeof(entry->state));

	/*
	 * Determine whether to keep the connection that we're about to make here
	 * open even after the transaction using it ends, so that the subsequent
	 * transactions can re-use it.
	 *
	 * By default, all the connections to any foreign servers are kept open.
	 *
	 * Also determine whether to commit/abort (sub)transactions opened on the
	 * remote server in parallel at (sub)transaction end, which is disabled by
	 * default.
	 *
	 * Note: it's enough to determine these only when making a new connection
	 * because if these settings for it are changed, it will be closed and
	 * re-made later.
	 */
	entry->keep_connections = true;
	entry->parallel_commit = false;
	entry->parallel_abort = false;
	foreach(lc, server->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "keep_connections") == 0)
			entry->keep_connections = defGetBoolean(def);
		else if (strcmp(def->defname, "parallel_commit") == 0)
			entry->parallel_commit = defGetBoolean(def);
		else if (strcmp(def->defname, "parallel_abort") == 0)
			entry->parallel_abort = defGetBoolean(def);
	}

	/* Now try to make the connection */
	entry->conn = connect_pg_server(server, user);

	elog(DEBUG3, "new postgres_fdw connection %p for server \"%s\" (user mapping oid %u, userid %u)",
		 entry->conn, server->servername, user->umid, user->userid);
}

/*
 * Check that non-superuser has used password or delegated credentials
 * to establish connection; otherwise, he's piggybacking on the
 * postgres server's user identity. See also dblink_security_check()
 * in contrib/dblink and check_conn_params.
 */
static void
pgfdw_security_check(const char **keywords, const char **values, UserMapping *user, PGconn *conn)
{
	/* Superusers bypass the check */
	if (superuser_arg(user->userid))
		return;

#ifdef ENABLE_GSS
	/* Connected via GSSAPI with delegated credentials- all good. */
	if (PQconnectionUsedGSSAPI(conn) && be_gssapi_get_delegation(MyProcPort))
		return;
#endif

	/* Ok if superuser set PW required false. */
	if (!UserMappingPasswordRequired(user))
		return;

	/* Connected via PW, with PW required true, and provided non-empty PW. */
	if (PQconnectionUsedPassword(conn))
	{
		/* ok if params contain a non-empty password */
		for (int i = 0; keywords[i] != NULL; i++)
		{
			if (strcmp(keywords[i], "password") == 0 && values[i][0] != '\0')
				return;
		}
	}

	/*
	 * Ok if SCRAM pass-through is being used and all required SCRAM options
	 * are set correctly. If pgfdw_has_required_scram_options returns true we
	 * assume that UseScramPassthrough is also true since SCRAM options are
	 * only set when UseScramPassthrough is enabled.
	 */
	if (MyProcPort->has_scram_keys && pgfdw_has_required_scram_options(keywords, values))
		return;

	ereport(ERROR,
			(errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
			 errmsg("password or GSSAPI delegated credentials required"),
			 errdetail("Non-superuser cannot connect if the server does not request a password or use GSSAPI with delegated credentials."),
			 errhint("Target server's authentication method must be changed or password_required=false set in the user mapping attributes.")));
}

/*
 * Connect to remote server using specified server and user mapping properties.
 */
static PGconn *
connect_pg_server(ForeignServer *server, UserMapping *user)
{
	PGconn	   *volatile conn = NULL;

	/*
	 * Use PG_TRY block to ensure closing connection on error.
	 */
	PG_TRY();
	{
		const char **keywords;
		const char **values;
		char	   *appname = NULL;
		int			n;

		/*
		 * Construct connection params from generic options of ForeignServer
		 * and UserMapping.  (Some of them might not be libpq options, in
		 * which case we'll just waste a few array slots.)  Add 4 extra slots
		 * for application_name, fallback_application_name, client_encoding,
		 * end marker, and 3 extra slots for scram keys and required scram
		 * pass-through options.
		 */
		n = list_length(server->options) + list_length(user->options) + 4 + 3;
		keywords = (const char **) palloc(n * sizeof(char *));
		values = (const char **) palloc(n * sizeof(char *));

		n = 0;
		n += ExtractConnectionOptions(server->options,
									  keywords + n, values + n);
		n += ExtractConnectionOptions(user->options,
									  keywords + n, values + n);

		/*
		 * Use pgfdw_application_name as application_name if set.
		 *
		 * PQconnectdbParams() processes the parameter arrays from start to
		 * end. If any key word is repeated, the last value is used. Therefore
		 * note that pgfdw_application_name must be added to the arrays after
		 * options of ForeignServer are, so that it can override
		 * application_name set in ForeignServer.
		 */
		if (pgfdw_application_name && *pgfdw_application_name != '\0')
		{
			keywords[n] = "application_name";
			values[n] = pgfdw_application_name;
			n++;
		}

		/*
		 * Search the parameter arrays to find application_name setting, and
		 * replace escape sequences in it with status information if found.
		 * The arrays are searched backwards because the last value is used if
		 * application_name is repeatedly set.
		 */
		for (int i = n - 1; i >= 0; i--)
		{
			if (strcmp(keywords[i], "application_name") == 0 &&
				*(values[i]) != '\0')
			{
				/*
				 * Use this application_name setting if it's not empty string
				 * even after any escape sequences in it are replaced.
				 */
				appname = process_pgfdw_appname(values[i]);
				if (appname[0] != '\0')
				{
					values[i] = appname;
					break;
				}

				/*
				 * This empty application_name is not used, so we set
				 * values[i] to NULL and keep searching the array to find the
				 * next one.
				 */
				values[i] = NULL;
				pfree(appname);
				appname = NULL;
			}
		}

		/* Use "postgres_fdw" as fallback_application_name */
		keywords[n] = "fallback_application_name";
		values[n] = "postgres_fdw";
		n++;

		/* Set client_encoding so that libpq can convert encoding properly. */
		keywords[n] = "client_encoding";
		values[n] = GetDatabaseEncodingName();
		n++;

		/* Add required SCRAM pass-through connection options if it's enabled. */
		if (MyProcPort->has_scram_keys && UseScramPassthrough(server, user))
		{
			int			len;
			int			encoded_len;

			keywords[n] = "scram_client_key";
			len = pg_b64_enc_len(sizeof(MyProcPort->scram_ClientKey));
			/* don't forget the zero-terminator */
			values[n] = palloc0(len + 1);
			encoded_len = pg_b64_encode(MyProcPort->scram_ClientKey,
										sizeof(MyProcPort->scram_ClientKey),
										(char *) values[n], len);
			if (encoded_len < 0)
				elog(ERROR, "could not encode SCRAM client key");
			n++;

			keywords[n] = "scram_server_key";
			len = pg_b64_enc_len(sizeof(MyProcPort->scram_ServerKey));
			/* don't forget the zero-terminator */
			values[n] = palloc0(len + 1);
			encoded_len = pg_b64_encode(MyProcPort->scram_ServerKey,
										sizeof(MyProcPort->scram_ServerKey),
										(char *) values[n], len);
			if (encoded_len < 0)
				elog(ERROR, "could not encode SCRAM server key");
			n++;

			/*
			 * Require scram-sha-256 to ensure that no other auth method is
			 * used when connecting with foreign server.
			 */
			keywords[n] = "require_auth";
			values[n] = "scram-sha-256";
			n++;
		}

		keywords[n] = values[n] = NULL;

		/* Verify the set of connection parameters. */
		check_conn_params(keywords, values, user);

		/* first time, allocate or get the custom wait event */
		if (pgfdw_we_connect == 0)
			pgfdw_we_connect = WaitEventExtensionNew("PostgresFdwConnect");

		/* OK to make connection */
		conn = libpqsrv_connect_params(keywords, values,
									   false,	/* expand_dbname */
									   pgfdw_we_connect);

		if (!conn || PQstatus(conn) != CONNECTION_OK)
			ereport(ERROR,
					(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
					 errmsg("could not connect to server \"%s\"",
							server->servername),
					 errdetail_internal("%s", pchomp(PQerrorMessage(conn)))));

		PQsetNoticeReceiver(conn, libpqsrv_notice_receiver,
							"received message via remote connection");

		/* Perform post-connection security checks. */
		pgfdw_security_check(keywords, values, user, conn);

		/* Prepare new session for use */
		configure_remote_session(conn);

		if (appname != NULL)
			pfree(appname);
		pfree(keywords);
		pfree(values);
	}
	PG_CATCH();
	{
		libpqsrv_disconnect(conn);
		PG_RE_THROW();
	}
	PG_END_TRY();

	return conn;
}

/*
 * Disconnect any open connection for a connection cache entry.
 */
static void
disconnect_pg_server(ConnCacheEntry *entry)
{
	if (entry->conn != NULL)
	{
		libpqsrv_disconnect(entry->conn);
		entry->conn = NULL;
	}
}

/*
 * Return true if the password_required is defined and false for this user
 * mapping, otherwise false. The mapping has been pre-validated.
 */
static bool
UserMappingPasswordRequired(UserMapping *user)
{
	ListCell   *cell;

	foreach(cell, user->options)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		if (strcmp(def->defname, "password_required") == 0)
			return defGetBoolean(def);
	}

	return true;
}

static bool
UseScramPassthrough(ForeignServer *server, UserMapping *user)
{
	ListCell   *cell;

	foreach(cell, server->options)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

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

/*
 * For non-superusers, insist that the connstr specify a password or that the
 * user provided their own GSSAPI delegated credentials.  This
 * prevents a password from being picked up from .pgpass, a service file, the
 * environment, etc.  We don't want the postgres user's passwords,
 * certificates, etc to be accessible to non-superusers.  (See also
 * dblink_connstr_check in contrib/dblink.)
 */
static void
check_conn_params(const char **keywords, const char **values, UserMapping *user)
{
	int			i;

	/* no check required if superuser */
	if (superuser_arg(user->userid))
		return;

#ifdef ENABLE_GSS
	/* ok if the user provided their own delegated credentials */
	if (be_gssapi_get_delegation(MyProcPort))
		return;
#endif

	/* ok if params contain a non-empty password */
	for (i = 0; keywords[i] != NULL; i++)
	{
		if (strcmp(keywords[i], "password") == 0 && values[i][0] != '\0')
			return;
	}

	/* ok if the superuser explicitly said so at user mapping creation time */
	if (!UserMappingPasswordRequired(user))
		return;

	/*
	 * Ok if SCRAM pass-through is being used and all required scram options
	 * are set correctly. If pgfdw_has_required_scram_options returns true we
	 * assume that UseScramPassthrough is also true since SCRAM options are
	 * only set when UseScramPassthrough is enabled.
	 */
	if (MyProcPort->has_scram_keys && pgfdw_has_required_scram_options(keywords, values))
		return;

	ereport(ERROR,
			(errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
			 errmsg("password or GSSAPI delegated credentials required"),
			 errdetail("Non-superusers must delegate GSSAPI credentials, provide a password, or enable SCRAM pass-through in user mapping.")));
}

/*
 * Issue SET commands to make sure remote session is configured properly.
 *
 * We do this just once at connection, assuming nothing will change the
 * values later.  Since we'll never send volatile function calls to the
 * remote, there shouldn't be any way to break this assumption from our end.
 * It's possible to think of ways to break it at the remote end, eg making
 * a foreign table point to a view that includes a set_config call ---
 * but once you admit the possibility of a malicious view definition,
 * there are any number of ways to break things.
 */
static void
configure_remote_session(PGconn *conn)
{
	int			remoteversion = PQserverVersion(conn);

	/* Force the search path to contain only pg_catalog (see deparse.c) */
	do_sql_command(conn, "SET search_path = pg_catalog");

	/*
	 * Set remote timezone; this is basically just cosmetic, since all
	 * transmitted and returned timestamptzs should specify a zone explicitly
	 * anyway.  However it makes the regression test outputs more predictable.
	 *
	 * We don't risk setting remote zone equal to ours, since the remote
	 * server might use a different timezone database.  Instead, use GMT
	 * (quoted, because very old servers are picky about case).  That's
	 * guaranteed to work regardless of the remote's timezone database,
	 * because pg_tzset() hard-wires it (at least in PG 9.2 and later).
	 */
	do_sql_command(conn, "SET timezone = 'GMT'");

	/*
	 * Set values needed to ensure unambiguous data output from remote.  (This
	 * logic should match what pg_dump does.  See also set_transmission_modes
	 * in postgres_fdw.c.)
	 */
	do_sql_command(conn, "SET datestyle = ISO");
	if (remoteversion >= 80400)
		do_sql_command(conn, "SET intervalstyle = postgres");
	if (remoteversion >= 90000)
		do_sql_command(conn, "SET extra_float_digits = 3");
	else
		do_sql_command(conn, "SET extra_float_digits = 2");
}

/*
 * Convenience subroutine to issue a non-data-returning SQL command to remote
 */
void
do_sql_command(PGconn *conn, const char *sql)
{
	do_sql_command_begin(conn, sql);
	do_sql_command_end(conn, sql, false);
}

static void
do_sql_command_begin(PGconn *conn, const char *sql)
{
	if (!PQsendQuery(conn, sql))
		pgfdw_report_error(ERROR, NULL, conn, sql);
}

static void
do_sql_command_end(PGconn *conn, const char *sql, bool consume_input)
{
	PGresult   *res;

	/*
	 * If requested, consume whatever data is available from the socket. (Note
	 * that if all data is available, this allows pgfdw_get_result to call
	 * PQgetResult without forcing the overhead of WaitLatchOrSocket, which
	 * would be large compared to the overhead of PQconsumeInput.)
	 */
	if (consume_input && !PQconsumeInput(conn))
		pgfdw_report_error(ERROR, NULL, conn, sql);
	res = pgfdw_get_result(conn);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pgfdw_report_error(ERROR, res, conn, sql);
	PQclear(res);
}

/*
 * Start remote transaction or subtransaction, if needed.
 *
 * Note that we always use at least REPEATABLE READ in the remote session.
 * This is so that, if a query initiates multiple scans of the same or
 * different foreign tables, we will get snapshot-consistent results from
 * those scans.  A disadvantage is that we can't provide sane emulation of
 * READ COMMITTED behavior --- it would be nice if we had some other way to
 * control which remote queries share a snapshot.
 */
static void
begin_remote_xact(ConnCacheEntry *entry)
{
	int			curlevel = GetCurrentTransactionNestLevel();

	/* Start main transaction if we haven't yet */
	if (entry->xact_depth <= 0)
	{
		const char *sql;

		elog(DEBUG3, "starting remote transaction on connection %p",
			 entry->conn);

		if (IsolationIsSerializable())
			sql = "START TRANSACTION ISOLATION LEVEL SERIALIZABLE";
		else
			sql = "START TRANSACTION ISOLATION LEVEL REPEATABLE READ";
		entry->changing_xact_state = true;
		do_sql_command(entry->conn, sql);
		entry->xact_depth = 1;
		entry->changing_xact_state = false;
	}

	/*
	 * If we're in a subtransaction, stack up savepoints to match our level.
	 * This ensures we can rollback just the desired effects when a
	 * subtransaction aborts.
	 */
	while (entry->xact_depth < curlevel)
	{
		char		sql[64];

		snprintf(sql, sizeof(sql), "SAVEPOINT s%d", entry->xact_depth + 1);
		entry->changing_xact_state = true;
		do_sql_command(entry->conn, sql);
		entry->xact_depth++;
		entry->changing_xact_state = false;
	}
}

/*
 * Release connection reference count created by calling GetConnection.
 */
void
ReleaseConnection(PGconn *conn)
{
	/*
	 * Currently, we don't actually track connection references because all
	 * cleanup is managed on a transaction or subtransaction basis instead. So
	 * there's nothing to do here.
	 */
}

/*
 * Assign a "unique" number for a cursor.
 *
 * These really only need to be unique per connection within a transaction.
 * For the moment we ignore the per-connection point and assign them across
 * all connections in the transaction, but we ask for the connection to be
 * supplied in case we want to refine that.
 *
 * Note that even if wraparound happens in a very long transaction, actual
 * collisions are highly improbable; just be sure to use %u not %d to print.
 */
unsigned int
GetCursorNumber(PGconn *conn)
{
	return ++cursor_number;
}

/*
 * Assign a "unique" number for a prepared statement.
 *
 * This works much like GetCursorNumber, except that we never reset the counter
 * within a session.  That's because we can't be 100% sure we've gotten rid
 * of all prepared statements on all connections, and it's not really worth
 * increasing the risk of prepared-statement name collisions by resetting.
 */
unsigned int
GetPrepStmtNumber(PGconn *conn)
{
	return ++prep_stmt_number;
}

/*
 * Submit a query and wait for the result.
 *
 * Since we don't use non-blocking mode, this can't process interrupts while
 * pushing the query text to the server.  That risk is relatively small, so we
 * ignore that for now.
 *
 * Caller is responsible for the error handling on the result.
 */
PGresult *
pgfdw_exec_query(PGconn *conn, const char *query, PgFdwConnState *state)
{
	/* First, process a pending asynchronous request, if any. */
	if (state && state->pendingAreq)
		process_pending_request(state->pendingAreq);

	if (!PQsendQuery(conn, query))
		return NULL;
	return pgfdw_get_result(conn);
}

/*
 * Wrap libpqsrv_get_result_last(), adding wait event.
 *
 * Caller is responsible for the error handling on the result.
 */
PGresult *
pgfdw_get_result(PGconn *conn)
{
	return libpqsrv_get_result_last(conn, pgfdw_we_get_result);
}

/*
 * Report an error we got from the remote server.
 *
 * elevel: error level to use (typically ERROR, but might be less)
 * res: PGresult containing the error (might be NULL)
 * conn: connection we did the query on
 * sql: NULL, or text of remote command we tried to execute
 *
 * If "res" is not NULL, it'll be PQclear'ed here (unless we throw error,
 * in which case memory context cleanup will clear it eventually).
 *
 * Note: callers that choose not to throw ERROR for a remote error are
 * responsible for making sure that the associated ConnCacheEntry gets
 * marked with have_error = true.
 */
void
pgfdw_report_error(int elevel, PGresult *res, PGconn *conn,
				   const char *sql)
{
	char	   *diag_sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
	char	   *message_primary = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
	char	   *message_detail = PQresultErrorField(res, PG_DIAG_MESSAGE_DETAIL);
	char	   *message_hint = PQresultErrorField(res, PG_DIAG_MESSAGE_HINT);
	char	   *message_context = PQresultErrorField(res, PG_DIAG_CONTEXT);
	int			sqlstate;

	if (diag_sqlstate)
		sqlstate = MAKE_SQLSTATE(diag_sqlstate[0],
								 diag_sqlstate[1],
								 diag_sqlstate[2],
								 diag_sqlstate[3],
								 diag_sqlstate[4]);
	else
		sqlstate = ERRCODE_CONNECTION_FAILURE;

	/*
	 * If we don't get a message from the PGresult, try the PGconn.  This is
	 * needed because for connection-level failures, PQgetResult may just
	 * return NULL, not a PGresult at all.
	 */
	if (message_primary == NULL)
		message_primary = pchomp(PQerrorMessage(conn));

	ereport(elevel,
			(errcode(sqlstate),
			 (message_primary != NULL && message_primary[0] != '\0') ?
			 errmsg_internal("%s", message_primary) :
			 errmsg("could not obtain message string for remote error"),
			 message_detail ? errdetail_internal("%s", message_detail) : 0,
			 message_hint ? errhint("%s", message_hint) : 0,
			 message_context ? errcontext("%s", message_context) : 0,
			 sql ? errcontext("remote SQL command: %s", sql) : 0));
	PQclear(res);
}

/*
 * pgfdw_xact_callback --- cleanup at main-transaction end.
 *
 * This runs just late enough that it must not enter user-defined code
 * locally.  (Entering such code on the remote side is fine.  Its remote
 * COMMIT TRANSACTION may run deferred triggers.)
 */
static void
pgfdw_xact_callback(XactEvent event, void *arg)
{
	HASH_SEQ_STATUS scan;
	ConnCacheEntry *entry;
	List	   *pending_entries = NIL;
	List	   *cancel_requested = NIL;

	/* Quick exit if no connections were touched in this transaction. */
	if (!xact_got_connection)
		return;

	/*
	 * Scan all connection cache entries to find open remote transactions, and
	 * close them.
	 */
	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		PGresult   *res;

		/* Ignore cache entry if no open connection right now */
		if (entry->conn == NULL)
			continue;

		/* If it has an open remote transaction, try to close it */
		if (entry->xact_depth > 0)
		{
			elog(DEBUG3, "closing remote transaction on connection %p",
				 entry->conn);

			switch (event)
			{
				case XACT_EVENT_PARALLEL_PRE_COMMIT:
				case XACT_EVENT_PRE_COMMIT:

					/*
					 * If abort cleanup previously failed for this connection,
					 * we can't issue any more commands against it.
					 */
					pgfdw_reject_incomplete_xact_state_change(entry);

					/* Commit all remote transactions during pre-commit */
					entry->changing_xact_state = true;
					if (entry->parallel_commit)
					{
						do_sql_command_begin(entry->conn, "COMMIT TRANSACTION");
						pending_entries = lappend(pending_entries, entry);
						continue;
					}
					do_sql_command(entry->conn, "COMMIT TRANSACTION");
					entry->changing_xact_state = false;

					/*
					 * If there were any errors in subtransactions, and we
					 * made prepared statements, do a DEALLOCATE ALL to make
					 * sure we get rid of all prepared statements. This is
					 * annoying and not terribly bulletproof, but it's
					 * probably not worth trying harder.
					 *
					 * DEALLOCATE ALL only exists in 8.3 and later, so this
					 * constrains how old a server postgres_fdw can
					 * communicate with.  We intentionally ignore errors in
					 * the DEALLOCATE, so that we can hobble along to some
					 * extent with older servers (leaking prepared statements
					 * as we go; but we don't really support update operations
					 * pre-8.3 anyway).
					 */
					if (entry->have_prep_stmt && entry->have_error)
					{
						res = pgfdw_exec_query(entry->conn, "DEALLOCATE ALL",
											   NULL);
						PQclear(res);
					}
					entry->have_prep_stmt = false;
					entry->have_error = false;
					break;
				case XACT_EVENT_PRE_PREPARE:

					/*
					 * We disallow any remote transactions, since it's not
					 * very reasonable to hold them open until the prepared
					 * transaction is committed.  For the moment, throw error
					 * unconditionally; later we might allow read-only cases.
					 * Note that the error will cause us to come right back
					 * here with event == XACT_EVENT_ABORT, so we'll clean up
					 * the connection state at that point.
					 */
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot PREPARE a transaction that has operated on postgres_fdw foreign tables")));
					break;
				case XACT_EVENT_PARALLEL_COMMIT:
				case XACT_EVENT_COMMIT:
				case XACT_EVENT_PREPARE:
					/* Pre-commit should have closed the open transaction */
					elog(ERROR, "missed cleaning up connection during pre-commit");
					break;
				case XACT_EVENT_PARALLEL_ABORT:
				case XACT_EVENT_ABORT:
					/* Rollback all remote transactions during abort */
					if (entry->parallel_abort)
					{
						if (pgfdw_abort_cleanup_begin(entry, true,
													  &pending_entries,
													  &cancel_requested))
							continue;
					}
					else
						pgfdw_abort_cleanup(entry, true);
					break;
			}
		}

		/* Reset state to show we're out of a transaction */
		pgfdw_reset_xact_state(entry, true);
	}

	/* If there are any pending connections, finish cleaning them up */
	if (pending_entries || cancel_requested)
	{
		if (event == XACT_EVENT_PARALLEL_PRE_COMMIT ||
			event == XACT_EVENT_PRE_COMMIT)
		{
			Assert(cancel_requested == NIL);
			pgfdw_finish_pre_commit_cleanup(pending_entries);
		}
		else
		{
			Assert(event == XACT_EVENT_PARALLEL_ABORT ||
				   event == XACT_EVENT_ABORT);
			pgfdw_finish_abort_cleanup(pending_entries, cancel_requested,
									   true);
		}
	}

	/*
	 * Regardless of the event type, we can now mark ourselves as out of the
	 * transaction.  (Note: if we are here during PRE_COMMIT or PRE_PREPARE,
	 * this saves a useless scan of the hashtable during COMMIT or PREPARE.)
	 */
	xact_got_connection = false;

	/* Also reset cursor numbering for next transaction */
	cursor_number = 0;
}

/*
 * pgfdw_subxact_callback --- cleanup at subtransaction end.
 */
static void
pgfdw_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
					   SubTransactionId parentSubid, void *arg)
{
	HASH_SEQ_STATUS scan;
	ConnCacheEntry *entry;
	int			curlevel;
	List	   *pending_entries = NIL;
	List	   *cancel_requested = NIL;

	/* Nothing to do at subxact start, nor after commit. */
	if (!(event == SUBXACT_EVENT_PRE_COMMIT_SUB ||
		  event == SUBXACT_EVENT_ABORT_SUB))
		return;

	/* Quick exit if no connections were touched in this transaction. */
	if (!xact_got_connection)
		return;

	/*
	 * Scan all connection cache entries to find open remote subtransactions
	 * of the current level, and close them.
	 */
	curlevel = GetCurrentTransactionNestLevel();
	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		char		sql[100];

		/*
		 * We only care about connections with open remote subtransactions of
		 * the current level.
		 */
		if (entry->conn == NULL || entry->xact_depth < curlevel)
			continue;

		if (entry->xact_depth > curlevel)
			elog(ERROR, "missed cleaning up remote subtransaction at level %d",
				 entry->xact_depth);

		if (event == SUBXACT_EVENT_PRE_COMMIT_SUB)
		{
			/*
			 * If abort cleanup previously failed for this connection, we
			 * can't issue any more commands against it.
			 */
			pgfdw_reject_incomplete_xact_state_change(entry);

			/* Commit all remote subtransactions during pre-commit */
			snprintf(sql, sizeof(sql), "RELEASE SAVEPOINT s%d", curlevel);
			entry->changing_xact_state = true;
			if (entry->parallel_commit)
			{
				do_sql_command_begin(entry->conn, sql);
				pending_entries = lappend(pending_entries, entry);
				continue;
			}
			do_sql_command(entry->conn, sql);
			entry->changing_xact_state = false;
		}
		else
		{
			/* Rollback all remote subtransactions during abort */
			if (entry->parallel_abort)
			{
				if (pgfdw_abort_cleanup_begin(entry, false,
											  &pending_entries,
											  &cancel_requested))
					continue;
			}
			else
				pgfdw_abort_cleanup(entry, false);
		}

		/* OK, we're outta that level of subtransaction */
		pgfdw_reset_xact_state(entry, false);
	}

	/* If there are any pending connections, finish cleaning them up */
	if (pending_entries || cancel_requested)
	{
		if (event == SUBXACT_EVENT_PRE_COMMIT_SUB)
		{
			Assert(cancel_requested == NIL);
			pgfdw_finish_pre_subcommit_cleanup(pending_entries, curlevel);
		}
		else
		{
			Assert(event == SUBXACT_EVENT_ABORT_SUB);
			pgfdw_finish_abort_cleanup(pending_entries, cancel_requested,
									   false);
		}
	}
}

/*
 * Connection invalidation callback function
 *
 * After a change to a pg_foreign_server or pg_user_mapping catalog entry,
 * close connections depending on that entry immediately if current transaction
 * has not used those connections yet. Otherwise, mark those connections as
 * invalid and then make pgfdw_xact_callback() close them at the end of current
 * transaction, since they cannot be closed in the midst of the transaction
 * using them. Closed connections will be remade at the next opportunity if
 * necessary.
 *
 * Although most cache invalidation callbacks blow away all the related stuff
 * regardless of the given hashvalue, connections are expensive enough that
 * it's worth trying to avoid that.
 *
 * NB: We could avoid unnecessary disconnection more strictly by examining
 * individual option values, but it seems too much effort for the gain.
 */
static void
pgfdw_inval_callback(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS scan;
	ConnCacheEntry *entry;

	Assert(cacheid == FOREIGNSERVEROID || cacheid == USERMAPPINGOID);

	/* ConnectionHash must exist already, if we're registered */
	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		/* Ignore invalid entries */
		if (entry->conn == NULL)
			continue;

		/* hashvalue == 0 means a cache reset, must clear all state */
		if (hashvalue == 0 ||
			(cacheid == FOREIGNSERVEROID &&
			 entry->server_hashvalue == hashvalue) ||
			(cacheid == USERMAPPINGOID &&
			 entry->mapping_hashvalue == hashvalue))
		{
			/*
			 * Close the connection immediately if it's not used yet in this
			 * transaction. Otherwise mark it as invalid so that
			 * pgfdw_xact_callback() can close it at the end of this
			 * transaction.
			 */
			if (entry->xact_depth == 0)
			{
				elog(DEBUG3, "discarding connection %p", entry->conn);
				disconnect_pg_server(entry);
			}
			else
				entry->invalidated = true;
		}
	}
}

/*
 * Raise an error if the given connection cache entry is marked as being
 * in the middle of an xact state change.  This should be called at which no
 * such change is expected to be in progress; if one is found to be in
 * progress, it means that we aborted in the middle of a previous state change
 * and now don't know what the remote transaction state actually is.
 * Such connections can't safely be further used.  Re-establishing the
 * connection would change the snapshot and roll back any writes already
 * performed, so that's not an option, either. Thus, we must abort.
 */
static void
pgfdw_reject_incomplete_xact_state_change(ConnCacheEntry *entry)
{
	ForeignServer *server;

	/* nothing to do for inactive entries and entries of sane state */
	if (entry->conn == NULL || !entry->changing_xact_state)
		return;

	/* make sure this entry is inactive */
	disconnect_pg_server(entry);

	/* find server name to be shown in the message below */
	server = GetForeignServer(entry->serverid);

	ereport(ERROR,
			(errcode(ERRCODE_CONNECTION_EXCEPTION),
			 errmsg("connection to server \"%s\" was lost",
					server->servername)));
}

/*
 * Reset state to show we're out of a (sub)transaction.
 */
static void
pgfdw_reset_xact_state(ConnCacheEntry *entry, bool toplevel)
{
	if (toplevel)
	{
		/* Reset state to show we're out of a transaction */
		entry->xact_depth = 0;

		/*
		 * If the connection isn't in a good idle state, it is marked as
		 * invalid or keep_connections option of its server is disabled, then
		 * discard it to recover. Next GetConnection will open a new
		 * connection.
		 */
		if (PQstatus(entry->conn) != CONNECTION_OK ||
			PQtransactionStatus(entry->conn) != PQTRANS_IDLE ||
			entry->changing_xact_state ||
			entry->invalidated ||
			!entry->keep_connections)
		{
			elog(DEBUG3, "discarding connection %p", entry->conn);
			disconnect_pg_server(entry);
		}
	}
	else
	{
		/* Reset state to show we're out of a subtransaction */
		entry->xact_depth--;
	}
}

/*
 * Cancel the currently-in-progress query (whose query text we do not have)
 * and ignore the result.  Returns true if we successfully cancel the query
 * and discard any pending result, and false if not.
 *
 * It's not a huge problem if we throw an ERROR here, but if we get into error
 * recursion trouble, we'll end up slamming the connection shut, which will
 * necessitate failing the entire toplevel transaction even if subtransactions
 * were used.  Try to use WARNING where we can.
 *
 * XXX: if the query was one sent by fetch_more_data_begin(), we could get the
 * query text from the pendingAreq saved in the per-connection state, then
 * report the query using it.
 */
static bool
pgfdw_cancel_query(PGconn *conn)
{
	TimestampTz now = GetCurrentTimestamp();
	TimestampTz endtime;
	TimestampTz retrycanceltime;

	/*
	 * If it takes too long to cancel the query and discard the result, assume
	 * the connection is dead.
	 */
	endtime = TimestampTzPlusMilliseconds(now, CONNECTION_CLEANUP_TIMEOUT);

	/*
	 * Also, lose patience and re-issue the cancel request after a little bit.
	 * (This serves to close some race conditions.)
	 */
	retrycanceltime = TimestampTzPlusMilliseconds(now, RETRY_CANCEL_TIMEOUT);

	if (!pgfdw_cancel_query_begin(conn, endtime))
		return false;
	return pgfdw_cancel_query_end(conn, endtime, retrycanceltime, false);
}

/*
 * Submit a cancel request to the given connection, waiting only until
 * the given time.
 *
 * We sleep interruptibly until we receive confirmation that the cancel
 * request has been accepted, and if it is, return true; if the timeout
 * lapses without that, or the request fails for whatever reason, return
 * false.
 */
static bool
pgfdw_cancel_query_begin(PGconn *conn, TimestampTz endtime)
{
	const char *errormsg = libpqsrv_cancel(conn, endtime);

	if (errormsg != NULL)
		ereport(WARNING,
				errcode(ERRCODE_CONNECTION_FAILURE),
				errmsg("could not send cancel request: %s", errormsg));

	return errormsg == NULL;
}

static bool
pgfdw_cancel_query_end(PGconn *conn, TimestampTz endtime,
					   TimestampTz retrycanceltime, bool consume_input)
{
	PGresult   *result;
	bool		timed_out;

	/*
	 * If requested, consume whatever data is available from the socket. (Note
	 * that if all data is available, this allows pgfdw_get_cleanup_result to
	 * call PQgetResult without forcing the overhead of WaitLatchOrSocket,
	 * which would be large compared to the overhead of PQconsumeInput.)
	 */
	if (consume_input && !PQconsumeInput(conn))
	{
		ereport(WARNING,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not get result of cancel request: %s",
						pchomp(PQerrorMessage(conn)))));
		return false;
	}

	/* Get and discard the result of the query. */
	if (pgfdw_get_cleanup_result(conn, endtime, retrycanceltime,
								 &result, &timed_out))
	{
		if (timed_out)
			ereport(WARNING,
					(errmsg("could not get result of cancel request due to timeout")));
		else
			ereport(WARNING,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("could not get result of cancel request: %s",
							pchomp(PQerrorMessage(conn)))));

		return false;
	}
	PQclear(result);

	return true;
}

/*
 * Submit a query during (sub)abort cleanup and wait up to 30 seconds for the
 * result.  If the query is executed without error, the return value is true.
 * If the query is executed successfully but returns an error, the return
 * value is true if and only if ignore_errors is set.  If the query can't be
 * sent or times out, the return value is false.
 *
 * It's not a huge problem if we throw an ERROR here, but if we get into error
 * recursion trouble, we'll end up slamming the connection shut, which will
 * necessitate failing the entire toplevel transaction even if subtransactions
 * were used.  Try to use WARNING where we can.
 */
static bool
pgfdw_exec_cleanup_query(PGconn *conn, const char *query, bool ignore_errors)
{
	TimestampTz endtime;

	/*
	 * If it takes too long to execute a cleanup query, assume the connection
	 * is dead.  It's fairly likely that this is why we aborted in the first
	 * place (e.g. statement timeout, user cancel), so the timeout shouldn't
	 * be too long.
	 */
	endtime = TimestampTzPlusMilliseconds(GetCurrentTimestamp(),
										  CONNECTION_CLEANUP_TIMEOUT);

	if (!pgfdw_exec_cleanup_query_begin(conn, query))
		return false;
	return pgfdw_exec_cleanup_query_end(conn, query, endtime,
										false, ignore_errors);
}

static bool
pgfdw_exec_cleanup_query_begin(PGconn *conn, const char *query)
{
	Assert(query != NULL);

	/*
	 * Submit a query.  Since we don't use non-blocking mode, this also can
	 * block.  But its risk is relatively small, so we ignore that for now.
	 */
	if (!PQsendQuery(conn, query))
	{
		pgfdw_report_error(WARNING, NULL, conn, query);
		return false;
	}

	return true;
}

static bool
pgfdw_exec_cleanup_query_end(PGconn *conn, const char *query,
							 TimestampTz endtime, bool consume_input,
							 bool ignore_errors)
{
	PGresult   *result;
	bool		timed_out;

	Assert(query != NULL);

	/*
	 * If requested, consume whatever data is available from the socket. (Note
	 * that if all data is available, this allows pgfdw_get_cleanup_result to
	 * call PQgetResult without forcing the overhead of WaitLatchOrSocket,
	 * which would be large compared to the overhead of PQconsumeInput.)
	 */
	if (consume_input && !PQconsumeInput(conn))
	{
		pgfdw_report_error(WARNING, NULL, conn, query);
		return false;
	}

	/* Get the result of the query. */
	if (pgfdw_get_cleanup_result(conn, endtime, endtime, &result, &timed_out))
	{
		if (timed_out)
			ereport(WARNING,
					(errmsg("could not get query result due to timeout"),
					 errcontext("remote SQL command: %s", query)));
		else
			pgfdw_report_error(WARNING, NULL, conn, query);

		return false;
	}

	/* Issue a warning if not successful. */
	if (PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		pgfdw_report_error(WARNING, result, conn, query);
		return ignore_errors;
	}
	PQclear(result);

	return true;
}

/*
 * Get, during abort cleanup, the result of a query that is in progress.
 * This might be a query that is being interrupted by a cancel request or by
 * transaction abort, or it might be a query that was initiated as part of
 * transaction abort to get the remote side back to the appropriate state.
 *
 * endtime is the time at which we should give up and assume the remote side
 * is dead.  retrycanceltime is the time at which we should issue a fresh
 * cancel request (pass the same value as endtime if this is not wanted).
 *
 * Returns true if the timeout expired or connection trouble occurred,
 * false otherwise.  Sets *result except in case of a true result.
 * Sets *timed_out to true only when the timeout expired.
 */
static bool
pgfdw_get_cleanup_result(PGconn *conn, TimestampTz endtime,
						 TimestampTz retrycanceltime,
						 PGresult **result,
						 bool *timed_out)
{
	bool		failed = false;
	PGresult   *last_res = NULL;
	int			canceldelta = RETRY_CANCEL_TIMEOUT * 2;

	*result = NULL;
	*timed_out = false;
	for (;;)
	{
		PGresult   *res;

		while (PQisBusy(conn))
		{
			int			wc;
			TimestampTz now = GetCurrentTimestamp();
			long		cur_timeout;

			/* If timeout has expired, give up. */
			if (now >= endtime)
			{
				*timed_out = true;
				failed = true;
				goto exit;
			}

			/* If we need to re-issue the cancel request, do that. */
			if (now >= retrycanceltime)
			{
				/* We ignore failure to issue the repeated request. */
				(void) libpqsrv_cancel(conn, endtime);

				/* Recompute "now" in case that took measurable time. */
				now = GetCurrentTimestamp();

				/* Adjust re-cancel timeout in increasing steps. */
				retrycanceltime = TimestampTzPlusMilliseconds(now,
															  canceldelta);
				canceldelta += canceldelta;
			}

			/* If timeout has expired, give up, else get sleep time. */
			cur_timeout = TimestampDifferenceMilliseconds(now,
														  Min(endtime,
															  retrycanceltime));
			if (cur_timeout <= 0)
			{
				*timed_out = true;
				failed = true;
				goto exit;
			}

			/* first time, allocate or get the custom wait event */
			if (pgfdw_we_cleanup_result == 0)
				pgfdw_we_cleanup_result = WaitEventExtensionNew("PostgresFdwCleanupResult");

			/* Sleep until there's something to do */
			wc = WaitLatchOrSocket(MyLatch,
								   WL_LATCH_SET | WL_SOCKET_READABLE |
								   WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
								   PQsocket(conn),
								   cur_timeout, pgfdw_we_cleanup_result);
			ResetLatch(MyLatch);

			CHECK_FOR_INTERRUPTS();

			/* Data available in socket? */
			if (wc & WL_SOCKET_READABLE)
			{
				if (!PQconsumeInput(conn))
				{
					/* connection trouble */
					failed = true;
					goto exit;
				}
			}
		}

		res = PQgetResult(conn);
		if (res == NULL)
			break;				/* query is complete */

		PQclear(last_res);
		last_res = res;
	}
exit:
	if (failed)
		PQclear(last_res);
	else
		*result = last_res;
	return failed;
}

/*
 * Abort remote transaction or subtransaction.
 *
 * "toplevel" should be set to true if toplevel (main) transaction is
 * rollbacked, false otherwise.
 *
 * Set entry->changing_xact_state to false on success, true on failure.
 */
static void
pgfdw_abort_cleanup(ConnCacheEntry *entry, bool toplevel)
{
	char		sql[100];

	/*
	 * Don't try to clean up the connection if we're already in error
	 * recursion trouble.
	 */
	if (in_error_recursion_trouble())
		entry->changing_xact_state = true;

	/*
	 * If connection is already unsalvageable, don't touch it further.
	 */
	if (entry->changing_xact_state)
		return;

	/*
	 * Mark this connection as in the process of changing transaction state.
	 */
	entry->changing_xact_state = true;

	/* Assume we might have lost track of prepared statements */
	entry->have_error = true;

	/*
	 * If a command has been submitted to the remote server by using an
	 * asynchronous execution function, the command might not have yet
	 * completed.  Check to see if a command is still being processed by the
	 * remote server, and if so, request cancellation of the command.
	 */
	if (PQtransactionStatus(entry->conn) == PQTRANS_ACTIVE &&
		!pgfdw_cancel_query(entry->conn))
		return;					/* Unable to cancel running query */

	CONSTRUCT_ABORT_COMMAND(sql, entry, toplevel);
	if (!pgfdw_exec_cleanup_query(entry->conn, sql, false))
		return;					/* Unable to abort remote (sub)transaction */

	if (toplevel)
	{
		if (entry->have_prep_stmt && entry->have_error &&
			!pgfdw_exec_cleanup_query(entry->conn,
									  "DEALLOCATE ALL",
									  true))
			return;				/* Trouble clearing prepared statements */

		entry->have_prep_stmt = false;
		entry->have_error = false;
	}

	/*
	 * If pendingAreq of the per-connection state is not NULL, it means that
	 * an asynchronous fetch begun by fetch_more_data_begin() was not done
	 * successfully and thus the per-connection state was not reset in
	 * fetch_more_data(); in that case reset the per-connection state here.
	 */
	if (entry->state.pendingAreq)
		memset(&entry->state, 0, sizeof(entry->state));

	/* Disarm changing_xact_state if it all worked */
	entry->changing_xact_state = false;
}

/*
 * Like pgfdw_abort_cleanup, submit an abort command or cancel request, but
 * don't wait for the result.
 *
 * Returns true if the abort command or cancel request is successfully issued,
 * false otherwise.  If the abort command is successfully issued, the given
 * connection cache entry is appended to *pending_entries.  Otherwise, if the
 * cancel request is successfully issued, it is appended to *cancel_requested.
 */
static bool
pgfdw_abort_cleanup_begin(ConnCacheEntry *entry, bool toplevel,
						  List **pending_entries, List **cancel_requested)
{
	/*
	 * Don't try to clean up the connection if we're already in error
	 * recursion trouble.
	 */
	if (in_error_recursion_trouble())
		entry->changing_xact_state = true;

	/*
	 * If connection is already unsalvageable, don't touch it further.
	 */
	if (entry->changing_xact_state)
		return false;

	/*
	 * Mark this connection as in the process of changing transaction state.
	 */
	entry->changing_xact_state = true;

	/* Assume we might have lost track of prepared statements */
	entry->have_error = true;

	/*
	 * If a command has been submitted to the remote server by using an
	 * asynchronous execution function, the command might not have yet
	 * completed.  Check to see if a command is still being processed by the
	 * remote server, and if so, request cancellation of the command.
	 */
	if (PQtransactionStatus(entry->conn) == PQTRANS_ACTIVE)
	{
		TimestampTz endtime;

		endtime = TimestampTzPlusMilliseconds(GetCurrentTimestamp(),
											  CONNECTION_CLEANUP_TIMEOUT);
		if (!pgfdw_cancel_query_begin(entry->conn, endtime))
			return false;		/* Unable to cancel running query */
		*cancel_requested = lappend(*cancel_requested, entry);
	}
	else
	{
		char		sql[100];

		CONSTRUCT_ABORT_COMMAND(sql, entry, toplevel);
		if (!pgfdw_exec_cleanup_query_begin(entry->conn, sql))
			return false;		/* Unable to abort remote transaction */
		*pending_entries = lappend(*pending_entries, entry);
	}

	return true;
}

/*
 * Finish pre-commit cleanup of connections on each of which we've sent a
 * COMMIT command to the remote server.
 */
static void
pgfdw_finish_pre_commit_cleanup(List *pending_entries)
{
	ConnCacheEntry *entry;
	List	   *pending_deallocs = NIL;
	ListCell   *lc;

	Assert(pending_entries);

	/*
	 * Get the result of the COMMIT command for each of the pending entries
	 */
	foreach(lc, pending_entries)
	{
		entry = (ConnCacheEntry *) lfirst(lc);

		Assert(entry->changing_xact_state);

		/*
		 * We might already have received the result on the socket, so pass
		 * consume_input=true to try to consume it first
		 */
		do_sql_command_end(entry->conn, "COMMIT TRANSACTION", true);
		entry->changing_xact_state = false;

		/* Do a DEALLOCATE ALL in parallel if needed */
		if (entry->have_prep_stmt && entry->have_error)
		{
			/* Ignore errors (see notes in pgfdw_xact_callback) */
			if (PQsendQuery(entry->conn, "DEALLOCATE ALL"))
			{
				pending_deallocs = lappend(pending_deallocs, entry);
				continue;
			}
		}
		entry->have_prep_stmt = false;
		entry->have_error = false;

		pgfdw_reset_xact_state(entry, true);
	}

	/* No further work if no pending entries */
	if (!pending_deallocs)
		return;

	/*
	 * Get the result of the DEALLOCATE command for each of the pending
	 * entries
	 */
	foreach(lc, pending_deallocs)
	{
		PGresult   *res;

		entry = (ConnCacheEntry *) lfirst(lc);

		/* Ignore errors (see notes in pgfdw_xact_callback) */
		while ((res = PQgetResult(entry->conn)) != NULL)
		{
			PQclear(res);
			/* Stop if the connection is lost (else we'll loop infinitely) */
			if (PQstatus(entry->conn) == CONNECTION_BAD)
				break;
		}
		entry->have_prep_stmt = false;
		entry->have_error = false;

		pgfdw_reset_xact_state(entry, true);
	}
}

/*
 * Finish pre-subcommit cleanup of connections on each of which we've sent a
 * RELEASE command to the remote server.
 */
static void
pgfdw_finish_pre_subcommit_cleanup(List *pending_entries, int curlevel)
{
	ConnCacheEntry *entry;
	char		sql[100];
	ListCell   *lc;

	Assert(pending_entries);

	/*
	 * Get the result of the RELEASE command for each of the pending entries
	 */
	snprintf(sql, sizeof(sql), "RELEASE SAVEPOINT s%d", curlevel);
	foreach(lc, pending_entries)
	{
		entry = (ConnCacheEntry *) lfirst(lc);

		Assert(entry->changing_xact_state);

		/*
		 * We might already have received the result on the socket, so pass
		 * consume_input=true to try to consume it first
		 */
		do_sql_command_end(entry->conn, sql, true);
		entry->changing_xact_state = false;

		pgfdw_reset_xact_state(entry, false);
	}
}

/*
 * Finish abort cleanup of connections on each of which we've sent an abort
 * command or cancel request to the remote server.
 */
static void
pgfdw_finish_abort_cleanup(List *pending_entries, List *cancel_requested,
						   bool toplevel)
{
	List	   *pending_deallocs = NIL;
	ListCell   *lc;

	/*
	 * For each of the pending cancel requests (if any), get and discard the
	 * result of the query, and submit an abort command to the remote server.
	 */
	if (cancel_requested)
	{
		foreach(lc, cancel_requested)
		{
			ConnCacheEntry *entry = (ConnCacheEntry *) lfirst(lc);
			TimestampTz now = GetCurrentTimestamp();
			TimestampTz endtime;
			TimestampTz retrycanceltime;
			char		sql[100];

			Assert(entry->changing_xact_state);

			/*
			 * Set end time.  You might think we should do this before issuing
			 * cancel request like in normal mode, but that is problematic,
			 * because if, for example, it took longer than 30 seconds to
			 * process the first few entries in the cancel_requested list, it
			 * would cause a timeout error when processing each of the
			 * remaining entries in the list, leading to slamming that entry's
			 * connection shut.
			 */
			endtime = TimestampTzPlusMilliseconds(now,
												  CONNECTION_CLEANUP_TIMEOUT);
			retrycanceltime = TimestampTzPlusMilliseconds(now,
														  RETRY_CANCEL_TIMEOUT);

			if (!pgfdw_cancel_query_end(entry->conn, endtime,
										retrycanceltime, true))
			{
				/* Unable to cancel running query */
				pgfdw_reset_xact_state(entry, toplevel);
				continue;
			}

			/* Send an abort command in parallel if needed */
			CONSTRUCT_ABORT_COMMAND(sql, entry, toplevel);
			if (!pgfdw_exec_cleanup_query_begin(entry->conn, sql))
			{
				/* Unable to abort remote (sub)transaction */
				pgfdw_reset_xact_state(entry, toplevel);
			}
			else
				pending_entries = lappend(pending_entries, entry);
		}
	}

	/* No further work if no pending entries */
	if (!pending_entries)
		return;

	/*
	 * Get the result of the abort command for each of the pending entries
	 */
	foreach(lc, pending_entries)
	{
		ConnCacheEntry *entry = (ConnCacheEntry *) lfirst(lc);
		TimestampTz endtime;
		char		sql[100];

		Assert(entry->changing_xact_state);

		/*
		 * Set end time.  We do this now, not before issuing the command like
		 * in normal mode, for the same reason as for the cancel_requested
		 * entries.
		 */
		endtime = TimestampTzPlusMilliseconds(GetCurrentTimestamp(),
											  CONNECTION_CLEANUP_TIMEOUT);

		CONSTRUCT_ABORT_COMMAND(sql, entry, toplevel);
		if (!pgfdw_exec_cleanup_query_end(entry->conn, sql, endtime,
										  true, false))
		{
			/* Unable to abort remote (sub)transaction */
			pgfdw_reset_xact_state(entry, toplevel);
			continue;
		}

		if (toplevel)
		{
			/* Do a DEALLOCATE ALL in parallel if needed */
			if (entry->have_prep_stmt && entry->have_error)
			{
				if (!pgfdw_exec_cleanup_query_begin(entry->conn,
													"DEALLOCATE ALL"))
				{
					/* Trouble clearing prepared statements */
					pgfdw_reset_xact_state(entry, toplevel);
				}
				else
					pending_deallocs = lappend(pending_deallocs, entry);
				continue;
			}
			entry->have_prep_stmt = false;
			entry->have_error = false;
		}

		/* Reset the per-connection state if needed */
		if (entry->state.pendingAreq)
			memset(&entry->state, 0, sizeof(entry->state));

		/* We're done with this entry; unset the changing_xact_state flag */
		entry->changing_xact_state = false;
		pgfdw_reset_xact_state(entry, toplevel);
	}

	/* No further work if no pending entries */
	if (!pending_deallocs)
		return;
	Assert(toplevel);

	/*
	 * Get the result of the DEALLOCATE command for each of the pending
	 * entries
	 */
	foreach(lc, pending_deallocs)
	{
		ConnCacheEntry *entry = (ConnCacheEntry *) lfirst(lc);
		TimestampTz endtime;

		Assert(entry->changing_xact_state);
		Assert(entry->have_prep_stmt);
		Assert(entry->have_error);

		/*
		 * Set end time.  We do this now, not before issuing the command like
		 * in normal mode, for the same reason as for the cancel_requested
		 * entries.
		 */
		endtime = TimestampTzPlusMilliseconds(GetCurrentTimestamp(),
											  CONNECTION_CLEANUP_TIMEOUT);

		if (!pgfdw_exec_cleanup_query_end(entry->conn, "DEALLOCATE ALL",
										  endtime, true, true))
		{
			/* Trouble clearing prepared statements */
			pgfdw_reset_xact_state(entry, toplevel);
			continue;
		}
		entry->have_prep_stmt = false;
		entry->have_error = false;

		/* Reset the per-connection state if needed */
		if (entry->state.pendingAreq)
			memset(&entry->state, 0, sizeof(entry->state));

		/* We're done with this entry; unset the changing_xact_state flag */
		entry->changing_xact_state = false;
		pgfdw_reset_xact_state(entry, toplevel);
	}
}

/* Number of output arguments (columns) for various API versions */
#define POSTGRES_FDW_GET_CONNECTIONS_COLS_V1_1	2
#define POSTGRES_FDW_GET_CONNECTIONS_COLS_V1_2	6
#define POSTGRES_FDW_GET_CONNECTIONS_COLS	6	/* maximum of above */

/*
 * Internal function used by postgres_fdw_get_connections variants.
 *
 * For API version 1.1, this function takes no input parameter and
 * returns a set of records with the following values:
 *
 * - server_name - server name of active connection. In case the foreign server
 *   is dropped but still the connection is active, then the server name will
 *   be NULL in output.
 * - valid - true/false representing whether the connection is valid or not.
 *   Note that connections can become invalid in pgfdw_inval_callback.
 *
 * For API version 1.2 and later, this function takes an input parameter
 * to check a connection status and returns the following
 * additional values along with the four values from version 1.1:
 *
 * - user_name - the local user name of the active connection. In case the
 *   user mapping is dropped but the connection is still active, then the
 *   user name will be NULL in the output.
 * - used_in_xact - true if the connection is used in the current transaction.
 * - closed - true if the connection is closed.
 * - remote_backend_pid - process ID of the remote backend, on the foreign
 *   server, handling the connection.
 *
 * No records are returned when there are no cached connections at all.
 */
static void
postgres_fdw_get_connections_internal(FunctionCallInfo fcinfo,
									  enum pgfdwVersion api_version)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	HASH_SEQ_STATUS scan;
	ConnCacheEntry *entry;

	InitMaterializedSRF(fcinfo, 0);

	/* If cache doesn't exist, we return no records */
	if (!ConnectionHash)
		return;

	/* Check we have the expected number of output arguments */
	switch (rsinfo->setDesc->natts)
	{
		case POSTGRES_FDW_GET_CONNECTIONS_COLS_V1_1:
			if (api_version != PGFDW_V1_1)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case POSTGRES_FDW_GET_CONNECTIONS_COLS_V1_2:
			if (api_version != PGFDW_V1_2)
				elog(ERROR, "incorrect number of output arguments");
			break;
		default:
			elog(ERROR, "incorrect number of output arguments");
	}

	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		ForeignServer *server;
		Datum		values[POSTGRES_FDW_GET_CONNECTIONS_COLS] = {0};
		bool		nulls[POSTGRES_FDW_GET_CONNECTIONS_COLS] = {0};
		int			i = 0;

		/* We only look for open remote connections */
		if (!entry->conn)
			continue;

		server = GetForeignServerExtended(entry->serverid, FSV_MISSING_OK);

		/*
		 * The foreign server may have been dropped in current explicit
		 * transaction. It is not possible to drop the server from another
		 * session when the connection associated with it is in use in the
		 * current transaction, if tried so, the drop query in another session
		 * blocks until the current transaction finishes.
		 *
		 * Even though the server is dropped in the current transaction, the
		 * cache can still have associated active connection entry, say we
		 * call such connections dangling. Since we can not fetch the server
		 * name from system catalogs for dangling connections, instead we show
		 * NULL value for server name in output.
		 *
		 * We could have done better by storing the server name in the cache
		 * entry instead of server oid so that it could be used in the output.
		 * But the server name in each cache entry requires 64 bytes of
		 * memory, which is huge, when there are many cached connections and
		 * the use case i.e. dropping the foreign server within the explicit
		 * current transaction seems rare. So, we chose to show NULL value for
		 * server name in output.
		 *
		 * Such dangling connections get closed either in next use or at the
		 * end of current explicit transaction in pgfdw_xact_callback.
		 */
		if (!server)
		{
			/*
			 * If the server has been dropped in the current explicit
			 * transaction, then this entry would have been invalidated in
			 * pgfdw_inval_callback at the end of drop server command. Note
			 * that this connection would not have been closed in
			 * pgfdw_inval_callback because it is still being used in the
			 * current explicit transaction. So, assert that here.
			 */
			Assert(entry->conn && entry->xact_depth > 0 && entry->invalidated);

			/* Show null, if no server name was found */
			nulls[i++] = true;
		}
		else
			values[i++] = CStringGetTextDatum(server->servername);

		if (api_version >= PGFDW_V1_2)
		{
			HeapTuple	tp;

			/* Use the system cache to obtain the user mapping */
			tp = SearchSysCache1(USERMAPPINGOID, ObjectIdGetDatum(entry->key));

			/*
			 * Just like in the foreign server case, user mappings can also be
			 * dropped in the current explicit transaction. Therefore, the
			 * similar check as in the server case is required.
			 */
			if (!HeapTupleIsValid(tp))
			{
				/*
				 * If we reach here, this entry must have been invalidated in
				 * pgfdw_inval_callback, same as in the server case.
				 */
				Assert(entry->conn && entry->xact_depth > 0 &&
					   entry->invalidated);

				nulls[i++] = true;
			}
			else
			{
				Oid			userid;

				userid = ((Form_pg_user_mapping) GETSTRUCT(tp))->umuser;
				values[i++] = CStringGetTextDatum(MappingUserName(userid));
				ReleaseSysCache(tp);
			}
		}

		values[i++] = BoolGetDatum(!entry->invalidated);

		if (api_version >= PGFDW_V1_2)
		{
			bool		check_conn = PG_GETARG_BOOL(0);

			/* Is this connection used in the current transaction? */
			values[i++] = BoolGetDatum(entry->xact_depth > 0);

			/*
			 * If a connection status check is requested and supported, return
			 * whether the connection is closed. Otherwise, return NULL.
			 */
			if (check_conn && pgfdw_conn_checkable())
				values[i++] = BoolGetDatum(pgfdw_conn_check(entry->conn) != 0);
			else
				nulls[i++] = true;

			/* Return process ID of remote backend */
			values[i++] = Int32GetDatum(PQbackendPID(entry->conn));
		}

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
}

/*
 * List active foreign server connections.
 *
 * The SQL API of this function has changed multiple times, and will likely
 * do so again in future.  To support the case where a newer version of this
 * loadable module is being used with an old SQL declaration of the function,
 * we continue to support the older API versions.
 */
Datum
postgres_fdw_get_connections_1_2(PG_FUNCTION_ARGS)
{
	postgres_fdw_get_connections_internal(fcinfo, PGFDW_V1_2);

	PG_RETURN_VOID();
}

Datum
postgres_fdw_get_connections(PG_FUNCTION_ARGS)
{
	postgres_fdw_get_connections_internal(fcinfo, PGFDW_V1_1);

	PG_RETURN_VOID();
}

/*
 * Disconnect the specified cached connections.
 *
 * This function discards the open connections that are established by
 * postgres_fdw from the local session to the foreign server with
 * the given name. Note that there can be multiple connections to
 * the given server using different user mappings. If the connections
 * are used in the current local transaction, they are not disconnected
 * and warning messages are reported. This function returns true
 * if it disconnects at least one connection, otherwise false. If no
 * foreign server with the given name is found, an error is reported.
 */
Datum
postgres_fdw_disconnect(PG_FUNCTION_ARGS)
{
	ForeignServer *server;
	char	   *servername;

	servername = text_to_cstring(PG_GETARG_TEXT_PP(0));
	server = GetForeignServerByName(servername, false);

	PG_RETURN_BOOL(disconnect_cached_connections(server->serverid));
}

/*
 * Disconnect all the cached connections.
 *
 * This function discards all the open connections that are established by
 * postgres_fdw from the local session to the foreign servers.
 * If the connections are used in the current local transaction, they are
 * not disconnected and warning messages are reported. This function
 * returns true if it disconnects at least one connection, otherwise false.
 */
Datum
postgres_fdw_disconnect_all(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(disconnect_cached_connections(InvalidOid));
}

/*
 * Workhorse to disconnect cached connections.
 *
 * This function scans all the connection cache entries and disconnects
 * the open connections whose foreign server OID matches with
 * the specified one. If InvalidOid is specified, it disconnects all
 * the cached connections.
 *
 * This function emits a warning for each connection that's used in
 * the current transaction and doesn't close it. It returns true if
 * it disconnects at least one connection, otherwise false.
 *
 * Note that this function disconnects even the connections that are
 * established by other users in the same local session using different
 * user mappings. This leads even non-superuser to be able to close
 * the connections established by superusers in the same local session.
 *
 * XXX As of now we don't see any security risk doing this. But we should
 * set some restrictions on that, for example, prevent non-superuser
 * from closing the connections established by superusers even
 * in the same session?
 */
static bool
disconnect_cached_connections(Oid serverid)
{
	HASH_SEQ_STATUS scan;
	ConnCacheEntry *entry;
	bool		all = !OidIsValid(serverid);
	bool		result = false;

	/*
	 * Connection cache hashtable has not been initialized yet in this
	 * session, so return false.
	 */
	if (!ConnectionHash)
		return false;

	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		/* Ignore cache entry if no open connection right now. */
		if (!entry->conn)
			continue;

		if (all || entry->serverid == serverid)
		{
			/*
			 * Emit a warning because the connection to close is used in the
			 * current transaction and cannot be disconnected right now.
			 */
			if (entry->xact_depth > 0)
			{
				ForeignServer *server;

				server = GetForeignServerExtended(entry->serverid,
												  FSV_MISSING_OK);

				if (!server)
				{
					/*
					 * If the foreign server was dropped while its connection
					 * was used in the current transaction, the connection
					 * must have been marked as invalid by
					 * pgfdw_inval_callback at the end of DROP SERVER command.
					 */
					Assert(entry->invalidated);

					ereport(WARNING,
							(errmsg("cannot close dropped server connection because it is still in use")));
				}
				else
					ereport(WARNING,
							(errmsg("cannot close connection for server \"%s\" because it is still in use",
									server->servername)));
			}
			else
			{
				elog(DEBUG3, "discarding connection %p", entry->conn);
				disconnect_pg_server(entry);
				result = true;
			}
		}
	}

	return result;
}

/*
 * Check if the remote server closed the connection.
 *
 * Returns 1 if the connection is closed, -1 if an error occurred,
 * and 0 if it's not closed or if the connection check is unavailable
 * on this platform.
 */
static int
pgfdw_conn_check(PGconn *conn)
{
	int			sock = PQsocket(conn);

	if (PQstatus(conn) != CONNECTION_OK || sock == -1)
		return -1;

#if (defined(HAVE_POLL) && defined(POLLRDHUP))
	{
		struct pollfd input_fd;
		int			result;

		input_fd.fd = sock;
		input_fd.events = POLLRDHUP;
		input_fd.revents = 0;

		do
			result = poll(&input_fd, 1, 0);
		while (result < 0 && errno == EINTR);

		if (result < 0)
			return -1;

		return (input_fd.revents &
				(POLLRDHUP | POLLHUP | POLLERR | POLLNVAL)) ? 1 : 0;
	}
#else
	return 0;
#endif
}

/*
 * Check if connection status checking is available on this platform.
 *
 * Returns true if available, false otherwise.
 */
static bool
pgfdw_conn_checkable(void)
{
#if (defined(HAVE_POLL) && defined(POLLRDHUP))
	return true;
#else
	return false;
#endif
}

/*
 * Ensure that require_auth and SCRAM keys are correctly set on values. SCRAM
 * keys used to pass-through are coming from the initial connection from the
 * client with the server.
 *
 * All required SCRAM options are set by postgres_fdw, so we just need to
 * ensure that these options are not overwritten by the user.
 */
static bool
pgfdw_has_required_scram_options(const char **keywords, const char **values)
{
	bool		has_scram_server_key = false;
	bool		has_scram_client_key = false;
	bool		has_require_auth = false;
	bool		has_scram_keys = false;

	/*
	 * Continue iterating even if we found the keys that we need to validate
	 * to make sure that there is no other declaration of these keys that can
	 * overwrite the first.
	 */
	for (int i = 0; keywords[i] != NULL; i++)
	{
		if (strcmp(keywords[i], "scram_client_key") == 0)
		{
			if (values[i] != NULL && values[i][0] != '\0')
				has_scram_client_key = true;
			else
				has_scram_client_key = false;
		}

		if (strcmp(keywords[i], "scram_server_key") == 0)
		{
			if (values[i] != NULL && values[i][0] != '\0')
				has_scram_server_key = true;
			else
				has_scram_server_key = false;
		}

		if (strcmp(keywords[i], "require_auth") == 0)
		{
			if (values[i] != NULL && strcmp(values[i], "scram-sha-256") == 0)
				has_require_auth = true;
			else
				has_require_auth = false;
		}
	}

	has_scram_keys = has_scram_client_key && has_scram_server_key && MyProcPort->has_scram_keys;

	return (has_scram_keys && has_require_auth);
}
