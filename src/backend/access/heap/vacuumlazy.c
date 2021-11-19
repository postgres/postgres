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
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
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
#define ParallelVacuumIsActive(lps) PointerIsValid(lps)

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
	 * estimated_count is true if reltuples is an estimated value.
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
	IndexBulkDeleteResult stats;
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

typedef struct LVRelStats
{
	char	   *relnamespace;
	char	   *relname;
	/* useindex = true means two-pass strategy; false means one-pass */
	bool		useindex;
	/* Overall statistics about rel */
	BlockNumber old_rel_pages;	/* previous value of pg_class.relpages */
	BlockNumber rel_pages;		/* total number of pages */
	BlockNumber scanned_pages;	/* number of pages we examined */
	BlockNumber pinskipped_pages;	/* # of pages we skipped due to a pin */
	BlockNumber frozenskipped_pages;	/* # of frozen pages we skipped */
	BlockNumber tupcount_pages; /* pages whose tuples we counted */
	double		old_live_tuples;	/* previous value of pg_class.reltuples */
	double		new_rel_tuples; /* new estimated total # of tuples */
	double		new_live_tuples;	/* new estimated total # of live tuples */
	double		new_dead_tuples;	/* new estimated total # of dead tuples */
	BlockNumber pages_removed;
	double		tuples_deleted;
	BlockNumber nonempty_pages; /* actually, last nonempty page + 1 */
	LVDeadTuples *dead_tuples;
	int			num_index_scans;
	TransactionId latestRemovedXid;
	bool		lock_waiter_detected;

	/* Used for error callback */
	char	   *indname;
	BlockNumber blkno;			/* used only for heap operations */
	VacErrPhase phase;
} LVRelStats;

/* Struct for saving and restoring vacuum error information. */
typedef struct LVSavedErrInfo
{
	BlockNumber blkno;
	VacErrPhase phase;
} LVSavedErrInfo;

/* A few variables that don't seem worth passing around as parameters */
static int	elevel = -1;

static TransactionId OldestXmin;
static TransactionId FreezeLimit;
static MultiXactId MultiXactCutoff;

static BufferAccessStrategy vac_strategy;


/* non-export function prototypes */
static void lazy_scan_heap(Relation onerel, VacuumParams *params,
						   LVRelStats *vacrelstats, Relation *Irel, int nindexes,
						   bool aggressive);
static void lazy_vacuum_heap(Relation onerel, LVRelStats *vacrelstats);
static bool lazy_check_needs_freeze(Buffer buf, bool *hastup);
static void lazy_vacuum_all_indexes(Relation onerel, Relation *Irel,
									IndexBulkDeleteResult **stats,
									LVRelStats *vacrelstats, LVParallelState *lps,
									int nindexes);
static void lazy_vacuum_index(Relation indrel, IndexBulkDeleteResult **stats,
							  LVDeadTuples *dead_tuples, double reltuples, LVRelStats *vacrelstats);
static void lazy_cleanup_index(Relation indrel,
							   IndexBulkDeleteResult **stats,
							   double reltuples, bool estimated_count, LVRelStats *vacrelstats);
static int	lazy_vacuum_page(Relation onerel, BlockNumber blkno, Buffer buffer,
							 int tupindex, LVRelStats *vacrelstats, Buffer *vmbuffer);
static bool should_attempt_truncation(VacuumParams *params,
									  LVRelStats *vacrelstats);
static void lazy_truncate_heap(Relation onerel, LVRelStats *vacrelstats);
static BlockNumber count_nondeletable_pages(Relation onerel,
											LVRelStats *vacrelstats);
static void lazy_space_alloc(LVRelStats *vacrelstats, BlockNumber relblocks);
static void lazy_record_dead_tuple(LVDeadTuples *dead_tuples,
								   ItemPointer itemptr);
static bool lazy_tid_reaped(ItemPointer itemptr, void *state);
static int	vac_cmp_itemptr(const void *left, const void *right);
static bool heap_page_is_all_visible(Relation rel, Buffer buf,
									 TransactionId *visibility_cutoff_xid, bool *all_frozen);
static void lazy_parallel_vacuum_indexes(Relation *Irel, IndexBulkDeleteResult **stats,
										 LVRelStats *vacrelstats, LVParallelState *lps,
										 int nindexes);
static void parallel_vacuum_index(Relation *Irel, IndexBulkDeleteResult **stats,
								  LVShared *lvshared, LVDeadTuples *dead_tuples,
								  int nindexes, LVRelStats *vacrelstats);
static void vacuum_indexes_leader(Relation *Irel, IndexBulkDeleteResult **stats,
								  LVRelStats *vacrelstats, LVParallelState *lps,
								  int nindexes);
static void vacuum_one_index(Relation indrel, IndexBulkDeleteResult **stats,
							 LVShared *lvshared, LVSharedIndStats *shared_indstats,
							 LVDeadTuples *dead_tuples, LVRelStats *vacrelstats);
static void lazy_cleanup_all_indexes(Relation *Irel, IndexBulkDeleteResult **stats,
									 LVRelStats *vacrelstats, LVParallelState *lps,
									 int nindexes);
static long compute_max_dead_tuples(BlockNumber relblocks, bool hasindex);
static int	compute_parallel_vacuum_workers(Relation *Irel, int nindexes, int nrequested,
											bool *can_parallel_vacuum);
static void prepare_index_statistics(LVShared *lvshared, bool *can_parallel_vacuum,
									 int nindexes);
static void update_index_statistics(Relation *Irel, IndexBulkDeleteResult **stats,
									int nindexes);
static LVParallelState *begin_parallel_vacuum(Oid relid, Relation *Irel,
											  LVRelStats *vacrelstats, BlockNumber nblocks,
											  int nindexes, int nrequested);
static void end_parallel_vacuum(IndexBulkDeleteResult **stats,
								LVParallelState *lps, int nindexes);
static LVSharedIndStats *get_indstats(LVShared *lvshared, int n);
static bool skip_parallel_vacuum_index(Relation indrel, LVShared *lvshared);
static void vacuum_error_callback(void *arg);
static void update_vacuum_error_info(LVRelStats *errinfo, LVSavedErrInfo *saved_err_info,
									 int phase, BlockNumber blkno);
