/*-------------------------------------------------------------------------
 *
 * filemap.c
 *	  A data structure for keeping track of files that have changed.
 *
 * Copyright (c) 2013-2020, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <sys/stat.h>
#include <unistd.h>

#include "catalog/pg_tablespace_d.h"
#include "common/string.h"
#include "datapagemap.h"
#include "filemap.h"
#include "pg_rewind.h"
#include "storage/fd.h"

filemap_t  *filemap = NULL;

static bool isRelDataFile(const char *path);
static char *datasegpath(RelFileNode rnode, ForkNumber forknum,
						 BlockNumber segno);
static int	path_cmp(const void *a, const void *b);

static file_entry_t *get_filemap_entry(const char *path, bool create);
static int	final_filemap_cmp(const void *a, const void *b);
static void filemap_list_to_array(filemap_t *map);
static bool check_file_excluded(const char *path, bool is_source);

/*
 * Definition of one element part of an exclusion list, used to exclude
 * contents when rewinding.  "name" is the name of the file or path to
 * check for exclusion.  If "match_prefix" is true, any items matching
 * the name as prefix are excluded.
 */
struct exclude_list_item
{
	const char *name;
	bool		match_prefix;
};

/*
 * The contents of these directories are removed or recreated during server
 * start so they are not included in data processed by pg_rewind.
 *
 * Note: those lists should be kept in sync with what basebackup.c provides.
 * Some of the values, contrary to what basebackup.c uses, are hardcoded as
 * they are defined in backend-only headers.  So this list is maintained
 * with a best effort in mind.
 */
static const char *excludeDirContents[] =
{
	/*
	 * Skip temporary statistics files. PG_STAT_TMP_DIR must be skipped even
	 * when stats_temp_directory is set because PGSS_TEXT_FILE is always
	 * created there.
	 */
	"pg_stat_tmp",				/* defined as PG_STAT_TMP_DIR */

	/*
	 * It is generally not useful to backup the contents of this directory
	 * even if the intention is to restore to another primary. See backup.sgml
	 * for a more detailed description.
	 */
	"pg_replslot",

	/* Contents removed on startup, see dsm_cleanup_for_mmap(). */
	"pg_dynshmem",				/* defined as PG_DYNSHMEM_DIR */

	/* Contents removed on startup, see AsyncShmemInit(). */
	"pg_notify",

	/*
	 * Old contents are loaded for possible debugging but are not required for
	 * normal operation, see SerialInit().
	 */
	"pg_serial",

	/* Contents removed on startup, see DeleteAllExportedSnapshotFiles(). */
	"pg_snapshots",

	/* Contents zeroed on startup, see StartupSUBTRANS(). */
	"pg_subtrans",

	/* end of list */
	NULL
};

/*
 * List of files excluded from filemap processing.   Files are excluded
 * if their prefix match.
 */
static const struct exclude_list_item excludeFiles[] =
{
	/* Skip auto conf temporary file. */
	{"postgresql.auto.conf.tmp", false},	/* defined as PG_AUTOCONF_FILENAME */

	/* Skip current log file temporary file */
	{"current_logfiles.tmp", false},	/* defined as
										 * LOG_METAINFO_DATAFILE_TMP */

	/* Skip relation cache because it is rebuilt on startup */
	{"pg_internal.init", true}, /* defined as RELCACHE_INIT_FILENAME */

	/*
	 * If there's a backup_label or tablespace_map file, it belongs to a
	 * backup started by the user with pg_start_backup().  It is *not* correct
	 * for this backup.  Our backup_label is written later on separately.
	 */
	{"backup_label", false},	/* defined as BACKUP_LABEL_FILE */
	{"tablespace_map", false},	/* defined as TABLESPACE_MAP */

	/*
	 * If there's a backup_manifest, it belongs to a backup that was used to
	 * start this server. It is *not* correct for this backup. Our
	 * backup_manifest is injected into the backup separately if users want
	 * it.
	 */
	{"backup_manifest", false},

	{"postmaster.pid", false},
	{"postmaster.opts", false},

	/* end of list */
	{NULL, false}
};

