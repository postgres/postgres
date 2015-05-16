/*
 *	check.c
 *
 *	server checks and output routines
 *
 *	Copyright (c) 2010, PostgreSQL Global Development Group
 *	$PostgreSQL: pgsql/contrib/pg_upgrade/check.c,v 1.11.2.3 2010/07/25 03:47:33 momjian Exp $
 */

#include "pg_upgrade.h"


static void set_locale_and_encoding(migratorContext *ctx, Cluster whichCluster);
static void check_new_db_is_empty(migratorContext *ctx);
static void check_locale_and_encoding(migratorContext *ctx, ControlData *oldctrl,
						  ControlData *newctrl);
static void check_proper_datallowconn(migratorContext *ctx, Cluster whichCluster);
static void check_for_isn_and_int8_passing_mismatch(migratorContext *ctx,
												Cluster whichCluster);
static void check_for_reg_data_type_usage(migratorContext *ctx, Cluster whichCluster);


/*
 * fix_path_separator
 * For non-Windows, just return the argument.
 * For Windows convert any forward slash to a backslash
 * such as is suitable for arguments to builtin commands 
 * like RMDIR and DEL.
 */
static char *
fix_path_separator(migratorContext *ctx, char *path)
{
#ifdef WIN32

	char *result;
	char *c;

	result = pg_strdup(ctx, path);

	for (c = result; *c != '\0'; c++)
		if (*c == '/')
			*c = '\\';

	return result;

#else

	return path;

#endif
}

void
output_check_banner(migratorContext *ctx, bool *live_check)
{
	if (ctx->check && is_server_running(ctx, ctx->old.pgdata))
	{
		*live_check = true;
		if (ctx->old.port == ctx->new.port)
			pg_log(ctx, PG_FATAL, "When checking a live server, "
				   "the old and new port numbers must be different.\n");
		pg_log(ctx, PG_REPORT, "Performing Consistency Checks on Old Live Server\n");
		pg_log(ctx, PG_REPORT, "------------------------------------------------\n");
	}
	else
	{
		pg_log(ctx, PG_REPORT, "Performing Consistency Checks\n");
		pg_log(ctx, PG_REPORT, "-----------------------------\n");
	}
}


void
check_old_cluster(migratorContext *ctx, bool live_check,
				  char **sequence_script_file_name)
{
	/* -- OLD -- */

	if (!live_check)
		start_postmaster(ctx, CLUSTER_OLD, false);

	set_locale_and_encoding(ctx, CLUSTER_OLD);

	get_pg_database_relfilenode(ctx, CLUSTER_OLD);

	/* Extract a list of databases and tables from the old cluster */
	get_db_and_rel_infos(ctx, &ctx->old.dbarr, CLUSTER_OLD);

	init_tablespaces(ctx);

	get_loadable_libraries(ctx);


	/*
	 * Check for various failure cases
	 */

	check_proper_datallowconn(ctx, CLUSTER_OLD);
	check_for_reg_data_type_usage(ctx, CLUSTER_OLD);
	check_for_isn_and_int8_passing_mismatch(ctx, CLUSTER_OLD);

	/* old = PG 8.3 checks? */
	if (GET_MAJOR_VERSION(ctx->old.major_version) <= 803)
	{
		old_8_3_check_for_name_data_type_usage(ctx, CLUSTER_OLD);
		old_8_3_check_for_tsquery_usage(ctx, CLUSTER_OLD);
		old_8_3_check_ltree_usage(ctx, CLUSTER_OLD);
		if (ctx->check)
		{
			old_8_3_rebuild_tsvector_tables(ctx, true, CLUSTER_OLD);
			old_8_3_invalidate_hash_gin_indexes(ctx, true, CLUSTER_OLD);
			old_8_3_invalidate_bpchar_pattern_ops_indexes(ctx, true, CLUSTER_OLD);
		}
		else

			/*
			 * While we have the old server running, create the script to
			 * properly restore its sequence values but we report this at the
			 * end.
			 */
			*sequence_script_file_name =
				old_8_3_create_sequence_script(ctx, CLUSTER_OLD);
	}

	/* Pre-PG 9.0 had no large object permissions */
	if (GET_MAJOR_VERSION(ctx->old.major_version) <= 804)
		new_9_0_populate_pg_largeobject_metadata(ctx, true, CLUSTER_OLD);

	/*
	 * While not a check option, we do this now because this is the only time
	 * the old server is running.
	 */
	if (!ctx->check)
	{
		generate_old_dump(ctx);
		split_old_dump(ctx);
	}

	if (!live_check)
		stop_postmaster(ctx, false, false);
}


