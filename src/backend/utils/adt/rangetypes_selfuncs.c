/*-------------------------------------------------------------------------
 *
 * rangetypes_selfuncs.c
 *	  Functions for selectivity estimation of range operators
 *
 * Estimates are based on histograms of lower and upper bounds, and the
 * fraction of empty ranges.
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
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
	TypeCacheEntry *typcache;
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

	typcache = range_get_typcache(fcinfo, vardata.vartype);

	/*
	 * OK, there's a Var and a Const we're dealing with here.  We need the
	 * Const to be of same range type as the column, else we can't do anything
	 * useful. (Such cases will likely fail at runtime, but here we'd rather
	 * just return a default estimate.)
	 *
	 * If the operator is "range @> element", the constant should be of the
	 * element type of the range column. Convert it to a range that includes
	 * only that single point, so that we don't need special handling for
	 * that in what follows.
	 */
	if (operator == OID_RANGE_CONTAINS_ELEM_OP)
	{
		if (((Const *) other)->consttype == typcache->rngelemtype->type_id)
		{
			RangeBound lower, upper;
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
	else
	{
		if (((Const *) other)->consttype == vardata.vartype)
			constrange = DatumGetRangeType(((Const *) other)->constvalue);
	}

	/*
	 * If we got a valid constant on one side of the operator, proceed to
	 * estimate using statistics. Otherwise punt and return a default
	 * constant estimate.
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
	float4		empty_frac, null_frac;

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
							 STATISTIC_KIND_RANGE_EMPTY_FRAC, InvalidOid,
							 NULL,
							 NULL, NULL,
							 &numbers, &nnumbers))
		{
			if (nnumbers != 1)
				elog(ERROR, "invalid empty fraction statistic"); /* shouldn't happen */
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
		 * anyway, assuming no NULLs and no empty ranges. This still allows
		 * us to give a better-than-nothing estimate based on whether the
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
			case OID_RANGE_OVERLAP_OP:
			case OID_RANGE_OVERLAPS_LEFT_OP:
			case OID_RANGE_OVERLAPS_RIGHT_OP:
			case OID_RANGE_LEFT_OP:
			case OID_RANGE_RIGHT_OP:
				/* these return false if either argument is empty */
				selec = 0.0;
				break;

			case OID_RANGE_CONTAINED_OP:
			case OID_RANGE_LESS_EQUAL_OP:
			case OID_RANGE_GREATER_EQUAL_OP:
				/*
				 * these return true when both args are empty, false if only
				 * one is empty
				 */
				selec = empty_frac;
				break;

			case OID_RANGE_CONTAINS_OP:
				/* everything contains an empty range */
				selec = 1.0;
				break;

			case OID_RANGE_CONTAINS_ELEM_OP:
			default:
				elog(ERROR, "unexpected operator %u", operator);
				selec = 0.0; /* keep compiler quiet */
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

	/* Extract the bounds of the constant value. */
	range_deserialize(typcache, constval, &const_lower, &const_upper, &empty);
	Assert (!empty);

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
												 hist_lower, nhist, true);
			break;

		case OID_RANGE_GREATER_EQUAL_OP:
			hist_selec =
				1 - calc_hist_selectivity_scalar(typcache, &const_lower,
												 hist_lower, nhist, false);
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
		case OID_RANGE_CONTAINED_OP:
			/* TODO: not implemented yet */
			hist_selec = -1.0;
			break;

		default:
			elog(ERROR, "unknown range operator %u", operator);
			hist_selec = -1.0; /* keep compiler quiet */
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
	Selectivity	selec;
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
 * bound in array which is less than given range bound. If all range bounds in
 * array are greater or equal than given range bound, return -1.
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
			return 0.5;		/* zero width bin */

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

