/*-------------------------------------------------------------------------
 *
 * nodeMergeAppend.c
 *	  routines to handle MergeAppend nodes.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeMergeAppend.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecInitMergeAppend		- initialize the MergeAppend node
 *		ExecMergeAppend			- retrieve the next tuple from the node
 *		ExecEndMergeAppend		- shut down the MergeAppend node
 *		ExecReScanMergeAppend	- rescan the MergeAppend node
 *
 *	 NOTES
 *		A MergeAppend node contains a list of one or more subplans.
 *		These are each expected to deliver tuples that are sorted according
 *		to a common sort key.  The MergeAppend node merges these streams
 *		to produce output sorted the same way.
 *
 *		MergeAppend nodes don't make use of their left and right
 *		subtrees, rather they maintain a list of subplans so
 *		a typical MergeAppend node looks like this in the plan tree:
 *
 *				   ...
 *				   /
 *				MergeAppend---+------+------+--- nil
 *				/	\		  |		 |		|
 *			  nil	nil		 ...    ...    ...
 *								 subplans
 */

#include "postgres.h"

#include "executor/execAsync.h"
#include "executor/executor.h"
#include "executor/execPartition.h"
#include "executor/nodeMergeAppend.h"
#include "lib/binaryheap.h"
#include "miscadmin.h"
#include "utils/sortsupport.h"
#include "utils/wait_event.h"

/*
 * We have one slot for each item in the heap array.  We use SlotNumber
 * to store slot indexes.  This doesn't actually provide any formal
 * type-safety, but it makes the code more self-documenting.
 */
typedef int32 SlotNumber;

static TupleTableSlot *ExecMergeAppend(PlanState *pstate);
static int	heap_compare_slots(Datum a, Datum b, void *arg);
static void ExecMergeAppendAsyncBegin(MergeAppendState *node,
									  Bitmapset *valid_asyncplans);
static void ExecMergeAppendAsyncRequest(MergeAppendState *node, SlotNumber i);
static void ExecMergeAppendAsyncDrain(MergeAppendState *node);
static void ExecMergeAppendAsyncEventWait(MergeAppendState *node);
static void ExecMergeAppendAsyncReset(MergeAppendState *node);


/* ----------------------------------------------------------------
 *		ExecInitMergeAppend
 *
 *		Begin all of the subscans of the MergeAppend node.
 * ----------------------------------------------------------------
 */
