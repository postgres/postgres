/*-------------------------------------------------------------------------
 *
 * nodeTidrangescan.c
 *	  Routines to support TID range scans of relations
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeTidrangescan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relscan.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "catalog/pg_operator.h"
#include "executor/executor.h"
#include "executor/nodeTidrangescan.h"
#include "nodes/nodeFuncs.h"
#include "utils/rel.h"


/*
 * It's sufficient to check varattno to identify the CTID variable, as any
 * Var in the relation scan qual must be for our table.  (Even if it's a
 * parameterized scan referencing some other table's CTID, the other table's
 * Var would have become a Param by the time it gets here.)
 */
#define IsCTIDVar(node)  \
	((node) != NULL && \
	 IsA((node), Var) && \
	 ((Var *) (node))->varattno == SelfItemPointerAttributeNumber)

typedef enum
{
	TIDEXPR_UPPER_BOUND,
	TIDEXPR_LOWER_BOUND,
} TidExprType;

/* Upper or lower range bound for scan */
typedef struct TidOpExpr
{
	TidExprType exprtype;		/* type of op; lower or upper */
	ExprState  *exprstate;		/* ExprState for a TID-yielding subexpr */
	bool		inclusive;		/* whether op is inclusive */
} TidOpExpr;

/*
 * For the given 'expr', build and return an appropriate TidOpExpr taking into
 * account the expr's operator and operand order.
 */
static TidOpExpr *
MakeTidOpExpr(OpExpr *expr, TidRangeScanState *tidstate)
{
	Node	   *arg1 = get_leftop((Expr *) expr);
	Node	   *arg2 = get_rightop((Expr *) expr);
	ExprState  *exprstate = NULL;
	bool		invert = false;
	TidOpExpr  *tidopexpr;

	if (IsCTIDVar(arg1))
		exprstate = ExecInitExpr((Expr *) arg2, &tidstate->ss.ps);
	else if (IsCTIDVar(arg2))
	{
		exprstate = ExecInitExpr((Expr *) arg1, &tidstate->ss.ps);
		invert = true;
	}
	else
		elog(ERROR, "could not identify CTID variable");

	tidopexpr = (TidOpExpr *) palloc(sizeof(TidOpExpr));
	tidopexpr->inclusive = false;	/* for now */

	switch (expr->opno)
	{
		case TIDLessEqOperator:
			tidopexpr->inclusive = true;
			/* fall through */
		case TIDLessOperator:
			tidopexpr->exprtype = invert ? TIDEXPR_LOWER_BOUND : TIDEXPR_UPPER_BOUND;
			break;
		case TIDGreaterEqOperator:
			tidopexpr->inclusive = true;
			/* fall through */
		case TIDGreaterOperator:
			tidopexpr->exprtype = invert ? TIDEXPR_UPPER_BOUND : TIDEXPR_LOWER_BOUND;
			break;
		default:
			elog(ERROR, "could not identify CTID operator");
	}

	tidopexpr->exprstate = exprstate;

	return tidopexpr;
}

/*
 * Extract the qual subexpressions that yield TIDs to search for,
 * and compile them into ExprStates if they're ordinary expressions.
 */
static void
TidExprListCreate(TidRangeScanState *tidrangestate)
{
	TidRangeScan *node = (TidRangeScan *) tidrangestate->ss.ps.plan;
	List	   *tidexprs = NIL;
	ListCell   *l;

	foreach(l, node->tidrangequals)
	{
		OpExpr	   *opexpr = lfirst(l);
		TidOpExpr  *tidopexpr;

		if (!IsA(opexpr, OpExpr))
			elog(ERROR, "could not identify CTID expression");

		tidopexpr = MakeTidOpExpr(opexpr, tidrangestate);
		tidexprs = lappend(tidexprs, tidopexpr);
	}

	tidrangestate->trss_tidexprs = tidexprs;
}

