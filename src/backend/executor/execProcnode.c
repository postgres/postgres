/*-------------------------------------------------------------------------
 *
 * execProcnode.c
 *	 contains dispatch functions which call the appropriate "initialize",
 *	 "get a tuple", and "cleanup" routines for the given node type.
 *	 If the node has children, then it will presumably call ExecInitNode,
 *	 ExecProcNode, or ExecEndNode on its subnodes and do the appropriate
 *	 processing..
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/execProcnode.c,v 1.26 2001/03/22 06:16:12 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 INTERFACE ROUTINES
 *		ExecInitNode	-		initialize a plan node and its subplans
 *		ExecProcNode	-		get a tuple by executing the plan node
 *		ExecEndNode		-		shut down a plan node and its subplans
 *		ExecCountSlotsNode -	count tuple slots needed by plan tree
 *		ExecGetTupType	-		get result tuple type of a plan node
 *
 *	 NOTES
 *		This used to be three files.  It is now all combined into
 *		one file so that it is easier to keep ExecInitNode, ExecProcNode,
 *		and ExecEndNode in sync when new nodes are added.
 *
 *	 EXAMPLE
 *		suppose we want the age of the manager of the shoe department and
 *		the number of employees in that department.  so we have the query:
 *
 *				retrieve (DEPT.no_emps, EMP.age)
 *				where EMP.name = DEPT.mgr and
 *					  DEPT.name = "shoe"
 *
 *		Suppose the planner gives us the following plan:
 *
 *						Nest Loop (DEPT.mgr = EMP.name)
 *						/		\
 *					   /		 \
 *				   Seq Scan		Seq Scan
 *					DEPT		  EMP
 *				(name = "shoe")
 *
 *		ExecStart() is called first.
 *		It calls InitPlan() which calls ExecInitNode() on
 *		the root of the plan -- the nest loop node.
 *
 *	  * ExecInitNode() notices that it is looking at a nest loop and
 *		as the code below demonstrates, it calls ExecInitNestLoop().
 *		Eventually this calls ExecInitNode() on the right and left subplans
 *		and so forth until the entire plan is initialized.
 *
 *	  * Then when ExecRun() is called, it calls ExecutePlan() which
 *		calls ExecProcNode() repeatedly on the top node of the plan.
 *		Each time this happens, ExecProcNode() will end up calling
 *		ExecNestLoop(), which calls ExecProcNode() on its subplans.
 *		Each of these subplans is a sequential scan so ExecSeqScan() is
 *		called.  The slots returned by ExecSeqScan() may contain
 *		tuples which contain the attributes ExecNestLoop() uses to
 *		form the tuples it returns.
 *
 *	  * Eventually ExecSeqScan() stops returning tuples and the nest
 *		loop join ends.  Lastly, ExecEnd() calls ExecEndNode() which
 *		calls ExecEndNestLoop() which in turn calls ExecEndNode() on
 *		its subplans which result in ExecEndSeqScan().
 *
 *		This should show how the executor works by having
 *		ExecInitNode(), ExecProcNode() and ExecEndNode() dispatch
 *		their work to the appopriate node support routines which may
 *		in turn call these routines themselves on their subplans.
 *
 */
#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "executor/nodeAppend.h"
#include "executor/nodeGroup.h"
#include "executor/nodeHash.h"
#include "executor/nodeHashjoin.h"
#include "executor/nodeIndexscan.h"
#include "executor/nodeTidscan.h"
#include "executor/nodeLimit.h"
#include "executor/nodeMaterial.h"
#include "executor/nodeMergejoin.h"
#include "executor/nodeNestloop.h"
#include "executor/nodeResult.h"
#include "executor/nodeSeqscan.h"
#include "executor/nodeSetOp.h"
#include "executor/nodeSort.h"
#include "executor/nodeSubplan.h"
#include "executor/nodeSubqueryscan.h"
#include "executor/nodeUnique.h"
#include "miscadmin.h"
#include "tcop/tcopprot.h"

/* ------------------------------------------------------------------------
 *		ExecInitNode
 *
 *		Recursively initializes all the nodes in the plan rooted
 *		at 'node'.
 *
 *		Initial States:
 *		  'node' is the plan produced by the query planner
 *
 *		returns TRUE/FALSE on whether the plan was successfully initialized
 * ------------------------------------------------------------------------
 */
