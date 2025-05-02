/*-------------------------------------------------------------------------
 *
 * gininsert.c
 *	  insert routines for the postgres inverted index access method.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/gininsert.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin_private.h"
#include "access/gin_tuple.h"
#include "access/parallel.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xloginsert.h"
#include "catalog/index.h"
#include "catalog/pg_collation.h"
#include "commands/progress.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
#include "tcop/tcopprot.h"
#include "utils/datum.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/builtins.h"


/* Magic numbers for parallel state sharing */
#define PARALLEL_KEY_GIN_SHARED			UINT64CONST(0xB000000000000001)
#define PARALLEL_KEY_TUPLESORT			UINT64CONST(0xB000000000000002)
#define PARALLEL_KEY_QUERY_TEXT			UINT64CONST(0xB000000000000003)
#define PARALLEL_KEY_WAL_USAGE			UINT64CONST(0xB000000000000004)
#define PARALLEL_KEY_BUFFER_USAGE		UINT64CONST(0xB000000000000005)

/*
 * Status for index builds performed in parallel.  This is allocated in a
 * dynamic shared memory segment.
 */
typedef struct GinBuildShared
{
	/*
	 * These fields are not modified during the build.  They primarily exist
	 * for the benefit of worker processes that need to create state
	 * corresponding to that used by the leader.
	 */
	Oid			heaprelid;
	Oid			indexrelid;
	bool		isconcurrent;
	int			scantuplesortstates;

	/*
	 * workersdonecv is used to monitor the progress of workers.  All parallel
	 * participants must indicate that they are done before leader can use
	 * results built by the workers (and before leader can write the data into
	 * the index).
	 */
	ConditionVariable workersdonecv;

	/*
	 * mutex protects all following fields
	 *
	 * These fields contain status information of interest to GIN index builds
	 * that must work just the same when an index is built in parallel.
	 */
	slock_t		mutex;

	/*
	 * Mutable state that is maintained by workers, and reported back to
	 * leader at end of the scans.
	 *
	 * nparticipantsdone is number of worker processes finished.
	 *
	 * reltuples is the total number of input heap tuples.
	 *
	 * indtuples is the total number of tuples that made it into the index.
	 */
	int			nparticipantsdone;
	double		reltuples;
	double		indtuples;

	/*
	 * ParallelTableScanDescData data follows. Can't directly embed here, as
	 * implementations of the parallel table scan desc interface might need
	 * stronger alignment.
	 */
} GinBuildShared;

/*
 * Return pointer to a GinBuildShared's parallel table scan.
 *
 * c.f. shm_toc_allocate as to why BUFFERALIGN is used, rather than just
 * MAXALIGN.
 */
#define ParallelTableScanFromGinBuildShared(shared) \
	(ParallelTableScanDesc) ((char *) (shared) + BUFFERALIGN(sizeof(GinBuildShared)))

/*
 * Status for leader in parallel index build.
 */
typedef struct GinLeader
{
	/* parallel context itself */
	ParallelContext *pcxt;

	/*
	 * nparticipanttuplesorts is the exact number of worker processes
	 * successfully launched, plus one leader process if it participates as a
	 * worker (only DISABLE_LEADER_PARTICIPATION builds avoid leader
	 * participating as a worker).
	 */
	int			nparticipanttuplesorts;

	/*
	 * Leader process convenience pointers to shared state (leader avoids TOC
	 * lookups).
	 *
	 * GinBuildShared is the shared state for entire build.  sharedsort is the
	 * shared, tuplesort-managed state passed to each process tuplesort.
	 * snapshot is the snapshot used by the scan iff an MVCC snapshot is
	 * required.
	 */
	GinBuildShared *ginshared;
	Sharedsort *sharedsort;
	Snapshot	snapshot;
	WalUsage   *walusage;
	BufferUsage *bufferusage;
} GinLeader;

typedef struct
{
	GinState	ginstate;
	double		indtuples;
	GinStatsData buildStats;
	MemoryContext tmpCtx;
	MemoryContext funcCtx;
	BuildAccumulator accum;
	ItemPointerData tid;
	int			work_mem;

	/*
	 * bs_leader is only present when a parallel index build is performed, and
	 * only in the leader process.
	 */
	GinLeader  *bs_leader;
	int			bs_worker_id;

	/* used to pass information from workers to leader */
	double		bs_numtuples;
	double		bs_reltuples;

	/*
	 * The sortstate is used by workers (including the leader). It has to be
	 * part of the build state, because that's the only thing passed to the
	 * build callback etc.
	 */
	Tuplesortstate *bs_sortstate;

	/*
	 * The sortstate used only within a single worker for the first merge pass
	 * happening there. In principle it doesn't need to be part of the build
	 * state and we could pass it around directly, but it's more convenient
	 * this way. And it's part of the build state, after all.
	 */
	Tuplesortstate *bs_worker_sort;
} GinBuildState;


/* parallel index builds */
static void _gin_begin_parallel(GinBuildState *buildstate, Relation heap, Relation index,
								bool isconcurrent, int request);
static void _gin_end_parallel(GinLeader *ginleader, GinBuildState *state);
static Size _gin_parallel_estimate_shared(Relation heap, Snapshot snapshot);
static double _gin_parallel_heapscan(GinBuildState *state);
static double _gin_parallel_merge(GinBuildState *state);
static void _gin_leader_participate_as_worker(GinBuildState *buildstate,
											  Relation heap, Relation index);
static void _gin_parallel_scan_and_build(GinBuildState *state,
										 GinBuildShared *ginshared,
										 Sharedsort *sharedsort,
										 Relation heap, Relation index,
										 int sortmem, bool progress);

static ItemPointer _gin_parse_tuple_items(GinTuple *a);
static Datum _gin_parse_tuple_key(GinTuple *a);

static GinTuple *_gin_build_tuple(OffsetNumber attrnum, unsigned char category,
								  Datum key, int16 typlen, bool typbyval,
								  ItemPointerData *items, uint32 nitems,
								  Size *len);

/*
 * Adds array of item pointers to tuple's posting list, or
 * creates posting tree and tuple pointing to tree in case
 * of not enough space.  Max size of tuple is defined in
 * GinFormTuple().  Returns a new, modified index tuple.
 * items[] must be in sorted order with no duplicates.
 */
static IndexTuple
addItemPointersToLeafTuple(GinState *ginstate,
						   IndexTuple old,
						   ItemPointerData *items, uint32 nitem,
						   GinStatsData *buildStats, Buffer buffer)
{
	OffsetNumber attnum;
	Datum		key;
	GinNullCategory category;
	IndexTuple	res;
	ItemPointerData *newItems,
			   *oldItems;
	int			oldNPosting,
				newNPosting;
	GinPostingList *compressedList;

	Assert(!GinIsPostingTree(old));

	attnum = gintuple_get_attrnum(ginstate, old);
	key = gintuple_get_key(ginstate, old, &category);

	/* merge the old and new posting lists */
	oldItems = ginReadTuple(ginstate, attnum, old, &oldNPosting);

	newItems = ginMergeItemPointers(items, nitem,
									oldItems, oldNPosting,
									&newNPosting);

	/* Compress the posting list, and try to a build tuple with room for it */
	res = NULL;
	compressedList = ginCompressPostingList(newItems, newNPosting, GinMaxItemSize,
											NULL);
	pfree(newItems);
	if (compressedList)
	{
		res = GinFormTuple(ginstate, attnum, key, category,
						   (char *) compressedList,
						   SizeOfGinPostingList(compressedList),
						   newNPosting,
						   false);
		pfree(compressedList);
	}
	if (!res)
	{
		/* posting list would be too big, convert to posting tree */
		BlockNumber postingRoot;

		/*
		 * Initialize posting tree with the old tuple's posting list.  It's
		 * surely small enough to fit on one posting-tree page, and should
		 * already be in order with no duplicates.
		 */
		postingRoot = createPostingTree(ginstate->index,
										oldItems,
										oldNPosting,
										buildStats,
										buffer);

		/* Now insert the TIDs-to-be-added into the posting tree */
		ginInsertItemPointers(ginstate->index, postingRoot,
							  items, nitem,
							  buildStats);

		/* And build a new posting-tree-only result tuple */
		res = GinFormTuple(ginstate, attnum, key, category, NULL, 0, 0, true);
		GinSetPostingTree(res, postingRoot);
	}
	pfree(oldItems);

	return res;
}

/*
 * Build a fresh leaf tuple, either posting-list or posting-tree format
 * depending on whether the given items list will fit.
 * items[] must be in sorted order with no duplicates.
 *
 * This is basically the same logic as in addItemPointersToLeafTuple,
 * but working from slightly different input.
 */
static IndexTuple
buildFreshLeafTuple(GinState *ginstate,
					OffsetNumber attnum, Datum key, GinNullCategory category,
					ItemPointerData *items, uint32 nitem,
					GinStatsData *buildStats, Buffer buffer)
{
	IndexTuple	res = NULL;
	GinPostingList *compressedList;

	/* try to build a posting list tuple with all the items */
	compressedList = ginCompressPostingList(items, nitem, GinMaxItemSize, NULL);
	if (compressedList)
	{
		res = GinFormTuple(ginstate, attnum, key, category,
						   (char *) compressedList,
						   SizeOfGinPostingList(compressedList),
						   nitem, false);
		pfree(compressedList);
	}
	if (!res)
	{
		/* posting list would be too big, build posting tree */
		BlockNumber postingRoot;

		/*
		 * Build posting-tree-only result tuple.  We do this first so as to
		 * fail quickly if the key is too big.
		 */
		res = GinFormTuple(ginstate, attnum, key, category, NULL, 0, 0, true);

		/*
		 * Initialize a new posting tree with the TIDs.
		 */
		postingRoot = createPostingTree(ginstate->index, items, nitem,
										buildStats, buffer);

		/* And save the root link in the result tuple */
		GinSetPostingTree(res, postingRoot);
	}

	return res;
}

