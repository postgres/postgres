/*
 *	function.c
 *
 *	server-side function support
 *
 *	Copyright (c) 2010-2016, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/function.c
 */

#include "postgres_fe.h"

#include "pg_upgrade.h"

#include "access/transam.h"
#include "catalog/pg_language.h"


/*
 * qsort comparator for pointers to library names
 *
 * We sort first by name length, then alphabetically for names of the same
 * length.  This is to ensure that, eg, "hstore_plpython" sorts after both
 * "hstore" and "plpython"; otherwise transform modules will probably fail
 * their LOAD tests.  (The backend ought to cope with that consideration,
 * but it doesn't yet, and even when it does it'll still be a good idea
 * to have a predictable order of probing here.)
 */
static int
library_name_compare(const void *p1, const void *p2)
{
	const char *str1 = *(const char *const *) p1;
	const char *str2 = *(const char *const *) p2;
	int			slen1 = strlen(str1);
	int			slen2 = strlen(str2);

	if (slen1 != slen2)
		return slen1 - slen2;
	return strcmp(str1, str2);
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
		 * Fetch all libraries containing non-built-in C functions in this DB.
		 */
		ress[dbnum] = executeQueryOrDie(conn,
										"SELECT DISTINCT probin "
										"FROM pg_catalog.pg_proc "
										"WHERE prolang = %u AND "
										"probin IS NOT NULL AND "
										"oid >= %u;",
										ClanguageId,
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
									"FROM pg_catalog.pg_proc p "
									"    JOIN pg_catalog.pg_namespace n "
									"    ON pronamespace = n.oid "
							   "WHERE proname = 'plpython_call_handler' AND "
									"nspname = 'public' AND "
									"prolang = %u AND "
									"probin = '$libdir/plpython' AND "
									"p.oid >= %u;",
									ClanguageId,
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

	/*
	 * Now we want to remove duplicates across DBs and sort the library names
	 * into order.  This avoids multiple probes of the same library, and
	 * ensures that libraries are probed in a consistent order, which is
	 * important for reproducible behavior if one library depends on another.
	 *
	 * First transfer all the names into one array, then sort, then remove
	 * duplicates.  Note: we strdup each name in the first loop so that we can
	 * safely clear the PGresults in the same loop.  This is a bit wasteful
	 * but it's unlikely there are enough names to matter.
	 */
	os_info.libraries = (char **) pg_malloc(totaltups * sizeof(char *));
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

			os_info.libraries[totaltups++] = pg_strdup(lib);
		}
		PQclear(res);
	}

	pg_free(ress);

	if (totaltups > 1)
	{
		int			i,
					lastnondup;

		qsort((void *) os_info.libraries, totaltups, sizeof(char *),
			  library_name_compare);

		for (i = 1, lastnondup = 0; i < totaltups; i++)
		{
			if (strcmp(os_info.libraries[i],
					   os_info.libraries[lastnondup]) != 0)
				os_info.libraries[++lastnondup] = os_info.libraries[i];
			else
				pg_free(os_info.libraries[i]);
		}
		totaltups = lastnondup + 1;
	}

	os_info.num_libraries = totaltups;
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

			if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
				pg_fatal("could not open file \"%s\": %s\n",
						 output_path, strerror(errno));
			fprintf(script, "could not load library \"%s\":\n%s\n",
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