/*
 * Create a new file map (stored in the global pointer "filemap").
 */
void
filemap_create(void)
{
	filemap_t  *map;

	map = pg_malloc(sizeof(filemap_t));
	map->first = map->last = NULL;
	map->nlist = 0;
	map->array = NULL;
	map->narray = 0;

	Assert(filemap == NULL);
	filemap = map;
}

/* Look up or create entry for 'path' */
static file_entry_t *
get_filemap_entry(const char *path, bool create)
{
	filemap_t  *map = filemap;
	file_entry_t *entry;
	file_entry_t **e;
	file_entry_t key;
	file_entry_t *key_ptr;

	if (map->array)
	{
		key.path = (char *) path;
		key_ptr = &key;
		e = bsearch(&key_ptr, map->array, map->narray, sizeof(file_entry_t *),
					path_cmp);
	}
	else
		e = NULL;

	if (e)
		entry = *e;
	else if (!create)
		entry = NULL;
	else
	{
		/* Create a new entry for this file */
		entry = pg_malloc(sizeof(file_entry_t));
		entry->path = pg_strdup(path);
		entry->isrelfile = isRelDataFile(path);
		entry->action = FILE_ACTION_UNDECIDED;

		entry->target_exists = false;
		entry->target_type = FILE_TYPE_UNDEFINED;
		entry->target_size = 0;
		entry->target_link_target = NULL;
		entry->target_pages_to_overwrite.bitmap = NULL;
		entry->target_pages_to_overwrite.bitmapsize = 0;

		entry->source_exists = false;
		entry->source_type = FILE_TYPE_UNDEFINED;
		entry->source_size = 0;
		entry->source_link_target = NULL;

		entry->next = NULL;

		if (map->last)
		{
			map->last->next = entry;
			map->last = entry;
		}
		else
			map->first = map->last = entry;
		map->nlist++;
	}

	return entry;
}

/*
 * Callback for processing source file list.
 *
 * This is called once for every file in the source server.  We record the
 * type and size of the file, so that decide_file_action() can later decide what
 * to do with it.
 */
void
process_source_file(const char *path, file_type_t type, size_t size,
					const char *link_target)
{
	file_entry_t *entry;

	Assert(filemap->array == NULL);

	/*
	 * Pretend that pg_wal is a directory, even if it's really a symlink. We
	 * don't want to mess with the symlink itself, nor complain if it's a
	 * symlink in source but not in target or vice versa.
	 */
	if (strcmp(path, "pg_wal") == 0 && type == FILE_TYPE_SYMLINK)
		type = FILE_TYPE_DIRECTORY;

	/*
	 * sanity check: a filename that looks like a data file better be a
	 * regular file
	 */
	if (type != FILE_TYPE_REGULAR && isRelDataFile(path))
		pg_fatal("data file \"%s\" in source is not a regular file", path);

	/* Remember this source file */
	entry = get_filemap_entry(path, true);
	entry->source_exists = true;
	entry->source_type = type;
	entry->source_size = size;
	entry->source_link_target = link_target ? pg_strdup(link_target) : NULL;
}

/*
 * Callback for processing target file list.
 *
 * All source files must be already processed before calling this.  We record
 * the type and size of file, so that decide_file_action() can later decide
 * what to do with it.
 */
void
process_target_file(const char *path, file_type_t type, size_t size,
					const char *link_target)
{
	filemap_t  *map = filemap;
	file_entry_t *entry;

	/*
	 * Do not apply any exclusion filters here.  This has advantage to remove
	 * from the target data folder all paths which have been filtered out from
	 * the source data folder when processing the source files.
	 */
	if (map->array == NULL)
	{
		/* on first call, initialize lookup array */
		if (map->nlist == 0)
		{
			/* should not happen */
			pg_fatal("source file list is empty");
		}

		filemap_list_to_array(map);

		Assert(map->array != NULL);

		qsort(map->array, map->narray, sizeof(file_entry_t *), path_cmp);
	}

	/*
	 * Like in process_source_file, pretend that pg_wal is always a directory.
	 */
	if (strcmp(path, "pg_wal") == 0 && type == FILE_TYPE_SYMLINK)
		type = FILE_TYPE_DIRECTORY;

	/* Remember this target file */
	entry = get_filemap_entry(path, true);
	entry->target_exists = true;
	entry->target_type = type;
	entry->target_size = size;
	entry->target_link_target = link_target ? pg_strdup(link_target) : NULL;
}

