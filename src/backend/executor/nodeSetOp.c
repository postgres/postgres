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
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeSetOp.c,v 1.3 2001/03/22 03:59:29 momjian Exp $
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
#include "executor/nodeGroup.h"
#include "executor/nodeSetOp.h"

/* ----------------------------------------------------------------
 *		ExecSetOp
 * ----------------------------------------------------------------
 */
TupleTableSlot *				/* return: a tuple or NULL */
ExecSetOp(SetOp *node)
{
	SetOpState *setopstate;
	TupleTableSlot *resultTupleSlot;
	Plan	   *outerPlan;
	TupleDesc	tupDesc;

	/* ----------------
	 *	get information from the node
	 * ----------------
	 */
	setopstate = node->setopstate;
	outerPlan = outerPlan((Plan *) node);
	resultTupleSlot = setopstate->cstate.cs_ResultTupleSlot;
	tupDesc = ExecGetResultType(&setopstate->cstate);

	/* ----------------
	 *	If the previously-returned tuple needs to be returned more than
	 *	once, keep returning it.
	 * ----------------
	 */
	if (setopstate->numOutput > 0)
	{
		setopstate->numOutput--;
		return resultTupleSlot;
	}

	/* Flag that we have no current tuple */
	ExecClearTuple(resultTupleSlot);

	/* ----------------
	 *	Absorb groups of duplicate tuples, counting them, and
	 *	saving the first of each group as a possible return value.
	 *	At the end of each group, decide whether to return anything.
	 *
	 *	We assume that the tuples arrive in sorted order
	 *	so we can detect duplicates easily.
	 * ----------------
	 */
	for (;;)
	{
		TupleTableSlot *inputTupleSlot;
		bool		endOfGroup;

		/* ----------------
		 *	 fetch a tuple from the outer subplan, unless we already did.
		 * ----------------
		 */
		if (setopstate->cstate.cs_OuterTupleSlot == NULL &&
			!setopstate->subplan_done)
		{
			setopstate->cstate.cs_OuterTupleSlot =
				ExecProcNode(outerPlan, (Plan *) node);
			if (TupIsNull(setopstate->cstate.cs_OuterTupleSlot))
				setopstate->subplan_done = true;
		}
		inputTupleSlot = setopstate->cstate.cs_OuterTupleSlot;

		if (TupIsNull(resultTupleSlot))
		{

			/*
			 * First of group: save a copy in result slot, and reset
			 * duplicate-counters for new group.
			 */
			if (setopstate->subplan_done)
				return NULL;	/* no more tuples */
			ExecStoreTuple(heap_copytuple(inputTupleSlot->val),
						   resultTupleSlot,
						   InvalidBuffer,
						   true);		/* free copied tuple at
										 * ExecClearTuple */
			setopstate->numLeft = 0;
			setopstate->numRight = 0;
			endOfGroup = false;
		}
		else if (setopstate->subplan_done)
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
								node->numCols, node->dupColIdx,
								setopstate->eqfunctions,
								setopstate->tempContext))
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
			switch (node->cmd)
			{
				case SETOPCMD_INTERSECT:
					if (setopstate->numLeft > 0 && setopstate->numRight > 0)
						setopstate->numOutput = 1;
					else
						setopstate->numOutput = 0;
					break;
				case SETOPCMD_INTERSECT_ALL:
					setopstate->numOutput =
						(setopstate->numLeft < setopstate->numRight) ?
						setopstate->numLeft : setopstate->numRight;
					break;
				case SETOPCMD_EXCEPT:
					if (setopstate->numLeft > 0 && setopstate->numRight == 0)
						setopstate->numOutput = 1;
					else
						setopstate->numOutput = 0;
					break;
				case SETOPCMD_EXCEPT_ALL:
					setopstate->numOutput =
						(setopstate->numLeft < setopstate->numRight) ?
						0 : (setopstate->numLeft - setopstate->numRight);
					break;
				default:
					elog(ERROR, "ExecSetOp: bogus command code %d",
						 (int) node->cmd);
					break;
			}
			/* Fall out of for-loop if we have tuples to emit */
			if (setopstate->numOutput > 0)
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
											  node->flagColIdx,
											  tupDesc,
											  &isNull));
			Assert(!isNull);
			if (flag)
				setopstate->numRight++;
			else
				setopstate->numLeft++;
			/* Set flag to fetch a new input tuple, and loop around */
			setopstate->cstate.cs_OuterTupleSlot = NULL;
		}
	}

	/*
	 * If we fall out of loop, then we need to emit at least one copy of
	 * resultTuple.
	 */
	Assert(setopstate->numOutput > 0);
	setopstate->numOutput--;
	return resultTupleSlot;
}

