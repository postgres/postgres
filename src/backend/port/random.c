/* $Id: random.c,v 1.7 1999/07/15 15:19:33 momjian Exp $ */

#include <stdlib.h>
#include <math.h>				/* for pow() prototype */
#include <errno.h>

long
random()
{
	return lrand48();
}
