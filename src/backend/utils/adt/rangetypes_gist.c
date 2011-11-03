/*-------------------------------------------------------------------------
 *
 * rangetypes_gist.c
 *	  GiST support for range types.
 *
 * Copyright (c) 2006-2011, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/rangetypes_gist.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gist.h"
#include "access/skey.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rangetypes.h"

#define RANGESTRAT_EQ					1
#define RANGESTRAT_NE					2
#define RANGESTRAT_OVERLAPS				3
#define RANGESTRAT_CONTAINS_ELEM		4
#define RANGESTRAT_ELEM_CONTAINED_BY	5
#define RANGESTRAT_CONTAINS				6
#define RANGESTRAT_CONTAINED_BY			7
#define RANGESTRAT_BEFORE				8
#define RANGESTRAT_AFTER				9
#define RANGESTRAT_OVERLEFT				10
#define RANGESTRAT_OVERRIGHT			11
#define RANGESTRAT_ADJACENT				12

static RangeType *range_super_union(FunctionCallInfo fcinfo, RangeType *r1,
									RangeType *r2);
static bool range_gist_consistent_int(FunctionCallInfo fcinfo,
									  StrategyNumber strategy, RangeType *key,
									  RangeType *query);
static bool range_gist_consistent_leaf(FunctionCallInfo fcinfo,
									   StrategyNumber strategy, RangeType *key,
									   RangeType *query);
static int sort_item_cmp(const void *a, const void *b);

/*
 * Auxiliary structure for picksplit method.
 */
typedef struct
{
	int					 index;
	RangeType			*data;
	FunctionCallInfo	 fcinfo;
} PickSplitSortItem;


Datum
range_gist_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY			*entry	  = (GISTENTRY *) PG_GETARG_POINTER(0);
	Datum				 dquery	  = PG_GETARG_DATUM(1);
	StrategyNumber		 strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	/* Oid subtype = PG_GETARG_OID(3); */
	bool				*recheck  = (bool *) PG_GETARG_POINTER(4);
	RangeType			*key	  = DatumGetRangeType(entry->key);
	RangeType			*query;

	RangeBound	lower;
	RangeBound	upper;
	bool		empty;
	Oid			rngtypid;

	*recheck = false;
	range_deserialize(fcinfo, key, &lower, &upper, &empty);
	rngtypid = lower.rngtypid;

	switch (strategy)
	{
		RangeBound lower;
		RangeBound upper;

		/*
		 * For contains and contained by operators, the other operand is a
		 * "point" of the subtype. Construct a singleton range containing just
		 * that value.
		 */
		case RANGESTRAT_CONTAINS_ELEM:
		case RANGESTRAT_ELEM_CONTAINED_BY:
			lower.rngtypid	= rngtypid;
			lower.inclusive = true;
			lower.val		= dquery;
			lower.lower		= true;
			lower.infinite	= false;
			upper.rngtypid	= rngtypid;
			upper.inclusive = true;
			upper.val		= dquery;
			upper.lower		= false;
			upper.infinite	= false;
			query			= DatumGetRangeType(
				make_range(fcinfo, &lower, &upper, false));
			break;

		default:
			query			= DatumGetRangeType(dquery);
			break;
	}

	if (GIST_LEAF(entry))
		PG_RETURN_BOOL(range_gist_consistent_leaf(
						   fcinfo, strategy, key, query));
	else
		PG_RETURN_BOOL(range_gist_consistent_int(
						   fcinfo, strategy, key, query));
}

Datum
range_gist_union(PG_FUNCTION_ARGS)
{
	GistEntryVector		*entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GISTENTRY			*ent	  = entryvec->vector;
	RangeType			*result_range;
	int					 i;

	result_range = DatumGetRangeType(ent[0].key);

	for (i = 1; i < entryvec->n; i++)
	{
		result_range = range_super_union(fcinfo, result_range,
										 DatumGetRangeType(ent[i].key));
	}

	PG_RETURN_RANGE(result_range);
}

Datum
range_gist_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	PG_RETURN_POINTER(entry);
}

Datum
range_gist_decompress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	PG_RETURN_POINTER(entry);
}

