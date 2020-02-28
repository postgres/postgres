/*-------------------------------------------------------------------------
 *
 * pg_isready --- checks the status of the PostgreSQL server
 *
 * Copyright (c) 2013-2020, PostgreSQL Global Development Group
 *
 * src/bin/scripts/pg_isready.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"
#include "common.h"
#include "common/logging.h"

#define DEFAULT_CONNECT_TIMEOUT "3"

static void
			help(const char *progname);

int
main(int argc, char **argv)
{
	int			c;

	const char *progname;

	const char *pghost = NULL;
	const char *pgport = NULL;
	const char *pguser = NULL;
	const char *pgdbname = NULL;
	const char *connect_timeout = DEFAULT_CONNECT_TIMEOUT;

	const char *pghost_str = NULL;
	const char *pghostaddr_str = NULL;
	const char *pgport_str = NULL;

#define PARAMS_ARRAY_SIZE	7

	const char *keywords[PARAMS_ARRAY_SIZE];
	const char *values[PARAMS_ARRAY_SIZE];

	bool		quiet = false;

	PGPing		rv;
	PQconninfoOption *opts = NULL;
	PQconninfoOption *defs = NULL;
	PQconninfoOption *opt;
	PQconninfoOption *def;
	char	   *errmsg = NULL;

	/*
	 * We accept user and database as options to avoid useless errors from
	 * connecting with invalid params
	 */

	static struct option long_options[] = {
		{"dbname", required_argument, NULL, 'd'},
		{"host", required_argument, NULL, 'h'},
		{"port", required_argument, NULL, 'p'},
		{"quiet", no_argument, NULL, 'q'},
		{"timeout", required_argument, NULL, 't'},
		{"username", required_argument, NULL, 'U'},
		{NULL, 0, NULL, 0}
	};

	pg_logging_init(argv[0]);
	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pgscripts"));
	handle_help_version_opts(argc, argv, progname, help);

	while ((c = getopt_long(argc, argv, "d:h:p:qt:U:", long_options, NULL)) != -1)
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
			case 't':
				connect_timeout = pg_strdup(optarg);
				break;
			case 'U':
				pguser = pg_strdup(optarg);
				break;
			default:
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);

				/*
				 * We need to make sure we don't return 1 here because someone
				 * checking the return code might infer unintended meaning
				 */
				exit(PQPING_NO_ATTEMPT);
		}
	}

	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);

		/*
		 * We need to make sure we don't return 1 here because someone
		 * checking the return code might infer unintended meaning
		 */
		exit(PQPING_NO_ATTEMPT);
	}

	keywords[0] = "host";
	values[0] = pghost;
	keywords[1] = "port";
	values[1] = pgport;
	keywords[2] = "user";
	values[2] = pguser;
	keywords[3] = "dbname";
	values[3] = pgdbname;
	keywords[4] = "connect_timeout";
	values[4] = connect_timeout;
	keywords[5] = "fallback_application_name";
	values[5] = progname;
	keywords[6] = NULL;
	values[6] = NULL;

	/*
	 * Get the host and port so we can display them in our output
	 */
	if (pgdbname &&
		(strncmp(pgdbname, "postgresql://", 13) == 0 ||
		 strncmp(pgdbname, "postgres://", 11) == 0 ||
		 strchr(pgdbname, '=') != NULL))
	{
		opts = PQconninfoParse(pgdbname, &errmsg);
		if (opts == NULL)
		{
			pg_log_error("%s", errmsg);
			exit(PQPING_NO_ATTEMPT);
		}
	}

	defs = PQconndefaults();
	if (defs == NULL)
	{
		pg_log_error("could not fetch default options");
		exit(PQPING_NO_ATTEMPT);
	}

	for (opt = opts, def = defs; def->keyword; def++)
	{
		if (strcmp(def->keyword, "host") == 0)
		{
			if (opt && opt->val)
				pghost_str = opt->val;
			else if (pghost)
				pghost_str = pghost;
			else if (def->val)
				pghost_str = def->val;
			else
				pghost_str = DEFAULT_PGSOCKET_DIR;
		}
		else if (strcmp(def->keyword, "hostaddr") == 0)
		{
			if (opt && opt->val)
				pghostaddr_str = opt->val;
			else if (def->val)
				pghostaddr_str = def->val;
		}
		else if (strcmp(def->keyword, "port") == 0)
		{
			if (opt && opt->val)
				pgport_str = opt->val;
			else if (pgport)
				pgport_str = pgport;
			else if (def->val)
				pgport_str = def->val;
		}

		if (opt)
			opt++;
	}

	rv = PQpingParams(keywords, values, 1);

	if (!quiet)
	{
		printf("%s:%s - ",
			   pghostaddr_str != NULL ? pghostaddr_str : pghost_str,
			   pgport_str);

		switch (rv)
		{
			case PQPING_OK:
				printf(_("accepting connections\n"));
				break;
			case PQPING_REJECT:
				printf(_("rejecting connections\n"));
				break;
			case PQPING_NO_RESPONSE:
				printf(_("no response\n"));
				break;
			case PQPING_NO_ATTEMPT:
				printf(_("no attempt\n"));
				break;
			default:
				printf(_("unknown\n"));
		}
	}

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
	printf(_("  -t, --timeout=SECS       seconds to wait when attempting connection, 0 disables (default: %s)\n"), DEFAULT_CONNECT_TIMEOUT);
	printf(_("  -U, --username=USERNAME  user name to connect as\n"));
	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}
