/*-------------------------------------------------------------------------
 *
 * nodeSetOp.c
 *	  Routines to handle INTERSECT and EXCEPT selection
 *
 * The input of a SetOp node consists of tuples from two relations,
 * which have been combined into one dataset and sorted on all the nonjunk
 * attributes.	In addition there is a junk attribute that shows which
 * relation each tuple came from.  The SetOp node scans each group of
 * identical tuples to determine how many came from each input relation.
 * Then it is a simple matter to emit the output demanded by the SQL spec
 * for INTERSECT, INTERSECT ALL, EXCEPT, or EXCEPT ALL.
 *
 * This node type is not used for UNION or UNION ALL, since those can be
 * implemented more cheaply (there's no need for the junk attribute to
 * identify the source relation).
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeSetOp.c,v 1.12 2003/08/04 02:39:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecSetOp		- filter input to generate INTERSECT/EXCEPT results
 *		ExecInitSetOp	- initialize node and subnodes..
 *		ExecEndSetOp	- shutdown node and subnodes
 */

#include "postgres.h"

#include "access/heapam.h"
#include "executor/executor.h"
#include "executor/nodeSetOp.h"


/* ----------------------------------------------------------------
 *		ExecSetOp
 * ----------------------------------------------------------------
 */
TupleTableSlot *				/* return: a tuple or NULL */
ExecSetOp(SetOpState *node)
{
	SetOp	   *plannode = (SetOp *) node->ps.plan;
	TupleTableSlot *resultTupleSlot;
	PlanState  *outerPlan;
	TupleDesc	tupDesc;

	/*
	 * get information from the node
	 */
	outerPlan = outerPlanState(node);
	resultTupleSlot = node->ps.ps_ResultTupleSlot;
	tupDesc = ExecGetResultType(&node->ps);

	/*
	 * If the previously-returned tuple needs to be returned more than
	 * once, keep returning it.
	 */
	if (node->numOutput > 0)
	{
		node->numOutput--;
		return resultTupleSlot;
	}

	/* Flag that we have no current tuple */
	ExecClearTuple(resultTupleSlot);

	/*
	 * Absorb groups of duplicate tuples, counting them, and saving the
	 * first of each group as a possible return value. At the end of each
	 * group, decide whether to return anything.
	 *
	 * We assume that the tuples arrive in sorted order so we can detect
	 * duplicates easily.
	 */
	for (;;)
	{
		TupleTableSlot *inputTupleSlot;
		bool		endOfGroup;

		/*
		 * fetch a tuple from the outer subplan, unless we already did.
		 */
		if (node->ps.ps_OuterTupleSlot == NULL &&
			!node->subplan_done)
		{
			node->ps.ps_OuterTupleSlot =
				ExecProcNode(outerPlan);
			if (TupIsNull(node->ps.ps_OuterTupleSlot))
				node->subplan_done = true;
		}
		inputTupleSlot = node->ps.ps_OuterTupleSlot;

		if (TupIsNull(resultTupleSlot))
		{
			/*
			 * First of group: save a copy in result slot, and reset
			 * duplicate-counters for new group.
			 */
			if (node->subplan_done)
				return NULL;	/* no more tuples */
			ExecStoreTuple(heap_copytuple(inputTupleSlot->val),
						   resultTupleSlot,
						   InvalidBuffer,
						   true);		/* free copied tuple at
										 * ExecClearTuple */
			node->numLeft = 0;
			node->numRight = 0;
			endOfGroup = false;
		}
		else if (node->subplan_done)
		{
			/*
			 * Reached end of input, so finish processing final group
			 */
			endOfGroup = true;
		}
		else
		{
			/*
			 * Else test if the new tuple and the previously saved tuple
			 * match.
			 */
			if (execTuplesMatch(inputTupleSlot->val,
								resultTupleSlot->val,
								tupDesc,
								plannode->numCols, plannode->dupColIdx,
								node->eqfunctions,
								node->tempContext))
				endOfGroup = false;
			else
				endOfGroup = true;
		}

		if (endOfGroup)
		{
			/*
			 * We've reached the end of the group containing resultTuple.
			 * Decide how many copies (if any) to emit.  This logic is
			 * straight from the SQL92 specification.
			 */
			switch (plannode->cmd)
			{
				case SETOPCMD_INTERSECT:
					if (node->numLeft > 0 && node->numRight > 0)
						node->numOutput = 1;
					else
						node->numOutput = 0;
					break;
				case SETOPCMD_INTERSECT_ALL:
					node->numOutput =
						(node->numLeft < node->numRight) ?
						node->numLeft : node->numRight;
					break;
				case SETOPCMD_EXCEPT:
					if (node->numLeft > 0 && node->numRight == 0)
						node->numOutput = 1;
					else
						node->numOutput = 0;
					break;
				case SETOPCMD_EXCEPT_ALL:
					node->numOutput =
						(node->numLeft < node->numRight) ?
						0 : (node->numLeft - node->numRight);
					break;
				default:
					elog(ERROR, "unrecognized set op: %d",
						 (int) plannode->cmd);
					break;
			}
			/* Fall out of for-loop if we have tuples to emit */
			if (node->numOutput > 0)
				break;
			/* Else flag that we have no current tuple, and loop around */
			ExecClearTuple(resultTupleSlot);
		}
		else
		{
			/*
			 * Current tuple is member of same group as resultTuple. Count
			 * it in the appropriate counter.
			 */
			int			flag;
			bool		isNull;

			flag = DatumGetInt32(heap_getattr(inputTupleSlot->val,
											  plannode->flagColIdx,
											  tupDesc,
											  &isNull));
			Assert(!isNull);
			if (flag)
				node->numRight++;
			else
				node->numLeft++;
			/* Set flag to fetch a new input tuple, and loop around */
			node->ps.ps_OuterTupleSlot = NULL;
		}
	}

	/*
	 * If we fall out of loop, then we need to emit at least one copy of
	 * resultTuple.
	 */
	Assert(node->numOutput > 0);
	node->numOutput--;
	return resultTupleSlot;
}

