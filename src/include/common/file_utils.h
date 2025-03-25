/*-------------------------------------------------------------------------
 *
 * Assorted utility functions to work on files.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
	PGFILETYPE_LNK,
} PGFileType;

typedef enum DataDirSyncMethod
{
	DATA_DIR_SYNC_METHOD_FSYNC,
	DATA_DIR_SYNC_METHOD_SYNCFS,
} DataDirSyncMethod;

struct iovec;					/* avoid including port/pg_iovec.h here */

#ifdef FRONTEND
extern int	pre_sync_fname(const char *fname, bool isdir);
extern int	fsync_fname(const char *fname, bool isdir);
extern void sync_pgdata(const char *pg_data, int serverVersion,
						DataDirSyncMethod sync_method, bool sync_data_files);
extern void sync_dir_recurse(const char *dir, DataDirSyncMethod sync_method);
extern int	durable_rename(const char *oldfile, const char *newfile);
extern int	fsync_parent_path(const char *fname);
#endif

extern PGFileType get_dirent_type(const char *path,
								  const struct dirent *de,
								  bool look_through_symlinks,
								  int elevel);

extern int	compute_remaining_iovec(struct iovec *destination,
									const struct iovec *source,
									int iovcnt,
									size_t transferred);

extern ssize_t pg_pwritev_with_retry(int fd,
									 const struct iovec *iov,
									 int iovcnt,
									 off_t offset);

extern ssize_t pg_pwrite_zeros(int fd, size_t size, off_t offset);

/* Filename components */
#define PG_TEMP_FILES_DIR "pgsql_tmp"
#define PG_TEMP_FILE_PREFIX "pgsql_tmp"

#endif							/* FILE_UTILS_H */
