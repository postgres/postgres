/*-------------------------------------------------------------------------
 *
 * dumpmem.h
 *	  Memory allocation routines used by pg_dump, pg_dumpall, and pg_restore
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_dump/dumpmem.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef DUMPMEM_H
#define DUMPMEM_H

extern char *pg_strdup(const char *string);
extern void *pg_malloc(size_t size);
extern void *pg_calloc(size_t nmemb, size_t size);
extern void *pg_realloc(void *ptr, size_t size);

#endif   /* DUMPMEM_H */
