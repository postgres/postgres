/*-------------------------------------------------------------------------
 *
 * nodeAppend.c
 *	  routines to handle append nodes.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeAppend.c
 *
 *-------------------------------------------------------------------------
 */
/* INTERFACE ROUTINES
 *		ExecInitAppend	- initialize the append node
 *		ExecAppend		- retrieve the next tuple from the node
 *		ExecEndAppend	- shut down the append node
 *		ExecReScanAppend - rescan the append node
 *
 *	 NOTES
 *		Each append node contains a list of one or more subplans which
 *		must be iteratively processed (forwards or backwards).
 *		Tuples are retrieved by executing the 'whichplan'th subplan
 *		until the subplan stops returning tuples, at which point that
 *		plan is shut down and the next started up.
 *
 *		Append nodes don't make use of their left and right
 *		subtrees, rather they maintain a list of subplans so
 *		a typical append node looks like this in the plan tree:
 *
 *				   ...
 *				   /
 *				Append -------+------+------+--- nil
 *				/	\		  |		 |		|
 *			  nil	nil		 ...    ...    ...
 *								 subplans
 *
 *		Append nodes are currently used for unions, and to support
 *		inheritance queries, where several relations need to be scanned.
 *		For example, in our standard person/student/employee/student-emp
 *		example, where student and employee inherit from person
 *		and student-emp inherits from student and employee, the
 *		query:
 *
 *				select name from person
 *
 *		generates the plan:
 *
 *				  |
 *				Append -------+-------+--------+--------+
 *				/	\		  |		  |		   |		|
 *			  nil	nil		 Scan	 Scan	  Scan	   Scan
 *							  |		  |		   |		|
 *							person employee student student-emp
 */

#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/execPartition.h"
#include "executor/nodeAppend.h"
#include "miscadmin.h"

/* Shared state for parallel-aware Append. */
struct ParallelAppendState
{
	LWLock		pa_lock;		/* mutual exclusion to choose next subplan */
	int			pa_next_plan;	/* next plan to choose by any worker */

	/*
	 * pa_finished[i] should be true if no more workers should select subplan
	 * i.  for a non-partial plan, this should be set to true as soon as a
	 * worker selects the plan; for a partial plan, it remains false until
	 * some worker executes the plan to completion.
	 */
	bool		pa_finished[FLEXIBLE_ARRAY_MEMBER];
};

#define INVALID_SUBPLAN_INDEX		-1

static TupleTableSlot *ExecAppend(PlanState *pstate);
static bool choose_next_subplan_locally(AppendState *node);
static bool choose_next_subplan_for_leader(AppendState *node);
static bool choose_next_subplan_for_worker(AppendState *node);
static void mark_invalid_subplans_as_finished(AppendState *node);

/* ----------------------------------------------------------------
 *		ExecInitAppend
 *
 *		Begin all of the subscans of the append node.
 *
 *	   (This is potentially wasteful, since the entire result of the
 *		append node may not be scanned, but this way all of the
 *		structures get allocated in the executor's top level memory
 *		block instead of that of the call to ExecAppend.)
 * ----------------------------------------------------------------
 */
