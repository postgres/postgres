/*-------------------------------------------------------------------------
 *
 * geqo_gene.h--
 *	  genome representation in optimizer/geqo
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: geqo_gene.h,v 1.6 1998/09/01 04:36:58 momjian Exp $
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


#ifndef GEQO_GENE_H
#define GEQO_GENE_H

#include "nodes/nodes.h"
#include "nodes/relation.h"
#include "optimizer/geqo_gene.h"

/* we presume that int instead of Relid
   is o.k. for Gene; so don't change it! */
typedef int Gene;

typedef struct Chromosome
{
	Gene	   *string;
	Cost		worth;
} Chromosome;

typedef struct Pool
{
	Chromosome *data;
	int			size;
	int			string_length;
} Pool;

#endif	 /* GEQO_GENE_H */
