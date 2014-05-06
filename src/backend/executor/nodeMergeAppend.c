/*-------------------------------------------------------------------------
 *
 * nodeMergeAppend.c
 *	  routines to handle MergeAppend nodes.
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
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

#include "access/nbtree.h"
#include "executor/execdebug.h"
#include "executor/nodeMergeAppend.h"
#include "utils/lsyscache.h"

/*
 * It gets quite confusing having a heap array (indexed by integers) which
 * contains integers which index into the slots array. These typedefs try to
 * clear it up, but they're only documentation.
 */
typedef int SlotNumber;
typedef int HeapPosition;

static void heap_insert_slot(MergeAppendState *node, SlotNumber new_slot);
static void heap_siftup_slot(MergeAppendState *node);
static int32 heap_compare_slots(MergeAppendState *node, SlotNumber slot1, SlotNumber slot2);


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
	int			nplans;
	int			i;
	ListCell   *lc;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * Set up empty vector of subplan states
	 */
	nplans = list_length(node->mergeplans);

	mergeplanstates = (PlanState **) palloc0(nplans * sizeof(PlanState *));

	/*
	 * create new MergeAppendState for our node
	 */
	mergestate->ps.plan = (Plan *) node;
	mergestate->ps.state = estate;
	mergestate->mergeplans = mergeplanstates;
	mergestate->ms_nplans = nplans;

	mergestate->ms_slots = (TupleTableSlot **) palloc0(sizeof(TupleTableSlot *) * nplans);
	mergestate->ms_heap = (int *) palloc0(sizeof(int) * nplans);

	/*
	 * Miscellaneous initialization
	 *
	 * MergeAppend plans don't have expression contexts because they never
	 * call ExecQual or ExecProject.
	 */

	/*
	 * MergeAppend nodes do have Result slots, which hold pointers to tuples,
	 * so we have to initialize them.
	 */
	ExecInitResultTupleSlot(estate, &mergestate->ps);

	/*
	 * call ExecInitNode on each of the plans to be executed and save the
	 * results into the array "mergeplans".
	 */
	i = 0;
	foreach(lc, node->mergeplans)
	{
		Plan	   *initNode = (Plan *) lfirst(lc);

		mergeplanstates[i] = ExecInitNode(initNode, estate, eflags);
		i++;
	}

	/*
	 * initialize output tuple type
	 */
	ExecAssignResultTypeFromTL(&mergestate->ps);
	mergestate->ps.ps_ProjInfo = NULL;

	/*
	 * initialize sort-key information
	 */
	mergestate->ms_nkeys = node->numCols;
	mergestate->ms_scankeys = palloc0(sizeof(ScanKeyData) * node->numCols);

	for (i = 0; i < node->numCols; i++)
	{
		Oid			sortFunction;
		bool		reverse;
		int			flags;

		if (!get_compare_function_for_ordering_op(node->sortOperators[i],
												  &sortFunction, &reverse))
			elog(ERROR, "operator %u is not a valid ordering operator",
				 node->sortOperators[i]);

		/* We use btree's conventions for encoding directionality */
		flags = 0;
		if (reverse)
			flags |= SK_BT_DESC;
		if (node->nullsFirst[i])
			flags |= SK_BT_NULLS_FIRST;

		/*
		 * We needn't fill in sk_strategy or sk_subtype since these scankeys
		 * will never be passed to an index.
		 */
		ScanKeyEntryInitialize(&mergestate->ms_scankeys[i],
							   flags,
							   node->sortColIdx[i],
							   InvalidStrategy,
							   InvalidOid,
							   node->collations[i],
							   sortFunction,
							   (Datum) 0);
	}

	/*
	 * initialize to show we have not run the subplans yet
	 */
	mergestate->ms_heap_size = 0;
	mergestate->ms_initialized = false;
	mergestate->ms_last_slot = -1;

	return mergestate;
}

