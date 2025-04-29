/*-------------------------------------------------------------------------
 *
 * pg_restore.c
 *	pg_restore is an utility extracting postgres database definitions
 *	from a backup archive created by pg_dump/pg_dumpall using the archiver
 *	interface.
 *
 *	pg_restore will read the backup archive and
 *	dump out a script that reproduces
 *	the schema of the database in terms of
 *		  user-defined types
 *		  user-defined functions
 *		  tables
 *		  indexes
 *		  aggregates
 *		  operators
 *		  ACL - grant/revoke
 *
 * the output script is SQL that is understood by PostgreSQL
 *
 * Basic process in a restore operation is:
 *
 *	Open the Archive and read the TOC.
 *	Set flags in TOC entries, and *maybe* reorder them.
 *	Generate script to stdout
 *	Exit
 *
 * Copyright (c) 2000, Philip Warner
 *		Rights are granted to use this software in any way so long
 *		as this notice is not removed.
 *
 *	The author is not responsible for loss or damages that may
 *	result from its use.
 *
 *
 * IDENTIFICATION
 *		src/bin/pg_dump/pg_restore.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <ctype.h>
#include <sys/stat.h>
#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif

#include "common/string.h"
#include "connectdb.h"
#include "fe_utils/option_utils.h"
#include "fe_utils/string_utils.h"
#include "filter.h"
#include "getopt_long.h"
#include "parallel.h"
#include "pg_backup_utils.h"

static void usage(const char *progname);
static void read_restore_filters(const char *filename, RestoreOptions *opts);
static bool file_exists_in_directory(const char *dir, const char *filename);
static int	restore_one_database(const char *inputFileSpec, RestoreOptions *opts,
								 int numWorkers, bool append_data, int num);
static int	read_one_statement(StringInfo inBuf, FILE *pfile);
static int	restore_all_databases(PGconn *conn, const char *dumpdirpath,
								  SimpleStringList db_exclude_patterns, RestoreOptions *opts, int numWorkers);
static int	process_global_sql_commands(PGconn *conn, const char *dumpdirpath,
										const char *outfile);
static void copy_or_print_global_file(const char *outfile, FILE *pfile);
static int	get_dbnames_list_to_restore(PGconn *conn,
										SimplePtrList *dbname_oid_list,
										SimpleStringList db_exclude_patterns);
static int	get_dbname_oid_list_from_mfile(const char *dumpdirpath,
										   SimplePtrList *dbname_oid_list);

/*
 * Stores a database OID and the corresponding name.
 */
typedef struct DbOidName
{
	Oid			oid;
	char		str[FLEXIBLE_ARRAY_MEMBER]; /* null-terminated string here */
} DbOidName;


