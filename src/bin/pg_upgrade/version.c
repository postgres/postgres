/*
 *	version.c
 *
 *	Postgres-version-specific routines
 *
 *	Copyright (c) 2010-2016, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/version.c
 */

#include "postgres_fe.h"

#include "pg_upgrade.h"
#include "fe_utils/string_utils.h"



/*
 * new_9_0_populate_pg_largeobject_metadata()
 *	new >= 9.0, old <= 8.4
 *	9.0 has a new pg_largeobject permission table
 */
void
new_9_0_populate_pg_largeobject_metadata(ClusterInfo *cluster, bool check_mode)
{
	int			dbnum;
	FILE	   *script = NULL;
	bool		found = false;
	char		output_path[MAXPGPATH];

	prep_status("Checking for large objects");

	snprintf(output_path, sizeof(output_path), "pg_largeobject.sql");

	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		int			i_count;
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);

		/* find if there are any large objects */
		res = executeQueryOrDie(conn,
								"SELECT count(*) "
								"FROM	pg_catalog.pg_largeobject ");

		i_count = PQfnumber(res, "count");
		if (atoi(PQgetvalue(res, 0, i_count)) != 0)
		{
			found = true;
			if (!check_mode)
			{
				PQExpBufferData connectbuf;

				if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
					pg_fatal("could not open file \"%s\": %s\n", output_path,
							 strerror(errno));

				initPQExpBuffer(&connectbuf);
				appendPsqlMetaConnect(&connectbuf, active_db->db_name);
				fputs(connectbuf.data, script);
				termPQExpBuffer(&connectbuf);

				fprintf(script,
						"SELECT pg_catalog.lo_create(t.loid)\n"
						"FROM (SELECT DISTINCT loid FROM pg_catalog.pg_largeobject) AS t;\n");
			}
		}

		PQclear(res);
		PQfinish(conn);
	}

	if (script)
		fclose(script);

	if (found)
	{
		report_status(PG_WARNING, "warning");
		if (check_mode)
			pg_log(PG_WARNING, "\n"
				   "Your installation contains large objects.  The new database has an\n"
				   "additional large object permission table.  After upgrading, you will be\n"
				   "given a command to populate the pg_largeobject permission table with\n"
				   "default permissions.\n\n");
		else
			pg_log(PG_WARNING, "\n"
				   "Your installation contains large objects.  The new database has an\n"
				   "additional large object permission table, so default permissions must be\n"
				   "defined for all large objects.  The file\n"
				   "    %s\n"
				   "when executed by psql by the database superuser will set the default\n"
				   "permissions.\n\n",
				   output_path);
	}
	else
		check_ok();
}


/*
 * old_9_3_check_for_line_data_type_usage()
 *	9.3 -> 9.4
 *	Fully implement the 'line' data type in 9.4, which previously returned
 *	"not enabled" by default and was only functionally enabled with a
 *	compile-time switch;  9.4 "line" has different binary and text
 *	representation formats;  checks tables and indexes.
 */
void
old_9_3_check_for_line_data_type_usage(ClusterInfo *cluster)
{
	int			dbnum;
	FILE	   *script = NULL;
	bool		found = false;
	char		output_path[MAXPGPATH];

	prep_status("Checking for invalid \"line\" user columns");

	snprintf(output_path, sizeof(output_path), "tables_using_line.txt");

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
		 * The pg_catalog.line type may be wrapped in a domain or composite
		 * type, or both (9.3 did not allow domains on composite types, but
		 * there may be multi-level composite type). To detect these cases
		 * we need a recursive CTE.
		 */
		res = executeQueryOrDie(conn,
								"WITH RECURSIVE oids AS ( "
		/* the pg_catalog.line type itself */
								"	SELECT 'pg_catalog.line'::pg_catalog.regtype AS oid "
								"	UNION ALL "
								"	SELECT * FROM ( "
		/* domains on the type */
								"		WITH x AS (SELECT oid FROM oids) "
								"			SELECT t.oid FROM pg_catalog.pg_type t, x WHERE typbasetype = x.oid AND typtype = 'd' "
								"			UNION "
		/* composite types containing the type */
								"			SELECT t.oid FROM pg_catalog.pg_type t, pg_catalog.pg_class c, pg_catalog.pg_attribute a, x "
								"			WHERE t.typtype = 'c' AND "
								"				  t.oid = c.reltype AND "
								"				  c.oid = a.attrelid AND "
								"				  NOT a.attisdropped AND "
								"				  a.atttypid = x.oid "
								"	) foo "
								") "
								"SELECT n.nspname, c.relname, a.attname "
								"FROM	pg_catalog.pg_class c, "
								"		pg_catalog.pg_namespace n, "
								"		pg_catalog.pg_attribute a "
								"WHERE	c.oid = a.attrelid AND "
								"		NOT a.attisdropped AND "
								"		a.atttypid IN (SELECT oid FROM oids) AND "
								"		c.relkind IN ("
								CppAsString2(RELKIND_RELATION) ", "
								CppAsString2(RELKIND_MATVIEW) ", "
								CppAsString2(RELKIND_INDEX) ") AND "
								"		c.relnamespace = n.oid AND "
		/* exclude possible orphaned temp tables */
								"		n.nspname !~ '^pg_temp_' AND "
						 "		n.nspname !~ '^pg_toast_temp_' AND "
								"		n.nspname NOT IN ('pg_catalog', 'information_schema')");

		ntups = PQntuples(res);
		i_nspname = PQfnumber(res, "nspname");
		i_relname = PQfnumber(res, "relname");
		i_attname = PQfnumber(res, "attname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			found = true;
			if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
				pg_fatal("could not open file \"%s\": %s\n", output_path,
						 strerror(errno));
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
		pg_fatal("Your installation contains the \"line\" data type in user tables.  This\n"
				 "data type changed its internal and input/output format between your old\n"
				 "and new clusters so this cluster cannot currently be upgraded.  You can\n"
				 "remove the problem tables and restart the upgrade.  A list of the problem\n"
				 "columns is in the file:\n"
				 "    %s\n\n", output_path);
	}
	else
		check_ok();
}
