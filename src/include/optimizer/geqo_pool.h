/*-------------------------------------------------------------------------
 *
 * geqo_pool.h
 *	  pool representation in optimizer/geqo
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: geqo_pool.h,v 1.17 2003/08/04 02:40:14 momjian Exp $
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

#include "optimizer/geqo_gene.h"
#include "nodes/parsenodes.h"

extern Pool *alloc_pool(int pool_size, int string_length);
extern void free_pool(Pool *pool);

extern void random_init_pool(Query *root, List *initial_rels,
				 Pool *pool, int strt, int stop);
extern Chromosome *alloc_chromo(int string_length);
extern void free_chromo(Chromosome *chromo);

extern void spread_chromo(Chromosome *chromo, Pool *pool);

extern void sort_pool(Pool *pool);

#endif   /* GEQO_POOL_H */
