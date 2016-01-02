/*-------------------------------------------------------------------------
 *
 * rangetypes_spgist.c
 *	  implementation of quad tree over ranges mapped to 2d-points for SP-GiST.
 *
 * Quad tree is a data structure similar to a binary tree, but is adapted to
 * 2d data. Each inner node of a quad tree contains a point (centroid) which
 * divides the 2d-space into 4 quadrants. Each quadrant is associated with a
 * child node.
 *
 * Ranges are mapped to 2d-points so that the lower bound is one dimension,
 * and the upper bound is another. By convention, we visualize the lower bound
 * to be the horizontal axis, and upper bound the vertical axis.
 *
 * One quirk with this mapping is the handling of empty ranges. An empty range
 * doesn't have lower and upper bounds, so it cannot be mapped to 2d space in
 * a straightforward way. To cope with that, the root node can have a 5th
 * quadrant, which is reserved for empty ranges. Furthermore, there can be
 * inner nodes in the tree with no centroid. They contain only two child nodes,
 * one for empty ranges and another for non-empty ones. Such a node can appear
 * as the root node, or in the tree under the 5th child of the root node (in
 * which case it will only contain empty nodes).
 *
 * The SP-GiST picksplit function uses medians along both axes as the centroid.
 * This implementation only uses the comparison function of the range element
 * datatype, therefore it works for any range type.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/utils/adt/rangetypes_spgist.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/spgist.h"
#include "access/stratnum.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/rangetypes.h"

/* SP-GiST API functions */
Datum		spg_range_quad_config(PG_FUNCTION_ARGS);
Datum		spg_range_quad_choose(PG_FUNCTION_ARGS);
Datum		spg_range_quad_picksplit(PG_FUNCTION_ARGS);
Datum		spg_range_quad_inner_consistent(PG_FUNCTION_ARGS);
Datum		spg_range_quad_leaf_consistent(PG_FUNCTION_ARGS);

static int16 getQuadrant(TypeCacheEntry *typcache, RangeType *centroid,
			RangeType *tst);
static int	bound_cmp(const void *a, const void *b, void *arg);

static int adjacent_inner_consistent(TypeCacheEntry *typcache,
						  RangeBound *arg, RangeBound *centroid,
						  RangeBound *prev);
static int adjacent_cmp_bounds(TypeCacheEntry *typcache, RangeBound *arg,
					RangeBound *centroid);

/*
 * SP-GiST 'config' interface function.
 */
Datum
spg_range_quad_config(PG_FUNCTION_ARGS)
{
	/* spgConfigIn *cfgin = (spgConfigIn *) PG_GETARG_POINTER(0); */
	spgConfigOut *cfg = (spgConfigOut *) PG_GETARG_POINTER(1);

	cfg->prefixType = ANYRANGEOID;
	cfg->labelType = VOIDOID;	/* we don't need node labels */
	cfg->canReturnData = true;
	cfg->longValuesOK = false;
	PG_RETURN_VOID();
}

/*----------
 * Determine which quadrant a 2d-mapped range falls into, relative to the
 * centroid.
 *
 * Quadrants are numbered like this:
 *
 *	 4	|  1
 *	----+----
 *	 3	|  2
 *
 * Where the lower bound of range is the horizontal axis and upper bound the
 * vertical axis.
 *
 * Ranges on one of the axes are taken to lie in the quadrant with higher value
 * along perpendicular axis. That is, a value on the horizontal axis is taken
 * to belong to quadrant 1 or 4, and a value on the vertical axis is taken to
 * belong to quadrant 1 or 2. A range equal to centroid is taken to lie in
 * quadrant 1.
 *
 * Empty ranges are taken to lie in the special quadrant 5.
 *----------
 */
