/*-------------------------------------------------------------------------
 *
 * nodeTableFuncscan.c
 *	  Support routines for scanning RangeTableFunc (XMLTABLE like functions).
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeTableFuncscan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecTableFuncScan		scans a function.
 *		ExecFunctionNext		retrieve next tuple in sequential order.
 *		ExecInitTableFuncScan	creates and initializes a TableFuncscan node.
 *		ExecEndTableFuncScan		releases any storage allocated.
 *		ExecReScanTableFuncScan rescans the function
 */
#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeTableFuncscan.h"
#include "executor/tablefunc.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/xml.h"

static TupleTableSlot *TableFuncNext(TableFuncScanState *node);
static bool TableFuncRecheck(TableFuncScanState *node, TupleTableSlot *slot);

static void tfuncFetchRows(TableFuncScanState *tstate, ExprContext *econtext);
static void tfuncInitialize(TableFuncScanState *tstate, ExprContext *econtext, Datum doc);
static void tfuncLoadRows(TableFuncScanState *tstate, ExprContext *econtext);

/* ----------------------------------------------------------------
 *						Scan Support
 * ----------------------------------------------------------------
 */
/* ----------------------------------------------------------------
 *		TableFuncNext
 *
 *		This is a workhorse for ExecTableFuncScan
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
TableFuncNext(TableFuncScanState *node)
{
	TupleTableSlot *scanslot;

	scanslot = node->ss.ss_ScanTupleSlot;

	/*
	 * If first time through, read all tuples from function and put them in a
	 * tuplestore. Subsequent calls just fetch tuples from tuplestore.
	 */
	if (node->tupstore == NULL)
		tfuncFetchRows(node, node->ss.ps.ps_ExprContext);

	/*
	 * Get the next tuple from tuplestore.
	 */
	(void) tuplestore_gettupleslot(node->tupstore,
								   true,
								   false,
								   scanslot);
	return scanslot;
}

/*
 * TableFuncRecheck -- access method routine to recheck a tuple in EvalPlanQual
 */
static bool
TableFuncRecheck(TableFuncScanState *node, TupleTableSlot *slot)
{
	/* nothing to check */
	return true;
}

/* ----------------------------------------------------------------
 *		ExecTableFuncScan(node)
 *
 *		Scans the function sequentially and returns the next qualifying
 *		tuple.
 *		We call the ExecScan() routine and pass it the appropriate
 *		access method functions.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecTableFuncScan(PlanState *pstate)
{
	TableFuncScanState *node = castNode(TableFuncScanState, pstate);

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) TableFuncNext,
					(ExecScanRecheckMtd) TableFuncRecheck);
}

/* ----------------------------------------------------------------
 *		ExecInitTableFuncScan
 * ----------------------------------------------------------------
 */
TableFuncScanState *
ExecInitTableFuncScan(TableFuncScan *node, EState *estate, int eflags)
{
	TableFuncScanState *scanstate;
	TableFunc  *tf = node->tablefunc;
	TupleDesc	tupdesc;
	int			i;

	/* check for unsupported flags */
	Assert(!(eflags & EXEC_FLAG_MARK));

	/*
	 * TableFuncscan should not have any children.
	 */
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * create new ScanState for node
	 */
	scanstate = makeNode(TableFuncScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;
	scanstate->ss.ps.ExecProcNode = ExecTableFuncScan;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	/*
	 * initialize source tuple type
	 */
	tupdesc = BuildDescFromLists(tf->colnames,
								 tf->coltypes,
								 tf->coltypmods,
								 tf->colcollations);
	/* and the corresponding scan slot */
	ExecInitScanTupleSlot(estate, &scanstate->ss, tupdesc,
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
		ExecInitQual(node->scan.plan.qual, &scanstate->ss.ps);

	/* Only XMLTABLE is supported currently */
	scanstate->routine = &XmlTableRoutine;

	scanstate->perTableCxt =
		AllocSetContextCreate(CurrentMemoryContext,
							  "TableFunc per value context",
							  ALLOCSET_DEFAULT_SIZES);
	scanstate->opaque = NULL;	/* initialized at runtime */

	scanstate->ns_names = tf->ns_names;

	scanstate->ns_uris =
		ExecInitExprList(tf->ns_uris, (PlanState *) scanstate);
	scanstate->docexpr =
		ExecInitExpr((Expr *) tf->docexpr, (PlanState *) scanstate);
	scanstate->rowexpr =
		ExecInitExpr((Expr *) tf->rowexpr, (PlanState *) scanstate);
	scanstate->colexprs =
		ExecInitExprList(tf->colexprs, (PlanState *) scanstate);
	scanstate->coldefexprs =
		ExecInitExprList(tf->coldefexprs, (PlanState *) scanstate);

	scanstate->notnulls = tf->notnulls;

	/* these are allocated now and initialized later */
	scanstate->in_functions = palloc(sizeof(FmgrInfo) * tupdesc->natts);
	scanstate->typioparams = palloc(sizeof(Oid) * tupdesc->natts);

	/*
	 * Fill in the necessary fmgr infos.
	 */
	for (i = 0; i < tupdesc->natts; i++)
	{
		Oid			in_funcid;

		getTypeInputInfo(TupleDescAttr(tupdesc, i)->atttypid,
						 &in_funcid, &scanstate->typioparams[i]);
		fmgr_info(in_funcid, &scanstate->in_functions[i]);
	}

	return scanstate;
}

/* ----------------------------------------------------------------
 *		ExecEndTableFuncScan
 *
 *		frees any storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndTableFuncScan(TableFuncScanState *node)
{
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
	 * Release tuplestore resources
	 */
	if (node->tupstore != NULL)
		tuplestore_end(node->tupstore);
	node->tupstore = NULL;
}

/* ----------------------------------------------------------------
 *		ExecReScanTableFuncScan
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecReScanTableFuncScan(TableFuncScanState *node)
{
	Bitmapset  *chgparam = node->ss.ps.chgParam;

	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecScanReScan(&node->ss);

	/*
	 * Recompute when parameters are changed.
	 */
	if (chgparam)
	{
		if (node->tupstore != NULL)
		{
			tuplestore_end(node->tupstore);
			node->tupstore = NULL;
		}
	}

	if (node->tupstore != NULL)
		tuplestore_rescan(node->tupstore);
}

