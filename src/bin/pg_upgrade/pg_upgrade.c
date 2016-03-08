/*
 *	pg_upgrade.c
 *
 *	main source file
 *
 *	Copyright (c) 2010-2016, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/pg_upgrade.c
 */

/*
 *	To simplify the upgrade process, we force certain system values to be
 *	identical between old and new clusters:
 *
 *	We control all assignments of pg_class.oid (and relfilenode) so toast
 *	oids are the same between old and new clusters.  This is important
 *	because toast oids are stored as toast pointers in user tables.
 *
 *	While pg_class.oid and pg_class.relfilenode are initially the same
 *	in a cluster, they can diverge due to CLUSTER, REINDEX, or VACUUM
 *	FULL.  In the new cluster, pg_class.oid and pg_class.relfilenode will
 *	be the same and will match the old pg_class.oid value.  Because of
 *	this, old/new pg_class.relfilenode values will not match if CLUSTER,
 *	REINDEX, or VACUUM FULL have been performed in the old cluster.
 *
 *	We control all assignments of pg_type.oid because these oids are stored
 *	in user composite type values.
 *
 *	We control all assignments of pg_enum.oid because these oids are stored
 *	in user tables as enum values.
 *
 *	We control all assignments of pg_authid.oid because these oids are stored
 *	in pg_largeobject_metadata.
 */



#include "postgres_fe.h"

#include "pg_upgrade.h"
#include "common/restricted_token.h"

#ifdef HAVE_LANGINFO_H
#include <langinfo.h>
#endif

static void prepare_new_cluster(void);
static void prepare_new_databases(void);
static void create_new_objects(void);
static void copy_clog_xlog_xid(void);
static void set_frozenxids(bool minmxid_only);
static void setup(char *argv0, bool *live_check);
static void cleanup(void);

ClusterInfo old_cluster,
			new_cluster;
OSInfo		os_info;

char	   *output_files[] = {
	SERVER_LOG_FILE,
#ifdef WIN32
	/* unique file for pg_ctl start */
	SERVER_START_LOG_FILE,
#endif
	UTILITY_LOG_FILE,
	INTERNAL_LOG_FILE,
	NULL
};


int
main(int argc, char **argv)
{
	char	   *analyze_script_file_name = NULL;
	char	   *deletion_script_file_name = NULL;
	bool		live_check = false;

	parseCommandLine(argc, argv);

	get_restricted_token(os_info.progname);

	adjust_data_dir(&old_cluster);
	adjust_data_dir(&new_cluster);

	setup(argv[0], &live_check);

	output_check_banner(live_check);

	check_cluster_versions();

	get_sock_dir(&old_cluster, live_check);
	get_sock_dir(&new_cluster, false);

	check_cluster_compatibility(live_check);

	check_and_dump_old_cluster(live_check);


	/* -- NEW -- */
	start_postmaster(&new_cluster, true);

	check_new_cluster();
	report_clusters_compatible();

	pg_log(PG_REPORT, "\nPerforming Upgrade\n");
	pg_log(PG_REPORT, "------------------\n");

	prepare_new_cluster();

	stop_postmaster(false);

	/*
	 * Destructive Changes to New Cluster
	 */

	copy_clog_xlog_xid();

	/* New now using xids of the old system */

	/* -- NEW -- */
	start_postmaster(&new_cluster, true);

	prepare_new_databases();

	create_new_objects();

	stop_postmaster(false);

	/*
	 * Most failures happen in create_new_objects(), which has completed at
	 * this point.  We do this here because it is just before linking, which
	 * will link the old and new cluster data files, preventing the old
	 * cluster from being safely started once the new cluster is started.
	 */
	if (user_opts.transfer_mode == TRANSFER_MODE_LINK)
		disable_old_cluster();

	transfer_all_new_tablespaces(&old_cluster.dbarr, &new_cluster.dbarr,
								 old_cluster.pgdata, new_cluster.pgdata);

	/*
	 * Assuming OIDs are only used in system tables, there is no need to
	 * restore the OID counter because we have not transferred any OIDs from
	 * the old system, but we do it anyway just in case.  We do it late here
	 * because there is no need to have the schema load use new oids.
	 */
	prep_status("Setting next OID for new cluster");
	exec_prog(UTILITY_LOG_FILE, NULL, true,
			  "\"%s/pg_resetxlog\" -o %u \"%s\"",
			  new_cluster.bindir, old_cluster.controldata.chkpnt_nxtoid,
			  new_cluster.pgdata);
	check_ok();

	prep_status("Sync data directory to disk");
	exec_prog(UTILITY_LOG_FILE, NULL, true,
			  "\"%s/initdb\" --sync-only \"%s\"", new_cluster.bindir,
			  new_cluster.pgdata);
	check_ok();

	create_script_for_cluster_analyze(&analyze_script_file_name);
	create_script_for_old_cluster_deletion(&deletion_script_file_name);

	issue_warnings();

	pg_log(PG_REPORT, "\nUpgrade Complete\n");
	pg_log(PG_REPORT, "----------------\n");

	output_completion_banner(analyze_script_file_name,
							 deletion_script_file_name);

	pg_free(analyze_script_file_name);
	pg_free(deletion_script_file_name);

	cleanup();

	return 0;
}


