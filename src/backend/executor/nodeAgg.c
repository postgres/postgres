/*-------------------------------------------------------------------------
 *
 * nodeAgg.c
 *	  Routines to handle aggregate nodes.
 *
 *	  ExecAgg evaluates each aggregate in the following steps:
 *
 *		 transvalue = initcond
 *		 foreach input_value do
 *			transvalue = transfunc(transvalue, input_value)
 *		 result = finalfunc(transvalue)
 *
 *	  If a finalfunc is not supplied then the result is just the ending
 *	  value of transvalue.
 *
 *	  If transfunc is marked "strict" in pg_proc and initcond is NULL,
 *	  then the first non-NULL input_value is assigned directly to transvalue,
 *	  and transfunc isn't applied until the second non-NULL input_value.
 *	  The agg's input type and transtype must be the same in this case!
 *
 *	  If transfunc is marked "strict" then NULL input_values are skipped,
 *	  keeping the previous transvalue.  If transfunc is not strict then it
 *	  is called for every input tuple and must deal with NULL initcond
 *	  or NULL input_value for itself.
 *
 *	  If finalfunc is marked "strict" then it is not called when the
 *	  ending transvalue is NULL, instead a NULL result is created
 *	  automatically (this is just the usual handling of strict functions,
 *	  of course).  A non-strict finalfunc can make its own choice of
 *	  what to return for a NULL ending transvalue.
 *
 *	  When the transvalue datatype is pass-by-reference, we have to be
 *	  careful to ensure that the values survive across tuple cycles yet
 *	  are not allowed to accumulate until end of query.  We do this by
 *	  "ping-ponging" between two memory contexts; successive calls to the
 *	  transfunc are executed in alternate contexts, passing the previous
 *	  transvalue that is in the other context.  At the beginning of each
 *	  tuple cycle we can reset the current output context to avoid memory
 *	  usage growth.  Note: we must use MemoryContextContains() to check
 *	  whether the transfunc has perhaps handed us back one of its input
 *	  values rather than a freshly palloc'd value; if so, we copy the value
 *	  to the context we want it in.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeAgg.c,v 1.75 2001/02/16 03:16:57 tgl Exp $
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
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "parser/parse_type.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/tuplesort.h"
#include "utils/datum.h"

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
	Oid			transfn_oid;
	Oid			finalfn_oid;	/* may be InvalidOid */

	/*
	 * fmgr lookup data for transfer functions --- only valid when
	 * corresponding oid is not InvalidOid.  Note in particular that
	 * fn_strict flags are kept here.
	 */
	FmgrInfo	transfn;
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
	 * initial value from pg_aggregate entry
	 */
	Datum		initValue;
	bool		initValueIsNull;

	/*
	 * We need the len and byval info for the agg's input, result, and
	 * transition data types in order to know how to copy/delete values.
	 */
	int16		inputtypeLen,
				resulttypeLen,
				transtypeLen;
	bool		inputtypeByVal,
				resulttypeByVal,
				transtypeByVal;

	/*
	 * These values are working state that is initialized at the start of
	 * an input tuple group and updated for each input tuple.
	 *
	 * For a simple (non DISTINCT) aggregate, we just feed the input values
	 * straight to the transition function.  If it's DISTINCT, we pass
	 * the input values into a Tuplesort object; then at completion of the
	 * input tuple group, we scan the sorted values, eliminate duplicates,
	 * and run the transition function on the rest.
	 */

	Tuplesortstate *sortstate;	/* sort object, if a DISTINCT agg */

	Datum		transValue;
	bool		transValueIsNull;

	bool		noTransValue;	/* true if transValue not set yet */

	/*
	 * Note: noTransValue initially has the same value as transValueIsNull,
	 * and if true both are cleared to false at the same time.  They are
	 * not the same though: if transfn later returns a NULL, we want to
	 * keep that NULL and not auto-replace it with a later input value.
	 * Only the first non-NULL input will be auto-substituted.
	 */
} AggStatePerAggData;


static void initialize_aggregate(AggStatePerAgg peraggstate);
static void advance_transition_function(AggStatePerAgg peraggstate,
										Datum newVal, bool isNull);
