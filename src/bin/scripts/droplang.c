/*-------------------------------------------------------------------------
 *
 * droplang
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/bin/scripts/droplang.c,v 1.21.2.1 2007/08/10 00:39:44 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"
#include "common.h"
#include "print.h"

#define atooid(x)  ((Oid) strtoul((x), NULL, 10))


static void help(const char *progname);


int
main(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"list", no_argument, NULL, 'l'},
		{"host", required_argument, NULL, 'h'},
		{"port", required_argument, NULL, 'p'},
		{"username", required_argument, NULL, 'U'},
		{"password", no_argument, NULL, 'W'},
		{"dbname", required_argument, NULL, 'd'},
		{"echo", no_argument, NULL, 'e'},
		{NULL, 0, NULL, 0}
	};

	const char *progname;
	int			optindex;
	int			c;

	bool		listlangs = false;
	const char *dbname = NULL;
	char	   *host = NULL;
	char	   *port = NULL;
	char	   *username = NULL;
	bool		password = false;
	bool		echo = false;
	char	   *langname = NULL;

	char	   *p;
	Oid			lanplcallfoid;
	Oid			lanvalidator;
	char	   *handler;
	char	   *validator;
	char	   *handler_ns;
	char	   *validator_ns;
	bool		keephandler;
	bool		keepvalidator;

	PQExpBufferData sql;

	PGconn	   *conn;
	PGresult   *result;

	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], "pgscripts");

	handle_help_version_opts(argc, argv, "droplang", help);

	while ((c = getopt_long(argc, argv, "lh:p:U:Wd:e", long_options, &optindex)) != -1)
	{
		switch (c)
		{
			case 'l':
				listlangs = true;
				break;
			case 'h':
				host = optarg;
				break;
			case 'p':
				port = optarg;
				break;
			case 'U':
				username = optarg;
				break;
			case 'W':
				password = true;
				break;
			case 'd':
				dbname = optarg;
				break;
			case 'e':
				echo = true;
				break;
			default:
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
				exit(1);
		}
	}

	if (argc - optind > 0)
	{
		if (listlangs)
			dbname = argv[optind++];
		else
		{
			langname = argv[optind++];
			if (argc - optind > 0)
				dbname = argv[optind++];
		}
	}

	if (argc - optind > 0)
	{
		fprintf(stderr, _("%s: too many command-line arguments (first is \"%s\")\n"),
				progname, argv[optind]);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
		exit(1);
	}

	if (dbname == NULL)
	{
		if (getenv("PGDATABASE"))
			dbname = getenv("PGDATABASE");
		else if (getenv("PGUSER"))
			dbname = getenv("PGUSER");
		else
			dbname = get_user_name(progname);
	}

	initPQExpBuffer(&sql);

	/*
	 * List option
	 */
	if (listlangs)
	{
		printQueryOpt popt;

		conn = connectDatabase(dbname, host, port, username, password,
							   progname);

		printfPQExpBuffer(&sql, "SELECT lanname as \"%s\", "
				"(CASE WHEN lanpltrusted THEN '%s' ELSE '%s' END) as \"%s\" "
						  "FROM pg_catalog.pg_language WHERE lanispl;",
						  _("Name"), _("yes"), _("no"), _("Trusted?"));
		result = executeQuery(conn, sql.data, progname, echo);

		memset(&popt, 0, sizeof(popt));
		popt.topt.format = PRINT_ALIGNED;
		popt.topt.border = 1;
		popt.topt.start_table = true;
		popt.topt.stop_table = true;
		popt.topt.encoding = PQclientEncoding(conn);
		popt.title = _("Procedural Languages");
		printQuery(result, &popt, stdout, NULL);

		PQfinish(conn);
		exit(0);
	}

	if (langname == NULL)
	{
		fprintf(stderr, _("%s: missing required argument language name\n"),
				progname);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	for (p = langname; *p; p++)
		if (*p >= 'A' && *p <= 'Z')
			*p += ('a' - 'A');

	conn = connectDatabase(dbname, host, port, username, password, progname);

	/*
	 * Force schema search path to be just pg_catalog, so that we don't have
	 * to be paranoid about search paths below.
	 */
	executeCommand(conn, "SET search_path = pg_catalog;", progname, echo);

	/*
	 * Make sure the language is installed and find the OIDs of the handler
	 * and validator functions
	 */
	printfPQExpBuffer(&sql, "SELECT lanplcallfoid, lanvalidator "
					  "FROM pg_language WHERE lanname = '%s' AND lanispl;",
					  langname);
	result = executeQuery(conn, sql.data, progname, echo);
	if (PQntuples(result) == 0)
	{
		PQfinish(conn);
		fprintf(stderr, _("%s: language \"%s\" is not installed in "
						  "database \"%s\"\n"),
				progname, langname, dbname);
		exit(1);
	}
	lanplcallfoid = atooid(PQgetvalue(result, 0, 0));
	lanvalidator = atooid(PQgetvalue(result, 0, 1));
	PQclear(result);

	/*
	 * Check that there are no functions left defined in that language
	 */
	printfPQExpBuffer(&sql, "SELECT count(proname) FROM pg_proc P, "
					  "pg_language L WHERE P.prolang = L.oid "
					  "AND L.lanname = '%s';", langname);
	result = executeQuery(conn, sql.data, progname, echo);
	if (strcmp(PQgetvalue(result, 0, 0), "0") != 0)
	{
		PQfinish(conn);
		fprintf(stderr,
				_("%s: still %s functions declared in language \"%s\"; "
				  "language not removed\n"),
				progname, PQgetvalue(result, 0, 0), langname);
		exit(1);
	}
	PQclear(result);

	/*
	 * Check that the handler function isn't used by some other language
	 */
	printfPQExpBuffer(&sql, "SELECT count(*) FROM pg_language "
					  "WHERE lanplcallfoid = %u AND lanname <> '%s';",
					  lanplcallfoid, langname);
	result = executeQuery(conn, sql.data, progname, echo);
	if (strcmp(PQgetvalue(result, 0, 0), "0") == 0)
		keephandler = false;
	else
		keephandler = true;
	PQclear(result);

	/*
	 * Find the handler name
	 */
	if (!keephandler)
	{
		printfPQExpBuffer(&sql, "SELECT proname, (SELECT nspname "
						  "FROM pg_namespace ns WHERE ns.oid = pronamespace) "
						  "AS prons FROM pg_proc WHERE oid = %u;",
						  lanplcallfoid);
		result = executeQuery(conn, sql.data, progname, echo);
		handler = strdup(PQgetvalue(result, 0, 0));
		handler_ns = strdup(PQgetvalue(result, 0, 1));
		PQclear(result);
	}
	else
	{
		handler = NULL;
		handler_ns = NULL;
	}

	/*
	 * Check that the validator function isn't used by some other language
	 */
	if (OidIsValid(lanvalidator))
	{
		printfPQExpBuffer(&sql, "SELECT count(*) FROM pg_language "
						  "WHERE lanvalidator = %u AND lanname <> '%s';",
						  lanvalidator, langname);
		result = executeQuery(conn, sql.data, progname, echo);
		if (strcmp(PQgetvalue(result, 0, 0), "0") == 0)
			keepvalidator = false;
		else
			keepvalidator = true;
		PQclear(result);
	}
	else
		keepvalidator = true;	/* don't try to delete it */

	/*
	 * Find the validator name
	 */
	if (!keepvalidator)
	{
		printfPQExpBuffer(&sql, "SELECT proname, (SELECT nspname "
						  "FROM pg_namespace ns WHERE ns.oid = pronamespace) "
						  "AS prons FROM pg_proc WHERE oid = %u;",
						  lanvalidator);
		result = executeQuery(conn, sql.data, progname, echo);
		validator = strdup(PQgetvalue(result, 0, 0));
		validator_ns = strdup(PQgetvalue(result, 0, 1));
		PQclear(result);
	}
	else
	{
		validator = NULL;
		validator_ns = NULL;
	}

	/*
	 * Drop the language and the functions
	 */
	printfPQExpBuffer(&sql, "DROP LANGUAGE \"%s\";\n", langname);
	if (!keephandler)
		appendPQExpBuffer(&sql, "DROP FUNCTION \"%s\".\"%s\" ();\n",
						  handler_ns, handler);
	if (!keepvalidator)
		appendPQExpBuffer(&sql, "DROP FUNCTION \"%s\".\"%s\" (oid);\n",
						  validator_ns, validator);
	if (echo)
		printf("%s", sql.data);
	result = PQexec(conn, sql.data);
	if (PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, _("%s: language removal failed: %s"),
				progname, PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}

	PQclear(result);
	PQfinish(conn);
	exit(0);
}


static void
help(const char *progname)
{
	printf(_("%s removes a procedural language from a database.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... LANGNAME [DBNAME]\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -d, --dbname=DBNAME       database from which to remove the language\n"));
	printf(_("  -e, --echo                show the commands being sent to the server\n"));
	printf(_("  -l, --list                show a list of currently installed languages\n"));
	printf(_("  -h, --host=HOSTNAME       database server host or socket directory\n"));
	printf(_("  -p, --port=PORT           database server port\n"));
	printf(_("  -U, --username=USERNAME   user name to connect as\n"));
	printf(_("  -W, --password            prompt for password\n"));
	printf(_("  --help                    show this help, then exit\n"));
	printf(_("  --version                 output version information, then exit\n"));
	printf(_("\nReport bugs to <pgsql-bugs@postgresql.org>.\n"));
}
