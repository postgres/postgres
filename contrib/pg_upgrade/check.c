/*
 *	check.c
 *
 *	server checks and output routines
 *
 *	Copyright (c) 2010-2013, PostgreSQL Global Development Group
 *	contrib/pg_upgrade/check.c
 */

#include "postgres_fe.h"

#include "pg_upgrade.h"


static void set_locale_and_encoding(ClusterInfo *cluster);
static void check_new_cluster_is_empty(void);
static void check_locale_and_encoding(ControlData *oldctrl,
						  ControlData *newctrl);
static void check_is_super_user(ClusterInfo *cluster);
static void check_for_prepared_transactions(ClusterInfo *cluster);
static void check_for_isn_and_int8_passing_mismatch(ClusterInfo *cluster);
static void check_for_reg_data_type_usage(ClusterInfo *cluster);
static void get_bin_version(ClusterInfo *cluster);
static char *get_canonical_locale_name(int category, const char *locale);


/*
 * fix_path_separator
 * For non-Windows, just return the argument.
 * For Windows convert any forward slash to a backslash
 * such as is suitable for arguments to builtin commands
 * like RMDIR and DEL.
 */
static char *
fix_path_separator(char *path)
{
#ifdef WIN32

	char	   *result;
	char	   *c;

	result = pg_strdup(path);

	for (c = result; *c != '\0'; c++)
		if (*c == '/')
			*c = '\\';

	return result;
#else

	return path;
#endif
}

void
output_check_banner(bool live_check)
{
	if (user_opts.check && live_check)
	{
		pg_log(PG_REPORT, "Performing Consistency Checks on Old Live Server\n");
		pg_log(PG_REPORT, "------------------------------------------------\n");
	}
	else
	{
		pg_log(PG_REPORT, "Performing Consistency Checks\n");
		pg_log(PG_REPORT, "-----------------------------\n");
	}
}


void
check_and_dump_old_cluster(bool live_check, char **sequence_script_file_name)
{
	/* -- OLD -- */

	if (!live_check)
		start_postmaster(&old_cluster, true);

	set_locale_and_encoding(&old_cluster);

	get_pg_database_relfilenode(&old_cluster);

	/* Extract a list of databases and tables from the old cluster */
	get_db_and_rel_infos(&old_cluster);

	init_tablespaces();

	get_loadable_libraries();


	/*
	 * Check for various failure cases
	 */
	check_is_super_user(&old_cluster);
	check_for_prepared_transactions(&old_cluster);
	check_for_reg_data_type_usage(&old_cluster);
	check_for_isn_and_int8_passing_mismatch(&old_cluster);

	/* old = PG 8.3 checks? */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 803)
	{
		old_8_3_check_for_name_data_type_usage(&old_cluster);
		old_8_3_check_for_tsquery_usage(&old_cluster);
		old_8_3_check_ltree_usage(&old_cluster);
		if (user_opts.check)
		{
			old_8_3_rebuild_tsvector_tables(&old_cluster, true);
			old_8_3_invalidate_hash_gin_indexes(&old_cluster, true);
			old_8_3_invalidate_bpchar_pattern_ops_indexes(&old_cluster, true);
		}
		else

			/*
			 * While we have the old server running, create the script to
			 * properly restore its sequence values but we report this at the
			 * end.
			 */
			*sequence_script_file_name =
				old_8_3_create_sequence_script(&old_cluster);
	}

	/* Pre-PG 9.0 had no large object permissions */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 804)
		new_9_0_populate_pg_largeobject_metadata(&old_cluster, true);

	/*
	 * While not a check option, we do this now because this is the only time
	 * the old server is running.
	 */
	if (!user_opts.check)
		generate_old_dump();

	if (!live_check)
		stop_postmaster(false);
}


void
check_new_cluster(void)
{
	set_locale_and_encoding(&new_cluster);

	check_locale_and_encoding(&old_cluster.controldata, &new_cluster.controldata);

	get_db_and_rel_infos(&new_cluster);

	check_new_cluster_is_empty();

	check_loadable_libraries();

	if (user_opts.transfer_mode == TRANSFER_MODE_LINK)
		check_hard_link();

	check_is_super_user(&new_cluster);

	/*
	 * We don't restore our own user, so both clusters must match have
	 * matching install-user oids.
	 */
	if (old_cluster.install_role_oid != new_cluster.install_role_oid)
		pg_log(PG_FATAL,
			   "Old and new cluster install users have different values for pg_authid.oid.\n");

	/*
	 * We only allow the install user in the new cluster because other defined
	 * users might match users defined in the old cluster and generate an
	 * error during pg_dump restore.
	 */
	if (new_cluster.role_count != 1)
		pg_log(PG_FATAL, "Only the install user can be defined in the new cluster.\n");

	check_for_prepared_transactions(&new_cluster);
}


