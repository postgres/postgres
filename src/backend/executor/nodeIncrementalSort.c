/*-------------------------------------------------------------------------
 *
 * nodeIncrementalSort.c
 *	  Routines to handle incremental sorting of relations.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeIncrementalSort.c
 *
 * DESCRIPTION
 *
 *	Incremental sort is an optimized variant of multikey sort for cases
 *	when the input is already sorted by a prefix of the sort keys.  For
 *	example when a sort by (key1, key2 ... keyN) is requested, and the
 *	input is already sorted by (key1, key2 ... keyM), M < N, we can
 *	divide the input into groups where keys (key1, ... keyM) are equal,
 *	and only sort on the remaining columns.
 *
 *	Consider the following example.  We have input tuples consisting of
 *	two integers (X, Y) already presorted by X, while it's required to
 *	sort them by both X and Y.  Let input tuples be following.
 *
 *	(1, 5)
 *	(1, 2)
 *	(2, 9)
 *	(2, 1)
 *	(2, 5)
 *	(3, 3)
 *	(3, 7)
 *
 *	An incremental sort algorithm would split the input into the following
 *	groups, which have equal X, and then sort them by Y individually:
 *
 *		(1, 5) (1, 2)
 *		(2, 9) (2, 1) (2, 5)
 *		(3, 3) (3, 7)
 *
 *	After sorting these groups and putting them altogether, we would get
 *	the following result which is sorted by X and Y, as requested:
 *
 *	(1, 2)
 *	(1, 5)
 *	(2, 1)
 *	(2, 5)
 *	(2, 9)
 *	(3, 3)
 *	(3, 7)
 *
 *	Incremental sort may be more efficient than plain sort, particularly
 *	on large datasets, as it reduces the amount of data to sort at once,
 *	making it more likely it fits into work_mem (eliminating the need to
 *	spill to disk).  But the main advantage of incremental sort is that
 *	it can start producing rows early, before sorting the whole dataset,
 *	which is a significant benefit especially for queries with LIMIT.
 *
 *	The algorithm we've implemented here is modified from the theoretical
 *	base described above by operating in two different modes:
 *	  - Fetching a minimum number of tuples without checking prefix key
 *	    group membership and sorting on all columns when safe.
 *	  - Fetching all tuples for a single prefix key group and sorting on
 *	    solely the unsorted columns.
 *	We always begin in the first mode, and employ a heuristic to switch
 *	into the second mode if we believe it's beneficial.
 *
 *	Sorting incrementally can potentially use less memory, avoid fetching
 *	and sorting all tuples in the dataset, and begin returning tuples before
 *	the entire result set is available.
 *
 *	The hybrid mode approach allows us to optimize for both very small
 *	groups (where the overhead of a new tuplesort is high) and very large
 *	groups (where we can lower cost by not having to sort on already sorted
 *	columns), albeit at some extra cost while switching between modes.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/nodeIncrementalSort.h"
#include "miscadmin.h"
#include "utils/lsyscache.h"
#include "utils/tuplesort.h"

/*
 * We need to store the instrumentation information in either local node's sort
 * info or, for a parallel worker process, in the shared info (this avoids
 * having to additionally memcpy the info from local memory to shared memory
 * at each instrumentation call). This macro expands to choose the proper sort
 * state and group info.
 *
 * Arguments:
 * - node: type IncrementalSortState *
 * - groupName: the token fullsort or prefixsort
 */
