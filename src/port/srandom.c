/* $Id: srandom.c,v 1.1 2002/07/18 04:13:59 momjian Exp $ */

#include "c.h"

#include <stdlib.h>
#include <math.h>
#include <errno.h>

void
srandom(unsigned int seed)
{
	srand48((long int) seed);
}
