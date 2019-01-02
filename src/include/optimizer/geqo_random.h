/*-------------------------------------------------------------------------
 *
 * geqo_random.h
 *	  random number generator
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/geqo_random.h
 *
 *-------------------------------------------------------------------------
 */

/* contributed by:
   =*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=
   *  Martin Utesch				 * Institute of Automatic Control	   *
   =							 = University of Mining and Technology =
   *  utesch@aut.tu-freiberg.de  * Freiberg, Germany				   *
   =*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=
 */

/* -- parts of this are adapted from D. Whitley's Genitor algorithm -- */

#ifndef GEQO_RANDOM_H
#define GEQO_RANDOM_H

#include <math.h>

#include "optimizer/geqo.h"


extern void geqo_set_seed(PlannerInfo *root, double seed);

/* geqo_rand returns a random float value between 0 and 1 inclusive */
extern double geqo_rand(PlannerInfo *root);

/* geqo_randint returns integer value between lower and upper inclusive */
#define geqo_randint(root, upper, lower) \
	( (int) floor( geqo_rand(root)*(((upper)-(lower))+0.999999) ) + (lower) )

#endif							/* GEQO_RANDOM_H */
