/*-------------------------------------------------------------------------
 *
 * connection.c
 *		  Connection management functions for postgres_fdw
 *
 * Portions Copyright (c) 2012-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  contrib/postgres_fdw/connection.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/pg_user_mapping.h"
#include "commands/defrem.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postgres_fdw.h"
#include "storage/fd.h"
#include "storage/latch.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
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

/*
 * SQL functions
 */
PG_FUNCTION_INFO_V1(postgres_fdw_get_connections);
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
static bool pgfdw_exec_cleanup_query(PGconn *conn, const char *query,
									 bool ignore_errors);
static bool pgfdw_get_cleanup_result(PGconn *conn, TimestampTz endtime,
									 PGresult **result, bool *timed_out);
static void pgfdw_abort_cleanup(ConnCacheEntry *entry, bool toplevel);
static void pgfdw_finish_pre_commit_cleanup(List *pending_entries);
static void pgfdw_finish_pre_subcommit_cleanup(List *pending_entries,
											   int curlevel);
static bool UserMappingPasswordRequired(UserMapping *user);
static bool disconnect_cached_connections(Oid serverid);

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

		if (entry->conn == NULL)
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
	 * Also determine whether to commit (sub)transactions opened on the remote
	 * server in parallel at (sub)transaction end, which is disabled by
	 * default.
	 *
	 * Note: it's enough to determine these only when making a new connection
	 * because these settings for it are changed, it will be closed and
	 * re-made later.
	 */
	entry->keep_connections = true;
	entry->parallel_commit = false;
	foreach(lc, server->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "keep_connections") == 0)
			entry->keep_connections = defGetBoolean(def);
		else if (strcmp(def->defname, "parallel_commit") == 0)
			entry->parallel_commit = defGetBoolean(def);
	}

	/* Now try to make the connection */
	entry->conn = connect_pg_server(server, user);

	elog(DEBUG3, "new postgres_fdw connection %p for server \"%s\" (user mapping oid %u, userid %u)",
		 entry->conn, server->servername, user->umid, user->userid);
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
		 * end marker.
		 */
		n = list_length(server->options) + list_length(user->options) + 4;
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

		keywords[n] = values[n] = NULL;

		/* verify the set of connection parameters */
		check_conn_params(keywords, values, user);

		/*
		 * We must obey fd.c's limit on non-virtual file descriptors.  Assume
		 * that a PGconn represents one long-lived FD.  (Doing this here also
		 * ensures that VFDs are closed if needed to make room.)
		 */
		if (!AcquireExternalFD())
		{
#ifndef WIN32					/* can't write #if within ereport() macro */
			ereport(ERROR,
					(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
					 errmsg("could not connect to server \"%s\"",
							server->servername),
					 errdetail("There are too many open files on the local server."),
					 errhint("Raise the server's max_files_per_process and/or \"ulimit -n\" limits.")));
#else
			ereport(ERROR,
					(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
					 errmsg("could not connect to server \"%s\"",
							server->servername),
					 errdetail("There are too many open files on the local server."),
					 errhint("Raise the server's max_files_per_process setting.")));
#endif
		}

		/* OK to make connection */
		conn = PQconnectdbParams(keywords, values, false);

		if (!conn)
			ReleaseExternalFD();	/* because the PG_CATCH block won't */

		if (!conn || PQstatus(conn) != CONNECTION_OK)
			ereport(ERROR,
					(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
					 errmsg("could not connect to server \"%s\"",
							server->servername),
					 errdetail_internal("%s", pchomp(PQerrorMessage(conn)))));

		/*
		 * Check that non-superuser has used password to establish connection;
		 * otherwise, he's piggybacking on the postgres server's user
		 * identity. See also dblink_security_check() in contrib/dblink and
		 * check_conn_params.
		 */
		if (!superuser_arg(user->userid) && UserMappingPasswordRequired(user) &&
			!PQconnectionUsedPassword(conn))
			ereport(ERROR,
					(errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
					 errmsg("password is required"),
					 errdetail("Non-superuser cannot connect if the server does not request a password."),
					 errhint("Target server's authentication method must be changed or password_required=false set in the user mapping attributes.")));

		/* Prepare new session for use */
		configure_remote_session(conn);

		if (appname != NULL)
			pfree(appname);
		pfree(keywords);
		pfree(values);
	}
	PG_CATCH();
	{
		/* Release PGconn data structure if we managed to create one */
		if (conn)
		{
			PQfinish(conn);
			ReleaseExternalFD();
		}
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
		PQfinish(entry->conn);
		entry->conn = NULL;
		ReleaseExternalFD();
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

/*
 * For non-superusers, insist that the connstr specify a password.  This
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

	/* ok if params contain a non-empty password */
	for (i = 0; keywords[i] != NULL; i++)
	{
		if (strcmp(keywords[i], "password") == 0 && values[i][0] != '\0')
			return;
	}

	/* ok if the superuser explicitly said so at user mapping creation time */
	if (!UserMappingPasswordRequired(user))
		return;

	ereport(ERROR,
			(errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
			 errmsg("password is required"),
			 errdetail("Non-superusers must provide a password in the user mapping.")));
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
	 * server might use a different timezone database.  Instead, use UTC
	 * (quoted, because very old servers are picky about case).
	 */
	do_sql_command(conn, "SET timezone = 'UTC'");

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
		pgfdw_report_error(ERROR, NULL, conn, false, sql);
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
		pgfdw_report_error(ERROR, NULL, conn, false, sql);
	res = pgfdw_get_result(conn, sql);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pgfdw_report_error(ERROR, res, conn, true, sql);
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
 * This function is interruptible by signals.
 *
 * Caller is responsible for the error handling on the result.
 */
PGresult *
pgfdw_exec_query(PGconn *conn, const char *query, PgFdwConnState *state)
{
	/* First, process a pending asynchronous request, if any. */
	if (state && state->pendingAreq)
		process_pending_request(state->pendingAreq);

	/*
	 * Submit a query.  Since we don't use non-blocking mode, this also can
	 * block.  But its risk is relatively small, so we ignore that for now.
	 */
	if (!PQsendQuery(conn, query))
		pgfdw_report_error(ERROR, NULL, conn, false, query);

	/* Wait for the result. */
	return pgfdw_get_result(conn, query);
}

/*
 * Wait for the result from a prior asynchronous execution function call.
 *
 * This function offers quick responsiveness by checking for any interruptions.
 *
 * This function emulates PQexec()'s behavior of returning the last result
 * when there are many.
 *
 * Caller is responsible for the error handling on the result.
 */
PGresult *
pgfdw_get_result(PGconn *conn, const char *query)
{
	PGresult   *volatile last_res = NULL;

	/* In what follows, do not leak any PGresults on an error. */
	PG_TRY();
	{
		for (;;)
		{
			PGresult   *res;

			while (PQisBusy(conn))
			{
				int			wc;

				/* Sleep until there's something to do */
				wc = WaitLatchOrSocket(MyLatch,
									   WL_LATCH_SET | WL_SOCKET_READABLE |
									   WL_EXIT_ON_PM_DEATH,
									   PQsocket(conn),
									   -1L, PG_WAIT_EXTENSION);
				ResetLatch(MyLatch);

				CHECK_FOR_INTERRUPTS();

				/* Data available in socket? */
				if (wc & WL_SOCKET_READABLE)
				{
					if (!PQconsumeInput(conn))
						pgfdw_report_error(ERROR, NULL, conn, false, query);
				}
			}

			res = PQgetResult(conn);
			if (res == NULL)
				break;			/* query is complete */

			PQclear(last_res);
			last_res = res;
		}
	}
	PG_CATCH();
	{
		PQclear(last_res);
		PG_RE_THROW();
	}
	PG_END_TRY();

	return last_res;
}

/*
 * Report an error we got from the remote server.
 *
 * elevel: error level to use (typically ERROR, but might be less)
 * res: PGresult containing the error
 * conn: connection we did the query on
 * clear: if true, PQclear the result (otherwise caller will handle it)
 * sql: NULL, or text of remote command we tried to execute
 *
 * Note: callers that choose not to throw ERROR for a remote error are
 * responsible for making sure that the associated ConnCacheEntry gets
 * marked with have_error = true.
 */
void
pgfdw_report_error(int elevel, PGresult *res, PGconn *conn,
				   bool clear, const char *sql)
{
	/* If requested, PGresult must be released before leaving this function. */
	PG_TRY();
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
		 * If we don't get a message from the PGresult, try the PGconn.  This
		 * is needed because for connection-level failures, PQexec may just
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
	}
	PG_FINALLY();
	{
		if (clear)
			PQclear(res);
	}
	PG_END_TRY();
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
						res = PQexec(entry->conn, "DEALLOCATE ALL");
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
					pgfdw_abort_cleanup(entry, true);
					break;
			}
		}

		/* Reset state to show we're out of a transaction */
		pgfdw_reset_xact_state(entry, true);
	}

	/* If there are any pending connections, finish cleaning them up */
	if (pending_entries)
	{
		Assert(event == XACT_EVENT_PARALLEL_PRE_COMMIT ||
			   event == XACT_EVENT_PRE_COMMIT);
		pgfdw_finish_pre_commit_cleanup(pending_entries);
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
			pgfdw_abort_cleanup(entry, false);
		}

		/* OK, we're outta that level of subtransaction */
		pgfdw_reset_xact_state(entry, false);
	}

	/* If there are any pending connections, finish cleaning them up */
	if (pending_entries)
	{
		Assert(event == SUBXACT_EVENT_PRE_COMMIT_SUB);
		pgfdw_finish_pre_subcommit_cleanup(pending_entries, curlevel);
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
	PGcancel   *cancel;
	char		errbuf[256];
	PGresult   *result = NULL;
	TimestampTz endtime;
	bool		timed_out;

	/*
	 * If it takes too long to cancel the query and discard the result, assume
	 * the connection is dead.
	 */
	endtime = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), 30000);

	/*
	 * Issue cancel request.  Unfortunately, there's no good way to limit the
	 * amount of time that we might block inside PQgetCancel().
	 */
	if ((cancel = PQgetCancel(conn)))
	{
		if (!PQcancel(cancel, errbuf, sizeof(errbuf)))
		{
			ereport(WARNING,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("could not send cancel request: %s",
							errbuf)));
			PQfreeCancel(cancel);
			return false;
		}
		PQfreeCancel(cancel);
	}

	/* Get and discard the result of the query. */
	if (pgfdw_get_cleanup_result(conn, endtime, &result, &timed_out))
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
	PGresult   *result = NULL;
	TimestampTz endtime;
	bool		timed_out;

	/*
	 * If it takes too long to execute a cleanup query, assume the connection
	 * is dead.  It's fairly likely that this is why we aborted in the first
	 * place (e.g. statement timeout, user cancel), so the timeout shouldn't
	 * be too long.
	 */
	endtime = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), 30000);

	/*
	 * Submit a query.  Since we don't use non-blocking mode, this also can
	 * block.  But its risk is relatively small, so we ignore that for now.
	 */
	if (!PQsendQuery(conn, query))
	{
		pgfdw_report_error(WARNING, NULL, conn, false, query);
		return false;
	}

	/* Get the result of the query. */
	if (pgfdw_get_cleanup_result(conn, endtime, &result, &timed_out))
	{
		if (timed_out)
			ereport(WARNING,
					(errmsg("could not get query result due to timeout"),
					 query ? errcontext("remote SQL command: %s", query) : 0));
		else
			pgfdw_report_error(WARNING, NULL, conn, false, query);

		return false;
	}

	/* Issue a warning if not successful. */
	if (PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		pgfdw_report_error(WARNING, result, conn, true, query);
		return ignore_errors;
	}
	PQclear(result);

	return true;
}

