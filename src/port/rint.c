/*-------------------------------------------------------------------------
 *
 * rint.c
 *	  rint() implementation
 *
 * IDENTIFICATION
 *	  src/port/rint.c
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
