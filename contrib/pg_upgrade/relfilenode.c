/*
 *	relfilenode.c
 *
 *	relfilenode functions
 *
 *	Copyright (c) 2010-2012, PostgreSQL Global Development Group
 *	contrib/pg_upgrade/relfilenode.c
 */

#include "postgres.h"

#include "pg_upgrade.h"

#include "catalog/pg_class.h"
#include "access/transam.h"


static void transfer_single_new_db(pageCnvCtx *pageConverter,
					   FileNameMap *maps, int size);
static void transfer_relfile(pageCnvCtx *pageConverter,
				 const char *fromfile, const char *tofile,
				 const char *nspname, const char *relname);


/*
 * transfer_all_new_dbs()
 *
 * Responsible for upgrading all database. invokes routines to generate mappings and then
 * physically link the databases.
 */
const char *
transfer_all_new_dbs(DbInfoArr *old_db_arr,
				   DbInfoArr *new_db_arr, char *old_pgdata, char *new_pgdata)
{
	int			old_dbnum,
				new_dbnum;
	const char *msg = NULL;

	pg_log(PG_REPORT, "%s user relation files\n",
	  user_opts.transfer_mode == TRANSFER_MODE_LINK ? "Linking" : "Copying");

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
			msg = setupPageConverter(&pageConverter);
#endif
			transfer_single_new_db(pageConverter, mappings, n_maps);

			pg_free(mappings);
		}
	}

	prep_status(" ");			/* in case nothing printed; pass a space so
								 * gcc doesn't complain about empty format
								 * string */
	check_ok();

	return msg;
}


