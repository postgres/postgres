/*-------------------------------------------------------------------------
 *
 * nodeGroup.c
 *	  Routines to handle group nodes (used for queries with GROUP BY clause).
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
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
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeGroup.c,v 1.52 2002/12/13 19:45:52 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/pg_operator.h"
#include "executor/executor.h"
#include "executor/nodeGroup.h"
#include "parser/parse_oper.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/*
 *	 ExecGroup -
 *
 *		Return one tuple for each group of matching input tuples.
 */
TupleTableSlot *
ExecGroup(GroupState *node)
{
	EState	   *estate;
	ExprContext *econtext;
	TupleDesc	tupdesc;
	int			numCols;
	AttrNumber *grpColIdx;
	HeapTuple	outerTuple = NULL;
	HeapTuple	firsttuple;
	TupleTableSlot *outerslot;
	ProjectionInfo *projInfo;
	TupleTableSlot *resultSlot;

	/*
	 * get state info from node
	 */
	if (node->grp_done)
		return NULL;
	estate = node->ss.ps.state;
	econtext = node->ss.ps.ps_ExprContext;
	tupdesc = ExecGetScanType(&node->ss);
	numCols = ((Group *) node->ss.ps.plan)->numCols;
	grpColIdx = ((Group *) node->ss.ps.plan)->grpColIdx;

	/*
	 * We need not call ResetExprContext here because execTuplesMatch will
	 * reset the per-tuple memory context once per input tuple.
	 */

	/* If we don't already have first tuple of group, fetch it */
	/* this should occur on the first call only */
	firsttuple = node->grp_firstTuple;
	if (firsttuple == NULL)
	{
		outerslot = ExecProcNode(outerPlanState(node));
		if (TupIsNull(outerslot))
		{
			node->grp_done = TRUE;
			return NULL;
		}
		node->grp_firstTuple = firsttuple =
			heap_copytuple(outerslot->val);
	}

	/*
	 * Scan over all tuples that belong to this group
	 */
	for (;;)
	{
		outerslot = ExecProcNode(outerPlanState(node));
		if (TupIsNull(outerslot))
		{
			node->grp_done = TRUE;
			outerTuple = NULL;
			break;
		}
		outerTuple = outerslot->val;

		/*
		 * Compare with first tuple and see if this tuple is of the same
		 * group.
		 */
		if (!execTuplesMatch(firsttuple, outerTuple,
							 tupdesc,
							 numCols, grpColIdx,
							 node->eqfunctions,
							 econtext->ecxt_per_tuple_memory))
			break;
	}

	/*
	 * form a projection tuple based on the (copied) first tuple of the
	 * group, and store it in the result tuple slot.
	 */
	ExecStoreTuple(firsttuple,
				   node->ss.ss_ScanTupleSlot,
				   InvalidBuffer,
				   false);
	econtext->ecxt_scantuple = node->ss.ss_ScanTupleSlot;
	projInfo = node->ss.ps.ps_ProjInfo;
	resultSlot = ExecProject(projInfo, NULL);

	/* save first tuple of next group, if we are not done yet */
	if (!node->grp_done)
	{
		heap_freetuple(firsttuple);
		node->grp_firstTuple = heap_copytuple(outerTuple);
	}

	return resultSlot;
}

/* -----------------
 * ExecInitGroup
 *
 *	Creates the run-time information for the group node produced by the
 *	planner and initializes its outer subtree
 * -----------------
 */
GroupState *
ExecInitGroup(Group *node, EState *estate)
{
	GroupState *grpstate;

	/*
	 * create state structure
	 */
	grpstate = makeNode(GroupState);
	grpstate->ss.ps.plan = (Plan *) node;
	grpstate->ss.ps.state = estate;
	grpstate->grp_firstTuple = NULL;
	grpstate->grp_done = FALSE;

	/*
	 * create expression context
	 */
	ExecAssignExprContext(estate, &grpstate->ss.ps);

#define GROUP_NSLOTS 2

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
	outerPlanState(grpstate) = ExecInitNode(outerPlan(node), estate);

	/*
	 * initialize tuple type.
	 */
	ExecAssignScanTypeFromOuterPlan(&grpstate->ss);

	/*
	 * Initialize tuple type for both result and scan. This node does no
	 * projection
	 */
	ExecAssignResultTypeFromTL(&grpstate->ss.ps);
	ExecAssignProjectionInfo(&grpstate->ss.ps);

	/*
	 * Precompute fmgr lookup data for inner loop
	 */
	grpstate->eqfunctions =
		execTuplesMatchPrepare(ExecGetScanType(&grpstate->ss),
							   node->numCols,
							   node->grpColIdx);

	return grpstate;
}

