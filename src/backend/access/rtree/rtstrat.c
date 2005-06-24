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
 *	  $PostgreSQL: pgsql/src/backend/access/rtree/rtstrat.c,v 1.27 2005/06/24 20:53:30 tgl Exp $
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
 */
static const StrategyNumber RTOperMap[RTNStrategies] = {
	RTOverRightStrategyNumber,	/* left */
	RTRightStrategyNumber,		/* overleft */
	RTOverlapStrategyNumber,	/* overlap */
	RTLeftStrategyNumber,		/* overright */
	RTOverLeftStrategyNumber,	/* right */
	RTContainsStrategyNumber,	/* same */
	RTContainsStrategyNumber,	/* contains */
	RTOverlapStrategyNumber,	/* contained-by */
	RTAboveStrategyNumber,		/* overbelow */
	RTOverAboveStrategyNumber,	/* below */
	RTOverBelowStrategyNumber,	/* above */
	RTBelowStrategyNumber		/* overabove */
};

/*
 * We may need to negate the result of the selected operator.  (This could
 * be avoided by expanding the set of operators required for an opclass.)
 */
static const bool RTNegateMap[RTNStrategies] = {
	true,						/* left */
	true,						/* overleft */
	false,						/* overlap */
	true,						/* overright */
	true,						/* right */
	false,						/* same */
	false,						/* contains */
	false,						/* contained-by */
	true,						/* overbelow */
	true,						/* below */
	true,						/* above */
	true						/* overabove */
};


StrategyNumber
RTMapToInternalOperator(StrategyNumber strat)
{
	Assert(strat > 0 && strat <= RTNStrategies);
	return RTOperMap[strat - 1];
}

bool
RTMapToInternalNegate(StrategyNumber strat)
{
	Assert(strat > 0 && strat <= RTNStrategies);
	return RTNegateMap[strat - 1];
}
