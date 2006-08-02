/*-------------------------------------------------------------------------
 *
 * nodeValuesscan.c
 *	  Support routines for scanning Values lists
 *    ("VALUES (...), (...), ..." in rangetable).
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeValuesscan.c,v 1.1 2006/08/02 01:59:45 joe Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecValuesScan			scans a values list.
 *		ExecValuesNext			retrieve next tuple in sequential order.
 *		ExecInitValuesScan		creates and initializes a valuesscan node.
 *		ExecEndValuesScan		releases any storage allocated.
 *		ExecValuesReScan		rescans the values list
 */
#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeValuesscan.h"
#include "parser/parsetree.h"
#include "utils/memutils.h"


static TupleTableSlot *ValuesNext(ValuesScanState *node);
static void ExecMakeValuesResult(List *targetlist,
					 ExprContext *econtext,
					 TupleTableSlot *slot);


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
	EState		   *estate;
	ExprContext	   *econtext;
	ScanDirection	direction;
	List		   *exprlist;

	/*
	 * get information from the estate and scan state
	 */
	estate = node->ss.ps.state;
	direction = estate->es_direction;
	slot = node->ss.ss_ScanTupleSlot;
	econtext = node->ss.ps.ps_ExprContext;

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

	if (exprlist)
	{
		List		   *init_exprlist;

		init_exprlist = (List *) ExecInitExpr((Expr *) exprlist,
											  (PlanState *) node);
		ExecMakeValuesResult(init_exprlist,
							 econtext,
							 slot);
		list_free_deep(init_exprlist);
	}
	else
		ExecClearTuple(slot);

	return slot;
}

/*
 *		ExecMakeValuesResult
 *
 * Evaluate a values list, store into a virtual slot.
 */
static void
ExecMakeValuesResult(List *targetlist,
					 ExprContext *econtext,
					 TupleTableSlot *slot)
{
	MemoryContext		oldContext;
	Datum	   *values;
	bool	   *isnull;
	ListCell		   *lc;
	int					resind = 0;

	/* caller should have checked all targetlists are the same length */
	Assert(list_length(targetlist) == slot->tts_tupleDescriptor->natts);

	/*
	 * Prepare to build a virtual result tuple.
	 */
	ExecClearTuple(slot);
	values = slot->tts_values;
	isnull = slot->tts_isnull;

	/*
	 * Switch to short-lived context for evaluating the row.
	 * Reset per-tuple memory context before each row.
	 */
	ResetExprContext(econtext);
	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	foreach(lc, targetlist)
	{
		ExprState *estate = (ExprState *) lfirst(lc);

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


/* ----------------------------------------------------------------
 *		ExecValuesScan(node)
 *
 *		Scans the values lists sequentially and returns the next qualifying
 *		tuple.
 *		It calls the ExecScan() routine and passes it the access method
 *		which retrieves tuples sequentially.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecValuesScan(ValuesScanState *node)
{
	/*
	 * use ValuesNext as access method
	 */
	return ExecScan(&node->ss, (ExecScanAccessMtd) ValuesNext);
}

/* ----------------------------------------------------------------
 *		ExecInitValuesScan
 * ----------------------------------------------------------------
 */
ValuesScanState *
ExecInitValuesScan(ValuesScan *node, EState *estate, int eflags)
{
	ValuesScanState	   *scanstate;
	RangeTblEntry	   *rte;
	TupleDesc			tupdesc;
	ListCell		   *vtl;
	int					i;
	PlanState		   *planstate;
	ExprContext		   *econtext;

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
	 *
	 * create expression context for node
	 */
	planstate = &scanstate->ss.ps;
	ExecAssignExprContext(estate, planstate);
	econtext = planstate->ps_ExprContext;

#define VALUESSCAN_NSLOTS 2

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
	rte = rt_fetch(node->scan.scanrelid, estate->es_range_table);
	Assert(rte->rtekind == RTE_VALUES);
	tupdesc = ExecTypeFromExprList((List *) linitial(rte->values_lists));

	ExecAssignScanType(&scanstate->ss, tupdesc);

	/*
	 * Other node-specific setup
	 */
	scanstate->marked_idx = -1;
	scanstate->curr_idx = -1;
	scanstate->array_len = list_length(rte->values_lists);

	/* convert list of sublists into array of sublists for easy addressing */
	scanstate->exprlists = (List **)
		palloc(scanstate->array_len * sizeof(List *));
	i = 0;
	foreach(vtl, rte->values_lists)
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

int
ExecCountSlotsValuesScan(ValuesScan *node)
{
	return ExecCountSlotsNode(outerPlan(node)) +
		ExecCountSlotsNode(innerPlan(node)) +
		VALUESSCAN_NSLOTS;
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
	 * Free the exprcontext
	 */
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
 *		ExecValuesReScan
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecValuesReScan(ValuesScanState *node, ExprContext *exprCtxt)
{
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	node->curr_idx = -1;
}
