/*------------------------------------------------------------------------
 *
 * geqo_main.c--
 *	  solution of the query optimization problem
 *	  by means of a Genetic Algorithm (GA)
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: geqo_main.c,v 1.8 1998/07/18 04:22:27 momjian Exp $
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

/* -- parts of this are adapted from D. Whitley's Genitor algorithm -- */

#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/relation.h"
#include "nodes/plannodes.h"
#include "nodes/primnodes.h"

#include "utils/palloc.h"
#include "utils/elog.h"

#include "optimizer/internal.h"
#include "optimizer/paths.h"
#include "optimizer/pathnode.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"

#include "optimizer/geqo_gene.h"
#include "optimizer/geqo.h"
#include "optimizer/geqo_pool.h"
#include "optimizer/geqo_selection.h"
#include "optimizer/geqo_recombination.h"
#include "optimizer/geqo_mutation.h"
#include "optimizer/geqo_misc.h"


/* define edge recombination crossover [ERX] per default */
#if !defined(ERX) && \
	!defined(PMX) && \
	!defined(CX)  && \
	!defined(PX)  && \
	!defined(OX1) && \
	!defined(OX2)
#define ERX
#endif


/*
 * geqo--
 *	  solution of the query optimization problem
 *	  similar to a constrained Traveling Salesman Problem (TSP)
 */

