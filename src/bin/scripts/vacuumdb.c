/*-------------------------------------------------------------------------
 *
 * vacuumdb
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/scripts/vacuumdb.c
 *
 *-------------------------------------------------------------------------
 */

#ifdef WIN32
#define FD_SETSIZE 1024			/* must set before winsock2.h is included */
#endif

#include "postgres_fe.h"

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include "catalog/pg_class_d.h"

#include "common.h"
#include "common/logging.h"
#include "fe_utils/connect.h"
#include "fe_utils/simple_list.h"
#include "fe_utils/string_utils.h"


#define ERRCODE_UNDEFINED_TABLE  "42P01"

/* Parallel vacuuming stuff */
typedef struct ParallelSlot
{
	PGconn	   *connection;		/* One connection */
	bool		isFree;			/* Is it known to be idle? */
} ParallelSlot;

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
} vacuumingOptions;


static void vacuum_one_database(const ConnParams *cparams,
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
							   const char *table, const char *progname, bool async);

static ParallelSlot *GetIdleSlot(ParallelSlot slots[], int numslots,
								 const char *progname);

static bool ProcessQueryResult(PGconn *conn, PGresult *result,
							   const char *progname);

static bool GetQueryResult(PGconn *conn, const char *progname);

static void DisconnectDatabase(ParallelSlot *slot);

static int	select_loop(int maxFd, fd_set *workerset, bool *aborting);

