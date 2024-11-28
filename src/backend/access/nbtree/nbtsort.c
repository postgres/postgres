/*-------------------------------------------------------------------------
 *
 * nbtsort.c
 *		Build a btree from sorted input by loading leaf pages sequentially.
 *
 * NOTES
 *
 * We use tuplesort.c to sort the given index tuples into order.
 * Then we scan the index tuples in order and build the btree pages
 * for each level.  We load source tuples into leaf-level pages.
 * Whenever we fill a page at one level, we add a link to it to its
 * parent level (starting a new parent level if necessary).  When
 * done, we write out each final page on each level, adding it to
 * its parent level.  When we have only one page on a level, it must be
 * the root -- it can be attached to the btree metapage and we are done.
 *
 * It is not wise to pack the pages entirely full, since then *any*
 * insertion would cause a split (and not only of the leaf page; the need
 * for a split would cascade right up the tree).  The steady-state load
 * factor for btrees is usually estimated at 70%.  We choose to pack leaf
 * pages to the user-controllable fill factor (default 90%) while upper pages
 * are always packed to 70%.  This gives us reasonable density (there aren't
 * many upper pages if the keys are reasonable-size) without risking a lot of
 * cascading splits during early insertions.
 *
 * We use the bulk smgr loading facility to bypass the buffer cache and
 * WAL-log the pages efficiently.
 *
 * This code isn't concerned about the FSM at all. The caller is responsible
 * for initializing that.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtsort.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/nbtree.h"
#include "access/parallel.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "commands/progress.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bulk_write.h"
#include "tcop/tcopprot.h"		/* pgrminclude ignore */
#include "utils/rel.h"
#include "utils/sortsupport.h"
#include "utils/tuplesort.h"


/* Magic numbers for parallel state sharing */
#define PARALLEL_KEY_BTREE_SHARED		UINT64CONST(0xA000000000000001)
#define PARALLEL_KEY_TUPLESORT			UINT64CONST(0xA000000000000002)
#define PARALLEL_KEY_TUPLESORT_SPOOL2	UINT64CONST(0xA000000000000003)
#define PARALLEL_KEY_QUERY_TEXT			UINT64CONST(0xA000000000000004)
#define PARALLEL_KEY_WAL_USAGE			UINT64CONST(0xA000000000000005)
#define PARALLEL_KEY_BUFFER_USAGE		UINT64CONST(0xA000000000000006)

/*
 * DISABLE_LEADER_PARTICIPATION disables the leader's participation in
 * parallel index builds.  This may be useful as a debugging aid.
#undef DISABLE_LEADER_PARTICIPATION
 */

/*
 * Status record for spooling/sorting phase.  (Note we may have two of
 * these due to the special requirements for uniqueness-checking with
 * dead tuples.)
 */
typedef struct BTSpool
{
	Tuplesortstate *sortstate;	/* state data for tuplesort.c */
	Relation	heap;
	Relation	index;
	bool		isunique;
	bool		nulls_not_distinct;
} BTSpool;

/*
 * Status for index builds performed in parallel.  This is allocated in a
 * dynamic shared memory segment.  Note that there is a separate tuplesort TOC
 * entry, private to tuplesort.c but allocated by this module on its behalf.
 */
typedef struct BTShared
{
	/*
	 * These fields are not modified during the sort.  They primarily exist
	 * for the benefit of worker processes that need to create BTSpool state
	 * corresponding to that used by the leader.
	 */
	Oid			heaprelid;
	Oid			indexrelid;
	bool		isunique;
	bool		nulls_not_distinct;
	bool		isconcurrent;
	int			scantuplesortstates;

	/* Query ID, for report in worker processes */
	uint64		queryid;

	/*
	 * workersdonecv is used to monitor the progress of workers.  All parallel
	 * participants must indicate that they are done before leader can use
	 * mutable state that workers maintain during scan (and before leader can
	 * proceed to tuplesort_performsort()).
	 */
	ConditionVariable workersdonecv;

	/*
	 * mutex protects all fields before heapdesc.
	 *
	 * These fields contain status information of interest to B-Tree index
	 * builds that must work just the same when an index is built in parallel.
	 */
	slock_t		mutex;

	/*
	 * Mutable state that is maintained by workers, and reported back to
	 * leader at end of parallel scan.
	 *
	 * nparticipantsdone is number of worker processes finished.
	 *
	 * reltuples is the total number of input heap tuples.
	 *
	 * havedead indicates if RECENTLY_DEAD tuples were encountered during
	 * build.
	 *
	 * indtuples is the total number of tuples that made it into the index.
	 *
	 * brokenhotchain indicates if any worker detected a broken HOT chain
	 * during build.
	 */
	int			nparticipantsdone;
	double		reltuples;
	bool		havedead;
	double		indtuples;
	bool		brokenhotchain;

	/*
	 * ParallelTableScanDescData data follows. Can't directly embed here, as
	 * implementations of the parallel table scan desc interface might need
	 * stronger alignment.
	 */
} BTShared;

/*
 * Return pointer to a BTShared's parallel table scan.
 *
 * c.f. shm_toc_allocate as to why BUFFERALIGN is used, rather than just
 * MAXALIGN.
 */
#define ParallelTableScanFromBTShared(shared) \
	(ParallelTableScanDesc) ((char *) (shared) + BUFFERALIGN(sizeof(BTShared)))

/*
 * Status for leader in parallel index build.
 */
typedef struct BTLeader
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
	 * btshared is the shared state for entire build.  sharedsort is the
	 * shared, tuplesort-managed state passed to each process tuplesort.
	 * sharedsort2 is the corresponding btspool2 shared state, used only when
	 * building unique indexes.  snapshot is the snapshot used by the scan iff
	 * an MVCC snapshot is required.
	 */
	BTShared   *btshared;
	Sharedsort *sharedsort;
	Sharedsort *sharedsort2;
	Snapshot	snapshot;
	WalUsage   *walusage;
	BufferUsage *bufferusage;
} BTLeader;

/*
 * Working state for btbuild and its callback.
 *
 * When parallel CREATE INDEX is used, there is a BTBuildState for each
 * participant.
 */
typedef struct BTBuildState
{
	bool		isunique;
	bool		nulls_not_distinct;
	bool		havedead;
	Relation	heap;
	BTSpool    *spool;

	/*
	 * spool2 is needed only when the index is a unique index. Dead tuples are
	 * put into spool2 instead of spool in order to avoid uniqueness check.
	 */
	BTSpool    *spool2;
	double		indtuples;

	/*
	 * btleader is only present when a parallel index build is performed, and
	 * only in the leader process. (Actually, only the leader has a
	 * BTBuildState.  Workers have their own spool and spool2, though.)
	 */
	BTLeader   *btleader;
} BTBuildState;

/*
 * Status record for a btree page being built.  We have one of these
 * for each active tree level.
 */
typedef struct BTPageState
{
	BulkWriteBuffer btps_buf;	/* workspace for page building */
	BlockNumber btps_blkno;		/* block # to write this page at */
	IndexTuple	btps_lowkey;	/* page's strict lower bound pivot tuple */
	OffsetNumber btps_lastoff;	/* last item offset loaded */
	Size		btps_lastextra; /* last item's extra posting list space */
	uint32		btps_level;		/* tree level (0 = leaf) */
	Size		btps_full;		/* "full" if less than this much free space */
	struct BTPageState *btps_next;	/* link to parent level, if any */
} BTPageState;

/*
 * Overall status record for index writing phase.
 */
typedef struct BTWriteState
{
	Relation	heap;
	Relation	index;
	BulkWriteState *bulkstate;
	BTScanInsert inskey;		/* generic insertion scankey */
	BlockNumber btws_pages_alloced; /* # pages allocated */
} BTWriteState;


static double _bt_spools_heapscan(Relation heap, Relation index,
								  BTBuildState *buildstate, IndexInfo *indexInfo);
static void _bt_spooldestroy(BTSpool *btspool);
static void _bt_spool(BTSpool *btspool, ItemPointer self,
					  Datum *values, bool *isnull);
static void _bt_leafbuild(BTSpool *btspool, BTSpool *btspool2);
static void _bt_build_callback(Relation index, ItemPointer tid, Datum *values,
							   bool *isnull, bool tupleIsAlive, void *state);
