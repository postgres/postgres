/*-------------------------------------------------------------------------
 *
 * vacuumdb
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/scripts/vacuumdb.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <limits.h>

#include "catalog/pg_class_d.h"
#include "common.h"
#include "common/connect.h"
#include "common/logging.h"
#include "fe_utils/cancel.h"
#include "fe_utils/option_utils.h"
#include "fe_utils/parallel_slot.h"
#include "fe_utils/query_utils.h"
#include "fe_utils/simple_list.h"
#include "fe_utils/string_utils.h"


/* vacuum options controlled by user flags */
typedef struct vacuumingOptions
{
	bool		analyze_only;
	bool		verbose;
	bool		and_analyze;
	bool		full;
	bool		freeze;
	bool		disable_page_skipping;
	bool		skip_locked;
	int			min_xid_age;
	int			min_mxid_age;
	int			parallel_workers;	/* >= 0 indicates user specified the
									 * parallel degree, otherwise -1 */
	bool		no_index_cleanup;
	bool		force_index_cleanup;
	bool		do_truncate;
	bool		process_toast;
} vacuumingOptions;


static void vacuum_one_database(ConnParams *cparams,
								vacuumingOptions *vacopts,
								int stage,
								SimpleStringList *tables,
								int concurrentCons,
								const char *progname, bool echo, bool quiet);

static void vacuum_all_databases(ConnParams *cparams,
								 vacuumingOptions *vacopts,
								 bool analyze_in_stages,
								 int concurrentCons,
								 const char *progname, bool echo, bool quiet);

static void prepare_vacuum_command(PQExpBuffer sql, int serverVersion,
								   vacuumingOptions *vacopts, const char *table);

static void run_vacuum_command(PGconn *conn, const char *sql, bool echo,
							   const char *table);

static void help(const char *progname);