static void restore_vacuum_error_info(LVRelStats *errinfo, const LVSavedErrInfo *saved_err_info);


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
heap_vacuum_rel(Relation onerel, VacuumParams *params,
				BufferAccessStrategy bstrategy)
{
	LVRelStats *vacrelstats;
	Relation   *Irel;
	int			nindexes;
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
	TransactionId xidFullScanLimit;
	MultiXactId mxactFullScanLimit;
	BlockNumber new_rel_pages;
	BlockNumber new_rel_allvisible;
	double		new_live_tuples;
	TransactionId new_frozen_xid;
	MultiXactId new_min_multi;
	ErrorContextCallback errcallback;

	Assert(params != NULL);
	Assert(params->index_cleanup != VACOPT_TERNARY_DEFAULT);
	Assert(params->truncate != VACOPT_TERNARY_DEFAULT);

	/* not every AM requires these to be valid, but heap does */
	Assert(TransactionIdIsNormal(onerel->rd_rel->relfrozenxid));
	Assert(MultiXactIdIsValid(onerel->rd_rel->relminmxid));

	/* measure elapsed time iff autovacuum logging requires it */
	if (IsAutoVacuumWorkerProcess() && params->log_min_duration >= 0)
	{
		pg_rusage_init(&ru0);
		starttime = GetCurrentTimestamp();
	}

	if (params->options & VACOPT_VERBOSE)
		elevel = INFO;
	else
		elevel = DEBUG2;

	pgstat_progress_start_command(PROGRESS_COMMAND_VACUUM,
								  RelationGetRelid(onerel));

	vac_strategy = bstrategy;

	vacuum_set_xid_limits(onerel,
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
	aggressive = TransactionIdPrecedesOrEquals(onerel->rd_rel->relfrozenxid,
											   xidFullScanLimit);
	aggressive |= MultiXactIdPrecedesOrEquals(onerel->rd_rel->relminmxid,
											  mxactFullScanLimit);
	if (params->options & VACOPT_DISABLE_PAGE_SKIPPING)
		aggressive = true;

	vacrelstats = (LVRelStats *) palloc0(sizeof(LVRelStats));

	vacrelstats->relnamespace = get_namespace_name(RelationGetNamespace(onerel));
	vacrelstats->relname = pstrdup(RelationGetRelationName(onerel));
	vacrelstats->indname = NULL;
	vacrelstats->phase = VACUUM_ERRCB_PHASE_UNKNOWN;
	vacrelstats->old_rel_pages = onerel->rd_rel->relpages;
	vacrelstats->old_live_tuples = onerel->rd_rel->reltuples;
	vacrelstats->num_index_scans = 0;
	vacrelstats->pages_removed = 0;
	vacrelstats->lock_waiter_detected = false;

	/* Open all indexes of the relation */
	vac_open_indexes(onerel, RowExclusiveLock, &nindexes, &Irel);
	vacrelstats->useindex = (nindexes > 0 &&
							 params->index_cleanup == VACOPT_TERNARY_ENABLED);

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
	errcallback.arg = vacrelstats;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* Do the vacuuming */
	lazy_scan_heap(onerel, params, vacrelstats, Irel, nindexes, aggressive);

	/* Done with indexes */
	vac_close_indexes(nindexes, Irel, NoLock);

	/*
	 * Compute whether we actually scanned the all unfrozen pages. If we did,
	 * we can adjust relfrozenxid and relminmxid.
	 *
	 * NB: We need to check this before truncating the relation, because that
	 * will change ->rel_pages.
	 */
	if ((vacrelstats->scanned_pages + vacrelstats->frozenskipped_pages)
		< vacrelstats->rel_pages)
	{
		Assert(!aggressive);
		scanned_all_unfrozen = false;
	}
	else
		scanned_all_unfrozen = true;

	/*
	 * Optionally truncate the relation.
	 */
	if (should_attempt_truncation(params, vacrelstats))
	{
		/*
		 * Update error traceback information.  This is the last phase during
		 * which we add context information to errors, so we don't need to
		 * revert to the previous phase.
		 */
		update_vacuum_error_info(vacrelstats, NULL, VACUUM_ERRCB_PHASE_TRUNCATE,
								 vacrelstats->nonempty_pages);
		lazy_truncate_heap(onerel, vacrelstats);
	}

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;

	/* Report that we are now doing final cleanup */
	pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
								 PROGRESS_VACUUM_PHASE_FINAL_CLEANUP);

	/*
	 * Update statistics in pg_class.
	 *
	 * A corner case here is that if we scanned no pages at all because every
	 * page is all-visible, we should not update relpages/reltuples, because
	 * we have no new information to contribute.  In particular this keeps us
	 * from replacing relpages=reltuples=0 (which means "unknown tuple
	 * density") with nonzero relpages and reltuples=0 (which means "zero
	 * tuple density") unless there's some actual evidence for the latter.
	 *
	 * It's important that we use tupcount_pages and not scanned_pages for the
	 * check described above; scanned_pages counts pages where we could not
	 * get cleanup lock, and which were processed only for frozenxid purposes.
	 *
	 * We do update relallvisible even in the corner case, since if the table
	 * is all-visible we'd definitely like to know that.  But clamp the value
	 * to be not more than what we're setting relpages to.
	 *
	 * Also, don't change relfrozenxid/relminmxid if we skipped any pages,
	 * since then we don't know for certain that all tuples have a newer xmin.
	 */
	new_rel_pages = vacrelstats->rel_pages;
	new_live_tuples = vacrelstats->new_live_tuples;
	if (vacrelstats->tupcount_pages == 0 && new_rel_pages > 0)
	{
		new_rel_pages = vacrelstats->old_rel_pages;
		new_live_tuples = vacrelstats->old_live_tuples;
	}

	visibilitymap_count(onerel, &new_rel_allvisible, NULL);
	if (new_rel_allvisible > new_rel_pages)
		new_rel_allvisible = new_rel_pages;

	new_frozen_xid = scanned_all_unfrozen ? FreezeLimit : InvalidTransactionId;
	new_min_multi = scanned_all_unfrozen ? MultiXactCutoff : InvalidMultiXactId;

	vac_update_relstats(onerel,
						new_rel_pages,
						new_live_tuples,
						new_rel_allvisible,
						nindexes > 0,
						new_frozen_xid,
						new_min_multi,
						false);

	/* report results to the stats collector, too */
	pgstat_report_vacuum(RelationGetRelid(onerel),
						 onerel->rd_rel->relisshared,
						 new_live_tuples,
						 vacrelstats->new_dead_tuples);
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
							 vacrelstats->relnamespace,
							 vacrelstats->relname,
							 vacrelstats->num_index_scans);
			appendStringInfo(&buf, _("pages: %u removed, %u remain, %u skipped due to pins, %u skipped frozen\n"),
							 vacrelstats->pages_removed,
							 vacrelstats->rel_pages,
							 vacrelstats->pinskipped_pages,
							 vacrelstats->frozenskipped_pages);
			appendStringInfo(&buf,
							 _("tuples: %.0f removed, %.0f remain, %.0f are dead but not yet removable, oldest xmin: %u\n"),
							 vacrelstats->tuples_deleted,
							 vacrelstats->new_rel_tuples,
							 vacrelstats->new_dead_tuples,
							 OldestXmin);
			appendStringInfo(&buf,
							 _("buffer usage: %lld hits, %lld misses, %lld dirtied\n"),
							 (long long) VacuumPageHit,
							 (long long) VacuumPageMiss,
							 (long long) VacuumPageDirty);
			appendStringInfo(&buf, _("avg read rate: %.3f MB/s, avg write rate: %.3f MB/s\n"),
							 read_rate, write_rate);
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
}

/*
 * For Hot Standby we need to know the highest transaction id that will
 * be removed by any change. VACUUM proceeds in a number of passes so
 * we need to consider how each pass operates. The first phase runs
 * heap_page_prune(), which can issue XLOG_HEAP2_CLEAN records as it
 * progresses - these will have a latestRemovedXid on each record.
 * In some cases this removes all of the tuples to be removed, though
 * often we have dead tuples with index pointers so we must remember them
 * for removal in phase 3. Index records for those rows are removed
 * in phase 2 and index blocks do not have MVCC information attached.
 * So before we can allow removal of any index tuples we need to issue
 * a WAL record containing the latestRemovedXid of rows that will be
 * removed in phase three. This allows recovery queries to block at the
 * correct place, i.e. before phase two, rather than during phase three
 * which would be after the rows have become inaccessible.
 */
static void
vacuum_log_cleanup_info(Relation rel, LVRelStats *vacrelstats)
{
	/*
	 * Skip this for relations for which no WAL is to be written, or if we're
	 * not trying to support archive recovery.
	 */
	if (!RelationNeedsWAL(rel) || !XLogIsNeeded())
		return;

	/*
	 * No need to write the record at all unless it contains a valid value
	 */
	if (TransactionIdIsValid(vacrelstats->latestRemovedXid))
		(void) log_heap_cleanup_info(rel->rd_node, vacrelstats->latestRemovedXid);
}

