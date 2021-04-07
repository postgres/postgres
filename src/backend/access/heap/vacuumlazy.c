/*-------------------------------------------------------------------------
 *
 * vacuumlazy.c
 *	  Concurrent ("lazy") vacuuming.
 *
 *
 * The major space usage for LAZY VACUUM is storage for the array of dead tuple
 * TIDs.  We want to ensure we can vacuum even the very largest relations with
 * finite memory space usage.  To do that, we set upper bounds on the number of
 * tuples we will keep track of at once.
 *
 * We are willing to use at most maintenance_work_mem (or perhaps
 * autovacuum_work_mem) memory space to keep track of dead tuples.  We
 * initially allocate an array of TIDs of that size, with an upper limit that
 * depends on table size (this limit ensures we don't allocate a huge area
 * uselessly for vacuuming small tables).  If the array threatens to overflow,
 * we suspend the heap scan phase and perform a pass of index cleanup and page
 * compaction, then resume the heap scan with an empty TID array.
 *
 * If we're processing a table with no indexes, we can just vacuum each page
 * as we go; there's no need to save up multiple tuples to minimize the number
 * of index scans performed.  So we don't use maintenance_work_mem memory for
 * the TID array, just enough to hold as many heap tuples as fit on one page.
 *
 * Lazy vacuum supports parallel execution with parallel worker processes.  In
 * a parallel vacuum, we perform both index vacuum and index cleanup with
 * parallel worker processes.  Individual indexes are processed by one vacuum
 * process.  At the beginning of a lazy vacuum (at lazy_scan_heap) we prepare
 * the parallel context and initialize the DSM segment that contains shared
 * information as well as the memory space for storing dead tuples.  When
 * starting either index vacuum or index cleanup, we launch parallel worker
 * processes.  Once all indexes are processed the parallel worker processes
 * exit.  After that, the leader process re-initializes the parallel context
 * so that it can use the same DSM for multiple passes of index vacuum and
 * for performing index cleanup.  For updating the index statistics, we need
 * to update the system table and since updates are not allowed during
 * parallel mode we update the index statistics after exiting from the
 * parallel mode.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/vacuumlazy.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/amapi.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/heapam_xlog.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/parallel.h"
#include "access/transam.h"
#include "access/visibilitymap.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/index.h"
#include "catalog/storage.h"
#include "commands/dbcommands.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "optimizer/paths.h"
#include "pgstat.h"
#include "portability/instr_time.h"
#include "postmaster/autovacuum.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/lmgr.h"
#include "tcop/tcopprot.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_rusage.h"
#include "utils/timestamp.h"


/*
 * Space/time tradeoff parameters: do these need to be user-tunable?
 *
 * To consider truncating the relation, we want there to be at least
 * REL_TRUNCATE_MINIMUM or (relsize / REL_TRUNCATE_FRACTION) (whichever
 * is less) potentially-freeable pages.
 */
#define REL_TRUNCATE_MINIMUM	1000
#define REL_TRUNCATE_FRACTION	16

/*
 * Timing parameters for truncate locking heuristics.
 *
 * These were not exposed as user tunable GUC values because it didn't seem
 * that the potential for improvement was great enough to merit the cost of
 * supporting them.
 */
#define VACUUM_TRUNCATE_LOCK_CHECK_INTERVAL		20	/* ms */
#define VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL		50	/* ms */
#define VACUUM_TRUNCATE_LOCK_TIMEOUT			5000	/* ms */

/*
 * When a table is small (i.e. smaller than this), save cycles by avoiding
 * repeated failsafe checks
 */
#define FAILSAFE_MIN_PAGES \
	((BlockNumber) (((uint64) 4 * 1024 * 1024 * 1024) / BLCKSZ))

/*
 * When a table has no indexes, vacuum the FSM after every 8GB, approximately
 * (it won't be exact because we only vacuum FSM after processing a heap page
 * that has some removable tuples).  When there are indexes, this is ignored,
 * and we vacuum FSM after each index/heap cleaning pass.
 */
#define VACUUM_FSM_EVERY_PAGES \
	((BlockNumber) (((uint64) 8 * 1024 * 1024 * 1024) / BLCKSZ))

/*
 * Guesstimation of number of dead tuples per page.  This is used to
 * provide an upper limit to memory allocated when vacuuming small
 * tables.
 */
#define LAZY_ALLOC_TUPLES		MaxHeapTuplesPerPage

/*
 * Before we consider skipping a page that's marked as clean in
 * visibility map, we must've seen at least this many clean pages.
 */
#define SKIP_PAGES_THRESHOLD	((BlockNumber) 32)

/*
 * Size of the prefetch window for lazy vacuum backwards truncation scan.
 * Needs to be a power of 2.
 */
#define PREFETCH_SIZE			((BlockNumber) 32)

/*
 * DSM keys for parallel vacuum.  Unlike other parallel execution code, since
 * we don't need to worry about DSM keys conflicting with plan_node_id we can
 * use small integers.
 */
#define PARALLEL_VACUUM_KEY_SHARED			1
#define PARALLEL_VACUUM_KEY_DEAD_TUPLES		2
#define PARALLEL_VACUUM_KEY_QUERY_TEXT		3
#define PARALLEL_VACUUM_KEY_BUFFER_USAGE	4
#define PARALLEL_VACUUM_KEY_WAL_USAGE		5

/*
 * Macro to check if we are in a parallel vacuum.  If true, we are in the
 * parallel mode and the DSM segment is initialized.
 */
#define ParallelVacuumIsActive(vacrel) ((vacrel)->lps != NULL)

/* Phases of vacuum during which we report error context. */
typedef enum
{
	VACUUM_ERRCB_PHASE_UNKNOWN,
	VACUUM_ERRCB_PHASE_SCAN_HEAP,
	VACUUM_ERRCB_PHASE_VACUUM_INDEX,
	VACUUM_ERRCB_PHASE_VACUUM_HEAP,
	VACUUM_ERRCB_PHASE_INDEX_CLEANUP,
	VACUUM_ERRCB_PHASE_TRUNCATE
} VacErrPhase;

/*
 * LVDeadTuples stores the dead tuple TIDs collected during the heap scan.
 * This is allocated in the DSM segment in parallel mode and in local memory
 * in non-parallel mode.
 */
typedef struct LVDeadTuples
{
	int			max_tuples;		/* # slots allocated in array */
	int			num_tuples;		/* current # of entries */
	/* List of TIDs of tuples we intend to delete */
	/* NB: this list is ordered by TID address */
	ItemPointerData itemptrs[FLEXIBLE_ARRAY_MEMBER];	/* array of
														 * ItemPointerData */
} LVDeadTuples;

/* The dead tuple space consists of LVDeadTuples and dead tuple TIDs */
#define SizeOfDeadTuples(cnt) \
	add_size(offsetof(LVDeadTuples, itemptrs), \
			 mul_size(sizeof(ItemPointerData), cnt))
#define MAXDEADTUPLES(max_size) \
		(((max_size) - offsetof(LVDeadTuples, itemptrs)) / sizeof(ItemPointerData))

/*
 * Shared information among parallel workers.  So this is allocated in the DSM
 * segment.
 */
typedef struct LVShared
{
	/*
	 * Target table relid and log level.  These fields are not modified during
	 * the lazy vacuum.
	 */
	Oid			relid;
	int			elevel;

	/*
	 * An indication for vacuum workers to perform either index vacuum or
	 * index cleanup.  first_time is true only if for_cleanup is true and
	 * bulk-deletion is not performed yet.
	 */
	bool		for_cleanup;
	bool		first_time;

	/*
	 * Fields for both index vacuum and cleanup.
	 *
	 * reltuples is the total number of input heap tuples.  We set either old
	 * live tuples in the index vacuum case or the new live tuples in the
	 * index cleanup case.
	 *
	 * estimated_count is true if reltuples is an estimated value.  (Note that
	 * reltuples could be -1 in this case, indicating we have no idea.)
	 */
	double		reltuples;
	bool		estimated_count;

	/*
	 * In single process lazy vacuum we could consume more memory during index
	 * vacuuming or cleanup apart from the memory for heap scanning.  In
	 * parallel vacuum, since individual vacuum workers can consume memory
	 * equal to maintenance_work_mem, the new maintenance_work_mem for each
	 * worker is set such that the parallel operation doesn't consume more
	 * memory than single process lazy vacuum.
	 */
	int			maintenance_work_mem_worker;

	/*
	 * Shared vacuum cost balance.  During parallel vacuum,
	 * VacuumSharedCostBalance points to this value and it accumulates the
	 * balance of each parallel vacuum worker.
	 */
	pg_atomic_uint32 cost_balance;

	/*
	 * Number of active parallel workers.  This is used for computing the
	 * minimum threshold of the vacuum cost balance before a worker sleeps for
	 * cost-based delay.
	 */
	pg_atomic_uint32 active_nworkers;

	/*
	 * Variables to control parallel vacuum.  We have a bitmap to indicate
	 * which index has stats in shared memory.  The set bit in the map
	 * indicates that the particular index supports a parallel vacuum.
	 */
	pg_atomic_uint32 idx;		/* counter for vacuuming and clean up */
	uint32		offset;			/* sizeof header incl. bitmap */
	bits8		bitmap[FLEXIBLE_ARRAY_MEMBER];	/* bit map of NULLs */

	/* Shared index statistics data follows at end of struct */
} LVShared;

#define SizeOfLVShared (offsetof(LVShared, bitmap) + sizeof(bits8))
#define GetSharedIndStats(s) \
	((LVSharedIndStats *)((char *)(s) + ((LVShared *)(s))->offset))
#define IndStatsIsNull(s, i) \
	(!(((LVShared *)(s))->bitmap[(i) >> 3] & (1 << ((i) & 0x07))))

/*
 * Struct for an index bulk-deletion statistic used for parallel vacuum.  This
 * is allocated in the DSM segment.
 */
typedef struct LVSharedIndStats
{
	bool		updated;		/* are the stats updated? */
	IndexBulkDeleteResult istat;
} LVSharedIndStats;

/* Struct for maintaining a parallel vacuum state. */
typedef struct LVParallelState
{
	ParallelContext *pcxt;

	/* Shared information among parallel vacuum workers */
	LVShared   *lvshared;

	/* Points to buffer usage area in DSM */
	BufferUsage *buffer_usage;

	/* Points to WAL usage area in DSM */
	WalUsage   *wal_usage;

	/*
	 * The number of indexes that support parallel index bulk-deletion and
	 * parallel index cleanup respectively.
	 */
	int			nindexes_parallel_bulkdel;
	int			nindexes_parallel_cleanup;
	int			nindexes_parallel_condcleanup;
} LVParallelState;

typedef struct LVRelState
{
	/* Target heap relation and its indexes */
	Relation	rel;
	Relation   *indrels;
	int			nindexes;
	/* Do index vacuuming/cleanup? */
	bool		do_index_vacuuming;
	bool		do_index_cleanup;
	/* Wraparound failsafe in effect? (implies !do_index_vacuuming) */
	bool		do_failsafe;

	/* Buffer access strategy and parallel state */
	BufferAccessStrategy bstrategy;
	LVParallelState *lps;

	/* Statistics from pg_class when we start out */
	BlockNumber old_rel_pages;	/* previous value of pg_class.relpages */
	double		old_live_tuples;	/* previous value of pg_class.reltuples */
	/* rel's initial relfrozenxid and relminmxid */
	TransactionId relfrozenxid;
	MultiXactId relminmxid;

	/* VACUUM operation's cutoff for pruning */
	TransactionId OldestXmin;
	/* VACUUM operation's cutoff for freezing XIDs and MultiXactIds */
	TransactionId FreezeLimit;
	MultiXactId MultiXactCutoff;

	/* Error reporting state */
	char	   *relnamespace;
	char	   *relname;
	char	   *indname;
	BlockNumber blkno;			/* used only for heap operations */
	OffsetNumber offnum;		/* used only for heap operations */
	VacErrPhase phase;

	/*
	 * State managed by lazy_scan_heap() follows
	 */
	LVDeadTuples *dead_tuples;	/* items to vacuum from indexes */
	BlockNumber rel_pages;		/* total number of pages */
	BlockNumber scanned_pages;	/* number of pages we examined */
	BlockNumber pinskipped_pages;	/* # of pages skipped due to a pin */
	BlockNumber frozenskipped_pages;	/* # of frozen pages we skipped */
	BlockNumber tupcount_pages; /* pages whose tuples we counted */
	BlockNumber pages_removed;	/* pages remove by truncation */
	BlockNumber lpdead_item_pages;	/* # pages with LP_DEAD items */
	BlockNumber nonempty_pages; /* actually, last nonempty page + 1 */
	bool		lock_waiter_detected;

	/* Statistics output by us, for table */
	double		new_rel_tuples; /* new estimated total # of tuples */
	double		new_live_tuples;	/* new estimated total # of live tuples */
	/* Statistics output by index AMs */
	IndexBulkDeleteResult **indstats;

	/* Instrumentation counters */
	int			num_index_scans;
	int64		tuples_deleted; /* # deleted from table */
	int64		lpdead_items;	/* # deleted from indexes */
	int64		new_dead_tuples;	/* new estimated total # of dead items in
									 * table */
	int64		num_tuples;		/* total number of nonremovable tuples */
	int64		live_tuples;	/* live tuples (reltuples estimate) */
} LVRelState;

/*
 * State returned by lazy_scan_prune()
 */
typedef struct LVPagePruneState
{
	bool		hastup;			/* Page is truncatable? */
	bool		has_lpdead_items;	/* includes existing LP_DEAD items */

	/*
	 * State describes the proper VM bit states to set for the page following
	 * pruning and freezing.  all_visible implies !has_lpdead_items, but don't
	 * trust all_frozen result unless all_visible is also set to true.
	 */
	bool		all_visible;	/* Every item visible to all? */
	bool		all_frozen;		/* provided all_visible is also true */
	TransactionId visibility_cutoff_xid;	/* For recovery conflicts */
} LVPagePruneState;

/* Struct for saving and restoring vacuum error information. */
typedef struct LVSavedErrInfo
{
	BlockNumber blkno;
	OffsetNumber offnum;
	VacErrPhase phase;
} LVSavedErrInfo;

/* elevel controls whole VACUUM's verbosity */
static int	elevel = -1;


/* non-export function prototypes */
static void lazy_scan_heap(LVRelState *vacrel, VacuumParams *params,
						   bool aggressive);
static void lazy_scan_prune(LVRelState *vacrel, Buffer buf,
							BlockNumber blkno, Page page,
							GlobalVisState *vistest,
							LVPagePruneState *prunestate);
static void lazy_vacuum(LVRelState *vacrel);
static bool lazy_vacuum_all_indexes(LVRelState *vacrel);
static void lazy_vacuum_heap_rel(LVRelState *vacrel);
static int	lazy_vacuum_heap_page(LVRelState *vacrel, BlockNumber blkno,
								  Buffer buffer, int tupindex, Buffer *vmbuffer);
static bool lazy_check_needs_freeze(Buffer buf, bool *hastup,
									LVRelState *vacrel);
static bool lazy_check_wraparound_failsafe(LVRelState *vacrel);
static void do_parallel_lazy_vacuum_all_indexes(LVRelState *vacrel);
static void do_parallel_lazy_cleanup_all_indexes(LVRelState *vacrel);
static void do_parallel_vacuum_or_cleanup(LVRelState *vacrel, int nworkers);
static void do_parallel_processing(LVRelState *vacrel,
								   LVShared *lvshared);
static void do_serial_processing_for_unsafe_indexes(LVRelState *vacrel,
													LVShared *lvshared);
static IndexBulkDeleteResult *parallel_process_one_index(Relation indrel,
														 IndexBulkDeleteResult *istat,
														 LVShared *lvshared,
														 LVSharedIndStats *shared_indstats,
														 LVRelState *vacrel);
static void lazy_cleanup_all_indexes(LVRelState *vacrel);
static IndexBulkDeleteResult *lazy_vacuum_one_index(Relation indrel,
													IndexBulkDeleteResult *istat,
													double reltuples,
													LVRelState *vacrel);
static IndexBulkDeleteResult *lazy_cleanup_one_index(Relation indrel,
													 IndexBulkDeleteResult *istat,
													 double reltuples,
													 bool estimated_count,
													 LVRelState *vacrel);
static bool should_attempt_truncation(LVRelState *vacrel,
									  VacuumParams *params);
static void lazy_truncate_heap(LVRelState *vacrel);
static BlockNumber count_nondeletable_pages(LVRelState *vacrel);
static long compute_max_dead_tuples(BlockNumber relblocks, bool hasindex);
static void lazy_space_alloc(LVRelState *vacrel, int nworkers,
							 BlockNumber relblocks);
static void lazy_space_free(LVRelState *vacrel);
static bool lazy_tid_reaped(ItemPointer itemptr, void *state);
static int	vac_cmp_itemptr(const void *left, const void *right);
static bool heap_page_is_all_visible(LVRelState *vacrel, Buffer buf,
									 TransactionId *visibility_cutoff_xid, bool *all_frozen);
static int	compute_parallel_vacuum_workers(LVRelState *vacrel,
											int nrequested,
											bool *can_parallel_vacuum);
static void update_index_statistics(LVRelState *vacrel);
static LVParallelState *begin_parallel_vacuum(LVRelState *vacrel,
											  BlockNumber nblocks,
											  int nrequested);
static void end_parallel_vacuum(LVRelState *vacrel);
static LVSharedIndStats *parallel_stats_for_idx(LVShared *lvshared, int getidx);
static bool parallel_processing_is_safe(Relation indrel, LVShared *lvshared);
static void vacuum_error_callback(void *arg);
static void update_vacuum_error_info(LVRelState *vacrel,
									 LVSavedErrInfo *saved_vacrel,
									 int phase, BlockNumber blkno,
									 OffsetNumber offnum);
static void restore_vacuum_error_info(LVRelState *vacrel,
									  const LVSavedErrInfo *saved_vacrel);


/*
 *	heap_vacuum_rel() -- perform VACUUM for one heap relation
 *
 *		This routine vacuums a single heap, cleans out its indexes, and
 *		updates its relpages and reltuples statistics.
 *
 *		At entry, we have already established a transaction and opened
 *		and locked the relation.
 */