/* ----------------------------------------------------------------
 *		TidRangeEval
 *
 *		Compute and set node's block and offset range to scan by evaluating
 *		the trss_tidexprs.  Returns false if we detect the range cannot
 *		contain any tuples.  Returns true if it's possible for the range to
 *		contain tuples.
 * ----------------------------------------------------------------
 */
static bool
TidRangeEval(TidRangeScanState *node)
{
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	ItemPointerData lowerBound;
	ItemPointerData upperBound;
	ListCell   *l;

	/*
	 * Set the upper and lower bounds to the absolute limits of the range of
	 * the ItemPointer type.  Below we'll try to narrow this range on either
	 * side by looking at the TidOpExprs.
	 */
	ItemPointerSet(&lowerBound, 0, 0);
	ItemPointerSet(&upperBound, InvalidBlockNumber, PG_UINT16_MAX);

	foreach(l, node->trss_tidexprs)
	{
		TidOpExpr  *tidopexpr = (TidOpExpr *) lfirst(l);
		ItemPointer itemptr;
		bool		isNull;

		/* Evaluate this bound. */
		itemptr = (ItemPointer)
			DatumGetPointer(ExecEvalExprSwitchContext(tidopexpr->exprstate,
													  econtext,
													  &isNull));

		/* If the bound is NULL, *nothing* matches the qual. */
		if (isNull)
			return false;

		if (tidopexpr->exprtype == TIDEXPR_LOWER_BOUND)
		{
			ItemPointerData lb;

			ItemPointerCopy(itemptr, &lb);

			/*
			 * Normalize non-inclusive ranges to become inclusive.  The
			 * resulting ItemPointer here may not be a valid item pointer.
			 */
			if (!tidopexpr->inclusive)
				ItemPointerInc(&lb);

			/* Check if we can narrow the range using this qual */
			if (ItemPointerCompare(&lb, &lowerBound) > 0)
				ItemPointerCopy(&lb, &lowerBound);
		}

		else if (tidopexpr->exprtype == TIDEXPR_UPPER_BOUND)
		{
			ItemPointerData ub;

			ItemPointerCopy(itemptr, &ub);

			/*
			 * Normalize non-inclusive ranges to become inclusive.  The
			 * resulting ItemPointer here may not be a valid item pointer.
			 */
			if (!tidopexpr->inclusive)
				ItemPointerDec(&ub);

			/* Check if we can narrow the range using this qual */
			if (ItemPointerCompare(&ub, &upperBound) < 0)
				ItemPointerCopy(&ub, &upperBound);
		}
	}

	ItemPointerCopy(&lowerBound, &node->trss_mintid);
	ItemPointerCopy(&upperBound, &node->trss_maxtid);

	return true;
}

/* ----------------------------------------------------------------
 *		TidRangeNext
 *
 *		Retrieve a tuple from the TidRangeScan node's currentRelation
 *		using the TIDs in the TidRangeScanState information.
 *
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
TidRangeNext(TidRangeScanState *node)
{
	TableScanDesc scandesc;
	EState	   *estate;
	ScanDirection direction;
	TupleTableSlot *slot;

	/*
	 * extract necessary information from TID scan node
	 */
	scandesc = node->ss.ss_currentScanDesc;
	estate = node->ss.ps.state;
	slot = node->ss.ss_ScanTupleSlot;
	direction = estate->es_direction;

	if (!node->trss_inScan)
	{
		/* First time through, compute TID range to scan */
		if (!TidRangeEval(node))
			return NULL;

		if (scandesc == NULL)
		{
			scandesc = table_beginscan_tidrange(node->ss.ss_currentRelation,
												estate->es_snapshot,
												&node->trss_mintid,
												&node->trss_maxtid);
			node->ss.ss_currentScanDesc = scandesc;
		}
		else
		{
			/* rescan with the updated TID range */
			table_rescan_tidrange(scandesc, &node->trss_mintid,
								  &node->trss_maxtid);
		}

		node->trss_inScan = true;
	}

	/* Fetch the next tuple. */
	if (!table_scan_getnextslot_tidrange(scandesc, direction, slot))
	{
		node->trss_inScan = false;
		ExecClearTuple(slot);
	}

	return slot;
}

