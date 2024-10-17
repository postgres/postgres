/*-------------------------------------------------------------------------
 *
 * nodeHash.c
 *	  Routines to hash relations for hashjoin
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeHash.c
 *
 * See note on parallelism in nodeHashjoin.c.
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		MultiExecHash	- generate an in-memory hash table of the relation
 *		ExecInitHash	- initialize node and subnodes
 *		ExecEndHash		- shutdown node and subnodes
 */

#include "postgres.h"

#include <math.h>
#include <limits.h>

#include "access/htup_details.h"
#include "access/parallel.h"
#include "catalog/pg_statistic.h"
#include "commands/tablespace.h"
#include "executor/execdebug.h"
#include "executor/hashjoin.h"
#include "executor/nodeHash.h"
#include "executor/nodeHashjoin.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/atomics.h"
#include "port/pg_bitutils.h"
#include "utils/dynahash.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static void ExecHashIncreaseNumBatches(HashJoinTable hashtable);
static void ExecHashIncreaseNumBuckets(HashJoinTable hashtable);
static void ExecParallelHashIncreaseNumBatches(HashJoinTable hashtable);
static void ExecParallelHashIncreaseNumBuckets(HashJoinTable hashtable);
static void ExecHashBuildSkewHash(HashJoinTable hashtable, Hash *node,
								  int mcvsToUse);
static void ExecHashSkewTableInsert(HashJoinTable hashtable,
									TupleTableSlot *slot,
									uint32 hashvalue,
									int bucketNumber);
static void ExecHashRemoveNextSkewBucket(HashJoinTable hashtable);

static void *dense_alloc(HashJoinTable hashtable, Size size);
static HashJoinTuple ExecParallelHashTupleAlloc(HashJoinTable hashtable,
												size_t size,
												dsa_pointer *shared);
static void MultiExecPrivateHash(HashState *node);
static void MultiExecParallelHash(HashState *node);
static inline HashJoinTuple ExecParallelHashFirstTuple(HashJoinTable table,
													   int bucketno);
static inline HashJoinTuple ExecParallelHashNextTuple(HashJoinTable table,
													  HashJoinTuple tuple);
static inline void ExecParallelHashPushTuple(dsa_pointer_atomic *head,
											 HashJoinTuple tuple,
											 dsa_pointer tuple_shared);
static void ExecParallelHashJoinSetUpBatches(HashJoinTable hashtable, int nbatch);
static void ExecParallelHashEnsureBatchAccessors(HashJoinTable hashtable);
static void ExecParallelHashRepartitionFirst(HashJoinTable hashtable);
static void ExecParallelHashRepartitionRest(HashJoinTable hashtable);
static HashMemoryChunk ExecParallelHashPopChunkQueue(HashJoinTable table,
													 dsa_pointer *shared);
static bool ExecParallelHashTuplePrealloc(HashJoinTable hashtable,
										  int batchno,
										  size_t size);
static void ExecParallelHashMergeCounters(HashJoinTable hashtable);
static void ExecParallelHashCloseBatchAccessors(HashJoinTable hashtable);


/* ----------------------------------------------------------------
 *		ExecHash
 *
 *		stub for pro forma compliance
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecHash(PlanState *pstate)
{
	elog(ERROR, "Hash node does not support ExecProcNode call convention");
	return NULL;
}

/* ----------------------------------------------------------------
 *		MultiExecHash
 *
 *		build hash table for hashjoin, doing partitioning if more
 *		than one batch is required.
 * ----------------------------------------------------------------
 */
Node *
MultiExecHash(HashState *node)
{
	/* must provide our own instrumentation support */
	if (node->ps.instrument)
		InstrStartNode(node->ps.instrument);

	if (node->parallel_state != NULL)
		MultiExecParallelHash(node);
	else
		MultiExecPrivateHash(node);

	/* must provide our own instrumentation support */
	if (node->ps.instrument)
		InstrStopNode(node->ps.instrument, node->hashtable->partialTuples);

	/*
	 * We do not return the hash table directly because it's not a subtype of
	 * Node, and so would violate the MultiExecProcNode API.  Instead, our
	 * parent Hashjoin node is expected to know how to fish it out of our node
	 * state.  Ugly but not really worth cleaning up, since Hashjoin knows
	 * quite a bit more about Hash besides that.
	 */
	return NULL;
}

/* ----------------------------------------------------------------
 *		MultiExecPrivateHash
 *
 *		parallel-oblivious version, building a backend-private
 *		hash table and (if necessary) batch files.
 * ----------------------------------------------------------------
 */
static void
MultiExecPrivateHash(HashState *node)
{
	PlanState  *outerNode;
	List	   *hashkeys;
	HashJoinTable hashtable;
	TupleTableSlot *slot;
	ExprContext *econtext;
	uint32		hashvalue;

	/*
	 * get state info from node
	 */
	outerNode = outerPlanState(node);
	hashtable = node->hashtable;

	/*
	 * set expression context
	 */
	hashkeys = node->hashkeys;
	econtext = node->ps.ps_ExprContext;

	/*
	 * Get all tuples from the node below the Hash node and insert into the
	 * hash table (or temp files).
	 */
	for (;;)
	{
		slot = ExecProcNode(outerNode);
		if (TupIsNull(slot))
			break;
		/* We have to compute the hash value */
		econtext->ecxt_outertuple = slot;
		if (ExecHashGetHashValue(hashtable, econtext, hashkeys,
								 false, hashtable->keepNulls,
								 &hashvalue))
		{
			int			bucketNumber;

			bucketNumber = ExecHashGetSkewBucket(hashtable, hashvalue);
			if (bucketNumber != INVALID_SKEW_BUCKET_NO)
			{
				/* It's a skew tuple, so put it into that hash table */
				ExecHashSkewTableInsert(hashtable, slot, hashvalue,
										bucketNumber);
				hashtable->skewTuples += 1;
			}
			else
			{
				/* Not subject to skew optimization, so insert normally */
				ExecHashTableInsert(hashtable, slot, hashvalue);
			}
			hashtable->totalTuples += 1;
		}
	}

	/* resize the hash table if needed (NTUP_PER_BUCKET exceeded) */
	if (hashtable->nbuckets != hashtable->nbuckets_optimal)
		ExecHashIncreaseNumBuckets(hashtable);

	/* Account for the buckets in spaceUsed (reported in EXPLAIN ANALYZE) */
	hashtable->spaceUsed += hashtable->nbuckets * sizeof(HashJoinTuple);
	if (hashtable->spaceUsed > hashtable->spacePeak)
		hashtable->spacePeak = hashtable->spaceUsed;

	hashtable->partialTuples = hashtable->totalTuples;
}

/* ----------------------------------------------------------------
 *		MultiExecParallelHash
 *
 *		parallel-aware version, building a shared hash table and
 *		(if necessary) batch files using the combined effort of
 *		a set of co-operating backends.
 * ----------------------------------------------------------------
 */
static void
MultiExecParallelHash(HashState *node)
{
	ParallelHashJoinState *pstate;
	PlanState  *outerNode;
	List	   *hashkeys;
	HashJoinTable hashtable;
	TupleTableSlot *slot;
	ExprContext *econtext;
	uint32		hashvalue;
	Barrier    *build_barrier;
	int			i;

	/*
	 * get state info from node
	 */
	outerNode = outerPlanState(node);
	hashtable = node->hashtable;

	/*
	 * set expression context
	 */
	hashkeys = node->hashkeys;
	econtext = node->ps.ps_ExprContext;

	/*
	 * Synchronize the parallel hash table build.  At this stage we know that
	 * the shared hash table has been or is being set up by
	 * ExecHashTableCreate(), but we don't know if our peers have returned
	 * from there or are here in MultiExecParallelHash(), and if so how far
	 * through they are.  To find out, we check the build_barrier phase then
	 * and jump to the right step in the build algorithm.
	 */
	pstate = hashtable->parallel_state;
	build_barrier = &pstate->build_barrier;
	Assert(BarrierPhase(build_barrier) >= PHJ_BUILD_ALLOCATING);
	switch (BarrierPhase(build_barrier))
	{
		case PHJ_BUILD_ALLOCATING:

			/*
			 * Either I just allocated the initial hash table in
			 * ExecHashTableCreate(), or someone else is doing that.  Either
			 * way, wait for everyone to arrive here so we can proceed.
			 */
			BarrierArriveAndWait(build_barrier, WAIT_EVENT_HASH_BUILD_ALLOCATING);
			/* Fall through. */

		case PHJ_BUILD_HASHING_INNER:

			/*
			 * It's time to begin hashing, or if we just arrived here then
			 * hashing is already underway, so join in that effort.  While
			 * hashing we have to be prepared to help increase the number of
			 * batches or buckets at any time, and if we arrived here when
			 * that was already underway we'll have to help complete that work
			 * immediately so that it's safe to access batches and buckets
			 * below.
			 */
			if (PHJ_GROW_BATCHES_PHASE(BarrierAttach(&pstate->grow_batches_barrier)) !=
				PHJ_GROW_BATCHES_ELECTING)
				ExecParallelHashIncreaseNumBatches(hashtable);
			if (PHJ_GROW_BUCKETS_PHASE(BarrierAttach(&pstate->grow_buckets_barrier)) !=
				PHJ_GROW_BUCKETS_ELECTING)
				ExecParallelHashIncreaseNumBuckets(hashtable);
			ExecParallelHashEnsureBatchAccessors(hashtable);
			ExecParallelHashTableSetCurrentBatch(hashtable, 0);
			for (;;)
			{
				slot = ExecProcNode(outerNode);
				if (TupIsNull(slot))
					break;
				econtext->ecxt_outertuple = slot;
				if (ExecHashGetHashValue(hashtable, econtext, hashkeys,
										 false, hashtable->keepNulls,
										 &hashvalue))
					ExecParallelHashTableInsert(hashtable, slot, hashvalue);
				hashtable->partialTuples++;
			}

			/*
			 * Make sure that any tuples we wrote to disk are visible to
			 * others before anyone tries to load them.
			 */
			for (i = 0; i < hashtable->nbatch; ++i)
				sts_end_write(hashtable->batches[i].inner_tuples);

			/*
			 * Update shared counters.  We need an accurate total tuple count
			 * to control the empty table optimization.
			 */
			ExecParallelHashMergeCounters(hashtable);

			BarrierDetach(&pstate->grow_buckets_barrier);
			BarrierDetach(&pstate->grow_batches_barrier);

			/*
			 * Wait for everyone to finish building and flushing files and
			 * counters.
			 */
			if (BarrierArriveAndWait(build_barrier,
									 WAIT_EVENT_HASH_BUILD_HASHING_INNER))
			{
				/*
				 * Elect one backend to disable any further growth.  Batches
				 * are now fixed.  While building them we made sure they'd fit
				 * in our memory budget when we load them back in later (or we
				 * tried to do that and gave up because we detected extreme
				 * skew).
				 */
				pstate->growth = PHJ_GROWTH_DISABLED;
			}
	}

	/*
	 * We're not yet attached to a batch.  We all agree on the dimensions and
	 * number of inner tuples (for the empty table optimization).
	 */
	hashtable->curbatch = -1;
	hashtable->nbuckets = pstate->nbuckets;
	hashtable->log2_nbuckets = my_log2(hashtable->nbuckets);
	hashtable->totalTuples = pstate->total_tuples;

	/*
	 * Unless we're completely done and the batch state has been freed, make
	 * sure we have accessors.
	 */
	if (BarrierPhase(build_barrier) < PHJ_BUILD_DONE)
		ExecParallelHashEnsureBatchAccessors(hashtable);

	/*
	 * The next synchronization point is in ExecHashJoin's HJ_BUILD_HASHTABLE
	 * case, which will bring the build phase to PHJ_BUILD_RUNNING (if it
	 * isn't there already).
	 */
	Assert(BarrierPhase(build_barrier) == PHJ_BUILD_HASHING_OUTER ||
		   BarrierPhase(build_barrier) == PHJ_BUILD_RUNNING ||
		   BarrierPhase(build_barrier) == PHJ_BUILD_DONE);
}

/* ----------------------------------------------------------------
 *		ExecInitHash
 *
 *		Init routine for Hash node
 * ----------------------------------------------------------------
 */
HashState *
ExecInitHash(Hash *node, EState *estate, int eflags)
{
	HashState  *hashstate;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * create state structure
	 */
	hashstate = makeNode(HashState);
	hashstate->ps.plan = (Plan *) node;
	hashstate->ps.state = estate;
	hashstate->ps.ExecProcNode = ExecHash;
	hashstate->hashtable = NULL;
	hashstate->hashkeys = NIL;	/* will be set by parent HashJoin */

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &hashstate->ps);

	/*
	 * initialize child nodes
	 */
	outerPlanState(hashstate) = ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * initialize our result slot and type. No need to build projection
	 * because this node doesn't do projections.
	 */
	ExecInitResultTupleSlotTL(&hashstate->ps, &TTSOpsMinimalTuple);
	hashstate->ps.ps_ProjInfo = NULL;

	/*
	 * initialize child expressions
	 */
	Assert(node->plan.qual == NIL);
	hashstate->hashkeys =
		ExecInitExprList(node->hashkeys, (PlanState *) hashstate);

	return hashstate;
}

/* ---------------------------------------------------------------
 *		ExecEndHash
 *
 *		clean up routine for Hash node
 * ----------------------------------------------------------------
 */
void
ExecEndHash(HashState *node)
{
	PlanState  *outerPlan;

	/*
	 * free exprcontext
	 */
	ExecFreeExprContext(&node->ps);

	/*
	 * shut down the subplan
	 */
	outerPlan = outerPlanState(node);
	ExecEndNode(outerPlan);
}


/* ----------------------------------------------------------------
 *		ExecHashTableCreate
 *
 *		create an empty hashtable data structure for hashjoin.
 * ----------------------------------------------------------------
 */
