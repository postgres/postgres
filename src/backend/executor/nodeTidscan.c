/*-------------------------------------------------------------------------
 *
 * nodeTidscan.c
 *	  Routines to support direct tid scans of relations
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeTidscan.c,v 1.35 2003/09/26 01:17:01 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *
 *		ExecTidScan			scans a relation using tids
 *		ExecInitTidScan		creates and initializes state info.
 *		ExecTidReScan		rescans the tid relation.
 *		ExecEndTidScan		releases all storage.
 *		ExecTidMarkPos		marks scan position.
 *		ExecTidRestrPos		restores scan position.
 */
#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/nodeTidscan.h"
#include "access/heapam.h"
#include "parser/parsetree.h"


static void TidListCreate(TidScanState *tidstate);
static TupleTableSlot *TidNext(TidScanState *node);


/*
 * Compute the list of TIDs to be visited, by evaluating the expressions
 * for them.
 */
static void
TidListCreate(TidScanState *tidstate)
{
	List	   *evalList = tidstate->tss_tideval;
	ExprContext *econtext = tidstate->ss.ps.ps_ExprContext;
	ItemPointerData *tidList;
	int			numTids = 0;
	List	   *lst;

	tidList = (ItemPointerData *)
		palloc(length(tidstate->tss_tideval) * sizeof(ItemPointerData));

	foreach(lst, evalList)
	{
		ItemPointer itemptr;
		bool		isNull;

		itemptr = (ItemPointer)
			DatumGetPointer(ExecEvalExprSwitchContext(lfirst(lst),
													  econtext,
													  &isNull,
													  NULL));
		if (!isNull && itemptr && ItemPointerIsValid(itemptr))
		{
			tidList[numTids] = *itemptr;
			numTids++;
		}
	}

	tidstate->tss_TidList = tidList;
	tidstate->tss_NumTids = numTids;
	tidstate->tss_TidPtr = -1;
}

/* ----------------------------------------------------------------
 *		TidNext
 *
 *		Retrieve a tuple from the TidScan node's currentRelation
 *		using the tids in the TidScanState information.
 *
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
TidNext(TidScanState *node)
{
	EState	   *estate;
	ScanDirection direction;
	Snapshot	snapshot;
	Relation	heapRelation;
	HeapTuple	tuple;
	TupleTableSlot *slot;
	Index		scanrelid;
	Buffer		buffer = InvalidBuffer;
	ItemPointerData *tidList;
	int			numTids;
	bool		bBackward;
	int			tidNumber;

	/*
	 * extract necessary information from tid scan node
	 */
	estate = node->ss.ps.state;
	direction = estate->es_direction;
	snapshot = estate->es_snapshot;
	heapRelation = node->ss.ss_currentRelation;
	slot = node->ss.ss_ScanTupleSlot;
	scanrelid = ((TidScan *) node->ss.ps.plan)->scan.scanrelid;

	/*
	 * Check if we are evaluating PlanQual for tuple of this relation.
	 * Additional checking is not good, but no other way for now. We could
	 * introduce new nodes for this case and handle TidScan --> NewNode
	 * switching in Init/ReScan plan...
	 */
	if (estate->es_evTuple != NULL &&
		estate->es_evTuple[scanrelid - 1] != NULL)
	{
		ExecClearTuple(slot);
		if (estate->es_evTupleNull[scanrelid - 1])
			return slot;		/* return empty slot */

		/*
		 * XXX shouldn't we check here to make sure tuple matches TID
		 * list? In runtime-key case this is not certain, is it?
		 */

		ExecStoreTuple(estate->es_evTuple[scanrelid - 1],
					   slot, InvalidBuffer, false);

		/* Flag for the next call that no more tuples */
		estate->es_evTupleNull[scanrelid - 1] = true;
		return (slot);
	}

	/*
	 * First time through, compute the list of TIDs to be visited
	 */
	if (node->tss_TidList == NULL)
		TidListCreate(node);

	tidList = node->tss_TidList;
	numTids = node->tss_NumTids;

	tuple = &(node->tss_htup);

	/*
	 * ok, now that we have what we need, fetch an tid tuple. if scanning
	 * this tid succeeded then return the appropriate heap tuple.. else
	 * return NULL.
	 */
	bBackward = ScanDirectionIsBackward(direction);
	if (bBackward)
	{
		tidNumber = numTids - node->tss_TidPtr - 1;
		if (tidNumber < 0)
		{
			tidNumber = 0;
			node->tss_TidPtr = numTids - 1;
		}
	}
	else
	{
		if ((tidNumber = node->tss_TidPtr) < 0)
		{
			tidNumber = 0;
			node->tss_TidPtr = 0;
		}
	}
	while (tidNumber < numTids)
	{
		bool		slot_is_valid = false;

		tuple->t_self = tidList[node->tss_TidPtr];
		if (heap_fetch(heapRelation, snapshot, tuple, &buffer, false, NULL))
		{
			bool		prev_matches = false;
			int			prev_tid;

			/*
			 * store the scanned tuple in the scan tuple slot of the scan
			 * state.  Eventually we will only do this and not return a
			 * tuple.  Note: we pass 'false' because tuples returned by
			 * amgetnext are pointers onto disk pages and were not created
			 * with palloc() and so should not be pfree()'d.
			 */
			ExecStoreTuple(tuple,		/* tuple to store */
						   slot,	/* slot to store in */
						   buffer,		/* buffer associated with tuple  */
						   false);		/* don't pfree */

			/*
			 * At this point we have an extra pin on the buffer, because
			 * ExecStoreTuple incremented the pin count. Drop our local
			 * pin.
			 */
			ReleaseBuffer(buffer);

			/*
			 * We must check to see if the current tuple would have been
			 * matched by an earlier tid, so we don't double report it.
			 */
			for (prev_tid = 0; prev_tid < node->tss_TidPtr;
				 prev_tid++)
			{
				if (ItemPointerEquals(&tidList[prev_tid], &tuple->t_self))
				{
					prev_matches = true;
					break;
				}
			}
			if (!prev_matches)
				slot_is_valid = true;
			else
				ExecClearTuple(slot);
		}
		tidNumber++;
		if (bBackward)
			node->tss_TidPtr--;
		else
			node->tss_TidPtr++;
		if (slot_is_valid)
			return slot;
	}

	/*
	 * if we get here it means the tid scan failed so we are at the end of
	 * the scan..
	 */
	return ExecClearTuple(slot);
}