static void process_sorted_aggregate(AggState *aggstate,
									 AggStatePerAgg peraggstate);
static void finalize_aggregate(AggStatePerAgg peraggstate,
				   Datum *resultVal, bool *resultIsNull);


/*
 * Initialize one aggregate for a new set of input values.
 *
 * When called, CurrentMemoryContext should be the per-query context.
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
	 * (Re)set transValue to the initial value.
	 *
	 * Note that when the initial value is pass-by-ref, we just reuse it
	 * without copying for each group.  Hence, transition function
	 * had better not scribble on its input, or it will fail for GROUP BY!
	 */
	peraggstate->transValue = peraggstate->initValue;
	peraggstate->transValueIsNull = peraggstate->initValueIsNull;

	/* ------------------------------------------
	 * If the initial value for the transition state doesn't exist in the
	 * pg_aggregate table then we will let the first non-NULL value returned
	 * from the outer procNode become the initial value. (This is useful for
	 * aggregates like max() and min().)  The noTransValue flag signals that
	 * we still need to do this.
	 * ------------------------------------------
	 */
	peraggstate->noTransValue = peraggstate->initValueIsNull;
}

/*
 * Given a new input value, advance the transition function of an aggregate.
 *
 * When called, CurrentMemoryContext should be the context we want the
 * transition function result to be delivered into on this cycle.
 */
static void
advance_transition_function(AggStatePerAgg peraggstate,
							Datum newVal, bool isNull)
{
	FunctionCallInfoData	fcinfo;

	if (peraggstate->transfn.fn_strict)
	{
		if (isNull)
		{
			/*
			 * For a strict transfn, nothing happens at a NULL input tuple;
			 * we just keep the prior transValue.  However, if the transtype
			 * is pass-by-ref, we have to copy it into the new context
			 * because the old one is going to get reset.
			 */
			if (!peraggstate->transValueIsNull)
				peraggstate->transValue = datumCopy(peraggstate->transValue,
												peraggstate->transtypeByVal,
												peraggstate->transtypeLen);
			return;
		}
		if (peraggstate->noTransValue)
		{
			/*
			 * transValue has not been initialized. This is the first non-NULL
			 * input value. We use it as the initial value for transValue.
			 * (We already checked that the agg's input type is binary-
			 * compatible with its transtype, so straight copy here is OK.)
			 *
			 * We had better copy the datum if it is pass-by-ref, since
			 * the given pointer may be pointing into a scan tuple that
			 * will be freed on the next iteration of the scan.
			 */
			peraggstate->transValue = datumCopy(newVal,
												peraggstate->transtypeByVal,
												peraggstate->transtypeLen);
			peraggstate->transValueIsNull = false;
			peraggstate->noTransValue = false;
			return;
		}
		if (peraggstate->transValueIsNull)
		{
			/*
			 * Don't call a strict function with NULL inputs.  Note it is
			 * possible to get here despite the above tests, if the transfn
			 * is strict *and* returned a NULL on a prior cycle.  If that
			 * happens we will propagate the NULL all the way to the end.
			 */
			return;
		}
	}

	/* OK to call the transition function */
	MemSet(&fcinfo, 0, sizeof(fcinfo));
	fcinfo.flinfo = &peraggstate->transfn;
	fcinfo.nargs = 2;
	fcinfo.arg[0] = peraggstate->transValue;
	fcinfo.argnull[0] = peraggstate->transValueIsNull;
	fcinfo.arg[1] = newVal;
	fcinfo.argnull[1] = isNull;

	newVal = FunctionCallInvoke(&fcinfo);

	/*
	 * If the transition function was uncooperative, it may have
	 * given us a pass-by-ref result that points at the scan tuple
	 * or the prior-cycle working memory.  Copy it into the active
	 * context if it doesn't look right.
	 */
	if (!peraggstate->transtypeByVal && !fcinfo.isnull &&
		! MemoryContextContains(CurrentMemoryContext,
								DatumGetPointer(newVal)))
		newVal = datumCopy(newVal,
						   peraggstate->transtypeByVal,
						   peraggstate->transtypeLen);

	peraggstate->transValue = newVal;
	peraggstate->transValueIsNull = fcinfo.isnull;
}