HashJoinTable
ExecHashTableCreate(HashState *state, List *hashOperators, List *hashCollations, bool keepNulls)
{
	Hash	   *node;
	HashJoinTable hashtable;
	Plan	   *outerNode;
	size_t		space_allowed;
	int			nbuckets;
	int			nbatch;
	double		rows;
	int			num_skew_mcvs;
	int			log2_nbuckets;
	int			nkeys;
	int			i;
	ListCell   *ho;
	ListCell   *hc;
	MemoryContext oldcxt;

	/*
	 * Get information about the size of the relation to be hashed (it's the
	 * "outer" subtree of this node, but the inner relation of the hashjoin).
	 * Compute the appropriate size of the hash table.
	 */
	node = (Hash *) state->ps.plan;
	outerNode = outerPlan(node);

	/*
	 * If this is shared hash table with a partial plan, then we can't use
	 * outerNode->plan_rows to estimate its size.  We need an estimate of the
	 * total number of rows across all copies of the partial plan.
	 */
	rows = node->plan.parallel_aware ? node->rows_total : outerNode->plan_rows;

	ExecChooseHashTableSize(rows, outerNode->plan_width,
							OidIsValid(node->skewTable),
							state->parallel_state != NULL,
							state->parallel_state != NULL ?
							state->parallel_state->nparticipants - 1 : 0,
							&space_allowed,
							&nbuckets, &nbatch, &num_skew_mcvs);

	/* nbuckets must be a power of 2 */
	log2_nbuckets = my_log2(nbuckets);
	Assert(nbuckets == (1 << log2_nbuckets));

	/*
	 * Initialize the hash table control block.
	 *
	 * The hashtable control block is just palloc'd from the executor's
	 * per-query memory context.  Everything else should be kept inside the
	 * subsidiary hashCxt or batchCxt.
	 */
	hashtable = (HashJoinTable) palloc(sizeof(HashJoinTableData));
	hashtable->nbuckets = nbuckets;
	hashtable->nbuckets_original = nbuckets;
	hashtable->nbuckets_optimal = nbuckets;
	hashtable->log2_nbuckets = log2_nbuckets;
	hashtable->log2_nbuckets_optimal = log2_nbuckets;
	hashtable->buckets.unshared = NULL;
	hashtable->keepNulls = keepNulls;
	hashtable->skewEnabled = false;
	hashtable->skewBucket = NULL;
	hashtable->skewBucketLen = 0;
	hashtable->nSkewBuckets = 0;
	hashtable->skewBucketNums = NULL;
	hashtable->nbatch = nbatch;
	hashtable->curbatch = 0;
	hashtable->nbatch_original = nbatch;
	hashtable->nbatch_outstart = nbatch;
	hashtable->growEnabled = true;
	hashtable->totalTuples = 0;
	hashtable->partialTuples = 0;
	hashtable->skewTuples = 0;
	hashtable->innerBatchFile = NULL;
	hashtable->outerBatchFile = NULL;
	hashtable->spaceUsed = 0;
	hashtable->spacePeak = 0;
	hashtable->spaceAllowed = space_allowed;
	hashtable->spaceUsedSkew = 0;
	hashtable->spaceAllowedSkew =
		hashtable->spaceAllowed * SKEW_WORK_MEM_PERCENT / 100;
	hashtable->chunks = NULL;
	hashtable->current_chunk = NULL;
	hashtable->parallel_state = state->parallel_state;
	hashtable->area = state->ps.state->es_query_dsa;
	hashtable->batches = NULL;

#ifdef HJDEBUG
	printf("Hashjoin %p: initial nbatch = %d, nbuckets = %d\n",
		   hashtable, nbatch, nbuckets);
#endif

	/*
	 * Create temporary memory contexts in which to keep the hashtable working
	 * storage.  See notes in executor/hashjoin.h.
	 */
	hashtable->hashCxt = AllocSetContextCreate(CurrentMemoryContext,
											   "HashTableContext",
											   ALLOCSET_DEFAULT_SIZES);

	hashtable->batchCxt = AllocSetContextCreate(hashtable->hashCxt,
												"HashBatchContext",
												ALLOCSET_DEFAULT_SIZES);

	/* Allocate data that will live for the life of the hashjoin */

	oldcxt = MemoryContextSwitchTo(hashtable->hashCxt);

	/*
	 * Get info about the hash functions to be used for each hash key. Also
	 * remember whether the join operators are strict.
	 */
	nkeys = list_length(hashOperators);
	hashtable->outer_hashfunctions =
		(FmgrInfo *) palloc(nkeys * sizeof(FmgrInfo));
	hashtable->inner_hashfunctions =
		(FmgrInfo *) palloc(nkeys * sizeof(FmgrInfo));
	hashtable->hashStrict = (bool *) palloc(nkeys * sizeof(bool));
	hashtable->collations = (Oid *) palloc(nkeys * sizeof(Oid));
	i = 0;
	forboth(ho, hashOperators, hc, hashCollations)
	{
		Oid			hashop = lfirst_oid(ho);
		Oid			left_hashfn;
		Oid			right_hashfn;

		if (!get_op_hash_functions(hashop, &left_hashfn, &right_hashfn))
			elog(ERROR, "could not find hash function for hash operator %u",
				 hashop);
		fmgr_info(left_hashfn, &hashtable->outer_hashfunctions[i]);
		fmgr_info(right_hashfn, &hashtable->inner_hashfunctions[i]);
		hashtable->hashStrict[i] = op_strict(hashop);
		hashtable->collations[i] = lfirst_oid(hc);
		i++;
	}

	if (nbatch > 1 && hashtable->parallel_state == NULL)
	{
		/*
		 * allocate and initialize the file arrays in hashCxt (not needed for
		 * parallel case which uses shared tuplestores instead of raw files)
		 */
		hashtable->innerBatchFile = (BufFile **)
			palloc0(nbatch * sizeof(BufFile *));
		hashtable->outerBatchFile = (BufFile **)
			palloc0(nbatch * sizeof(BufFile *));
		/* The files will not be opened until needed... */
		/* ... but make sure we have temp tablespaces established for them */
		PrepareTempTablespaces();
	}

	MemoryContextSwitchTo(oldcxt);

	if (hashtable->parallel_state)
	{
		ParallelHashJoinState *pstate = hashtable->parallel_state;
		Barrier    *build_barrier;

		/*
		 * Attach to the build barrier.  The corresponding detach operation is
		 * in ExecHashTableDetach.  Note that we won't attach to the
		 * batch_barrier for batch 0 yet.  We'll attach later and start it out
		 * in PHJ_BATCH_PROBING phase, because batch 0 is allocated up front
		 * and then loaded while hashing (the standard hybrid hash join
		 * algorithm), and we'll coordinate that using build_barrier.
		 */
		build_barrier = &pstate->build_barrier;
		BarrierAttach(build_barrier);

		/*
		 * So far we have no idea whether there are any other participants,
		 * and if so, what phase they are working on.  The only thing we care
		 * about at this point is whether someone has already created the
		 * SharedHashJoinBatch objects and the hash table for batch 0.  One
		 * backend will be elected to do that now if necessary.
		 */
		if (BarrierPhase(build_barrier) == PHJ_BUILD_ELECTING &&
			BarrierArriveAndWait(build_barrier, WAIT_EVENT_HASH_BUILD_ELECTING))
		{
			pstate->nbatch = nbatch;
			pstate->space_allowed = space_allowed;
			pstate->growth = PHJ_GROWTH_OK;

			/* Set up the shared state for coordinating batches. */
			ExecParallelHashJoinSetUpBatches(hashtable, nbatch);

			/*
			 * Allocate batch 0's hash table up front so we can load it
			 * directly while hashing.
			 */
			pstate->nbuckets = nbuckets;
			ExecParallelHashTableAlloc(hashtable, 0);
		}

		/*
		 * The next Parallel Hash synchronization point is in
		 * MultiExecParallelHash(), which will progress it all the way to
		 * PHJ_BUILD_RUNNING.  The caller must not return control from this
		 * executor node between now and then.
		 */
	}
	else
	{
		/*
		 * Prepare context for the first-scan space allocations; allocate the
		 * hashbucket array therein, and set each bucket "empty".
		 */
		MemoryContextSwitchTo(hashtable->batchCxt);

		hashtable->buckets.unshared = (HashJoinTuple *)
			palloc0(nbuckets * sizeof(HashJoinTuple));

		/*
		 * Set up for skew optimization, if possible and there's a need for
		 * more than one batch.  (In a one-batch join, there's no point in
		 * it.)
		 */
		if (nbatch > 1)
			ExecHashBuildSkewHash(hashtable, node, num_skew_mcvs);

		MemoryContextSwitchTo(oldcxt);
	}

	return hashtable;
}


/*
 * Compute appropriate size for hashtable given the estimated size of the
 * relation to be hashed (number of rows and average row width).
 *
 * This is exported so that the planner's costsize.c can use it.
 */

/* Target bucket loading (tuples per bucket) */
#define NTUP_PER_BUCKET			1

void
ExecChooseHashTableSize(double ntuples, int tupwidth, bool useskew,
						bool try_combined_work_mem,
						int parallel_workers,
						size_t *space_allowed,
						int *numbuckets,
						int *numbatches,
						int *num_skew_mcvs)
{
	int			tupsize;
	double		inner_rel_bytes;
	long		bucket_bytes;
	long		hash_table_bytes;
	long		skew_table_bytes;
	long		max_pointers;
	long		mppow2;
	int			nbatch = 1;
	int			nbuckets;
	double		dbuckets;

	/* Force a plausible relation size if no info */
	if (ntuples <= 0.0)
		ntuples = 1000.0;

	/*
	 * Estimate tupsize based on footprint of tuple in hashtable... note this
	 * does not allow for any palloc overhead.  The manipulations of spaceUsed
	 * don't count palloc overhead either.
	 */
	tupsize = HJTUPLE_OVERHEAD +
		MAXALIGN(SizeofMinimalTupleHeader) +
		MAXALIGN(tupwidth);
	inner_rel_bytes = ntuples * tupsize;

	/*
	 * Target in-memory hashtable size is work_mem kilobytes.
	 */
	hash_table_bytes = work_mem * 1024L;

	/*
	 * Parallel Hash tries to use the combined work_mem of all workers to
	 * avoid the need to batch.  If that won't work, it falls back to work_mem
	 * per worker and tries to process batches in parallel.
	 */
	if (try_combined_work_mem)
		hash_table_bytes += hash_table_bytes * parallel_workers;

	*space_allowed = hash_table_bytes;

	/*
	 * If skew optimization is possible, estimate the number of skew buckets
	 * that will fit in the memory allowed, and decrement the assumed space
	 * available for the main hash table accordingly.
	 *
	 * We make the optimistic assumption that each skew bucket will contain
	 * one inner-relation tuple.  If that turns out to be low, we will recover
	 * at runtime by reducing the number of skew buckets.
	 *
	 * hashtable->skewBucket will have up to 8 times as many HashSkewBucket
	 * pointers as the number of MCVs we allow, since ExecHashBuildSkewHash
	 * will round up to the next power of 2 and then multiply by 4 to reduce
	 * collisions.
	 */
	if (useskew)
	{
		skew_table_bytes = hash_table_bytes * SKEW_WORK_MEM_PERCENT / 100;

		/*----------
		 * Divisor is:
		 * size of a hash tuple +
		 * worst-case size of skewBucket[] per MCV +
		 * size of skewBucketNums[] entry +
		 * size of skew bucket struct itself
		 *----------
		 */
		*num_skew_mcvs = skew_table_bytes / (tupsize +
											 (8 * sizeof(HashSkewBucket *)) +
											 sizeof(int) +
											 SKEW_BUCKET_OVERHEAD);
		if (*num_skew_mcvs > 0)
			hash_table_bytes -= skew_table_bytes;
	}
	else
		*num_skew_mcvs = 0;

	/*
	 * Set nbuckets to achieve an average bucket load of NTUP_PER_BUCKET when
	 * memory is filled, assuming a single batch; but limit the value so that
	 * the pointer arrays we'll try to allocate do not exceed work_mem nor
	 * MaxAllocSize.
	 *
	 * Note that both nbuckets and nbatch must be powers of 2 to make
	 * ExecHashGetBucketAndBatch fast.
	 */
	max_pointers = *space_allowed / sizeof(HashJoinTuple);
	max_pointers = Min(max_pointers, MaxAllocSize / sizeof(HashJoinTuple));
	/* If max_pointers isn't a power of 2, must round it down to one */
	mppow2 = 1L << my_log2(max_pointers);
	if (max_pointers != mppow2)
		max_pointers = mppow2 / 2;

	/* Also ensure we avoid integer overflow in nbatch and nbuckets */
	/* (this step is redundant given the current value of MaxAllocSize) */
	max_pointers = Min(max_pointers, INT_MAX / 2);

	dbuckets = ceil(ntuples / NTUP_PER_BUCKET);
	dbuckets = Min(dbuckets, max_pointers);
	nbuckets = (int) dbuckets;
	/* don't let nbuckets be really small, though ... */
	nbuckets = Max(nbuckets, 1024);
	/* ... and force it to be a power of 2. */
	nbuckets = 1 << my_log2(nbuckets);

	/*
	 * If there's not enough space to store the projected number of tuples and
	 * the required bucket headers, we will need multiple batches.
	 */
	bucket_bytes = sizeof(HashJoinTuple) * nbuckets;
	if (inner_rel_bytes + bucket_bytes > hash_table_bytes)
	{
		/* We'll need multiple batches */
		long		lbuckets;
		double		dbatch;
		int			minbatch;
		long		bucket_size;

		/*
		 * If Parallel Hash with combined work_mem would still need multiple
		 * batches, we'll have to fall back to regular work_mem budget.
		 */
		if (try_combined_work_mem)
		{
			ExecChooseHashTableSize(ntuples, tupwidth, useskew,
									false, parallel_workers,
									space_allowed,
									numbuckets,
									numbatches,
									num_skew_mcvs);
			return;
		}

		/*
		 * Estimate the number of buckets we'll want to have when work_mem is
		 * entirely full.  Each bucket will contain a bucket pointer plus
		 * NTUP_PER_BUCKET tuples, whose projected size already includes
		 * overhead for the hash code, pointer to the next tuple, etc.
		 */
		bucket_size = (tupsize * NTUP_PER_BUCKET + sizeof(HashJoinTuple));
		lbuckets = 1L << my_log2(hash_table_bytes / bucket_size);
		lbuckets = Min(lbuckets, max_pointers);
		nbuckets = (int) lbuckets;
		nbuckets = 1 << my_log2(nbuckets);
		bucket_bytes = nbuckets * sizeof(HashJoinTuple);

		/*
		 * Buckets are simple pointers to hashjoin tuples, while tupsize
		 * includes the pointer, hash code, and MinimalTupleData.  So buckets
		 * should never really exceed 25% of work_mem (even for
		 * NTUP_PER_BUCKET=1); except maybe for work_mem values that are not
		 * 2^N bytes, where we might get more because of doubling. So let's
		 * look for 50% here.
		 */
		Assert(bucket_bytes <= hash_table_bytes / 2);

		/* Calculate required number of batches. */
		dbatch = ceil(inner_rel_bytes / (hash_table_bytes - bucket_bytes));
		dbatch = Min(dbatch, max_pointers);
		minbatch = (int) dbatch;
		nbatch = 2;
		while (nbatch < minbatch)
			nbatch <<= 1;
	}

	Assert(nbuckets > 0);
	Assert(nbatch > 0);

	*numbuckets = nbuckets;
	*numbatches = nbatch;
}


/* ----------------------------------------------------------------
 *		ExecHashTableDestroy
 *
 *		destroy a hash table
 * ----------------------------------------------------------------
 */
void
ExecHashTableDestroy(HashJoinTable hashtable)
{
	int			i;

	/*
	 * Make sure all the temp files are closed.  We skip batch 0, since it
	 * can't have any temp files (and the arrays might not even exist if
	 * nbatch is only 1).  Parallel hash joins don't use these files.
	 */
	if (hashtable->innerBatchFile != NULL)
	{
		for (i = 1; i < hashtable->nbatch; i++)
		{
			if (hashtable->innerBatchFile[i])
				BufFileClose(hashtable->innerBatchFile[i]);
			if (hashtable->outerBatchFile[i])
				BufFileClose(hashtable->outerBatchFile[i]);
		}
	}

	/* Release working memory (batchCxt is a child, so it goes away too) */
	MemoryContextDelete(hashtable->hashCxt);

	/* And drop the control block */
	pfree(hashtable);
}

