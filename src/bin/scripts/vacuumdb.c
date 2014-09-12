/*-------------------------------------------------------------------------
 *
 * vacuumdb
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/scripts/vacuumdb.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"
#include "common.h"
#include "dumputils.h"


static void vacuum_one_database(const char *dbname, bool full, bool verbose,
	bool and_analyze, bool analyze_only, bool analyze_in_stages, int stage, bool freeze,
					const char *table, const char *host, const char *port,
					const char *username, enum trivalue prompt_password,
					const char *progname, bool echo, bool quiet);
static void vacuum_all_databases(bool full, bool verbose, bool and_analyze,
					 bool analyze_only, bool analyze_in_stages, bool freeze,
					 const char *maintenance_db,
					 const char *host, const char *port,
					 const char *username, enum trivalue prompt_password,
					 const char *progname, bool echo, bool quiet);

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
		{"quiet", no_argument, NULL, 'q'},
		{"dbname", required_argument, NULL, 'd'},
		{"analyze", no_argument, NULL, 'z'},
		{"analyze-only", no_argument, NULL, 'Z'},
		{"freeze", no_argument, NULL, 'F'},
		{"all", no_argument, NULL, 'a'},
		{"table", required_argument, NULL, 't'},
		{"full", no_argument, NULL, 'f'},
		{"verbose", no_argument, NULL, 'v'},
		{"maintenance-db", required_argument, NULL, 2},
		{"analyze-in-stages", no_argument, NULL, 3},
		{NULL, 0, NULL, 0}
	};

	const char *progname;
	int			optindex;
	int			c;

	const char *dbname = NULL;
	const char *maintenance_db = NULL;
	char	   *host = NULL;
	char	   *port = NULL;
	char	   *username = NULL;
	enum trivalue prompt_password = TRI_DEFAULT;
	bool		echo = false;
	bool		quiet = false;
	bool		and_analyze = false;
	bool		analyze_only = false;
	bool		analyze_in_stages = false;
	bool		freeze = false;
	bool		alldb = false;
	bool		full = false;
	bool		verbose = false;
	SimpleStringList tables = {NULL, NULL};

	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pgscripts"));

	handle_help_version_opts(argc, argv, "vacuumdb", help);

	while ((c = getopt_long(argc, argv, "h:p:U:wWeqd:zZFat:fv", long_options, &optindex)) != -1)
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
			case 'q':
				quiet = true;
				break;
			case 'd':
				dbname = pg_strdup(optarg);
				break;
			case 'z':
				and_analyze = true;
				break;
			case 'Z':
				analyze_only = true;
				break;
			case 'F':
				freeze = true;
				break;
			case 'a':
				alldb = true;
				break;
			case 't':
				simple_string_list_append(&tables, optarg);
				break;
			case 'f':
				full = true;
				break;
			case 'v':
				verbose = true;
				break;
			case 2:
				maintenance_db = pg_strdup(optarg);
				break;
			case 3:
				analyze_in_stages = analyze_only = true;
				break;
			default:
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
				exit(1);
		}
	}


	/*
	 * Non-option argument specifies database name as long as it wasn't
	 * already specified with -d / --dbname
	 */
	if (optind < argc && dbname == NULL)
	{
		dbname = argv[optind];
		optind++;
	}

	if (optind < argc)
	{
		fprintf(stderr, _("%s: too many command-line arguments (first is \"%s\")\n"),
				progname, argv[optind]);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
		exit(1);
	}

	if (analyze_only)
	{
		if (full)
		{
			fprintf(stderr, _("%s: cannot use the \"full\" option when performing only analyze\n"),
					progname);
			exit(1);
		}
		if (freeze)
		{
			fprintf(stderr, _("%s: cannot use the \"freeze\" option when performing only analyze\n"),
					progname);
			exit(1);
		}
		/* allow 'and_analyze' with 'analyze_only' */
	}

	setup_cancel_handler();

	if (alldb)
	{
		if (dbname)
		{
			fprintf(stderr, _("%s: cannot vacuum all databases and a specific one at the same time\n"),
					progname);
			exit(1);
		}
		if (tables.head != NULL)
		{
			fprintf(stderr, _("%s: cannot vacuum specific table(s) in all databases\n"),
					progname);
			exit(1);
		}

		vacuum_all_databases(full, verbose, and_analyze, analyze_only, analyze_in_stages, freeze,
							 maintenance_db, host, port, username,
							 prompt_password, progname, echo, quiet);
	}
	else
	{
		if (dbname == NULL)
		{
			if (getenv("PGDATABASE"))
				dbname = getenv("PGDATABASE");
			else if (getenv("PGUSER"))
				dbname = getenv("PGUSER");
			else
				dbname = get_user_name_or_exit(progname);
		}

		if (tables.head != NULL)
		{
			SimpleStringListCell *cell;

			for (cell = tables.head; cell; cell = cell->next)
			{
				vacuum_one_database(dbname, full, verbose, and_analyze,
									analyze_only, analyze_in_stages, -1,
									freeze, cell->val,
									host, port, username, prompt_password,
									progname, echo, quiet);
			}
		}
		else
			vacuum_one_database(dbname, full, verbose, and_analyze,
								analyze_only, analyze_in_stages, -1,
								freeze, NULL,
								host, port, username, prompt_password,
								progname, echo, quiet);
	}

	exit(0);
}


