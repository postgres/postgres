/*-------------------------------------------------------------------------
 *
 * strdup.c--
 *    copies a null-terminated string.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/port/ultrix4/Attic/strdup.c,v 1.2 1996/11/26 03:19:04 bryanh Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>

#include <utils/palloc.h>

#include "port-protos.h"

char *
strdup(char const *string)
{
    char *nstr;

    nstr = strcpy((char *)palloc(strlen(string)+1), string);
    return nstr;
}
