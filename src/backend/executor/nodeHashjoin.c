/*-------------------------------------------------------------------------
 *
 * nodeHashjoin.c
 *	  Routines to handle hash join nodes
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeHashjoin.c
 *
 * PARALLELISM
 *
 * Hash joins can participate in parallel query execution in several ways.  A
 * parallel-oblivious hash join is one where the node is unaware that it is
 * part of a parallel plan.  In this case, a copy of the inner plan is used to
 * build a copy of the hash table in every backend, and the outer plan could
 * either be built from a partial or complete path, so that the results of the
 * hash join are correspondingly either partial or complete.  A parallel-aware
 * hash join is one that behaves differently, coordinating work between
 * backends, and appears as Parallel Hash Join in EXPLAIN output.  A Parallel
 * Hash Join always appears with a Parallel Hash node.
 *
 * Parallel-aware hash joins use the same per-backend state machine to track
 * progress through the hash join algorithm as parallel-oblivious hash joins.
 * In a parallel-aware hash join, there is also a shared state machine that
 * co-operating backends use to synchronize their local state machines and
 * program counters.  The shared state machine is managed with a Barrier IPC
 * primitive.  When all attached participants arrive at a barrier, the phase
 * advances and all waiting participants are released.
 *
 * When a participant begins working on a parallel hash join, it must first
 * figure out how much progress has already been made, because participants
 * don't wait for each other to begin.  For this reason there are switch
 * statements at key points in the code where we have to synchronize our local
 * state machine with the phase, and then jump to the correct part of the
 * algorithm so that we can get started.
 *
 * One barrier called build_barrier is used to coordinate the hashing phases.
 * The phase is represented by an integer which begins at zero and increments
 * one by one, but in the code it is referred to by symbolic names as follows:
 *
 *   PHJ_BUILD_ELECTING              -- initial state
 *   PHJ_BUILD_ALLOCATING            -- one sets up the batches and table 0
 *   PHJ_BUILD_HASHING_INNER         -- all hash the inner rel
 *   PHJ_BUILD_HASHING_OUTER         -- (multi-batch only) all hash the outer
 *   PHJ_BUILD_DONE                  -- building done, probing can begin
 *
 * While in the phase PHJ_BUILD_HASHING_INNER a separate pair of barriers may
 * be used repeatedly as required to coordinate expansions in the number of
 * batches or buckets.  Their phases are as follows:
 *
 *   PHJ_GROW_BATCHES_ELECTING       -- initial state
 *   PHJ_GROW_BATCHES_ALLOCATING     -- one allocates new batches
 *   PHJ_GROW_BATCHES_REPARTITIONING -- all repartition
 *   PHJ_GROW_BATCHES_FINISHING      -- one cleans up, detects skew
 *
 *   PHJ_GROW_BUCKETS_ELECTING       -- initial state
 *   PHJ_GROW_BUCKETS_ALLOCATING     -- one allocates new buckets
 *   PHJ_GROW_BUCKETS_REINSERTING    -- all insert tuples
 *
 * If the planner got the number of batches and buckets right, those won't be
 * necessary, but on the other hand we might finish up needing to expand the
 * buckets or batches multiple times while hashing the inner relation to stay
 * within our memory budget and load factor target.  For that reason it's a
 * separate pair of barriers using circular phases.
 *
 * The PHJ_BUILD_HASHING_OUTER phase is required only for multi-batch joins,
 * because we need to divide the outer relation into batches up front in order
 * to be able to process batches entirely independently.  In contrast, the
 * parallel-oblivious algorithm simply throws tuples 'forward' to 'later'
 * batches whenever it encounters them while scanning and probing, which it
 * can do because it processes batches in serial order.
 *
 * Once PHJ_BUILD_DONE is reached, backends then split up and process
 * different batches, or gang up and work together on probing batches if there
 * aren't enough to go around.  For each batch there is a separate barrier
 * with the following phases:
 *
 *  PHJ_BATCH_ELECTING       -- initial state
 *  PHJ_BATCH_ALLOCATING     -- one allocates buckets
 *  PHJ_BATCH_LOADING        -- all load the hash table from disk
 *  PHJ_BATCH_PROBING        -- all probe
 *  PHJ_BATCH_DONE           -- end
 *
 * Batch 0 is a special case, because it starts out in phase
 * PHJ_BATCH_PROBING; populating batch 0's hash table is done during
 * PHJ_BUILD_HASHING_INNER so we can skip loading.
 *
 * Initially we try to plan for a single-batch hash join using the combined
 * hash_mem of all participants to create a large shared hash table.  If that
 * turns out either at planning or execution time to be impossible then we
 * fall back to regular hash_mem sized hash tables.
 * If a given batch causes the number of batches to be doubled and data skew
 * causes too few or too many tuples to be relocated to the child of this batch,
 * the batch which is now home to the skewed tuples is marked as a "fallback"
 * batch. This means that it will be processed using multiple loops --
 * each loop probing an arbitrary stripe of tuples from this batch
 * which fit in hash_mem or combined hash_mem.
 * This batch is no longer permitted to cause growth in the number of batches.
 *
 * When the inner side of a fallback batch is loaded into memory, stripes of
 * arbitrary tuples totaling hash_mem or combined hash_mem in size are loaded
 * into the hashtable. After probing this stripe, the outer side batch is
 * rewound and the next stripe is loaded. Each stripe of the inner batch is
 * probed until all tuples from that batch have been processed.
 *
 * Tuples that match are emitted (depending on the join semantics of the
 * particular join type) during probing of the stripe. However, in order to make
 * left outer join work, unmatched tuples cannot be emitted NULL-extended until
 * all stripes have been probed. To address this, a bitmap is created with a bit
 * for each tuple of the outer side. If a tuple on the outer side matches a
 * tuple from the inner, the corresponding bit is set. At the end of probing all
 * stripes, the executor scans the bitmap and emits unmatched outer tuples.
 *
 * To avoid deadlocks, we never wait for any barrier unless it is known that
 * all other backends attached to it are actively executing the node or have
 * already arrived.  Practically, that means that we never return a tuple
 * while attached to a barrier, unless the barrier has reached its final
 * state.  In the slightly special case of the per-batch barrier, we return
 * tuples while in PHJ_BATCH_PROBING phase, but that's OK because we use
 * BarrierArriveAndDetach() to advance it to PHJ_BATCH_DONE without waiting.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/parallel.h"
#include "executor/executor.h"
#include "executor/hashjoin.h"
#include "executor/nodeHash.h"
#include "executor/nodeHashjoin.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/memutils.h"
#include "utils/sharedtuplestore.h"


/*
 * States of the ExecHashJoin state machine
 */
#define HJ_BUILD_HASHTABLE		1
#define HJ_NEED_NEW_OUTER		2
#define HJ_SCAN_BUCKET			3
#define HJ_FILL_OUTER_TUPLE		4
#define HJ_FILL_INNER_TUPLES	5
#define HJ_NEED_NEW_STRIPE      6

/* Returns true if doing null-fill on outer relation */
#define HJ_FILL_OUTER(hjstate)	((hjstate)->hj_NullInnerTupleSlot != NULL)
/* Returns true if doing null-fill on inner relation */
#define HJ_FILL_INNER(hjstate)	((hjstate)->hj_NullOuterTupleSlot != NULL)

static TupleTableSlot *ExecHashJoinOuterGetTuple(PlanState *outerNode,
												 HashJoinState *hjstate,
												 uint32 *hashvalue);
static TupleTableSlot *ExecParallelHashJoinOuterGetTuple(PlanState *outerNode,
														 HashJoinState *hjstate,
														 uint32 *hashvalue);
static TupleTableSlot *ExecHashJoinGetSavedTuple(HashJoinState *hjstate,
												 BufFile *file,
												 uint32 *hashvalue,
												 TupleTableSlot *tupleSlot);
static int	ExecHashJoinLoadStripe(HashJoinState *hjstate);
static bool ExecHashJoinNewBatch(HashJoinState *hjstate);
static bool ExecParallelHashJoinNewBatch(HashJoinState *hjstate);
static bool ExecParallelHashJoinLoadStripe(HashJoinState *hjstate);
static void ExecParallelHashJoinPartitionOuter(HashJoinState *node);
static bool checkbit(HashJoinState *hjstate);
static void set_match_bit(HashJoinState *hjstate);

static pg_attribute_always_inline bool
			IsHashloopFallback(HashJoinTable hashtable);

#define UINT_BITS (sizeof(unsigned int) * CHAR_BIT)

static void
set_match_bit(HashJoinState *hjstate)
{
	HashJoinTable hashtable = hjstate->hj_HashTable;
	BufFile    *statusFile = hashtable->hashloopBatchFile[hashtable->curbatch];
	int			tupindex = hjstate->hj_CurNumOuterTuples - 1;
	size_t		unit_size = sizeof(hjstate->hj_CurOuterMatchStatus);
	off_t		offset = tupindex / UINT_BITS * unit_size;

	int			fileno;
	off_t		cursor;

	BufFileTell(statusFile, &fileno, &cursor);

	/* Extend the statusFile if this is stripe zero. */
	if (hashtable->curstripe == 0)
	{
		for (; cursor < offset + unit_size; cursor += unit_size)
		{
			hjstate->hj_CurOuterMatchStatus = 0;
			BufFileWrite(statusFile, &hjstate->hj_CurOuterMatchStatus, unit_size);
		}
	}

	if (cursor != offset)
		BufFileSeek(statusFile, 0, offset, SEEK_SET);

	BufFileRead(statusFile, &hjstate->hj_CurOuterMatchStatus, unit_size);
	BufFileSeek(statusFile, 0, -unit_size, SEEK_CUR);

	hjstate->hj_CurOuterMatchStatus |= 1U << tupindex % UINT_BITS;
	BufFileWrite(statusFile, &hjstate->hj_CurOuterMatchStatus, unit_size);
}

