/*-------------------------------------------------------------------------
 *
 * network_selfuncs.c
 *	  Functions for selectivity estimation of inet/cidr operators
 *
 * This module provides estimators for the subnet inclusion and overlap
 * operators.  Estimates are based on null fraction, most common values,
 * and histogram of inet/cidr columns.
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/network_selfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_statistic.h"
#include "utils/inet.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"


/* Default selectivity for the inet overlap operator */
#define DEFAULT_OVERLAP_SEL 0.01

/* Default selectivity for the various inclusion operators */
#define DEFAULT_INCLUSION_SEL 0.005

/* Default selectivity for specified operator */
#define DEFAULT_SEL(operator) \
	((operator) == OID_INET_OVERLAP_OP ? \
	 DEFAULT_OVERLAP_SEL : DEFAULT_INCLUSION_SEL)

/* Maximum number of items to consider in join selectivity calculations */
#define MAX_CONSIDERED_ELEMS 1024

static Selectivity networkjoinsel_inner(Oid operator,
					 VariableStatData *vardata1, VariableStatData *vardata2);
static Selectivity networkjoinsel_semi(Oid operator,
					VariableStatData *vardata1, VariableStatData *vardata2);
static Selectivity mcv_population(float4 *mcv_numbers, int mcv_nvalues);
static Selectivity inet_hist_value_sel(Datum *values, int nvalues,
					Datum constvalue, int opr_codenum);
static Selectivity inet_mcv_join_sel(Datum *mcv1_values,
				  float4 *mcv1_numbers, int mcv1_nvalues, Datum *mcv2_values,
				  float4 *mcv2_numbers, int mcv2_nvalues, Oid operator);
static Selectivity inet_mcv_hist_sel(Datum *mcv_values, float4 *mcv_numbers,
				  int mcv_nvalues, Datum *hist_values, int hist_nvalues,
				  int opr_codenum);
static Selectivity inet_hist_inclusion_join_sel(Datum *hist1_values,
							 int hist1_nvalues,
							 Datum *hist2_values, int hist2_nvalues,
							 int opr_codenum);
static Selectivity inet_semi_join_sel(Datum lhs_value,
				   bool mcv_exists, Datum *mcv_values, int mcv_nvalues,
				   bool hist_exists, Datum *hist_values, int hist_nvalues,
				   double hist_weight,
				   FmgrInfo *proc, int opr_codenum);
static int	inet_opr_codenum(Oid operator);
static int	inet_inclusion_cmp(inet *left, inet *right, int opr_codenum);
static int inet_masklen_inclusion_cmp(inet *left, inet *right,
						   int opr_codenum);
static int inet_hist_match_divider(inet *boundary, inet *query,
						int opr_codenum);

/*
 * Selectivity estimation for the subnet inclusion/overlap operators
 */
Datum
networksel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;
	Selectivity selec,
				mcv_selec,
				non_mcv_selec;
	Datum		constvalue,
			   *hist_values;
	int			hist_nvalues;
	Form_pg_statistic stats;
	double		sumcommon,
				nullfrac;
	FmgrInfo	proc;

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

	/* All of the operators handled here are strict. */
	if (((Const *) other)->constisnull)
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(0.0);
	}
	constvalue = ((Const *) other)->constvalue;

	/* Otherwise, we need stats in order to produce a non-default estimate. */
	if (!HeapTupleIsValid(vardata.statsTuple))
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(DEFAULT_SEL(operator));
	}

	stats = (Form_pg_statistic) GETSTRUCT(vardata.statsTuple);
	nullfrac = stats->stanullfrac;

	/*
	 * If we have most-common-values info, add up the fractions of the MCV
	 * entries that satisfy MCV OP CONST.  These fractions contribute directly
	 * to the result selectivity.  Also add up the total fraction represented
	 * by MCV entries.
	 */
	fmgr_info(get_opcode(operator), &proc);
	mcv_selec = mcv_selectivity(&vardata, &proc, constvalue, varonleft,
								&sumcommon);

	/*
	 * If we have a histogram, use it to estimate the proportion of the
	 * non-MCV population that satisfies the clause.  If we don't, apply the
	 * default selectivity to that population.
	 */
	if (get_attstatsslot(vardata.statsTuple,
						 vardata.atttype, vardata.atttypmod,
						 STATISTIC_KIND_HISTOGRAM, InvalidOid,
						 NULL,
						 &hist_values, &hist_nvalues,
						 NULL, NULL))
	{
		int			opr_codenum = inet_opr_codenum(operator);

		/* Commute if needed, so we can consider histogram to be on the left */
		if (!varonleft)
			opr_codenum = -opr_codenum;
		non_mcv_selec = inet_hist_value_sel(hist_values, hist_nvalues,
											constvalue, opr_codenum);

		free_attstatsslot(vardata.atttype, hist_values, hist_nvalues, NULL, 0);
	}
	else
		non_mcv_selec = DEFAULT_SEL(operator);

	/* Combine selectivities for MCV and non-MCV populations */
	selec = mcv_selec + (1.0 - nullfrac - sumcommon) * non_mcv_selec;

	/* Result should be in range, but make sure... */
	CLAMP_PROBABILITY(selec);

	ReleaseVariableStats(vardata);

	PG_RETURN_FLOAT8(selec);
}

