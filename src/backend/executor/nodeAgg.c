/*-------------------------------------------------------------------------
 *
 * nodeAgg.c
 *	  Routines to handle aggregate nodes.
 *
 *	  ExecAgg evaluates each aggregate in the following steps: (initcond1,
 *	  initcond2 are the initial values and sfunc1, sfunc2, and finalfunc are
 *	  the transition functions.)
 *
 *		 value1 = initcond1
 *		 value2 = initcond2
 *		 foreach input_value do
 *			value1 = sfunc1(value1, input_value)
 *			value2 = sfunc2(value2)
 *		 value1 = finalfunc(value1, value2)
 *
 *	  If initcond1 is NULL then the first non-NULL input_value is
 *	  assigned directly to value1.	sfunc1 isn't applied until value1
 *	  is non-NULL.
 *
 *	  sfunc1 is never applied when the current tuple's input_value is NULL.
 *	  sfunc2 is applied for each tuple if the aggref is marked 'usenulls',
 *	  otherwise it is only applied when input_value is not NULL.
 *	  (usenulls was formerly used for COUNT(*), but is no longer needed for
 *	  that purpose; as of 10/1999 the support for usenulls is dead code.
 *	  I have not removed it because it seems like a potentially useful
 *	  feature for user-defined aggregates.	We'd just need to add a
 *	  flag column to pg_aggregate and a parameter to CREATE AGGREGATE...)
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeAgg.c,v 1.68 2000/06/28 03:31:33 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_operator.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "optimizer/clauses.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "parser/parse_type.h"
#include "utils/syscache.h"
#include "utils/tuplesort.h"

/*
 * AggStatePerAggData - per-aggregate working state for the Agg scan
 */
typedef struct AggStatePerAggData
{

	/*
	 * These values are set up during ExecInitAgg() and do not change
	 * thereafter:
	 */

	/* Link to Aggref node this working state is for */
	Aggref	   *aggref;

	/* Oids of transfer functions */
	Oid			xfn1_oid;
	Oid			xfn2_oid;
	Oid			finalfn_oid;

	/*
	 * fmgr lookup data for transfer functions --- only valid when
	 * corresponding oid is not InvalidOid
	 */
	FmgrInfo	xfn1;
	FmgrInfo	xfn2;
	FmgrInfo	finalfn;

	/*
	 * Type of input data and Oid of sort operator to use for it; only
	 * set/used when aggregate has DISTINCT flag.  (These are not used
	 * directly by nodeAgg, but must be passed to the Tuplesort object.)
	 */
	Oid			inputType;
	Oid			sortOperator;

	/*
	 * fmgr lookup data for input type's equality operator --- only
	 * set/used when aggregate has DISTINCT flag.
	 */
	FmgrInfo	equalfn;

	/*
	 * initial values from pg_aggregate entry
	 */
	Datum		initValue1;		/* for transtype1 */
	Datum		initValue2;		/* for transtype2 */
	bool		initValue1IsNull,
				initValue2IsNull;

	/*
	 * We need the len and byval info for the agg's input and transition
	 * data types in order to know how to copy/delete values.
	 */
	int			inputtypeLen,
				transtype1Len,
				transtype2Len;
	bool		inputtypeByVal,
				transtype1ByVal,
				transtype2ByVal;

	/*
	 * These values are working state that is initialized at the start of
	 * an input tuple group and updated for each input tuple.
	 *
	 * For a simple (non DISTINCT) aggregate, we just feed the input values
	 * straight to the transition functions.  If it's DISTINCT, we pass
	 * the input values into a Tuplesort object; then at completion of the
	 * input tuple group, we scan the sorted values, eliminate duplicates,
	 * and run the transition functions on the rest.
	 */

	Tuplesortstate *sortstate;	/* sort object, if a DISTINCT agg */

	Datum		value1,			/* current transfer values 1 and 2 */
				value2;
	bool		value1IsNull,
				value2IsNull;
	bool		noInitValue;	/* true if value1 not set yet */

	/*
	 * Note: right now, noInitValue always has the same value as
	 * value1IsNull. But we should keep them separate because once the
	 * fmgr interface is fixed, we'll need to distinguish a null returned
	 * by transfn1 from a null we haven't yet replaced with an input
	 * value.
	 */
} AggStatePerAggData;


