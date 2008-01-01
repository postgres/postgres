/*-------------------------------------------------------------------------
 *
 * execAmi.c
 *	  miscellaneous executor access method routines
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	$PostgreSQL: pgsql/src/backend/executor/execAmi.c,v 1.94 2008/01/01 19:45:49 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/instrument.h"
#include "executor/nodeAgg.h"
#include "executor/nodeAppend.h"
#include "executor/nodeBitmapAnd.h"
#include "executor/nodeBitmapHeapscan.h"
#include "executor/nodeBitmapIndexscan.h"
#include "executor/nodeBitmapOr.h"
#include "executor/nodeFunctionscan.h"
#include "executor/nodeGroup.h"
#include "executor/nodeGroup.h"
#include "executor/nodeHash.h"
#include "executor/nodeHashjoin.h"
#include "executor/nodeIndexscan.h"
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
#include "executor/nodeTidscan.h"
#include "executor/nodeUnique.h"
#include "executor/nodeValuesscan.h"


/*
 * ExecReScan
 *		Reset a plan node so that its output can be re-scanned.
 *
 * Note that if the plan node has parameters that have changed value,
 * the output might be different from last time.
 *
 * The second parameter is currently only used to pass a NestLoop plan's
 * econtext down to its inner child plan, in case that is an indexscan that
 * needs access to variables of the current outer tuple.  (The handling of
 * this parameter is currently pretty inconsistent: some callers pass NULL
 * and some pass down their parent's value; so don't rely on it in other
 * situations.	It'd probably be better to remove the whole thing and use
 * the generalized parameter mechanism instead.)
 */