/*
 * Join selectivity estimation for the subnet inclusion/overlap operators
 *
 * This function has the same structure as eqjoinsel() in selfuncs.c.
 *
 * Throughout networkjoinsel and its subroutines, we have a performance issue
 * in that the amount of work to be done is O(N^2) in the length of the MCV
 * and histogram arrays.  To keep the runtime from getting out of hand when
 * large statistics targets have been set, we arbitrarily limit the number of
 * values considered to 1024 (MAX_CONSIDERED_ELEMS).  For the MCV arrays, this
 * is easy: just consider at most the first N elements.  (Since the MCVs are
 * sorted by decreasing frequency, this correctly gets us the first N MCVs.)
 * For the histogram arrays, we decimate; that is consider only every k'th
 * element, where k is chosen so that no more than MAX_CONSIDERED_ELEMS
 * elements are considered.  This should still give us a good random sample of
 * the non-MCV population.  Decimation is done on-the-fly in the loops that
 * iterate over the histogram arrays.
 */
Datum
networkjoinsel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
#ifdef NOT_USED
	JoinType	jointype = (JoinType) PG_GETARG_INT16(3);
#endif
	SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) PG_GETARG_POINTER(4);
	double		selec;
	VariableStatData vardata1;
	VariableStatData vardata2;
	bool		join_is_reversed;

	get_join_variables(root, args, sjinfo,
					   &vardata1, &vardata2, &join_is_reversed);

	switch (sjinfo->jointype)
	{
		case JOIN_INNER:
		case JOIN_LEFT:
		case JOIN_FULL:

			/*
			 * Selectivity for left/full join is not exactly the same as inner
			 * join, but we neglect the difference, as eqjoinsel does.
			 */
			selec = networkjoinsel_inner(operator, &vardata1, &vardata2);
			break;
		case JOIN_SEMI:
		case JOIN_ANTI:
			/* Here, it's important that we pass the outer var on the left. */
			if (!join_is_reversed)
				selec = networkjoinsel_semi(operator, &vardata1, &vardata2);
			else
				selec = networkjoinsel_semi(get_commutator(operator),
											&vardata2, &vardata1);
			break;
		default:
			/* other values not expected here */
			elog(ERROR, "unrecognized join type: %d",
				 (int) sjinfo->jointype);
			selec = 0;			/* keep compiler quiet */
			break;
	}

	ReleaseVariableStats(vardata1);
	ReleaseVariableStats(vardata2);

	CLAMP_PROBABILITY(selec);

	PG_RETURN_FLOAT8((float8) selec);
}

/*
 * Inner join selectivity estimation for subnet inclusion/overlap operators
 *
 * Calculates MCV vs MCV, MCV vs histogram and histogram vs histogram
 * selectivity for join using the subnet inclusion operators.  Unlike the
 * join selectivity function for the equality operator, eqjoinsel_inner(),
 * one to one matching of the values is not enough.  Network inclusion
 * operators are likely to match many to many, so we must check all pairs.
 * (Note: it might be possible to exploit understanding of the histogram's
 * btree ordering to reduce the work needed, but we don't currently try.)
 * Also, MCV vs histogram selectivity is not neglected as in eqjoinsel_inner().
 */
