/*-------------------------------------------------------------------------
 *
 * srandom.c
 *	  srandom() wrapper
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/port/srandom.c,v 1.5 2004/12/31 22:03:53 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include <stdlib.h>
#include <math.h>
#include <errno.h>

void
srandom(unsigned int seed)
{
	srand48((long int) seed);
}
