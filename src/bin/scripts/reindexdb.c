/*-------------------------------------------------------------------------
 *
 * reindexdb
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 *
 * src/bin/scripts/reindexdb.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

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

typedef enum ReindexType
{
	REINDEX_DATABASE,
	REINDEX_INDEX,
	REINDEX_SCHEMA,
	REINDEX_SYSTEM,
	REINDEX_TABLE
} ReindexType;


static SimpleStringList *get_parallel_object_list(PGconn *conn,
												  ReindexType type,
												  SimpleStringList *user_list,
												  bool echo);
static void reindex_one_database(ConnParams *cparams, ReindexType type,
								 SimpleStringList *user_list,
								 const char *progname,
								 bool echo, bool verbose, bool concurrently,
								 int concurrentCons, const char *tablespace);
static void reindex_all_databases(ConnParams *cparams,
								  const char *progname, bool echo,
								  bool quiet, bool verbose, bool concurrently,
								  int concurrentCons, const char *tablespace);
static void run_reindex_command(PGconn *conn, ReindexType type,
								const char *name, bool echo, bool verbose,
								bool concurrently, bool async,
								const char *tablespace);

static void help(const char *progname);

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
		{"schema", required_argument, NULL, 'S'},
		{"dbname", required_argument, NULL, 'd'},
		{"all", no_argument, NULL, 'a'},
		{"system", no_argument, NULL, 's'},
		{"table", required_argument, NULL, 't'},
		{"index", required_argument, NULL, 'i'},
		{"jobs", required_argument, NULL, 'j'},
		{"verbose", no_argument, NULL, 'v'},
		{"concurrently", no_argument, NULL, 1},
		{"maintenance-db", required_argument, NULL, 2},
		{"tablespace", required_argument, NULL, 3},
		{NULL, 0, NULL, 0}
	};

	const char *progname;
	int			optindex;
	int			c;

	const char *dbname = NULL;
	const char *maintenance_db = NULL;
	const char *host = NULL;
	const char *port = NULL;
	const char *username = NULL;
	const char *tablespace = NULL;
	enum trivalue prompt_password = TRI_DEFAULT;
	ConnParams	cparams;
	bool		syscatalog = false;
	bool		alldb = false;
	bool		echo = false;
	bool		quiet = false;
	bool		verbose = false;
	bool		concurrently = false;
	SimpleStringList indexes = {NULL, NULL};
	SimpleStringList tables = {NULL, NULL};
	SimpleStringList schemas = {NULL, NULL};
	int			concurrentCons = 1;

	pg_logging_init(argv[0]);
	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pgscripts"));

	handle_help_version_opts(argc, argv, "reindexdb", help);

	/* process command-line options */
	while ((c = getopt_long(argc, argv, "h:p:U:wWeqS:d:ast:i:j:v", long_options, &optindex)) != -1)
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
			case 'S':
				simple_string_list_append(&schemas, optarg);
				break;
			case 'd':
				dbname = pg_strdup(optarg);
				break;
			case 'a':
				alldb = true;
				break;
			case 's':
				syscatalog = true;
				break;
			case 't':
				simple_string_list_append(&tables, optarg);
				break;
			case 'i':
				simple_string_list_append(&indexes, optarg);
				break;
			case 'j':
				concurrentCons = atoi(optarg);
				if (concurrentCons <= 0)
				{
					pg_log_error("number of parallel jobs must be at least 1");
					exit(1);
				}
				break;
			case 'v':
				verbose = true;
				break;
			case 1:
				concurrently = true;
				break;
			case 2:
				maintenance_db = pg_strdup(optarg);
				break;
			case 3:
				tablespace = pg_strdup(optarg);
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

	/* fill cparams except for dbname, which is set below */
	cparams.pghost = host;
	cparams.pgport = port;
	cparams.pguser = username;
	cparams.prompt_password = prompt_password;
	cparams.override_dbname = NULL;

	setup_cancel_handler(NULL);

	if (alldb)
	{
		if (dbname)
		{
			pg_log_error("cannot reindex all databases and a specific one at the same time");
			exit(1);
		}
		if (syscatalog)
		{
			pg_log_error("cannot reindex all databases and system catalogs at the same time");
			exit(1);
		}
		if (schemas.head != NULL)
		{
			pg_log_error("cannot reindex specific schema(s) in all databases");
			exit(1);
		}
		if (tables.head != NULL)
		{
			pg_log_error("cannot reindex specific table(s) in all databases");
			exit(1);
		}
		if (indexes.head != NULL)
		{
			pg_log_error("cannot reindex specific index(es) in all databases");
			exit(1);
		}

		cparams.dbname = maintenance_db;

		reindex_all_databases(&cparams, progname, echo, quiet, verbose,
							  concurrently, concurrentCons, tablespace);
	}
	else if (syscatalog)
	{
		if (schemas.head != NULL)
		{
			pg_log_error("cannot reindex specific schema(s) and system catalogs at the same time");
			exit(1);
		}
		if (tables.head != NULL)
		{
			pg_log_error("cannot reindex specific table(s) and system catalogs at the same time");
			exit(1);
		}
		if (indexes.head != NULL)
		{
			pg_log_error("cannot reindex specific index(es) and system catalogs at the same time");
			exit(1);
		}

		if (concurrentCons > 1)
		{
			pg_log_error("cannot use multiple jobs to reindex system catalogs");
			exit(1);
		}

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

		reindex_one_database(&cparams, REINDEX_SYSTEM, NULL,
							 progname, echo, verbose,
							 concurrently, 1, tablespace);
	}
	else
	{
		/*
		 * Index-level REINDEX is not supported with multiple jobs as we
		 * cannot control the concurrent processing of multiple indexes
		 * depending on the same relation.
		 */
		if (concurrentCons > 1 && indexes.head != NULL)
		{
			pg_log_error("cannot use multiple jobs to reindex indexes");
			exit(1);
		}

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

		if (schemas.head != NULL)
			reindex_one_database(&cparams, REINDEX_SCHEMA, &schemas,
								 progname, echo, verbose,
								 concurrently, concurrentCons, tablespace);

		if (indexes.head != NULL)
			reindex_one_database(&cparams, REINDEX_INDEX, &indexes,
								 progname, echo, verbose,
								 concurrently, 1, tablespace);

		if (tables.head != NULL)
			reindex_one_database(&cparams, REINDEX_TABLE, &tables,
								 progname, echo, verbose,
								 concurrently, concurrentCons, tablespace);

		/*
		 * reindex database only if neither index nor table nor schema is
		 * specified
		 */
		if (indexes.head == NULL && tables.head == NULL && schemas.head == NULL)
			reindex_one_database(&cparams, REINDEX_DATABASE, NULL,
								 progname, echo, verbose,
								 concurrently, concurrentCons, tablespace);
	}

	exit(0);
}

