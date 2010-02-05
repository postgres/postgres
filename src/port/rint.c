/*-------------------------------------------------------------------------
 *
 * rint.c
 *	  rint() implementation
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/port/rint.c,v 1.3 2010/02/05 03:20:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"
#include <math.h>

double
rint(double x)
{
	return (x > 0.0) ? floor(x + 0.5) : ceil(x - 0.5);
}
