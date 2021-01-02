/*------------------------------------------------------------------------
 *
 * geqo_pool.c
 *	  Genetic Algorithm (GA) pool stuff
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/optimizer/geqo/geqo_pool.c
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

#include <float.h>
#include <limits.h>
#include <math.h>

#include "optimizer/geqo_copy.h"
#include "optimizer/geqo_pool.h"
#include "optimizer/geqo_recombination.h"


static int	compare(const void *arg1, const void *arg2);

/*
 * alloc_pool
 *		allocates memory for GA pool
 */
Pool *
alloc_pool(PlannerInfo *root, int pool_size, int string_length)
{
	Pool	   *new_pool;
	Chromosome *chromo;
	int			i;

	/* pool */
	new_pool = (Pool *) palloc(sizeof(Pool));
	new_pool->size = (int) pool_size;
	new_pool->string_length = (int) string_length;

	/* all chromosome */
	new_pool->data = (Chromosome *) palloc(pool_size * sizeof(Chromosome));

	/* all gene */
	chromo = (Chromosome *) new_pool->data; /* vector of all chromos */
	for (i = 0; i < pool_size; i++)
		chromo[i].string = palloc((string_length + 1) * sizeof(Gene));

	return new_pool;
}

/*
 * free_pool
 *		deallocates memory for GA pool
 */
void
free_pool(PlannerInfo *root, Pool *pool)
{
	Chromosome *chromo;
	int			i;

	/* all gene */
	chromo = (Chromosome *) pool->data; /* vector of all chromos */
	for (i = 0; i < pool->size; i++)
		pfree(chromo[i].string);

	/* all chromosome */
	pfree(pool->data);

	/* pool */
	pfree(pool);
}

/*
 * random_init_pool
 *		initialize genetic pool
 */
void
random_init_pool(PlannerInfo *root, Pool *pool)
{
	Chromosome *chromo = (Chromosome *) pool->data;
	int			i;
	int			bad = 0;

	/*
	 * We immediately discard any invalid individuals (those that geqo_eval
	 * returns DBL_MAX for), thereby not wasting pool space on them.
	 *
	 * If we fail to make any valid individuals after 10000 tries, give up;
	 * this probably means something is broken, and we shouldn't just let
	 * ourselves get stuck in an infinite loop.
	 */
	i = 0;
	while (i < pool->size)
	{
		init_tour(root, chromo[i].string, pool->string_length);
		pool->data[i].worth = geqo_eval(root, chromo[i].string,
										pool->string_length);
		if (pool->data[i].worth < DBL_MAX)
			i++;
		else
		{
			bad++;
			if (i == 0 && bad >= 10000)
				elog(ERROR, "geqo failed to make a valid plan");
		}
	}

#ifdef GEQO_DEBUG
	if (bad > 0)
		elog(DEBUG1, "%d invalid tours found while selecting %d pool entries",
			 bad, pool->size);
#endif
}

/*
 * sort_pool
 *	 sorts input pool according to worth, from smallest to largest
 *
 *	 maybe you have to change compare() for different ordering ...
 */
void
sort_pool(PlannerInfo *root, Pool *pool)
{
	qsort(pool->data, pool->size, sizeof(Chromosome), compare);
}

/*
 * compare
 *	 qsort comparison function for sort_pool
 */
static int
compare(const void *arg1, const void *arg2)
{
	const Chromosome *chromo1 = (const Chromosome *) arg1;
	const Chromosome *chromo2 = (const Chromosome *) arg2;

	if (chromo1->worth == chromo2->worth)
		return 0;
	else if (chromo1->worth > chromo2->worth)
		return 1;
	else
		return -1;
}

/* alloc_chromo
 *	  allocates a chromosome and string space
 */
Chromosome *
alloc_chromo(PlannerInfo *root, int string_length)
{
	Chromosome *chromo;

	chromo = (Chromosome *) palloc(sizeof(Chromosome));
	chromo->string = (Gene *) palloc((string_length + 1) * sizeof(Gene));

	return chromo;
}

/* free_chromo
 *	  deallocates a chromosome and string space
 */
void
free_chromo(PlannerInfo *root, Chromosome *chromo)
{
	pfree(chromo->string);
	pfree(chromo);
}

/* spread_chromo
 *	 inserts a new chromosome into the pool, displacing worst gene in pool
 *	 assumes best->worst = smallest->largest
 */
void
spread_chromo(PlannerInfo *root, Chromosome *chromo, Pool *pool)
{
	int			top,
				mid,
				bot;
	int			i,
				index;
	Chromosome	swap_chromo,
				tmp_chromo;

	/* new chromo is so bad we can't use it */
	if (chromo->worth > pool->data[pool->size - 1].worth)
		return;

	/* do a binary search to find the index of the new chromo */

	top = 0;
	mid = pool->size / 2;
	bot = pool->size - 1;
	index = -1;

	while (index == -1)
	{
		/* these 4 cases find a new location */

		if (chromo->worth <= pool->data[top].worth)
			index = top;
		else if (chromo->worth == pool->data[mid].worth)
			index = mid;
		else if (chromo->worth == pool->data[bot].worth)
			index = bot;
		else if (bot - top <= 1)
			index = bot;


		/*
		 * these 2 cases move the search indices since a new location has not
		 * yet been found.
		 */

		else if (chromo->worth < pool->data[mid].worth)
		{
			bot = mid;
			mid = top + ((bot - top) / 2);
		}
		else
		{						/* (chromo->worth > pool->data[mid].worth) */
			top = mid;
			mid = top + ((bot - top) / 2);
		}
	}							/* ... while */

	/* now we have index for chromo */

	/*
	 * move every gene from index on down one position to make room for chromo
	 */

	/*
	 * copy new gene into pool storage; always replace worst gene in pool
	 */

	geqo_copy(root, &pool->data[pool->size - 1], chromo, pool->string_length);

	swap_chromo.string = pool->data[pool->size - 1].string;
	swap_chromo.worth = pool->data[pool->size - 1].worth;

	for (i = index; i < pool->size; i++)
	{
		tmp_chromo.string = pool->data[i].string;
		tmp_chromo.worth = pool->data[i].worth;

		pool->data[i].string = swap_chromo.string;
		pool->data[i].worth = swap_chromo.worth;

		swap_chromo.string = tmp_chromo.string;
		swap_chromo.worth = tmp_chromo.worth;
	}
}
