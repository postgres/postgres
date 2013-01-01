/*------------------------------------------------------------------------
 *
 * geqo_main.c
 *	  solution to the query optimization problem
 *	  by means of a Genetic Algorithm (GA)
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/optimizer/geqo/geqo_main.c
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

#include <math.h>

#include "optimizer/geqo_misc.h"
#include "optimizer/geqo_mutation.h"
#include "optimizer/geqo_pool.h"
#include "optimizer/geqo_random.h"
#include "optimizer/geqo_selection.h"


/*
 * Configuration options
 */
int			Geqo_effort;
int			Geqo_pool_size;
int			Geqo_generations;
double		Geqo_selection_bias;
double		Geqo_seed;


static int	gimme_pool_size(int nr_rel);
static int	gimme_number_generations(int pool_size);

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
 * geqo
 *	  solution of the query optimization problem
 *	  similar to a constrained Traveling Salesman Problem (TSP)
 */

RelOptInfo *
geqo(PlannerInfo *root, int number_of_rels, List *initial_rels)
{
	GeqoPrivateData private;
	int			generation;
	Chromosome *momma;
	Chromosome *daddy;
	Chromosome *kid;
	Pool	   *pool;
	int			pool_size,
				number_generations;

#ifdef GEQO_DEBUG
	int			status_interval;
#endif
	Gene	   *best_tour;
	RelOptInfo *best_rel;

#if defined(ERX)
	Edge	   *edge_table;		/* list of edges */
	int			edge_failures = 0;
#endif
#if defined(CX) || defined(PX) || defined(OX1) || defined(OX2)
	City	   *city_table;		/* list of cities */
#endif
#if defined(CX)
	int			cycle_diffs = 0;
	int			mutations = 0;
#endif

/* set up private information */
	root->join_search_private = (void *) &private;
	private.initial_rels = initial_rels;

/* initialize private number generator */
	geqo_set_seed(root, Geqo_seed);

/* set GA parameters */
	pool_size = gimme_pool_size(number_of_rels);
	number_generations = gimme_number_generations(pool_size);
#ifdef GEQO_DEBUG
	status_interval = 10;
#endif

/* allocate genetic pool memory */
	pool = alloc_pool(root, pool_size, number_of_rels);

/* random initialization of the pool */
	random_init_pool(root, pool);

/* sort the pool according to cheapest path as fitness */
	sort_pool(root, pool);		/* we have to do it only one time, since all
								 * kids replace the worst individuals in
								 * future (-> geqo_pool.c:spread_chromo ) */

#ifdef GEQO_DEBUG
	elog(DEBUG1, "GEQO selected %d pool entries, best %.2f, worst %.2f",
		 pool_size,
		 pool->data[0].worth,
		 pool->data[pool_size - 1].worth);
#endif

/* allocate chromosome momma and daddy memory */
	momma = alloc_chromo(root, pool->string_length);
	daddy = alloc_chromo(root, pool->string_length);

#if defined (ERX)
#ifdef GEQO_DEBUG
	elog(DEBUG2, "using edge recombination crossover [ERX]");
#endif
/* allocate edge table memory */
	edge_table = alloc_edge_table(root, pool->string_length);
#elif defined(PMX)
#ifdef GEQO_DEBUG
	elog(DEBUG2, "using partially matched crossover [PMX]");
#endif
/* allocate chromosome kid memory */
	kid = alloc_chromo(root, pool->string_length);
#elif defined(CX)
#ifdef GEQO_DEBUG
	elog(DEBUG2, "using cycle crossover [CX]");
#endif
/* allocate city table memory */
	kid = alloc_chromo(root, pool->string_length);
	city_table = alloc_city_table(root, pool->string_length);
#elif defined(PX)
#ifdef GEQO_DEBUG
	elog(DEBUG2, "using position crossover [PX]");
#endif
/* allocate city table memory */
	kid = alloc_chromo(root, pool->string_length);
	city_table = alloc_city_table(root, pool->string_length);
#elif defined(OX1)
#ifdef GEQO_DEBUG
	elog(DEBUG2, "using order crossover [OX1]");
#endif
/* allocate city table memory */
	kid = alloc_chromo(root, pool->string_length);
	city_table = alloc_city_table(root, pool->string_length);
#elif defined(OX2)
#ifdef GEQO_DEBUG
	elog(DEBUG2, "using order crossover [OX2]");
#endif
/* allocate city table memory */
	kid = alloc_chromo(root, pool->string_length);
	city_table = alloc_city_table(root, pool->string_length);
#endif


/* my pain main part: */
/* iterative optimization */

	for (generation = 0; generation < number_generations; generation++)
	{
		/* SELECTION: using linear bias function */
		geqo_selection(root, momma, daddy, pool, Geqo_selection_bias);

#if defined (ERX)
		/* EDGE RECOMBINATION CROSSOVER */
		gimme_edge_table(root, momma->string, daddy->string, pool->string_length, edge_table);

		kid = momma;

		/* are there any edge failures ? */
		edge_failures += gimme_tour(root, edge_table, kid->string, pool->string_length);
#elif defined(PMX)
		/* PARTIALLY MATCHED CROSSOVER */
		pmx(root, momma->string, daddy->string, kid->string, pool->string_length);
#elif defined(CX)
		/* CYCLE CROSSOVER */
		cycle_diffs = cx(root, momma->string, daddy->string, kid->string, pool->string_length, city_table);
		/* mutate the child */
		if (cycle_diffs == 0)
		{
			mutations++;
			geqo_mutation(root, kid->string, pool->string_length);
		}
#elif defined(PX)
		/* POSITION CROSSOVER */
		px(root, momma->string, daddy->string, kid->string, pool->string_length, city_table);
#elif defined(OX1)
		/* ORDER CROSSOVER */
		ox1(root, momma->string, daddy->string, kid->string, pool->string_length, city_table);
#elif defined(OX2)
		/* ORDER CROSSOVER */
		ox2(root, momma->string, daddy->string, kid->string, pool->string_length, city_table);
#endif


		/* EVALUATE FITNESS */
		kid->worth = geqo_eval(root, kid->string, pool->string_length);

		/* push the kid into the wilderness of life according to its worth */
		spread_chromo(root, kid, pool);


#ifdef GEQO_DEBUG
		if (status_interval && !(generation % status_interval))
			print_gen(stdout, pool, generation);
#endif

	}


#if defined(ERX) && defined(GEQO_DEBUG)
	if (edge_failures != 0)
		elog(LOG, "[GEQO] failures: %d, average: %d",
			 edge_failures, (int) number_generations / edge_failures);
	else
		elog(LOG, "[GEQO] no edge failures detected");
#endif

#if defined(CX) && defined(GEQO_DEBUG)
	if (mutations != 0)
		elog(LOG, "[GEQO] mutations: %d, generations: %d",
			 mutations, number_generations);
	else
		elog(LOG, "[GEQO] no mutations processed");
#endif

#ifdef GEQO_DEBUG
	print_pool(stdout, pool, 0, pool_size - 1);
#endif

#ifdef GEQO_DEBUG
	elog(DEBUG1, "GEQO best is %.2f after %d generations",
		 pool->data[0].worth, number_generations);
#endif


	/*
	 * got the cheapest query tree processed by geqo; first element of the
	 * population indicates the best query tree
	 */
	best_tour = (Gene *) pool->data[0].string;

	best_rel = gimme_tree(root, best_tour, pool->string_length);

	/* DBG: show the query plan */
#ifdef NOT_USED
	print_plan(best_plan, root);
#endif

	/* ... free memory stuff */
	free_chromo(root, momma);
	free_chromo(root, daddy);

#if defined (ERX)
	free_edge_table(root, edge_table);
#elif defined(PMX)
	free_chromo(root, kid);
#elif defined(CX)
	free_chromo(root, kid);
	free_city_table(root, city_table);
#elif defined(PX)
	free_chromo(root, kid);
	free_city_table(root, city_table);
#elif defined(OX1)
	free_chromo(root, kid);
	free_city_table(root, city_table);
#elif defined(OX2)
	free_chromo(root, kid);
	free_city_table(root, city_table);
#endif

	free_pool(root, pool);

	/* ... clear root pointer to our private storage */
	root->join_search_private = NULL;

	return best_rel;
}


/*
 * Return either configured pool size or a good default
 *
 * The default is based on query size (no. of relations) = 2^(QS+1),
 * but constrained to a range based on the effort value.
 */
static int
gimme_pool_size(int nr_rel)
{
	double		size;
	int			minsize;
	int			maxsize;

	/* Legal pool size *must* be at least 2, so ignore attempt to select 1 */
	if (Geqo_pool_size >= 2)
		return Geqo_pool_size;

	size = pow(2.0, nr_rel + 1.0);

	maxsize = 50 * Geqo_effort; /* 50 to 500 individuals */
	if (size > maxsize)
		return maxsize;

	minsize = 10 * Geqo_effort; /* 10 to 100 individuals */
	if (size < minsize)
		return minsize;

	return (int) ceil(size);
}


/*
 * Return either configured number of generations or a good default
 *
 * The default is the same as the pool size, which allows us to be
 * sure that less-fit individuals get pushed out of the breeding
 * population before the run finishes.
 */
static int
gimme_number_generations(int pool_size)
{
	if (Geqo_generations > 0)
		return Geqo_generations;

	return pool_size;
}