/*
 * This callback gets called while we read the WAL in the target, for every
 * block that has changed in the target system.  It decides if the given
 * 'blkno' in the target relfile needs to be overwritten from the source, and
 * if so, records it in 'target_pages_to_overwrite' bitmap.
 *
 * NOTE: All the files on both systems must have already been added to the
 * file map!
 */
void
process_target_wal_block_change(ForkNumber forknum, RelFileNode rnode,
								BlockNumber blkno)
{
	char	   *path;
	file_entry_t *entry;
	BlockNumber blkno_inseg;
	int			segno;

	Assert(filemap->array);

	segno = blkno / RELSEG_SIZE;
	blkno_inseg = blkno % RELSEG_SIZE;

	path = datasegpath(rnode, forknum, segno);
	entry = get_filemap_entry(path, false);
	pfree(path);

	if (entry)
	{
		int64		end_offset;

		Assert(entry->isrelfile);

		if (entry->target_type != FILE_TYPE_REGULAR)
			pg_fatal("unexpected page modification for non-regular file \"%s\"",
					 entry->path);

		/*
		 * If the block beyond the EOF in the source system, no need to
		 * remember it now, because we're going to truncate it away from the
		 * target anyway. Also no need to remember the block if it's beyond
		 * the current EOF in the target system; we will copy it over with the
		 * "tail" from the source system, anyway.
		 */
		end_offset = (blkno_inseg + 1) * BLCKSZ;
		if (end_offset <= entry->source_size &&
			end_offset <= entry->target_size)
			datapagemap_add(&entry->target_pages_to_overwrite, blkno_inseg);
	}
	else
	{
		/*
		 * If we don't have any record of this file in the file map, it means
		 * that it's a relation that doesn't exist in the source system.  It
		 * could exist in the target system; we haven't moved the target-only
		 * entries from the linked list to the array yet!  But in any case, if
		 * it doesn't exist in the source it will be removed from the target
		 * too, and we can safely ignore it.
		 */
	}
}

/*
 * Is this the path of file that pg_rewind can skip copying?
 */
static bool
check_file_excluded(const char *path, bool is_source)
{
	char		localpath[MAXPGPATH];
	int			excludeIdx;
	const char *filename;

	/*
	 * Skip all temporary files, .../pgsql_tmp/... and .../pgsql_tmp.*
	 */
	if (strstr(path, "/" PG_TEMP_FILE_PREFIX) != NULL ||
		strstr(path, "/" PG_TEMP_FILES_DIR "/") != NULL)
	{
		return true;
	}

	/* check individual files... */
	for (excludeIdx = 0; excludeFiles[excludeIdx].name != NULL; excludeIdx++)
	{
		int			cmplen = strlen(excludeFiles[excludeIdx].name);

		filename = last_dir_separator(path);
		if (filename == NULL)
			filename = path;
		else
			filename++;

		if (!excludeFiles[excludeIdx].match_prefix)
			cmplen++;
		if (strncmp(filename, excludeFiles[excludeIdx].name, cmplen) == 0)
		{
			if (is_source)
				pg_log_debug("entry \"%s\" excluded from source file list",
							 path);
			else
				pg_log_debug("entry \"%s\" excluded from target file list",
							 path);
			return true;
		}
	}

	/*
	 * ... And check some directories.  Note that this includes any contents
	 * within the directories themselves.
	 */
	for (excludeIdx = 0; excludeDirContents[excludeIdx] != NULL; excludeIdx++)
	{
		snprintf(localpath, sizeof(localpath), "%s/",
				 excludeDirContents[excludeIdx]);
		if (strstr(path, localpath) == path)
		{
			if (is_source)
				pg_log_debug("entry \"%s\" excluded from source file list",
							 path);
			else
				pg_log_debug("entry \"%s\" excluded from target file list",
							 path);
			return true;
		}
	}

	return false;
}

