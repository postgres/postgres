/*-------------------------------------------------------------------------
 *
 * nodeCtescan.c
 *	  routines to handle CteScan nodes.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeCtescan.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/nodeCtescan.h"
#include "miscadmin.h"

static TupleTableSlot *CteScanNext(CteScanState *node);

/* ----------------------------------------------------------------
 *		CteScanNext
 *
 *		This is a workhorse for ExecCteScan
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
CteScanNext(CteScanState *node)
{
	EState	   *estate;
	ScanDirection dir;
	bool		forward;
	Tuplestorestate *tuplestorestate;
	bool		eof_tuplestore;
	TupleTableSlot *slot;

	/*
	 * get state info from node
	 */
	estate = node->ss.ps.state;
	dir = estate->es_direction;
	forward = ScanDirectionIsForward(dir);
	tuplestorestate = node->leader->cte_table;
	tuplestore_select_read_pointer(tuplestorestate, node->readptr);
	slot = node->ss.ss_ScanTupleSlot;

	/*
	 * If we are not at the end of the tuplestore, or are going backwards, try
	 * to fetch a tuple from tuplestore.
	 */
	eof_tuplestore = tuplestore_ateof(tuplestorestate);

	if (!forward && eof_tuplestore)
	{
		if (!node->leader->eof_cte)
		{
			/*
			 * When reversing direction at tuplestore EOF, the first
			 * gettupleslot call will fetch the last-added tuple; but we want
			 * to return the one before that, if possible. So do an extra
			 * fetch.
			 */
			if (!tuplestore_advance(tuplestorestate, forward))
				return NULL;	/* the tuplestore must be empty */
		}
		eof_tuplestore = false;
	}

	/*
	 * If we can fetch another tuple from the tuplestore, return it.
	 *
	 * Note: we have to use copy=true in the tuplestore_gettupleslot call,
	 * because we are sharing the tuplestore with other nodes that might write
	 * into the tuplestore before we get called again.
	 */
	if (!eof_tuplestore)
	{
		if (tuplestore_gettupleslot(tuplestorestate, forward, true, slot))
			return slot;
		if (forward)
			eof_tuplestore = true;
	}

	/*
	 * If necessary, try to fetch another row from the CTE query.
	 *
	 * Note: the eof_cte state variable exists to short-circuit further calls
	 * of the CTE plan.  It's not optional, unfortunately, because some plan
	 * node types are not robust about being called again when they've already
	 * returned NULL.
	 */
	if (eof_tuplestore && !node->leader->eof_cte)
	{
		TupleTableSlot *cteslot;

		/*
		 * We can only get here with forward==true, so no need to worry about
		 * which direction the subplan will go.
		 */
		cteslot = ExecProcNode(node->cteplanstate);
		if (TupIsNull(cteslot))
		{
			node->leader->eof_cte = true;
			return NULL;
		}

		/*
		 * There are corner cases where the subplan could change which
		 * tuplestore read pointer is active, so be sure to reselect ours
		 * before storing the tuple we got.
		 */
		tuplestore_select_read_pointer(tuplestorestate, node->readptr);

		/*
		 * Append a copy of the returned tuple to tuplestore.  NOTE: because
		 * our read pointer is certainly in EOF state, its read position will
		 * move forward over the added tuple.  This is what we want.  Also,
		 * any other readers will *not* move past the new tuple, which is what
		 * they want.
		 */
		tuplestore_puttupleslot(tuplestorestate, cteslot);

		/*
		 * We MUST copy the CTE query's output tuple into our own slot. This
		 * is because other CteScan nodes might advance the CTE query before
		 * we are called again, and our output tuple must stay stable over
		 * that.
		 */
		return ExecCopySlot(slot, cteslot);
	}

	/*
	 * Nothing left ...
	 */
	return ExecClearTuple(slot);
}

/*
 * CteScanRecheck -- access method routine to recheck a tuple in EvalPlanQual
 */
static bool
CteScanRecheck(CteScanState *node, TupleTableSlot *slot)
{
	/* nothing to check */
	return true;
}

/* ----------------------------------------------------------------
 *		ExecCteScan(node)
 *
 *		Scans the CTE sequentially and returns the next qualifying tuple.
 *		We call the ExecScan() routine and pass it the appropriate
 *		access method functions.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecCteScan(PlanState *pstate)
{
	CteScanState *node = castNode(CteScanState, pstate);

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) CteScanNext,
					(ExecScanRecheckMtd) CteScanRecheck);
}


/* ----------------------------------------------------------------
 *		ExecInitCteScan
 * ----------------------------------------------------------------
 */
