/* $Id: random.c,v 1.5 1998/09/01 03:24:30 momjian Exp $ */

#include <math.h>				/* for pow() prototype */

#include <errno.h>
#include "rusagestub.h"

long
random()
{
	return lrand48();
}
