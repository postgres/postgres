/*-------------------------------------------------------------------------
 *
 * strdup.c
 *	  copies a null-terminated string.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/port/strdup.c,v 1.8 2005/09/27 04:53:23 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include "strdup.h"


char *
strdup(const char *string)
{
	char	   *nstr;

	nstr = (char *) malloc(strlen(string) + 1);
	if (nstr)
		strcpy(nstr, string);
	return nstr;
}
