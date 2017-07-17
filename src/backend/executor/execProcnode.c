/*-------------------------------------------------------------------------
 *
 * execProcnode.c
 *	 contains dispatch functions which call the appropriate "initialize",
 *	 "get a tuple", and "cleanup" routines for the given node type.
 *	 If the node has children, then it will presumably call ExecInitNode,
 *	 ExecProcNode, or ExecEndNode on its subnodes and do the appropriate
 *	 processing.
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execProcnode.c
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 NOTES
 *		This used to be three files.  It is now all combined into
 *		one file so that it is easier to keep the dispatch routines
 *		in sync when new nodes are added.
 *
 *	 EXAMPLE
 *		Suppose we want the age of the manager of the shoe department and
 *		the number of employees in that department.  So we have the query:
 *
 *				select DEPT.no_emps, EMP.age
 *				from DEPT, EMP
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
 *		ExecutorStart() is called first.
 *		It calls InitPlan() which calls ExecInitNode() on
 *		the root of the plan -- the nest loop node.
 *
 *	  * ExecInitNode() notices that it is looking at a nest loop and
 *		as the code below demonstrates, it calls ExecInitNestLoop().
 *		Eventually this calls ExecInitNode() on the right and left subplans
 *		and so forth until the entire plan is initialized.  The result
 *		of ExecInitNode() is a plan state tree built with the same structure
 *		as the underlying plan tree.
 *
 *	  * Then when ExecutorRun() is called, it calls ExecutePlan() which calls
 *		ExecProcNode() repeatedly on the top node of the plan state tree.
 *		Each time this happens, ExecProcNode() will end up calling
 *		ExecNestLoop(), which calls ExecProcNode() on its subplans.
 *		Each of these subplans is a sequential scan so ExecSeqScan() is
 *		called.  The slots returned by ExecSeqScan() may contain
 *		tuples which contain the attributes ExecNestLoop() uses to
 *		form the tuples it returns.
 *
 *	  * Eventually ExecSeqScan() stops returning tuples and the nest
 *		loop join ends.  Lastly, ExecutorEnd() calls ExecEndNode() which
 *		calls ExecEndNestLoop() which in turn calls ExecEndNode() on
 *		its subplans which result in ExecEndSeqScan().
 *
 *		This should show how the executor works by having
 *		ExecInitNode(), ExecProcNode() and ExecEndNode() dispatch
 *		their work to the appropriate node support routines which may
 *		in turn call these routines themselves on their subplans.
 */
#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "executor/nodeAppend.h"
#include "executor/nodeBitmapAnd.h"
#include "executor/nodeBitmapHeapscan.h"
#include "executor/nodeBitmapIndexscan.h"
#include "executor/nodeBitmapOr.h"
#include "executor/nodeCtescan.h"
#include "executor/nodeCustom.h"
#include "executor/nodeForeignscan.h"
#include "executor/nodeFunctionscan.h"
#include "executor/nodeGather.h"
#include "executor/nodeGatherMerge.h"
#include "executor/nodeGroup.h"
#include "executor/nodeHash.h"
#include "executor/nodeHashjoin.h"
#include "executor/nodeIndexonlyscan.h"
#include "executor/nodeIndexscan.h"
#include "executor/nodeLimit.h"
#include "executor/nodeLockRows.h"
#include "executor/nodeMaterial.h"
#include "executor/nodeMergeAppend.h"
#include "executor/nodeMergejoin.h"
#include "executor/nodeModifyTable.h"
#include "executor/nodeNamedtuplestorescan.h"
#include "executor/nodeNestloop.h"
#include "executor/nodeProjectSet.h"
#include "executor/nodeRecursiveunion.h"
#include "executor/nodeResult.h"
#include "executor/nodeSamplescan.h"
#include "executor/nodeSeqscan.h"
#include "executor/nodeSetOp.h"
#include "executor/nodeSort.h"
#include "executor/nodeSubplan.h"
#include "executor/nodeSubqueryscan.h"
#include "executor/nodeTableFuncscan.h"
#include "executor/nodeTidscan.h"
#include "executor/nodeUnique.h"
#include "executor/nodeValuesscan.h"
#include "executor/nodeWindowAgg.h"
#include "executor/nodeWorktablescan.h"
#include "nodes/nodeFuncs.h"
#include "miscadmin.h"


