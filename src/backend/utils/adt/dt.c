/*-------------------------------------------------------------------------
 *
 * dt.c--
 *    Functions for the built-in type "dt".
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/adt/Attic/dt.c,v 1.1.1.1 1996/07/09 06:22:04 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"
#include "utils/palloc.h"
#include "utils/builtins.h"		/* where function declarations go */


/***************************************************************************** 
 *   USER I/O ROUTINES                                                       *
 *****************************************************************************/

/*
 *	dtin		- converts "nseconds" to internal representation
 *
 *	XXX Currently, just creates an integer.
 */
int32 dtin(char *datetime)
{
    if (datetime == NULL)
	return((int32) 0);
    return((int32) atol(datetime));
}

/*
 *	dtout		- converts internal form to "..."
 *
 *	XXX assumes sign, 10 digits max, '\0'
 */
char *dtout(int32 datetime)
{
    char		*result;
    
    result = (char *) palloc(12);
    Assert(result);
    ltoa(datetime, result);
    return(result);
}

/***************************************************************************** 
 *   PUBLIC ROUTINES                                                         *
 *****************************************************************************/
/* (see int.c for comparison/operation routines) */

/***************************************************************************** 
 *   PRIVATE ROUTINES                                                        *
 *****************************************************************************/
/* (none) */