Datum
range_gist_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY	*origentry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY	*newentry  = (GISTENTRY *) PG_GETARG_POINTER(1);
	float		*penalty   = (float *) PG_GETARG_POINTER(2);
	RangeType	*orig	   = DatumGetRangeType(origentry->key);
	RangeType	*new	   = DatumGetRangeType(newentry->key);
	RangeType	*s_union   = range_super_union(fcinfo, orig, new);

	FmgrInfo	*subtype_diff;

	RangeBound	lower1, lower2;
	RangeBound	upper1, upper2;
	bool		empty1, empty2;

	float		lower_diff, upper_diff;

	RangeTypeInfo rngtypinfo;

	range_deserialize(fcinfo, orig, &lower1, &upper1, &empty1);
	range_deserialize(fcinfo, s_union, &lower2, &upper2, &empty2);

	range_gettypinfo(fcinfo, lower1.rngtypid, &rngtypinfo);
	subtype_diff = &rngtypinfo.subdiffFn;

	Assert(empty1 || !empty2);

	if (empty1 && empty2)
		return 0;
	else if (empty1 && !empty2)
	{
		if (lower2.infinite || upper2.infinite)
			/* from empty to infinite */
			return get_float8_infinity();
		else if (subtype_diff->fn_addr != NULL)
			/* from empty to upper2-lower2 */
			return DatumGetFloat8(FunctionCall2(subtype_diff,
												upper2.val, lower2.val));
		else
			/* wild guess */
			return 1.0;
	}

	Assert(lower2.infinite || !lower1.infinite);

	if (lower2.infinite && !lower1.infinite)
		lower_diff = get_float8_infinity();
	else if (lower2.infinite && lower1.infinite)
		lower_diff = 0;
	else if (subtype_diff->fn_addr != NULL)
	{
		lower_diff = DatumGetFloat8(FunctionCall2(subtype_diff,
												  lower1.val, lower2.val));
		if (lower_diff < 0)
			lower_diff = 0;		/* subtype_diff is broken */
	}
	else /* only know whether there is a difference or not */
		lower_diff = (float) range_cmp_bounds(fcinfo, &lower1, &lower2);

	Assert(upper2.infinite || !upper1.infinite);

	if (upper2.infinite && !upper1.infinite)
		upper_diff = get_float8_infinity();
	else if (upper2.infinite && upper1.infinite)
		upper_diff = 0;
	else if (subtype_diff->fn_addr != NULL)
	{
		upper_diff = DatumGetFloat8(FunctionCall2(subtype_diff,
												  upper2.val, upper1.val));
		if (upper_diff < 0)
			upper_diff = 0;		/* subtype_diff is broken */
	}
	else /* only know whether there is a difference or not */
		upper_diff = (float) range_cmp_bounds(fcinfo, &upper2, &upper1);

	Assert(lower_diff >= 0 && upper_diff >= 0);

	*penalty = (float) (lower_diff + upper_diff);
	PG_RETURN_POINTER(penalty);
}

/*
 * The GiST PickSplit method for ranges
 *
 * Algorithm based on sorting. Incoming array of periods is sorted using
 * sort_item_cmp function. After that first half of periods goes to the left
 * datum, and the second half of periods goes to the right datum.
 */
Datum
range_gist_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector		*entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC		*v		  = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	OffsetNumber		 i;
	RangeType			*pred_left;
	RangeType			*pred_right;
	PickSplitSortItem	*sortItems;
	int					 nbytes;
	OffsetNumber		 split_idx;
	OffsetNumber		*left;
	OffsetNumber		*right;
	OffsetNumber		 maxoff;

	maxoff = entryvec->n - 1;
	nbytes = (maxoff + 1) * sizeof(OffsetNumber);
	sortItems = (PickSplitSortItem *) palloc(
		maxoff * sizeof(PickSplitSortItem));
	v->spl_left = (OffsetNumber *) palloc(nbytes);
	v->spl_right = (OffsetNumber *) palloc(nbytes);

	/*
	 * Preparing auxiliary array and sorting.
	 */
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		sortItems[i - 1].index	= i;
		sortItems[i - 1].data	= DatumGetRangeType(entryvec->vector[i].key);
		sortItems[i - 1].fcinfo = fcinfo;
	}
	qsort(sortItems, maxoff, sizeof(PickSplitSortItem), sort_item_cmp);
	split_idx = maxoff / 2;

	left = v->spl_left;
	v->spl_nleft = 0;
	right = v->spl_right;
	v->spl_nright = 0;

	/*
	 * First half of segs goes to the left datum.
	 */
	pred_left = DatumGetRangeType(sortItems[0].data);
	*left++ = sortItems[0].index;
	v->spl_nleft++;
	for (i = 1; i < split_idx; i++)
	{
		pred_left = range_super_union(fcinfo, pred_left,
									  DatumGetRangeType(sortItems[i].data));
		*left++ = sortItems[i].index;
		v->spl_nleft++;
	}

	/*
	 * Second half of segs goes to the right datum.
	 */
	pred_right = DatumGetRangeType(sortItems[split_idx].data);
	*right++ = sortItems[split_idx].index;
	v->spl_nright++;
	for (i = split_idx + 1; i < maxoff; i++)
	{
		pred_right = range_super_union(fcinfo, pred_right,
									   DatumGetRangeType(sortItems[i].data));
		*right++ = sortItems[i].index;
		v->spl_nright++;
	}

	*left = *right = FirstOffsetNumber; /* sentinel value, see dosplit() */

	v->spl_ldatum = RangeTypeGetDatum(pred_left);
	v->spl_rdatum = RangeTypeGetDatum(pred_right);

	PG_RETURN_POINTER(v);
}

