/*-------------------------------------------------------------------------
 *
 * nodeNestloop.c
 *	  routines to support nest-loop joins
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeNestloop.c,v 1.22 2001/01/24 19:42:55 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 INTERFACE ROUTINES
 *		ExecNestLoop	 - process a nestloop join of two plans
 *		ExecInitNestLoop - initialize the join
 *		ExecEndNestLoop  - shut down the join
 */

#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/nodeNestloop.h"
#include "utils/memutils.h"


/* ----------------------------------------------------------------
 *		ExecNestLoop(node)
 *
 * old comments
 *		Returns the tuple joined from inner and outer tuples which
 *		satisfies the qualification clause.
 *
 *		It scans the inner relation to join with current outer tuple.
 *
 *		If none is found, next tuple from the outer relation is retrieved
 *		and the inner relation is scanned from the beginning again to join
 *		with the outer tuple.
 *
 *		NULL is returned if all the remaining outer tuples are tried and
 *		all fail to join with the inner tuples.
 *
 *		NULL is also returned if there is no tuple from inner relation.
 *
 *		Conditions:
 *		  -- outerTuple contains current tuple from outer relation and
 *			 the right son(inner relation) maintains "cursor" at the tuple
 *			 returned previously.
 *				This is achieved by maintaining a scan position on the outer
 *				relation.
 *
 *		Initial States:
 *		  -- the outer child and the inner child
 *			   are prepared to return the first tuple.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecNestLoop(NestLoop *node)
{
	NestLoopState *nlstate;
	Plan	   *innerPlan;
	Plan	   *outerPlan;
	TupleTableSlot *outerTupleSlot;
	TupleTableSlot *innerTupleSlot;
	List	   *joinqual;
	List	   *otherqual;
	ExprContext *econtext;

	/* ----------------
	 *	get information from the node
	 * ----------------
	 */
	ENL1_printf("getting info from node");

	nlstate = node->nlstate;
	joinqual = node->join.joinqual;
	otherqual = node->join.plan.qual;
	outerPlan = outerPlan((Plan *) node);
	innerPlan = innerPlan((Plan *) node);
	econtext = nlstate->jstate.cs_ExprContext;

	/* ----------------
	 * get the current outer tuple
	 * ----------------
	 */
	outerTupleSlot = nlstate->jstate.cs_OuterTupleSlot;
	econtext->ecxt_outertuple = outerTupleSlot;

	/* ----------------
	 *	Check to see if we're still projecting out tuples from a previous
	 *	join tuple (because there is a function-returning-set in the
	 *	projection expressions).  If so, try to project another one.
	 * ----------------
	 */
	if (nlstate->jstate.cs_TupFromTlist)
	{
		TupleTableSlot *result;
		ExprDoneCond	isDone;

		result = ExecProject(nlstate->jstate.cs_ProjInfo, &isDone);
		if (isDone == ExprMultipleResult)
			return result;
		/* Done with that source tuple... */
		nlstate->jstate.cs_TupFromTlist = false;
	}

	/* ----------------
	 *	Reset per-tuple memory context to free any expression evaluation
	 *	storage allocated in the previous tuple cycle.  Note this can't
	 *	happen until we're done projecting out tuples from a join tuple.
	 * ----------------
	 */
	ResetExprContext(econtext);

	/* ----------------
	 *	Ok, everything is setup for the join so now loop until
	 *	we return a qualifying join tuple.
	 * ----------------
	 */
	ENL1_printf("entering main loop");

	for (;;)
	{
		/* ----------------
		 *	If we don't have an outer tuple, get the next one and
		 *	reset the inner scan.
		 * ----------------
		 */
		if (nlstate->nl_NeedNewOuter)
		{
			ENL1_printf("getting new outer tuple");
			outerTupleSlot = ExecProcNode(outerPlan, (Plan *) node);

			/* ----------------
			 *	if there are no more outer tuples, then the join
			 *	is complete..
			 * ----------------
			 */
			if (TupIsNull(outerTupleSlot))
			{
				ENL1_printf("no outer tuple, ending join");
				return NULL;
			}

			ENL1_printf("saving new outer tuple information");
			nlstate->jstate.cs_OuterTupleSlot = outerTupleSlot;
			econtext->ecxt_outertuple = outerTupleSlot;
			nlstate->nl_NeedNewOuter = false;
			nlstate->nl_MatchedOuter = false;

			/* ----------------
			 *	now rescan the inner plan
			 * ----------------
			 */
			ENL1_printf("rescanning inner plan");

			/*
			 * The scan key of the inner plan might depend on the current
			 * outer tuple (e.g. in index scans), that's why we pass our
			 * expr context.
			 */
			ExecReScan(innerPlan, econtext, (Plan *) node);
		}

		/* ----------------
		 *	we have an outerTuple, try to get the next inner tuple.
		 * ----------------
		 */
		ENL1_printf("getting new inner tuple");

		innerTupleSlot = ExecProcNode(innerPlan, (Plan *) node);
		econtext->ecxt_innertuple = innerTupleSlot;

		if (TupIsNull(innerTupleSlot))
		{
			ENL1_printf("no inner tuple, need new outer tuple");

			nlstate->nl_NeedNewOuter = true;

			if (! nlstate->nl_MatchedOuter &&
				node->join.jointype == JOIN_LEFT)
			{
				/*
				 * We are doing an outer join and there were no join matches
				 * for this outer tuple.  Generate a fake join tuple with
				 * nulls for the inner tuple, and return it if it passes
				 * the non-join quals.
				 */
				econtext->ecxt_innertuple = nlstate->nl_NullInnerTupleSlot;

				ENL1_printf("testing qualification for outer-join tuple");

				if (ExecQual(otherqual, econtext, false))
				{
					/* ----------------
					 *	qualification was satisfied so we project and
					 *	return the slot containing the result tuple
					 *	using ExecProject().
					 * ----------------
					 */
					TupleTableSlot *result;
					ExprDoneCond isDone;

					ENL1_printf("qualification succeeded, projecting tuple");

					result = ExecProject(nlstate->jstate.cs_ProjInfo, &isDone);

					if (isDone != ExprEndResult)
					{
						nlstate->jstate.cs_TupFromTlist =
							(isDone == ExprMultipleResult);
						return result;
					}
				}
			}
			/*
			 * Otherwise just return to top of loop for a new outer tuple.
			 */
			continue;
		}

		/* ----------------
		 *	 at this point we have a new pair of inner and outer
		 *	 tuples so we test the inner and outer tuples to see
		 *	 if they satisfy the node's qualification.
		 *
		 *	 Only the joinquals determine MatchedOuter status,
		 *	 but all quals must pass to actually return the tuple.
		 * ----------------
		 */
		ENL1_printf("testing qualification");

		if (ExecQual(joinqual, econtext, false))
		{
			nlstate->nl_MatchedOuter = true;

			if (otherqual == NIL || ExecQual(otherqual, econtext, false))
			{
				/* ----------------
				 *	qualification was satisfied so we project and
				 *	return the slot containing the result tuple
				 *	using ExecProject().
				 * ----------------
				 */
				TupleTableSlot *result;
				ExprDoneCond isDone;

				ENL1_printf("qualification succeeded, projecting tuple");

				result = ExecProject(nlstate->jstate.cs_ProjInfo, &isDone);

				if (isDone != ExprEndResult)
				{
					nlstate->jstate.cs_TupFromTlist =
						(isDone == ExprMultipleResult);
					return result;
				}
			}
		}

		/* ----------------
		 *	Tuple fails qual, so free per-tuple memory and try again.
		 * ----------------
		 */
		ResetExprContext(econtext);

		ENL1_printf("qualification failed, looping");
	}
}

