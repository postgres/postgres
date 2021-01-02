/*-------------------------------------------------------------------------
 *
 * file_ops.h
 *	  Helper functions for operating on files
 *
 * Copyright (c) 2013-2021, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#ifndef FILE_OPS_H
#define FILE_OPS_H

#include "filemap.h"

extern void open_target_file(const char *path, bool trunc);
extern void write_target_range(char *buf, off_t begin, size_t size);
extern void close_target_file(void);
extern void remove_target_file(const char *path, bool missing_ok);
extern void truncate_target_file(const char *path, off_t newsize);
extern void create_target(file_entry_t *t);
extern void remove_target(file_entry_t *t);
extern void sync_target_dir(void);

extern char *slurpFile(const char *datadir, const char *path, size_t *filesize);

typedef void (*process_file_callback_t) (const char *path, file_type_t type, size_t size, const char *link_target);
extern void traverse_datadir(const char *datadir, process_file_callback_t callback);

#endif							/* FILE_OPS_H */