bool
ExecInitNode(Plan *node, EState *estate, Plan *parent)
{
	bool		result;
	List	   *subp;

	/*
	 * do nothing when we get to the end of a leaf on tree.
	 */
	if (node == NULL)
		return FALSE;

	foreach(subp, node->initPlan)
	{
		result = ExecInitSubPlan((SubPlan *) lfirst(subp), estate, node);
		if (result == FALSE)
			return FALSE;
	}

	switch (nodeTag(node))
	{

			/*
			 * control nodes
			 */
		case T_Result:
			result = ExecInitResult((Result *) node, estate, parent);
			break;

		case T_Append:
			result = ExecInitAppend((Append *) node, estate, parent);
			break;

			/*
			 * scan nodes
			 */
		case T_SeqScan:
			result = ExecInitSeqScan((SeqScan *) node, estate, parent);
			break;

		case T_IndexScan:
			result = ExecInitIndexScan((IndexScan *) node, estate, parent);
			break;

		case T_TidScan:
			result = ExecInitTidScan((TidScan *) node, estate, parent);
			break;

		case T_SubqueryScan:
			result = ExecInitSubqueryScan((SubqueryScan *) node, estate,
										  parent);
			break;

			/*
			 * join nodes
			 */
		case T_NestLoop:
			result = ExecInitNestLoop((NestLoop *) node, estate, parent);
			break;

		case T_MergeJoin:
			result = ExecInitMergeJoin((MergeJoin *) node, estate, parent);
			break;

		case T_Hash:
			result = ExecInitHash((Hash *) node, estate, parent);
			break;

		case T_HashJoin:
			result = ExecInitHashJoin((HashJoin *) node, estate, parent);
			break;

			/*
			 * materialization nodes
			 */
		case T_Material:
			result = ExecInitMaterial((Material *) node, estate, parent);
			break;

		case T_Sort:
			result = ExecInitSort((Sort *) node, estate, parent);
			break;

		case T_Unique:
			result = ExecInitUnique((Unique *) node, estate, parent);
			break;

		case T_SetOp:
			result = ExecInitSetOp((SetOp *) node, estate, parent);
			break;

		case T_Limit:
			result = ExecInitLimit((Limit *) node, estate, parent);
			break;

		case T_Group:
			result = ExecInitGroup((Group *) node, estate, parent);
			break;

		case T_Agg:
			result = ExecInitAgg((Agg *) node, estate, parent);
			break;

		default:
			elog(ERROR, "ExecInitNode: node type %d unsupported",
				 (int) nodeTag(node));
			result = FALSE;
	}

	if (result != FALSE)
	{
		foreach(subp, node->subPlan)
		{
			result = ExecInitSubPlan((SubPlan *) lfirst(subp), estate, node);
			if (result == FALSE)
				return FALSE;
		}
	}

	return result;
}


/* ----------------------------------------------------------------
 *		ExecProcNode
 *
 *		Initial States:
 *		  the query tree must be initialized once by calling ExecInit.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecProcNode(Plan *node, Plan *parent)
{
	TupleTableSlot *result;

	CHECK_FOR_INTERRUPTS();

	/*
	 * deal with NULL nodes..
	 */
	if (node == NULL)
		return NULL;

	if (node->chgParam != NULL) /* something changed */
		ExecReScan(node, NULL, parent); /* let ReScan handle this */

	switch (nodeTag(node))
	{

			/*
			 * control nodes
			 */
		case T_Result:
			result = ExecResult((Result *) node);
			break;

		case T_Append:
			result = ExecProcAppend((Append *) node);
			break;

			/*
			 * scan nodes
			 */
		case T_SeqScan:
			result = ExecSeqScan((SeqScan *) node);
			break;

		case T_IndexScan:
			result = ExecIndexScan((IndexScan *) node);
			break;

		case T_TidScan:
			result = ExecTidScan((TidScan *) node);
			break;

		case T_SubqueryScan:
			result = ExecSubqueryScan((SubqueryScan *) node);
			break;

			/*
			 * join nodes
			 */
		case T_NestLoop:
			result = ExecNestLoop((NestLoop *) node);
			break;

		case T_MergeJoin:
			result = ExecMergeJoin((MergeJoin *) node);
			break;

		case T_Hash:
			result = ExecHash((Hash *) node);
			break;

		case T_HashJoin:
			result = ExecHashJoin((HashJoin *) node);
			break;

			/*
			 * materialization nodes
			 */
		case T_Material:
			result = ExecMaterial((Material *) node);
			break;

		case T_Sort:
			result = ExecSort((Sort *) node);
			break;

		case T_Unique:
			result = ExecUnique((Unique *) node);
			break;

		case T_SetOp:
			result = ExecSetOp((SetOp *) node);
			break;

		case T_Limit:
			result = ExecLimit((Limit *) node);
			break;

		case T_Group:
			result = ExecGroup((Group *) node);
			break;

		case T_Agg:
			result = ExecAgg((Agg *) node);
			break;

		default:
			elog(ERROR, "ExecProcNode: node type %d unsupported",
				 (int) nodeTag(node));
			result = NULL;
	}

	return result;
}