/*
 * Insert one or more heap TIDs associated with the given key value.
 * This will either add a single key entry, or enlarge a pre-existing entry.
 *
 * During an index build, buildStats is non-null and the counters
 * it contains should be incremented as needed.
 */
void
ginEntryInsert(GinState *ginstate,
			   OffsetNumber attnum, Datum key, GinNullCategory category,
			   ItemPointerData *items, uint32 nitem,
			   GinStatsData *buildStats)
{
	GinBtreeData btree;
	GinBtreeEntryInsertData insertdata;
	GinBtreeStack *stack;
	IndexTuple	itup;
	Page		page;

	insertdata.isDelete = false;

	ginPrepareEntryScan(&btree, attnum, key, category, ginstate);
	btree.isBuild = (buildStats != NULL);

	stack = ginFindLeafPage(&btree, false, false);
	page = BufferGetPage(stack->buffer);

	if (btree.findItem(&btree, stack))
	{
		/* found pre-existing entry */
		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, stack->off));

		if (GinIsPostingTree(itup))
		{
			/* add entries to existing posting tree */
			BlockNumber rootPostingTree = GinGetPostingTree(itup);

			/* release all stack */
			LockBuffer(stack->buffer, GIN_UNLOCK);
			freeGinBtreeStack(stack);

			/* insert into posting tree */
			ginInsertItemPointers(ginstate->index, rootPostingTree,
								  items, nitem,
								  buildStats);
			return;
		}

		CheckForSerializableConflictIn(ginstate->index, NULL,
									   BufferGetBlockNumber(stack->buffer));
		/* modify an existing leaf entry */
		itup = addItemPointersToLeafTuple(ginstate, itup,
										  items, nitem, buildStats, stack->buffer);

		insertdata.isDelete = true;
	}
	else
	{
		CheckForSerializableConflictIn(ginstate->index, NULL,
									   BufferGetBlockNumber(stack->buffer));
		/* no match, so construct a new leaf entry */
		itup = buildFreshLeafTuple(ginstate, attnum, key, category,
								   items, nitem, buildStats, stack->buffer);

		/*
		 * nEntries counts leaf tuples, so increment it only when we make a
		 * new one.
		 */
		if (buildStats)
			buildStats->nEntries++;
	}

	/* Insert the new or modified leaf tuple */
	insertdata.entry = itup;
	ginInsertValue(&btree, stack, &insertdata, buildStats);
	pfree(itup);
}

/*
 * Extract index entries for a single indexable item, and add them to the
 * BuildAccumulator's state.
 *
 * This function is used only during initial index creation.
 */
static void
ginHeapTupleBulkInsert(GinBuildState *buildstate, OffsetNumber attnum,
					   Datum value, bool isNull,
					   ItemPointer heapptr)
{
	Datum	   *entries;
	GinNullCategory *categories;
	int32		nentries;
	MemoryContext oldCtx;

	oldCtx = MemoryContextSwitchTo(buildstate->funcCtx);
	entries = ginExtractEntries(buildstate->accum.ginstate, attnum,
								value, isNull,
								&nentries, &categories);
	MemoryContextSwitchTo(oldCtx);

	ginInsertBAEntries(&buildstate->accum, heapptr, attnum,
					   entries, categories, nentries);

	buildstate->indtuples += nentries;

	MemoryContextReset(buildstate->funcCtx);
}

static void
ginBuildCallback(Relation index, ItemPointer tid, Datum *values,
				 bool *isnull, bool tupleIsAlive, void *state)
{
	GinBuildState *buildstate = (GinBuildState *) state;
	MemoryContext oldCtx;
	int			i;

	oldCtx = MemoryContextSwitchTo(buildstate->tmpCtx);

	for (i = 0; i < buildstate->ginstate.origTupdesc->natts; i++)
		ginHeapTupleBulkInsert(buildstate, (OffsetNumber) (i + 1),
							   values[i], isnull[i], tid);

	/* If we've maxed out our available memory, dump everything to the index */
	if (buildstate->accum.allocatedMemory >= maintenance_work_mem * (Size) 1024)
	{
		ItemPointerData *list;
		Datum		key;
		GinNullCategory category;
		uint32		nlist;
		OffsetNumber attnum;

		ginBeginBAScan(&buildstate->accum);
		while ((list = ginGetBAEntry(&buildstate->accum,
									 &attnum, &key, &category, &nlist)) != NULL)
		{
			/* there could be many entries, so be willing to abort here */
			CHECK_FOR_INTERRUPTS();
			ginEntryInsert(&buildstate->ginstate, attnum, key, category,
						   list, nlist, &buildstate->buildStats);
		}

		MemoryContextReset(buildstate->tmpCtx);
		ginInitBA(&buildstate->accum);
	}

	MemoryContextSwitchTo(oldCtx);
}

/*
 * ginFlushBuildState
 *		Write all data from BuildAccumulator into the tuplesort.
 */
static void
ginFlushBuildState(GinBuildState *buildstate, Relation index)
{
	ItemPointerData *list;
	Datum		key;
	GinNullCategory category;
	uint32		nlist;
	OffsetNumber attnum;
	TupleDesc	tdesc = RelationGetDescr(index);

	ginBeginBAScan(&buildstate->accum);
	while ((list = ginGetBAEntry(&buildstate->accum,
								 &attnum, &key, &category, &nlist)) != NULL)
	{
		/* information about the key */
		Form_pg_attribute attr = TupleDescAttr(tdesc, (attnum - 1));

		/* GIN tuple and tuple length */
		GinTuple   *tup;
		Size		tuplen;

		/* there could be many entries, so be willing to abort here */
		CHECK_FOR_INTERRUPTS();

		tup = _gin_build_tuple(attnum, category,
							   key, attr->attlen, attr->attbyval,
							   list, nlist, &tuplen);

		tuplesort_putgintuple(buildstate->bs_worker_sort, tup, tuplen);

		pfree(tup);
	}

	MemoryContextReset(buildstate->tmpCtx);
	ginInitBA(&buildstate->accum);
}

/*
 * ginBuildCallbackParallel
 *		Callback for the parallel index build.
 *
 * This is similar to the serial build callback ginBuildCallback, but
 * instead of writing the accumulated entries into the index, each worker
 * writes them into a (local) tuplesort.
 *
 * The worker then sorts and combines these entries, before writing them
 * into a shared tuplesort for the leader (see _gin_parallel_scan_and_build
 * for the whole process).
 */
static void
ginBuildCallbackParallel(Relation index, ItemPointer tid, Datum *values,
						 bool *isnull, bool tupleIsAlive, void *state)
{
	GinBuildState *buildstate = (GinBuildState *) state;
	MemoryContext oldCtx;
	int			i;

	oldCtx = MemoryContextSwitchTo(buildstate->tmpCtx);

	/*
	 * if scan wrapped around - flush accumulated entries and start anew
	 *
	 * With parallel scans, we don't have a guarantee the scan does not start
	 * half-way through the relation (serial builds disable sync scans and
	 * always start from block 0, parallel scans require allow_sync=true).
	 *
	 * Building the posting lists assumes the TIDs are monotonic and never go
	 * back, and the wrap around would break that. We handle that by detecting
	 * the wraparound, and flushing all entries. This means we'll later see
	 * two separate entries with non-overlapping TID lists (which can be
	 * combined by merge sort).
	 *
	 * To detect a wraparound, we remember the last TID seen by each worker
	 * (for any key). If the next TID seen by the worker is lower, the scan
	 * must have wrapped around.
	 */
	if (ItemPointerCompare(tid, &buildstate->tid) < 0)
		ginFlushBuildState(buildstate, index);

	/* remember the TID we're about to process */
	buildstate->tid = *tid;

	for (i = 0; i < buildstate->ginstate.origTupdesc->natts; i++)
		ginHeapTupleBulkInsert(buildstate, (OffsetNumber) (i + 1),
							   values[i], isnull[i], tid);

	/*
	 * If we've maxed out our available memory, dump everything to the
	 * tuplesort. We use half the per-worker fraction of maintenance_work_mem,
	 * the other half is used for the tuplesort.
	 */
	if (buildstate->accum.allocatedMemory >= buildstate->work_mem * (Size) 1024)
		ginFlushBuildState(buildstate, index);

	MemoryContextSwitchTo(oldCtx);
}

