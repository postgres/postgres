/*-------------------------------------------------------------------------
 *
 * nodeGroup.c
 *	  Routines to handle group nodes (used for queries with GROUP BY clause).
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
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
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeGroup.c,v 1.39 2000/11/16 22:30:22 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/pg_operator.h"
#include "executor/executor.h"
#include "executor/nodeGroup.h"
#include "parser/parse_oper.h"
#include "parser/parse_type.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

static TupleTableSlot *ExecGroupEveryTuple(Group *node);
static TupleTableSlot *ExecGroupOneTuple(Group *node);

/* ---------------------------------------
 *	 ExecGroup -
 *
 *		There are two modes in which tuples are returned by ExecGroup. If
 *		tuplePerGroup is TRUE, every tuple from the same group will be
 *		returned, followed by a NULL at the end of each group. This is
 *		useful for Agg node which needs to aggregate over tuples of the same
 *		group. (eg. SELECT salary, count(*) FROM emp GROUP BY salary)
 *
 *		If tuplePerGroup is FALSE, only one tuple per group is returned. The
 *		tuple returned contains only the group columns. NULL is returned only
 *		at the end when no more groups are present. This is useful when
 *		the query does not involve aggregates. (eg. SELECT salary FROM emp
 *		GROUP BY salary)
 * ------------------------------------------
 */
TupleTableSlot *
ExecGroup(Group *node)
{
	if (node->tuplePerGroup)
		return ExecGroupEveryTuple(node);
	else
		return ExecGroupOneTuple(node);
}

/*
 * ExecGroupEveryTuple -
 *	 return every tuple with a NULL between each group
 */
static TupleTableSlot *
ExecGroupEveryTuple(Group *node)
{
	GroupState *grpstate;
	EState	   *estate;
	ExprContext *econtext;
	TupleDesc	tupdesc;
	HeapTuple	outerTuple = NULL;
	HeapTuple	firsttuple;
	TupleTableSlot *outerslot;
	ProjectionInfo *projInfo;
	TupleTableSlot *resultSlot;

	/* ---------------------
	 *	get state info from node
	 * ---------------------
	 */
	grpstate = node->grpstate;
	if (grpstate->grp_done)
		return NULL;
	estate = node->plan.state;
	econtext = grpstate->csstate.cstate.cs_ExprContext;
	tupdesc = ExecGetScanType(&grpstate->csstate);

	/*
	 *	We need not call ResetExprContext here because execTuplesMatch
	 *	will reset the per-tuple memory context once per input tuple.
	 */

	/* if we haven't returned first tuple of a new group yet ... */
	if (grpstate->grp_useFirstTuple)
	{
		grpstate->grp_useFirstTuple = FALSE;

		/*
		 * note we rely on subplan to hold ownership of the tuple for as
		 * long as we need it; we don't copy it.
		 */
		ExecStoreTuple(grpstate->grp_firstTuple,
					   grpstate->csstate.css_ScanTupleSlot,
					   InvalidBuffer, false);
	}
	else
	{
		outerslot = ExecProcNode(outerPlan(node), (Plan *) node);
		if (TupIsNull(outerslot))
		{
			grpstate->grp_done = TRUE;
			return NULL;
		}
		outerTuple = outerslot->val;

		firsttuple = grpstate->grp_firstTuple;
		if (firsttuple == NULL)
		{
			/* this should occur on the first call only */
			grpstate->grp_firstTuple = heap_copytuple(outerTuple);
		}
		else
		{

			/*
			 * Compare with first tuple and see if this tuple is of the
			 * same group.
			 */
			if (!execTuplesMatch(firsttuple, outerTuple,
								 tupdesc,
								 node->numCols, node->grpColIdx,
								 grpstate->eqfunctions,
								 econtext->ecxt_per_tuple_memory))
			{

				/*
				 * No; save the tuple to return it next time, and return
				 * NULL
				 */
				grpstate->grp_useFirstTuple = TRUE;
				heap_freetuple(firsttuple);
				grpstate->grp_firstTuple = heap_copytuple(outerTuple);

				return NULL;	/* signifies the end of the group */
			}
		}

		/*
		 * note we rely on subplan to hold ownership of the tuple for as
		 * long as we need it; we don't copy it.
		 */
		ExecStoreTuple(outerTuple,
					   grpstate->csstate.css_ScanTupleSlot,
					   InvalidBuffer, false);
	}

	/* ----------------
	 *	form a projection tuple, store it in the result tuple
	 *	slot and return it.
	 * ----------------
	 */
	projInfo = grpstate->csstate.cstate.cs_ProjInfo;

	econtext->ecxt_scantuple = grpstate->csstate.css_ScanTupleSlot;
	resultSlot = ExecProject(projInfo, NULL);

	return resultSlot;
}

/*
 * ExecGroupOneTuple -
 *	  returns one tuple per group, a NULL at the end when there are no more
 *	  tuples.
 */