int
main(int argc, char **argv)
{
	RestoreOptions *opts;
	int			c;
	int			numWorkers = 1;
	char	   *inputFileSpec;
	bool		data_only = false;
	bool		schema_only = false;
	int			n_errors = 0;
	bool		globals_only = false;
	SimpleStringList db_exclude_patterns = {NULL, NULL};
	static int	disable_triggers = 0;
	static int	enable_row_security = 0;
	static int	if_exists = 0;
	static int	no_data_for_failed_tables = 0;
	static int	outputNoTableAm = 0;
	static int	outputNoTablespaces = 0;
	static int	use_setsessauth = 0;
	static int	no_comments = 0;
	static int	no_data = 0;
	static int	no_policies = 0;
	static int	no_publications = 0;
	static int	no_schema = 0;
	static int	no_security_labels = 0;
	static int	no_statistics = 0;
	static int	no_subscriptions = 0;
	static int	strict_names = 0;
	static int	statistics_only = 0;
	static int	with_data = 0;
	static int	with_schema = 0;
	static int	with_statistics = 0;

	struct option cmdopts[] = {
		{"clean", 0, NULL, 'c'},
		{"create", 0, NULL, 'C'},
		{"data-only", 0, NULL, 'a'},
		{"globals-only", 0, NULL, 'g'},
		{"dbname", 1, NULL, 'd'},
		{"exit-on-error", 0, NULL, 'e'},
		{"exclude-schema", 1, NULL, 'N'},
		{"file", 1, NULL, 'f'},
		{"format", 1, NULL, 'F'},
		{"function", 1, NULL, 'P'},
		{"host", 1, NULL, 'h'},
		{"index", 1, NULL, 'I'},
		{"jobs", 1, NULL, 'j'},
		{"list", 0, NULL, 'l'},
		{"no-privileges", 0, NULL, 'x'},
		{"no-acl", 0, NULL, 'x'},
		{"no-owner", 0, NULL, 'O'},
		{"no-reconnect", 0, NULL, 'R'},
		{"port", 1, NULL, 'p'},
		{"no-password", 0, NULL, 'w'},
		{"password", 0, NULL, 'W'},
		{"schema", 1, NULL, 'n'},
		{"schema-only", 0, NULL, 's'},
		{"superuser", 1, NULL, 'S'},
		{"table", 1, NULL, 't'},
		{"trigger", 1, NULL, 'T'},
		{"use-list", 1, NULL, 'L'},
		{"username", 1, NULL, 'U'},
		{"verbose", 0, NULL, 'v'},
		{"single-transaction", 0, NULL, '1'},

		/*
		 * the following options don't have an equivalent short option letter
		 */
		{"disable-triggers", no_argument, &disable_triggers, 1},
		{"enable-row-security", no_argument, &enable_row_security, 1},
		{"if-exists", no_argument, &if_exists, 1},
		{"no-data-for-failed-tables", no_argument, &no_data_for_failed_tables, 1},
		{"no-table-access-method", no_argument, &outputNoTableAm, 1},
		{"no-tablespaces", no_argument, &outputNoTablespaces, 1},
		{"role", required_argument, NULL, 2},
		{"section", required_argument, NULL, 3},
		{"strict-names", no_argument, &strict_names, 1},
		{"transaction-size", required_argument, NULL, 5},
		{"use-set-session-authorization", no_argument, &use_setsessauth, 1},
		{"no-comments", no_argument, &no_comments, 1},
		{"no-data", no_argument, &no_data, 1},
		{"no-policies", no_argument, &no_policies, 1},
		{"no-publications", no_argument, &no_publications, 1},
		{"no-schema", no_argument, &no_schema, 1},
		{"no-security-labels", no_argument, &no_security_labels, 1},
		{"no-subscriptions", no_argument, &no_subscriptions, 1},
		{"no-statistics", no_argument, &no_statistics, 1},
		{"with-data", no_argument, &with_data, 1},
		{"with-schema", no_argument, &with_schema, 1},
		{"with-statistics", no_argument, &with_statistics, 1},
		{"statistics-only", no_argument, &statistics_only, 1},
		{"filter", required_argument, NULL, 4},
		{"exclude-database", required_argument, NULL, 6},

		{NULL, 0, NULL, 0}
	};

	pg_logging_init(argv[0]);
	pg_logging_set_level(PG_LOG_WARNING);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_dump"));

	init_parallel_dump_utils();

	opts = NewRestoreOptions();

	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage(progname);
			exit_nicely(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_restore (PostgreSQL) " PG_VERSION);
			exit_nicely(0);
		}
	}

	while ((c = getopt_long(argc, argv, "acCd:ef:F:gh:I:j:lL:n:N:Op:P:RsS:t:T:U:vwWx1",
							cmdopts, NULL)) != -1)
	{
		switch (c)
		{
			case 'a':			/* Dump data only */
				data_only = true;
				break;
			case 'c':			/* clean (i.e., drop) schema prior to create */
				opts->dropSchema = 1;
				break;
			case 'C':
				opts->createDB = 1;
				break;
			case 'd':
				opts->cparams.dbname = pg_strdup(optarg);
				break;
			case 'e':
				opts->exit_on_error = true;
				break;
			case 'f':			/* output file name */
				opts->filename = pg_strdup(optarg);
				break;
			case 'F':
				if (strlen(optarg) != 0)
					opts->formatName = pg_strdup(optarg);
				break;
			case 'g':
				/* restore only global.dat file from directory */
				globals_only = true;
				break;
			case 'h':
				if (strlen(optarg) != 0)
					opts->cparams.pghost = pg_strdup(optarg);
				break;
			case 'j':			/* number of restore jobs */
				if (!option_parse_int(optarg, "-j/--jobs", 1,
									  PG_MAX_JOBS,
									  &numWorkers))
					exit(1);
				break;

			case 'l':			/* Dump the TOC summary */
				opts->tocSummary = 1;
				break;

			case 'L':			/* input TOC summary file name */
				opts->tocFile = pg_strdup(optarg);
				break;

			case 'n':			/* Dump data for this schema only */
				simple_string_list_append(&opts->schemaNames, optarg);
				break;

			case 'N':			/* Do not dump data for this schema */
				simple_string_list_append(&opts->schemaExcludeNames, optarg);
				break;

			case 'O':
				opts->noOwner = 1;
				break;

			case 'p':
				if (strlen(optarg) != 0)
					opts->cparams.pgport = pg_strdup(optarg);
				break;
			case 'R':
				/* no-op, still accepted for backwards compatibility */
				break;
			case 'P':			/* Function */
				opts->selTypes = 1;
				opts->selFunction = 1;
				simple_string_list_append(&opts->functionNames, optarg);
				break;
			case 'I':			/* Index */
				opts->selTypes = 1;
				opts->selIndex = 1;
				simple_string_list_append(&opts->indexNames, optarg);
				break;
			case 'T':			/* Trigger */
				opts->selTypes = 1;
				opts->selTrigger = 1;
				simple_string_list_append(&opts->triggerNames, optarg);
				break;
			case 's':			/* dump schema only */
				schema_only = true;
				break;
			case 'S':			/* Superuser username */
				if (strlen(optarg) != 0)
					opts->superuser = pg_strdup(optarg);
				break;
			case 't':			/* Dump specified table(s) only */
				opts->selTypes = 1;
				opts->selTable = 1;
				simple_string_list_append(&opts->tableNames, optarg);
				break;

			case 'U':
				opts->cparams.username = pg_strdup(optarg);
				break;

			case 'v':			/* verbose */
				opts->verbose = 1;
				pg_logging_increase_verbosity();
				break;

			case 'w':
				opts->cparams.promptPassword = TRI_NO;
				break;

			case 'W':
				opts->cparams.promptPassword = TRI_YES;
				break;

			case 'x':			/* skip ACL dump */
				opts->aclsSkip = 1;
				break;

			case '1':			/* Restore data in a single transaction */
				opts->single_txn = true;
				opts->exit_on_error = true;
				break;

			case 0:

				/*
				 * This covers the long options without a short equivalent.
				 */
				break;

			case 2:				/* SET ROLE */
				opts->use_role = pg_strdup(optarg);
				break;

			case 3:				/* section */
				set_dump_section(optarg, &(opts->dumpSections));
				break;

			case 4:				/* filter */
				read_restore_filters(optarg, opts);
				break;

			case 5:				/* transaction-size */
				if (!option_parse_int(optarg, "--transaction-size",
									  1, INT_MAX,
									  &opts->txn_size))
					exit(1);
				opts->exit_on_error = true;
				break;
			case 6:				/* database patterns to skip */
				simple_string_list_append(&db_exclude_patterns, optarg);
				break;

			default:
				/* getopt_long already emitted a complaint */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit_nicely(1);
		}
	}

	/* Get file name from command line */
	if (optind < argc)
		inputFileSpec = argv[optind++];
	else
		inputFileSpec = NULL;

	/* Complain if any arguments remain */
	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit_nicely(1);
	}

	/* Complain if neither -f nor -d was specified (except if dumping TOC) */
	if (!opts->cparams.dbname && !opts->filename && !opts->tocSummary)
		pg_fatal("one of -d/--dbname and -f/--file must be specified");

	if (db_exclude_patterns.head != NULL && globals_only)
	{
		pg_log_error("option --exclude-database cannot be used together with -g/--globals-only");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit_nicely(1);
	}

	/* Should get at most one of -d and -f, else user is confused */
	if (opts->cparams.dbname)
	{
		if (opts->filename)
		{
			pg_log_error("options -d/--dbname and -f/--file cannot be used together");
			pg_log_error_hint("Try \"%s --help\" for more information.", progname);
			exit_nicely(1);
		}
		opts->useDB = 1;
	}

	/* reject conflicting "-only" options */
	if (data_only && schema_only)
		pg_fatal("options -s/--schema-only and -a/--data-only cannot be used together");
	if (schema_only && statistics_only)
		pg_fatal("options -s/--schema-only and --statistics-only cannot be used together");
	if (data_only && statistics_only)
		pg_fatal("options -a/--data-only and --statistics-only cannot be used together");

	/* reject conflicting "-only" and "no-" options */
	if (data_only && no_data)
		pg_fatal("options -a/--data-only and --no-data cannot be used together");
	if (schema_only && no_schema)
		pg_fatal("options -s/--schema-only and --no-schema cannot be used together");
	if (statistics_only && no_statistics)
		pg_fatal("options --statistics-only and --no-statistics cannot be used together");

	/* reject conflicting "with-" and "no-" options */
	if (with_data && no_data)
		pg_fatal("options --with-data and --no-data cannot be used together");
	if (with_schema && no_schema)
		pg_fatal("options --with-schema and --no-schema cannot be used together");
	if (with_statistics && no_statistics)
		pg_fatal("options --with-statistics and --no-statistics cannot be used together");

	if (data_only && opts->dropSchema)
		pg_fatal("options -c/--clean and -a/--data-only cannot be used together");

	if (opts->single_txn && opts->txn_size > 0)
		pg_fatal("options -1/--single-transaction and --transaction-size cannot be used together");

	/*
	 * -C is not compatible with -1, because we can't create a database inside
	 * a transaction block.
	 */
	if (opts->createDB && opts->single_txn)
		pg_fatal("options -C/--create and -1/--single-transaction cannot be used together");

	/* Can't do single-txn mode with multiple connections */
	if (opts->single_txn && numWorkers > 1)
		pg_fatal("cannot specify both --single-transaction and multiple jobs");

	/*
	 * Set derivative flags. An "-only" option may be overridden by an
	 * explicit "with-" option; e.g. "--schema-only --with-statistics" will
	 * include schema and statistics. Other ambiguous or nonsensical
	 * combinations, e.g. "--schema-only --no-schema", will have already
	 * caused an error in one of the checks above.
	 */
	opts->dumpData = ((opts->dumpData && !schema_only && !statistics_only) ||
					  (data_only || with_data)) && !no_data;
	opts->dumpSchema = ((opts->dumpSchema && !data_only && !statistics_only) ||
						(schema_only || with_schema)) && !no_schema;
	opts->dumpStatistics = ((opts->dumpStatistics && !schema_only && !data_only) ||
							(statistics_only || with_statistics)) && !no_statistics;

	opts->disable_triggers = disable_triggers;
	opts->enable_row_security = enable_row_security;
	opts->noDataForFailedTables = no_data_for_failed_tables;
	opts->noTableAm = outputNoTableAm;
	opts->noTablespace = outputNoTablespaces;
	opts->use_setsessauth = use_setsessauth;
	opts->no_comments = no_comments;
	opts->no_policies = no_policies;
	opts->no_publications = no_publications;
	opts->no_security_labels = no_security_labels;
	opts->no_subscriptions = no_subscriptions;

	if (if_exists && !opts->dropSchema)
		pg_fatal("option --if-exists requires option -c/--clean");
	opts->if_exists = if_exists;
	opts->strict_names = strict_names;

	if (opts->formatName)
	{
		if (pg_strcasecmp(opts->formatName, "c") == 0 ||
			pg_strcasecmp(opts->formatName, "custom") == 0)
			opts->format = archCustom;
		else if (pg_strcasecmp(opts->formatName, "d") == 0 ||
				 pg_strcasecmp(opts->formatName, "directory") == 0)
			opts->format = archDirectory;
		else if (pg_strcasecmp(opts->formatName, "t") == 0 ||
				 pg_strcasecmp(opts->formatName, "tar") == 0)
			opts->format = archTar;
		else if (pg_strcasecmp(opts->formatName, "p") == 0 ||
				 pg_strcasecmp(opts->formatName, "plain") == 0)
		{
			/* recognize this for consistency with pg_dump */
			pg_fatal("archive format \"%s\" is not supported; please use psql",
					 opts->formatName);
		}
		else
			pg_fatal("unrecognized archive format \"%s\"; please specify \"c\", \"d\", or \"t\"",
					 opts->formatName);
	}

	/*
	 * If toc.dat file is not present in the current path, then check for
	 * global.dat.  If global.dat file is present, then restore all the
	 * databases from map.dat (if it exists), but skip restoring those
	 * matching --exclude-database patterns.
	 */
	if (inputFileSpec != NULL && !file_exists_in_directory(inputFileSpec, "toc.dat") &&
		file_exists_in_directory(inputFileSpec, "global.dat"))
	{
		PGconn	   *conn = NULL;	/* Connection to restore global sql
									 * commands. */

		/*
		 * Can only use --list or --use-list options with a single database
		 * dump.
		 */
		if (opts->tocSummary)
			pg_fatal("option -l/--list cannot be used when restoring an archive created by pg_dumpall");
		else if (opts->tocFile)
			pg_fatal("option -L/--use-list cannot be used when restoring an archive created by pg_dumpall");

		/*
		 * To restore from a pg_dumpall archive, -C (create database) option
		 * must be specified unless we are only restoring globals.
		 */
		if (!globals_only && opts->createDB != 1)
		{
			pg_log_error("-C/--create option should be specified when restoring an archive created by pg_dumpall");
			pg_log_error_hint("Try \"%s --help\" for more information.", progname);
			pg_log_error_hint("Individual databases can be restored using their specific archives.");
			exit_nicely(1);
		}

		/*
		 * Connect to the database to execute global sql commands from
		 * global.dat file.
		 */
		if (opts->cparams.dbname)
		{
			conn = ConnectDatabase(opts->cparams.dbname, NULL, opts->cparams.pghost,
								   opts->cparams.pgport, opts->cparams.username, TRI_DEFAULT,
								   false, progname, NULL, NULL, NULL, NULL);


			if (!conn)
				pg_fatal("could not connect to database \"%s\"", opts->cparams.dbname);
		}

		/* If globals-only, then return from here. */
		if (globals_only)
		{
			/*
			 * Open global.dat file and execute/append all the global sql
			 * commands.
			 */
			n_errors = process_global_sql_commands(conn, inputFileSpec,
												   opts->filename);

			if (conn)
				PQfinish(conn);

			pg_log_info("database restoring skipped as -g/--globals-only option was specified");
		}
		else
		{
			/* Now restore all the databases from map.dat */
			n_errors = restore_all_databases(conn, inputFileSpec, db_exclude_patterns,
											 opts, numWorkers);
		}

		/* Free db pattern list. */
		simple_string_list_destroy(&db_exclude_patterns);
	}
	else						/* process if global.dat file does not exist. */
	{
		if (db_exclude_patterns.head != NULL)
			pg_fatal("option --exclude-database can be used only when restoring an archive created by pg_dumpall");

		if (globals_only)
			pg_fatal("option -g/--globals-only can be used only when restoring an archive created by pg_dumpall");

		n_errors = restore_one_database(inputFileSpec, opts, numWorkers, false, 0);
	}

	/* Done, print a summary of ignored errors during restore. */
	if (n_errors)
	{
		pg_log_warning("errors ignored on restore: %d", n_errors);
		return 1;
	}

	return 0;
}

