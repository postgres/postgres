/*
 *	check.c
 *
 *	server checks and output routines
 *
 *	Copyright (c) 2010-2020, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/check.c
 */

#include "postgres_fe.h"

#include "catalog/pg_authid_d.h"
#include "fe_utils/string_utils.h"
#include "mb/pg_wchar.h"
#include "pg_upgrade.h"

static void check_new_cluster_is_empty(void);
static void check_databases_are_compatible(void);
static void check_locale_and_encoding(DbInfo *olddb, DbInfo *newdb);
static bool equivalent_locale(int category, const char *loca, const char *locb);
static void check_is_install_user(ClusterInfo *cluster);
static void check_proper_datallowconn(ClusterInfo *cluster);
static void check_for_prepared_transactions(ClusterInfo *cluster);
static void check_for_isn_and_int8_passing_mismatch(ClusterInfo *cluster);
static void check_for_tables_with_oids(ClusterInfo *cluster);
static void check_for_composite_data_type_usage(ClusterInfo *cluster);
static void check_for_reg_data_type_usage(ClusterInfo *cluster);
static void check_for_jsonb_9_4_usage(ClusterInfo *cluster);
static void check_for_pg_role_prefix(ClusterInfo *cluster);
static void check_for_new_tablespace_dir(ClusterInfo *new_cluster);
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
		pg_log(PG_REPORT,
			   "Performing Consistency Checks on Old Live Server\n"
			   "------------------------------------------------\n");
	}
	else
	{
		pg_log(PG_REPORT,
			   "Performing Consistency Checks\n"
			   "-----------------------------\n");
	}
}


void
check_and_dump_old_cluster(bool live_check)
{
	/* -- OLD -- */

	if (!live_check)
		start_postmaster(&old_cluster, true);

	/* Extract a list of databases and tables from the old cluster */
	get_db_and_rel_infos(&old_cluster);

	init_tablespaces();

	get_loadable_libraries();


	/*
	 * Check for various failure cases
	 */
	check_is_install_user(&old_cluster);
	check_proper_datallowconn(&old_cluster);
	check_for_prepared_transactions(&old_cluster);
	check_for_composite_data_type_usage(&old_cluster);
	check_for_reg_data_type_usage(&old_cluster);
	check_for_isn_and_int8_passing_mismatch(&old_cluster);

	/*
	 * Pre-PG 12 allowed tables to be declared WITH OIDS, which is not
	 * supported anymore. Verify there are none, iff applicable.
	 */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 1100)
		check_for_tables_with_oids(&old_cluster);

	/*
	 * PG 12 changed the 'sql_identifier' type storage to be based on name,
	 * not varchar, which breaks on-disk format for existing data. So we need
	 * to prevent upgrade when used in user objects (tables, indexes, ...).
	 */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 1100)
		old_11_check_for_sql_identifier_data_type_usage(&old_cluster);

	/*
	 * Pre-PG 10 allowed tables with 'unknown' type columns and non WAL logged
	 * hash indexes
	 */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 906)
	{
		old_9_6_check_for_unknown_data_type_usage(&old_cluster);
		if (user_opts.check)
			old_9_6_invalidate_hash_indexes(&old_cluster, true);
	}

	/* 9.5 and below should not have roles starting with pg_ */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 905)
		check_for_pg_role_prefix(&old_cluster);

	if (GET_MAJOR_VERSION(old_cluster.major_version) == 904 &&
		old_cluster.controldata.cat_ver < JSONB_FORMAT_CHANGE_CAT_VER)
		check_for_jsonb_9_4_usage(&old_cluster);

	/* Pre-PG 9.4 had a different 'line' data type internal format */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 903)
		old_9_3_check_for_line_data_type_usage(&old_cluster);

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
	get_db_and_rel_infos(&new_cluster);

	check_new_cluster_is_empty();
	check_databases_are_compatible();

	check_loadable_libraries();

	switch (user_opts.transfer_mode)
	{
		case TRANSFER_MODE_CLONE:
			check_file_clone();
			break;
		case TRANSFER_MODE_COPY:
			break;
		case TRANSFER_MODE_LINK:
			check_hard_link();
			break;
	}

	check_is_install_user(&new_cluster);

	check_for_prepared_transactions(&new_cluster);

	check_for_new_tablespace_dir(&new_cluster);
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
issue_warnings_and_set_wal_level(void)
{
	/*
	 * We unconditionally start/stop the new server because pg_resetwal -o set
	 * wal_level to 'minimum'.  If the user is upgrading standby servers using
	 * the rsync instructions, they will need pg_upgrade to write its final
	 * WAL record showing wal_level as 'replica'.
	 */
	start_postmaster(&new_cluster, true);

	/* Create dummy large object permissions for old < PG 9.0? */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 804)
		new_9_0_populate_pg_largeobject_metadata(&new_cluster, false);

	/* Reindex hash indexes for old < 10.0 */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 906)
		old_9_6_invalidate_hash_indexes(&new_cluster, false);

	report_extension_updates(&new_cluster);

	stop_postmaster(false);
}


