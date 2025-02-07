/*-------------------------------------------------------------------------
 *
 * nodeMergeAppend.c
 *	  routines to handle MergeAppend nodes.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeMergeAppend.c
 *
 *-------------------------------------------------------------------------
 */
/* INTERFACE ROUTINES
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

#include "executor/executor.h"
#include "executor/execPartition.h"
#include "executor/nodeMergeAppend.h"
#include "lib/binaryheap.h"
#include "miscadmin.h"

/*
 * We have one slot for each item in the heap array.  We use SlotNumber
 * to store slot indexes.  This doesn't actually provide any formal
 * type-safety, but it makes the code more self-documenting.
 */
typedef int32 SlotNumber;

static TupleTableSlot *ExecMergeAppend(PlanState *pstate);
static int	heap_compare_slots(Datum a, Datum b, void *arg);


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

	mergeplanstates = (PlanState **) palloc(nplans * sizeof(PlanState *));
	mergestate->mergeplans = mergeplanstates;
	mergestate->ms_nplans = nplans;

	mergestate->ms_slots = (TupleTableSlot **) palloc0(sizeof(TupleTableSlot *) * nplans);
	mergestate->ms_heap = binaryheap_allocate(nplans, heap_compare_slots,
											  mergestate);

	/*
	 * call ExecInitNode on each of the valid plans to be executed and save
	 * the results into the mergeplanstates array.
	 */
	j = 0;
	i = -1;
	while ((i = bms_next_member(validsubplans, i)) >= 0)
	{
		Plan	   *initNode = (Plan *) list_nth(node->mergeplans, i);

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

	/*
	 * Miscellaneous initialization
	 */
	mergestate->ps.ps_ProjInfo = NULL;

	/*
	 * initialize sort-key information
	 */
	mergestate->ms_nkeys = node->numCols;
	mergestate->ms_sortkeys = palloc0(sizeof(SortSupportData) * node->numCols);

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
		 */
		i = -1;
		while ((i = bms_next_member(node->ms_valid_subplans, i)) >= 0)
		{
			node->ms_slots[i] = ExecProcNode(node->mergeplans[i]);
			if (!TupIsNull(node->ms_slots[i]))
				binaryheap_add_unordered(node->ms_heap, Int32GetDatum(i));
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
		 */
		i = DatumGetInt32(binaryheap_first(node->ms_heap));
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
		 * first ExecProcNode.
		 */
		if (subnode->chgParam == NULL)
			ExecReScan(subnode);
	}
	binaryheap_reset(node->ms_heap);
	node->ms_initialized = false;
}
