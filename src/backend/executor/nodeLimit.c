/*-------------------------------------------------------------------------
 *
 * nodeLimit.c
 *	  Routines to handle limiting of query results where appropriate
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeLimit.c,v 1.11 2002/11/22 22:10:01 tgl Exp $
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

	/*
	 * get information from the node
	 */
	limitstate = node->limitstate;
	direction = node->plan.state->es_direction;
	outerPlan = outerPlan((Plan *) node);
	resultTupleSlot = limitstate->cstate.cs_ResultTupleSlot;

	/*
	 * The main logic is a simple state machine.
	 */
	switch (limitstate->lstate)
	{
		case LIMIT_INITIAL:
			/*
			 * If backwards scan, just return NULL without changing state.
			 */
			if (!ScanDirectionIsForward(direction))
				return NULL;
			/*
			 * First call for this scan, so compute limit/offset. (We can't do
			 * this any earlier, because parameters from upper nodes may not
			 * be set until now.)  This also sets position = 0.
			 */
			recompute_limits(node);
			/*
			 * Check for empty window; if so, treat like empty subplan.
			 */
			if (limitstate->count <= 0 && !limitstate->noCount)
			{
				limitstate->lstate = LIMIT_EMPTY;
				return NULL;
			}
			/*
			 * Fetch rows from subplan until we reach position > offset.
			 */
			for (;;)
			{
				slot = ExecProcNode(outerPlan, (Plan *) node);
				if (TupIsNull(slot))
				{
					/*
					 * The subplan returns too few tuples for us to produce
					 * any output at all.
					 */
					limitstate->lstate = LIMIT_EMPTY;
					return NULL;
				}
				limitstate->subSlot = slot;
				if (++limitstate->position > limitstate->offset)
					break;
			}
			/*
			 * Okay, we have the first tuple of the window.
			 */
			limitstate->lstate = LIMIT_INWINDOW;
			break;

		case LIMIT_EMPTY:
			/*
			 * The subplan is known to return no tuples (or not more than
			 * OFFSET tuples, in general).  So we return no tuples.
			 */
			return NULL;

		case LIMIT_INWINDOW:
			if (ScanDirectionIsForward(direction))
			{
				/*
				 * Forwards scan, so check for stepping off end of window.
				 * If we are at the end of the window, return NULL without
				 * advancing the subplan or the position variable; but
				 * change the state machine state to record having done so.
				 */
				if (!limitstate->noCount &&
					limitstate->position >= limitstate->offset + limitstate->count)
				{
					limitstate->lstate = LIMIT_WINDOWEND;
					return NULL;
				}
				/*
				 * Get next tuple from subplan, if any.
				 */
				slot = ExecProcNode(outerPlan, (Plan *) node);
				if (TupIsNull(slot))
				{
					limitstate->lstate = LIMIT_SUBPLANEOF;
					return NULL;
				}
				limitstate->subSlot = slot;
				limitstate->position++;
			}
			else
			{
				/*
				 * Backwards scan, so check for stepping off start of window.
				 * As above, change only state-machine status if so.
				 */
				if (limitstate->position <= limitstate->offset + 1)
				{
					limitstate->lstate = LIMIT_WINDOWSTART;
					return NULL;
				}
				/*
				 * Get previous tuple from subplan; there should be one!
				 */
				slot = ExecProcNode(outerPlan, (Plan *) node);
				if (TupIsNull(slot))
					elog(ERROR, "ExecLimit: subplan failed to run backwards");
				limitstate->subSlot = slot;
				limitstate->position--;
			}
			break;

		case LIMIT_SUBPLANEOF:
			if (ScanDirectionIsForward(direction))
				return NULL;
			/*
			 * Backing up from subplan EOF, so re-fetch previous tuple;
			 * there should be one!  Note previous tuple must be in window.
			 */
			slot = ExecProcNode(outerPlan, (Plan *) node);
			if (TupIsNull(slot))
				elog(ERROR, "ExecLimit: subplan failed to run backwards");
			limitstate->subSlot = slot;
			limitstate->lstate = LIMIT_INWINDOW;
			/* position does not change 'cause we didn't advance it before */
			break;

		case LIMIT_WINDOWEND:
			if (ScanDirectionIsForward(direction))
				return NULL;
			/*
			 * Backing up from window end: simply re-return the last
			 * tuple fetched from the subplan.
			 */
			slot = limitstate->subSlot;
			limitstate->lstate = LIMIT_INWINDOW;
			/* position does not change 'cause we didn't advance it before */
			break;

		case LIMIT_WINDOWSTART:
			if (!ScanDirectionIsForward(direction))
				return NULL;
			/*
			 * Advancing after having backed off window start: simply
			 * re-return the last tuple fetched from the subplan.
			 */
			slot = limitstate->subSlot;
			limitstate->lstate = LIMIT_INWINDOW;
			/* position does not change 'cause we didn't change it before */
			break;

		default:
			elog(ERROR, "ExecLimit: impossible state %d",
				 (int) limitstate->lstate);
			slot = NULL;		/* keep compiler quiet */
			break;
	}

	/* Return the current tuple */
	Assert(!TupIsNull(slot));

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
		limitstate->offset =
			DatumGetInt32(ExecEvalExprSwitchContext(node->limitOffset,
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
		limitstate->noCount = false;
		limitstate->count =
			DatumGetInt32(ExecEvalExprSwitchContext(node->limitCount,
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

	/* Reset position to start-of-scan */
	limitstate->position = 0;
	limitstate->subSlot = NULL;
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

	/*
	 * assign execution state to node
	 */
	node->plan.state = estate;

	/*
	 * create new LimitState for node
	 */
	limitstate = makeNode(LimitState);
	node->limitstate = limitstate;
	limitstate->lstate = LIMIT_INITIAL;

	/*
	 * Miscellaneous initialization
	 *
	 * Limit nodes never call ExecQual or ExecProject, but they need an
	 * exprcontext anyway to evaluate the limit/offset parameters in.
	 */
	ExecAssignExprContext(estate, &limitstate->cstate);

#define LIMIT_NSLOTS 1

	/*
	 * Tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &limitstate->cstate);

	/*
	 * then initialize outer plan
	 */
	outerPlan = outerPlan((Plan *) node);
	ExecInitNode(outerPlan, estate, (Plan *) node);

	/*
	 * limit nodes do no projections, so initialize projection info for
	 * this node appropriately
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

	/* resetting lstate will force offset/limit recalculation */
	limitstate->lstate = LIMIT_INITIAL;

	ExecClearTuple(limitstate->cstate.cs_ResultTupleSlot);

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (((Plan *) node)->lefttree->chgParam == NULL)
		ExecReScan(((Plan *) node)->lefttree, exprCtxt, (Plan *) node);
}
