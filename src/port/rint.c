/*-------------------------------------------------------------------------
 *
 * rint.c
 *	  rint() implementation
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/port/rint.c,v 1.4 2010/02/06 05:42:49 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <math.h>

double
rint(double x)
{
	return (x >= 0.0) ? floor(x + 0.5) : ceil(x - 0.5);
}
