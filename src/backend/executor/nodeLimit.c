/*-------------------------------------------------------------------------
 *
 * nodeLimit.c
 *	  Routines to handle limiting of query results where appropriate
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeLimit.c,v 1.2 2000/11/05 00:15:52 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecLimit		- extract a limited range of tuples
 *		ExecInitLimit	- initialize node and subnodes..
 *		ExecEndLimit	- shutdown node and subnodes
 */

#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeLimit.h"

static void recompute_limits(Limit *node);


/* ----------------------------------------------------------------
 *		ExecLimit
 *
 *		This is a very simple node which just performs LIMIT/OFFSET
 *		filtering on the stream of tuples returned by a subplan.
 * ----------------------------------------------------------------
 */
TupleTableSlot *				/* return: a tuple or NULL */
ExecLimit(Limit *node)
{
	LimitState *limitstate;
	ScanDirection direction;
	TupleTableSlot *resultTupleSlot;
	TupleTableSlot *slot;
	Plan	   *outerPlan;
	long		netlimit;

	/* ----------------
	 *	get information from the node
	 * ----------------
	 */
	limitstate = node->limitstate;
	direction = node->plan.state->es_direction;
	outerPlan = outerPlan((Plan *) node);
	resultTupleSlot = limitstate->cstate.cs_ResultTupleSlot;

	/* ----------------
	 *	If first call for this scan, compute limit/offset.
	 *	(We can't do this any earlier, because parameters from upper nodes
	 *	may not be set until now.)
	 * ----------------
	 */
	if (! limitstate->parmsSet)
		recompute_limits(node);
	netlimit = limitstate->offset + limitstate->count;

	/* ----------------
	 *	now loop, returning only desired tuples.
	 * ----------------
	 */
	for (;;)
	{
		/*----------------
		 *	 If we have reached the subplan EOF or the limit, just quit.
		 *
		 * NOTE: when scanning forwards, we must fetch one tuple beyond the
		 * COUNT limit before we can return NULL, else the subplan won't be
		 * properly positioned to start going backwards.  Hence test here
		 * is for position > netlimit not position >= netlimit.
		 *
		 * Similarly, when scanning backwards, we must re-fetch the last
		 * tuple in the offset region before we can return NULL.  Otherwise
		 * we won't be correctly aligned to start going forward again.  So,
		 * although you might think we can quit when position = offset + 1,
		 * we have to fetch a subplan tuple first, and then exit when
		 * position = offset.
		 *----------------
		 */
		if (ScanDirectionIsForward(direction))
		{
			if (limitstate->atEnd)
				return NULL;
			if (! limitstate->noCount && limitstate->position > netlimit)
				return NULL;
		}
		else
		{
			if (limitstate->position <= limitstate->offset)
				return NULL;
		}
		/* ----------------
		 *	 fetch a tuple from the outer subplan
		 * ----------------
		 */
		slot = ExecProcNode(outerPlan, (Plan *) node);
		if (TupIsNull(slot))
		{
			/*
			 * We are at start or end of the subplan.  Update local state
			 * appropriately, but always return NULL.
			 */
			if (ScanDirectionIsForward(direction))
			{
				Assert(! limitstate->atEnd);
				/* must bump position to stay in sync for backwards fetch */
				limitstate->position++;
				limitstate->atEnd = true;
			}
			else
			{
				limitstate->position = 0;
				limitstate->atEnd = false;
			}
			return NULL;
		}
		/*
		 * We got the next subplan tuple successfully, so adjust state.
		 */
		if (ScanDirectionIsForward(direction))
			limitstate->position++;
		else
		{
			limitstate->position--;
			Assert(limitstate->position > 0);
		}
		limitstate->atEnd = false;

		/* ----------------
		 *	 Now, is this a tuple we want?  If not, loop around to fetch
		 *	 another tuple from the subplan.
		 * ----------------
		 */
		if (limitstate->position > limitstate->offset &&
			(limitstate->noCount || limitstate->position <= netlimit))
			break;
	}

	ExecStoreTuple(slot->val,
				   resultTupleSlot,
				   InvalidBuffer,
				   false);		/* tuple does not belong to slot */

	return resultTupleSlot;
}

/*
 * Evaluate the limit/offset expressions --- done at start of each scan.
 *
 * This is also a handy place to reset the current-position state info.
 */