/* ----------------------------------------------------------------
 *		ExecInitSetOp
 *
 *		This initializes the setop node state structures and
 *		the node's subplan.
 * ----------------------------------------------------------------
 */
bool							/* return: initialization status */
ExecInitSetOp(SetOp *node, EState *estate, Plan *parent)
{
	SetOpState *setopstate;
	Plan	   *outerPlan;

	/* ----------------
	 *	assign execution state to node
	 * ----------------
	 */
	node->plan.state = estate;

	/* ----------------
	 *	create new SetOpState for node
	 * ----------------
	 */
	setopstate = makeNode(SetOpState);
	node->setopstate = setopstate;
	setopstate->cstate.cs_OuterTupleSlot = NULL;
	setopstate->subplan_done = false;
	setopstate->numOutput = 0;

	/* ----------------
	 *	Miscellaneous initialization
	 *
	 *	SetOp nodes have no ExprContext initialization because
	 *	they never call ExecQual or ExecProject.  But they do need a
	 *	per-tuple memory context anyway for calling execTuplesMatch.
	 * ----------------
	 */
	setopstate->tempContext =
		AllocSetContextCreate(CurrentMemoryContext,
							  "SetOp",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);

#define SETOP_NSLOTS 1
	/* ------------
	 * Tuple table initialization
	 * ------------
	 */
	ExecInitResultTupleSlot(estate, &setopstate->cstate);

	/* ----------------
	 *	then initialize outer plan
	 * ----------------
	 */
	outerPlan = outerPlan((Plan *) node);
	ExecInitNode(outerPlan, estate, (Plan *) node);

	/* ----------------
	 *	setop nodes do no projections, so initialize
	 *	projection info for this node appropriately
	 * ----------------
	 */
	ExecAssignResultTypeFromOuterPlan((Plan *) node, &setopstate->cstate);
	setopstate->cstate.cs_ProjInfo = NULL;

	/*
	 * Precompute fmgr lookup data for inner loop
	 */
	setopstate->eqfunctions =
		execTuplesMatchPrepare(ExecGetResultType(&setopstate->cstate),
							   node->numCols,
							   node->dupColIdx);

	return TRUE;
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
ExecEndSetOp(SetOp *node)
{
	SetOpState *setopstate = node->setopstate;

	ExecEndNode(outerPlan((Plan *) node), (Plan *) node);

	MemoryContextDelete(setopstate->tempContext);

	/* clean up tuple table */
	ExecClearTuple(setopstate->cstate.cs_ResultTupleSlot);
	setopstate->cstate.cs_OuterTupleSlot = NULL;
}


void
ExecReScanSetOp(SetOp *node, ExprContext *exprCtxt, Plan *parent)
{
	SetOpState *setopstate = node->setopstate;

	ExecClearTuple(setopstate->cstate.cs_ResultTupleSlot);
	setopstate->cstate.cs_OuterTupleSlot = NULL;
	setopstate->subplan_done = false;
	setopstate->numOutput = 0;

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (((Plan *) node)->lefttree->chgParam == NULL)
		ExecReScan(((Plan *) node)->lefttree, exprCtxt, (Plan *) node);
}
