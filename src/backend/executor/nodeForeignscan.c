/*-------------------------------------------------------------------------
 *
 * nodeForeignscan.c
 *	  Routines to support scans of foreign tables
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeForeignscan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *
 *		ExecForeignScan			scans a foreign table.
 *		ExecInitForeignScan		creates and initializes state info.
 *		ExecReScanForeignScan	rescans the foreign relation.
 *		ExecEndForeignScan		releases any resources allocated.
 */
#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeForeignscan.h"
#include "foreign/fdwapi.h"
#include "utils/memutils.h"
#include "utils/rel.h"

static TupleTableSlot *ForeignNext(ForeignScanState *node);
static bool ForeignRecheck(ForeignScanState *node, TupleTableSlot *slot);


/* ----------------------------------------------------------------
 *		ForeignNext
 *
 *		This is a workhorse for ExecForeignScan
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ForeignNext(ForeignScanState *node)
{
	TupleTableSlot *slot;
	ForeignScan *plan = (ForeignScan *) node->ss.ps.plan;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	MemoryContext oldcontext;

	/* Call the Iterate function in short-lived context */
	oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
	if (plan->operation != CMD_SELECT)
	{
		/*
		 * direct modifications cannot be re-evaluated, so shouldn't get here
		 * during EvalPlanQual processing
		 */
		Assert(node->ss.ps.state->es_epq_active == NULL);

		slot = node->fdwroutine->IterateDirectModify(node);
	}
	else
		slot = node->fdwroutine->IterateForeignScan(node);
	MemoryContextSwitchTo(oldcontext);

	/*
	 * Insert valid value into tableoid, the only actually-useful system
	 * column.
	 */
	if (plan->fsSystemCol && !TupIsNull(slot))
		slot->tts_tableOid = RelationGetRelid(node->ss.ss_currentRelation);

	return slot;
}

/*
 * ForeignRecheck -- access method routine to recheck a tuple in EvalPlanQual
 */
static bool
ForeignRecheck(ForeignScanState *node, TupleTableSlot *slot)
{
	FdwRoutine *fdwroutine = node->fdwroutine;
	ExprContext *econtext;

	/*
	 * extract necessary information from foreign scan node
	 */
	econtext = node->ss.ps.ps_ExprContext;

	/* Does the tuple meet the remote qual condition? */
	econtext->ecxt_scantuple = slot;

	ResetExprContext(econtext);

	/*
	 * If an outer join is pushed down, RecheckForeignScan may need to store a
	 * different tuple in the slot, because a different set of columns may go
	 * to NULL upon recheck.  Otherwise, it shouldn't need to change the slot
	 * contents, just return true or false to indicate whether the quals still
	 * pass.  For simple cases, setting fdw_recheck_quals may be easier than
	 * providing this callback.
	 */
	if (fdwroutine->RecheckForeignScan &&
		!fdwroutine->RecheckForeignScan(node, slot))
		return false;

	return ExecQual(node->fdw_recheck_quals, econtext);
}

/* ----------------------------------------------------------------
 *		ExecForeignScan(node)
 *
 *		Fetches the next tuple from the FDW, checks local quals, and
 *		returns it.
 *		We call the ExecScan() routine and pass it the appropriate
 *		access method functions.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecForeignScan(PlanState *pstate)
{
	ForeignScanState *node = castNode(ForeignScanState, pstate);
	ForeignScan *plan = (ForeignScan *) node->ss.ps.plan;
	EState	   *estate = node->ss.ps.state;

	/*
	 * Ignore direct modifications when EvalPlanQual is active --- they are
	 * irrelevant for EvalPlanQual rechecking
	 */
	if (estate->es_epq_active != NULL && plan->operation != CMD_SELECT)
		return NULL;

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) ForeignNext,
					(ExecScanRecheckMtd) ForeignRecheck);
}


/* ----------------------------------------------------------------
 *		ExecInitForeignScan
 * ----------------------------------------------------------------
 */
