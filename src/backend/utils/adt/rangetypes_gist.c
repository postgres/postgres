/*-------------------------------------------------------------------------
 *
 * rangetypes_gist.c
 *	  GiST support for range types.
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
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
#include "utils/rangetypes.h"


/* Operator strategy numbers used in the GiST range opclass */
/* Numbers are chosen to match up operator names with existing usages */
#define RANGESTRAT_BEFORE				1
#define RANGESTRAT_OVERLEFT				2
#define RANGESTRAT_OVERLAPS				3
#define RANGESTRAT_OVERRIGHT			4
#define RANGESTRAT_AFTER				5
#define RANGESTRAT_ADJACENT				6
#define RANGESTRAT_CONTAINS				7
#define RANGESTRAT_CONTAINED_BY			8
#define RANGESTRAT_CONTAINS_ELEM		16
#define RANGESTRAT_EQ					18
#define RANGESTRAT_NE					19

/*
 * Auxiliary structure for picksplit method.
 */
typedef struct
{
	int			index;			/* original index in entryvec->vector[] */
	RangeType  *data;			/* range value to sort */
	TypeCacheEntry *typcache;	/* range type's info */
}	PickSplitSortItem;

static RangeType *range_super_union(TypeCacheEntry *typcache, RangeType * r1,
				  RangeType * r2);
static bool range_gist_consistent_int(FmgrInfo *flinfo,
						  StrategyNumber strategy, RangeType *key,
						  Datum query);
static bool range_gist_consistent_leaf(FmgrInfo *flinfo,
						   StrategyNumber strategy, RangeType *key,
						   Datum query);
static int	sort_item_cmp(const void *a, const void *b);


/* GiST query consistency check */
Datum
range_gist_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	Datum		query = PG_GETARG_DATUM(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	/* Oid subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	RangeType  *key = DatumGetRangeType(entry->key);

	/* All operators served by this function are exact */
	*recheck = false;

	if (GIST_LEAF(entry))
		PG_RETURN_BOOL(range_gist_consistent_leaf(fcinfo->flinfo, strategy,
												  key, query));
	else
		PG_RETURN_BOOL(range_gist_consistent_int(fcinfo->flinfo, strategy,
												 key, query));
}

/* form union range */
Datum
range_gist_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GISTENTRY  *ent = entryvec->vector;
	RangeType  *result_range;
	TypeCacheEntry *typcache;
	int			i;

	result_range = DatumGetRangeType(ent[0].key);

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(result_range));

	for (i = 1; i < entryvec->n; i++)
	{
		result_range = range_super_union(typcache, result_range,
										 DatumGetRangeType(ent[i].key));
	}

	PG_RETURN_RANGE(result_range);
}

/* compress, decompress are no-ops */
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

