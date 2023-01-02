/*-------------------------------------------------------------------------
 *
 * nodeValuesscan.c
 *	  Support routines for scanning Values lists
 *	  ("VALUES (...), (...), ..." in rangetable).
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeValuesscan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecValuesScan			scans a values list.
 *		ExecValuesNext			retrieve next tuple in sequential order.
 *		ExecInitValuesScan		creates and initializes a valuesscan node.
 *		ExecEndValuesScan		releases any storage allocated.
 *		ExecReScanValuesScan	rescans the values list
 */
#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeValuesscan.h"
#include "jit/jit.h"
#include "optimizer/clauses.h"
#include "utils/expandeddatum.h"


static TupleTableSlot *ValuesNext(ValuesScanState *node);


/* ----------------------------------------------------------------
 *						Scan Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ValuesNext
 *
 *		This is a workhorse for ExecValuesScan
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ValuesNext(ValuesScanState *node)
{
	TupleTableSlot *slot;
	EState	   *estate;
	ExprContext *econtext;
	ScanDirection direction;
	int			curr_idx;

	/*
	 * get information from the estate and scan state
	 */
	estate = node->ss.ps.state;
	direction = estate->es_direction;
	slot = node->ss.ss_ScanTupleSlot;
	econtext = node->rowcontext;

	/*
	 * Get the next tuple. Return NULL if no more tuples.
	 */
	if (ScanDirectionIsForward(direction))
	{
		if (node->curr_idx < node->array_len)
			node->curr_idx++;
	}
	else
	{
		if (node->curr_idx >= 0)
			node->curr_idx--;
	}

	/*
	 * Always clear the result slot; this is appropriate if we are at the end
	 * of the data, and if we're not, we still need it as the first step of
	 * the store-virtual-tuple protocol.  It seems wise to clear the slot
	 * before we reset the context it might have pointers into.
	 */
	ExecClearTuple(slot);

	curr_idx = node->curr_idx;
	if (curr_idx >= 0 && curr_idx < node->array_len)
	{
		List	   *exprlist = node->exprlists[curr_idx];
		List	   *exprstatelist = node->exprstatelists[curr_idx];
		MemoryContext oldContext;
		Datum	   *values;
		bool	   *isnull;
		ListCell   *lc;
		int			resind;

		/*
		 * Get rid of any prior cycle's leftovers.  We use ReScanExprContext
		 * not just ResetExprContext because we want any registered shutdown
		 * callbacks to be called.
		 */
		ReScanExprContext(econtext);

		/*
		 * Do per-VALUES-row work in the per-tuple context.
		 */
		oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

		/*
		 * Unless we already made the expression eval state for this row,
		 * build it in the econtext's per-tuple memory.  This is a tad
		 * unusual, but we want to delete the eval state again when we move to
		 * the next row, to avoid growth of memory requirements over a long
		 * values list.  For rows in which that won't work, we already built
		 * the eval state at plan startup.
		 */
		if (exprstatelist == NIL)
		{
			/*
			 * Pass parent as NULL, not my plan node, because we don't want
			 * anything in this transient state linking into permanent state.
			 * The only expression type that might wish to do so is a SubPlan,
			 * and we already checked that there aren't any.
			 *
			 * Note that passing parent = NULL also disables JIT compilation
			 * of the expressions, which is a win, because they're only going
			 * to be used once under normal circumstances.
			 */
			exprstatelist = ExecInitExprList(exprlist, NULL);
		}

		/* parser should have checked all sublists are the same length */
		Assert(list_length(exprstatelist) == slot->tts_tupleDescriptor->natts);

		/*
		 * Compute the expressions and build a virtual result tuple. We
		 * already did ExecClearTuple(slot).
		 */
		values = slot->tts_values;
		isnull = slot->tts_isnull;

		resind = 0;
		foreach(lc, exprstatelist)
		{
			ExprState  *estate = (ExprState *) lfirst(lc);
			Form_pg_attribute attr = TupleDescAttr(slot->tts_tupleDescriptor,
												   resind);

			values[resind] = ExecEvalExpr(estate,
										  econtext,
										  &isnull[resind]);

			/*
			 * We must force any R/W expanded datums to read-only state, in
			 * case they are multiply referenced in the plan node's output
			 * expressions, or in case we skip the output projection and the
			 * output column is multiply referenced in higher plan nodes.
			 */
			values[resind] = MakeExpandedObjectReadOnly(values[resind],
														isnull[resind],
														attr->attlen);

			resind++;
		}

		MemoryContextSwitchTo(oldContext);

		/*
		 * And return the virtual tuple.
		 */
		ExecStoreVirtualTuple(slot);
	}

	return slot;
}

/*
 * ValuesRecheck -- access method routine to recheck a tuple in EvalPlanQual
 */
static bool
ValuesRecheck(ValuesScanState *node, TupleTableSlot *slot)
{
	/* nothing to check */
	return true;
}

