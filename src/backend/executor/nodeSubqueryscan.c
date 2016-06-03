/*-------------------------------------------------------------------------
 *
 * nodeSubqueryscan.c
 *	  Support routines for scanning subqueries (subselects in rangetable).
 *
 * This is just enough different from sublinks (nodeSubplan.c) to mean that
 * we need two sets of code.  Ought to look at trying to unify the cases.
 *
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeSubqueryscan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecSubqueryScan			scans a subquery.
 *		ExecSubqueryNext			retrieve next tuple in sequential order.
 *		ExecInitSubqueryScan		creates and initializes a subqueryscan node.
 *		ExecEndSubqueryScan			releases any storage allocated.
 *		ExecReScanSubqueryScan		rescans the relation
 *
 */
#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/nodeSubqueryscan.h"

static TupleTableSlot *SubqueryNext(SubqueryScanState *node);

/* ----------------------------------------------------------------
 *						Scan Support
 * ----------------------------------------------------------------
 */
/* ----------------------------------------------------------------
 *		SubqueryNext
 *
 *		This is a workhorse for ExecSubqueryScan
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
SubqueryNext(SubqueryScanState *node)
{
	TupleTableSlot *slot;

	/*
	 * Get the next tuple from the sub-query.
	 */
	slot = ExecProcNode(node->subplan);

	/*
	 * We just return the subplan's result slot, rather than expending extra
	 * cycles for ExecCopySlot().  (Our own ScanTupleSlot is used only for
	 * EvalPlanQual rechecks.)
	 */
	return slot;
}

/*
 * SubqueryRecheck -- access method routine to recheck a tuple in EvalPlanQual
 */
static bool
SubqueryRecheck(SubqueryScanState *node, TupleTableSlot *slot)
{
	/* nothing to check */
	return true;
}

/* ----------------------------------------------------------------
 *		ExecSubqueryScan(node)
 *
 *		Scans the subquery sequentially and returns the next qualifying
 *		tuple.
 *		We call the ExecScan() routine and pass it the appropriate
 *		access method functions.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecSubqueryScan(SubqueryScanState *node)
{
	return ExecScan(&node->ss,
					(ExecScanAccessMtd) SubqueryNext,
					(ExecScanRecheckMtd) SubqueryRecheck);
}

/* ----------------------------------------------------------------
 *		ExecInitSubqueryScan
 * ----------------------------------------------------------------
 */
SubqueryScanState *
ExecInitSubqueryScan(SubqueryScan *node, EState *estate, int eflags)
{
	SubqueryScanState *subquerystate;

	/* check for unsupported flags */
	Assert(!(eflags & EXEC_FLAG_MARK));

	/* SubqueryScan should not have any "normal" children */
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * create state structure
	 */
	subquerystate = makeNode(SubqueryScanState);
	subquerystate->ss.ps.plan = (Plan *) node;
	subquerystate->ss.ps.state = estate;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &subquerystate->ss.ps);

	/*
	 * initialize child expressions
	 */
	subquerystate->ss.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->scan.plan.targetlist,
					 (PlanState *) subquerystate);
	subquerystate->ss.ps.qual = (List *)
		ExecInitExpr((Expr *) node->scan.plan.qual,
					 (PlanState *) subquerystate);

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &subquerystate->ss.ps);
	ExecInitScanTupleSlot(estate, &subquerystate->ss);

	/*
	 * initialize subquery
	 */
	subquerystate->subplan = ExecInitNode(node->subplan, estate, eflags);

	subquerystate->ss.ps.ps_TupFromTlist = false;

	/*
	 * Initialize scan tuple type (needed by ExecAssignScanProjectionInfo)
	 */
	ExecAssignScanType(&subquerystate->ss,
					   ExecGetResultType(subquerystate->subplan));

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&subquerystate->ss.ps);
	ExecAssignScanProjectionInfo(&subquerystate->ss);

	return subquerystate;
}

/* ----------------------------------------------------------------
 *		ExecEndSubqueryScan
 *
 *		frees any storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndSubqueryScan(SubqueryScanState *node)
{
	/*
	 * Free the exprcontext
	 */
	ExecFreeExprContext(&node->ss.ps);

	/*
	 * clean out the upper tuple table
	 */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/*
	 * close down subquery
	 */
	ExecEndNode(node->subplan);
}

/* ----------------------------------------------------------------
 *		ExecReScanSubqueryScan
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecReScanSubqueryScan(SubqueryScanState *node)
{
	ExecScanReScan(&node->ss);

	/*
	 * ExecReScan doesn't know about my subplan, so I have to do
	 * changed-parameter signaling myself.  This is just as well, because the
	 * subplan has its own memory context in which its chgParam state lives.
	 */
	if (node->ss.ps.chgParam != NULL)
		UpdateChangedParamSet(node->subplan, node->ss.ps.chgParam);

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (node->subplan->chgParam == NULL)
		ExecReScan(node->subplan);
}