void
heap_vacuum_rel(Relation rel, VacuumParams *params,
				BufferAccessStrategy bstrategy)
{
	LVRelState *vacrel;
	PGRUsage	ru0;
	TimestampTz starttime = 0;
	WalUsage	walusage_start = pgWalUsage;
	WalUsage	walusage = {0, 0, 0};
	long		secs;
	int			usecs;
	double		read_rate,
				write_rate;
	bool		aggressive;		/* should we scan all unfrozen pages? */
	bool		scanned_all_unfrozen;	/* actually scanned all such pages? */
	char	  **indnames = NULL;
	TransactionId xidFullScanLimit;
	MultiXactId mxactFullScanLimit;
	BlockNumber new_rel_pages;
	BlockNumber new_rel_allvisible;
	double		new_live_tuples;
	TransactionId new_frozen_xid;
	MultiXactId new_min_multi;
	ErrorContextCallback errcallback;
	PgStat_Counter startreadtime = 0;
	PgStat_Counter startwritetime = 0;
	TransactionId OldestXmin;
	TransactionId FreezeLimit;
	MultiXactId MultiXactCutoff;

	Assert(params != NULL);
	Assert(params->index_cleanup != VACOPT_TERNARY_DEFAULT);
	Assert(params->truncate != VACOPT_TERNARY_DEFAULT);

	/* measure elapsed time iff autovacuum logging requires it */
	if (IsAutoVacuumWorkerProcess() && params->log_min_duration >= 0)
	{
		pg_rusage_init(&ru0);
		starttime = GetCurrentTimestamp();
		if (track_io_timing)
		{
			startreadtime = pgStatBlockReadTime;
			startwritetime = pgStatBlockWriteTime;
		}
	}

	if (params->options & VACOPT_VERBOSE)
		elevel = INFO;
	else
		elevel = DEBUG2;

	pgstat_progress_start_command(PROGRESS_COMMAND_VACUUM,
								  RelationGetRelid(rel));

	vacuum_set_xid_limits(rel,
						  params->freeze_min_age,
						  params->freeze_table_age,
						  params->multixact_freeze_min_age,
						  params->multixact_freeze_table_age,
						  &OldestXmin, &FreezeLimit, &xidFullScanLimit,
						  &MultiXactCutoff, &mxactFullScanLimit);

	/*
	 * We request an aggressive scan if the table's frozen Xid is now older
	 * than or equal to the requested Xid full-table scan limit; or if the
	 * table's minimum MultiXactId is older than or equal to the requested
	 * mxid full-table scan limit; or if DISABLE_PAGE_SKIPPING was specified.
	 */
	aggressive = TransactionIdPrecedesOrEquals(rel->rd_rel->relfrozenxid,
											   xidFullScanLimit);
	aggressive |= MultiXactIdPrecedesOrEquals(rel->rd_rel->relminmxid,
											  mxactFullScanLimit);
	if (params->options & VACOPT_DISABLE_PAGE_SKIPPING)
		aggressive = true;

	vacrel = (LVRelState *) palloc0(sizeof(LVRelState));

	/* Set up high level stuff about rel */
	vacrel->rel = rel;
	vac_open_indexes(vacrel->rel, RowExclusiveLock, &vacrel->nindexes,
					 &vacrel->indrels);
	vacrel->do_index_vacuuming = true;
	vacrel->do_index_cleanup = true;
	vacrel->do_failsafe = false;
	if (params->index_cleanup == VACOPT_TERNARY_DISABLED)
	{
		vacrel->do_index_vacuuming = false;
		vacrel->do_index_cleanup = false;
	}
	vacrel->bstrategy = bstrategy;
	vacrel->old_rel_pages = rel->rd_rel->relpages;
	vacrel->old_live_tuples = rel->rd_rel->reltuples;
	vacrel->relfrozenxid = rel->rd_rel->relfrozenxid;
	vacrel->relminmxid = rel->rd_rel->relminmxid;

	/* Set cutoffs for entire VACUUM */
	vacrel->OldestXmin = OldestXmin;
	vacrel->FreezeLimit = FreezeLimit;
	vacrel->MultiXactCutoff = MultiXactCutoff;

	vacrel->relnamespace = get_namespace_name(RelationGetNamespace(rel));
	vacrel->relname = pstrdup(RelationGetRelationName(rel));
	vacrel->indname = NULL;
	vacrel->phase = VACUUM_ERRCB_PHASE_UNKNOWN;

	/* Save index names iff autovacuum logging requires it */
	if (IsAutoVacuumWorkerProcess() && params->log_min_duration >= 0 &&
		vacrel->nindexes > 0)
	{
		indnames = palloc(sizeof(char *) * vacrel->nindexes);
		for (int i = 0; i < vacrel->nindexes; i++)
			indnames[i] =
				pstrdup(RelationGetRelationName(vacrel->indrels[i]));
	}

	/*
	 * Setup error traceback support for ereport().  The idea is to set up an
	 * error context callback to display additional information on any error
	 * during a vacuum.  During different phases of vacuum (heap scan, heap
	 * vacuum, index vacuum, index clean up, heap truncate), we update the
	 * error context callback to display appropriate information.
	 *
	 * Note that the index vacuum and heap vacuum phases may be called
	 * multiple times in the middle of the heap scan phase.  So the old phase
	 * information is restored at the end of those phases.
	 */
	errcallback.callback = vacuum_error_callback;
	errcallback.arg = vacrel;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* Do the vacuuming */
	lazy_scan_heap(vacrel, params, aggressive);

	/* Done with indexes */
	vac_close_indexes(vacrel->nindexes, vacrel->indrels, NoLock);

	/*
	 * Compute whether we actually scanned the all unfrozen pages. If we did,
	 * we can adjust relfrozenxid and relminmxid.
	 *
	 * NB: We need to check this before truncating the relation, because that
	 * will change ->rel_pages.
	 */
	if ((vacrel->scanned_pages + vacrel->frozenskipped_pages)
		< vacrel->rel_pages)
	{
		Assert(!aggressive);
		scanned_all_unfrozen = false;
	}
	else
		scanned_all_unfrozen = true;

	/*
	 * Optionally truncate the relation.
	 */
	if (should_attempt_truncation(vacrel, params))
	{
		/*
		 * Update error traceback information.  This is the last phase during
		 * which we add context information to errors, so we don't need to
		 * revert to the previous phase.
		 */
		update_vacuum_error_info(vacrel, NULL, VACUUM_ERRCB_PHASE_TRUNCATE,
								 vacrel->nonempty_pages,
								 InvalidOffsetNumber);
		lazy_truncate_heap(vacrel);
	}

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;

	/* Report that we are now doing final cleanup */
	pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
								 PROGRESS_VACUUM_PHASE_FINAL_CLEANUP);

	/*
	 * Update statistics in pg_class.
	 *
	 * In principle new_live_tuples could be -1 indicating that we (still)
	 * don't know the tuple count.  In practice that probably can't happen,
	 * since we'd surely have scanned some pages if the table is new and
	 * nonempty.
	 *
	 * For safety, clamp relallvisible to be not more than what we're setting
	 * relpages to.
	 *
	 * Also, don't change relfrozenxid/relminmxid if we skipped any pages,
	 * since then we don't know for certain that all tuples have a newer xmin.
	 */
	new_rel_pages = vacrel->rel_pages;
	new_live_tuples = vacrel->new_live_tuples;

	visibilitymap_count(rel, &new_rel_allvisible, NULL);
	if (new_rel_allvisible > new_rel_pages)
		new_rel_allvisible = new_rel_pages;

	new_frozen_xid = scanned_all_unfrozen ? FreezeLimit : InvalidTransactionId;
	new_min_multi = scanned_all_unfrozen ? MultiXactCutoff : InvalidMultiXactId;

	vac_update_relstats(rel,
						new_rel_pages,
						new_live_tuples,
						new_rel_allvisible,
						vacrel->nindexes > 0,
						new_frozen_xid,
						new_min_multi,
						false);

	/* report results to the stats collector, too */
	pgstat_report_vacuum(RelationGetRelid(rel),
						 rel->rd_rel->relisshared,
						 Max(new_live_tuples, 0),
						 vacrel->new_dead_tuples);
	pgstat_progress_end_command();

	/* and log the action if appropriate */
	if (IsAutoVacuumWorkerProcess() && params->log_min_duration >= 0)
	{
		TimestampTz endtime = GetCurrentTimestamp();

		if (params->log_min_duration == 0 ||
			TimestampDifferenceExceeds(starttime, endtime,
									   params->log_min_duration))
		{
			StringInfoData buf;
			char	   *msgfmt;

			TimestampDifference(starttime, endtime, &secs, &usecs);

			memset(&walusage, 0, sizeof(WalUsage));
			WalUsageAccumDiff(&walusage, &pgWalUsage, &walusage_start);

			read_rate = 0;
			write_rate = 0;
			if ((secs > 0) || (usecs > 0))
			{
				read_rate = (double) BLCKSZ * VacuumPageMiss / (1024 * 1024) /
					(secs + usecs / 1000000.0);
				write_rate = (double) BLCKSZ * VacuumPageDirty / (1024 * 1024) /
					(secs + usecs / 1000000.0);
			}

			/*
			 * This is pretty messy, but we split it up so that we can skip
			 * emitting individual parts of the message when not applicable.
			 */
			initStringInfo(&buf);
			if (params->is_wraparound)
			{
				/*
				 * While it's possible for a VACUUM to be both is_wraparound
				 * and !aggressive, that's just a corner-case -- is_wraparound
				 * implies aggressive.  Produce distinct output for the corner
				 * case all the same, just in case.
				 */
				if (aggressive)
					msgfmt = _("automatic aggressive vacuum to prevent wraparound of table \"%s.%s.%s\": index scans: %d\n");
				else
					msgfmt = _("automatic vacuum to prevent wraparound of table \"%s.%s.%s\": index scans: %d\n");
			}
			else
			{
				if (aggressive)
					msgfmt = _("automatic aggressive vacuum of table \"%s.%s.%s\": index scans: %d\n");
				else
					msgfmt = _("automatic vacuum of table \"%s.%s.%s\": index scans: %d\n");
			}
			appendStringInfo(&buf, msgfmt,
							 get_database_name(MyDatabaseId),
							 vacrel->relnamespace,
							 vacrel->relname,
							 vacrel->num_index_scans);
			appendStringInfo(&buf, _("pages: %u removed, %u remain, %u skipped due to pins, %u skipped frozen\n"),
							 vacrel->pages_removed,
							 vacrel->rel_pages,
							 vacrel->pinskipped_pages,
							 vacrel->frozenskipped_pages);
			appendStringInfo(&buf,
							 _("tuples: %lld removed, %lld remain, %lld are dead but not yet removable, oldest xmin: %u\n"),
							 (long long) vacrel->tuples_deleted,
							 (long long) vacrel->new_rel_tuples,
							 (long long) vacrel->new_dead_tuples,
							 OldestXmin);
			appendStringInfo(&buf,
							 _("buffer usage: %lld hits, %lld misses, %lld dirtied\n"),
							 (long long) VacuumPageHit,
							 (long long) VacuumPageMiss,
							 (long long) VacuumPageDirty);
			for (int i = 0; i < vacrel->nindexes; i++)
			{
				IndexBulkDeleteResult *istat = vacrel->indstats[i];

				if (!istat)
					continue;

				appendStringInfo(&buf,
								 _("index \"%s\": pages: %u in total, %u newly deleted, %u currently deleted, %u reusable\n"),
								 indnames[i],
								 istat->num_pages,
								 istat->pages_newly_deleted,
								 istat->pages_deleted,
								 istat->pages_free);
			}
			appendStringInfo(&buf, _("avg read rate: %.3f MB/s, avg write rate: %.3f MB/s\n"),
							 read_rate, write_rate);
			if (track_io_timing)
			{
				appendStringInfoString(&buf, _("I/O Timings:"));
				if (pgStatBlockReadTime - startreadtime > 0)
					appendStringInfo(&buf, _(" read=%.3f"),
									 (double) (pgStatBlockReadTime - startreadtime) / 1000);
				if (pgStatBlockWriteTime - startwritetime > 0)
					appendStringInfo(&buf, _(" write=%.3f"),
									 (double) (pgStatBlockWriteTime - startwritetime) / 1000);
				appendStringInfoChar(&buf, '\n');
			}
			appendStringInfo(&buf, _("system usage: %s\n"), pg_rusage_show(&ru0));
			appendStringInfo(&buf,
							 _("WAL usage: %ld records, %ld full page images, %llu bytes"),
							 walusage.wal_records,
							 walusage.wal_fpi,
							 (unsigned long long) walusage.wal_bytes);

			ereport(LOG,
					(errmsg_internal("%s", buf.data)));
			pfree(buf.data);
		}
	}

	/* Cleanup index statistics and index names */
	for (int i = 0; i < vacrel->nindexes; i++)
	{
		if (vacrel->indstats[i])
			pfree(vacrel->indstats[i]);

		if (indnames && indnames[i])
			pfree(indnames[i]);
	}
}

/*
 *	lazy_scan_heap() -- scan an open heap relation
 *
 *		This routine prunes each page in the heap, which will among other
 *		things truncate dead tuples to dead line pointers, defragment the
 *		page, and set commit status bits (see heap_page_prune).  It also builds
 *		lists of dead tuples and pages with free space, calculates statistics
 *		on the number of live tuples in the heap, and marks pages as
 *		all-visible if appropriate.  When done, or when we run low on space
 *		for dead-tuple TIDs, invoke lazy_vacuum to vacuum indexes and vacuum
 *		heap relation during its own second pass over the heap.
 *
 *		If the table has at least two indexes, we execute both index vacuum
 *		and index cleanup with parallel workers unless parallel vacuum is
 *		disabled.  In a parallel vacuum, we enter parallel mode and then
 *		create both the parallel context and the DSM segment before starting
 *		heap scan so that we can record dead tuples to the DSM segment.  All
 *		parallel workers are launched at beginning of index vacuuming and
 *		index cleanup and they exit once done with all indexes.  At the end of
 *		this function we exit from parallel mode.  Index bulk-deletion results
 *		are stored in the DSM segment and we update index statistics for all
 *		the indexes after exiting from parallel mode since writes are not
 *		allowed during parallel mode.
 *
 *		If there are no indexes then we can reclaim line pointers on the fly;
 *		dead line pointers need only be retained until all index pointers that
 *		reference them have been killed.
 */
