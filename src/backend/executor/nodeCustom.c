/* ------------------------------------------------------------------------
 *
 * nodeCustom.c
 *		Routines to handle execution of custom scan node
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * ------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/parallel.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"
#include "parser/parsetree.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"

CustomScanState *
ExecInitCustomScan(CustomScan *cscan, EState *estate, int eflags)
{
	CustomScanState *css;
	Relation	scan_rel = NULL;
	Index		scanrelid = cscan->scan.scanrelid;
	Index		tlistvarno;

	/*
	 * Allocate the CustomScanState object.  We let the custom scan provider
	 * do the palloc, in case it wants to make a larger object that embeds
	 * CustomScanState as the first field.  It must set the node tag and the
	 * methods field correctly at this time.  Other standard fields should be
	 * set to zero.
	 */
	css = (CustomScanState *) cscan->methods->CreateCustomScanState(cscan);
	Assert(IsA(css, CustomScanState));

	/* ensure flags is filled correctly */
	css->flags = cscan->flags;

	/* fill up fields of ScanState */
	css->ss.ps.plan = &cscan->scan.plan;
	css->ss.ps.state = estate;

	/* create expression context for node */
	ExecAssignExprContext(estate, &css->ss.ps);

	css->ss.ps.ps_TupFromTlist = false;

	/* initialize child expressions */
	css->ss.ps.targetlist = (List *)
		ExecInitExpr((Expr *) cscan->scan.plan.targetlist,
					 (PlanState *) css);
	css->ss.ps.qual = (List *)
		ExecInitExpr((Expr *) cscan->scan.plan.qual,
					 (PlanState *) css);

	/* tuple table initialization */
	ExecInitScanTupleSlot(estate, &css->ss);
	ExecInitResultTupleSlot(estate, &css->ss.ps);

	/*
	 * open the base relation, if any, and acquire an appropriate lock on it
	 */
	if (scanrelid > 0)
	{
		scan_rel = ExecOpenScanRelation(estate, scanrelid, eflags);
		css->ss.ss_currentRelation = scan_rel;
	}

	/*
	 * Determine the scan tuple type.  If the custom scan provider provided a
	 * targetlist describing the scan tuples, use that; else use base
	 * relation's rowtype.
	 */
	if (cscan->custom_scan_tlist != NIL || scan_rel == NULL)
	{
		TupleDesc	scan_tupdesc;

		scan_tupdesc = ExecTypeFromTL(cscan->custom_scan_tlist, false);
		ExecAssignScanType(&css->ss, scan_tupdesc);
		/* Node's targetlist will contain Vars with varno = INDEX_VAR */
		tlistvarno = INDEX_VAR;
	}
	else
	{
		ExecAssignScanType(&css->ss, RelationGetDescr(scan_rel));
		/* Node's targetlist will contain Vars with varno = scanrelid */
		tlistvarno = scanrelid;
	}

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&css->ss.ps);
	ExecAssignScanProjectionInfoWithVarno(&css->ss, tlistvarno);

	/*
	 * The callback of custom-scan provider applies the final initialization
	 * of the custom-scan-state node according to its logic.
	 */
	css->methods->BeginCustomScan(css, estate, eflags);

	return css;
}

TupleTableSlot *
ExecCustomScan(CustomScanState *node)
{
	Assert(node->methods->ExecCustomScan != NULL);
	return node->methods->ExecCustomScan(node);
}

void
ExecEndCustomScan(CustomScanState *node)
{
	Assert(node->methods->EndCustomScan != NULL);
	node->methods->EndCustomScan(node);

	/* Free the exprcontext */
	ExecFreeExprContext(&node->ss.ps);

	/* Clean out the tuple table */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/* Close the heap relation */
	if (node->ss.ss_currentRelation)
		ExecCloseScanRelation(node->ss.ss_currentRelation);
}

void
ExecReScanCustomScan(CustomScanState *node)
{
	Assert(node->methods->ReScanCustomScan != NULL);
	node->methods->ReScanCustomScan(node);
}

void
ExecCustomMarkPos(CustomScanState *node)
{
	if (!node->methods->MarkPosCustomScan)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("custom scan \"%s\" does not support MarkPos",
						node->methods->CustomName)));
	node->methods->MarkPosCustomScan(node);
}

void
ExecCustomRestrPos(CustomScanState *node)
{
	if (!node->methods->RestrPosCustomScan)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("custom scan \"%s\" does not support MarkPos",
						node->methods->CustomName)));
	node->methods->RestrPosCustomScan(node);
}

void
ExecCustomScanEstimate(CustomScanState *node, ParallelContext *pcxt)
{
	const CustomExecMethods *methods = node->methods;

	if (methods->EstimateDSMCustomScan)
	{
		node->pscan_len = methods->EstimateDSMCustomScan(node, pcxt);
		shm_toc_estimate_chunk(&pcxt->estimator, node->pscan_len);
		shm_toc_estimate_keys(&pcxt->estimator, 1);
	}
}

void
ExecCustomScanInitializeDSM(CustomScanState *node, ParallelContext *pcxt)
{
	const CustomExecMethods *methods = node->methods;

	if (methods->InitializeDSMCustomScan)
	{
		int			plan_node_id = node->ss.ps.plan->plan_node_id;
		void	   *coordinate;

		coordinate = shm_toc_allocate(pcxt->toc, node->pscan_len);
		methods->InitializeDSMCustomScan(node, pcxt, coordinate);
		shm_toc_insert(pcxt->toc, plan_node_id, coordinate);
	}
}

void
ExecCustomScanInitializeWorker(CustomScanState *node, shm_toc *toc)
{
	const CustomExecMethods *methods = node->methods;

	if (methods->InitializeWorkerCustomScan)
	{
		int			plan_node_id = node->ss.ps.plan->plan_node_id;
		void	   *coordinate;

		coordinate = shm_toc_lookup(toc, plan_node_id);
		methods->InitializeWorkerCustomScan(node, toc, coordinate);
	}
}
