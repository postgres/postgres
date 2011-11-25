/*-------------------------------------------------------------------------
 *
 * common.c
 *	  common routines between pg_dump and pg_restore (but not pg_dumpall
 *	  because there is no failure location to report).
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/bin/pg_dump/common.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"
#include "pg_backup.h"
#include "common.h"

#include <ctype.h>

/*
 * Safer versions of some standard C library functions. If an
 * out-of-memory condition occurs, these functions will bail out
 * safely; therefore, their return value is guaranteed to be non-NULL.
 * We also report the program name and close the database connection.
 */

char *
pg_strdup(const char *string)
{
	char	   *tmp;

	if (!string)
		exit_horribly(NULL, NULL, "cannot duplicate null pointer\n");
	tmp = strdup(string);
	if (!tmp)
		exit_horribly(NULL, NULL, "out of memory\n");
	return tmp;
}

void *
pg_malloc(size_t size)
{
	void	   *tmp;

	tmp = malloc(size);
	if (!tmp)
		exit_horribly(NULL, NULL, "out of memory\n");
	return tmp;
}

void *
pg_calloc(size_t nmemb, size_t size)
{
	void	   *tmp;

	tmp = calloc(nmemb, size);
	if (!tmp)
		exit_horribly(NULL, NULL, _("out of memory\n"));
	return tmp;
}

void *
pg_realloc(void *ptr, size_t size)
{
	void	   *tmp;

	tmp = realloc(ptr, size);
	if (!tmp)
		exit_horribly(NULL, NULL, _("out of memory\n"));
	return tmp;
}