static BulkWriteBuffer _bt_blnewpage(BTWriteState *wstate, uint32 level);
static BTPageState *_bt_pagestate(BTWriteState *wstate, uint32 level);
static void _bt_slideleft(Page rightmostpage);
static void _bt_sortaddtup(Page page, Size itemsize,
						   IndexTuple itup, OffsetNumber itup_off,
						   bool newfirstdataitem);
static void _bt_buildadd(BTWriteState *wstate, BTPageState *state,
						 IndexTuple itup, Size truncextra);
static void _bt_sort_dedup_finish_pending(BTWriteState *wstate,
										  BTPageState *state,
										  BTDedupState dstate);
static void _bt_uppershutdown(BTWriteState *wstate, BTPageState *state);
static void _bt_load(BTWriteState *wstate,
					 BTSpool *btspool, BTSpool *btspool2);
static void _bt_begin_parallel(BTBuildState *buildstate, bool isconcurrent,
							   int request);
static void _bt_end_parallel(BTLeader *btleader);
static Size _bt_parallel_estimate_shared(Relation heap, Snapshot snapshot);
static double _bt_parallel_heapscan(BTBuildState *buildstate,
									bool *brokenhotchain);
static void _bt_leader_participate_as_worker(BTBuildState *buildstate);
static void _bt_parallel_scan_and_sort(BTSpool *btspool, BTSpool *btspool2,
									   BTShared *btshared, Sharedsort *sharedsort,
									   Sharedsort *sharedsort2, int sortmem,
									   bool progress);


/*
 *	btbuild() -- build a new btree index.
 */
IndexBuildResult *
btbuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	BTBuildState buildstate;
	double		reltuples;

#ifdef BTREE_BUILD_STATS
	if (log_btree_build_stats)
		ResetUsage();
#endif							/* BTREE_BUILD_STATS */

	buildstate.isunique = indexInfo->ii_Unique;
	buildstate.nulls_not_distinct = indexInfo->ii_NullsNotDistinct;
	buildstate.havedead = false;
	buildstate.heap = heap;
	buildstate.spool = NULL;
	buildstate.spool2 = NULL;
	buildstate.indtuples = 0;
	buildstate.btleader = NULL;

	/*
	 * We expect to be called exactly once for any index relation. If that's
	 * not the case, big trouble's what we have.
	 */
	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	reltuples = _bt_spools_heapscan(heap, index, &buildstate, indexInfo);

	/*
	 * Finish the build by (1) completing the sort of the spool file, (2)
	 * inserting the sorted tuples into btree pages and (3) building the upper
	 * levels.  Finally, it may also be necessary to end use of parallelism.
	 */
	_bt_leafbuild(buildstate.spool, buildstate.spool2);
	_bt_spooldestroy(buildstate.spool);
	if (buildstate.spool2)
		_bt_spooldestroy(buildstate.spool2);
	if (buildstate.btleader)
		_bt_end_parallel(buildstate.btleader);

	result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));

	result->heap_tuples = reltuples;
	result->index_tuples = buildstate.indtuples;

#ifdef BTREE_BUILD_STATS
	if (log_btree_build_stats)
	{
		ShowUsage("BTREE BUILD STATS");
		ResetUsage();
	}
#endif							/* BTREE_BUILD_STATS */

	return result;
}

/*
 * Create and initialize one or two spool structures, and save them in caller's
 * buildstate argument.  May also fill-in fields within indexInfo used by index
 * builds.
 *
 * Scans the heap, possibly in parallel, filling spools with IndexTuples.  This
 * routine encapsulates all aspects of managing parallelism.  Caller need only
 * call _bt_end_parallel() in parallel case after it is done with spool/spool2.
 *
 * Returns the total number of heap tuples scanned.
 */
static double
_bt_spools_heapscan(Relation heap, Relation index, BTBuildState *buildstate,
					IndexInfo *indexInfo)
{
	BTSpool    *btspool = (BTSpool *) palloc0(sizeof(BTSpool));
	SortCoordinate coordinate = NULL;
	double		reltuples = 0;

	/*
	 * We size the sort area as maintenance_work_mem rather than work_mem to
	 * speed index creation.  This should be OK since a single backend can't
	 * run multiple index creations in parallel (see also: notes on
	 * parallelism and maintenance_work_mem below).
	 */
	btspool->heap = heap;
	btspool->index = index;
	btspool->isunique = indexInfo->ii_Unique;
	btspool->nulls_not_distinct = indexInfo->ii_NullsNotDistinct;

	/* Save as primary spool */
	buildstate->spool = btspool;

	/* Report table scan phase started */
	pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE,
								 PROGRESS_BTREE_PHASE_INDEXBUILD_TABLESCAN);

	/* Attempt to launch parallel worker scan when required */
	if (indexInfo->ii_ParallelWorkers > 0)
		_bt_begin_parallel(buildstate, indexInfo->ii_Concurrent,
						   indexInfo->ii_ParallelWorkers);

	/*
	 * If parallel build requested and at least one worker process was
	 * successfully launched, set up coordination state
	 */
	if (buildstate->btleader)
	{
		coordinate = (SortCoordinate) palloc0(sizeof(SortCoordinateData));
		coordinate->isWorker = false;
		coordinate->nParticipants =
			buildstate->btleader->nparticipanttuplesorts;
		coordinate->sharedsort = buildstate->btleader->sharedsort;
	}

	/*
	 * Begin serial/leader tuplesort.
	 *
	 * In cases where parallelism is involved, the leader receives the same
	 * share of maintenance_work_mem as a serial sort (it is generally treated
	 * in the same way as a serial sort once we return).  Parallel worker
	 * Tuplesortstates will have received only a fraction of
	 * maintenance_work_mem, though.
	 *
	 * We rely on the lifetime of the Leader Tuplesortstate almost not
	 * overlapping with any worker Tuplesortstate's lifetime.  There may be
	 * some small overlap, but that's okay because we rely on leader
	 * Tuplesortstate only allocating a small, fixed amount of memory here.
	 * When its tuplesort_performsort() is called (by our caller), and
	 * significant amounts of memory are likely to be used, all workers must
	 * have already freed almost all memory held by their Tuplesortstates
	 * (they are about to go away completely, too).  The overall effect is
	 * that maintenance_work_mem always represents an absolute high watermark
	 * on the amount of memory used by a CREATE INDEX operation, regardless of
	 * the use of parallelism or any other factor.
	 */
	buildstate->spool->sortstate =
		tuplesort_begin_index_btree(heap, index, buildstate->isunique,
									buildstate->nulls_not_distinct,
									maintenance_work_mem, coordinate,
									TUPLESORT_NONE);

	/*
	 * If building a unique index, put dead tuples in a second spool to keep
	 * them out of the uniqueness check.  We expect that the second spool (for
	 * dead tuples) won't get very full, so we give it only work_mem.
	 */
	if (indexInfo->ii_Unique)
	{
		BTSpool    *btspool2 = (BTSpool *) palloc0(sizeof(BTSpool));
		SortCoordinate coordinate2 = NULL;

		/* Initialize secondary spool */
		btspool2->heap = heap;
		btspool2->index = index;
		btspool2->isunique = false;
		/* Save as secondary spool */
		buildstate->spool2 = btspool2;

		if (buildstate->btleader)
		{
			/*
			 * Set up non-private state that is passed to
			 * tuplesort_begin_index_btree() about the basic high level
			 * coordination of a parallel sort.
			 */
			coordinate2 = (SortCoordinate) palloc0(sizeof(SortCoordinateData));
			coordinate2->isWorker = false;
			coordinate2->nParticipants =
				buildstate->btleader->nparticipanttuplesorts;
			coordinate2->sharedsort = buildstate->btleader->sharedsort2;
		}

		/*
		 * We expect that the second one (for dead tuples) won't get very
		 * full, so we give it only work_mem
		 */
		buildstate->spool2->sortstate =
			tuplesort_begin_index_btree(heap, index, false, false, work_mem,
										coordinate2, TUPLESORT_NONE);
	}

	/* Fill spool using either serial or parallel heap scan */
	if (!buildstate->btleader)
		reltuples = table_index_build_scan(heap, index, indexInfo, true, true,
										   _bt_build_callback, buildstate,
										   NULL);
	else
		reltuples = _bt_parallel_heapscan(buildstate,
										  &indexInfo->ii_BrokenHotChain);

	/*
	 * Set the progress target for the next phase.  Reset the block number
	 * values set by table_index_build_scan
	 */
	{
		const int	progress_index[] = {
			PROGRESS_CREATEIDX_TUPLES_TOTAL,
			PROGRESS_SCAN_BLOCKS_TOTAL,
			PROGRESS_SCAN_BLOCKS_DONE
		};
		const int64 progress_vals[] = {
			buildstate->indtuples,
			0, 0
		};

		pgstat_progress_update_multi_param(3, progress_index, progress_vals);
	}

	/* okay, all heap tuples are spooled */
	if (buildstate->spool2 && !buildstate->havedead)
	{
		/* spool2 turns out to be unnecessary */
		_bt_spooldestroy(buildstate->spool2);
		buildstate->spool2 = NULL;
	}

	return reltuples;
}

