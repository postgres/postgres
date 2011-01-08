/*
 *	info.c
 *
 *	information support functions
 *
 *	Copyright (c) 2010-2011, PostgreSQL Global Development Group
 *	contrib/pg_upgrade/info.c
 */

#include "pg_upgrade.h"

#include "access/transam.h"


static void get_db_infos(ClusterInfo *cluster);
static void print_db_arr(ClusterInfo *cluster);
static void print_rel_arr(RelInfoArr *arr);
static void get_rel_infos(ClusterInfo *cluster, DbInfo *dbinfo);
static void free_rel_arr(RelInfoArr *rel_arr);
static void create_rel_filename_map(const char *old_data, const char *new_data,
			  const DbInfo *old_db, const DbInfo *new_db,
			  const RelInfo *old_rel, const RelInfo *new_rel,
			  FileNameMap *map);


/*
 * gen_db_file_maps()
 *
 * generates database mappings for "old_db" and "new_db". Returns a malloc'ed
 * array of mappings. nmaps is a return parameter which refers to the number
 * mappings.
 */
FileNameMap *
gen_db_file_maps(DbInfo *old_db, DbInfo *new_db,
				 int *nmaps, const char *old_pgdata, const char *new_pgdata)
{
	FileNameMap *maps;
	int			relnum;
	int			num_maps = 0;

	if (old_db->rel_arr.nrels != new_db->rel_arr.nrels)
		pg_log(PG_FATAL, "old and new databases \"%s\" have a different number of relations\n",
					old_db->db_name);

	maps = (FileNameMap *) pg_malloc(sizeof(FileNameMap) *
									 old_db->rel_arr.nrels);

	for (relnum = 0; relnum < old_db->rel_arr.nrels; relnum++)
	{
		RelInfo    *old_rel = &old_db->rel_arr.rels[relnum];
		RelInfo    *new_rel = &old_db->rel_arr.rels[relnum];

		if (old_rel->reloid != new_rel->reloid)
			pg_log(PG_FATAL, "mismatch of relation id: database \"%s\", old relid %d, new relid %d\n",
			old_db->db_name, old_rel->reloid, new_rel->reloid);
	
		create_rel_filename_map(old_pgdata, new_pgdata, old_db, new_db,
				old_rel, new_rel, maps + num_maps);
		num_maps++;
	}

	*nmaps = num_maps;
	return maps;
}


/*
 * create_rel_filename_map()
 *
 * fills a file node map structure and returns it in "map".
 */
static void
create_rel_filename_map(const char *old_data, const char *new_data,
			  const DbInfo *old_db, const DbInfo *new_db,
			  const RelInfo *old_rel, const RelInfo *new_rel,
			  FileNameMap *map)
{
	if (strlen(old_rel->tablespace) == 0)
	{
		/*
		 * relation belongs to the default tablespace, hence relfiles should
		 * exist in the data directories.
		 */
		snprintf(map->old_dir, sizeof(map->old_dir), "%s/base/%u", old_data,
				 old_db->db_oid);
		snprintf(map->new_dir, sizeof(map->new_dir), "%s/base/%u", new_data,
				 new_db->db_oid);
	}
	else
	{
		/* relation belongs to a tablespace, so use the tablespace location */
		snprintf(map->old_dir, sizeof(map->old_dir), "%s%s/%u", old_rel->tablespace,
				 old_cluster.tablespace_suffix, old_db->db_oid);
		snprintf(map->new_dir, sizeof(map->new_dir), "%s%s/%u", new_rel->tablespace,
				 new_cluster.tablespace_suffix, new_db->db_oid);
	}

	/*
	 *	old_relfilenode might differ from pg_class.oid (and hence
	 *	new_relfilenode) because of CLUSTER, REINDEX, or VACUUM FULL.
	 */
	map->old_relfilenode = old_rel->relfilenode;

	/* new_relfilenode will match old and new pg_class.oid */
	map->new_relfilenode = new_rel->relfilenode;

	/* used only for logging and error reporing, old/new are identical */
	snprintf(map->nspname, sizeof(map->nspname), "%s", old_rel->nspname);
	snprintf(map->relname, sizeof(map->relname), "%s", old_rel->relname);
}