/*
 * ExecHashIncreaseNumBatches
 *		increase the original number of batches in order to reduce
 *		current memory consumption
 */
static void
ExecHashIncreaseNumBatches(HashJoinTable hashtable)
{
	int			oldnbatch = hashtable->nbatch;
	int			curbatch = hashtable->curbatch;
	int			nbatch;
	MemoryContext oldcxt;
	long		ninmemory;
	long		nfreed;
	HashMemoryChunk oldchunks;

	/* do nothing if we've decided to shut off growth */
	if (!hashtable->growEnabled)
		return;

	/* safety check to avoid overflow */
	if (oldnbatch > Min(INT_MAX / 2, MaxAllocSize / (sizeof(void *) * 2)))
		return;

	nbatch = oldnbatch * 2;
	Assert(nbatch > 1);

#ifdef HJDEBUG
	printf("Hashjoin %p: increasing nbatch to %d because space = %zu\n",
		   hashtable, nbatch, hashtable->spaceUsed);
#endif

	oldcxt = MemoryContextSwitchTo(hashtable->hashCxt);

	if (hashtable->innerBatchFile == NULL)
	{
		/* we had no file arrays before */
		hashtable->innerBatchFile = (BufFile **)
			palloc0(nbatch * sizeof(BufFile *));
		hashtable->outerBatchFile = (BufFile **)
			palloc0(nbatch * sizeof(BufFile *));
		/* time to establish the temp tablespaces, too */
		PrepareTempTablespaces();
	}
	else
	{
		/* enlarge arrays and zero out added entries */
		hashtable->innerBatchFile = (BufFile **)
			repalloc(hashtable->innerBatchFile, nbatch * sizeof(BufFile *));
		hashtable->outerBatchFile = (BufFile **)
			repalloc(hashtable->outerBatchFile, nbatch * sizeof(BufFile *));
		MemSet(hashtable->innerBatchFile + oldnbatch, 0,
			   (nbatch - oldnbatch) * sizeof(BufFile *));
		MemSet(hashtable->outerBatchFile + oldnbatch, 0,
			   (nbatch - oldnbatch) * sizeof(BufFile *));
	}

	MemoryContextSwitchTo(oldcxt);

	hashtable->nbatch = nbatch;

	/*
	 * Scan through the existing hash table entries and dump out any that are
	 * no longer of the current batch.
	 */
	ninmemory = nfreed = 0;

	/* If know we need to resize nbuckets, we can do it while rebatching. */
	if (hashtable->nbuckets_optimal != hashtable->nbuckets)
	{
		/* we never decrease the number of buckets */
		Assert(hashtable->nbuckets_optimal > hashtable->nbuckets);

		hashtable->nbuckets = hashtable->nbuckets_optimal;
		hashtable->log2_nbuckets = hashtable->log2_nbuckets_optimal;

		hashtable->buckets.unshared =
			repalloc(hashtable->buckets.unshared,
					 sizeof(HashJoinTuple) * hashtable->nbuckets);
	}

	/*
	 * We will scan through the chunks directly, so that we can reset the
	 * buckets now and not have to keep track which tuples in the buckets have
	 * already been processed. We will free the old chunks as we go.
	 */
	memset(hashtable->buckets.unshared, 0,
		   sizeof(HashJoinTuple) * hashtable->nbuckets);
	oldchunks = hashtable->chunks;
	hashtable->chunks = NULL;

	/* so, let's scan through the old chunks, and all tuples in each chunk */
	while (oldchunks != NULL)
	{
		HashMemoryChunk nextchunk = oldchunks->next.unshared;

		/* position within the buffer (up to oldchunks->used) */
		size_t		idx = 0;

		/* process all tuples stored in this chunk (and then free it) */
		while (idx < oldchunks->used)
		{
			HashJoinTuple hashTuple = (HashJoinTuple) (HASH_CHUNK_DATA(oldchunks) + idx);
			MinimalTuple tuple = HJTUPLE_MINTUPLE(hashTuple);
			int			hashTupleSize = (HJTUPLE_OVERHEAD + tuple->t_len);
			int			bucketno;
			int			batchno;

			ninmemory++;
			ExecHashGetBucketAndBatch(hashtable, hashTuple->hashvalue,
									  &bucketno, &batchno);

			if (batchno == curbatch)
			{
				/* keep tuple in memory - copy it into the new chunk */
				HashJoinTuple copyTuple;

				copyTuple = (HashJoinTuple) dense_alloc(hashtable, hashTupleSize);
				memcpy(copyTuple, hashTuple, hashTupleSize);

				/* and add it back to the appropriate bucket */
				copyTuple->next.unshared = hashtable->buckets.unshared[bucketno];
				hashtable->buckets.unshared[bucketno] = copyTuple;
			}
			else
			{
				/* dump it out */
				Assert(batchno > curbatch);
				ExecHashJoinSaveTuple(HJTUPLE_MINTUPLE(hashTuple),
									  hashTuple->hashvalue,
									  &hashtable->innerBatchFile[batchno]);

				hashtable->spaceUsed -= hashTupleSize;
				nfreed++;
			}

			/* next tuple in this chunk */
			idx += MAXALIGN(hashTupleSize);

			/* allow this loop to be cancellable */
			CHECK_FOR_INTERRUPTS();
		}

		/* we're done with this chunk - free it and proceed to the next one */
		pfree(oldchunks);
		oldchunks = nextchunk;
	}

#ifdef HJDEBUG
	printf("Hashjoin %p: freed %ld of %ld tuples, space now %zu\n",
		   hashtable, nfreed, ninmemory, hashtable->spaceUsed);
#endif

	/*
	 * If we dumped out either all or none of the tuples in the table, disable
	 * further expansion of nbatch.  This situation implies that we have
	 * enough tuples of identical hashvalues to overflow spaceAllowed.
	 * Increasing nbatch will not fix it since there's no way to subdivide the
	 * group any more finely. We have to just gut it out and hope the server
	 * has enough RAM.
	 */
	if (nfreed == 0 || nfreed == ninmemory)
	{
		hashtable->growEnabled = false;
#ifdef HJDEBUG
		printf("Hashjoin %p: disabling further increase of nbatch\n",
			   hashtable);
#endif
	}
}

/*
 * ExecParallelHashIncreaseNumBatches
 *		Every participant attached to grow_batches_barrier must run this
 *		function when it observes growth == PHJ_GROWTH_NEED_MORE_BATCHES.
 */
static void
ExecParallelHashIncreaseNumBatches(HashJoinTable hashtable)
{
	ParallelHashJoinState *pstate = hashtable->parallel_state;
	int			i;

	Assert(BarrierPhase(&pstate->build_barrier) == PHJ_BUILD_HASHING_INNER);

	/*
	 * It's unlikely, but we need to be prepared for new participants to show
	 * up while we're in the middle of this operation so we need to switch on
	 * barrier phase here.
	 */
	switch (PHJ_GROW_BATCHES_PHASE(BarrierPhase(&pstate->grow_batches_barrier)))
	{
		case PHJ_GROW_BATCHES_ELECTING:

			/*
			 * Elect one participant to prepare to grow the number of batches.
			 * This involves reallocating or resetting the buckets of batch 0
			 * in preparation for all participants to begin repartitioning the
			 * tuples.
			 */
			if (BarrierArriveAndWait(&pstate->grow_batches_barrier,
									 WAIT_EVENT_HASH_GROW_BATCHES_ELECTING))
			{
				dsa_pointer_atomic *buckets;
				ParallelHashJoinBatch *old_batch0;
				int			new_nbatch;
				int			i;

				/* Move the old batch out of the way. */
				old_batch0 = hashtable->batches[0].shared;
				pstate->old_batches = pstate->batches;
				pstate->old_nbatch = hashtable->nbatch;
				pstate->batches = InvalidDsaPointer;

				/* Free this backend's old accessors. */
				ExecParallelHashCloseBatchAccessors(hashtable);

				/* Figure out how many batches to use. */
				if (hashtable->nbatch == 1)
				{
					/*
					 * We are going from single-batch to multi-batch.  We need
					 * to switch from one large combined memory budget to the
					 * regular work_mem budget.
					 */
					pstate->space_allowed = work_mem * 1024L;

					/*
					 * The combined work_mem of all participants wasn't
					 * enough. Therefore one batch per participant would be
					 * approximately equivalent and would probably also be
					 * insufficient.  So try two batches per participant,
					 * rounded up to a power of two.
					 */
					new_nbatch = 1 << my_log2(pstate->nparticipants * 2);
				}
				else
				{
					/*
					 * We were already multi-batched.  Try doubling the number
					 * of batches.
					 */
					new_nbatch = hashtable->nbatch * 2;
				}

				/* Allocate new larger generation of batches. */
				Assert(hashtable->nbatch == pstate->nbatch);
				ExecParallelHashJoinSetUpBatches(hashtable, new_nbatch);
				Assert(hashtable->nbatch == pstate->nbatch);

				/* Replace or recycle batch 0's bucket array. */
				if (pstate->old_nbatch == 1)
				{
					double		dtuples;
					double		dbuckets;
					int			new_nbuckets;
					uint32		max_buckets;

					/*
					 * We probably also need a smaller bucket array.  How many
					 * tuples do we expect per batch, assuming we have only
					 * half of them so far?  Normally we don't need to change
					 * the bucket array's size, because the size of each batch
					 * stays the same as we add more batches, but in this
					 * special case we move from a large batch to many smaller
					 * batches and it would be wasteful to keep the large
					 * array.
					 */
					dtuples = (old_batch0->ntuples * 2.0) / new_nbatch;

					/*
					 * We need to calculate the maximum number of buckets to
					 * stay within the MaxAllocSize boundary.  Round the
					 * maximum number to the previous power of 2 given that
					 * later we round the number to the next power of 2.
					 */
					max_buckets = MaxAllocSize / sizeof(dsa_pointer_atomic);
					if ((max_buckets & (max_buckets - 1)) != 0)
						max_buckets = 1 << (my_log2(max_buckets) - 1);
					dbuckets = ceil(dtuples / NTUP_PER_BUCKET);
					dbuckets = Min(dbuckets, max_buckets);
					new_nbuckets = (int) dbuckets;
					new_nbuckets = Max(new_nbuckets, 1024);
					new_nbuckets = 1 << my_log2(new_nbuckets);
					dsa_free(hashtable->area, old_batch0->buckets);
					hashtable->batches[0].shared->buckets =
						dsa_allocate(hashtable->area,
									 sizeof(dsa_pointer_atomic) * new_nbuckets);
					buckets = (dsa_pointer_atomic *)
						dsa_get_address(hashtable->area,
										hashtable->batches[0].shared->buckets);
					for (i = 0; i < new_nbuckets; ++i)
						dsa_pointer_atomic_init(&buckets[i], InvalidDsaPointer);
					pstate->nbuckets = new_nbuckets;
				}
				else
				{
					/* Recycle the existing bucket array. */
					hashtable->batches[0].shared->buckets = old_batch0->buckets;
					buckets = (dsa_pointer_atomic *)
						dsa_get_address(hashtable->area, old_batch0->buckets);
					for (i = 0; i < hashtable->nbuckets; ++i)
						dsa_pointer_atomic_write(&buckets[i], InvalidDsaPointer);
				}

				/* Move all chunks to the work queue for parallel processing. */
				pstate->chunk_work_queue = old_batch0->chunks;

				/* Disable further growth temporarily while we're growing. */
				pstate->growth = PHJ_GROWTH_DISABLED;
			}
			else
			{
				/* All other participants just flush their tuples to disk. */
				ExecParallelHashCloseBatchAccessors(hashtable);
			}
			/* Fall through. */

		case PHJ_GROW_BATCHES_ALLOCATING:
			/* Wait for the above to be finished. */
			BarrierArriveAndWait(&pstate->grow_batches_barrier,
								 WAIT_EVENT_HASH_GROW_BATCHES_ALLOCATING);
			/* Fall through. */

		case PHJ_GROW_BATCHES_REPARTITIONING:
			/* Make sure that we have the current dimensions and buckets. */
			ExecParallelHashEnsureBatchAccessors(hashtable);
			ExecParallelHashTableSetCurrentBatch(hashtable, 0);
			/* Then partition, flush counters. */
			ExecParallelHashRepartitionFirst(hashtable);
			ExecParallelHashRepartitionRest(hashtable);
			ExecParallelHashMergeCounters(hashtable);
			/* Wait for the above to be finished. */
			BarrierArriveAndWait(&pstate->grow_batches_barrier,
								 WAIT_EVENT_HASH_GROW_BATCHES_REPARTITIONING);
			/* Fall through. */

		case PHJ_GROW_BATCHES_DECIDING:

			/*
			 * Elect one participant to clean up and decide whether further
			 * repartitioning is needed, or should be disabled because it's
			 * not helping.
			 */
			if (BarrierArriveAndWait(&pstate->grow_batches_barrier,
									 WAIT_EVENT_HASH_GROW_BATCHES_DECIDING))
			{
				ParallelHashJoinBatch *old_batches;
				bool		space_exhausted = false;
				bool		extreme_skew_detected = false;

				/* Make sure that we have the current dimensions and buckets. */
				ExecParallelHashEnsureBatchAccessors(hashtable);
				ExecParallelHashTableSetCurrentBatch(hashtable, 0);

				old_batches = dsa_get_address(hashtable->area, pstate->old_batches);

				/* Are any of the new generation of batches exhausted? */
				for (i = 0; i < hashtable->nbatch; ++i)
				{
					ParallelHashJoinBatch *batch;
					ParallelHashJoinBatch *old_batch;
					int			parent;

					batch = hashtable->batches[i].shared;
					if (batch->space_exhausted ||
						batch->estimated_size > pstate->space_allowed)
						space_exhausted = true;

					parent = i % pstate->old_nbatch;
					old_batch = NthParallelHashJoinBatch(old_batches, parent);
					if (old_batch->space_exhausted ||
						batch->estimated_size > pstate->space_allowed)
					{
						/*
						 * Did this batch receive ALL of the tuples from its
						 * parent batch?  That would indicate that further
						 * repartitioning isn't going to help (the hash values
						 * are probably all the same).
						 */
						if (batch->ntuples == hashtable->batches[parent].shared->old_ntuples)
							extreme_skew_detected = true;
					}
				}

				/* Don't keep growing if it's not helping or we'd overflow. */
				if (extreme_skew_detected || hashtable->nbatch >= INT_MAX / 2)
					pstate->growth = PHJ_GROWTH_DISABLED;
				else if (space_exhausted)
					pstate->growth = PHJ_GROWTH_NEED_MORE_BATCHES;
				else
					pstate->growth = PHJ_GROWTH_OK;

				/* Free the old batches in shared memory. */
				dsa_free(hashtable->area, pstate->old_batches);
				pstate->old_batches = InvalidDsaPointer;
			}
			/* Fall through. */

		case PHJ_GROW_BATCHES_FINISHING:
			/* Wait for the above to complete. */
			BarrierArriveAndWait(&pstate->grow_batches_barrier,
								 WAIT_EVENT_HASH_GROW_BATCHES_FINISHING);
	}
}

