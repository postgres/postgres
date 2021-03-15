/*-------------------------------------------------------------------------
 *
 * pg_amcheck.c
 *		Detects corruption within database relations.
 *
 * Copyright (c) 2017-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/bin/pg_amcheck/pg_amcheck.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <time.h>

#include "catalog/pg_am_d.h"
#include "catalog/pg_namespace_d.h"
#include "common/logging.h"
#include "common/username.h"
#include "fe_utils/cancel.h"
#include "fe_utils/option_utils.h"
#include "fe_utils/parallel_slot.h"
#include "fe_utils/query_utils.h"
#include "fe_utils/simple_list.h"
#include "fe_utils/string_utils.h"
#include "getopt_long.h"		/* pgrminclude ignore */
#include "pgtime.h"
#include "storage/block.h"

typedef struct PatternInfo
{
	const char *pattern;		/* Unaltered pattern from the command line */
	char	   *db_regex;		/* Database regexp parsed from pattern, or
								 * NULL */
	char	   *nsp_regex;		/* Schema regexp parsed from pattern, or NULL */
	char	   *rel_regex;		/* Relation regexp parsed from pattern, or
								 * NULL */
	bool		heap_only;		/* true if rel_regex should only match heap
								 * tables */
	bool		btree_only;		/* true if rel_regex should only match btree
								 * indexes */
	bool		matched;		/* true if the pattern matched in any database */
} PatternInfo;

typedef struct PatternInfoArray
{
	PatternInfo *data;
	size_t		len;
} PatternInfoArray;

/* pg_amcheck command line options controlled by user flags */
typedef struct AmcheckOptions
{
	bool		dbpattern;
	bool		alldb;
	bool		echo;
	bool		quiet;
	bool		verbose;
	bool		strict_names;
	bool		show_progress;
	int			jobs;

	/* Objects to check or not to check, as lists of PatternInfo structs. */
	PatternInfoArray include;
	PatternInfoArray exclude;

	/*
	 * As an optimization, if any pattern in the exclude list applies to heap
	 * tables, or similarly if any such pattern applies to btree indexes, or
	 * to schemas, then these will be true, otherwise false.  These should
	 * always agree with what you'd conclude by grep'ing through the exclude
	 * list.
	 */
	bool		excludetbl;
	bool		excludeidx;
	bool		excludensp;

	/*
	 * If any inclusion pattern exists, then we should only be checking
	 * matching relations rather than all relations, so this is true iff
	 * include is empty.
	 */
	bool		allrel;

	/* heap table checking options */
	bool		no_toast_expansion;
	bool		reconcile_toast;
	bool		on_error_stop;
	int64		startblock;
	int64		endblock;
	const char *skip;

	/* btree index checking options */
	bool		parent_check;
	bool		rootdescend;
	bool		heapallindexed;

	/* heap and btree hybrid option */
	bool		no_btree_expansion;
} AmcheckOptions;

static AmcheckOptions opts = {
	.dbpattern = false,
	.alldb = false,
	.echo = false,
	.quiet = false,
	.verbose = false,
	.strict_names = true,
	.show_progress = false,
	.jobs = 1,
	.include = {NULL, 0},
	.exclude = {NULL, 0},
	.excludetbl = false,
	.excludeidx = false,
	.excludensp = false,
	.allrel = true,
	.no_toast_expansion = false,
	.reconcile_toast = true,
	.on_error_stop = false,
	.startblock = -1,
	.endblock = -1,
	.skip = "none",
	.parent_check = false,
	.rootdescend = false,
	.heapallindexed = false,
	.no_btree_expansion = false
};

static const char *progname = NULL;

/* Whether all relations have so far passed their corruption checks */
static bool all_checks_pass = true;

/* Time last progress report was displayed */
static pg_time_t last_progress_report = 0;
static bool progress_since_last_stderr = false;

typedef struct DatabaseInfo
{
	char	   *datname;
	char	   *amcheck_schema; /* escaped, quoted literal */
} DatabaseInfo;

typedef struct RelationInfo
{
	const DatabaseInfo *datinfo;	/* shared by other relinfos */
	Oid			reloid;
	bool		is_heap;		/* true if heap, false if btree */
	char	   *nspname;
	char	   *relname;
	int			relpages;
	int			blocks_to_check;
	char	   *sql;			/* set during query run, pg_free'd after */
} RelationInfo;

/*
 * Query for determining if contrib's amcheck is installed.  If so, selects the
 * namespace name where amcheck's functions can be found.
 */
static const char *amcheck_sql =
"SELECT n.nspname, x.extversion FROM pg_catalog.pg_extension x"
"\nJOIN pg_catalog.pg_namespace n ON x.extnamespace = n.oid"
"\nWHERE x.extname = 'amcheck'";

static void prepare_heap_command(PQExpBuffer sql, RelationInfo *rel,
								 PGconn *conn);
static void prepare_btree_command(PQExpBuffer sql, RelationInfo *rel,
								  PGconn *conn);
static void run_command(ParallelSlot *slot, const char *sql);
static bool verify_heap_slot_handler(PGresult *res, PGconn *conn,
									 void *context);
static bool verify_btree_slot_handler(PGresult *res, PGconn *conn, void *context);
static void help(const char *progname);
static void progress_report(uint64 relations_total, uint64 relations_checked,
							uint64 relpages_total, uint64 relpages_checked,
							const char *datname, bool force, bool finished);

static void append_database_pattern(PatternInfoArray *pia, const char *pattern,
									int encoding);
static void append_schema_pattern(PatternInfoArray *pia, const char *pattern,
								  int encoding);
static void append_relation_pattern(PatternInfoArray *pia, const char *pattern,
									int encoding);
static void append_heap_pattern(PatternInfoArray *pia, const char *pattern,
								int encoding);
static void append_btree_pattern(PatternInfoArray *pia, const char *pattern,
								 int encoding);
static void compile_database_list(PGconn *conn, SimplePtrList *databases,
								  const char *initial_dbname);
static void compile_relation_list_one_db(PGconn *conn, SimplePtrList *relations,
										 const DatabaseInfo *datinfo,
										 uint64 *pagecount);

#define log_no_match(...) do { \
		if (opts.strict_names) \
			pg_log_generic(PG_LOG_ERROR, __VA_ARGS__); \
		else \
			pg_log_generic(PG_LOG_WARNING, __VA_ARGS__); \
	} while(0)

#define FREE_AND_SET_NULL(x) do { \
	pg_free(x); \
	(x) = NULL; \
	} while (0)