/* ----------------------------------------------------------------
 *		ExecTidScan(node)
 *
 *		Scans the relation using tids and returns
 *		   the next qualifying tuple in the direction specified.
 *		It calls ExecScan() and passes it the access methods which returns
 *		the next tuple using the tids.
 *
 *		Conditions:
 *		  -- the "cursor" maintained by the AMI is positioned at the tuple
 *			 returned previously.
 *
 *		Initial States:
 *		  -- the relation indicated is opened for scanning so that the
 *			 "cursor" is positioned before the first qualifying tuple.
 *		  -- tidPtr points to the first tid.
 *		  -- state variable ruleFlag = nil.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecTidScan(TidScanState *node)
{
	/*
	 * use TidNext as access method
	 */
	return ExecScan(&node->ss, (ExecScanAccessMtd) TidNext);
}

/* ----------------------------------------------------------------
 *		ExecTidReScan(node)
 * ----------------------------------------------------------------
 */
void
ExecTidReScan(TidScanState *node, ExprContext *exprCtxt)
{
	EState	   *estate;
	Index		scanrelid;

	estate = node->ss.ps.state;
	scanrelid = ((TidScan *) node->ss.ps.plan)->scan.scanrelid;

	/* If we are being passed an outer tuple, save it for runtime key calc */
	if (exprCtxt != NULL)
		node->ss.ps.ps_ExprContext->ecxt_outertuple =
			exprCtxt->ecxt_outertuple;

	/* If this is re-scanning of PlanQual ... */
	if (estate->es_evTuple != NULL &&
		estate->es_evTuple[scanrelid - 1] != NULL)
	{
		estate->es_evTupleNull[scanrelid - 1] = false;
		return;
	}

	if (node->tss_TidList)
		pfree(node->tss_TidList);
	node->tss_TidList = NULL;
	node->tss_NumTids = 0;
	node->tss_TidPtr = -1;
}

/* ----------------------------------------------------------------
 *		ExecEndTidScan
 *
 *		Releases any storage allocated through C routines.
 *		Returns nothing.
 * ----------------------------------------------------------------
 */
