/*-------------------------------------------------------------------------
 *
 * nodeUnique.c
 *	  Routines to handle unique'ing of queries where appropriate
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeUnique.c,v 1.40 2003/08/04 02:39:59 momjian Exp $
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
 */

#include "postgres.h"

#include "access/heapam.h"
#include "executor/executor.h"
#include "executor/nodeUnique.h"


/* ----------------------------------------------------------------
 *		ExecUnique
 *
 *		This is a very simple node which filters out duplicate
 *		tuples from a stream of sorted tuples from a subplan.
 * ----------------------------------------------------------------
 */
TupleTableSlot *				/* return: a tuple or NULL */
ExecUnique(UniqueState *node)
{
	Unique	   *plannode = (Unique *) node->ps.plan;
	TupleTableSlot *resultTupleSlot;
	TupleTableSlot *slot;
	PlanState  *outerPlan;
	TupleDesc	tupDesc;

	/*
	 * get information from the node
	 */
	outerPlan = outerPlanState(node);
	resultTupleSlot = node->ps.ps_ResultTupleSlot;
	tupDesc = ExecGetResultType(&node->ps);

	/*
	 * now loop, returning only non-duplicate tuples. We assume that the
	 * tuples arrive in sorted order so we can detect duplicates easily.
	 *
	 * We return the first tuple from each group of duplicates (or the last
	 * tuple of each group, when moving backwards).  At either end of the
	 * subplan, clear priorTuple so that we correctly return the
	 * first/last tuple when reversing direction.
	 */
	for (;;)
	{
		/*
		 * fetch a tuple from the outer subplan
		 */
		slot = ExecProcNode(outerPlan);
		if (TupIsNull(slot))
		{
			/* end of subplan; reset in case we change direction */
			if (node->priorTuple != NULL)
				heap_freetuple(node->priorTuple);
			node->priorTuple = NULL;
			return NULL;
		}

		/*
		 * Always return the first/last tuple from the subplan.
		 */
		if (node->priorTuple == NULL)
			break;

		/*
		 * Else test if the new tuple and the previously returned tuple
		 * match.  If so then we loop back and fetch another new tuple
		 * from the subplan.
		 */
		if (!execTuplesMatch(slot->val, node->priorTuple,
							 tupDesc,
							 plannode->numCols, plannode->uniqColIdx,
							 node->eqfunctions,
							 node->tempContext))
			break;
	}

	/*
	 * We have a new tuple different from the previous saved tuple (if
	 * any). Save it and return it.  We must copy it because the source
	 * subplan won't guarantee that this source tuple is still accessible
	 * after fetching the next source tuple.
	 *
	 * Note that we manage the copy ourselves.	We can't rely on the result
	 * tuple slot to maintain the tuple reference because our caller may
	 * replace the slot contents with a different tuple (see junk filter
	 * handling in execMain.c).  We assume that the caller will no longer
	 * be interested in the current tuple after he next calls us.
	 */
	if (node->priorTuple != NULL)
		heap_freetuple(node->priorTuple);
	node->priorTuple = heap_copytuple(slot->val);

	ExecStoreTuple(node->priorTuple,
				   resultTupleSlot,
				   InvalidBuffer,
				   false);		/* tuple does not belong to slot */

	return resultTupleSlot;
}

/* ----------------------------------------------------------------
 *		ExecInitUnique
 *
 *		This initializes the unique node state structures and
 *		the node's subplan.
 * ----------------------------------------------------------------
 */
UniqueState *
ExecInitUnique(Unique *node, EState *estate)
{
	UniqueState *uniquestate;

	/*
	 * create state structure
	 */
	uniquestate = makeNode(UniqueState);
	uniquestate->ps.plan = (Plan *) node;
	uniquestate->ps.state = estate;

	uniquestate->priorTuple = NULL;

	/*
	 * Miscellaneous initialization
	 *
	 * Unique nodes have no ExprContext initialization because they never
	 * call ExecQual or ExecProject.  But they do need a per-tuple memory
	 * context anyway for calling execTuplesMatch.
	 */
	uniquestate->tempContext =
		AllocSetContextCreate(CurrentMemoryContext,
							  "Unique",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);

#define UNIQUE_NSLOTS 1

	/*
	 * Tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &uniquestate->ps);

	/*
	 * then initialize outer plan
	 */
	outerPlanState(uniquestate) = ExecInitNode(outerPlan(node), estate);

	/*
	 * unique nodes do no projections, so initialize projection info for
	 * this node appropriately
	 */
	ExecAssignResultTypeFromOuterPlan(&uniquestate->ps);
	uniquestate->ps.ps_ProjInfo = NULL;

	/*
	 * Precompute fmgr lookup data for inner loop
	 */
	uniquestate->eqfunctions =
		execTuplesMatchPrepare(ExecGetResultType(&uniquestate->ps),
							   node->numCols,
							   node->uniqColIdx);

	return uniquestate;
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
ExecEndUnique(UniqueState *node)
{
	/* clean up tuple table */
	ExecClearTuple(node->ps.ps_ResultTupleSlot);

	MemoryContextDelete(node->tempContext);

	ExecEndNode(outerPlanState(node));
}


void
ExecReScanUnique(UniqueState *node, ExprContext *exprCtxt)
{
	ExecClearTuple(node->ps.ps_ResultTupleSlot);
	if (node->priorTuple != NULL)
	{
		heap_freetuple(node->priorTuple);
		node->priorTuple = NULL;
	}

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (((PlanState *) node)->lefttree->chgParam == NULL)
		ExecReScan(((PlanState *) node)->lefttree, exprCtxt);
}
