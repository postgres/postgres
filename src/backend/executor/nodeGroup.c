/*-------------------------------------------------------------------------
 *
 * nodeGroup.c--
 *	  Routines to handle group nodes (used for queries with GROUP BY clause).
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * DESCRIPTION
 *	  The Group node is designed for handling queries with a GROUP BY clause.
 *	  It's outer plan must be a sort node. It assumes that the tuples it gets
 *	  back from the outer plan is sorted in the order specified by the group
 *	  columns. (ie. tuples from the same group are consecutive)
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeGroup.c,v 1.21 1998/07/16 02:58:09 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>

#include "postgres.h"
#include "fmgr.h"

#include "access/heapam.h"
#include "catalog/catalog.h"
#include "access/printtup.h"
#include "executor/executor.h"
#include "executor/nodeGroup.h"

static TupleTableSlot *ExecGroupEveryTuple(Group *node);
static TupleTableSlot *ExecGroupOneTuple(Group *node);
static bool
sameGroup(HeapTuple oldslot, HeapTuple newslot,
		  int numCols, AttrNumber *grpColIdx, TupleDesc tupdesc);

/* ---------------------------------------
 *	 ExecGroup -
 *
 *		There are two modes in which tuples are returned by ExecGroup. If
 *		tuplePerGroup is TRUE, every tuple from the same group will be
 *		returned, followed by a NULL at the end of each group. This is
 *		useful for Agg node which needs to aggregate over tuples of the same
 *		group. (eg. SELECT salary, count{*} FROM emp GROUP BY salary)
 *
 *		If tuplePerGroup is FALSE, only one tuple per group is returned. The
 *		tuple returned contains only the group columns. NULL is returned only
 *		at the end when no more groups is present. This is useful when
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

	HeapTuple	outerTuple = NULL;
	HeapTuple	firsttuple;
	TupleTableSlot *outerslot;
	ProjectionInfo *projInfo;
	TupleTableSlot *resultSlot;

	bool		isDone;

	/* ---------------------
	 *	get state info from node
	 * ---------------------
	 */
	grpstate = node->grpstate;
	if (grpstate->grp_done)
		return NULL;

	estate = node->plan.state;

	econtext = grpstate->csstate.cstate.cs_ExprContext;

	/* if we haven't returned first tuple of new group yet ... */
	if (grpstate->grp_useFirstTuple)
	{
		grpstate->grp_useFirstTuple = FALSE;

		ExecStoreTuple(grpstate->grp_firstTuple,
					   grpstate->csstate.css_ScanTupleSlot,
					   InvalidBuffer,
					   false);
	}
	else
	{
		outerslot = ExecProcNode(outerPlan(node), (Plan *) node);
		if (outerslot)
			outerTuple = outerslot->val;
		if (!HeapTupleIsValid(outerTuple))
		{
			grpstate->grp_done = TRUE;
			return NULL;
		}

		firsttuple = grpstate->grp_firstTuple;
		/* this should occur on the first call only */
		if (firsttuple == NULL)
			grpstate->grp_firstTuple = heap_copytuple(outerTuple);
		else
		{

			/*
			 * Compare with first tuple and see if this tuple is of the
			 * same group.
			 */
			if (!sameGroup(firsttuple, outerslot->val,
						   node->numCols, node->grpColIdx,
						   ExecGetScanType(&grpstate->csstate)))
			{
				grpstate->grp_useFirstTuple = TRUE;
				pfree(firsttuple);
				grpstate->grp_firstTuple = heap_copytuple(outerTuple);

				return NULL;	/* signifies the end of the group */
			}
		}

		ExecStoreTuple(outerTuple,
					   grpstate->csstate.css_ScanTupleSlot,
					   outerslot->ttc_buffer,
					   false);
	}

	/* ----------------
	 *	form a projection tuple, store it in the result tuple
	 *	slot and return it.
	 * ----------------
	 */
	projInfo = grpstate->csstate.cstate.cs_ProjInfo;

	econtext->ecxt_scantuple = grpstate->csstate.css_ScanTupleSlot;
	resultSlot = ExecProject(projInfo, &isDone);

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

	HeapTuple	outerTuple = NULL;
	HeapTuple	firsttuple;
	TupleTableSlot *outerslot;
	ProjectionInfo *projInfo;
	TupleTableSlot *resultSlot;

	bool		isDone;

	/* ---------------------
	 *	get state info from node
	 * ---------------------
	 */
	grpstate = node->grpstate;
	if (grpstate->grp_done)
		return NULL;

	estate = node->plan.state;

	econtext = node->grpstate->csstate.cstate.cs_ExprContext;

	firsttuple = grpstate->grp_firstTuple;
	/* this should occur on the first call only */
	if (firsttuple == NULL)
	{
		outerslot = ExecProcNode(outerPlan(node), (Plan *) node);
		if (outerslot)
			outerTuple = outerslot->val;
		if (!HeapTupleIsValid(outerTuple))
		{
			grpstate->grp_done = TRUE;
			return NULL;
		}
		grpstate->grp_firstTuple = firsttuple = heap_copytuple(outerTuple);
	}

	/*
	 * find all tuples that belong to a group
	 */
	for (;;)
	{
		outerslot = ExecProcNode(outerPlan(node), (Plan *) node);
		outerTuple = (outerslot) ? outerslot->val : NULL;
		if (!HeapTupleIsValid(outerTuple))
		{
			grpstate->grp_done = TRUE;
			break;
		}

		/* ----------------
		 *	Compare with first tuple and see if this tuple is of
		 *	the same group.
		 * ----------------
		 */
		if ((!sameGroup(firsttuple, outerslot->val,
						node->numCols, node->grpColIdx,
						ExecGetScanType(&grpstate->csstate))))
			break;
	}

	/* ----------------
	 *	form a projection tuple, store it in the result tuple
	 *	slot and return it.
	 * ----------------
	 */
	projInfo = grpstate->csstate.cstate.cs_ProjInfo;

	ExecStoreTuple(firsttuple,
				   grpstate->csstate.css_ScanTupleSlot,
				   InvalidBuffer,
				   false);
	econtext->ecxt_scantuple = grpstate->csstate.css_ScanTupleSlot;
	resultSlot = ExecProject(projInfo, &isDone);

	/* save outerTuple if we are not done yet */
	if (!grpstate->grp_done)
	{
		pfree(firsttuple);
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

	/*
	 * assign node's base id and create expression context
	 */
	ExecAssignNodeBaseInfo(estate, &grpstate->csstate.cstate,
						   (Plan *) parent);
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

	outerPlan = outerPlan(node);
	ExecEndNode(outerPlan, (Plan *) node);

	/* clean up tuple table */
	ExecClearTuple(grpstate->csstate.css_ScanTupleSlot);
	if (grpstate->grp_firstTuple != NULL)
	{
		pfree(grpstate->grp_firstTuple);
		grpstate->grp_firstTuple = NULL;
	}
}