/*
 *	lazy_scan_heap() -- scan an open heap relation
 *
 *		This routine prunes each page in the heap, which will among other
 *		things truncate dead tuples to dead line pointers, defragment the
 *		page, and set commit status bits (see heap_page_prune).  It also builds
 *		lists of dead tuples and pages with free space, calculates statistics
 *		on the number of live tuples in the heap, and marks pages as
 *		all-visible if appropriate.  When done, or when we run low on space for
 *		dead-tuple TIDs, invoke vacuuming of indexes and call lazy_vacuum_heap
 *		to reclaim dead line pointers.
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
lazy_scan_heap(Relation onerel, VacuumParams *params, LVRelStats *vacrelstats,
			   Relation *Irel, int nindexes, bool aggressive)
{
	LVParallelState *lps = NULL;
	LVDeadTuples *dead_tuples;
	BlockNumber nblocks,
				blkno;
	HeapTupleData tuple;
	TransactionId relfrozenxid = onerel->rd_rel->relfrozenxid;
	TransactionId relminmxid = onerel->rd_rel->relminmxid;
	BlockNumber empty_pages,
				vacuumed_pages,
				next_fsm_block_to_vacuum;
	double		num_tuples,		/* total number of nonremovable tuples */
				live_tuples,	/* live tuples (reltuples estimate) */
				tups_vacuumed,	/* tuples cleaned up by vacuum */
				nkeep,			/* dead-but-not-removable tuples */
				nunused;		/* unused line pointers */
	IndexBulkDeleteResult **indstats;
	int			i;
	PGRUsage	ru0;
	Buffer		vmbuffer = InvalidBuffer;
	BlockNumber next_unskippable_block;
	bool		skipping_blocks;
	xl_heap_freeze_tuple *frozen;
	StringInfoData buf;
	const int	initprog_index[] = {
		PROGRESS_VACUUM_PHASE,
		PROGRESS_VACUUM_TOTAL_HEAP_BLKS,
		PROGRESS_VACUUM_MAX_DEAD_TUPLES
	};
	int64		initprog_val[3];

	pg_rusage_init(&ru0);

	if (aggressive)
		ereport(elevel,
				(errmsg("aggressively vacuuming \"%s.%s\"",
						vacrelstats->relnamespace,
						vacrelstats->relname)));
	else
		ereport(elevel,
				(errmsg("vacuuming \"%s.%s\"",
						vacrelstats->relnamespace,
						vacrelstats->relname)));

	empty_pages = vacuumed_pages = 0;
	next_fsm_block_to_vacuum = (BlockNumber) 0;
	num_tuples = live_tuples = tups_vacuumed = nkeep = nunused = 0;

	indstats = (IndexBulkDeleteResult **)
		palloc0(nindexes * sizeof(IndexBulkDeleteResult *));

	nblocks = RelationGetNumberOfBlocks(onerel);
	vacrelstats->rel_pages = nblocks;
	vacrelstats->scanned_pages = 0;
	vacrelstats->tupcount_pages = 0;
	vacrelstats->nonempty_pages = 0;
	vacrelstats->latestRemovedXid = InvalidTransactionId;

	/*
	 * Initialize state for a parallel vacuum.  As of now, only one worker can
	 * be used for an index, so we invoke parallelism only if there are at
	 * least two indexes on a table.
	 */
	if (params->nworkers >= 0 && vacrelstats->useindex && nindexes > 1)
	{
		/*
		 * Since parallel workers cannot access data in temporary tables, we
		 * can't perform parallel vacuum on them.
		 */
		if (RelationUsesLocalBuffers(onerel))
		{
			/*
			 * Give warning only if the user explicitly tries to perform a
			 * parallel vacuum on the temporary table.
			 */
			if (params->nworkers > 0)
				ereport(WARNING,
						(errmsg("disabling parallel option of vacuum on \"%s\" --- cannot vacuum temporary tables in parallel",
								vacrelstats->relname)));
		}
		else
			lps = begin_parallel_vacuum(RelationGetRelid(onerel), Irel,
										vacrelstats, nblocks, nindexes,
										params->nworkers);
	}

	/*
	 * Allocate the space for dead tuples in case parallel vacuum is not
	 * initialized.
	 */
	if (!ParallelVacuumIsActive(lps))
		lazy_space_alloc(vacrelstats, nblocks);

	dead_tuples = vacrelstats->dead_tuples;
	frozen = palloc(sizeof(xl_heap_freeze_tuple) * MaxHeapTuplesPerPage);

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
	next_unskippable_block = 0;
	if ((params->options & VACOPT_DISABLE_PAGE_SKIPPING) == 0)
	{
		while (next_unskippable_block < nblocks)
		{
			uint8		vmstatus;

			vmstatus = visibilitymap_get_status(onerel, next_unskippable_block,
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
		OffsetNumber offnum,
					maxoff;
		bool		tupgone,
					hastup;
		int			prev_dead_count;
		int			nfrozen;
		Size		freespace;
		bool		all_visible_according_to_vm = false;
		bool		all_visible;
		bool		all_frozen = true;	/* provided all_visible is also true */
		bool		has_dead_tuples;
		TransactionId visibility_cutoff_xid = InvalidTransactionId;

		/* see note above about forcing scanning of last page */
#define FORCE_CHECK_PAGE() \
		(blkno == nblocks - 1 && should_attempt_truncation(params, vacrelstats))

		pgstat_progress_update_param(PROGRESS_VACUUM_HEAP_BLKS_SCANNED, blkno);

		update_vacuum_error_info(vacrelstats, NULL, VACUUM_ERRCB_PHASE_SCAN_HEAP,
								 blkno);

		if (blkno == next_unskippable_block)
		{
			/* Time to advance next_unskippable_block */
			next_unskippable_block++;
			if ((params->options & VACOPT_DISABLE_PAGE_SKIPPING) == 0)
			{
				while (next_unskippable_block < nblocks)
				{
					uint8		vmskipflags;

					vmskipflags = visibilitymap_get_status(onerel,
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
			if (aggressive && VM_ALL_VISIBLE(onerel, blkno, &vmbuffer))
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
				if (aggressive || VM_ALL_FROZEN(onerel, blkno, &vmbuffer))
					vacrelstats->frozenskipped_pages++;
				continue;
			}
			all_visible_according_to_vm = true;
		}

		vacuum_delay_point();

		/*
		 * If we are close to overrunning the available space for dead-tuple
		 * TIDs, pause and do a cycle of vacuuming before we tackle this page.
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

			/* Work on all the indexes, then the heap */
			lazy_vacuum_all_indexes(onerel, Irel, indstats,
									vacrelstats, lps, nindexes);

			/* Remove tuples from heap */
			lazy_vacuum_heap(onerel, vacrelstats);

			/*
			 * Forget the now-vacuumed tuples, and press on, but be careful
			 * not to reset latestRemovedXid since we want that value to be
			 * valid.
			 */
			dead_tuples->num_tuples = 0;

			/*
			 * Vacuum the Free Space Map to make newly-freed space visible on
			 * upper-level FSM pages.  Note we have not yet processed blkno.
			 */
			FreeSpaceMapVacuumRange(onerel, next_fsm_block_to_vacuum, blkno);
			next_fsm_block_to_vacuum = blkno;

			/* Report that we are once again scanning the heap */
			pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
										 PROGRESS_VACUUM_PHASE_SCAN_HEAP);
		}

		/*
		 * Pin the visibility map page in case we need to mark the page
		 * all-visible.  In most cases this will be very cheap, because we'll
		 * already have the correct page pinned anyway.  However, it's
		 * possible that (a) next_unskippable_block is covered by a different
		 * VM page than the current block or (b) we released our pin and did a
		 * cycle of index vacuuming.
		 *
		 */
		visibilitymap_pin(onerel, blkno, &vmbuffer);

		buf = ReadBufferExtended(onerel, MAIN_FORKNUM, blkno,
								 RBM_NORMAL, vac_strategy);

		/* We need buffer cleanup lock so that we can prune HOT chains. */
		if (!ConditionalLockBufferForCleanup(buf))
		{
			/*
			 * If we're not performing an aggressive scan to guard against XID
			 * wraparound, and we don't want to forcibly check the page, then
			 * it's OK to skip vacuuming pages we get a lock conflict on. They
			 * will be dealt with in some future vacuum.
			 */
			if (!aggressive && !FORCE_CHECK_PAGE())
			{
				ReleaseBuffer(buf);
				vacrelstats->pinskipped_pages++;
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
			if (!lazy_check_needs_freeze(buf, &hastup))
			{
				UnlockReleaseBuffer(buf);
				vacrelstats->scanned_pages++;
				vacrelstats->pinskipped_pages++;
				if (hastup)
					vacrelstats->nonempty_pages = blkno + 1;
				continue;
			}
			if (!aggressive)
			{
				/*
				 * Here, we must not advance scanned_pages; that would amount
				 * to claiming that the page contains no freezable tuples.
				 */
				UnlockReleaseBuffer(buf);
				vacrelstats->pinskipped_pages++;
				if (hastup)
					vacrelstats->nonempty_pages = blkno + 1;
				continue;
			}
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			LockBufferForCleanup(buf);
			/* drop through to normal processing */
		}

		vacrelstats->scanned_pages++;
		vacrelstats->tupcount_pages++;

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

			empty_pages++;

			if (GetRecordedFreeSpace(onerel, blkno) == 0)
			{
				Size		freespace;

				freespace = BufferGetPageSize(buf) - SizeOfPageHeaderData;
				RecordPageWithFreeSpace(onerel, blkno, freespace);
			}
			continue;
		}

		if (PageIsEmpty(page))
		{
			empty_pages++;
			freespace = PageGetHeapFreeSpace(page);

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
				if (RelationNeedsWAL(onerel) &&
					PageGetLSN(page) == InvalidXLogRecPtr)
					log_newpage_buffer(buf, true);

				PageSetAllVisible(page);
				visibilitymap_set(onerel, blkno, buf, InvalidXLogRecPtr,
								  vmbuffer, InvalidTransactionId,
								  VISIBILITYMAP_ALL_VISIBLE | VISIBILITYMAP_ALL_FROZEN);
				END_CRIT_SECTION();
			}

			UnlockReleaseBuffer(buf);
			RecordPageWithFreeSpace(onerel, blkno, freespace);
			continue;
		}

		/*
		 * Prune all HOT-update chains in this page.
		 *
		 * We count tuples removed by the pruning step as removed by VACUUM.
		 */
		tups_vacuumed += heap_page_prune(onerel, buf, OldestXmin, false,
										 &vacrelstats->latestRemovedXid);

		/*
		 * Now scan the page to collect vacuumable items and check for tuples
		 * requiring freezing.
		 */
		all_visible = true;
		has_dead_tuples = false;
		nfrozen = 0;
		hastup = false;
		prev_dead_count = dead_tuples->num_tuples;
		maxoff = PageGetMaxOffsetNumber(page);

		/*
		 * Note: If you change anything in the loop below, also look at
		 * heap_page_is_all_visible to see if that needs to be changed.
		 */
		for (offnum = FirstOffsetNumber;
			 offnum <= maxoff;
			 offnum = OffsetNumberNext(offnum))
		{
			ItemId		itemid;

			itemid = PageGetItemId(page, offnum);

			/* Unused items require no processing, but we count 'em */
			if (!ItemIdIsUsed(itemid))
			{
				nunused += 1;
				continue;
			}

			/* Redirect items mustn't be touched */
			if (ItemIdIsRedirected(itemid))
			{
				hastup = true;	/* this page won't be truncatable */
				continue;
			}

			ItemPointerSet(&(tuple.t_self), blkno, offnum);

			/*
			 * DEAD line pointers are to be vacuumed normally; but we don't
			 * count them in tups_vacuumed, else we'd be double-counting (at
			 * least in the common case where heap_page_prune() just freed up
			 * a non-HOT tuple).
			 */
			if (ItemIdIsDead(itemid))
			{
				lazy_record_dead_tuple(dead_tuples, &(tuple.t_self));
				all_visible = false;
				continue;
			}

			Assert(ItemIdIsNormal(itemid));

			tuple.t_data = (HeapTupleHeader) PageGetItem(page, itemid);
			tuple.t_len = ItemIdGetLength(itemid);
			tuple.t_tableOid = RelationGetRelid(onerel);

			tupgone = false;

			/*
			 * The criteria for counting a tuple as live in this block need to
			 * match what analyze.c's acquire_sample_rows() does, otherwise
			 * VACUUM and ANALYZE may produce wildly different reltuples
			 * values, e.g. when there are many recently-dead tuples.
			 *
			 * The logic here is a bit simpler than acquire_sample_rows(), as
			 * VACUUM can't run inside a transaction block, which makes some
			 * cases impossible (e.g. in-progress insert from the same
			 * transaction).
			 */
			switch (HeapTupleSatisfiesVacuum(&tuple, OldestXmin, buf))
			{
				case HEAPTUPLE_DEAD:

					/*
					 * Ordinarily, DEAD tuples would have been removed by
					 * heap_page_prune(), but it's possible that the tuple
					 * state changed since heap_page_prune() looked.  In
					 * particular an INSERT_IN_PROGRESS tuple could have
					 * changed to DEAD if the inserter aborted.  So this
					 * cannot be considered an error condition.
					 *
					 * If the tuple is HOT-updated then it must only be
					 * removed by a prune operation; so we keep it just as if
					 * it were RECENTLY_DEAD.  Also, if it's a heap-only
					 * tuple, we choose to keep it, because it'll be a lot
					 * cheaper to get rid of it in the next pruning pass than
					 * to treat it like an indexed tuple. Finally, if index
					 * cleanup is disabled, the second heap pass will not
					 * execute, and the tuple will not get removed, so we must
					 * treat it like any other dead tuple that we choose to
					 * keep.
					 *
					 * If this were to happen for a tuple that actually needed
					 * to be deleted, we'd be in trouble, because it'd
					 * possibly leave a tuple below the relation's xmin
					 * horizon alive.  heap_prepare_freeze_tuple() is prepared
					 * to detect that case and abort the transaction,
					 * preventing corruption.
					 */
					if (HeapTupleIsHotUpdated(&tuple) ||
						HeapTupleIsHeapOnly(&tuple) ||
						params->index_cleanup == VACOPT_TERNARY_DISABLED)
						nkeep += 1;
					else
						tupgone = true; /* we can delete the tuple */
					all_visible = false;
					break;
				case HEAPTUPLE_LIVE:

					/*
					 * Count it as live.  Not only is this natural, but it's
					 * also what acquire_sample_rows() does.
					 */
					live_tuples += 1;

					/*
					 * Is the tuple definitely visible to all transactions?
					 *
					 * NB: Like with per-tuple hint bits, we can't set the
					 * PD_ALL_VISIBLE flag if the inserter committed
					 * asynchronously. See SetHintBits for more info. Check
					 * that the tuple is hinted xmin-committed because of
					 * that.
					 */
					if (all_visible)
					{
						TransactionId xmin;

						if (!HeapTupleHeaderXminCommitted(tuple.t_data))
						{
							all_visible = false;
							break;
						}

						/*
						 * The inserter definitely committed. But is it old
						 * enough that everyone sees it as committed?
						 */
						xmin = HeapTupleHeaderGetXmin(tuple.t_data);
						if (!TransactionIdPrecedes(xmin, OldestXmin))
						{
							all_visible = false;
							break;
						}

						/* Track newest xmin on page. */
						if (TransactionIdFollows(xmin, visibility_cutoff_xid))
							visibility_cutoff_xid = xmin;
					}
					break;
				case HEAPTUPLE_RECENTLY_DEAD:

					/*
					 * If tuple is recently deleted then we must not remove it
					 * from relation.
					 */
					nkeep += 1;
					all_visible = false;
					break;
				case HEAPTUPLE_INSERT_IN_PROGRESS:

					/*
					 * This is an expected case during concurrent vacuum.
					 *
					 * We do not count these rows as live, because we expect
					 * the inserting transaction to update the counters at
					 * commit, and we assume that will happen only after we
					 * report our results.  This assumption is a bit shaky,
					 * but it is what acquire_sample_rows() does, so be
					 * consistent.
					 */
					all_visible = false;
					break;
				case HEAPTUPLE_DELETE_IN_PROGRESS:
					/* This is an expected case during concurrent vacuum */
					all_visible = false;

					/*
					 * Count such rows as live.  As above, we assume the
					 * deleting transaction will commit and update the
					 * counters after we report.
					 */
					live_tuples += 1;
					break;
				default:
					elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
					break;
			}

			if (tupgone)
			{
				lazy_record_dead_tuple(dead_tuples, &(tuple.t_self));
				HeapTupleHeaderAdvanceLatestRemovedXid(tuple.t_data,
													   &vacrelstats->latestRemovedXid);
				tups_vacuumed += 1;
				has_dead_tuples = true;
			}
			else
			{
				bool		tuple_totally_frozen;

				num_tuples += 1;
				hastup = true;

				/*
				 * Each non-removable tuple must be checked to see if it needs
				 * freezing.  Note we already have exclusive buffer lock.
				 */
				if (heap_prepare_freeze_tuple(tuple.t_data,
											  relfrozenxid, relminmxid,
											  FreezeLimit, MultiXactCutoff,
											  &frozen[nfrozen],
											  &tuple_totally_frozen))
					frozen[nfrozen++].offset = offnum;

				if (!tuple_totally_frozen)
					all_frozen = false;
			}
		}						/* scan along page */

		/*
		 * If we froze any tuples, mark the buffer dirty, and write a WAL
		 * record recording the changes.  We must log the changes to be
		 * crash-safe against future truncation of CLOG.
		 */
		if (nfrozen > 0)
		{
			START_CRIT_SECTION();

			MarkBufferDirty(buf);

			/* execute collected freezes */
			for (i = 0; i < nfrozen; i++)
			{
				ItemId		itemid;
				HeapTupleHeader htup;

				itemid = PageGetItemId(page, frozen[i].offset);
				htup = (HeapTupleHeader) PageGetItem(page, itemid);

				heap_execute_freeze_tuple(htup, &frozen[i]);
			}

			/* Now WAL-log freezing if necessary */
			if (RelationNeedsWAL(onerel))
			{
				XLogRecPtr	recptr;

				recptr = log_heap_freeze(onerel, buf, FreezeLimit,
										 frozen, nfrozen);
				PageSetLSN(page, recptr);
			}

			END_CRIT_SECTION();
		}

		/*
		 * If there are no indexes we can vacuum the page right now instead of
		 * doing a second scan. Also we don't do that but forget dead tuples
		 * when index cleanup is disabled.
		 */
		if (!vacrelstats->useindex && dead_tuples->num_tuples > 0)
		{
			if (nindexes == 0)
			{
				/* Remove tuples from heap if the table has no index */
				lazy_vacuum_page(onerel, blkno, buf, 0, vacrelstats, &vmbuffer);
				vacuumed_pages++;
				has_dead_tuples = false;
			}
			else
			{
				/*
				 * Here, we have indexes but index cleanup is disabled.
				 * Instead of vacuuming the dead tuples on the heap, we just
				 * forget them.
				 *
				 * Note that vacrelstats->dead_tuples could have tuples which
				 * became dead after HOT-pruning but are not marked dead yet.
				 * We do not process them because it's a very rare condition,
				 * and the next vacuum will process them anyway.
				 */
				Assert(params->index_cleanup == VACOPT_TERNARY_DISABLED);
			}

			/*
			 * Forget the now-vacuumed tuples, and press on, but be careful
			 * not to reset latestRemovedXid since we want that value to be
			 * valid.
			 */
			dead_tuples->num_tuples = 0;

			/*
			 * Periodically do incremental FSM vacuuming to make newly-freed
			 * space visible on upper FSM pages.  Note: although we've cleaned
			 * the current block, we haven't yet updated its FSM entry (that
			 * happens further down), so passing end == blkno is correct.
			 */
			if (blkno - next_fsm_block_to_vacuum >= VACUUM_FSM_EVERY_PAGES)
			{
				FreeSpaceMapVacuumRange(onerel, next_fsm_block_to_vacuum,
										blkno);
				next_fsm_block_to_vacuum = blkno;
			}
		}

		freespace = PageGetHeapFreeSpace(page);

		/* mark page all-visible, if appropriate */
		if (all_visible && !all_visible_according_to_vm)
		{
			uint8		flags = VISIBILITYMAP_ALL_VISIBLE;

			if (all_frozen)
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
			visibilitymap_set(onerel, blkno, buf, InvalidXLogRecPtr,
							  vmbuffer, visibility_cutoff_xid, flags);
		}

		/*
		 * As of PostgreSQL 9.2, the visibility map bit should never be set if
		 * the page-level bit is clear.  However, it's possible that the bit
		 * got cleared after we checked it and before we took the buffer
		 * content lock, so we must recheck before jumping to the conclusion
		 * that something bad has happened.
		 */
		else if (all_visible_according_to_vm && !PageIsAllVisible(page)
				 && VM_ALL_VISIBLE(onerel, blkno, &vmbuffer))
		{
			elog(WARNING, "page is not marked all-visible but visibility map bit is set in relation \"%s\" page %u",
				 vacrelstats->relname, blkno);
			visibilitymap_clear(onerel, blkno, vmbuffer,
								VISIBILITYMAP_VALID_BITS);
		}

		/*
		 * It's possible for the value returned by GetOldestXmin() to move
		 * backwards, so it's not wrong for us to see tuples that appear to
		 * not be visible to everyone yet, while PD_ALL_VISIBLE is already
		 * set. The real safe xmin value never moves backwards, but
		 * GetOldestXmin() is conservative and sometimes returns a value
		 * that's unnecessarily small, so if we see that contradiction it just
		 * means that the tuples that we think are not visible to everyone yet
		 * actually are, and the PD_ALL_VISIBLE flag is correct.
		 *
		 * There should never be dead tuples on a page with PD_ALL_VISIBLE
		 * set, however.
		 */
		else if (PageIsAllVisible(page) && has_dead_tuples)
		{
			elog(WARNING, "page containing dead tuples is marked as all-visible in relation \"%s\" page %u",
				 vacrelstats->relname, blkno);
			PageClearAllVisible(page);
			MarkBufferDirty(buf);
			visibilitymap_clear(onerel, blkno, vmbuffer,
								VISIBILITYMAP_VALID_BITS);
		}

		/*
		 * If the all-visible page is all-frozen but not marked as such yet,
		 * mark it as all-frozen.  Note that all_frozen is only valid if
		 * all_visible is true, so we must check both.
		 */
		else if (all_visible_according_to_vm && all_visible && all_frozen &&
				 !VM_ALL_FROZEN(onerel, blkno, &vmbuffer))
		{
			/*
			 * We can pass InvalidTransactionId as the cutoff XID here,
			 * because setting the all-frozen bit doesn't cause recovery
			 * conflicts.
			 */
			visibilitymap_set(onerel, blkno, buf, InvalidXLogRecPtr,
							  vmbuffer, InvalidTransactionId,
							  VISIBILITYMAP_ALL_FROZEN);
		}

		UnlockReleaseBuffer(buf);

		/* Remember the location of the last page with nonremovable tuples */
		if (hastup)
			vacrelstats->nonempty_pages = blkno + 1;

		/*
		 * If we remembered any tuples for deletion, then the page will be
		 * visited again by lazy_vacuum_heap, which will compute and record
		 * its post-compaction free space.  If not, then we're done with this
		 * page, so remember its free space as-is.  (This path will always be
		 * taken if there are no indexes.)
		 */
		if (dead_tuples->num_tuples == prev_dead_count)
			RecordPageWithFreeSpace(onerel, blkno, freespace);
	}

	/* report that everything is scanned and vacuumed */
	pgstat_progress_update_param(PROGRESS_VACUUM_HEAP_BLKS_SCANNED, blkno);

	/* Clear the block number information */
	vacrelstats->blkno = InvalidBlockNumber;

	pfree(frozen);

	/* save stats for use later */
	vacrelstats->tuples_deleted = tups_vacuumed;
	vacrelstats->new_dead_tuples = nkeep;

	/* now we can compute the new value for pg_class.reltuples */
	vacrelstats->new_live_tuples = vac_estimate_reltuples(onerel,
														  nblocks,
														  vacrelstats->tupcount_pages,
														  live_tuples);

	/* also compute total number of surviving heap entries */
	vacrelstats->new_rel_tuples =
		vacrelstats->new_live_tuples + vacrelstats->new_dead_tuples;

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
	{
		/* Work on all the indexes, and then the heap */
		lazy_vacuum_all_indexes(onerel, Irel, indstats, vacrelstats,
								lps, nindexes);

		/* Remove tuples from heap */
		lazy_vacuum_heap(onerel, vacrelstats);
	}

	/*
	 * Vacuum the remainder of the Free Space Map.  We must do this whether or
	 * not there were indexes.
	 */
	if (blkno > next_fsm_block_to_vacuum)
		FreeSpaceMapVacuumRange(onerel, next_fsm_block_to_vacuum, blkno);

	/* report all blocks vacuumed */
	pgstat_progress_update_param(PROGRESS_VACUUM_HEAP_BLKS_VACUUMED, blkno);

	/* Do post-vacuum cleanup */
	if (vacrelstats->useindex)
		lazy_cleanup_all_indexes(Irel, indstats, vacrelstats, lps, nindexes);

	/*
	 * End parallel mode before updating index statistics as we cannot write
	 * during parallel mode.
	 */
	if (ParallelVacuumIsActive(lps))
		end_parallel_vacuum(indstats, lps, nindexes);

	/* Update index statistics */
	if (vacrelstats->useindex)
		update_index_statistics(Irel, indstats, nindexes);

	/* If no indexes, make log report that lazy_vacuum_heap would've made */
	if (vacuumed_pages)
		ereport(elevel,
				(errmsg("\"%s\": removed %.0f row versions in %u pages",
						vacrelstats->relname,
						tups_vacuumed, vacuumed_pages)));

	/*
	 * This is pretty messy, but we split it up so that we can skip emitting
	 * individual parts of the message when not applicable.
	 */
	initStringInfo(&buf);
	appendStringInfo(&buf,
					 _("%.0f dead row versions cannot be removed yet, oldest xmin: %u\n"),
					 nkeep, OldestXmin);
	appendStringInfo(&buf, _("There were %.0f unused item identifiers.\n"),
					 nunused);
	appendStringInfo(&buf, ngettext("Skipped %u page due to buffer pins, ",
									"Skipped %u pages due to buffer pins, ",
									vacrelstats->pinskipped_pages),
					 vacrelstats->pinskipped_pages);
	appendStringInfo(&buf, ngettext("%u frozen page.\n",
									"%u frozen pages.\n",
									vacrelstats->frozenskipped_pages),
					 vacrelstats->frozenskipped_pages);
	appendStringInfo(&buf, ngettext("%u page is entirely empty.\n",
									"%u pages are entirely empty.\n",
									empty_pages),
					 empty_pages);
	appendStringInfo(&buf, _("%s."), pg_rusage_show(&ru0));

	ereport(elevel,
			(errmsg("\"%s\": found %.0f removable, %.0f nonremovable row versions in %u out of %u pages",
					vacrelstats->relname,
					tups_vacuumed, num_tuples,
					vacrelstats->scanned_pages, nblocks),
			 errdetail_internal("%s", buf.data)));
	pfree(buf.data);
}

