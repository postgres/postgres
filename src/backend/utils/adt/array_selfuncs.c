/*-------------------------------------------------------------------------
 *
 * array_selfuncs.c
 *	  Functions for selectivity estimation of array operators
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/array_selfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_statistic.h"
#include "utils/array.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/typcache.h"


/* Default selectivity constant for "@>" and "<@" operators */
#define DEFAULT_CONTAIN_SEL 0.005

/* Default selectivity constant for "&&" operator */
#define DEFAULT_OVERLAP_SEL 0.01

/* Default selectivity for given operator */
#define DEFAULT_SEL(operator) \
	((operator) == OID_ARRAY_OVERLAP_OP ? \
		DEFAULT_OVERLAP_SEL : DEFAULT_CONTAIN_SEL)

static Selectivity calc_arraycontsel(VariableStatData *vardata, Datum constval,
									 Oid elemtype, Oid operator);
static Selectivity mcelem_array_selec(ArrayType *array,
									  TypeCacheEntry *typentry,
									  Datum *mcelem, int nmcelem,
									  float4 *numbers, int nnumbers,
									  float4 *hist, int nhist,
									  Oid operator);
static Selectivity mcelem_array_contain_overlap_selec(Datum *mcelem, int nmcelem,
													  float4 *numbers, int nnumbers,
													  Datum *array_data, int nitems,
													  Oid operator, TypeCacheEntry *typentry);
static Selectivity mcelem_array_contained_selec(Datum *mcelem, int nmcelem,
												float4 *numbers, int nnumbers,
												Datum *array_data, int nitems,
												float4 *hist, int nhist,
												Oid operator, TypeCacheEntry *typentry);
static float *calc_hist(const float4 *hist, int nhist, int n);
static float *calc_distr(const float *p, int n, int m, float rest);
static int	floor_log2(uint32 n);
static bool find_next_mcelem(Datum *mcelem, int nmcelem, Datum value,
							 int *index, TypeCacheEntry *typentry);
static int	element_compare(const void *key1, const void *key2, void *arg);
static int	float_compare_desc(const void *key1, const void *key2);


/*
 * scalararraysel_containment
 *		Estimate selectivity of ScalarArrayOpExpr via array containment.
 *
 * If we have const =/<> ANY/ALL (array_var) then we can estimate the
 * selectivity as though this were an array containment operator,
 * array_var op ARRAY[const].
 *
 * scalararraysel() has already verified that the ScalarArrayOpExpr's operator
 * is the array element type's default equality or inequality operator, and
 * has aggressively simplified both inputs to constants.
 *
 * Returns selectivity (0..1), or -1 if we fail to estimate selectivity.
 */
