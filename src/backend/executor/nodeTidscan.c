/*-------------------------------------------------------------------------
 *
 * nodeTidscan.c
 *	  Routines to support direct tid scans of relations
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeTidscan.c,v 1.6 2000/04/12 17:15:10 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *
 *		ExecTidScan		scans a relation using tids
 *		ExecInitTidScan		creates and initializes state info.
 *		ExecTidReScan		rescans the tid relation.
 *		ExecEndTidScan		releases all storage.
 *		ExecTidMarkPos		marks scan position.
 *		ExecTidRestrPos		restores scan position.
 *
 */
#include "postgres.h"

#include "executor/executor.h"
#include "executor/execdebug.h"
#include "executor/nodeTidscan.h"
#include "optimizer/clauses.h"	/* for get_op, get_leftop, get_rightop */
#include "access/heapam.h"
#include "parser/parsetree.h"

static int	TidListCreate(List *, ExprContext *, ItemPointer *);
static TupleTableSlot *TidNext(TidScan *node);

static int
TidListCreate(List *evalList, ExprContext *econtext, ItemPointer *tidList)
{
	List	   *lst;
	ItemPointer itemptr;
	bool		isNull;
	int			numTids = 0;

	foreach(lst, evalList)
	{
		itemptr = (ItemPointer) ExecEvalExpr(lfirst(lst), econtext,
											 &isNull, (bool *) 0);
		if (itemptr && ItemPointerIsValid(itemptr))
		{
			tidList[numTids] = itemptr;
			numTids++;
		}
	}
	return numTids;
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
TidNext(TidScan *node)
{
	EState	   *estate;
	CommonScanState *scanstate;
	TidScanState *tidstate;
	ScanDirection direction;
	Snapshot	snapshot;
	Relation	heapRelation;
	HeapTuple	tuple;
	TupleTableSlot *slot;
	Buffer		buffer = InvalidBuffer;
	int			numTids;

	bool		bBackward;
	int			tidNumber;
	ItemPointer *tidList,
				itemptr;

	/* ----------------
	 *	extract necessary information from tid scan node
	 * ----------------
	 */
	estate = node->scan.plan.state;
	direction = estate->es_direction;
	snapshot = estate->es_snapshot;
	scanstate = node->scan.scanstate;
	tidstate = node->tidstate;
	heapRelation = scanstate->css_currentRelation;
	numTids = tidstate->tss_NumTids;
	tidList = tidstate->tss_TidList;
	slot = scanstate->css_ScanTupleSlot;

	/*
	 * Check if we are evaluating PlanQual for tuple of this relation.
	 * Additional checking is not good, but no other way for now. We could
	 * introduce new nodes for this case and handle TidScan --> NewNode
	 * switching in Init/ReScan plan...
	 */
	if (estate->es_evTuple != NULL &&
		estate->es_evTuple[node->scan.scanrelid - 1] != NULL)
	{
		ExecClearTuple(slot);
		if (estate->es_evTupleNull[node->scan.scanrelid - 1])
			return slot;		/* return empty slot */

		/* probably ought to use ExecStoreTuple here... */
		slot->val = estate->es_evTuple[node->scan.scanrelid - 1];
		slot->ttc_shouldFree = false;

		/* Flag for the next call that no more tuples */
		estate->es_evTupleNull[node->scan.scanrelid - 1] = true;
		return (slot);
	}

	tuple = &(tidstate->tss_htup);

	/* ----------------
	 *	ok, now that we have what we need, fetch an tid tuple.
	 *	if scanning this tid succeeded then return the
	 *	appropriate heap tuple.. else return NULL.
	 * ----------------
	 */
	bBackward = ScanDirectionIsBackward(direction);
	if (bBackward)
	{
		tidNumber = numTids - tidstate->tss_TidPtr - 1;
		if (tidNumber < 0)
		{
			tidNumber = 0;
			tidstate->tss_TidPtr = numTids - 1;
		}
	}
	else
	{
		if ((tidNumber = tidstate->tss_TidPtr) < 0)
		{
			tidNumber = 0;
			tidstate->tss_TidPtr = 0;
		}
	}
	while (tidNumber < numTids)
	{
		bool		slot_is_valid = false;

		itemptr = tidList[tidstate->tss_TidPtr];
		tuple->t_datamcxt = NULL;
		tuple->t_data = NULL;
		if (itemptr)
		{
			tuple->t_self = *(itemptr);
			heap_fetch(heapRelation, snapshot, tuple, &buffer);
		}
		if (tuple->t_data != NULL)
		{
			bool		prev_matches = false;
			int			prev_tid;

			/* ----------------
			 *	store the scanned tuple in the scan tuple slot of
			 *	the scan state.  Eventually we will only do this and not
			 *	return a tuple.  Note: we pass 'false' because tuples
			 *	returned by amgetnext are pointers onto disk pages and
			 *	were not created with palloc() and so should not be pfree()'d.
			 * ----------------
			 */
			ExecStoreTuple(tuple,		/* tuple to store */
						   slot,/* slot to store in */
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
			 * matched by an earlier tid, so we don't double report it. We
			 * do this by passing the tuple through ExecQual and look for
			 * failure with all previous qualifications.
			 */
			for (prev_tid = 0; prev_tid < tidstate->tss_TidPtr;
				 prev_tid++)
			{
				if (ItemPointerEquals(tidList[prev_tid], &tuple->t_self))
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
		else if (BufferIsValid(buffer))
			ReleaseBuffer(buffer);
		tidNumber++;
		if (bBackward)
			tidstate->tss_TidPtr--;
		else
			tidstate->tss_TidPtr++;
		if (slot_is_valid)
			return slot;
	}
	/* ----------------
	 *	if we get here it means the tid scan failed so we
	 *	are at the end of the scan..
	 * ----------------
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
ExecTidScan(TidScan *node)
{
	/* ----------------
	 *	use TidNext as access method
	 * ----------------
	 */
	return ExecScan(&node->scan, TidNext);
}

/* ----------------------------------------------------------------
 *		ExecTidReScan(node)
 * ----------------------------------------------------------------
 */
void
ExecTidReScan(TidScan *node, ExprContext *exprCtxt, Plan *parent)
{
	EState	   *estate;
	TidScanState *tidstate;
	ItemPointer *tidList;

	tidstate = node->tidstate;
	estate = node->scan.plan.state;
	tidstate->tss_TidPtr = -1;
	tidList = tidstate->tss_TidList;

	/* If we are being passed an outer tuple, save it for runtime key calc */
	if (exprCtxt != NULL)
		node->scan.scanstate->cstate.cs_ExprContext->ecxt_outertuple =
			exprCtxt->ecxt_outertuple;

	/* If this is re-scanning of PlanQual ... */
	if (estate->es_evTuple != NULL &&
		estate->es_evTuple[node->scan.scanrelid - 1] != NULL)
	{
		estate->es_evTupleNull[node->scan.scanrelid - 1] = false;
		return;
	}

	tidstate->tss_NumTids = TidListCreate(node->tideval,
							 node->scan.scanstate->cstate.cs_ExprContext,
										  tidList);

	/* ----------------
	 *	perhaps return something meaningful
	 * ----------------
	 */
	return;
}

/* ----------------------------------------------------------------
 *		ExecEndTidScan
 *
 *		Releases any storage allocated through C routines.
 *		Returns nothing.
 * ----------------------------------------------------------------
 */
void
ExecEndTidScan(TidScan *node)
{
	CommonScanState *scanstate;
	TidScanState *tidstate;

	scanstate = node->scan.scanstate;
	tidstate = node->tidstate;
	if (tidstate && tidstate->tss_TidList)
		pfree(tidstate->tss_TidList);

	/* ----------------
	 *	extract information from the node
	 * ----------------
	 */

	/* ----------------
	 *	Free the projection info and the scan attribute info
	 *
	 *	Note: we don't ExecFreeResultType(scanstate)
	 *		  because the rule manager depends on the tupType
	 *		  returned by ExecMain().  So for now, this
	 *		  is freed at end-transaction time.  -cim 6/2/91
	 * ----------------
	 */
	ExecFreeProjectionInfo(&scanstate->cstate);

	/* ----------------
	 *	close the heap and tid relations
	 * ----------------
	 */
	ExecCloseR((Plan *) node);

	/* ----------------
	 *	clear out tuple table slots
	 * ----------------
	 */
	ExecClearTuple(scanstate->cstate.cs_ResultTupleSlot);
	ExecClearTuple(scanstate->css_ScanTupleSlot);
/*	  ExecClearTuple(scanstate->css_RawTupleSlot); */
}

/* ----------------------------------------------------------------
 *		ExecTidMarkPos
 *
 *		Marks scan position by marking the current tid.
 *		Returns nothing.
 * ----------------------------------------------------------------
 */
void
ExecTidMarkPos(TidScan *node)
{
	TidScanState *tidstate;

	tidstate = node->tidstate;
	tidstate->tss_MarkTidPtr = tidstate->tss_TidPtr;
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
ExecTidRestrPos(TidScan *node)
{
	TidScanState *tidstate;

	tidstate = node->tidstate;
	tidstate->tss_TidPtr = tidstate->tss_MarkTidPtr;
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
bool
ExecInitTidScan(TidScan *node, EState *estate, Plan *parent)
{
	TidScanState *tidstate;
	CommonScanState *scanstate;
	ItemPointer *tidList;
	int			numTids;
	int			tidPtr;
	List	   *rangeTable;
	RangeTblEntry *rtentry;
	Oid			relid;
	Oid			reloid;

	Relation	currentRelation;
	int			baseid;

	List	   *execParam = NULL;

	/* ----------------
	 *	assign execution state to node
	 * ----------------
	 */
	node->scan.plan.state = estate;

	/* --------------------------------
	 *	Part 1)  initialize scan state
	 *
	 *	create new CommonScanState for node
	 * --------------------------------
	 */
	scanstate = makeNode(CommonScanState);
/*
	scanstate->ss_ProcOuterFlag = false;
	scanstate->ss_OldRelId = 0;
*/

	node->scan.scanstate = scanstate;

	/* ----------------
	 *	assign node's base_id .. we don't use AssignNodeBaseid() because
	 *	the increment is done later on after we assign the tid scan's
	 *	scanstate.	see below.
	 * ----------------
	 */
	baseid = estate->es_BaseId;
/*	  scanstate->csstate.cstate.bnode.base_id = baseid; */
	scanstate->cstate.cs_base_id = baseid;

	/* ----------------
	 *	create expression context for node
	 * ----------------
	 */
	ExecAssignExprContext(estate, &scanstate->cstate);

#define TIDSCAN_NSLOTS 3
	/* ----------------
	 *	tuple table initialization
	 * ----------------
	 */
	ExecInitResultTupleSlot(estate, &scanstate->cstate);
	ExecInitScanTupleSlot(estate, scanstate);
/*	  ExecInitRawTupleSlot(estate, scanstate); */

	/* ----------------
	 *	initialize projection info.  result type comes from scan desc
	 *	below..
	 * ----------------
	 */
	ExecAssignProjectionInfo((Plan *) node, &scanstate->cstate);

	/* --------------------------------
	  *  Part 2)  initialize tid scan state
	  *
	  *  create new TidScanState for node
	  * --------------------------------
	  */
	tidstate = makeNode(TidScanState);
	node->tidstate = tidstate;

	/* ----------------
	 *	assign base id to tid scan state also
	 * ----------------
	 */
	tidstate->cstate.cs_base_id = baseid;
	baseid++;
	estate->es_BaseId = baseid;

	/* ----------------
	 *	get the tid node information
	 * ----------------
	 */
	tidList = (ItemPointer *) palloc(length(node->tideval) * sizeof(ItemPointer));
	numTids = 0;
	if (!node->needRescan)
		numTids = TidListCreate(node->tideval, scanstate->cstate.cs_ExprContext, tidList);
	tidPtr = -1;

	CXT1_printf("ExecInitTidScan: context is %d\n", CurrentMemoryContext);

	tidstate->tss_NumTids = numTids;
	tidstate->tss_TidPtr = tidPtr;
	tidstate->tss_TidList = tidList;

	/* ----------------
	 *	get the range table and direction information
	 *	from the execution state (these are needed to
	 *	open the relations).
	 * ----------------
	 */
	rangeTable = estate->es_range_table;

	/* ----------------
	 *	open the base relation
	 * ----------------
	 */
	relid = node->scan.scanrelid;
	rtentry = rt_fetch(relid, rangeTable);
	reloid = rtentry->relid;

	currentRelation = heap_open(reloid, AccessShareLock);
	if (currentRelation == NULL)
		elog(ERROR, "ExecInitTidScan heap_open failed.");
	scanstate->css_currentRelation = currentRelation;
	scanstate->css_currentScanDesc = 0;

	/* ----------------
	 *	get the scan type from the relation descriptor.
	 * ----------------
	 */
	ExecAssignScanType(scanstate, RelationGetDescr(currentRelation));
	ExecAssignResultTypeFromTL((Plan *) node, &scanstate->cstate);

	/* ----------------
	 *	tid scans don't have subtrees..
	 * ----------------
	 */
/*	  scanstate->ss_ProcOuterFlag = false; */

	tidstate->cstate.cs_TupFromTlist = false;

	/*
	 * if there are some PARAM_EXEC in skankeys then force tid rescan on
	 * first scan.
	 */
	((Plan *) node)->chgParam = execParam;

	/* ----------------
	 *	all done.
	 * ----------------
	 */
	return TRUE;
}

int
ExecCountSlotsTidScan(TidScan *node)
{
	return ExecCountSlotsNode(outerPlan((Plan *) node)) +
	ExecCountSlotsNode(innerPlan((Plan *) node)) + TIDSCAN_NSLOTS;
}
