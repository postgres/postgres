/*-------------------------------------------------------------------------
 *
 * _int_selfuncs.c
 *	  Functions for selectivity estimation of intarray operators
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  contrib/intarray/_int_selfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "_int.h"
#include "access/htup_details.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"

PG_FUNCTION_INFO_V1(_int_overlap_sel);
PG_FUNCTION_INFO_V1(_int_contains_sel);
PG_FUNCTION_INFO_V1(_int_contained_sel);
PG_FUNCTION_INFO_V1(_int_overlap_joinsel);
PG_FUNCTION_INFO_V1(_int_contains_joinsel);
PG_FUNCTION_INFO_V1(_int_contained_joinsel);
PG_FUNCTION_INFO_V1(_int_matchsel);


static Selectivity int_query_opr_selec(ITEM *item, Datum *values, float4 *freqs,
									   int nmncelems, float4 minfreq);
static int	compare_val_int4(const void *a, const void *b);

/*
 * Wrappers around the default array selectivity estimation functions.
 *
 * The default array selectivity operators for the @>, && and @< operators
 * work fine for integer arrays. However, if we tried to just use arraycontsel
 * and arraycontjoinsel directly as the cost estimator functions for our
 * operators, they would not work as intended, because they look at the
 * operator's OID. Our operators behave exactly like the built-in anyarray
 * versions, but we must tell the cost estimator functions which built-in
 * operators they correspond to. These wrappers just replace the operator
 * OID with the corresponding built-in operator's OID, and call the built-in
 * function.
 */

Datum
_int_overlap_sel(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall4(arraycontsel,
										PG_GETARG_DATUM(0),
										ObjectIdGetDatum(OID_ARRAY_OVERLAP_OP),
										PG_GETARG_DATUM(2),
										PG_GETARG_DATUM(3)));
}

Datum
_int_contains_sel(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall4(arraycontsel,
										PG_GETARG_DATUM(0),
										ObjectIdGetDatum(OID_ARRAY_CONTAINS_OP),
										PG_GETARG_DATUM(2),
										PG_GETARG_DATUM(3)));
}

Datum
_int_contained_sel(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall4(arraycontsel,
										PG_GETARG_DATUM(0),
										ObjectIdGetDatum(OID_ARRAY_CONTAINED_OP),
										PG_GETARG_DATUM(2),
										PG_GETARG_DATUM(3)));
}

Datum
_int_overlap_joinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall5(arraycontjoinsel,
										PG_GETARG_DATUM(0),
										ObjectIdGetDatum(OID_ARRAY_OVERLAP_OP),
										PG_GETARG_DATUM(2),
										PG_GETARG_DATUM(3),
										PG_GETARG_DATUM(4)));
}

Datum
_int_contains_joinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall5(arraycontjoinsel,
										PG_GETARG_DATUM(0),
										ObjectIdGetDatum(OID_ARRAY_CONTAINS_OP),
										PG_GETARG_DATUM(2),
										PG_GETARG_DATUM(3),
										PG_GETARG_DATUM(4)));
}

Datum
_int_contained_joinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall5(arraycontjoinsel,
										PG_GETARG_DATUM(0),
										ObjectIdGetDatum(OID_ARRAY_CONTAINED_OP),
										PG_GETARG_DATUM(2),
										PG_GETARG_DATUM(3),
										PG_GETARG_DATUM(4)));
}


/*
 * _int_matchsel -- restriction selectivity function for intarray @@ query_int
 */
