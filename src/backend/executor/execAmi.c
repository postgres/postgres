/*-------------------------------------------------------------------------
 *
 * execAmi.c--
 *	  miscellanious executor access method routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/execAmi.c,v 1.7 1997/09/08 02:22:23 momjian Exp $
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
 *		ExecReScanNode	   /  to access methods    \	amrescan
 *		ExecReScanR		  /							\	amrescan
 *		ExecMarkPos		 /							 \	ammarkpos
 *		ExecRestrPos	/							  \ amrestpos
 *
 *		ExecCreatR		function to create temporary relations
 *
 */
#include <stdio.h>				/* for sprintf() */

#include "postgres.h"

#include "executor/executor.h"
#include "storage/smgr.h"
#include "utils/mcxt.h"
#include "executor/nodeSeqscan.h"
#include "executor/nodeIndexscan.h"
#include "executor/nodeSort.h"
#include "executor/nodeTee.h"
#include "executor/execdebug.h"
#include "optimizer/internal.h" /* for _TEMP_RELATION_ID_ */
#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/heap.h"

static Pointer
ExecBeginScan(Relation relation, int nkeys, ScanKey skeys,
			  bool isindex, ScanDirection dir, TimeQual time_range);
static Relation ExecOpenR(Oid relationOid, bool isindex);

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
			  TimeQual timeRange,
			  Relation * returnRelation,		/* return */
			  Pointer * returnScanDesc) /* return */
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
	relation = ExecOpenR(relOid, isindex);
	scanDesc = ExecBeginScan(relation,
							 nkeys,
							 skeys,
							 isindex,
							 dir,
							 timeRange);

	if (returnRelation != NULL)
		*returnRelation = relation;
	if (scanDesc != NULL)
		*returnScanDesc = scanDesc;
}

/* ----------------------------------------------------------------
 *		ExecOpenR
 *
 *		returns a relation descriptor given an object id.
 * ----------------------------------------------------------------
 */
static Relation
ExecOpenR(Oid relationOid, bool isindex)
{
	Relation	relation;

	relation = (Relation) NULL;

	/* ----------------
	 *	open the relation with the correct call depending
	 *	on whether this is a heap relation or an index relation.
	 * ----------------
	 */
	if (isindex)
	{
		relation = index_open(relationOid);
	}
	else
		relation = heap_open(relationOid);

	if (relation == NULL)
		elog(DEBUG, "ExecOpenR: relation == NULL, heap_open failed.");

	return relation;
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
			  TimeQual time_range)
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
											time_range,
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
 *		closes the relation and scan descriptor for a scan or sort
 *		node.  Also closes index relations and scans for index scans.
 *
 * old comments
 *		closes the relation indicated in 'relID'
 * ----------------------------------------------------------------
 */