void
ExecReScan(PlanState *node, ExprContext *exprCtxt)
{
	/* If collecting timing stats, update them */
	if (node->instrument)
		InstrEndLoop(node->instrument);

	/* If we have changed parameters, propagate that info */
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
			ExecReScanResult((ResultState *) node, exprCtxt);
			break;

		case T_AppendState:
			ExecReScanAppend((AppendState *) node, exprCtxt);
			break;

		case T_BitmapAndState:
			ExecReScanBitmapAnd((BitmapAndState *) node, exprCtxt);
			break;

		case T_BitmapOrState:
			ExecReScanBitmapOr((BitmapOrState *) node, exprCtxt);
			break;

		case T_SeqScanState:
			ExecSeqReScan((SeqScanState *) node, exprCtxt);
			break;

		case T_IndexScanState:
			ExecIndexReScan((IndexScanState *) node, exprCtxt);
			break;

		case T_BitmapIndexScanState:
			ExecBitmapIndexReScan((BitmapIndexScanState *) node, exprCtxt);
			break;

		case T_BitmapHeapScanState:
			ExecBitmapHeapReScan((BitmapHeapScanState *) node, exprCtxt);
			break;

		case T_TidScanState:
			ExecTidReScan((TidScanState *) node, exprCtxt);
			break;

		case T_SubqueryScanState:
			ExecSubqueryReScan((SubqueryScanState *) node, exprCtxt);
			break;

		case T_FunctionScanState:
			ExecFunctionReScan((FunctionScanState *) node, exprCtxt);
			break;

		case T_ValuesScanState:
			ExecValuesReScan((ValuesScanState *) node, exprCtxt);
			break;

		case T_NestLoopState:
			ExecReScanNestLoop((NestLoopState *) node, exprCtxt);
			break;

		case T_MergeJoinState:
			ExecReScanMergeJoin((MergeJoinState *) node, exprCtxt);
			break;

		case T_HashJoinState:
			ExecReScanHashJoin((HashJoinState *) node, exprCtxt);
			break;

		case T_MaterialState:
			ExecMaterialReScan((MaterialState *) node, exprCtxt);
			break;

		case T_SortState:
			ExecReScanSort((SortState *) node, exprCtxt);
			break;

		case T_GroupState:
			ExecReScanGroup((GroupState *) node, exprCtxt);
			break;

		case T_AggState:
			ExecReScanAgg((AggState *) node, exprCtxt);
			break;

		case T_UniqueState:
			ExecReScanUnique((UniqueState *) node, exprCtxt);
			break;

		case T_HashState:
			ExecReScanHash((HashState *) node, exprCtxt);
			break;

		case T_SetOpState:
			ExecReScanSetOp((SetOpState *) node, exprCtxt);
			break;

		case T_LimitState:
			ExecReScanLimit((LimitState *) node, exprCtxt);
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
 */
void
ExecMarkPos(PlanState *node)
{
	switch (nodeTag(node))
	{
		case T_SeqScanState:
			ExecSeqMarkPos((SeqScanState *) node);
			break;

		case T_IndexScanState:
			ExecIndexMarkPos((IndexScanState *) node);
			break;

		case T_TidScanState:
			ExecTidMarkPos((TidScanState *) node);
			break;

		case T_FunctionScanState:
			ExecFunctionMarkPos((FunctionScanState *) node);
			break;

		case T_ValuesScanState:
			ExecValuesMarkPos((ValuesScanState *) node);
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
 * the mark operation.	It is unspecified what happens to the plan node's
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
		case T_SeqScanState:
			ExecSeqRestrPos((SeqScanState *) node);
			break;

		case T_IndexScanState:
			ExecIndexRestrPos((IndexScanState *) node);
			break;

		case T_TidScanState:
			ExecTidRestrPos((TidScanState *) node);
			break;

		case T_FunctionScanState:
			ExecFunctionRestrPos((FunctionScanState *) node);
			break;

		case T_ValuesScanState:
			ExecValuesRestrPos((ValuesScanState *) node);
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
 * ExecSupportsMarkRestore - does a plan type support mark/restore?
 *
 * XXX Ideally, all plan node types would support mark/restore, and this
 * wouldn't be needed.  For now, this had better match the routines above.
 * But note the test is on Plan nodetype, not PlanState nodetype.
 *
 * (However, since the only present use of mark/restore is in mergejoin,
 * there is no need to support mark/restore in any plan type that is not
 * capable of generating ordered output.  So the seqscan, tidscan,
 * functionscan, and valuesscan support is actually useless code at present.)
 */
bool
ExecSupportsMarkRestore(NodeTag plantype)
{
	switch (plantype)
	{
		case T_SeqScan:
		case T_IndexScan:
		case T_TidScan:
		case T_FunctionScan:
		case T_ValuesScan:
		case T_Material:
		case T_Sort:
			return true;

		case T_Result:

			/*
			 * T_Result only supports mark/restore if it has a child plan that
			 * does, so we do not have enough information to give a really
			 * correct answer.	However, for current uses it's enough to
			 * always say "false", because this routine is not asked about
			 * gating Result plans, only base-case Results.
			 */
			return false;

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

	switch (nodeTag(node))
	{
		case T_Result:
			if (outerPlan(node) != NULL)
				return ExecSupportsBackwardScan(outerPlan(node));
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
				return true;
			}

		case T_SeqScan:
		case T_IndexScan:
		case T_TidScan:
		case T_FunctionScan:
		case T_ValuesScan:
			return true;

		case T_SubqueryScan:
			return ExecSupportsBackwardScan(((SubqueryScan *) node)->subplan);

		case T_Material:
		case T_Sort:
			return true;

		case T_Unique:
			return ExecSupportsBackwardScan(outerPlan(node));

		case T_Limit:
			return ExecSupportsBackwardScan(outerPlan(node));

		default:
			return false;
	}
}

/*
 * ExecMayReturnRawTuples
 *		Check whether a plan tree may return "raw" disk tuples (that is,
 *		pointers to original data in disk buffers, as opposed to temporary
 *		tuples constructed by projection steps).  In the case of Append,
 *		some subplans may return raw tuples and others projected tuples;
 *		we return "true" if any of the returned tuples could be raw.
 *
 * This must be passed an already-initialized planstate tree, because we
 * need to look at the results of ExecAssignScanProjectionInfo().
 */
bool
ExecMayReturnRawTuples(PlanState *node)
{
	/*
	 * At a table scan node, we check whether ExecAssignScanProjectionInfo
	 * decided to do projection or not.  Most non-scan nodes always project
	 * and so we can return "false" immediately.  For nodes that don't project
	 * but just pass up input tuples, we have to recursively examine the input
	 * plan node.
	 *
	 * Note: Hash and Material are listed here because they sometimes return
	 * an original input tuple, not a copy.  But Sort and SetOp never return
	 * an original tuple, so they can be treated like projecting nodes.
	 */
	switch (nodeTag(node))
	{
			/* Table scan nodes */
		case T_SeqScanState:
		case T_IndexScanState:
		case T_BitmapHeapScanState:
		case T_TidScanState:
			if (node->ps_ProjInfo == NULL)
				return true;
			break;

		case T_SubqueryScanState:
			/* If not projecting, look at input plan */
			if (node->ps_ProjInfo == NULL)
				return ExecMayReturnRawTuples(((SubqueryScanState *) node)->subplan);
			break;

			/* Non-projecting nodes */
		case T_HashState:
		case T_MaterialState:
		case T_UniqueState:
		case T_LimitState:
			return ExecMayReturnRawTuples(node->lefttree);

		case T_AppendState:
			{
				AppendState *appendstate = (AppendState *) node;
				int			j;

				for (j = 0; j < appendstate->as_nplans; j++)
				{
					if (ExecMayReturnRawTuples(appendstate->appendplans[j]))
						return true;
				}
				break;
			}

			/* All projecting node types come here */
		default:
			break;
	}
	return false;
}
