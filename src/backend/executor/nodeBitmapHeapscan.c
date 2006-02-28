/*-------------------------------------------------------------------------
 *
 * nodeBitmapHeapscan.c
 *	  Routines to support bitmapped scans of relations
 *
 * NOTE: it is critical that this plan type only be used with MVCC-compliant
 * snapshots (ie, regular snapshots, not SnapshotNow or one of the other
 * special snapshots).	The reason is that since index and heap scans are
 * decoupled, there can be no assurance that the index tuple prompting a
 * visit to a particular heap TID still exists when the visit is made.
 * Therefore the tuple might not exist anymore either (which is OK because
 * heap_fetch will cope) --- but worse, the tuple slot could have been
 * re-used for a newer tuple.  With an MVCC snapshot the newer tuple is
 * certain to fail the time qual and so it will not be mistakenly returned.
 * With SnapshotNow we might return a tuple that doesn't meet the required
 * index qual conditions.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeBitmapHeapscan.c,v 1.9 2006/02/28 04:10:27 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecBitmapHeapScan			scans a relation using bitmap info
 *		ExecBitmapHeapNext			workhorse for above
 *		ExecInitBitmapHeapScan		creates and initializes state info.
 *		ExecBitmapHeapReScan		prepares to rescan the plan.
 *		ExecEndBitmapHeapScan		releases all storage.
 */
#include "postgres.h"

#include "access/heapam.h"
#include "executor/execdebug.h"
#include "executor/nodeBitmapHeapscan.h"
#include "parser/parsetree.h"
#include "pgstat.h"
#include "utils/memutils.h"


static TupleTableSlot *BitmapHeapNext(BitmapHeapScanState *node);
static void bitgetpage(HeapScanDesc scan, TBMIterateResult *tbmres);