static Selectivity
networkjoinsel_inner(Oid operator,
					 VariableStatData *vardata1, VariableStatData *vardata2)
{
	Form_pg_statistic stats;
	double		nullfrac1 = 0.0,
				nullfrac2 = 0.0;
	Selectivity selec = 0.0,
				sumcommon1 = 0.0,
				sumcommon2 = 0.0;
	bool		mcv1_exists = false,
				mcv2_exists = false,
				hist1_exists = false,
				hist2_exists = false;
	int			opr_codenum;
	int			mcv1_nvalues,
				mcv2_nvalues,
				mcv1_nnumbers,
				mcv2_nnumbers,
				hist1_nvalues,
				hist2_nvalues,
				mcv1_length = 0,
				mcv2_length = 0;
	Datum	   *mcv1_values,
			   *mcv2_values,
			   *hist1_values,
			   *hist2_values;
	float4	   *mcv1_numbers,
			   *mcv2_numbers;

	if (HeapTupleIsValid(vardata1->statsTuple))
	{
		stats = (Form_pg_statistic) GETSTRUCT(vardata1->statsTuple);
		nullfrac1 = stats->stanullfrac;

		mcv1_exists = get_attstatsslot(vardata1->statsTuple,
									   vardata1->atttype, vardata1->atttypmod,
									   STATISTIC_KIND_MCV, InvalidOid,
									   NULL,
									   &mcv1_values, &mcv1_nvalues,
									   &mcv1_numbers, &mcv1_nnumbers);
		hist1_exists = get_attstatsslot(vardata1->statsTuple,
									  vardata1->atttype, vardata1->atttypmod,
										STATISTIC_KIND_HISTOGRAM, InvalidOid,
										NULL,
										&hist1_values, &hist1_nvalues,
										NULL, NULL);
		/* Arbitrarily limit number of MCVs considered */
		mcv1_length = Min(mcv1_nvalues, MAX_CONSIDERED_ELEMS);
		if (mcv1_exists)
			sumcommon1 = mcv_population(mcv1_numbers, mcv1_length);
	}

	if (HeapTupleIsValid(vardata2->statsTuple))
	{
		stats = (Form_pg_statistic) GETSTRUCT(vardata2->statsTuple);
		nullfrac2 = stats->stanullfrac;

		mcv2_exists = get_attstatsslot(vardata2->statsTuple,
									   vardata2->atttype, vardata2->atttypmod,
									   STATISTIC_KIND_MCV, InvalidOid,
									   NULL,
									   &mcv2_values, &mcv2_nvalues,
									   &mcv2_numbers, &mcv2_nnumbers);
		hist2_exists = get_attstatsslot(vardata2->statsTuple,
									  vardata2->atttype, vardata2->atttypmod,
										STATISTIC_KIND_HISTOGRAM, InvalidOid,
										NULL,
										&hist2_values, &hist2_nvalues,
										NULL, NULL);
		/* Arbitrarily limit number of MCVs considered */
		mcv2_length = Min(mcv2_nvalues, MAX_CONSIDERED_ELEMS);
		if (mcv2_exists)
			sumcommon2 = mcv_population(mcv2_numbers, mcv2_length);
	}

	opr_codenum = inet_opr_codenum(operator);

	/*
	 * Calculate selectivity for MCV vs MCV matches.
	 */
	if (mcv1_exists && mcv2_exists)
		selec += inet_mcv_join_sel(mcv1_values, mcv1_numbers, mcv1_length,
								   mcv2_values, mcv2_numbers, mcv2_length,
								   operator);

	/*
	 * Add in selectivities for MCV vs histogram matches, scaling according to
	 * the fractions of the populations represented by the histograms. Note
	 * that the second case needs to commute the operator.
	 */
	if (mcv1_exists && hist2_exists)
		selec += (1.0 - nullfrac2 - sumcommon2) *
			inet_mcv_hist_sel(mcv1_values, mcv1_numbers, mcv1_length,
							  hist2_values, hist2_nvalues,
							  opr_codenum);
	if (mcv2_exists && hist1_exists)
		selec += (1.0 - nullfrac1 - sumcommon1) *
			inet_mcv_hist_sel(mcv2_values, mcv2_numbers, mcv2_length,
							  hist1_values, hist1_nvalues,
							  -opr_codenum);

	/*
	 * Add in selectivity for histogram vs histogram matches, again scaling
	 * appropriately.
	 */
	if (hist1_exists && hist2_exists)
		selec += (1.0 - nullfrac1 - sumcommon1) *
			(1.0 - nullfrac2 - sumcommon2) *
			inet_hist_inclusion_join_sel(hist1_values, hist1_nvalues,
										 hist2_values, hist2_nvalues,
										 opr_codenum);

	/*
	 * If useful statistics are not available then use the default estimate.
	 * We can apply null fractions if known, though.
	 */
	if ((!mcv1_exists && !hist1_exists) || (!mcv2_exists && !hist2_exists))
		selec = (1.0 - nullfrac1) * (1.0 - nullfrac2) * DEFAULT_SEL(operator);

	/* Release stats. */
	if (mcv1_exists)
		free_attstatsslot(vardata1->atttype, mcv1_values, mcv1_nvalues,
						  mcv1_numbers, mcv1_nnumbers);
	if (mcv2_exists)
		free_attstatsslot(vardata2->atttype, mcv2_values, mcv2_nvalues,
						  mcv2_numbers, mcv2_nnumbers);
	if (hist1_exists)
		free_attstatsslot(vardata1->atttype, hist1_values, hist1_nvalues,
						  NULL, 0);
	if (hist2_exists)
		free_attstatsslot(vardata2->atttype, hist2_values, hist2_nvalues,
						  NULL, 0);

	return selec;
}

