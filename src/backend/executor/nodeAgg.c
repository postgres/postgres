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
 *	  keeping the previous transvalue.	If transfunc is not strict then it
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
 *	  transvalue that is in the other context.	At the beginning of each
 *	  tuple cycle we can reset the current output context to avoid memory
 *	  usage growth.  Note: we must use MemoryContextContains() to check
 *	  whether the transfunc has perhaps handed us back one of its input
 *	  values rather than a freshly palloc'd value; if so, we copy the value
 *	  to the context we want it in.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeAgg.c,v 1.91 2002/11/06 00:00:43 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_operator.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "executor/nodeGroup.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "utils/acl.h"
#include "utils/builtins.h"
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
	 * straight to the transition function.  If it's DISTINCT, we pass the
	 * input values into a Tuplesort object; then at completion of the
	 * input tuple group, we scan the sorted values, eliminate duplicates,
	 * and run the transition function on the rest.
	 */

	Tuplesortstate *sortstate;	/* sort object, if a DISTINCT agg */

	Datum		transValue;
	bool		transValueIsNull;

	bool		noTransValue;	/* true if transValue not set yet */

	/*
	 * Note: noTransValue initially has the same value as
	 * transValueIsNull, and if true both are cleared to false at the same
	 * time.  They are not the same though: if transfn later returns a
	 * NULL, we want to keep that NULL and not auto-replace it with a
	 * later input value. Only the first non-NULL input will be
	 * auto-substituted.
	 */
} AggStatePerAggData;


static void initialize_aggregate(AggStatePerAgg peraggstate);
static void advance_transition_function(AggStatePerAgg peraggstate,
							Datum newVal, bool isNull);
static void advance_aggregates(AggState *aggstate, ExprContext *econtext);
static void process_sorted_aggregate(AggState *aggstate,
						 AggStatePerAgg peraggstate);
static void finalize_aggregate(AggStatePerAgg peraggstate,
				   Datum *resultVal, bool *resultIsNull);
static Datum GetAggInitVal(Datum textInitVal, Oid transtype);


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
	 * without copying for each group.	Hence, transition function had
	 * better not scribble on its input, or it will fail for GROUP BY!
	 */
	peraggstate->transValue = peraggstate->initValue;
	peraggstate->transValueIsNull = peraggstate->initValueIsNull;

	/*
	 * If the initial value for the transition state doesn't exist in the
	 * pg_aggregate table then we will let the first non-NULL value
	 * returned from the outer procNode become the initial value. (This is
	 * useful for aggregates like max() and min().)  The noTransValue flag
	 * signals that we still need to do this.
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
	FunctionCallInfoData fcinfo;

	if (peraggstate->transfn.fn_strict)
	{
		if (isNull)
		{
			/*
			 * For a strict transfn, nothing happens at a NULL input
			 * tuple; we just keep the prior transValue.  However, if the
			 * transtype is pass-by-ref, we have to copy it into the new
			 * context because the old one is going to get reset.
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
			 * transValue has not been initialized. This is the first
			 * non-NULL input value. We use it as the initial value for
			 * transValue. (We already checked that the agg's input type
			 * is binary-compatible with its transtype, so straight copy
			 * here is OK.)
			 *
			 * We had better copy the datum if it is pass-by-ref, since the
			 * given pointer may be pointing into a scan tuple that will
			 * be freed on the next iteration of the scan.
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
			 * possible to get here despite the above tests, if the
			 * transfn is strict *and* returned a NULL on a prior cycle.
			 * If that happens we will propagate the NULL all the way to
			 * the end.
			 */
			return;
		}
	}

	/*
	 * OK to call the transition function
	 *
	 * This is heavily-used code, so manually zero just the necessary fields
	 * instead of using MemSet().  Compare FunctionCall2().
	 */

	/* MemSet(&fcinfo, 0, sizeof(fcinfo)); */
	fcinfo.context = NULL;
	fcinfo.resultinfo = NULL;
	fcinfo.isnull = false;

	fcinfo.flinfo = &peraggstate->transfn;
	fcinfo.nargs = 2;
	fcinfo.arg[0] = peraggstate->transValue;
	fcinfo.argnull[0] = peraggstate->transValueIsNull;
	fcinfo.arg[1] = newVal;
	fcinfo.argnull[1] = isNull;

	newVal = FunctionCallInvoke(&fcinfo);

	/*
	 * If the transition function was uncooperative, it may have given us
	 * a pass-by-ref result that points at the scan tuple or the
	 * prior-cycle working memory.	Copy it into the active context if it
	 * doesn't look right.
	 */
	if (!peraggstate->transtypeByVal && !fcinfo.isnull &&
		!MemoryContextContains(CurrentMemoryContext,
							   DatumGetPointer(newVal)))
		newVal = datumCopy(newVal,
						   peraggstate->transtypeByVal,
						   peraggstate->transtypeLen);

	peraggstate->transValue = newVal;
	peraggstate->transValueIsNull = fcinfo.isnull;
}