static void
run_vacuum_command(PGconn *conn, const char *sql, bool echo, const char *dbname, const char *table, const char *progname)
{
	if (!executeMaintenanceCommand(conn, sql, echo))
	{
		if (table)
			fprintf(stderr, _("%s: vacuuming of table \"%s\" in database \"%s\" failed: %s"),
					progname, table, dbname, PQerrorMessage(conn));
		else
			fprintf(stderr, _("%s: vacuuming of database \"%s\" failed: %s"),
					progname, dbname, PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}
}


static void
vacuum_one_database(const char *dbname, bool full, bool verbose, bool and_analyze,
	bool analyze_only, bool analyze_in_stages, int stage, bool freeze, const char *table,
					const char *host, const char *port,
					const char *username, enum trivalue prompt_password,
					const char *progname, bool echo, bool quiet)
{
	PQExpBufferData sql;

	PGconn	   *conn;

	initPQExpBuffer(&sql);

	conn = connectDatabase(dbname, host, port, username, prompt_password,
						   progname, false);

	if (analyze_only)
	{
		appendPQExpBufferStr(&sql, "ANALYZE");
		if (verbose)
			appendPQExpBufferStr(&sql, " VERBOSE");
	}
	else
	{
		appendPQExpBufferStr(&sql, "VACUUM");
		if (PQserverVersion(conn) >= 90000)
		{
			const char *paren = " (";
			const char *comma = ", ";
			const char *sep = paren;

			if (full)
			{
				appendPQExpBuffer(&sql, "%sFULL", sep);
				sep = comma;
			}
			if (freeze)
			{
				appendPQExpBuffer(&sql, "%sFREEZE", sep);
				sep = comma;
			}
			if (verbose)
			{
				appendPQExpBuffer(&sql, "%sVERBOSE", sep);
				sep = comma;
			}
			if (and_analyze)
			{
				appendPQExpBuffer(&sql, "%sANALYZE", sep);
				sep = comma;
			}
			if (sep != paren)
				appendPQExpBufferStr(&sql, ")");
		}
		else
		{
			if (full)
				appendPQExpBufferStr(&sql, " FULL");
			if (freeze)
				appendPQExpBufferStr(&sql, " FREEZE");
			if (verbose)
				appendPQExpBufferStr(&sql, " VERBOSE");
			if (and_analyze)
				appendPQExpBufferStr(&sql, " ANALYZE");
		}
	}
	if (table)
		appendPQExpBuffer(&sql, " %s", table);
	appendPQExpBufferStr(&sql, ";");

	if (analyze_in_stages)
	{
		const char *stage_commands[] = {
			"SET default_statistics_target=1; SET vacuum_cost_delay=0;",
			"SET default_statistics_target=10; RESET vacuum_cost_delay;",
			"RESET default_statistics_target;"
		};
		const char *stage_messages[] = {
			gettext_noop("Generating minimal optimizer statistics (1 target)"),
			gettext_noop("Generating medium optimizer statistics (10 targets)"),
			gettext_noop("Generating default (full) optimizer statistics")
		};

		if (stage == -1)
		{
			int		i;

			/* Run all stages. */
			for (i = 0; i < 3; i++)
			{
				if (!quiet)
				{
					puts(gettext(stage_messages[i]));
					fflush(stdout);
				}
				executeCommand(conn, stage_commands[i], progname, echo);
				run_vacuum_command(conn, sql.data, echo, dbname, table, progname);
			}
		}
		else
		{
			/* Otherwise, we got a stage from vacuum_all_databases(), so run
			 * only that one. */
			if (!quiet)
			{
				puts(gettext(stage_messages[stage]));
				fflush(stdout);
			}
			executeCommand(conn, stage_commands[stage], progname, echo);
			run_vacuum_command(conn, sql.data, echo, dbname, table, progname);
		}

	}
	else
		run_vacuum_command(conn, sql.data, echo, dbname, NULL, progname);

	PQfinish(conn);
	termPQExpBuffer(&sql);
}


static void
vacuum_all_databases(bool full, bool verbose, bool and_analyze, bool analyze_only,
			 bool analyze_in_stages, bool freeze, const char *maintenance_db,
					 const char *host, const char *port,
					 const char *username, enum trivalue prompt_password,
					 const char *progname, bool echo, bool quiet)
{
	PGconn	   *conn;
	PGresult   *result;
	int			stage;

	conn = connectMaintenanceDatabase(maintenance_db, host, port,
									  username, prompt_password, progname);
	result = executeQuery(conn, "SELECT datname FROM pg_database WHERE datallowconn ORDER BY 1;", progname, echo);
	PQfinish(conn);

	/* If analyzing in stages, then run through all stages.  Otherwise just
	 * run once, passing -1 as the stage. */
	for (stage = (analyze_in_stages ? 0 : -1);
		 stage < (analyze_in_stages ? 3 : 0);
		 stage++)
	{
		int			i;

		for (i = 0; i < PQntuples(result); i++)
		{
			char	   *dbname = PQgetvalue(result, i, 0);

			if (!quiet)
			{
				printf(_("%s: vacuuming database \"%s\"\n"), progname, dbname);
				fflush(stdout);
			}

			vacuum_one_database(dbname, full, verbose, and_analyze, analyze_only,
								analyze_in_stages, stage,
							freeze, NULL, host, port, username, prompt_password,
								progname, echo, quiet);
		}
	}

	PQclear(result);
}


static void
help(const char *progname)
{
	printf(_("%s cleans and analyzes a PostgreSQL database.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [DBNAME]\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -a, --all                       vacuum all databases\n"));
	printf(_("  -d, --dbname=DBNAME             database to vacuum\n"));
	printf(_("  -e, --echo                      show the commands being sent to the server\n"));
	printf(_("  -f, --full                      do full vacuuming\n"));
	printf(_("  -F, --freeze                    freeze row transaction information\n"));
	printf(_("  -q, --quiet                     don't write any messages\n"));
	printf(_("  -t, --table='TABLE[(COLUMNS)]'  vacuum specific table(s) only\n"));
	printf(_("  -v, --verbose                   write a lot of output\n"));
	printf(_("  -V, --version                   output version information, then exit\n"));
	printf(_("  -z, --analyze                   update optimizer statistics\n"));
	printf(_("  -Z, --analyze-only              only update optimizer statistics\n"));
	printf(_("      --analyze-in-stages         only update optimizer statistics, in multiple\n"
		   "                                  stages for faster results\n"));
	printf(_("  -?, --help                      show this help, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME       database server host or socket directory\n"));
	printf(_("  -p, --port=PORT           database server port\n"));
	printf(_("  -U, --username=USERNAME   user name to connect as\n"));
	printf(_("  -w, --no-password         never prompt for password\n"));
	printf(_("  -W, --password            force password prompt\n"));
	printf(_("  --maintenance-db=DBNAME   alternate maintenance database\n"));
	printf(_("\nRead the description of the SQL command VACUUM for details.\n"));
	printf(_("\nReport bugs to <pgsql-bugs@postgresql.org>.\n"));
}