/*
 *	lazy_vacuum_all_indexes() -- vacuum all indexes of relation.
 *
 * We process the indexes serially unless we are doing parallel vacuum.
 */
static void
lazy_vacuum_all_indexes(Relation onerel, Relation *Irel,
						IndexBulkDeleteResult **stats,
						LVRelStats *vacrelstats, LVParallelState *lps,
						int nindexes)
{
	Assert(!IsParallelWorker());
	Assert(nindexes > 0);

	/* Log cleanup info before we touch indexes */
	vacuum_log_cleanup_info(onerel, vacrelstats);

	/* Report that we are now vacuuming indexes */
	pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
								 PROGRESS_VACUUM_PHASE_VACUUM_INDEX);

	/* Perform index vacuuming with parallel workers for parallel vacuum. */
	if (ParallelVacuumIsActive(lps))
	{
		/* Tell parallel workers to do index vacuuming */
		lps->lvshared->for_cleanup = false;
		lps->lvshared->first_time = false;

		/*
		 * We can only provide an approximate value of num_heap_tuples in
		 * vacuum cases.
		 */
		lps->lvshared->reltuples = vacrelstats->old_live_tuples;
		lps->lvshared->estimated_count = true;

		lazy_parallel_vacuum_indexes(Irel, stats, vacrelstats, lps, nindexes);
	}
	else
	{
		int			idx;

		for (idx = 0; idx < nindexes; idx++)
			lazy_vacuum_index(Irel[idx], &stats[idx], vacrelstats->dead_tuples,
							  vacrelstats->old_live_tuples, vacrelstats);
	}

	/* Increase and report the number of index scans */
	vacrelstats->num_index_scans++;
	pgstat_progress_update_param(PROGRESS_VACUUM_NUM_INDEX_VACUUMS,
								 vacrelstats->num_index_scans);
}