static void
setup(char *argv0, bool *live_check)
{
	char		exec_path[MAXPGPATH];	/* full path to my executable */

	/*
	 * make sure the user has a clean environment, otherwise, we may confuse
	 * libpq when we connect to one (or both) of the servers.
	 */
	check_pghost_envvar();

	verify_directories();

	/* no postmasters should be running, except for a live check */
	if (pid_lock_file_exists(old_cluster.pgdata))
	{
		/*
		 * If we have a postmaster.pid file, try to start the server.  If it
		 * starts, the pid file was stale, so stop the server.  If it doesn't
		 * start, assume the server is running.  If the pid file is left over
		 * from a server crash, this also allows any committed transactions
		 * stored in the WAL to be replayed so they are not lost, because WAL
		 * files are not transferred from old to new servers.
		 */
		if (start_postmaster(&old_cluster, false))
			stop_postmaster(false);
		else
		{
			if (!user_opts.check)
				pg_fatal("There seems to be a postmaster servicing the old cluster.\n"
						 "Please shutdown that postmaster and try again.\n");
			else
				*live_check = true;
		}
	}

	/* same goes for the new postmaster */
	if (pid_lock_file_exists(new_cluster.pgdata))
	{
		if (start_postmaster(&new_cluster, false))
			stop_postmaster(false);
		else
			pg_fatal("There seems to be a postmaster servicing the new cluster.\n"
					 "Please shutdown that postmaster and try again.\n");
	}

	/* get path to pg_upgrade executable */
	if (find_my_exec(argv0, exec_path) < 0)
		pg_fatal("Could not get path name to pg_upgrade: %s\n", getErrorText());

	/* Trim off program name and keep just path */
	*last_dir_separator(exec_path) = '\0';
	canonicalize_path(exec_path);
	os_info.exec_path = pg_strdup(exec_path);
}


static void
prepare_new_cluster(void)
{
	/*
	 * It would make more sense to freeze after loading the schema, but that
	 * would cause us to lose the frozenids restored by the load. We use
	 * --analyze so autovacuum doesn't update statistics later
	 */
	prep_status("Analyzing all rows in the new cluster");
	exec_prog(UTILITY_LOG_FILE, NULL, true,
			  "\"%s/vacuumdb\" %s --all --analyze %s",
			  new_cluster.bindir, cluster_conn_opts(&new_cluster),
			  log_opts.verbose ? "--verbose" : "");
	check_ok();

	/*
	 * We do freeze after analyze so pg_statistic is also frozen. template0 is
	 * not frozen here, but data rows were frozen by initdb, and we set its
	 * datfrozenxid, relfrozenxids, and relminmxid later to match the new xid
	 * counter later.
	 */
	prep_status("Freezing all rows on the new cluster");
	exec_prog(UTILITY_LOG_FILE, NULL, true,
			  "\"%s/vacuumdb\" %s --all --freeze %s",
			  new_cluster.bindir, cluster_conn_opts(&new_cluster),
			  log_opts.verbose ? "--verbose" : "");
	check_ok();
}


