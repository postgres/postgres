/*-------------------------------------------------------------------------
 *
 * createlang
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Header: /cvsroot/pgsql/src/bin/scripts/createlang.c,v 1.6 2003/08/04 00:43:29 momjian Exp $
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
		{"pglib", required_argument, NULL, 'L'},
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
	char	   *pglib = NULL;
	char	   *langname = NULL;

	char	   *p;
	bool		handlerexists;
	bool		trusted;
	char	   *handler;
	char	   *object;

	PQExpBufferData sql;

	PGconn	   *conn;
	PGresult   *result;

	progname = get_progname(argv[0]);
	init_nls();
	handle_help_version_opts(argc, argv, "createlang", help);

	while ((c = getopt_long(argc, argv, "lh:p:U:Wd:L:e", long_options, &optindex)) != -1)
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
			case 'L':
				pglib = optarg;
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

	if (!pglib)
		pglib = "$libdir";

	for (p = langname; *p; p++)
		if (*p >= 'A' && *p <= 'Z')
			*p += ('a' - 'A');

	if (strcmp(langname, "plpgsql") == 0)
	{
		trusted = true;
		handler = "plpgsql_call_handler";
		object = "plpgsql";
	}
	else if (strcmp(langname, "pltcl") == 0)
	{
		trusted = true;
		handler = "pltcl_call_handler";
		object = "pltcl";
	}
	else if (strcmp(langname, "pltclu") == 0)
	{
		trusted = false;
		handler = "pltclu_call_handler";
		object = "pltcl";
	}
	else if (strcmp(langname, "plperl") == 0)
	{
		trusted = true;
		handler = "plperl_call_handler";
		object = "plperl";
	}
	else if (strcmp(langname, "plperlu") == 0)
	{
		trusted = false;
		handler = "plperl_call_handler";
		object = "plperl";
	}
	else if (strcmp(langname, "plpythonu") == 0)
	{
		trusted = false;
		handler = "plpython_call_handler";
		object = "plpython";
	}
	else
	{
		fprintf(stderr, _("%s: unsupported language \"%s\"\n"), progname, langname);
		fprintf(stderr, _("Supported languages are plpgsql, pltcl, pltclu, plperl, plperlu, and plpythonu.\n"));
		exit(1);
	}

	conn = connectDatabase(dbname, host, port, username, password, progname);

	/*
	 * Make sure the language isn't already installed
	 */
	printfPQExpBuffer(&sql, "SELECT oid FROM pg_language WHERE lanname = '%s';", langname);
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
	 * Check whether the call handler exists
	 */
	printfPQExpBuffer(&sql, "SELECT oid FROM pg_proc WHERE proname = '%s' AND prorettype = (SELECT oid FROM pg_type WHERE typname = 'language_handler') AND pronargs = 0;", handler);
	result = executeQuery(conn, sql.data, progname, echo);
	handlerexists = (PQntuples(result) > 0);
	PQclear(result);

	/*
	 * Create the call handler and the language
	 */
	resetPQExpBuffer(&sql);

	if (!handlerexists)
		appendPQExpBuffer(&sql,
						  "CREATE FUNCTION \"%s\" () RETURNS language_handler AS '%s/%s' LANGUAGE C;\n",
						  handler, pglib, object);

	appendPQExpBuffer(&sql,
					  "CREATE %sLANGUAGE \"%s\" HANDLER \"%s\";\n",
					  (trusted ? "TRUSTED " : ""), langname, handler);

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
	printf(_("  -L, --pglib=DIRECTORY     find language interpreter file in DIRECTORY\n"));
	printf(_("  -h, --host=HOSTNAME       database server host or socket directory\n"));
	printf(_("  -p, --port=PORT           database server port\n"));
	printf(_("  -U, --username=USERNAME   user name to connect as\n"));
	printf(_("  -W, --password            prompt for password\n"));
	printf(_("  --help                    show this help, then exit\n"));
	printf(_("  --version                 output version information, then exit\n"));
	printf(_("\nReport bugs to <pgsql-bugs@postgresql.org>.\n"));
}
