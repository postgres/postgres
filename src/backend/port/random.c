/* $Id: random.c,v 1.6 1999/02/07 22:07:02 tgl Exp $ */

#include <stdlib.h>
#include <math.h>				/* for pow() prototype */
#include <errno.h>

#include "config.h"
#include "rusagestub.h"

long
random()
{
	return lrand48();
}
