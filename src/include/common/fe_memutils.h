/*
 *	fe_memutils.h
 *		memory management support for frontend code
 *
 *	Copyright (c) 2003-2024, PostgreSQL Global Development Group
 *
 *	src/include/common/fe_memutils.h
 */
#ifndef FE_MEMUTILS_H
#define FE_MEMUTILS_H

/*
 * Flags for pg_malloc_extended and palloc_extended, deliberately named
 * the same as the backend flags.
 */
#define MCXT_ALLOC_HUGE			0x01	/* allow huge allocation (> 1 GB) not
										 * actually used for frontends */
#define MCXT_ALLOC_NO_OOM		0x02	/* no failure if out-of-memory */
#define MCXT_ALLOC_ZERO			0x04	/* zero allocated memory */

/*
 * "Safe" memory allocation functions --- these exit(1) on failure
 * (except pg_malloc_extended with MCXT_ALLOC_NO_OOM)
 */
extern char *pg_strdup(const char *in);
extern void *pg_malloc(size_t size);
extern void *pg_malloc0(size_t size);
extern void *pg_malloc_extended(size_t size, int flags);
extern void *pg_realloc(void *ptr, size_t size);
extern void pg_free(void *ptr);

/*
 * Support for safe calculation of memory request sizes
 */
extern Size add_size(Size s1, Size s2);
extern Size mul_size(Size s1, Size s2);
extern void *pg_malloc_mul(Size s1, Size s2);
extern void *pg_malloc0_mul(Size s1, Size s2);
extern void *pg_malloc_mul_extended(Size s1, Size s2, int flags);
extern void *pg_realloc_mul(void *p, Size s1, Size s2);

/*
 * Variants with easier notation and more type safety
 */

/*
 * Allocate space for one object of type "type"
 */
#define pg_malloc_object(type) ((type *) pg_malloc(sizeof(type)))
#define pg_malloc0_object(type) ((type *) pg_malloc0(sizeof(type)))

/*
 * Allocate space for "count" objects of type "type"
 */
#define pg_malloc_array(type, count) ((type *) pg_malloc_mul(sizeof(type), count))
#define pg_malloc0_array(type, count) ((type *) pg_malloc0_mul(sizeof(type), count))
#define pg_malloc_array_extended(type, count, flags) ((type *) pg_malloc_mul_extended(sizeof(type), count, flags))

/*
 * Change size of allocation pointed to by "pointer" to have space for "count"
 * objects of type "type"
 */
#define pg_realloc_array(pointer, type, count) ((type *) pg_realloc_mul(pointer, sizeof(type), count))

/* Equivalent functions, deliberately named the same as backend functions */
extern char *pstrdup(const char *in);
extern char *pnstrdup(const char *in, Size size);
extern void *palloc(Size size);
extern void *palloc0(Size size);
extern void *palloc_extended(Size size, int flags);
extern void *repalloc(void *pointer, Size size);
extern void pfree(void *pointer);
extern void *palloc_mul(Size s1, Size s2);
extern void *palloc0_mul(Size s1, Size s2);
extern void *palloc_mul_extended(Size s1, Size s2, int flags);
extern void *repalloc_mul(void *p, Size s1, Size s2);

#define palloc_object(type) ((type *) palloc(sizeof(type)))
#define palloc0_object(type) ((type *) palloc0(sizeof(type)))
#define palloc_array(type, count) ((type *) palloc_mul(sizeof(type), count))
#define palloc0_array(type, count) ((type *) palloc0_mul(sizeof(type), count))
#define palloc_array_extended(type, count, flags) ((type *) palloc_mul_extended(sizeof(type), count, flags))
#define repalloc_array(pointer, type, count) ((type *) repalloc_mul(pointer, sizeof(type), count))

/* sprintf into a palloc'd buffer --- these are in psprintf.c */
extern char *psprintf(const char *fmt,...) pg_attribute_printf(1, 2);
extern size_t pvsnprintf(char *buf, size_t len, const char *fmt, va_list args) pg_attribute_printf(3, 0);

#endif							/* FE_MEMUTILS_H */
