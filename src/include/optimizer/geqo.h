/*-------------------------------------------------------------------------
 *
 * geqo.h
 *	  prototypes for various files in optimizer/geqo
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/optimizer/geqo.h,v 1.43 2008/01/01 19:45:58 momjian Exp $
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
 *
 * If you change these, update backend/utils/misc/postgresql.sample.conf
 */
extern int	Geqo_effort;		/* 1 .. 10, knob for adjustment of defaults */

#define DEFAULT_GEQO_EFFORT 5
#define MIN_GEQO_EFFORT 1
#define MAX_GEQO_EFFORT 10

extern int	Geqo_pool_size;		/* 2 .. inf, or 0 to use default */

extern int	Geqo_generations;	/* 1 .. inf, or 0 to use default */

extern double Geqo_selection_bias;

#define DEFAULT_GEQO_SELECTION_BIAS 2.0
#define MIN_GEQO_SELECTION_BIAS 1.5
#define MAX_GEQO_SELECTION_BIAS 2.0


/*
 * Data structure to encapsulate information needed for building plan trees
 * (i.e., geqo_eval and gimme_tree).
 */
typedef struct
{
	PlannerInfo *root;			/* the query we are planning */
	List	   *initial_rels;	/* the base relations */
} GeqoEvalData;


/* routines in geqo_main.c */
extern RelOptInfo *geqo(PlannerInfo *root,
	 int number_of_rels, List *initial_rels);

/* routines in geqo_eval.c */
extern Cost geqo_eval(Gene *tour, int num_gene, GeqoEvalData *evaldata);
extern RelOptInfo *gimme_tree(Gene *tour, int num_gene,
		   GeqoEvalData *evaldata);

#endif   /* GEQO_H */
