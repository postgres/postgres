/*-------------------------------------------------------------------------
 *
 * geqo_copy.h
 *	  prototypes for copy functions in optimizer/geqo
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: geqo_copy.h,v 1.7 1999/02/13 23:21:44 momjian Exp $
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

#ifndef GEQO_COPY_H
#define GEQO_COPY_H

#include "optimizer/geqo_gene.h"

extern void geqo_copy(Chromosome *chromo1, Chromosome *chromo2, int string_length);

#endif	 /* GEQO_COPY_H */