static int16
getQuadrant(TypeCacheEntry *typcache, RangeType *centroid, RangeType *tst)
{
	RangeBound	centroidLower,
				centroidUpper;
	bool		centroidEmpty;
	RangeBound	lower,
				upper;
	bool		empty;

	range_deserialize(typcache, centroid, &centroidLower, &centroidUpper,
					  &centroidEmpty);
	range_deserialize(typcache, tst, &lower, &upper, &empty);

	if (empty)
		return 5;

	if (range_cmp_bounds(typcache, &lower, &centroidLower) >= 0)
	{
		if (range_cmp_bounds(typcache, &upper, &centroidUpper) >= 0)
			return 1;
		else
			return 2;
	}
	else
	{
		if (range_cmp_bounds(typcache, &upper, &centroidUpper) >= 0)
			return 4;
		else
			return 3;
	}
}

/*
 * Choose SP-GiST function: choose path for addition of new range.
 */
Datum
spg_range_quad_choose(PG_FUNCTION_ARGS)
{
	spgChooseIn *in = (spgChooseIn *) PG_GETARG_POINTER(0);
	spgChooseOut *out = (spgChooseOut *) PG_GETARG_POINTER(1);
	RangeType  *inRange = DatumGetRangeType(in->datum),
			   *centroid;
	int16		quadrant;
	TypeCacheEntry *typcache;

	if (in->allTheSame)
	{
		out->resultType = spgMatchNode;
		/* nodeN will be set by core */
		out->result.matchNode.levelAdd = 0;
		out->result.matchNode.restDatum = RangeTypeGetDatum(inRange);
		PG_RETURN_VOID();
	}

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(inRange));

	/*
	 * A node with no centroid divides ranges purely on whether they're empty
	 * or not. All empty ranges go to child node 0, all non-empty ranges go to
	 * node 1.
	 */
	if (!in->hasPrefix)
	{
		out->resultType = spgMatchNode;
		if (RangeIsEmpty(inRange))
			out->result.matchNode.nodeN = 0;
		else
			out->result.matchNode.nodeN = 1;
		out->result.matchNode.levelAdd = 1;
		out->result.matchNode.restDatum = RangeTypeGetDatum(inRange);
		PG_RETURN_VOID();
	}

	centroid = DatumGetRangeType(in->prefixDatum);
	quadrant = getQuadrant(typcache, centroid, inRange);

	Assert(quadrant <= in->nNodes);

	/* Select node matching to quadrant number */
	out->resultType = spgMatchNode;
	out->result.matchNode.nodeN = quadrant - 1;
	out->result.matchNode.levelAdd = 1;
	out->result.matchNode.restDatum = RangeTypeGetDatum(inRange);

	PG_RETURN_VOID();
}

/*
 * Bound comparison for sorting.
 */
static int
bound_cmp(const void *a, const void *b, void *arg)
{
	RangeBound *ba = (RangeBound *) a;
	RangeBound *bb = (RangeBound *) b;
	TypeCacheEntry *typcache = (TypeCacheEntry *) arg;

	return range_cmp_bounds(typcache, ba, bb);
}

/*
 * Picksplit SP-GiST function: split ranges into nodes. Select "centroid"
 * range and distribute ranges according to quadrants.
 */
