/* $Id: srandom.c,v 1.4 1998/02/24 03:45:07 scrappy Exp $ */

#include <math.h>               /* for pow() prototype */

#include <errno.h>
#include "rusagestub.h"

void
srandom(unsigned int seed)
{
	srand48((long int) seed);
}

