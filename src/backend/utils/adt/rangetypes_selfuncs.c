/*-------------------------------------------------------------------------
 *
 * rangetypes_selfuncs.c
 *	  Functions for selectivity estimation of range operators
 *
 * Estimates are based on histograms of lower and upper bounds, and the
 * fraction of empty ranges.
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/rangetypes_selfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_statistic.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rangetypes.h"
#include "utils/selfuncs.h"
#include "utils/typcache.h"

static double calc_rangesel(TypeCacheEntry *typcache, VariableStatData *vardata,
			  RangeType *constval, Oid operator);
static double default_range_selectivity(Oid operator);
static double calc_hist_selectivity(TypeCacheEntry *typcache,
					  VariableStatData *vardata, RangeType *constval,
					  Oid operator);
static double calc_hist_selectivity_scalar(TypeCacheEntry *typcache,
							 RangeBound *constbound,
							 RangeBound *hist, int hist_nvalues,
							 bool equal);
static int rbound_bsearch(TypeCacheEntry *typcache, RangeBound *value,
			   RangeBound *hist, int hist_length, bool equal);
static float8 get_position(TypeCacheEntry *typcache, RangeBound *value,
			 RangeBound *hist1, RangeBound *hist2);
static float8 get_len_position(double value, double hist1, double hist2);
static float8 get_distance(TypeCacheEntry *typcache, RangeBound *bound1,
			 RangeBound *bound2);
static int length_hist_bsearch(Datum *length_hist_values,
					int length_hist_nvalues, double value, bool equal);
static double calc_length_hist_frac(Datum *length_hist_values,
		int length_hist_nvalues, double length1, double length2, bool equal);
static double calc_hist_selectivity_contained(TypeCacheEntry *typcache,
								RangeBound *lower, RangeBound *upper,
								RangeBound *hist_lower, int hist_nvalues,
						 Datum *length_hist_values, int length_hist_nvalues);
static double calc_hist_selectivity_contains(TypeCacheEntry *typcache,
							   RangeBound *lower, RangeBound *upper,
							   RangeBound *hist_lower, int hist_nvalues,
						 Datum *length_hist_values, int length_hist_nvalues);

/*
 * Returns a default selectivity estimate for given operator, when we don't
 * have statistics or cannot use them for some reason.
 */
static double
default_range_selectivity(Oid operator)
{
	switch (operator)
	{
		case OID_RANGE_OVERLAP_OP:
			return 0.01;

		case OID_RANGE_CONTAINS_OP:
		case OID_RANGE_CONTAINED_OP:
			return 0.005;

		case OID_RANGE_CONTAINS_ELEM_OP:
		case OID_RANGE_ELEM_CONTAINED_OP:

			/*
			 * "range @> elem" is more or less identical to a scalar
			 * inequality "A >= b AND A <= c".
			 */
			return DEFAULT_RANGE_INEQ_SEL;

		case OID_RANGE_LESS_OP:
		case OID_RANGE_LESS_EQUAL_OP:
		case OID_RANGE_GREATER_OP:
		case OID_RANGE_GREATER_EQUAL_OP:
		case OID_RANGE_LEFT_OP:
		case OID_RANGE_RIGHT_OP:
		case OID_RANGE_OVERLAPS_LEFT_OP:
		case OID_RANGE_OVERLAPS_RIGHT_OP:
			/* these are similar to regular scalar inequalities */
			return DEFAULT_INEQ_SEL;

		default:
			/* all range operators should be handled above, but just in case */
			return 0.01;
	}
}

/*
 * rangesel -- restriction selectivity for range operators
 */