static void initialize_aggregate(AggStatePerAgg peraggstate);
static void advance_transition_functions(AggStatePerAgg peraggstate,
							 Datum newVal, bool isNull);
static void finalize_aggregate(AggStatePerAgg peraggstate,
				   Datum *resultVal, bool *resultIsNull);
static Datum copyDatum(Datum val, int typLen, bool typByVal);


/*
 * Initialize one aggregate for a new set of input values.
 */
static void
initialize_aggregate(AggStatePerAgg peraggstate)
{
	Aggref	   *aggref = peraggstate->aggref;

	/*
	 * Start a fresh sort operation for each DISTINCT aggregate.
	 */
	if (aggref->aggdistinct)
	{

		/*
		 * In case of rescan, maybe there could be an uncompleted sort
		 * operation?  Clean it up if so.
		 */
		if (peraggstate->sortstate)
			tuplesort_end(peraggstate->sortstate);

		peraggstate->sortstate =
			tuplesort_begin_datum(peraggstate->inputType,
								  peraggstate->sortOperator,
								  false);
	}

	/*
	 * (Re)set value1 and value2 to their initial values.
	 */
	if (OidIsValid(peraggstate->xfn1_oid) &&
		!peraggstate->initValue1IsNull)
		peraggstate->value1 = copyDatum(peraggstate->initValue1,
										peraggstate->transtype1Len,
										peraggstate->transtype1ByVal);
	else
		peraggstate->value1 = (Datum) NULL;
	peraggstate->value1IsNull = peraggstate->initValue1IsNull;

	if (OidIsValid(peraggstate->xfn2_oid) &&
		!peraggstate->initValue2IsNull)
		peraggstate->value2 = copyDatum(peraggstate->initValue2,
										peraggstate->transtype2Len,
										peraggstate->transtype2ByVal);
	else
		peraggstate->value2 = (Datum) NULL;
	peraggstate->value2IsNull = peraggstate->initValue2IsNull;

	/* ------------------------------------------
	 * If the initial value for the first transition function
	 * doesn't exist in the pg_aggregate table then we will let
	 * the first value returned from the outer procNode become
	 * the initial value. (This is useful for aggregates like
	 * max{} and min{}.)  The noInitValue flag signals that we
	 * still need to do this.
	 * ------------------------------------------
	 */
	peraggstate->noInitValue = peraggstate->initValue1IsNull;
}

/*
 * Given a new input value, advance the transition functions of an aggregate.
 *
 * Note: if the agg does not have usenulls set, null inputs will be filtered
 * out before reaching here.
 */
