/*-------------------------------------------------------------------------
 *
 * vacuumlazy.c
 *	  Concurrent ("lazy") vacuuming.
 *
 *
 * The major space usage for LAZY VACUUM is storage for the array of dead
 * tuple TIDs, with the next biggest need being storage for per-disk-page
 * free space info.  We want to ensure we can vacuum even the very largest
 * relations with finite memory space usage.  To do that, we set upper bounds
 * on the number of tuples and pages we will keep track of at once.
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
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/vacuumlazy.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/heapam_xlog.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/transam.h"
#include "access/visibilitymap.h"
#include "access/xlog.h"
#include "catalog/catalog.h"
#include "catalog/storage.h"
#include "commands/dbcommands.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "portability/instr_time.h"
#include "postmaster/autovacuum.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/lmgr.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_rusage.h"
#include "utils/timestamp.h"
#include "utils/tqual.h"


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
#define VACUUM_TRUNCATE_LOCK_CHECK_INTERVAL		20		/* ms */
#define VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL		50		/* ms */
#define VACUUM_TRUNCATE_LOCK_TIMEOUT			5000	/* ms */

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

typedef struct LVRelStats
{
	/* hasindex = true means two-pass strategy; false means one-pass */
	bool		hasindex;
	/* Overall statistics about rel */
	BlockNumber old_rel_pages;	/* previous value of pg_class.relpages */
	BlockNumber rel_pages;		/* total number of pages */
	BlockNumber scanned_pages;	/* number of pages we examined */
	BlockNumber pinskipped_pages;		/* # of pages we skipped due to a pin */
	double		scanned_tuples; /* counts only tuples on scanned pages */
	double		old_rel_tuples; /* previous value of pg_class.reltuples */
	double		new_rel_tuples; /* new estimated total # of tuples */
	double		new_dead_tuples;	/* new estimated total # of dead tuples */
	BlockNumber pages_removed;
	double		tuples_deleted;
	BlockNumber nonempty_pages; /* actually, last nonempty page + 1 */
	/* List of TIDs of tuples we intend to delete */
	/* NB: this list is ordered by TID address */
	int			num_dead_tuples;	/* current # of entries */
	int			max_dead_tuples;	/* # slots allocated in array */
	ItemPointer dead_tuples;	/* array of ItemPointerData */
	int			num_index_scans;
	TransactionId latestRemovedXid;
	bool		lock_waiter_detected;
} LVRelStats;


/* A few variables that don't seem worth passing around as parameters */
static int	elevel = -1;

static TransactionId OldestXmin;
static TransactionId FreezeLimit;
static MultiXactId MultiXactCutoff;

static BufferAccessStrategy vac_strategy;


/* non-export function prototypes */
static void lazy_scan_heap(Relation onerel, LVRelStats *vacrelstats,
			   Relation *Irel, int nindexes, bool scan_all);
static void lazy_vacuum_heap(Relation onerel, LVRelStats *vacrelstats);
static bool lazy_check_needs_freeze(Buffer buf);
static void lazy_vacuum_index(Relation indrel,
				  IndexBulkDeleteResult **stats,
				  LVRelStats *vacrelstats);
static void lazy_cleanup_index(Relation indrel,
				   IndexBulkDeleteResult *stats,
				   LVRelStats *vacrelstats);
static int lazy_vacuum_page(Relation onerel, BlockNumber blkno, Buffer buffer,
				 int tupindex, LVRelStats *vacrelstats, Buffer *vmbuffer);
static void lazy_truncate_heap(Relation onerel, LVRelStats *vacrelstats);
static BlockNumber count_nondeletable_pages(Relation onerel,
						 LVRelStats *vacrelstats);
static void lazy_space_alloc(LVRelStats *vacrelstats, BlockNumber relblocks);
static void lazy_record_dead_tuple(LVRelStats *vacrelstats,
					   ItemPointer itemptr);
static bool lazy_tid_reaped(ItemPointer itemptr, void *state);
static int	vac_cmp_itemptr(const void *left, const void *right);
static bool heap_page_is_all_visible(Relation rel, Buffer buf,
						 TransactionId *visibility_cutoff_xid);


/*
 *	lazy_vacuum_rel() -- perform LAZY VACUUM for one heap relation
 *
 *		This routine vacuums a single heap, cleans out its indexes, and
 *		updates its relpages and reltuples statistics.
 *
 *		At entry, we have already established a transaction and opened
 *		and locked the relation.
 */
