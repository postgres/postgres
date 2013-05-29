/*-------------------------------------------------------------------------
 *
 * rangetypes_gist.c
 *	  GiST support for range types.
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
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
#include "utils/datum.h"
#include "utils/rangetypes.h"


/*
 * Range class properties used to segregate different classes of ranges in
 * GiST.  Each unique combination of properties is a class.  CLS_EMPTY cannot
 * be combined with anything else.
 */
#define CLS_NORMAL			0	/* Ordinary finite range (no bits set) */
#define CLS_LOWER_INF		1	/* Lower bound is infinity */
#define CLS_UPPER_INF		2	/* Upper bound is infinity */
#define CLS_CONTAIN_EMPTY	4	/* Contains underlying empty ranges */
#define CLS_EMPTY			8	/* Special class for empty ranges */

#define CLS_COUNT			9	/* # of classes; includes all combinations of
								 * properties. CLS_EMPTY doesn't combine with
								 * anything else, so it's only 2^3 + 1. */

/*
 * Minimum accepted ratio of split for items of the same class.  If the items
 * are of different classes, we will separate along those lines regardless of
 * the ratio.
 */
#define LIMIT_RATIO  0.3

/* Constants for fixed penalty values */
#define INFINITE_BOUND_PENALTY	2.0
#define CONTAIN_EMPTY_PENALTY  1.0
#define DEFAULT_SUBTYPE_DIFF_PENALTY  1.0

/*
 * Per-item data for range_gist_single_sorting_split.
 */
typedef struct
{
	int			index;
	RangeBound	bound;
} SingleBoundSortItem;

/* place on left or right side of split? */
typedef enum
{
	SPLIT_LEFT = 0,				/* makes initialization to SPLIT_LEFT easier */
	SPLIT_RIGHT
} SplitLR;

/*
 * Context for range_gist_consider_split.
 */
typedef struct
{
	TypeCacheEntry *typcache;	/* typcache for range type */
	bool		has_subtype_diff;		/* does it have subtype_diff? */
	int			entries_count;	/* total number of entries being split */

	/* Information about currently selected split follows */

	bool		first;			/* true if no split was selected yet */

	RangeBound *left_upper;		/* upper bound of left interval */
	RangeBound *right_lower;	/* lower bound of right interval */

	float4		ratio;			/* split ratio */
	float4		overlap;		/* overlap between left and right predicate */
	int			common_left;	/* # common entries destined for each side */
	int			common_right;
} ConsiderSplitContext;

/*
 * Bounds extracted from a non-empty range, for use in
 * range_gist_double_sorting_split.
 */
typedef struct
{
	RangeBound	lower;
	RangeBound	upper;
} NonEmptyRange;

/*
 * Represents information about an entry that can be placed in either group
 * without affecting overlap over selected axis ("common entry").
 */
typedef struct
{
	/* Index of entry in the initial array */
	int			index;
	/* Delta between closeness of range to each of the two groups */
	double		delta;
} CommonEntry;

/* Helper macros to place an entry in the left or right group during split */
/* Note direct access to variables v, typcache, left_range, right_range */
#define PLACE_LEFT(range, off)					\
	do {										\
		if (v->spl_nleft > 0)					\
			left_range = range_super_union(typcache, left_range, range); \
		else									\
			left_range = (range);				\
		v->spl_left[v->spl_nleft++] = (off);	\
	} while(0)

#define PLACE_RIGHT(range, off)					\
	do {										\
		if (v->spl_nright > 0)					\
			right_range = range_super_union(typcache, right_range, range); \
		else									\
			right_range = (range);				\
		v->spl_right[v->spl_nright++] = (off);	\
	} while(0)

/* Copy a RangeType datum (hardwires typbyval and typlen for ranges...) */
#define rangeCopy(r) \
	((RangeType *) DatumGetPointer(datumCopy(PointerGetDatum(r), \
											 false, -1)))

static RangeType *range_super_union(TypeCacheEntry *typcache, RangeType *r1,
				  RangeType *r2);
static bool range_gist_consistent_int(TypeCacheEntry *typcache,
						  StrategyNumber strategy, RangeType *key,
						  Datum query);
static bool range_gist_consistent_leaf(TypeCacheEntry *typcache,
						   StrategyNumber strategy, RangeType *key,
						   Datum query);
static void range_gist_fallback_split(TypeCacheEntry *typcache,
						  GistEntryVector *entryvec,
						  GIST_SPLITVEC *v);
static void range_gist_class_split(TypeCacheEntry *typcache,
					   GistEntryVector *entryvec,
					   GIST_SPLITVEC *v,
					   SplitLR *classes_groups);
static void range_gist_single_sorting_split(TypeCacheEntry *typcache,
								GistEntryVector *entryvec,
								GIST_SPLITVEC *v,
								bool use_upper_bound);
static void range_gist_double_sorting_split(TypeCacheEntry *typcache,
								GistEntryVector *entryvec,
								GIST_SPLITVEC *v);
static void range_gist_consider_split(ConsiderSplitContext *context,
						  RangeBound *right_lower, int min_left_count,
						  RangeBound *left_upper, int max_left_count);