static void
advance_transition_functions(AggStatePerAgg peraggstate,
							 Datum newVal, bool isNull)
{
	FunctionCallInfoData	fcinfo;

	MemSet(&fcinfo, 0, sizeof(fcinfo));

	/*
	 * XXX reconsider isNULL handling here
	 */
	if (OidIsValid(peraggstate->xfn1_oid) && !isNull)
	{
		if (peraggstate->noInitValue)
		{

			/*
			 * value1 has not been initialized. This is the first non-NULL
			 * input value. We use it as the initial value for value1.
			 *
			 * XXX We assume, without having checked, that the agg's input
			 * type is binary-compatible with its transtype1!
			 *
			 * We have to copy the datum since the tuple from which it came
			 * will be freed on the next iteration of the scan.
			 */
			peraggstate->value1 = copyDatum(newVal,
											peraggstate->transtype1Len,
											peraggstate->transtype1ByVal);
			peraggstate->value1IsNull = false;
			peraggstate->noInitValue = false;
		}
		else
		{
			/* apply transition function 1 */
			fcinfo.flinfo = &peraggstate->xfn1;
			fcinfo.nargs = 2;
			fcinfo.arg[0] = peraggstate->value1;
			fcinfo.argnull[0] = peraggstate->value1IsNull;
			fcinfo.arg[1] = newVal;
			fcinfo.argnull[1] = isNull;
			if (fcinfo.flinfo->fn_strict &&
				(peraggstate->value1IsNull || isNull))
			{
				/* don't call a strict function with NULL inputs */
				newVal = (Datum) 0;
				fcinfo.isnull = true;
			}
			else
				newVal = FunctionCallInvoke(&fcinfo);
			if (!peraggstate->transtype1ByVal && !peraggstate->value1IsNull)
				pfree(DatumGetPointer(peraggstate->value1));
			peraggstate->value1 = newVal;
			peraggstate->value1IsNull = fcinfo.isnull;
		}
	}

	if (OidIsValid(peraggstate->xfn2_oid))
	{
		/* apply transition function 2 */
		fcinfo.flinfo = &peraggstate->xfn2;
		fcinfo.nargs = 1;
		fcinfo.arg[0] = peraggstate->value2;
		fcinfo.argnull[0] = peraggstate->value2IsNull;
		fcinfo.isnull = false;	/* must reset after use by xfn1 */
		if (fcinfo.flinfo->fn_strict && peraggstate->value2IsNull)
		{
			/* don't call a strict function with NULL inputs */
			newVal = (Datum) 0;
			fcinfo.isnull = true;
		}
		else
			newVal = FunctionCallInvoke(&fcinfo);
		if (!peraggstate->transtype2ByVal && !peraggstate->value2IsNull)
			pfree(DatumGetPointer(peraggstate->value2));
		peraggstate->value2 = newVal;
		peraggstate->value2IsNull = fcinfo.isnull;
	}
}

/*
 * Compute the final value of one aggregate.
 */
