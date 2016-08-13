/*-------------------------------------------------------------------------
 *
 * execAmi.c
 *	  miscellaneous executor access method routines
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	src/backend/executor/execAmi.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/htup_details.h"
#include "executor/execdebug.h"
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
#include "executor/nodeGroup.h"
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
#include "executor/nodeNestloop.h"
#include "executor/nodeRecursiveunion.h"
#include "executor/nodeResult.h"
#include "executor/nodeSamplescan.h"
#include "executor/nodeSeqscan.h"
#include "executor/nodeSetOp.h"
#include "executor/nodeSort.h"
#include "executor/nodeSubplan.h"
#include "executor/nodeSubqueryscan.h"
#include "executor/nodeTidscan.h"
#include "executor/nodeUnique.h"
#include "executor/nodeValuesscan.h"
#include "executor/nodeWindowAgg.h"
#include "executor/nodeWorktablescan.h"
#include "nodes/nodeFuncs.h"
#include "nodes/relation.h"
#include "utils/rel.h"
#include "utils/syscache.h"


static bool TargetListSupportsBackwardScan(List *targetlist);
static bool IndexSupportsBackwardScan(Oid indexid);


/*
 * ExecReScan
 *		Reset a plan node so that its output can be re-scanned.
 *
 * Note that if the plan node has parameters that have changed value,
 * the output might be different from last time.
 */
