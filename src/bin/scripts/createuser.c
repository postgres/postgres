/*-------------------------------------------------------------------------
 *
 * createuser
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Header: /cvsroot/pgsql/src/bin/scripts/createuser.c,v 1.6.4.2 2004/01/09 00:15:19 tgl Exp $
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
		{"createdb", no_argument, NULL, 'd'},
		{"no-createdb", no_argument, NULL, 'D'},
		{"adduser", no_argument, NULL, 'a'},
		{"no-adduser", no_argument, NULL, 'A'},
		{"sysid", required_argument, NULL, 'i'},
		{"pwprompt", no_argument, NULL, 'P'},
		{"encrypted", no_argument, NULL, 'E'},
		{"unencrypted", no_argument, NULL, 'N'},
		{NULL, 0, NULL, 0}
	};

	char	   *progname;
	int			optindex;
	int			c;

	char	   *newuser = NULL;
	char	   *host = NULL;
	char	   *port = NULL;
	char	   *username = NULL;
	bool		password = false;
	bool		echo = false;
	bool		quiet = false;
	int			createdb = 0;
	int			adduser = 0;
	char	   *sysid = NULL;
	bool		pwprompt = false;
	int			encrypted = 0;	/* 0 uses server default */
	char	   *newpassword = NULL;

	PQExpBufferData sql;

	PGconn	   *conn;
	PGresult   *result;

	progname = get_progname(argv[0]);
	init_nls();
	handle_help_version_opts(argc, argv, "createuser", help);

	while ((c = getopt_long(argc, argv, "h:p:U:WeqaAdDi:PEN", long_options, &optindex)) != -1)
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
			case 'a':
				adduser = +1;
				break;
			case 'A':
				adduser = -1;
				break;
			case 'd':
				createdb = +1;
				break;
			case 'D':
				createdb = -1;
				break;
			case 'i':
				sysid = optarg;
				break;
			case 'P':
				pwprompt = true;
				break;
			case 'E':
				encrypted = +1;
				break;
			case 'N':
				encrypted = -1;
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
			newuser = argv[optind];
			break;
		default:
			fprintf(stderr, _("%s: too many command-line arguments (first is \"%s\")\n"),
					progname, argv[optind + 1]);
			fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
			exit(1);
	}

	if (sysid)
	{
		char	   *endptr;

		if (strtol(sysid, &endptr, 10) <= 0 || *endptr != '\0')
		{
			fprintf(stderr, _("%s: user ID must be a positive number\n"), progname);
			exit(1);
		}
	}

	if (newuser == NULL)
		newuser = simple_prompt("Enter name of user to add: ", 128, true);

	if (pwprompt)
	{
		char	   *pw1,
				   *pw2;

		pw1 = simple_prompt("Enter password for new user: ", 100, false);
		pw2 = simple_prompt("Enter it again: ", 100, false);
		if (strcmp(pw1, pw2) != 0)
		{
			fprintf(stderr, _("Passwords didn't match.\n"));
			exit(1);
		}
		newpassword = pw1;
		free(pw2);
	}

	if (createdb == 0)
	{
		char	   *reply;

		reply = simple_prompt("Shall the new user be allowed to create databases? (y/n) ", 1, true);
		if (check_yesno_response(reply) == 1)
			createdb = +1;
		else
			createdb = -1;
	}

	if (adduser == 0)
	{
		char	   *reply;

		reply = simple_prompt("Shall the new user be allowed to create more new users? (y/n) ", 1, true);
		if (check_yesno_response(reply) == 1)
			adduser = +1;
		else
			adduser = -1;
	}

	initPQExpBuffer(&sql);

	printfPQExpBuffer(&sql, "CREATE USER %s", fmtId(newuser));
	if (sysid)
		appendPQExpBuffer(&sql, " SYSID %s", sysid);
	if (newpassword)
	{
		if (encrypted == +1)
			appendPQExpBuffer(&sql, " ENCRYPTED");
		if (encrypted == -1)
			appendPQExpBuffer(&sql, " UNENCRYPTED");
		appendPQExpBuffer(&sql, " PASSWORD ");
		appendStringLiteral(&sql, newpassword, false);
	}
	if (createdb == +1)
		appendPQExpBuffer(&sql, " CREATEDB");
	if (createdb == -1)
		appendPQExpBuffer(&sql, " NOCREATEDB");
	if (adduser == +1)
		appendPQExpBuffer(&sql, " CREATEUSER");
	if (adduser == -1)
		appendPQExpBuffer(&sql, " NOCREATEUSER");
	appendPQExpBuffer(&sql, ";\n");

	conn = connectDatabase("template1", host, port, username, password, progname);

	if (echo)
		printf("%s", sql.data);
	result = PQexec(conn, sql.data);

	if (PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, _("%s: creation of new user failed: %s"),
				progname, PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}

	PQfinish(conn);
	if (!quiet)
	{
		puts("CREATE USER");
		fflush(stdout);
	}
	exit(0);
}


static void
help(const char *progname)
{
	printf(_("%s creates a new PostgreSQL user.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [USERNAME]\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -a, --adduser             user can add new users\n"));
	printf(_("  -A, --no-adduser          user cannot add new users\n"));
	printf(_("  -d, --createdb            user can create new databases\n"));
	printf(_("  -D, --no-createdb         user cannot create databases\n"));
	printf(_("  -P, --pwprompt            assign a password to new user\n"));
	printf(_("  -E, --encrypted           encrypt stored password\n"));
	printf(_("  -N, --unencrypted         do no encrypt stored password\n"));
	printf(_("  -i, --sysid=SYSID         select sysid for new user\n"));
	printf(_("  -e, --echo                show the commands being sent to the server\n"));
	printf(_("  -q, --quiet               don't write any messages\n"));
	printf(_("  --help                    show this help, then exit\n"));
	printf(_("  --version                 output version information, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME       database server host or socket directory\n"));
	printf(_("  -p, --port=PORT           database server port\n"));
	printf(_("  -U, --username=USERNAME   user name to connect as (not the one to create)\n"));
	printf(_("  -W, --password            prompt for password to connect\n"));
	printf(_("\nIf one of -a, -A, -d, -D, and USERNAME is not specified, you will\n"
			 "be prompted interactively.\n"));
	printf(_("\nReport bugs to <pgsql-bugs@postgresql.org>.\n"));
}