IndexBuildResult *
ginbuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	double		reltuples;
	GinBuildState buildstate;
	GinBuildState *state = &buildstate;
	Buffer		RootBuffer,
				MetaBuffer;
	ItemPointerData *list;
	Datum		key;
	GinNullCategory category;
	uint32		nlist;
	MemoryContext oldCtx;
	OffsetNumber attnum;

	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	initGinState(&buildstate.ginstate, index);
	buildstate.indtuples = 0;
	memset(&buildstate.buildStats, 0, sizeof(GinStatsData));

	/* Initialize fields for parallel build too. */
	buildstate.bs_numtuples = 0;
	buildstate.bs_reltuples = 0;
	buildstate.bs_leader = NULL;
	memset(&buildstate.tid, 0, sizeof(ItemPointerData));

	/* initialize the meta page */
	MetaBuffer = GinNewBuffer(index);

	/* initialize the root page */
	RootBuffer = GinNewBuffer(index);

	START_CRIT_SECTION();
	GinInitMetabuffer(MetaBuffer);
	MarkBufferDirty(MetaBuffer);
	GinInitBuffer(RootBuffer, GIN_LEAF);
	MarkBufferDirty(RootBuffer);


	UnlockReleaseBuffer(MetaBuffer);
	UnlockReleaseBuffer(RootBuffer);
	END_CRIT_SECTION();

	/* count the root as first entry page */
	buildstate.buildStats.nEntryPages++;

	/*
	 * create a temporary memory context that is used to hold data not yet
	 * dumped out to the index
	 */
	buildstate.tmpCtx = AllocSetContextCreate(CurrentMemoryContext,
											  "Gin build temporary context",
											  ALLOCSET_DEFAULT_SIZES);

	/*
	 * create a temporary memory context that is used for calling
	 * ginExtractEntries(), and can be reset after each tuple
	 */
	buildstate.funcCtx = AllocSetContextCreate(CurrentMemoryContext,
											   "Gin build temporary context for user-defined function",
											   ALLOCSET_DEFAULT_SIZES);

	buildstate.accum.ginstate = &buildstate.ginstate;
	ginInitBA(&buildstate.accum);

	/* Report table scan phase started */
	pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE,
								 PROGRESS_GIN_PHASE_INDEXBUILD_TABLESCAN);

	/*
	 * Attempt to launch parallel worker scan when required
	 *
	 * XXX plan_create_index_workers makes the number of workers dependent on
	 * maintenance_work_mem, requiring 32MB for each worker. For GIN that's
	 * reasonable too, because we sort the data just like btree. It does
	 * ignore the memory used to accumulate data in memory (set by work_mem),
	 * but there is no way to communicate that to plan_create_index_workers.
	 */
	if (indexInfo->ii_ParallelWorkers > 0)
		_gin_begin_parallel(state, heap, index, indexInfo->ii_Concurrent,
							indexInfo->ii_ParallelWorkers);

	/*
	 * If parallel build requested and at least one worker process was
	 * successfully launched, set up coordination state, wait for workers to
	 * complete. Then read all tuples from the shared tuplesort and insert
	 * them into the index.
	 *
	 * In serial mode, simply scan the table and build the index one index
	 * tuple at a time.
	 */
	if (state->bs_leader)
	{
		SortCoordinate coordinate;

		coordinate = (SortCoordinate) palloc0(sizeof(SortCoordinateData));
		coordinate->isWorker = false;
		coordinate->nParticipants =
			state->bs_leader->nparticipanttuplesorts;
		coordinate->sharedsort = state->bs_leader->sharedsort;

		/*
		 * Begin leader tuplesort.
		 *
		 * In cases where parallelism is involved, the leader receives the
		 * same share of maintenance_work_mem as a serial sort (it is
		 * generally treated in the same way as a serial sort once we return).
		 * Parallel worker Tuplesortstates will have received only a fraction
		 * of maintenance_work_mem, though.
		 *
		 * We rely on the lifetime of the Leader Tuplesortstate almost not
		 * overlapping with any worker Tuplesortstate's lifetime.  There may
		 * be some small overlap, but that's okay because we rely on leader
		 * Tuplesortstate only allocating a small, fixed amount of memory
		 * here. When its tuplesort_performsort() is called (by our caller),
		 * and significant amounts of memory are likely to be used, all
		 * workers must have already freed almost all memory held by their
		 * Tuplesortstates (they are about to go away completely, too).  The
		 * overall effect is that maintenance_work_mem always represents an
		 * absolute high watermark on the amount of memory used by a CREATE
		 * INDEX operation, regardless of the use of parallelism or any other
		 * factor.
		 */
		state->bs_sortstate =
			tuplesort_begin_index_gin(heap, index,
									  maintenance_work_mem, coordinate,
									  TUPLESORT_NONE);

		/* scan the relation in parallel and merge per-worker results */
		reltuples = _gin_parallel_merge(state);

		_gin_end_parallel(state->bs_leader, state);
	}
	else						/* no parallel index build */
	{
		/*
		 * Do the heap scan.  We disallow sync scan here because
		 * dataPlaceToPage prefers to receive tuples in TID order.
		 */
		reltuples = table_index_build_scan(heap, index, indexInfo, false, true,
										   ginBuildCallback, &buildstate, NULL);

		/* dump remaining entries to the index */
		oldCtx = MemoryContextSwitchTo(buildstate.tmpCtx);
		ginBeginBAScan(&buildstate.accum);
		while ((list = ginGetBAEntry(&buildstate.accum,
									 &attnum, &key, &category, &nlist)) != NULL)
		{
			/* there could be many entries, so be willing to abort here */
			CHECK_FOR_INTERRUPTS();
			ginEntryInsert(&buildstate.ginstate, attnum, key, category,
						   list, nlist, &buildstate.buildStats);
		}
		MemoryContextSwitchTo(oldCtx);
	}

	MemoryContextDelete(buildstate.funcCtx);
	MemoryContextDelete(buildstate.tmpCtx);

	/*
	 * Update metapage stats
	 */
	buildstate.buildStats.nTotalPages = RelationGetNumberOfBlocks(index);
	ginUpdateStats(index, &buildstate.buildStats, true);

	/*
	 * We didn't write WAL records as we built the index, so if WAL-logging is
	 * required, write all pages to the WAL now.
	 */
	if (RelationNeedsWAL(index))
	{
		log_newpage_range(index, MAIN_FORKNUM,
						  0, RelationGetNumberOfBlocks(index),
						  true);
	}

	/*
	 * Return statistics
	 */
	result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));

	result->heap_tuples = reltuples;
	result->index_tuples = buildstate.indtuples;

	return result;
}

/*
 *	ginbuildempty() -- build an empty gin index in the initialization fork
 */
void
ginbuildempty(Relation index)
{
	Buffer		RootBuffer,
				MetaBuffer;

	/* An empty GIN index has two pages. */
	MetaBuffer = ExtendBufferedRel(BMR_REL(index), INIT_FORKNUM, NULL,
								   EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);
	RootBuffer = ExtendBufferedRel(BMR_REL(index), INIT_FORKNUM, NULL,
								   EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);

	/* Initialize and xlog metabuffer and root buffer. */
	START_CRIT_SECTION();
	GinInitMetabuffer(MetaBuffer);
	MarkBufferDirty(MetaBuffer);
	log_newpage_buffer(MetaBuffer, true);
	GinInitBuffer(RootBuffer, GIN_LEAF);
	MarkBufferDirty(RootBuffer);
	log_newpage_buffer(RootBuffer, false);
	END_CRIT_SECTION();

	/* Unlock and release the buffers. */
	UnlockReleaseBuffer(MetaBuffer);
	UnlockReleaseBuffer(RootBuffer);
}

/*
 * Insert index entries for a single indexable item during "normal"
 * (non-fast-update) insertion
 */
static void
ginHeapTupleInsert(GinState *ginstate, OffsetNumber attnum,
				   Datum value, bool isNull,
				   ItemPointer item)
{
	Datum	   *entries;
	GinNullCategory *categories;
	int32		i,
				nentries;

	entries = ginExtractEntries(ginstate, attnum, value, isNull,
								&nentries, &categories);

	for (i = 0; i < nentries; i++)
		ginEntryInsert(ginstate, attnum, entries[i], categories[i],
					   item, 1, NULL);
}

bool
gininsert(Relation index, Datum *values, bool *isnull,
		  ItemPointer ht_ctid, Relation heapRel,
		  IndexUniqueCheck checkUnique,
		  bool indexUnchanged,
		  IndexInfo *indexInfo)
{
	GinState   *ginstate = (GinState *) indexInfo->ii_AmCache;
	MemoryContext oldCtx;
	MemoryContext insertCtx;
	int			i;

	/* Initialize GinState cache if first call in this statement */
	if (ginstate == NULL)
	{
		oldCtx = MemoryContextSwitchTo(indexInfo->ii_Context);
		ginstate = (GinState *) palloc(sizeof(GinState));
		initGinState(ginstate, index);
		indexInfo->ii_AmCache = ginstate;
		MemoryContextSwitchTo(oldCtx);
	}

	insertCtx = AllocSetContextCreate(CurrentMemoryContext,
									  "Gin insert temporary context",
									  ALLOCSET_DEFAULT_SIZES);

	oldCtx = MemoryContextSwitchTo(insertCtx);

	if (GinGetUseFastUpdate(index))
	{
		GinTupleCollector collector;

		memset(&collector, 0, sizeof(GinTupleCollector));

		for (i = 0; i < ginstate->origTupdesc->natts; i++)
			ginHeapTupleFastCollect(ginstate, &collector,
									(OffsetNumber) (i + 1),
									values[i], isnull[i],
									ht_ctid);

		ginHeapTupleFastInsert(ginstate, &collector);
	}
	else
	{
		for (i = 0; i < ginstate->origTupdesc->natts; i++)
			ginHeapTupleInsert(ginstate, (OffsetNumber) (i + 1),
							   values[i], isnull[i],
							   ht_ctid);
	}

	MemoryContextSwitchTo(oldCtx);
	MemoryContextDelete(insertCtx);

	return false;
}

/*
 * Create parallel context, and launch workers for leader.
 *
 * buildstate argument should be initialized (with the exception of the
 * tuplesort states, which may later be created based on shared
 * state initially set up here).
 *
 * isconcurrent indicates if operation is CREATE INDEX CONCURRENTLY.
 *
 * request is the target number of parallel worker processes to launch.
 *
 * Sets buildstate's GinLeader, which caller must use to shut down parallel
 * mode by passing it to _gin_end_parallel() at the very end of its index
 * build.  If not even a single worker process can be launched, this is
 * never set, and caller should proceed with a serial index build.
 */
