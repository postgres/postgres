/*
 *	fe_memutils.h
 *		memory management support for frontend code
 *
 *	Copyright (c) 2003-2013, PostgreSQL Global Development Group
 *
 *	src/include/common/fe_memutils.h
 */
#ifndef FE_MEMUTILS_H
#define FE_MEMUTILS_H

/* "Safe" memory allocation functions --- these exit(1) on failure */
extern char *pg_strdup(const char *in);
extern void *pg_malloc(size_t size);
extern void *pg_malloc0(size_t size);
extern void *pg_realloc(void *pointer, size_t size);
extern void pg_free(void *pointer);

/* Equivalent functions, deliberately named the same as backend functions */
extern char *pstrdup(const char *in);
extern void *palloc(Size size);
extern void *palloc0(Size size);
extern void *repalloc(void *pointer, Size size);
extern void pfree(void *pointer);

#endif   /* FE_MEMUTILS_H */