/* return true if bit is set and false if not */
static bool
checkbit(HashJoinState *hjstate)
{
	HashJoinTable hashtable = hjstate->hj_HashTable;
	int			curbatch = hashtable->curbatch;
	BufFile    *outer_match_statuses;

	int			bitno = hjstate->hj_EmitOuterTupleId % UINT_BITS;

	hjstate->hj_EmitOuterTupleId++;
	outer_match_statuses = hjstate->hj_HashTable->hashloopBatchFile[curbatch];

	/*
	 * if current chunk of bitmap is exhausted, read next chunk of bitmap from
	 * outer_match_status_file
	 */
	if (bitno == 0)
		BufFileRead(outer_match_statuses, &hjstate->hj_CurOuterMatchStatus,
					sizeof(hjstate->hj_CurOuterMatchStatus));

	/*
	 * check if current tuple's match bit is set in outer match status file
	 */
	return hjstate->hj_CurOuterMatchStatus & (1U << bitno);
}

static bool
IsHashloopFallback(HashJoinTable hashtable)
{
	if (hashtable->parallel_state)
		return hashtable->batches[hashtable->curbatch].shared->hashloop_fallback;

	if (!hashtable->hashloopBatchFile)
		return false;

	return hashtable->hashloopBatchFile[hashtable->curbatch];
}

/* ----------------------------------------------------------------
 *		ExecHashJoinImpl
 *
 *		This function implements the Hybrid Hashjoin algorithm.  It is marked
 *		with an always-inline attribute so that ExecHashJoin() and
 *		ExecParallelHashJoin() can inline it.  Compilers that respect the
 *		attribute should create versions specialized for parallel == true and
 *		parallel == false with unnecessary branches removed.
 *
 *		Note: the relation we build hash table on is the "inner"
 *			  the other one is "outer".
 * ----------------------------------------------------------------
 */