Datum
rangesel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;
	Selectivity selec;
	TypeCacheEntry *typcache = NULL;
	RangeType  *constrange = NULL;

	/*
	 * If expression is not (variable op something) or (something op
	 * variable), then punt and return a default estimate.
	 */
	if (!get_restriction_variable(root, args, varRelid,
								  &vardata, &other, &varonleft))
		PG_RETURN_FLOAT8(default_range_selectivity(operator));

	/*
	 * Can't do anything useful if the something is not a constant, either.
	 */
	if (!IsA(other, Const))
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(default_range_selectivity(operator));
	}

	/*
	 * All the range operators are strict, so we can cope with a NULL constant
	 * right away.
	 */
	if (((Const *) other)->constisnull)
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(0.0);
	}

	/*
	 * If var is on the right, commute the operator, so that we can assume the
	 * var is on the left in what follows.
	 */
	if (!varonleft)
	{
		/* we have other Op var, commute to make var Op other */
		operator = get_commutator(operator);
		if (!operator)
		{
			/* Use default selectivity (should we raise an error instead?) */
			ReleaseVariableStats(vardata);
			PG_RETURN_FLOAT8(default_range_selectivity(operator));
		}
	}

	/*
	 * OK, there's a Var and a Const we're dealing with here.  We need the
	 * Const to be of same range type as the column, else we can't do anything
	 * useful. (Such cases will likely fail at runtime, but here we'd rather
	 * just return a default estimate.)
	 *
	 * If the operator is "range @> element", the constant should be of the
	 * element type of the range column. Convert it to a range that includes
	 * only that single point, so that we don't need special handling for that
	 * in what follows.
	 */
	if (operator == OID_RANGE_CONTAINS_ELEM_OP)
	{
		typcache = range_get_typcache(fcinfo, vardata.vartype);

		if (((Const *) other)->consttype == typcache->rngelemtype->type_id)
		{
			RangeBound	lower,
						upper;

			lower.inclusive = true;
			lower.val = ((Const *) other)->constvalue;
			lower.infinite = false;
			lower.lower = true;
			upper.inclusive = true;
			upper.val = ((Const *) other)->constvalue;
			upper.infinite = false;
			upper.lower = false;
			constrange = range_serialize(typcache, &lower, &upper, false);
		}
	}
	else if (operator == OID_RANGE_ELEM_CONTAINED_OP)
	{
		/*
		 * Here, the Var is the elem, not the range.  For now we just punt and
		 * return the default estimate.  In future we could disassemble the
		 * range constant and apply scalarineqsel ...
		 */
	}
	else if (((Const *) other)->consttype == vardata.vartype)
	{
		/* Both sides are the same range type */
		typcache = range_get_typcache(fcinfo, vardata.vartype);

		constrange = DatumGetRangeType(((Const *) other)->constvalue);
	}

	/*
	 * If we got a valid constant on one side of the operator, proceed to
	 * estimate using statistics. Otherwise punt and return a default constant
	 * estimate.  Note that calc_rangesel need not handle
	 * OID_RANGE_ELEM_CONTAINED_OP.
	 */
	if (constrange)
		selec = calc_rangesel(typcache, &vardata, constrange, operator);
	else
		selec = default_range_selectivity(operator);

	ReleaseVariableStats(vardata);

	CLAMP_PROBABILITY(selec);

	PG_RETURN_FLOAT8((float8) selec);
}

static double
calc_rangesel(TypeCacheEntry *typcache, VariableStatData *vardata,
			  RangeType *constval, Oid operator)
{
	double		hist_selec;
	double		selec;
	float4		empty_frac,
				null_frac;

	/*
	 * First look up the fraction of NULLs and empty ranges from pg_statistic.
	 */
	if (HeapTupleIsValid(vardata->statsTuple))
	{
		Form_pg_statistic stats;
		float4	   *numbers;
		int			nnumbers;

		stats = (Form_pg_statistic) GETSTRUCT(vardata->statsTuple);
		null_frac = stats->stanullfrac;

		/* Try to get fraction of empty ranges */
		if (get_attstatsslot(vardata->statsTuple,
							 vardata->atttype, vardata->atttypmod,
						   STATISTIC_KIND_RANGE_LENGTH_HISTOGRAM, InvalidOid,
							 NULL,
							 NULL, NULL,
							 &numbers, &nnumbers))
		{
			if (nnumbers != 1)
				elog(ERROR, "invalid empty fraction statistic");		/* shouldn't happen */
			empty_frac = numbers[0];
		}
		else
		{
			/* No empty fraction statistic. Assume no empty ranges. */
			empty_frac = 0.0;
		}
	}
	else
	{
		/*
		 * No stats are available. Follow through the calculations below
		 * anyway, assuming no NULLs and no empty ranges. This still allows us
		 * to give a better-than-nothing estimate based on whether the
		 * constant is an empty range or not.
		 */
		null_frac = 0.0;
		empty_frac = 0.0;
	}

	if (RangeIsEmpty(constval))
	{
		/*
		 * An empty range matches all ranges, all empty ranges, or nothing,
		 * depending on the operator
		 */
		switch (operator)
		{
				/* these return false if either argument is empty */
			case OID_RANGE_OVERLAP_OP:
			case OID_RANGE_OVERLAPS_LEFT_OP:
			case OID_RANGE_OVERLAPS_RIGHT_OP:
			case OID_RANGE_LEFT_OP:
			case OID_RANGE_RIGHT_OP:
				/* nothing is less than an empty range */
			case OID_RANGE_LESS_OP:
				selec = 0.0;
				break;

				/* only empty ranges can be contained by an empty range */
			case OID_RANGE_CONTAINED_OP:
				/* only empty ranges are <= an empty range */
			case OID_RANGE_LESS_EQUAL_OP:
				selec = empty_frac;
				break;

				/* everything contains an empty range */
			case OID_RANGE_CONTAINS_OP:
				/* everything is >= an empty range */
			case OID_RANGE_GREATER_EQUAL_OP:
				selec = 1.0;
				break;

				/* all non-empty ranges are > an empty range */
			case OID_RANGE_GREATER_OP:
				selec = 1.0 - empty_frac;
				break;

				/* an element cannot be empty */
			case OID_RANGE_CONTAINS_ELEM_OP:
			default:
				elog(ERROR, "unexpected operator %u", operator);
				selec = 0.0;	/* keep compiler quiet */
				break;
		}
	}
	else
	{
		/*
		 * Calculate selectivity using bound histograms. If that fails for
		 * some reason, e.g no histogram in pg_statistic, use the default
		 * constant estimate for the fraction of non-empty values. This is
		 * still somewhat better than just returning the default estimate,
		 * because this still takes into account the fraction of empty and
		 * NULL tuples, if we had statistics for them.
		 */
		hist_selec = calc_hist_selectivity(typcache, vardata, constval,
										   operator);
		if (hist_selec < 0.0)
			hist_selec = default_range_selectivity(operator);

		/*
		 * Now merge the results for the empty ranges and histogram
		 * calculations, realizing that the histogram covers only the
		 * non-null, non-empty values.
		 */
		if (operator == OID_RANGE_CONTAINED_OP)
		{
			/* empty is contained by anything non-empty */
			selec = (1.0 - empty_frac) * hist_selec + empty_frac;
		}
		else
		{
			/* with any other operator, empty Op non-empty matches nothing */
			selec = (1.0 - empty_frac) * hist_selec;
		}
	}

	/* all range operators are strict */
	selec *= (1.0 - null_frac);

	/* result should be in range, but make sure... */
	CLAMP_PROBABILITY(selec);

	return selec;
}