void
check_new_cluster(migratorContext *ctx)
{
	set_locale_and_encoding(ctx, CLUSTER_NEW);

	check_new_db_is_empty(ctx);

	check_loadable_libraries(ctx);

	check_locale_and_encoding(ctx, &ctx->old.controldata, &ctx->new.controldata);

	if (ctx->transfer_mode == TRANSFER_MODE_LINK)
		check_hard_link(ctx);
}


void
report_clusters_compatible(migratorContext *ctx)
{
	if (ctx->check)
	{
		pg_log(ctx, PG_REPORT, "\n*Clusters are compatible*\n");
		/* stops new cluster */
		stop_postmaster(ctx, false, false);
		exit_nicely(ctx, false);
	}

	pg_log(ctx, PG_REPORT, "\n"
		   "| If pg_upgrade fails after this point, you must\n"
		   "| re-initdb the new cluster before continuing.\n"
		   "| You will also need to remove the \".old\" suffix\n"
		   "| from %s/global/pg_control.old.\n", ctx->old.pgdata);
}


void
issue_warnings(migratorContext *ctx, char *sequence_script_file_name)
{
	/* old = PG 8.3 warnings? */
	if (GET_MAJOR_VERSION(ctx->old.major_version) <= 803)
	{
		start_postmaster(ctx, CLUSTER_NEW, true);

		/* restore proper sequence values using file created from old server */
		if (sequence_script_file_name)
		{
			prep_status(ctx, "Adjusting sequences");
			exec_prog(ctx, true,
				  SYSTEMQUOTE "\"%s/psql\" --set ON_ERROR_STOP=on "
				  "--no-psqlrc --port %d --username \"%s\" "
				  "-f \"%s\" --dbname template1 >> \"%s\"" SYSTEMQUOTE,
					  ctx->new.bindir, ctx->new.port, ctx->user,
					  sequence_script_file_name, ctx->logfile);
			unlink(sequence_script_file_name);
			check_ok(ctx);
		}

		old_8_3_rebuild_tsvector_tables(ctx, false, CLUSTER_NEW);
		old_8_3_invalidate_hash_gin_indexes(ctx, false, CLUSTER_NEW);
		old_8_3_invalidate_bpchar_pattern_ops_indexes(ctx, false, CLUSTER_NEW);
		stop_postmaster(ctx, false, true);
	}

	/* Create dummy large object permissions for old < PG 9.0? */
	if (GET_MAJOR_VERSION(ctx->old.major_version) <= 804)
	{
		start_postmaster(ctx, CLUSTER_NEW, true);
		new_9_0_populate_pg_largeobject_metadata(ctx, false, CLUSTER_NEW);
		stop_postmaster(ctx, false, true);
	}
}


void
output_completion_banner(migratorContext *ctx, char *deletion_script_file_name)
{
	/* Did we migrate the free space files? */
	if (GET_MAJOR_VERSION(ctx->old.major_version) >= 804)
		pg_log(ctx, PG_REPORT,
			   "| Optimizer statistics is not transferred by pg_upgrade\n"
			   "| so consider running:\n"
			   "| \tvacuumdb --all --analyze-only\n"
			   "| on the newly-upgraded cluster.\n\n");
	else
		pg_log(ctx, PG_REPORT,
			   "| Optimizer statistics and free space information\n"
			   "| are not transferred by pg_upgrade so consider\n"
			   "| running:\n"
			   "| \tvacuumdb --all --analyze\n"
			   "| on the newly-upgraded cluster.\n\n");

	pg_log(ctx, PG_REPORT,
		   "| Running this script will delete the old cluster's data files:\n"
		   "| \t%s\n",
		   deletion_script_file_name);
}


void
check_cluster_versions(migratorContext *ctx)
{
	/* get old and new cluster versions */
	ctx->old.major_version = get_major_server_version(ctx, &ctx->old.major_version_str, CLUSTER_OLD);
	ctx->new.major_version = get_major_server_version(ctx, &ctx->new.major_version_str, CLUSTER_NEW);

	/* We allow migration from/to the same major version for beta upgrades */

	if (GET_MAJOR_VERSION(ctx->old.major_version) < 803)
		pg_log(ctx, PG_FATAL, "This utility can only upgrade from PostgreSQL version 8.3 and later.\n");

	/* Only current PG version is supported as a target */
	if (GET_MAJOR_VERSION(ctx->new.major_version) != GET_MAJOR_VERSION(PG_VERSION_NUM))
		pg_log(ctx, PG_FATAL, "This utility can only upgrade to PostgreSQL version %s.\n",
			   PG_MAJORVERSION);

	/*
	 * We can't allow downgrading because we use the target pg_dumpall, and
	 * pg_dumpall cannot operate on new datbase versions, only older versions.
	 */
	if (ctx->old.major_version > ctx->new.major_version)
		pg_log(ctx, PG_FATAL, "This utility cannot be used to downgrade to older major PostgreSQL versions.\n");
}


