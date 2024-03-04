/* ------------------------------------------------------------------------
 *
 * nodeCustom.c
 *		Routines to handle execution of custom scan node
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * ------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/parallel.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/extensible.h"
#include "nodes/plannodes.h"
#include "utils/rel.h"

static TupleTableSlot *ExecCustomScan(PlanState *pstate);


CustomScanState *
ExecInitCustomScan(CustomScan *cscan, EState *estate, int eflags)
{
	CustomScanState *css;
	const TupleTableSlotOps *slotOps;
	Relation	scan_rel = NULL;
	Index		scanrelid = cscan->scan.scanrelid;
	int			tlistvarno;

	/*
	 * Allocate the CustomScanState object.  We let the custom scan provider
	 * do the palloc, in case it wants to make a larger object that embeds
	 * CustomScanState as the first field.  It must set the node tag and the
	 * methods field correctly at this time.  Other standard fields should be
	 * set to zero.
	 */
	css = castNode(CustomScanState,
				   cscan->methods->CreateCustomScanState(cscan));

	/* ensure flags is filled correctly */
	css->flags = cscan->flags;

	/* fill up fields of ScanState */
	css->ss.ps.plan = &cscan->scan.plan;
	css->ss.ps.state = estate;
	css->ss.ps.ExecProcNode = ExecCustomScan;

	/* create expression context for node */
	ExecAssignExprContext(estate, &css->ss.ps);

	/*
	 * open the scan relation, if any
	 */
	if (scanrelid > 0)
	{
		scan_rel = ExecOpenScanRelation(estate, scanrelid, eflags);
		css->ss.ss_currentRelation = scan_rel;
	}

	/*
	 * Use a custom slot if specified in CustomScanState or use virtual slot
	 * otherwise.
	 */
	slotOps = css->slotOps;
	if (!slotOps)
		slotOps = &TTSOpsVirtual;

	/*
	 * Determine the scan tuple type.  If the custom scan provider provided a
	 * targetlist describing the scan tuples, use that; else use base
	 * relation's rowtype.
	 */
	if (cscan->custom_scan_tlist != NIL || scan_rel == NULL)
	{
		TupleDesc	scan_tupdesc;

		scan_tupdesc = ExecTypeFromTL(cscan->custom_scan_tlist);
		ExecInitScanTupleSlot(estate, &css->ss, scan_tupdesc, slotOps);
		/* Node's targetlist will contain Vars with varno = INDEX_VAR */
		tlistvarno = INDEX_VAR;
	}
	else
	{
		ExecInitScanTupleSlot(estate, &css->ss, RelationGetDescr(scan_rel),
							  slotOps);
		/* Node's targetlist will contain Vars with varno = scanrelid */
		tlistvarno = scanrelid;
	}

	/*
	 * Initialize result slot, type and projection.
	 */
	ExecInitResultTupleSlotTL(&css->ss.ps, &TTSOpsVirtual);
	ExecAssignScanProjectionInfoWithVarno(&css->ss, tlistvarno);

	/* initialize child expressions */
	css->ss.ps.qual =
		ExecInitQual(cscan->scan.plan.qual, (PlanState *) css);

	/*
	 * The callback of custom-scan provider applies the final initialization
	 * of the custom-scan-state node according to its logic.
	 */
	css->methods->BeginCustomScan(css, estate, eflags);

	return css;
}

static TupleTableSlot *
ExecCustomScan(PlanState *pstate)
{
	CustomScanState *node = castNode(CustomScanState, pstate);

	CHECK_FOR_INTERRUPTS();

	Assert(node->methods->ExecCustomScan != NULL);
	return node->methods->ExecCustomScan(node);
}

void
ExecEndCustomScan(CustomScanState *node)
{
	Assert(node->methods->EndCustomScan != NULL);
	node->methods->EndCustomScan(node);
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
ExecCustomScanReInitializeDSM(CustomScanState *node, ParallelContext *pcxt)
{
	const CustomExecMethods *methods = node->methods;

	if (methods->ReInitializeDSMCustomScan)
	{
		int			plan_node_id = node->ss.ps.plan->plan_node_id;
		void	   *coordinate;

		coordinate = shm_toc_lookup(pcxt->toc, plan_node_id, false);
		methods->ReInitializeDSMCustomScan(node, pcxt, coordinate);
	}
}

void
ExecCustomScanInitializeWorker(CustomScanState *node,
							   ParallelWorkerContext *pwcxt)
{
	const CustomExecMethods *methods = node->methods;

	if (methods->InitializeWorkerCustomScan)
	{
		int			plan_node_id = node->ss.ps.plan->plan_node_id;
		void	   *coordinate;

		coordinate = shm_toc_lookup(pwcxt->toc, plan_node_id, false);
		methods->InitializeWorkerCustomScan(node, pwcxt->toc, coordinate);
	}
}

void
ExecShutdownCustomScan(CustomScanState *node)
{
	const CustomExecMethods *methods = node->methods;

	if (methods->ShutdownCustomScan)
		methods->ShutdownCustomScan(node);
}
