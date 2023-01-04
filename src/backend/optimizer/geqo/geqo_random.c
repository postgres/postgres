/*------------------------------------------------------------------------
 *
 * geqo_random.c
 *	   random number generator
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/optimizer/geqo/geqo_random.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "optimizer/geqo_random.h"


void
geqo_set_seed(PlannerInfo *root, double seed)
{
	GeqoPrivateData *private = (GeqoPrivateData *) root->join_search_private;

	pg_prng_fseed(&private->random_state, seed);
}

double
geqo_rand(PlannerInfo *root)
{
	GeqoPrivateData *private = (GeqoPrivateData *) root->join_search_private;

	return pg_prng_double(&private->random_state);
}

int
geqo_randint(PlannerInfo *root, int upper, int lower)
{
	GeqoPrivateData *private = (GeqoPrivateData *) root->join_search_private;

	/*
	 * In current usage, "lower" is never negative so we can just use
	 * pg_prng_uint64_range directly.
	 */
	return (int) pg_prng_uint64_range(&private->random_state, lower, upper);
}
