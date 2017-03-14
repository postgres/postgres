/*-------------------------------------------------------------------------
 *
 * nodeGatherMerge.c
 *		Scan a plan in multiple workers, and do order-preserving merge.
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeGatherMerge.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/relscan.h"
#include "access/xact.h"
#include "executor/execdebug.h"
#include "executor/execParallel.h"
#include "executor/nodeGatherMerge.h"
#include "executor/nodeSubplan.h"
#include "executor/tqueue.h"
#include "lib/binaryheap.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/*
 * Tuple array for each worker
 */
typedef struct GMReaderTupleBuffer
{
	HeapTuple  *tuple;
	int			readCounter;
	int			nTuples;
	bool		done;
}	GMReaderTupleBuffer;

/*
 * When we read tuples from workers, it's a good idea to read several at once
 * for efficiency when possible: this minimizes context-switching overhead.
 * But reading too many at a time wastes memory without improving performance.
 */
#define MAX_TUPLE_STORE 10

static int32 heap_compare_slots(Datum a, Datum b, void *arg);
static TupleTableSlot *gather_merge_getnext(GatherMergeState *gm_state);
static HeapTuple gm_readnext_tuple(GatherMergeState *gm_state, int nreader,
				  bool nowait, bool *done);
static void gather_merge_init(GatherMergeState *gm_state);
static void ExecShutdownGatherMergeWorkers(GatherMergeState *node);
static bool gather_merge_readnext(GatherMergeState *gm_state, int reader,
					  bool nowait);
static void form_tuple_array(GatherMergeState *gm_state, int reader);

/* ----------------------------------------------------------------
 *		ExecInitGather
 * ----------------------------------------------------------------
 */
GatherMergeState *
ExecInitGatherMerge(GatherMerge *node, EState *estate, int eflags)
{
	GatherMergeState *gm_state;
	Plan	   *outerNode;
	bool		hasoid;
	TupleDesc	tupDesc;

	/* Gather merge node doesn't have innerPlan node. */
	Assert(innerPlan(node) == NULL);

	/*
	 * create state structure
	 */
	gm_state = makeNode(GatherMergeState);
	gm_state->ps.plan = (Plan *) node;
	gm_state->ps.state = estate;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &gm_state->ps);

	/*
	 * initialize child expressions
	 */
	gm_state->ps.qual =
		ExecInitQual(node->plan.qual, &gm_state->ps);

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &gm_state->ps);

	/*
	 * now initialize outer plan
	 */
	outerNode = outerPlan(node);
	outerPlanState(gm_state) = ExecInitNode(outerNode, estate, eflags);

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&gm_state->ps);
	ExecAssignProjectionInfo(&gm_state->ps, NULL);

	gm_state->gm_initialized = false;

	/*
	 * initialize sort-key information
	 */
	if (node->numCols)
	{
		int			i;

		gm_state->gm_nkeys = node->numCols;
		gm_state->gm_sortkeys =
			palloc0(sizeof(SortSupportData) * node->numCols);

		for (i = 0; i < node->numCols; i++)
		{
			SortSupport sortKey = gm_state->gm_sortkeys + i;

			sortKey->ssup_cxt = CurrentMemoryContext;
			sortKey->ssup_collation = node->collations[i];
			sortKey->ssup_nulls_first = node->nullsFirst[i];
			sortKey->ssup_attno = node->sortColIdx[i];

			/*
			 * We don't perform abbreviated key conversion here, for the same
			 * reasons that it isn't used in MergeAppend
			 */
			sortKey->abbreviate = false;

			PrepareSortSupportFromOrderingOp(node->sortOperators[i], sortKey);
		}
	}

	/*
	 * store the tuple descriptor into gather merge state, so we can use it
	 * later while initializing the gather merge slots.
	 */
	if (!ExecContextForcesOids(&gm_state->ps, &hasoid))
		hasoid = false;
	tupDesc = ExecTypeFromTL(outerNode->targetlist, hasoid);
	gm_state->tupDesc = tupDesc;

	return gm_state;
}