static TupleTableSlot *
ExecGroupOneTuple(Group *node)
{
	GroupState *grpstate;
	EState	   *estate;
	ExprContext *econtext;
	TupleDesc	tupdesc;
	HeapTuple	outerTuple = NULL;
	HeapTuple	firsttuple;
	TupleTableSlot *outerslot;
	ProjectionInfo *projInfo;
	TupleTableSlot *resultSlot;

	/* ---------------------
	 *	get state info from node
	 * ---------------------
	 */
	grpstate = node->grpstate;
	if (grpstate->grp_done)
		return NULL;
	estate = node->plan.state;
	econtext = node->grpstate->csstate.cstate.cs_ExprContext;
	tupdesc = ExecGetScanType(&grpstate->csstate);

	/*
	 *	We need not call ResetExprContext here because execTuplesMatch
	 *	will reset the per-tuple memory context once per input tuple.
	 */

	firsttuple = grpstate->grp_firstTuple;
	if (firsttuple == NULL)
	{
		/* this should occur on the first call only */
		outerslot = ExecProcNode(outerPlan(node), (Plan *) node);
		if (TupIsNull(outerslot))
		{
			grpstate->grp_done = TRUE;
			return NULL;
		}
		grpstate->grp_firstTuple = firsttuple =
			heap_copytuple(outerslot->val);
	}

	/*
	 * find all tuples that belong to a group
	 */
	for (;;)
	{
		outerslot = ExecProcNode(outerPlan(node), (Plan *) node);
		if (TupIsNull(outerslot))
		{
			grpstate->grp_done = TRUE;
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
							 node->numCols, node->grpColIdx,
							 grpstate->eqfunctions,
							 econtext->ecxt_per_tuple_memory))
			break;
	}

	/* ----------------
	 *	form a projection tuple, store it in the result tuple
	 *	slot and return it.
	 * ----------------
	 */
	projInfo = grpstate->csstate.cstate.cs_ProjInfo;

	/*
	 * note we rely on subplan to hold ownership of the tuple for as long
	 * as we need it; we don't copy it.
	 */
	ExecStoreTuple(firsttuple,
				   grpstate->csstate.css_ScanTupleSlot,
				   InvalidBuffer, false);
	econtext->ecxt_scantuple = grpstate->csstate.css_ScanTupleSlot;
	resultSlot = ExecProject(projInfo, NULL);

	/* save outerTuple if we are not done yet */
	if (!grpstate->grp_done)
	{
		heap_freetuple(firsttuple);
		grpstate->grp_firstTuple = heap_copytuple(outerTuple);
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
bool
ExecInitGroup(Group *node, EState *estate, Plan *parent)
{
	GroupState *grpstate;
	Plan	   *outerPlan;

	/*
	 * assign the node's execution state
	 */
	node->plan.state = estate;

	/*
	 * create state structure
	 */
	grpstate = makeNode(GroupState);
	node->grpstate = grpstate;
	grpstate->grp_useFirstTuple = FALSE;
	grpstate->grp_done = FALSE;
	grpstate->grp_firstTuple = NULL;

	/*
	 * create expression context
	 */
	ExecAssignExprContext(estate, &grpstate->csstate.cstate);

#define GROUP_NSLOTS 2

	/*
	 * tuple table initialization
	 */
	ExecInitScanTupleSlot(estate, &grpstate->csstate);
	ExecInitResultTupleSlot(estate, &grpstate->csstate.cstate);

	/*
	 * initializes child nodes
	 */
	outerPlan = outerPlan(node);
	ExecInitNode(outerPlan, estate, (Plan *) node);

	/* ----------------
	 *	initialize tuple type.
	 * ----------------
	 */
	ExecAssignScanTypeFromOuterPlan((Plan *) node, &grpstate->csstate);

	/*
	 * Initialize tuple type for both result and scan. This node does no
	 * projection
	 */
	ExecAssignResultTypeFromTL((Plan *) node, &grpstate->csstate.cstate);
	ExecAssignProjectionInfo((Plan *) node, &grpstate->csstate.cstate);

	/*
	 * Precompute fmgr lookup data for inner loop
	 */
	grpstate->eqfunctions =
		execTuplesMatchPrepare(ExecGetScanType(&grpstate->csstate),
							   node->numCols,
							   node->grpColIdx);

	return TRUE;
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
ExecEndGroup(Group *node)
{
	GroupState *grpstate;
	Plan	   *outerPlan;

	grpstate = node->grpstate;

	ExecFreeProjectionInfo(&grpstate->csstate.cstate);
	ExecFreeExprContext(&grpstate->csstate.cstate);

	outerPlan = outerPlan(node);
	ExecEndNode(outerPlan, (Plan *) node);

	/* clean up tuple table */
	ExecClearTuple(grpstate->csstate.css_ScanTupleSlot);
	if (grpstate->grp_firstTuple != NULL)
	{
		heap_freetuple(grpstate->grp_firstTuple);
		grpstate->grp_firstTuple = NULL;
	}
}

void
ExecReScanGroup(Group *node, ExprContext *exprCtxt, Plan *parent)
{
	GroupState *grpstate = node->grpstate;

	grpstate->grp_useFirstTuple = FALSE;
	grpstate->grp_done = FALSE;
	if (grpstate->grp_firstTuple != NULL)
	{
		heap_freetuple(grpstate->grp_firstTuple);
		grpstate->grp_firstTuple = NULL;
	}

	if (((Plan *) node)->lefttree &&
		((Plan *) node)->lefttree->chgParam == NULL)
		ExecReScan(((Plan *) node)->lefttree, exprCtxt, (Plan *) node);
}

/*****************************************************************************
 *		Code shared with nodeUnique.c
 *****************************************************************************/

/*
 * execTuplesMatch
 *		Return true if two tuples match in all the indicated fields.
 *		This is used to detect group boundaries in nodeGroup, and to
 *		decide whether two tuples are distinct or not in nodeUnique.
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
	 * That's the most likely to be different...
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

		if (! DatumGetBool(FunctionCall2(&eqfunctions[i],
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
FmgrInfo   *
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
		Operator	eq_operator;
		Form_pg_operator pgopform;

		eq_operator = oper("=", typid, typid, true);
		if (!HeapTupleIsValid(eq_operator))
			elog(ERROR, "Unable to identify an equality operator for type '%s'",
				 typeidTypeName(typid));
		pgopform = (Form_pg_operator) GETSTRUCT(eq_operator);
		fmgr_info(pgopform->oprcode, &eqfunctions[i]);
		ReleaseSysCache(eq_operator);
	}

	return eqfunctions;
}
