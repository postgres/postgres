/*-------------------------------------------------------------------------
 *
 * nodeFunctionscan.c
 *	  Support routines for scanning RangeFunctions (functions in rangetable).
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeFunctionscan.c,v 1.4 2002/08/04 19:48:09 momjian Exp $
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

#include "miscadmin.h"
#include "access/heapam.h"
#include "catalog/pg_type.h"
#include "executor/execdebug.h"
#include "executor/execdefs.h"
#include "executor/execdesc.h"
#include "executor/nodeFunctionscan.h"
#include "parser/parsetree.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "storage/lmgr.h"
#include "tcop/pquery.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"

static TupleTableSlot *FunctionNext(FunctionScan *node);
static TupleTableSlot *function_getonetuple(FunctionScanState *scanstate,
											bool *isNull,
											ExprDoneCond *isDone);
static FunctionMode get_functionmode(Node *expr);
static bool tupledesc_mismatch(TupleDesc tupdesc1, TupleDesc tupdesc2);

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
FunctionNext(FunctionScan *node)
{
	TupleTableSlot	   *slot;
	EState			   *estate;
	ScanDirection		direction;
	Tuplestorestate	   *tuplestorestate;
	FunctionScanState  *scanstate;
	bool				should_free;
	HeapTuple			heapTuple;

	/*
	 * get information from the estate and scan state
	 */
	scanstate = (FunctionScanState *) node->scan.scanstate;
	estate = node->scan.plan.state;
	direction = estate->es_direction;

	tuplestorestate = scanstate->tuplestorestate;

	/*
	 * If first time through, read all tuples from function and pass them to
	 * tuplestore.c. Subsequent calls just fetch tuples from tuplestore.
	 */
	if (tuplestorestate == NULL)
	{
		/*
		 * Initialize tuplestore module.
		 */
		tuplestorestate = tuplestore_begin_heap(true,	/* randomAccess */
												SortMem);

		scanstate->tuplestorestate = (void *) tuplestorestate;

		/*
		 * Compute all the function tuples and pass to tuplestore.
		 */
		for (;;)
		{
			bool				isNull;
			ExprDoneCond		isDone;

			isNull = false;
			isDone = ExprSingleResult;
			slot = function_getonetuple(scanstate, &isNull, &isDone);
			if (TupIsNull(slot))
				break;

			tuplestore_puttuple(tuplestorestate, (void *) slot->val);
			ExecClearTuple(slot);

			if (isDone != ExprMultipleResult)
				break;
		}

		/*
		 * Complete the store.
		 */
		tuplestore_donestoring(tuplestorestate);
	}

	/*
	 * Get the next tuple from tuplestore. Return NULL if no more tuples.
	 */
	slot = scanstate->csstate.css_ScanTupleSlot;
	heapTuple = tuplestore_getheaptuple(tuplestorestate,
										ScanDirectionIsForward(direction),
										&should_free);

	return ExecStoreTuple(heapTuple, slot, InvalidBuffer, should_free);
}

/* ----------------------------------------------------------------
 *		ExecFunctionScan(node)
 *
 *		Scans the Function sequentially and returns the next qualifying
 *		tuple.
 *		It calls the ExecScan() routine and passes it the access method
 *		which retrieve tuples sequentially.
 *
 */

TupleTableSlot *
ExecFunctionScan(FunctionScan *node)
{
	/*
	 * use FunctionNext as access method
	 */
	return ExecScan(&node->scan, (ExecScanAccessMtd) FunctionNext);
}

/* ----------------------------------------------------------------
 *		ExecInitFunctionScan
 * ----------------------------------------------------------------
 */
bool
ExecInitFunctionScan(FunctionScan *node, EState *estate, Plan *parent)
{
	FunctionScanState  *scanstate;
	RangeTblEntry	   *rte;
	Oid					funcrettype;
	Oid					funcrelid;
	char				functyptype;
	TupleDesc			tupdesc = NULL;

	/*
	 * FunctionScan should not have any children.
	 */
	Assert(outerPlan((Plan *) node) == NULL);
	Assert(innerPlan((Plan *) node) == NULL);

	/*
	 * assign the node's execution state
	 */
	node->scan.plan.state = estate;

	/*
	 * create new ScanState for node
	 */
	scanstate = makeNode(FunctionScanState);
	node->scan.scanstate = &scanstate->csstate;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &scanstate->csstate.cstate);

