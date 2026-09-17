/*-------------------------------------------------------------------------
 *
 * geqo_gene.h
 *	  genome representation in optimizer/geqo
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/geqo_gene.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * contributed by:
 * =*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=
 * *  Martin Utesch				 * Institute of Automatic Control	   *
 * =							 = University of Mining and Technology =
 * *  utesch@aut.tu-freiberg.de  * Freiberg, Germany				   *
 * =*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=
 */


#ifndef GEQO_GENE_H
#define GEQO_GENE_H

#include <float.h>
#include <limits.h>

#include "nodes/nodes.h"

/*
 * we presume that int instead of Relid
 * is o.k. for Gene; so don't change it!
 */
typedef int Gene;

/*
 * Fitness of a candidate join order.
 *
 * A tour for which no valid plan could be constructed is represented by
 * disabled_nodes = INT_MAX and cost = DBL_MAX.
 */
typedef struct Fitness
{
	int			disabled_nodes;
	Cost		cost;
} Fitness;

/*
 * Is this a valid tour?
 */
static inline bool
fitness_is_valid(Fitness fitness)
{
	return fitness.disabled_nodes < INT_MAX || fitness.cost < DBL_MAX;
}

/*
 * Which fitness is better?
 */
static inline int
fitness_compare(Fitness fitness1, Fitness fitness2)
{
	if (fitness1.disabled_nodes != fitness2.disabled_nodes)
		return fitness1.disabled_nodes < fitness2.disabled_nodes ? -1 : 1;
	if (fitness1.cost != fitness2.cost)
		return fitness1.cost < fitness2.cost ? -1 : 1;
	return 0;
}

typedef struct Chromosome
{
	Gene	   *string;
	Fitness		worth;
} Chromosome;

typedef struct Pool
{
	Chromosome *data;
	int			size;
	int			string_length;
} Pool;

#endif							/* GEQO_GENE_H */
