/*-------------------------------------------------------------------------
 *
 * strdup.c
 *	  copies a null-terminated string.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/utils/Attic/strdup.c,v 1.8 2001/01/24 19:43:33 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include <stdlib.h>
#include "strdup.h"

char *
strdup(char const * string)
{
	char	   *nstr;

	nstr = strcpy((char *) malloc(strlen(string) + 1), string);
	return nstr;
}
