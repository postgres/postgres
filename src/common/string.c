/*-------------------------------------------------------------------------
 *
 * string.c
 *		string handling helpers
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/common/string.c
 *
 *-------------------------------------------------------------------------
 */


#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "common/string.h"


/*
 * Returns whether the string `str' has the postfix `end'.
 */
bool
pg_str_endswith(const char *str, const char *end)
{
	size_t		slen = strlen(str);
	size_t		elen = strlen(end);

	/* can't be a postfix if longer */
	if (elen > slen)
		return false;

	/* compare the end of the strings */
	str += slen - elen;
	return strcmp(str, end) == 0;
}
