/*-------------------------------------------------------------------------
 *
 * orderedsetaggs.c
 *		Ordered-set aggregate functions.
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/orderedsetaggs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_aggregate.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/tlist.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/timestamp.h"
#include "utils/tuplesort.h"


/*
 * Generic support for ordered-set aggregates
 */

typedef struct OrderedSetAggState
{
	/* Aggref for this aggregate: */
	Aggref	   *aggref;
	/* Sort object we're accumulating data in: */
	Tuplesortstate *sortstate;
	/* Number of normal rows inserted into sortstate: */
	int64		number_of_rows;

	/* These fields are used only when accumulating tuples: */

	/* Tuple descriptor for tuples inserted into sortstate: */
	TupleDesc	tupdesc;
	/* Tuple slot we can use for inserting/extracting tuples: */
	TupleTableSlot *tupslot;

	/* These fields are used only when accumulating datums: */

	/* Info about datatype of datums being sorted: */
	Oid			datumtype;
	int16		typLen;
	bool		typByVal;
	char		typAlign;
	/* Info about equality operator associated with sort operator: */
	Oid			eqOperator;
} OrderedSetAggState;

static void ordered_set_shutdown(Datum arg);


/*
 * Set up working state for an ordered-set aggregate
 */
static OrderedSetAggState *
ordered_set_startup(FunctionCallInfo fcinfo, bool use_tuples)
{
	OrderedSetAggState *osastate;
	Aggref	   *aggref;
	ExprContext *peraggecontext;
	MemoryContext aggcontext;
	MemoryContext oldcontext;
	List	   *sortlist;
	int			numSortCols;

	/* Must be called as aggregate; get the Agg node's query-lifespan context */
	if (AggCheckCallContext(fcinfo, &aggcontext) != AGG_CONTEXT_AGGREGATE)
		elog(ERROR, "ordered-set aggregate called in non-aggregate context");
	/* Need the Aggref as well */
	aggref = AggGetAggref(fcinfo);
	if (!aggref)
		elog(ERROR, "ordered-set aggregate called in non-aggregate context");
	if (!AGGKIND_IS_ORDERED_SET(aggref->aggkind))
		elog(ERROR, "ordered-set aggregate support function called for non-ordered-set aggregate");
	/* Also get output exprcontext so we can register shutdown callback */
	peraggecontext = AggGetPerAggEContext(fcinfo);
	if (!peraggecontext)
		elog(ERROR, "ordered-set aggregate called in non-aggregate context");

	/* Initialize working-state object in the aggregate-lifespan context */
	osastate = (OrderedSetAggState *)
		MemoryContextAllocZero(aggcontext, sizeof(OrderedSetAggState));
	osastate->aggref = aggref;

	/* Extract the sort information */
	sortlist = aggref->aggorder;
	numSortCols = list_length(sortlist);

	if (use_tuples)
	{
		bool		ishypothetical = (aggref->aggkind == AGGKIND_HYPOTHETICAL);
		AttrNumber *sortColIdx;
		Oid		   *sortOperators;
		Oid		   *sortCollations;
		bool	   *sortNullsFirst;
		ListCell   *lc;
		int			i;

		if (ishypothetical)
			numSortCols++;		/* make space for flag column */
		/* these arrays are made in short-lived context */
		sortColIdx = (AttrNumber *) palloc(numSortCols * sizeof(AttrNumber));
		sortOperators = (Oid *) palloc(numSortCols * sizeof(Oid));
		sortCollations = (Oid *) palloc(numSortCols * sizeof(Oid));
		sortNullsFirst = (bool *) palloc(numSortCols * sizeof(bool));

		i = 0;
		foreach(lc, sortlist)
		{
			SortGroupClause *sortcl = (SortGroupClause *) lfirst(lc);
			TargetEntry *tle = get_sortgroupclause_tle(sortcl, aggref->args);

			/* the parser should have made sure of this */
			Assert(OidIsValid(sortcl->sortop));

			sortColIdx[i] = tle->resno;
			sortOperators[i] = sortcl->sortop;
			sortCollations[i] = exprCollation((Node *) tle->expr);
			sortNullsFirst[i] = sortcl->nulls_first;
			i++;
		}

		if (ishypothetical)
		{
			/* Add an integer flag column as the last sort column */
			sortColIdx[i] = list_length(aggref->args) + 1;
			sortOperators[i] = Int4LessOperator;
			sortCollations[i] = InvalidOid;
			sortNullsFirst[i] = false;
			i++;
		}

		Assert(i == numSortCols);

		/* Now build the stuff we need in aggregate-lifespan context */
		oldcontext = MemoryContextSwitchTo(aggcontext);

		/*
		 * Get a tupledesc corresponding to the aggregated inputs (including
		 * sort expressions) of the agg.
		 */
		osastate->tupdesc = ExecTypeFromTL(aggref->args, false);

		/* If we need a flag column, hack the tupledesc to include that */
		if (ishypothetical)
		{
			TupleDesc	newdesc;
			int			natts = osastate->tupdesc->natts;

			newdesc = CreateTemplateTupleDesc(natts + 1, false);
			for (i = 1; i <= natts; i++)
				TupleDescCopyEntry(newdesc, i, osastate->tupdesc, i);

			TupleDescInitEntry(newdesc,
							   (AttrNumber) ++natts,
							   "flag",
							   INT4OID,
							   -1,
							   0);

			FreeTupleDesc(osastate->tupdesc);
			osastate->tupdesc = newdesc;
		}

		/* Initialize tuplesort object */
		osastate->sortstate = tuplesort_begin_heap(osastate->tupdesc,
												   numSortCols,
												   sortColIdx,
												   sortOperators,
												   sortCollations,
												   sortNullsFirst,
												   work_mem, false);

		/* Create slot we'll use to store/retrieve rows */
		osastate->tupslot = MakeSingleTupleTableSlot(osastate->tupdesc);
	}
	else
	{
		/* Sort single datums */
		SortGroupClause *sortcl;
		TargetEntry *tle;
		Oid			sortColType;
		Oid			sortOperator;
		Oid			eqOperator;
		Oid			sortCollation;
		bool		sortNullsFirst;

		if (numSortCols != 1 || aggref->aggkind == AGGKIND_HYPOTHETICAL)
			elog(ERROR, "ordered-set aggregate support function does not support multiple aggregated columns");

		sortcl = (SortGroupClause *) linitial(sortlist);
		tle = get_sortgroupclause_tle(sortcl, aggref->args);

		/* the parser should have made sure of this */
		Assert(OidIsValid(sortcl->sortop));

		sortColType = exprType((Node *) tle->expr);
		sortOperator = sortcl->sortop;
		eqOperator = sortcl->eqop;
		sortCollation = exprCollation((Node *) tle->expr);
		sortNullsFirst = sortcl->nulls_first;

		/* Save datatype info */
		osastate->datumtype = sortColType;
		get_typlenbyvalalign(sortColType,
							 &osastate->typLen,
							 &osastate->typByVal,
							 &osastate->typAlign);
		osastate->eqOperator = eqOperator;

		/* Now build the stuff we need in aggregate-lifespan context */
		oldcontext = MemoryContextSwitchTo(aggcontext);

		/* Initialize tuplesort object */
		osastate->sortstate = tuplesort_begin_datum(sortColType,
													sortOperator,
													sortCollation,
													sortNullsFirst,
													work_mem, false);
	}

	/* Now register a shutdown callback to clean it all up */
	RegisterExprContextCallback(peraggecontext,
								ordered_set_shutdown,
								PointerGetDatum(osastate));

	MemoryContextSwitchTo(oldcontext);

	return osastate;
}