static void init_slot(ParallelSlot *slot, PGconn *conn);

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
		{"maintenance-db", required_argument, NULL, 2},
		{"analyze-in-stages", no_argument, NULL, 3},
		{"disable-page-skipping", no_argument, NULL, 4},
		{"skip-locked", no_argument, NULL, 5},
		{"min-xid-age", required_argument, NULL, 6},
		{"min-mxid-age", required_argument, NULL, 7},
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

	/* initialize options to all false */
	memset(&vacopts, 0, sizeof(vacopts));

	pg_logging_init(argv[0]);
	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pgscripts"));

	handle_help_version_opts(argc, argv, "vacuumdb", help);

	while ((c = getopt_long(argc, argv, "h:p:U:wWeqd:zZFat:fvj:", long_options, &optindex)) != -1)
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
				concurrentCons = atoi(optarg);
				if (concurrentCons <= 0)
				{
					pg_log_error("number of parallel jobs must be at least 1");
					exit(1);
				}
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
				vacopts.min_xid_age = atoi(optarg);
				if (vacopts.min_xid_age <= 0)
				{
					pg_log_error("minimum transaction ID age must be at least 1");
					exit(1);
				}
				break;
			case 7:
				vacopts.min_mxid_age = atoi(optarg);
				if (vacopts.min_mxid_age <= 0)
				{
					pg_log_error("minimum multixact ID age must be at least 1");
					exit(1);
				}
				break;
			default:
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
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
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
		exit(1);
	}

	if (vacopts.analyze_only)
	{
		if (vacopts.full)
		{
			pg_log_error("cannot use the \"%s\" option when performing only analyze",
						 "full");
			exit(1);
		}
		if (vacopts.freeze)
		{
			pg_log_error("cannot use the \"%s\" option when performing only analyze",
						 "freeze");
			exit(1);
		}
		if (vacopts.disable_page_skipping)
		{
			pg_log_error("cannot use the \"%s\" option when performing only analyze",
						 "disable-page-skipping");
			exit(1);
		}
		/* allow 'and_analyze' with 'analyze_only' */
	}

	/* fill cparams except for dbname, which is set below */
	cparams.pghost = host;
	cparams.pgport = port;
	cparams.pguser = username;
	cparams.prompt_password = prompt_password;
	cparams.override_dbname = NULL;

	setup_cancel_handler();

	/* Avoid opening extra connections. */
	if (tbl_count && (concurrentCons > tbl_count))
		concurrentCons = tbl_count;

	if (alldb)
	{
		if (dbname)
		{
			pg_log_error("cannot vacuum all databases and a specific one at the same time");
			exit(1);
		}
		if (tables.head != NULL)
		{
			pg_log_error("cannot vacuum specific table(s) in all databases");
			exit(1);
		}

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
vacuum_one_database(const ConnParams *cparams,
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
	ParallelSlot *slots;
	SimpleStringList dbtables = {NULL, NULL};
	int			i;
	int			ntups;
	bool		failed = false;
	bool		parallel = concurrentCons > 1;
	bool		tables_listed = false;
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
		pg_log_error("cannot use the \"%s\" option on server versions older than PostgreSQL %s",
					 "disable-page-skipping", "9.6");
		exit(1);
	}

	if (vacopts->skip_locked && PQserverVersion(conn) < 120000)
	{
		PQfinish(conn);
		pg_log_error("cannot use the \"%s\" option on server versions older than PostgreSQL %s",
					 "skip-locked", "12");
		exit(1);
	}

	if (vacopts->min_xid_age != 0 && PQserverVersion(conn) < 90600)
	{
		pg_log_error("cannot use the \"%s\" option on server versions older than PostgreSQL %s",
					 "--min-xid-age", "9.6");
		exit(1);
	}

	if (vacopts->min_mxid_age != 0 && PQserverVersion(conn) < 90600)
	{
		pg_log_error("cannot use the \"%s\" option on server versions older than PostgreSQL %s",
					 "--min-mxid-age", "9.6");
		exit(1);
	}

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
			appendPQExpBuffer(&catalog_query,
							  "WITH listed_tables (table_oid, column_list) "
							  "AS (\n  VALUES (");
			tables_listed = true;
		}
		else
			appendPQExpBuffer(&catalog_query, ",\n  (");

		appendStringLiteralConn(&catalog_query, just_table, conn);
		appendPQExpBuffer(&catalog_query, "::pg_catalog.regclass, ");

		if (just_columns && just_columns[0] != '\0')
			appendStringLiteralConn(&catalog_query, just_columns, conn);
		else
			appendPQExpBufferStr(&catalog_query, "NULL");

		appendPQExpBufferStr(&catalog_query, "::pg_catalog.text)");

		pg_free(just_table);
	}

	/* Finish formatting the CTE */
	if (tables_listed)
		appendPQExpBuffer(&catalog_query, "\n)\n");

	appendPQExpBuffer(&catalog_query, "SELECT c.relname, ns.nspname");

	if (tables_listed)
		appendPQExpBuffer(&catalog_query, ", listed_tables.column_list");

	appendPQExpBuffer(&catalog_query,
					  " FROM pg_catalog.pg_class c\n"
					  " JOIN pg_catalog.pg_namespace ns"
					  " ON c.relnamespace OPERATOR(pg_catalog.=) ns.oid\n"
					  " LEFT JOIN pg_catalog.pg_class t"
					  " ON c.reltoastrelid OPERATOR(pg_catalog.=) t.oid\n");

	/*
	 * Used to match the tables listed by the user, completing the JOIN
	 * clause.
	 */
	if (tables_listed)
		appendPQExpBuffer(&catalog_query, " JOIN listed_tables"
						  " ON listed_tables.table_oid OPERATOR(pg_catalog.=) c.oid\n");

	/*
	 * Exclude temporary tables, beginning the WHERE clause.
	 */
	appendPQExpBufferStr(&catalog_query,
						 " WHERE c.relpersistence OPERATOR(pg_catalog.!=) "
						 CppAsString2(RELPERSISTENCE_TEMP) "\n");


	/*
	 * If no tables were listed, filter for the relevant relation types.  If
	 * tables were given via --table, don't bother filtering by relation type.
	 * Instead, let the server decide whether a given relation can be
	 * processed in which case the user will know about it.
	 */
	if (!tables_listed)
	{
		appendPQExpBuffer(&catalog_query, " AND c.relkind OPERATOR(pg_catalog.=) ANY (array["
						  CppAsString2(RELKIND_RELATION) ", "
						  CppAsString2(RELKIND_MATVIEW) "])\n");
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
						  " AND GREATEST(pg_catalog.age(c.relfrozenxid),"
						  " pg_catalog.age(t.relfrozenxid)) "
						  " OPERATOR(pg_catalog.>=) '%d'::pg_catalog.int4\n"
						  " AND c.relfrozenxid OPERATOR(pg_catalog.!=)"
						  " '0'::pg_catalog.xid\n",
						  vacopts->min_xid_age);
	}

	if (vacopts->min_mxid_age != 0)
	{
		appendPQExpBuffer(&catalog_query,
						  " AND GREATEST(pg_catalog.mxid_age(c.relminmxid),"
						  " pg_catalog.mxid_age(t.relminmxid)) OPERATOR(pg_catalog.>=)"
						  " '%d'::pg_catalog.int4\n"
						  " AND c.relminmxid OPERATOR(pg_catalog.!=)"
						  " '0'::pg_catalog.xid\n",
						  vacopts->min_mxid_age);
	}

	/*
	 * Execute the catalog query.  We use the default search_path for this
	 * query for consistency with table lookups done elsewhere by the user.
	 */
	appendPQExpBuffer(&catalog_query, " ORDER BY c.relpages DESC;");
	executeCommand(conn, "RESET search_path;", progname, echo);
	res = executeQuery(conn, catalog_query.data, progname, echo);
	termPQExpBuffer(&catalog_query);
	PQclear(executeQuery(conn, ALWAYS_SECURE_SEARCH_PATH_SQL,
						 progname, echo));

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
	 * If there are more connections than vacuumable relations, we don't need
	 * to use them all.
	 */
	if (parallel)
	{
		if (concurrentCons > ntups)
			concurrentCons = ntups;
		if (concurrentCons <= 1)
			parallel = false;
	}

	/*
	 * Setup the database connections. We reuse the connection we already have
	 * for the first slot.  If not in parallel mode, the first slot in the
	 * array contains the connection.
	 */
	if (concurrentCons <= 0)
		concurrentCons = 1;
	slots = (ParallelSlot *) pg_malloc(sizeof(ParallelSlot) * concurrentCons);
	init_slot(slots, conn);
	if (parallel)
	{
		for (i = 1; i < concurrentCons; i++)
		{
			conn = connectDatabase(cparams, progname, echo, false, true);

			/*
			 * POSIX defines FD_SETSIZE as the highest file descriptor
			 * acceptable to FD_SET() and allied macros.  Windows defines it
			 * as a ceiling on the count of file descriptors in the set, not a
			 * ceiling on the value of each file descriptor; see
			 * https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-select
			 * and
			 * https://learn.microsoft.com/en-us/windows/win32/api/winsock/ns-winsock-fd_set.
			 * We can't ignore that, because Windows starts file descriptors
			 * at a higher value, delays reuse, and skips values.  With less
			 * than ten concurrent file descriptors, opened and closed
			 * rapidly, one can reach file descriptor 1024.
			 *
			 * Doing a hard exit here is a bit grotty, but it doesn't seem
			 * worth complicating the API to make it less grotty.
			 */
#ifdef WIN32
			if (i >= FD_SETSIZE)
			{
				pg_log_fatal("too many jobs for this platform: %d", i);
				exit(1);
			}
#else
			{
				int			fd = PQsocket(conn);

				if (fd >= FD_SETSIZE)
				{
					pg_log_fatal("socket file descriptor out of range for select(): %d",
								 fd);
					exit(1);
				}
			}
#endif

			init_slot(slots + i, conn);
		}
	}

	/*
	 * Prepare all the connections to run the appropriate analyze stage, if
	 * caller requested that mode.
	 */
	if (stage != ANALYZE_NO_STAGE)
	{
		int			j;

		/* We already emitted the message above */

		for (j = 0; j < concurrentCons; j++)
			executeCommand((slots + j)->connection,
						   stage_commands[stage], progname, echo);
	}

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

		/*
		 * Get the connection slot to use.  If in parallel mode, here we wait
		 * for one connection to become available if none already is.  In
		 * non-parallel mode we simply use the only slot we have, which we
		 * know to be free.
		 */
		if (parallel)
		{
			/*
			 * Get a free slot, waiting until one becomes free if none
			 * currently is.
			 */
			free_slot = GetIdleSlot(slots, concurrentCons, progname);
			if (!free_slot)
			{
				failed = true;
				goto finish;
			}

			free_slot->isFree = false;
		}
		else
			free_slot = slots;

		prepare_vacuum_command(&sql, PQserverVersion(free_slot->connection),
							   vacopts, tabname);

		/*
		 * Execute the vacuum.  If not in parallel mode, this terminates the
		 * program in case of an error.  (The parallel case handles query
		 * errors in ProcessQueryResult through GetIdleSlot.)
		 */
		run_vacuum_command(free_slot->connection, sql.data,
						   echo, tabname, progname, parallel);

		cell = cell->next;
	} while (cell != NULL);

	if (parallel)
	{
		int			j;

		/* wait for all connections to finish */
		for (j = 0; j < concurrentCons; j++)
		{
			if (!GetQueryResult((slots + j)->connection, progname))
			{
				failed = true;
				goto finish;
			}
		}
	}