int
main(int argc, char *argv[])
{
	PGconn	   *conn = NULL;
	SimplePtrListCell *cell;
	SimplePtrList databases = {NULL, NULL};
	SimplePtrList relations = {NULL, NULL};
	bool		failed = false;
	const char *latest_datname;
	int			parallel_workers;
	ParallelSlotArray *sa;
	PQExpBufferData sql;
	uint64		reltotal = 0;
	uint64		pageschecked = 0;
	uint64		pagestotal = 0;
	uint64		relprogress = 0;
	int			pattern_id;

	static struct option long_options[] = {
		/* Connection options */
		{"host", required_argument, NULL, 'h'},
		{"port", required_argument, NULL, 'p'},
		{"username", required_argument, NULL, 'U'},
		{"no-password", no_argument, NULL, 'w'},
		{"password", no_argument, NULL, 'W'},
		{"maintenance-db", required_argument, NULL, 1},

		/* check options */
		{"all", no_argument, NULL, 'a'},
		{"database", required_argument, NULL, 'd'},
		{"exclude-database", required_argument, NULL, 'D'},
		{"echo", no_argument, NULL, 'e'},
		{"index", required_argument, NULL, 'i'},
		{"exclude-index", required_argument, NULL, 'I'},
		{"jobs", required_argument, NULL, 'j'},
		{"progress", no_argument, NULL, 'P'},
		{"quiet", no_argument, NULL, 'q'},
		{"relation", required_argument, NULL, 'r'},
		{"exclude-relation", required_argument, NULL, 'R'},
		{"schema", required_argument, NULL, 's'},
		{"exclude-schema", required_argument, NULL, 'S'},
		{"table", required_argument, NULL, 't'},
		{"exclude-table", required_argument, NULL, 'T'},
		{"verbose", no_argument, NULL, 'v'},
		{"no-dependent-indexes", no_argument, NULL, 2},
		{"no-dependent-toast", no_argument, NULL, 3},
		{"exclude-toast-pointers", no_argument, NULL, 4},
		{"on-error-stop", no_argument, NULL, 5},
		{"skip", required_argument, NULL, 6},
		{"startblock", required_argument, NULL, 7},
		{"endblock", required_argument, NULL, 8},
		{"rootdescend", no_argument, NULL, 9},
		{"no-strict-names", no_argument, NULL, 10},
		{"heapallindexed", no_argument, NULL, 11},
		{"parent-check", no_argument, NULL, 12},

		{NULL, 0, NULL, 0}
	};

	int			optindex;
	int			c;

	const char *db = NULL;
	const char *maintenance_db = NULL;

	const char *host = NULL;
	const char *port = NULL;
	const char *username = NULL;
	enum trivalue prompt_password = TRI_DEFAULT;
	int			encoding = pg_get_encoding_from_locale(NULL, false);
	ConnParams	cparams;

	pg_logging_init(argv[0]);
	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_amcheck"));

	handle_help_version_opts(argc, argv, progname, help);

	/* process command-line options */
	while ((c = getopt_long(argc, argv, "ad:D:eh:Hi:I:j:p:Pqr:R:s:S:t:T:U:wWv",
							long_options, &optindex)) != -1)
	{
		char	   *endptr;

		switch (c)
		{
			case 'a':
				opts.alldb = true;
				break;
			case 'd':
				opts.dbpattern = true;
				append_database_pattern(&opts.include, optarg, encoding);
				break;
			case 'D':
				opts.dbpattern = true;
				append_database_pattern(&opts.exclude, optarg, encoding);
				break;
			case 'e':
				opts.echo = true;
				break;
			case 'h':
				host = pg_strdup(optarg);
				break;
			case 'i':
				opts.allrel = false;
				append_btree_pattern(&opts.include, optarg, encoding);
				break;
			case 'I':
				opts.excludeidx = true;
				append_btree_pattern(&opts.exclude, optarg, encoding);
				break;
			case 'j':
				opts.jobs = atoi(optarg);
				if (opts.jobs < 1)
				{
					fprintf(stderr,
							"number of parallel jobs must be at least 1\n");
					exit(1);
				}
				break;
			case 'p':
				port = pg_strdup(optarg);
				break;
			case 'P':
				opts.show_progress = true;
				break;
			case 'q':
				opts.quiet = true;
				break;
			case 'r':
				opts.allrel = false;
				append_relation_pattern(&opts.include, optarg, encoding);
				break;
			case 'R':
				opts.excludeidx = true;
				opts.excludetbl = true;
				append_relation_pattern(&opts.exclude, optarg, encoding);
				break;
			case 's':
				opts.allrel = false;
				append_schema_pattern(&opts.include, optarg, encoding);
				break;
			case 'S':
				opts.excludensp = true;
				append_schema_pattern(&opts.exclude, optarg, encoding);
				break;
			case 't':
				opts.allrel = false;
				append_heap_pattern(&opts.include, optarg, encoding);
				break;
			case 'T':
				opts.excludetbl = true;
				append_heap_pattern(&opts.exclude, optarg, encoding);
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
			case 'v':
				opts.verbose = true;
				pg_logging_increase_verbosity();
				break;
			case 1:
				maintenance_db = pg_strdup(optarg);
				break;
			case 2:
				opts.no_btree_expansion = true;
				break;
			case 3:
				opts.no_toast_expansion = true;
				break;
			case 4:
				opts.reconcile_toast = false;
				break;
			case 5:
				opts.on_error_stop = true;
				break;
			case 6:
				if (pg_strcasecmp(optarg, "all-visible") == 0)
					opts.skip = "all visible";
				else if (pg_strcasecmp(optarg, "all-frozen") == 0)
					opts.skip = "all frozen";
				else
				{
					fprintf(stderr, "invalid skip option\n");
					exit(1);
				}
				break;
			case 7:
				opts.startblock = strtol(optarg, &endptr, 10);
				if (*endptr != '\0')
				{
					fprintf(stderr,
							"invalid start block\n");
					exit(1);
				}
				if (opts.startblock > MaxBlockNumber || opts.startblock < 0)
				{
					fprintf(stderr,
							"start block out of bounds\n");
					exit(1);
				}
				break;
			case 8:
				opts.endblock = strtol(optarg, &endptr, 10);
				if (*endptr != '\0')
				{
					fprintf(stderr,
							"invalid end block\n");
					exit(1);
				}
				if (opts.endblock > MaxBlockNumber || opts.endblock < 0)
				{
					fprintf(stderr,
							"end block out of bounds\n");
					exit(1);
				}
				break;
			case 9:
				opts.rootdescend = true;
				opts.parent_check = true;
				break;
			case 10:
				opts.strict_names = false;
				break;
			case 11:
				opts.heapallindexed = true;
				break;
			case 12:
				opts.parent_check = true;
				break;
			default:
				fprintf(stderr,
						"Try \"%s --help\" for more information.\n",
						progname);
				exit(1);
		}
	}

	if (opts.endblock >= 0 && opts.endblock < opts.startblock)
	{
		fprintf(stderr,
				"end block precedes start block\n");
		exit(1);
	}

	/*
	 * A single non-option arguments specifies a database name or connection
	 * string.
	 */
	if (optind < argc)
	{
		db = argv[optind];
		optind++;
	}

	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
		exit(1);
	}

	/* fill cparams except for dbname, which is set below */
	cparams.pghost = host;
	cparams.pgport = port;
	cparams.pguser = username;
	cparams.prompt_password = prompt_password;
	cparams.dbname = NULL;
	cparams.override_dbname = NULL;

	setup_cancel_handler(NULL);

	/* choose the database for our initial connection */
	if (opts.alldb)
	{
		if (db != NULL)
		{
			pg_log_error("cannot specify a database name with --all");
			exit(1);
		}
		cparams.dbname = maintenance_db;
	}
	else if (db != NULL)
	{
		if (opts.dbpattern)
		{
			pg_log_error("cannot specify both a database name and database patterns");
			exit(1);
		}
		cparams.dbname = db;
	}

	if (opts.alldb || opts.dbpattern)
	{
		conn = connectMaintenanceDatabase(&cparams, progname, opts.echo);
		compile_database_list(conn, &databases, NULL);
	}
	else
	{
		if (cparams.dbname == NULL)
		{
			if (getenv("PGDATABASE"))
				cparams.dbname = getenv("PGDATABASE");
			else if (getenv("PGUSER"))
				cparams.dbname = getenv("PGUSER");
			else
				cparams.dbname = get_user_name_or_exit(progname);
		}
		conn = connectDatabase(&cparams, progname, opts.echo, false, true);
		compile_database_list(conn, &databases, PQdb(conn));
	}

	if (databases.head == NULL)
	{
		if (conn != NULL)
			disconnectDatabase(conn);
		pg_log_error("no databases to check");
		exit(0);
	}

	/*
	 * Compile a list of all relations spanning all databases to be checked.
	 */
	for (cell = databases.head; cell; cell = cell->next)
	{
		PGresult   *result;
		int			ntups;
		const char *amcheck_schema = NULL;
		DatabaseInfo *dat = (DatabaseInfo *) cell->ptr;

		cparams.override_dbname = dat->datname;
		if (conn == NULL || strcmp(PQdb(conn), dat->datname) != 0)
		{
			if (conn != NULL)
				disconnectDatabase(conn);
			conn = connectDatabase(&cparams, progname, opts.echo, false, true);
		}

		/*
		 * Verify that amcheck is installed for this next database.  User
		 * error could result in a database not having amcheck that should
		 * have it, but we also could be iterating over multiple databases
		 * where not all of them have amcheck installed (for example,
		 * 'template1').
		 */
		result = executeQuery(conn, amcheck_sql, opts.echo);
		if (PQresultStatus(result) != PGRES_TUPLES_OK)
		{
			/* Querying the catalog failed. */
			pg_log_error("database \"%s\": %s",
						 PQdb(conn), PQerrorMessage(conn));
			pg_log_info("query was: %s", amcheck_sql);
			PQclear(result);
			disconnectDatabase(conn);
			exit(1);
		}
		ntups = PQntuples(result);
		if (ntups == 0)
		{
			/* Querying the catalog succeeded, but amcheck is missing. */
			pg_log_warning("skipping database \"%s\": amcheck is not installed",
						   PQdb(conn));
			disconnectDatabase(conn);
			conn = NULL;
			continue;
		}
		amcheck_schema = PQgetvalue(result, 0, 0);
		if (opts.verbose)
			pg_log_info("in database \"%s\": using amcheck version \"%s\" in schema \"%s\"",
						PQdb(conn), PQgetvalue(result, 0, 1), amcheck_schema);
		dat->amcheck_schema = PQescapeIdentifier(conn, amcheck_schema,
												 strlen(amcheck_schema));
		PQclear(result);

		compile_relation_list_one_db(conn, &relations, dat, &pagestotal);
	}

	/*
	 * Check that all inclusion patterns matched at least one schema or
	 * relation that we can check.
	 */
	for (pattern_id = 0; pattern_id < opts.include.len; pattern_id++)
	{
		PatternInfo *pat = &opts.include.data[pattern_id];

		if (!pat->matched && (pat->nsp_regex != NULL || pat->rel_regex != NULL))
		{
			failed = opts.strict_names;

			if (!opts.quiet || failed)
			{
				if (pat->heap_only)
					log_no_match("no heap tables to check matching \"%s\"",
								 pat->pattern);
				else if (pat->btree_only)
					log_no_match("no btree indexes to check matching \"%s\"",
								 pat->pattern);
				else if (pat->rel_regex == NULL)
					log_no_match("no relations to check in schemas matching \"%s\"",
								 pat->pattern);
				else
					log_no_match("no relations to check matching \"%s\"",
								 pat->pattern);
			}
		}
	}

	if (failed)
	{
		if (conn != NULL)
			disconnectDatabase(conn);
		exit(1);
	}

	/*
	 * Set parallel_workers to the lesser of opts.jobs and the number of
	 * relations.
	 */
	parallel_workers = 0;
	for (cell = relations.head; cell; cell = cell->next)
	{
		reltotal++;
		if (parallel_workers < opts.jobs)
			parallel_workers++;
	}

	if (reltotal == 0)
	{
		if (conn != NULL)
			disconnectDatabase(conn);
		pg_log_error("no relations to check");
		exit(1);
	}
	progress_report(reltotal, relprogress, pagestotal, pageschecked,
					NULL, true, false);

	/*
	 * Main event loop.
	 *
	 * We use server-side parallelism to check up to parallel_workers
	 * relations in parallel.  The list of relations was computed in database
	 * order, which minimizes the number of connects and disconnects as we
	 * process the list.
	 */
	latest_datname = NULL;
	sa = ParallelSlotsSetup(parallel_workers, &cparams, progname, opts.echo,
							NULL);
	if (conn != NULL)
	{
		ParallelSlotsAdoptConn(sa, conn);
		conn = NULL;
	}

	initPQExpBuffer(&sql);
	for (relprogress = 0, cell = relations.head; cell; cell = cell->next)
	{
		ParallelSlot *free_slot;
		RelationInfo *rel;

		rel = (RelationInfo *) cell->ptr;

		if (CancelRequested)
		{
			failed = true;
			break;
		}

		/*
		 * The list of relations is in database sorted order.  If this next
		 * relation is in a different database than the last one seen, we are
		 * about to start checking this database.  Note that other slots may
		 * still be working on relations from prior databases.
		 */
		latest_datname = rel->datinfo->datname;

		progress_report(reltotal, relprogress, pagestotal, pageschecked,
						latest_datname, false, false);

		relprogress++;
		pageschecked += rel->blocks_to_check;

		/*
		 * Get a parallel slot for the next amcheck command, blocking if
		 * necessary until one is available, or until a previously issued slot
		 * command fails, indicating that we should abort checking the
		 * remaining objects.
		 */
		free_slot = ParallelSlotsGetIdle(sa, rel->datinfo->datname);
		if (!free_slot)
		{
			/*
			 * Something failed.  We don't need to know what it was, because
			 * the handler should already have emitted the necessary error
			 * messages.
			 */
			failed = true;
			break;
		}

		if (opts.verbose)
			PQsetErrorVerbosity(free_slot->connection, PQERRORS_VERBOSE);
		else if (opts.quiet)
			PQsetErrorVerbosity(free_slot->connection, PQERRORS_TERSE);

		/*
		 * Execute the appropriate amcheck command for this relation using our
		 * slot's database connection.  We do not wait for the command to
		 * complete, nor do we perform any error checking, as that is done by
		 * the parallel slots and our handler callback functions.
		 */
		if (rel->is_heap)
		{
			if (opts.verbose)
			{
				if (opts.show_progress && progress_since_last_stderr)
					fprintf(stderr, "\n");
				pg_log_info("checking heap table \"%s\".\"%s\".\"%s\"",
							rel->datinfo->datname, rel->nspname, rel->relname);
				progress_since_last_stderr = false;
			}
			prepare_heap_command(&sql, rel, free_slot->connection);
			rel->sql = pstrdup(sql.data);	/* pg_free'd after command */
			ParallelSlotSetHandler(free_slot, verify_heap_slot_handler, rel);
			run_command(free_slot, rel->sql);
		}
		else
		{
			if (opts.verbose)
			{
				if (opts.show_progress && progress_since_last_stderr)
					fprintf(stderr, "\n");

				pg_log_info("checking btree index \"%s\".\"%s\".\"%s\"",
							rel->datinfo->datname, rel->nspname, rel->relname);
				progress_since_last_stderr = false;
			}
			prepare_btree_command(&sql, rel, free_slot->connection);
			rel->sql = pstrdup(sql.data);	/* pg_free'd after command */
			ParallelSlotSetHandler(free_slot, verify_btree_slot_handler, rel);
			run_command(free_slot, rel->sql);
		}
	}
	termPQExpBuffer(&sql);

	if (!failed)
	{

		/*
		 * Wait for all slots to complete, or for one to indicate that an
		 * error occurred.  Like above, we rely on the handler emitting the
		 * necessary error messages.
		 */
		if (sa && !ParallelSlotsWaitCompletion(sa))
			failed = true;

		progress_report(reltotal, relprogress, pagestotal, pageschecked, NULL, true, true);
	}

	if (sa)
	{
		ParallelSlotsTerminate(sa);
		FREE_AND_SET_NULL(sa);
	}

	if (failed)
		exit(1);

	if (!all_checks_pass)
		exit(2);
}

