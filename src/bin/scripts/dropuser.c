/*-------------------------------------------------------------------------
 *
 * dropuser
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/scripts/dropuser.c
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
	static int	if_exists = 0;

	static struct option long_options[] = {
		{"host", required_argument, NULL, 'h'},
		{"port", required_argument, NULL, 'p'},
		{"username", required_argument, NULL, 'U'},
		{"no-password", no_argument, NULL, 'w'},
		{"password", no_argument, NULL, 'W'},
		{"echo", no_argument, NULL, 'e'},
		{"interactive", no_argument, NULL, 'i'},
		{"if-exists", no_argument, &if_exists, 1},
		{NULL, 0, NULL, 0}
	};

	const char *progname;
	int			optindex;
	int			c;

	char	   *dropuser = NULL;
	char	   *host = NULL;
	char	   *port = NULL;
	char	   *username = NULL;
	enum trivalue prompt_password = TRI_DEFAULT;
	bool		echo = false;
	bool		interactive = false;

	PQExpBufferData sql;

	PGconn	   *conn;
	PGresult   *result;

	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pgscripts"));

	handle_help_version_opts(argc, argv, "dropuser", help);

	while ((c = getopt_long(argc, argv, "h:p:U:wWei", long_options, &optindex)) != -1)
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
			case 'i':
				interactive = true;
				break;
			case 0:
				/* this covers the long options */
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
	{
		if (interactive)
			dropuser = simple_prompt("Enter name of role to drop: ", 128, true);
		else
		{
			fprintf(stderr, _("%s: missing required argument role name\n"), progname);
			fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
			exit(1);
		}
	}

	if (interactive)
	{
		printf(_("Role \"%s\" will be permanently removed.\n"), dropuser);
		if (!yesno_prompt("Are you sure?"))
			exit(0);
	}

	initPQExpBuffer(&sql);
	appendPQExpBuffer(&sql, "DROP ROLE %s%s;",
					  (if_exists ? "IF EXISTS " : ""), fmtId(dropuser));

	conn = connectDatabase("postgres", host, port, username, prompt_password,
						   progname, false, false);

	if (echo)
		printf("%s\n", sql.data);
	result = PQexec(conn, sql.data);

	if (PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, _("%s: removal of role \"%s\" failed: %s"),
				progname, dropuser, PQerrorMessage(conn));
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
	printf(_("%s removes a PostgreSQL role.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [ROLENAME]\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -e, --echo                show the commands being sent to the server\n"));
	printf(_("  -i, --interactive         prompt before deleting anything, and prompt for\n"
			 "                            role name if not specified\n"));
	printf(_("  -V, --version             output version information, then exit\n"));
	printf(_("  --if-exists               don't report error if user doesn't exist\n"));
	printf(_("  -?, --help                show this help, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME       database server host or socket directory\n"));
	printf(_("  -p, --port=PORT           database server port\n"));
	printf(_("  -U, --username=USERNAME   user name to connect as (not the one to drop)\n"));
	printf(_("  -w, --no-password         never prompt for password\n"));
	printf(_("  -W, --password            force password prompt\n"));
	printf(_("\nReport bugs to <pgsql-bugs@postgresql.org>.\n"));
}
