#include <math.h>               /* for pow() prototype */

#include <errno.h>
#include "rusagestub.h"
#include "port-protos.h"

void
srandom(unsigned int seed)
{
	srand48((long int) seed);
}

