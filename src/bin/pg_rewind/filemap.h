/*-------------------------------------------------------------------------
 *
 * filemap.h
 *
 * Copyright (c) 2013-2024, PostgreSQL Global Development Group
 *-------------------------------------------------------------------------
 */
#ifndef FILEMAP_H
#define FILEMAP_H

#include "datapagemap.h"
#include "storage/block.h"
#include "storage/relfilelocator.h"

/* these enum values are sorted in the order we want actions to be processed */
typedef enum
{
	FILE_ACTION_UNDECIDED = 0,	/* not decided yet */

	FILE_ACTION_CREATE,			/* create local directory or symbolic link */
	FILE_ACTION_COPY,			/* copy whole file, overwriting if exists */
	FILE_ACTION_COPY_TAIL,		/* copy tail from 'source_size' to
								 * 'target_size' */
	FILE_ACTION_NONE,			/* no action (we might still copy modified
								 * blocks based on the parsed WAL) */
	FILE_ACTION_TRUNCATE,		/* truncate local file to 'newsize' bytes */
	FILE_ACTION_REMOVE,			/* remove local file / directory / symlink */
} file_action_t;

typedef enum
{
	FILE_TYPE_UNDEFINED = 0,

	FILE_TYPE_REGULAR,
	FILE_TYPE_DIRECTORY,
	FILE_TYPE_SYMLINK,
} file_type_t;

/*
 * For every file found in the local or remote system, we have a file entry
 * that contains information about the file on both systems.  For relation
 * files, there is also a page map that marks pages in the file that were
 * changed in the target after the last common checkpoint.
 *
 * When gathering information, these are kept in a hash table, private to
 * filemap.c.  decide_file_actions() fills in the 'action' field, sorts all
 * the entries, and returns them in an array, ready for executing the actions.
 */
typedef struct file_entry_t
{
	uint32		status;			/* hash status */

	const char *path;
	bool		isrelfile;		/* is it a relation data file? */

	/*
	 * Status of the file in the target.
	 */
	bool		target_exists;
	file_type_t target_type;
	size_t		target_size;	/* for a regular file */
	char	   *target_link_target; /* for a symlink */

	/*
	 * Pages that were modified in the target and need to be replaced from the
	 * source.
	 */
	datapagemap_t target_pages_to_overwrite;

	/*
	 * Status of the file in the source.
	 */
	bool		source_exists;
	file_type_t source_type;
	size_t		source_size;
	char	   *source_link_target; /* for a symlink */

	/*
	 * What will we do to the file?
	 */
	file_action_t action;
} file_entry_t;

/*
 * This contains the final decisions on what to do with each file.
 * 'entries' array contains an entry for each file, sorted in the order
 * that their actions should executed.
 */
typedef struct filemap_t
{
	/* Summary information, filled by calculate_totals() */
	uint64		total_size;		/* total size of the source cluster */
	uint64		fetch_size;		/* number of bytes that needs to be copied */

	int			nentries;		/* size of 'entries' array */
	file_entry_t *entries[FLEXIBLE_ARRAY_MEMBER];
} filemap_t;

/* Functions for populating the filemap */
extern void filehash_init(void);
extern void process_source_file(const char *path, file_type_t type,
								size_t size, const char *link_target);
extern void process_target_file(const char *path, file_type_t type,
								size_t size, const char *link_target);
extern void process_target_wal_block_change(ForkNumber forknum,
											RelFileLocator rlocator,
											BlockNumber blkno);

extern filemap_t *decide_file_actions(void);
extern void calculate_totals(filemap_t *filemap);
extern void print_filemap(filemap_t *filemap);

extern void keepwal_init(void);
extern void keepwal_add_entry(const char *path);

#endif							/* FILEMAP_H */