/*
 *	lazy_vacuum_heap() -- second pass over the heap
 *
 *		This routine marks dead tuples as unused and compacts out free
 *		space on their pages.  Pages not having dead tuples recorded from
 *		lazy_scan_heap are not visited at all.
 *
 * Note: the reason for doing this as a second pass is we cannot remove
 * the tuples until we've removed their index entries, and we want to
 * process index entry removal in batches as large as possible.
 */
static void
lazy_vacuum_heap(Relation onerel, LVRelStats *vacrelstats)
{
	int			tupindex;
	int			npages;
	PGRUsage	ru0;
	Buffer		vmbuffer = InvalidBuffer;
	LVSavedErrInfo saved_err_info;

	/* Report that we are now vacuuming the heap */
	pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
								 PROGRESS_VACUUM_PHASE_VACUUM_HEAP);

	/* Update error traceback information */
	update_vacuum_error_info(vacrelstats, &saved_err_info, VACUUM_ERRCB_PHASE_VACUUM_HEAP,
							 InvalidBlockNumber);

	pg_rusage_init(&ru0);
	npages = 0;

	tupindex = 0;
	while (tupindex < vacrelstats->dead_tuples->num_tuples)
	{
		BlockNumber tblk;
		Buffer		buf;
		Page		page;
		Size		freespace;

		vacuum_delay_point();

		tblk = ItemPointerGetBlockNumber(&vacrelstats->dead_tuples->itemptrs[tupindex]);
		vacrelstats->blkno = tblk;
		buf = ReadBufferExtended(onerel, MAIN_FORKNUM, tblk, RBM_NORMAL,
								 vac_strategy);
		if (!ConditionalLockBufferForCleanup(buf))
		{
			ReleaseBuffer(buf);
			++tupindex;
			continue;
		}
		tupindex = lazy_vacuum_page(onerel, tblk, buf, tupindex, vacrelstats,
									&vmbuffer);

		/* Now that we've compacted the page, record its available space */
		page = BufferGetPage(buf);
		freespace = PageGetHeapFreeSpace(page);

		UnlockReleaseBuffer(buf);
		RecordPageWithFreeSpace(onerel, tblk, freespace);
		npages++;
	}

	/* Clear the block number information */
	vacrelstats->blkno = InvalidBlockNumber;

	if (BufferIsValid(vmbuffer))
	{
		ReleaseBuffer(vmbuffer);
		vmbuffer = InvalidBuffer;
	}

	ereport(elevel,
			(errmsg("\"%s\": removed %d row versions in %d pages",
					vacrelstats->relname,
					tupindex, npages),
			 errdetail_internal("%s", pg_rusage_show(&ru0))));

	/* Revert to the previous phase information for error traceback */
	restore_vacuum_error_info(vacrelstats, &saved_err_info);
}

/*
 *	lazy_vacuum_page() -- free dead tuples on a page
 *					 and repair its fragmentation.
 *
 * Caller must hold pin and buffer cleanup lock on the buffer.
 *
 * tupindex is the index in vacrelstats->dead_tuples of the first dead
 * tuple for this page.  We assume the rest follow sequentially.
 * The return value is the first tupindex after the tuples of this page.
 */
static int
lazy_vacuum_page(Relation onerel, BlockNumber blkno, Buffer buffer,
				 int tupindex, LVRelStats *vacrelstats, Buffer *vmbuffer)
{
	LVDeadTuples *dead_tuples = vacrelstats->dead_tuples;
	Page		page = BufferGetPage(buffer);
	OffsetNumber unused[MaxOffsetNumber];
	int			uncnt = 0;
	TransactionId visibility_cutoff_xid;
	bool		all_frozen;
	LVSavedErrInfo saved_err_info;

	pgstat_progress_update_param(PROGRESS_VACUUM_HEAP_BLKS_VACUUMED, blkno);

	/* Update error traceback information */
	update_vacuum_error_info(vacrelstats, &saved_err_info, VACUUM_ERRCB_PHASE_VACUUM_HEAP,
							 blkno);

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
		ItemIdSetUnused(itemid);
		unused[uncnt++] = toff;
	}

	PageRepairFragmentation(page);

	/*
	 * Mark buffer dirty before we write WAL.
	 */
	MarkBufferDirty(buffer);

	/* XLOG stuff */
	if (RelationNeedsWAL(onerel))
	{
		XLogRecPtr	recptr;

		recptr = log_heap_clean(onerel, buffer,
								NULL, 0, NULL, 0,
								unused, uncnt,
								vacrelstats->latestRemovedXid);
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
	 * Now that we have removed the dead tuples from the page, once again
	 * check if the page has become all-visible.  The page is already marked
	 * dirty, exclusively locked, and, if needed, a full page image has been
	 * emitted in the log_heap_clean() above.
	 */
	if (heap_page_is_all_visible(onerel, buffer, &visibility_cutoff_xid,
								 &all_frozen))
		PageSetAllVisible(page);

	/*
	 * All the changes to the heap page have been done. If the all-visible
	 * flag is now set, also set the VM all-visible bit (and, if possible, the
	 * all-frozen bit) unless this has already been done previously.
	 */
	if (PageIsAllVisible(page))
	{
		uint8		vm_status = visibilitymap_get_status(onerel, blkno, vmbuffer);
		uint8		flags = 0;

		/* Set the VM all-frozen bit to flag, if needed */
		if ((vm_status & VISIBILITYMAP_ALL_VISIBLE) == 0)
			flags |= VISIBILITYMAP_ALL_VISIBLE;
		if ((vm_status & VISIBILITYMAP_ALL_FROZEN) == 0 && all_frozen)
			flags |= VISIBILITYMAP_ALL_FROZEN;

		Assert(BufferIsValid(*vmbuffer));
		if (flags != 0)
			visibilitymap_set(onerel, blkno, buffer, InvalidXLogRecPtr,
							  *vmbuffer, visibility_cutoff_xid, flags);
	}

	/* Revert to the previous phase information for error traceback */
	restore_vacuum_error_info(vacrelstats, &saved_err_info);
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
lazy_check_needs_freeze(Buffer buf, bool *hastup)
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

		itemid = PageGetItemId(page, offnum);

		/* this should match hastup test in count_nondeletable_pages() */
		if (ItemIdIsUsed(itemid))
			*hastup = true;

		/* dead and redirect items never need freezing */
		if (!ItemIdIsNormal(itemid))
			continue;

		tupleheader = (HeapTupleHeader) PageGetItem(page, itemid);

		if (heap_tuple_needs_freeze(tupleheader, FreezeLimit,
									MultiXactCutoff, buf))
			return true;
	}							/* scan along page */

	return false;
}

