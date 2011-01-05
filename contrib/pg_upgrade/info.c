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
static RelInfo *relarr_lookup_rel_name(ClusterInfo *cluster, RelInfoArr *rel_arr,
				  const char *nspname, const char *relname);
static RelInfo *relarr_lookup_rel_oid(ClusterInfo *cluster, RelInfoArr *rel_arr,
				Oid oid);


/*
 * gen_db_file_maps()
 *
 * generates database mappings for "old_db" and "new_db". Returns a malloc'ed
 * array of mappings. nmaps is a return parameter which refers to the number
 * mappings.
 *
 * NOTE: Its the Caller's responsibility to free the returned array.
 */
FileNameMap *
gen_db_file_maps(DbInfo *old_db, DbInfo *new_db,
				 int *nmaps, const char *old_pgdata, const char *new_pgdata)
{
	FileNameMap *maps;
	int			relnum;
	int			num_maps = 0;

	maps = (FileNameMap *) pg_malloc(sizeof(FileNameMap) *
									 new_db->rel_arr.nrels);

	for (relnum = 0; relnum < new_db->rel_arr.nrels; relnum++)
	{
		RelInfo    *newrel = &new_db->rel_arr.rels[relnum];
		RelInfo    *oldrel;

		/* toast tables are handled by their parent */
		if (strcmp(newrel->nspname, "pg_toast") == 0)
			continue;

		oldrel = relarr_lookup_rel_name(&old_cluster, &old_db->rel_arr,
								   newrel->nspname, newrel->relname);

		create_rel_filename_map(old_pgdata, new_pgdata, old_db, new_db,
				oldrel, newrel, maps + num_maps);
		num_maps++;

		/*
		 * So much for mapping this relation;  now we need a mapping
		 * for its corresponding toast relation, if any.
		 */
		if (oldrel->toastrelid > 0)
		{
			RelInfo    *new_toast;
			RelInfo    *old_toast;
			char		new_name[MAXPGPATH];
			char		old_name[MAXPGPATH];

			/* construct the new and old relnames for the toast relation */
			snprintf(old_name, sizeof(old_name), "pg_toast_%u", oldrel->reloid);
			snprintf(new_name, sizeof(new_name), "pg_toast_%u", newrel->reloid);

			/* look them up in their respective arrays */
			old_toast = relarr_lookup_rel_oid(&old_cluster, &old_db->rel_arr,
											 oldrel->toastrelid);
			new_toast = relarr_lookup_rel_name(&new_cluster, &new_db->rel_arr,
										  "pg_toast", new_name);

			/* finally create a mapping for them */
			create_rel_filename_map(old_pgdata, new_pgdata, old_db, new_db,
					old_toast, new_toast, maps + num_maps);
			num_maps++;

			/*
			 * also need to provide a mapping for the index of this toast
			 * relation. The procedure is similar to what we did above for
			 * toast relation itself, the only difference being that the
			 * relnames need to be appended with _index.
			 */

			/*
			 * construct the new and old relnames for the toast index
			 * relations
			 */
			snprintf(old_name, sizeof(old_name), "%s_index", old_toast->relname);
			snprintf(new_name, sizeof(new_name), "pg_toast_%u_index",
					 newrel->reloid);

			/* look them up in their respective arrays */
			old_toast = relarr_lookup_rel_name(&old_cluster, &old_db->rel_arr,
										  "pg_toast", old_name);
			new_toast = relarr_lookup_rel_name(&new_cluster, &new_db->rel_arr,
										  "pg_toast", new_name);

			/* finally create a mapping for them */
			create_rel_filename_map(old_pgdata, new_pgdata, old_db,
					new_db, old_toast, new_toast, maps + num_maps);
			num_maps++;
		}
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
	map->old_relfilenode = old_rel->relfilenode;
	map->new_relfilenode = new_rel->relfilenode;

	snprintf(map->old_nspname, sizeof(map->old_nspname), "%s", old_rel->nspname);
	snprintf(map->new_nspname, sizeof(map->new_nspname), "%s", new_rel->nspname);

	snprintf(map->old_relname, sizeof(map->old_relname), "%s", old_rel->relname);
	snprintf(map->new_relname, sizeof(map->new_relname), "%s", new_rel->relname);

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
		/*
		 * relation belongs to some tablespace, so use the tablespace location
		 */
		snprintf(map->old_dir, sizeof(map->old_dir), "%s%s/%u", old_rel->tablespace,
				 old_cluster.tablespace_suffix, old_db->db_oid);
		snprintf(map->new_dir, sizeof(map->new_dir), "%s%s/%u", new_rel->tablespace,
				 new_cluster.tablespace_suffix, new_db->db_oid);
	}
}