void
report_clusters_compatible(void)
{
	if (user_opts.check)
	{
		pg_log(PG_REPORT, "\n*Clusters are compatible*\n");
		/* stops new cluster */
		stop_postmaster(false);
		exit(0);
	}

	pg_log(PG_REPORT, "\n"
		   "If pg_upgrade fails after this point, you must re-initdb the\n"
		   "new cluster before continuing.\n");
}


void
issue_warnings(char *sequence_script_file_name)
{
	/* old = PG 8.3 warnings? */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 803)
	{
		start_postmaster(&new_cluster, true);

		/* restore proper sequence values using file created from old server */
		if (sequence_script_file_name)
		{
			prep_status("Adjusting sequences");
			exec_prog(UTILITY_LOG_FILE, NULL, true,
					  "\"%s/psql\" " EXEC_PSQL_ARGS " %s -f \"%s\"",
					  new_cluster.bindir, cluster_conn_opts(&new_cluster),
					  sequence_script_file_name);
			unlink(sequence_script_file_name);
			check_ok();
		}

		old_8_3_rebuild_tsvector_tables(&new_cluster, false);
		old_8_3_invalidate_hash_gin_indexes(&new_cluster, false);
		old_8_3_invalidate_bpchar_pattern_ops_indexes(&new_cluster, false);
		stop_postmaster(false);
	}

	/* Create dummy large object permissions for old < PG 9.0? */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 804)
	{
		start_postmaster(&new_cluster, true);
		new_9_0_populate_pg_largeobject_metadata(&new_cluster, false);
		stop_postmaster(false);
	}
}


void
output_completion_banner(char *analyze_script_file_name,
						 char *deletion_script_file_name)
{
	/* Did we copy the free space files? */
	if (GET_MAJOR_VERSION(old_cluster.major_version) >= 804)
		pg_log(PG_REPORT,
			   "Optimizer statistics are not transferred by pg_upgrade so,\n"
			   "once you start the new server, consider running:\n"
			   "    %s\n\n", analyze_script_file_name);
	else
		pg_log(PG_REPORT,
			   "Optimizer statistics and free space information are not transferred\n"
		"by pg_upgrade so, once you start the new server, consider running:\n"
			   "    %s\n\n", analyze_script_file_name);


	if (deletion_script_file_name)
		pg_log(PG_REPORT,
			"Running this script will delete the old cluster's data files:\n"
			   "    %s\n",
			   deletion_script_file_name);
	else
		pg_log(PG_REPORT,
			   "Could not create a script to delete the old cluster's data\n"
		  "files because user-defined tablespaces exist in the old cluster\n"
		"directory.  The old cluster's contents must be deleted manually.\n");
}


void
check_cluster_versions(void)
{
	prep_status("Checking cluster versions");

	/* get old and new cluster versions */
	old_cluster.major_version = get_major_server_version(&old_cluster);
	new_cluster.major_version = get_major_server_version(&new_cluster);

	/*
	 * We allow upgrades from/to the same major version for alpha/beta
	 * upgrades
	 */

	if (GET_MAJOR_VERSION(old_cluster.major_version) < 803)
		pg_log(PG_FATAL, "This utility can only upgrade from PostgreSQL version 8.3 and later.\n");

	/* Only current PG version is supported as a target */
	if (GET_MAJOR_VERSION(new_cluster.major_version) != GET_MAJOR_VERSION(PG_VERSION_NUM))
		pg_log(PG_FATAL, "This utility can only upgrade to PostgreSQL version %s.\n",
			   PG_MAJORVERSION);

	/*
	 * We can't allow downgrading because we use the target pg_dumpall, and
	 * pg_dumpall cannot operate on new database versions, only older
	 * versions.
	 */
	if (old_cluster.major_version > new_cluster.major_version)
		pg_log(PG_FATAL, "This utility cannot be used to downgrade to older major PostgreSQL versions.\n");

	/* get old and new binary versions */
	get_bin_version(&old_cluster);
	get_bin_version(&new_cluster);

	/* Ensure binaries match the designated data directories */
	if (GET_MAJOR_VERSION(old_cluster.major_version) !=
		GET_MAJOR_VERSION(old_cluster.bin_version))
		pg_log(PG_FATAL,
			   "Old cluster data and binary directories are from different major versions.\n");
	if (GET_MAJOR_VERSION(new_cluster.major_version) !=
		GET_MAJOR_VERSION(new_cluster.bin_version))
		pg_log(PG_FATAL,
			   "New cluster data and binary directories are from different major versions.\n");

	check_ok();
}


