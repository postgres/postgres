/*-------------------------------------------------------------------------
 *
 * nodeFunctionscan.c
 *	  Support routines for scanning RangeFunctions (functions in rangetable).
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
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

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "executor/nodeFunctionscan.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_coerce.h"
#include "pgstat.h"
#include "utils/builtins.h"
#include "utils/expandeddatum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"
#include "utils/typcache.h"


/*
 * Runtime data for each function being scanned.
 */
typedef struct FunctionScanPerFuncState
{
	SetExprState *setexpr;		/* state of the expression being evaluated */
	TupleDesc	tupdesc;		/* desc of the function result type */
	int			colcount;		/* expected number of result columns */
	Tuplestorestate *tstore;	/* holds the function result set */
	TupleTableSlot *func_slot;	/* function result slot (or NULL) */


	bool		started;
	bool		returnsTuple;
	FunctionCallInfo fcinfo;
	ReturnSetInfo rsinfo;
} FunctionScanPerFuncState;

static TupleTableSlot *FunctionNext(FunctionScanState *node);
static void ExecBeginFunctionResult(FunctionScanState *node,
												FunctionScanPerFuncState *perfunc);
static void ExecNextFunctionResult(FunctionScanState *node,
											   FunctionScanPerFuncState *perfunc);
static void tupledesc_match(TupleDesc dst_tupdesc, TupleDesc src_tupdesc);


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
	TupleTableSlot *scanslot;
	bool		alldone;
	int			funcno;
	int			att;

	/*
	 * get information from the estate and scan state
	 */
	estate = node->ss.ps.state;
	direction = estate->es_direction;
	scanslot = node->ss.ss_ScanTupleSlot;

	Assert(ScanDirectionIsForward(direction));

	if (node->simple)
	{
		/*
		 * Fast path for the trivial case: the function return type and scan
		 * result type are the same, so we fetch the function result straight
		 * into the scan result slot. No need to update ordinality either.
		 */
		FunctionScanPerFuncState *fs = &node->funcstates[0];


		/*
		 * If first time through, call the SRF. Subsequent calls read from a
		 * tuplestore (for SFRM_Materialize) or call the function again (if
		 * SFRM_ValuePerCall).
		 */
		if (!fs->started)
			ExecBeginFunctionResult(node, fs);
		else
			ExecNextFunctionResult(node, fs);

		scanslot = fs->func_slot;

		return scanslot;
	}

	/*
	 * Increment ordinal counter before checking for end-of-data, so that we
	 * can move off the end of the result by 1 (and no more than 1) without
	 * losing correct count.  See PortalRunSelect for why we can assume that
	 * we won't be called repeatedly in the end-of-data state.
	 */
	node->ordinal++;

	/*
	 * Main loop over functions.
	 *
	 * We fetch the function results into func_slots (which match the function
	 * return types), and then copy the values to scanslot (which matches the
	 * scan result type), setting the ordinal column (if any) as well.
	 */
	ExecClearTuple(scanslot);
	att = 0;
	alldone = true;
	for (funcno = 0; funcno < node->nfuncs; funcno++)
	{
		FunctionScanPerFuncState *fs = &node->funcstates[funcno];
		int			i;

		/*
		 * If first time through, call the SRF. Subsequent calls read from a
		 * tuplestore (for SFRM_Materialize) or call the function again (if
		 * SFRM_ValuePerCall).
		 */
		if (!fs->started)
			ExecBeginFunctionResult(node, fs);
		else
			ExecNextFunctionResult(node, fs);

		if (TupIsNull(fs->func_slot))
		{
			/*
			 * populate the result cols with nulls
			 */
			for (i = 0; i < fs->colcount; i++)
			{
				scanslot->tts_values[att] = (Datum) 0;
				scanslot->tts_isnull[att] = true;
				att++;
			}
		}
		else
		{
			/*
			 * we have a result, so just copy it to the result cols.
			 */
			slot_getallattrs(fs->func_slot);

			for (i = 0; i < fs->colcount; i++)
			{
				scanslot->tts_values[att] = fs->func_slot->tts_values[i];
				scanslot->tts_isnull[att] = fs->func_slot->tts_isnull[i];
				att++;
			}

			/*
			 * We're not done until every function result is exhausted; we pad
			 * the shorter results with nulls until then.
			 */
			alldone = false;
		}
	}

	/*
	 * ordinal col is always last, per spec.
	 */
	if (node->ordinality)
	{
		scanslot->tts_values[att] = Int64GetDatumFast(node->ordinal);
		scanslot->tts_isnull[att] = false;
	}

	/*
	 * If alldone, we just return the previously-cleared scanslot.  Otherwise,
	 * finish creating the virtual tuple.
	 */
	if (!alldone)
		ExecStoreVirtualTuple(scanslot);

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
static TupleTableSlot *
ExecFunctionScan(PlanState *pstate)
{
	FunctionScanState *node = castNode(FunctionScanState, pstate);

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
	int			nfuncs = list_length(node->functions);
	TupleDesc	scan_tupdesc;
	int			i,
				natts;
	ListCell   *lc;

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
	scanstate->ss.ps.ExecProcNode = ExecFunctionScan;
	scanstate->eflags = eflags;

	/*
	 * are we adding an ordinality column?
	 */
	scanstate->ordinality = node->funcordinality;

	scanstate->nfuncs = nfuncs;
	if (nfuncs > 1 || node->funcordinality)
		scanstate->simple = false;
	else
		scanstate->simple = true;

	/* ordinal 0 represents the "before the first row" position */
	scanstate->ordinal = 0;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	scanstate->funcstates = palloc(nfuncs * sizeof(FunctionScanPerFuncState));

	natts = 0;
	i = 0;
	foreach(lc, node->functions)
	{
		RangeTblFunction *rtfunc = (RangeTblFunction *) lfirst(lc);
		Node	   *funcexpr = rtfunc->funcexpr;
		int			colcount = rtfunc->funccolcount;
		FunctionScanPerFuncState *fs = &scanstate->funcstates[i];
		TypeFuncClass functypclass;
		Oid			funcrettype;
		TupleDesc	tupdesc;

		fs->setexpr =
			ExecInitTableFunctionResult((Expr *) funcexpr,
										scanstate->ss.ps.ps_ExprContext,
										&scanstate->ss.ps);

		/*
		 * Don't allocate the tuplestores; the actual calls to the functions
		 * do that if necessary.  started = false means that we have not
		 * called the function yet (or need to call it again after a rescan).
		 */
		fs->tstore = NULL;
		fs->started = false;
		fs->rsinfo.setDesc = NULL;

		/*
		 * Now determine if the function returns a simple or composite type,
		 * and build an appropriate tupdesc.  Note that in the composite case,
		 * the function may now return more columns than it did when the plan
		 * was made; we have to ignore any columns beyond "colcount".
		 */
		functypclass = get_expr_result_type(funcexpr,
											&funcrettype,
											&tupdesc);

		if (functypclass == TYPEFUNC_COMPOSITE ||
			functypclass == TYPEFUNC_COMPOSITE_DOMAIN)
		{
			/* Composite data type, e.g. a table's row type */
			Assert(tupdesc);
			Assert(tupdesc->natts >= colcount);
			/* Must copy it out of typcache for safety */
			tupdesc = CreateTupleDescCopy(tupdesc);
			fs->returnsTuple = true;
		}
		else if (functypclass == TYPEFUNC_SCALAR)
		{
			/* Base data type, i.e. scalar */
			tupdesc = CreateTemplateTupleDesc(1);
			TupleDescInitEntry(tupdesc,
							   (AttrNumber) 1,
							   NULL,	/* don't care about the name here */
							   funcrettype,
							   -1,
							   0);
			TupleDescInitEntryCollation(tupdesc,
										(AttrNumber) 1,
										exprCollation(funcexpr));
			fs->returnsTuple = false;
		}
		else if (functypclass == TYPEFUNC_RECORD)
		{
			tupdesc = BuildDescFromLists(rtfunc->funccolnames,
										 rtfunc->funccoltypes,
										 rtfunc->funccoltypmods,
										 rtfunc->funccolcollations);

			/*
			 * For RECORD results, make sure a typmod has been assigned.  (The
			 * function should do this for itself, but let's cover things in
			 * case it doesn't.)
			 */
			BlessTupleDesc(tupdesc);
			fs->returnsTuple = true;
		}
		else
		{
			/* crummy error message, but parser should have caught this */
			elog(ERROR, "function in FROM has unsupported return type");
		}

		fs->tupdesc = tupdesc;
		fs->colcount = colcount;

		/*
		 * We only need separate slots for the function results if we are
		 * doing ordinality or multiple functions; otherwise, we'll fetch
		 * function results directly into the scan slot.
		 */
		if (!scanstate->simple)
		{
			fs->func_slot = ExecInitExtraTupleSlot(estate, fs->tupdesc,
												   &TTSOpsMinimalTuple);
		}
		else
			fs->func_slot = scanstate->ss.ss_ScanTupleSlot;

		natts += colcount;
		i++;
	}

	/*
	 * Create the combined TupleDesc
	 *
	 * If there is just one function without ordinality, the scan result
	 * tupdesc is the same as the function result tupdesc --- except that we
	 * may stuff new names into it below, so drop any rowtype label.
	 */
	if (scanstate->simple)
	{
		scan_tupdesc = CreateTupleDescCopy(scanstate->funcstates[0].tupdesc);
		scan_tupdesc->tdtypeid = RECORDOID;
		scan_tupdesc->tdtypmod = -1;
	}
	else
	{
		AttrNumber	attno = 0;

		if (node->funcordinality)
			natts++;

		scan_tupdesc = CreateTemplateTupleDesc(natts);

		for (i = 0; i < nfuncs; i++)
		{
			TupleDesc	tupdesc = scanstate->funcstates[i].tupdesc;
			int			colcount = scanstate->funcstates[i].colcount;
			int			j;

			for (j = 1; j <= colcount; j++)
				TupleDescCopyEntry(scan_tupdesc, ++attno, tupdesc, j);
		}

		/* If doing ordinality, add a column of type "bigint" at the end */
		if (node->funcordinality)
		{
			TupleDescInitEntry(scan_tupdesc,
							   ++attno,
							   NULL,	/* don't care about the name here */
							   INT8OID,
							   -1,
							   0);
		}

		Assert(attno == natts);
	}

	/*
	 * Initialize scan slot and type.
	 */
	ExecInitScanTupleSlot(estate, &scanstate->ss, scan_tupdesc,
						  &TTSOpsMinimalTuple);

	/*
	 * Initialize result slot, type and projection.
	 */
	ExecInitResultTypeTL(&scanstate->ss.ps);
	ExecAssignScanProjectionInfo(&scanstate->ss);

	/*
	 * initialize child expressions
	 */
	scanstate->ss.ps.qual =
		ExecInitQual(node->scan.plan.qual, (PlanState *) scanstate);

	/*
	 * Create a memory context that is used to evaluate function arguments in.
	 * We can't use the per-tuple context for this because it gets reset too
	 * often; but we don't want to leak evaluation results into the
	 * query-lifespan context either.  We currently just use one context for
	 * all functions, they're evaluated at the same time anyway - most of the
	 * time creating separate contexts would use more memory, than being able
	 * to reset separately would save.
	 */
	scanstate->argcontext = AllocSetContextCreate(CurrentMemoryContext,
												  "Table function arguments",
												  ALLOCSET_DEFAULT_SIZES);

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
	int			i;

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
	 * Release slots and tuplestore resources
	 */
	for (i = 0; i < node->nfuncs; i++)
	{
		FunctionScanPerFuncState *fs = &node->funcstates[i];

		if (fs->func_slot)
			ExecClearTuple(fs->func_slot);

		if (fs->tstore != NULL)
		{
			tuplestore_end(node->funcstates[i].tstore);
			fs->tstore = NULL;
		}
	}
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
	int			i;

	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	for (i = 0; i < node->nfuncs; i++)
	{
		FunctionScanPerFuncState *fs = &node->funcstates[i];

		if (fs->func_slot)
			ExecClearTuple(fs->func_slot);

		if (node->funcstates[i].tstore != NULL)
		{
			tuplestore_end(node->funcstates[i].tstore);
			node->funcstates[i].tstore = NULL;
		}

		/*
		 * If it is a dynamically-allocated TupleDesc, free it: it is
		 * typically allocated in a per-query context, so we must avoid
		 * leaking it across multiple usages.
		 */
		if (fs->rsinfo.setDesc && fs->rsinfo.setDesc->tdrefcount == -1)
		{
			FreeTupleDesc(fs->rsinfo.setDesc);
			fs->rsinfo.setDesc = NULL;
		}

		fs->started = false;
	}

	ExecScanReScan(&node->ss);

	/* Reset ordinality counter */
	node->ordinal = 0;
}


