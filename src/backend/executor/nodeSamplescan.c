/*-------------------------------------------------------------------------
 *
 * nodeSamplescan.c
 *	  Support routines for sample scans of relations (table sampling).
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeSamplescan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tablesample.h"
#include "executor/executor.h"
#include "executor/nodeSamplescan.h"
#include "miscadmin.h"
#include "parser/parsetree.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/tqual.h"

static void InitScanRelation(SampleScanState *node, EState *estate,
							 int eflags, TableSampleClause *tablesample);
static TupleTableSlot *SampleNext(SampleScanState *node);


/* ----------------------------------------------------------------
 *						Scan Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		SampleNext
 *
 *		This is a workhorse for ExecSampleScan
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
SampleNext(SampleScanState *node)
{
	TupleTableSlot	   *slot;
	TableSampleDesc	   *tsdesc;
	HeapTuple			tuple;

	/*
	 * get information from the scan state
	 */
	slot = node->ss.ss_ScanTupleSlot;
	tsdesc = node->tsdesc;

	tuple = tablesample_getnext(tsdesc);

	if (tuple)
		ExecStoreTuple(tuple,	/* tuple to store */
					   slot,	/* slot to store in */
					   tsdesc->heapScan->rs_cbuf,	/* buffer associated with this tuple */
					   false);	/* don't pfree this pointer */
	else
		ExecClearTuple(slot);

	return slot;
}

/*
 * SampleRecheck -- access method routine to recheck a tuple in EvalPlanQual
 */
static bool
SampleRecheck(SampleScanState *node, TupleTableSlot *slot)
{
	/* No need to recheck for SampleScan */
	return true;
}

/* ----------------------------------------------------------------
 *		ExecSampleScan(node)
 *
 *		Scans the relation using the sampling method and returns
 *		the next qualifying tuple.
 *		We call the ExecScan() routine and pass it the appropriate
 *		access method functions.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecSampleScan(SampleScanState *node)
{
	return ExecScan((ScanState *) node,
					(ExecScanAccessMtd) SampleNext,
					(ExecScanRecheckMtd) SampleRecheck);
}

/* ----------------------------------------------------------------
 *		InitScanRelation
 *
 *		Set up to access the scan relation.
 * ----------------------------------------------------------------
 */
static void
InitScanRelation(SampleScanState *node, EState *estate, int eflags,
				 TableSampleClause *tablesample)
{
	Relation	currentRelation;

	/*
	 * get the relation object id from the relid'th entry in the range table,
	 * open that relation and acquire appropriate lock on it.
	 */
	currentRelation = ExecOpenScanRelation(estate,
										   ((SampleScan *) node->ss.ps.plan)->scanrelid,
										   eflags);

	node->ss.ss_currentRelation = currentRelation;

	/*
	 * Even though we aren't going to do a conventional seqscan, it is useful
	 * to create a HeapScanDesc --- many of the fields in it are usable.
	 */
	node->ss.ss_currentScanDesc =
		heap_beginscan_sampling(currentRelation, estate->es_snapshot, 0, NULL,
								tablesample->tsmseqscan,
								tablesample->tsmpagemode);

	/* and report the scan tuple slot's rowtype */
	ExecAssignScanType(&node->ss, RelationGetDescr(currentRelation));
}


/* ----------------------------------------------------------------
 *		ExecInitSampleScan
 * ----------------------------------------------------------------
 */
SampleScanState *
ExecInitSampleScan(SampleScan *node, EState *estate, int eflags)
{
	SampleScanState *scanstate;
	RangeTblEntry *rte = rt_fetch(node->scanrelid,
								  estate->es_range_table);

	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);
	Assert(rte->tablesample != NULL);

	/*
	 * create state structure
	 */
	scanstate = makeNode(SampleScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;

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
		ExecInitExpr((Expr *) node->plan.targetlist,
					 (PlanState *) scanstate);
	scanstate->ss.ps.qual = (List *)
		ExecInitExpr((Expr *) node->plan.qual,
					 (PlanState *) scanstate);

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &scanstate->ss.ps);
	ExecInitScanTupleSlot(estate, &scanstate->ss);

	/*
	 * initialize scan relation
	 */
	InitScanRelation(scanstate, estate, eflags, rte->tablesample);

	scanstate->ss.ps.ps_TupFromTlist = false;

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&scanstate->ss.ps);
	ExecAssignScanProjectionInfo(&scanstate->ss);

	scanstate->tsdesc = tablesample_init(scanstate, rte->tablesample);

	return scanstate;
}

/* ----------------------------------------------------------------
 *		ExecEndSampleScan
 *
 *		frees any storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndSampleScan(SampleScanState *node)
{
	/*
	 * Tell sampling function that we finished the scan.
	 */
	tablesample_end(node->tsdesc);

	/*
	 * Free the exprcontext
	 */
	ExecFreeExprContext(&node->ss.ps);

	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/*
	 * close heap scan
	 */
	heap_endscan(node->ss.ss_currentScanDesc);

	/*
	 * close the heap relation.
	 */
	ExecCloseScanRelation(node->ss.ss_currentRelation);
}

/* ----------------------------------------------------------------
 *						Join Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecReScanSampleScan
 *
 *		Rescans the relation.
 *
 * ----------------------------------------------------------------
 */
void
ExecReScanSampleScan(SampleScanState *node)
{
	heap_rescan(node->ss.ss_currentScanDesc, NULL);

	/*
	 * Tell sampling function to reset its state for rescan.
	 */
	tablesample_reset(node->tsdesc);

	ExecScanReScan(&node->ss);
}
