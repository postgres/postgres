/* $Id: random.c,v 1.4 1998/02/26 04:34:11 momjian Exp $ */

#include <math.h>				/* for pow() prototype */

#include <errno.h>
#include "rusagestub.h"

long
random()
{
	return (lrand48());
}