static TupleTableSlot *ExecProcNodeFirst(PlanState *node);
static TupleTableSlot *ExecProcNodeInstr(PlanState *node);


/* ------------------------------------------------------------------------
 *		ExecInitNode
 *
 *		Recursively initializes all the nodes in the plan tree rooted
 *		at 'node'.
 *
 *		Inputs:
 *		  'node' is the current node of the plan produced by the query planner
 *		  'estate' is the shared execution state for the plan tree
 *		  'eflags' is a bitwise OR of flag bits described in executor.h
 *
 *		Returns a PlanState node corresponding to the given Plan node.
 * ------------------------------------------------------------------------
 */
PlanState *
ExecInitNode(Plan *node, EState *estate, int eflags)
{
	PlanState  *result;
	List	   *subps;
	ListCell   *l;

	/*
	 * do nothing when we get to the end of a leaf on tree.
	 */
	if (node == NULL)
		return NULL;

	/*
	 * Make sure there's enough stack available. Need to check here, in
	 * addition to ExecProcNode() (via ExecProcNodeFirst()), to ensure the
	 * stack isn't overrun while initializing the node tree.
	 */
	check_stack_depth();

	switch (nodeTag(node))
	{
			/*
			 * control nodes
			 */
		case T_Result:
			result = (PlanState *) ExecInitResult((Result *) node,
												  estate, eflags);
			break;

		case T_ProjectSet:
			result = (PlanState *) ExecInitProjectSet((ProjectSet *) node,
													  estate, eflags);
			break;

		case T_ModifyTable:
			result = (PlanState *) ExecInitModifyTable((ModifyTable *) node,
													   estate, eflags);
			break;

		case T_Append:
			result = (PlanState *) ExecInitAppend((Append *) node,
												  estate, eflags);
			break;

		case T_MergeAppend:
			result = (PlanState *) ExecInitMergeAppend((MergeAppend *) node,
													   estate, eflags);
			break;

		case T_RecursiveUnion:
			result = (PlanState *) ExecInitRecursiveUnion((RecursiveUnion *) node,
														  estate, eflags);
			break;

		case T_BitmapAnd:
			result = (PlanState *) ExecInitBitmapAnd((BitmapAnd *) node,
													 estate, eflags);
			break;

		case T_BitmapOr:
			result = (PlanState *) ExecInitBitmapOr((BitmapOr *) node,
													estate, eflags);
			break;

			/*
			 * scan nodes
			 */
		case T_SeqScan:
			result = (PlanState *) ExecInitSeqScan((SeqScan *) node,
												   estate, eflags);
			break;

		case T_SampleScan:
			result = (PlanState *) ExecInitSampleScan((SampleScan *) node,
													  estate, eflags);
			break;

		case T_IndexScan:
			result = (PlanState *) ExecInitIndexScan((IndexScan *) node,
													 estate, eflags);
			break;

		case T_IndexOnlyScan:
			result = (PlanState *) ExecInitIndexOnlyScan((IndexOnlyScan *) node,
														 estate, eflags);
			break;

		case T_BitmapIndexScan:
			result = (PlanState *) ExecInitBitmapIndexScan((BitmapIndexScan *) node,
														   estate, eflags);
			break;

		case T_BitmapHeapScan:
			result = (PlanState *) ExecInitBitmapHeapScan((BitmapHeapScan *) node,
														  estate, eflags);
			break;

		case T_TidScan:
			result = (PlanState *) ExecInitTidScan((TidScan *) node,
												   estate, eflags);
			break;

		case T_SubqueryScan:
			result = (PlanState *) ExecInitSubqueryScan((SubqueryScan *) node,
														estate, eflags);
			break;

		case T_FunctionScan:
			result = (PlanState *) ExecInitFunctionScan((FunctionScan *) node,
														estate, eflags);
			break;

		case T_TableFuncScan:
			result = (PlanState *) ExecInitTableFuncScan((TableFuncScan *) node,
														 estate, eflags);
			break;

		case T_ValuesScan:
			result = (PlanState *) ExecInitValuesScan((ValuesScan *) node,
													  estate, eflags);
			break;

		case T_CteScan:
			result = (PlanState *) ExecInitCteScan((CteScan *) node,
												   estate, eflags);
			break;

		case T_NamedTuplestoreScan:
			result = (PlanState *) ExecInitNamedTuplestoreScan((NamedTuplestoreScan *) node,
															   estate, eflags);
			break;

		case T_WorkTableScan:
			result = (PlanState *) ExecInitWorkTableScan((WorkTableScan *) node,
														 estate, eflags);
			break;

		case T_ForeignScan:
			result = (PlanState *) ExecInitForeignScan((ForeignScan *) node,
													   estate, eflags);
			break;

		case T_CustomScan:
			result = (PlanState *) ExecInitCustomScan((CustomScan *) node,
													  estate, eflags);
			break;

			/*
			 * join nodes
			 */
		case T_NestLoop:
			result = (PlanState *) ExecInitNestLoop((NestLoop *) node,
													estate, eflags);
			break;

		case T_MergeJoin:
			result = (PlanState *) ExecInitMergeJoin((MergeJoin *) node,
													 estate, eflags);
			break;

		case T_HashJoin:
			result = (PlanState *) ExecInitHashJoin((HashJoin *) node,
													estate, eflags);
			break;

			/*
			 * materialization nodes
			 */
		case T_Material:
			result = (PlanState *) ExecInitMaterial((Material *) node,
													estate, eflags);
			break;

		case T_Sort:
			result = (PlanState *) ExecInitSort((Sort *) node,
												estate, eflags);
			break;

		case T_Group:
			result = (PlanState *) ExecInitGroup((Group *) node,
												 estate, eflags);
			break;

		case T_Agg:
			result = (PlanState *) ExecInitAgg((Agg *) node,
											   estate, eflags);
			break;

		case T_WindowAgg:
			result = (PlanState *) ExecInitWindowAgg((WindowAgg *) node,
													 estate, eflags);
			break;

		case T_Unique:
			result = (PlanState *) ExecInitUnique((Unique *) node,
												  estate, eflags);
			break;

		case T_Gather:
			result = (PlanState *) ExecInitGather((Gather *) node,
												  estate, eflags);
			break;

		case T_GatherMerge:
			result = (PlanState *) ExecInitGatherMerge((GatherMerge *) node,
													   estate, eflags);
			break;

		case T_Hash:
			result = (PlanState *) ExecInitHash((Hash *) node,
												estate, eflags);
			break;

		case T_SetOp:
			result = (PlanState *) ExecInitSetOp((SetOp *) node,
												 estate, eflags);
			break;

		case T_LockRows:
			result = (PlanState *) ExecInitLockRows((LockRows *) node,
													estate, eflags);
			break;

		case T_Limit:
			result = (PlanState *) ExecInitLimit((Limit *) node,
												 estate, eflags);
			break;

		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
			result = NULL;		/* keep compiler quiet */
			break;
	}

	/*
	 * Add a wrapper around the ExecProcNode callback that checks stack depth
	 * during the first execution.
	 */
	result->ExecProcNodeReal = result->ExecProcNode;
	result->ExecProcNode = ExecProcNodeFirst;

	/*
	 * Initialize any initPlans present in this node.  The planner put them in
	 * a separate list for us.
	 */
	subps = NIL;
	foreach(l, node->initPlan)
	{
		SubPlan    *subplan = (SubPlan *) lfirst(l);
		SubPlanState *sstate;

		Assert(IsA(subplan, SubPlan));
		sstate = ExecInitSubPlan(subplan, result);
		subps = lappend(subps, sstate);
	}
	result->initPlan = subps;

	/* Set up instrumentation for this node if requested */
	if (estate->es_instrument)
		result->instrument = InstrAlloc(1, estate->es_instrument);

	return result;
}