/*
 * prepare_heap_command
 *
 * Creates a SQL command for running amcheck checking on the given heap
 * relation.  The command is phrased as a SQL query, with column order and
 * names matching the expectations of verify_heap_slot_handler, which will
 * receive and handle each row returned from the verify_heapam() function.
 *
 * sql: buffer into which the heap table checking command will be written
 * rel: relation information for the heap table to be checked
 * conn: the connection to be used, for string escaping purposes
 */
static void
prepare_heap_command(PQExpBuffer sql, RelationInfo *rel, PGconn *conn)
{
	resetPQExpBuffer(sql);
	appendPQExpBuffer(sql,
					  "SELECT blkno, offnum, attnum, msg FROM %s.verify_heapam("
					  "\nrelation := %u, on_error_stop := %s, check_toast := %s, skip := '%s'",
					  rel->datinfo->amcheck_schema,
					  rel->reloid,
					  opts.on_error_stop ? "true" : "false",
					  opts.reconcile_toast ? "true" : "false",
					  opts.skip);

	if (opts.startblock >= 0)
		appendPQExpBuffer(sql, ", startblock := " INT64_FORMAT, opts.startblock);
	if (opts.endblock >= 0)
		appendPQExpBuffer(sql, ", endblock := " INT64_FORMAT, opts.endblock);

	appendPQExpBuffer(sql, ")");
}

/*
 * prepare_btree_command
 *
 * Creates a SQL command for running amcheck checking on the given btree index
 * relation.  The command does not select any columns, as btree checking
 * functions do not return any, but rather return corruption information by
 * raising errors, which verify_btree_slot_handler expects.
 *
 * sql: buffer into which the heap table checking command will be written
 * rel: relation information for the index to be checked
 * conn: the connection to be used, for string escaping purposes
 */
static void
prepare_btree_command(PQExpBuffer sql, RelationInfo *rel, PGconn *conn)
{
	resetPQExpBuffer(sql);

	/*
	 * Embed the database, schema, and relation name in the query, so if the
	 * check throws an error, the user knows which relation the error came
	 * from.
	 */
	if (opts.parent_check)
		appendPQExpBuffer(sql,
						  "SELECT * FROM %s.bt_index_parent_check("
						  "index := '%u'::regclass, heapallindexed := %s, "
						  "rootdescend := %s)",
						  rel->datinfo->amcheck_schema,
						  rel->reloid,
						  (opts.heapallindexed ? "true" : "false"),
						  (opts.rootdescend ? "true" : "false"));
	else
		appendPQExpBuffer(sql,
						  "SELECT * FROM %s.bt_index_check("
						  "index := '%u'::regclass, heapallindexed := %s)",
						  rel->datinfo->amcheck_schema,
						  rel->reloid,
						  (opts.heapallindexed ? "true" : "false"));
}

/*
 * run_command
 *
 * Sends a command to the server without waiting for the command to complete.
 * Logs an error if the command cannot be sent, but otherwise any errors are
 * expected to be handled by a ParallelSlotHandler.
 *
 * If reconnecting to the database is necessary, the cparams argument may be
 * modified.
 *
 * slot: slot with connection to the server we should use for the command
 * sql: query to send
 */
static void
run_command(ParallelSlot *slot, const char *sql)
{
	if (opts.echo)
		printf("%s\n", sql);

	if (PQsendQuery(slot->connection, sql) == 0)
	{
		pg_log_error("error sending command to database \"%s\": %s",
					 PQdb(slot->connection),
					 PQerrorMessage(slot->connection));
		pg_log_error("command was: %s", sql);
		exit(1);
	}
}

/*
 * should_processing_continue
 *
 * Checks a query result returned from a query (presumably issued on a slot's
 * connection) to determine if parallel slots should continue issuing further
 * commands.
 *
 * Note: Heap relation corruption is reported by verify_heapam() via the result
 * set, rather than an ERROR, but running verify_heapam() on a corrupted heap
 * table may still result in an error being returned from the server due to
 * missing relation files, bad checksums, etc.  The btree corruption checking
 * functions always use errors to communicate corruption messages.  We can't
 * just abort processing because we got a mere ERROR.
 *
 * res: result from an executed sql query
 */
