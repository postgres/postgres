/* $Id: random.c,v 1.3 1998/02/24 03:45:07 scrappy Exp $ */

#include <math.h>               /* for pow() prototype */

#include <errno.h>
#include "rusagestub.h"

long
random()
{
	return (lrand48());
}

