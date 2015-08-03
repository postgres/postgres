/*-------------------------------------------------------------------------
 *
 * filemap.c
 *	  A data structure for keeping track of files that have changed.
 *
 * Copyright (c) 2013-2015, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "datapagemap.h"
#include "filemap.h"
#include "logging.h"
#include "pg_rewind.h"

#include "common/string.h"
#include "catalog/pg_tablespace.h"
#include "storage/fd.h"

filemap_t  *filemap = NULL;

static bool isRelDataFile(const char *path);
static char *datasegpath(RelFileNode rnode, ForkNumber forknum,
			BlockNumber segno);
static int	path_cmp(const void *a, const void *b);
static int	final_filemap_cmp(const void *a, const void *b);
static void filemap_list_to_array(filemap_t *map);

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

/*
 * Callback for processing source file list.
 *
 * This is called once for every file in the source server. We decide what
 * action needs to be taken for the file, depending on whether the file
 * exists in the target and whether the size matches.
 */
void
process_source_file(const char *path, file_type_t type, size_t newsize,
					const char *link_target)
{
	bool		exists;
	char		localpath[MAXPGPATH];
	struct stat statbuf;
	filemap_t  *map = filemap;
	file_action_t action = FILE_ACTION_NONE;
	size_t		oldsize = 0;
	file_entry_t *entry;

	Assert(map->array == NULL);

	/*
	 * Completely ignore some special files in source and destination.
	 */
	if (strcmp(path, "postmaster.pid") == 0 ||
		strcmp(path, "postmaster.opts") == 0)
		return;

	/*
	 * Pretend that pg_xlog is a directory, even if it's really a symlink.
	 * We don't want to mess with the symlink itself, nor complain if it's a
	 * symlink in source but not in target or vice versa.
	 */
	if (strcmp(path, "pg_xlog") == 0 && type == FILE_TYPE_SYMLINK)
		type = FILE_TYPE_DIRECTORY;

	/*
	 * Skip temporary files, .../pgsql_tmp/... and .../pgsql_tmp.* in source.
	 * This has the effect that all temporary files in the destination will be
	 * removed.
	 */
	if (strstr(path, "/" PG_TEMP_FILE_PREFIX) != NULL)
		return;
	if (strstr(path, "/" PG_TEMP_FILES_DIR "/") != NULL)
		return;

	/*
	 * sanity check: a filename that looks like a data file better be a
	 * regular file
	 */
	if (type != FILE_TYPE_REGULAR && isRelDataFile(path))
		pg_fatal("data file \"%s\" in source is not a regular file\n", path);

	snprintf(localpath, sizeof(localpath), "%s/%s", datadir_target, path);

	/* Does the corresponding file exist in the target data dir? */
	if (lstat(localpath, &statbuf) < 0)
	{
		if (errno != ENOENT)
			pg_fatal("could not stat file \"%s\": %s\n",
					 localpath, strerror(errno));

		exists = false;
	}
	else
		exists = true;

	switch (type)
	{
		case FILE_TYPE_DIRECTORY:
			if (exists && !S_ISDIR(statbuf.st_mode) && strcmp(path, "pg_xlog") != 0)
			{
				/* it's a directory in source, but not in target. Strange.. */
				pg_fatal("\"%s\" is not a directory\n", localpath);
			}

			if (!exists)
				action = FILE_ACTION_CREATE;
			else
				action = FILE_ACTION_NONE;
			oldsize = 0;
			break;

		case FILE_TYPE_SYMLINK:
			if (exists &&
#ifndef WIN32
				!S_ISLNK(statbuf.st_mode)
#else
				!pgwin32_is_junction(localpath)
#endif
				)
			{
				/*
				 * It's a symbolic link in source, but not in target.
				 * Strange..
				 */
				pg_fatal("\"%s\" is not a symbolic link\n", localpath);
			}

			if (!exists)
				action = FILE_ACTION_CREATE;
			else
				action = FILE_ACTION_NONE;
			oldsize = 0;
			break;

		case FILE_TYPE_REGULAR:
			if (exists && !S_ISREG(statbuf.st_mode))
				pg_fatal("\"%s\" is not a regular file\n", localpath);

			if (!exists || !isRelDataFile(path))
			{
				/*
				 * File exists in source, but not in target. Or it's a
				 * non-data file that we have no special processing for. Copy
				 * it in toto.
				 *
				 * An exception: PG_VERSIONs should be identical, but avoid
				 * overwriting it for paranoia.
				 */
				if (pg_str_endswith(path, "PG_VERSION"))
				{
					action = FILE_ACTION_NONE;
					oldsize = statbuf.st_size;
				}
				else
				{
					action = FILE_ACTION_COPY;
					oldsize = 0;
				}
			}
			else
			{
				/*
				 * It's a data file that exists in both.
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
				oldsize = statbuf.st_size;
				if (oldsize < newsize)
					action = FILE_ACTION_COPY_TAIL;
				else if (oldsize > newsize)
					action = FILE_ACTION_TRUNCATE;
				else
					action = FILE_ACTION_NONE;
			}
			break;
	}

	/* Create a new entry for this file */
	entry = pg_malloc(sizeof(file_entry_t));
	entry->path = pg_strdup(path);
	entry->type = type;
	entry->action = action;
	entry->oldsize = oldsize;
	entry->newsize = newsize;
	entry->link_target = link_target ? pg_strdup(link_target) : NULL;
	entry->next = NULL;
	entry->pagemap.bitmap = NULL;
	entry->pagemap.bitmapsize = 0;
	entry->isrelfile = isRelDataFile(path);

	if (map->last)
	{
		map->last->next = entry;
		map->last = entry;
	}
	else
		map->first = map->last = entry;
	map->nlist++;
}

/*
 * Callback for processing target file list.
 *
 * All source files must be already processed before calling this. This only
 * marks target data directory's files that didn't exist in the source for
 * deletion.
 */
void
process_target_file(const char *path, file_type_t type, size_t oldsize,
					const char *link_target)
{
	bool		exists;
	char		localpath[MAXPGPATH];
	struct stat statbuf;
	file_entry_t key;
	file_entry_t *key_ptr;
	filemap_t  *map = filemap;
	file_entry_t *entry;

	snprintf(localpath, sizeof(localpath), "%s/%s", datadir_target, path);
	if (lstat(localpath, &statbuf) < 0)
	{
		if (errno != ENOENT)
			pg_fatal("could not stat file \"%s\": %s\n",
					 localpath, strerror(errno));

		exists = false;
	}

	if (map->array == NULL)
	{
		/* on first call, initialize lookup array */
		if (map->nlist == 0)
		{
			/* should not happen */
			pg_fatal("source file list is empty\n");
		}

		filemap_list_to_array(map);

		Assert(map->array != NULL);

		qsort(map->array, map->narray, sizeof(file_entry_t *), path_cmp);
	}

	/*
	 * Completely ignore some special files
	 */
	if (strcmp(path, "postmaster.pid") == 0 ||
		strcmp(path, "postmaster.opts") == 0)
		return;

	/*
	 * Like in process_source_file, pretend that xlog is always a  directory.
	 */
	if (strcmp(path, "pg_xlog") == 0 && type == FILE_TYPE_SYMLINK)
		type = FILE_TYPE_DIRECTORY;

	key.path = (char *) path;
	key_ptr = &key;
	exists = (bsearch(&key_ptr, map->array, map->narray, sizeof(file_entry_t *),
					  path_cmp) != NULL);

	/* Remove any file or folder that doesn't exist in the source system. */
	if (!exists)
	{
		entry = pg_malloc(sizeof(file_entry_t));
		entry->path = pg_strdup(path);
		entry->type = type;
		entry->action = FILE_ACTION_REMOVE;
		entry->oldsize = oldsize;
		entry->newsize = 0;
		entry->link_target = link_target ? pg_strdup(link_target) : NULL;
		entry->next = NULL;
		entry->pagemap.bitmap = NULL;
		entry->pagemap.bitmapsize = 0;
		entry->isrelfile = isRelDataFile(path);

		if (map->last == NULL)
			map->first = entry;
		else
			map->last->next = entry;
		map->last = entry;
		map->nlist++;
	}
	else
	{
		/*
		 * We already handled all files that exist in the source system in
		 * process_source_file().
		 */
	}
}

/*
 * This callback gets called while we read the WAL in the target, for every
 * block that have changed in the target system. It makes note of all the
 * changed blocks in the pagemap of the file.
 */
void
process_block_change(ForkNumber forknum, RelFileNode rnode, BlockNumber blkno)
{
	char	   *path;
	file_entry_t key;
	file_entry_t *key_ptr;
	file_entry_t *entry;
	BlockNumber blkno_inseg;
	int			segno;
	filemap_t  *map = filemap;
	file_entry_t **e;

	Assert(map->array);

	segno = blkno / RELSEG_SIZE;
	blkno_inseg = blkno % RELSEG_SIZE;

	path = datasegpath(rnode, forknum, segno);

	key.path = (char *) path;
	key_ptr = &key;

	e = bsearch(&key_ptr, map->array, map->narray, sizeof(file_entry_t *),
				path_cmp);
	if (e)
		entry = *e;
	else
		entry = NULL;
	pfree(path);

	if (entry)
	{
		Assert(entry->isrelfile);

		switch (entry->action)
		{
			case FILE_ACTION_NONE:
			case FILE_ACTION_TRUNCATE:
				/* skip if we're truncating away the modified block anyway */
				if ((blkno_inseg + 1) * BLCKSZ <= entry->newsize)
					datapagemap_add(&entry->pagemap, blkno_inseg);
				break;

			case FILE_ACTION_COPY_TAIL:

				/*
				 * skip the modified block if it is part of the "tail" that
				 * we're copying anyway.
				 */
				if ((blkno_inseg + 1) * BLCKSZ <= entry->oldsize)
					datapagemap_add(&entry->pagemap, blkno_inseg);
				break;

			case FILE_ACTION_COPY:
			case FILE_ACTION_REMOVE:
				break;

			case FILE_ACTION_CREATE:
				pg_fatal("unexpected page modification for directory or symbolic link \"%s\"\n", entry->path);
		}
	}
	else
	{
		/*
		 * If we don't have any record of this file in the file map, it means
		 * that it's a relation that doesn't exist in the source system, and
		 * it was subsequently removed in the target system, too. We can
		 * safely ignore it.
		 */
	}
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

void
filemap_finalize(void)
{
	filemap_t  *map = filemap;

	filemap_list_to_array(map);
	qsort(map->array, map->narray, sizeof(file_entry_t *),
		  final_filemap_cmp);
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

		if (entry->type != FILE_TYPE_REGULAR)
			continue;

		map->total_size += entry->newsize;

		if (entry->action == FILE_ACTION_COPY)
		{
			map->fetch_size += entry->newsize;
			continue;
		}

		if (entry->action == FILE_ACTION_COPY_TAIL)
			map->fetch_size += (entry->newsize - entry->oldsize);

		if (entry->pagemap.bitmapsize > 0)
		{
			datapagemap_iterator_t *iter;
			BlockNumber blk;

			iter = datapagemap_iterate(&entry->pagemap);
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
			entry->pagemap.bitmapsize > 0)
		{
			printf("%s (%s)\n", entry->path, action_to_str(entry->action));

			if (entry->pagemap.bitmapsize > 0)
				datapagemap_print(&entry->pagemap);
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
	char		buf[20 + 1];
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
	 * pg_tblspc/<tblspc oid>/PG_9.4_201403261/
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
			nmatch = sscanf(path, "pg_tblspc/%u/PG_%20s/%u/%u.%u",
						  &rnode.spcNode, buf, &rnode.dbNode, &rnode.relNode,
							&segNo);
			if (nmatch == 4 || nmatch == 5)
				matched = true;
		}
	}

	/*
	 * The sscanf tests above can match files that have extra characters at
	 * the end, and the last check can also match a path belonging to a
	 * different version (different TABLESPACE_VERSION_DIRECTORY). To make
	 * eliminate such cases, cross-check that GetRelationPath creates the
	 * exact same filename, when passed the RelFileNode information we
	 * extracted from the filename.
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
		return -strcmp(fa->path, fb->path);
	else
		return strcmp(fa->path, fb->path);
}
