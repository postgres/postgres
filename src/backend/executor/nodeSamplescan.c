/*-------------------------------------------------------------------------
 *
 * nodeSamplescan.c
 *	  Support routines for sample scans of relations (table sampling).
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeSamplescan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relscan.h"
#include "access/tableam.h"
#include "access/tsmapi.h"
#include "executor/executor.h"
#include "executor/nodeSamplescan.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
#include "utils/builtins.h"
#include "utils/rel.h"

static TupleTableSlot *SampleNext(SampleScanState *node);
static void tablesample_init(SampleScanState *scanstate);
static TupleTableSlot *tablesample_getnext(SampleScanState *scanstate);

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
	/*
	 * if this is first call within a scan, initialize
	 */
	if (!node->begun)
		tablesample_init(node);

	/*
	 * get the next tuple, and store it in our result slot
	 */
	return tablesample_getnext(node);
}

/*
 * SampleRecheck -- access method routine to recheck a tuple in EvalPlanQual
 */
static bool
SampleRecheck(SampleScanState *node, TupleTableSlot *slot)
{
	/*
	 * No need to recheck for SampleScan, since like SeqScan we don't pass any
	 * checkable keys to heap_beginscan.
	 */
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
static TupleTableSlot *
ExecSampleScan(PlanState *pstate)
{
	SampleScanState *node = castNode(SampleScanState, pstate);

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) SampleNext,
					(ExecScanRecheckMtd) SampleRecheck);
}

/* ----------------------------------------------------------------
 *		ExecInitSampleScan
 * ----------------------------------------------------------------
 */
SampleScanState *
ExecInitSampleScan(SampleScan *node, EState *estate, int eflags)
{
	SampleScanState *scanstate;
	TableSampleClause *tsc = node->tablesample;
	TsmRoutine *tsm;

	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * create state structure
	 */
	scanstate = makeNode(SampleScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;
	scanstate->ss.ps.ExecProcNode = ExecSampleScan;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	/*
	 * open the scan relation
	 */
	scanstate->ss.ss_currentRelation =
		ExecOpenScanRelation(estate,
							 node->scan.scanrelid,
							 eflags);

	/* we won't set up the HeapScanDesc till later */
	scanstate->ss.ss_currentScanDesc = NULL;

	/* and create slot with appropriate rowtype */
	ExecInitScanTupleSlot(estate, &scanstate->ss,
						  RelationGetDescr(scanstate->ss.ss_currentRelation),
						  table_slot_callbacks(scanstate->ss.ss_currentRelation));

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

	scanstate->args = ExecInitExprList(tsc->args, (PlanState *) scanstate);
	scanstate->repeatable =
		ExecInitExpr(tsc->repeatable, (PlanState *) scanstate);

	/*
	 * If we don't have a REPEATABLE clause, select a random seed.  We want to
	 * do this just once, since the seed shouldn't change over rescans.
	 */
	if (tsc->repeatable == NULL)
		scanstate->seed = random();

	/*
	 * Finally, initialize the TABLESAMPLE method handler.
	 */
	tsm = GetTsmRoutine(tsc->tsmhandler);
	scanstate->tsmroutine = tsm;
	scanstate->tsm_state = NULL;

	if (tsm->InitSampleScan)
		tsm->InitSampleScan(scanstate, eflags);

	/* We'll do BeginSampleScan later; we can't evaluate params yet */
	scanstate->begun = false;

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
	if (node->tsmroutine->EndSampleScan)
		node->tsmroutine->EndSampleScan(node);

	/*
	 * Free the exprcontext
	 */
	ExecFreeExprContext(&node->ss.ps);

	/*
	 * clean out the tuple table
	 */
	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/*
	 * close heap scan
	 */
	if (node->ss.ss_currentScanDesc)
		table_endscan(node->ss.ss_currentScanDesc);
}

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
	/* Remember we need to do BeginSampleScan again (if we did it at all) */
	node->begun = false;
	node->done = false;
	node->haveblock = false;
	node->donetuples = 0;

	ExecScanReScan(&node->ss);
}


/*
 * Initialize the TABLESAMPLE method: evaluate params and call BeginSampleScan.
 */
