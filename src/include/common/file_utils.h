/*-------------------------------------------------------------------------
 *
 * File-processing utility routines for frontend code
 *
 * Assorted utility functions to work on files.
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/file_utils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FILE_UTILS_H
#define FILE_UTILS_H

extern int fsync_fname(const char *fname, bool isdir,
					   const char *progname);
extern void fsync_pgdata(const char *pg_data, const char *progname);
extern int durable_rename(const char *oldfile, const char *newfile,
						  const char *progname);
extern int fsync_parent_path(const char *fname, const char *progname);

#endif   /* FILE_UTILS_H */
