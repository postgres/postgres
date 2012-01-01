/*-------------------------------------------------------------------------
 *
 * geqo_mutation.h
 *	  prototypes for mutation functions in optimizer/geqo
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/geqo_mutation.h
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

#ifndef GEQO_MUTATION_H
#define GEQO_MUTATION_H

#include "optimizer/geqo.h"


extern void geqo_mutation(PlannerInfo *root, Gene *tour, int num_gene);

#endif   /* GEQO_MUTATION_H */