finish:
	for (i = 0; i < concurrentCons; i++)
		DisconnectDatabase(slots + i);
	pfree(slots);

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
						  "SELECT datname FROM pg_database WHERE datallowconn AND datconnlimit <> -2 ORDER BY 1;",
						  progname, echo);
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
 * Send a vacuum/analyze command to the server.  In async mode, return after
 * sending the command; else, wait for it to finish.
 *
 * Any errors during command execution are reported to stderr.  If async is
 * false, this function exits the program after reporting the error.
 */
static void
run_vacuum_command(PGconn *conn, const char *sql, bool echo,
				   const char *table, const char *progname, bool async)
{
	bool		status;

	if (async)
	{
		if (echo)
			printf("%s\n", sql);

		status = PQsendQuery(conn, sql) == 1;
	}
	else
		status = executeMaintenanceCommand(conn, sql, echo);

	if (!status)
	{
		if (table)
			pg_log_error("vacuuming of table \"%s\" in database \"%s\" failed: %s",
						 table, PQdb(conn), PQerrorMessage(conn));
		else
			pg_log_error("vacuuming of database \"%s\" failed: %s",
						 PQdb(conn), PQerrorMessage(conn));

		if (!async)
		{
			PQfinish(conn);
			exit(1);
		}
	}
}

