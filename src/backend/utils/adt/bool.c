/*-------------------------------------------------------------------------
 *
 * bool.c--
 *    Functions for the built-in type "bool".
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/adt/bool.c,v 1.2 1996/11/03 06:53:03 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/builtins.h"	/* where the declarations go */
#include "utils/palloc.h"

/***************************************************************************** 
 *   USER I/O ROUTINES                                                       *
 *****************************************************************************/

/*
 *	boolin		- converts "t" or "f" to 1 or 0
 */
int32
boolin(char *b)
{
    if (b == NULL)
	elog(WARN, "Bad input string for type bool");
    return((int32) (*b == 't') || (*b == 'T'));
}

/*
 *	boolout		- converts 1 or 0 to "t" or "f"
 */
char *
boolout(long b)
{
    char	*result = (char *) palloc(2);
    
    *result = (b) ? 't' : 'f';
    result[1] = '\0';
    return(result);
}

/***************************************************************************** 
 *   PUBLIC ROUTINES                                                         *
 *****************************************************************************/

int32
booleq(int8 arg1, int8 arg2)	
{ 
    return(arg1 == arg2); 
}

int32
boolne(int8 arg1, int8 arg2)	
{
    return(arg1 != arg2); 
}