void
output_completion_banner(char *analyze_script_file_name,
						 char *deletion_script_file_name)
{
	pg_log(PG_REPORT,
		   "Optimizer statistics are not transferred by pg_upgrade so,\n"
		   "once you start the new server, consider running:\n"
		   "    %s\n\n", analyze_script_file_name);

	if (deletion_script_file_name)
		pg_log(PG_REPORT,
			   "Running this script will delete the old cluster's data files:\n"
			   "    %s\n",
			   deletion_script_file_name);
	else
		pg_log(PG_REPORT,
			   "Could not create a script to delete the old cluster's data files\n"
			   "because user-defined tablespaces or the new cluster's data directory\n"
			   "exist in the old cluster directory.  The old cluster's contents must\n"
			   "be deleted manually.\n");
}


void
check_cluster_versions(void)
{
	prep_status("Checking cluster versions");

	/* cluster versions should already have been obtained */
	Assert(old_cluster.major_version != 0);
	Assert(new_cluster.major_version != 0);

	/*
	 * We allow upgrades from/to the same major version for alpha/beta
	 * upgrades
	 */

	if (GET_MAJOR_VERSION(old_cluster.major_version) < 804)
		pg_fatal("This utility can only upgrade from PostgreSQL version 8.4 and later.\n");

	/* Only current PG version is supported as a target */
	if (GET_MAJOR_VERSION(new_cluster.major_version) != GET_MAJOR_VERSION(PG_VERSION_NUM))
		pg_fatal("This utility can only upgrade to PostgreSQL version %s.\n",
				 PG_MAJORVERSION);

	/*
	 * We can't allow downgrading because we use the target pg_dump, and
	 * pg_dump cannot operate on newer database versions, only current and
	 * older versions.
	 */
	if (old_cluster.major_version > new_cluster.major_version)
		pg_fatal("This utility cannot be used to downgrade to older major PostgreSQL versions.\n");

	/* Ensure binaries match the designated data directories */
	if (GET_MAJOR_VERSION(old_cluster.major_version) !=
		GET_MAJOR_VERSION(old_cluster.bin_version))
		pg_fatal("Old cluster data and binary directories are from different major versions.\n");
	if (GET_MAJOR_VERSION(new_cluster.major_version) !=
		GET_MAJOR_VERSION(new_cluster.bin_version))
		pg_fatal("New cluster data and binary directories are from different major versions.\n");

	check_ok();
}


void
check_cluster_compatibility(bool live_check)
{
	/* get/check pg_control data of servers */
	get_control_data(&old_cluster, live_check);
	get_control_data(&new_cluster, false);
	check_control_data(&old_cluster.controldata, &new_cluster.controldata);

	/* We read the real port number for PG >= 9.1 */
	if (live_check && GET_MAJOR_VERSION(old_cluster.major_version) <= 900 &&
		old_cluster.port == DEF_PGUPORT)
		pg_fatal("When checking a pre-PG 9.1 live old server, "
				 "you must specify the old server's port number.\n");

	if (live_check && old_cluster.port == new_cluster.port)
		pg_fatal("When checking a live server, "
				 "the old and new port numbers must be different.\n");
}


/*
 * check_locale_and_encoding()
 *
 * Check that locale and encoding of a database in the old and new clusters
 * are compatible.
 */