void
check_cluster_compatibility(bool live_check)
{
	/* get/check pg_control data of servers */
	get_control_data(&old_cluster, live_check);
	get_control_data(&new_cluster, false);
	check_control_data(&old_cluster.controldata, &new_cluster.controldata);

	/* Is it 9.0 but without tablespace directories? */
	if (GET_MAJOR_VERSION(new_cluster.major_version) == 900 &&
		new_cluster.controldata.cat_ver < TABLE_SPACE_SUBDIRS_CAT_VER)
		pg_log(PG_FATAL, "This utility can only upgrade to PostgreSQL version 9.0 after 2010-01-11\n"
			   "because of backend API changes made during development.\n");

	/* We read the real port number for PG >= 9.1 */
	if (live_check && GET_MAJOR_VERSION(old_cluster.major_version) < 901 &&
		old_cluster.port == DEF_PGUPORT)
		pg_log(PG_FATAL, "When checking a pre-PG 9.1 live old server, "
			   "you must specify the old server's port number.\n");

	if (live_check && old_cluster.port == new_cluster.port)
		pg_log(PG_FATAL, "When checking a live server, "
			   "the old and new port numbers must be different.\n");
}


/*
 * set_locale_and_encoding()
 *
 * query the database to get the template0 locale
 */
static void
set_locale_and_encoding(ClusterInfo *cluster)
{
	ControlData *ctrl = &cluster->controldata;
	PGconn	   *conn;
	PGresult   *res;
	int			i_encoding;
	int			cluster_version = cluster->major_version;

	conn = connectToServer(cluster, "template1");

	/* for pg < 80400, we got the values from pg_controldata */
	if (cluster_version >= 80400)
	{
		int			i_datcollate;
		int			i_datctype;

		res = executeQueryOrDie(conn,
								"SELECT datcollate, datctype "
								"FROM 	pg_catalog.pg_database "
								"WHERE	datname = 'template0' ");
		assert(PQntuples(res) == 1);

		i_datcollate = PQfnumber(res, "datcollate");
		i_datctype = PQfnumber(res, "datctype");

		if (GET_MAJOR_VERSION(cluster->major_version) < 902)
		{
			/*
			 * Pre-9.2 did not canonicalize the supplied locale names to match
			 * what the system returns, while 9.2+ does, so convert pre-9.2 to
			 * match.
			 */
			ctrl->lc_collate = get_canonical_locale_name(LC_COLLATE,
								pg_strdup(PQgetvalue(res, 0, i_datcollate)));
			ctrl->lc_ctype = get_canonical_locale_name(LC_CTYPE,
								  pg_strdup(PQgetvalue(res, 0, i_datctype)));
		}
		else
		{
			ctrl->lc_collate = pg_strdup(PQgetvalue(res, 0, i_datcollate));
			ctrl->lc_ctype = pg_strdup(PQgetvalue(res, 0, i_datctype));
		}

		PQclear(res);
	}

	res = executeQueryOrDie(conn,
							"SELECT pg_catalog.pg_encoding_to_char(encoding) "
							"FROM 	pg_catalog.pg_database "
							"WHERE	datname = 'template0' ");
	assert(PQntuples(res) == 1);

	i_encoding = PQfnumber(res, "pg_encoding_to_char");
	ctrl->encoding = pg_strdup(PQgetvalue(res, 0, i_encoding));

	PQclear(res);

	PQfinish(conn);
}


/*
 * check_locale_and_encoding()
 *
 *	locale is not in pg_controldata in 8.4 and later so
 *	we probably had to get via a database query.
 */
