/*-------------------------------------------------------------------------
 *
 * quotes.c
 *	  string quoting and escaping functions
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/quotes.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

/*
 * Escape (by doubling) any single quotes or backslashes in given string
 *
 * Note: this is used to process postgresql.conf entries and to quote
 * string literals in pg_basebackup for creating recovery.conf.
 * Since postgresql.conf strings are defined to treat backslashes as escapes,
 * we have to double backslashes here.
 *
 * Since this function is only used for parsing or creating configuration
 * files, we do not care about encoding considerations.
 *
 * Returns a malloced() string that it's the responsibility of the caller
 * to free.
 */
char *
escape_single_quotes_ascii(const char *src)
{
	int			len = strlen(src),
				i,
				j;
	char	   *result = malloc(len * 2 + 1);

	if (!result)
		return NULL;

	for (i = 0, j = 0; i < len; i++)
	{
		if (SQL_STR_DOUBLE(src[i], true))
			result[j++] = src[i];
		result[j++] = src[i];
	}
	result[j] = '\0';
	return result;
}
