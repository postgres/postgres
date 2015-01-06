/* ------------------------------------------------------------------------
 *
 * nodeCustom.c
 *		Routines to handle execution of custom scan node
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * ------------------------------------------------------------------------
 */
#include "postgres.h"

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
	CustomScanState    *css;
	Relation			scan_rel;

	/* populate a CustomScanState according to the CustomScan */
	css = (CustomScanState *) cscan->methods->CreateCustomScanState(cscan);
	Assert(IsA(css, CustomScanState));

	/* fill up fields of ScanState */
	css->ss.ps.plan = &cscan->scan.plan;
	css->ss.ps.state = estate;

	/* create expression context for node */
	ExecAssignExprContext(estate, &css->ss.ps);

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

	/* initialize scan relation */
	scan_rel = ExecOpenScanRelation(estate, cscan->scan.scanrelid, eflags);
	css->ss.ss_currentRelation = scan_rel;
	css->ss.ss_currentScanDesc = NULL;	/* set by provider */
	ExecAssignScanType(&css->ss, RelationGetDescr(scan_rel));

	css->ss.ps.ps_TupFromTlist = false;

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&css->ss.ps);
	ExecAssignScanProjectionInfo(&css->ss);

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
	if (node->ss.ss_ScanTupleSlot)
		ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/* Close the heap relation */
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
				 errmsg("custom-scan \"%s\" does not support MarkPos",
						node->methods->CustomName)));
	node->methods->MarkPosCustomScan(node);
}

void
ExecCustomRestrPos(CustomScanState *node)
{
	if (!node->methods->RestrPosCustomScan)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("custom-scan \"%s\" does not support MarkPos",
						node->methods->CustomName)));
	node->methods->RestrPosCustomScan(node);
}
