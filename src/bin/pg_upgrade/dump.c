/*
 *	dump.c
 *
 *	dump functions
 *
 *	Copyright (c) 2010-2016, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/dump.c
 */

#include "postgres_fe.h"

#include "pg_upgrade.h"

#include <sys/types.h>
#include "catalog/binary_upgrade.h"


void
generate_old_dump(void)
{
	int			dbnum;
	mode_t		old_umask;

	prep_status("Creating dump of global objects");

	/* run new pg_dumpall binary for globals */
	exec_prog(UTILITY_LOG_FILE, NULL, true,
			  "\"%s/pg_dumpall\" %s --globals-only --quote-all-identifiers "
			  "--binary-upgrade %s -f %s",
			  new_cluster.bindir, cluster_conn_opts(&old_cluster),
			  log_opts.verbose ? "--verbose" : "",
			  GLOBALS_DUMP_FILE);
	check_ok();

	prep_status("Creating dump of database schemas\n");

	/*
	 * Set umask for this function, all functions it calls, and all
	 * subprocesses/threads it creates.  We can't use fopen_priv() as Windows
	 * uses threads and umask is process-global.
	 */
	old_umask = umask(S_IRWXG | S_IRWXO);

	/* create per-db dump files */
	for (dbnum = 0; dbnum < old_cluster.dbarr.ndbs; dbnum++)
	{
		char		sql_file_name[MAXPGPATH],
					log_file_name[MAXPGPATH];
		DbInfo	   *old_db = &old_cluster.dbarr.dbs[dbnum];

		pg_log(PG_STATUS, "%s", old_db->db_name);
		snprintf(sql_file_name, sizeof(sql_file_name), DB_DUMP_FILE_MASK, old_db->db_oid);
		snprintf(log_file_name, sizeof(log_file_name), DB_DUMP_LOG_FILE_MASK, old_db->db_oid);

		parallel_exec_prog(log_file_name, NULL,
				   "\"%s/pg_dump\" %s --schema-only --quote-all-identifiers "
				  "--binary-upgrade --format=custom %s --file=\"%s\" \"%s\"",
						 new_cluster.bindir, cluster_conn_opts(&old_cluster),
						   log_opts.verbose ? "--verbose" : "",
						   sql_file_name, old_db->db_name);
	}

	/* reap all children */
	while (reap_child(true) == true)
		;

	umask(old_umask);

	end_progress_output();
	check_ok();
}


/*
 * It is possible for there to be a mismatch in the need for TOAST tables
 * between the old and new servers, e.g. some pre-9.1 tables didn't need
 * TOAST tables but will need them in 9.1+.  (There are also opposite cases,
 * but these are handled by setting binary_upgrade_next_toast_pg_class_oid.)
 *
 * We can't allow the TOAST table to be created by pg_dump with a
 * pg_dump-assigned oid because it might conflict with a later table that
 * uses that oid, causing a "file exists" error for pg_class conflicts, and
 * a "duplicate oid" error for pg_type conflicts.  (TOAST tables need pg_type
 * entries.)
 *
 * Therefore, a backend in binary-upgrade mode will not create a TOAST
 * table unless an OID as passed in via pg_upgrade_support functions.
 * This function is called after the restore and uses ALTER TABLE to
 * auto-create any needed TOAST tables which will not conflict with
 * restored oids.
 */
void
optionally_create_toast_tables(void)
{
	int			dbnum;

	prep_status("Creating newly-required TOAST tables");

	for (dbnum = 0; dbnum < new_cluster.dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		int			ntups;
		int			rowno;
		int			i_nspname,
					i_relname;
		DbInfo	   *active_db = &new_cluster.dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(&new_cluster, active_db->db_name);

		res = executeQueryOrDie(conn,
								"SELECT n.nspname, c.relname "
								"FROM	pg_catalog.pg_class c, "
								"		pg_catalog.pg_namespace n "
								"WHERE	c.relnamespace = n.oid AND "
								"		n.nspname NOT IN ('pg_catalog', 'information_schema') AND "
								"c.relkind IN ('r', 'm') AND "
								"c.reltoastrelid = 0");

		ntups = PQntuples(res);
		i_nspname = PQfnumber(res, "nspname");
		i_relname = PQfnumber(res, "relname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			/* enable auto-oid-numbered TOAST creation if needed */
			PQclear(executeQueryOrDie(conn, "SELECT pg_catalog.binary_upgrade_set_next_toast_pg_class_oid('%d'::pg_catalog.oid);",
									  OPTIONALLY_CREATE_TOAST_OID));

			/* dummy command that also triggers check for required TOAST table */
			PQclear(executeQueryOrDie(conn, "ALTER TABLE %s.%s RESET (binary_upgrade_dummy_option);",
						 quote_identifier(PQgetvalue(res, rowno, i_nspname)),
					   quote_identifier(PQgetvalue(res, rowno, i_relname))));
		}

		PQclear(res);

		PQfinish(conn);
	}

	check_ok();
}