/* page split penalty function */
Datum
range_gist_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *origentry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *newentry = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *penalty = (float *) PG_GETARG_POINTER(2);
	RangeType  *orig = DatumGetRangeType(origentry->key);
	RangeType  *new = DatumGetRangeType(newentry->key);
	TypeCacheEntry *typcache;
	RangeType  *s_union;
	FmgrInfo   *subtype_diff;
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;
	float8		lower_diff,
				upper_diff;

	if (RangeTypeGetOid(orig) != RangeTypeGetOid(new))
		elog(ERROR, "range types do not match");

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(orig));

	subtype_diff = &typcache->rng_subdiff_finfo;

	/*
	 * We want to compare the size of "orig" to size of "orig union new".
	 * The penalty will be the sum of the reduction in the lower bound plus
	 * the increase in the upper bound.
	 */
	s_union = range_super_union(typcache, orig, new);

	range_deserialize(typcache, orig, &lower1, &upper1, &empty1);
	range_deserialize(typcache, s_union, &lower2, &upper2, &empty2);

	/* handle cases where orig is empty */
	if (empty1 && empty2)
	{
		*penalty = 0;
		PG_RETURN_POINTER(penalty);
	}
	else if (empty1)
	{
		if (lower2.infinite || upper2.infinite)
		{
			/* from empty to infinite */
			*penalty = get_float4_infinity();
			PG_RETURN_POINTER(penalty);
		}
		else if (OidIsValid(subtype_diff->fn_oid))
		{
			/* from empty to upper2-lower2 */
			*penalty = DatumGetFloat8(FunctionCall2Coll(subtype_diff,
														typcache->rng_collation,
														upper2.val,
														lower2.val));
			/* upper2 must be >= lower2 */
			if (*penalty < 0)
				*penalty = 0;		/* subtype_diff is broken */
			PG_RETURN_POINTER(penalty);
		}
		else
		{
			/* wild guess */
			*penalty = 1.0;
			PG_RETURN_POINTER(penalty);
		}
	}

	/* if orig isn't empty, s_union can't be either */
	Assert(!empty2);

	/* similarly, if orig's lower bound is infinite, s_union's must be too */
	Assert(lower2.infinite || !lower1.infinite);

	if (lower2.infinite && lower1.infinite)
		lower_diff = 0;
	else if (lower2.infinite)
		lower_diff = get_float8_infinity();
	else if (OidIsValid(subtype_diff->fn_oid))
	{
		lower_diff = DatumGetFloat8(FunctionCall2Coll(subtype_diff,
													  typcache->rng_collation,
													  lower1.val,
													  lower2.val));
		/* orig's lower bound must be >= s_union's */
		if (lower_diff < 0)
			lower_diff = 0;		/* subtype_diff is broken */
	}
	else
	{
		/* only know whether there is a difference or not */
		lower_diff = range_cmp_bounds(typcache, &lower1, &lower2) > 0 ? 1 : 0;
	}

	/* similarly, if orig's upper bound is infinite, s_union's must be too */
	Assert(upper2.infinite || !upper1.infinite);

	if (upper2.infinite && upper1.infinite)
		upper_diff = 0;
	else if (upper2.infinite)
		upper_diff = get_float8_infinity();
	else if (OidIsValid(subtype_diff->fn_oid))
	{
		upper_diff = DatumGetFloat8(FunctionCall2Coll(subtype_diff,
													  typcache->rng_collation,
													  upper2.val,
													  upper1.val));
		/* orig's upper bound must be <= s_union's */
		if (upper_diff < 0)
			upper_diff = 0;		/* subtype_diff is broken */
	}
	else
	{
		/* only know whether there is a difference or not */
		upper_diff = range_cmp_bounds(typcache, &upper2, &upper1) > 0 ? 1 : 0;
	}

	Assert(lower_diff >= 0 && upper_diff >= 0);

	*penalty = (float) (lower_diff + upper_diff);
	PG_RETURN_POINTER(penalty);
}

/*
 * The GiST PickSplit method for ranges
 *
 * Algorithm based on sorting.  Incoming array of ranges is sorted using
 * sort_item_cmp function.  After that first half of ranges goes to the left
 * output, and the second half of ranges goes to the right output.
 */
Datum
range_gist_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	TypeCacheEntry *typcache;
	OffsetNumber i;
	RangeType  *pred_left;
	RangeType  *pred_right;
	PickSplitSortItem *sortItems;
	int			nbytes;
	OffsetNumber split_idx;
	OffsetNumber *left;
	OffsetNumber *right;
	OffsetNumber maxoff;

	/* use first item to look up range type's info */
	pred_left = DatumGetRangeType(entryvec->vector[FirstOffsetNumber].key);
	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(pred_left));

	/* allocate result and work arrays */
	maxoff = entryvec->n - 1;
	nbytes = (maxoff + 1) * sizeof(OffsetNumber);
	v->spl_left = (OffsetNumber *) palloc(nbytes);
	v->spl_right = (OffsetNumber *) palloc(nbytes);
	sortItems = (PickSplitSortItem *) palloc(maxoff * sizeof(PickSplitSortItem));

	/*
	 * Prepare auxiliary array and sort the values.
	 */
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		sortItems[i - 1].index = i;
		sortItems[i - 1].data = DatumGetRangeType(entryvec->vector[i].key);
		sortItems[i - 1].typcache = typcache;
	}
	qsort(sortItems, maxoff, sizeof(PickSplitSortItem), sort_item_cmp);

	split_idx = maxoff / 2;

	left = v->spl_left;
	v->spl_nleft = 0;
	right = v->spl_right;
	v->spl_nright = 0;

	/*
	 * First half of items goes to the left output.
	 */
	pred_left = sortItems[0].data;
	*left++ = sortItems[0].index;
	v->spl_nleft++;
	for (i = 1; i < split_idx; i++)
	{
		pred_left = range_super_union(typcache, pred_left, sortItems[i].data);
		*left++ = sortItems[i].index;
		v->spl_nleft++;
	}

	/*
	 * Second half of items goes to the right output.
	 */
	pred_right = sortItems[split_idx].data;
	*right++ = sortItems[split_idx].index;
	v->spl_nright++;
	for (i = split_idx + 1; i < maxoff; i++)
	{
		pred_right = range_super_union(typcache, pred_right, sortItems[i].data);
		*right++ = sortItems[i].index;
		v->spl_nright++;
	}

	*left = *right = FirstOffsetNumber; /* sentinel value, see dosplit() */

	v->spl_ldatum = RangeTypeGetDatum(pred_left);
	v->spl_rdatum = RangeTypeGetDatum(pred_right);

	PG_RETURN_POINTER(v);
}

