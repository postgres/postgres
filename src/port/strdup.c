/*-------------------------------------------------------------------------
 *
 * strdup.c
 *	  copies a null-terminated string.
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/port/strdup.c,v 1.15 2009/01/01 17:24:04 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"


char *
strdup(const char *string)
{
	char	   *nstr;

	nstr = (char *) malloc(strlen(string) + 1);
	if (nstr)
		strcpy(nstr, string);
	return nstr;
}