static pg_attribute_always_inline TupleTableSlot *
ExecHashJoinImpl(PlanState *pstate, bool parallel)
{
	HashJoinState *node = castNode(HashJoinState, pstate);
	PlanState  *outerNode;
	HashState  *hashNode;
	ExprState  *joinqual;
	ExprState  *otherqual;
	ExprContext *econtext;
	HashJoinTable hashtable;
	TupleTableSlot *outerTupleSlot;
	uint32		hashvalue;
	int			batchno;
	ParallelHashJoinState *parallel_state;

	/*
	 * get information from HashJoin node
	 */
	joinqual = node->js.joinqual;
	otherqual = node->js.ps.qual;
	hashNode = (HashState *) innerPlanState(node);
	outerNode = outerPlanState(node);
	hashtable = node->hj_HashTable;
	econtext = node->js.ps.ps_ExprContext;
	parallel_state = hashNode->parallel_state;

	/*
	 * Reset per-tuple memory context to free any expression evaluation
	 * storage allocated in the previous tuple cycle.
	 */
	ResetExprContext(econtext);

	/*
	 * run the hash join state machine
	 */
	for (;;)
	{
		/*
		 * It's possible to iterate this loop many times before returning a
		 * tuple, in some pathological cases such as needing to move much of
		 * the current batch to a later batch.  So let's check for interrupts
		 * each time through.
		 */
		CHECK_FOR_INTERRUPTS();

		switch (node->hj_JoinState)
		{
			case HJ_BUILD_HASHTABLE:

				/*
				 * First time through: build hash table for inner relation.
				 */
				Assert(hashtable == NULL);

				/*
				 * If the outer relation is completely empty, and it's not
				 * right/full join, we can quit without building the hash
				 * table.  However, for an inner join it is only a win to
				 * check this when the outer relation's startup cost is less
				 * than the projected cost of building the hash table.
				 * Otherwise it's best to build the hash table first and see
				 * if the inner relation is empty.  (When it's a left join, we
				 * should always make this check, since we aren't going to be
				 * able to skip the join on the strength of an empty inner
				 * relation anyway.)
				 *
				 * If we are rescanning the join, we make use of information
				 * gained on the previous scan: don't bother to try the
				 * prefetch if the previous scan found the outer relation
				 * nonempty. This is not 100% reliable since with new
				 * parameters the outer relation might yield different
				 * results, but it's a good heuristic.
				 *
				 * The only way to make the check is to try to fetch a tuple
				 * from the outer plan node.  If we succeed, we have to stash
				 * it away for later consumption by ExecHashJoinOuterGetTuple.
				 */
				if (HJ_FILL_INNER(node))
				{
					/* no chance to not build the hash table */
					node->hj_FirstOuterTupleSlot = NULL;
				}
				else if (parallel)
				{
					/*
					 * The empty-outer optimization is not implemented for
					 * shared hash tables, because no one participant can
					 * determine that there are no outer tuples, and it's not
					 * yet clear that it's worth the synchronization overhead
					 * of reaching consensus to figure that out.  So we have
					 * to build the hash table.
					 */
					node->hj_FirstOuterTupleSlot = NULL;
				}
				else if (HJ_FILL_OUTER(node) ||
						 (outerNode->plan->startup_cost < hashNode->ps.plan->total_cost &&
						  !node->hj_OuterNotEmpty))
				{
					node->hj_FirstOuterTupleSlot = ExecProcNode(outerNode);
					if (TupIsNull(node->hj_FirstOuterTupleSlot))
					{
						node->hj_OuterNotEmpty = false;
						return NULL;
					}
					else
						node->hj_OuterNotEmpty = true;
				}
				else
					node->hj_FirstOuterTupleSlot = NULL;

				/*
				 * Create the hash table.  If using Parallel Hash, then
				 * whoever gets here first will create the hash table and any
				 * later arrivals will merely attach to it.
				 */
				hashtable = ExecHashTableCreate(hashNode,
												node->hj_HashOperators,
												node->hj_Collations,
												HJ_FILL_INNER(node));
				node->hj_HashTable = hashtable;

				/*
				 * Execute the Hash node, to build the hash table.  If using
				 * Parallel Hash, then we'll try to help hashing unless we
				 * arrived too late.
				 */
				hashNode->hashtable = hashtable;
				(void) MultiExecProcNode((PlanState *) hashNode);

				/*
				 * After building the hashtable, stripe 0 of batch 0 will have
				 * been loaded.
				 */
				hashtable->curstripe = 0;

				/*
				 * If the inner relation is completely empty, and we're not
				 * doing a left outer join, we can quit without scanning the
				 * outer relation.
				 */
				if (hashtable->totalTuples == 0 && !HJ_FILL_OUTER(node))
					return NULL;

				/*
				 * need to remember whether nbatch has increased since we
				 * began scanning the outer relation
				 */
				hashtable->nbatch_outstart = hashtable->nbatch;

				/*
				 * Reset OuterNotEmpty for scan.  (It's OK if we fetched a
				 * tuple above, because ExecHashJoinOuterGetTuple will
				 * immediately set it again.)
				 */
				node->hj_OuterNotEmpty = false;

				if (parallel)
				{
					Barrier    *build_barrier;

					build_barrier = &parallel_state->build_barrier;
					Assert(BarrierPhase(build_barrier) == PHJ_BUILD_HASHING_OUTER ||
						   BarrierPhase(build_barrier) == PHJ_BUILD_DONE);
					if (BarrierPhase(build_barrier) == PHJ_BUILD_HASHING_OUTER)
					{
						/*
						 * If multi-batch, we need to hash the outer relation
						 * up front.
						 */
						if (hashtable->nbatch > 1)
							ExecParallelHashJoinPartitionOuter(node);
						BarrierArriveAndWait(build_barrier,
											 WAIT_EVENT_HASH_BUILD_HASH_OUTER);
					}
					Assert(BarrierPhase(build_barrier) == PHJ_BUILD_DONE);

					/* Each backend should now select a batch to work on. */
					hashtable->curbatch = -1;

					if (!ExecParallelHashJoinNewBatch(node))
						return NULL;
				}
				node->hj_JoinState = HJ_NEED_NEW_OUTER;

				/* FALL THRU */

			case HJ_NEED_NEW_OUTER:

				/*
				 * We don't have an outer tuple, try to get the next one
				 */
				if (parallel)
					outerTupleSlot =
						ExecParallelHashJoinOuterGetTuple(outerNode, node,
														  &hashvalue);
				else
					outerTupleSlot =
						ExecHashJoinOuterGetTuple(outerNode, node, &hashvalue);

				if (TupIsNull(outerTupleSlot))
				{
					/* end of batch, or maybe whole join */
					if (HJ_FILL_INNER(node))
					{
						/* set up to scan for unmatched inner tuples */
						ExecPrepHashTableForUnmatched(node);
						node->hj_JoinState = HJ_FILL_INNER_TUPLES;
					}
					else
						node->hj_JoinState = HJ_NEED_NEW_STRIPE;
					continue;
				}

				econtext->ecxt_outertuple = outerTupleSlot;

				/*
				 * Don't reset hj_MatchedOuter after the first stripe as it
				 * would cancel out whatever we found before
				 */
				if (node->hj_HashTable->curstripe == 0)
					node->hj_MatchedOuter = false;

				/*
				 * Find the corresponding bucket for this tuple in the main
				 * hash table or skew hash table.
				 */
				node->hj_CurHashValue = hashvalue;
				ExecHashGetBucketAndBatch(hashtable, hashvalue,
										  &node->hj_CurBucketNo, &batchno);
				node->hj_CurSkewBucketNo = ExecHashGetSkewBucket(hashtable,
																 hashvalue);
				node->hj_CurTuple = NULL;

				/*
				 * The tuple might not belong to the current batch (where
				 * "current batch" includes the skew buckets if any).
				 *
				 * This should only be done once per tuple per batch. If a
				 * batch "falls back", its inner side will be split into
				 * stripes. Any displaced outer tuples should only be
				 * relocated while probing the first stripe of the inner side.
				 */
				if (batchno != hashtable->curbatch &&
					node->hj_CurSkewBucketNo == INVALID_SKEW_BUCKET_NO &&
					node->hj_HashTable->curstripe == 0)
				{
					bool		shouldFree;
					MinimalTuple mintuple = ExecFetchSlotMinimalTuple(outerTupleSlot,
																	  &shouldFree);

					/*
					 * Need to postpone this outer tuple to a later batch.
					 * Save it in the corresponding outer-batch file.
					 */
					Assert(parallel_state == NULL);
					Assert(batchno > hashtable->curbatch);
					ExecHashJoinSaveTuple(mintuple, hashvalue,
										  &hashtable->outerBatchFile[batchno]);

					if (shouldFree)
						heap_free_minimal_tuple(mintuple);

					/* Loop around, staying in HJ_NEED_NEW_OUTER state */
					continue;
				}

				/*
				 * While probing the phantom stripe, don't increment
				 * hj_CurNumOuterTuples or extend the bitmap
				 */
				if (!parallel && hashtable->curstripe != PHANTOM_STRIPE)
					node->hj_CurNumOuterTuples++;

				/* OK, let's scan the bucket for matches */
				node->hj_JoinState = HJ_SCAN_BUCKET;

				/* FALL THRU */

			case HJ_SCAN_BUCKET:

				/*
				 * Scan the selected hash bucket for matches to current outer
				 */
				if (parallel)
				{
					if (!ExecParallelScanHashBucket(node, econtext))
					{
						/* out of matches; check for possible outer-join fill */
						node->hj_JoinState = HJ_FILL_OUTER_TUPLE;
						continue;
					}
				}
				else
				{
					if (!ExecScanHashBucket(node, econtext))
					{
						/* out of matches; check for possible outer-join fill */
						node->hj_JoinState = HJ_FILL_OUTER_TUPLE;
						continue;
					}
				}

				/*
				 * We've got a match, but still need to test non-hashed quals.
				 * ExecScanHashBucket already set up all the state needed to
				 * call ExecQual.
				 *
				 * If we pass the qual, then save state for next call and have
				 * ExecProject form the projection, store it in the tuple
				 * table, and return the slot.
				 *
				 * Only the joinquals determine tuple match status, but all
				 * quals must pass to actually return the tuple.
				 */
				if (joinqual == NULL || ExecQual(joinqual, econtext))
				{
					node->hj_MatchedOuter = true;

					if (HJ_FILL_OUTER(node) && IsHashloopFallback(hashtable))
					{
						/*
						 * Each bit corresponds to a single tuple. Setting the
						 * match bit keeps track of which tuples were matched
						 * for batches which are using the block nested
						 * hashloop fallback method. It persists this match
						 * status across multiple stripes of tuples, each of
						 * which is loaded into the hashtable and probed. The
						 * outer match status file is the cumulative match
						 * status of outer tuples for a given batch across all
						 * stripes of that inner side batch.
						 */
						if (parallel)
							sb_setbit(hashtable->batches[hashtable->curbatch].sba, econtext->ecxt_outertuple->tts_tuplenum);
						else
							set_match_bit(node);
					}

					if (parallel)
					{
						/*
						 * Full/right outer joins are currently not supported
						 * for parallel joins, so we don't need to set the
						 * match bit.  Experiments show that it's worth
						 * avoiding the shared memory traffic on large
						 * systems.
						 */
						Assert(!HJ_FILL_INNER(node));
					}
					else
					{
						/*
						 * This is really only needed if HJ_FILL_INNER(node),
						 * but we'll avoid the branch and just set it always.
						 */
						HeapTupleHeaderSetMatch(HJTUPLE_MINTUPLE(node->hj_CurTuple));
					}

					/* In an antijoin, we never return a matched tuple */
					if (node->js.jointype == JOIN_ANTI)
					{
						node->hj_JoinState = HJ_NEED_NEW_OUTER;
						continue;
					}

					/*
					 * If we only need to join to the first matching inner
					 * tuple, then consider returning this one, but after that
					 * continue with next outer tuple.
					 */
					if (node->js.single_match)
					{
						node->hj_JoinState = HJ_NEED_NEW_OUTER;

						/*
						 * Only consider returning the tuple while on the
						 * first stripe.
						 */
						if (node->hj_HashTable->curstripe != 0)
							continue;
					}

					if (otherqual == NULL || ExecQual(otherqual, econtext))
						return ExecProject(node->js.ps.ps_ProjInfo);
					else
						InstrCountFiltered2(node, 1);
				}
				else
					InstrCountFiltered1(node, 1);
				break;

			case HJ_FILL_OUTER_TUPLE:

				/*
				 * The current outer tuple has run out of matches, so check
				 * whether to emit a dummy outer-join tuple.  Whether we emit
				 * one or not, the next state is NEED_NEW_OUTER.
				 */
				node->hj_JoinState = HJ_NEED_NEW_OUTER;

				if (IsHashloopFallback(hashtable) && HJ_FILL_OUTER(node))
				{
					if (hashtable->curstripe != PHANTOM_STRIPE)
						continue;

					if (parallel)
					{
						ParallelHashJoinBatchAccessor *accessor =
						&node->hj_HashTable->batches[node->hj_HashTable->curbatch];

						node->hj_MatchedOuter = sb_checkbit(accessor->sba, econtext->ecxt_outertuple->tts_tuplenum);
					}
					else
						node->hj_MatchedOuter = checkbit(node);
				}

				if (!node->hj_MatchedOuter &&
					HJ_FILL_OUTER(node))
				{
					/*
					 * Generate a fake join tuple with nulls for the inner
					 * tuple, and return it if it passes the non-join quals.
					 */
					econtext->ecxt_innertuple = node->hj_NullInnerTupleSlot;

					if (otherqual == NULL || ExecQual(otherqual, econtext))
						return ExecProject(node->js.ps.ps_ProjInfo);
					else
						InstrCountFiltered2(node, 1);
				}
				break;

			case HJ_FILL_INNER_TUPLES:

				/*
				 * We have finished a batch, but we are doing right/full join,
				 * so any unmatched inner tuples in the hashtable have to be
				 * emitted before we continue to the next batch.
				 */
				if (!ExecScanHashTableForUnmatched(node, econtext))
				{
					/* no more unmatched tuples */
					node->hj_JoinState = HJ_NEED_NEW_STRIPE;
					continue;
				}

				/*
				 * Generate a fake join tuple with nulls for the outer tuple,
				 * and return it if it passes the non-join quals.
				 */
				econtext->ecxt_outertuple = node->hj_NullOuterTupleSlot;

				if (otherqual == NULL || ExecQual(otherqual, econtext))
					return ExecProject(node->js.ps.ps_ProjInfo);
				else
					InstrCountFiltered2(node, 1);
				break;

			case HJ_NEED_NEW_STRIPE:

				/*
				 * Try to advance to next stripe. Then try to advance to the
				 * next batch if there are no more stripes in this batch. Done
				 * if there are no more batches.
				 */
				if (parallel)
				{
					if (!ExecParallelHashJoinLoadStripe(node) &&
						!ExecParallelHashJoinNewBatch(node))
						return NULL;	/* end of parallel-aware join */
				}
				else
				{
					if (!ExecHashJoinLoadStripe(node) &&
						!ExecHashJoinNewBatch(node))
						return NULL;	/* end of parallel-oblivious join */
				}
				node->hj_JoinState = HJ_NEED_NEW_OUTER;
				break;

			default:
				elog(ERROR, "unrecognized hashjoin state: %d",
					 (int) node->hj_JoinState);
		}
	}
}

/* ----------------------------------------------------------------
 *		ExecHashJoin
 *
 *		Parallel-oblivious version.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *			/* return: a tuple or NULL */
ExecHashJoin(PlanState *pstate)
{
	/*
	 * On sufficiently smart compilers this should be inlined with the
	 * parallel-aware branches removed.
	 */
	return ExecHashJoinImpl(pstate, false);
}

/* ----------------------------------------------------------------
 *		ExecParallelHashJoin
 *
 *		Parallel-aware version.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *			/* return: a tuple or NULL */