MergeAppendState *
ExecInitMergeAppend(MergeAppend *node, EState *estate, int eflags)
{
	MergeAppendState *mergestate = makeNode(MergeAppendState);
	PlanState **mergeplanstates;
	const TupleTableSlotOps *mergeops;
	Bitmapset  *validsubplans;
	int			nplans;
	int			i,
				j;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * create new MergeAppendState for our node
	 */
	mergestate->ps.plan = (Plan *) node;
	mergestate->ps.state = estate;
	mergestate->ps.ExecProcNode = ExecMergeAppend;

	/* If run-time partition pruning is enabled, then set that up now */
	if (node->part_prune_index >= 0)
	{
		PartitionPruneState *prunestate;

		/*
		 * Set up pruning data structure.  This also initializes the set of
		 * subplans to initialize (validsubplans) by taking into account the
		 * result of performing initial pruning if any.
		 */
		prunestate = ExecInitPartitionExecPruning(&mergestate->ps,
												  list_length(node->mergeplans),
												  node->part_prune_index,
												  node->apprelids,
												  &validsubplans);
		mergestate->ms_prune_state = prunestate;
		nplans = bms_num_members(validsubplans);

		/*
		 * When no run-time pruning is required and there's at least one
		 * subplan, we can fill ms_valid_subplans immediately, preventing
		 * later calls to ExecFindMatchingSubPlans.
		 */
		if (!prunestate->do_exec_prune && nplans > 0)
			mergestate->ms_valid_subplans = bms_add_range(NULL, 0, nplans - 1);
	}
	else
	{
		nplans = list_length(node->mergeplans);

		/*
		 * When run-time partition pruning is not enabled we can just mark all
		 * subplans as valid; they must also all be initialized.
		 */
		Assert(nplans > 0);
		mergestate->ms_valid_subplans = validsubplans =
			bms_add_range(NULL, 0, nplans - 1);
		mergestate->ms_prune_state = NULL;
	}

	mergeplanstates = palloc_array(PlanState *, nplans);
	mergestate->mergeplans = mergeplanstates;
	mergestate->ms_nplans = nplans;

	mergestate->ms_slots = palloc0_array(TupleTableSlot *, nplans);
	mergestate->ms_heap = binaryheap_allocate(nplans, heap_compare_slots,
											  mergestate);

	/*
	 * call ExecInitNode on each of the valid plans to be executed and save
	 * the results into the mergeplanstates array.
	 *
	 * While at it, find out which subplans can be executed asynchronously.
	 */
	j = 0;
	i = -1;
	mergestate->ms_asyncplans = NULL;
	mergestate->ms_nasyncplans = 0;
	while ((i = bms_next_member(validsubplans, i)) >= 0)
	{
		Plan	   *initNode = (Plan *) list_nth(node->mergeplans, i);

		/*
		 * Record async subplans.  When executing EvalPlanQual, we treat them
		 * as sync ones; don't do this when initializing an EvalPlanQual plan
		 * tree.
		 */
		if (initNode->async_capable && estate->es_epq_active == NULL)
		{
			mergestate->ms_asyncplans =
				bms_add_member(mergestate->ms_asyncplans, j);
			mergestate->ms_nasyncplans++;
		}

		mergeplanstates[j++] = ExecInitNode(initNode, estate, eflags);
	}

	/*
	 * Initialize MergeAppend's result tuple type and slot.  If the child
	 * plans all produce the same fixed slot type, we can use that slot type;
	 * otherwise make a virtual slot.  (Note that the result slot itself is
	 * used only to return a null tuple at end of execution; real tuples are
	 * returned to the caller in the children's own result slots.  What we are
	 * doing here is allowing the parent plan node to optimize if the
	 * MergeAppend will return only one kind of slot.)
	 */
	mergeops = ExecGetCommonSlotOps(mergeplanstates, j);
	if (mergeops != NULL)
	{
		ExecInitResultTupleSlotTL(&mergestate->ps, mergeops);
	}
	else
	{
		ExecInitResultTupleSlotTL(&mergestate->ps, &TTSOpsVirtual);
		/* show that the output slot type is not fixed */
		mergestate->ps.resultopsset = true;
		mergestate->ps.resultopsfixed = false;
	}

	/* Initialize async state */
	mergestate->ms_asyncrequests = NULL;
	mergestate->ms_asyncpending = NULL;
	mergestate->ms_eventset = NULL;

	if (mergestate->ms_nasyncplans > 0)
	{
		mergestate->ms_asyncrequests = palloc0_array(AsyncRequest *, nplans);

		i = -1;
		while ((i = bms_next_member(mergestate->ms_asyncplans, i)) >= 0)
		{
			AsyncRequest *areq;

			areq = palloc_object(AsyncRequest);
			areq->requestor = (PlanState *) mergestate;
			areq->requestee = mergeplanstates[i];
			areq->request_index = i;
			areq->callback_pending = false;
			areq->request_complete = false;
			areq->result = NULL;

			mergestate->ms_asyncrequests[i] = areq;
		}
	}

	/*
	 * Miscellaneous initialization
	 */
	mergestate->ps.ps_ProjInfo = NULL;

	/*
	 * initialize sort-key information
	 */
	mergestate->ms_nkeys = node->numCols;
	mergestate->ms_sortkeys = palloc0_array(SortSupportData, node->numCols);

	for (i = 0; i < node->numCols; i++)
	{
		SortSupport sortKey = mergestate->ms_sortkeys + i;

		sortKey->ssup_cxt = CurrentMemoryContext;
		sortKey->ssup_collation = node->collations[i];
		sortKey->ssup_nulls_first = node->nullsFirst[i];
		sortKey->ssup_attno = node->sortColIdx[i];

		/*
		 * It isn't feasible to perform abbreviated key conversion, since
		 * tuples are pulled into mergestate's binary heap as needed.  It
		 * would likely be counter-productive to convert tuples into an
		 * abbreviated representation as they're pulled up, so opt out of that
		 * additional optimization entirely.
		 */
		sortKey->abbreviate = false;

		PrepareSortSupportFromOrderingOp(node->sortOperators[i], sortKey);
	}

	/*
	 * initialize to show we have not run the subplans yet
	 */
	mergestate->ms_initialized = false;

	return mergestate;
}

