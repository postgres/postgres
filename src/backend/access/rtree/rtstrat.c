/*-------------------------------------------------------------------------
 *
 * rtstrat.c
 *	  strategy map data for rtrees.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/rtree/rtstrat.c,v 1.25 2004/12/31 21:59:26 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/rtree.h"


/*
 *	Here's something peculiar to rtrees that doesn't apply to most other
 *	indexing structures:  When we're searching a tree for a given value, we
 *	can't do the same sorts of comparisons on internal node entries as we
 *	do at leaves.  The reason is that if we're looking for (say) all boxes
 *	that are the same as (0,0,10,10), then we need to find all leaf pages
 *	that overlap that region.  So internally we search for overlap, and at
 *	the leaf we search for equality.
 *
 *	This array maps leaf search operators to the internal search operators.
 *	We assume the normal ordering on operators:
 *
 *		left, left-or-overlap, overlap, right-or-overlap, right, same,
 *		contains, contained-by
 */
static const StrategyNumber RTOperMap[RTNStrategies] = {
	RTOverLeftStrategyNumber,
	RTOverLeftStrategyNumber,
	RTOverlapStrategyNumber,
	RTOverRightStrategyNumber,
	RTOverRightStrategyNumber,
	RTContainsStrategyNumber,
	RTContainsStrategyNumber,
	RTOverlapStrategyNumber
};


StrategyNumber
RTMapToInternalOperator(StrategyNumber strat)
{
	Assert(strat > 0 && strat <= RTNStrategies);
	return RTOperMap[strat - 1];
}