/*
 * Semi join selectivity estimation for subnet inclusion/overlap operators
 *
 * Calculates MCV vs MCV, MCV vs histogram, histogram vs MCV, and histogram vs
 * histogram selectivity for semi/anti join cases.
 */
static Selectivity
networkjoinsel_semi(Oid operator,
					VariableStatData *vardata1, VariableStatData *vardata2)
{
	Form_pg_statistic stats;
	Selectivity selec = 0.0,
				sumcommon1 = 0.0,
				sumcommon2 = 0.0;
	double		nullfrac1 = 0.0,
				nullfrac2 = 0.0,
				hist2_weight = 0.0;
	bool		mcv1_exists = false,
				mcv2_exists = false,
				hist1_exists = false,
				hist2_exists = false;
	int			opr_codenum;
	FmgrInfo	proc;
	int			i,
				mcv1_nvalues,
				mcv2_nvalues,
				mcv1_nnumbers,
				mcv2_nnumbers,
				hist1_nvalues,
				hist2_nvalues,
				mcv1_length = 0,
				mcv2_length = 0;
	Datum	   *mcv1_values,
			   *mcv2_values,
			   *hist1_values,
			   *hist2_values;
	float4	   *mcv1_numbers,
			   *mcv2_numbers;

	if (HeapTupleIsValid(vardata1->statsTuple))
	{
		stats = (Form_pg_statistic) GETSTRUCT(vardata1->statsTuple);
		nullfrac1 = stats->stanullfrac;

		mcv1_exists = get_attstatsslot(vardata1->statsTuple,
									   vardata1->atttype, vardata1->atttypmod,
									   STATISTIC_KIND_MCV, InvalidOid,
									   NULL,
									   &mcv1_values, &mcv1_nvalues,
									   &mcv1_numbers, &mcv1_nnumbers);
		hist1_exists = get_attstatsslot(vardata1->statsTuple,
									  vardata1->atttype, vardata1->atttypmod,
										STATISTIC_KIND_HISTOGRAM, InvalidOid,
										NULL,
										&hist1_values, &hist1_nvalues,
										NULL, NULL);
		/* Arbitrarily limit number of MCVs considered */
		mcv1_length = Min(mcv1_nvalues, MAX_CONSIDERED_ELEMS);
		if (mcv1_exists)
			sumcommon1 = mcv_population(mcv1_numbers, mcv1_length);
	}

	if (HeapTupleIsValid(vardata2->statsTuple))
	{
		stats = (Form_pg_statistic) GETSTRUCT(vardata2->statsTuple);
		nullfrac2 = stats->stanullfrac;

		mcv2_exists = get_attstatsslot(vardata2->statsTuple,
									   vardata2->atttype, vardata2->atttypmod,
									   STATISTIC_KIND_MCV, InvalidOid,
									   NULL,
									   &mcv2_values, &mcv2_nvalues,
									   &mcv2_numbers, &mcv2_nnumbers);
		hist2_exists = get_attstatsslot(vardata2->statsTuple,
									  vardata2->atttype, vardata2->atttypmod,
										STATISTIC_KIND_HISTOGRAM, InvalidOid,
										NULL,
										&hist2_values, &hist2_nvalues,
										NULL, NULL);
		/* Arbitrarily limit number of MCVs considered */
		mcv2_length = Min(mcv2_nvalues, MAX_CONSIDERED_ELEMS);
		if (mcv2_exists)
			sumcommon2 = mcv_population(mcv2_numbers, mcv2_length);
	}

	opr_codenum = inet_opr_codenum(operator);
	fmgr_info(get_opcode(operator), &proc);

	/* Estimate number of input rows represented by RHS histogram. */
	if (hist2_exists && vardata2->rel)
		hist2_weight = (1.0 - nullfrac2 - sumcommon2) * vardata2->rel->rows;

	/*
	 * Consider each element of the LHS MCV list, matching it to whatever RHS
	 * stats we have.  Scale according to the known frequency of the MCV.
	 */
	if (mcv1_exists && (mcv2_exists || hist2_exists))
	{
		for (i = 0; i < mcv1_length; i++)
		{
			selec += mcv1_numbers[i] *
				inet_semi_join_sel(mcv1_values[i],
								   mcv2_exists, mcv2_values, mcv2_length,
								   hist2_exists, hist2_values, hist2_nvalues,
								   hist2_weight,
								   &proc, opr_codenum);
		}
	}

	/*
	 * Consider each element of the LHS histogram, except for the first and
	 * last elements, which we exclude on the grounds that they're outliers
	 * and thus not very representative.  Scale on the assumption that each
	 * such histogram element represents an equal share of the LHS histogram
	 * population (which is a bit bogus, because the members of its bucket may
	 * not all act the same with respect to the join clause, but it's hard to
	 * do better).
	 *
	 * If there are too many histogram elements, decimate to limit runtime.
	 */
	if (hist1_exists && hist1_nvalues > 2 && (mcv2_exists || hist2_exists))
	{
		double		hist_selec_sum = 0.0;
		int			k,
					n;

		k = (hist1_nvalues - 3) / MAX_CONSIDERED_ELEMS + 1;

		n = 0;
		for (i = 1; i < hist1_nvalues - 1; i += k)
		{
			hist_selec_sum +=
				inet_semi_join_sel(hist1_values[i],
								   mcv2_exists, mcv2_values, mcv2_length,
								   hist2_exists, hist2_values, hist2_nvalues,
								   hist2_weight,
								   &proc, opr_codenum);
			n++;
		}

		selec += (1.0 - nullfrac1 - sumcommon1) * hist_selec_sum / n;
	}

	/*
	 * If useful statistics are not available then use the default estimate.
	 * We can apply null fractions if known, though.
	 */
	if ((!mcv1_exists && !hist1_exists) || (!mcv2_exists && !hist2_exists))
		selec = (1.0 - nullfrac1) * (1.0 - nullfrac2) * DEFAULT_SEL(operator);

	/* Release stats. */
	if (mcv1_exists)
		free_attstatsslot(vardata1->atttype, mcv1_values, mcv1_nvalues,
						  mcv1_numbers, mcv1_nnumbers);
	if (mcv2_exists)
		free_attstatsslot(vardata2->atttype, mcv2_values, mcv2_nvalues,
						  mcv2_numbers, mcv2_nnumbers);
	if (hist1_exists)
		free_attstatsslot(vardata1->atttype, hist1_values, hist1_nvalues,
						  NULL, 0);
	if (hist2_exists)
		free_attstatsslot(vardata2->atttype, hist2_values, hist2_nvalues,
						  NULL, 0);

	return selec;
}

