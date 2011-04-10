/*
 *	function.c
 *
 *	server-side function support
 *
 *	Copyright (c) 2010-2011, PostgreSQL Global Development Group
 *	contrib/pg_upgrade/function.c
 */

#include "pg_upgrade.h"

#include "access/transam.h"


/*
 * install_support_functions_in_new_db()
 *
 * pg_upgrade requires some support functions that enable it to modify
 * backend behavior.
 */
void
install_support_functions_in_new_db(const char *db_name)
{
	PGconn	   *conn = connectToServer(&new_cluster, db_name);

	/* suppress NOTICE of dropped objects */
	PQclear(executeQueryOrDie(conn,
							  "SET client_min_messages = warning;"));
	PQclear(executeQueryOrDie(conn,
						   "DROP SCHEMA IF EXISTS binary_upgrade CASCADE;"));
	PQclear(executeQueryOrDie(conn,
							  "RESET client_min_messages;"));

	PQclear(executeQueryOrDie(conn,
							  "CREATE SCHEMA binary_upgrade;"));

	PQclear(executeQueryOrDie(conn,
							  "CREATE OR REPLACE FUNCTION "
							  "binary_upgrade.set_next_pg_type_oid(OID) "
							  "RETURNS VOID "
							  "AS '$libdir/pg_upgrade_support' "
							  "LANGUAGE C STRICT;"));
	PQclear(executeQueryOrDie(conn,
							  "CREATE OR REPLACE FUNCTION "
							"binary_upgrade.set_next_array_pg_type_oid(OID) "
							  "RETURNS VOID "
							  "AS '$libdir/pg_upgrade_support' "
							  "LANGUAGE C STRICT;"));
	PQclear(executeQueryOrDie(conn,
							  "CREATE OR REPLACE FUNCTION "
							"binary_upgrade.set_next_toast_pg_type_oid(OID) "
							  "RETURNS VOID "
							  "AS '$libdir/pg_upgrade_support' "
							  "LANGUAGE C STRICT;"));
	PQclear(executeQueryOrDie(conn,
							  "CREATE OR REPLACE FUNCTION "
							"binary_upgrade.set_next_heap_pg_class_oid(OID) "
							  "RETURNS VOID "
							  "AS '$libdir/pg_upgrade_support' "
							  "LANGUAGE C STRICT;"));
	PQclear(executeQueryOrDie(conn,
							  "CREATE OR REPLACE FUNCTION "
						   "binary_upgrade.set_next_index_pg_class_oid(OID) "
							  "RETURNS VOID "
							  "AS '$libdir/pg_upgrade_support' "
							  "LANGUAGE C STRICT;"));
	PQclear(executeQueryOrDie(conn,
							  "CREATE OR REPLACE FUNCTION "
						   "binary_upgrade.set_next_toast_pg_class_oid(OID) "
							  "RETURNS VOID "
							  "AS '$libdir/pg_upgrade_support' "
							  "LANGUAGE C STRICT;"));
	PQclear(executeQueryOrDie(conn,
							  "CREATE OR REPLACE FUNCTION "
							  "binary_upgrade.set_next_pg_enum_oid(OID) "
							  "RETURNS VOID "
							  "AS '$libdir/pg_upgrade_support' "
							  "LANGUAGE C STRICT;"));
	PQclear(executeQueryOrDie(conn,
							  "CREATE OR REPLACE FUNCTION "
							  "binary_upgrade.set_next_pg_authid_oid(OID) "
							  "RETURNS VOID "
							  "AS '$libdir/pg_upgrade_support' "
							  "LANGUAGE C STRICT;"));
	PQclear(executeQueryOrDie(conn,
							  "CREATE OR REPLACE FUNCTION "
							  "binary_upgrade.create_empty_extension(text, text, bool, text, oid[], text[], text[]) "
							  "RETURNS VOID "
							  "AS '$libdir/pg_upgrade_support' "
							  "LANGUAGE C;"));
	PQfinish(conn);
}


