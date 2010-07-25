/*
 *	function.c
 *
 *	server-side function support
 *
 *	Copyright (c) 2010, PostgreSQL Global Development Group
 *	$PostgreSQL: pgsql/contrib/pg_upgrade/function.c,v 1.6.2.1 2010/07/25 03:28:39 momjian Exp $
 */

#include "pg_upgrade.h"

#include "access/transam.h"


/*
 * install_support_functions()
 *
 * pg_upgrade requires some support functions that enable it to modify
 * backend behavior.
 */
void
install_support_functions(migratorContext *ctx)
{
	int			dbnum;

	prep_status(ctx, "Adding support functions to new cluster");

	for (dbnum = 0; dbnum < ctx->new.dbarr.ndbs; dbnum++)
	{
		DbInfo	   *newdb = &ctx->new.dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(ctx, newdb->db_name, CLUSTER_NEW);

		/* suppress NOTICE of dropped objects */
		PQclear(executeQueryOrDie(ctx, conn,
								  "SET client_min_messages = warning;"));
		PQclear(executeQueryOrDie(ctx, conn,
						   "DROP SCHEMA IF EXISTS binary_upgrade CASCADE;"));
		PQclear(executeQueryOrDie(ctx, conn,
								  "RESET client_min_messages;"));

		PQclear(executeQueryOrDie(ctx, conn,
								  "CREATE SCHEMA binary_upgrade;"));

		PQclear(executeQueryOrDie(ctx, conn,
								  "CREATE OR REPLACE FUNCTION "
					 "		binary_upgrade.set_next_pg_type_oid(OID) "
								  "RETURNS VOID "
								  "AS '$libdir/pg_upgrade_support' "
								  "LANGUAGE C STRICT;"));
		PQclear(executeQueryOrDie(ctx, conn,
								  "CREATE OR REPLACE FUNCTION "
			   "		binary_upgrade.set_next_pg_type_array_oid(OID) "
								  "RETURNS VOID "
								  "AS '$libdir/pg_upgrade_support' "
								  "LANGUAGE C STRICT;"));
		PQclear(executeQueryOrDie(ctx, conn,
								  "CREATE OR REPLACE FUNCTION "
			   "		binary_upgrade.set_next_pg_type_toast_oid(OID) "
								  "RETURNS VOID "
								  "AS '$libdir/pg_upgrade_support' "
								  "LANGUAGE C STRICT;"));
		PQclear(executeQueryOrDie(ctx, conn,
								  "CREATE OR REPLACE FUNCTION "
				"		binary_upgrade.set_next_heap_relfilenode(OID) "
								  "RETURNS VOID "
								  "AS '$libdir/pg_upgrade_support' "
								  "LANGUAGE C STRICT;"));
		PQclear(executeQueryOrDie(ctx, conn,
								  "CREATE OR REPLACE FUNCTION "
			   "		binary_upgrade.set_next_toast_relfilenode(OID) "
								  "RETURNS VOID "
								  "AS '$libdir/pg_upgrade_support' "
								  "LANGUAGE C STRICT;"));
		PQclear(executeQueryOrDie(ctx, conn,
								  "CREATE OR REPLACE FUNCTION "
			   "		binary_upgrade.set_next_index_relfilenode(OID) "
								  "RETURNS VOID "
								  "AS '$libdir/pg_upgrade_support' "
								  "LANGUAGE C STRICT;"));
		PQclear(executeQueryOrDie(ctx, conn,
								  "CREATE OR REPLACE FUNCTION "
			 "		binary_upgrade.add_pg_enum_label(OID, OID, NAME) "
								  "RETURNS VOID "
								  "AS '$libdir/pg_upgrade_support' "
								  "LANGUAGE C STRICT;"));
		PQfinish(conn);
	}
	check_ok(ctx);
}


void
uninstall_support_functions(migratorContext *ctx)
{
	int			dbnum;

	prep_status(ctx, "Removing support functions from new cluster");

	for (dbnum = 0; dbnum < ctx->new.dbarr.ndbs; dbnum++)
	{
		DbInfo	   *newdb = &ctx->new.dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(ctx, newdb->db_name, CLUSTER_NEW);

		/* suppress NOTICE of dropped objects */
		PQclear(executeQueryOrDie(ctx, conn,
								  "SET client_min_messages = warning;"));
		PQclear(executeQueryOrDie(ctx, conn,
								  "DROP SCHEMA binary_upgrade CASCADE;"));
		PQclear(executeQueryOrDie(ctx, conn,
								  "RESET client_min_messages;"));
		PQfinish(conn);
	}
	check_ok(ctx);
}