/* ----------------------------------------------------------------
 *		ExecGatherMerge(node)
 *
 *		Scans the relation via multiple workers and returns
 *		the next qualifying tuple.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecGatherMerge(GatherMergeState *node)
{
	TupleTableSlot *slot;
	ExprContext *econtext;
	int			i;

	/*
	 * As with Gather, we don't launch workers until this node is actually
	 * executed.
	 */
	if (!node->initialized)
	{
		EState	   *estate = node->ps.state;
		GatherMerge *gm = (GatherMerge *) node->ps.plan;

		/*
		 * Sometimes we might have to run without parallelism; but if parallel
		 * mode is active then we can try to fire up some workers.
		 */
		if (gm->num_workers > 0 && IsInParallelMode())
		{
			ParallelContext *pcxt;

			/* Initialize data structures for workers. */
			if (!node->pei)
				node->pei = ExecInitParallelPlan(node->ps.lefttree,
												 estate,
												 gm->num_workers);

			/* Try to launch workers. */
			pcxt = node->pei->pcxt;
			LaunchParallelWorkers(pcxt);
			node->nworkers_launched = pcxt->nworkers_launched;

			/* Set up tuple queue readers to read the results. */
			if (pcxt->nworkers_launched > 0)
			{
				node->nreaders = 0;
				node->reader = palloc(pcxt->nworkers_launched *
									  sizeof(TupleQueueReader *));

				Assert(gm->numCols);

				for (i = 0; i < pcxt->nworkers_launched; ++i)
				{
					shm_mq_set_handle(node->pei->tqueue[i],
									  pcxt->worker[i].bgwhandle);
					node->reader[node->nreaders++] =
						CreateTupleQueueReader(node->pei->tqueue[i],
											   node->tupDesc);
				}
			}
			else
			{
				/* No workers?	Then never mind. */
				ExecShutdownGatherMergeWorkers(node);
			}
		}

		/* always allow leader to participate */
		node->need_to_scan_locally = true;
		node->initialized = true;
	}

	/*
	 * Reset per-tuple memory context to free any expression evaluation
	 * storage allocated in the previous tuple cycle.
	 */
	econtext = node->ps.ps_ExprContext;
	ResetExprContext(econtext);

	/*
	 * Get next tuple, either from one of our workers, or by running the
	 * plan ourselves.
	 */
	slot = gather_merge_getnext(node);
	if (TupIsNull(slot))
		return NULL;

	/*
	 * form the result tuple using ExecProject(), and return it --- unless
	 * the projection produces an empty set, in which case we must loop
	 * back around for another tuple
	 */
	econtext->ecxt_outertuple = slot;
	return ExecProject(node->ps.ps_ProjInfo);
}

/* ----------------------------------------------------------------
 *		ExecEndGatherMerge
 *
 *		frees any storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndGatherMerge(GatherMergeState *node)
{
	ExecEndNode(outerPlanState(node));      /* let children clean up first */
	ExecShutdownGatherMerge(node);
	ExecFreeExprContext(&node->ps);
	ExecClearTuple(node->ps.ps_ResultTupleSlot);
}

/* ----------------------------------------------------------------
 *		ExecShutdownGatherMerge
 *
 *		Destroy the setup for parallel workers including parallel context.
 *		Collect all the stats after workers are stopped, else some work
 *		done by workers won't be accounted.
 * ----------------------------------------------------------------
 */
void
ExecShutdownGatherMerge(GatherMergeState *node)
{
	ExecShutdownGatherMergeWorkers(node);

	/* Now destroy the parallel context. */
	if (node->pei != NULL)
	{
		ExecParallelCleanup(node->pei);
		node->pei = NULL;
	}
}

/* ----------------------------------------------------------------
 *		ExecShutdownGatherMergeWorkers
 *
 *		Destroy the parallel workers.  Collect all the stats after
 *		workers are stopped, else some work done by workers won't be
 *		accounted.
 * ----------------------------------------------------------------
 */