static void
_gin_begin_parallel(GinBuildState *buildstate, Relation heap, Relation index,
					bool isconcurrent, int request)
{
	ParallelContext *pcxt;
	int			scantuplesortstates;
	Snapshot	snapshot;
	Size		estginshared;
	Size		estsort;
	GinBuildShared *ginshared;
	Sharedsort *sharedsort;
	GinLeader  *ginleader = (GinLeader *) palloc0(sizeof(GinLeader));
	WalUsage   *walusage;
	BufferUsage *bufferusage;
	bool		leaderparticipates = true;
	int			querylen;

#ifdef DISABLE_LEADER_PARTICIPATION
	leaderparticipates = false;
#endif

	/*
	 * Enter parallel mode, and create context for parallel build of gin index
	 */
	EnterParallelMode();
	Assert(request > 0);
	pcxt = CreateParallelContext("postgres", "_gin_parallel_build_main",
								 request);

	scantuplesortstates = leaderparticipates ? request + 1 : request;

	/*
	 * Prepare for scan of the base relation.  In a normal index build, we use
	 * SnapshotAny because we must retrieve all tuples and do our own time
	 * qual checks (because we have to index RECENTLY_DEAD tuples).  In a
	 * concurrent build, we take a regular MVCC snapshot and index whatever's
	 * live according to that.
	 */
	if (!isconcurrent)
		snapshot = SnapshotAny;
	else
		snapshot = RegisterSnapshot(GetTransactionSnapshot());

	/*
	 * Estimate size for our own PARALLEL_KEY_GIN_SHARED workspace.
	 */
	estginshared = _gin_parallel_estimate_shared(heap, snapshot);
	shm_toc_estimate_chunk(&pcxt->estimator, estginshared);
	estsort = tuplesort_estimate_shared(scantuplesortstates);
	shm_toc_estimate_chunk(&pcxt->estimator, estsort);

	shm_toc_estimate_keys(&pcxt->estimator, 2);

	/*
	 * Estimate space for WalUsage and BufferUsage -- PARALLEL_KEY_WAL_USAGE
	 * and PARALLEL_KEY_BUFFER_USAGE.
	 *
	 * If there are no extensions loaded that care, we could skip this.  We
	 * have no way of knowing whether anyone's looking at pgWalUsage or
	 * pgBufferUsage, so do it unconditionally.
	 */
	shm_toc_estimate_chunk(&pcxt->estimator,
						   mul_size(sizeof(WalUsage), pcxt->nworkers));
	shm_toc_estimate_keys(&pcxt->estimator, 1);
	shm_toc_estimate_chunk(&pcxt->estimator,
						   mul_size(sizeof(BufferUsage), pcxt->nworkers));
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/* Finally, estimate PARALLEL_KEY_QUERY_TEXT space */
	if (debug_query_string)
	{
		querylen = strlen(debug_query_string);
		shm_toc_estimate_chunk(&pcxt->estimator, querylen + 1);
		shm_toc_estimate_keys(&pcxt->estimator, 1);
	}
	else
		querylen = 0;			/* keep compiler quiet */

	/* Everyone's had a chance to ask for space, so now create the DSM */
	InitializeParallelDSM(pcxt);

	/* If no DSM segment was available, back out (do serial build) */
	if (pcxt->seg == NULL)
	{
		if (IsMVCCSnapshot(snapshot))
			UnregisterSnapshot(snapshot);
		DestroyParallelContext(pcxt);
		ExitParallelMode();
		return;
	}

	/* Store shared build state, for which we reserved space */
	ginshared = (GinBuildShared *) shm_toc_allocate(pcxt->toc, estginshared);
	/* Initialize immutable state */
	ginshared->heaprelid = RelationGetRelid(heap);
	ginshared->indexrelid = RelationGetRelid(index);
	ginshared->isconcurrent = isconcurrent;
	ginshared->scantuplesortstates = scantuplesortstates;

	ConditionVariableInit(&ginshared->workersdonecv);
	SpinLockInit(&ginshared->mutex);

	/* Initialize mutable state */
	ginshared->nparticipantsdone = 0;
	ginshared->reltuples = 0.0;
	ginshared->indtuples = 0.0;

	table_parallelscan_initialize(heap,
								  ParallelTableScanFromGinBuildShared(ginshared),
								  snapshot);

	/*
	 * Store shared tuplesort-private state, for which we reserved space.
	 * Then, initialize opaque state using tuplesort routine.
	 */
	sharedsort = (Sharedsort *) shm_toc_allocate(pcxt->toc, estsort);
	tuplesort_initialize_shared(sharedsort, scantuplesortstates,
								pcxt->seg);

	shm_toc_insert(pcxt->toc, PARALLEL_KEY_GIN_SHARED, ginshared);
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_TUPLESORT, sharedsort);

	/* Store query string for workers */
	if (debug_query_string)
	{
		char	   *sharedquery;

		sharedquery = (char *) shm_toc_allocate(pcxt->toc, querylen + 1);
		memcpy(sharedquery, debug_query_string, querylen + 1);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_QUERY_TEXT, sharedquery);
	}

	/*
	 * Allocate space for each worker's WalUsage and BufferUsage; no need to
	 * initialize.
	 */
	walusage = shm_toc_allocate(pcxt->toc,
								mul_size(sizeof(WalUsage), pcxt->nworkers));
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_WAL_USAGE, walusage);
	bufferusage = shm_toc_allocate(pcxt->toc,
								   mul_size(sizeof(BufferUsage), pcxt->nworkers));
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_BUFFER_USAGE, bufferusage);

	/* Launch workers, saving status for leader/caller */
	LaunchParallelWorkers(pcxt);
	ginleader->pcxt = pcxt;
	ginleader->nparticipanttuplesorts = pcxt->nworkers_launched;
	if (leaderparticipates)
		ginleader->nparticipanttuplesorts++;
	ginleader->ginshared = ginshared;
	ginleader->sharedsort = sharedsort;
	ginleader->snapshot = snapshot;
	ginleader->walusage = walusage;
	ginleader->bufferusage = bufferusage;

	/* If no workers were successfully launched, back out (do serial build) */
	if (pcxt->nworkers_launched == 0)
	{
		_gin_end_parallel(ginleader, NULL);
		return;
	}

	/* Save leader state now that it's clear build will be parallel */
	buildstate->bs_leader = ginleader;

	/* Join heap scan ourselves */
	if (leaderparticipates)
		_gin_leader_participate_as_worker(buildstate, heap, index);

	/*
	 * Caller needs to wait for all launched workers when we return.  Make
	 * sure that the failure-to-start case will not hang forever.
	 */
	WaitForParallelWorkersToAttach(pcxt);
}

/*
 * Shut down workers, destroy parallel context, and end parallel mode.
 */
static void
_gin_end_parallel(GinLeader *ginleader, GinBuildState *state)
{
	int			i;

	/* Shutdown worker processes */
	WaitForParallelWorkersToFinish(ginleader->pcxt);

	/*
	 * Next, accumulate WAL usage.  (This must wait for the workers to finish,
	 * or we might get incomplete data.)
	 */
	for (i = 0; i < ginleader->pcxt->nworkers_launched; i++)
		InstrAccumParallelQuery(&ginleader->bufferusage[i], &ginleader->walusage[i]);

	/* Free last reference to MVCC snapshot, if one was used */
	if (IsMVCCSnapshot(ginleader->snapshot))
		UnregisterSnapshot(ginleader->snapshot);
	DestroyParallelContext(ginleader->pcxt);
	ExitParallelMode();
}

/*
 * Within leader, wait for end of heap scan.
 *
 * When called, parallel heap scan started by _gin_begin_parallel() will
 * already be underway within worker processes (when leader participates
 * as a worker, we should end up here just as workers are finishing).
 *
 * Returns the total number of heap tuples scanned.
 */
static double
_gin_parallel_heapscan(GinBuildState *state)
{
	GinBuildShared *ginshared = state->bs_leader->ginshared;
	int			nparticipanttuplesorts;

	nparticipanttuplesorts = state->bs_leader->nparticipanttuplesorts;
	for (;;)
	{
		SpinLockAcquire(&ginshared->mutex);
		if (ginshared->nparticipantsdone == nparticipanttuplesorts)
		{
			/* copy the data into leader state */
			state->bs_reltuples = ginshared->reltuples;
			state->bs_numtuples = ginshared->indtuples;

			SpinLockRelease(&ginshared->mutex);
			break;
		}
		SpinLockRelease(&ginshared->mutex);

		ConditionVariableSleep(&ginshared->workersdonecv,
							   WAIT_EVENT_PARALLEL_CREATE_INDEX_SCAN);
	}

	ConditionVariableCancelSleep();

	return state->bs_reltuples;
}

/*
 * Buffer used to accumulate TIDs from multiple GinTuples for the same key
 * (we read these from the tuplesort, sorted by the key).
 *
 * This is similar to BuildAccumulator in that it's used to collect TIDs
 * in memory before inserting them into the index, but it's much simpler
 * as it only deals with a single index key at a time.
 *
 * When adding TIDs to the buffer, we make sure to keep them sorted, both
 * during the initial table scan (and detecting when the scan wraps around),
 * and during merging (where we do mergesort).
 */
typedef struct GinBuffer
{
	OffsetNumber attnum;
	GinNullCategory category;
	Datum		key;			/* 0 if no key (and keylen == 0) */
	Size		keylen;			/* number of bytes (not typlen) */

	/* type info */
	int16		typlen;
	bool		typbyval;

	/* Number of TIDs to collect before attempt to write some out. */
	int			maxitems;

	/* array of TID values */
	int			nitems;
	int			nfrozen;
	SortSupport ssup;			/* for sorting/comparing keys */
	ItemPointerData *items;
} GinBuffer;

/*
 * Check that TID array contains valid values, and that it's sorted (if we
 * expect it to be).
 */
static void
AssertCheckItemPointers(GinBuffer *buffer)
{
#ifdef USE_ASSERT_CHECKING
	/* we should not have a buffer with no TIDs to sort */
	Assert(buffer->items != NULL);
	Assert(buffer->nitems > 0);

	for (int i = 0; i < buffer->nitems; i++)
	{
		Assert(ItemPointerIsValid(&buffer->items[i]));

		/* don't check ordering for the first TID item */
		if (i == 0)
			continue;

		Assert(ItemPointerCompare(&buffer->items[i - 1], &buffer->items[i]) < 0);
	}
#endif
}

/*
 * GinBuffer checks
 *
 * Make sure the nitems/items fields are consistent (either the array is empty
 * or not empty, the fields need to agree). If there are items, check ordering.
 */
static void
AssertCheckGinBuffer(GinBuffer *buffer)
{
#ifdef USE_ASSERT_CHECKING
	/* if we have any items, the array must exist */
	Assert(!((buffer->nitems > 0) && (buffer->items == NULL)));

	/*
	 * The buffer may be empty, in which case we must not call the check of
	 * item pointers, because that assumes non-emptiness.
	 */
	if (buffer->nitems == 0)
		return;

	/* Make sure the item pointers are valid and sorted. */
	AssertCheckItemPointers(buffer);
#endif
}

