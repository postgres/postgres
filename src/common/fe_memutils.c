/*-------------------------------------------------------------------------
 *
 * fe_memutils.c
 *	  memory management support for frontend code
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/common/fe_memutils.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#error "This file is not expected to be compiled for backend code"
#endif

#include "postgres_fe.h"

#include "common/int.h"

pg_noreturn static pg_noinline void add_size_error(Size s1, Size s2);
pg_noreturn static pg_noinline void mul_size_error(Size s1, Size s2);


static inline void *
pg_malloc_internal(size_t size, int flags)
{
	void	   *tmp;

	/* Avoid unportable behavior of malloc(0) */
	if (size == 0)
		size = 1;
	tmp = malloc(size);
	if (tmp == NULL)
	{
		if ((flags & MCXT_ALLOC_NO_OOM) == 0)
		{
			fprintf(stderr, _("out of memory\n"));
			exit(EXIT_FAILURE);
		}
		return NULL;
	}

	if ((flags & MCXT_ALLOC_ZERO) != 0)
		MemSet(tmp, 0, size);
	return tmp;
}

void *
pg_malloc(size_t size)
{
	return pg_malloc_internal(size, 0);
}

void *
pg_malloc0(size_t size)
{
	return pg_malloc_internal(size, MCXT_ALLOC_ZERO);
}

void *
pg_malloc_extended(size_t size, int flags)
{
	return pg_malloc_internal(size, flags);
}

void *
pg_realloc(void *ptr, size_t size)
{
	void	   *tmp;

	/* Avoid unportable behavior of realloc(NULL, 0) */
	if (ptr == NULL && size == 0)
		size = 1;
	tmp = realloc(ptr, size);
	if (!tmp)
	{
		fprintf(stderr, _("out of memory\n"));
		exit(EXIT_FAILURE);
	}
	return tmp;
}

/*
 * "Safe" wrapper around strdup().
 */
char *
pg_strdup(const char *in)
{
	char	   *tmp;

	if (!in)
	{
		fprintf(stderr,
				_("cannot duplicate null pointer (internal error)\n"));
		exit(EXIT_FAILURE);
	}
	tmp = strdup(in);
	if (!tmp)
	{
		fprintf(stderr, _("out of memory\n"));
		exit(EXIT_FAILURE);
	}
	return tmp;
}

void
pg_free(void *ptr)
{
	free(ptr);
}

/*
 * Frontend emulation of backend memory management functions.  Useful for
 * programs that compile backend files.
 */
void *
palloc(Size size)
{
	return pg_malloc_internal(size, 0);
}

void *
palloc0(Size size)
{
	return pg_malloc_internal(size, MCXT_ALLOC_ZERO);
}

void *
palloc_extended(Size size, int flags)
{
	return pg_malloc_internal(size, flags);
}

void
pfree(void *pointer)
{
	pg_free(pointer);
}

char *
pstrdup(const char *in)
{
	return pg_strdup(in);
}

char *
pnstrdup(const char *in, Size size)
{
	char	   *tmp;
	int			len;

	if (!in)
	{
		fprintf(stderr,
				_("cannot duplicate null pointer (internal error)\n"));
		exit(EXIT_FAILURE);
	}

	len = strnlen(in, size);
	tmp = malloc(len + 1);
	if (tmp == NULL)
	{
		fprintf(stderr, _("out of memory\n"));
		exit(EXIT_FAILURE);
	}

	memcpy(tmp, in, len);
	tmp[len] = '\0';

	return tmp;
}

void *
repalloc(void *pointer, Size size)
{
	return pg_realloc(pointer, size);
}

/*
 * Support for safe calculation of memory request sizes
 *
 * These functions perform the requested calculation, but throw error if the
 * result overflows.
 *
 * An important property of these functions is that if an argument was a
 * negative signed int before promotion (implying overflow in calculating it)
 * we will detect that as an error.  That happens because we reject results
 * larger than SIZE_MAX / 2.  In the backend we rely on later checks to do
 * that, but in frontend we must do it here.
 */
Size
add_size(Size s1, Size s2)
{
	Size		result;

	if (unlikely(pg_add_size_overflow(s1, s2, &result) ||
				 result > (SIZE_MAX / 2)))
		add_size_error(s1, s2);
	return result;
}