/* ----------------------------------------------------------------
 *		ExecInitSetOp
 *
 *		This initializes the setop node state structures and
 *		the node's subplan.
 * ----------------------------------------------------------------
 */
SetOpState *
ExecInitSetOp(SetOp *node, EState *estate)
{
	SetOpState *setopstate;

	/*
	 * create state structure
	 */
	setopstate = makeNode(SetOpState);
	setopstate->ps.plan = (Plan *) node;
	setopstate->ps.state = estate;

	setopstate->ps.ps_OuterTupleSlot = NULL;
	setopstate->subplan_done = false;
	setopstate->numOutput = 0;

	/*
	 * Miscellaneous initialization
	 *
	 * SetOp nodes have no ExprContext initialization because they never call
	 * ExecQual or ExecProject.  But they do need a per-tuple memory
	 * context anyway for calling execTuplesMatch.
	 */
	setopstate->tempContext =
		AllocSetContextCreate(CurrentMemoryContext,
							  "SetOp",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);

#define SETOP_NSLOTS 1

	/*
	 * Tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &setopstate->ps);

	/*
	 * then initialize outer plan
	 */
	outerPlanState(setopstate) = ExecInitNode(outerPlan(node), estate);

	/*
	 * setop nodes do no projections, so initialize projection info for
	 * this node appropriately
	 */
	ExecAssignResultTypeFromOuterPlan(&setopstate->ps);
	setopstate->ps.ps_ProjInfo = NULL;

	/*
	 * Precompute fmgr lookup data for inner loop
	 */
	setopstate->eqfunctions =
		execTuplesMatchPrepare(ExecGetResultType(&setopstate->ps),
							   node->numCols,
							   node->dupColIdx);

	return setopstate;
}

int
ExecCountSlotsSetOp(SetOp *node)
{
	return ExecCountSlotsNode(outerPlan(node)) +
		ExecCountSlotsNode(innerPlan(node)) +
		SETOP_NSLOTS;
}

/* ----------------------------------------------------------------
 *		ExecEndSetOp
 *
 *		This shuts down the subplan and frees resources allocated
 *		to this node.
 * ----------------------------------------------------------------
 */
void
ExecEndSetOp(SetOpState *node)
{
	/* clean up tuple table */
	ExecClearTuple(node->ps.ps_ResultTupleSlot);
	node->ps.ps_OuterTupleSlot = NULL;

	MemoryContextDelete(node->tempContext);

	ExecEndNode(outerPlanState(node));
}


void
ExecReScanSetOp(SetOpState *node, ExprContext *exprCtxt)
{
	ExecClearTuple(node->ps.ps_ResultTupleSlot);
	node->ps.ps_OuterTupleSlot = NULL;
	node->subplan_done = false;
	node->numOutput = 0;

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (((PlanState *) node)->lefttree->chgParam == NULL)
		ExecReScan(((PlanState *) node)->lefttree, exprCtxt);
}