static void
ExecShutdownGatherMergeWorkers(GatherMergeState *node)
{
	/* Shut down tuple queue readers before shutting down workers. */
	if (node->reader != NULL)
	{
		int			i;

		for (i = 0; i < node->nreaders; ++i)
			if (node->reader[i])
				DestroyTupleQueueReader(node->reader[i]);

		pfree(node->reader);
		node->reader = NULL;
	}

	/* Now shut down the workers. */
	if (node->pei != NULL)
		ExecParallelFinish(node->pei);
}

/* ----------------------------------------------------------------
 *		ExecReScanGatherMerge
 *
 *		Re-initialize the workers and rescans a relation via them.
 * ----------------------------------------------------------------
 */
void
ExecReScanGatherMerge(GatherMergeState *node)
{
	/*
	 * Re-initialize the parallel workers to perform rescan of relation. We
	 * want to gracefully shutdown all the workers so that they should be able
	 * to propagate any error or other information to master backend before
	 * dying.  Parallel context will be reused for rescan.
	 */
	ExecShutdownGatherMergeWorkers(node);

	node->initialized = false;

	if (node->pei)
		ExecParallelReinitialize(node->pei);

	ExecReScan(node->ps.lefttree);
}

/*
 * Initialize the Gather merge tuple read.
 *
 * Pull at least a single tuple from each worker + leader and set up the heap.
 */
static void
gather_merge_init(GatherMergeState *gm_state)
{
	int			nreaders = gm_state->nreaders;
	bool		initialize = true;
	int			i;

	/*
	 * Allocate gm_slots for the number of worker + one more slot for leader.
	 * Last slot is always for leader. Leader always calls ExecProcNode() to
	 * read the tuple which will return the TupleTableSlot. Later it will
	 * directly get assigned to gm_slot. So just initialize leader gm_slot
	 * with NULL. For other slots below code will call
	 * ExecInitExtraTupleSlot() which will do the initialization of worker
	 * slots.
	 */
	gm_state->gm_slots =
		palloc((gm_state->nreaders + 1) * sizeof(TupleTableSlot *));
	gm_state->gm_slots[gm_state->nreaders] = NULL;

	/* Initialize the tuple slot and tuple array for each worker */
	gm_state->gm_tuple_buffers =
		(GMReaderTupleBuffer *) palloc0(sizeof(GMReaderTupleBuffer) *
										(gm_state->nreaders + 1));
	for (i = 0; i < gm_state->nreaders; i++)
	{
		/* Allocate the tuple array with MAX_TUPLE_STORE size */
		gm_state->gm_tuple_buffers[i].tuple =
			(HeapTuple *) palloc0(sizeof(HeapTuple) * MAX_TUPLE_STORE);

		/* Initialize slot for worker */
		gm_state->gm_slots[i] = ExecInitExtraTupleSlot(gm_state->ps.state);
		ExecSetSlotDescriptor(gm_state->gm_slots[i],
							  gm_state->tupDesc);
	}

	/* Allocate the resources for the merge */
	gm_state->gm_heap = binaryheap_allocate(gm_state->nreaders + 1,
											heap_compare_slots,
											gm_state);

	/*
	 * First, try to read a tuple from each worker (including leader) in
	 * nowait mode, so that we initialize read from each worker as well as
	 * leader. After this, if all active workers are unable to produce a
	 * tuple, then re-read and this time use wait mode. For workers that were
	 * able to produce a tuple in the earlier loop and are still active, just
	 * try to fill the tuple array if more tuples are avaiable.
	 */
reread:
	for (i = 0; i < nreaders + 1; i++)
	{
		if (!gm_state->gm_tuple_buffers[i].done &&
			(TupIsNull(gm_state->gm_slots[i]) ||
			 gm_state->gm_slots[i]->tts_isempty))
		{
			if (gather_merge_readnext(gm_state, i, initialize))
			{
				binaryheap_add_unordered(gm_state->gm_heap,
										 Int32GetDatum(i));
			}
		}
		else
			form_tuple_array(gm_state, i);
	}
	initialize = false;

	for (i = 0; i < nreaders; i++)
		if (!gm_state->gm_tuple_buffers[i].done &&
			(TupIsNull(gm_state->gm_slots[i]) ||
			 gm_state->gm_slots[i]->tts_isempty))
			goto reread;

	binaryheap_build(gm_state->gm_heap);
	gm_state->gm_initialized = true;
}

