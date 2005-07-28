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
 *	  $PostgreSQL: pgsql/src/port/srandom.c,v 1.6 2005/07/28 04:03:14 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include <math.h>


void
srandom(unsigned int seed)
{
	srand48((long int) seed);
}
