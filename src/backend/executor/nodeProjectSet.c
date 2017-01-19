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
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
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
#include "utils/memutils.h"


static TupleTableSlot *ExecProjectSRF(ProjectSetState *node, bool continuing);


/* ----------------------------------------------------------------
 *		ExecProjectSet(node)
 *
 *		Return tuples after evaluating the targetlist (which contains set
 *		returning functions).
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecProjectSet(ProjectSetState *node)
{
	TupleTableSlot *outerTupleSlot;
	TupleTableSlot *resultSlot;
	PlanState  *outerPlan;
	ExprContext *econtext;

	econtext = node->ps.ps_ExprContext;

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
	 * Reset per-tuple memory context to free any expression evaluation
	 * storage allocated in the previous tuple cycle.  Note this can't happen
	 * until we're done projecting out tuples from a scan tuple.
	 */
	ResetExprContext(econtext);

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
	bool		hassrf PG_USED_FOR_ASSERTS_ONLY = false;
	bool		hasresult;
	int			argno;
	ListCell   *lc;

	ExecClearTuple(resultSlot);

	/*
	 * Assume no further tuples are produced unless an ExprMultipleResult is
	 * encountered from a set returning function.
	 */
	node->pending_srf_tuples = false;

	hasresult = false;
	argno = 0;
	foreach(lc, node->ps.targetlist)
	{
		GenericExprState *gstate = (GenericExprState *) lfirst(lc);
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
		else if (IsA(gstate->arg, FuncExprState) &&
				 ((FuncExprState *) gstate->arg)->funcReturnsSet)
		{
			/*
			 * Evaluate SRF - possibly continuing previously started output.
			 */
			*result = ExecMakeFunctionResultSet((FuncExprState *) gstate->arg,
												econtext, isnull, isdone);

			if (*isdone != ExprEndResult)
				hasresult = true;
			if (*isdone == ExprMultipleResult)
				node->pending_srf_tuples = true;
			hassrf = true;
		}
		else
		{
			/* Non-SRF tlist expression, just evaluate normally. */
			*result = ExecEvalExpr(gstate->arg, econtext, isnull);
			*isdone = ExprSingleResult;
		}

		argno++;
	}

	/* ProjectSet should not be used if there's no SRFs */
	Assert(hassrf);

	/*
	 * If all the SRFs returned EndResult, we consider that as no row being
	 * produced.
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

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_MARK | EXEC_FLAG_BACKWARD)));

	/*
	 * create state structure
	 */
	state = makeNode(ProjectSetState);
	state->ps.plan = (Plan *) node;
	state->ps.state = estate;

	state->pending_srf_tuples = false;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &state->ps);

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &state->ps);

	/*
	 * initialize child expressions
	 */
	state->ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->plan.targetlist,
					 (PlanState *) state);
	Assert(node->plan.qual == NIL);

	/*
	 * initialize child nodes
	 */
	outerPlanState(state) = ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * we don't use inner plan
	 */
	Assert(innerPlan(node) == NULL);

	/*
	 * initialize tuple type and projection info
	 */
	ExecAssignResultTypeFromTL(&state->ps);

	/* Create workspace for per-SRF is-done state */
	state->nelems = list_length(node->plan.targetlist);
	state->elemdone = (ExprDoneCond *)
		palloc(sizeof(ExprDoneCond) * state->nelems);

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
