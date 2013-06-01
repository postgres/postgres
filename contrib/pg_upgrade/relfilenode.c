/*
 *	relfilenode.c
 *
 *	relfilenode functions
 *
 *	Copyright (c) 2010-2013, PostgreSQL Global Development Group
 *	contrib/pg_upgrade/relfilenode.c
 */

#include "postgres_fe.h"

#include "pg_upgrade.h"

#include "catalog/pg_class.h"
#include "access/transam.h"


static void transfer_single_new_db(pageCnvCtx *pageConverter,
					   FileNameMap *maps, int size, char *old_tablespace);
static void transfer_relfile(pageCnvCtx *pageConverter, FileNameMap *map,
				 const char *suffix);


/*
 * transfer_all_new_tablespaces()
 *
 * Responsible for upgrading all database. invokes routines to generate mappings and then
 * physically link the databases.
 */
void
transfer_all_new_tablespaces(DbInfoArr *old_db_arr, DbInfoArr *new_db_arr,
							 char *old_pgdata, char *new_pgdata)
{
	pg_log(PG_REPORT, "%s user relation files\n",
	  user_opts.transfer_mode == TRANSFER_MODE_LINK ? "Linking" : "Copying");

	/*
	 * Transfering files by tablespace is tricky because a single database can
	 * use multiple tablespaces.  For non-parallel mode, we just pass a NULL
	 * tablespace path, which matches all tablespaces.	In parallel mode, we
	 * pass the default tablespace and all user-created tablespaces and let
	 * those operations happen in parallel.
	 */
	if (user_opts.jobs <= 1)
		parallel_transfer_all_new_dbs(old_db_arr, new_db_arr, old_pgdata,
									  new_pgdata, NULL);
	else
	{
		int			tblnum;

		/* transfer default tablespace */
		parallel_transfer_all_new_dbs(old_db_arr, new_db_arr, old_pgdata,
									  new_pgdata, old_pgdata);

		for (tblnum = 0; tblnum < os_info.num_old_tablespaces; tblnum++)
			parallel_transfer_all_new_dbs(old_db_arr,
										  new_db_arr,
										  old_pgdata,
										  new_pgdata,
										  os_info.old_tablespaces[tblnum]);
		/* reap all children */
		while (reap_child(true) == true)
			;
	}

	end_progress_output();
	check_ok();

	return;
}


/*
 * transfer_all_new_dbs()
 *
 * Responsible for upgrading all database. invokes routines to generate mappings and then
 * physically link the databases.
 */
void
transfer_all_new_dbs(DbInfoArr *old_db_arr, DbInfoArr *new_db_arr,
					 char *old_pgdata, char *new_pgdata, char *old_tablespace)
{
	int			old_dbnum,
				new_dbnum;

	/* Scan the old cluster databases and transfer their files */
	for (old_dbnum = new_dbnum = 0;
		 old_dbnum < old_db_arr->ndbs;
		 old_dbnum++, new_dbnum++)
	{
		DbInfo	   *old_db = &old_db_arr->dbs[old_dbnum],
				   *new_db = NULL;
		FileNameMap *mappings;
		int			n_maps;
		pageCnvCtx *pageConverter = NULL;

		/*
		 * Advance past any databases that exist in the new cluster but not in
		 * the old, e.g. "postgres".  (The user might have removed the
		 * 'postgres' database from the old cluster.)
		 */
		for (; new_dbnum < new_db_arr->ndbs; new_dbnum++)
		{
			new_db = &new_db_arr->dbs[new_dbnum];
			if (strcmp(old_db->db_name, new_db->db_name) == 0)
				break;
		}

		if (new_dbnum >= new_db_arr->ndbs)
			pg_log(PG_FATAL, "old database \"%s\" not found in the new cluster\n",
				   old_db->db_name);

		n_maps = 0;
		mappings = gen_db_file_maps(old_db, new_db, &n_maps, old_pgdata,
									new_pgdata);

		if (n_maps)
		{
			print_maps(mappings, n_maps, new_db->db_name);

#ifdef PAGE_CONVERSION
			pageConverter = setupPageConverter();
#endif
			transfer_single_new_db(pageConverter, mappings, n_maps,
								   old_tablespace);

			pg_free(mappings);
		}
	}

	return;
}


/*
 * get_pg_database_relfilenode()
 *
 *	Retrieves the relfilenode for a few system-catalog tables.	We need these
 *	relfilenodes later in the upgrade process.
 */
void
get_pg_database_relfilenode(ClusterInfo *cluster)
{
	PGconn	   *conn = connectToServer(cluster, "template1");
	PGresult   *res;
	int			i_relfile;

	res = executeQueryOrDie(conn,
							"SELECT c.relname, c.relfilenode "
							"FROM	pg_catalog.pg_class c, "
							"		pg_catalog.pg_namespace n "
							"WHERE	c.relnamespace = n.oid AND "
							"		n.nspname = 'pg_catalog' AND "
							"		c.relname = 'pg_database' "
							"ORDER BY c.relname");

	i_relfile = PQfnumber(res, "relfilenode");
	cluster->pg_database_oid = atooid(PQgetvalue(res, 0, i_relfile));

	PQclear(res);
	PQfinish(conn);
}