/*
 * Perform index vacuum or index cleanup with parallel workers.  This function
 * must be used by the parallel vacuum leader process.  The caller must set
 * lps->lvshared->for_cleanup to indicate whether to perform vacuum or
 * cleanup.
 */
static void
lazy_parallel_vacuum_indexes(Relation *Irel, IndexBulkDeleteResult **stats,
							 LVRelStats *vacrelstats, LVParallelState *lps,
							 int nindexes)
{
	int			nworkers;

	Assert(!IsParallelWorker());
	Assert(ParallelVacuumIsActive(lps));
	Assert(nindexes > 0);

	/* Determine the number of parallel workers to launch */
	if (lps->lvshared->for_cleanup)
	{
		if (lps->lvshared->first_time)
			nworkers = lps->nindexes_parallel_cleanup +
				lps->nindexes_parallel_condcleanup;
		else
			nworkers = lps->nindexes_parallel_cleanup;
	}
	else
		nworkers = lps->nindexes_parallel_bulkdel;

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
		if (vacrelstats->num_index_scans > 0)
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
	vacuum_indexes_leader(Irel, stats, vacrelstats, lps, nindexes);

	/*
	 * Join as a parallel worker.  The leader process alone processes all the
	 * indexes in the case where no workers are launched.
	 */
	parallel_vacuum_index(Irel, stats, lps->lvshared,
						  vacrelstats->dead_tuples, nindexes, vacrelstats);

	/*
	 * Next, accumulate buffer and WAL usage.  (This must wait for the workers
	 * to finish, or we might get incomplete data.)
	 */
	if (nworkers > 0)
	{
		int			i;

		/* Wait for all vacuum workers to finish */
		WaitForParallelWorkersToFinish(lps->pcxt);

		for (i = 0; i < lps->pcxt->nworkers_launched; i++)
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
parallel_vacuum_index(Relation *Irel, IndexBulkDeleteResult **stats,
					  LVShared *lvshared, LVDeadTuples *dead_tuples,
					  int nindexes, LVRelStats *vacrelstats)
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
		LVSharedIndStats *shared_indstats;

		/* Get an index number to process */
		idx = pg_atomic_fetch_add_u32(&(lvshared->idx), 1);

		/* Done for all indexes? */
		if (idx >= nindexes)
			break;

		/* Get the index statistics of this index from DSM */
		shared_indstats = get_indstats(lvshared, idx);

		/*
		 * Skip processing indexes that don't participate in parallel
		 * operation
		 */
		if (shared_indstats == NULL ||
			skip_parallel_vacuum_index(Irel[idx], lvshared))
			continue;

		/* Do vacuum or cleanup of the index */
		vacuum_one_index(Irel[idx], &(stats[idx]), lvshared, shared_indstats,
						 dead_tuples, vacrelstats);
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
vacuum_indexes_leader(Relation *Irel, IndexBulkDeleteResult **stats,
					  LVRelStats *vacrelstats, LVParallelState *lps,
					  int nindexes)
{
	int			i;

	Assert(!IsParallelWorker());

	/*
	 * Increment the active worker count if we are able to launch any worker.
	 */
	if (VacuumActiveNWorkers)
		pg_atomic_add_fetch_u32(VacuumActiveNWorkers, 1);

	for (i = 0; i < nindexes; i++)
	{
		LVSharedIndStats *shared_indstats;

		shared_indstats = get_indstats(lps->lvshared, i);

		/* Process the indexes skipped by parallel workers */
		if (shared_indstats == NULL ||
			skip_parallel_vacuum_index(Irel[i], lps->lvshared))
			vacuum_one_index(Irel[i], &(stats[i]), lps->lvshared,
							 shared_indstats, vacrelstats->dead_tuples,
							 vacrelstats);
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
static void
vacuum_one_index(Relation indrel, IndexBulkDeleteResult **stats,
				 LVShared *lvshared, LVSharedIndStats *shared_indstats,
				 LVDeadTuples *dead_tuples, LVRelStats *vacrelstats)
{
	IndexBulkDeleteResult *bulkdelete_res = NULL;

	if (shared_indstats)
	{
		/* Get the space for IndexBulkDeleteResult */
		bulkdelete_res = &(shared_indstats->stats);

		/*
		 * Update the pointer to the corresponding bulk-deletion result if
		 * someone has already updated it.
		 */
		if (shared_indstats->updated && *stats == NULL)
			*stats = bulkdelete_res;
	}

	/* Do vacuum or cleanup of the index */
	if (lvshared->for_cleanup)
		lazy_cleanup_index(indrel, stats, lvshared->reltuples,
						   lvshared->estimated_count, vacrelstats);
	else
		lazy_vacuum_index(indrel, stats, dead_tuples,
						  lvshared->reltuples, vacrelstats);

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
	if (shared_indstats && !shared_indstats->updated && *stats != NULL)
	{
		memcpy(bulkdelete_res, *stats, sizeof(IndexBulkDeleteResult));
		shared_indstats->updated = true;

		/*
		 * Now that stats[idx] points to the DSM segment, we don't need the
		 * locally allocated results.
		 */
		pfree(*stats);
		*stats = bulkdelete_res;
	}
}

/*
 *	lazy_cleanup_all_indexes() -- cleanup all indexes of relation.
 *
 * Cleanup indexes.  We process the indexes serially unless we are doing
 * parallel vacuum.
 */
static void
lazy_cleanup_all_indexes(Relation *Irel, IndexBulkDeleteResult **stats,
						 LVRelStats *vacrelstats, LVParallelState *lps,
						 int nindexes)
{
	int			idx;

	Assert(!IsParallelWorker());
	Assert(nindexes > 0);

	/* Report that we are now cleaning up indexes */
	pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
								 PROGRESS_VACUUM_PHASE_INDEX_CLEANUP);

	/*
	 * If parallel vacuum is active we perform index cleanup with parallel
	 * workers.
	 */
	if (ParallelVacuumIsActive(lps))
	{
		/* Tell parallel workers to do index cleanup */
		lps->lvshared->for_cleanup = true;
		lps->lvshared->first_time =
			(vacrelstats->num_index_scans == 0);

		/*
		 * Now we can provide a better estimate of total number of surviving
		 * tuples (we assume indexes are more interested in that than in the
		 * number of nominally live tuples).
		 */
		lps->lvshared->reltuples = vacrelstats->new_rel_tuples;
		lps->lvshared->estimated_count =
			(vacrelstats->tupcount_pages < vacrelstats->rel_pages);

		lazy_parallel_vacuum_indexes(Irel, stats, vacrelstats, lps, nindexes);
	}
	else
	{
		for (idx = 0; idx < nindexes; idx++)
			lazy_cleanup_index(Irel[idx], &stats[idx],
							   vacrelstats->new_rel_tuples,
							   vacrelstats->tupcount_pages < vacrelstats->rel_pages,
							   vacrelstats);
	}
}

/*
 *	lazy_vacuum_index() -- vacuum one index relation.
 *
 *		Delete all the index entries pointing to tuples listed in
 *		dead_tuples, and update running statistics.
 *
 *		reltuples is the number of heap tuples to be passed to the
 *		bulkdelete callback.
 */
static void
lazy_vacuum_index(Relation indrel, IndexBulkDeleteResult **stats,
				  LVDeadTuples *dead_tuples, double reltuples, LVRelStats *vacrelstats)
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
	ivinfo.strategy = vac_strategy;

	/*
	 * Update error traceback information.
	 *
	 * The index name is saved during this phase and restored immediately
	 * after this phase.  See vacuum_error_callback.
	 */
	Assert(vacrelstats->indname == NULL);
	vacrelstats->indname = pstrdup(RelationGetRelationName(indrel));
	update_vacuum_error_info(vacrelstats, &saved_err_info,
							 VACUUM_ERRCB_PHASE_VACUUM_INDEX,
							 InvalidBlockNumber);

	/* Do bulk deletion */
	*stats = index_bulk_delete(&ivinfo, *stats,
							   lazy_tid_reaped, (void *) dead_tuples);

	ereport(elevel,
			(errmsg("scanned index \"%s\" to remove %d row versions",
					vacrelstats->indname,
					dead_tuples->num_tuples),
			 errdetail_internal("%s", pg_rusage_show(&ru0))));

	/* Revert to the previous phase information for error traceback */
	restore_vacuum_error_info(vacrelstats, &saved_err_info);
	pfree(vacrelstats->indname);
	vacrelstats->indname = NULL;
}

/*
 *	lazy_cleanup_index() -- do post-vacuum cleanup for one index relation.
 *
 *		reltuples is the number of heap tuples and estimated_count is true
 *		if reltuples is an estimated value.
 */
static void
lazy_cleanup_index(Relation indrel,
				   IndexBulkDeleteResult **stats,
				   double reltuples, bool estimated_count, LVRelStats *vacrelstats)
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
	ivinfo.strategy = vac_strategy;

	/*
	 * Update error traceback information.
	 *
	 * The index name is saved during this phase and restored immediately
	 * after this phase.  See vacuum_error_callback.
	 */
	Assert(vacrelstats->indname == NULL);
	vacrelstats->indname = pstrdup(RelationGetRelationName(indrel));
	update_vacuum_error_info(vacrelstats, &saved_err_info,
							 VACUUM_ERRCB_PHASE_INDEX_CLEANUP,
							 InvalidBlockNumber);

	*stats = index_vacuum_cleanup(&ivinfo, *stats);

	if (*stats)
	{
		ereport(elevel,
				(errmsg("index \"%s\" now contains %.0f row versions in %u pages",
						RelationGetRelationName(indrel),
						(*stats)->num_index_tuples,
						(*stats)->num_pages),
				 errdetail("%.0f index row versions were removed.\n"
						   "%u index pages have been deleted, %u are currently reusable.\n"
						   "%s.",
						   (*stats)->tuples_removed,
						   (*stats)->pages_deleted, (*stats)->pages_free,
						   pg_rusage_show(&ru0))));
	}

	/* Revert back to the old phase information for error traceback */
	restore_vacuum_error_info(vacrelstats, &saved_err_info);
	pfree(vacrelstats->indname);
	vacrelstats->indname = NULL;
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
should_attempt_truncation(VacuumParams *params, LVRelStats *vacrelstats)
{
	BlockNumber possibly_freeable;

	if (params->truncate == VACOPT_TERNARY_DISABLED)
		return false;

	possibly_freeable = vacrelstats->rel_pages - vacrelstats->nonempty_pages;
	if (possibly_freeable > 0 &&
		(possibly_freeable >= REL_TRUNCATE_MINIMUM ||
		 possibly_freeable >= vacrelstats->rel_pages / REL_TRUNCATE_FRACTION) &&
		old_snapshot_threshold < 0)
		return true;
	else
		return false;
}

/*
 * lazy_truncate_heap - try to truncate off any empty pages at the end
 */
static void
lazy_truncate_heap(Relation onerel, LVRelStats *vacrelstats)
{
	BlockNumber old_rel_pages = vacrelstats->rel_pages;
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
		vacrelstats->lock_waiter_detected = false;
		lock_retry = 0;
		while (true)
		{
			if (ConditionalLockRelation(onerel, AccessExclusiveLock))
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
				vacrelstats->lock_waiter_detected = true;
				ereport(elevel,
						(errmsg("\"%s\": stopping truncate due to conflicting lock request",
								vacrelstats->relname)));
				return;
			}

			pg_usleep(VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL * 1000L);
		}

		/*
		 * Now that we have exclusive lock, look to see if the rel has grown
		 * whilst we were vacuuming with non-exclusive lock.  If so, give up;
		 * the newly added pages presumably contain non-deletable tuples.
		 */
		new_rel_pages = RelationGetNumberOfBlocks(onerel);
		if (new_rel_pages != old_rel_pages)
		{
			/*
			 * Note: we intentionally don't update vacrelstats->rel_pages with
			 * the new rel size here.  If we did, it would amount to assuming
			 * that the new pages are empty, which is unlikely. Leaving the
			 * numbers alone amounts to assuming that the new pages have the
			 * same tuple density as existing ones, which is less unlikely.
			 */
			UnlockRelation(onerel, AccessExclusiveLock);
			return;
		}

		/*
		 * Scan backwards from the end to verify that the end pages actually
		 * contain no tuples.  This is *necessary*, not optional, because
		 * other backends could have added tuples to these pages whilst we
		 * were vacuuming.
		 */
		new_rel_pages = count_nondeletable_pages(onerel, vacrelstats);
		vacrelstats->blkno = new_rel_pages;

		if (new_rel_pages >= old_rel_pages)
		{
			/* can't do anything after all */
			UnlockRelation(onerel, AccessExclusiveLock);
			return;
		}

		/*
		 * Okay to truncate.
		 */
		RelationTruncate(onerel, new_rel_pages);

		/*
		 * We can release the exclusive lock as soon as we have truncated.
		 * Other backends can't safely access the relation until they have
		 * processed the smgr invalidation that smgrtruncate sent out ... but
		 * that should happen as part of standard invalidation processing once
		 * they acquire lock on the relation.
		 */
		UnlockRelation(onerel, AccessExclusiveLock);

		/*
		 * Update statistics.  Here, it *is* correct to adjust rel_pages
		 * without also touching reltuples, since the tuple count wasn't
		 * changed by the truncation.
		 */
		vacrelstats->pages_removed += old_rel_pages - new_rel_pages;
		vacrelstats->rel_pages = new_rel_pages;

		ereport(elevel,
				(errmsg("\"%s\": truncated %u to %u pages",
						vacrelstats->relname,
						old_rel_pages, new_rel_pages),
				 errdetail_internal("%s",
									pg_rusage_show(&ru0))));
		old_rel_pages = new_rel_pages;
	} while (new_rel_pages > vacrelstats->nonempty_pages &&
			 vacrelstats->lock_waiter_detected);
}

