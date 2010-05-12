/*
 *	tablespace.c
 *
 *	tablespace functions
 */

#include "pg_upgrade.h"

static void get_tablespace_paths(migratorContext *ctx);
static void set_tablespace_directory_suffix(migratorContext *ctx,
							Cluster whichCluster);


void
init_tablespaces(migratorContext *ctx)
{
	get_tablespace_paths(ctx);

	set_tablespace_directory_suffix(ctx, CLUSTER_OLD);
	set_tablespace_directory_suffix(ctx, CLUSTER_NEW);

	if (ctx->num_tablespaces > 0 &&
		strcmp(ctx->old.tablespace_suffix, ctx->new.tablespace_suffix) == 0)
		pg_log(ctx, PG_FATAL,
			   "Cannot migrate to/from the same system catalog version when\n"
			   "using tablespaces.\n");
}


/*
 * get_tablespace_paths()
 *
 * Scans pg_tablespace and returns a malloc'ed array of all tablespace
 * paths. Its the caller's responsibility to free the array.
 */
static void
get_tablespace_paths(migratorContext *ctx)
{
	PGconn	   *conn = connectToServer(ctx, "template1", CLUSTER_OLD);
	PGresult   *res;
	int			ntups;
	int			tblnum;
	int			i_spclocation;

	res = executeQueryOrDie(ctx, conn,
							"SELECT	spclocation "
							"FROM	pg_catalog.pg_tablespace "
							"WHERE	spcname != 'pg_default' AND "
							"		spcname != 'pg_global'");

	ctx->num_tablespaces = ntups = PQntuples(res);
	ctx->tablespaces = (char **) pg_malloc(ctx, ntups * sizeof(char *));

	i_spclocation = PQfnumber(res, "spclocation");

	for (tblnum = 0; tblnum < ntups; tblnum++)
		ctx->tablespaces[tblnum] = pg_strdup(ctx,
									 PQgetvalue(res, tblnum, i_spclocation));

	PQclear(res);

	PQfinish(conn);

	return;
}


static void
set_tablespace_directory_suffix(migratorContext *ctx, Cluster whichCluster)
{
	ClusterInfo *cluster = (whichCluster == CLUSTER_OLD) ? &ctx->old : &ctx->new;

	if (GET_MAJOR_VERSION(cluster->major_version) <= 804)
		cluster->tablespace_suffix = pg_strdup(ctx, "");
	else
	{
		/* This cluster has a version-specific subdirectory */
		cluster->tablespace_suffix = pg_malloc(ctx, 4 + strlen(cluster->major_version_str) +
											   10 /* OIDCHARS */ + 1);

		/* The leading slash is needed to start a new directory. */
		sprintf(cluster->tablespace_suffix, "/PG_%s_%d", cluster->major_version_str,
				cluster->controldata.cat_ver);
	}
}