Datum
spg_range_quad_picksplit(PG_FUNCTION_ARGS)
{
	spgPickSplitIn *in = (spgPickSplitIn *) PG_GETARG_POINTER(0);
	spgPickSplitOut *out = (spgPickSplitOut *) PG_GETARG_POINTER(1);
	int			i;
	int			j;
	int			nonEmptyCount;
	RangeType  *centroid;
	bool		empty;
	TypeCacheEntry *typcache;

	/* Use the median values of lower and upper bounds as the centroid range */
	RangeBound *lowerBounds,
			   *upperBounds;

	typcache = range_get_typcache(fcinfo,
						  RangeTypeGetOid(DatumGetRangeType(in->datums[0])));

	/* Allocate memory for bounds */
	lowerBounds = palloc(sizeof(RangeBound) * in->nTuples);
	upperBounds = palloc(sizeof(RangeBound) * in->nTuples);
	j = 0;

	/* Deserialize bounds of ranges, count non-empty ranges */
	for (i = 0; i < in->nTuples; i++)
	{
		range_deserialize(typcache, DatumGetRangeType(in->datums[i]),
						  &lowerBounds[j], &upperBounds[j], &empty);
		if (!empty)
			j++;
	}
	nonEmptyCount = j;

	/*
	 * All the ranges are empty. The best we can do is to construct an inner
	 * node with no centroid, and put all ranges into node 0. If non-empty
	 * ranges are added later, they will be routed to node 1.
	 */
	if (nonEmptyCount == 0)
	{
		out->nNodes = 2;
		out->hasPrefix = false;
		/* Prefix is empty */
		out->prefixDatum = PointerGetDatum(NULL);
		out->nodeLabels = NULL;

		out->mapTuplesToNodes = palloc(sizeof(int) * in->nTuples);
		out->leafTupleDatums = palloc(sizeof(Datum) * in->nTuples);

		/* Place all ranges into node 0 */
		for (i = 0; i < in->nTuples; i++)
		{
			RangeType  *range = DatumGetRangeType(in->datums[i]);

			out->leafTupleDatums[i] = RangeTypeGetDatum(range);
			out->mapTuplesToNodes[i] = 0;
		}
		PG_RETURN_VOID();
	}

	/* Sort range bounds in order to find medians */
	qsort_arg(lowerBounds, nonEmptyCount, sizeof(RangeBound),
			  bound_cmp, typcache);
	qsort_arg(upperBounds, nonEmptyCount, sizeof(RangeBound),
			  bound_cmp, typcache);

	/* Construct "centroid" range from medians of lower and upper bounds */
	centroid = range_serialize(typcache, &lowerBounds[nonEmptyCount / 2],
							   &upperBounds[nonEmptyCount / 2], false);
	out->hasPrefix = true;
	out->prefixDatum = RangeTypeGetDatum(centroid);

	/* Create node for empty ranges only if it is a root node */
	out->nNodes = (in->level == 0) ? 5 : 4;
	out->nodeLabels = NULL;		/* we don't need node labels */

	out->mapTuplesToNodes = palloc(sizeof(int) * in->nTuples);
	out->leafTupleDatums = palloc(sizeof(Datum) * in->nTuples);

	/*
	 * Assign ranges to corresponding nodes according to quadrants relative to
	 * "centroid" range.
	 */
	for (i = 0; i < in->nTuples; i++)
	{
		RangeType  *range = DatumGetRangeType(in->datums[i]);
		int16		quadrant = getQuadrant(typcache, centroid, range);

		out->leafTupleDatums[i] = RangeTypeGetDatum(range);
		out->mapTuplesToNodes[i] = quadrant - 1;
	}

	PG_RETURN_VOID();
}

/*
 * SP-GiST consistent function for inner nodes: check which nodes are
 * consistent with given set of queries.
 */