static int	get_gist_range_class(RangeType *range);
static int	single_bound_cmp(const void *a, const void *b, void *arg);
static int	interval_cmp_lower(const void *a, const void *b, void *arg);
static int	interval_cmp_upper(const void *a, const void *b, void *arg);
static int	common_entry_cmp(const void *i1, const void *i2);
static float8 call_subtype_diff(TypeCacheEntry *typcache,
				  Datum val1, Datum val2);


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
	TypeCacheEntry *typcache;

	/* All operators served by this function are exact */
	*recheck = false;

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(key));

	if (GIST_LEAF(entry))
		PG_RETURN_BOOL(range_gist_consistent_leaf(typcache, strategy,
												  key, query));
	else
		PG_RETURN_BOOL(range_gist_consistent_int(typcache, strategy,
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

/*
 * GiST page split penalty function.
 *
 * The penalty function has the following goals (in order from most to least
 * important):
 * - Keep normal ranges separate
 * - Avoid broadening the class of the original predicate
 * - Avoid broadening (as determined by subtype_diff) the original predicate
 * - Favor adding ranges to narrower original predicates
 */
Datum
range_gist_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *origentry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *newentry = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *penalty = (float *) PG_GETARG_POINTER(2);
	RangeType  *orig = DatumGetRangeType(origentry->key);
	RangeType  *new = DatumGetRangeType(newentry->key);
	TypeCacheEntry *typcache;
	bool		has_subtype_diff;
	RangeBound	orig_lower,
				new_lower,
				orig_upper,
				new_upper;
	bool		orig_empty,
				new_empty;

	if (RangeTypeGetOid(orig) != RangeTypeGetOid(new))
		elog(ERROR, "range types do not match");

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(orig));

	has_subtype_diff = OidIsValid(typcache->rng_subdiff_finfo.fn_oid);

	range_deserialize(typcache, orig, &orig_lower, &orig_upper, &orig_empty);
	range_deserialize(typcache, new, &new_lower, &new_upper, &new_empty);

	/*
	 * Distinct branches for handling distinct classes of ranges.  Note that
	 * penalty values only need to be commensurate within the same class of
	 * new range.
	 */
	if (new_empty)
	{
		/* Handle insertion of empty range */
		if (orig_empty)
		{
			/*
			 * The best case is to insert it to empty original range.
			 * Insertion here means no broadening of original range. Also
			 * original range is the most narrow.
			 */
			*penalty = 0.0;
		}
		else if (RangeIsOrContainsEmpty(orig))
		{
			/*
			 * The second case is to insert empty range into range which
			 * contains at least one underlying empty range.  There is still
			 * no broadening of original range, but original range is not as
			 * narrow as possible.
			 */
			*penalty = CONTAIN_EMPTY_PENALTY;
		}
		else if (orig_lower.infinite && orig_upper.infinite)
		{
			/*
			 * Original range requires broadening.	(-inf; +inf) is most far
			 * from normal range in this case.
			 */
			*penalty = 2 * CONTAIN_EMPTY_PENALTY;
		}
		else if (orig_lower.infinite || orig_upper.infinite)
		{
			/*
			 * (-inf, x) or (x, +inf) original ranges are closer to normal
			 * ranges, so it's worse to mix it with empty ranges.
			 */
			*penalty = 3 * CONTAIN_EMPTY_PENALTY;
		}
		else
		{
			/*
			 * The least preferred case is broadening of normal range.
			 */
			*penalty = 4 * CONTAIN_EMPTY_PENALTY;
		}
	}
	else if (new_lower.infinite && new_upper.infinite)
	{
		/* Handle insertion of (-inf, +inf) range */
		if (orig_lower.infinite && orig_upper.infinite)
		{
			/*
			 * Best case is inserting to (-inf, +inf) original range.
			 */
			*penalty = 0.0;
		}
		else if (orig_lower.infinite || orig_upper.infinite)
		{
			/*
			 * When original range is (-inf, x) or (x, +inf) it requires
			 * broadening of original range (extension of one bound to
			 * infinity).
			 */
			*penalty = INFINITE_BOUND_PENALTY;
		}
		else
		{
			/*
			 * Insertion to normal original range is least preferred.
			 */
			*penalty = 2 * INFINITE_BOUND_PENALTY;
		}

		if (RangeIsOrContainsEmpty(orig))
		{
			/*
			 * Original range is narrower when it doesn't contain empty
			 * ranges. Add additional penalty otherwise.
			 */
			*penalty += CONTAIN_EMPTY_PENALTY;
		}
	}
	else if (new_lower.infinite)
	{
		/* Handle insertion of (-inf, x) range */
		if (!orig_empty && orig_lower.infinite)
		{
			if (orig_upper.infinite)
			{
				/*
				 * (-inf, +inf) range won't be extended by insertion of (-inf,
				 * x) range. It's a less desirable case than insertion to
				 * (-inf, y) original range without extension, because in that
				 * case original range is narrower. But we can't express that
				 * in single float value.
				 */
				*penalty = 0.0;
			}
			else
			{
				if (range_cmp_bounds(typcache, &new_upper, &orig_upper) > 0)
				{
					/*
					 * Get extension of original range using subtype_diff. Use
					 * constant if subtype_diff unavailable.
					 */
					if (has_subtype_diff)
						*penalty = call_subtype_diff(typcache,
													 new_upper.val,
													 orig_upper.val);
					else
						*penalty = DEFAULT_SUBTYPE_DIFF_PENALTY;
				}
				else
				{
					/* No extension of original range */
					*penalty = 0.0;
				}
			}
		}
		else
		{
			/*
			 * If lower bound of original range is not -inf, then extension of
			 * it is infinity.
			 */
			*penalty = get_float4_infinity();
		}
	}
	else if (new_upper.infinite)
	{
		/* Handle insertion of (x, +inf) range */
		if (!orig_empty && orig_upper.infinite)
		{
			if (orig_lower.infinite)
			{
				/*
				 * (-inf, +inf) range won't be extended by insertion of (x,
				 * +inf) range. It's a less desirable case than insertion to
				 * (y, +inf) original range without extension, because in that
				 * case original range is narrower. But we can't express that
				 * in single float value.
				 */
				*penalty = 0.0;
			}
			else
			{
				if (range_cmp_bounds(typcache, &new_lower, &orig_lower) < 0)
				{
					/*
					 * Get extension of original range using subtype_diff. Use
					 * constant if subtype_diff unavailable.
					 */
					if (has_subtype_diff)
						*penalty = call_subtype_diff(typcache,
													 orig_lower.val,
													 new_lower.val);
					else
						*penalty = DEFAULT_SUBTYPE_DIFF_PENALTY;
				}
				else
				{
					/* No extension of original range */
					*penalty = 0.0;
				}
			}
		}
		else
		{
			/*
			 * If upper bound of original range is not +inf, then extension of
			 * it is infinity.
			 */
			*penalty = get_float4_infinity();
		}
	}
	else
	{
		/* Handle insertion of normal (non-empty, non-infinite) range */
		if (orig_empty || orig_lower.infinite || orig_upper.infinite)
		{
			/*
			 * Avoid mixing normal ranges with infinite and empty ranges.
			 */
			*penalty = get_float4_infinity();
		}
		else
		{
			/*
			 * Calculate extension of original range by calling subtype_diff.
			 * Use constant if subtype_diff unavailable.
			 */
			float8		diff = 0.0;

			if (range_cmp_bounds(typcache, &new_lower, &orig_lower) < 0)
			{
				if (has_subtype_diff)
					diff += call_subtype_diff(typcache,
											  orig_lower.val,
											  new_lower.val);
				else
					diff += DEFAULT_SUBTYPE_DIFF_PENALTY;
			}
			if (range_cmp_bounds(typcache, &new_upper, &orig_upper) > 0)
			{
				if (has_subtype_diff)
					diff += call_subtype_diff(typcache,
											  new_upper.val,
											  orig_upper.val);
				else
					diff += DEFAULT_SUBTYPE_DIFF_PENALTY;
			}
			*penalty = diff;
		}
	}

	PG_RETURN_POINTER(penalty);
}

