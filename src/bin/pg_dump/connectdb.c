/*-------------------------------------------------------------------------
 *
 * connectdb.c
 *    This is a common file connection to the database.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    src/bin/pg_dump/connectdb.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "common/connect.h"
#include "common/logging.h"
#include "common/string.h"
#include "connectdb.h"
#include "dumputils.h"
#include "fe_utils/string_utils.h"

static char *constructConnStr(const char **keywords, const char **values);

/*
 * ConnectDatabase
 *
 * Make a database connection with the given parameters.  An
 * interactive password prompt is automatically issued if required.
 *
 * If fail_on_error is false, we return NULL without printing any message
 * on failure, but preserve any prompted password for the next try.
 *
 * On success, the 'connstr' is set to a connection string containing
 * the options used and 'server_version' is set to version so that caller
 * can use them.
 */
PGconn *
ConnectDatabase(const char *dbname, const char *connection_string,
				const char *pghost, const char *pgport, const char *pguser,
				trivalue prompt_password, bool fail_on_error, const char *progname,
				const char **connstr, int *server_version, char *password,
				char *override_dbname)
{
	PGconn	   *conn;
	bool		new_pass;
	const char *remoteversion_str;
	int			my_version;
	const char **keywords = NULL;
	const char **values = NULL;
	PQconninfoOption *conn_opts = NULL;
	int			server_version_temp;

	if (prompt_password == TRI_YES && !password)
		password = simple_prompt("Password: ", false);

	/*
	 * Start the connection.  Loop until we have a password if requested by
	 * backend.
	 */
	do
	{
		int			argcount = 8;
		PQconninfoOption *conn_opt;
		char	   *err_msg = NULL;
		int			i = 0;

		free(keywords);
		free(values);
		PQconninfoFree(conn_opts);

		/*
		 * Merge the connection info inputs given in form of connection string
		 * and other options.  Explicitly discard any dbname value in the
		 * connection string; otherwise, PQconnectdbParams() would interpret
		 * that value as being itself a connection string.
		 */
		if (connection_string)
		{
			conn_opts = PQconninfoParse(connection_string, &err_msg);
			if (conn_opts == NULL)
				pg_fatal("%s", err_msg);

			for (conn_opt = conn_opts; conn_opt->keyword != NULL; conn_opt++)
			{
				if (conn_opt->val != NULL && conn_opt->val[0] != '\0' &&
					strcmp(conn_opt->keyword, "dbname") != 0)
					argcount++;
			}

			keywords = pg_malloc0((argcount + 1) * sizeof(*keywords));
			values = pg_malloc0((argcount + 1) * sizeof(*values));

			for (conn_opt = conn_opts; conn_opt->keyword != NULL; conn_opt++)
			{
				if (conn_opt->val != NULL && conn_opt->val[0] != '\0' &&
					strcmp(conn_opt->keyword, "dbname") != 0)
				{
					keywords[i] = conn_opt->keyword;
					values[i] = conn_opt->val;
					i++;
				}
			}
		}
		else
		{
			keywords = pg_malloc0((argcount + 1) * sizeof(*keywords));
			values = pg_malloc0((argcount + 1) * sizeof(*values));
		}

		if (pghost)
		{
			keywords[i] = "host";
			values[i] = pghost;
			i++;
		}
		if (pgport)
		{
			keywords[i] = "port";
			values[i] = pgport;
			i++;
		}
		if (pguser)
		{
			keywords[i] = "user";
			values[i] = pguser;
			i++;
		}
		if (password)
		{
			keywords[i] = "password";
			values[i] = password;
			i++;
		}
		if (dbname)
		{
			keywords[i] = "dbname";
			values[i] = dbname;
			i++;
		}
		if (override_dbname)
		{
			keywords[i] = "dbname";
			values[i] = override_dbname;
			i++;
		}

		keywords[i] = "fallback_application_name";
		values[i] = progname;
		i++;

		new_pass = false;
		conn = PQconnectdbParams(keywords, values, true);

		if (!conn)
			pg_fatal("could not connect to database \"%s\"", dbname);

		if (PQstatus(conn) == CONNECTION_BAD &&
			PQconnectionNeedsPassword(conn) &&
			!password &&
			prompt_password != TRI_NO)
		{
			PQfinish(conn);
			password = simple_prompt("Password: ", false);
			new_pass = true;
		}
	} while (new_pass);

	/* check to see that the backend connection was successfully made */
	if (PQstatus(conn) == CONNECTION_BAD)
	{
		if (fail_on_error)
			pg_fatal("%s", PQerrorMessage(conn));
		else
		{
			PQfinish(conn);

			free(keywords);
			free(values);
			PQconninfoFree(conn_opts);

			return NULL;
		}
	}

	/*
	 * Ok, connected successfully. If requested, remember the options used, in
	 * the form of a connection string.
	 */
	if (connstr)
		*connstr = constructConnStr(keywords, values);

	free(keywords);
	free(values);
	PQconninfoFree(conn_opts);

	/* Check version */
	remoteversion_str = PQparameterStatus(conn, "server_version");
	if (!remoteversion_str)
		pg_fatal("could not get server version");

	server_version_temp = PQserverVersion(conn);
	if (server_version_temp == 0)
		pg_fatal("could not parse server version \"%s\"",
				 remoteversion_str);

	/* If requested, then copy server version to out variable. */
	if (server_version)
		*server_version = server_version_temp;

	my_version = PG_VERSION_NUM;

	/*
	 * We allow the server to be back to 9.2, and up to any minor release of
	 * our own major version.  (See also version check in pg_dump.c.)
	 */
	if (my_version != server_version_temp
		&& (server_version_temp < 90200 ||
			(server_version_temp / 100) > (my_version / 100)))
	{
		pg_log_error("aborting because of server version mismatch");
		pg_log_error_detail("server version: %s; %s version: %s",
							remoteversion_str, progname, PG_VERSION);
		exit_nicely(1);
	}

	PQclear(executeQuery(conn, ALWAYS_SECURE_SEARCH_PATH_SQL));

	return conn;
}

