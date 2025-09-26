/*-------------------------------------------------------------------------
 * vacuuming.c
 *		Helper routines for vacuumdb
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/scripts/vacuuming.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "catalog/pg_attribute_d.h"
#include "catalog/pg_class_d.h"
#include "common/connect.h"
#include "common/logging.h"
#include "fe_utils/cancel.h"
#include "fe_utils/option_utils.h"
#include "fe_utils/parallel_slot.h"
#include "fe_utils/query_utils.h"
#include "fe_utils/string_utils.h"
#include "vacuuming.h"


static int	vacuum_one_database(ConnParams *cparams,
								vacuumingOptions *vacopts,
								int stage,
								SimpleStringList *objects,
								SimpleStringList **found_objs,
								int concurrentCons,
								const char *progname, bool echo, bool quiet);
static int	vacuum_all_databases(ConnParams *cparams,
								 vacuumingOptions *vacopts,
								 SimpleStringList *objects,
								 int concurrentCons,
								 const char *progname, bool echo, bool quiet);
static SimpleStringList *retrieve_objects(PGconn *conn,
										  vacuumingOptions *vacopts,
										  SimpleStringList *objects,
										  bool echo);
static void prepare_vacuum_command(PGconn *conn, PQExpBuffer sql,
								   vacuumingOptions *vacopts, const char *table);
static void run_vacuum_command(PGconn *conn, const char *sql, bool echo,
							   const char *table);

/*
 * Executes vacuum/analyze as indicated.  Returns 0 if the plan is carried
 * to completion, or -1 in case of certain errors (which should hopefully
 * been already reported.)  Other errors are reported via pg_fatal().
 */