/*
 * Calculate range operator selectivity using histograms of range bounds.
 *
 * This estimate is for the portion of values that are not empty and not
 * NULL.
 */
static double
calc_hist_selectivity(TypeCacheEntry *typcache, VariableStatData *vardata,
					  RangeType *constval, Oid operator)
{
	Datum	   *hist_values;
	int			nhist;
	Datum	   *length_hist_values;
	int			length_nhist;
	RangeBound *hist_lower;
	RangeBound *hist_upper;
	int			i;
	RangeBound	const_lower;
	RangeBound	const_upper;
	bool		empty;
	double		hist_selec;

	/* Try to get histogram of ranges */
	if (!(HeapTupleIsValid(vardata->statsTuple) &&
		  get_attstatsslot(vardata->statsTuple,
						   vardata->atttype, vardata->atttypmod,
						   STATISTIC_KIND_BOUNDS_HISTOGRAM, InvalidOid,
						   NULL,
						   &hist_values, &nhist,
						   NULL, NULL)))
		return -1.0;

	/*
	 * Convert histogram of ranges into histograms of its lower and upper
	 * bounds.
	 */
	hist_lower = (RangeBound *) palloc(sizeof(RangeBound) * nhist);
	hist_upper = (RangeBound *) palloc(sizeof(RangeBound) * nhist);
	for (i = 0; i < nhist; i++)
	{
		range_deserialize(typcache, DatumGetRangeType(hist_values[i]),
						  &hist_lower[i], &hist_upper[i], &empty);
		/* The histogram should not contain any empty ranges */
		if (empty)
			elog(ERROR, "bounds histogram contains an empty range");
	}

	/* @> and @< also need a histogram of range lengths */
	if (operator == OID_RANGE_CONTAINS_OP ||
		operator == OID_RANGE_CONTAINED_OP)
	{
		if (!(HeapTupleIsValid(vardata->statsTuple) &&
			  get_attstatsslot(vardata->statsTuple,
							   vardata->atttype, vardata->atttypmod,
							   STATISTIC_KIND_RANGE_LENGTH_HISTOGRAM,
							   InvalidOid,
							   NULL,
							   &length_hist_values, &length_nhist,
							   NULL, NULL)))
			return -1.0;

		/* check that it's a histogram, not just a dummy entry */
		if (length_nhist < 2)
			return -1.0;
	}

	/* Extract the bounds of the constant value. */
	range_deserialize(typcache, constval, &const_lower, &const_upper, &empty);
	Assert(!empty);

	/*
	 * Calculate selectivity comparing the lower or upper bound of the
	 * constant with the histogram of lower or upper bounds.
	 */
	switch (operator)
	{
		case OID_RANGE_LESS_OP:

			/*
			 * The regular b-tree comparison operators (<, <=, >, >=) compare
			 * the lower bounds first, and the upper bounds for values with
			 * equal lower bounds. Estimate that by comparing the lower bounds
			 * only. This gives a fairly accurate estimate assuming there
			 * aren't many rows with a lower bound equal to the constant's
			 * lower bound.
			 */
			hist_selec =
				calc_hist_selectivity_scalar(typcache, &const_lower,
											 hist_lower, nhist, false);
			break;

		case OID_RANGE_LESS_EQUAL_OP:
			hist_selec =
				calc_hist_selectivity_scalar(typcache, &const_lower,
											 hist_lower, nhist, true);
			break;

		case OID_RANGE_GREATER_OP:
			hist_selec =
				1 - calc_hist_selectivity_scalar(typcache, &const_lower,
												 hist_lower, nhist, false);
			break;

		case OID_RANGE_GREATER_EQUAL_OP:
			hist_selec =
				1 - calc_hist_selectivity_scalar(typcache, &const_lower,
												 hist_lower, nhist, true);
			break;

		case OID_RANGE_LEFT_OP:
			/* var << const when upper(var) < lower(const) */
			hist_selec =
				calc_hist_selectivity_scalar(typcache, &const_lower,
											 hist_upper, nhist, false);
			break;

		case OID_RANGE_RIGHT_OP:
			/* var >> const when lower(var) > upper(const) */
			hist_selec =
				1 - calc_hist_selectivity_scalar(typcache, &const_upper,
												 hist_lower, nhist, true);
			break;

		case OID_RANGE_OVERLAPS_RIGHT_OP:
			/* compare lower bounds */
			hist_selec =
				1 - calc_hist_selectivity_scalar(typcache, &const_lower,
												 hist_lower, nhist, false);
			break;

		case OID_RANGE_OVERLAPS_LEFT_OP:
			/* compare upper bounds */
			hist_selec =
				calc_hist_selectivity_scalar(typcache, &const_upper,
											 hist_upper, nhist, true);
			break;

		case OID_RANGE_OVERLAP_OP:
		case OID_RANGE_CONTAINS_ELEM_OP:

			/*
			 * A && B <=> NOT (A << B OR A >> B).
			 *
			 * Since A << B and A >> B are mutually exclusive events we can
			 * sum their probabilities to find probability of (A << B OR A >>
			 * B).
			 *
			 * "range @> elem" is equivalent to "range && [elem,elem]". The
			 * caller already constructed the singular range from the element
			 * constant, so just treat it the same as &&.
			 */
			hist_selec =
				calc_hist_selectivity_scalar(typcache, &const_lower, hist_upper,
											 nhist, false);
			hist_selec +=
				(1.0 - calc_hist_selectivity_scalar(typcache, &const_upper, hist_lower,
													nhist, true));
			hist_selec = 1.0 - hist_selec;
			break;

		case OID_RANGE_CONTAINS_OP:
			hist_selec =
				calc_hist_selectivity_contains(typcache, &const_lower,
											 &const_upper, hist_lower, nhist,
										   length_hist_values, length_nhist);
			break;

		case OID_RANGE_CONTAINED_OP:
			if (const_lower.infinite)
			{
				/*
				 * Lower bound no longer matters. Just estimate the fraction
				 * with an upper bound <= const uppert bound
				 */
				hist_selec =
					calc_hist_selectivity_scalar(typcache, &const_upper,
												 hist_upper, nhist, true);
			}
			else if (const_upper.infinite)
			{
				hist_selec =
					1.0 - calc_hist_selectivity_scalar(typcache, &const_lower,
												   hist_lower, nhist, false);
			}
			else
			{
				hist_selec =
					calc_hist_selectivity_contained(typcache, &const_lower,
											 &const_upper, hist_lower, nhist,
										   length_hist_values, length_nhist);
			}
			break;

		default:
			elog(ERROR, "unknown range operator %u", operator);
			hist_selec = -1.0;	/* keep compiler quiet */
			break;
	}

	return hist_selec;
}


