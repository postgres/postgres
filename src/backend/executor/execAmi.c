/*-------------------------------------------------------------------------
 *
 * execAmi.c
 *	  miscellaneous executor access method routines
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	$Id: execAmi.c,v 1.65 2002/11/30 05:21:01 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/heap.h"
#include "executor/execdebug.h"
#include "executor/instrument.h"
#include "executor/nodeAgg.h"
#include "executor/nodeAppend.h"
#include "executor/nodeGroup.h"
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
#include "executor/nodeFunctionscan.h"
#include "executor/nodeUnique.h"


/* ----------------------------------------------------------------
 *		ExecReScan
 *
 *		XXX this should be extended to cope with all the node types..
 *
 *		takes the new expression context as an argument, so that
 *		index scans needn't have their scan keys updated separately
 *		- marcel 09/20/94
 * ----------------------------------------------------------------
 */
void
ExecReScan(Plan *node, ExprContext *exprCtxt, Plan *parent)
{
	if (node->instrument)
		InstrEndLoop(node->instrument);

	if (node->chgParam != NULL) /* Wow! */
	{
		List	   *lst;

		foreach(lst, node->initPlan)
		{
			Plan	   *splan = ((SubPlan *) lfirst(lst))->plan;

			if (splan->extParam != NULL)		/* don't care about child
												 * locParam */
				SetChangedParamList(splan, node->chgParam);
			if (splan->chgParam != NULL)
				ExecReScanSetParamPlan((SubPlan *) lfirst(lst), node);
		}
		foreach(lst, node->subPlan)
		{
			Plan	   *splan = ((SubPlan *) lfirst(lst))->plan;

			if (splan->extParam != NULL)
				SetChangedParamList(splan, node->chgParam);
		}
		/* Well. Now set chgParam for left/right trees. */
		if (node->lefttree != NULL)
			SetChangedParamList(node->lefttree, node->chgParam);
		if (node->righttree != NULL)
			SetChangedParamList(node->righttree, node->chgParam);
	}

	switch (nodeTag(node))
	{
		case T_SeqScan:
			ExecSeqReScan((SeqScan *) node, exprCtxt, parent);
			break;

		case T_IndexScan:
			ExecIndexReScan((IndexScan *) node, exprCtxt, parent);
			break;

		case T_TidScan:
			ExecTidReScan((TidScan *) node, exprCtxt, parent);
			break;

		case T_SubqueryScan:
			ExecSubqueryReScan((SubqueryScan *) node, exprCtxt, parent);
			break;

		case T_FunctionScan:
			ExecFunctionReScan((FunctionScan *) node, exprCtxt, parent);
			break;

		case T_Material:
			ExecMaterialReScan((Material *) node, exprCtxt, parent);
			break;

		case T_NestLoop:
			ExecReScanNestLoop((NestLoop *) node, exprCtxt, parent);
			break;

		case T_HashJoin:
			ExecReScanHashJoin((HashJoin *) node, exprCtxt, parent);
			break;

		case T_Hash:
			ExecReScanHash((Hash *) node, exprCtxt, parent);
			break;

		case T_Agg:
			ExecReScanAgg((Agg *) node, exprCtxt, parent);
			break;

		case T_Group:
			ExecReScanGroup((Group *) node, exprCtxt, parent);
			break;

		case T_Result:
			ExecReScanResult((Result *) node, exprCtxt, parent);
			break;

		case T_Unique:
			ExecReScanUnique((Unique *) node, exprCtxt, parent);
			break;

		case T_SetOp:
			ExecReScanSetOp((SetOp *) node, exprCtxt, parent);
			break;

		case T_Limit:
			ExecReScanLimit((Limit *) node, exprCtxt, parent);
			break;

		case T_Sort:
			ExecReScanSort((Sort *) node, exprCtxt, parent);
			break;

		case T_MergeJoin:
			ExecReScanMergeJoin((MergeJoin *) node, exprCtxt, parent);
			break;

		case T_Append:
			ExecReScanAppend((Append *) node, exprCtxt, parent);
			break;

		default:
			elog(ERROR, "ExecReScan: node type %d not supported",
				 nodeTag(node));
			return;
	}

	if (node->chgParam != NULL)
	{
		freeList(node->chgParam);
		node->chgParam = NULL;
	}
}

/*
 * ExecMarkPos
 *
 * Marks the current scan position.
 */
void
ExecMarkPos(Plan *node)
{
	switch (nodeTag(node))
	{
		case T_SeqScan:
			ExecSeqMarkPos((SeqScan *) node);
			break;

		case T_IndexScan:
			ExecIndexMarkPos((IndexScan *) node);
			break;

		case T_TidScan:
			ExecTidMarkPos((TidScan *) node);
			break;

		case T_FunctionScan:
			ExecFunctionMarkPos((FunctionScan *) node);
			break;

		case T_Material:
			ExecMaterialMarkPos((Material *) node);
			break;

		case T_Sort:
			ExecSortMarkPos((Sort *) node);
			break;

		default:
			/* don't make hard error unless caller asks to restore... */
			elog(LOG, "ExecMarkPos: node type %d not supported",
				 nodeTag(node));
			break;
	}
}

/*
 * ExecRestrPos
 *
 * restores the scan position previously saved with ExecMarkPos()
 */
void
ExecRestrPos(Plan *node)
{
	switch (nodeTag(node))
	{
		case T_SeqScan:
			ExecSeqRestrPos((SeqScan *) node);
			break;

		case T_IndexScan:
			ExecIndexRestrPos((IndexScan *) node);
			break;

		case T_TidScan:
			ExecTidRestrPos((TidScan *) node);
			break;

		case T_FunctionScan:
			ExecFunctionRestrPos((FunctionScan *) node);
			break;

		case T_Material:
			ExecMaterialRestrPos((Material *) node);
			break;

		case T_Sort:
			ExecSortRestrPos((Sort *) node);
			break;

		default:
			elog(ERROR, "ExecRestrPos: node type %d not supported",
				 nodeTag(node));
			break;
	}
}

/*
 * ExecSupportsMarkRestore - does a plan type support mark/restore?
 *
 * XXX Ideally, all plan node types would support mark/restore, and this
 * wouldn't be needed.  For now, this had better match the routines above.
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
		case T_Material:
		case T_Sort:
			return true;

		default:
			break;
	}

	return false;
}