RelOptInfo *
geqo(Query *root)
{
	int			generation;
	Chromosome *momma;
	Chromosome *daddy;
	Chromosome *kid;

#if defined(ERX)
	Edge	   *edge_table;		/* list of edges */
	int			edge_failures = 0;
	float		difference;

#endif

#if defined(CX) || defined(PX) || defined(OX1) || defined(OX2)
	City	   *city_table;		/* list of cities */

#endif

#if defined(CX)
	int			cycle_diffs = 0;
	int			mutations = 0;

#endif


	int			number_of_rels;

	Pool	   *pool;
	int			pool_size,
				number_generations,
				status_interval;

	Gene	   *best_tour;
	RelOptInfo		   *best_rel;

/*	Plan *best_plan; */


/* set tour size */
	number_of_rels = length(root->base_relation_list_);

/* set GA parameters */
	geqo_params(number_of_rels);/* out of "$PGDATA/pg_geqo" file */
	pool_size = PoolSize;
	number_generations = Generations;
	status_interval = 10;

/* seed random number generator */
	srandom(RandomSeed);

/* allocate genetic pool memory */
	pool = alloc_pool(pool_size, number_of_rels);

/* random initialization of the pool */
	random_init_pool(root, pool, 0, pool->size);

/* sort the pool according to cheapest path as fitness */
	sort_pool(pool);			/* we have to do it only one time, since
								 * all kids replace the worst individuals
								 * in future (-> geqo_pool.c:spread_chromo
								 * ) */

/* allocate chromosome momma and daddy memory */
	momma = alloc_chromo(pool->string_length);
	daddy = alloc_chromo(pool->string_length);

#if defined (ERX)
	elog(DEBUG, "geqo_main: using edge recombination crossover [ERX]");
/* allocate edge table memory */
	edge_table = alloc_edge_table(pool->string_length);
#elif defined(PMX)
	elog(DEBUG, "geqo_main: using partially matched crossover [PMX]");
/* allocate chromosome kid memory */
	kid = alloc_chromo(pool->string_length);
#elif defined(CX)
	elog(DEBUG, "geqo_main: using cycle crossover [CX]");
/* allocate city table memory */
	kid = alloc_chromo(pool->string_length);
	city_table = alloc_city_table(pool->string_length);
#elif defined(PX)
	elog(DEBUG, "geqo_main: using position crossover [PX]");
/* allocate city table memory */
	kid = alloc_chromo(pool->string_length);
	city_table = alloc_city_table(pool->string_length);
#elif defined(OX1)
	elog(DEBUG, "geqo_main: using order crossover [OX1]");
/* allocate city table memory */
	kid = alloc_chromo(pool->string_length);
	city_table = alloc_city_table(pool->string_length);
#elif defined(OX2)
	elog(DEBUG, "geqo_main: using order crossover [OX2]");
/* allocate city table memory */
	kid = alloc_chromo(pool->string_length);
	city_table = alloc_city_table(pool->string_length);
#endif


/* my pain main part: */
/* iterative optimization */

	for (generation = 0; generation < number_generations; generation++)
	{

		/* SELECTION */
		geqo_selection(momma, daddy, pool, SelectionBias);		/* using linear bias
																 * function */



#if defined (ERX)
		/* EDGE RECOMBINATION CROSSOVER */
		difference = gimme_edge_table(momma->string, daddy->string, pool->string_length, edge_table);

		/* let the kid grow in momma's womb (storage) for nine months ;-) */
		/* sleep(23328000) -- har har har */
		kid = momma;

		/* are there any edge failures ? */
		edge_failures += gimme_tour(edge_table, kid->string, pool->string_length);
#elif defined(PMX)
		/* PARTIALLY MATCHED CROSSOVER */
		pmx(momma->string, daddy->string, kid->string, pool->string_length);
#elif defined(CX)
		/* CYCLE CROSSOVER */
		cycle_diffs =
			cx(momma->string, daddy->string, kid->string, pool->string_length, city_table);
		/* mutate the child */
		if (cycle_diffs == 0)
		{
			mutations++;
			geqo_mutation(kid->string, pool->string_length);
		}
#elif defined(PX)
		/* POSITION CROSSOVER */
		px(momma->string, daddy->string, kid->string, pool->string_length, city_table);
#elif defined(OX1)
		/* ORDER CROSSOVER */
		ox1(momma->string, daddy->string, kid->string, pool->string_length, city_table);
#elif defined(OX2)
		/* ORDER CROSSOVER */
		ox2(momma->string, daddy->string, kid->string, pool->string_length, city_table);
#endif


		/* EVALUATE FITNESS */
		kid->worth = geqo_eval(root, kid->string, pool->string_length);

		/* push the kid into the wilderness of life according to its worth */
		spread_chromo(kid, pool);


#ifdef GEQO_DEBUG
		if (status_interval && !(generation % status_interval))
			print_gen(stdout, pool, generation);
#endif

	}							/* end of iterative optimization */


#if defined(ERX) && defined(GEQO_DEBUG)
	if (edge_failures != 0)
		fprintf(stdout, "\nFailures: %d  Avg: %d\n", edge_failures, (int) generation / edge_failures);

	else
		fprintf(stdout, "No edge failures detected.\n");
#endif


#if defined(CX) && defined(GEQO_DEBUG)
	if (mutations != 0)
		fprintf(stdout, "\nMutations: %d  Generations: %d\n", mutations, generation);

	else
		fprintf(stdout, "No mutations processed.\n");
#endif


#ifdef GEQO_DEBUG
	fprintf(stdout, "\n");
	print_pool(stdout, pool, 0, pool_size - 1);
#endif


/* got the cheapest query tree processed by geqo;
   first element of the population indicates the best query tree */

	best_tour = (Gene *) pool->data[0].string;

/* root->join_relation_list_ will be modified during this ! */
	best_rel = (RelOptInfo *) gimme_tree(root, best_tour, 0, pool->string_length, NULL);

/* DBG: show the query plan
print_plan(best_plan, root);
   DBG */

/* ... free memory stuff */
	free_chromo(momma);
	free_chromo(daddy);

#if defined (ERX)
	free_edge_table(edge_table);
#elif defined(PMX)
	free_chromo(kid);
#elif defined(CX)
	free_chromo(kid);
	free_city_table(city_table);
#elif defined(PX)
	free_chromo(kid);
	free_city_table(city_table);
#elif defined(OX1)
	free_chromo(kid);
	free_city_table(city_table);
#elif defined(OX2)
	free_chromo(kid);
	free_city_table(city_table);
#endif

	free_pool(pool);

	return (best_rel);
}