static void
check_locale_and_encoding(ControlData *oldctrl,
						  ControlData *newctrl)
{
	/*
	 * These are often defined with inconsistent case, so use pg_strcasecmp().
	 * They also often use inconsistent hyphenation, which we cannot fix, e.g.
	 * UTF-8 vs. UTF8, so at least we display the mismatching values.
	 */
	if (pg_strcasecmp(oldctrl->lc_collate, newctrl->lc_collate) != 0)
		pg_log(PG_FATAL,
		 "lc_collate cluster values do not match:  old \"%s\", new \"%s\"\n",
			   oldctrl->lc_collate, newctrl->lc_collate);
	if (pg_strcasecmp(oldctrl->lc_ctype, newctrl->lc_ctype) != 0)
		pg_log(PG_FATAL,
		   "lc_ctype cluster values do not match:  old \"%s\", new \"%s\"\n",
			   oldctrl->lc_ctype, newctrl->lc_ctype);
	if (pg_strcasecmp(oldctrl->encoding, newctrl->encoding) != 0)
		pg_log(PG_FATAL,
		   "encoding cluster values do not match:  old \"%s\", new \"%s\"\n",
			   oldctrl->encoding, newctrl->encoding);
}


static void
check_new_cluster_is_empty(void)
{
	int			dbnum;

	for (dbnum = 0; dbnum < new_cluster.dbarr.ndbs; dbnum++)
	{
		int			relnum;
		RelInfoArr *rel_arr = &new_cluster.dbarr.dbs[dbnum].rel_arr;

		for (relnum = 0; relnum < rel_arr->nrels;
			 relnum++)
		{
			/* pg_largeobject and its index should be skipped */
			if (strcmp(rel_arr->rels[relnum].nspname, "pg_catalog") != 0)
				pg_log(PG_FATAL, "New cluster database \"%s\" is not empty\n",
					   new_cluster.dbarr.dbs[dbnum].db_name);
		}
	}

}


/*
 * create_script_for_cluster_analyze()
 *
 *	This incrementally generates better optimizer statistics
 */