/*
 * Look up the fraction of values less than (or equal, if 'equal' argument
 * is true) a given const in a histogram of range bounds.
 */
static double
calc_hist_selectivity_scalar(TypeCacheEntry *typcache, RangeBound *constbound,
							 RangeBound *hist, int hist_nvalues, bool equal)
{
	Selectivity selec;
	int			index;

	/*
	 * Find the histogram bin the given constant falls into. Estimate
	 * selectivity as the number of preceding whole bins.
	 */
	index = rbound_bsearch(typcache, constbound, hist, hist_nvalues, equal);
	selec = (Selectivity) (Max(index, 0)) / (Selectivity) (hist_nvalues - 1);

	/* Adjust using linear interpolation within the bin */
	if (index >= 0 && index < hist_nvalues - 1)
		selec += get_position(typcache, constbound, &hist[index],
						&hist[index + 1]) / (Selectivity) (hist_nvalues - 1);

	return selec;
}

/*
 * Binary search on an array of range bounds. Returns greatest index of range
 * bound in array which is less(less or equal) than given range bound. If all
 * range bounds in array are greater or equal(greater) than given range bound,
 * return -1. When "equal" flag is set conditions in brackets are used.
 *
 * This function is used in scalar operator selectivity estimation. Another
 * goal of this function is to find a histogram bin where to stop
 * interpolation of portion of bounds which are less or equal to given bound.
 */