static void
ExecBeginFunctionResult(FunctionScanState *node,
						FunctionScanPerFuncState *perfunc)
{
	bool		returnsSet = false;
	MemoryContext callerContext;
	MemoryContext oldcontext;
	bool		direct_function_call = false;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	SetExprState *setexpr = perfunc->setexpr;
	Datum		result;

	callerContext = CurrentMemoryContext;

	Assert(perfunc->tupdesc != NULL);

 	/*
	 * Prepare a resultinfo node for communication.  We always do this even if
	 * not expecting a set result, so that we can pass expectedDesc.  In the
	 * generic-expression case, the expression doesn't actually get to see the
	 * resultinfo, but set it up anyway because we use some of the fields as
	 * our own state variables.
 	 */
	perfunc->rsinfo.type = T_ReturnSetInfo;
	perfunc->rsinfo.econtext = econtext;
	perfunc->rsinfo.expectedDesc = perfunc->tupdesc;
	perfunc->rsinfo.allowedModes = (int) (SFRM_ValuePerCall | SFRM_Materialize);
	perfunc->rsinfo.returnMode = SFRM_ValuePerCall;
	/* isDone is filled below */
	perfunc->rsinfo.setResult = NULL;
	perfunc->rsinfo.setDesc = NULL;
	perfunc->tstore = NULL;

	/*
	 * Normally the passed expression tree will be a FuncExprState, since the
	 * grammar only allows a function call at the top level of a table
	 * function reference.  However, if the function doesn't return set then
	 * the planner might have replaced the function call via constant-folding
	 * or inlining.  So if we see any other kind of expression node, execute
	 * it via the general ExecEvalExpr() code; the only difference is that we
	 * don't get a chance to pass a special ReturnSetInfo to any functions
	 * buried in the expression.
	 */
	if (setexpr && IsA(setexpr, SetExprState) &&
		IsA(setexpr->expr, FuncExpr))
 	{
		/*
		 * This path is similar to ExecMakeFunctionResult.
		 */
		direct_function_call = true;

		/*
		 * Initialize function cache if first time through
		 */
		if (!perfunc->started)
 		{
//			ReturnSetInfo rsinfo;
//

			//setexpr =
			//	ExecInitFunctionResultSet(setexpr->expr, econtext, NULL);

			ReturnSetInfo *rsinfo = palloc0(sizeof(ReturnSetInfo));
			rsinfo->type = T_ReturnSetInfo;
			rsinfo->econtext = econtext;
			rsinfo->expectedDesc = setexpr->funcResultDesc;
			rsinfo->allowedModes = (int) (SFRM_ValuePerCall | SFRM_Materialize);
			/* note we do not set SFRM_Materialize_Random or _Preferred */
			rsinfo->returnMode = SFRM_ValuePerCall;
			/* isDone is filled below */
			rsinfo->setResult = NULL;
			rsinfo->setDesc = NULL;

			setexpr->fcinfo->resultinfo = rsinfo;
			perfunc->fcinfo = setexpr->fcinfo;

			perfunc->started = true;

			perfunc->func_slot = node->ss.ss_ScanTupleSlot;

		}

		returnsSet = setexpr->func.fn_retset;

		/*
		 * Evaluate the function's argument list.
		 *
		 * We can't do this in the per-tuple context: the argument values
		 * would disappear when we reset that context in the inner loop.  And
		 * the caller's CurrentMemoryContext is typically a query-lifespan
		 * context, so we don't want to leak memory there.  We require the
		 * caller to pass a separate memory context that can be used for this,
		 * and can be reset each time the node is re-scanned.
		 */
		oldcontext = MemoryContextSwitchTo(node->argcontext);
		ExecEvalFuncArgs(perfunc->fcinfo, setexpr->args, econtext);
		MemoryContextSwitchTo(oldcontext);

		/*
		 * If function is strict, and there are any NULL arguments, skip
		 * calling the function and act like it returned NULL (or an empty
		 * set, in the returns-set case).
		 */
		if (setexpr->func.fn_strict)
		{
			int			i;

			for (i = 0; i < perfunc->fcinfo->nargs; i++)
 			{
				if (perfunc->fcinfo->args[i].isnull)
					goto no_function_result;
 			}
		}
	}
	else
	{
		/* Treat funcexpr as a generic expression */
		direct_function_call = false;
	}

	/*
	 * Switch to short-lived context for calling the function or expression.
	 */
	MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/*
	 * reset per-tuple memory context before each call of the function or
	 * expression. This cleans up any local memory the function may leak
	 * when called.
	 */
	ResetExprContext(econtext);

	/* Call the function or expression one time */
	if (direct_function_call)
	{
		PgStat_FunctionCallUsage fcusage;

		pgstat_init_function_usage(perfunc->fcinfo, &fcusage);

		perfunc->fcinfo->isnull = false;
		perfunc->rsinfo.isDone = ExprMultipleResult;
		result = FunctionCallInvoke(perfunc->fcinfo);

		pgstat_end_function_usage(&fcusage,
								  perfunc->rsinfo.isDone != ExprMultipleResult);
	}
	else
	{
		perfunc->rsinfo.isDone = ExprSingleResult;
		result = ExecEvalExpr(setexpr->elidedFuncState, econtext,
							  &perfunc->fcinfo->isnull);

		/* done after this, will use SFRM_ValuePerCall branch below */
	}

	/* Which protocol does function want to use? */
	if (perfunc->rsinfo.returnMode == SFRM_ValuePerCall)
	{
		/*
		 * Check for end of result set.
		 */
		if (perfunc->rsinfo.isDone == ExprEndResult)
			goto no_function_result;

		/*
		 * Store current resultset item.
		 */
		if (perfunc->returnsTuple)
		{
			if (!perfunc->fcinfo->isnull)
			{
				HeapTupleHeader td = DatumGetHeapTupleHeader(result);
				HeapTupleData tmptup;

				if (perfunc->rsinfo.setDesc == NULL)
				{
					/*
					 * This is the first non-NULL result from the
					 * function.  Use the type info embedded in the
					 * rowtype Datum to look up the needed tupdesc.  Make
					 * a copy for the query.
					 */
					oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);
					perfunc->rsinfo.setDesc =
						lookup_rowtype_tupdesc_copy(HeapTupleHeaderGetTypeId(td),
													HeapTupleHeaderGetTypMod(td));
					MemoryContextSwitchTo(oldcontext);

					/*
					 * Cross-check tupdesc.  We only really need to do this
					 * for functions returning RECORD, but might as well do it
					 * always.
					 */
					tupledesc_match(perfunc->tupdesc, perfunc->rsinfo.setDesc);
				}

				tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
				tmptup.t_data = td;

				ExecStoreHeapTuple(&tmptup, perfunc->func_slot, false);
				/* materializing handles expanded and toasted datums */
				/* XXX: would be nice if this could be optimized away */
				ExecMaterializeSlot(perfunc->func_slot);
			}
			else
			{
				/*
				 * NULL result from a tuple-returning function; expand it
				 * to a row of all nulls.
				 */
				ExecStoreAllNullTuple(perfunc->func_slot);
			}
		}
		else
		{
			/*
			 * Scalar-type case: just store the function result
			 */
			ExecClearTuple(perfunc->func_slot);
			perfunc->func_slot->tts_values[0] = result;
			perfunc->func_slot->tts_isnull[0] = perfunc->fcinfo->isnull;
			ExecStoreVirtualTuple(perfunc->func_slot);

			/* materializing handles expanded and toasted datums */
			ExecMaterializeSlot(perfunc->func_slot);
		}
	}
	else if (perfunc->rsinfo.returnMode == SFRM_Materialize)
	{
		EState	   *estate;
		ScanDirection direction;

		estate = node->ss.ps.state;
		direction = estate->es_direction;

		/* check we're on the same page as the function author */
		if (perfunc->rsinfo.isDone != ExprSingleResult)
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_SRF_PROTOCOL_VIOLATED),
					 errmsg("table-function protocol for materialize mode was not followed")));

		if (perfunc->rsinfo.setResult != NULL)
		{
			perfunc->tstore = perfunc->rsinfo.setResult;

			/*
			 * paranoia - cope if the function, which may have constructed the
			 * tuplestore itself, didn't leave it pointing at the start. This
			 * call is fast, so the overhead shouldn't be an issue.
			 */
			tuplestore_rescan(perfunc->rsinfo.setResult);

			/*
			 * If function provided a tupdesc, cross-check it.  We only really need to
			 * do this for functions returning RECORD, but might as well do it always.
			 */
			if (perfunc->rsinfo.setDesc)
			{
				tupledesc_match(perfunc->tupdesc, perfunc->rsinfo.setDesc);

				/*
				 * If it is a dynamically-allocated TupleDesc, free it: it is
				 * typically allocated in a per-query context, so we must avoid
				 * leaking it across multiple usages.
				 */
				if (perfunc->rsinfo.setDesc->tdrefcount == -1)
				{
					FreeTupleDesc(perfunc->rsinfo.setDesc);
					perfunc->rsinfo.setDesc = NULL;
				}
			}

			/* and return first row */
			(void) tuplestore_gettupleslot(perfunc->rsinfo.setResult,
										   ScanDirectionIsForward(direction),
										   false,
										   perfunc->func_slot);
		}
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_SRF_PROTOCOL_VIOLATED),
				 errmsg("unrecognized table-function returnMode: %d",
						(int) perfunc->rsinfo.returnMode)));
	goto done;

