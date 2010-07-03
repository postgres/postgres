/*
 *	version.c
 *
 *	Postgres-version-specific routines
 *
 *	Copyright (c) 2010, PostgreSQL Global Development Group
 *	$PostgreSQL: pgsql/contrib/pg_upgrade/version.c,v 1.5 2010/07/03 16:33:14 momjian Exp $
 */

#include "pg_upgrade.h"

#include "access/transam.h"


/*
 * new_9_0_populate_pg_largeobject_metadata()
 *	new >= 9.0, old <= 8.4
 *	9.0 has a new pg_largeobject permission table
 */
void
new_9_0_populate_pg_largeobject_metadata(migratorContext *ctx, bool check_mode,
										 Cluster whichCluster)
{
	ClusterInfo *active_cluster = (whichCluster == CLUSTER_OLD) ?
	&ctx->old : &ctx->new;
	int			dbnum;
	FILE	   *script = NULL;
	bool		found = false;
	char		output_path[MAXPGPATH];

	prep_status(ctx, "Checking for large objects");

	snprintf(output_path, sizeof(output_path), "%s/pg_largeobject.sql",
			 ctx->cwd);

	for (dbnum = 0; dbnum < active_cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		int			i_count;
		DbInfo	   *active_db = &active_cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(ctx, active_db->db_name, whichCluster);

		/* find if there are any large objects */
		res = executeQueryOrDie(ctx, conn,
								"SELECT count(*) "
								"FROM	pg_catalog.pg_largeobject ");

		i_count = PQfnumber(res, "count");
		if (atoi(PQgetvalue(res, 0, i_count)) != 0)
		{
			found = true;
			if (!check_mode)
			{
				if (script == NULL && (script = fopen(output_path, "w")) == NULL)
					pg_log(ctx, PG_FATAL, "Could not create necessary file:  %s\n", output_path);
				fprintf(script, "\\connect %s\n",
						quote_identifier(ctx, active_db->db_name));
				fprintf(script,
						"SELECT pg_catalog.lo_create(t.loid)\n"
						"FROM (SELECT DISTINCT loid FROM pg_catalog.pg_largeobject) AS t;\n");
			}
		}

		PQclear(res);
		PQfinish(conn);
	}

	if (found)
	{
		if (!check_mode)
			fclose(script);
		report_status(ctx, PG_WARNING, "warning");
		if (check_mode)
			pg_log(ctx, PG_WARNING, "\n"
				   "| Your installation contains large objects.\n"
				   "| The new database has an additional large object\n"
				   "| permission table.  After migration, you will be\n"
				   "| given a command to populate the pg_largeobject\n"
				   "| permission table with default permissions.\n\n");
		else
			pg_log(ctx, PG_WARNING, "\n"
				   "| Your installation contains large objects.\n"
				   "| The new database has an additional large object\n"
				   "| permission table so default permissions must be\n"
				   "| defined for all large objects.  The file:\n"
				   "| \t%s\n"
				   "| when executed by psql by the database super-user\n"
				   "| will define the default permissions.\n\n",
				   output_path);
	}
	else
		check_ok(ctx);
}