CteScanState *
ExecInitCteScan(CteScan *node, EState *estate, int eflags)
{
	CteScanState *scanstate;
	ParamExecData *prmdata;

	/* check for unsupported flags */
	Assert(!(eflags & EXEC_FLAG_MARK));

	/*
	 * For the moment we have to force the tuplestore to allow REWIND, because
	 * we might be asked to rescan the CTE even though upper levels didn't
	 * tell us to be prepared to do it efficiently.  Annoying, since this
	 * prevents truncation of the tuplestore.  XXX FIXME
	 *
	 * Note: if we are in an EPQ recheck plan tree, it's likely that no access
	 * to the tuplestore is needed at all, making this even more annoying.
	 * It's not worth improving that as long as all the read pointers would
	 * have REWIND anyway, but if we ever improve this logic then that aspect
	 * should be considered too.
	 */
	eflags |= EXEC_FLAG_REWIND;

	/*
	 * CteScan should not have any children.
	 */
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * create new CteScanState for node
	 */
	scanstate = makeNode(CteScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;
	scanstate->ss.ps.ExecProcNode = ExecCteScan;
	scanstate->eflags = eflags;
	scanstate->cte_table = NULL;
	scanstate->eof_cte = false;

	/*
	 * Find the already-initialized plan for the CTE query.
	 */
	scanstate->cteplanstate = (PlanState *) list_nth(estate->es_subplanstates,
													 node->ctePlanId - 1);

	/*
	 * The Param slot associated with the CTE query is used to hold a pointer
	 * to the CteState of the first CteScan node that initializes for this
	 * CTE.  This node will be the one that holds the shared state for all the
	 * CTEs, particularly the shared tuplestore.
	 */
	prmdata = &(estate->es_param_exec_vals[node->cteParam]);
	Assert(prmdata->execPlan == NULL);
	Assert(!prmdata->isnull);
	scanstate->leader = castNode(CteScanState, DatumGetPointer(prmdata->value));
	if (scanstate->leader == NULL)
	{
		/* I am the leader */
		prmdata->value = PointerGetDatum(scanstate);
		scanstate->leader = scanstate;
		scanstate->cte_table = tuplestore_begin_heap(true, false, work_mem);
		tuplestore_set_eflags(scanstate->cte_table, scanstate->eflags);
		scanstate->readptr = 0;
	}
	else
	{
		/* Not the leader */
		/* Create my own read pointer, and ensure it is at start */
		scanstate->readptr =
			tuplestore_alloc_read_pointer(scanstate->leader->cte_table,
										  scanstate->eflags);
		tuplestore_select_read_pointer(scanstate->leader->cte_table,
									   scanstate->readptr);
		tuplestore_rescan(scanstate->leader->cte_table);
	}

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	/*
	 * The scan tuple type (ie, the rowtype we expect to find in the work
	 * table) is the same as the result rowtype of the CTE query.
	 */
	ExecInitScanTupleSlot(estate, &scanstate->ss,
						  ExecGetResultType(scanstate->cteplanstate),
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
		ExecInitQual(node->scan.plan.qual, (PlanState *) scanstate);

	return scanstate;
}

/* ----------------------------------------------------------------
 *		ExecEndCteScan
 *
 *		frees any storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndCteScan(CteScanState *node)
{
	/*
	 * Free exprcontext
	 */
	ExecFreeExprContext(&node->ss.ps);

	/*
	 * clean out the tuple table
	 */
	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/*
	 * If I am the leader, free the tuplestore.
	 */
	if (node->leader == node)
	{
		tuplestore_end(node->cte_table);
		node->cte_table = NULL;
	}
}

/* ----------------------------------------------------------------
 *		ExecReScanCteScan
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecReScanCteScan(CteScanState *node)
{
	Tuplestorestate *tuplestorestate = node->leader->cte_table;

	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	ExecScanReScan(&node->ss);

	/*
	 * Clear the tuplestore if a new scan of the underlying CTE is required.
	 * This implicitly resets all the tuplestore's read pointers.  Note that
	 * multiple CTE nodes might redundantly clear the tuplestore; that's OK,
	 * and not unduly expensive.  We'll stop taking this path as soon as
	 * somebody has attempted to read something from the underlying CTE
	 * (thereby causing its chgParam to be cleared).
	 */
	if (node->leader->cteplanstate->chgParam != NULL)
	{
		tuplestore_clear(tuplestorestate);
		node->leader->eof_cte = false;
	}
	else
	{
		/*
		 * Else, just rewind my own pointer.  Either the underlying CTE
		 * doesn't need a rescan (and we can re-read what's in the tuplestore
		 * now), or somebody else already took care of it.
		 */
		tuplestore_select_read_pointer(tuplestorestate, node->readptr);
		tuplestore_rescan(tuplestorestate);
	}
}