/*
 * TidRangeRecheck -- access method routine to recheck a tuple in EvalPlanQual
 */
static bool
TidRangeRecheck(TidRangeScanState *node, TupleTableSlot *slot)
{
	return true;
}

/* ----------------------------------------------------------------
 *		ExecTidRangeScan(node)
 *
 *		Scans the relation using tids and returns the next qualifying tuple.
 *		We call the ExecScan() routine and pass it the appropriate
 *		access method functions.
 *
 *		Conditions:
 *		  -- the "cursor" maintained by the AMI is positioned at the tuple
 *			 returned previously.
 *
 *		Initial States:
 *		  -- the relation indicated is opened for TID range scanning.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecTidRangeScan(PlanState *pstate)
{
	TidRangeScanState *node = castNode(TidRangeScanState, pstate);

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) TidRangeNext,
					(ExecScanRecheckMtd) TidRangeRecheck);
}

/* ----------------------------------------------------------------
 *		ExecReScanTidRangeScan(node)
 * ----------------------------------------------------------------
 */
void
ExecReScanTidRangeScan(TidRangeScanState *node)
{
	/* mark scan as not in progress, and tid range list as not computed yet */
	node->trss_inScan = false;

	/*
	 * We must wait until TidRangeNext before calling table_rescan_tidrange.
	 */
	ExecScanReScan(&node->ss);
}

/* ----------------------------------------------------------------
 *		ExecEndTidRangeScan
 *
 *		Releases any storage allocated through C routines.
 *		Returns nothing.
 * ----------------------------------------------------------------
 */
void
ExecEndTidRangeScan(TidRangeScanState *node)
{
	TableScanDesc scan = node->ss.ss_currentScanDesc;

	if (scan != NULL)
		table_endscan(scan);
}

/* ----------------------------------------------------------------
 *		ExecInitTidRangeScan
 *
 *		Initializes the tid range scan's state information, creates
 *		scan keys, and opens the scan relation.
 *
 *		Parameters:
 *		  node: TidRangeScan node produced by the planner.
 *		  estate: the execution state initialized in InitPlan.
 * ----------------------------------------------------------------
 */
TidRangeScanState *
ExecInitTidRangeScan(TidRangeScan *node, EState *estate, int eflags)
{
	TidRangeScanState *tidrangestate;
	Relation	currentRelation;

	/*
	 * create state structure
	 */
	tidrangestate = makeNode(TidRangeScanState);
	tidrangestate->ss.ps.plan = (Plan *) node;
	tidrangestate->ss.ps.state = estate;
	tidrangestate->ss.ps.ExecProcNode = ExecTidRangeScan;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &tidrangestate->ss.ps);

	/*
	 * mark scan as not in progress, and TID range as not computed yet
	 */
	tidrangestate->trss_inScan = false;

	/*
	 * open the scan relation
	 */
	currentRelation = ExecOpenScanRelation(estate, node->scan.scanrelid, eflags);

	tidrangestate->ss.ss_currentRelation = currentRelation;
	tidrangestate->ss.ss_currentScanDesc = NULL;	/* no table scan here */

	/*
	 * get the scan type from the relation descriptor.
	 */
	ExecInitScanTupleSlot(estate, &tidrangestate->ss,
						  RelationGetDescr(currentRelation),
						  table_slot_callbacks(currentRelation));

	/*
	 * Initialize result type and projection.
	 */
	ExecInitResultTypeTL(&tidrangestate->ss.ps);
	ExecAssignScanProjectionInfo(&tidrangestate->ss);

	/*
	 * initialize child expressions
	 */
	tidrangestate->ss.ps.qual =
		ExecInitQual(node->scan.plan.qual, (PlanState *) tidrangestate);

	TidExprListCreate(tidrangestate);

	/*
	 * all done.
	 */
	return tidrangestate;
}