AppendState *
ExecInitAppend(Append *node, EState *estate, int eflags)
{
	AppendState *appendstate = makeNode(AppendState);
	PlanState **appendplanstates;
	Bitmapset  *validsubplans;
	int			nplans;
	int			firstvalid;
	int			i,
				j;

	/* check for unsupported flags */
	Assert(!(eflags & EXEC_FLAG_MARK));

	/*
	 * create new AppendState for our append node
	 */
	appendstate->ps.plan = (Plan *) node;
	appendstate->ps.state = estate;
	appendstate->ps.ExecProcNode = ExecAppend;

	/* Let choose_next_subplan_* function handle setting the first subplan */
	appendstate->as_whichplan = INVALID_SUBPLAN_INDEX;

	/* If run-time partition pruning is enabled, then set that up now */
	if (node->part_prune_info != NULL)
	{
		PartitionPruneState *prunestate;

		/* We may need an expression context to evaluate partition exprs */
		ExecAssignExprContext(estate, &appendstate->ps);

		/* Create the working data structure for pruning. */
		prunestate = ExecCreatePartitionPruneState(&appendstate->ps,
												   node->part_prune_info);
		appendstate->as_prune_state = prunestate;

		/* Perform an initial partition prune, if required. */
		if (prunestate->do_initial_prune)
		{
			/* Determine which subplans survive initial pruning */
			validsubplans = ExecFindInitialMatchingSubPlans(prunestate,
															list_length(node->appendplans));

			nplans = bms_num_members(validsubplans);
		}
		else
		{
			/* We'll need to initialize all subplans */
			nplans = list_length(node->appendplans);
			Assert(nplans > 0);
			validsubplans = bms_add_range(NULL, 0, nplans - 1);
		}

		/*
		 * When no run-time pruning is required and there's at least one
		 * subplan, we can fill as_valid_subplans immediately, preventing
		 * later calls to ExecFindMatchingSubPlans.
		 */
		if (!prunestate->do_exec_prune && nplans > 0)
			appendstate->as_valid_subplans = bms_add_range(NULL, 0, nplans - 1);
	}
	else
	{
		nplans = list_length(node->appendplans);

		/*
		 * When run-time partition pruning is not enabled we can just mark all
		 * subplans as valid; they must also all be initialized.
		 */
		Assert(nplans > 0);
		appendstate->as_valid_subplans = validsubplans =
			bms_add_range(NULL, 0, nplans - 1);
		appendstate->as_prune_state = NULL;
	}

	/*
	 * Initialize result tuple type and slot.
	 */
	ExecInitResultTupleSlotTL(&appendstate->ps, &TTSOpsVirtual);

	/* node returns slots from each of its subnodes, therefore not fixed */
	appendstate->ps.resultopsset = true;
	appendstate->ps.resultopsfixed = false;

	appendplanstates = (PlanState **) palloc(nplans *
											 sizeof(PlanState *));

	/*
	 * call ExecInitNode on each of the valid plans to be executed and save
	 * the results into the appendplanstates array.
	 *
	 * While at it, find out the first valid partial plan.
	 */
	j = 0;
	firstvalid = nplans;
	i = -1;
	while ((i = bms_next_member(validsubplans, i)) >= 0)
	{
		Plan	   *initNode = (Plan *) list_nth(node->appendplans, i);

		/*
		 * Record the lowest appendplans index which is a valid partial plan.
		 */
		if (i >= node->first_partial_plan && j < firstvalid)
			firstvalid = j;

		appendplanstates[j++] = ExecInitNode(initNode, estate, eflags);
	}

	appendstate->as_first_partial_plan = firstvalid;
	appendstate->appendplans = appendplanstates;
	appendstate->as_nplans = nplans;

	/*
	 * Miscellaneous initialization
	 */

	appendstate->ps.ps_ProjInfo = NULL;

	/* For parallel query, this will be overridden later. */
	appendstate->choose_next_subplan = choose_next_subplan_locally;

	return appendstate;
}

/* ----------------------------------------------------------------
 *	   ExecAppend
 *
 *		Handles iteration over multiple subplans.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecAppend(PlanState *pstate)
{
	AppendState *node = castNode(AppendState, pstate);

	if (node->as_whichplan < 0)
	{
		/* Nothing to do if there are no subplans */
		if (node->as_nplans == 0)
			return ExecClearTuple(node->ps.ps_ResultTupleSlot);

		/*
		 * If no subplan has been chosen, we must choose one before
		 * proceeding.
		 */
		if (node->as_whichplan == INVALID_SUBPLAN_INDEX &&
			!node->choose_next_subplan(node))
			return ExecClearTuple(node->ps.ps_ResultTupleSlot);
	}

	for (;;)
	{
		PlanState  *subnode;
		TupleTableSlot *result;

		CHECK_FOR_INTERRUPTS();

		/*
		 * figure out which subplan we are currently processing
		 */
		Assert(node->as_whichplan >= 0 && node->as_whichplan < node->as_nplans);
		subnode = node->appendplans[node->as_whichplan];

		/*
		 * get a tuple from the subplan
		 */
		result = ExecProcNode(subnode);

		if (!TupIsNull(result))
		{
			/*
			 * If the subplan gave us something then return it as-is. We do
			 * NOT make use of the result slot that was set up in
			 * ExecInitAppend; there's no need for it.
			 */
			return result;
		}

		/* choose new subplan; if none, we're done */
		if (!node->choose_next_subplan(node))
			return ExecClearTuple(node->ps.ps_ResultTupleSlot);
	}
}