/*
 * The GiST PickSplit method for ranges
 *
 * Primarily, we try to segregate ranges of different classes.	If splitting
 * ranges of the same class, use the appropriate split method for that class.
 */
Datum
range_gist_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	TypeCacheEntry *typcache;
	OffsetNumber i;
	RangeType  *pred_left;
	int			nbytes;
	OffsetNumber maxoff;
	int			count_in_classes[CLS_COUNT];
	int			j;
	int			non_empty_classes_count = 0;
	int			biggest_class = -1;
	int			biggest_class_count = 0;
	int			total_count;

	/* use first item to look up range type's info */
	pred_left = DatumGetRangeType(entryvec->vector[FirstOffsetNumber].key);
	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(pred_left));

	maxoff = entryvec->n - 1;
	nbytes = (maxoff + 1) * sizeof(OffsetNumber);
	v->spl_left = (OffsetNumber *) palloc(nbytes);
	v->spl_right = (OffsetNumber *) palloc(nbytes);

	/*
	 * Get count distribution of range classes.
	 */
	memset(count_in_classes, 0, sizeof(count_in_classes));
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		RangeType  *range = DatumGetRangeType(entryvec->vector[i].key);

		count_in_classes[get_gist_range_class(range)]++;
	}

	/*
	 * Count non-empty classes and find biggest class.
	 */
	total_count = maxoff;
	for (j = 0; j < CLS_COUNT; j++)
	{
		if (count_in_classes[j] > 0)
		{
			if (count_in_classes[j] > biggest_class_count)
			{
				biggest_class_count = count_in_classes[j];
				biggest_class = j;
			}
			non_empty_classes_count++;
		}
	}

	Assert(non_empty_classes_count > 0);

	if (non_empty_classes_count == 1)
	{
		/* One non-empty class, so split inside class */
		if ((biggest_class & ~CLS_CONTAIN_EMPTY) == CLS_NORMAL)
		{
			/* double sorting split for normal ranges */
			range_gist_double_sorting_split(typcache, entryvec, v);
		}
		else if ((biggest_class & ~CLS_CONTAIN_EMPTY) == CLS_LOWER_INF)
		{
			/* upper bound sorting split for (-inf, x) ranges */
			range_gist_single_sorting_split(typcache, entryvec, v, true);
		}
		else if ((biggest_class & ~CLS_CONTAIN_EMPTY) == CLS_UPPER_INF)
		{
			/* lower bound sorting split for (x, +inf) ranges */
			range_gist_single_sorting_split(typcache, entryvec, v, false);
		}
		else
		{
			/* trivial split for all (-inf, +inf) or all empty ranges */
			range_gist_fallback_split(typcache, entryvec, v);
		}
	}
	else
	{
		/*
		 * Class based split.
		 *
		 * To which side of the split should each class go?  Initialize them
		 * all to go to the left side.
		 */
		SplitLR		classes_groups[CLS_COUNT];

		memset(classes_groups, 0, sizeof(classes_groups));

		if (count_in_classes[CLS_NORMAL] > 0)
		{
			/* separate normal ranges if any */
			classes_groups[CLS_NORMAL] = SPLIT_RIGHT;
		}
		else
		{
			/*----------
			 * Try to split classes in one of two ways:
			 *	1) containing infinities - not containing infinities
			 *	2) containing empty - not containing empty
			 *
			 * Select the way which balances the ranges between left and right
			 * the best. If split in these ways is not possible, there are at
			 * most 3 classes, so just separate biggest class.
			 *----------
			 */
			int			infCount,
						nonInfCount;
			int			emptyCount,
						nonEmptyCount;

			nonInfCount =
				count_in_classes[CLS_NORMAL] +
				count_in_classes[CLS_CONTAIN_EMPTY] +
				count_in_classes[CLS_EMPTY];
			infCount = total_count - nonInfCount;

			nonEmptyCount =
				count_in_classes[CLS_NORMAL] +
				count_in_classes[CLS_LOWER_INF] +
				count_in_classes[CLS_UPPER_INF] +
				count_in_classes[CLS_LOWER_INF | CLS_UPPER_INF];
			emptyCount = total_count - nonEmptyCount;

			if (infCount > 0 && nonInfCount > 0 &&
				(Abs(infCount - nonInfCount) <=
				 Abs(emptyCount - nonEmptyCount)))
			{
				classes_groups[CLS_NORMAL] = SPLIT_RIGHT;
				classes_groups[CLS_CONTAIN_EMPTY] = SPLIT_RIGHT;
				classes_groups[CLS_EMPTY] = SPLIT_RIGHT;
			}
			else if (emptyCount > 0 && nonEmptyCount > 0)
			{
				classes_groups[CLS_NORMAL] = SPLIT_RIGHT;
				classes_groups[CLS_LOWER_INF] = SPLIT_RIGHT;
				classes_groups[CLS_UPPER_INF] = SPLIT_RIGHT;
				classes_groups[CLS_LOWER_INF | CLS_UPPER_INF] = SPLIT_RIGHT;
			}
			else
			{
				/*
				 * Either total_count == emptyCount or total_count ==
				 * infCount.
				 */
				classes_groups[biggest_class] = SPLIT_RIGHT;
			}
		}

		range_gist_class_split(typcache, entryvec, v, classes_groups);
	}

	PG_RETURN_POINTER(v);
}

