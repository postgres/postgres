/*-------------------------------------------------------------------------
 *
 * geqo.h
 *	  prototypes for various files in optimizer/geqo
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: geqo.h,v 1.19 2000/05/31 00:28:38 petere Exp $
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


/*
 * Configuration options
 */
extern int  	    Geqo_pool_size;
#define DEFAULT_GEQO_POOL_SIZE 0 /* = default based on no. of relations. */
#define MIN_GEQO_POOL_SIZE 128
#define MAX_GEQO_POOL_SIZE 1024

extern int          Geqo_effort; /* 1 .. inf, only used to calculate generations default */
extern int  	    Geqo_generations; /* 1 .. inf, or 0 to use default based on pool size */

extern double 		Geqo_selection_bias;
#define DEFAULT_GEQO_SELECTION_BIAS 2.0
#define MIN_GEQO_SELECTION_BIAS 1.5
#define MAX_GEQO_SELECTION_BIAS 2.0

extern int          Geqo_random_seed; /* or negative to use current time */


/* routines in geqo_main.c */
extern RelOptInfo *geqo(Query *root);

/* routines in geqo_eval.c */
extern void geqo_eval_startup(void);
extern Cost geqo_eval(Query *root, Gene *tour, int num_gene);
extern RelOptInfo *gimme_tree(Query *root, Gene *tour, int rel_count,
		   int num_gene, RelOptInfo *old_rel);

#endif	 /* GEQO_H */