void
print_maps(FileNameMap *maps, int n, const char *dbName)
{
	if (log_opts.debug)
	{
		int			mapnum;

		pg_log(PG_DEBUG, "mappings for db %s:\n", dbName);

		for (mapnum = 0; mapnum < n; mapnum++)
			pg_log(PG_DEBUG, "%s.%s:%u ==> %s.%s:%u\n",
				   maps[mapnum].old_nspname, maps[mapnum].old_relname,
				   maps[mapnum].old_relfilenode,
				   maps[mapnum].new_nspname, maps[mapnum].new_relname,
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
							"WHERE d.datallowconn = true");

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
	 * pg_largeobject contains user data that does not appear the pg_dumpall
	 * --schema-only output, so we have to upgrade that system table heap and
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
			 "WHERE (( n.nspname NOT IN ('pg_catalog', 'information_schema') "
			 "	AND c.oid >= %u "
			 "	) OR ( "
			 "	n.nspname = 'pg_catalog' "
			 "	AND relname IN "
			 "        ('pg_largeobject', 'pg_largeobject_loid_pn_index') )) "
			 "	AND relkind IN ('r','t', 'i'%s)"
			 "GROUP BY  c.oid, n.nspname, c.relname, c.relfilenode,"
			 "			c.reltoastrelid, t.spclocation, "
			 "			n.nspname "
			 "ORDER BY t.spclocation, n.nspname, c.relname;",
			 FirstNormalObjectId,
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
	dbinfo->rel_arr.last_relname_lookup = 0;
}


/*
 * dbarr_lookup_db()
 *
 * Returns the pointer to the DbInfo structure
 */
DbInfo *
dbarr_lookup_db(DbInfoArr *db_arr, const char *db_name)
{
	int			dbnum;

	for (dbnum = 0; dbnum < db_arr->ndbs; dbnum++)
	{
		if (strcmp(db_arr->dbs[dbnum].db_name, db_name) == 0)
			return &db_arr->dbs[dbnum];
	}

	return NULL;
}


/*
 * relarr_lookup_rel_name()
 *
 * Searches "relname" in rel_arr. Returns the *real* pointer to the
 * RelInfo structure.
 */
static RelInfo *
relarr_lookup_rel_name(ClusterInfo *cluster, RelInfoArr *rel_arr,
					const char *nspname, const char *relname)
{
	int			relnum;

	/* Test next lookup first, for speed */
	if (rel_arr->last_relname_lookup + 1 < rel_arr->nrels &&
		strcmp(rel_arr->rels[rel_arr->last_relname_lookup + 1].nspname, nspname) == 0 &&
		strcmp(rel_arr->rels[rel_arr->last_relname_lookup + 1].relname, relname) == 0)
	{
		rel_arr->last_relname_lookup++;
		return &rel_arr->rels[rel_arr->last_relname_lookup];
	}

	for (relnum = 0; relnum < rel_arr->nrels; relnum++)
	{
		if (strcmp(rel_arr->rels[relnum].nspname, nspname) == 0 &&
			strcmp(rel_arr->rels[relnum].relname, relname) == 0)
		{
			rel_arr->last_relname_lookup = relnum;
			return &rel_arr->rels[relnum];
		}
	}
	pg_log(PG_FATAL, "Could not find %s.%s in %s cluster\n",
		   nspname, relname, CLUSTER_NAME(cluster));
	return NULL;
}


/*
 * relarr_lookup_rel_oid()
 *
 *	Returns a pointer to the RelInfo structure for the
 *	given oid or NULL if the desired entry cannot be
 *	found.
 */
static RelInfo *
relarr_lookup_rel_oid(ClusterInfo *cluster, RelInfoArr *rel_arr, Oid oid)
{
	int			relnum;

	for (relnum = 0; relnum < rel_arr->nrels; relnum++)
	{
		if (rel_arr->rels[relnum].reloid == oid)
			return &rel_arr->rels[relnum];
	}
	pg_log(PG_FATAL, "Could not find %d in %s cluster\n",
		   oid, CLUSTER_NAME(cluster));
	return NULL;
}


static void
free_rel_arr(RelInfoArr *rel_arr)
{
	pg_free(rel_arr->rels);
	rel_arr->nrels = 0;
	rel_arr->last_relname_lookup = 0;
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
