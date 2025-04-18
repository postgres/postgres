/*
 *	pg_upgrade.c
 *
 *	main source file
 *
 *	Copyright (c) 2010-2025, PostgreSQL Global Development Group
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
 *	While pg_class.oid and pg_class.relfilenode are initially the same in a
 *	cluster, they can diverge due to CLUSTER, REINDEX, or VACUUM FULL. We
 *	control assignments of pg_class.relfilenode because we want the filenames
 *	to match between the old and new cluster.
 *
 *	We control assignment of pg_tablespace.oid because we want the oid to match
 *	between the old and new cluster.
 *
 *	We control all assignments of pg_type.oid because these oids are stored
 *	in user composite type values.
 *
 *	We control all assignments of pg_enum.oid because these oids are stored
 *	in user tables as enum values.
 *
 *	We control all assignments of pg_authid.oid for historical reasons (the
 *	oids used to be stored in pg_largeobject_metadata, which is now copied via
 *	SQL commands), that might change at some point in the future.
 *
 *	We control all assignments of pg_database.oid because we want the directory
 *	names to match between the old and new cluster.
 */



#include "postgres_fe.h"

#include <time.h>

#include "catalog/pg_class_d.h"
#include "common/file_perm.h"
#include "common/logging.h"
#include "common/restricted_token.h"
#include "fe_utils/string_utils.h"
#include "pg_upgrade.h"

/*
 * Maximum number of pg_restore actions (TOC entries) to process within one
 * transaction.  At some point we might want to make this user-controllable,
 * but for now a hard-wired setting will suffice.
 */
#define RESTORE_TRANSACTION_SIZE 1000

static void set_new_cluster_char_signedness(void);
static void set_locale_and_encoding(void);
static void prepare_new_cluster(void);
static void prepare_new_globals(void);
static void create_new_objects(void);
static void copy_xact_xlog_xid(void);
static void set_frozenxids(bool minmxid_only);
static void make_outputdirs(char *pgdata);
static void setup(char *argv0);
static void create_logical_replication_slots(void);

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
	char	   *deletion_script_file_name = NULL;

	/*
	 * pg_upgrade doesn't currently use common/logging.c, but initialize it
	 * anyway because we might call common code that does.
	 */
	pg_logging_init(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_upgrade"));

	/* Set default restrictive mask until new cluster permissions are read */
	umask(PG_MODE_MASK_OWNER);

	parseCommandLine(argc, argv);

	get_restricted_token();

	adjust_data_dir(&old_cluster);
	adjust_data_dir(&new_cluster);

	/*
	 * Set mask based on PGDATA permissions, needed for the creation of the
	 * output directories with correct permissions.
	 */
	if (!GetDataDirectoryCreatePerm(new_cluster.pgdata))
		pg_fatal("could not read permissions of directory \"%s\": %m",
				 new_cluster.pgdata);

	umask(pg_mode_mask);

	/*
	 * This needs to happen after adjusting the data directory of the new
	 * cluster in adjust_data_dir().
	 */
	make_outputdirs(new_cluster.pgdata);

	setup(argv[0]);

	output_check_banner();

	check_cluster_versions();

	get_sock_dir(&old_cluster);
	get_sock_dir(&new_cluster);

	check_cluster_compatibility();

	check_and_dump_old_cluster();


	/* -- NEW -- */
	start_postmaster(&new_cluster, true);

	check_new_cluster();
	report_clusters_compatible();

	pg_log(PG_REPORT,
		   "\n"
		   "Performing Upgrade\n"
		   "------------------");

	set_locale_and_encoding();

	prepare_new_cluster();

	stop_postmaster(false);

	/*
	 * Destructive Changes to New Cluster
	 */

	copy_xact_xlog_xid();
	set_new_cluster_char_signedness();

	/* New now using xids of the old system */

	/* -- NEW -- */
	start_postmaster(&new_cluster, true);

	prepare_new_globals();

	create_new_objects();

	stop_postmaster(false);

	/*
	 * Most failures happen in create_new_objects(), which has completed at
	 * this point.  We do this here because it is just before file transfer,
	 * which for --link will make it unsafe to start the old cluster once the
	 * new cluster is started, and for --swap will make it unsafe to start the
	 * old cluster at all.
	 */
	if (user_opts.transfer_mode == TRANSFER_MODE_LINK ||
		user_opts.transfer_mode == TRANSFER_MODE_SWAP)
		disable_old_cluster(user_opts.transfer_mode);

	transfer_all_new_tablespaces(&old_cluster.dbarr, &new_cluster.dbarr,
								 old_cluster.pgdata, new_cluster.pgdata);

	/*
	 * Assuming OIDs are only used in system tables, there is no need to
	 * restore the OID counter because we have not transferred any OIDs from
	 * the old system, but we do it anyway just in case.  We do it late here
	 * because there is no need to have the schema load use new oids.
	 */
	prep_status("Setting next OID for new cluster");
	exec_prog(UTILITY_LOG_FILE, NULL, true, true,
			  "\"%s/pg_resetwal\" -o %u \"%s\"",
			  new_cluster.bindir, old_cluster.controldata.chkpnt_nxtoid,
			  new_cluster.pgdata);
	check_ok();

	/*
	 * Migrate the logical slots to the new cluster.  Note that we need to do
	 * this after resetting WAL because otherwise the required WAL would be
	 * removed and slots would become unusable.  There is a possibility that
	 * background processes might generate some WAL before we could create the
	 * slots in the new cluster but we can ignore that WAL as that won't be
	 * required downstream.
	 */
	if (count_old_cluster_logical_slots())
	{
		start_postmaster(&new_cluster, true);
		create_logical_replication_slots();
		stop_postmaster(false);
	}

	if (user_opts.do_sync)
	{
		prep_status("Sync data directory to disk");
		exec_prog(UTILITY_LOG_FILE, NULL, true, true,
				  "\"%s/initdb\" --sync-only %s \"%s\" --sync-method %s",
				  new_cluster.bindir,
				  (user_opts.transfer_mode == TRANSFER_MODE_SWAP) ?
				  "--no-sync-data-files" : "",
				  new_cluster.pgdata,
				  user_opts.sync_method);
		check_ok();
	}

	create_script_for_old_cluster_deletion(&deletion_script_file_name);

	issue_warnings_and_set_wal_level();

	pg_log(PG_REPORT,
		   "\n"
		   "Upgrade Complete\n"
		   "----------------");

	output_completion_banner(deletion_script_file_name);

	pg_free(deletion_script_file_name);

	cleanup_output_dirs();

	return 0;
}

