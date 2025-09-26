/*-------------------------------------------------------------------------
 *
 * vacuumdb
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/scripts/vacuumdb.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <limits.h>

#include "common.h"
#include "common/logging.h"
#include "fe_utils/option_utils.h"
#include "vacuuming.h"

static void help(const char *progname);
static void check_objfilter(bits32 objfilter);


int
main(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"host", required_argument, NULL, 'h'},
		{"port", required_argument, NULL, 'p'},
		{"username", required_argument, NULL, 'U'},
		{"no-password", no_argument, NULL, 'w'},
		{"password", no_argument, NULL, 'W'},
		{"echo", no_argument, NULL, 'e'},
		{"quiet", no_argument, NULL, 'q'},
		{"dbname", required_argument, NULL, 'd'},
		{"analyze", no_argument, NULL, 'z'},
		{"analyze-only", no_argument, NULL, 'Z'},
		{"freeze", no_argument, NULL, 'F'},
		{"all", no_argument, NULL, 'a'},
		{"table", required_argument, NULL, 't'},
		{"full", no_argument, NULL, 'f'},
		{"verbose", no_argument, NULL, 'v'},
		{"jobs", required_argument, NULL, 'j'},
		{"parallel", required_argument, NULL, 'P'},
		{"schema", required_argument, NULL, 'n'},
		{"exclude-schema", required_argument, NULL, 'N'},
		{"maintenance-db", required_argument, NULL, 2},
		{"analyze-in-stages", no_argument, NULL, 3},
		{"disable-page-skipping", no_argument, NULL, 4},
		{"skip-locked", no_argument, NULL, 5},
		{"min-xid-age", required_argument, NULL, 6},
		{"min-mxid-age", required_argument, NULL, 7},
		{"no-index-cleanup", no_argument, NULL, 8},
		{"force-index-cleanup", no_argument, NULL, 9},
		{"no-truncate", no_argument, NULL, 10},
		{"no-process-toast", no_argument, NULL, 11},
		{"no-process-main", no_argument, NULL, 12},
		{"buffer-usage-limit", required_argument, NULL, 13},
		{"missing-stats-only", no_argument, NULL, 14},
		{NULL, 0, NULL, 0}
	};

	const char *progname;
	int			optindex;
	int			c;
	const char *dbname = NULL;
	const char *maintenance_db = NULL;
	ConnParams	cparams;
	bool		echo = false;
	bool		quiet = false;
	vacuumingOptions vacopts;
	SimpleStringList objects = {NULL, NULL};
	int			concurrentCons = 1;
	unsigned int tbl_count = 0;
	int			ret;

	/* initialize options */
	memset(&vacopts, 0, sizeof(vacopts));
	vacopts.objfilter = 0;		/* no filter */
	vacopts.parallel_workers = -1;
	vacopts.buffer_usage_limit = NULL;
	vacopts.no_index_cleanup = false;
	vacopts.force_index_cleanup = false;
	vacopts.do_truncate = true;
	vacopts.process_main = true;
	vacopts.process_toast = true;

	/* the same for connection parameters */
	memset(&cparams, 0, sizeof(cparams));
	cparams.prompt_password = TRI_DEFAULT;

	pg_logging_init(argv[0]);
	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pgscripts"));

	handle_help_version_opts(argc, argv, "vacuumdb", help);

	while ((c = getopt_long(argc, argv, "ad:efFh:j:n:N:p:P:qt:U:vwWzZ",
							long_options, &optindex)) != -1)
	{
		switch (c)
		{
			case 'a':
				vacopts.objfilter |= OBJFILTER_ALL_DBS;
				break;
			case 'd':
				vacopts.objfilter |= OBJFILTER_DATABASE;
				dbname = pg_strdup(optarg);
				break;
			case 'e':
				echo = true;
				break;
			case 'f':
				vacopts.full = true;
				break;
			case 'F':
				vacopts.freeze = true;
				break;
			case 'h':
				cparams.pghost = pg_strdup(optarg);
				break;
			case 'j':
				if (!option_parse_int(optarg, "-j/--jobs", 1, INT_MAX,
									  &concurrentCons))
					exit(1);
				break;
			case 'n':
				vacopts.objfilter |= OBJFILTER_SCHEMA;
				simple_string_list_append(&objects, optarg);
				break;
			case 'N':
				vacopts.objfilter |= OBJFILTER_SCHEMA_EXCLUDE;
				simple_string_list_append(&objects, optarg);
				break;
			case 'p':
				cparams.pgport = pg_strdup(optarg);
				break;
			case 'P':
				if (!option_parse_int(optarg, "-P/--parallel", 0, INT_MAX,
									  &vacopts.parallel_workers))
					exit(1);
				break;
			case 'q':
				quiet = true;
				break;
			case 't':
				vacopts.objfilter |= OBJFILTER_TABLE;
				simple_string_list_append(&objects, optarg);
				tbl_count++;
				break;
			case 'U':
				cparams.pguser = pg_strdup(optarg);
				break;
			case 'v':
				vacopts.verbose = true;
				break;
			case 'w':
				cparams.prompt_password = TRI_NO;
				break;
			case 'W':
				cparams.prompt_password = TRI_YES;
				break;
			case 'z':
				vacopts.and_analyze = true;
				break;
			case 'Z':
				/* if analyze-in-stages is given, don't override it */
				if (vacopts.mode != MODE_ANALYZE_IN_STAGES)
					vacopts.mode = MODE_ANALYZE;
				break;
			case 2:
				maintenance_db = pg_strdup(optarg);
				break;
			case 3:
				vacopts.mode = MODE_ANALYZE_IN_STAGES;
				break;
			case 4:
				vacopts.disable_page_skipping = true;
				break;
			case 5:
				vacopts.skip_locked = true;
				break;
			case 6:
				if (!option_parse_int(optarg, "--min-xid-age", 1, INT_MAX,
									  &vacopts.min_xid_age))
					exit(1);
				break;
			case 7:
				if (!option_parse_int(optarg, "--min-mxid-age", 1, INT_MAX,
									  &vacopts.min_mxid_age))
					exit(1);
				break;
			case 8:
				vacopts.no_index_cleanup = true;
				break;
			case 9:
				vacopts.force_index_cleanup = true;
				break;
			case 10:
				vacopts.do_truncate = false;
				break;
			case 11:
				vacopts.process_toast = false;
				break;
			case 12:
				vacopts.process_main = false;
				break;
			case 13:
				vacopts.buffer_usage_limit = escape_quotes(optarg);
				break;
			case 14:
				vacopts.missing_stats_only = true;
				break;
			default:
				/* getopt_long already emitted a complaint */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit(1);
		}
	}

	/*
	 * Non-option argument specifies database name as long as it wasn't
	 * already specified with -d / --dbname
	 */
	if (optind < argc && dbname == NULL)
	{
		vacopts.objfilter |= OBJFILTER_DATABASE;
		dbname = argv[optind];
		optind++;
	}

	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/*
	 * Validate the combination of filters specified in the command-line
	 * options.
	 */
	check_objfilter(vacopts.objfilter);

	if (vacopts.mode == MODE_ANALYZE ||
		vacopts.mode == MODE_ANALYZE_IN_STAGES)
	{
		if (vacopts.full)
			pg_fatal("cannot use the \"%s\" option when performing only analyze",
					 "full");
		if (vacopts.freeze)
			pg_fatal("cannot use the \"%s\" option when performing only analyze",
					 "freeze");
		if (vacopts.disable_page_skipping)
			pg_fatal("cannot use the \"%s\" option when performing only analyze",
					 "disable-page-skipping");
		if (vacopts.no_index_cleanup)
			pg_fatal("cannot use the \"%s\" option when performing only analyze",
					 "no-index-cleanup");
		if (vacopts.force_index_cleanup)
			pg_fatal("cannot use the \"%s\" option when performing only analyze",
					 "force-index-cleanup");
		if (!vacopts.do_truncate)
			pg_fatal("cannot use the \"%s\" option when performing only analyze",
					 "no-truncate");
		if (!vacopts.process_main)
			pg_fatal("cannot use the \"%s\" option when performing only analyze",
					 "no-process-main");
		if (!vacopts.process_toast)
			pg_fatal("cannot use the \"%s\" option when performing only analyze",
					 "no-process-toast");
		/* allow 'and_analyze' with 'analyze_only' */
	}

	/* Prohibit full and analyze_only options with parallel option */
	if (vacopts.parallel_workers >= 0)
	{
		if (vacopts.mode == MODE_ANALYZE ||
			vacopts.mode == MODE_ANALYZE_IN_STAGES)
			pg_fatal("cannot use the \"%s\" option when performing only analyze",
					 "parallel");
		if (vacopts.full)
			pg_fatal("cannot use the \"%s\" option when performing full vacuum",
					 "parallel");
	}

	/* Prohibit --no-index-cleanup and --force-index-cleanup together */
	if (vacopts.no_index_cleanup && vacopts.force_index_cleanup)
		pg_fatal("cannot use the \"%s\" option with the \"%s\" option",
				 "no-index-cleanup", "force-index-cleanup");

	/*
	 * buffer-usage-limit is not allowed with VACUUM FULL unless ANALYZE is
	 * included too.
	 */
	if (vacopts.buffer_usage_limit && vacopts.full && !vacopts.and_analyze)
		pg_fatal("cannot use the \"%s\" option with the \"%s\" option",
				 "buffer-usage-limit", "full");

	/*
	 * Prohibit --missing-stats-only without --analyze-only or
	 * --analyze-in-stages.
	 */
	if (vacopts.missing_stats_only && (vacopts.mode != MODE_ANALYZE &&
									   vacopts.mode != MODE_ANALYZE_IN_STAGES))
		pg_fatal("cannot use the \"%s\" option without \"%s\" or \"%s\"",
				 "missing-stats-only", "analyze-only", "analyze-in-stages");

	ret = vacuuming_main(&cparams, dbname, maintenance_db, &vacopts,
						 &objects, tbl_count,
						 concurrentCons,
						 progname, echo, quiet);
	exit(ret);
}

