/*-------------------------------------------------------------------------
 *
 * nodeSubqueryscan.c
 *	  Support routines for scanning subqueries (subselects in rangetable).
 *
 * This is just enough different from sublinks (nodeSubplan.c) to mean that
 * we need two sets of code.  Ought to look at trying to unify the cases.
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
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
static TupleTableSlot *
ExecSubqueryScan(PlanState *pstate)
{
	SubqueryScanState *node = castNode(SubqueryScanState, pstate);

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
	subquerystate->ss.ps.ExecProcNode = ExecSubqueryScan;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &subquerystate->ss.ps);

	/*
	 * initialize subquery
	 */
	subquerystate->subplan = ExecInitNode(node->subplan, estate, eflags);

	/*
	 * Initialize scan slot and type (needed by ExecAssignScanProjectionInfo)
	 */
	ExecInitScanTupleSlot(estate, &subquerystate->ss,
						  ExecGetResultType(subquerystate->subplan),
						  ExecGetResultSlotOps(subquerystate->subplan, NULL));

	/*
	 * The slot used as the scantuple isn't the slot above (outside of EPQ),
	 * but the one from the node below.
	 */
	subquerystate->ss.ps.scanopsset = true;
	subquerystate->ss.ps.scanops = ExecGetResultSlotOps(subquerystate->subplan,
														&subquerystate->ss.ps.scanopsfixed);
	subquerystate->ss.ps.resultopsset = true;
	subquerystate->ss.ps.resultops = subquerystate->ss.ps.scanops;
	subquerystate->ss.ps.resultopsfixed = subquerystate->ss.ps.scanopsfixed;

	/*
	 * Initialize result type and projection.
	 */
	ExecInitResultTypeTL(&subquerystate->ss.ps);
	ExecAssignScanProjectionInfo(&subquerystate->ss);

	/*
	 * initialize child expressions
	 */
	subquerystate->ss.ps.qual =
		ExecInitQual(node->scan.plan.qual, (PlanState *) subquerystate);

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
	if (node->ss.ps.ps_ResultTupleSlot)
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