/* ----------------------------------------------------------------
 *		ExecInitNestLoop
 *
 *		Creates the run-time state information for the nestloop node
 *		produced by the planner and initailizes inner and outer relations
 *		(child nodes).
 * ----------------------------------------------------------------
 */
bool
ExecInitNestLoop(NestLoop *node, EState *estate, Plan *parent)
{
	NestLoopState *nlstate;

	NL1_printf("ExecInitNestLoop: %s\n",
			   "initializing node");

	/* ----------------
	 *	assign execution state to node
	 * ----------------
	 */
	node->join.plan.state = estate;

	/* ----------------
	 *	  create new nest loop state
	 * ----------------
	 */
	nlstate = makeNode(NestLoopState);
	node->nlstate = nlstate;

	/* ----------------
	 *	Miscellaneous initialization
	 *
	 *		 +	create expression context for node
	 * ----------------
	 */
	ExecAssignExprContext(estate, &nlstate->jstate);

	/* ----------------
	 *	  now initialize children
	 * ----------------
	 */
	ExecInitNode(outerPlan((Plan *) node), estate, (Plan *) node);
	ExecInitNode(innerPlan((Plan *) node), estate, (Plan *) node);

#define NESTLOOP_NSLOTS 2
	/* ----------------
	 *	tuple table initialization
	 * ----------------
	 */
	ExecInitResultTupleSlot(estate, &nlstate->jstate);

	switch (node->join.jointype)
	{
		case JOIN_INNER:
			break;
		case JOIN_LEFT:
			nlstate->nl_NullInnerTupleSlot =
				ExecInitNullTupleSlot(estate,
									  ExecGetTupType(innerPlan((Plan*) node)));
			break;
		default:
			elog(ERROR, "ExecInitNestLoop: unsupported join type %d",
				 (int) node->join.jointype);
	}

	/* ----------------
	 *	initialize tuple type and projection info
	 * ----------------
	 */
	ExecAssignResultTypeFromTL((Plan *) node, &nlstate->jstate);
	ExecAssignProjectionInfo((Plan *) node, &nlstate->jstate);

	/* ----------------
	 *	finally, wipe the current outer tuple clean.
	 * ----------------
	 */
	nlstate->jstate.cs_OuterTupleSlot = NULL;
	nlstate->jstate.cs_TupFromTlist = false;
	nlstate->nl_NeedNewOuter = true;
	nlstate->nl_MatchedOuter = false;

	NL1_printf("ExecInitNestLoop: %s\n",
			   "node initialized");
	return TRUE;
}