/*
 * GinBufferInit
 *		Initialize buffer to store tuples for a GIN index.
 *
 * Initialize the buffer used to accumulate TID for a single key at a time
 * (we process the data sorted), so we know when we received all data for
 * a given key.
 *
 * Initializes sort support procedures for all index attributes.
 */
static GinBuffer *
GinBufferInit(Relation index)
{
	GinBuffer  *buffer = palloc0(sizeof(GinBuffer));
	int			i,
				nKeys;
	TupleDesc	desc = RelationGetDescr(index);

	/*
	 * How many items can we fit into the memory limit? We don't want to end
	 * with too many TIDs. and 64kB seems more than enough. But maybe this
	 * should be tied to maintenance_work_mem or something like that?
	 */
	buffer->maxitems = (64 * 1024L) / sizeof(ItemPointerData);

	nKeys = IndexRelationGetNumberOfKeyAttributes(index);

	buffer->ssup = palloc0(sizeof(SortSupportData) * nKeys);

	/*
	 * Lookup ordering operator for the index key data type, and initialize
	 * the sort support function.
	 */
	for (i = 0; i < nKeys; i++)
	{
		Oid			cmpFunc;
		SortSupport sortKey = &buffer->ssup[i];
		Form_pg_attribute att = TupleDescAttr(desc, i);

		sortKey->ssup_cxt = CurrentMemoryContext;
		sortKey->ssup_collation = index->rd_indcollation[i];

		if (!OidIsValid(sortKey->ssup_collation))
			sortKey->ssup_collation = DEFAULT_COLLATION_OID;

		sortKey->ssup_nulls_first = false;
		sortKey->ssup_attno = i + 1;
		sortKey->abbreviate = false;

		Assert(sortKey->ssup_attno != 0);

		/*
		 * If the compare proc isn't specified in the opclass definition, look
		 * up the index key type's default btree comparator.
		 */
		cmpFunc = index_getprocid(index, i + 1, GIN_COMPARE_PROC);
		if (cmpFunc == InvalidOid)
		{
			TypeCacheEntry *typentry;

			typentry = lookup_type_cache(att->atttypid,
										 TYPECACHE_CMP_PROC_FINFO);
			if (!OidIsValid(typentry->cmp_proc_finfo.fn_oid))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("could not identify a comparison function for type %s",
								format_type_be(att->atttypid))));

			cmpFunc = typentry->cmp_proc_finfo.fn_oid;
		}

		PrepareSortSupportComparisonShim(cmpFunc, sortKey);
	}

	return buffer;
}

/* Is the buffer empty, i.e. has no TID values in the array? */
static bool
GinBufferIsEmpty(GinBuffer *buffer)
{
	return (buffer->nitems == 0);
}

/*
 * GinBufferKeyEquals
 *		Can the buffer store TIDs for the provided GIN tuple (same key)?
 *
 * Compare if the tuple matches the already accumulated data in the GIN
 * buffer. Compare scalar fields first, before the actual key.
 *
 * Returns true if the key matches, and the TID belongs to the buffer, or
 * false if the key does not match.
 */
static bool
GinBufferKeyEquals(GinBuffer *buffer, GinTuple *tup)
{
	int			r;
	Datum		tupkey;

	AssertCheckGinBuffer(buffer);

	if (tup->attrnum != buffer->attnum)
		return false;

	/* same attribute should have the same type info */
	Assert(tup->typbyval == buffer->typbyval);
	Assert(tup->typlen == buffer->typlen);

	if (tup->category != buffer->category)
		return false;

	/*
	 * For NULL/empty keys, this means equality, for normal keys we need to
	 * compare the actual key value.
	 */
	if (buffer->category != GIN_CAT_NORM_KEY)
		return true;

	/*
	 * For the tuple, get either the first sizeof(Datum) bytes for byval
	 * types, or a pointer to the beginning of the data array.
	 */
	tupkey = (buffer->typbyval) ? *(Datum *) tup->data : PointerGetDatum(tup->data);

	r = ApplySortComparator(buffer->key, false,
							tupkey, false,
							&buffer->ssup[buffer->attnum - 1]);

	return (r == 0);
}

/*
 * GinBufferShouldTrim
 *		Should we trim the list of item pointers?
 *
 * By trimming we understand writing out and removing the tuple IDs that
 * we know can't change by future merges. We can deduce the TID up to which
 * this is guaranteed from the "first" TID in each GIN tuple, which provides
 * a "horizon" (for a given key) thanks to the sort.
 *
 * We don't want to do this too often - compressing longer TID lists is more
 * efficient. But we also don't want to accumulate too many TIDs, for two
 * reasons. First, it consumes memory and we might exceed maintenance_work_mem
 * (or whatever limit applies), even if that's unlikely because TIDs are very
 * small so we can fit a lot of them. Second, and more importantly, long TID
 * lists are an issue if the scan wraps around, because a key may get a very
 * wide list (with min/max TID for that key), forcing "full" mergesorts for
 * every list merged into it (instead of the efficient append).
 *
 * So we look at two things when deciding if to trim - if the resulting list
 * (after adding TIDs from the new tuple) would be too long, and if there is
 * enough TIDs to trim (with values less than "first" TID from the new tuple),
 * we do the trim. By enough we mean at least 128 TIDs (mostly an arbitrary
 * number).
 */
static bool
GinBufferShouldTrim(GinBuffer *buffer, GinTuple *tup)
{
	/* not enough TIDs to trim (1024 is somewhat arbitrary number) */
	if (buffer->nfrozen < 1024)
		return false;

	/* no need to trim if we have not hit the memory limit yet */
	if ((buffer->nitems + tup->nitems) < buffer->maxitems)
		return false;

	/*
	 * OK, we have enough frozen TIDs to flush, and we have hit the memory
	 * limit, so it's time to write it out.
	 */
	return true;
}

/*
 * GinBufferStoreTuple
 *		Add data (especially TID list) from a GIN tuple to the buffer.
 *
 * The buffer is expected to be empty (in which case it's initialized), or
 * having the same key. The TID values from the tuple are combined with the
 * stored values using a merge sort.
 *
 * The tuples (for the same key) are expected to be sorted by first TID. But
 * this does not guarantee the lists do not overlap, especially in the leader,
 * because the workers process interleaving data. There should be no overlaps
 * in a single worker - it could happen when the parallel scan wraps around,
 * but we detect that and flush the data (see ginBuildCallbackParallel).
 *
 * By sorting the GinTuple not only by key, but also by the first TID, we make
 * it more less likely the lists will overlap during merge. We merge them using
 * mergesort, but it's cheaper to just append one list to the other.
 *
 * How often can the lists overlap? There should be no overlaps in workers,
 * and in the leader we can see overlaps between lists built by different
 * workers. But the workers merge the items as much as possible, so there
 * should not be too many.
 */
static void
GinBufferStoreTuple(GinBuffer *buffer, GinTuple *tup)
{
	ItemPointerData *items;
	Datum		key;

	AssertCheckGinBuffer(buffer);

	key = _gin_parse_tuple_key(tup);
	items = _gin_parse_tuple_items(tup);

	/* if the buffer is empty, set the fields (and copy the key) */
	if (GinBufferIsEmpty(buffer))
	{
		buffer->category = tup->category;
		buffer->keylen = tup->keylen;
		buffer->attnum = tup->attrnum;

		buffer->typlen = tup->typlen;
		buffer->typbyval = tup->typbyval;

		if (tup->category == GIN_CAT_NORM_KEY)
			buffer->key = datumCopy(key, buffer->typbyval, buffer->typlen);
		else
			buffer->key = (Datum) 0;
	}

	/*
	 * Try freeze TIDs at the beginning of the list, i.e. exclude them from
	 * the mergesort. We can do that with TIDs before the first TID in the new
	 * tuple we're about to add into the buffer.
	 *
	 * We do this incrementally when adding data into the in-memory buffer,
	 * and not later (e.g. when hitting a memory limit), because it allows us
	 * to skip the frozen data during the mergesort, making it cheaper.
	 */

	/*
	 * Check if the last TID in the current list is frozen. This is the case
	 * when merging non-overlapping lists, e.g. in each parallel worker.
	 */
	if ((buffer->nitems > 0) &&
		(ItemPointerCompare(&buffer->items[buffer->nitems - 1],
							GinTupleGetFirst(tup)) == 0))
		buffer->nfrozen = buffer->nitems;

	/*
	 * Now find the last TID we know to be frozen, i.e. the last TID right
	 * before the new GIN tuple.
	 *
	 * Start with the first not-yet-frozen tuple, and walk until we find the
	 * first TID that's higher. If we already know the whole list is frozen
	 * (i.e. nfrozen == nitems), this does nothing.
	 *
	 * XXX This might do a binary search for sufficiently long lists, but it
	 * does not seem worth the complexity. Overlapping lists should be rare
	 * common, TID comparisons are cheap, and we should quickly freeze most of
	 * the list.
	 */
	for (int i = buffer->nfrozen; i < buffer->nitems; i++)
	{
		/* Is the TID after the first TID of the new tuple? Can't freeze. */
		if (ItemPointerCompare(&buffer->items[i],
							   GinTupleGetFirst(tup)) > 0)
			break;

		buffer->nfrozen++;
	}

	/* add the new TIDs into the buffer, combine using merge-sort */
	{
		int			nnew;
		ItemPointer new;

		/*
		 * Resize the array - we do this first, because we'll dereference the
		 * first unfrozen TID, which would fail if the array is NULL. We'll
		 * still pass 0 as number of elements in that array though.
		 */
		if (buffer->items == NULL)
			buffer->items = palloc((buffer->nitems + tup->nitems) * sizeof(ItemPointerData));
		else
			buffer->items = repalloc(buffer->items,
									 (buffer->nitems + tup->nitems) * sizeof(ItemPointerData));

		new = ginMergeItemPointers(&buffer->items[buffer->nfrozen], /* first unfrozen */
								   (buffer->nitems - buffer->nfrozen),	/* num of unfrozen */
								   items, tup->nitems, &nnew);

		Assert(nnew == (tup->nitems + (buffer->nitems - buffer->nfrozen)));

		memcpy(&buffer->items[buffer->nfrozen], new,
			   nnew * sizeof(ItemPointerData));

		pfree(new);

		buffer->nitems += tup->nitems;

		AssertCheckItemPointers(buffer);
	}

	/* free the decompressed TID list */
	pfree(items);
}