static void
finalize_aggregate(AggStatePerAgg peraggstate,
				   Datum *resultVal, bool *resultIsNull)
{
	Aggref				   *aggref = peraggstate->aggref;
	FunctionCallInfoData	fcinfo;

	MemSet(&fcinfo, 0, sizeof(fcinfo));

	/*
	 * If it's a DISTINCT aggregate, all we've done so far is to stuff the
	 * input values into the sort object.  Complete the sort, then run the
	 * transition functions on the non-duplicate values.  Note that
	 * DISTINCT always suppresses nulls, per SQL spec, regardless of
	 * usenulls.
	 */
	if (aggref->aggdistinct)
	{
		Datum		oldVal = (Datum) 0;
		bool		haveOldVal = false;
		Datum		newVal;
		bool		isNull;

		tuplesort_performsort(peraggstate->sortstate);
		while (tuplesort_getdatum(peraggstate->sortstate, true,
								  &newVal, &isNull))
		{
			if (isNull)
				continue;
			if (haveOldVal)
			{
				if (DatumGetBool(FunctionCall2(&peraggstate->equalfn,
											   oldVal, newVal)))
				{
					/* equal to prior, so forget this one */
					if (!peraggstate->inputtypeByVal)
						pfree(DatumGetPointer(newVal));
					continue;
				}
			}
			advance_transition_functions(peraggstate, newVal, false);
			if (haveOldVal && !peraggstate->inputtypeByVal)
				pfree(DatumGetPointer(oldVal));
			oldVal = newVal;
			haveOldVal = true;
		}
		if (haveOldVal && !peraggstate->inputtypeByVal)
			pfree(DatumGetPointer(oldVal));
		tuplesort_end(peraggstate->sortstate);
		peraggstate->sortstate = NULL;
	}

	/*
	 * Now apply the agg's finalfn, or substitute the appropriate
	 * transition value if there is no finalfn.
	 *
	 * XXX For now, only apply finalfn if we got at least one non-null input
	 * value.  This prevents zero divide in AVG(). If we had cleaner
	 * handling of null inputs/results in functions, we could probably
	 * take out this hack and define the result for no inputs as whatever
	 * finalfn returns for null input.
	 */
	if (OidIsValid(peraggstate->finalfn_oid) &&
		!peraggstate->noInitValue)
	{
		fcinfo.flinfo = &peraggstate->finalfn;
		if (peraggstate->finalfn.fn_nargs > 1)
		{
			fcinfo.nargs = 2;
			fcinfo.arg[0] = peraggstate->value1;
			fcinfo.argnull[0] = peraggstate->value1IsNull;
			fcinfo.arg[1] = peraggstate->value2;
			fcinfo.argnull[1] = peraggstate->value2IsNull;
		}
		else if (OidIsValid(peraggstate->xfn1_oid))
		{
			fcinfo.nargs = 1;
			fcinfo.arg[0] = peraggstate->value1;
			fcinfo.argnull[0] = peraggstate->value1IsNull;
		}
		else if (OidIsValid(peraggstate->xfn2_oid))
		{
			fcinfo.nargs = 1;
			fcinfo.arg[0] = peraggstate->value2;
			fcinfo.argnull[0] = peraggstate->value2IsNull;
		}
		else
			elog(ERROR, "ExecAgg: no valid transition functions??");
		if (fcinfo.flinfo->fn_strict &&
			(fcinfo.argnull[0] || fcinfo.argnull[1]))
		{
			/* don't call a strict function with NULL inputs */
			*resultVal = (Datum) 0;
			*resultIsNull = true;
		}
		else
		{
			*resultVal = FunctionCallInvoke(&fcinfo);
			*resultIsNull = fcinfo.isnull;
		}
	}
	else if (OidIsValid(peraggstate->xfn1_oid))
	{
		/* Return value1 */
		*resultVal = peraggstate->value1;
		*resultIsNull = peraggstate->value1IsNull;
		/* prevent pfree below */
		peraggstate->value1IsNull = true;
	}
	else if (OidIsValid(peraggstate->xfn2_oid))
	{
		/* Return value2 */
		*resultVal = peraggstate->value2;
		*resultIsNull = peraggstate->value2IsNull;
		/* prevent pfree below */
		peraggstate->value2IsNull = true;
	}
	else
		elog(ERROR, "ExecAgg: no valid transition functions??");

	/*
	 * Release any per-group working storage, unless we're passing it back
	 * as the result of the aggregate.
	 */
	if (OidIsValid(peraggstate->xfn1_oid) &&
		!peraggstate->value1IsNull &&
		!peraggstate->transtype1ByVal)
		pfree(DatumGetPointer(peraggstate->value1));

	if (OidIsValid(peraggstate->xfn2_oid) &&
		!peraggstate->value2IsNull &&
		!peraggstate->transtype2ByVal)
		pfree(DatumGetPointer(peraggstate->value2));
}

/* ---------------------------------------
 *
 * ExecAgg -
 *
 *	  ExecAgg receives tuples from its outer subplan and aggregates over
 *	  the appropriate attribute for each aggregate function use (Aggref
 *	  node) appearing in the targetlist or qual of the node.  The number
 *	  of tuples to aggregate over depends on whether a GROUP BY clause is
 *	  present.	We can produce an aggregate result row per group, or just
 *	  one for the whole query.	The value of each aggregate is stored in
 *	  the expression context to be used when ExecProject evaluates the
 *	  result tuple.
 *
 *	  If the outer subplan is a Group node, ExecAgg returns as many tuples
 *	  as there are groups.
 *
 * ------------------------------------------
 */
