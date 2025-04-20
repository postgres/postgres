/*
 *	relfilenumber.c
 *
 *	relfilenumber functions
 *
 *	Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/relfilenumber.c
 */

#include "postgres_fe.h"

#include <sys/stat.h>

#include "common/file_perm.h"
#include "common/file_utils.h"
#include "common/int.h"
#include "common/logging.h"
#include "pg_upgrade.h"

static void transfer_single_new_db(FileNameMap *maps, int size, char *old_tablespace);
static void transfer_relfile(FileNameMap *map, const char *type_suffix, bool vm_must_add_frozenbit);

/*
 * The following set of sync_queue_* functions are used for --swap to reduce
 * the amount of time spent synchronizing the swapped catalog files.  When a
 * file is added to the queue, we also alert the file system that we'd like it
 * to be persisted to disk in the near future (if that operation is supported
 * by the current platform).  Once the queue is full, all of the files are
 * synchronized to disk.  This strategy should generally be much faster than
 * simply calling fsync() on the files right away.
 *
 * The general usage pattern should be something like:
 *
 *     for (int i = 0; i < num_files; i++)
 *         sync_queue_push(files[i]);
 *
 *     // be sure to sync any remaining files in the queue
 *     sync_queue_sync_all();
 *     sync_queue_destroy();
 */

#define SYNC_QUEUE_MAX_LEN	(1024)

static char *sync_queue[SYNC_QUEUE_MAX_LEN];
static bool sync_queue_inited;
static int	sync_queue_len;

static inline void
sync_queue_init(void)
{
	if (sync_queue_inited)
		return;

	sync_queue_inited = true;
	for (int i = 0; i < SYNC_QUEUE_MAX_LEN; i++)
		sync_queue[i] = palloc(MAXPGPATH);
}

static inline void
sync_queue_sync_all(void)
{
	if (!sync_queue_inited)
		return;

	for (int i = 0; i < sync_queue_len; i++)
	{
		if (fsync_fname(sync_queue[i], false) != 0)
			pg_fatal("could not synchronize file \"%s\": %m", sync_queue[i]);
	}

	sync_queue_len = 0;
}

static inline void
sync_queue_push(const char *fname)
{
	sync_queue_init();

	pre_sync_fname(fname, false);

	strncpy(sync_queue[sync_queue_len++], fname, MAXPGPATH);
	if (sync_queue_len >= SYNC_QUEUE_MAX_LEN)
		sync_queue_sync_all();
}

static inline void
sync_queue_destroy(void)
{
	if (!sync_queue_inited)
		return;

	sync_queue_inited = false;
	sync_queue_len = 0;
	for (int i = 0; i < SYNC_QUEUE_MAX_LEN; i++)
	{
		pfree(sync_queue[i]);
		sync_queue[i] = NULL;
	}
}

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
	switch (user_opts.transfer_mode)
	{
		case TRANSFER_MODE_CLONE:
			prep_status_progress("Cloning user relation files");
			break;
		case TRANSFER_MODE_COPY:
			prep_status_progress("Copying user relation files");
			break;
		case TRANSFER_MODE_COPY_FILE_RANGE:
			prep_status_progress("Copying user relation files with copy_file_range");
			break;
		case TRANSFER_MODE_LINK:
			prep_status_progress("Linking user relation files");
			break;
		case TRANSFER_MODE_SWAP:
			prep_status_progress("Swapping data directories");
			break;
	}

	/*
	 * Transferring files by tablespace is tricky because a single database
	 * can use multiple tablespaces.  For non-parallel mode, we just pass a
	 * NULL tablespace path, which matches all tablespaces.  In parallel mode,
	 * we pass the default tablespace and all user-created tablespaces and let
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
			pg_fatal("old database \"%s\" not found in the new cluster",
					 old_db->db_name);

		mappings = gen_db_file_maps(old_db, new_db, &n_maps, old_pgdata,
									new_pgdata);
		if (n_maps)
		{
			transfer_single_new_db(mappings, n_maps, old_tablespace);
		}
		/* We allocate something even for n_maps == 0 */
		pg_free(mappings);
	}

	/*
	 * Make sure anything pending synchronization in swap mode is fully
	 * persisted to disk.  This is a no-op for other transfer modes.
	 */
	sync_queue_sync_all();
	sync_queue_destroy();
}