ForeignScanState *
ExecInitForeignScan(ForeignScan *node, EState *estate, int eflags)
{
	ForeignScanState *scanstate;
	Relation	currentRelation = NULL;
	Index		scanrelid = node->scan.scanrelid;
	int			tlistvarno;
	FdwRoutine *fdwroutine;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * create state structure
	 */
	scanstate = makeNode(ForeignScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;
	scanstate->ss.ps.ExecProcNode = ExecForeignScan;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	/*
	 * open the scan relation, if any; also acquire function pointers from the
	 * FDW's handler
	 */
	if (scanrelid > 0)
	{
		currentRelation = ExecOpenScanRelation(estate, scanrelid, eflags);
		scanstate->ss.ss_currentRelation = currentRelation;
		fdwroutine = GetFdwRoutineForRelation(currentRelation, true);
	}
	else
	{
		/* We can't use the relcache, so get fdwroutine the hard way */
		fdwroutine = GetFdwRoutineByServerId(node->fs_server);
	}

	/*
	 * Determine the scan tuple type.  If the FDW provided a targetlist
	 * describing the scan tuples, use that; else use base relation's rowtype.
	 */
	if (node->fdw_scan_tlist != NIL || currentRelation == NULL)
	{
		TupleDesc	scan_tupdesc;

		scan_tupdesc = ExecTypeFromTL(node->fdw_scan_tlist);
		ExecInitScanTupleSlot(estate, &scanstate->ss, scan_tupdesc,
							  &TTSOpsHeapTuple);
		/* Node's targetlist will contain Vars with varno = INDEX_VAR */
		tlistvarno = INDEX_VAR;
	}
	else
	{
		TupleDesc	scan_tupdesc;

		/* don't trust FDWs to return tuples fulfilling NOT NULL constraints */
		scan_tupdesc = CreateTupleDescCopy(RelationGetDescr(currentRelation));
		ExecInitScanTupleSlot(estate, &scanstate->ss, scan_tupdesc,
							  &TTSOpsHeapTuple);
		/* Node's targetlist will contain Vars with varno = scanrelid */
		tlistvarno = scanrelid;
	}

	/* Don't know what an FDW might return */
	scanstate->ss.ps.scanopsfixed = false;
	scanstate->ss.ps.scanopsset = true;

	/*
	 * Initialize result slot, type and projection.
	 */
	ExecInitResultTypeTL(&scanstate->ss.ps);
	ExecAssignScanProjectionInfoWithVarno(&scanstate->ss, tlistvarno);

	/*
	 * initialize child expressions
	 */
	scanstate->ss.ps.qual =
		ExecInitQual(node->scan.plan.qual, (PlanState *) scanstate);
	scanstate->fdw_recheck_quals =
		ExecInitQual(node->fdw_recheck_quals, (PlanState *) scanstate);

	/*
	 * Determine whether to scan the foreign relation asynchronously or not;
	 * this has to be kept in sync with the code in ExecInitAppend().
	 */
	scanstate->ss.ps.async_capable = (((Plan *) node)->async_capable &&
									  estate->es_epq_active == NULL);

	/*
	 * Initialize FDW-related state.
	 */
	scanstate->fdwroutine = fdwroutine;
	scanstate->fdw_state = NULL;

	/*
	 * For the FDW's convenience, look up the modification target relation's
	 * ResultRelInfo.  The ModifyTable node should have initialized it for us,
	 * see ExecInitModifyTable.
	 *
	 * Don't try to look up the ResultRelInfo when EvalPlanQual is active,
	 * though.  Direct modifications cannot be re-evaluated as part of
	 * EvalPlanQual.  The lookup wouldn't work anyway because during
	 * EvalPlanQual processing, EvalPlanQual only initializes the subtree
	 * under the ModifyTable, and doesn't run ExecInitModifyTable.
	 */
	if (node->resultRelation > 0 && estate->es_epq_active == NULL)
	{
		if (estate->es_result_relations == NULL ||
			estate->es_result_relations[node->resultRelation - 1] == NULL)
		{
			elog(ERROR, "result relation not initialized");
		}
		scanstate->resultRelInfo = estate->es_result_relations[node->resultRelation - 1];
	}

	/* Initialize any outer plan. */
	if (outerPlan(node))
		outerPlanState(scanstate) =
			ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * Tell the FDW to initialize the scan.
	 */
	if (node->operation != CMD_SELECT)
	{
		/*
		 * Direct modifications cannot be re-evaluated by EvalPlanQual, so
		 * don't bother preparing the FDW.
		 *
		 * In case of an inherited UPDATE/DELETE with foreign targets there
		 * can be direct-modify ForeignScan nodes in the EvalPlanQual subtree,
		 * so we need to ignore such ForeignScan nodes during EvalPlanQual
		 * processing.  See also ExecForeignScan/ExecReScanForeignScan.
		 */
		if (estate->es_epq_active == NULL)
			fdwroutine->BeginDirectModify(scanstate, eflags);
	}
	else
		fdwroutine->BeginForeignScan(scanstate, eflags);

	return scanstate;
}

/* ----------------------------------------------------------------
 *		ExecEndForeignScan
 *
 *		frees any storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndForeignScan(ForeignScanState *node)
{
	ForeignScan *plan = (ForeignScan *) node->ss.ps.plan;
	EState	   *estate = node->ss.ps.state;

	/* Let the FDW shut down */
	if (plan->operation != CMD_SELECT)
	{
		if (estate->es_epq_active == NULL)
			node->fdwroutine->EndDirectModify(node);
	}
	else
		node->fdwroutine->EndForeignScan(node);

	/* Shut down any outer plan. */
	if (outerPlanState(node))
		ExecEndNode(outerPlanState(node));

	/* Free the exprcontext */
	ExecFreeExprContext(&node->ss.ps);

	/* clean out the tuple table */
	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);
}

