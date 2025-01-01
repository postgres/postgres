/*-------------------------------------------------------------------------
 *
 * nodeResult.c
 *	  support for constant nodes needing special code.
 *
 * DESCRIPTION
 *
 *		Result nodes are used in queries where no relations are scanned.
 *		Examples of such queries are:
 *
 *				select 1 * 2
 *
 *				insert into emp values ('mike', 15000)
 *
 *		(Remember that in an INSERT or UPDATE, we need a plan tree that
 *		generates the new rows.)
 *
 *		Result nodes are also used to optimise queries with constant
 *		qualifications (ie, quals that do not depend on the scanned data),
 *		such as:
 *
 *				select * from emp where 2 > 1
 *
 *		In this case, the plan generated is
 *
 *						Result	(with 2 > 1 qual)
 *						/
 *				   SeqScan (emp.*)
 *
 *		At runtime, the Result node evaluates the constant qual once,
 *		which is shown by EXPLAIN as a One-Time Filter.  If it's
 *		false, we can return an empty result set without running the
 *		controlled plan at all.  If it's true, we run the controlled
 *		plan normally and pass back the results.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeResult.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeResult.h"
#include "miscadmin.h"


/* ----------------------------------------------------------------
 *		ExecResult(node)
 *
 *		returns the tuples from the outer plan which satisfy the
 *		qualification clause.  Since result nodes with right
 *		subtrees are never planned, we ignore the right subtree
 *		entirely (for now).. -cim 10/7/89
 *
 *		The qualification containing only constant clauses are
 *		checked first before any processing is done. It always returns
 *		'nil' if the constant qualification is not satisfied.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecResult(PlanState *pstate)
{
	ResultState *node = castNode(ResultState, pstate);
	TupleTableSlot *outerTupleSlot;
	PlanState  *outerPlan;
	ExprContext *econtext;

	CHECK_FOR_INTERRUPTS();

	econtext = node->ps.ps_ExprContext;

	/*
	 * check constant qualifications like (2 > 1), if not already done
	 */
	if (node->rs_checkqual)
	{
		bool		qualResult = ExecQual(node->resconstantqual, econtext);

		node->rs_checkqual = false;
		if (!qualResult)
		{
			node->rs_done = true;
			return NULL;
		}
	}

	/*
	 * Reset per-tuple memory context to free any expression evaluation
	 * storage allocated in the previous tuple cycle.
	 */
	ResetExprContext(econtext);

	/*
	 * if rs_done is true then it means that we were asked to return a
	 * constant tuple and we already did the last time ExecResult() was
	 * called, OR that we failed the constant qual check. Either way, now we
	 * are through.
	 */
	if (!node->rs_done)
	{
		outerPlan = outerPlanState(node);

		if (outerPlan != NULL)
		{
			/*
			 * retrieve tuples from the outer plan until there are no more.
			 */
			outerTupleSlot = ExecProcNode(outerPlan);

			if (TupIsNull(outerTupleSlot))
				return NULL;

			/*
			 * prepare to compute projection expressions, which will expect to
			 * access the input tuples as varno OUTER.
			 */
			econtext->ecxt_outertuple = outerTupleSlot;
		}
		else
		{
			/*
			 * if we don't have an outer plan, then we are just generating the
			 * results from a constant target list.  Do it only once.
			 */
			node->rs_done = true;
		}

		/* form the result tuple using ExecProject(), and return it */
		return ExecProject(node->ps.ps_ProjInfo);
	}

	return NULL;
}

/* ----------------------------------------------------------------
 *		ExecResultMarkPos
 * ----------------------------------------------------------------
 */
void
ExecResultMarkPos(ResultState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	if (outerPlan != NULL)
		ExecMarkPos(outerPlan);
	else
		elog(DEBUG2, "Result nodes do not support mark/restore");
}

/* ----------------------------------------------------------------
 *		ExecResultRestrPos
 * ----------------------------------------------------------------
 */
void
ExecResultRestrPos(ResultState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	if (outerPlan != NULL)
		ExecRestrPos(outerPlan);
	else
		elog(ERROR, "Result nodes do not support mark/restore");
}

/* ----------------------------------------------------------------
 *		ExecInitResult
 *
 *		Creates the run-time state information for the result node
 *		produced by the planner and initializes outer relations
 *		(child nodes).
 * ----------------------------------------------------------------
 */
ResultState *
ExecInitResult(Result *node, EState *estate, int eflags)
{
	ResultState *resstate;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_MARK | EXEC_FLAG_BACKWARD)) ||
		   outerPlan(node) != NULL);

	/*
	 * create state structure
	 */
	resstate = makeNode(ResultState);
	resstate->ps.plan = (Plan *) node;
	resstate->ps.state = estate;
	resstate->ps.ExecProcNode = ExecResult;

	resstate->rs_done = false;
	resstate->rs_checkqual = (node->resconstantqual != NULL);

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &resstate->ps);

	/*
	 * initialize child nodes
	 */
	outerPlanState(resstate) = ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * we don't use inner plan
	 */
	Assert(innerPlan(node) == NULL);

	/*
	 * Initialize result slot, type and projection.
	 */
	ExecInitResultTupleSlotTL(&resstate->ps, &TTSOpsVirtual);
	ExecAssignProjectionInfo(&resstate->ps, NULL);

	/*
	 * initialize child expressions
	 */
	resstate->ps.qual =
		ExecInitQual(node->plan.qual, (PlanState *) resstate);
	resstate->resconstantqual =
		ExecInitQual((List *) node->resconstantqual, (PlanState *) resstate);

	return resstate;
}

/* ----------------------------------------------------------------
 *		ExecEndResult
 *
 *		frees up storage allocated through C routines
 * ----------------------------------------------------------------
 */
void
ExecEndResult(ResultState *node)
{
	/*
	 * shut down subplans
	 */
	ExecEndNode(outerPlanState(node));
}

void
ExecReScanResult(ResultState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	node->rs_done = false;
	node->rs_checkqual = (node->resconstantqual != NULL);

	/*
	 * If chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (outerPlan && outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
}
