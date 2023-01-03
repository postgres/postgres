/*
 *	info.c
 *
 *	information support functions
 *
 *	Copyright (c) 2010-2022, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/info.c
 */

#include "postgres_fe.h"

#include "access/transam.h"
#include "catalog/pg_class_d.h"
#include "pg_upgrade.h"

static void create_rel_filename_map(const char *old_data, const char *new_data,
									const DbInfo *old_db, const DbInfo *new_db,
									const RelInfo *old_rel, const RelInfo *new_rel,
									FileNameMap *map);
static void report_unmatched_relation(const RelInfo *rel, const DbInfo *db,
									  bool is_new_db);
static void free_db_and_rel_infos(DbInfoArr *db_arr);
static void get_db_infos(ClusterInfo *cluster);
static void get_rel_infos(ClusterInfo *cluster, DbInfo *dbinfo);
static void free_rel_infos(RelInfoArr *rel_arr);
static void print_db_infos(DbInfoArr *dbinfo);
static void print_rel_infos(RelInfoArr *rel_arr);


/*
 * gen_db_file_maps()
 *
 * generates a database mapping from "old_db" to "new_db".
 *
 * Returns a malloc'ed array of mappings.  The length of the array
 * is returned into *nmaps.
 */
FileNameMap *
gen_db_file_maps(DbInfo *old_db, DbInfo *new_db,
				 int *nmaps,
				 const char *old_pgdata, const char *new_pgdata)
{
	FileNameMap *maps;
	int			old_relnum,
				new_relnum;
	int			num_maps = 0;
	bool		all_matched = true;

	/* There will certainly not be more mappings than there are old rels */
	maps = (FileNameMap *) pg_malloc(sizeof(FileNameMap) *
									 old_db->rel_arr.nrels);

	/*
	 * Each of the RelInfo arrays should be sorted by OID.  Scan through them
	 * and match them up.  If we fail to match everything, we'll abort, but
	 * first print as much info as we can about mismatches.
	 */
	old_relnum = new_relnum = 0;
	while (old_relnum < old_db->rel_arr.nrels ||
		   new_relnum < new_db->rel_arr.nrels)
	{
		RelInfo    *old_rel = (old_relnum < old_db->rel_arr.nrels) ?
		&old_db->rel_arr.rels[old_relnum] : NULL;
		RelInfo    *new_rel = (new_relnum < new_db->rel_arr.nrels) ?
		&new_db->rel_arr.rels[new_relnum] : NULL;

		/* handle running off one array before the other */
		if (!new_rel)
		{
			/*
			 * old_rel is unmatched.  This should never happen, because we
			 * force new rels to have TOAST tables if the old one did.
			 */
			report_unmatched_relation(old_rel, old_db, false);
			all_matched = false;
			old_relnum++;
			continue;
		}
		if (!old_rel)
		{
			/*
			 * new_rel is unmatched.  This shouldn't really happen either, but
			 * if it's a TOAST table, we can ignore it and continue
			 * processing, assuming that the new server made a TOAST table
			 * that wasn't needed.
			 */
			if (strcmp(new_rel->nspname, "pg_toast") != 0)
			{
				report_unmatched_relation(new_rel, new_db, true);
				all_matched = false;
			}
			new_relnum++;
			continue;
		}

		/* check for mismatched OID */
		if (old_rel->reloid < new_rel->reloid)
		{
			/* old_rel is unmatched, see comment above */
			report_unmatched_relation(old_rel, old_db, false);
			all_matched = false;
			old_relnum++;
			continue;
		}
		else if (old_rel->reloid > new_rel->reloid)
		{
			/* new_rel is unmatched, see comment above */
			if (strcmp(new_rel->nspname, "pg_toast") != 0)
			{
				report_unmatched_relation(new_rel, new_db, true);
				all_matched = false;
			}
			new_relnum++;
			continue;
		}

		/*
		 * Verify that rels of same OID have same name.  The namespace name
		 * should always match, but the relname might not match for TOAST
		 * tables (and, therefore, their indexes).
		 */
		if (strcmp(old_rel->nspname, new_rel->nspname) != 0 ||
			strcmp(old_rel->relname, new_rel->relname) != 0)
		{
			pg_log(PG_WARNING, "Relation names for OID %u in database \"%s\" do not match: "
				   "old name \"%s.%s\", new name \"%s.%s\"\n",
				   old_rel->reloid, old_db->db_name,
				   old_rel->nspname, old_rel->relname,
				   new_rel->nspname, new_rel->relname);
			all_matched = false;
			old_relnum++;
			new_relnum++;
			continue;
		}

		/* OK, create a mapping entry */
		create_rel_filename_map(old_pgdata, new_pgdata, old_db, new_db,
								old_rel, new_rel, maps + num_maps);
		num_maps++;
		old_relnum++;
		new_relnum++;
	}

	if (!all_matched)
		pg_fatal("Failed to match up old and new tables in database \"%s\"\n",
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
	/* In case old/new tablespaces don't match, do them separately. */
	if (strlen(old_rel->tablespace) == 0)
	{
		/*
		 * relation belongs to the default tablespace, hence relfiles should
		 * exist in the data directories.
		 */
		map->old_tablespace = old_data;
		map->old_tablespace_suffix = "/base";
	}
	else
	{
		/* relation belongs to a tablespace, so use the tablespace location */
		map->old_tablespace = old_rel->tablespace;
		map->old_tablespace_suffix = old_cluster.tablespace_suffix;
	}

	/* Do the same for new tablespaces */
	if (strlen(new_rel->tablespace) == 0)
	{
		map->new_tablespace = new_data;
		map->new_tablespace_suffix = "/base";
	}
	else
	{
		map->new_tablespace = new_rel->tablespace;
		map->new_tablespace_suffix = new_cluster.tablespace_suffix;
	}

	/* DB oid and relfilenodes are preserved between old and new cluster */
	map->db_oid = old_db->db_oid;
	map->relfilenode = old_rel->relfilenode;

	/* used only for logging and error reporting, old/new are identical */
	map->nspname = old_rel->nspname;
	map->relname = old_rel->relname;
}


/*
 * Complain about a relation we couldn't match to the other database,
 * identifying it as best we can.
 */
static void
report_unmatched_relation(const RelInfo *rel, const DbInfo *db, bool is_new_db)
{
	Oid			reloid = rel->reloid;	/* we might change rel below */
	char		reldesc[1000];
	int			i;

	snprintf(reldesc, sizeof(reldesc), "\"%s.%s\"",
			 rel->nspname, rel->relname);
	if (rel->indtable)
	{
		for (i = 0; i < db->rel_arr.nrels; i++)
		{
			const RelInfo *hrel = &db->rel_arr.rels[i];

			if (hrel->reloid == rel->indtable)
			{
				snprintf(reldesc + strlen(reldesc),
						 sizeof(reldesc) - strlen(reldesc),
						 _(" which is an index on \"%s.%s\""),
						 hrel->nspname, hrel->relname);
				/* Shift attention to index's table for toast check */
				rel = hrel;
				break;
			}
		}
		if (i >= db->rel_arr.nrels)
			snprintf(reldesc + strlen(reldesc),
					 sizeof(reldesc) - strlen(reldesc),
					 _(" which is an index on OID %u"), rel->indtable);
	}
	if (rel->toastheap)
	{
		for (i = 0; i < db->rel_arr.nrels; i++)
		{
			const RelInfo *brel = &db->rel_arr.rels[i];

			if (brel->reloid == rel->toastheap)
			{
				snprintf(reldesc + strlen(reldesc),
						 sizeof(reldesc) - strlen(reldesc),
						 _(" which is the TOAST table for \"%s.%s\""),
						 brel->nspname, brel->relname);
				break;
			}
		}
		if (i >= db->rel_arr.nrels)
			snprintf(reldesc + strlen(reldesc),
					 sizeof(reldesc) - strlen(reldesc),
					 _(" which is the TOAST table for OID %u"), rel->toastheap);
	}

	if (is_new_db)
		pg_log(PG_WARNING, "No match found in old cluster for new relation with OID %u in database \"%s\": %s\n",
			   reloid, db->db_name, reldesc);
	else
		pg_log(PG_WARNING, "No match found in new cluster for old relation with OID %u in database \"%s\": %s\n",
			   reloid, db->db_name, reldesc);
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

	if (cluster == &old_cluster)
		pg_log(PG_VERBOSE, "\nsource databases:\n");
	else
		pg_log(PG_VERBOSE, "\ntarget databases:\n");

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
				i_encoding,
				i_datcollate,
				i_datctype,
				i_datlocprovider,
				i_daticulocale,
				i_spclocation;
	char		query[QUERY_ALLOC];

	snprintf(query, sizeof(query),
			 "SELECT d.oid, d.datname, d.encoding, d.datcollate, d.datctype, ");
	if (GET_MAJOR_VERSION(cluster->major_version) < 1500)
		snprintf(query + strlen(query), sizeof(query) - strlen(query),
				 "'c' AS datlocprovider, NULL AS daticulocale, ");
	else
		snprintf(query + strlen(query), sizeof(query) - strlen(query),
				 "datlocprovider, daticulocale, ");
	snprintf(query + strlen(query), sizeof(query) - strlen(query),
			 "pg_catalog.pg_tablespace_location(t.oid) AS spclocation "
			 "FROM pg_catalog.pg_database d "
			 " LEFT OUTER JOIN pg_catalog.pg_tablespace t "
			 " ON d.dattablespace = t.oid "
			 "WHERE d.datallowconn = true "
			 "ORDER BY 1");

	res = executeQueryOrDie(conn, "%s", query);

	i_oid = PQfnumber(res, "oid");
	i_datname = PQfnumber(res, "datname");
	i_encoding = PQfnumber(res, "encoding");
	i_datcollate = PQfnumber(res, "datcollate");
	i_datctype = PQfnumber(res, "datctype");
	i_datlocprovider = PQfnumber(res, "datlocprovider");
	i_daticulocale = PQfnumber(res, "daticulocale");
	i_spclocation = PQfnumber(res, "spclocation");

	ntups = PQntuples(res);
	dbinfos = (DbInfo *) pg_malloc(sizeof(DbInfo) * ntups);

	for (tupnum = 0; tupnum < ntups; tupnum++)
	{
		dbinfos[tupnum].db_oid = atooid(PQgetvalue(res, tupnum, i_oid));
		dbinfos[tupnum].db_name = pg_strdup(PQgetvalue(res, tupnum, i_datname));
		dbinfos[tupnum].db_encoding = atoi(PQgetvalue(res, tupnum, i_encoding));
		dbinfos[tupnum].db_collate = pg_strdup(PQgetvalue(res, tupnum, i_datcollate));
		dbinfos[tupnum].db_ctype = pg_strdup(PQgetvalue(res, tupnum, i_datctype));
		dbinfos[tupnum].db_collprovider = PQgetvalue(res, tupnum, i_datlocprovider)[0];
		if (PQgetisnull(res, tupnum, i_daticulocale))
			dbinfos[tupnum].db_iculocale = NULL;
		else
			dbinfos[tupnum].db_iculocale = pg_strdup(PQgetvalue(res, tupnum, i_daticulocale));
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
 * gets the relinfos for all the user tables and indexes of the database
 * referred to by "dbinfo".
 *
 * Note: the resulting RelInfo array is assumed to be sorted by OID.
 * This allows later processing to match up old and new databases efficiently.
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
				i_reloid,
				i_indtable,
				i_toastheap,
				i_relfilenode,
				i_reltablespace;
	char		query[QUERY_ALLOC];
	char	   *last_namespace = NULL,
			   *last_tablespace = NULL;

	query[0] = '\0';			/* initialize query string to empty */

	/*
	 * Create a CTE that collects OIDs of regular user tables and matviews,
	 * but excluding toast tables and indexes.  We assume that relations with
	 * OIDs >= FirstNormalObjectId belong to the user.  (That's probably
	 * redundant with the namespace-name exclusions, but let's be safe.)
	 *
	 * pg_largeobject contains user data that does not appear in pg_dump
	 * output, so we have to copy that system table.  It's easiest to do that
	 * by treating it as a user table.
	 */
	snprintf(query + strlen(query), sizeof(query) - strlen(query),
			 "WITH regular_heap (reloid, indtable, toastheap) AS ( "
			 "  SELECT c.oid, 0::oid, 0::oid "
			 "  FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n "
			 "         ON c.relnamespace = n.oid "
			 "  WHERE relkind IN (" CppAsString2(RELKIND_RELATION) ", "
			 CppAsString2(RELKIND_MATVIEW) ") AND "
	/* exclude possible orphaned temp tables */
			 "    ((n.nspname !~ '^pg_temp_' AND "
			 "      n.nspname !~ '^pg_toast_temp_' AND "
			 "      n.nspname NOT IN ('pg_catalog', 'information_schema', "
			 "                        'binary_upgrade', 'pg_toast') AND "
			 "      c.oid >= %u::pg_catalog.oid) OR "
			 "     (n.nspname = 'pg_catalog' AND "
			 "      relname IN ('pg_largeobject') ))), ",
			 FirstNormalObjectId);

	/*
	 * Add a CTE that collects OIDs of toast tables belonging to the tables
	 * selected by the regular_heap CTE.  (We have to do this separately
	 * because the namespace-name rules above don't work for toast tables.)
	 */
	snprintf(query + strlen(query), sizeof(query) - strlen(query),
			 "  toast_heap (reloid, indtable, toastheap) AS ( "
			 "  SELECT c.reltoastrelid, 0::oid, c.oid "
			 "  FROM regular_heap JOIN pg_catalog.pg_class c "
			 "      ON regular_heap.reloid = c.oid "
			 "  WHERE c.reltoastrelid != 0), ");

	/*
	 * Add a CTE that collects OIDs of all valid indexes on the previously
	 * selected tables.  We can ignore invalid indexes since pg_dump does.
	 * Testing indisready is necessary in 9.2, and harmless in earlier/later
	 * versions.
	 */
	snprintf(query + strlen(query), sizeof(query) - strlen(query),
			 "  all_index (reloid, indtable, toastheap) AS ( "
			 "  SELECT indexrelid, indrelid, 0::oid "
			 "  FROM pg_catalog.pg_index "
			 "  WHERE indisvalid AND indisready "
			 "    AND indrelid IN "
			 "        (SELECT reloid FROM regular_heap "
			 "         UNION ALL "
			 "         SELECT reloid FROM toast_heap)) ");

	/*
	 * And now we can write the query that retrieves the data we want for each
	 * heap and index relation.  Make sure result is sorted by OID.
	 */
	snprintf(query + strlen(query), sizeof(query) - strlen(query),
			 "SELECT all_rels.*, n.nspname, c.relname, "
			 "  c.relfilenode, c.reltablespace, "
			 "  pg_catalog.pg_tablespace_location(t.oid) AS spclocation "
			 "FROM (SELECT * FROM regular_heap "
			 "      UNION ALL "
			 "      SELECT * FROM toast_heap "
			 "      UNION ALL "
			 "      SELECT * FROM all_index) all_rels "
			 "  JOIN pg_catalog.pg_class c "
			 "      ON all_rels.reloid = c.oid "
			 "  JOIN pg_catalog.pg_namespace n "
			 "     ON c.relnamespace = n.oid "
			 "  LEFT OUTER JOIN pg_catalog.pg_tablespace t "
			 "     ON c.reltablespace = t.oid "
			 "ORDER BY 1;");

	res = executeQueryOrDie(conn, "%s", query);

	ntups = PQntuples(res);

	relinfos = (RelInfo *) pg_malloc(sizeof(RelInfo) * ntups);

	i_reloid = PQfnumber(res, "reloid");
	i_indtable = PQfnumber(res, "indtable");
	i_toastheap = PQfnumber(res, "toastheap");
	i_nspname = PQfnumber(res, "nspname");
	i_relname = PQfnumber(res, "relname");
	i_relfilenode = PQfnumber(res, "relfilenode");
	i_reltablespace = PQfnumber(res, "reltablespace");
	i_spclocation = PQfnumber(res, "spclocation");

	for (relnum = 0; relnum < ntups; relnum++)
	{
		RelInfo    *curr = &relinfos[num_rels++];

		curr->reloid = atooid(PQgetvalue(res, relnum, i_reloid));
		curr->indtable = atooid(PQgetvalue(res, relnum, i_indtable));
		curr->toastheap = atooid(PQgetvalue(res, relnum, i_toastheap));

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

		/* Is the tablespace oid non-default? */
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