/*
 * constructConnStr
 *
 * Construct a connection string from the given keyword/value pairs. It is
 * used to pass the connection options to the pg_dump subprocess.
 *
 * The following parameters are excluded:
 *	dbname		- varies in each pg_dump invocation
 *	password	- it's not secure to pass a password on the command line
 *	fallback_application_name - we'll let pg_dump set it
 */
static char *
constructConnStr(const char **keywords, const char **values)
{
	PQExpBuffer buf = createPQExpBuffer();
	char	   *connstr;
	int			i;
	bool		firstkeyword = true;

	/* Construct a new connection string in key='value' format. */
	for (i = 0; keywords[i] != NULL; i++)
	{
		if (strcmp(keywords[i], "dbname") == 0 ||
			strcmp(keywords[i], "password") == 0 ||
			strcmp(keywords[i], "fallback_application_name") == 0)
			continue;

		if (!firstkeyword)
			appendPQExpBufferChar(buf, ' ');
		firstkeyword = false;
		appendPQExpBuffer(buf, "%s=", keywords[i]);
		appendConnStrVal(buf, values[i]);
	}

	connstr = pg_strdup(buf->data);
	destroyPQExpBuffer(buf);
	return connstr;
}

/*
 * executeQuery
 *
 * Run a query, return the results, exit program on failure.
 */
PGresult *
executeQuery(PGconn *conn, const char *query)
{
	PGresult   *res;

	pg_log_info("executing %s", query);

	res = PQexec(conn, query);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("query failed: %s", PQerrorMessage(conn));
		pg_log_error_detail("Query was: %s", query);
		PQfinish(conn);
		exit_nicely(1);
	}

	return res;
}