/* For analyze-in-stages mode */
#define ANALYZE_NO_STAGE	-1
#define ANALYZE_NUM_STAGES	3


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
		{NULL, 0, NULL, 0}
	};

	const char *progname;
	int			optindex;
	int			c;
	const char *dbname = NULL;
	const char *maintenance_db = NULL;
	char	   *host = NULL;
	char	   *port = NULL;
	char	   *username = NULL;
	enum trivalue prompt_password = TRI_DEFAULT;
	ConnParams	cparams;
	bool		echo = false;
	bool		quiet = false;
	vacuumingOptions vacopts;
	bool		analyze_in_stages = false;
	bool		alldb = false;
	SimpleStringList tables = {NULL, NULL};
	int			concurrentCons = 1;
	int			tbl_count = 0;

	/* initialize options */
	memset(&vacopts, 0, sizeof(vacopts));
	vacopts.parallel_workers = -1;
	vacopts.no_index_cleanup = false;
	vacopts.force_index_cleanup = false;
	vacopts.do_truncate = true;
	vacopts.process_toast = true;

	pg_logging_init(argv[0]);
	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pgscripts"));

	handle_help_version_opts(argc, argv, "vacuumdb", help);

	while ((c = getopt_long(argc, argv, "h:p:U:wWeqd:zZFat:fvj:P:", long_options, &optindex)) != -1)
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
			case 'q':
				quiet = true;
				break;
			case 'd':
				dbname = pg_strdup(optarg);
				break;
			case 'z':
				vacopts.and_analyze = true;
				break;
			case 'Z':
				vacopts.analyze_only = true;
				break;
			case 'F':
				vacopts.freeze = true;
				break;
			case 'a':
				alldb = true;
				break;
			case 't':
				{
					simple_string_list_append(&tables, optarg);
					tbl_count++;
					break;
				}
			case 'f':
				vacopts.full = true;
				break;
			case 'v':
				vacopts.verbose = true;
				break;
			case 'j':
				if (!option_parse_int(optarg, "-j/--jobs", 1, INT_MAX,
									  &concurrentCons))
					exit(1);
				break;
			case 'P':
				if (!option_parse_int(optarg, "-P/--parallel", 0, INT_MAX,
									  &vacopts.parallel_workers))
					exit(1);
				break;
			case 2:
				maintenance_db = pg_strdup(optarg);
				break;
			case 3:
				analyze_in_stages = vacopts.analyze_only = true;
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

	if (vacopts.analyze_only)
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
		if (!vacopts.process_toast)
			pg_fatal("cannot use the \"%s\" option when performing only analyze",
					 "no-process-toast");
		/* allow 'and_analyze' with 'analyze_only' */
	}

	/* Prohibit full and analyze_only options with parallel option */
	if (vacopts.parallel_workers >= 0)
	{
		if (vacopts.analyze_only)
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

	/* fill cparams except for dbname, which is set below */
	cparams.pghost = host;
	cparams.pgport = port;
	cparams.pguser = username;
	cparams.prompt_password = prompt_password;
	cparams.override_dbname = NULL;

	setup_cancel_handler(NULL);

	/* Avoid opening extra connections. */
	if (tbl_count && (concurrentCons > tbl_count))
		concurrentCons = tbl_count;

	if (alldb)
	{
		if (dbname)
			pg_fatal("cannot vacuum all databases and a specific one at the same time");
		if (tables.head != NULL)
			pg_fatal("cannot vacuum specific table(s) in all databases");

		cparams.dbname = maintenance_db;

		vacuum_all_databases(&cparams, &vacopts,
							 analyze_in_stages,
							 concurrentCons,
							 progname, echo, quiet);
	}
	else
	{
		if (dbname == NULL)
		{
			if (getenv("PGDATABASE"))
				dbname = getenv("PGDATABASE");
			else if (getenv("PGUSER"))
				dbname = getenv("PGUSER");
			else
				dbname = get_user_name_or_exit(progname);
		}

		cparams.dbname = dbname;

		if (analyze_in_stages)
		{
			int			stage;

			for (stage = 0; stage < ANALYZE_NUM_STAGES; stage++)
			{
				vacuum_one_database(&cparams, &vacopts,
									stage,
									&tables,
									concurrentCons,
									progname, echo, quiet);
			}
		}
		else
			vacuum_one_database(&cparams, &vacopts,
								ANALYZE_NO_STAGE,
								&tables,
								concurrentCons,
								progname, echo, quiet);
	}

	exit(0);
}

/*
 * vacuum_one_database
 *
 * Process tables in the given database.  If the 'tables' list is empty,
 * process all tables in the database.
 *
 * Note that this function is only concerned with running exactly one stage
 * when in analyze-in-stages mode; caller must iterate on us if necessary.
 *
 * If concurrentCons is > 1, multiple connections are used to vacuum tables
 * in parallel.  In this case and if the table list is empty, we first obtain
 * a list of tables from the database.
 */