/*
 * Clear out the tuple table slots for each gather merge input,
 * and return a cleared slot.
 */
static TupleTableSlot *
gather_merge_clear_slots(GatherMergeState *gm_state)
{
	int			i;

	for (i = 0; i < gm_state->nreaders; i++)
	{
		pfree(gm_state->gm_tuple_buffers[i].tuple);
		gm_state->gm_slots[i] = ExecClearTuple(gm_state->gm_slots[i]);
	}

	/* Free tuple array as we don't need it any more */
	pfree(gm_state->gm_tuple_buffers);
	/* Free the binaryheap, which was created for sort */
	binaryheap_free(gm_state->gm_heap);

	/* return any clear slot */
	return gm_state->gm_slots[0];
}

/*
 * Read the next tuple for gather merge.
 *
 * Fetch the sorted tuple out of the heap.
 */
static TupleTableSlot *
gather_merge_getnext(GatherMergeState *gm_state)
{
	int			i;

	if (!gm_state->gm_initialized)
	{
		/*
		 * First time through: pull the first tuple from each participant, and
		 * set up the heap.
		 */
		gather_merge_init(gm_state);
	}
	else
	{
		/*
		 * Otherwise, pull the next tuple from whichever participant we
		 * returned from last time, and reinsert that participant's index into
		 * the heap, because it might now compare differently against the
		 * other elements of the heap.
		 */
		i = DatumGetInt32(binaryheap_first(gm_state->gm_heap));

		if (gather_merge_readnext(gm_state, i, false))
			binaryheap_replace_first(gm_state->gm_heap, Int32GetDatum(i));
		else
			(void) binaryheap_remove_first(gm_state->gm_heap);
	}

	if (binaryheap_empty(gm_state->gm_heap))
	{
		/* All the queues are exhausted, and so is the heap */
		return gather_merge_clear_slots(gm_state);
	}
	else
	{
		/* Return next tuple from whichever participant has the leading one */
		i = DatumGetInt32(binaryheap_first(gm_state->gm_heap));
		return gm_state->gm_slots[i];
	}
}

/*
 * Read the tuple for given reader in nowait mode, and form the tuple array.
 */
static void
form_tuple_array(GatherMergeState *gm_state, int reader)
{
	GMReaderTupleBuffer *tuple_buffer = &gm_state->gm_tuple_buffers[reader];
	int			i;

	/* Last slot is for leader and we don't build tuple array for leader */
	if (reader == gm_state->nreaders)
		return;

	/*
	 * We here because we already read all the tuples from the tuple array, so
	 * initialize the counter to zero.
	 */
	if (tuple_buffer->nTuples == tuple_buffer->readCounter)
		tuple_buffer->nTuples = tuple_buffer->readCounter = 0;

	/* Tuple array is already full? */
	if (tuple_buffer->nTuples == MAX_TUPLE_STORE)
		return;

	for (i = tuple_buffer->nTuples; i < MAX_TUPLE_STORE; i++)
	{
		tuple_buffer->tuple[i] = heap_copytuple(gm_readnext_tuple(gm_state,
																  reader,
																  false,
													   &tuple_buffer->done));
		if (!HeapTupleIsValid(tuple_buffer->tuple[i]))
			break;
		tuple_buffer->nTuples++;
	}
}

/*
 * Store the next tuple for a given reader into the appropriate slot.
 *
 * Returns false if the reader is exhausted, and true otherwise.
 */