#define INSTRUMENT_SORT_GROUP(node, groupName) \
	do { \
		if ((node)->ss.ps.instrument != NULL) \
		{ \
			if ((node)->shared_info && (node)->am_worker) \
			{ \
				Assert(IsParallelWorker()); \
				Assert(ParallelWorkerNumber <= (node)->shared_info->num_workers); \
				instrumentSortedGroup(&(node)->shared_info->sinfo[ParallelWorkerNumber].groupName##GroupInfo, \
									  (node)->groupName##_state); \
			} \
			else \
			{ \
				instrumentSortedGroup(&(node)->incsort_info.groupName##GroupInfo, \
									  (node)->groupName##_state); \
			} \
		} \
	} while (0)


/* ----------------------------------------------------------------
 * instrumentSortedGroup
 *
 * Because incremental sort processes (potentially many) sort batches, we need
 * to capture tuplesort stats each time we finalize a sort state. This summary
 * data is later used for EXPLAIN ANALYZE output.
 * ----------------------------------------------------------------
 */
static void
instrumentSortedGroup(IncrementalSortGroupInfo *groupInfo,
					  Tuplesortstate *sortState)
{
	TuplesortInstrumentation sort_instr;

	groupInfo->groupCount++;

	tuplesort_get_stats(sortState, &sort_instr);

	/* Calculate total and maximum memory and disk space used. */
	switch (sort_instr.spaceType)
	{
		case SORT_SPACE_TYPE_DISK:
			groupInfo->totalDiskSpaceUsed += sort_instr.spaceUsed;
			if (sort_instr.spaceUsed > groupInfo->maxDiskSpaceUsed)
				groupInfo->maxDiskSpaceUsed = sort_instr.spaceUsed;

			break;
		case SORT_SPACE_TYPE_MEMORY:
			groupInfo->totalMemorySpaceUsed += sort_instr.spaceUsed;
			if (sort_instr.spaceUsed > groupInfo->maxMemorySpaceUsed)
				groupInfo->maxMemorySpaceUsed = sort_instr.spaceUsed;

			break;
	}

	/* Track each sort method we've used. */
	groupInfo->sortMethods |= sort_instr.sortMethod;
}

/* ----------------------------------------------------------------
 * preparePresortedCols
 *
 * Prepare information for presorted_keys comparisons.
 * ----------------------------------------------------------------
 */
static void
preparePresortedCols(IncrementalSortState *node)
{
	IncrementalSort *plannode = castNode(IncrementalSort, node->ss.ps.plan);

	node->presorted_keys =
		(PresortedKeyData *) palloc(plannode->nPresortedCols *
									sizeof(PresortedKeyData));

	/* Pre-cache comparison functions for each pre-sorted key. */
	for (int i = 0; i < plannode->nPresortedCols; i++)
	{
		Oid			equalityOp,
					equalityFunc;
		PresortedKeyData *key;

		key = &node->presorted_keys[i];
		key->attno = plannode->sort.sortColIdx[i];

		equalityOp = get_equality_op_for_ordering_op(plannode->sort.sortOperators[i],
													 NULL);
		if (!OidIsValid(equalityOp))
			elog(ERROR, "missing equality operator for ordering operator %u",
				 plannode->sort.sortOperators[i]);

		equalityFunc = get_opcode(equalityOp);
		if (!OidIsValid(equalityFunc))
			elog(ERROR, "missing function for operator %u", equalityOp);

		/* Lookup the comparison function */
		fmgr_info_cxt(equalityFunc, &key->flinfo, CurrentMemoryContext);

		/* We can initialize the callinfo just once and re-use it */
		key->fcinfo = palloc0(SizeForFunctionCallInfo(2));
		InitFunctionCallInfoData(*key->fcinfo, &key->flinfo, 2,
								 plannode->sort.collations[i], NULL, NULL);
		key->fcinfo->args[0].isnull = false;
		key->fcinfo->args[1].isnull = false;
	}
}

/* ----------------------------------------------------------------
 * isCurrentGroup
 *
 * Check whether a given tuple belongs to the current sort group by comparing
 * the presorted column values to the pivot tuple of the current group.
 * ----------------------------------------------------------------
 */
static bool
isCurrentGroup(IncrementalSortState *node, TupleTableSlot *pivot, TupleTableSlot *tuple)
{
	int			nPresortedCols;

	nPresortedCols = castNode(IncrementalSort, node->ss.ps.plan)->nPresortedCols;

	/*
	 * That the input is sorted by keys * (0, ... n) implies that the tail
	 * keys are more likely to change. Therefore we do our comparison starting
	 * from the last pre-sorted column to optimize for early detection of
	 * inequality and minimizing the number of function calls..
	 */
	for (int i = nPresortedCols - 1; i >= 0; i--)
	{
		Datum		datumA,
					datumB,
					result;
		bool		isnullA,
					isnullB;
		AttrNumber	attno = node->presorted_keys[i].attno;
		PresortedKeyData *key;

		datumA = slot_getattr(pivot, attno, &isnullA);
		datumB = slot_getattr(tuple, attno, &isnullB);

		/* Special case for NULL-vs-NULL, else use standard comparison */
		if (isnullA || isnullB)
		{
			if (isnullA == isnullB)
				continue;
			else
				return false;
		}

		key = &node->presorted_keys[i];

		key->fcinfo->args[0].value = datumA;
		key->fcinfo->args[1].value = datumB;

		/* just for paranoia's sake, we reset isnull each time */
		key->fcinfo->isnull = false;

		result = FunctionCallInvoke(key->fcinfo);

		/* Check for null result, since caller is clearly not expecting one */
		if (key->fcinfo->isnull)
			elog(ERROR, "function %u returned NULL", key->flinfo.fn_oid);

		if (!DatumGetBool(result))
			return false;
	}
	return true;
}

/* ----------------------------------------------------------------
 * switchToPresortedPrefixMode
 *
 * When we determine that we've likely encountered a large batch of tuples all
 * having the same presorted prefix values, we want to optimize tuplesort by
 * only sorting on unsorted suffix keys.
 *
 * The problem is that we've already accumulated several tuples in another
 * tuplesort configured to sort by all columns (assuming that there may be
 * more than one prefix key group). So to switch to presorted prefix mode we
 * have to go back and look at all the tuples we've already accumulated to
 * verify they're all part of the same prefix key group before sorting them
 * solely by unsorted suffix keys.
 *
 * While it's likely that all tuples already fetched are all part of a single
 * prefix group, we also have to handle the possibility that there is at least
 * one different prefix key group before the large prefix key group.
 * ----------------------------------------------------------------
 */
static void
switchToPresortedPrefixMode(PlanState *pstate)
{
	IncrementalSortState *node = castNode(IncrementalSortState, pstate);
	ScanDirection dir;
	int64		nTuples;
	TupleDesc	tupDesc;
	PlanState  *outerNode;
	IncrementalSort *plannode = castNode(IncrementalSort, node->ss.ps.plan);

	dir = node->ss.ps.state->es_direction;
	outerNode = outerPlanState(node);
	tupDesc = ExecGetResultType(outerNode);

	/* Configure the prefix sort state the first time around. */
	if (node->prefixsort_state == NULL)
	{
		Tuplesortstate *prefixsort_state;
		int			nPresortedCols = plannode->nPresortedCols;

		/*
		 * Optimize the sort by assuming the prefix columns are all equal and
		 * thus we only need to sort by any remaining columns.
		 */
		prefixsort_state = tuplesort_begin_heap(tupDesc,
												plannode->sort.numCols - nPresortedCols,
												&(plannode->sort.sortColIdx[nPresortedCols]),
												&(plannode->sort.sortOperators[nPresortedCols]),
												&(plannode->sort.collations[nPresortedCols]),
												&(plannode->sort.nullsFirst[nPresortedCols]),
												work_mem,
												NULL,
												node->bounded ? TUPLESORT_ALLOWBOUNDED : TUPLESORT_NONE);
		node->prefixsort_state = prefixsort_state;
	}
	else
	{
		/* Next group of presorted data */
		tuplesort_reset(node->prefixsort_state);
	}

	/*
	 * If the current node has a bound, then it's reasonably likely that a
	 * large prefix key group will benefit from bounded sort, so configure the
	 * tuplesort to allow for that optimization.
	 */
	if (node->bounded)
	{
		SO1_printf("Setting bound on presorted prefix tuplesort to: " INT64_FORMAT "\n",
				   node->bound - node->bound_Done);
		tuplesort_set_bound(node->prefixsort_state,
							node->bound - node->bound_Done);
	}

	/*
	 * Copy as many tuples as we can (i.e., in the same prefix key group) from
	 * the full sort state to the prefix sort state.
	 */
	for (nTuples = 0; nTuples < node->n_fullsort_remaining; nTuples++)
	{
		/*
		 * When we encounter multiple prefix key groups inside the full sort
		 * tuplesort we have to carry over the last read tuple into the next
		 * batch.
		 */
		if (nTuples == 0 && !TupIsNull(node->transfer_tuple))
		{
			tuplesort_puttupleslot(node->prefixsort_state, node->transfer_tuple);
			/* The carried over tuple is our new group pivot tuple. */
			ExecCopySlot(node->group_pivot, node->transfer_tuple);
		}
		else
		{
			tuplesort_gettupleslot(node->fullsort_state,
								   ScanDirectionIsForward(dir),
								   false, node->transfer_tuple, NULL);

			/*
			 * If this is our first time through the loop, then we need to
			 * save the first tuple we get as our new group pivot.
			 */
			if (TupIsNull(node->group_pivot))
				ExecCopySlot(node->group_pivot, node->transfer_tuple);

			if (isCurrentGroup(node, node->group_pivot, node->transfer_tuple))
			{
				tuplesort_puttupleslot(node->prefixsort_state, node->transfer_tuple);
			}
			else
			{
				/*
				 * The tuple isn't part of the current batch so we need to
				 * carry it over into the next batch of tuples we transfer out
				 * of the full sort tuplesort into the presorted prefix
				 * tuplesort. We don't actually have to do anything special to
				 * save the tuple since we've already loaded it into the
				 * node->transfer_tuple slot, and, even though that slot
				 * points to memory inside the full sort tuplesort, we can't
				 * reset that tuplesort anyway until we've fully transferred
				 * out its tuples, so this reference is safe. We do need to
				 * reset the group pivot tuple though since we've finished the
				 * current prefix key group.
				 */
				ExecClearTuple(node->group_pivot);

				/* Break out of for-loop early */
				break;
			}
		}
	}

	/*
	 * Track how many tuples remain in the full sort batch so that we know if
	 * we need to sort multiple prefix key groups before processing tuples
	 * remaining in the large single prefix key group we think we've
	 * encountered.
	 */
	SO1_printf("Moving " INT64_FORMAT " tuples to presorted prefix tuplesort\n", nTuples);
	node->n_fullsort_remaining -= nTuples;
	SO1_printf("Setting n_fullsort_remaining to " INT64_FORMAT "\n", node->n_fullsort_remaining);

	if (node->n_fullsort_remaining == 0)
	{
		/*
		 * We've found that all tuples remaining in the full sort batch are in
		 * the same prefix key group and moved all of those tuples into the
		 * presorted prefix tuplesort.  We don't know that we've yet found the
		 * last tuple in the current prefix key group, so save our pivot
		 * comparison tuple and continue fetching tuples from the outer
		 * execution node to load into the presorted prefix tuplesort.
		 */
		ExecCopySlot(node->group_pivot, node->transfer_tuple);
		SO_printf("Setting execution_status to INCSORT_LOADPREFIXSORT (switchToPresortedPrefixMode)\n");
		node->execution_status = INCSORT_LOADPREFIXSORT;

		/*
		 * Make sure we clear the transfer tuple slot so that next time we
		 * encounter a large prefix key group we don't incorrectly assume we
		 * have a tuple carried over from the previous group.
		 */
		ExecClearTuple(node->transfer_tuple);
	}
	else
	{
		/*
		 * We finished a group but didn't consume all of the tuples from the
		 * full sort state, so we'll sort this batch, let the outer node read
		 * out all of those tuples, and then come back around to find another
		 * batch.
		 */
		SO1_printf("Sorting presorted prefix tuplesort with " INT64_FORMAT " tuples\n", nTuples);
		tuplesort_performsort(node->prefixsort_state);

		INSTRUMENT_SORT_GROUP(node, prefixsort);

		if (node->bounded)
		{
			/*
			 * If the current node has a bound and we've already sorted n
			 * tuples, then the functional bound remaining is (original bound
			 * - n), so store the current number of processed tuples for use
			 * in configuring sorting bound.
			 */
			SO2_printf("Changing bound_Done from " INT64_FORMAT " to " INT64_FORMAT "\n",
					   Min(node->bound, node->bound_Done + nTuples), node->bound_Done);
			node->bound_Done = Min(node->bound, node->bound_Done + nTuples);
		}

		SO_printf("Setting execution_status to INCSORT_READPREFIXSORT  (switchToPresortedPrefixMode)\n");
		node->execution_status = INCSORT_READPREFIXSORT;
	}
}

/*
 * Sorting many small groups with tuplesort is inefficient. In order to
 * cope with this problem we don't start a new group until the current one
 * contains at least DEFAULT_MIN_GROUP_SIZE tuples (unfortunately this also
 * means we can't assume small groups of tuples all have the same prefix keys.)
 * When we have a bound that's less than DEFAULT_MIN_GROUP_SIZE we start looking
 * for the new group as soon as we've met our bound to avoid fetching more
 * tuples than we absolutely have to fetch.
 */
#define DEFAULT_MIN_GROUP_SIZE 32

/*
 * While we've optimized for small prefix key groups by not starting our prefix
 * key comparisons until we've reached a minimum number of tuples, we don't want
 * that optimization to cause us to lose out on the benefits of being able to
 * assume a large group of tuples is fully presorted by its prefix keys.
 * Therefore we use the DEFAULT_MAX_FULL_SORT_GROUP_SIZE cutoff as a heuristic
 * for determining when we believe we've encountered a large group, and, if we
 * get to that point without finding a new prefix key group we transition to
 * presorted prefix key mode.
 */
#define DEFAULT_MAX_FULL_SORT_GROUP_SIZE (2 * DEFAULT_MIN_GROUP_SIZE)

/* ----------------------------------------------------------------
 *		ExecIncrementalSort
 *
 *		Assuming that outer subtree returns tuple presorted by some prefix
 *		of target sort columns, performs incremental sort.
 *
 *		Conditions:
 *		  -- none.
 *
 *		Initial States:
 *		  -- the outer child is prepared to return the first tuple.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecIncrementalSort(PlanState *pstate)
{
	IncrementalSortState *node = castNode(IncrementalSortState, pstate);
	EState	   *estate;
	ScanDirection dir;
	Tuplesortstate *read_sortstate;
	Tuplesortstate *fullsort_state;
	TupleTableSlot *slot;
	IncrementalSort *plannode = (IncrementalSort *) node->ss.ps.plan;
	PlanState  *outerNode;
	TupleDesc	tupDesc;
	int64		nTuples = 0;
	int64		minGroupSize;

	CHECK_FOR_INTERRUPTS();

	estate = node->ss.ps.state;
	dir = estate->es_direction;
	fullsort_state = node->fullsort_state;

	/*
	 * If a previous iteration has sorted a batch, then we need to check to
	 * see if there are any remaining tuples in that batch that we can return
	 * before moving on to other execution states.
	 */
	if (node->execution_status == INCSORT_READFULLSORT
		|| node->execution_status == INCSORT_READPREFIXSORT)
	{
		/*
		 * Return next tuple from the current sorted group set if available.
		 */
		read_sortstate = node->execution_status == INCSORT_READFULLSORT ?
			fullsort_state : node->prefixsort_state;
		slot = node->ss.ps.ps_ResultTupleSlot;

		/*
		 * We have to populate the slot from the tuplesort before checking
		 * outerNodeDone because it will set the slot to NULL if no more
		 * tuples remain. If the tuplesort is empty, but we don't have any
		 * more tuples available for sort from the outer node, then
		 * outerNodeDone will have been set so we'll return that now-empty
		 * slot to the caller.
		 */
		if (tuplesort_gettupleslot(read_sortstate, ScanDirectionIsForward(dir),
								   false, slot, NULL) || node->outerNodeDone)

			/*
			 * Note: there isn't a good test case for the node->outerNodeDone
			 * check directly, but we need it for any plan where the outer
			 * node will fail when trying to fetch too many tuples.
			 */
			return slot;
		else if (node->n_fullsort_remaining > 0)
		{
			/*
			 * When we transition to presorted prefix mode, we might have
			 * accumulated at least one additional prefix key group in the
			 * full sort tuplesort. The first call to
			 * switchToPresortedPrefixMode() will have pulled the first one of
			 * those groups out, and we've returned those tuples to the parent
			 * node, but if at this point we still have tuples remaining in
			 * the full sort state (i.e., n_fullsort_remaining > 0), then we
			 * need to re-execute the prefix mode transition function to pull
			 * out the next prefix key group.
			 */
			SO1_printf("Re-calling switchToPresortedPrefixMode() because n_fullsort_remaining is > 0 (" INT64_FORMAT ")\n",
					   node->n_fullsort_remaining);
			switchToPresortedPrefixMode(pstate);
		}
		else
		{
			/*
			 * If we don't have any sorted tuples to read and we're not
			 * currently transitioning into presorted prefix sort mode, then
			 * it's time to start the process all over again by building a new
			 * group in the full sort state.
			 */
			SO_printf("Setting execution_status to INCSORT_LOADFULLSORT (n_fullsort_remaining > 0)\n");
			node->execution_status = INCSORT_LOADFULLSORT;
		}
	}

	/*
	 * Scan the subplan in the forward direction while creating the sorted
	 * data.
	 */
	estate->es_direction = ForwardScanDirection;

	outerNode = outerPlanState(node);
	tupDesc = ExecGetResultType(outerNode);

	/* Load tuples into the full sort state. */
	if (node->execution_status == INCSORT_LOADFULLSORT)
	{
		/*
		 * Initialize sorting structures.
		 */
		if (fullsort_state == NULL)
		{
			/*
			 * Initialize presorted column support structures for
			 * isCurrentGroup(). It's correct to do this along with the
			 * initial initialization for the full sort state (and not for the
			 * prefix sort state) since we always load the full sort state
			 * first.
			 */
			preparePresortedCols(node);

			/*
			 * Since we optimize small prefix key groups by accumulating a
			 * minimum number of tuples before sorting, we can't assume that a
			 * group of tuples all have the same prefix key values. Hence we
			 * setup the full sort tuplesort to sort by all requested sort
			 * keys.
			 */
			fullsort_state = tuplesort_begin_heap(tupDesc,
												  plannode->sort.numCols,
												  plannode->sort.sortColIdx,
												  plannode->sort.sortOperators,
												  plannode->sort.collations,
												  plannode->sort.nullsFirst,
												  work_mem,
												  NULL,
												  node->bounded ?
												  TUPLESORT_ALLOWBOUNDED :
												  TUPLESORT_NONE);
			node->fullsort_state = fullsort_state;
		}
		else
		{
			/* Reset sort for the next batch. */
			tuplesort_reset(fullsort_state);
		}

		/*
		 * Calculate the remaining tuples left if bounded and configure both
		 * bounded sort and the minimum group size accordingly.
		 */
		if (node->bounded)
		{
			int64		currentBound = node->bound - node->bound_Done;

			/*
			 * Bounded sort isn't likely to be a useful optimization for full
			 * sort mode since we limit full sort mode to a relatively small
			 * number of tuples and tuplesort doesn't switch over to top-n
			 * heap sort anyway unless it hits (2 * bound) tuples.
			 */
			if (currentBound < DEFAULT_MIN_GROUP_SIZE)
				tuplesort_set_bound(fullsort_state, currentBound);

			minGroupSize = Min(DEFAULT_MIN_GROUP_SIZE, currentBound);
		}
		else
			minGroupSize = DEFAULT_MIN_GROUP_SIZE;

		/*
		 * Because we have to read the next tuple to find out that we've
		 * encountered a new prefix key group, on subsequent groups we have to
		 * carry over that extra tuple and add it to the new group's sort here
		 * before we read any new tuples from the outer node.
		 */
		if (!TupIsNull(node->group_pivot))
		{
			tuplesort_puttupleslot(fullsort_state, node->group_pivot);
			nTuples++;

			/*
			 * We're in full sort mode accumulating a minimum number of tuples
			 * and not checking for prefix key equality yet, so we can't
			 * assume the group pivot tuple will remain the same -- unless
			 * we're using a minimum group size of 1, in which case the pivot
			 * is obviously still the pivot.
			 */
			if (nTuples != minGroupSize)
				ExecClearTuple(node->group_pivot);
		}


		/*
		 * Pull as many tuples from the outer node as possible given our
		 * current operating mode.
		 */
		for (;;)
		{
			slot = ExecProcNode(outerNode);

			/*
			 * If the outer node can't provide us any more tuples, then we can
			 * sort the current group and return those tuples.
			 */
			if (TupIsNull(slot))
			{
				/*
				 * We need to know later if the outer node has completed to be
				 * able to distinguish between being done with a batch and
				 * being done with the whole node.
				 */
				node->outerNodeDone = true;

				SO1_printf("Sorting fullsort with " INT64_FORMAT " tuples\n", nTuples);
				tuplesort_performsort(fullsort_state);

				INSTRUMENT_SORT_GROUP(node, fullsort);

				SO_printf("Setting execution_status to INCSORT_READFULLSORT (final tuple)\n");
				node->execution_status = INCSORT_READFULLSORT;
				break;
			}

			/* Accumulate the next group of presorted tuples. */
			if (nTuples < minGroupSize)
			{
				/*
				 * If we haven't yet hit our target minimum group size, then
				 * we don't need to bother checking for inclusion in the
				 * current prefix group since at this point we'll assume that
				 * we'll full sort this batch to avoid a large number of very
				 * tiny (and thus inefficient) sorts.
				 */
				tuplesort_puttupleslot(fullsort_state, slot);
				nTuples++;

				/*
				 * If we've reached our minimum group size, then we need to
				 * store the most recent tuple as a pivot.
				 */
				if (nTuples == minGroupSize)
					ExecCopySlot(node->group_pivot, slot);
			}
			else
			{
				/*
				 * If we've already accumulated enough tuples to reach our
				 * minimum group size, then we need to compare any additional
				 * tuples to our pivot tuple to see if we reach the end of
				 * that prefix key group. Only after we find changed prefix
				 * keys can we guarantee sort stability of the tuples we've
				 * already accumulated.
				 */
				if (isCurrentGroup(node, node->group_pivot, slot))
				{
					/*
					 * As long as the prefix keys match the pivot tuple then
					 * load the tuple into the tuplesort.
					 */
					tuplesort_puttupleslot(fullsort_state, slot);
					nTuples++;
				}
				else
				{
					/*
					 * Since the tuple we fetched isn't part of the current
					 * prefix key group we don't want to sort it as part of
					 * the current batch. Instead we use the group_pivot slot
					 * to carry it over to the next batch (even though we
					 * won't actually treat it as a group pivot).
					 */
					ExecCopySlot(node->group_pivot, slot);

					if (node->bounded)
					{
						/*
						 * If the current node has a bound, and we've already
						 * sorted n tuples, then the functional bound
						 * remaining is (original bound - n), so store the
						 * current number of processed tuples for later use
						 * configuring the sort state's bound.
						 */
						SO2_printf("Changing bound_Done from " INT64_FORMAT " to " INT64_FORMAT "\n",
								   node->bound_Done,
								   Min(node->bound, node->bound_Done + nTuples));
						node->bound_Done = Min(node->bound, node->bound_Done + nTuples);
					}

					/*
					 * Once we find changed prefix keys we can complete the
					 * sort and transition modes to reading out the sorted
					 * tuples.
					 */
					SO1_printf("Sorting fullsort tuplesort with " INT64_FORMAT " tuples\n",
							   nTuples);
					tuplesort_performsort(fullsort_state);

					INSTRUMENT_SORT_GROUP(node, fullsort);

					SO_printf("Setting execution_status to INCSORT_READFULLSORT (found end of group)\n");
					node->execution_status = INCSORT_READFULLSORT;
					break;
				}
			}

			/*
			 * Unless we've already transitioned modes to reading from the
			 * full sort state, then we assume that having read at least
			 * DEFAULT_MAX_FULL_SORT_GROUP_SIZE tuples means it's likely we're
			 * processing a large group of tuples all having equal prefix keys
			 * (but haven't yet found the final tuple in that prefix key
			 * group), so we need to transition into presorted prefix mode.
			 */
			if (nTuples > DEFAULT_MAX_FULL_SORT_GROUP_SIZE &&
				node->execution_status != INCSORT_READFULLSORT)
			{
				/*
				 * The group pivot we have stored has already been put into
				 * the tuplesort; we don't want to carry it over. Since we
				 * haven't yet found the end of the prefix key group, it might
				 * seem like we should keep this, but we don't actually know
				 * how many prefix key groups might be represented in the full
				 * sort state, so we'll let the mode transition function
				 * manage this state for us.
				 */
				ExecClearTuple(node->group_pivot);

				/*
				 * Unfortunately the tuplesort API doesn't include a way to
				 * retrieve tuples unless a sort has been performed, so we
				 * perform the sort even though we could just as easily rely
				 * on FIFO retrieval semantics when transferring them to the
				 * presorted prefix tuplesort.
				 */
				SO1_printf("Sorting fullsort tuplesort with " INT64_FORMAT " tuples\n", nTuples);
				tuplesort_performsort(fullsort_state);

				INSTRUMENT_SORT_GROUP(node, fullsort);

				/*
				 * If the full sort tuplesort happened to switch into top-n
				 * heapsort mode then we will only be able to retrieve
				 * currentBound tuples (since the tuplesort will have only
				 * retained the top-n tuples). This is safe even though we
				 * haven't yet completed fetching the current prefix key group
				 * because the tuples we've "lost" already sorted "below" the
				 * retained ones, and we're already contractually guaranteed
				 * to not need any more than the currentBound tuples.
				 */
				if (tuplesort_used_bound(node->fullsort_state))
				{
					int64		currentBound = node->bound - node->bound_Done;

					SO2_printf("Read " INT64_FORMAT " tuples, but setting to " INT64_FORMAT " because we used bounded sort\n",
							   nTuples, Min(currentBound, nTuples));
					nTuples = Min(currentBound, nTuples);
				}

				SO1_printf("Setting n_fullsort_remaining to " INT64_FORMAT " and calling switchToPresortedPrefixMode()\n",
						   nTuples);

				/*
				 * We might have multiple prefix key groups in the full sort
				 * state, so the mode transition function needs to know that
				 * it needs to move from the fullsort to presorted prefix
				 * sort.
				 */
				node->n_fullsort_remaining = nTuples;

				/* Transition the tuples to the presorted prefix tuplesort. */
				switchToPresortedPrefixMode(pstate);

				/*
				 * Since we know we had tuples to move to the presorted prefix
				 * tuplesort, we know that unless that transition has verified
				 * that all tuples belonged to the same prefix key group (in
				 * which case we can go straight to continuing to load tuples
				 * into that tuplesort), we should have a tuple to return
				 * here.
				 *
				 * Either way, the appropriate execution status should have
				 * been set by switchToPresortedPrefixMode(), so we can drop
				 * out of the loop here and let the appropriate path kick in.
				 */
				break;
			}
		}
	}

	if (node->execution_status == INCSORT_LOADPREFIXSORT)
	{
		/*
		 * We only enter this state after the mode transition function has
		 * confirmed all remaining tuples from the full sort state have the
		 * same prefix and moved those tuples to the prefix sort state. That
		 * function has also set a group pivot tuple (which doesn't need to be
		 * carried over; it's already been put into the prefix sort state).
		 */
		Assert(!TupIsNull(node->group_pivot));

		/*
		 * Read tuples from the outer node and load them into the prefix sort
		 * state until we encounter a tuple whose prefix keys don't match the
		 * current group_pivot tuple, since we can't guarantee sort stability
		 * until we have all tuples matching those prefix keys.
		 */
		for (;;)
		{
			slot = ExecProcNode(outerNode);

			/*
			 * If we've exhausted tuples from the outer node we're done
			 * loading the prefix sort state.
			 */
			if (TupIsNull(slot))
			{
				/*
				 * We need to know later if the outer node has completed to be
				 * able to distinguish between being done with a batch and
				 * being done with the whole node.
				 */
				node->outerNodeDone = true;
				break;
			}

			/*
			 * If the tuple's prefix keys match our pivot tuple, we're not
			 * done yet and can load it into the prefix sort state. If not, we
			 * don't want to sort it as part of the current batch. Instead we
			 * use the group_pivot slot to carry it over to the next batch
			 * (even though we won't actually treat it as a group pivot).
			 */
			if (isCurrentGroup(node, node->group_pivot, slot))
			{
				tuplesort_puttupleslot(node->prefixsort_state, slot);
				nTuples++;
			}
			else
			{
				ExecCopySlot(node->group_pivot, slot);
				break;
			}
		}

		/*
		 * Perform the sort and begin returning the tuples to the parent plan
		 * node.
		 */
		SO1_printf("Sorting presorted prefix tuplesort with " INT64_FORMAT " tuples\n", nTuples);
		tuplesort_performsort(node->prefixsort_state);

		INSTRUMENT_SORT_GROUP(node, prefixsort);

		SO_printf("Setting execution_status to INCSORT_READPREFIXSORT (found end of group)\n");
		node->execution_status = INCSORT_READPREFIXSORT;

		if (node->bounded)
		{
			/*
			 * If the current node has a bound, and we've already sorted n
			 * tuples, then the functional bound remaining is (original bound
			 * - n), so store the current number of processed tuples for use
			 * in configuring sorting bound.
			 */
			SO2_printf("Changing bound_Done from " INT64_FORMAT " to " INT64_FORMAT "\n",
					   node->bound_Done,
					   Min(node->bound, node->bound_Done + nTuples));
			node->bound_Done = Min(node->bound, node->bound_Done + nTuples);
		}
	}

	/* Restore to user specified direction. */
	estate->es_direction = dir;

	/*
	 * Get the first or next tuple from tuplesort. Returns NULL if no more
	 * tuples.
	 */
	read_sortstate = node->execution_status == INCSORT_READFULLSORT ?
		fullsort_state : node->prefixsort_state;
	slot = node->ss.ps.ps_ResultTupleSlot;
	(void) tuplesort_gettupleslot(read_sortstate, ScanDirectionIsForward(dir),
								  false, slot, NULL);
	return slot;
}

/* ----------------------------------------------------------------
 *		ExecInitIncrementalSort
 *
 *		Creates the run-time state information for the sort node
 *		produced by the planner and initializes its outer subtree.
 * ----------------------------------------------------------------
 */
IncrementalSortState *
ExecInitIncrementalSort(IncrementalSort *node, EState *estate, int eflags)
{
	IncrementalSortState *incrsortstate;

	SO_printf("ExecInitIncrementalSort: initializing sort node\n");

	/*
	 * Incremental sort can't be used with EXEC_FLAG_BACKWARD or
	 * EXEC_FLAG_MARK, because the current sort state contains only one sort
	 * batch rather than the full result set.
	 */
	Assert((eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)) == 0);

	/* Initialize state structure. */
	incrsortstate = makeNode(IncrementalSortState);
	incrsortstate->ss.ps.plan = (Plan *) node;
	incrsortstate->ss.ps.state = estate;
	incrsortstate->ss.ps.ExecProcNode = ExecIncrementalSort;

	incrsortstate->execution_status = INCSORT_LOADFULLSORT;
	incrsortstate->bounded = false;
	incrsortstate->outerNodeDone = false;
	incrsortstate->bound_Done = 0;
	incrsortstate->fullsort_state = NULL;
	incrsortstate->prefixsort_state = NULL;
	incrsortstate->group_pivot = NULL;
	incrsortstate->transfer_tuple = NULL;
	incrsortstate->n_fullsort_remaining = 0;
	incrsortstate->presorted_keys = NULL;

	if (incrsortstate->ss.ps.instrument != NULL)
	{
		IncrementalSortGroupInfo *fullsortGroupInfo =
			&incrsortstate->incsort_info.fullsortGroupInfo;
		IncrementalSortGroupInfo *prefixsortGroupInfo =
			&incrsortstate->incsort_info.prefixsortGroupInfo;

		fullsortGroupInfo->groupCount = 0;
		fullsortGroupInfo->maxDiskSpaceUsed = 0;
		fullsortGroupInfo->totalDiskSpaceUsed = 0;
		fullsortGroupInfo->maxMemorySpaceUsed = 0;
		fullsortGroupInfo->totalMemorySpaceUsed = 0;
		fullsortGroupInfo->sortMethods = 0;
		prefixsortGroupInfo->groupCount = 0;
		prefixsortGroupInfo->maxDiskSpaceUsed = 0;
		prefixsortGroupInfo->totalDiskSpaceUsed = 0;
		prefixsortGroupInfo->maxMemorySpaceUsed = 0;
		prefixsortGroupInfo->totalMemorySpaceUsed = 0;
		prefixsortGroupInfo->sortMethods = 0;
	}

	/*
	 * Miscellaneous initialization
	 *
	 * Sort nodes don't initialize their ExprContexts because they never call
	 * ExecQual or ExecProject.
	 */

	/*
	 * Initialize child nodes.
	 *
	 * Incremental sort does not support backwards scans and mark/restore, so
	 * we don't bother removing the flags from eflags here. We allow passing a
	 * REWIND flag, because although incremental sort can't use it, the child
	 * nodes may be able to do something more useful.
	 */
	outerPlanState(incrsortstate) = ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * Initialize scan slot and type.
	 */
	ExecCreateScanSlotFromOuterPlan(estate, &incrsortstate->ss, &TTSOpsMinimalTuple);

	/*
	 * Initialize return slot and type. No need to initialize projection info
	 * because we don't do any projections.
	 */
	ExecInitResultTupleSlotTL(&incrsortstate->ss.ps, &TTSOpsMinimalTuple);
	incrsortstate->ss.ps.ps_ProjInfo = NULL;

	/*
	 * Initialize standalone slots to store a tuple for pivot prefix keys and
	 * for carrying over a tuple from one batch to the next.
	 */
	incrsortstate->group_pivot =
		MakeSingleTupleTableSlot(ExecGetResultType(outerPlanState(incrsortstate)),
								 &TTSOpsMinimalTuple);
	incrsortstate->transfer_tuple =
		MakeSingleTupleTableSlot(ExecGetResultType(outerPlanState(incrsortstate)),
								 &TTSOpsMinimalTuple);

	SO_printf("ExecInitIncrementalSort: sort node initialized\n");

	return incrsortstate;
}