/*
 * clean up a spool structure and its substructures.
 */
static void
_bt_spooldestroy(BTSpool *btspool)
{
	tuplesort_end(btspool->sortstate);
	pfree(btspool);
}

/*
 * spool an index entry into the sort file.
 */
static void
_bt_spool(BTSpool *btspool, ItemPointer self, Datum *values, bool *isnull)
{
	tuplesort_putindextuplevalues(btspool->sortstate, btspool->index,
								  self, values, isnull);
}

/*
 * given a spool loaded by successive calls to _bt_spool,
 * create an entire btree.
 */
static void
_bt_leafbuild(BTSpool *btspool, BTSpool *btspool2)
{
	BTWriteState wstate;

#ifdef BTREE_BUILD_STATS
	if (log_btree_build_stats)
	{
		ShowUsage("BTREE BUILD (Spool) STATISTICS");
		ResetUsage();
	}
#endif							/* BTREE_BUILD_STATS */

	/* Execute the sort */
	pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE,
								 PROGRESS_BTREE_PHASE_PERFORMSORT_1);
	tuplesort_performsort(btspool->sortstate);
	if (btspool2)
	{
		pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE,
									 PROGRESS_BTREE_PHASE_PERFORMSORT_2);
		tuplesort_performsort(btspool2->sortstate);
	}

	wstate.heap = btspool->heap;
	wstate.index = btspool->index;
	wstate.inskey = _bt_mkscankey(wstate.index, NULL);
	/* _bt_mkscankey() won't set allequalimage without metapage */
	wstate.inskey->allequalimage = _bt_allequalimage(wstate.index, true);

	/* reserve the metapage */
	wstate.btws_pages_alloced = BTREE_METAPAGE + 1;

	pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE,
								 PROGRESS_BTREE_PHASE_LEAF_LOAD);
	_bt_load(&wstate, btspool, btspool2);
}

/*
 * Per-tuple callback for table_index_build_scan
 */
static void
_bt_build_callback(Relation index,
				   ItemPointer tid,
				   Datum *values,
				   bool *isnull,
				   bool tupleIsAlive,
				   void *state)
{
	BTBuildState *buildstate = (BTBuildState *) state;

	/*
	 * insert the index tuple into the appropriate spool file for subsequent
	 * processing
	 */
	if (tupleIsAlive || buildstate->spool2 == NULL)
		_bt_spool(buildstate->spool, tid, values, isnull);
	else
	{
		/* dead tuples are put into spool2 */
		buildstate->havedead = true;
		_bt_spool(buildstate->spool2, tid, values, isnull);
	}

	buildstate->indtuples += 1;
}

/*
 * allocate workspace for a new, clean btree page, not linked to any siblings.
 */
static BulkWriteBuffer
_bt_blnewpage(BTWriteState *wstate, uint32 level)
{
	BulkWriteBuffer buf;
	Page		page;
	BTPageOpaque opaque;

	buf = smgr_bulk_get_buf(wstate->bulkstate);
	page = (Page) buf;

	/* Zero the page and set up standard page header info */
	_bt_pageinit(page, BLCKSZ);

	/* Initialize BT opaque state */
	opaque = BTPageGetOpaque(page);
	opaque->btpo_prev = opaque->btpo_next = P_NONE;
	opaque->btpo_level = level;
	opaque->btpo_flags = (level > 0) ? 0 : BTP_LEAF;
	opaque->btpo_cycleid = 0;

	/* Make the P_HIKEY line pointer appear allocated */
	((PageHeader) page)->pd_lower += sizeof(ItemIdData);

	return buf;
}

/*
 * emit a completed btree page, and release the working storage.
 */
static void
_bt_blwritepage(BTWriteState *wstate, BulkWriteBuffer buf, BlockNumber blkno)
{
	smgr_bulk_write(wstate->bulkstate, blkno, buf, true);
	/* smgr_bulk_write took ownership of 'buf' */
}

/*
 * allocate and initialize a new BTPageState.  the returned structure
 * is suitable for immediate use by _bt_buildadd.
 */
static BTPageState *
_bt_pagestate(BTWriteState *wstate, uint32 level)
{
	BTPageState *state = (BTPageState *) palloc0(sizeof(BTPageState));

	/* create initial page for level */
	state->btps_buf = _bt_blnewpage(wstate, level);

	/* and assign it a page position */
	state->btps_blkno = wstate->btws_pages_alloced++;

	state->btps_lowkey = NULL;
	/* initialize lastoff so first item goes into P_FIRSTKEY */
	state->btps_lastoff = P_HIKEY;
	state->btps_lastextra = 0;
	state->btps_level = level;
	/* set "full" threshold based on level.  See notes at head of file. */
	if (level > 0)
		state->btps_full = (BLCKSZ * (100 - BTREE_NONLEAF_FILLFACTOR) / 100);
	else
		state->btps_full = BTGetTargetPageFreeSpace(wstate->index);

	/* no parent level, yet */
	state->btps_next = NULL;

	return state;
}

/*
 * Slide the array of ItemIds from the page back one slot (from P_FIRSTKEY to
 * P_HIKEY, overwriting P_HIKEY).
 *
 * _bt_blnewpage() makes the P_HIKEY line pointer appear allocated, but the
 * rightmost page on its level is not supposed to get a high key.  Now that
 * it's clear that this page is a rightmost page, remove the unneeded empty
 * P_HIKEY line pointer space.
 */
static void
_bt_slideleft(Page rightmostpage)
{
	OffsetNumber off;
	OffsetNumber maxoff;
	ItemId		previi;

	maxoff = PageGetMaxOffsetNumber(rightmostpage);
	Assert(maxoff >= P_FIRSTKEY);
	previi = PageGetItemId(rightmostpage, P_HIKEY);
	for (off = P_FIRSTKEY; off <= maxoff; off = OffsetNumberNext(off))
	{
		ItemId		thisii = PageGetItemId(rightmostpage, off);

		*previi = *thisii;
		previi = thisii;
	}
	((PageHeader) rightmostpage)->pd_lower -= sizeof(ItemIdData);
}

/*
 * Add an item to a page being built.
 *
 * This is very similar to nbtinsert.c's _bt_pgaddtup(), but this variant
 * raises an error directly.
 *
 * Note that our nbtsort.c caller does not know yet if the page will be
 * rightmost.  Offset P_FIRSTKEY is always assumed to be the first data key by
 * caller.  Page that turns out to be the rightmost on its level is fixed by
 * calling _bt_slideleft().
 */
static void
_bt_sortaddtup(Page page,
			   Size itemsize,
			   IndexTuple itup,
			   OffsetNumber itup_off,
			   bool newfirstdataitem)
{
	IndexTupleData trunctuple;

	if (newfirstdataitem)
	{
		trunctuple = *itup;
		trunctuple.t_info = sizeof(IndexTupleData);
		BTreeTupleSetNAtts(&trunctuple, 0, false);
		itup = &trunctuple;
		itemsize = sizeof(IndexTupleData);
	}

	if (PageAddItem(page, (Item) itup, itemsize, itup_off,
					false, false) == InvalidOffsetNumber)
		elog(ERROR, "failed to add item to the index page");
}