/* ----------------------------------------------------------------
 *		ExecReScanForeignScan
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecReScanForeignScan(ForeignScanState *node)
{
	ForeignScan *plan = (ForeignScan *) node->ss.ps.plan;
	EState	   *estate = node->ss.ps.state;
	PlanState  *outerPlan = outerPlanState(node);

	/*
	 * Ignore direct modifications when EvalPlanQual is active --- they are
	 * irrelevant for EvalPlanQual rechecking
	 */
	if (estate->es_epq_active != NULL && plan->operation != CMD_SELECT)
		return;

	node->fdwroutine->ReScanForeignScan(node);

	/*
	 * If chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.  outerPlan may also be NULL, in which case there is
	 * nothing to rescan at all.
	 */
	if (outerPlan != NULL && outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);

	ExecScanReScan(&node->ss);
}

/* ----------------------------------------------------------------
 *		ExecForeignScanEstimate
 *
 *		Informs size of the parallel coordination information, if any
 * ----------------------------------------------------------------
 */
void
ExecForeignScanEstimate(ForeignScanState *node, ParallelContext *pcxt)
{
	FdwRoutine *fdwroutine = node->fdwroutine;

	if (fdwroutine->EstimateDSMForeignScan)
	{
		node->pscan_len = fdwroutine->EstimateDSMForeignScan(node, pcxt);
		shm_toc_estimate_chunk(&pcxt->estimator, node->pscan_len);
		shm_toc_estimate_keys(&pcxt->estimator, 1);
	}
}

/* ----------------------------------------------------------------
 *		ExecForeignScanInitializeDSM
 *
 *		Initialize the parallel coordination information
 * ----------------------------------------------------------------
 */