/* ----------------------------------------------------------------
 *		tfuncFetchRows
 *
 *		Read rows from a TableFunc producer
 * ----------------------------------------------------------------
 */
static void
tfuncFetchRows(TableFuncScanState *tstate, ExprContext *econtext)
{
	const TableFuncRoutine *routine = tstate->routine;
	MemoryContext oldcxt;
	Datum		value;
	bool		isnull;

	Assert(tstate->opaque == NULL);

	/* build tuplestore for the result */
	oldcxt = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);
	tstate->tupstore = tuplestore_begin_heap(false, false, work_mem);

	/*
	 * Each call to fetch a new set of rows - of which there may be very many
	 * if XMLTABLE is being used in a lateral join - will allocate a possibly
	 * substantial amount of memory, so we cannot use the per-query context
	 * here. perTableCxt now serves the same function as "argcontext" does in
	 * FunctionScan - a place to store per-one-call (i.e. one result table)
	 * lifetime data (as opposed to per-query or per-result-tuple).
	 */
	MemoryContextSwitchTo(tstate->perTableCxt);

	PG_TRY();
	{
		routine->InitOpaque(tstate,
							tstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor->natts);

		/*
		 * If evaluating the document expression returns NULL, the table
		 * expression is empty and we return immediately.
		 */
		value = ExecEvalExpr(tstate->docexpr, econtext, &isnull);

		if (!isnull)
		{
			/* otherwise, pass the document value to the table builder */
			tfuncInitialize(tstate, econtext, value);

			/* initialize ordinality counter */
			tstate->ordinal = 1;

			/* Load all rows into the tuplestore, and we're done */
			tfuncLoadRows(tstate, econtext);
		}
	}
	PG_CATCH();
	{
		if (tstate->opaque != NULL)
			routine->DestroyOpaque(tstate);
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* clean up and return to original memory context */

	if (tstate->opaque != NULL)
	{
		routine->DestroyOpaque(tstate);
		tstate->opaque = NULL;
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextReset(tstate->perTableCxt);
}

/*
 * Fill in namespace declarations, the row filter, and column filters in a
 * table expression builder context.
 */
static void
tfuncInitialize(TableFuncScanState *tstate, ExprContext *econtext, Datum doc)
{
	const TableFuncRoutine *routine = tstate->routine;
	TupleDesc	tupdesc;
	ListCell   *lc1,
			   *lc2;
	bool		isnull;
	int			colno;
	Datum		value;
	int			ordinalitycol =
		((TableFuncScan *) (tstate->ss.ps.plan))->tablefunc->ordinalitycol;

	/*
	 * Install the document as a possibly-toasted Datum into the tablefunc
	 * context.
	 */
	routine->SetDocument(tstate, doc);

	/* Evaluate namespace specifications */
	forboth(lc1, tstate->ns_uris, lc2, tstate->ns_names)
	{
		ExprState  *expr = (ExprState *) lfirst(lc1);
		String	   *ns_node = lfirst_node(String, lc2);
		char	   *ns_uri;
		char	   *ns_name;

		value = ExecEvalExpr((ExprState *) expr, econtext, &isnull);
		if (isnull)
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("namespace URI must not be null")));
		ns_uri = TextDatumGetCString(value);

		/* DEFAULT is passed down to SetNamespace as NULL */
		ns_name = ns_node ? strVal(ns_node) : NULL;

		routine->SetNamespace(tstate, ns_name, ns_uri);
	}

	/* Install the row filter expression into the table builder context */
	value = ExecEvalExpr(tstate->rowexpr, econtext, &isnull);
	if (isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("row filter expression must not be null")));

	routine->SetRowFilter(tstate, TextDatumGetCString(value));

	/*
	 * Install the column filter expressions into the table builder context.
	 * If an expression is given, use that; otherwise the column name itself
	 * is the column filter.
	 */
	colno = 0;
	tupdesc = tstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	foreach(lc1, tstate->colexprs)
	{
		char	   *colfilter;
		Form_pg_attribute att = TupleDescAttr(tupdesc, colno);

		if (colno != ordinalitycol)
		{
			ExprState  *colexpr = lfirst(lc1);

			if (colexpr != NULL)
			{
				value = ExecEvalExpr(colexpr, econtext, &isnull);
				if (isnull)
					ereport(ERROR,
							(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
							 errmsg("column filter expression must not be null"),
							 errdetail("Filter for column \"%s\" is null.",
									   NameStr(att->attname))));
				colfilter = TextDatumGetCString(value);
			}
			else
				colfilter = NameStr(att->attname);

			routine->SetColumnFilter(tstate, colfilter, colno);
		}

		colno++;
	}
}