/*
 * Clean up when evaluation of an ordered-set aggregate is complete.
 *
 * We don't need to bother freeing objects in the aggcontext memory context,
 * since that will get reset anyway by nodeAgg.c, but we should take care to
 * release any potential non-memory resources.
 *
 * This callback is arguably unnecessary, since we don't support use of
 * ordered-set aggs in AGG_HASHED mode and there is currently no non-error
 * code path in non-hashed modes wherein nodeAgg.c won't call the finalfn
 * after calling the transfn one or more times.  So in principle we could rely
 * on the finalfn to delete the tuplestore etc.  However, it's possible that
 * such a code path might exist in future, and in any case it'd be
 * notationally tedious and sometimes require extra data copying to ensure
 * we always delete the tuplestore in the finalfn.
 */
static void
ordered_set_shutdown(Datum arg)
{
	OrderedSetAggState *osastate = (OrderedSetAggState *) DatumGetPointer(arg);

	/* Tuplesort object might have temp files. */
	if (osastate->sortstate)
		tuplesort_end(osastate->sortstate);
	osastate->sortstate = NULL;
	/* The tupleslot probably can't be holding a pin, but let's be safe. */
	if (osastate->tupslot)
		ExecDropSingleTupleTableSlot(osastate->tupslot);
	osastate->tupslot = NULL;
}


/*
 * Generic transition function for ordered-set aggregates
 * with a single input column in which we want to suppress nulls
 */
Datum
ordered_set_transition(PG_FUNCTION_ARGS)
{
	OrderedSetAggState *osastate;

	/* If first call, create the transition state workspace */
	if (PG_ARGISNULL(0))
		osastate = ordered_set_startup(fcinfo, false);
	else
	{
		/* safety check */
		if (AggCheckCallContext(fcinfo, NULL) != AGG_CONTEXT_AGGREGATE)
			elog(ERROR, "ordered-set aggregate called in non-aggregate context");
		osastate = (OrderedSetAggState *) PG_GETARG_POINTER(0);
	}

	/* Load the datum into the tuplesort object, but only if it's not null */
	if (!PG_ARGISNULL(1))
	{
		tuplesort_putdatum(osastate->sortstate, PG_GETARG_DATUM(1), false);
		osastate->number_of_rows++;
	}

	PG_RETURN_POINTER(osastate);
}

/*
 * Generic transition function for ordered-set aggregates
 * with (potentially) multiple aggregated input columns
 */