/* ----------------------------------------------------------------
 *		BitmapHeapNext
 *
 *		Retrieve next tuple from the BitmapHeapScan node's currentRelation
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
BitmapHeapNext(BitmapHeapScanState *node)
{
	EState	   *estate;
	ExprContext *econtext;
	HeapScanDesc scan;
	Index		scanrelid;
	TIDBitmap  *tbm;
	TBMIterateResult *tbmres;
	OffsetNumber targoffset;
	TupleTableSlot *slot;

	/*
	 * extract necessary information from index scan node
	 */
	estate = node->ss.ps.state;
	econtext = node->ss.ps.ps_ExprContext;
	slot = node->ss.ss_ScanTupleSlot;
	scan = node->ss.ss_currentScanDesc;
	scanrelid = ((BitmapHeapScan *) node->ss.ps.plan)->scan.scanrelid;
	tbm = node->tbm;
	tbmres = node->tbmres;

	/*
	 * Check if we are evaluating PlanQual for tuple of this relation.
	 * Additional checking is not good, but no other way for now. We could
	 * introduce new nodes for this case and handle IndexScan --> NewNode
	 * switching in Init/ReScan plan...
	 */
	if (estate->es_evTuple != NULL &&
		estate->es_evTuple[scanrelid - 1] != NULL)
	{
		if (estate->es_evTupleNull[scanrelid - 1])
			return ExecClearTuple(slot);

		ExecStoreTuple(estate->es_evTuple[scanrelid - 1],
					   slot, InvalidBuffer, false);

		/* Does the tuple meet the original qual conditions? */
		econtext->ecxt_scantuple = slot;

		ResetExprContext(econtext);

		if (!ExecQual(node->bitmapqualorig, econtext, false))
			ExecClearTuple(slot);		/* would not be returned by scan */

		/* Flag for the next call that no more tuples */
		estate->es_evTupleNull[scanrelid - 1] = true;

		return slot;
	}

	/*
	 * If we haven't yet performed the underlying index scan, do it, and
	 * prepare the bitmap to be iterated over.
	 */
	if (tbm == NULL)
	{
		tbm = (TIDBitmap *) MultiExecProcNode(outerPlanState(node));

		if (!tbm || !IsA(tbm, TIDBitmap))
			elog(ERROR, "unrecognized result from subplan");

		node->tbm = tbm;
		node->tbmres = tbmres = NULL;

		tbm_begin_iterate(tbm);
	}

	for (;;)
	{
		Page		dp;
		ItemId		lp;

		/*
		 * Get next page of results if needed
		 */
		if (tbmres == NULL)
		{
			node->tbmres = tbmres = tbm_iterate(tbm);
			if (tbmres == NULL)
			{
				/* no more entries in the bitmap */
				break;
			}

			/*
			 * Ignore any claimed entries past what we think is the end of the
			 * relation.  (This is probably not necessary given that we got at
			 * least AccessShareLock on the table before performing any of the
			 * indexscans, but let's be safe.)
			 */
			if (tbmres->blockno >= scan->rs_nblocks)
			{
				node->tbmres = tbmres = NULL;
				continue;
			}

			/*
			 * Fetch the current heap page and identify candidate tuples.
			 */
			bitgetpage(scan, tbmres);

			/*
			 * Set rs_cindex to first slot to examine
			 */
			scan->rs_cindex = 0;
		}
		else
		{
			/*
			 * Continuing in previously obtained page; advance rs_cindex
			 */
			scan->rs_cindex++;
		}

		/*
		 * Out of range?  If so, nothing more to look at on this page
		 */
		if (scan->rs_cindex < 0 || scan->rs_cindex >= scan->rs_ntuples)
		{
			node->tbmres = tbmres = NULL;
			continue;
		}

		/*
		 * Okay to fetch the tuple
		 */
		targoffset = scan->rs_vistuples[scan->rs_cindex];
		dp = (Page) BufferGetPage(scan->rs_cbuf);
		lp = PageGetItemId(dp, targoffset);
		Assert(ItemIdIsUsed(lp));

		scan->rs_ctup.t_data = (HeapTupleHeader) PageGetItem((Page) dp, lp);
		scan->rs_ctup.t_len = ItemIdGetLength(lp);
		ItemPointerSet(&scan->rs_ctup.t_self, tbmres->blockno, targoffset);

		pgstat_count_heap_fetch(&scan->rs_pgstat_info);

		/*
		 * Set up the result slot to point to this tuple. Note that the
		 * slot acquires a pin on the buffer.
		 */
		ExecStoreTuple(&scan->rs_ctup,
					   slot,
					   scan->rs_cbuf,
					   false);

		/*
		 * If we are using lossy info, we have to recheck the qual
		 * conditions at every tuple.
		 */
		if (tbmres->ntuples < 0)
		{
			econtext->ecxt_scantuple = slot;
			ResetExprContext(econtext);

			if (!ExecQual(node->bitmapqualorig, econtext, false))
			{
				/* Fails recheck, so drop it and loop back for another */
				ExecClearTuple(slot);
				continue;
			}
		}

		/* OK to return this tuple */
		return slot;
	}

	/*
	 * if we get here it means we are at the end of the scan..
	 */
	return ExecClearTuple(slot);
}

/*
 * bitgetpage - subroutine for BitmapHeapNext()
 *
 * This routine reads and pins the specified page of the relation, then
 * builds an array indicating which tuples on the page are both potentially
 * interesting according to the bitmap, and visible according to the snapshot.
 */
