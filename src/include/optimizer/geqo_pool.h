/*-------------------------------------------------------------------------
 *
 * geqo_pool.h--
 *	  pool representation in optimizer/geqo
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: geqo_pool.h,v 1.2 1997/09/07 04:59:03 momjian Exp $
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


extern Pool    *alloc_pool(int pool_size, int string_length);
extern void		free_pool(Pool * pool);

extern void		random_init_pool(Query * root, Pool * pool, int strt, int stop);
extern Chromosome *alloc_chromo(int string_length);
extern void		free_chromo(Chromosome * chromo);

extern void		spread_chromo(Chromosome * chromo, Pool * pool);

extern void		sort_pool(Pool * pool);

#endif							/* GEQO_POOL_H */