static void
lazy_scan_heap(LVRelState *vacrel, VacuumParams *params, bool aggressive)
{
	LVDeadTuples *dead_tuples;
	BlockNumber nblocks,
				blkno,
				next_unskippable_block,
				next_fsm_block_to_vacuum;
	PGRUsage	ru0;
	Buffer		vmbuffer = InvalidBuffer;
	bool		skipping_blocks;
	StringInfoData buf;
	const int	initprog_index[] = {
		PROGRESS_VACUUM_PHASE,
		PROGRESS_VACUUM_TOTAL_HEAP_BLKS,
		PROGRESS_VACUUM_MAX_DEAD_TUPLES
	};
	int64		initprog_val[3];
	GlobalVisState *vistest;

	pg_rusage_init(&ru0);

	if (aggressive)
		ereport(elevel,
				(errmsg("aggressively vacuuming \"%s.%s\"",
						vacrel->relnamespace,
						vacrel->relname)));
	else
		ereport(elevel,
				(errmsg("vacuuming \"%s.%s\"",
						vacrel->relnamespace,
						vacrel->relname)));

	nblocks = RelationGetNumberOfBlocks(vacrel->rel);
	next_unskippable_block = 0;
	next_fsm_block_to_vacuum = 0;
	vacrel->rel_pages = nblocks;
	vacrel->scanned_pages = 0;
	vacrel->pinskipped_pages = 0;
	vacrel->frozenskipped_pages = 0;
	vacrel->tupcount_pages = 0;
	vacrel->pages_removed = 0;
	vacrel->lpdead_item_pages = 0;
	vacrel->nonempty_pages = 0;
	vacrel->lock_waiter_detected = false;

	/* Initialize instrumentation counters */
	vacrel->num_index_scans = 0;
	vacrel->tuples_deleted = 0;
	vacrel->lpdead_items = 0;
	vacrel->new_dead_tuples = 0;
	vacrel->num_tuples = 0;
	vacrel->live_tuples = 0;

	vistest = GlobalVisTestFor(vacrel->rel);

	vacrel->indstats = (IndexBulkDeleteResult **)
		palloc0(vacrel->nindexes * sizeof(IndexBulkDeleteResult *));

	/*
	 * Before beginning scan, check if it's already necessary to apply
	 * failsafe
	 */
	lazy_check_wraparound_failsafe(vacrel);

	/*
	 * Allocate the space for dead tuples.  Note that this handles parallel
	 * VACUUM initialization as part of allocating shared memory space used
	 * for dead_tuples.
	 */
	lazy_space_alloc(vacrel, params->nworkers, nblocks);
	dead_tuples = vacrel->dead_tuples;

	/* Report that we're scanning the heap, advertising total # of blocks */
	initprog_val[0] = PROGRESS_VACUUM_PHASE_SCAN_HEAP;
	initprog_val[1] = nblocks;
	initprog_val[2] = dead_tuples->max_tuples;
	pgstat_progress_update_multi_param(3, initprog_index, initprog_val);

	/*
	 * Except when aggressive is set, we want to skip pages that are
	 * all-visible according to the visibility map, but only when we can skip
	 * at least SKIP_PAGES_THRESHOLD consecutive pages.  Since we're reading
	 * sequentially, the OS should be doing readahead for us, so there's no
	 * gain in skipping a page now and then; that's likely to disable
	 * readahead and so be counterproductive. Also, skipping even a single
	 * page means that we can't update relfrozenxid, so we only want to do it
	 * if we can skip a goodly number of pages.
	 *
	 * When aggressive is set, we can't skip pages just because they are
	 * all-visible, but we can still skip pages that are all-frozen, since
	 * such pages do not need freezing and do not affect the value that we can
	 * safely set for relfrozenxid or relminmxid.
	 *
	 * Before entering the main loop, establish the invariant that
	 * next_unskippable_block is the next block number >= blkno that we can't
	 * skip based on the visibility map, either all-visible for a regular scan
	 * or all-frozen for an aggressive scan.  We set it to nblocks if there's
	 * no such block.  We also set up the skipping_blocks flag correctly at
	 * this stage.
	 *
	 * Note: The value returned by visibilitymap_get_status could be slightly
	 * out-of-date, since we make this test before reading the corresponding
	 * heap page or locking the buffer.  This is OK.  If we mistakenly think
	 * that the page is all-visible or all-frozen when in fact the flag's just
	 * been cleared, we might fail to vacuum the page.  It's easy to see that
	 * skipping a page when aggressive is not set is not a very big deal; we
	 * might leave some dead tuples lying around, but the next vacuum will
	 * find them.  But even when aggressive *is* set, it's still OK if we miss
	 * a page whose all-frozen marking has just been cleared.  Any new XIDs
	 * just added to that page are necessarily newer than the GlobalXmin we
	 * computed, so they'll have no effect on the value to which we can safely
	 * set relfrozenxid.  A similar argument applies for MXIDs and relminmxid.
	 *
	 * We will scan the table's last page, at least to the extent of
	 * determining whether it has tuples or not, even if it should be skipped
	 * according to the above rules; except when we've already determined that
	 * it's not worth trying to truncate the table.  This avoids having
	 * lazy_truncate_heap() take access-exclusive lock on the table to attempt
	 * a truncation that just fails immediately because there are tuples in
	 * the last page.  This is worth avoiding mainly because such a lock must
	 * be replayed on any hot standby, where it can be disruptive.
	 */
	if ((params->options & VACOPT_DISABLE_PAGE_SKIPPING) == 0)
	{
		while (next_unskippable_block < nblocks)
		{
			uint8		vmstatus;

			vmstatus = visibilitymap_get_status(vacrel->rel,
												next_unskippable_block,
												&vmbuffer);
			if (aggressive)
			{
				if ((vmstatus & VISIBILITYMAP_ALL_FROZEN) == 0)
					break;
			}
			else
			{
				if ((vmstatus & VISIBILITYMAP_ALL_VISIBLE) == 0)
					break;
			}
			vacuum_delay_point();
			next_unskippable_block++;
		}
	}

	if (next_unskippable_block >= SKIP_PAGES_THRESHOLD)
		skipping_blocks = true;
	else
		skipping_blocks = false;

	for (blkno = 0; blkno < nblocks; blkno++)
	{
		Buffer		buf;
		Page		page;
		bool		all_visible_according_to_vm = false;
		LVPagePruneState prunestate;

		/*
		 * Consider need to skip blocks.  See note above about forcing
		 * scanning of last page.
		 */
#define FORCE_CHECK_PAGE() \
		(blkno == nblocks - 1 && should_attempt_truncation(vacrel, params))

		pgstat_progress_update_param(PROGRESS_VACUUM_HEAP_BLKS_SCANNED, blkno);

		update_vacuum_error_info(vacrel, NULL, VACUUM_ERRCB_PHASE_SCAN_HEAP,
								 blkno, InvalidOffsetNumber);

		if (blkno == next_unskippable_block)
		{
			/* Time to advance next_unskippable_block */
			next_unskippable_block++;
			if ((params->options & VACOPT_DISABLE_PAGE_SKIPPING) == 0)
			{
				while (next_unskippable_block < nblocks)
				{
					uint8		vmskipflags;

					vmskipflags = visibilitymap_get_status(vacrel->rel,
														   next_unskippable_block,
														   &vmbuffer);
					if (aggressive)
					{
						if ((vmskipflags & VISIBILITYMAP_ALL_FROZEN) == 0)
							break;
					}
					else
					{
						if ((vmskipflags & VISIBILITYMAP_ALL_VISIBLE) == 0)
							break;
					}
					vacuum_delay_point();
					next_unskippable_block++;
				}
			}

			/*
			 * We know we can't skip the current block.  But set up
			 * skipping_blocks to do the right thing at the following blocks.
			 */
			if (next_unskippable_block - blkno > SKIP_PAGES_THRESHOLD)
				skipping_blocks = true;
			else
				skipping_blocks = false;

			/*
			 * Normally, the fact that we can't skip this block must mean that
			 * it's not all-visible.  But in an aggressive vacuum we know only
			 * that it's not all-frozen, so it might still be all-visible.
			 */
			if (aggressive && VM_ALL_VISIBLE(vacrel->rel, blkno, &vmbuffer))
				all_visible_according_to_vm = true;
		}
		else
		{
			/*
			 * The current block is potentially skippable; if we've seen a
			 * long enough run of skippable blocks to justify skipping it, and
			 * we're not forced to check it, then go ahead and skip.
			 * Otherwise, the page must be at least all-visible if not
			 * all-frozen, so we can set all_visible_according_to_vm = true.
			 */
			if (skipping_blocks && !FORCE_CHECK_PAGE())
			{
				/*
				 * Tricky, tricky.  If this is in aggressive vacuum, the page
				 * must have been all-frozen at the time we checked whether it
				 * was skippable, but it might not be any more.  We must be
				 * careful to count it as a skipped all-frozen page in that
				 * case, or else we'll think we can't update relfrozenxid and
				 * relminmxid.  If it's not an aggressive vacuum, we don't
				 * know whether it was all-frozen, so we have to recheck; but
				 * in this case an approximate answer is OK.
				 */
				if (aggressive || VM_ALL_FROZEN(vacrel->rel, blkno, &vmbuffer))
					vacrel->frozenskipped_pages++;
				continue;
			}
			all_visible_according_to_vm = true;
		}

		vacuum_delay_point();

		/*
		 * Consider if we definitely have enough space to process TIDs on page
		 * already.  If we are close to overrunning the available space for
		 * dead-tuple TIDs, pause and do a cycle of vacuuming before we tackle
		 * this page.
		 */
		if ((dead_tuples->max_tuples - dead_tuples->num_tuples) < MaxHeapTuplesPerPage &&
			dead_tuples->num_tuples > 0)
		{
			/*
			 * Before beginning index vacuuming, we release any pin we may
			 * hold on the visibility map page.  This isn't necessary for
			 * correctness, but we do it anyway to avoid holding the pin
			 * across a lengthy, unrelated operation.
			 */
			if (BufferIsValid(vmbuffer))
			{
				ReleaseBuffer(vmbuffer);
				vmbuffer = InvalidBuffer;
			}

			/* Remove the collected garbage tuples from table and indexes */
			lazy_vacuum(vacrel);

			/*
			 * Vacuum the Free Space Map to make newly-freed space visible on
			 * upper-level FSM pages.  Note we have not yet processed blkno.
			 */
			FreeSpaceMapVacuumRange(vacrel->rel, next_fsm_block_to_vacuum,
									blkno);
			next_fsm_block_to_vacuum = blkno;

			/* Report that we are once again scanning the heap */
			pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
										 PROGRESS_VACUUM_PHASE_SCAN_HEAP);
		}

		/*
		 * Set up visibility map page as needed.
		 *
		 * Pin the visibility map page in case we need to mark the page
		 * all-visible.  In most cases this will be very cheap, because we'll
		 * already have the correct page pinned anyway.  However, it's
		 * possible that (a) next_unskippable_block is covered by a different
		 * VM page than the current block or (b) we released our pin and did a
		 * cycle of index vacuuming.
		 */
		visibilitymap_pin(vacrel->rel, blkno, &vmbuffer);

		buf = ReadBufferExtended(vacrel->rel, MAIN_FORKNUM, blkno,
								 RBM_NORMAL, vacrel->bstrategy);

		/*
		 * We need buffer cleanup lock so that we can prune HOT chains and
		 * defragment the page.
		 */
		if (!ConditionalLockBufferForCleanup(buf))
		{
			bool		hastup;

			/*
			 * If we're not performing an aggressive scan to guard against XID
			 * wraparound, and we don't want to forcibly check the page, then
			 * it's OK to skip vacuuming pages we get a lock conflict on. They
			 * will be dealt with in some future vacuum.
			 */
			if (!aggressive && !FORCE_CHECK_PAGE())
			{
				ReleaseBuffer(buf);
				vacrel->pinskipped_pages++;
				continue;
			}

			/*
			 * Read the page with share lock to see if any xids on it need to
			 * be frozen.  If not we just skip the page, after updating our
			 * scan statistics.  If there are some, we wait for cleanup lock.
			 *
			 * We could defer the lock request further by remembering the page
			 * and coming back to it later, or we could even register
			 * ourselves for multiple buffers and then service whichever one
			 * is received first.  For now, this seems good enough.
			 *
			 * If we get here with aggressive false, then we're just forcibly
			 * checking the page, and so we don't want to insist on getting
			 * the lock; we only need to know if the page contains tuples, so
			 * that we can update nonempty_pages correctly.  It's convenient
			 * to use lazy_check_needs_freeze() for both situations, though.
			 */
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			if (!lazy_check_needs_freeze(buf, &hastup, vacrel))
			{
				UnlockReleaseBuffer(buf);
				vacrel->scanned_pages++;
				vacrel->pinskipped_pages++;
				if (hastup)
					vacrel->nonempty_pages = blkno + 1;
				continue;
			}
			if (!aggressive)
			{
				/*
				 * Here, we must not advance scanned_pages; that would amount
				 * to claiming that the page contains no freezable tuples.
				 */
				UnlockReleaseBuffer(buf);
				vacrel->pinskipped_pages++;
				if (hastup)
					vacrel->nonempty_pages = blkno + 1;
				continue;
			}
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			LockBufferForCleanup(buf);
			/* drop through to normal processing */
		}

		/*
		 * By here we definitely have enough dead_tuples space for whatever
		 * LP_DEAD tids are on this page, we have the visibility map page set
		 * up in case we need to set this page's all_visible/all_frozen bit,
		 * and we have a super-exclusive lock.  Any tuples on this page are
		 * now sure to be "counted" by this VACUUM.
		 *
		 * One last piece of preamble needs to take place before we can prune:
		 * we need to consider new and empty pages.
		 */
		vacrel->scanned_pages++;
		vacrel->tupcount_pages++;

		page = BufferGetPage(buf);

		if (PageIsNew(page))
		{
			/*
			 * All-zeroes pages can be left over if either a backend extends
			 * the relation by a single page, but crashes before the newly
			 * initialized page has been written out, or when bulk-extending
			 * the relation (which creates a number of empty pages at the tail
			 * end of the relation, but enters them into the FSM).
			 *
			 * Note we do not enter the page into the visibilitymap. That has
			 * the downside that we repeatedly visit this page in subsequent
			 * vacuums, but otherwise we'll never not discover the space on a
			 * promoted standby. The harm of repeated checking ought to
			 * normally not be too bad - the space usually should be used at
			 * some point, otherwise there wouldn't be any regular vacuums.
			 *
			 * Make sure these pages are in the FSM, to ensure they can be
			 * reused. Do that by testing if there's any space recorded for
			 * the page. If not, enter it. We do so after releasing the lock
			 * on the heap page, the FSM is approximate, after all.
			 */
			UnlockReleaseBuffer(buf);

			if (GetRecordedFreeSpace(vacrel->rel, blkno) == 0)
			{
				Size		freespace = BLCKSZ - SizeOfPageHeaderData;

				RecordPageWithFreeSpace(vacrel->rel, blkno, freespace);
			}
			continue;
		}

		if (PageIsEmpty(page))
		{
			Size		freespace = PageGetHeapFreeSpace(page);

			/*
			 * Empty pages are always all-visible and all-frozen (note that
			 * the same is currently not true for new pages, see above).
			 */
			if (!PageIsAllVisible(page))
			{
				START_CRIT_SECTION();

				/* mark buffer dirty before writing a WAL record */
				MarkBufferDirty(buf);

				/*
				 * It's possible that another backend has extended the heap,
				 * initialized the page, and then failed to WAL-log the page
				 * due to an ERROR.  Since heap extension is not WAL-logged,
				 * recovery might try to replay our record setting the page
				 * all-visible and find that the page isn't initialized, which
				 * will cause a PANIC.  To prevent that, check whether the
				 * page has been previously WAL-logged, and if not, do that
				 * now.
				 */
				if (RelationNeedsWAL(vacrel->rel) &&
					PageGetLSN(page) == InvalidXLogRecPtr)
					log_newpage_buffer(buf, true);

				PageSetAllVisible(page);
				visibilitymap_set(vacrel->rel, blkno, buf, InvalidXLogRecPtr,
								  vmbuffer, InvalidTransactionId,
								  VISIBILITYMAP_ALL_VISIBLE | VISIBILITYMAP_ALL_FROZEN);
				END_CRIT_SECTION();
			}

			UnlockReleaseBuffer(buf);
			RecordPageWithFreeSpace(vacrel->rel, blkno, freespace);
			continue;
		}

		/*
		 * Prune and freeze tuples.
		 *
		 * Accumulates details of remaining LP_DEAD line pointers on page in
		 * dead tuple list.  This includes LP_DEAD line pointers that we
		 * pruned ourselves, as well as existing LP_DEAD line pointers that
		 * were pruned some time earlier.  Also considers freezing XIDs in the
		 * tuple headers of remaining items with storage.
		 */
		lazy_scan_prune(vacrel, buf, blkno, page, vistest, &prunestate);

		/* Remember the location of the last page with nonremovable tuples */
		if (prunestate.hastup)
			vacrel->nonempty_pages = blkno + 1;

		if (vacrel->nindexes == 0)
		{
			/*
			 * Consider the need to do page-at-a-time heap vacuuming when
			 * using the one-pass strategy now.
			 *
			 * The one-pass strategy will never call lazy_vacuum().  The steps
			 * performed here can be thought of as the one-pass equivalent of
			 * a call to lazy_vacuum().
			 */
			if (prunestate.has_lpdead_items)
			{
				Size		freespace;

				lazy_vacuum_heap_page(vacrel, blkno, buf, 0, &vmbuffer);

				/* Forget the now-vacuumed tuples */
				dead_tuples->num_tuples = 0;

				/*
				 * Periodically perform FSM vacuuming to make newly-freed
				 * space visible on upper FSM pages.  Note we have not yet
				 * performed FSM processing for blkno.
				 *
				 * Call lazy_check_wraparound_failsafe() here, too, since we
				 * also don't want to do that too frequently, or too
				 * infrequently.
				 */
				if (blkno - next_fsm_block_to_vacuum >= VACUUM_FSM_EVERY_PAGES)
				{
					FreeSpaceMapVacuumRange(vacrel->rel, next_fsm_block_to_vacuum,
											blkno);
					next_fsm_block_to_vacuum = blkno;
					lazy_check_wraparound_failsafe(vacrel);
				}

				/*
				 * Now perform FSM processing for blkno, and move on to next
				 * page.
				 *
				 * Our call to lazy_vacuum_heap_page() will have considered if
				 * it's possible to set all_visible/all_frozen independently
				 * of lazy_scan_prune().  Note that prunestate was invalidated
				 * by lazy_vacuum_heap_page() call.
				 */
				freespace = PageGetHeapFreeSpace(page);

				UnlockReleaseBuffer(buf);
				RecordPageWithFreeSpace(vacrel->rel, blkno, freespace);
				continue;
			}

			/*
			 * There was no call to lazy_vacuum_heap_page() because pruning
			 * didn't encounter/create any LP_DEAD items that needed to be
			 * vacuumed.  Prune state has not been invalidated, so proceed
			 * with prunestate-driven visibility map and FSM steps (just like
			 * the two-pass strategy).
			 */
			Assert(dead_tuples->num_tuples == 0);
		}

		/*
		 * Handle setting visibility map bit based on what the VM said about
		 * the page before pruning started, and using prunestate
		 */
		Assert(!prunestate.all_visible || !prunestate.has_lpdead_items);
		if (!all_visible_according_to_vm && prunestate.all_visible)
		{
			uint8		flags = VISIBILITYMAP_ALL_VISIBLE;

			if (prunestate.all_frozen)
				flags |= VISIBILITYMAP_ALL_FROZEN;

			/*
			 * It should never be the case that the visibility map page is set
			 * while the page-level bit is clear, but the reverse is allowed
			 * (if checksums are not enabled).  Regardless, set both bits so
			 * that we get back in sync.
			 *
			 * NB: If the heap page is all-visible but the VM bit is not set,
			 * we don't need to dirty the heap page.  However, if checksums
			 * are enabled, we do need to make sure that the heap page is
			 * dirtied before passing it to visibilitymap_set(), because it
			 * may be logged.  Given that this situation should only happen in
			 * rare cases after a crash, it is not worth optimizing.
			 */
			PageSetAllVisible(page);
			MarkBufferDirty(buf);
			visibilitymap_set(vacrel->rel, blkno, buf, InvalidXLogRecPtr,
							  vmbuffer, prunestate.visibility_cutoff_xid,
							  flags);
		}

		/*
		 * As of PostgreSQL 9.2, the visibility map bit should never be set if
		 * the page-level bit is clear.  However, it's possible that the bit
		 * got cleared after we checked it and before we took the buffer
		 * content lock, so we must recheck before jumping to the conclusion
		 * that something bad has happened.
		 */
		else if (all_visible_according_to_vm && !PageIsAllVisible(page)
				 && VM_ALL_VISIBLE(vacrel->rel, blkno, &vmbuffer))
		{
			elog(WARNING, "page is not marked all-visible but visibility map bit is set in relation \"%s\" page %u",
				 vacrel->relname, blkno);
			visibilitymap_clear(vacrel->rel, blkno, vmbuffer,
								VISIBILITYMAP_VALID_BITS);
		}

		/*
		 * It's possible for the value returned by
		 * GetOldestNonRemovableTransactionId() to move backwards, so it's not
		 * wrong for us to see tuples that appear to not be visible to
		 * everyone yet, while PD_ALL_VISIBLE is already set. The real safe
		 * xmin value never moves backwards, but
		 * GetOldestNonRemovableTransactionId() is conservative and sometimes
		 * returns a value that's unnecessarily small, so if we see that
		 * contradiction it just means that the tuples that we think are not
		 * visible to everyone yet actually are, and the PD_ALL_VISIBLE flag
		 * is correct.
		 *
		 * There should never be dead tuples on a page with PD_ALL_VISIBLE
		 * set, however.
		 */
		else if (prunestate.has_lpdead_items && PageIsAllVisible(page))
		{
			elog(WARNING, "page containing dead tuples is marked as all-visible in relation \"%s\" page %u",
				 vacrel->relname, blkno);
			PageClearAllVisible(page);
			MarkBufferDirty(buf);
			visibilitymap_clear(vacrel->rel, blkno, vmbuffer,
								VISIBILITYMAP_VALID_BITS);
		}

		/*
		 * If the all-visible page is all-frozen but not marked as such yet,
		 * mark it as all-frozen.  Note that all_frozen is only valid if
		 * all_visible is true, so we must check both.
		 */
		else if (all_visible_according_to_vm && prunestate.all_visible &&
				 prunestate.all_frozen &&
				 !VM_ALL_FROZEN(vacrel->rel, blkno, &vmbuffer))
		{
			/*
			 * We can pass InvalidTransactionId as the cutoff XID here,
			 * because setting the all-frozen bit doesn't cause recovery
			 * conflicts.
			 */
			visibilitymap_set(vacrel->rel, blkno, buf, InvalidXLogRecPtr,
							  vmbuffer, InvalidTransactionId,
							  VISIBILITYMAP_ALL_FROZEN);
		}

		/*
		 * Final steps for block: drop super-exclusive lock, record free space
		 * in the FSM
		 */
		if (prunestate.has_lpdead_items && vacrel->do_index_vacuuming)
		{
			/*
			 * Wait until lazy_vacuum_heap_rel() to save free space.  This
			 * doesn't just save us some cycles; it also allows us to record
			 * any additional free space that lazy_vacuum_heap_page() will
			 * make available in cases where it's possible to truncate the
			 * page's line pointer array.
			 *
			 * Note: It's not in fact 100% certain that we really will call
			 * lazy_vacuum_heap_rel() -- lazy_vacuum() might yet opt to skip
			 * index vacuuming (and so must skip heap vacuuming).  This is
			 * deemed okay because it only happens in emergencies. (Besides,
			 * we start recording free space in the FSM once index vacuuming
			 * has been abandoned.)
			 *
			 * Note: The one-pass (no indexes) case is only supposed to make
			 * it this far when there were no LP_DEAD items during pruning.
			 */
			Assert(vacrel->nindexes > 0);
			UnlockReleaseBuffer(buf);
		}
		else
		{
			Size		freespace = PageGetHeapFreeSpace(page);

			UnlockReleaseBuffer(buf);
			RecordPageWithFreeSpace(vacrel->rel, blkno, freespace);
		}
	}

	/* report that everything is now scanned */
	pgstat_progress_update_param(PROGRESS_VACUUM_HEAP_BLKS_SCANNED, blkno);

	/* Clear the block number information */
	vacrel->blkno = InvalidBlockNumber;

	/* now we can compute the new value for pg_class.reltuples */
	vacrel->new_live_tuples = vac_estimate_reltuples(vacrel->rel, nblocks,
													 vacrel->tupcount_pages,
													 vacrel->live_tuples);

	/*
	 * Also compute the total number of surviving heap entries.  In the
	 * (unlikely) scenario that new_live_tuples is -1, take it as zero.
	 */
	vacrel->new_rel_tuples =
		Max(vacrel->new_live_tuples, 0) + vacrel->new_dead_tuples;

	/*
	 * Release any remaining pin on visibility map page.
	 */
	if (BufferIsValid(vmbuffer))
	{
		ReleaseBuffer(vmbuffer);
		vmbuffer = InvalidBuffer;
	}

	/* If any tuples need to be deleted, perform final vacuum cycle */
	/* XXX put a threshold on min number of tuples here? */
	if (dead_tuples->num_tuples > 0)
		lazy_vacuum(vacrel);

	/*
	 * Vacuum the remainder of the Free Space Map.  We must do this whether or
	 * not there were indexes, and whether or not we bypassed index vacuuming.
	 */
	if (blkno > next_fsm_block_to_vacuum)
		FreeSpaceMapVacuumRange(vacrel->rel, next_fsm_block_to_vacuum, blkno);

	/* report all blocks vacuumed */
	pgstat_progress_update_param(PROGRESS_VACUUM_HEAP_BLKS_VACUUMED, blkno);

	/* Do post-vacuum cleanup */
	if (vacrel->nindexes > 0 && vacrel->do_index_cleanup)
		lazy_cleanup_all_indexes(vacrel);

	/*
	 * Free resources managed by lazy_space_alloc().  (We must end parallel
	 * mode/free shared memory before updating index statistics.  We cannot
	 * write while in parallel mode.)
	 */
	lazy_space_free(vacrel);

	/* Update index statistics */
	if (vacrel->nindexes > 0 && vacrel->do_index_cleanup)
		update_index_statistics(vacrel);

	/*
	 * If table has no indexes and at least one heap pages was vacuumed, make
	 * log report that lazy_vacuum_heap_rel would've made had there been
	 * indexes (having indexes implies using the two pass strategy).
	 */
	if (vacrel->nindexes == 0 && vacrel->lpdead_item_pages > 0)
		ereport(elevel,
				(errmsg("\"%s\": removed %lld dead item identifiers in %u pages",
						vacrel->relname, (long long) vacrel->lpdead_items,
						vacrel->lpdead_item_pages)));

	initStringInfo(&buf);
	appendStringInfo(&buf,
					 _("%lld dead row versions cannot be removed yet, oldest xmin: %u\n"),
					 (long long) vacrel->new_dead_tuples, vacrel->OldestXmin);
	appendStringInfo(&buf, ngettext("%u page removed.\n",
									"%u pages removed.\n",
									vacrel->pages_removed),
					 vacrel->pages_removed);
	appendStringInfo(&buf, ngettext("Skipped %u page due to buffer pins, ",
									"Skipped %u pages due to buffer pins, ",
									vacrel->pinskipped_pages),
					 vacrel->pinskipped_pages);
	appendStringInfo(&buf, ngettext("%u frozen page.\n",
									"%u frozen pages.\n",
									vacrel->frozenskipped_pages),
					 vacrel->frozenskipped_pages);
	appendStringInfo(&buf, _("%s."), pg_rusage_show(&ru0));

	ereport(elevel,
			(errmsg("\"%s\": found %lld removable, %lld nonremovable row versions in %u out of %u pages",
					vacrel->relname,
					(long long) vacrel->tuples_deleted,
					(long long) vacrel->num_tuples, vacrel->scanned_pages,
					nblocks),
			 errdetail_internal("%s", buf.data)));
	pfree(buf.data);
}