/*
 * Convert the linked list of entries in map->first/last to the array,
 * map->array.
 */
static void
filemap_list_to_array(filemap_t *map)
{
	int			narray;
	file_entry_t *entry,
			   *next;

	map->array = (file_entry_t **)
		pg_realloc(map->array,
				   (map->nlist + map->narray) * sizeof(file_entry_t *));

	narray = map->narray;
	for (entry = map->first; entry != NULL; entry = next)
	{
		map->array[narray++] = entry;
		next = entry->next;
		entry->next = NULL;
	}
	Assert(narray == map->nlist + map->narray);
	map->narray = narray;
	map->nlist = 0;
	map->first = map->last = NULL;
}

static const char *
action_to_str(file_action_t action)
{
	switch (action)
	{
		case FILE_ACTION_NONE:
			return "NONE";
		case FILE_ACTION_COPY:
			return "COPY";
		case FILE_ACTION_TRUNCATE:
			return "TRUNCATE";
		case FILE_ACTION_COPY_TAIL:
			return "COPY_TAIL";
		case FILE_ACTION_CREATE:
			return "CREATE";
		case FILE_ACTION_REMOVE:
			return "REMOVE";

		default:
			return "unknown";
	}
}

/*
 * Calculate the totals needed for progress reports.
 */
void
calculate_totals(void)
{
	file_entry_t *entry;
	int			i;
	filemap_t  *map = filemap;

	map->total_size = 0;
	map->fetch_size = 0;

	for (i = 0; i < map->narray; i++)
	{
		entry = map->array[i];

		if (entry->source_type != FILE_TYPE_REGULAR)
			continue;

		map->total_size += entry->source_size;

		if (entry->action == FILE_ACTION_COPY)
		{
			map->fetch_size += entry->source_size;
			continue;
		}

		if (entry->action == FILE_ACTION_COPY_TAIL)
			map->fetch_size += (entry->source_size - entry->target_size);

		if (entry->target_pages_to_overwrite.bitmapsize > 0)
		{
			datapagemap_iterator_t *iter;
			BlockNumber blk;

			iter = datapagemap_iterate(&entry->target_pages_to_overwrite);
			while (datapagemap_next(iter, &blk))
				map->fetch_size += BLCKSZ;

			pg_free(iter);
		}
	}
}

void
print_filemap(void)
{
	filemap_t  *map = filemap;
	file_entry_t *entry;
	int			i;

	for (i = 0; i < map->narray; i++)
	{
		entry = map->array[i];
		if (entry->action != FILE_ACTION_NONE ||
			entry->target_pages_to_overwrite.bitmapsize > 0)
		{
			pg_log_debug("%s (%s)", entry->path,
						 action_to_str(entry->action));

			if (entry->target_pages_to_overwrite.bitmapsize > 0)
				datapagemap_print(&entry->target_pages_to_overwrite);
		}
	}
	fflush(stdout);
}

/*
 * Does it look like a relation data file?
 *
 * For our purposes, only files belonging to the main fork are considered
 * relation files. Other forks are always copied in toto, because we cannot
 * reliably track changes to them, because WAL only contains block references
 * for the main fork.
 */