/* ----------------------------------------------------------------
 *	   ExecMergeAppend
 *
 *		Handles iteration over multiple subplans.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecMergeAppend(PlanState *pstate)
{
	MergeAppendState *node = castNode(MergeAppendState, pstate);
	TupleTableSlot *result;
	SlotNumber	i;

	CHECK_FOR_INTERRUPTS();

	if (!node->ms_initialized)
	{
		/* Nothing to do if all subplans were pruned */
		if (node->ms_nplans == 0)
			return ExecClearTuple(node->ps.ps_ResultTupleSlot);

		/*
		 * If we've yet to determine the valid subplans then do so now.  If
		 * run-time pruning is disabled then the valid subplans will always be
		 * set to all subplans.
		 */
		if (node->ms_valid_subplans == NULL)
			node->ms_valid_subplans =
				ExecFindMatchingSubPlans(node->ms_prune_state, false, NULL);

		/*
		 * First time through: pull the first tuple from each valid subplan,
		 * and set up the heap.
		 *
		 * When there are async-capable subplans, fire off all their requests
		 * up front so that they can run concurrently (for example, on
		 * different foreign servers).  We then pull the first tuple from each
		 * synchronous subplan while the asynchronous ones are still in
		 * flight, and finally collect the asynchronous results.
		 */
		if (node->ms_nasyncplans > 0)
		{
			Bitmapset  *valid_asyncplans;

			/* Determine the valid async subplans and begin executing them. */
			valid_asyncplans = bms_intersect(node->ms_valid_subplans,
											 node->ms_asyncplans);
			ExecMergeAppendAsyncBegin(node, valid_asyncplans);

			/* Pull the first tuple from each valid synchronous subplan. */
			i = -1;
			while ((i = bms_next_member(node->ms_valid_subplans, i)) >= 0)
			{
				if (bms_is_member(i, node->ms_asyncplans))
					continue;

				node->ms_slots[i] = ExecProcNode(node->mergeplans[i]);
				if (!TupIsNull(node->ms_slots[i]))
					binaryheap_add_unordered(node->ms_heap, Int32GetDatum(i));
			}

			/* Now wait for the asynchronous subplans to deliver their tuples. */
			ExecMergeAppendAsyncDrain(node);
			i = -1;
			while ((i = bms_next_member(valid_asyncplans, i)) >= 0)
			{
				if (!TupIsNull(node->ms_slots[i]))
					binaryheap_add_unordered(node->ms_heap, Int32GetDatum(i));
			}

			bms_free(valid_asyncplans);
		}
		else
		{
			i = -1;
			while ((i = bms_next_member(node->ms_valid_subplans, i)) >= 0)
			{
				node->ms_slots[i] = ExecProcNode(node->mergeplans[i]);
				if (!TupIsNull(node->ms_slots[i]))
					binaryheap_add_unordered(node->ms_heap, Int32GetDatum(i));
			}
		}

		binaryheap_build(node->ms_heap);
		node->ms_initialized = true;
	}
	else
	{
		/*
		 * Otherwise, pull the next tuple from whichever subplan we returned
		 * from last time, and reinsert the subplan index into the heap,
		 * because it might now compare differently against the existing
		 * elements of the heap.  (We could perhaps simplify the logic a bit
		 * by doing this before returning from the prior call, but it's better
		 * to not pull tuples until necessary.)
		 *
		 * If the subplan is async-capable, we fire a fresh request for it and
		 * wait for the result.  Unlike a plain Append, the merge cannot make
		 * progress until this particular subplan's next tuple is known (it
		 * might be the new heap minimum), so there is at most one outstanding
		 * async request during this phase.
		 */
		i = DatumGetInt32(binaryheap_first(node->ms_heap));

		if (bms_is_member(i, node->ms_asyncplans))
		{
			ExecMergeAppendAsyncRequest(node, i);
			ExecMergeAppendAsyncDrain(node);
		}
		else
			node->ms_slots[i] = ExecProcNode(node->mergeplans[i]);

		if (!TupIsNull(node->ms_slots[i]))
			binaryheap_replace_first(node->ms_heap, Int32GetDatum(i));
		else
			(void) binaryheap_remove_first(node->ms_heap);
	}

	if (binaryheap_empty(node->ms_heap))
	{
		/* All the subplans are exhausted, and so is the heap */
		result = ExecClearTuple(node->ps.ps_ResultTupleSlot);
	}
	else
	{
		i = DatumGetInt32(binaryheap_first(node->ms_heap));
		result = node->ms_slots[i];
	}

	return result;
}

/*
 * Compare the tuples in the two given slots.
 */