static void
prepare_new_databases(void)
{
	/*
	 * We set autovacuum_freeze_max_age to its maximum value so autovacuum
	 * does not launch here and delete clog files, before the frozen xids are
	 * set.
	 */

	set_frozenxids(false);

	prep_status("Restoring global objects in the new cluster");

	/*
	 * We have to create the databases first so we can install support
	 * functions in all the other databases.  Ideally we could create the
	 * support functions in template1 but pg_dumpall creates database using
	 * the template0 template.
	 */
	exec_prog(UTILITY_LOG_FILE, NULL, true,
			  "\"%s/psql\" " EXEC_PSQL_ARGS " %s -f \"%s\"",
			  new_cluster.bindir, cluster_conn_opts(&new_cluster),
			  GLOBALS_DUMP_FILE);
	check_ok();

	/* we load this to get a current list of databases */
	get_db_and_rel_infos(&new_cluster);
}


static void
create_new_objects(void)
{
	int			dbnum;

	prep_status("Restoring database schemas in the new cluster\n");

	for (dbnum = 0; dbnum < old_cluster.dbarr.ndbs; dbnum++)
	{
		char		sql_file_name[MAXPGPATH],
					log_file_name[MAXPGPATH];
		DbInfo	   *old_db = &old_cluster.dbarr.dbs[dbnum];

		pg_log(PG_STATUS, "%s", old_db->db_name);
		snprintf(sql_file_name, sizeof(sql_file_name), DB_DUMP_FILE_MASK, old_db->db_oid);
		snprintf(log_file_name, sizeof(log_file_name), DB_DUMP_LOG_FILE_MASK, old_db->db_oid);

		/*
		 * pg_dump only produces its output at the end, so there is little
		 * parallelism if using the pipe.
		 */
		parallel_exec_prog(log_file_name,
						   NULL,
						   "\"%s/pg_restore\" %s --exit-on-error --verbose --dbname \"%s\" \"%s\"",
						   new_cluster.bindir,
						   cluster_conn_opts(&new_cluster),
						   old_db->db_name,
						   sql_file_name);
	}

	/* reap all children */
	while (reap_child(true) == true)
		;

	end_progress_output();
	check_ok();

	/*
	 * We don't have minmxids for databases or relations in pre-9.3 clusters,
	 * so set those after we have restores the schemas.
	 */
	if (GET_MAJOR_VERSION(old_cluster.major_version) < 903)
		set_frozenxids(true);

	optionally_create_toast_tables();

	/* regenerate now that we have objects in the databases */
	get_db_and_rel_infos(&new_cluster);
}

/*
 * Delete the given subdirectory contents from the new cluster
 */
static void
remove_new_subdir(char *subdir, bool rmtopdir)
{
	char		new_path[MAXPGPATH];

	prep_status("Deleting files from new %s", subdir);

	snprintf(new_path, sizeof(new_path), "%s/%s", new_cluster.pgdata, subdir);
	if (!rmtree(new_path, rmtopdir))
		pg_fatal("could not delete directory \"%s\"\n", new_path);

	check_ok();
}

/*
 * Copy the files from the old cluster into it
 */
static void
copy_subdir_files(char *subdir)
{
	char		old_path[MAXPGPATH];
	char		new_path[MAXPGPATH];

	remove_new_subdir(subdir, true);

	snprintf(old_path, sizeof(old_path), "%s/%s", old_cluster.pgdata, subdir);
	snprintf(new_path, sizeof(new_path), "%s/%s", new_cluster.pgdata, subdir);

	prep_status("Copying old %s to new server", subdir);

	exec_prog(UTILITY_LOG_FILE, NULL, true,
#ifndef WIN32
			  "cp -Rf \"%s\" \"%s\"",
#else
	/* flags: everything, no confirm, quiet, overwrite read-only */
			  "xcopy /e /y /q /r \"%s\" \"%s\\\"",
#endif
			  old_path, new_path);

	check_ok();
}

