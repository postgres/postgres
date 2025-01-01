/*-------------------------------------------------------------------------
 *
 * blcost.c
 *		Cost estimate function for bloom indexes.
 *
 * Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/bloom/blcost.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "bloom.h"
#include "utils/selfuncs.h"

/*
 * Estimate cost of bloom index scan.
 */
void
blcostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
			   Cost *indexStartupCost, Cost *indexTotalCost,
			   Selectivity *indexSelectivity, double *indexCorrelation,
			   double *indexPages)
{
	IndexOptInfo *index = path->indexinfo;
	GenericCosts costs = {0};

	/* We have to visit all index tuples anyway */
	costs.numIndexTuples = index->tuples;

	/* Use generic estimate */
	genericcostestimate(root, path, loop_count, &costs);

	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = costs.indexCorrelation;
	*indexPages = costs.numIndexPages;
}
