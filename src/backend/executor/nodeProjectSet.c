/*-------------------------------------------------------------------------
 *
 * nodeProjectSet.c
 *	  support for evaluating targetlists containing set-returning functions
 *
 * DESCRIPTION
 *
 *		ProjectSet nodes are inserted by the planner to evaluate set-returning
 *		functions in the targetlist.  It's guaranteed that all set-returning
 *		functions are directly at the top level of the targetlist, i.e. they
 *		can't be inside more-complex expressions.  If that'd otherwise be
 *		the case, the planner adds additional ProjectSet nodes.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeProjectSet.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeProjectSet.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "utils/memutils.h"


static TupleTableSlot *ExecProjectSRF(ProjectSetState *node, bool continuing);


/* ----------------------------------------------------------------
 *		ExecProjectSet(node)
 *
 *		Return tuples after evaluating the targetlist (which contains set
 *		returning functions).
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecProjectSet(PlanState *pstate)
{
	ProjectSetState *node = castNode(ProjectSetState, pstate);
	TupleTableSlot *outerTupleSlot;
	TupleTableSlot *resultSlot;
	PlanState  *outerPlan;
	ExprContext *econtext;

	CHECK_FOR_INTERRUPTS();

	econtext = node->ps.ps_ExprContext;

	/*
	 * Reset per-tuple context to free expression-evaluation storage allocated
	 * for a potentially previously returned tuple. Note that the SRF argument
	 * context has a different lifetime and is reset below.
	 */
	ResetExprContext(econtext);

	/*
	 * Check to see if we're still projecting out tuples from a previous scan
	 * tuple (because there is a function-returning-set in the projection
	 * expressions).  If so, try to project another one.
	 */
	if (node->pending_srf_tuples)
	{
		resultSlot = ExecProjectSRF(node, true);

		if (resultSlot != NULL)
			return resultSlot;
	}

	/*
	 * Reset argument context to free any expression evaluation storage
	 * allocated in the previous tuple cycle.  Note this can't happen until
	 * we're done projecting out tuples from a scan tuple, as ValuePerCall
	 * functions are allowed to reference the arguments for each returned
	 * tuple.
	 */
	MemoryContextReset(node->argcontext);

	/*
	 * Get another input tuple and project SRFs from it.
	 */
	for (;;)
	{
		/*
		 * Retrieve tuples from the outer plan until there are no more.
		 */
		outerPlan = outerPlanState(node);
		outerTupleSlot = ExecProcNode(outerPlan);

		if (TupIsNull(outerTupleSlot))
			return NULL;

		/*
		 * Prepare to compute projection expressions, which will expect to
		 * access the input tuples as varno OUTER.
		 */
		econtext->ecxt_outertuple = outerTupleSlot;

		/* Evaluate the expressions */
		resultSlot = ExecProjectSRF(node, false);

		/*
		 * Return the tuple unless the projection produced no rows (due to an
		 * empty set), in which case we must loop back to see if there are
		 * more outerPlan tuples.
		 */
		if (resultSlot)
			return resultSlot;
	}

	return NULL;
}

/* ----------------------------------------------------------------
 *		ExecProjectSRF
 *
 *		Project a targetlist containing one or more set-returning functions.
 *
 *		'continuing' indicates whether to continue projecting rows for the
 *		same input tuple; or whether a new input tuple is being projected.
 *
 *		Returns NULL if no output tuple has been produced.
 *
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecProjectSRF(ProjectSetState *node, bool continuing)
{
	TupleTableSlot *resultSlot = node->ps.ps_ResultTupleSlot;
	ExprContext *econtext = node->ps.ps_ExprContext;
	MemoryContext oldcontext;
	bool		hassrf PG_USED_FOR_ASSERTS_ONLY;
	bool		hasresult;
	int			argno;

	ExecClearTuple(resultSlot);

	/* Call SRFs, as well as plain expressions, in per-tuple context */
	oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/*
	 * Assume no further tuples are produced unless an ExprMultipleResult is
	 * encountered from a set returning function.
	 */
	node->pending_srf_tuples = false;

	hassrf = hasresult = false;
	for (argno = 0; argno < node->nelems; argno++)
	{
		Node	   *elem = node->elems[argno];
		ExprDoneCond *isdone = &node->elemdone[argno];
		Datum	   *result = &resultSlot->tts_values[argno];
		bool	   *isnull = &resultSlot->tts_isnull[argno];

		if (continuing && *isdone == ExprEndResult)
		{
			/*
			 * If we're continuing to project output rows from a source tuple,
			 * return NULLs once the SRF has been exhausted.
			 */
			*result = (Datum) 0;
			*isnull = true;
			hassrf = true;
		}
		else if (IsA(elem, SetExprState))
		{
			/*
			 * Evaluate SRF - possibly continuing previously started output.
			 */
			*result = ExecMakeFunctionResultSet((SetExprState *) elem,
												econtext, node->argcontext,
												isnull, isdone);

			if (*isdone != ExprEndResult)
				hasresult = true;
			if (*isdone == ExprMultipleResult)
				node->pending_srf_tuples = true;
			hassrf = true;
		}
		else
		{
			/* Non-SRF tlist expression, just evaluate normally. */
			*result = ExecEvalExpr((ExprState *) elem, econtext, isnull);
			*isdone = ExprSingleResult;
		}
	}

	MemoryContextSwitchTo(oldcontext);

	/* ProjectSet should not be used if there's no SRFs */
	Assert(hassrf);

	/*
	 * If all the SRFs returned ExprEndResult, we consider that as no row
	 * being produced.
	 */
	if (hasresult)
	{
		ExecStoreVirtualTuple(resultSlot);
		return resultSlot;
	}

	return NULL;
}