/*
 *	lazy_scan_prune() -- lazy_scan_heap() pruning and freezing.
 *
 * Caller must hold pin and buffer cleanup lock on the buffer.
 *
 * Prior to PostgreSQL 14 there were very rare cases where heap_page_prune()
 * was allowed to disagree with our HeapTupleSatisfiesVacuum() call about
 * whether or not a tuple should be considered DEAD.  This happened when an
 * inserting transaction concurrently aborted (after our heap_page_prune()
 * call, before our HeapTupleSatisfiesVacuum() call).  There was rather a lot
 * of complexity just so we could deal with tuples that were DEAD to VACUUM,
 * but nevertheless were left with storage after pruning.
 *
 * The approach we take now is to restart pruning when the race condition is
 * detected.  This allows heap_page_prune() to prune the tuples inserted by
 * the now-aborted transaction.  This is a little crude, but it guarantees
 * that any items that make it into the dead_tuples array are simple LP_DEAD
 * line pointers, and that every remaining item with tuple storage is
 * considered as a candidate for freezing.
 */
static void
lazy_scan_prune(LVRelState *vacrel,
				Buffer buf,
				BlockNumber blkno,
				Page page,
				GlobalVisState *vistest,
				LVPagePruneState *prunestate)
{
	Relation	rel = vacrel->rel;
	OffsetNumber offnum,
				maxoff;
	ItemId		itemid;
	HeapTupleData tuple;
	HTSV_Result res;
	int			tuples_deleted,
				lpdead_items,
				new_dead_tuples,
				num_tuples,
				live_tuples;
	int			nfrozen;
	OffsetNumber deadoffsets[MaxHeapTuplesPerPage];
	xl_heap_freeze_tuple frozen[MaxHeapTuplesPerPage];

	maxoff = PageGetMaxOffsetNumber(page);

retry:

	/* Initialize (or reset) page-level counters */
	tuples_deleted = 0;
	lpdead_items = 0;
	new_dead_tuples = 0;
	num_tuples = 0;
	live_tuples = 0;

	/*
	 * Prune all HOT-update chains in this page.
	 *
	 * We count tuples removed by the pruning step as tuples_deleted.  Its
	 * final value can be thought of as the number of tuples that have been
	 * deleted from the table.  It should not be confused with lpdead_items;
	 * lpdead_items's final value can be thought of as the number of tuples
	 * that were deleted from indexes.
	 */
	tuples_deleted = heap_page_prune(rel, buf, vistest,
									 InvalidTransactionId, 0, false,
									 &vacrel->offnum);

	/*
	 * Now scan the page to collect LP_DEAD items and check for tuples
	 * requiring freezing among remaining tuples with storage
	 */
	prunestate->hastup = false;
	prunestate->has_lpdead_items = false;
	prunestate->all_visible = true;
	prunestate->all_frozen = true;
	prunestate->visibility_cutoff_xid = InvalidTransactionId;
	nfrozen = 0;

	for (offnum = FirstOffsetNumber;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		bool		tuple_totally_frozen;

		/*
		 * Set the offset number so that we can display it along with any
		 * error that occurred while processing this tuple.
		 */
		vacrel->offnum = offnum;
		itemid = PageGetItemId(page, offnum);

		if (!ItemIdIsUsed(itemid))
			continue;

		/* Redirect items mustn't be touched */
		if (ItemIdIsRedirected(itemid))
		{
			prunestate->hastup = true;	/* page won't be truncatable */
			continue;
		}

		/*
		 * LP_DEAD items are processed outside of the loop.
		 *
		 * Note that we deliberately don't set hastup=true in the case of an
		 * LP_DEAD item here, which is not how lazy_check_needs_freeze() or
		 * count_nondeletable_pages() do it -- they only consider pages empty
		 * when they only have LP_UNUSED items, which is important for
		 * correctness.
		 *
		 * Our assumption is that any LP_DEAD items we encounter here will
		 * become LP_UNUSED inside lazy_vacuum_heap_page() before we actually
		 * call count_nondeletable_pages().  In any case our opinion of
		 * whether or not a page 'hastup' (which is how our caller sets its
		 * vacrel->nonempty_pages value) is inherently race-prone.  It must be
		 * treated as advisory/unreliable, so we might as well be slightly
		 * optimistic.
		 */
		if (ItemIdIsDead(itemid))
		{
			deadoffsets[lpdead_items++] = offnum;
			prunestate->all_visible = false;
			prunestate->has_lpdead_items = true;
			continue;
		}

		Assert(ItemIdIsNormal(itemid));

		ItemPointerSet(&(tuple.t_self), blkno, offnum);
		tuple.t_data = (HeapTupleHeader) PageGetItem(page, itemid);
		tuple.t_len = ItemIdGetLength(itemid);
		tuple.t_tableOid = RelationGetRelid(rel);

		/*
		 * DEAD tuples are almost always pruned into LP_DEAD line pointers by
		 * heap_page_prune(), but it's possible that the tuple state changed
		 * since heap_page_prune() looked.  Handle that here by restarting.
		 * (See comments at the top of function for a full explanation.)
		 */
		res = HeapTupleSatisfiesVacuum(&tuple, vacrel->OldestXmin, buf);

		if (unlikely(res == HEAPTUPLE_DEAD))
			goto retry;

		/*
		 * The criteria for counting a tuple as live in this block need to
		 * match what analyze.c's acquire_sample_rows() does, otherwise VACUUM
		 * and ANALYZE may produce wildly different reltuples values, e.g.
		 * when there are many recently-dead tuples.
		 *
		 * The logic here is a bit simpler than acquire_sample_rows(), as
		 * VACUUM can't run inside a transaction block, which makes some cases
		 * impossible (e.g. in-progress insert from the same transaction).
		 */
		switch (res)
		{
			case HEAPTUPLE_LIVE:

				/*
				 * Count it as live.  Not only is this natural, but it's also
				 * what acquire_sample_rows() does.
				 */
				live_tuples++;

				/*
				 * Is the tuple definitely visible to all transactions?
				 *
				 * NB: Like with per-tuple hint bits, we can't set the
				 * PD_ALL_VISIBLE flag if the inserter committed
				 * asynchronously. See SetHintBits for more info. Check that
				 * the tuple is hinted xmin-committed because of that.
				 */
				if (prunestate->all_visible)
				{
					TransactionId xmin;

					if (!HeapTupleHeaderXminCommitted(tuple.t_data))
					{
						prunestate->all_visible = false;
						break;
					}

					/*
					 * The inserter definitely committed. But is it old enough
					 * that everyone sees it as committed?
					 */
					xmin = HeapTupleHeaderGetXmin(tuple.t_data);
					if (!TransactionIdPrecedes(xmin, vacrel->OldestXmin))
					{
						prunestate->all_visible = false;
						break;
					}

					/* Track newest xmin on page. */
					if (TransactionIdFollows(xmin, prunestate->visibility_cutoff_xid))
						prunestate->visibility_cutoff_xid = xmin;
				}
				break;
			case HEAPTUPLE_RECENTLY_DEAD:

				/*
				 * If tuple is recently deleted then we must not remove it
				 * from relation.  (We only remove items that are LP_DEAD from
				 * pruning.)
				 */
				new_dead_tuples++;
				prunestate->all_visible = false;
				break;
			case HEAPTUPLE_INSERT_IN_PROGRESS:

				/*
				 * We do not count these rows as live, because we expect the
				 * inserting transaction to update the counters at commit, and
				 * we assume that will happen only after we report our
				 * results.  This assumption is a bit shaky, but it is what
				 * acquire_sample_rows() does, so be consistent.
				 */
				prunestate->all_visible = false;
				break;
			case HEAPTUPLE_DELETE_IN_PROGRESS:
				/* This is an expected case during concurrent vacuum */
				prunestate->all_visible = false;

				/*
				 * Count such rows as live.  As above, we assume the deleting
				 * transaction will commit and update the counters after we
				 * report.
				 */
				live_tuples++;
				break;
			default:
				elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
				break;
		}

		/*
		 * Non-removable tuple (i.e. tuple with storage).
		 *
		 * Check tuple left behind after pruning to see if needs to be frozen
		 * now.
		 */
		num_tuples++;
		prunestate->hastup = true;
		if (heap_prepare_freeze_tuple(tuple.t_data,
									  vacrel->relfrozenxid,
									  vacrel->relminmxid,
									  vacrel->FreezeLimit,
									  vacrel->MultiXactCutoff,
									  &frozen[nfrozen],
									  &tuple_totally_frozen))
		{
			/* Will execute freeze below */
			frozen[nfrozen++].offset = offnum;
		}

		/*
		 * If tuple is not frozen (and not about to become frozen) then caller
		 * had better not go on to set this page's VM bit
		 */
		if (!tuple_totally_frozen)
			prunestate->all_frozen = false;
	}

	/*
	 * We have now divided every item on the page into either an LP_DEAD item
	 * that will need to be vacuumed in indexes later, or a LP_NORMAL tuple
	 * that remains and needs to be considered for freezing now (LP_UNUSED and
	 * LP_REDIRECT items also remain, but are of no further interest to us).
	 *
	 * Add page level counters to caller's counts, and then actually process
	 * LP_DEAD and LP_NORMAL items.
	 */
	vacrel->offnum = InvalidOffsetNumber;

	/*
	 * Consider the need to freeze any items with tuple storage from the page
	 * first (arbitrary)
	 */
	if (nfrozen > 0)
	{
		Assert(prunestate->hastup);

		/*
		 * At least one tuple with storage needs to be frozen -- execute that
		 * now.
		 *
		 * If we need to freeze any tuples we'll mark the buffer dirty, and
		 * write a WAL record recording the changes.  We must log the changes
		 * to be crash-safe against future truncation of CLOG.
		 */
		START_CRIT_SECTION();

		MarkBufferDirty(buf);

		/* execute collected freezes */
		for (int i = 0; i < nfrozen; i++)
		{
			HeapTupleHeader htup;

			itemid = PageGetItemId(page, frozen[i].offset);
			htup = (HeapTupleHeader) PageGetItem(page, itemid);

			heap_execute_freeze_tuple(htup, &frozen[i]);
		}

		/* Now WAL-log freezing if necessary */
		if (RelationNeedsWAL(vacrel->rel))
		{
			XLogRecPtr	recptr;

			recptr = log_heap_freeze(vacrel->rel, buf, vacrel->FreezeLimit,
									 frozen, nfrozen);
			PageSetLSN(page, recptr);
		}

		END_CRIT_SECTION();
	}

	/*
	 * The second pass over the heap can also set visibility map bits, using
	 * the same approach.  This is important when the table frequently has a
	 * few old LP_DEAD items on each page by the time we get to it (typically
	 * because past opportunistic pruning operations freed some non-HOT
	 * tuples).
	 *
	 * VACUUM will call heap_page_is_all_visible() during the second pass over
	 * the heap to determine all_visible and all_frozen for the page -- this
	 * is a specialized version of the logic from this function.  Now that
	 * we've finished pruning and freezing, make sure that we're in total
	 * agreement with heap_page_is_all_visible() using an assertion.
	 */
#ifdef USE_ASSERT_CHECKING
	/* Note that all_frozen value does not matter when !all_visible */
	if (prunestate->all_visible)
	{
		TransactionId cutoff;
		bool		all_frozen;

		if (!heap_page_is_all_visible(vacrel, buf, &cutoff, &all_frozen))
			Assert(false);

		Assert(lpdead_items == 0);
		Assert(prunestate->all_frozen == all_frozen);

		/*
		 * It's possible that we froze tuples and made the page's XID cutoff
		 * (for recovery conflict purposes) FrozenTransactionId.  This is okay
		 * because visibility_cutoff_xid will be logged by our caller in a
		 * moment.
		 */
		Assert(cutoff == FrozenTransactionId ||
			   cutoff == prunestate->visibility_cutoff_xid);
	}
#endif

	/* Add page-local counts to whole-VACUUM counts */
	vacrel->tuples_deleted += tuples_deleted;
	vacrel->lpdead_items += lpdead_items;
	vacrel->new_dead_tuples += new_dead_tuples;
	vacrel->num_tuples += num_tuples;
	vacrel->live_tuples += live_tuples;

	/*
	 * Now save details of the LP_DEAD items from the page in the dead_tuples
	 * array.  Also record that page has dead items in per-page prunestate.
	 */
	if (lpdead_items > 0)
	{
		LVDeadTuples *dead_tuples = vacrel->dead_tuples;
		ItemPointerData tmp;

		Assert(!prunestate->all_visible);
		Assert(prunestate->has_lpdead_items);

		vacrel->lpdead_item_pages++;

		ItemPointerSetBlockNumber(&tmp, blkno);

		for (int i = 0; i < lpdead_items; i++)
		{
			ItemPointerSetOffsetNumber(&tmp, deadoffsets[i]);
			dead_tuples->itemptrs[dead_tuples->num_tuples++] = tmp;
		}

		Assert(dead_tuples->num_tuples <= dead_tuples->max_tuples);
		pgstat_progress_update_param(PROGRESS_VACUUM_NUM_DEAD_TUPLES,
									 dead_tuples->num_tuples);
	}
}

/*
 * Remove the collected garbage tuples from the table and its indexes.
 *
 * In rare emergencies, the ongoing VACUUM operation can be made to skip both
 * index vacuuming and index cleanup at the point we're called.  This avoids
 * having the whole system refuse to allocate further XIDs/MultiXactIds due to
 * wraparound.
 */