/* equality comparator for GiST */
Datum
range_gist_same(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE(0);
	RangeType  *r2 = PG_GETARG_RANGE(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	/*
	 * range_eq will ignore the RANGE_CONTAIN_EMPTY flag, so we have to check
	 * that for ourselves.	More generally, if the entries have been properly
	 * normalized, then unequal flags bytes must mean unequal ranges ... so
	 * let's just test all the flag bits at once.
	 */
	if (range_get_flags(r1) != range_get_flags(r2))
		*result = false;
	else
	{
		TypeCacheEntry *typcache;

		typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

		*result = range_eq_internal(typcache, r1, r2);
	}

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
 * This differs from regular range_union in two critical ways:
 * 1. It won't throw an error for non-adjacent r1 and r2, but just absorb
 * the intervening values into the result range.
 * 2. We track whether any empty range has been union'd into the result,
 * so that contained_by searches can be indexed.  Note that this means
 * that *all* unions formed within the GiST index must go through here.
 */
static RangeType *
range_super_union(TypeCacheEntry *typcache, RangeType *r1, RangeType *r2)
{
	RangeType  *result;
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;
	char		flags1,
				flags2;
	RangeBound *result_lower;
	RangeBound *result_upper;

	range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
	range_deserialize(typcache, r2, &lower2, &upper2, &empty2);
	flags1 = range_get_flags(r1);
	flags2 = range_get_flags(r2);

	if (empty1)
	{
		/* We can return r2 as-is if it already is or contains empty */
		if (flags2 & (RANGE_EMPTY | RANGE_CONTAIN_EMPTY))
			return r2;
		/* Else we'd better copy it (modify-in-place isn't safe) */
		r2 = rangeCopy(r2);
		range_set_contain_empty(r2);
		return r2;
	}
	if (empty2)
	{
		/* We can return r1 as-is if it already is or contains empty */
		if (flags1 & (RANGE_EMPTY | RANGE_CONTAIN_EMPTY))
			return r1;
		/* Else we'd better copy it (modify-in-place isn't safe) */
		r1 = rangeCopy(r1);
		range_set_contain_empty(r1);
		return r1;
	}

	if (range_cmp_bounds(typcache, &lower1, &lower2) <= 0)
		result_lower = &lower1;
	else
		result_lower = &lower2;

	if (range_cmp_bounds(typcache, &upper1, &upper2) >= 0)
		result_upper = &upper1;
	else
		result_upper = &upper2;

	/* optimization to avoid constructing a new range */
	if (result_lower == &lower1 && result_upper == &upper1 &&
		((flags1 & RANGE_CONTAIN_EMPTY) || !(flags2 & RANGE_CONTAIN_EMPTY)))
		return r1;
	if (result_lower == &lower2 && result_upper == &upper2 &&
		((flags2 & RANGE_CONTAIN_EMPTY) || !(flags1 & RANGE_CONTAIN_EMPTY)))
		return r2;

	result = make_range(typcache, result_lower, result_upper, false);

	if ((flags1 & RANGE_CONTAIN_EMPTY) || (flags2 & RANGE_CONTAIN_EMPTY))
		range_set_contain_empty(result);

	return result;
}

/*
 * GiST consistent test on an index internal page
 */
static bool
range_gist_consistent_int(TypeCacheEntry *typcache, StrategyNumber strategy,
						  RangeType *key, Datum query)
{
	switch (strategy)
	{
		case RANGESTRAT_BEFORE:
			if (RangeIsEmpty(key) || RangeIsEmpty(DatumGetRangeType(query)))
				return false;
			return (!range_overright_internal(typcache, key,
											  DatumGetRangeType(query)));
		case RANGESTRAT_OVERLEFT:
			if (RangeIsEmpty(key) || RangeIsEmpty(DatumGetRangeType(query)))
				return false;
			return (!range_after_internal(typcache, key,
										  DatumGetRangeType(query)));
		case RANGESTRAT_OVERLAPS:
			return range_overlaps_internal(typcache, key,
										   DatumGetRangeType(query));
		case RANGESTRAT_OVERRIGHT:
			if (RangeIsEmpty(key) || RangeIsEmpty(DatumGetRangeType(query)))
				return false;
			return (!range_before_internal(typcache, key,
										   DatumGetRangeType(query)));
		case RANGESTRAT_AFTER:
			if (RangeIsEmpty(key) || RangeIsEmpty(DatumGetRangeType(query)))
				return false;
			return (!range_overleft_internal(typcache, key,
											 DatumGetRangeType(query)));
		case RANGESTRAT_ADJACENT:
			if (RangeIsEmpty(key) || RangeIsEmpty(DatumGetRangeType(query)))
				return false;
			if (range_adjacent_internal(typcache, key,
										DatumGetRangeType(query)))
				return true;
			return range_overlaps_internal(typcache, key,
										   DatumGetRangeType(query));
		case RANGESTRAT_CONTAINS:
			return range_contains_internal(typcache, key,
										   DatumGetRangeType(query));
		case RANGESTRAT_CONTAINED_BY:

			/*
			 * Empty ranges are contained by anything, so if key is or
			 * contains any empty ranges, we must descend into it.	Otherwise,
			 * descend only if key overlaps the query.
			 */
			if (RangeIsOrContainsEmpty(key))
				return true;
			return range_overlaps_internal(typcache, key,
										   DatumGetRangeType(query));
		case RANGESTRAT_CONTAINS_ELEM:
			return range_contains_elem_internal(typcache, key, query);
		case RANGESTRAT_EQ:

			/*
			 * If query is empty, descend only if the key is or contains any
			 * empty ranges.  Otherwise, descend if key contains query.
			 */
			if (RangeIsEmpty(DatumGetRangeType(query)))
				return RangeIsOrContainsEmpty(key);
			return range_contains_internal(typcache, key,
										   DatumGetRangeType(query));
		default:
			elog(ERROR, "unrecognized range strategy: %d", strategy);
			return false;		/* keep compiler quiet */
	}
}

/*
 * GiST consistent test on an index leaf page
 */
static bool
range_gist_consistent_leaf(TypeCacheEntry *typcache, StrategyNumber strategy,
						   RangeType *key, Datum query)
{
	switch (strategy)
	{
		case RANGESTRAT_BEFORE:
			return range_before_internal(typcache, key,
										 DatumGetRangeType(query));
		case RANGESTRAT_OVERLEFT:
			return range_overleft_internal(typcache, key,
										   DatumGetRangeType(query));
		case RANGESTRAT_OVERLAPS:
			return range_overlaps_internal(typcache, key,
										   DatumGetRangeType(query));
		case RANGESTRAT_OVERRIGHT:
			return range_overright_internal(typcache, key,
											DatumGetRangeType(query));
		case RANGESTRAT_AFTER:
			return range_after_internal(typcache, key,
										DatumGetRangeType(query));
		case RANGESTRAT_ADJACENT:
			return range_adjacent_internal(typcache, key,
										   DatumGetRangeType(query));
		case RANGESTRAT_CONTAINS:
			return range_contains_internal(typcache, key,
										   DatumGetRangeType(query));
		case RANGESTRAT_CONTAINED_BY:
			return range_contained_by_internal(typcache, key,
											   DatumGetRangeType(query));
		case RANGESTRAT_CONTAINS_ELEM:
			return range_contains_elem_internal(typcache, key, query);
		case RANGESTRAT_EQ:
			return range_eq_internal(typcache, key, DatumGetRangeType(query));
		default:
			elog(ERROR, "unrecognized range strategy: %d", strategy);
			return false;		/* keep compiler quiet */
	}
}

/*
 * Trivial split: half of entries will be placed on one page
 * and the other half on the other page.
 */
static void
range_gist_fallback_split(TypeCacheEntry *typcache,
						  GistEntryVector *entryvec,
						  GIST_SPLITVEC *v)
{
	RangeType  *left_range = NULL;
	RangeType  *right_range = NULL;
	OffsetNumber i,
				maxoff,
				split_idx;

	maxoff = entryvec->n - 1;
	/* Split entries before this to left page, after to right: */
	split_idx = (maxoff - FirstOffsetNumber) / 2 + FirstOffsetNumber;

	v->spl_nleft = 0;
	v->spl_nright = 0;
	for (i = FirstOffsetNumber; i <= maxoff; i++)
	{
		RangeType  *range = DatumGetRangeType(entryvec->vector[i].key);

		if (i < split_idx)
			PLACE_LEFT(range, i);
		else
			PLACE_RIGHT(range, i);
	}

	v->spl_ldatum = RangeTypeGetDatum(left_range);
	v->spl_rdatum = RangeTypeGetDatum(right_range);
}

/*
 * Split based on classes of ranges.
 *
 * See get_gist_range_class for class definitions.
 * classes_groups is an array of length CLS_COUNT indicating the side of the
 * split to which each class should go.
 */
static void
range_gist_class_split(TypeCacheEntry *typcache,
					   GistEntryVector *entryvec,
					   GIST_SPLITVEC *v,
					   SplitLR *classes_groups)
{
	RangeType  *left_range = NULL;
	RangeType  *right_range = NULL;
	OffsetNumber i,
				maxoff;

	maxoff = entryvec->n - 1;

	v->spl_nleft = 0;
	v->spl_nright = 0;
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		RangeType  *range = DatumGetRangeType(entryvec->vector[i].key);
		int			class;

		/* Get class of range */
		class = get_gist_range_class(range);

		/* Place range to appropriate page */
		if (classes_groups[class] == SPLIT_LEFT)
			PLACE_LEFT(range, i);
		else
		{
			Assert(classes_groups[class] == SPLIT_RIGHT);
			PLACE_RIGHT(range, i);
		}
	}

	v->spl_ldatum = RangeTypeGetDatum(left_range);
	v->spl_rdatum = RangeTypeGetDatum(right_range);
}

/*
 * Sorting based split. First half of entries according to the sort will be
 * placed to one page, and second half of entries will be placed to other
 * page. use_upper_bound parameter indicates whether to use upper or lower
 * bound for sorting.
 */
static void
range_gist_single_sorting_split(TypeCacheEntry *typcache,
								GistEntryVector *entryvec,
								GIST_SPLITVEC *v,
								bool use_upper_bound)
{
	SingleBoundSortItem *sortItems;
	RangeType  *left_range = NULL;
	RangeType  *right_range = NULL;
	OffsetNumber i,
				maxoff,
				split_idx;

	maxoff = entryvec->n - 1;

	sortItems = (SingleBoundSortItem *)
		palloc(maxoff * sizeof(SingleBoundSortItem));

	/*
	 * Prepare auxiliary array and sort the values.
	 */
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		RangeType  *range = DatumGetRangeType(entryvec->vector[i].key);
		RangeBound	bound2;
		bool		empty;

		sortItems[i - 1].index = i;
		/* Put appropriate bound into array */
		if (use_upper_bound)
			range_deserialize(typcache, range, &bound2,
							  &sortItems[i - 1].bound, &empty);
		else
			range_deserialize(typcache, range, &sortItems[i - 1].bound,
							  &bound2, &empty);
		Assert(!empty);
	}

	qsort_arg(sortItems, maxoff, sizeof(SingleBoundSortItem),
			  single_bound_cmp, typcache);

	split_idx = maxoff / 2;

	v->spl_nleft = 0;
	v->spl_nright = 0;

	for (i = 0; i < maxoff; i++)
	{
		int			idx = sortItems[i].index;
		RangeType  *range = DatumGetRangeType(entryvec->vector[idx].key);

		if (i < split_idx)
			PLACE_LEFT(range, idx);
		else
			PLACE_RIGHT(range, idx);
	}

	v->spl_ldatum = RangeTypeGetDatum(left_range);
	v->spl_rdatum = RangeTypeGetDatum(right_range);
}