Datum
ordered_set_transition_multi(PG_FUNCTION_ARGS)
{
	OrderedSetAggState *osastate;
	TupleTableSlot *slot;
	int			nargs;
	int			i;

	/* If first call, create the transition state workspace */
	if (PG_ARGISNULL(0))
		osastate = ordered_set_startup(fcinfo, true);
	else
	{
		/* safety check */
		if (AggCheckCallContext(fcinfo, NULL) != AGG_CONTEXT_AGGREGATE)
			elog(ERROR, "ordered-set aggregate called in non-aggregate context");
		osastate = (OrderedSetAggState *) PG_GETARG_POINTER(0);
	}

	/* Form a tuple from all the other inputs besides the transition value */
	slot = osastate->tupslot;
	ExecClearTuple(slot);
	nargs = PG_NARGS() - 1;
	for (i = 0; i < nargs; i++)
	{
		slot->tts_values[i] = PG_GETARG_DATUM(i + 1);
		slot->tts_isnull[i] = PG_ARGISNULL(i + 1);
	}
	if (osastate->aggref->aggkind == AGGKIND_HYPOTHETICAL)
	{
		/* Add a zero flag value to mark this row as a normal input row */
		slot->tts_values[i] = Int32GetDatum(0);
		slot->tts_isnull[i] = false;
		i++;
	}
	Assert(i == slot->tts_tupleDescriptor->natts);
	ExecStoreVirtualTuple(slot);

	/* Load the row into the tuplesort object */
	tuplesort_puttupleslot(osastate->sortstate, slot);
	osastate->number_of_rows++;

	PG_RETURN_POINTER(osastate);
}


/*
 * percentile_disc(float8) within group(anyelement) - discrete percentile
 */
Datum
percentile_disc_final(PG_FUNCTION_ARGS)
{
	OrderedSetAggState *osastate;
	double		percentile;
	Datum		val;
	bool		isnull;
	int64		rownum;

	/* safety check */
	if (AggCheckCallContext(fcinfo, NULL) != AGG_CONTEXT_AGGREGATE)
		elog(ERROR, "ordered-set aggregate called in non-aggregate context");

	/* Get and check the percentile argument */
	if (PG_ARGISNULL(1))
		PG_RETURN_NULL();

	percentile = PG_GETARG_FLOAT8(1);

	if (percentile < 0 || percentile > 1 || isnan(percentile))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("percentile value %g is not between 0 and 1",
						percentile)));

	/* If there were no regular rows, the result is NULL */
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	osastate = (OrderedSetAggState *) PG_GETARG_POINTER(0);

	/* number_of_rows could be zero if we only saw NULL input values */
	if (osastate->number_of_rows == 0)
		PG_RETURN_NULL();

	/* Finish the sort */
	tuplesort_performsort(osastate->sortstate);

	/*----------
	 * We need the smallest K such that (K/N) >= percentile.
	 * N>0, therefore K >= N*percentile, therefore K = ceil(N*percentile).
	 * So we skip K-1 rows (if K>0) and return the next row fetched.
	 *----------
	 */
	rownum = (int64) ceil(percentile * osastate->number_of_rows);
	Assert(rownum <= osastate->number_of_rows);

	if (rownum > 1)
	{
		if (!tuplesort_skiptuples(osastate->sortstate, rownum - 1, true))
			elog(ERROR, "missing row in percentile_disc");
	}

	if (!tuplesort_getdatum(osastate->sortstate, true, &val, &isnull))
		elog(ERROR, "missing row in percentile_disc");

	/*
	 * Note: we *cannot* clean up the tuplesort object here, because the value
	 * to be returned is allocated inside its sortcontext.	We could use
	 * datumCopy to copy it out of there, but it doesn't seem worth the
	 * trouble, since the cleanup callback will clear the tuplesort later.
	 */

	/* We shouldn't have stored any nulls, but do the right thing anyway */
	if (isnull)
		PG_RETURN_NULL();
	else
		PG_RETURN_DATUM(val);
}


/*
 * For percentile_cont, we need a way to interpolate between consecutive
 * values. Use a helper function for that, so that we can share the rest
 * of the code between types.
 */
typedef Datum (*LerpFunc) (Datum lo, Datum hi, double pct);

static Datum
float8_lerp(Datum lo, Datum hi, double pct)
{
	double		loval = DatumGetFloat8(lo);
	double		hival = DatumGetFloat8(hi);

	return Float8GetDatum(loval + (pct * (hival - loval)));
}

static Datum
interval_lerp(Datum lo, Datum hi, double pct)
{
	Datum		diff_result = DirectFunctionCall2(interval_mi, hi, lo);
	Datum		mul_result = DirectFunctionCall2(interval_mul,
												 diff_result,
												 Float8GetDatumFast(pct));

	return DirectFunctionCall2(interval_pl, mul_result, lo);
}

/*
 * Continuous percentile
 */
