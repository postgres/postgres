/*-------------------------------------------------------------------------
 *
 * droplang
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Header: /cvsroot/pgsql/src/bin/scripts/droplang.c,v 1.5 2003/08/04 00:43:29 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"
#include "common.h"
#include "print.h"


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

	char	   *progname;
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
	char	   *lanplcallfoid;
	char	   *handler;
	bool		keephandler;

	PQExpBufferData sql;

	PGconn	   *conn;
	PGresult   *result;

	progname = get_progname(argv[0]);
	init_nls();
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

		conn = connectDatabase(dbname, host, port, username, password, progname);

		printfPQExpBuffer(&sql, "SELECT lanname as \"%s\", (CASE WHEN lanpltrusted THEN '%s' ELSE '%s' END) as \"%s\" FROM pg_language WHERE lanispl IS TRUE;", _("Name"), _("yes"), _("no"), _("Trusted?"));
		result = executeQuery(conn, sql.data, progname, echo);

		memset(&popt, 0, sizeof(popt));
		popt.topt.format = PRINT_ALIGNED;
		popt.topt.border = 1;
		popt.topt.encoding = PQclientEncoding(conn);
		popt.title = _("Procedural Languages");
		printQuery(result, &popt, stdout);

		PQfinish(conn);
		exit(0);
	}

	if (langname == NULL)
	{
		fprintf(stderr, _("%s: missing required argument language name\n"), progname);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
		exit(1);
	}

	for (p = langname; *p; p++)
		if (*p >= 'A' && *p <= 'Z')
			*p += ('a' - 'A');

	conn = connectDatabase(dbname, host, port, username, password, progname);

	/*
	 * Make sure the language is installed and find the OID of the handler
	 * function
	 */
	printfPQExpBuffer(&sql, "SELECT lanplcallfoid FROM pg_language WHERE lanname = '%s' AND lanispl;", langname);
	result = executeQuery(conn, sql.data, progname, echo);
	if (PQntuples(result) == 0)
	{
		PQfinish(conn);
		fprintf(stderr, _("%s: language \"%s\" is not installed in database \"%s\"\n"),
				progname, langname, dbname);
		exit(1);
	}
	lanplcallfoid = PQgetvalue(result, 0, 0);
	/* result not cleared! */

	/*
	 * Check that there are no functions left defined in that language
	 */
	printfPQExpBuffer(&sql, "SELECT count(proname) FROM pg_proc P, pg_language L WHERE P.prolang = L.oid AND L.lanname = '%s';", langname);
	result = executeQuery(conn, sql.data, progname, echo);
	if (strcmp(PQgetvalue(result, 0, 0), "0") != 0)
	{
		PQfinish(conn);
		fprintf(stderr,
				_("%s: still %s functions declared in language \"%s\"; language not removed\n"),
				progname, PQgetvalue(result, 0, 0), langname);
		exit(1);
	}
	PQclear(result);

	/*
	 * Check that the handler function isn't used by some other language
	 */
	printfPQExpBuffer(&sql, "SELECT count(*) FROM pg_language WHERE lanplcallfoid = %s AND lanname <> '%s';", lanplcallfoid, langname);
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
		printfPQExpBuffer(&sql, "SELECT proname FROM pg_proc WHERE oid = %s;", lanplcallfoid);
		result = executeQuery(conn, sql.data, progname, echo);
		handler = PQgetvalue(result, 0, 0);
		/* result not cleared! */
	}
	else
		handler = NULL;

	/*
	 * Drop the language
	 */
	printfPQExpBuffer(&sql, "DROP LANGUAGE \"%s\";\n", langname);
	if (!keephandler)
		appendPQExpBuffer(&sql, "DROP FUNCTION \"%s\" ();\n", handler);
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