/*----------
 * Add an item to a disk page from the sort output (or add a posting list
 * item formed from the sort output).
 *
 * We must be careful to observe the page layout conventions of nbtsearch.c:
 * - rightmost pages start data items at P_HIKEY instead of at P_FIRSTKEY.
 * - on non-leaf pages, the key portion of the first item need not be
 *	 stored, we should store only the link.
 *
 * A leaf page being built looks like:
 *
 * +----------------+---------------------------------+
 * | PageHeaderData | linp0 linp1 linp2 ...           |
 * +-----------+----+---------------------------------+
 * | ... linpN |									  |
 * +-----------+--------------------------------------+
 * |	 ^ last										  |
 * |												  |
 * +-------------+------------------------------------+
 * |			 | itemN ...                          |
 * +-------------+------------------+-----------------+
 * |		  ... item3 item2 item1 | "special space" |
 * +--------------------------------+-----------------+
 *
 * Contrast this with the diagram in bufpage.h; note the mismatch
 * between linps and items.  This is because we reserve linp0 as a
 * placeholder for the pointer to the "high key" item; when we have
 * filled up the page, we will set linp0 to point to itemN and clear
 * linpN.  On the other hand, if we find this is the last (rightmost)
 * page, we leave the items alone and slide the linp array over.  If
 * the high key is to be truncated, offset 1 is deleted, and we insert
 * the truncated high key at offset 1.
 *
 * 'last' pointer indicates the last offset added to the page.
 *
 * 'truncextra' is the size of the posting list in itup, if any.  This
 * information is stashed for the next call here, when we may benefit
 * from considering the impact of truncating away the posting list on
 * the page before deciding to finish the page off.  Posting lists are
 * often relatively large, so it is worth going to the trouble of
 * accounting for the saving from truncating away the posting list of
 * the tuple that becomes the high key (that may be the only way to
 * get close to target free space on the page).  Note that this is
 * only used for the soft fillfactor-wise limit, not the critical hard
 * limit.
 *----------
 */
static void
_bt_buildadd(BTWriteState *wstate, BTPageState *state, IndexTuple itup,
			 Size truncextra)
{
	BulkWriteBuffer nbuf;
	Page		npage;
	BlockNumber nblkno;
	OffsetNumber last_off;
	Size		last_truncextra;
	Size		pgspc;
	Size		itupsz;
	bool		isleaf;

	/*
	 * This is a handy place to check for cancel interrupts during the btree
	 * load phase of index creation.
	 */
	CHECK_FOR_INTERRUPTS();

	nbuf = state->btps_buf;
	npage = (Page) nbuf;
	nblkno = state->btps_blkno;
	last_off = state->btps_lastoff;
	last_truncextra = state->btps_lastextra;
	state->btps_lastextra = truncextra;

	pgspc = PageGetFreeSpace(npage);
	itupsz = IndexTupleSize(itup);
	itupsz = MAXALIGN(itupsz);
	/* Leaf case has slightly different rules due to suffix truncation */
	isleaf = (state->btps_level == 0);

	/*
	 * Check whether the new item can fit on a btree page on current level at
	 * all.
	 *
	 * Every newly built index will treat heap TID as part of the keyspace,
	 * which imposes the requirement that new high keys must occasionally have
	 * a heap TID appended within _bt_truncate().  That may leave a new pivot
	 * tuple one or two MAXALIGN() quantums larger than the original
	 * firstright tuple it's derived from.  v4 deals with the problem by
	 * decreasing the limit on the size of tuples inserted on the leaf level
	 * by the same small amount.  Enforce the new v4+ limit on the leaf level,
	 * and the old limit on internal levels, since pivot tuples may need to
	 * make use of the reserved space.  This should never fail on internal
	 * pages.
	 */
	if (unlikely(itupsz > BTMaxItemSize(npage)))
		_bt_check_third_page(wstate->index, wstate->heap, isleaf, npage,
							 itup);

	/*
	 * Check to see if current page will fit new item, with space left over to
	 * append a heap TID during suffix truncation when page is a leaf page.
	 *
	 * It is guaranteed that we can fit at least 2 non-pivot tuples plus a
	 * high key with heap TID when finishing off a leaf page, since we rely on
	 * _bt_check_third_page() rejecting oversized non-pivot tuples.  On
	 * internal pages we can always fit 3 pivot tuples with larger internal
	 * page tuple limit (includes page high key).
	 *
	 * Most of the time, a page is only "full" in the sense that the soft
	 * fillfactor-wise limit has been exceeded.  However, we must always leave
	 * at least two items plus a high key on each page before starting a new
	 * page.  Disregard fillfactor and insert on "full" current page if we
	 * don't have the minimum number of items yet.  (Note that we deliberately
	 * assume that suffix truncation neither enlarges nor shrinks new high key
	 * when applying soft limit, except when last tuple has a posting list.)
	 */
	Assert(last_truncextra == 0 || isleaf);
	if (pgspc < itupsz + (isleaf ? MAXALIGN(sizeof(ItemPointerData)) : 0) ||
		(pgspc + last_truncextra < state->btps_full && last_off > P_FIRSTKEY))
	{
		/*
		 * Finish off the page and write it out.
		 */
		BulkWriteBuffer obuf = nbuf;
		Page		opage = npage;
		BlockNumber oblkno = nblkno;
		ItemId		ii;
		ItemId		hii;
		IndexTuple	oitup;

		/* Create new page of same level */
		nbuf = _bt_blnewpage(wstate, state->btps_level);
		npage = (Page) nbuf;

		/* and assign it a page position */
		nblkno = wstate->btws_pages_alloced++;

		/*
		 * We copy the last item on the page into the new page, and then
		 * rearrange the old page so that the 'last item' becomes its high key
		 * rather than a true data item.  There had better be at least two
		 * items on the page already, else the page would be empty of useful
		 * data.
		 */
		Assert(last_off > P_FIRSTKEY);
		ii = PageGetItemId(opage, last_off);
		oitup = (IndexTuple) PageGetItem(opage, ii);
		_bt_sortaddtup(npage, ItemIdGetLength(ii), oitup, P_FIRSTKEY,
					   !isleaf);

		/*
		 * Move 'last' into the high key position on opage.  _bt_blnewpage()
		 * allocated empty space for a line pointer when opage was first
		 * created, so this is a matter of rearranging already-allocated space
		 * on page, and initializing high key line pointer. (Actually, leaf
		 * pages must also swap oitup with a truncated version of oitup, which
		 * is sometimes larger than oitup, though never by more than the space
		 * needed to append a heap TID.)
		 */
		hii = PageGetItemId(opage, P_HIKEY);
		*hii = *ii;
		ItemIdSetUnused(ii);	/* redundant */
		((PageHeader) opage)->pd_lower -= sizeof(ItemIdData);

		if (isleaf)
		{
			IndexTuple	lastleft;
			IndexTuple	truncated;

			/*
			 * Truncate away any unneeded attributes from high key on leaf
			 * level.  This is only done at the leaf level because downlinks
			 * in internal pages are either negative infinity items, or get
			 * their contents from copying from one level down.  See also:
			 * _bt_split().
			 *
			 * We don't try to bias our choice of split point to make it more
			 * likely that _bt_truncate() can truncate away more attributes,
			 * whereas the split point used within _bt_split() is chosen much
			 * more delicately.  Even still, the lastleft and firstright
			 * tuples passed to _bt_truncate() here are at least not fully
			 * equal to each other when deduplication is used, unless there is
			 * a large group of duplicates (also, unique index builds usually
			 * have few or no spool2 duplicates).  When the split point is
			 * between two unequal tuples, _bt_truncate() will avoid including
			 * a heap TID in the new high key, which is the most important
			 * benefit of suffix truncation.
			 *
			 * Overwrite the old item with new truncated high key directly.
			 * oitup is already located at the physical beginning of tuple
			 * space, so this should directly reuse the existing tuple space.
			 */
			ii = PageGetItemId(opage, OffsetNumberPrev(last_off));
			lastleft = (IndexTuple) PageGetItem(opage, ii);

			Assert(IndexTupleSize(oitup) > last_truncextra);
			truncated = _bt_truncate(wstate->index, lastleft, oitup,
									 wstate->inskey);
			if (!PageIndexTupleOverwrite(opage, P_HIKEY, (Item) truncated,
										 IndexTupleSize(truncated)))
				elog(ERROR, "failed to add high key to the index page");
			pfree(truncated);

			/* oitup should continue to point to the page's high key */
			hii = PageGetItemId(opage, P_HIKEY);
			oitup = (IndexTuple) PageGetItem(opage, hii);
		}

		/*
		 * Link the old page into its parent, using its low key.  If we don't
		 * have a parent, we have to create one; this adds a new btree level.
		 */
		if (state->btps_next == NULL)
			state->btps_next = _bt_pagestate(wstate, state->btps_level + 1);

		Assert((BTreeTupleGetNAtts(state->btps_lowkey, wstate->index) <=
				IndexRelationGetNumberOfKeyAttributes(wstate->index) &&
				BTreeTupleGetNAtts(state->btps_lowkey, wstate->index) > 0) ||
			   P_LEFTMOST(BTPageGetOpaque(opage)));
		Assert(BTreeTupleGetNAtts(state->btps_lowkey, wstate->index) == 0 ||
			   !P_LEFTMOST(BTPageGetOpaque(opage)));
		BTreeTupleSetDownLink(state->btps_lowkey, oblkno);
		_bt_buildadd(wstate, state->btps_next, state->btps_lowkey, 0);
		pfree(state->btps_lowkey);

		/*
		 * Save a copy of the high key from the old page.  It is also the low
		 * key for the new page.
		 */
		state->btps_lowkey = CopyIndexTuple(oitup);

		/*
		 * Set the sibling links for both pages.
		 */
		{
			BTPageOpaque oopaque = BTPageGetOpaque(opage);
			BTPageOpaque nopaque = BTPageGetOpaque(npage);

			oopaque->btpo_next = nblkno;
			nopaque->btpo_prev = oblkno;
			nopaque->btpo_next = P_NONE;	/* redundant */
		}

		/*
		 * Write out the old page. _bt_blwritepage takes ownership of the
		 * 'opage' buffer.
		 */
		_bt_blwritepage(wstate, obuf, oblkno);

		/*
		 * Reset last_off to point to new page
		 */
		last_off = P_FIRSTKEY;
	}

	/*
	 * By here, either original page is still the current page, or a new page
	 * was created that became the current page.  Either way, the current page
	 * definitely has space for new item.
	 *
	 * If the new item is the first for its page, it must also be the first
	 * item on its entire level.  On later same-level pages, a low key for a
	 * page will be copied from the prior page in the code above.  Generate a
	 * minus infinity low key here instead.
	 */
	if (last_off == P_HIKEY)
	{
		Assert(state->btps_lowkey == NULL);
		state->btps_lowkey = palloc0(sizeof(IndexTupleData));
		state->btps_lowkey->t_info = sizeof(IndexTupleData);
		BTreeTupleSetNAtts(state->btps_lowkey, 0, false);
	}

	/*
	 * Add the new item into the current page.
	 */
	last_off = OffsetNumberNext(last_off);
	_bt_sortaddtup(npage, itupsz, itup, last_off,
				   !isleaf && last_off == P_FIRSTKEY);

	state->btps_buf = nbuf;
	state->btps_blkno = nblkno;
	state->btps_lastoff = last_off;
}

