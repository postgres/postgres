/*-------------------------------------------------------------------------
 *
 * fetch.h
 *	  Fetching data from a local or remote data directory.
 *
 * This file includes the prototypes for functions used to copy files from
 * one data directory to another. The source to copy from can be a local
 * directory (copy method), or a remote PostgreSQL server (libpq fetch
 * method).
 *
 * Copyright (c) 2013-2015, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#ifndef FETCH_H
#define FETCH_H

#include "c.h"

#include "access/xlogdefs.h"

#include "filemap.h"

/*
 * Common interface. Calls the copy or libpq method depending on global
 * config options.
 */
extern void fetchSourceFileList(void);
extern char *fetchFile(char *filename, size_t *filesize);
extern void executeFileMap(void);

/* in libpq_fetch.c */
extern void libpqProcessFileList(void);
extern char *libpqGetFile(const char *filename, size_t *filesize);
extern void libpq_executeFileMap(filemap_t *map);

extern void libpqConnect(const char *connstr);
extern XLogRecPtr libpqGetCurrentXlogInsertLocation(void);

/* in copy_fetch.c */
extern void copy_executeFileMap(filemap_t *map);

typedef void (*process_file_callback_t) (const char *path, file_type_t type, size_t size, const char *link_target);
extern void traverse_datadir(const char *datadir, process_file_callback_t callback);

#endif   /* FETCH_H */
