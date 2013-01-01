/*-------------------------------------------------------------------------
 *
 * nodeValuesscan.c
 *	  Support routines for scanning Values lists
 *	  ("VALUES (...), (...), ..." in rangetable).
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
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
#include "parser/parsetree.h"


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
	List	   *exprlist;

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
		if (node->curr_idx < node->array_len)
			exprlist = node->exprlists[node->curr_idx];
		else
			exprlist = NIL;
	}
	else
	{
		if (node->curr_idx >= 0)
			node->curr_idx--;
		if (node->curr_idx >= 0)
			exprlist = node->exprlists[node->curr_idx];
		else
			exprlist = NIL;
	}

	/*
	 * Always clear the result slot; this is appropriate if we are at the end
	 * of the data, and if we're not, we still need it as the first step of
	 * the store-virtual-tuple protocol.  It seems wise to clear the slot
	 * before we reset the context it might have pointers into.
	 */
	ExecClearTuple(slot);

	if (exprlist)
	{
		MemoryContext oldContext;
		List	   *exprstatelist;
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
		 * Build the expression eval state in the econtext's per-tuple memory.
		 * This is a tad unusual, but we want to delete the eval state again
		 * when we move to the next row, to avoid growth of memory
		 * requirements over a long values list.
		 */
		oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

		/*
		 * Pass NULL, not my plan node, because we don't want anything in this
		 * transient state linking into permanent state.  The only possibility
		 * is a SubPlan, and there shouldn't be any (any subselects in the
		 * VALUES list should be InitPlans).
		 */
		exprstatelist = (List *) ExecInitExpr((Expr *) exprlist, NULL);

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

			values[resind] = ExecEvalExpr(estate,
										  econtext,
										  &isnull[resind],
										  NULL);
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
TupleTableSlot *
ExecValuesScan(ValuesScanState *node)
{
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
	RangeTblEntry *rte = rt_fetch(node->scan.scanrelid,
								  estate->es_range_table);
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

	/*
	 * Miscellaneous initialization
	 */
	planstate = &scanstate->ss.ps;

	/*
	 * Create expression contexts.	We need two, one for per-sublist
	 * processing and one for execScan.c to use for quals and projections. We
	 * cheat a little by using ExecAssignExprContext() to build both.
	 */
	ExecAssignExprContext(estate, planstate);
	scanstate->rowcontext = planstate->ps_ExprContext;
	ExecAssignExprContext(estate, planstate);

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &scanstate->ss.ps);
	ExecInitScanTupleSlot(estate, &scanstate->ss);

	/*
	 * initialize child expressions
	 */
	scanstate->ss.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->scan.plan.targetlist,
					 (PlanState *) scanstate);
	scanstate->ss.ps.qual = (List *)
		ExecInitExpr((Expr *) node->scan.plan.qual,
					 (PlanState *) scanstate);

	/*
	 * get info about values list
	 */
	tupdesc = ExecTypeFromExprList((List *) linitial(node->values_lists),
								   rte->eref->colnames);

	ExecAssignScanType(&scanstate->ss, tupdesc);

	/*
	 * Other node-specific setup
	 */
	scanstate->marked_idx = -1;
	scanstate->curr_idx = -1;
	scanstate->array_len = list_length(node->values_lists);

	/* convert list of sublists into array of sublists for easy addressing */
	scanstate->exprlists = (List **)
		palloc(scanstate->array_len * sizeof(List *));
	i = 0;
	foreach(vtl, node->values_lists)
	{
		scanstate->exprlists[i++] = (List *) lfirst(vtl);
	}

	scanstate->ss.ps.ps_TupFromTlist = false;

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&scanstate->ss.ps);
	ExecAssignScanProjectionInfo(&scanstate->ss);

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
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);
}

/* ----------------------------------------------------------------
 *		ExecValuesMarkPos
 *
 *		Marks scan position.
 * ----------------------------------------------------------------
 */
void
ExecValuesMarkPos(ValuesScanState *node)
{
	node->marked_idx = node->curr_idx;
}

/* ----------------------------------------------------------------
 *		ExecValuesRestrPos
 *
 *		Restores scan position.
 * ----------------------------------------------------------------
 */
void
ExecValuesRestrPos(ValuesScanState *node)
{
	node->curr_idx = node->marked_idx;
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
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	ExecScanReScan(&node->ss);

	node->curr_idx = -1;
}
