/* $Id: random.c,v 1.8 1999/07/16 03:13:06 momjian Exp $ */

#include <stdlib.h>
#include <math.h>
#include <errno.h>

long
random()
{
	return lrand48();
}