/*
 * ExecProcNode wrapper that performs some one-time checks, before calling
 * the relevant node method (possibly via an instrumentation wrapper).
 */
static TupleTableSlot *
ExecProcNodeFirst(PlanState *node)
{
	/*
	 * Perform stack depth check during the first execution of the node.  We
	 * only do so the first time round because it turns out to not be cheap on
	 * some common architectures (eg. x86).  This relies on the assumption that
	 * ExecProcNode calls for a given plan node will always be made at roughly
	 * the same stack depth.
	 */
	check_stack_depth();

	/*
	 * If instrumentation is required, change the wrapper to one that just
	 * does instrumentation.  Otherwise we can dispense with all wrappers and
	 * have ExecProcNode() directly call the relevant function from now on.
	 */
	if (node->instrument)
		node->ExecProcNode = ExecProcNodeInstr;
	else
		node->ExecProcNode = node->ExecProcNodeReal;

	return node->ExecProcNode(node);
}


/*
 * ExecProcNode wrapper that performs instrumentation calls.  By keeping
 * this a separate function, we avoid overhead in the normal case where
 * no instrumentation is wanted.
 */
static TupleTableSlot *
ExecProcNodeInstr(PlanState *node)
{
	TupleTableSlot *result;

	InstrStartNode(node->instrument);

	result = node->ExecProcNodeReal(node);

	InstrStopNode(node->instrument, TupIsNull(result) ? 0.0 : 1.0);

	return result;
}


