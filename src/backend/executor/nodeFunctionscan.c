/*-------------------------------------------------------------------------
 *
 * nodeFunctionscan.c
 *	  Support routines for scanning RangeFunctions (functions in rangetable).
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeFunctionscan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecFunctionScan		scans a function.
 *		ExecFunctionNext		retrieve next tuple in sequential order.
 *		ExecInitFunctionScan	creates and initializes a functionscan node.
 *		ExecEndFunctionScan		releases any storage allocated.
 *		ExecReScanFunctionScan	rescans the function
 */
#include "postgres.h"

#include "executor/nodeFunctionscan.h"
#include "funcapi.h"
#include "nodes/nodeFuncs.h"
#include "catalog/pg_type.h"

static TupleTableSlot *FunctionNext(FunctionScanState *node);

/* ----------------------------------------------------------------
 *						Scan Support
 * ----------------------------------------------------------------
 */
/* ----------------------------------------------------------------
 *		FunctionNext
 *
 *		This is a workhorse for ExecFunctionScan
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
FunctionNext(FunctionScanState *node)
{
	EState	   *estate;
	ScanDirection direction;
	Tuplestorestate *tuplestorestate;
	TupleTableSlot *scanslot;
	TupleTableSlot *funcslot;

	if (node->func_slot)
	{
		/*
		 * ORDINALITY case:
		 *
		 * We fetch the function result into FUNCSLOT (which matches the
		 * function return type), and then copy the values to SCANSLOT
		 * (which matches the scan result type), setting the ordinal
		 * column in the process.
		 */

		funcslot = node->func_slot;
		scanslot = node->ss.ss_ScanTupleSlot;
	}
	else
	{
		/*
		 * non-ORDINALITY case: the function return type and scan result
		 * type are the same, so we fetch the function result straight
		 * into the scan result slot.
		 */

		funcslot = node->ss.ss_ScanTupleSlot;
		scanslot = NULL;
	}

	/*
	 * get information from the estate and scan state
	 */
	estate = node->ss.ps.state;
	direction = estate->es_direction;

	tuplestorestate = node->tuplestorestate;

	/*
	 * If first time through, read all tuples from function and put them in a
	 * tuplestore. Subsequent calls just fetch tuples from tuplestore.
	 */
	if (tuplestorestate == NULL)
	{
		node->tuplestorestate = tuplestorestate =
			ExecMakeTableFunctionResult(node->funcexpr,
										node->ss.ps.ps_ExprContext,
										node->func_tupdesc,
										node->eflags & EXEC_FLAG_BACKWARD);
	}

	/*
	 * Get the next tuple from tuplestore. Return NULL if no more tuples.
	 */
	(void) tuplestore_gettupleslot(tuplestorestate,
								   ScanDirectionIsForward(direction),
								   false,
								   funcslot);

	if (!scanslot)
		return funcslot;

	/*
	 * we're doing ordinality, so we copy the values from the function return
	 * slot to the (distinct) scan slot. We can do this because the lifetimes
	 * of the values in each slot are the same; until we reset the scan or
	 * fetch the next tuple, both will be valid.
	 */

	ExecClearTuple(scanslot);

	/*
	 * increment or decrement before checking for end-of-data, so that we can
	 * move off either end of the result by 1 (and no more than 1) without
	 * losing correct count. See PortalRunSelect for why we assume that we
	 * won't be called repeatedly in the end-of-data state.
	 */

	if (ScanDirectionIsForward(direction))
		node->ordinal++;
	else
		node->ordinal--;

	if (!TupIsNull(funcslot))
	{
		int     natts = funcslot->tts_tupleDescriptor->natts;
		int     i;

		slot_getallattrs(funcslot);

		for (i = 0; i < natts; ++i)
		{
			scanslot->tts_values[i] = funcslot->tts_values[i];
			scanslot->tts_isnull[i] = funcslot->tts_isnull[i];
		}

		scanslot->tts_values[natts] = Int64GetDatumFast(node->ordinal);
		scanslot->tts_isnull[natts] = false;

		ExecStoreVirtualTuple(scanslot);
	}

	return scanslot;
}

/*
 * FunctionRecheck -- access method routine to recheck a tuple in EvalPlanQual
 */
static bool
FunctionRecheck(FunctionScanState *node, TupleTableSlot *slot)
{
	/* nothing to check */
	return true;
}

/* ----------------------------------------------------------------
 *		ExecFunctionScan(node)
 *
 *		Scans the function sequentially and returns the next qualifying
 *		tuple.
 *		We call the ExecScan() routine and pass it the appropriate
 *		access method functions.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecFunctionScan(FunctionScanState *node)
{
	return ExecScan(&node->ss,
					(ExecScanAccessMtd) FunctionNext,
					(ExecScanRecheckMtd) FunctionRecheck);
}

/* ----------------------------------------------------------------
 *		ExecInitFunctionScan
 * ----------------------------------------------------------------
 */