/* equality comparator for GiST */
Datum
range_gist_same(PG_FUNCTION_ARGS)
{
	/* Datum r1 = PG_GETARG_DATUM(0); */
	/* Datum r2 = PG_GETARG_DATUM(1); */
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	/*
	 * We can safely call range_eq using our fcinfo directly; it won't notice
	 * the third argument.  This allows it to use fn_extra for caching.
	 */
	*result = DatumGetBool(range_eq(fcinfo));

	PG_RETURN_POINTER(result);
}

/*
 *----------------------------------------------------------
 * STATIC FUNCTIONS
 *----------------------------------------------------------
 */

/*
 * Return the smallest range that contains r1 and r2
 *
 * XXX would it be better to redefine range_union as working this way?
 */
static RangeType *
range_super_union(TypeCacheEntry *typcache, RangeType * r1, RangeType * r2)
{
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;
	RangeBound *result_lower;
	RangeBound *result_upper;

	range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
	range_deserialize(typcache, r2, &lower2, &upper2, &empty2);

	if (empty1)
		return r2;
	if (empty2)
		return r1;

	if (range_cmp_bounds(typcache, &lower1, &lower2) <= 0)
		result_lower = &lower1;
	else
		result_lower = &lower2;

	if (range_cmp_bounds(typcache, &upper1, &upper2) >= 0)
		result_upper = &upper1;
	else
		result_upper = &upper2;

	/* optimization to avoid constructing a new range */
	if (result_lower == &lower1 && result_upper == &upper1)
		return r1;
	if (result_lower == &lower2 && result_upper == &upper2)
		return r2;

	return make_range(typcache, result_lower, result_upper, false);
}

/*
 * trick function call: call the given function with given FmgrInfo
 *
 * To allow the various functions called here to cache lookups of range
 * datatype information, we use a trick: we pass them the FmgrInfo struct
 * for the GiST consistent function.  This relies on the knowledge that
 * none of them consult FmgrInfo for anything but fn_extra, and that they
 * all use fn_extra the same way, i.e. as a pointer to the typcache entry
 * for the range data type.  Since the FmgrInfo is long-lived (it's actually
 * part of the relcache entry for the index, typically) this essentially
 * eliminates lookup overhead during operations on a GiST range index.
 */
static Datum
TrickFunctionCall2(PGFunction proc, FmgrInfo *flinfo, Datum arg1, Datum arg2)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	InitFunctionCallInfoData(fcinfo, flinfo, 2, InvalidOid, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;

	result = (*proc) (&fcinfo);

	if (fcinfo.isnull)
		elog(ERROR, "function %p returned NULL", proc);

	return result;
}

/*
 * GiST consistent test on an index internal page
 */