static int
rbound_bsearch(TypeCacheEntry *typcache, RangeBound *value, RangeBound *hist,
			   int hist_length, bool equal)
{
	int			lower = -1,
				upper = hist_length - 1,
				cmp,
				middle;

	while (lower < upper)
	{
		middle = (lower + upper + 1) / 2;
		cmp = range_cmp_bounds(typcache, &hist[middle], value);

		if (cmp < 0 || (equal && cmp == 0))
			lower = middle;
		else
			upper = middle - 1;
	}
	return lower;
}


/*
 * Binary search on length histogram. Returns greatest index of range length in
 * histogram which is less than (less than or equal) the given length value. If
 * all lengths in the histogram are greater than (greater than or equal) the
 * given length, returns -1.
 */
static int
length_hist_bsearch(Datum *length_hist_values, int length_hist_nvalues,
					double value, bool equal)
{
	int			lower = -1,
				upper = length_hist_nvalues - 1,
				middle;

	while (lower < upper)
	{
		double		middleval;

		middle = (lower + upper + 1) / 2;

		middleval = DatumGetFloat8(length_hist_values[middle]);
		if (middleval < value || (equal && middleval <= value))
			lower = middle;
		else
			upper = middle - 1;
	}
	return lower;
}

/*
 * Get relative position of value in histogram bin in [0,1] range.
 */
static float8
get_position(TypeCacheEntry *typcache, RangeBound *value, RangeBound *hist1,
			 RangeBound *hist2)
{
	bool		has_subdiff = OidIsValid(typcache->rng_subdiff_finfo.fn_oid);
	float8		position;

	if (!hist1->infinite && !hist2->infinite)
	{
		float8		bin_width;

		/*
		 * Both bounds are finite. Assuming the subtype's comparison function
		 * works sanely, the value must be finite, too, because it lies
		 * somewhere between the bounds. If it doesn't, just return something.
		 */
		if (value->infinite)
			return 0.5;

		/* Can't interpolate without subdiff function */
		if (!has_subdiff)
			return 0.5;

		/* Calculate relative position using subdiff function. */
		bin_width = DatumGetFloat8(FunctionCall2Coll(
												&typcache->rng_subdiff_finfo,
													 typcache->rng_collation,
													 hist2->val,
													 hist1->val));
		if (bin_width <= 0.0)
			return 0.5;			/* zero width bin */

		position = DatumGetFloat8(FunctionCall2Coll(
												&typcache->rng_subdiff_finfo,
													typcache->rng_collation,
													value->val,
													hist1->val))
			/ bin_width;

		/* Relative position must be in [0,1] range */
		position = Max(position, 0.0);
		position = Min(position, 1.0);
		return position;
	}
	else if (hist1->infinite && !hist2->infinite)
	{
		/*
		 * Lower bin boundary is -infinite, upper is finite. If the value is
		 * -infinite, return 0.0 to indicate it's equal to the lower bound.
		 * Otherwise return 1.0 to indicate it's infinitely far from the lower
		 * bound.
		 */
		return ((value->infinite && value->lower) ? 0.0 : 1.0);
	}
	else if (!hist1->infinite && hist2->infinite)
	{
		/* same as above, but in reverse */
		return ((value->infinite && !value->lower) ? 1.0 : 0.0);
	}
	else
	{
		/*
		 * If both bin boundaries are infinite, they should be equal to each
		 * other, and the value should also be infinite and equal to both
		 * bounds. (But don't Assert that, to avoid crashing if a user creates
		 * a datatype with a broken comparison function).
		 *
		 * Assume the value to lie in the middle of the infinite bounds.
		 */
		return 0.5;
	}
}


/*
 * Get relative position of value in a length histogram bin in [0,1] range.
 */