/*
 * Load all the rows from the TableFunc table builder into a tuplestore.
 */
static void
tfuncLoadRows(TableFuncScanState *tstate, ExprContext *econtext)
{
	const TableFuncRoutine *routine = tstate->routine;
	TupleTableSlot *slot = tstate->ss.ss_ScanTupleSlot;
	TupleDesc	tupdesc = slot->tts_tupleDescriptor;
	Datum	   *values = slot->tts_values;
	bool	   *nulls = slot->tts_isnull;
	int			natts = tupdesc->natts;
	MemoryContext oldcxt;
	int			ordinalitycol;

	ordinalitycol =
		((TableFuncScan *) (tstate->ss.ps.plan))->tablefunc->ordinalitycol;

	/*
	 * We need a short-lived memory context that we can clean up each time
	 * around the loop, to avoid wasting space. Our default per-tuple context
	 * is fine for the job, since we won't have used it for anything yet in
	 * this tuple cycle.
	 */
	oldcxt = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/*
	 * Keep requesting rows from the table builder until there aren't any.
	 */
	while (routine->FetchRow(tstate))
	{
		ListCell   *cell = list_head(tstate->coldefexprs);
		int			colno;

		CHECK_FOR_INTERRUPTS();

		ExecClearTuple(tstate->ss.ss_ScanTupleSlot);

		/*
		 * Obtain the value of each column for this row, installing them into
		 * the slot; then add the tuple to the tuplestore.
		 */
		for (colno = 0; colno < natts; colno++)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, colno);

			if (colno == ordinalitycol)
			{
				/* Fast path for ordinality column */
				values[colno] = Int32GetDatum(tstate->ordinal++);
				nulls[colno] = false;
			}
			else
			{
				bool		isnull;

				values[colno] = routine->GetValue(tstate,
												  colno,
												  att->atttypid,
												  att->atttypmod,
												  &isnull);

				/* No value?  Evaluate and apply the default, if any */
				if (isnull && cell != NULL)
				{
					ExprState  *coldefexpr = (ExprState *) lfirst(cell);

					if (coldefexpr != NULL)
						values[colno] = ExecEvalExpr(coldefexpr, econtext,
													 &isnull);
				}

				/* Verify a possible NOT NULL constraint */
				if (isnull && bms_is_member(colno, tstate->notnulls))
					ereport(ERROR,
							(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
							 errmsg("null is not allowed in column \"%s\"",
									NameStr(att->attname))));

				nulls[colno] = isnull;
			}

			/* advance list of default expressions */
			if (cell != NULL)
				cell = lnext(tstate->coldefexprs, cell);
		}

		tuplestore_putvalues(tstate->tupstore, tupdesc, values, nulls);

		MemoryContextReset(econtext->ecxt_per_tuple_memory);
	}

	MemoryContextSwitchTo(oldcxt);
}