static bool
isRelDataFile(const char *path)
{
	RelFileNode rnode;
	unsigned int segNo;
	int			nmatch;
	bool		matched;

	/*----
	 * Relation data files can be in one of the following directories:
	 *
	 * global/
	 *		shared relations
	 *
	 * base/<db oid>/
	 *		regular relations, default tablespace
	 *
	 * pg_tblspc/<tblspc oid>/<tblspc version>/
	 *		within a non-default tablespace (the name of the directory
	 *		depends on version)
	 *
	 * And the relation data files themselves have a filename like:
	 *
	 * <oid>.<segment number>
	 *
	 *----
	 */
	rnode.spcNode = InvalidOid;
	rnode.dbNode = InvalidOid;
	rnode.relNode = InvalidOid;
	segNo = 0;
	matched = false;

	nmatch = sscanf(path, "global/%u.%u", &rnode.relNode, &segNo);
	if (nmatch == 1 || nmatch == 2)
	{
		rnode.spcNode = GLOBALTABLESPACE_OID;
		rnode.dbNode = 0;
		matched = true;
	}
	else
	{
		nmatch = sscanf(path, "base/%u/%u.%u",
						&rnode.dbNode, &rnode.relNode, &segNo);
		if (nmatch == 2 || nmatch == 3)
		{
			rnode.spcNode = DEFAULTTABLESPACE_OID;
			matched = true;
		}
		else
		{
			nmatch = sscanf(path, "pg_tblspc/%u/" TABLESPACE_VERSION_DIRECTORY "/%u/%u.%u",
							&rnode.spcNode, &rnode.dbNode, &rnode.relNode,
							&segNo);
			if (nmatch == 3 || nmatch == 4)
				matched = true;
		}
	}

	/*
	 * The sscanf tests above can match files that have extra characters at
	 * the end. To eliminate such cases, cross-check that GetRelationPath
	 * creates the exact same filename, when passed the RelFileNode
	 * information we extracted from the filename.
	 */
	if (matched)
	{
		char	   *check_path = datasegpath(rnode, MAIN_FORKNUM, segNo);

		if (strcmp(check_path, path) != 0)
			matched = false;

		pfree(check_path);
	}

	return matched;
}

/*
 * A helper function to create the path of a relation file and segment.
 *
 * The returned path is palloc'd
 */
static char *
datasegpath(RelFileNode rnode, ForkNumber forknum, BlockNumber segno)
{
	char	   *path;
	char	   *segpath;

	path = relpathperm(rnode, forknum);
	if (segno > 0)
	{
		segpath = psprintf("%s.%u", path, segno);
		pfree(path);
		return segpath;
	}
	else
		return path;
}

static int
path_cmp(const void *a, const void *b)
{
	file_entry_t *fa = *((file_entry_t **) a);
	file_entry_t *fb = *((file_entry_t **) b);

	return strcmp(fa->path, fb->path);
}

/*
 * In the final stage, the filemap is sorted so that removals come last.
 * From disk space usage point of view, it would be better to do removals
 * first, but for now, safety first. If a whole directory is deleted, all
 * files and subdirectories inside it need to removed first. On creation,
 * parent directory needs to be created before files and directories inside
 * it. To achieve that, the file_action_t enum is ordered so that we can
 * just sort on that first. Furthermore, sort REMOVE entries in reverse
 * path order, so that "foo/bar" subdirectory is removed before "foo".
 */
static int
final_filemap_cmp(const void *a, const void *b)
{
	file_entry_t *fa = *((file_entry_t **) a);
	file_entry_t *fb = *((file_entry_t **) b);

	if (fa->action > fb->action)
		return 1;
	if (fa->action < fb->action)
		return -1;

	if (fa->action == FILE_ACTION_REMOVE)
		return strcmp(fb->path, fa->path);
	else
		return strcmp(fa->path, fb->path);
}

/*
 * Decide what action to perform to a file.
 */