#define FUNCTIONSCAN_NSLOTS 2

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &scanstate->csstate.cstate);
	ExecInitScanTupleSlot(estate, &scanstate->csstate);

	/*
	 * get info about function
	 */
	rte = rt_fetch(node->scan.scanrelid, estate->es_range_table);
	Assert(rte->rtekind == RTE_FUNCTION);
	funcrettype = exprType(rte->funcexpr);

	/*
	 * Now determine if the function returns a simple or composite type,
	 * and check/add column aliases.
	 */
	functyptype = typeid_get_typtype(funcrettype);

	/*
	 * Build a suitable tupledesc representing the output rows
	 */
	if (functyptype == 'c')
	{
		funcrelid = typeidTypeRelid(funcrettype);
		if (OidIsValid(funcrelid))
		{
			/*
			 * Composite data type, i.e. a table's row type
			 * Same as ordinary relation RTE
			 */
			Relation	rel;

			rel = relation_open(funcrelid, AccessShareLock);
			tupdesc = CreateTupleDescCopy(RelationGetDescr(rel));
			relation_close(rel, AccessShareLock);
			scanstate->returnsTuple = true;
		}
		else
			elog(ERROR, "Invalid return relation specified for function");
	}
	else if (functyptype == 'b')
	{
		/*
		 * Must be a base data type, i.e. scalar
		 */
		char	   *attname = strVal(lfirst(rte->eref->colnames));

		tupdesc = CreateTemplateTupleDesc(1, WITHOUTOID);
		TupleDescInitEntry(tupdesc,
						   (AttrNumber) 1,
						   attname,
						   funcrettype,
						   -1,
						   0,
						   false);
		scanstate->returnsTuple = false;
	}
	else if (functyptype == 'p' && funcrettype == RECORDOID)
	{
		/*
		 * Must be a pseudo type, i.e. record
		 */
		List *coldeflist = rte->coldeflist;

		tupdesc = BuildDescForRelation(coldeflist);
		scanstate->returnsTuple = true;
	}
	else
		elog(ERROR, "Unknown kind of return type specified for function");

	scanstate->fn_typeid = funcrettype;
	scanstate->fn_typtype = functyptype;
	scanstate->tupdesc = tupdesc;
	ExecSetSlotDescriptor(scanstate->csstate.css_ScanTupleSlot,
						  tupdesc, false);

	/*
	 * Other node-specific setup
	 */
	scanstate->tuplestorestate = NULL;
	scanstate->funcexpr = rte->funcexpr;

	scanstate->functionmode = get_functionmode(rte->funcexpr);

	scanstate->csstate.cstate.cs_TupFromTlist = false;

	/*
	 * initialize tuple type
	 */
	ExecAssignResultTypeFromTL((Plan *) node, &scanstate->csstate.cstate);
	ExecAssignProjectionInfo((Plan *) node, &scanstate->csstate.cstate);

	return TRUE;
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
ExecEndFunctionScan(FunctionScan *node)
{
	FunctionScanState  *scanstate;
	EState			   *estate;

	/*
	 * get information from node
	 */
	scanstate = (FunctionScanState *) node->scan.scanstate;
	estate = node->scan.plan.state;

	/*
	 * Free the projection info and the scan attribute info
	 *
	 * Note: we don't ExecFreeResultType(scanstate) because the rule manager
	 * depends on the tupType returned by ExecMain().  So for now, this is
	 * freed at end-transaction time.  -cim 6/2/91
	 */
	ExecFreeProjectionInfo(&scanstate->csstate.cstate);
	ExecFreeExprContext(&scanstate->csstate.cstate);

	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(scanstate->csstate.cstate.cs_ResultTupleSlot);
	ExecClearTuple(scanstate->csstate.css_ScanTupleSlot);

	/*
	 * Release tuplestore resources
	 */
	if (scanstate->tuplestorestate != NULL)
		tuplestore_end((Tuplestorestate *) scanstate->tuplestorestate);
	scanstate->tuplestorestate = NULL;
}

/* ----------------------------------------------------------------
 *		ExecFunctionMarkPos
 *
 *		Calls tuplestore to save the current position in the stored file.
 * ----------------------------------------------------------------
 */
void
ExecFunctionMarkPos(FunctionScan *node)
{
	FunctionScanState  *scanstate;

	scanstate = (FunctionScanState *) node->scan.scanstate;

	/*
	 * if we haven't materialized yet, just return.
	 */
	if (!scanstate->tuplestorestate)
		return;

	tuplestore_markpos((Tuplestorestate *) scanstate->tuplestorestate);
}