static bool
should_processing_continue(PGresult *res)
{
	const char *severity;

	switch (PQresultStatus(res))
	{
			/* These are expected and ok */
		case PGRES_COMMAND_OK:
		case PGRES_TUPLES_OK:
		case PGRES_NONFATAL_ERROR:
			break;

			/* This is expected but requires closer scrutiny */
		case PGRES_FATAL_ERROR:
			severity = PQresultErrorField(res, PG_DIAG_SEVERITY_NONLOCALIZED);
			if (strcmp(severity, "FATAL") == 0)
				return false;
			if (strcmp(severity, "PANIC") == 0)
				return false;
			break;

			/* These are unexpected */
		case PGRES_BAD_RESPONSE:
		case PGRES_EMPTY_QUERY:
		case PGRES_COPY_OUT:
		case PGRES_COPY_IN:
		case PGRES_COPY_BOTH:
		case PGRES_SINGLE_TUPLE:
		case PGRES_PIPELINE_SYNC:
		case PGRES_PIPELINE_ABORTED:
			return false;
	}
	return true;
}

/*
 * Returns a copy of the argument string with all lines indented four spaces.
 *
 * The caller should pg_free the result when finished with it.
 */
static char *
indent_lines(const char *str)
{
	PQExpBufferData buf;
	const char *c;
	char	   *result;

	initPQExpBuffer(&buf);
	appendPQExpBufferStr(&buf, "    ");
	for (c = str; *c; c++)
	{
		appendPQExpBufferChar(&buf, *c);
		if (c[0] == '\n' && c[1] != '\0')
			appendPQExpBufferStr(&buf, "    ");
	}
	result = pstrdup(buf.data);
	termPQExpBuffer(&buf);

	return result;
}

/*
 * verify_heap_slot_handler
 *
 * ParallelSlotHandler that receives results from a heap table checking command
 * created by prepare_heap_command and outputs the results for the user.
 *
 * res: result from an executed sql query
 * conn: connection on which the sql query was executed
 * context: the sql query being handled, as a cstring
 */
static bool
verify_heap_slot_handler(PGresult *res, PGconn *conn, void *context)
{
	RelationInfo *rel = (RelationInfo *) context;

	if (PQresultStatus(res) == PGRES_TUPLES_OK)
	{
		int			i;
		int			ntups = PQntuples(res);

		if (ntups > 0)
			all_checks_pass = false;

		for (i = 0; i < ntups; i++)
		{
			const char *msg;

			/* The message string should never be null, but check */
			if (PQgetisnull(res, i, 3))
				msg = "NO MESSAGE";
			else
				msg = PQgetvalue(res, i, 3);

			if (!PQgetisnull(res, i, 2))
				printf("heap table \"%s\".\"%s\".\"%s\", block %s, offset %s, attribute %s:\n    %s\n",
					   rel->datinfo->datname, rel->nspname, rel->relname,
					   PQgetvalue(res, i, 0),	/* blkno */
					   PQgetvalue(res, i, 1),	/* offnum */
					   PQgetvalue(res, i, 2),	/* attnum */
					   msg);

			else if (!PQgetisnull(res, i, 1))
				printf("heap table \"%s\".\"%s\".\"%s\", block %s, offset %s:\n    %s\n",
					   rel->datinfo->datname, rel->nspname, rel->relname,
					   PQgetvalue(res, i, 0),	/* blkno */
					   PQgetvalue(res, i, 1),	/* offnum */
					   msg);

			else if (!PQgetisnull(res, i, 0))
				printf("heap table \"%s\".\"%s\".\"%s\", block %s:\n    %s\n",
					   rel->datinfo->datname, rel->nspname, rel->relname,
					   PQgetvalue(res, i, 0),	/* blkno */
					   msg);

			else
				printf("heap table \"%s\".\"%s\".\"%s\":\n    %s\n",
					   rel->datinfo->datname, rel->nspname, rel->relname, msg);
		}
	}
	else if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		char	   *msg = indent_lines(PQerrorMessage(conn));

		all_checks_pass = false;
		printf("heap table \"%s\".\"%s\".\"%s\":\n%s",
			   rel->datinfo->datname, rel->nspname, rel->relname, msg);
		if (opts.verbose)
			printf("query was: %s\n", rel->sql);
		FREE_AND_SET_NULL(msg);
	}

	FREE_AND_SET_NULL(rel->sql);
	FREE_AND_SET_NULL(rel->nspname);
	FREE_AND_SET_NULL(rel->relname);

	return should_processing_continue(res);
}

/*
 * verify_btree_slot_handler
 *
 * ParallelSlotHandler that receives results from a btree checking command
 * created by prepare_btree_command and outputs them for the user.  The results
 * from the btree checking command is assumed to be empty, but when the results
 * are an error code, the useful information about the corruption is expected
 * in the connection's error message.
 *
 * res: result from an executed sql query
 * conn: connection on which the sql query was executed
 * context: unused
 */
static bool
verify_btree_slot_handler(PGresult *res, PGconn *conn, void *context)
{
	RelationInfo *rel = (RelationInfo *) context;

	if (PQresultStatus(res) == PGRES_TUPLES_OK)
	{
		int			ntups = PQntuples(res);

		if (ntups != 1)
		{
			/*
			 * We expect the btree checking functions to return one void row
			 * each, so we should output some sort of warning if we get
			 * anything else, not because it indicates corruption, but because
			 * it suggests a mismatch between amcheck and pg_amcheck versions.
			 *
			 * In conjunction with --progress, anything written to stderr at
			 * this time would present strangely to the user without an extra
			 * newline, so we print one.  If we were multithreaded, we'd have
			 * to avoid splitting this across multiple calls, but we're in an
			 * event loop, so it doesn't matter.
			 */
			if (opts.show_progress && progress_since_last_stderr)
				fprintf(stderr, "\n");
			pg_log_warning("btree index \"%s\".\"%s\".\"%s\": btree checking function returned unexpected number of rows: %d",
						   rel->datinfo->datname, rel->nspname, rel->relname, ntups);
			if (opts.verbose)
				pg_log_info("query was: %s", rel->sql);
			pg_log_warning("are %s's and amcheck's versions compatible?",
						   progname);
			progress_since_last_stderr = false;
		}
	}
	else
	{
		char	   *msg = indent_lines(PQerrorMessage(conn));

		all_checks_pass = false;
		printf("btree index \"%s\".\"%s\".\"%s\":\n%s",
			   rel->datinfo->datname, rel->nspname, rel->relname, msg);
		if (opts.verbose)
			printf("query was: %s\n", rel->sql);
		FREE_AND_SET_NULL(msg);
	}

	FREE_AND_SET_NULL(rel->sql);
	FREE_AND_SET_NULL(rel->nspname);
	FREE_AND_SET_NULL(rel->relname);

	return should_processing_continue(res);
}

/*
 * help
 *
 * Prints help page for the program
 *
 * progname: the name of the executed program, such as "pg_amcheck"
 */
static void
help(const char *progname)
{
	printf("%s uses amcheck module to check objects in a PostgreSQL database for corruption.\n\n", progname);
	printf("Usage:\n");
	printf("  %s [OPTION]... [DBNAME]\n", progname);
	printf("\nTarget Options:\n");
	printf("  -a, --all                      check all databases\n");
	printf("  -d, --database=PATTERN         check matching database(s)\n");
	printf("  -D, --exclude-database=PATTERN do NOT check matching database(s)\n");
	printf("  -i, --index=PATTERN            check matching index(es)\n");
	printf("  -I, --exclude-index=PATTERN    do NOT check matching index(es)\n");
	printf("  -r, --relation=PATTERN         check matching relation(s)\n");
	printf("  -R, --exclude-relation=PATTERN do NOT check matching relation(s)\n");
	printf("  -s, --schema=PATTERN           check matching schema(s)\n");
	printf("  -S, --exclude-schema=PATTERN   do NOT check matching schema(s)\n");
	printf("  -t, --table=PATTERN            check matching table(s)\n");
	printf("  -T, --exclude-table=PATTERN    do NOT check matching table(s)\n");
	printf("      --no-dependent-indexes     do NOT expand list of relations to include indexes\n");
	printf("      --no-dependent-toast       do NOT expand list of relations to include toast\n");
	printf("      --no-strict-names          do NOT require patterns to match objects\n");
	printf("\nTable Checking Options:\n");
	printf("      --exclude-toast-pointers   do NOT follow relation toast pointers\n");
	printf("      --on-error-stop            stop checking at end of first corrupt page\n");
	printf("      --skip=OPTION              do NOT check \"all-frozen\" or \"all-visible\" blocks\n");
	printf("      --startblock=BLOCK         begin checking table(s) at the given block number\n");
	printf("      --endblock=BLOCK           check table(s) only up to the given block number\n");
	printf("\nBtree Index Checking Options:\n");
	printf("      --heapallindexed           check all heap tuples are found within indexes\n");
	printf("      --parent-check             check index parent/child relationships\n");
	printf("      --rootdescend              search from root page to refind tuples\n");
	printf("\nConnection options:\n");
	printf("  -h, --host=HOSTNAME            database server host or socket directory\n");
	printf("  -p, --port=PORT                database server port\n");
	printf("  -U, --username=USERNAME        user name to connect as\n");
	printf("  -w, --no-password              never prompt for password\n");
	printf("  -W, --password                 force password prompt\n");
	printf("      --maintenance-db=DBNAME    alternate maintenance database\n");
	printf("\nOther Options:\n");
	printf("  -e, --echo                     show the commands being sent to the server\n");
	printf("  -j, --jobs=NUM                 use this many concurrent connections to the server\n");
	printf("  -q, --quiet                    don't write any messages\n");
	printf("  -v, --verbose                  write a lot of output\n");
	printf("  -V, --version                  output version information, then exit\n");
	printf("  -P, --progress                 show progress information\n");
	printf("  -?, --help                     show this help, then exit\n");

	printf("\nReport bugs to <%s>.\n", PACKAGE_BUGREPORT);
	printf("%s home page: <%s>\n", PACKAGE_NAME, PACKAGE_URL);
}