static double
get_len_position(double value, double hist1, double hist2)
{
	if (!is_infinite(hist1) && !is_infinite(hist2))
	{
		/*
		 * Both bounds are finite. The value should be finite too, because it
		 * lies somewhere between the bounds. If it doesn't, just return
		 * something.
		 */
		if (is_infinite(value))
			return 0.5;

		return 1.0 - (hist2 - value) / (hist2 - hist1);
	}
	else if (is_infinite(hist1) && !is_infinite(hist2))
	{
		/*
		 * Lower bin boundary is -infinite, upper is finite. Return 1.0 to
		 * indicate the value is infinitely far from the lower bound.
		 */
		return 1.0;
	}
	else if (is_infinite(hist1) && is_infinite(hist2))
	{
		/* same as above, but in reverse */
		return 0.0;
	}
	else
	{
		/*
		 * If both bin boundaries are infinite, they should be equal to each
		 * other, and the value should also be infinite and equal to both
		 * bounds. (But don't Assert that, to avoid crashing unnecessarily if
		 * the caller messes up)
		 *
		 * Assume the value to lie in the middle of the infinite bounds.
		 */
		return 0.5;
	}
}

/*
 * Measure distance between two range bounds.
 */
static float8
get_distance(TypeCacheEntry *typcache, RangeBound *bound1, RangeBound *bound2)
{
	bool		has_subdiff = OidIsValid(typcache->rng_subdiff_finfo.fn_oid);

	if (!bound1->infinite && !bound2->infinite)
	{
		/*
		 * No bounds are infinite, use subdiff function or return default
		 * value of 1.0 if no subdiff is available.
		 */
		if (has_subdiff)
			return
				DatumGetFloat8(FunctionCall2Coll(&typcache->rng_subdiff_finfo,
												 typcache->rng_collation,
												 bound2->val,
												 bound1->val));
		else
			return 1.0;
	}
	else if (bound1->infinite && bound2->infinite)
	{
		/* Both bounds are infinite */
		if (bound1->lower == bound2->lower)
			return 0.0;
		else
			return get_float8_infinity();
	}
	else
	{
		/* One bound is infinite, another is not */
		return get_float8_infinity();
	}
}

/*
 * Calculate the average of function P(x), in the interval [length1, length2],
 * where P(x) is the fraction of tuples with length < x (or length <= x if
 * 'equal' is true).
 */
