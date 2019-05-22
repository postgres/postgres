/*-------------------------------------------------------------------------
 *
 * File-processing utility routines for frontend code
 *
 * Assorted utility functions to work on files.
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/file_utils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FILE_UTILS_H
#define FILE_UTILS_H

extern int	fsync_fname(const char *fname, bool isdir);
extern void fsync_pgdata(const char *pg_data, int serverVersion);
extern void fsync_dir_recurse(const char *dir);
extern int	durable_rename(const char *oldfile, const char *newfile);
extern int	fsync_parent_path(const char *fname);

#endif							/* FILE_UTILS_H */