static void
copy_clog_xlog_xid(void)
{
	/* copy old commit logs to new data dir */
	copy_subdir_files("pg_clog");

	/* set the next transaction id and epoch of the new cluster */
	prep_status("Setting next transaction ID and epoch for new cluster");
	exec_prog(UTILITY_LOG_FILE, NULL, true,
			  "\"%s/pg_resetxlog\" -f -x %u \"%s\"",
			  new_cluster.bindir, old_cluster.controldata.chkpnt_nxtxid,
			  new_cluster.pgdata);
	exec_prog(UTILITY_LOG_FILE, NULL, true,
			  "\"%s/pg_resetxlog\" -f -e %u \"%s\"",
			  new_cluster.bindir, old_cluster.controldata.chkpnt_nxtepoch,
			  new_cluster.pgdata);
	/* must reset commit timestamp limits also */
	exec_prog(UTILITY_LOG_FILE, NULL, true,
			  "\"%s/pg_resetxlog\" -f -c %u,%u \"%s\"",
			  new_cluster.bindir,
			  old_cluster.controldata.chkpnt_nxtxid,
			  old_cluster.controldata.chkpnt_nxtxid,
			  new_cluster.pgdata);
	check_ok();

	/*
	 * If the old server is before the MULTIXACT_FORMATCHANGE_CAT_VER change
	 * (see pg_upgrade.h) and the new server is after, then we don't copy
	 * pg_multixact files, but we need to reset pg_control so that the new
	 * server doesn't attempt to read multis older than the cutoff value.
	 */
	if (old_cluster.controldata.cat_ver >= MULTIXACT_FORMATCHANGE_CAT_VER &&
		new_cluster.controldata.cat_ver >= MULTIXACT_FORMATCHANGE_CAT_VER)
	{
		copy_subdir_files("pg_multixact/offsets");
		copy_subdir_files("pg_multixact/members");

		prep_status("Setting next multixact ID and offset for new cluster");

		/*
		 * we preserve all files and contents, so we must preserve both "next"
		 * counters here and the oldest multi present on system.
		 */
		exec_prog(UTILITY_LOG_FILE, NULL, true,
				  "\"%s/pg_resetxlog\" -O %u -m %u,%u \"%s\"",
				  new_cluster.bindir,
				  old_cluster.controldata.chkpnt_nxtmxoff,
				  old_cluster.controldata.chkpnt_nxtmulti,
				  old_cluster.controldata.chkpnt_oldstMulti,
				  new_cluster.pgdata);
		check_ok();
	}
	else if (new_cluster.controldata.cat_ver >= MULTIXACT_FORMATCHANGE_CAT_VER)
	{
		/*
		 * Remove offsets/0000 file created by initdb that no longer matches
		 * the new multi-xid value.  "members" starts at zero so no need to
		 * remove it.
		 */
		remove_new_subdir("pg_multixact/offsets", false);

		prep_status("Setting oldest multixact ID on new cluster");

		/*
		 * We don't preserve files in this case, but it's important that the
		 * oldest multi is set to the latest value used by the old system, so
		 * that multixact.c returns the empty set for multis that might be
		 * present on disk.  We set next multi to the value following that; it
		 * might end up wrapped around (i.e. 0) if the old cluster had
		 * next=MaxMultiXactId, but multixact.c can cope with that just fine.
		 */
		exec_prog(UTILITY_LOG_FILE, NULL, true,
				  "\"%s/pg_resetxlog\" -m %u,%u \"%s\"",
				  new_cluster.bindir,
				  old_cluster.controldata.chkpnt_nxtmulti + 1,
				  old_cluster.controldata.chkpnt_nxtmulti,
				  new_cluster.pgdata);
		check_ok();
	}

	/* now reset the wal archives in the new cluster */
	prep_status("Resetting WAL archives");
	exec_prog(UTILITY_LOG_FILE, NULL, true,
	/* use timeline 1 to match controldata and no WAL history file */
			  "\"%s/pg_resetxlog\" -l 00000001%s \"%s\"", new_cluster.bindir,
			  old_cluster.controldata.nextxlogfile + 8,
			  new_cluster.pgdata);
	check_ok();
}


/*
 *	set_frozenxids()
 *
 *	We have frozen all xids, so set datfrozenxid, relfrozenxid, and
 *	relminmxid to be the old cluster's xid counter, which we just set
 *	in the new cluster.  User-table frozenxid and minmxid values will
 *	be set by pg_dump --binary-upgrade, but objects not set by the pg_dump
 *	must have proper frozen counters.
 */
