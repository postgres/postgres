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
 *				retrieve (x = 1)
 *		and
 *				append emp (name = "mike", salary = 15000)
 *
 *		Result nodes are also used to optimise queries with constant
 *		qualifications (ie, quals that do not depend on the scanned data),
 *		such as:
 *
 *				retrieve (emp.all) where 2 > 1
 *
 *		In this case, the plan generated is
 *
 *						Result	(with 2 > 1 qual)
 *						/
 *				   SeqScan (emp.all)
 *
 *		At runtime, the Result node evaluates the constant qual once.
 *		If it's false, we can return an empty result set without running
 *		the controlled plan at all.  If it's true, we run the controlled
 *		plan normally and pass back the results.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeResult.c,v 1.14 2000/07/12 02:37:04 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeResult.h"

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
TupleTableSlot *
ExecResult(Result *node)
{
	ResultState *resstate;
	TupleTableSlot *outerTupleSlot;
	TupleTableSlot *resultSlot;
	Plan	   *outerPlan;
	ExprContext *econtext;
	bool		isDone;
	ProjectionInfo *projInfo;

	/* ----------------
	 *	initialize the result node's state
	 * ----------------
	 */
	resstate = node->resstate;
	econtext = resstate->cstate.cs_ExprContext;

	/* ----------------
	 *	Reset per-tuple memory context to free any expression evaluation
	 *	storage allocated in the previous tuple cycle.
	 * ----------------
	 */
	ResetExprContext(econtext);

	/* ----------------
	 *	 check constant qualifications like (2 > 1), if not already done
	 * ----------------
	 */
	if (resstate->rs_checkqual)
	{
		bool		qualResult = ExecQual((List *) node->resconstantqual,
										  econtext,
										  false);

		resstate->rs_checkqual = false;
		if (qualResult == false)
		{
			resstate->rs_done = true;
			return NULL;
		}
	}

	/* ----------------
	 *	Check to see if we're still projecting out tuples from a previous
	 *	scan tuple (because there is a function-returning-set in the
	 *	projection expressions).  If so, try to project another one.
	 * ----------------
	 */
	if (resstate->cstate.cs_TupFromTlist)
	{
		resultSlot = ExecProject(resstate->cstate.cs_ProjInfo, &isDone);
		if (!isDone)
			return resultSlot;
		/* Done with that source tuple... */
		resstate->cstate.cs_TupFromTlist = false;
	}

	/* ----------------
	 *	if rs_done is true then it means that we were asked to return
	 *	a constant tuple and we already did the last time ExecResult()
	 *	was called, OR that we failed the constant qual check.
	 *	Either way, now we are through.
	 * ----------------
	 */
	if (!resstate->rs_done)
	{
		outerPlan = outerPlan(node);

		if (outerPlan != NULL)
		{
			/* ----------------
			 *	retrieve tuples from the outer plan until there are no more.
			 * ----------------
			 */
			outerTupleSlot = ExecProcNode(outerPlan, (Plan *) node);

			if (TupIsNull(outerTupleSlot))
				return NULL;

			resstate->cstate.cs_OuterTupleSlot = outerTupleSlot;

			/* ----------------
			 *	 XXX gross hack. use outer tuple as scan tuple for projection
			 * ----------------
			 */
			econtext->ecxt_outertuple = outerTupleSlot;
			econtext->ecxt_scantuple = outerTupleSlot;
		}
		else
		{
			/* ----------------
			 *	if we don't have an outer plan, then we are just generating
			 *	the results from a constant target list.  Do it only once.
			 * ----------------
			 */
			resstate->rs_done = true;
		}

		/* ----------------
		 *	 form the result tuple using ExecProject(), and return it.
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
	resstate->rs_done = false;
	resstate->rs_checkqual = (node->resconstantqual == NULL) ? false : true;
	node->resstate = resstate;

	/* ----------------
	 *	Miscellaneous initialization
	 *
	 *		 +	create expression context for node
	 * ----------------
	 */
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
 *		frees up storage allocated through C routines
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
	ExecFreeExprContext(&resstate->cstate);

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
	pfree(resstate);
	node->resstate = NULL;		/* XXX - new for us - er1p */
}

void
ExecReScanResult(Result *node, ExprContext *exprCtxt, Plan *parent)
{
	ResultState *resstate = node->resstate;

	resstate->rs_done = false;
	resstate->cstate.cs_TupFromTlist = false;
	resstate->rs_checkqual = (node->resconstantqual == NULL) ? false : true;

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (((Plan *) node)->lefttree &&
		((Plan *) node)->lefttree->chgParam == NULL)
		ExecReScan(((Plan *) node)->lefttree, exprCtxt, (Plan *) node);
}
