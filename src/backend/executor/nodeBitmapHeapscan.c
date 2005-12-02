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
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeBitmapHeapscan.c,v 1.4.2.1 2005/12/02 01:30:26 tgl Exp $
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
	HeapScanDesc scandesc;
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
	scandesc = node->ss.ss_currentScanDesc;
	scanrelid = ((BitmapHeapScan *) node->ss.ps.plan)->scan.scanrelid;
	tbm = node->tbm;
	tbmres = node->tbmres;

	/*
	 * Clear any reference to the previously returned tuple.  The idea here is
	 * to not have the tuple slot be the last holder of a pin on that tuple's
	 * buffer; if it is, we'll need a separate visit to the bufmgr to release
	 * the buffer.	By clearing here, we get to have the release done by
	 * ReleaseAndReadBuffer, below.
	 */
	ExecClearTuple(slot);

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
			return slot;		/* return empty slot */

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
			 * relation.  (This is probably not necessary given that we got
			 * AccessShareLock before performing any of the indexscans, but
			 * let's be safe.)
			 */
			if (tbmres->blockno >= scandesc->rs_nblocks)
			{
				node->tbmres = tbmres = NULL;
				continue;
			}

			/*
			 * Acquire pin on the current heap page.  We'll hold the pin until
			 * done looking at the page.  We trade in any pin we held before.
			 */
			scandesc->rs_cbuf = ReleaseAndReadBuffer(scandesc->rs_cbuf,
													 scandesc->rs_rd,
													 tbmres->blockno);

			/*
			 * Determine how many entries we need to look at on this page. If
			 * the bitmap is lossy then we need to look at each physical item
			 * pointer; otherwise we just look through the offsets listed in
			 * tbmres.
			 */
			if (tbmres->ntuples >= 0)
			{
				/* non-lossy case */
				node->minslot = 0;
				node->maxslot = tbmres->ntuples - 1;
			}
			else
			{
				/* lossy case */
				Page		dp;

				LockBuffer(scandesc->rs_cbuf, BUFFER_LOCK_SHARE);
				dp = (Page) BufferGetPage(scandesc->rs_cbuf);

				node->minslot = FirstOffsetNumber;
				node->maxslot = PageGetMaxOffsetNumber(dp);

				LockBuffer(scandesc->rs_cbuf, BUFFER_LOCK_UNLOCK);
			}

			/*
			 * Set curslot to first slot to examine
			 */
			node->curslot = node->minslot;
		}
		else
		{
			/*
			 * Continuing in previously obtained page; advance curslot
			 */
			node->curslot++;
		}

		/*
		 * Out of range?  If so, nothing more to look at on this page
		 */
		if (node->curslot < node->minslot || node->curslot > node->maxslot)
		{
			node->tbmres = tbmres = NULL;
			continue;
		}

		/*
		 * Okay to try to fetch the tuple
		 */
		if (tbmres->ntuples >= 0)
		{
			/* non-lossy case */
			targoffset = tbmres->offsets[node->curslot];
		}
		else
		{
			/* lossy case */
			targoffset = (OffsetNumber) node->curslot;
		}

		ItemPointerSet(&scandesc->rs_ctup.t_self, tbmres->blockno, targoffset);

		/*
		 * Fetch the heap tuple and see if it matches the snapshot. We use
		 * heap_release_fetch to avoid useless bufmgr traffic.
		 */
		if (heap_release_fetch(scandesc->rs_rd,
							   scandesc->rs_snapshot,
							   &scandesc->rs_ctup,
							   &scandesc->rs_cbuf,
							   true,
							   &scandesc->rs_pgstat_info))
		{
			/*
			 * Set up the result slot to point to this tuple. Note that the
			 * slot acquires a pin on the buffer.
			 */
			ExecStoreTuple(&scandesc->rs_ctup,
						   slot,
						   scandesc->rs_cbuf,
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
		 * Failed the snap, so loop back and try again.
		 */
	}

	/*
	 * if we get here it means we are at the end of the scan..
	 */
	return ExecClearTuple(slot);
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
	 *
	 * Currently, we do not release the AccessShareLock acquired by
	 * ExecInitBitmapHeapScan.	This lock should be held till end of
	 * transaction. (There is a faction that considers this too much locking,
	 * however.)
	 */
	heap_close(relation, NoLock);
}

/* ----------------------------------------------------------------
 *		ExecInitBitmapHeapScan
 *
 *		Initializes the scan's state information.
 * ----------------------------------------------------------------
 */
BitmapHeapScanState *
ExecInitBitmapHeapScan(BitmapHeapScan *node, EState *estate)
{
	BitmapHeapScanState *scanstate;
	RangeTblEntry *rtentry;
	Index		relid;
	Oid			reloid;
	Relation	currentRelation;

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
	 * open the base relation and acquire AccessShareLock on it.
	 */
	relid = node->scan.scanrelid;
	rtentry = rt_fetch(relid, estate->es_range_table);
	reloid = rtentry->relid;

	currentRelation = heap_open(reloid, AccessShareLock);

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
	outerPlanState(scanstate) = ExecInitNode(outerPlan(node), estate);

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