int
vacuuming_main(ConnParams *cparams, const char *dbname,
			   const char *maintenance_db, vacuumingOptions *vacopts,
			   SimpleStringList *objects,
			   unsigned int tbl_count, int concurrentCons,
			   const char *progname, bool echo, bool quiet)
{
	setup_cancel_handler(NULL);

	/* Avoid opening extra connections. */
	if (tbl_count > 0 && (concurrentCons > tbl_count))
		concurrentCons = tbl_count;

	if (vacopts->objfilter & OBJFILTER_ALL_DBS)
	{
		cparams->dbname = maintenance_db;

		return vacuum_all_databases(cparams, vacopts,
									objects,
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

		cparams->dbname = dbname;

		if (vacopts->mode == MODE_ANALYZE_IN_STAGES)
		{
			SimpleStringList *found_objs = NULL;

			for (int stage = 0; stage < ANALYZE_NUM_STAGES; stage++)
			{
				int			ret;

				ret = vacuum_one_database(cparams, vacopts,
										  stage,
										  objects,
										  vacopts->missing_stats_only ? &found_objs : NULL,
										  concurrentCons,
										  progname, echo, quiet);
				if (ret != 0)
					return ret;
			}

			return EXIT_SUCCESS;
		}
		else
			return vacuum_one_database(cparams, vacopts,
									   ANALYZE_NO_STAGE,
									   objects, NULL,
									   concurrentCons,
									   progname, echo, quiet);
	}
}

/*
 * vacuum_one_database
 *
 * Process tables in the given database.
 *
 * There are two ways to specify the list of objects to process:
 *
 * 1) The "found_objs" parameter is a double pointer to a fully qualified list
 *    of objects to process, as returned by a previous call to
 *    vacuum_one_database().
 *
 *     a) If both "found_objs" (the double pointer) and "*found_objs" (the
 *        once-dereferenced double pointer) are not NULL, this list takes
 *        priority, and anything specified in "objects" is ignored.
 *
 *     b) If "found_objs" (the double pointer) is not NULL but "*found_objs"
 *        (the once-dereferenced double pointer) _is_ NULL, the "objects"
 *        parameter takes priority, and the results of the catalog query
 *        described in (2) are stored in "found_objs".
 *
 *     c) If "found_objs" (the double pointer) is NULL, the "objects"
 *        parameter again takes priority, and the results of the catalog query
 *        are not saved.
 *
 * 2) The "objects" parameter is a user-specified list of objects to process.
 *    When (1b) or (1c) applies, this function performs a catalog query to
 *    retrieve a fully qualified list of objects to process, as described
 *    below.
 *
 *     a) If "objects" is not NULL, the catalog query gathers only the objects
 *        listed in "objects".
 *
 *     b) If "objects" is NULL, all tables in the database are gathered.
 *
 * Note that this function is only concerned with running exactly one stage
 * when in analyze-in-stages mode; caller must iterate on us if necessary.
 *
 * If concurrentCons is > 1, multiple connections are used to vacuum tables
 * in parallel.
 */
static int
vacuum_one_database(ConnParams *cparams,
					vacuumingOptions *vacopts,
					int stage,
					SimpleStringList *objects,
					SimpleStringList **found_objs,
					int concurrentCons,
					const char *progname, bool echo, bool quiet)
{
	PQExpBufferData sql;
	PGconn	   *conn;
	SimpleStringListCell *cell;
	ParallelSlotArray *sa;
	int			ntups = 0;
	const char *initcmd;
	SimpleStringList *retobjs = NULL;
	int			ret = EXIT_SUCCESS;
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

	if (!vacopts->process_main && PQserverVersion(conn) < 160000)
	{
		PQfinish(conn);
		pg_fatal("cannot use the \"%s\" option on server versions older than PostgreSQL %s",
				 "no-process-main", "16");
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
	{
		PQfinish(conn);
		pg_fatal("cannot use the \"%s\" option on server versions older than PostgreSQL %s",
				 "--min-xid-age", "9.6");
	}

	if (vacopts->min_mxid_age != 0 && PQserverVersion(conn) < 90600)
	{
		PQfinish(conn);
		pg_fatal("cannot use the \"%s\" option on server versions older than PostgreSQL %s",
				 "--min-mxid-age", "9.6");
	}

	if (vacopts->parallel_workers >= 0 && PQserverVersion(conn) < 130000)
	{
		PQfinish(conn);
		pg_fatal("cannot use the \"%s\" option on server versions older than PostgreSQL %s",
				 "--parallel", "13");
	}

	if (vacopts->buffer_usage_limit && PQserverVersion(conn) < 160000)
	{
		PQfinish(conn);
		pg_fatal("cannot use the \"%s\" option on server versions older than PostgreSQL %s",
				 "--buffer-usage-limit", "16");
	}

	if (vacopts->missing_stats_only && PQserverVersion(conn) < 150000)
	{
		PQfinish(conn);
		pg_fatal("cannot use the \"%s\" option on server versions older than PostgreSQL %s",
				 "--missing-stats-only", "15");
	}

	/* skip_database_stats is used automatically if server supports it */
	vacopts->skip_database_stats = (PQserverVersion(conn) >= 160000);

	if (!quiet)
	{
		if (vacopts->mode == MODE_ANALYZE_IN_STAGES)
			printf(_("%s: processing database \"%s\": %s\n"),
				   progname, PQdb(conn), _(stage_messages[stage]));
		else
			printf(_("%s: vacuuming database \"%s\"\n"),
				   progname, PQdb(conn));
		fflush(stdout);
	}

	/*
	 * If the caller provided the results of a previous catalog query, just
	 * use that.  Otherwise, run the catalog query ourselves and set the
	 * return variable if provided.
	 */
	if (found_objs && *found_objs)
		retobjs = *found_objs;
	else
	{
		retobjs = retrieve_objects(conn, vacopts, objects, echo);
		if (found_objs)
			*found_objs = retobjs;
	}

	/*
	 * Count the number of objects in the catalog query result.  If there are
	 * none, we are done.
	 */
	for (cell = retobjs ? retobjs->head : NULL; cell; cell = cell->next)
		ntups++;

	if (ntups == 0)
	{
		PQfinish(conn);
		return EXIT_SUCCESS;
	}

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
	if (vacopts->mode == MODE_ANALYZE_IN_STAGES)
	{
		initcmd = stage_commands[stage];
		executeCommand(conn, initcmd, echo);
	}
	else
		initcmd = NULL;

	/*
	 * Setup the database connections. We reuse the connection we already have
	 * for the first slot.  If not in parallel mode, the first slot in the
	 * array contains the connection.
	 */
	sa = ParallelSlotsSetup(concurrentCons, cparams, progname, echo, initcmd);
	ParallelSlotsAdoptConn(sa, conn);

	initPQExpBuffer(&sql);

	cell = retobjs->head;
	do
	{
		const char *tabname = cell->val;
		ParallelSlot *free_slot;

		if (CancelRequested)
		{
			ret = EXIT_FAILURE;
			goto finish;
		}

		free_slot = ParallelSlotsGetIdle(sa, NULL);
		if (!free_slot)
		{
			ret = EXIT_FAILURE;
			goto finish;
		}

		prepare_vacuum_command(free_slot->connection, &sql,
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
	{
		ret = EXIT_FAILURE;
		goto finish;
	}

	/* If we used SKIP_DATABASE_STATS, mop up with ONLY_DATABASE_STATS */
	if (vacopts->mode == MODE_VACUUM && vacopts->skip_database_stats)
	{
		const char *cmd = "VACUUM (ONLY_DATABASE_STATS);";
		ParallelSlot *free_slot = ParallelSlotsGetIdle(sa, NULL);

		if (!free_slot)
		{
			ret = EXIT_FAILURE;
			goto finish;
		}

		ParallelSlotSetHandler(free_slot, TableCommandResultHandler, NULL);
		run_vacuum_command(free_slot->connection, cmd, echo, NULL);

		if (!ParallelSlotsWaitCompletion(sa))
			ret = EXIT_FAILURE; /* error already reported by handler */
	}

finish:
	ParallelSlotsTerminate(sa);
	pg_free(sa);
	termPQExpBuffer(&sql);

	return ret;
}

/*
 * Vacuum/analyze all connectable databases.
 *
 * In analyze-in-stages mode, we process all databases in one stage before
 * moving on to the next stage.  That ensure minimal stats are available
 * quickly everywhere before generating more detailed ones.
 */
static int
vacuum_all_databases(ConnParams *cparams,
					 vacuumingOptions *vacopts,
					 SimpleStringList *objects,
					 int concurrentCons,
					 const char *progname, bool echo, bool quiet)
{
	PGconn	   *conn;
	PGresult   *result;

	conn = connectMaintenanceDatabase(cparams, progname, echo);
	result = executeQuery(conn,
						  "SELECT datname FROM pg_database WHERE datallowconn AND datconnlimit <> -2 ORDER BY 1;",
						  echo);
	PQfinish(conn);

	if (vacopts->mode == MODE_ANALYZE_IN_STAGES)
	{
		SimpleStringList **found_objs = NULL;

		if (vacopts->missing_stats_only)
			found_objs = palloc0(PQntuples(result) * sizeof(SimpleStringList *));

		/*
		 * When analyzing all databases in stages, we analyze them all in the
		 * fastest stage first, so that initial statistics become available
		 * for all of them as soon as possible.
		 *
		 * This means we establish several times as many connections, but
		 * that's a secondary consideration.
		 */
		for (int stage = 0; stage < ANALYZE_NUM_STAGES; stage++)
		{
			for (int i = 0; i < PQntuples(result); i++)
			{
				int			ret;

				cparams->override_dbname = PQgetvalue(result, i, 0);
				ret = vacuum_one_database(cparams, vacopts, stage,
										  objects,
										  vacopts->missing_stats_only ? &found_objs[i] : NULL,
										  concurrentCons,
										  progname, echo, quiet);
				if (ret != EXIT_SUCCESS)
					return ret;
			}
		}
	}
	else
	{
		for (int i = 0; i < PQntuples(result); i++)
		{
			int			ret;

			cparams->override_dbname = PQgetvalue(result, i, 0);
			ret = vacuum_one_database(cparams, vacopts,
									  ANALYZE_NO_STAGE,
									  objects,
									  NULL,
									  concurrentCons,
									  progname, echo, quiet);
			if (ret != EXIT_SUCCESS)
				return ret;
		}
	}

	PQclear(result);

	return EXIT_SUCCESS;
}

/*
 * Prepare the list of tables to process by querying the catalogs.
 *
 * Since we execute the constructed query with the default search_path (which
 * could be unsafe), everything in this query MUST be fully qualified.
 *
 * First, build a WITH clause for the catalog query if any tables were
 * specified, with a set of values made of relation names and their optional
 * set of columns.  This is used to match any provided column lists with the
 * generated qualified identifiers and to filter for the tables provided via
 * --table.  If a listed table does not exist, the catalog query will fail.
 */
static SimpleStringList *
retrieve_objects(PGconn *conn, vacuumingOptions *vacopts,
				 SimpleStringList *objects, bool echo)
{
	PQExpBufferData buf;
	PQExpBufferData catalog_query;
	PGresult   *res;
	SimpleStringListCell *cell;
	SimpleStringList *found_objs = palloc0(sizeof(SimpleStringList));
	bool		objects_listed = false;

	initPQExpBuffer(&catalog_query);
	for (cell = objects ? objects->head : NULL; cell; cell = cell->next)
	{
		char	   *just_table = NULL;
		const char *just_columns = NULL;

		if (!objects_listed)
		{
			appendPQExpBufferStr(&catalog_query,
								 "WITH listed_objects (object_oid, column_list) AS (\n"
								 "  VALUES (");
			objects_listed = true;
		}
		else
			appendPQExpBufferStr(&catalog_query, ",\n  (");

		if (vacopts->objfilter & (OBJFILTER_SCHEMA | OBJFILTER_SCHEMA_EXCLUDE))
		{
			appendStringLiteralConn(&catalog_query, cell->val, conn);
			appendPQExpBufferStr(&catalog_query, "::pg_catalog.regnamespace, ");
		}

		if (vacopts->objfilter & OBJFILTER_TABLE)
		{
			/*
			 * Split relation and column names given by the user, this is used
			 * to feed the CTE with values on which are performed pre-run
			 * validity checks as well.  For now these happen only on the
			 * relation name.
			 */
			splitTableColumnsSpec(cell->val, PQclientEncoding(conn),
								  &just_table, &just_columns);

			appendStringLiteralConn(&catalog_query, just_table, conn);
			appendPQExpBufferStr(&catalog_query, "::pg_catalog.regclass, ");
		}

		if (just_columns && just_columns[0] != '\0')
			appendStringLiteralConn(&catalog_query, just_columns, conn);
		else
			appendPQExpBufferStr(&catalog_query, "NULL");

		appendPQExpBufferStr(&catalog_query, "::pg_catalog.text)");

		pg_free(just_table);
	}

	/* Finish formatting the CTE */
	if (objects_listed)
		appendPQExpBufferStr(&catalog_query, "\n)\n");

	appendPQExpBufferStr(&catalog_query, "SELECT c.relname, ns.nspname");

	if (objects_listed)
		appendPQExpBufferStr(&catalog_query, ", listed_objects.column_list");

	appendPQExpBufferStr(&catalog_query,
						 " FROM pg_catalog.pg_class c\n"
						 " JOIN pg_catalog.pg_namespace ns"
						 " ON c.relnamespace OPERATOR(pg_catalog.=) ns.oid\n"
						 " CROSS JOIN LATERAL (SELECT c.relkind IN ("
						 CppAsString2(RELKIND_PARTITIONED_TABLE) ", "
						 CppAsString2(RELKIND_PARTITIONED_INDEX) ")) as p (inherited)\n"
						 " LEFT JOIN pg_catalog.pg_class t"
						 " ON c.reltoastrelid OPERATOR(pg_catalog.=) t.oid\n");

	/*
	 * Used to match the tables or schemas listed by the user, completing the
	 * JOIN clause.
	 */
	if (objects_listed)
	{
		appendPQExpBufferStr(&catalog_query, " LEFT JOIN listed_objects"
							 " ON listed_objects.object_oid"
							 " OPERATOR(pg_catalog.=) ");

		if (vacopts->objfilter & OBJFILTER_TABLE)
			appendPQExpBufferStr(&catalog_query, "c.oid\n");
		else
			appendPQExpBufferStr(&catalog_query, "ns.oid\n");
	}

	/*
	 * Exclude temporary tables, beginning the WHERE clause.
	 */
	appendPQExpBufferStr(&catalog_query,
						 " WHERE c.relpersistence OPERATOR(pg_catalog.!=) "
						 CppAsString2(RELPERSISTENCE_TEMP) "\n");

	/*
	 * Used to match the tables or schemas listed by the user, for the WHERE
	 * clause.
	 */
	if (objects_listed)
	{
		if (vacopts->objfilter & OBJFILTER_SCHEMA_EXCLUDE)
			appendPQExpBufferStr(&catalog_query,
								 " AND listed_objects.object_oid IS NULL\n");
		else
			appendPQExpBufferStr(&catalog_query,
								 " AND listed_objects.object_oid IS NOT NULL\n");
	}

	/*
	 * If no tables were listed, filter for the relevant relation types.  If
	 * tables were given via --table, don't bother filtering by relation type.
	 * Instead, let the server decide whether a given relation can be
	 * processed in which case the user will know about it.
	 */
	if ((vacopts->objfilter & OBJFILTER_TABLE) == 0)
	{
		/*
		 * vacuumdb should generally follow the behavior of the underlying
		 * VACUUM and ANALYZE commands.  In MODE_ANALYZE mode, process regular
		 * tables, materialized views, and partitioned tables, just like
		 * ANALYZE (with no specific target tables) does. Otherwise, process
		 * only regular tables and materialized views, since VACUUM skips
		 * partitioned tables when no target tables are specified.
		 */
		if (vacopts->mode == MODE_ANALYZE)
			appendPQExpBufferStr(&catalog_query,
								 " AND c.relkind OPERATOR(pg_catalog.=) ANY (array["
								 CppAsString2(RELKIND_RELATION) ", "
								 CppAsString2(RELKIND_MATVIEW) ", "
								 CppAsString2(RELKIND_PARTITIONED_TABLE) "])\n");
		else
			appendPQExpBufferStr(&catalog_query,
								 " AND c.relkind OPERATOR(pg_catalog.=) ANY (array["
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

	if (vacopts->missing_stats_only)
	{
		appendPQExpBufferStr(&catalog_query, " AND (\n");

		/* regular stats */
		appendPQExpBufferStr(&catalog_query,
							 " EXISTS (SELECT NULL FROM pg_catalog.pg_attribute a\n"
							 " WHERE a.attrelid OPERATOR(pg_catalog.=) c.oid\n"
							 " AND a.attnum OPERATOR(pg_catalog.>) 0::pg_catalog.int2\n"
							 " AND NOT a.attisdropped\n"
							 " AND a.attstattarget IS DISTINCT FROM 0::pg_catalog.int2\n"
							 " AND a.attgenerated OPERATOR(pg_catalog.<>) "
							 CppAsString2(ATTRIBUTE_GENERATED_VIRTUAL) "\n"
							 " AND NOT EXISTS (SELECT NULL FROM pg_catalog.pg_statistic s\n"
							 " WHERE s.starelid OPERATOR(pg_catalog.=) a.attrelid\n"
							 " AND s.staattnum OPERATOR(pg_catalog.=) a.attnum\n"
							 " AND s.stainherit OPERATOR(pg_catalog.=) p.inherited))\n");

		/* extended stats */
		appendPQExpBufferStr(&catalog_query,
							 " OR EXISTS (SELECT NULL FROM pg_catalog.pg_statistic_ext e\n"
							 " WHERE e.stxrelid OPERATOR(pg_catalog.=) c.oid\n"
							 " AND e.stxstattarget IS DISTINCT FROM 0::pg_catalog.int2\n"
							 " AND NOT EXISTS (SELECT NULL FROM pg_catalog.pg_statistic_ext_data d\n"
							 " WHERE d.stxoid OPERATOR(pg_catalog.=) e.oid\n"
							 " AND d.stxdinherit OPERATOR(pg_catalog.=) p.inherited))\n");

		/* expression indexes */
		appendPQExpBufferStr(&catalog_query,
							 " OR EXISTS (SELECT NULL FROM pg_catalog.pg_attribute a\n"
							 " JOIN pg_catalog.pg_index i"
							 " ON i.indexrelid OPERATOR(pg_catalog.=) a.attrelid\n"
							 " WHERE i.indrelid OPERATOR(pg_catalog.=) c.oid\n"
							 " AND i.indkey[a.attnum OPERATOR(pg_catalog.-) 1::pg_catalog.int2]"
							 " OPERATOR(pg_catalog.=) 0::pg_catalog.int2\n"
							 " AND a.attnum OPERATOR(pg_catalog.>) 0::pg_catalog.int2\n"
							 " AND NOT a.attisdropped\n"
							 " AND a.attstattarget IS DISTINCT FROM 0::pg_catalog.int2\n"
							 " AND NOT EXISTS (SELECT NULL FROM pg_catalog.pg_statistic s\n"
							 " WHERE s.starelid OPERATOR(pg_catalog.=) a.attrelid\n"
							 " AND s.staattnum OPERATOR(pg_catalog.=) a.attnum\n"
							 " AND s.stainherit OPERATOR(pg_catalog.=) p.inherited))\n");

		/* inheritance and regular stats */
		appendPQExpBufferStr(&catalog_query,
							 " OR EXISTS (SELECT NULL FROM pg_catalog.pg_attribute a\n"
							 " WHERE a.attrelid OPERATOR(pg_catalog.=) c.oid\n"
							 " AND a.attnum OPERATOR(pg_catalog.>) 0::pg_catalog.int2\n"
							 " AND NOT a.attisdropped\n"
							 " AND a.attstattarget IS DISTINCT FROM 0::pg_catalog.int2\n"
							 " AND a.attgenerated OPERATOR(pg_catalog.<>) "
							 CppAsString2(ATTRIBUTE_GENERATED_VIRTUAL) "\n"
							 " AND c.relhassubclass\n"
							 " AND NOT p.inherited\n"
							 " AND EXISTS (SELECT NULL FROM pg_catalog.pg_inherits h\n"
							 " WHERE h.inhparent OPERATOR(pg_catalog.=) c.oid)\n"
							 " AND NOT EXISTS (SELECT NULL FROM pg_catalog.pg_statistic s\n"
							 " WHERE s.starelid OPERATOR(pg_catalog.=) a.attrelid\n"
							 " AND s.staattnum OPERATOR(pg_catalog.=) a.attnum\n"
							 " AND s.stainherit))\n");

		/* inheritance and extended stats */
		appendPQExpBufferStr(&catalog_query,
							 " OR EXISTS (SELECT NULL FROM pg_catalog.pg_statistic_ext e\n"
							 " WHERE e.stxrelid OPERATOR(pg_catalog.=) c.oid\n"
							 " AND e.stxstattarget IS DISTINCT FROM 0::pg_catalog.int2\n"
							 " AND c.relhassubclass\n"
							 " AND NOT p.inherited\n"
							 " AND EXISTS (SELECT NULL FROM pg_catalog.pg_inherits h\n"
							 " WHERE h.inhparent OPERATOR(pg_catalog.=) c.oid)\n"
							 " AND NOT EXISTS (SELECT NULL FROM pg_catalog.pg_statistic_ext_data d\n"
							 " WHERE d.stxoid OPERATOR(pg_catalog.=) e.oid\n"
							 " AND d.stxdinherit))\n");

		appendPQExpBufferStr(&catalog_query, " )\n");
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
	 * Build qualified identifiers for each table, including the column list
	 * if given.
	 */
	initPQExpBuffer(&buf);
	for (int i = 0; i < PQntuples(res); i++)
	{
		appendPQExpBufferStr(&buf,
							 fmtQualifiedIdEnc(PQgetvalue(res, i, 1),
											   PQgetvalue(res, i, 0),
											   PQclientEncoding(conn)));

		if (objects_listed && !PQgetisnull(res, i, 2))
			appendPQExpBufferStr(&buf, PQgetvalue(res, i, 2));

		simple_string_list_append(found_objs, buf.data);
		resetPQExpBuffer(&buf);
	}
	termPQExpBuffer(&buf);
	PQclear(res);

	return found_objs;
}

/*
 * Construct a vacuum/analyze command to run based on the given
 * options, in the given string buffer, which may contain previous garbage.
 *
 * The table name used must be already properly quoted.  The command generated
 * depends on the server version involved and it is semicolon-terminated.
 */
static void
prepare_vacuum_command(PGconn *conn, PQExpBuffer sql,
					   vacuumingOptions *vacopts, const char *table)
{
	int			serverVersion = PQserverVersion(conn);
	const char *paren = " (";
	const char *comma = ", ";
	const char *sep = paren;

	resetPQExpBuffer(sql);

	if (vacopts->mode == MODE_ANALYZE ||
		vacopts->mode == MODE_ANALYZE_IN_STAGES)
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
			if (vacopts->buffer_usage_limit)
			{
				Assert(serverVersion >= 160000);
				appendPQExpBuffer(sql, "%sBUFFER_USAGE_LIMIT '%s'", sep,
								  vacopts->buffer_usage_limit);
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
			if (!vacopts->process_main)
			{
				/* PROCESS_MAIN is supported since v16 */
				Assert(serverVersion >= 160000);
				appendPQExpBuffer(sql, "%sPROCESS_MAIN FALSE", sep);
				sep = comma;
			}
			if (!vacopts->process_toast)
			{
				/* PROCESS_TOAST is supported since v14 */
				Assert(serverVersion >= 140000);
				appendPQExpBuffer(sql, "%sPROCESS_TOAST FALSE", sep);
				sep = comma;
			}
			if (vacopts->skip_database_stats)
			{
				/* SKIP_DATABASE_STATS is supported since v16 */
				Assert(serverVersion >= 160000);
				appendPQExpBuffer(sql, "%sSKIP_DATABASE_STATS", sep);
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
			if (vacopts->buffer_usage_limit)
			{
				Assert(serverVersion >= 160000);
				appendPQExpBuffer(sql, "%sBUFFER_USAGE_LIMIT '%s'", sep,
								  vacopts->buffer_usage_limit);
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
		{
			pg_log_error("vacuuming of table \"%s\" in database \"%s\" failed: %s",
						 table, PQdb(conn), PQerrorMessage(conn));
		}
		else
		{
			pg_log_error("vacuuming of database \"%s\" failed: %s",
						 PQdb(conn), PQerrorMessage(conn));
		}
	}
}

/*
 * Returns a newly malloc'd version of 'src' with escaped single quotes and
 * backslashes.
 */
char *
escape_quotes(const char *src)
{
	char	   *result = escape_single_quotes_ascii(src);

	if (!result)
		pg_fatal("out of memory");
	return result;
}
