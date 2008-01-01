/*-------------------------------------------------------------------------
 *
 * createuser
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/bin/scripts/createuser.c,v 1.38 2008/01/01 19:45:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"
#include "common.h"
#include "dumputils.h"


static void help(const char *progname);

enum trivalue
{
	TRI_DEFAULT,
	TRI_NO,
	TRI_YES
};

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
	char	   *conn_limit = NULL;
	bool		pwprompt = false;
	char	   *newpassword = NULL;

	/* Tri-valued variables.  */
	enum trivalue createdb = TRI_DEFAULT,
				superuser = TRI_DEFAULT,
				createrole = TRI_DEFAULT,
				inherit = TRI_DEFAULT,
				login = TRI_DEFAULT,
				encrypted = TRI_DEFAULT;

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
				/* obsolete; remove in 8.4 */
				break;
			case 'd':
				createdb = TRI_YES;
				break;
			case 'D':
				createdb = TRI_NO;
				break;
			case 's':
			case 'a':
				superuser = TRI_YES;
				break;
			case 'S':
			case 'A':
				superuser = TRI_NO;
				break;
			case 'r':
				createrole = TRI_YES;
				break;
			case 'R':
				createrole = TRI_NO;
				break;
			case 'i':
				inherit = TRI_YES;
				break;
			case 'I':
				inherit = TRI_NO;
				break;
			case 'l':
				login = TRI_YES;
				break;
			case 'L':
				login = TRI_NO;
				break;
			case 'c':
				conn_limit = optarg;
				break;
			case 'P':
				pwprompt = true;
				break;
			case 'E':
				encrypted = TRI_YES;
				break;
			case 'N':
				encrypted = TRI_NO;
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
		if (yesno_prompt("Shall the new role be a superuser?"))
			superuser = TRI_YES;
		else
			superuser = TRI_NO;
	}

	if (superuser == TRI_YES)
	{
		/* Not much point in trying to restrict a superuser */
		createdb = TRI_YES;
		createrole = TRI_YES;
	}

	if (createdb == 0)
	{
		if (yesno_prompt("Shall the new role be allowed to create databases?"))
			createdb = TRI_YES;
		else
			createdb = TRI_NO;
	}

	if (createrole == 0)
	{
		if (yesno_prompt("Shall the new role be allowed to create more new roles?"))
			createrole = TRI_YES;
		else
			createrole = TRI_NO;
	}

	if (inherit == 0)
		inherit = TRI_YES;

	if (login == 0)
		login = TRI_YES;

	conn = connectDatabase("postgres", host, port, username, password, progname);

	initPQExpBuffer(&sql);

	printfPQExpBuffer(&sql, "CREATE ROLE %s", fmtId(newuser));
	if (newpassword)
	{
		if (encrypted == TRI_YES)
			appendPQExpBuffer(&sql, " ENCRYPTED");
		if (encrypted == TRI_NO)
			appendPQExpBuffer(&sql, " UNENCRYPTED");
		appendPQExpBuffer(&sql, " PASSWORD ");

		if (encrypted != TRI_NO)
		{
			char	   *encrypted_password;

			encrypted_password = PQencryptPassword(newpassword,
												   newuser);
			if (!encrypted_password)
			{
				fprintf(stderr, _("Password encryption failed.\n"));
				exit(1);
			}
			appendStringLiteralConn(&sql, encrypted_password, conn);
			PQfreemem(encrypted_password);
		}
		else
			appendStringLiteralConn(&sql, newpassword, conn);
	}
	if (superuser == TRI_YES)
		appendPQExpBuffer(&sql, " SUPERUSER");
	if (superuser == TRI_NO)
		appendPQExpBuffer(&sql, " NOSUPERUSER");
	if (createdb == TRI_YES)
		appendPQExpBuffer(&sql, " CREATEDB");
	if (createdb == TRI_NO)
		appendPQExpBuffer(&sql, " NOCREATEDB");
	if (createrole == TRI_YES)
		appendPQExpBuffer(&sql, " CREATEROLE");
	if (createrole == TRI_NO)
		appendPQExpBuffer(&sql, " NOCREATEROLE");
	if (inherit == TRI_YES)
		appendPQExpBuffer(&sql, " INHERIT");
	if (inherit == TRI_NO)
		appendPQExpBuffer(&sql, " NOINHERIT");
	if (login == TRI_YES)
		appendPQExpBuffer(&sql, " LOGIN");
	if (login == TRI_NO)
		appendPQExpBuffer(&sql, " NOLOGIN");
	if (conn_limit != NULL)
		appendPQExpBuffer(&sql, " CONNECTION LIMIT %s", conn_limit);
	appendPQExpBuffer(&sql, ";\n");

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

	PQclear(result);
	PQfinish(conn);
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
	printf(_("  --help                    show this help, then exit\n"));
	printf(_("  --version                 output version information, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME       database server host or socket directory\n"));
	printf(_("  -p, --port=PORT           database server port\n"));
	printf(_("  -U, --username=USERNAME   user name to connect as (not the one to create)\n"));
	printf(_("  -W, --password            force password prompt\n"));
	printf(_("\nIf one of -s, -S, -d, -D, -r, -R and ROLENAME is not specified, you will\n"
			 "be prompted interactively.\n"));
	printf(_("\nReport bugs to <pgsql-bugs@postgresql.org>.\n"));
}