/*
 * transfer_single_new_db()
 *
 * create links for mappings stored in "maps" array.
 */
static void
transfer_single_new_db(pageCnvCtx *pageConverter,
					   FileNameMap *maps, int size, char *old_tablespace)
{
	int			mapnum;
	bool		vm_crashsafe_match = true;

	/*
	 * Do the old and new cluster disagree on the crash-safetiness of the vm
	 * files?  If so, do not copy them.
	 */
	if (old_cluster.controldata.cat_ver < VISIBILITY_MAP_CRASHSAFE_CAT_VER &&
		new_cluster.controldata.cat_ver >= VISIBILITY_MAP_CRASHSAFE_CAT_VER)
		vm_crashsafe_match = false;

	for (mapnum = 0; mapnum < size; mapnum++)
	{
		if (old_tablespace == NULL ||
			strcmp(maps[mapnum].old_tablespace, old_tablespace) == 0)
		{
			/* transfer primary file */
			transfer_relfile(pageConverter, &maps[mapnum], "");

			/* fsm/vm files added in PG 8.4 */
			if (GET_MAJOR_VERSION(old_cluster.major_version) >= 804)
			{
				/*
				 * Copy/link any fsm and vm files, if they exist
				 */
				transfer_relfile(pageConverter, &maps[mapnum], "_fsm");
				if (vm_crashsafe_match)
					transfer_relfile(pageConverter, &maps[mapnum], "_vm");
			}
		}
	}
}


/*
 * transfer_relfile()
 *
 * Copy or link file from old cluster to new one.
 */
static void
transfer_relfile(pageCnvCtx *pageConverter, FileNameMap *map,
				 const char *type_suffix)
{
	const char *msg;
	char		old_file[MAXPGPATH];
	char		new_file[MAXPGPATH];
	int			fd;
	int			segno;
	char		extent_suffix[65];

	/*
	 * Now copy/link any related segments as well. Remember, PG breaks large
	 * files into 1GB segments, the first segment has no extension, subsequent
	 * segments are named relfilenode.1, relfilenode.2, relfilenode.3. copied.
	 */
	for (segno = 0;; segno++)
	{
		if (segno == 0)
			extent_suffix[0] = '\0';
		else
			snprintf(extent_suffix, sizeof(extent_suffix), ".%d", segno);

		snprintf(old_file, sizeof(old_file), "%s%s/%u/%u%s%s",
				 map->old_tablespace,
				 map->old_tablespace_suffix,
				 map->old_db_oid,
				 map->old_relfilenode,
				 type_suffix,
				 extent_suffix);
		snprintf(new_file, sizeof(new_file), "%s%s/%u/%u%s%s",
				 map->new_tablespace,
				 map->new_tablespace_suffix,
				 map->new_db_oid,
				 map->new_relfilenode,
				 type_suffix,
				 extent_suffix);

		/* Is it an extent, fsm, or vm file? */
		if (type_suffix[0] != '\0' || segno != 0)
		{
			/* Did file open fail? */
			if ((fd = open(old_file, O_RDONLY, 0)) == -1)
			{
				/* File does not exist?  That's OK, just return */
				if (errno == ENOENT)
					return;
				else
					pg_log(PG_FATAL, "error while checking for file existence \"%s.%s\" (\"%s\" to \"%s\"): %s\n",
						   map->nspname, map->relname, old_file, new_file,
						   getErrorText(errno));
			}
			close(fd);
		}

		unlink(new_file);

		/* Copying files might take some time, so give feedback. */
		pg_log(PG_STATUS, "%s", old_file);

		if ((user_opts.transfer_mode == TRANSFER_MODE_LINK) && (pageConverter != NULL))
			pg_log(PG_FATAL, "This upgrade requires page-by-page conversion, "
				   "you must use copy mode instead of link mode.\n");

		if (user_opts.transfer_mode == TRANSFER_MODE_COPY)
		{
			pg_log(PG_VERBOSE, "copying \"%s\" to \"%s\"\n", old_file, new_file);

			if ((msg = copyAndUpdateFile(pageConverter, old_file, new_file, true)) != NULL)
				pg_log(PG_FATAL, "error while copying relation \"%s.%s\" (\"%s\" to \"%s\"): %s\n",
					   map->nspname, map->relname, old_file, new_file, msg);
		}
		else
		{
			pg_log(PG_VERBOSE, "linking \"%s\" to \"%s\"\n", old_file, new_file);

			if ((msg = linkAndUpdateFile(pageConverter, old_file, new_file)) != NULL)
				pg_log(PG_FATAL,
					   "error while creating link for relation \"%s.%s\" (\"%s\" to \"%s\"): %s\n",
					   map->nspname, map->relname, old_file, new_file, msg);
		}
	}

	return;
}
