/*-------------------------------------------------------------------------
 *
 * nodeNestloop.c--
 *	  routines to support nest-loop joins
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeNestloop.c,v 1.7 1997/09/08 21:43:16 momjian Exp $
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

#include "executor/executor.h"
#include "executor/execdebug.h"
#include "executor/nodeNestloop.h"
#include "executor/nodeIndexscan.h"

/* ----------------------------------------------------------------
 *		ExecNestLoop(node)
 *
 * old comments
 *		Returns the tuple joined from inner and outer tuples which
 *		satisfies the qualification clause.
 *
 *		It scans the inner relation to join with current outer tuple.
 *
 *		If none is found, next tuple form the outer relation is retrieved
 *		and the inner relation is scanned from the beginning again to join
 *		with the outer tuple.
 *
 *		Nil is returned if all the remaining outer tuples are tried and
 *		all fail to join with the inner tuples.
 *
 *		Nil is also returned if there is no tuple from inner realtion.
 *
 *		Conditions:
 *		  -- outerTuple contains current tuple from outer relation and
 *			 the right son(inner realtion) maintains "cursor" at the tuple
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
ExecNestLoop(NestLoop *node, Plan *parent)
{
	NestLoopState *nlstate;
	Plan	   *innerPlan;
	Plan	   *outerPlan;
	bool		needNewOuterTuple;

	TupleTableSlot *outerTupleSlot;
	TupleTableSlot *innerTupleSlot;

	List	   *qual;
	bool		qualResult;
	ExprContext *econtext;

	/* ----------------
	 *	get information from the node
	 * ----------------
	 */
	ENL1_printf("getting info from node");

	nlstate = node->nlstate;
	qual = node->join.qual;
	outerPlan = outerPlan(&node->join);
	innerPlan = innerPlan(&node->join);

	/* ----------------
	 *	initialize expression context
	 * ----------------
	 */
	econtext = nlstate->jstate.cs_ExprContext;

	/* ----------------			* get the current outer tuple
	 * ----------------
	 */
	outerTupleSlot = nlstate->jstate.cs_OuterTupleSlot;
	econtext->ecxt_outertuple = outerTupleSlot;

	/* ----------------
	 *	Ok, everything is setup for the join so now loop until
	 *	we return a qualifying join tuple..
	 * ----------------
	 */

	if (nlstate->jstate.cs_TupFromTlist)
	{
		TupleTableSlot *result;
		bool		isDone;

		result = ExecProject(nlstate->jstate.cs_ProjInfo, &isDone);
		if (!isDone)
			return result;
	}

	ENL1_printf("entering main loop");
	for (;;)
	{
		/* ----------------
		 *	The essential idea now is to get the next inner tuple
		 *	and join it with the current outer tuple.
		 * ----------------
		 */
		needNewOuterTuple = false;

		/* ----------------
		 *	If outer tuple is not null then that means
		 *	we are in the middle of a scan and we should
		 *	restore our previously saved scan position.
		 * ----------------
		 */
		if (!TupIsNull(outerTupleSlot))
		{
			ENL1_printf("have outer tuple, restoring outer plan");
			ExecRestrPos(outerPlan);
		}
		else
		{
			ENL1_printf("outer tuple is nil, need new outer tuple");
			needNewOuterTuple = true;
		}

		/* ----------------
		 *	if we have an outerTuple, try to get the next inner tuple.
		 * ----------------
		 */
		if (!needNewOuterTuple)
		{
			ENL1_printf("getting new inner tuple");

			innerTupleSlot = ExecProcNode(innerPlan, (Plan *) node);
			econtext->ecxt_innertuple = innerTupleSlot;

			if (TupIsNull(innerTupleSlot))
			{
				ENL1_printf("no inner tuple, need new outer tuple");
				needNewOuterTuple = true;
			}
		}

		/* ----------------
		 *	loop until we have a new outer tuple and a new
		 *	inner tuple.
		 * ----------------
		 */
		while (needNewOuterTuple)
		{
			/* ----------------
			 *	now try to get the next outer tuple
			 * ----------------
			 */
			ENL1_printf("getting new outer tuple");
			outerTupleSlot = ExecProcNode(outerPlan, (Plan *) node);
			econtext->ecxt_outertuple = outerTupleSlot;

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

			/* ----------------
			 *	we have a new outer tuple so we mark our position
			 *	in the outer scan and save the outer tuple in the
			 *	NestLoop state
			 * ----------------
			 */
			ENL1_printf("saving new outer tuple information");
			ExecMarkPos(outerPlan);
			nlstate->jstate.cs_OuterTupleSlot = outerTupleSlot;

			/* ----------------
			 *	now rescan the inner plan and get a new inner tuple
			 * ----------------
			 */

			ENL1_printf("rescanning inner plan");

			/*
			 * The scan key of the inner plan might depend on the current
			 * outer tuple (e.g. in index scans), that's why we pass our
			 * expr context.
			 */
			ExecReScan(innerPlan, econtext, parent);

			ENL1_printf("getting new inner tuple");

			innerTupleSlot = ExecProcNode(innerPlan, (Plan *) node);
			econtext->ecxt_innertuple = innerTupleSlot;

			if (TupIsNull(innerTupleSlot))
			{
				ENL1_printf("couldn't get inner tuple - need new outer tuple");
			}
			else
			{
				ENL1_printf("got inner and outer tuples");
				needNewOuterTuple = false;
			}
		}						/* while (needNewOuterTuple) */

		/* ----------------
		 *	 at this point we have a new pair of inner and outer
		 *	 tuples so we test the inner and outer tuples to see
		 *	 if they satisify the node's qualification.
		 * ----------------
		 */
		ENL1_printf("testing qualification");
		qualResult = ExecQual((List *) qual, econtext);

		if (qualResult)
		{
			/* ----------------
			 *	qualification was satisified so we project and
			 *	return the slot containing the result tuple
			 *	using ExecProject().
			 * ----------------
			 */
			ProjectionInfo *projInfo;
			TupleTableSlot *result;
			bool		isDone;

			ENL1_printf("qualification succeeded, projecting tuple");

			projInfo = nlstate->jstate.cs_ProjInfo;
			result = ExecProject(projInfo, &isDone);
			nlstate->jstate.cs_TupFromTlist = !isDone;
			return result;
		}

		/* ----------------
		 *	qualification failed so we have to try again..
		 * ----------------
		 */
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
	node->join.state = estate;

	/* ----------------
	 *	  create new nest loop state
	 * ----------------
	 */
	nlstate = makeNode(NestLoopState);
	nlstate->nl_PortalFlag = false;
	node->nlstate = nlstate;

	/* ----------------
	 *	Miscellanious initialization
	 *
	 *		 +	assign node's base_id
	 *		 +	assign debugging hooks and
	 *		 +	create expression context for node
	 * ----------------
	 */
	ExecAssignNodeBaseInfo(estate, &nlstate->jstate, parent);
	ExecAssignExprContext(estate, &nlstate->jstate);

#define NESTLOOP_NSLOTS 1
	/* ----------------
	 *	tuple table initialization
	 * ----------------
	 */
	ExecInitResultTupleSlot(estate, &nlstate->jstate);

	/* ----------------
	 *	  now initialize children
	 * ----------------
	 */
	ExecInitNode(outerPlan((Plan *) node), estate, (Plan *) node);
	ExecInitNode(innerPlan((Plan *) node), estate, (Plan *) node);

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