/*
 * Advance all the aggregates for one input tuple.  The input tuple
 * has been stored in econtext->ecxt_scantuple, so that it is accessible
 * to ExecEvalExpr.
 *
 * When called, CurrentMemoryContext should be the per-query context.
 */
static void
advance_aggregates(AggState *aggstate, ExprContext *econtext)
{
	MemoryContext oldContext;
	int			aggno;

	/*
	 * Clear and select the current working context for evaluation
	 * of the input expressions and transition functions at this
	 * input tuple.
	 */
	econtext->ecxt_per_tuple_memory = aggstate->agg_cxt[aggstate->which_cxt];
	ResetExprContext(econtext);
	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	for (aggno = 0; aggno < aggstate->numaggs; aggno++)
	{
		AggStatePerAgg peraggstate = &aggstate->peragg[aggno];
		Aggref	   *aggref = peraggstate->aggref;
		Datum		newVal;
		bool		isNull;

		newVal = ExecEvalExpr(aggref->target, econtext, &isNull, NULL);

		if (aggref->aggdistinct)
		{
			/* in DISTINCT mode, we may ignore nulls */
			if (isNull)
				continue;
			/* putdatum has to be called in per-query context */
			MemoryContextSwitchTo(oldContext);
			tuplesort_putdatum(peraggstate->sortstate, newVal, isNull);
			MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
		}
		else
		{
			advance_transition_function(peraggstate, newVal, isNull);
		}
	}

	/*
	 * Make the other context current so that these transition
	 * results are preserved.
	 */
	aggstate->which_cxt = 1 - aggstate->which_cxt;

	MemoryContextSwitchTo(oldContext);
}