/*
 * Create and assign proper permissions to the set of output directories
 * used to store any data generated internally, filling in log_opts in
 * the process.
 */
static void
make_outputdirs(char *pgdata)
{
	FILE	   *fp;
	char	  **filename;
	time_t		run_time = time(NULL);
	char		filename_path[MAXPGPATH];
	char		timebuf[128];
	struct timeval time;
	time_t		tt;
	int			len;

	log_opts.rootdir = (char *) pg_malloc0(MAXPGPATH);
	len = snprintf(log_opts.rootdir, MAXPGPATH, "%s/%s", pgdata, BASE_OUTPUTDIR);
	if (len >= MAXPGPATH)
		pg_fatal("directory path for new cluster is too long");

	/* BASE_OUTPUTDIR/$timestamp/ */
	gettimeofday(&time, NULL);
	tt = (time_t) time.tv_sec;
	strftime(timebuf, sizeof(timebuf), "%Y%m%dT%H%M%S", localtime(&tt));
	/* append milliseconds */
	snprintf(timebuf + strlen(timebuf), sizeof(timebuf) - strlen(timebuf),
			 ".%03d", (int) (time.tv_usec / 1000));
	log_opts.basedir = (char *) pg_malloc0(MAXPGPATH);
	len = snprintf(log_opts.basedir, MAXPGPATH, "%s/%s", log_opts.rootdir,
				   timebuf);
	if (len >= MAXPGPATH)
		pg_fatal("directory path for new cluster is too long");

	/* BASE_OUTPUTDIR/$timestamp/dump/ */
	log_opts.dumpdir = (char *) pg_malloc0(MAXPGPATH);
	len = snprintf(log_opts.dumpdir, MAXPGPATH, "%s/%s/%s", log_opts.rootdir,
				   timebuf, DUMP_OUTPUTDIR);
	if (len >= MAXPGPATH)
		pg_fatal("directory path for new cluster is too long");

	/* BASE_OUTPUTDIR/$timestamp/log/ */
	log_opts.logdir = (char *) pg_malloc0(MAXPGPATH);
	len = snprintf(log_opts.logdir, MAXPGPATH, "%s/%s/%s", log_opts.rootdir,
				   timebuf, LOG_OUTPUTDIR);
	if (len >= MAXPGPATH)
		pg_fatal("directory path for new cluster is too long");

	/*
	 * Ignore the error case where the root path exists, as it is kept the
	 * same across runs.
	 */
	if (mkdir(log_opts.rootdir, pg_dir_create_mode) < 0 && errno != EEXIST)
		pg_fatal("could not create directory \"%s\": %m", log_opts.rootdir);
	if (mkdir(log_opts.basedir, pg_dir_create_mode) < 0)
		pg_fatal("could not create directory \"%s\": %m", log_opts.basedir);
	if (mkdir(log_opts.dumpdir, pg_dir_create_mode) < 0)
		pg_fatal("could not create directory \"%s\": %m", log_opts.dumpdir);
	if (mkdir(log_opts.logdir, pg_dir_create_mode) < 0)
		pg_fatal("could not create directory \"%s\": %m", log_opts.logdir);

	len = snprintf(filename_path, sizeof(filename_path), "%s/%s",
				   log_opts.logdir, INTERNAL_LOG_FILE);
	if (len >= sizeof(filename_path))
		pg_fatal("directory path for new cluster is too long");

	if ((log_opts.internal = fopen_priv(filename_path, "a")) == NULL)
		pg_fatal("could not open log file \"%s\": %m", filename_path);

	/* label start of upgrade in logfiles */
	for (filename = output_files; *filename != NULL; filename++)
	{
		len = snprintf(filename_path, sizeof(filename_path), "%s/%s",
					   log_opts.logdir, *filename);
		if (len >= sizeof(filename_path))
			pg_fatal("directory path for new cluster is too long");
		if ((fp = fopen_priv(filename_path, "a")) == NULL)
			pg_fatal("could not write to log file \"%s\": %m", filename_path);

		fprintf(fp,
				"-----------------------------------------------------------------\n"
				"  pg_upgrade run on %s"
				"-----------------------------------------------------------------\n\n",
				ctime(&run_time));
		fclose(fp);
	}
}


