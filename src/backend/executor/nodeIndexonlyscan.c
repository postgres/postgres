/*-------------------------------------------------------------------------
 *
 * nodeIndexonlyscan.c
 *	  Routines to support index-only scans
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeIndexonlyscan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecIndexOnlyScan			scans an index
 *		IndexOnlyNext				retrieve next tuple
 *		ExecInitIndexOnlyScan		creates and initializes state info.
 *		ExecReScanIndexOnlyScan		rescans the indexed relation.
 *		ExecEndIndexOnlyScan		releases all storage.
 *		ExecIndexOnlyMarkPos		marks scan position.
 *		ExecIndexOnlyRestrPos		restores scan position.
 */
#include "postgres.h"

#include "access/relscan.h"
#include "access/visibilitymap.h"
#include "executor/execdebug.h"
#include "executor/nodeIndexonlyscan.h"
#include "executor/nodeIndexscan.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
#include "utils/memutils.h"
#include "utils/rel.h"


static TupleTableSlot *IndexOnlyNext(IndexOnlyScanState *node);
static void StoreIndexTuple(TupleTableSlot *slot, IndexTuple itup,
				TupleDesc itupdesc);


/* ----------------------------------------------------------------
 *		IndexOnlyNext
 *
 *		Retrieve a tuple from the IndexOnlyScan node's index.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
IndexOnlyNext(IndexOnlyScanState *node)
{
	EState	   *estate;
	ExprContext *econtext;
	ScanDirection direction;
	IndexScanDesc scandesc;
	TupleTableSlot *slot;
	ItemPointer tid;

	/*
	 * extract necessary information from index scan node
	 */
	estate = node->ss.ps.state;
	direction = estate->es_direction;
	/* flip direction if this is an overall backward scan */
	if (ScanDirectionIsBackward(((IndexOnlyScan *) node->ss.ps.plan)->indexorderdir))
	{
		if (ScanDirectionIsForward(direction))
			direction = BackwardScanDirection;
		else if (ScanDirectionIsBackward(direction))
			direction = ForwardScanDirection;
	}
	scandesc = node->ioss_ScanDesc;
	econtext = node->ss.ps.ps_ExprContext;
	slot = node->ss.ss_ScanTupleSlot;

	/*
	 * OK, now that we have what we need, fetch the next tuple.
	 */
	while ((tid = index_getnext_tid(scandesc, direction)) != NULL)
	{
		HeapTuple	tuple = NULL;

		/*
		 * We can skip the heap fetch if the TID references a heap page on
		 * which all tuples are known visible to everybody.  In any case,
		 * we'll use the index tuple not the heap tuple as the data source.
		 *
		 * Note on Memory Ordering Effects: visibilitymap_get_status does not
		 * lock the visibility map buffer, and therefore the result we read
		 * here could be slightly stale.  However, it can't be stale enough to
		 * matter.
		 *
		 * We need to detect clearing a VM bit due to an insert right away,
		 * because the tuple is present in the index page but not visible. The
		 * reading of the TID by this scan (using a shared lock on the index
		 * buffer) is serialized with the insert of the TID into the index
		 * (using an exclusive lock on the index buffer). Because the VM bit
		 * is cleared before updating the index, and locking/unlocking of the
		 * index page acts as a full memory barrier, we are sure to see the
		 * cleared bit if we see a recently-inserted TID.
		 *
		 * Deletes do not update the index page (only VACUUM will clear out
		 * the TID), so the clearing of the VM bit by a delete is not
		 * serialized with this test below, and we may see a value that is
		 * significantly stale. However, we don't care about the delete right
		 * away, because the tuple is still visible until the deleting
		 * transaction commits or the statement ends (if it's our
		 * transaction). In either case, the lock on the VM buffer will have
		 * been released (acting as a write barrier) after clearing the bit.
		 * And for us to have a snapshot that includes the deleting
		 * transaction (making the tuple invisible), we must have acquired
		 * ProcArrayLock after that time, acting as a read barrier.
		 *
		 * It's worth going through this complexity to avoid needing to lock
		 * the VM buffer, which could cause significant contention.
		 */
		if (!VM_ALL_VISIBLE(scandesc->heapRelation,
							ItemPointerGetBlockNumber(tid),
							&node->ioss_VMBuffer))
		{
			/*
			 * Rats, we have to visit the heap to check visibility.
			 */
			node->ioss_HeapFetches++;
			tuple = index_fetch_heap(scandesc);
			if (tuple == NULL)
				continue;		/* no visible tuple, try next index entry */

			/*
			 * Only MVCC snapshots are supported here, so there should be no
			 * need to keep following the HOT chain once a visible entry has
			 * been found.  If we did want to allow that, we'd need to keep
			 * more state to remember not to call index_getnext_tid next time.
			 */
			if (scandesc->xs_continue_hot)
				elog(ERROR, "non-MVCC snapshots are not supported in index-only scans");

			/*
			 * Note: at this point we are holding a pin on the heap page, as
			 * recorded in scandesc->xs_cbuf.  We could release that pin now,
			 * but it's not clear whether it's a win to do so.  The next index
			 * entry might require a visit to the same heap page.
			 */
		}

		/*
		 * Fill the scan tuple slot with data from the index.
		 */
		StoreIndexTuple(slot, scandesc->xs_itup, scandesc->xs_itupdesc);

		/*
		 * If the index was lossy, we have to recheck the index quals.
		 * (Currently, this can never happen, but we should support the case
		 * for possible future use, eg with GiST indexes.)
		 */
		if (scandesc->xs_recheck)
		{
			econtext->ecxt_scantuple = slot;
			ResetExprContext(econtext);
			if (!ExecQual(node->indexqual, econtext, false))
			{
				/* Fails recheck, so drop it and loop back for another */
				InstrCountFiltered2(node, 1);
				continue;
			}
		}

		/*
		 * We don't currently support rechecking ORDER BY distances.  (In
		 * principle, if the index can support retrieval of the originally
		 * indexed value, it should be able to produce an exact distance
		 * calculation too.  So it's not clear that adding code here for
		 * recheck/re-sort would be worth the trouble.  But we should at least
		 * throw an error if someone tries it.)
		 */
		if (scandesc->numberOfOrderBys > 0 && scandesc->xs_recheckorderby)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("lossy distance functions are not supported in index-only scans")));

		/*
		 * Predicate locks for index-only scans must be acquired at the page
		 * level when the heap is not accessed, since tuple-level predicate
		 * locks need the tuple's xmin value.  If we had to visit the tuple
		 * anyway, then we already have the tuple-level lock and can skip the
		 * page lock.
		 */
		if (tuple == NULL)
			PredicateLockPage(scandesc->heapRelation,
							  ItemPointerGetBlockNumber(tid),
							  estate->es_snapshot);

		return slot;
	}

	/*
	 * if we get here it means the index scan failed so we are at the end of
	 * the scan..
	 */
	return ExecClearTuple(slot);
}

