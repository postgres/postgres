/*-------------------------------------------------------------------------
 *
 * nodeBitmapIndexscan.c
 *	  Routines to support bitmapped index scans of relations
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeBitmapIndexscan.c,v 1.10.2.1 2005/11/22 18:23:09 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		MultiExecBitmapIndexScan	scans a relation using index.
 *		ExecInitBitmapIndexScan		creates and initializes state info.
 *		ExecBitmapIndexReScan		prepares to rescan the plan.
 *		ExecEndBitmapIndexScan		releases all storage.
 */
#include "postgres.h"

#include "access/genam.h"
#include "executor/execdebug.h"
#include "executor/instrument.h"
#include "executor/nodeBitmapIndexscan.h"
#include "executor/nodeIndexscan.h"
#include "miscadmin.h"
#include "utils/memutils.h"


/* ----------------------------------------------------------------
 *		MultiExecBitmapIndexScan(node)
 * ----------------------------------------------------------------
 */
Node *
MultiExecBitmapIndexScan(BitmapIndexScanState *node)
{
#define MAX_TIDS	1024
	TIDBitmap  *tbm;
	IndexScanDesc scandesc;
	ItemPointerData tids[MAX_TIDS];
	int32		ntids;
	double		nTuples = 0;

	/* must provide our own instrumentation support */
	if (node->ss.ps.instrument)
		InstrStartNode(node->ss.ps.instrument);

	/*
	 * extract necessary information from index scan node
	 */
	scandesc = node->biss_ScanDesc;

	/*
	 * If we have runtime keys and they've not already been set up, do it now.
	 */
	if (node->biss_RuntimeKeyInfo && !node->biss_RuntimeKeysReady)
		ExecReScan((PlanState *) node, NULL);

	/*
	 * Prepare the result bitmap.  Normally we just create a new one to pass
	 * back; however, our parent node is allowed to store a pre-made one into
	 * node->biss_result, in which case we just OR our tuple IDs into the
	 * existing bitmap.  (This saves needing explicit UNION steps.)
	 */
	if (node->biss_result)
	{
		tbm = node->biss_result;
		node->biss_result = NULL;		/* reset for next time */
	}
	else
	{
		/* XXX should we use less than work_mem for this? */
		tbm = tbm_create(work_mem * 1024L);
	}

	/*
	 * Get TIDs from index and insert into bitmap
	 */
	for (;;)
	{
		bool		more = index_getmulti(scandesc, tids, MAX_TIDS, &ntids);

		if (ntids > 0)
		{
			tbm_add_tuples(tbm, tids, ntids);
			nTuples += ntids;
		}

		if (!more)
			break;

		CHECK_FOR_INTERRUPTS();
	}

	/* must provide our own instrumentation support */
	if (node->ss.ps.instrument)
		InstrStopNodeMulti(node->ss.ps.instrument, nTuples);

	return (Node *) tbm;
}

/* ----------------------------------------------------------------
 *		ExecBitmapIndexReScan(node)
 *
 *		Recalculates the value of the scan keys whose value depends on
 *		information known at runtime and rescans the indexed relation.
 * ----------------------------------------------------------------
 */
void
ExecBitmapIndexReScan(BitmapIndexScanState *node, ExprContext *exprCtxt)
{
	ExprContext *econtext;
	ExprState **runtimeKeyInfo;

	econtext = node->biss_RuntimeContext;		/* context for runtime keys */
	runtimeKeyInfo = node->biss_RuntimeKeyInfo;

	if (econtext)
	{
		/*
		 * If we are being passed an outer tuple, save it for runtime key
		 * calc.
		 */
		if (exprCtxt != NULL)
			econtext->ecxt_outertuple = exprCtxt->ecxt_outertuple;

		/*
		 * Reset the runtime-key context so we don't leak memory as each outer
		 * tuple is scanned.  Note this assumes that we will recalculate *all*
		 * runtime keys on each call.
		 */
		ResetExprContext(econtext);
	}

	/*
	 * If we are doing runtime key calculations (ie, the index keys depend on
	 * data from an outer scan), compute the new key values
	 */
	if (runtimeKeyInfo)
	{
		ExecIndexEvalRuntimeKeys(econtext,
								 runtimeKeyInfo,
								 node->biss_ScanKeys,
								 node->biss_NumScanKeys);
		node->biss_RuntimeKeysReady = true;
	}

	/* reset index scan */
	index_rescan(node->biss_ScanDesc, node->biss_ScanKeys);
}

/* ----------------------------------------------------------------
 *		ExecEndBitmapIndexScan
 * ----------------------------------------------------------------
 */