/*
 * Finalize pending posting list tuple, and add it to the index.  Final tuple
 * is based on saved base tuple, and saved list of heap TIDs.
 *
 * This is almost like _bt_dedup_finish_pending(), but it adds a new tuple
 * using _bt_buildadd().
 */
static void
_bt_sort_dedup_finish_pending(BTWriteState *wstate, BTPageState *state,
							  BTDedupState dstate)
{
	Assert(dstate->nitems > 0);

	if (dstate->nitems == 1)
		_bt_buildadd(wstate, state, dstate->base, 0);
	else
	{
		IndexTuple	postingtuple;
		Size		truncextra;

		/* form a tuple with a posting list */
		postingtuple = _bt_form_posting(dstate->base,
										dstate->htids,
										dstate->nhtids);
		/* Calculate posting list overhead */
		truncextra = IndexTupleSize(postingtuple) -
			BTreeTupleGetPostingOffset(postingtuple);

		_bt_buildadd(wstate, state, postingtuple, truncextra);
		pfree(postingtuple);
	}

	dstate->nmaxitems = 0;
	dstate->nhtids = 0;
	dstate->nitems = 0;
	dstate->phystupsize = 0;
}

/*
 * Finish writing out the completed btree.
 */
static void
_bt_uppershutdown(BTWriteState *wstate, BTPageState *state)
{
	BTPageState *s;
	BlockNumber rootblkno = P_NONE;
	uint32		rootlevel = 0;
	BulkWriteBuffer metabuf;

	/*
	 * Each iteration of this loop completes one more level of the tree.
	 */
	for (s = state; s != NULL; s = s->btps_next)
	{
		BlockNumber blkno;
		BTPageOpaque opaque;

		blkno = s->btps_blkno;
		opaque = BTPageGetOpaque((Page) s->btps_buf);

		/*
		 * We have to link the last page on this level to somewhere.
		 *
		 * If we're at the top, it's the root, so attach it to the metapage.
		 * Otherwise, add an entry for it to its parent using its low key.
		 * This may cause the last page of the parent level to split, but
		 * that's not a problem -- we haven't gotten to it yet.
		 */
		if (s->btps_next == NULL)
		{
			opaque->btpo_flags |= BTP_ROOT;
			rootblkno = blkno;
			rootlevel = s->btps_level;
		}
		else
		{
			Assert((BTreeTupleGetNAtts(s->btps_lowkey, wstate->index) <=
					IndexRelationGetNumberOfKeyAttributes(wstate->index) &&
					BTreeTupleGetNAtts(s->btps_lowkey, wstate->index) > 0) ||
				   P_LEFTMOST(opaque));
			Assert(BTreeTupleGetNAtts(s->btps_lowkey, wstate->index) == 0 ||
				   !P_LEFTMOST(opaque));
			BTreeTupleSetDownLink(s->btps_lowkey, blkno);
			_bt_buildadd(wstate, s->btps_next, s->btps_lowkey, 0);
			pfree(s->btps_lowkey);
			s->btps_lowkey = NULL;
		}

		/*
		 * This is the rightmost page, so the ItemId array needs to be slid
		 * back one slot.  Then we can dump out the page.
		 */
		_bt_slideleft((Page) s->btps_buf);
		_bt_blwritepage(wstate, s->btps_buf, s->btps_blkno);
		s->btps_buf = NULL;		/* writepage took ownership of the buffer */
	}

	/*
	 * As the last step in the process, construct the metapage and make it
	 * point to the new root (unless we had no data at all, in which case it's
	 * set to point to "P_NONE").  This changes the index to the "valid" state
	 * by filling in a valid magic number in the metapage.
	 */
	metabuf = smgr_bulk_get_buf(wstate->bulkstate);
	_bt_initmetapage((Page) metabuf, rootblkno, rootlevel,
					 wstate->inskey->allequalimage);
	_bt_blwritepage(wstate, metabuf, BTREE_METAPAGE);
}

/*
 * Read tuples in correct sort order from tuplesort, and load them into
 * btree leaves.
 */
