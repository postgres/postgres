/*
 *	info.c
 *
 *	information support functions
 *
 *	Copyright (c) 2010-2013, PostgreSQL Global Development Group
 *	contrib/pg_upgrade/info.c
 */

#include "postgres_fe.h"

#include "pg_upgrade.h"

#include "access/transam.h"


static void create_rel_filename_map(const char *old_data, const char *new_data,
						const DbInfo *old_db, const DbInfo *new_db,
						const RelInfo *old_rel, const RelInfo *new_rel,
						FileNameMap *map);
static void free_db_and_rel_infos(DbInfoArr *db_arr);
static void get_db_infos(ClusterInfo *cluster);
static void get_rel_infos(ClusterInfo *cluster, DbInfo *dbinfo);
static void free_rel_infos(RelInfoArr *rel_arr);
static void print_db_infos(DbInfoArr *dbinfo);
static void print_rel_infos(RelInfoArr *rel_arr);


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

	maps = (FileNameMap *) pg_malloc(sizeof(FileNameMap) *
									 old_db->rel_arr.nrels);

	for (relnum = 0; relnum < Min(old_db->rel_arr.nrels, new_db->rel_arr.nrels);
		 relnum++)
	{
		RelInfo    *old_rel = &old_db->rel_arr.rels[relnum];
		RelInfo    *new_rel = &new_db->rel_arr.rels[relnum];

		if (old_rel->reloid != new_rel->reloid)
			pg_log(PG_FATAL, "Mismatch of relation OID in database \"%s\": old OID %d, new OID %d\n",
				   old_db->db_name, old_rel->reloid, new_rel->reloid);

		/*
		 * TOAST table names initially match the heap pg_class oid. In
		 * pre-8.4, TOAST table names change during CLUSTER; in pre-9.0, TOAST
		 * table names change during ALTER TABLE ALTER COLUMN SET TYPE. In >=
		 * 9.0, TOAST relation names always use heap table oids, hence we
		 * cannot check relation names when upgrading from pre-9.0. Clusters
		 * upgraded to 9.0 will get matching TOAST names. If index names don't
		 * match primary key constraint names, this will fail because pg_dump
		 * dumps constraint names and pg_upgrade checks index names.
		 */
		if (strcmp(old_rel->nspname, new_rel->nspname) != 0 ||
			((GET_MAJOR_VERSION(old_cluster.major_version) >= 900 ||
			  strcmp(old_rel->nspname, "pg_toast") != 0) &&
			 strcmp(old_rel->relname, new_rel->relname) != 0))
			pg_log(PG_FATAL, "Mismatch of relation names in database \"%s\": "
				   "old name \"%s.%s\", new name \"%s.%s\"\n",
				   old_db->db_name, old_rel->nspname, old_rel->relname,
				   new_rel->nspname, new_rel->relname);

		create_rel_filename_map(old_pgdata, new_pgdata, old_db, new_db,
								old_rel, new_rel, maps + num_maps);
		num_maps++;
	}

	/*
	 * Do this check after the loop so hopefully we will produce a clearer
	 * error above
	 */
	if (old_db->rel_arr.nrels != new_db->rel_arr.nrels)
		pg_log(PG_FATAL, "old and new databases \"%s\" have a different number of relations\n",
			   old_db->db_name);

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
		strlcpy(map->old_tablespace, old_data, sizeof(map->old_tablespace));
		strlcpy(map->new_tablespace, new_data, sizeof(map->new_tablespace));
		strlcpy(map->old_tablespace_suffix, "/base", sizeof(map->old_tablespace_suffix));
		strlcpy(map->new_tablespace_suffix, "/base", sizeof(map->new_tablespace_suffix));
	}
	else
	{
		/* relation belongs to a tablespace, so use the tablespace location */
		strlcpy(map->old_tablespace, old_rel->tablespace, sizeof(map->old_tablespace));
		strlcpy(map->new_tablespace, new_rel->tablespace, sizeof(map->new_tablespace));
		strlcpy(map->old_tablespace_suffix, old_cluster.tablespace_suffix,
				sizeof(map->old_tablespace_suffix));
		strlcpy(map->new_tablespace_suffix, new_cluster.tablespace_suffix,
				sizeof(map->new_tablespace_suffix));
	}

	map->old_db_oid = old_db->db_oid;
	map->new_db_oid = new_db->db_oid;

	/*
	 * old_relfilenode might differ from pg_class.oid (and hence
	 * new_relfilenode) because of CLUSTER, REINDEX, or VACUUM FULL.
	 */
	map->old_relfilenode = old_rel->relfilenode;

	/* new_relfilenode will match old and new pg_class.oid */
	map->new_relfilenode = new_rel->relfilenode;

	/* used only for logging and error reporing, old/new are identical */
	map->nspname = old_rel->nspname;
	map->relname = old_rel->relname;
}