/*
 * Compute the fraction of a relation's population that is represented
 * by the MCV list.
 */
static Selectivity
mcv_population(float4 *mcv_numbers, int mcv_nvalues)
{
	Selectivity sumcommon = 0.0;
	int			i;

	for (i = 0; i < mcv_nvalues; i++)
	{
		sumcommon += mcv_numbers[i];
	}

	return sumcommon;
}

/*
 * Inet histogram vs single value selectivity estimation
 *
 * Estimate the fraction of the histogram population that satisfies
 * "value OPR CONST".  (The result needs to be scaled to reflect the
 * proportion of the total population represented by the histogram.)
 *
 * The histogram is originally for the inet btree comparison operators.
 * Only the common bits of the network part and the length of the network part
 * (masklen) are interesting for the subnet inclusion operators.  Fortunately,
 * btree comparison treats the network part as the major sort key.  Even so,
 * the length of the network part would not really be significant in the
 * histogram.  This would lead to big mistakes for data sets with uneven
 * masklen distribution.  To reduce this problem, comparisons with the left
 * and the right sides of the buckets are used together.
 *
 * Histogram bucket matches are calculated in two forms.  If the constant
 * matches both bucket endpoints the bucket is considered as fully matched.
 * The second form is to match the bucket partially; we recognize this when
 * the constant matches just one endpoint, or the two endpoints fall on
 * opposite sides of the constant.  (Note that when the constant matches an
 * interior histogram element, it gets credit for partial matches to the
 * buckets on both sides, while a match to a histogram endpoint gets credit
 * for only one partial match.  This is desirable.)
 *
 * The divider in the partial bucket match is imagined as the distance
 * between the decisive bits and the common bits of the addresses.  It will
 * be used as a power of two as it is the natural scale for the IP network
 * inclusion.  This partial bucket match divider calculation is an empirical
 * formula and subject to change with more experiment.
 *
 * For a partial match, we try to calculate dividers for both of the
 * boundaries.  If the address family of a boundary value does not match the
 * constant or comparison of the length of the network parts is not correct
 * for the operator, the divider for that boundary will not be taken into
 * account.  If both of the dividers are valid, the greater one will be used
 * to minimize the mistake in buckets that have disparate masklens.  This
 * calculation is unfair when dividers can be calculated for both of the
 * boundaries but they are far from each other; but it is not a common
 * situation as the boundaries are expected to share most of their significant
 * bits of their masklens.  The mistake would be greater, if we would use the
 * minimum instead of the maximum, and we don't know a sensible way to combine
 * them.
 *
 * For partial match in buckets that have different address families on the
 * left and right sides, only the boundary with the same address family is
 * taken into consideration.  This can cause more mistakes for these buckets
 * if the masklens of their boundaries are also disparate.  But this can only
 * happen in one bucket, since only two address families exist.  It seems a
 * better option than not considering these buckets at all.
 */