/*
 * Double sorting split algorithm.
 *
 * The algorithm considers dividing ranges into two groups. The first (left)
 * group contains general left bound. The second (right) group contains
 * general right bound. The challenge is to find upper bound of left group
 * and lower bound of right group so that overlap of groups is minimal and
 * ratio of distribution is acceptable. Algorithm finds for each lower bound of
 * right group minimal upper bound of left group, and for each upper bound of
 * left group maximal lower bound of right group. For each found pair
 * range_gist_consider_split considers replacement of currently selected
 * split with the new one.
 *
 * After that, all the entries are divided into three groups:
 * 1) Entries which should be placed to the left group
 * 2) Entries which should be placed to the right group
 * 3) "Common entries" which can be placed to either group without affecting
 *	  amount of overlap.
 *
 * The common ranges are distributed by difference of distance from lower
 * bound of common range to lower bound of right group and distance from upper
 * bound of common range to upper bound of left group.
 *
 * For details see:
 * "A new double sorting-based node splitting algorithm for R-tree",
 * A. Korotkov
 * http://syrcose.ispras.ru/2011/files/SYRCoSE2011_Proceedings.pdf#page=36
 */
static void
range_gist_double_sorting_split(TypeCacheEntry *typcache,
								GistEntryVector *entryvec,
								GIST_SPLITVEC *v)
{
	ConsiderSplitContext context;
	OffsetNumber i,
				maxoff;
	RangeType  *range,
			   *left_range = NULL,
			   *right_range = NULL;
	int			common_entries_count;
	NonEmptyRange *by_lower,
			   *by_upper;
	CommonEntry *common_entries;
	int			nentries,
				i1,
				i2;
	RangeBound *right_lower,
			   *left_upper;

	memset(&context, 0, sizeof(ConsiderSplitContext));
	context.typcache = typcache;
	context.has_subtype_diff = OidIsValid(typcache->rng_subdiff_finfo.fn_oid);

	maxoff = entryvec->n - 1;
	nentries = context.entries_count = maxoff - FirstOffsetNumber + 1;
	context.first = true;

	/* Allocate arrays for sorted range bounds */
	by_lower = (NonEmptyRange *) palloc(nentries * sizeof(NonEmptyRange));
	by_upper = (NonEmptyRange *) palloc(nentries * sizeof(NonEmptyRange));

	/* Fill arrays of bounds */
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		RangeType  *range = DatumGetRangeType(entryvec->vector[i].key);
		bool		empty;

		range_deserialize(typcache, range,
						  &by_lower[i - FirstOffsetNumber].lower,
						  &by_lower[i - FirstOffsetNumber].upper,
						  &empty);
		Assert(!empty);
	}

	/*
	 * Make two arrays of range bounds: one sorted by lower bound and another
	 * sorted by upper bound.
	 */
	memcpy(by_upper, by_lower, nentries * sizeof(NonEmptyRange));
	qsort_arg(by_lower, nentries, sizeof(NonEmptyRange),
			  interval_cmp_lower, typcache);
	qsort_arg(by_upper, nentries, sizeof(NonEmptyRange),
			  interval_cmp_upper, typcache);

	/*----------
	 * The goal is to form a left and right range, so that every entry
	 * range is contained by either left or right interval (or both).
	 *
	 * For example, with the ranges (0,1), (1,3), (2,3), (2,4):
	 *
	 * 0 1 2 3 4
	 * +-+
	 *	 +---+
	 *	   +-+
	 *	   +---+
	 *
	 * The left and right ranges are of the form (0,a) and (b,4).
	 * We first consider splits where b is the lower bound of an entry.
	 * We iterate through all entries, and for each b, calculate the
	 * smallest possible a. Then we consider splits where a is the
	 * upper bound of an entry, and for each a, calculate the greatest
	 * possible b.
	 *
	 * In the above example, the first loop would consider splits:
	 * b=0: (0,1)-(0,4)
	 * b=1: (0,1)-(1,4)
	 * b=2: (0,3)-(2,4)
	 *
	 * And the second loop:
	 * a=1: (0,1)-(1,4)
	 * a=3: (0,3)-(2,4)
	 * a=4: (0,4)-(2,4)
	 *----------
	 */

	/*
	 * Iterate over lower bound of right group, finding smallest possible
	 * upper bound of left group.
	 */
	i1 = 0;
	i2 = 0;
	right_lower = &by_lower[i1].lower;
	left_upper = &by_upper[i2].lower;
	while (true)
	{
		/*
		 * Find next lower bound of right group.
		 */
		while (i1 < nentries &&
			   range_cmp_bounds(typcache, right_lower,
								&by_lower[i1].lower) == 0)
		{
			if (range_cmp_bounds(typcache, &by_lower[i1].upper,
								 left_upper) > 0)
				left_upper = &by_lower[i1].upper;
			i1++;
		}
		if (i1 >= nentries)
			break;
		right_lower = &by_lower[i1].lower;

		/*
		 * Find count of ranges which anyway should be placed to the left
		 * group.
		 */
		while (i2 < nentries &&
			   range_cmp_bounds(typcache, &by_upper[i2].upper,
								left_upper) <= 0)
			i2++;

		/*
		 * Consider found split to see if it's better than what we had.
		 */
		range_gist_consider_split(&context, right_lower, i1, left_upper, i2);
	}

	/*
	 * Iterate over upper bound of left group finding greatest possible lower
	 * bound of right group.
	 */
	i1 = nentries - 1;
	i2 = nentries - 1;
	right_lower = &by_lower[i1].upper;
	left_upper = &by_upper[i2].upper;
	while (true)
	{
		/*
		 * Find next upper bound of left group.
		 */
		while (i2 >= 0 &&
			   range_cmp_bounds(typcache, left_upper,
								&by_upper[i2].upper) == 0)
		{
			if (range_cmp_bounds(typcache, &by_upper[i2].lower,
								 right_lower) < 0)
				right_lower = &by_upper[i2].lower;
			i2--;
		}
		if (i2 < 0)
			break;
		left_upper = &by_upper[i2].upper;

		/*
		 * Find count of intervals which anyway should be placed to the right
		 * group.
		 */
		while (i1 >= 0 &&
			   range_cmp_bounds(typcache, &by_lower[i1].lower,
								right_lower) >= 0)
			i1--;

		/*
		 * Consider found split to see if it's better than what we had.
		 */
		range_gist_consider_split(&context, right_lower, i1 + 1,
								  left_upper, i2 + 1);
	}

	/*
	 * If we failed to find any acceptable splits, use trivial split.
	 */
	if (context.first)
	{
		range_gist_fallback_split(typcache, entryvec, v);
		return;
	}

	/*
	 * Ok, we have now selected bounds of the groups. Now we have to
	 * distribute entries themselves. At first we distribute entries which can
	 * be placed unambiguously and collect "common entries" to array.
	 */

	/* Allocate vectors for results */
	v->spl_left = (OffsetNumber *) palloc(nentries * sizeof(OffsetNumber));
	v->spl_right = (OffsetNumber *) palloc(nentries * sizeof(OffsetNumber));
	v->spl_nleft = 0;
	v->spl_nright = 0;

	/*
	 * Allocate an array for "common entries" - entries which can be placed to
	 * either group without affecting overlap along selected axis.
	 */
	common_entries_count = 0;
	common_entries = (CommonEntry *) palloc(nentries * sizeof(CommonEntry));

	/*
	 * Distribute entries which can be distributed unambiguously, and collect
	 * common entries.
	 */
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		RangeBound	lower,
					upper;
		bool		empty;

		/*
		 * Get upper and lower bounds along selected axis.
		 */
		range = DatumGetRangeType(entryvec->vector[i].key);

		range_deserialize(typcache, range, &lower, &upper, &empty);

		if (range_cmp_bounds(typcache, &upper, context.left_upper) <= 0)
		{
			/* Fits in the left group */
			if (range_cmp_bounds(typcache, &lower, context.right_lower) >= 0)
			{
				/* Fits also in the right group, so "common entry" */
				common_entries[common_entries_count].index = i;
				if (context.has_subtype_diff)
				{
					/*
					 * delta = (lower - context.right_lower) -
					 * (context.left_upper - upper)
					 */
					common_entries[common_entries_count].delta =
						call_subtype_diff(typcache,
										  lower.val,
										  context.right_lower->val) -
						call_subtype_diff(typcache,
										  context.left_upper->val,
										  upper.val);
				}
				else
				{
					/* Without subtype_diff, take all deltas as zero */
					common_entries[common_entries_count].delta = 0;
				}
				common_entries_count++;
			}
			else
			{
				/* Doesn't fit to the right group, so join to the left group */
				PLACE_LEFT(range, i);
			}
		}
		else
		{
			/*
			 * Each entry should fit on either left or right group. Since this
			 * entry didn't fit in the left group, it better fit in the right
			 * group.
			 */
			Assert(range_cmp_bounds(typcache, &lower,
									context.right_lower) >= 0);
			PLACE_RIGHT(range, i);
		}
	}

	/*
	 * Distribute "common entries", if any.
	 */
	if (common_entries_count > 0)
	{
		/*
		 * Sort "common entries" by calculated deltas in order to distribute
		 * the most ambiguous entries first.
		 */
		qsort(common_entries, common_entries_count, sizeof(CommonEntry),
			  common_entry_cmp);

		/*
		 * Distribute "common entries" between groups according to sorting.
		 */
		for (i = 0; i < common_entries_count; i++)
		{
			int			idx = common_entries[i].index;

			range = DatumGetRangeType(entryvec->vector[idx].key);

			/*
			 * Check if we have to place this entry in either group to achieve
			 * LIMIT_RATIO.
			 */
			if (i < context.common_left)
				PLACE_LEFT(range, idx);
			else
				PLACE_RIGHT(range, idx);
		}
	}

	v->spl_ldatum = PointerGetDatum(left_range);
	v->spl_rdatum = PointerGetDatum(right_range);
}

