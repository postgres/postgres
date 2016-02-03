/*-------------------------------------------------------------------------
 *
 * fe_memutils.c
 *	  memory management support for frontend code
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
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
	if (ptr != NULL)
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

void *
repalloc(void *pointer, Size size)
{
	return pg_realloc(pointer, size);
}