Selectivity
scalararraysel_containment(PlannerInfo *root,
						   Node *leftop, Node *rightop,
						   Oid elemtype, bool isEquality, bool useOr,
						   int varRelid)
{
	Selectivity selec;
	VariableStatData vardata;
	Datum		constval;
	TypeCacheEntry *typentry;
	FmgrInfo   *cmpfunc;

	/*
	 * rightop must be a variable, else punt.
	 */
	examine_variable(root, rightop, varRelid, &vardata);
	if (!vardata.rel)
	{
		ReleaseVariableStats(vardata);
		return -1.0;
	}

	/*
	 * leftop must be a constant, else punt.
	 */
	if (!IsA(leftop, Const))
	{
		ReleaseVariableStats(vardata);
		return -1.0;
	}
	if (((Const *) leftop)->constisnull)
	{
		/* qual can't succeed if null on left */
		ReleaseVariableStats(vardata);
		return (Selectivity) 0.0;
	}
	constval = ((Const *) leftop)->constvalue;

	/* Get element type's default comparison function */
	typentry = lookup_type_cache(elemtype, TYPECACHE_CMP_PROC_FINFO);
	if (!OidIsValid(typentry->cmp_proc_finfo.fn_oid))
	{
		ReleaseVariableStats(vardata);
		return -1.0;
	}
	cmpfunc = &typentry->cmp_proc_finfo;

	/*
	 * If the operator is <>, swap ANY/ALL, then invert the result later.
	 */
	if (!isEquality)
		useOr = !useOr;

	/* Get array element stats for var, if available */
	if (HeapTupleIsValid(vardata.statsTuple) &&
		statistic_proc_security_check(&vardata, cmpfunc->fn_oid))
	{
		Form_pg_statistic stats;
		AttStatsSlot sslot;
		AttStatsSlot hslot;

		stats = (Form_pg_statistic) GETSTRUCT(vardata.statsTuple);

		/* MCELEM will be an array of same type as element */
		if (get_attstatsslot(&sslot, vardata.statsTuple,
							 STATISTIC_KIND_MCELEM, InvalidOid,
							 ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS))
		{
			/* For ALL case, also get histogram of distinct-element counts */
			if (useOr ||
				!get_attstatsslot(&hslot, vardata.statsTuple,
								  STATISTIC_KIND_DECHIST, InvalidOid,
								  ATTSTATSSLOT_NUMBERS))
				memset(&hslot, 0, sizeof(hslot));

			/*
			 * For = ANY, estimate as var @> ARRAY[const].
			 *
			 * For = ALL, estimate as var <@ ARRAY[const].
			 */
			if (useOr)
				selec = mcelem_array_contain_overlap_selec(sslot.values,
														   sslot.nvalues,
														   sslot.numbers,
														   sslot.nnumbers,
														   &constval, 1,
														   OID_ARRAY_CONTAINS_OP,
														   typentry);
			else
				selec = mcelem_array_contained_selec(sslot.values,
													 sslot.nvalues,
													 sslot.numbers,
													 sslot.nnumbers,
													 &constval, 1,
													 hslot.numbers,
													 hslot.nnumbers,
													 OID_ARRAY_CONTAINED_OP,
													 typentry);

			free_attstatsslot(&hslot);
			free_attstatsslot(&sslot);
		}
		else
		{
			/* No most-common-elements info, so do without */
			if (useOr)
				selec = mcelem_array_contain_overlap_selec(NULL, 0,
														   NULL, 0,
														   &constval, 1,
														   OID_ARRAY_CONTAINS_OP,
														   typentry);
			else
				selec = mcelem_array_contained_selec(NULL, 0,
													 NULL, 0,
													 &constval, 1,
													 NULL, 0,
													 OID_ARRAY_CONTAINED_OP,
													 typentry);
		}

		/*
		 * MCE stats count only non-null rows, so adjust for null rows.
		 */
		selec *= (1.0 - stats->stanullfrac);
	}
	else
	{
		/* No stats at all, so do without */
		if (useOr)
			selec = mcelem_array_contain_overlap_selec(NULL, 0,
													   NULL, 0,
													   &constval, 1,
													   OID_ARRAY_CONTAINS_OP,
													   typentry);
		else
			selec = mcelem_array_contained_selec(NULL, 0,
												 NULL, 0,
												 &constval, 1,
												 NULL, 0,
												 OID_ARRAY_CONTAINED_OP,
												 typentry);
		/* we assume no nulls here, so no stanullfrac correction */
	}

	ReleaseVariableStats(vardata);

	/*
	 * If the operator is <>, invert the results.
	 */
	if (!isEquality)
		selec = 1.0 - selec;

	CLAMP_PROBABILITY(selec);

	return selec;
}

/*
 * arraycontsel -- restriction selectivity for array @>, &&, <@ operators
 */
Datum
arraycontsel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;
	Selectivity selec;
	Oid			element_typeid;

	/*
	 * If expression is not (variable op something) or (something op
	 * variable), then punt and return a default estimate.
	 */
	if (!get_restriction_variable(root, args, varRelid,
								  &vardata, &other, &varonleft))
		PG_RETURN_FLOAT8(DEFAULT_SEL(operator));

	/*
	 * Can't do anything useful if the something is not a constant, either.
	 */
	if (!IsA(other, Const))
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(DEFAULT_SEL(operator));
	}

	/*
	 * The "&&", "@>" and "<@" operators are strict, so we can cope with a
	 * NULL constant right away.
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
		if (operator == OID_ARRAY_CONTAINS_OP)
			operator = OID_ARRAY_CONTAINED_OP;
		else if (operator == OID_ARRAY_CONTAINED_OP)
			operator = OID_ARRAY_CONTAINS_OP;
	}

	/*
	 * OK, there's a Var and a Const we're dealing with here.  We need the
	 * Const to be an array with same element type as column, else we can't do
	 * anything useful.  (Such cases will likely fail at runtime, but here
	 * we'd rather just return a default estimate.)
	 */
	element_typeid = get_base_element_type(((Const *) other)->consttype);
	if (element_typeid != InvalidOid &&
		element_typeid == get_base_element_type(vardata.vartype))
	{
		selec = calc_arraycontsel(&vardata, ((Const *) other)->constvalue,
								  element_typeid, operator);
	}
	else
	{
		selec = DEFAULT_SEL(operator);
	}

	ReleaseVariableStats(vardata);

	CLAMP_PROBABILITY(selec);

	PG_RETURN_FLOAT8((float8) selec);
}