ExecParallelHashJoin(PlanState *pstate)
{
	/*
	 * On sufficiently smart compilers this should be inlined with the
	 * parallel-oblivious branches removed.
	 */
	return ExecHashJoinImpl(pstate, true);
}

/* ----------------------------------------------------------------
 *		ExecInitHashJoin
 *
 *		Init routine for HashJoin node.
 * ----------------------------------------------------------------
 */
HashJoinState *
ExecInitHashJoin(HashJoin *node, EState *estate, int eflags)
{
	HashJoinState *hjstate;
	Plan	   *outerNode;
	Hash	   *hashNode;
	TupleDesc	outerDesc,
				innerDesc;
	const TupleTableSlotOps *ops;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * create state structure
	 */
	hjstate = makeNode(HashJoinState);
	hjstate->js.ps.plan = (Plan *) node;
	hjstate->js.ps.state = estate;

	/*
	 * See ExecHashJoinInitializeDSM() and ExecHashJoinInitializeWorker()
	 * where this function may be replaced with a parallel version, if we
	 * managed to launch a parallel query.
	 */
	hjstate->js.ps.ExecProcNode = ExecHashJoin;
	hjstate->js.jointype = node->join.jointype;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &hjstate->js.ps);

	/*
	 * initialize child nodes
	 *
	 * Note: we could suppress the REWIND flag for the inner input, which
	 * would amount to betting that the hash will be a single batch.  Not
	 * clear if this would be a win or not.
	 */
	outerNode = outerPlan(node);
	hashNode = (Hash *) innerPlan(node);

	outerPlanState(hjstate) = ExecInitNode(outerNode, estate, eflags);
	outerDesc = ExecGetResultType(outerPlanState(hjstate));
	innerPlanState(hjstate) = ExecInitNode((Plan *) hashNode, estate, eflags);
	innerDesc = ExecGetResultType(innerPlanState(hjstate));

	/*
	 * Initialize result slot, type and projection.
	 */
	ExecInitResultTupleSlotTL(&hjstate->js.ps, &TTSOpsVirtual);
	ExecAssignProjectionInfo(&hjstate->js.ps, NULL);

	/*
	 * tuple table initialization
	 */
	ops = ExecGetResultSlotOps(outerPlanState(hjstate), NULL);
	hjstate->hj_OuterTupleSlot = ExecInitExtraTupleSlot(estate, outerDesc,
														ops);

	/*
	 * detect whether we need only consider the first matching inner tuple
	 */
	hjstate->js.single_match = (node->join.inner_unique ||
								node->join.jointype == JOIN_SEMI);

	/* set up null tuples for outer joins, if needed */
	switch (node->join.jointype)
	{
		case JOIN_INNER:
		case JOIN_SEMI:
			break;
		case JOIN_LEFT:
		case JOIN_ANTI:
			hjstate->hj_NullInnerTupleSlot =
				ExecInitNullTupleSlot(estate, innerDesc, &TTSOpsVirtual);
			break;
		case JOIN_RIGHT:
			hjstate->hj_NullOuterTupleSlot =
				ExecInitNullTupleSlot(estate, outerDesc, &TTSOpsVirtual);
			break;
		case JOIN_FULL:
			hjstate->hj_NullOuterTupleSlot =
				ExecInitNullTupleSlot(estate, outerDesc, &TTSOpsVirtual);
			hjstate->hj_NullInnerTupleSlot =
				ExecInitNullTupleSlot(estate, innerDesc, &TTSOpsVirtual);
			break;
		default:
			elog(ERROR, "unrecognized join type: %d",
				 (int) node->join.jointype);
	}

	/*
	 * now for some voodoo.  our temporary tuple slot is actually the result
	 * tuple slot of the Hash node (which is our inner plan).  we can do this
	 * because Hash nodes don't return tuples via ExecProcNode() -- instead
	 * the hash join node uses ExecScanHashBucket() to get at the contents of
	 * the hash table.  -cim 6/9/91
	 */
	{
		HashState  *hashstate = (HashState *) innerPlanState(hjstate);
		TupleTableSlot *slot = hashstate->ps.ps_ResultTupleSlot;

		hjstate->hj_HashTupleSlot = slot;
	}

	/*
	 * initialize child expressions
	 */
	hjstate->js.ps.qual =
		ExecInitQual(node->join.plan.qual, (PlanState *) hjstate);
	hjstate->js.joinqual =
		ExecInitQual(node->join.joinqual, (PlanState *) hjstate);
	hjstate->hashclauses =
		ExecInitQual(node->hashclauses, (PlanState *) hjstate);

	/*
	 * initialize hash-specific info
	 */
	hjstate->hj_HashTable = NULL;
	hjstate->hj_FirstOuterTupleSlot = NULL;

	hjstate->hj_CurHashValue = 0;
	hjstate->hj_CurBucketNo = 0;
	hjstate->hj_CurSkewBucketNo = INVALID_SKEW_BUCKET_NO;
	hjstate->hj_CurTuple = NULL;

	hjstate->hj_OuterHashKeys = ExecInitExprList(node->hashkeys,
												 (PlanState *) hjstate);
	hjstate->hj_HashOperators = node->hashoperators;
	hjstate->hj_Collations = node->hashcollations;

	hjstate->hj_JoinState = HJ_BUILD_HASHTABLE;
	hjstate->hj_MatchedOuter = false;
	hjstate->hj_OuterNotEmpty = false;
	hjstate->hj_CurNumOuterTuples = 0;
	hjstate->hj_CurOuterMatchStatus = 0;

	return hjstate;
}

/* ----------------------------------------------------------------
 *		ExecEndHashJoin
 *
 *		clean up routine for HashJoin node
 * ----------------------------------------------------------------
 */
void
ExecEndHashJoin(HashJoinState *node)
{
	/*
	 * Free hash table
	 */
	if (node->hj_HashTable)
	{
		ExecHashTableDestroy(node->hj_HashTable);
		node->hj_HashTable = NULL;
	}

	/*
	 * Free the exprcontext
	 */
	ExecFreeExprContext(&node->js.ps);

	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(node->js.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->hj_OuterTupleSlot);
	ExecClearTuple(node->hj_HashTupleSlot);

	/*
	 * clean up subtrees
	 */
	ExecEndNode(outerPlanState(node));
	ExecEndNode(innerPlanState(node));
}

/*
 * ExecHashJoinOuterGetTuple
 *
 *		get the next outer tuple for a parallel oblivious hashjoin: either by
 *		executing the outer plan node in the first pass, or from the temp
 *		files for the hashjoin batches.
 *
 * Returns a null slot if no more outer tuples (within the current batch).
 *
 * On success, the tuple's hash value is stored at *hashvalue --- this is
 * either originally computed, or re-read from the temp file.
 */
static TupleTableSlot *
ExecHashJoinOuterGetTuple(PlanState *outerNode,
						  HashJoinState *hjstate,
						  uint32 *hashvalue)
{
	HashJoinTable hashtable = hjstate->hj_HashTable;
	int			curbatch = hashtable->curbatch;
	TupleTableSlot *slot;

	if (curbatch == 0)			/* if it is the first pass */
	{
		/*
		 * Check to see if first outer tuple was already fetched by
		 * ExecHashJoin() and not used yet.
		 */
		slot = hjstate->hj_FirstOuterTupleSlot;
		if (!TupIsNull(slot))
			hjstate->hj_FirstOuterTupleSlot = NULL;
		else
			slot = ExecProcNode(outerNode);

		while (!TupIsNull(slot))
		{
			/*
			 * We have to compute the tuple's hash value.
			 */
			ExprContext *econtext = hjstate->js.ps.ps_ExprContext;

			econtext->ecxt_outertuple = slot;
			if (ExecHashGetHashValue(hashtable, econtext,
									 hjstate->hj_OuterHashKeys,
									 true,	/* outer tuple */
									 HJ_FILL_OUTER(hjstate),
									 hashvalue))
			{
				/* remember outer relation is not empty for possible rescan */
				hjstate->hj_OuterNotEmpty = true;

				return slot;
			}

			/*
			 * That tuple couldn't match because of a NULL, so discard it and
			 * continue with the next one.
			 */
			slot = ExecProcNode(outerNode);
		}
	}
	else if (curbatch < hashtable->nbatch)
	{
		BufFile    *file = hashtable->outerBatchFile[curbatch];

		/*
		 * In outer-join cases, we could get here even though the batch file
		 * is empty.
		 */
		if (file == NULL)
			return NULL;

		slot = ExecHashJoinGetSavedTuple(hjstate,
										 file,
										 hashvalue,
										 hjstate->hj_OuterTupleSlot);
		if (!TupIsNull(slot))
			return slot;
	}

	/* End of this batch */
	return NULL;
}

/*
 * ExecHashJoinOuterGetTuple variant for the parallel case.
 */