/*
 * prepare_for_swap()
 *
 * This function moves the database directory from the old cluster to the new
 * cluster in preparation for moving the pg_restore-generated catalog files
 * into place.  Returns false if the database with the given OID does not have
 * a directory in the given tablespace, otherwise returns true.
 *
 * This function will return paths in the following variables, which the caller
 * must ensure are sized to MAXPGPATH bytes:
 *
 *	old_catalog_dir: The directory for the old cluster's catalog files.
 *	new_db_dir: The new cluster's database directory for db_oid.
 *	moved_db_dir: Destination for the pg_restore-generated database directory.
 */
static bool
prepare_for_swap(const char *old_tablespace, Oid db_oid,
				 char *old_catalog_dir, char *new_db_dir, char *moved_db_dir)
{
	const char *new_tablespace;
	const char *old_tblspc_suffix;
	const char *new_tblspc_suffix;
	char		old_tblspc[MAXPGPATH];
	char		new_tblspc[MAXPGPATH];
	char		moved_tblspc[MAXPGPATH];
	char		old_db_dir[MAXPGPATH];
	struct stat st;

	if (strcmp(old_tablespace, old_cluster.pgdata) == 0)
	{
		new_tablespace = new_cluster.pgdata;
		new_tblspc_suffix = "/base";
		old_tblspc_suffix = "/base";
	}
	else
	{
		/*
		 * XXX: The below line is a hack to deal with the fact that we
		 * presently don't have an easy way to find the corresponding new
		 * tablespace's path.  This will need to be fixed if/when we add
		 * pg_upgrade support for in-place tablespaces.
		 */
		new_tablespace = old_tablespace;

		new_tblspc_suffix = new_cluster.tablespace_suffix;
		old_tblspc_suffix = old_cluster.tablespace_suffix;
	}

	/* Old and new cluster paths. */
	snprintf(old_tblspc, sizeof(old_tblspc), "%s%s", old_tablespace, old_tblspc_suffix);
	snprintf(new_tblspc, sizeof(new_tblspc), "%s%s", new_tablespace, new_tblspc_suffix);
	snprintf(old_db_dir, sizeof(old_db_dir), "%s/%u", old_tblspc, db_oid);
	snprintf(new_db_dir, MAXPGPATH, "%s/%u", new_tblspc, db_oid);

	/*
	 * Paths for "moved aside" stuff.  We intentionally put these in the old
	 * cluster so that the delete_old_cluster.{sh,bat} script handles them.
	 */
	snprintf(moved_tblspc, sizeof(moved_tblspc), "%s/moved_for_upgrade", old_tblspc);
	snprintf(old_catalog_dir, MAXPGPATH, "%s/%u_old_catalogs", moved_tblspc, db_oid);
	snprintf(moved_db_dir, MAXPGPATH, "%s/%u", moved_tblspc, db_oid);

	/* Check that the database directory exists in the given tablespace. */
	if (stat(old_db_dir, &st) != 0)
	{
		if (errno != ENOENT)
			pg_fatal("could not stat file \"%s\": %m", old_db_dir);
		return false;
	}

	/* Create directory for stuff that is moved aside. */
	if (pg_mkdir_p(moved_tblspc, pg_dir_create_mode) != 0 && errno != EEXIST)
		pg_fatal("could not create directory \"%s\"", moved_tblspc);

	/* Create directory for old catalog files. */
	if (pg_mkdir_p(old_catalog_dir, pg_dir_create_mode) != 0)
		pg_fatal("could not create directory \"%s\"", old_catalog_dir);

	/* Move the new cluster's database directory aside. */
	if (rename(new_db_dir, moved_db_dir) != 0)
		pg_fatal("could not rename \"%s\" to \"%s\"", new_db_dir, moved_db_dir);

	/* Move the old cluster's database directory into place. */
	if (rename(old_db_dir, new_db_dir) != 0)
		pg_fatal("could not rename \"%s\" to \"%s\"", old_db_dir, new_db_dir);

	return true;
}

