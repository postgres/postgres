/*-------------------------------------------------------------------------
 *
 * strdup.c--
 *    copies a null-terminated string.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/utils/Attic/strdup.c,v 1.1 1996/11/27 01:46:52 bryanh Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include "strdup.h"

char *
strdup(char *string)
{
    char *nstr;

    nstr = strcpy((char *)palloc(strlen(string)+1), string);
    return nstr;
}