static void
setup(char *argv0)
{
	/*
	 * make sure the user has a clean environment, otherwise, we may confuse
	 * libpq when we connect to one (or both) of the servers.
	 */
	check_pghost_envvar();

	/*
	 * In case the user hasn't specified the directory for the new binaries
	 * with -B, default to using the path of the currently executed pg_upgrade
	 * binary.
	 */
	if (!new_cluster.bindir)
	{
		char		exec_path[MAXPGPATH];

		if (find_my_exec(argv0, exec_path) < 0)
			pg_fatal("%s: could not find own program executable", argv0);
		/* Trim off program name and keep just path */
		*last_dir_separator(exec_path) = '\0';
		canonicalize_path(exec_path);
		new_cluster.bindir = pg_strdup(exec_path);
	}

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
		 * files are not transferred from old to new servers.  We later check
		 * for a clean shutdown.
		 */
		if (start_postmaster(&old_cluster, false))
			stop_postmaster(false);
		else
		{
			if (!user_opts.check)
				pg_fatal("There seems to be a postmaster servicing the old cluster.\n"
						 "Please shutdown that postmaster and try again.");
			else
				user_opts.live_check = true;
		}
	}

	/* same goes for the new postmaster */
	if (pid_lock_file_exists(new_cluster.pgdata))
	{
		if (start_postmaster(&new_cluster, false))
			stop_postmaster(false);
		else
			pg_fatal("There seems to be a postmaster servicing the new cluster.\n"
					 "Please shutdown that postmaster and try again.");
	}
}

/*
 * Set the new cluster's default char signedness using the old cluster's
 * value.
 */
static void
set_new_cluster_char_signedness(void)
{
	bool		new_char_signedness;

	/*
	 * Use the specified char signedness if specified. Otherwise we inherit
	 * the source database's signedness.
	 */
	if (user_opts.char_signedness != -1)
		new_char_signedness = (user_opts.char_signedness == 1);
	else
		new_char_signedness = old_cluster.controldata.default_char_signedness;

	/* Change the char signedness of the new cluster, if necessary */
	if (new_cluster.controldata.default_char_signedness != new_char_signedness)
	{
		prep_status("Setting the default char signedness for new cluster");

		exec_prog(UTILITY_LOG_FILE, NULL, true, true,
				  "\"%s/pg_resetwal\" --char-signedness %s \"%s\"",
				  new_cluster.bindir,
				  new_char_signedness ? "signed" : "unsigned",
				  new_cluster.pgdata);

		check_ok();
	}
}

