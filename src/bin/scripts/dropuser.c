/*-------------------------------------------------------------------------
 *
 * dropuser
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Header: /cvsroot/pgsql/src/bin/scripts/dropuser.c,v 1.5.4.1 2004/01/01 19:27:28 tgl Exp $
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
		{"interactive", no_argument, NULL, 'i'},
		{NULL, 0, NULL, 0}
	};

	char	   *progname;
	int			optindex;
	int			c;

	char	   *dropuser = NULL;
	char	   *host = NULL;
	char	   *port = NULL;
	char	   *username = NULL;
	bool		password = false;
	bool		echo = false;
	bool		quiet = false;
	bool		interactive = false;

	PQExpBufferData sql;

	PGconn	   *conn;
	PGresult   *result;

	progname = get_progname(argv[0]);
	init_nls();
	handle_help_version_opts(argc, argv, "dropuser", help);

	while ((c = getopt_long(argc, argv, "h:p:U:Weqi", long_options, &optindex)) != -1)
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
			case 'i':
				interactive = true;
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
			dropuser = argv[optind];
			break;
		default:
			fprintf(stderr, _("%s: too many command-line arguments (first is \"%s\")\n"),
					progname, argv[optind + 1]);
			fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
			exit(1);
	}

	if (dropuser == NULL)
		dropuser = simple_prompt("Enter name of user to drop: ", 128, true);

	if (interactive)
	{
		char	   *reply;

		printf(_("User \"%s\" will be permanently removed.\n"), dropuser);
		reply = simple_prompt("Are you sure? (y/n) ", 1, true);
		if (check_yesno_response(reply) != 1)
			exit(0);
	}

	initPQExpBuffer(&sql);
	appendPQExpBuffer(&sql, "DROP USER %s;\n", fmtId(dropuser));

	conn = connectDatabase("template1", host, port, username, password, progname);

	if (echo)
		printf("%s", sql.data);
	result = PQexec(conn, sql.data);

	if (PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, _("%s: removal of user \"%s\" failed: %s"),
				progname, dropuser, PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}

	PQfinish(conn);
	if (!quiet)
	{
		puts("DROP USER");
		fflush(stdout);
	}
	exit(0);
}


static void
help(const char *progname)
{
	printf(_("%s removes a PostgreSQL user.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [USERNAME]\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -e, --echo                show the commands being sent to the server\n"));
	printf(_("  -i, --interactive         prompt before deleting anything\n"));
	printf(_("  -q, --quiet               don't write any messages\n"));
	printf(_("  -h, --host=HOSTNAME       database server host or socket directory\n"));
	printf(_("  -p, --port=PORT           database server port\n"));
	printf(_("  -U, --username=USERNAME   user name to connect as (not the one to drop)\n"));
	printf(_("  -W, --password            prompt for password to connect\n"));
	printf(_("  --help                    show this help, then exit\n"));
	printf(_("  --version                 output version information, then exit\n"));
	printf(_("\nReport bugs to <pgsql-bugs@postgresql.org>.\n"));
}