static void
reindex_one_database(ConnParams *cparams, ReindexType type,
					 SimpleStringList *user_list,
					 const char *progname, bool echo,
					 bool verbose, bool concurrently, int concurrentCons,
					 const char *tablespace)
{
	PGconn	   *conn;
	SimpleStringListCell *cell;
	bool		parallel = concurrentCons > 1;
	SimpleStringList *process_list = user_list;
	ReindexType process_type = type;
	ParallelSlotArray *sa;
	bool		failed = false;
	int			items_count = 0;

	conn = connectDatabase(cparams, progname, echo, false, false);

	if (concurrently && PQserverVersion(conn) < 120000)
	{
		PQfinish(conn);
		pg_log_error("cannot use the \"%s\" option on server versions older than PostgreSQL %s",
					 "concurrently", "12");
		exit(1);
	}

	if (tablespace && PQserverVersion(conn) < 140000)
	{
		PQfinish(conn);
		pg_log_error("cannot use the \"%s\" option on server versions older than PostgreSQL %s",
					 "tablespace", "14");
		exit(1);
	}

	if (!parallel)
	{
		switch (process_type)
		{
			case REINDEX_DATABASE:
			case REINDEX_SYSTEM:

				/*
				 * Database and system reindexes only need to work on the
				 * database itself, so build a list with a single entry.
				 */
				Assert(user_list == NULL);
				process_list = pg_malloc0(sizeof(SimpleStringList));
				simple_string_list_append(process_list, PQdb(conn));
				break;

			case REINDEX_INDEX:
			case REINDEX_SCHEMA:
			case REINDEX_TABLE:
				Assert(user_list != NULL);
				break;
		}
	}
	else
	{
		switch (process_type)
		{
			case REINDEX_DATABASE:

				/*
				 * Database-wide parallel reindex requires special processing.
				 * If multiple jobs were asked, we have to reindex system
				 * catalogs first as they cannot be processed in parallel.
				 */
				if (concurrently)
					pg_log_warning("cannot reindex system catalogs concurrently, skipping all");
				else
					run_reindex_command(conn, REINDEX_SYSTEM, PQdb(conn), echo,
										verbose, concurrently, false,
										tablespace);

				/* Build a list of relations from the database */
				process_list = get_parallel_object_list(conn, process_type,
														user_list, echo);
				process_type = REINDEX_TABLE;

				/* Bail out if nothing to process */
				if (process_list == NULL)
					return;
				break;

			case REINDEX_SCHEMA:
				Assert(user_list != NULL);

				/* Build a list of relations from all the schemas */
				process_list = get_parallel_object_list(conn, process_type,
														user_list, echo);
				process_type = REINDEX_TABLE;

				/* Bail out if nothing to process */
				if (process_list == NULL)
					return;
				break;

			case REINDEX_SYSTEM:
			case REINDEX_INDEX:
				/* not supported */
				Assert(false);
				break;

			case REINDEX_TABLE:

				/*
				 * Fall through.  The list of items for tables is already
				 * created.
				 */
				break;
		}
	}

	/*
	 * Adjust the number of concurrent connections depending on the items in
	 * the list.  We choose the minimum between the number of concurrent
	 * connections and the number of items in the list.
	 */
	for (cell = process_list->head; cell; cell = cell->next)
	{
		items_count++;

		/* no need to continue if there are more elements than jobs */
		if (items_count >= concurrentCons)
			break;
	}
	concurrentCons = Min(concurrentCons, items_count);
	Assert(concurrentCons > 0);

	Assert(process_list != NULL);

	sa = ParallelSlotsSetup(concurrentCons, cparams, progname, echo, NULL);
	ParallelSlotsAdoptConn(sa, conn);

	cell = process_list->head;
	do
	{
		const char *objname = cell->val;
		ParallelSlot *free_slot = NULL;

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

		ParallelSlotSetHandler(free_slot, TableCommandResultHandler, NULL);
		run_reindex_command(free_slot->connection, process_type, objname,
							echo, verbose, concurrently, true, tablespace);

		cell = cell->next;
	} while (cell != NULL);

	if (!ParallelSlotsWaitCompletion(sa))
		failed = true;

finish:
	if (process_list != user_list)
	{
		simple_string_list_destroy(process_list);
		pg_free(process_list);
	}

	ParallelSlotsTerminate(sa);
	pfree(sa);

	if (failed)
		exit(1);
}