/* ----------------------------------------------------------------
 *		ExecInitProjectSet
 *
 *		Creates the run-time state information for the ProjectSet node
 *		produced by the planner and initializes outer relations
 *		(child nodes).
 * ----------------------------------------------------------------
 */
ProjectSetState *
ExecInitProjectSet(ProjectSet *node, EState *estate, int eflags)
{
	ProjectSetState *state;
	ListCell   *lc;
	int			off;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_MARK | EXEC_FLAG_BACKWARD)));

	/*
	 * create state structure
	 */
	state = makeNode(ProjectSetState);
	state->ps.plan = (Plan *) node;
	state->ps.state = estate;
	state->ps.ExecProcNode = ExecProjectSet;

	state->pending_srf_tuples = false;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &state->ps);

	/*
	 * initialize child nodes
	 */
	outerPlanState(state) = ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * we don't use inner plan
	 */
	Assert(innerPlan(node) == NULL);

	/*
	 * tuple table and result type initialization
	 */
	ExecInitResultTupleSlotTL(&state->ps, &TTSOpsVirtual);

	/* Create workspace for per-tlist-entry expr state & SRF-is-done state */
	state->nelems = list_length(node->plan.targetlist);
	state->elems = (Node **)
		palloc(sizeof(Node *) * state->nelems);
	state->elemdone = (ExprDoneCond *)
		palloc(sizeof(ExprDoneCond) * state->nelems);

	/*
	 * Build expressions to evaluate targetlist.  We can't use
	 * ExecBuildProjectionInfo here, since that doesn't deal with SRFs.
	 * Instead compile each expression separately, using
	 * ExecInitFunctionResultSet where applicable.
	 */
	off = 0;
	foreach(lc, node->plan.targetlist)
	{
		TargetEntry *te = (TargetEntry *) lfirst(lc);
		Expr	   *expr = te->expr;

		if ((IsA(expr, FuncExpr) && ((FuncExpr *) expr)->funcretset) ||
			(IsA(expr, OpExpr) && ((OpExpr *) expr)->opretset))
		{
			state->elems[off] = (Node *)
				ExecInitFunctionResultSet(expr, state->ps.ps_ExprContext,
										  &state->ps);
		}
		else
		{
			Assert(!expression_returns_set((Node *) expr));
			state->elems[off] = (Node *) ExecInitExpr(expr, &state->ps);
		}

		off++;
	}

	/* We don't support any qual on ProjectSet nodes */
	Assert(node->plan.qual == NIL);

	/*
	 * Create a memory context that ExecMakeFunctionResultSet can use to
	 * evaluate function arguments in.  We can't use the per-tuple context for
	 * this because it gets reset too often; but we don't want to leak
	 * evaluation results into the query-lifespan context either.  We use one
	 * context for the arguments of all tSRFs, as they have roughly equivalent
	 * lifetimes.
	 */
	state->argcontext = AllocSetContextCreate(CurrentMemoryContext,
											  "tSRF function arguments",
											  ALLOCSET_DEFAULT_SIZES);

	return state;
}

/* ----------------------------------------------------------------
 *		ExecEndProjectSet
 *
 *		frees up storage allocated through C routines
 * ----------------------------------------------------------------
 */
void
ExecEndProjectSet(ProjectSetState *node)
{
	/*
	 * Free the exprcontext
	 */
	ExecFreeExprContext(&node->ps);

	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(node->ps.ps_ResultTupleSlot);

	/*
	 * shut down subplans
	 */
	ExecEndNode(outerPlanState(node));
}

void
ExecReScanProjectSet(ProjectSetState *node)
{
	/* Forget any incompletely-evaluated SRFs */
	node->pending_srf_tuples = false;

	/*
	 * If chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (node->ps.lefttree->chgParam == NULL)
		ExecReScan(node->ps.lefttree);
}
