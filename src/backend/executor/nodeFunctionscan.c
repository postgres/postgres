/*-------------------------------------------------------------------------
 *
 * nodeFunctionscan.c
 *	  Support routines for scanning RangeFunctions (functions in rangetable).
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeFunctionscan.c,v 1.22 2003/09/25 23:02:12 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecFunctionScan		scans a function.
 *		ExecFunctionNext		retrieve next tuple in sequential order.
 *		ExecInitFunctionScan	creates and initializes a functionscan node.
 *		ExecEndFunctionScan		releases any storage allocated.
 *		ExecFunctionReScan		rescans the function
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/pg_type.h"
#include "executor/execdebug.h"
#include "executor/execdefs.h"
#include "executor/execdesc.h"
#include "executor/nodeFunctionscan.h"
#include "parser/parsetree.h"
#include "parser/parse_expr.h"
#include "parser/parse_type.h"
#include "utils/lsyscache.h"


static TupleTableSlot *FunctionNext(FunctionScanState *node);
static bool tupledesc_match(TupleDesc dst_tupdesc, TupleDesc src_tupdesc);

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
	TupleTableSlot *slot;
	EState	   *estate;
	ScanDirection direction;
	Tuplestorestate *tuplestorestate;
	bool		should_free;
	HeapTuple	heapTuple;

	/*
	 * get information from the estate and scan state
	 */
	estate = node->ss.ps.state;
	direction = estate->es_direction;

	tuplestorestate = node->tuplestorestate;

	/*
	 * If first time through, read all tuples from function and put them
	 * in a tuplestore. Subsequent calls just fetch tuples from
	 * tuplestore.
	 */
	if (tuplestorestate == NULL)
	{
		ExprContext *econtext = node->ss.ps.ps_ExprContext;
		TupleDesc	funcTupdesc;

		node->tuplestorestate = tuplestorestate =
			ExecMakeTableFunctionResult(node->funcexpr,
										econtext,
										node->tupdesc,
										&funcTupdesc);

		/*
		 * If function provided a tupdesc, cross-check it.	We only really
		 * need to do this for functions returning RECORD, but might as
		 * well do it always.
		 */
		if (funcTupdesc && !tupledesc_match(node->tupdesc, funcTupdesc))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("query-specified return row and actual function return row do not match")));
	}

	/*
	 * Get the next tuple from tuplestore. Return NULL if no more tuples.
	 */
	slot = node->ss.ss_ScanTupleSlot;
	if (tuplestorestate)
		heapTuple = tuplestore_getheaptuple(tuplestorestate,
									   ScanDirectionIsForward(direction),
											&should_free);
	else
	{
		heapTuple = NULL;
		should_free = false;
	}

	return ExecStoreTuple(heapTuple, slot, InvalidBuffer, should_free);
}

/* ----------------------------------------------------------------
 *		ExecFunctionScan(node)
 *
 *		Scans the function sequentially and returns the next qualifying
 *		tuple.
 *		It calls the ExecScan() routine and passes it the access method
 *		which retrieves tuples sequentially.
 *
 */

TupleTableSlot *
ExecFunctionScan(FunctionScanState *node)
{
	/*
	 * use FunctionNext as access method
	 */
	return ExecScan(&node->ss, (ExecScanAccessMtd) FunctionNext);
}

/* ----------------------------------------------------------------
 *		ExecInitFunctionScan
 * ----------------------------------------------------------------
 */