void
create_script_for_cluster_analyze(char **analyze_script_file_name)
{
	FILE	   *script = NULL;

	*analyze_script_file_name = pg_malloc(MAXPGPATH);

	prep_status("Creating script to analyze new cluster");

	snprintf(*analyze_script_file_name, MAXPGPATH, "analyze_new_cluster.%s",
			 SCRIPT_EXT);

	if ((script = fopen_priv(*analyze_script_file_name, "w")) == NULL)
		pg_log(PG_FATAL, "Could not open file \"%s\": %s\n",
			   *analyze_script_file_name, getErrorText(errno));

#ifndef WIN32
	/* add shebang header */
	fprintf(script, "#!/bin/sh\n\n");
#else
	/* suppress command echoing */
	fprintf(script, "@echo off\n");
#endif

	fprintf(script, "echo %sThis script will generate minimal optimizer statistics rapidly%s\n",
			ECHO_QUOTE, ECHO_QUOTE);
	fprintf(script, "echo %sso your system is usable, and then gather statistics twice more%s\n",
			ECHO_QUOTE, ECHO_QUOTE);
	fprintf(script, "echo %swith increasing accuracy.  When it is done, your system will%s\n",
			ECHO_QUOTE, ECHO_QUOTE);
	fprintf(script, "echo %shave the default level of optimizer statistics.%s\n",
			ECHO_QUOTE, ECHO_QUOTE);
	fprintf(script, "echo%s\n\n", ECHO_BLANK);

	fprintf(script, "echo %sIf you have used ALTER TABLE to modify the statistics target for%s\n",
			ECHO_QUOTE, ECHO_QUOTE);
	fprintf(script, "echo %sany tables, you might want to remove them and restore them after%s\n",
			ECHO_QUOTE, ECHO_QUOTE);
	fprintf(script, "echo %srunning this script because they will delay fast statistics generation.%s\n",
			ECHO_QUOTE, ECHO_QUOTE);
	fprintf(script, "echo%s\n\n", ECHO_BLANK);

	fprintf(script, "echo %sIf you would like default statistics as quickly as possible, cancel%s\n",
			ECHO_QUOTE, ECHO_QUOTE);
	fprintf(script, "echo %sthis script and run:%s\n",
			ECHO_QUOTE, ECHO_QUOTE);
	fprintf(script, "echo %s    \"%s/vacuumdb\" --all %s%s\n", ECHO_QUOTE, new_cluster.bindir,
	/* Did we copy the free space files? */
			(GET_MAJOR_VERSION(old_cluster.major_version) >= 804) ?
			"--analyze-only" : "--analyze", ECHO_QUOTE);
	fprintf(script, "echo%s\n\n", ECHO_BLANK);

#ifndef WIN32
	fprintf(script, "sleep 2\n");
	fprintf(script, "PGOPTIONS='-c default_statistics_target=1 -c vacuum_cost_delay=0'\n");
	/* only need to export once */
	fprintf(script, "export PGOPTIONS\n");
#else
	fprintf(script, "REM simulate sleep 2\n");
	fprintf(script, "PING 1.1.1.1 -n 1 -w 2000 > nul\n");
	fprintf(script, "SET PGOPTIONS=-c default_statistics_target=1 -c vacuum_cost_delay=0\n");
#endif

	fprintf(script, "echo %sGenerating minimal optimizer statistics (1 target)%s\n",
			ECHO_QUOTE, ECHO_QUOTE);
	fprintf(script, "echo %s--------------------------------------------------%s\n",
			ECHO_QUOTE, ECHO_QUOTE);
	fprintf(script, "\"%s/vacuumdb\" --all --analyze-only\n", new_cluster.bindir);
	fprintf(script, "echo%s\n", ECHO_BLANK);
	fprintf(script, "echo %sThe server is now available with minimal optimizer statistics.%s\n",
			ECHO_QUOTE, ECHO_QUOTE);
	fprintf(script, "echo %sQuery performance will be optimal once this script completes.%s\n",
			ECHO_QUOTE, ECHO_QUOTE);
	fprintf(script, "echo%s\n\n", ECHO_BLANK);

#ifndef WIN32
	fprintf(script, "sleep 2\n");
	fprintf(script, "PGOPTIONS='-c default_statistics_target=10'\n");
#else
	fprintf(script, "REM simulate sleep\n");
	fprintf(script, "PING 1.1.1.1 -n 1 -w 2000 > nul\n");
	fprintf(script, "SET PGOPTIONS=-c default_statistics_target=10\n");
#endif

	fprintf(script, "echo %sGenerating medium optimizer statistics (10 targets)%s\n",
			ECHO_QUOTE, ECHO_QUOTE);
	fprintf(script, "echo %s---------------------------------------------------%s\n",
			ECHO_QUOTE, ECHO_QUOTE);
	fprintf(script, "\"%s/vacuumdb\" --all --analyze-only\n", new_cluster.bindir);
	fprintf(script, "echo%s\n\n", ECHO_BLANK);

#ifndef WIN32
	fprintf(script, "unset PGOPTIONS\n");
#else
	fprintf(script, "SET PGOPTIONS\n");
#endif

	fprintf(script, "echo %sGenerating default (full) optimizer statistics (100 targets?)%s\n",
			ECHO_QUOTE, ECHO_QUOTE);
	fprintf(script, "echo %s-------------------------------------------------------------%s\n",
			ECHO_QUOTE, ECHO_QUOTE);
	fprintf(script, "\"%s/vacuumdb\" --all %s\n", new_cluster.bindir,
	/* Did we copy the free space files? */
			(GET_MAJOR_VERSION(old_cluster.major_version) >= 804) ?
			"--analyze-only" : "--analyze");

	fprintf(script, "echo%s\n\n", ECHO_BLANK);
	fprintf(script, "echo %sDone%s\n",
			ECHO_QUOTE, ECHO_QUOTE);

	fclose(script);

#ifndef WIN32
	if (chmod(*analyze_script_file_name, S_IRWXU) != 0)
		pg_log(PG_FATAL, "Could not add execute permission to file \"%s\": %s\n",
			   *analyze_script_file_name, getErrorText(errno));
#endif

	check_ok();
}


/*
 * create_script_for_old_cluster_deletion()
 *
 *	This is particularly useful for tablespace deletion.
 */