void
ExecReScan(PlanState *node)
{
	/* If collecting timing stats, update them */
	if (node->instrument)
		InstrEndLoop(node->instrument);

	/*
	 * If we have changed parameters, propagate that info.
	 *
	 * Note: ExecReScanSetParamPlan() can add bits to node->chgParam,
	 * corresponding to the output param(s) that the InitPlan will update.
	 * Since we make only one pass over the list, that means that an InitPlan
	 * can depend on the output param(s) of a sibling InitPlan only if that
	 * sibling appears earlier in the list.  This is workable for now given
	 * the limited ways in which one InitPlan could depend on another, but
	 * eventually we might need to work harder (or else make the planner
	 * enlarge the extParam/allParam sets to include the params of depended-on
	 * InitPlans).
	 */
	if (node->chgParam != NULL)
	{
		ListCell   *l;

		foreach(l, node->initPlan)
		{
			SubPlanState *sstate = (SubPlanState *) lfirst(l);
			PlanState  *splan = sstate->planstate;

			if (splan->plan->extParam != NULL)	/* don't care about child
												 * local Params */
				UpdateChangedParamSet(splan, node->chgParam);
			if (splan->chgParam != NULL)
				ExecReScanSetParamPlan(sstate, node);
		}
		foreach(l, node->subPlan)
		{
			SubPlanState *sstate = (SubPlanState *) lfirst(l);
			PlanState  *splan = sstate->planstate;

			if (splan->plan->extParam != NULL)
				UpdateChangedParamSet(splan, node->chgParam);
		}
		/* Well. Now set chgParam for left/right trees. */
		if (node->lefttree != NULL)
			UpdateChangedParamSet(node->lefttree, node->chgParam);
		if (node->righttree != NULL)
			UpdateChangedParamSet(node->righttree, node->chgParam);
	}

	/* Shut down any SRFs in the plan node's targetlist */
	if (node->ps_ExprContext)
		ReScanExprContext(node->ps_ExprContext);

	/* And do node-type-specific processing */
	switch (nodeTag(node))
	{
		case T_ResultState:
			ExecReScanResult((ResultState *) node);
			break;

		case T_ModifyTableState:
			ExecReScanModifyTable((ModifyTableState *) node);
			break;

		case T_AppendState:
			ExecReScanAppend((AppendState *) node);
			break;

		case T_MergeAppendState:
			ExecReScanMergeAppend((MergeAppendState *) node);
			break;

		case T_RecursiveUnionState:
			ExecReScanRecursiveUnion((RecursiveUnionState *) node);
			break;

		case T_BitmapAndState:
			ExecReScanBitmapAnd((BitmapAndState *) node);
			break;

		case T_BitmapOrState:
			ExecReScanBitmapOr((BitmapOrState *) node);
			break;

		case T_SeqScanState:
			ExecReScanSeqScan((SeqScanState *) node);
			break;

		case T_SampleScanState:
			ExecReScanSampleScan((SampleScanState *) node);
			break;

		case T_GatherState:
			ExecReScanGather((GatherState *) node);
			break;

		case T_IndexScanState:
			ExecReScanIndexScan((IndexScanState *) node);
			break;

		case T_IndexOnlyScanState:
			ExecReScanIndexOnlyScan((IndexOnlyScanState *) node);
			break;

		case T_BitmapIndexScanState:
			ExecReScanBitmapIndexScan((BitmapIndexScanState *) node);
			break;

		case T_BitmapHeapScanState:
			ExecReScanBitmapHeapScan((BitmapHeapScanState *) node);
			break;

		case T_TidScanState:
			ExecReScanTidScan((TidScanState *) node);
			break;

		case T_SubqueryScanState:
			ExecReScanSubqueryScan((SubqueryScanState *) node);
			break;

		case T_FunctionScanState:
			ExecReScanFunctionScan((FunctionScanState *) node);
			break;

		case T_ValuesScanState:
			ExecReScanValuesScan((ValuesScanState *) node);
			break;

		case T_CteScanState:
			ExecReScanCteScan((CteScanState *) node);
			break;

		case T_WorkTableScanState:
			ExecReScanWorkTableScan((WorkTableScanState *) node);
			break;

		case T_ForeignScanState:
			ExecReScanForeignScan((ForeignScanState *) node);
			break;

		case T_CustomScanState:
			ExecReScanCustomScan((CustomScanState *) node);
			break;

		case T_NestLoopState:
			ExecReScanNestLoop((NestLoopState *) node);
			break;

		case T_MergeJoinState:
			ExecReScanMergeJoin((MergeJoinState *) node);
			break;

		case T_HashJoinState:
			ExecReScanHashJoin((HashJoinState *) node);
			break;

		case T_MaterialState:
			ExecReScanMaterial((MaterialState *) node);
			break;

		case T_SortState:
			ExecReScanSort((SortState *) node);
			break;

		case T_GroupState:
			ExecReScanGroup((GroupState *) node);
			break;

		case T_AggState:
			ExecReScanAgg((AggState *) node);
			break;

		case T_WindowAggState:
			ExecReScanWindowAgg((WindowAggState *) node);
			break;

		case T_UniqueState:
			ExecReScanUnique((UniqueState *) node);
			break;

		case T_HashState:
			ExecReScanHash((HashState *) node);
			break;

		case T_SetOpState:
			ExecReScanSetOp((SetOpState *) node);
			break;

		case T_LockRowsState:
			ExecReScanLockRows((LockRowsState *) node);
			break;

		case T_LimitState:
			ExecReScanLimit((LimitState *) node);
			break;

		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
			break;
	}

	if (node->chgParam != NULL)
	{
		bms_free(node->chgParam);
		node->chgParam = NULL;
	}
}

/*
 * ExecMarkPos
 *
 * Marks the current scan position.
 *
 * NOTE: mark/restore capability is currently needed only for plan nodes
 * that are the immediate inner child of a MergeJoin node.  Since MergeJoin
 * requires sorted input, there is never any need to support mark/restore in
 * node types that cannot produce sorted output.  There are some cases in
 * which a node can pass through sorted data from its child; if we don't
 * implement mark/restore for such a node type, the planner compensates by
 * inserting a Material node above that node.
 */