void
print_maps(FileNameMap *maps, int n, const char *dbName)
{
	if (log_opts.debug)
	{
		int			mapnum;

		pg_log(PG_DEBUG, "mappings for db %s:\n", dbName);

		for (mapnum = 0; mapnum < n; mapnum++)
			pg_log(PG_DEBUG, "%s.%s: %u to %u\n",
				   maps[mapnum].nspname, maps[mapnum].relname,
				   maps[mapnum].old_relfilenode,
				   maps[mapnum].new_relfilenode);

		pg_log(PG_DEBUG, "\n\n");
	}
}


/*
 * get_db_infos()
 *
 * Scans pg_database system catalog and populates all user
 * databases.
 */
static void
get_db_infos(ClusterInfo *cluster)
{
	PGconn	   *conn = connectToServer(cluster, "template1");
	PGresult   *res;
	int			ntups;
	int			tupnum;
	DbInfo	   *dbinfos;
	int			i_datname;
	int			i_oid;
	int			i_spclocation;

	res = executeQueryOrDie(conn,
							"SELECT d.oid, d.datname, t.spclocation "
							"FROM pg_catalog.pg_database d "
							" LEFT OUTER JOIN pg_catalog.pg_tablespace t "
							" ON d.dattablespace = t.oid "
							"WHERE d.datallowconn = true "
	/* we don't preserve pg_database.oid so we sort by name */
							"ORDER BY 2");

	i_datname = PQfnumber(res, "datname");
	i_oid = PQfnumber(res, "oid");
	i_spclocation = PQfnumber(res, "spclocation");

	ntups = PQntuples(res);
	dbinfos = (DbInfo *) pg_malloc(sizeof(DbInfo) * ntups);

	for (tupnum = 0; tupnum < ntups; tupnum++)
	{
		dbinfos[tupnum].db_oid = atooid(PQgetvalue(res, tupnum, i_oid));

		snprintf(dbinfos[tupnum].db_name, sizeof(dbinfos[tupnum].db_name), "%s",
				 PQgetvalue(res, tupnum, i_datname));
		snprintf(dbinfos[tupnum].db_tblspace, sizeof(dbinfos[tupnum].db_tblspace), "%s",
				 PQgetvalue(res, tupnum, i_spclocation));
	}
	PQclear(res);

	PQfinish(conn);

	cluster->dbarr.dbs = dbinfos;
	cluster->dbarr.ndbs = ntups;
}


/*
 * get_db_and_rel_infos()
 *
 * higher level routine to generate dbinfos for the database running
 * on the given "port". Assumes that server is already running.
 */
void
get_db_and_rel_infos(ClusterInfo *cluster)
{
	int			dbnum;

	get_db_infos(cluster);

	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
		get_rel_infos(cluster, &cluster->dbarr.dbs[dbnum]);

	if (log_opts.debug)
		print_db_arr(cluster);
}


/*
 * get_rel_infos()
 *
 * gets the relinfos for all the user tables of the database refered
 * by "db".
 *
 * NOTE: we assume that relations/entities with oids greater than
 * FirstNormalObjectId belongs to the user
 */