pg_noreturn static pg_noinline void
add_size_error(Size s1, Size s2)
{
	fprintf(stderr, _("invalid memory allocation request size %zu + %zu\n"),
			s1, s2);
	exit(EXIT_FAILURE);
}

Size
mul_size(Size s1, Size s2)
{
	Size		result;

	if (unlikely(pg_mul_size_overflow(s1, s2, &result) ||
				 result > (SIZE_MAX / 2)))
		mul_size_error(s1, s2);
	return result;
}

pg_noreturn static pg_noinline void
mul_size_error(Size s1, Size s2)
{
	fprintf(stderr, _("invalid memory allocation request size %zu * %zu\n"),
			s1, s2);
	exit(EXIT_FAILURE);
}

/*
 * pg_malloc_mul
 *		Equivalent to pg_malloc(mul_size(s1, s2)).
 */
void *
pg_malloc_mul(Size s1, Size s2)
{
	/* inline mul_size() for efficiency */
	Size		req;

	if (unlikely(pg_mul_size_overflow(s1, s2, &req) ||
				 req > (SIZE_MAX / 2)))
		mul_size_error(s1, s2);
	return pg_malloc(req);
}

/*
 * pg_malloc0_mul
 *		Equivalent to pg_malloc0(mul_size(s1, s2)).
 *
 * This is comparable to standard calloc's behavior.
 */
void *
pg_malloc0_mul(Size s1, Size s2)
{
	/* inline mul_size() for efficiency */
	Size		req;

	if (unlikely(pg_mul_size_overflow(s1, s2, &req) ||
				 req > (SIZE_MAX / 2)))
		mul_size_error(s1, s2);
	return pg_malloc0(req);
}

/*
 * pg_malloc_mul_extended
 *		Equivalent to pg_malloc_extended(mul_size(s1, s2), flags).
 */
void *
pg_malloc_mul_extended(Size s1, Size s2, int flags)
{
	/* inline mul_size() for efficiency */
	Size		req;

	if (unlikely(pg_mul_size_overflow(s1, s2, &req) ||
				 req > (SIZE_MAX / 2)))
		mul_size_error(s1, s2);
	return pg_malloc_extended(req, flags);
}

/*
 * pg_realloc_mul
 *		Equivalent to pg_realloc(p, mul_size(s1, s2)).
 */
void *
pg_realloc_mul(void *p, Size s1, Size s2)
{
	/* inline mul_size() for efficiency */
	Size		req;

	if (unlikely(pg_mul_size_overflow(s1, s2, &req) ||
				 req > (SIZE_MAX / 2)))
		mul_size_error(s1, s2);
	return pg_realloc(p, req);
}

/*
 * palloc_mul
 *		Equivalent to palloc(mul_size(s1, s2)).
 */
void *
palloc_mul(Size s1, Size s2)
{
	/* inline mul_size() for efficiency */
	Size		req;

	if (unlikely(pg_mul_size_overflow(s1, s2, &req) ||
				 req > (SIZE_MAX / 2)))
		mul_size_error(s1, s2);
	return palloc(req);
}

/*
 * palloc0_mul
 *		Equivalent to palloc0(mul_size(s1, s2)).
 *
 * This is comparable to standard calloc's behavior.
 */
void *
palloc0_mul(Size s1, Size s2)
{
	/* inline mul_size() for efficiency */
	Size		req;

	if (unlikely(pg_mul_size_overflow(s1, s2, &req) ||
				 req > (SIZE_MAX / 2)))
		mul_size_error(s1, s2);
	return palloc0(req);
}

/*
 * palloc_mul_extended
 *		Equivalent to palloc_extended(mul_size(s1, s2), flags).
 */
void *
palloc_mul_extended(Size s1, Size s2, int flags)
{
	/* inline mul_size() for efficiency */
	Size		req;

	if (unlikely(pg_mul_size_overflow(s1, s2, &req) ||
				 req > (SIZE_MAX / 2)))
		mul_size_error(s1, s2);
	return palloc_extended(req, flags);
}

/*
 * repalloc_mul
 *		Equivalent to repalloc(p, mul_size(s1, s2)).
 */
void *
repalloc_mul(void *p, Size s1, Size s2)
{
	/* inline mul_size() for efficiency */
	Size		req;

	if (unlikely(pg_mul_size_overflow(s1, s2, &req) ||
				 req > (SIZE_MAX / 2)))
		mul_size_error(s1, s2);
	return repalloc(p, req);
}