/* ----------------------------------------------------------------
 *	   ExecMergeAppend
 *
 *		Handles iteration over multiple subplans.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecMergeAppend(MergeAppendState *node)
{
	TupleTableSlot *result;
	SlotNumber	i;

	if (!node->ms_initialized)
	{
		/*
		 * First time through: pull the first tuple from each subplan, and set
		 * up the heap.
		 */
		for (i = 0; i < node->ms_nplans; i++)
		{
			node->ms_slots[i] = ExecProcNode(node->mergeplans[i]);
			if (!TupIsNull(node->ms_slots[i]))
				heap_insert_slot(node, i);
		}
		node->ms_initialized = true;
	}
	else
	{
		/*
		 * Otherwise, pull the next tuple from whichever subplan we returned
		 * from last time, and insert it into the heap.  (We could simplify
		 * the logic a bit by doing this before returning from the prior call,
		 * but it's better to not pull tuples until necessary.)
		 */
		i = node->ms_last_slot;
		node->ms_slots[i] = ExecProcNode(node->mergeplans[i]);
		if (!TupIsNull(node->ms_slots[i]))
			heap_insert_slot(node, i);
	}

	if (node->ms_heap_size > 0)
	{
		/* Return the topmost heap node, and sift up the remaining nodes */
		i = node->ms_heap[0];
		result = node->ms_slots[i];
		node->ms_last_slot = i;
		heap_siftup_slot(node);
	}
	else
	{
		/* All the subplans are exhausted, and so is the heap */
		result = ExecClearTuple(node->ps.ps_ResultTupleSlot);
	}

	return result;
}

/*
 * Insert a new slot into the heap.  The slot must contain a valid tuple.
 */
static void
heap_insert_slot(MergeAppendState *node, SlotNumber new_slot)
{
	SlotNumber *heap = node->ms_heap;
	HeapPosition j;

	Assert(!TupIsNull(node->ms_slots[new_slot]));

	j = node->ms_heap_size++;	/* j is where the "hole" is */
	while (j > 0)
	{
		int			i = (j - 1) / 2;

		if (heap_compare_slots(node, new_slot, node->ms_heap[i]) >= 0)
			break;
		heap[j] = heap[i];
		j = i;
	}
	heap[j] = new_slot;
}

/*
 * Delete the heap top (the slot in heap[0]), and sift up.
 */
static void
heap_siftup_slot(MergeAppendState *node)
{
	SlotNumber *heap = node->ms_heap;
	HeapPosition i,
				n;

	if (--node->ms_heap_size <= 0)
		return;
	n = node->ms_heap_size;		/* heap[n] needs to be reinserted */
	i = 0;						/* i is where the "hole" is */
	for (;;)
	{
		int			j = 2 * i + 1;

		if (j >= n)
			break;
		if (j + 1 < n && heap_compare_slots(node, heap[j], heap[j + 1]) > 0)
			j++;
		if (heap_compare_slots(node, heap[n], heap[j]) <= 0)
			break;
		heap[i] = heap[j];
		i = j;
	}
	heap[i] = heap[n];
}

/*
 * Compare the tuples in the two given slots.
 */
static int32
heap_compare_slots(MergeAppendState *node, SlotNumber slot1, SlotNumber slot2)
{
	TupleTableSlot *s1 = node->ms_slots[slot1];
	TupleTableSlot *s2 = node->ms_slots[slot2];
	int			nkey;

	Assert(!TupIsNull(s1));
	Assert(!TupIsNull(s2));

	for (nkey = 0; nkey < node->ms_nkeys; nkey++)
	{
		ScanKey		scankey = node->ms_scankeys + nkey;
		AttrNumber	attno = scankey->sk_attno;
		Datum		datum1,
					datum2;
		bool		isNull1,
					isNull2;
		int32		compare;

		datum1 = slot_getattr(s1, attno, &isNull1);
		datum2 = slot_getattr(s2, attno, &isNull2);

		if (isNull1)
		{
			if (isNull2)
				continue;		/* NULL "=" NULL */
			else if (scankey->sk_flags & SK_BT_NULLS_FIRST)
				return -1;		/* NULL "<" NOT_NULL */
			else
				return 1;		/* NULL ">" NOT_NULL */
		}
		else if (isNull2)
		{
			if (scankey->sk_flags & SK_BT_NULLS_FIRST)
				return 1;		/* NOT_NULL ">" NULL */
			else
				return -1;		/* NOT_NULL "<" NULL */
		}
		else
		{
			compare = DatumGetInt32(FunctionCall2Coll(&scankey->sk_func,
													  scankey->sk_collation,
													  datum1, datum2));
			if (compare != 0)
			{
				if (scankey->sk_flags & SK_BT_DESC)
					compare = -compare;
				return compare;
			}
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
	node->ms_heap_size = 0;
	node->ms_initialized = false;
	node->ms_last_slot = -1;
}
