/* $Id: srandom.c,v 1.7 1999/07/15 15:19:34 momjian Exp $ */

#include <stdlib.h>
#include <math.h>				/* for pow() prototype */
#include <errno.h>

void
srandom(unsigned int seed)
{
	srand48((long int) seed);
}
