/*-------------------------------------------------------------------------
 *
 * srandom.c
 *	  srandom() wrapper
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/port/srandom.c,v 1.2 2003/11/11 23:52:45 momjian Exp $
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
