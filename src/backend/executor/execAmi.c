/*-------------------------------------------------------------------------
 *
 * execAmi.c
 *	  miscellaneous executor access method routines
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	$Id: execAmi.c,v 1.54 2000/10/26 21:35:15 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 INTERFACE ROUTINES
 *
 *		ExecOpenScanR	\							  / amopen
 *		ExecBeginScan	 \							 /	ambeginscan
 *		ExecCloseR		  \							/	amclose
 *		ExecInsert		   \  executor interface   /	aminsert
 *		ExecReScanR		   /  to access methods    \	amrescan
 *		ExecMarkPos		  /							\	ammarkpos
 *		ExecRestrPos	 /							 \  amrestpos
 */

#include "postgres.h"


#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/heap.h"
#include "executor/execdebug.h"
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
#include "executor/nodeUnique.h"

static Pointer ExecBeginScan(Relation relation, int nkeys, ScanKey skeys,
			  bool isindex, ScanDirection dir, Snapshot snapshot);

/* ----------------------------------------------------------------
 *		ExecOpenScanR
 *
 * old comments:
 *		Parameters:
 *		  relation -- relation to be opened and scanned.
 *		  nkeys    -- number of keys
 *		  skeys    -- keys to restrict scanning
 *			 isindex  -- if this is true, the relation is the relid of
 *						 an index relation, else it is an index into the
 *						 range table.
 *		Returns the relation as(relDesc scanDesc)
 *		   If this structure is changed, need to modify the access macros
 *		defined in execInt.h.
 * ----------------------------------------------------------------
 */
void
ExecOpenScanR(Oid relOid,
			  int nkeys,
			  ScanKey skeys,
			  bool isindex,
			  ScanDirection dir,
			  Snapshot snapshot,
			  Relation *returnRelation, /* return */
			  Pointer *returnScanDesc)	/* return */
{
	Relation	relation;
	Pointer		scanDesc;

	/* ----------------
	 *	note: scanDesc returned by ExecBeginScan can be either
	 *		  a HeapScanDesc or an IndexScanDesc so for now we
	 *		  make it a Pointer.  There should be a better scan
	 *		  abstraction someday -cim 9/9/89
	 * ----------------
	 */

	/* ----------------
	 *	open the relation with the correct call depending
	 *	on whether this is a heap relation or an index relation.
	 *
	 *	Do not lock the rel here; beginscan will acquire AccessShareLock.
	 * ----------------
	 */
	if (isindex)
		relation = index_open(relOid);
	else
		relation = heap_open(relOid, NoLock);

	scanDesc = ExecBeginScan(relation,
							 nkeys,
							 skeys,
							 isindex,
							 dir,
							 snapshot);

	if (returnRelation != NULL)
		*returnRelation = relation;
	if (scanDesc != NULL)
		*returnScanDesc = scanDesc;
}

/* ----------------------------------------------------------------
 *		ExecBeginScan
 *
 *		beginscans a relation in current direction.
 *
 *		XXX fix parameters to AMbeginscan (and btbeginscan)
 *				currently we need to pass a flag stating whether
 *				or not the scan should begin at an endpoint of
 *				the relation.. Right now we always pass false
 *				-cim 9/14/89
 * ----------------------------------------------------------------
 */
static Pointer
ExecBeginScan(Relation relation,
			  int nkeys,
			  ScanKey skeys,
			  bool isindex,
			  ScanDirection dir,
			  Snapshot snapshot)
{
	Pointer		scanDesc;

	scanDesc = NULL;

	/* ----------------
	 *	open the appropriate type of scan.
	 *
	 *	Note: ambeginscan()'s second arg is a boolean indicating
	 *		  that the scan should be done in reverse..  That is,
	 *		  if you pass it true, then the scan is backward.
	 * ----------------
	 */
	if (isindex)
	{
		scanDesc = (Pointer) index_beginscan(relation,
											 false,		/* see above comment */
											 nkeys,
											 skeys);
	}
	else
	{
		scanDesc = (Pointer) heap_beginscan(relation,
											ScanDirectionIsBackward(dir),
											snapshot,
											nkeys,
											skeys);
	}

	if (scanDesc == NULL)
		elog(DEBUG, "ExecBeginScan: scanDesc = NULL, heap_beginscan failed.");

	return scanDesc;
}

