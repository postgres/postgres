/* $Id: srandom.c,v 1.3 1997/12/19 13:34:31 scrappy Exp $ */

#include <math.h>               /* for pow() prototype */

#include <errno.h>
#include "rusagestub.h"
#include "port-protos.h"

void
srandom(unsigned int seed)
{
	srand48((long int) seed);
}