static void
run_reindex_command(PGconn *conn, ReindexType type, const char *name,
					bool echo, bool verbose, bool concurrently, bool async,
					const char *tablespace)
{
	const char *paren = "(";
	const char *comma = ", ";
	const char *sep = paren;
	PQExpBufferData sql;
	bool		status;

	Assert(name);

	/* build the REINDEX query */
	initPQExpBuffer(&sql);

	appendPQExpBufferStr(&sql, "REINDEX ");

	if (verbose)
	{
		appendPQExpBuffer(&sql, "%sVERBOSE", sep);
		sep = comma;
	}

	if (tablespace)
	{
		appendPQExpBuffer(&sql, "%sTABLESPACE %s", sep, fmtId(tablespace));
		sep = comma;
	}

	if (sep != paren)
		appendPQExpBufferStr(&sql, ") ");

	/* object type */
	switch (type)
	{
		case REINDEX_DATABASE:
			appendPQExpBufferStr(&sql, "DATABASE ");
			break;
		case REINDEX_INDEX:
			appendPQExpBufferStr(&sql, "INDEX ");
			break;
		case REINDEX_SCHEMA:
			appendPQExpBufferStr(&sql, "SCHEMA ");
			break;
		case REINDEX_SYSTEM:
			appendPQExpBufferStr(&sql, "SYSTEM ");
			break;
		case REINDEX_TABLE:
			appendPQExpBufferStr(&sql, "TABLE ");
			break;
	}

	/*
	 * Parenthesized grammar is only supported for CONCURRENTLY since
	 * PostgreSQL 14.  Since 12, CONCURRENTLY can be specified after the
	 * object type.
	 */
	if (concurrently)
		appendPQExpBufferStr(&sql, "CONCURRENTLY ");

	/* object name */
	switch (type)
	{
		case REINDEX_DATABASE:
		case REINDEX_SYSTEM:
			appendPQExpBufferStr(&sql, fmtId(name));
			break;
		case REINDEX_INDEX:
		case REINDEX_TABLE:
			appendQualifiedRelation(&sql, name, conn, echo);
			break;
		case REINDEX_SCHEMA:
			appendPQExpBufferStr(&sql, name);
			break;
	}

	/* finish the query */
	appendPQExpBufferChar(&sql, ';');

	if (async)
	{
		if (echo)
			printf("%s\n", sql.data);

		status = PQsendQuery(conn, sql.data) == 1;
	}
	else
		status = executeMaintenanceCommand(conn, sql.data, echo);

	if (!status)
	{
		switch (type)
		{
			case REINDEX_DATABASE:
				pg_log_error("reindexing of database \"%s\" failed: %s",
							 PQdb(conn), PQerrorMessage(conn));
				break;
			case REINDEX_INDEX:
				pg_log_error("reindexing of index \"%s\" in database \"%s\" failed: %s",
							 name, PQdb(conn), PQerrorMessage(conn));
				break;
			case REINDEX_SCHEMA:
				pg_log_error("reindexing of schema \"%s\" in database \"%s\" failed: %s",
							 name, PQdb(conn), PQerrorMessage(conn));
				break;
			case REINDEX_SYSTEM:
				pg_log_error("reindexing of system catalogs in database \"%s\" failed: %s",
							 PQdb(conn), PQerrorMessage(conn));
				break;
			case REINDEX_TABLE:
				pg_log_error("reindexing of table \"%s\" in database \"%s\" failed: %s",
							 name, PQdb(conn), PQerrorMessage(conn));
				break;
		}
		if (!async)
		{
			PQfinish(conn);
			exit(1);
		}
	}

	termPQExpBuffer(&sql);
}