/* ----------------------------------------------------------------
 *		MultiExecProcNode
 *
 *		Execute a node that doesn't return individual tuples
 *		(it might return a hashtable, bitmap, etc).  Caller should
 *		check it got back the expected kind of Node.
 *
 * This has essentially the same responsibilities as ExecProcNode,
 * but it does not do InstrStartNode/InstrStopNode (mainly because
 * it can't tell how many returned tuples to count).  Each per-node
 * function must provide its own instrumentation support.
 * ----------------------------------------------------------------
 */
Node *
MultiExecProcNode(PlanState *node)
{
	Node	   *result;

	check_stack_depth();

	CHECK_FOR_INTERRUPTS();

	if (node->chgParam != NULL) /* something changed */
		ExecReScan(node);		/* let ReScan handle this */

	switch (nodeTag(node))
	{
			/*
			 * Only node types that actually support multiexec will be listed
			 */

		case T_HashState:
			result = MultiExecHash((HashState *) node);
			break;

		case T_BitmapIndexScanState:
			result = MultiExecBitmapIndexScan((BitmapIndexScanState *) node);
			break;

		case T_BitmapAndState:
			result = MultiExecBitmapAnd((BitmapAndState *) node);
			break;

		case T_BitmapOrState:
			result = MultiExecBitmapOr((BitmapOrState *) node);
			break;

		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
			result = NULL;
			break;
	}

	return result;
}


/* ----------------------------------------------------------------
 *		ExecEndNode
 *
 *		Recursively cleans up all the nodes in the plan rooted
 *		at 'node'.
 *
 *		After this operation, the query plan will not be able to be
 *		processed any further.  This should be called only after
 *		the query plan has been fully executed.
 * ----------------------------------------------------------------
 */