TupleTableSlot *
ExecAgg(Agg *node)
{
	AggState   *aggstate;
	EState	   *estate;
	Plan	   *outerPlan;
	ExprContext *econtext;
	ProjectionInfo *projInfo;
	Datum	   *aggvalues;
	bool	   *aggnulls;
	AggStatePerAgg peragg;
	TupleTableSlot *resultSlot;
	HeapTuple	inputTuple;
	int			aggno;
	bool		isDone;
	bool		isNull;

	/* ---------------------
	 *	get state info from node
	 * ---------------------
	 */
	aggstate = node->aggstate;
	estate = node->plan.state;
	outerPlan = outerPlan(node);
	econtext = aggstate->csstate.cstate.cs_ExprContext;
	aggvalues = econtext->ecxt_aggvalues;
	aggnulls = econtext->ecxt_aggnulls;
	projInfo = aggstate->csstate.cstate.cs_ProjInfo;
	peragg = aggstate->peragg;

	/*
	 * We loop retrieving groups until we find one matching
	 * node->plan.qual
	 */
	do
	{
		if (aggstate->agg_done)
			return NULL;

		/*
		 * Initialize working state for a new input tuple group
		 */
		for (aggno = 0; aggno < aggstate->numaggs; aggno++)
		{
			AggStatePerAgg peraggstate = &peragg[aggno];

			initialize_aggregate(peraggstate);
		}

		inputTuple = NULL;		/* no saved input tuple yet */

		/* ----------------
		 *	 for each tuple from the outer plan, update all the aggregates
		 * ----------------
		 */
		for (;;)
		{
			TupleTableSlot *outerslot;

			outerslot = ExecProcNode(outerPlan, (Plan *) node);
			if (TupIsNull(outerslot))
				break;
			econtext->ecxt_scantuple = outerslot;

			for (aggno = 0; aggno < aggstate->numaggs; aggno++)
			{
				AggStatePerAgg peraggstate = &peragg[aggno];
				Aggref	   *aggref = peraggstate->aggref;
				Datum		newVal;

				newVal = ExecEvalExpr(aggref->target, econtext,
									  &isNull, &isDone);

				if (isNull && !aggref->usenulls)
					continue;	/* ignore this tuple for this agg */

				if (aggref->aggdistinct)
					tuplesort_putdatum(peraggstate->sortstate,
									   newVal, isNull);
				else
					advance_transition_functions(peraggstate,
												 newVal, isNull);
			}

			/*
			 * Keep a copy of the first input tuple for the projection.
			 * (We only need one since only the GROUP BY columns in it can
			 * be referenced, and these will be the same for all tuples
			 * aggregated over.)
			 */
			if (!inputTuple)
				inputTuple = heap_copytuple(outerslot->val);
		}

		/*
		 * Done scanning input tuple group. Finalize each aggregate
		 * calculation.
		 */
		for (aggno = 0; aggno < aggstate->numaggs; aggno++)
		{
			AggStatePerAgg peraggstate = &peragg[aggno];

			finalize_aggregate(peraggstate,
							   &aggvalues[aggno], &aggnulls[aggno]);
		}

		/*
		 * If the outerPlan is a Group node, we will reach here after each
		 * group.  We are not done unless the Group node is done (a little
		 * ugliness here while we reach into the Group's state to find
		 * out). Furthermore, when grouping we return nothing at all
		 * unless we had some input tuple(s).  By the nature of Group,
		 * there are no empty groups, so if we get here with no input the
		 * whole scan is empty.
		 *
		 * If the outerPlan isn't a Group, we are done when we get here, and
		 * we will emit a (single) tuple even if there were no input
		 * tuples.
		 */
		if (IsA(outerPlan, Group))
		{
			/* aggregation over groups */
			aggstate->agg_done = ((Group *) outerPlan)->grpstate->grp_done;
			/* check for no groups */
			if (inputTuple == NULL)
				return NULL;
		}
		else
		{
			aggstate->agg_done = true;

			/*
			 * If inputtuple==NULL (ie, the outerPlan didn't return
			 * anything), create a dummy all-nulls input tuple for use by
			 * execProject. 99.44% of the time this is a waste of cycles,
			 * because ordinarily the projected output tuple's targetlist
			 * cannot contain any direct (non-aggregated) references to
			 * input columns, so the dummy tuple will not be referenced.
			 * However there are special cases where this isn't so --- in
			 * particular an UPDATE involving an aggregate will have a
			 * targetlist reference to ctid.  We need to return a null for
			 * ctid in that situation, not coredump.
			 *
			 * The values returned for the aggregates will be the initial
			 * values of the transition functions.
			 */
			if (inputTuple == NULL)
			{
				TupleDesc	tupType;
				Datum	   *tupValue;
				char	   *null_array;
				AttrNumber	attnum;

				tupType = aggstate->csstate.css_ScanTupleSlot->ttc_tupleDescriptor;
				tupValue = projInfo->pi_tupValue;
				/* watch out for null input tuples, though... */
				if (tupType && tupValue)
				{
					null_array = (char *) palloc(sizeof(char) * tupType->natts);
					for (attnum = 0; attnum < tupType->natts; attnum++)
						null_array[attnum] = 'n';
					inputTuple = heap_formtuple(tupType, tupValue, null_array);
					pfree(null_array);
				}
			}
		}

		/*
		 * Store the representative input tuple in the tuple table slot
		 * reserved for it.
		 */
		ExecStoreTuple(inputTuple,
					   aggstate->csstate.css_ScanTupleSlot,
					   InvalidBuffer,
					   true);
		econtext->ecxt_scantuple = aggstate->csstate.css_ScanTupleSlot;

		/*
		 * Form a projection tuple using the aggregate results and the
		 * representative input tuple.	Store it in the result tuple slot.
		 */
		resultSlot = ExecProject(projInfo, &isDone);

		/*
		 * If the completed tuple does not match the qualifications, it is
		 * ignored and we loop back to try to process another group.
		 * Otherwise, return the tuple.
		 */
	}
	while (!ExecQual(node->plan.qual, econtext, false));

	return resultSlot;
}