static void
bitgetpage(HeapScanDesc scan, TBMIterateResult *tbmres)
{
	BlockNumber	page = tbmres->blockno;
	Buffer		buffer;
	Snapshot	snapshot;
	Page		dp;
	int			ntup;
	int			curslot;
	int			minslot;
	int			maxslot;
	int			maxoff;

	/*
	 * Acquire pin on the target heap page, trading in any pin we held before.
	 */
	Assert(page < scan->rs_nblocks);

	scan->rs_cbuf = ReleaseAndReadBuffer(scan->rs_cbuf,
										 scan->rs_rd,
										 page);
	buffer = scan->rs_cbuf;
	snapshot = scan->rs_snapshot;

	/*
	 * We must hold share lock on the buffer content while examining
	 * tuple visibility.  Afterwards, however, the tuples we have found
	 * to be visible are guaranteed good as long as we hold the buffer pin.
	 */
	LockBuffer(buffer, BUFFER_LOCK_SHARE);

	dp = (Page) BufferGetPage(buffer);
	maxoff = PageGetMaxOffsetNumber(dp);

	/*
	 * Determine how many entries we need to look at on this page. If
	 * the bitmap is lossy then we need to look at each physical item
	 * pointer; otherwise we just look through the offsets listed in
	 * tbmres.
	 */
	if (tbmres->ntuples >= 0)
	{
		/* non-lossy case */
		minslot = 0;
		maxslot = tbmres->ntuples - 1;
	}
	else
	{
		/* lossy case */
		minslot = FirstOffsetNumber;
		maxslot = maxoff;
	}

	ntup = 0;
	for (curslot = minslot; curslot <= maxslot; curslot++)
	{
		OffsetNumber targoffset;
		ItemId		lp;
		HeapTupleData loctup;
		bool		valid;

		if (tbmres->ntuples >= 0)
		{
			/* non-lossy case */
			targoffset = tbmres->offsets[curslot];
		}
		else
		{
			/* lossy case */
			targoffset = (OffsetNumber) curslot;
		}

		/*
		 * We'd better check for out-of-range offnum in case of VACUUM since
		 * the TID was obtained.
		 */
		if (targoffset < FirstOffsetNumber || targoffset > maxoff)
			continue;

		lp = PageGetItemId(dp, targoffset);

		/*
		 * Must check for deleted tuple.
		 */
		if (!ItemIdIsUsed(lp))
			continue;

		/*
		 * check time qualification of tuple, remember it if valid
		 */
		loctup.t_data = (HeapTupleHeader) PageGetItem((Page) dp, lp);
		loctup.t_len = ItemIdGetLength(lp);
		ItemPointerSet(&(loctup.t_self), page, targoffset);

		valid = HeapTupleSatisfiesVisibility(&loctup, snapshot, buffer);
		if (valid)
			scan->rs_vistuples[ntup++] = targoffset;
	}

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	Assert(ntup <= MaxHeapTuplesPerPage);
	scan->rs_ntuples = ntup;
}

/* ----------------------------------------------------------------
 *		ExecBitmapHeapScan(node)
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecBitmapHeapScan(BitmapHeapScanState *node)
{
	/*
	 * use BitmapHeapNext as access method
	 */
	return ExecScan(&node->ss, (ExecScanAccessMtd) BitmapHeapNext);
}

/* ----------------------------------------------------------------
 *		ExecBitmapHeapReScan(node)
 * ----------------------------------------------------------------
 */
void
ExecBitmapHeapReScan(BitmapHeapScanState *node, ExprContext *exprCtxt)
{
	EState	   *estate;
	Index		scanrelid;

	estate = node->ss.ps.state;
	scanrelid = ((BitmapHeapScan *) node->ss.ps.plan)->scan.scanrelid;

	/*
	 * If we are being passed an outer tuple, link it into the "regular"
	 * per-tuple econtext for possible qual eval.
	 */
	if (exprCtxt != NULL)
	{
		ExprContext *stdecontext;

		stdecontext = node->ss.ps.ps_ExprContext;
		stdecontext->ecxt_outertuple = exprCtxt->ecxt_outertuple;
	}

	/* If this is re-scanning of PlanQual ... */
	if (estate->es_evTuple != NULL &&
		estate->es_evTuple[scanrelid - 1] != NULL)
	{
		estate->es_evTupleNull[scanrelid - 1] = false;
	}

	/* rescan to release any page pin */
	heap_rescan(node->ss.ss_currentScanDesc, NULL);

	/* undo bogus "seq scan" count (see notes in ExecInitBitmapHeapScan) */
	pgstat_discount_heap_scan(&node->ss.ss_currentScanDesc->rs_pgstat_info);

	if (node->tbm)
		tbm_free(node->tbm);
	node->tbm = NULL;
	node->tbmres = NULL;

	/*
	 * Always rescan the input immediately, to ensure we can pass down any
	 * outer tuple that might be used in index quals.
	 */
	ExecReScan(outerPlanState(node), exprCtxt);
}