/*
 * arraycontjoinsel -- join selectivity for array @>, &&, <@ operators
 */
Datum
arraycontjoinsel(PG_FUNCTION_ARGS)
{
	/* For the moment this is just a stub */
	Oid			operator = PG_GETARG_OID(1);

	PG_RETURN_FLOAT8(DEFAULT_SEL(operator));
}

/*
 * Calculate selectivity for "arraycolumn @> const", "arraycolumn && const"
 * or "arraycolumn <@ const" based on the statistics
 *
 * This function is mainly responsible for extracting the pg_statistic data
 * to be used; we then pass the problem on to mcelem_array_selec().
 */
static Selectivity
calc_arraycontsel(VariableStatData *vardata, Datum constval,
				  Oid elemtype, Oid operator)
{
	Selectivity selec;
	TypeCacheEntry *typentry;
	FmgrInfo   *cmpfunc;
	ArrayType  *array;

	/* Get element type's default comparison function */
	typentry = lookup_type_cache(elemtype, TYPECACHE_CMP_PROC_FINFO);
	if (!OidIsValid(typentry->cmp_proc_finfo.fn_oid))
		return DEFAULT_SEL(operator);
	cmpfunc = &typentry->cmp_proc_finfo;

	/*
	 * The caller made sure the const is an array with same element type, so
	 * get it now
	 */
	array = DatumGetArrayTypeP(constval);

	if (HeapTupleIsValid(vardata->statsTuple) &&
		statistic_proc_security_check(vardata, cmpfunc->fn_oid))
	{
		Form_pg_statistic stats;
		AttStatsSlot sslot;
		AttStatsSlot hslot;

		stats = (Form_pg_statistic) GETSTRUCT(vardata->statsTuple);

		/* MCELEM will be an array of same type as column */
		if (get_attstatsslot(&sslot, vardata->statsTuple,
							 STATISTIC_KIND_MCELEM, InvalidOid,
							 ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS))
		{
			/*
			 * For "array <@ const" case we also need histogram of distinct
			 * element counts.
			 */
			if (operator != OID_ARRAY_CONTAINED_OP ||
				!get_attstatsslot(&hslot, vardata->statsTuple,
								  STATISTIC_KIND_DECHIST, InvalidOid,
								  ATTSTATSSLOT_NUMBERS))
				memset(&hslot, 0, sizeof(hslot));

			/* Use the most-common-elements slot for the array Var. */
			selec = mcelem_array_selec(array, typentry,
									   sslot.values, sslot.nvalues,
									   sslot.numbers, sslot.nnumbers,
									   hslot.numbers, hslot.nnumbers,
									   operator);

			free_attstatsslot(&hslot);
			free_attstatsslot(&sslot);
		}
		else
		{
			/* No most-common-elements info, so do without */
			selec = mcelem_array_selec(array, typentry,
									   NULL, 0, NULL, 0, NULL, 0,
									   operator);
		}

		/*
		 * MCE stats count only non-null rows, so adjust for null rows.
		 */
		selec *= (1.0 - stats->stanullfrac);
	}
	else
	{
		/* No stats at all, so do without */
		selec = mcelem_array_selec(array, typentry,
								   NULL, 0, NULL, 0, NULL, 0,
								   operator);
		/* we assume no nulls here, so no stanullfrac correction */
	}

	/* If constant was toasted, release the copy we made */
	if (PointerGetDatum(array) != constval)
		pfree(array);

	return selec;
}

/*
 * Array selectivity estimation based on most common elements statistics
 *
 * This function just deconstructs and sorts the array constant's contents,
 * and then passes the problem on to mcelem_array_contain_overlap_selec or
 * mcelem_array_contained_selec depending on the operator.
 */