/*
 * Print a progress report based on the global variables.
 *
 * Progress report is written at maximum once per second, unless the force
 * parameter is set to true.
 *
 * If finished is set to true, this is the last progress report. The cursor
 * is moved to the next line.
 */
static void
progress_report(uint64 relations_total, uint64 relations_checked,
				uint64 relpages_total, uint64 relpages_checked,
				const char *datname, bool force, bool finished)
{
	int			percent_rel = 0;
	int			percent_pages = 0;
	char		checked_rel[32];
	char		total_rel[32];
	char		checked_pages[32];
	char		total_pages[32];
	pg_time_t	now;

	if (!opts.show_progress)
		return;

	now = time(NULL);
	if (now == last_progress_report && !force && !finished)
		return;					/* Max once per second */

	last_progress_report = now;
	if (relations_total)
		percent_rel = (int) (relations_checked * 100 / relations_total);
	if (relpages_total)
		percent_pages = (int) (relpages_checked * 100 / relpages_total);

	/*
	 * Separate step to keep platform-dependent format code out of fprintf
	 * calls.  We only test for INT64_FORMAT availability in snprintf, not
	 * fprintf.
	 */
	snprintf(checked_rel, sizeof(checked_rel), INT64_FORMAT, relations_checked);
	snprintf(total_rel, sizeof(total_rel), INT64_FORMAT, relations_total);
	snprintf(checked_pages, sizeof(checked_pages), INT64_FORMAT, relpages_checked);
	snprintf(total_pages, sizeof(total_pages), INT64_FORMAT, relpages_total);

#define VERBOSE_DATNAME_LENGTH 35
	if (opts.verbose)
	{
		if (!datname)

			/*
			 * No datname given, so clear the status line (used for first and
			 * last call)
			 */
			fprintf(stderr,
					"%*s/%s relations (%d%%) %*s/%s pages (%d%%) %*s",
					(int) strlen(total_rel),
					checked_rel, total_rel, percent_rel,
					(int) strlen(total_pages),
					checked_pages, total_pages, percent_pages,
					VERBOSE_DATNAME_LENGTH + 2, "");
		else
		{
			bool		truncate = (strlen(datname) > VERBOSE_DATNAME_LENGTH);

			fprintf(stderr,
					"%*s/%s relations (%d%%) %*s/%s pages (%d%%), (%s%-*.*s)",
					(int) strlen(total_rel),
					checked_rel, total_rel, percent_rel,
					(int) strlen(total_pages),
					checked_pages, total_pages, percent_pages,
			/* Prefix with "..." if we do leading truncation */
					truncate ? "..." : "",
					truncate ? VERBOSE_DATNAME_LENGTH - 3 : VERBOSE_DATNAME_LENGTH,
					truncate ? VERBOSE_DATNAME_LENGTH - 3 : VERBOSE_DATNAME_LENGTH,
			/* Truncate datname at beginning if it's too long */
					truncate ? datname + strlen(datname) - VERBOSE_DATNAME_LENGTH + 3 : datname);
		}
	}
	else
		fprintf(stderr,
				"%*s/%s relations (%d%%) %*s/%s pages (%d%%)",
				(int) strlen(total_rel),
				checked_rel, total_rel, percent_rel,
				(int) strlen(total_pages),
				checked_pages, total_pages, percent_pages);

	/*
	 * Stay on the same line if reporting to a terminal and we're not done
	 * yet.
	 */
	if (!finished && isatty(fileno(stderr)))
	{
		fputc('\r', stderr);
		progress_since_last_stderr = true;
	}
	else
		fputc('\n', stderr);
}

/*
 * Extend the pattern info array to hold one additional initialized pattern
 * info entry.
 *
 * Returns a pointer to the new entry.
 */
static PatternInfo *
extend_pattern_info_array(PatternInfoArray *pia)
{
	PatternInfo *result;

	pia->len++;
	pia->data = (PatternInfo *) pg_realloc(pia->data, pia->len * sizeof(PatternInfo));
	result = &pia->data[pia->len - 1];
	memset(result, 0, sizeof(*result));

	return result;
}

/*
 * append_database_pattern
 *
 * Adds the given pattern interpreted as a database name pattern.
 *
 * pia: the pattern info array to be appended
 * pattern: the database name pattern
 * encoding: client encoding for parsing the pattern
 */
static void
append_database_pattern(PatternInfoArray *pia, const char *pattern, int encoding)
{
	PQExpBufferData buf;
	PatternInfo *info = extend_pattern_info_array(pia);

	initPQExpBuffer(&buf);
	patternToSQLRegex(encoding, NULL, NULL, &buf, pattern, false);
	info->pattern = pattern;
	info->db_regex = pstrdup(buf.data);

	termPQExpBuffer(&buf);
}

/*
 * append_schema_pattern
 *
 * Adds the given pattern interpreted as a schema name pattern.
 *
 * pia: the pattern info array to be appended
 * pattern: the schema name pattern
 * encoding: client encoding for parsing the pattern
 */
static void
append_schema_pattern(PatternInfoArray *pia, const char *pattern, int encoding)
{
	PQExpBufferData dbbuf;
	PQExpBufferData nspbuf;
	PatternInfo *info = extend_pattern_info_array(pia);

	initPQExpBuffer(&dbbuf);
	initPQExpBuffer(&nspbuf);

	patternToSQLRegex(encoding, NULL, &dbbuf, &nspbuf, pattern, false);
	info->pattern = pattern;
	if (dbbuf.data[0])
	{
		opts.dbpattern = true;
		info->db_regex = pstrdup(dbbuf.data);
	}
	if (nspbuf.data[0])
		info->nsp_regex = pstrdup(nspbuf.data);

	termPQExpBuffer(&dbbuf);
	termPQExpBuffer(&nspbuf);
}

/*
 * append_relation_pattern_helper
 *
 * Adds to a list the given pattern interpreted as a relation pattern.
 *
 * pia: the pattern info array to be appended
 * pattern: the relation name pattern
 * encoding: client encoding for parsing the pattern
 * heap_only: whether the pattern should only be matched against heap tables
 * btree_only: whether the pattern should only be matched against btree indexes
 */
static void
append_relation_pattern_helper(PatternInfoArray *pia, const char *pattern,
							   int encoding, bool heap_only, bool btree_only)
{
	PQExpBufferData dbbuf;
	PQExpBufferData nspbuf;
	PQExpBufferData relbuf;
	PatternInfo *info = extend_pattern_info_array(pia);

	initPQExpBuffer(&dbbuf);
	initPQExpBuffer(&nspbuf);
	initPQExpBuffer(&relbuf);

	patternToSQLRegex(encoding, &dbbuf, &nspbuf, &relbuf, pattern, false);
	info->pattern = pattern;
	if (dbbuf.data[0])
	{
		opts.dbpattern = true;
		info->db_regex = pstrdup(dbbuf.data);
	}
	if (nspbuf.data[0])
		info->nsp_regex = pstrdup(nspbuf.data);
	if (relbuf.data[0])
		info->rel_regex = pstrdup(relbuf.data);

	termPQExpBuffer(&dbbuf);
	termPQExpBuffer(&nspbuf);
	termPQExpBuffer(&relbuf);

	info->heap_only = heap_only;
	info->btree_only = btree_only;
}

/*
 * append_relation_pattern
 *
 * Adds the given pattern interpreted as a relation pattern, to be matched
 * against both heap tables and btree indexes.
 *
 * pia: the pattern info array to be appended
 * pattern: the relation name pattern
 * encoding: client encoding for parsing the pattern
 */
static void
append_relation_pattern(PatternInfoArray *pia, const char *pattern, int encoding)
{
	append_relation_pattern_helper(pia, pattern, encoding, false, false);
}

/*
 * append_heap_pattern
 *
 * Adds the given pattern interpreted as a relation pattern, to be matched only
 * against heap tables.
 *
 * pia: the pattern info array to be appended
 * pattern: the relation name pattern
 * encoding: client encoding for parsing the pattern
 */
static void
append_heap_pattern(PatternInfoArray *pia, const char *pattern, int encoding)
{
	append_relation_pattern_helper(pia, pattern, encoding, true, false);
}

/*
 * append_btree_pattern
 *
 * Adds the given pattern interpreted as a relation pattern, to be matched only
 * against btree indexes.
 *
 * pia: the pattern info array to be appended
 * pattern: the relation name pattern
 * encoding: client encoding for parsing the pattern
 */
static void
append_btree_pattern(PatternInfoArray *pia, const char *pattern, int encoding)
{
	append_relation_pattern_helper(pia, pattern, encoding, false, true);
}

