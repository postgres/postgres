/*-------------------------------------------------------------------------
 *
 * createuser
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/bin/scripts/createuser.c,v 1.21 2005/10/15 02:49:41 momjian Exp $
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
		{"superuser", no_argument, NULL, 's'},
		{"no-superuser", no_argument, NULL, 'S'},
		{"createrole", no_argument, NULL, 'r'},
		{"no-createrole", no_argument, NULL, 'R'},
		{"inherit", no_argument, NULL, 'i'},
		{"no-inherit", no_argument, NULL, 'I'},
		{"login", no_argument, NULL, 'l'},
		{"no-login", no_argument, NULL, 'L'},
		/* adduser is obsolete, undocumented spelling of superuser */
		{"adduser", no_argument, NULL, 'a'},
		{"no-adduser", no_argument, NULL, 'A'},
		{"connection-limit", required_argument, NULL, 'c'},
		{"pwprompt", no_argument, NULL, 'P'},
		{"encrypted", no_argument, NULL, 'E'},
		{"unencrypted", no_argument, NULL, 'N'},
		{NULL, 0, NULL, 0}
	};

	const char *progname;
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
	int			superuser = 0;
	int			createrole = 0;
	int			inherit = 0;
	int			login = 0;
	char	   *conn_limit = NULL;
	bool		pwprompt = false;
	int			encrypted = 0;	/* 0 uses server default */
	char	   *newpassword = NULL;

	PQExpBufferData sql;

	PGconn	   *conn;
	PGresult   *result;

	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], "pgscripts");

	handle_help_version_opts(argc, argv, "createuser", help);

	while ((c = getopt_long(argc, argv, "h:p:U:WeqdDsSaArRiIlLc:PEN",
							long_options, &optindex)) != -1)
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
				createdb = +1;
				break;
			case 'D':
				createdb = -1;
				break;
			case 's':
			case 'a':
				superuser = +1;
				break;
			case 'S':
			case 'A':
				superuser = -1;
				break;
			case 'r':
				createrole = +1;
				break;
			case 'R':
				createrole = -1;
				break;
			case 'i':
				inherit = +1;
				break;
			case 'I':
				inherit = -1;
				break;
			case 'l':
				login = +1;
				break;
			case 'L':
				login = -1;
				break;
			case 'c':
				conn_limit = optarg;
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

	if (newuser == NULL)
		newuser = simple_prompt("Enter name of role to add: ", 128, true);

	if (pwprompt)
	{
		char	   *pw1,
				   *pw2;

		pw1 = simple_prompt("Enter password for new role: ", 100, false);
		pw2 = simple_prompt("Enter it again: ", 100, false);
		if (strcmp(pw1, pw2) != 0)
		{
			fprintf(stderr, _("Passwords didn't match.\n"));
			exit(1);
		}
		newpassword = pw1;
		free(pw2);
	}

	if (superuser == 0)
	{
		char	   *reply;

		reply = simple_prompt("Shall the new role be a superuser? (y/n) ", 1, true);
		if (check_yesno_response(reply) == 1)
			superuser = +1;
		else
			superuser = -1;
	}

	if (superuser == +1)
	{
		/* Not much point in trying to restrict a superuser */
		createdb = +1;
		createrole = +1;
	}

	if (createdb == 0)
	{
		char	   *reply;

		reply = simple_prompt("Shall the new role be allowed to create databases? (y/n) ", 1, true);
		if (check_yesno_response(reply) == 1)
			createdb = +1;
		else
			createdb = -1;
	}

	if (createrole == 0)
	{
		char	   *reply;

		reply = simple_prompt("Shall the new role be allowed to create more new roles? (y/n) ", 1, true);
		if (check_yesno_response(reply) == 1)
			createrole = +1;
		else
			createrole = -1;
	}

	if (inherit == 0)
	{
		/* silently default to YES */
		inherit = +1;
	}

	if (login == 0)
	{
		/* silently default to YES */
		login = +1;
	}

	initPQExpBuffer(&sql);

	printfPQExpBuffer(&sql, "CREATE ROLE %s", fmtId(newuser));
	if (newpassword)
	{
		if (encrypted == +1)
			appendPQExpBuffer(&sql, " ENCRYPTED");
		if (encrypted == -1)
			appendPQExpBuffer(&sql, " UNENCRYPTED");
		appendPQExpBuffer(&sql, " PASSWORD ");
		appendStringLiteral(&sql, newpassword, false);
	}
	if (superuser == +1)
		appendPQExpBuffer(&sql, " SUPERUSER");
	if (superuser == -1)
		appendPQExpBuffer(&sql, " NOSUPERUSER");
	if (createdb == +1)
		appendPQExpBuffer(&sql, " CREATEDB");
	if (createdb == -1)
		appendPQExpBuffer(&sql, " NOCREATEDB");
	if (createrole == +1)
		appendPQExpBuffer(&sql, " CREATEROLE");
	if (createrole == -1)
		appendPQExpBuffer(&sql, " NOCREATEROLE");
	if (inherit == +1)
		appendPQExpBuffer(&sql, " INHERIT");
	if (inherit == -1)
		appendPQExpBuffer(&sql, " NOINHERIT");
	if (login == +1)
		appendPQExpBuffer(&sql, " LOGIN");
	if (login == -1)
		appendPQExpBuffer(&sql, " NOLOGIN");
	if (conn_limit != NULL)
		appendPQExpBuffer(&sql, " CONNECTION LIMIT %s", conn_limit);
	appendPQExpBuffer(&sql, ";\n");

	conn = connectDatabase("postgres", host, port, username, password, progname);

	if (echo)
		printf("%s", sql.data);
	result = PQexec(conn, sql.data);

	if (PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, _("%s: creation of new role failed: %s"),
				progname, PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}

	PQfinish(conn);
	if (!quiet)
	{
		puts("CREATE ROLE");
		fflush(stdout);
	}
	exit(0);
}