static Selectivity
mcelem_array_selec(ArrayType *array, TypeCacheEntry *typentry,
				   Datum *mcelem, int nmcelem,
				   float4 *numbers, int nnumbers,
				   float4 *hist, int nhist,
				   Oid operator)
{
	Selectivity selec;
	int			num_elems;
	Datum	   *elem_values;
	bool	   *elem_nulls;
	bool		null_present;
	int			nonnull_nitems;
	int			i;

	/*
	 * Prepare constant array data for sorting.  Sorting lets us find unique
	 * elements and efficiently merge with the MCELEM array.
	 */
	deconstruct_array(array,
					  typentry->type_id,
					  typentry->typlen,
					  typentry->typbyval,
					  typentry->typalign,
					  &elem_values, &elem_nulls, &num_elems);

	/* Collapse out any null elements */
	nonnull_nitems = 0;
	null_present = false;
	for (i = 0; i < num_elems; i++)
	{
		if (elem_nulls[i])
			null_present = true;
		else
			elem_values[nonnull_nitems++] = elem_values[i];
	}

	/*
	 * Query "column @> '{anything, null}'" matches nothing.  For the other
	 * two operators, presence of a null in the constant can be ignored.
	 */
	if (null_present && operator == OID_ARRAY_CONTAINS_OP)
	{
		pfree(elem_values);
		pfree(elem_nulls);
		return (Selectivity) 0.0;
	}

	/* Sort extracted elements using their default comparison function. */
	qsort_arg(elem_values, nonnull_nitems, sizeof(Datum),
			  element_compare, typentry);

	/* Separate cases according to operator */
	if (operator == OID_ARRAY_CONTAINS_OP || operator == OID_ARRAY_OVERLAP_OP)
		selec = mcelem_array_contain_overlap_selec(mcelem, nmcelem,
												   numbers, nnumbers,
												   elem_values, nonnull_nitems,
												   operator, typentry);
	else if (operator == OID_ARRAY_CONTAINED_OP)
		selec = mcelem_array_contained_selec(mcelem, nmcelem,
											 numbers, nnumbers,
											 elem_values, nonnull_nitems,
											 hist, nhist,
											 operator, typentry);
	else
	{
		elog(ERROR, "arraycontsel called for unrecognized operator %u",
			 operator);
		selec = 0.0;			/* keep compiler quiet */
	}

	pfree(elem_values);
	pfree(elem_nulls);
	return selec;
}

/*
 * Estimate selectivity of "column @> const" and "column && const" based on
 * most common element statistics.  This estimation assumes element
 * occurrences are independent.
 *
 * mcelem (of length nmcelem) and numbers (of length nnumbers) are from
 * the array column's MCELEM statistics slot, or are NULL/0 if stats are
 * not available.  array_data (of length nitems) is the constant's elements.
 *
 * Both the mcelem and array_data arrays are assumed presorted according
 * to the element type's cmpfunc.  Null elements are not present.
 *
 * TODO: this estimate probably could be improved by using the distinct
 * elements count histogram.  For example, excepting the special case of
 * "column @> '{}'", we can multiply the calculated selectivity by the
 * fraction of nonempty arrays in the column.
 */
static Selectivity
mcelem_array_contain_overlap_selec(Datum *mcelem, int nmcelem,
								   float4 *numbers, int nnumbers,
								   Datum *array_data, int nitems,
								   Oid operator, TypeCacheEntry *typentry)
{
	Selectivity selec,
				elem_selec;
	int			mcelem_index,
				i;
	bool		use_bsearch;
	float4		minfreq;

	/*
	 * There should be three more Numbers than Values, because the last three
	 * cells should hold minimal and maximal frequency among the non-null
	 * elements, and then the frequency of null elements.  Ignore the Numbers
	 * if not right.
	 */
	if (nnumbers != nmcelem + 3)
	{
		numbers = NULL;
		nnumbers = 0;
	}

	if (numbers)
	{
		/* Grab the lowest observed frequency */
		minfreq = numbers[nmcelem];
	}
	else
	{
		/* Without statistics make some default assumptions */
		minfreq = 2 * (float4) DEFAULT_CONTAIN_SEL;
	}

	/* Decide whether it is faster to use binary search or not. */
	if (nitems * floor_log2((uint32) nmcelem) < nmcelem + nitems)
		use_bsearch = true;
	else
		use_bsearch = false;

	if (operator == OID_ARRAY_CONTAINS_OP)
	{
		/*
		 * Initial selectivity for "column @> const" query is 1.0, and it will
		 * be decreased with each element of constant array.
		 */
		selec = 1.0;
	}
	else
	{
		/*
		 * Initial selectivity for "column && const" query is 0.0, and it will
		 * be increased with each element of constant array.
		 */
		selec = 0.0;
	}

	/* Scan mcelem and array in parallel. */
	mcelem_index = 0;
	for (i = 0; i < nitems; i++)
	{
		bool		match = false;

		/* Ignore any duplicates in the array data. */
		if (i > 0 &&
			element_compare(&array_data[i - 1], &array_data[i], typentry) == 0)
			continue;

		/* Find the smallest MCELEM >= this array item. */
		if (use_bsearch)
		{
			match = find_next_mcelem(mcelem, nmcelem, array_data[i],
									 &mcelem_index, typentry);
		}
		else
		{
			while (mcelem_index < nmcelem)
			{
				int			cmp = element_compare(&mcelem[mcelem_index],
												  &array_data[i],
												  typentry);

				if (cmp < 0)
					mcelem_index++;
				else
				{
					if (cmp == 0)
						match = true;	/* mcelem is found */
					break;
				}
			}
		}

		if (match && numbers)
		{
			/* MCELEM matches the array item; use its frequency. */
			elem_selec = numbers[mcelem_index];
			mcelem_index++;
		}
		else
		{
			/*
			 * The element is not in MCELEM.  Punt, but assume that the
			 * selectivity cannot be more than minfreq / 2.
			 */
			elem_selec = Min(DEFAULT_CONTAIN_SEL, minfreq / 2);
		}

		/*
		 * Update overall selectivity using the current element's selectivity
		 * and an assumption of element occurrence independence.
		 */
		if (operator == OID_ARRAY_CONTAINS_OP)
			selec *= elem_selec;
		else
			selec = selec + elem_selec - selec * elem_selec;

		/* Clamp intermediate results to stay sane despite roundoff error */
		CLAMP_PROBABILITY(selec);
	}

	return selec;
}