static void
tablesample_init(SampleScanState *scanstate)
{
	TsmRoutine *tsm = scanstate->tsmroutine;
	ExprContext *econtext = scanstate->ss.ps.ps_ExprContext;
	Datum	   *params;
	Datum		datum;
	bool		isnull;
	uint32		seed;
	bool		allow_sync;
	int			i;
	ListCell   *arg;

	scanstate->donetuples = 0;
	params = (Datum *) palloc(list_length(scanstate->args) * sizeof(Datum));

	i = 0;
	foreach(arg, scanstate->args)
	{
		ExprState  *argstate = (ExprState *) lfirst(arg);

		params[i] = ExecEvalExprSwitchContext(argstate,
											  econtext,
											  &isnull);
		if (isnull)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLESAMPLE_ARGUMENT),
					 errmsg("TABLESAMPLE parameter cannot be null")));
		i++;
	}

	if (scanstate->repeatable)
	{
		datum = ExecEvalExprSwitchContext(scanstate->repeatable,
										  econtext,
										  &isnull);
		if (isnull)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLESAMPLE_REPEAT),
					 errmsg("TABLESAMPLE REPEATABLE parameter cannot be null")));

		/*
		 * The REPEATABLE parameter has been coerced to float8 by the parser.
		 * The reason for using float8 at the SQL level is that it will
		 * produce unsurprising results both for users used to databases that
		 * accept only integers in the REPEATABLE clause and for those who
		 * might expect that REPEATABLE works like setseed() (a float in the
		 * range from -1 to 1).
		 *
		 * We use hashfloat8() to convert the supplied value into a suitable
		 * seed.  For regression-testing purposes, that has the convenient
		 * property that REPEATABLE(0) gives a machine-independent result.
		 */
		seed = DatumGetUInt32(DirectFunctionCall1(hashfloat8, datum));
	}
	else
	{
		/* Use the seed selected by ExecInitSampleScan */
		seed = scanstate->seed;
	}

	/* Set default values for params that BeginSampleScan can adjust */
	scanstate->use_bulkread = true;
	scanstate->use_pagemode = true;

	/* Let tablesample method do its thing */
	tsm->BeginSampleScan(scanstate,
						 params,
						 list_length(scanstate->args),
						 seed);

	/* We'll use syncscan if there's no NextSampleBlock function */
	allow_sync = (tsm->NextSampleBlock == NULL);

	/* Now we can create or reset the HeapScanDesc */
	if (scanstate->ss.ss_currentScanDesc == NULL)
	{
		scanstate->ss.ss_currentScanDesc =
			table_beginscan_sampling(scanstate->ss.ss_currentRelation,
									 scanstate->ss.ps.state->es_snapshot,
									 0, NULL,
									 scanstate->use_bulkread,
									 allow_sync,
									 scanstate->use_pagemode);
	}
	else
	{
		table_rescan_set_params(scanstate->ss.ss_currentScanDesc, NULL,
								scanstate->use_bulkread,
								allow_sync,
								scanstate->use_pagemode);
	}

	pfree(params);

	/* And we're initialized. */
	scanstate->begun = true;
}

/*
 * Get next tuple from TABLESAMPLE method.
 */
static TupleTableSlot *
tablesample_getnext(SampleScanState *scanstate)
{
	TableScanDesc scan = scanstate->ss.ss_currentScanDesc;
	TupleTableSlot *slot = scanstate->ss.ss_ScanTupleSlot;

	ExecClearTuple(slot);

	if (scanstate->done)
		return NULL;

	for (;;)
	{
		if (!scanstate->haveblock)
		{
			if (!table_scan_sample_next_block(scan, scanstate))
			{
				scanstate->haveblock = false;
				scanstate->done = true;

				/* exhausted relation */
				return NULL;
			}

			scanstate->haveblock = true;
		}

		if (!table_scan_sample_next_tuple(scan, scanstate, slot))
		{
			/*
			 * If we get here, it means we've exhausted the items on this page
			 * and it's time to move to the next.
			 */
			scanstate->haveblock = false;
			continue;
		}

		/* Found visible tuple, return it. */
		break;
	}

	scanstate->donetuples++;

	return slot;
}