static TupleTableSlot *
ExecParallelHashJoinOuterGetTuple(PlanState *outerNode,
								  HashJoinState *hjstate,
								  uint32 *hashvalue)
{
	HashJoinTable hashtable = hjstate->hj_HashTable;
	int			curbatch = hashtable->curbatch;
	TupleTableSlot *slot;

	/*
	 * In the Parallel Hash case we only run the outer plan directly for
	 * single-batch hash joins.  Otherwise we have to go to batch files, even
	 * for batch 0.
	 */
	if (curbatch == 0 && hashtable->nbatch == 1)
	{
		slot = ExecProcNode(outerNode);

		while (!TupIsNull(slot))
		{
			ExprContext *econtext = hjstate->js.ps.ps_ExprContext;

			econtext->ecxt_outertuple = slot;
			if (ExecHashGetHashValue(hashtable, econtext,
									 hjstate->hj_OuterHashKeys,
									 true,	/* outer tuple */
									 HJ_FILL_OUTER(hjstate),
									 hashvalue))
				return slot;

			/*
			 * That tuple couldn't match because of a NULL, so discard it and
			 * continue with the next one.
			 */
			slot = ExecProcNode(outerNode);
		}
	}
	else if (curbatch < hashtable->nbatch)
	{
		tupleMetadata metadata;
		MinimalTuple tuple;

		tuple = sts_parallel_scan_next(hashtable->batches[curbatch].outer_tuples,
									   &metadata);
		*hashvalue = metadata.hashvalue;

		if (tuple != NULL)
		{
			ExecForceStoreMinimalTuple(tuple,
									   hjstate->hj_OuterTupleSlot,
									   false);

			/*
			 * TODO: should we use tupleid instead of position in the serial
			 * case too?
			 */
			hjstate->hj_OuterTupleSlot->tts_tuplenum = metadata.tupleid;
			slot = hjstate->hj_OuterTupleSlot;
			return slot;
		}
		else
			ExecClearTuple(hjstate->hj_OuterTupleSlot);
	}

	/* End of this batch */
	return NULL;
}

/*
 * ExecHashJoinNewBatch
 *		switch to a new hashjoin batch
 *
 * Returns true if successful, false if there are no more batches.
 */
static bool
ExecHashJoinNewBatch(HashJoinState *hjstate)
{
	HashJoinTable hashtable = hjstate->hj_HashTable;
	int			nbatch;
	int			curbatch;
	BufFile    *innerFile = NULL;
	BufFile    *outerFile = NULL;

	nbatch = hashtable->nbatch;
	curbatch = hashtable->curbatch;

	/*
	 * We no longer need the previous outer batch file; close it right away to
	 * free disk space.
	 */
	if (hashtable->outerBatchFile && hashtable->outerBatchFile[curbatch])
	{
		BufFileClose(hashtable->outerBatchFile[curbatch]);
		hashtable->outerBatchFile[curbatch] = NULL;
	}
	if (IsHashloopFallback(hashtable))
	{
		BufFileClose(hashtable->hashloopBatchFile[curbatch]);
		hashtable->hashloopBatchFile[curbatch] = NULL;
	}

	/*
	 * We are surely done with the inner batch file now
	 */
	if (hashtable->innerBatchFile && hashtable->innerBatchFile[curbatch])
	{
		BufFileClose(hashtable->innerBatchFile[curbatch]);
		hashtable->innerBatchFile[curbatch] = NULL;
	}

	if (curbatch == 0)			/* we just finished the first batch */
	{
		/*
		 * Reset some of the skew optimization state variables, since we no
		 * longer need to consider skew tuples after the first batch. The
		 * memory context reset we are about to do will release the skew
		 * hashtable itself.
		 */
		hashtable->skewEnabled = false;
		hashtable->skewBucket = NULL;
		hashtable->skewBucketNums = NULL;
		hashtable->nSkewBuckets = 0;
		hashtable->spaceUsedSkew = 0;
	}

	/*
	 * We can always skip over any batches that are completely empty on both
	 * sides.  We can sometimes skip over batches that are empty on only one
	 * side, but there are exceptions:
	 *
	 * 1. In a left/full outer join, we have to process outer batches even if
	 * the inner batch is empty.  Similarly, in a right/full outer join, we
	 * have to process inner batches even if the outer batch is empty.
	 *
	 * 2. If we have increased nbatch since the initial estimate, we have to
	 * scan inner batches since they might contain tuples that need to be
	 * reassigned to later inner batches.
	 *
	 * 3. Similarly, if we have increased nbatch since starting the outer
	 * scan, we have to rescan outer batches in case they contain tuples that
	 * need to be reassigned.
	 */
	curbatch++;
	while (curbatch < nbatch &&
		   (hashtable->outerBatchFile[curbatch] == NULL ||
			hashtable->innerBatchFile[curbatch] == NULL))
	{
		if (hashtable->outerBatchFile[curbatch] &&
			HJ_FILL_OUTER(hjstate))
			break;				/* must process due to rule 1 */
		if (hashtable->innerBatchFile[curbatch] &&
			HJ_FILL_INNER(hjstate))
			break;				/* must process due to rule 1 */
		if (hashtable->innerBatchFile[curbatch] &&
			nbatch != hashtable->nbatch_original)
			break;				/* must process due to rule 2 */
		if (hashtable->outerBatchFile[curbatch] &&
			nbatch != hashtable->nbatch_outstart)
			break;				/* must process due to rule 3 */
		/* We can ignore this batch. */
		/* Release associated temp files right away. */
		if (hashtable->innerBatchFile[curbatch])
			BufFileClose(hashtable->innerBatchFile[curbatch]);
		hashtable->innerBatchFile[curbatch] = NULL;
		if (hashtable->outerBatchFile[curbatch])
			BufFileClose(hashtable->outerBatchFile[curbatch]);
		hashtable->outerBatchFile[curbatch] = NULL;
		curbatch++;
	}

	if (curbatch >= nbatch)
		return false;			/* no more batches */

	hashtable->curbatch = curbatch;
	hashtable->curstripe = STRIPE_DETACHED;
	hjstate->hj_CurNumOuterTuples = 0;

	if (hashtable->innerBatchFile && hashtable->innerBatchFile[curbatch])
		innerFile = hashtable->innerBatchFile[curbatch];

	if (innerFile && BufFileSeek(innerFile, 0, 0L, SEEK_SET))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rewind hash-join temporary file: %m")));

	/* Need to rewind outer when this is the first stripe of a new batch */
	if (hashtable->outerBatchFile && hashtable->outerBatchFile[curbatch])
		outerFile = hashtable->outerBatchFile[curbatch];

	if (outerFile && BufFileSeek(outerFile, 0, 0L, SEEK_SET))
		ereport(ERROR,
				(errcode_for_file_access(),
					errmsg("could not rewind hash-join temporary file: %m")));

	ExecHashJoinLoadStripe(hjstate);
	return true;
}

static inline void
InstrIncrBatchStripes(List *fallback_batches_stats, int curbatch)
{
	ListCell   *lc;

	foreach(lc, fallback_batches_stats)
	{
		FallbackBatchStats *fallback_batch_stats = lfirst(lc);

		if (fallback_batch_stats->batchno == curbatch)
		{
			fallback_batch_stats->numstripes++;
			break;
		}
	}
}

/*
 * Returns false when the inner batch file is exhausted
 */
static int
ExecHashJoinLoadStripe(HashJoinState *hjstate)
{
	HashJoinTable hashtable = hjstate->hj_HashTable;
	int			curbatch = hashtable->curbatch;
	TupleTableSlot *slot;
	uint32		hashvalue;
	bool		loaded_inner = false;

	if (hashtable->curstripe == PHANTOM_STRIPE)
		return false;

	/*
	 * Rewind outer batch file (if present), so that we can start reading it.
	 * TODO: This is only necessary if this is not the first stripe of the
	 * batch
	 */
	if (hashtable->outerBatchFile && hashtable->outerBatchFile[curbatch])
	{
		if (BufFileSeek(hashtable->outerBatchFile[curbatch], 0, 0L, SEEK_SET))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not rewind hash-join temporary file: %m")));
	}
	if (hashtable->innerBatchFile && hashtable->innerBatchFile[curbatch] && hashtable->curbatch == 0 && hashtable->curstripe == 0)
	{
		if (BufFileSeek(hashtable->innerBatchFile[curbatch], 0, 0L, SEEK_SET))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not rewind hash-join temporary file: %m")));
	}

	hashtable->curstripe++;

	if (!hashtable->innerBatchFile || !hashtable->innerBatchFile[curbatch])
		return false;

	/*
	 * Reload the hash table with the new inner stripe
	 */
	ExecHashTableReset(hashtable);

	while ((slot = ExecHashJoinGetSavedTuple(hjstate,
											 hashtable->innerBatchFile[curbatch],
											 &hashvalue,
											 hjstate->hj_HashTupleSlot)))
	{
		/*
		 * NOTE: some tuples may be sent to future batches.  Also, it is
		 * possible for hashtable->nbatch to be increased here!
		 */
		uint32		hashTupleSize;

		/*
		 * TODO: wouldn't it be cool if this returned the size of the tuple
		 * inserted
		 */
		ExecHashTableInsert(hashtable, slot, hashvalue);
		loaded_inner = true;

		if (!IsHashloopFallback(hashtable))
			continue;

		hashTupleSize = slot->tts_ops->get_minimal_tuple(slot)->t_len + HJTUPLE_OVERHEAD;

		if (hashtable->spaceUsed + hashTupleSize +
			hashtable->nbuckets_optimal * sizeof(HashJoinTuple)
			> hashtable->spaceAllowed)
			break;
	}

	/*
	 * if we didn't load anything and it is a FOJ/LOJ fallback batch, we will
	 * transition to emit unmatched outer tuples next. we want to know how
	 * many tuples were in the batch in that case, so don't zero it out then
	 */

	/*
	 * if we loaded anything into the hashtable or it is the phantom stripe,
	 * must proceed to probing
	 */
	if (loaded_inner)
	{
		hjstate->hj_CurNumOuterTuples = 0;
		InstrIncrBatchStripes(hashtable->fallback_batches_stats, curbatch);
		return true;
	}

	if (IsHashloopFallback(hashtable) && HJ_FILL_OUTER(hjstate))
	{
		/*
		 * if we didn't load anything and it is a fallback batch, we will
		 * prepare to emit outer tuples during the phantom stripe probing
		 */
		hashtable->curstripe = PHANTOM_STRIPE;
		hjstate->hj_EmitOuterTupleId = 0;
		hjstate->hj_CurOuterMatchStatus = 0;
		BufFileSeek(hashtable->hashloopBatchFile[curbatch], 0, 0, SEEK_SET);
		if (hashtable->outerBatchFile[curbatch])
		BufFileSeek(hashtable->outerBatchFile[curbatch], 0, 0L, SEEK_SET);
		return true;
	}
	return false;
}