/*
 * Estimate selectivity of "column <@ const" based on most common element
 * statistics.
 *
 * mcelem (of length nmcelem) and numbers (of length nnumbers) are from
 * the array column's MCELEM statistics slot, or are NULL/0 if stats are
 * not available.  array_data (of length nitems) is the constant's elements.
 * hist (of length nhist) is from the array column's DECHIST statistics slot,
 * or is NULL/0 if those stats are not available.
 *
 * Both the mcelem and array_data arrays are assumed presorted according
 * to the element type's cmpfunc.  Null elements are not present.
 *
 * Independent element occurrence would imply a particular distribution of
 * distinct element counts among matching rows.  Real data usually falsifies
 * that assumption.  For example, in a set of 11-element integer arrays having
 * elements in the range [0..10], element occurrences are typically not
 * independent.  If they were, a sufficiently-large set would include all
 * distinct element counts 0 through 11.  We correct for this using the
 * histogram of distinct element counts.
 *
 * In the "column @> const" and "column && const" cases, we usually have a
 * "const" with low number of elements (otherwise we have selectivity close
 * to 0 or 1 respectively).  That's why the effect of dependence related
 * to distinct element count distribution is negligible there.  In the
 * "column <@ const" case, number of elements is usually high (otherwise we
 * have selectivity close to 0).  That's why we should do a correction with
 * the array distinct element count distribution here.
 *
 * Using the histogram of distinct element counts produces a different
 * distribution law than independent occurrences of elements.  This
 * distribution law can be described as follows:
 *
 * P(o1, o2, ..., on) = f1^o1 * (1 - f1)^(1 - o1) * f2^o2 *
 *	  (1 - f2)^(1 - o2) * ... * fn^on * (1 - fn)^(1 - on) * hist[m] / ind[m]
 *
 * where:
 * o1, o2, ..., on - occurrences of elements 1, 2, ..., n
 *		(1 - occurrence, 0 - no occurrence) in row
 * f1, f2, ..., fn - frequencies of elements 1, 2, ..., n
 *		(scalar values in [0..1]) according to collected statistics
 * m = o1 + o2 + ... + on = total number of distinct elements in row
 * hist[m] - histogram data for occurrence of m elements.
 * ind[m] - probability of m occurrences from n events assuming their
 *	  probabilities to be equal to frequencies of array elements.
 *
 * ind[m] = sum(f1^o1 * (1 - f1)^(1 - o1) * f2^o2 * (1 - f2)^(1 - o2) *
 * ... * fn^on * (1 - fn)^(1 - on), o1, o2, ..., on) | o1 + o2 + .. on = m
 */