/*
 * get_loadable_libraries()
 *
 *	Fetch the names of all old libraries containing C-language functions.
 *	We will later check that they all exist in the new installation.
 */
void
get_loadable_libraries(migratorContext *ctx)
{
	ClusterInfo *active_cluster = &ctx->old;
	PGresult  **ress;
	int			totaltups;
	int			dbnum;

	ress = (PGresult **)
		pg_malloc(ctx, active_cluster->dbarr.ndbs * sizeof(PGresult *));
	totaltups = 0;

	/* Fetch all library names, removing duplicates within each DB */
	for (dbnum = 0; dbnum < active_cluster->dbarr.ndbs; dbnum++)
	{
		DbInfo	   *active_db = &active_cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(ctx, active_db->db_name, CLUSTER_OLD);

		/* Fetch all libraries referenced in this DB */
		ress[dbnum] = executeQueryOrDie(ctx, conn,
										"SELECT DISTINCT probin "
										"FROM	pg_catalog.pg_proc "
										"WHERE	prolang = 13 /* C */ AND "
									 "		probin IS NOT NULL AND "
										"		oid >= %u;",
										FirstNormalObjectId);
		totaltups += PQntuples(ress[dbnum]);

		PQfinish(conn);
	}

	/* Allocate what's certainly enough space */
	if (totaltups > 0)
		ctx->libraries = (char **) pg_malloc(ctx, totaltups * sizeof(char *));
	else
		ctx->libraries = NULL;

	/*
	 * Now remove duplicates across DBs.  This is pretty inefficient code, but
	 * there probably aren't enough entries to matter.
	 */
	totaltups = 0;

	for (dbnum = 0; dbnum < active_cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res = ress[dbnum];
		int			ntups;
		int			rowno;

		ntups = PQntuples(res);
		for (rowno = 0; rowno < ntups; rowno++)
		{
			char	   *lib = PQgetvalue(res, rowno, 0);
			bool		dup = false;
			int			n;

			for (n = 0; n < totaltups; n++)
			{
				if (strcmp(lib, ctx->libraries[n]) == 0)
				{
					dup = true;
					break;
				}
			}
			if (!dup)
				ctx->libraries[totaltups++] = pg_strdup(ctx, lib);
		}

		PQclear(res);
	}

	ctx->num_libraries = totaltups;

	pg_free(ress);
}


/*
 * check_loadable_libraries()
 *
 *	Check that the new cluster contains all required libraries.
 *	We do this by actually trying to LOAD each one, thereby testing
 *	compatibility as well as presence.
 */
void
check_loadable_libraries(migratorContext *ctx)
{
	PGconn	   *conn = connectToServer(ctx, "template1", CLUSTER_NEW);
	int			libnum;
	FILE	   *script = NULL;
	bool		found = false;
	char		output_path[MAXPGPATH];

	prep_status(ctx, "Checking for presence of required libraries");

	snprintf(output_path, sizeof(output_path), "%s/loadable_libraries.txt",
			 ctx->cwd);

	for (libnum = 0; libnum < ctx->num_libraries; libnum++)
	{
		char	   *lib = ctx->libraries[libnum];
		int			llen = strlen(lib);
		char	   *cmd = (char *) pg_malloc(ctx, 8 + 2 * llen + 1);
		PGresult   *res;

		strcpy(cmd, "LOAD '");
		PQescapeStringConn(conn, cmd + 6, lib, llen, NULL);
		strcat(cmd, "'");

		res = PQexec(conn, cmd);

		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			found = true;
			if (script == NULL && (script = fopen(output_path, "w")) == NULL)
				pg_log(ctx, PG_FATAL, "Could not create necessary file:  %s\n",
					   output_path);
			fprintf(script, "Failed to load library: %s\n%s\n",
					lib,
					PQerrorMessage(conn));
		}

		PQclear(res);
		pg_free(cmd);
	}

	PQfinish(conn);

	if (found)
	{
		fclose(script);
		pg_log(ctx, PG_REPORT, "fatal\n");
		pg_log(ctx, PG_FATAL,
			 "| Your installation references loadable libraries that are missing\n"
			 "| from the new installation.  You can add these libraries to\n"
			   "| the new installation, or remove the functions using them\n"
			"| from the old installation.  A list of the problem libraries\n"
			   "| is in the file\n"
			   "| \"%s\".\n\n", output_path);
	}
	else
		check_ok(ctx);
}