static void
vacuum_one_database(ConnParams *cparams,
					vacuumingOptions *vacopts,
					int stage,
					SimpleStringList *tables,
					int concurrentCons,
					const char *progname, bool echo, bool quiet)
{
	PQExpBufferData sql;
	PQExpBufferData buf;
	PQExpBufferData catalog_query;
	PGresult   *res;
	PGconn	   *conn;
	SimpleStringListCell *cell;
	ParallelSlotArray *sa;
	SimpleStringList dbtables = {NULL, NULL};
	int			i;
	int			ntups;
	bool		failed = false;
	bool		tables_listed = false;
	bool		has_where = false;
	const char *initcmd;
	const char *stage_commands[] = {
		"SET default_statistics_target=1; SET vacuum_cost_delay=0;",
		"SET default_statistics_target=10; RESET vacuum_cost_delay;",
		"RESET default_statistics_target;"
	};
	const char *stage_messages[] = {
		gettext_noop("Generating minimal optimizer statistics (1 target)"),
		gettext_noop("Generating medium optimizer statistics (10 targets)"),
		gettext_noop("Generating default (full) optimizer statistics")
	};

	Assert(stage == ANALYZE_NO_STAGE ||
		   (stage >= 0 && stage < ANALYZE_NUM_STAGES));

	conn = connectDatabase(cparams, progname, echo, false, true);

	if (vacopts->disable_page_skipping && PQserverVersion(conn) < 90600)
	{
		PQfinish(conn);
		pg_fatal("cannot use the \"%s\" option on server versions older than PostgreSQL %s",
				 "disable-page-skipping", "9.6");
	}

	if (vacopts->no_index_cleanup && PQserverVersion(conn) < 120000)
	{
		PQfinish(conn);
		pg_fatal("cannot use the \"%s\" option on server versions older than PostgreSQL %s",
				 "no-index-cleanup", "12");
	}

	if (vacopts->force_index_cleanup && PQserverVersion(conn) < 120000)
	{
		PQfinish(conn);
		pg_fatal("cannot use the \"%s\" option on server versions older than PostgreSQL %s",
				 "force-index-cleanup", "12");
	}

	if (!vacopts->do_truncate && PQserverVersion(conn) < 120000)
	{
		PQfinish(conn);
		pg_fatal("cannot use the \"%s\" option on server versions older than PostgreSQL %s",
				 "no-truncate", "12");
	}

	if (!vacopts->process_toast && PQserverVersion(conn) < 140000)
	{
		PQfinish(conn);
		pg_fatal("cannot use the \"%s\" option on server versions older than PostgreSQL %s",
				 "no-process-toast", "14");
	}

	if (vacopts->skip_locked && PQserverVersion(conn) < 120000)
	{
		PQfinish(conn);
		pg_fatal("cannot use the \"%s\" option on server versions older than PostgreSQL %s",
				 "skip-locked", "12");
	}

	if (vacopts->min_xid_age != 0 && PQserverVersion(conn) < 90600)
		pg_fatal("cannot use the \"%s\" option on server versions older than PostgreSQL %s",
				 "--min-xid-age", "9.6");

	if (vacopts->min_mxid_age != 0 && PQserverVersion(conn) < 90600)
		pg_fatal("cannot use the \"%s\" option on server versions older than PostgreSQL %s",
				 "--min-mxid-age", "9.6");

	if (vacopts->parallel_workers >= 0 && PQserverVersion(conn) < 130000)
		pg_fatal("cannot use the \"%s\" option on server versions older than PostgreSQL %s",
				 "--parallel", "13");

	if (!quiet)
	{
		if (stage != ANALYZE_NO_STAGE)
			printf(_("%s: processing database \"%s\": %s\n"),
				   progname, PQdb(conn), _(stage_messages[stage]));
		else
			printf(_("%s: vacuuming database \"%s\"\n"),
				   progname, PQdb(conn));
		fflush(stdout);
	}

	/*
	 * Prepare the list of tables to process by querying the catalogs.
	 *
	 * Since we execute the constructed query with the default search_path
	 * (which could be unsafe), everything in this query MUST be fully
	 * qualified.
	 *
	 * First, build a WITH clause for the catalog query if any tables were
	 * specified, with a set of values made of relation names and their
	 * optional set of columns.  This is used to match any provided column
	 * lists with the generated qualified identifiers and to filter for the
	 * tables provided via --table.  If a listed table does not exist, the
	 * catalog query will fail.
	 */
	initPQExpBuffer(&catalog_query);
	for (cell = tables ? tables->head : NULL; cell; cell = cell->next)
	{
		char	   *just_table;
		const char *just_columns;

		/*
		 * Split relation and column names given by the user, this is used to
		 * feed the CTE with values on which are performed pre-run validity
		 * checks as well.  For now these happen only on the relation name.
		 */
		splitTableColumnsSpec(cell->val, PQclientEncoding(conn),
							  &just_table, &just_columns);

		if (!tables_listed)
		{
			appendPQExpBufferStr(&catalog_query,
								 "WITH listed_tables (table_oid, column_list) "
								 "AS (\n  VALUES (");
			tables_listed = true;
		}
		else
			appendPQExpBufferStr(&catalog_query, ",\n  (");

		appendStringLiteralConn(&catalog_query, just_table, conn);
		appendPQExpBufferStr(&catalog_query, "::pg_catalog.regclass, ");

		if (just_columns && just_columns[0] != '\0')
			appendStringLiteralConn(&catalog_query, just_columns, conn);
		else
			appendPQExpBufferStr(&catalog_query, "NULL");

		appendPQExpBufferStr(&catalog_query, "::pg_catalog.text)");

		pg_free(just_table);
	}

	/* Finish formatting the CTE */
	if (tables_listed)
		appendPQExpBufferStr(&catalog_query, "\n)\n");

	appendPQExpBufferStr(&catalog_query, "SELECT c.relname, ns.nspname");

	if (tables_listed)
		appendPQExpBufferStr(&catalog_query, ", listed_tables.column_list");

	appendPQExpBufferStr(&catalog_query,
						 " FROM pg_catalog.pg_class c\n"
						 " JOIN pg_catalog.pg_namespace ns"
						 " ON c.relnamespace OPERATOR(pg_catalog.=) ns.oid\n"
						 " LEFT JOIN pg_catalog.pg_class t"
						 " ON c.reltoastrelid OPERATOR(pg_catalog.=) t.oid\n");

	/* Used to match the tables listed by the user */
	if (tables_listed)
		appendPQExpBufferStr(&catalog_query, " JOIN listed_tables"
							 " ON listed_tables.table_oid OPERATOR(pg_catalog.=) c.oid\n");

	/*
	 * If no tables were listed, filter for the relevant relation types.  If
	 * tables were given via --table, don't bother filtering by relation type.
	 * Instead, let the server decide whether a given relation can be
	 * processed in which case the user will know about it.
	 */
	if (!tables_listed)
	{
		appendPQExpBufferStr(&catalog_query, " WHERE c.relkind OPERATOR(pg_catalog.=) ANY (array["
							 CppAsString2(RELKIND_RELATION) ", "
							 CppAsString2(RELKIND_MATVIEW) "])\n");
		has_where = true;
	}

	/*
	 * For --min-xid-age and --min-mxid-age, the age of the relation is the
	 * greatest of the ages of the main relation and its associated TOAST
	 * table.  The commands generated by vacuumdb will also process the TOAST
	 * table for the relation if necessary, so it does not need to be
	 * considered separately.
	 */
	if (vacopts->min_xid_age != 0)
	{
		appendPQExpBuffer(&catalog_query,
						  " %s GREATEST(pg_catalog.age(c.relfrozenxid),"
						  " pg_catalog.age(t.relfrozenxid)) "
						  " OPERATOR(pg_catalog.>=) '%d'::pg_catalog.int4\n"
						  " AND c.relfrozenxid OPERATOR(pg_catalog.!=)"
						  " '0'::pg_catalog.xid\n",
						  has_where ? "AND" : "WHERE", vacopts->min_xid_age);
		has_where = true;
	}

	if (vacopts->min_mxid_age != 0)
	{
		appendPQExpBuffer(&catalog_query,
						  " %s GREATEST(pg_catalog.mxid_age(c.relminmxid),"
						  " pg_catalog.mxid_age(t.relminmxid)) OPERATOR(pg_catalog.>=)"
						  " '%d'::pg_catalog.int4\n"
						  " AND c.relminmxid OPERATOR(pg_catalog.!=)"
						  " '0'::pg_catalog.xid\n",
						  has_where ? "AND" : "WHERE", vacopts->min_mxid_age);
		has_where = true;
	}

	/*
	 * Execute the catalog query.  We use the default search_path for this
	 * query for consistency with table lookups done elsewhere by the user.
	 */
	appendPQExpBufferStr(&catalog_query, " ORDER BY c.relpages DESC;");
	executeCommand(conn, "RESET search_path;", echo);
	res = executeQuery(conn, catalog_query.data, echo);
	termPQExpBuffer(&catalog_query);
	PQclear(executeQuery(conn, ALWAYS_SECURE_SEARCH_PATH_SQL, echo));

	/*
	 * If no rows are returned, there are no matching tables, so we are done.
	 */
	ntups = PQntuples(res);
	if (ntups == 0)
	{
		PQclear(res);
		PQfinish(conn);
		return;
	}

	/*
	 * Build qualified identifiers for each table, including the column list
	 * if given.
	 */
	initPQExpBuffer(&buf);
	for (i = 0; i < ntups; i++)
	{
		appendPQExpBufferStr(&buf,
							 fmtQualifiedId(PQgetvalue(res, i, 1),
											PQgetvalue(res, i, 0)));

		if (tables_listed && !PQgetisnull(res, i, 2))
			appendPQExpBufferStr(&buf, PQgetvalue(res, i, 2));

		simple_string_list_append(&dbtables, buf.data);
		resetPQExpBuffer(&buf);
	}
	termPQExpBuffer(&buf);
	PQclear(res);

	/*
	 * Ensure concurrentCons is sane.  If there are more connections than
	 * vacuumable relations, we don't need to use them all.
	 */
	if (concurrentCons > ntups)
		concurrentCons = ntups;
	if (concurrentCons <= 0)
		concurrentCons = 1;

	/*
	 * All slots need to be prepared to run the appropriate analyze stage, if
	 * caller requested that mode.  We have to prepare the initial connection
	 * ourselves before setting up the slots.
	 */
	if (stage == ANALYZE_NO_STAGE)
		initcmd = NULL;
	else
	{
		initcmd = stage_commands[stage];
		executeCommand(conn, initcmd, echo);
	}

	/*
	 * Setup the database connections. We reuse the connection we already have
	 * for the first slot.  If not in parallel mode, the first slot in the
	 * array contains the connection.
	 */
	sa = ParallelSlotsSetup(concurrentCons, cparams, progname, echo, initcmd);
	ParallelSlotsAdoptConn(sa, conn);

	initPQExpBuffer(&sql);

	cell = dbtables.head;
	do
	{
		const char *tabname = cell->val;
		ParallelSlot *free_slot;

		if (CancelRequested)
		{
			failed = true;
			goto finish;
		}

		free_slot = ParallelSlotsGetIdle(sa, NULL);
		if (!free_slot)
		{
			failed = true;
			goto finish;
		}

		prepare_vacuum_command(&sql, PQserverVersion(free_slot->connection),
							   vacopts, tabname);

		/*
		 * Execute the vacuum.  All errors are handled in processQueryResult
		 * through ParallelSlotsGetIdle.
		 */
		ParallelSlotSetHandler(free_slot, TableCommandResultHandler, NULL);
		run_vacuum_command(free_slot->connection, sql.data,
						   echo, tabname);

		cell = cell->next;
	} while (cell != NULL);

	if (!ParallelSlotsWaitCompletion(sa))
		failed = true;

finish:
	ParallelSlotsTerminate(sa);
	pg_free(sa);

	termPQExpBuffer(&sql);

	if (failed)
		exit(1);
}