/*
 * Choose a batch to work on, and attach to it.  Returns true if successful,
 * false if there are no more batches.
 */
static bool
ExecParallelHashJoinNewBatch(HashJoinState *hjstate)
{
	HashJoinTable hashtable = hjstate->hj_HashTable;
	int			start_batchno;
	int			batchno;

	/*
	 * If we started up so late that the batch tracking array has been freed
	 * already by ExecHashTableDetach(), then we are finished.  See also
	 * ExecParallelHashEnsureBatchAccessors().
	 */
	if (hashtable->batches == NULL)
		return false;

	/*
	 * If we were already attached to a batch, remember not to bother checking
	 * it again, and detach from it (possibly freeing the hash table if we are
	 * last to detach). curbatch is set when the batch_barrier phase is either
	 * PHJ_BATCH_LOADING or PHJ_BATCH_STRIPING (note that the
	 * PHJ_BATCH_LOADING case will fall through to the PHJ_BATCH_STRIPING
	 * case). The PHJ_BATCH_STRIPING case returns to the caller. So when this
	 * function is reentered with a curbatch >= 0 then we must be done
	 * probing.
	 */

	if (hashtable->curbatch >= 0)
	{
		ParallelHashJoinBatchAccessor *batch_accessor = &hashtable->batches[hashtable->curbatch];

		if (IsHashloopFallback(hashtable))
			sb_end_write(hashtable->batches[hashtable->curbatch].sba);
		batch_accessor->done = PHJ_BATCH_ACCESSOR_DONE;
		ExecHashTableDetachBatch(hashtable);
	}

	/*
	 * Search for a batch that isn't done.  We use an atomic counter to start
	 * our search at a different batch in every participant when there are
	 * more batches than participants.
	 */
	batchno = start_batchno =
		pg_atomic_fetch_add_u32(&hashtable->parallel_state->distributor, 1) %
		hashtable->nbatch;
	do
	{
		if (hashtable->batches[batchno].done != PHJ_BATCH_ACCESSOR_DONE)
		{
			Barrier    *batch_barrier =
			&hashtable->batches[batchno].shared->batch_barrier;

			switch (BarrierAttach(batch_barrier))
			{
				case PHJ_BATCH_ELECTING:

					/* One backend allocates the hash table. */
					if (BarrierArriveAndWait(batch_barrier,
											 WAIT_EVENT_HASH_BATCH_ELECT))
					{
						ExecParallelHashTableAlloc(hashtable, batchno);

						/*
						 * one worker needs to 0 out the read_pages of all the
						 * participants in the new batch
						 */
						sts_reinitialize(hashtable->batches[batchno].inner_tuples);
					}
					/* Fall through. */

				case PHJ_BATCH_ALLOCATING:
					/* Wait for allocation to complete. */
					BarrierArriveAndWait(batch_barrier,
										 WAIT_EVENT_HASH_BATCH_ALLOCATE);
					/* Fall through. */

				case PHJ_BATCH_STRIPING:

					ExecParallelHashTableSetCurrentBatch(hashtable, batchno);
					sts_begin_parallel_scan(hashtable->batches[batchno].inner_tuples);
					if (hashtable->batches[batchno].shared->hashloop_fallback)
						sb_initialize_accessor(hashtable->batches[hashtable->curbatch].sba,
											   sts_get_tuplenum(hashtable->batches[hashtable->curbatch].outer_tuples));
					hashtable->curstripe = STRIPE_DETACHED;
					if (ExecParallelHashJoinLoadStripe(hjstate))
						return true;

					/*
					 * ExecParallelHashJoinLoadStripe() will return false from
					 * here when no more work can be done by this worker on
					 * this batch. Until further optimized, this worker will
					 * have detached from the stripe_barrier and should close
					 * its outer match statuses bitmap and then detach from
					 * the batch. In order to reuse the code below, fall
					 * through, even though the phase will not have been
					 * advanced
					 */
					if (hashtable->batches[batchno].shared->hashloop_fallback)
						sb_end_write(hashtable->batches[batchno].sba);

					/* Fall through. */

				case PHJ_BATCH_DONE:

					/*
					 * Already done.  Detach and go around again (if any
					 * remain).
					 */
					BarrierDetach(batch_barrier);
					hashtable->batches[batchno].done = PHJ_BATCH_ACCESSOR_DONE;
					hashtable->curbatch = -1;
					break;

				default:
					elog(ERROR, "unexpected batch phase %d",
						 BarrierPhase(batch_barrier));
			}
		}
		batchno = (batchno + 1) % hashtable->nbatch;
	} while (batchno != start_batchno);

	return false;
}



/*
 * Returns true if ready to probe and false if the inner is exhausted
 * (there are no more stripes)
 */