/*
 * Consider replacement of currently selected split with a better one
 * during range_gist_double_sorting_split.
 */
static void
range_gist_consider_split(ConsiderSplitContext *context,
						  RangeBound *right_lower, int min_left_count,
						  RangeBound *left_upper, int max_left_count)
{
	int			left_count,
				right_count;
	float4		ratio,
				overlap;

	/*
	 * Calculate entries distribution ratio assuming most uniform distribution
	 * of common entries.
	 */
	if (min_left_count >= (context->entries_count + 1) / 2)
		left_count = min_left_count;
	else if (max_left_count <= context->entries_count / 2)
		left_count = max_left_count;
	else
		left_count = context->entries_count / 2;
	right_count = context->entries_count - left_count;

	/*
	 * Ratio of split: quotient between size of smaller group and total
	 * entries count.  This is necessarily 0.5 or less; if it's less than
	 * LIMIT_RATIO then we will never accept the new split.
	 */
	ratio = ((float4) Min(left_count, right_count)) /
		((float4) context->entries_count);

	if (ratio > LIMIT_RATIO)
	{
		bool		selectthis = false;

		/*
		 * The ratio is acceptable, so compare current split with previously
		 * selected one. We search for minimal overlap (allowing negative
		 * values) and minimal ratio secondarily.  If subtype_diff is
		 * available, it's used for overlap measure.  Without subtype_diff we
		 * use number of "common entries" as an overlap measure.
		 */
		if (context->has_subtype_diff)
			overlap = call_subtype_diff(context->typcache,
										left_upper->val,
										right_lower->val);
		else
			overlap = max_left_count - min_left_count;

		/* If there is no previous selection, select this split */
		if (context->first)
			selectthis = true;
		else
		{
			/*
			 * Choose the new split if it has a smaller overlap, or same
			 * overlap but better ratio.
			 */
			if (overlap < context->overlap ||
				(overlap == context->overlap && ratio > context->ratio))
				selectthis = true;
		}

		if (selectthis)
		{
			/* save information about selected split */
			context->first = false;
			context->ratio = ratio;
			context->overlap = overlap;
			context->right_lower = right_lower;
			context->left_upper = left_upper;
			context->common_left = max_left_count - left_count;
			context->common_right = left_count - min_left_count;
		}
	}
}