/*
 * restore_one_database
 *
 * This will restore one database using toc.dat file.
 *
 * returns the number of errors while doing restore.
 */
static int
restore_one_database(const char *inputFileSpec, RestoreOptions *opts,
					 int numWorkers, bool append_data, int num)
{
	Archive    *AH;
	int			n_errors;

	AH = OpenArchive(inputFileSpec, opts->format);

	SetArchiveOptions(AH, NULL, opts);

	/*
	 * We don't have a connection yet but that doesn't matter. The connection
	 * is initialized to NULL and if we terminate through exit_nicely() while
	 * it's still NULL, the cleanup function will just be a no-op. If we are
	 * restoring multiple databases, then only update AX handle for cleanup as
	 * the previous entry was already in the array and we had closed previous
	 * connection, so we can use the same array slot.
	 */
	if (!append_data || num == 0)
		on_exit_close_archive(AH);
	else
		replace_on_exit_close_archive(AH);

	/* Let the archiver know how noisy to be */
	AH->verbose = opts->verbose;

	/*
	 * Whether to keep submitting sql commands as "pg_restore ... | psql ... "
	 */
	AH->exit_on_error = opts->exit_on_error;

	if (opts->tocFile)
		SortTocFromFile(AH);

	AH->numWorkers = numWorkers;

	if (opts->tocSummary)
		PrintTOCSummary(AH);
	else
	{
		ProcessArchiveRestoreOptions(AH);
		RestoreArchive(AH, append_data);
	}

	n_errors = AH->n_errors;

	/* AH may be freed in CloseArchive? */
	CloseArchive(AH);

	return n_errors;
}

