/*-------------------------------------------------------------------------
 *
 * rint.c
 *	  rint() implementation
 *
 * Copyright (c) 1999, repas AEG Automation GmbH
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/port/qnx4/Attic/rint.c,v 1.3 2001/08/24 14:07:49 petere Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"
#include <math.h>

double
rint(double x)
{
	double		f,
				n = 0.;

	f = modf(x, &n);

	if (x > 0.)
	{
		if (f > .5)
			n += 1.;
	}
	else if (x < 0.)
	{
		if (f < -.5)
			n -= 1.;
	}
	return n;
}
