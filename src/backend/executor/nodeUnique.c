/*-------------------------------------------------------------------------
 *
 * nodeUnique.c
 *	  Routines to handle unique'ing of queries where appropriate
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeUnique.c,v 1.28 2000/04/12 17:15:10 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecUnique		- generate a unique'd temporary relation
 *		ExecInitUnique	- initialize node and subnodes..
 *		ExecEndUnique	- shutdown node and subnodes
 *
 * NOTES
 *		Assumes tuples returned from subplan arrive in
 *		sorted order.
 *
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/printtup.h"
#include "executor/executor.h"
#include "executor/nodeGroup.h"
#include "executor/nodeUnique.h"

/* ----------------------------------------------------------------
 *		ExecUnique
 *
 *		This is a very simple node which filters out duplicate
 *		tuples from a stream of sorted tuples from a subplan.
 * ----------------------------------------------------------------
 */
TupleTableSlot *				/* return: a tuple or NULL */
ExecUnique(Unique *node)
{
	UniqueState *uniquestate;
	TupleTableSlot *resultTupleSlot;
	TupleTableSlot *slot;
	Plan	   *outerPlan;
	TupleDesc	tupDesc;

	/* ----------------
	 *	get information from the node
	 * ----------------
	 */
	uniquestate = node->uniquestate;
	outerPlan = outerPlan((Plan *) node);
	resultTupleSlot = uniquestate->cstate.cs_ResultTupleSlot;
	tupDesc = ExecGetResultType(&uniquestate->cstate);

	/* ----------------
	 *	now loop, returning only non-duplicate tuples.
	 *	We assume that the tuples arrive in sorted order
	 *	so we can detect duplicates easily.
	 * ----------------
	 */
	for (;;)
	{
		/* ----------------
		 *	 fetch a tuple from the outer subplan
		 * ----------------
		 */
		slot = ExecProcNode(outerPlan, (Plan *) node);
		if (TupIsNull(slot))
			return NULL;

		/* ----------------
		 *	 Always return the first tuple from the subplan.
		 * ----------------
		 */
		if (uniquestate->priorTuple == NULL)
			break;

		/* ----------------
		 *	 Else test if the new tuple and the previously returned
		 *	 tuple match.  If so then we loop back and fetch
		 *	 another new tuple from the subplan.
		 * ----------------
		 */
		if (!execTuplesMatch(slot->val, uniquestate->priorTuple,
							 tupDesc,
							 node->numCols, node->uniqColIdx,
							 uniquestate->eqfunctions))
			break;
	}

	/* ----------------
	 *	We have a new tuple different from the previous saved tuple (if any).
	 *	Save it and return it.	Note that we make two copies of the tuple:
	 *	one to keep for our own future comparisons, and one to return to the
	 *	caller.  We need to copy the tuple returned by the subplan to avoid
	 *	holding buffer refcounts, and we need our own copy because the caller
	 *	may alter the resultTupleSlot (eg via ExecRemoveJunk).
	 * ----------------
	 */
	if (uniquestate->priorTuple != NULL)
		heap_freetuple(uniquestate->priorTuple);
	uniquestate->priorTuple = heap_copytuple(slot->val);

	ExecStoreTuple(heap_copytuple(slot->val),
				   resultTupleSlot,
				   InvalidBuffer,
				   true);

	return resultTupleSlot;
}

/* ----------------------------------------------------------------
 *		ExecInitUnique
 *
 *		This initializes the unique node state structures and
 *		the node's subplan.
 * ----------------------------------------------------------------
 */
bool							/* return: initialization status */
ExecInitUnique(Unique *node, EState *estate, Plan *parent)
{
	UniqueState *uniquestate;
	Plan	   *outerPlan;

	/* ----------------
	 *	assign execution state to node
	 * ----------------
	 */
	node->plan.state = estate;

	/* ----------------
	 *	create new UniqueState for node
	 * ----------------
	 */
	uniquestate = makeNode(UniqueState);
	node->uniquestate = uniquestate;
	uniquestate->priorTuple = NULL;

	/* ----------------
	 *	Miscellaneous initialization
	 *
	 *		 +	assign node's base_id
	 *		 +	assign debugging hooks and
	 *
	 *	Unique nodes have no ExprContext initialization because
	 *	they never call ExecQual or ExecTargetList.
	 * ----------------
	 */
	ExecAssignNodeBaseInfo(estate, &uniquestate->cstate, parent);

#define UNIQUE_NSLOTS 1
	/* ------------
	 * Tuple table initialization
	 * ------------
	 */
	ExecInitResultTupleSlot(estate, &uniquestate->cstate);

	/* ----------------
	 *	then initialize outer plan
	 * ----------------
	 */
	outerPlan = outerPlan((Plan *) node);
	ExecInitNode(outerPlan, estate, (Plan *) node);

	/* ----------------
	 *	unique nodes do no projections, so initialize
	 *	projection info for this node appropriately
	 * ----------------
	 */
	ExecAssignResultTypeFromOuterPlan((Plan *) node, &uniquestate->cstate);
	uniquestate->cstate.cs_ProjInfo = NULL;

	/*
	 * Precompute fmgr lookup data for inner loop
	 */
	uniquestate->eqfunctions =
		execTuplesMatchPrepare(ExecGetResultType(&uniquestate->cstate),
							   node->numCols,
							   node->uniqColIdx);

	return TRUE;
}

int
ExecCountSlotsUnique(Unique *node)
{
	return ExecCountSlotsNode(outerPlan(node)) +
	ExecCountSlotsNode(innerPlan(node)) +
	UNIQUE_NSLOTS;
}

/* ----------------------------------------------------------------
 *		ExecEndUnique
 *
 *		This shuts down the subplan and frees resources allocated
 *		to this node.
 * ----------------------------------------------------------------
 */
void
ExecEndUnique(Unique *node)
{
	UniqueState *uniquestate = node->uniquestate;

	ExecEndNode(outerPlan((Plan *) node), (Plan *) node);

	/* clean up tuple table */
	ExecClearTuple(uniquestate->cstate.cs_ResultTupleSlot);
	if (uniquestate->priorTuple != NULL)
	{
		heap_freetuple(uniquestate->priorTuple);
		uniquestate->priorTuple = NULL;
	}
}


void
ExecReScanUnique(Unique *node, ExprContext *exprCtxt, Plan *parent)
{
	UniqueState *uniquestate = node->uniquestate;

	ExecClearTuple(uniquestate->cstate.cs_ResultTupleSlot);
	if (uniquestate->priorTuple != NULL)
	{
		heap_freetuple(uniquestate->priorTuple);
		uniquestate->priorTuple = NULL;
	}

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (((Plan *) node)->lefttree->chgParam == NULL)
		ExecReScan(((Plan *) node)->lefttree, exprCtxt, (Plan *) node);

}