no_function_result:
	MemoryContextSwitchTo(callerContext);

	/*
	 * If we got nothing from the function (ie, an empty-set or NULL
	 * result), we have to manufacture a result. I.e. if it's a
	 * non-set-returning function then return a single all-nulls row.
	 */
	perfunc->rsinfo.isDone = ExprEndResult;
	if (returnsSet)
		ExecClearTuple(perfunc->func_slot);
	else
		ExecStoreAllNullTuple(perfunc->func_slot);
done:
	MemoryContextSwitchTo(callerContext);
}

static void
ExecNextFunctionResult(FunctionScanState *node,
					   FunctionScanPerFuncState *perfunc)
{
	EState	   *estate;
	ScanDirection direction;
	MemoryContext callerContext;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;

	estate = node->ss.ps.state;
	direction = estate->es_direction;

	callerContext = CurrentMemoryContext;

	if (perfunc->tstore)
	{
		(void) tuplestore_gettupleslot(perfunc->tstore,
									   ScanDirectionIsForward(direction),
									   false,
									   perfunc->func_slot);
	}
	else if (perfunc->rsinfo.isDone == ExprSingleResult ||
			 perfunc->rsinfo.isDone == ExprEndResult)
	{
		ExecClearTuple(perfunc->func_slot);
	}
	else
	{
		Datum result;
		PgStat_FunctionCallUsage fcusage;

		/* ensure called in a sane context */
		Assert(perfunc->rsinfo.returnMode == SFRM_ValuePerCall);

		/*
		 * Switch to short-lived context for calling the function or expression.
		 */
		MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

		/* next call in percall mode */
		pgstat_init_function_usage(perfunc->fcinfo, &fcusage);

		perfunc->fcinfo->isnull = false;
		//perfunc->rsinfo.isDone = ExprSingleResult;
		result = FunctionCallInvoke(perfunc->fcinfo);


		//elog(WARNING, "perfunc->fcinfo->resultinfo: %d", ((ReturnSetInfo *)(perfunc->fcinfo)->resultinfo)->isDone);
		//elog(WARNING, "perfunc->rsinfo.isDone: %d", perfunc->rsinfo.isDone);

		int isDone = ((ReturnSetInfo *)(perfunc->fcinfo)->resultinfo)->isDone;
		pgstat_end_function_usage(&fcusage,
								isDone != ExprMultipleResult);

		Assert(perfunc->rsinfo.returnMode == SFRM_ValuePerCall);

		if (isDone == ExprEndResult)
		{
			ExecClearTuple(perfunc->func_slot);
			goto out;
		}

		if (perfunc->returnsTuple)
		{
			if (!perfunc->fcinfo->isnull)
			{
				HeapTupleHeader td = DatumGetHeapTupleHeader(result);
				HeapTupleData tmptup;
				TupleDesc tupdesc;

				if (perfunc->rsinfo.setDesc == NULL)
				{
					MemoryContext oldcontext;

					/*
					 * This is the first non-NULL result from the
					 * function.  Use the type info embedded in the
					 * rowtype Datum to look up the needed tupdesc.  Make
					 * a copy for the query.
					 */
					oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);
					perfunc->rsinfo.setDesc =
						lookup_rowtype_tupdesc_copy(HeapTupleHeaderGetTypeId(td),
													HeapTupleHeaderGetTypMod(td));
					MemoryContextSwitchTo(oldcontext);

					/*
					 * Cross-check tupdesc.  We only really need to do this
					 * for functions returning RECORD, but might as well do it
					 * always.
					 */
					tupledesc_match(perfunc->tupdesc, perfunc->rsinfo.setDesc);
				}

				tupdesc = perfunc->rsinfo.setDesc;

				/*
				 * Verify all later returned rows have same subtype;
				 * necessary in case the type is RECORD.
				 */
				if (HeapTupleHeaderGetTypeId(td) != tupdesc->tdtypeid ||
					HeapTupleHeaderGetTypMod(td) != tupdesc->tdtypmod)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("rows returned by function are not all of the same row type")));

				tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
				tmptup.t_data = td;

				ExecStoreHeapTuple(&tmptup, perfunc->func_slot, false);
				/* materializing handles expanded and toasted datums */
				/* XXX: would be nice if this could be optimized away */
				ExecMaterializeSlot(perfunc->func_slot);
			}
			else
			{
				ExecStoreAllNullTuple(perfunc->func_slot);
			}
		}
		else
		{
			/* Scalar-type case: just store the function result */
			ExecClearTuple(perfunc->func_slot);
			perfunc->func_slot->tts_values[0] = result;
			perfunc->func_slot->tts_isnull[0] = perfunc->fcinfo->isnull;
			ExecStoreVirtualTuple(perfunc->func_slot);

			/* materializing handles expanded and toasted datums */
			ExecMaterializeSlot(perfunc->func_slot);
 		}
 	}