static Selectivity
inet_hist_value_sel(Datum *values, int nvalues, Datum constvalue,
					int opr_codenum)
{
	Selectivity match = 0.0;
	inet	   *query,
			   *left,
			   *right;
	int			i,
				k,
				n;
	int			left_order,
				right_order,
				left_divider,
				right_divider;

	/* guard against zero-divide below */
	if (nvalues <= 1)
		return 0.0;

	/* if there are too many histogram elements, decimate to limit runtime */
	k = (nvalues - 2) / MAX_CONSIDERED_ELEMS + 1;

	query = DatumGetInetPP(constvalue);

	/* "left" is the left boundary value of the current bucket ... */
	left = DatumGetInetPP(values[0]);
	left_order = inet_inclusion_cmp(left, query, opr_codenum);

	n = 0;
	for (i = k; i < nvalues; i += k)
	{
		/* ... and "right" is the right boundary value */
		right = DatumGetInetPP(values[i]);
		right_order = inet_inclusion_cmp(right, query, opr_codenum);

		if (left_order == 0 && right_order == 0)
		{
			/* The whole bucket matches, since both endpoints do. */
			match += 1.0;
		}
		else if ((left_order <= 0 && right_order >= 0) ||
				 (left_order >= 0 && right_order <= 0))
		{
			/* Partial bucket match. */
			left_divider = inet_hist_match_divider(left, query, opr_codenum);
			right_divider = inet_hist_match_divider(right, query, opr_codenum);

			if (left_divider >= 0 || right_divider >= 0)
				match += 1.0 / pow(2.0, Max(left_divider, right_divider));
		}

		/* Shift the variables. */
		left = right;
		left_order = right_order;

		/* Count the number of buckets considered. */
		n++;
	}

	return match / n;
}

/*
 * Inet MCV vs MCV join selectivity estimation
 *
 * We simply add up the fractions of the populations that satisfy the clause.
 * The result is exact and does not need to be scaled further.
 */
static Selectivity
inet_mcv_join_sel(Datum *mcv1_values, float4 *mcv1_numbers, int mcv1_nvalues,
				  Datum *mcv2_values, float4 *mcv2_numbers, int mcv2_nvalues,
				  Oid operator)
{
	Selectivity selec = 0.0;
	FmgrInfo	proc;
	int			i,
				j;

	fmgr_info(get_opcode(operator), &proc);

	for (i = 0; i < mcv1_nvalues; i++)
	{
		for (j = 0; j < mcv2_nvalues; j++)
			if (DatumGetBool(FunctionCall2(&proc,
										   mcv1_values[i],
										   mcv2_values[j])))
				selec += mcv1_numbers[i] * mcv2_numbers[j];
	}
	return selec;
}

/*
 * Inet MCV vs histogram join selectivity estimation
 *
 * For each MCV on the lefthand side, estimate the fraction of the righthand's
 * histogram population that satisfies the join clause, and add those up,
 * scaling by the MCV's frequency.  The result still needs to be scaled
 * according to the fraction of the righthand's population represented by
 * the histogram.
 */