void
ExecEndBitmapIndexScan(BitmapIndexScanState *node)
{
	Relation	indexRelationDesc;
	IndexScanDesc indexScanDesc;

	/*
	 * extract information from the node
	 */
	indexRelationDesc = node->biss_RelationDesc;
	indexScanDesc = node->biss_ScanDesc;

	/*
	 * Free the exprcontext ... now dead code, see ExecFreeExprContext
	 */
#ifdef NOT_USED
	if (node->biss_RuntimeContext)
		FreeExprContext(node->biss_RuntimeContext);
#endif

	/*
	 * close the index relation
	 */
	index_endscan(indexScanDesc);
	index_close(indexRelationDesc);
}

/* ----------------------------------------------------------------
 *		ExecInitBitmapIndexScan
 *
 *		Initializes the index scan's state information.
 * ----------------------------------------------------------------
 */
BitmapIndexScanState *
ExecInitBitmapIndexScan(BitmapIndexScan *node, EState *estate)
{
	BitmapIndexScanState *indexstate;
	ScanKey		scanKeys;
	int			numScanKeys;
	ExprState **runtimeKeyInfo;
	bool		have_runtime_keys;

	/*
	 * create state structure
	 */
	indexstate = makeNode(BitmapIndexScanState);
	indexstate->ss.ps.plan = (Plan *) node;
	indexstate->ss.ps.state = estate;

	/* normally we don't make the result bitmap till runtime */
	indexstate->biss_result = NULL;

	/*
	 * Miscellaneous initialization
	 *
	 * We do not need a standard exprcontext for this node, though we may
	 * decide below to create a runtime-key exprcontext
	 */

	/*
	 * initialize child expressions
	 *
	 * We don't need to initialize targetlist or qual since neither are used.
	 *
	 * Note: we don't initialize all of the indexqual expression, only the
	 * sub-parts corresponding to runtime keys (see below).
	 */

#define BITMAPINDEXSCAN_NSLOTS 0

	/*
	 * Initialize index-specific scan state
	 */
	indexstate->biss_RuntimeKeysReady = false;

	CXT1_printf("ExecInitBitmapIndexScan: context is %d\n", CurrentMemoryContext);

	/*
	 * build the index scan keys from the index qualification
	 */
	have_runtime_keys =
		ExecIndexBuildScanKeys((PlanState *) indexstate,
							   node->indexqual,
							   node->indexstrategy,
							   node->indexsubtype,
							   &runtimeKeyInfo,
							   &scanKeys,
							   &numScanKeys);

	indexstate->biss_RuntimeKeyInfo = runtimeKeyInfo;
	indexstate->biss_ScanKeys = scanKeys;
	indexstate->biss_NumScanKeys = numScanKeys;

	/*
	 * If we have runtime keys, we need an ExprContext to evaluate them. We
	 * could just create a "standard" plan node exprcontext, but to keep the
	 * code looking similar to nodeIndexscan.c, it seems better to stick with
	 * the approach of using a separate ExprContext.
	 */
	if (have_runtime_keys)
	{
		ExprContext *stdecontext = indexstate->ss.ps.ps_ExprContext;

		ExecAssignExprContext(estate, &indexstate->ss.ps);
		indexstate->biss_RuntimeContext = indexstate->ss.ps.ps_ExprContext;
		indexstate->ss.ps.ps_ExprContext = stdecontext;
	}
	else
	{
		indexstate->biss_RuntimeContext = NULL;
	}

	/*
	 * We do not open or lock the base relation here.  We assume that an
	 * ancestor BitmapHeapScan node is holding AccessShareLock on the heap
	 * relation throughout the execution of the plan tree.
	 */

	indexstate->ss.ss_currentRelation = NULL;
	indexstate->ss.ss_currentScanDesc = NULL;

	/*
	 * open the index relation and initialize relation and scan descriptors.
	 * Note we acquire no locks here; the index machinery does its own locks
	 * and unlocks.
	 */
	indexstate->biss_RelationDesc = index_open(node->indexid);
	indexstate->biss_ScanDesc =
		index_beginscan_multi(indexstate->biss_RelationDesc,
							  estate->es_snapshot,
							  numScanKeys,
							  scanKeys);

	/*
	 * all done.
	 */
	return indexstate;
}

int
ExecCountSlotsBitmapIndexScan(BitmapIndexScan *node)
{
	return ExecCountSlotsNode(outerPlan((Plan *) node)) +
		ExecCountSlotsNode(innerPlan((Plan *) node)) + BITMAPINDEXSCAN_NSLOTS;
}