void
ExecEndTidScan(TidScanState *node)
{
	/*
	 * Free the exprcontext
	 */
	ExecFreeExprContext(&node->ss.ps);

	/*
	 * clear out tuple table slots
	 */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/*
	 * close the heap relation.
	 *
	 * Currently, we do not release the AccessShareLock acquired by
	 * ExecInitTidScan.  This lock should be held till end of transaction.
	 * (There is a faction that considers this too much locking, however.)
	 */
	heap_close(node->ss.ss_currentRelation, NoLock);
}

/* ----------------------------------------------------------------
 *		ExecTidMarkPos
 *
 *		Marks scan position by marking the current tid.
 *		Returns nothing.
 * ----------------------------------------------------------------
 */
void
ExecTidMarkPos(TidScanState *node)
{
	node->tss_MarkTidPtr = node->tss_TidPtr;
}

/* ----------------------------------------------------------------
 *		ExecTidRestrPos
 *
 *		Restores scan position by restoring the current tid.
 *		Returns nothing.
 *
 *		XXX Assumes previously marked scan position belongs to current tid
 * ----------------------------------------------------------------
 */
void
ExecTidRestrPos(TidScanState *node)
{
	node->tss_TidPtr = node->tss_MarkTidPtr;
}

/* ----------------------------------------------------------------
 *		ExecInitTidScan
 *
 *		Initializes the tid scan's state information, creates
 *		scan keys, and opens the base and tid relations.
 *
 *		Parameters:
 *		  node: TidNode node produced by the planner.
 *		  estate: the execution state initialized in InitPlan.
 * ----------------------------------------------------------------
 */
TidScanState *
ExecInitTidScan(TidScan *node, EState *estate)
{
	TidScanState *tidstate;
	List	   *rangeTable;
	RangeTblEntry *rtentry;
	Oid			relid;
	Oid			reloid;
	Relation	currentRelation;
	Bitmapset  *execParam = NULL;

	/*
	 * create state structure
	 */
	tidstate = makeNode(TidScanState);
	tidstate->ss.ps.plan = (Plan *) node;
	tidstate->ss.ps.state = estate;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &tidstate->ss.ps);

	/*
	 * initialize child expressions
	 */
	tidstate->ss.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->scan.plan.targetlist,
					 (PlanState *) tidstate);
	tidstate->ss.ps.qual = (List *)
		ExecInitExpr((Expr *) node->scan.plan.qual,
					 (PlanState *) tidstate);

	tidstate->tss_tideval = (List *)
		ExecInitExpr((Expr *) node->tideval,
					 (PlanState *) tidstate);

#define TIDSCAN_NSLOTS 2

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &tidstate->ss.ps);
	ExecInitScanTupleSlot(estate, &tidstate->ss);

	/*
	 * mark tid list as not computed yet
	 */
	tidstate->tss_TidList = NULL;
	tidstate->tss_NumTids = 0;
	tidstate->tss_TidPtr = -1;

	/*
	 * get the range table and direction information from the execution
	 * state (these are needed to open the relations).
	 */
	rangeTable = estate->es_range_table;

	/*
	 * open the base relation
	 *
	 * We acquire AccessShareLock for the duration of the scan.
	 */
	relid = node->scan.scanrelid;
	rtentry = rt_fetch(relid, rangeTable);
	reloid = rtentry->relid;

	currentRelation = heap_open(reloid, AccessShareLock);

	tidstate->ss.ss_currentRelation = currentRelation;
	tidstate->ss.ss_currentScanDesc = NULL;		/* no heap scan here */

	/*
	 * get the scan type from the relation descriptor.
	 */
	ExecAssignScanType(&tidstate->ss, RelationGetDescr(currentRelation), false);

	/*
	 * if there are some PARAM_EXEC in skankeys then force tid rescan on
	 * first scan.
	 */
	tidstate->ss.ps.chgParam = execParam;

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&tidstate->ss.ps);
	ExecAssignScanProjectionInfo(&tidstate->ss);

	/*
	 * all done.
	 */
	return tidstate;
}

int
ExecCountSlotsTidScan(TidScan *node)
{
	return ExecCountSlotsNode(outerPlan((Plan *) node)) +
		ExecCountSlotsNode(innerPlan((Plan *) node)) + TIDSCAN_NSLOTS;
}