static void
get_rel_infos(ClusterInfo *cluster, DbInfo *dbinfo)
{
	PGconn	   *conn = connectToServer(cluster,
									   dbinfo->db_name);
	PGresult   *res;
	RelInfo    *relinfos;
	int			ntups;
	int			relnum;
	int			num_rels = 0;
	char	   *nspname = NULL;
	char	   *relname = NULL;
	int			i_spclocation = -1;
	int			i_nspname = -1;
	int			i_relname = -1;
	int			i_oid = -1;
	int			i_relfilenode = -1;
	int			i_reltoastrelid = -1;
	char		query[QUERY_ALLOC];

	/*
	 * pg_largeobject contains user data that does not appear in pg_dumpall
	 * --schema-only output, so we have to copy that system table heap and
	 * index.  Ideally we could just get the relfilenode from template1 but
	 * pg_largeobject_loid_pn_index's relfilenode can change if the table was
	 * reindexed so we get the relfilenode for each database and upgrade it as
	 * a normal user table.
	 * Order by tablespace so we can cache the directory contents efficiently.
	 */

	snprintf(query, sizeof(query),
			 "SELECT DISTINCT c.oid, n.nspname, c.relname, "
			 "	c.relfilenode, c.reltoastrelid, t.spclocation "
			 "FROM pg_catalog.pg_class c JOIN "
			 "		pg_catalog.pg_namespace n "
			 "	ON c.relnamespace = n.oid "
			 "   LEFT OUTER JOIN pg_catalog.pg_tablespace t "
			 "	ON c.reltablespace = t.oid "
			 "WHERE (( n.nspname NOT IN ('pg_catalog', 'information_schema', 'binary_upgrade') "
			 "	AND c.oid >= %u "
			 "	) OR ( "
			 "	n.nspname = 'pg_catalog' "
			 "	AND relname IN "
			 "        ('pg_largeobject', 'pg_largeobject_loid_pn_index'%s) )) "
			 "	AND relkind IN ('r','t', 'i'%s)"
			 "GROUP BY  c.oid, n.nspname, c.relname, c.relfilenode,"
			 "			c.reltoastrelid, t.spclocation, "
			 "			n.nspname "
	/* we preserve pg_class.oid so we sort by it to match old/new */
			 "ORDER BY 1;",
			 FirstNormalObjectId,
	/* does pg_largeobject_metadata need to be migrated? */
			 (GET_MAJOR_VERSION(old_cluster.major_version) <= 804) ?
			 "" : ", 'pg_largeobject_metadata', 'pg_largeobject_metadata_oid_index'",
	/* see the comment at the top of old_8_3_create_sequence_script() */
			 (GET_MAJOR_VERSION(old_cluster.major_version) <= 803) ?
			 "" : ", 'S'");

	res = executeQueryOrDie(conn, query);

	ntups = PQntuples(res);

	relinfos = (RelInfo *) pg_malloc(sizeof(RelInfo) * ntups);

	i_oid = PQfnumber(res, "oid");
	i_nspname = PQfnumber(res, "nspname");
	i_relname = PQfnumber(res, "relname");
	i_relfilenode = PQfnumber(res, "relfilenode");
	i_reltoastrelid = PQfnumber(res, "reltoastrelid");
	i_spclocation = PQfnumber(res, "spclocation");

	for (relnum = 0; relnum < ntups; relnum++)
	{
		RelInfo    *curr = &relinfos[num_rels++];
		const char *tblspace;

		curr->reloid = atooid(PQgetvalue(res, relnum, i_oid));

		nspname = PQgetvalue(res, relnum, i_nspname);
		strlcpy(curr->nspname, nspname, sizeof(curr->nspname));

		relname = PQgetvalue(res, relnum, i_relname);
		strlcpy(curr->relname, relname, sizeof(curr->relname));

		curr->relfilenode = atooid(PQgetvalue(res, relnum, i_relfilenode));
		curr->toastrelid = atooid(PQgetvalue(res, relnum, i_reltoastrelid));

		tblspace = PQgetvalue(res, relnum, i_spclocation);
		/* if no table tablespace, use the database tablespace */
		if (strlen(tblspace) == 0)
			tblspace = dbinfo->db_tblspace;
		strlcpy(curr->tablespace, tblspace, sizeof(curr->tablespace));
	}
	PQclear(res);

	PQfinish(conn);

	dbinfo->rel_arr.rels = relinfos;
	dbinfo->rel_arr.nrels = num_rels;
}


static void
free_rel_arr(RelInfoArr *rel_arr)
{
	pg_free(rel_arr->rels);
	rel_arr->nrels = 0;
}


void
dbarr_free(DbInfoArr *db_arr)
{
	int			dbnum;

	for (dbnum = 0; dbnum < db_arr->ndbs; dbnum++)
		free_rel_arr(&db_arr->dbs[dbnum].rel_arr);
	db_arr->ndbs = 0;
}


static void
print_db_arr(ClusterInfo *cluster)
{
	int			dbnum;

	pg_log(PG_DEBUG, "%s databases\n", CLUSTER_NAME(cluster));

	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		pg_log(PG_DEBUG, "Database: %s\n", cluster->dbarr.dbs[dbnum].db_name);
		print_rel_arr(&cluster->dbarr.dbs[dbnum].rel_arr);
		pg_log(PG_DEBUG, "\n\n");
	}
}


static void
print_rel_arr(RelInfoArr *arr)
{
	int			relnum;

	for (relnum = 0; relnum < arr->nrels; relnum++)
		pg_log(PG_DEBUG, "relname: %s.%s: reloid: %u reltblspace: %s\n",
			   arr->rels[relnum].nspname, arr->rels[relnum].relname,
			   arr->rels[relnum].reloid, arr->rels[relnum].tablespace);
}