/*
 * Repartition the tuples currently loaded into memory for inner batch 0
 * because the number of batches has been increased.  Some tuples are retained
 * in memory and some are written out to a later batch.
 */
static void
ExecParallelHashRepartitionFirst(HashJoinTable hashtable)
{
	dsa_pointer chunk_shared;
	HashMemoryChunk chunk;

	Assert(hashtable->nbatch == hashtable->parallel_state->nbatch);

	while ((chunk = ExecParallelHashPopChunkQueue(hashtable, &chunk_shared)))
	{
		size_t		idx = 0;

		/* Repartition all tuples in this chunk. */
		while (idx < chunk->used)
		{
			HashJoinTuple hashTuple = (HashJoinTuple) (HASH_CHUNK_DATA(chunk) + idx);
			MinimalTuple tuple = HJTUPLE_MINTUPLE(hashTuple);
			HashJoinTuple copyTuple;
			dsa_pointer shared;
			int			bucketno;
			int			batchno;

			ExecHashGetBucketAndBatch(hashtable, hashTuple->hashvalue,
									  &bucketno, &batchno);

			Assert(batchno < hashtable->nbatch);
			if (batchno == 0)
			{
				/* It still belongs in batch 0.  Copy to a new chunk. */
				copyTuple =
					ExecParallelHashTupleAlloc(hashtable,
											   HJTUPLE_OVERHEAD + tuple->t_len,
											   &shared);
				copyTuple->hashvalue = hashTuple->hashvalue;
				memcpy(HJTUPLE_MINTUPLE(copyTuple), tuple, tuple->t_len);
				ExecParallelHashPushTuple(&hashtable->buckets.shared[bucketno],
										  copyTuple, shared);
			}
			else
			{
				size_t		tuple_size =
				MAXALIGN(HJTUPLE_OVERHEAD + tuple->t_len);

				/* It belongs in a later batch. */
				hashtable->batches[batchno].estimated_size += tuple_size;
				sts_puttuple(hashtable->batches[batchno].inner_tuples,
							 &hashTuple->hashvalue, tuple);
			}

			/* Count this tuple. */
			++hashtable->batches[0].old_ntuples;
			++hashtable->batches[batchno].ntuples;

			idx += MAXALIGN(HJTUPLE_OVERHEAD +
							HJTUPLE_MINTUPLE(hashTuple)->t_len);
		}

		/* Free this chunk. */
		dsa_free(hashtable->area, chunk_shared);

		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * Help repartition inner batches 1..n.
 */
static void
ExecParallelHashRepartitionRest(HashJoinTable hashtable)
{
	ParallelHashJoinState *pstate = hashtable->parallel_state;
	int			old_nbatch = pstate->old_nbatch;
	SharedTuplestoreAccessor **old_inner_tuples;
	ParallelHashJoinBatch *old_batches;
	int			i;

	/* Get our hands on the previous generation of batches. */
	old_batches = (ParallelHashJoinBatch *)
		dsa_get_address(hashtable->area, pstate->old_batches);
	old_inner_tuples = palloc0(sizeof(SharedTuplestoreAccessor *) * old_nbatch);
	for (i = 1; i < old_nbatch; ++i)
	{
		ParallelHashJoinBatch *shared =
		NthParallelHashJoinBatch(old_batches, i);

		old_inner_tuples[i] = sts_attach(ParallelHashJoinBatchInner(shared),
										 ParallelWorkerNumber + 1,
										 &pstate->fileset);
	}

	/* Join in the effort to repartition them. */
	for (i = 1; i < old_nbatch; ++i)
	{
		MinimalTuple tuple;
		uint32		hashvalue;

		/* Scan one partition from the previous generation. */
		sts_begin_parallel_scan(old_inner_tuples[i]);
		while ((tuple = sts_parallel_scan_next(old_inner_tuples[i], &hashvalue)))
		{
			size_t		tuple_size = MAXALIGN(HJTUPLE_OVERHEAD + tuple->t_len);
			int			bucketno;
			int			batchno;

			/* Decide which partition it goes to in the new generation. */
			ExecHashGetBucketAndBatch(hashtable, hashvalue, &bucketno,
									  &batchno);

			hashtable->batches[batchno].estimated_size += tuple_size;
			++hashtable->batches[batchno].ntuples;
			++hashtable->batches[i].old_ntuples;

			/* Store the tuple its new batch. */
			sts_puttuple(hashtable->batches[batchno].inner_tuples,
						 &hashvalue, tuple);

			CHECK_FOR_INTERRUPTS();
		}
		sts_end_parallel_scan(old_inner_tuples[i]);
	}

	pfree(old_inner_tuples);
}

/*
 * Transfer the backend-local per-batch counters to the shared totals.
 */
static void
ExecParallelHashMergeCounters(HashJoinTable hashtable)
{
	ParallelHashJoinState *pstate = hashtable->parallel_state;
	int			i;

	LWLockAcquire(&pstate->lock, LW_EXCLUSIVE);
	pstate->total_tuples = 0;
	for (i = 0; i < hashtable->nbatch; ++i)
	{
		ParallelHashJoinBatchAccessor *batch = &hashtable->batches[i];

		batch->shared->size += batch->size;
		batch->shared->estimated_size += batch->estimated_size;
		batch->shared->ntuples += batch->ntuples;
		batch->shared->old_ntuples += batch->old_ntuples;
		batch->size = 0;
		batch->estimated_size = 0;
		batch->ntuples = 0;
		batch->old_ntuples = 0;
		pstate->total_tuples += batch->shared->ntuples;
	}
	LWLockRelease(&pstate->lock);
}

/*
 * ExecHashIncreaseNumBuckets
 *		increase the original number of buckets in order to reduce
 *		number of tuples per bucket
 */
static void
ExecHashIncreaseNumBuckets(HashJoinTable hashtable)
{
	HashMemoryChunk chunk;

	/* do nothing if not an increase (it's called increase for a reason) */
	if (hashtable->nbuckets >= hashtable->nbuckets_optimal)
		return;

#ifdef HJDEBUG
	printf("Hashjoin %p: increasing nbuckets %d => %d\n",
		   hashtable, hashtable->nbuckets, hashtable->nbuckets_optimal);
#endif

	hashtable->nbuckets = hashtable->nbuckets_optimal;
	hashtable->log2_nbuckets = hashtable->log2_nbuckets_optimal;

	Assert(hashtable->nbuckets > 1);
	Assert(hashtable->nbuckets <= (INT_MAX / 2));
	Assert(hashtable->nbuckets == (1 << hashtable->log2_nbuckets));

	/*
	 * Just reallocate the proper number of buckets - we don't need to walk
	 * through them - we can walk the dense-allocated chunks (just like in
	 * ExecHashIncreaseNumBatches, but without all the copying into new
	 * chunks)
	 */
	hashtable->buckets.unshared =
		(HashJoinTuple *) repalloc(hashtable->buckets.unshared,
								   hashtable->nbuckets * sizeof(HashJoinTuple));

	memset(hashtable->buckets.unshared, 0,
		   hashtable->nbuckets * sizeof(HashJoinTuple));

	/* scan through all tuples in all chunks to rebuild the hash table */
	for (chunk = hashtable->chunks; chunk != NULL; chunk = chunk->next.unshared)
	{
		/* process all tuples stored in this chunk */
		size_t		idx = 0;

		while (idx < chunk->used)
		{
			HashJoinTuple hashTuple = (HashJoinTuple) (HASH_CHUNK_DATA(chunk) + idx);
			int			bucketno;
			int			batchno;

			ExecHashGetBucketAndBatch(hashtable, hashTuple->hashvalue,
									  &bucketno, &batchno);

			/* add the tuple to the proper bucket */
			hashTuple->next.unshared = hashtable->buckets.unshared[bucketno];
			hashtable->buckets.unshared[bucketno] = hashTuple;

			/* advance index past the tuple */
			idx += MAXALIGN(HJTUPLE_OVERHEAD +
							HJTUPLE_MINTUPLE(hashTuple)->t_len);
		}

		/* allow this loop to be cancellable */
		CHECK_FOR_INTERRUPTS();
	}
}

static void
ExecParallelHashIncreaseNumBuckets(HashJoinTable hashtable)
{
	ParallelHashJoinState *pstate = hashtable->parallel_state;
	int			i;
	HashMemoryChunk chunk;
	dsa_pointer chunk_s;

	Assert(BarrierPhase(&pstate->build_barrier) == PHJ_BUILD_HASHING_INNER);

	/*
	 * It's unlikely, but we need to be prepared for new participants to show
	 * up while we're in the middle of this operation so we need to switch on
	 * barrier phase here.
	 */
	switch (PHJ_GROW_BUCKETS_PHASE(BarrierPhase(&pstate->grow_buckets_barrier)))
	{
		case PHJ_GROW_BUCKETS_ELECTING:
			/* Elect one participant to prepare to increase nbuckets. */
			if (BarrierArriveAndWait(&pstate->grow_buckets_barrier,
									 WAIT_EVENT_HASH_GROW_BUCKETS_ELECTING))
			{
				size_t		size;
				dsa_pointer_atomic *buckets;

				/* Double the size of the bucket array. */
				pstate->nbuckets *= 2;
				size = pstate->nbuckets * sizeof(dsa_pointer_atomic);
				hashtable->batches[0].shared->size += size / 2;
				dsa_free(hashtable->area, hashtable->batches[0].shared->buckets);
				hashtable->batches[0].shared->buckets =
					dsa_allocate(hashtable->area, size);
				buckets = (dsa_pointer_atomic *)
					dsa_get_address(hashtable->area,
									hashtable->batches[0].shared->buckets);
				for (i = 0; i < pstate->nbuckets; ++i)
					dsa_pointer_atomic_init(&buckets[i], InvalidDsaPointer);

				/* Put the chunk list onto the work queue. */
				pstate->chunk_work_queue = hashtable->batches[0].shared->chunks;

				/* Clear the flag. */
				pstate->growth = PHJ_GROWTH_OK;
			}
			/* Fall through. */

		case PHJ_GROW_BUCKETS_ALLOCATING:
			/* Wait for the above to complete. */
			BarrierArriveAndWait(&pstate->grow_buckets_barrier,
								 WAIT_EVENT_HASH_GROW_BUCKETS_ALLOCATING);
			/* Fall through. */

		case PHJ_GROW_BUCKETS_REINSERTING:
			/* Reinsert all tuples into the hash table. */
			ExecParallelHashEnsureBatchAccessors(hashtable);
			ExecParallelHashTableSetCurrentBatch(hashtable, 0);
			while ((chunk = ExecParallelHashPopChunkQueue(hashtable, &chunk_s)))
			{
				size_t		idx = 0;

				while (idx < chunk->used)
				{
					HashJoinTuple hashTuple = (HashJoinTuple) (HASH_CHUNK_DATA(chunk) + idx);
					dsa_pointer shared = chunk_s + HASH_CHUNK_HEADER_SIZE + idx;
					int			bucketno;
					int			batchno;

					ExecHashGetBucketAndBatch(hashtable, hashTuple->hashvalue,
											  &bucketno, &batchno);
					Assert(batchno == 0);

					/* add the tuple to the proper bucket */
					ExecParallelHashPushTuple(&hashtable->buckets.shared[bucketno],
											  hashTuple, shared);

					/* advance index past the tuple */
					idx += MAXALIGN(HJTUPLE_OVERHEAD +
									HJTUPLE_MINTUPLE(hashTuple)->t_len);
				}

				/* allow this loop to be cancellable */
				CHECK_FOR_INTERRUPTS();
			}
			BarrierArriveAndWait(&pstate->grow_buckets_barrier,
								 WAIT_EVENT_HASH_GROW_BUCKETS_REINSERTING);
	}
}

/*
 * ExecHashTableInsert
 *		insert a tuple into the hash table depending on the hash value
 *		it may just go to a temp file for later batches
 *
 * Note: the passed TupleTableSlot may contain a regular, minimal, or virtual
 * tuple; the minimal case in particular is certain to happen while reloading
 * tuples from batch files.  We could save some cycles in the regular-tuple
 * case by not forcing the slot contents into minimal form; not clear if it's
 * worth the messiness required.
 */
void
ExecHashTableInsert(HashJoinTable hashtable,
					TupleTableSlot *slot,
					uint32 hashvalue)
{
	bool		shouldFree;
	MinimalTuple tuple = ExecFetchSlotMinimalTuple(slot, &shouldFree);
	int			bucketno;
	int			batchno;

	ExecHashGetBucketAndBatch(hashtable, hashvalue,
							  &bucketno, &batchno);

	/*
	 * decide whether to put the tuple in the hash table or a temp file
	 */
	if (batchno == hashtable->curbatch)
	{
		/*
		 * put the tuple in hash table
		 */
		HashJoinTuple hashTuple;
		int			hashTupleSize;
		double		ntuples = (hashtable->totalTuples - hashtable->skewTuples);

		/* Create the HashJoinTuple */
		hashTupleSize = HJTUPLE_OVERHEAD + tuple->t_len;
		hashTuple = (HashJoinTuple) dense_alloc(hashtable, hashTupleSize);

		hashTuple->hashvalue = hashvalue;
		memcpy(HJTUPLE_MINTUPLE(hashTuple), tuple, tuple->t_len);

		/*
		 * We always reset the tuple-matched flag on insertion.  This is okay
		 * even when reloading a tuple from a batch file, since the tuple
		 * could not possibly have been matched to an outer tuple before it
		 * went into the batch file.
		 */
		HeapTupleHeaderClearMatch(HJTUPLE_MINTUPLE(hashTuple));

		/* Push it onto the front of the bucket's list */
		hashTuple->next.unshared = hashtable->buckets.unshared[bucketno];
		hashtable->buckets.unshared[bucketno] = hashTuple;

		/*
		 * Increase the (optimal) number of buckets if we just exceeded the
		 * NTUP_PER_BUCKET threshold, but only when there's still a single
		 * batch.
		 */
		if (hashtable->nbatch == 1 &&
			ntuples > (hashtable->nbuckets_optimal * NTUP_PER_BUCKET))
		{
			/* Guard against integer overflow and alloc size overflow */
			if (hashtable->nbuckets_optimal <= INT_MAX / 2 &&
				hashtable->nbuckets_optimal * 2 <= MaxAllocSize / sizeof(HashJoinTuple))
			{
				hashtable->nbuckets_optimal *= 2;
				hashtable->log2_nbuckets_optimal += 1;
			}
		}

		/* Account for space used, and back off if we've used too much */
		hashtable->spaceUsed += hashTupleSize;
		if (hashtable->spaceUsed > hashtable->spacePeak)
			hashtable->spacePeak = hashtable->spaceUsed;
		if (hashtable->spaceUsed +
			hashtable->nbuckets_optimal * sizeof(HashJoinTuple)
			> hashtable->spaceAllowed)
			ExecHashIncreaseNumBatches(hashtable);
	}
	else
	{
		/*
		 * put the tuple into a temp file for later batches
		 */
		Assert(batchno > hashtable->curbatch);
		ExecHashJoinSaveTuple(tuple,
							  hashvalue,
							  &hashtable->innerBatchFile[batchno]);
	}

	if (shouldFree)
		heap_free_minimal_tuple(tuple);
}

/*
 * ExecParallelHashTableInsert
 *		insert a tuple into a shared hash table or shared batch tuplestore
 */
void
ExecParallelHashTableInsert(HashJoinTable hashtable,
							TupleTableSlot *slot,
							uint32 hashvalue)
{
	bool		shouldFree;
	MinimalTuple tuple = ExecFetchSlotMinimalTuple(slot, &shouldFree);
	dsa_pointer shared;
	int			bucketno;
	int			batchno;

retry:
	ExecHashGetBucketAndBatch(hashtable, hashvalue, &bucketno, &batchno);

	if (batchno == 0)
	{
		HashJoinTuple hashTuple;

		/* Try to load it into memory. */
		Assert(BarrierPhase(&hashtable->parallel_state->build_barrier) ==
			   PHJ_BUILD_HASHING_INNER);
		hashTuple = ExecParallelHashTupleAlloc(hashtable,
											   HJTUPLE_OVERHEAD + tuple->t_len,
											   &shared);
		if (hashTuple == NULL)
			goto retry;

		/* Store the hash value in the HashJoinTuple header. */
		hashTuple->hashvalue = hashvalue;
		memcpy(HJTUPLE_MINTUPLE(hashTuple), tuple, tuple->t_len);

		/* Push it onto the front of the bucket's list */
		ExecParallelHashPushTuple(&hashtable->buckets.shared[bucketno],
								  hashTuple, shared);
	}
	else
	{
		size_t		tuple_size = MAXALIGN(HJTUPLE_OVERHEAD + tuple->t_len);

		Assert(batchno > 0);

		/* Try to preallocate space in the batch if necessary. */
		if (hashtable->batches[batchno].preallocated < tuple_size)
		{
			if (!ExecParallelHashTuplePrealloc(hashtable, batchno, tuple_size))
				goto retry;
		}

		Assert(hashtable->batches[batchno].preallocated >= tuple_size);
		hashtable->batches[batchno].preallocated -= tuple_size;
		sts_puttuple(hashtable->batches[batchno].inner_tuples, &hashvalue,
					 tuple);
	}
	++hashtable->batches[batchno].ntuples;

	if (shouldFree)
		heap_free_minimal_tuple(tuple);
}

/*
 * Insert a tuple into the current hash table.  Unlike
 * ExecParallelHashTableInsert, this version is not prepared to send the tuple
 * to other batches or to run out of memory, and should only be called with
 * tuples that belong in the current batch once growth has been disabled.
 */
void
ExecParallelHashTableInsertCurrentBatch(HashJoinTable hashtable,
										TupleTableSlot *slot,
										uint32 hashvalue)
{
	bool		shouldFree;
	MinimalTuple tuple = ExecFetchSlotMinimalTuple(slot, &shouldFree);
	HashJoinTuple hashTuple;
	dsa_pointer shared;
	int			batchno;
	int			bucketno;

	ExecHashGetBucketAndBatch(hashtable, hashvalue, &bucketno, &batchno);
	Assert(batchno == hashtable->curbatch);
	hashTuple = ExecParallelHashTupleAlloc(hashtable,
										   HJTUPLE_OVERHEAD + tuple->t_len,
										   &shared);
	hashTuple->hashvalue = hashvalue;
	memcpy(HJTUPLE_MINTUPLE(hashTuple), tuple, tuple->t_len);
	HeapTupleHeaderClearMatch(HJTUPLE_MINTUPLE(hashTuple));
	ExecParallelHashPushTuple(&hashtable->buckets.shared[bucketno],
							  hashTuple, shared);

	if (shouldFree)
		heap_free_minimal_tuple(tuple);
}

/*
 * ExecHashGetHashValue
 *		Compute the hash value for a tuple
 *
 * The tuple to be tested must be in econtext->ecxt_outertuple (thus Vars in
 * the hashkeys expressions need to have OUTER_VAR as varno). If outer_tuple
 * is false (meaning it's the HashJoin's inner node, Hash), econtext,
 * hashkeys, and slot need to be from Hash, with hashkeys/slot referencing and
 * being suitable for tuples from the node below the Hash. Conversely, if
 * outer_tuple is true, econtext is from HashJoin, and hashkeys/slot need to
 * be appropriate for tuples from HashJoin's outer node.
 *
 * A true result means the tuple's hash value has been successfully computed
 * and stored at *hashvalue.  A false result means the tuple cannot match
 * because it contains a null attribute, and hence it should be discarded
 * immediately.  (If keep_nulls is true then false is never returned.)
 */
bool
ExecHashGetHashValue(HashJoinTable hashtable,
					 ExprContext *econtext,
					 List *hashkeys,
					 bool outer_tuple,
					 bool keep_nulls,
					 uint32 *hashvalue)
{
	uint32		hashkey = 0;
	FmgrInfo   *hashfunctions;
	ListCell   *hk;
	int			i = 0;
	MemoryContext oldContext;

	/*
	 * We reset the eval context each time to reclaim any memory leaked in the
	 * hashkey expressions.
	 */
	ResetExprContext(econtext);

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	if (outer_tuple)
		hashfunctions = hashtable->outer_hashfunctions;
	else
		hashfunctions = hashtable->inner_hashfunctions;

	foreach(hk, hashkeys)
	{
		ExprState  *keyexpr = (ExprState *) lfirst(hk);
		Datum		keyval;
		bool		isNull;

		/* rotate hashkey left 1 bit at each step */
		hashkey = (hashkey << 1) | ((hashkey & 0x80000000) ? 1 : 0);

		/*
		 * Get the join attribute value of the tuple
		 */
		keyval = ExecEvalExpr(keyexpr, econtext, &isNull);

		/*
		 * If the attribute is NULL, and the join operator is strict, then
		 * this tuple cannot pass the join qual so we can reject it
		 * immediately (unless we're scanning the outside of an outer join, in
		 * which case we must not reject it).  Otherwise we act like the
		 * hashcode of NULL is zero (this will support operators that act like
		 * IS NOT DISTINCT, though not any more-random behavior).  We treat
		 * the hash support function as strict even if the operator is not.
		 *
		 * Note: currently, all hashjoinable operators must be strict since
		 * the hash index AM assumes that.  However, it takes so little extra
		 * code here to allow non-strict that we may as well do it.
		 */
		if (isNull)
		{
			if (hashtable->hashStrict[i] && !keep_nulls)
			{
				MemoryContextSwitchTo(oldContext);
				return false;	/* cannot match */
			}
			/* else, leave hashkey unmodified, equivalent to hashcode 0 */
		}
		else
		{
			/* Compute the hash function */
			uint32		hkey;

			hkey = DatumGetUInt32(FunctionCall1Coll(&hashfunctions[i], hashtable->collations[i], keyval));
			hashkey ^= hkey;
		}

		i++;
	}

	MemoryContextSwitchTo(oldContext);

	*hashvalue = hashkey;
	return true;
}

/*
 * ExecHashGetBucketAndBatch
 *		Determine the bucket number and batch number for a hash value
 *
 * Note: on-the-fly increases of nbatch must not change the bucket number
 * for a given hash code (since we don't move tuples to different hash
 * chains), and must only cause the batch number to remain the same or
 * increase.  Our algorithm is
 *		bucketno = hashvalue MOD nbuckets
 *		batchno = ROR(hashvalue, log2_nbuckets) MOD nbatch
 * where nbuckets and nbatch are both expected to be powers of 2, so we can
 * do the computations by shifting and masking.  (This assumes that all hash
 * functions are good about randomizing all their output bits, else we are
 * likely to have very skewed bucket or batch occupancy.)
 *
 * nbuckets and log2_nbuckets may change while nbatch == 1 because of dynamic
 * bucket count growth.  Once we start batching, the value is fixed and does
 * not change over the course of the join (making it possible to compute batch
 * number the way we do here).
 *
 * nbatch is always a power of 2; we increase it only by doubling it.  This
 * effectively adds one more bit to the top of the batchno.  In very large
 * joins, we might run out of bits to add, so we do this by rotating the hash
 * value.  This causes batchno to steal bits from bucketno when the number of
 * virtual buckets exceeds 2^32.  It's better to have longer bucket chains
 * than to lose the ability to divide batches.
 */
void
ExecHashGetBucketAndBatch(HashJoinTable hashtable,
						  uint32 hashvalue,
						  int *bucketno,
						  int *batchno)
{
	uint32		nbuckets = (uint32) hashtable->nbuckets;
	uint32		nbatch = (uint32) hashtable->nbatch;

	if (nbatch > 1)
	{
		*bucketno = hashvalue & (nbuckets - 1);
		*batchno = pg_rotate_right32(hashvalue,
									 hashtable->log2_nbuckets) & (nbatch - 1);
	}
	else
	{
		*bucketno = hashvalue & (nbuckets - 1);
		*batchno = 0;
	}
}

/*
 * ExecScanHashBucket
 *		scan a hash bucket for matches to the current outer tuple
 *
 * The current outer tuple must be stored in econtext->ecxt_outertuple.
 *
 * On success, the inner tuple is stored into hjstate->hj_CurTuple and
 * econtext->ecxt_innertuple, using hjstate->hj_HashTupleSlot as the slot
 * for the latter.
 */
bool
ExecScanHashBucket(HashJoinState *hjstate,
				   ExprContext *econtext)
{
	ExprState  *hjclauses = hjstate->hashclauses;
	HashJoinTable hashtable = hjstate->hj_HashTable;
	HashJoinTuple hashTuple = hjstate->hj_CurTuple;
	uint32		hashvalue = hjstate->hj_CurHashValue;

	/*
	 * hj_CurTuple is the address of the tuple last returned from the current
	 * bucket, or NULL if it's time to start scanning a new bucket.
	 *
	 * If the tuple hashed to a skew bucket then scan the skew bucket
	 * otherwise scan the standard hashtable bucket.
	 */
	if (hashTuple != NULL)
		hashTuple = hashTuple->next.unshared;
	else if (hjstate->hj_CurSkewBucketNo != INVALID_SKEW_BUCKET_NO)
		hashTuple = hashtable->skewBucket[hjstate->hj_CurSkewBucketNo]->tuples;
	else
		hashTuple = hashtable->buckets.unshared[hjstate->hj_CurBucketNo];

	while (hashTuple != NULL)
	{
		if (hashTuple->hashvalue == hashvalue)
		{
			TupleTableSlot *inntuple;

			/* insert hashtable's tuple into exec slot so ExecQual sees it */
			inntuple = ExecStoreMinimalTuple(HJTUPLE_MINTUPLE(hashTuple),
											 hjstate->hj_HashTupleSlot,
											 false);	/* do not pfree */
			econtext->ecxt_innertuple = inntuple;

			if (ExecQualAndReset(hjclauses, econtext))
			{
				hjstate->hj_CurTuple = hashTuple;
				return true;
			}
		}

		hashTuple = hashTuple->next.unshared;
	}

	/*
	 * no match
	 */
	return false;
}

/*
 * ExecParallelScanHashBucket
 *		scan a hash bucket for matches to the current outer tuple
 *
 * The current outer tuple must be stored in econtext->ecxt_outertuple.
 *
 * On success, the inner tuple is stored into hjstate->hj_CurTuple and
 * econtext->ecxt_innertuple, using hjstate->hj_HashTupleSlot as the slot
 * for the latter.
 */
bool
ExecParallelScanHashBucket(HashJoinState *hjstate,
						   ExprContext *econtext)
{
	ExprState  *hjclauses = hjstate->hashclauses;
	HashJoinTable hashtable = hjstate->hj_HashTable;
	HashJoinTuple hashTuple = hjstate->hj_CurTuple;
	uint32		hashvalue = hjstate->hj_CurHashValue;

	/*
	 * hj_CurTuple is the address of the tuple last returned from the current
	 * bucket, or NULL if it's time to start scanning a new bucket.
	 */
	if (hashTuple != NULL)
		hashTuple = ExecParallelHashNextTuple(hashtable, hashTuple);
	else
		hashTuple = ExecParallelHashFirstTuple(hashtable,
											   hjstate->hj_CurBucketNo);

	while (hashTuple != NULL)
	{
		if (hashTuple->hashvalue == hashvalue)
		{
			TupleTableSlot *inntuple;

			/* insert hashtable's tuple into exec slot so ExecQual sees it */
			inntuple = ExecStoreMinimalTuple(HJTUPLE_MINTUPLE(hashTuple),
											 hjstate->hj_HashTupleSlot,
											 false);	/* do not pfree */
			econtext->ecxt_innertuple = inntuple;

			if (ExecQualAndReset(hjclauses, econtext))
			{
				hjstate->hj_CurTuple = hashTuple;
				return true;
			}
		}

		hashTuple = ExecParallelHashNextTuple(hashtable, hashTuple);
	}

	/*
	 * no match
	 */
	return false;
}

/*
 * ExecPrepHashTableForUnmatched
 *		set up for a series of ExecScanHashTableForUnmatched calls
 */
void
ExecPrepHashTableForUnmatched(HashJoinState *hjstate)
{
	/*----------
	 * During this scan we use the HashJoinState fields as follows:
	 *
	 * hj_CurBucketNo: next regular bucket to scan
	 * hj_CurSkewBucketNo: next skew bucket (an index into skewBucketNums)
	 * hj_CurTuple: last tuple returned, or NULL to start next bucket
	 *----------
	 */
	hjstate->hj_CurBucketNo = 0;
	hjstate->hj_CurSkewBucketNo = 0;
	hjstate->hj_CurTuple = NULL;
}

/*
 * ExecScanHashTableForUnmatched
 *		scan the hash table for unmatched inner tuples
 *
 * On success, the inner tuple is stored into hjstate->hj_CurTuple and
 * econtext->ecxt_innertuple, using hjstate->hj_HashTupleSlot as the slot
 * for the latter.
 */
bool
ExecScanHashTableForUnmatched(HashJoinState *hjstate, ExprContext *econtext)
{
	HashJoinTable hashtable = hjstate->hj_HashTable;
	HashJoinTuple hashTuple = hjstate->hj_CurTuple;

	for (;;)
	{
		/*
		 * hj_CurTuple is the address of the tuple last returned from the
		 * current bucket, or NULL if it's time to start scanning a new
		 * bucket.
		 */
		if (hashTuple != NULL)
			hashTuple = hashTuple->next.unshared;
		else if (hjstate->hj_CurBucketNo < hashtable->nbuckets)
		{
			hashTuple = hashtable->buckets.unshared[hjstate->hj_CurBucketNo];
			hjstate->hj_CurBucketNo++;
		}
		else if (hjstate->hj_CurSkewBucketNo < hashtable->nSkewBuckets)
		{
			int			j = hashtable->skewBucketNums[hjstate->hj_CurSkewBucketNo];

			hashTuple = hashtable->skewBucket[j]->tuples;
			hjstate->hj_CurSkewBucketNo++;
		}
		else
			break;				/* finished all buckets */

		while (hashTuple != NULL)
		{
			if (!HeapTupleHeaderHasMatch(HJTUPLE_MINTUPLE(hashTuple)))
			{
				TupleTableSlot *inntuple;

				/* insert hashtable's tuple into exec slot */
				inntuple = ExecStoreMinimalTuple(HJTUPLE_MINTUPLE(hashTuple),
												 hjstate->hj_HashTupleSlot,
												 false);	/* do not pfree */
				econtext->ecxt_innertuple = inntuple;

				/*
				 * Reset temp memory each time; although this function doesn't
				 * do any qual eval, the caller will, so let's keep it
				 * parallel to ExecScanHashBucket.
				 */
				ResetExprContext(econtext);

				hjstate->hj_CurTuple = hashTuple;
				return true;
			}

			hashTuple = hashTuple->next.unshared;
		}

		/* allow this loop to be cancellable */
		CHECK_FOR_INTERRUPTS();
	}

	/*
	 * no more unmatched tuples
	 */
	return false;
}

/*
 * ExecHashTableReset
 *
 *		reset hash table header for new batch
 */
void
ExecHashTableReset(HashJoinTable hashtable)
{
	MemoryContext oldcxt;
	int			nbuckets = hashtable->nbuckets;

	/*
	 * Release all the hash buckets and tuples acquired in the prior pass, and
	 * reinitialize the context for a new pass.
	 */
	MemoryContextReset(hashtable->batchCxt);
	oldcxt = MemoryContextSwitchTo(hashtable->batchCxt);

	/* Reallocate and reinitialize the hash bucket headers. */
	hashtable->buckets.unshared = (HashJoinTuple *)
		palloc0(nbuckets * sizeof(HashJoinTuple));

	hashtable->spaceUsed = 0;

	MemoryContextSwitchTo(oldcxt);

	/* Forget the chunks (the memory was freed by the context reset above). */
	hashtable->chunks = NULL;
}

/*
 * ExecHashTableResetMatchFlags
 *		Clear all the HeapTupleHeaderHasMatch flags in the table
 */
void
ExecHashTableResetMatchFlags(HashJoinTable hashtable)
{
	HashJoinTuple tuple;
	int			i;

	/* Reset all flags in the main table ... */
	for (i = 0; i < hashtable->nbuckets; i++)
	{
		for (tuple = hashtable->buckets.unshared[i]; tuple != NULL;
			 tuple = tuple->next.unshared)
			HeapTupleHeaderClearMatch(HJTUPLE_MINTUPLE(tuple));
	}

	/* ... and the same for the skew buckets, if any */
	for (i = 0; i < hashtable->nSkewBuckets; i++)
	{
		int			j = hashtable->skewBucketNums[i];
		HashSkewBucket *skewBucket = hashtable->skewBucket[j];

		for (tuple = skewBucket->tuples; tuple != NULL; tuple = tuple->next.unshared)
			HeapTupleHeaderClearMatch(HJTUPLE_MINTUPLE(tuple));
	}
}


void
ExecReScanHash(HashState *node)
{
	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (node->ps.lefttree->chgParam == NULL)
		ExecReScan(node->ps.lefttree);
}


/*
 * ExecHashBuildSkewHash
 *
 *		Set up for skew optimization if we can identify the most common values
 *		(MCVs) of the outer relation's join key.  We make a skew hash bucket
 *		for the hash value of each MCV, up to the number of slots allowed
 *		based on available memory.
 */
static void
ExecHashBuildSkewHash(HashJoinTable hashtable, Hash *node, int mcvsToUse)
{
	HeapTupleData *statsTuple;
	AttStatsSlot sslot;

	/* Do nothing if planner didn't identify the outer relation's join key */
	if (!OidIsValid(node->skewTable))
		return;
	/* Also, do nothing if we don't have room for at least one skew bucket */
	if (mcvsToUse <= 0)
		return;

	/*
	 * Try to find the MCV statistics for the outer relation's join key.
	 */
	statsTuple = SearchSysCache3(STATRELATTINH,
								 ObjectIdGetDatum(node->skewTable),
								 Int16GetDatum(node->skewColumn),
								 BoolGetDatum(node->skewInherit));
	if (!HeapTupleIsValid(statsTuple))
		return;

	if (get_attstatsslot(&sslot, statsTuple,
						 STATISTIC_KIND_MCV, InvalidOid,
						 ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS))
	{
		double		frac;
		int			nbuckets;
		FmgrInfo   *hashfunctions;
		int			i;

		if (mcvsToUse > sslot.nvalues)
			mcvsToUse = sslot.nvalues;

		/*
		 * Calculate the expected fraction of outer relation that will
		 * participate in the skew optimization.  If this isn't at least
		 * SKEW_MIN_OUTER_FRACTION, don't use skew optimization.
		 */
		frac = 0;
		for (i = 0; i < mcvsToUse; i++)
			frac += sslot.numbers[i];
		if (frac < SKEW_MIN_OUTER_FRACTION)
		{
			free_attstatsslot(&sslot);
			ReleaseSysCache(statsTuple);
			return;
		}

		/*
		 * Okay, set up the skew hashtable.
		 *
		 * skewBucket[] is an open addressing hashtable with a power of 2 size
		 * that is greater than the number of MCV values.  (This ensures there
		 * will be at least one null entry, so searches will always
		 * terminate.)
		 *
		 * Note: this code could fail if mcvsToUse exceeds INT_MAX/8 or
		 * MaxAllocSize/sizeof(void *)/8, but that is not currently possible
		 * since we limit pg_statistic entries to much less than that.
		 */
		nbuckets = 2;
		while (nbuckets <= mcvsToUse)
			nbuckets <<= 1;
		/* use two more bits just to help avoid collisions */
		nbuckets <<= 2;

		hashtable->skewEnabled = true;
		hashtable->skewBucketLen = nbuckets;

		/*
		 * We allocate the bucket memory in the hashtable's batch context. It
		 * is only needed during the first batch, and this ensures it will be
		 * automatically removed once the first batch is done.
		 */
		hashtable->skewBucket = (HashSkewBucket **)
			MemoryContextAllocZero(hashtable->batchCxt,
								   nbuckets * sizeof(HashSkewBucket *));
		hashtable->skewBucketNums = (int *)
			MemoryContextAllocZero(hashtable->batchCxt,
								   mcvsToUse * sizeof(int));

		hashtable->spaceUsed += nbuckets * sizeof(HashSkewBucket *)
			+ mcvsToUse * sizeof(int);
		hashtable->spaceUsedSkew += nbuckets * sizeof(HashSkewBucket *)
			+ mcvsToUse * sizeof(int);
		if (hashtable->spaceUsed > hashtable->spacePeak)
			hashtable->spacePeak = hashtable->spaceUsed;

		/*
		 * Create a skew bucket for each MCV hash value.
		 *
		 * Note: it is very important that we create the buckets in order of
		 * decreasing MCV frequency.  If we have to remove some buckets, they
		 * must be removed in reverse order of creation (see notes in
		 * ExecHashRemoveNextSkewBucket) and we want the least common MCVs to
		 * be removed first.
		 */
		hashfunctions = hashtable->outer_hashfunctions;

		for (i = 0; i < mcvsToUse; i++)
		{
			uint32		hashvalue;
			int			bucket;

			hashvalue = DatumGetUInt32(FunctionCall1Coll(&hashfunctions[0],
														 hashtable->collations[0],
														 sslot.values[i]));

			/*
			 * While we have not hit a hole in the hashtable and have not hit
			 * the desired bucket, we have collided with some previous hash
			 * value, so try the next bucket location.  NB: this code must
			 * match ExecHashGetSkewBucket.
			 */
			bucket = hashvalue & (nbuckets - 1);
			while (hashtable->skewBucket[bucket] != NULL &&
				   hashtable->skewBucket[bucket]->hashvalue != hashvalue)
				bucket = (bucket + 1) & (nbuckets - 1);

			/*
			 * If we found an existing bucket with the same hashvalue, leave
			 * it alone.  It's okay for two MCVs to share a hashvalue.
			 */
			if (hashtable->skewBucket[bucket] != NULL)
				continue;

			/* Okay, create a new skew bucket for this hashvalue. */
			hashtable->skewBucket[bucket] = (HashSkewBucket *)
				MemoryContextAlloc(hashtable->batchCxt,
								   sizeof(HashSkewBucket));
			hashtable->skewBucket[bucket]->hashvalue = hashvalue;
			hashtable->skewBucket[bucket]->tuples = NULL;
			hashtable->skewBucketNums[hashtable->nSkewBuckets] = bucket;
			hashtable->nSkewBuckets++;
			hashtable->spaceUsed += SKEW_BUCKET_OVERHEAD;
			hashtable->spaceUsedSkew += SKEW_BUCKET_OVERHEAD;
			if (hashtable->spaceUsed > hashtable->spacePeak)
				hashtable->spacePeak = hashtable->spaceUsed;
		}

		free_attstatsslot(&sslot);
	}

	ReleaseSysCache(statsTuple);
}

/*
 * ExecHashGetSkewBucket
 *
 *		Returns the index of the skew bucket for this hashvalue,
 *		or INVALID_SKEW_BUCKET_NO if the hashvalue is not
 *		associated with any active skew bucket.
 */
int
ExecHashGetSkewBucket(HashJoinTable hashtable, uint32 hashvalue)
{
	int			bucket;

	/*
	 * Always return INVALID_SKEW_BUCKET_NO if not doing skew optimization (in
	 * particular, this happens after the initial batch is done).
	 */
	if (!hashtable->skewEnabled)
		return INVALID_SKEW_BUCKET_NO;

	/*
	 * Since skewBucketLen is a power of 2, we can do a modulo by ANDing.
	 */
	bucket = hashvalue & (hashtable->skewBucketLen - 1);

	/*
	 * While we have not hit a hole in the hashtable and have not hit the
	 * desired bucket, we have collided with some other hash value, so try the
	 * next bucket location.
	 */
	while (hashtable->skewBucket[bucket] != NULL &&
		   hashtable->skewBucket[bucket]->hashvalue != hashvalue)
		bucket = (bucket + 1) & (hashtable->skewBucketLen - 1);

	/*
	 * Found the desired bucket?
	 */
	if (hashtable->skewBucket[bucket] != NULL)
		return bucket;

	/*
	 * There must not be any hashtable entry for this hash value.
	 */
	return INVALID_SKEW_BUCKET_NO;
}

/*
 * ExecHashSkewTableInsert
 *
 *		Insert a tuple into the skew hashtable.
 *
 * This should generally match up with the current-batch case in
 * ExecHashTableInsert.
 */
static void
ExecHashSkewTableInsert(HashJoinTable hashtable,
						TupleTableSlot *slot,
						uint32 hashvalue,
						int bucketNumber)
{
	bool		shouldFree;
	MinimalTuple tuple = ExecFetchSlotMinimalTuple(slot, &shouldFree);
	HashJoinTuple hashTuple;
	int			hashTupleSize;

	/* Create the HashJoinTuple */
	hashTupleSize = HJTUPLE_OVERHEAD + tuple->t_len;
	hashTuple = (HashJoinTuple) MemoryContextAlloc(hashtable->batchCxt,
												   hashTupleSize);
	hashTuple->hashvalue = hashvalue;
	memcpy(HJTUPLE_MINTUPLE(hashTuple), tuple, tuple->t_len);
	HeapTupleHeaderClearMatch(HJTUPLE_MINTUPLE(hashTuple));

	/* Push it onto the front of the skew bucket's list */
	hashTuple->next.unshared = hashtable->skewBucket[bucketNumber]->tuples;
	hashtable->skewBucket[bucketNumber]->tuples = hashTuple;
	Assert(hashTuple != hashTuple->next.unshared);

	/* Account for space used, and back off if we've used too much */
	hashtable->spaceUsed += hashTupleSize;
	hashtable->spaceUsedSkew += hashTupleSize;
	if (hashtable->spaceUsed > hashtable->spacePeak)
		hashtable->spacePeak = hashtable->spaceUsed;
	while (hashtable->spaceUsedSkew > hashtable->spaceAllowedSkew)
		ExecHashRemoveNextSkewBucket(hashtable);

	/* Check we are not over the total spaceAllowed, either */
	if (hashtable->spaceUsed > hashtable->spaceAllowed)
		ExecHashIncreaseNumBatches(hashtable);

	if (shouldFree)
		heap_free_minimal_tuple(tuple);
}

/*
 *		ExecHashRemoveNextSkewBucket
 *
 *		Remove the least valuable skew bucket by pushing its tuples into
 *		the main hash table.
 */
static void
ExecHashRemoveNextSkewBucket(HashJoinTable hashtable)
{
	int			bucketToRemove;
	HashSkewBucket *bucket;
	uint32		hashvalue;
	int			bucketno;
	int			batchno;
	HashJoinTuple hashTuple;

	/* Locate the bucket to remove */
	bucketToRemove = hashtable->skewBucketNums[hashtable->nSkewBuckets - 1];
	bucket = hashtable->skewBucket[bucketToRemove];

	/*
	 * Calculate which bucket and batch the tuples belong to in the main
	 * hashtable.  They all have the same hash value, so it's the same for all
	 * of them.  Also note that it's not possible for nbatch to increase while
	 * we are processing the tuples.
	 */
	hashvalue = bucket->hashvalue;
	ExecHashGetBucketAndBatch(hashtable, hashvalue, &bucketno, &batchno);

	/* Process all tuples in the bucket */
	hashTuple = bucket->tuples;
	while (hashTuple != NULL)
	{
		HashJoinTuple nextHashTuple = hashTuple->next.unshared;
		MinimalTuple tuple;
		Size		tupleSize;

		/*
		 * This code must agree with ExecHashTableInsert.  We do not use
		 * ExecHashTableInsert directly as ExecHashTableInsert expects a
		 * TupleTableSlot while we already have HashJoinTuples.
		 */
		tuple = HJTUPLE_MINTUPLE(hashTuple);
		tupleSize = HJTUPLE_OVERHEAD + tuple->t_len;

		/* Decide whether to put the tuple in the hash table or a temp file */
		if (batchno == hashtable->curbatch)
		{
			/* Move the tuple to the main hash table */
			HashJoinTuple copyTuple;

			/*
			 * We must copy the tuple into the dense storage, else it will not
			 * be found by, eg, ExecHashIncreaseNumBatches.
			 */
			copyTuple = (HashJoinTuple) dense_alloc(hashtable, tupleSize);
			memcpy(copyTuple, hashTuple, tupleSize);
			pfree(hashTuple);

			copyTuple->next.unshared = hashtable->buckets.unshared[bucketno];
			hashtable->buckets.unshared[bucketno] = copyTuple;

			/* We have reduced skew space, but overall space doesn't change */
			hashtable->spaceUsedSkew -= tupleSize;
		}
		else
		{
			/* Put the tuple into a temp file for later batches */
			Assert(batchno > hashtable->curbatch);
			ExecHashJoinSaveTuple(tuple, hashvalue,
								  &hashtable->innerBatchFile[batchno]);
			pfree(hashTuple);
			hashtable->spaceUsed -= tupleSize;
			hashtable->spaceUsedSkew -= tupleSize;
		}

		hashTuple = nextHashTuple;

		/* allow this loop to be cancellable */
		CHECK_FOR_INTERRUPTS();
	}

	/*
	 * Free the bucket struct itself and reset the hashtable entry to NULL.
	 *
	 * NOTE: this is not nearly as simple as it looks on the surface, because
	 * of the possibility of collisions in the hashtable.  Suppose that hash
	 * values A and B collide at a particular hashtable entry, and that A was
	 * entered first so B gets shifted to a different table entry.  If we were
	 * to remove A first then ExecHashGetSkewBucket would mistakenly start
	 * reporting that B is not in the hashtable, because it would hit the NULL
	 * before finding B.  However, we always remove entries in the reverse
	 * order of creation, so this failure cannot happen.
	 */
	hashtable->skewBucket[bucketToRemove] = NULL;
	hashtable->nSkewBuckets--;
	pfree(bucket);
	hashtable->spaceUsed -= SKEW_BUCKET_OVERHEAD;
	hashtable->spaceUsedSkew -= SKEW_BUCKET_OVERHEAD;

	/*
	 * If we have removed all skew buckets then give up on skew optimization.
	 * Release the arrays since they aren't useful any more.
	 */
	if (hashtable->nSkewBuckets == 0)
	{
		hashtable->skewEnabled = false;
		pfree(hashtable->skewBucket);
		pfree(hashtable->skewBucketNums);
		hashtable->skewBucket = NULL;
		hashtable->skewBucketNums = NULL;
		hashtable->spaceUsed -= hashtable->spaceUsedSkew;
		hashtable->spaceUsedSkew = 0;
	}
}

/*
 * Reserve space in the DSM segment for instrumentation data.
 */
void
ExecHashEstimate(HashState *node, ParallelContext *pcxt)
{
	size_t		size;

	/* don't need this if not instrumenting or no workers */
	if (!node->ps.instrument || pcxt->nworkers == 0)
		return;

	size = mul_size(pcxt->nworkers, sizeof(HashInstrumentation));
	size = add_size(size, offsetof(SharedHashInfo, hinstrument));
	shm_toc_estimate_chunk(&pcxt->estimator, size);
	shm_toc_estimate_keys(&pcxt->estimator, 1);
}

/*
 * Set up a space in the DSM for all workers to record instrumentation data
 * about their hash table.
 */
void
ExecHashInitializeDSM(HashState *node, ParallelContext *pcxt)
{
	size_t		size;

	/* don't need this if not instrumenting or no workers */
	if (!node->ps.instrument || pcxt->nworkers == 0)
		return;

	size = offsetof(SharedHashInfo, hinstrument) +
		pcxt->nworkers * sizeof(HashInstrumentation);
	node->shared_info = (SharedHashInfo *) shm_toc_allocate(pcxt->toc, size);
	memset(node->shared_info, 0, size);
	node->shared_info->num_workers = pcxt->nworkers;
	shm_toc_insert(pcxt->toc, node->ps.plan->plan_node_id,
				   node->shared_info);
}

/*
 * Locate the DSM space for hash table instrumentation data that we'll write
 * to at shutdown time.
 */
void
ExecHashInitializeWorker(HashState *node, ParallelWorkerContext *pwcxt)
{
	SharedHashInfo *shared_info;

	/* don't need this if not instrumenting */
	if (!node->ps.instrument)
		return;

	shared_info = (SharedHashInfo *)
		shm_toc_lookup(pwcxt->toc, node->ps.plan->plan_node_id, false);
	node->hinstrument = &shared_info->hinstrument[ParallelWorkerNumber];
}

/*
 * Copy instrumentation data from this worker's hash table (if it built one)
 * to DSM memory so the leader can retrieve it.  This must be done in an
 * ExecShutdownHash() rather than ExecEndHash() because the latter runs after
 * we've detached from the DSM segment.
 */
void
ExecShutdownHash(HashState *node)
{
	if (node->hinstrument && node->hashtable)
		ExecHashGetInstrumentation(node->hinstrument, node->hashtable);
}

/*
 * Retrieve instrumentation data from workers before the DSM segment is
 * detached, so that EXPLAIN can access it.
 */
void
ExecHashRetrieveInstrumentation(HashState *node)
{
	SharedHashInfo *shared_info = node->shared_info;
	size_t		size;

	if (shared_info == NULL)
		return;

	/* Replace node->shared_info with a copy in backend-local memory. */
	size = offsetof(SharedHashInfo, hinstrument) +
		shared_info->num_workers * sizeof(HashInstrumentation);
	node->shared_info = palloc(size);
	memcpy(node->shared_info, shared_info, size);
}

/*
 * Copy the instrumentation data from 'hashtable' into a HashInstrumentation
 * struct.
 */
void
ExecHashGetInstrumentation(HashInstrumentation *instrument,
						   HashJoinTable hashtable)
{
	instrument->nbuckets = hashtable->nbuckets;
	instrument->nbuckets_original = hashtable->nbuckets_original;
	instrument->nbatch = hashtable->nbatch;
	instrument->nbatch_original = hashtable->nbatch_original;
	instrument->space_peak = hashtable->spacePeak;
}

/*
 * Allocate 'size' bytes from the currently active HashMemoryChunk
 */
static void *
dense_alloc(HashJoinTable hashtable, Size size)
{
	HashMemoryChunk newChunk;
	char	   *ptr;

	/* just in case the size is not already aligned properly */
	size = MAXALIGN(size);

	/*
	 * If tuple size is larger than threshold, allocate a separate chunk.
	 */
	if (size > HASH_CHUNK_THRESHOLD)
	{
		/* allocate new chunk and put it at the beginning of the list */
		newChunk = (HashMemoryChunk) MemoryContextAlloc(hashtable->batchCxt,
														HASH_CHUNK_HEADER_SIZE + size);
		newChunk->maxlen = size;
		newChunk->used = size;
		newChunk->ntuples = 1;

		/*
		 * Add this chunk to the list after the first existing chunk, so that
		 * we don't lose the remaining space in the "current" chunk.
		 */
		if (hashtable->chunks != NULL)
		{
			newChunk->next = hashtable->chunks->next;
			hashtable->chunks->next.unshared = newChunk;
		}
		else
		{
			newChunk->next.unshared = hashtable->chunks;
			hashtable->chunks = newChunk;
		}

		return HASH_CHUNK_DATA(newChunk);
	}

	/*
	 * See if we have enough space for it in the current chunk (if any). If
	 * not, allocate a fresh chunk.
	 */
	if ((hashtable->chunks == NULL) ||
		(hashtable->chunks->maxlen - hashtable->chunks->used) < size)
	{
		/* allocate new chunk and put it at the beginning of the list */
		newChunk = (HashMemoryChunk) MemoryContextAlloc(hashtable->batchCxt,
														HASH_CHUNK_HEADER_SIZE + HASH_CHUNK_SIZE);

		newChunk->maxlen = HASH_CHUNK_SIZE;
		newChunk->used = size;
		newChunk->ntuples = 1;

		newChunk->next.unshared = hashtable->chunks;
		hashtable->chunks = newChunk;

		return HASH_CHUNK_DATA(newChunk);
	}

	/* There is enough space in the current chunk, let's add the tuple */
	ptr = HASH_CHUNK_DATA(hashtable->chunks) + hashtable->chunks->used;
	hashtable->chunks->used += size;
	hashtable->chunks->ntuples += 1;

	/* return pointer to the start of the tuple memory */
	return ptr;
}

/*
 * Allocate space for a tuple in shared dense storage.  This is equivalent to
 * dense_alloc but for Parallel Hash using shared memory.
 *
 * While loading a tuple into shared memory, we might run out of memory and
 * decide to repartition, or determine that the load factor is too high and
 * decide to expand the bucket array, or discover that another participant has
 * commanded us to help do that.  Return NULL if number of buckets or batches
 * has changed, indicating that the caller must retry (considering the
 * possibility that the tuple no longer belongs in the same batch).
 */
static HashJoinTuple
ExecParallelHashTupleAlloc(HashJoinTable hashtable, size_t size,
						   dsa_pointer *shared)
{
	ParallelHashJoinState *pstate = hashtable->parallel_state;
	dsa_pointer chunk_shared;
	HashMemoryChunk chunk;
	Size		chunk_size;
	HashJoinTuple result;
	int			curbatch = hashtable->curbatch;

	size = MAXALIGN(size);

	/*
	 * Fast path: if there is enough space in this backend's current chunk,
	 * then we can allocate without any locking.
	 */
	chunk = hashtable->current_chunk;
	if (chunk != NULL &&
		size <= HASH_CHUNK_THRESHOLD &&
		chunk->maxlen - chunk->used >= size)
	{

		chunk_shared = hashtable->current_chunk_shared;
		Assert(chunk == dsa_get_address(hashtable->area, chunk_shared));
		*shared = chunk_shared + HASH_CHUNK_HEADER_SIZE + chunk->used;
		result = (HashJoinTuple) (HASH_CHUNK_DATA(chunk) + chunk->used);
		chunk->used += size;

		Assert(chunk->used <= chunk->maxlen);
		Assert(result == dsa_get_address(hashtable->area, *shared));

		return result;
	}

	/* Slow path: try to allocate a new chunk. */
	LWLockAcquire(&pstate->lock, LW_EXCLUSIVE);

	/*
	 * Check if we need to help increase the number of buckets or batches.
	 */
	if (pstate->growth == PHJ_GROWTH_NEED_MORE_BATCHES ||
		pstate->growth == PHJ_GROWTH_NEED_MORE_BUCKETS)
	{
		ParallelHashGrowth growth = pstate->growth;

		hashtable->current_chunk = NULL;
		LWLockRelease(&pstate->lock);

		/* Another participant has commanded us to help grow. */
		if (growth == PHJ_GROWTH_NEED_MORE_BATCHES)
			ExecParallelHashIncreaseNumBatches(hashtable);
		else if (growth == PHJ_GROWTH_NEED_MORE_BUCKETS)
			ExecParallelHashIncreaseNumBuckets(hashtable);

		/* The caller must retry. */
		return NULL;
	}

	/* Oversized tuples get their own chunk. */
	if (size > HASH_CHUNK_THRESHOLD)
		chunk_size = size + HASH_CHUNK_HEADER_SIZE;
	else
		chunk_size = HASH_CHUNK_SIZE;

	/* Check if it's time to grow batches or buckets. */
	if (pstate->growth != PHJ_GROWTH_DISABLED)
	{
		Assert(curbatch == 0);
		Assert(BarrierPhase(&pstate->build_barrier) == PHJ_BUILD_HASHING_INNER);

		/*
		 * Check if our space limit would be exceeded.  To avoid choking on
		 * very large tuples or very low work_mem setting, we'll always allow
		 * each backend to allocate at least one chunk.
		 */
		if (hashtable->batches[0].at_least_one_chunk &&
			hashtable->batches[0].shared->size +
			chunk_size > pstate->space_allowed)
		{
			pstate->growth = PHJ_GROWTH_NEED_MORE_BATCHES;
			hashtable->batches[0].shared->space_exhausted = true;
			LWLockRelease(&pstate->lock);

			return NULL;
		}

		/* Check if our load factor limit would be exceeded. */
		if (hashtable->nbatch == 1)
		{
			hashtable->batches[0].shared->ntuples += hashtable->batches[0].ntuples;
			hashtable->batches[0].ntuples = 0;
			/* Guard against integer overflow and alloc size overflow */
			if (hashtable->batches[0].shared->ntuples + 1 >
				hashtable->nbuckets * NTUP_PER_BUCKET &&
				hashtable->nbuckets < (INT_MAX / 2) &&
				hashtable->nbuckets * 2 <=
				MaxAllocSize / sizeof(dsa_pointer_atomic))
			{
				pstate->growth = PHJ_GROWTH_NEED_MORE_BUCKETS;
				LWLockRelease(&pstate->lock);

				return NULL;
			}
		}
	}

	/* We are cleared to allocate a new chunk. */
	chunk_shared = dsa_allocate(hashtable->area, chunk_size);
	hashtable->batches[curbatch].shared->size += chunk_size;
	hashtable->batches[curbatch].at_least_one_chunk = true;

	/* Set up the chunk. */
	chunk = (HashMemoryChunk) dsa_get_address(hashtable->area, chunk_shared);
	*shared = chunk_shared + HASH_CHUNK_HEADER_SIZE;
	chunk->maxlen = chunk_size - HASH_CHUNK_HEADER_SIZE;
	chunk->used = size;

	/*
	 * Push it onto the list of chunks, so that it can be found if we need to
	 * increase the number of buckets or batches (batch 0 only) and later for
	 * freeing the memory (all batches).
	 */
	chunk->next.shared = hashtable->batches[curbatch].shared->chunks;
	hashtable->batches[curbatch].shared->chunks = chunk_shared;

	if (size <= HASH_CHUNK_THRESHOLD)
	{
		/*
		 * Make this the current chunk so that we can use the fast path to
		 * fill the rest of it up in future calls.
		 */
		hashtable->current_chunk = chunk;
		hashtable->current_chunk_shared = chunk_shared;
	}
	LWLockRelease(&pstate->lock);

	Assert(HASH_CHUNK_DATA(chunk) == dsa_get_address(hashtable->area, *shared));
	result = (HashJoinTuple) HASH_CHUNK_DATA(chunk);

	return result;
}

/*
 * One backend needs to set up the shared batch state including tuplestores.
 * Other backends will ensure they have correctly configured accessors by
 * called ExecParallelHashEnsureBatchAccessors().
 */
static void
ExecParallelHashJoinSetUpBatches(HashJoinTable hashtable, int nbatch)
{
	ParallelHashJoinState *pstate = hashtable->parallel_state;
	ParallelHashJoinBatch *batches;
	MemoryContext oldcxt;
	int			i;

	Assert(hashtable->batches == NULL);

	/* Allocate space. */
	pstate->batches =
		dsa_allocate0(hashtable->area,
					  EstimateParallelHashJoinBatch(hashtable) * nbatch);
	pstate->nbatch = nbatch;
	batches = dsa_get_address(hashtable->area, pstate->batches);

	/* Use hash join memory context. */
	oldcxt = MemoryContextSwitchTo(hashtable->hashCxt);

	/* Allocate this backend's accessor array. */
	hashtable->nbatch = nbatch;
	hashtable->batches = (ParallelHashJoinBatchAccessor *)
		palloc0(sizeof(ParallelHashJoinBatchAccessor) * hashtable->nbatch);

	/* Set up the shared state, tuplestores and backend-local accessors. */
	for (i = 0; i < hashtable->nbatch; ++i)
	{
		ParallelHashJoinBatchAccessor *accessor = &hashtable->batches[i];
		ParallelHashJoinBatch *shared = NthParallelHashJoinBatch(batches, i);
		char		name[MAXPGPATH];

		/*
		 * All members of shared were zero-initialized.  We just need to set
		 * up the Barrier.
		 */
		BarrierInit(&shared->batch_barrier, 0);
		if (i == 0)
		{
			/* Batch 0 doesn't need to be loaded. */
			BarrierAttach(&shared->batch_barrier);
			while (BarrierPhase(&shared->batch_barrier) < PHJ_BATCH_PROBING)
				BarrierArriveAndWait(&shared->batch_barrier, 0);
			BarrierDetach(&shared->batch_barrier);
		}

		/* Initialize accessor state.  All members were zero-initialized. */
		accessor->shared = shared;

		/* Initialize the shared tuplestores. */
		snprintf(name, sizeof(name), "i%dof%d", i, hashtable->nbatch);
		accessor->inner_tuples =
			sts_initialize(ParallelHashJoinBatchInner(shared),
						   pstate->nparticipants,
						   ParallelWorkerNumber + 1,
						   sizeof(uint32),
						   SHARED_TUPLESTORE_SINGLE_PASS,
						   &pstate->fileset,
						   name);
		snprintf(name, sizeof(name), "o%dof%d", i, hashtable->nbatch);
		accessor->outer_tuples =
			sts_initialize(ParallelHashJoinBatchOuter(shared,
													  pstate->nparticipants),
						   pstate->nparticipants,
						   ParallelWorkerNumber + 1,
						   sizeof(uint32),
						   SHARED_TUPLESTORE_SINGLE_PASS,
						   &pstate->fileset,
						   name);
	}

	MemoryContextSwitchTo(oldcxt);
}

/*
 * Free the current set of ParallelHashJoinBatchAccessor objects.
 */
static void
ExecParallelHashCloseBatchAccessors(HashJoinTable hashtable)
{
	int			i;

	for (i = 0; i < hashtable->nbatch; ++i)
	{
		/* Make sure no files are left open. */
		sts_end_write(hashtable->batches[i].inner_tuples);
		sts_end_write(hashtable->batches[i].outer_tuples);
		sts_end_parallel_scan(hashtable->batches[i].inner_tuples);
		sts_end_parallel_scan(hashtable->batches[i].outer_tuples);
	}
	pfree(hashtable->batches);
	hashtable->batches = NULL;
}

/*
 * Make sure this backend has up-to-date accessors for the current set of
 * batches.
 */
static void
ExecParallelHashEnsureBatchAccessors(HashJoinTable hashtable)
{
	ParallelHashJoinState *pstate = hashtable->parallel_state;
	ParallelHashJoinBatch *batches;
	MemoryContext oldcxt;
	int			i;

	if (hashtable->batches != NULL)
	{
		if (hashtable->nbatch == pstate->nbatch)
			return;
		ExecParallelHashCloseBatchAccessors(hashtable);
	}

	/*
	 * We should never see a state where the batch-tracking array is freed,
	 * because we should have given up sooner if we join when the build
	 * barrier has reached the PHJ_BUILD_DONE phase.
	 */
	Assert(DsaPointerIsValid(pstate->batches));

	/* Use hash join memory context. */
	oldcxt = MemoryContextSwitchTo(hashtable->hashCxt);

	/* Allocate this backend's accessor array. */
	hashtable->nbatch = pstate->nbatch;
	hashtable->batches = (ParallelHashJoinBatchAccessor *)
		palloc0(sizeof(ParallelHashJoinBatchAccessor) * hashtable->nbatch);

	/* Find the base of the pseudo-array of ParallelHashJoinBatch objects. */
	batches = (ParallelHashJoinBatch *)
		dsa_get_address(hashtable->area, pstate->batches);

	/* Set up the accessor array and attach to the tuplestores. */
	for (i = 0; i < hashtable->nbatch; ++i)
	{
		ParallelHashJoinBatchAccessor *accessor = &hashtable->batches[i];
		ParallelHashJoinBatch *shared = NthParallelHashJoinBatch(batches, i);

		accessor->shared = shared;
		accessor->preallocated = 0;
		accessor->done = false;
		accessor->inner_tuples =
			sts_attach(ParallelHashJoinBatchInner(shared),
					   ParallelWorkerNumber + 1,
					   &pstate->fileset);
		accessor->outer_tuples =
			sts_attach(ParallelHashJoinBatchOuter(shared,
												  pstate->nparticipants),
					   ParallelWorkerNumber + 1,
					   &pstate->fileset);
	}

	MemoryContextSwitchTo(oldcxt);
}

/*
 * Allocate an empty shared memory hash table for a given batch.
 */
void
ExecParallelHashTableAlloc(HashJoinTable hashtable, int batchno)
{
	ParallelHashJoinBatch *batch = hashtable->batches[batchno].shared;
	dsa_pointer_atomic *buckets;
	int			nbuckets = hashtable->parallel_state->nbuckets;
	int			i;

	batch->buckets =
		dsa_allocate(hashtable->area, sizeof(dsa_pointer_atomic) * nbuckets);
	buckets = (dsa_pointer_atomic *)
		dsa_get_address(hashtable->area, batch->buckets);
	for (i = 0; i < nbuckets; ++i)
		dsa_pointer_atomic_init(&buckets[i], InvalidDsaPointer);
}

/*
 * If we are currently attached to a shared hash join batch, detach.  If we
 * are last to detach, clean up.
 */
void
ExecHashTableDetachBatch(HashJoinTable hashtable)
{
	if (hashtable->parallel_state != NULL &&
		hashtable->curbatch >= 0)
	{
		int			curbatch = hashtable->curbatch;
		ParallelHashJoinBatch *batch = hashtable->batches[curbatch].shared;

		/* Make sure any temporary files are closed. */
		sts_end_parallel_scan(hashtable->batches[curbatch].inner_tuples);
		sts_end_parallel_scan(hashtable->batches[curbatch].outer_tuples);

		/* Detach from the batch we were last working on. */
		if (BarrierArriveAndDetach(&batch->batch_barrier))
		{
			/*
			 * Technically we shouldn't access the barrier because we're no
			 * longer attached, but since there is no way it's moving after
			 * this point it seems safe to make the following assertion.
			 */
			Assert(BarrierPhase(&batch->batch_barrier) == PHJ_BATCH_DONE);

			/* Free shared chunks and buckets. */
			while (DsaPointerIsValid(batch->chunks))
			{
				HashMemoryChunk chunk =
				dsa_get_address(hashtable->area, batch->chunks);
				dsa_pointer next = chunk->next.shared;

				dsa_free(hashtable->area, batch->chunks);
				batch->chunks = next;
			}
			if (DsaPointerIsValid(batch->buckets))
			{
				dsa_free(hashtable->area, batch->buckets);
				batch->buckets = InvalidDsaPointer;
			}
		}

		/*
		 * Track the largest batch we've been attached to.  Though each
		 * backend might see a different subset of batches, explain.c will
		 * scan the results from all backends to find the largest value.
		 */
		hashtable->spacePeak =
			Max(hashtable->spacePeak,
				batch->size + sizeof(dsa_pointer_atomic) * hashtable->nbuckets);

		/* Remember that we are not attached to a batch. */
		hashtable->curbatch = -1;
	}
}

/*
 * Detach from all shared resources.  If we are last to detach, clean up.
 */
void
ExecHashTableDetach(HashJoinTable hashtable)
{
	ParallelHashJoinState *pstate = hashtable->parallel_state;

	/*
	 * If we're involved in a parallel query, we must either have gotten all
	 * the way to PHJ_BUILD_RUNNING, or joined too late and be in
	 * PHJ_BUILD_DONE.
	 */
	Assert(!pstate ||
		   BarrierPhase(&pstate->build_barrier) >= PHJ_BUILD_RUNNING);

	if (pstate && BarrierPhase(&pstate->build_barrier) == PHJ_BUILD_RUNNING)
	{
		int			i;

		/* Make sure any temporary files are closed. */
		if (hashtable->batches)
		{
			for (i = 0; i < hashtable->nbatch; ++i)
			{
				sts_end_write(hashtable->batches[i].inner_tuples);
				sts_end_write(hashtable->batches[i].outer_tuples);
				sts_end_parallel_scan(hashtable->batches[i].inner_tuples);
				sts_end_parallel_scan(hashtable->batches[i].outer_tuples);
			}
		}

		/* If we're last to detach, clean up shared memory. */
		if (BarrierArriveAndDetach(&pstate->build_barrier))
		{
			/*
			 * Late joining processes will see this state and give up
			 * immediately.
			 */
			Assert(BarrierPhase(&pstate->build_barrier) == PHJ_BUILD_DONE);

			if (DsaPointerIsValid(pstate->batches))
			{
				dsa_free(hashtable->area, pstate->batches);
				pstate->batches = InvalidDsaPointer;
			}
		}
	}
	hashtable->parallel_state = NULL;
}

/*
 * Get the first tuple in a given bucket identified by number.
 */
static inline HashJoinTuple
ExecParallelHashFirstTuple(HashJoinTable hashtable, int bucketno)
{
	HashJoinTuple tuple;
	dsa_pointer p;

	Assert(hashtable->parallel_state);
	p = dsa_pointer_atomic_read(&hashtable->buckets.shared[bucketno]);
	tuple = (HashJoinTuple) dsa_get_address(hashtable->area, p);

	return tuple;
}

/*
 * Get the next tuple in the same bucket as 'tuple'.
 */
static inline HashJoinTuple
ExecParallelHashNextTuple(HashJoinTable hashtable, HashJoinTuple tuple)
{
	HashJoinTuple next;

	Assert(hashtable->parallel_state);
	next = (HashJoinTuple) dsa_get_address(hashtable->area, tuple->next.shared);

	return next;
}

/*
 * Insert a tuple at the front of a chain of tuples in DSA memory atomically.
 */
static inline void
ExecParallelHashPushTuple(dsa_pointer_atomic *head,
						  HashJoinTuple tuple,
						  dsa_pointer tuple_shared)
{
	for (;;)
	{
		tuple->next.shared = dsa_pointer_atomic_read(head);
		if (dsa_pointer_atomic_compare_exchange(head,
												&tuple->next.shared,
												tuple_shared))
			break;
	}
}

/*
 * Prepare to work on a given batch.
 */
void
ExecParallelHashTableSetCurrentBatch(HashJoinTable hashtable, int batchno)
{
	Assert(hashtable->batches[batchno].shared->buckets != InvalidDsaPointer);

	hashtable->curbatch = batchno;
	hashtable->buckets.shared = (dsa_pointer_atomic *)
		dsa_get_address(hashtable->area,
						hashtable->batches[batchno].shared->buckets);
	hashtable->nbuckets = hashtable->parallel_state->nbuckets;
	hashtable->log2_nbuckets = my_log2(hashtable->nbuckets);
	hashtable->current_chunk = NULL;
	hashtable->current_chunk_shared = InvalidDsaPointer;
	hashtable->batches[batchno].at_least_one_chunk = false;
}

/*
 * Take the next available chunk from the queue of chunks being worked on in
 * parallel.  Return NULL if there are none left.  Otherwise return a pointer
 * to the chunk, and set *shared to the DSA pointer to the chunk.
 */
static HashMemoryChunk
ExecParallelHashPopChunkQueue(HashJoinTable hashtable, dsa_pointer *shared)
{
	ParallelHashJoinState *pstate = hashtable->parallel_state;
	HashMemoryChunk chunk;

	LWLockAcquire(&pstate->lock, LW_EXCLUSIVE);
	if (DsaPointerIsValid(pstate->chunk_work_queue))
	{
		*shared = pstate->chunk_work_queue;
		chunk = (HashMemoryChunk)
			dsa_get_address(hashtable->area, *shared);
		pstate->chunk_work_queue = chunk->next.shared;
	}
	else
		chunk = NULL;
	LWLockRelease(&pstate->lock);

	return chunk;
}

/*
 * Increase the space preallocated in this backend for a given inner batch by
 * at least a given amount.  This allows us to track whether a given batch
 * would fit in memory when loaded back in.  Also increase the number of
 * batches or buckets if required.
 *
 * This maintains a running estimation of how much space will be taken when we
 * load the batch back into memory by simulating the way chunks will be handed
 * out to workers.  It's not perfectly accurate because the tuples will be
 * packed into memory chunks differently by ExecParallelHashTupleAlloc(), but
 * it should be pretty close.  It tends to overestimate by a fraction of a
 * chunk per worker since all workers gang up to preallocate during hashing,
 * but workers tend to reload batches alone if there are enough to go around,
 * leaving fewer partially filled chunks.  This effect is bounded by
 * nparticipants.
 *
 * Return false if the number of batches or buckets has changed, and the
 * caller should reconsider which batch a given tuple now belongs in and call
 * again.
 */
static bool
ExecParallelHashTuplePrealloc(HashJoinTable hashtable, int batchno, size_t size)
{
	ParallelHashJoinState *pstate = hashtable->parallel_state;
	ParallelHashJoinBatchAccessor *batch = &hashtable->batches[batchno];
	size_t		want = Max(size, HASH_CHUNK_SIZE - HASH_CHUNK_HEADER_SIZE);

	Assert(batchno > 0);
	Assert(batchno < hashtable->nbatch);
	Assert(size == MAXALIGN(size));

	LWLockAcquire(&pstate->lock, LW_EXCLUSIVE);

	/* Has another participant commanded us to help grow? */
	if (pstate->growth == PHJ_GROWTH_NEED_MORE_BATCHES ||
		pstate->growth == PHJ_GROWTH_NEED_MORE_BUCKETS)
	{
		ParallelHashGrowth growth = pstate->growth;

		LWLockRelease(&pstate->lock);
		if (growth == PHJ_GROWTH_NEED_MORE_BATCHES)
			ExecParallelHashIncreaseNumBatches(hashtable);
		else if (growth == PHJ_GROWTH_NEED_MORE_BUCKETS)
			ExecParallelHashIncreaseNumBuckets(hashtable);

		return false;
	}

	if (pstate->growth != PHJ_GROWTH_DISABLED &&
		batch->at_least_one_chunk &&
		(batch->shared->estimated_size + want + HASH_CHUNK_HEADER_SIZE
		 > pstate->space_allowed))
	{
		/*
		 * We have determined that this batch would exceed the space budget if
		 * loaded into memory.  Command all participants to help repartition.
		 */
		batch->shared->space_exhausted = true;
		pstate->growth = PHJ_GROWTH_NEED_MORE_BATCHES;
		LWLockRelease(&pstate->lock);

		return false;
	}

	batch->at_least_one_chunk = true;
	batch->shared->estimated_size += want + HASH_CHUNK_HEADER_SIZE;
	batch->preallocated = want;
	LWLockRelease(&pstate->lock);

	return true;
}
