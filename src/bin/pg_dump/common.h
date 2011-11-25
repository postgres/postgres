/*-------------------------------------------------------------------------
 *
 * common.h
 *	  Common header file for the pg_dump, pg_dumpall, and pg_restore
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_dump/common.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef COMMON_H
#define COMMON_H

#include "postgres_fe.h"

extern char *pg_strdup(const char *string);
extern void *pg_malloc(size_t size);
extern void *pg_calloc(size_t nmemb, size_t size);
extern void *pg_realloc(void *ptr, size_t size);

#endif   /* COMMON_H */
