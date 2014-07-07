/*
 *	info.c
 *
 *	information support functions
 *
 *	Copyright (c) 2010-2014, PostgreSQL Global Development Group
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
	int			old_relnum, new_relnum;
	int			num_maps = 0;

	maps = (FileNameMap *) pg_malloc(sizeof(FileNameMap) *
									 old_db->rel_arr.nrels);

	/*
	 * The old database shouldn't have more relations than the new one.
	 * We force the new cluster to have a TOAST table if the old table
	 * had one.
	 */
	if (old_db->rel_arr.nrels > new_db->rel_arr.nrels)
		pg_fatal("old and new databases \"%s\" have a mismatched number of relations\n",
				 old_db->db_name);

	/* Drive the loop using new_relnum, which might be higher. */
	for (old_relnum = new_relnum = 0; new_relnum < new_db->rel_arr.nrels;
		 new_relnum++)
	{
		RelInfo    *old_rel;
		RelInfo    *new_rel = &new_db->rel_arr.rels[new_relnum];

		/*
		 * It is possible that the new cluster has a TOAST table for a table
		 * that didn't need one in the old cluster, e.g. 9.0 to 9.1 changed the
		 * NUMERIC length computation.  Therefore, if we have a TOAST table
		 * in the new cluster that doesn't match, skip over it and continue
		 * processing.  It is possible this TOAST table used an OID that was
		 * reserved in the old cluster, but we have no way of testing that,
		 * and we would have already gotten an error at the new cluster schema
		 * creation stage.  Fortunately, since we only restore the OID counter
		 * after schema restore, and restore in OID order via pg_dump, a
		 * conflict would only happen if the new TOAST table had a very low
		 * OID.  However, TOAST tables created long after initial table
		 * creation can have any OID, particularly after OID wraparound.
		 */
		if (old_relnum == old_db->rel_arr.nrels)
		{
			if (strcmp(new_rel->nspname, "pg_toast") == 0)
				continue;
			else
				pg_fatal("Extra non-TOAST relation found in database \"%s\": new OID %d\n",
						 old_db->db_name, new_rel->reloid);
		}

		old_rel = &old_db->rel_arr.rels[old_relnum];

		if (old_rel->reloid != new_rel->reloid)
		{
			if (strcmp(new_rel->nspname, "pg_toast") == 0)
				continue;
			else
				pg_fatal("Mismatch of relation OID in database \"%s\": old OID %d, new OID %d\n",
						 old_db->db_name, old_rel->reloid, new_rel->reloid);
		}

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
			pg_fatal("Mismatch of relation names in database \"%s\": "
					 "old name \"%s.%s\", new name \"%s.%s\"\n",
					 old_db->db_name, old_rel->nspname, old_rel->relname,
					 new_rel->nspname, new_rel->relname);

		create_rel_filename_map(old_pgdata, new_pgdata, old_db, new_db,
								old_rel, new_rel, maps + num_maps);
		num_maps++;
		old_relnum++;
	}

	/* Did we fail to exhaust the old array? */
	if (old_relnum != old_db->rel_arr.nrels)
		pg_fatal("old and new databases \"%s\" have a mismatched number of relations\n",
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
		map->old_tablespace = old_data;
		map->new_tablespace = new_data;
		map->old_tablespace_suffix = "/base";
		map->new_tablespace_suffix = "/base";
	}
	else
	{
		/* relation belongs to a tablespace, so use the tablespace location */
		map->old_tablespace = old_rel->tablespace;
		map->new_tablespace = new_rel->tablespace;
		map->old_tablespace_suffix = old_cluster.tablespace_suffix;
		map->new_tablespace_suffix = new_cluster.tablespace_suffix;
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
		snprintf(dbinfos[tupnum].db_tablespace, sizeof(dbinfos[tupnum].db_tablespace), "%s",
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
	char	   *tablespace = NULL;
	int			i_spclocation,
				i_nspname,
				i_relname,
				i_oid,
				i_relfilenode,
				i_reltablespace;
	char		query[QUERY_ALLOC];
	char	   *last_namespace = NULL,
			   *last_tablespace = NULL;

	/*
	 * pg_largeobject contains user data that does not appear in pg_dump
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
							  "		ON i.reloid = c.oid "
						  "		AND c.reltoastrelid != %u", InvalidOid));
	PQclear(executeQueryOrDie(conn,
							  "INSERT INTO info_rels "
							  "SELECT indexrelid "
							  "FROM pg_index "
							  "WHERE indisvalid "
							  "    AND indrelid IN (SELECT reltoastrelid "
							  "        FROM info_rels i "
							  "            JOIN pg_catalog.pg_class c "
							  "            ON i.reloid = c.oid "
							  "            AND c.reltoastrelid != %u)",
							  InvalidOid));

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

		curr->reloid = atooid(PQgetvalue(res, relnum, i_oid));

		nspname = PQgetvalue(res, relnum, i_nspname);
		curr->nsp_alloc = false;

		/*
		 * Many of the namespace and tablespace strings are identical, so we
		 * try to reuse the allocated string pointers where possible to reduce
		 * memory consumption.
		 */
		/* Can we reuse the previous string allocation? */
		if (last_namespace && strcmp(nspname, last_namespace) == 0)
			curr->nspname = last_namespace;
		else
		{
			last_namespace = curr->nspname = pg_strdup(nspname);
			curr->nsp_alloc = true;
		}

		relname = PQgetvalue(res, relnum, i_relname);
		curr->relname = pg_strdup(relname);

		curr->relfilenode = atooid(PQgetvalue(res, relnum, i_relfilenode));
		curr->tblsp_alloc = false;

		/* Is the tablespace oid non-zero? */
		if (atooid(PQgetvalue(res, relnum, i_reltablespace)) != 0)
		{
			/*
			 * The tablespace location might be "", meaning the cluster
			 * default location, i.e. pg_default or pg_global.
			 */
			tablespace = PQgetvalue(res, relnum, i_spclocation);

			/* Can we reuse the previous string allocation? */
			if (last_tablespace && strcmp(tablespace, last_tablespace) == 0)
				curr->tablespace = last_tablespace;
			else
			{
				last_tablespace = curr->tablespace = pg_strdup(tablespace);
				curr->tblsp_alloc = true;
			}
		}
		else
			/* A zero reltablespace oid indicates the database tablespace. */
			curr->tablespace = dbinfo->db_tablespace;
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
		if (rel_arr->rels[relnum].nsp_alloc)
			pg_free(rel_arr->rels[relnum].nspname);
		pg_free(rel_arr->rels[relnum].relname);
		if (rel_arr->rels[relnum].tblsp_alloc)
			pg_free(rel_arr->rels[relnum].tablespace);
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
