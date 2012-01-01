/*-------------------------------------------------------------------------
 *
 * dumpmem.c
 *	  Memory allocation routines used by pg_dump, pg_dumpall, and pg_restore
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/bin/pg_dump/dumpmem.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "dumputils.h"
#include "dumpmem.h"


/*
 * Safer versions of some standard C library functions. If an
 * out-of-memory condition occurs, these functions will bail out via exit();
 *therefore, their return value is guaranteed to be non-NULL.
 */

char *
pg_strdup(const char *string)
{
	char	   *tmp;

	if (!string)
		exit_horribly(NULL, "cannot duplicate null pointer\n");
	tmp = strdup(string);
	if (!tmp)
		exit_horribly(NULL, "out of memory\n");
	return tmp;
}

void *
pg_malloc(size_t size)
{
	void	   *tmp;

	tmp = malloc(size);
	if (!tmp)
		exit_horribly(NULL, "out of memory\n");
	return tmp;
}

void *
pg_calloc(size_t nmemb, size_t size)
{
	void	   *tmp;

	tmp = calloc(nmemb, size);
	if (!tmp)
		exit_horribly(NULL, "out of memory\n");
	return tmp;
}

void *
pg_realloc(void *ptr, size_t size)
{
	void	   *tmp;

	tmp = realloc(ptr, size);
	if (!tmp)
		exit_horribly(NULL, "out of memory\n");
	return tmp;
}