/*
 * Run the transition function for a DISTINCT aggregate.  This is called
 * after we have completed entering all the input values into the sort
 * object.  We complete the sort, read out the values in sorted order,
 * and run the transition function on each non-duplicate value.
 *
 * When called, CurrentMemoryContext should be the per-query context.
 */
static void
process_sorted_aggregate(AggState *aggstate,
						 AggStatePerAgg peraggstate)
{
	Datum		oldVal = (Datum) 0;
	bool		haveOldVal = false;
	MemoryContext oldContext;
	Datum		newVal;
	bool		isNull;

	tuplesort_performsort(peraggstate->sortstate);

	/*
	 * Note: if input type is pass-by-ref, the datums returned by the sort
	 * are freshly palloc'd in the per-query context, so we must be careful
	 * to pfree them when they are no longer needed.
	 */

	while (tuplesort_getdatum(peraggstate->sortstate, true,
							  &newVal, &isNull))
	{
		/*
		 * DISTINCT always suppresses nulls, per SQL spec, regardless of
		 * the transition function's strictness.
		 */
		if (isNull)
			continue;
		/*
		 * Clear and select the current working context for evaluation of
		 * the equality function and transition function.
		 */
		MemoryContextReset(aggstate->agg_cxt[aggstate->which_cxt]);
		oldContext =
			MemoryContextSwitchTo(aggstate->agg_cxt[aggstate->which_cxt]);

		if (haveOldVal &&
			DatumGetBool(FunctionCall2(&peraggstate->equalfn,
									   oldVal, newVal)))
		{
			/* equal to prior, so forget this one */
			if (!peraggstate->inputtypeByVal)
				pfree(DatumGetPointer(newVal));
			/*
			 * note we do NOT flip contexts in this case, so no need to
			 * copy prior transValue to other context.
			 */
		}
		else
		{
			advance_transition_function(peraggstate, newVal, false);
			/*
			 * Make the other context current so that this transition
			 * result is preserved.
			 */
			aggstate->which_cxt = 1 - aggstate->which_cxt;
			/* forget the old value, if any */
			if (haveOldVal && !peraggstate->inputtypeByVal)
				pfree(DatumGetPointer(oldVal));
			oldVal = newVal;
			haveOldVal = true;
		}

		MemoryContextSwitchTo(oldContext);
	}

	if (haveOldVal && !peraggstate->inputtypeByVal)
		pfree(DatumGetPointer(oldVal));

	tuplesort_end(peraggstate->sortstate);
	peraggstate->sortstate = NULL;
}

/*
 * Compute the final value of one aggregate.
 *
 * When called, CurrentMemoryContext should be the context where we want
 * final values delivered (ie, the per-output-tuple expression context).
 */