Datum
spg_range_quad_inner_consistent(PG_FUNCTION_ARGS)
{
	spgInnerConsistentIn *in = (spgInnerConsistentIn *) PG_GETARG_POINTER(0);
	spgInnerConsistentOut *out = (spgInnerConsistentOut *) PG_GETARG_POINTER(1);
	int			which;
	int			i;

	/*
	 * For adjacent search we need also previous centroid (if any) to improve
	 * the precision of the consistent check. In this case needPrevious flag
	 * is set and centroid is passed into reconstructedValues. This is not the
	 * intended purpose of reconstructedValues (because we already have the
	 * full value available at the leaf), but it's a convenient place to store
	 * state while traversing the tree.
	 */
	bool		needPrevious = false;

	if (in->allTheSame)
	{
		/* Report that all nodes should be visited */
		out->nNodes = in->nNodes;
		out->nodeNumbers = (int *) palloc(sizeof(int) * in->nNodes);
		for (i = 0; i < in->nNodes; i++)
			out->nodeNumbers[i] = i;
		PG_RETURN_VOID();
	}

	if (!in->hasPrefix)
	{
		/*
		 * No centroid on this inner node. Such a node has two child nodes,
		 * the first for empty ranges, and the second for non-empty ones.
		 */
		Assert(in->nNodes == 2);

		/*
		 * Nth bit of which variable means that (N - 1)th node should be
		 * visited. Initially all bits are set. Bits of nodes which should be
		 * skipped will be unset.
		 */
		which = (1 << 1) | (1 << 2);
		for (i = 0; i < in->nkeys; i++)
		{
			StrategyNumber strategy = in->scankeys[i].sk_strategy;
			bool		empty;

			/*
			 * The only strategy when second argument of operator is not range
			 * is RANGESTRAT_CONTAINS_ELEM.
			 */
			if (strategy != RANGESTRAT_CONTAINS_ELEM)
				empty = RangeIsEmpty(
							 DatumGetRangeType(in->scankeys[i].sk_argument));
			else
				empty = false;

			switch (strategy)
			{
				case RANGESTRAT_BEFORE:
				case RANGESTRAT_OVERLEFT:
				case RANGESTRAT_OVERLAPS:
				case RANGESTRAT_OVERRIGHT:
				case RANGESTRAT_AFTER:
				case RANGESTRAT_ADJACENT:
					/* These strategies return false if any argument is empty */
					if (empty)
						which = 0;
					else
						which &= (1 << 2);
					break;

				case RANGESTRAT_CONTAINS:

					/*
					 * All ranges contain an empty range. Only non-empty
					 * ranges can contain a non-empty range.
					 */
					if (!empty)
						which &= (1 << 2);
					break;

				case RANGESTRAT_CONTAINED_BY:

					/*
					 * Only an empty range is contained by an empty range.
					 * Both empty and non-empty ranges can be contained by a
					 * non-empty range.
					 */
					if (empty)
						which &= (1 << 1);
					break;

				case RANGESTRAT_CONTAINS_ELEM:
					which &= (1 << 2);
					break;

				case RANGESTRAT_EQ:
					if (empty)
						which &= (1 << 1);
					else
						which &= (1 << 2);
					break;

				default:
					elog(ERROR, "unrecognized range strategy: %d", strategy);
					break;
			}
			if (which == 0)
				break;			/* no need to consider remaining conditions */
		}
	}
	else
	{
		RangeBound	centroidLower,
					centroidUpper;
		bool		centroidEmpty;
		TypeCacheEntry *typcache;
		RangeType  *centroid;

		/* This node has a centroid. Fetch it. */
		centroid = DatumGetRangeType(in->prefixDatum);
		typcache = range_get_typcache(fcinfo,
							   RangeTypeGetOid(DatumGetRangeType(centroid)));
		range_deserialize(typcache, centroid, &centroidLower, &centroidUpper,
						  &centroidEmpty);

		Assert(in->nNodes == 4 || in->nNodes == 5);

		/*
		 * Nth bit of which variable means that (N - 1)th node (Nth quadrant)
		 * should be visited. Initially all bits are set. Bits of nodes which
		 * can be skipped will be unset.
		 */
		which = (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5);

		for (i = 0; i < in->nkeys; i++)
		{
			StrategyNumber strategy;
			RangeBound	lower,
						upper;
			bool		empty;
			RangeType  *range = NULL;

			RangeType  *prevCentroid = NULL;
			RangeBound	prevLower,
						prevUpper;
			bool		prevEmpty;

			/* Restrictions on range bounds according to scan strategy */
			RangeBound *minLower = NULL,
					   *maxLower = NULL,
					   *minUpper = NULL,
					   *maxUpper = NULL;

			/* Are the restrictions on range bounds inclusive? */
			bool		inclusive = true;
			bool		strictEmpty = true;
			int			cmp,
						which1,
						which2;

			strategy = in->scankeys[i].sk_strategy;

			/*
			 * RANGESTRAT_CONTAINS_ELEM is just like RANGESTRAT_CONTAINS, but
			 * the argument is a single element. Expand the single element to
			 * a range containing only the element, and treat it like
			 * RANGESTRAT_CONTAINS.
			 */
			if (strategy == RANGESTRAT_CONTAINS_ELEM)
			{
				lower.inclusive = true;
				lower.infinite = false;
				lower.lower = true;
				lower.val = in->scankeys[i].sk_argument;

				upper.inclusive = true;
				upper.infinite = false;
				upper.lower = false;
				upper.val = in->scankeys[i].sk_argument;

				empty = false;

				strategy = RANGESTRAT_CONTAINS;
			}
			else
			{
				range = DatumGetRangeType(in->scankeys[i].sk_argument);
				range_deserialize(typcache, range, &lower, &upper, &empty);
			}

			/*
			 * Most strategies are handled by forming a bounding box from the
			 * search key, defined by a minLower, maxLower, minUpper,
			 * maxUpper. Some modify 'which' directly, to specify exactly
			 * which quadrants need to be visited.
			 *
			 * For most strategies, nothing matches an empty search key, and
			 * an empty range never matches a non-empty key. If a strategy
			 * does not behave like that wrt. empty ranges, set strictEmpty to
			 * false.
			 */
			switch (strategy)
			{
				case RANGESTRAT_BEFORE:

					/*
					 * Range A is before range B if upper bound of A is lower
					 * than lower bound of B.
					 */
					maxUpper = &lower;
					inclusive = false;
					break;

				case RANGESTRAT_OVERLEFT:

					/*
					 * Range A is overleft to range B if upper bound of A is
					 * less or equal to upper bound of B.
					 */
					maxUpper = &upper;
					break;

				case RANGESTRAT_OVERLAPS:

					/*
					 * Non-empty ranges overlap, if lower bound of each range
					 * is lower or equal to upper bound of the other range.
					 */
					maxLower = &upper;
					minUpper = &lower;
					break;

				case RANGESTRAT_OVERRIGHT:

					/*
					 * Range A is overright to range B if lower bound of A is
					 * greater or equal to lower bound of B.
					 */
					minLower = &lower;
					break;

				case RANGESTRAT_AFTER:

					/*
					 * Range A is after range B if lower bound of A is greater
					 * than upper bound of B.
					 */
					minLower = &upper;
					inclusive = false;
					break;

				case RANGESTRAT_ADJACENT:
					if (empty)
						break;	/* Skip to strictEmpty check. */

					/*
					 * Previously selected quadrant could exclude possibility
					 * for lower or upper bounds to be adjacent. Deserialize
					 * previous centroid range if present for checking this.
					 */
					if (in->reconstructedValue != (Datum) 0)
					{
						prevCentroid = DatumGetRangeType(in->reconstructedValue);
						range_deserialize(typcache, prevCentroid,
										  &prevLower, &prevUpper, &prevEmpty);
					}

					/*
					 * For a range's upper bound to be adjacent to the
					 * argument's lower bound, it will be found along the line
					 * adjacent to (and just below) Y=lower. Therefore, if the
					 * argument's lower bound is less than the centroid's
					 * upper bound, the line falls in quadrants 2 and 3; if
					 * greater, the line falls in quadrants 1 and 4. (see
					 * adjacent_cmp_bounds for description of edge cases).
					 */
					cmp = adjacent_inner_consistent(typcache, &lower,
													&centroidUpper,
										   prevCentroid ? &prevUpper : NULL);
					if (cmp > 0)
						which1 = (1 << 1) | (1 << 4);
					else if (cmp < 0)
						which1 = (1 << 2) | (1 << 3);
					else
						which1 = 0;

					/*
					 * Also search for ranges's adjacent to argument's upper
					 * bound. They will be found along the line adjacent to
					 * (and just right of) X=upper, which falls in quadrants 3
					 * and 4, or 1 and 2.
					 */
					cmp = adjacent_inner_consistent(typcache, &upper,
													&centroidLower,
										   prevCentroid ? &prevLower : NULL);
					if (cmp > 0)
						which2 = (1 << 1) | (1 << 2);
					else if (cmp < 0)
						which2 = (1 << 3) | (1 << 4);
					else
						which2 = 0;

					/* We must chase down ranges adjacent to either bound. */
					which &= which1 | which2;

					needPrevious = true;
					break;

				case RANGESTRAT_CONTAINS:

					/*
					 * Non-empty range A contains non-empty range B if lower
					 * bound of A is lower or equal to lower bound of range B
					 * and upper bound of range A is greater or equal to upper
					 * bound of range A.
					 *
					 * All non-empty ranges contain an empty range.
					 */
					strictEmpty = false;
					if (!empty)
					{
						which &= (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4);
						maxLower = &lower;
						minUpper = &upper;
					}
					break;

				case RANGESTRAT_CONTAINED_BY:
					/* The opposite of contains. */
					strictEmpty = false;
					if (empty)
					{
						/* An empty range is only contained by an empty range */
						which &= (1 << 5);
					}
					else
					{
						minLower = &lower;
						maxUpper = &upper;
					}
					break;

				case RANGESTRAT_EQ:

					/*
					 * Equal range can be only in the same quadrant where
					 * argument would be placed to.
					 */
					strictEmpty = false;
					which &= (1 << getQuadrant(typcache, centroid, range));
					break;

				default:
					elog(ERROR, "unrecognized range strategy: %d", strategy);
					break;
			}

			if (strictEmpty)
			{
				if (empty)
				{
					/* Scan key is empty, no branches are satisfying */
					which = 0;
					break;
				}
				else
				{
					/* Shouldn't visit tree branch with empty ranges */
					which &= (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4);
				}
			}

			/*
			 * Using the bounding box, see which quadrants we have to descend
			 * into.
			 */
			if (minLower)
			{
				/*
				 * If the centroid's lower bound is less than or equal to the
				 * minimum lower bound, anything in the 3rd and 4th quadrants
				 * will have an even smaller lower bound, and thus can't
				 * match.
				 */
				if (range_cmp_bounds(typcache, &centroidLower, minLower) <= 0)
					which &= (1 << 1) | (1 << 2) | (1 << 5);
			}
			if (maxLower)
			{
				/*
				 * If the centroid's lower bound is greater than the maximum
				 * lower bound, anything in the 1st and 2nd quadrants will
				 * also have a greater than or equal lower bound, and thus
				 * can't match. If the centroid's lower bound is equal to the
				 * maximum lower bound, we can still exclude the 1st and 2nd
				 * quadrants if we're looking for a value strictly greater
				 * than the maximum.
				 */
				int			cmp;

				cmp = range_cmp_bounds(typcache, &centroidLower, maxLower);
				if (cmp > 0 || (!inclusive && cmp == 0))
					which &= (1 << 3) | (1 << 4) | (1 << 5);
			}
			if (minUpper)
			{
				/*
				 * If the centroid's upper bound is less than or equal to the
				 * minimum upper bound, anything in the 2nd and 3rd quadrants
				 * will have an even smaller upper bound, and thus can't
				 * match.
				 */
				if (range_cmp_bounds(typcache, &centroidUpper, minUpper) <= 0)
					which &= (1 << 1) | (1 << 4) | (1 << 5);
			}
			if (maxUpper)
			{
				/*
				 * If the centroid's upper bound is greater than the maximum
				 * upper bound, anything in the 1st and 4th quadrants will
				 * also have a greater than or equal upper bound, and thus
				 * can't match. If the centroid's upper bound is equal to the
				 * maximum upper bound, we can still exclude the 1st and 4th
				 * quadrants if we're looking for a value strictly greater
				 * than the maximum.
				 */
				int			cmp;

				cmp = range_cmp_bounds(typcache, &centroidUpper, maxUpper);
				if (cmp > 0 || (!inclusive && cmp == 0))
					which &= (1 << 2) | (1 << 3) | (1 << 5);
			}

			if (which == 0)
				break;			/* no need to consider remaining conditions */
		}
	}

	/* We must descend into the quadrant(s) identified by 'which' */
	out->nodeNumbers = (int *) palloc(sizeof(int) * in->nNodes);
	if (needPrevious)
		out->reconstructedValues = (Datum *) palloc(sizeof(Datum) * in->nNodes);
	out->nNodes = 0;
	for (i = 1; i <= in->nNodes; i++)
	{
		if (which & (1 << i))
		{
			/* Save previous prefix if needed */
			if (needPrevious)
				out->reconstructedValues[out->nNodes] = in->prefixDatum;
			out->nodeNumbers[out->nNodes++] = i - 1;
		}
	}

	PG_RETURN_VOID();
}

