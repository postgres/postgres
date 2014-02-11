/*-------------------------------------------------------------------------
 *
 * createdb
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/scripts/createdb.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "common.h"
#include "dumputils.h"


static void help(const char *progname);


int
main(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"host", required_argument, NULL, 'h'},
		{"port", required_argument, NULL, 'p'},
		{"username", required_argument, NULL, 'U'},
		{"no-password", no_argument, NULL, 'w'},
		{"password", no_argument, NULL, 'W'},
		{"echo", no_argument, NULL, 'e'},
		{"owner", required_argument, NULL, 'O'},
		{"tablespace", required_argument, NULL, 'D'},
		{"template", required_argument, NULL, 'T'},
		{"encoding", required_argument, NULL, 'E'},
		{"lc-collate", required_argument, NULL, 1},
		{"lc-ctype", required_argument, NULL, 2},
		{"locale", required_argument, NULL, 'l'},
		{"maintenance-db", required_argument, NULL, 3},
		{NULL, 0, NULL, 0}
	};

	const char *progname;
	int			optindex;
	int			c;

	const char *dbname = NULL;
	const char *maintenance_db = NULL;
	char	   *comment = NULL;
	char	   *host = NULL;
	char	   *port = NULL;
	char	   *username = NULL;
	enum trivalue prompt_password = TRI_DEFAULT;
	bool		echo = false;
	char	   *owner = NULL;
	char	   *tablespace = NULL;
	char	   *template = NULL;
	char	   *encoding = NULL;
	char	   *lc_collate = NULL;
	char	   *lc_ctype = NULL;
	char	   *locale = NULL;

	PQExpBufferData sql;

	PGconn	   *conn;
	PGresult   *result;

	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pgscripts"));

	handle_help_version_opts(argc, argv, "createdb", help);

	while ((c = getopt_long(argc, argv, "h:p:U:wWeO:D:T:E:l:", long_options, &optindex)) != -1)
	{
		switch (c)
		{
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
			case 'e':
				echo = true;
				break;
			case 'O':
				owner = pg_strdup(optarg);
				break;
			case 'D':
				tablespace = pg_strdup(optarg);
				break;
			case 'T':
				template = pg_strdup(optarg);
				break;
			case 'E':
				encoding = pg_strdup(optarg);
				break;
			case 1:
				lc_collate = pg_strdup(optarg);
				break;
			case 2:
				lc_ctype = pg_strdup(optarg);
				break;
			case 'l':
				locale = pg_strdup(optarg);
				break;
			case 3:
				maintenance_db = pg_strdup(optarg);
				break;
			default:
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
				exit(1);
		}
	}

	switch (argc - optind)
	{
		case 0:
			break;
		case 1:
			dbname = argv[optind];
			break;
		case 2:
			dbname = argv[optind];
			comment = argv[optind + 1];
			break;
		default:
			fprintf(stderr, _("%s: too many command-line arguments (first is \"%s\")\n"),
					progname, argv[optind + 2]);
			fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
			exit(1);
	}

	if (locale)
	{
		if (lc_ctype)
		{
			fprintf(stderr, _("%s: only one of --locale and --lc-ctype can be specified\n"),
					progname);
			exit(1);
		}
		if (lc_collate)
		{
			fprintf(stderr, _("%s: only one of --locale and --lc-collate can be specified\n"),
					progname);
			exit(1);
		}
		lc_ctype = locale;
		lc_collate = locale;
	}

	if (encoding)
	{
		if (pg_char_to_encoding(encoding) < 0)
		{
			fprintf(stderr, _("%s: \"%s\" is not a valid encoding name\n"),
					progname, encoding);
			exit(1);
		}
	}

	if (dbname == NULL)
	{
		if (getenv("PGDATABASE"))
			dbname = getenv("PGDATABASE");
		else if (getenv("PGUSER"))
			dbname = getenv("PGUSER");
		else
			dbname = get_user_name_or_exit(progname);
	}

	initPQExpBuffer(&sql);

	appendPQExpBuffer(&sql, "CREATE DATABASE %s",
					  fmtId(dbname));

	if (owner)
		appendPQExpBuffer(&sql, " OWNER %s", fmtId(owner));
	if (tablespace)
		appendPQExpBuffer(&sql, " TABLESPACE %s", fmtId(tablespace));
	if (encoding)
		appendPQExpBuffer(&sql, " ENCODING '%s'", encoding);
	if (template)
		appendPQExpBuffer(&sql, " TEMPLATE %s", fmtId(template));
	if (lc_collate)
		appendPQExpBuffer(&sql, " LC_COLLATE '%s'", lc_collate);
	if (lc_ctype)
		appendPQExpBuffer(&sql, " LC_CTYPE '%s'", lc_ctype);

	appendPQExpBufferStr(&sql, ";");

	/* No point in trying to use postgres db when creating postgres db. */
	if (maintenance_db == NULL && strcmp(dbname, "postgres") == 0)
		maintenance_db = "template1";

	conn = connectMaintenanceDatabase(maintenance_db, host, port, username,
									  prompt_password, progname);

	if (echo)
		printf("%s\n", sql.data);
	result = PQexec(conn, sql.data);

	if (PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, _("%s: database creation failed: %s"),
				progname, PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}

	PQclear(result);

	if (comment)
	{
		printfPQExpBuffer(&sql, "COMMENT ON DATABASE %s IS ", fmtId(dbname));
		appendStringLiteralConn(&sql, comment, conn);
		appendPQExpBufferStr(&sql, ";");

		if (echo)
			printf("%s\n", sql.data);
		result = PQexec(conn, sql.data);

		if (PQresultStatus(result) != PGRES_COMMAND_OK)
		{
			fprintf(stderr, _("%s: comment creation failed (database was created): %s"),
					progname, PQerrorMessage(conn));
			PQfinish(conn);
			exit(1);
		}

		PQclear(result);
	}

	PQfinish(conn);

	exit(0);
}


static void
help(const char *progname)
{
	printf(_("%s creates a PostgreSQL database.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [DBNAME] [DESCRIPTION]\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -D, --tablespace=TABLESPACE  default tablespace for the database\n"));
	printf(_("  -e, --echo                   show the commands being sent to the server\n"));
	printf(_("  -E, --encoding=ENCODING      encoding for the database\n"));
	printf(_("  -l, --locale=LOCALE          locale settings for the database\n"));
	printf(_("      --lc-collate=LOCALE      LC_COLLATE setting for the database\n"));
	printf(_("      --lc-ctype=LOCALE        LC_CTYPE setting for the database\n"));
	printf(_("  -O, --owner=OWNER            database user to own the new database\n"));
	printf(_("  -T, --template=TEMPLATE      template database to copy\n"));
	printf(_("  -V, --version                output version information, then exit\n"));
	printf(_("  -?, --help                   show this help, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME          database server host or socket directory\n"));
	printf(_("  -p, --port=PORT              database server port\n"));
	printf(_("  -U, --username=USERNAME      user name to connect as\n"));
	printf(_("  -w, --no-password            never prompt for password\n"));
	printf(_("  -W, --password               force password prompt\n"));
	printf(_("  --maintenance-db=DBNAME      alternate maintenance database\n"));
	printf(_("\nBy default, a database with the same name as the current user is created.\n"));
	printf(_("\nReport bugs to <pgsql-bugs@postgresql.org>.\n"));
}
