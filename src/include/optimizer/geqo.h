/*-------------------------------------------------------------------------
 *
 * geqo.h--
 *	  prototypes for various files in optimizer/geqo
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: geqo.h,v 1.7 1997/11/26 01:13:23 momjian Exp $
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

#ifndef GEQO_H
#define GEQO_H

#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/relation.h"
#include "optimizer/geqo_gene.h"

/* GEQO debug flag */
/*
 #define GEQO_DEBUG
*/

/* recombination mechanism */
/*
 #define ERX
 #define PMX
 #define CX
 #define PX
 #define OX1
 #define OX2
 */
#define ERX

/* genetic algorithm parameters */

#define GEQO_FILE "pg_geqo"		/* Name of the ga config file */

#define MIN_POOL 128			/* minimum number of individuals */
#define MAX_POOL 1024			/* maximum number of individuals */

#define LOW_EFFORT 1			/* optimization effort values */
#define MEDIUM_EFFORT 40		/* are multipliers for computed */
#define HIGH_EFFORT 80			/* number of generations */

#define SELECTION_BIAS 2.0		/* selective pressure within population */
 /* should be 1.5 <= SELECTION_BIAS <= 2.0 */

int			PoolSize;
int			Generations;

long		RandomSeed;			/* defaults to (long) time(NULL) in
								 * geqo_params.c */
double		SelectionBias;

/* logarithmic base for rel->size decrease in case of long
   queries that cause an integer overflow; used in geqo_eval.c */

#define GEQO_LOG_BASE 1.5		/* should be 1.0 < GEQO_LOG_BASE <= 2.0 */
 /* ^^^						*/

/* geqo prototypes */
extern Rel *geqo(Query *root);

extern void geqo_params(int string_length);

extern Cost geqo_eval(Query *root, Gene *tour, int num_gene);
double		geqo_log(double x, double b);
extern Rel *gimme_tree(Query *root, Gene *tour, int rel_count, int num_gene, Rel *outer_rel);


#endif							/* GEQO_H */