static void
check_locale_and_encoding(DbInfo *olddb, DbInfo *newdb)
{
	if (olddb->db_encoding != newdb->db_encoding)
		pg_fatal("encodings for database \"%s\" do not match:  old \"%s\", new \"%s\"\n",
				 olddb->db_name,
				 pg_encoding_to_char(olddb->db_encoding),
				 pg_encoding_to_char(newdb->db_encoding));
	if (!equivalent_locale(LC_COLLATE, olddb->db_collate, newdb->db_collate))
		pg_fatal("lc_collate values for database \"%s\" do not match:  old \"%s\", new \"%s\"\n",
				 olddb->db_name, olddb->db_collate, newdb->db_collate);
	if (!equivalent_locale(LC_CTYPE, olddb->db_ctype, newdb->db_ctype))
		pg_fatal("lc_ctype values for database \"%s\" do not match:  old \"%s\", new \"%s\"\n",
				 olddb->db_name, olddb->db_ctype, newdb->db_ctype);
}

/*
 * equivalent_locale()
 *
 * Best effort locale-name comparison.  Return false if we are not 100% sure
 * the locales are equivalent.
 *
 * Note: The encoding parts of the names are ignored. This function is
 * currently used to compare locale names stored in pg_database, and
 * pg_database contains a separate encoding field. That's compared directly
 * in check_locale_and_encoding().
 */
static bool
equivalent_locale(int category, const char *loca, const char *locb)
{
	const char *chara;
	const char *charb;
	char	   *canona;
	char	   *canonb;
	int			lena;
	int			lenb;

	/*
	 * If the names are equal, the locales are equivalent. Checking this first
	 * avoids calling setlocale() in the common case that the names are equal.
	 * That's a good thing, if setlocale() is buggy, for example.
	 */
	if (pg_strcasecmp(loca, locb) == 0)
		return true;

	/*
	 * Not identical. Canonicalize both names, remove the encoding parts, and
	 * try again.
	 */
	canona = get_canonical_locale_name(category, loca);
	chara = strrchr(canona, '.');
	lena = chara ? (chara - canona) : strlen(canona);

	canonb = get_canonical_locale_name(category, locb);
	charb = strrchr(canonb, '.');
	lenb = charb ? (charb - canonb) : strlen(canonb);

	if (lena == lenb && pg_strncasecmp(canona, canonb, lena) == 0)
	{
		pg_free(canona);
		pg_free(canonb);
		return true;
	}

	pg_free(canona);
	pg_free(canonb);
	return false;
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
				pg_fatal("New cluster database \"%s\" is not empty: found relation \"%s.%s\"\n",
						 new_cluster.dbarr.dbs[dbnum].db_name,
						 rel_arr->rels[relnum].nspname,
						 rel_arr->rels[relnum].relname);
		}
	}
}

/*
 * Check that every database that already exists in the new cluster is
 * compatible with the corresponding database in the old one.
 */
