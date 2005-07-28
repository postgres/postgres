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
 *	  $PostgreSQL: pgsql/src/port/strdup.c,v 1.7 2005/07/28 04:03:14 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include "strdup.h"


char *
strdup(char const * string)
{
	char	   *nstr;

	nstr = strcpy((char *) malloc(strlen(string) + 1), string);
	return nstr;
}