Datum
range_gist_same(PG_FUNCTION_ARGS)
{
	Datum r1 = PG_GETARG_DATUM(0);
	Datum r2 = PG_GETARG_DATUM(1);
	bool *result = (bool *) PG_GETARG_POINTER(2);

	*result = DatumGetBool(OidFunctionCall2(F_RANGE_EQ, r1, r2));
	PG_RETURN_POINTER(result);
}

/*
 *----------------------------------------------------------
 * STATIC FUNCTIONS
 *----------------------------------------------------------
 */

/* return the smallest range that contains r1 and r2 */
static RangeType *
range_super_union(FunctionCallInfo fcinfo, RangeType *r1, RangeType *r2)
{
	RangeBound	 lower1, lower2;
	RangeBound	 upper1, upper2;
	bool		 empty1, empty2;
	RangeBound	*result_lower;
	RangeBound	*result_upper;

	range_deserialize(fcinfo, r1, &lower1, &upper1, &empty1);
	range_deserialize(fcinfo, r2, &lower2, &upper2, &empty2);

	if (empty1)
		return r2;
	if (empty2)
		return r1;

	if (range_cmp_bounds(fcinfo, &lower1, &lower2) <= 0)
		result_lower = &lower1;
	else
		result_lower = &lower2;

	if (range_cmp_bounds(fcinfo, &upper1, &upper2) >= 0)
		result_upper = &upper1;
	else
		result_upper = &upper2;

	/* optimization to avoid constructing a new range */
	if (result_lower == &lower1 && result_upper == &upper1)
		return r1;
	if (result_lower == &lower2 && result_upper == &upper2)
		return r2;

	return DatumGetRangeType(
		make_range(fcinfo, result_lower, result_upper, false));
}

static bool
range_gist_consistent_int(FunctionCallInfo fcinfo, StrategyNumber strategy,
						  RangeType *key, RangeType *query)
{
	Oid proc = InvalidOid;

	RangeBound	lower1, lower2;
	RangeBound	upper1, upper2;
	bool		empty1, empty2;

	bool retval;
	bool negate = false;

	range_deserialize(fcinfo, key, &lower1, &upper1, &empty1);
	range_deserialize(fcinfo, query, &lower2, &upper2, &empty2);

	switch (strategy)
	{
		case RANGESTRAT_EQ:
			proc   = F_RANGE_CONTAINS;
			break;
		case RANGESTRAT_NE:
			return true;
			break;
		case RANGESTRAT_OVERLAPS:
			proc   = F_RANGE_OVERLAPS;
			break;
		case RANGESTRAT_CONTAINS_ELEM:
		case RANGESTRAT_CONTAINS:
			proc   = F_RANGE_CONTAINS;
			break;
		case RANGESTRAT_ELEM_CONTAINED_BY:
		case RANGESTRAT_CONTAINED_BY:
			return true;
			break;
		case RANGESTRAT_BEFORE:
			if (empty1)
				return false;
			proc   = F_RANGE_OVERRIGHT;
			negate = true;
			break;
		case RANGESTRAT_AFTER:
			if (empty1)
				return false;
			proc   = F_RANGE_OVERLEFT;
			negate = true;
			break;
		case RANGESTRAT_OVERLEFT:
			if (empty1)
				return false;
			proc   = F_RANGE_AFTER;
			negate = true;
			break;
		case RANGESTRAT_OVERRIGHT:
			if (empty1)
				return false;
			proc = F_RANGE_BEFORE;
			negate = true;
			break;
		case RANGESTRAT_ADJACENT:
			if (empty1 || empty2)
				return false;
			if (DatumGetBool(
					OidFunctionCall2(F_RANGE_ADJACENT,
									 RangeTypeGetDatum(key),
									 RangeTypeGetDatum(query))))
				return true;
			proc = F_RANGE_OVERLAPS;
			break;
	}

	retval = DatumGetBool(OidFunctionCall2(proc, RangeTypeGetDatum(key),
										   RangeTypeGetDatum(query)));

	if (negate)
		retval = !retval;

	PG_RETURN_BOOL(retval);
}