static void
check_databases_are_compatible(void)
{
	int			newdbnum;
	int			olddbnum;
	DbInfo	   *newdbinfo;
	DbInfo	   *olddbinfo;

	for (newdbnum = 0; newdbnum < new_cluster.dbarr.ndbs; newdbnum++)
	{
		newdbinfo = &new_cluster.dbarr.dbs[newdbnum];

		/* Find the corresponding database in the old cluster */
		for (olddbnum = 0; olddbnum < old_cluster.dbarr.ndbs; olddbnum++)
		{
			olddbinfo = &old_cluster.dbarr.dbs[olddbnum];
			if (strcmp(newdbinfo->db_name, olddbinfo->db_name) == 0)
			{
				check_locale_and_encoding(olddbinfo, newdbinfo);
				break;
			}
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
	PQExpBufferData user_specification;

	prep_status("Creating script to analyze new cluster");

	initPQExpBuffer(&user_specification);
	if (os_info.user_specified)
	{
		appendPQExpBufferStr(&user_specification, "-U ");
		appendShellString(&user_specification, os_info.user);
		appendPQExpBufferChar(&user_specification, ' ');
	}

	*analyze_script_file_name = psprintf("%sanalyze_new_cluster.%s",
										 SCRIPT_PREFIX, SCRIPT_EXT);

	if ((script = fopen_priv(*analyze_script_file_name, "w")) == NULL)
		pg_fatal("could not open file \"%s\": %s\n",
				 *analyze_script_file_name, strerror(errno));

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
	fprintf(script, "echo %s    \"%s/vacuumdb\" %s--all --analyze-only%s\n", ECHO_QUOTE,
			new_cluster.bindir, user_specification.data, ECHO_QUOTE);
	fprintf(script, "echo%s\n\n", ECHO_BLANK);

	fprintf(script, "\"%s/vacuumdb\" %s--all --analyze-in-stages\n",
			new_cluster.bindir, user_specification.data);

	fprintf(script, "echo%s\n\n", ECHO_BLANK);
	fprintf(script, "echo %sDone%s\n",
			ECHO_QUOTE, ECHO_QUOTE);

	fclose(script);

#ifndef WIN32
	if (chmod(*analyze_script_file_name, S_IRWXU) != 0)
		pg_fatal("could not add execute permission to file \"%s\": %s\n",
				 *analyze_script_file_name, strerror(errno));
#endif

	termPQExpBuffer(&user_specification);

	check_ok();
}


/*
 * A previous run of pg_upgrade might have failed and the new cluster
 * directory recreated, but they might have forgotten to remove
 * the new cluster's tablespace directories.  Therefore, check that
 * new cluster tablespace directories do not already exist.  If
 * they do, it would cause an error while restoring global objects.
 * This allows the failure to be detected at check time, rather than
 * during schema restore.
 *
 * Note, v8.4 has no tablespace_suffix, which is fine so long as the
 * version being upgraded *to* has a suffix, since it's not allowed
 * to pg_upgrade from a version to the same version if tablespaces are
 * in use.
 */
static void
check_for_new_tablespace_dir(ClusterInfo *new_cluster)
{
	int		tblnum;
	char	new_tablespace_dir[MAXPGPATH];

	prep_status("Checking for new cluster tablespace directories");

	for (tblnum = 0; tblnum < os_info.num_old_tablespaces; tblnum++)
	{
		struct stat statbuf;

		snprintf(new_tablespace_dir, MAXPGPATH, "%s%s",
				os_info.old_tablespaces[tblnum],
				new_cluster->tablespace_suffix);

		if (stat(new_tablespace_dir, &statbuf) == 0 || errno != ENOENT)
			pg_fatal("new cluster tablespace directory already exists: \"%s\"\n",
					 new_tablespace_dir);
	}

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
	char		old_cluster_pgdata[MAXPGPATH],
				new_cluster_pgdata[MAXPGPATH];

	*deletion_script_file_name = psprintf("%sdelete_old_cluster.%s",
										  SCRIPT_PREFIX, SCRIPT_EXT);

	strlcpy(old_cluster_pgdata, old_cluster.pgdata, MAXPGPATH);
	canonicalize_path(old_cluster_pgdata);

	strlcpy(new_cluster_pgdata, new_cluster.pgdata, MAXPGPATH);
	canonicalize_path(new_cluster_pgdata);

	/* Some people put the new data directory inside the old one. */
	if (path_is_prefix_of_path(old_cluster_pgdata, new_cluster_pgdata))
	{
		pg_log(PG_WARNING,
			   "\nWARNING:  new data directory should not be inside the old data directory, e.g. %s\n", old_cluster_pgdata);

		/* Unlink file in case it is left over from a previous run. */
		unlink(*deletion_script_file_name);
		pg_free(*deletion_script_file_name);
		*deletion_script_file_name = NULL;
		return;
	}

	/*
	 * Some users (oddly) create tablespaces inside the cluster data
	 * directory.  We can't create a proper old cluster delete script in that
	 * case.
	 */
	for (tblnum = 0; tblnum < os_info.num_old_tablespaces; tblnum++)
	{
		char		old_tablespace_dir[MAXPGPATH];

		strlcpy(old_tablespace_dir, os_info.old_tablespaces[tblnum], MAXPGPATH);
		canonicalize_path(old_tablespace_dir);
		if (path_is_prefix_of_path(old_cluster_pgdata, old_tablespace_dir))
		{
			/* reproduce warning from CREATE TABLESPACE that is in the log */
			pg_log(PG_WARNING,
				   "\nWARNING:  user-defined tablespace locations should not be inside the data directory, e.g. %s\n", old_tablespace_dir);

			/* Unlink file in case it is left over from a previous run. */
			unlink(*deletion_script_file_name);
			pg_free(*deletion_script_file_name);
			*deletion_script_file_name = NULL;
			return;
		}
	}

	prep_status("Creating script to delete old cluster");

	if ((script = fopen_priv(*deletion_script_file_name, "w")) == NULL)
		pg_fatal("could not open file \"%s\": %s\n",
				 *deletion_script_file_name, strerror(errno));

#ifndef WIN32
	/* add shebang header */
	fprintf(script, "#!/bin/sh\n\n");
#endif

	/* delete old cluster's default tablespace */
	fprintf(script, RMDIR_CMD " %c%s%c\n", PATH_QUOTE,
			fix_path_separator(old_cluster.pgdata), PATH_QUOTE);

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
				fprintf(script, RM_CMD " %s%cPG_VERSION\n",
						fix_path_separator(os_info.old_tablespaces[tblnum]),
						PATH_SEPARATOR);

			for (dbnum = 0; dbnum < old_cluster.dbarr.ndbs; dbnum++)
				fprintf(script, RMDIR_CMD " %c%s%c%d%c\n", PATH_QUOTE,
						fix_path_separator(os_info.old_tablespaces[tblnum]),
						PATH_SEPARATOR, old_cluster.dbarr.dbs[dbnum].db_oid,
						PATH_QUOTE);
		}
		else
		{
			char	   *suffix_path = pg_strdup(old_cluster.tablespace_suffix);

			/*
			 * Simply delete the tablespace directory, which might be ".old"
			 * or a version-specific subdirectory.
			 */
			fprintf(script, RMDIR_CMD " %c%s%s%c\n", PATH_QUOTE,
					fix_path_separator(os_info.old_tablespaces[tblnum]),
					fix_path_separator(suffix_path), PATH_QUOTE);
			pfree(suffix_path);
		}
	}

	fclose(script);

#ifndef WIN32
	if (chmod(*deletion_script_file_name, S_IRWXU) != 0)
		pg_fatal("could not add execute permission to file \"%s\": %s\n",
				 *deletion_script_file_name, strerror(errno));
#endif

	check_ok();
}