void
create_script_for_old_cluster_deletion(char **deletion_script_file_name)
{
	FILE	   *script = NULL;
	int			tblnum;
	char		old_cluster_pgdata[MAXPGPATH];

	*deletion_script_file_name = pg_malloc(MAXPGPATH);

	snprintf(*deletion_script_file_name, MAXPGPATH, "delete_old_cluster.%s",
			 SCRIPT_EXT);

	/*
	 * Some users (oddly) create tablespaces inside the cluster data
	 * directory.  We can't create a proper old cluster delete script in that
	 * case.
	 */
	strlcpy(old_cluster_pgdata, old_cluster.pgdata, MAXPGPATH);
	canonicalize_path(old_cluster_pgdata);
	for (tblnum = 0; tblnum < os_info.num_old_tablespaces; tblnum++)
	{
		char		old_tablespace_dir[MAXPGPATH];

		strlcpy(old_tablespace_dir, os_info.old_tablespaces[tblnum], MAXPGPATH);
		canonicalize_path(old_tablespace_dir);
		if (path_is_prefix_of_path(old_cluster_pgdata, old_tablespace_dir))
		{
			/* Unlink file in case it is left over from a previous run. */
			unlink(*deletion_script_file_name);
			pg_free(*deletion_script_file_name);
			*deletion_script_file_name = NULL;
			return;
		}
	}

	prep_status("Creating script to delete old cluster");

	if ((script = fopen_priv(*deletion_script_file_name, "w")) == NULL)
		pg_log(PG_FATAL, "Could not open file \"%s\": %s\n",
			   *deletion_script_file_name, getErrorText(errno));

#ifndef WIN32
	/* add shebang header */
	fprintf(script, "#!/bin/sh\n\n");
#endif

	/* delete old cluster's default tablespace */
	fprintf(script, RMDIR_CMD " %s\n", fix_path_separator(old_cluster.pgdata));

	/* delete old cluster's alternate tablespaces */
	for (tblnum = 0; tblnum < os_info.num_old_tablespaces; tblnum++)
	{
		/*
		 * Do the old cluster's per-database directories share a directory
		 * with a new version-specific tablespace?
		 */
		if (strlen(old_cluster.tablespace_suffix) == 0)
		{
			/* delete per-database directories */
			int			dbnum;

			fprintf(script, "\n");
			/* remove PG_VERSION? */
			if (GET_MAJOR_VERSION(old_cluster.major_version) <= 804)
				fprintf(script, RM_CMD " %s%s%cPG_VERSION\n",
						fix_path_separator(os_info.old_tablespaces[tblnum]),
						fix_path_separator(old_cluster.tablespace_suffix),
						PATH_SEPARATOR);

			for (dbnum = 0; dbnum < old_cluster.dbarr.ndbs; dbnum++)
			{
				fprintf(script, RMDIR_CMD " %s%s%c%d\n",
						fix_path_separator(os_info.old_tablespaces[tblnum]),
						fix_path_separator(old_cluster.tablespace_suffix),
						PATH_SEPARATOR, old_cluster.dbarr.dbs[dbnum].db_oid);
			}
		}
		else

			/*
			 * Simply delete the tablespace directory, which might be ".old"
			 * or a version-specific subdirectory.
			 */
			fprintf(script, RMDIR_CMD " %s%s\n",
					fix_path_separator(os_info.old_tablespaces[tblnum]),
					fix_path_separator(old_cluster.tablespace_suffix));
	}

	fclose(script);

#ifndef WIN32
	if (chmod(*deletion_script_file_name, S_IRWXU) != 0)
		pg_log(PG_FATAL, "Could not add execute permission to file \"%s\": %s\n",
			   *deletion_script_file_name, getErrorText(errno));
#endif

	check_ok();
}


/*
 *	check_is_super_user()
 *
 *	Check we are superuser, and out user id and user count
 */
static void
check_is_super_user(ClusterInfo *cluster)
{
	PGresult   *res;
	PGconn	   *conn = connectToServer(cluster, "template1");

	prep_status("Checking database user is a superuser");

	/* Can't use pg_authid because only superusers can view it. */
	res = executeQueryOrDie(conn,
							"SELECT rolsuper, oid "
							"FROM pg_catalog.pg_roles "
							"WHERE rolname = current_user");

	if (PQntuples(res) != 1 || strcmp(PQgetvalue(res, 0, 0), "t") != 0)
		pg_log(PG_FATAL, "database user \"%s\" is not a superuser\n",
			   os_info.user);

	cluster->install_role_oid = atooid(PQgetvalue(res, 0, 1));

	PQclear(res);

	res = executeQueryOrDie(conn,
							"SELECT COUNT(*) "
							"FROM pg_catalog.pg_roles ");

	if (PQntuples(res) != 1)
		pg_log(PG_FATAL, "could not determine the number of users\n");

	cluster->role_count = atoi(PQgetvalue(res, 0, 0));

	PQclear(res);

	PQfinish(conn);

	check_ok();
}