/* -----------------
 * ExecInitAgg
 *
 *	Creates the run-time information for the agg node produced by the
 *	planner and initializes its outer subtree
 * -----------------
 */
bool
ExecInitAgg(Agg *node, EState *estate, Plan *parent)
{
	AggState   *aggstate;
	AggStatePerAgg peragg;
	Plan	   *outerPlan;
	ExprContext *econtext;
	int			numaggs,
				aggno;
	List	   *alist;

	/*
	 * assign the node's execution state
	 */
	node->plan.state = estate;

	/*
	 * create state structure
	 */
	aggstate = makeNode(AggState);
	node->aggstate = aggstate;
	aggstate->agg_done = false;

	/*
	 * find aggregates in targetlist and quals
	 *
	 * Note: pull_agg_clauses also checks that no aggs contain other agg
	 * calls in their arguments.  This would make no sense under SQL
	 * semantics anyway (and it's forbidden by the spec).  Because that is
	 * true, we don't need to worry about evaluating the aggs in any
	 * particular order.
	 */
	aggstate->aggs = nconc(pull_agg_clause((Node *) node->plan.targetlist),
						   pull_agg_clause((Node *) node->plan.qual));
	aggstate->numaggs = numaggs = length(aggstate->aggs);
	if (numaggs <= 0)
	{

		/*
		 * This used to be treated as an error, but we can't do that
		 * anymore because constant-expression simplification could
		 * optimize away all of the Aggrefs in the targetlist and qual.
		 * So, just make a debug note, and force numaggs positive so that
		 * palloc()s below don't choke.
		 */
		elog(DEBUG, "ExecInitAgg: could not find any aggregate functions");
		numaggs = 1;
	}

	/*
	 * assign node's base id and create expression context
	 */
	ExecAssignNodeBaseInfo(estate, &aggstate->csstate.cstate, (Plan *) parent);
	ExecAssignExprContext(estate, &aggstate->csstate.cstate);

#define AGG_NSLOTS 2

	/*
	 * tuple table initialization
	 */
	ExecInitScanTupleSlot(estate, &aggstate->csstate);
	ExecInitResultTupleSlot(estate, &aggstate->csstate.cstate);

	/*
	 * Set up aggregate-result storage in the expr context, and also
	 * allocate my private per-agg working storage
	 */
	econtext = aggstate->csstate.cstate.cs_ExprContext;
	econtext->ecxt_aggvalues = (Datum *) palloc(sizeof(Datum) * numaggs);
	MemSet(econtext->ecxt_aggvalues, 0, sizeof(Datum) * numaggs);
	econtext->ecxt_aggnulls = (bool *) palloc(sizeof(bool) * numaggs);
	MemSet(econtext->ecxt_aggnulls, 0, sizeof(bool) * numaggs);

	peragg = (AggStatePerAgg) palloc(sizeof(AggStatePerAggData) * numaggs);
	MemSet(peragg, 0, sizeof(AggStatePerAggData) * numaggs);
	aggstate->peragg = peragg;

	/*
	 * initialize child nodes
	 */
	outerPlan = outerPlan(node);
	ExecInitNode(outerPlan, estate, (Plan *) node);

	/* ----------------
	 *	initialize source tuple type.
	 * ----------------
	 */
	ExecAssignScanTypeFromOuterPlan((Plan *) node, &aggstate->csstate);

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL((Plan *) node, &aggstate->csstate.cstate);
	ExecAssignProjectionInfo((Plan *) node, &aggstate->csstate.cstate);

	/*
	 * Perform lookups of aggregate function info, and initialize the
	 * unchanging fields of the per-agg data
	 */
	aggno = -1;
	foreach(alist, aggstate->aggs)
	{
		Aggref	   *aggref = (Aggref *) lfirst(alist);
		AggStatePerAgg peraggstate = &peragg[++aggno];
		char	   *aggname = aggref->aggname;
		HeapTuple	aggTuple;
		Form_pg_aggregate aggform;
		Type		typeInfo;
		Oid			xfn1_oid,
					xfn2_oid,
					finalfn_oid;

		/* Mark Aggref node with its associated index in the result array */
		aggref->aggno = aggno;

		/* Fill in the peraggstate data */
		peraggstate->aggref = aggref;

		aggTuple = SearchSysCacheTuple(AGGNAME,
									   PointerGetDatum(aggname),
									   ObjectIdGetDatum(aggref->basetype),
									   0, 0);
		if (!HeapTupleIsValid(aggTuple))
			elog(ERROR, "ExecAgg: cache lookup failed for aggregate %s(%s)",
				 aggname,
				 typeidTypeName(aggref->basetype));
		aggform = (Form_pg_aggregate) GETSTRUCT(aggTuple);

		peraggstate->initValue1 =
			AggNameGetInitVal(aggname,
							  aggform->aggbasetype,
							  1,
							  &peraggstate->initValue1IsNull);

		peraggstate->initValue2 =
			AggNameGetInitVal(aggname,
							  aggform->aggbasetype,
							  2,
							  &peraggstate->initValue2IsNull);

		peraggstate->xfn1_oid = xfn1_oid = aggform->aggtransfn1;
		peraggstate->xfn2_oid = xfn2_oid = aggform->aggtransfn2;
		peraggstate->finalfn_oid = finalfn_oid = aggform->aggfinalfn;

		if (OidIsValid(xfn1_oid))
		{
			fmgr_info(xfn1_oid, &peraggstate->xfn1);
			/* If a transfn1 is specified, transtype1 had better be, too */
			typeInfo = typeidType(aggform->aggtranstype1);
			peraggstate->transtype1Len = typeLen(typeInfo);
			peraggstate->transtype1ByVal = typeByVal(typeInfo);
		}

		if (OidIsValid(xfn2_oid))
		{
			fmgr_info(xfn2_oid, &peraggstate->xfn2);
			/* If a transfn2 is specified, transtype2 had better be, too */
			typeInfo = typeidType(aggform->aggtranstype2);
			peraggstate->transtype2Len = typeLen(typeInfo);
			peraggstate->transtype2ByVal = typeByVal(typeInfo);
			/* ------------------------------------------
			 * If there is a second transition function, its initial
			 * value must exist -- as it does not depend on data values,
			 * we have no other way of determining an initial value.
			 * ------------------------------------------
			 */
			if (peraggstate->initValue2IsNull)
				elog(ERROR, "ExecInitAgg: agginitval2 is null");
		}

		if (OidIsValid(finalfn_oid))
			fmgr_info(finalfn_oid, &peraggstate->finalfn);

		if (aggref->aggdistinct)
		{
			Oid			inputType = exprType(aggref->target);
			Operator	eq_operator;
			Form_pg_operator pgopform;

			peraggstate->inputType = inputType;
			typeInfo = typeidType(inputType);
			peraggstate->inputtypeLen = typeLen(typeInfo);
			peraggstate->inputtypeByVal = typeByVal(typeInfo);

			eq_operator = oper("=", inputType, inputType, true);
			if (!HeapTupleIsValid(eq_operator))
			{
				elog(ERROR, "Unable to identify an equality operator for type '%s'",
					 typeidTypeName(inputType));
			}
			pgopform = (Form_pg_operator) GETSTRUCT(eq_operator);
			fmgr_info(pgopform->oprcode, &(peraggstate->equalfn));
			peraggstate->sortOperator = any_ordering_op(inputType);
			peraggstate->sortstate = NULL;
		}
	}

	return TRUE;
}

