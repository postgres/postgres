/*-------------------------------------------------------------------------
 *
 * nodeSubqueryscan.c
 *	  Support routines for scanning subqueries (subselects in rangetable).
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeSubqueryscan.c,v 1.1 2000/09/29 18:21:29 tgl Exp $
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

static TupleTableSlot *SubqueryNext(SubqueryScan *node);

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
SubqueryNext(SubqueryScan *node)
{
	SubqueryScanState *subquerystate;
	EState	   *estate;
	ScanDirection direction;
	int			execdir;
	TupleTableSlot *slot;
	Const		countOne;

	/* ----------------
	 *	get information from the estate and scan state
	 * ----------------
	 */
	estate = node->scan.plan.state;
	subquerystate = (SubqueryScanState *) node->scan.scanstate;
	direction = estate->es_direction;
	execdir = ScanDirectionIsBackward(direction) ? EXEC_BACK : EXEC_FOR;
	slot = subquerystate->csstate.css_ScanTupleSlot;

	/*
	 * Check if we are evaluating PlanQual for tuple of this relation.
	 * Additional checking is not good, but no other way for now. We could
	 * introduce new nodes for this case and handle SubqueryScan --> NewNode
	 * switching in Init/ReScan plan...
	 */
	if (estate->es_evTuple != NULL &&
		estate->es_evTuple[node->scan.scanrelid - 1] != NULL)
	{
		ExecClearTuple(slot);
		if (estate->es_evTupleNull[node->scan.scanrelid - 1])
			return slot;		/* return empty slot */

		/* probably ought to use ExecStoreTuple here... */
		slot->val = estate->es_evTuple[node->scan.scanrelid - 1];
		slot->ttc_shouldFree = false;

		/* Flag for the next call that no more tuples */
		estate->es_evTupleNull[node->scan.scanrelid - 1] = true;
		return (slot);
	}

	memset(&countOne, 0, sizeof(countOne));
	countOne.type = T_Const;
	countOne.consttype = INT4OID;
	countOne.constlen = sizeof(int4);
	countOne.constvalue = Int32GetDatum(1);
	countOne.constisnull = false;
	countOne.constbyval = true;
	countOne.constisset = false;
	countOne.constiscast = false;

	/* ----------------
	 *	get the next tuple from the sub-query
	 * ----------------
	 */
	slot = ExecutorRun(subquerystate->sss_SubQueryDesc,
					   subquerystate->sss_SubEState,
					   execdir,
					   NULL,	/* offset */
					   (Node *) &countOne);

	subquerystate->csstate.css_ScanTupleSlot = slot;

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
ExecSubqueryScan(SubqueryScan *node)
{
	/* ----------------
	 *	use SubqueryNext as access method
	 * ----------------
	 */
	return ExecScan(&node->scan, (ExecScanAccessMtd) SubqueryNext);
}

/* ----------------------------------------------------------------
 *		ExecInitSubqueryScan
 * ----------------------------------------------------------------
 */
bool
ExecInitSubqueryScan(SubqueryScan *node, EState *estate, Plan *parent)
{
	SubqueryScanState *subquerystate;
	RangeTblEntry *rte;

	/* ----------------
	 *	SubqueryScan should not have any "normal" children.
	 * ----------------
	 */
	Assert(outerPlan((Plan *) node) == NULL);
	Assert(innerPlan((Plan *) node) == NULL);

	/* ----------------
	 *	assign the node's execution state
	 * ----------------
	 */
	node->scan.plan.state = estate;

	/* ----------------
	 *	 create new SubqueryScanState for node
	 * ----------------
	 */
	subquerystate = makeNode(SubqueryScanState);
	node->scan.scanstate = (CommonScanState *) subquerystate;

	/* ----------------
	 *	Miscellaneous initialization
	 *
	 *		 +	create expression context for node
	 * ----------------
	 */
	ExecAssignExprContext(estate, &subquerystate->csstate.cstate);

#define SUBQUERYSCAN_NSLOTS 2
	/* ----------------
	 *	tuple table initialization
	 * ----------------
	 */
	ExecInitResultTupleSlot(estate, &subquerystate->csstate.cstate);

	/* ----------------
	 *	initialize subquery
	 * ----------------
	 */
	rte = rt_fetch(node->scan.scanrelid, estate->es_range_table);
	Assert(rte->subquery != NULL);

	subquerystate->sss_SubQueryDesc = CreateQueryDesc(rte->subquery,
													  node->subplan,
													  None);
	subquerystate->sss_SubEState = CreateExecutorState();

	ExecutorStart(subquerystate->sss_SubQueryDesc,
				  subquerystate->sss_SubEState);

	subquerystate->csstate.css_ScanTupleSlot = NULL;
	subquerystate->csstate.cstate.cs_TupFromTlist = false;

	/* ----------------
	 *	initialize tuple type
	 * ----------------
	 */
	ExecAssignResultTypeFromTL((Plan *) node, &subquerystate->csstate.cstate);
	ExecAssignProjectionInfo((Plan *) node, &subquerystate->csstate.cstate);

	return TRUE;
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
ExecEndSubqueryScan(SubqueryScan *node)
{
	SubqueryScanState *subquerystate;

	/* ----------------
	 *	get information from node
	 * ----------------
	 */
	subquerystate = (SubqueryScanState *) node->scan.scanstate;

	/* ----------------
	 *	Free the projection info and the scan attribute info
	 *
	 *	Note: we don't ExecFreeResultType(subquerystate)
	 *		  because the rule manager depends on the tupType
	 *		  returned by ExecMain().  So for now, this
	 *		  is freed at end-transaction time.  -cim 6/2/91
	 * ----------------
	 */
	ExecFreeProjectionInfo(&subquerystate->csstate.cstate);
	ExecFreeExprContext(&subquerystate->csstate.cstate);

	/* ----------------
	 * close down subquery
	 * ----------------
	 */
	ExecutorEnd(subquerystate->sss_SubQueryDesc,
				subquerystate->sss_SubEState);

	/* XXX we seem to be leaking the querydesc and sub-EState... */

	subquerystate->csstate.css_ScanTupleSlot = NULL;

	/* ----------------
	 *	clean out the tuple table
	 * ----------------
	 */
	ExecClearTuple(subquerystate->csstate.cstate.cs_ResultTupleSlot);
}

/* ----------------------------------------------------------------
 *		ExecSubqueryReScan
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecSubqueryReScan(SubqueryScan *node, ExprContext *exprCtxt, Plan *parent)
{
	SubqueryScanState *subquerystate;
	EState	   *estate;

	subquerystate = (SubqueryScanState *) node->scan.scanstate;
	estate = node->scan.plan.state;

	/* If this is re-scanning of PlanQual ... */
	if (estate->es_evTuple != NULL &&
		estate->es_evTuple[node->scan.scanrelid - 1] != NULL)
	{
		estate->es_evTupleNull[node->scan.scanrelid - 1] = false;
		return;
	}

	ExecReScan(node->subplan, NULL, NULL);
	subquerystate->csstate.css_ScanTupleSlot = NULL;
}