/*
 * adjacent_cmp_bounds
 *
 * Given an argument and centroid bound, this function determines if any
 * bounds that are adjacent to the argument are smaller than, or greater than
 * or equal to centroid. For brevity, we call the arg < centroid "left", and
 * arg >= centroid case "right". This corresponds to how the quadrants are
 * arranged, if you imagine that "left" is equivalent to "down" and "right"
 * is equivalent to "up".
 *
 * For the "left" case, returns -1, and for the "right" case, returns 1.
 */
static int
adjacent_cmp_bounds(TypeCacheEntry *typcache, RangeBound *arg,
					RangeBound *centroid)
{
	int			cmp;

	Assert(arg->lower != centroid->lower);

	cmp = range_cmp_bounds(typcache, arg, centroid);

	if (centroid->lower)
	{
		/*------
		 * The argument is an upper bound, we are searching for adjacent lower
		 * bounds. A matching adjacent lower bound must be *larger* than the
		 * argument, but only just.
		 *
		 * The following table illustrates the desired result with a fixed
		 * argument bound, and different centroids. The CMP column shows
		 * the value of 'cmp' variable, and ADJ shows whether the argument
		 * and centroid are adjacent, per bounds_adjacent(). (N) means we
		 * don't need to check for that case, because it's implied by CMP.
		 * With the argument range [..., 500), the adjacent range we're
		 * searching for is [500, ...):
		 *
		 *	ARGUMENT   CENTROID		CMP   ADJ
		 *	[..., 500) [498, ...)	 >	  (N)	[500, ...) is to the right
		 *	[..., 500) [499, ...)	 =	  (N)	[500, ...) is to the right
		 *	[..., 500) [500, ...)	 <	   Y	[500, ...) is to the right
		 *	[..., 500) [501, ...)	 <	   N	[500, ...) is to the left
		 *
		 * So, we must search left when the argument is smaller than, and not
		 * adjacent, to the centroid. Otherwise search right.
		 *------
		 */
		if (cmp < 0 && !bounds_adjacent(typcache, *arg, *centroid))
			return -1;
		else
			return 1;
	}
	else
	{
		/*------
		 * The argument is a lower bound, we are searching for adjacent upper
		 * bounds. A matching adjacent upper bound must be *smaller* than the
		 * argument, but only just.
		 *
		 *	ARGUMENT   CENTROID		CMP   ADJ
		 *	[500, ...) [..., 499)	 >	  (N)	[..., 500) is to the right
		 *	[500, ...) [..., 500)	 >	  (Y)	[..., 500) is to the right
		 *	[500, ...) [..., 501)	 =	  (N)	[..., 500) is to the left
		 *	[500, ...) [..., 502)	 <	  (N)	[..., 500) is to the left
		 *
		 * We must search left when the argument is smaller than or equal to
		 * the centroid. Otherwise search right. We don't need to check
		 * whether the argument is adjacent with the centroid, because it
		 * doesn't matter.
		 *------
		 */
		if (cmp <= 0)
			return -1;
		else
			return 1;
	}
}