void
check_cluster_compatibility(migratorContext *ctx, bool live_check)
{
	char		libfile[MAXPGPATH];
	FILE	   *lib_test;

	/*
	 * Test pg_upgrade_support.so is in the proper place.    We cannot copy it
	 * ourselves because install directories are typically root-owned.
	 */
	snprintf(libfile, sizeof(libfile), "%s/pg_upgrade_support%s", ctx->new.libpath,
			 DLSUFFIX);

	if ((lib_test = fopen(libfile, "r")) == NULL)
		pg_log(ctx, PG_FATAL,
			   "\npg_upgrade_support%s must be created and installed in %s\n", DLSUFFIX, libfile);
	else
		fclose(lib_test);

	/* get/check pg_control data of servers */
	get_control_data(ctx, &ctx->old, live_check);
	get_control_data(ctx, &ctx->new, false);
	check_control_data(ctx, &ctx->old.controldata, &ctx->new.controldata);

	/* Is it 9.0 but without tablespace directories? */
	if (GET_MAJOR_VERSION(ctx->new.major_version) == 900 &&
		ctx->new.controldata.cat_ver < TABLE_SPACE_SUBDIRS)
		pg_log(ctx, PG_FATAL, "This utility can only upgrade to PostgreSQL version 9.0 after 2010-01-11\n"
			   "because of backend API changes made during development.\n");
}


/*
 * set_locale_and_encoding()
 *
 * query the database to get the template0 locale
 */
