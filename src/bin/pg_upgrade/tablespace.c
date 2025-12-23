/*
 *	tablespace.c
 *
 *	tablespace functions
 *
 *	Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/tablespace.c
 */

#include "postgres_fe.h"

#include "pg_upgrade.h"

static void get_tablespace_paths(void);
static void set_tablespace_directory_suffix(ClusterInfo *cluster);


void
init_tablespaces(void)
{
	get_tablespace_paths();

	set_tablespace_directory_suffix(&old_cluster);
	set_tablespace_directory_suffix(&new_cluster);

	if (old_cluster.num_tablespaces > 0 &&
		strcmp(old_cluster.tablespace_suffix, new_cluster.tablespace_suffix) == 0)
	{
		for (int i = 0; i < old_cluster.num_tablespaces; i++)
		{
			/*
			 * In-place tablespaces are okay for same-version upgrades because
			 * their paths will differ between clusters.
			 */
			if (strcmp(old_cluster.tablespaces[i], new_cluster.tablespaces[i]) == 0)
				pg_fatal("Cannot upgrade to/from the same system catalog version when\n"
						 "using tablespaces.");
		}
	}
}


/*
 * get_tablespace_paths()
 *
 * Scans pg_tablespace and returns a malloc'ed array of all tablespace
 * paths. It's the caller's responsibility to free the array.
 */
static void
get_tablespace_paths(void)
{
	PGconn	   *conn = connectToServer(&old_cluster, "template1");
	PGresult   *res;
	int			tblnum;
	int			i_spclocation;
	char		query[QUERY_ALLOC];

	snprintf(query, sizeof(query),
			 "SELECT pg_catalog.pg_tablespace_location(oid) AS spclocation "
			 "FROM	pg_catalog.pg_tablespace "
			 "WHERE	spcname != 'pg_default' AND "
			 "		spcname != 'pg_global'");

	res = executeQueryOrDie(conn, "%s", query);

	old_cluster.num_tablespaces = PQntuples(res);
	new_cluster.num_tablespaces = PQntuples(res);

	if (PQntuples(res) != 0)
	{
		old_cluster.tablespaces =
			(char **) pg_malloc(old_cluster.num_tablespaces * sizeof(char *));
		new_cluster.tablespaces =
			(char **) pg_malloc(new_cluster.num_tablespaces * sizeof(char *));
	}
	else
	{
		old_cluster.tablespaces = NULL;
		new_cluster.tablespaces = NULL;
	}

	i_spclocation = PQfnumber(res, "spclocation");

	for (tblnum = 0; tblnum < old_cluster.num_tablespaces; tblnum++)
	{
		struct stat statBuf;
		char	   *spcloc = PQgetvalue(res, tblnum, i_spclocation);

		/*
		 * For now, we do not expect non-in-place tablespaces to move during
		 * upgrade.  If that changes, it will likely become necessary to run
		 * the above query on the new cluster, too.
		 *
		 * pg_tablespace_location() returns absolute paths for non-in-place
		 * tablespaces and relative paths for in-place ones, so we use
		 * is_absolute_path() to distinguish between them.
		 */
		if (is_absolute_path(PQgetvalue(res, tblnum, i_spclocation)))
		{
			old_cluster.tablespaces[tblnum] = pg_strdup(spcloc);
			new_cluster.tablespaces[tblnum] = old_cluster.tablespaces[tblnum];
		}
		else
		{
			old_cluster.tablespaces[tblnum] = psprintf("%s/%s", old_cluster.pgdata, spcloc);
			new_cluster.tablespaces[tblnum] = psprintf("%s/%s", new_cluster.pgdata, spcloc);
		}

		/*
		 * Check that the tablespace path exists and is a directory.
		 * Effectively, this is checking only for tables/indexes in
		 * non-existent tablespace directories.  Databases located in
		 * non-existent tablespaces already throw a backend error.
		 * Non-existent tablespace directories can occur when a data directory
		 * that contains user tablespaces is moved as part of pg_upgrade
		 * preparation and the symbolic links are not updated.
		 */
		if (stat(old_cluster.tablespaces[tblnum], &statBuf) != 0)
		{
			if (errno == ENOENT)
				report_status(PG_FATAL,
							  "tablespace directory \"%s\" does not exist",
							  old_cluster.tablespaces[tblnum]);
			else
				report_status(PG_FATAL,
							  "could not stat tablespace directory \"%s\": %m",
							  old_cluster.tablespaces[tblnum]);
		}
		if (!S_ISDIR(statBuf.st_mode))
			report_status(PG_FATAL,
						  "tablespace path \"%s\" is not a directory",
						  old_cluster.tablespaces[tblnum]);
	}

	PQclear(res);

	PQfinish(conn);
}


static void
set_tablespace_directory_suffix(ClusterInfo *cluster)
{
	/* This cluster has a version-specific subdirectory */

	/* The leading slash is needed to start a new directory. */
	cluster->tablespace_suffix = psprintf("/PG_%s_%d",
										  cluster->major_version_str,
										  cluster->controldata.cat_ver);
}
