/*
 *	dump.c
 *
 *	dump functions
 *
 *	Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/dump.c
 */

#include "postgres_fe.h"

#include "fe_utils/string_utils.h"
#include "pg_upgrade.h"

void
generate_old_dump(void)
{
	int			dbnum;

	prep_status("Creating dump of global objects");

	/* run new pg_dumpall binary for globals */
	exec_prog(UTILITY_LOG_FILE, NULL, true, true,
			  "\"%s/pg_dumpall\" %s --globals-only --quote-all-identifiers "
			  "--binary-upgrade %s --no-sync -f \"%s/%s\"",
			  new_cluster.bindir, cluster_conn_opts(&old_cluster),
			  log_opts.verbose ? "--verbose" : "",
			  log_opts.dumpdir,
			  GLOBALS_DUMP_FILE);
	check_ok();

	prep_status_progress("Creating dump of database schemas");

	/* create per-db dump files */
	for (dbnum = 0; dbnum < old_cluster.dbarr.ndbs; dbnum++)
	{
		char		sql_file_name[MAXPGPATH],
					log_file_name[MAXPGPATH];
		DbInfo	   *old_db = &old_cluster.dbarr.dbs[dbnum];
		PQExpBufferData connstr,
					escaped_connstr;

		initPQExpBuffer(&connstr);
		appendPQExpBufferStr(&connstr, "dbname=");
		appendConnStrVal(&connstr, old_db->db_name);
		initPQExpBuffer(&escaped_connstr);
		appendShellString(&escaped_connstr, connstr.data);
		termPQExpBuffer(&connstr);

		pg_log(PG_STATUS, "%s", old_db->db_name);
		snprintf(sql_file_name, sizeof(sql_file_name), DB_DUMP_FILE_MASK, old_db->db_oid);
		snprintf(log_file_name, sizeof(log_file_name), DB_DUMP_LOG_FILE_MASK, old_db->db_oid);

		parallel_exec_prog(log_file_name, NULL,
						   "\"%s/pg_dump\" %s --no-data %s --quote-all-identifiers "
						   "--binary-upgrade --format=custom %s --no-sync --file=\"%s/%s\" %s",
						   new_cluster.bindir, cluster_conn_opts(&old_cluster),
						   log_opts.verbose ? "--verbose" : "",
						   user_opts.do_statistics ? "" : "--no-statistics",
						   log_opts.dumpdir,
						   sql_file_name, escaped_connstr.data);

		termPQExpBuffer(&escaped_connstr);
	}

	/* reap all children */
	while (reap_child(true) == true)
		;

	end_progress_output();
	check_ok();
}