static void
lazy_vacuum(LVRelState *vacrel)
{
	/* Should not end up here with no indexes */
	Assert(vacrel->nindexes > 0);
	Assert(!IsParallelWorker());
	Assert(vacrel->lpdead_item_pages > 0);

	if (!vacrel->do_index_vacuuming)
	{
		Assert(!vacrel->do_index_cleanup);
		vacrel->dead_tuples->num_tuples = 0;
		return;
	}

	if (lazy_vacuum_all_indexes(vacrel))
	{
		/*
		 * We successfully completed a round of index vacuuming.  Do related
		 * heap vacuuming now.
		 */
		lazy_vacuum_heap_rel(vacrel);
	}
	else
	{
		/*
		 * Failsafe case.
		 *
		 * we attempted index vacuuming, but didn't finish a full round/full
		 * index scan.  This happens when relfrozenxid or relminmxid is too
		 * far in the past.
		 *
		 * From this point on the VACUUM operation will do no further index
		 * vacuuming or heap vacuuming.  It will do any remaining pruning that
		 * may be required, plus other heap-related and relation-level
		 * maintenance tasks.  But that's it.
		 */
		Assert(vacrel->do_failsafe);
	}

	/*
	 * Forget the now-vacuumed tuples -- just press on
	 */
	vacrel->dead_tuples->num_tuples = 0;
}

/*
 *	lazy_vacuum_all_indexes() -- Main entry for index vacuuming
 *
 * Returns true in the common case when all indexes were successfully
 * vacuumed.  Returns false in rare cases where we determined that the ongoing
 * VACUUM operation is at risk of taking too long to finish, leading to
 * wraparound failure.
 */
static bool
lazy_vacuum_all_indexes(LVRelState *vacrel)
{
	bool		allindexes = true;

	Assert(!IsParallelWorker());
	Assert(vacrel->nindexes > 0);
	Assert(vacrel->do_index_vacuuming);
	Assert(vacrel->do_index_cleanup);
	Assert(TransactionIdIsNormal(vacrel->relfrozenxid));
	Assert(MultiXactIdIsValid(vacrel->relminmxid));

	/* Precheck for XID wraparound emergencies */
	if (lazy_check_wraparound_failsafe(vacrel))
	{
		/* Wraparound emergency -- don't even start an index scan */
		return false;
	}

	/* Report that we are now vacuuming indexes */
	pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
								 PROGRESS_VACUUM_PHASE_VACUUM_INDEX);

	if (!ParallelVacuumIsActive(vacrel))
	{
		for (int idx = 0; idx < vacrel->nindexes; idx++)
		{
			Relation	indrel = vacrel->indrels[idx];
			IndexBulkDeleteResult *istat = vacrel->indstats[idx];

			vacrel->indstats[idx] =
				lazy_vacuum_one_index(indrel, istat, vacrel->old_live_tuples,
									  vacrel);

			if (lazy_check_wraparound_failsafe(vacrel))
			{
				/* Wraparound emergency -- end current index scan */
				allindexes = false;
				break;
			}
		}
	}
	else
	{
		/* Outsource everything to parallel variant */
		do_parallel_lazy_vacuum_all_indexes(vacrel);

		/*
		 * Do a postcheck to consider applying wraparound failsafe now.  Note
		 * that parallel VACUUM only gets the precheck and this postcheck.
		 */
		if (lazy_check_wraparound_failsafe(vacrel))
			allindexes = false;
	}

	/*
	 * We delete all LP_DEAD items from the first heap pass in all indexes on
	 * each call here (except calls where we choose to do the failsafe). This
	 * makes the next call to lazy_vacuum_heap_rel() safe (except in the event
	 * of the failsafe triggering, which prevents the next call from taking
	 * place).
	 */
	Assert(vacrel->num_index_scans > 0 ||
		   vacrel->dead_tuples->num_tuples == vacrel->lpdead_items);
	Assert(allindexes || vacrel->do_failsafe);

	/*
	 * Increase and report the number of index scans.
	 *
	 * We deliberately include the case where we started a round of bulk
	 * deletes that we weren't able to finish due to the failsafe triggering.
	 */
	vacrel->num_index_scans++;
	pgstat_progress_update_param(PROGRESS_VACUUM_NUM_INDEX_VACUUMS,
								 vacrel->num_index_scans);

	return allindexes;
}

/*
 *	lazy_vacuum_heap_rel() -- second pass over the heap for two pass strategy
 *
 * This routine marks LP_DEAD items in vacrel->dead_tuples array as LP_UNUSED.
 * Pages that never had lazy_scan_prune record LP_DEAD items are not visited
 * at all.
 *
 * We may also be able to truncate the line pointer array of the heap pages we
 * visit.  If there is a contiguous group of LP_UNUSED items at the end of the
 * array, it can be reclaimed as free space.  These LP_UNUSED items usually
 * start out as LP_DEAD items recorded by lazy_scan_prune (we set items from
 * each page to LP_UNUSED, and then consider if it's possible to truncate the
 * page's line pointer array).
 *
 * Note: the reason for doing this as a second pass is we cannot remove the
 * tuples until we've removed their index entries, and we want to process
 * index entry removal in batches as large as possible.
 */
static void
lazy_vacuum_heap_rel(LVRelState *vacrel)
{
	int			tupindex;
	int			vacuumed_pages;
	PGRUsage	ru0;
	Buffer		vmbuffer = InvalidBuffer;
	LVSavedErrInfo saved_err_info;

	Assert(vacrel->do_index_vacuuming);
	Assert(vacrel->do_index_cleanup);
	Assert(vacrel->num_index_scans > 0);

	/* Report that we are now vacuuming the heap */
	pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
								 PROGRESS_VACUUM_PHASE_VACUUM_HEAP);

	/* Update error traceback information */
	update_vacuum_error_info(vacrel, &saved_err_info,
							 VACUUM_ERRCB_PHASE_VACUUM_HEAP,
							 InvalidBlockNumber, InvalidOffsetNumber);

	pg_rusage_init(&ru0);
	vacuumed_pages = 0;

	tupindex = 0;
	while (tupindex < vacrel->dead_tuples->num_tuples)
	{
		BlockNumber tblk;
		Buffer		buf;
		Page		page;
		Size		freespace;

		vacuum_delay_point();

		tblk = ItemPointerGetBlockNumber(&vacrel->dead_tuples->itemptrs[tupindex]);
		vacrel->blkno = tblk;
		buf = ReadBufferExtended(vacrel->rel, MAIN_FORKNUM, tblk, RBM_NORMAL,
								 vacrel->bstrategy);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		tupindex = lazy_vacuum_heap_page(vacrel, tblk, buf, tupindex,
										 &vmbuffer);

		/* Now that we've vacuumed the page, record its available space */
		page = BufferGetPage(buf);
		freespace = PageGetHeapFreeSpace(page);

		UnlockReleaseBuffer(buf);
		RecordPageWithFreeSpace(vacrel->rel, tblk, freespace);
		vacuumed_pages++;
	}

	/* Clear the block number information */
	vacrel->blkno = InvalidBlockNumber;

	if (BufferIsValid(vmbuffer))
	{
		ReleaseBuffer(vmbuffer);
		vmbuffer = InvalidBuffer;
	}

	/*
	 * We set all LP_DEAD items from the first heap pass to LP_UNUSED during
	 * the second heap pass.  No more, no less.
	 */
	Assert(vacrel->num_index_scans > 1 ||
		   (tupindex == vacrel->lpdead_items &&
			vacuumed_pages == vacrel->lpdead_item_pages));

	ereport(elevel,
			(errmsg("\"%s\": removed %d dead item identifiers in %u pages",
					vacrel->relname, tupindex, vacuumed_pages),
			 errdetail_internal("%s", pg_rusage_show(&ru0))));

	/* Revert to the previous phase information for error traceback */
	restore_vacuum_error_info(vacrel, &saved_err_info);
}

/*
 *	lazy_vacuum_heap_page() -- free page's LP_DEAD items listed in the
 *						  vacrel->dead_tuples array.
 *
 * Caller must have an exclusive buffer lock on the buffer (though a
 * super-exclusive lock is also acceptable).
 *
 * tupindex is the index in vacrel->dead_tuples of the first dead tuple for
 * this page.  We assume the rest follow sequentially.  The return value is
 * the first tupindex after the tuples of this page.
 *
 * Prior to PostgreSQL 14 there were rare cases where this routine had to set
 * tuples with storage to unused.  These days it is strictly responsible for
 * marking LP_DEAD stub line pointers as unused.  This only happens for those
 * LP_DEAD items on the page that were determined to be LP_DEAD items back
 * when the same page was visited by lazy_scan_prune() (i.e. those whose TID
 * was recorded in the dead_tuples array).
 */
static int
lazy_vacuum_heap_page(LVRelState *vacrel, BlockNumber blkno, Buffer buffer,
					  int tupindex, Buffer *vmbuffer)
{
	LVDeadTuples *dead_tuples = vacrel->dead_tuples;
	Page		page = BufferGetPage(buffer);
	OffsetNumber unused[MaxHeapTuplesPerPage];
	int			uncnt = 0;
	TransactionId visibility_cutoff_xid;
	bool		all_frozen;
	LVSavedErrInfo saved_err_info;

	Assert(vacrel->nindexes == 0 || vacrel->do_index_vacuuming);

	pgstat_progress_update_param(PROGRESS_VACUUM_HEAP_BLKS_VACUUMED, blkno);

	/* Update error traceback information */
	update_vacuum_error_info(vacrel, &saved_err_info,
							 VACUUM_ERRCB_PHASE_VACUUM_HEAP, blkno,
							 InvalidOffsetNumber);

	START_CRIT_SECTION();

	for (; tupindex < dead_tuples->num_tuples; tupindex++)
	{
		BlockNumber tblk;
		OffsetNumber toff;
		ItemId		itemid;

		tblk = ItemPointerGetBlockNumber(&dead_tuples->itemptrs[tupindex]);
		if (tblk != blkno)
			break;				/* past end of tuples for this block */
		toff = ItemPointerGetOffsetNumber(&dead_tuples->itemptrs[tupindex]);
		itemid = PageGetItemId(page, toff);

		Assert(ItemIdIsDead(itemid) && !ItemIdHasStorage(itemid));
		ItemIdSetUnused(itemid);
		unused[uncnt++] = toff;
	}

	Assert(uncnt > 0);

	/* Attempt to truncate line pointer array now */
	PageTruncateLinePointerArray(page);

	/*
	 * Mark buffer dirty before we write WAL.
	 */
	MarkBufferDirty(buffer);

	/* XLOG stuff */
	if (RelationNeedsWAL(vacrel->rel))
	{
		xl_heap_vacuum xlrec;
		XLogRecPtr	recptr;

		xlrec.nunused = uncnt;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfHeapVacuum);

		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
		XLogRegisterBufData(0, (char *) unused, uncnt * sizeof(OffsetNumber));

		recptr = XLogInsert(RM_HEAP2_ID, XLOG_HEAP2_VACUUM);

		PageSetLSN(page, recptr);
	}

	/*
	 * End critical section, so we safely can do visibility tests (which
	 * possibly need to perform IO and allocate memory!). If we crash now the
	 * page (including the corresponding vm bit) might not be marked all
	 * visible, but that's fine. A later vacuum will fix that.
	 */
	END_CRIT_SECTION();

	/*
	 * Now that we have removed the LD_DEAD items from the page, once again
	 * check if the page has become all-visible.  The page is already marked
	 * dirty, exclusively locked, and, if needed, a full page image has been
	 * emitted.
	 */
	if (heap_page_is_all_visible(vacrel, buffer, &visibility_cutoff_xid,
								 &all_frozen))
		PageSetAllVisible(page);

	/*
	 * All the changes to the heap page have been done. If the all-visible
	 * flag is now set, also set the VM all-visible bit (and, if possible, the
	 * all-frozen bit) unless this has already been done previously.
	 */
	if (PageIsAllVisible(page))
	{
		uint8		flags = 0;
		uint8		vm_status = visibilitymap_get_status(vacrel->rel,
														 blkno, vmbuffer);

		/* Set the VM all-frozen bit to flag, if needed */
		if ((vm_status & VISIBILITYMAP_ALL_VISIBLE) == 0)
			flags |= VISIBILITYMAP_ALL_VISIBLE;
		if ((vm_status & VISIBILITYMAP_ALL_FROZEN) == 0 && all_frozen)
			flags |= VISIBILITYMAP_ALL_FROZEN;

		Assert(BufferIsValid(*vmbuffer));
		if (flags != 0)
			visibilitymap_set(vacrel->rel, blkno, buffer, InvalidXLogRecPtr,
							  *vmbuffer, visibility_cutoff_xid, flags);
	}

	/* Revert to the previous phase information for error traceback */
	restore_vacuum_error_info(vacrel, &saved_err_info);
	return tupindex;
}

/*
 *	lazy_check_needs_freeze() -- scan page to see if any tuples
 *					 need to be cleaned to avoid wraparound
 *
 * Returns true if the page needs to be vacuumed using cleanup lock.
 * Also returns a flag indicating whether page contains any tuples at all.
 */
static bool
lazy_check_needs_freeze(Buffer buf, bool *hastup, LVRelState *vacrel)
{
	Page		page = BufferGetPage(buf);
	OffsetNumber offnum,
				maxoff;
	HeapTupleHeader tupleheader;

	*hastup = false;

	/*
	 * New and empty pages, obviously, don't contain tuples. We could make
	 * sure that the page is registered in the FSM, but it doesn't seem worth
	 * waiting for a cleanup lock just for that, especially because it's
	 * likely that the pin holder will do so.
	 */
	if (PageIsNew(page) || PageIsEmpty(page))
		return false;

	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = FirstOffsetNumber;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemid;

		/*
		 * Set the offset number so that we can display it along with any
		 * error that occurred while processing this tuple.
		 */
		vacrel->offnum = offnum;
		itemid = PageGetItemId(page, offnum);

		/* this should match hastup test in count_nondeletable_pages() */
		if (ItemIdIsUsed(itemid))
			*hastup = true;

		/* dead and redirect items never need freezing */
		if (!ItemIdIsNormal(itemid))
			continue;

		tupleheader = (HeapTupleHeader) PageGetItem(page, itemid);

		if (heap_tuple_needs_freeze(tupleheader, vacrel->FreezeLimit,
									vacrel->MultiXactCutoff, buf))
			break;
	}							/* scan along page */

	/* Clear the offset information once we have processed the given page. */
	vacrel->offnum = InvalidOffsetNumber;

	return (offnum <= maxoff);
}

/*
 * Trigger the failsafe to avoid wraparound failure when vacrel table has a
 * relfrozenxid and/or relminmxid that is dangerously far in the past.
 *
 * Triggering the failsafe makes the ongoing VACUUM bypass any further index
 * vacuuming and heap vacuuming.  It also stops the ongoing VACUUM from
 * applying any cost-based delay that may be in effect.
 *
 * Returns true when failsafe has been triggered.
 *
 * Caller is expected to call here before and after vacuuming each index in
 * the case of two-pass VACUUM, or every VACUUM_FSM_EVERY_PAGES blocks in the
 * case of no-indexes/one-pass VACUUM.
 *
 * There is also a precheck before the first pass over the heap begins, which
 * is helpful when the failsafe initially triggers during a non-aggressive
 * VACUUM -- the automatic aggressive vacuum to prevent wraparound that
 * follows can independently trigger the failsafe right away.
 */
static bool
lazy_check_wraparound_failsafe(LVRelState *vacrel)
{
	/* Avoid calling vacuum_xid_failsafe_check() very frequently */
	if (vacrel->num_index_scans == 0 &&
		vacrel->rel_pages <= FAILSAFE_MIN_PAGES)
		return false;

	/* Don't warn more than once per VACUUM */
	if (vacrel->do_failsafe)
		return true;

	if (unlikely(vacuum_xid_failsafe_check(vacrel->relfrozenxid,
										   vacrel->relminmxid)))
	{
		Assert(vacrel->do_index_vacuuming);
		Assert(vacrel->do_index_cleanup);

		vacrel->do_index_vacuuming = false;
		vacrel->do_index_cleanup = false;
		vacrel->do_failsafe = true;

		ereport(WARNING,
				(errmsg("abandoned index vacuuming of table \"%s.%s.%s\" as a failsafe after %d index scans",
						get_database_name(MyDatabaseId),
						vacrel->relnamespace,
						vacrel->relname,
						vacrel->num_index_scans),
				 errdetail("table's relfrozenxid or relminmxid is too far in the past"),
				 errhint("Consider increasing configuration parameter \"maintenance_work_mem\" or \"autovacuum_work_mem\".\n"
						 "You might also need to consider other ways for VACUUM to keep up with the allocation of transaction IDs.")));

		/* Stop applying cost limits from this point on */
		VacuumCostActive = false;
		VacuumCostBalance = 0;

		return true;
	}

	return false;
}

/*
 * Perform lazy_vacuum_all_indexes() steps in parallel
 */
static void
do_parallel_lazy_vacuum_all_indexes(LVRelState *vacrel)
{
	/* Tell parallel workers to do index vacuuming */
	vacrel->lps->lvshared->for_cleanup = false;
	vacrel->lps->lvshared->first_time = false;

	/*
	 * We can only provide an approximate value of num_heap_tuples in vacuum
	 * cases.
	 */
	vacrel->lps->lvshared->reltuples = vacrel->old_live_tuples;
	vacrel->lps->lvshared->estimated_count = true;

	do_parallel_vacuum_or_cleanup(vacrel,
								  vacrel->lps->nindexes_parallel_bulkdel);
}

/*
 * Perform lazy_cleanup_all_indexes() steps in parallel
 */
static void
do_parallel_lazy_cleanup_all_indexes(LVRelState *vacrel)
{
	int			nworkers;

	/*
	 * If parallel vacuum is active we perform index cleanup with parallel
	 * workers.
	 *
	 * Tell parallel workers to do index cleanup.
	 */
	vacrel->lps->lvshared->for_cleanup = true;
	vacrel->lps->lvshared->first_time = (vacrel->num_index_scans == 0);

	/*
	 * Now we can provide a better estimate of total number of surviving
	 * tuples (we assume indexes are more interested in that than in the
	 * number of nominally live tuples).
	 */
	vacrel->lps->lvshared->reltuples = vacrel->new_rel_tuples;
	vacrel->lps->lvshared->estimated_count =
		(vacrel->tupcount_pages < vacrel->rel_pages);

	/* Determine the number of parallel workers to launch */
	if (vacrel->lps->lvshared->first_time)
		nworkers = vacrel->lps->nindexes_parallel_cleanup +
			vacrel->lps->nindexes_parallel_condcleanup;
	else
		nworkers = vacrel->lps->nindexes_parallel_cleanup;

	do_parallel_vacuum_or_cleanup(vacrel, nworkers);
}