static Selectivity
mcelem_array_contained_selec(Datum *mcelem, int nmcelem,
							 float4 *numbers, int nnumbers,
							 Datum *array_data, int nitems,
							 float4 *hist, int nhist,
							 Oid operator, TypeCacheEntry *typentry)
{
	int			mcelem_index,
				i,
				unique_nitems = 0;
	float		selec,
				minfreq,
				nullelem_freq;
	float	   *dist,
			   *mcelem_dist,
			   *hist_part;
	float		avg_count,
				mult,
				rest;
	float	   *elem_selec;

	/*
	 * There should be three more Numbers than Values in the MCELEM slot,
	 * because the last three cells should hold minimal and maximal frequency
	 * among the non-null elements, and then the frequency of null elements.
	 * Punt if not right, because we can't do much without the element freqs.
	 */
	if (numbers == NULL || nnumbers != nmcelem + 3)
		return DEFAULT_CONTAIN_SEL;

	/* Can't do much without a count histogram, either */
	if (hist == NULL || nhist < 3)
		return DEFAULT_CONTAIN_SEL;

	/*
	 * Grab some of the summary statistics that compute_array_stats() stores:
	 * lowest frequency, frequency of null elements, and average distinct
	 * element count.
	 */
	minfreq = numbers[nmcelem];
	nullelem_freq = numbers[nmcelem + 2];
	avg_count = hist[nhist - 1];

	/*
	 * "rest" will be the sum of the frequencies of all elements not
	 * represented in MCELEM.  The average distinct element count is the sum
	 * of the frequencies of *all* elements.  Begin with that; we will proceed
	 * to subtract the MCELEM frequencies.
	 */
	rest = avg_count;

	/*
	 * mult is a multiplier representing estimate of probability that each
	 * mcelem that is not present in constant doesn't occur.
	 */
	mult = 1.0f;

	/*
	 * elem_selec is array of estimated frequencies for elements in the
	 * constant.
	 */
	elem_selec = (float *) palloc(sizeof(float) * nitems);

	/* Scan mcelem and array in parallel. */
	mcelem_index = 0;
	for (i = 0; i < nitems; i++)
	{
		bool		match = false;

		/* Ignore any duplicates in the array data. */
		if (i > 0 &&
			element_compare(&array_data[i - 1], &array_data[i], typentry) == 0)
			continue;

		/*
		 * Iterate over MCELEM until we find an entry greater than or equal to
		 * this element of the constant.  Update "rest" and "mult" for mcelem
		 * entries skipped over.
		 */
		while (mcelem_index < nmcelem)
		{
			int			cmp = element_compare(&mcelem[mcelem_index],
											  &array_data[i],
											  typentry);

			if (cmp < 0)
			{
				mult *= (1.0f - numbers[mcelem_index]);
				rest -= numbers[mcelem_index];
				mcelem_index++;
			}
			else
			{
				if (cmp == 0)
					match = true;	/* mcelem is found */
				break;
			}
		}

		if (match)
		{
			/* MCELEM matches the array item. */
			elem_selec[unique_nitems] = numbers[mcelem_index];
			/* "rest" is decremented for all mcelems, matched or not */
			rest -= numbers[mcelem_index];
			mcelem_index++;
		}
		else
		{
			/*
			 * The element is not in MCELEM.  Punt, but assume that the
			 * selectivity cannot be more than minfreq / 2.
			 */
			elem_selec[unique_nitems] = Min(DEFAULT_CONTAIN_SEL,
											minfreq / 2);
		}

		unique_nitems++;
	}

	/*
	 * If we handled all constant elements without exhausting the MCELEM
	 * array, finish walking it to complete calculation of "rest" and "mult".
	 */
	while (mcelem_index < nmcelem)
	{
		mult *= (1.0f - numbers[mcelem_index]);
		rest -= numbers[mcelem_index];
		mcelem_index++;
	}

	/*
	 * The presence of many distinct rare elements materially decreases
	 * selectivity.  Use the Poisson distribution to estimate the probability
	 * of a column value having zero occurrences of such elements.  See above
	 * for the definition of "rest".
	 */
	mult *= exp(-rest);

	/*----------
	 * Using the distinct element count histogram requires
	 *		O(unique_nitems * (nmcelem + unique_nitems))
	 * operations.  Beyond a certain computational cost threshold, it's
	 * reasonable to sacrifice accuracy for decreased planning time.  We limit
	 * the number of operations to EFFORT * nmcelem; since nmcelem is limited
	 * by the column's statistics target, the work done is user-controllable.
	 *
	 * If the number of operations would be too large, we can reduce it
	 * without losing all accuracy by reducing unique_nitems and considering
	 * only the most-common elements of the constant array.  To make the
	 * results exactly match what we would have gotten with only those
	 * elements to start with, we'd have to remove any discarded elements'
	 * frequencies from "mult", but since this is only an approximation
	 * anyway, we don't bother with that.  Therefore it's sufficient to qsort
	 * elem_selec[] and take the largest elements.  (They will no longer match
	 * up with the elements of array_data[], but we don't care.)
	 *----------
	 */
#define EFFORT 100

	if ((nmcelem + unique_nitems) > 0 &&
		unique_nitems > EFFORT * nmcelem / (nmcelem + unique_nitems))
	{
		/*
		 * Use the quadratic formula to solve for largest allowable N.  We
		 * have A = 1, B = nmcelem, C = - EFFORT * nmcelem.
		 */
		double		b = (double) nmcelem;
		int			n;

		n = (int) ((sqrt(b * b + 4 * EFFORT * b) - b) / 2);

		/* Sort, then take just the first n elements */
		qsort(elem_selec, unique_nitems, sizeof(float),
			  float_compare_desc);
		unique_nitems = n;
	}

	/*
	 * Calculate probabilities of each distinct element count for both mcelems
	 * and constant elements.  At this point, assume independent element
	 * occurrence.
	 */
	dist = calc_distr(elem_selec, unique_nitems, unique_nitems, 0.0f);
	mcelem_dist = calc_distr(numbers, nmcelem, unique_nitems, rest);

	/* ignore hist[nhist-1], which is the average not a histogram member */
	hist_part = calc_hist(hist, nhist - 1, unique_nitems);

	selec = 0.0f;
	for (i = 0; i <= unique_nitems; i++)
	{
		/*
		 * mult * dist[i] / mcelem_dist[i] gives us probability of qual
		 * matching from assumption of independent element occurrence with the
		 * condition that distinct element count = i.
		 */
		if (mcelem_dist[i] > 0)
			selec += hist_part[i] * mult * dist[i] / mcelem_dist[i];
	}

	pfree(dist);
	pfree(mcelem_dist);
	pfree(hist_part);
	pfree(elem_selec);

	/* Take into account occurrence of NULL element. */
	selec *= (1.0f - nullelem_freq);

	CLAMP_PROBABILITY(selec);

	return selec;
}

