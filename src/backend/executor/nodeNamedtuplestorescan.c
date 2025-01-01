/*-------------------------------------------------------------------------
 *
 * nodeNamedtuplestorescan.c
 *	  routines to handle NamedTuplestoreScan nodes.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeNamedtuplestorescan.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeNamedtuplestorescan.h"
#include "utils/queryenvironment.h"

static TupleTableSlot *NamedTuplestoreScanNext(NamedTuplestoreScanState *node);

/* ----------------------------------------------------------------
 *		NamedTuplestoreScanNext
 *
 *		This is a workhorse for ExecNamedTuplestoreScan
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
NamedTuplestoreScanNext(NamedTuplestoreScanState *node)
{
	TupleTableSlot *slot;

	/* We intentionally do not support backward scan. */
	Assert(ScanDirectionIsForward(node->ss.ps.state->es_direction));

	/*
	 * Get the next tuple from tuplestore. Return NULL if no more tuples.
	 */
	slot = node->ss.ss_ScanTupleSlot;
	tuplestore_select_read_pointer(node->relation, node->readptr);
	(void) tuplestore_gettupleslot(node->relation, true, false, slot);
	return slot;
}

/*
 * NamedTuplestoreScanRecheck -- access method routine to recheck a tuple in
 * EvalPlanQual
 */
static bool
NamedTuplestoreScanRecheck(NamedTuplestoreScanState *node, TupleTableSlot *slot)
{
	/* nothing to check */
	return true;
}

/* ----------------------------------------------------------------
 *		ExecNamedTuplestoreScan(node)
 *
 *		Scans the CTE sequentially and returns the next qualifying tuple.
 *		We call the ExecScan() routine and pass it the appropriate
 *		access method functions.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecNamedTuplestoreScan(PlanState *pstate)
{
	NamedTuplestoreScanState *node = castNode(NamedTuplestoreScanState, pstate);

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) NamedTuplestoreScanNext,
					(ExecScanRecheckMtd) NamedTuplestoreScanRecheck);
}


/* ----------------------------------------------------------------
 *		ExecInitNamedTuplestoreScan
 * ----------------------------------------------------------------
 */
NamedTuplestoreScanState *
ExecInitNamedTuplestoreScan(NamedTuplestoreScan *node, EState *estate, int eflags)
{
	NamedTuplestoreScanState *scanstate;
	EphemeralNamedRelation enr;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * NamedTuplestoreScan should not have any children.
	 */
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * create new NamedTuplestoreScanState for node
	 */
	scanstate = makeNode(NamedTuplestoreScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;
	scanstate->ss.ps.ExecProcNode = ExecNamedTuplestoreScan;

	enr = get_ENR(estate->es_queryEnv, node->enrname);
	if (!enr)
		elog(ERROR, "executor could not find named tuplestore \"%s\"",
			 node->enrname);

	Assert(enr->reldata);
	scanstate->relation = (Tuplestorestate *) enr->reldata;
	scanstate->tupdesc = ENRMetadataGetTupDesc(&(enr->md));
	scanstate->readptr =
		tuplestore_alloc_read_pointer(scanstate->relation, EXEC_FLAG_REWIND);

	/*
	 * The new read pointer copies its position from read pointer 0, which
	 * could be anywhere, so explicitly rewind it.
	 */
	tuplestore_select_read_pointer(scanstate->relation, scanstate->readptr);
	tuplestore_rescan(scanstate->relation);

	/*
	 * XXX: Should we add a function to free that read pointer when done?
	 *
	 * This was attempted, but it did not improve performance or memory usage
	 * in any tested cases.
	 */

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	/*
	 * The scan tuple type is specified for the tuplestore.
	 */
	ExecInitScanTupleSlot(estate, &scanstate->ss, scanstate->tupdesc,
						  &TTSOpsMinimalTuple);

	/*
	 * Initialize result type and projection.
	 */
	ExecInitResultTypeTL(&scanstate->ss.ps);
	ExecAssignScanProjectionInfo(&scanstate->ss);

	/*
	 * initialize child expressions
	 */
	scanstate->ss.ps.qual =
		ExecInitQual(node->scan.plan.qual, (PlanState *) scanstate);

	return scanstate;
}

/* ----------------------------------------------------------------
 *		ExecReScanNamedTuplestoreScan
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecReScanNamedTuplestoreScan(NamedTuplestoreScanState *node)
{
	Tuplestorestate *tuplestorestate = node->relation;

	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	ExecScanReScan(&node->ss);

	/*
	 * Rewind my own pointer.
	 */
	tuplestore_select_read_pointer(tuplestorestate, node->readptr);
	tuplestore_rescan(tuplestorestate);
}
