/*-------------------------------------------------------------------------
 *
 * nodeUnique.c
 *	  Routines to handle unique'ing of queries where appropriate
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeUnique.c,v 1.34.2.1 2003/02/02 19:09:08 tgl Exp $
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

	/*
	 * get information from the node
	 */
	uniquestate = node->uniquestate;
	outerPlan = outerPlan((Plan *) node);
	resultTupleSlot = uniquestate->cstate.cs_ResultTupleSlot;
	tupDesc = ExecGetResultType(&uniquestate->cstate);

	/*
	 * now loop, returning only non-duplicate tuples. We assume that the
	 * tuples arrive in sorted order so we can detect duplicates easily.
	 *
	 * We return the first tuple from each group of duplicates (or the
	 * last tuple of each group, when moving backwards).  At either end
	 * of the subplan, clear priorTuple so that we correctly return the
	 * first/last tuple when reversing direction.
	 */
	for (;;)
	{
		/*
		 * fetch a tuple from the outer subplan
		 */
		slot = ExecProcNode(outerPlan, (Plan *) node);
		if (TupIsNull(slot))
		{
			/* end of subplan; reset in case we change direction */
			if (uniquestate->priorTuple != NULL)
				heap_freetuple(uniquestate->priorTuple);
			uniquestate->priorTuple = NULL;
			return NULL;
		}

		/*
		 * Always return the first/last tuple from the subplan.
		 */
		if (uniquestate->priorTuple == NULL)
			break;

		/*
		 * Else test if the new tuple and the previously returned tuple
		 * match.  If so then we loop back and fetch another new tuple
		 * from the subplan.
		 */
		if (!execTuplesMatch(slot->val, uniquestate->priorTuple,
							 tupDesc,
							 node->numCols, node->uniqColIdx,
							 uniquestate->eqfunctions,
							 uniquestate->tempContext))
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
	if (uniquestate->priorTuple != NULL)
		heap_freetuple(uniquestate->priorTuple);
	uniquestate->priorTuple = heap_copytuple(slot->val);

	ExecStoreTuple(uniquestate->priorTuple,
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
bool							/* return: initialization status */
ExecInitUnique(Unique *node, EState *estate, Plan *parent)
{
	UniqueState *uniquestate;
	Plan	   *outerPlan;

	/*
	 * assign execution state to node
	 */
	node->plan.state = estate;

	/*
	 * create new UniqueState for node
	 */
	uniquestate = makeNode(UniqueState);
	node->uniquestate = uniquestate;
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
	ExecInitResultTupleSlot(estate, &uniquestate->cstate);

	/*
	 * then initialize outer plan
	 */
	outerPlan = outerPlan((Plan *) node);
	ExecInitNode(outerPlan, estate, (Plan *) node);

	/*
	 * unique nodes do no projections, so initialize projection info for
	 * this node appropriately
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

	MemoryContextDelete(uniquestate->tempContext);

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