/*
 * append_db_pattern_cte
 *
 * Appends to the buffer the body of a Common Table Expression (CTE) containing
 * the database portions filtered from the list of patterns expressed as two
 * columns:
 *
 *     pattern_id: the index of this pattern in pia->data[]
 *     rgx: the database regular expression parsed from the pattern
 *
 * Patterns without a database portion are skipped.  Patterns with more than
 * just a database portion are optionally skipped, depending on argument
 * 'inclusive'.
 *
 * buf: the buffer to be appended
 * pia: the array of patterns to be inserted into the CTE
 * conn: the database connection
 * inclusive: whether to include patterns with schema and/or relation parts
 *
 * Returns whether any database patterns were appended.
 */
static bool
append_db_pattern_cte(PQExpBuffer buf, const PatternInfoArray *pia,
					  PGconn *conn, bool inclusive)
{
	int			pattern_id;
	const char *comma;
	bool		have_values;

	comma = "";
	have_values = false;
	for (pattern_id = 0; pattern_id < pia->len; pattern_id++)
	{
		PatternInfo *info = &pia->data[pattern_id];

		if (info->db_regex != NULL &&
			(inclusive || (info->nsp_regex == NULL && info->rel_regex == NULL)))
		{
			if (!have_values)
				appendPQExpBufferStr(buf, "\nVALUES");
			have_values = true;
			appendPQExpBuffer(buf, "%s\n(%d, ", comma, pattern_id);
			appendStringLiteralConn(buf, info->db_regex, conn);
			appendPQExpBufferStr(buf, ")");
			comma = ",";
		}
	}

	if (!have_values)
		appendPQExpBufferStr(buf, "\nSELECT NULL, NULL, NULL WHERE false");

	return have_values;
}

/*
 * compile_database_list
 *
 * If any database patterns exist, or if --all was given, compiles a distinct
 * list of databases to check using a SQL query based on the patterns plus the
 * literal initial database name, if given.  If no database patterns exist and
 * --all was not given, the query is not necessary, and only the initial
 * database name (if any) is added to the list.
 *
 * conn: connection to the initial database
 * databases: the list onto which databases should be appended
 * initial_dbname: an optional extra database name to include in the list
 */
static void
compile_database_list(PGconn *conn, SimplePtrList *databases,
					  const char *initial_dbname)
{
	PGresult   *res;
	PQExpBufferData sql;
	int			ntups;
	int			i;
	bool		fatal;

	if (initial_dbname)
	{
		DatabaseInfo *dat = (DatabaseInfo *) pg_malloc0(sizeof(DatabaseInfo));

		/* This database is included.  Add to list */
		if (opts.verbose)
			pg_log_info("including database: \"%s\"", initial_dbname);

		dat->datname = pstrdup(initial_dbname);
		simple_ptr_list_append(databases, dat);
	}

	initPQExpBuffer(&sql);

	/* Append the include patterns CTE. */
	appendPQExpBufferStr(&sql, "WITH include_raw (pattern_id, rgx) AS (");
	if (!append_db_pattern_cte(&sql, &opts.include, conn, true) &&
		!opts.alldb)
	{
		/*
		 * None of the inclusion patterns (if any) contain database portions,
		 * so there is no need to query the database to resolve database
		 * patterns.
		 *
		 * Since we're also not operating under --all, we don't need to query
		 * the exhaustive list of connectable databases, either.
		 */
		termPQExpBuffer(&sql);
		return;
	}

	/* Append the exclude patterns CTE. */
	appendPQExpBufferStr(&sql, "),\nexclude_raw (pattern_id, rgx) AS (");
	append_db_pattern_cte(&sql, &opts.exclude, conn, false);
	appendPQExpBufferStr(&sql, "),");

	/*
	 * Append the database CTE, which includes whether each database is
	 * connectable and also joins against exclude_raw to determine whether
	 * each database is excluded.
	 */
	appendPQExpBufferStr(&sql,
						 "\ndatabase (datname) AS ("
						 "\nSELECT d.datname "
						 "FROM pg_catalog.pg_database d "
						 "LEFT OUTER JOIN exclude_raw e "
						 "ON d.datname ~ e.rgx "
						 "\nWHERE d.datallowconn "
						 "AND e.pattern_id IS NULL"
						 "),"

	/*
	 * Append the include_pat CTE, which joins the include_raw CTE against the
	 * databases CTE to determine if all the inclusion patterns had matches,
	 * and whether each matched pattern had the misfortune of only matching
	 * excluded or unconnectable databases.
	 */
						 "\ninclude_pat (pattern_id, checkable) AS ("
						 "\nSELECT i.pattern_id, "
						 "COUNT(*) FILTER ("
						 "WHERE d IS NOT NULL"
						 ") AS checkable"
						 "\nFROM include_raw i "
						 "LEFT OUTER JOIN database d "
						 "ON d.datname ~ i.rgx"
						 "\nGROUP BY i.pattern_id"
						 "),"

	/*
	 * Append the filtered_databases CTE, which selects from the database CTE
	 * optionally joined against the include_raw CTE to only select databases
	 * that match an inclusion pattern.  This appears to duplicate what the
	 * include_pat CTE already did above, but here we want only databases, and
	 * there we wanted patterns.
	 */
						 "\nfiltered_databases (datname) AS ("
						 "\nSELECT DISTINCT d.datname "
						 "FROM database d");
	if (!opts.alldb)
		appendPQExpBufferStr(&sql,
							 " INNER JOIN include_raw i "
							 "ON d.datname ~ i.rgx");
	appendPQExpBufferStr(&sql,
						 ")"

	/*
	 * Select the checkable databases and the unmatched inclusion patterns.
	 */
						 "\nSELECT pattern_id, datname FROM ("
						 "\nSELECT pattern_id, NULL::TEXT AS datname "
						 "FROM include_pat "
						 "WHERE checkable = 0 "
						 "UNION ALL"
						 "\nSELECT NULL, datname "
						 "FROM filtered_databases"
						 ") AS combined_records"
						 "\nORDER BY pattern_id NULLS LAST, datname");

	res = executeQuery(conn, sql.data, opts.echo);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("query failed: %s", PQerrorMessage(conn));
		pg_log_info("query was: %s", sql.data);
		disconnectDatabase(conn);
		exit(1);
	}
	termPQExpBuffer(&sql);

	ntups = PQntuples(res);
	for (fatal = false, i = 0; i < ntups; i++)
	{
		int			pattern_id = -1;
		const char *datname = NULL;

		if (!PQgetisnull(res, i, 0))
			pattern_id = atoi(PQgetvalue(res, i, 0));
		if (!PQgetisnull(res, i, 1))
			datname = PQgetvalue(res, i, 1);

		if (pattern_id >= 0)
		{
			/*
			 * Current record pertains to an inclusion pattern that matched no
			 * checkable databases.
			 */
			fatal = opts.strict_names;
			if (pattern_id >= opts.include.len)
			{
				pg_log_error("internal error: received unexpected database pattern_id %d",
							 pattern_id);
				exit(1);
			}
			log_no_match("no connectable databases to check matching \"%s\"",
						 opts.include.data[pattern_id].pattern);
		}
		else
		{
			DatabaseInfo *dat;

			/* Current record pertains to a database */
			Assert(datname != NULL);

			/* Avoid entering a duplicate entry matching the initial_dbname */
			if (initial_dbname != NULL && strcmp(initial_dbname, datname) == 0)
				continue;

			/* This database is included.  Add to list */
			if (opts.verbose)
				pg_log_info("including database: \"%s\"", datname);

			dat = (DatabaseInfo *) pg_malloc0(sizeof(DatabaseInfo));
			dat->datname = pstrdup(datname);
			simple_ptr_list_append(databases, dat);
		}
	}
	PQclear(res);

	if (fatal)
	{
		if (conn != NULL)
			disconnectDatabase(conn);
		exit(1);
	}
}

/*
 * append_rel_pattern_raw_cte
 *
 * Appends to the buffer the body of a Common Table Expression (CTE) containing
 * the given patterns as six columns:
 *
 *     pattern_id: the index of this pattern in pia->data[]
 *     db_regex: the database regexp parsed from the pattern, or NULL if the
 *               pattern had no database part
 *     nsp_regex: the namespace regexp parsed from the pattern, or NULL if the
 *                pattern had no namespace part
 *     rel_regex: the relname regexp parsed from the pattern, or NULL if the
 *                pattern had no relname part
 *     heap_only: true if the pattern applies only to heap tables (not indexes)
 *     btree_only: true if the pattern applies only to btree indexes (not tables)
 *
 * buf: the buffer to be appended
 * patterns: the array of patterns to be inserted into the CTE
 * conn: the database connection
 */