void
print_maps(FileNameMap *maps, int n_maps, const char *db_name)
{
	if (log_opts.verbose)
	{
		int			mapnum;

		pg_log(PG_VERBOSE, "mappings for database \"%s\":\n", db_name);

		for (mapnum = 0; mapnum < n_maps; mapnum++)
			pg_log(PG_VERBOSE, "%s.%s: %u to %u\n",
				   maps[mapnum].nspname, maps[mapnum].relname,
				   maps[mapnum].old_relfilenode,
				   maps[mapnum].new_relfilenode);

		pg_log(PG_VERBOSE, "\n\n");
	}
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

	if (cluster->dbarr.dbs != NULL)
		free_db_and_rel_infos(&cluster->dbarr);

	get_db_infos(cluster);

	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
		get_rel_infos(cluster, &cluster->dbarr.dbs[dbnum]);

	pg_log(PG_VERBOSE, "\n%s databases:\n", CLUSTER_NAME(cluster));
	if (log_opts.verbose)
		print_db_infos(&cluster->dbarr);
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
	int			i_datname,
				i_oid,
				i_spclocation;
	char		query[QUERY_ALLOC];

	snprintf(query, sizeof(query),
			 "SELECT d.oid, d.datname, %s "
			 "FROM pg_catalog.pg_database d "
			 " LEFT OUTER JOIN pg_catalog.pg_tablespace t "
			 " ON d.dattablespace = t.oid "
			 "WHERE d.datallowconn = true "
	/* we don't preserve pg_database.oid so we sort by name */
			 "ORDER BY 2",
	/* 9.2 removed the spclocation column */
			 (GET_MAJOR_VERSION(cluster->major_version) <= 901) ?
			 "t.spclocation" : "pg_catalog.pg_tablespace_location(t.oid) AS spclocation");

	res = executeQueryOrDie(conn, "%s", query);

	i_oid = PQfnumber(res, "oid");
	i_datname = PQfnumber(res, "datname");
	i_spclocation = PQfnumber(res, "spclocation");

	ntups = PQntuples(res);
	dbinfos = (DbInfo *) pg_malloc(sizeof(DbInfo) * ntups);

	for (tupnum = 0; tupnum < ntups; tupnum++)
	{
		dbinfos[tupnum].db_oid = atooid(PQgetvalue(res, tupnum, i_oid));
		dbinfos[tupnum].db_name = pg_strdup(PQgetvalue(res, tupnum, i_datname));
		snprintf(dbinfos[tupnum].db_tblspace, sizeof(dbinfos[tupnum].db_tblspace), "%s",
				 PQgetvalue(res, tupnum, i_spclocation));
	}
	PQclear(res);

	PQfinish(conn);

	cluster->dbarr.dbs = dbinfos;
	cluster->dbarr.ndbs = ntups;
}


