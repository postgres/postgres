/*-------------------------------------------------------------------------
 *
 * random.c
 *	  random() wrapper
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/random.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include <math.h>


long
random(void)
{
	return pg_lrand48();
}