/*
 *	check_for_prepared_transactions()
 *
 *	Make sure there are no prepared transactions because the storage format
 *	might have changed.
 */
static void
check_for_prepared_transactions(ClusterInfo *cluster)
{
	PGresult   *res;
	PGconn	   *conn = connectToServer(cluster, "template1");

	prep_status("Checking for prepared transactions");

	res = executeQueryOrDie(conn,
							"SELECT * "
							"FROM pg_catalog.pg_prepared_xacts");

	if (PQntuples(res) != 0)
		pg_log(PG_FATAL, "The %s cluster contains prepared transactions\n",
			   CLUSTER_NAME(cluster));

	PQclear(res);

	PQfinish(conn);

	check_ok();
}


/*
 *	check_for_isn_and_int8_passing_mismatch()
 *
 *	contrib/isn relies on data type int8, and in 8.4 int8 can now be passed
 *	by value.  The schema dumps the CREATE TYPE PASSEDBYVALUE setting so
 *	it must match for the old and new servers.
 */
static void
check_for_isn_and_int8_passing_mismatch(ClusterInfo *cluster)
{
	int			dbnum;
	FILE	   *script = NULL;
	bool		found = false;
	char		output_path[MAXPGPATH];

	prep_status("Checking for contrib/isn with bigint-passing mismatch");

	if (old_cluster.controldata.float8_pass_by_value ==
		new_cluster.controldata.float8_pass_by_value)
	{
		/* no mismatch */
		check_ok();
		return;
	}

	snprintf(output_path, sizeof(output_path),
			 "contrib_isn_and_int8_pass_by_value.txt");

	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_nspname,
					i_proname;
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);

		/* Find any functions coming from contrib/isn */
		res = executeQueryOrDie(conn,
								"SELECT n.nspname, p.proname "
								"FROM	pg_catalog.pg_proc p, "
								"		pg_catalog.pg_namespace n "
								"WHERE	p.pronamespace = n.oid AND "
								"		p.probin = '$libdir/isn'");

		ntups = PQntuples(res);
		i_nspname = PQfnumber(res, "nspname");
		i_proname = PQfnumber(res, "proname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			found = true;
			if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
				pg_log(PG_FATAL, "Could not open file \"%s\": %s\n",
					   output_path, getErrorText(errno));
			if (!db_used)
			{
				fprintf(script, "Database: %s\n", active_db->db_name);
				db_used = true;
			}
			fprintf(script, "  %s.%s\n",
					PQgetvalue(res, rowno, i_nspname),
					PQgetvalue(res, rowno, i_proname));
		}

		PQclear(res);

		PQfinish(conn);
	}

	if (script)
		fclose(script);

	if (found)
	{
		pg_log(PG_REPORT, "fatal\n");
		pg_log(PG_FATAL,
			   "Your installation contains \"contrib/isn\" functions which rely on the\n"
		  "bigint data type.  Your old and new clusters pass bigint values\n"
		"differently so this cluster cannot currently be upgraded.  You can\n"
			   "manually upgrade databases that use \"contrib/isn\" facilities and remove\n"
			   "\"contrib/isn\" from the old cluster and restart the upgrade.  A list of\n"
			   "the problem functions is in the file:\n"
			   "    %s\n\n", output_path);
	}
	else
		check_ok();
}


/*
 * check_for_reg_data_type_usage()
 *	pg_upgrade only preserves these system values:
 *		pg_class.oid
 *		pg_type.oid
 *		pg_enum.oid
 *
 *	Many of the reg* data types reference system catalog info that is
 *	not preserved, and hence these data types cannot be used in user
 *	tables upgraded by pg_upgrade.
 */