FunctionScanState *
ExecInitFunctionScan(FunctionScan *node, EState *estate, int eflags)
{
	FunctionScanState *scanstate;
	Oid			funcrettype;
	TypeFuncClass functypclass;
	TupleDesc	func_tupdesc = NULL;
	TupleDesc	scan_tupdesc = NULL;

	/* check for unsupported flags */
	Assert(!(eflags & EXEC_FLAG_MARK));

	/*
	 * FunctionScan should not have any children.
	 */
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * create new ScanState for node
	 */
	scanstate = makeNode(FunctionScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;
	scanstate->eflags = eflags;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &scanstate->ss.ps);
	ExecInitScanTupleSlot(estate, &scanstate->ss);

	/*
	 * We only need a separate slot for the function result if we are doing
	 * ordinality; otherwise, we fetch function results directly into the
	 * scan slot.
	 */
	if (node->funcordinality)
		scanstate->func_slot = ExecInitExtraTupleSlot(estate);
	else
		scanstate->func_slot = NULL;

	/*
	 * initialize child expressions
	 */
	scanstate->ss.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->scan.plan.targetlist,
					 (PlanState *) scanstate);
	scanstate->ss.ps.qual = (List *)
		ExecInitExpr((Expr *) node->scan.plan.qual,
					 (PlanState *) scanstate);

	/*
	 * Now determine if the function returns a simple or composite
	 * type, and build an appropriate tupdesc. This tupdesc
	 * (func_tupdesc) is the one that matches the shape of the
	 * function result, no extra columns.
	 */
	functypclass = get_expr_result_type(node->funcexpr,
										&funcrettype,
										&func_tupdesc);

	if (functypclass == TYPEFUNC_COMPOSITE)
	{
		/* Composite data type, e.g. a table's row type */
		Assert(func_tupdesc);

		/*
		 * XXX
		 * Existing behaviour is a bit inconsistent with regard to aliases and
		 * whole-row Vars of the function result. If the function returns a
		 * composite type, then the whole-row Var will refer to this tupdesc,
		 * which has the type's own column names rather than the alias column
		 * names given in the query. This affects the output of constructs like
		 * row_to_json which read the column names from the passed-in values.
		 */

		/* Must copy it out of typcache for safety */
		func_tupdesc = CreateTupleDescCopy(func_tupdesc);
	}
	else if (functypclass == TYPEFUNC_SCALAR)
	{
		/* Base data type, i.e. scalar */
		char	   *attname = strVal(linitial(node->funccolnames));

		func_tupdesc = CreateTemplateTupleDesc(1, false);
		TupleDescInitEntry(func_tupdesc,
						   (AttrNumber) 1,
						   attname,
						   funcrettype,
						   -1,
						   0);
		TupleDescInitEntryCollation(func_tupdesc,
									(AttrNumber) 1,
									exprCollation(node->funcexpr));
	}
	else if (functypclass == TYPEFUNC_RECORD)
	{
		func_tupdesc = BuildDescFromLists(node->funccolnames,
										  node->funccoltypes,
										  node->funccoltypmods,
										  node->funccolcollations);
	}
	else
	{
		/* crummy error message, but parser should have caught this */
		elog(ERROR, "function in FROM has unsupported return type");
	}

	/*
	 * For RECORD results, make sure a typmod has been assigned.  (The
	 * function should do this for itself, but let's cover things in case it
	 * doesn't.)
	 */
	BlessTupleDesc(func_tupdesc);

	/*
	 * If doing ordinality, we need a new tupdesc with one additional column
	 * tacked on, always of type "bigint". The name to use has already been
	 * recorded by the parser as the last element of funccolnames.
	 *
	 * Without ordinality, the scan result tupdesc is the same as the
	 * function result tupdesc. (No need to make a copy.)
	 */
	if (node->funcordinality)
	{
		int natts = func_tupdesc->natts;

		scan_tupdesc = CreateTupleDescCopyExtend(func_tupdesc, 1);

		TupleDescInitEntry(scan_tupdesc,
						   natts + 1,
						   strVal(llast(node->funccolnames)),
						   INT8OID,
						   -1,
						   0);

		BlessTupleDesc(scan_tupdesc);
	}
	else
		scan_tupdesc = func_tupdesc;

	scanstate->scan_tupdesc = scan_tupdesc;
	scanstate->func_tupdesc = func_tupdesc;
	ExecAssignScanType(&scanstate->ss, scan_tupdesc);

	if (scanstate->func_slot)
		ExecSetSlotDescriptor(scanstate->func_slot, func_tupdesc);

	/*
	 * Other node-specific setup
	 */
	scanstate->ordinal = 0;
	scanstate->tuplestorestate = NULL;

	scanstate->funcexpr = ExecInitExpr((Expr *) node->funcexpr,
									   (PlanState *) scanstate);

	scanstate->ss.ps.ps_TupFromTlist = false;

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&scanstate->ss.ps);
	ExecAssignScanProjectionInfo(&scanstate->ss);

	return scanstate;
}

/* ----------------------------------------------------------------
 *		ExecEndFunctionScan
 *
 *		frees any storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndFunctionScan(FunctionScanState *node)
{
	/*
	 * Free the exprcontext
	 */
	ExecFreeExprContext(&node->ss.ps);

	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	if (node->func_slot)
		ExecClearTuple(node->func_slot);

	/*
	 * Release tuplestore resources
	 */
	if (node->tuplestorestate != NULL)
		tuplestore_end(node->tuplestorestate);
	node->tuplestorestate = NULL;
}

/* ----------------------------------------------------------------
 *		ExecReScanFunctionScan
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecReScanFunctionScan(FunctionScanState *node)
{
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	if (node->func_slot)
		ExecClearTuple(node->func_slot);

	ExecScanReScan(&node->ss);

	node->ordinal = 0;

	/*
	 * If we haven't materialized yet, just return.
	 */
	if (!node->tuplestorestate)
		return;

	/*
	 * Here we have a choice whether to drop the tuplestore (and recompute the
	 * function outputs) or just rescan it.  We must recompute if the
	 * expression contains parameters, else we rescan.	XXX maybe we should
	 * recompute if the function is volatile?
	 */
	if (node->ss.ps.chgParam != NULL)
	{
		tuplestore_end(node->tuplestorestate);
		node->tuplestorestate = NULL;
	}
	else
		tuplestore_rescan(node->tuplestorestate);
}