Datum
_int_matchsel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);

	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;
	Selectivity selec;
	QUERYTYPE  *query;
	Datum	   *mcelems = NULL;
	float4	   *mcefreqs = NULL;
	int			nmcelems = 0;
	float4		minfreq = 0.0;
	float4		nullfrac = 0.0;
	AttStatsSlot sslot;

	/*
	 * If expression is not "variable @@ something" or "something @@ variable"
	 * then punt and return a default estimate.
	 */
	if (!get_restriction_variable(root, args, varRelid,
								  &vardata, &other, &varonleft))
		PG_RETURN_FLOAT8(DEFAULT_EQ_SEL);

	/*
	 * Variable should be int[]. We don't support cases where variable is
	 * query_int.
	 */
	if (vardata.vartype != INT4ARRAYOID)
		PG_RETURN_FLOAT8(DEFAULT_EQ_SEL);

	/*
	 * Can't do anything useful if the something is not a constant, either.
	 */
	if (!IsA(other, Const))
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(DEFAULT_EQ_SEL);
	}

	/*
	 * The "@@" operator is strict, so we can cope with NULL right away.
	 */
	if (((Const *) other)->constisnull)
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(0.0);
	}

	/* The caller made sure the const is a query, so get it now */
	query = DatumGetQueryTypeP(((Const *) other)->constvalue);

	/* Empty query matches nothing */
	if (query->size == 0)
	{
		ReleaseVariableStats(vardata);
		return (Selectivity) 0.0;
	}

	/*
	 * Get the statistics for the intarray column.
	 *
	 * We're interested in the Most-Common-Elements list, and the NULL
	 * fraction.
	 */
	if (HeapTupleIsValid(vardata.statsTuple))
	{
		Form_pg_statistic stats;

		stats = (Form_pg_statistic) GETSTRUCT(vardata.statsTuple);
		nullfrac = stats->stanullfrac;

		/*
		 * For an int4 array, the default array type analyze function will
		 * collect a Most Common Elements list, which is an array of int4s.
		 */
		if (get_attstatsslot(&sslot, vardata.statsTuple,
							 STATISTIC_KIND_MCELEM, InvalidOid,
							 ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS))
		{
			Assert(sslot.valuetype == INT4OID);

			/*
			 * There should be three more Numbers than Values, because the
			 * last three (for intarray) cells are taken for minimal, maximal
			 * and nulls frequency. Punt if not.
			 */
			if (sslot.nnumbers == sslot.nvalues + 3)
			{
				/* Grab the lowest frequency. */
				minfreq = sslot.numbers[sslot.nnumbers - (sslot.nnumbers - sslot.nvalues)];

				mcelems = sslot.values;
				mcefreqs = sslot.numbers;
				nmcelems = sslot.nvalues;
			}
		}
	}
	else
		memset(&sslot, 0, sizeof(sslot));

	/* Process the logical expression in the query, using the stats */
	selec = int_query_opr_selec(GETQUERY(query) + query->size - 1,
								mcelems, mcefreqs, nmcelems, minfreq);

	/* MCE stats count only non-null rows, so adjust for null rows. */
	selec *= (1.0 - nullfrac);

	free_attstatsslot(&sslot);
	ReleaseVariableStats(vardata);

	CLAMP_PROBABILITY(selec);

	PG_RETURN_FLOAT8((float8) selec);
}

/*
 * Estimate selectivity of single intquery operator
 */
static Selectivity
int_query_opr_selec(ITEM *item, Datum *mcelems, float4 *mcefreqs,
					int nmcelems, float4 minfreq)
{
	Selectivity selec;

	/* since this function recurses, it could be driven to stack overflow */
	check_stack_depth();

	if (item->type == VAL)
	{
		Datum	   *searchres;

		if (mcelems == NULL)
			return (Selectivity) DEFAULT_EQ_SEL;

		searchres = (Datum *) bsearch(&item->val, mcelems, nmcelems,
									  sizeof(Datum), compare_val_int4);
		if (searchres)
		{
			/*
			 * The element is in MCELEM.  Return precise selectivity (or at
			 * least as precise as ANALYZE could find out).
			 */
			selec = mcefreqs[searchres - mcelems];
		}
		else
		{
			/*
			 * The element is not in MCELEM.  Punt, but assume that the
			 * selectivity cannot be more than minfreq / 2.
			 */
			selec = Min(DEFAULT_EQ_SEL, minfreq / 2);
		}
	}
	else if (item->type == OPR)
	{
		/* Current query node is an operator */
		Selectivity s1,
					s2;

		s1 = int_query_opr_selec(item - 1, mcelems, mcefreqs, nmcelems,
								 minfreq);
		switch (item->val)
		{
			case (int32) '!':
				selec = 1.0 - s1;
				break;

			case (int32) '&':
				s2 = int_query_opr_selec(item + item->left, mcelems, mcefreqs,
										 nmcelems, minfreq);
				selec = s1 * s2;
				break;

			case (int32) '|':
				s2 = int_query_opr_selec(item + item->left, mcelems, mcefreqs,
										 nmcelems, minfreq);
				selec = s1 + s2 - s1 * s2;
				break;

			default:
				elog(ERROR, "unrecognized operator: %d", item->val);
				selec = 0;		/* keep compiler quiet */
				break;
		}
	}
	else
	{
		elog(ERROR, "unrecognized int query item type: %u", item->type);
		selec = 0;				/* keep compiler quiet */
	}

	/* Clamp intermediate results to stay sane despite roundoff error */
	CLAMP_PROBABILITY(selec);

	return selec;
}

/*
 * Comparison function for binary search in mcelem array.
 */
static int
compare_val_int4(const void *a, const void *b)
{
	int32		key = *(int32 *) a;
	const Datum *t = (const Datum *) b;

	return key - DatumGetInt32(*t);
}