/* ----------------------------------------------------------------
 *		ExecEndAppend
 *
 *		Shuts down the subscans of the append node.
 *
 *		Returns nothing of interest.
 * ----------------------------------------------------------------
 */
void
ExecEndAppend(AppendState *node)
{
	PlanState **appendplans;
	int			nplans;
	int			i;

	/*
	 * get information from the node
	 */
	appendplans = node->appendplans;
	nplans = node->as_nplans;

	/*
	 * shut down each of the subscans
	 */
	for (i = 0; i < nplans; i++)
		ExecEndNode(appendplans[i]);
}

void
ExecReScanAppend(AppendState *node)
{
	int			i;

	/*
	 * If any PARAM_EXEC Params used in pruning expressions have changed, then
	 * we'd better unset the valid subplans so that they are reselected for
	 * the new parameter values.
	 */
	if (node->as_prune_state &&
		bms_overlap(node->ps.chgParam,
					node->as_prune_state->execparamids))
	{
		bms_free(node->as_valid_subplans);
		node->as_valid_subplans = NULL;
	}

	for (i = 0; i < node->as_nplans; i++)
	{
		PlanState  *subnode = node->appendplans[i];

		/*
		 * ExecReScan doesn't know about my subplans, so I have to do
		 * changed-parameter signaling myself.
		 */
		if (node->ps.chgParam != NULL)
			UpdateChangedParamSet(subnode, node->ps.chgParam);

		/*
		 * If chgParam of subnode is not null then plan will be re-scanned by
		 * first ExecProcNode.
		 */
		if (subnode->chgParam == NULL)
			ExecReScan(subnode);
	}

	/* Let choose_next_subplan_* function handle setting the first subplan */
	node->as_whichplan = INVALID_SUBPLAN_INDEX;
}

/* ----------------------------------------------------------------
 *						Parallel Append Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecAppendEstimate
 *
 *		Compute the amount of space we'll need in the parallel
 *		query DSM, and inform pcxt->estimator about our needs.
 * ----------------------------------------------------------------
 */
void
ExecAppendEstimate(AppendState *node,
				   ParallelContext *pcxt)
{
	node->pstate_len =
		add_size(offsetof(ParallelAppendState, pa_finished),
				 sizeof(bool) * node->as_nplans);

	shm_toc_estimate_chunk(&pcxt->estimator, node->pstate_len);
	shm_toc_estimate_keys(&pcxt->estimator, 1);
}


/* ----------------------------------------------------------------
 *		ExecAppendInitializeDSM
 *
 *		Set up shared state for Parallel Append.
 * ----------------------------------------------------------------
 */
void
ExecAppendInitializeDSM(AppendState *node,
						ParallelContext *pcxt)
{
	ParallelAppendState *pstate;

	pstate = shm_toc_allocate(pcxt->toc, node->pstate_len);
	memset(pstate, 0, node->pstate_len);
	LWLockInitialize(&pstate->pa_lock, LWTRANCHE_PARALLEL_APPEND);
	shm_toc_insert(pcxt->toc, node->ps.plan->plan_node_id, pstate);

	node->as_pstate = pstate;
	node->choose_next_subplan = choose_next_subplan_for_leader;
}

/* ----------------------------------------------------------------
 *		ExecAppendReInitializeDSM
 *
 *		Reset shared state before beginning a fresh scan.
 * ----------------------------------------------------------------
 */
void
ExecAppendReInitializeDSM(AppendState *node, ParallelContext *pcxt)
{
	ParallelAppendState *pstate = node->as_pstate;

	pstate->pa_next_plan = 0;
	memset(pstate->pa_finished, 0, sizeof(bool) * node->as_nplans);
}

/* ----------------------------------------------------------------
 *		ExecAppendInitializeWorker
 *
 *		Copy relevant information from TOC into planstate, and initialize
 *		whatever is required to choose and execute the optimal subplan.
 * ----------------------------------------------------------------
 */
void
ExecAppendInitializeWorker(AppendState *node, ParallelWorkerContext *pwcxt)
{
	node->as_pstate = shm_toc_lookup(pwcxt->toc, node->ps.plan->plan_node_id, false);
	node->choose_next_subplan = choose_next_subplan_for_worker;
}

/* ----------------------------------------------------------------
 *		choose_next_subplan_locally
 *
 *		Choose next subplan for a non-parallel-aware Append,
 *		returning false if there are no more.
 * ----------------------------------------------------------------
 */