/*
 * Find class number for range.
 *
 * The class number is a valid combination of the properties of the
 * range.  Note: the highest possible number is 8, because CLS_EMPTY
 * can't be combined with anything else.
 */
static int
get_gist_range_class(RangeType *range)
{
	int			classNumber;
	char		flags;

	flags = range_get_flags(range);
	if (flags & RANGE_EMPTY)
	{
		classNumber = CLS_EMPTY;
	}
	else
	{
		classNumber = 0;
		if (flags & RANGE_LB_INF)
			classNumber |= CLS_LOWER_INF;
		if (flags & RANGE_UB_INF)
			classNumber |= CLS_UPPER_INF;
		if (flags & RANGE_CONTAIN_EMPTY)
			classNumber |= CLS_CONTAIN_EMPTY;
	}
	return classNumber;
}

/*
 * Comparison function for range_gist_single_sorting_split.
 */
static int
single_bound_cmp(const void *a, const void *b, void *arg)
{
	SingleBoundSortItem *i1 = (SingleBoundSortItem *) a;
	SingleBoundSortItem *i2 = (SingleBoundSortItem *) b;
	TypeCacheEntry *typcache = (TypeCacheEntry *) arg;

	return range_cmp_bounds(typcache, &i1->bound, &i2->bound);
}