/*
 * Perform index vacuum or index cleanup with parallel workers.  This function
 * must be used by the parallel vacuum leader process.  The caller must set
 * lps->lvshared->for_cleanup to indicate whether to perform vacuum or
 * cleanup.
 */
static void
do_parallel_vacuum_or_cleanup(LVRelState *vacrel, int nworkers)
{
	LVParallelState *lps = vacrel->lps;

	Assert(!IsParallelWorker());
	Assert(ParallelVacuumIsActive(vacrel));
	Assert(vacrel->nindexes > 0);

	/* The leader process will participate */
	nworkers--;

	/*
	 * It is possible that parallel context is initialized with fewer workers
	 * than the number of indexes that need a separate worker in the current
	 * phase, so we need to consider it.  See compute_parallel_vacuum_workers.
	 */
	nworkers = Min(nworkers, lps->pcxt->nworkers);

	/* Setup the shared cost-based vacuum delay and launch workers */
	if (nworkers > 0)
	{
		if (vacrel->num_index_scans > 0)
		{
			/* Reset the parallel index processing counter */
			pg_atomic_write_u32(&(lps->lvshared->idx), 0);

			/* Reinitialize the parallel context to relaunch parallel workers */
			ReinitializeParallelDSM(lps->pcxt);
		}

		/*
		 * Set up shared cost balance and the number of active workers for
		 * vacuum delay.  We need to do this before launching workers as
		 * otherwise, they might not see the updated values for these
		 * parameters.
		 */
		pg_atomic_write_u32(&(lps->lvshared->cost_balance), VacuumCostBalance);
		pg_atomic_write_u32(&(lps->lvshared->active_nworkers), 0);

		/*
		 * The number of workers can vary between bulkdelete and cleanup
		 * phase.
		 */
		ReinitializeParallelWorkers(lps->pcxt, nworkers);

		LaunchParallelWorkers(lps->pcxt);

		if (lps->pcxt->nworkers_launched > 0)
		{
			/*
			 * Reset the local cost values for leader backend as we have
			 * already accumulated the remaining balance of heap.
			 */
			VacuumCostBalance = 0;
			VacuumCostBalanceLocal = 0;

			/* Enable shared cost balance for leader backend */
			VacuumSharedCostBalance = &(lps->lvshared->cost_balance);
			VacuumActiveNWorkers = &(lps->lvshared->active_nworkers);
		}

		if (lps->lvshared->for_cleanup)
			ereport(elevel,
					(errmsg(ngettext("launched %d parallel vacuum worker for index cleanup (planned: %d)",
									 "launched %d parallel vacuum workers for index cleanup (planned: %d)",
									 lps->pcxt->nworkers_launched),
							lps->pcxt->nworkers_launched, nworkers)));
		else
			ereport(elevel,
					(errmsg(ngettext("launched %d parallel vacuum worker for index vacuuming (planned: %d)",
									 "launched %d parallel vacuum workers for index vacuuming (planned: %d)",
									 lps->pcxt->nworkers_launched),
							lps->pcxt->nworkers_launched, nworkers)));
	}

	/* Process the indexes that can be processed by only leader process */
	do_serial_processing_for_unsafe_indexes(vacrel, lps->lvshared);

	/*
	 * Join as a parallel worker.  The leader process alone processes all the
	 * indexes in the case where no workers are launched.
	 */
	do_parallel_processing(vacrel, lps->lvshared);

	/*
	 * Next, accumulate buffer and WAL usage.  (This must wait for the workers
	 * to finish, or we might get incomplete data.)
	 */
	if (nworkers > 0)
	{
		/* Wait for all vacuum workers to finish */
		WaitForParallelWorkersToFinish(lps->pcxt);

		for (int i = 0; i < lps->pcxt->nworkers_launched; i++)
			InstrAccumParallelQuery(&lps->buffer_usage[i], &lps->wal_usage[i]);
	}

	/*
	 * Carry the shared balance value to heap scan and disable shared costing
	 */
	if (VacuumSharedCostBalance)
	{
		VacuumCostBalance = pg_atomic_read_u32(VacuumSharedCostBalance);
		VacuumSharedCostBalance = NULL;
		VacuumActiveNWorkers = NULL;
	}
}

/*
 * Index vacuum/cleanup routine used by the leader process and parallel
 * vacuum worker processes to process the indexes in parallel.
 */
static void
do_parallel_processing(LVRelState *vacrel, LVShared *lvshared)
{
	/*
	 * Increment the active worker count if we are able to launch any worker.
	 */
	if (VacuumActiveNWorkers)
		pg_atomic_add_fetch_u32(VacuumActiveNWorkers, 1);

	/* Loop until all indexes are vacuumed */
	for (;;)
	{
		int			idx;
		LVSharedIndStats *shared_istat;
		Relation	indrel;
		IndexBulkDeleteResult *istat;

		/* Get an index number to process */
		idx = pg_atomic_fetch_add_u32(&(lvshared->idx), 1);

		/* Done for all indexes? */
		if (idx >= vacrel->nindexes)
			break;

		/* Get the index statistics of this index from DSM */
		shared_istat = parallel_stats_for_idx(lvshared, idx);

		/* Skip indexes not participating in parallelism */
		if (shared_istat == NULL)
			continue;

		indrel = vacrel->indrels[idx];

		/*
		 * Skip processing indexes that are unsafe for workers (these are
		 * processed in do_serial_processing_for_unsafe_indexes() by leader)
		 */
		if (!parallel_processing_is_safe(indrel, lvshared))
			continue;

		/* Do vacuum or cleanup of the index */
		istat = (vacrel->indstats[idx]);
		vacrel->indstats[idx] = parallel_process_one_index(indrel, istat,
														   lvshared,
														   shared_istat,
														   vacrel);
	}

	/*
	 * We have completed the index vacuum so decrement the active worker
	 * count.
	 */
	if (VacuumActiveNWorkers)
		pg_atomic_sub_fetch_u32(VacuumActiveNWorkers, 1);
}

/*
 * Vacuum or cleanup indexes that can be processed by only the leader process
 * because these indexes don't support parallel operation at that phase.
 */
static void
do_serial_processing_for_unsafe_indexes(LVRelState *vacrel, LVShared *lvshared)
{
	Assert(!IsParallelWorker());

	/*
	 * Increment the active worker count if we are able to launch any worker.
	 */
	if (VacuumActiveNWorkers)
		pg_atomic_add_fetch_u32(VacuumActiveNWorkers, 1);

	for (int idx = 0; idx < vacrel->nindexes; idx++)
	{
		LVSharedIndStats *shared_istat;
		Relation	indrel;
		IndexBulkDeleteResult *istat;

		shared_istat = parallel_stats_for_idx(lvshared, idx);

		/* Skip already-complete indexes */
		if (shared_istat != NULL)
			continue;

		indrel = vacrel->indrels[idx];

		/*
		 * We're only here for the unsafe indexes
		 */
		if (parallel_processing_is_safe(indrel, lvshared))
			continue;

		/* Do vacuum or cleanup of the index */
		istat = (vacrel->indstats[idx]);
		vacrel->indstats[idx] = parallel_process_one_index(indrel, istat,
														   lvshared,
														   shared_istat,
														   vacrel);
	}

	/*
	 * We have completed the index vacuum so decrement the active worker
	 * count.
	 */
	if (VacuumActiveNWorkers)
		pg_atomic_sub_fetch_u32(VacuumActiveNWorkers, 1);
}

/*
 * Vacuum or cleanup index either by leader process or by one of the worker
 * process.  After processing the index this function copies the index
 * statistics returned from ambulkdelete and amvacuumcleanup to the DSM
 * segment.
 */
static IndexBulkDeleteResult *
parallel_process_one_index(Relation indrel,
						   IndexBulkDeleteResult *istat,
						   LVShared *lvshared,
						   LVSharedIndStats *shared_istat,
						   LVRelState *vacrel)
{
	IndexBulkDeleteResult *istat_res;

	/*
	 * Update the pointer to the corresponding bulk-deletion result if someone
	 * has already updated it
	 */
	if (shared_istat && shared_istat->updated && istat == NULL)
		istat = &shared_istat->istat;

	/* Do vacuum or cleanup of the index */
	if (lvshared->for_cleanup)
		istat_res = lazy_cleanup_one_index(indrel, istat, lvshared->reltuples,
										   lvshared->estimated_count, vacrel);
	else
		istat_res = lazy_vacuum_one_index(indrel, istat, lvshared->reltuples,
										  vacrel);

	/*
	 * Copy the index bulk-deletion result returned from ambulkdelete and
	 * amvacuumcleanup to the DSM segment if it's the first cycle because they
	 * allocate locally and it's possible that an index will be vacuumed by a
	 * different vacuum process the next cycle.  Copying the result normally
	 * happens only the first time an index is vacuumed.  For any additional
	 * vacuum pass, we directly point to the result on the DSM segment and
	 * pass it to vacuum index APIs so that workers can update it directly.
	 *
	 * Since all vacuum workers write the bulk-deletion result at different
	 * slots we can write them without locking.
	 */
	if (shared_istat && !shared_istat->updated && istat_res != NULL)
	{
		memcpy(&shared_istat->istat, istat_res, sizeof(IndexBulkDeleteResult));
		shared_istat->updated = true;

		/* Free the locally-allocated bulk-deletion result */
		pfree(istat_res);

		/* return the pointer to the result from shared memory */
		return &shared_istat->istat;
	}

	return istat_res;
}

/*
 *	lazy_cleanup_all_indexes() -- cleanup all indexes of relation.
 */
static void
lazy_cleanup_all_indexes(LVRelState *vacrel)
{
	Assert(!IsParallelWorker());
	Assert(vacrel->nindexes > 0);

	/* Report that we are now cleaning up indexes */
	pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
								 PROGRESS_VACUUM_PHASE_INDEX_CLEANUP);

	if (!ParallelVacuumIsActive(vacrel))
	{
		double		reltuples = vacrel->new_rel_tuples;
		bool		estimated_count =
		vacrel->tupcount_pages < vacrel->rel_pages;

		for (int idx = 0; idx < vacrel->nindexes; idx++)
		{
			Relation	indrel = vacrel->indrels[idx];
			IndexBulkDeleteResult *istat = vacrel->indstats[idx];

			vacrel->indstats[idx] =
				lazy_cleanup_one_index(indrel, istat, reltuples,
									   estimated_count, vacrel);
		}
	}
	else
	{
		/* Outsource everything to parallel variant */
		do_parallel_lazy_cleanup_all_indexes(vacrel);
	}
}

/*
 *	lazy_vacuum_one_index() -- vacuum index relation.
 *
 *		Delete all the index entries pointing to tuples listed in
 *		dead_tuples, and update running statistics.
 *
 *		reltuples is the number of heap tuples to be passed to the
 *		bulkdelete callback.  It's always assumed to be estimated.
 *
 * Returns bulk delete stats derived from input stats
 */
static IndexBulkDeleteResult *
lazy_vacuum_one_index(Relation indrel, IndexBulkDeleteResult *istat,
					  double reltuples, LVRelState *vacrel)
{
	IndexVacuumInfo ivinfo;
	PGRUsage	ru0;
	LVSavedErrInfo saved_err_info;

	pg_rusage_init(&ru0);

	ivinfo.index = indrel;
	ivinfo.analyze_only = false;
	ivinfo.report_progress = false;
	ivinfo.estimated_count = true;
	ivinfo.message_level = elevel;
	ivinfo.num_heap_tuples = reltuples;
	ivinfo.strategy = vacrel->bstrategy;

	/*
	 * Update error traceback information.
	 *
	 * The index name is saved during this phase and restored immediately
	 * after this phase.  See vacuum_error_callback.
	 */
	Assert(vacrel->indname == NULL);
	vacrel->indname = pstrdup(RelationGetRelationName(indrel));
	update_vacuum_error_info(vacrel, &saved_err_info,
							 VACUUM_ERRCB_PHASE_VACUUM_INDEX,
							 InvalidBlockNumber, InvalidOffsetNumber);

	/* Do bulk deletion */
	istat = index_bulk_delete(&ivinfo, istat, lazy_tid_reaped,
							  (void *) vacrel->dead_tuples);

	ereport(elevel,
			(errmsg("scanned index \"%s\" to remove %d row versions",
					vacrel->indname, vacrel->dead_tuples->num_tuples),
			 errdetail_internal("%s", pg_rusage_show(&ru0))));

	/* Revert to the previous phase information for error traceback */
	restore_vacuum_error_info(vacrel, &saved_err_info);
	pfree(vacrel->indname);
	vacrel->indname = NULL;

	return istat;
}

/*
 *	lazy_cleanup_one_index() -- do post-vacuum cleanup for index relation.
 *
 *		reltuples is the number of heap tuples and estimated_count is true
 *		if reltuples is an estimated value.
 *
 * Returns bulk delete stats derived from input stats
 */
static IndexBulkDeleteResult *
lazy_cleanup_one_index(Relation indrel, IndexBulkDeleteResult *istat,
					   double reltuples, bool estimated_count,
					   LVRelState *vacrel)
{
	IndexVacuumInfo ivinfo;
	PGRUsage	ru0;
	LVSavedErrInfo saved_err_info;

	pg_rusage_init(&ru0);

	ivinfo.index = indrel;
	ivinfo.analyze_only = false;
	ivinfo.report_progress = false;
	ivinfo.estimated_count = estimated_count;
	ivinfo.message_level = elevel;

	ivinfo.num_heap_tuples = reltuples;
	ivinfo.strategy = vacrel->bstrategy;

	/*
	 * Update error traceback information.
	 *
	 * The index name is saved during this phase and restored immediately
	 * after this phase.  See vacuum_error_callback.
	 */
	Assert(vacrel->indname == NULL);
	vacrel->indname = pstrdup(RelationGetRelationName(indrel));
	update_vacuum_error_info(vacrel, &saved_err_info,
							 VACUUM_ERRCB_PHASE_INDEX_CLEANUP,
							 InvalidBlockNumber, InvalidOffsetNumber);

	istat = index_vacuum_cleanup(&ivinfo, istat);

	if (istat)
	{
		ereport(elevel,
				(errmsg("index \"%s\" now contains %.0f row versions in %u pages",
						RelationGetRelationName(indrel),
						(istat)->num_index_tuples,
						(istat)->num_pages),
				 errdetail("%.0f index row versions were removed.\n"
						   "%u index pages were newly deleted.\n"
						   "%u index pages are currently deleted, of which %u are currently reusable.\n"
						   "%s.",
						   (istat)->tuples_removed,
						   (istat)->pages_newly_deleted,
						   (istat)->pages_deleted, (istat)->pages_free,
						   pg_rusage_show(&ru0))));
	}

	/* Revert to the previous phase information for error traceback */
	restore_vacuum_error_info(vacrel, &saved_err_info);
	pfree(vacrel->indname);
	vacrel->indname = NULL;

	return istat;
}

/*
 * should_attempt_truncation - should we attempt to truncate the heap?
 *
 * Don't even think about it unless we have a shot at releasing a goodly
 * number of pages.  Otherwise, the time taken isn't worth it.
 *
 * Also don't attempt it if we are doing early pruning/vacuuming, because a
 * scan which cannot find a truncated heap page cannot determine that the
 * snapshot is too old to read that page.  We might be able to get away with
 * truncating all except one of the pages, setting its LSN to (at least) the
 * maximum of the truncated range if we also treated an index leaf tuple
 * pointing to a missing heap page as something to trigger the "snapshot too
 * old" error, but that seems fragile and seems like it deserves its own patch
 * if we consider it.
 *
 * This is split out so that we can test whether truncation is going to be
 * called for before we actually do it.  If you change the logic here, be
 * careful to depend only on fields that lazy_scan_heap updates on-the-fly.
 */
static bool
should_attempt_truncation(LVRelState *vacrel, VacuumParams *params)
{
	BlockNumber possibly_freeable;

	if (params->truncate == VACOPT_TERNARY_DISABLED)
		return false;

	possibly_freeable = vacrel->rel_pages - vacrel->nonempty_pages;
	if (possibly_freeable > 0 &&
		(possibly_freeable >= REL_TRUNCATE_MINIMUM ||
		 possibly_freeable >= vacrel->rel_pages / REL_TRUNCATE_FRACTION) &&
		old_snapshot_threshold < 0)
		return true;
	else
		return false;
}

/*
 * lazy_truncate_heap - try to truncate off any empty pages at the end
 */
static void
lazy_truncate_heap(LVRelState *vacrel)
{
	BlockNumber old_rel_pages = vacrel->rel_pages;
	BlockNumber new_rel_pages;
	int			lock_retry;

	/* Report that we are now truncating */
	pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
								 PROGRESS_VACUUM_PHASE_TRUNCATE);

	/*
	 * Loop until no more truncating can be done.
	 */
	do
	{
		PGRUsage	ru0;

		pg_rusage_init(&ru0);

		/*
		 * We need full exclusive lock on the relation in order to do
		 * truncation. If we can't get it, give up rather than waiting --- we
		 * don't want to block other backends, and we don't want to deadlock
		 * (which is quite possible considering we already hold a lower-grade
		 * lock).
		 */
		vacrel->lock_waiter_detected = false;
		lock_retry = 0;
		while (true)
		{
			if (ConditionalLockRelation(vacrel->rel, AccessExclusiveLock))
				break;

			/*
			 * Check for interrupts while trying to (re-)acquire the exclusive
			 * lock.
			 */
			CHECK_FOR_INTERRUPTS();

			if (++lock_retry > (VACUUM_TRUNCATE_LOCK_TIMEOUT /
								VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL))
			{
				/*
				 * We failed to establish the lock in the specified number of
				 * retries. This means we give up truncating.
				 */
				vacrel->lock_waiter_detected = true;
				ereport(elevel,
						(errmsg("\"%s\": stopping truncate due to conflicting lock request",
								vacrel->relname)));
				return;
			}

			pg_usleep(VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL * 1000L);
		}

		/*
		 * Now that we have exclusive lock, look to see if the rel has grown
		 * whilst we were vacuuming with non-exclusive lock.  If so, give up;
		 * the newly added pages presumably contain non-deletable tuples.
		 */
		new_rel_pages = RelationGetNumberOfBlocks(vacrel->rel);
		if (new_rel_pages != old_rel_pages)
		{
			/*
			 * Note: we intentionally don't update vacrel->rel_pages with the
			 * new rel size here.  If we did, it would amount to assuming that
			 * the new pages are empty, which is unlikely. Leaving the numbers
			 * alone amounts to assuming that the new pages have the same
			 * tuple density as existing ones, which is less unlikely.
			 */
			UnlockRelation(vacrel->rel, AccessExclusiveLock);
			return;
		}

		/*
		 * Scan backwards from the end to verify that the end pages actually
		 * contain no tuples.  This is *necessary*, not optional, because
		 * other backends could have added tuples to these pages whilst we
		 * were vacuuming.
		 */
		new_rel_pages = count_nondeletable_pages(vacrel);
		vacrel->blkno = new_rel_pages;

		if (new_rel_pages >= old_rel_pages)
		{
			/* can't do anything after all */
			UnlockRelation(vacrel->rel, AccessExclusiveLock);
			return;
		}

		/*
		 * Okay to truncate.
		 */
		RelationTruncate(vacrel->rel, new_rel_pages);

		/*
		 * We can release the exclusive lock as soon as we have truncated.
		 * Other backends can't safely access the relation until they have
		 * processed the smgr invalidation that smgrtruncate sent out ... but
		 * that should happen as part of standard invalidation processing once
		 * they acquire lock on the relation.
		 */
		UnlockRelation(vacrel->rel, AccessExclusiveLock);

		/*
		 * Update statistics.  Here, it *is* correct to adjust rel_pages
		 * without also touching reltuples, since the tuple count wasn't
		 * changed by the truncation.
		 */
		vacrel->pages_removed += old_rel_pages - new_rel_pages;
		vacrel->rel_pages = new_rel_pages;

		ereport(elevel,
				(errmsg("\"%s\": truncated %u to %u pages",
						vacrel->relname,
						old_rel_pages, new_rel_pages),
				 errdetail_internal("%s",
									pg_rusage_show(&ru0))));
		old_rel_pages = new_rel_pages;
	} while (new_rel_pages > vacrel->nonempty_pages &&
			 vacrel->lock_waiter_detected);
}