static bool
range_gist_consistent_leaf(FunctionCallInfo fcinfo, StrategyNumber strategy,
						   RangeType *key, RangeType *query)
{
	Oid proc = InvalidOid;

	RangeBound	lower1, lower2;
	RangeBound	upper1, upper2;
	bool		empty1, empty2;

	range_deserialize(fcinfo, key, &lower1, &upper1, &empty1);
	range_deserialize(fcinfo, query, &lower2, &upper2, &empty2);

	switch (strategy)
	{
		case RANGESTRAT_EQ:
			proc = F_RANGE_EQ;
			break;
		case RANGESTRAT_NE:
			proc = F_RANGE_NE;
			break;
		case RANGESTRAT_OVERLAPS:
			proc = F_RANGE_OVERLAPS;
			break;
		case RANGESTRAT_CONTAINS_ELEM:
		case RANGESTRAT_CONTAINS:
			proc = F_RANGE_CONTAINS;
			break;
		case RANGESTRAT_ELEM_CONTAINED_BY:
		case RANGESTRAT_CONTAINED_BY:
			proc = F_RANGE_CONTAINED_BY;
			break;
		case RANGESTRAT_BEFORE:
			if (empty1 || empty2)
				return false;
			proc = F_RANGE_BEFORE;
			break;
		case RANGESTRAT_AFTER:
			if (empty1 || empty2)
				return false;
			proc = F_RANGE_AFTER;
			break;
		case RANGESTRAT_OVERLEFT:
			if (empty1 || empty2)
				return false;
			proc = F_RANGE_OVERLEFT;
			break;
		case RANGESTRAT_OVERRIGHT:
			if (empty1 || empty2)
				return false;
			proc = F_RANGE_OVERRIGHT;
			break;
		case RANGESTRAT_ADJACENT:
			if (empty1 || empty2)
				return false;
			proc = F_RANGE_ADJACENT;
			break;
	}

	return DatumGetBool(OidFunctionCall2(proc, RangeTypeGetDatum(key),
										 RangeTypeGetDatum(query)));
}

/*
 * Compare function for PickSplitSortItem. This is actually the
 * interesting part of the picksplit algorithm.
 *
 * We want to separate out empty ranges, bounded ranges, and unbounded
 * ranges. We assume that "contains" and "overlaps" are the most
 * important queries, so empty ranges will rarely match and unbounded
 * ranges frequently will. Bounded ranges should be in the middle.
 *
 * Empty ranges we push all the way to the left, then bounded ranges
 * (sorted on lower bound, then upper), then ranges with no lower
 * bound, then ranges with no upper bound; and finally, ranges with no
 * upper or lower bound all the way to the right.
 */
static int
sort_item_cmp(const void *a, const void *b)
{
	PickSplitSortItem	*i1 = (PickSplitSortItem *)a;
	PickSplitSortItem	*i2 = (PickSplitSortItem *)b;
	RangeType			*r1 = i1->data;
	RangeType			*r2 = i2->data;

	RangeBound	lower1, lower2;
	RangeBound	upper1, upper2;
	bool		empty1, empty2;

	FunctionCallInfo fcinfo = i1->fcinfo;

	int cmp;

	range_deserialize(fcinfo, r1, &lower1, &upper1, &empty1);
	range_deserialize(fcinfo, r2, &lower2, &upper2, &empty2);

	if (empty1 || empty2)
	{
		if (empty1 && empty2)
			return 0;
		else if (empty1)
			return -1;
		else if (empty2)
			return 1;
		else
			Assert(false);
	}

	/*
	 * If both lower or both upper bounds are infinite, we sort by
	 * ascending range size. That means that if both upper bounds are
	 * infinite, we sort by the lower bound _descending_. That creates
	 * a slightly odd total order, but keeps the pages with very
	 * unselective predicates grouped more closely together on the
	 * right.
	 */
	if (lower1.infinite || upper1.infinite ||
		lower2.infinite || upper2.infinite)
	{
		if (lower1.infinite && lower2.infinite)
			return range_cmp_bounds(fcinfo, &upper1, &upper2);
		else if (lower1.infinite)
			return -1;
		else if (lower2.infinite)
			return 1;
		else if (upper1.infinite && upper2.infinite)
			return -1 * range_cmp_bounds(fcinfo, &lower1, &lower2);
		else if (upper1.infinite)
			return 1;
		else if (upper2.infinite)
			return -1;
		else
			Assert(false);
	}

	if ((cmp = range_cmp_bounds(fcinfo, &lower1, &lower2)) != 0)
		return cmp;

	return range_cmp_bounds(fcinfo, &upper1, &upper2);
}
