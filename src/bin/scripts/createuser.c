/*-------------------------------------------------------------------------
 *
 * createuser
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/scripts/createuser.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"
#include "common.h"
#include "common/logging.h"
#include "common/string.h"
#include "fe_utils/option_utils.h"
#include "fe_utils/simple_list.h"
#include "fe_utils/string_utils.h"


static void help(const char *progname);

int
main(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"host", required_argument, NULL, 'h'},
		{"port", required_argument, NULL, 'p'},
		{"username", required_argument, NULL, 'U'},
		{"role", required_argument, NULL, 'g'},
		{"no-password", no_argument, NULL, 'w'},
		{"password", no_argument, NULL, 'W'},
		{"echo", no_argument, NULL, 'e'},
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
		{"replication", no_argument, NULL, 1},
		{"no-replication", no_argument, NULL, 2},
		{"interactive", no_argument, NULL, 3},
		{"connection-limit", required_argument, NULL, 'c'},
		{"pwprompt", no_argument, NULL, 'P'},
		{"encrypted", no_argument, NULL, 'E'},
		{NULL, 0, NULL, 0}
	};

	const char *progname;
	int			optindex;
	int			c;
	const char *newuser = NULL;
	char	   *host = NULL;
	char	   *port = NULL;
	char	   *username = NULL;
	SimpleStringList roles = {NULL, NULL};
	enum trivalue prompt_password = TRI_DEFAULT;
	ConnParams	cparams;
	bool		echo = false;
	bool		interactive = false;
	int			conn_limit = -2;	/* less than minimum valid value */
	bool		pwprompt = false;
	char	   *newpassword = NULL;

	/* Tri-valued variables.  */
	enum trivalue createdb = TRI_DEFAULT,
				superuser = TRI_DEFAULT,
				createrole = TRI_DEFAULT,
				inherit = TRI_DEFAULT,
				login = TRI_DEFAULT,
				replication = TRI_DEFAULT;

	PQExpBufferData sql;

	PGconn	   *conn;
	PGresult   *result;

	pg_logging_init(argv[0]);
	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pgscripts"));

	handle_help_version_opts(argc, argv, "createuser", help);

	while ((c = getopt_long(argc, argv, "h:p:U:g:wWedDsSrRiIlLc:PE",
							long_options, &optindex)) != -1)
	{
		char	   *endptr;

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
			case 'g':
				simple_string_list_append(&roles, optarg);
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
			case 'd':
				createdb = TRI_YES;
				break;
			case 'D':
				createdb = TRI_NO;
				break;
			case 's':
				superuser = TRI_YES;
				break;
			case 'S':
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
				conn_limit = strtol(optarg, &endptr, 10);
				if (*endptr != '\0' || conn_limit < -1) /* minimum valid value */
				{
					pg_log_error("invalid value for --connection-limit: %s",
								 optarg);
					exit(1);
				}
				break;
			case 'P':
				pwprompt = true;
				break;
			case 'E':
				/* no-op, accepted for backward compatibility */
				break;
			case 1:
				replication = TRI_YES;
				break;
			case 2:
				replication = TRI_NO;
				break;
			case 3:
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
			newuser = argv[optind];
			break;
		default:
			pg_log_error("too many command-line arguments (first is \"%s\")",
						 argv[optind + 1]);
			fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
			exit(1);
	}

	if (newuser == NULL)
	{
		if (interactive)
		{
			newuser = simple_prompt("Enter name of role to add: ", true);
		}
		else
		{
			if (getenv("PGUSER"))
				newuser = getenv("PGUSER");
			else
				newuser = get_user_name_or_exit(progname);
		}
	}

	if (pwprompt)
	{
		char	   *pw2;

		newpassword = simple_prompt("Enter password for new role: ", false);
		pw2 = simple_prompt("Enter it again: ", false);
		if (strcmp(newpassword, pw2) != 0)
		{
			fprintf(stderr, _("Passwords didn't match.\n"));
			exit(1);
		}
		free(pw2);
	}

	if (superuser == 0)
	{
		if (interactive && yesno_prompt("Shall the new role be a superuser?"))
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
		if (interactive && yesno_prompt("Shall the new role be allowed to create databases?"))
			createdb = TRI_YES;
		else
			createdb = TRI_NO;
	}

	if (createrole == 0)
	{
		if (interactive && yesno_prompt("Shall the new role be allowed to create more new roles?"))
			createrole = TRI_YES;
		else
			createrole = TRI_NO;
	}

	if (inherit == 0)
		inherit = TRI_YES;

	if (login == 0)
		login = TRI_YES;

	cparams.dbname = NULL;		/* this program lacks any dbname option... */
	cparams.pghost = host;
	cparams.pgport = port;
	cparams.pguser = username;
	cparams.prompt_password = prompt_password;
	cparams.override_dbname = NULL;

	conn = connectMaintenanceDatabase(&cparams, progname, echo);

	initPQExpBuffer(&sql);

	printfPQExpBuffer(&sql, "CREATE ROLE %s", fmtId(newuser));
	if (newpassword)
	{
		char	   *encrypted_password;

		appendPQExpBufferStr(&sql, " PASSWORD ");

		encrypted_password = PQencryptPasswordConn(conn,
												   newpassword,
												   newuser,
												   NULL);
		if (!encrypted_password)
		{
			pg_log_error("password encryption failed: %s",
						 PQerrorMessage(conn));
			exit(1);
		}
		appendStringLiteralConn(&sql, encrypted_password, conn);
		PQfreemem(encrypted_password);
	}
	if (superuser == TRI_YES)
		appendPQExpBufferStr(&sql, " SUPERUSER");
	if (superuser == TRI_NO)
		appendPQExpBufferStr(&sql, " NOSUPERUSER");
	if (createdb == TRI_YES)
		appendPQExpBufferStr(&sql, " CREATEDB");
	if (createdb == TRI_NO)
		appendPQExpBufferStr(&sql, " NOCREATEDB");
	if (createrole == TRI_YES)
		appendPQExpBufferStr(&sql, " CREATEROLE");
	if (createrole == TRI_NO)
		appendPQExpBufferStr(&sql, " NOCREATEROLE");
	if (inherit == TRI_YES)
		appendPQExpBufferStr(&sql, " INHERIT");
	if (inherit == TRI_NO)
		appendPQExpBufferStr(&sql, " NOINHERIT");
	if (login == TRI_YES)
		appendPQExpBufferStr(&sql, " LOGIN");
	if (login == TRI_NO)
		appendPQExpBufferStr(&sql, " NOLOGIN");
	if (replication == TRI_YES)
		appendPQExpBufferStr(&sql, " REPLICATION");
	if (replication == TRI_NO)
		appendPQExpBufferStr(&sql, " NOREPLICATION");
	if (conn_limit >= -1)
		appendPQExpBuffer(&sql, " CONNECTION LIMIT %d", conn_limit);
	if (roles.head != NULL)
	{
		SimpleStringListCell *cell;

		appendPQExpBufferStr(&sql, " IN ROLE ");

		for (cell = roles.head; cell; cell = cell->next)
		{
			if (cell->next)
				appendPQExpBuffer(&sql, "%s,", fmtId(cell->val));
			else
				appendPQExpBufferStr(&sql, fmtId(cell->val));
		}
	}
	appendPQExpBufferChar(&sql, ';');

	if (echo)
		printf("%s\n", sql.data);
	result = PQexec(conn, sql.data);

	if (PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		pg_log_error("creation of new role failed: %s", PQerrorMessage(conn));
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
	printf(_("  -c, --connection-limit=N  connection limit for role (default: no limit)\n"));
	printf(_("  -d, --createdb            role can create new databases\n"));
	printf(_("  -D, --no-createdb         role cannot create databases (default)\n"));
	printf(_("  -e, --echo                show the commands being sent to the server\n"));
	printf(_("  -g, --role=ROLE           new role will be a member of this role\n"));
	printf(_("  -i, --inherit             role inherits privileges of roles it is a\n"
			 "                            member of (default)\n"));
	printf(_("  -I, --no-inherit          role does not inherit privileges\n"));
	printf(_("  -l, --login               role can login (default)\n"));
	printf(_("  -L, --no-login            role cannot login\n"));
	printf(_("  -P, --pwprompt            assign a password to new role\n"));
	printf(_("  -r, --createrole          role can create new roles\n"));
	printf(_("  -R, --no-createrole       role cannot create roles (default)\n"));
	printf(_("  -s, --superuser           role will be superuser\n"));
	printf(_("  -S, --no-superuser        role will not be superuser (default)\n"));
	printf(_("  -V, --version             output version information, then exit\n"));
	printf(_("  --interactive             prompt for missing role name and attributes rather\n"
			 "                            than using defaults\n"));
	printf(_("  --replication             role can initiate replication\n"));
	printf(_("  --no-replication          role cannot initiate replication\n"));
	printf(_("  -?, --help                show this help, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME       database server host or socket directory\n"));
	printf(_("  -p, --port=PORT           database server port\n"));
	printf(_("  -U, --username=USERNAME   user name to connect as (not the one to create)\n"));
	printf(_("  -w, --no-password         never prompt for password\n"));
	printf(_("  -W, --password            force password prompt\n"));
	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}