/*
 *	check_is_install_user()
 *
 *	Check we are the install user, and that the new cluster
 *	has no other users.
 */
static void
check_is_install_user(ClusterInfo *cluster)
{
	PGresult   *res;
	PGconn	   *conn = connectToServer(cluster, "template1");

	prep_status("Checking database user is the install user");

	/* Can't use pg_authid because only superusers can view it. */
	res = executeQueryOrDie(conn,
							"SELECT rolsuper, oid "
							"FROM pg_catalog.pg_roles "
							"WHERE rolname = current_user "
							"AND rolname !~ '^pg_'");

	/*
	 * We only allow the install user in the new cluster (see comment below)
	 * and we preserve pg_authid.oid, so this must be the install user in the
	 * old cluster too.
	 */
	if (PQntuples(res) != 1 ||
		atooid(PQgetvalue(res, 0, 1)) != BOOTSTRAP_SUPERUSERID)
		pg_fatal("database user \"%s\" is not the install user\n",
				 os_info.user);

	PQclear(res);

	res = executeQueryOrDie(conn,
							"SELECT COUNT(*) "
							"FROM pg_catalog.pg_roles "
							"WHERE rolname !~ '^pg_'");

	if (PQntuples(res) != 1)
		pg_fatal("could not determine the number of users\n");

	/*
	 * We only allow the install user in the new cluster because other defined
	 * users might match users defined in the old cluster and generate an
	 * error during pg_dump restore.
	 */
	if (cluster == &new_cluster && atooid(PQgetvalue(res, 0, 0)) != 1)
		pg_fatal("Only the install user can be defined in the new cluster.\n");

	PQclear(res);

	PQfinish(conn);

	check_ok();
}


static void
check_proper_datallowconn(ClusterInfo *cluster)
{
	int			dbnum;
	PGconn	   *conn_template1;
	PGresult   *dbres;
	int			ntups;
	int			i_datname;
	int			i_datallowconn;

	prep_status("Checking database connection settings");

	conn_template1 = connectToServer(cluster, "template1");

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

		if (strcmp(datname, "template0") == 0)
		{
			/* avoid restore failure when pg_dumpall tries to create template0 */
			if (strcmp(datallowconn, "t") == 0)
				pg_fatal("template0 must not allow connections, "
						 "i.e. its pg_database.datallowconn must be false\n");
		}
		else
		{
			/*
			 * avoid datallowconn == false databases from being skipped on
			 * restore
			 */
			if (strcmp(datallowconn, "f") == 0)
				pg_fatal("All non-template0 databases must allow connections, "
						 "i.e. their pg_database.datallowconn must be true\n");
		}
	}

	PQclear(dbres);

	PQfinish(conn_template1);

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
	{
		if (cluster == &old_cluster)
			pg_fatal("The source cluster contains prepared transactions\n");
		else
			pg_fatal("The target cluster contains prepared transactions\n");
	}

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
				pg_fatal("could not open file \"%s\": %s\n",
						 output_path, strerror(errno));
			if (!db_used)
			{
				fprintf(script, "In database: %s\n", active_db->db_name);
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
		pg_fatal("Your installation contains \"contrib/isn\" functions which rely on the\n"
				 "bigint data type.  Your old and new clusters pass bigint values\n"
				 "differently so this cluster cannot currently be upgraded.  You can\n"
				 "manually dump databases in the old cluster that use \"contrib/isn\"\n"
				 "facilities, drop them, perform the upgrade, and then restore them.  A\n"
				 "list of the problem functions is in the file:\n"
				 "    %s\n\n", output_path);
	}
	else
		check_ok();
}