static Selectivity
inet_mcv_hist_sel(Datum *mcv_values, float4 *mcv_numbers, int mcv_nvalues,
				  Datum *hist_values, int hist_nvalues,
				  int opr_codenum)
{
	Selectivity selec = 0.0;
	int			i;

	/*
	 * We'll call inet_hist_value_selec with the histogram on the left, so we
	 * must commute the operator.
	 */
	opr_codenum = -opr_codenum;

	for (i = 0; i < mcv_nvalues; i++)
	{
		selec += mcv_numbers[i] *
			inet_hist_value_sel(hist_values, hist_nvalues, mcv_values[i],
								opr_codenum);
	}
	return selec;
}

/*
 * Inet histogram vs histogram join selectivity estimation
 *
 * Here, we take all values listed in the second histogram (except for the
 * first and last elements, which are excluded on the grounds of possibly
 * not being very representative) and treat them as a uniform sample of
 * the non-MCV population for that relation.  For each one, we apply
 * inet_hist_value_selec to see what fraction of the first histogram
 * it matches.
 *
 * We could alternatively do this the other way around using the operator's
 * commutator.  XXX would it be worthwhile to do it both ways and take the
 * average?  That would at least avoid non-commutative estimation results.
 */
static Selectivity
inet_hist_inclusion_join_sel(Datum *hist1_values, int hist1_nvalues,
							 Datum *hist2_values, int hist2_nvalues,
							 int opr_codenum)
{
	double		match = 0.0;
	int			i,
				k,
				n;

	if (hist2_nvalues <= 2)
		return 0.0;				/* no interior histogram elements */

	/* if there are too many histogram elements, decimate to limit runtime */
	k = (hist2_nvalues - 3) / MAX_CONSIDERED_ELEMS + 1;

	n = 0;
	for (i = 1; i < hist2_nvalues - 1; i += k)
	{
		match += inet_hist_value_sel(hist1_values, hist1_nvalues,
									 hist2_values[i], opr_codenum);
		n++;
	}

	return match / n;
}

/*
 * Inet semi join selectivity estimation for one value
 *
 * The function calculates the probability that there is at least one row
 * in the RHS table that satisfies the "lhs_value op column" condition.
 * It is used in semi join estimation to check a sample from the left hand
 * side table.
 *
 * The MCV and histogram from the right hand side table should be provided as
 * arguments with the lhs_value from the left hand side table for the join.
 * hist_weight is the total number of rows represented by the histogram.
 * For example, if the table has 1000 rows, and 10% of the rows are in the MCV
 * list, and another 10% are NULLs, hist_weight would be 800.
 *
 * First, the lhs_value will be matched to the most common values.  If it
 * matches any of them, 1.0 will be returned, because then there is surely
 * a match.
 *
 * Otherwise, the histogram will be used to estimate the number of rows in
 * the second table that match the condition.  If the estimate is greater
 * than 1.0, 1.0 will be returned, because it means there is a greater chance
 * that the lhs_value will match more than one row in the table.  If it is
 * between 0.0 and 1.0, it will be returned as the probability.
 */
static Selectivity
inet_semi_join_sel(Datum lhs_value,
				   bool mcv_exists, Datum *mcv_values, int mcv_nvalues,
				   bool hist_exists, Datum *hist_values, int hist_nvalues,
				   double hist_weight,
				   FmgrInfo *proc, int opr_codenum)
{
	if (mcv_exists)
	{
		int			i;

		for (i = 0; i < mcv_nvalues; i++)
		{
			if (DatumGetBool(FunctionCall2(proc,
										   lhs_value,
										   mcv_values[i])))
				return 1.0;
		}
	}

	if (hist_exists && hist_weight > 0)
	{
		Selectivity hist_selec;

		/* Commute operator, since we're passing lhs_value on the right */
		hist_selec = inet_hist_value_sel(hist_values, hist_nvalues,
										 lhs_value, -opr_codenum);

		if (hist_selec > 0)
			return Min(1.0, hist_weight * hist_selec);
	}

	return 0.0;
}

/*
 * Assign useful code numbers for the subnet inclusion/overlap operators
 *
 * Only inet_masklen_inclusion_cmp() and inet_hist_match_divider() depend
 * on the exact codes assigned here; but many other places in this file
 * know that they can negate a code to obtain the code for the commutator
 * operator.
 */
