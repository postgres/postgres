/*-------------------------------------------------------------------------
 *
 * pg_isready --- checks the status of the PostgreSQL server
 *
 * Copyright (c) 2013, PostgreSQL Global Development Group
 *
 * src/bin/scripts/pg_isready.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"
#include "common.h"

static void
help(const char *progname);

int
main(int argc, char **argv)
{
	int c,optindex,opt_index = 0;

	const char *progname;

	const char *pghost = NULL;
	const char *pgport = NULL;
	const char *pguser = NULL;
	const char *pgdbname = NULL;

	const char *keywords[4], *values[4];

	bool quiet = false;

	PGPing rv;
	PQconninfoOption *connect_options, *conn_opt_ptr;

	/*
	 * We accept user and database as options to avoid
	 * useless errors from connecting with invalid params
	 */

	static struct option long_options[] = {
			{"dbname", required_argument, NULL, 'd'},
			{"host", required_argument, NULL, 'h'},
			{"port", required_argument, NULL, 'p'},
			{"quiet", no_argument, NULL, 'q'},
			{"username", required_argument, NULL, 'U'},
			{NULL, 0, NULL, 0}
		};

	progname = get_progname(argv[0]);
	handle_help_version_opts(argc, argv, progname, help);

	while ((c = getopt_long(argc, argv, "d:h:p:qU:V", long_options, &optindex)) != -1)
	{
		switch (c)
		{
			case 'd':
				pgdbname = pg_strdup(optarg);
				break;
			case 'h':
				pghost = pg_strdup(optarg);
				break;
			case 'p':
				pgport = pg_strdup(optarg);
				break;
			case 'q':
				quiet = true;
				break;
			case 'U':
				pguser = pg_strdup(optarg);
				break;
			default:
				/*
				 * We need to make sure we don't return 1 here because someone
				 * checking the return code might infer unintended meaning
				 */
				exit(PQPING_NO_ATTEMPT);
		}
	}

	if (optind < argc)
	{
		fprintf(stderr, _("%s: too many command-line arguments (first is \"%s\")\n"),
				progname, argv[optind]);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
		/*
		 * We need to make sure we don't return 1 here because someone
		 * checking the return code might infer unintended meaning
		 */
		exit(PQPING_NO_ATTEMPT);
	}

	/*
	 * Get the default options so we can display them in our output
	 */

	connect_options = PQconndefaults();
	conn_opt_ptr = connect_options;

	while (conn_opt_ptr->keyword)
	{
		if (strncmp(conn_opt_ptr->keyword, "host", 5) == 0)
		{
			if (pghost)
			{
				keywords[opt_index] = conn_opt_ptr->keyword;
				values[opt_index] = pghost;
				opt_index++;
			}
			else if (conn_opt_ptr->val)
				pghost = conn_opt_ptr->val;
			else
				pghost = DEFAULT_PGSOCKET_DIR;
		}
		else if (strncmp(conn_opt_ptr->keyword, "port", 5) == 0)
		{
			if (pgport)
			{
				keywords[opt_index] = conn_opt_ptr->keyword;
				values[opt_index] = pgport;
				opt_index++;
			}
			else if (conn_opt_ptr->val)
				pgport = conn_opt_ptr->val;
		}
		else if (strncmp(conn_opt_ptr->keyword, "user", 5) == 0)
		{
			if (pguser)
			{
				keywords[opt_index] = conn_opt_ptr->keyword;
				values[opt_index] = pguser;
				opt_index++;
			}
			else if (conn_opt_ptr->val)
				pguser = conn_opt_ptr->val;
		}
		else if (strncmp(conn_opt_ptr->keyword, "dbname", 7) == 0)
		{
			if (pgdbname)
			{
				keywords[opt_index] = conn_opt_ptr->keyword;
				values[opt_index] = pgdbname;
				opt_index++;
			}
			else if (conn_opt_ptr->val)
				pgdbname = conn_opt_ptr->val;
		}
		conn_opt_ptr++;
	}

	keywords[opt_index] = NULL;
	values[opt_index] = NULL;

	rv = PQpingParams(keywords, values, 1);

	if (!quiet)
	{
		printf("%s:%s - ", pghost, pgport);

		switch (rv)
		{
			case PQPING_OK:
				printf("accepting connections\n");
				break;
			case PQPING_REJECT:
				printf("rejecting connections\n");
				break;
			case PQPING_NO_RESPONSE:
				printf("no response\n");
				break;
			case PQPING_NO_ATTEMPT:
				printf("no attempt\n");
				break;
			default:
				printf("unknown\n");
		}
	}

	PQconninfoFree(connect_options);

	exit(rv);
}

static void
help(const char *progname)
{
	printf(_("%s issues a connection check to a PostgreSQL database.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]...\n"), progname);

	printf(_("\nOptions:\n"));
	printf(_("  -d, --dbname=DBNAME      database name\n"));
	printf(_("  -q, --quiet              run quietly\n"));
	printf(_("  -V, --version            output version information, then exit\n"));
	printf(_("  -?, --help               show this help, then exit\n"));

	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME      database server host or socket directory\n"));
	printf(_("  -p, --port=PORT          database server port\n"));
	printf(_("  -U, --username=USERNAME  database username\n"));
}
