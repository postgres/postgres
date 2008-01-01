/*-------------------------------------------------------------------------
 *
 * geqo_pool.h
 *	  pool representation in optimizer/geqo
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/optimizer/geqo_pool.h,v 1.24 2008/01/01 19:45:58 momjian Exp $
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


extern Pool *alloc_pool(int pool_size, int string_length);
extern void free_pool(Pool *pool);

extern void random_init_pool(Pool *pool, GeqoEvalData *evaldata);
extern Chromosome *alloc_chromo(int string_length);
extern void free_chromo(Chromosome *chromo);

extern void spread_chromo(Chromosome *chromo, Pool *pool);

extern void sort_pool(Pool *pool);

#endif   /* GEQO_POOL_H */