/*
 * Calculate the first n distinct element count probabilities from a
 * histogram of distinct element counts.
 *
 * Returns a palloc'd array of n+1 entries, with array[k] being the
 * probability of element count k, k in [0..n].
 *
 * We assume that a histogram box with bounds a and b gives 1 / ((b - a + 1) *
 * (nhist - 1)) probability to each value in (a,b) and an additional half of
 * that to a and b themselves.
 */
static float *
calc_hist(const float4 *hist, int nhist, int n)
{
	float	   *hist_part;
	int			k,
				i = 0;
	float		prev_interval = 0,
				next_interval;
	float		frac;

	hist_part = (float *) palloc((n + 1) * sizeof(float));

	/*
	 * frac is a probability contribution for each interval between histogram
	 * values.  We have nhist - 1 intervals, so contribution of each one will
	 * be 1 / (nhist - 1).
	 */
	frac = 1.0f / ((float) (nhist - 1));

	for (k = 0; k <= n; k++)
	{
		int			count = 0;

		/*
		 * Count the histogram boundaries equal to k.  (Although the histogram
		 * should theoretically contain only exact integers, entries are
		 * floats so there could be roundoff error in large values.  Treat any
		 * fractional value as equal to the next larger k.)
		 */
		while (i < nhist && hist[i] <= k)
		{
			count++;
			i++;
		}

		if (count > 0)
		{
			/* k is an exact bound for at least one histogram box. */
			float		val;

			/* Find length between current histogram value and the next one */
			if (i < nhist)
				next_interval = hist[i] - hist[i - 1];
			else
				next_interval = 0;

			/*
			 * count - 1 histogram boxes contain k exclusively.  They
			 * contribute a total of (count - 1) * frac probability.  Also
			 * factor in the partial histogram boxes on either side.
			 */
			val = (float) (count - 1);
			if (next_interval > 0)
				val += 0.5f / next_interval;
			if (prev_interval > 0)
				val += 0.5f / prev_interval;
			hist_part[k] = frac * val;

			prev_interval = next_interval;
		}
		else
		{
			/* k does not appear as an exact histogram bound. */
			if (prev_interval > 0)
				hist_part[k] = frac / prev_interval;
			else
				hist_part[k] = 0.0f;
		}
	}

	return hist_part;
}

/*
 * Consider n independent events with probabilities p[].  This function
 * calculates probabilities of exact k of events occurrence for k in [0..m].
 * Returns a palloc'd array of size m+1.
 *
 * "rest" is the sum of the probabilities of all low-probability events not
 * included in p.
 *
 * Imagine matrix M of size (n + 1) x (m + 1).  Element M[i,j] denotes the
 * probability that exactly j of first i events occur.  Obviously M[0,0] = 1.
 * For any constant j, each increment of i increases the probability iff the
 * event occurs.  So, by the law of total probability:
 *	M[i,j] = M[i - 1, j] * (1 - p[i]) + M[i - 1, j - 1] * p[i]
 *		for i > 0, j > 0.
 *	M[i,0] = M[i - 1, 0] * (1 - p[i]) for i > 0.
 */