static file_action_t
decide_file_action(file_entry_t *entry)
{
	const char *path = entry->path;

	/*
	 * Don't touch the control file. It is handled specially, after copying
	 * all the other files.
	 */
	if (strcmp(path, "global/pg_control") == 0)
		return FILE_ACTION_NONE;

	/*
	 * Remove all files matching the exclusion filters in the target.
	 */
	if (check_file_excluded(path, true))
	{
		if (entry->target_exists)
			return FILE_ACTION_REMOVE;
		else
			return FILE_ACTION_NONE;
	}

	/*
	 * Handle cases where the file is missing from one of the systems.
	 */
	if (!entry->target_exists && entry->source_exists)
	{
		/*
		 * File exists in source, but not in target. Copy it in toto. (If it's
		 * a relation data file, WAL replay after rewinding should re-create
		 * it anyway. But there's no harm in copying it now.)
		 */
		switch (entry->source_type)
		{
			case FILE_TYPE_DIRECTORY:
			case FILE_TYPE_SYMLINK:
				return FILE_ACTION_CREATE;
			case FILE_TYPE_REGULAR:
				return FILE_ACTION_COPY;
			case FILE_TYPE_UNDEFINED:
				pg_fatal("unknown file type for \"%s\"", entry->path);
				break;
		}
	}
	else if (entry->target_exists && !entry->source_exists)
	{
		/* File exists in target, but not source. Remove it. */
		return FILE_ACTION_REMOVE;
	}
	else if (!entry->target_exists && !entry->source_exists)
	{
		/*
		 * Doesn't exist in either server. Why does it have an entry in the
		 * first place??
		 */
		Assert(false);
		return FILE_ACTION_NONE;
	}

	/*
	 * Otherwise, the file exists on both systems
	 */
	Assert(entry->target_exists && entry->source_exists);

	if (entry->source_type != entry->target_type)
	{
		/* But it's a different kind of object. Strange.. */
		pg_fatal("file \"%s\" is of different type in source and target", entry->path);
	}

	/*
	 * PG_VERSION files should be identical on both systems, but avoid
	 * overwriting them for paranoia.
	 */
	if (pg_str_endswith(entry->path, "PG_VERSION"))
		return FILE_ACTION_NONE;

	switch (entry->source_type)
	{
		case FILE_TYPE_DIRECTORY:
			return FILE_ACTION_NONE;

		case FILE_TYPE_SYMLINK:

			/*
			 * XXX: Should we check if it points to the same target?
			 */
			return FILE_ACTION_NONE;

		case FILE_TYPE_REGULAR:
			if (!entry->isrelfile)
			{
				/*
				 * It's a non-data file that we have no special processing
				 * for. Copy it in toto.
				 */
				return FILE_ACTION_COPY;
			}
			else
			{
				/*
				 * It's a data file that exists in both systems.
				 *
				 * If it's larger in target, we can truncate it. There will
				 * also be a WAL record of the truncation in the source
				 * system, so WAL replay would eventually truncate the target
				 * too, but we might as well do it now.
				 *
				 * If it's smaller in the target, it means that it has been
				 * truncated in the target, or enlarged in the source, or
				 * both. If it was truncated in the target, we need to copy
				 * the missing tail from the source system. If it was enlarged
				 * in the source system, there will be WAL records in the
				 * source system for the new blocks, so we wouldn't need to
				 * copy them here. But we don't know which scenario we're
				 * dealing with, and there's no harm in copying the missing
				 * blocks now, so do it now.
				 *
				 * If it's the same size, do nothing here. Any blocks modified
				 * in the target will be copied based on parsing the target
				 * system's WAL, and any blocks modified in the source will be
				 * updated after rewinding, when the source system's WAL is
				 * replayed.
				 */
				if (entry->target_size < entry->source_size)
					return FILE_ACTION_COPY_TAIL;
				else if (entry->target_size > entry->source_size)
					return FILE_ACTION_TRUNCATE;
				else
					return FILE_ACTION_NONE;
			}
			break;

		case FILE_TYPE_UNDEFINED:
			pg_fatal("unknown file type for \"%s\"", path);
			break;
	}

	/* unreachable */
	pg_fatal("could not decide what to do with file \"%s\"", path);
}

/*
 * Decide what to do with each file.
 */
void
decide_file_actions(void)
{
	int			i;

	filemap_list_to_array(filemap);

	for (i = 0; i < filemap->narray; i++)
	{
		file_entry_t *entry = filemap->array[i];

		entry->action = decide_file_action(entry);
	}

	/* Sort the actions to the order that they should be performed */
	qsort(filemap->array, filemap->narray, sizeof(file_entry_t *),
		  final_filemap_cmp);
}