/* ----------------------------------------------------------------
 *		ExecEndIncrementalSort(node)
 * ----------------------------------------------------------------
 */
void
ExecEndIncrementalSort(IncrementalSortState *node)
{
	SO_printf("ExecEndIncrementalSort: shutting down sort node\n");

	ExecDropSingleTupleTableSlot(node->group_pivot);
	ExecDropSingleTupleTableSlot(node->transfer_tuple);

	/*
	 * Release tuplesort resources.
	 */
	if (node->fullsort_state != NULL)
	{
		tuplesort_end(node->fullsort_state);
		node->fullsort_state = NULL;
	}
	if (node->prefixsort_state != NULL)
	{
		tuplesort_end(node->prefixsort_state);
		node->prefixsort_state = NULL;
	}

	/*
	 * Shut down the subplan.
	 */
	ExecEndNode(outerPlanState(node));

	SO_printf("ExecEndIncrementalSort: sort node shutdown\n");
}

void
ExecReScanIncrementalSort(IncrementalSortState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	/*
	 * Incremental sort doesn't support efficient rescan even when parameters
	 * haven't changed (e.g., rewind) because unlike regular sort we don't
	 * store all tuples at once for the full sort.
	 *
	 * So even if EXEC_FLAG_REWIND is set we just reset all of our state and
	 * re-execute the sort along with the child node. Incremental sort itself
	 * can't do anything smarter, but maybe the child nodes can.
	 *
	 * In theory if we've only filled the full sort with one batch (and
	 * haven't reset it for a new batch yet) then we could efficiently rewind,
	 * but that seems a narrow enough case that it's not worth handling
	 * specially at this time.
	 */

	/* must drop pointer to sort result tuple */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	if (node->group_pivot != NULL)
		ExecClearTuple(node->group_pivot);
	if (node->transfer_tuple != NULL)
		ExecClearTuple(node->transfer_tuple);

	node->outerNodeDone = false;
	node->n_fullsort_remaining = 0;
	node->bound_Done = 0;

	node->execution_status = INCSORT_LOADFULLSORT;

	/*
	 * If we've set up either of the sort states yet, we need to reset them.
	 * We could end them and null out the pointers, but there's no reason to
	 * repay the setup cost, and because ExecIncrementalSort guards presorted
	 * column functions by checking to see if the full sort state has been
	 * initialized yet, setting the sort states to null here might actually
	 * cause a leak.
	 */
	if (node->fullsort_state != NULL)
		tuplesort_reset(node->fullsort_state);
	if (node->prefixsort_state != NULL)
		tuplesort_reset(node->prefixsort_state);

	/*
	 * If chgParam of subnode is not null, then the plan will be re-scanned by
	 * the first ExecProcNode.
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
}

/* ----------------------------------------------------------------
 *						Parallel Query Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecSortEstimate
 *
 *		Estimate space required to propagate sort statistics.
 * ----------------------------------------------------------------
 */
void
ExecIncrementalSortEstimate(IncrementalSortState *node, ParallelContext *pcxt)
{
	Size		size;

	/* don't need this if not instrumenting or no workers */
	if (!node->ss.ps.instrument || pcxt->nworkers == 0)
		return;

	size = mul_size(pcxt->nworkers, sizeof(IncrementalSortInfo));
	size = add_size(size, offsetof(SharedIncrementalSortInfo, sinfo));
	shm_toc_estimate_chunk(&pcxt->estimator, size);
	shm_toc_estimate_keys(&pcxt->estimator, 1);
}

/* ----------------------------------------------------------------
 *		ExecSortInitializeDSM
 *
 *		Initialize DSM space for sort statistics.
 * ----------------------------------------------------------------
 */
void
ExecIncrementalSortInitializeDSM(IncrementalSortState *node, ParallelContext *pcxt)
{
	Size		size;

	/* don't need this if not instrumenting or no workers */
	if (!node->ss.ps.instrument || pcxt->nworkers == 0)
		return;

	size = offsetof(SharedIncrementalSortInfo, sinfo)
		+ pcxt->nworkers * sizeof(IncrementalSortInfo);
	node->shared_info = shm_toc_allocate(pcxt->toc, size);
	/* ensure any unfilled slots will contain zeroes */
	memset(node->shared_info, 0, size);
	node->shared_info->num_workers = pcxt->nworkers;
	shm_toc_insert(pcxt->toc, node->ss.ps.plan->plan_node_id,
				   node->shared_info);
}

/* ----------------------------------------------------------------
 *		ExecSortInitializeWorker
 *
 *		Attach worker to DSM space for sort statistics.
 * ----------------------------------------------------------------
 */
void
ExecIncrementalSortInitializeWorker(IncrementalSortState *node, ParallelWorkerContext *pwcxt)
{
	node->shared_info =
		shm_toc_lookup(pwcxt->toc, node->ss.ps.plan->plan_node_id, true);
	node->am_worker = true;
}

/* ----------------------------------------------------------------
 *		ExecSortRetrieveInstrumentation
 *
 *		Transfer sort statistics from DSM to private memory.
 * ----------------------------------------------------------------
 */
void
ExecIncrementalSortRetrieveInstrumentation(IncrementalSortState *node)
{
	Size		size;
	SharedIncrementalSortInfo *si;

	if (node->shared_info == NULL)
		return;

	size = offsetof(SharedIncrementalSortInfo, sinfo)
		+ node->shared_info->num_workers * sizeof(IncrementalSortInfo);
	si = palloc(size);
	memcpy(si, node->shared_info, size);
	node->shared_info = si;
}
