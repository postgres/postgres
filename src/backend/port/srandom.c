/* $Id: srandom.c,v 1.8 1999/07/16 03:13:08 momjian Exp $ */

#include <stdlib.h>
#include <math.h>
#include <errno.h>

void
srandom(unsigned int seed)
{
	srand48((long int) seed);
}