void
uninstall_support_functions_from_new_cluster(void)
{
	int			dbnum;

	prep_status("Removing support functions from new cluster");

	for (dbnum = 0; dbnum < new_cluster.dbarr.ndbs; dbnum++)
	{
		DbInfo	   *new_db = &new_cluster.dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(&new_cluster, new_db->db_name);

		/* suppress NOTICE of dropped objects */
		PQclear(executeQueryOrDie(conn,
								  "SET client_min_messages = warning;"));
		PQclear(executeQueryOrDie(conn,
								  "DROP SCHEMA binary_upgrade CASCADE;"));
		PQclear(executeQueryOrDie(conn,
								  "RESET client_min_messages;"));
		PQfinish(conn);
	}
	check_ok();
}


/*
 * get_loadable_libraries()
 *
 *	Fetch the names of all old libraries containing C-language functions.
 *	We will later check that they all exist in the new installation.
 */
void
get_loadable_libraries(void)
{
	PGresult  **ress;
	int			totaltups;
	int			dbnum;

	ress = (PGresult **)
		pg_malloc(old_cluster.dbarr.ndbs * sizeof(PGresult *));
	totaltups = 0;

	/* Fetch all library names, removing duplicates within each DB */
	for (dbnum = 0; dbnum < old_cluster.dbarr.ndbs; dbnum++)
	{
		DbInfo	   *active_db = &old_cluster.dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(&old_cluster, active_db->db_name);

		/* Fetch all libraries referenced in this DB */
		ress[dbnum] = executeQueryOrDie(conn,
										"SELECT DISTINCT probin "
										"FROM	pg_catalog.pg_proc "
										"WHERE	prolang = 13 /* C */ AND "
										"probin IS NOT NULL AND "
										"oid >= %u;",
										FirstNormalObjectId);
		totaltups += PQntuples(ress[dbnum]);

		PQfinish(conn);
	}

	/* Allocate what's certainly enough space */
	if (totaltups > 0)
		os_info.libraries = (char **) pg_malloc(totaltups * sizeof(char *));
	else
		os_info.libraries = NULL;

	/*
	 * Now remove duplicates across DBs.  This is pretty inefficient code, but
	 * there probably aren't enough entries to matter.
	 */
	totaltups = 0;

	for (dbnum = 0; dbnum < old_cluster.dbarr.ndbs; dbnum++)
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
				if (strcmp(lib, os_info.libraries[n]) == 0)
				{
					dup = true;
					break;
				}
			}
			if (!dup)
				os_info.libraries[totaltups++] = pg_strdup(lib);
		}

		PQclear(res);
	}

	os_info.num_libraries = totaltups;

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
check_loadable_libraries(void)
{
	PGconn	   *conn = connectToServer(&new_cluster, "template1");
	int			libnum;
	FILE	   *script = NULL;
	bool		found = false;
	char		output_path[MAXPGPATH];

	prep_status("Checking for presence of required libraries");

	snprintf(output_path, sizeof(output_path), "%s/loadable_libraries.txt",
			 os_info.cwd);

	for (libnum = 0; libnum < os_info.num_libraries; libnum++)
	{
		char	   *lib = os_info.libraries[libnum];
		int			llen = strlen(lib);
		char	   *cmd = (char *) pg_malloc(8 + 2 * llen + 1);
		PGresult   *res;

		strcpy(cmd, "LOAD '");
		PQescapeStringConn(conn, cmd + 6, lib, llen, NULL);
		strcat(cmd, "'");

		res = PQexec(conn, cmd);

		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			found = true;
			if (script == NULL && (script = fopen(output_path, "w")) == NULL)
				pg_log(PG_FATAL, "Could not create necessary file:  %s\n",
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
		pg_log(PG_REPORT, "fatal\n");
		pg_log(PG_FATAL,
		"| Your installation references loadable libraries that are missing\n"
			 "| from the new installation.  You can add these libraries to\n"
			   "| the new installation, or remove the functions using them\n"
			"| from the old installation.  A list of the problem libraries\n"
			   "| is in the file\n"
			   "| \"%s\".\n\n", output_path);
	}
	else
		check_ok();
}
