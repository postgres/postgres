/*-------------------------------------------------------------------------
 *
 * geqo_mutation.h
 *	  prototypes for mutation functions in optimizer/geqo
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/optimizer/geqo_mutation.h,v 1.21 2009/01/01 17:24:00 momjian Exp $
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

#include "optimizer/geqo_gene.h"

extern void geqo_mutation(Gene *tour, int num_gene);

#endif   /* GEQO_MUTATION_H */