/*
 * Copy locale and encoding information into the new cluster's template0.
 *
 * We need to copy the encoding, datlocprovider, datcollate, datctype, and
 * datlocale. We don't need datcollversion because that's never set for
 * template0.
 */
static void
set_locale_and_encoding(void)
{
	PGconn	   *conn_new_template1;
	char	   *datcollate_literal;
	char	   *datctype_literal;
	char	   *datlocale_literal = NULL;
	DbLocaleInfo *locale = old_cluster.template0;

	prep_status("Setting locale and encoding for new cluster");

	/* escape literals with respect to new cluster */
	conn_new_template1 = connectToServer(&new_cluster, "template1");

	datcollate_literal = PQescapeLiteral(conn_new_template1,
										 locale->db_collate,
										 strlen(locale->db_collate));
	datctype_literal = PQescapeLiteral(conn_new_template1,
									   locale->db_ctype,
									   strlen(locale->db_ctype));

	if (locale->db_locale)
		datlocale_literal = PQescapeLiteral(conn_new_template1,
											locale->db_locale,
											strlen(locale->db_locale));
	else
		datlocale_literal = "NULL";

	/* update template0 in new cluster */
	if (GET_MAJOR_VERSION(new_cluster.major_version) >= 1700)
		PQclear(executeQueryOrDie(conn_new_template1,
								  "UPDATE pg_catalog.pg_database "
								  "  SET encoding = %d, "
								  "      datlocprovider = '%c', "
								  "      datcollate = %s, "
								  "      datctype = %s, "
								  "      datlocale = %s "
								  "  WHERE datname = 'template0' ",
								  locale->db_encoding,
								  locale->db_collprovider,
								  datcollate_literal,
								  datctype_literal,
								  datlocale_literal));
	else if (GET_MAJOR_VERSION(new_cluster.major_version) >= 1500)
		PQclear(executeQueryOrDie(conn_new_template1,
								  "UPDATE pg_catalog.pg_database "
								  "  SET encoding = %d, "
								  "      datlocprovider = '%c', "
								  "      datcollate = %s, "
								  "      datctype = %s, "
								  "      daticulocale = %s "
								  "  WHERE datname = 'template0' ",
								  locale->db_encoding,
								  locale->db_collprovider,
								  datcollate_literal,
								  datctype_literal,
								  datlocale_literal));
	else
		PQclear(executeQueryOrDie(conn_new_template1,
								  "UPDATE pg_catalog.pg_database "
								  "  SET encoding = %d, "
								  "      datcollate = %s, "
								  "      datctype = %s "
								  "  WHERE datname = 'template0' ",
								  locale->db_encoding,
								  datcollate_literal,
								  datctype_literal));

	PQfreemem(datcollate_literal);
	PQfreemem(datctype_literal);
	if (locale->db_locale)
		PQfreemem(datlocale_literal);

	PQfinish(conn_new_template1);

	check_ok();
}