/*----------
 * adjacent_inner_consistent
 *
 * Like adjacent_cmp_bounds, but also takes into account the previous
 * level's centroid. We might've traversed left (or right) at the previous
 * node, in search for ranges adjacent to the other bound, even though we
 * already ruled out the possibility for any matches in that direction for
 * this bound. By comparing the argument with the previous centroid, and
 * the previous centroid with the current centroid, we can determine which
 * direction we should've moved in at previous level, and which direction we
 * actually moved.
 *
 * If there can be any matches to the left, returns -1. If to the right,
 * returns 1. If there can be no matches below this centroid, because we
 * already ruled them out at the previous level, returns 0.
 *
 * XXX: Comparing just the previous and current level isn't foolproof; we
 * might still search some branches unnecessarily. For example, imagine that
 * we are searching for value 15, and we traverse the following centroids
 * (only considering one bound for the moment):
 *
 * Level 1: 20
 * Level 2: 50
 * Level 3: 25
 *
 * At this point, previous centroid is 50, current centroid is 25, and the
 * target value is to the left. But because we already moved right from
 * centroid 20 to 50 in the first level, there cannot be any values < 20 in
 * the current branch. But we don't know that just by looking at the previous
 * and current centroid, so we traverse left, unnecessarily. The reason we are
 * down this branch is that we're searching for matches with the *other*
 * bound. If we kept track of which bound we are searching for explicitly,
 * instead of deducing that from the previous and current centroid, we could
 * avoid some unnecessary work.
 *----------
 */
