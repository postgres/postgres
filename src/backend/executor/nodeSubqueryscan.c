/*-------------------------------------------------------------------------
 *
 * nodeSubqueryscan.c
 *	  Support routines for scanning subqueries (subselects in rangetable).
 *
 * This is just enough different from sublinks (nodeSubplan.c) to mean that
 * we need two sets of code.  Ought to look at trying to unify the cases.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeSubqueryscan.c,v 1.14 2002/12/05 15:50:33 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecSubqueryScan			scans a subquery.
 *		ExecSubqueryNext			retrieve next tuple in sequential order.
 *		ExecInitSubqueryScan		creates and initializes a subqueryscan node.
 *		ExecEndSubqueryScan			releases any storage allocated.
 *		ExecSubqueryReScan			rescans the relation
 *
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "executor/execdebug.h"
#include "executor/execdefs.h"
#include "executor/execdesc.h"
#include "executor/nodeSubqueryscan.h"
#include "parser/parsetree.h"
#include "tcop/pquery.h"

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
	EState	   *estate;
	ScanDirection direction;
	TupleTableSlot *slot;

	/*
	 * get information from the estate and scan state
	 */
	estate = node->ss.ps.state;
	direction = estate->es_direction;

	/*
	 * We need not support EvalPlanQual here, since we are not scanning a
	 * real relation.
	 */

	/*
	 * get the next tuple from the sub-query
	 */
	node->sss_SubEState->es_direction = direction;

	slot = ExecProcNode(node->subplan);

	node->ss.ss_ScanTupleSlot = slot;

	return slot;
}

/* ----------------------------------------------------------------
 *		ExecSubqueryScan(node)
 *
 *		Scans the subquery sequentially and returns the next qualifying
 *		tuple.
 *		It calls the ExecScan() routine and passes it the access method
 *		which retrieve tuples sequentially.
 *
 */

TupleTableSlot *
ExecSubqueryScan(SubqueryScanState *node)
{
	/*
	 * use SubqueryNext as access method
	 */
	return ExecScan(&node->ss, (ExecScanAccessMtd) SubqueryNext);
}

/* ----------------------------------------------------------------
 *		ExecInitSubqueryScan
 * ----------------------------------------------------------------
 */
SubqueryScanState *
ExecInitSubqueryScan(SubqueryScan *node, EState *estate)
{
	SubqueryScanState *subquerystate;
	RangeTblEntry *rte;
	EState	   *sp_estate;

	/*
	 * SubqueryScan should not have any "normal" children.
	 */
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
		ExecInitExpr((Node *) node->scan.plan.targetlist,
					 (PlanState *) subquerystate);
	subquerystate->ss.ps.qual = (List *)
		ExecInitExpr((Node *) node->scan.plan.qual,
					 (PlanState *) subquerystate);

#define SUBQUERYSCAN_NSLOTS 1

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &subquerystate->ss.ps);

	/*
	 * initialize subquery
	 *
	 * This should agree with ExecInitSubPlan
	 */
	rte = rt_fetch(node->scan.scanrelid, estate->es_range_table);
	Assert(rte->rtekind == RTE_SUBQUERY);

	sp_estate = CreateExecutorState();
	subquerystate->sss_SubEState = sp_estate;

	sp_estate->es_range_table = rte->subquery->rtable;
	sp_estate->es_param_list_info = estate->es_param_list_info;
	sp_estate->es_param_exec_vals = estate->es_param_exec_vals;
	sp_estate->es_tupleTable =
		ExecCreateTupleTable(ExecCountSlotsNode(node->subplan) + 10);
	sp_estate->es_snapshot = estate->es_snapshot;
	sp_estate->es_instrument = estate->es_instrument;

	subquerystate->subplan = ExecInitNode(node->subplan, sp_estate);

	subquerystate->ss.ss_ScanTupleSlot = NULL;
	subquerystate->ss.ps.ps_TupFromTlist = false;

	/*
	 * initialize tuple type
	 */
	ExecAssignResultTypeFromTL(&subquerystate->ss.ps);
	ExecAssignProjectionInfo(&subquerystate->ss.ps);

	return subquerystate;
}

int
ExecCountSlotsSubqueryScan(SubqueryScan *node)
{
	/*
	 * The subplan has its own tuple table and must not be counted here!
	 */
	return ExecCountSlotsNode(outerPlan(node)) +
		ExecCountSlotsNode(innerPlan(node)) +
		SUBQUERYSCAN_NSLOTS;
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
	 * Free the projection info and the scan attribute info
	 */
	ExecFreeProjectionInfo(&node->ss.ps);
	ExecFreeExprContext(&node->ss.ps);

	/*
	 * clean out the upper tuple table
	 */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	/*
	 * close down subquery
	 */
	ExecEndNode(node->subplan);

	/*
	 * clean up subquery's tuple table
	 */
	node->ss.ss_ScanTupleSlot = NULL;
	ExecDropTupleTable(node->sss_SubEState->es_tupleTable, true);

	/* XXX we seem to be leaking the sub-EState... */
}

/* ----------------------------------------------------------------
 *		ExecSubqueryReScan
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecSubqueryReScan(SubqueryScanState *node, ExprContext *exprCtxt)
{
	EState	   *estate;

	estate = node->ss.ps.state;

	/*
	 * ExecReScan doesn't know about my subplan, so I have to do
	 * changed-parameter signaling myself.
	 */
	if (node->ss.ps.chgParam != NULL)
		SetChangedParamList(node->subplan, node->ss.ps.chgParam);

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (node->subplan->chgParam == NULL)
		ExecReScan(node->subplan, NULL);

	node->ss.ss_ScanTupleSlot = NULL;
}
