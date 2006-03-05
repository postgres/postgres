/*-------------------------------------------------------------------------
 *
 * random.c
 *	  random() wrapper
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/port/random.c,v 1.7 2006/03/05 15:59:10 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include <math.h>


long
random()
{
	return lrand48();
}
