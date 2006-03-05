/*-------------------------------------------------------------------------
 *
 * vacuumdb
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/bin/scripts/vacuumdb.c,v 1.14 2006/03/05 15:58:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"
#include "common.h"


static void vacuum_one_database(const char *dbname, bool full, bool verbose, bool analyze,
					const char *table,
					const char *host, const char *port,
					const char *username, bool password,
					const char *progname, bool echo, bool quiet);
static void vacuum_all_databases(bool full, bool verbose, bool analyze,
					 const char *host, const char *port,
					 const char *username, bool password,
					 const char *progname, bool echo, bool quiet);

static void help(const char *progname);


int
main(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"host", required_argument, NULL, 'h'},
		{"port", required_argument, NULL, 'p'},
		{"username", required_argument, NULL, 'U'},
		{"password", no_argument, NULL, 'W'},
		{"echo", no_argument, NULL, 'e'},
		{"quiet", no_argument, NULL, 'q'},
		{"dbname", required_argument, NULL, 'd'},
		{"analyze", no_argument, NULL, 'z'},
		{"all", no_argument, NULL, 'a'},
		{"table", required_argument, NULL, 't'},
		{"full", no_argument, NULL, 'f'},
		{"verbose", no_argument, NULL, 'v'},
		{NULL, 0, NULL, 0}
	};

	const char *progname;
	int			optindex;
	int			c;

	const char *dbname = NULL;
	char	   *host = NULL;
	char	   *port = NULL;
	char	   *username = NULL;
	bool		password = false;
	bool		echo = false;
	bool		quiet = false;
	bool		analyze = false;
	bool		alldb = false;
	char	   *table = NULL;
	bool		full = false;
	bool		verbose = false;

	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], "pgscripts");

	handle_help_version_opts(argc, argv, "vacuumdb", help);

	while ((c = getopt_long(argc, argv, "h:p:U:Weqd:zat:fv", long_options, &optindex)) != -1)
	{
		switch (c)
		{
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
			case 'e':
				echo = true;
				break;
			case 'q':
				quiet = true;
				break;
			case 'd':
				dbname = optarg;
				break;
			case 'z':
				analyze = true;
				break;
			case 'a':
				alldb = true;
				break;
			case 't':
				table = optarg;
				break;
			case 'f':
				full = true;
				break;
			case 'v':
				verbose = true;
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
		default:
			fprintf(stderr, _("%s: too many command-line arguments (first is \"%s\")\n"),
					progname, argv[optind + 1]);
			fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
			exit(1);
	}

	if (alldb)
	{
		if (dbname)
		{
			fprintf(stderr, _("%s: cannot vacuum all databases and a specific one at the same time\n"),
					progname);
			exit(1);
		}
		if (table)
		{
			fprintf(stderr, _("%s: cannot vacuum a specific table in all databases\n"),
					progname);
			exit(1);
		}

		vacuum_all_databases(full, verbose, analyze,
							 host, port, username, password,
							 progname, echo, quiet);
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
				dbname = get_user_name(progname);
		}

		vacuum_one_database(dbname, full, verbose, analyze, table,
							host, port, username, password,
							progname, echo, quiet);
	}

	exit(0);
}


static void
vacuum_one_database(const char *dbname, bool full, bool verbose, bool analyze,
					const char *table,
					const char *host, const char *port,
					const char *username, bool password,
					const char *progname, bool echo, bool quiet)
{
	PQExpBufferData sql;

	PGconn	   *conn;
	PGresult   *result;

	initPQExpBuffer(&sql);

	appendPQExpBuffer(&sql, "VACUUM");
	if (full)
		appendPQExpBuffer(&sql, " FULL");
	if (verbose)
		appendPQExpBuffer(&sql, " VERBOSE");
	if (analyze)
		appendPQExpBuffer(&sql, " ANALYZE");
	if (table)
		appendPQExpBuffer(&sql, " %s", table);
	appendPQExpBuffer(&sql, ";\n");

	conn = connectDatabase(dbname, host, port, username, password, progname);

	if (echo)
		printf("%s", sql.data);
	result = PQexec(conn, sql.data);

	if (PQresultStatus(result) != PGRES_COMMAND_OK)
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

	PQclear(result);
	PQfinish(conn);
	termPQExpBuffer(&sql);

	if (!quiet)
	{
		puts("VACUUM");
		fflush(stdout);
	}
}


static void
vacuum_all_databases(bool full, bool verbose, bool analyze,
					 const char *host, const char *port,
					 const char *username, bool password,
					 const char *progname, bool echo, bool quiet)
{
	PGconn	   *conn;
	PGresult   *result;
	int			i;

	conn = connectDatabase("postgres", host, port, username, password, progname);
	result = executeQuery(conn, "SELECT datname FROM pg_database WHERE datallowconn;", progname, echo);
	PQfinish(conn);

	for (i = 0; i < PQntuples(result); i++)
	{
		char	   *dbname = PQgetvalue(result, i, 0);

		if (!quiet)
			fprintf(stderr, _("%s: vacuuming database \"%s\"\n"), progname, dbname);

		vacuum_one_database(dbname, full, verbose, analyze, NULL,
							host, port, username, password,
							progname, echo, quiet);
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
	printf(_("  -t, --table='TABLE[(COLUMNS)]'  vacuum specific table only\n"));
	printf(_("  -f, --full                      do full vacuuming\n"));
	printf(_("  -z, --analyze                   update optimizer hints\n"));
	printf(_("  -e, --echo                      show the commands being sent to the server\n"));
	printf(_("  -q, --quiet                     don't write any messages\n"));
	printf(_("  -v, --verbose                   write a lot of output\n"));
	printf(_("  --help                          show this help, then exit\n"));
	printf(_("  --version                       output version information, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME       database server host or socket directory\n"));
	printf(_("  -p, --port=PORT           database server port\n"));
	printf(_("  -U, --username=USERNAME   user name to connect as\n"));
	printf(_("  -W, --password            prompt for password\n"));
	printf(_("\nRead the description of the SQL command VACUUM for details.\n"));
	printf(_("\nReport bugs to <pgsql-bugs@postgresql.org>.\n"));
}
