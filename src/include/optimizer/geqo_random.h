/*-------------------------------------------------------------------------
 *
 * geqo_random.h--
 *	  random number generator
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: geqo_random.h,v 1.3 1998/09/01 04:37:04 momjian Exp $
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

#endif	 /* GEQO_RANDOM_H */