/*
 * Prepare the list of objects to process by querying the catalogs.
 *
 * This function will return a SimpleStringList object containing the entire
 * list of tables in the given database that should be processed by a parallel
 * database-wide reindex (excluding system tables), or NULL if there's no such
 * table.
 */
static SimpleStringList *
get_parallel_object_list(PGconn *conn, ReindexType type,
						 SimpleStringList *user_list, bool echo)
{
	PQExpBufferData catalog_query;
	PQExpBufferData buf;
	PGresult   *res;
	SimpleStringList *tables;
	int			ntups,
				i;

	initPQExpBuffer(&catalog_query);

	/*
	 * The queries here are using a safe search_path, so there's no need to
	 * fully qualify everything.
	 */
	switch (type)
	{
		case REINDEX_DATABASE:
			Assert(user_list == NULL);
			appendPQExpBufferStr(&catalog_query,
								 "SELECT c.relname, ns.nspname\n"
								 " FROM pg_catalog.pg_class c\n"
								 " JOIN pg_catalog.pg_namespace ns"
								 " ON c.relnamespace = ns.oid\n"
								 " WHERE ns.nspname != 'pg_catalog'\n"
								 "   AND c.relkind IN ("
								 CppAsString2(RELKIND_RELATION) ", "
								 CppAsString2(RELKIND_MATVIEW) ")\n"
								 " ORDER BY c.relpages DESC;");
			break;

		case REINDEX_SCHEMA:
			{
				SimpleStringListCell *cell;
				bool		nsp_listed = false;

				Assert(user_list != NULL);

				/*
				 * All the tables from all the listed schemas are grabbed at
				 * once.
				 */
				appendPQExpBufferStr(&catalog_query,
									 "SELECT c.relname, ns.nspname\n"
									 " FROM pg_catalog.pg_class c\n"
									 " JOIN pg_catalog.pg_namespace ns"
									 " ON c.relnamespace = ns.oid\n"
									 " WHERE c.relkind IN ("
									 CppAsString2(RELKIND_RELATION) ", "
									 CppAsString2(RELKIND_MATVIEW) ")\n"
									 " AND ns.nspname IN (");

				for (cell = user_list->head; cell; cell = cell->next)
				{
					const char *nspname = cell->val;

					if (nsp_listed)
						appendPQExpBufferStr(&catalog_query, ", ");
					else
						nsp_listed = true;

					appendStringLiteralConn(&catalog_query, nspname, conn);
				}

				appendPQExpBufferStr(&catalog_query, ")\n"
									 " ORDER BY c.relpages DESC;");
			}
			break;

		case REINDEX_SYSTEM:
		case REINDEX_INDEX:
		case REINDEX_TABLE:
			Assert(false);
			break;
	}

	res = executeQuery(conn, catalog_query.data, echo);
	termPQExpBuffer(&catalog_query);

	/*
	 * If no rows are returned, there are no matching tables, so we are done.
	 */
	ntups = PQntuples(res);
	if (ntups == 0)
	{
		PQclear(res);
		PQfinish(conn);
		return NULL;
	}

	tables = pg_malloc0(sizeof(SimpleStringList));

	/* Build qualified identifiers for each table */
	initPQExpBuffer(&buf);
	for (i = 0; i < ntups; i++)
	{
		appendPQExpBufferStr(&buf,
							 fmtQualifiedId(PQgetvalue(res, i, 1),
											PQgetvalue(res, i, 0)));

		simple_string_list_append(tables, buf.data);
		resetPQExpBuffer(&buf);
	}
	termPQExpBuffer(&buf);
	PQclear(res);

	return tables;
}