/* ----------------------------------------------------------------
 *		ExecEndBitmapHeapScan
 * ----------------------------------------------------------------
 */
void
ExecEndBitmapHeapScan(BitmapHeapScanState *node)
{
	Relation	relation;
	HeapScanDesc scanDesc;

	/*
	 * extract information from the node
	 */
	relation = node->ss.ss_currentRelation;
	scanDesc = node->ss.ss_currentScanDesc;

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
	 * close down subplans
	 */
	ExecEndNode(outerPlanState(node));

	/*
	 * release bitmap if any
	 */
	if (node->tbm)
		tbm_free(node->tbm);

	/*
	 * close heap scan
	 */
	heap_endscan(scanDesc);

	/*
	 * close the heap relation.
	 */
	ExecCloseScanRelation(relation);
}

/* ----------------------------------------------------------------
 *		ExecInitBitmapHeapScan
 *
 *		Initializes the scan's state information.
 * ----------------------------------------------------------------
 */
BitmapHeapScanState *
ExecInitBitmapHeapScan(BitmapHeapScan *node, EState *estate, int eflags)
{
	BitmapHeapScanState *scanstate;
	Relation	currentRelation;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * Assert caller didn't ask for an unsafe snapshot --- see comments
	 * at head of file.
	 */
	Assert(IsMVCCSnapshot(estate->es_snapshot));

	/*
	 * create state structure
	 */
	scanstate = makeNode(BitmapHeapScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;

	scanstate->tbm = NULL;
	scanstate->tbmres = NULL;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	/*
	 * initialize child expressions
	 */
	scanstate->ss.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->scan.plan.targetlist,
					 (PlanState *) scanstate);
	scanstate->ss.ps.qual = (List *)
		ExecInitExpr((Expr *) node->scan.plan.qual,
					 (PlanState *) scanstate);
	scanstate->bitmapqualorig = (List *)
		ExecInitExpr((Expr *) node->bitmapqualorig,
					 (PlanState *) scanstate);

#define BITMAPHEAPSCAN_NSLOTS 2

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &scanstate->ss.ps);
	ExecInitScanTupleSlot(estate, &scanstate->ss);

	CXT1_printf("ExecInitBitmapHeapScan: context is %d\n", CurrentMemoryContext);

	/*
	 * open the base relation and acquire appropriate lock on it.
	 */
	currentRelation = ExecOpenScanRelation(estate, node->scan.scanrelid);

	scanstate->ss.ss_currentRelation = currentRelation;

	/*
	 * Even though we aren't going to do a conventional seqscan, it is useful
	 * to create a HeapScanDesc --- this checks the relation size and sets up
	 * statistical infrastructure for us.
	 */
	scanstate->ss.ss_currentScanDesc = heap_beginscan(currentRelation,
													  estate->es_snapshot,
													  0,
													  NULL);

	/*
	 * One problem is that heap_beginscan counts a "sequential scan" start,
	 * when we actually aren't doing any such thing.  Reverse out the added
	 * scan count.	(Eventually we may want to count bitmap scans separately.)
	 */
	pgstat_discount_heap_scan(&scanstate->ss.ss_currentScanDesc->rs_pgstat_info);

	/*
	 * get the scan type from the relation descriptor.
	 */
	ExecAssignScanType(&scanstate->ss, RelationGetDescr(currentRelation), false);

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&scanstate->ss.ps);
	ExecAssignScanProjectionInfo(&scanstate->ss);

	/*
	 * initialize child nodes
	 *
	 * We do this last because the child nodes will open indexscans on our
	 * relation's indexes, and we want to be sure we have acquired a lock
	 * on the relation first.
	 */
	outerPlanState(scanstate) = ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * all done.
	 */
	return scanstate;
}

int
ExecCountSlotsBitmapHeapScan(BitmapHeapScan *node)
{
	return ExecCountSlotsNode(outerPlan((Plan *) node)) +
		ExecCountSlotsNode(innerPlan((Plan *) node)) + BITMAPHEAPSCAN_NSLOTS;
}
