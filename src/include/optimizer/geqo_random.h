/*-------------------------------------------------------------------------
 *
 * geqo_random.h
 *	  random number generator
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: geqo_random.h,v 1.6 2000/01/26 05:58:20 momjian Exp $
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

#define GEQOMASK 2147483647

#define geqo_rand() ((double)random()/GEQOMASK)

/* geqo_randint returns integer value
   between lower and upper inclusive */

#define geqo_randint(upper,lower) ( (int) floor( geqo_rand()*((upper-lower)+0.999999) )  +	lower )

#endif	 /* GEQO_RANDOM_H */
