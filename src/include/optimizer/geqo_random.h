/*-------------------------------------------------------------------------
 *
 * geqo_random.h--
 *	  random number generator
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: geqo_random.h,v 1.2 1997/09/07 04:59:03 momjian Exp $
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

#define MASK 2147483647

#define geqo_rand() ((double)random()/MASK)

/* geqo_randint returns integer value
   between lower and upper inclusive */

#define geqo_randint(upper,lower) ( (int) floor( geqo_rand()*((upper-lower)+0.999999) )  +	lower )

#endif							/* GEQO_RANDOM_H */