void
lazy_vacuum_rel(Relation onerel, int options, VacuumParams *params,
				BufferAccessStrategy bstrategy)
{
	LVRelStats *vacrelstats;
	Relation   *Irel;
	int			nindexes;
	BlockNumber possibly_freeable;
	PGRUsage	ru0;
	TimestampTz starttime = 0;
	long		secs;
	int			usecs;
	double		read_rate,
				write_rate;
	bool		scan_all;		/* should we scan all pages? */
	bool		scanned_all;	/* did we actually scan all pages? */
	TransactionId xidFullScanLimit;
	MultiXactId mxactFullScanLimit;
	BlockNumber new_rel_pages;
	double		new_rel_tuples;
	BlockNumber new_rel_allvisible;
	double		new_live_tuples;
	TransactionId new_frozen_xid;
	MultiXactId new_min_multi;

	Assert(params != NULL);

	/* measure elapsed time iff autovacuum logging requires it */
	if (IsAutoVacuumWorkerProcess() && params->log_min_duration >= 0)
	{
		pg_rusage_init(&ru0);
		starttime = GetCurrentTimestamp();
	}

	if (options & VACOPT_VERBOSE)
		elevel = INFO;
	else
		elevel = DEBUG2;

	vac_strategy = bstrategy;

	vacuum_set_xid_limits(onerel,
						  params->freeze_min_age,
						  params->freeze_table_age,
						  params->multixact_freeze_min_age,
						  params->multixact_freeze_table_age,
						  &OldestXmin, &FreezeLimit, &xidFullScanLimit,
						  &MultiXactCutoff, &mxactFullScanLimit);

	/*
	 * We request a full scan if either the table's frozen Xid is now older
	 * than or equal to the requested Xid full-table scan limit; or if the
	 * table's minimum MultiXactId is older than or equal to the requested
	 * mxid full-table scan limit.
	 */
	scan_all = TransactionIdPrecedesOrEquals(onerel->rd_rel->relfrozenxid,
											 xidFullScanLimit);
	scan_all |= MultiXactIdPrecedesOrEquals(onerel->rd_rel->relminmxid,
											mxactFullScanLimit);

	vacrelstats = (LVRelStats *) palloc0(sizeof(LVRelStats));

	vacrelstats->old_rel_pages = onerel->rd_rel->relpages;
	vacrelstats->old_rel_tuples = onerel->rd_rel->reltuples;
	vacrelstats->num_index_scans = 0;
	vacrelstats->pages_removed = 0;
	vacrelstats->lock_waiter_detected = false;

	/* Open all indexes of the relation */
	vac_open_indexes(onerel, RowExclusiveLock, &nindexes, &Irel);
	vacrelstats->hasindex = (nindexes > 0);

	/* Do the vacuuming */
	lazy_scan_heap(onerel, vacrelstats, Irel, nindexes, scan_all);

	/* Done with indexes */
	vac_close_indexes(nindexes, Irel, NoLock);

	/*
	 * Compute whether we actually scanned the whole relation. If we did, we
	 * can adjust relfrozenxid and relminmxid.
	 *
	 * NB: We need to check this before truncating the relation, because that
	 * will change ->rel_pages.
	 */
	if (vacrelstats->scanned_pages < vacrelstats->rel_pages)
	{
		Assert(!scan_all);
		scanned_all = false;
	}
	else
		scanned_all = true;

	/*
	 * Optionally truncate the relation.
	 *
	 * Don't even think about it unless we have a shot at releasing a goodly
	 * number of pages.  Otherwise, the time taken isn't worth it.
	 */
	possibly_freeable = vacrelstats->rel_pages - vacrelstats->nonempty_pages;
	if (possibly_freeable > 0 &&
		(possibly_freeable >= REL_TRUNCATE_MINIMUM ||
		 possibly_freeable >= vacrelstats->rel_pages / REL_TRUNCATE_FRACTION))
		lazy_truncate_heap(onerel, vacrelstats);

	/* Vacuum the Free Space Map */
	FreeSpaceMapVacuum(onerel);

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
	 * We do update relallvisible even in the corner case, since if the table
	 * is all-visible we'd definitely like to know that.  But clamp the value
	 * to be not more than what we're setting relpages to.
	 *
	 * Also, don't change relfrozenxid/relminmxid if we skipped any pages,
	 * since then we don't know for certain that all tuples have a newer xmin.
	 */
	new_rel_pages = vacrelstats->rel_pages;
	new_rel_tuples = vacrelstats->new_rel_tuples;
	if (vacrelstats->scanned_pages == 0 && new_rel_pages > 0)
	{
		new_rel_pages = vacrelstats->old_rel_pages;
		new_rel_tuples = vacrelstats->old_rel_tuples;
	}

	new_rel_allvisible = visibilitymap_count(onerel);
	if (new_rel_allvisible > new_rel_pages)
		new_rel_allvisible = new_rel_pages;

	new_frozen_xid = scanned_all ? FreezeLimit : InvalidTransactionId;
	new_min_multi = scanned_all ? MultiXactCutoff : InvalidMultiXactId;

	vac_update_relstats(onerel,
						new_rel_pages,
						new_rel_tuples,
						new_rel_allvisible,
						vacrelstats->hasindex,
						new_frozen_xid,
						new_min_multi,
						false);

	/* report results to the stats collector, too */
	new_live_tuples = new_rel_tuples - vacrelstats->new_dead_tuples;
	if (new_live_tuples < 0)
		new_live_tuples = 0;	/* just in case */

	pgstat_report_vacuum(RelationGetRelid(onerel),
						 onerel->rd_rel->relisshared,
						 new_live_tuples,
						 vacrelstats->new_dead_tuples);

	/* and log the action if appropriate */
	if (IsAutoVacuumWorkerProcess() && params->log_min_duration >= 0)
	{
		TimestampTz endtime = GetCurrentTimestamp();

		if (params->log_min_duration == 0 ||
			TimestampDifferenceExceeds(starttime, endtime,
									   params->log_min_duration))
		{
			StringInfoData buf;

			TimestampDifference(starttime, endtime, &secs, &usecs);

			read_rate = 0;
			write_rate = 0;
			if ((secs > 0) || (usecs > 0))
			{
				read_rate = (double) BLCKSZ *VacuumPageMiss / (1024 * 1024) /
							(secs + usecs / 1000000.0);
				write_rate = (double) BLCKSZ *VacuumPageDirty / (1024 * 1024) /
							(secs + usecs / 1000000.0);
			}

			/*
			 * This is pretty messy, but we split it up so that we can skip
			 * emitting individual parts of the message when not applicable.
			 */
			initStringInfo(&buf);
			appendStringInfo(&buf, _("automatic vacuum of table \"%s.%s.%s\": index scans: %d\n"),
							 get_database_name(MyDatabaseId),
							 get_namespace_name(RelationGetNamespace(onerel)),
							 RelationGetRelationName(onerel),
							 vacrelstats->num_index_scans);
			appendStringInfo(&buf, _("pages: %u removed, %u remain, %u skipped due to pins\n"),
							 vacrelstats->pages_removed,
							 vacrelstats->rel_pages,
							 vacrelstats->pinskipped_pages);
			appendStringInfo(&buf,
							 _("tuples: %.0f removed, %.0f remain, %.0f are dead but not yet removable\n"),
							 vacrelstats->tuples_deleted,
							 vacrelstats->new_rel_tuples,
							 vacrelstats->new_dead_tuples);
			appendStringInfo(&buf,
						 _("buffer usage: %d hits, %d misses, %d dirtied\n"),
							 VacuumPageHit,
							 VacuumPageMiss,
							 VacuumPageDirty);
			appendStringInfo(&buf, _("avg read rate: %.3f MB/s, avg write rate: %.3f MB/s\n"),
							 read_rate, write_rate);
			appendStringInfo(&buf, _("system usage: %s"), pg_rusage_show(&ru0));

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
 *		If there are no indexes then we can reclaim line pointers on the fly;
 *		dead line pointers need only be retained until all index pointers that
 *		reference them have been killed.
 */
static void
lazy_scan_heap(Relation onerel, LVRelStats *vacrelstats,
			   Relation *Irel, int nindexes, bool scan_all)
{
	BlockNumber nblocks,
				blkno;
	HeapTupleData tuple;
	char	   *relname;
	BlockNumber empty_pages,
				vacuumed_pages;
	double		num_tuples,
				tups_vacuumed,
				nkeep,
				nunused;
	IndexBulkDeleteResult **indstats;
	int			i;
	PGRUsage	ru0;
	Buffer		vmbuffer = InvalidBuffer;
	BlockNumber next_not_all_visible_block;
	bool		skipping_all_visible_blocks;
	xl_heap_freeze_tuple *frozen;
	StringInfoData buf;

	pg_rusage_init(&ru0);

	relname = RelationGetRelationName(onerel);
	ereport(elevel,
			(errmsg("vacuuming \"%s.%s\"",
					get_namespace_name(RelationGetNamespace(onerel)),
					relname)));

	empty_pages = vacuumed_pages = 0;
	num_tuples = tups_vacuumed = nkeep = nunused = 0;

	indstats = (IndexBulkDeleteResult **)
		palloc0(nindexes * sizeof(IndexBulkDeleteResult *));

	nblocks = RelationGetNumberOfBlocks(onerel);
	vacrelstats->rel_pages = nblocks;
	vacrelstats->scanned_pages = 0;
	vacrelstats->nonempty_pages = 0;
	vacrelstats->latestRemovedXid = InvalidTransactionId;

	lazy_space_alloc(vacrelstats, nblocks);
	frozen = palloc(sizeof(xl_heap_freeze_tuple) * MaxHeapTuplesPerPage);

	/*
	 * We want to skip pages that don't require vacuuming according to the
	 * visibility map, but only when we can skip at least SKIP_PAGES_THRESHOLD
	 * consecutive pages.  Since we're reading sequentially, the OS should be
	 * doing readahead for us, so there's no gain in skipping a page now and
	 * then; that's likely to disable readahead and so be counterproductive.
	 * Also, skipping even a single page means that we can't update
	 * relfrozenxid, so we only want to do it if we can skip a goodly number
	 * of pages.
	 *
	 * Before entering the main loop, establish the invariant that
	 * next_not_all_visible_block is the next block number >= blkno that's not
	 * all-visible according to the visibility map, or nblocks if there's no
	 * such block.  Also, we set up the skipping_all_visible_blocks flag,
	 * which is needed because we need hysteresis in the decision: once we've
	 * started skipping blocks, we may as well skip everything up to the next
	 * not-all-visible block.
	 *
	 * Note: if scan_all is true, we won't actually skip any pages; but we
	 * maintain next_not_all_visible_block anyway, so as to set up the
	 * all_visible_according_to_vm flag correctly for each page.
	 *
	 * Note: The value returned by visibilitymap_test could be slightly
	 * out-of-date, since we make this test before reading the corresponding
	 * heap page or locking the buffer.  This is OK.  If we mistakenly think
	 * that the page is all-visible when in fact the flag's just been cleared,
	 * we might fail to vacuum the page.  But it's OK to skip pages when
	 * scan_all is not set, so no great harm done; the next vacuum will find
	 * them.  If we make the reverse mistake and vacuum a page unnecessarily,
	 * it'll just be a no-op.
	 */
	for (next_not_all_visible_block = 0;
		 next_not_all_visible_block < nblocks;
		 next_not_all_visible_block++)
	{
		if (!visibilitymap_test(onerel, next_not_all_visible_block, &vmbuffer))
			break;
		vacuum_delay_point();
	}
	if (next_not_all_visible_block >= SKIP_PAGES_THRESHOLD)
		skipping_all_visible_blocks = true;
	else
		skipping_all_visible_blocks = false;

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
		bool		all_visible_according_to_vm;
		bool		all_visible;
		bool		has_dead_tuples;
		TransactionId visibility_cutoff_xid = InvalidTransactionId;

		if (blkno == next_not_all_visible_block)
		{
			/* Time to advance next_not_all_visible_block */
			for (next_not_all_visible_block++;
				 next_not_all_visible_block < nblocks;
				 next_not_all_visible_block++)
			{
				if (!visibilitymap_test(onerel, next_not_all_visible_block,
										&vmbuffer))
					break;
				vacuum_delay_point();
			}

			/*
			 * We know we can't skip the current block.  But set up
			 * skipping_all_visible_blocks to do the right thing at the
			 * following blocks.
			 */
			if (next_not_all_visible_block - blkno > SKIP_PAGES_THRESHOLD)
				skipping_all_visible_blocks = true;
			else
				skipping_all_visible_blocks = false;
			all_visible_according_to_vm = false;
		}
		else
		{
			/* Current block is all-visible */
			if (skipping_all_visible_blocks && !scan_all)
				continue;
			all_visible_according_to_vm = true;
		}

		vacuum_delay_point();

		/*
		 * If we are close to overrunning the available space for dead-tuple
		 * TIDs, pause and do a cycle of vacuuming before we tackle this page.
		 */
		if ((vacrelstats->max_dead_tuples - vacrelstats->num_dead_tuples) < MaxHeapTuplesPerPage &&
			vacrelstats->num_dead_tuples > 0)
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

			/* Log cleanup info before we touch indexes */
			vacuum_log_cleanup_info(onerel, vacrelstats);

			/* Remove index entries */
			for (i = 0; i < nindexes; i++)
				lazy_vacuum_index(Irel[i],
								  &indstats[i],
								  vacrelstats);
			/* Remove tuples from heap */
			lazy_vacuum_heap(onerel, vacrelstats);

			/*
			 * Forget the now-vacuumed tuples, and press on, but be careful
			 * not to reset latestRemovedXid since we want that value to be
			 * valid.
			 */
			vacrelstats->num_dead_tuples = 0;
			vacrelstats->num_index_scans++;
		}

		/*
		 * Pin the visibility map page in case we need to mark the page
		 * all-visible.  In most cases this will be very cheap, because we'll
		 * already have the correct page pinned anyway.  However, it's
		 * possible that (a) next_not_all_visible_block is covered by a
		 * different VM page than the current block or (b) we released our pin
		 * and did a cycle of index vacuuming.
		 */
		visibilitymap_pin(onerel, blkno, &vmbuffer);

		buf = ReadBufferExtended(onerel, MAIN_FORKNUM, blkno,
								 RBM_NORMAL, vac_strategy);

		/* We need buffer cleanup lock so that we can prune HOT chains. */
		if (!ConditionalLockBufferForCleanup(buf))
		{
			/*
			 * If we're not scanning the whole relation to guard against XID
			 * wraparound, it's OK to skip vacuuming a page.  The next vacuum
			 * will clean it up.
			 */
			if (!scan_all)
			{
				ReleaseBuffer(buf);
				vacrelstats->pinskipped_pages++;
				continue;
			}

			/*
			 * If this is a wraparound checking vacuum, then we read the page
			 * with share lock to see if any xids need to be frozen. If the
			 * page doesn't need attention we just skip and continue. If it
			 * does, we wait for cleanup lock.
			 *
			 * We could defer the lock request further by remembering the page
			 * and coming back to it later, or we could even register
			 * ourselves for multiple buffers and then service whichever one
			 * is received first.  For now, this seems good enough.
			 */
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			if (!lazy_check_needs_freeze(buf))
			{
				UnlockReleaseBuffer(buf);
				vacrelstats->scanned_pages++;
				vacrelstats->pinskipped_pages++;
				continue;
			}
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			LockBufferForCleanup(buf);
			/* drop through to normal processing */
		}

		vacrelstats->scanned_pages++;

		page = BufferGetPage(buf);

		if (PageIsNew(page))
		{
			/*
			 * An all-zeroes page could be left over if a backend extends the
			 * relation but crashes before initializing the page. Reclaim such
			 * pages for use.
			 *
			 * We have to be careful here because we could be looking at a
			 * page that someone has just added to the relation and not yet
			 * been able to initialize (see RelationGetBufferForTuple). To
			 * protect against that, release the buffer lock, grab the
			 * relation extension lock momentarily, and re-lock the buffer. If
			 * the page is still uninitialized by then, it must be left over
			 * from a crashed backend, and we can initialize it.
			 *
			 * We don't really need the relation lock when this is a new or
			 * temp relation, but it's probably not worth the code space to
			 * check that, since this surely isn't a critical path.
			 *
			 * Note: the comparable code in vacuum.c need not worry because
			 * it's got exclusive lock on the whole relation.
			 */
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			LockRelationForExtension(onerel, ExclusiveLock);
			UnlockRelationForExtension(onerel, ExclusiveLock);
			LockBufferForCleanup(buf);
			if (PageIsNew(page))
			{
				ereport(WARNING,
				(errmsg("relation \"%s\" page %u is uninitialized --- fixing",
						relname, blkno)));
				PageInit(page, BufferGetPageSize(buf), 0);
				empty_pages++;
			}
			freespace = PageGetHeapFreeSpace(page);
			MarkBufferDirty(buf);
			UnlockReleaseBuffer(buf);

			RecordPageWithFreeSpace(onerel, blkno, freespace);
			continue;
		}

		if (PageIsEmpty(page))
		{
			empty_pages++;
			freespace = PageGetHeapFreeSpace(page);

			/* empty pages are always all-visible */
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
								  vmbuffer, InvalidTransactionId);
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
		prev_dead_count = vacrelstats->num_dead_tuples;
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
			 * DEAD item pointers are to be vacuumed normally; but we don't
			 * count them in tups_vacuumed, else we'd be double-counting (at
			 * least in the common case where heap_page_prune() just freed up
			 * a non-HOT tuple).
			 */
			if (ItemIdIsDead(itemid))
			{
				lazy_record_dead_tuple(vacrelstats, &(tuple.t_self));
				all_visible = false;
				continue;
			}

			Assert(ItemIdIsNormal(itemid));

			tuple.t_data = (HeapTupleHeader) PageGetItem(page, itemid);
			tuple.t_len = ItemIdGetLength(itemid);
			tuple.t_tableOid = RelationGetRelid(onerel);

			tupgone = false;

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
					 * to treat it like an indexed tuple.
					 */
					if (HeapTupleIsHotUpdated(&tuple) ||
						HeapTupleIsHeapOnly(&tuple))
						nkeep += 1;
					else
						tupgone = true; /* we can delete the tuple */
					all_visible = false;
					break;
				case HEAPTUPLE_LIVE:
					/* Tuple is good --- but let's do some validity checks */
					if (onerel->rd_rel->relhasoids &&
						!OidIsValid(HeapTupleGetOid(&tuple)))
						elog(WARNING, "relation \"%s\" TID %u/%u: OID is invalid",
							 relname, blkno, offnum);

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
					/* This is an expected case during concurrent vacuum */
					all_visible = false;
					break;
				case HEAPTUPLE_DELETE_IN_PROGRESS:
					/* This is an expected case during concurrent vacuum */
					all_visible = false;
					break;
				default:
					elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
					break;
			}

			if (tupgone)
			{
				lazy_record_dead_tuple(vacrelstats, &(tuple.t_self));
				HeapTupleHeaderAdvanceLatestRemovedXid(tuple.t_data,
											 &vacrelstats->latestRemovedXid);
				tups_vacuumed += 1;
				has_dead_tuples = true;
			}
			else
			{
				num_tuples += 1;
				hastup = true;

				/*
				 * Each non-removable tuple must be checked to see if it needs
				 * freezing.  Note we already have exclusive buffer lock.
				 */
				if (heap_prepare_freeze_tuple(tuple.t_data, FreezeLimit,
										  MultiXactCutoff, &frozen[nfrozen]))
					frozen[nfrozen++].offset = offnum;
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
		 * If there are no indexes then we can vacuum the page right now
		 * instead of doing a second scan.
		 */
		if (nindexes == 0 &&
			vacrelstats->num_dead_tuples > 0)
		{
			/* Remove tuples from heap */
			lazy_vacuum_page(onerel, blkno, buf, 0, vacrelstats, &vmbuffer);
			has_dead_tuples = false;

			/*
			 * Forget the now-vacuumed tuples, and press on, but be careful
			 * not to reset latestRemovedXid since we want that value to be
			 * valid.
			 */
			vacrelstats->num_dead_tuples = 0;
			vacuumed_pages++;
		}

		freespace = PageGetHeapFreeSpace(page);

		/* mark page all-visible, if appropriate */
		if (all_visible && !all_visible_according_to_vm)
		{
			/*
			 * It should never be the case that the visibility map page is set
			 * while the page-level bit is clear, but the reverse is allowed
			 * (if checksums are not enabled).  Regardless, set the both bits
			 * so that we get back in sync.
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
							  vmbuffer, visibility_cutoff_xid);
		}

		/*
		 * As of PostgreSQL 9.2, the visibility map bit should never be set if
		 * the page-level bit is clear.  However, it's possible that the bit
		 * got cleared after we checked it and before we took the buffer
		 * content lock, so we must recheck before jumping to the conclusion
		 * that something bad has happened.
		 */
		else if (all_visible_according_to_vm && !PageIsAllVisible(page)
				 && visibilitymap_test(onerel, blkno, &vmbuffer))
		{
			elog(WARNING, "page is not marked all-visible but visibility map bit is set in relation \"%s\" page %u",
				 relname, blkno);
			visibilitymap_clear(onerel, blkno, vmbuffer);
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
				 relname, blkno);
			PageClearAllVisible(page);
			MarkBufferDirty(buf);
			visibilitymap_clear(onerel, blkno, vmbuffer);
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
		if (vacrelstats->num_dead_tuples == prev_dead_count)
			RecordPageWithFreeSpace(onerel, blkno, freespace);
	}

	pfree(frozen);

	/* save stats for use later */
	vacrelstats->scanned_tuples = num_tuples;
	vacrelstats->tuples_deleted = tups_vacuumed;
	vacrelstats->new_dead_tuples = nkeep;

	/* now we can compute the new value for pg_class.reltuples */
	vacrelstats->new_rel_tuples = vac_estimate_reltuples(onerel, false,
														 nblocks,
												  vacrelstats->scanned_pages,
														 num_tuples);

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
	if (vacrelstats->num_dead_tuples > 0)
	{
		/* Log cleanup info before we touch indexes */
		vacuum_log_cleanup_info(onerel, vacrelstats);

		/* Remove index entries */
		for (i = 0; i < nindexes; i++)
			lazy_vacuum_index(Irel[i],
							  &indstats[i],
							  vacrelstats);
		/* Remove tuples from heap */
		lazy_vacuum_heap(onerel, vacrelstats);
		vacrelstats->num_index_scans++;
	}

	/* Do post-vacuum cleanup and statistics update for each index */
	for (i = 0; i < nindexes; i++)
		lazy_cleanup_index(Irel[i], indstats[i], vacrelstats);

	/* If no indexes, make log report that lazy_vacuum_heap would've made */
	if (vacuumed_pages)
		ereport(elevel,
				(errmsg("\"%s\": removed %.0f row versions in %u pages",
						RelationGetRelationName(onerel),
						tups_vacuumed, vacuumed_pages)));

	/*
	 * This is pretty messy, but we split it up so that we can skip emitting
	 * individual parts of the message when not applicable.
	 */
	initStringInfo(&buf);
	appendStringInfo(&buf,
					 _("%.0f dead row versions cannot be removed yet.\n"),
					 nkeep);
	appendStringInfo(&buf, _("There were %.0f unused item pointers.\n"),
					 nunused);
	appendStringInfo(&buf, ngettext("Skipped %u page due to buffer pins.\n",
									"Skipped %u pages due to buffer pins.\n",
									vacrelstats->pinskipped_pages),
					 vacrelstats->pinskipped_pages);
	appendStringInfo(&buf, ngettext("%u page is entirely empty.\n",
									"%u pages are entirely empty.\n",
									empty_pages),
					 empty_pages);
	appendStringInfo(&buf, _("%s."),
					 pg_rusage_show(&ru0));

	ereport(elevel,
			(errmsg("\"%s\": found %.0f removable, %.0f nonremovable row versions in %u out of %u pages",
					RelationGetRelationName(onerel),
					tups_vacuumed, num_tuples,
					vacrelstats->scanned_pages, nblocks),
			 errdetail_internal("%s", buf.data)));
	pfree(buf.data);
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

	pg_rusage_init(&ru0);
	npages = 0;

	tupindex = 0;
	while (tupindex < vacrelstats->num_dead_tuples)
	{
		BlockNumber tblk;
		Buffer		buf;
		Page		page;
		Size		freespace;

		vacuum_delay_point();

		tblk = ItemPointerGetBlockNumber(&vacrelstats->dead_tuples[tupindex]);
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

	if (BufferIsValid(vmbuffer))
	{
		ReleaseBuffer(vmbuffer);
		vmbuffer = InvalidBuffer;
	}

	ereport(elevel,
			(errmsg("\"%s\": removed %d row versions in %d pages",
					RelationGetRelationName(onerel),
					tupindex, npages),
			 errdetail("%s.",
					   pg_rusage_show(&ru0))));
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
	Page		page = BufferGetPage(buffer);
	OffsetNumber unused[MaxOffsetNumber];
	int			uncnt = 0;
	TransactionId visibility_cutoff_xid;

	START_CRIT_SECTION();

	for (; tupindex < vacrelstats->num_dead_tuples; tupindex++)
	{
		BlockNumber tblk;
		OffsetNumber toff;
		ItemId		itemid;

		tblk = ItemPointerGetBlockNumber(&vacrelstats->dead_tuples[tupindex]);
		if (tblk != blkno)
			break;				/* past end of tuples for this block */
		toff = ItemPointerGetOffsetNumber(&vacrelstats->dead_tuples[tupindex]);
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
	if (heap_page_is_all_visible(onerel, buffer, &visibility_cutoff_xid))
		PageSetAllVisible(page);

	/*
	 * All the changes to the heap page have been done. If the all-visible
	 * flag is now set, also set the VM bit.
	 */
	if (PageIsAllVisible(page) &&
		!visibilitymap_test(onerel, blkno, vmbuffer))
	{
		Assert(BufferIsValid(*vmbuffer));
		visibilitymap_set(onerel, blkno, buffer, InvalidXLogRecPtr, *vmbuffer,
						  visibility_cutoff_xid);
	}

	return tupindex;
}

/*
 *	lazy_check_needs_freeze() -- scan page to see if any tuples
 *					 need to be cleaned to avoid wraparound
 *
 * Returns true if the page needs to be vacuumed using cleanup lock.
 */
static bool
lazy_check_needs_freeze(Buffer buf)
{
	Page		page;
	OffsetNumber offnum,
				maxoff;
	HeapTupleHeader tupleheader;

	page = BufferGetPage(buf);

	if (PageIsNew(page) || PageIsEmpty(page))
	{
		/* PageIsNew probably shouldn't happen... */
		return false;
	}

	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = FirstOffsetNumber;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemid;

		itemid = PageGetItemId(page, offnum);

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
 *	lazy_vacuum_index() -- vacuum one index relation.
 *
 *		Delete all the index entries pointing to tuples listed in
 *		vacrelstats->dead_tuples, and update running statistics.
 */
static void
lazy_vacuum_index(Relation indrel,
				  IndexBulkDeleteResult **stats,
				  LVRelStats *vacrelstats)
{
	IndexVacuumInfo ivinfo;
	PGRUsage	ru0;

	pg_rusage_init(&ru0);

	ivinfo.index = indrel;
	ivinfo.analyze_only = false;
	ivinfo.estimated_count = true;
	ivinfo.message_level = elevel;
	ivinfo.num_heap_tuples = vacrelstats->old_rel_tuples;
	ivinfo.strategy = vac_strategy;

	/* Do bulk deletion */
	*stats = index_bulk_delete(&ivinfo, *stats,
							   lazy_tid_reaped, (void *) vacrelstats);

	ereport(elevel,
			(errmsg("scanned index \"%s\" to remove %d row versions",
					RelationGetRelationName(indrel),
					vacrelstats->num_dead_tuples),
			 errdetail("%s.", pg_rusage_show(&ru0))));
}

/*
 *	lazy_cleanup_index() -- do post-vacuum cleanup for one index relation.
 */
static void
lazy_cleanup_index(Relation indrel,
				   IndexBulkDeleteResult *stats,
				   LVRelStats *vacrelstats)
{
	IndexVacuumInfo ivinfo;
	PGRUsage	ru0;

	pg_rusage_init(&ru0);

	ivinfo.index = indrel;
	ivinfo.analyze_only = false;
	ivinfo.estimated_count = (vacrelstats->scanned_pages < vacrelstats->rel_pages);
	ivinfo.message_level = elevel;
	ivinfo.num_heap_tuples = vacrelstats->new_rel_tuples;
	ivinfo.strategy = vac_strategy;

	stats = index_vacuum_cleanup(&ivinfo, stats);

	if (!stats)
		return;

	/*
	 * Now update statistics in pg_class, but only if the index says the count
	 * is accurate.
	 */
	if (!stats->estimated_count)
		vac_update_relstats(indrel,
							stats->num_pages,
							stats->num_index_tuples,
							0,
							false,
							InvalidTransactionId,
							InvalidMultiXactId,
							false);

	ereport(elevel,
			(errmsg("index \"%s\" now contains %.0f row versions in %u pages",
					RelationGetRelationName(indrel),
					stats->num_index_tuples,
					stats->num_pages),
			 errdetail("%.0f index row versions were removed.\n"
			 "%u index pages have been deleted, %u are currently reusable.\n"
					   "%s.",
					   stats->tuples_removed,
					   stats->pages_deleted, stats->pages_free,
					   pg_rusage_show(&ru0))));

	pfree(stats);
}

/*
 * lazy_truncate_heap - try to truncate off any empty pages at the end
 */
static void
lazy_truncate_heap(Relation onerel, LVRelStats *vacrelstats)
{
	BlockNumber old_rel_pages = vacrelstats->rel_pages;
	BlockNumber new_rel_pages;
	PGRUsage	ru0;
	int			lock_retry;

	pg_rusage_init(&ru0);

	/*
	 * Loop until no more truncating can be done.
	 */
	do
	{
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
								RelationGetRelationName(onerel))));
				return;
			}

			pg_usleep(VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL);
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
						RelationGetRelationName(onerel),
						old_rel_pages, new_rel_pages),
				 errdetail("%s.",
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
	instr_time	starttime;

	/* Initialize the starttime if we check for conflicting lock requests */
	INSTR_TIME_SET_CURRENT(starttime);

	/* Strange coding of loop control is needed because blkno is unsigned */
	blkno = vacrelstats->rel_pages;
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
									RelationGetRelationName(onerel))));

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

		buf = ReadBufferExtended(onerel, MAIN_FORKNUM, blkno,
								 RBM_NORMAL, vac_strategy);

		/* In this phase we only need shared access to the buffer */
		LockBuffer(buf, BUFFER_LOCK_SHARE);

		page = BufferGetPage(buf);

		if (PageIsNew(page) || PageIsEmpty(page))
		{
			/* PageIsNew probably shouldn't happen... */
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
 * lazy_space_alloc - space allocation decisions for lazy vacuum
 *
 * See the comments at the head of this file for rationale.
 */
static void
lazy_space_alloc(LVRelStats *vacrelstats, BlockNumber relblocks)
{
	long		maxtuples;
	int			vac_work_mem = IsAutoVacuumWorkerProcess() &&
	autovacuum_work_mem != -1 ?
	autovacuum_work_mem : maintenance_work_mem;

	if (vacrelstats->hasindex)
	{
		maxtuples = (vac_work_mem * 1024L) / sizeof(ItemPointerData);
		maxtuples = Min(maxtuples, INT_MAX);
		maxtuples = Min(maxtuples, MaxAllocSize / sizeof(ItemPointerData));

		/* curious coding here to ensure the multiplication can't overflow */
		if ((BlockNumber) (maxtuples / LAZY_ALLOC_TUPLES) > relblocks)
			maxtuples = relblocks * LAZY_ALLOC_TUPLES;

		/* stay sane if small maintenance_work_mem */
		maxtuples = Max(maxtuples, MaxHeapTuplesPerPage);
	}
	else
	{
		maxtuples = MaxHeapTuplesPerPage;
	}

	vacrelstats->num_dead_tuples = 0;
	vacrelstats->max_dead_tuples = (int) maxtuples;
	vacrelstats->dead_tuples = (ItemPointer)
		palloc(maxtuples * sizeof(ItemPointerData));
}

/*
 * lazy_record_dead_tuple - remember one deletable tuple
 */
static void
lazy_record_dead_tuple(LVRelStats *vacrelstats,
					   ItemPointer itemptr)
{
	/*
	 * The array shouldn't overflow under normal behavior, but perhaps it
	 * could if we are given a really small maintenance_work_mem. In that
	 * case, just forget the last few tuples (we'll get 'em next time).
	 */
	if (vacrelstats->num_dead_tuples < vacrelstats->max_dead_tuples)
	{
		vacrelstats->dead_tuples[vacrelstats->num_dead_tuples] = *itemptr;
		vacrelstats->num_dead_tuples++;
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
	LVRelStats *vacrelstats = (LVRelStats *) state;
	ItemPointer res;

	res = (ItemPointer) bsearch((void *) itemptr,
								(void *) vacrelstats->dead_tuples,
								vacrelstats->num_dead_tuples,
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
 * xmin amongst the visible tuples.
 */
static bool
heap_page_is_all_visible(Relation rel, Buffer buf, TransactionId *visibility_cutoff_xid)
{
	Page		page = BufferGetPage(buf);
	BlockNumber blockno = BufferGetBlockNumber(buf);
	OffsetNumber offnum,
				maxoff;
	bool		all_visible = true;

	*visibility_cutoff_xid = InvalidTransactionId;

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
						break;
					}

					/* Track newest xmin on page. */
					if (TransactionIdFollows(xmin, *visibility_cutoff_xid))
						*visibility_cutoff_xid = xmin;
				}
				break;

			case HEAPTUPLE_DEAD:
			case HEAPTUPLE_RECENTLY_DEAD:
			case HEAPTUPLE_INSERT_IN_PROGRESS:
			case HEAPTUPLE_DELETE_IN_PROGRESS:
				all_visible = false;
				break;

			default:
				elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
				break;
		}
	}							/* scan along page */

	return all_visible;
}