static int32
heap_compare_slots(Datum a, Datum b, void *arg)
{
	MergeAppendState *node = (MergeAppendState *) arg;
	SlotNumber	slot1 = DatumGetInt32(a);
	SlotNumber	slot2 = DatumGetInt32(b);

	TupleTableSlot *s1 = node->ms_slots[slot1];
	TupleTableSlot *s2 = node->ms_slots[slot2];
	int			nkey;

	Assert(!TupIsNull(s1));
	Assert(!TupIsNull(s2));

	for (nkey = 0; nkey < node->ms_nkeys; nkey++)
	{
		SortSupport sortKey = node->ms_sortkeys + nkey;
		AttrNumber	attno = sortKey->ssup_attno;
		Datum		datum1,
					datum2;
		bool		isNull1,
					isNull2;
		int			compare;

		datum1 = slot_getattr(s1, attno, &isNull1);
		datum2 = slot_getattr(s2, attno, &isNull2);

		compare = ApplySortComparator(datum1, isNull1,
									  datum2, isNull2,
									  sortKey);
		if (compare != 0)
		{
			INVERT_COMPARE_RESULT(compare);
			return compare;
		}
	}
	return 0;
}

/* ----------------------------------------------------------------
 *		ExecEndMergeAppend
 *
 *		Shuts down the subscans of the MergeAppend node.
 *
 *		Returns nothing of interest.
 * ----------------------------------------------------------------
 */
void
ExecEndMergeAppend(MergeAppendState *node)
{
	PlanState **mergeplans;
	int			nplans;
	int			i;

	/*
	 * get information from the node
	 */
	mergeplans = node->mergeplans;
	nplans = node->ms_nplans;

	/*
	 * shut down each of the subscans
	 */
	for (i = 0; i < nplans; i++)
		ExecEndNode(mergeplans[i]);
}

void
ExecReScanMergeAppend(MergeAppendState *node)
{
	int			i;

	/* If there are any async subplans, reset async requests made for them. */
	if (node->ms_nasyncplans > 0)
		ExecMergeAppendAsyncReset(node);

	/*
	 * If any PARAM_EXEC Params used in pruning expressions have changed, then
	 * we'd better unset the valid subplans so that they are reselected for
	 * the new parameter values.
	 */
	if (node->ms_prune_state &&
		bms_overlap(node->ps.chgParam,
					node->ms_prune_state->execparamids))
	{
		bms_free(node->ms_valid_subplans);
		node->ms_valid_subplans = NULL;
	}

	for (i = 0; i < node->ms_nplans; i++)
	{
		PlanState  *subnode = node->mergeplans[i];

		/*
		 * ExecReScan doesn't know about my subplans, so I have to do
		 * changed-parameter signaling myself.
		 */
		if (node->ps.chgParam != NULL)
			UpdateChangedParamSet(subnode, node->ps.chgParam);

		/*
		 * If chgParam of subnode is not null then plan will be re-scanned by
		 * first ExecProcNode or by first ExecAsyncRequest.
		 */
		if (subnode->chgParam == NULL)
			ExecReScan(subnode);
	}

	binaryheap_reset(node->ms_heap);
	node->ms_initialized = false;
}

/* ----------------------------------------------------------------
 *						Asynchronous MergeAppend Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecMergeAppendAsyncBegin
 *
 *		Begin executing the designated valid async-capable subplans.
 * ----------------------------------------------------------------
 */
static void
ExecMergeAppendAsyncBegin(MergeAppendState *node, Bitmapset *valid_asyncplans)
{
	int			i;

	/* Backward scan is not supported by async-aware MergeAppends. */
	Assert(ScanDirectionIsForward(node->ps.state->es_direction));

	/* We should never be called when there are no async subplans. */
	Assert(node->ms_nasyncplans > 0);

	/* Make a request for each of the valid async subplans. */
	i = -1;
	while ((i = bms_next_member(valid_asyncplans, i)) >= 0)
		ExecMergeAppendAsyncRequest(node, i);
}

/* ----------------------------------------------------------------
 *		ExecMergeAppendAsyncRequest
 *
 *		Request the next tuple from a single async-capable subplan.
 *
 *		The subplan is marked as having an outstanding request.  If
 *		the request happens to complete synchronously,
 *		ExecAsyncMergeAppendResponse clears that marker (and stores
 *		the result slot) before this returns.
 * ----------------------------------------------------------------
 */