void
ExecEndNode(PlanState *node)
{
	/*
	 * do nothing when we get to the end of a leaf on tree.
	 */
	if (node == NULL)
		return;

	/*
	 * Make sure there's enough stack available. Need to check here, in
	 * addition to ExecProcNode() (via ExecProcNodeFirst()), because it's not
	 * guaranteed that ExecProcNode() is reached for all nodes.
	 */
	check_stack_depth();

	if (node->chgParam != NULL)
	{
		bms_free(node->chgParam);
		node->chgParam = NULL;
	}

	switch (nodeTag(node))
	{
			/*
			 * control nodes
			 */
		case T_ResultState:
			ExecEndResult((ResultState *) node);
			break;

		case T_ProjectSetState:
			ExecEndProjectSet((ProjectSetState *) node);
			break;

		case T_ModifyTableState:
			ExecEndModifyTable((ModifyTableState *) node);
			break;

		case T_AppendState:
			ExecEndAppend((AppendState *) node);
			break;

		case T_MergeAppendState:
			ExecEndMergeAppend((MergeAppendState *) node);
			break;

		case T_RecursiveUnionState:
			ExecEndRecursiveUnion((RecursiveUnionState *) node);
			break;

		case T_BitmapAndState:
			ExecEndBitmapAnd((BitmapAndState *) node);
			break;

		case T_BitmapOrState:
			ExecEndBitmapOr((BitmapOrState *) node);
			break;

			/*
			 * scan nodes
			 */
		case T_SeqScanState:
			ExecEndSeqScan((SeqScanState *) node);
			break;

		case T_SampleScanState:
			ExecEndSampleScan((SampleScanState *) node);
			break;

		case T_GatherState:
			ExecEndGather((GatherState *) node);
			break;

		case T_GatherMergeState:
			ExecEndGatherMerge((GatherMergeState *) node);
			break;

		case T_IndexScanState:
			ExecEndIndexScan((IndexScanState *) node);
			break;

		case T_IndexOnlyScanState:
			ExecEndIndexOnlyScan((IndexOnlyScanState *) node);
			break;

		case T_BitmapIndexScanState:
			ExecEndBitmapIndexScan((BitmapIndexScanState *) node);
			break;

		case T_BitmapHeapScanState:
			ExecEndBitmapHeapScan((BitmapHeapScanState *) node);
			break;

		case T_TidScanState:
			ExecEndTidScan((TidScanState *) node);
			break;

		case T_SubqueryScanState:
			ExecEndSubqueryScan((SubqueryScanState *) node);
			break;

		case T_FunctionScanState:
			ExecEndFunctionScan((FunctionScanState *) node);
			break;

		case T_TableFuncScanState:
			ExecEndTableFuncScan((TableFuncScanState *) node);
			break;

		case T_ValuesScanState:
			ExecEndValuesScan((ValuesScanState *) node);
			break;

		case T_CteScanState:
			ExecEndCteScan((CteScanState *) node);
			break;

		case T_NamedTuplestoreScanState:
			ExecEndNamedTuplestoreScan((NamedTuplestoreScanState *) node);
			break;

		case T_WorkTableScanState:
			ExecEndWorkTableScan((WorkTableScanState *) node);
			break;

		case T_ForeignScanState:
			ExecEndForeignScan((ForeignScanState *) node);
			break;

		case T_CustomScanState:
			ExecEndCustomScan((CustomScanState *) node);
			break;

			/*
			 * join nodes
			 */
		case T_NestLoopState:
			ExecEndNestLoop((NestLoopState *) node);
			break;

		case T_MergeJoinState:
			ExecEndMergeJoin((MergeJoinState *) node);
			break;

		case T_HashJoinState:
			ExecEndHashJoin((HashJoinState *) node);
			break;

			/*
			 * materialization nodes
			 */
		case T_MaterialState:
			ExecEndMaterial((MaterialState *) node);
			break;

		case T_SortState:
			ExecEndSort((SortState *) node);
			break;

		case T_GroupState:
			ExecEndGroup((GroupState *) node);
			break;

		case T_AggState:
			ExecEndAgg((AggState *) node);
			break;

		case T_WindowAggState:
			ExecEndWindowAgg((WindowAggState *) node);
			break;

		case T_UniqueState:
			ExecEndUnique((UniqueState *) node);
			break;

		case T_HashState:
			ExecEndHash((HashState *) node);
			break;

		case T_SetOpState:
			ExecEndSetOp((SetOpState *) node);
			break;

		case T_LockRowsState:
			ExecEndLockRows((LockRowsState *) node);
			break;

		case T_LimitState:
			ExecEndLimit((LimitState *) node);
			break;

		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
			break;
	}
}

/*
 * ExecShutdownNode
 *
 * Give execution nodes a chance to stop asynchronous resource consumption
 * and release any resources still held.  Currently, this is only used for
 * parallel query, but we might want to extend it to other cases also (e.g.
 * FDW).  We might also want to call it sooner, as soon as it's evident that
 * no more rows will be needed (e.g. when a Limit is filled) rather than only
 * at the end of ExecutorRun.
 */
bool
ExecShutdownNode(PlanState *node)
{
	if (node == NULL)
		return false;

	check_stack_depth();

	planstate_tree_walker(node, ExecShutdownNode, NULL);

	switch (nodeTag(node))
	{
		case T_GatherState:
			ExecShutdownGather((GatherState *) node);
			break;
		case T_ForeignScanState:
			ExecShutdownForeignScan((ForeignScanState *) node);
			break;
		case T_CustomScanState:
			ExecShutdownCustomScan((CustomScanState *) node);
			break;
		case T_GatherMergeState:
			ExecShutdownGatherMerge((GatherMergeState *) node);
			break;
		default:
			break;
	}

	return false;
}