static void
prepare_new_cluster(void)
{
	/*
	 * It would make more sense to freeze after loading the schema, but that
	 * would cause us to lose the frozenxids restored by the load. We use
	 * --analyze so autovacuum doesn't update statistics later
	 */
	prep_status("Analyzing all rows in the new cluster");
	exec_prog(UTILITY_LOG_FILE, NULL, true, true,
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
	prep_status("Freezing all rows in the new cluster");
	exec_prog(UTILITY_LOG_FILE, NULL, true, true,
			  "\"%s/vacuumdb\" %s --all --freeze %s",
			  new_cluster.bindir, cluster_conn_opts(&new_cluster),
			  log_opts.verbose ? "--verbose" : "");
	check_ok();
}


static void
prepare_new_globals(void)
{
	/*
	 * Before we restore anything, set frozenxids of initdb-created tables.
	 */
	set_frozenxids(false);

	/*
	 * Now restore global objects (roles and tablespaces).
	 */
	prep_status("Restoring global objects in the new cluster");

	exec_prog(UTILITY_LOG_FILE, NULL, true, true,
			  "\"%s/psql\" " EXEC_PSQL_ARGS " %s -f \"%s/%s\"",
			  new_cluster.bindir, cluster_conn_opts(&new_cluster),
			  log_opts.dumpdir,
			  GLOBALS_DUMP_FILE);
	check_ok();
}


static void
create_new_objects(void)
{
	int			dbnum;
	PGconn	   *conn_new_template1;

	prep_status_progress("Restoring database schemas in the new cluster");

	/*
	 * Ensure that any changes to template0 are fully written out to disk
	 * prior to restoring the databases.  This is necessary because we use the
	 * FILE_COPY strategy to create the databases (which testing has shown to
	 * be faster), and when the server is in binary upgrade mode, it skips the
	 * checkpoints this strategy ordinarily performs.
	 */
	conn_new_template1 = connectToServer(&new_cluster, "template1");
	PQclear(executeQueryOrDie(conn_new_template1, "CHECKPOINT"));
	PQfinish(conn_new_template1);

	/*
	 * We cannot process the template1 database concurrently with others,
	 * because when it's transiently dropped, connection attempts would fail.
	 * So handle it in a separate non-parallelized pass.
	 */
	for (dbnum = 0; dbnum < old_cluster.dbarr.ndbs; dbnum++)
	{
		char		sql_file_name[MAXPGPATH],
					log_file_name[MAXPGPATH];
		DbInfo	   *old_db = &old_cluster.dbarr.dbs[dbnum];
		const char *create_opts;

		/* Process only template1 in this pass */
		if (strcmp(old_db->db_name, "template1") != 0)
			continue;

		pg_log(PG_STATUS, "%s", old_db->db_name);
		snprintf(sql_file_name, sizeof(sql_file_name), DB_DUMP_FILE_MASK, old_db->db_oid);
		snprintf(log_file_name, sizeof(log_file_name), DB_DUMP_LOG_FILE_MASK, old_db->db_oid);

		/*
		 * template1 database will already exist in the target installation,
		 * so tell pg_restore to drop and recreate it; otherwise we would fail
		 * to propagate its database-level properties.
		 */
		create_opts = "--clean --create";

		exec_prog(log_file_name,
				  NULL,
				  true,
				  true,
				  "\"%s/pg_restore\" %s %s --exit-on-error --verbose "
				  "--transaction-size=%d "
				  "--dbname postgres \"%s/%s\"",
				  new_cluster.bindir,
				  cluster_conn_opts(&new_cluster),
				  create_opts,
				  RESTORE_TRANSACTION_SIZE,
				  log_opts.dumpdir,
				  sql_file_name);

		break;					/* done once we've processed template1 */
	}

	for (dbnum = 0; dbnum < old_cluster.dbarr.ndbs; dbnum++)
	{
		char		sql_file_name[MAXPGPATH],
					log_file_name[MAXPGPATH];
		DbInfo	   *old_db = &old_cluster.dbarr.dbs[dbnum];
		const char *create_opts;
		int			txn_size;

		/* Skip template1 in this pass */
		if (strcmp(old_db->db_name, "template1") == 0)
			continue;

		pg_log(PG_STATUS, "%s", old_db->db_name);
		snprintf(sql_file_name, sizeof(sql_file_name), DB_DUMP_FILE_MASK, old_db->db_oid);
		snprintf(log_file_name, sizeof(log_file_name), DB_DUMP_LOG_FILE_MASK, old_db->db_oid);

		/*
		 * postgres database will already exist in the target installation, so
		 * tell pg_restore to drop and recreate it; otherwise we would fail to
		 * propagate its database-level properties.
		 */
		if (strcmp(old_db->db_name, "postgres") == 0)
			create_opts = "--clean --create";
		else
			create_opts = "--create";

		/*
		 * In parallel mode, reduce the --transaction-size of each restore job
		 * so that the total number of locks that could be held across all the
		 * jobs stays in bounds.
		 */
		txn_size = RESTORE_TRANSACTION_SIZE;
		if (user_opts.jobs > 1)
		{
			txn_size /= user_opts.jobs;
			/* Keep some sanity if -j is huge */
			txn_size = Max(txn_size, 10);
		}

		parallel_exec_prog(log_file_name,
						   NULL,
						   "\"%s/pg_restore\" %s %s --exit-on-error --verbose "
						   "--transaction-size=%d "
						   "--dbname template1 \"%s/%s\"",
						   new_cluster.bindir,
						   cluster_conn_opts(&new_cluster),
						   create_opts,
						   txn_size,
						   log_opts.dumpdir,
						   sql_file_name);
	}

	/* reap all children */
	while (reap_child(true) == true)
		;

	end_progress_output();
	check_ok();

	/*
	 * We don't have minmxids for databases or relations in pre-9.3 clusters,
	 * so set those after we have restored the schema.
	 */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 902)
		set_frozenxids(true);

	/* update new_cluster info now that we have objects in the databases */
	get_db_rel_and_slot_infos(&new_cluster);
}