void
ExecCloseR(Plan * node)
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

		case T_Material:
			state = &(((Material *) node)->matstate->csstate);
			break;

		case T_Sort:
			state = &(((Sort *) node)->sortstate->csstate);
			break;

		case T_Agg:
			state = &(((Agg *) node)->aggstate->csstate);
			break;

		default:
			elog(DEBUG, "ExecCloseR: not a scan, material, or sort node!");
			return;
	}

	relation = state->css_currentRelation;
	scanDesc = state->css_currentScanDesc;

	if (scanDesc != NULL)
		heap_endscan(scanDesc);

	if (relation != NULL)
		heap_close(relation);

	/* ----------------
	 *	if this is an index scan then we have to take care
	 *	of the index relations as well..
	 * ----------------
	 */
	if (nodeTag(node) == T_IndexScan)
	{
		IndexScan  *iscan = (IndexScan *) node;
		IndexScanState *indexstate;
		int			numIndices;
		RelationPtr indexRelationDescs;
		IndexScanDescPtr indexScanDescs;
		int			i;

		indexstate = iscan->indxstate;
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
ExecReScan(Plan * node, ExprContext * exprCtxt, Plan * parent)
{
	switch (nodeTag(node))
	{
			case T_SeqScan:
			ExecSeqReScan((SeqScan *) node, exprCtxt, parent);
			return;

		case T_IndexScan:
			ExecIndexReScan((IndexScan *) node, exprCtxt, parent);
			return;

		case T_Material:

			/*
			 * the first call to ExecReScan should have no effect because
			 * everything is initialized properly already.	the following
			 * calls will be handled by ExecSeqReScan() because the nodes
			 * below the Material node have already been materialized into
			 * a temp relation.
			 */
			return;

		case T_Tee:
			ExecTeeReScan((Tee *) node, exprCtxt, parent);
			break;

		default:
			elog(WARN, "ExecReScan: not a seqscan or indexscan node.");
			return;
	}
}

/* ----------------------------------------------------------------
 *		ExecReScanR
 *
 *		XXX this does not do the right thing with indices yet.
 * ----------------------------------------------------------------
 */
HeapScanDesc
ExecReScanR(Relation relDesc,			/* LLL relDesc unused  */
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
 *		XXX Needs to be extended to include all the node types.
 * ----------------------------------------------------------------
 */
void
ExecMarkPos(Plan * node)
{
	switch (nodeTag(node))
	{
			case T_SeqScan:
			ExecSeqMarkPos((SeqScan *) node);
			break;

		case T_IndexScan:
			ExecIndexMarkPos((IndexScan *) node);
			break;

		case T_Sort:
			ExecSortMarkPos((Sort *) node);
			break;

		default:
			/* elog(DEBUG, "ExecMarkPos: unsupported node type"); */
			break;
	}
	return;
}

/* ----------------------------------------------------------------
 *		ExecRestrPos
 *
 *		restores the scan position previously saved with ExecMarkPos()
 * ----------------------------------------------------------------
 */
void
ExecRestrPos(Plan * node)
{
	switch (nodeTag(node))
	{
			case T_SeqScan:
			ExecSeqRestrPos((SeqScan *) node);
			return;

		case T_IndexScan:
			ExecIndexRestrPos((IndexScan *) node);
			return;

		case T_Sort:
			ExecSortRestrPos((Sort *) node);
			return;

		default:
			/* elog(DEBUG, "ExecRestrPos: node type not supported"); */
			return;
	}
}

/* ----------------------------------------------------------------
 *		ExecCreatR
 *
 * old comments
 *		Creates a relation.
 *
 *		Parameters:
 *		  attrType	-- type information on the attributes.
 *		  accessMtd -- access methods used to access the created relation.
 *		  relation	-- optional. Either an index to the range table or
 *					   negative number indicating a temporary relation.
 *					   A temporary relation is assume is this field is absent.
 * ----------------------------------------------------------------
 */

Relation
ExecCreatR(TupleDesc tupType,
		   Oid relationOid)
{
	Relation	relDesc;

	EU3_printf("ExecCreatR: %s type=%d oid=%d\n",
			   "entering: ", tupType, relationOid);
	CXT1_printf("ExecCreatR: context is %d\n", CurrentMemoryContext);

	relDesc = NULL;

	if (relationOid == _TEMP_RELATION_ID_)
	{
		/* ----------------
		 *	 create a temporary relation
		 *	 (currently the planner always puts a _TEMP_RELATION_ID
		 *	 in the relation argument so we expect this to be the case although
		 *	 it's possible that someday we'll get the name from
		 *	 from the range table.. -cim 10/12/89)
		 * ----------------
		 */
/*
		sprintf(tempname, "temp_%d.%d", getpid(), tmpcnt++);
		EU1_printf("ExecCreatR: attempting to create %s\n", tempname);
*/

		/*
		 * heap_creatr creates a name if the argument to heap_creatr is
		 * '\0 '
		 */
		relDesc = heap_creatr("",
							  DEFAULT_SMGR,
							  tupType);
	}
	else
	{
		/* ----------------
		 *		use a relation from the range table
		 * ----------------
		 */
		elog(DEBUG, "ExecCreatR: %s",
			 "stuff using range table id's is not functional");
	}

	if (relDesc == NULL)
		elog(DEBUG, "ExecCreatR: failed to create relation.");

	EU1_printf("ExecCreatR: returning relDesc=%d\n", relDesc);

	return relDesc;
}