static void
set_locale_and_encoding(migratorContext *ctx, Cluster whichCluster)
{
	PGconn	   *conn;
	PGresult   *res;
	int			i_encoding;
	ControlData *ctrl = (whichCluster == CLUSTER_OLD) ?
	&ctx->old.controldata : &ctx->new.controldata;
	int			cluster_version = (whichCluster == CLUSTER_OLD) ?
	ctx->old.major_version : ctx->new.major_version;

	conn = connectToServer(ctx, "template1", whichCluster);

	/* for pg < 80400, we got the values from pg_controldata */
	if (cluster_version >= 80400)
	{
		int			i_datcollate;
		int			i_datctype;

		res = executeQueryOrDie(ctx, conn,
								"SELECT datcollate, datctype "
								"FROM 	pg_catalog.pg_database "
								"WHERE	datname = 'template0' ");
		assert(PQntuples(res) == 1);

		i_datcollate = PQfnumber(res, "datcollate");
		i_datctype = PQfnumber(res, "datctype");

		ctrl->lc_collate = pg_strdup(ctx, PQgetvalue(res, 0, i_datcollate));
		ctrl->lc_ctype = pg_strdup(ctx, PQgetvalue(res, 0, i_datctype));

		PQclear(res);
	}

	res = executeQueryOrDie(ctx, conn,
							"SELECT pg_catalog.pg_encoding_to_char(encoding) "
							"FROM 	pg_catalog.pg_database "
							"WHERE	datname = 'template0' ");
	assert(PQntuples(res) == 1);

	i_encoding = PQfnumber(res, "pg_encoding_to_char");
	ctrl->encoding = pg_strdup(ctx, PQgetvalue(res, 0, i_encoding));

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
check_locale_and_encoding(migratorContext *ctx, ControlData *oldctrl,
						  ControlData *newctrl)
{
	if (strcmp(oldctrl->lc_collate, newctrl->lc_collate) != 0)
		pg_log(ctx, PG_FATAL,
			   "old and new cluster lc_collate values do not match\n");
	if (strcmp(oldctrl->lc_ctype, newctrl->lc_ctype) != 0)
		pg_log(ctx, PG_FATAL,
			   "old and new cluster lc_ctype values do not match\n");
	if (strcmp(oldctrl->encoding, newctrl->encoding) != 0)
		pg_log(ctx, PG_FATAL,
			   "old and new cluster encoding values do not match\n");
}


static void
check_new_db_is_empty(migratorContext *ctx)
{
	int			dbnum;
	bool		found = false;

	get_db_and_rel_infos(ctx, &ctx->new.dbarr, CLUSTER_NEW);

	for (dbnum = 0; dbnum < ctx->new.dbarr.ndbs; dbnum++)
	{
		int			relnum;
		RelInfoArr *rel_arr = &ctx->new.dbarr.dbs[dbnum].rel_arr;

		for (relnum = 0; relnum < rel_arr->nrels;
			 relnum++)
		{
			/* pg_largeobject and its index should be skipped */
			if (strcmp(rel_arr->rels[relnum].nspname, "pg_catalog") != 0)
			{
				found = true;
				break;
			}
		}
	}

	dbarr_free(&ctx->new.dbarr);

	if (found)
		pg_log(ctx, PG_FATAL, "New cluster is not empty; exiting\n");
}


/*
 * create_script_for_old_cluster_deletion()
 *
 *	This is particularly useful for tablespace deletion.
 */
void
create_script_for_old_cluster_deletion(migratorContext *ctx,
									   char **deletion_script_file_name)
{
	FILE	   *script = NULL;
	int			tblnum;

	*deletion_script_file_name = pg_malloc(ctx, MAXPGPATH);

	prep_status(ctx, "Creating script to delete old cluster");

	snprintf(*deletion_script_file_name, MAXPGPATH, "%s/delete_old_cluster.%s",
			 ctx->cwd, SHELL_EXT);

	if ((script = fopen(*deletion_script_file_name, "w")) == NULL)
		pg_log(ctx, PG_FATAL, "Could not create necessary file:  %s\n",
			   *deletion_script_file_name);

#ifndef WIN32
	/* add shebang header */
	fprintf(script, "#!/bin/sh\n\n");
#endif

	/* delete old cluster's default tablespace */
	fprintf(script, RMDIR_CMD " \"%s\"\n", fix_path_separator(ctx, ctx->old.pgdata));

	/* delete old cluster's alternate tablespaces */
	for (tblnum = 0; tblnum < ctx->num_tablespaces; tblnum++)
	{
		/*
		 * Do the old cluster's per-database directories share a directory
		 * with a new version-specific tablespace?
		 */
		if (strlen(ctx->old.tablespace_suffix) == 0)
		{
			/* delete per-database directories */
			int			dbnum;

			fprintf(script, "\n");
			/* remove PG_VERSION? */
			if (GET_MAJOR_VERSION(ctx->old.major_version) <= 804)
				fprintf(script, RM_CMD " %s%s%cPG_VERSION\n",
						fix_path_separator(ctx, ctx->tablespaces[tblnum]),
						fix_path_separator(ctx, ctx->old.tablespace_suffix),
						PATH_SEPARATOR);

			for (dbnum = 0; dbnum < ctx->new.dbarr.ndbs; dbnum++)
			{
				fprintf(script, RMDIR_CMD " \"%s%s%c%d\"\n",
						fix_path_separator(ctx, ctx->tablespaces[tblnum]),
						fix_path_separator(ctx, ctx->old.tablespace_suffix),
						PATH_SEPARATOR, ctx->old.dbarr.dbs[dbnum].db_oid);
			}
		}
		else

			/*
			 * Simply delete the tablespace directory, which might be ".old"
			 * or a version-specific subdirectory.
			 */
			fprintf(script, RMDIR_CMD " \"%s%s\"\n",
					fix_path_separator(ctx, ctx->tablespaces[tblnum]),
					fix_path_separator(ctx, ctx->old.tablespace_suffix));
	}

	fclose(script);

#ifndef WIN32
	if (chmod(*deletion_script_file_name, S_IRWXU) != 0)
		pg_log(ctx, PG_FATAL, "Could not add execute permission to file:  %s\n",
			   *deletion_script_file_name);
#endif

	check_ok(ctx);
}


static void
check_proper_datallowconn(migratorContext *ctx, Cluster whichCluster)
{
	int			dbnum;
	PGconn	   *conn_template1;
	PGresult   *dbres;
	int			ntups;
	int			i_datname;
	int			i_datallowconn;

	prep_status(ctx, "Checking database connection settings");

	conn_template1 = connectToServer(ctx, "template1", whichCluster);

	/* get database names */
	dbres = executeQueryOrDie(ctx, conn_template1,
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
				pg_log(ctx, PG_FATAL, "template0 must not allow connections, "
						 "i.e. its pg_database.datallowconn must be false\n");
		}
		else
		{
			/* avoid datallowconn == false databases from being skipped on restore */
			if (strcmp(datallowconn, "f") == 0)
				pg_log(ctx, PG_FATAL, "All non-template0 databases must allow connections, "
						 "i.e. their pg_database.datallowconn must be true\n");
		}
	}

	PQclear(dbres);

	PQfinish(conn_template1);

	check_ok(ctx);
}


