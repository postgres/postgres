/*-------------------------------------------------------------------------
 *
 * pgtclId.c--
 *    useful routines to convert between strings and pointers
 *  Needed because everything in tcl is a string, but we want pointers
 *  to data structures
 *
 *  ASSUMPTION:  sizeof(long) >= sizeof(void*)
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/interfaces/libpgtcl/Attic/pgtclId.c,v 1.1.1.1 1996/07/09 06:22:16 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <stdlib.h>
#include <string.h>
#include "tcl.h"

#include "pgtclId.h"

/* convert a pointer into a string */
void
PgSetId(char *id, void *ptr)
{
  (void) sprintf(id, "pgp%lx", (long) ptr);
}


/* get back a pointer from a string */
void *
PgGetId(char *id)
{
  long ptr; 
  ptr = strtol(id+3, NULL, 16);
  return (void *) ptr;
}

/* check to see if the string is a valid pgtcl pointer */
int 
PgValidId(char* id)
{
    if ( (strlen(id) > 3) && id[0]=='p' && id[1] == 'g' && id[2] == 'p')
	return 1;
    else
	return 0;
}
