/*-------------------------------------------------------------------------
 *
 * createdb
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/scripts/createdb.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "common.h"
#include "common/logging.h"
#include "fe_utils/option_utils.h"
#include "fe_utils/string_utils.h"


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
		{"strategy", required_argument, NULL, 'S'},
		{"lc-collate", required_argument, NULL, 1},
		{"lc-ctype", required_argument, NULL, 2},
		{"locale", required_argument, NULL, 'l'},
		{"maintenance-db", required_argument, NULL, 3},
		{"locale-provider", required_argument, NULL, 4},
		{"icu-locale", required_argument, NULL, 5},
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
	ConnParams	cparams;
	bool		echo = false;
	char	   *owner = NULL;
	char	   *tablespace = NULL;
	char	   *template = NULL;
	char	   *encoding = NULL;
	char	   *strategy = NULL;
	char	   *lc_collate = NULL;
	char	   *lc_ctype = NULL;
	char	   *locale = NULL;
	char	   *locale_provider = NULL;
	char	   *icu_locale = NULL;

	PQExpBufferData sql;

	PGconn	   *conn;
	PGresult   *result;

	pg_logging_init(argv[0]);
	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pgscripts"));

	handle_help_version_opts(argc, argv, "createdb", help);

	while ((c = getopt_long(argc, argv, "h:p:U:wWeO:D:T:E:l:S:", long_options, &optindex)) != -1)
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
			case 'S':
				strategy = pg_strdup(optarg);
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
			case 4:
				locale_provider = pg_strdup(optarg);
				break;
			case 5:
				icu_locale = pg_strdup(optarg);
				break;
			default:
				/* getopt_long already emitted a complaint */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
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
			pg_log_error("too many command-line arguments (first is \"%s\")",
						 argv[optind + 2]);
			pg_log_error_hint("Try \"%s --help\" for more information.", progname);
			exit(1);
	}

	if (locale)
	{
		if (!lc_ctype)
			lc_ctype = locale;
		if (!lc_collate)
			lc_collate = locale;
	}

	if (encoding)
	{
		if (pg_char_to_encoding(encoding) < 0)
			pg_fatal("\"%s\" is not a valid encoding name", encoding);
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

	/* No point in trying to use postgres db when creating postgres db. */
	if (maintenance_db == NULL && strcmp(dbname, "postgres") == 0)
		maintenance_db = "template1";

	cparams.dbname = maintenance_db;
	cparams.pghost = host;
	cparams.pgport = port;
	cparams.pguser = username;
	cparams.prompt_password = prompt_password;
	cparams.override_dbname = NULL;

	conn = connectMaintenanceDatabase(&cparams, progname, echo);

	initPQExpBuffer(&sql);

	appendPQExpBuffer(&sql, "CREATE DATABASE %s",
					  fmtId(dbname));

	if (owner)
		appendPQExpBuffer(&sql, " OWNER %s", fmtId(owner));
	if (tablespace)
		appendPQExpBuffer(&sql, " TABLESPACE %s", fmtId(tablespace));
	if (encoding)
	{
		appendPQExpBufferStr(&sql, " ENCODING ");
		appendStringLiteralConn(&sql, encoding, conn);
	}
	if (strategy)
		appendPQExpBuffer(&sql, " STRATEGY %s", fmtId(strategy));
	if (template)
		appendPQExpBuffer(&sql, " TEMPLATE %s", fmtId(template));
	if (lc_collate)
	{
		appendPQExpBufferStr(&sql, " LC_COLLATE ");
		appendStringLiteralConn(&sql, lc_collate, conn);
	}
	if (lc_ctype)
	{
		appendPQExpBufferStr(&sql, " LC_CTYPE ");
		appendStringLiteralConn(&sql, lc_ctype, conn);
	}
	if (locale_provider)
		appendPQExpBuffer(&sql, " LOCALE_PROVIDER %s", locale_provider);
	if (icu_locale)
	{
		appendPQExpBufferStr(&sql, " ICU_LOCALE ");
		appendStringLiteralConn(&sql, icu_locale, conn);
	}

	appendPQExpBufferChar(&sql, ';');

	if (echo)
		printf("%s\n", sql.data);
	result = PQexec(conn, sql.data);

	if (PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		pg_log_error("database creation failed: %s", PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}

	PQclear(result);

	if (comment)
	{
		printfPQExpBuffer(&sql, "COMMENT ON DATABASE %s IS ", fmtId(dbname));
		appendStringLiteralConn(&sql, comment, conn);
		appendPQExpBufferChar(&sql, ';');

		if (echo)
			printf("%s\n", sql.data);
		result = PQexec(conn, sql.data);

		if (PQresultStatus(result) != PGRES_COMMAND_OK)
		{
			pg_log_error("comment creation failed (database was created): %s",
						 PQerrorMessage(conn));
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
	printf(_("      --icu-locale=LOCALE      ICU locale setting for the database\n"));
	printf(_("      --locale-provider={libc|icu}\n"
			 "                               locale provider for the database's default collation\n"));
	printf(_("  -O, --owner=OWNER            database user to own the new database\n"));
	printf(_("  -S, --strategy=STRATEGY      database creation strategy wal_log or file_copy\n"));
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
	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}