/*
 * 	check_for_isn_and_int8_passing_mismatch()
 *
 *	/contrib/isn relies on data type int8, and in 8.4 int8 can now be passed
 *	by value.  The schema dumps the CREATE TYPE PASSEDBYVALUE setting so
 *	it must match for the old and new servers.
 */
void
check_for_isn_and_int8_passing_mismatch(migratorContext *ctx, Cluster whichCluster)
{
	ClusterInfo *active_cluster = (whichCluster == CLUSTER_OLD) ?
	&ctx->old : &ctx->new;
	int			dbnum;
	FILE	   *script = NULL;
	bool		found = false;
	char		output_path[MAXPGPATH];

	prep_status(ctx, "Checking for /contrib/isn with bigint-passing mismatch");

	if (ctx->old.controldata.float8_pass_by_value ==
		ctx->new.controldata.float8_pass_by_value)
	{
		/* no mismatch */
		check_ok(ctx);
		return;
	}

	snprintf(output_path, sizeof(output_path), "%s/contrib_isn_and_int8_pass_by_value.txt",
			 ctx->cwd);

	for (dbnum = 0; dbnum < active_cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_nspname,
					i_proname;
		DbInfo	   *active_db = &active_cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(ctx, active_db->db_name, whichCluster);

		/* Find any functions coming from contrib/isn */
		res = executeQueryOrDie(ctx, conn,
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
			if (script == NULL && (script = fopen(output_path, "w")) == NULL)
				pg_log(ctx, PG_FATAL, "Could not create necessary file:  %s\n", output_path);
			if (!db_used)
			{
				fprintf(script, "Database:  %s\n", active_db->db_name);
				db_used = true;
			}
			fprintf(script, "  %s.%s\n",
					PQgetvalue(res, rowno, i_nspname),
					PQgetvalue(res, rowno, i_proname));
		}

		PQclear(res);

		PQfinish(conn);
	}

	if (found)
	{
		fclose(script);
		pg_log(ctx, PG_REPORT, "fatal\n");
		pg_log(ctx, PG_FATAL,
			   "| Your installation contains \"/contrib/isn\" functions\n"
			   "| which rely on the bigint data type.  Your old and\n"
			   "| new clusters pass bigint values differently so this\n"
			   "| cluster cannot currently be upgraded.  You can\n"
			   "| manually migrate data that use \"/contrib/isn\"\n"
			   "| facilities and remove \"/contrib/isn\" from the\n"
			   "| old cluster and restart the migration.  A list\n"
			   "| of the problem functions is in the file:\n"
			   "| \t%s\n\n", output_path);
	}
	else
		check_ok(ctx);
}


/*
 * check_for_reg_data_type_usage()
 *	pg_upgrade only preserves these system values:
 *		pg_class.relfilenode
 *		pg_type.oid
 *		pg_enum.oid
 *
 *  Most of the reg* data types reference system catalog info that is
 *	not preserved, and hence these data types cannot be used in user
 *	tables upgraded by pg_upgrade.
 */
void
check_for_reg_data_type_usage(migratorContext *ctx, Cluster whichCluster)
{
	ClusterInfo *active_cluster = (whichCluster == CLUSTER_OLD) ?
	&ctx->old : &ctx->new;
	int			dbnum;
	FILE	   *script = NULL;
	bool		found = false;
	char		output_path[MAXPGPATH];

	prep_status(ctx, "Checking for reg* system oid user data types");

	snprintf(output_path, sizeof(output_path), "%s/tables_using_reg.txt",
			 ctx->cwd);

	for (dbnum = 0; dbnum < active_cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_nspname,
					i_relname,
					i_attname;
		DbInfo	   *active_db = &active_cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(ctx, active_db->db_name, whichCluster);

		res = executeQueryOrDie(ctx, conn,
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
/*	allow						"			'pg_catalog.regclass'::pg_catalog.regtype, " */
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
			if (script == NULL && (script = fopen(output_path, "w")) == NULL)
				pg_log(ctx, PG_FATAL, "Could not create necessary file:  %s\n", output_path);
			if (!db_used)
			{
				fprintf(script, "Database:  %s\n", active_db->db_name);
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

	if (found)
	{
		fclose(script);
		pg_log(ctx, PG_REPORT, "fatal\n");
		pg_log(ctx, PG_FATAL,
			   "| Your installation contains one of the reg* data types in\n"
			   "| user tables.  These data types reference system oids that\n"
			   "| are not preserved by pg_upgrade, so this cluster cannot\n"
			   "| currently be upgraded.  You can remove the problem tables\n"
			   "| and restart the migration.  A list of the problem columns\n"
			   "| is in the file:\n"
			   "| \t%s\n\n", output_path);
	}
	else
		check_ok(ctx);
}
