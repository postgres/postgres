/*-------------------------------------------------------------------------
 *
 * Assorted utility functions to work on files.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/file_utils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FILE_UTILS_H
#define FILE_UTILS_H

#include <dirent.h>

typedef enum PGFileType
{
	PGFILETYPE_ERROR,
	PGFILETYPE_UNKNOWN,
	PGFILETYPE_REG,
	PGFILETYPE_DIR,
	PGFILETYPE_LNK
} PGFileType;

#ifdef FRONTEND
extern int	fsync_fname(const char *fname, bool isdir);
extern void fsync_pgdata(const char *pg_data, int serverVersion);
extern void fsync_dir_recurse(const char *dir);
extern int	durable_rename(const char *oldfile, const char *newfile);
extern int	fsync_parent_path(const char *fname);
#endif

extern PGFileType get_dirent_type(const char *path,
								  const struct dirent *de,
								  bool look_through_symlinks,
								  int elevel);

#endif							/* FILE_UTILS_H */