/*
 * GinBufferReset
 *		Reset the buffer into a state as if it contains no data.
 */
static void
GinBufferReset(GinBuffer *buffer)
{
	Assert(!GinBufferIsEmpty(buffer));

	/* release byref values, do nothing for by-val ones */
	if ((buffer->category == GIN_CAT_NORM_KEY) && !buffer->typbyval)
		pfree(DatumGetPointer(buffer->key));

	/*
	 * Not required, but makes it more likely to trigger NULL dereference if
	 * using the value incorrectly, etc.
	 */
	buffer->key = (Datum) 0;

	buffer->attnum = 0;
	buffer->category = 0;
	buffer->keylen = 0;
	buffer->nitems = 0;
	buffer->nfrozen = 0;

	buffer->typlen = 0;
	buffer->typbyval = 0;
}

/*
 * GinBufferTrim
 *		Discard the "frozen" part of the TID list (which should have been
 *		written to disk/index before this call).
 */
static void
GinBufferTrim(GinBuffer *buffer)
{
	Assert((buffer->nfrozen > 0) && (buffer->nfrozen <= buffer->nitems));

	memmove(&buffer->items[0], &buffer->items[buffer->nfrozen],
			sizeof(ItemPointerData) * (buffer->nitems - buffer->nfrozen));

	buffer->nitems -= buffer->nfrozen;
	buffer->nfrozen = 0;
}

/*
 * GinBufferFree
 *		Release memory associated with the GinBuffer (including TID array).
 */
static void
GinBufferFree(GinBuffer *buffer)
{
	if (buffer->items)
		pfree(buffer->items);

	/* release byref values, do nothing for by-val ones */
	if (!GinBufferIsEmpty(buffer) &&
		(buffer->category == GIN_CAT_NORM_KEY) && !buffer->typbyval)
		pfree(DatumGetPointer(buffer->key));

	pfree(buffer);
}

/*
 * GinBufferCanAddKey
 *		Check if a given GIN tuple can be added to the current buffer.
 *
 * Returns true if the buffer is either empty or for the same index key.
 */
static bool
GinBufferCanAddKey(GinBuffer *buffer, GinTuple *tup)
{
	/* empty buffer can accept data for any key */
	if (GinBufferIsEmpty(buffer))
		return true;

	/* otherwise just data for the same key */
	return GinBufferKeyEquals(buffer, tup);
}

/*
 * Within leader, wait for end of heap scan and merge per-worker results.
 *
 * After waiting for all workers to finish, merge the per-worker results into
 * the complete index. The results from each worker are sorted by block number
 * (start of the page range). While combining the per-worker results we merge
 * summaries for the same page range, and also fill-in empty summaries for
 * ranges without any tuples.
 *
 * Returns the total number of heap tuples scanned.
 */
static double
_gin_parallel_merge(GinBuildState *state)
{
	GinTuple   *tup;
	Size		tuplen;
	double		reltuples = 0;
	GinBuffer  *buffer;

	/* GIN tuples from workers, merged by leader */
	double		numtuples = 0;

	/* wait for workers to scan table and produce partial results */
	reltuples = _gin_parallel_heapscan(state);

	/* Execute the sort */
	pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE,
								 PROGRESS_GIN_PHASE_PERFORMSORT_2);

	/* do the actual sort in the leader */
	tuplesort_performsort(state->bs_sortstate);

	/*
	 * Initialize buffer to combine entries for the same key.
	 *
	 * The leader is allowed to use the whole maintenance_work_mem buffer to
	 * combine data. The parallel workers already completed.
	 */
	buffer = GinBufferInit(state->ginstate.index);

	/*
	 * Set the progress target for the next phase.  Reset the block number
	 * values set by table_index_build_scan
	 */
	{
		const int	progress_index[] = {
			PROGRESS_CREATEIDX_SUBPHASE,
			PROGRESS_CREATEIDX_TUPLES_TOTAL,
			PROGRESS_SCAN_BLOCKS_TOTAL,
			PROGRESS_SCAN_BLOCKS_DONE
		};
		const int64 progress_vals[] = {
			PROGRESS_GIN_PHASE_MERGE_2,
			state->bs_numtuples,
			0, 0
		};

		pgstat_progress_update_multi_param(4, progress_index, progress_vals);
	}

	/*
	 * Read the GIN tuples from the shared tuplesort, sorted by category and
	 * key. That probably gives us order matching how data is organized in the
	 * index.
	 *
	 * We don't insert the GIN tuples right away, but instead accumulate as
	 * many TIDs for the same key as possible, and then insert that at once.
	 * This way we don't need to decompress/recompress the posting lists, etc.
	 */
	while ((tup = tuplesort_getgintuple(state->bs_sortstate, &tuplen, true)) != NULL)
	{
		MemoryContext oldCtx;

		CHECK_FOR_INTERRUPTS();

		/*
		 * If the buffer can accept the new GIN tuple, just store it there and
		 * we're done. If it's a different key (or maybe too much data) flush
		 * the current contents into the index first.
		 */
		if (!GinBufferCanAddKey(buffer, tup))
		{
			/*
			 * Buffer is not empty and it's storing a different key - flush
			 * the data into the insert, and start a new entry for current
			 * GinTuple.
			 */
			AssertCheckItemPointers(buffer);

			oldCtx = MemoryContextSwitchTo(state->tmpCtx);

			ginEntryInsert(&state->ginstate,
						   buffer->attnum, buffer->key, buffer->category,
						   buffer->items, buffer->nitems, &state->buildStats);

			MemoryContextSwitchTo(oldCtx);
			MemoryContextReset(state->tmpCtx);

			/* discard the existing data */
			GinBufferReset(buffer);
		}

		/*
		 * We're about to add a GIN tuple to the buffer - check the memory
		 * limit first, and maybe write out some of the data into the index
		 * first, if needed (and possible). We only flush the part of the TID
		 * list that we know won't change, and only if there's enough data for
		 * compression to work well.
		 */
		if (GinBufferShouldTrim(buffer, tup))
		{
			Assert(buffer->nfrozen > 0);

			/*
			 * Buffer is not empty and it's storing a different key - flush
			 * the data into the insert, and start a new entry for current
			 * GinTuple.
			 */
			AssertCheckItemPointers(buffer);

			oldCtx = MemoryContextSwitchTo(state->tmpCtx);

			ginEntryInsert(&state->ginstate,
						   buffer->attnum, buffer->key, buffer->category,
						   buffer->items, buffer->nfrozen, &state->buildStats);

			MemoryContextSwitchTo(oldCtx);
			MemoryContextReset(state->tmpCtx);

			/* truncate the data we've just discarded */
			GinBufferTrim(buffer);
		}

		/*
		 * Remember data for the current tuple (either remember the new key,
		 * or append if to the existing data).
		 */
		GinBufferStoreTuple(buffer, tup);

		/* Report progress */
		pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE,
									 ++numtuples);
	}

	/* flush data remaining in the buffer (for the last key) */
	if (!GinBufferIsEmpty(buffer))
	{
		AssertCheckItemPointers(buffer);

		ginEntryInsert(&state->ginstate,
					   buffer->attnum, buffer->key, buffer->category,
					   buffer->items, buffer->nitems, &state->buildStats);

		/* discard the existing data */
		GinBufferReset(buffer);

		/* Report progress */
		pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE,
									 ++numtuples);
	}

	/* relase all the memory */
	GinBufferFree(buffer);

	tuplesort_end(state->bs_sortstate);

	return reltuples;
}

/*
 * Returns size of shared memory required to store state for a parallel
 * gin index build based on the snapshot its parallel scan will use.
 */
static Size
_gin_parallel_estimate_shared(Relation heap, Snapshot snapshot)
{
	/* c.f. shm_toc_allocate as to why BUFFERALIGN is used */
	return add_size(BUFFERALIGN(sizeof(GinBuildShared)),
					table_parallelscan_estimate(heap, snapshot));
}

/*
 * Within leader, participate as a parallel worker.
 */
static void
_gin_leader_participate_as_worker(GinBuildState *buildstate, Relation heap, Relation index)
{
	GinLeader  *ginleader = buildstate->bs_leader;
	int			sortmem;

	/*
	 * Might as well use reliable figure when doling out maintenance_work_mem
	 * (when requested number of workers were not launched, this will be
	 * somewhat higher than it is for other workers).
	 */
	sortmem = maintenance_work_mem / ginleader->nparticipanttuplesorts;

	/* Perform work common to all participants */
	_gin_parallel_scan_and_build(buildstate, ginleader->ginshared,
								 ginleader->sharedsort, heap, index,
								 sortmem, true);
}

/*
 * _gin_process_worker_data
 *		First phase of the key merging, happening in the worker.
 *
 * Depending on the number of distinct keys, the TID lists produced by the
 * callback may be very short (due to frequent evictions in the callback).
 * But combining many tiny lists is expensive, so we try to do as much as
 * possible in the workers and only then pass the results to the leader.
 *
 * We read the tuples sorted by the key, and merge them into larger lists.
 * At the moment there's no memory limit, so this will just produce one
 * huge (sorted) list per key in each worker. Which means the leader will
 * do a very limited number of mergesorts, which is good.
 */