int
ExecCountSlotsNode(Plan *node)
{
	if (node == (Plan *) NULL)
		return 0;

	switch (nodeTag(node))
	{

			/*
			 * control nodes
			 */
		case T_Result:
			return ExecCountSlotsResult((Result *) node);

		case T_Append:
			return ExecCountSlotsAppend((Append *) node);

			/*
			 * scan nodes
			 */
		case T_SeqScan:
			return ExecCountSlotsSeqScan((SeqScan *) node);

		case T_IndexScan:
			return ExecCountSlotsIndexScan((IndexScan *) node);

		case T_TidScan:
			return ExecCountSlotsTidScan((TidScan *) node);

		case T_SubqueryScan:
			return ExecCountSlotsSubqueryScan((SubqueryScan *) node);

			/*
			 * join nodes
			 */
		case T_NestLoop:
			return ExecCountSlotsNestLoop((NestLoop *) node);

		case T_MergeJoin:
			return ExecCountSlotsMergeJoin((MergeJoin *) node);

		case T_Hash:
			return ExecCountSlotsHash((Hash *) node);

		case T_HashJoin:
			return ExecCountSlotsHashJoin((HashJoin *) node);

			/*
			 * materialization nodes
			 */
		case T_Material:
			return ExecCountSlotsMaterial((Material *) node);

		case T_Sort:
			return ExecCountSlotsSort((Sort *) node);

		case T_Unique:
			return ExecCountSlotsUnique((Unique *) node);

		case T_SetOp:
			return ExecCountSlotsSetOp((SetOp *) node);

		case T_Limit:
			return ExecCountSlotsLimit((Limit *) node);

		case T_Group:
			return ExecCountSlotsGroup((Group *) node);

		case T_Agg:
			return ExecCountSlotsAgg((Agg *) node);

		default:
			elog(ERROR, "ExecCountSlotsNode: node type %d unsupported",
				 (int) nodeTag(node));
			break;
	}
	return 0;
}

/* ----------------------------------------------------------------
 *		ExecEndNode
 *
 *		Recursively cleans up all the nodes in the plan rooted
 *		at 'node'.
 *
 *		After this operation, the query plan will not be able to
 *		processed any further.	This should be called only after
 *		the query plan has been fully executed.
 * ----------------------------------------------------------------
 */
void
ExecEndNode(Plan *node, Plan *parent)
{
	List	   *subp;

	/*
	 * do nothing when we get to the end of a leaf on tree.
	 */
	if (node == NULL)
		return;

	foreach(subp, node->initPlan)
		ExecEndSubPlan((SubPlan *) lfirst(subp));
	foreach(subp, node->subPlan)
		ExecEndSubPlan((SubPlan *) lfirst(subp));
	if (node->chgParam != NULL)
	{
		freeList(node->chgParam);
		node->chgParam = NULL;
	}

	switch (nodeTag(node))
	{

			/*
			 * control nodes
			 */
		case T_Result:
			ExecEndResult((Result *) node);
			break;

		case T_Append:
			ExecEndAppend((Append *) node);
			break;

			/*
			 * scan nodes
			 */
		case T_SeqScan:
			ExecEndSeqScan((SeqScan *) node);
			break;

		case T_IndexScan:
			ExecEndIndexScan((IndexScan *) node);
			break;

		case T_TidScan:
			ExecEndTidScan((TidScan *) node);
			break;

		case T_SubqueryScan:
			ExecEndSubqueryScan((SubqueryScan *) node);
			break;

			/*
			 * join nodes
			 */
		case T_NestLoop:
			ExecEndNestLoop((NestLoop *) node);
			break;

		case T_MergeJoin:
			ExecEndMergeJoin((MergeJoin *) node);
			break;

		case T_Hash:
			ExecEndHash((Hash *) node);
			break;

		case T_HashJoin:
			ExecEndHashJoin((HashJoin *) node);
			break;

			/*
			 * materialization nodes
			 */
		case T_Material:
			ExecEndMaterial((Material *) node);
			break;

		case T_Sort:
			ExecEndSort((Sort *) node);
			break;

		case T_Unique:
			ExecEndUnique((Unique *) node);
			break;

		case T_SetOp:
			ExecEndSetOp((SetOp *) node);
			break;

		case T_Limit:
			ExecEndLimit((Limit *) node);
			break;

		case T_Group:
			ExecEndGroup((Group *) node);
			break;

		case T_Agg:
			ExecEndAgg((Agg *) node);
			break;

		default:
			elog(ERROR, "ExecEndNode: node type %d unsupported",
				 (int) nodeTag(node));
			break;
	}
}