static
void
set_frozenxids(bool minmxid_only)
{
	int			dbnum;
	PGconn	   *conn,
			   *conn_template1;
	PGresult   *dbres;
	int			ntups;
	int			i_datname;
	int			i_datallowconn;

	if (!minmxid_only)
		prep_status("Setting frozenxid and minmxid counters in new cluster");
	else
		prep_status("Setting minmxid counter in new cluster");

	conn_template1 = connectToServer(&new_cluster, "template1");

	if (!minmxid_only)
		/* set pg_database.datfrozenxid */
		PQclear(executeQueryOrDie(conn_template1,
								  "UPDATE pg_catalog.pg_database "
								  "SET	datfrozenxid = '%u'",
								  old_cluster.controldata.chkpnt_nxtxid));

	/* set pg_database.datminmxid */
	PQclear(executeQueryOrDie(conn_template1,
							  "UPDATE pg_catalog.pg_database "
							  "SET	datminmxid = '%u'",
							  old_cluster.controldata.chkpnt_nxtmulti));

	/* get database names */
	dbres = executeQueryOrDie(conn_template1,
							  "SELECT	datname, datallowconn "
							  "FROM	pg_catalog.pg_database");

	i_datname = PQfnumber(dbres, "datname");
	i_datallowconn = PQfnumber(dbres, "datallowconn");

	ntups = PQntuples(dbres);
	for (dbnum = 0; dbnum < ntups; dbnum++)
	{
		char	   *datname = PQgetvalue(dbres, dbnum, i_datname);
		char	   *datallowconn = PQgetvalue(dbres, dbnum, i_datallowconn);

		/*
		 * We must update databases where datallowconn = false, e.g.
		 * template0, because autovacuum increments their datfrozenxids,
		 * relfrozenxids, and relminmxid even if autovacuum is turned off,
		 * and even though all the data rows are already frozen.  To enable
		 * this, we temporarily change datallowconn.
		 */
		if (strcmp(datallowconn, "f") == 0)
			PQclear(executeQueryOrDie(conn_template1,
								"ALTER DATABASE %s ALLOW_CONNECTIONS = true",
									  quote_identifier(datname)));

		conn = connectToServer(&new_cluster, datname);

		if (!minmxid_only)
			/* set pg_class.relfrozenxid */
			PQclear(executeQueryOrDie(conn,
									  "UPDATE	pg_catalog.pg_class "
									  "SET	relfrozenxid = '%u' "
			/* only heap, materialized view, and TOAST are vacuumed */
									  "WHERE	relkind IN ('r', 'm', 't')",
									  old_cluster.controldata.chkpnt_nxtxid));

		/* set pg_class.relminmxid */
		PQclear(executeQueryOrDie(conn,
								  "UPDATE	pg_catalog.pg_class "
								  "SET	relminmxid = '%u' "
		/* only heap, materialized view, and TOAST are vacuumed */
								  "WHERE	relkind IN ('r', 'm', 't')",
								  old_cluster.controldata.chkpnt_nxtmulti));
		PQfinish(conn);

		/* Reset datallowconn flag */
		if (strcmp(datallowconn, "f") == 0)
			PQclear(executeQueryOrDie(conn_template1,
							   "ALTER DATABASE %s ALLOW_CONNECTIONS = false",
									  quote_identifier(datname)));
	}

	PQclear(dbres);

	PQfinish(conn_template1);

	check_ok();
}


static void
cleanup(void)
{
	fclose(log_opts.internal);

	/* Remove dump and log files? */
	if (!log_opts.retain)
	{
		int			dbnum;
		char	  **filename;

		for (filename = output_files; *filename != NULL; filename++)
			unlink(*filename);

		/* remove dump files */
		unlink(GLOBALS_DUMP_FILE);

		if (old_cluster.dbarr.dbs)
			for (dbnum = 0; dbnum < old_cluster.dbarr.ndbs; dbnum++)
			{
				char		sql_file_name[MAXPGPATH],
							log_file_name[MAXPGPATH];
				DbInfo	   *old_db = &old_cluster.dbarr.dbs[dbnum];

				snprintf(sql_file_name, sizeof(sql_file_name), DB_DUMP_FILE_MASK, old_db->db_oid);
				unlink(sql_file_name);

				snprintf(log_file_name, sizeof(log_file_name), DB_DUMP_LOG_FILE_MASK, old_db->db_oid);
				unlink(log_file_name);
			}
	}
}