static void
append_rel_pattern_raw_cte(PQExpBuffer buf, const PatternInfoArray *pia,
						   PGconn *conn)
{
	int			pattern_id;
	const char *comma;
	bool		have_values;

	comma = "";
	have_values = false;
	for (pattern_id = 0; pattern_id < pia->len; pattern_id++)
	{
		PatternInfo *info = &pia->data[pattern_id];

		if (!have_values)
			appendPQExpBufferStr(buf, "\nVALUES");
		have_values = true;
		appendPQExpBuffer(buf, "%s\n(%d::INTEGER, ", comma, pattern_id);
		if (info->db_regex == NULL)
			appendPQExpBufferStr(buf, "NULL");
		else
			appendStringLiteralConn(buf, info->db_regex, conn);
		appendPQExpBufferStr(buf, "::TEXT, ");
		if (info->nsp_regex == NULL)
			appendPQExpBufferStr(buf, "NULL");
		else
			appendStringLiteralConn(buf, info->nsp_regex, conn);
		appendPQExpBufferStr(buf, "::TEXT, ");
		if (info->rel_regex == NULL)
			appendPQExpBufferStr(buf, "NULL");
		else
			appendStringLiteralConn(buf, info->rel_regex, conn);
		if (info->heap_only)
			appendPQExpBufferStr(buf, "::TEXT, true::BOOLEAN");
		else
			appendPQExpBufferStr(buf, "::TEXT, false::BOOLEAN");
		if (info->btree_only)
			appendPQExpBufferStr(buf, ", true::BOOLEAN");
		else
			appendPQExpBufferStr(buf, ", false::BOOLEAN");
		appendPQExpBufferStr(buf, ")");
		comma = ",";
	}

	if (!have_values)
		appendPQExpBufferStr(buf,
							 "\nSELECT NULL::INTEGER, NULL::TEXT, NULL::TEXT, "
							 "NULL::TEXT, NULL::BOOLEAN, NULL::BOOLEAN "
							 "WHERE false");
}

/*
 * append_rel_pattern_filtered_cte
 *
 * Appends to the buffer a Common Table Expression (CTE) which selects
 * all patterns from the named raw CTE, filtered by database.  All patterns
 * which have no database portion or whose database portion matches our
 * connection's database name are selected, with other patterns excluded.
 *
 * The basic idea here is that if we're connected to database "foo" and we have
 * patterns "foo.bar.baz", "alpha.beta" and "one.two.three", we only want to
 * use the first two while processing relations in this database, as the third
 * one is not relevant.
 *
 * buf: the buffer to be appended
 * raw: the name of the CTE to select from
 * filtered: the name of the CTE to create
 * conn: the database connection
 */
static void
append_rel_pattern_filtered_cte(PQExpBuffer buf, const char *raw,
								const char *filtered, PGconn *conn)
{
	appendPQExpBuffer(buf,
					  "\n%s (pattern_id, nsp_regex, rel_regex, heap_only, btree_only) AS ("
					  "\nSELECT pattern_id, nsp_regex, rel_regex, heap_only, btree_only "
					  "FROM %s r"
					  "\nWHERE (r.db_regex IS NULL "
					  "OR ",
					  filtered, raw);
	appendStringLiteralConn(buf, PQdb(conn), conn);
	appendPQExpBufferStr(buf, " ~ r.db_regex)");
	appendPQExpBufferStr(buf,
						 " AND (r.nsp_regex IS NOT NULL"
						 " OR r.rel_regex IS NOT NULL)"
						 "),");
}

/*
 * compile_relation_list_one_db
 *
 * Compiles a list of relations to check within the currently connected
 * database based on the user supplied options, sorted by descending size,
 * and appends them to the given list of relations.
 *
 * The cells of the constructed list contain all information about the relation
 * necessary to connect to the database and check the object, including which
 * database to connect to, where contrib/amcheck is installed, and the Oid and
 * type of object (heap table vs. btree index).  Rather than duplicating the
 * database details per relation, the relation structs use references to the
 * same database object, provided by the caller.
 *
 * conn: connection to this next database, which should be the same as in 'dat'
 * relations: list onto which the relations information should be appended
 * dat: the database info struct for use by each relation
 * pagecount: gets incremented by the number of blocks to check in all
 * relations added
 */