static Datum
percentile_cont_final_common(FunctionCallInfo fcinfo,
							 Oid expect_type,
							 LerpFunc lerpfunc)
{
	OrderedSetAggState *osastate;
	double		percentile;
	int64		first_row = 0;
	int64		second_row = 0;
	Datum		val;
	Datum		first_val;
	Datum		second_val;
	double		proportion;
	bool		isnull;

	/* safety check */
	if (AggCheckCallContext(fcinfo, NULL) != AGG_CONTEXT_AGGREGATE)
		elog(ERROR, "ordered-set aggregate called in non-aggregate context");

	/* Get and check the percentile argument */
	if (PG_ARGISNULL(1))
		PG_RETURN_NULL();

	percentile = PG_GETARG_FLOAT8(1);

	if (percentile < 0 || percentile > 1 || isnan(percentile))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("percentile value %g is not between 0 and 1",
						percentile)));

	/* If there were no regular rows, the result is NULL */
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	osastate = (OrderedSetAggState *) PG_GETARG_POINTER(0);

	/* number_of_rows could be zero if we only saw NULL input values */
	if (osastate->number_of_rows == 0)
		PG_RETURN_NULL();

	Assert(expect_type == osastate->datumtype);

	/* Finish the sort */
	tuplesort_performsort(osastate->sortstate);

	first_row = floor(percentile * (osastate->number_of_rows - 1));
	second_row = ceil(percentile * (osastate->number_of_rows - 1));

	Assert(first_row < osastate->number_of_rows);

	if (!tuplesort_skiptuples(osastate->sortstate, first_row, true))
		elog(ERROR, "missing row in percentile_cont");

	if (!tuplesort_getdatum(osastate->sortstate, true, &first_val, &isnull))
		elog(ERROR, "missing row in percentile_cont");
	if (isnull)
		PG_RETURN_NULL();

	if (first_row == second_row)
	{
		val = first_val;
	}
	else
	{
		if (!tuplesort_getdatum(osastate->sortstate, true, &second_val, &isnull))
			elog(ERROR, "missing row in percentile_cont");

		if (isnull)
			PG_RETURN_NULL();

		proportion = (percentile * (osastate->number_of_rows - 1)) - first_row;
		val = lerpfunc(first_val, second_val, proportion);
	}

	/*
	 * Note: we *cannot* clean up the tuplesort object here, because the value
	 * to be returned may be allocated inside its sortcontext.	We could use
	 * datumCopy to copy it out of there, but it doesn't seem worth the
	 * trouble, since the cleanup callback will clear the tuplesort later.
	 */

	if (isnull)
		PG_RETURN_NULL();
	else
		PG_RETURN_DATUM(val);
}

/*
 * percentile_cont(float8) within group (float8)	- continuous percentile
 */
Datum
percentile_cont_float8_final(PG_FUNCTION_ARGS)
{
	return percentile_cont_final_common(fcinfo, FLOAT8OID, float8_lerp);
}

/*
 * percentile_cont(float8) within group (interval)	- continuous percentile
 */
Datum
percentile_cont_interval_final(PG_FUNCTION_ARGS)
{
	return percentile_cont_final_common(fcinfo, INTERVALOID, interval_lerp);
}


/*
 * Support code for handling arrays of percentiles
 *
 * Note: in each pct_info entry, second_row should be equal to or
 * exactly one more than first_row.
 */
struct pct_info
{
	int64		first_row;		/* first row to sample */
	int64		second_row;		/* possible second row to sample */
	double		proportion;		/* interpolation fraction */
	int			idx;			/* index of this item in original array */
};

/*
 * Sort comparator to sort pct_infos by first_row then second_row
 */
static int
pct_info_cmp(const void *pa, const void *pb)
{
	const struct pct_info *a = (const struct pct_info *) pa;
	const struct pct_info *b = (const struct pct_info *) pb;

	if (a->first_row != b->first_row)
		return (a->first_row < b->first_row) ? -1 : 1;
	if (a->second_row != b->second_row)
		return (a->second_row < b->second_row) ? -1 : 1;
	return 0;
}

/*
 * Construct array showing which rows to sample for percentiles.
 */
static struct pct_info *
setup_pct_info(int num_percentiles,
			   Datum *percentiles_datum,
			   bool *percentiles_null,
			   int64 rowcount,
			   bool continuous)
{
	struct pct_info *pct_info;
	int			i;

	pct_info = (struct pct_info *) palloc(num_percentiles * sizeof(struct pct_info));

	for (i = 0; i < num_percentiles; i++)
	{
		pct_info[i].idx = i;

		if (percentiles_null[i])
		{
			/* dummy entry for any NULL in array */
			pct_info[i].first_row = 0;
			pct_info[i].second_row = 0;
			pct_info[i].proportion = 0;
		}
		else
		{
			double		p = DatumGetFloat8(percentiles_datum[i]);

			if (p < 0 || p > 1 || isnan(p))
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("percentile value %g is not between 0 and 1",
								p)));

			if (continuous)
			{
				pct_info[i].first_row = 1 + floor(p * (rowcount - 1));
				pct_info[i].second_row = 1 + ceil(p * (rowcount - 1));
				pct_info[i].proportion = (p * (rowcount - 1)) - floor(p * (rowcount - 1));
			}
			else
			{
				/*----------
				 * We need the smallest K such that (K/N) >= percentile.
				 * N>0, therefore K >= N*percentile, therefore
				 * K = ceil(N*percentile); but not less than 1.
				 *----------
				 */
				int64		row = (int64) ceil(p * rowcount);

				row = Max(1, row);
				pct_info[i].first_row = row;
				pct_info[i].second_row = row;
				pct_info[i].proportion = 0;
			}
		}
	}

	/*
	 * The parameter array wasn't necessarily in sorted order, but we need to
	 * visit the rows in order, so sort by first_row/second_row.
	 */
	qsort(pct_info, num_percentiles, sizeof(struct pct_info), pct_info_cmp);

	return pct_info;
}