static bool
range_gist_consistent_int(FmgrInfo *flinfo, StrategyNumber strategy,
						  RangeType *key, Datum query)
{
	PGFunction	proc;
	bool		negate = false;
	bool		retval;

	switch (strategy)
	{
		case RANGESTRAT_BEFORE:
			if (RangeIsEmpty(key))
				return false;
			proc = range_overright;
			negate = true;
			break;
		case RANGESTRAT_OVERLEFT:
			if (RangeIsEmpty(key))
				return false;
			proc = range_after;
			negate = true;
			break;
		case RANGESTRAT_OVERLAPS:
			proc = range_overlaps;
			break;
		case RANGESTRAT_OVERRIGHT:
			if (RangeIsEmpty(key))
				return false;
			proc = range_before;
			negate = true;
			break;
		case RANGESTRAT_AFTER:
			if (RangeIsEmpty(key))
				return false;
			proc = range_overleft;
			negate = true;
			break;
		case RANGESTRAT_ADJACENT:
			if (RangeIsEmpty(key) || RangeIsEmpty(DatumGetRangeType(query)))
				return false;
			if (DatumGetBool(TrickFunctionCall2(range_adjacent, flinfo,
												RangeTypeGetDatum(key),
												query)))
				return true;
			proc = range_overlaps;
			break;
		case RANGESTRAT_CONTAINS:
			proc = range_contains;
			break;
		case RANGESTRAT_CONTAINED_BY:
			return true;
			break;
		case RANGESTRAT_CONTAINS_ELEM:
			proc = range_contains_elem;
			break;
		case RANGESTRAT_EQ:
			proc = range_contains;
			break;
		case RANGESTRAT_NE:
			return true;
			break;
		default:
			elog(ERROR, "unrecognized range strategy: %d", strategy);
			proc = NULL;		/* keep compiler quiet */
			break;
	}

	retval = DatumGetBool(TrickFunctionCall2(proc, flinfo,
											 RangeTypeGetDatum(key),
											 query));
	if (negate)
		retval = !retval;

	return retval;
}

/*
 * GiST consistent test on an index leaf page
 */
static bool
range_gist_consistent_leaf(FmgrInfo *flinfo, StrategyNumber strategy,
						   RangeType *key, Datum query)
{
	PGFunction	proc;

	switch (strategy)
	{
		case RANGESTRAT_BEFORE:
			proc = range_before;
			break;
		case RANGESTRAT_OVERLEFT:
			proc = range_overleft;
			break;
		case RANGESTRAT_OVERLAPS:
			proc = range_overlaps;
			break;
		case RANGESTRAT_OVERRIGHT:
			proc = range_overright;
			break;
		case RANGESTRAT_AFTER:
			proc = range_after;
			break;
		case RANGESTRAT_ADJACENT:
			proc = range_adjacent;
			break;
		case RANGESTRAT_CONTAINS:
			proc = range_contains;
			break;
		case RANGESTRAT_CONTAINED_BY:
			proc = range_contained_by;
			break;
		case RANGESTRAT_CONTAINS_ELEM:
			proc = range_contains_elem;
			break;
		case RANGESTRAT_EQ:
			proc = range_eq;
			break;
		case RANGESTRAT_NE:
			proc = range_ne;
			break;
		default:
			elog(ERROR, "unrecognized range strategy: %d", strategy);
			proc = NULL;		/* keep compiler quiet */
			break;
	}

	return DatumGetBool(TrickFunctionCall2(proc, flinfo,
										   RangeTypeGetDatum(key),
										   query));
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
	PickSplitSortItem *i1 = (PickSplitSortItem *) a;
	PickSplitSortItem *i2 = (PickSplitSortItem *) b;
	RangeType  *r1 = i1->data;
	RangeType  *r2 = i2->data;
	TypeCacheEntry *typcache = i1->typcache;
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;
	int			cmp;

	range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
	range_deserialize(typcache, r2, &lower2, &upper2, &empty2);

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
	 * If both lower or both upper bounds are infinite, we sort by ascending
	 * range size. That means that if both upper bounds are infinite, we sort
	 * by the lower bound _descending_. That creates a slightly odd total
	 * order, but keeps the pages with very unselective predicates grouped
	 * more closely together on the right.
	 */
	if (lower1.infinite || upper1.infinite ||
		lower2.infinite || upper2.infinite)
	{
		if (lower1.infinite && lower2.infinite)
			return range_cmp_bounds(typcache, &upper1, &upper2);
		else if (lower1.infinite)
			return -1;
		else if (lower2.infinite)
			return 1;
		else if (upper1.infinite && upper2.infinite)
			return -(range_cmp_bounds(typcache, &lower1, &lower2));
		else if (upper1.infinite)
			return 1;
		else if (upper2.infinite)
			return -1;
		else
			Assert(false);
	}

	if ((cmp = range_cmp_bounds(typcache, &lower1, &lower2)) != 0)
		return cmp;

	return range_cmp_bounds(typcache, &upper1, &upper2);
}