static void
ExecMergeAppendAsyncRequest(MergeAppendState *node, SlotNumber i)
{
	AsyncRequest *areq = node->ms_asyncrequests[i];

	Assert(areq != NULL);
	Assert(areq->request_index == i);
	Assert(!areq->callback_pending);
	Assert(!bms_is_member(i, node->ms_asyncpending));

	node->ms_asyncpending = bms_add_member(node->ms_asyncpending, i);

	/* Do the actual work. */
	ExecAsyncRequest(areq);
}

/* ----------------------------------------------------------------
 *		ExecMergeAppendAsyncDrain
 *
 *		Wait until every outstanding async request has delivered its
 *		tuple (or reached end-of-scan) into the corresponding
 *		ms_slots entry.
 * ----------------------------------------------------------------
 */
static void
ExecMergeAppendAsyncDrain(MergeAppendState *node)
{
	while (!bms_is_empty(node->ms_asyncpending))
	{
		CHECK_FOR_INTERRUPTS();

		ExecMergeAppendAsyncEventWait(node);
	}
}

/* ----------------------------------------------------------------
 *		ExecMergeAppendAsyncEventWait
 *
 *		Wait for file descriptor events and fire callbacks.
 *
 *		Unlike an async Append, a MergeAppend cannot make progress
 *		until the outstanding tuples arrive, so we always wait
 *		(indefinitely) for at least one event.
 * ----------------------------------------------------------------
 */
static void
ExecMergeAppendAsyncEventWait(MergeAppendState *node)
{
	int			nevents = node->ms_nasyncplans + 2;

	/* We should never be called when there are no outstanding requests. */
	Assert(!bms_is_empty(node->ms_asyncpending));

	Assert(node->ms_eventset == NULL);
	node->ms_eventset = CreateWaitEventSet(CurrentResourceOwner, nevents);
	ExecAsyncEventWait(node->ms_asyncplans,
					   node->ms_asyncrequests,
					   node->ms_eventset,
					   nevents,
					   -1,
					   WAIT_EVENT_MERGE_APPEND_READY);
	FreeWaitEventSet(node->ms_eventset);
	node->ms_eventset = NULL;
}

/* ----------------------------------------------------------------
 *		ExecMergeAppendAsyncReset
 *
 *		Reset asynchronous requests made for async-capable subplans.
 * ----------------------------------------------------------------
 */
static void
ExecMergeAppendAsyncReset(MergeAppendState *node)
{
	int			i;

	/* We should never be called when there are no async subplans. */
	Assert(node->ms_nasyncplans > 0);

	/* Reset async requests. */
	i = -1;
	while ((i = bms_next_member(node->ms_asyncplans, i)) >= 0)
	{
		AsyncRequest *areq = node->ms_asyncrequests[i];

		/*
		 * Unlike in ExecAppendAsyncReset, the request wouldn't have been
		 * pending anymore at ReScan.
		 */
		Assert(!areq->callback_pending);
		areq->request_complete = false;
		areq->result = NULL;
	}

	/* Reset state variables. */
	bms_free(node->ms_asyncpending);
	node->ms_asyncpending = NULL;
}

/* ----------------------------------------------------------------
 *		ExecAsyncMergeAppendResponse
 *
 *		Receive a response from an asynchronous request we made.
 * ----------------------------------------------------------------
 */
void
ExecAsyncMergeAppendResponse(AsyncRequest *areq)
{
	MergeAppendState *node = (MergeAppendState *) areq->requestor;
	TupleTableSlot *slot = areq->result;
	SlotNumber	i = areq->request_index;

	/* The result should be a TupleTableSlot or NULL. */
	Assert(slot == NULL || IsA(slot, TupleTableSlot));

	/* Nothing to do if the request is still pending. */
	if (!areq->request_complete)
	{
		/* The request would have been pending for a callback. */
		Assert(areq->callback_pending);
		return;
	}

	/*
	 * Store the delivered result (a tuple, or an empty/NULL slot at
	 * end-of-scan) as this subplan's current slot, and mark the request as no
	 * longer outstanding.
	 *
	 * Note that the binary heap is manipulated by ExecMergeAppend, not here,
	 * because the required operation differs between the initial heap build
	 * and later single-subplan refills.
	 */
	Assert(bms_is_member(i, node->ms_asyncpending));
	node->ms_slots[i] = slot;
	node->ms_asyncpending = bms_del_member(node->ms_asyncpending, i);
}
