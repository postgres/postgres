/*
 *	fe_memutils.h
 *		memory management support for frontend code
 *
 *	Copyright (c) 2003-2014, PostgreSQL Global Development Group
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

/* sprintf into a palloc'd buffer --- these are in psprintf.c */
extern char *
psprintf(const char *fmt,...)
__attribute__((format(PG_PRINTF_ATTRIBUTE, 1, 2)));
extern size_t
pvsnprintf(char *buf, size_t len, const char *fmt, va_list args)
__attribute__((format(PG_PRINTF_ATTRIBUTE, 3, 0)));

#endif   /* FE_MEMUTILS_H */
