/*-------------------------------------------------------------------------
 *
 * geqo_copy.h
 *	  prototypes for copy functions in optimizer/geqo
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/optimizer/geqo_copy.h,v 1.18 2006/03/05 15:58:57 momjian Exp $
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

#endif   /* GEQO_COPY_H */