/*
 * StoreIndexTuple
 *		Fill the slot with data from the index tuple.
 *
 * At some point this might be generally-useful functionality, but
 * right now we don't need it elsewhere.
 */
static void
StoreIndexTuple(TupleTableSlot *slot, IndexTuple itup, TupleDesc itupdesc)
{
	int			nindexatts = itupdesc->natts;
	Datum	   *values = slot->tts_values;
	bool	   *isnull = slot->tts_isnull;
	int			i;

	/*
	 * Note: we must use the tupdesc supplied by the AM in index_getattr, not
	 * the slot's tupdesc, in case the latter has different datatypes (this
	 * happens for btree name_ops in particular).  They'd better have the same
	 * number of columns though, as well as being datatype-compatible which is
	 * something we can't so easily check.
	 */
	Assert(slot->tts_tupleDescriptor->natts == nindexatts);

	ExecClearTuple(slot);
	for (i = 0; i < nindexatts; i++)
		values[i] = index_getattr(itup, i + 1, itupdesc, &isnull[i]);
	ExecStoreVirtualTuple(slot);
}

/*
 * IndexOnlyRecheck -- access method routine to recheck a tuple in EvalPlanQual
 *
 * This can't really happen, since an index can't supply CTID which would
 * be necessary data for any potential EvalPlanQual target relation.  If it
 * did happen, the EPQ code would pass us the wrong data, namely a heap
 * tuple not an index tuple.  So throw an error.
 */
static bool
IndexOnlyRecheck(IndexOnlyScanState *node, TupleTableSlot *slot)
{
	elog(ERROR, "EvalPlanQual recheck is not supported in index-only scans");
	return false;				/* keep compiler quiet */
}

/* ----------------------------------------------------------------
 *		ExecIndexOnlyScan(node)
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecIndexOnlyScan(IndexOnlyScanState *node)
{
	/*
	 * If we have runtime keys and they've not already been set up, do it now.
	 */
	if (node->ioss_NumRuntimeKeys != 0 && !node->ioss_RuntimeKeysReady)
		ExecReScan((PlanState *) node);

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) IndexOnlyNext,
					(ExecScanRecheckMtd) IndexOnlyRecheck);
}

/* ----------------------------------------------------------------
 *		ExecReScanIndexOnlyScan(node)
 *
 *		Recalculates the values of any scan keys whose value depends on
 *		information known at runtime, then rescans the indexed relation.
 *
 *		Updating the scan key was formerly done separately in
 *		ExecUpdateIndexScanKeys. Integrating it into ReScan makes
 *		rescans of indices and relations/general streams more uniform.
 * ----------------------------------------------------------------
 */
