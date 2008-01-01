/*-------------------------------------------------------------------------
 *
 * createdb
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/bin/scripts/createdb.c,v 1.26 2008/01/01 19:45:56 momjian Exp $
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
		{"password", no_argument, NULL, 'W'},
		{"echo", no_argument, NULL, 'e'},
		{"quiet", no_argument, NULL, 'q'},
		{"owner", required_argument, NULL, 'O'},
		{"tablespace", required_argument, NULL, 'D'},
		{"template", required_argument, NULL, 'T'},
		{"encoding", required_argument, NULL, 'E'},
		{NULL, 0, NULL, 0}
	};

	const char *progname;
	int			optindex;
	int			c;

	const char *dbname = NULL;
	char	   *comment = NULL;
	char	   *host = NULL;
	char	   *port = NULL;
	char	   *username = NULL;
	bool		password = false;
	bool		echo = false;
	char	   *owner = NULL;
	char	   *tablespace = NULL;
	char	   *template = NULL;
	char	   *encoding = NULL;

	PQExpBufferData sql;

	PGconn	   *conn;
	PGresult   *result;

	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], "pgscripts");

	handle_help_version_opts(argc, argv, "createdb", help);

	while ((c = getopt_long(argc, argv, "h:p:U:WeqO:D:T:E:", long_options, &optindex)) != -1)
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
				/* obsolete; remove in 8.4 */
				break;
			case 'O':
				owner = optarg;
				break;
			case 'D':
				tablespace = optarg;
				break;
			case 'T':
				template = optarg;
				break;
			case 'E':
				encoding = optarg;
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
			dbname = get_user_name(progname);
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
	appendPQExpBuffer(&sql, ";\n");

	conn = connectDatabase(strcmp(dbname, "postgres") == 0 ? "template1" : "postgres",
						   host, port, username, password, progname);

	if (echo)
		printf("%s", sql.data);
	result = PQexec(conn, sql.data);

	if (PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, _("%s: database creation failed: %s"),
				progname, PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}

	PQclear(result);
	PQfinish(conn);

	if (comment)
	{
		conn = connectDatabase(dbname, host, port, username, password, progname);

		printfPQExpBuffer(&sql, "COMMENT ON DATABASE %s IS ", fmtId(dbname));
		appendStringLiteralConn(&sql, comment, conn);
		appendPQExpBuffer(&sql, ";\n");

		if (echo)
			printf("%s", sql.data);
		result = PQexec(conn, sql.data);

		if (PQresultStatus(result) != PGRES_COMMAND_OK)
		{
			fprintf(stderr, _("%s: comment creation failed (database was created): %s"),
					progname, PQerrorMessage(conn));
			PQfinish(conn);
			exit(1);
		}

		PQclear(result);
		PQfinish(conn);
	}

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
	printf(_("  -E, --encoding=ENCODING      encoding for the database\n"));
	printf(_("  -O, --owner=OWNER            database user to own the new database\n"));
	printf(_("  -T, --template=TEMPLATE      template database to copy\n"));
	printf(_("  -e, --echo                   show the commands being sent to the server\n"));
	printf(_("  --help                       show this help, then exit\n"));
	printf(_("  --version                    output version information, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME          database server host or socket directory\n"));
	printf(_("  -p, --port=PORT              database server port\n"));
	printf(_("  -U, --username=USERNAME      user name to connect as\n"));
	printf(_("  -W, --password               force password prompt\n"));
	printf(_("\nBy default, a database with the same name as the current user is created.\n"));
	printf(_("\nReport bugs to <pgsql-bugs@postgresql.org>.\n"));
}
