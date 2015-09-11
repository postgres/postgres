/*
 *	info.c
 *
 *	information support functions
 *
 *	Copyright (c) 2010, PostgreSQL Global Development Group
 *	$PostgreSQL: pgsql/contrib/pg_upgrade/info.c,v 1.11 2010/07/06 19:18:55 momjian Exp $
 */

#include "pg_upgrade.h"

#include "access/transam.h"


static void get_db_infos(migratorContext *ctx, DbInfoArr *dbinfos,
			 Cluster whichCluster);
static void dbarr_print(migratorContext *ctx, DbInfoArr *arr,
			Cluster whichCluster);
static void relarr_print(migratorContext *ctx, RelInfoArr *arr);
static void get_rel_infos(migratorContext *ctx, const DbInfo *dbinfo,
			  RelInfoArr *relarr, Cluster whichCluster);
static void relarr_free(RelInfoArr *rel_arr);
static void map_rel(migratorContext *ctx, const RelInfo *oldrel,
		const RelInfo *newrel, const DbInfo *old_db,
		const DbInfo *new_db, const char *olddata,
		const char *newdata, FileNameMap *map);
static void map_rel_by_id(migratorContext *ctx, Oid oldid, Oid newid,
			  const char *old_nspname, const char *old_relname,
			  const char *new_nspname, const char *new_relname,
			  const char *old_tablespace, const char *new_tablespace, const DbInfo *old_db,
			  const DbInfo *new_db, const char *olddata,
			  const char *newdata, FileNameMap *map);
static RelInfo *relarr_lookup_reloid(migratorContext *ctx,
					 RelInfoArr *rel_arr, Oid oid, Cluster whichCluster);
static RelInfo *relarr_lookup_rel(migratorContext *ctx, RelInfoArr *rel_arr,
				  const char *nspname, const char *relname,
				  Cluster whichCluster);


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
gen_db_file_maps(migratorContext *ctx, DbInfo *old_db, DbInfo *new_db,
				 int *nmaps, const char *old_pgdata, const char *new_pgdata)
{
	FileNameMap *maps;
	int			relnum;
	int			num_maps = 0;

	maps = (FileNameMap *) pg_malloc(ctx, sizeof(FileNameMap) *
									 new_db->rel_arr.nrels);

	for (relnum = 0; relnum < new_db->rel_arr.nrels; relnum++)
	{
		RelInfo    *newrel = &new_db->rel_arr.rels[relnum];
		RelInfo    *oldrel;

		/* toast tables are handled by their parent */
		if (strcmp(newrel->nspname, "pg_toast") == 0)
			continue;

		oldrel = relarr_lookup_rel(ctx, &(old_db->rel_arr), newrel->nspname,
								   newrel->relname, CLUSTER_OLD);

		map_rel(ctx, oldrel, newrel, old_db, new_db, old_pgdata, new_pgdata,
				maps + num_maps);
		num_maps++;

		/*
		 * so much for the mapping of this relation. Now we need a mapping for
		 * its corresponding toast relation if any.
		 */
		if (oldrel->toastrelid > 0)
		{
			RelInfo    *new_toast;
			RelInfo    *old_toast;
			char		new_name[MAXPGPATH];
			char		old_name[MAXPGPATH];

			/* construct the new and old relnames for the toast relation */
			snprintf(old_name, sizeof(old_name), "pg_toast_%u",
					 oldrel->reloid);
			snprintf(new_name, sizeof(new_name), "pg_toast_%u",
					 newrel->reloid);

			/* look them up in their respective arrays */
			old_toast = relarr_lookup_reloid(ctx, &old_db->rel_arr,
											 oldrel->toastrelid, CLUSTER_OLD);
			new_toast = relarr_lookup_rel(ctx, &new_db->rel_arr,
										  "pg_toast", new_name, CLUSTER_NEW);

			/* finally create a mapping for them */
			map_rel(ctx, old_toast, new_toast, old_db, new_db, old_pgdata, new_pgdata,
					maps + num_maps);
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
			old_toast = relarr_lookup_rel(ctx, &old_db->rel_arr,
										  "pg_toast", old_name, CLUSTER_OLD);
			new_toast = relarr_lookup_rel(ctx, &new_db->rel_arr,
										  "pg_toast", new_name, CLUSTER_NEW);

			/* finally create a mapping for them */
			map_rel(ctx, old_toast, new_toast, old_db, new_db, old_pgdata,
					new_pgdata, maps + num_maps);
			num_maps++;
		}
	}

	*nmaps = num_maps;
	return maps;
}