/*
 * Vacuum/analyze all connectable databases.
 *
 * In analyze-in-stages mode, we process all databases in one stage before
 * moving on to the next stage.  That ensure minimal stats are available
 * quickly everywhere before generating more detailed ones.
 */
static void
vacuum_all_databases(ConnParams *cparams,
					 vacuumingOptions *vacopts,
					 bool analyze_in_stages,
					 int concurrentCons,
					 const char *progname, bool echo, bool quiet)
{
	PGconn	   *conn;
	PGresult   *result;
	int			stage;
	int			i;

	conn = connectMaintenanceDatabase(cparams, progname, echo);
	result = executeQuery(conn,
						  "SELECT datname FROM pg_database WHERE datallowconn ORDER BY 1;",
						  echo);
	PQfinish(conn);

	if (analyze_in_stages)
	{
		/*
		 * When analyzing all databases in stages, we analyze them all in the
		 * fastest stage first, so that initial statistics become available
		 * for all of them as soon as possible.
		 *
		 * This means we establish several times as many connections, but
		 * that's a secondary consideration.
		 */
		for (stage = 0; stage < ANALYZE_NUM_STAGES; stage++)
		{
			for (i = 0; i < PQntuples(result); i++)
			{
				cparams->override_dbname = PQgetvalue(result, i, 0);

				vacuum_one_database(cparams, vacopts,
									stage,
									NULL,
									concurrentCons,
									progname, echo, quiet);
			}
		}
	}
	else
	{
		for (i = 0; i < PQntuples(result); i++)
		{
			cparams->override_dbname = PQgetvalue(result, i, 0);

			vacuum_one_database(cparams, vacopts,
								ANALYZE_NO_STAGE,
								NULL,
								concurrentCons,
								progname, echo, quiet);
		}
	}

	PQclear(result);
}

