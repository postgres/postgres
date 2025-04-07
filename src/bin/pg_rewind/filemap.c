/*-------------------------------------------------------------------------
 *
 * filemap.c
 *	  A data structure for keeping track of files that have changed.
 *
 * This source file contains the logic to decide what to do with different
 * kinds of files, and the data structure to support it.  Before modifying
 * anything, pg_rewind collects information about all the files and their
 * attributes in the target and source data directories.  It also scans the
 * WAL log in the target, and collects information about data blocks that
 * were changed.  All this information is stored in a hash table, using the
 * file path relative to the root of the data directory as the key.
 *
 * After collecting all the information required, the decide_file_actions()
 * function scans the hash table and decides what action needs to be taken
 * for each file.  Finally, it sorts the array to the final order that the
 * actions should be executed in.
 *
 * Copyright (c) 2013-2025, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/xlog_internal.h"
#include "catalog/pg_tablespace_d.h"
#include "common/file_utils.h"
#include "common/hashfn_unstable.h"
#include "common/string.h"
#include "datapagemap.h"
#include "filemap.h"
#include "pg_rewind.h"

/*
 * Define a hash table which we can use to store information about the files
 * appearing in source and target systems.
 */
#define SH_PREFIX				filehash
#define SH_ELEMENT_TYPE			file_entry_t
#define SH_KEY_TYPE				const char *
#define SH_KEY					path
#define SH_HASH_KEY(tb, key)	hash_string(key)
#define SH_EQUAL(tb, a, b)		(strcmp(a, b) == 0)
#define SH_SCOPE				static inline
#define SH_RAW_ALLOCATOR		pg_malloc0
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

#define FILEHASH_INITIAL_SIZE	1000

static filehash_hash *filehash;

static bool isRelDataFile(const char *path);
static char *datasegpath(RelFileLocator rlocator, ForkNumber forknum,
						 BlockNumber segno);

static file_entry_t *insert_filehash_entry(const char *path);
static file_entry_t *lookup_filehash_entry(const char *path);

/*
 * A separate hash table which tracks WAL files that must not be deleted.
 */
typedef struct keepwal_entry
{
	const char *path;
	uint32		status;
} keepwal_entry;

#define SH_PREFIX				keepwal
#define SH_ELEMENT_TYPE			keepwal_entry
#define SH_KEY_TYPE				const char *
#define SH_KEY					path
#define SH_HASH_KEY(tb, key)	hash_string(key)
#define SH_EQUAL(tb, a, b)		(strcmp(a, b) == 0)
#define SH_SCOPE				static inline
#define SH_RAW_ALLOCATOR		pg_malloc0
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

#define KEEPWAL_INITIAL_SIZE	1000


static keepwal_hash *keepwal = NULL;
static bool keepwal_entry_exists(const char *path);

