/*-------------------------------------------------------------------------
 *
 * nodeNestloop.c
 *	  routines to support nest-loop joins
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeNestloop.c,v 1.18 2000/07/17 03:04:53 tgl Exp $
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
ExecNestLoop(NestLoop *node, Plan *parent)
{
	NestLoopState *nlstate;
	Plan	   *innerPlan;
	Plan	   *outerPlan;
	bool		needNewOuterTuple;
	TupleTableSlot *outerTupleSlot;
	TupleTableSlot *innerTupleSlot;
	List	   *qual;
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
	econtext = nlstate->jstate.cs_ExprContext;

	/* ----------------
	 * get the current outer tuple
	 * ----------------
	 */
	outerTupleSlot = nlstate->jstate.cs_OuterTupleSlot;
	econtext->ecxt_outertuple = outerTupleSlot;

	/* ----------------
	 *	Reset per-tuple memory context to free any expression evaluation
	 *	storage allocated in the previous tuple cycle.
	 * ----------------
	 */
	ResetExprContext(econtext);

	/* ----------------
	 *	Check to see if we're still projecting out tuples from a previous
	 *	join tuple (because there is a function-returning-set in the
	 *	projection expressions).  If so, try to project another one.
	 * ----------------
	 */
	if (nlstate->jstate.cs_TupFromTlist)
	{
		TupleTableSlot *result;
		bool		isDone;

		result = ExecProject(nlstate->jstate.cs_ProjInfo, &isDone);
		if (!isDone)
			return result;
		/* Done with that source tuple... */
		nlstate->jstate.cs_TupFromTlist = false;
	}

	/* ----------------
	 *	Ok, everything is setup for the join so now loop until
	 *	we return a qualifying join tuple..
	 * ----------------
	 */
	ENL1_printf("entering main loop");

	for (;;)
	{
		/* ----------------
		 *	The essential idea now is to get the next inner tuple
		 *	and join it with the current outer tuple.
		 * ----------------
		 */
		needNewOuterTuple = TupIsNull(outerTupleSlot);

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

			ENL1_printf("saving new outer tuple information");
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
				ENL1_printf("couldn't get inner tuple - need new outer tuple");
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

		if (ExecQual((List *) qual, econtext, false))
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
	node->join.state = estate;

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
}