static void
_bt_load(BTWriteState *wstate, BTSpool *btspool, BTSpool *btspool2)
{
	BTPageState *state = NULL;
	bool		merge = (btspool2 != NULL);
	IndexTuple	itup,
				itup2 = NULL;
	bool		load1;
	TupleDesc	tupdes = RelationGetDescr(wstate->index);
	int			i,
				keysz = IndexRelationGetNumberOfKeyAttributes(wstate->index);
	SortSupport sortKeys;
	int64		tuples_done = 0;
	bool		deduplicate;

	wstate->bulkstate = smgr_bulk_start_rel(wstate->index, MAIN_FORKNUM);

	deduplicate = wstate->inskey->allequalimage && !btspool->isunique &&
		BTGetDeduplicateItems(wstate->index);

	if (merge)
	{
		/*
		 * Another BTSpool for dead tuples exists. Now we have to merge
		 * btspool and btspool2.
		 */

		/* the preparation of merge */
		itup = tuplesort_getindextuple(btspool->sortstate, true);
		itup2 = tuplesort_getindextuple(btspool2->sortstate, true);

		/* Prepare SortSupport data for each column */
		sortKeys = (SortSupport) palloc0(keysz * sizeof(SortSupportData));

		for (i = 0; i < keysz; i++)
		{
			SortSupport sortKey = sortKeys + i;
			ScanKey		scanKey = wstate->inskey->scankeys + i;
			int16		strategy;

			sortKey->ssup_cxt = CurrentMemoryContext;
			sortKey->ssup_collation = scanKey->sk_collation;
			sortKey->ssup_nulls_first =
				(scanKey->sk_flags & SK_BT_NULLS_FIRST) != 0;
			sortKey->ssup_attno = scanKey->sk_attno;
			/* Abbreviation is not supported here */
			sortKey->abbreviate = false;

			Assert(sortKey->ssup_attno != 0);

			strategy = (scanKey->sk_flags & SK_BT_DESC) != 0 ?
				BTGreaterStrategyNumber : BTLessStrategyNumber;

			PrepareSortSupportFromIndexRel(wstate->index, strategy, sortKey);
		}

		for (;;)
		{
			load1 = true;		/* load BTSpool next ? */
			if (itup2 == NULL)
			{
				if (itup == NULL)
					break;
			}
			else if (itup != NULL)
			{
				int32		compare = 0;

				for (i = 1; i <= keysz; i++)
				{
					SortSupport entry;
					Datum		attrDatum1,
								attrDatum2;
					bool		isNull1,
								isNull2;

					entry = sortKeys + i - 1;
					attrDatum1 = index_getattr(itup, i, tupdes, &isNull1);
					attrDatum2 = index_getattr(itup2, i, tupdes, &isNull2);

					compare = ApplySortComparator(attrDatum1, isNull1,
												  attrDatum2, isNull2,
												  entry);
					if (compare > 0)
					{
						load1 = false;
						break;
					}
					else if (compare < 0)
						break;
				}

				/*
				 * If key values are equal, we sort on ItemPointer.  This is
				 * required for btree indexes, since heap TID is treated as an
				 * implicit last key attribute in order to ensure that all
				 * keys in the index are physically unique.
				 */
				if (compare == 0)
				{
					compare = ItemPointerCompare(&itup->t_tid, &itup2->t_tid);
					Assert(compare != 0);
					if (compare > 0)
						load1 = false;
				}
			}
			else
				load1 = false;

			/* When we see first tuple, create first index page */
			if (state == NULL)
				state = _bt_pagestate(wstate, 0);

			if (load1)
			{
				_bt_buildadd(wstate, state, itup, 0);
				itup = tuplesort_getindextuple(btspool->sortstate, true);
			}
			else
			{
				_bt_buildadd(wstate, state, itup2, 0);
				itup2 = tuplesort_getindextuple(btspool2->sortstate, true);
			}

			/* Report progress */
			pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE,
										 ++tuples_done);
		}
		pfree(sortKeys);
	}
	else if (deduplicate)
	{
		/* merge is unnecessary, deduplicate into posting lists */
		BTDedupState dstate;

		dstate = (BTDedupState) palloc(sizeof(BTDedupStateData));
		dstate->deduplicate = true; /* unused */
		dstate->nmaxitems = 0;	/* unused */
		dstate->maxpostingsize = 0; /* set later */
		/* Metadata about base tuple of current pending posting list */
		dstate->base = NULL;
		dstate->baseoff = InvalidOffsetNumber;	/* unused */
		dstate->basetupsize = 0;
		/* Metadata about current pending posting list TIDs */
		dstate->htids = NULL;
		dstate->nhtids = 0;
		dstate->nitems = 0;
		dstate->phystupsize = 0;	/* unused */
		dstate->nintervals = 0; /* unused */

		while ((itup = tuplesort_getindextuple(btspool->sortstate,
											   true)) != NULL)
		{
			/* When we see first tuple, create first index page */
			if (state == NULL)
			{
				state = _bt_pagestate(wstate, 0);

				/*
				 * Limit size of posting list tuples to 1/10 space we want to
				 * leave behind on the page, plus space for final item's line
				 * pointer.  This is equal to the space that we'd like to
				 * leave behind on each leaf page when fillfactor is 90,
				 * allowing us to get close to fillfactor% space utilization
				 * when there happen to be a great many duplicates.  (This
				 * makes higher leaf fillfactor settings ineffective when
				 * building indexes that have many duplicates, but packing
				 * leaf pages full with few very large tuples doesn't seem
				 * like a useful goal.)
				 */
				dstate->maxpostingsize = MAXALIGN_DOWN((BLCKSZ * 10 / 100)) -
					sizeof(ItemIdData);
				Assert(dstate->maxpostingsize <= BTMaxItemSize((Page) state->btps_buf) &&
					   dstate->maxpostingsize <= INDEX_SIZE_MASK);
				dstate->htids = palloc(dstate->maxpostingsize);

				/* start new pending posting list with itup copy */
				_bt_dedup_start_pending(dstate, CopyIndexTuple(itup),
										InvalidOffsetNumber);
			}
			else if (_bt_keep_natts_fast(wstate->index, dstate->base,
										 itup) > keysz &&
					 _bt_dedup_save_htid(dstate, itup))
			{
				/*
				 * Tuple is equal to base tuple of pending posting list.  Heap
				 * TID from itup has been saved in state.
				 */
			}
			else
			{
				/*
				 * Tuple is not equal to pending posting list tuple, or
				 * _bt_dedup_save_htid() opted to not merge current item into
				 * pending posting list.
				 */
				_bt_sort_dedup_finish_pending(wstate, state, dstate);
				pfree(dstate->base);

				/* start new pending posting list with itup copy */
				_bt_dedup_start_pending(dstate, CopyIndexTuple(itup),
										InvalidOffsetNumber);
			}

			/* Report progress */
			pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE,
										 ++tuples_done);
		}

		if (state)
		{
			/*
			 * Handle the last item (there must be a last item when the
			 * tuplesort returned one or more tuples)
			 */
			_bt_sort_dedup_finish_pending(wstate, state, dstate);
			pfree(dstate->base);
			pfree(dstate->htids);
		}

		pfree(dstate);
	}
	else
	{
		/* merging and deduplication are both unnecessary */
		while ((itup = tuplesort_getindextuple(btspool->sortstate,
											   true)) != NULL)
		{
			/* When we see first tuple, create first index page */
			if (state == NULL)
				state = _bt_pagestate(wstate, 0);

			_bt_buildadd(wstate, state, itup, 0);

			/* Report progress */
			pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE,
										 ++tuples_done);
		}
	}

	/* Close down final pages and write the metapage */
	_bt_uppershutdown(wstate, state);
	smgr_bulk_finish(wstate->bulkstate);
}

/*
 * Create parallel context, and launch workers for leader.
 *
 * buildstate argument should be initialized (with the exception of the
 * tuplesort state in spools, which may later be created based on shared
 * state initially set up here).
 *
 * isconcurrent indicates if operation is CREATE INDEX CONCURRENTLY.
 *
 * request is the target number of parallel worker processes to launch.
 *
 * Sets buildstate's BTLeader, which caller must use to shut down parallel
 * mode by passing it to _bt_end_parallel() at the very end of its index
 * build.  If not even a single worker process can be launched, this is
 * never set, and caller should proceed with a serial index build.
 */