/*
 * Compare NonEmptyRanges by lower bound.
 */
static int
interval_cmp_lower(const void *a, const void *b, void *arg)
{
	NonEmptyRange *i1 = (NonEmptyRange *) a;
	NonEmptyRange *i2 = (NonEmptyRange *) b;
	TypeCacheEntry *typcache = (TypeCacheEntry *) arg;

	return range_cmp_bounds(typcache, &i1->lower, &i2->lower);
}

/*
 * Compare NonEmptyRanges by upper bound.
 */
static int
interval_cmp_upper(const void *a, const void *b, void *arg)
{
	NonEmptyRange *i1 = (NonEmptyRange *) a;
	NonEmptyRange *i2 = (NonEmptyRange *) b;
	TypeCacheEntry *typcache = (TypeCacheEntry *) arg;

	return range_cmp_bounds(typcache, &i1->upper, &i2->upper);
}

/*
 * Compare CommonEntrys by their deltas.
 */
static int
common_entry_cmp(const void *i1, const void *i2)
{
	double		delta1 = ((CommonEntry *) i1)->delta;
	double		delta2 = ((CommonEntry *) i2)->delta;

	if (delta1 < delta2)
		return -1;
	else if (delta1 > delta2)
		return 1;
	else
		return 0;
}

/*
 * Convenience function to invoke type-specific subtype_diff function.
 * Caller must have already checked that there is one for the range type.
 */
static float8
call_subtype_diff(TypeCacheEntry *typcache, Datum val1, Datum val2)
{
	float8		value;

	value = DatumGetFloat8(FunctionCall2Coll(&typcache->rng_subdiff_finfo,
											 typcache->rng_collation,
											 val1, val2));
	/* Cope with buggy subtype_diff function by returning zero */
	if (value >= 0.0)
		return value;
	return 0.0;
}