/*
 * percentile_disc(float8[]) within group (anyelement)	- discrete percentiles
 */
Datum
percentile_disc_multi_final(PG_FUNCTION_ARGS)
{
	OrderedSetAggState *osastate;
	ArrayType  *param;
	Datum	   *percentiles_datum;
	bool	   *percentiles_null;
	int			num_percentiles;
	struct pct_info *pct_info;
	Datum	   *result_datum;
	bool	   *result_isnull;
	int64		rownum = 0;
	Datum		val = (Datum) 0;
	bool		isnull = true;
	int			i;

	/* safety check */
	if (AggCheckCallContext(fcinfo, NULL) != AGG_CONTEXT_AGGREGATE)
		elog(ERROR, "ordered-set aggregate called in non-aggregate context");

	/* If there were no regular rows, the result is NULL */
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	osastate = (OrderedSetAggState *) PG_GETARG_POINTER(0);

	/* number_of_rows could be zero if we only saw NULL input values */
	if (osastate->number_of_rows == 0)
		PG_RETURN_NULL();

	/* Deconstruct the percentile-array input */
	if (PG_ARGISNULL(1))
		PG_RETURN_NULL();
	param = PG_GETARG_ARRAYTYPE_P(1);

	deconstruct_array(param, FLOAT8OID,
	/* hard-wired info on type float8 */
					  8, FLOAT8PASSBYVAL, 'd',
					  &percentiles_datum,
					  &percentiles_null,
					  &num_percentiles);

	if (num_percentiles == 0)
		PG_RETURN_POINTER(construct_empty_array(osastate->datumtype));

	pct_info = setup_pct_info(num_percentiles,
							  percentiles_datum,
							  percentiles_null,
							  osastate->number_of_rows,
							  false);

	result_datum = (Datum *) palloc(num_percentiles * sizeof(Datum));
	result_isnull = (bool *) palloc(num_percentiles * sizeof(bool));

	/*
	 * Start by dealing with any nulls in the param array - those are sorted
	 * to the front on row=0, so set the corresponding result indexes to null
	 */
	for (i = 0; i < num_percentiles; i++)
	{
		int			idx = pct_info[i].idx;

		if (pct_info[i].first_row > 0)
			break;

		result_datum[idx] = (Datum) 0;
		result_isnull[idx] = true;
	}

	/*
	 * If there's anything left after doing the nulls, then grind the input
	 * and extract the needed values
	 */
	if (i < num_percentiles)
	{
		/* Finish the sort */
		tuplesort_performsort(osastate->sortstate);

		for (; i < num_percentiles; i++)
		{
			int64		target_row = pct_info[i].first_row;
			int			idx = pct_info[i].idx;

			/* Advance to target row, if not already there */
			if (target_row > rownum)
			{
				if (!tuplesort_skiptuples(osastate->sortstate, target_row - rownum - 1, true))
					elog(ERROR, "missing row in percentile_disc");

				if (!tuplesort_getdatum(osastate->sortstate, true, &val, &isnull))
					elog(ERROR, "missing row in percentile_disc");

				rownum = target_row;
			}

			result_datum[idx] = val;
			result_isnull[idx] = isnull;
		}
	}

	/*
	 * We could clean up the tuplesort object after forming the array, but
	 * probably not worth the trouble.
	 */

	/* We make the output array the same shape as the input */
	PG_RETURN_POINTER(construct_md_array(result_datum, result_isnull,
										 ARR_NDIM(param),
										 ARR_DIMS(param),
										 ARR_LBOUND(param),
										 osastate->datumtype,
										 osastate->typLen,
										 osastate->typByVal,
										 osastate->typAlign));
}

/*
 * percentile_cont(float8[]) within group ()	- continuous percentiles
 */