static void
_gin_process_worker_data(GinBuildState *state, Tuplesortstate *worker_sort,
						 bool progress)
{
	GinTuple   *tup;
	Size		tuplen;

	GinBuffer  *buffer;

	/*
	 * Initialize buffer to combine entries for the same key.
	 *
	 * The workers are limited to the same amount of memory as during the sort
	 * in ginBuildCallbackParallel. But this probably should be the 32MB used
	 * during planning, just like there.
	 */
	buffer = GinBufferInit(state->ginstate.index);

	/* sort the raw per-worker data */
	if (progress)
		pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE,
									 PROGRESS_GIN_PHASE_PERFORMSORT_1);

	tuplesort_performsort(state->bs_worker_sort);

	/* reset the number of GIN tuples produced by this worker */
	state->bs_numtuples = 0;

	if (progress)
		pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE,
									 PROGRESS_GIN_PHASE_MERGE_1);

	/*
	 * Read the GIN tuples from the shared tuplesort, sorted by the key, and
	 * merge them into larger chunks for the leader to combine.
	 */
	while ((tup = tuplesort_getgintuple(worker_sort, &tuplen, true)) != NULL)
	{

		CHECK_FOR_INTERRUPTS();

		/*
		 * If the buffer can accept the new GIN tuple, just store it there and
		 * we're done. If it's a different key (or maybe too much data) flush
		 * the current contents into the index first.
		 */
		if (!GinBufferCanAddKey(buffer, tup))
		{
			GinTuple   *ntup;
			Size		ntuplen;

			/*
			 * Buffer is not empty and it's storing a different key - flush
			 * the data into the insert, and start a new entry for current
			 * GinTuple.
			 */
			AssertCheckItemPointers(buffer);

			ntup = _gin_build_tuple(buffer->attnum, buffer->category,
									buffer->key, buffer->typlen, buffer->typbyval,
									buffer->items, buffer->nitems, &ntuplen);

			tuplesort_putgintuple(state->bs_sortstate, ntup, ntuplen);
			state->bs_numtuples++;

			pfree(ntup);

			/* discard the existing data */
			GinBufferReset(buffer);
		}

		/*
		 * We're about to add a GIN tuple to the buffer - check the memory
		 * limit first, and maybe write out some of the data into the index
		 * first, if needed (and possible). We only flush the part of the TID
		 * list that we know won't change, and only if there's enough data for
		 * compression to work well.
		 */
		if (GinBufferShouldTrim(buffer, tup))
		{
			GinTuple   *ntup;
			Size		ntuplen;

			Assert(buffer->nfrozen > 0);

			/*
			 * Buffer is not empty and it's storing a different key - flush
			 * the data into the insert, and start a new entry for current
			 * GinTuple.
			 */
			AssertCheckItemPointers(buffer);

			ntup = _gin_build_tuple(buffer->attnum, buffer->category,
									buffer->key, buffer->typlen, buffer->typbyval,
									buffer->items, buffer->nfrozen, &ntuplen);

			tuplesort_putgintuple(state->bs_sortstate, ntup, ntuplen);

			pfree(ntup);

			/* truncate the data we've just discarded */
			GinBufferTrim(buffer);
		}

		/*
		 * Remember data for the current tuple (either remember the new key,
		 * or append if to the existing data).
		 */
		GinBufferStoreTuple(buffer, tup);
	}

	/* flush data remaining in the buffer (for the last key) */
	if (!GinBufferIsEmpty(buffer))
	{
		GinTuple   *ntup;
		Size		ntuplen;

		AssertCheckItemPointers(buffer);

		ntup = _gin_build_tuple(buffer->attnum, buffer->category,
								buffer->key, buffer->typlen, buffer->typbyval,
								buffer->items, buffer->nitems, &ntuplen);

		tuplesort_putgintuple(state->bs_sortstate, ntup, ntuplen);
		state->bs_numtuples++;

		pfree(ntup);

		/* discard the existing data */
		GinBufferReset(buffer);
	}

	/* relase all the memory */
	GinBufferFree(buffer);

	tuplesort_end(worker_sort);
}

/*
 * Perform a worker's portion of a parallel GIN index build sort.
 *
 * This generates a tuplesort for the worker portion of the table.
 *
 * sortmem is the amount of working memory to use within each worker,
 * expressed in KBs.
 *
 * When this returns, workers are done, and need only release resources.
 *
 * Before feeding data into a shared tuplesort (for the leader process),
 * the workers process data in two phases.
 *
 * 1) A worker reads a portion of rows from the table, accumulates entries
 * in memory, and flushes them into a private tuplesort (e.g. because of
 * using too much memory).
 *
 * 2) The private tuplesort gets sorted (by key and TID), the worker reads
 * the data again, and combines the entries as much as possible. This has
 * to happen eventually, and this way it's done in workers in parallel.
 *
 * Finally, the combined entries are written into the shared tuplesort, so
 * that the leader can process them.
 *
 * How well this works (compared to just writing entries into the shared
 * tuplesort) depends on the data set. For large tables with many distinct
 * keys this helps a lot. With many distinct keys it's likely the buffers has
 * to be flushed often, generating many entries with the same key and short
 * TID lists. These entries need to be sorted and merged at some point,
 * before writing them to the index. The merging is quite expensive, it can
 * easily be ~50% of a serial build, and doing as much of it in the workers
 * means it's parallelized. The leader still has to merge results from the
 * workers, but it's much more efficient to merge few large entries than
 * many tiny ones.
 *
 * This also reduces the amount of data the workers pass to the leader through
 * the shared tuplesort. OTOH the workers need more space for the private sort,
 * possibly up to 2x of the data, if no entries be merged in a worker. But this
 * is very unlikely, and the only consequence is inefficiency, so we ignore it.
 */
static void
_gin_parallel_scan_and_build(GinBuildState *state,
							 GinBuildShared *ginshared, Sharedsort *sharedsort,
							 Relation heap, Relation index,
							 int sortmem, bool progress)
{
	SortCoordinate coordinate;
	TableScanDesc scan;
	double		reltuples;
	IndexInfo  *indexInfo;

	/* Initialize local tuplesort coordination state */
	coordinate = palloc0(sizeof(SortCoordinateData));
	coordinate->isWorker = true;
	coordinate->nParticipants = -1;
	coordinate->sharedsort = sharedsort;

	/* remember how much space is allowed for the accumulated entries */
	state->work_mem = (sortmem / 2);

	/* Begin "partial" tuplesort */
	state->bs_sortstate = tuplesort_begin_index_gin(heap, index,
													state->work_mem,
													coordinate,
													TUPLESORT_NONE);

	/* Local per-worker sort of raw-data */
	state->bs_worker_sort = tuplesort_begin_index_gin(heap, index,
													  state->work_mem,
													  NULL,
													  TUPLESORT_NONE);

	/* Join parallel scan */
	indexInfo = BuildIndexInfo(index);
	indexInfo->ii_Concurrent = ginshared->isconcurrent;

	scan = table_beginscan_parallel(heap,
									ParallelTableScanFromGinBuildShared(ginshared));

	reltuples = table_index_build_scan(heap, index, indexInfo, true, progress,
									   ginBuildCallbackParallel, state, scan);

	/* write remaining accumulated entries */
	ginFlushBuildState(state, index);

	/*
	 * Do the first phase of in-worker processing - sort the data produced by
	 * the callback, and combine them into much larger chunks and place that
	 * into the shared tuplestore for leader to process.
	 */
	_gin_process_worker_data(state, state->bs_worker_sort, progress);

	/* sort the GIN tuples built by this worker */
	tuplesort_performsort(state->bs_sortstate);

	state->bs_reltuples += reltuples;

	/*
	 * Done.  Record ambuild statistics.
	 */
	SpinLockAcquire(&ginshared->mutex);
	ginshared->nparticipantsdone++;
	ginshared->reltuples += state->bs_reltuples;
	ginshared->indtuples += state->bs_numtuples;
	SpinLockRelease(&ginshared->mutex);

	/* Notify leader */
	ConditionVariableSignal(&ginshared->workersdonecv);

	tuplesort_end(state->bs_sortstate);
}

/*
 * Perform work within a launched parallel process.
 */
void
_gin_parallel_build_main(dsm_segment *seg, shm_toc *toc)
{
	char	   *sharedquery;
	GinBuildShared *ginshared;
	Sharedsort *sharedsort;
	GinBuildState buildstate;
	Relation	heapRel;
	Relation	indexRel;
	LOCKMODE	heapLockmode;
	LOCKMODE	indexLockmode;
	WalUsage   *walusage;
	BufferUsage *bufferusage;
	int			sortmem;

	/*
	 * The only possible status flag that can be set to the parallel worker is
	 * PROC_IN_SAFE_IC.
	 */
	Assert((MyProc->statusFlags == 0) ||
		   (MyProc->statusFlags == PROC_IN_SAFE_IC));

	/* Set debug_query_string for individual workers first */
	sharedquery = shm_toc_lookup(toc, PARALLEL_KEY_QUERY_TEXT, true);
	debug_query_string = sharedquery;

	/* Report the query string from leader */
	pgstat_report_activity(STATE_RUNNING, debug_query_string);

	/* Look up gin shared state */
	ginshared = shm_toc_lookup(toc, PARALLEL_KEY_GIN_SHARED, false);

	/* Open relations using lock modes known to be obtained by index.c */
	if (!ginshared->isconcurrent)
	{
		heapLockmode = ShareLock;
		indexLockmode = AccessExclusiveLock;
	}
	else
	{
		heapLockmode = ShareUpdateExclusiveLock;
		indexLockmode = RowExclusiveLock;
	}

	/* Open relations within worker */
	heapRel = table_open(ginshared->heaprelid, heapLockmode);
	indexRel = index_open(ginshared->indexrelid, indexLockmode);

	/* initialize the GIN build state */
	initGinState(&buildstate.ginstate, indexRel);
	buildstate.indtuples = 0;
	memset(&buildstate.buildStats, 0, sizeof(GinStatsData));
	memset(&buildstate.tid, 0, sizeof(ItemPointerData));

	/*
	 * create a temporary memory context that is used to hold data not yet
	 * dumped out to the index
	 */
	buildstate.tmpCtx = AllocSetContextCreate(CurrentMemoryContext,
											  "Gin build temporary context",
											  ALLOCSET_DEFAULT_SIZES);

	/*
	 * create a temporary memory context that is used for calling
	 * ginExtractEntries(), and can be reset after each tuple
	 */
	buildstate.funcCtx = AllocSetContextCreate(CurrentMemoryContext,
											   "Gin build temporary context for user-defined function",
											   ALLOCSET_DEFAULT_SIZES);

	buildstate.accum.ginstate = &buildstate.ginstate;
	ginInitBA(&buildstate.accum);


	/* Look up shared state private to tuplesort.c */
	sharedsort = shm_toc_lookup(toc, PARALLEL_KEY_TUPLESORT, false);
	tuplesort_attach_shared(sharedsort, seg);

	/* Prepare to track buffer usage during parallel execution */
	InstrStartParallelQuery();

	/*
	 * Might as well use reliable figure when doling out maintenance_work_mem
	 * (when requested number of workers were not launched, this will be
	 * somewhat higher than it is for other workers).
	 */
	sortmem = maintenance_work_mem / ginshared->scantuplesortstates;

	_gin_parallel_scan_and_build(&buildstate, ginshared, sharedsort,
								 heapRel, indexRel, sortmem, false);

	/* Report WAL/buffer usage during parallel execution */
	bufferusage = shm_toc_lookup(toc, PARALLEL_KEY_BUFFER_USAGE, false);
	walusage = shm_toc_lookup(toc, PARALLEL_KEY_WAL_USAGE, false);
	InstrEndParallelQuery(&bufferusage[ParallelWorkerNumber],
						  &walusage[ParallelWorkerNumber]);

	index_close(indexRel, indexLockmode);
	table_close(heapRel, heapLockmode);
}