/* ----------------------------------------------------------------
 *		ExecFunctionRestrPos
 *
 *		Calls tuplestore to restore the last saved file position.
 * ----------------------------------------------------------------
 */
void
ExecFunctionRestrPos(FunctionScan *node)
{
	FunctionScanState  *scanstate;

	scanstate = (FunctionScanState *) node->scan.scanstate;

	/*
	 * if we haven't materialized yet, just return.
	 */
	if (!scanstate->tuplestorestate)
		return;

	tuplestore_restorepos((Tuplestorestate *) scanstate->tuplestorestate);
}

/* ----------------------------------------------------------------
 *		ExecFunctionReScan
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecFunctionReScan(FunctionScan *node, ExprContext *exprCtxt, Plan *parent)
{
	FunctionScanState  *scanstate;

	/*
	 * get information from node
	 */
	scanstate = (FunctionScanState *) node->scan.scanstate;

	ExecClearTuple(scanstate->csstate.cstate.cs_ResultTupleSlot);

	/*
	 * If we haven't materialized yet, just return.
	 */
	if (!scanstate->tuplestorestate)
		return;

	/*
	 * Here we have a choice whether to drop the tuplestore (and recompute
	 * the function outputs) or just rescan it.  This should depend on
	 * whether the function expression contains parameters and/or is
	 * marked volatile.  FIXME soon.
	 */
	if (node->scan.plan.chgParam != NULL)
	{
		tuplestore_end((Tuplestorestate *) scanstate->tuplestorestate);
		scanstate->tuplestorestate = NULL;
	}
	else
		tuplestore_rescan((Tuplestorestate *) scanstate->tuplestorestate);
}

/*
 * Run the underlying function to get the next tuple
 */
static TupleTableSlot *
function_getonetuple(FunctionScanState *scanstate,
					 bool *isNull,
					 ExprDoneCond *isDone)
{
	HeapTuple		tuple;
	Datum			retDatum;
	char			nullflag;
	TupleDesc		tupdesc = scanstate->tupdesc;
	bool			returnsTuple = scanstate->returnsTuple;
	Node		   *expr = scanstate->funcexpr;
	Oid				fn_typeid = scanstate->fn_typeid;
	char			fn_typtype = scanstate->fn_typtype;
	ExprContext	   *econtext = scanstate->csstate.cstate.cs_ExprContext;
	TupleTableSlot *slot = scanstate->csstate.css_ScanTupleSlot;

	/*
	 * get the next Datum from the function
	 */
	retDatum = ExecEvalExprSwitchContext(expr, econtext, isNull, isDone);

	/*
	 * check to see if we're really done
	 */
	if (*isDone == ExprEndResult)
		slot = NULL;
	else
	{
		if (returnsTuple)
		{
			/*
			 * Composite data type, i.e. a table's row type
			 * function returns pointer to tts??
			 */
			slot = (TupleTableSlot *) retDatum;

			/*
			 * if function return type was RECORD, we need to check to be
			 * sure the structure from the query matches the actual return
			 * structure
			 */
			if (fn_typtype == 'p' && fn_typeid == RECORDOID)
				if (tupledesc_mismatch(tupdesc, slot->ttc_tupleDescriptor))
					elog(ERROR, "Query specified return tuple and actual"
									" function return tuple do not match");
		}
		else
		{
			/*
			 * Must be a base data type, i.e. scalar
			 * turn it into a tuple
			 */
			nullflag = *isNull ? 'n' : ' ';
			tuple = heap_formtuple(tupdesc, &retDatum, &nullflag);

			/*
			 * save the tuple in the scan tuple slot and return the slot.
			 */
			slot = ExecStoreTuple(tuple,			/* tuple to store */
								  slot,				/* slot to store in */
								  InvalidBuffer,	/* buffer associated with
													 * this tuple */
								  true);			/* pfree this pointer */
		}
	}

	return slot;
}

static FunctionMode
get_functionmode(Node *expr)
{
	/*
	 * for the moment, hardwire this
	 */
	return PM_REPEATEDCALL;
}

static bool
tupledesc_mismatch(TupleDesc tupdesc1, TupleDesc tupdesc2)
{
	int			i;

	if (tupdesc1->natts != tupdesc2->natts)
		return true;

	for (i = 0; i < tupdesc1->natts; i++)
	{
		Form_pg_attribute attr1 = tupdesc1->attrs[i];
		Form_pg_attribute attr2 = tupdesc2->attrs[i];

		/*
		 * We really only care about number of attributes and data type
		 */
		if (attr1->atttypid != attr2->atttypid)
			return true;
	}

	return false;
}