static Datum
percentile_cont_multi_final_common(FunctionCallInfo fcinfo,
								   Oid expect_type,
								   int16 typLen, bool typByVal, char typAlign,
								   LerpFunc lerpfunc)
{
	OrderedSetAggState *osastate;
	ArrayType  *param;
	Datum	   *percentiles_datum;
	bool	   *percentiles_null;
	int			num_percentiles;
	struct pct_info *pct_info;
	Datum	   *result_datum;
	bool	   *result_isnull;
	int64		rownum = 0;
	Datum		first_val = (Datum) 0;
	Datum		second_val = (Datum) 0;
	bool		isnull;
	int			i;

	/* safety check */
	if (AggCheckCallContext(fcinfo, NULL) != AGG_CONTEXT_AGGREGATE)
		elog(ERROR, "ordered-set aggregate called in non-aggregate context");

	/* If there were no regular rows, the result is NULL */
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	osastate = (OrderedSetAggState *) PG_GETARG_POINTER(0);

	/* number_of_rows could be zero if we only saw NULL input values */
	if (osastate->number_of_rows == 0)
		PG_RETURN_NULL();

	Assert(expect_type == osastate->datumtype);

	/* Deconstruct the percentile-array input */
	if (PG_ARGISNULL(1))
		PG_RETURN_NULL();
	param = PG_GETARG_ARRAYTYPE_P(1);

	deconstruct_array(param, FLOAT8OID,
	/* hard-wired info on type float8 */
					  8, FLOAT8PASSBYVAL, 'd',
					  &percentiles_datum,
					  &percentiles_null,
					  &num_percentiles);

	if (num_percentiles == 0)
		PG_RETURN_POINTER(construct_empty_array(osastate->datumtype));

	pct_info = setup_pct_info(num_percentiles,
							  percentiles_datum,
							  percentiles_null,
							  osastate->number_of_rows,
							  true);

	result_datum = (Datum *) palloc(num_percentiles * sizeof(Datum));
	result_isnull = (bool *) palloc(num_percentiles * sizeof(bool));

	/*
	 * Start by dealing with any nulls in the param array - those are sorted
	 * to the front on row=0, so set the corresponding result indexes to null
	 */
	for (i = 0; i < num_percentiles; i++)
	{
		int			idx = pct_info[i].idx;

		if (pct_info[i].first_row > 0)
			break;

		result_datum[idx] = (Datum) 0;
		result_isnull[idx] = true;
	}

	/*
	 * If there's anything left after doing the nulls, then grind the input
	 * and extract the needed values
	 */
	if (i < num_percentiles)
	{
		/* Finish the sort */
		tuplesort_performsort(osastate->sortstate);

		for (; i < num_percentiles; i++)
		{
			int64		target_row = pct_info[i].first_row;
			bool		need_lerp = (pct_info[i].second_row > target_row);
			int			idx = pct_info[i].idx;

			/* Advance to first_row, if not already there */
			if (target_row > rownum)
			{
				if (!tuplesort_skiptuples(osastate->sortstate, target_row - rownum - 1, true))
					elog(ERROR, "missing row in percentile_cont");

				if (!tuplesort_getdatum(osastate->sortstate, true, &first_val, &isnull) || isnull)
					elog(ERROR, "missing row in percentile_cont");

				rownum = target_row;
			}
			else
			{
				/*
				 * We are already at the target row, so we must previously
				 * have read its value into second_val.
				 */
				first_val = second_val;
			}

			/* Fetch second_row if needed */
			if (need_lerp)
			{
				if (!tuplesort_getdatum(osastate->sortstate, true, &second_val, &isnull) || isnull)
					elog(ERROR, "missing row in percentile_cont");
				rownum++;
			}
			else
				second_val = first_val;

			/* Compute appropriate result */
			if (need_lerp)
				result_datum[idx] = lerpfunc(first_val, second_val,
											 pct_info[i].proportion);
			else
				result_datum[idx] = first_val;

			result_isnull[idx] = false;
		}
	}

	/*
	 * We could clean up the tuplesort object after forming the array, but
	 * probably not worth the trouble.
	 */

	/* We make the output array the same shape as the input */
	PG_RETURN_POINTER(construct_md_array(result_datum, result_isnull,
										 ARR_NDIM(param),
										 ARR_DIMS(param), ARR_LBOUND(param),
										 expect_type,
										 typLen,
										 typByVal,
										 typAlign));
}

/*
 * percentile_cont(float8[]) within group (float8)	- continuous percentiles
 */
Datum
percentile_cont_float8_multi_final(PG_FUNCTION_ARGS)
{
	return percentile_cont_multi_final_common(fcinfo,
											  FLOAT8OID,
	/* hard-wired info on type float8 */
											  8, FLOAT8PASSBYVAL, 'd',
											  float8_lerp);
}

/*
 * percentile_cont(float8[]) within group (interval)  - continuous percentiles
 */
Datum
percentile_cont_interval_multi_final(PG_FUNCTION_ARGS)
{
	return percentile_cont_multi_final_common(fcinfo,
											  INTERVALOID,
	/* hard-wired info on type interval */
											  16, false, 'd',
											  interval_lerp);
}


/*
 * mode() within group (anyelement) - most common value
 */
Datum
mode_final(PG_FUNCTION_ARGS)
{
	OrderedSetAggState *osastate;
	Datum		val;
	bool		isnull;
	Datum		mode_val = (Datum) 0;
	int64		mode_freq = 0;
	Datum		last_val = (Datum) 0;
	int64		last_val_freq = 0;
	bool		last_val_is_mode = false;
	FmgrInfo	equalfn;
	bool		shouldfree;

	/* safety check */
	if (AggCheckCallContext(fcinfo, NULL) != AGG_CONTEXT_AGGREGATE)
		elog(ERROR, "ordered-set aggregate called in non-aggregate context");

	/* If there were no regular rows, the result is NULL */
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	osastate = (OrderedSetAggState *) PG_GETARG_POINTER(0);

	/* number_of_rows could be zero if we only saw NULL input values */
	if (osastate->number_of_rows == 0)
		PG_RETURN_NULL();

	/* Look up the equality function for the datatype */
	fmgr_info(get_opcode(osastate->eqOperator), &equalfn);

	shouldfree = !(osastate->typByVal);

	/* Finish the sort */
	tuplesort_performsort(osastate->sortstate);

	/* Scan tuples and count frequencies */
	while (tuplesort_getdatum(osastate->sortstate, true, &val, &isnull))
	{
		/* we don't expect any nulls, but ignore them if found */
		if (isnull)
			continue;

		if (last_val_freq == 0)
		{
			/* first nonnull value - it's the mode for now */
			mode_val = last_val = val;
			mode_freq = last_val_freq = 1;
			last_val_is_mode = true;
		}
		else if (DatumGetBool(FunctionCall2(&equalfn, val, last_val)))
		{
			/* value equal to previous value, count it */
			if (last_val_is_mode)
				mode_freq++;	/* needn't maintain last_val_freq */
			else if (++last_val_freq > mode_freq)
			{
				/* last_val becomes new mode */
				if (shouldfree)
					pfree(DatumGetPointer(mode_val));
				mode_val = last_val;
				mode_freq = last_val_freq;
				last_val_is_mode = true;
			}
			if (shouldfree)
				pfree(DatumGetPointer(val));
		}
		else
		{
			/* val should replace last_val */
			if (shouldfree && !last_val_is_mode)
				pfree(DatumGetPointer(last_val));
			last_val = val;
			last_val_freq = 1;
			last_val_is_mode = false;
		}

		CHECK_FOR_INTERRUPTS();
	}

	if (shouldfree && !last_val_is_mode)
		pfree(DatumGetPointer(last_val));

	/*
	 * Note: we *cannot* clean up the tuplesort object here, because the value
	 * to be returned is allocated inside its sortcontext.	We could use
	 * datumCopy to copy it out of there, but it doesn't seem worth the
	 * trouble, since the cleanup callback will clear the tuplesort later.
	 */

	if (mode_freq)
		PG_RETURN_DATUM(mode_val);
	else
		PG_RETURN_NULL();
}


