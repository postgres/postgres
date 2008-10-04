/*-------------------------------------------------------------------------
 *
 * nodeWorktablescan.c
 *	  routines to handle WorkTableScan nodes.
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeWorktablescan.c,v 1.1 2008/10/04 21:56:53 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/nodeWorktablescan.h"

static TupleTableSlot *WorkTableScanNext(WorkTableScanState *node);

/* ----------------------------------------------------------------
 *		WorkTableScanNext
 *
 *		This is a workhorse for ExecWorkTableScan
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
WorkTableScanNext(WorkTableScanState *node)
{
	TupleTableSlot *slot;
	EState	   *estate;
	ScanDirection direction;
	Tuplestorestate *tuplestorestate;

	/*
	 * get information from the estate and scan state
	 */
	estate = node->ss.ps.state;
	direction = estate->es_direction;

	tuplestorestate = node->rustate->working_table;

	/*
	 * Get the next tuple from tuplestore. Return NULL if no more tuples.
	 */
	slot = node->ss.ss_ScanTupleSlot;
	(void) tuplestore_gettupleslot(tuplestorestate,
								   ScanDirectionIsForward(direction),
								   slot);
	return slot;
}

/* ----------------------------------------------------------------
 *		ExecWorkTableScan(node)
 *
 *		Scans the worktable sequentially and returns the next qualifying tuple.
 *		It calls the ExecScan() routine and passes it the access method
 *		which retrieves tuples sequentially.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecWorkTableScan(WorkTableScanState *node)
{
	/*
	 * use WorkTableScanNext as access method
	 */
	return ExecScan(&node->ss, (ExecScanAccessMtd) WorkTableScanNext);
}


/* ----------------------------------------------------------------
 *		ExecInitWorkTableScan
 * ----------------------------------------------------------------
 */
WorkTableScanState *
ExecInitWorkTableScan(WorkTableScan *node, EState *estate, int eflags)
{
	WorkTableScanState *scanstate;
	ParamExecData *prmdata;

	/* check for unsupported flags */
	Assert(!(eflags & EXEC_FLAG_MARK));

	/*
	 * WorkTableScan should not have any children.
	 */
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * create new WorkTableScanState for node
	 */
	scanstate = makeNode(WorkTableScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;

	/*
	 * Find the ancestor RecursiveUnion's state
	 * via the Param slot reserved for it.
	 */
	prmdata = &(estate->es_param_exec_vals[node->wtParam]);
	Assert(prmdata->execPlan == NULL);
	Assert(!prmdata->isnull);
	scanstate->rustate = (RecursiveUnionState *) DatumGetPointer(prmdata->value);
	Assert(scanstate->rustate && IsA(scanstate->rustate, RecursiveUnionState));

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	/*
	 * initialize child expressions
	 */
	scanstate->ss.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->scan.plan.targetlist,
					 (PlanState *) scanstate);
	scanstate->ss.ps.qual = (List *)
		ExecInitExpr((Expr *) node->scan.plan.qual,
					 (PlanState *) scanstate);

#define WORKTABLESCAN_NSLOTS 2

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &scanstate->ss.ps);
	ExecInitScanTupleSlot(estate, &scanstate->ss);

	/*
	 * The scan tuple type (ie, the rowtype we expect to find in the work
	 * table) is the same as the result rowtype of the ancestor RecursiveUnion
	 * node.  Note this depends on the assumption that RecursiveUnion doesn't
	 * allow projection.
	 */
	ExecAssignScanType(&scanstate->ss,
					   ExecGetResultType(&scanstate->rustate->ps));

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&scanstate->ss.ps);
	ExecAssignScanProjectionInfo(&scanstate->ss);

	scanstate->ss.ps.ps_TupFromTlist = false;

	return scanstate;
}

int
ExecCountSlotsWorkTableScan(WorkTableScan *node)
{
	return ExecCountSlotsNode(outerPlan(node)) +
		ExecCountSlotsNode(innerPlan(node)) +
		WORKTABLESCAN_NSLOTS;
}

/* ----------------------------------------------------------------
 *		ExecEndWorkTableScan
 *
 *		frees any storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndWorkTableScan(WorkTableScanState *node)
{
	/*
	 * Free exprcontext
	 */
	ExecFreeExprContext(&node->ss.ps);

	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);
}

/* ----------------------------------------------------------------
 *		ExecWorkTableScanReScan
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecWorkTableScanReScan(WorkTableScanState *node, ExprContext *exprCtxt)
{
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	tuplestore_rescan(node->rustate->working_table);
}
