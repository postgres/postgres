/*-------------------------------------------------------------------------
 *
 * geqo_copy.h--
 *    prototypes for copy functions in optimizer/geqo
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: geqo_copy.h,v 1.1 1997/02/19 12:58:32 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

/* contributed by:
   =*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=
   *  Martin Utesch              * Institute of Automatic Control      *
   =                             = University of Mining and Technology =
   *  utesch@aut.tu-freiberg.de  * Freiberg, Germany                   *
   =*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=
 */

#ifndef	GEQO_COPY_H
#define	GEQO_COPY_H


extern void geqo_copy (Chromosome *chromo1, Chromosome *chromo2, int string_length);

#endif  /* GEQO_COPY_H */
