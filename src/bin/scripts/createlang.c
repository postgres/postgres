/*-------------------------------------------------------------------------
 *
 * createlang
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/scripts/createlang.c
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
		{"no-password", no_argument, NULL, 'w'},
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
	enum trivalue prompt_password = TRI_DEFAULT;
	bool		echo = false;
	char	   *langname = NULL;

	char	   *p;

	PQExpBufferData sql;

	PGconn	   *conn;
	PGresult   *result;

	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pgscripts"));

	handle_help_version_opts(argc, argv, "createlang", help);

	while ((c = getopt_long(argc, argv, "lh:p:U:wWd:e", long_options, &optindex)) != -1)
	{
		switch (c)
		{
			case 'l':
				listlangs = true;
				break;
			case 'h':
				host = pg_strdup(optarg);
				break;
			case 'p':
				port = pg_strdup(optarg);
				break;
			case 'U':
				username = pg_strdup(optarg);
				break;
			case 'w':
				prompt_password = TRI_NO;
				break;
			case 'W':
				prompt_password = TRI_YES;
				break;
			case 'd':
				dbname = pg_strdup(optarg);
				break;
			case 'e':
				echo = true;
				break;
			default:
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
				exit(1);
		}
	}

	/*
	 * We set dbname from positional arguments if it is not already set by
	 * option arguments -d. If not doing listlangs, positional dbname must
	 * follow positional langname.
	 */

	if (argc - optind > 0)
	{
		if (listlangs)
		{
			if (dbname == NULL)
				dbname = argv[optind++];
		}
		else
		{
			langname = argv[optind++];
			if (argc - optind > 0 && dbname == NULL)
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
		static const bool translate_columns[] = {false, true};

		conn = connectDatabase(dbname, host, port, username, prompt_password,
							   progname, false);

		printfPQExpBuffer(&sql, "SELECT lanname as \"%s\", "
				"(CASE WHEN lanpltrusted THEN '%s' ELSE '%s' END) as \"%s\" "
						  "FROM pg_catalog.pg_language WHERE lanispl;",
						  gettext_noop("Name"),
						  gettext_noop("yes"), gettext_noop("no"),
						  gettext_noop("Trusted?"));
		result = executeQuery(conn, sql.data, progname, echo);

		memset(&popt, 0, sizeof(popt));
		popt.topt.format = PRINT_ALIGNED;
		popt.topt.border = 1;
		popt.topt.start_table = true;
		popt.topt.stop_table = true;
		popt.topt.encoding = PQclientEncoding(conn);
		popt.title = _("Procedural Languages");
		popt.translate_header = true;
		popt.translate_columns = translate_columns;
		printQuery(result, &popt, stdout, NULL);

		PQfinish(conn);
		exit(0);
	}

	if (langname == NULL)
	{
		fprintf(stderr, _("%s: missing required argument language name\n"), progname);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
		exit(1);
	}

	/* lower case language name */
	for (p = langname; *p; p++)
		if (*p >= 'A' && *p <= 'Z')
			*p += ('a' - 'A');

	conn = connectDatabase(dbname, host, port, username, prompt_password,
						   progname, false);

	/*
	 * Make sure the language isn't already installed
	 */
	printfPQExpBuffer(&sql,
			  "SELECT oid FROM pg_catalog.pg_language WHERE lanname = '%s';",
					  langname);
	result = executeQuery(conn, sql.data, progname, echo);
	if (PQntuples(result) > 0)
	{
		PQfinish(conn);
		fprintf(stderr,
		  _("%s: language \"%s\" is already installed in database \"%s\"\n"),
				progname, langname, dbname);
		/* separate exit status for "already installed" */
		exit(2);
	}
	PQclear(result);

	/*
	 * In 9.1 and up, assume that languages should be installed using CREATE
	 * EXTENSION.  However, it's possible this tool could be used against an
	 * older server, and it's easy enough to continue supporting the old way.
	 */
	if (PQserverVersion(conn) >= 90100)
		printfPQExpBuffer(&sql, "CREATE EXTENSION \"%s\";\n", langname);
	else
		printfPQExpBuffer(&sql, "CREATE LANGUAGE \"%s\";\n", langname);

	if (echo)
		printf("%s", sql.data);
	result = PQexec(conn, sql.data);
	if (PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, _("%s: language installation failed: %s"),
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
	printf(_("%s installs a procedural language into a PostgreSQL database.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... LANGNAME [DBNAME]\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -d, --dbname=DBNAME       database to install language in\n"));
	printf(_("  -e, --echo                show the commands being sent to the server\n"));
	printf(_("  -l, --list                show a list of currently installed languages\n"));
	printf(_("  -V, --version             output version information, then exit\n"));
	printf(_("  -?, --help                show this help, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME       database server host or socket directory\n"));
	printf(_("  -p, --port=PORT           database server port\n"));
	printf(_("  -U, --username=USERNAME   user name to connect as\n"));
	printf(_("  -w, --no-password         never prompt for password\n"));
	printf(_("  -W, --password            force password prompt\n"));
	printf(_("\nReport bugs to <pgsql-bugs@postgresql.org>.\n"));
}