static float *
calc_distr(const float *p, int n, int m, float rest)
{
	float	   *row,
			   *prev_row,
			   *tmp;
	int			i,
				j;

	/*
	 * Since we return only the last row of the matrix and need only the
	 * current and previous row for calculations, allocate two rows.
	 */
	row = (float *) palloc((m + 1) * sizeof(float));
	prev_row = (float *) palloc((m + 1) * sizeof(float));

	/* M[0,0] = 1 */
	row[0] = 1.0f;
	for (i = 1; i <= n; i++)
	{
		float		t = p[i - 1];

		/* Swap rows */
		tmp = row;
		row = prev_row;
		prev_row = tmp;

		/* Calculate next row */
		for (j = 0; j <= i && j <= m; j++)
		{
			float		val = 0.0f;

			if (j < i)
				val += prev_row[j] * (1.0f - t);
			if (j > 0)
				val += prev_row[j - 1] * t;
			row[j] = val;
		}
	}

	/*
	 * The presence of many distinct rare (not in "p") elements materially
	 * decreases selectivity.  Model their collective occurrence with the
	 * Poisson distribution.
	 */
	if (rest > DEFAULT_CONTAIN_SEL)
	{
		float		t;

		/* Swap rows */
		tmp = row;
		row = prev_row;
		prev_row = tmp;

		for (i = 0; i <= m; i++)
			row[i] = 0.0f;

		/* Value of Poisson distribution for 0 occurrences */
		t = exp(-rest);

		/*
		 * Calculate convolution of previously computed distribution and the
		 * Poisson distribution.
		 */
		for (i = 0; i <= m; i++)
		{
			for (j = 0; j <= m - i; j++)
				row[j + i] += prev_row[j] * t;

			/* Get Poisson distribution value for (i + 1) occurrences */
			t *= rest / (float) (i + 1);
		}
	}

	pfree(prev_row);
	return row;
}

/* Fast function for floor value of 2 based logarithm calculation. */
static int
floor_log2(uint32 n)
{
	int			logval = 0;

	if (n == 0)
		return -1;
	if (n >= (1 << 16))
	{
		n >>= 16;
		logval += 16;
	}
	if (n >= (1 << 8))
	{
		n >>= 8;
		logval += 8;
	}
	if (n >= (1 << 4))
	{
		n >>= 4;
		logval += 4;
	}
	if (n >= (1 << 2))
	{
		n >>= 2;
		logval += 2;
	}
	if (n >= (1 << 1))
	{
		logval += 1;
	}
	return logval;
}

/*
 * find_next_mcelem binary-searches a most common elements array, starting
 * from *index, for the first member >= value.  It saves the position of the
 * match into *index and returns true if it's an exact match.  (Note: we
 * assume the mcelem elements are distinct so there can't be more than one
 * exact match.)
 */
static bool
find_next_mcelem(Datum *mcelem, int nmcelem, Datum value, int *index,
				 TypeCacheEntry *typentry)
{
	int			l = *index,
				r = nmcelem - 1,
				i,
				res;

	while (l <= r)
	{
		i = (l + r) / 2;
		res = element_compare(&mcelem[i], &value, typentry);
		if (res == 0)
		{
			*index = i;
			return true;
		}
		else if (res < 0)
			l = i + 1;
		else
			r = i - 1;
	}
	*index = l;
	return false;
}

/*
 * Comparison function for elements.
 *
 * We use the element type's default btree opclass, and its default collation
 * if the type is collation-sensitive.
 *
 * XXX consider using SortSupport infrastructure
 */
static int
element_compare(const void *key1, const void *key2, void *arg)
{
	Datum		d1 = *((const Datum *) key1);
	Datum		d2 = *((const Datum *) key2);
	TypeCacheEntry *typentry = (TypeCacheEntry *) arg;
	FmgrInfo   *cmpfunc = &typentry->cmp_proc_finfo;
	Datum		c;

	c = FunctionCall2Coll(cmpfunc, typentry->typcollation, d1, d2);
	return DatumGetInt32(c);
}

/*
 * Comparison function for sorting floats into descending order.
 */
static int
float_compare_desc(const void *key1, const void *key2)
{
	float		d1 = *((const float *) key1);
	float		d2 = *((const float *) key2);

	if (d1 > d2)
		return -1;
	else if (d1 < d2)
		return 1;
	else
		return 0;
}