int
ExecCountSlotsGroup(Group *node)
{
	return ExecCountSlotsNode(outerPlan(node)) + GROUP_NSLOTS;
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

	ExecFreeProjectionInfo(&node->ss.ps);
	ExecFreeExprContext(&node->ss.ps);

	outerPlan = outerPlanState(node);
	ExecEndNode(outerPlan);

	/* clean up tuple table */
	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	if (node->grp_firstTuple != NULL)
	{
		heap_freetuple(node->grp_firstTuple);
		node->grp_firstTuple = NULL;
	}
}

void
ExecReScanGroup(GroupState *node, ExprContext *exprCtxt)
{
	node->grp_done = FALSE;
	if (node->grp_firstTuple != NULL)
	{
		heap_freetuple(node->grp_firstTuple);
		node->grp_firstTuple = NULL;
	}

	if (((PlanState *) node)->lefttree &&
		((PlanState *) node)->lefttree->chgParam == NULL)
		ExecReScan(((PlanState *) node)->lefttree, exprCtxt);
}

/*****************************************************************************
 *		Code shared with nodeUnique.c and nodeAgg.c
 *****************************************************************************/

/*
 * execTuplesMatch
 *		Return true if two tuples match in all the indicated fields.
 *		This is used to detect group boundaries in nodeGroup and nodeAgg,
 *		and to decide whether two tuples are distinct or not in nodeUnique.
 *
 * tuple1, tuple2: the tuples to compare
 * tupdesc: tuple descriptor applying to both tuples
 * numCols: the number of attributes to be examined
 * matchColIdx: array of attribute column numbers
 * eqFunctions: array of fmgr lookup info for the equality functions to use
 * evalContext: short-term memory context for executing the functions
 *
 * NB: evalContext is reset each time!
 */
bool
execTuplesMatch(HeapTuple tuple1,
				HeapTuple tuple2,
				TupleDesc tupdesc,
				int numCols,
				AttrNumber *matchColIdx,
				FmgrInfo *eqfunctions,
				MemoryContext evalContext)
{
	MemoryContext oldContext;
	bool		result;
	int			i;

	/* Reset and switch into the temp context. */
	MemoryContextReset(evalContext);
	oldContext = MemoryContextSwitchTo(evalContext);

	/*
	 * We cannot report a match without checking all the fields, but we
	 * can report a non-match as soon as we find unequal fields.  So,
	 * start comparing at the last field (least significant sort key).
	 * That's the most likely to be different if we are dealing with
	 * sorted input.
	 */
	result = true;

	for (i = numCols; --i >= 0;)
	{
		AttrNumber	att = matchColIdx[i];
		Datum		attr1,
					attr2;
		bool		isNull1,
					isNull2;

		attr1 = heap_getattr(tuple1,
							 att,
							 tupdesc,
							 &isNull1);

		attr2 = heap_getattr(tuple2,
							 att,
							 tupdesc,
							 &isNull2);

		if (isNull1 != isNull2)
		{
			result = false;		/* one null and one not; they aren't equal */
			break;
		}

		if (isNull1)
			continue;			/* both are null, treat as equal */

		/* Apply the type-specific equality function */

		if (!DatumGetBool(FunctionCall2(&eqfunctions[i],
										attr1, attr2)))
		{
			result = false;		/* they aren't equal */
			break;
		}
	}

	MemoryContextSwitchTo(oldContext);

	return result;
}

/*
 * execTuplesMatchPrepare
 *		Look up the equality functions needed for execTuplesMatch.
 *		The result is a palloc'd array.
 */
FmgrInfo *
execTuplesMatchPrepare(TupleDesc tupdesc,
					   int numCols,
					   AttrNumber *matchColIdx)
{
	FmgrInfo   *eqfunctions = (FmgrInfo *) palloc(numCols * sizeof(FmgrInfo));
	int			i;

	for (i = 0; i < numCols; i++)
	{
		AttrNumber	att = matchColIdx[i];
		Oid			typid = tupdesc->attrs[att - 1]->atttypid;
		Oid			eq_function;

		eq_function = equality_oper_funcid(typid);
		fmgr_info(eq_function, &eqfunctions[i]);
	}

	return eqfunctions;
}