void
ExecMarkPos(PlanState *node)
{
	switch (nodeTag(node))
	{
		case T_IndexScanState:
			ExecIndexMarkPos((IndexScanState *) node);
			break;

		case T_IndexOnlyScanState:
			ExecIndexOnlyMarkPos((IndexOnlyScanState *) node);
			break;

		case T_CustomScanState:
			ExecCustomMarkPos((CustomScanState *) node);
			break;

		case T_MaterialState:
			ExecMaterialMarkPos((MaterialState *) node);
			break;

		case T_SortState:
			ExecSortMarkPos((SortState *) node);
			break;

		case T_ResultState:
			ExecResultMarkPos((ResultState *) node);
			break;

		default:
			/* don't make hard error unless caller asks to restore... */
			elog(DEBUG2, "unrecognized node type: %d", (int) nodeTag(node));
			break;
	}
}

/*
 * ExecRestrPos
 *
 * restores the scan position previously saved with ExecMarkPos()
 *
 * NOTE: the semantics of this are that the first ExecProcNode following
 * the restore operation will yield the same tuple as the first one following
 * the mark operation.  It is unspecified what happens to the plan node's
 * result TupleTableSlot.  (In most cases the result slot is unchanged by
 * a restore, but the node may choose to clear it or to load it with the
 * restored-to tuple.)	Hence the caller should discard any previously
 * returned TupleTableSlot after doing a restore.
 */
void
ExecRestrPos(PlanState *node)
{
	switch (nodeTag(node))
	{
		case T_IndexScanState:
			ExecIndexRestrPos((IndexScanState *) node);
			break;

		case T_IndexOnlyScanState:
			ExecIndexOnlyRestrPos((IndexOnlyScanState *) node);
			break;

		case T_CustomScanState:
			ExecCustomRestrPos((CustomScanState *) node);
			break;

		case T_MaterialState:
			ExecMaterialRestrPos((MaterialState *) node);
			break;

		case T_SortState:
			ExecSortRestrPos((SortState *) node);
			break;

		case T_ResultState:
			ExecResultRestrPos((ResultState *) node);
			break;

		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
			break;
	}
}

/*
 * ExecSupportsMarkRestore - does a Path support mark/restore?
 *
 * This is used during planning and so must accept a Path, not a Plan.
 * We keep it here to be adjacent to the routines above, which also must
 * know which plan types support mark/restore.
 */
bool
ExecSupportsMarkRestore(Path *pathnode)
{
	/*
	 * For consistency with the routines above, we do not examine the nodeTag
	 * but rather the pathtype, which is the Plan node type the Path would
	 * produce.
	 */
	switch (pathnode->pathtype)
	{
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_Material:
		case T_Sort:
			return true;

		case T_CustomScan:
			Assert(IsA(pathnode, CustomPath));
			if (((CustomPath *) pathnode)->flags & CUSTOMPATH_SUPPORT_MARK_RESTORE)
				return true;
			return false;

		case T_Result:

			/*
			 * Result supports mark/restore iff it has a child plan that does.
			 *
			 * We have to be careful here because there is more than one Path
			 * type that can produce a Result plan node.
			 */
			if (IsA(pathnode, ProjectionPath))
				return ExecSupportsMarkRestore(((ProjectionPath *) pathnode)->subpath);
			else if (IsA(pathnode, MinMaxAggPath))
				return false;	/* childless Result */
			else
			{
				Assert(IsA(pathnode, ResultPath));
				return false;	/* childless Result */
			}

		default:
			break;
	}

	return false;
}

/*
 * ExecSupportsBackwardScan - does a plan type support backwards scanning?
 *
 * Ideally, all plan types would support backwards scan, but that seems
 * unlikely to happen soon.  In some cases, a plan node passes the backwards
 * scan down to its children, and so supports backwards scan only if its
 * children do.  Therefore, this routine must be passed a complete plan tree.
 */