static void
check_for_reg_data_type_usage(ClusterInfo *cluster)
{
	int			dbnum;
	FILE	   *script = NULL;
	bool		found = false;
	char		output_path[MAXPGPATH];

	prep_status("Checking for reg* system OID user data types");

	snprintf(output_path, sizeof(output_path), "tables_using_reg.txt");

	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_nspname,
					i_relname,
					i_attname;
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);

		/*
		 * While several relkinds don't store any data, e.g. views, they can
		 * be used to define data types of other columns, so we check all
		 * relkinds.
		 */
		res = executeQueryOrDie(conn,
								"SELECT n.nspname, c.relname, a.attname "
								"FROM	pg_catalog.pg_class c, "
								"		pg_catalog.pg_namespace n, "
								"		pg_catalog.pg_attribute a "
								"WHERE	c.oid = a.attrelid AND "
								"		NOT a.attisdropped AND "
								"		a.atttypid IN ( "
		  "			'pg_catalog.regproc'::pg_catalog.regtype, "
								"			'pg_catalog.regprocedure'::pg_catalog.regtype, "
		  "			'pg_catalog.regoper'::pg_catalog.regtype, "
								"			'pg_catalog.regoperator'::pg_catalog.regtype, "
		/* regclass.oid is preserved, so 'regclass' is OK */
		/* regtype.oid is preserved, so 'regtype' is OK */
		"			'pg_catalog.regconfig'::pg_catalog.regtype, "
								"			'pg_catalog.regdictionary'::pg_catalog.regtype) AND "
								"		c.relnamespace = n.oid AND "
							  "		n.nspname != 'pg_catalog' AND "
						 "		n.nspname != 'information_schema'");

		ntups = PQntuples(res);
		i_nspname = PQfnumber(res, "nspname");
		i_relname = PQfnumber(res, "relname");
		i_attname = PQfnumber(res, "attname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			found = true;
			if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
				pg_log(PG_FATAL, "Could not open file \"%s\": %s\n",
					   output_path, getErrorText(errno));
			if (!db_used)
			{
				fprintf(script, "Database: %s\n", active_db->db_name);
				db_used = true;
			}
			fprintf(script, "  %s.%s.%s\n",
					PQgetvalue(res, rowno, i_nspname),
					PQgetvalue(res, rowno, i_relname),
					PQgetvalue(res, rowno, i_attname));
		}

		PQclear(res);

		PQfinish(conn);
	}

	if (script)
		fclose(script);

	if (found)
	{
		pg_log(PG_REPORT, "fatal\n");
		pg_log(PG_FATAL,
			   "Your installation contains one of the reg* data types in user tables.\n"
		 "These data types reference system OIDs that are not preserved by\n"
		"pg_upgrade, so this cluster cannot currently be upgraded.  You can\n"
			   "remove the problem tables and restart the upgrade.  A list of the problem\n"
			   "columns is in the file:\n"
			   "    %s\n\n", output_path);
	}
	else
		check_ok();
}


static void
get_bin_version(ClusterInfo *cluster)
{
	char		cmd[MAXPGPATH],
				cmd_output[MAX_STRING];
	FILE	   *output;
	int			pre_dot,
				post_dot;

	snprintf(cmd, sizeof(cmd), "\"%s/pg_ctl\" --version", cluster->bindir);

	if ((output = popen(cmd, "r")) == NULL ||
		fgets(cmd_output, sizeof(cmd_output), output) == NULL)
		pg_log(PG_FATAL, "Could not get pg_ctl version data using %s: %s\n",
			   cmd, getErrorText(errno));

	pclose(output);

	/* Remove trailing newline */
	if (strchr(cmd_output, '\n') != NULL)
		*strchr(cmd_output, '\n') = '\0';

	if (sscanf(cmd_output, "%*s %*s %d.%d", &pre_dot, &post_dot) != 2)
		pg_log(PG_FATAL, "could not get version from %s\n", cmd);

	cluster->bin_version = (pre_dot * 100 + post_dot) * 100;
}


/*
 * get_canonical_locale_name
 *
 * Send the locale name to the system, and hope we get back a canonical
 * version.  This should match the backend's check_locale() function.
 */
static char *
get_canonical_locale_name(int category, const char *locale)
{
	char	   *save;
	char	   *res;

	save = setlocale(category, NULL);
	if (!save)
		pg_log(PG_FATAL, "failed to get the current locale\n");

	/* 'save' may be pointing at a modifiable scratch variable, so copy it. */
	save = pg_strdup(save);

	/* set the locale with setlocale, to see if it accepts it. */
	res = setlocale(category, locale);

	if (!res)
		pg_log(PG_FATAL, "failed to get system local name for \"%s\"\n", res);

	res = pg_strdup(res);

	/* restore old value. */
	if (!setlocale(category, save))
		pg_log(PG_FATAL, "failed to restore old locale \"%s\"\n", save);

	pg_free(save);

	return res;
}