static void
map_rel(migratorContext *ctx, const RelInfo *oldrel, const RelInfo *newrel,
		const DbInfo *old_db, const DbInfo *new_db, const char *olddata,
		const char *newdata, FileNameMap *map)
{
	map_rel_by_id(ctx, oldrel->relfilenode, newrel->relfilenode, oldrel->nspname,
				  oldrel->relname, newrel->nspname, newrel->relname, oldrel->tablespace, newrel->tablespace, old_db,
				  new_db, olddata, newdata, map);
}


/*
 * map_rel_by_id()
 *
 * fills a file node map structure and returns it in "map".
 */
static void
map_rel_by_id(migratorContext *ctx, Oid oldid, Oid newid,
			  const char *old_nspname, const char *old_relname,
			  const char *new_nspname, const char *new_relname,
			  const char *old_tablespace, const char *new_tablespace, const DbInfo *old_db,
			  const DbInfo *new_db, const char *olddata,
			  const char *newdata, FileNameMap *map)
{
	map->new = newid;
	map->old = oldid;

	snprintf(map->old_nspname, sizeof(map->old_nspname), "%s", old_nspname);
	snprintf(map->old_relname, sizeof(map->old_relname), "%s", old_relname);
	snprintf(map->new_nspname, sizeof(map->new_nspname), "%s", new_nspname);
	snprintf(map->new_relname, sizeof(map->new_relname), "%s", new_relname);

	/* In case old/new tablespaces don't match, do them separately. */
	if (strlen(old_tablespace) == 0)
	{
		/*
		 * relation belongs to the default tablespace, hence relfiles would
		 * exist in the data directories.
		 */
		snprintf(map->old_file, sizeof(map->old_file), "%s/base/%u", olddata, old_db->db_oid);
	}
	else
	{
		/*
		 * relation belongs to some tablespace, hence copy its physical
		 * location
		 */
		snprintf(map->old_file, sizeof(map->old_file), "%s%s/%u", old_tablespace,
				 ctx->old.tablespace_suffix, old_db->db_oid);
	}

	/* Do the same for new tablespaces */
	if (strlen(new_tablespace) == 0)
	{
		/*
		 * relation belongs to the default tablespace, hence relfiles would
		 * exist in the data directories.
		 */
		snprintf(map->new_file, sizeof(map->new_file), "%s/base/%u", newdata, new_db->db_oid);
	}
	else
	{
		/*
		 * relation belongs to some tablespace, hence copy its physical
		 * location
		 */
		snprintf(map->new_file, sizeof(map->new_file), "%s%s/%u", new_tablespace,
				 ctx->new.tablespace_suffix, new_db->db_oid);
	}
}


void
print_maps(migratorContext *ctx, FileNameMap *maps, int n, const char *dbName)
{
	if (ctx->debug)
	{
		int			mapnum;

		pg_log(ctx, PG_DEBUG, "mappings for db %s:\n", dbName);

		for (mapnum = 0; mapnum < n; mapnum++)
			pg_log(ctx, PG_DEBUG, "%s.%s:%u ==> %s.%s:%u\n",
				   maps[mapnum].old_nspname, maps[mapnum].old_relname, maps[mapnum].old,
				   maps[mapnum].new_nspname, maps[mapnum].new_relname, maps[mapnum].new);

		pg_log(ctx, PG_DEBUG, "\n\n");
	}
}


/*
 * get_db_infos()
 *
 * Scans pg_database system catalog and returns (in dbinfs_arr) all user
 * databases.
 */