/* ----------------------------------------------------------------
 *		ExecGetTupType
 *
 *		this gives you the tuple descriptor for tuples returned
 *		by this node.  I really wish I could ditch this routine,
 *		but since not all nodes store their type info in the same
 *		place, we have to do something special for each node type.
 *
 * ----------------------------------------------------------------
 */
TupleDesc
ExecGetTupType(Plan *node)
{
	TupleTableSlot *slot;

	if (node == NULL)
		return NULL;

	switch (nodeTag(node))
	{
		case T_Result:
			{
				ResultState *resstate = ((Result *) node)->resstate;

				slot = resstate->cstate.cs_ResultTupleSlot;
			}
			break;

		case T_SeqScan:
			{
				CommonScanState *scanstate = ((SeqScan *) node)->scanstate;

				slot = scanstate->cstate.cs_ResultTupleSlot;
			}
			break;

		case T_NestLoop:
			{
				NestLoopState *nlstate = ((NestLoop *) node)->nlstate;

				slot = nlstate->jstate.cs_ResultTupleSlot;
			}
			break;

		case T_Append:
			{
				AppendState *appendstate = ((Append *) node)->appendstate;

				slot = appendstate->cstate.cs_ResultTupleSlot;
			}
			break;

		case T_IndexScan:
			{
				CommonScanState *scanstate = ((IndexScan *) node)->scan.scanstate;

				slot = scanstate->cstate.cs_ResultTupleSlot;
			}
			break;

		case T_TidScan:
			{
				CommonScanState *scanstate = ((TidScan *) node)->scan.scanstate;

				slot = scanstate->cstate.cs_ResultTupleSlot;
			}
			break;

		case T_SubqueryScan:
			{
				CommonScanState *scanstate = ((SubqueryScan *) node)->scan.scanstate;

				slot = scanstate->cstate.cs_ResultTupleSlot;
			}
			break;

		case T_Material:
			{
				MaterialState *matstate = ((Material *) node)->matstate;

				slot = matstate->csstate.css_ScanTupleSlot;
			}
			break;

		case T_Sort:
			{
				SortState  *sortstate = ((Sort *) node)->sortstate;

				slot = sortstate->csstate.css_ScanTupleSlot;
			}
			break;

		case T_Agg:
			{
				AggState   *aggstate = ((Agg *) node)->aggstate;

				slot = aggstate->csstate.cstate.cs_ResultTupleSlot;
			}
			break;

		case T_Group:
			{
				GroupState *grpstate = ((Group *) node)->grpstate;

				slot = grpstate->csstate.cstate.cs_ResultTupleSlot;
			}
			break;

		case T_Hash:
			{
				HashState  *hashstate = ((Hash *) node)->hashstate;

				slot = hashstate->cstate.cs_ResultTupleSlot;
			}
			break;

		case T_Unique:
			{
				UniqueState *uniquestate = ((Unique *) node)->uniquestate;

				slot = uniquestate->cstate.cs_ResultTupleSlot;
			}
			break;

		case T_SetOp:
			{
				SetOpState *setopstate = ((SetOp *) node)->setopstate;

				slot = setopstate->cstate.cs_ResultTupleSlot;
			}
			break;

		case T_Limit:
			{
				LimitState *limitstate = ((Limit *) node)->limitstate;

				slot = limitstate->cstate.cs_ResultTupleSlot;
			}
			break;

		case T_MergeJoin:
			{
				MergeJoinState *mergestate = ((MergeJoin *) node)->mergestate;

				slot = mergestate->jstate.cs_ResultTupleSlot;
			}
			break;

		case T_HashJoin:
			{
				HashJoinState *hashjoinstate = ((HashJoin *) node)->hashjoinstate;

				slot = hashjoinstate->jstate.cs_ResultTupleSlot;
			}
			break;

		default:

			/*
			 * should never get here
			 */
			elog(ERROR, "ExecGetTupType: node type %d unsupported",
				 (int) nodeTag(node));
			return NULL;
	}

	return slot->ttc_tupleDescriptor;
}
