/* $Id: random.c,v 1.9 1999/07/16 23:09:45 tgl Exp $ */

#include <stdlib.h>
#include <math.h>
#include <errno.h>

#include "config.h"

long
random()
{
	return lrand48();
}
