/*-------------------------------------------------------------------------
 *
 * execProcnode.c--
 *	 contains dispatch functions which call the appropriate "initialize",
 *	 "get a tuple", and "cleanup" routines for the given node type.
 *	 If the node has children, then it will presumably call ExecInitNode,
 *	 ExecProcNode, or ExecEndNode on it's subnodes and do the appropriate
 *	 processing..
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/execProcnode.c,v 1.4 1997/09/08 02:22:30 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 INTERFACE ROUTINES
 *		ExecInitNode	-		initialize a plan node and it's subplans
 *		ExecProcNode	-		get a tuple by executing the plan node
 *		ExecEndNode		-		shut down a plan node and it's subplans
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
#include "executor/nodeResult.h"
#include "executor/nodeAppend.h"
#include "executor/nodeSeqscan.h"
#include "executor/nodeIndexscan.h"
#include "executor/nodeNestloop.h"
#include "executor/nodeMergejoin.h"
#include "executor/nodeMaterial.h"
#include "executor/nodeSort.h"
#include "executor/nodeUnique.h"
#include "executor/nodeGroup.h"
#include "executor/nodeAgg.h"
#include "executor/nodeHash.h"
#include "executor/nodeHashjoin.h"
#include "executor/nodeTee.h"

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
ExecInitNode(Plan * node, EState * estate, Plan * parent)
{
	bool		result;

	/* ----------------
	 *	do nothing when we get to the end
	 *	of a leaf on tree.
	 * ----------------
	 */
	if (node == NULL)
		return FALSE;

	switch (nodeTag(node))
	{
			/* ----------------
			 *		control nodes
			 * ----------------
			 */
		case T_Result:
			result = ExecInitResult((Result *) node, estate, parent);
			break;

		case T_Append:
			result = ExecInitAppend((Append *) node, estate, parent);
			break;

			/* ----------------
			 *		scan nodes
			 * ----------------
			 */
		case T_SeqScan:
			result = ExecInitSeqScan((SeqScan *) node, estate, parent);
			break;

		case T_IndexScan:
			result = ExecInitIndexScan((IndexScan *) node, estate, parent);
			break;

			/* ----------------
			 *		join nodes
			 * ----------------
			 */
		case T_NestLoop:
			result = ExecInitNestLoop((NestLoop *) node, estate, parent);
			break;

		case T_MergeJoin:
			result = ExecInitMergeJoin((MergeJoin *) node, estate, parent);
			break;

			/* ----------------
			 *		materialization nodes
			 * ----------------
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

		case T_Group:
			result = ExecInitGroup((Group *) node, estate, parent);
			break;

		case T_Agg:
			result = ExecInitAgg((Agg *) node, estate, parent);
			break;

		case T_Hash:
			result = ExecInitHash((Hash *) node, estate, parent);
			break;

		case T_HashJoin:
			result = ExecInitHashJoin((HashJoin *) node, estate, parent);
			break;

		case T_Tee:
			result = ExecInitTee((Tee *) node, estate, parent);
			break;

		default:
			elog(DEBUG, "ExecInitNode: node not yet supported: %d",
				 nodeTag(node));
			result = FALSE;
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
ExecProcNode(Plan * node, Plan * parent)
{
	TupleTableSlot *result;

	/* ----------------
	 *	deal with NULL nodes..
	 * ----------------
	 */
	if (node == NULL)
		return NULL;

	switch (nodeTag(node))
	{
			/* ----------------
			 *		control nodes
			 * ----------------
			 */
		case T_Result:
			result = ExecResult((Result *) node);
			break;

		case T_Append:
			result = ExecProcAppend((Append *) node);
			break;

			/* ----------------
			 *		scan nodes
			 * ----------------
			 */
		case T_SeqScan:
			result = ExecSeqScan((SeqScan *) node);
			break;

		case T_IndexScan:
			result = ExecIndexScan((IndexScan *) node);
			break;

			/* ----------------
			 *		join nodes
			 * ----------------
			 */
		case T_NestLoop:
			result = ExecNestLoop((NestLoop *) node, parent);
			break;

		case T_MergeJoin:
			result = ExecMergeJoin((MergeJoin *) node);
			break;

			/* ----------------
			 *		materialization nodes
			 * ----------------
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

		case T_Group:
			result = ExecGroup((Group *) node);
			break;

		case T_Agg:
			result = ExecAgg((Agg *) node);
			break;

		case T_Hash:
			result = ExecHash((Hash *) node);
			break;

		case T_HashJoin:
			result = ExecHashJoin((HashJoin *) node);
			break;

		case T_Tee:
			result = ExecTee((Tee *) node, parent);
			break;

		default:
			elog(DEBUG, "ExecProcNode: node not yet supported: %d",
				 nodeTag(node));
			result = FALSE;
	}

	return result;
}

int
ExecCountSlotsNode(Plan * node)
{
	if (node == (Plan *) NULL)
		return 0;

	switch (nodeTag(node))
	{
			/* ----------------
			 *		control nodes
			 * ----------------
			 */
		case T_Result:
			return ExecCountSlotsResult((Result *) node);

		case T_Append:
			return ExecCountSlotsAppend((Append *) node);

			/* ----------------
			 *		scan nodes
			 * ----------------
			 */
		case T_SeqScan:
			return ExecCountSlotsSeqScan((SeqScan *) node);

		case T_IndexScan:
			return ExecCountSlotsIndexScan((IndexScan *) node);

			/* ----------------
			 *		join nodes
			 * ----------------
			 */
		case T_NestLoop:
			return ExecCountSlotsNestLoop((NestLoop *) node);

		case T_MergeJoin:
			return ExecCountSlotsMergeJoin((MergeJoin *) node);

			/* ----------------
			 *		materialization nodes
			 * ----------------
			 */
		case T_Material:
			return ExecCountSlotsMaterial((Material *) node);

		case T_Sort:
			return ExecCountSlotsSort((Sort *) node);

		case T_Unique:
			return ExecCountSlotsUnique((Unique *) node);

		case T_Group:
			return ExecCountSlotsGroup((Group *) node);

		case T_Agg:
			return ExecCountSlotsAgg((Agg *) node);

		case T_Hash:
			return ExecCountSlotsHash((Hash *) node);

		case T_HashJoin:
			return ExecCountSlotsHashJoin((HashJoin *) node);

		case T_Tee:
			return ExecCountSlotsTee((Tee *) node);

		default:
			elog(WARN, "ExecCountSlotsNode: node not yet supported: %d",
				 nodeTag(node));
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
ExecEndNode(Plan * node, Plan * parent)
{
	/* ----------------
	 *	do nothing when we get to the end
	 *	of a leaf on tree.
	 * ----------------
	 */
	if (node == NULL)
		return;

	switch (nodeTag(node))
	{
			/* ----------------
			 *	control nodes
			 * ----------------
			 */
		case T_Result:
			ExecEndResult((Result *) node);
			break;

		case T_Append:
			ExecEndAppend((Append *) node);
			break;

			/* ----------------
			 *		scan nodes
			 * ----------------
			 */
		case T_SeqScan:
			ExecEndSeqScan((SeqScan *) node);
			break;

		case T_IndexScan:
			ExecEndIndexScan((IndexScan *) node);
			break;

			/* ----------------
			 *		join nodes
			 * ----------------
			 */
		case T_NestLoop:
			ExecEndNestLoop((NestLoop *) node);
			break;

		case T_MergeJoin:
			ExecEndMergeJoin((MergeJoin *) node);
			break;

			/* ----------------
			 *		materialization nodes
			 * ----------------
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

		case T_Group:
			ExecEndGroup((Group *) node);
			break;

		case T_Agg:
			ExecEndAgg((Agg *) node);
			break;

			/* ----------------
			 *		XXX add hooks to these
			 * ----------------
			 */
		case T_Hash:
			ExecEndHash((Hash *) node);
			break;

		case T_HashJoin:
			ExecEndHashJoin((HashJoin *) node);
			break;

		case T_Tee:
			ExecEndTee((Tee *) node, parent);
			break;

		default:
			elog(DEBUG, "ExecEndNode: node not yet supported",
				 nodeTag(node));
			break;
	}
}
