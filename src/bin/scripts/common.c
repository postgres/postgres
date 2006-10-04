/*-------------------------------------------------------------------------
 *
 *	common.c
 *		Common support routines for bin/scripts/
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/bin/scripts/common.c,v 1.24 2006/10/04 00:30:06 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <pwd.h>
#include <unistd.h>

#include "common.h"

#ifndef HAVE_INT_OPTRESET
int			optreset;
#endif


/*
 * Returns the current user name.
 */
const char *
get_user_name(const char *progname)
{
#ifndef WIN32
	struct passwd *pw;

	pw = getpwuid(geteuid());
	if (!pw)
	{
		fprintf(stderr, _("%s: could not obtain information about current user: %s\n"),
				progname, strerror(errno));
		exit(1);
	}
	return pw->pw_name;
#else
	static char username[128];	/* remains after function exit */
	DWORD		len = sizeof(username) - 1;

	if (!GetUserName(username, &len))
	{
		fprintf(stderr, _("%s: could not get current user name: %s\n"),
				progname, strerror(errno));
		exit(1);
	}
	return username;
#endif
}


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
 * Make a database connection with the given parameters.  An
 * interactive password prompt is automatically issued if required.
 */
PGconn *
connectDatabase(const char *dbname, const char *pghost, const char *pgport,
				const char *pguser, bool require_password,
				const char *progname)
{
	PGconn	   *conn;
	char	   *password = NULL;
	bool		need_pass = false;

	if (require_password)
		password = simple_prompt("Password: ", 100, false);

	/*
	 * Start the connection.  Loop until we have a password if requested by
	 * backend.
	 */
	do
	{
		need_pass = false;
		conn = PQsetdbLogin(pghost, pgport, NULL, NULL, dbname, pguser, password);

		if (!conn)
		{
			fprintf(stderr, _("%s: could not connect to database %s\n"),
					progname, dbname);
			exit(1);
		}

		if (PQstatus(conn) == CONNECTION_BAD &&
			strcmp(PQerrorMessage(conn), PQnoPasswordSupplied) == 0 &&
			!feof(stdin))
		{
			PQfinish(conn);
			need_pass = true;
			free(password);
			password = NULL;
			password = simple_prompt("Password: ", 100, false);
		}
	} while (need_pass);

	if (password)
		free(password);

	/* check to see that the backend connection was successfully made */
	if (PQstatus(conn) == CONNECTION_BAD)
	{
		fprintf(stderr, _("%s: could not connect to database %s: %s"),
				progname, dbname, PQerrorMessage(conn));
		exit(1);
	}

	return conn;
}


/*
 * Run a query, return the results, exit program on failure.
 */
PGresult *
executeQuery(PGconn *conn, const char *query, const char *progname, bool echo)
{
	PGresult   *res;

	if (echo)
		printf("%s\n", query);

	res = PQexec(conn, query);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, _("%s: query failed: %s"),
				progname, PQerrorMessage(conn));
		fprintf(stderr, _("%s: query was: %s\n"),
				progname, query);
		PQfinish(conn);
		exit(1);
	}

	return res;
}


/*
 * As above for a SQL command (which returns nothing).
 */
void
executeCommand(PGconn *conn, const char *query,
			   const char *progname, bool echo)
{
	PGresult   *res;

	if (echo)
		printf("%s\n", query);

	res = PQexec(conn, query);
	if (!res ||
		PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, _("%s: query failed: %s"),
				progname, PQerrorMessage(conn));
		fprintf(stderr, _("%s: query was: %s\n"),
				progname, query);
		PQfinish(conn);
		exit(1);
	}

	PQclear(res);
}


/*
 * Check yes/no answer in a localized way.	1=yes, 0=no, -1=neither.
 */

/* translator: abbreviation for "yes" */
#define PG_YESLETTER gettext_noop("y")
/* translator: abbreviation for "no" */
#define PG_NOLETTER gettext_noop("n")

bool
yesno_prompt(const char *question)
{
	char		prompt[256];

	/*
	 * translator: This is a question followed by the translated options for
	 * "yes" and "no".
	 */
	snprintf(prompt, sizeof(prompt), _("%s (%s/%s) "),
			 _(question), _(PG_YESLETTER), _(PG_NOLETTER));

	for (;;)
	{
		char	   *resp;

		resp = simple_prompt(prompt, 1, true);

		if (strcmp(resp, _(PG_YESLETTER)) == 0)
		{
			free(resp);
			return true;
		}
		else if (strcmp(resp, _(PG_NOLETTER)) == 0)
		{
			free(resp);
			return false;
		}

		free(resp);
		printf(_("Please answer \"%s\" or \"%s\".\n"),
			   _(PG_YESLETTER), _(PG_NOLETTER));
	}
}
