/*-------------------------------------------------------------------------
 *
 * geqo.h
 *	  prototypes for various files in optimizer/geqo
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: geqo.h,v 1.32 2003/09/07 15:26:54 tgl Exp $
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
/* If you change these, update backend/utils/misc/postgresql.sample.conf */
extern int	Geqo_pool_size;

#define DEFAULT_GEQO_POOL_SIZE 0	/* = default based on no. of relations. */
#define MIN_GEQO_POOL_SIZE 128
#define MAX_GEQO_POOL_SIZE 1024

extern int	Geqo_effort;		/* 1 .. inf, only used to calculate
								 * generations default */
extern int	Geqo_generations;	/* 1 .. inf, or 0 to use default based on
								 * pool size */

extern double Geqo_selection_bias;

/* If you change these, update backend/utils/misc/postgresql.sample.conf */
#define DEFAULT_GEQO_SELECTION_BIAS 2.0
#define MIN_GEQO_SELECTION_BIAS 1.5
#define MAX_GEQO_SELECTION_BIAS 2.0


/* routines in geqo_main.c */
extern RelOptInfo *geqo(Query *root, int number_of_rels, List *initial_rels);

/* routines in geqo_eval.c */
extern Cost geqo_eval(Query *root, List *initial_rels,
		  Gene *tour, int num_gene);
extern RelOptInfo *gimme_tree(Query *root, List *initial_rels,
		   Gene *tour, int num_gene);

#endif   /* GEQO_H */
