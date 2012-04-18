/*-------------------------------------------------------------------------
 *
 * clusterdb
 *
 * Portions Copyright (c) 2002-2008, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/scripts/clusterdb.c,v 1.20 2008/01/01 19:45:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"
#include "common.h"
#include "dumputils.h"


static void cluster_one_database(const char *dbname, const char *table,
					 const char *host, const char *port,
					 const char *username, bool password,
					 const char *progname, bool echo);
static void cluster_all_databases(const char *host, const char *port,
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
		{"all", no_argument, NULL, 'a'},
		{"table", required_argument, NULL, 't'},
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
	bool		alldb = false;
	char	   *table = NULL;

	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], "pgscripts");

	handle_help_version_opts(argc, argv, "clusterdb", help);

	while ((c = getopt_long(argc, argv, "h:p:U:Weqd:at:", long_options, &optindex)) != -1)
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
			case 'a':
				alldb = true;
				break;
			case 't':
				table = optarg;
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

	setup_cancel_handler();

	if (alldb)
	{
		if (dbname)
		{
			fprintf(stderr, _("%s: cannot cluster all databases and a specific one at the same time\n"),
					progname);
			exit(1);
		}
		if (table)
		{
			fprintf(stderr, _("%s: cannot cluster a specific table in all databases\n"),
					progname);
			exit(1);
		}

		cluster_all_databases(host, port, username, password,
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

		cluster_one_database(dbname, table,
							 host, port, username, password,
							 progname, echo);
	}

	exit(0);
}


static void
cluster_one_database(const char *dbname, const char *table,
					 const char *host, const char *port,
					 const char *username, bool password,
					 const char *progname, bool echo)
{
	PQExpBufferData sql;

	PGconn	   *conn;

	initPQExpBuffer(&sql);

	appendPQExpBuffer(&sql, "CLUSTER");
	if (table)
		appendPQExpBuffer(&sql, " %s", fmtId(table));
	appendPQExpBuffer(&sql, ";\n");

	conn = connectDatabase(dbname, host, port, username, password, progname);
	if (!executeMaintenanceCommand(conn, sql.data, echo))
	{
		if (table)
			fprintf(stderr, _("%s: clustering of table \"%s\" in database \"%s\" failed: %s"),
					progname, table, dbname, PQerrorMessage(conn));
		else
			fprintf(stderr, _("%s: clustering of database \"%s\" failed: %s"),
					progname, dbname, PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}
	PQfinish(conn);
	termPQExpBuffer(&sql);
}


static void
cluster_all_databases(const char *host, const char *port,
					  const char *username, bool password,
					  const char *progname, bool echo, bool quiet)
{
	PGconn	   *conn;
	PGresult   *result;
	int			i;

	conn = connectDatabase("postgres", host, port, username, password, progname);
	result = executeQuery(conn, "SELECT datname FROM pg_database WHERE datallowconn ORDER BY 1;", progname, echo);
	PQfinish(conn);

	for (i = 0; i < PQntuples(result); i++)
	{
		char	   *dbname = PQgetvalue(result, i, 0);

		if (!quiet)
		{
			printf(_("%s: clustering database \"%s\"\n"), progname, dbname);
			fflush(stdout);
		}

		cluster_one_database(dbname, NULL,
							 host, port, username, password,
							 progname, echo);
	}

	PQclear(result);
}


static void
help(const char *progname)
{
	printf(_("%s clusters all previously clustered tables in a database.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [DBNAME]\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -a, --all                 cluster all databases\n"));
	printf(_("  -d, --dbname=DBNAME       database to cluster\n"));
	printf(_("  -t, --table=TABLE         cluster specific table only\n"));
	printf(_("  -e, --echo                show the commands being sent to the server\n"));
	printf(_("  -q, --quiet               don't write any messages\n"));
	printf(_("  --help                    show this help, then exit\n"));
	printf(_("  --version                 output version information, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME       database server host or socket directory\n"));
	printf(_("  -p, --port=PORT           database server port\n"));
	printf(_("  -U, --username=USERNAME   user name to connect as\n"));
	printf(_("  -W, --password            force password prompt\n"));
	printf(_("\nRead the description of the SQL command CLUSTER for details.\n"));
	printf(_("\nReport bugs to <pgsql-bugs@postgresql.org>.\n"));
}