static void
get_db_infos(migratorContext *ctx, DbInfoArr *dbinfs_arr, Cluster whichCluster)
{
	PGconn	   *conn = connectToServer(ctx, "template1", whichCluster);
	PGresult   *res;
	int			ntups;
	int			tupnum;
	DbInfo	   *dbinfos;
	int			i_datname;
	int			i_oid;
	int			i_spclocation;

	res = executeQueryOrDie(ctx, conn,
							"SELECT d.oid, d.datname, t.spclocation "
							"FROM pg_catalog.pg_database d "
							" LEFT OUTER JOIN pg_catalog.pg_tablespace t "
							" ON d.dattablespace = t.oid "
							"WHERE d.datallowconn = true");

	i_datname = PQfnumber(res, "datname");
	i_oid = PQfnumber(res, "oid");
	i_spclocation = PQfnumber(res, "spclocation");

	ntups = PQntuples(res);
	dbinfos = (DbInfo *) pg_malloc(ctx, sizeof(DbInfo) * ntups);

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

	dbinfs_arr->dbs = dbinfos;
	dbinfs_arr->ndbs = ntups;
}


/*
 * get_db_and_rel_infos()
 *
 * higher level routine to generate dbinfos for the database running
 * on the given "port". Assumes that server is already running.
 */