static int	final_filemap_cmp(const void *a, const void *b);

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
static const char *const excludeDirContents[] =
{
	/*
	 * Skip temporary statistics files. PG_STAT_TMP_DIR must be skipped
	 * because extensions like pg_stat_statements store data there.
	 */
	"pg_stat_tmp",				/* defined as PG_STAT_TMP_DIR */

	/*
	 * It is generally not useful to backup the contents of this directory
	 * even if the intention is to restore to another primary. See backup.sgml
	 * for a more detailed description.
	 */
	"pg_replslot",				/* defined as PG_REPLSLOT_DIR */

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
	 * If there is a backup_label or tablespace_map file, it indicates that a
	 * recovery failed and this cluster probably can't be rewound, but exclude
	 * them anyway if they are found.
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
 * Initialize the hash table for the file map.
 */
void
filehash_init(void)
{
	filehash = filehash_create(FILEHASH_INITIAL_SIZE, NULL);
}

/* Look up entry for 'path', creating a new one if it doesn't exist */
static file_entry_t *
insert_filehash_entry(const char *path)
{
	file_entry_t *entry;
	bool		found;

	entry = filehash_insert(filehash, path, &found);
	if (!found)
	{
		entry->path = pg_strdup(path);
		entry->isrelfile = isRelDataFile(path);

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

		entry->action = FILE_ACTION_UNDECIDED;
	}

	return entry;
}

static file_entry_t *
lookup_filehash_entry(const char *path)
{
	return filehash_lookup(filehash, path);
}

/*
 * Initialize a hash table to store WAL file names that must be kept.
 */
void
keepwal_init(void)
{
	/* An initial hash size out of thin air */
	keepwal = keepwal_create(KEEPWAL_INITIAL_SIZE, NULL);
}

/* Mark the given file to prevent its removal */
void
keepwal_add_entry(const char *path)
{
	keepwal_entry *entry;
	bool		found;

	/* Should only be called with keepwal initialized */
	Assert(keepwal != NULL);

	entry = keepwal_insert(keepwal, path, &found);

	if (!found)
		entry->path = pg_strdup(path);
}

/* Return true if file is marked as not to be removed, false otherwise */
static bool
keepwal_entry_exists(const char *path)
{
	return keepwal_lookup(keepwal, path) != NULL;
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
	entry = insert_filehash_entry(path);
	if (entry->source_exists)
		pg_fatal("duplicate source file \"%s\"", path);
	entry->source_exists = true;
	entry->source_type = type;
	entry->source_size = size;
	entry->source_link_target = link_target ? pg_strdup(link_target) : NULL;
}

/*
 * Callback for processing target file list.
 *
 * Record the type and size of the file, like process_source_file() does.
 */
void
process_target_file(const char *path, file_type_t type, size_t size,
					const char *link_target)
{
	file_entry_t *entry;

	/*
	 * Do not apply any exclusion filters here.  This has advantage to remove
	 * from the target data folder all paths which have been filtered out from
	 * the source data folder when processing the source files.
	 */

	/*
	 * Like in process_source_file, pretend that pg_wal is always a directory.
	 */
	if (strcmp(path, "pg_wal") == 0 && type == FILE_TYPE_SYMLINK)
		type = FILE_TYPE_DIRECTORY;

	/* Remember this target file */
	entry = insert_filehash_entry(path);
	if (entry->target_exists)
		pg_fatal("duplicate source file \"%s\"", path);
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
 * hash table!
 */
void
process_target_wal_block_change(ForkNumber forknum, RelFileLocator rlocator,
								BlockNumber blkno)
{
	char	   *path;
	file_entry_t *entry;
	BlockNumber blkno_inseg;
	int			segno;

	segno = blkno / RELSEG_SIZE;
	blkno_inseg = blkno % RELSEG_SIZE;

	path = datasegpath(rlocator, forknum, segno);
	entry = lookup_filehash_entry(path);
	pfree(path);

	/*
	 * If the block still exists in both systems, remember it. Otherwise we
	 * can safely ignore it.
	 *
	 * If the block is beyond the EOF in the source system, or the file
	 * doesn't exist in the source at all, we're going to truncate/remove it
	 * away from the target anyway. Likewise, if it doesn't exist in the
	 * target anymore, we will copy it over with the "tail" from the source
	 * system, anyway.
	 *
	 * It is possible to find WAL for a file that doesn't exist on either
	 * system anymore. It means that the relation was dropped later in the
	 * target system, and independently on the source system too, or that it
	 * was created and dropped in the target system and it never existed in
	 * the source. Either way, we can safely ignore it.
	 */
	if (entry)
	{
		Assert(entry->isrelfile);

		if (entry->target_exists)
		{
			if (entry->target_type != FILE_TYPE_REGULAR)
				pg_fatal("unexpected page modification for non-regular file \"%s\"",
						 entry->path);

			if (entry->source_exists)
			{
				off_t		end_offset;

				end_offset = (blkno_inseg + 1) * BLCKSZ;
				if (end_offset <= entry->source_size && end_offset <= entry->target_size)
					datapagemap_add(&entry->target_pages_to_overwrite, blkno_inseg);
			}
		}
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
calculate_totals(filemap_t *filemap)
{
	file_entry_t *entry;
	int			i;

	filemap->total_size = 0;
	filemap->fetch_size = 0;

	for (i = 0; i < filemap->nentries; i++)
	{
		entry = filemap->entries[i];

		if (entry->source_type != FILE_TYPE_REGULAR)
			continue;

		filemap->total_size += entry->source_size;

		if (entry->action == FILE_ACTION_COPY)
		{
			filemap->fetch_size += entry->source_size;
			continue;
		}

		if (entry->action == FILE_ACTION_COPY_TAIL)
			filemap->fetch_size += (entry->source_size - entry->target_size);

		if (entry->target_pages_to_overwrite.bitmapsize > 0)
		{
			datapagemap_iterator_t *iter;
			BlockNumber blk;

			iter = datapagemap_iterate(&entry->target_pages_to_overwrite);
			while (datapagemap_next(iter, &blk))
				filemap->fetch_size += BLCKSZ;

			pg_free(iter);
		}
	}
}

void
print_filemap(filemap_t *filemap)
{
	file_entry_t *entry;
	int			i;

	for (i = 0; i < filemap->nentries; i++)
	{
		entry = filemap->entries[i];
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
	RelFileLocator rlocator;
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
	rlocator.spcOid = InvalidOid;
	rlocator.dbOid = InvalidOid;
	rlocator.relNumber = InvalidRelFileNumber;
	segNo = 0;
	matched = false;

	nmatch = sscanf(path, "global/%u.%u", &rlocator.relNumber, &segNo);
	if (nmatch == 1 || nmatch == 2)
	{
		rlocator.spcOid = GLOBALTABLESPACE_OID;
		rlocator.dbOid = 0;
		matched = true;
	}
	else
	{
		nmatch = sscanf(path, "base/%u/%u.%u",
						&rlocator.dbOid, &rlocator.relNumber, &segNo);
		if (nmatch == 2 || nmatch == 3)
		{
			rlocator.spcOid = DEFAULTTABLESPACE_OID;
			matched = true;
		}
		else
		{
			nmatch = sscanf(path, "pg_tblspc/%u/" TABLESPACE_VERSION_DIRECTORY "/%u/%u.%u",
							&rlocator.spcOid, &rlocator.dbOid, &rlocator.relNumber,
							&segNo);
			if (nmatch == 3 || nmatch == 4)
				matched = true;
		}
	}

	/*
	 * The sscanf tests above can match files that have extra characters at
	 * the end. To eliminate such cases, cross-check that GetRelationPath
	 * creates the exact same filename, when passed the RelFileLocator
	 * information we extracted from the filename.
	 */
	if (matched)
	{
		char	   *check_path = datasegpath(rlocator, MAIN_FORKNUM, segNo);

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
datasegpath(RelFileLocator rlocator, ForkNumber forknum, BlockNumber segno)
{
	RelPathStr	path;
	char	   *segpath;

	path = relpathperm(rlocator, forknum);
	if (segno > 0)
	{
		segpath = psprintf("%s.%u", path.str, segno);
		return segpath;
	}
	else
		return pstrdup(path.str);
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
	if (strcmp(path, XLOG_CONTROL_FILE) == 0)
		return FILE_ACTION_NONE;

	/* Skip macOS system files */
	if (strstr(path, ".DS_Store") != NULL)
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
		/*
		 * For files that exist in target but not in source, we check the
		 * keepwal hash table; any files listed therein must not be removed.
		 */
		if (keepwal_entry_exists(path))
		{
			pg_log_debug("Not removing file \"%s\" because it is required for recovery", path);
			return FILE_ACTION_NONE;
		}
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
 *
 * Returns a 'filemap' with the entries in the order that their actions
 * should be executed.
 */
filemap_t *
decide_file_actions(void)
{
	int			i;
	filehash_iterator it;
	file_entry_t *entry;
	filemap_t  *filemap;

	filehash_start_iterate(filehash, &it);
	while ((entry = filehash_iterate(filehash, &it)) != NULL)
	{
		entry->action = decide_file_action(entry);
	}

	/*
	 * Turn the hash table into an array, and sort in the order that the
	 * actions should be performed.
	 */
	filemap = pg_malloc(offsetof(filemap_t, entries) +
						filehash->members * sizeof(file_entry_t *));
	filemap->nentries = filehash->members;
	filehash_start_iterate(filehash, &it);
	i = 0;
	while ((entry = filehash_iterate(filehash, &it)) != NULL)
	{
		filemap->entries[i++] = entry;
	}

	qsort(&filemap->entries, filemap->nentries, sizeof(file_entry_t *),
		  final_filemap_cmp);

	return filemap;
}