static double
calc_length_hist_frac(Datum *length_hist_values, int length_hist_nvalues,
					  double length1, double length2, bool equal)
{
	double		frac;
	double		A,
				B,
				PA,
				PB;
	double		pos;
	int			i;
	double		area;

	Assert(length2 >= length1);

	if (length2 < 0.0)
		return 0.0;				/* shouldn't happen, but doesn't hurt to check */

	/* All lengths in the table are <= infinite. */
	if (is_infinite(length2) && equal)
		return 1.0;

	/*----------
	 * The average of a function between A and B can be calculated by the
	 * formula:
	 *
	 *			B
	 *	  1		/
	 * -------	| P(x)dx
	 *	B - A	/
	 *			A
	 *
	 * The geometrical interpretation of the integral is the area under the
	 * graph of P(x). P(x) is defined by the length histogram. We calculate
	 * the area in a piecewise fashion, iterating through the length histogram
	 * bins. Each bin is a trapezoid:
	 *
	 *		 P(x2)
	 *		  /|
	 *		 / |
	 * P(x1)/  |
	 *	   |   |
	 *	   |   |
	 *	---+---+--
	 *	   x1  x2
	 *
	 * where x1 and x2 are the boundaries of the current histogram, and P(x1)
	 * and P(x1) are the cumulative fraction of tuples at the boundaries.
	 *
	 * The area of each trapezoid is 1/2 * (P(x2) + P(x1)) * (x2 - x1)
	 *
	 * The first bin contains the lower bound passed by the caller, so we
	 * use linear interpolation between the previous and next histogram bin
	 * boundary to calculate P(x1). Likewise for the last bin: we use linear
	 * interpolation to calculate P(x2). For the bins in between, x1 and x2
	 * lie on histogram bin boundaries, so P(x1) and P(x2) are simply:
	 * P(x1) =	  (bin index) / (number of bins)
	 * P(x2) = (bin index + 1 / (number of bins)
	 */

	/* First bin, the one that contains lower bound */
	i = length_hist_bsearch(length_hist_values, length_hist_nvalues, length1, equal);
	if (i >= length_hist_nvalues - 1)
		return 1.0;

	if (i < 0)
	{
		i = 0;
		pos = 0.0;
	}
	else
	{
		/* interpolate length1's position in the bin */
		pos = get_len_position(length1,
							   DatumGetFloat8(length_hist_values[i]),
							   DatumGetFloat8(length_hist_values[i + 1]));
	}
	PB = (((double) i) + pos) / (double) (length_hist_nvalues - 1);
	B = length1;

	/*
	 * In the degenerate case that length1 == length2, simply return
	 * P(length1). This is not merely an optimization: if length1 == length2,
	 * we'd divide by zero later on.
	 */
	if (length2 == length1)
		return PB;

	/*
	 * Loop through all the bins, until we hit the last bin, the one that
	 * contains the upper bound. (if lower and upper bounds are in the same
	 * bin, this falls out immediately)
	 */
	area = 0.0;
	for (; i < length_hist_nvalues - 1; i++)
	{
		double		bin_upper = DatumGetFloat8(length_hist_values[i + 1]);

		/* check if we've reached the last bin */
		if (!(bin_upper < length2 || (equal && bin_upper <= length2)))
			break;

		/* the upper bound of previous bin is the lower bound of this bin */
		A = B;
		PA = PB;

		B = bin_upper;
		PB = (double) i / (double) (length_hist_nvalues - 1);

		/*
		 * Add the area of this trapezoid to the total. The point of the
		 * if-check is to avoid NaN, in the corner case that PA == PB == 0,
		 * and B - A == Inf. The area of a zero-height trapezoid (PA == PB ==
		 * 0) is zero, regardless of the width (B - A).
		 */
		if (PA > 0 || PB > 0)
			area += 0.5 * (PB + PA) * (B - A);
	}

	/* Last bin */
	A = B;
	PA = PB;

	B = length2;				/* last bin ends at the query upper bound */
	if (i >= length_hist_nvalues - 1)
		pos = 0.0;
	else
	{
		if (DatumGetFloat8(length_hist_values[i]) == DatumGetFloat8(length_hist_values[i + 1]))
			pos = 0.0;
		else
			pos = get_len_position(length2, DatumGetFloat8(length_hist_values[i]), DatumGetFloat8(length_hist_values[i + 1]));
	}
	PB = (((double) i) + pos) / (double) (length_hist_nvalues - 1);

	if (PA > 0 || PB > 0)
		area += 0.5 * (PB + PA) * (B - A);

	/*
	 * Ok, we have calculated the area, ie. the integral. Divide by width to
	 * get the requested average.
	 *
	 * Avoid NaN arising from infinite / infinite. This happens at least if
	 * length2 is infinite. It's not clear what the correct value would be in
	 * that case, so 0.5 seems as good as any value.
	 */
	if (is_infinite(area) && is_infinite(length2))
		frac = 0.5;
	else
		frac = area / (length2 - length1);

	return frac;
}

/*
 * Calculate selectivity of "var <@ const" operator, ie. estimate the fraction
 * of ranges that fall within the constant lower and upper bounds. This uses
 * the histograms of range lower bounds and range lengths, on the assumption
 * that the range lengths are independent of the lower bounds.
 *
 * The caller has already checked that constant lower and upper bounds are
 * finite.
 */