static int
inet_opr_codenum(Oid operator)
{
	switch (operator)
	{
		case OID_INET_SUP_OP:
			return -2;
		case OID_INET_SUPEQ_OP:
			return -1;
		case OID_INET_OVERLAP_OP:
			return 0;
		case OID_INET_SUBEQ_OP:
			return 1;
		case OID_INET_SUB_OP:
			return 2;
		default:
			elog(ERROR, "unrecognized operator %u for inet selectivity",
				 operator);
	}
	return 0;					/* unreached, but keep compiler quiet */
}

/*
 * Comparison function for the subnet inclusion/overlap operators
 *
 * If the comparison is okay for the specified inclusion operator, the return
 * value will be 0.  Otherwise the return value will be less than or greater
 * than 0 as appropriate for the operator.
 *
 * Comparison is compatible with the basic comparison function for the inet
 * type.  See network_cmp_internal() in network.c for the original.  Basic
 * comparison operators are implemented with the network_cmp_internal()
 * function.  It is possible to implement the subnet inclusion operators with
 * this function.
 *
 * Comparison is first on the common bits of the network part, then on the
 * length of the network part (masklen) as in the network_cmp_internal()
 * function.  Only the first part is in this function.  The second part is
 * separated to another function for reusability.  The difference between the
 * second part and the original network_cmp_internal() is that the inclusion
 * operator is considered while comparing the lengths of the network parts.
 * See the inet_masklen_inclusion_cmp() function below.
 */
static int
inet_inclusion_cmp(inet *left, inet *right, int opr_codenum)
{
	if (ip_family(left) == ip_family(right))
	{
		int			order;

		order = bitncmp(ip_addr(left), ip_addr(right),
						Min(ip_bits(left), ip_bits(right)));
		if (order != 0)
			return order;

		return inet_masklen_inclusion_cmp(left, right, opr_codenum);
	}

	return ip_family(left) - ip_family(right);
}

/*
 * Masklen comparison function for the subnet inclusion/overlap operators
 *
 * Compares the lengths of the network parts of the inputs.  If the comparison
 * is okay for the specified inclusion operator, the return value will be 0.
 * Otherwise the return value will be less than or greater than 0 as
 * appropriate for the operator.
 */
static int
inet_masklen_inclusion_cmp(inet *left, inet *right, int opr_codenum)
{
	int			order;

	order = (int) ip_bits(left) - (int) ip_bits(right);

	/*
	 * Return 0 if the operator would accept this combination of masklens.
	 * Note that opr_codenum zero (overlaps) will accept all cases.
	 */
	if ((order > 0 && opr_codenum >= 0) ||
		(order == 0 && opr_codenum >= -1 && opr_codenum <= 1) ||
		(order < 0 && opr_codenum <= 0))
		return 0;

	/*
	 * Otherwise, return a negative value for sup/supeq (notionally, the RHS
	 * needs to have a larger masklen than it has, which would make it sort
	 * later), or a positive value for sub/subeq (vice versa).
	 */
	return opr_codenum;
}

/*
 * Inet histogram partial match divider calculation
 *
 * First the families and the lengths of the network parts are compared using
 * the subnet inclusion operator.  If those are acceptable for the operator,
 * the divider will be calculated using the masklens and the common bits of
 * the addresses.  -1 will be returned if it cannot be calculated.
 *
 * See commentary for inet_hist_value_sel() for some rationale for this.
 */
static int
inet_hist_match_divider(inet *boundary, inet *query, int opr_codenum)
{
	if (ip_family(boundary) == ip_family(query) &&
		inet_masklen_inclusion_cmp(boundary, query, opr_codenum) == 0)
	{
		int			min_bits,
					decisive_bits;

		min_bits = Min(ip_bits(boundary), ip_bits(query));

		/*
		 * Set decisive_bits to the masklen of the one that should contain the
		 * other according to the operator.
		 */
		if (opr_codenum < 0)
			decisive_bits = ip_bits(boundary);
		else if (opr_codenum > 0)
			decisive_bits = ip_bits(query);
		else
			decisive_bits = min_bits;

		/*
		 * Now return the number of non-common decisive bits.  (This will be
		 * zero if the boundary and query in fact match, else positive.)
		 */
		if (min_bits > 0)
			return decisive_bits - bitncommon(ip_addr(boundary),
											  ip_addr(query),
											  min_bits);
		return decisive_bits;
	}

	return -1;
}