/*
 * Common code to sanity-check args for hypothetical-set functions. No need
 * for friendly errors, these can only happen if someone's messing up the
 * aggregate definitions. The checks are needed for security, however.
 */
static void
hypothetical_check_argtypes(FunctionCallInfo fcinfo, int nargs,
							TupleDesc tupdesc)
{
	int			i;

	/* check that we have an int4 flag column */
	if (!tupdesc ||
		(nargs + 1) != tupdesc->natts ||
		tupdesc->attrs[nargs]->atttypid != INT4OID)
		elog(ERROR, "type mismatch in hypothetical-set function");

	/* check that direct args match in type with aggregated args */
	for (i = 0; i < nargs; i++)
	{
		if (get_fn_expr_argtype(fcinfo->flinfo, i + 1) != tupdesc->attrs[i]->atttypid)
			elog(ERROR, "type mismatch in hypothetical-set function");
	}
}

/*
 * compute rank of hypothetical row
 *
 * flag should be -1 to sort hypothetical row ahead of its peers, or +1
 * to sort behind.
 * total number of regular rows is returned into *number_of_rows.
 */
static int64
hypothetical_rank_common(FunctionCallInfo fcinfo, int flag,
						 int64 *number_of_rows)
{
	int			nargs = PG_NARGS() - 1;
	int64		rank = 1;
	OrderedSetAggState *osastate;
	TupleTableSlot *slot;
	int			i;

	/* safety check */
	if (AggCheckCallContext(fcinfo, NULL) != AGG_CONTEXT_AGGREGATE)
		elog(ERROR, "ordered-set aggregate called in non-aggregate context");

	/* If there were no regular rows, the rank is always 1 */
	if (PG_ARGISNULL(0))
	{
		*number_of_rows = 0;
		return 1;
	}

	osastate = (OrderedSetAggState *) PG_GETARG_POINTER(0);
	*number_of_rows = osastate->number_of_rows;

	/* Adjust nargs to be the number of direct (or aggregated) args */
	if (nargs % 2 != 0)
		elog(ERROR, "wrong number of arguments in hypothetical-set function");
	nargs /= 2;

	hypothetical_check_argtypes(fcinfo, nargs, osastate->tupdesc);

	/* insert the hypothetical row into the sort */
	slot = osastate->tupslot;
	ExecClearTuple(slot);
	for (i = 0; i < nargs; i++)
	{
		slot->tts_values[i] = PG_GETARG_DATUM(i + 1);
		slot->tts_isnull[i] = PG_ARGISNULL(i + 1);
	}
	slot->tts_values[i] = Int32GetDatum(flag);
	slot->tts_isnull[i] = false;
	ExecStoreVirtualTuple(slot);

	tuplesort_puttupleslot(osastate->sortstate, slot);

	/* finish the sort */
	tuplesort_performsort(osastate->sortstate);

	/* iterate till we find the hypothetical row */
	while (tuplesort_gettupleslot(osastate->sortstate, true, slot))
	{
		bool		isnull;
		Datum		d = slot_getattr(slot, nargs + 1, &isnull);

		if (!isnull && DatumGetInt32(d) != 0)
			break;

		rank++;

		CHECK_FOR_INTERRUPTS();
	}

	ExecClearTuple(slot);

	/* Might as well clean up the tuplesort object immediately */
	tuplesort_end(osastate->sortstate);
	osastate->sortstate = NULL;

	return rank;
}


/*
 * rank()  - rank of hypothetical row
 */
Datum
hypothetical_rank_final(PG_FUNCTION_ARGS)
{
	int64		rank;
	int64		rowcount;

	rank = hypothetical_rank_common(fcinfo, -1, &rowcount);

	PG_RETURN_INT64(rank);
}

/*
 * percent_rank()	- percentile rank of hypothetical row
 */