bool
ExecParallelHashJoinLoadStripe(HashJoinState *hjstate)
{
	HashJoinTable hashtable = hjstate->hj_HashTable;
	int			batchno = hashtable->curbatch;
	ParallelHashJoinBatch *batch = hashtable->batches[batchno].shared;
	Barrier    *stripe_barrier = &batch->stripe_barrier;
	SharedTuplestoreAccessor *outer_tuples;
	SharedTuplestoreAccessor *inner_tuples;
	ParallelHashJoinBatchAccessor *accessor;
	dsa_pointer_atomic *buckets;

	outer_tuples = hashtable->batches[batchno].outer_tuples;
	inner_tuples = hashtable->batches[batchno].inner_tuples;

	if (hashtable->curstripe >= 0)
	{
		/*
		 * If a worker is already attached to a stripe, wait until all
		 * participants have finished probing and detach. The last worker,
		 * however, can re-attach to the stripe_barrier and proceed to load
		 * and probe the other stripes
		 */
		/*
		 * After finishing with participating in a stripe, if a worker is the
		 * only one working on a batch, it will continue working on it.
		 * However, if a worker is not the only worker working on a batch, it
		 * would risk deadlock if it waits on the barrier. Instead, it will
		 * detach from the stripe, and, eventually the batch.
		 *
		 * This means all stripes after the first stripe will be executed
		 * serially. TODO: allow workers to provisionally detach from the
		 * batch and reattach later if there is still work to be done. I had a
		 * patch that did this. Workers who were not the last worker saved the
		 * state of the stripe barrier upon detaching and then mark the batch
		 * as "provisionally" done (not done). Later, when the worker comes
		 * back to the batch in the batch phase machine, if the batch is not
		 * complete and the phase has advanced since the worker was last
		 * participating, then the worker can join back in. This had problems.
		 * There were synchronization issues with workers having multiple
		 * outer match status bitmap files open at the same time, so, I had
		 * workers close their bitmap and make a new one the next time they
		 * joined in. This didn't work with the current code because the
		 * original outer match status bitmap file that the worker had created
		 * while probing stripe 1 did not get combined into the combined
		 * bitmap This could be specifically fixed, but I think it is better
		 * to address the lack of parallel execution for stripes after stripe
		 * 0 more holistically.
		 */
		if (!BarrierArriveAndDetach(stripe_barrier))
		{
			sb_end_write(hashtable->batches[hashtable->curbatch].sba);
			hashtable->curstripe = STRIPE_DETACHED;
			return false;
		}

		/*
		 * This isn't a race condition if no other workers can stay attached
		 * to this barrier in the intervening time. Basically, if you attach
		 * to a stripe barrier in the PHJ_STRIPE_DONE phase, detach
		 * immediately and move on.
		 */
		BarrierAttach(stripe_barrier);
	}
	else if (hashtable->curstripe == STRIPE_DETACHED)
	{
		int			phase = BarrierAttach(stripe_barrier);

		/*
		 * If a worker enters this phase machine on a stripe number greater
		 * than the batch's maximum stripe number, then: 1) The batch is done,
		 * or 2) The batch is on the phantom stripe that's used for hashloop
		 * fallback Either way the worker can't contribute so just detach and
		 * move on.
		 */

		if (PHJ_STRIPE_NUMBER(phase) > batch->maximum_stripe_number ||
			PHJ_STRIPE_PHASE(phase) == PHJ_STRIPE_DONE)
			return ExecHashTableDetachStripe(hashtable);
	}
	else if (hashtable->curstripe == PHANTOM_STRIPE)
	{
		sts_end_parallel_scan(outer_tuples);

		/*
		 * TODO: ideally this would go somewhere in the batch phase machine
		 * Putting it in ExecHashTableDetachBatch didn't do the trick
		 */
		sb_end_read(hashtable->batches[batchno].sba);
		return ExecHashTableDetachStripe(hashtable);
	}

	hashtable->curstripe = PHJ_STRIPE_NUMBER(BarrierPhase(stripe_barrier));

	/*
	 * The outer side is exhausted and either 1) the current stripe of the
	 * inner side is exhausted and it is time to advance the stripe 2) the
	 * last stripe of the inner side is exhausted and it is time to advance
	 * the batch
	 */
	for (;;)
	{
		int			phase = BarrierPhase(stripe_barrier);

		switch (PHJ_STRIPE_PHASE(phase))
		{
			case PHJ_STRIPE_ELECTING:
				if (BarrierArriveAndWait(stripe_barrier, WAIT_EVENT_HASH_STRIPE_ELECT))
				{
					sts_reinitialize(outer_tuples);

					/*
					 * set the rewound flag back to false to prepare for the
					 * next stripe
					 */
					sts_reset_rewound(inner_tuples);
				}

				/* FALLTHROUGH */

			case PHJ_STRIPE_RESETTING:
				/* TODO: not needed for phantom stripe */
				BarrierArriveAndWait(stripe_barrier, WAIT_EVENT_HASH_STRIPE_RESET);
				/* FALLTHROUGH */

			case PHJ_STRIPE_LOADING:
				{
					MinimalTuple tuple;
					tupleMetadata metadata;

					/*
					 * Start (or join in) loading the next stripe of inner
					 * tuples.
					 */

					/*
					 * I'm afraid there potential issue if a worker joins in
					 * this phase and doesn't do the actions and resetting of
					 * variables in sts_resume_parallel_scan. that is, if it
					 * doesn't reset start_page and read_next_page in between
					 * stripes. For now, call it. However, I think it might be
					 * able to be removed.
					 */

					/*
					 * TODO: sts_resume_parallel_scan() is overkill for stripe
					 * 0 of each batch
					 */
					sts_resume_parallel_scan(inner_tuples);

					while ((tuple = sts_parallel_scan_next(inner_tuples, &metadata)))
					{
						/* The tuple is from a previous stripe. Skip it */
						if (metadata.stripe < PHJ_STRIPE_NUMBER(phase))
							continue;

						/*
						 * tuple from future. time to back out read_page. end
						 * of stripe
						 */
						if (metadata.stripe > PHJ_STRIPE_NUMBER(phase))
						{
							sts_parallel_scan_rewind(inner_tuples);
							continue;
						}

						ExecForceStoreMinimalTuple(tuple, hjstate->hj_HashTupleSlot, false);
						ExecParallelHashTableInsertCurrentBatch(
																hashtable,
																hjstate->hj_HashTupleSlot,
																metadata.hashvalue);
					}
					BarrierArriveAndWait(stripe_barrier, WAIT_EVENT_HASH_STRIPE_LOAD);
				}
				/* FALLTHROUGH */

			case PHJ_STRIPE_PROBING:

				/*
				 * do this again here in case a worker began the scan and then
				 * entered after loading before probing
				 */
				sts_end_parallel_scan(inner_tuples);
				sts_begin_parallel_scan(outer_tuples);
				return true;

			case PHJ_STRIPE_DONE:

				if (PHJ_STRIPE_NUMBER(phase) >= batch->maximum_stripe_number)
				{
					/*
					 * Handle the phantom stripe case.
					 */
					if (batch->hashloop_fallback && HJ_FILL_OUTER(hjstate))
						goto fallback_stripe;

					/* Return if this is the last stripe */
					return ExecHashTableDetachStripe(hashtable);
				}

				/* this, effectively, increments the stripe number */
				if (BarrierArriveAndWait(stripe_barrier, WAIT_EVENT_HASH_STRIPE_LOAD))
				{
					/*
					 * reset inner's hashtable and recycle the existing bucket
					 * array.
					 */
					buckets = (dsa_pointer_atomic *)
						dsa_get_address(hashtable->area, batch->buckets);

					for (size_t i = 0; i < hashtable->nbuckets; ++i)
						dsa_pointer_atomic_write(&buckets[i], InvalidDsaPointer);
				}

				hashtable->curstripe++;
				continue;

			default:
				elog(ERROR, "unexpected stripe phase %d. pid %i. batch %i.", BarrierPhase(stripe_barrier), MyProcPid, batchno);
		}
	}

fallback_stripe:
	accessor = &hashtable->batches[hashtable->curbatch];
	sb_end_write(accessor->sba);

	/* Ensure that only a single worker is attached to the barrier */
	if (!BarrierArriveAndWait(stripe_barrier, WAIT_EVENT_HASH_STRIPE_LOAD))
		return ExecHashTableDetachStripe(hashtable);


	/* No one except the last worker will run this code */
	hashtable->curstripe = PHANTOM_STRIPE;

	/*
	 * reset inner's hashtable and recycle the existing bucket array.
	 */
	buckets = (dsa_pointer_atomic *)
		dsa_get_address(hashtable->area, batch->buckets);

	for (size_t i = 0; i < hashtable->nbuckets; ++i)
		dsa_pointer_atomic_write(&buckets[i], InvalidDsaPointer);

	/*
	 * If all workers (including this one) have finished probing the batch,
	 * one worker is elected to Loop through the outer match status files from
	 * all workers that were attached to this batch Combine them into one
	 * bitmap Use the bitmap, loop through the outer batch file again, and
	 * emit unmatched tuples All workers will detach from the batch barrier
	 * and the last worker will clean up the hashtable. All workers except the
	 * last worker will end their scans of the outer and inner side. The last
	 * worker will end its scan of the inner side
	 */

	sb_combine(accessor->sba);
	sts_reinitialize(outer_tuples);

	sts_begin_parallel_scan(outer_tuples);

	return true;
}

/*
 * ExecHashJoinSaveTuple
 *		save a tuple to a batch file.
 *
 * The data recorded in the file for each tuple is its hash value,
 * then the tuple in MinimalTuple format.
 *
 * Note: it is important always to call this in the regular executor
 * context, not in a shorter-lived context; else the temp file buffers
 * will get messed up.
 */
void
ExecHashJoinSaveTuple(MinimalTuple tuple, uint32 hashvalue,
					  BufFile **fileptr)
{
	BufFile    *file = *fileptr;

	if (file == NULL)
	{
		/* First write to this batch file, so open it. */
		file = BufFileCreateTemp(false);
		*fileptr = file;
	}

	BufFileWrite(file, (void *) &hashvalue, sizeof(uint32));
	BufFileWrite(file, (void *) tuple, tuple->t_len);
}

/*
 * ExecHashJoinGetSavedTuple
 *		read the next tuple from a batch file.  Return NULL if no more.
 *
 * On success, *hashvalue is set to the tuple's hash value, and the tuple
 * itself is stored in the given slot.
 */
static TupleTableSlot *
ExecHashJoinGetSavedTuple(HashJoinState *hjstate,
						  BufFile *file,
						  uint32 *hashvalue,
						  TupleTableSlot *tupleSlot)
{
	uint32		header[2];
	size_t		nread;
	MinimalTuple tuple;

	/*
	 * We check for interrupts here because this is typically taken as an
	 * alternative code path to an ExecProcNode() call, which would include
	 * such a check.
	 */
	CHECK_FOR_INTERRUPTS();

	/*
	 * Since both the hash value and the MinimalTuple length word are uint32,
	 * we can read them both in one BufFileRead() call without any type
	 * cheating.
	 */
	nread = BufFileRead(file, (void *) header, sizeof(header));
	if (nread == 0)				/* end of file */
	{
		ExecClearTuple(tupleSlot);
		return NULL;
	}
	if (nread != sizeof(header))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read from hash-join temporary file: read only %zu of %zu bytes",
						nread, sizeof(header))));
	*hashvalue = header[0];
	tuple = (MinimalTuple) palloc(header[1]);
	tuple->t_len = header[1];
	nread = BufFileRead(file,
						(void *) ((char *) tuple + sizeof(uint32)),
						header[1] - sizeof(uint32));
	if (nread != header[1] - sizeof(uint32))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read from hash-join temporary file: read only %zu of %zu bytes",
						nread, header[1] - sizeof(uint32))));
	ExecForceStoreMinimalTuple(tuple, tupleSlot, true);
	return tupleSlot;
}


