/*------------------------------------------------------------------------
 *
 * geqo_misc.c
 *	   misc. printout and debug stuff
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/optimizer/geqo/geqo_misc.c
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

#include "postgres.h"

#include "optimizer/geqo_misc.h"


#ifdef GEQO_DEBUG


/*
 * avg_pool
 */
static double
avg_pool(Pool *pool)
{
	int			i;
	double		cumulative = 0.0;

	if (pool->size <= 0)
		elog(ERROR, "pool_size is zero");

	/*
	 * Since the pool may contain multiple occurrences of DBL_MAX, divide by
	 * pool->size before summing, not after, to avoid overflow.  This loses a
	 * little in speed and accuracy, but this routine is only used for debug
	 * printouts, so we don't care that much.
	 */
	for (i = 0; i < pool->size; i++)
		cumulative += pool->data[i].worth / pool->size;

	return cumulative;
}

/* print_pool
 */
void
print_pool(FILE *fp, Pool *pool, int start, int stop)
{
	int			i,
				j;

	/* be extra careful that start and stop are valid inputs */

	if (start < 0)
		start = 0;
	if (stop > pool->size)
		stop = pool->size;

	if (start + stop > pool->size)
	{
		start = 0;
		stop = pool->size;
	}

	for (i = start; i < stop; i++)
	{
		fprintf(fp, "%d)\t", i);
		for (j = 0; j < pool->string_length; j++)
			fprintf(fp, "%d ", pool->data[i].string[j]);
		fprintf(fp, "%g\n", pool->data[i].worth);
	}

	fflush(fp);
}

/* print_gen
 *
 *	 printout for chromosome: best, worst, mean, average
 */
void
print_gen(FILE *fp, Pool *pool, int generation)
{
	int			lowest;

	/* Get index to lowest ranking gene in poplulation. */
	/* Use 2nd to last since last is buffer. */
	lowest = pool->size > 1 ? pool->size - 2 : 0;

	fprintf(fp,
			"%5d | Best: %g  Worst: %g  Mean: %g  Avg: %g\n",
			generation,
			pool->data[0].worth,
			pool->data[lowest].worth,
			pool->data[pool->size / 2].worth,
			avg_pool(pool));

	fflush(fp);
}


void
print_edge_table(FILE *fp, Edge *edge_table, int num_gene)
{
	int			i,
				j;

	fprintf(fp, "\nEDGE TABLE\n");

	for (i = 1; i <= num_gene; i++)
	{
		fprintf(fp, "%d :", i);
		for (j = 0; j < edge_table[i].unused_edges; j++)
			fprintf(fp, " %d", edge_table[i].edge_list[j]);
		fprintf(fp, "\n");
	}

	fprintf(fp, "\n");

	fflush(fp);
}

#endif   /* GEQO_DEBUG */