Datum
hypothetical_percent_rank_final(PG_FUNCTION_ARGS)
{
	int64		rank;
	int64		rowcount;
	double		result_val;

	rank = hypothetical_rank_common(fcinfo, -1, &rowcount);

	if (rowcount == 0)
		PG_RETURN_FLOAT8(0);

	result_val = (double) (rank - 1) / (double) (rowcount);

	PG_RETURN_FLOAT8(result_val);
}

/*
 * cume_dist()	- cumulative distribution of hypothetical row
 */
Datum
hypothetical_cume_dist_final(PG_FUNCTION_ARGS)
{
	int64		rank;
	int64		rowcount;
	double		result_val;

	rank = hypothetical_rank_common(fcinfo, 1, &rowcount);

	result_val = (double) (rank) / (double) (rowcount + 1);

	PG_RETURN_FLOAT8(result_val);
}

/*
 * dense_rank() - rank of hypothetical row without gaps in ranking
 */
Datum
hypothetical_dense_rank_final(PG_FUNCTION_ARGS)
{
	int			nargs = PG_NARGS() - 1;
	int64		rank = 1;
	int64		duplicate_count = 0;
	OrderedSetAggState *osastate;
	List	   *sortlist;
	int			numDistinctCols;
	AttrNumber *sortColIdx;
	FmgrInfo   *equalfns;
	TupleTableSlot *slot;
	TupleTableSlot *extraslot;
	TupleTableSlot *slot2;
	MemoryContext tmpcontext;
	ListCell   *lc;
	int			i;

	/* safety check */
	if (AggCheckCallContext(fcinfo, NULL) != AGG_CONTEXT_AGGREGATE)
		elog(ERROR, "ordered-set aggregate called in non-aggregate context");

	/* If there were no regular rows, the rank is always 1 */
	if (PG_ARGISNULL(0))
		PG_RETURN_INT64(rank);

	osastate = (OrderedSetAggState *) PG_GETARG_POINTER(0);

	/* Adjust nargs to be the number of direct (or aggregated) args */
	if (nargs % 2 != 0)
		elog(ERROR, "wrong number of arguments in hypothetical-set function");
	nargs /= 2;

	hypothetical_check_argtypes(fcinfo, nargs, osastate->tupdesc);

	/*
	 * Construct list of columns to compare for uniqueness.  We can omit the
	 * flag column since we will only compare rows with flag == 0.
	 */
	sortlist = osastate->aggref->aggorder;
	numDistinctCols = list_length(sortlist);
	sortColIdx = (AttrNumber *) palloc(numDistinctCols * sizeof(AttrNumber));
	equalfns = (FmgrInfo *) palloc(numDistinctCols * sizeof(FmgrInfo));

	i = 0;
	foreach(lc, sortlist)
	{
		SortGroupClause *sortcl = (SortGroupClause *) lfirst(lc);
		TargetEntry *tle = get_sortgroupclause_tle(sortcl,
												   osastate->aggref->args);

		sortColIdx[i] = tle->resno;
		fmgr_info(get_opcode(sortcl->eqop), &equalfns[i]);
		i++;
	}

	/* Get short-term context we can use for execTuplesMatch */
	tmpcontext = AggGetPerTupleEContext(fcinfo)->ecxt_per_tuple_memory;

	/* insert the hypothetical row into the sort */
	slot = osastate->tupslot;
	ExecClearTuple(slot);
	for (i = 0; i < nargs; i++)
	{
		slot->tts_values[i] = PG_GETARG_DATUM(i + 1);
		slot->tts_isnull[i] = PG_ARGISNULL(i + 1);
	}
	slot->tts_values[i] = Int32GetDatum(-1);
	slot->tts_isnull[i] = false;
	ExecStoreVirtualTuple(slot);

	tuplesort_puttupleslot(osastate->sortstate, slot);

	/* finish the sort */
	tuplesort_performsort(osastate->sortstate);

	/*
	 * We alternate fetching into osastate->tupslot and extraslot so that we
	 * have the previous row available for comparisons.  This is accomplished
	 * by swapping the slot pointer variables after each row.
	 */
	extraslot = MakeSingleTupleTableSlot(osastate->tupdesc);
	slot2 = extraslot;

	/* iterate till we find the hypothetical row */
	while (tuplesort_gettupleslot(osastate->sortstate, true, slot))
	{
		bool		isnull;
		Datum		d = slot_getattr(slot, nargs + 1, &isnull);
		TupleTableSlot *tmpslot;

		if (!isnull && DatumGetInt32(d) != 0)
			break;

		/* count non-distinct tuples */
		if (!TupIsNull(slot2) &&
			execTuplesMatch(slot, slot2,
							numDistinctCols,
							sortColIdx,
							equalfns,
							tmpcontext))
			duplicate_count++;

		tmpslot = slot2;
		slot2 = slot;
		slot = tmpslot;

		rank++;

		CHECK_FOR_INTERRUPTS();
	}

	ExecClearTuple(slot);
	ExecClearTuple(slot2);

	ExecDropSingleTupleTableSlot(extraslot);

	/* Might as well clean up the tuplesort object immediately */
	tuplesort_end(osastate->sortstate);
	osastate->sortstate = NULL;

	rank = rank - duplicate_count;

	PG_RETURN_INT64(rank);
}