int
ExecCountSlotsAgg(Agg *node)
{
	return ExecCountSlotsNode(outerPlan(node)) +
	ExecCountSlotsNode(innerPlan(node)) +
	AGG_NSLOTS;
}

void
ExecEndAgg(Agg *node)
{
	AggState   *aggstate = node->aggstate;
	Plan	   *outerPlan;

	ExecFreeProjectionInfo(&aggstate->csstate.cstate);

	outerPlan = outerPlan(node);
	ExecEndNode(outerPlan, (Plan *) node);

	/* clean up tuple table */
	ExecClearTuple(aggstate->csstate.css_ScanTupleSlot);
}

void
ExecReScanAgg(Agg *node, ExprContext *exprCtxt, Plan *parent)
{
	AggState   *aggstate = node->aggstate;
	ExprContext *econtext = aggstate->csstate.cstate.cs_ExprContext;

	aggstate->agg_done = false;
	MemSet(econtext->ecxt_aggvalues, 0, sizeof(Datum) * aggstate->numaggs);
	MemSet(econtext->ecxt_aggnulls, 0, sizeof(bool) * aggstate->numaggs);

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (((Plan *) node)->lefttree->chgParam == NULL)
		ExecReScan(((Plan *) node)->lefttree, exprCtxt, (Plan *) node);

}


/*
 * Helper routine to make a copy of a Datum.
 *
 * NB: input had better not be a NULL; might cause null-pointer dereference.
 */
static Datum
copyDatum(Datum val, int typLen, bool typByVal)
{
	if (typByVal)
		return val;
	else
	{
		char	   *newVal;

		if (typLen == -1)		/* variable length type? */
			typLen = VARSIZE((struct varlena *) DatumGetPointer(val));
		newVal = (char *) palloc(typLen);
		memcpy(newVal, DatumGetPointer(val), typLen);
		return PointerGetDatum(newVal);
	}
}