FunctionScanState *
ExecInitFunctionScan(FunctionScan *node, EState *estate)
{
	FunctionScanState *scanstate;
	RangeTblEntry *rte;
	Oid			funcrettype;
	char		functyptype;
	TupleDesc	tupdesc = NULL;

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

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

#define FUNCTIONSCAN_NSLOTS 2

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &scanstate->ss.ps);
	ExecInitScanTupleSlot(estate, &scanstate->ss);

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
	 * get info about function
	 */
	rte = rt_fetch(node->scan.scanrelid, estate->es_range_table);
	Assert(rte->rtekind == RTE_FUNCTION);
	funcrettype = exprType(rte->funcexpr);

	/*
	 * Now determine if the function returns a simple or composite type,
	 * and build an appropriate tupdesc.
	 */
	functyptype = get_typtype(funcrettype);

	if (functyptype == 'c')
	{
		/*
		 * Composite data type, i.e. a table's row type
		 */
		Oid			funcrelid;
		Relation	rel;

		funcrelid = typeidTypeRelid(funcrettype);
		if (!OidIsValid(funcrelid))
			elog(ERROR, "invalid typrelid for complex type %u",
				 funcrettype);
		rel = relation_open(funcrelid, AccessShareLock);
		tupdesc = CreateTupleDescCopy(RelationGetDescr(rel));
		relation_close(rel, AccessShareLock);
	}
	else if (functyptype == 'b' || functyptype == 'd')
	{
		/*
		 * Must be a base data type, i.e. scalar
		 */
		char	   *attname = strVal(lfirst(rte->eref->colnames));

		tupdesc = CreateTemplateTupleDesc(1, false);
		TupleDescInitEntry(tupdesc,
						   (AttrNumber) 1,
						   attname,
						   funcrettype,
						   -1,
						   0,
						   false);
	}
	else if (functyptype == 'p' && funcrettype == RECORDOID)
	{
		/*
		 * Must be a pseudo type, i.e. record
		 */
		tupdesc = BuildDescForRelation(rte->coldeflist);
	}
	else
	{
		/* crummy error message, but parser should have caught this */
		elog(ERROR, "function in FROM has unsupported return type");
	}

	scanstate->tupdesc = tupdesc;
	ExecSetSlotDescriptor(scanstate->ss.ss_ScanTupleSlot,
						  tupdesc, false);

	/*
	 * Other node-specific setup
	 */
	scanstate->tuplestorestate = NULL;
	scanstate->funcexpr = ExecInitExpr((Expr *) rte->funcexpr,
									   (PlanState *) scanstate);

	scanstate->ss.ps.ps_TupFromTlist = false;

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&scanstate->ss.ps);
	ExecAssignProjectionInfo(&scanstate->ss.ps);

	return scanstate;
}

int
ExecCountSlotsFunctionScan(FunctionScan *node)
{
	return ExecCountSlotsNode(outerPlan(node)) +
		ExecCountSlotsNode(innerPlan(node)) +
		FUNCTIONSCAN_NSLOTS;
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

	/*
	 * Release tuplestore resources
	 */
	if (node->tuplestorestate != NULL)
		tuplestore_end(node->tuplestorestate);
	node->tuplestorestate = NULL;
}

/* ----------------------------------------------------------------
 *		ExecFunctionMarkPos
 *
 *		Calls tuplestore to save the current position in the stored file.
 * ----------------------------------------------------------------
 */
void
ExecFunctionMarkPos(FunctionScanState *node)
{
	/*
	 * if we haven't materialized yet, just return.
	 */
	if (!node->tuplestorestate)
		return;

	tuplestore_markpos(node->tuplestorestate);
}

/* ----------------------------------------------------------------
 *		ExecFunctionRestrPos
 *
 *		Calls tuplestore to restore the last saved file position.
 * ----------------------------------------------------------------
 */
void
ExecFunctionRestrPos(FunctionScanState *node)
{
	/*
	 * if we haven't materialized yet, just return.
	 */
	if (!node->tuplestorestate)
		return;

	tuplestore_restorepos(node->tuplestorestate);
}

/* ----------------------------------------------------------------
 *		ExecFunctionReScan
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecFunctionReScan(FunctionScanState *node, ExprContext *exprCtxt)
{
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	/*
	 * If we haven't materialized yet, just return.
	 */
	if (!node->tuplestorestate)
		return;

	/*
	 * Here we have a choice whether to drop the tuplestore (and recompute
	 * the function outputs) or just rescan it.  This should depend on
	 * whether the function expression contains parameters and/or is
	 * marked volatile.  FIXME soon.
	 */
	if (node->ss.ps.chgParam != NULL)
	{
		tuplestore_end(node->tuplestorestate);
		node->tuplestorestate = NULL;
	}
	else
		tuplestore_rescan(node->tuplestorestate);
}

/*
 * Check that function result tuple type (src_tupdesc) matches or can be
 * considered to match what the query expects (dst_tupdesc).
 *
 * We really only care about number of attributes and data type.
 * Also, we can ignore type mismatch on columns that are dropped in the
 * destination type, so long as the physical storage matches.  This is
 * helpful in some cases involving out-of-date cached plans.
 */
static bool
tupledesc_match(TupleDesc dst_tupdesc, TupleDesc src_tupdesc)
{
	int			i;

	if (dst_tupdesc->natts != src_tupdesc->natts)
		return false;

	for (i = 0; i < dst_tupdesc->natts; i++)
	{
		Form_pg_attribute dattr = dst_tupdesc->attrs[i];
		Form_pg_attribute sattr = src_tupdesc->attrs[i];

		if (dattr->atttypid == sattr->atttypid)
			continue;			/* no worries */
		if (!dattr->attisdropped)
			return false;
		if (dattr->attlen != sattr->attlen ||
			dattr->attalign != sattr->attalign)
			return false;
	}

	return true;
}