static void
usage(const char *progname)
{
	printf(_("%s restores PostgreSQL databases from archives created by pg_dump or pg_dumpall.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [FILE]\n"), progname);

	printf(_("\nGeneral options:\n"));
	printf(_("  -d, --dbname=NAME        connect to database name\n"));
	printf(_("  -f, --file=FILENAME      output file name (- for stdout)\n"));
	printf(_("  -F, --format=c|d|t       backup file format (should be automatic)\n"));
	printf(_("  -l, --list               print summarized TOC of the archive\n"));
	printf(_("  -v, --verbose            verbose mode\n"));
	printf(_("  -V, --version            output version information, then exit\n"));
	printf(_("  -?, --help               show this help, then exit\n"));

	printf(_("\nOptions controlling the restore:\n"));
	printf(_("  -a, --data-only              restore only the data, no schema\n"));
	printf(_("  -c, --clean                  clean (drop) database objects before recreating\n"));
	printf(_("  -C, --create                 create the target database\n"));
	printf(_("  -e, --exit-on-error          exit on error, default is to continue\n"));
	printf(_("  -g, --globals-only           restore only global objects, no databases\n"));
	printf(_("  -I, --index=NAME             restore named index\n"));
	printf(_("  -j, --jobs=NUM               use this many parallel jobs to restore\n"));
	printf(_("  -L, --use-list=FILENAME      use table of contents from this file for\n"
			 "                               selecting/ordering output\n"));
	printf(_("  -n, --schema=NAME            restore only objects in this schema\n"));
	printf(_("  -N, --exclude-schema=NAME    do not restore objects in this schema\n"));
	printf(_("  -O, --no-owner               skip restoration of object ownership\n"));
	printf(_("  -P, --function=NAME(args)    restore named function\n"));
	printf(_("  -s, --schema-only            restore only the schema, no data\n"));
	printf(_("  -S, --superuser=NAME         superuser user name to use for disabling triggers\n"));
	printf(_("  -t, --table=NAME             restore named relation (table, view, etc.)\n"));
	printf(_("  -T, --trigger=NAME           restore named trigger\n"));
	printf(_("  -x, --no-privileges          skip restoration of access privileges (grant/revoke)\n"));
	printf(_("  -1, --single-transaction     restore as a single transaction\n"));
	printf(_("  --disable-triggers           disable triggers during data-only restore\n"));
	printf(_("  --enable-row-security        enable row security\n"));
	printf(_("  --exclude-database=PATTERN   do not restore the specified database(s)\n"));
	printf(_("  --filter=FILENAME            restore or skip objects based on expressions\n"
			 "                               in FILENAME\n"));
	printf(_("  --if-exists                  use IF EXISTS when dropping objects\n"));
	printf(_("  --no-comments                do not restore comment commands\n"));
	printf(_("  --no-data                    do not restore data\n"));
	printf(_("  --no-data-for-failed-tables  do not restore data of tables that could not be\n"
			 "                               created\n"));
	printf(_("  --no-policies                do not restore row security policies\n"));
	printf(_("  --no-publications            do not restore publications\n"));
	printf(_("  --no-schema                  do not restore schema\n"));
	printf(_("  --no-security-labels         do not restore security labels\n"));
	printf(_("  --no-statistics              do not restore statistics\n"));
	printf(_("  --no-subscriptions           do not restore subscriptions\n"));
	printf(_("  --no-table-access-method     do not restore table access methods\n"));
	printf(_("  --no-tablespaces             do not restore tablespace assignments\n"));
	printf(_("  --section=SECTION            restore named section (pre-data, data, or post-data)\n"));
	printf(_("  --statistics-only            restore only the statistics, not schema or data\n"));
	printf(_("  --strict-names               require table and/or schema include patterns to\n"
			 "                               match at least one entity each\n"));
	printf(_("  --transaction-size=N         commit after every N objects\n"));
	printf(_("  --use-set-session-authorization\n"
			 "                               use SET SESSION AUTHORIZATION commands instead of\n"
			 "                               ALTER OWNER commands to set ownership\n"));
	printf(_("  --with-data                  dump the data\n"));
	printf(_("  --with-schema                dump the schema\n"));
	printf(_("  --with-statistics            dump the statistics\n"));

	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME      database server host or socket directory\n"));
	printf(_("  -p, --port=PORT          database server port number\n"));
	printf(_("  -U, --username=NAME      connect as specified database user\n"));
	printf(_("  -w, --no-password        never prompt for password\n"));
	printf(_("  -W, --password           force password prompt (should happen automatically)\n"));
	printf(_("  --role=ROLENAME          do SET ROLE before restore\n"));

	printf(_("\n"
			 "The options -I, -n, -N, -P, -t, -T, --section, and --exclude-database can be combined\n"
			 "and specified multiple times to select multiple objects.\n"));
	printf(_("\nIf no input file name is supplied, then standard input is used.\n\n"));
	printf(_("Report bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}

/*
 * read_restore_filters - retrieve object identifier patterns from file
 *
 * Parse the specified filter file for include and exclude patterns, and add
 * them to the relevant lists.  If the filename is "-" then filters will be
 * read from STDIN rather than a file.
 */
static void
read_restore_filters(const char *filename, RestoreOptions *opts)
{
	FilterStateData fstate;
	char	   *objname;
	FilterCommandType comtype;
	FilterObjectType objtype;

	filter_init(&fstate, filename, exit_nicely);

	while (filter_read_item(&fstate, &objname, &comtype, &objtype))
	{
		if (comtype == FILTER_COMMAND_TYPE_INCLUDE)
		{
			switch (objtype)
			{
				case FILTER_OBJECT_TYPE_NONE:
					break;
				case FILTER_OBJECT_TYPE_TABLE_DATA:
				case FILTER_OBJECT_TYPE_TABLE_DATA_AND_CHILDREN:
				case FILTER_OBJECT_TYPE_TABLE_AND_CHILDREN:
				case FILTER_OBJECT_TYPE_DATABASE:
				case FILTER_OBJECT_TYPE_EXTENSION:
				case FILTER_OBJECT_TYPE_FOREIGN_DATA:
					pg_log_filter_error(&fstate, _("%s filter for \"%s\" is not allowed"),
										"include",
										filter_object_type_name(objtype));
					exit_nicely(1);

				case FILTER_OBJECT_TYPE_FUNCTION:
					opts->selTypes = 1;
					opts->selFunction = 1;
					simple_string_list_append(&opts->functionNames, objname);
					break;
				case FILTER_OBJECT_TYPE_INDEX:
					opts->selTypes = 1;
					opts->selIndex = 1;
					simple_string_list_append(&opts->indexNames, objname);
					break;
				case FILTER_OBJECT_TYPE_SCHEMA:
					simple_string_list_append(&opts->schemaNames, objname);
					break;
				case FILTER_OBJECT_TYPE_TABLE:
					opts->selTypes = 1;
					opts->selTable = 1;
					simple_string_list_append(&opts->tableNames, objname);
					break;
				case FILTER_OBJECT_TYPE_TRIGGER:
					opts->selTypes = 1;
					opts->selTrigger = 1;
					simple_string_list_append(&opts->triggerNames, objname);
					break;
			}
		}
		else if (comtype == FILTER_COMMAND_TYPE_EXCLUDE)
		{
			switch (objtype)
			{
				case FILTER_OBJECT_TYPE_NONE:
					break;
				case FILTER_OBJECT_TYPE_TABLE_DATA:
				case FILTER_OBJECT_TYPE_TABLE_DATA_AND_CHILDREN:
				case FILTER_OBJECT_TYPE_DATABASE:
				case FILTER_OBJECT_TYPE_EXTENSION:
				case FILTER_OBJECT_TYPE_FOREIGN_DATA:
				case FILTER_OBJECT_TYPE_FUNCTION:
				case FILTER_OBJECT_TYPE_INDEX:
				case FILTER_OBJECT_TYPE_TABLE:
				case FILTER_OBJECT_TYPE_TABLE_AND_CHILDREN:
				case FILTER_OBJECT_TYPE_TRIGGER:
					pg_log_filter_error(&fstate, _("%s filter for \"%s\" is not allowed"),
										"exclude",
										filter_object_type_name(objtype));
					exit_nicely(1);

				case FILTER_OBJECT_TYPE_SCHEMA:
					simple_string_list_append(&opts->schemaExcludeNames, objname);
					break;
			}
		}
		else
		{
			Assert(comtype == FILTER_COMMAND_TYPE_NONE);
			Assert(objtype == FILTER_OBJECT_TYPE_NONE);
		}

		if (objname)
			free(objname);
	}

	filter_free(&fstate);
}

/*
 * file_exists_in_directory
 *
 * Returns true if the file exists in the given directory.
 */
static bool
file_exists_in_directory(const char *dir, const char *filename)
{
	struct stat st;
	char		buf[MAXPGPATH];

	if (snprintf(buf, MAXPGPATH, "%s/%s", dir, filename) >= MAXPGPATH)
		pg_fatal("directory name too long: \"%s\"", dir);

	return (stat(buf, &st) == 0 && S_ISREG(st.st_mode));
}

/*
 * read_one_statement
 *
 * This will start reading from passed file pointer using fgetc and read till
 * semicolon(sql statement terminator for global.dat file)
 *
 * EOF is returned if end-of-file input is seen; time to shut down.
 */

static int
read_one_statement(StringInfo inBuf, FILE *pfile)
{
	int			c;				/* character read from getc() */
	int			m;

	StringInfoData q;

	initStringInfo(&q);

	resetStringInfo(inBuf);

	/*
	 * Read characters until EOF or the appropriate delimiter is seen.
	 */
	while ((c = fgetc(pfile)) != EOF)
	{
		if (c != '\'' && c != '"' && c != '\n' && c != ';')
		{
			appendStringInfoChar(inBuf, (char) c);
			while ((c = fgetc(pfile)) != EOF)
			{
				if (c != '\'' && c != '"' && c != ';' && c != '\n')
					appendStringInfoChar(inBuf, (char) c);
				else
					break;
			}
		}

		if (c == '\'' || c == '"')
		{
			appendStringInfoChar(&q, (char) c);
			m = c;

			while ((c = fgetc(pfile)) != EOF)
			{
				appendStringInfoChar(&q, (char) c);

				if (c == m)
				{
					appendStringInfoString(inBuf, q.data);
					resetStringInfo(&q);
					break;
				}
			}
		}

		if (c == ';')
		{
			appendStringInfoChar(inBuf, (char) ';');
			break;
		}

		if (c == '\n')
			appendStringInfoChar(inBuf, (char) '\n');
	}

	pg_free(q.data);

	/* No input before EOF signal means time to quit. */
	if (c == EOF && inBuf->len == 0)
		return EOF;

	/* return something that's not EOF */
	return 'Q';
}

/*
 * get_dbnames_list_to_restore
 *
 * This will mark for skipping any entries from dbname_oid_list that pattern match an
 * entry in the db_exclude_patterns list.
 *
 * Returns the number of database to be restored.
 *
 */
static int
get_dbnames_list_to_restore(PGconn *conn,
							SimplePtrList *dbname_oid_list,
							SimpleStringList db_exclude_patterns)
{
	int			count_db = 0;
	PQExpBuffer query;
	PGresult   *res;

	query = createPQExpBuffer();

	if (!conn)
		pg_log_info("considering PATTERN as NAME for --exclude-database option as no db connection while doing pg_restore.");

	/*
	 * Process one by one all dbnames and if specified to skip restoring, then
	 * remove dbname from list.
	 */
	for (SimplePtrListCell *db_cell = dbname_oid_list->head;
		 db_cell; db_cell = db_cell->next)
	{
		DbOidName  *dbidname = (DbOidName *) db_cell->ptr;
		bool		skip_db_restore = false;
		PQExpBuffer db_lit = createPQExpBuffer();

		appendStringLiteralConn(db_lit, dbidname->str, conn);

		for (SimpleStringListCell *pat_cell = db_exclude_patterns.head; pat_cell; pat_cell = pat_cell->next)
		{
			/*
			 * If there is an exact match then we don't need to try a pattern
			 * match
			 */
			if (pg_strcasecmp(dbidname->str, pat_cell->val) == 0)
				skip_db_restore = true;
			/* Otherwise, try a pattern match if there is a connection */
			else if (conn)
			{
				int			dotcnt;

				appendPQExpBufferStr(query, "SELECT 1 ");
				processSQLNamePattern(conn, query, pat_cell->val, false,
									  false, NULL, db_lit->data,
									  NULL, NULL, NULL, &dotcnt);

				if (dotcnt > 0)
				{
					pg_log_error("improper qualified name (too many dotted names): %s",
								 dbidname->str);
					PQfinish(conn);
					exit_nicely(1);
				}

				res = executeQuery(conn, query->data);

				if ((PQresultStatus(res) == PGRES_TUPLES_OK) && PQntuples(res))
				{
					skip_db_restore = true;
					pg_log_info("database \"%s\" matches exclude pattern: \"%s\"", dbidname->str, pat_cell->val);
				}

				PQclear(res);
				resetPQExpBuffer(query);
			}

			if (skip_db_restore)
				break;
		}

		destroyPQExpBuffer(db_lit);

		/*
		 * Mark db to be skipped or increment the counter of dbs to be
		 * restored
		 */
		if (skip_db_restore)
		{
			pg_log_info("excluding database \"%s\"", dbidname->str);
			dbidname->oid = InvalidOid;
		}
		else
		{
			count_db++;
		}
	}

	destroyPQExpBuffer(query);

	return count_db;
}

/*
 * get_dbname_oid_list_from_mfile
 *
 * Open map.dat file and read line by line and then prepare a list of database
 * names and corresponding db_oid.
 *
 * Returns, total number of database names in map.dat file.
 */
static int
get_dbname_oid_list_from_mfile(const char *dumpdirpath, SimplePtrList *dbname_oid_list)
{
	StringInfoData linebuf;
	FILE	   *pfile;
	char		map_file_path[MAXPGPATH];
	int			count = 0;


	/*
	 * If there is only global.dat file in dump, then return from here as
	 * there is no database to restore.
	 */
	if (!file_exists_in_directory(dumpdirpath, "map.dat"))
	{
		pg_log_info("database restoring is skipped as \"map.dat\" is not present in \"%s\"", dumpdirpath);
		return 0;
	}

	snprintf(map_file_path, MAXPGPATH, "%s/map.dat", dumpdirpath);

	/* Open map.dat file. */
	pfile = fopen(map_file_path, PG_BINARY_R);

	if (pfile == NULL)
		pg_fatal("could not open \"%s\": %m", map_file_path);

	initStringInfo(&linebuf);

	/* Append all the dbname/db_oid combinations to the list. */
	while (pg_get_line_buf(pfile, &linebuf))
	{
		Oid			db_oid = InvalidOid;
		char	   *dbname;
		DbOidName  *dbidname;
		int			namelen;
		char	   *p = linebuf.data;

		/* Extract dboid. */
		while (isdigit((unsigned char) *p))
			p++;
		if (p > linebuf.data && *p == ' ')
		{
			sscanf(linebuf.data, "%u", &db_oid);
			p++;
		}

		/* dbname is the rest of the line */
		dbname = p;
		namelen = strlen(dbname);

		/* Report error and exit if the file has any corrupted data. */
		if (!OidIsValid(db_oid) || namelen <= 1)
			pg_fatal("invalid entry in \"%s\" at line: %d", map_file_path,
					 count + 1);

		pg_log_info("found database \"%s\" (OID: %u) in \"%s\"",
					dbname, db_oid, map_file_path);

		dbidname = pg_malloc(offsetof(DbOidName, str) + namelen + 1);
		dbidname->oid = db_oid;
		strlcpy(dbidname->str, dbname, namelen);

		simple_ptr_list_append(dbname_oid_list, dbidname);
		count++;
	}

	/* Close map.dat file. */
	fclose(pfile);

	return count;
}

/*
 * restore_all_databases
 *
 * This will restore databases those dumps are present in
 * directory based on map.dat file mapping.
 *
 * This will skip restoring for databases that are specified with
 * exclude-database option.
 *
 * returns, number of errors while doing restore.
 */
static int
restore_all_databases(PGconn *conn, const char *dumpdirpath,
					  SimpleStringList db_exclude_patterns, RestoreOptions *opts,
					  int numWorkers)
{
	SimplePtrList dbname_oid_list = {NULL, NULL};
	int			num_db_restore = 0;
	int			num_total_db;
	int			n_errors_total;
	int			count = 0;
	char	   *connected_db = NULL;
	bool		dumpData = opts->dumpData;
	bool		dumpSchema = opts->dumpSchema;
	bool		dumpStatistics = opts->dumpSchema;

	/* Save db name to reuse it for all the database. */
	if (opts->cparams.dbname)
		connected_db = opts->cparams.dbname;

	num_total_db = get_dbname_oid_list_from_mfile(dumpdirpath, &dbname_oid_list);

	/* If map.dat has no entries, return after processing global.dat */
	if (dbname_oid_list.head == NULL)
		return process_global_sql_commands(conn, dumpdirpath, opts->filename);

	pg_log_info("found %d database names in \"map.dat\"", num_total_db);

	if (!conn)
	{
		pg_log_info("trying to connect database \"postgres\"");

		conn = ConnectDatabase("postgres", NULL, opts->cparams.pghost,
							   opts->cparams.pgport, opts->cparams.username, TRI_DEFAULT,
							   false, progname, NULL, NULL, NULL, NULL);

		/* Try with template1. */
		if (!conn)
		{
			pg_log_info("trying to connect database \"template1\"");

			conn = ConnectDatabase("template1", NULL, opts->cparams.pghost,
								   opts->cparams.pgport, opts->cparams.username, TRI_DEFAULT,
								   false, progname, NULL, NULL, NULL, NULL);
		}
	}

	/*
	 * filter the db list according to the exclude patterns
	 */
	num_db_restore = get_dbnames_list_to_restore(conn, &dbname_oid_list,
												 db_exclude_patterns);

	/* Open global.dat file and execute/append all the global sql commands. */
	n_errors_total = process_global_sql_commands(conn, dumpdirpath, opts->filename);

	/* Close the db connection as we are done with globals and patterns. */
	if (conn)
		PQfinish(conn);

	/* Exit if no db needs to be restored. */
	if (dbname_oid_list.head == NULL || num_db_restore == 0)
	{
		pg_log_info("no database needs to restore out of %d databases", num_total_db);
		return n_errors_total;
	}

	pg_log_info("need to restore %d databases out of %d databases", num_db_restore, num_total_db);

	/*
	 * We have a list of databases to restore after processing the
	 * exclude-database switch(es).  Now we can restore them one by one.
	 */
	for (SimplePtrListCell *db_cell = dbname_oid_list.head;
		 db_cell; db_cell = db_cell->next)
	{
		DbOidName  *dbidname = (DbOidName *) db_cell->ptr;
		char		subdirpath[MAXPGPATH];
		char		subdirdbpath[MAXPGPATH];
		char		dbfilename[MAXPGPATH];
		int			n_errors;

		/* ignore dbs marked for skipping */
		if (dbidname->oid == InvalidOid)
			continue;

		/*
		 * We need to reset override_dbname so that objects can be restored
		 * into an already created database. (used with -d/--dbname option)
		 */
		if (opts->cparams.override_dbname)
		{
			pfree(opts->cparams.override_dbname);
			opts->cparams.override_dbname = NULL;
		}

		snprintf(subdirdbpath, MAXPGPATH, "%s/databases", dumpdirpath);

		/*
		 * Look for the database dump file/dir. If there is an {oid}.tar or
		 * {oid}.dmp file, use it. Otherwise try to use a directory called
		 * {oid}
		 */
		snprintf(dbfilename, MAXPGPATH, "%u.tar", dbidname->oid);
		if (file_exists_in_directory(subdirdbpath, dbfilename))
			snprintf(subdirpath, MAXPGPATH, "%s/databases/%u.tar", dumpdirpath, dbidname->oid);
		else
		{
			snprintf(dbfilename, MAXPGPATH, "%u.dmp", dbidname->oid);

			if (file_exists_in_directory(subdirdbpath, dbfilename))
				snprintf(subdirpath, MAXPGPATH, "%s/databases/%u.dmp", dumpdirpath, dbidname->oid);
			else
				snprintf(subdirpath, MAXPGPATH, "%s/databases/%u", dumpdirpath, dbidname->oid);
		}

		pg_log_info("restoring database \"%s\"", dbidname->str);

		/* If database is already created, then don't set createDB flag. */
		if (opts->cparams.dbname)
		{
			PGconn	   *test_conn;

			test_conn = ConnectDatabase(dbidname->str, NULL, opts->cparams.pghost,
										opts->cparams.pgport, opts->cparams.username, TRI_DEFAULT,
										false, progname, NULL, NULL, NULL, NULL);
			if (test_conn)
			{
				PQfinish(test_conn);

				/* Use already created database for connection. */
				opts->createDB = 0;
				opts->cparams.dbname = dbidname->str;
			}
			else
			{
				/* we'll have to create it */
				opts->createDB = 1;
				opts->cparams.dbname = connected_db;
			}
		}

		/*
		 * Reset flags - might have been reset in pg_backup_archiver.c by the
		 * previous restore.
		 */
		opts->dumpData = dumpData;
		opts->dumpSchema = dumpSchema;
		opts->dumpStatistics = dumpStatistics;

		/* Restore the single database. */
		n_errors = restore_one_database(subdirpath, opts, numWorkers, true, count);

		/* Print a summary of ignored errors during single database restore. */
		if (n_errors)
		{
			n_errors_total += n_errors;
			pg_log_warning("errors ignored on database \"%s\" restore: %d", dbidname->str, n_errors);
		}

		count++;
	}

	/* Log number of processed databases. */
	pg_log_info("number of restored databases is %d", num_db_restore);

	/* Free dbname and dboid list. */
	simple_ptr_list_destroy(&dbname_oid_list);

	return n_errors_total;
}

/*
 * process_global_sql_commands
 *
 * Open global.dat and execute or copy the sql commands one by one.
 *
 * If outfile is not NULL, copy all sql commands into outfile rather than
 * executing them.
 *
 * Returns the number of errors while processing global.dat
 */
static int
process_global_sql_commands(PGconn *conn, const char *dumpdirpath, const char *outfile)
{
	char		global_file_path[MAXPGPATH];
	PGresult   *result;
	StringInfoData sqlstatement,
				user_create;
	FILE	   *pfile;
	int			n_errors = 0;

	snprintf(global_file_path, MAXPGPATH, "%s/global.dat", dumpdirpath);

	/* Open global.dat file. */
	pfile = fopen(global_file_path, PG_BINARY_R);

	if (pfile == NULL)
		pg_fatal("could not open \"%s\": %m", global_file_path);

	/*
	 * If outfile is given, then just copy all global.dat file data into
	 * outfile.
	 */
	if (outfile)
	{
		copy_or_print_global_file(outfile, pfile);
		return 0;
	}

	/* Init sqlstatement to append commands. */
	initStringInfo(&sqlstatement);

	/* creation statement for our current role */
	initStringInfo(&user_create);
	appendStringInfoString(&user_create, "CREATE ROLE ");
	/* should use fmtId here, but we don't know the encoding */
	appendStringInfoString(&user_create, PQuser(conn));
	appendStringInfoChar(&user_create, ';');

	/* Process file till EOF and execute sql statements. */
	while (read_one_statement(&sqlstatement, pfile) != EOF)
	{
		/* don't try to create the role we are connected as */
		if (strstr(sqlstatement.data, user_create.data))
			continue;

		pg_log_info("executing query: %s", sqlstatement.data);
		result = PQexec(conn, sqlstatement.data);

		switch (PQresultStatus(result))
		{
			case PGRES_COMMAND_OK:
			case PGRES_TUPLES_OK:
			case PGRES_EMPTY_QUERY:
				break;
			default:
				n_errors++;
				pg_log_error("could not execute query: \"%s\" \nCommand was: \"%s\"", PQerrorMessage(conn), sqlstatement.data);
		}
		PQclear(result);
	}

	/* Print a summary of ignored errors during global.dat. */
	if (n_errors)
		pg_log_warning("ignored %d errors in \"%s\"", n_errors, global_file_path);

	fclose(pfile);

	return n_errors;
}

/*
 * copy_or_print_global_file
 *
 * Copy global.dat into the output file.  If "-" is used as outfile,
 * then print commands to stdout.
 */
static void
copy_or_print_global_file(const char *outfile, FILE *pfile)
{
	char		out_file_path[MAXPGPATH];
	FILE	   *OPF;
	int			c;

	/* "-" is used for stdout. */
	if (strcmp(outfile, "-") == 0)
		OPF = stdout;
	else
	{
		snprintf(out_file_path, MAXPGPATH, "%s", outfile);
		OPF = fopen(out_file_path, PG_BINARY_W);

		if (OPF == NULL)
		{
			fclose(pfile);
			pg_fatal("could not open file: \"%s\"", outfile);
		}
	}

	/* Append global.dat into output file or print to stdout. */
	while ((c = fgetc(pfile)) != EOF)
		fputc(c, OPF);

	fclose(pfile);

	/* Close output file. */
	if (strcmp(outfile, "-") != 0)
		fclose(OPF);
}