static bool
choose_next_subplan_locally(AppendState *node)
{
	int			whichplan = node->as_whichplan;
	int			nextplan;

	/* We should never be called when there are no subplans */
	Assert(node->as_nplans > 0);

	/*
	 * If first call then have the bms member function choose the first valid
	 * subplan by initializing whichplan to -1.  If there happen to be no
	 * valid subplans then the bms member function will handle that by
	 * returning a negative number which will allow us to exit returning a
	 * false value.
	 */
	if (whichplan == INVALID_SUBPLAN_INDEX)
	{
		if (node->as_valid_subplans == NULL)
			node->as_valid_subplans =
				ExecFindMatchingSubPlans(node->as_prune_state);

		whichplan = -1;
	}

	/* Ensure whichplan is within the expected range */
	Assert(whichplan >= -1 && whichplan <= node->as_nplans);

	if (ScanDirectionIsForward(node->ps.state->es_direction))
		nextplan = bms_next_member(node->as_valid_subplans, whichplan);
	else
		nextplan = bms_prev_member(node->as_valid_subplans, whichplan);

	if (nextplan < 0)
		return false;

	node->as_whichplan = nextplan;

	return true;
}

/* ----------------------------------------------------------------
 *		choose_next_subplan_for_leader
 *
 *      Try to pick a plan which doesn't commit us to doing much
 *      work locally, so that as much work as possible is done in
 *      the workers.  Cheapest subplans are at the end.
 * ----------------------------------------------------------------
 */
static bool
choose_next_subplan_for_leader(AppendState *node)
{
	ParallelAppendState *pstate = node->as_pstate;

	/* Backward scan is not supported by parallel-aware plans */
	Assert(ScanDirectionIsForward(node->ps.state->es_direction));

	/* We should never be called when there are no subplans */
	Assert(node->as_nplans > 0);

	LWLockAcquire(&pstate->pa_lock, LW_EXCLUSIVE);

	if (node->as_whichplan != INVALID_SUBPLAN_INDEX)
	{
		/* Mark just-completed subplan as finished. */
		node->as_pstate->pa_finished[node->as_whichplan] = true;
	}
	else
	{
		/* Start with last subplan. */
		node->as_whichplan = node->as_nplans - 1;

		/*
		 * If we've yet to determine the valid subplans then do so now.  If
		 * run-time pruning is disabled then the valid subplans will always be
		 * set to all subplans.
		 */
		if (node->as_valid_subplans == NULL)
		{
			node->as_valid_subplans =
				ExecFindMatchingSubPlans(node->as_prune_state);

			/*
			 * Mark each invalid plan as finished to allow the loop below to
			 * select the first valid subplan.
			 */
			mark_invalid_subplans_as_finished(node);
		}
	}

	/* Loop until we find a subplan to execute. */
	while (pstate->pa_finished[node->as_whichplan])
	{
		if (node->as_whichplan == 0)
		{
			pstate->pa_next_plan = INVALID_SUBPLAN_INDEX;
			node->as_whichplan = INVALID_SUBPLAN_INDEX;
			LWLockRelease(&pstate->pa_lock);
			return false;
		}

		/*
		 * We needn't pay attention to as_valid_subplans here as all invalid
		 * plans have been marked as finished.
		 */
		node->as_whichplan--;
	}

	/* If non-partial, immediately mark as finished. */
	if (node->as_whichplan < node->as_first_partial_plan)
		node->as_pstate->pa_finished[node->as_whichplan] = true;

	LWLockRelease(&pstate->pa_lock);

	return true;
}

/* ----------------------------------------------------------------
 *		choose_next_subplan_for_worker
 *
 *		Choose next subplan for a parallel-aware Append, returning
 *		false if there are no more.
 *
 *		We start from the first plan and advance through the list;
 *		when we get back to the end, we loop back to the first
 *		partial plan.  This assigns the non-partial plans first in
 *		order of descending cost and then spreads out the workers
 *		as evenly as possible across the remaining partial plans.
 * ----------------------------------------------------------------
 */
