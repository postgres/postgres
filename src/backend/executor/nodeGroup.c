/*-------------------------------------------------------------------------
 *
 * nodeGroup.c
 *	  Routines to handle group nodes (used for queries with GROUP BY clause).
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * DESCRIPTION
 *	  The Group node is designed for handling queries with a GROUP BY clause.
 *	  Its outer plan must deliver tuples that are sorted in the order
 *	  specified by the grouping columns (ie. tuples from the same group are
 *	  consecutive).  That way, we just have to compare adjacent tuples to
 *	  locate group boundaries.
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeGroup.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeGroup.h"


/*
 *	 ExecGroup -
 *
 *		Return one tuple for each group of matching input tuples.
 */
TupleTableSlot *
ExecGroup(GroupState *node)
{
	ExprContext *econtext;
	int			numCols;
	AttrNumber *grpColIdx;
	TupleTableSlot *firsttupleslot;
	TupleTableSlot *outerslot;

	/*
	 * get state info from node
	 */
	if (node->grp_done)
		return NULL;
	econtext = node->ss.ps.ps_ExprContext;
	numCols = ((Group *) node->ss.ps.plan)->numCols;
	grpColIdx = ((Group *) node->ss.ps.plan)->grpColIdx;

	/*
	 * Check to see if we're still projecting out tuples from a previous group
	 * tuple (because there is a function-returning-set in the projection
	 * expressions).  If so, try to project another one.
	 */
	if (node->ss.ps.ps_TupFromTlist)
	{
		TupleTableSlot *result;
		ExprDoneCond isDone;

		result = ExecProject(node->ss.ps.ps_ProjInfo, &isDone);
		if (isDone == ExprMultipleResult)
			return result;
		/* Done with that source tuple... */
		node->ss.ps.ps_TupFromTlist = false;
	}

	/*
	 * The ScanTupleSlot holds the (copied) first tuple of each group.
	 */
	firsttupleslot = node->ss.ss_ScanTupleSlot;

	/*
	 * We need not call ResetExprContext here because execTuplesMatch will
	 * reset the per-tuple memory context once per input tuple.
	 */

	/*
	 * If first time through, acquire first input tuple and determine whether
	 * to return it or not.
	 */
	if (TupIsNull(firsttupleslot))
	{
		outerslot = ExecProcNode(outerPlanState(node));
		if (TupIsNull(outerslot))
		{
			/* empty input, so return nothing */
			node->grp_done = TRUE;
			return NULL;
		}
		/* Copy tuple into firsttupleslot */
		ExecCopySlot(firsttupleslot, outerslot);

		/*
		 * Set it up as input for qual test and projection.  The expressions
		 * will access the input tuple as varno OUTER.
		 */
		econtext->ecxt_outertuple = firsttupleslot;

		/*
		 * Check the qual (HAVING clause); if the group does not match, ignore
		 * it and fall into scan loop.
		 */
		if (ExecQual(node->ss.ps.qual, econtext, false))
		{
			/*
			 * Form and return a projection tuple using the first input tuple.
			 */
			TupleTableSlot *result;
			ExprDoneCond isDone;

			result = ExecProject(node->ss.ps.ps_ProjInfo, &isDone);

			if (isDone != ExprEndResult)
			{
				node->ss.ps.ps_TupFromTlist = (isDone == ExprMultipleResult);
				return result;
			}
		}
		else
			InstrCountFiltered1(node, 1);
	}

	/*
	 * This loop iterates once per input tuple group.  At the head of the
	 * loop, we have finished processing the first tuple of the group and now
	 * need to scan over all the other group members.
	 */
	for (;;)
	{
		/*
		 * Scan over all remaining tuples that belong to this group
		 */
		for (;;)
		{
			outerslot = ExecProcNode(outerPlanState(node));
			if (TupIsNull(outerslot))
			{
				/* no more groups, so we're done */
				node->grp_done = TRUE;
				return NULL;
			}

			/*
			 * Compare with first tuple and see if this tuple is of the same
			 * group.  If so, ignore it and keep scanning.
			 */
			if (!execTuplesMatch(firsttupleslot, outerslot,
								 numCols, grpColIdx,
								 node->eqfunctions,
								 econtext->ecxt_per_tuple_memory))
				break;
		}

		/*
		 * We have the first tuple of the next input group.  See if we want to
		 * return it.
		 */
		/* Copy tuple, set up as input for qual test and projection */
		ExecCopySlot(firsttupleslot, outerslot);
		econtext->ecxt_outertuple = firsttupleslot;

		/*
		 * Check the qual (HAVING clause); if the group does not match, ignore
		 * it and loop back to scan the rest of the group.
		 */
		if (ExecQual(node->ss.ps.qual, econtext, false))
		{
			/*
			 * Form and return a projection tuple using the first input tuple.
			 */
			TupleTableSlot *result;
			ExprDoneCond isDone;

			result = ExecProject(node->ss.ps.ps_ProjInfo, &isDone);

			if (isDone != ExprEndResult)
			{
				node->ss.ps.ps_TupFromTlist = (isDone == ExprMultipleResult);
				return result;
			}
		}
		else
			InstrCountFiltered1(node, 1);
	}
}

/* -----------------
 * ExecInitGroup
 *
 *	Creates the run-time information for the group node produced by the
 *	planner and initializes its outer subtree
 * -----------------
 */
GroupState *
ExecInitGroup(Group *node, EState *estate, int eflags)
{
	GroupState *grpstate;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * create state structure
	 */
	grpstate = makeNode(GroupState);
	grpstate->ss.ps.plan = (Plan *) node;
	grpstate->ss.ps.state = estate;
	grpstate->grp_done = FALSE;

	/*
	 * create expression context
	 */
	ExecAssignExprContext(estate, &grpstate->ss.ps);

	/*
	 * tuple table initialization
	 */
	ExecInitScanTupleSlot(estate, &grpstate->ss);
	ExecInitResultTupleSlot(estate, &grpstate->ss.ps);

	/*
	 * initialize child expressions
	 */
	grpstate->ss.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->plan.targetlist,
					 (PlanState *) grpstate);
	grpstate->ss.ps.qual = (List *)
		ExecInitExpr((Expr *) node->plan.qual,
					 (PlanState *) grpstate);

	/*
	 * initialize child nodes
	 */
	outerPlanState(grpstate) = ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * initialize tuple type.
	 */
	ExecAssignScanTypeFromOuterPlan(&grpstate->ss);

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&grpstate->ss.ps);
	ExecAssignProjectionInfo(&grpstate->ss.ps, NULL);

	grpstate->ss.ps.ps_TupFromTlist = false;

	/*
	 * Precompute fmgr lookup data for inner loop
	 */
	grpstate->eqfunctions =
		execTuplesMatchPrepare(node->numCols,
							   node->grpOperators);

	return grpstate;
}

/* ------------------------
 *		ExecEndGroup(node)
 *
 * -----------------------
 */
void
ExecEndGroup(GroupState *node)
{
	PlanState  *outerPlan;

	ExecFreeExprContext(&node->ss.ps);

	/* clean up tuple table */
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	outerPlan = outerPlanState(node);
	ExecEndNode(outerPlan);
}

void
ExecReScanGroup(GroupState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	node->grp_done = FALSE;
	node->ss.ps.ps_TupFromTlist = false;
	/* must clear first tuple */
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
}