/* ----------------------------------------------------------------
 *		ExecValuesScan(node)
 *
 *		Scans the values lists sequentially and returns the next qualifying
 *		tuple.
 *		We call the ExecScan() routine and pass it the appropriate
 *		access method functions.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecValuesScan(PlanState *pstate)
{
	ValuesScanState *node = castNode(ValuesScanState, pstate);

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) ValuesNext,
					(ExecScanRecheckMtd) ValuesRecheck);
}

/* ----------------------------------------------------------------
 *		ExecInitValuesScan
 * ----------------------------------------------------------------
 */
ValuesScanState *
ExecInitValuesScan(ValuesScan *node, EState *estate, int eflags)
{
	ValuesScanState *scanstate;
	TupleDesc	tupdesc;
	ListCell   *vtl;
	int			i;
	PlanState  *planstate;

	/*
	 * ValuesScan should not have any children.
	 */
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * create new ScanState for node
	 */
	scanstate = makeNode(ValuesScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;
	scanstate->ss.ps.ExecProcNode = ExecValuesScan;

	/*
	 * Miscellaneous initialization
	 */
	planstate = &scanstate->ss.ps;

	/*
	 * Create expression contexts.  We need two, one for per-sublist
	 * processing and one for execScan.c to use for quals and projections. We
	 * cheat a little by using ExecAssignExprContext() to build both.
	 */
	ExecAssignExprContext(estate, planstate);
	scanstate->rowcontext = planstate->ps_ExprContext;
	ExecAssignExprContext(estate, planstate);

	/*
	 * Get info about values list, initialize scan slot with it.
	 */
	tupdesc = ExecTypeFromExprList((List *) linitial(node->values_lists));
	ExecInitScanTupleSlot(estate, &scanstate->ss, tupdesc, &TTSOpsVirtual);

	/*
	 * Initialize result type and projection.
	 */
	ExecInitResultTypeTL(&scanstate->ss.ps);
	ExecAssignScanProjectionInfo(&scanstate->ss);

	/*
	 * initialize child expressions
	 */
	scanstate->ss.ps.qual =
		ExecInitQual(node->scan.plan.qual, (PlanState *) scanstate);

	/*
	 * Other node-specific setup
	 */
	scanstate->curr_idx = -1;
	scanstate->array_len = list_length(node->values_lists);

	/*
	 * Convert the list of expression sublists into an array for easier
	 * addressing at runtime.  Also, detect whether any sublists contain
	 * SubPlans; for just those sublists, go ahead and do expression
	 * initialization.  (This avoids problems with SubPlans wanting to connect
	 * themselves up to the outer plan tree.  Notably, EXPLAIN won't see the
	 * subplans otherwise; also we will have troubles with dangling pointers
	 * and/or leaked resources if we try to handle SubPlans the same as
	 * simpler expressions.)
	 */
	scanstate->exprlists = (List **)
		palloc(scanstate->array_len * sizeof(List *));
	scanstate->exprstatelists = (List **)
		palloc0(scanstate->array_len * sizeof(List *));
	i = 0;
	foreach(vtl, node->values_lists)
	{
		List	   *exprs = lfirst_node(List, vtl);

		scanstate->exprlists[i] = exprs;

		/*
		 * We can avoid the cost of a contain_subplans() scan in the simple
		 * case where there are no SubPlans anywhere.
		 */
		if (estate->es_subplanstates &&
			contain_subplans((Node *) exprs))
		{
			int			saved_jit_flags;

			/*
			 * As these expressions are only used once, disable JIT for them.
			 * This is worthwhile because it's common to insert significant
			 * amounts of data via VALUES().  Note that this doesn't prevent
			 * use of JIT *within* a subplan, since that's initialized
			 * separately; this just affects the upper-level subexpressions.
			 */
			saved_jit_flags = estate->es_jit_flags;
			estate->es_jit_flags = PGJIT_NONE;

			scanstate->exprstatelists[i] = ExecInitExprList(exprs,
															&scanstate->ss.ps);

			estate->es_jit_flags = saved_jit_flags;
		}
		i++;
	}

	return scanstate;
}

/* ----------------------------------------------------------------
 *		ExecEndValuesScan
 *
 *		frees any storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndValuesScan(ValuesScanState *node)
{
	/*
	 * Free both exprcontexts
	 */
	ExecFreeExprContext(&node->ss.ps);
	node->ss.ps.ps_ExprContext = node->rowcontext;
	ExecFreeExprContext(&node->ss.ps);

	/*
	 * clean out the tuple table
	 */
	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);
}

/* ----------------------------------------------------------------
 *		ExecReScanValuesScan
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecReScanValuesScan(ValuesScanState *node)
{
	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	ExecScanReScan(&node->ss);

	node->curr_idx = -1;
}