/*
 * Rescan end pages to verify that they are (still) empty of tuples.
 *
 * Returns number of nondeletable pages (last nonempty page + 1).
 */
static BlockNumber
count_nondeletable_pages(Relation onerel, LVRelStats *vacrelstats)
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
	blkno = vacrelstats->rel_pages;
	StaticAssertStmt((PREFETCH_SIZE & (PREFETCH_SIZE - 1)) == 0,
					 "prefetch size must be power of 2");
	prefetchedUntil = InvalidBlockNumber;
	while (blkno > vacrelstats->nonempty_pages)
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
				if (LockHasWaitersRelation(onerel, AccessExclusiveLock))
				{
					ereport(elevel,
							(errmsg("\"%s\": suspending truncate due to conflicting lock request",
									vacrelstats->relname)));

					vacrelstats->lock_waiter_detected = true;
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
				PrefetchBuffer(onerel, MAIN_FORKNUM, pblkno);
				CHECK_FOR_INTERRUPTS();
			}
			prefetchedUntil = prefetchStart;
		}

		buf = ReadBufferExtended(onerel, MAIN_FORKNUM, blkno,
								 RBM_NORMAL, vac_strategy);

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
	return vacrelstats->nonempty_pages;
}

/*
 * Return the maximum number of dead tuples we can record.
 */