/* ----------------------------------------------------------------
 *		ExecCloseR
 *
 *		closes the relation and scan descriptor for a scan node.
 *		Also closes index relations and scans for index scans.
 * ----------------------------------------------------------------
 */
void
ExecCloseR(Plan *node)
{
	CommonScanState *state;
	Relation	relation;
	HeapScanDesc scanDesc;

	/* ----------------
	 *	shut down the heap scan and close the heap relation
	 * ----------------
	 */
	switch (nodeTag(node))
	{

		case T_SeqScan:
			state = ((SeqScan *) node)->scanstate;
			break;

		case T_IndexScan:
			state = ((IndexScan *) node)->scan.scanstate;
			break;

		case T_TidScan:
			state = ((TidScan *) node)->scan.scanstate;
			break;

		default:
			elog(DEBUG, "ExecCloseR: not a scan node!");
			return;
	}

	relation = state->css_currentRelation;
	scanDesc = state->css_currentScanDesc;

	if (scanDesc != NULL)
		heap_endscan(scanDesc);

	/*
	 * endscan released AccessShareLock acquired by beginscan.	If we are
	 * holding any stronger locks on the rel, they should be held till end
	 * of xact.  Therefore, we need only close the rel and not release
	 * locks.
	 */
	if (relation != NULL)
		heap_close(relation, NoLock);

	/* ----------------
	 *	if this is an index scan then we have to take care
	 *	of the index relations as well..
	 * ----------------
	 */
	if (IsA(node, IndexScan))
	{
		IndexScan  *iscan = (IndexScan *) node;
		IndexScanState *indexstate = iscan->indxstate;
		int			numIndices;
		RelationPtr indexRelationDescs;
		IndexScanDescPtr indexScanDescs;
		int			i;

		numIndices = indexstate->iss_NumIndices;
		indexRelationDescs = indexstate->iss_RelationDescs;
		indexScanDescs = indexstate->iss_ScanDescs;

		for (i = 0; i < numIndices; i++)
		{
			/* ----------------
			 *	shut down each of the scans and
			 *	close each of the index relations
			 * ----------------
			 */
			if (indexScanDescs[i] != NULL)
				index_endscan(indexScanDescs[i]);

			if (indexRelationDescs[i] != NULL)
				index_close(indexRelationDescs[i]);
		}
	}
}

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

/* ----------------------------------------------------------------
 *		ExecReScanR
 *
 *		XXX this does not do the right thing with indices yet.
 * ----------------------------------------------------------------
 */
HeapScanDesc
ExecReScanR(Relation relDesc,	/* LLL relDesc unused  */
			HeapScanDesc scanDesc,
			ScanDirection direction,
			int nkeys,			/* LLL nkeys unused  */
			ScanKey skeys)
{
	if (scanDesc != NULL)
		heap_rescan(scanDesc,	/* scan desc */
					ScanDirectionIsBackward(direction), /* backward flag */
					skeys);		/* scan keys */

	return scanDesc;
}

/* ----------------------------------------------------------------
 *		ExecMarkPos
 *
 *		Marks the current scan position.
 *
 *		XXX Needs to be extended to include all the node types,
 *		or at least all the ones that can be directly below a mergejoin.
 * ----------------------------------------------------------------
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

		case T_Material:
			ExecMaterialMarkPos((Material *) node);
			break;

		case T_Sort:
			ExecSortMarkPos((Sort *) node);
			break;

		case T_TidScan:
			ExecTidMarkPos((TidScan *) node);
			break;

		default:
			/* don't make hard error unless caller asks to restore... */
			elog(DEBUG, "ExecMarkPos: node type %d not supported",
				 nodeTag(node));
			break;
	}
}

/* ----------------------------------------------------------------
 *		ExecRestrPos
 *
 *		restores the scan position previously saved with ExecMarkPos()
 *
 *		XXX Needs to be extended to include all the node types,
 *		or at least all the ones that can be directly below a mergejoin.
 * ----------------------------------------------------------------
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