/*
 * Get, during abort cleanup, the result of a query that is in progress.  This
 * might be a query that is being interrupted by transaction abort, or it might
 * be a query that was initiated as part of transaction abort to get the remote
 * side back to the appropriate state.
 *
 * endtime is the time at which we should give up and assume the remote
 * side is dead.  Returns true if the timeout expired or connection trouble
 * occurred, false otherwise.  Sets *result except in case of a timeout.
 * Sets timed_out to true only when the timeout expired.
 */
static bool
pgfdw_get_cleanup_result(PGconn *conn, TimestampTz endtime, PGresult **result,
						 bool *timed_out)
{
	volatile bool failed = false;
	PGresult   *volatile last_res = NULL;

	*timed_out = false;

	/* In what follows, do not leak any PGresults on an error. */
	PG_TRY();
	{
		for (;;)
		{
			PGresult   *res;

			while (PQisBusy(conn))
			{
				int			wc;
				TimestampTz now = GetCurrentTimestamp();
				long		cur_timeout;

				/* If timeout has expired, give up, else get sleep time. */
				cur_timeout = TimestampDifferenceMilliseconds(now, endtime);
				if (cur_timeout <= 0)
				{
					*timed_out = true;
					failed = true;
					goto exit;
				}

				/* Sleep until there's something to do */
				wc = WaitLatchOrSocket(MyLatch,
									   WL_LATCH_SET | WL_SOCKET_READABLE |
									   WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
									   PQsocket(conn),
									   cur_timeout, PG_WAIT_EXTENSION);
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
				break;			/* query is complete */

			PQclear(last_res);
			last_res = res;
		}
exit:	;
	}
	PG_CATCH();
	{
		PQclear(last_res);
		PG_RE_THROW();
	}
	PG_END_TRY();

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

	if (toplevel)
		snprintf(sql, sizeof(sql), "ABORT TRANSACTION");
	else
		snprintf(sql, sizeof(sql),
				 "ROLLBACK TO SAVEPOINT s%d; RELEASE SAVEPOINT s%d",
				 entry->xact_depth, entry->xact_depth);
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
 * List active foreign server connections.
 *
 * This function takes no input parameter and returns setof record made of
 * following values:
 * - server_name - server name of active connection. In case the foreign server
 *   is dropped but still the connection is active, then the server name will
 *   be NULL in output.
 * - valid - true/false representing whether the connection is valid or not.
 * 	 Note that the connections can get invalidated in pgfdw_inval_callback.
 *
 * No records are returned when there are no cached connections at all.
 */
Datum
postgres_fdw_get_connections(PG_FUNCTION_ARGS)
{
#define POSTGRES_FDW_GET_CONNECTIONS_COLS	2
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	HASH_SEQ_STATUS scan;
	ConnCacheEntry *entry;

	SetSingleFuncCall(fcinfo, 0);

	/* If cache doesn't exist, we return no records */
	if (!ConnectionHash)
		PG_RETURN_VOID();

	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		ForeignServer *server;
		Datum		values[POSTGRES_FDW_GET_CONNECTIONS_COLS];
		bool		nulls[POSTGRES_FDW_GET_CONNECTIONS_COLS];

		/* We only look for open remote connections */
		if (!entry->conn)
			continue;

		server = GetForeignServerExtended(entry->serverid, FSV_MISSING_OK);

		MemSet(values, 0, sizeof(values));
		MemSet(nulls, 0, sizeof(nulls));

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
			nulls[0] = true;
		}
		else
			values[0] = CStringGetTextDatum(server->servername);

		values[1] = BoolGetDatum(!entry->invalidated);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

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