static void
reindex_all_databases(ConnParams *cparams,
					  const char *progname, bool echo, bool quiet, bool verbose,
					  bool concurrently, int concurrentCons,
					  const char *tablespace)
{
	PGconn	   *conn;
	PGresult   *result;
	int			i;

	conn = connectMaintenanceDatabase(cparams, progname, echo);
	result = executeQuery(conn, "SELECT datname FROM pg_database WHERE datallowconn ORDER BY 1;", echo);
	PQfinish(conn);

	for (i = 0; i < PQntuples(result); i++)
	{
		char	   *dbname = PQgetvalue(result, i, 0);

		if (!quiet)
		{
			printf(_("%s: reindexing database \"%s\"\n"), progname, dbname);
			fflush(stdout);
		}

		cparams->override_dbname = dbname;

		reindex_one_database(cparams, REINDEX_DATABASE, NULL,
							 progname, echo, verbose, concurrently,
							 concurrentCons, tablespace);
	}

	PQclear(result);
}

static void
help(const char *progname)
{
	printf(_("%s reindexes a PostgreSQL database.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [DBNAME]\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -a, --all                    reindex all databases\n"));
	printf(_("      --concurrently           reindex concurrently\n"));
	printf(_("  -d, --dbname=DBNAME          database to reindex\n"));
	printf(_("  -e, --echo                   show the commands being sent to the server\n"));
	printf(_("  -i, --index=INDEX            recreate specific index(es) only\n"));
	printf(_("  -j, --jobs=NUM               use this many concurrent connections to reindex\n"));
	printf(_("  -q, --quiet                  don't write any messages\n"));
	printf(_("  -s, --system                 reindex system catalogs\n"));
	printf(_("  -S, --schema=SCHEMA          reindex specific schema(s) only\n"));
	printf(_("  -t, --table=TABLE            reindex specific table(s) only\n"));
	printf(_("      --tablespace=TABLESPACE  tablespace where indexes are rebuilt\n"));
	printf(_("  -v, --verbose                write a lot of output\n"));
	printf(_("  -V, --version                output version information, then exit\n"));
	printf(_("  -?, --help                   show this help, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME          database server host or socket directory\n"));
	printf(_("  -p, --port=PORT              database server port\n"));
	printf(_("  -U, --username=USERNAME      user name to connect as\n"));
	printf(_("  -w, --no-password            never prompt for password\n"));
	printf(_("  -W, --password               force password prompt\n"));
	printf(_("  --maintenance-db=DBNAME      alternate maintenance database\n"));
	printf(_("\nRead the description of the SQL command REINDEX for details.\n"));
	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}