/*
 * Construct a vacuum/analyze command to run based on the given options, in the
 * given string buffer, which may contain previous garbage.
 *
 * The table name used must be already properly quoted.  The command generated
 * depends on the server version involved and it is semicolon-terminated.
 */
static void
prepare_vacuum_command(PQExpBuffer sql, int serverVersion,
					   vacuumingOptions *vacopts, const char *table)
{
	const char *paren = " (";
	const char *comma = ", ";
	const char *sep = paren;

	resetPQExpBuffer(sql);

	if (vacopts->analyze_only)
	{
		appendPQExpBufferStr(sql, "ANALYZE");

		/* parenthesized grammar of ANALYZE is supported since v11 */
		if (serverVersion >= 110000)
		{
			if (vacopts->skip_locked)
			{
				/* SKIP_LOCKED is supported since v12 */
				Assert(serverVersion >= 120000);
				appendPQExpBuffer(sql, "%sSKIP_LOCKED", sep);
				sep = comma;
			}
			if (vacopts->verbose)
			{
				appendPQExpBuffer(sql, "%sVERBOSE", sep);
				sep = comma;
			}
			if (sep != paren)
				appendPQExpBufferChar(sql, ')');
		}
		else
		{
			if (vacopts->verbose)
				appendPQExpBufferStr(sql, " VERBOSE");
		}
	}
	else
	{
		appendPQExpBufferStr(sql, "VACUUM");

		/* parenthesized grammar of VACUUM is supported since v9.0 */
		if (serverVersion >= 90000)
		{
			if (vacopts->disable_page_skipping)
			{
				/* DISABLE_PAGE_SKIPPING is supported since v9.6 */
				Assert(serverVersion >= 90600);
				appendPQExpBuffer(sql, "%sDISABLE_PAGE_SKIPPING", sep);
				sep = comma;
			}
			if (vacopts->no_index_cleanup)
			{
				/* "INDEX_CLEANUP FALSE" has been supported since v12 */
				Assert(serverVersion >= 120000);
				Assert(!vacopts->force_index_cleanup);
				appendPQExpBuffer(sql, "%sINDEX_CLEANUP FALSE", sep);
				sep = comma;
			}
			if (vacopts->force_index_cleanup)
			{
				/* "INDEX_CLEANUP TRUE" has been supported since v12 */
				Assert(serverVersion >= 120000);
				Assert(!vacopts->no_index_cleanup);
				appendPQExpBuffer(sql, "%sINDEX_CLEANUP TRUE", sep);
				sep = comma;
			}
			if (!vacopts->do_truncate)
			{
				/* TRUNCATE is supported since v12 */
				Assert(serverVersion >= 120000);
				appendPQExpBuffer(sql, "%sTRUNCATE FALSE", sep);
				sep = comma;
			}
			if (!vacopts->process_toast)
			{
				/* PROCESS_TOAST is supported since v14 */
				Assert(serverVersion >= 140000);
				appendPQExpBuffer(sql, "%sPROCESS_TOAST FALSE", sep);
				sep = comma;
			}
			if (vacopts->skip_locked)
			{
				/* SKIP_LOCKED is supported since v12 */
				Assert(serverVersion >= 120000);
				appendPQExpBuffer(sql, "%sSKIP_LOCKED", sep);
				sep = comma;
			}
			if (vacopts->full)
			{
				appendPQExpBuffer(sql, "%sFULL", sep);
				sep = comma;
			}
			if (vacopts->freeze)
			{
				appendPQExpBuffer(sql, "%sFREEZE", sep);
				sep = comma;
			}
			if (vacopts->verbose)
			{
				appendPQExpBuffer(sql, "%sVERBOSE", sep);
				sep = comma;
			}
			if (vacopts->and_analyze)
			{
				appendPQExpBuffer(sql, "%sANALYZE", sep);
				sep = comma;
			}
			if (vacopts->parallel_workers >= 0)
			{
				/* PARALLEL is supported since v13 */
				Assert(serverVersion >= 130000);
				appendPQExpBuffer(sql, "%sPARALLEL %d", sep,
								  vacopts->parallel_workers);
				sep = comma;
			}
			if (sep != paren)
				appendPQExpBufferChar(sql, ')');
		}
		else
		{
			if (vacopts->full)
				appendPQExpBufferStr(sql, " FULL");
			if (vacopts->freeze)
				appendPQExpBufferStr(sql, " FREEZE");
			if (vacopts->verbose)
				appendPQExpBufferStr(sql, " VERBOSE");
			if (vacopts->and_analyze)
				appendPQExpBufferStr(sql, " ANALYZE");
		}
	}

	appendPQExpBuffer(sql, " %s;", table);
}