/*
 * Delete the given subdirectory contents from the new cluster
 */
static void
remove_new_subdir(const char *subdir, bool rmtopdir)
{
	char		new_path[MAXPGPATH];

	prep_status("Deleting files from new %s", subdir);

	snprintf(new_path, sizeof(new_path), "%s/%s", new_cluster.pgdata, subdir);
	if (!rmtree(new_path, rmtopdir))
		pg_fatal("could not delete directory \"%s\"", new_path);

	check_ok();
}

/*
 * Copy the files from the old cluster into it
 */
static void
copy_subdir_files(const char *old_subdir, const char *new_subdir)
{
	char		old_path[MAXPGPATH];
	char		new_path[MAXPGPATH];

	remove_new_subdir(new_subdir, true);

	snprintf(old_path, sizeof(old_path), "%s/%s", old_cluster.pgdata, old_subdir);
	snprintf(new_path, sizeof(new_path), "%s/%s", new_cluster.pgdata, new_subdir);

	prep_status("Copying old %s to new server", old_subdir);

	exec_prog(UTILITY_LOG_FILE, NULL, true, true,
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
copy_xact_xlog_xid(void)
{
	/*
	 * Copy old commit logs to new data dir. pg_clog has been renamed to
	 * pg_xact in post-10 clusters.
	 */
	copy_subdir_files(GET_MAJOR_VERSION(old_cluster.major_version) <= 906 ?
					  "pg_clog" : "pg_xact",
					  GET_MAJOR_VERSION(new_cluster.major_version) <= 906 ?
					  "pg_clog" : "pg_xact");

	prep_status("Setting oldest XID for new cluster");
	exec_prog(UTILITY_LOG_FILE, NULL, true, true,
			  "\"%s/pg_resetwal\" -f -u %u \"%s\"",
			  new_cluster.bindir, old_cluster.controldata.chkpnt_oldstxid,
			  new_cluster.pgdata);
	check_ok();

	/* set the next transaction id and epoch of the new cluster */
	prep_status("Setting next transaction ID and epoch for new cluster");
	exec_prog(UTILITY_LOG_FILE, NULL, true, true,
			  "\"%s/pg_resetwal\" -f -x %u \"%s\"",
			  new_cluster.bindir, old_cluster.controldata.chkpnt_nxtxid,
			  new_cluster.pgdata);
	exec_prog(UTILITY_LOG_FILE, NULL, true, true,
			  "\"%s/pg_resetwal\" -f -e %u \"%s\"",
			  new_cluster.bindir, old_cluster.controldata.chkpnt_nxtepoch,
			  new_cluster.pgdata);
	/* must reset commit timestamp limits also */
	exec_prog(UTILITY_LOG_FILE, NULL, true, true,
			  "\"%s/pg_resetwal\" -f -c %u,%u \"%s\"",
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
		copy_subdir_files("pg_multixact/offsets", "pg_multixact/offsets");
		copy_subdir_files("pg_multixact/members", "pg_multixact/members");

		prep_status("Setting next multixact ID and offset for new cluster");

		/*
		 * we preserve all files and contents, so we must preserve both "next"
		 * counters here and the oldest multi present on system.
		 */
		exec_prog(UTILITY_LOG_FILE, NULL, true, true,
				  "\"%s/pg_resetwal\" -O %u -m %u,%u \"%s\"",
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

		prep_status("Setting oldest multixact ID in new cluster");

		/*
		 * We don't preserve files in this case, but it's important that the
		 * oldest multi is set to the latest value used by the old system, so
		 * that multixact.c returns the empty set for multis that might be
		 * present on disk.  We set next multi to the value following that; it
		 * might end up wrapped around (i.e. 0) if the old cluster had
		 * next=MaxMultiXactId, but multixact.c can cope with that just fine.
		 */
		exec_prog(UTILITY_LOG_FILE, NULL, true, true,
				  "\"%s/pg_resetwal\" -m %u,%u \"%s\"",
				  new_cluster.bindir,
				  old_cluster.controldata.chkpnt_nxtmulti + 1,
				  old_cluster.controldata.chkpnt_nxtmulti,
				  new_cluster.pgdata);
		check_ok();
	}

	/* now reset the wal archives in the new cluster */
	prep_status("Resetting WAL archives");
	exec_prog(UTILITY_LOG_FILE, NULL, true, true,
	/* use timeline 1 to match controldata and no WAL history file */
			  "\"%s/pg_resetwal\" -l 00000001%s \"%s\"", new_cluster.bindir,
			  old_cluster.controldata.nextxlogfile + 8,
			  new_cluster.pgdata);
	check_ok();
}


/*
 *	set_frozenxids()
 *
 * This is called on the new cluster before we restore anything, with
 * minmxid_only = false.  Its purpose is to ensure that all initdb-created
 * vacuumable tables have relfrozenxid/relminmxid matching the old cluster's
 * xid/mxid counters.  We also initialize the datfrozenxid/datminmxid of the
 * built-in databases to match.
 *
 * As we create user tables later, their relfrozenxid/relminmxid fields will
 * be restored properly by the binary-upgrade restore script.  Likewise for
 * user-database datfrozenxid/datminmxid.  However, if we're upgrading from a
 * pre-9.3 database, which does not store per-table or per-DB minmxid, then
 * the relminmxid/datminmxid values filled in by the restore script will just
 * be zeroes.
 *
 * Hence, with a pre-9.3 source database, a second call occurs after
 * everything is restored, with minmxid_only = true.  This pass will
 * initialize all tables and databases, both those made by initdb and user
 * objects, with the desired minmxid value.  frozenxid values are left alone.
 */
static void
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
		 * relfrozenxids, and relminmxid even if autovacuum is turned off, and
		 * even though all the data rows are already frozen.  To enable this,
		 * we temporarily change datallowconn.
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
									  "WHERE	relkind IN ("
									  CppAsString2(RELKIND_RELATION) ", "
									  CppAsString2(RELKIND_MATVIEW) ", "
									  CppAsString2(RELKIND_TOASTVALUE) ")",
									  old_cluster.controldata.chkpnt_nxtxid));

		/* set pg_class.relminmxid */
		PQclear(executeQueryOrDie(conn,
								  "UPDATE	pg_catalog.pg_class "
								  "SET	relminmxid = '%u' "
		/* only heap, materialized view, and TOAST are vacuumed */
								  "WHERE	relkind IN ("
								  CppAsString2(RELKIND_RELATION) ", "
								  CppAsString2(RELKIND_MATVIEW) ", "
								  CppAsString2(RELKIND_TOASTVALUE) ")",
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

/*
 * create_logical_replication_slots()
 *
 * Similar to create_new_objects() but only restores logical replication slots.
 */
static void
create_logical_replication_slots(void)
{
	prep_status_progress("Restoring logical replication slots in the new cluster");

	for (int dbnum = 0; dbnum < old_cluster.dbarr.ndbs; dbnum++)
	{
		DbInfo	   *old_db = &old_cluster.dbarr.dbs[dbnum];
		LogicalSlotInfoArr *slot_arr = &old_db->slot_arr;
		PGconn	   *conn;
		PQExpBuffer query;

		/* Skip this database if there are no slots */
		if (slot_arr->nslots == 0)
			continue;

		conn = connectToServer(&new_cluster, old_db->db_name);
		query = createPQExpBuffer();

		pg_log(PG_STATUS, "%s", old_db->db_name);

		for (int slotnum = 0; slotnum < slot_arr->nslots; slotnum++)
		{
			LogicalSlotInfo *slot_info = &slot_arr->slots[slotnum];

			/* Constructs a query for creating logical replication slots */
			appendPQExpBufferStr(query,
								 "SELECT * FROM "
								 "pg_catalog.pg_create_logical_replication_slot(");
			appendStringLiteralConn(query, slot_info->slotname, conn);
			appendPQExpBufferStr(query, ", ");
			appendStringLiteralConn(query, slot_info->plugin, conn);

			appendPQExpBuffer(query, ", false, %s, %s);",
							  slot_info->two_phase ? "true" : "false",
							  slot_info->failover ? "true" : "false");

			PQclear(executeQueryOrDie(conn, "%s", query->data));

			resetPQExpBuffer(query);
		}

		PQfinish(conn);

		destroyPQExpBuffer(query);
	}

	end_progress_output();
	check_ok();

	return;
}
