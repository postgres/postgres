/*-------------------------------------------------------------------------
 *
 * filemap.h
 *
 * Copyright (c) 2013-2020, PostgreSQL Global Development Group
 *-------------------------------------------------------------------------
 */
#ifndef FILEMAP_H
#define FILEMAP_H

#include "datapagemap.h"
#include "storage/block.h"
#include "storage/relfilenode.h"

/*
 * For every file found in the local or remote system, we have a file entry
 * that contains information about the file on both systems.  For relation
 * files, there is also a page map that marks pages in the file that were
 * changed in the target after the last common checkpoint.  Each entry also
 * contains an 'action' field, which says what we are going to do with the
 * file.
 */

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
	FILE_ACTION_REMOVE			/* remove local file / directory / symlink */
} file_action_t;

typedef enum
{
	FILE_TYPE_UNDEFINED = 0,

	FILE_TYPE_REGULAR,
	FILE_TYPE_DIRECTORY,
	FILE_TYPE_SYMLINK
} file_type_t;

typedef struct file_entry_t
{
	char	   *path;
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

	struct file_entry_t *next;
} file_entry_t;

typedef struct filemap_t
{
	/*
	 * New entries are accumulated to a linked list, in process_source_file
	 * and process_target_file.
	 */
	file_entry_t *first;
	file_entry_t *last;
	int			nlist;			/* number of entries currently in list */

	/*
	 * After processing all the remote files, the entries in the linked list
	 * are moved to this array. After processing local files, too, all the
	 * local entries are added to the array by decide_file_actions(), and
	 * sorted in the final order. After decide_file_actions(), all the entries
	 * are in the array, and the linked list is empty.
	 */
	file_entry_t **array;
	int			narray;			/* current length of array */

	/*
	 * Summary information.
	 */
	uint64		total_size;		/* total size of the source cluster */
	uint64		fetch_size;		/* number of bytes that needs to be copied */
} filemap_t;

extern filemap_t *filemap;

extern void filemap_create(void);
extern void calculate_totals(void);
extern void print_filemap(void);

/* Functions for populating the filemap */
extern void process_source_file(const char *path, file_type_t type,
								size_t size, const char *link_target);
extern void process_target_file(const char *path, file_type_t type,
								size_t size, const char *link_target);
extern void process_target_wal_block_change(ForkNumber forknum,
											RelFileNode rnode,
											BlockNumber blkno);
extern void decide_file_actions(void);

#endif							/* FILEMAP_H */
