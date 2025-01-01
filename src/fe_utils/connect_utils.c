/*-------------------------------------------------------------------------
 *
 * Facilities for frontend code to connect to and disconnect from databases.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/fe_utils/connect_utils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "common/connect.h"
#include "common/logging.h"
#include "common/string.h"
#include "fe_utils/connect_utils.h"
#include "fe_utils/query_utils.h"

/*
 * Make a database connection with the given parameters.
 *
 * An interactive password prompt is automatically issued if needed and
 * allowed by cparams->prompt_password.
 *
 * If allow_password_reuse is true, we will try to re-use any password
 * given during previous calls to this routine.  (Callers should not pass
 * allow_password_reuse=true unless reconnecting to the same host+port+user
 * as before, else we might create password exposure hazards.)
 */
PGconn *
connectDatabase(const ConnParams *cparams, const char *progname,
				bool echo, bool fail_ok, bool allow_password_reuse)
{
	PGconn	   *conn;
	bool		new_pass;
	static char *password = NULL;

	/* Callers must supply at least dbname; other params can be NULL */
	Assert(cparams->dbname);

	if (!allow_password_reuse && password)
	{
		free(password);
		password = NULL;
	}

	if (cparams->prompt_password == TRI_YES && password == NULL)
		password = simple_prompt("Password: ", false);

	/*
	 * Start the connection.  Loop until we have a password if requested by
	 * backend.
	 */
	do
	{
		const char *keywords[8];
		const char *values[8];
		int			i = 0;

		/*
		 * If dbname is a connstring, its entries can override the other
		 * values obtained from cparams; but in turn, override_dbname can
		 * override the dbname component of it.
		 */
		keywords[i] = "host";
		values[i++] = cparams->pghost;
		keywords[i] = "port";
		values[i++] = cparams->pgport;
		keywords[i] = "user";
		values[i++] = cparams->pguser;
		keywords[i] = "password";
		values[i++] = password;
		keywords[i] = "dbname";
		values[i++] = cparams->dbname;
		if (cparams->override_dbname)
		{
			keywords[i] = "dbname";
			values[i++] = cparams->override_dbname;
		}
		keywords[i] = "fallback_application_name";
		values[i++] = progname;
		keywords[i] = NULL;
		values[i++] = NULL;
		Assert(i <= lengthof(keywords));

		new_pass = false;
		conn = PQconnectdbParams(keywords, values, true);

		if (!conn)
			pg_fatal("could not connect to database %s: out of memory",
					 cparams->dbname);

		/*
		 * No luck?  Trying asking (again) for a password.
		 */
		if (PQstatus(conn) == CONNECTION_BAD &&
			PQconnectionNeedsPassword(conn) &&
			cparams->prompt_password != TRI_NO)
		{
			PQfinish(conn);
			free(password);
			password = simple_prompt("Password: ", false);
			new_pass = true;
		}
	} while (new_pass);

	/* check to see that the backend connection was successfully made */
	if (PQstatus(conn) == CONNECTION_BAD)
	{
		if (fail_ok)
		{
			PQfinish(conn);
			return NULL;
		}
		pg_fatal("%s", PQerrorMessage(conn));
	}

	/* Start strict; callers may override this. */
	PQclear(executeQuery(conn, ALWAYS_SECURE_SEARCH_PATH_SQL, echo));

	return conn;
}

/*
 * Try to connect to the appropriate maintenance database.
 *
 * This differs from connectDatabase only in that it has a rule for
 * inserting a default "dbname" if none was given (which is why cparams
 * is not const).  Note that cparams->dbname should typically come from
 * a --maintenance-db command line parameter.
 */
PGconn *
connectMaintenanceDatabase(ConnParams *cparams,
						   const char *progname, bool echo)
{
	PGconn	   *conn;

	/* If a maintenance database name was specified, just connect to it. */
	if (cparams->dbname)
		return connectDatabase(cparams, progname, echo, false, false);

	/* Otherwise, try postgres first and then template1. */
	cparams->dbname = "postgres";
	conn = connectDatabase(cparams, progname, echo, true, false);
	if (!conn)
	{
		cparams->dbname = "template1";
		conn = connectDatabase(cparams, progname, echo, false, false);
	}
	return conn;
}

/*
 * Disconnect the given connection, canceling any statement if one is active.
 */
void
disconnectDatabase(PGconn *conn)
{
	Assert(conn != NULL);

	if (PQtransactionStatus(conn) == PQTRANS_ACTIVE)
	{
		PGcancelConn *cancelConn = PQcancelCreate(conn);

		(void) PQcancelBlocking(cancelConn);
		PQcancelFinish(cancelConn);
	}

	PQfinish(conn);
}