/*
 * Run the transition function for a DISTINCT aggregate.  This is called
 * after we have completed entering all the input values into the sort
 * object.	We complete the sort, read out the values in sorted order,
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
	 * are freshly palloc'd in the per-query context, so we must be
	 * careful to pfree them when they are no longer needed.
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
		FunctionCallInfoData fcinfo;

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
	if (!peraggstate->resulttypeByVal && !*resultIsNull &&
		!MemoryContextContains(CurrentMemoryContext,
							   DatumGetPointer(*resultVal)))
		*resultVal = datumCopy(*resultVal,
							   peraggstate->resulttypeByVal,
							   peraggstate->resulttypeLen);
}


/*
 * ExecAgg -
 *
 *	  ExecAgg receives tuples from its outer subplan and aggregates over
 *	  the appropriate attribute for each aggregate function use (Aggref
 *	  node) appearing in the targetlist or qual of the node.  The number
 *	  of tuples to aggregate over depends on whether grouped or plain
 *	  aggregation is selected.  In grouped aggregation, we produce a result
 *	  row for each group; in plain aggregation there's a single result row
 *	  for the whole query.  In either case, the value of each aggregate is
 *	  stored in the expression context to be used when ExecProject evaluates
 *	  the result tuple.
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
	TupleTableSlot *outerslot;
	TupleTableSlot *firstSlot;
	TupleTableSlot *resultSlot;
	int			aggno;

	/*
	 * get state info from node
	 */
	aggstate = node->aggstate;
	estate = node->plan.state;
	outerPlan = outerPlan(node);
	econtext = aggstate->csstate.cstate.cs_ExprContext;
	aggvalues = econtext->ecxt_aggvalues;
	aggnulls = econtext->ecxt_aggnulls;
	projInfo = aggstate->csstate.cstate.cs_ProjInfo;
	peragg = aggstate->peragg;
	firstSlot = aggstate->csstate.css_ScanTupleSlot;

	/*
	 * We loop retrieving groups until we find one matching
	 * node->plan.qual
	 */
	do
	{
		if (aggstate->agg_done)
			return NULL;

		/*
		 * If we don't already have the first tuple of the new group,
		 * fetch it from the outer plan.
		 */
		if (aggstate->grp_firstTuple == NULL)
		{
			outerslot = ExecProcNode(outerPlan, (Plan *) node);
			if (!TupIsNull(outerslot))
			{
				/*
				 * Make a copy of the first input tuple; we will use this
				 * for comparisons (in group mode) and for projection.
				 */
				aggstate->grp_firstTuple = heap_copytuple(outerslot->val);
			}
			else
			{
				/* outer plan produced no tuples at all */
				aggstate->agg_done = true;
				/* If we are grouping, we should produce no tuples too */
				if (node->aggstrategy != AGG_PLAIN)
					return NULL;
			}
		}

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

		if (aggstate->grp_firstTuple != NULL)
		{
			/*
			 * Store the copied first input tuple in the tuple table slot
			 * reserved for it.  The tuple will be deleted when it is
			 * cleared from the slot.
			 */
			ExecStoreTuple(aggstate->grp_firstTuple,
						   firstSlot,
						   InvalidBuffer,
						   true);
			aggstate->grp_firstTuple = NULL; /* don't keep two pointers */

			/* set up for first advance_aggregates call */
			econtext->ecxt_scantuple = firstSlot;

			/*
			 * Process each outer-plan tuple, and then fetch the next one,
			 * until we exhaust the outer plan or cross a group boundary.
			 */
			for (;;)
			{
				advance_aggregates(aggstate, econtext);

				outerslot = ExecProcNode(outerPlan, (Plan *) node);
				if (TupIsNull(outerslot))
				{
					/* no more outer-plan tuples available */
					aggstate->agg_done = true;
					break;
				}
				/* set up for next advance_aggregates call */
				econtext->ecxt_scantuple = outerslot;

				/*
				 * If we are grouping, check whether we've crossed a group
				 * boundary.
				 */
				if (node->aggstrategy == AGG_SORTED)
				{
					if (!execTuplesMatch(firstSlot->val,
										 outerslot->val,
										 firstSlot->ttc_tupleDescriptor,
										 node->numCols, node->grpColIdx,
										 aggstate->eqfunctions,
										 aggstate->agg_cxt[aggstate->which_cxt]))
					{
						/*
						 * Save the first input tuple of the next group.
						 */
						aggstate->grp_firstTuple = heap_copytuple(outerslot->val);
						break;
					}
				}
			}
		}

		/*
		 * Done scanning input tuple group. Finalize each aggregate
		 * calculation, and stash results in the per-output-tuple context.
		 *
		 * This is a bit tricky when there are both DISTINCT and plain
		 * aggregates: we must first finalize all the plain aggs and then
		 * all the DISTINCT ones.  This is needed because the last
		 * transition values for the plain aggs are stored in the
		 * not-current working context, and we have to evaluate those aggs
		 * (and stash the results in the output tup_cxt!) before we start
		 * flipping contexts again in process_sorted_aggregate.
		 */
		oldContext = MemoryContextSwitchTo(aggstate->tup_cxt);
		for (aggno = 0; aggno < aggstate->numaggs; aggno++)
		{
			AggStatePerAgg peraggstate = &peragg[aggno];

			if (!peraggstate->aggref->aggdistinct)
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
		 * If we have no first tuple (ie, the outerPlan didn't return
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
		if (TupIsNull(firstSlot))
		{
			TupleDesc	tupType;

			/* Should only happen in non-grouped mode */
			Assert(node->aggstrategy == AGG_PLAIN);
			Assert(aggstate->agg_done);

			tupType = firstSlot->ttc_tupleDescriptor;
			/* watch out for zero-column input tuples, though... */
			if (tupType && tupType->natts > 0)
			{
				HeapTuple	nullsTuple;
				Datum	   *dvalues;
				char	   *dnulls;

				dvalues = (Datum *) palloc(sizeof(Datum) * tupType->natts);
				dnulls = (char *) palloc(sizeof(char) * tupType->natts);
				MemSet(dvalues, 0, sizeof(Datum) * tupType->natts);
				MemSet(dnulls, 'n', sizeof(char) * tupType->natts);
				nullsTuple = heap_formtuple(tupType, dvalues, dnulls);
				ExecStoreTuple(nullsTuple,
							   firstSlot,
							   InvalidBuffer,
							   true);
				pfree(dvalues);
				pfree(dnulls);
			}
		}

		/*
		 * Do projection and qual check in the per-output-tuple context.
		 */
		econtext->ecxt_per_tuple_memory = aggstate->tup_cxt;

		/*
		 * Form a projection tuple using the aggregate results and the
		 * representative input tuple.	Store it in the result tuple slot.
		 * Note we do not support aggregates returning sets ...
		 */
		econtext->ecxt_scantuple = firstSlot;
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
	aggstate->eqfunctions = NULL;
	aggstate->grp_firstTuple = NULL;
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
		 * This is not an error condition: we might be using the Agg node just
		 * to do hash-based grouping.  Even in the regular case,
		 * constant-expression simplification could optimize away all of the
		 * Aggrefs in the targetlist and qual.  So keep going, but force local
		 * copy of numaggs positive so that palloc()s below don't choke.
		 */
		numaggs = 1;
	}

	/*
	 * Create expression context
	 */
	ExecAssignExprContext(estate, &aggstate->csstate.cstate);

	/*
	 * We actually need three separate expression memory contexts: one for
	 * calculating per-output-tuple values (ie, the finished aggregate
	 * results), and two that we ping-pong between for per-input-tuple
	 * evaluation of input expressions and transition functions.  The
	 * context made by ExecAssignExprContext() is used as the output
	 * context.
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

	/*
	 * initialize source tuple type.
	 */
	ExecAssignScanTypeFromOuterPlan((Plan *) node, &aggstate->csstate);

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL((Plan *) node, &aggstate->csstate.cstate);
	ExecAssignProjectionInfo((Plan *) node, &aggstate->csstate.cstate);

	/*
	 * If we are grouping, precompute fmgr lookup data for inner loop
	 */
	if (node->numCols > 0)
	{
		aggstate->eqfunctions =
			execTuplesMatchPrepare(ExecGetScanType(&aggstate->csstate),
								   node->numCols,
								   node->grpColIdx);
	}

	/*
	 * Perform lookups of aggregate function info, and initialize the
	 * unchanging fields of the per-agg data
	 */
	aggno = -1;
	foreach(alist, aggstate->aggs)
	{
		Aggref	   *aggref = (Aggref *) lfirst(alist);
		AggStatePerAgg peraggstate = &peragg[++aggno];
		HeapTuple	aggTuple;
		Form_pg_aggregate aggform;
		AclResult	aclresult;
		Oid			transfn_oid,
					finalfn_oid;
		Datum		textInitVal;

		/* Mark Aggref node with its associated index in the result array */
		aggref->aggno = aggno;

		/* Fill in the peraggstate data */
		peraggstate->aggref = aggref;

		aggTuple = SearchSysCache(AGGFNOID,
								  ObjectIdGetDatum(aggref->aggfnoid),
								  0, 0, 0);
		if (!HeapTupleIsValid(aggTuple))
			elog(ERROR, "ExecAgg: cache lookup failed for aggregate %u",
				 aggref->aggfnoid);
		aggform = (Form_pg_aggregate) GETSTRUCT(aggTuple);

		/* Check permission to call aggregate function */
		aclresult = pg_proc_aclcheck(aggref->aggfnoid, GetUserId(),
									 ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, get_func_name(aggref->aggfnoid));

		get_typlenbyval(aggref->aggtype,
						&peraggstate->resulttypeLen,
						&peraggstate->resulttypeByVal);
		get_typlenbyval(aggform->aggtranstype,
						&peraggstate->transtypeLen,
						&peraggstate->transtypeByVal);

		/*
		 * initval is potentially null, so don't try to access it as a
		 * struct field. Must do it the hard way with SysCacheGetAttr.
		 */
		textInitVal = SysCacheGetAttr(AGGFNOID, aggTuple,
									  Anum_pg_aggregate_agginitval,
									  &peraggstate->initValueIsNull);

		if (peraggstate->initValueIsNull)
			peraggstate->initValue = (Datum) 0;
		else
			peraggstate->initValue = GetAggInitVal(textInitVal,
												   aggform->aggtranstype);

		peraggstate->transfn_oid = transfn_oid = aggform->aggtransfn;
		peraggstate->finalfn_oid = finalfn_oid = aggform->aggfinalfn;

		fmgr_info(transfn_oid, &peraggstate->transfn);
		if (OidIsValid(finalfn_oid))
			fmgr_info(finalfn_oid, &peraggstate->finalfn);

		/*
		 * If the transfn is strict and the initval is NULL, make sure
		 * input type and transtype are the same (or at least binary-
		 * compatible), so that it's OK to use the first input value as
		 * the initial transValue.	This should have been checked at agg
		 * definition time, but just in case...
		 */
		if (peraggstate->transfn.fn_strict && peraggstate->initValueIsNull)
		{
			/*
			 * Note: use the type from the input expression here, not from
			 * pg_proc.proargtypes, because the latter might be 0.
			 * (Consider COUNT(*).)
			 */
			Oid			inputType = exprType(aggref->target);

			if (!IsBinaryCoercible(inputType, aggform->aggtranstype))
				elog(ERROR, "Aggregate %u needs to have compatible input type and transition type",
					 aggref->aggfnoid);
		}

		if (aggref->aggdistinct)
		{
			/*
			 * Note: use the type from the input expression here, not from
			 * pg_proc.proargtypes, because the latter might be 0.
			 * (Consider COUNT(*).)
			 */
			Oid			inputType = exprType(aggref->target);
			Oid			eq_function;

			peraggstate->inputType = inputType;
			get_typlenbyval(inputType,
							&peraggstate->inputtypeLen,
							&peraggstate->inputtypeByVal);

			eq_function = compatible_oper_funcid(makeList1(makeString("=")),
												 inputType, inputType,
												 true);
			if (!OidIsValid(eq_function))
				elog(ERROR, "Unable to identify an equality operator for type %s",
					 format_type_be(inputType));
			fmgr_info(eq_function, &(peraggstate->equalfn));
			peraggstate->sortOperator = any_ordering_op(inputType);
			peraggstate->sortstate = NULL;
		}

		ReleaseSysCache(aggTuple);
	}

	return TRUE;
}

static Datum
GetAggInitVal(Datum textInitVal, Oid transtype)
{
	char	   *strInitVal;
	HeapTuple	tup;
	Oid			typinput,
				typelem;
	Datum		initVal;

	strInitVal = DatumGetCString(DirectFunctionCall1(textout, textInitVal));

	tup = SearchSysCache(TYPEOID,
						 ObjectIdGetDatum(transtype),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "GetAggInitVal: cache lookup failed on aggregate transition function return type %u", transtype);

	typinput = ((Form_pg_type) GETSTRUCT(tup))->typinput;
	typelem = ((Form_pg_type) GETSTRUCT(tup))->typelem;
	ReleaseSysCache(tup);

	initVal = OidFunctionCall3(typinput,
							   CStringGetDatum(strInitVal),
							   ObjectIdGetDatum(typelem),
							   Int32GetDatum(-1));

	pfree(strInitVal);
	return initVal;
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
	if (aggstate->grp_firstTuple != NULL)
	{
		heap_freetuple(aggstate->grp_firstTuple);
		aggstate->grp_firstTuple = NULL;
	}
}

void
ExecReScanAgg(Agg *node, ExprContext *exprCtxt, Plan *parent)
{
	AggState   *aggstate = node->aggstate;
	ExprContext *econtext = aggstate->csstate.cstate.cs_ExprContext;

	aggstate->agg_done = false;
	if (aggstate->grp_firstTuple != NULL)
	{
		heap_freetuple(aggstate->grp_firstTuple);
		aggstate->grp_firstTuple = NULL;
	}
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
 * aggregate_dummy - dummy execution routine for aggregate functions
 *
 * This function is listed as the implementation (prosrc field) of pg_proc
 * entries for aggregate functions.  Its only purpose is to throw an error
 * if someone mistakenly executes such a function in the normal way.
 *
 * Perhaps someday we could assign real meaning to the prosrc field of
 * an aggregate?
 */
Datum
aggregate_dummy(PG_FUNCTION_ARGS)
{
	elog(ERROR, "Aggregate function %u called as normal function",
		 fcinfo->flinfo->fn_oid);
	return (Datum) 0;			/* keep compiler quiet */
}
