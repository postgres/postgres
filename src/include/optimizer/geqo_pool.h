/*-------------------------------------------------------------------------
 *
 * geqo_pool.h
 *	  pool representation in optimizer/geqo
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/geqo_pool.h
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


#ifndef GEQO_POOL_H
#define GEQO_POOL_H

#include "optimizer/geqo.h"


extern Pool *alloc_pool(PlannerInfo *root, int pool_size, int string_length);
extern void free_pool(PlannerInfo *root, Pool *pool);

extern void random_init_pool(PlannerInfo *root, Pool *pool);
extern Chromosome *alloc_chromo(PlannerInfo *root, int string_length);
extern void free_chromo(PlannerInfo *root, Chromosome *chromo);

extern void spread_chromo(PlannerInfo *root, Chromosome *chromo, Pool *pool);

extern void sort_pool(PlannerInfo *root, Pool *pool);

#endif   /* GEQO_POOL_H */
