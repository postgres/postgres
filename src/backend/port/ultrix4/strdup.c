/*-------------------------------------------------------------------------
 *
 * strdup.c--
 *    copies a null-terminated string.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/port/ultrix4/Attic/strdup.c,v 1.1.1.1 1996/07/09 06:21:45 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>

char *
strdup(char *string)
{
    char *nstr;

    nstr = strcpy((char *)palloc(strlen(string)+1), string);
    return nstr;
}