static void
recompute_limits(Limit *node)
{
	LimitState *limitstate = node->limitstate;
	ExprContext *econtext = limitstate->cstate.cs_ExprContext;
	bool		isNull;

	if (node->limitOffset)
	{
		limitstate->offset = DatumGetInt32(ExecEvalExpr(node->limitOffset,
														econtext,
														&isNull,
														NULL));
		/* Interpret NULL offset as no offset */
		if (isNull)
			limitstate->offset = 0;
		else if (limitstate->offset < 0)
			limitstate->offset = 0;
	}
	else
	{
		/* No OFFSET supplied */
		limitstate->offset = 0;
	}

	if (node->limitCount)
	{
		limitstate->count = DatumGetInt32(ExecEvalExpr(node->limitCount,
														econtext,
														&isNull,
														NULL));
		/* Interpret NULL count as no count (LIMIT ALL) */
		if (isNull)
			limitstate->noCount = true;
		else if (limitstate->count < 0)
			limitstate->count = 0;
	}
	else
	{
		/* No COUNT supplied */
		limitstate->count = 0;
		limitstate->noCount = true;
	}

	/* Reset position data to start-of-scan */
	limitstate->position = 0;
	limitstate->atEnd = false;

	/* Set flag that params are computed */
	limitstate->parmsSet = true;
}

/* ----------------------------------------------------------------
 *		ExecInitLimit
 *
 *		This initializes the limit node state structures and
 *		the node's subplan.
 * ----------------------------------------------------------------
 */
bool							/* return: initialization status */
ExecInitLimit(Limit *node, EState *estate, Plan *parent)
{
	LimitState *limitstate;
	Plan	   *outerPlan;

	/* ----------------
	 *	assign execution state to node
	 * ----------------
	 */
	node->plan.state = estate;

	/* ----------------
	 *	create new LimitState for node
	 * ----------------
	 */
	limitstate = makeNode(LimitState);
	node->limitstate = limitstate;
	limitstate->parmsSet = false;

	/* ----------------
	 *	Miscellaneous initialization
	 *
	 *	Limit nodes never call ExecQual or ExecProject, but they need
	 *	an exprcontext anyway to evaluate the limit/offset parameters in.
	 * ----------------
	 */
	ExecAssignExprContext(estate, &limitstate->cstate);

#define LIMIT_NSLOTS 1
	/* ------------
	 * Tuple table initialization
	 * ------------
	 */
	ExecInitResultTupleSlot(estate, &limitstate->cstate);

	/* ----------------
	 *	then initialize outer plan
	 * ----------------
	 */
	outerPlan = outerPlan((Plan *) node);
	ExecInitNode(outerPlan, estate, (Plan *) node);

	/* ----------------
	 *	limit nodes do no projections, so initialize
	 *	projection info for this node appropriately
	 * ----------------
	 */
	ExecAssignResultTypeFromOuterPlan((Plan *) node, &limitstate->cstate);
	limitstate->cstate.cs_ProjInfo = NULL;

	return TRUE;
}

int
ExecCountSlotsLimit(Limit *node)
{
	return ExecCountSlotsNode(outerPlan(node)) +
	ExecCountSlotsNode(innerPlan(node)) +
	LIMIT_NSLOTS;
}

/* ----------------------------------------------------------------
 *		ExecEndLimit
 *
 *		This shuts down the subplan and frees resources allocated
 *		to this node.
 * ----------------------------------------------------------------
 */
void
ExecEndLimit(Limit *node)
{
	LimitState *limitstate = node->limitstate;

	ExecFreeExprContext(&limitstate->cstate);

	ExecEndNode(outerPlan((Plan *) node), (Plan *) node);

	/* clean up tuple table */
	ExecClearTuple(limitstate->cstate.cs_ResultTupleSlot);
}


void
ExecReScanLimit(Limit *node, ExprContext *exprCtxt, Plan *parent)
{
	LimitState *limitstate = node->limitstate;

	ExecClearTuple(limitstate->cstate.cs_ResultTupleSlot);

	/* force recalculation of limit expressions on first call */
	limitstate->parmsSet = false;

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (((Plan *) node)->lefttree->chgParam == NULL)
		ExecReScan(((Plan *) node)->lefttree, exprCtxt, (Plan *) node);
}
