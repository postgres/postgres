/*-------------------------------------------------------------------------
 *
 * nodeUnique.c
 *	  Routines to handle unique'ing of queries where appropriate
 *
 * Unique is a very simple node type that just filters out duplicate
 * tuples from a stream of sorted tuples from its subplan.  It's essentially
 * a dumbed-down form of Group: the duplicate-removal functionality is
 * identical.  However, Unique doesn't do projection nor qual checking,
 * so it's marginally more efficient for cases where neither is needed.
 * (It's debatable whether the savings justifies carrying two plan node
 * types, though.)
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeUnique.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecUnique		- generate a unique'd temporary relation
 *		ExecInitUnique	- initialize node and subnodes
 *		ExecEndUnique	- shutdown node and subnodes
 *
 * NOTES
 *		Assumes tuples returned from subplan arrive in
 *		sorted order.
 */

#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeUnique.h"
#include "miscadmin.h"


/* ----------------------------------------------------------------
 *		ExecUnique
 * ----------------------------------------------------------------
 */
static TupleTableSlot *			/* return: a tuple or NULL */
ExecUnique(PlanState *pstate)
{
	UniqueState *node = castNode(UniqueState, pstate);
	ExprContext *econtext = node->ps.ps_ExprContext;
	TupleTableSlot *resultTupleSlot;
	TupleTableSlot *slot;
	PlanState  *outerPlan;

	CHECK_FOR_INTERRUPTS();

	/*
	 * get information from the node
	 */
	outerPlan = outerPlanState(node);
	resultTupleSlot = node->ps.ps_ResultTupleSlot;

	/*
	 * now loop, returning only non-duplicate tuples. We assume that the
	 * tuples arrive in sorted order so we can detect duplicates easily. The
	 * first tuple of each group is returned.
	 */
	for (;;)
	{
		/*
		 * fetch a tuple from the outer subplan
		 */
		slot = ExecProcNode(outerPlan);
		if (TupIsNull(slot))
		{
			/* end of subplan, so we're done */
			ExecClearTuple(resultTupleSlot);
			return NULL;
		}

		/*
		 * Always return the first tuple from the subplan.
		 */
		if (TupIsNull(resultTupleSlot))
			break;

		/*
		 * Else test if the new tuple and the previously returned tuple match.
		 * If so then we loop back and fetch another new tuple from the
		 * subplan.
		 */
		econtext->ecxt_innertuple = slot;
		econtext->ecxt_outertuple = resultTupleSlot;
		if (!ExecQualAndReset(node->eqfunction, econtext))
			break;
	}

	/*
	 * We have a new tuple different from the previous saved tuple (if any).
	 * Save it and return it.  We must copy it because the source subplan
	 * won't guarantee that this source tuple is still accessible after
	 * fetching the next source tuple.
	 */
	return ExecCopySlot(resultTupleSlot, slot);
}

/* ----------------------------------------------------------------
 *		ExecInitUnique
 *
 *		This initializes the unique node state structures and
 *		the node's subplan.
 * ----------------------------------------------------------------
 */
UniqueState *
ExecInitUnique(Unique *node, EState *estate, int eflags)
{
	UniqueState *uniquestate;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * create state structure
	 */
	uniquestate = makeNode(UniqueState);
	uniquestate->ps.plan = (Plan *) node;
	uniquestate->ps.state = estate;
	uniquestate->ps.ExecProcNode = ExecUnique;

	/*
	 * create expression context
	 */
	ExecAssignExprContext(estate, &uniquestate->ps);

	/*
	 * then initialize outer plan
	 */
	outerPlanState(uniquestate) = ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * Initialize result slot and type. Unique nodes do no projections, so
	 * initialize projection info for this node appropriately.
	 */
	ExecInitResultTupleSlotTL(&uniquestate->ps, &TTSOpsMinimalTuple);
	uniquestate->ps.ps_ProjInfo = NULL;

	/*
	 * Precompute fmgr lookup data for inner loop
	 */
	uniquestate->eqfunction =
		execTuplesMatchPrepare(ExecGetResultType(outerPlanState(uniquestate)),
							   node->numCols,
							   node->uniqColIdx,
							   node->uniqOperators,
							   node->uniqCollations,
							   &uniquestate->ps);

	return uniquestate;
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
	ExecEndNode(outerPlanState(node));
}


void
ExecReScanUnique(UniqueState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	/* must clear result tuple so first input tuple is returned */
	ExecClearTuple(node->ps.ps_ResultTupleSlot);

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
}