void
ExecReScanIndexOnlyScan(IndexOnlyScanState *node)
{
	/*
	 * If we are doing runtime key calculations (ie, any of the index key
	 * values weren't simple Consts), compute the new key values.  But first,
	 * reset the context so we don't leak memory as each outer tuple is
	 * scanned.  Note this assumes that we will recalculate *all* runtime keys
	 * on each call.
	 */
	if (node->ioss_NumRuntimeKeys != 0)
	{
		ExprContext *econtext = node->ioss_RuntimeContext;

		ResetExprContext(econtext);
		ExecIndexEvalRuntimeKeys(econtext,
								 node->ioss_RuntimeKeys,
								 node->ioss_NumRuntimeKeys);
	}
	node->ioss_RuntimeKeysReady = true;

	/* reset index scan */
	index_rescan(node->ioss_ScanDesc,
				 node->ioss_ScanKeys, node->ioss_NumScanKeys,
				 node->ioss_OrderByKeys, node->ioss_NumOrderByKeys);

	ExecScanReScan(&node->ss);
}


/* ----------------------------------------------------------------
 *		ExecEndIndexOnlyScan
 * ----------------------------------------------------------------
 */
void
ExecEndIndexOnlyScan(IndexOnlyScanState *node)
{
	Relation	indexRelationDesc;
	IndexScanDesc indexScanDesc;
	Relation	relation;

	/*
	 * extract information from the node
	 */
	indexRelationDesc = node->ioss_RelationDesc;
	indexScanDesc = node->ioss_ScanDesc;
	relation = node->ss.ss_currentRelation;

	/* Release VM buffer pin, if any. */
	if (node->ioss_VMBuffer != InvalidBuffer)
	{
		ReleaseBuffer(node->ioss_VMBuffer);
		node->ioss_VMBuffer = InvalidBuffer;
	}

	/*
	 * Free the exprcontext(s) ... now dead code, see ExecFreeExprContext
	 */
#ifdef NOT_USED
	ExecFreeExprContext(&node->ss.ps);
	if (node->ioss_RuntimeContext)
		FreeExprContext(node->ioss_RuntimeContext, true);
#endif

	/*
	 * clear out tuple table slots
	 */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/*
	 * close the index relation (no-op if we didn't open it)
	 */
	if (indexScanDesc)
		index_endscan(indexScanDesc);
	if (indexRelationDesc)
		index_close(indexRelationDesc, NoLock);

	/*
	 * close the heap relation.
	 */
	ExecCloseScanRelation(relation);
}

/* ----------------------------------------------------------------
 *		ExecIndexOnlyMarkPos
 * ----------------------------------------------------------------
 */
void
ExecIndexOnlyMarkPos(IndexOnlyScanState *node)
{
	index_markpos(node->ioss_ScanDesc);
}

/* ----------------------------------------------------------------
 *		ExecIndexOnlyRestrPos
 * ----------------------------------------------------------------
 */
void
ExecIndexOnlyRestrPos(IndexOnlyScanState *node)
{
	index_restrpos(node->ioss_ScanDesc);
}

/* ----------------------------------------------------------------
 *		ExecInitIndexOnlyScan
 *
 *		Initializes the index scan's state information, creates
 *		scan keys, and opens the base and index relations.
 *
 *		Note: index scans have 2 sets of state information because
 *			  we have to keep track of the base relation and the
 *			  index relation.
 * ----------------------------------------------------------------
 */