/*
 * Verify that the filters used at command line are compatible.
 */
void
check_objfilter(bits32 objfilter)
{
	if ((objfilter & OBJFILTER_ALL_DBS) &&
		(objfilter & OBJFILTER_DATABASE))
		pg_fatal("cannot vacuum all databases and a specific one at the same time");

	if ((objfilter & OBJFILTER_TABLE) &&
		(objfilter & OBJFILTER_SCHEMA))
		pg_fatal("cannot vacuum all tables in schema(s) and specific table(s) at the same time");

	if ((objfilter & OBJFILTER_TABLE) &&
		(objfilter & OBJFILTER_SCHEMA_EXCLUDE))
		pg_fatal("cannot vacuum specific table(s) and exclude schema(s) at the same time");

	if ((objfilter & OBJFILTER_SCHEMA) &&
		(objfilter & OBJFILTER_SCHEMA_EXCLUDE))
		pg_fatal("cannot vacuum all tables in schema(s) and exclude schema(s) at the same time");
}


static void
help(const char *progname)
{
	printf(_("%s cleans and analyzes a PostgreSQL database.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [DBNAME]\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -a, --all                       vacuum all databases\n"));
	printf(_("      --buffer-usage-limit=SIZE   size of ring buffer used for vacuum\n"));
	printf(_("  -d, --dbname=DBNAME             database to vacuum\n"));
	printf(_("      --disable-page-skipping     disable all page-skipping behavior\n"));
	printf(_("  -e, --echo                      show the commands being sent to the server\n"));
	printf(_("  -f, --full                      do full vacuuming\n"));
	printf(_("  -F, --freeze                    freeze row transaction information\n"));
	printf(_("      --force-index-cleanup       always remove index entries that point to dead tuples\n"));
	printf(_("  -j, --jobs=NUM                  use this many concurrent connections to vacuum\n"));
	printf(_("      --min-mxid-age=MXID_AGE     minimum multixact ID age of tables to vacuum\n"));
	printf(_("      --min-xid-age=XID_AGE       minimum transaction ID age of tables to vacuum\n"));
	printf(_("      --missing-stats-only        only analyze relations with missing statistics\n"));
	printf(_("      --no-index-cleanup          don't remove index entries that point to dead tuples\n"));
	printf(_("      --no-process-main           skip the main relation\n"));
	printf(_("      --no-process-toast          skip the TOAST table associated with the table to vacuum\n"));
	printf(_("      --no-truncate               don't truncate empty pages at the end of the table\n"));
	printf(_("  -n, --schema=SCHEMA             vacuum tables in the specified schema(s) only\n"));
	printf(_("  -N, --exclude-schema=SCHEMA     do not vacuum tables in the specified schema(s)\n"));
	printf(_("  -P, --parallel=PARALLEL_WORKERS use this many background workers for vacuum, if available\n"));
	printf(_("  -q, --quiet                     don't write any messages\n"));
	printf(_("      --skip-locked               skip relations that cannot be immediately locked\n"));
	printf(_("  -t, --table='TABLE[(COLUMNS)]'  vacuum specific table(s) only\n"));
	printf(_("  -v, --verbose                   write a lot of output\n"));
	printf(_("  -V, --version                   output version information, then exit\n"));
	printf(_("  -z, --analyze                   update optimizer statistics\n"));
	printf(_("  -Z, --analyze-only              only update optimizer statistics; no vacuum\n"));
	printf(_("      --analyze-in-stages         only update optimizer statistics, in multiple\n"
			 "                                  stages for faster results; no vacuum\n"));
	printf(_("  -?, --help                      show this help, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME       database server host or socket directory\n"));
	printf(_("  -p, --port=PORT           database server port\n"));
	printf(_("  -U, --username=USERNAME   user name to connect as\n"));
	printf(_("  -w, --no-password         never prompt for password\n"));
	printf(_("  -W, --password            force password prompt\n"));
	printf(_("  --maintenance-db=DBNAME   alternate maintenance database\n"));
	printf(_("\nRead the description of the SQL command VACUUM for details.\n"));
	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}