static double
calc_hist_selectivity_contained(TypeCacheEntry *typcache,
								RangeBound *lower, RangeBound *upper,
								RangeBound *hist_lower, int hist_nvalues,
						  Datum *length_hist_values, int length_hist_nvalues)
{
	int			i,
				upper_index;
	float8		prev_dist;
	double		bin_width;
	double		upper_bin_width;
	double		sum_frac;

	/*
	 * Begin by finding the bin containing the upper bound, in the lower bound
	 * histogram. Any range with a lower bound > constant upper bound can't
	 * match, ie. there are no matches in bins greater than upper_index.
	 */
	upper->inclusive = !upper->inclusive;
	upper->lower = true;
	upper_index = rbound_bsearch(typcache, upper, hist_lower, hist_nvalues,
								 false);

	/*
	 * Calculate upper_bin_width, ie. the fraction of the (upper_index,
	 * upper_index + 1) bin which is greater than upper bound of query range
	 * using linear interpolation of subdiff function.
	 */
	if (upper_index >= 0 && upper_index < hist_nvalues - 1)
		upper_bin_width = get_position(typcache, upper,
									   &hist_lower[upper_index],
									   &hist_lower[upper_index + 1]);
	else
		upper_bin_width = 0.0;

	/*
	 * In the loop, dist and prev_dist are the distance of the "current" bin's
	 * lower and upper bounds from the constant upper bound.
	 *
	 * bin_width represents the width of the current bin. Normally it is 1.0,
	 * meaning a full width bin, but can be less in the corner cases: start
	 * and end of the loop. We start with bin_width = upper_bin_width, because
	 * we begin at the bin containing the upper bound.
	 */
	prev_dist = 0.0;
	bin_width = upper_bin_width;

	sum_frac = 0.0;
	for (i = upper_index; i >= 0; i--)
	{
		double		dist;
		double		length_hist_frac;
		bool		final_bin = false;

		/*
		 * dist -- distance from upper bound of query range to lower bound of
		 * the current bin in the lower bound histogram. Or to the lower bound
		 * of the constant range, if this is the final bin, containing the
		 * constant lower bound.
		 */
		if (range_cmp_bounds(typcache, &hist_lower[i], lower) < 0)
		{
			dist = get_distance(typcache, lower, upper);

			/*
			 * Subtract from bin_width the portion of this bin that we want to
			 * ignore.
			 */
			bin_width -= get_position(typcache, lower, &hist_lower[i],
									  &hist_lower[i + 1]);
			if (bin_width < 0.0)
				bin_width = 0.0;
			final_bin = true;
		}
		else
			dist = get_distance(typcache, &hist_lower[i], upper);

		/*
		 * Estimate the fraction of tuples in this bin that are narrow enough
		 * to not exceed the distance to the upper bound of the query range.
		 */
		length_hist_frac = calc_length_hist_frac(length_hist_values,
												 length_hist_nvalues,
												 prev_dist, dist, true);

		/*
		 * Add the fraction of tuples in this bin, with a suitable length, to
		 * the total.
		 */
		sum_frac += length_hist_frac * bin_width / (double) (hist_nvalues - 1);

		if (final_bin)
			break;

		bin_width = 1.0;
		prev_dist = dist;
	}

	return sum_frac;
}

/*
 * Calculate selectivity of "var @> const" operator, ie. estimate the fraction
 * of ranges that contain the constant lower and upper bounds. This uses
 * the histograms of range lower bounds and range lengths, on the assumption
 * that the range lengths are independent of the lower bounds.
 *
 * Note, this is "var @> const", ie. estimate the fraction of ranges that
 * contain the constant lower and upper bounds.
 */
static double
calc_hist_selectivity_contains(TypeCacheEntry *typcache,
							   RangeBound *lower, RangeBound *upper,
							   RangeBound *hist_lower, int hist_nvalues,
						  Datum *length_hist_values, int length_hist_nvalues)
{
	int			i,
				lower_index;
	double		bin_width,
				lower_bin_width;
	double		sum_frac;
	float8		prev_dist;

	/* Find the bin containing the lower bound of query range. */
	lower_index = rbound_bsearch(typcache, lower, hist_lower, hist_nvalues,
								 true);

	/*
	 * Calculate lower_bin_width, ie. the fraction of the of (lower_index,
	 * lower_index + 1) bin which is greater than lower bound of query range
	 * using linear interpolation of subdiff function.
	 */
	if (lower_index >= 0 && lower_index < hist_nvalues - 1)
		lower_bin_width = get_position(typcache, lower, &hist_lower[lower_index],
									   &hist_lower[lower_index + 1]);
	else
		lower_bin_width = 0.0;

	/*
	 * Loop through all the lower bound bins, smaller than the query lower
	 * bound. In the loop, dist and prev_dist are the distance of the
	 * "current" bin's lower and upper bounds from the constant upper bound.
	 * We begin from query lower bound, and walk backwards, so the first bin's
	 * upper bound is the query lower bound, and its distance to the query
	 * upper bound is the length of the query range.
	 *
	 * bin_width represents the width of the current bin. Normally it is 1.0,
	 * meaning a full width bin, except for the first bin, which is only
	 * counted up to the constant lower bound.
	 */
	prev_dist = get_distance(typcache, lower, upper);
	sum_frac = 0.0;
	bin_width = lower_bin_width;
	for (i = lower_index; i >= 0; i--)
	{
		float8		dist;
		double		length_hist_frac;

		/*
		 * dist -- distance from upper bound of query range to current value
		 * of lower bound histogram or lower bound of query range (if we've
		 * reach it).
		 */
		dist = get_distance(typcache, &hist_lower[i], upper);

		/*
		 * Get average fraction of length histogram which covers intervals
		 * longer than (or equal to) distance to upper bound of query range.
		 */
		length_hist_frac =
			1.0 - calc_length_hist_frac(length_hist_values,
										length_hist_nvalues,
										prev_dist, dist, false);

		sum_frac += length_hist_frac * bin_width / (double) (hist_nvalues - 1);

		bin_width = 1.0;
		prev_dist = dist;
	}

	return sum_frac;
}