bool
ExecSupportsBackwardScan(Plan *node)
{
	if (node == NULL)
		return false;

	/*
	 * Parallel-aware nodes return a subset of the tuples in each worker, and
	 * in general we can't expect to have enough bookkeeping state to know
	 * which ones we returned in this worker as opposed to some other worker.
	 */
	if (node->parallel_aware)
		return false;

	switch (nodeTag(node))
	{
		case T_Result:
			if (outerPlan(node) != NULL)
				return ExecSupportsBackwardScan(outerPlan(node)) &&
					TargetListSupportsBackwardScan(node->targetlist);
			else
				return false;

		case T_Append:
			{
				ListCell   *l;

				foreach(l, ((Append *) node)->appendplans)
				{
					if (!ExecSupportsBackwardScan((Plan *) lfirst(l)))
						return false;
				}
				/* need not check tlist because Append doesn't evaluate it */
				return true;
			}

		case T_SeqScan:
		case T_TidScan:
		case T_FunctionScan:
		case T_ValuesScan:
		case T_CteScan:
			return TargetListSupportsBackwardScan(node->targetlist);

		case T_SampleScan:
			/* Simplify life for tablesample methods by disallowing this */
			return false;

		case T_Gather:
			return false;

		case T_IndexScan:
			return IndexSupportsBackwardScan(((IndexScan *) node)->indexid) &&
				TargetListSupportsBackwardScan(node->targetlist);

		case T_IndexOnlyScan:
			return IndexSupportsBackwardScan(((IndexOnlyScan *) node)->indexid) &&
				TargetListSupportsBackwardScan(node->targetlist);

		case T_SubqueryScan:
			return ExecSupportsBackwardScan(((SubqueryScan *) node)->subplan) &&
				TargetListSupportsBackwardScan(node->targetlist);

		case T_CustomScan:
			{
				uint32		flags = ((CustomScan *) node)->flags;

				if ((flags & CUSTOMPATH_SUPPORT_BACKWARD_SCAN) &&
					TargetListSupportsBackwardScan(node->targetlist))
					return true;
			}
			return false;

		case T_Material:
		case T_Sort:
			/* these don't evaluate tlist */
			return true;

		case T_LockRows:
		case T_Limit:
			/* these don't evaluate tlist */
			return ExecSupportsBackwardScan(outerPlan(node));

		default:
			return false;
	}
}

/*
 * If the tlist contains set-returning functions, we can't support backward
 * scan, because the TupFromTlist code is direction-ignorant.
 */
static bool
TargetListSupportsBackwardScan(List *targetlist)
{
	if (expression_returns_set((Node *) targetlist))
		return false;
	return true;
}

/*
 * An IndexScan or IndexOnlyScan node supports backward scan only if the
 * index's AM does.
 */
static bool
IndexSupportsBackwardScan(Oid indexid)
{
	bool		result;
	HeapTuple	ht_idxrel;
	Form_pg_class idxrelrec;
	IndexAmRoutine *amroutine;

	/* Fetch the pg_class tuple of the index relation */
	ht_idxrel = SearchSysCache1(RELOID, ObjectIdGetDatum(indexid));
	if (!HeapTupleIsValid(ht_idxrel))
		elog(ERROR, "cache lookup failed for relation %u", indexid);
	idxrelrec = (Form_pg_class) GETSTRUCT(ht_idxrel);

	/* Fetch the index AM's API struct */
	amroutine = GetIndexAmRoutineByAmId(idxrelrec->relam, false);

	result = amroutine->amcanbackward;

	pfree(amroutine);
	ReleaseSysCache(ht_idxrel);

	return result;
}

/*
 * ExecMaterializesOutput - does a plan type materialize its output?
 *
 * Returns true if the plan node type is one that automatically materializes
 * its output (typically by keeping it in a tuplestore).  For such plans,
 * a rescan without any parameter change will have zero startup cost and
 * very low per-tuple cost.
 */
bool
ExecMaterializesOutput(NodeTag plantype)
{
	switch (plantype)
	{
		case T_Material:
		case T_FunctionScan:
		case T_CteScan:
		case T_WorkTableScan:
		case T_Sort:
			return true;

		default:
			break;
	}

	return false;
}