void
ExecReScanHashJoin(HashJoinState *node)
{
	/*
	 * In a multi-batch join, we currently have to do rescans the hard way,
	 * primarily because batch temp files may have already been released. But
	 * if it's a single-batch join, and there is no parameter change for the
	 * inner subnode, then we can just re-use the existing hash table without
	 * rebuilding it.
	 */
	if (node->hj_HashTable != NULL)
	{
		if (node->hj_HashTable->nbatch == 1 &&
			node->js.ps.righttree->chgParam == NULL)
		{
			/*
			 * Okay to reuse the hash table; needn't rescan inner, either.
			 *
			 * However, if it's a right/full join, we'd better reset the
			 * inner-tuple match flags contained in the table.
			 */
			if (HJ_FILL_INNER(node))
				ExecHashTableResetMatchFlags(node->hj_HashTable);

			/*
			 * Also, we need to reset our state about the emptiness of the
			 * outer relation, so that the new scan of the outer will update
			 * it correctly if it turns out to be empty this time. (There's no
			 * harm in clearing it now because ExecHashJoin won't need the
			 * info.  In the other cases, where the hash table doesn't exist
			 * or we are destroying it, we leave this state alone because
			 * ExecHashJoin will need it the first time through.)
			 */
			node->hj_OuterNotEmpty = false;

			/* ExecHashJoin can skip the BUILD_HASHTABLE step */
			node->hj_JoinState = HJ_NEED_NEW_OUTER;
		}
		else
		{
			/* must destroy and rebuild hash table */
			HashState  *hashNode = castNode(HashState, innerPlanState(node));

			Assert(hashNode->hashtable == node->hj_HashTable);
			/* accumulate stats from old hash table, if wanted */
			/* (this should match ExecShutdownHash) */
			if (hashNode->ps.instrument && !hashNode->hinstrument)
				hashNode->hinstrument = (HashInstrumentation *)
					palloc0(sizeof(HashInstrumentation));
			if (hashNode->hinstrument)
				ExecHashAccumInstrumentation(hashNode->hinstrument,
											 hashNode->hashtable);
			/* for safety, be sure to clear child plan node's pointer too */
			hashNode->hashtable = NULL;

			ExecHashTableDestroy(node->hj_HashTable);
			node->hj_HashTable = NULL;
			node->hj_JoinState = HJ_BUILD_HASHTABLE;

			/*
			 * if chgParam of subnode is not null then plan will be re-scanned
			 * by first ExecProcNode.
			 */
			if (node->js.ps.righttree->chgParam == NULL)
				ExecReScan(node->js.ps.righttree);
		}
	}

	/* Always reset intra-tuple state */
	node->hj_CurHashValue = 0;
	node->hj_CurBucketNo = 0;
	node->hj_CurSkewBucketNo = INVALID_SKEW_BUCKET_NO;
	node->hj_CurTuple = NULL;

	node->hj_MatchedOuter = false;
	node->hj_FirstOuterTupleSlot = NULL;

	node->hj_CurNumOuterTuples = 0;
	node->hj_CurOuterMatchStatus = 0;

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (node->js.ps.lefttree->chgParam == NULL)
		ExecReScan(node->js.ps.lefttree);
}

void
ExecShutdownHashJoin(HashJoinState *node)
{
	if (node->hj_HashTable)
	{
		/*
		 * Detach from shared state before DSM memory goes away.  This makes
		 * sure that we don't have any pointers into DSM memory by the time
		 * ExecEndHashJoin runs.
		 */
		ExecHashTableDetachBatch(node->hj_HashTable);
		ExecHashTableDetach(node->hj_HashTable);
	}
}

static void
ExecParallelHashJoinPartitionOuter(HashJoinState *hjstate)
{
	PlanState  *outerState = outerPlanState(hjstate);
	ExprContext *econtext = hjstate->js.ps.ps_ExprContext;
	HashJoinTable hashtable = hjstate->hj_HashTable;
	TupleTableSlot *slot;
	int			i;

	Assert(hjstate->hj_FirstOuterTupleSlot == NULL);

	/* Execute outer plan, writing all tuples to shared tuplestores. */
	for (;;)
	{
		tupleMetadata metadata;

		slot = ExecProcNode(outerState);
		if (TupIsNull(slot))
			break;
		econtext->ecxt_outertuple = slot;
		if (ExecHashGetHashValue(hashtable, econtext,
								 hjstate->hj_OuterHashKeys,
								 true,	/* outer tuple */
								 HJ_FILL_OUTER(hjstate),
								 &metadata.hashvalue))
		{
			int			batchno;
			int			bucketno;
			bool		shouldFree;
			SharedTuplestoreAccessor *accessor;

			MinimalTuple mintup = ExecFetchSlotMinimalTuple(slot, &shouldFree);

			ExecHashGetBucketAndBatch(hashtable, metadata.hashvalue, &bucketno,
									  &batchno);
			accessor = hashtable->batches[batchno].outer_tuples;

			/* cannot count on deterministic order of tupleids */
			metadata.tupleid = sts_increment_ntuples(accessor);

			sts_puttuple(hashtable->batches[batchno].outer_tuples, &metadata.hashvalue, mintup);

			if (shouldFree)
				heap_free_minimal_tuple(mintup);
		}
		CHECK_FOR_INTERRUPTS();
	}

	/* Make sure all outer partitions are readable by any backend. */
	for (i = 0; i < hashtable->nbatch; ++i)
		sts_end_write(hashtable->batches[i].outer_tuples);
}

void
ExecHashJoinEstimate(HashJoinState *state, ParallelContext *pcxt)
{
	shm_toc_estimate_chunk(&pcxt->estimator, sizeof(ParallelHashJoinState));
	shm_toc_estimate_keys(&pcxt->estimator, 1);
}

void
ExecHashJoinInitializeDSM(HashJoinState *state, ParallelContext *pcxt)
{
	int			plan_node_id = state->js.ps.plan->plan_node_id;
	HashState  *hashNode;
	ParallelHashJoinState *pstate;

	/*
	 * Disable shared hash table mode if we failed to create a real DSM
	 * segment, because that means that we don't have a DSA area to work with.
	 */
	if (pcxt->seg == NULL)
		return;

	ExecSetExecProcNode(&state->js.ps, ExecParallelHashJoin);

	/*
	 * Set up the state needed to coordinate access to the shared hash
	 * table(s), using the plan node ID as the toc key.
	 */
	pstate = shm_toc_allocate(pcxt->toc, sizeof(ParallelHashJoinState));
	shm_toc_insert(pcxt->toc, plan_node_id, pstate);

	/*
	 * Set up the shared hash join state with no batches initially.
	 * ExecHashTableCreate() will prepare at least one later and set nbatch
	 * and space_allowed.
	 */
	pstate->nbatch = 0;
	pstate->space_allowed = 0;
	pstate->batches = InvalidDsaPointer;
	pstate->old_batches = InvalidDsaPointer;
	pstate->nbuckets = 0;
	pstate->growth = PHJ_GROWTH_OK;
	pstate->chunk_work_queue = InvalidDsaPointer;
	pg_atomic_init_u32(&pstate->distributor, 0);
	pstate->nparticipants = pcxt->nworkers + 1;
	pstate->total_tuples = 0;
	LWLockInitialize(&pstate->lock,
					 LWTRANCHE_PARALLEL_HASH_JOIN);
	BarrierInit(&pstate->build_barrier, 0);
	BarrierInit(&pstate->grow_batches_barrier, 0);
	BarrierInit(&pstate->grow_buckets_barrier, 0);

	/* Set up the space we'll use for shared temporary files. */
	SharedFileSetInit(&pstate->fileset, pcxt->seg);

	/* Initialize the shared state in the hash node. */
	hashNode = (HashState *) innerPlanState(state);
	hashNode->parallel_state = pstate;
}

/* ----------------------------------------------------------------
 *		ExecHashJoinReInitializeDSM
 *
 *		Reset shared state before beginning a fresh scan.
 * ----------------------------------------------------------------
 */
void
ExecHashJoinReInitializeDSM(HashJoinState *state, ParallelContext *cxt)
{
	int			plan_node_id = state->js.ps.plan->plan_node_id;
	ParallelHashJoinState *pstate =
	shm_toc_lookup(cxt->toc, plan_node_id, false);

	/*
	 * It would be possible to reuse the shared hash table in single-batch
	 * cases by resetting and then fast-forwarding build_barrier to
	 * PHJ_BUILD_DONE and batch 0's batch_barrier to PHJ_BATCH_PROBING, but
	 * currently shared hash tables are already freed by now (by the last
	 * participant to detach from the batch).  We could consider keeping it
	 * around for single-batch joins.  We'd also need to adjust
	 * finalize_plan() so that it doesn't record a dummy dependency for
	 * Parallel Hash nodes, preventing the rescan optimization.  For now we
	 * don't try.
	 */

	/* Detach, freeing any remaining shared memory. */
	if (state->hj_HashTable != NULL)
	{
		ExecHashTableDetachBatch(state->hj_HashTable);
		ExecHashTableDetach(state->hj_HashTable);
	}

	/* Clear any shared batch files. */
	SharedFileSetDeleteAll(&pstate->fileset);

	/* Reset build_barrier to PHJ_BUILD_ELECTING so we can go around again. */
	BarrierInit(&pstate->build_barrier, 0);
}

void
ExecHashJoinInitializeWorker(HashJoinState *state,
							 ParallelWorkerContext *pwcxt)
{
	HashState  *hashNode;
	int			plan_node_id = state->js.ps.plan->plan_node_id;
	ParallelHashJoinState *pstate =
	shm_toc_lookup(pwcxt->toc, plan_node_id, false);

	/* Attach to the space for shared temporary files. */
	SharedFileSetAttach(&pstate->fileset, pwcxt->seg);

	/* Attach to the shared state in the hash node. */
	hashNode = (HashState *) innerPlanState(state);
	hashNode->parallel_state = pstate;

	ExecSetExecProcNode(&state->js.ps, ExecParallelHashJoin);
}