/*
 * get_pg_database_relfilenode()
 *
 *	Retrieves the relfilenode for a few system-catalog tables.  We need these
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
					   FileNameMap *maps, int size)
{
	char		old_dir[MAXPGPATH];
	char		file_pattern[MAXPGPATH];
	char		**namelist = NULL;
	int			numFiles = 0;
	int			mapnum;
	int			fileno;
	bool		vm_crashsafe_change = false;

	old_dir[0] = '\0';

	/* Do not copy non-crashsafe vm files for binaries that assume crashsafety */
	if (old_cluster.controldata.cat_ver < VISIBILITY_MAP_CRASHSAFE_CAT_VER &&
		new_cluster.controldata.cat_ver >= VISIBILITY_MAP_CRASHSAFE_CAT_VER)
		vm_crashsafe_change = true;

	for (mapnum = 0; mapnum < size; mapnum++)
	{
		char		old_file[MAXPGPATH];
		char		new_file[MAXPGPATH];

		/* Changed tablespaces?  Need a new directory scan? */
		if (strcmp(maps[mapnum].old_dir, old_dir) != 0)
		{
			if (numFiles > 0)
			{
				for (fileno = 0; fileno < numFiles; fileno++)
					pg_free(namelist[fileno]);
				pg_free(namelist);
			}

			snprintf(old_dir, sizeof(old_dir), "%s", maps[mapnum].old_dir);
			numFiles = load_directory(old_dir, &namelist);
		}

		/* Copying files might take some time, so give feedback. */

		snprintf(old_file, sizeof(old_file), "%s/%u", maps[mapnum].old_dir,
				 maps[mapnum].old_relfilenode);
		snprintf(new_file, sizeof(new_file), "%s/%u", maps[mapnum].new_dir,
				 maps[mapnum].new_relfilenode);
		pg_log(PG_REPORT, OVERWRITE_MESSAGE, old_file);

		/*
		 * Copy/link the relation's primary file (segment 0 of main fork)
		 * to the new cluster
		 */
		unlink(new_file);
		transfer_relfile(pageConverter, old_file, new_file,
						 maps[mapnum].nspname, maps[mapnum].relname);

		/* fsm/vm files added in PG 8.4 */
		if (GET_MAJOR_VERSION(old_cluster.major_version) >= 804)
		{
			/*
			 * Copy/link any fsm and vm files, if they exist
			 */
			snprintf(file_pattern, sizeof(file_pattern), "%u_",
					 maps[mapnum].old_relfilenode);

			for (fileno = 0; fileno < numFiles; fileno++)
			{
				char	   *vm_offset = strstr(namelist[fileno], "_vm");
				bool		is_vm_file = false;

				/* Is a visibility map file? (name ends with _vm) */
				if (vm_offset && strlen(vm_offset) == strlen("_vm"))
					is_vm_file = true;

				if (strncmp(namelist[fileno], file_pattern,
							strlen(file_pattern)) == 0 &&
					(!is_vm_file || !vm_crashsafe_change))
				{
					snprintf(old_file, sizeof(old_file), "%s/%s", maps[mapnum].old_dir,
							 namelist[fileno]);
					snprintf(new_file, sizeof(new_file), "%s/%u%s", maps[mapnum].new_dir,
							 maps[mapnum].new_relfilenode, strchr(namelist[fileno], '_'));

					unlink(new_file);
					transfer_relfile(pageConverter, old_file, new_file,
								 maps[mapnum].nspname, maps[mapnum].relname);
				}
			}
		}

		/*
		 * Now copy/link any related segments as well. Remember, PG breaks
		 * large files into 1GB segments, the first segment has no extension,
		 * subsequent segments are named relfilenode.1, relfilenode.2,
		 * relfilenode.3, ...  'fsm' and 'vm' files use underscores so are not
		 * copied.
		 */
		snprintf(file_pattern, sizeof(file_pattern), "%u.",
				 maps[mapnum].old_relfilenode);

		for (fileno = 0; fileno < numFiles; fileno++)
		{
			if (strncmp(namelist[fileno], file_pattern,
						strlen(file_pattern)) == 0)
			{
				snprintf(old_file, sizeof(old_file), "%s/%s", maps[mapnum].old_dir,
						 namelist[fileno]);
				snprintf(new_file, sizeof(new_file), "%s/%u%s", maps[mapnum].new_dir,
						 maps[mapnum].new_relfilenode, strchr(namelist[fileno], '.'));

				unlink(new_file);
				transfer_relfile(pageConverter, old_file, new_file,
								 maps[mapnum].nspname, maps[mapnum].relname);
			}
		}
	}

	if (numFiles > 0)
	{
		for (fileno = 0; fileno < numFiles; fileno++)
			pg_free(namelist[fileno]);
		pg_free(namelist);
	}
}


/*
 * transfer_relfile()
 *
 * Copy or link file from old cluster to new one.
 */
static void
transfer_relfile(pageCnvCtx *pageConverter, const char *old_file,
			  const char *new_file, const char *nspname, const char *relname)
{
	const char *msg;

	if ((user_opts.transfer_mode == TRANSFER_MODE_LINK) && (pageConverter != NULL))
		pg_log(PG_FATAL, "This upgrade requires page-by-page conversion, "
			   "you must use copy mode instead of link mode.\n");

	if (user_opts.transfer_mode == TRANSFER_MODE_COPY)
	{
		pg_log(PG_VERBOSE, "copying \"%s\" to \"%s\"\n", old_file, new_file);

		if ((msg = copyAndUpdateFile(pageConverter, old_file, new_file, true)) != NULL)
			pg_log(PG_FATAL, "error while copying relation \"%s.%s\" (\"%s\" to \"%s\"): %s\n",
				   nspname, relname, old_file, new_file, msg);
	}
	else
	{
		pg_log(PG_VERBOSE, "linking \"%s\" to \"%s\"\n", old_file, new_file);

		if ((msg = linkAndUpdateFile(pageConverter, old_file, new_file)) != NULL)
			pg_log(PG_FATAL,
				   "error while creating link for relation \"%s.%s\" (\"%s\" to \"%s\"): %s\n",
				   nspname, relname, old_file, new_file, msg);
	}
	return;
}
