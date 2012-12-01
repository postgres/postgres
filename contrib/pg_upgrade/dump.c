/*
 *	dump.c
 *
 *	dump functions
 *
 *	Copyright (c) 2010-2012, PostgreSQL Global Development Group
 *	contrib/pg_upgrade/dump.c
 */

#include "postgres.h"

#include "pg_upgrade.h"

#include <sys/types.h>

void
generate_old_dump(void)
{
	int			dbnum;

	prep_status("Creating dump of global objects");

	/* run new pg_dumpall binary for globals */
	exec_prog(UTILITY_LOG_FILE, NULL, true,
			  "\"%s/pg_dumpall\" %s --schema-only --globals-only --binary-upgrade %s -f %s",
			  new_cluster.bindir, cluster_conn_opts(&old_cluster),
			  log_opts.verbose ? "--verbose" : "",
			  GLOBALS_DUMP_FILE);
	check_ok();

	prep_status("Creating dump of database schemas\n");

 	/* create per-db dump files */
	for (dbnum = 0; dbnum < old_cluster.dbarr.ndbs; dbnum++)
	{
		char 		file_name[MAXPGPATH];
		DbInfo     *old_db = &old_cluster.dbarr.dbs[dbnum];

		pg_log(PG_REPORT, OVERWRITE_MESSAGE, old_db->db_name);
		snprintf(file_name, sizeof(file_name), DB_DUMP_FILE_MASK, old_db->db_oid);

		exec_prog(RESTORE_LOG_FILE, NULL, true,
				  "\"%s/pg_dump\" %s --schema-only --binary-upgrade --format=custom %s --file=\"%s\" \"%s\"",
				  new_cluster.bindir, cluster_conn_opts(&old_cluster),
				  log_opts.verbose ? "--verbose" : "", file_name, old_db->db_name);
	}

	end_progress_output();
	check_ok();
}
