/* $Id: srandom.c,v 1.5 1998/02/26 04:34:14 momjian Exp $ */

#include <math.h>				/* for pow() prototype */

#include <errno.h>
#include "rusagestub.h"

void
srandom(unsigned int seed)
{
	srand48((long int) seed);
}