void
get_db_and_rel_infos(migratorContext *ctx, DbInfoArr *db_arr, Cluster whichCluster)
{
	int			dbnum;

	get_db_infos(ctx, db_arr, whichCluster);

	for (dbnum = 0; dbnum < db_arr->ndbs; dbnum++)
		get_rel_infos(ctx, &db_arr->dbs[dbnum],
					  &(db_arr->dbs[dbnum].rel_arr), whichCluster);

	if (ctx->debug)
		dbarr_print(ctx, db_arr, whichCluster);
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
get_rel_infos(migratorContext *ctx, const DbInfo *dbinfo,
			  RelInfoArr *relarr, Cluster whichCluster)
{
	PGconn	   *conn = connectToServer(ctx, dbinfo->db_name, whichCluster);
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
	int			i_reltablespace = -1;
	int			i_reltoastrelid = -1;
	char		query[QUERY_ALLOC];

	/*
	 * pg_largeobject contains user data that does not appear the pg_dumpall
	 * --schema-only output, so we have to migrate that system table heap and
	 * index.  Ideally we could just get the relfilenode from template1 but
	 * pg_largeobject_loid_pn_index's relfilenode can change if the table was
	 * reindexed so we get the relfilenode for each database and migrate it as
	 * a normal user table.
	 */

	snprintf(query, sizeof(query),
			 "SELECT DISTINCT c.oid, n.nspname, c.relname, "
			 "	c.relfilenode, c.reltoastrelid, c.reltablespace, t.spclocation "
			 "FROM pg_catalog.pg_class c JOIN "
			 "		pg_catalog.pg_namespace n "
			 "	ON c.relnamespace = n.oid "
			 "  LEFT OUTER JOIN pg_catalog.pg_index i "
			 "	   ON c.oid = i.indexrelid "
			 "   LEFT OUTER JOIN pg_catalog.pg_tablespace t "
			 "	ON c.reltablespace = t.oid "
			 "WHERE (( "
			 /* exclude pg_catalog and pg_temp_ (could be orphaned tables) */
			 "    n.nspname != 'pg_catalog' "
			 "    AND n.nspname !~ '^pg_temp_' "
			 "    AND n.nspname !~ '^pg_toast_temp_' "
			 "	  AND n.nspname != 'information_schema' "
			 "	  AND c.oid >= %u "
			 "	) OR ( "
			 "	n.nspname = 'pg_catalog' "
			 "	AND relname IN "
			 "        ('pg_largeobject', 'pg_largeobject_loid_pn_index'%s) )) "
			 "	AND relkind IN ('r','t', 'i'%s) "
			 /* pg_dump only dumps valid indexes;  testing indisready is
			 * necessary in 9.2, and harmless in earlier/later versions. */
			 " AND i.indisvalid IS DISTINCT FROM false AND "
			 " i.indisready IS DISTINCT FROM false "
			 "GROUP BY  c.oid, n.nspname, c.relname, c.relfilenode,"
			 "			c.reltoastrelid, c.reltablespace, t.spclocation, "
			 "			n.nspname "
			 "ORDER BY n.nspname, c.relname;",
			 FirstNormalObjectId,
	/* does pg_largeobject_metadata need to be migrated? */
			 (GET_MAJOR_VERSION(ctx->old.major_version) <= 804) ?
			 "" : ", 'pg_largeobject_metadata', 'pg_largeobject_metadata_oid_index'",
	/* see the comment at the top of old_8_3_create_sequence_script() */
			 (GET_MAJOR_VERSION(ctx->old.major_version) <= 803) ?
			 "" : ", 'S'");

	res = executeQueryOrDie(ctx, conn, query);

	ntups = PQntuples(res);

	relinfos = (RelInfo *) pg_malloc(ctx, sizeof(RelInfo) * ntups);

	i_oid = PQfnumber(res, "oid");
	i_nspname = PQfnumber(res, "nspname");
	i_relname = PQfnumber(res, "relname");
	i_relfilenode = PQfnumber(res, "relfilenode");
	i_reltoastrelid = PQfnumber(res, "reltoastrelid");
	i_reltablespace = PQfnumber(res, "reltablespace");
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

	relarr->rels = relinfos;
	relarr->nrels = num_rels;
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

	if (!db_arr || !db_name)
		return NULL;

	for (dbnum = 0; dbnum < db_arr->ndbs; dbnum++)
	{
		if (strcmp(db_arr->dbs[dbnum].db_name, db_name) == 0)
			return &db_arr->dbs[dbnum];
	}

	return NULL;
}


/*
 * relarr_lookup_rel()
 *
 * Searches "relname" in rel_arr. Returns the *real* pointer to the
 * RelInfo structure.
 */
static RelInfo *
relarr_lookup_rel(migratorContext *ctx, RelInfoArr *rel_arr,
				  const char *nspname, const char *relname,
				  Cluster whichCluster)
{
	int			relnum;

	if (!rel_arr || !relname)
		return NULL;

	for (relnum = 0; relnum < rel_arr->nrels; relnum++)
	{
		if (strcmp(rel_arr->rels[relnum].nspname, nspname) == 0 &&
			strcmp(rel_arr->rels[relnum].relname, relname) == 0)
			return &rel_arr->rels[relnum];
	}
	pg_log(ctx, PG_FATAL, "Could not find %s.%s in %s cluster\n",
		   nspname, relname, CLUSTERNAME(whichCluster));
	return NULL;
}


/*
 * relarr_lookup_reloid()
 *
 *	Returns a pointer to the RelInfo structure for the
 *	given oid or NULL if the desired entry cannot be
 *	found.
 */
static RelInfo *
relarr_lookup_reloid(migratorContext *ctx, RelInfoArr *rel_arr, Oid oid,
					 Cluster whichCluster)
{
	int			relnum;

	if (!rel_arr || !oid)
		return NULL;

	for (relnum = 0; relnum < rel_arr->nrels; relnum++)
	{
		if (rel_arr->rels[relnum].reloid == oid)
			return &rel_arr->rels[relnum];
	}
	pg_log(ctx, PG_FATAL, "Could not find %d in %s cluster\n",
		   oid, CLUSTERNAME(whichCluster));
	return NULL;
}


static void
relarr_free(RelInfoArr *rel_arr)
{
	pg_free(rel_arr->rels);
	rel_arr->nrels = 0;
}


void
dbarr_free(DbInfoArr *db_arr)
{
	int			dbnum;

	for (dbnum = 0; dbnum < db_arr->ndbs; dbnum++)
		relarr_free(&db_arr->dbs[dbnum].rel_arr);
	db_arr->ndbs = 0;
}


static void
dbarr_print(migratorContext *ctx, DbInfoArr *arr, Cluster whichCluster)
{
	int			dbnum;

	pg_log(ctx, PG_DEBUG, "%s databases\n", CLUSTERNAME(whichCluster));

	for (dbnum = 0; dbnum < arr->ndbs; dbnum++)
	{
		pg_log(ctx, PG_DEBUG, "Database: %s\n", arr->dbs[dbnum].db_name);
		relarr_print(ctx, &arr->dbs[dbnum].rel_arr);
		pg_log(ctx, PG_DEBUG, "\n\n");
	}
}


static void
relarr_print(migratorContext *ctx, RelInfoArr *arr)
{
	int			relnum;

	for (relnum = 0; relnum < arr->nrels; relnum++)
		pg_log(ctx, PG_DEBUG, "relname: %s.%s: reloid: %u reltblspace: %s\n",
			   arr->rels[relnum].nspname, arr->rels[relnum].relname,
			   arr->rels[relnum].reloid, arr->rels[relnum].tablespace);
}