/*
 * Verify that no tables are declared WITH OIDS.
 */
static void
check_for_tables_with_oids(ClusterInfo *cluster)
{
	int			dbnum;
	FILE	   *script = NULL;
	bool		found = false;
	char		output_path[MAXPGPATH];

	prep_status("Checking for tables WITH OIDS");

	snprintf(output_path, sizeof(output_path),
			 "tables_with_oids.txt");

	/* Find any tables declared WITH OIDS */
	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_nspname,
					i_relname;
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);

		res = executeQueryOrDie(conn,
								"SELECT n.nspname, c.relname "
								"FROM	pg_catalog.pg_class c, "
								"		pg_catalog.pg_namespace n "
								"WHERE	c.relnamespace = n.oid AND "
								"		c.relhasoids AND"
								"       n.nspname NOT IN ('pg_catalog')");

		ntups = PQntuples(res);
		i_nspname = PQfnumber(res, "nspname");
		i_relname = PQfnumber(res, "relname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			found = true;
			if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
				pg_fatal("could not open file \"%s\": %s\n",
						 output_path, strerror(errno));
			if (!db_used)
			{
				fprintf(script, "In database: %s\n", active_db->db_name);
				db_used = true;
			}
			fprintf(script, "  %s.%s\n",
					PQgetvalue(res, rowno, i_nspname),
					PQgetvalue(res, rowno, i_relname));
		}

		PQclear(res);

		PQfinish(conn);
	}

	if (script)
		fclose(script);

	if (found)
	{
		pg_log(PG_REPORT, "fatal\n");
		pg_fatal("Your installation contains tables declared WITH OIDS, which is not\n"
				 "supported anymore.  Consider removing the oid column using\n"
				 "    ALTER TABLE ... SET WITHOUT OIDS;\n"
				 "A list of tables with the problem is in the file:\n"
				 "    %s\n\n", output_path);
	}
	else
		check_ok();
}


/*
 * check_for_composite_data_type_usage()
 *	Check for system-defined composite types used in user tables.
 *
 *	The OIDs of rowtypes of system catalogs and information_schema views
 *	can change across major versions; unlike user-defined types, we have
 *	no mechanism for forcing them to be the same in the new cluster.
 *	Hence, if any user table uses one, that's problematic for pg_upgrade.
 */
