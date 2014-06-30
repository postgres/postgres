/*
 *	function.c
 *
 *	server-side function support
 *
 *	Copyright (c) 2010-2014, PostgreSQL Global Development Group
 *	contrib/pg_upgrade/function.c
 */

#include "postgres_fe.h"

#include "pg_upgrade.h"

#include "access/transam.h"

#define PG_UPGRADE_SUPPORT	"$libdir/pg_upgrade_support"

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
	bool		found_public_plpython_handler = false;

	ress = (PGresult **) pg_malloc(old_cluster.dbarr.ndbs * sizeof(PGresult *));
	totaltups = 0;

	/* Fetch all library names, removing duplicates within each DB */
	for (dbnum = 0; dbnum < old_cluster.dbarr.ndbs; dbnum++)
	{
		DbInfo	   *active_db = &old_cluster.dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(&old_cluster, active_db->db_name);

		/*
		 * Fetch all libraries referenced in this DB.  We can't exclude the
		 * "pg_catalog" schema because, while such functions are not
		 * explicitly dumped by pg_dump, they do reference implicit objects
		 * that pg_dump does dump, e.g. CREATE LANGUAGE plperl.
		 */
		ress[dbnum] = executeQueryOrDie(conn,
										"SELECT DISTINCT probin "
										"FROM	pg_catalog.pg_proc "
										"WHERE	prolang = 13 /* C */ AND "
										"probin IS NOT NULL AND "
										"oid >= %u;",
										FirstNormalObjectId);
		totaltups += PQntuples(ress[dbnum]);

		/*
		 * Systems that install plpython before 8.1 have
		 * plpython_call_handler() defined in the "public" schema, causing
		 * pg_dump to dump it.  However that function still references
		 * "plpython" (no "2"), so it throws an error on restore.  This code
		 * checks for the problem function, reports affected databases to the
		 * user and explains how to remove them. 8.1 git commit:
		 * e0dedd0559f005d60c69c9772163e69c204bac69
		 * http://archives.postgresql.org/pgsql-hackers/2012-03/msg01101.php
		 * http://archives.postgresql.org/pgsql-bugs/2012-05/msg00206.php
		 */
		if (GET_MAJOR_VERSION(old_cluster.major_version) < 901)
		{
			PGresult   *res;

			res = executeQueryOrDie(conn,
									"SELECT 1 "
						   "FROM	pg_catalog.pg_proc JOIN pg_namespace "
							 "		ON pronamespace = pg_namespace.oid "
							   "WHERE proname = 'plpython_call_handler' AND "
									"nspname = 'public' AND "
									"prolang = 13 /* C */ AND "
									"probin = '$libdir/plpython' AND "
									"pg_proc.oid >= %u;",
									FirstNormalObjectId);
			if (PQntuples(res) > 0)
			{
				if (!found_public_plpython_handler)
				{
					pg_log(PG_WARNING,
						   "\nThe old cluster has a \"plpython_call_handler\" function defined\n"
						   "in the \"public\" schema which is a duplicate of the one defined\n"
						   "in the \"pg_catalog\" schema.  You can confirm this by executing\n"
						   "in psql:\n"
						   "\n"
						   "    \\df *.plpython_call_handler\n"
						   "\n"
						   "The \"public\" schema version of this function was created by a\n"
						   "pre-8.1 install of plpython, and must be removed for pg_upgrade\n"
						   "to complete because it references a now-obsolete \"plpython\"\n"
						   "shared object file.  You can remove the \"public\" schema version\n"
					   "of this function by running the following command:\n"
						   "\n"
						 "    DROP FUNCTION public.plpython_call_handler()\n"
						   "\n"
						   "in each affected database:\n"
						   "\n");
				}
				pg_log(PG_WARNING, "    %s\n", active_db->db_name);
				found_public_plpython_handler = true;
			}
			PQclear(res);
		}

		PQfinish(conn);
	}

	if (found_public_plpython_handler)
		pg_fatal("Remove the problem functions from the old cluster to continue.\n");

	totaltups++;				/* reserve for pg_upgrade_support */

	/* Allocate what's certainly enough space */
	os_info.libraries = (char **) pg_malloc(totaltups * sizeof(char *));

	/*
	 * Now remove duplicates across DBs.  This is pretty inefficient code, but
	 * there probably aren't enough entries to matter.
	 */
	totaltups = 0;
	os_info.libraries[totaltups++] = pg_strdup(PG_UPGRADE_SUPPORT);

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

	snprintf(output_path, sizeof(output_path), "loadable_libraries.txt");

	for (libnum = 0; libnum < os_info.num_libraries; libnum++)
	{
		char	   *lib = os_info.libraries[libnum];
		int			llen = strlen(lib);
		char		cmd[7 + 2 * MAXPGPATH + 1];
		PGresult   *res;

		/*
		 * In Postgres 9.0, Python 3 support was added, and to do that, a
		 * plpython2u language was created with library name plpython2.so as a
		 * symbolic link to plpython.so.  In Postgres 9.1, only the
		 * plpython2.so library was created, and both plpythonu and plpython2u
		 * pointing to it.  For this reason, any reference to library name
		 * "plpython" in an old PG <= 9.1 cluster must look for "plpython2" in
		 * the new cluster.
		 *
		 * For this case, we could check pg_pltemplate, but that only works
		 * for languages, and does not help with function shared objects, so
		 * we just do a general fix.
		 */
		if (GET_MAJOR_VERSION(old_cluster.major_version) < 901 &&
			strcmp(lib, "$libdir/plpython") == 0)
		{
			lib = "$libdir/plpython2";
			llen = strlen(lib);
		}

		strcpy(cmd, "LOAD '");
		PQescapeStringConn(conn, cmd + strlen(cmd), lib, llen, NULL);
		strcat(cmd, "'");

		res = PQexec(conn, cmd);

		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			found = true;

			/* exit and report missing support library with special message */
			if (strcmp(lib, PG_UPGRADE_SUPPORT) == 0)
				pg_fatal("The pg_upgrade_support module must be created and installed in the new cluster.\n");

			if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
				pg_fatal("Could not open file \"%s\": %s\n",
						 output_path, getErrorText(errno));
			fprintf(script, "Could not load library \"%s\"\n%s\n",
					lib,
					PQerrorMessage(conn));
		}

		PQclear(res);
	}

	PQfinish(conn);

	if (found)
	{
		fclose(script);
		pg_log(PG_REPORT, "fatal\n");
		pg_fatal("Your installation references loadable libraries that are missing from the\n"
				 "new installation.  You can add these libraries to the new installation,\n"
				 "or remove the functions using them from the old installation.  A list of\n"
				 "problem libraries is in the file:\n"
				 "    %s\n\n", output_path);
	}
	else
		check_ok();
}