static void
_bt_begin_parallel(BTBuildState *buildstate, bool isconcurrent, int request)
{
	ParallelContext *pcxt;
	int			scantuplesortstates;
	Snapshot	snapshot;
	Size		estbtshared;
	Size		estsort;
	BTShared   *btshared;
	Sharedsort *sharedsort;
	Sharedsort *sharedsort2;
	BTSpool    *btspool = buildstate->spool;
	BTLeader   *btleader = (BTLeader *) palloc0(sizeof(BTLeader));
	WalUsage   *walusage;
	BufferUsage *bufferusage;
	bool		leaderparticipates = true;
	int			querylen;

#ifdef DISABLE_LEADER_PARTICIPATION
	leaderparticipates = false;
#endif

	/*
	 * Enter parallel mode, and create context for parallel build of btree
	 * index
	 */
	EnterParallelMode();
	Assert(request > 0);
	pcxt = CreateParallelContext("postgres", "_bt_parallel_build_main",
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
	 * Estimate size for our own PARALLEL_KEY_BTREE_SHARED workspace, and
	 * PARALLEL_KEY_TUPLESORT tuplesort workspace
	 */
	estbtshared = _bt_parallel_estimate_shared(btspool->heap, snapshot);
	shm_toc_estimate_chunk(&pcxt->estimator, estbtshared);
	estsort = tuplesort_estimate_shared(scantuplesortstates);
	shm_toc_estimate_chunk(&pcxt->estimator, estsort);

	/*
	 * Unique case requires a second spool, and so we may have to account for
	 * another shared workspace for that -- PARALLEL_KEY_TUPLESORT_SPOOL2
	 */
	if (!btspool->isunique)
		shm_toc_estimate_keys(&pcxt->estimator, 2);
	else
	{
		shm_toc_estimate_chunk(&pcxt->estimator, estsort);
		shm_toc_estimate_keys(&pcxt->estimator, 3);
	}

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
	btshared = (BTShared *) shm_toc_allocate(pcxt->toc, estbtshared);
	/* Initialize immutable state */
	btshared->heaprelid = RelationGetRelid(btspool->heap);
	btshared->indexrelid = RelationGetRelid(btspool->index);
	btshared->isunique = btspool->isunique;
	btshared->nulls_not_distinct = btspool->nulls_not_distinct;
	btshared->isconcurrent = isconcurrent;
	btshared->scantuplesortstates = scantuplesortstates;
	btshared->queryid = pgstat_get_my_query_id();
	ConditionVariableInit(&btshared->workersdonecv);
	SpinLockInit(&btshared->mutex);
	/* Initialize mutable state */
	btshared->nparticipantsdone = 0;
	btshared->reltuples = 0.0;
	btshared->havedead = false;
	btshared->indtuples = 0.0;
	btshared->brokenhotchain = false;
	table_parallelscan_initialize(btspool->heap,
								  ParallelTableScanFromBTShared(btshared),
								  snapshot);

	/*
	 * Store shared tuplesort-private state, for which we reserved space.
	 * Then, initialize opaque state using tuplesort routine.
	 */
	sharedsort = (Sharedsort *) shm_toc_allocate(pcxt->toc, estsort);
	tuplesort_initialize_shared(sharedsort, scantuplesortstates,
								pcxt->seg);

	shm_toc_insert(pcxt->toc, PARALLEL_KEY_BTREE_SHARED, btshared);
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_TUPLESORT, sharedsort);

	/* Unique case requires a second spool, and associated shared state */
	if (!btspool->isunique)
		sharedsort2 = NULL;
	else
	{
		/*
		 * Store additional shared tuplesort-private state, for which we
		 * reserved space.  Then, initialize opaque state using tuplesort
		 * routine.
		 */
		sharedsort2 = (Sharedsort *) shm_toc_allocate(pcxt->toc, estsort);
		tuplesort_initialize_shared(sharedsort2, scantuplesortstates,
									pcxt->seg);

		shm_toc_insert(pcxt->toc, PARALLEL_KEY_TUPLESORT_SPOOL2, sharedsort2);
	}

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
	btleader->pcxt = pcxt;
	btleader->nparticipanttuplesorts = pcxt->nworkers_launched;
	if (leaderparticipates)
		btleader->nparticipanttuplesorts++;
	btleader->btshared = btshared;
	btleader->sharedsort = sharedsort;
	btleader->sharedsort2 = sharedsort2;
	btleader->snapshot = snapshot;
	btleader->walusage = walusage;
	btleader->bufferusage = bufferusage;

	/* If no workers were successfully launched, back out (do serial build) */
	if (pcxt->nworkers_launched == 0)
	{
		_bt_end_parallel(btleader);
		return;
	}

	/* Save leader state now that it's clear build will be parallel */
	buildstate->btleader = btleader;

	/* Join heap scan ourselves */
	if (leaderparticipates)
		_bt_leader_participate_as_worker(buildstate);

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
_bt_end_parallel(BTLeader *btleader)
{
	int			i;

	/* Shutdown worker processes */
	WaitForParallelWorkersToFinish(btleader->pcxt);

	/*
	 * Next, accumulate WAL usage.  (This must wait for the workers to finish,
	 * or we might get incomplete data.)
	 */
	for (i = 0; i < btleader->pcxt->nworkers_launched; i++)
		InstrAccumParallelQuery(&btleader->bufferusage[i], &btleader->walusage[i]);

	/* Free last reference to MVCC snapshot, if one was used */
	if (IsMVCCSnapshot(btleader->snapshot))
		UnregisterSnapshot(btleader->snapshot);
	DestroyParallelContext(btleader->pcxt);
	ExitParallelMode();
}

/*
 * Returns size of shared memory required to store state for a parallel
 * btree index build based on the snapshot its parallel scan will use.
 */
static Size
_bt_parallel_estimate_shared(Relation heap, Snapshot snapshot)
{
	/* c.f. shm_toc_allocate as to why BUFFERALIGN is used */
	return add_size(BUFFERALIGN(sizeof(BTShared)),
					table_parallelscan_estimate(heap, snapshot));
}

/*
 * Within leader, wait for end of heap scan.
 *
 * When called, parallel heap scan started by _bt_begin_parallel() will
 * already be underway within worker processes (when leader participates
 * as a worker, we should end up here just as workers are finishing).
 *
 * Fills in fields needed for ambuild statistics, and lets caller set
 * field indicating that some worker encountered a broken HOT chain.
 *
 * Returns the total number of heap tuples scanned.
 */
static double
_bt_parallel_heapscan(BTBuildState *buildstate, bool *brokenhotchain)
{
	BTShared   *btshared = buildstate->btleader->btshared;
	int			nparticipanttuplesorts;
	double		reltuples;

	nparticipanttuplesorts = buildstate->btleader->nparticipanttuplesorts;
	for (;;)
	{
		SpinLockAcquire(&btshared->mutex);
		if (btshared->nparticipantsdone == nparticipanttuplesorts)
		{
			buildstate->havedead = btshared->havedead;
			buildstate->indtuples = btshared->indtuples;
			*brokenhotchain = btshared->brokenhotchain;
			reltuples = btshared->reltuples;
			SpinLockRelease(&btshared->mutex);
			break;
		}
		SpinLockRelease(&btshared->mutex);

		ConditionVariableSleep(&btshared->workersdonecv,
							   WAIT_EVENT_PARALLEL_CREATE_INDEX_SCAN);
	}

	ConditionVariableCancelSleep();

	return reltuples;
}

/*
 * Within leader, participate as a parallel worker.
 */
static void
_bt_leader_participate_as_worker(BTBuildState *buildstate)
{
	BTLeader   *btleader = buildstate->btleader;
	BTSpool    *leaderworker;
	BTSpool    *leaderworker2;
	int			sortmem;

	/* Allocate memory and initialize private spool */
	leaderworker = (BTSpool *) palloc0(sizeof(BTSpool));
	leaderworker->heap = buildstate->spool->heap;
	leaderworker->index = buildstate->spool->index;
	leaderworker->isunique = buildstate->spool->isunique;
	leaderworker->nulls_not_distinct = buildstate->spool->nulls_not_distinct;

	/* Initialize second spool, if required */
	if (!btleader->btshared->isunique)
		leaderworker2 = NULL;
	else
	{
		/* Allocate memory for worker's own private secondary spool */
		leaderworker2 = (BTSpool *) palloc0(sizeof(BTSpool));

		/* Initialize worker's own secondary spool */
		leaderworker2->heap = leaderworker->heap;
		leaderworker2->index = leaderworker->index;
		leaderworker2->isunique = false;
	}

	/*
	 * Might as well use reliable figure when doling out maintenance_work_mem
	 * (when requested number of workers were not launched, this will be
	 * somewhat higher than it is for other workers).
	 */
	sortmem = maintenance_work_mem / btleader->nparticipanttuplesorts;

	/* Perform work common to all participants */
	_bt_parallel_scan_and_sort(leaderworker, leaderworker2, btleader->btshared,
							   btleader->sharedsort, btleader->sharedsort2,
							   sortmem, true);

#ifdef BTREE_BUILD_STATS
	if (log_btree_build_stats)
	{
		ShowUsage("BTREE BUILD (Leader Partial Spool) STATISTICS");
		ResetUsage();
	}
#endif							/* BTREE_BUILD_STATS */
}

/*
 * Perform work within a launched parallel process.
 */
void
_bt_parallel_build_main(dsm_segment *seg, shm_toc *toc)
{
	char	   *sharedquery;
	BTSpool    *btspool;
	BTSpool    *btspool2;
	BTShared   *btshared;
	Sharedsort *sharedsort;
	Sharedsort *sharedsort2;
	Relation	heapRel;
	Relation	indexRel;
	LOCKMODE	heapLockmode;
	LOCKMODE	indexLockmode;
	WalUsage   *walusage;
	BufferUsage *bufferusage;
	int			sortmem;

#ifdef BTREE_BUILD_STATS
	if (log_btree_build_stats)
		ResetUsage();
#endif							/* BTREE_BUILD_STATS */

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

	/* Look up nbtree shared state */
	btshared = shm_toc_lookup(toc, PARALLEL_KEY_BTREE_SHARED, false);

	/* Open relations using lock modes known to be obtained by index.c */
	if (!btshared->isconcurrent)
	{
		heapLockmode = ShareLock;
		indexLockmode = AccessExclusiveLock;
	}
	else
	{
		heapLockmode = ShareUpdateExclusiveLock;
		indexLockmode = RowExclusiveLock;
	}

	/* Track query ID */
	pgstat_report_query_id(btshared->queryid, false);

	/* Open relations within worker */
	heapRel = table_open(btshared->heaprelid, heapLockmode);
	indexRel = index_open(btshared->indexrelid, indexLockmode);

	/* Initialize worker's own spool */
	btspool = (BTSpool *) palloc0(sizeof(BTSpool));
	btspool->heap = heapRel;
	btspool->index = indexRel;
	btspool->isunique = btshared->isunique;
	btspool->nulls_not_distinct = btshared->nulls_not_distinct;

	/* Look up shared state private to tuplesort.c */
	sharedsort = shm_toc_lookup(toc, PARALLEL_KEY_TUPLESORT, false);
	tuplesort_attach_shared(sharedsort, seg);
	if (!btshared->isunique)
	{
		btspool2 = NULL;
		sharedsort2 = NULL;
	}
	else
	{
		/* Allocate memory for worker's own private secondary spool */
		btspool2 = (BTSpool *) palloc0(sizeof(BTSpool));

		/* Initialize worker's own secondary spool */
		btspool2->heap = btspool->heap;
		btspool2->index = btspool->index;
		btspool2->isunique = false;
		/* Look up shared state private to tuplesort.c */
		sharedsort2 = shm_toc_lookup(toc, PARALLEL_KEY_TUPLESORT_SPOOL2, false);
		tuplesort_attach_shared(sharedsort2, seg);
	}

	/* Prepare to track buffer usage during parallel execution */
	InstrStartParallelQuery();

	/* Perform sorting of spool, and possibly a spool2 */
	sortmem = maintenance_work_mem / btshared->scantuplesortstates;
	_bt_parallel_scan_and_sort(btspool, btspool2, btshared, sharedsort,
							   sharedsort2, sortmem, false);

	/* Report WAL/buffer usage during parallel execution */
	bufferusage = shm_toc_lookup(toc, PARALLEL_KEY_BUFFER_USAGE, false);
	walusage = shm_toc_lookup(toc, PARALLEL_KEY_WAL_USAGE, false);
	InstrEndParallelQuery(&bufferusage[ParallelWorkerNumber],
						  &walusage[ParallelWorkerNumber]);

#ifdef BTREE_BUILD_STATS
	if (log_btree_build_stats)
	{
		ShowUsage("BTREE BUILD (Worker Partial Spool) STATISTICS");
		ResetUsage();
	}
#endif							/* BTREE_BUILD_STATS */

	index_close(indexRel, indexLockmode);
	table_close(heapRel, heapLockmode);
}

/*
 * Perform a worker's portion of a parallel sort.
 *
 * This generates a tuplesort for passed btspool, and a second tuplesort
 * state if a second btspool is need (i.e. for unique index builds).  All
 * other spool fields should already be set when this is called.
 *
 * sortmem is the amount of working memory to use within each worker,
 * expressed in KBs.
 *
 * When this returns, workers are done, and need only release resources.
 */
static void
_bt_parallel_scan_and_sort(BTSpool *btspool, BTSpool *btspool2,
						   BTShared *btshared, Sharedsort *sharedsort,
						   Sharedsort *sharedsort2, int sortmem, bool progress)
{
	SortCoordinate coordinate;
	BTBuildState buildstate;
	TableScanDesc scan;
	double		reltuples;
	IndexInfo  *indexInfo;

	/* Initialize local tuplesort coordination state */
	coordinate = palloc0(sizeof(SortCoordinateData));
	coordinate->isWorker = true;
	coordinate->nParticipants = -1;
	coordinate->sharedsort = sharedsort;

	/* Begin "partial" tuplesort */
	btspool->sortstate = tuplesort_begin_index_btree(btspool->heap,
													 btspool->index,
													 btspool->isunique,
													 btspool->nulls_not_distinct,
													 sortmem, coordinate,
													 TUPLESORT_NONE);

	/*
	 * Just as with serial case, there may be a second spool.  If so, a
	 * second, dedicated spool2 partial tuplesort is required.
	 */
	if (btspool2)
	{
		SortCoordinate coordinate2;

		/*
		 * We expect that the second one (for dead tuples) won't get very
		 * full, so we give it only work_mem (unless sortmem is less for
		 * worker).  Worker processes are generally permitted to allocate
		 * work_mem independently.
		 */
		coordinate2 = palloc0(sizeof(SortCoordinateData));
		coordinate2->isWorker = true;
		coordinate2->nParticipants = -1;
		coordinate2->sharedsort = sharedsort2;
		btspool2->sortstate =
			tuplesort_begin_index_btree(btspool->heap, btspool->index, false, false,
										Min(sortmem, work_mem), coordinate2,
										false);
	}

	/* Fill in buildstate for _bt_build_callback() */
	buildstate.isunique = btshared->isunique;
	buildstate.nulls_not_distinct = btshared->nulls_not_distinct;
	buildstate.havedead = false;
	buildstate.heap = btspool->heap;
	buildstate.spool = btspool;
	buildstate.spool2 = btspool2;
	buildstate.indtuples = 0;
	buildstate.btleader = NULL;

	/* Join parallel scan */
	indexInfo = BuildIndexInfo(btspool->index);
	indexInfo->ii_Concurrent = btshared->isconcurrent;
	scan = table_beginscan_parallel(btspool->heap,
									ParallelTableScanFromBTShared(btshared));
	reltuples = table_index_build_scan(btspool->heap, btspool->index, indexInfo,
									   true, progress, _bt_build_callback,
									   &buildstate, scan);

	/* Execute this worker's part of the sort */
	if (progress)
		pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE,
									 PROGRESS_BTREE_PHASE_PERFORMSORT_1);
	tuplesort_performsort(btspool->sortstate);
	if (btspool2)
	{
		if (progress)
			pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE,
										 PROGRESS_BTREE_PHASE_PERFORMSORT_2);
		tuplesort_performsort(btspool2->sortstate);
	}

	/*
	 * Done.  Record ambuild statistics, and whether we encountered a broken
	 * HOT chain.
	 */
	SpinLockAcquire(&btshared->mutex);
	btshared->nparticipantsdone++;
	btshared->reltuples += reltuples;
	if (buildstate.havedead)
		btshared->havedead = true;
	btshared->indtuples += buildstate.indtuples;
	if (indexInfo->ii_BrokenHotChain)
		btshared->brokenhotchain = true;
	SpinLockRelease(&btshared->mutex);

	/* Notify leader */
	ConditionVariableSignal(&btshared->workersdonecv);

	/* We can end tuplesorts immediately */
	tuplesort_end(btspool->sortstate);
	if (btspool2)
		tuplesort_end(btspool2->sortstate);
}
