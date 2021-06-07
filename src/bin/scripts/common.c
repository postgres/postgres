/*-------------------------------------------------------------------------
 *
 *	common.c
 *		Common support routines for bin/scripts/
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/scripts/common.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <signal.h>
#include <unistd.h>

#include "common.h"
#include "common/connect.h"
#include "common/logging.h"
#include "fe_utils/cancel.h"
#include "fe_utils/string_utils.h"

#define ERRCODE_UNDEFINED_TABLE  "42P01"

#define PQmblenBounded(s, e)  strnlen(s, PQmblen(s, e))

/*
 * Provide strictly harmonized handling of --help and --version
 * options.
 */
void
handle_help_version_opts(int argc, char *argv[],
						 const char *fixed_progname, help_handler hlp)
{
	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			hlp(get_progname(argv[0]));
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			printf("%s (PostgreSQL) " PG_VERSION "\n", fixed_progname);
			exit(0);
		}
	}
}


/*
 * Make a database connection with the given parameters.
 *
 * An interactive password prompt is automatically issued if needed and
 * allowed by cparams->prompt_password.
 *
 * If allow_password_reuse is true, we will try to re-use any password
 * given during previous calls to this routine.  (Callers should not pass
 * allow_password_reuse=true unless reconnecting to the same database+user
 * as before, else we might create password exposure hazards.)
 */
PGconn *
connectDatabase(const ConnParams *cparams, const char *progname,
				bool echo, bool fail_ok, bool allow_password_reuse)
{
	PGconn	   *conn;
	bool		new_pass;
	static bool have_password = false;
	static char password[100];

	/* Callers must supply at least dbname; other params can be NULL */
	Assert(cparams->dbname);

	if (!allow_password_reuse)
		have_password = false;

	if (cparams->prompt_password == TRI_YES && !have_password)
	{
		simple_prompt("Password: ", password, sizeof(password), false);
		have_password = true;
	}

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
		values[i++] = have_password ? password : NULL;
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
		{
			pg_log_error("could not connect to database %s: out of memory",
						 cparams->dbname);
			exit(1);
		}

		/*
		 * No luck?  Trying asking (again) for a password.
		 */
		if (PQstatus(conn) == CONNECTION_BAD &&
			PQconnectionNeedsPassword(conn) &&
			cparams->prompt_password != TRI_NO)
		{
			PQfinish(conn);
			simple_prompt("Password: ", password, sizeof(password), false);
			have_password = true;
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
		pg_log_error("could not connect to database %s: %s",
					 cparams->dbname, PQerrorMessage(conn));
		exit(1);
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
	char		errbuf[256];

	Assert(conn != NULL);

	if (PQtransactionStatus(conn) == PQTRANS_ACTIVE)
	{
		PGcancel   *cancel;

		if ((cancel = PQgetCancel(conn)))
		{
			(void) PQcancel(cancel, errbuf, sizeof(errbuf));
			PQfreeCancel(cancel);
		}
	}

	PQfinish(conn);
}

/*
 * Run a query, return the results, exit program on failure.
 */
PGresult *
executeQuery(PGconn *conn, const char *query, bool echo)
{
	PGresult   *res;

	if (echo)
		printf("%s\n", query);

	res = PQexec(conn, query);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("query failed: %s", PQerrorMessage(conn));
		pg_log_info("query was: %s", query);
		PQfinish(conn);
		exit(1);
	}

	return res;
}


/*
 * As above for a SQL command (which returns nothing).
 */
void
executeCommand(PGconn *conn, const char *query, bool echo)
{
	PGresult   *res;

	if (echo)
		printf("%s\n", query);

	res = PQexec(conn, query);
	if (!res ||
		PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		pg_log_error("query failed: %s", PQerrorMessage(conn));
		pg_log_info("query was: %s", query);
		PQfinish(conn);
		exit(1);
	}

	PQclear(res);
}


/*
 * As above for a SQL maintenance command (returns command success).
 * Command is executed with a cancel handler set, so Ctrl-C can
 * interrupt it.
 */
bool
executeMaintenanceCommand(PGconn *conn, const char *query, bool echo)
{
	PGresult   *res;
	bool		r;

	if (echo)
		printf("%s\n", query);

	SetCancelConn(conn);
	res = PQexec(conn, query);
	ResetCancelConn();

	r = (res && PQresultStatus(res) == PGRES_COMMAND_OK);

	if (res)
		PQclear(res);

	return r;
}

/*
 * Consume all the results generated for the given connection until
 * nothing remains.  If at least one error is encountered, return false.
 * Note that this will block if the connection is busy.
 */
bool
consumeQueryResult(PGconn *conn)
{
	bool		ok = true;
	PGresult   *result;

	SetCancelConn(conn);
	while ((result = PQgetResult(conn)) != NULL)
	{
		if (!processQueryResult(conn, result))
			ok = false;
	}
	ResetCancelConn();
	return ok;
}

/*
 * Process (and delete) a query result.  Returns true if there's no error,
 * false otherwise -- but errors about trying to work on a missing relation
 * are reported and subsequently ignored.
 */