static void
help(const char *progname)
{
	printf(_("%s creates a new PostgreSQL role.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [ROLENAME]\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -s, --superuser           role will be superuser\n"));
	printf(_("  -S, --no-superuser        role will not be superuser\n"));
	printf(_("  -d, --createdb            role can create new databases\n"));
	printf(_("  -D, --no-createdb         role cannot create databases\n"));
	printf(_("  -r, --createrole          role can create new roles\n"));
	printf(_("  -R, --no-createrole       role cannot create roles\n"));
	printf(_("  -l, --login               role can login (default)\n"));
	printf(_("  -L, --no-login            role cannot login\n"));
	printf(_("  -i, --inherit             role inherits privileges of roles it is a\n"
			 "                            member of (default)\n"));
	printf(_("  -I, --no-inherit          role does not inherit privileges\n"));
	printf(_("  -c, --connection-limit=N  connection limit for role (default: no limit)\n"));
	printf(_("  -P, --pwprompt            assign a password to new role\n"));
	printf(_("  -E, --encrypted           encrypt stored password\n"));
	printf(_("  -N, --unencrypted         do not encrypt stored password\n"));
	printf(_("  -e, --echo                show the commands being sent to the server\n"));
	printf(_("  -q, --quiet               don't write any messages\n"));
	printf(_("  --help                    show this help, then exit\n"));
	printf(_("  --version                 output version information, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME       database server host or socket directory\n"));
	printf(_("  -p, --port=PORT           database server port\n"));
	printf(_("  -U, --username=USERNAME   user name to connect as (not the one to create)\n"));
	printf(_("  -W, --password            prompt for password to connect\n"));
	printf(_("\nIf one of -s, -S, -d, -D, -r, -R and ROLENAME is not specified, you will\n"
			 "be prompted interactively.\n"));
	printf(_("\nReport bugs to <pgsql-bugs@postgresql.org>.\n"));
}