/*
 * FileNameMapCmp()
 *
 * qsort() comparator for FileNameMap that sorts by RelFileNumber.
 */
static int
FileNameMapCmp(const void *a, const void *b)
{
	const FileNameMap *map1 = (const FileNameMap *) a;
	const FileNameMap *map2 = (const FileNameMap *) b;

	return pg_cmp_u32(map1->relfilenumber, map2->relfilenumber);
}

/*
 * parse_relfilenumber()
 *
 * Attempt to parse the RelFileNumber of the given file name.  If we can't,
 * return InvalidRelFileNumber.  Note that this code snippet is lifted from
 * parse_filename_for_nontemp_relation().
 */
static RelFileNumber
parse_relfilenumber(const char *filename)
{
	char	   *endp;
	unsigned long n;

	if (filename[0] < '1' || filename[0] > '9')
		return InvalidRelFileNumber;

	errno = 0;
	n = strtoul(filename, &endp, 10);
	if (errno || filename == endp || n <= 0 || n > PG_UINT32_MAX)
		return InvalidRelFileNumber;

	return (RelFileNumber) n;
}

/*
 * swap_catalog_files()
 *
 * Moves the old catalog files aside, and moves the new catalog files into
 * place.  prepare_for_swap() should have already been called (and returned
 * true) for the tablespace/database being transferred.
 *
 * The arguments for the following parameters should be the corresponding
 * variables returned by prepare_for_swap():
 *
 *	old_catalog_dir: The directory for the old cluster's catalog files.
 *	new_db_dir: New cluster's database directory (for DB being transferred).
 *	moved_db_dir: Moved-aside pg_restore-generated database directory.
 */
static void
swap_catalog_files(FileNameMap *maps, int size, const char *old_catalog_dir,
				   const char *new_db_dir, const char *moved_db_dir)
{
	DIR		   *dir;
	struct dirent *de;
	char		path[MAXPGPATH];
	char		dest[MAXPGPATH];
	RelFileNumber rfn;

	/* Move the old catalog files aside. */
	dir = opendir(new_db_dir);
	if (dir == NULL)
		pg_fatal("could not open directory \"%s\": %m", new_db_dir);
	while (errno = 0, (de = readdir(dir)) != NULL)
	{
		snprintf(path, sizeof(path), "%s/%s", new_db_dir, de->d_name);
		if (get_dirent_type(path, de, false, PG_LOG_ERROR) != PGFILETYPE_REG)
			continue;

		rfn = parse_relfilenumber(de->d_name);
		if (RelFileNumberIsValid(rfn))
		{
			FileNameMap key = {.relfilenumber = rfn};

			if (bsearch(&key, maps, size, sizeof(FileNameMap), FileNameMapCmp))
				continue;
		}

		snprintf(dest, sizeof(dest), "%s/%s", old_catalog_dir, de->d_name);
		if (rename(path, dest) != 0)
			pg_fatal("could not rename \"%s\" to \"%s\": %m", path, dest);
	}
	if (errno)
		pg_fatal("could not read directory \"%s\": %m", new_db_dir);
	(void) closedir(dir);

	/* Move the new catalog files into place. */
	dir = opendir(moved_db_dir);
	if (dir == NULL)
		pg_fatal("could not open directory \"%s\": %m", moved_db_dir);
	while (errno = 0, (de = readdir(dir)) != NULL)
	{
		snprintf(path, sizeof(path), "%s/%s", moved_db_dir, de->d_name);
		if (get_dirent_type(path, de, false, PG_LOG_ERROR) != PGFILETYPE_REG)
			continue;

		rfn = parse_relfilenumber(de->d_name);
		if (RelFileNumberIsValid(rfn))
		{
			FileNameMap key = {.relfilenumber = rfn};

			if (bsearch(&key, maps, size, sizeof(FileNameMap), FileNameMapCmp))
				continue;
		}

		snprintf(dest, sizeof(dest), "%s/%s", new_db_dir, de->d_name);
		if (rename(path, dest) != 0)
			pg_fatal("could not rename \"%s\" to \"%s\": %m", path, dest);

		/*
		 * We don't fsync() the database files in the file synchronization
		 * stage of pg_upgrade in swap mode, so we need to synchronize them
		 * ourselves.  We only do this for the catalog files because they were
		 * created during pg_restore with fsync=off.  We assume that the user
		 * data files were properly persisted to disk when the user last shut
		 * it down.
		 */
		if (user_opts.do_sync)
			sync_queue_push(dest);
	}
	if (errno)
		pg_fatal("could not read directory \"%s\": %m", moved_db_dir);
	(void) closedir(dir);

	/* Ensure the directory entries are persisted to disk. */
	if (fsync_fname(new_db_dir, true) != 0)
		pg_fatal("could not synchronize directory \"%s\": %m", new_db_dir);
	if (fsync_parent_path(new_db_dir) != 0)
		pg_fatal("could not synchronize parent directory of \"%s\": %m", new_db_dir);
}

