/*-------------------------------------------------------------------------
 *
 * geqo.h
 *	  prototypes for various files in optimizer/geqo
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: geqo.h,v 1.18 2000/01/26 05:58:20 momjian Exp $
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

/* parameter values set in geqo_params.c */
extern int	PoolSize;
extern int	Generations;
extern long RandomSeed;
extern double SelectionBias;

/* routines in geqo_main.c */
extern RelOptInfo *geqo(Query *root);

/* routines in geqo_params.c */
extern void geqo_params(int string_length);

/* routines in geqo_eval.c */
extern void geqo_eval_startup(void);
extern Cost geqo_eval(Query *root, Gene *tour, int num_gene);
extern RelOptInfo *gimme_tree(Query *root, Gene *tour, int rel_count,
		   int num_gene, RelOptInfo *old_rel);

#endif	 /* GEQO_H */
