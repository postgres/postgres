/*-------------------------------------------------------------------------
 *
 * reindexdb
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/scripts/reindexdb.c,v 1.13 2008/01/01 19:45:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"
#include "common.h"
#include "dumputils.h"


static void reindex_one_database(const char *name, const char *dbname,
					 const char *type, const char *host,
					 const char *port, const char *username,
					 bool password, const char *progname,
					 bool echo);
static void reindex_all_databases(const char *host, const char *port,
					  const char *username, bool password,
					  const char *progname, bool echo,
					  bool quiet);
static void reindex_system_catalogs(const char *dbname,
						const char *host, const char *port,
						const char *username, bool password,
						const char *progname, bool echo);
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
		{"system", no_argument, NULL, 's'},
		{"table", required_argument, NULL, 't'},
		{"index", required_argument, NULL, 'i'},
		{NULL, 0, NULL, 0}
	};

	const char *progname;
	int			optindex;
	int			c;

	const char *dbname = NULL;
	const char *host = NULL;
	const char *port = NULL;
	const char *username = NULL;
	bool		password = false;
	bool		syscatalog = false;
	bool		alldb = false;
	bool		echo = false;
	bool		quiet = false;
	const char *table = NULL;
	const char *index = NULL;

	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], "pgscripts");

	handle_help_version_opts(argc, argv, "reindexdb", help);

	/* process command-line options */
	while ((c = getopt_long(argc, argv, "h:p:U:Weqd:ast:i:", long_options, &optindex)) != -1)
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
			case 's':
				syscatalog = true;
				break;
			case 't':
				table = optarg;
				break;
			case 'i':
				index = optarg;
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
			fprintf(stderr, _("%s: too many command-line arguments (first is \"%s\")\n"), progname, argv[optind + 1]);
			fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
			exit(1);
	}

	setup_cancel_handler();

	if (alldb)
	{
		if (dbname)
		{
			fprintf(stderr, _("%s: cannot reindex all databases and a specific one at the same time\n"), progname);
			exit(1);
		}
		if (syscatalog)
		{
			fprintf(stderr, _("%s: cannot reindex all databases and system catalogs at the same time\n"), progname);
			exit(1);
		}
		if (table)
		{
			fprintf(stderr, _("%s: cannot reindex a specific table in all databases\n"), progname);
			exit(1);
		}
		if (index)
		{
			fprintf(stderr, _("%s: cannot reindex a specific index in all databases\n"), progname);
			exit(1);
		}

		reindex_all_databases(host, port, username, password,
							  progname, echo, quiet);
	}
	else if (syscatalog)
	{
		if (table)
		{
			fprintf(stderr, _("%s: cannot reindex a specific table and system catalogs at the same time\n"), progname);
			exit(1);
		}
		if (index)
		{
			fprintf(stderr, _("%s: cannot reindex a specific index and system catalogs at the same time\n"), progname);
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

		reindex_system_catalogs(dbname, host, port, username, password,
								progname, echo);
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

		if (index)
			reindex_one_database(index, dbname, "INDEX", host, port,
								 username, password, progname, echo);
		if (table)
			reindex_one_database(table, dbname, "TABLE", host, port,
								 username, password, progname, echo);
		/* reindex database only if index or table is not specified */
		if (index == NULL && table == NULL)
			reindex_one_database(dbname, dbname, "DATABASE", host, port,
								 username, password, progname, echo);
	}

	exit(0);
}

static void
reindex_one_database(const char *name, const char *dbname, const char *type,
					 const char *host, const char *port, const char *username,
					 bool password, const char *progname, bool echo)
{
	PQExpBufferData sql;

	PGconn	   *conn;

	initPQExpBuffer(&sql);

	appendPQExpBuffer(&sql, "REINDEX");
	if (strcmp(type, "TABLE") == 0)
		appendPQExpBuffer(&sql, " TABLE %s", fmtId(name));
	else if (strcmp(type, "INDEX") == 0)
		appendPQExpBuffer(&sql, " INDEX %s", fmtId(name));
	else if (strcmp(type, "DATABASE") == 0)
		appendPQExpBuffer(&sql, " DATABASE %s", fmtId(name));
	appendPQExpBuffer(&sql, ";\n");

	conn = connectDatabase(dbname, host, port, username, password, progname);

	if (!executeMaintenanceCommand(conn, sql.data, echo))
	{
		if (strcmp(type, "TABLE") == 0)
			fprintf(stderr, _("%s: reindexing of table \"%s\" in database \"%s\" failed: %s"),
					progname, name, dbname, PQerrorMessage(conn));
		if (strcmp(type, "INDEX") == 0)
			fprintf(stderr, _("%s: reindexing of index \"%s\" in database \"%s\" failed: %s"),
					progname, name, dbname, PQerrorMessage(conn));
		else
			fprintf(stderr, _("%s: reindexing of database \"%s\" failed: %s"),
					progname, dbname, PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}

	PQfinish(conn);
	termPQExpBuffer(&sql);
}

static void
reindex_all_databases(const char *host, const char *port,
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
			printf(_("%s: reindexing database \"%s\"\n"), progname, dbname);
			fflush(stdout);
		}

		reindex_one_database(dbname, dbname, "DATABASE", host, port, username,
							 password, progname, echo);
	}

	PQclear(result);
}

static void
reindex_system_catalogs(const char *dbname, const char *host, const char *port,
						const char *username, bool password,
						const char *progname, bool echo)
{
	PQExpBufferData sql;

	PGconn	   *conn;

	initPQExpBuffer(&sql);

	appendPQExpBuffer(&sql, "REINDEX SYSTEM %s;\n", dbname);

	conn = connectDatabase(dbname, host, port, username, password, progname);
	if (!executeMaintenanceCommand(conn, sql.data, echo))
	{
		fprintf(stderr, _("%s: reindexing of system catalogs failed: %s"),
				progname, PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}
	PQfinish(conn);
	termPQExpBuffer(&sql);
}

static void
help(const char *progname)
{
	printf(_("%s reindexes a PostgreSQL database.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [DBNAME]\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -a, --all                 reindex all databases\n"));
	printf(_("  -s, --system              reindex system catalogs\n"));
	printf(_("  -d, --dbname=DBNAME       database to reindex\n"));
	printf(_("  -t, --table=TABLE         reindex specific table only\n"));
	printf(_("  -i, --index=INDEX         recreate specific index only\n"));
	printf(_("  -e, --echo                show the commands being sent to the server\n"));
	printf(_("  -q, --quiet               don't write any messages\n"));
	printf(_("  --help                    show this help, then exit\n"));
	printf(_("  --version                 output version information, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME       database server host or socket directory\n"));
	printf(_("  -p, --port=PORT           database server port\n"));
	printf(_("  -U, --username=USERNAME   user name to connect as\n"));
	printf(_("  -W, --password            force password prompt\n"));
	printf(_("\nRead the description of the SQL command REINDEX for details.\n"));
	printf(_("\nReport bugs to <pgsql-bugs@postgresql.org>.\n"));
}
