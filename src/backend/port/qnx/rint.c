/*-------------------------------------------------------------------------
 *
 * rint.c
 *	  rint() implementation
 *
 * Copyright (c) 1999, repas AEG Automation GmbH
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/port/qnx/Attic/rint.c,v 1.1 1999/12/16 01:25:06 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <math.h>
#include "os.h"

double rint(double x)
{
  double f, n = 0.;

  f = modf( x, &n );

  if( x > 0. )  {
    if( f > .5 ) n += 1.;
  }
  else if( x < 0. )  {
    if( f < -.5 ) n -= 1.;
  }
  return n;
}
