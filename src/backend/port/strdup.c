
/*-------------------------------------------------------------------------
 *
 * strdup.c--
 *	  copies a null-terminated string.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/port/Attic/strdup.c,v 1.1 1997/12/19 02:56:17 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>

#include <utils/palloc.h>

#include "port-protos.h"

char	   *
strdup(char const * string)
{
	char	   *nstr;

	nstr = strcpy((char *) palloc(strlen(string) + 1), string);
	return nstr;
}