/*
 * GetIdleSlot
 *		Return a connection slot that is ready to execute a command.
 *
 * We return the first slot we find that is marked isFree, if one is;
 * otherwise, we loop on select() until one socket becomes available.  When
 * this happens, we read the whole set and mark as free all sockets that become
 * available.
 *
 * If an error occurs, NULL is returned.
 */
static ParallelSlot *
GetIdleSlot(ParallelSlot slots[], int numslots,
			const char *progname)
{
	int			i;
	int			firstFree = -1;

	/* Any connection already known free? */
	for (i = 0; i < numslots; i++)
	{
		if (slots[i].isFree)
			return slots + i;
	}

	/*
	 * No free slot found, so wait until one of the connections has finished
	 * its task and return the available slot.
	 */
	while (firstFree < 0)
	{
		fd_set		slotset;
		int			maxFd = 0;
		bool		aborting;

		/* We must reconstruct the fd_set for each call to select_loop */
		FD_ZERO(&slotset);

		for (i = 0; i < numslots; i++)
		{
			int			sock = PQsocket(slots[i].connection);

			/*
			 * We don't really expect any connections to lose their sockets
			 * after startup, but just in case, cope by ignoring them.
			 */
			if (sock < 0)
				continue;

			FD_SET(sock, &slotset);
			if (sock > maxFd)
				maxFd = sock;
		}

		SetCancelConn(slots->connection);
		i = select_loop(maxFd, &slotset, &aborting);
		ResetCancelConn();

		if (aborting)
		{
			/*
			 * We set the cancel-receiving connection to the one in the zeroth
			 * slot above, so fetch the error from there.
			 */
			GetQueryResult(slots->connection, progname);
			return NULL;
		}
		Assert(i != 0);

		for (i = 0; i < numslots; i++)
		{
			int			sock = PQsocket(slots[i].connection);

			if (sock >= 0 && FD_ISSET(sock, &slotset))
			{
				/* select() says input is available, so consume it */
				PQconsumeInput(slots[i].connection);
			}

			/* Collect result(s) as long as any are available */
			while (!PQisBusy(slots[i].connection))
			{
				PGresult   *result = PQgetResult(slots[i].connection);

				if (result != NULL)
				{
					/* Check and discard the command result */
					if (!ProcessQueryResult(slots[i].connection, result,
											progname))
						return NULL;
				}
				else
				{
					/* This connection has become idle */
					slots[i].isFree = true;
					if (firstFree < 0)
						firstFree = i;
					break;
				}
			}
		}
	}

	return slots + firstFree;
}