static bool
gather_merge_readnext(GatherMergeState *gm_state, int reader, bool nowait)
{
	GMReaderTupleBuffer *tuple_buffer;
	HeapTuple	tup = NULL;

	/*
	 * If we're being asked to generate a tuple from the leader, then we
	 * just call ExecProcNode as normal to produce one.
	 */
	if (gm_state->nreaders == reader)
	{
		if (gm_state->need_to_scan_locally)
		{
			PlanState  *outerPlan = outerPlanState(gm_state);
			TupleTableSlot *outerTupleSlot;

			outerTupleSlot = ExecProcNode(outerPlan);

			if (!TupIsNull(outerTupleSlot))
			{
				gm_state->gm_slots[reader] = outerTupleSlot;
				return true;
			}
			gm_state->gm_tuple_buffers[reader].done = true;
			gm_state->need_to_scan_locally = false;
		}
		return false;
	}

	/* Otherwise, check the state of the relevant tuple buffer. */
	tuple_buffer = &gm_state->gm_tuple_buffers[reader];

	if (tuple_buffer->nTuples > tuple_buffer->readCounter)
	{
		/* Return any tuple previously read that is still buffered. */
		tuple_buffer = &gm_state->gm_tuple_buffers[reader];
		tup = tuple_buffer->tuple[tuple_buffer->readCounter++];
	}
	else if (tuple_buffer->done)
	{
		/* Reader is known to be exhausted. */
		DestroyTupleQueueReader(gm_state->reader[reader]);
		gm_state->reader[reader] = NULL;
		return false;
	}
	else
	{
		/* Read and buffer next tuple. */
		tup = heap_copytuple(gm_readnext_tuple(gm_state,
											   reader,
											   nowait,
											   &tuple_buffer->done));

		/*
		 * Attempt to read more tuples in nowait mode and store them in
		 * the tuple array.
		 */
		if (HeapTupleIsValid(tup))
			form_tuple_array(gm_state, reader);
		else
			return false;
	}

	Assert(HeapTupleIsValid(tup));

	/* Build the TupleTableSlot for the given tuple */
	ExecStoreTuple(tup,			/* tuple to store */
				   gm_state->gm_slots[reader],	/* slot in which to store the
												 * tuple */
				   InvalidBuffer,		/* buffer associated with this tuple */
				   true);		/* pfree this pointer if not from heap */

	return true;
}

/*
 * Attempt to read a tuple from given reader.
 */
static HeapTuple
gm_readnext_tuple(GatherMergeState *gm_state, int nreader, bool nowait,
				  bool *done)
{
	TupleQueueReader *reader;
	HeapTuple	tup = NULL;
	MemoryContext oldContext;
	MemoryContext tupleContext;

	tupleContext = gm_state->ps.ps_ExprContext->ecxt_per_tuple_memory;

	if (done != NULL)
		*done = false;

	/* Check for async events, particularly messages from workers. */
	CHECK_FOR_INTERRUPTS();

	/* Attempt to read a tuple. */
	reader = gm_state->reader[nreader];

	/* Run TupleQueueReaders in per-tuple context */
	oldContext = MemoryContextSwitchTo(tupleContext);
	tup = TupleQueueReaderNext(reader, nowait, done);
	MemoryContextSwitchTo(oldContext);

	return tup;
}

/*
 * We have one slot for each item in the heap array.  We use SlotNumber
 * to store slot indexes.  This doesn't actually provide any formal
 * type-safety, but it makes the code more self-documenting.
 */
typedef int32 SlotNumber;

/*
 * Compare the tuples in the two given slots.
 */
static int32
heap_compare_slots(Datum a, Datum b, void *arg)
{
	GatherMergeState *node = (GatherMergeState *) arg;
	SlotNumber	slot1 = DatumGetInt32(a);
	SlotNumber	slot2 = DatumGetInt32(b);

	TupleTableSlot *s1 = node->gm_slots[slot1];
	TupleTableSlot *s2 = node->gm_slots[slot2];
	int			nkey;

	Assert(!TupIsNull(s1));
	Assert(!TupIsNull(s2));

	for (nkey = 0; nkey < node->gm_nkeys; nkey++)
	{
		SortSupport sortKey = node->gm_sortkeys + nkey;
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
			return -compare;
	}
	return 0;
}