/*
 * do_swap()
 *
 * Perform the required steps for --swap for a single database.  In short this
 * moves the old cluster's database directory into the new cluster and then
 * replaces any files for system catalogs with the ones that were generated
 * during pg_restore.
 */
static void
do_swap(FileNameMap *maps, int size, char *old_tablespace)
{
	char		old_catalog_dir[MAXPGPATH];
	char		new_db_dir[MAXPGPATH];
	char		moved_db_dir[MAXPGPATH];

	/*
	 * We perform many lookups on maps by relfilenumber in swap mode, so make
	 * sure it's sorted by relfilenumber.  maps should already be sorted by
	 * OID, so in general this shouldn't have much work to do.
	 */
	qsort(maps, size, sizeof(FileNameMap), FileNameMapCmp);

	/*
	 * If an old tablespace is given, we only need to process that one.  If no
	 * old tablespace is specified, we need to process all the tablespaces on
	 * the system.
	 */
	if (old_tablespace)
	{
		if (prepare_for_swap(old_tablespace, maps[0].db_oid,
							 old_catalog_dir, new_db_dir, moved_db_dir))
			swap_catalog_files(maps, size,
							   old_catalog_dir, new_db_dir, moved_db_dir);
	}
	else
	{
		if (prepare_for_swap(old_cluster.pgdata, maps[0].db_oid,
							 old_catalog_dir, new_db_dir, moved_db_dir))
			swap_catalog_files(maps, size,
							   old_catalog_dir, new_db_dir, moved_db_dir);

		for (int tblnum = 0; tblnum < os_info.num_old_tablespaces; tblnum++)
		{
			if (prepare_for_swap(os_info.old_tablespaces[tblnum], maps[0].db_oid,
								 old_catalog_dir, new_db_dir, moved_db_dir))
				swap_catalog_files(maps, size,
								   old_catalog_dir, new_db_dir, moved_db_dir);
		}
	}
}

/*
 * transfer_single_new_db()
 *
 * create links for mappings stored in "maps" array.
 */
static void
transfer_single_new_db(FileNameMap *maps, int size, char *old_tablespace)
{
	int			mapnum;
	bool		vm_must_add_frozenbit = false;

	/*
	 * Do we need to rewrite visibilitymap?
	 */
	if (old_cluster.controldata.cat_ver < VISIBILITY_MAP_FROZEN_BIT_CAT_VER &&
		new_cluster.controldata.cat_ver >= VISIBILITY_MAP_FROZEN_BIT_CAT_VER)
		vm_must_add_frozenbit = true;

	/* --swap has its own subroutine */
	if (user_opts.transfer_mode == TRANSFER_MODE_SWAP)
	{
		/*
		 * We don't support --swap to upgrade from versions that require
		 * rewriting the visibility map.  We should've failed already if
		 * someone tries to do that.
		 */
		Assert(!vm_must_add_frozenbit);

		do_swap(maps, size, old_tablespace);
		return;
	}

	for (mapnum = 0; mapnum < size; mapnum++)
	{
		if (old_tablespace == NULL ||
			strcmp(maps[mapnum].old_tablespace, old_tablespace) == 0)
		{
			/* transfer primary file */
			transfer_relfile(&maps[mapnum], "", vm_must_add_frozenbit);

			/*
			 * Copy/link any fsm and vm files, if they exist
			 */
			transfer_relfile(&maps[mapnum], "_fsm", vm_must_add_frozenbit);
			transfer_relfile(&maps[mapnum], "_vm", vm_must_add_frozenbit);
		}
	}
}


