/*-------------------------------------------------------------------------
 *
 * strdup.c--
 *    copies a null-terminated string.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/utils/Attic/strdup.c,v 1.2 1996/11/28 03:32:18 bryanh Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include <stdlib.h>
#include "strdup.h"

char *
strdup(char const *string)
{
    char *nstr;

    nstr = strcpy((char *)malloc(strlen(string)+1), string);
    return nstr;
}