out:
	MemoryContextSwitchTo(callerContext);
}

/*
 * Check that function result tuple type (src_tupdesc) matches or can
 * be considered to match what the query expects (dst_tupdesc). If
 * they don't match, ereport.
 *
 * We really only care about number of attributes and data type.
 * Also, we can ignore type mismatch on columns that are dropped in the
 * destination type, so long as the physical storage matches.  This is
 * helpful in some cases involving out-of-date cached plans.
 */
static void
tupledesc_match(TupleDesc dst_tupdesc, TupleDesc src_tupdesc)
{
	int			i;

	if (dst_tupdesc->natts != src_tupdesc->natts)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("function return row and query-specified return row do not match"),
				 errdetail_plural("Returned row contains %d attribute, but query expects %d.",
				"Returned row contains %d attributes, but query expects %d.",
								  src_tupdesc->natts,
								  src_tupdesc->natts, dst_tupdesc->natts)));

	for (i = 0; i < dst_tupdesc->natts; i++)
 	{
		Form_pg_attribute dattr = &dst_tupdesc->attrs[i];
		Form_pg_attribute sattr = &src_tupdesc->attrs[i];

		if (IsBinaryCoercible(sattr->atttypid, dattr->atttypid))
			continue;			/* no worries */
		if (!dattr->attisdropped)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("function return row and query-specified return row do not match"),
					 errdetail("Returned type %s at ordinal position %d, but query expects %s.",
							   format_type_be(sattr->atttypid),
							   i + 1,
							   format_type_be(dattr->atttypid))));

		if (dattr->attlen != sattr->attlen ||
			dattr->attalign != sattr->attalign)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("function return row and query-specified return row do not match"),
					 errdetail("Physical storage mismatch on dropped attribute at ordinal position %d.",
							   i + 1)));
 	}
 }