/*****************************************************************************
 *
 *****************************************************************************/

/*
 * code swiped from nodeUnique.c
 */
static bool
sameGroup(HeapTuple oldtuple,
		  HeapTuple newtuple,
		  int numCols,
		  AttrNumber *grpColIdx,
		  TupleDesc tupdesc)
{
	bool		isNull1,
				isNull2;
	Datum		attr1,
				attr2;
	char	   *val1,
			   *val2;
	int			i;
	AttrNumber	att;
	Oid			typoutput;

	for (i = 0; i < numCols; i++)
	{
		att = grpColIdx[i];
		typoutput = typtoout((Oid) tupdesc->attrs[att - 1]->atttypid);

		attr1 = heap_getattr(oldtuple,
							 att,
							 tupdesc,
							 &isNull1);

		attr2 = heap_getattr(newtuple,
							 att,
							 tupdesc,
							 &isNull2);

		if (isNull1 == isNull2)
		{
			if (isNull1)		/* both are null, they are equal */
				continue;

			val1 = fmgr(typoutput, attr1,
						gettypelem(tupdesc->attrs[att - 1]->atttypid),
						tupdesc->attrs[att - 1]->atttypmod);
			val2 = fmgr(typoutput, attr2,
						gettypelem(tupdesc->attrs[att - 1]->atttypid),
						tupdesc->attrs[att - 1]->atttypmod);

			/*
			 * now, val1 and val2 are ascii representations so we can use
			 * strcmp for comparison
			 */
			if (strcmp(val1, val2) != 0)
			{
				pfree(val1);
				pfree(val2);
				return FALSE;
			}
			pfree(val1);
			pfree(val2);
		}
		else
		{
			/* one is null and the other isn't, they aren't equal */
			return FALSE;
		}
	}

	return TRUE;
}

void
ExecReScanGroup(Group *node, ExprContext *exprCtxt, Plan *parent)
{
	GroupState *grpstate = node->grpstate;

	grpstate->grp_useFirstTuple = FALSE;
	grpstate->grp_done = FALSE;

	if (((Plan *) node)->lefttree &&
		((Plan *) node)->lefttree->chgParam == NULL)
		ExecReScan(((Plan *) node)->lefttree, exprCtxt, (Plan *) node);
}