void
ExecForeignScanInitializeDSM(ForeignScanState *node, ParallelContext *pcxt)
{
	FdwRoutine *fdwroutine = node->fdwroutine;

	if (fdwroutine->InitializeDSMForeignScan)
	{
		int			plan_node_id = node->ss.ps.plan->plan_node_id;
		void	   *coordinate;

		coordinate = shm_toc_allocate(pcxt->toc, node->pscan_len);
		fdwroutine->InitializeDSMForeignScan(node, pcxt, coordinate);
		shm_toc_insert(pcxt->toc, plan_node_id, coordinate);
	}
}

/* ----------------------------------------------------------------
 *		ExecForeignScanReInitializeDSM
 *
 *		Reset shared state before beginning a fresh scan.
 * ----------------------------------------------------------------
 */
void
ExecForeignScanReInitializeDSM(ForeignScanState *node, ParallelContext *pcxt)
{
	FdwRoutine *fdwroutine = node->fdwroutine;

	if (fdwroutine->ReInitializeDSMForeignScan)
	{
		int			plan_node_id = node->ss.ps.plan->plan_node_id;
		void	   *coordinate;

		coordinate = shm_toc_lookup(pcxt->toc, plan_node_id, false);
		fdwroutine->ReInitializeDSMForeignScan(node, pcxt, coordinate);
	}
}

/* ----------------------------------------------------------------
 *		ExecForeignScanInitializeWorker
 *
 *		Initialization according to the parallel coordination information
 * ----------------------------------------------------------------
 */
void
ExecForeignScanInitializeWorker(ForeignScanState *node,
								ParallelWorkerContext *pwcxt)
{
	FdwRoutine *fdwroutine = node->fdwroutine;

	if (fdwroutine->InitializeWorkerForeignScan)
	{
		int			plan_node_id = node->ss.ps.plan->plan_node_id;
		void	   *coordinate;

		coordinate = shm_toc_lookup(pwcxt->toc, plan_node_id, false);
		fdwroutine->InitializeWorkerForeignScan(node, pwcxt->toc, coordinate);
	}
}

/* ----------------------------------------------------------------
 *		ExecShutdownForeignScan
 *
 *		Gives FDW chance to stop asynchronous resource consumption
 *		and release any resources still held.
 * ----------------------------------------------------------------
 */
void
ExecShutdownForeignScan(ForeignScanState *node)
{
	FdwRoutine *fdwroutine = node->fdwroutine;

	if (fdwroutine->ShutdownForeignScan)
		fdwroutine->ShutdownForeignScan(node);
}

/* ----------------------------------------------------------------
 *		ExecAsyncForeignScanRequest
 *
 *		Asynchronously request a tuple from a designed async-capable node
 * ----------------------------------------------------------------
 */
void
ExecAsyncForeignScanRequest(AsyncRequest *areq)
{
	ForeignScanState *node = (ForeignScanState *) areq->requestee;
	FdwRoutine *fdwroutine = node->fdwroutine;

	Assert(fdwroutine->ForeignAsyncRequest != NULL);
	fdwroutine->ForeignAsyncRequest(areq);
}

/* ----------------------------------------------------------------
 *		ExecAsyncForeignScanConfigureWait
 *
 *		In async mode, configure for a wait
 * ----------------------------------------------------------------
 */
void
ExecAsyncForeignScanConfigureWait(AsyncRequest *areq)
{
	ForeignScanState *node = (ForeignScanState *) areq->requestee;
	FdwRoutine *fdwroutine = node->fdwroutine;

	Assert(fdwroutine->ForeignAsyncConfigureWait != NULL);
	fdwroutine->ForeignAsyncConfigureWait(areq);
}

/* ----------------------------------------------------------------
 *		ExecAsyncForeignScanNotify
 *
 *		Callback invoked when a relevant event has occurred
 * ----------------------------------------------------------------
 */
void
ExecAsyncForeignScanNotify(AsyncRequest *areq)
{
	ForeignScanState *node = (ForeignScanState *) areq->requestee;
	FdwRoutine *fdwroutine = node->fdwroutine;

	Assert(fdwroutine->ForeignAsyncNotify != NULL);
	fdwroutine->ForeignAsyncNotify(areq);
}
