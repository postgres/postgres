/*-------------------------------------------------------------------------
 *
 * geqo_random.h
 *	  random number generator
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: geqo_random.h,v 1.13 2003/08/04 02:40:14 momjian Exp $
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

/* geqo_rand returns a random float value between 0 and 1 inclusive */

#define geqo_rand() (((double) random()) / ((double) MAX_RANDOM_VALUE))

/* geqo_randint returns integer value between lower and upper inclusive */

#define geqo_randint(upper,lower) \
	( (int) floor( geqo_rand()*(((upper)-(lower))+0.999999) ) + (lower) )

#endif   /* GEQO_RANDOM_H */