/*
 * ProcessQueryResult
 *
 * Process (and delete) a query result.  Returns true if there's no error,
 * false otherwise -- but errors about trying to vacuum a missing relation
 * are reported and subsequently ignored.
 */
static bool
ProcessQueryResult(PGconn *conn, PGresult *result, const char *progname)
{
	/*
	 * If it's an error, report it.  Errors about a missing table are harmless
	 * so we continue processing; but die for other errors.
	 */
	if (PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		char	   *sqlState = PQresultErrorField(result, PG_DIAG_SQLSTATE);

		pg_log_error("vacuuming of database \"%s\" failed: %s",
					 PQdb(conn), PQerrorMessage(conn));

		if (sqlState && strcmp(sqlState, ERRCODE_UNDEFINED_TABLE) != 0)
		{
			PQclear(result);
			return false;
		}
	}

	PQclear(result);
	return true;
}

/*
 * GetQueryResult
 *
 * Pump the conn till it's dry of results; return false if any are errors.
 * Note that this will block if the conn is busy.
 */
static bool
GetQueryResult(PGconn *conn, const char *progname)
{
	bool		ok = true;
	PGresult   *result;

	SetCancelConn(conn);
	while ((result = PQgetResult(conn)) != NULL)
	{
		if (!ProcessQueryResult(conn, result, progname))
			ok = false;
	}
	ResetCancelConn();
	return ok;
}

/*
 * DisconnectDatabase
 *		Disconnect the connection associated with the given slot
 */
static void
DisconnectDatabase(ParallelSlot *slot)
{
	char		errbuf[256];

	if (!slot->connection)
		return;

	if (PQtransactionStatus(slot->connection) == PQTRANS_ACTIVE)
	{
		PGcancel   *cancel;

		if ((cancel = PQgetCancel(slot->connection)))
		{
			(void) PQcancel(cancel, errbuf, sizeof(errbuf));
			PQfreeCancel(cancel);
		}
	}

	PQfinish(slot->connection);
	slot->connection = NULL;
}

/*
 * Loop on select() until a descriptor from the given set becomes readable.
 *
 * If we get a cancel request while we're waiting, we forego all further
 * processing and set the *aborting flag to true.  The return value must be
 * ignored in this case.  Otherwise, *aborting is set to false.
 */
static int
select_loop(int maxFd, fd_set *workerset, bool *aborting)
{
	int			i;
	fd_set		saveSet = *workerset;

	if (CancelRequested)
	{
		*aborting = true;
		return -1;
	}
	else
		*aborting = false;

	for (;;)
	{
		/*
		 * On Windows, we need to check once in a while for cancel requests;
		 * on other platforms we rely on select() returning when interrupted.
		 */
		struct timeval *tvp;
#ifdef WIN32
		struct timeval tv = {0, 1000000};

		tvp = &tv;
#else
		tvp = NULL;
#endif

		*workerset = saveSet;
		i = select(maxFd + 1, workerset, NULL, NULL, tvp);

#ifdef WIN32
		if (i == SOCKET_ERROR)
		{
			i = -1;

			if (WSAGetLastError() == WSAEINTR)
				errno = EINTR;
		}
#endif

		if (i < 0 && errno == EINTR)
			continue;			/* ignore this */
		if (i < 0 || CancelRequested)
			*aborting = true;	/* but not this */
		if (i == 0)
			continue;			/* timeout (Win32 only) */
		break;
	}

	return i;
}

static void
init_slot(ParallelSlot *slot, PGconn *conn)
{
	slot->connection = conn;
	/* Initially assume connection is idle */
	slot->isFree = true;
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
	printf(_("  -j, --jobs=NUM                  use this many concurrent connections to vacuum\n"));
	printf(_("      --min-mxid-age=MXID_AGE     minimum multixact ID age of tables to vacuum\n"));
	printf(_("      --min-xid-age=XID_AGE       minimum transaction ID age of tables to vacuum\n"));
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
	printf(_("\nReport bugs to <pgsql-bugs@lists.postgresql.org>.\n"));
}
