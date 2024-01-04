/*-------------------------------------------------------------------------
 *
 * dropdb
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/scripts/dropdb.c
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
	static int	if_exists = 0;

	static struct option long_options[] = {
		{"host", required_argument, NULL, 'h'},
		{"port", required_argument, NULL, 'p'},
		{"username", required_argument, NULL, 'U'},
		{"no-password", no_argument, NULL, 'w'},
		{"password", no_argument, NULL, 'W'},
		{"echo", no_argument, NULL, 'e'},
		{"interactive", no_argument, NULL, 'i'},
		{"if-exists", no_argument, &if_exists, 1},
		{"maintenance-db", required_argument, NULL, 2},
		{"force", no_argument, NULL, 'f'},
		{NULL, 0, NULL, 0}
	};

	const char *progname;
	int			optindex;
	int			c;

	char	   *dbname = NULL;
	char	   *maintenance_db = NULL;
	char	   *host = NULL;
	char	   *port = NULL;
	char	   *username = NULL;
	enum trivalue prompt_password = TRI_DEFAULT;
	ConnParams	cparams;
	bool		echo = false;
	bool		interactive = false;
	bool		force = false;

	PQExpBufferData sql;

	PGconn	   *conn;
	PGresult   *result;

	pg_logging_init(argv[0]);
	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pgscripts"));

	handle_help_version_opts(argc, argv, "dropdb", help);

	while ((c = getopt_long(argc, argv, "efh:ip:U:wW", long_options, &optindex)) != -1)
	{
		switch (c)
		{
			case 'e':
				echo = true;
				break;
			case 'f':
				force = true;
				break;
			case 'h':
				host = pg_strdup(optarg);
				break;
			case 'i':
				interactive = true;
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
			case 0:
				/* this covers the long options */
				break;
			case 2:
				maintenance_db = pg_strdup(optarg);
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
			pg_log_error("missing required argument database name");
			pg_log_error_hint("Try \"%s --help\" for more information.", progname);
			exit(1);
		case 1:
			dbname = argv[optind];
			break;
		default:
			pg_log_error("too many command-line arguments (first is \"%s\")",
						 argv[optind + 1]);
			pg_log_error_hint("Try \"%s --help\" for more information.", progname);
			exit(1);
	}

	if (interactive)
	{
		printf(_("Database \"%s\" will be permanently removed.\n"), dbname);
		if (!yesno_prompt("Are you sure?"))
			exit(0);
	}

	initPQExpBuffer(&sql);

	appendPQExpBuffer(&sql, "DROP DATABASE %s%s%s;",
					  (if_exists ? "IF EXISTS " : ""),
					  fmtId(dbname),
					  force ? " WITH (FORCE)" : "");

	/* Avoid trying to drop postgres db while we are connected to it. */
	if (maintenance_db == NULL && strcmp(dbname, "postgres") == 0)
		maintenance_db = "template1";

	cparams.dbname = maintenance_db;
	cparams.pghost = host;
	cparams.pgport = port;
	cparams.pguser = username;
	cparams.prompt_password = prompt_password;
	cparams.override_dbname = NULL;

	conn = connectMaintenanceDatabase(&cparams, progname, echo);

	if (echo)
		printf("%s\n", sql.data);
	result = PQexec(conn, sql.data);
	if (PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		pg_log_error("database removal failed: %s", PQerrorMessage(conn));
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
	printf(_("%s removes a PostgreSQL database.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... DBNAME\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -e, --echo                show the commands being sent to the server\n"));
	printf(_("  -f, --force               try to terminate other connections before dropping\n"));
	printf(_("  -i, --interactive         prompt before deleting anything\n"));
	printf(_("  -V, --version             output version information, then exit\n"));
	printf(_("  --if-exists               don't report error if database doesn't exist\n"));
	printf(_("  -?, --help                show this help, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME       database server host or socket directory\n"));
	printf(_("  -p, --port=PORT           database server port\n"));
	printf(_("  -U, --username=USERNAME   user name to connect as\n"));
	printf(_("  -w, --no-password         never prompt for password\n"));
	printf(_("  -W, --password            force password prompt\n"));
	printf(_("  --maintenance-db=DBNAME   alternate maintenance database\n"));
	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}