static int
adjacent_inner_consistent(TypeCacheEntry *typcache, RangeBound *arg,
						  RangeBound *centroid, RangeBound *prev)
{
	if (prev)
	{
		int			prevcmp;
		int			cmp;

		/*
		 * Which direction were we supposed to traverse at previous level,
		 * left or right?
		 */
		prevcmp = adjacent_cmp_bounds(typcache, arg, prev);

		/* and which direction did we actually go? */
		cmp = range_cmp_bounds(typcache, centroid, prev);

		/* if the two don't agree, there's nothing to see here */
		if ((prevcmp < 0 && cmp >= 0) || (prevcmp > 0 && cmp < 0))
			return 0;
	}

	return adjacent_cmp_bounds(typcache, arg, centroid);
}

/*
 * SP-GiST consistent function for leaf nodes: check leaf value against query
 * using corresponding function.
 */
Datum
spg_range_quad_leaf_consistent(PG_FUNCTION_ARGS)
{
	spgLeafConsistentIn *in = (spgLeafConsistentIn *) PG_GETARG_POINTER(0);
	spgLeafConsistentOut *out = (spgLeafConsistentOut *) PG_GETARG_POINTER(1);
	RangeType  *leafRange = DatumGetRangeType(in->leafDatum);
	TypeCacheEntry *typcache;
	bool		res;
	int			i;

	/* all tests are exact */
	out->recheck = false;

	/* leafDatum is what it is... */
	out->leafValue = in->leafDatum;

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(leafRange));

	/* Perform the required comparison(s) */
	res = true;
	for (i = 0; i < in->nkeys; i++)
	{
		Datum		keyDatum = in->scankeys[i].sk_argument;

		/* Call the function corresponding to the scan strategy */
		switch (in->scankeys[i].sk_strategy)
		{
			case RANGESTRAT_BEFORE:
				res = range_before_internal(typcache, leafRange,
											DatumGetRangeType(keyDatum));
				break;
			case RANGESTRAT_OVERLEFT:
				res = range_overleft_internal(typcache, leafRange,
											  DatumGetRangeType(keyDatum));
				break;
			case RANGESTRAT_OVERLAPS:
				res = range_overlaps_internal(typcache, leafRange,
											  DatumGetRangeType(keyDatum));
				break;
			case RANGESTRAT_OVERRIGHT:
				res = range_overright_internal(typcache, leafRange,
											   DatumGetRangeType(keyDatum));
				break;
			case RANGESTRAT_AFTER:
				res = range_after_internal(typcache, leafRange,
										   DatumGetRangeType(keyDatum));
				break;
			case RANGESTRAT_ADJACENT:
				res = range_adjacent_internal(typcache, leafRange,
											  DatumGetRangeType(keyDatum));
				break;
			case RANGESTRAT_CONTAINS:
				res = range_contains_internal(typcache, leafRange,
											  DatumGetRangeType(keyDatum));
				break;
			case RANGESTRAT_CONTAINED_BY:
				res = range_contained_by_internal(typcache, leafRange,
												DatumGetRangeType(keyDatum));
				break;
			case RANGESTRAT_CONTAINS_ELEM:
				res = range_contains_elem_internal(typcache, leafRange,
												   keyDatum);
				break;
			case RANGESTRAT_EQ:
				res = range_eq_internal(typcache, leafRange,
										DatumGetRangeType(keyDatum));
				break;
			default:
				elog(ERROR, "unrecognized range strategy: %d",
					 in->scankeys[i].sk_strategy);
				break;
		}

		/*
		 * If leaf datum doesn't match to a query key, no need to check
		 * subsequent keys.
		 */
		if (!res)
			break;
	}

	PG_RETURN_BOOL(res);
}