int
ExecCountSlotsNestLoop(NestLoop *node)
{
	return ExecCountSlotsNode(outerPlan(node)) +
	ExecCountSlotsNode(innerPlan(node)) +
	NESTLOOP_NSLOTS;
}

/* ----------------------------------------------------------------
 *		ExecEndNestLoop
 *
 *		closes down scans and frees allocated storage
 * ----------------------------------------------------------------
 */
void
ExecEndNestLoop(NestLoop *node)
{
	NestLoopState *nlstate;

	NL1_printf("ExecEndNestLoop: %s\n",
			   "ending node processing");

	/* ----------------
	 *	get info from the node
	 * ----------------
	 */
	nlstate = node->nlstate;

	/* ----------------
	 *	Free the projection info
	 *
	 *	Note: we don't ExecFreeResultType(nlstate)
	 *		  because the rule manager depends on the tupType
	 *		  returned by ExecMain().  So for now, this
	 *		  is freed at end-transaction time.  -cim 6/2/91
	 * ----------------
	 */
	ExecFreeProjectionInfo(&nlstate->jstate);
	ExecFreeExprContext(&nlstate->jstate);

	/* ----------------
	 *	close down subplans
	 * ----------------
	 */
	ExecEndNode(outerPlan((Plan *) node), (Plan *) node);
	ExecEndNode(innerPlan((Plan *) node), (Plan *) node);

	/* ----------------
	 *	clean out the tuple table
	 * ----------------
	 */
	ExecClearTuple(nlstate->jstate.cs_ResultTupleSlot);

	NL1_printf("ExecEndNestLoop: %s\n",
			   "node processing ended");
}

/* ----------------------------------------------------------------
 *		ExecReScanNestLoop
 * ----------------------------------------------------------------
 */
void
ExecReScanNestLoop(NestLoop *node, ExprContext *exprCtxt, Plan *parent)
{
	NestLoopState *nlstate = node->nlstate;
	Plan	   *outerPlan = outerPlan((Plan *) node);

	/*
	 * If outerPlan->chgParam is not null then plan will be automatically
	 * re-scanned by first ExecProcNode. innerPlan is re-scanned for each
	 * new outer tuple and MUST NOT be re-scanned from here or you'll get
	 * troubles from inner index scans when outer Vars are used as
	 * run-time keys...
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan, exprCtxt, (Plan *) node);

	/* let outerPlan to free its result tuple ... */
	nlstate->jstate.cs_OuterTupleSlot = NULL;
	nlstate->jstate.cs_TupFromTlist = false;
	nlstate->nl_NeedNewOuter = true;
	nlstate->nl_MatchedOuter = false;
}