bool
processQueryResult(PGconn *conn, PGresult *result)
{
	/*
	 * If it's an error, report it.  Errors about a missing table are harmless
	 * so we continue processing; but die for other errors.
	 */
	if (PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		char	   *sqlState = PQresultErrorField(result, PG_DIAG_SQLSTATE);

		pg_log_error("processing of database \"%s\" failed: %s",
					 PQdb(conn), PQerrorMessage(conn));

		if (sqlState && strcmp(sqlState, ERRCODE_UNDEFINED_TABLE) != 0)
		{
			PQclear(result);
			return false;
		}
	}

	PQclear(result);
	return true;
}


/*
 * Split TABLE[(COLUMNS)] into TABLE and [(COLUMNS)] portions.  When you
 * finish using them, pg_free(*table).  *columns is a pointer into "spec",
 * possibly to its NUL terminator.
 */
void
splitTableColumnsSpec(const char *spec, int encoding,
					  char **table, const char **columns)
{
	bool		inquotes = false;
	const char *cp = spec;

	/*
	 * Find the first '(' not identifier-quoted.  Based on
	 * dequote_downcase_identifier().
	 */
	while (*cp && (*cp != '(' || inquotes))
	{
		if (*cp == '"')
		{
			if (inquotes && cp[1] == '"')
				cp++;			/* pair does not affect quoting */
			else
				inquotes = !inquotes;
			cp++;
		}
		else
			cp += PQmblenBounded(cp, encoding);
	}
	*table = pnstrdup(spec, cp - spec);
	*columns = cp;
}

/*
 * Break apart TABLE[(COLUMNS)] of "spec".  With the reset_val of search_path
 * in effect, have regclassin() interpret the TABLE portion.  Append to "buf"
 * the qualified name of TABLE, followed by any (COLUMNS).  Exit on failure.
 * We use this to interpret --table=foo under the search path psql would get,
 * in advance of "ANALYZE public.foo" under the always-secure search path.
 */
void
appendQualifiedRelation(PQExpBuffer buf, const char *spec,
						PGconn *conn, bool echo)
{
	char	   *table;
	const char *columns;
	PQExpBufferData sql;
	PGresult   *res;
	int			ntups;

	splitTableColumnsSpec(spec, PQclientEncoding(conn), &table, &columns);

	/*
	 * Query must remain ABSOLUTELY devoid of unqualified names.  This would
	 * be unnecessary given a regclassin() variant taking a search_path
	 * argument.
	 */
	initPQExpBuffer(&sql);
	appendPQExpBufferStr(&sql,
						 "SELECT c.relname, ns.nspname\n"
						 " FROM pg_catalog.pg_class c,"
						 " pg_catalog.pg_namespace ns\n"
						 " WHERE c.relnamespace OPERATOR(pg_catalog.=) ns.oid\n"
						 "  AND c.oid OPERATOR(pg_catalog.=) ");
	appendStringLiteralConn(&sql, table, conn);
	appendPQExpBufferStr(&sql, "::pg_catalog.regclass;");

	executeCommand(conn, "RESET search_path;", echo);

	/*
	 * One row is a typical result, as is a nonexistent relation ERROR.
	 * regclassin() unconditionally accepts all-digits input as an OID; if no
	 * relation has that OID; this query returns no rows.  Catalog corruption
	 * might elicit other row counts.
	 */
	res = executeQuery(conn, sql.data, echo);
	ntups = PQntuples(res);
	if (ntups != 1)
	{
		pg_log_error(ngettext("query returned %d row instead of one: %s",
							  "query returned %d rows instead of one: %s",
							  ntups),
					 ntups, sql.data);
		PQfinish(conn);
		exit(1);
	}
	appendPQExpBufferStr(buf,
						 fmtQualifiedId(PQgetvalue(res, 0, 1),
										PQgetvalue(res, 0, 0)));
	appendPQExpBufferStr(buf, columns);
	PQclear(res);
	termPQExpBuffer(&sql);
	pg_free(table);

	PQclear(executeQuery(conn, ALWAYS_SECURE_SEARCH_PATH_SQL, echo));
}


/*
 * Check yes/no answer in a localized way.  1=yes, 0=no, -1=neither.
 */

/* translator: abbreviation for "yes" */
#define PG_YESLETTER gettext_noop("y")
/* translator: abbreviation for "no" */
#define PG_NOLETTER gettext_noop("n")

bool
yesno_prompt(const char *question)
{
	char		prompt[256];

	/*------
	   translator: This is a question followed by the translated options for
	   "yes" and "no". */
	snprintf(prompt, sizeof(prompt), _("%s (%s/%s) "),
			 _(question), _(PG_YESLETTER), _(PG_NOLETTER));

	for (;;)
	{
		char		resp[10];

		simple_prompt(prompt, resp, sizeof(resp), true);

		if (strcmp(resp, _(PG_YESLETTER)) == 0)
			return true;
		if (strcmp(resp, _(PG_NOLETTER)) == 0)
			return false;

		printf(_("Please answer \"%s\" or \"%s\".\n"),
			   _(PG_YESLETTER), _(PG_NOLETTER));
	}
}