static void
finalize_aggregate(AggStatePerAgg peraggstate,
				   Datum *resultVal, bool *resultIsNull)
{
	/*
	 * Apply the agg's finalfn if one is provided, else return transValue.
	 */
	if (OidIsValid(peraggstate->finalfn_oid))
	{
		FunctionCallInfoData	fcinfo;

		MemSet(&fcinfo, 0, sizeof(fcinfo));
		fcinfo.flinfo = &peraggstate->finalfn;
		fcinfo.nargs = 1;
		fcinfo.arg[0] = peraggstate->transValue;
		fcinfo.argnull[0] = peraggstate->transValueIsNull;
		if (fcinfo.flinfo->fn_strict && peraggstate->transValueIsNull)
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
	else
	{
		*resultVal = peraggstate->transValue;
		*resultIsNull = peraggstate->transValueIsNull;
	}

	/*
	 * If result is pass-by-ref, make sure it is in the right context.
	 */
	if (!peraggstate->resulttypeByVal && ! *resultIsNull &&
		! MemoryContextContains(CurrentMemoryContext,
								DatumGetPointer(*resultVal)))
		*resultVal = datumCopy(*resultVal,
							   peraggstate->resulttypeByVal,
							   peraggstate->resulttypeLen);
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
	MemoryContext oldContext;
	TupleTableSlot *resultSlot;
	HeapTuple	inputTuple;
	int			aggno;
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
	 * We loop retrieving groups until we find one matching node->plan.qual
	 */
	do
	{
		if (aggstate->agg_done)
			return NULL;

		/*
		 * Clear the per-output-tuple context for each group
		 */
		MemoryContextReset(aggstate->tup_cxt);

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

			/*
			 * Clear and select the current working context for evaluation
			 * of the input expressions and transition functions at this
			 * input tuple.
			 */
			econtext->ecxt_per_tuple_memory =
				aggstate->agg_cxt[aggstate->which_cxt];
			ResetExprContext(econtext);
			oldContext =
				MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

			for (aggno = 0; aggno < aggstate->numaggs; aggno++)
			{
				AggStatePerAgg peraggstate = &peragg[aggno];
				Aggref	   *aggref = peraggstate->aggref;
				Datum		newVal;

				newVal = ExecEvalExpr(aggref->target, econtext,
									  &isNull, NULL);

				if (aggref->aggdistinct)
				{
					/* in DISTINCT mode, we may ignore nulls */
					if (isNull)
						continue;
					/* putdatum has to be called in per-query context */
					MemoryContextSwitchTo(oldContext);
					tuplesort_putdatum(peraggstate->sortstate,
									   newVal, isNull);
					MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
				}
				else
				{
					advance_transition_function(peraggstate,
												newVal, isNull);
				}
			}

			/*
			 * Make the other context current so that these transition
			 * results are preserved.
			 */
			aggstate->which_cxt = 1 - aggstate->which_cxt;

			MemoryContextSwitchTo(oldContext);

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
		 * calculation, and stash results in the per-output-tuple context.
		 *
		 * This is a bit tricky when there are both DISTINCT and plain
		 * aggregates: we must first finalize all the plain aggs and then all
		 * the DISTINCT ones.  This is needed because the last transition
		 * values for the plain aggs are stored in the not-current working
		 * context, and we have to evaluate those aggs (and stash the results
		 * in the output tup_cxt!) before we start flipping contexts again
		 * in process_sorted_aggregate.
		 */
		oldContext = MemoryContextSwitchTo(aggstate->tup_cxt);
		for (aggno = 0; aggno < aggstate->numaggs; aggno++)
		{
			AggStatePerAgg peraggstate = &peragg[aggno];

			if (! peraggstate->aggref->aggdistinct)
				finalize_aggregate(peraggstate,
								   &aggvalues[aggno], &aggnulls[aggno]);
		}
		MemoryContextSwitchTo(oldContext);
		for (aggno = 0; aggno < aggstate->numaggs; aggno++)
		{
			AggStatePerAgg peraggstate = &peragg[aggno];

			if (peraggstate->aggref->aggdistinct)
			{
				process_sorted_aggregate(aggstate, peraggstate);
				oldContext = MemoryContextSwitchTo(aggstate->tup_cxt);
				finalize_aggregate(peraggstate,
								   &aggvalues[aggno], &aggnulls[aggno]);
				MemoryContextSwitchTo(oldContext);
			}
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
			 * ExecProject. 99.44% of the time this is a waste of cycles,
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
		 * reserved for it.  The tuple will be deleted when it is cleared
		 * from the slot.
		 */
		ExecStoreTuple(inputTuple,
					   aggstate->csstate.css_ScanTupleSlot,
					   InvalidBuffer,
					   true);
		econtext->ecxt_scantuple = aggstate->csstate.css_ScanTupleSlot;

		/*
		 * Do projection and qual check in the per-output-tuple context.
		 */
		econtext->ecxt_per_tuple_memory = aggstate->tup_cxt;

		/*
		 * Form a projection tuple using the aggregate results and the
		 * representative input tuple.	Store it in the result tuple slot.
		 * Note we do not support aggregates returning sets ...
		 */
		resultSlot = ExecProject(projInfo, NULL);

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
	 * Create expression context
	 */
	ExecAssignExprContext(estate, &aggstate->csstate.cstate);

	/*
	 * We actually need three separate expression memory contexts: one
	 * for calculating per-output-tuple values (ie, the finished aggregate
	 * results), and two that we ping-pong between for per-input-tuple
	 * evaluation of input expressions and transition functions.  The
	 * context made by ExecAssignExprContext() is used as the output context.
	 */
	aggstate->tup_cxt =
		aggstate->csstate.cstate.cs_ExprContext->ecxt_per_tuple_memory;
	aggstate->agg_cxt[0] = 
		AllocSetContextCreate(CurrentMemoryContext,
							  "AggExprContext1",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);
	aggstate->agg_cxt[1] = 
		AllocSetContextCreate(CurrentMemoryContext,
							  "AggExprContext2",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);
	aggstate->which_cxt = 0;

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
		Oid			transfn_oid,
					finalfn_oid;

		/* Mark Aggref node with its associated index in the result array */
		aggref->aggno = aggno;

		/* Fill in the peraggstate data */
		peraggstate->aggref = aggref;

		aggTuple = SearchSysCache(AGGNAME,
								  PointerGetDatum(aggname),
								  ObjectIdGetDatum(aggref->basetype),
								  0, 0);
		if (!HeapTupleIsValid(aggTuple))
			elog(ERROR, "ExecAgg: cache lookup failed for aggregate %s(%s)",
				 aggname,
				 aggref->basetype ?
				 typeidTypeName(aggref->basetype) : (char *) "");
		aggform = (Form_pg_aggregate) GETSTRUCT(aggTuple);

		get_typlenbyval(aggform->aggfinaltype,
						&peraggstate->resulttypeLen,
						&peraggstate->resulttypeByVal);
		get_typlenbyval(aggform->aggtranstype,
						&peraggstate->transtypeLen,
						&peraggstate->transtypeByVal);

		peraggstate->initValue =
			AggNameGetInitVal(aggname,
							  aggform->aggbasetype,
							  &peraggstate->initValueIsNull);

		peraggstate->transfn_oid = transfn_oid = aggform->aggtransfn;
		peraggstate->finalfn_oid = finalfn_oid = aggform->aggfinalfn;

		fmgr_info(transfn_oid, &peraggstate->transfn);
		if (OidIsValid(finalfn_oid))
			fmgr_info(finalfn_oid, &peraggstate->finalfn);

		/*
		 * If the transfn is strict and the initval is NULL, make sure
		 * input type and transtype are the same (or at least binary-
		 * compatible), so that it's OK to use the first input value
		 * as the initial transValue.  This should have been checked at
		 * agg definition time, but just in case...
		 */
		if (peraggstate->transfn.fn_strict && peraggstate->initValueIsNull)
		{
			/*
			 * Note: use the type from the input expression here,
			 * not aggform->aggbasetype, because the latter might be 0.
			 * (Consider COUNT(*).)
			 */
			Oid			inputType = exprType(aggref->target);

			if (inputType != aggform->aggtranstype &&
				! IS_BINARY_COMPATIBLE(inputType, aggform->aggtranstype))
				elog(ERROR, "Aggregate %s needs to have compatible input type and transition type",
					 aggname);
		}

		if (aggref->aggdistinct)
		{
			/*
			 * Note: use the type from the input expression here,
			 * not aggform->aggbasetype, because the latter might be 0.
			 * (Consider COUNT(*).)
			 */
			Oid			inputType = exprType(aggref->target);
			Oid			eq_function;

			peraggstate->inputType = inputType;
			get_typlenbyval(inputType,
							&peraggstate->inputtypeLen,
							&peraggstate->inputtypeByVal);

			eq_function = compatible_oper_funcid("=", inputType, inputType,
												 true);
			if (!OidIsValid(eq_function))
				elog(ERROR, "Unable to identify an equality operator for type '%s'",
					 typeidTypeName(inputType));
			fmgr_info(eq_function, &(peraggstate->equalfn));
			peraggstate->sortOperator = any_ordering_op(inputType);
			peraggstate->sortstate = NULL;
		}

		ReleaseSysCache(aggTuple);
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
	/*
	 * Make sure ExecFreeExprContext() frees the right expr context...
	 */
	aggstate->csstate.cstate.cs_ExprContext->ecxt_per_tuple_memory =
		aggstate->tup_cxt;
	ExecFreeExprContext(&aggstate->csstate.cstate);
	/*
	 * ... and I free the others.
	 */
	MemoryContextDelete(aggstate->agg_cxt[0]);
	MemoryContextDelete(aggstate->agg_cxt[1]);

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