static void
compile_relation_list_one_db(PGconn *conn, SimplePtrList *relations,
							 const DatabaseInfo *dat,
							 uint64 *pagecount)
{
	PGresult   *res;
	PQExpBufferData sql;
	int			ntups;
	int			i;

	initPQExpBuffer(&sql);
	appendPQExpBufferStr(&sql, "WITH");

	/* Append CTEs for the relation inclusion patterns, if any */
	if (!opts.allrel)
	{
		appendPQExpBufferStr(&sql,
							 " include_raw (pattern_id, db_regex, nsp_regex, rel_regex, heap_only, btree_only) AS (");
		append_rel_pattern_raw_cte(&sql, &opts.include, conn);
		appendPQExpBufferStr(&sql, "\n),");
		append_rel_pattern_filtered_cte(&sql, "include_raw", "include_pat", conn);
	}

	/* Append CTEs for the relation exclusion patterns, if any */
	if (opts.excludetbl || opts.excludeidx || opts.excludensp)
	{
		appendPQExpBufferStr(&sql,
							 " exclude_raw (pattern_id, db_regex, nsp_regex, rel_regex, heap_only, btree_only) AS (");
		append_rel_pattern_raw_cte(&sql, &opts.exclude, conn);
		appendPQExpBufferStr(&sql, "\n),");
		append_rel_pattern_filtered_cte(&sql, "exclude_raw", "exclude_pat", conn);
	}

	/* Append the relation CTE. */
	appendPQExpBufferStr(&sql,
						 " relation (pattern_id, oid, nspname, relname, reltoastrelid, relpages, is_heap, is_btree) AS ("
						 "\nSELECT DISTINCT ON (c.oid");
	if (!opts.allrel)
		appendPQExpBufferStr(&sql, ", ip.pattern_id) ip.pattern_id,");
	else
		appendPQExpBufferStr(&sql, ") NULL::INTEGER AS pattern_id,");
	appendPQExpBuffer(&sql,
					  "\nc.oid, n.nspname, c.relname, c.reltoastrelid, c.relpages, "
					  "c.relam = %u AS is_heap, "
					  "c.relam = %u AS is_btree"
					  "\nFROM pg_catalog.pg_class c "
					  "INNER JOIN pg_catalog.pg_namespace n "
					  "ON c.relnamespace = n.oid",
					  HEAP_TABLE_AM_OID, BTREE_AM_OID);
	if (!opts.allrel)
		appendPQExpBuffer(&sql,
						  "\nINNER JOIN include_pat ip"
						  "\nON (n.nspname ~ ip.nsp_regex OR ip.nsp_regex IS NULL)"
						  "\nAND (c.relname ~ ip.rel_regex OR ip.rel_regex IS NULL)"
						  "\nAND (c.relam = %u OR NOT ip.heap_only)"
						  "\nAND (c.relam = %u OR NOT ip.btree_only)",
						  HEAP_TABLE_AM_OID, BTREE_AM_OID);
	if (opts.excludetbl || opts.excludeidx || opts.excludensp)
		appendPQExpBuffer(&sql,
						  "\nLEFT OUTER JOIN exclude_pat ep"
						  "\nON (n.nspname ~ ep.nsp_regex OR ep.nsp_regex IS NULL)"
						  "\nAND (c.relname ~ ep.rel_regex OR ep.rel_regex IS NULL)"
						  "\nAND (c.relam = %u OR NOT ep.heap_only OR ep.rel_regex IS NULL)"
						  "\nAND (c.relam = %u OR NOT ep.btree_only OR ep.rel_regex IS NULL)",
						  HEAP_TABLE_AM_OID, BTREE_AM_OID);

	if (opts.excludetbl || opts.excludeidx || opts.excludensp)
		appendPQExpBufferStr(&sql, "\nWHERE ep.pattern_id IS NULL");
	else
		appendPQExpBufferStr(&sql, "\nWHERE true");

	/*
	 * We need to be careful not to break the --no-dependent-toast and
	 * --no-dependent-indexes options.  By default, the btree indexes, toast
	 * tables, and toast table btree indexes associated with primary heap
	 * tables are included, using their own CTEs below.  We implement the
	 * --exclude-* options by not creating those CTEs, but that's no use if
	 * we've already selected the toast and indexes here.  On the other hand,
	 * we want inclusion patterns that match indexes or toast tables to be
	 * honored.  So, if inclusion patterns were given, we want to select all
	 * tables, toast tables, or indexes that match the patterns.  But if no
	 * inclusion patterns were given, and we're simply matching all relations,
	 * then we only want to match the primary tables here.
	 */
	if (opts.allrel)
		appendPQExpBuffer(&sql,
						  " AND c.relam = %u "
						  "AND c.relkind IN ('r', 'm', 't') "
						  "AND c.relnamespace != %u",
						  HEAP_TABLE_AM_OID, PG_TOAST_NAMESPACE);
	else
		appendPQExpBuffer(&sql,
						  " AND c.relam IN (%u, %u)"
						  "AND c.relkind IN ('r', 'm', 't', 'i') "
						  "AND ((c.relam = %u AND c.relkind IN ('r', 'm', 't')) OR "
						  "(c.relam = %u AND c.relkind = 'i'))",
						  HEAP_TABLE_AM_OID, BTREE_AM_OID,
						  HEAP_TABLE_AM_OID, BTREE_AM_OID);

	appendPQExpBufferStr(&sql,
						 "\nORDER BY c.oid)");

	if (!opts.no_toast_expansion)
	{
		/*
		 * Include a CTE for toast tables associated with primary heap tables
		 * selected above, filtering by exclusion patterns (if any) that match
		 * toast table names.
		 */
		appendPQExpBufferStr(&sql,
							 ", toast (oid, nspname, relname, relpages) AS ("
							 "\nSELECT t.oid, 'pg_toast', t.relname, t.relpages"
							 "\nFROM pg_catalog.pg_class t "
							 "INNER JOIN relation r "
							 "ON r.reltoastrelid = t.oid");
		if (opts.excludetbl || opts.excludensp)
			appendPQExpBufferStr(&sql,
								 "\nLEFT OUTER JOIN exclude_pat ep"
								 "\nON ('pg_toast' ~ ep.nsp_regex OR ep.nsp_regex IS NULL)"
								 "\nAND (t.relname ~ ep.rel_regex OR ep.rel_regex IS NULL)"
								 "\nAND ep.heap_only"
								 "\nWHERE ep.pattern_id IS NULL");
		appendPQExpBufferStr(&sql,
							 "\n)");
	}
	if (!opts.no_btree_expansion)
	{
		/*
		 * Include a CTE for btree indexes associated with primary heap tables
		 * selected above, filtering by exclusion patterns (if any) that match
		 * btree index names.
		 */
		appendPQExpBuffer(&sql,
						  ", index (oid, nspname, relname, relpages) AS ("
						  "\nSELECT c.oid, r.nspname, c.relname, c.relpages "
						  "FROM relation r"
						  "\nINNER JOIN pg_catalog.pg_index i "
						  "ON r.oid = i.indrelid "
						  "INNER JOIN pg_catalog.pg_class c "
						  "ON i.indexrelid = c.oid");
		if (opts.excludeidx || opts.excludensp)
			appendPQExpBufferStr(&sql,
								 "\nINNER JOIN pg_catalog.pg_namespace n "
								 "ON c.relnamespace = n.oid"
								 "\nLEFT OUTER JOIN exclude_pat ep "
								 "ON (n.nspname ~ ep.nsp_regex OR ep.nsp_regex IS NULL) "
								 "AND (c.relname ~ ep.rel_regex OR ep.rel_regex IS NULL) "
								 "AND ep.btree_only"
								 "\nWHERE ep.pattern_id IS NULL");
		else
			appendPQExpBufferStr(&sql,
								 "\nWHERE true");
		appendPQExpBuffer(&sql,
						  " AND c.relam = %u "
						  "AND c.relkind = 'i'",
						  BTREE_AM_OID);
		if (opts.no_toast_expansion)
			appendPQExpBuffer(&sql,
							  " AND c.relnamespace != %u",
							  PG_TOAST_NAMESPACE);
		appendPQExpBufferStr(&sql, "\n)");
	}

	if (!opts.no_toast_expansion && !opts.no_btree_expansion)
	{
		/*
		 * Include a CTE for btree indexes associated with toast tables of
		 * primary heap tables selected above, filtering by exclusion patterns
		 * (if any) that match the toast index names.
		 */
		appendPQExpBuffer(&sql,
						  ", toast_index (oid, nspname, relname, relpages) AS ("
						  "\nSELECT c.oid, 'pg_toast', c.relname, c.relpages "
						  "FROM toast t "
						  "INNER JOIN pg_catalog.pg_index i "
						  "ON t.oid = i.indrelid"
						  "\nINNER JOIN pg_catalog.pg_class c "
						  "ON i.indexrelid = c.oid");
		if (opts.excludeidx)
			appendPQExpBufferStr(&sql,
								 "\nLEFT OUTER JOIN exclude_pat ep "
								 "ON ('pg_toast' ~ ep.nsp_regex OR ep.nsp_regex IS NULL) "
								 "AND (c.relname ~ ep.rel_regex OR ep.rel_regex IS NULL) "
								 "AND ep.btree_only "
								 "WHERE ep.pattern_id IS NULL");
		else
			appendPQExpBufferStr(&sql,
								 "\nWHERE true");
		appendPQExpBuffer(&sql,
						  " AND c.relam = %u"
						  " AND c.relkind = 'i')",
						  BTREE_AM_OID);
	}

	/*
	 * Roll-up distinct rows from CTEs.
	 *
	 * Relations that match more than one pattern may occur more than once in
	 * the list, and indexes and toast for primary relations may also have
	 * matched in their own right, so we rely on UNION to deduplicate the
	 * list.
	 */
	appendPQExpBuffer(&sql,
					  "\nSELECT pattern_id, is_heap, is_btree, oid, nspname, relname, relpages "
					  "FROM (");
	appendPQExpBufferStr(&sql,
	/* Inclusion patterns that failed to match */
						 "\nSELECT pattern_id, is_heap, is_btree, "
						 "NULL::OID AS oid, "
						 "NULL::TEXT AS nspname, "
						 "NULL::TEXT AS relname, "
						 "NULL::INTEGER AS relpages"
						 "\nFROM relation "
						 "WHERE pattern_id IS NOT NULL "
						 "UNION"
	/* Primary relations */
						 "\nSELECT NULL::INTEGER AS pattern_id, "
						 "is_heap, is_btree, oid, nspname, relname, relpages "
						 "FROM relation");
	if (!opts.no_toast_expansion)
		appendPQExpBufferStr(&sql,
							 " UNION"
		/* Toast tables for primary relations */
							 "\nSELECT NULL::INTEGER AS pattern_id, TRUE AS is_heap, "
							 "FALSE AS is_btree, oid, nspname, relname, relpages "
							 "FROM toast");
	if (!opts.no_btree_expansion)
		appendPQExpBufferStr(&sql,
							 " UNION"
		/* Indexes for primary relations */
							 "\nSELECT NULL::INTEGER AS pattern_id, FALSE AS is_heap, "
							 "TRUE AS is_btree, oid, nspname, relname, relpages "
							 "FROM index");
	if (!opts.no_toast_expansion && !opts.no_btree_expansion)
		appendPQExpBufferStr(&sql,
							 " UNION"
		/* Indexes for toast relations */
							 "\nSELECT NULL::INTEGER AS pattern_id, FALSE AS is_heap, "
							 "TRUE AS is_btree, oid, nspname, relname, relpages "
							 "FROM toast_index");
	appendPQExpBufferStr(&sql,
						 "\n) AS combined_records "
						 "ORDER BY relpages DESC NULLS FIRST, oid");

	res = executeQuery(conn, sql.data, opts.echo);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("query failed: %s", PQerrorMessage(conn));
		pg_log_info("query was: %s", sql.data);
		disconnectDatabase(conn);
		exit(1);
	}
	termPQExpBuffer(&sql);

	ntups = PQntuples(res);
	for (i = 0; i < ntups; i++)
	{
		int			pattern_id = -1;
		bool		is_heap = false;
		bool		is_btree PG_USED_FOR_ASSERTS_ONLY = false;
		Oid			oid = InvalidOid;
		const char *nspname = NULL;
		const char *relname = NULL;
		int			relpages = 0;

		if (!PQgetisnull(res, i, 0))
			pattern_id = atoi(PQgetvalue(res, i, 0));
		if (!PQgetisnull(res, i, 1))
			is_heap = (PQgetvalue(res, i, 1)[0] == 't');
		if (!PQgetisnull(res, i, 2))
			is_btree = (PQgetvalue(res, i, 2)[0] == 't');
		if (!PQgetisnull(res, i, 3))
			oid = atooid(PQgetvalue(res, i, 3));
		if (!PQgetisnull(res, i, 4))
			nspname = PQgetvalue(res, i, 4);
		if (!PQgetisnull(res, i, 5))
			relname = PQgetvalue(res, i, 5);
		if (!PQgetisnull(res, i, 6))
			relpages = atoi(PQgetvalue(res, i, 6));

		if (pattern_id >= 0)
		{
			/*
			 * Current record pertains to an inclusion pattern.  Record that
			 * it matched.
			 */

			if (pattern_id >= opts.include.len)
			{
				pg_log_error("internal error: received unexpected relation pattern_id %d",
							 pattern_id);
				exit(1);
			}

			opts.include.data[pattern_id].matched = true;
		}
		else
		{
			/* Current record pertains to a relation */

			RelationInfo *rel = (RelationInfo *) pg_malloc0(sizeof(RelationInfo));

			Assert(OidIsValid(oid));
			Assert((is_heap && !is_btree) || (is_btree && !is_heap));

			rel->datinfo = dat;
			rel->reloid = oid;
			rel->is_heap = is_heap;
			rel->nspname = pstrdup(nspname);
			rel->relname = pstrdup(relname);
			rel->relpages = relpages;
			rel->blocks_to_check = relpages;
			if (is_heap && (opts.startblock >= 0 || opts.endblock >= 0))
			{
				/*
				 * We apply --startblock and --endblock to heap tables, but
				 * not btree indexes, and for progress purposes we need to
				 * track how many blocks we expect to check.
				 */
				if (opts.endblock >= 0 && rel->blocks_to_check > opts.endblock)
					rel->blocks_to_check = opts.endblock + 1;
				if (opts.startblock >= 0)
				{
					if (rel->blocks_to_check > opts.startblock)
						rel->blocks_to_check -= opts.startblock;
					else
						rel->blocks_to_check = 0;
				}
			}
			*pagecount += rel->blocks_to_check;

			simple_ptr_list_append(relations, rel);
		}
	}
	PQclear(res);
}