static void
check_for_composite_data_type_usage(ClusterInfo *cluster)
{
	bool		found;
	Oid			firstUserOid;
	char		output_path[MAXPGPATH];
	char	   *base_query;

	prep_status("Checking for system-defined composite types in user tables");

	snprintf(output_path, sizeof(output_path), "tables_using_composite.txt");

	/*
	 * Look for composite types that were made during initdb *or* belong to
	 * information_schema; that's important in case information_schema was
	 * dropped and reloaded.
	 *
	 * The cutoff OID here should match the source cluster's value of
	 * FirstNormalObjectId.  We hardcode it rather than using that C #define
	 * because, if that #define is ever changed, our own version's value is
	 * NOT what to use.  Eventually we may need a test on the source cluster's
	 * version to select the correct value.
	 */
	firstUserOid = 16384;

	base_query = psprintf("SELECT t.oid FROM pg_catalog.pg_type t "
						  "LEFT JOIN pg_catalog.pg_namespace n ON t.typnamespace = n.oid "
						  " WHERE typtype = 'c' AND (t.oid < %u OR nspname = 'information_schema')",
						  firstUserOid);

	found = check_for_data_types_usage(cluster, base_query, output_path);

	free(base_query);

	if (found)
	{
		pg_log(PG_REPORT, "fatal\n");
		pg_fatal("Your installation contains system-defined composite type(s) in user tables.\n"
				 "These type OIDs are not stable across PostgreSQL versions,\n"
				 "so this cluster cannot currently be upgraded.  You can\n"
				 "drop the problem columns and restart the upgrade.\n"
				 "A list of the problem columns is in the file:\n"
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
	bool		found;
	char		output_path[MAXPGPATH];

	prep_status("Checking for reg* data types in user tables");

	snprintf(output_path, sizeof(output_path), "tables_using_reg.txt");

	/*
	 * Note: older servers will not have all of these reg* types, so we have
	 * to write the query like this rather than depending on casts to regtype.
	 */
	found = check_for_data_types_usage(cluster,
									   "SELECT oid FROM pg_catalog.pg_type t "
									   "WHERE t.typnamespace = "
									   "        (SELECT oid FROM pg_catalog.pg_namespace "
									   "         WHERE nspname = 'pg_catalog') "
									   "  AND t.typname IN ( "
	/* pg_class.oid is preserved, so 'regclass' is OK */
									   "           'regcollation', "
									   "           'regconfig', "
									   "           'regdictionary', "
									   "           'regnamespace', "
									   "           'regoper', "
									   "           'regoperator', "
									   "           'regproc', "
									   "           'regprocedure' "
	/* pg_authid.oid is preserved, so 'regrole' is OK */
	/* pg_type.oid is (mostly) preserved, so 'regtype' is OK */
									   "         )",
									   output_path);

	if (found)
	{
		pg_log(PG_REPORT, "fatal\n");
		pg_fatal("Your installation contains one of the reg* data types in user tables.\n"
				 "These data types reference system OIDs that are not preserved by\n"
				 "pg_upgrade, so this cluster cannot currently be upgraded.  You can\n"
				 "remove the problem tables and restart the upgrade.  A list of the\n"
				 "problem columns is in the file:\n"
				 "    %s\n\n", output_path);
	}
	else
		check_ok();
}


/*
 * check_for_jsonb_9_4_usage()
 *
 *	JSONB changed its storage format during 9.4 beta, so check for it.
 */
static void
check_for_jsonb_9_4_usage(ClusterInfo *cluster)
{
	char		output_path[MAXPGPATH];

	prep_status("Checking for incompatible \"jsonb\" data type");

	snprintf(output_path, sizeof(output_path), "tables_using_jsonb.txt");

	if (check_for_data_type_usage(cluster, "pg_catalog.jsonb", output_path))
	{
		pg_log(PG_REPORT, "fatal\n");
		pg_fatal("Your installation contains the \"jsonb\" data type in user tables.\n"
				 "The internal format of \"jsonb\" changed during 9.4 beta so this\n"
				 "cluster cannot currently be upgraded.  You can remove the problem\n"
				 "tables and restart the upgrade.  A list of the problem columns is\n"
				 "in the file:\n"
				 "    %s\n\n", output_path);
	}
	else
		check_ok();
}

/*
 * check_for_pg_role_prefix()
 *
 *	Versions older than 9.6 should not have any pg_* roles
 */
static void
check_for_pg_role_prefix(ClusterInfo *cluster)
{
	PGresult   *res;
	PGconn	   *conn = connectToServer(cluster, "template1");

	prep_status("Checking for roles starting with \"pg_\"");

	res = executeQueryOrDie(conn,
							"SELECT * "
							"FROM pg_catalog.pg_roles "
							"WHERE rolname ~ '^pg_'");

	if (PQntuples(res) != 0)
	{
		if (cluster == &old_cluster)
			pg_fatal("The source cluster contains roles starting with \"pg_\"\n");
		else
			pg_fatal("The target cluster contains roles starting with \"pg_\"\n");
	}

	PQclear(res);

	PQfinish(conn);

	check_ok();
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

	/* get the current setting, so we can restore it. */
	save = setlocale(category, NULL);
	if (!save)
		pg_fatal("failed to get the current locale\n");

	/* 'save' may be pointing at a modifiable scratch variable, so copy it. */
	save = pg_strdup(save);

	/* set the locale with setlocale, to see if it accepts it. */
	res = setlocale(category, locale);

	if (!res)
		pg_fatal("failed to get system locale name for \"%s\"\n", locale);

	res = pg_strdup(res);

	/* restore old value. */
	if (!setlocale(category, save))
		pg_fatal("failed to restore old locale \"%s\"\n", save);

	pg_free(save);

	return res;
}
