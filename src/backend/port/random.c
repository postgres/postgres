/* $Id: random.c,v 1.2 1997/12/19 13:34:29 scrappy Exp $ */

#include <math.h>               /* for pow() prototype */

#include <errno.h>
#include "rusagestub.h"
#include "port-protos.h"

long
random()
{
	return (lrand48());
}