/*
 * Rescan end pages to verify that they are (still) empty of tuples.
 *
 * Returns number of nondeletable pages (last nonempty page + 1).
 */
static BlockNumber
count_nondeletable_pages(LVRelState *vacrel)
{
	BlockNumber blkno;
	BlockNumber prefetchedUntil;
	instr_time	starttime;

	/* Initialize the starttime if we check for conflicting lock requests */
	INSTR_TIME_SET_CURRENT(starttime);

	/*
	 * Start checking blocks at what we believe relation end to be and move
	 * backwards.  (Strange coding of loop control is needed because blkno is
	 * unsigned.)  To make the scan faster, we prefetch a few blocks at a time
	 * in forward direction, so that OS-level readahead can kick in.
	 */
	blkno = vacrel->rel_pages;
	StaticAssertStmt((PREFETCH_SIZE & (PREFETCH_SIZE - 1)) == 0,
					 "prefetch size must be power of 2");
	prefetchedUntil = InvalidBlockNumber;
	while (blkno > vacrel->nonempty_pages)
	{
		Buffer		buf;
		Page		page;
		OffsetNumber offnum,
					maxoff;
		bool		hastup;

		/*
		 * Check if another process requests a lock on our relation. We are
		 * holding an AccessExclusiveLock here, so they will be waiting. We
		 * only do this once per VACUUM_TRUNCATE_LOCK_CHECK_INTERVAL, and we
		 * only check if that interval has elapsed once every 32 blocks to
		 * keep the number of system calls and actual shared lock table
		 * lookups to a minimum.
		 */
		if ((blkno % 32) == 0)
		{
			instr_time	currenttime;
			instr_time	elapsed;

			INSTR_TIME_SET_CURRENT(currenttime);
			elapsed = currenttime;
			INSTR_TIME_SUBTRACT(elapsed, starttime);
			if ((INSTR_TIME_GET_MICROSEC(elapsed) / 1000)
				>= VACUUM_TRUNCATE_LOCK_CHECK_INTERVAL)
			{
				if (LockHasWaitersRelation(vacrel->rel, AccessExclusiveLock))
				{
					ereport(elevel,
							(errmsg("\"%s\": suspending truncate due to conflicting lock request",
									vacrel->relname)));

					vacrel->lock_waiter_detected = true;
					return blkno;
				}
				starttime = currenttime;
			}
		}

		/*
		 * We don't insert a vacuum delay point here, because we have an
		 * exclusive lock on the table which we want to hold for as short a
		 * time as possible.  We still need to check for interrupts however.
		 */
		CHECK_FOR_INTERRUPTS();

		blkno--;

		/* If we haven't prefetched this lot yet, do so now. */
		if (prefetchedUntil > blkno)
		{
			BlockNumber prefetchStart;
			BlockNumber pblkno;

			prefetchStart = blkno & ~(PREFETCH_SIZE - 1);
			for (pblkno = prefetchStart; pblkno <= blkno; pblkno++)
			{
				PrefetchBuffer(vacrel->rel, MAIN_FORKNUM, pblkno);
				CHECK_FOR_INTERRUPTS();
			}
			prefetchedUntil = prefetchStart;
		}

		buf = ReadBufferExtended(vacrel->rel, MAIN_FORKNUM, blkno, RBM_NORMAL,
								 vacrel->bstrategy);

		/* In this phase we only need shared access to the buffer */
		LockBuffer(buf, BUFFER_LOCK_SHARE);

		page = BufferGetPage(buf);

		if (PageIsNew(page) || PageIsEmpty(page))
		{
			UnlockReleaseBuffer(buf);
			continue;
		}

		hastup = false;
		maxoff = PageGetMaxOffsetNumber(page);
		for (offnum = FirstOffsetNumber;
			 offnum <= maxoff;
			 offnum = OffsetNumberNext(offnum))
		{
			ItemId		itemid;

			itemid = PageGetItemId(page, offnum);

			/*
			 * Note: any non-unused item should be taken as a reason to keep
			 * this page.  We formerly thought that DEAD tuples could be
			 * thrown away, but that's not so, because we'd not have cleaned
			 * out their index entries.
			 */
			if (ItemIdIsUsed(itemid))
			{
				hastup = true;
				break;			/* can stop scanning */
			}
		}						/* scan along page */

		UnlockReleaseBuffer(buf);

		/* Done scanning if we found a tuple here */
		if (hastup)
			return blkno + 1;
	}

	/*
	 * If we fall out of the loop, all the previously-thought-to-be-empty
	 * pages still are; we need not bother to look at the last known-nonempty
	 * page.
	 */
	return vacrel->nonempty_pages;
}

/*
 * Return the maximum number of dead tuples we can record.
 */
static long
compute_max_dead_tuples(BlockNumber relblocks, bool hasindex)
{
	long		maxtuples;
	int			vac_work_mem = IsAutoVacuumWorkerProcess() &&
	autovacuum_work_mem != -1 ?
	autovacuum_work_mem : maintenance_work_mem;

	if (hasindex)
	{
		maxtuples = MAXDEADTUPLES(vac_work_mem * 1024L);
		maxtuples = Min(maxtuples, INT_MAX);
		maxtuples = Min(maxtuples, MAXDEADTUPLES(MaxAllocSize));

		/* curious coding here to ensure the multiplication can't overflow */
		if ((BlockNumber) (maxtuples / LAZY_ALLOC_TUPLES) > relblocks)
			maxtuples = relblocks * LAZY_ALLOC_TUPLES;

		/* stay sane if small maintenance_work_mem */
		maxtuples = Max(maxtuples, MaxHeapTuplesPerPage);
	}
	else
		maxtuples = MaxHeapTuplesPerPage;

	return maxtuples;
}

/*
 * lazy_space_alloc - space allocation decisions for lazy vacuum
 *
 * See the comments at the head of this file for rationale.
 */
static void
lazy_space_alloc(LVRelState *vacrel, int nworkers, BlockNumber nblocks)
{
	LVDeadTuples *dead_tuples;
	long		maxtuples;

	/*
	 * Initialize state for a parallel vacuum.  As of now, only one worker can
	 * be used for an index, so we invoke parallelism only if there are at
	 * least two indexes on a table.
	 */
	if (nworkers >= 0 && vacrel->nindexes > 1 && vacrel->do_index_vacuuming)
	{
		/*
		 * Since parallel workers cannot access data in temporary tables, we
		 * can't perform parallel vacuum on them.
		 */
		if (RelationUsesLocalBuffers(vacrel->rel))
		{
			/*
			 * Give warning only if the user explicitly tries to perform a
			 * parallel vacuum on the temporary table.
			 */
			if (nworkers > 0)
				ereport(WARNING,
						(errmsg("disabling parallel option of vacuum on \"%s\" --- cannot vacuum temporary tables in parallel",
								vacrel->relname)));
		}
		else
			vacrel->lps = begin_parallel_vacuum(vacrel, nblocks, nworkers);

		/* If parallel mode started, we're done */
		if (ParallelVacuumIsActive(vacrel))
			return;
	}

	maxtuples = compute_max_dead_tuples(nblocks, vacrel->nindexes > 0);

	dead_tuples = (LVDeadTuples *) palloc(SizeOfDeadTuples(maxtuples));
	dead_tuples->num_tuples = 0;
	dead_tuples->max_tuples = (int) maxtuples;

	vacrel->dead_tuples = dead_tuples;
}

/*
 * lazy_space_free - free space allocated in lazy_space_alloc
 */
static void
lazy_space_free(LVRelState *vacrel)
{
	if (!ParallelVacuumIsActive(vacrel))
		return;

	/*
	 * End parallel mode before updating index statistics as we cannot write
	 * during parallel mode.
	 */
	end_parallel_vacuum(vacrel);
}

/*
 *	lazy_tid_reaped() -- is a particular tid deletable?
 *
 *		This has the right signature to be an IndexBulkDeleteCallback.
 *
 *		Assumes dead_tuples array is in sorted order.
 */
static bool
lazy_tid_reaped(ItemPointer itemptr, void *state)
{
	LVDeadTuples *dead_tuples = (LVDeadTuples *) state;
	int64		litem,
				ritem,
				item;
	ItemPointer res;

	litem = itemptr_encode(&dead_tuples->itemptrs[0]);
	ritem = itemptr_encode(&dead_tuples->itemptrs[dead_tuples->num_tuples - 1]);
	item = itemptr_encode(itemptr);

	/*
	 * Doing a simple bound check before bsearch() is useful to avoid the
	 * extra cost of bsearch(), especially if dead tuples on the heap are
	 * concentrated in a certain range.  Since this function is called for
	 * every index tuple, it pays to be really fast.
	 */
	if (item < litem || item > ritem)
		return false;

	res = (ItemPointer) bsearch((void *) itemptr,
								(void *) dead_tuples->itemptrs,
								dead_tuples->num_tuples,
								sizeof(ItemPointerData),
								vac_cmp_itemptr);

	return (res != NULL);
}

/*
 * Comparator routines for use with qsort() and bsearch().
 */
static int
vac_cmp_itemptr(const void *left, const void *right)
{
	BlockNumber lblk,
				rblk;
	OffsetNumber loff,
				roff;

	lblk = ItemPointerGetBlockNumber((ItemPointer) left);
	rblk = ItemPointerGetBlockNumber((ItemPointer) right);

	if (lblk < rblk)
		return -1;
	if (lblk > rblk)
		return 1;

	loff = ItemPointerGetOffsetNumber((ItemPointer) left);
	roff = ItemPointerGetOffsetNumber((ItemPointer) right);

	if (loff < roff)
		return -1;
	if (loff > roff)
		return 1;

	return 0;
}

/*
 * Check if every tuple in the given page is visible to all current and future
 * transactions. Also return the visibility_cutoff_xid which is the highest
 * xmin amongst the visible tuples.  Set *all_frozen to true if every tuple
 * on this page is frozen.
 */
static bool
heap_page_is_all_visible(LVRelState *vacrel, Buffer buf,
						 TransactionId *visibility_cutoff_xid,
						 bool *all_frozen)
{
	Page		page = BufferGetPage(buf);
	BlockNumber blockno = BufferGetBlockNumber(buf);
	OffsetNumber offnum,
				maxoff;
	bool		all_visible = true;

	*visibility_cutoff_xid = InvalidTransactionId;
	*all_frozen = true;

	/*
	 * This is a stripped down version of the line pointer scan in
	 * lazy_scan_heap(). So if you change anything here, also check that code.
	 */
	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = FirstOffsetNumber;
		 offnum <= maxoff && all_visible;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemid;
		HeapTupleData tuple;

		/*
		 * Set the offset number so that we can display it along with any
		 * error that occurred while processing this tuple.
		 */
		vacrel->offnum = offnum;
		itemid = PageGetItemId(page, offnum);

		/* Unused or redirect line pointers are of no interest */
		if (!ItemIdIsUsed(itemid) || ItemIdIsRedirected(itemid))
			continue;

		ItemPointerSet(&(tuple.t_self), blockno, offnum);

		/*
		 * Dead line pointers can have index pointers pointing to them. So
		 * they can't be treated as visible
		 */
		if (ItemIdIsDead(itemid))
		{
			all_visible = false;
			*all_frozen = false;
			break;
		}

		Assert(ItemIdIsNormal(itemid));

		tuple.t_data = (HeapTupleHeader) PageGetItem(page, itemid);
		tuple.t_len = ItemIdGetLength(itemid);
		tuple.t_tableOid = RelationGetRelid(vacrel->rel);

		switch (HeapTupleSatisfiesVacuum(&tuple, vacrel->OldestXmin, buf))
		{
			case HEAPTUPLE_LIVE:
				{
					TransactionId xmin;

					/* Check comments in lazy_scan_heap. */
					if (!HeapTupleHeaderXminCommitted(tuple.t_data))
					{
						all_visible = false;
						*all_frozen = false;
						break;
					}

					/*
					 * The inserter definitely committed. But is it old enough
					 * that everyone sees it as committed?
					 */
					xmin = HeapTupleHeaderGetXmin(tuple.t_data);
					if (!TransactionIdPrecedes(xmin, vacrel->OldestXmin))
					{
						all_visible = false;
						*all_frozen = false;
						break;
					}

					/* Track newest xmin on page. */
					if (TransactionIdFollows(xmin, *visibility_cutoff_xid))
						*visibility_cutoff_xid = xmin;

					/* Check whether this tuple is already frozen or not */
					if (all_visible && *all_frozen &&
						heap_tuple_needs_eventual_freeze(tuple.t_data))
						*all_frozen = false;
				}
				break;

			case HEAPTUPLE_DEAD:
			case HEAPTUPLE_RECENTLY_DEAD:
			case HEAPTUPLE_INSERT_IN_PROGRESS:
			case HEAPTUPLE_DELETE_IN_PROGRESS:
				{
					all_visible = false;
					*all_frozen = false;
					break;
				}
			default:
				elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
				break;
		}
	}							/* scan along page */

	/* Clear the offset information once we have processed the given page. */
	vacrel->offnum = InvalidOffsetNumber;

	return all_visible;
}

/*
 * Compute the number of parallel worker processes to request.  Both index
 * vacuum and index cleanup can be executed with parallel workers.  The index
 * is eligible for parallel vacuum iff its size is greater than
 * min_parallel_index_scan_size as invoking workers for very small indexes
 * can hurt performance.
 *
 * nrequested is the number of parallel workers that user requested.  If
 * nrequested is 0, we compute the parallel degree based on nindexes, that is
 * the number of indexes that support parallel vacuum.  This function also
 * sets can_parallel_vacuum to remember indexes that participate in parallel
 * vacuum.
 */
static int
compute_parallel_vacuum_workers(LVRelState *vacrel, int nrequested,
								bool *can_parallel_vacuum)
{
	int			nindexes_parallel = 0;
	int			nindexes_parallel_bulkdel = 0;
	int			nindexes_parallel_cleanup = 0;
	int			parallel_workers;

	/*
	 * We don't allow performing parallel operation in standalone backend or
	 * when parallelism is disabled.
	 */
	if (!IsUnderPostmaster || max_parallel_maintenance_workers == 0)
		return 0;

	/*
	 * Compute the number of indexes that can participate in parallel vacuum.
	 */
	for (int idx = 0; idx < vacrel->nindexes; idx++)
	{
		Relation	indrel = vacrel->indrels[idx];
		uint8		vacoptions = indrel->rd_indam->amparallelvacuumoptions;

		if (vacoptions == VACUUM_OPTION_NO_PARALLEL ||
			RelationGetNumberOfBlocks(indrel) < min_parallel_index_scan_size)
			continue;

		can_parallel_vacuum[idx] = true;

		if ((vacoptions & VACUUM_OPTION_PARALLEL_BULKDEL) != 0)
			nindexes_parallel_bulkdel++;
		if (((vacoptions & VACUUM_OPTION_PARALLEL_CLEANUP) != 0) ||
			((vacoptions & VACUUM_OPTION_PARALLEL_COND_CLEANUP) != 0))
			nindexes_parallel_cleanup++;
	}

	nindexes_parallel = Max(nindexes_parallel_bulkdel,
							nindexes_parallel_cleanup);

	/* The leader process takes one index */
	nindexes_parallel--;

	/* No index supports parallel vacuum */
	if (nindexes_parallel <= 0)
		return 0;

	/* Compute the parallel degree */
	parallel_workers = (nrequested > 0) ?
		Min(nrequested, nindexes_parallel) : nindexes_parallel;

	/* Cap by max_parallel_maintenance_workers */
	parallel_workers = Min(parallel_workers, max_parallel_maintenance_workers);

	return parallel_workers;
}

/*
 * Update index statistics in pg_class if the statistics are accurate.
 */
static void
update_index_statistics(LVRelState *vacrel)
{
	Relation   *indrels = vacrel->indrels;
	int			nindexes = vacrel->nindexes;
	IndexBulkDeleteResult **indstats = vacrel->indstats;

	Assert(!IsInParallelMode());

	for (int idx = 0; idx < nindexes; idx++)
	{
		Relation	indrel = indrels[idx];
		IndexBulkDeleteResult *istat = indstats[idx];

		if (istat == NULL || istat->estimated_count)
			continue;

		/* Update index statistics */
		vac_update_relstats(indrel,
							istat->num_pages,
							istat->num_index_tuples,
							0,
							false,
							InvalidTransactionId,
							InvalidMultiXactId,
							false);
	}
}

/*
 * This function prepares and returns parallel vacuum state if we can launch
 * even one worker.  This function is responsible for entering parallel mode,
 * create a parallel context, and then initialize the DSM segment.
 */
