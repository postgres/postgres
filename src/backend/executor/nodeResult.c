/*-------------------------------------------------------------------------
 *
 * nodeResult.c--
 *	  support for constant nodes needing special code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * DESCRIPTION
 *
 *		Example: in constant queries where no relations are scanned,
 *		the planner generates result nodes.  Examples of such queries are:
 *
 *				retrieve (x = 1)
 *		and
 *				append emp (name = "mike", salary = 15000)
 *
 *		Result nodes are also used to optimise queries
 *		with tautological qualifications like:
 *
 *				retrieve (emp.all) where 2 > 1
 *
 *		In this case, the plan generated is
 *
 *						Result	(with 2 > 1 qual)
 *						/
 *				   SeqScan (emp.all)
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeResult.c,v 1.5 1997/09/08 21:43:16 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"


#include "executor/executor.h"
#include "executor/nodeResult.h"

/* ----------------------------------------------------------------
 *		ExecResult(node)
 *
 *		returns the tuples from the outer plan which satisify the
 *		qualification clause.  Since result nodes with right
 *		subtrees are never planned, we ignore the right subtree
 *		entirely (for now).. -cim 10/7/89
 *
 *		The qualification containing only constant clauses are
 *		checked first before any processing is done. It always returns
 *		'nil' if the constant qualification is not satisfied.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecResult(Result *node)
{
	ResultState *resstate;
	TupleTableSlot *outerTupleSlot;
	TupleTableSlot *resultSlot;
	Plan	   *outerPlan;
	ExprContext *econtext;
	Node	   *qual;
	bool		qualResult;
	bool		isDone;
	ProjectionInfo *projInfo;

	/* ----------------
	 *	initialize the result node's state
	 * ----------------
	 */
	resstate = node->resstate;

	/* ----------------
	 *	get the expression context
	 * ----------------
	 */
	econtext = resstate->cstate.cs_ExprContext;

	/* ----------------
	 *	 check tautological qualifications like (2 > 1)
	 * ----------------
	 */
	qual = node->resconstantqual;
	if (qual != NULL)
	{
		qualResult = ExecQual((List *) qual, econtext);
		/* ----------------
		 *	if we failed the constant qual, then there
		 *	is no need to continue processing because regardless of
		 *	what happens, the constant qual will be false..
		 * ----------------
		 */
		if (qualResult == false)
			return NULL;

		/* ----------------
		 *	our constant qualification succeeded so now we
		 *	throw away the qual because we know it will always
		 *	succeed.
		 * ----------------
		 */
		node->resconstantqual = NULL;
	}

	if (resstate->cstate.cs_TupFromTlist)
	{
		ProjectionInfo *projInfo;

		projInfo = resstate->cstate.cs_ProjInfo;
		resultSlot = ExecProject(projInfo, &isDone);
		if (!isDone)
			return resultSlot;
	}

	/* ----------------
	 *	retrieve a tuple that satisfy the qual from the outer plan until
	 *	there are no more.
	 *
	 *	if rs_done is 1 then it means that we were asked to return
	 *	a constant tuple and we alread did the last time ExecResult()
	 *	was called, so now we are through.
	 * ----------------
	 */
	outerPlan = outerPlan(node);

	while (!resstate->rs_done)
	{

		/* ----------------
		 *	  get next outer tuple if necessary.
		 * ----------------
		 */
		if (outerPlan != NULL)
		{
			outerTupleSlot = ExecProcNode(outerPlan, (Plan *) node);

			if (TupIsNull(outerTupleSlot))
				return NULL;

			resstate->cstate.cs_OuterTupleSlot = outerTupleSlot;
		}
		else
		{

			/* ----------------
			 *	if we don't have an outer plan, then it's probably
			 *	the case that we are doing a retrieve or an append
			 *	with a constant target list, so we should only return
			 *	the constant tuple once or never if we fail the qual.
			 * ----------------
			 */
			resstate->rs_done = 1;
		}

		/* ----------------
		 *	  get the information to place into the expr context
		 * ----------------
		 */
		resstate = node->resstate;

		outerTupleSlot = resstate->cstate.cs_OuterTupleSlot;

		/* ----------------
		 *	 fill in the information in the expression context
		 *	 XXX gross hack. use outer tuple as scan tuple
		 * ----------------
		 */
		econtext->ecxt_outertuple = outerTupleSlot;
		econtext->ecxt_scantuple = outerTupleSlot;

		/* ----------------
		 *	 form the result tuple and pass it back using ExecProject()
		 * ----------------
		 */
		projInfo = resstate->cstate.cs_ProjInfo;
		resultSlot = ExecProject(projInfo, &isDone);
		resstate->cstate.cs_TupFromTlist = !isDone;
		return resultSlot;
	}

	return NULL;
}

/* ----------------------------------------------------------------
 *		ExecInitResult
 *
 *		Creates the run-time state information for the result node
 *		produced by the planner and initailizes outer relations
 *		(child nodes).
 * ----------------------------------------------------------------
 */
bool
ExecInitResult(Result *node, EState *estate, Plan *parent)
{
	ResultState *resstate;

	/* ----------------
	 *	assign execution state to node
	 * ----------------
	 */
	node->plan.state = estate;

	/* ----------------
	 *	create new ResultState for node
	 * ----------------
	 */
	resstate = makeNode(ResultState);
	resstate->rs_done = 0;
	node->resstate = resstate;

	/* ----------------
	 *	Miscellanious initialization
	 *
	 *		 +	assign node's base_id
	 *		 +	assign debugging hooks and
	 *		 +	create expression context for node
	 * ----------------
	 */
	ExecAssignNodeBaseInfo(estate, &resstate->cstate, parent);
	ExecAssignExprContext(estate, &resstate->cstate);

#define RESULT_NSLOTS 1
	/* ----------------
	 *	tuple table initialization
	 * ----------------
	 */
	ExecInitResultTupleSlot(estate, &resstate->cstate);

	/* ----------------
	 *	then initialize children
	 * ----------------
	 */
	ExecInitNode(outerPlan(node), estate, (Plan *) node);

	/*
	 * we don't use inner plan
	 */
	Assert(innerPlan(node) == NULL);

	/* ----------------
	 *	initialize tuple type and projection info
	 * ----------------
	 */
	ExecAssignResultTypeFromTL((Plan *) node, &resstate->cstate);
	ExecAssignProjectionInfo((Plan *) node, &resstate->cstate);

	/* ----------------
	 *	set "are we done yet" to false
	 * ----------------
	 */
	resstate->rs_done = 0;

	return TRUE;
}

int
ExecCountSlotsResult(Result *node)
{
	return ExecCountSlotsNode(outerPlan(node)) + RESULT_NSLOTS;
}

/* ----------------------------------------------------------------
 *		ExecEndResult
 *
 *		fees up storage allocated through C routines
 * ----------------------------------------------------------------
 */
void
ExecEndResult(Result *node)
{
	ResultState *resstate;

	resstate = node->resstate;

	/* ----------------
	 *	Free the projection info
	 *
	 *	Note: we don't ExecFreeResultType(resstate)
	 *		  because the rule manager depends on the tupType
	 *		  returned by ExecMain().  So for now, this
	 *		  is freed at end-transaction time.  -cim 6/2/91
	 * ----------------
	 */
	ExecFreeProjectionInfo(&resstate->cstate);

	/* ----------------
	 *	shut down subplans
	 * ----------------
	 */
	ExecEndNode(outerPlan(node), (Plan *) node);

	/* ----------------
	 *	clean out the tuple table
	 * ----------------
	 */
	ExecClearTuple(resstate->cstate.cs_ResultTupleSlot);
}