static long
compute_max_dead_tuples(BlockNumber relblocks, bool useindex)
{
	long		maxtuples;
	int			vac_work_mem = IsAutoVacuumWorkerProcess() &&
	autovacuum_work_mem != -1 ?
	autovacuum_work_mem : maintenance_work_mem;

	if (useindex)
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
lazy_space_alloc(LVRelStats *vacrelstats, BlockNumber relblocks)
{
	LVDeadTuples *dead_tuples = NULL;
	long		maxtuples;

	maxtuples = compute_max_dead_tuples(relblocks, vacrelstats->useindex);

	dead_tuples = (LVDeadTuples *) palloc(SizeOfDeadTuples(maxtuples));
	dead_tuples->num_tuples = 0;
	dead_tuples->max_tuples = (int) maxtuples;

	vacrelstats->dead_tuples = dead_tuples;
}

/*
 * lazy_record_dead_tuple - remember one deletable tuple
 */
static void
lazy_record_dead_tuple(LVDeadTuples *dead_tuples, ItemPointer itemptr)
{
	/*
	 * The array shouldn't overflow under normal behavior, but perhaps it
	 * could if we are given a really small maintenance_work_mem. In that
	 * case, just forget the last few tuples (we'll get 'em next time).
	 */
	if (dead_tuples->num_tuples < dead_tuples->max_tuples)
	{
		dead_tuples->itemptrs[dead_tuples->num_tuples] = *itemptr;
		dead_tuples->num_tuples++;
		pgstat_progress_update_param(PROGRESS_VACUUM_NUM_DEAD_TUPLES,
									 dead_tuples->num_tuples);
	}
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
	ItemPointer res;

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
heap_page_is_all_visible(Relation rel, Buffer buf,
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
		tuple.t_tableOid = RelationGetRelid(rel);

		switch (HeapTupleSatisfiesVacuum(&tuple, OldestXmin, buf))
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
					if (!TransactionIdPrecedes(xmin, OldestXmin))
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
compute_parallel_vacuum_workers(Relation *Irel, int nindexes, int nrequested,
								bool *can_parallel_vacuum)
{
	int			nindexes_parallel = 0;
	int			nindexes_parallel_bulkdel = 0;
	int			nindexes_parallel_cleanup = 0;
	int			parallel_workers;
	int			i;

	/*
	 * We don't allow performing parallel operation in standalone backend or
	 * when parallelism is disabled.
	 */
	if (!IsUnderPostmaster || max_parallel_maintenance_workers == 0)
		return 0;

	/*
	 * Compute the number of indexes that can participate in parallel vacuum.
	 */
	for (i = 0; i < nindexes; i++)
	{
		uint8		vacoptions = Irel[i]->rd_indam->amparallelvacuumoptions;

		if (vacoptions == VACUUM_OPTION_NO_PARALLEL ||
			RelationGetNumberOfBlocks(Irel[i]) < min_parallel_index_scan_size)
			continue;

		can_parallel_vacuum[i] = true;

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
 * Initialize variables for shared index statistics, set NULL bitmap and the
 * size of stats for each index.
 */
static void
prepare_index_statistics(LVShared *lvshared, bool *can_parallel_vacuum,
						 int nindexes)
{
	int			i;

	/* Currently, we don't support parallel vacuum for autovacuum */
	Assert(!IsAutoVacuumWorkerProcess());

	/* Set NULL for all indexes */
	memset(lvshared->bitmap, 0x00, BITMAPLEN(nindexes));

	for (i = 0; i < nindexes; i++)
	{
		if (!can_parallel_vacuum[i])
			continue;

		/* Set NOT NULL as this index does support parallelism */
		lvshared->bitmap[i >> 3] |= 1 << (i & 0x07);
	}
}

/*
 * Update index statistics in pg_class if the statistics are accurate.
 */
static void
update_index_statistics(Relation *Irel, IndexBulkDeleteResult **stats,
						int nindexes)
{
	int			i;

	Assert(!IsInParallelMode());

	for (i = 0; i < nindexes; i++)
	{
		if (stats[i] == NULL || stats[i]->estimated_count)
			continue;

		/* Update index statistics */
		vac_update_relstats(Irel[i],
							stats[i]->num_pages,
							stats[i]->num_index_tuples,
							0,
							false,
							InvalidTransactionId,
							InvalidMultiXactId,
							false);
		pfree(stats[i]);
	}
}

/*
 * This function prepares and returns parallel vacuum state if we can launch
 * even one worker.  This function is responsible for entering parallel mode,
 * create a parallel context, and then initialize the DSM segment.
 */
static LVParallelState *
begin_parallel_vacuum(Oid relid, Relation *Irel, LVRelStats *vacrelstats,
					  BlockNumber nblocks, int nindexes, int nrequested)
{
	LVParallelState *lps = NULL;
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
	int			i;

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
	parallel_workers = compute_parallel_vacuum_workers(Irel, nindexes,
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
	for (i = 0; i < nindexes; i++)
	{
		uint8		vacoptions = Irel[i]->rd_indam->amparallelvacuumoptions;

		/*
		 * Cleanup option should be either disabled, always performing in
		 * parallel or conditionally performing in parallel.
		 */
		Assert(((vacoptions & VACUUM_OPTION_PARALLEL_CLEANUP) == 0) ||
			   ((vacoptions & VACUUM_OPTION_PARALLEL_COND_CLEANUP) == 0));
		Assert(vacoptions <= VACUUM_OPTION_MAX_VALID_VALUE);

		/* Skip indexes that don't participate in parallel vacuum */
		if (!can_parallel_vacuum[i])
			continue;

		if (Irel[i]->rd_indam->amusemaintenanceworkmem)
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
	shared->relid = relid;
	shared->elevel = elevel;
	shared->maintenance_work_mem_worker =
		(nindexes_mwm > 0) ?
		maintenance_work_mem / Min(parallel_workers, nindexes_mwm) :
		maintenance_work_mem;

	pg_atomic_init_u32(&(shared->cost_balance), 0);
	pg_atomic_init_u32(&(shared->active_nworkers), 0);
	pg_atomic_init_u32(&(shared->idx), 0);
	shared->offset = MAXALIGN(add_size(SizeOfLVShared, BITMAPLEN(nindexes)));
	prepare_index_statistics(shared, can_parallel_vacuum, nindexes);

	shm_toc_insert(pcxt->toc, PARALLEL_VACUUM_KEY_SHARED, shared);
	lps->lvshared = shared;

	/* Prepare the dead tuple space */
	dead_tuples = (LVDeadTuples *) shm_toc_allocate(pcxt->toc, est_deadtuples);
	dead_tuples->max_tuples = maxtuples;
	dead_tuples->num_tuples = 0;
	MemSet(dead_tuples->itemptrs, 0, sizeof(ItemPointerData) * maxtuples);
	shm_toc_insert(pcxt->toc, PARALLEL_VACUUM_KEY_DEAD_TUPLES, dead_tuples);
	vacrelstats->dead_tuples = dead_tuples;

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
end_parallel_vacuum(IndexBulkDeleteResult **stats, LVParallelState *lps,
					int nindexes)
{
	int			i;

	Assert(!IsParallelWorker());

	/* Copy the updated statistics */
	for (i = 0; i < nindexes; i++)
	{
		LVSharedIndStats *indstats = get_indstats(lps->lvshared, i);

		/*
		 * Skip unused slot.  The statistics of this index are already stored
		 * in local memory.
		 */
		if (indstats == NULL)
			continue;

		if (indstats->updated)
		{
			stats[i] = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
			memcpy(stats[i], &(indstats->stats), sizeof(IndexBulkDeleteResult));
		}
		else
			stats[i] = NULL;
	}

	DestroyParallelContext(lps->pcxt);
	ExitParallelMode();

	/* Deactivate parallel vacuum */
	pfree(lps);
	lps = NULL;
}

/* Return the Nth index statistics or NULL */
static LVSharedIndStats *
get_indstats(LVShared *lvshared, int n)
{
	int			i;
	char	   *p;

	if (IndStatsIsNull(lvshared, n))
		return NULL;

	p = (char *) GetSharedIndStats(lvshared);
	for (i = 0; i < n; i++)
	{
		if (IndStatsIsNull(lvshared, i))
			continue;

		p += sizeof(LVSharedIndStats);
	}

	return (LVSharedIndStats *) p;
}

/*
 * Returns true, if the given index can't participate in parallel index vacuum
 * or parallel index cleanup, false, otherwise.
 */
static bool
skip_parallel_vacuum_index(Relation indrel, LVShared *lvshared)
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
			return true;
	}
	else if ((vacoptions & VACUUM_OPTION_PARALLEL_BULKDEL) == 0)
	{
		/* Skip if the index does not support parallel bulk deletion */
		return true;
	}

	return false;
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
	Relation	onerel;
	Relation   *indrels;
	LVShared   *lvshared;
	LVDeadTuples *dead_tuples;
	BufferUsage *buffer_usage;
	WalUsage   *wal_usage;
	int			nindexes;
	char	   *sharedquery;
	IndexBulkDeleteResult **stats;
	LVRelStats	vacrelstats;
	ErrorContextCallback errcallback;

	/*
	 * A parallel vacuum worker must have only PROC_IN_VACUUM flag since we
	 * don't support parallel vacuum for autovacuum as of now.
	 */
	Assert(MyPgXact->vacuumFlags == PROC_IN_VACUUM);

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
	onerel = table_open(lvshared->relid, ShareUpdateExclusiveLock);

	/*
	 * Open all indexes. indrels are sorted in order by OID, which should be
	 * matched to the leader's one.
	 */
	vac_open_indexes(onerel, RowExclusiveLock, &nindexes, &indrels);
	Assert(nindexes > 0);

	/* Each parallel VACUUM worker gets its own access strategy */
	vac_strategy = GetAccessStrategy(BAS_VACUUM);

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

	stats = (IndexBulkDeleteResult **)
		palloc0(nindexes * sizeof(IndexBulkDeleteResult *));

	if (lvshared->maintenance_work_mem_worker > 0)
		maintenance_work_mem = lvshared->maintenance_work_mem_worker;

	/*
	 * Initialize vacrelstats for use as error callback arg by parallel
	 * worker.
	 */
	vacrelstats.relnamespace = get_namespace_name(RelationGetNamespace(onerel));
	vacrelstats.relname = pstrdup(RelationGetRelationName(onerel));
	vacrelstats.indname = NULL;
	vacrelstats.phase = VACUUM_ERRCB_PHASE_UNKNOWN; /* Not yet processing */

	/* Setup error traceback support for ereport() */
	errcallback.callback = vacuum_error_callback;
	errcallback.arg = &vacrelstats;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* Prepare to track buffer usage during parallel execution */
	InstrStartParallelQuery();

	/* Process indexes to perform vacuum/cleanup */
	parallel_vacuum_index(indrels, stats, lvshared, dead_tuples, nindexes,
						  &vacrelstats);

	/* Report buffer/WAL usage during parallel execution */
	buffer_usage = shm_toc_lookup(toc, PARALLEL_VACUUM_KEY_BUFFER_USAGE, false);
	wal_usage = shm_toc_lookup(toc, PARALLEL_VACUUM_KEY_WAL_USAGE, false);
	InstrEndParallelQuery(&buffer_usage[ParallelWorkerNumber],
						  &wal_usage[ParallelWorkerNumber]);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;

	vac_close_indexes(nindexes, indrels, RowExclusiveLock);
	table_close(onerel, ShareUpdateExclusiveLock);
	FreeAccessStrategy(vac_strategy);
	pfree(stats);
}

/*
 * Error context callback for errors occurring during vacuum.
 */
static void
vacuum_error_callback(void *arg)
{
	LVRelStats *errinfo = arg;

	switch (errinfo->phase)
	{
		case VACUUM_ERRCB_PHASE_SCAN_HEAP:
			if (BlockNumberIsValid(errinfo->blkno))
				errcontext("while scanning block %u of relation \"%s.%s\"",
						   errinfo->blkno, errinfo->relnamespace, errinfo->relname);
			else
				errcontext("while scanning relation \"%s.%s\"",
						   errinfo->relnamespace, errinfo->relname);
			break;

		case VACUUM_ERRCB_PHASE_VACUUM_HEAP:
			if (BlockNumberIsValid(errinfo->blkno))
				errcontext("while vacuuming block %u of relation \"%s.%s\"",
						   errinfo->blkno, errinfo->relnamespace, errinfo->relname);
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
update_vacuum_error_info(LVRelStats *errinfo, LVSavedErrInfo *saved_err_info, int phase,
						 BlockNumber blkno)
{
	if (saved_err_info)
	{
		saved_err_info->blkno = errinfo->blkno;
		saved_err_info->phase = errinfo->phase;
	}

	errinfo->blkno = blkno;
	errinfo->phase = phase;
}

/*
 * Restores the vacuum information saved via a prior call to update_vacuum_error_info.
 */
static void
restore_vacuum_error_info(LVRelStats *errinfo, const LVSavedErrInfo *saved_err_info)
{
	errinfo->blkno = saved_err_info->blkno;
	errinfo->phase = saved_err_info->phase;
}