static LVParallelState *
begin_parallel_vacuum(LVRelState *vacrel, BlockNumber nblocks,
					  int nrequested)
{
	LVParallelState *lps = NULL;
	Relation   *indrels = vacrel->indrels;
	int			nindexes = vacrel->nindexes;
	ParallelContext *pcxt;
	LVShared   *shared;
	LVDeadTuples *dead_tuples;
	BufferUsage *buffer_usage;
	WalUsage   *wal_usage;
	bool	   *can_parallel_vacuum;
	long		maxtuples;
	Size		est_shared;
	Size		est_deadtuples;
	int			nindexes_mwm = 0;
	int			parallel_workers = 0;
	int			querylen;

	/*
	 * A parallel vacuum must be requested and there must be indexes on the
	 * relation
	 */
	Assert(nrequested >= 0);
	Assert(nindexes > 0);

	/*
	 * Compute the number of parallel vacuum workers to launch
	 */
	can_parallel_vacuum = (bool *) palloc0(sizeof(bool) * nindexes);
	parallel_workers = compute_parallel_vacuum_workers(vacrel,
													   nrequested,
													   can_parallel_vacuum);

	/* Can't perform vacuum in parallel */
	if (parallel_workers <= 0)
	{
		pfree(can_parallel_vacuum);
		return lps;
	}

	lps = (LVParallelState *) palloc0(sizeof(LVParallelState));

	EnterParallelMode();
	pcxt = CreateParallelContext("postgres", "parallel_vacuum_main",
								 parallel_workers);
	Assert(pcxt->nworkers > 0);
	lps->pcxt = pcxt;

	/* Estimate size for shared information -- PARALLEL_VACUUM_KEY_SHARED */
	est_shared = MAXALIGN(add_size(SizeOfLVShared, BITMAPLEN(nindexes)));
	for (int idx = 0; idx < nindexes; idx++)
	{
		Relation	indrel = indrels[idx];
		uint8		vacoptions = indrel->rd_indam->amparallelvacuumoptions;

		/*
		 * Cleanup option should be either disabled, always performing in
		 * parallel or conditionally performing in parallel.
		 */
		Assert(((vacoptions & VACUUM_OPTION_PARALLEL_CLEANUP) == 0) ||
			   ((vacoptions & VACUUM_OPTION_PARALLEL_COND_CLEANUP) == 0));
		Assert(vacoptions <= VACUUM_OPTION_MAX_VALID_VALUE);

		/* Skip indexes that don't participate in parallel vacuum */
		if (!can_parallel_vacuum[idx])
			continue;

		if (indrel->rd_indam->amusemaintenanceworkmem)
			nindexes_mwm++;

		est_shared = add_size(est_shared, sizeof(LVSharedIndStats));

		/*
		 * Remember the number of indexes that support parallel operation for
		 * each phase.
		 */
		if ((vacoptions & VACUUM_OPTION_PARALLEL_BULKDEL) != 0)
			lps->nindexes_parallel_bulkdel++;
		if ((vacoptions & VACUUM_OPTION_PARALLEL_CLEANUP) != 0)
			lps->nindexes_parallel_cleanup++;
		if ((vacoptions & VACUUM_OPTION_PARALLEL_COND_CLEANUP) != 0)
			lps->nindexes_parallel_condcleanup++;
	}
	shm_toc_estimate_chunk(&pcxt->estimator, est_shared);
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/* Estimate size for dead tuples -- PARALLEL_VACUUM_KEY_DEAD_TUPLES */
	maxtuples = compute_max_dead_tuples(nblocks, true);
	est_deadtuples = MAXALIGN(SizeOfDeadTuples(maxtuples));
	shm_toc_estimate_chunk(&pcxt->estimator, est_deadtuples);
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/*
	 * Estimate space for BufferUsage and WalUsage --
	 * PARALLEL_VACUUM_KEY_BUFFER_USAGE and PARALLEL_VACUUM_KEY_WAL_USAGE.
	 *
	 * If there are no extensions loaded that care, we could skip this.  We
	 * have no way of knowing whether anyone's looking at pgBufferUsage or
	 * pgWalUsage, so do it unconditionally.
	 */
	shm_toc_estimate_chunk(&pcxt->estimator,
						   mul_size(sizeof(BufferUsage), pcxt->nworkers));
	shm_toc_estimate_keys(&pcxt->estimator, 1);
	shm_toc_estimate_chunk(&pcxt->estimator,
						   mul_size(sizeof(WalUsage), pcxt->nworkers));
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/* Finally, estimate PARALLEL_VACUUM_KEY_QUERY_TEXT space */
	if (debug_query_string)
	{
		querylen = strlen(debug_query_string);
		shm_toc_estimate_chunk(&pcxt->estimator, querylen + 1);
		shm_toc_estimate_keys(&pcxt->estimator, 1);
	}
	else
		querylen = 0;			/* keep compiler quiet */

	InitializeParallelDSM(pcxt);

	/* Prepare shared information */
	shared = (LVShared *) shm_toc_allocate(pcxt->toc, est_shared);
	MemSet(shared, 0, est_shared);
	shared->relid = RelationGetRelid(vacrel->rel);
	shared->elevel = elevel;
	shared->maintenance_work_mem_worker =
		(nindexes_mwm > 0) ?
		maintenance_work_mem / Min(parallel_workers, nindexes_mwm) :
		maintenance_work_mem;

	pg_atomic_init_u32(&(shared->cost_balance), 0);
	pg_atomic_init_u32(&(shared->active_nworkers), 0);
	pg_atomic_init_u32(&(shared->idx), 0);
	shared->offset = MAXALIGN(add_size(SizeOfLVShared, BITMAPLEN(nindexes)));

	/*
	 * Initialize variables for shared index statistics, set NULL bitmap and
	 * the size of stats for each index.
	 */
	memset(shared->bitmap, 0x00, BITMAPLEN(nindexes));
	for (int idx = 0; idx < nindexes; idx++)
	{
		if (!can_parallel_vacuum[idx])
			continue;

		/* Set NOT NULL as this index does support parallelism */
		shared->bitmap[idx >> 3] |= 1 << (idx & 0x07);
	}

	shm_toc_insert(pcxt->toc, PARALLEL_VACUUM_KEY_SHARED, shared);
	lps->lvshared = shared;

	/* Prepare the dead tuple space */
	dead_tuples = (LVDeadTuples *) shm_toc_allocate(pcxt->toc, est_deadtuples);
	dead_tuples->max_tuples = maxtuples;
	dead_tuples->num_tuples = 0;
	MemSet(dead_tuples->itemptrs, 0, sizeof(ItemPointerData) * maxtuples);
	shm_toc_insert(pcxt->toc, PARALLEL_VACUUM_KEY_DEAD_TUPLES, dead_tuples);
	vacrel->dead_tuples = dead_tuples;

	/*
	 * Allocate space for each worker's BufferUsage and WalUsage; no need to
	 * initialize
	 */
	buffer_usage = shm_toc_allocate(pcxt->toc,
									mul_size(sizeof(BufferUsage), pcxt->nworkers));
	shm_toc_insert(pcxt->toc, PARALLEL_VACUUM_KEY_BUFFER_USAGE, buffer_usage);
	lps->buffer_usage = buffer_usage;
	wal_usage = shm_toc_allocate(pcxt->toc,
								 mul_size(sizeof(WalUsage), pcxt->nworkers));
	shm_toc_insert(pcxt->toc, PARALLEL_VACUUM_KEY_WAL_USAGE, wal_usage);
	lps->wal_usage = wal_usage;

	/* Store query string for workers */
	if (debug_query_string)
	{
		char	   *sharedquery;

		sharedquery = (char *) shm_toc_allocate(pcxt->toc, querylen + 1);
		memcpy(sharedquery, debug_query_string, querylen + 1);
		sharedquery[querylen] = '\0';
		shm_toc_insert(pcxt->toc,
					   PARALLEL_VACUUM_KEY_QUERY_TEXT, sharedquery);
	}

	pfree(can_parallel_vacuum);
	return lps;
}

/*
 * Destroy the parallel context, and end parallel mode.
 *
 * Since writes are not allowed during parallel mode, copy the
 * updated index statistics from DSM into local memory and then later use that
 * to update the index statistics.  One might think that we can exit from
 * parallel mode, update the index statistics and then destroy parallel
 * context, but that won't be safe (see ExitParallelMode).
 */
static void
end_parallel_vacuum(LVRelState *vacrel)
{
	IndexBulkDeleteResult **indstats = vacrel->indstats;
	LVParallelState *lps = vacrel->lps;
	int			nindexes = vacrel->nindexes;

	Assert(!IsParallelWorker());

	/* Copy the updated statistics */
	for (int idx = 0; idx < nindexes; idx++)
	{
		LVSharedIndStats *shared_istat;

		shared_istat = parallel_stats_for_idx(lps->lvshared, idx);

		/*
		 * Skip unused slot.  The statistics of this index are already stored
		 * in local memory.
		 */
		if (shared_istat == NULL)
			continue;

		if (shared_istat->updated)
		{
			indstats[idx] = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
			memcpy(indstats[idx], &(shared_istat->istat), sizeof(IndexBulkDeleteResult));
		}
		else
			indstats[idx] = NULL;
	}

	DestroyParallelContext(lps->pcxt);
	ExitParallelMode();

	/* Deactivate parallel vacuum */
	pfree(lps);
	vacrel->lps = NULL;
}

/*
 * Return shared memory statistics for index at offset 'getidx', if any
 */
static LVSharedIndStats *
parallel_stats_for_idx(LVShared *lvshared, int getidx)
{
	char	   *p;

	if (IndStatsIsNull(lvshared, getidx))
		return NULL;

	p = (char *) GetSharedIndStats(lvshared);
	for (int idx = 0; idx < getidx; idx++)
	{
		if (IndStatsIsNull(lvshared, idx))
			continue;

		p += sizeof(LVSharedIndStats);
	}

	return (LVSharedIndStats *) p;
}

/*
 * Returns false, if the given index can't participate in parallel index
 * vacuum or parallel index cleanup
 */
static bool
parallel_processing_is_safe(Relation indrel, LVShared *lvshared)
{
	uint8		vacoptions = indrel->rd_indam->amparallelvacuumoptions;

	/* first_time must be true only if for_cleanup is true */
	Assert(lvshared->for_cleanup || !lvshared->first_time);

	if (lvshared->for_cleanup)
	{
		/* Skip, if the index does not support parallel cleanup */
		if (((vacoptions & VACUUM_OPTION_PARALLEL_CLEANUP) == 0) &&
			((vacoptions & VACUUM_OPTION_PARALLEL_COND_CLEANUP) == 0))
			return true;

		/*
		 * Skip, if the index supports parallel cleanup conditionally, but we
		 * have already processed the index (for bulkdelete).  See the
		 * comments for option VACUUM_OPTION_PARALLEL_COND_CLEANUP to know
		 * when indexes support parallel cleanup conditionally.
		 */
		if (!lvshared->first_time &&
			((vacoptions & VACUUM_OPTION_PARALLEL_COND_CLEANUP) != 0))
			return false;
	}
	else if ((vacoptions & VACUUM_OPTION_PARALLEL_BULKDEL) == 0)
	{
		/* Skip if the index does not support parallel bulk deletion */
		return false;
	}

	return true;
}

/*
 * Perform work within a launched parallel process.
 *
 * Since parallel vacuum workers perform only index vacuum or index cleanup,
 * we don't need to report progress information.
 */
void
parallel_vacuum_main(dsm_segment *seg, shm_toc *toc)
{
	Relation	rel;
	Relation   *indrels;
	LVShared   *lvshared;
	LVDeadTuples *dead_tuples;
	BufferUsage *buffer_usage;
	WalUsage   *wal_usage;
	int			nindexes;
	char	   *sharedquery;
	LVRelState	vacrel;
	ErrorContextCallback errcallback;

	lvshared = (LVShared *) shm_toc_lookup(toc, PARALLEL_VACUUM_KEY_SHARED,
										   false);
	elevel = lvshared->elevel;

	if (lvshared->for_cleanup)
		elog(DEBUG1, "starting parallel vacuum worker for cleanup");
	else
		elog(DEBUG1, "starting parallel vacuum worker for bulk delete");

	/* Set debug_query_string for individual workers */
	sharedquery = shm_toc_lookup(toc, PARALLEL_VACUUM_KEY_QUERY_TEXT, true);
	debug_query_string = sharedquery;
	pgstat_report_activity(STATE_RUNNING, debug_query_string);

	/*
	 * Open table.  The lock mode is the same as the leader process.  It's
	 * okay because the lock mode does not conflict among the parallel
	 * workers.
	 */
	rel = table_open(lvshared->relid, ShareUpdateExclusiveLock);

	/*
	 * Open all indexes. indrels are sorted in order by OID, which should be
	 * matched to the leader's one.
	 */
	vac_open_indexes(rel, RowExclusiveLock, &nindexes, &indrels);
	Assert(nindexes > 0);

	/* Set dead tuple space */
	dead_tuples = (LVDeadTuples *) shm_toc_lookup(toc,
												  PARALLEL_VACUUM_KEY_DEAD_TUPLES,
												  false);

	/* Set cost-based vacuum delay */
	VacuumCostActive = (VacuumCostDelay > 0);
	VacuumCostBalance = 0;
	VacuumPageHit = 0;
	VacuumPageMiss = 0;
	VacuumPageDirty = 0;
	VacuumCostBalanceLocal = 0;
	VacuumSharedCostBalance = &(lvshared->cost_balance);
	VacuumActiveNWorkers = &(lvshared->active_nworkers);

	vacrel.rel = rel;
	vacrel.indrels = indrels;
	vacrel.nindexes = nindexes;
	/* Each parallel VACUUM worker gets its own access strategy */
	vacrel.bstrategy = GetAccessStrategy(BAS_VACUUM);
	vacrel.indstats = (IndexBulkDeleteResult **)
		palloc0(nindexes * sizeof(IndexBulkDeleteResult *));

	if (lvshared->maintenance_work_mem_worker > 0)
		maintenance_work_mem = lvshared->maintenance_work_mem_worker;

	/*
	 * Initialize vacrel for use as error callback arg by parallel worker.
	 */
	vacrel.relnamespace = get_namespace_name(RelationGetNamespace(rel));
	vacrel.relname = pstrdup(RelationGetRelationName(rel));
	vacrel.indname = NULL;
	vacrel.phase = VACUUM_ERRCB_PHASE_UNKNOWN;	/* Not yet processing */
	vacrel.dead_tuples = dead_tuples;

	/* Setup error traceback support for ereport() */
	errcallback.callback = vacuum_error_callback;
	errcallback.arg = &vacrel;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* Prepare to track buffer usage during parallel execution */
	InstrStartParallelQuery();

	/* Process indexes to perform vacuum/cleanup */
	do_parallel_processing(&vacrel, lvshared);

	/* Report buffer/WAL usage during parallel execution */
	buffer_usage = shm_toc_lookup(toc, PARALLEL_VACUUM_KEY_BUFFER_USAGE, false);
	wal_usage = shm_toc_lookup(toc, PARALLEL_VACUUM_KEY_WAL_USAGE, false);
	InstrEndParallelQuery(&buffer_usage[ParallelWorkerNumber],
						  &wal_usage[ParallelWorkerNumber]);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;

	vac_close_indexes(nindexes, indrels, RowExclusiveLock);
	table_close(rel, ShareUpdateExclusiveLock);
	FreeAccessStrategy(vacrel.bstrategy);
	pfree(vacrel.indstats);
}

/*
 * Error context callback for errors occurring during vacuum.
 */
static void
vacuum_error_callback(void *arg)
{
	LVRelState *errinfo = arg;

	switch (errinfo->phase)
	{
		case VACUUM_ERRCB_PHASE_SCAN_HEAP:
			if (BlockNumberIsValid(errinfo->blkno))
			{
				if (OffsetNumberIsValid(errinfo->offnum))
					errcontext("while scanning block %u and offset %u of relation \"%s.%s\"",
							   errinfo->blkno, errinfo->offnum, errinfo->relnamespace, errinfo->relname);
				else
					errcontext("while scanning block %u of relation \"%s.%s\"",
							   errinfo->blkno, errinfo->relnamespace, errinfo->relname);
			}
			else
				errcontext("while scanning relation \"%s.%s\"",
						   errinfo->relnamespace, errinfo->relname);
			break;

		case VACUUM_ERRCB_PHASE_VACUUM_HEAP:
			if (BlockNumberIsValid(errinfo->blkno))
			{
				if (OffsetNumberIsValid(errinfo->offnum))
					errcontext("while vacuuming block %u and offset %u of relation \"%s.%s\"",
							   errinfo->blkno, errinfo->offnum, errinfo->relnamespace, errinfo->relname);
				else
					errcontext("while vacuuming block %u of relation \"%s.%s\"",
							   errinfo->blkno, errinfo->relnamespace, errinfo->relname);
			}
			else
				errcontext("while vacuuming relation \"%s.%s\"",
						   errinfo->relnamespace, errinfo->relname);
			break;

		case VACUUM_ERRCB_PHASE_VACUUM_INDEX:
			errcontext("while vacuuming index \"%s\" of relation \"%s.%s\"",
					   errinfo->indname, errinfo->relnamespace, errinfo->relname);
			break;

		case VACUUM_ERRCB_PHASE_INDEX_CLEANUP:
			errcontext("while cleaning up index \"%s\" of relation \"%s.%s\"",
					   errinfo->indname, errinfo->relnamespace, errinfo->relname);
			break;

		case VACUUM_ERRCB_PHASE_TRUNCATE:
			if (BlockNumberIsValid(errinfo->blkno))
				errcontext("while truncating relation \"%s.%s\" to %u blocks",
						   errinfo->relnamespace, errinfo->relname, errinfo->blkno);
			break;

		case VACUUM_ERRCB_PHASE_UNKNOWN:
		default:
			return;				/* do nothing; the errinfo may not be
								 * initialized */
	}
}

/*
 * Updates the information required for vacuum error callback.  This also saves
 * the current information which can be later restored via restore_vacuum_error_info.
 */
static void
update_vacuum_error_info(LVRelState *vacrel, LVSavedErrInfo *saved_vacrel,
						 int phase, BlockNumber blkno, OffsetNumber offnum)
{
	if (saved_vacrel)
	{
		saved_vacrel->offnum = vacrel->offnum;
		saved_vacrel->blkno = vacrel->blkno;
		saved_vacrel->phase = vacrel->phase;
	}

	vacrel->blkno = blkno;
	vacrel->offnum = offnum;
	vacrel->phase = phase;
}

/*
 * Restores the vacuum information saved via a prior call to update_vacuum_error_info.
 */
static void
restore_vacuum_error_info(LVRelState *vacrel,
						  const LVSavedErrInfo *saved_vacrel)
{
	vacrel->blkno = saved_vacrel->blkno;
	vacrel->offnum = saved_vacrel->offnum;
	vacrel->phase = saved_vacrel->phase;
}