/*
 * Send a vacuum/analyze command to the server, returning after sending the
 * command.
 *
 * Any errors during command execution are reported to stderr.
 */
static void
run_vacuum_command(PGconn *conn, const char *sql, bool echo,
				   const char *table)
{
	bool		status;

	if (echo)
		printf("%s\n", sql);

	status = PQsendQuery(conn, sql) == 1;

	if (!status)
	{
		if (table)
			pg_log_error("vacuuming of table \"%s\" in database \"%s\" failed: %s",
						 table, PQdb(conn), PQerrorMessage(conn));
		else
			pg_log_error("vacuuming of database \"%s\" failed: %s",
						 PQdb(conn), PQerrorMessage(conn));
	}
}

static void
help(const char *progname)
{
	printf(_("%s cleans and analyzes a PostgreSQL database.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [DBNAME]\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -a, --all                       vacuum all databases\n"));
	printf(_("  -d, --dbname=DBNAME             database to vacuum\n"));
	printf(_("      --disable-page-skipping     disable all page-skipping behavior\n"));
	printf(_("  -e, --echo                      show the commands being sent to the server\n"));
	printf(_("  -f, --full                      do full vacuuming\n"));
	printf(_("  -F, --freeze                    freeze row transaction information\n"));
	printf(_("      --force-index-cleanup       always remove index entries that point to dead tuples\n"));
	printf(_("  -j, --jobs=NUM                  use this many concurrent connections to vacuum\n"));
	printf(_("      --min-mxid-age=MXID_AGE     minimum multixact ID age of tables to vacuum\n"));
	printf(_("      --min-xid-age=XID_AGE       minimum transaction ID age of tables to vacuum\n"));
	printf(_("      --no-index-cleanup          don't remove index entries that point to dead tuples\n"));
	printf(_("      --no-process-toast          skip the TOAST table associated with the table to vacuum\n"));
	printf(_("      --no-truncate               don't truncate empty pages at the end of the table\n"));
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