/*
 * transfer_relfile()
 *
 * Copy or link file from old cluster to new one.  If vm_must_add_frozenbit
 * is true, visibility map forks are converted and rewritten, even in link
 * mode.
 */
static void
transfer_relfile(FileNameMap *map, const char *type_suffix, bool vm_must_add_frozenbit)
{
	char		old_file[MAXPGPATH];
	char		new_file[MAXPGPATH];
	int			segno;
	char		extent_suffix[65];
	struct stat statbuf;

	/*
	 * Now copy/link any related segments as well. Remember, PG breaks large
	 * files into 1GB segments, the first segment has no extension, subsequent
	 * segments are named relfilenumber.1, relfilenumber.2, relfilenumber.3.
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
				 map->db_oid,
				 map->relfilenumber,
				 type_suffix,
				 extent_suffix);
		snprintf(new_file, sizeof(new_file), "%s%s/%u/%u%s%s",
				 map->new_tablespace,
				 map->new_tablespace_suffix,
				 map->db_oid,
				 map->relfilenumber,
				 type_suffix,
				 extent_suffix);

		/* Is it an extent, fsm, or vm file? */
		if (type_suffix[0] != '\0' || segno != 0)
		{
			/* Did file open fail? */
			if (stat(old_file, &statbuf) != 0)
			{
				/* File does not exist?  That's OK, just return */
				if (errno == ENOENT)
					return;
				else
					pg_fatal("error while checking for file existence \"%s.%s\" (\"%s\" to \"%s\"): %m",
							 map->nspname, map->relname, old_file, new_file);
			}

			/* If file is empty, just return */
			if (statbuf.st_size == 0)
				return;
		}

		unlink(new_file);

		/* Copying files might take some time, so give feedback. */
		pg_log(PG_STATUS, "%s", old_file);

		if (vm_must_add_frozenbit && strcmp(type_suffix, "_vm") == 0)
		{
			/* Need to rewrite visibility map format */
			pg_log(PG_VERBOSE, "rewriting \"%s\" to \"%s\"",
				   old_file, new_file);
			rewriteVisibilityMap(old_file, new_file, map->nspname, map->relname);
		}
		else
			switch (user_opts.transfer_mode)
			{
				case TRANSFER_MODE_CLONE:
					pg_log(PG_VERBOSE, "cloning \"%s\" to \"%s\"",
						   old_file, new_file);
					cloneFile(old_file, new_file, map->nspname, map->relname);
					break;
				case TRANSFER_MODE_COPY:
					pg_log(PG_VERBOSE, "copying \"%s\" to \"%s\"",
						   old_file, new_file);
					copyFile(old_file, new_file, map->nspname, map->relname);
					break;
				case TRANSFER_MODE_COPY_FILE_RANGE:
					pg_log(PG_VERBOSE, "copying \"%s\" to \"%s\" with copy_file_range",
						   old_file, new_file);
					copyFileByRange(old_file, new_file, map->nspname, map->relname);
					break;
				case TRANSFER_MODE_LINK:
					pg_log(PG_VERBOSE, "linking \"%s\" to \"%s\"",
						   old_file, new_file);
					linkFile(old_file, new_file, map->nspname, map->relname);
					break;
				case TRANSFER_MODE_SWAP:
					/* swap mode is handled in its own code path */
					pg_fatal("should never happen");
					break;
			}
	}
}