/*
 * Used to keep track of compressed TID lists when building a GIN tuple.
 */
typedef struct
{
	dlist_node	node;			/* linked list pointers */
	GinPostingList *seg;
} GinSegmentInfo;

/*
 * _gin_build_tuple
 *		Serialize the state for an index key into a tuple for tuplesort.
 *
 * The tuple has a number of scalar fields (mostly matching the build state),
 * and then a data array that stores the key first, and then the TID list.
 *
 * For by-reference data types, we store the actual data. For by-val types
 * we simply copy the whole Datum, so that we don't have to care about stuff
 * like endianess etc. We could make it a little bit smaller, but it's not
 * worth it - it's a tiny fraction of the data, and we need to MAXALIGN the
 * start of the TID list anyway. So we wouldn't save anything.
 *
 * The TID list is serialized as compressed - it's highly compressible, and
 * we already have ginCompressPostingList for this purpose. The list may be
 * pretty long, so we compress it into multiple segments and then copy all
 * of that into the GIN tuple.
 */
static GinTuple *
_gin_build_tuple(OffsetNumber attrnum, unsigned char category,
				 Datum key, int16 typlen, bool typbyval,
				 ItemPointerData *items, uint32 nitems,
				 Size *len)
{
	GinTuple   *tuple;
	char	   *ptr;

	Size		tuplen;
	int			keylen;

	dlist_mutable_iter iter;
	dlist_head	segments;
	int			ncompressed;
	Size		compresslen;

	/*
	 * Calculate how long is the key value. Only keys with GIN_CAT_NORM_KEY
	 * have actual non-empty key. We include varlena headers and \0 bytes for
	 * strings, to make it easier to access the data in-line.
	 *
	 * For byval types we simply copy the whole Datum. We could store just the
	 * necessary bytes, but this is simpler to work with and not worth the
	 * extra complexity. Moreover we still need to do the MAXALIGN to allow
	 * direct access to items pointers.
	 *
	 * XXX Note that for byval types we store the whole datum, no matter what
	 * the typlen value is.
	 */
	if (category != GIN_CAT_NORM_KEY)
		keylen = 0;
	else if (typbyval)
		keylen = sizeof(Datum);
	else if (typlen > 0)
		keylen = typlen;
	else if (typlen == -1)
		keylen = VARSIZE_ANY(key);
	else if (typlen == -2)
		keylen = strlen(DatumGetPointer(key)) + 1;
	else
		elog(ERROR, "unexpected typlen value (%d)", typlen);

	/* compress the item pointers */
	ncompressed = 0;
	compresslen = 0;
	dlist_init(&segments);

	/* generate compressed segments of TID list chunks */
	while (ncompressed < nitems)
	{
		int			cnt;
		GinSegmentInfo *seginfo = palloc(sizeof(GinSegmentInfo));

		seginfo->seg = ginCompressPostingList(&items[ncompressed],
											  (nitems - ncompressed),
											  UINT16_MAX,
											  &cnt);

		ncompressed += cnt;
		compresslen += SizeOfGinPostingList(seginfo->seg);

		dlist_push_tail(&segments, &seginfo->node);
	}

	/*
	 * Determine GIN tuple length with all the data included. Be careful about
	 * alignment, to allow direct access to compressed segments (those require
	 * only SHORTALIGN).
	 */
	tuplen = SHORTALIGN(offsetof(GinTuple, data) + keylen) + compresslen;

	*len = tuplen;

	/*
	 * Allocate space for the whole GIN tuple.
	 *
	 * The palloc0 is needed - writetup_index_gin will write the whole tuple
	 * to disk, so we need to make sure the padding bytes are defined
	 * (otherwise valgrind would report this).
	 */
	tuple = palloc0(tuplen);

	tuple->tuplen = tuplen;
	tuple->attrnum = attrnum;
	tuple->category = category;
	tuple->keylen = keylen;
	tuple->nitems = nitems;

	/* key type info */
	tuple->typlen = typlen;
	tuple->typbyval = typbyval;

	/*
	 * Copy the key and items into the tuple. First the key value, which we
	 * can simply copy right at the beginning of the data array.
	 */
	if (category == GIN_CAT_NORM_KEY)
	{
		if (typbyval)
		{
			memcpy(tuple->data, &key, sizeof(Datum));
		}
		else if (typlen > 0)	/* byref, fixed length */
		{
			memcpy(tuple->data, DatumGetPointer(key), typlen);
		}
		else if (typlen == -1)
		{
			memcpy(tuple->data, DatumGetPointer(key), keylen);
		}
		else if (typlen == -2)
		{
			memcpy(tuple->data, DatumGetPointer(key), keylen);
		}
	}

	/* finally, copy the TIDs into the array */
	ptr = (char *) tuple + SHORTALIGN(offsetof(GinTuple, data) + keylen);

	/* copy in the compressed data, and free the segments */
	dlist_foreach_modify(iter, &segments)
	{
		GinSegmentInfo *seginfo = dlist_container(GinSegmentInfo, node, iter.cur);

		memcpy(ptr, seginfo->seg, SizeOfGinPostingList(seginfo->seg));

		ptr += SizeOfGinPostingList(seginfo->seg);

		dlist_delete(&seginfo->node);

		pfree(seginfo->seg);
		pfree(seginfo);
	}

	return tuple;
}

/*
 * _gin_parse_tuple_key
 *		Return a Datum representing the key stored in the tuple.
 *
 * Most of the tuple fields are directly accessible, the only thing that
 * needs more care is the key and the TID list.
 *
 * For the key, this returns a regular Datum representing it. It's either the
 * actual key value, or a pointer to the beginning of the data array (which is
 * where the data was copied by _gin_build_tuple).
 */
static Datum
_gin_parse_tuple_key(GinTuple *a)
{
	Datum		key;

	if (a->category != GIN_CAT_NORM_KEY)
		return (Datum) 0;

	if (a->typbyval)
	{
		memcpy(&key, a->data, a->keylen);
		return key;
	}

	return PointerGetDatum(a->data);
}

/*
* _gin_parse_tuple_items
 *		Return a pointer to a palloc'd array of decompressed TID array.
 */
static ItemPointer
_gin_parse_tuple_items(GinTuple *a)
{
	int			len;
	char	   *ptr;
	int			ndecoded;
	ItemPointer items;

	len = a->tuplen - SHORTALIGN(offsetof(GinTuple, data) + a->keylen);
	ptr = (char *) a + SHORTALIGN(offsetof(GinTuple, data) + a->keylen);

	items = ginPostingListDecodeAllSegments((GinPostingList *) ptr, len, &ndecoded);

	Assert(ndecoded == a->nitems);

	return (ItemPointer) items;
}

/*
 * _gin_compare_tuples
 *		Compare GIN tuples, used by tuplesort during parallel index build.
 *
 * The scalar fields (attrnum, category) are compared first, the key value is
 * compared last. The comparisons are done using type-specific sort support
 * functions.
 *
 * If the key value matches, we compare the first TID value in the TID list,
 * which means the tuples are merged in an order in which they are most
 * likely to be simply concatenated. (This "first" TID will also allow us
 * to determine a point up to which the list is fully determined and can be
 * written into the index to enforce a memory limit etc.)
 */
int
_gin_compare_tuples(GinTuple *a, GinTuple *b, SortSupport ssup)
{
	int			r;
	Datum		keya,
				keyb;

	if (a->attrnum < b->attrnum)
		return -1;

	if (a->attrnum > b->attrnum)
		return 1;

	if (a->category < b->category)
		return -1;

	if (a->category > b->category)
		return 1;

	if (a->category == GIN_CAT_NORM_KEY)
	{
		keya = _gin_parse_tuple_key(a);
		keyb = _gin_parse_tuple_key(b);

		r = ApplySortComparator(keya, false,
								keyb, false,
								&ssup[a->attrnum - 1]);

		/* if the key is the same, consider the first TID in the array */
		return (r != 0) ? r : ItemPointerCompare(GinTupleGetFirst(a),
												 GinTupleGetFirst(b));
	}

	return ItemPointerCompare(GinTupleGetFirst(a),
							  GinTupleGetFirst(b));
}