static bool
choose_next_subplan_for_worker(AppendState *node)
{
	ParallelAppendState *pstate = node->as_pstate;

	/* Backward scan is not supported by parallel-aware plans */
	Assert(ScanDirectionIsForward(node->ps.state->es_direction));

	/* We should never be called when there are no subplans */
	Assert(node->as_nplans > 0);

	LWLockAcquire(&pstate->pa_lock, LW_EXCLUSIVE);

	/* Mark just-completed subplan as finished. */
	if (node->as_whichplan != INVALID_SUBPLAN_INDEX)
		node->as_pstate->pa_finished[node->as_whichplan] = true;

	/*
	 * If we've yet to determine the valid subplans then do so now.  If
	 * run-time pruning is disabled then the valid subplans will always be set
	 * to all subplans.
	 */
	else if (node->as_valid_subplans == NULL)
	{
		node->as_valid_subplans =
			ExecFindMatchingSubPlans(node->as_prune_state);
		mark_invalid_subplans_as_finished(node);
	}

	/* If all the plans are already done, we have nothing to do */
	if (pstate->pa_next_plan == INVALID_SUBPLAN_INDEX)
	{
		LWLockRelease(&pstate->pa_lock);
		return false;
	}

	/* Save the plan from which we are starting the search. */
	node->as_whichplan = pstate->pa_next_plan;

	/* Loop until we find a valid subplan to execute. */
	while (pstate->pa_finished[pstate->pa_next_plan])
	{
		int			nextplan;

		nextplan = bms_next_member(node->as_valid_subplans,
								   pstate->pa_next_plan);
		if (nextplan >= 0)
		{
			/* Advance to the next valid plan. */
			pstate->pa_next_plan = nextplan;
		}
		else if (node->as_whichplan > node->as_first_partial_plan)
		{
			/*
			 * Try looping back to the first valid partial plan, if there is
			 * one.  If there isn't, arrange to bail out below.
			 */
			nextplan = bms_next_member(node->as_valid_subplans,
									   node->as_first_partial_plan - 1);
			pstate->pa_next_plan =
				nextplan < 0 ? node->as_whichplan : nextplan;
		}
		else
		{
			/*
			 * At last plan, and either there are no partial plans or we've
			 * tried them all.  Arrange to bail out.
			 */
			pstate->pa_next_plan = node->as_whichplan;
		}

		if (pstate->pa_next_plan == node->as_whichplan)
		{
			/* We've tried everything! */
			pstate->pa_next_plan = INVALID_SUBPLAN_INDEX;
			LWLockRelease(&pstate->pa_lock);
			return false;
		}
	}

	/* Pick the plan we found, and advance pa_next_plan one more time. */
	node->as_whichplan = pstate->pa_next_plan;
	pstate->pa_next_plan = bms_next_member(node->as_valid_subplans,
										   pstate->pa_next_plan);

	/*
	 * If there are no more valid plans then try setting the next plan to the
	 * first valid partial plan.
	 */
	if (pstate->pa_next_plan < 0)
	{
		int			nextplan = bms_next_member(node->as_valid_subplans,
											   node->as_first_partial_plan - 1);

		if (nextplan >= 0)
			pstate->pa_next_plan = nextplan;
		else
		{
			/*
			 * There are no valid partial plans, and we already chose the last
			 * non-partial plan; so flag that there's nothing more for our
			 * fellow workers to do.
			 */
			pstate->pa_next_plan = INVALID_SUBPLAN_INDEX;
		}
	}

	/* If non-partial, immediately mark as finished. */
	if (node->as_whichplan < node->as_first_partial_plan)
		node->as_pstate->pa_finished[node->as_whichplan] = true;

	LWLockRelease(&pstate->pa_lock);

	return true;
}

/*
 * mark_invalid_subplans_as_finished
 *		Marks the ParallelAppendState's pa_finished as true for each invalid
 *		subplan.
 *
 * This function should only be called for parallel Append with run-time
 * pruning enabled.
 */
static void
mark_invalid_subplans_as_finished(AppendState *node)
{
	int			i;

	/* Only valid to call this while in parallel Append mode */
	Assert(node->as_pstate);

	/* Shouldn't have been called when run-time pruning is not enabled */
	Assert(node->as_prune_state);

	/* Nothing to do if all plans are valid */
	if (bms_num_members(node->as_valid_subplans) == node->as_nplans)
		return;

	/* Mark all non-valid plans as finished */
	for (i = 0; i < node->as_nplans; i++)
	{
		if (!bms_is_member(i, node->as_valid_subplans))
			node->as_pstate->pa_finished[i] = true;
	}
}