/*
 * get_rel_infos()
 *
 * gets the relinfos for all the user tables of the database referred
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
	int			i_spclocation,
				i_nspname,
				i_relname,
				i_oid,
				i_relfilenode,
				i_reltablespace;
	char		query[QUERY_ALLOC];

	/*
	 * pg_largeobject contains user data that does not appear in pg_dumpall
	 * --schema-only output, so we have to copy that system table heap and
	 * index.  We could grab the pg_largeobject oids from template1, but it is
	 * easy to treat it as a normal table. Order by oid so we can join old/new
	 * structures efficiently.
	 */

	snprintf(query, sizeof(query),
			 "CREATE TEMPORARY TABLE info_rels (reloid) AS SELECT c.oid "
			 "FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n "
			 "	   ON c.relnamespace = n.oid "
			 "LEFT OUTER JOIN pg_catalog.pg_index i "
			 "	   ON c.oid = i.indexrelid "
			 "WHERE relkind IN ('r', 'm', 'i'%s) AND "

	/*
	 * pg_dump only dumps valid indexes;  testing indisready is necessary in
	 * 9.2, and harmless in earlier/later versions.
	 */
			 " i.indisvalid IS DISTINCT FROM false AND "
			 " i.indisready IS DISTINCT FROM false AND "
	/* exclude possible orphaned temp tables */
			 "  ((n.nspname !~ '^pg_temp_' AND "
			 "    n.nspname !~ '^pg_toast_temp_' AND "
	/* skip pg_toast because toast index have relkind == 'i', not 't' */
			 "    n.nspname NOT IN ('pg_catalog', 'information_schema', "
			 "						'binary_upgrade', 'pg_toast') AND "
			 "	  c.oid >= %u) "
			 "  OR (n.nspname = 'pg_catalog' AND "
	"    relname IN ('pg_largeobject', 'pg_largeobject_loid_pn_index'%s) ));",
	/* see the comment at the top of old_8_3_create_sequence_script() */
			 (GET_MAJOR_VERSION(old_cluster.major_version) <= 803) ?
			 "" : ", 'S'",
			 FirstNormalObjectId,
	/* does pg_largeobject_metadata need to be migrated? */
			 (GET_MAJOR_VERSION(old_cluster.major_version) <= 804) ?
	"" : ", 'pg_largeobject_metadata', 'pg_largeobject_metadata_oid_index'");

	PQclear(executeQueryOrDie(conn, "%s", query));

	/*
	 * Get TOAST tables and indexes;  we have to gather the TOAST tables in
	 * later steps because we can't schema-qualify TOAST tables.
	 */
	PQclear(executeQueryOrDie(conn,
							  "INSERT INTO info_rels "
							  "SELECT reltoastrelid "
							  "FROM info_rels i JOIN pg_catalog.pg_class c "
							  "		ON i.reloid = c.oid"));
	PQclear(executeQueryOrDie(conn,
							  "INSERT INTO info_rels "
							  "SELECT reltoastidxid "
							  "FROM info_rels i JOIN pg_catalog.pg_class c "
							  "		ON i.reloid = c.oid"));

	snprintf(query, sizeof(query),
			 "SELECT c.oid, n.nspname, c.relname, "
			 "	c.relfilenode, c.reltablespace, %s "
			 "FROM info_rels i JOIN pg_catalog.pg_class c "
			 "		ON i.reloid = c.oid "
			 "  JOIN pg_catalog.pg_namespace n "
			 "	   ON c.relnamespace = n.oid "
			 "  LEFT OUTER JOIN pg_catalog.pg_tablespace t "
			 "	   ON c.reltablespace = t.oid "
	/* we preserve pg_class.oid so we sort by it to match old/new */
			 "ORDER BY 1;",
	/* 9.2 removed the spclocation column */
			 (GET_MAJOR_VERSION(cluster->major_version) <= 901) ?
			 "t.spclocation" : "pg_catalog.pg_tablespace_location(t.oid) AS spclocation");

	res = executeQueryOrDie(conn, "%s", query);

	ntups = PQntuples(res);

	relinfos = (RelInfo *) pg_malloc(sizeof(RelInfo) * ntups);

	i_oid = PQfnumber(res, "oid");
	i_nspname = PQfnumber(res, "nspname");
	i_relname = PQfnumber(res, "relname");
	i_relfilenode = PQfnumber(res, "relfilenode");
	i_reltablespace = PQfnumber(res, "reltablespace");
	i_spclocation = PQfnumber(res, "spclocation");

	for (relnum = 0; relnum < ntups; relnum++)
	{
		RelInfo    *curr = &relinfos[num_rels++];
		const char *tblspace;

		curr->reloid = atooid(PQgetvalue(res, relnum, i_oid));

		nspname = PQgetvalue(res, relnum, i_nspname);
		curr->nspname = pg_strdup(nspname);

		relname = PQgetvalue(res, relnum, i_relname);
		curr->relname = pg_strdup(relname);

		curr->relfilenode = atooid(PQgetvalue(res, relnum, i_relfilenode));

		if (atooid(PQgetvalue(res, relnum, i_reltablespace)) != 0)
			/* Might be "", meaning the cluster default location. */
			tblspace = PQgetvalue(res, relnum, i_spclocation);
		else
			/* A zero reltablespace indicates the database tablespace. */
			tblspace = dbinfo->db_tblspace;

		strlcpy(curr->tablespace, tblspace, sizeof(curr->tablespace));
	}
	PQclear(res);

	PQfinish(conn);

	dbinfo->rel_arr.rels = relinfos;
	dbinfo->rel_arr.nrels = num_rels;
}


static void
free_db_and_rel_infos(DbInfoArr *db_arr)
{
	int			dbnum;

	for (dbnum = 0; dbnum < db_arr->ndbs; dbnum++)
	{
		free_rel_infos(&db_arr->dbs[dbnum].rel_arr);
		pg_free(db_arr->dbs[dbnum].db_name);
	}
	pg_free(db_arr->dbs);
	db_arr->dbs = NULL;
	db_arr->ndbs = 0;
}


static void
free_rel_infos(RelInfoArr *rel_arr)
{
	int			relnum;

	for (relnum = 0; relnum < rel_arr->nrels; relnum++)
	{
		pg_free(rel_arr->rels[relnum].nspname);
		pg_free(rel_arr->rels[relnum].relname);
	}
	pg_free(rel_arr->rels);
	rel_arr->nrels = 0;
}


static void
print_db_infos(DbInfoArr *db_arr)
{
	int			dbnum;

	for (dbnum = 0; dbnum < db_arr->ndbs; dbnum++)
	{
		pg_log(PG_VERBOSE, "Database: %s\n", db_arr->dbs[dbnum].db_name);
		print_rel_infos(&db_arr->dbs[dbnum].rel_arr);
		pg_log(PG_VERBOSE, "\n\n");
	}
}


static void
print_rel_infos(RelInfoArr *rel_arr)
{
	int			relnum;

	for (relnum = 0; relnum < rel_arr->nrels; relnum++)
		pg_log(PG_VERBOSE, "relname: %s.%s: reloid: %u reltblspace: %s\n",
			   rel_arr->rels[relnum].nspname,
			   rel_arr->rels[relnum].relname,
			   rel_arr->rels[relnum].reloid,
			   rel_arr->rels[relnum].tablespace);
}