IndexOnlyScanState *
ExecInitIndexOnlyScan(IndexOnlyScan *node, EState *estate, int eflags)
{
	IndexOnlyScanState *indexstate;
	Relation	currentRelation;
	bool		relistarget;
	TupleDesc	tupDesc;

	/*
	 * create state structure
	 */
	indexstate = makeNode(IndexOnlyScanState);
	indexstate->ss.ps.plan = (Plan *) node;
	indexstate->ss.ps.state = estate;
	indexstate->ioss_HeapFetches = 0;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &indexstate->ss.ps);

	indexstate->ss.ps.ps_TupFromTlist = false;

	/*
	 * initialize child expressions
	 *
	 * Note: we don't initialize all of the indexorderby expression, only the
	 * sub-parts corresponding to runtime keys (see below).
	 */
	indexstate->ss.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->scan.plan.targetlist,
					 (PlanState *) indexstate);
	indexstate->ss.ps.qual = (List *)
		ExecInitExpr((Expr *) node->scan.plan.qual,
					 (PlanState *) indexstate);
	indexstate->indexqual = (List *)
		ExecInitExpr((Expr *) node->indexqual,
					 (PlanState *) indexstate);

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &indexstate->ss.ps);
	ExecInitScanTupleSlot(estate, &indexstate->ss);

	/*
	 * open the base relation and acquire appropriate lock on it.
	 */
	currentRelation = ExecOpenScanRelation(estate, node->scan.scanrelid, eflags);

	indexstate->ss.ss_currentRelation = currentRelation;
	indexstate->ss.ss_currentScanDesc = NULL;	/* no heap scan here */

	/*
	 * Build the scan tuple type using the indextlist generated by the
	 * planner.  We use this, rather than the index's physical tuple
	 * descriptor, because the latter contains storage column types not the
	 * types of the original datums.  (It's the AM's responsibility to return
	 * suitable data anyway.)
	 */
	tupDesc = ExecTypeFromTL(node->indextlist, false);
	ExecAssignScanType(&indexstate->ss, tupDesc);

	/*
	 * Initialize result tuple type and projection info.  The node's
	 * targetlist will contain Vars with varno = INDEX_VAR, referencing the
	 * scan tuple.
	 */
	ExecAssignResultTypeFromTL(&indexstate->ss.ps);
	ExecAssignScanProjectionInfoWithVarno(&indexstate->ss, INDEX_VAR);

	/*
	 * If we are just doing EXPLAIN (ie, aren't going to run the plan), stop
	 * here.  This allows an index-advisor plugin to EXPLAIN a plan containing
	 * references to nonexistent indexes.
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return indexstate;

	/*
	 * Open the index relation.
	 *
	 * If the parent table is one of the target relations of the query, then
	 * InitPlan already opened and write-locked the index, so we can avoid
	 * taking another lock here.  Otherwise we need a normal reader's lock.
	 */
	relistarget = ExecRelationIsTargetRelation(estate, node->scan.scanrelid);
	indexstate->ioss_RelationDesc = index_open(node->indexid,
									 relistarget ? NoLock : AccessShareLock);

	/*
	 * Initialize index-specific scan state
	 */
	indexstate->ioss_RuntimeKeysReady = false;
	indexstate->ioss_RuntimeKeys = NULL;
	indexstate->ioss_NumRuntimeKeys = 0;

	/*
	 * build the index scan keys from the index qualification
	 */
	ExecIndexBuildScanKeys((PlanState *) indexstate,
						   indexstate->ioss_RelationDesc,
						   node->indexqual,
						   false,
						   &indexstate->ioss_ScanKeys,
						   &indexstate->ioss_NumScanKeys,
						   &indexstate->ioss_RuntimeKeys,
						   &indexstate->ioss_NumRuntimeKeys,
						   NULL,	/* no ArrayKeys */
						   NULL);

	/*
	 * any ORDER BY exprs have to be turned into scankeys in the same way
	 */
	ExecIndexBuildScanKeys((PlanState *) indexstate,
						   indexstate->ioss_RelationDesc,
						   node->indexorderby,
						   true,
						   &indexstate->ioss_OrderByKeys,
						   &indexstate->ioss_NumOrderByKeys,
						   &indexstate->ioss_RuntimeKeys,
						   &indexstate->ioss_NumRuntimeKeys,
						   NULL,	/* no ArrayKeys */
						   NULL);

	/*
	 * If we have runtime keys, we need an ExprContext to evaluate them. The
	 * node's standard context won't do because we want to reset that context
	 * for every tuple.  So, build another context just like the other one...
	 * -tgl 7/11/00
	 */
	if (indexstate->ioss_NumRuntimeKeys != 0)
	{
		ExprContext *stdecontext = indexstate->ss.ps.ps_ExprContext;

		ExecAssignExprContext(estate, &indexstate->ss.ps);
		indexstate->ioss_RuntimeContext = indexstate->ss.ps.ps_ExprContext;
		indexstate->ss.ps.ps_ExprContext = stdecontext;
	}
	else
	{
		indexstate->ioss_RuntimeContext = NULL;
	}

	/*
	 * Initialize scan descriptor.
	 */
	indexstate->ioss_ScanDesc = index_beginscan(currentRelation,
												indexstate->ioss_RelationDesc,
												estate->es_snapshot,
												indexstate->ioss_NumScanKeys,
											indexstate->ioss_NumOrderByKeys);

	/* Set it up for index-only scan */
	indexstate->ioss_ScanDesc->xs_want_itup = true;
	indexstate->ioss_VMBuffer = InvalidBuffer;

	/*
	 * If no run-time keys to calculate, go ahead and pass the scankeys to the
	 * index AM.
	 */
	if (indexstate->ioss_NumRuntimeKeys == 0)
		index_rescan(indexstate->ioss_ScanDesc,
					 indexstate->ioss_ScanKeys,
					 indexstate->ioss_NumScanKeys,
					 indexstate->ioss_OrderByKeys,
					 indexstate->ioss_NumOrderByKeys);

	/*
	 * all done.
	 */
	return indexstate;
}
