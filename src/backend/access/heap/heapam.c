/*-------------------------------------------------------------------------
 *
 * heapam.c
 *	  heap access method code
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/heapam.c
 *
 *
 * INTERFACE ROUTINES
 *		heap_beginscan	- begin relation scan
 *		heap_rescan		- restart a relation scan
 *		heap_endscan	- end relation scan
 *		heap_getnext	- retrieve next tuple in scan
 *		heap_fetch		- retrieve tuple with given tid
 *		heap_insert		- insert tuple into a relation
 *		heap_multi_insert - insert multiple tuples into a relation
 *		heap_delete		- delete a tuple from a relation
 *		heap_update		- replace a tuple in a relation with another tuple
 *
 * NOTES
 *	  This file contains the heap_ routines which implement
 *	  the POSTGRES heap access method used for all POSTGRES
 *	  relations.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/heaptoast.h"
#include "access/hio.h"
#include "access/multixact.h"
#include "access/subtrans.h"
#include "access/syncscan.h"
#include "access/valid.h"
#include "access/visibilitymap.h"
#include "access/xloginsert.h"
#include "catalog/pg_database.h"
#include "catalog/pg_database_d.h"
#include "commands/vacuum.h"
#include "pgstat.h"
#include "port/pg_bitutils.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "utils/datum.h"
#include "utils/injection_point.h"
#include "utils/inval.h"
#include "utils/spccache.h"
#include "utils/syscache.h"


static HeapTuple heap_prepare_insert(Relation relation, HeapTuple tup,
									 TransactionId xid, CommandId cid, int options);
static XLogRecPtr log_heap_update(Relation reln, Buffer oldbuf,
								  Buffer newbuf, HeapTuple oldtup,
								  HeapTuple newtup, HeapTuple old_key_tuple,
								  bool all_visible_cleared, bool new_all_visible_cleared);
#ifdef USE_ASSERT_CHECKING
static void check_lock_if_inplace_updateable_rel(Relation relation,
												 ItemPointer otid,
												 HeapTuple newtup);
static void check_inplace_rel_lock(HeapTuple oldtup);
#endif
static Bitmapset *HeapDetermineColumnsInfo(Relation relation,
										   Bitmapset *interesting_cols,
										   Bitmapset *external_cols,
										   HeapTuple oldtup, HeapTuple newtup,
										   bool *has_external);
static bool heap_acquire_tuplock(Relation relation, ItemPointer tid,
								 LockTupleMode mode, LockWaitPolicy wait_policy,
								 bool *have_tuple_lock);
static inline BlockNumber heapgettup_advance_block(HeapScanDesc scan,
												   BlockNumber block,
												   ScanDirection dir);
static pg_noinline BlockNumber heapgettup_initial_block(HeapScanDesc scan,
														ScanDirection dir);
static void compute_new_xmax_infomask(TransactionId xmax, uint16 old_infomask,
									  uint16 old_infomask2, TransactionId add_to_xmax,
									  LockTupleMode mode, bool is_update,
									  TransactionId *result_xmax, uint16 *result_infomask,
									  uint16 *result_infomask2);
static TM_Result heap_lock_updated_tuple(Relation rel, HeapTuple tuple,
										 ItemPointer ctid, TransactionId xid,
										 LockTupleMode mode);
static void GetMultiXactIdHintBits(MultiXactId multi, uint16 *new_infomask,
								   uint16 *new_infomask2);
static TransactionId MultiXactIdGetUpdateXid(TransactionId xmax,
											 uint16 t_infomask);
static bool DoesMultiXactIdConflict(MultiXactId multi, uint16 infomask,
									LockTupleMode lockmode, bool *current_is_member);
static void MultiXactIdWait(MultiXactId multi, MultiXactStatus status, uint16 infomask,
							Relation rel, ItemPointer ctid, XLTW_Oper oper,
							int *remaining);
static bool ConditionalMultiXactIdWait(MultiXactId multi, MultiXactStatus status,
									   uint16 infomask, Relation rel, int *remaining,
									   bool logLockFailure);
static void index_delete_sort(TM_IndexDeleteOp *delstate);
static int	bottomup_sort_and_shrink(TM_IndexDeleteOp *delstate);
static XLogRecPtr log_heap_new_cid(Relation relation, HeapTuple tup);
static HeapTuple ExtractReplicaIdentity(Relation relation, HeapTuple tp, bool key_required,
										bool *copy);


/*
 * Each tuple lock mode has a corresponding heavyweight lock, and one or two
 * corresponding MultiXactStatuses (one to merely lock tuples, another one to
 * update them).  This table (and the macros below) helps us determine the
 * heavyweight lock mode and MultiXactStatus values to use for any particular
 * tuple lock strength.
 *
 * These interact with InplaceUpdateTupleLock, an alias for ExclusiveLock.
 *
 * Don't look at lockstatus/updstatus directly!  Use get_mxact_status_for_lock
 * instead.
 */
static const struct
{
	LOCKMODE	hwlock;
	int			lockstatus;
	int			updstatus;
}

			tupleLockExtraInfo[MaxLockTupleMode + 1] =
{
	{							/* LockTupleKeyShare */
		AccessShareLock,
		MultiXactStatusForKeyShare,
		-1						/* KeyShare does not allow updating tuples */
	},
	{							/* LockTupleShare */
		RowShareLock,
		MultiXactStatusForShare,
		-1						/* Share does not allow updating tuples */
	},
	{							/* LockTupleNoKeyExclusive */
		ExclusiveLock,
		MultiXactStatusForNoKeyUpdate,
		MultiXactStatusNoKeyUpdate
	},
	{							/* LockTupleExclusive */
		AccessExclusiveLock,
		MultiXactStatusForUpdate,
		MultiXactStatusUpdate
	}
};

/* Get the LOCKMODE for a given MultiXactStatus */
#define LOCKMODE_from_mxstatus(status) \
			(tupleLockExtraInfo[TUPLOCK_from_mxstatus((status))].hwlock)

/*
 * Acquire heavyweight locks on tuples, using a LockTupleMode strength value.
 * This is more readable than having every caller translate it to lock.h's
 * LOCKMODE.
 */
#define LockTupleTuplock(rel, tup, mode) \
	LockTuple((rel), (tup), tupleLockExtraInfo[mode].hwlock)
#define UnlockTupleTuplock(rel, tup, mode) \
	UnlockTuple((rel), (tup), tupleLockExtraInfo[mode].hwlock)
#define ConditionalLockTupleTuplock(rel, tup, mode, log) \
	ConditionalLockTuple((rel), (tup), tupleLockExtraInfo[mode].hwlock, (log))

#ifdef USE_PREFETCH
/*
 * heap_index_delete_tuples and index_delete_prefetch_buffer use this
 * structure to coordinate prefetching activity
 */
typedef struct
{
	BlockNumber cur_hblkno;
	int			next_item;
	int			ndeltids;
	TM_IndexDelete *deltids;
} IndexDeletePrefetchState;
#endif

/* heap_index_delete_tuples bottom-up index deletion costing constants */
#define BOTTOMUP_MAX_NBLOCKS			6
#define BOTTOMUP_TOLERANCE_NBLOCKS		3

/*
 * heap_index_delete_tuples uses this when determining which heap blocks it
 * must visit to help its bottom-up index deletion caller
 */
typedef struct IndexDeleteCounts
{
	int16		npromisingtids; /* Number of "promising" TIDs in group */
	int16		ntids;			/* Number of TIDs in group */
	int16		ifirsttid;		/* Offset to group's first deltid */
} IndexDeleteCounts;

/*
 * This table maps tuple lock strength values for each particular
 * MultiXactStatus value.
 */
static const int MultiXactStatusLock[MaxMultiXactStatus + 1] =
{
	LockTupleKeyShare,			/* ForKeyShare */
	LockTupleShare,				/* ForShare */
	LockTupleNoKeyExclusive,	/* ForNoKeyUpdate */
	LockTupleExclusive,			/* ForUpdate */
	LockTupleNoKeyExclusive,	/* NoKeyUpdate */
	LockTupleExclusive			/* Update */
};

/* Get the LockTupleMode for a given MultiXactStatus */
#define TUPLOCK_from_mxstatus(status) \
			(MultiXactStatusLock[(status)])

/* ----------------------------------------------------------------
 *						 heap support routines
 * ----------------------------------------------------------------
 */

/*
 * Streaming read API callback for parallel sequential scans. Returns the next
 * block the caller wants from the read stream or InvalidBlockNumber when done.
 */
static BlockNumber
heap_scan_stream_read_next_parallel(ReadStream *stream,
									void *callback_private_data,
									void *per_buffer_data)
{
	HeapScanDesc scan = (HeapScanDesc) callback_private_data;

	Assert(ScanDirectionIsForward(scan->rs_dir));
	Assert(scan->rs_base.rs_parallel);

	if (unlikely(!scan->rs_inited))
	{
		/* parallel scan */
		table_block_parallelscan_startblock_init(scan->rs_base.rs_rd,
												 scan->rs_parallelworkerdata,
												 (ParallelBlockTableScanDesc) scan->rs_base.rs_parallel);

		/* may return InvalidBlockNumber if there are no more blocks */
		scan->rs_prefetch_block = table_block_parallelscan_nextpage(scan->rs_base.rs_rd,
																	scan->rs_parallelworkerdata,
																	(ParallelBlockTableScanDesc) scan->rs_base.rs_parallel);
		scan->rs_inited = true;
	}
	else
	{
		scan->rs_prefetch_block = table_block_parallelscan_nextpage(scan->rs_base.rs_rd,
																	scan->rs_parallelworkerdata, (ParallelBlockTableScanDesc)
																	scan->rs_base.rs_parallel);
	}

	return scan->rs_prefetch_block;
}

/*
 * Streaming read API callback for serial sequential and TID range scans.
 * Returns the next block the caller wants from the read stream or
 * InvalidBlockNumber when done.
 */
static BlockNumber
heap_scan_stream_read_next_serial(ReadStream *stream,
								  void *callback_private_data,
								  void *per_buffer_data)
{
	HeapScanDesc scan = (HeapScanDesc) callback_private_data;

	if (unlikely(!scan->rs_inited))
	{
		scan->rs_prefetch_block = heapgettup_initial_block(scan, scan->rs_dir);
		scan->rs_inited = true;
	}
	else
		scan->rs_prefetch_block = heapgettup_advance_block(scan,
														   scan->rs_prefetch_block,
														   scan->rs_dir);

	return scan->rs_prefetch_block;
}

/*
 * Read stream API callback for bitmap heap scans.
 * Returns the next block the caller wants from the read stream or
 * InvalidBlockNumber when done.
 */
static BlockNumber
bitmapheap_stream_read_next(ReadStream *pgsr, void *private_data,
							void *per_buffer_data)
{
	TBMIterateResult *tbmres = per_buffer_data;
	BitmapHeapScanDesc bscan = (BitmapHeapScanDesc) private_data;
	HeapScanDesc hscan = (HeapScanDesc) bscan;
	TableScanDesc sscan = &hscan->rs_base;

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		/* no more entries in the bitmap */
		if (!tbm_iterate(&sscan->st.rs_tbmiterator, tbmres))
			return InvalidBlockNumber;

		/*
		 * Ignore any claimed entries past what we think is the end of the
		 * relation. It may have been extended after the start of our scan (we
		 * only hold an AccessShareLock, and it could be inserts from this
		 * backend).  We don't take this optimization in SERIALIZABLE
		 * isolation though, as we need to examine all invisible tuples
		 * reachable by the index.
		 */
		if (!IsolationIsSerializable() &&
			tbmres->blockno >= hscan->rs_nblocks)
			continue;

		/*
		 * We can skip fetching the heap page if we don't need any fields from
		 * the heap, the bitmap entries don't need rechecking, and all tuples
		 * on the page are visible to our transaction.
		 */
		if (!(sscan->rs_flags & SO_NEED_TUPLES) &&
			!tbmres->recheck &&
			VM_ALL_VISIBLE(sscan->rs_rd, tbmres->blockno, &bscan->rs_vmbuffer))
		{
			OffsetNumber offsets[TBM_MAX_TUPLES_PER_PAGE];
			int			noffsets;

			/* can't be lossy in the skip_fetch case */
			Assert(!tbmres->lossy);
			Assert(bscan->rs_empty_tuples_pending >= 0);

			/*
			 * We throw away the offsets, but this is the easiest way to get a
			 * count of tuples.
			 */
			noffsets = tbm_extract_page_tuple(tbmres, offsets, TBM_MAX_TUPLES_PER_PAGE);
			bscan->rs_empty_tuples_pending += noffsets;
			continue;
		}

		return tbmres->blockno;
	}

	/* not reachable */
	Assert(false);
}

/* ----------------
 *		initscan - scan code common to heap_beginscan and heap_rescan
 * ----------------
 */
static void
initscan(HeapScanDesc scan, ScanKey key, bool keep_startblock)
{
	ParallelBlockTableScanDesc bpscan = NULL;
	bool		allow_strat;
	bool		allow_sync;

	/*
	 * Determine the number of blocks we have to scan.
	 *
	 * It is sufficient to do this once at scan start, since any tuples added
	 * while the scan is in progress will be invisible to my snapshot anyway.
	 * (That is not true when using a non-MVCC snapshot.  However, we couldn't
	 * guarantee to return tuples added after scan start anyway, since they
	 * might go into pages we already scanned.  To guarantee consistent
	 * results for a non-MVCC snapshot, the caller must hold some higher-level
	 * lock that ensures the interesting tuple(s) won't change.)
	 */
	if (scan->rs_base.rs_parallel != NULL)
	{
		bpscan = (ParallelBlockTableScanDesc) scan->rs_base.rs_parallel;
		scan->rs_nblocks = bpscan->phs_nblocks;
	}
	else
		scan->rs_nblocks = RelationGetNumberOfBlocks(scan->rs_base.rs_rd);

	/*
	 * If the table is large relative to NBuffers, use a bulk-read access
	 * strategy and enable synchronized scanning (see syncscan.c).  Although
	 * the thresholds for these features could be different, we make them the
	 * same so that there are only two behaviors to tune rather than four.
	 * (However, some callers need to be able to disable one or both of these
	 * behaviors, independently of the size of the table; also there is a GUC
	 * variable that can disable synchronized scanning.)
	 *
	 * Note that table_block_parallelscan_initialize has a very similar test;
	 * if you change this, consider changing that one, too.
	 */
	if (!RelationUsesLocalBuffers(scan->rs_base.rs_rd) &&
		scan->rs_nblocks > NBuffers / 4)
	{
		allow_strat = (scan->rs_base.rs_flags & SO_ALLOW_STRAT) != 0;
		allow_sync = (scan->rs_base.rs_flags & SO_ALLOW_SYNC) != 0;
	}
	else
		allow_strat = allow_sync = false;

	if (allow_strat)
	{
		/* During a rescan, keep the previous strategy object. */
		if (scan->rs_strategy == NULL)
			scan->rs_strategy = GetAccessStrategy(BAS_BULKREAD);
	}
	else
	{
		if (scan->rs_strategy != NULL)
			FreeAccessStrategy(scan->rs_strategy);
		scan->rs_strategy = NULL;
	}

	if (scan->rs_base.rs_parallel != NULL)
	{
		/* For parallel scan, believe whatever ParallelTableScanDesc says. */
		if (scan->rs_base.rs_parallel->phs_syncscan)
			scan->rs_base.rs_flags |= SO_ALLOW_SYNC;
		else
			scan->rs_base.rs_flags &= ~SO_ALLOW_SYNC;
	}
	else if (keep_startblock)
	{
		/*
		 * When rescanning, we want to keep the previous startblock setting,
		 * so that rewinding a cursor doesn't generate surprising results.
		 * Reset the active syncscan setting, though.
		 */
		if (allow_sync && synchronize_seqscans)
			scan->rs_base.rs_flags |= SO_ALLOW_SYNC;
		else
			scan->rs_base.rs_flags &= ~SO_ALLOW_SYNC;
	}
	else if (allow_sync && synchronize_seqscans)
	{
		scan->rs_base.rs_flags |= SO_ALLOW_SYNC;
		scan->rs_startblock = ss_get_location(scan->rs_base.rs_rd, scan->rs_nblocks);
	}
	else
	{
		scan->rs_base.rs_flags &= ~SO_ALLOW_SYNC;
		scan->rs_startblock = 0;
	}

	scan->rs_numblocks = InvalidBlockNumber;
	scan->rs_inited = false;
	scan->rs_ctup.t_data = NULL;
	ItemPointerSetInvalid(&scan->rs_ctup.t_self);
	scan->rs_cbuf = InvalidBuffer;
	scan->rs_cblock = InvalidBlockNumber;
	scan->rs_ntuples = 0;
	scan->rs_cindex = 0;

	/*
	 * Initialize to ForwardScanDirection because it is most common and
	 * because heap scans go forward before going backward (e.g. CURSORs).
	 */
	scan->rs_dir = ForwardScanDirection;
	scan->rs_prefetch_block = InvalidBlockNumber;

	/* page-at-a-time fields are always invalid when not rs_inited */

	/*
	 * copy the scan key, if appropriate
	 */
	if (key != NULL && scan->rs_base.rs_nkeys > 0)
		memcpy(scan->rs_base.rs_key, key, scan->rs_base.rs_nkeys * sizeof(ScanKeyData));

	/*
	 * Currently, we only have a stats counter for sequential heap scans (but
	 * e.g for bitmap scans the underlying bitmap index scans will be counted,
	 * and for sample scans we update stats for tuple fetches).
	 */
	if (scan->rs_base.rs_flags & SO_TYPE_SEQSCAN)
		pgstat_count_heap_scan(scan->rs_base.rs_rd);
}

/*
 * heap_setscanlimits - restrict range of a heapscan
 *
 * startBlk is the page to start at
 * numBlks is number of pages to scan (InvalidBlockNumber means "all")
 */
void
heap_setscanlimits(TableScanDesc sscan, BlockNumber startBlk, BlockNumber numBlks)
{
	HeapScanDesc scan = (HeapScanDesc) sscan;

	Assert(!scan->rs_inited);	/* else too late to change */
	/* else rs_startblock is significant */
	Assert(!(scan->rs_base.rs_flags & SO_ALLOW_SYNC));

	/* Check startBlk is valid (but allow case of zero blocks...) */
	Assert(startBlk == 0 || startBlk < scan->rs_nblocks);

	scan->rs_startblock = startBlk;
	scan->rs_numblocks = numBlks;
}

/*
 * Per-tuple loop for heap_prepare_pagescan(). Pulled out so it can be called
 * multiple times, with constant arguments for all_visible,
 * check_serializable.
 */
pg_attribute_always_inline
static int
page_collect_tuples(HeapScanDesc scan, Snapshot snapshot,
					Page page, Buffer buffer,
					BlockNumber block, int lines,
					bool all_visible, bool check_serializable)
{
	int			ntup = 0;
	OffsetNumber lineoff;

	for (lineoff = FirstOffsetNumber; lineoff <= lines; lineoff++)
	{
		ItemId		lpp = PageGetItemId(page, lineoff);
		HeapTupleData loctup;
		bool		valid;

		if (!ItemIdIsNormal(lpp))
			continue;

		loctup.t_data = (HeapTupleHeader) PageGetItem(page, lpp);
		loctup.t_len = ItemIdGetLength(lpp);
		loctup.t_tableOid = RelationGetRelid(scan->rs_base.rs_rd);
		ItemPointerSet(&(loctup.t_self), block, lineoff);

		if (all_visible)
			valid = true;
		else
			valid = HeapTupleSatisfiesVisibility(&loctup, snapshot, buffer);

		if (check_serializable)
			HeapCheckForSerializableConflictOut(valid, scan->rs_base.rs_rd,
												&loctup, buffer, snapshot);

		if (valid)
		{
			scan->rs_vistuples[ntup] = lineoff;
			ntup++;
		}
	}

	Assert(ntup <= MaxHeapTuplesPerPage);

	return ntup;
}

/*
 * heap_prepare_pagescan - Prepare current scan page to be scanned in pagemode
 *
 * Preparation currently consists of 1. prune the scan's rs_cbuf page, and 2.
 * fill the rs_vistuples[] array with the OffsetNumbers of visible tuples.
 */
void
heap_prepare_pagescan(TableScanDesc sscan)
{
	HeapScanDesc scan = (HeapScanDesc) sscan;
	Buffer		buffer = scan->rs_cbuf;
	BlockNumber block = scan->rs_cblock;
	Snapshot	snapshot;
	Page		page;
	int			lines;
	bool		all_visible;
	bool		check_serializable;

	Assert(BufferGetBlockNumber(buffer) == block);

	/* ensure we're not accidentally being used when not in pagemode */
	Assert(scan->rs_base.rs_flags & SO_ALLOW_PAGEMODE);
	snapshot = scan->rs_base.rs_snapshot;

	/*
	 * Prune and repair fragmentation for the whole page, if possible.
	 */
	heap_page_prune_opt(scan->rs_base.rs_rd, buffer);

	/*
	 * We must hold share lock on the buffer content while examining tuple
	 * visibility.  Afterwards, however, the tuples we have found to be
	 * visible are guaranteed good as long as we hold the buffer pin.
	 */
	LockBuffer(buffer, BUFFER_LOCK_SHARE);

	page = BufferGetPage(buffer);
	lines = PageGetMaxOffsetNumber(page);

	/*
	 * If the all-visible flag indicates that all tuples on the page are
	 * visible to everyone, we can skip the per-tuple visibility tests.
	 *
	 * Note: In hot standby, a tuple that's already visible to all
	 * transactions on the primary might still be invisible to a read-only
	 * transaction in the standby. We partly handle this problem by tracking
	 * the minimum xmin of visible tuples as the cut-off XID while marking a
	 * page all-visible on the primary and WAL log that along with the
	 * visibility map SET operation. In hot standby, we wait for (or abort)
	 * all transactions that can potentially may not see one or more tuples on
	 * the page. That's how index-only scans work fine in hot standby. A
	 * crucial difference between index-only scans and heap scans is that the
	 * index-only scan completely relies on the visibility map where as heap
	 * scan looks at the page-level PD_ALL_VISIBLE flag. We are not sure if
	 * the page-level flag can be trusted in the same way, because it might
	 * get propagated somehow without being explicitly WAL-logged, e.g. via a
	 * full page write. Until we can prove that beyond doubt, let's check each
	 * tuple for visibility the hard way.
	 */
	all_visible = PageIsAllVisible(page) && !snapshot->takenDuringRecovery;
	check_serializable =
		CheckForSerializableConflictOutNeeded(scan->rs_base.rs_rd, snapshot);

	/*
	 * We call page_collect_tuples() with constant arguments, to get the
	 * compiler to constant fold the constant arguments. Separate calls with
	 * constant arguments, rather than variables, are needed on several
	 * compilers to actually perform constant folding.
	 */
	if (likely(all_visible))
	{
		if (likely(!check_serializable))
			scan->rs_ntuples = page_collect_tuples(scan, snapshot, page, buffer,
												   block, lines, true, false);
		else
			scan->rs_ntuples = page_collect_tuples(scan, snapshot, page, buffer,
												   block, lines, true, true);
	}
	else
	{
		if (likely(!check_serializable))
			scan->rs_ntuples = page_collect_tuples(scan, snapshot, page, buffer,
												   block, lines, false, false);
		else
			scan->rs_ntuples = page_collect_tuples(scan, snapshot, page, buffer,
												   block, lines, false, true);
	}

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
}

/*
 * heap_fetch_next_buffer - read and pin the next block from MAIN_FORKNUM.
 *
 * Read the next block of the scan relation from the read stream and save it
 * in the scan descriptor.  It is already pinned.
 */
static inline void
heap_fetch_next_buffer(HeapScanDesc scan, ScanDirection dir)
{
	Assert(scan->rs_read_stream);

	/* release previous scan buffer, if any */
	if (BufferIsValid(scan->rs_cbuf))
	{
		ReleaseBuffer(scan->rs_cbuf);
		scan->rs_cbuf = InvalidBuffer;
	}

	/*
	 * Be sure to check for interrupts at least once per page.  Checks at
	 * higher code levels won't be able to stop a seqscan that encounters many
	 * pages' worth of consecutive dead tuples.
	 */
	CHECK_FOR_INTERRUPTS();

	/*
	 * If the scan direction is changing, reset the prefetch block to the
	 * current block. Otherwise, we will incorrectly prefetch the blocks
	 * between the prefetch block and the current block again before
	 * prefetching blocks in the new, correct scan direction.
	 */
	if (unlikely(scan->rs_dir != dir))
	{
		scan->rs_prefetch_block = scan->rs_cblock;
		read_stream_reset(scan->rs_read_stream);
	}

	scan->rs_dir = dir;

	scan->rs_cbuf = read_stream_next_buffer(scan->rs_read_stream, NULL);
	if (BufferIsValid(scan->rs_cbuf))
		scan->rs_cblock = BufferGetBlockNumber(scan->rs_cbuf);
}

/*
 * heapgettup_initial_block - return the first BlockNumber to scan
 *
 * Returns InvalidBlockNumber when there are no blocks to scan.  This can
 * occur with empty tables and in parallel scans when parallel workers get all
 * of the pages before we can get a chance to get our first page.
 */
static pg_noinline BlockNumber
heapgettup_initial_block(HeapScanDesc scan, ScanDirection dir)
{
	Assert(!scan->rs_inited);
	Assert(scan->rs_base.rs_parallel == NULL);

	/* When there are no pages to scan, return InvalidBlockNumber */
	if (scan->rs_nblocks == 0 || scan->rs_numblocks == 0)
		return InvalidBlockNumber;

	if (ScanDirectionIsForward(dir))
	{
		return scan->rs_startblock;
	}
	else
	{
		/*
		 * Disable reporting to syncscan logic in a backwards scan; it's not
		 * very likely anyone else is doing the same thing at the same time,
		 * and much more likely that we'll just bollix things for forward
		 * scanners.
		 */
		scan->rs_base.rs_flags &= ~SO_ALLOW_SYNC;

		/*
		 * Start from last page of the scan.  Ensure we take into account
		 * rs_numblocks if it's been adjusted by heap_setscanlimits().
		 */
		if (scan->rs_numblocks != InvalidBlockNumber)
			return (scan->rs_startblock + scan->rs_numblocks - 1) % scan->rs_nblocks;

		if (scan->rs_startblock > 0)
			return scan->rs_startblock - 1;

		return scan->rs_nblocks - 1;
	}
}


/*
 * heapgettup_start_page - helper function for heapgettup()
 *
 * Return the next page to scan based on the scan->rs_cbuf and set *linesleft
 * to the number of tuples on this page.  Also set *lineoff to the first
 * offset to scan with forward scans getting the first offset and backward
 * getting the final offset on the page.
 */
static Page
heapgettup_start_page(HeapScanDesc scan, ScanDirection dir, int *linesleft,
					  OffsetNumber *lineoff)
{
	Page		page;

	Assert(scan->rs_inited);
	Assert(BufferIsValid(scan->rs_cbuf));

	/* Caller is responsible for ensuring buffer is locked if needed */
	page = BufferGetPage(scan->rs_cbuf);

	*linesleft = PageGetMaxOffsetNumber(page) - FirstOffsetNumber + 1;

	if (ScanDirectionIsForward(dir))
		*lineoff = FirstOffsetNumber;
	else
		*lineoff = (OffsetNumber) (*linesleft);

	/* lineoff now references the physically previous or next tid */
	return page;
}


/*
 * heapgettup_continue_page - helper function for heapgettup()
 *
 * Return the next page to scan based on the scan->rs_cbuf and set *linesleft
 * to the number of tuples left to scan on this page.  Also set *lineoff to
 * the next offset to scan according to the ScanDirection in 'dir'.
 */
static inline Page
heapgettup_continue_page(HeapScanDesc scan, ScanDirection dir, int *linesleft,
						 OffsetNumber *lineoff)
{
	Page		page;

	Assert(scan->rs_inited);
	Assert(BufferIsValid(scan->rs_cbuf));

	/* Caller is responsible for ensuring buffer is locked if needed */
	page = BufferGetPage(scan->rs_cbuf);

	if (ScanDirectionIsForward(dir))
	{
		*lineoff = OffsetNumberNext(scan->rs_coffset);
		*linesleft = PageGetMaxOffsetNumber(page) - (*lineoff) + 1;
	}
	else
	{
		/*
		 * The previous returned tuple may have been vacuumed since the
		 * previous scan when we use a non-MVCC snapshot, so we must
		 * re-establish the lineoff <= PageGetMaxOffsetNumber(page) invariant
		 */
		*lineoff = Min(PageGetMaxOffsetNumber(page), OffsetNumberPrev(scan->rs_coffset));
		*linesleft = *lineoff;
	}

	/* lineoff now references the physically previous or next tid */
	return page;
}

/*
 * heapgettup_advance_block - helper for heap_fetch_next_buffer()
 *
 * Given the current block number, the scan direction, and various information
 * contained in the scan descriptor, calculate the BlockNumber to scan next
 * and return it.  If there are no further blocks to scan, return
 * InvalidBlockNumber to indicate this fact to the caller.
 *
 * This should not be called to determine the initial block number -- only for
 * subsequent blocks.
 *
 * This also adjusts rs_numblocks when a limit has been imposed by
 * heap_setscanlimits().
 */
static inline BlockNumber
heapgettup_advance_block(HeapScanDesc scan, BlockNumber block, ScanDirection dir)
{
	Assert(scan->rs_base.rs_parallel == NULL);

	if (likely(ScanDirectionIsForward(dir)))
	{
		block++;

		/* wrap back to the start of the heap */
		if (block >= scan->rs_nblocks)
			block = 0;

		/*
		 * Report our new scan position for synchronization purposes. We don't
		 * do that when moving backwards, however. That would just mess up any
		 * other forward-moving scanners.
		 *
		 * Note: we do this before checking for end of scan so that the final
		 * state of the position hint is back at the start of the rel.  That's
		 * not strictly necessary, but otherwise when you run the same query
		 * multiple times the starting position would shift a little bit
		 * backwards on every invocation, which is confusing. We don't
		 * guarantee any specific ordering in general, though.
		 */
		if (scan->rs_base.rs_flags & SO_ALLOW_SYNC)
			ss_report_location(scan->rs_base.rs_rd, block);

		/* we're done if we're back at where we started */
		if (block == scan->rs_startblock)
			return InvalidBlockNumber;

		/* check if the limit imposed by heap_setscanlimits() is met */
		if (scan->rs_numblocks != InvalidBlockNumber)
		{
			if (--scan->rs_numblocks == 0)
				return InvalidBlockNumber;
		}

		return block;
	}
	else
	{
		/* we're done if the last block is the start position */
		if (block == scan->rs_startblock)
			return InvalidBlockNumber;

		/* check if the limit imposed by heap_setscanlimits() is met */
		if (scan->rs_numblocks != InvalidBlockNumber)
		{
			if (--scan->rs_numblocks == 0)
				return InvalidBlockNumber;
		}

		/* wrap to the end of the heap when the last page was page 0 */
		if (block == 0)
			block = scan->rs_nblocks;

		block--;

		return block;
	}
}

/* ----------------
 *		heapgettup - fetch next heap tuple
 *
 *		Initialize the scan if not already done; then advance to the next
 *		tuple as indicated by "dir"; return the next tuple in scan->rs_ctup,
 *		or set scan->rs_ctup.t_data = NULL if no more tuples.
 *
 * Note: the reason nkeys/key are passed separately, even though they are
 * kept in the scan descriptor, is that the caller may not want us to check
 * the scankeys.
 *
 * Note: when we fall off the end of the scan in either direction, we
 * reset rs_inited.  This means that a further request with the same
 * scan direction will restart the scan, which is a bit odd, but a
 * request with the opposite scan direction will start a fresh scan
 * in the proper direction.  The latter is required behavior for cursors,
 * while the former case is generally undefined behavior in Postgres
 * so we don't care too much.
 * ----------------
 */
static void
heapgettup(HeapScanDesc scan,
		   ScanDirection dir,
		   int nkeys,
		   ScanKey key)
{
	HeapTuple	tuple = &(scan->rs_ctup);
	Page		page;
	OffsetNumber lineoff;
	int			linesleft;

	if (likely(scan->rs_inited))
	{
		/* continue from previously returned page/tuple */
		LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);
		page = heapgettup_continue_page(scan, dir, &linesleft, &lineoff);
		goto continue_page;
	}

	/*
	 * advance the scan until we find a qualifying tuple or run out of stuff
	 * to scan
	 */
	while (true)
	{
		heap_fetch_next_buffer(scan, dir);

		/* did we run out of blocks to scan? */
		if (!BufferIsValid(scan->rs_cbuf))
			break;

		Assert(BufferGetBlockNumber(scan->rs_cbuf) == scan->rs_cblock);

		LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);
		page = heapgettup_start_page(scan, dir, &linesleft, &lineoff);
continue_page:

		/*
		 * Only continue scanning the page while we have lines left.
		 *
		 * Note that this protects us from accessing line pointers past
		 * PageGetMaxOffsetNumber(); both for forward scans when we resume the
		 * table scan, and for when we start scanning a new page.
		 */
		for (; linesleft > 0; linesleft--, lineoff += dir)
		{
			bool		visible;
			ItemId		lpp = PageGetItemId(page, lineoff);

			if (!ItemIdIsNormal(lpp))
				continue;

			tuple->t_data = (HeapTupleHeader) PageGetItem(page, lpp);
			tuple->t_len = ItemIdGetLength(lpp);
			ItemPointerSet(&(tuple->t_self), scan->rs_cblock, lineoff);

			visible = HeapTupleSatisfiesVisibility(tuple,
												   scan->rs_base.rs_snapshot,
												   scan->rs_cbuf);

			HeapCheckForSerializableConflictOut(visible, scan->rs_base.rs_rd,
												tuple, scan->rs_cbuf,
												scan->rs_base.rs_snapshot);

			/* skip tuples not visible to this snapshot */
			if (!visible)
				continue;

			/* skip any tuples that don't match the scan key */
			if (key != NULL &&
				!HeapKeyTest(tuple, RelationGetDescr(scan->rs_base.rs_rd),
							 nkeys, key))
				continue;

			LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);
			scan->rs_coffset = lineoff;
			return;
		}

		/*
		 * if we get here, it means we've exhausted the items on this page and
		 * it's time to move to the next.
		 */
		LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);
	}

	/* end of scan */
	if (BufferIsValid(scan->rs_cbuf))
		ReleaseBuffer(scan->rs_cbuf);

	scan->rs_cbuf = InvalidBuffer;
	scan->rs_cblock = InvalidBlockNumber;
	scan->rs_prefetch_block = InvalidBlockNumber;
	tuple->t_data = NULL;
	scan->rs_inited = false;
}

/* ----------------
 *		heapgettup_pagemode - fetch next heap tuple in page-at-a-time mode
 *
 *		Same API as heapgettup, but used in page-at-a-time mode
 *
 * The internal logic is much the same as heapgettup's too, but there are some
 * differences: we do not take the buffer content lock (that only needs to
 * happen inside heap_prepare_pagescan), and we iterate through just the
 * tuples listed in rs_vistuples[] rather than all tuples on the page.  Notice
 * that lineindex is 0-based, where the corresponding loop variable lineoff in
 * heapgettup is 1-based.
 * ----------------
 */
static void
heapgettup_pagemode(HeapScanDesc scan,
					ScanDirection dir,
					int nkeys,
					ScanKey key)
{
	HeapTuple	tuple = &(scan->rs_ctup);
	Page		page;
	uint32		lineindex;
	uint32		linesleft;

	if (likely(scan->rs_inited))
	{
		/* continue from previously returned page/tuple */
		page = BufferGetPage(scan->rs_cbuf);

		lineindex = scan->rs_cindex + dir;
		if (ScanDirectionIsForward(dir))
			linesleft = scan->rs_ntuples - lineindex;
		else
			linesleft = scan->rs_cindex;
		/* lineindex now references the next or previous visible tid */

		goto continue_page;
	}

	/*
	 * advance the scan until we find a qualifying tuple or run out of stuff
	 * to scan
	 */
	while (true)
	{
		heap_fetch_next_buffer(scan, dir);

		/* did we run out of blocks to scan? */
		if (!BufferIsValid(scan->rs_cbuf))
			break;

		Assert(BufferGetBlockNumber(scan->rs_cbuf) == scan->rs_cblock);

		/* prune the page and determine visible tuple offsets */
		heap_prepare_pagescan((TableScanDesc) scan);
		page = BufferGetPage(scan->rs_cbuf);
		linesleft = scan->rs_ntuples;
		lineindex = ScanDirectionIsForward(dir) ? 0 : linesleft - 1;

		/* lineindex now references the next or previous visible tid */
continue_page:

		for (; linesleft > 0; linesleft--, lineindex += dir)
		{
			ItemId		lpp;
			OffsetNumber lineoff;

			Assert(lineindex <= scan->rs_ntuples);
			lineoff = scan->rs_vistuples[lineindex];
			lpp = PageGetItemId(page, lineoff);
			Assert(ItemIdIsNormal(lpp));

			tuple->t_data = (HeapTupleHeader) PageGetItem(page, lpp);
			tuple->t_len = ItemIdGetLength(lpp);
			ItemPointerSet(&(tuple->t_self), scan->rs_cblock, lineoff);

			/* skip any tuples that don't match the scan key */
			if (key != NULL &&
				!HeapKeyTest(tuple, RelationGetDescr(scan->rs_base.rs_rd),
							 nkeys, key))
				continue;

			scan->rs_cindex = lineindex;
			return;
		}
	}

	/* end of scan */
	if (BufferIsValid(scan->rs_cbuf))
		ReleaseBuffer(scan->rs_cbuf);
	scan->rs_cbuf = InvalidBuffer;
	scan->rs_cblock = InvalidBlockNumber;
	scan->rs_prefetch_block = InvalidBlockNumber;
	tuple->t_data = NULL;
	scan->rs_inited = false;
}


/* ----------------------------------------------------------------
 *					 heap access method interface
 * ----------------------------------------------------------------
 */


TableScanDesc
heap_beginscan(Relation relation, Snapshot snapshot,
			   int nkeys, ScanKey key,
			   ParallelTableScanDesc parallel_scan,
			   uint32 flags)
{
	HeapScanDesc scan;

	/*
	 * increment relation ref count while scanning relation
	 *
	 * This is just to make really sure the relcache entry won't go away while
	 * the scan has a pointer to it.  Caller should be holding the rel open
	 * anyway, so this is redundant in all normal scenarios...
	 */
	RelationIncrementReferenceCount(relation);

	/*
	 * allocate and initialize scan descriptor
	 */
	if (flags & SO_TYPE_BITMAPSCAN)
	{
		BitmapHeapScanDesc bscan = palloc(sizeof(BitmapHeapScanDescData));

		bscan->rs_vmbuffer = InvalidBuffer;
		bscan->rs_empty_tuples_pending = 0;
		scan = (HeapScanDesc) bscan;
	}
	else
		scan = (HeapScanDesc) palloc(sizeof(HeapScanDescData));

	scan->rs_base.rs_rd = relation;
	scan->rs_base.rs_snapshot = snapshot;
	scan->rs_base.rs_nkeys = nkeys;
	scan->rs_base.rs_flags = flags;
	scan->rs_base.rs_parallel = parallel_scan;
	scan->rs_strategy = NULL;	/* set in initscan */
	scan->rs_cbuf = InvalidBuffer;

	/*
	 * Disable page-at-a-time mode if it's not a MVCC-safe snapshot.
	 */
	if (!(snapshot && IsMVCCSnapshot(snapshot)))
		scan->rs_base.rs_flags &= ~SO_ALLOW_PAGEMODE;

	/*
	 * For seqscan and sample scans in a serializable transaction, acquire a
	 * predicate lock on the entire relation. This is required not only to
	 * lock all the matching tuples, but also to conflict with new insertions
	 * into the table. In an indexscan, we take page locks on the index pages
	 * covering the range specified in the scan qual, but in a heap scan there
	 * is nothing more fine-grained to lock. A bitmap scan is a different
	 * story, there we have already scanned the index and locked the index
	 * pages covering the predicate. But in that case we still have to lock
	 * any matching heap tuples. For sample scan we could optimize the locking
	 * to be at least page-level granularity, but we'd need to add per-tuple
	 * locking for that.
	 */
	if (scan->rs_base.rs_flags & (SO_TYPE_SEQSCAN | SO_TYPE_SAMPLESCAN))
	{
		/*
		 * Ensure a missing snapshot is noticed reliably, even if the
		 * isolation mode means predicate locking isn't performed (and
		 * therefore the snapshot isn't used here).
		 */
		Assert(snapshot);
		PredicateLockRelation(relation, snapshot);
	}

	/* we only need to set this up once */
	scan->rs_ctup.t_tableOid = RelationGetRelid(relation);

	/*
	 * Allocate memory to keep track of page allocation for parallel workers
	 * when doing a parallel scan.
	 */
	if (parallel_scan != NULL)
		scan->rs_parallelworkerdata = palloc(sizeof(ParallelBlockTableScanWorkerData));
	else
		scan->rs_parallelworkerdata = NULL;

	/*
	 * we do this here instead of in initscan() because heap_rescan also calls
	 * initscan() and we don't want to allocate memory again
	 */
	if (nkeys > 0)
		scan->rs_base.rs_key = (ScanKey) palloc(sizeof(ScanKeyData) * nkeys);
	else
		scan->rs_base.rs_key = NULL;

	initscan(scan, key, false);

	scan->rs_read_stream = NULL;

	/*
	 * Set up a read stream for sequential scans and TID range scans. This
	 * should be done after initscan() because initscan() allocates the
	 * BufferAccessStrategy object passed to the read stream API.
	 */
	if (scan->rs_base.rs_flags & SO_TYPE_SEQSCAN ||
		scan->rs_base.rs_flags & SO_TYPE_TIDRANGESCAN)
	{
		ReadStreamBlockNumberCB cb;

		if (scan->rs_base.rs_parallel)
			cb = heap_scan_stream_read_next_parallel;
		else
			cb = heap_scan_stream_read_next_serial;

		scan->rs_read_stream = read_stream_begin_relation(READ_STREAM_SEQUENTIAL,
														  scan->rs_strategy,
														  scan->rs_base.rs_rd,
														  MAIN_FORKNUM,
														  cb,
														  scan,
														  0);
	}
	else if (scan->rs_base.rs_flags & SO_TYPE_BITMAPSCAN)
	{
		scan->rs_read_stream = read_stream_begin_relation(READ_STREAM_DEFAULT,
														  scan->rs_strategy,
														  scan->rs_base.rs_rd,
														  MAIN_FORKNUM,
														  bitmapheap_stream_read_next,
														  scan,
														  sizeof(TBMIterateResult));
	}


	return (TableScanDesc) scan;
}

void
heap_rescan(TableScanDesc sscan, ScanKey key, bool set_params,
			bool allow_strat, bool allow_sync, bool allow_pagemode)
{
	HeapScanDesc scan = (HeapScanDesc) sscan;

	if (set_params)
	{
		if (allow_strat)
			scan->rs_base.rs_flags |= SO_ALLOW_STRAT;
		else
			scan->rs_base.rs_flags &= ~SO_ALLOW_STRAT;

		if (allow_sync)
			scan->rs_base.rs_flags |= SO_ALLOW_SYNC;
		else
			scan->rs_base.rs_flags &= ~SO_ALLOW_SYNC;

		if (allow_pagemode && scan->rs_base.rs_snapshot &&
			IsMVCCSnapshot(scan->rs_base.rs_snapshot))
			scan->rs_base.rs_flags |= SO_ALLOW_PAGEMODE;
		else
			scan->rs_base.rs_flags &= ~SO_ALLOW_PAGEMODE;
	}

	/*
	 * unpin scan buffers
	 */
	if (BufferIsValid(scan->rs_cbuf))
	{
		ReleaseBuffer(scan->rs_cbuf);
		scan->rs_cbuf = InvalidBuffer;
	}

	if (scan->rs_base.rs_flags & SO_TYPE_BITMAPSCAN)
	{
		BitmapHeapScanDesc bscan = (BitmapHeapScanDesc) scan;

		/*
		 * Reset empty_tuples_pending, a field only used by bitmap heap scan,
		 * to avoid incorrectly emitting NULL-filled tuples from a previous
		 * scan on rescan.
		 */
		bscan->rs_empty_tuples_pending = 0;

		if (BufferIsValid(bscan->rs_vmbuffer))
		{
			ReleaseBuffer(bscan->rs_vmbuffer);
			bscan->rs_vmbuffer = InvalidBuffer;
		}
	}

	/*
	 * The read stream is reset on rescan. This must be done before
	 * initscan(), as some state referred to by read_stream_reset() is reset
	 * in initscan().
	 */
	if (scan->rs_read_stream)
		read_stream_reset(scan->rs_read_stream);

	/*
	 * reinitialize scan descriptor
	 */
	initscan(scan, key, true);
}

void
heap_endscan(TableScanDesc sscan)
{
	HeapScanDesc scan = (HeapScanDesc) sscan;

	/* Note: no locking manipulations needed */

	/*
	 * unpin scan buffers
	 */
	if (BufferIsValid(scan->rs_cbuf))
		ReleaseBuffer(scan->rs_cbuf);

	if (scan->rs_base.rs_flags & SO_TYPE_BITMAPSCAN)
	{
		BitmapHeapScanDesc bscan = (BitmapHeapScanDesc) sscan;

		bscan->rs_empty_tuples_pending = 0;
		if (BufferIsValid(bscan->rs_vmbuffer))
			ReleaseBuffer(bscan->rs_vmbuffer);
	}

	/*
	 * Must free the read stream before freeing the BufferAccessStrategy.
	 */
	if (scan->rs_read_stream)
		read_stream_end(scan->rs_read_stream);

	/*
	 * decrement relation reference count and free scan descriptor storage
	 */
	RelationDecrementReferenceCount(scan->rs_base.rs_rd);

	if (scan->rs_base.rs_key)
		pfree(scan->rs_base.rs_key);

	if (scan->rs_strategy != NULL)
		FreeAccessStrategy(scan->rs_strategy);

	if (scan->rs_parallelworkerdata != NULL)
		pfree(scan->rs_parallelworkerdata);

	if (scan->rs_base.rs_flags & SO_TEMP_SNAPSHOT)
		UnregisterSnapshot(scan->rs_base.rs_snapshot);

	pfree(scan);
}

HeapTuple
heap_getnext(TableScanDesc sscan, ScanDirection direction)
{
	HeapScanDesc scan = (HeapScanDesc) sscan;

	/*
	 * This is still widely used directly, without going through table AM, so
	 * add a safety check.  It's possible we should, at a later point,
	 * downgrade this to an assert. The reason for checking the AM routine,
	 * rather than the AM oid, is that this allows to write regression tests
	 * that create another AM reusing the heap handler.
	 */
	if (unlikely(sscan->rs_rd->rd_tableam != GetHeapamTableAmRoutine()))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg_internal("only heap AM is supported")));

	/*
	 * We don't expect direct calls to heap_getnext with valid CheckXidAlive
	 * for catalog or regular tables.  See detailed comments in xact.c where
	 * these variables are declared.  Normally we have such a check at tableam
	 * level API but this is called from many places so we need to ensure it
	 * here.
	 */
	if (unlikely(TransactionIdIsValid(CheckXidAlive) && !bsysscan))
		elog(ERROR, "unexpected heap_getnext call during logical decoding");

	/* Note: no locking manipulations needed */

	if (scan->rs_base.rs_flags & SO_ALLOW_PAGEMODE)
		heapgettup_pagemode(scan, direction,
							scan->rs_base.rs_nkeys, scan->rs_base.rs_key);
	else
		heapgettup(scan, direction,
				   scan->rs_base.rs_nkeys, scan->rs_base.rs_key);

	if (scan->rs_ctup.t_data == NULL)
		return NULL;

	/*
	 * if we get here it means we have a new current scan tuple, so point to
	 * the proper return buffer and return the tuple.
	 */

	pgstat_count_heap_getnext(scan->rs_base.rs_rd);

	return &scan->rs_ctup;
}

bool
heap_getnextslot(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot)
{
	HeapScanDesc scan = (HeapScanDesc) sscan;

	/* Note: no locking manipulations needed */

	if (sscan->rs_flags & SO_ALLOW_PAGEMODE)
		heapgettup_pagemode(scan, direction, sscan->rs_nkeys, sscan->rs_key);
	else
		heapgettup(scan, direction, sscan->rs_nkeys, sscan->rs_key);

	if (scan->rs_ctup.t_data == NULL)
	{
		ExecClearTuple(slot);
		return false;
	}

	/*
	 * if we get here it means we have a new current scan tuple, so point to
	 * the proper return buffer and return the tuple.
	 */

	pgstat_count_heap_getnext(scan->rs_base.rs_rd);

	ExecStoreBufferHeapTuple(&scan->rs_ctup, slot,
							 scan->rs_cbuf);
	return true;
}

void
heap_set_tidrange(TableScanDesc sscan, ItemPointer mintid,
				  ItemPointer maxtid)
{
	HeapScanDesc scan = (HeapScanDesc) sscan;
	BlockNumber startBlk;
	BlockNumber numBlks;
	ItemPointerData highestItem;
	ItemPointerData lowestItem;

	/*
	 * For relations without any pages, we can simply leave the TID range
	 * unset.  There will be no tuples to scan, therefore no tuples outside
	 * the given TID range.
	 */
	if (scan->rs_nblocks == 0)
		return;

	/*
	 * Set up some ItemPointers which point to the first and last possible
	 * tuples in the heap.
	 */
	ItemPointerSet(&highestItem, scan->rs_nblocks - 1, MaxOffsetNumber);
	ItemPointerSet(&lowestItem, 0, FirstOffsetNumber);

	/*
	 * If the given maximum TID is below the highest possible TID in the
	 * relation, then restrict the range to that, otherwise we scan to the end
	 * of the relation.
	 */
	if (ItemPointerCompare(maxtid, &highestItem) < 0)
		ItemPointerCopy(maxtid, &highestItem);

	/*
	 * If the given minimum TID is above the lowest possible TID in the
	 * relation, then restrict the range to only scan for TIDs above that.
	 */
	if (ItemPointerCompare(mintid, &lowestItem) > 0)
		ItemPointerCopy(mintid, &lowestItem);

	/*
	 * Check for an empty range and protect from would be negative results
	 * from the numBlks calculation below.
	 */
	if (ItemPointerCompare(&highestItem, &lowestItem) < 0)
	{
		/* Set an empty range of blocks to scan */
		heap_setscanlimits(sscan, 0, 0);
		return;
	}

	/*
	 * Calculate the first block and the number of blocks we must scan. We
	 * could be more aggressive here and perform some more validation to try
	 * and further narrow the scope of blocks to scan by checking if the
	 * lowestItem has an offset above MaxOffsetNumber.  In this case, we could
	 * advance startBlk by one.  Likewise, if highestItem has an offset of 0
	 * we could scan one fewer blocks.  However, such an optimization does not
	 * seem worth troubling over, currently.
	 */
	startBlk = ItemPointerGetBlockNumberNoCheck(&lowestItem);

	numBlks = ItemPointerGetBlockNumberNoCheck(&highestItem) -
		ItemPointerGetBlockNumberNoCheck(&lowestItem) + 1;

	/* Set the start block and number of blocks to scan */
	heap_setscanlimits(sscan, startBlk, numBlks);

	/* Finally, set the TID range in sscan */
	ItemPointerCopy(&lowestItem, &sscan->st.tidrange.rs_mintid);
	ItemPointerCopy(&highestItem, &sscan->st.tidrange.rs_maxtid);
}

bool
heap_getnextslot_tidrange(TableScanDesc sscan, ScanDirection direction,
						  TupleTableSlot *slot)
{
	HeapScanDesc scan = (HeapScanDesc) sscan;
	ItemPointer mintid = &sscan->st.tidrange.rs_mintid;
	ItemPointer maxtid = &sscan->st.tidrange.rs_maxtid;

	/* Note: no locking manipulations needed */
	for (;;)
	{
		if (sscan->rs_flags & SO_ALLOW_PAGEMODE)
			heapgettup_pagemode(scan, direction, sscan->rs_nkeys, sscan->rs_key);
		else
			heapgettup(scan, direction, sscan->rs_nkeys, sscan->rs_key);

		if (scan->rs_ctup.t_data == NULL)
		{
			ExecClearTuple(slot);
			return false;
		}

		/*
		 * heap_set_tidrange will have used heap_setscanlimits to limit the
		 * range of pages we scan to only ones that can contain the TID range
		 * we're scanning for.  Here we must filter out any tuples from these
		 * pages that are outside of that range.
		 */
		if (ItemPointerCompare(&scan->rs_ctup.t_self, mintid) < 0)
		{
			ExecClearTuple(slot);

			/*
			 * When scanning backwards, the TIDs will be in descending order.
			 * Future tuples in this direction will be lower still, so we can
			 * just return false to indicate there will be no more tuples.
			 */
			if (ScanDirectionIsBackward(direction))
				return false;

			continue;
		}

		/*
		 * Likewise for the final page, we must filter out TIDs greater than
		 * maxtid.
		 */
		if (ItemPointerCompare(&scan->rs_ctup.t_self, maxtid) > 0)
		{
			ExecClearTuple(slot);

			/*
			 * When scanning forward, the TIDs will be in ascending order.
			 * Future tuples in this direction will be higher still, so we can
			 * just return false to indicate there will be no more tuples.
			 */
			if (ScanDirectionIsForward(direction))
				return false;
			continue;
		}

		break;
	}

	/*
	 * if we get here it means we have a new current scan tuple, so point to
	 * the proper return buffer and return the tuple.
	 */
	pgstat_count_heap_getnext(scan->rs_base.rs_rd);

	ExecStoreBufferHeapTuple(&scan->rs_ctup, slot, scan->rs_cbuf);
	return true;
}

/*
 *	heap_fetch		- retrieve tuple with given tid
 *
 * On entry, tuple->t_self is the TID to fetch.  We pin the buffer holding
 * the tuple, fill in the remaining fields of *tuple, and check the tuple
 * against the specified snapshot.
 *
 * If successful (tuple found and passes snapshot time qual), then *userbuf
 * is set to the buffer holding the tuple and true is returned.  The caller
 * must unpin the buffer when done with the tuple.
 *
 * If the tuple is not found (ie, item number references a deleted slot),
 * then tuple->t_data is set to NULL, *userbuf is set to InvalidBuffer,
 * and false is returned.
 *
 * If the tuple is found but fails the time qual check, then the behavior
 * depends on the keep_buf parameter.  If keep_buf is false, the results
 * are the same as for the tuple-not-found case.  If keep_buf is true,
 * then tuple->t_data and *userbuf are returned as for the success case,
 * and again the caller must unpin the buffer; but false is returned.
 *
 * heap_fetch does not follow HOT chains: only the exact TID requested will
 * be fetched.
 *
 * It is somewhat inconsistent that we ereport() on invalid block number but
 * return false on invalid item number.  There are a couple of reasons though.
 * One is that the caller can relatively easily check the block number for
 * validity, but cannot check the item number without reading the page
 * himself.  Another is that when we are following a t_ctid link, we can be
 * reasonably confident that the page number is valid (since VACUUM shouldn't
 * truncate off the destination page without having killed the referencing
 * tuple first), but the item number might well not be good.
 */
bool
heap_fetch(Relation relation,
		   Snapshot snapshot,
		   HeapTuple tuple,
		   Buffer *userbuf,
		   bool keep_buf)
{
	ItemPointer tid = &(tuple->t_self);
	ItemId		lp;
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	bool		valid;

	/*
	 * Fetch and pin the appropriate page of the relation.
	 */
	buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));

	/*
	 * Need share lock on buffer to examine tuple commit status.
	 */
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buffer);

	/*
	 * We'd better check for out-of-range offnum in case of VACUUM since the
	 * TID was obtained.
	 */
	offnum = ItemPointerGetOffsetNumber(tid);
	if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		*userbuf = InvalidBuffer;
		tuple->t_data = NULL;
		return false;
	}

	/*
	 * get the item line pointer corresponding to the requested tid
	 */
	lp = PageGetItemId(page, offnum);

	/*
	 * Must check for deleted tuple.
	 */
	if (!ItemIdIsNormal(lp))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		*userbuf = InvalidBuffer;
		tuple->t_data = NULL;
		return false;
	}

	/*
	 * fill in *tuple fields
	 */
	tuple->t_data = (HeapTupleHeader) PageGetItem(page, lp);
	tuple->t_len = ItemIdGetLength(lp);
	tuple->t_tableOid = RelationGetRelid(relation);

	/*
	 * check tuple visibility, then release lock
	 */
	valid = HeapTupleSatisfiesVisibility(tuple, snapshot, buffer);

	if (valid)
		PredicateLockTID(relation, &(tuple->t_self), snapshot,
						 HeapTupleHeaderGetXmin(tuple->t_data));

	HeapCheckForSerializableConflictOut(valid, relation, tuple, buffer, snapshot);

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	if (valid)
	{
		/*
		 * All checks passed, so return the tuple as valid. Caller is now
		 * responsible for releasing the buffer.
		 */
		*userbuf = buffer;

		return true;
	}

	/* Tuple failed time qual, but maybe caller wants to see it anyway. */
	if (keep_buf)
		*userbuf = buffer;
	else
	{
		ReleaseBuffer(buffer);
		*userbuf = InvalidBuffer;
		tuple->t_data = NULL;
	}

	return false;
}

/*
 *	heap_hot_search_buffer	- search HOT chain for tuple satisfying snapshot
 *
 * On entry, *tid is the TID of a tuple (either a simple tuple, or the root
 * of a HOT chain), and buffer is the buffer holding this tuple.  We search
 * for the first chain member satisfying the given snapshot.  If one is
 * found, we update *tid to reference that tuple's offset number, and
 * return true.  If no match, return false without modifying *tid.
 *
 * heapTuple is a caller-supplied buffer.  When a match is found, we return
 * the tuple here, in addition to updating *tid.  If no match is found, the
 * contents of this buffer on return are undefined.
 *
 * If all_dead is not NULL, we check non-visible tuples to see if they are
 * globally dead; *all_dead is set true if all members of the HOT chain
 * are vacuumable, false if not.
 *
 * Unlike heap_fetch, the caller must already have pin and (at least) share
 * lock on the buffer; it is still pinned/locked at exit.
 */
bool
heap_hot_search_buffer(ItemPointer tid, Relation relation, Buffer buffer,
					   Snapshot snapshot, HeapTuple heapTuple,
					   bool *all_dead, bool first_call)
{
	Page		page = BufferGetPage(buffer);
	TransactionId prev_xmax = InvalidTransactionId;
	BlockNumber blkno;
	OffsetNumber offnum;
	bool		at_chain_start;
	bool		valid;
	bool		skip;
	GlobalVisState *vistest = NULL;

	/* If this is not the first call, previous call returned a (live!) tuple */
	if (all_dead)
		*all_dead = first_call;

	blkno = ItemPointerGetBlockNumber(tid);
	offnum = ItemPointerGetOffsetNumber(tid);
	at_chain_start = first_call;
	skip = !first_call;

	/* XXX: we should assert that a snapshot is pushed or registered */
	Assert(TransactionIdIsValid(RecentXmin));
	Assert(BufferGetBlockNumber(buffer) == blkno);

	/* Scan through possible multiple members of HOT-chain */
	for (;;)
	{
		ItemId		lp;

		/* check for bogus TID */
		if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page))
			break;

		lp = PageGetItemId(page, offnum);

		/* check for unused, dead, or redirected items */
		if (!ItemIdIsNormal(lp))
		{
			/* We should only see a redirect at start of chain */
			if (ItemIdIsRedirected(lp) && at_chain_start)
			{
				/* Follow the redirect */
				offnum = ItemIdGetRedirect(lp);
				at_chain_start = false;
				continue;
			}
			/* else must be end of chain */
			break;
		}

		/*
		 * Update heapTuple to point to the element of the HOT chain we're
		 * currently investigating. Having t_self set correctly is important
		 * because the SSI checks and the *Satisfies routine for historical
		 * MVCC snapshots need the correct tid to decide about the visibility.
		 */
		heapTuple->t_data = (HeapTupleHeader) PageGetItem(page, lp);
		heapTuple->t_len = ItemIdGetLength(lp);
		heapTuple->t_tableOid = RelationGetRelid(relation);
		ItemPointerSet(&heapTuple->t_self, blkno, offnum);

		/*
		 * Shouldn't see a HEAP_ONLY tuple at chain start.
		 */
		if (at_chain_start && HeapTupleIsHeapOnly(heapTuple))
			break;

		/*
		 * The xmin should match the previous xmax value, else chain is
		 * broken.
		 */
		if (TransactionIdIsValid(prev_xmax) &&
			!TransactionIdEquals(prev_xmax,
								 HeapTupleHeaderGetXmin(heapTuple->t_data)))
			break;

		/*
		 * When first_call is true (and thus, skip is initially false) we'll
		 * return the first tuple we find.  But on later passes, heapTuple
		 * will initially be pointing to the tuple we returned last time.
		 * Returning it again would be incorrect (and would loop forever), so
		 * we skip it and return the next match we find.
		 */
		if (!skip)
		{
			/* If it's visible per the snapshot, we must return it */
			valid = HeapTupleSatisfiesVisibility(heapTuple, snapshot, buffer);
			HeapCheckForSerializableConflictOut(valid, relation, heapTuple,
												buffer, snapshot);

			if (valid)
			{
				ItemPointerSetOffsetNumber(tid, offnum);
				PredicateLockTID(relation, &heapTuple->t_self, snapshot,
								 HeapTupleHeaderGetXmin(heapTuple->t_data));
				if (all_dead)
					*all_dead = false;
				return true;
			}
		}
		skip = false;

		/*
		 * If we can't see it, maybe no one else can either.  At caller
		 * request, check whether all chain members are dead to all
		 * transactions.
		 *
		 * Note: if you change the criterion here for what is "dead", fix the
		 * planner's get_actual_variable_range() function to match.
		 */
		if (all_dead && *all_dead)
		{
			if (!vistest)
				vistest = GlobalVisTestFor(relation);

			if (!HeapTupleIsSurelyDead(heapTuple, vistest))
				*all_dead = false;
		}

		/*
		 * Check to see if HOT chain continues past this tuple; if so fetch
		 * the next offnum and loop around.
		 */
		if (HeapTupleIsHotUpdated(heapTuple))
		{
			Assert(ItemPointerGetBlockNumber(&heapTuple->t_data->t_ctid) ==
				   blkno);
			offnum = ItemPointerGetOffsetNumber(&heapTuple->t_data->t_ctid);
			at_chain_start = false;
			prev_xmax = HeapTupleHeaderGetUpdateXid(heapTuple->t_data);
		}
		else
			break;				/* end of chain */
	}

	return false;
}

/*
 *	heap_get_latest_tid -  get the latest tid of a specified tuple
 *
 * Actually, this gets the latest version that is visible according to the
 * scan's snapshot.  Create a scan using SnapshotDirty to get the very latest,
 * possibly uncommitted version.
 *
 * *tid is both an input and an output parameter: it is updated to
 * show the latest version of the row.  Note that it will not be changed
 * if no version of the row passes the snapshot test.
 */
void
heap_get_latest_tid(TableScanDesc sscan,
					ItemPointer tid)
{
	Relation	relation = sscan->rs_rd;
	Snapshot	snapshot = sscan->rs_snapshot;
	ItemPointerData ctid;
	TransactionId priorXmax;

	/*
	 * table_tuple_get_latest_tid() verified that the passed in tid is valid.
	 * Assume that t_ctid links are valid however - there shouldn't be invalid
	 * ones in the table.
	 */
	Assert(ItemPointerIsValid(tid));

	/*
	 * Loop to chase down t_ctid links.  At top of loop, ctid is the tuple we
	 * need to examine, and *tid is the TID we will return if ctid turns out
	 * to be bogus.
	 *
	 * Note that we will loop until we reach the end of the t_ctid chain.
	 * Depending on the snapshot passed, there might be at most one visible
	 * version of the row, but we don't try to optimize for that.
	 */
	ctid = *tid;
	priorXmax = InvalidTransactionId;	/* cannot check first XMIN */
	for (;;)
	{
		Buffer		buffer;
		Page		page;
		OffsetNumber offnum;
		ItemId		lp;
		HeapTupleData tp;
		bool		valid;

		/*
		 * Read, pin, and lock the page.
		 */
		buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(&ctid));
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);

		/*
		 * Check for bogus item number.  This is not treated as an error
		 * condition because it can happen while following a t_ctid link. We
		 * just assume that the prior tid is OK and return it unchanged.
		 */
		offnum = ItemPointerGetOffsetNumber(&ctid);
		if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page))
		{
			UnlockReleaseBuffer(buffer);
			break;
		}
		lp = PageGetItemId(page, offnum);
		if (!ItemIdIsNormal(lp))
		{
			UnlockReleaseBuffer(buffer);
			break;
		}

		/* OK to access the tuple */
		tp.t_self = ctid;
		tp.t_data = (HeapTupleHeader) PageGetItem(page, lp);
		tp.t_len = ItemIdGetLength(lp);
		tp.t_tableOid = RelationGetRelid(relation);

		/*
		 * After following a t_ctid link, we might arrive at an unrelated
		 * tuple.  Check for XMIN match.
		 */
		if (TransactionIdIsValid(priorXmax) &&
			!TransactionIdEquals(priorXmax, HeapTupleHeaderGetXmin(tp.t_data)))
		{
			UnlockReleaseBuffer(buffer);
			break;
		}

		/*
		 * Check tuple visibility; if visible, set it as the new result
		 * candidate.
		 */
		valid = HeapTupleSatisfiesVisibility(&tp, snapshot, buffer);
		HeapCheckForSerializableConflictOut(valid, relation, &tp, buffer, snapshot);
		if (valid)
			*tid = ctid;

		/*
		 * If there's a valid t_ctid link, follow it, else we're done.
		 */
		if ((tp.t_data->t_infomask & HEAP_XMAX_INVALID) ||
			HeapTupleHeaderIsOnlyLocked(tp.t_data) ||
			HeapTupleHeaderIndicatesMovedPartitions(tp.t_data) ||
			ItemPointerEquals(&tp.t_self, &tp.t_data->t_ctid))
		{
			UnlockReleaseBuffer(buffer);
			break;
		}

		ctid = tp.t_data->t_ctid;
		priorXmax = HeapTupleHeaderGetUpdateXid(tp.t_data);
		UnlockReleaseBuffer(buffer);
	}							/* end of loop */
}


/*
 * UpdateXmaxHintBits - update tuple hint bits after xmax transaction ends
 *
 * This is called after we have waited for the XMAX transaction to terminate.
 * If the transaction aborted, we guarantee the XMAX_INVALID hint bit will
 * be set on exit.  If the transaction committed, we set the XMAX_COMMITTED
 * hint bit if possible --- but beware that that may not yet be possible,
 * if the transaction committed asynchronously.
 *
 * Note that if the transaction was a locker only, we set HEAP_XMAX_INVALID
 * even if it commits.
 *
 * Hence callers should look only at XMAX_INVALID.
 *
 * Note this is not allowed for tuples whose xmax is a multixact.
 */
static void
UpdateXmaxHintBits(HeapTupleHeader tuple, Buffer buffer, TransactionId xid)
{
	Assert(TransactionIdEquals(HeapTupleHeaderGetRawXmax(tuple), xid));
	Assert(!(tuple->t_infomask & HEAP_XMAX_IS_MULTI));

	if (!(tuple->t_infomask & (HEAP_XMAX_COMMITTED | HEAP_XMAX_INVALID)))
	{
		if (!HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask) &&
			TransactionIdDidCommit(xid))
			HeapTupleSetHintBits(tuple, buffer, HEAP_XMAX_COMMITTED,
								 xid);
		else
			HeapTupleSetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
								 InvalidTransactionId);
	}
}


/*
 * GetBulkInsertState - prepare status object for a bulk insert
 */
BulkInsertState
GetBulkInsertState(void)
{
	BulkInsertState bistate;

	bistate = (BulkInsertState) palloc(sizeof(BulkInsertStateData));
	bistate->strategy = GetAccessStrategy(BAS_BULKWRITE);
	bistate->current_buf = InvalidBuffer;
	bistate->next_free = InvalidBlockNumber;
	bistate->last_free = InvalidBlockNumber;
	bistate->already_extended_by = 0;
	return bistate;
}

/*
 * FreeBulkInsertState - clean up after finishing a bulk insert
 */
void
FreeBulkInsertState(BulkInsertState bistate)
{
	if (bistate->current_buf != InvalidBuffer)
		ReleaseBuffer(bistate->current_buf);
	FreeAccessStrategy(bistate->strategy);
	pfree(bistate);
}

/*
 * ReleaseBulkInsertStatePin - release a buffer currently held in bistate
 */
void
ReleaseBulkInsertStatePin(BulkInsertState bistate)
{
	if (bistate->current_buf != InvalidBuffer)
		ReleaseBuffer(bistate->current_buf);
	bistate->current_buf = InvalidBuffer;

	/*
	 * Despite the name, we also reset bulk relation extension state.
	 * Otherwise we can end up erroring out due to looking for free space in
	 * ->next_free of one partition, even though ->next_free was set when
	 * extending another partition. It could obviously also be bad for
	 * efficiency to look at existing blocks at offsets from another
	 * partition, even if we don't error out.
	 */
	bistate->next_free = InvalidBlockNumber;
	bistate->last_free = InvalidBlockNumber;
}


/*
 *	heap_insert		- insert tuple into a heap
 *
 * The new tuple is stamped with current transaction ID and the specified
 * command ID.
 *
 * See table_tuple_insert for comments about most of the input flags, except
 * that this routine directly takes a tuple rather than a slot.
 *
 * There's corresponding HEAP_INSERT_ options to all the TABLE_INSERT_
 * options, and there additionally is HEAP_INSERT_SPECULATIVE which is used to
 * implement table_tuple_insert_speculative().
 *
 * On return the header fields of *tup are updated to match the stored tuple;
 * in particular tup->t_self receives the actual TID where the tuple was
 * stored.  But note that any toasting of fields within the tuple data is NOT
 * reflected into *tup.
 */
void
heap_insert(Relation relation, HeapTuple tup, CommandId cid,
			int options, BulkInsertState bistate)
{
	TransactionId xid = GetCurrentTransactionId();
	HeapTuple	heaptup;
	Buffer		buffer;
	Buffer		vmbuffer = InvalidBuffer;
	bool		all_visible_cleared = false;

	/* Cheap, simplistic check that the tuple matches the rel's rowtype. */
	Assert(HeapTupleHeaderGetNatts(tup->t_data) <=
		   RelationGetNumberOfAttributes(relation));

	/*
	 * Fill in tuple header fields and toast the tuple if necessary.
	 *
	 * Note: below this point, heaptup is the data we actually intend to store
	 * into the relation; tup is the caller's original untoasted data.
	 */
	heaptup = heap_prepare_insert(relation, tup, xid, cid, options);

	/*
	 * Find buffer to insert this tuple into.  If the page is all visible,
	 * this will also pin the requisite visibility map page.
	 */
	buffer = RelationGetBufferForTuple(relation, heaptup->t_len,
									   InvalidBuffer, options, bistate,
									   &vmbuffer, NULL,
									   0);

	/*
	 * We're about to do the actual insert -- but check for conflict first, to
	 * avoid possibly having to roll back work we've just done.
	 *
	 * This is safe without a recheck as long as there is no possibility of
	 * another process scanning the page between this check and the insert
	 * being visible to the scan (i.e., an exclusive buffer content lock is
	 * continuously held from this point until the tuple insert is visible).
	 *
	 * For a heap insert, we only need to check for table-level SSI locks. Our
	 * new tuple can't possibly conflict with existing tuple locks, and heap
	 * page locks are only consolidated versions of tuple locks; they do not
	 * lock "gaps" as index page locks do.  So we don't need to specify a
	 * buffer when making the call, which makes for a faster check.
	 */
	CheckForSerializableConflictIn(relation, NULL, InvalidBlockNumber);

	/* NO EREPORT(ERROR) from here till changes are logged */
	START_CRIT_SECTION();

	RelationPutHeapTuple(relation, buffer, heaptup,
						 (options & HEAP_INSERT_SPECULATIVE) != 0);

	if (PageIsAllVisible(BufferGetPage(buffer)))
	{
		all_visible_cleared = true;
		PageClearAllVisible(BufferGetPage(buffer));
		visibilitymap_clear(relation,
							ItemPointerGetBlockNumber(&(heaptup->t_self)),
							vmbuffer, VISIBILITYMAP_VALID_BITS);
	}

	/*
	 * XXX Should we set PageSetPrunable on this page ?
	 *
	 * The inserting transaction may eventually abort thus making this tuple
	 * DEAD and hence available for pruning. Though we don't want to optimize
	 * for aborts, if no other tuple in this page is UPDATEd/DELETEd, the
	 * aborted tuple will never be pruned until next vacuum is triggered.
	 *
	 * If you do add PageSetPrunable here, add it in heap_xlog_insert too.
	 */

	MarkBufferDirty(buffer);

	/* XLOG stuff */
	if (RelationNeedsWAL(relation))
	{
		xl_heap_insert xlrec;
		xl_heap_header xlhdr;
		XLogRecPtr	recptr;
		Page		page = BufferGetPage(buffer);
		uint8		info = XLOG_HEAP_INSERT;
		int			bufflags = 0;

		/*
		 * If this is a catalog, we need to transmit combo CIDs to properly
		 * decode, so log that as well.
		 */
		if (RelationIsAccessibleInLogicalDecoding(relation))
			log_heap_new_cid(relation, heaptup);

		/*
		 * If this is the single and first tuple on page, we can reinit the
		 * page instead of restoring the whole thing.  Set flag, and hide
		 * buffer references from XLogInsert.
		 */
		if (ItemPointerGetOffsetNumber(&(heaptup->t_self)) == FirstOffsetNumber &&
			PageGetMaxOffsetNumber(page) == FirstOffsetNumber)
		{
			info |= XLOG_HEAP_INIT_PAGE;
			bufflags |= REGBUF_WILL_INIT;
		}

		xlrec.offnum = ItemPointerGetOffsetNumber(&heaptup->t_self);
		xlrec.flags = 0;
		if (all_visible_cleared)
			xlrec.flags |= XLH_INSERT_ALL_VISIBLE_CLEARED;
		if (options & HEAP_INSERT_SPECULATIVE)
			xlrec.flags |= XLH_INSERT_IS_SPECULATIVE;
		Assert(ItemPointerGetBlockNumber(&heaptup->t_self) == BufferGetBlockNumber(buffer));

		/*
		 * For logical decoding, we need the tuple even if we're doing a full
		 * page write, so make sure it's included even if we take a full-page
		 * image. (XXX We could alternatively store a pointer into the FPW).
		 */
		if (RelationIsLogicallyLogged(relation) &&
			!(options & HEAP_INSERT_NO_LOGICAL))
		{
			xlrec.flags |= XLH_INSERT_CONTAINS_NEW_TUPLE;
			bufflags |= REGBUF_KEEP_DATA;

			if (IsToastRelation(relation))
				xlrec.flags |= XLH_INSERT_ON_TOAST_RELATION;
		}

		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfHeapInsert);

		xlhdr.t_infomask2 = heaptup->t_data->t_infomask2;
		xlhdr.t_infomask = heaptup->t_data->t_infomask;
		xlhdr.t_hoff = heaptup->t_data->t_hoff;

		/*
		 * note we mark xlhdr as belonging to buffer; if XLogInsert decides to
		 * write the whole page to the xlog, we don't need to store
		 * xl_heap_header in the xlog.
		 */
		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD | bufflags);
		XLogRegisterBufData(0, &xlhdr, SizeOfHeapHeader);
		/* PG73FORMAT: write bitmap [+ padding] [+ oid] + data */
		XLogRegisterBufData(0,
							(char *) heaptup->t_data + SizeofHeapTupleHeader,
							heaptup->t_len - SizeofHeapTupleHeader);

		/* filtering by origin on a row level is much more efficient */
		XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);

		recptr = XLogInsert(RM_HEAP_ID, info);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	UnlockReleaseBuffer(buffer);
	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);

	/*
	 * If tuple is cachable, mark it for invalidation from the caches in case
	 * we abort.  Note it is OK to do this after releasing the buffer, because
	 * the heaptup data structure is all in local memory, not in the shared
	 * buffer.
	 */
	CacheInvalidateHeapTuple(relation, heaptup, NULL);

	/* Note: speculative insertions are counted too, even if aborted later */
	pgstat_count_heap_insert(relation, 1);

	/*
	 * If heaptup is a private copy, release it.  Don't forget to copy t_self
	 * back to the caller's image, too.
	 */
	if (heaptup != tup)
	{
		tup->t_self = heaptup->t_self;
		heap_freetuple(heaptup);
	}
}

/*
 * Subroutine for heap_insert(). Prepares a tuple for insertion. This sets the
 * tuple header fields and toasts the tuple if necessary.  Returns a toasted
 * version of the tuple if it was toasted, or the original tuple if not. Note
 * that in any case, the header fields are also set in the original tuple.
 */
static HeapTuple
heap_prepare_insert(Relation relation, HeapTuple tup, TransactionId xid,
					CommandId cid, int options)
{
	/*
	 * To allow parallel inserts, we need to ensure that they are safe to be
	 * performed in workers. We have the infrastructure to allow parallel
	 * inserts in general except for the cases where inserts generate a new
	 * CommandId (eg. inserts into a table having a foreign key column).
	 */
	if (IsParallelWorker())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot insert tuples in a parallel worker")));

	tup->t_data->t_infomask &= ~(HEAP_XACT_MASK);
	tup->t_data->t_infomask2 &= ~(HEAP2_XACT_MASK);
	tup->t_data->t_infomask |= HEAP_XMAX_INVALID;
	HeapTupleHeaderSetXmin(tup->t_data, xid);
	if (options & HEAP_INSERT_FROZEN)
		HeapTupleHeaderSetXminFrozen(tup->t_data);

	HeapTupleHeaderSetCmin(tup->t_data, cid);
	HeapTupleHeaderSetXmax(tup->t_data, 0); /* for cleanliness */
	tup->t_tableOid = RelationGetRelid(relation);

	/*
	 * If the new tuple is too big for storage or contains already toasted
	 * out-of-line attributes from some other relation, invoke the toaster.
	 */
	if (relation->rd_rel->relkind != RELKIND_RELATION &&
		relation->rd_rel->relkind != RELKIND_MATVIEW)
	{
		/* toast table entries should never be recursively toasted */
		Assert(!HeapTupleHasExternal(tup));
		return tup;
	}
	else if (HeapTupleHasExternal(tup) || tup->t_len > TOAST_TUPLE_THRESHOLD)
		return heap_toast_insert_or_update(relation, tup, NULL, options);
	else
		return tup;
}

/*
 * Helper for heap_multi_insert() that computes the number of entire pages
 * that inserting the remaining heaptuples requires. Used to determine how
 * much the relation needs to be extended by.
 */
static int
heap_multi_insert_pages(HeapTuple *heaptuples, int done, int ntuples, Size saveFreeSpace)
{
	size_t		page_avail = BLCKSZ - SizeOfPageHeaderData - saveFreeSpace;
	int			npages = 1;

	for (int i = done; i < ntuples; i++)
	{
		size_t		tup_sz = sizeof(ItemIdData) + MAXALIGN(heaptuples[i]->t_len);

		if (page_avail < tup_sz)
		{
			npages++;
			page_avail = BLCKSZ - SizeOfPageHeaderData - saveFreeSpace;
		}
		page_avail -= tup_sz;
	}

	return npages;
}

/*
 *	heap_multi_insert	- insert multiple tuples into a heap
 *
 * This is like heap_insert(), but inserts multiple tuples in one operation.
 * That's faster than calling heap_insert() in a loop, because when multiple
 * tuples can be inserted on a single page, we can write just a single WAL
 * record covering all of them, and only need to lock/unlock the page once.
 *
 * Note: this leaks memory into the current memory context. You can create a
 * temporary context before calling this, if that's a problem.
 */
void
heap_multi_insert(Relation relation, TupleTableSlot **slots, int ntuples,
				  CommandId cid, int options, BulkInsertState bistate)
{
	TransactionId xid = GetCurrentTransactionId();
	HeapTuple  *heaptuples;
	int			i;
	int			ndone;
	PGAlignedBlock scratch;
	Page		page;
	Buffer		vmbuffer = InvalidBuffer;
	bool		needwal;
	Size		saveFreeSpace;
	bool		need_tuple_data = RelationIsLogicallyLogged(relation);
	bool		need_cids = RelationIsAccessibleInLogicalDecoding(relation);
	bool		starting_with_empty_page = false;
	int			npages = 0;
	int			npages_used = 0;

	/* currently not needed (thus unsupported) for heap_multi_insert() */
	Assert(!(options & HEAP_INSERT_NO_LOGICAL));

	needwal = RelationNeedsWAL(relation);
	saveFreeSpace = RelationGetTargetPageFreeSpace(relation,
												   HEAP_DEFAULT_FILLFACTOR);

	/* Toast and set header data in all the slots */
	heaptuples = palloc(ntuples * sizeof(HeapTuple));
	for (i = 0; i < ntuples; i++)
	{
		HeapTuple	tuple;

		tuple = ExecFetchSlotHeapTuple(slots[i], true, NULL);
		slots[i]->tts_tableOid = RelationGetRelid(relation);
		tuple->t_tableOid = slots[i]->tts_tableOid;
		heaptuples[i] = heap_prepare_insert(relation, tuple, xid, cid,
											options);
	}

	/*
	 * We're about to do the actual inserts -- but check for conflict first,
	 * to minimize the possibility of having to roll back work we've just
	 * done.
	 *
	 * A check here does not definitively prevent a serialization anomaly;
	 * that check MUST be done at least past the point of acquiring an
	 * exclusive buffer content lock on every buffer that will be affected,
	 * and MAY be done after all inserts are reflected in the buffers and
	 * those locks are released; otherwise there is a race condition.  Since
	 * multiple buffers can be locked and unlocked in the loop below, and it
	 * would not be feasible to identify and lock all of those buffers before
	 * the loop, we must do a final check at the end.
	 *
	 * The check here could be omitted with no loss of correctness; it is
	 * present strictly as an optimization.
	 *
	 * For heap inserts, we only need to check for table-level SSI locks. Our
	 * new tuples can't possibly conflict with existing tuple locks, and heap
	 * page locks are only consolidated versions of tuple locks; they do not
	 * lock "gaps" as index page locks do.  So we don't need to specify a
	 * buffer when making the call, which makes for a faster check.
	 */
	CheckForSerializableConflictIn(relation, NULL, InvalidBlockNumber);

	ndone = 0;
	while (ndone < ntuples)
	{
		Buffer		buffer;
		bool		all_visible_cleared = false;
		bool		all_frozen_set = false;
		int			nthispage;

		CHECK_FOR_INTERRUPTS();

		/*
		 * Compute number of pages needed to fit the to-be-inserted tuples in
		 * the worst case.  This will be used to determine how much to extend
		 * the relation by in RelationGetBufferForTuple(), if needed.  If we
		 * filled a prior page from scratch, we can just update our last
		 * computation, but if we started with a partially filled page,
		 * recompute from scratch, the number of potentially required pages
		 * can vary due to tuples needing to fit onto the page, page headers
		 * etc.
		 */
		if (ndone == 0 || !starting_with_empty_page)
		{
			npages = heap_multi_insert_pages(heaptuples, ndone, ntuples,
											 saveFreeSpace);
			npages_used = 0;
		}
		else
			npages_used++;

		/*
		 * Find buffer where at least the next tuple will fit.  If the page is
		 * all-visible, this will also pin the requisite visibility map page.
		 *
		 * Also pin visibility map page if COPY FREEZE inserts tuples into an
		 * empty page. See all_frozen_set below.
		 */
		buffer = RelationGetBufferForTuple(relation, heaptuples[ndone]->t_len,
										   InvalidBuffer, options, bistate,
										   &vmbuffer, NULL,
										   npages - npages_used);
		page = BufferGetPage(buffer);

		starting_with_empty_page = PageGetMaxOffsetNumber(page) == 0;

		if (starting_with_empty_page && (options & HEAP_INSERT_FROZEN))
			all_frozen_set = true;

		/* NO EREPORT(ERROR) from here till changes are logged */
		START_CRIT_SECTION();

		/*
		 * RelationGetBufferForTuple has ensured that the first tuple fits.
		 * Put that on the page, and then as many other tuples as fit.
		 */
		RelationPutHeapTuple(relation, buffer, heaptuples[ndone], false);

		/*
		 * For logical decoding we need combo CIDs to properly decode the
		 * catalog.
		 */
		if (needwal && need_cids)
			log_heap_new_cid(relation, heaptuples[ndone]);

		for (nthispage = 1; ndone + nthispage < ntuples; nthispage++)
		{
			HeapTuple	heaptup = heaptuples[ndone + nthispage];

			if (PageGetHeapFreeSpace(page) < MAXALIGN(heaptup->t_len) + saveFreeSpace)
				break;

			RelationPutHeapTuple(relation, buffer, heaptup, false);

			/*
			 * For logical decoding we need combo CIDs to properly decode the
			 * catalog.
			 */
			if (needwal && need_cids)
				log_heap_new_cid(relation, heaptup);
		}

		/*
		 * If the page is all visible, need to clear that, unless we're only
		 * going to add further frozen rows to it.
		 *
		 * If we're only adding already frozen rows to a previously empty
		 * page, mark it as all-visible.
		 */
		if (PageIsAllVisible(page) && !(options & HEAP_INSERT_FROZEN))
		{
			all_visible_cleared = true;
			PageClearAllVisible(page);
			visibilitymap_clear(relation,
								BufferGetBlockNumber(buffer),
								vmbuffer, VISIBILITYMAP_VALID_BITS);
		}
		else if (all_frozen_set)
			PageSetAllVisible(page);

		/*
		 * XXX Should we set PageSetPrunable on this page ? See heap_insert()
		 */

		MarkBufferDirty(buffer);

		/* XLOG stuff */
		if (needwal)
		{
			XLogRecPtr	recptr;
			xl_heap_multi_insert *xlrec;
			uint8		info = XLOG_HEAP2_MULTI_INSERT;
			char	   *tupledata;
			int			totaldatalen;
			char	   *scratchptr = scratch.data;
			bool		init;
			int			bufflags = 0;

			/*
			 * If the page was previously empty, we can reinit the page
			 * instead of restoring the whole thing.
			 */
			init = starting_with_empty_page;

			/* allocate xl_heap_multi_insert struct from the scratch area */
			xlrec = (xl_heap_multi_insert *) scratchptr;
			scratchptr += SizeOfHeapMultiInsert;

			/*
			 * Allocate offsets array. Unless we're reinitializing the page,
			 * in that case the tuples are stored in order starting at
			 * FirstOffsetNumber and we don't need to store the offsets
			 * explicitly.
			 */
			if (!init)
				scratchptr += nthispage * sizeof(OffsetNumber);

			/* the rest of the scratch space is used for tuple data */
			tupledata = scratchptr;

			/* check that the mutually exclusive flags are not both set */
			Assert(!(all_visible_cleared && all_frozen_set));

			xlrec->flags = 0;
			if (all_visible_cleared)
				xlrec->flags = XLH_INSERT_ALL_VISIBLE_CLEARED;
			if (all_frozen_set)
				xlrec->flags = XLH_INSERT_ALL_FROZEN_SET;

			xlrec->ntuples = nthispage;

			/*
			 * Write out an xl_multi_insert_tuple and the tuple data itself
			 * for each tuple.
			 */
			for (i = 0; i < nthispage; i++)
			{
				HeapTuple	heaptup = heaptuples[ndone + i];
				xl_multi_insert_tuple *tuphdr;
				int			datalen;

				if (!init)
					xlrec->offsets[i] = ItemPointerGetOffsetNumber(&heaptup->t_self);
				/* xl_multi_insert_tuple needs two-byte alignment. */
				tuphdr = (xl_multi_insert_tuple *) SHORTALIGN(scratchptr);
				scratchptr = ((char *) tuphdr) + SizeOfMultiInsertTuple;

				tuphdr->t_infomask2 = heaptup->t_data->t_infomask2;
				tuphdr->t_infomask = heaptup->t_data->t_infomask;
				tuphdr->t_hoff = heaptup->t_data->t_hoff;

				/* write bitmap [+ padding] [+ oid] + data */
				datalen = heaptup->t_len - SizeofHeapTupleHeader;
				memcpy(scratchptr,
					   (char *) heaptup->t_data + SizeofHeapTupleHeader,
					   datalen);
				tuphdr->datalen = datalen;
				scratchptr += datalen;
			}
			totaldatalen = scratchptr - tupledata;
			Assert((scratchptr - scratch.data) < BLCKSZ);

			if (need_tuple_data)
				xlrec->flags |= XLH_INSERT_CONTAINS_NEW_TUPLE;

			/*
			 * Signal that this is the last xl_heap_multi_insert record
			 * emitted by this call to heap_multi_insert(). Needed for logical
			 * decoding so it knows when to cleanup temporary data.
			 */
			if (ndone + nthispage == ntuples)
				xlrec->flags |= XLH_INSERT_LAST_IN_MULTI;

			if (init)
			{
				info |= XLOG_HEAP_INIT_PAGE;
				bufflags |= REGBUF_WILL_INIT;
			}

			/*
			 * If we're doing logical decoding, include the new tuple data
			 * even if we take a full-page image of the page.
			 */
			if (need_tuple_data)
				bufflags |= REGBUF_KEEP_DATA;

			XLogBeginInsert();
			XLogRegisterData(xlrec, tupledata - scratch.data);
			XLogRegisterBuffer(0, buffer, REGBUF_STANDARD | bufflags);

			XLogRegisterBufData(0, tupledata, totaldatalen);

			/* filtering by origin on a row level is much more efficient */
			XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);

			recptr = XLogInsert(RM_HEAP2_ID, info);

			PageSetLSN(page, recptr);
		}

		END_CRIT_SECTION();

		/*
		 * If we've frozen everything on the page, update the visibilitymap.
		 * We're already holding pin on the vmbuffer.
		 */
		if (all_frozen_set)
		{
			Assert(PageIsAllVisible(page));
			Assert(visibilitymap_pin_ok(BufferGetBlockNumber(buffer), vmbuffer));

			/*
			 * It's fine to use InvalidTransactionId here - this is only used
			 * when HEAP_INSERT_FROZEN is specified, which intentionally
			 * violates visibility rules.
			 */
			visibilitymap_set(relation, BufferGetBlockNumber(buffer), buffer,
							  InvalidXLogRecPtr, vmbuffer,
							  InvalidTransactionId,
							  VISIBILITYMAP_ALL_VISIBLE | VISIBILITYMAP_ALL_FROZEN);
		}

		UnlockReleaseBuffer(buffer);
		ndone += nthispage;

		/*
		 * NB: Only release vmbuffer after inserting all tuples - it's fairly
		 * likely that we'll insert into subsequent heap pages that are likely
		 * to use the same vm page.
		 */
	}

	/* We're done with inserting all tuples, so release the last vmbuffer. */
	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);

	/*
	 * We're done with the actual inserts.  Check for conflicts again, to
	 * ensure that all rw-conflicts in to these inserts are detected.  Without
	 * this final check, a sequential scan of the heap may have locked the
	 * table after the "before" check, missing one opportunity to detect the
	 * conflict, and then scanned the table before the new tuples were there,
	 * missing the other chance to detect the conflict.
	 *
	 * For heap inserts, we only need to check for table-level SSI locks. Our
	 * new tuples can't possibly conflict with existing tuple locks, and heap
	 * page locks are only consolidated versions of tuple locks; they do not
	 * lock "gaps" as index page locks do.  So we don't need to specify a
	 * buffer when making the call.
	 */
	CheckForSerializableConflictIn(relation, NULL, InvalidBlockNumber);

	/*
	 * If tuples are cachable, mark them for invalidation from the caches in
	 * case we abort.  Note it is OK to do this after releasing the buffer,
	 * because the heaptuples data structure is all in local memory, not in
	 * the shared buffer.
	 */
	if (IsCatalogRelation(relation))
	{
		for (i = 0; i < ntuples; i++)
			CacheInvalidateHeapTuple(relation, heaptuples[i], NULL);
	}

	/* copy t_self fields back to the caller's slots */
	for (i = 0; i < ntuples; i++)
		slots[i]->tts_tid = heaptuples[i]->t_self;

	pgstat_count_heap_insert(relation, ntuples);
}

/*
 *	simple_heap_insert - insert a tuple
 *
 * Currently, this routine differs from heap_insert only in supplying
 * a default command ID and not allowing access to the speedup options.
 *
 * This should be used rather than using heap_insert directly in most places
 * where we are modifying system catalogs.
 */
void
simple_heap_insert(Relation relation, HeapTuple tup)
{
	heap_insert(relation, tup, GetCurrentCommandId(true), 0, NULL);
}

/*
 * Given infomask/infomask2, compute the bits that must be saved in the
 * "infobits" field of xl_heap_delete, xl_heap_update, xl_heap_lock,
 * xl_heap_lock_updated WAL records.
 *
 * See fix_infomask_from_infobits.
 */
static uint8
compute_infobits(uint16 infomask, uint16 infomask2)
{
	return
		((infomask & HEAP_XMAX_IS_MULTI) != 0 ? XLHL_XMAX_IS_MULTI : 0) |
		((infomask & HEAP_XMAX_LOCK_ONLY) != 0 ? XLHL_XMAX_LOCK_ONLY : 0) |
		((infomask & HEAP_XMAX_EXCL_LOCK) != 0 ? XLHL_XMAX_EXCL_LOCK : 0) |
	/* note we ignore HEAP_XMAX_SHR_LOCK here */
		((infomask & HEAP_XMAX_KEYSHR_LOCK) != 0 ? XLHL_XMAX_KEYSHR_LOCK : 0) |
		((infomask2 & HEAP_KEYS_UPDATED) != 0 ?
		 XLHL_KEYS_UPDATED : 0);
}

/*
 * Given two versions of the same t_infomask for a tuple, compare them and
 * return whether the relevant status for a tuple Xmax has changed.  This is
 * used after a buffer lock has been released and reacquired: we want to ensure
 * that the tuple state continues to be the same it was when we previously
 * examined it.
 *
 * Note the Xmax field itself must be compared separately.
 */
static inline bool
xmax_infomask_changed(uint16 new_infomask, uint16 old_infomask)
{
	const uint16 interesting =
		HEAP_XMAX_IS_MULTI | HEAP_XMAX_LOCK_ONLY | HEAP_LOCK_MASK;

	if ((new_infomask & interesting) != (old_infomask & interesting))
		return true;

	return false;
}

/*
 *	heap_delete - delete a tuple
 *
 * See table_tuple_delete() for an explanation of the parameters, except that
 * this routine directly takes a tuple rather than a slot.
 *
 * In the failure cases, the routine fills *tmfd with the tuple's t_ctid,
 * t_xmax (resolving a possible MultiXact, if necessary), and t_cmax (the last
 * only for TM_SelfModified, since we cannot obtain cmax from a combo CID
 * generated by another transaction).
 */
TM_Result
heap_delete(Relation relation, ItemPointer tid,
			CommandId cid, Snapshot crosscheck, bool wait,
			TM_FailureData *tmfd, bool changingPart)
{
	TM_Result	result;
	TransactionId xid = GetCurrentTransactionId();
	ItemId		lp;
	HeapTupleData tp;
	Page		page;
	BlockNumber block;
	Buffer		buffer;
	Buffer		vmbuffer = InvalidBuffer;
	TransactionId new_xmax;
	uint16		new_infomask,
				new_infomask2;
	bool		have_tuple_lock = false;
	bool		iscombo;
	bool		all_visible_cleared = false;
	HeapTuple	old_key_tuple = NULL;	/* replica identity of the tuple */
	bool		old_key_copied = false;

	Assert(ItemPointerIsValid(tid));

	/*
	 * Forbid this during a parallel operation, lest it allocate a combo CID.
	 * Other workers might need that combo CID for visibility checks, and we
	 * have no provision for broadcasting it to them.
	 */
	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot delete tuples during a parallel operation")));

	block = ItemPointerGetBlockNumber(tid);
	buffer = ReadBuffer(relation, block);
	page = BufferGetPage(buffer);

	/*
	 * Before locking the buffer, pin the visibility map page if it appears to
	 * be necessary.  Since we haven't got the lock yet, someone else might be
	 * in the middle of changing this, so we'll need to recheck after we have
	 * the lock.
	 */
	if (PageIsAllVisible(page))
		visibilitymap_pin(relation, block, &vmbuffer);

	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));
	Assert(ItemIdIsNormal(lp));

	tp.t_tableOid = RelationGetRelid(relation);
	tp.t_data = (HeapTupleHeader) PageGetItem(page, lp);
	tp.t_len = ItemIdGetLength(lp);
	tp.t_self = *tid;

l1:

	/*
	 * If we didn't pin the visibility map page and the page has become all
	 * visible while we were busy locking the buffer, we'll have to unlock and
	 * re-lock, to avoid holding the buffer lock across an I/O.  That's a bit
	 * unfortunate, but hopefully shouldn't happen often.
	 */
	if (vmbuffer == InvalidBuffer && PageIsAllVisible(page))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		visibilitymap_pin(relation, block, &vmbuffer);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	}

	result = HeapTupleSatisfiesUpdate(&tp, cid, buffer);

	if (result == TM_Invisible)
	{
		UnlockReleaseBuffer(buffer);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("attempted to delete invisible tuple")));
	}
	else if (result == TM_BeingModified && wait)
	{
		TransactionId xwait;
		uint16		infomask;

		/* must copy state data before unlocking buffer */
		xwait = HeapTupleHeaderGetRawXmax(tp.t_data);
		infomask = tp.t_data->t_infomask;

		/*
		 * Sleep until concurrent transaction ends -- except when there's a
		 * single locker and it's our own transaction.  Note we don't care
		 * which lock mode the locker has, because we need the strongest one.
		 *
		 * Before sleeping, we need to acquire tuple lock to establish our
		 * priority for the tuple (see heap_lock_tuple).  LockTuple will
		 * release us when we are next-in-line for the tuple.
		 *
		 * If we are forced to "start over" below, we keep the tuple lock;
		 * this arranges that we stay at the head of the line while rechecking
		 * tuple state.
		 */
		if (infomask & HEAP_XMAX_IS_MULTI)
		{
			bool		current_is_member = false;

			if (DoesMultiXactIdConflict((MultiXactId) xwait, infomask,
										LockTupleExclusive, &current_is_member))
			{
				LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

				/*
				 * Acquire the lock, if necessary (but skip it when we're
				 * requesting a lock and already have one; avoids deadlock).
				 */
				if (!current_is_member)
					heap_acquire_tuplock(relation, &(tp.t_self), LockTupleExclusive,
										 LockWaitBlock, &have_tuple_lock);

				/* wait for multixact */
				MultiXactIdWait((MultiXactId) xwait, MultiXactStatusUpdate, infomask,
								relation, &(tp.t_self), XLTW_Delete,
								NULL);
				LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

				/*
				 * If xwait had just locked the tuple then some other xact
				 * could update this tuple before we get to this point.  Check
				 * for xmax change, and start over if so.
				 *
				 * We also must start over if we didn't pin the VM page, and
				 * the page has become all visible.
				 */
				if ((vmbuffer == InvalidBuffer && PageIsAllVisible(page)) ||
					xmax_infomask_changed(tp.t_data->t_infomask, infomask) ||
					!TransactionIdEquals(HeapTupleHeaderGetRawXmax(tp.t_data),
										 xwait))
					goto l1;
			}

			/*
			 * You might think the multixact is necessarily done here, but not
			 * so: it could have surviving members, namely our own xact or
			 * other subxacts of this backend.  It is legal for us to delete
			 * the tuple in either case, however (the latter case is
			 * essentially a situation of upgrading our former shared lock to
			 * exclusive).  We don't bother changing the on-disk hint bits
			 * since we are about to overwrite the xmax altogether.
			 */
		}
		else if (!TransactionIdIsCurrentTransactionId(xwait))
		{
			/*
			 * Wait for regular transaction to end; but first, acquire tuple
			 * lock.
			 */
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			heap_acquire_tuplock(relation, &(tp.t_self), LockTupleExclusive,
								 LockWaitBlock, &have_tuple_lock);
			XactLockTableWait(xwait, relation, &(tp.t_self), XLTW_Delete);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

			/*
			 * xwait is done, but if xwait had just locked the tuple then some
			 * other xact could update this tuple before we get to this point.
			 * Check for xmax change, and start over if so.
			 *
			 * We also must start over if we didn't pin the VM page, and the
			 * page has become all visible.
			 */
			if ((vmbuffer == InvalidBuffer && PageIsAllVisible(page)) ||
				xmax_infomask_changed(tp.t_data->t_infomask, infomask) ||
				!TransactionIdEquals(HeapTupleHeaderGetRawXmax(tp.t_data),
									 xwait))
				goto l1;

			/* Otherwise check if it committed or aborted */
			UpdateXmaxHintBits(tp.t_data, buffer, xwait);
		}

		/*
		 * We may overwrite if previous xmax aborted, or if it committed but
		 * only locked the tuple without updating it.
		 */
		if ((tp.t_data->t_infomask & HEAP_XMAX_INVALID) ||
			HEAP_XMAX_IS_LOCKED_ONLY(tp.t_data->t_infomask) ||
			HeapTupleHeaderIsOnlyLocked(tp.t_data))
			result = TM_Ok;
		else if (!ItemPointerEquals(&tp.t_self, &tp.t_data->t_ctid))
			result = TM_Updated;
		else
			result = TM_Deleted;
	}

	/* sanity check the result HeapTupleSatisfiesUpdate() and the logic above */
	if (result != TM_Ok)
	{
		Assert(result == TM_SelfModified ||
			   result == TM_Updated ||
			   result == TM_Deleted ||
			   result == TM_BeingModified);
		Assert(!(tp.t_data->t_infomask & HEAP_XMAX_INVALID));
		Assert(result != TM_Updated ||
			   !ItemPointerEquals(&tp.t_self, &tp.t_data->t_ctid));
	}

	if (crosscheck != InvalidSnapshot && result == TM_Ok)
	{
		/* Perform additional check for transaction-snapshot mode RI updates */
		if (!HeapTupleSatisfiesVisibility(&tp, crosscheck, buffer))
			result = TM_Updated;
	}

	if (result != TM_Ok)
	{
		tmfd->ctid = tp.t_data->t_ctid;
		tmfd->xmax = HeapTupleHeaderGetUpdateXid(tp.t_data);
		if (result == TM_SelfModified)
			tmfd->cmax = HeapTupleHeaderGetCmax(tp.t_data);
		else
			tmfd->cmax = InvalidCommandId;
		UnlockReleaseBuffer(buffer);
		if (have_tuple_lock)
			UnlockTupleTuplock(relation, &(tp.t_self), LockTupleExclusive);
		if (vmbuffer != InvalidBuffer)
			ReleaseBuffer(vmbuffer);
		return result;
	}

	/*
	 * We're about to do the actual delete -- check for conflict first, to
	 * avoid possibly having to roll back work we've just done.
	 *
	 * This is safe without a recheck as long as there is no possibility of
	 * another process scanning the page between this check and the delete
	 * being visible to the scan (i.e., an exclusive buffer content lock is
	 * continuously held from this point until the tuple delete is visible).
	 */
	CheckForSerializableConflictIn(relation, tid, BufferGetBlockNumber(buffer));

	/* replace cid with a combo CID if necessary */
	HeapTupleHeaderAdjustCmax(tp.t_data, &cid, &iscombo);

	/*
	 * Compute replica identity tuple before entering the critical section so
	 * we don't PANIC upon a memory allocation failure.
	 */
	old_key_tuple = ExtractReplicaIdentity(relation, &tp, true, &old_key_copied);

	/*
	 * If this is the first possibly-multixact-able operation in the current
	 * transaction, set my per-backend OldestMemberMXactId setting. We can be
	 * certain that the transaction will never become a member of any older
	 * MultiXactIds than that.  (We have to do this even if we end up just
	 * using our own TransactionId below, since some other backend could
	 * incorporate our XID into a MultiXact immediately afterwards.)
	 */
	MultiXactIdSetOldestMember();

	compute_new_xmax_infomask(HeapTupleHeaderGetRawXmax(tp.t_data),
							  tp.t_data->t_infomask, tp.t_data->t_infomask2,
							  xid, LockTupleExclusive, true,
							  &new_xmax, &new_infomask, &new_infomask2);

	START_CRIT_SECTION();

	/*
	 * If this transaction commits, the tuple will become DEAD sooner or
	 * later.  Set flag that this page is a candidate for pruning once our xid
	 * falls below the OldestXmin horizon.  If the transaction finally aborts,
	 * the subsequent page pruning will be a no-op and the hint will be
	 * cleared.
	 */
	PageSetPrunable(page, xid);

	if (PageIsAllVisible(page))
	{
		all_visible_cleared = true;
		PageClearAllVisible(page);
		visibilitymap_clear(relation, BufferGetBlockNumber(buffer),
							vmbuffer, VISIBILITYMAP_VALID_BITS);
	}

	/* store transaction information of xact deleting the tuple */
	tp.t_data->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
	tp.t_data->t_infomask2 &= ~HEAP_KEYS_UPDATED;
	tp.t_data->t_infomask |= new_infomask;
	tp.t_data->t_infomask2 |= new_infomask2;
	HeapTupleHeaderClearHotUpdated(tp.t_data);
	HeapTupleHeaderSetXmax(tp.t_data, new_xmax);
	HeapTupleHeaderSetCmax(tp.t_data, cid, iscombo);
	/* Make sure there is no forward chain link in t_ctid */
	tp.t_data->t_ctid = tp.t_self;

	/* Signal that this is actually a move into another partition */
	if (changingPart)
		HeapTupleHeaderSetMovedPartitions(tp.t_data);

	MarkBufferDirty(buffer);

	/*
	 * XLOG stuff
	 *
	 * NB: heap_abort_speculative() uses the same xlog record and replay
	 * routines.
	 */
	if (RelationNeedsWAL(relation))
	{
		xl_heap_delete xlrec;
		xl_heap_header xlhdr;
		XLogRecPtr	recptr;

		/*
		 * For logical decode we need combo CIDs to properly decode the
		 * catalog
		 */
		if (RelationIsAccessibleInLogicalDecoding(relation))
			log_heap_new_cid(relation, &tp);

		xlrec.flags = 0;
		if (all_visible_cleared)
			xlrec.flags |= XLH_DELETE_ALL_VISIBLE_CLEARED;
		if (changingPart)
			xlrec.flags |= XLH_DELETE_IS_PARTITION_MOVE;
		xlrec.infobits_set = compute_infobits(tp.t_data->t_infomask,
											  tp.t_data->t_infomask2);
		xlrec.offnum = ItemPointerGetOffsetNumber(&tp.t_self);
		xlrec.xmax = new_xmax;

		if (old_key_tuple != NULL)
		{
			if (relation->rd_rel->relreplident == REPLICA_IDENTITY_FULL)
				xlrec.flags |= XLH_DELETE_CONTAINS_OLD_TUPLE;
			else
				xlrec.flags |= XLH_DELETE_CONTAINS_OLD_KEY;
		}

		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfHeapDelete);

		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

		/*
		 * Log replica identity of the deleted tuple if there is one
		 */
		if (old_key_tuple != NULL)
		{
			xlhdr.t_infomask2 = old_key_tuple->t_data->t_infomask2;
			xlhdr.t_infomask = old_key_tuple->t_data->t_infomask;
			xlhdr.t_hoff = old_key_tuple->t_data->t_hoff;

			XLogRegisterData(&xlhdr, SizeOfHeapHeader);
			XLogRegisterData((char *) old_key_tuple->t_data
							 + SizeofHeapTupleHeader,
							 old_key_tuple->t_len
							 - SizeofHeapTupleHeader);
		}

		/* filtering by origin on a row level is much more efficient */
		XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);

		recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_DELETE);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);

	/*
	 * If the tuple has toasted out-of-line attributes, we need to delete
	 * those items too.  We have to do this before releasing the buffer
	 * because we need to look at the contents of the tuple, but it's OK to
	 * release the content lock on the buffer first.
	 */
	if (relation->rd_rel->relkind != RELKIND_RELATION &&
		relation->rd_rel->relkind != RELKIND_MATVIEW)
	{
		/* toast table entries should never be recursively toasted */
		Assert(!HeapTupleHasExternal(&tp));
	}
	else if (HeapTupleHasExternal(&tp))
		heap_toast_delete(relation, &tp, false);

	/*
	 * Mark tuple for invalidation from system caches at next command
	 * boundary. We have to do this before releasing the buffer because we
	 * need to look at the contents of the tuple.
	 */
	CacheInvalidateHeapTuple(relation, &tp, NULL);

	/* Now we can release the buffer */
	ReleaseBuffer(buffer);

	/*
	 * Release the lmgr tuple lock, if we had it.
	 */
	if (have_tuple_lock)
		UnlockTupleTuplock(relation, &(tp.t_self), LockTupleExclusive);

	pgstat_count_heap_delete(relation);

	if (old_key_tuple != NULL && old_key_copied)
		heap_freetuple(old_key_tuple);

	return TM_Ok;
}

/*
 *	simple_heap_delete - delete a tuple
 *
 * This routine may be used to delete a tuple when concurrent updates of
 * the target tuple are not expected (for example, because we have a lock
 * on the relation associated with the tuple).  Any failure is reported
 * via ereport().
 */
void
simple_heap_delete(Relation relation, ItemPointer tid)
{
	TM_Result	result;
	TM_FailureData tmfd;

	result = heap_delete(relation, tid,
						 GetCurrentCommandId(true), InvalidSnapshot,
						 true /* wait for commit */ ,
						 &tmfd, false /* changingPart */ );
	switch (result)
	{
		case TM_SelfModified:
			/* Tuple was already updated in current command? */
			elog(ERROR, "tuple already updated by self");
			break;

		case TM_Ok:
			/* done successfully */
			break;

		case TM_Updated:
			elog(ERROR, "tuple concurrently updated");
			break;

		case TM_Deleted:
			elog(ERROR, "tuple concurrently deleted");
			break;

		default:
			elog(ERROR, "unrecognized heap_delete status: %u", result);
			break;
	}
}

/*
 *	heap_update - replace a tuple
 *
 * See table_tuple_update() for an explanation of the parameters, except that
 * this routine directly takes a tuple rather than a slot.
 *
 * In the failure cases, the routine fills *tmfd with the tuple's t_ctid,
 * t_xmax (resolving a possible MultiXact, if necessary), and t_cmax (the last
 * only for TM_SelfModified, since we cannot obtain cmax from a combo CID
 * generated by another transaction).
 */
TM_Result
heap_update(Relation relation, ItemPointer otid, HeapTuple newtup,
			CommandId cid, Snapshot crosscheck, bool wait,
			TM_FailureData *tmfd, LockTupleMode *lockmode,
			TU_UpdateIndexes *update_indexes)
{
	TM_Result	result;
	TransactionId xid = GetCurrentTransactionId();
	Bitmapset  *hot_attrs;
	Bitmapset  *sum_attrs;
	Bitmapset  *key_attrs;
	Bitmapset  *id_attrs;
	Bitmapset  *interesting_attrs;
	Bitmapset  *modified_attrs;
	ItemId		lp;
	HeapTupleData oldtup;
	HeapTuple	heaptup;
	HeapTuple	old_key_tuple = NULL;
	bool		old_key_copied = false;
	Page		page;
	BlockNumber block;
	MultiXactStatus mxact_status;
	Buffer		buffer,
				newbuf,
				vmbuffer = InvalidBuffer,
				vmbuffer_new = InvalidBuffer;
	bool		need_toast;
	Size		newtupsize,
				pagefree;
	bool		have_tuple_lock = false;
	bool		iscombo;
	bool		use_hot_update = false;
	bool		summarized_update = false;
	bool		key_intact;
	bool		all_visible_cleared = false;
	bool		all_visible_cleared_new = false;
	bool		checked_lockers;
	bool		locker_remains;
	bool		id_has_external = false;
	TransactionId xmax_new_tuple,
				xmax_old_tuple;
	uint16		infomask_old_tuple,
				infomask2_old_tuple,
				infomask_new_tuple,
				infomask2_new_tuple;

	Assert(ItemPointerIsValid(otid));

	/* Cheap, simplistic check that the tuple matches the rel's rowtype. */
	Assert(HeapTupleHeaderGetNatts(newtup->t_data) <=
		   RelationGetNumberOfAttributes(relation));

	/*
	 * Forbid this during a parallel operation, lest it allocate a combo CID.
	 * Other workers might need that combo CID for visibility checks, and we
	 * have no provision for broadcasting it to them.
	 */
	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot update tuples during a parallel operation")));

#ifdef USE_ASSERT_CHECKING
	check_lock_if_inplace_updateable_rel(relation, otid, newtup);
#endif

	/*
	 * Fetch the list of attributes to be checked for various operations.
	 *
	 * For HOT considerations, this is wasted effort if we fail to update or
	 * have to put the new tuple on a different page.  But we must compute the
	 * list before obtaining buffer lock --- in the worst case, if we are
	 * doing an update on one of the relevant system catalogs, we could
	 * deadlock if we try to fetch the list later.  In any case, the relcache
	 * caches the data so this is usually pretty cheap.
	 *
	 * We also need columns used by the replica identity and columns that are
	 * considered the "key" of rows in the table.
	 *
	 * Note that we get copies of each bitmap, so we need not worry about
	 * relcache flush happening midway through.
	 */
	hot_attrs = RelationGetIndexAttrBitmap(relation,
										   INDEX_ATTR_BITMAP_HOT_BLOCKING);
	sum_attrs = RelationGetIndexAttrBitmap(relation,
										   INDEX_ATTR_BITMAP_SUMMARIZED);
	key_attrs = RelationGetIndexAttrBitmap(relation, INDEX_ATTR_BITMAP_KEY);
	id_attrs = RelationGetIndexAttrBitmap(relation,
										  INDEX_ATTR_BITMAP_IDENTITY_KEY);
	interesting_attrs = NULL;
	interesting_attrs = bms_add_members(interesting_attrs, hot_attrs);
	interesting_attrs = bms_add_members(interesting_attrs, sum_attrs);
	interesting_attrs = bms_add_members(interesting_attrs, key_attrs);
	interesting_attrs = bms_add_members(interesting_attrs, id_attrs);

	block = ItemPointerGetBlockNumber(otid);
	INJECTION_POINT("heap_update-before-pin");
	buffer = ReadBuffer(relation, block);
	page = BufferGetPage(buffer);

	/*
	 * Before locking the buffer, pin the visibility map page if it appears to
	 * be necessary.  Since we haven't got the lock yet, someone else might be
	 * in the middle of changing this, so we'll need to recheck after we have
	 * the lock.
	 */
	if (PageIsAllVisible(page))
		visibilitymap_pin(relation, block, &vmbuffer);

	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	lp = PageGetItemId(page, ItemPointerGetOffsetNumber(otid));

	/*
	 * Usually, a buffer pin and/or snapshot blocks pruning of otid, ensuring
	 * we see LP_NORMAL here.  When the otid origin is a syscache, we may have
	 * neither a pin nor a snapshot.  Hence, we may see other LP_ states, each
	 * of which indicates concurrent pruning.
	 *
	 * Failing with TM_Updated would be most accurate.  However, unlike other
	 * TM_Updated scenarios, we don't know the successor ctid in LP_UNUSED and
	 * LP_DEAD cases.  While the distinction between TM_Updated and TM_Deleted
	 * does matter to SQL statements UPDATE and MERGE, those SQL statements
	 * hold a snapshot that ensures LP_NORMAL.  Hence, the choice between
	 * TM_Updated and TM_Deleted affects only the wording of error messages.
	 * Settle on TM_Deleted, for two reasons.  First, it avoids complicating
	 * the specification of when tmfd->ctid is valid.  Second, it creates
	 * error log evidence that we took this branch.
	 *
	 * Since it's possible to see LP_UNUSED at otid, it's also possible to see
	 * LP_NORMAL for a tuple that replaced LP_UNUSED.  If it's a tuple for an
	 * unrelated row, we'll fail with "duplicate key value violates unique".
	 * XXX if otid is the live, newer version of the newtup row, we'll discard
	 * changes originating in versions of this catalog row after the version
	 * the caller got from syscache.  See syscache-update-pruned.spec.
	 */
	if (!ItemIdIsNormal(lp))
	{
		Assert(RelationSupportsSysCache(RelationGetRelid(relation)));

		UnlockReleaseBuffer(buffer);
		Assert(!have_tuple_lock);
		if (vmbuffer != InvalidBuffer)
			ReleaseBuffer(vmbuffer);
		tmfd->ctid = *otid;
		tmfd->xmax = InvalidTransactionId;
		tmfd->cmax = InvalidCommandId;
		*update_indexes = TU_None;

		bms_free(hot_attrs);
		bms_free(sum_attrs);
		bms_free(key_attrs);
		bms_free(id_attrs);
		/* modified_attrs not yet initialized */
		bms_free(interesting_attrs);
		return TM_Deleted;
	}

	/*
	 * Fill in enough data in oldtup for HeapDetermineColumnsInfo to work
	 * properly.
	 */
	oldtup.t_tableOid = RelationGetRelid(relation);
	oldtup.t_data = (HeapTupleHeader) PageGetItem(page, lp);
	oldtup.t_len = ItemIdGetLength(lp);
	oldtup.t_self = *otid;

	/* the new tuple is ready, except for this: */
	newtup->t_tableOid = RelationGetRelid(relation);

	/*
	 * Determine columns modified by the update.  Additionally, identify
	 * whether any of the unmodified replica identity key attributes in the
	 * old tuple is externally stored or not.  This is required because for
	 * such attributes the flattened value won't be WAL logged as part of the
	 * new tuple so we must include it as part of the old_key_tuple.  See
	 * ExtractReplicaIdentity.
	 */
	modified_attrs = HeapDetermineColumnsInfo(relation, interesting_attrs,
											  id_attrs, &oldtup,
											  newtup, &id_has_external);

	/*
	 * If we're not updating any "key" column, we can grab a weaker lock type.
	 * This allows for more concurrency when we are running simultaneously
	 * with foreign key checks.
	 *
	 * Note that if a column gets detoasted while executing the update, but
	 * the value ends up being the same, this test will fail and we will use
	 * the stronger lock.  This is acceptable; the important case to optimize
	 * is updates that don't manipulate key columns, not those that
	 * serendipitously arrive at the same key values.
	 */
	if (!bms_overlap(modified_attrs, key_attrs))
	{
		*lockmode = LockTupleNoKeyExclusive;
		mxact_status = MultiXactStatusNoKeyUpdate;
		key_intact = true;

		/*
		 * If this is the first possibly-multixact-able operation in the
		 * current transaction, set my per-backend OldestMemberMXactId
		 * setting. We can be certain that the transaction will never become a
		 * member of any older MultiXactIds than that.  (We have to do this
		 * even if we end up just using our own TransactionId below, since
		 * some other backend could incorporate our XID into a MultiXact
		 * immediately afterwards.)
		 */
		MultiXactIdSetOldestMember();
	}
	else
	{
		*lockmode = LockTupleExclusive;
		mxact_status = MultiXactStatusUpdate;
		key_intact = false;
	}

	/*
	 * Note: beyond this point, use oldtup not otid to refer to old tuple.
	 * otid may very well point at newtup->t_self, which we will overwrite
	 * with the new tuple's location, so there's great risk of confusion if we
	 * use otid anymore.
	 */

l2:
	checked_lockers = false;
	locker_remains = false;
	result = HeapTupleSatisfiesUpdate(&oldtup, cid, buffer);

	/* see below about the "no wait" case */
	Assert(result != TM_BeingModified || wait);

	if (result == TM_Invisible)
	{
		UnlockReleaseBuffer(buffer);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("attempted to update invisible tuple")));
	}
	else if (result == TM_BeingModified && wait)
	{
		TransactionId xwait;
		uint16		infomask;
		bool		can_continue = false;

		/*
		 * XXX note that we don't consider the "no wait" case here.  This
		 * isn't a problem currently because no caller uses that case, but it
		 * should be fixed if such a caller is introduced.  It wasn't a
		 * problem previously because this code would always wait, but now
		 * that some tuple locks do not conflict with one of the lock modes we
		 * use, it is possible that this case is interesting to handle
		 * specially.
		 *
		 * This may cause failures with third-party code that calls
		 * heap_update directly.
		 */

		/* must copy state data before unlocking buffer */
		xwait = HeapTupleHeaderGetRawXmax(oldtup.t_data);
		infomask = oldtup.t_data->t_infomask;

		/*
		 * Now we have to do something about the existing locker.  If it's a
		 * multi, sleep on it; we might be awakened before it is completely
		 * gone (or even not sleep at all in some cases); we need to preserve
		 * it as locker, unless it is gone completely.
		 *
		 * If it's not a multi, we need to check for sleeping conditions
		 * before actually going to sleep.  If the update doesn't conflict
		 * with the locks, we just continue without sleeping (but making sure
		 * it is preserved).
		 *
		 * Before sleeping, we need to acquire tuple lock to establish our
		 * priority for the tuple (see heap_lock_tuple).  LockTuple will
		 * release us when we are next-in-line for the tuple.  Note we must
		 * not acquire the tuple lock until we're sure we're going to sleep;
		 * otherwise we're open for race conditions with other transactions
		 * holding the tuple lock which sleep on us.
		 *
		 * If we are forced to "start over" below, we keep the tuple lock;
		 * this arranges that we stay at the head of the line while rechecking
		 * tuple state.
		 */
		if (infomask & HEAP_XMAX_IS_MULTI)
		{
			TransactionId update_xact;
			int			remain;
			bool		current_is_member = false;

			if (DoesMultiXactIdConflict((MultiXactId) xwait, infomask,
										*lockmode, &current_is_member))
			{
				LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

				/*
				 * Acquire the lock, if necessary (but skip it when we're
				 * requesting a lock and already have one; avoids deadlock).
				 */
				if (!current_is_member)
					heap_acquire_tuplock(relation, &(oldtup.t_self), *lockmode,
										 LockWaitBlock, &have_tuple_lock);

				/* wait for multixact */
				MultiXactIdWait((MultiXactId) xwait, mxact_status, infomask,
								relation, &oldtup.t_self, XLTW_Update,
								&remain);
				checked_lockers = true;
				locker_remains = remain != 0;
				LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

				/*
				 * If xwait had just locked the tuple then some other xact
				 * could update this tuple before we get to this point.  Check
				 * for xmax change, and start over if so.
				 */
				if (xmax_infomask_changed(oldtup.t_data->t_infomask,
										  infomask) ||
					!TransactionIdEquals(HeapTupleHeaderGetRawXmax(oldtup.t_data),
										 xwait))
					goto l2;
			}

			/*
			 * Note that the multixact may not be done by now.  It could have
			 * surviving members; our own xact or other subxacts of this
			 * backend, and also any other concurrent transaction that locked
			 * the tuple with LockTupleKeyShare if we only got
			 * LockTupleNoKeyExclusive.  If this is the case, we have to be
			 * careful to mark the updated tuple with the surviving members in
			 * Xmax.
			 *
			 * Note that there could have been another update in the
			 * MultiXact. In that case, we need to check whether it committed
			 * or aborted. If it aborted we are safe to update it again;
			 * otherwise there is an update conflict, and we have to return
			 * TableTuple{Deleted, Updated} below.
			 *
			 * In the LockTupleExclusive case, we still need to preserve the
			 * surviving members: those would include the tuple locks we had
			 * before this one, which are important to keep in case this
			 * subxact aborts.
			 */
			if (!HEAP_XMAX_IS_LOCKED_ONLY(oldtup.t_data->t_infomask))
				update_xact = HeapTupleGetUpdateXid(oldtup.t_data);
			else
				update_xact = InvalidTransactionId;

			/*
			 * There was no UPDATE in the MultiXact; or it aborted. No
			 * TransactionIdIsInProgress() call needed here, since we called
			 * MultiXactIdWait() above.
			 */
			if (!TransactionIdIsValid(update_xact) ||
				TransactionIdDidAbort(update_xact))
				can_continue = true;
		}
		else if (TransactionIdIsCurrentTransactionId(xwait))
		{
			/*
			 * The only locker is ourselves; we can avoid grabbing the tuple
			 * lock here, but must preserve our locking information.
			 */
			checked_lockers = true;
			locker_remains = true;
			can_continue = true;
		}
		else if (HEAP_XMAX_IS_KEYSHR_LOCKED(infomask) && key_intact)
		{
			/*
			 * If it's just a key-share locker, and we're not changing the key
			 * columns, we don't need to wait for it to end; but we need to
			 * preserve it as locker.
			 */
			checked_lockers = true;
			locker_remains = true;
			can_continue = true;
		}
		else
		{
			/*
			 * Wait for regular transaction to end; but first, acquire tuple
			 * lock.
			 */
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			heap_acquire_tuplock(relation, &(oldtup.t_self), *lockmode,
								 LockWaitBlock, &have_tuple_lock);
			XactLockTableWait(xwait, relation, &oldtup.t_self,
							  XLTW_Update);
			checked_lockers = true;
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

			/*
			 * xwait is done, but if xwait had just locked the tuple then some
			 * other xact could update this tuple before we get to this point.
			 * Check for xmax change, and start over if so.
			 */
			if (xmax_infomask_changed(oldtup.t_data->t_infomask, infomask) ||
				!TransactionIdEquals(xwait,
									 HeapTupleHeaderGetRawXmax(oldtup.t_data)))
				goto l2;

			/* Otherwise check if it committed or aborted */
			UpdateXmaxHintBits(oldtup.t_data, buffer, xwait);
			if (oldtup.t_data->t_infomask & HEAP_XMAX_INVALID)
				can_continue = true;
		}

		if (can_continue)
			result = TM_Ok;
		else if (!ItemPointerEquals(&oldtup.t_self, &oldtup.t_data->t_ctid))
			result = TM_Updated;
		else
			result = TM_Deleted;
	}

	/* Sanity check the result HeapTupleSatisfiesUpdate() and the logic above */
	if (result != TM_Ok)
	{
		Assert(result == TM_SelfModified ||
			   result == TM_Updated ||
			   result == TM_Deleted ||
			   result == TM_BeingModified);
		Assert(!(oldtup.t_data->t_infomask & HEAP_XMAX_INVALID));
		Assert(result != TM_Updated ||
			   !ItemPointerEquals(&oldtup.t_self, &oldtup.t_data->t_ctid));
	}

	if (crosscheck != InvalidSnapshot && result == TM_Ok)
	{
		/* Perform additional check for transaction-snapshot mode RI updates */
		if (!HeapTupleSatisfiesVisibility(&oldtup, crosscheck, buffer))
			result = TM_Updated;
	}

	if (result != TM_Ok)
	{
		tmfd->ctid = oldtup.t_data->t_ctid;
		tmfd->xmax = HeapTupleHeaderGetUpdateXid(oldtup.t_data);
		if (result == TM_SelfModified)
			tmfd->cmax = HeapTupleHeaderGetCmax(oldtup.t_data);
		else
			tmfd->cmax = InvalidCommandId;
		UnlockReleaseBuffer(buffer);
		if (have_tuple_lock)
			UnlockTupleTuplock(relation, &(oldtup.t_self), *lockmode);
		if (vmbuffer != InvalidBuffer)
			ReleaseBuffer(vmbuffer);
		*update_indexes = TU_None;

		bms_free(hot_attrs);
		bms_free(sum_attrs);
		bms_free(key_attrs);
		bms_free(id_attrs);
		bms_free(modified_attrs);
		bms_free(interesting_attrs);
		return result;
	}

	/*
	 * If we didn't pin the visibility map page and the page has become all
	 * visible while we were busy locking the buffer, or during some
	 * subsequent window during which we had it unlocked, we'll have to unlock
	 * and re-lock, to avoid holding the buffer lock across an I/O.  That's a
	 * bit unfortunate, especially since we'll now have to recheck whether the
	 * tuple has been locked or updated under us, but hopefully it won't
	 * happen very often.
	 */
	if (vmbuffer == InvalidBuffer && PageIsAllVisible(page))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		visibilitymap_pin(relation, block, &vmbuffer);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		goto l2;
	}

	/* Fill in transaction status data */

	/*
	 * If the tuple we're updating is locked, we need to preserve the locking
	 * info in the old tuple's Xmax.  Prepare a new Xmax value for this.
	 */
	compute_new_xmax_infomask(HeapTupleHeaderGetRawXmax(oldtup.t_data),
							  oldtup.t_data->t_infomask,
							  oldtup.t_data->t_infomask2,
							  xid, *lockmode, true,
							  &xmax_old_tuple, &infomask_old_tuple,
							  &infomask2_old_tuple);

	/*
	 * And also prepare an Xmax value for the new copy of the tuple.  If there
	 * was no xmax previously, or there was one but all lockers are now gone,
	 * then use InvalidTransactionId; otherwise, get the xmax from the old
	 * tuple.  (In rare cases that might also be InvalidTransactionId and yet
	 * not have the HEAP_XMAX_INVALID bit set; that's fine.)
	 */
	if ((oldtup.t_data->t_infomask & HEAP_XMAX_INVALID) ||
		HEAP_LOCKED_UPGRADED(oldtup.t_data->t_infomask) ||
		(checked_lockers && !locker_remains))
		xmax_new_tuple = InvalidTransactionId;
	else
		xmax_new_tuple = HeapTupleHeaderGetRawXmax(oldtup.t_data);

	if (!TransactionIdIsValid(xmax_new_tuple))
	{
		infomask_new_tuple = HEAP_XMAX_INVALID;
		infomask2_new_tuple = 0;
	}
	else
	{
		/*
		 * If we found a valid Xmax for the new tuple, then the infomask bits
		 * to use on the new tuple depend on what was there on the old one.
		 * Note that since we're doing an update, the only possibility is that
		 * the lockers had FOR KEY SHARE lock.
		 */
		if (oldtup.t_data->t_infomask & HEAP_XMAX_IS_MULTI)
		{
			GetMultiXactIdHintBits(xmax_new_tuple, &infomask_new_tuple,
								   &infomask2_new_tuple);
		}
		else
		{
			infomask_new_tuple = HEAP_XMAX_KEYSHR_LOCK | HEAP_XMAX_LOCK_ONLY;
			infomask2_new_tuple = 0;
		}
	}

	/*
	 * Prepare the new tuple with the appropriate initial values of Xmin and
	 * Xmax, as well as initial infomask bits as computed above.
	 */
	newtup->t_data->t_infomask &= ~(HEAP_XACT_MASK);
	newtup->t_data->t_infomask2 &= ~(HEAP2_XACT_MASK);
	HeapTupleHeaderSetXmin(newtup->t_data, xid);
	HeapTupleHeaderSetCmin(newtup->t_data, cid);
	newtup->t_data->t_infomask |= HEAP_UPDATED | infomask_new_tuple;
	newtup->t_data->t_infomask2 |= infomask2_new_tuple;
	HeapTupleHeaderSetXmax(newtup->t_data, xmax_new_tuple);

	/*
	 * Replace cid with a combo CID if necessary.  Note that we already put
	 * the plain cid into the new tuple.
	 */
	HeapTupleHeaderAdjustCmax(oldtup.t_data, &cid, &iscombo);

	/*
	 * If the toaster needs to be activated, OR if the new tuple will not fit
	 * on the same page as the old, then we need to release the content lock
	 * (but not the pin!) on the old tuple's buffer while we are off doing
	 * TOAST and/or table-file-extension work.  We must mark the old tuple to
	 * show that it's locked, else other processes may try to update it
	 * themselves.
	 *
	 * We need to invoke the toaster if there are already any out-of-line
	 * toasted values present, or if the new tuple is over-threshold.
	 */
	if (relation->rd_rel->relkind != RELKIND_RELATION &&
		relation->rd_rel->relkind != RELKIND_MATVIEW)
	{
		/* toast table entries should never be recursively toasted */
		Assert(!HeapTupleHasExternal(&oldtup));
		Assert(!HeapTupleHasExternal(newtup));
		need_toast = false;
	}
	else
		need_toast = (HeapTupleHasExternal(&oldtup) ||
					  HeapTupleHasExternal(newtup) ||
					  newtup->t_len > TOAST_TUPLE_THRESHOLD);

	pagefree = PageGetHeapFreeSpace(page);

	newtupsize = MAXALIGN(newtup->t_len);

	if (need_toast || newtupsize > pagefree)
	{
		TransactionId xmax_lock_old_tuple;
		uint16		infomask_lock_old_tuple,
					infomask2_lock_old_tuple;
		bool		cleared_all_frozen = false;

		/*
		 * To prevent concurrent sessions from updating the tuple, we have to
		 * temporarily mark it locked, while we release the page-level lock.
		 *
		 * To satisfy the rule that any xid potentially appearing in a buffer
		 * written out to disk, we unfortunately have to WAL log this
		 * temporary modification.  We can reuse xl_heap_lock for this
		 * purpose.  If we crash/error before following through with the
		 * actual update, xmax will be of an aborted transaction, allowing
		 * other sessions to proceed.
		 */

		/*
		 * Compute xmax / infomask appropriate for locking the tuple. This has
		 * to be done separately from the combo that's going to be used for
		 * updating, because the potentially created multixact would otherwise
		 * be wrong.
		 */
		compute_new_xmax_infomask(HeapTupleHeaderGetRawXmax(oldtup.t_data),
								  oldtup.t_data->t_infomask,
								  oldtup.t_data->t_infomask2,
								  xid, *lockmode, false,
								  &xmax_lock_old_tuple, &infomask_lock_old_tuple,
								  &infomask2_lock_old_tuple);

		Assert(HEAP_XMAX_IS_LOCKED_ONLY(infomask_lock_old_tuple));

		START_CRIT_SECTION();

		/* Clear obsolete visibility flags ... */
		oldtup.t_data->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
		oldtup.t_data->t_infomask2 &= ~HEAP_KEYS_UPDATED;
		HeapTupleClearHotUpdated(&oldtup);
		/* ... and store info about transaction updating this tuple */
		Assert(TransactionIdIsValid(xmax_lock_old_tuple));
		HeapTupleHeaderSetXmax(oldtup.t_data, xmax_lock_old_tuple);
		oldtup.t_data->t_infomask |= infomask_lock_old_tuple;
		oldtup.t_data->t_infomask2 |= infomask2_lock_old_tuple;
		HeapTupleHeaderSetCmax(oldtup.t_data, cid, iscombo);

		/* temporarily make it look not-updated, but locked */
		oldtup.t_data->t_ctid = oldtup.t_self;

		/*
		 * Clear all-frozen bit on visibility map if needed. We could
		 * immediately reset ALL_VISIBLE, but given that the WAL logging
		 * overhead would be unchanged, that doesn't seem necessarily
		 * worthwhile.
		 */
		if (PageIsAllVisible(page) &&
			visibilitymap_clear(relation, block, vmbuffer,
								VISIBILITYMAP_ALL_FROZEN))
			cleared_all_frozen = true;

		MarkBufferDirty(buffer);

		if (RelationNeedsWAL(relation))
		{
			xl_heap_lock xlrec;
			XLogRecPtr	recptr;

			XLogBeginInsert();
			XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

			xlrec.offnum = ItemPointerGetOffsetNumber(&oldtup.t_self);
			xlrec.xmax = xmax_lock_old_tuple;
			xlrec.infobits_set = compute_infobits(oldtup.t_data->t_infomask,
												  oldtup.t_data->t_infomask2);
			xlrec.flags =
				cleared_all_frozen ? XLH_LOCK_ALL_FROZEN_CLEARED : 0;
			XLogRegisterData(&xlrec, SizeOfHeapLock);
			recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_LOCK);
			PageSetLSN(page, recptr);
		}

		END_CRIT_SECTION();

		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

		/*
		 * Let the toaster do its thing, if needed.
		 *
		 * Note: below this point, heaptup is the data we actually intend to
		 * store into the relation; newtup is the caller's original untoasted
		 * data.
		 */
		if (need_toast)
		{
			/* Note we always use WAL and FSM during updates */
			heaptup = heap_toast_insert_or_update(relation, newtup, &oldtup, 0);
			newtupsize = MAXALIGN(heaptup->t_len);
		}
		else
			heaptup = newtup;

		/*
		 * Now, do we need a new page for the tuple, or not?  This is a bit
		 * tricky since someone else could have added tuples to the page while
		 * we weren't looking.  We have to recheck the available space after
		 * reacquiring the buffer lock.  But don't bother to do that if the
		 * former amount of free space is still not enough; it's unlikely
		 * there's more free now than before.
		 *
		 * What's more, if we need to get a new page, we will need to acquire
		 * buffer locks on both old and new pages.  To avoid deadlock against
		 * some other backend trying to get the same two locks in the other
		 * order, we must be consistent about the order we get the locks in.
		 * We use the rule "lock the lower-numbered page of the relation
		 * first".  To implement this, we must do RelationGetBufferForTuple
		 * while not holding the lock on the old page, and we must rely on it
		 * to get the locks on both pages in the correct order.
		 *
		 * Another consideration is that we need visibility map page pin(s) if
		 * we will have to clear the all-visible flag on either page.  If we
		 * call RelationGetBufferForTuple, we rely on it to acquire any such
		 * pins; but if we don't, we have to handle that here.  Hence we need
		 * a loop.
		 */
		for (;;)
		{
			if (newtupsize > pagefree)
			{
				/* It doesn't fit, must use RelationGetBufferForTuple. */
				newbuf = RelationGetBufferForTuple(relation, heaptup->t_len,
												   buffer, 0, NULL,
												   &vmbuffer_new, &vmbuffer,
												   0);
				/* We're all done. */
				break;
			}
			/* Acquire VM page pin if needed and we don't have it. */
			if (vmbuffer == InvalidBuffer && PageIsAllVisible(page))
				visibilitymap_pin(relation, block, &vmbuffer);
			/* Re-acquire the lock on the old tuple's page. */
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
			/* Re-check using the up-to-date free space */
			pagefree = PageGetHeapFreeSpace(page);
			if (newtupsize > pagefree ||
				(vmbuffer == InvalidBuffer && PageIsAllVisible(page)))
			{
				/*
				 * Rats, it doesn't fit anymore, or somebody just now set the
				 * all-visible flag.  We must now unlock and loop to avoid
				 * deadlock.  Fortunately, this path should seldom be taken.
				 */
				LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			}
			else
			{
				/* We're all done. */
				newbuf = buffer;
				break;
			}
		}
	}
	else
	{
		/* No TOAST work needed, and it'll fit on same page */
		newbuf = buffer;
		heaptup = newtup;
	}

	/*
	 * We're about to do the actual update -- check for conflict first, to
	 * avoid possibly having to roll back work we've just done.
	 *
	 * This is safe without a recheck as long as there is no possibility of
	 * another process scanning the pages between this check and the update
	 * being visible to the scan (i.e., exclusive buffer content lock(s) are
	 * continuously held from this point until the tuple update is visible).
	 *
	 * For the new tuple the only check needed is at the relation level, but
	 * since both tuples are in the same relation and the check for oldtup
	 * will include checking the relation level, there is no benefit to a
	 * separate check for the new tuple.
	 */
	CheckForSerializableConflictIn(relation, &oldtup.t_self,
								   BufferGetBlockNumber(buffer));

	/*
	 * At this point newbuf and buffer are both pinned and locked, and newbuf
	 * has enough space for the new tuple.  If they are the same buffer, only
	 * one pin is held.
	 */

	if (newbuf == buffer)
	{
		/*
		 * Since the new tuple is going into the same page, we might be able
		 * to do a HOT update.  Check if any of the index columns have been
		 * changed.
		 */
		if (!bms_overlap(modified_attrs, hot_attrs))
		{
			use_hot_update = true;

			/*
			 * If none of the columns that are used in hot-blocking indexes
			 * were updated, we can apply HOT, but we do still need to check
			 * if we need to update the summarizing indexes, and update those
			 * indexes if the columns were updated, or we may fail to detect
			 * e.g. value bound changes in BRIN minmax indexes.
			 */
			if (bms_overlap(modified_attrs, sum_attrs))
				summarized_update = true;
		}
	}
	else
	{
		/* Set a hint that the old page could use prune/defrag */
		PageSetFull(page);
	}

	/*
	 * Compute replica identity tuple before entering the critical section so
	 * we don't PANIC upon a memory allocation failure.
	 * ExtractReplicaIdentity() will return NULL if nothing needs to be
	 * logged.  Pass old key required as true only if the replica identity key
	 * columns are modified or it has external data.
	 */
	old_key_tuple = ExtractReplicaIdentity(relation, &oldtup,
										   bms_overlap(modified_attrs, id_attrs) ||
										   id_has_external,
										   &old_key_copied);

	/* NO EREPORT(ERROR) from here till changes are logged */
	START_CRIT_SECTION();

	/*
	 * If this transaction commits, the old tuple will become DEAD sooner or
	 * later.  Set flag that this page is a candidate for pruning once our xid
	 * falls below the OldestXmin horizon.  If the transaction finally aborts,
	 * the subsequent page pruning will be a no-op and the hint will be
	 * cleared.
	 *
	 * XXX Should we set hint on newbuf as well?  If the transaction aborts,
	 * there would be a prunable tuple in the newbuf; but for now we choose
	 * not to optimize for aborts.  Note that heap_xlog_update must be kept in
	 * sync if this decision changes.
	 */
	PageSetPrunable(page, xid);

	if (use_hot_update)
	{
		/* Mark the old tuple as HOT-updated */
		HeapTupleSetHotUpdated(&oldtup);
		/* And mark the new tuple as heap-only */
		HeapTupleSetHeapOnly(heaptup);
		/* Mark the caller's copy too, in case different from heaptup */
		HeapTupleSetHeapOnly(newtup);
	}
	else
	{
		/* Make sure tuples are correctly marked as not-HOT */
		HeapTupleClearHotUpdated(&oldtup);
		HeapTupleClearHeapOnly(heaptup);
		HeapTupleClearHeapOnly(newtup);
	}

	RelationPutHeapTuple(relation, newbuf, heaptup, false); /* insert new tuple */


	/* Clear obsolete visibility flags, possibly set by ourselves above... */
	oldtup.t_data->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
	oldtup.t_data->t_infomask2 &= ~HEAP_KEYS_UPDATED;
	/* ... and store info about transaction updating this tuple */
	Assert(TransactionIdIsValid(xmax_old_tuple));
	HeapTupleHeaderSetXmax(oldtup.t_data, xmax_old_tuple);
	oldtup.t_data->t_infomask |= infomask_old_tuple;
	oldtup.t_data->t_infomask2 |= infomask2_old_tuple;
	HeapTupleHeaderSetCmax(oldtup.t_data, cid, iscombo);

	/* record address of new tuple in t_ctid of old one */
	oldtup.t_data->t_ctid = heaptup->t_self;

	/* clear PD_ALL_VISIBLE flags, reset all visibilitymap bits */
	if (PageIsAllVisible(BufferGetPage(buffer)))
	{
		all_visible_cleared = true;
		PageClearAllVisible(BufferGetPage(buffer));
		visibilitymap_clear(relation, BufferGetBlockNumber(buffer),
							vmbuffer, VISIBILITYMAP_VALID_BITS);
	}
	if (newbuf != buffer && PageIsAllVisible(BufferGetPage(newbuf)))
	{
		all_visible_cleared_new = true;
		PageClearAllVisible(BufferGetPage(newbuf));
		visibilitymap_clear(relation, BufferGetBlockNumber(newbuf),
							vmbuffer_new, VISIBILITYMAP_VALID_BITS);
	}

	if (newbuf != buffer)
		MarkBufferDirty(newbuf);
	MarkBufferDirty(buffer);

	/* XLOG stuff */
	if (RelationNeedsWAL(relation))
	{
		XLogRecPtr	recptr;

		/*
		 * For logical decoding we need combo CIDs to properly decode the
		 * catalog.
		 */
		if (RelationIsAccessibleInLogicalDecoding(relation))
		{
			log_heap_new_cid(relation, &oldtup);
			log_heap_new_cid(relation, heaptup);
		}

		recptr = log_heap_update(relation, buffer,
								 newbuf, &oldtup, heaptup,
								 old_key_tuple,
								 all_visible_cleared,
								 all_visible_cleared_new);
		if (newbuf != buffer)
		{
			PageSetLSN(BufferGetPage(newbuf), recptr);
		}
		PageSetLSN(BufferGetPage(buffer), recptr);
	}

	END_CRIT_SECTION();

	if (newbuf != buffer)
		LockBuffer(newbuf, BUFFER_LOCK_UNLOCK);
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	/*
	 * Mark old tuple for invalidation from system caches at next command
	 * boundary, and mark the new tuple for invalidation in case we abort. We
	 * have to do this before releasing the buffer because oldtup is in the
	 * buffer.  (heaptup is all in local memory, but it's necessary to process
	 * both tuple versions in one call to inval.c so we can avoid redundant
	 * sinval messages.)
	 */
	CacheInvalidateHeapTuple(relation, &oldtup, heaptup);

	/* Now we can release the buffer(s) */
	if (newbuf != buffer)
		ReleaseBuffer(newbuf);
	ReleaseBuffer(buffer);
	if (BufferIsValid(vmbuffer_new))
		ReleaseBuffer(vmbuffer_new);
	if (BufferIsValid(vmbuffer))
		ReleaseBuffer(vmbuffer);

	/*
	 * Release the lmgr tuple lock, if we had it.
	 */
	if (have_tuple_lock)
		UnlockTupleTuplock(relation, &(oldtup.t_self), *lockmode);

	pgstat_count_heap_update(relation, use_hot_update, newbuf != buffer);

	/*
	 * If heaptup is a private copy, release it.  Don't forget to copy t_self
	 * back to the caller's image, too.
	 */
	if (heaptup != newtup)
	{
		newtup->t_self = heaptup->t_self;
		heap_freetuple(heaptup);
	}

	/*
	 * If it is a HOT update, the update may still need to update summarized
	 * indexes, lest we fail to update those summaries and get incorrect
	 * results (for example, minmax bounds of the block may change with this
	 * update).
	 */
	if (use_hot_update)
	{
		if (summarized_update)
			*update_indexes = TU_Summarizing;
		else
			*update_indexes = TU_None;
	}
	else
		*update_indexes = TU_All;

	if (old_key_tuple != NULL && old_key_copied)
		heap_freetuple(old_key_tuple);

	bms_free(hot_attrs);
	bms_free(sum_attrs);
	bms_free(key_attrs);
	bms_free(id_attrs);
	bms_free(modified_attrs);
	bms_free(interesting_attrs);

	return TM_Ok;
}

#ifdef USE_ASSERT_CHECKING
/*
 * Confirm adequate lock held during heap_update(), per rules from
 * README.tuplock section "Locking to write inplace-updated tables".
 */
static void
check_lock_if_inplace_updateable_rel(Relation relation,
									 ItemPointer otid,
									 HeapTuple newtup)
{
	/* LOCKTAG_TUPLE acceptable for any catalog */
	switch (RelationGetRelid(relation))
	{
		case RelationRelationId:
		case DatabaseRelationId:
			{
				LOCKTAG		tuptag;

				SET_LOCKTAG_TUPLE(tuptag,
								  relation->rd_lockInfo.lockRelId.dbId,
								  relation->rd_lockInfo.lockRelId.relId,
								  ItemPointerGetBlockNumber(otid),
								  ItemPointerGetOffsetNumber(otid));
				if (LockHeldByMe(&tuptag, InplaceUpdateTupleLock, false))
					return;
			}
			break;
		default:
			Assert(!IsInplaceUpdateRelation(relation));
			return;
	}

	switch (RelationGetRelid(relation))
	{
		case RelationRelationId:
			{
				/* LOCKTAG_TUPLE or LOCKTAG_RELATION ok */
				Form_pg_class classForm = (Form_pg_class) GETSTRUCT(newtup);
				Oid			relid = classForm->oid;
				Oid			dbid;
				LOCKTAG		tag;

				if (IsSharedRelation(relid))
					dbid = InvalidOid;
				else
					dbid = MyDatabaseId;

				if (classForm->relkind == RELKIND_INDEX)
				{
					Relation	irel = index_open(relid, AccessShareLock);

					SET_LOCKTAG_RELATION(tag, dbid, irel->rd_index->indrelid);
					index_close(irel, AccessShareLock);
				}
				else
					SET_LOCKTAG_RELATION(tag, dbid, relid);

				if (!LockHeldByMe(&tag, ShareUpdateExclusiveLock, false) &&
					!LockHeldByMe(&tag, ShareRowExclusiveLock, true))
					elog(WARNING,
						 "missing lock for relation \"%s\" (OID %u, relkind %c) @ TID (%u,%u)",
						 NameStr(classForm->relname),
						 relid,
						 classForm->relkind,
						 ItemPointerGetBlockNumber(otid),
						 ItemPointerGetOffsetNumber(otid));
			}
			break;
		case DatabaseRelationId:
			{
				/* LOCKTAG_TUPLE required */
				Form_pg_database dbForm = (Form_pg_database) GETSTRUCT(newtup);

				elog(WARNING,
					 "missing lock on database \"%s\" (OID %u) @ TID (%u,%u)",
					 NameStr(dbForm->datname),
					 dbForm->oid,
					 ItemPointerGetBlockNumber(otid),
					 ItemPointerGetOffsetNumber(otid));
			}
			break;
	}
}

/*
 * Confirm adequate relation lock held, per rules from README.tuplock section
 * "Locking to write inplace-updated tables".
 */
static void
check_inplace_rel_lock(HeapTuple oldtup)
{
	Form_pg_class classForm = (Form_pg_class) GETSTRUCT(oldtup);
	Oid			relid = classForm->oid;
	Oid			dbid;
	LOCKTAG		tag;

	if (IsSharedRelation(relid))
		dbid = InvalidOid;
	else
		dbid = MyDatabaseId;

	if (classForm->relkind == RELKIND_INDEX)
	{
		Relation	irel = index_open(relid, AccessShareLock);

		SET_LOCKTAG_RELATION(tag, dbid, irel->rd_index->indrelid);
		index_close(irel, AccessShareLock);
	}
	else
		SET_LOCKTAG_RELATION(tag, dbid, relid);

	if (!LockHeldByMe(&tag, ShareUpdateExclusiveLock, true))
		elog(WARNING,
			 "missing lock for relation \"%s\" (OID %u, relkind %c) @ TID (%u,%u)",
			 NameStr(classForm->relname),
			 relid,
			 classForm->relkind,
			 ItemPointerGetBlockNumber(&oldtup->t_self),
			 ItemPointerGetOffsetNumber(&oldtup->t_self));
}
#endif

/*
 * Check if the specified attribute's values are the same.  Subroutine for
 * HeapDetermineColumnsInfo.
 */
static bool
heap_attr_equals(TupleDesc tupdesc, int attrnum, Datum value1, Datum value2,
				 bool isnull1, bool isnull2)
{
	/*
	 * If one value is NULL and other is not, then they are certainly not
	 * equal
	 */
	if (isnull1 != isnull2)
		return false;

	/*
	 * If both are NULL, they can be considered equal.
	 */
	if (isnull1)
		return true;

	/*
	 * We do simple binary comparison of the two datums.  This may be overly
	 * strict because there can be multiple binary representations for the
	 * same logical value.  But we should be OK as long as there are no false
	 * positives.  Using a type-specific equality operator is messy because
	 * there could be multiple notions of equality in different operator
	 * classes; furthermore, we cannot safely invoke user-defined functions
	 * while holding exclusive buffer lock.
	 */
	if (attrnum <= 0)
	{
		/* The only allowed system columns are OIDs, so do this */
		return (DatumGetObjectId(value1) == DatumGetObjectId(value2));
	}
	else
	{
		CompactAttribute *att;

		Assert(attrnum <= tupdesc->natts);
		att = TupleDescCompactAttr(tupdesc, attrnum - 1);
		return datumIsEqual(value1, value2, att->attbyval, att->attlen);
	}
}

/*
 * Check which columns are being updated.
 *
 * Given an updated tuple, determine (and return into the output bitmapset),
 * from those listed as interesting, the set of columns that changed.
 *
 * has_external indicates if any of the unmodified attributes (from those
 * listed as interesting) of the old tuple is a member of external_cols and is
 * stored externally.
 */
static Bitmapset *
HeapDetermineColumnsInfo(Relation relation,
						 Bitmapset *interesting_cols,
						 Bitmapset *external_cols,
						 HeapTuple oldtup, HeapTuple newtup,
						 bool *has_external)
{
	int			attidx;
	Bitmapset  *modified = NULL;
	TupleDesc	tupdesc = RelationGetDescr(relation);

	attidx = -1;
	while ((attidx = bms_next_member(interesting_cols, attidx)) >= 0)
	{
		/* attidx is zero-based, attrnum is the normal attribute number */
		AttrNumber	attrnum = attidx + FirstLowInvalidHeapAttributeNumber;
		Datum		value1,
					value2;
		bool		isnull1,
					isnull2;

		/*
		 * If it's a whole-tuple reference, say "not equal".  It's not really
		 * worth supporting this case, since it could only succeed after a
		 * no-op update, which is hardly a case worth optimizing for.
		 */
		if (attrnum == 0)
		{
			modified = bms_add_member(modified, attidx);
			continue;
		}

		/*
		 * Likewise, automatically say "not equal" for any system attribute
		 * other than tableOID; we cannot expect these to be consistent in a
		 * HOT chain, or even to be set correctly yet in the new tuple.
		 */
		if (attrnum < 0)
		{
			if (attrnum != TableOidAttributeNumber)
			{
				modified = bms_add_member(modified, attidx);
				continue;
			}
		}

		/*
		 * Extract the corresponding values.  XXX this is pretty inefficient
		 * if there are many indexed columns.  Should we do a single
		 * heap_deform_tuple call on each tuple, instead?	But that doesn't
		 * work for system columns ...
		 */
		value1 = heap_getattr(oldtup, attrnum, tupdesc, &isnull1);
		value2 = heap_getattr(newtup, attrnum, tupdesc, &isnull2);

		if (!heap_attr_equals(tupdesc, attrnum, value1,
							  value2, isnull1, isnull2))
		{
			modified = bms_add_member(modified, attidx);
			continue;
		}

		/*
		 * No need to check attributes that can't be stored externally. Note
		 * that system attributes can't be stored externally.
		 */
		if (attrnum < 0 || isnull1 ||
			TupleDescCompactAttr(tupdesc, attrnum - 1)->attlen != -1)
			continue;

		/*
		 * Check if the old tuple's attribute is stored externally and is a
		 * member of external_cols.
		 */
		if (VARATT_IS_EXTERNAL((struct varlena *) DatumGetPointer(value1)) &&
			bms_is_member(attidx, external_cols))
			*has_external = true;
	}

	return modified;
}

/*
 *	simple_heap_update - replace a tuple
 *
 * This routine may be used to update a tuple when concurrent updates of
 * the target tuple are not expected (for example, because we have a lock
 * on the relation associated with the tuple).  Any failure is reported
 * via ereport().
 */
void
simple_heap_update(Relation relation, ItemPointer otid, HeapTuple tup,
				   TU_UpdateIndexes *update_indexes)
{
	TM_Result	result;
	TM_FailureData tmfd;
	LockTupleMode lockmode;

	result = heap_update(relation, otid, tup,
						 GetCurrentCommandId(true), InvalidSnapshot,
						 true /* wait for commit */ ,
						 &tmfd, &lockmode, update_indexes);
	switch (result)
	{
		case TM_SelfModified:
			/* Tuple was already updated in current command? */
			elog(ERROR, "tuple already updated by self");
			break;

		case TM_Ok:
			/* done successfully */
			break;

		case TM_Updated:
			elog(ERROR, "tuple concurrently updated");
			break;

		case TM_Deleted:
			elog(ERROR, "tuple concurrently deleted");
			break;

		default:
			elog(ERROR, "unrecognized heap_update status: %u", result);
			break;
	}
}


/*
 * Return the MultiXactStatus corresponding to the given tuple lock mode.
 */
static MultiXactStatus
get_mxact_status_for_lock(LockTupleMode mode, bool is_update)
{
	int			retval;

	if (is_update)
		retval = tupleLockExtraInfo[mode].updstatus;
	else
		retval = tupleLockExtraInfo[mode].lockstatus;

	if (retval == -1)
		elog(ERROR, "invalid lock tuple mode %d/%s", mode,
			 is_update ? "true" : "false");

	return (MultiXactStatus) retval;
}

/*
 *	heap_lock_tuple - lock a tuple in shared or exclusive mode
 *
 * Note that this acquires a buffer pin, which the caller must release.
 *
 * Input parameters:
 *	relation: relation containing tuple (caller must hold suitable lock)
 *	tid: TID of tuple to lock
 *	cid: current command ID (used for visibility test, and stored into
 *		tuple's cmax if lock is successful)
 *	mode: indicates if shared or exclusive tuple lock is desired
 *	wait_policy: what to do if tuple lock is not available
 *	follow_updates: if true, follow the update chain to also lock descendant
 *		tuples.
 *
 * Output parameters:
 *	*tuple: all fields filled in
 *	*buffer: set to buffer holding tuple (pinned but not locked at exit)
 *	*tmfd: filled in failure cases (see below)
 *
 * Function results are the same as the ones for table_tuple_lock().
 *
 * In the failure cases other than TM_Invisible, the routine fills
 * *tmfd with the tuple's t_ctid, t_xmax (resolving a possible MultiXact,
 * if necessary), and t_cmax (the last only for TM_SelfModified,
 * since we cannot obtain cmax from a combo CID generated by another
 * transaction).
 * See comments for struct TM_FailureData for additional info.
 *
 * See README.tuplock for a thorough explanation of this mechanism.
 */
TM_Result
heap_lock_tuple(Relation relation, HeapTuple tuple,
				CommandId cid, LockTupleMode mode, LockWaitPolicy wait_policy,
				bool follow_updates,
				Buffer *buffer, TM_FailureData *tmfd)
{
	TM_Result	result;
	ItemPointer tid = &(tuple->t_self);
	ItemId		lp;
	Page		page;
	Buffer		vmbuffer = InvalidBuffer;
	BlockNumber block;
	TransactionId xid,
				xmax;
	uint16		old_infomask,
				new_infomask,
				new_infomask2;
	bool		first_time = true;
	bool		skip_tuple_lock = false;
	bool		have_tuple_lock = false;
	bool		cleared_all_frozen = false;

	*buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));
	block = ItemPointerGetBlockNumber(tid);

	/*
	 * Before locking the buffer, pin the visibility map page if it appears to
	 * be necessary.  Since we haven't got the lock yet, someone else might be
	 * in the middle of changing this, so we'll need to recheck after we have
	 * the lock.
	 */
	if (PageIsAllVisible(BufferGetPage(*buffer)))
		visibilitymap_pin(relation, block, &vmbuffer);

	LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);

	page = BufferGetPage(*buffer);
	lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));
	Assert(ItemIdIsNormal(lp));

	tuple->t_data = (HeapTupleHeader) PageGetItem(page, lp);
	tuple->t_len = ItemIdGetLength(lp);
	tuple->t_tableOid = RelationGetRelid(relation);

l3:
	result = HeapTupleSatisfiesUpdate(tuple, cid, *buffer);

	if (result == TM_Invisible)
	{
		/*
		 * This is possible, but only when locking a tuple for ON CONFLICT
		 * UPDATE.  We return this value here rather than throwing an error in
		 * order to give that case the opportunity to throw a more specific
		 * error.
		 */
		result = TM_Invisible;
		goto out_locked;
	}
	else if (result == TM_BeingModified ||
			 result == TM_Updated ||
			 result == TM_Deleted)
	{
		TransactionId xwait;
		uint16		infomask;
		uint16		infomask2;
		bool		require_sleep;
		ItemPointerData t_ctid;

		/* must copy state data before unlocking buffer */
		xwait = HeapTupleHeaderGetRawXmax(tuple->t_data);
		infomask = tuple->t_data->t_infomask;
		infomask2 = tuple->t_data->t_infomask2;
		ItemPointerCopy(&tuple->t_data->t_ctid, &t_ctid);

		LockBuffer(*buffer, BUFFER_LOCK_UNLOCK);

		/*
		 * If any subtransaction of the current top transaction already holds
		 * a lock as strong as or stronger than what we're requesting, we
		 * effectively hold the desired lock already.  We *must* succeed
		 * without trying to take the tuple lock, else we will deadlock
		 * against anyone wanting to acquire a stronger lock.
		 *
		 * Note we only do this the first time we loop on the HTSU result;
		 * there is no point in testing in subsequent passes, because
		 * evidently our own transaction cannot have acquired a new lock after
		 * the first time we checked.
		 */
		if (first_time)
		{
			first_time = false;

			if (infomask & HEAP_XMAX_IS_MULTI)
			{
				int			i;
				int			nmembers;
				MultiXactMember *members;

				/*
				 * We don't need to allow old multixacts here; if that had
				 * been the case, HeapTupleSatisfiesUpdate would have returned
				 * MayBeUpdated and we wouldn't be here.
				 */
				nmembers =
					GetMultiXactIdMembers(xwait, &members, false,
										  HEAP_XMAX_IS_LOCKED_ONLY(infomask));

				for (i = 0; i < nmembers; i++)
				{
					/* only consider members of our own transaction */
					if (!TransactionIdIsCurrentTransactionId(members[i].xid))
						continue;

					if (TUPLOCK_from_mxstatus(members[i].status) >= mode)
					{
						pfree(members);
						result = TM_Ok;
						goto out_unlocked;
					}
					else
					{
						/*
						 * Disable acquisition of the heavyweight tuple lock.
						 * Otherwise, when promoting a weaker lock, we might
						 * deadlock with another locker that has acquired the
						 * heavyweight tuple lock and is waiting for our
						 * transaction to finish.
						 *
						 * Note that in this case we still need to wait for
						 * the multixact if required, to avoid acquiring
						 * conflicting locks.
						 */
						skip_tuple_lock = true;
					}
				}

				if (members)
					pfree(members);
			}
			else if (TransactionIdIsCurrentTransactionId(xwait))
			{
				switch (mode)
				{
					case LockTupleKeyShare:
						Assert(HEAP_XMAX_IS_KEYSHR_LOCKED(infomask) ||
							   HEAP_XMAX_IS_SHR_LOCKED(infomask) ||
							   HEAP_XMAX_IS_EXCL_LOCKED(infomask));
						result = TM_Ok;
						goto out_unlocked;
					case LockTupleShare:
						if (HEAP_XMAX_IS_SHR_LOCKED(infomask) ||
							HEAP_XMAX_IS_EXCL_LOCKED(infomask))
						{
							result = TM_Ok;
							goto out_unlocked;
						}
						break;
					case LockTupleNoKeyExclusive:
						if (HEAP_XMAX_IS_EXCL_LOCKED(infomask))
						{
							result = TM_Ok;
							goto out_unlocked;
						}
						break;
					case LockTupleExclusive:
						if (HEAP_XMAX_IS_EXCL_LOCKED(infomask) &&
							infomask2 & HEAP_KEYS_UPDATED)
						{
							result = TM_Ok;
							goto out_unlocked;
						}
						break;
				}
			}
		}

		/*
		 * Initially assume that we will have to wait for the locking
		 * transaction(s) to finish.  We check various cases below in which
		 * this can be turned off.
		 */
		require_sleep = true;
		if (mode == LockTupleKeyShare)
		{
			/*
			 * If we're requesting KeyShare, and there's no update present, we
			 * don't need to wait.  Even if there is an update, we can still
			 * continue if the key hasn't been modified.
			 *
			 * However, if there are updates, we need to walk the update chain
			 * to mark future versions of the row as locked, too.  That way,
			 * if somebody deletes that future version, we're protected
			 * against the key going away.  This locking of future versions
			 * could block momentarily, if a concurrent transaction is
			 * deleting a key; or it could return a value to the effect that
			 * the transaction deleting the key has already committed.  So we
			 * do this before re-locking the buffer; otherwise this would be
			 * prone to deadlocks.
			 *
			 * Note that the TID we're locking was grabbed before we unlocked
			 * the buffer.  For it to change while we're not looking, the
			 * other properties we're testing for below after re-locking the
			 * buffer would also change, in which case we would restart this
			 * loop above.
			 */
			if (!(infomask2 & HEAP_KEYS_UPDATED))
			{
				bool		updated;

				updated = !HEAP_XMAX_IS_LOCKED_ONLY(infomask);

				/*
				 * If there are updates, follow the update chain; bail out if
				 * that cannot be done.
				 */
				if (follow_updates && updated)
				{
					TM_Result	res;

					res = heap_lock_updated_tuple(relation, tuple, &t_ctid,
												  GetCurrentTransactionId(),
												  mode);
					if (res != TM_Ok)
					{
						result = res;
						/* recovery code expects to have buffer lock held */
						LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
						goto failed;
					}
				}

				LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);

				/*
				 * Make sure it's still an appropriate lock, else start over.
				 * Also, if it wasn't updated before we released the lock, but
				 * is updated now, we start over too; the reason is that we
				 * now need to follow the update chain to lock the new
				 * versions.
				 */
				if (!HeapTupleHeaderIsOnlyLocked(tuple->t_data) &&
					((tuple->t_data->t_infomask2 & HEAP_KEYS_UPDATED) ||
					 !updated))
					goto l3;

				/* Things look okay, so we can skip sleeping */
				require_sleep = false;

				/*
				 * Note we allow Xmax to change here; other updaters/lockers
				 * could have modified it before we grabbed the buffer lock.
				 * However, this is not a problem, because with the recheck we
				 * just did we ensure that they still don't conflict with the
				 * lock we want.
				 */
			}
		}
		else if (mode == LockTupleShare)
		{
			/*
			 * If we're requesting Share, we can similarly avoid sleeping if
			 * there's no update and no exclusive lock present.
			 */
			if (HEAP_XMAX_IS_LOCKED_ONLY(infomask) &&
				!HEAP_XMAX_IS_EXCL_LOCKED(infomask))
			{
				LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);

				/*
				 * Make sure it's still an appropriate lock, else start over.
				 * See above about allowing xmax to change.
				 */
				if (!HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_data->t_infomask) ||
					HEAP_XMAX_IS_EXCL_LOCKED(tuple->t_data->t_infomask))
					goto l3;
				require_sleep = false;
			}
		}
		else if (mode == LockTupleNoKeyExclusive)
		{
			/*
			 * If we're requesting NoKeyExclusive, we might also be able to
			 * avoid sleeping; just ensure that there no conflicting lock
			 * already acquired.
			 */
			if (infomask & HEAP_XMAX_IS_MULTI)
			{
				if (!DoesMultiXactIdConflict((MultiXactId) xwait, infomask,
											 mode, NULL))
				{
					/*
					 * No conflict, but if the xmax changed under us in the
					 * meantime, start over.
					 */
					LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
					if (xmax_infomask_changed(tuple->t_data->t_infomask, infomask) ||
						!TransactionIdEquals(HeapTupleHeaderGetRawXmax(tuple->t_data),
											 xwait))
						goto l3;

					/* otherwise, we're good */
					require_sleep = false;
				}
			}
			else if (HEAP_XMAX_IS_KEYSHR_LOCKED(infomask))
			{
				LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);

				/* if the xmax changed in the meantime, start over */
				if (xmax_infomask_changed(tuple->t_data->t_infomask, infomask) ||
					!TransactionIdEquals(HeapTupleHeaderGetRawXmax(tuple->t_data),
										 xwait))
					goto l3;
				/* otherwise, we're good */
				require_sleep = false;
			}
		}

		/*
		 * As a check independent from those above, we can also avoid sleeping
		 * if the current transaction is the sole locker of the tuple.  Note
		 * that the strength of the lock already held is irrelevant; this is
		 * not about recording the lock in Xmax (which will be done regardless
		 * of this optimization, below).  Also, note that the cases where we
		 * hold a lock stronger than we are requesting are already handled
		 * above by not doing anything.
		 *
		 * Note we only deal with the non-multixact case here; MultiXactIdWait
		 * is well equipped to deal with this situation on its own.
		 */
		if (require_sleep && !(infomask & HEAP_XMAX_IS_MULTI) &&
			TransactionIdIsCurrentTransactionId(xwait))
		{
			/* ... but if the xmax changed in the meantime, start over */
			LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
			if (xmax_infomask_changed(tuple->t_data->t_infomask, infomask) ||
				!TransactionIdEquals(HeapTupleHeaderGetRawXmax(tuple->t_data),
									 xwait))
				goto l3;
			Assert(HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_data->t_infomask));
			require_sleep = false;
		}

		/*
		 * Time to sleep on the other transaction/multixact, if necessary.
		 *
		 * If the other transaction is an update/delete that's already
		 * committed, then sleeping cannot possibly do any good: if we're
		 * required to sleep, get out to raise an error instead.
		 *
		 * By here, we either have already acquired the buffer exclusive lock,
		 * or we must wait for the locking transaction or multixact; so below
		 * we ensure that we grab buffer lock after the sleep.
		 */
		if (require_sleep && (result == TM_Updated || result == TM_Deleted))
		{
			LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
			goto failed;
		}
		else if (require_sleep)
		{
			/*
			 * Acquire tuple lock to establish our priority for the tuple, or
			 * die trying.  LockTuple will release us when we are next-in-line
			 * for the tuple.  We must do this even if we are share-locking,
			 * but not if we already have a weaker lock on the tuple.
			 *
			 * If we are forced to "start over" below, we keep the tuple lock;
			 * this arranges that we stay at the head of the line while
			 * rechecking tuple state.
			 */
			if (!skip_tuple_lock &&
				!heap_acquire_tuplock(relation, tid, mode, wait_policy,
									  &have_tuple_lock))
			{
				/*
				 * This can only happen if wait_policy is Skip and the lock
				 * couldn't be obtained.
				 */
				result = TM_WouldBlock;
				/* recovery code expects to have buffer lock held */
				LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
				goto failed;
			}

			if (infomask & HEAP_XMAX_IS_MULTI)
			{
				MultiXactStatus status = get_mxact_status_for_lock(mode, false);

				/* We only ever lock tuples, never update them */
				if (status >= MultiXactStatusNoKeyUpdate)
					elog(ERROR, "invalid lock mode in heap_lock_tuple");

				/* wait for multixact to end, or die trying  */
				switch (wait_policy)
				{
					case LockWaitBlock:
						MultiXactIdWait((MultiXactId) xwait, status, infomask,
										relation, &tuple->t_self, XLTW_Lock, NULL);
						break;
					case LockWaitSkip:
						if (!ConditionalMultiXactIdWait((MultiXactId) xwait,
														status, infomask, relation,
														NULL, false))
						{
							result = TM_WouldBlock;
							/* recovery code expects to have buffer lock held */
							LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
							goto failed;
						}
						break;
					case LockWaitError:
						if (!ConditionalMultiXactIdWait((MultiXactId) xwait,
														status, infomask, relation,
														NULL, log_lock_failure))
							ereport(ERROR,
									(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
									 errmsg("could not obtain lock on row in relation \"%s\"",
											RelationGetRelationName(relation))));

						break;
				}

				/*
				 * Of course, the multixact might not be done here: if we're
				 * requesting a light lock mode, other transactions with light
				 * locks could still be alive, as well as locks owned by our
				 * own xact or other subxacts of this backend.  We need to
				 * preserve the surviving MultiXact members.  Note that it
				 * isn't absolutely necessary in the latter case, but doing so
				 * is simpler.
				 */
			}
			else
			{
				/* wait for regular transaction to end, or die trying */
				switch (wait_policy)
				{
					case LockWaitBlock:
						XactLockTableWait(xwait, relation, &tuple->t_self,
										  XLTW_Lock);
						break;
					case LockWaitSkip:
						if (!ConditionalXactLockTableWait(xwait, false))
						{
							result = TM_WouldBlock;
							/* recovery code expects to have buffer lock held */
							LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
							goto failed;
						}
						break;
					case LockWaitError:
						if (!ConditionalXactLockTableWait(xwait, log_lock_failure))
							ereport(ERROR,
									(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
									 errmsg("could not obtain lock on row in relation \"%s\"",
											RelationGetRelationName(relation))));
						break;
				}
			}

			/* if there are updates, follow the update chain */
			if (follow_updates && !HEAP_XMAX_IS_LOCKED_ONLY(infomask))
			{
				TM_Result	res;

				res = heap_lock_updated_tuple(relation, tuple, &t_ctid,
											  GetCurrentTransactionId(),
											  mode);
				if (res != TM_Ok)
				{
					result = res;
					/* recovery code expects to have buffer lock held */
					LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
					goto failed;
				}
			}

			LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);

			/*
			 * xwait is done, but if xwait had just locked the tuple then some
			 * other xact could update this tuple before we get to this point.
			 * Check for xmax change, and start over if so.
			 */
			if (xmax_infomask_changed(tuple->t_data->t_infomask, infomask) ||
				!TransactionIdEquals(HeapTupleHeaderGetRawXmax(tuple->t_data),
									 xwait))
				goto l3;

			if (!(infomask & HEAP_XMAX_IS_MULTI))
			{
				/*
				 * Otherwise check if it committed or aborted.  Note we cannot
				 * be here if the tuple was only locked by somebody who didn't
				 * conflict with us; that would have been handled above.  So
				 * that transaction must necessarily be gone by now.  But
				 * don't check for this in the multixact case, because some
				 * locker transactions might still be running.
				 */
				UpdateXmaxHintBits(tuple->t_data, *buffer, xwait);
			}
		}

		/* By here, we're certain that we hold buffer exclusive lock again */

		/*
		 * We may lock if previous xmax aborted, or if it committed but only
		 * locked the tuple without updating it; or if we didn't have to wait
		 * at all for whatever reason.
		 */
		if (!require_sleep ||
			(tuple->t_data->t_infomask & HEAP_XMAX_INVALID) ||
			HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_data->t_infomask) ||
			HeapTupleHeaderIsOnlyLocked(tuple->t_data))
			result = TM_Ok;
		else if (!ItemPointerEquals(&tuple->t_self, &tuple->t_data->t_ctid))
			result = TM_Updated;
		else
			result = TM_Deleted;
	}

failed:
	if (result != TM_Ok)
	{
		Assert(result == TM_SelfModified || result == TM_Updated ||
			   result == TM_Deleted || result == TM_WouldBlock);

		/*
		 * When locking a tuple under LockWaitSkip semantics and we fail with
		 * TM_WouldBlock above, it's possible for concurrent transactions to
		 * release the lock and set HEAP_XMAX_INVALID in the meantime.  So
		 * this assert is slightly different from the equivalent one in
		 * heap_delete and heap_update.
		 */
		Assert((result == TM_WouldBlock) ||
			   !(tuple->t_data->t_infomask & HEAP_XMAX_INVALID));
		Assert(result != TM_Updated ||
			   !ItemPointerEquals(&tuple->t_self, &tuple->t_data->t_ctid));
		tmfd->ctid = tuple->t_data->t_ctid;
		tmfd->xmax = HeapTupleHeaderGetUpdateXid(tuple->t_data);
		if (result == TM_SelfModified)
			tmfd->cmax = HeapTupleHeaderGetCmax(tuple->t_data);
		else
			tmfd->cmax = InvalidCommandId;
		goto out_locked;
	}

	/*
	 * If we didn't pin the visibility map page and the page has become all
	 * visible while we were busy locking the buffer, or during some
	 * subsequent window during which we had it unlocked, we'll have to unlock
	 * and re-lock, to avoid holding the buffer lock across I/O.  That's a bit
	 * unfortunate, especially since we'll now have to recheck whether the
	 * tuple has been locked or updated under us, but hopefully it won't
	 * happen very often.
	 */
	if (vmbuffer == InvalidBuffer && PageIsAllVisible(page))
	{
		LockBuffer(*buffer, BUFFER_LOCK_UNLOCK);
		visibilitymap_pin(relation, block, &vmbuffer);
		LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
		goto l3;
	}

	xmax = HeapTupleHeaderGetRawXmax(tuple->t_data);
	old_infomask = tuple->t_data->t_infomask;

	/*
	 * If this is the first possibly-multixact-able operation in the current
	 * transaction, set my per-backend OldestMemberMXactId setting. We can be
	 * certain that the transaction will never become a member of any older
	 * MultiXactIds than that.  (We have to do this even if we end up just
	 * using our own TransactionId below, since some other backend could
	 * incorporate our XID into a MultiXact immediately afterwards.)
	 */
	MultiXactIdSetOldestMember();

	/*
	 * Compute the new xmax and infomask to store into the tuple.  Note we do
	 * not modify the tuple just yet, because that would leave it in the wrong
	 * state if multixact.c elogs.
	 */
	compute_new_xmax_infomask(xmax, old_infomask, tuple->t_data->t_infomask2,
							  GetCurrentTransactionId(), mode, false,
							  &xid, &new_infomask, &new_infomask2);

	START_CRIT_SECTION();

	/*
	 * Store transaction information of xact locking the tuple.
	 *
	 * Note: Cmax is meaningless in this context, so don't set it; this avoids
	 * possibly generating a useless combo CID.  Moreover, if we're locking a
	 * previously updated tuple, it's important to preserve the Cmax.
	 *
	 * Also reset the HOT UPDATE bit, but only if there's no update; otherwise
	 * we would break the HOT chain.
	 */
	tuple->t_data->t_infomask &= ~HEAP_XMAX_BITS;
	tuple->t_data->t_infomask2 &= ~HEAP_KEYS_UPDATED;
	tuple->t_data->t_infomask |= new_infomask;
	tuple->t_data->t_infomask2 |= new_infomask2;
	if (HEAP_XMAX_IS_LOCKED_ONLY(new_infomask))
		HeapTupleHeaderClearHotUpdated(tuple->t_data);
	HeapTupleHeaderSetXmax(tuple->t_data, xid);

	/*
	 * Make sure there is no forward chain link in t_ctid.  Note that in the
	 * cases where the tuple has been updated, we must not overwrite t_ctid,
	 * because it was set by the updater.  Moreover, if the tuple has been
	 * updated, we need to follow the update chain to lock the new versions of
	 * the tuple as well.
	 */
	if (HEAP_XMAX_IS_LOCKED_ONLY(new_infomask))
		tuple->t_data->t_ctid = *tid;

	/* Clear only the all-frozen bit on visibility map if needed */
	if (PageIsAllVisible(page) &&
		visibilitymap_clear(relation, block, vmbuffer,
							VISIBILITYMAP_ALL_FROZEN))
		cleared_all_frozen = true;


	MarkBufferDirty(*buffer);

	/*
	 * XLOG stuff.  You might think that we don't need an XLOG record because
	 * there is no state change worth restoring after a crash.  You would be
	 * wrong however: we have just written either a TransactionId or a
	 * MultiXactId that may never have been seen on disk before, and we need
	 * to make sure that there are XLOG entries covering those ID numbers.
	 * Else the same IDs might be re-used after a crash, which would be
	 * disastrous if this page made it to disk before the crash.  Essentially
	 * we have to enforce the WAL log-before-data rule even in this case.
	 * (Also, in a PITR log-shipping or 2PC environment, we have to have XLOG
	 * entries for everything anyway.)
	 */
	if (RelationNeedsWAL(relation))
	{
		xl_heap_lock xlrec;
		XLogRecPtr	recptr;

		XLogBeginInsert();
		XLogRegisterBuffer(0, *buffer, REGBUF_STANDARD);

		xlrec.offnum = ItemPointerGetOffsetNumber(&tuple->t_self);
		xlrec.xmax = xid;
		xlrec.infobits_set = compute_infobits(new_infomask,
											  tuple->t_data->t_infomask2);
		xlrec.flags = cleared_all_frozen ? XLH_LOCK_ALL_FROZEN_CLEARED : 0;
		XLogRegisterData(&xlrec, SizeOfHeapLock);

		/* we don't decode row locks atm, so no need to log the origin */

		recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_LOCK);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	result = TM_Ok;

out_locked:
	LockBuffer(*buffer, BUFFER_LOCK_UNLOCK);

out_unlocked:
	if (BufferIsValid(vmbuffer))
		ReleaseBuffer(vmbuffer);

	/*
	 * Don't update the visibility map here. Locking a tuple doesn't change
	 * visibility info.
	 */

	/*
	 * Now that we have successfully marked the tuple as locked, we can
	 * release the lmgr tuple lock, if we had it.
	 */
	if (have_tuple_lock)
		UnlockTupleTuplock(relation, tid, mode);

	return result;
}

/*
 * Acquire heavyweight lock on the given tuple, in preparation for acquiring
 * its normal, Xmax-based tuple lock.
 *
 * have_tuple_lock is an input and output parameter: on input, it indicates
 * whether the lock has previously been acquired (and this function does
 * nothing in that case).  If this function returns success, have_tuple_lock
 * has been flipped to true.
 *
 * Returns false if it was unable to obtain the lock; this can only happen if
 * wait_policy is Skip.
 */
static bool
heap_acquire_tuplock(Relation relation, ItemPointer tid, LockTupleMode mode,
					 LockWaitPolicy wait_policy, bool *have_tuple_lock)
{
	if (*have_tuple_lock)
		return true;

	switch (wait_policy)
	{
		case LockWaitBlock:
			LockTupleTuplock(relation, tid, mode);
			break;

		case LockWaitSkip:
			if (!ConditionalLockTupleTuplock(relation, tid, mode, false))
				return false;
			break;

		case LockWaitError:
			if (!ConditionalLockTupleTuplock(relation, tid, mode, log_lock_failure))
				ereport(ERROR,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("could not obtain lock on row in relation \"%s\"",
								RelationGetRelationName(relation))));
			break;
	}
	*have_tuple_lock = true;

	return true;
}

/*
 * Given an original set of Xmax and infomask, and a transaction (identified by
 * add_to_xmax) acquiring a new lock of some mode, compute the new Xmax and
 * corresponding infomasks to use on the tuple.
 *
 * Note that this might have side effects such as creating a new MultiXactId.
 *
 * Most callers will have called HeapTupleSatisfiesUpdate before this function;
 * that will have set the HEAP_XMAX_INVALID bit if the xmax was a MultiXactId
 * but it was not running anymore. There is a race condition, which is that the
 * MultiXactId may have finished since then, but that uncommon case is handled
 * either here, or within MultiXactIdExpand.
 *
 * There is a similar race condition possible when the old xmax was a regular
 * TransactionId.  We test TransactionIdIsInProgress again just to narrow the
 * window, but it's still possible to end up creating an unnecessary
 * MultiXactId.  Fortunately this is harmless.
 */
static void
compute_new_xmax_infomask(TransactionId xmax, uint16 old_infomask,
						  uint16 old_infomask2, TransactionId add_to_xmax,
						  LockTupleMode mode, bool is_update,
						  TransactionId *result_xmax, uint16 *result_infomask,
						  uint16 *result_infomask2)
{
	TransactionId new_xmax;
	uint16		new_infomask,
				new_infomask2;

	Assert(TransactionIdIsCurrentTransactionId(add_to_xmax));

l5:
	new_infomask = 0;
	new_infomask2 = 0;
	if (old_infomask & HEAP_XMAX_INVALID)
	{
		/*
		 * No previous locker; we just insert our own TransactionId.
		 *
		 * Note that it's critical that this case be the first one checked,
		 * because there are several blocks below that come back to this one
		 * to implement certain optimizations; old_infomask might contain
		 * other dirty bits in those cases, but we don't really care.
		 */
		if (is_update)
		{
			new_xmax = add_to_xmax;
			if (mode == LockTupleExclusive)
				new_infomask2 |= HEAP_KEYS_UPDATED;
		}
		else
		{
			new_infomask |= HEAP_XMAX_LOCK_ONLY;
			switch (mode)
			{
				case LockTupleKeyShare:
					new_xmax = add_to_xmax;
					new_infomask |= HEAP_XMAX_KEYSHR_LOCK;
					break;
				case LockTupleShare:
					new_xmax = add_to_xmax;
					new_infomask |= HEAP_XMAX_SHR_LOCK;
					break;
				case LockTupleNoKeyExclusive:
					new_xmax = add_to_xmax;
					new_infomask |= HEAP_XMAX_EXCL_LOCK;
					break;
				case LockTupleExclusive:
					new_xmax = add_to_xmax;
					new_infomask |= HEAP_XMAX_EXCL_LOCK;
					new_infomask2 |= HEAP_KEYS_UPDATED;
					break;
				default:
					new_xmax = InvalidTransactionId;	/* silence compiler */
					elog(ERROR, "invalid lock mode");
			}
		}
	}
	else if (old_infomask & HEAP_XMAX_IS_MULTI)
	{
		MultiXactStatus new_status;

		/*
		 * Currently we don't allow XMAX_COMMITTED to be set for multis, so
		 * cross-check.
		 */
		Assert(!(old_infomask & HEAP_XMAX_COMMITTED));

		/*
		 * A multixact together with LOCK_ONLY set but neither lock bit set
		 * (i.e. a pg_upgraded share locked tuple) cannot possibly be running
		 * anymore.  This check is critical for databases upgraded by
		 * pg_upgrade; both MultiXactIdIsRunning and MultiXactIdExpand assume
		 * that such multis are never passed.
		 */
		if (HEAP_LOCKED_UPGRADED(old_infomask))
		{
			old_infomask &= ~HEAP_XMAX_IS_MULTI;
			old_infomask |= HEAP_XMAX_INVALID;
			goto l5;
		}

		/*
		 * If the XMAX is already a MultiXactId, then we need to expand it to
		 * include add_to_xmax; but if all the members were lockers and are
		 * all gone, we can do away with the IS_MULTI bit and just set
		 * add_to_xmax as the only locker/updater.  If all lockers are gone
		 * and we have an updater that aborted, we can also do without a
		 * multi.
		 *
		 * The cost of doing GetMultiXactIdMembers would be paid by
		 * MultiXactIdExpand if we weren't to do this, so this check is not
		 * incurring extra work anyhow.
		 */
		if (!MultiXactIdIsRunning(xmax, HEAP_XMAX_IS_LOCKED_ONLY(old_infomask)))
		{
			if (HEAP_XMAX_IS_LOCKED_ONLY(old_infomask) ||
				!TransactionIdDidCommit(MultiXactIdGetUpdateXid(xmax,
																old_infomask)))
			{
				/*
				 * Reset these bits and restart; otherwise fall through to
				 * create a new multi below.
				 */
				old_infomask &= ~HEAP_XMAX_IS_MULTI;
				old_infomask |= HEAP_XMAX_INVALID;
				goto l5;
			}
		}

		new_status = get_mxact_status_for_lock(mode, is_update);

		new_xmax = MultiXactIdExpand((MultiXactId) xmax, add_to_xmax,
									 new_status);
		GetMultiXactIdHintBits(new_xmax, &new_infomask, &new_infomask2);
	}
	else if (old_infomask & HEAP_XMAX_COMMITTED)
	{
		/*
		 * It's a committed update, so we need to preserve him as updater of
		 * the tuple.
		 */
		MultiXactStatus status;
		MultiXactStatus new_status;

		if (old_infomask2 & HEAP_KEYS_UPDATED)
			status = MultiXactStatusUpdate;
		else
			status = MultiXactStatusNoKeyUpdate;

		new_status = get_mxact_status_for_lock(mode, is_update);

		/*
		 * since it's not running, it's obviously impossible for the old
		 * updater to be identical to the current one, so we need not check
		 * for that case as we do in the block above.
		 */
		new_xmax = MultiXactIdCreate(xmax, status, add_to_xmax, new_status);
		GetMultiXactIdHintBits(new_xmax, &new_infomask, &new_infomask2);
	}
	else if (TransactionIdIsInProgress(xmax))
	{
		/*
		 * If the XMAX is a valid, in-progress TransactionId, then we need to
		 * create a new MultiXactId that includes both the old locker or
		 * updater and our own TransactionId.
		 */
		MultiXactStatus new_status;
		MultiXactStatus old_status;
		LockTupleMode old_mode;

		if (HEAP_XMAX_IS_LOCKED_ONLY(old_infomask))
		{
			if (HEAP_XMAX_IS_KEYSHR_LOCKED(old_infomask))
				old_status = MultiXactStatusForKeyShare;
			else if (HEAP_XMAX_IS_SHR_LOCKED(old_infomask))
				old_status = MultiXactStatusForShare;
			else if (HEAP_XMAX_IS_EXCL_LOCKED(old_infomask))
			{
				if (old_infomask2 & HEAP_KEYS_UPDATED)
					old_status = MultiXactStatusForUpdate;
				else
					old_status = MultiXactStatusForNoKeyUpdate;
			}
			else
			{
				/*
				 * LOCK_ONLY can be present alone only when a page has been
				 * upgraded by pg_upgrade.  But in that case,
				 * TransactionIdIsInProgress() should have returned false.  We
				 * assume it's no longer locked in this case.
				 */
				elog(WARNING, "LOCK_ONLY found for Xid in progress %u", xmax);
				old_infomask |= HEAP_XMAX_INVALID;
				old_infomask &= ~HEAP_XMAX_LOCK_ONLY;
				goto l5;
			}
		}
		else
		{
			/* it's an update, but which kind? */
			if (old_infomask2 & HEAP_KEYS_UPDATED)
				old_status = MultiXactStatusUpdate;
			else
				old_status = MultiXactStatusNoKeyUpdate;
		}

		old_mode = TUPLOCK_from_mxstatus(old_status);

		/*
		 * If the lock to be acquired is for the same TransactionId as the
		 * existing lock, there's an optimization possible: consider only the
		 * strongest of both locks as the only one present, and restart.
		 */
		if (xmax == add_to_xmax)
		{
			/*
			 * Note that it's not possible for the original tuple to be
			 * updated: we wouldn't be here because the tuple would have been
			 * invisible and we wouldn't try to update it.  As a subtlety,
			 * this code can also run when traversing an update chain to lock
			 * future versions of a tuple.  But we wouldn't be here either,
			 * because the add_to_xmax would be different from the original
			 * updater.
			 */
			Assert(HEAP_XMAX_IS_LOCKED_ONLY(old_infomask));

			/* acquire the strongest of both */
			if (mode < old_mode)
				mode = old_mode;
			/* mustn't touch is_update */

			old_infomask |= HEAP_XMAX_INVALID;
			goto l5;
		}

		/* otherwise, just fall back to creating a new multixact */
		new_status = get_mxact_status_for_lock(mode, is_update);
		new_xmax = MultiXactIdCreate(xmax, old_status,
									 add_to_xmax, new_status);
		GetMultiXactIdHintBits(new_xmax, &new_infomask, &new_infomask2);
	}
	else if (!HEAP_XMAX_IS_LOCKED_ONLY(old_infomask) &&
			 TransactionIdDidCommit(xmax))
	{
		/*
		 * It's a committed update, so we gotta preserve him as updater of the
		 * tuple.
		 */
		MultiXactStatus status;
		MultiXactStatus new_status;

		if (old_infomask2 & HEAP_KEYS_UPDATED)
			status = MultiXactStatusUpdate;
		else
			status = MultiXactStatusNoKeyUpdate;

		new_status = get_mxact_status_for_lock(mode, is_update);

		/*
		 * since it's not running, it's obviously impossible for the old
		 * updater to be identical to the current one, so we need not check
		 * for that case as we do in the block above.
		 */
		new_xmax = MultiXactIdCreate(xmax, status, add_to_xmax, new_status);
		GetMultiXactIdHintBits(new_xmax, &new_infomask, &new_infomask2);
	}
	else
	{
		/*
		 * Can get here iff the locking/updating transaction was running when
		 * the infomask was extracted from the tuple, but finished before
		 * TransactionIdIsInProgress got to run.  Deal with it as if there was
		 * no locker at all in the first place.
		 */
		old_infomask |= HEAP_XMAX_INVALID;
		goto l5;
	}

	*result_infomask = new_infomask;
	*result_infomask2 = new_infomask2;
	*result_xmax = new_xmax;
}

/*
 * Subroutine for heap_lock_updated_tuple_rec.
 *
 * Given a hypothetical multixact status held by the transaction identified
 * with the given xid, does the current transaction need to wait, fail, or can
 * it continue if it wanted to acquire a lock of the given mode?  "needwait"
 * is set to true if waiting is necessary; if it can continue, then TM_Ok is
 * returned.  If the lock is already held by the current transaction, return
 * TM_SelfModified.  In case of a conflict with another transaction, a
 * different HeapTupleSatisfiesUpdate return code is returned.
 *
 * The held status is said to be hypothetical because it might correspond to a
 * lock held by a single Xid, i.e. not a real MultiXactId; we express it this
 * way for simplicity of API.
 */
static TM_Result
test_lockmode_for_conflict(MultiXactStatus status, TransactionId xid,
						   LockTupleMode mode, HeapTuple tup,
						   bool *needwait)
{
	MultiXactStatus wantedstatus;

	*needwait = false;
	wantedstatus = get_mxact_status_for_lock(mode, false);

	/*
	 * Note: we *must* check TransactionIdIsInProgress before
	 * TransactionIdDidAbort/Commit; see comment at top of heapam_visibility.c
	 * for an explanation.
	 */
	if (TransactionIdIsCurrentTransactionId(xid))
	{
		/*
		 * The tuple has already been locked by our own transaction.  This is
		 * very rare but can happen if multiple transactions are trying to
		 * lock an ancient version of the same tuple.
		 */
		return TM_SelfModified;
	}
	else if (TransactionIdIsInProgress(xid))
	{
		/*
		 * If the locking transaction is running, what we do depends on
		 * whether the lock modes conflict: if they do, then we must wait for
		 * it to finish; otherwise we can fall through to lock this tuple
		 * version without waiting.
		 */
		if (DoLockModesConflict(LOCKMODE_from_mxstatus(status),
								LOCKMODE_from_mxstatus(wantedstatus)))
		{
			*needwait = true;
		}

		/*
		 * If we set needwait above, then this value doesn't matter;
		 * otherwise, this value signals to caller that it's okay to proceed.
		 */
		return TM_Ok;
	}
	else if (TransactionIdDidAbort(xid))
		return TM_Ok;
	else if (TransactionIdDidCommit(xid))
	{
		/*
		 * The other transaction committed.  If it was only a locker, then the
		 * lock is completely gone now and we can return success; but if it
		 * was an update, then what we do depends on whether the two lock
		 * modes conflict.  If they conflict, then we must report error to
		 * caller. But if they don't, we can fall through to allow the current
		 * transaction to lock the tuple.
		 *
		 * Note: the reason we worry about ISUPDATE here is because as soon as
		 * a transaction ends, all its locks are gone and meaningless, and
		 * thus we can ignore them; whereas its updates persist.  In the
		 * TransactionIdIsInProgress case, above, we don't need to check
		 * because we know the lock is still "alive" and thus a conflict needs
		 * always be checked.
		 */
		if (!ISUPDATE_from_mxstatus(status))
			return TM_Ok;

		if (DoLockModesConflict(LOCKMODE_from_mxstatus(status),
								LOCKMODE_from_mxstatus(wantedstatus)))
		{
			/* bummer */
			if (!ItemPointerEquals(&tup->t_self, &tup->t_data->t_ctid))
				return TM_Updated;
			else
				return TM_Deleted;
		}

		return TM_Ok;
	}

	/* Not in progress, not aborted, not committed -- must have crashed */
	return TM_Ok;
}


/*
 * Recursive part of heap_lock_updated_tuple
 *
 * Fetch the tuple pointed to by tid in rel, and mark it as locked by the given
 * xid with the given mode; if this tuple is updated, recurse to lock the new
 * version as well.
 */
static TM_Result
heap_lock_updated_tuple_rec(Relation rel, ItemPointer tid, TransactionId xid,
							LockTupleMode mode)
{
	TM_Result	result;
	ItemPointerData tupid;
	HeapTupleData mytup;
	Buffer		buf;
	uint16		new_infomask,
				new_infomask2,
				old_infomask,
				old_infomask2;
	TransactionId xmax,
				new_xmax;
	TransactionId priorXmax = InvalidTransactionId;
	bool		cleared_all_frozen = false;
	bool		pinned_desired_page;
	Buffer		vmbuffer = InvalidBuffer;
	BlockNumber block;

	ItemPointerCopy(tid, &tupid);

	for (;;)
	{
		new_infomask = 0;
		new_xmax = InvalidTransactionId;
		block = ItemPointerGetBlockNumber(&tupid);
		ItemPointerCopy(&tupid, &(mytup.t_self));

		if (!heap_fetch(rel, SnapshotAny, &mytup, &buf, false))
		{
			/*
			 * if we fail to find the updated version of the tuple, it's
			 * because it was vacuumed/pruned away after its creator
			 * transaction aborted.  So behave as if we got to the end of the
			 * chain, and there's no further tuple to lock: return success to
			 * caller.
			 */
			result = TM_Ok;
			goto out_unlocked;
		}

l4:
		CHECK_FOR_INTERRUPTS();

		/*
		 * Before locking the buffer, pin the visibility map page if it
		 * appears to be necessary.  Since we haven't got the lock yet,
		 * someone else might be in the middle of changing this, so we'll need
		 * to recheck after we have the lock.
		 */
		if (PageIsAllVisible(BufferGetPage(buf)))
		{
			visibilitymap_pin(rel, block, &vmbuffer);
			pinned_desired_page = true;
		}
		else
			pinned_desired_page = false;

		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

		/*
		 * If we didn't pin the visibility map page and the page has become
		 * all visible while we were busy locking the buffer, we'll have to
		 * unlock and re-lock, to avoid holding the buffer lock across I/O.
		 * That's a bit unfortunate, but hopefully shouldn't happen often.
		 *
		 * Note: in some paths through this function, we will reach here
		 * holding a pin on a vm page that may or may not be the one matching
		 * this page.  If this page isn't all-visible, we won't use the vm
		 * page, but we hold onto such a pin till the end of the function.
		 */
		if (!pinned_desired_page && PageIsAllVisible(BufferGetPage(buf)))
		{
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			visibilitymap_pin(rel, block, &vmbuffer);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		}

		/*
		 * Check the tuple XMIN against prior XMAX, if any.  If we reached the
		 * end of the chain, we're done, so return success.
		 */
		if (TransactionIdIsValid(priorXmax) &&
			!TransactionIdEquals(HeapTupleHeaderGetXmin(mytup.t_data),
								 priorXmax))
		{
			result = TM_Ok;
			goto out_locked;
		}

		/*
		 * Also check Xmin: if this tuple was created by an aborted
		 * (sub)transaction, then we already locked the last live one in the
		 * chain, thus we're done, so return success.
		 */
		if (TransactionIdDidAbort(HeapTupleHeaderGetXmin(mytup.t_data)))
		{
			result = TM_Ok;
			goto out_locked;
		}

		old_infomask = mytup.t_data->t_infomask;
		old_infomask2 = mytup.t_data->t_infomask2;
		xmax = HeapTupleHeaderGetRawXmax(mytup.t_data);

		/*
		 * If this tuple version has been updated or locked by some concurrent
		 * transaction(s), what we do depends on whether our lock mode
		 * conflicts with what those other transactions hold, and also on the
		 * status of them.
		 */
		if (!(old_infomask & HEAP_XMAX_INVALID))
		{
			TransactionId rawxmax;
			bool		needwait;

			rawxmax = HeapTupleHeaderGetRawXmax(mytup.t_data);
			if (old_infomask & HEAP_XMAX_IS_MULTI)
			{
				int			nmembers;
				int			i;
				MultiXactMember *members;

				/*
				 * We don't need a test for pg_upgrade'd tuples: this is only
				 * applied to tuples after the first in an update chain.  Said
				 * first tuple in the chain may well be locked-in-9.2-and-
				 * pg_upgraded, but that one was already locked by our caller,
				 * not us; and any subsequent ones cannot be because our
				 * caller must necessarily have obtained a snapshot later than
				 * the pg_upgrade itself.
				 */
				Assert(!HEAP_LOCKED_UPGRADED(mytup.t_data->t_infomask));

				nmembers = GetMultiXactIdMembers(rawxmax, &members, false,
												 HEAP_XMAX_IS_LOCKED_ONLY(old_infomask));
				for (i = 0; i < nmembers; i++)
				{
					result = test_lockmode_for_conflict(members[i].status,
														members[i].xid,
														mode,
														&mytup,
														&needwait);

					/*
					 * If the tuple was already locked by ourselves in a
					 * previous iteration of this (say heap_lock_tuple was
					 * forced to restart the locking loop because of a change
					 * in xmax), then we hold the lock already on this tuple
					 * version and we don't need to do anything; and this is
					 * not an error condition either.  We just need to skip
					 * this tuple and continue locking the next version in the
					 * update chain.
					 */
					if (result == TM_SelfModified)
					{
						pfree(members);
						goto next;
					}

					if (needwait)
					{
						LockBuffer(buf, BUFFER_LOCK_UNLOCK);
						XactLockTableWait(members[i].xid, rel,
										  &mytup.t_self,
										  XLTW_LockUpdated);
						pfree(members);
						goto l4;
					}
					if (result != TM_Ok)
					{
						pfree(members);
						goto out_locked;
					}
				}
				if (members)
					pfree(members);
			}
			else
			{
				MultiXactStatus status;

				/*
				 * For a non-multi Xmax, we first need to compute the
				 * corresponding MultiXactStatus by using the infomask bits.
				 */
				if (HEAP_XMAX_IS_LOCKED_ONLY(old_infomask))
				{
					if (HEAP_XMAX_IS_KEYSHR_LOCKED(old_infomask))
						status = MultiXactStatusForKeyShare;
					else if (HEAP_XMAX_IS_SHR_LOCKED(old_infomask))
						status = MultiXactStatusForShare;
					else if (HEAP_XMAX_IS_EXCL_LOCKED(old_infomask))
					{
						if (old_infomask2 & HEAP_KEYS_UPDATED)
							status = MultiXactStatusForUpdate;
						else
							status = MultiXactStatusForNoKeyUpdate;
					}
					else
					{
						/*
						 * LOCK_ONLY present alone (a pg_upgraded tuple marked
						 * as share-locked in the old cluster) shouldn't be
						 * seen in the middle of an update chain.
						 */
						elog(ERROR, "invalid lock status in tuple");
					}
				}
				else
				{
					/* it's an update, but which kind? */
					if (old_infomask2 & HEAP_KEYS_UPDATED)
						status = MultiXactStatusUpdate;
					else
						status = MultiXactStatusNoKeyUpdate;
				}

				result = test_lockmode_for_conflict(status, rawxmax, mode,
													&mytup, &needwait);

				/*
				 * If the tuple was already locked by ourselves in a previous
				 * iteration of this (say heap_lock_tuple was forced to
				 * restart the locking loop because of a change in xmax), then
				 * we hold the lock already on this tuple version and we don't
				 * need to do anything; and this is not an error condition
				 * either.  We just need to skip this tuple and continue
				 * locking the next version in the update chain.
				 */
				if (result == TM_SelfModified)
					goto next;

				if (needwait)
				{
					LockBuffer(buf, BUFFER_LOCK_UNLOCK);
					XactLockTableWait(rawxmax, rel, &mytup.t_self,
									  XLTW_LockUpdated);
					goto l4;
				}
				if (result != TM_Ok)
				{
					goto out_locked;
				}
			}
		}

		/* compute the new Xmax and infomask values for the tuple ... */
		compute_new_xmax_infomask(xmax, old_infomask, mytup.t_data->t_infomask2,
								  xid, mode, false,
								  &new_xmax, &new_infomask, &new_infomask2);

		if (PageIsAllVisible(BufferGetPage(buf)) &&
			visibilitymap_clear(rel, block, vmbuffer,
								VISIBILITYMAP_ALL_FROZEN))
			cleared_all_frozen = true;

		START_CRIT_SECTION();

		/* ... and set them */
		HeapTupleHeaderSetXmax(mytup.t_data, new_xmax);
		mytup.t_data->t_infomask &= ~HEAP_XMAX_BITS;
		mytup.t_data->t_infomask2 &= ~HEAP_KEYS_UPDATED;
		mytup.t_data->t_infomask |= new_infomask;
		mytup.t_data->t_infomask2 |= new_infomask2;

		MarkBufferDirty(buf);

		/* XLOG stuff */
		if (RelationNeedsWAL(rel))
		{
			xl_heap_lock_updated xlrec;
			XLogRecPtr	recptr;
			Page		page = BufferGetPage(buf);

			XLogBeginInsert();
			XLogRegisterBuffer(0, buf, REGBUF_STANDARD);

			xlrec.offnum = ItemPointerGetOffsetNumber(&mytup.t_self);
			xlrec.xmax = new_xmax;
			xlrec.infobits_set = compute_infobits(new_infomask, new_infomask2);
			xlrec.flags =
				cleared_all_frozen ? XLH_LOCK_ALL_FROZEN_CLEARED : 0;

			XLogRegisterData(&xlrec, SizeOfHeapLockUpdated);

			recptr = XLogInsert(RM_HEAP2_ID, XLOG_HEAP2_LOCK_UPDATED);

			PageSetLSN(page, recptr);
		}

		END_CRIT_SECTION();

next:
		/* if we find the end of update chain, we're done. */
		if (mytup.t_data->t_infomask & HEAP_XMAX_INVALID ||
			HeapTupleHeaderIndicatesMovedPartitions(mytup.t_data) ||
			ItemPointerEquals(&mytup.t_self, &mytup.t_data->t_ctid) ||
			HeapTupleHeaderIsOnlyLocked(mytup.t_data))
		{
			result = TM_Ok;
			goto out_locked;
		}

		/* tail recursion */
		priorXmax = HeapTupleHeaderGetUpdateXid(mytup.t_data);
		ItemPointerCopy(&(mytup.t_data->t_ctid), &tupid);
		UnlockReleaseBuffer(buf);
	}

	result = TM_Ok;

out_locked:
	UnlockReleaseBuffer(buf);

out_unlocked:
	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);

	return result;
}

/*
 * heap_lock_updated_tuple
 *		Follow update chain when locking an updated tuple, acquiring locks (row
 *		marks) on the updated versions.
 *
 * The initial tuple is assumed to be already locked.
 *
 * This function doesn't check visibility, it just unconditionally marks the
 * tuple(s) as locked.  If any tuple in the updated chain is being deleted
 * concurrently (or updated with the key being modified), sleep until the
 * transaction doing it is finished.
 *
 * Note that we don't acquire heavyweight tuple locks on the tuples we walk
 * when we have to wait for other transactions to release them, as opposed to
 * what heap_lock_tuple does.  The reason is that having more than one
 * transaction walking the chain is probably uncommon enough that risk of
 * starvation is not likely: one of the preconditions for being here is that
 * the snapshot in use predates the update that created this tuple (because we
 * started at an earlier version of the tuple), but at the same time such a
 * transaction cannot be using repeatable read or serializable isolation
 * levels, because that would lead to a serializability failure.
 */
static TM_Result
heap_lock_updated_tuple(Relation rel, HeapTuple tuple, ItemPointer ctid,
						TransactionId xid, LockTupleMode mode)
{
	/*
	 * If the tuple has not been updated, or has moved into another partition
	 * (effectively a delete) stop here.
	 */
	if (!HeapTupleHeaderIndicatesMovedPartitions(tuple->t_data) &&
		!ItemPointerEquals(&tuple->t_self, ctid))
	{
		/*
		 * If this is the first possibly-multixact-able operation in the
		 * current transaction, set my per-backend OldestMemberMXactId
		 * setting. We can be certain that the transaction will never become a
		 * member of any older MultiXactIds than that.  (We have to do this
		 * even if we end up just using our own TransactionId below, since
		 * some other backend could incorporate our XID into a MultiXact
		 * immediately afterwards.)
		 */
		MultiXactIdSetOldestMember();

		return heap_lock_updated_tuple_rec(rel, ctid, xid, mode);
	}

	/* nothing to lock */
	return TM_Ok;
}

/*
 *	heap_finish_speculative - mark speculative insertion as successful
 *
 * To successfully finish a speculative insertion we have to clear speculative
 * token from tuple.  To do so the t_ctid field, which will contain a
 * speculative token value, is modified in place to point to the tuple itself,
 * which is characteristic of a newly inserted ordinary tuple.
 *
 * NB: It is not ok to commit without either finishing or aborting a
 * speculative insertion.  We could treat speculative tuples of committed
 * transactions implicitly as completed, but then we would have to be prepared
 * to deal with speculative tokens on committed tuples.  That wouldn't be
 * difficult - no-one looks at the ctid field of a tuple with invalid xmax -
 * but clearing the token at completion isn't very expensive either.
 * An explicit confirmation WAL record also makes logical decoding simpler.
 */
void
heap_finish_speculative(Relation relation, ItemPointer tid)
{
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	ItemId		lp = NULL;
	HeapTupleHeader htup;

	buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	page = (Page) BufferGetPage(buffer);

	offnum = ItemPointerGetOffsetNumber(tid);
	if (PageGetMaxOffsetNumber(page) >= offnum)
		lp = PageGetItemId(page, offnum);

	if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsNormal(lp))
		elog(ERROR, "invalid lp");

	htup = (HeapTupleHeader) PageGetItem(page, lp);

	/* NO EREPORT(ERROR) from here till changes are logged */
	START_CRIT_SECTION();

	Assert(HeapTupleHeaderIsSpeculative(htup));

	MarkBufferDirty(buffer);

	/*
	 * Replace the speculative insertion token with a real t_ctid, pointing to
	 * itself like it does on regular tuples.
	 */
	htup->t_ctid = *tid;

	/* XLOG stuff */
	if (RelationNeedsWAL(relation))
	{
		xl_heap_confirm xlrec;
		XLogRecPtr	recptr;

		xlrec.offnum = ItemPointerGetOffsetNumber(tid);

		XLogBeginInsert();

		/* We want the same filtering on this as on a plain insert */
		XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);

		XLogRegisterData(&xlrec, SizeOfHeapConfirm);
		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

		recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_CONFIRM);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	UnlockReleaseBuffer(buffer);
}

/*
 *	heap_abort_speculative - kill a speculatively inserted tuple
 *
 * Marks a tuple that was speculatively inserted in the same command as dead,
 * by setting its xmin as invalid.  That makes it immediately appear as dead
 * to all transactions, including our own.  In particular, it makes
 * HeapTupleSatisfiesDirty() regard the tuple as dead, so that another backend
 * inserting a duplicate key value won't unnecessarily wait for our whole
 * transaction to finish (it'll just wait for our speculative insertion to
 * finish).
 *
 * Killing the tuple prevents "unprincipled deadlocks", which are deadlocks
 * that arise due to a mutual dependency that is not user visible.  By
 * definition, unprincipled deadlocks cannot be prevented by the user
 * reordering lock acquisition in client code, because the implementation level
 * lock acquisitions are not under the user's direct control.  If speculative
 * inserters did not take this precaution, then under high concurrency they
 * could deadlock with each other, which would not be acceptable.
 *
 * This is somewhat redundant with heap_delete, but we prefer to have a
 * dedicated routine with stripped down requirements.  Note that this is also
 * used to delete the TOAST tuples created during speculative insertion.
 *
 * This routine does not affect logical decoding as it only looks at
 * confirmation records.
 */
void
heap_abort_speculative(Relation relation, ItemPointer tid)
{
	TransactionId xid = GetCurrentTransactionId();
	ItemId		lp;
	HeapTupleData tp;
	Page		page;
	BlockNumber block;
	Buffer		buffer;

	Assert(ItemPointerIsValid(tid));

	block = ItemPointerGetBlockNumber(tid);
	buffer = ReadBuffer(relation, block);
	page = BufferGetPage(buffer);

	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	/*
	 * Page can't be all visible, we just inserted into it, and are still
	 * running.
	 */
	Assert(!PageIsAllVisible(page));

	lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));
	Assert(ItemIdIsNormal(lp));

	tp.t_tableOid = RelationGetRelid(relation);
	tp.t_data = (HeapTupleHeader) PageGetItem(page, lp);
	tp.t_len = ItemIdGetLength(lp);
	tp.t_self = *tid;

	/*
	 * Sanity check that the tuple really is a speculatively inserted tuple,
	 * inserted by us.
	 */
	if (tp.t_data->t_choice.t_heap.t_xmin != xid)
		elog(ERROR, "attempted to kill a tuple inserted by another transaction");
	if (!(IsToastRelation(relation) || HeapTupleHeaderIsSpeculative(tp.t_data)))
		elog(ERROR, "attempted to kill a non-speculative tuple");
	Assert(!HeapTupleHeaderIsHeapOnly(tp.t_data));

	/*
	 * No need to check for serializable conflicts here.  There is never a
	 * need for a combo CID, either.  No need to extract replica identity, or
	 * do anything special with infomask bits.
	 */

	START_CRIT_SECTION();

	/*
	 * The tuple will become DEAD immediately.  Flag that this page is a
	 * candidate for pruning by setting xmin to TransactionXmin. While not
	 * immediately prunable, it is the oldest xid we can cheaply determine
	 * that's safe against wraparound / being older than the table's
	 * relfrozenxid.  To defend against the unlikely case of a new relation
	 * having a newer relfrozenxid than our TransactionXmin, use relfrozenxid
	 * if so (vacuum can't subsequently move relfrozenxid to beyond
	 * TransactionXmin, so there's no race here).
	 */
	Assert(TransactionIdIsValid(TransactionXmin));
	{
		TransactionId relfrozenxid = relation->rd_rel->relfrozenxid;
		TransactionId prune_xid;

		if (TransactionIdPrecedes(TransactionXmin, relfrozenxid))
			prune_xid = relfrozenxid;
		else
			prune_xid = TransactionXmin;
		PageSetPrunable(page, prune_xid);
	}

	/* store transaction information of xact deleting the tuple */
	tp.t_data->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
	tp.t_data->t_infomask2 &= ~HEAP_KEYS_UPDATED;

	/*
	 * Set the tuple header xmin to InvalidTransactionId.  This makes the
	 * tuple immediately invisible everyone.  (In particular, to any
	 * transactions waiting on the speculative token, woken up later.)
	 */
	HeapTupleHeaderSetXmin(tp.t_data, InvalidTransactionId);

	/* Clear the speculative insertion token too */
	tp.t_data->t_ctid = tp.t_self;

	MarkBufferDirty(buffer);

	/*
	 * XLOG stuff
	 *
	 * The WAL records generated here match heap_delete().  The same recovery
	 * routines are used.
	 */
	if (RelationNeedsWAL(relation))
	{
		xl_heap_delete xlrec;
		XLogRecPtr	recptr;

		xlrec.flags = XLH_DELETE_IS_SUPER;
		xlrec.infobits_set = compute_infobits(tp.t_data->t_infomask,
											  tp.t_data->t_infomask2);
		xlrec.offnum = ItemPointerGetOffsetNumber(&tp.t_self);
		xlrec.xmax = xid;

		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfHeapDelete);
		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

		/* No replica identity & replication origin logged */

		recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_DELETE);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	if (HeapTupleHasExternal(&tp))
	{
		Assert(!IsToastRelation(relation));
		heap_toast_delete(relation, &tp, true);
	}

	/*
	 * Never need to mark tuple for invalidation, since catalogs don't support
	 * speculative insertion
	 */

	/* Now we can release the buffer */
	ReleaseBuffer(buffer);

	/* count deletion, as we counted the insertion too */
	pgstat_count_heap_delete(relation);
}

/*
 * heap_inplace_lock - protect inplace update from concurrent heap_update()
 *
 * Evaluate whether the tuple's state is compatible with a no-key update.
 * Current transaction rowmarks are fine, as is KEY SHARE from any
 * transaction.  If compatible, return true with the buffer exclusive-locked,
 * and the caller must release that by calling
 * heap_inplace_update_and_unlock(), calling heap_inplace_unlock(), or raising
 * an error.  Otherwise, call release_callback(arg), wait for blocking
 * transactions to end, and return false.
 *
 * Since this is intended for system catalogs and SERIALIZABLE doesn't cover
 * DDL, this doesn't guarantee any particular predicate locking.
 *
 * One could modify this to return true for tuples with delete in progress,
 * All inplace updaters take a lock that conflicts with DROP.  If explicit
 * "DELETE FROM pg_class" is in progress, we'll wait for it like we would an
 * update.
 *
 * Readers of inplace-updated fields expect changes to those fields are
 * durable.  For example, vac_truncate_clog() reads datfrozenxid from
 * pg_database tuples via catalog snapshots.  A future snapshot must not
 * return a lower datfrozenxid for the same database OID (lower in the
 * FullTransactionIdPrecedes() sense).  We achieve that since no update of a
 * tuple can start while we hold a lock on its buffer.  In cases like
 * BEGIN;GRANT;CREATE INDEX;COMMIT we're inplace-updating a tuple visible only
 * to this transaction.  ROLLBACK then is one case where it's okay to lose
 * inplace updates.  (Restoring relhasindex=false on ROLLBACK is fine, since
 * any concurrent CREATE INDEX would have blocked, then inplace-updated the
 * committed tuple.)
 *
 * In principle, we could avoid waiting by overwriting every tuple in the
 * updated tuple chain.  Reader expectations permit updating a tuple only if
 * it's aborted, is the tail of the chain, or we already updated the tuple
 * referenced in its t_ctid.  Hence, we would need to overwrite the tuples in
 * order from tail to head.  That would imply either (a) mutating all tuples
 * in one critical section or (b) accepting a chance of partial completion.
 * Partial completion of a relfrozenxid update would have the weird
 * consequence that the table's next VACUUM could see the table's relfrozenxid
 * move forward between vacuum_get_cutoffs() and finishing.
 */
bool
heap_inplace_lock(Relation relation,
				  HeapTuple oldtup_ptr, Buffer buffer,
				  void (*release_callback) (void *), void *arg)
{
	HeapTupleData oldtup = *oldtup_ptr; /* minimize diff vs. heap_update() */
	TM_Result	result;
	bool		ret;

#ifdef USE_ASSERT_CHECKING
	if (RelationGetRelid(relation) == RelationRelationId)
		check_inplace_rel_lock(oldtup_ptr);
#endif

	Assert(BufferIsValid(buffer));

	/*
	 * Construct shared cache inval if necessary.  Because we pass a tuple
	 * version without our own inplace changes or inplace changes other
	 * sessions complete while we wait for locks, inplace update mustn't
	 * change catcache lookup keys.  But we aren't bothering with index
	 * updates either, so that's true a fortiori.  After LockBuffer(), it
	 * would be too late, because this might reach a
	 * CatalogCacheInitializeCache() that locks "buffer".
	 */
	CacheInvalidateHeapTupleInplace(relation, oldtup_ptr, NULL);

	LockTuple(relation, &oldtup.t_self, InplaceUpdateTupleLock);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	/*----------
	 * Interpret HeapTupleSatisfiesUpdate() like heap_update() does, except:
	 *
	 * - wait unconditionally
	 * - already locked tuple above, since inplace needs that unconditionally
	 * - don't recheck header after wait: simpler to defer to next iteration
	 * - don't try to continue even if the updater aborts: likewise
	 * - no crosscheck
	 */
	result = HeapTupleSatisfiesUpdate(&oldtup, GetCurrentCommandId(false),
									  buffer);

	if (result == TM_Invisible)
	{
		/* no known way this can happen */
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg_internal("attempted to overwrite invisible tuple")));
	}
	else if (result == TM_SelfModified)
	{
		/*
		 * CREATE INDEX might reach this if an expression is silly enough to
		 * call e.g. SELECT ... FROM pg_class FOR SHARE.  C code of other SQL
		 * statements might get here after a heap_update() of the same row, in
		 * the absence of an intervening CommandCounterIncrement().
		 */
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("tuple to be updated was already modified by an operation triggered by the current command")));
	}
	else if (result == TM_BeingModified)
	{
		TransactionId xwait;
		uint16		infomask;

		xwait = HeapTupleHeaderGetRawXmax(oldtup.t_data);
		infomask = oldtup.t_data->t_infomask;

		if (infomask & HEAP_XMAX_IS_MULTI)
		{
			LockTupleMode lockmode = LockTupleNoKeyExclusive;
			MultiXactStatus mxact_status = MultiXactStatusNoKeyUpdate;
			int			remain;

			if (DoesMultiXactIdConflict((MultiXactId) xwait, infomask,
										lockmode, NULL))
			{
				LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
				release_callback(arg);
				ret = false;
				MultiXactIdWait((MultiXactId) xwait, mxact_status, infomask,
								relation, &oldtup.t_self, XLTW_Update,
								&remain);
			}
			else
				ret = true;
		}
		else if (TransactionIdIsCurrentTransactionId(xwait))
			ret = true;
		else if (HEAP_XMAX_IS_KEYSHR_LOCKED(infomask))
			ret = true;
		else
		{
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			release_callback(arg);
			ret = false;
			XactLockTableWait(xwait, relation, &oldtup.t_self,
							  XLTW_Update);
		}
	}
	else
	{
		ret = (result == TM_Ok);
		if (!ret)
		{
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			release_callback(arg);
		}
	}

	/*
	 * GetCatalogSnapshot() relies on invalidation messages to know when to
	 * take a new snapshot.  COMMIT of xwait is responsible for sending the
	 * invalidation.  We're not acquiring heavyweight locks sufficient to
	 * block if not yet sent, so we must take a new snapshot to ensure a later
	 * attempt has a fair chance.  While we don't need this if xwait aborted,
	 * don't bother optimizing that.
	 */
	if (!ret)
	{
		UnlockTuple(relation, &oldtup.t_self, InplaceUpdateTupleLock);
		ForgetInplace_Inval();
		InvalidateCatalogSnapshot();
	}
	return ret;
}

/*
 * heap_inplace_update_and_unlock - core of systable_inplace_update_finish
 *
 * The tuple cannot change size, and therefore its header fields and null
 * bitmap (if any) don't change either.
 *
 * Since we hold LOCKTAG_TUPLE, no updater has a local copy of this tuple.
 */
void
heap_inplace_update_and_unlock(Relation relation,
							   HeapTuple oldtup, HeapTuple tuple,
							   Buffer buffer)
{
	HeapTupleHeader htup = oldtup->t_data;
	uint32		oldlen;
	uint32		newlen;
	char	   *dst;
	char	   *src;
	int			nmsgs = 0;
	SharedInvalidationMessage *invalMessages = NULL;
	bool		RelcacheInitFileInval = false;

	Assert(ItemPointerEquals(&oldtup->t_self, &tuple->t_self));
	oldlen = oldtup->t_len - htup->t_hoff;
	newlen = tuple->t_len - tuple->t_data->t_hoff;
	if (oldlen != newlen || htup->t_hoff != tuple->t_data->t_hoff)
		elog(ERROR, "wrong tuple length");

	dst = (char *) htup + htup->t_hoff;
	src = (char *) tuple->t_data + tuple->t_data->t_hoff;

	/* Like RecordTransactionCommit(), log only if needed */
	if (XLogStandbyInfoActive())
		nmsgs = inplaceGetInvalidationMessages(&invalMessages,
											   &RelcacheInitFileInval);

	/*
	 * Unlink relcache init files as needed.  If unlinking, acquire
	 * RelCacheInitLock until after associated invalidations.  By doing this
	 * in advance, if we checkpoint and then crash between inplace
	 * XLogInsert() and inval, we don't rely on StartupXLOG() ->
	 * RelationCacheInitFileRemove().  That uses elevel==LOG, so replay would
	 * neglect to PANIC on EIO.
	 */
	PreInplace_Inval();

	/*----------
	 * NO EREPORT(ERROR) from here till changes are complete
	 *
	 * Our buffer lock won't stop a reader having already pinned and checked
	 * visibility for this tuple.  Hence, we write WAL first, then mutate the
	 * buffer.  Like in MarkBufferDirtyHint() or RecordTransactionCommit(),
	 * checkpoint delay makes that acceptable.  With the usual order of
	 * changes, a crash after memcpy() and before XLogInsert() could allow
	 * datfrozenxid to overtake relfrozenxid:
	 *
	 * ["D" is a VACUUM (ONLY_DATABASE_STATS)]
	 * ["R" is a VACUUM tbl]
	 * D: vac_update_datfrozenxid() -> systable_beginscan(pg_class)
	 * D: systable_getnext() returns pg_class tuple of tbl
	 * R: memcpy() into pg_class tuple of tbl
	 * D: raise pg_database.datfrozenxid, XLogInsert(), finish
	 * [crash]
	 * [recovery restores datfrozenxid w/o relfrozenxid]
	 *
	 * Like in MarkBufferDirtyHint() subroutine XLogSaveBufferForHint(), copy
	 * the buffer to the stack before logging.  Here, that facilitates a FPI
	 * of the post-mutation block before we accept other sessions seeing it.
	 */
	Assert((MyProc->delayChkptFlags & DELAY_CHKPT_START) == 0);
	START_CRIT_SECTION();
	MyProc->delayChkptFlags |= DELAY_CHKPT_START;

	/* XLOG stuff */
	if (RelationNeedsWAL(relation))
	{
		xl_heap_inplace xlrec;
		PGAlignedBlock copied_buffer;
		char	   *origdata = (char *) BufferGetBlock(buffer);
		Page		page = BufferGetPage(buffer);
		uint16		lower = ((PageHeader) page)->pd_lower;
		uint16		upper = ((PageHeader) page)->pd_upper;
		uintptr_t	dst_offset_in_block;
		RelFileLocator rlocator;
		ForkNumber	forkno;
		BlockNumber blkno;
		XLogRecPtr	recptr;

		xlrec.offnum = ItemPointerGetOffsetNumber(&tuple->t_self);
		xlrec.dbId = MyDatabaseId;
		xlrec.tsId = MyDatabaseTableSpace;
		xlrec.relcacheInitFileInval = RelcacheInitFileInval;
		xlrec.nmsgs = nmsgs;

		XLogBeginInsert();
		XLogRegisterData(&xlrec, MinSizeOfHeapInplace);
		if (nmsgs != 0)
			XLogRegisterData(invalMessages,
							 nmsgs * sizeof(SharedInvalidationMessage));

		/* register block matching what buffer will look like after changes */
		memcpy(copied_buffer.data, origdata, lower);
		memcpy(copied_buffer.data + upper, origdata + upper, BLCKSZ - upper);
		dst_offset_in_block = dst - origdata;
		memcpy(copied_buffer.data + dst_offset_in_block, src, newlen);
		BufferGetTag(buffer, &rlocator, &forkno, &blkno);
		Assert(forkno == MAIN_FORKNUM);
		XLogRegisterBlock(0, &rlocator, forkno, blkno, copied_buffer.data,
						  REGBUF_STANDARD);
		XLogRegisterBufData(0, src, newlen);

		/* inplace updates aren't decoded atm, don't log the origin */

		recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_INPLACE);

		PageSetLSN(page, recptr);
	}

	memcpy(dst, src, newlen);

	MarkBufferDirty(buffer);

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	/*
	 * Send invalidations to shared queue.  SearchSysCacheLocked1() assumes we
	 * do this before UnlockTuple().
	 *
	 * If we're mutating a tuple visible only to this transaction, there's an
	 * equivalent transactional inval from the action that created the tuple,
	 * and this inval is superfluous.
	 */
	AtInplace_Inval();

	MyProc->delayChkptFlags &= ~DELAY_CHKPT_START;
	END_CRIT_SECTION();
	UnlockTuple(relation, &tuple->t_self, InplaceUpdateTupleLock);

	AcceptInvalidationMessages();	/* local processing of just-sent inval */

	/*
	 * Queue a transactional inval.  The immediate invalidation we just sent
	 * is the only one known to be necessary.  To reduce risk from the
	 * transition to immediate invalidation, continue sending a transactional
	 * invalidation like we've long done.  Third-party code might rely on it.
	 */
	if (!IsBootstrapProcessingMode())
		CacheInvalidateHeapTuple(relation, tuple, NULL);
}

/*
 * heap_inplace_unlock - reverse of heap_inplace_lock
 */
void
heap_inplace_unlock(Relation relation,
					HeapTuple oldtup, Buffer buffer)
{
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	UnlockTuple(relation, &oldtup->t_self, InplaceUpdateTupleLock);
	ForgetInplace_Inval();
}

#define		FRM_NOOP				0x0001
#define		FRM_INVALIDATE_XMAX		0x0002
#define		FRM_RETURN_IS_XID		0x0004
#define		FRM_RETURN_IS_MULTI		0x0008
#define		FRM_MARK_COMMITTED		0x0010

/*
 * FreezeMultiXactId
 *		Determine what to do during freezing when a tuple is marked by a
 *		MultiXactId.
 *
 * "flags" is an output value; it's used to tell caller what to do on return.
 * "pagefrz" is an input/output value, used to manage page level freezing.
 *
 * Possible values that we can set in "flags":
 * FRM_NOOP
 *		don't do anything -- keep existing Xmax
 * FRM_INVALIDATE_XMAX
 *		mark Xmax as InvalidTransactionId and set XMAX_INVALID flag.
 * FRM_RETURN_IS_XID
 *		The Xid return value is a single update Xid to set as xmax.
 * FRM_MARK_COMMITTED
 *		Xmax can be marked as HEAP_XMAX_COMMITTED
 * FRM_RETURN_IS_MULTI
 *		The return value is a new MultiXactId to set as new Xmax.
 *		(caller must obtain proper infomask bits using GetMultiXactIdHintBits)
 *
 * Caller delegates control of page freezing to us.  In practice we always
 * force freezing of caller's page unless FRM_NOOP processing is indicated.
 * We help caller ensure that XIDs < FreezeLimit and MXIDs < MultiXactCutoff
 * can never be left behind.  We freely choose when and how to process each
 * Multi, without ever violating the cutoff postconditions for freezing.
 *
 * It's useful to remove Multis on a proactive timeline (relative to freezing
 * XIDs) to keep MultiXact member SLRU buffer misses to a minimum.  It can also
 * be cheaper in the short run, for us, since we too can avoid SLRU buffer
 * misses through eager processing.
 *
 * NB: Creates a _new_ MultiXactId when FRM_RETURN_IS_MULTI is set, though only
 * when FreezeLimit and/or MultiXactCutoff cutoffs leave us with no choice.
 * This can usually be put off, which is usually enough to avoid it altogether.
 * Allocating new multis during VACUUM should be avoided on general principle;
 * only VACUUM can advance relminmxid, so allocating new Multis here comes with
 * its own special risks.
 *
 * NB: Caller must maintain "no freeze" NewRelfrozenXid/NewRelminMxid trackers
 * using heap_tuple_should_freeze when we haven't forced page-level freezing.
 *
 * NB: Caller should avoid needlessly calling heap_tuple_should_freeze when we
 * have already forced page-level freezing, since that might incur the same
 * SLRU buffer misses that we specifically intended to avoid by freezing.
 */
static TransactionId
FreezeMultiXactId(MultiXactId multi, uint16 t_infomask,
				  const struct VacuumCutoffs *cutoffs, uint16 *flags,
				  HeapPageFreeze *pagefrz)
{
	TransactionId newxmax;
	MultiXactMember *members;
	int			nmembers;
	bool		need_replace;
	int			nnewmembers;
	MultiXactMember *newmembers;
	bool		has_lockers;
	TransactionId update_xid;
	bool		update_committed;
	TransactionId FreezePageRelfrozenXid;

	*flags = 0;

	/* We should only be called in Multis */
	Assert(t_infomask & HEAP_XMAX_IS_MULTI);

	if (!MultiXactIdIsValid(multi) ||
		HEAP_LOCKED_UPGRADED(t_infomask))
	{
		*flags |= FRM_INVALIDATE_XMAX;
		pagefrz->freeze_required = true;
		return InvalidTransactionId;
	}
	else if (MultiXactIdPrecedes(multi, cutoffs->relminmxid))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("found multixact %u from before relminmxid %u",
								 multi, cutoffs->relminmxid)));
	else if (MultiXactIdPrecedes(multi, cutoffs->OldestMxact))
	{
		TransactionId update_xact;

		/*
		 * This old multi cannot possibly have members still running, but
		 * verify just in case.  If it was a locker only, it can be removed
		 * without any further consideration; but if it contained an update,
		 * we might need to preserve it.
		 */
		if (MultiXactIdIsRunning(multi,
								 HEAP_XMAX_IS_LOCKED_ONLY(t_infomask)))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("multixact %u from before multi freeze cutoff %u found to be still running",
									 multi, cutoffs->OldestMxact)));

		if (HEAP_XMAX_IS_LOCKED_ONLY(t_infomask))
		{
			*flags |= FRM_INVALIDATE_XMAX;
			pagefrz->freeze_required = true;
			return InvalidTransactionId;
		}

		/* replace multi with single XID for its updater? */
		update_xact = MultiXactIdGetUpdateXid(multi, t_infomask);
		if (TransactionIdPrecedes(update_xact, cutoffs->relfrozenxid))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("multixact %u contains update XID %u from before relfrozenxid %u",
									 multi, update_xact,
									 cutoffs->relfrozenxid)));
		else if (TransactionIdPrecedes(update_xact, cutoffs->OldestXmin))
		{
			/*
			 * Updater XID has to have aborted (otherwise the tuple would have
			 * been pruned away instead, since updater XID is < OldestXmin).
			 * Just remove xmax.
			 */
			if (TransactionIdDidCommit(update_xact))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg_internal("multixact %u contains committed update XID %u from before removable cutoff %u",
										 multi, update_xact,
										 cutoffs->OldestXmin)));
			*flags |= FRM_INVALIDATE_XMAX;
			pagefrz->freeze_required = true;
			return InvalidTransactionId;
		}

		/* Have to keep updater XID as new xmax */
		*flags |= FRM_RETURN_IS_XID;
		pagefrz->freeze_required = true;
		return update_xact;
	}

	/*
	 * Some member(s) of this Multi may be below FreezeLimit xid cutoff, so we
	 * need to walk the whole members array to figure out what to do, if
	 * anything.
	 */
	nmembers =
		GetMultiXactIdMembers(multi, &members, false,
							  HEAP_XMAX_IS_LOCKED_ONLY(t_infomask));
	if (nmembers <= 0)
	{
		/* Nothing worth keeping */
		*flags |= FRM_INVALIDATE_XMAX;
		pagefrz->freeze_required = true;
		return InvalidTransactionId;
	}

	/*
	 * The FRM_NOOP case is the only case where we might need to ratchet back
	 * FreezePageRelfrozenXid or FreezePageRelminMxid.  It is also the only
	 * case where our caller might ratchet back its NoFreezePageRelfrozenXid
	 * or NoFreezePageRelminMxid "no freeze" trackers to deal with a multi.
	 * FRM_NOOP handling should result in the NewRelfrozenXid/NewRelminMxid
	 * trackers managed by VACUUM being ratcheting back by xmax to the degree
	 * required to make it safe to leave xmax undisturbed, independent of
	 * whether or not page freezing is triggered somewhere else.
	 *
	 * Our policy is to force freezing in every case other than FRM_NOOP,
	 * which obviates the need to maintain either set of trackers, anywhere.
	 * Every other case will reliably execute a freeze plan for xmax that
	 * either replaces xmax with an XID/MXID >= OldestXmin/OldestMxact, or
	 * sets xmax to an InvalidTransactionId XID, rendering xmax fully frozen.
	 * (VACUUM's NewRelfrozenXid/NewRelminMxid trackers are initialized with
	 * OldestXmin/OldestMxact, so later values never need to be tracked here.)
	 */
	need_replace = false;
	FreezePageRelfrozenXid = pagefrz->FreezePageRelfrozenXid;
	for (int i = 0; i < nmembers; i++)
	{
		TransactionId xid = members[i].xid;

		Assert(!TransactionIdPrecedes(xid, cutoffs->relfrozenxid));

		if (TransactionIdPrecedes(xid, cutoffs->FreezeLimit))
		{
			/* Can't violate the FreezeLimit postcondition */
			need_replace = true;
			break;
		}
		if (TransactionIdPrecedes(xid, FreezePageRelfrozenXid))
			FreezePageRelfrozenXid = xid;
	}

	/* Can't violate the MultiXactCutoff postcondition, either */
	if (!need_replace)
		need_replace = MultiXactIdPrecedes(multi, cutoffs->MultiXactCutoff);

	if (!need_replace)
	{
		/*
		 * vacuumlazy.c might ratchet back NewRelminMxid, NewRelfrozenXid, or
		 * both together to make it safe to retain this particular multi after
		 * freezing its page
		 */
		*flags |= FRM_NOOP;
		pagefrz->FreezePageRelfrozenXid = FreezePageRelfrozenXid;
		if (MultiXactIdPrecedes(multi, pagefrz->FreezePageRelminMxid))
			pagefrz->FreezePageRelminMxid = multi;
		pfree(members);
		return multi;
	}

	/*
	 * Do a more thorough second pass over the multi to figure out which
	 * member XIDs actually need to be kept.  Checking the precise status of
	 * individual members might even show that we don't need to keep anything.
	 * That is quite possible even though the Multi must be >= OldestMxact,
	 * since our second pass only keeps member XIDs when it's truly necessary;
	 * even member XIDs >= OldestXmin often won't be kept by second pass.
	 */
	nnewmembers = 0;
	newmembers = palloc(sizeof(MultiXactMember) * nmembers);
	has_lockers = false;
	update_xid = InvalidTransactionId;
	update_committed = false;

	/*
	 * Determine whether to keep each member xid, or to ignore it instead
	 */
	for (int i = 0; i < nmembers; i++)
	{
		TransactionId xid = members[i].xid;
		MultiXactStatus mstatus = members[i].status;

		Assert(!TransactionIdPrecedes(xid, cutoffs->relfrozenxid));

		if (!ISUPDATE_from_mxstatus(mstatus))
		{
			/*
			 * Locker XID (not updater XID).  We only keep lockers that are
			 * still running.
			 */
			if (TransactionIdIsCurrentTransactionId(xid) ||
				TransactionIdIsInProgress(xid))
			{
				if (TransactionIdPrecedes(xid, cutoffs->OldestXmin))
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg_internal("multixact %u contains running locker XID %u from before removable cutoff %u",
											 multi, xid,
											 cutoffs->OldestXmin)));
				newmembers[nnewmembers++] = members[i];
				has_lockers = true;
			}

			continue;
		}

		/*
		 * Updater XID (not locker XID).  Should we keep it?
		 *
		 * Since the tuple wasn't totally removed when vacuum pruned, the
		 * update Xid cannot possibly be older than OldestXmin cutoff unless
		 * the updater XID aborted.  If the updater transaction is known
		 * aborted or crashed then it's okay to ignore it, otherwise not.
		 *
		 * In any case the Multi should never contain two updaters, whatever
		 * their individual commit status.  Check for that first, in passing.
		 */
		if (TransactionIdIsValid(update_xid))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("multixact %u has two or more updating members",
									 multi),
					 errdetail_internal("First updater XID=%u second updater XID=%u.",
										update_xid, xid)));

		/*
		 * As with all tuple visibility routines, it's critical to test
		 * TransactionIdIsInProgress before TransactionIdDidCommit, because of
		 * race conditions explained in detail in heapam_visibility.c.
		 */
		if (TransactionIdIsCurrentTransactionId(xid) ||
			TransactionIdIsInProgress(xid))
			update_xid = xid;
		else if (TransactionIdDidCommit(xid))
		{
			/*
			 * The transaction committed, so we can tell caller to set
			 * HEAP_XMAX_COMMITTED.  (We can only do this because we know the
			 * transaction is not running.)
			 */
			update_committed = true;
			update_xid = xid;
		}
		else
		{
			/*
			 * Not in progress, not committed -- must be aborted or crashed;
			 * we can ignore it.
			 */
			continue;
		}

		/*
		 * We determined that updater must be kept -- add it to pending new
		 * members list
		 */
		if (TransactionIdPrecedes(xid, cutoffs->OldestXmin))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("multixact %u contains committed update XID %u from before removable cutoff %u",
									 multi, xid, cutoffs->OldestXmin)));
		newmembers[nnewmembers++] = members[i];
	}

	pfree(members);

	/*
	 * Determine what to do with caller's multi based on information gathered
	 * during our second pass
	 */
	if (nnewmembers == 0)
	{
		/* Nothing worth keeping */
		*flags |= FRM_INVALIDATE_XMAX;
		newxmax = InvalidTransactionId;
	}
	else if (TransactionIdIsValid(update_xid) && !has_lockers)
	{
		/*
		 * If there's a single member and it's an update, pass it back alone
		 * without creating a new Multi.  (XXX we could do this when there's a
		 * single remaining locker, too, but that would complicate the API too
		 * much; moreover, the case with the single updater is more
		 * interesting, because those are longer-lived.)
		 */
		Assert(nnewmembers == 1);
		*flags |= FRM_RETURN_IS_XID;
		if (update_committed)
			*flags |= FRM_MARK_COMMITTED;
		newxmax = update_xid;
	}
	else
	{
		/*
		 * Create a new multixact with the surviving members of the previous
		 * one, to set as new Xmax in the tuple
		 */
		newxmax = MultiXactIdCreateFromMembers(nnewmembers, newmembers);
		*flags |= FRM_RETURN_IS_MULTI;
	}

	pfree(newmembers);

	pagefrz->freeze_required = true;
	return newxmax;
}

/*
 * heap_prepare_freeze_tuple
 *
 * Check to see whether any of the XID fields of a tuple (xmin, xmax, xvac)
 * are older than the OldestXmin and/or OldestMxact freeze cutoffs.  If so,
 * setup enough state (in the *frz output argument) to enable caller to
 * process this tuple as part of freezing its page, and return true.  Return
 * false if nothing can be changed about the tuple right now.
 *
 * Also sets *totally_frozen to true if the tuple will be totally frozen once
 * caller executes returned freeze plan (or if the tuple was already totally
 * frozen by an earlier VACUUM).  This indicates that there are no remaining
 * XIDs or MultiXactIds that will need to be processed by a future VACUUM.
 *
 * VACUUM caller must assemble HeapTupleFreeze freeze plan entries for every
 * tuple that we returned true for, and then execute freezing.  Caller must
 * initialize pagefrz fields for page as a whole before first call here for
 * each heap page.
 *
 * VACUUM caller decides on whether or not to freeze the page as a whole.
 * We'll often prepare freeze plans for a page that caller just discards.
 * However, VACUUM doesn't always get to make a choice; it must freeze when
 * pagefrz.freeze_required is set, to ensure that any XIDs < FreezeLimit (and
 * MXIDs < MultiXactCutoff) can never be left behind.  We help to make sure
 * that VACUUM always follows that rule.
 *
 * We sometimes force freezing of xmax MultiXactId values long before it is
 * strictly necessary to do so just to ensure the FreezeLimit postcondition.
 * It's worth processing MultiXactIds proactively when it is cheap to do so,
 * and it's convenient to make that happen by piggy-backing it on the "force
 * freezing" mechanism.  Conversely, we sometimes delay freezing MultiXactIds
 * because it is expensive right now (though only when it's still possible to
 * do so without violating the FreezeLimit/MultiXactCutoff postcondition).
 *
 * It is assumed that the caller has checked the tuple with
 * HeapTupleSatisfiesVacuum() and determined that it is not HEAPTUPLE_DEAD
 * (else we should be removing the tuple, not freezing it).
 *
 * NB: This function has side effects: it might allocate a new MultiXactId.
 * It will be set as tuple's new xmax when our *frz output is processed within
 * heap_execute_freeze_tuple later on.  If the tuple is in a shared buffer
 * then caller had better have an exclusive lock on it already.
 */
bool
heap_prepare_freeze_tuple(HeapTupleHeader tuple,
						  const struct VacuumCutoffs *cutoffs,
						  HeapPageFreeze *pagefrz,
						  HeapTupleFreeze *frz, bool *totally_frozen)
{
	bool		xmin_already_frozen = false,
				xmax_already_frozen = false;
	bool		freeze_xmin = false,
				replace_xvac = false,
				replace_xmax = false,
				freeze_xmax = false;
	TransactionId xid;

	frz->xmax = HeapTupleHeaderGetRawXmax(tuple);
	frz->t_infomask2 = tuple->t_infomask2;
	frz->t_infomask = tuple->t_infomask;
	frz->frzflags = 0;
	frz->checkflags = 0;

	/*
	 * Process xmin, while keeping track of whether it's already frozen, or
	 * will become frozen iff our freeze plan is executed by caller (could be
	 * neither).
	 */
	xid = HeapTupleHeaderGetXmin(tuple);
	if (!TransactionIdIsNormal(xid))
		xmin_already_frozen = true;
	else
	{
		if (TransactionIdPrecedes(xid, cutoffs->relfrozenxid))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("found xmin %u from before relfrozenxid %u",
									 xid, cutoffs->relfrozenxid)));

		/* Will set freeze_xmin flags in freeze plan below */
		freeze_xmin = TransactionIdPrecedes(xid, cutoffs->OldestXmin);

		/* Verify that xmin committed if and when freeze plan is executed */
		if (freeze_xmin)
			frz->checkflags |= HEAP_FREEZE_CHECK_XMIN_COMMITTED;
	}

	/*
	 * Old-style VACUUM FULL is gone, but we have to process xvac for as long
	 * as we support having MOVED_OFF/MOVED_IN tuples in the database
	 */
	xid = HeapTupleHeaderGetXvac(tuple);
	if (TransactionIdIsNormal(xid))
	{
		Assert(TransactionIdPrecedesOrEquals(cutoffs->relfrozenxid, xid));
		Assert(TransactionIdPrecedes(xid, cutoffs->OldestXmin));

		/*
		 * For Xvac, we always freeze proactively.  This allows totally_frozen
		 * tracking to ignore xvac.
		 */
		replace_xvac = pagefrz->freeze_required = true;

		/* Will set replace_xvac flags in freeze plan below */
	}

	/* Now process xmax */
	xid = frz->xmax;
	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		/* Raw xmax is a MultiXactId */
		TransactionId newxmax;
		uint16		flags;

		/*
		 * We will either remove xmax completely (in the "freeze_xmax" path),
		 * process xmax by replacing it (in the "replace_xmax" path), or
		 * perform no-op xmax processing.  The only constraint is that the
		 * FreezeLimit/MultiXactCutoff postcondition must never be violated.
		 */
		newxmax = FreezeMultiXactId(xid, tuple->t_infomask, cutoffs,
									&flags, pagefrz);

		if (flags & FRM_NOOP)
		{
			/*
			 * xmax is a MultiXactId, and nothing about it changes for now.
			 * This is the only case where 'freeze_required' won't have been
			 * set for us by FreezeMultiXactId, as well as the only case where
			 * neither freeze_xmax nor replace_xmax are set (given a multi).
			 *
			 * This is a no-op, but the call to FreezeMultiXactId might have
			 * ratcheted back NewRelfrozenXid and/or NewRelminMxid trackers
			 * for us (the "freeze page" variants, specifically).  That'll
			 * make it safe for our caller to freeze the page later on, while
			 * leaving this particular xmax undisturbed.
			 *
			 * FreezeMultiXactId is _not_ responsible for the "no freeze"
			 * NewRelfrozenXid/NewRelminMxid trackers, though -- that's our
			 * job.  A call to heap_tuple_should_freeze for this same tuple
			 * will take place below if 'freeze_required' isn't set already.
			 * (This repeats work from FreezeMultiXactId, but allows "no
			 * freeze" tracker maintenance to happen in only one place.)
			 */
			Assert(!MultiXactIdPrecedes(newxmax, cutoffs->MultiXactCutoff));
			Assert(MultiXactIdIsValid(newxmax) && xid == newxmax);
		}
		else if (flags & FRM_RETURN_IS_XID)
		{
			/*
			 * xmax will become an updater Xid (original MultiXact's updater
			 * member Xid will be carried forward as a simple Xid in Xmax).
			 */
			Assert(!TransactionIdPrecedes(newxmax, cutoffs->OldestXmin));

			/*
			 * NB -- some of these transformations are only valid because we
			 * know the return Xid is a tuple updater (i.e. not merely a
			 * locker.) Also note that the only reason we don't explicitly
			 * worry about HEAP_KEYS_UPDATED is because it lives in
			 * t_infomask2 rather than t_infomask.
			 */
			frz->t_infomask &= ~HEAP_XMAX_BITS;
			frz->xmax = newxmax;
			if (flags & FRM_MARK_COMMITTED)
				frz->t_infomask |= HEAP_XMAX_COMMITTED;
			replace_xmax = true;
		}
		else if (flags & FRM_RETURN_IS_MULTI)
		{
			uint16		newbits;
			uint16		newbits2;

			/*
			 * xmax is an old MultiXactId that we have to replace with a new
			 * MultiXactId, to carry forward two or more original member XIDs.
			 */
			Assert(!MultiXactIdPrecedes(newxmax, cutoffs->OldestMxact));

			/*
			 * We can't use GetMultiXactIdHintBits directly on the new multi
			 * here; that routine initializes the masks to all zeroes, which
			 * would lose other bits we need.  Doing it this way ensures all
			 * unrelated bits remain untouched.
			 */
			frz->t_infomask &= ~HEAP_XMAX_BITS;
			frz->t_infomask2 &= ~HEAP_KEYS_UPDATED;
			GetMultiXactIdHintBits(newxmax, &newbits, &newbits2);
			frz->t_infomask |= newbits;
			frz->t_infomask2 |= newbits2;
			frz->xmax = newxmax;
			replace_xmax = true;
		}
		else
		{
			/*
			 * Freeze plan for tuple "freezes xmax" in the strictest sense:
			 * it'll leave nothing in xmax (neither an Xid nor a MultiXactId).
			 */
			Assert(flags & FRM_INVALIDATE_XMAX);
			Assert(!TransactionIdIsValid(newxmax));

			/* Will set freeze_xmax flags in freeze plan below */
			freeze_xmax = true;
		}

		/* MultiXactId processing forces freezing (barring FRM_NOOP case) */
		Assert(pagefrz->freeze_required || (!freeze_xmax && !replace_xmax));
	}
	else if (TransactionIdIsNormal(xid))
	{
		/* Raw xmax is normal XID */
		if (TransactionIdPrecedes(xid, cutoffs->relfrozenxid))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("found xmax %u from before relfrozenxid %u",
									 xid, cutoffs->relfrozenxid)));

		/* Will set freeze_xmax flags in freeze plan below */
		freeze_xmax = TransactionIdPrecedes(xid, cutoffs->OldestXmin);

		/*
		 * Verify that xmax aborted if and when freeze plan is executed,
		 * provided it's from an update. (A lock-only xmax can be removed
		 * independent of this, since the lock is released at xact end.)
		 */
		if (freeze_xmax && !HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
			frz->checkflags |= HEAP_FREEZE_CHECK_XMAX_ABORTED;
	}
	else if (!TransactionIdIsValid(xid))
	{
		/* Raw xmax is InvalidTransactionId XID */
		Assert((tuple->t_infomask & HEAP_XMAX_IS_MULTI) == 0);
		xmax_already_frozen = true;
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("found raw xmax %u (infomask 0x%04x) not invalid and not multi",
								 xid, tuple->t_infomask)));

	if (freeze_xmin)
	{
		Assert(!xmin_already_frozen);

		frz->t_infomask |= HEAP_XMIN_FROZEN;
	}
	if (replace_xvac)
	{
		/*
		 * If a MOVED_OFF tuple is not dead, the xvac transaction must have
		 * failed; whereas a non-dead MOVED_IN tuple must mean the xvac
		 * transaction succeeded.
		 */
		Assert(pagefrz->freeze_required);
		if (tuple->t_infomask & HEAP_MOVED_OFF)
			frz->frzflags |= XLH_INVALID_XVAC;
		else
			frz->frzflags |= XLH_FREEZE_XVAC;
	}
	if (replace_xmax)
	{
		Assert(!xmax_already_frozen && !freeze_xmax);
		Assert(pagefrz->freeze_required);

		/* Already set replace_xmax flags in freeze plan earlier */
	}
	if (freeze_xmax)
	{
		Assert(!xmax_already_frozen && !replace_xmax);

		frz->xmax = InvalidTransactionId;

		/*
		 * The tuple might be marked either XMAX_INVALID or XMAX_COMMITTED +
		 * LOCKED.  Normalize to INVALID just to be sure no one gets confused.
		 * Also get rid of the HEAP_KEYS_UPDATED bit.
		 */
		frz->t_infomask &= ~HEAP_XMAX_BITS;
		frz->t_infomask |= HEAP_XMAX_INVALID;
		frz->t_infomask2 &= ~HEAP_HOT_UPDATED;
		frz->t_infomask2 &= ~HEAP_KEYS_UPDATED;
	}

	/*
	 * Determine if this tuple is already totally frozen, or will become
	 * totally frozen (provided caller executes freeze plans for the page)
	 */
	*totally_frozen = ((freeze_xmin || xmin_already_frozen) &&
					   (freeze_xmax || xmax_already_frozen));

	if (!pagefrz->freeze_required && !(xmin_already_frozen &&
									   xmax_already_frozen))
	{
		/*
		 * So far no previous tuple from the page made freezing mandatory.
		 * Does this tuple force caller to freeze the entire page?
		 */
		pagefrz->freeze_required =
			heap_tuple_should_freeze(tuple, cutoffs,
									 &pagefrz->NoFreezePageRelfrozenXid,
									 &pagefrz->NoFreezePageRelminMxid);
	}

	/* Tell caller if this tuple has a usable freeze plan set in *frz */
	return freeze_xmin || replace_xvac || replace_xmax || freeze_xmax;
}

/*
 * Perform xmin/xmax XID status sanity checks before actually executing freeze
 * plans.
 *
 * heap_prepare_freeze_tuple doesn't perform these checks directly because
 * pg_xact lookups are relatively expensive.  They shouldn't be repeated by
 * successive VACUUMs that each decide against freezing the same page.
 */
void
heap_pre_freeze_checks(Buffer buffer,
					   HeapTupleFreeze *tuples, int ntuples)
{
	Page		page = BufferGetPage(buffer);

	for (int i = 0; i < ntuples; i++)
	{
		HeapTupleFreeze *frz = tuples + i;
		ItemId		itemid = PageGetItemId(page, frz->offset);
		HeapTupleHeader htup;

		htup = (HeapTupleHeader) PageGetItem(page, itemid);

		/* Deliberately avoid relying on tuple hint bits here */
		if (frz->checkflags & HEAP_FREEZE_CHECK_XMIN_COMMITTED)
		{
			TransactionId xmin = HeapTupleHeaderGetRawXmin(htup);

			Assert(!HeapTupleHeaderXminFrozen(htup));
			if (unlikely(!TransactionIdDidCommit(xmin)))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg_internal("uncommitted xmin %u needs to be frozen",
										 xmin)));
		}

		/*
		 * TransactionIdDidAbort won't work reliably in the presence of XIDs
		 * left behind by transactions that were in progress during a crash,
		 * so we can only check that xmax didn't commit
		 */
		if (frz->checkflags & HEAP_FREEZE_CHECK_XMAX_ABORTED)
		{
			TransactionId xmax = HeapTupleHeaderGetRawXmax(htup);

			Assert(TransactionIdIsNormal(xmax));
			if (unlikely(TransactionIdDidCommit(xmax)))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg_internal("cannot freeze committed xmax %u",
										 xmax)));
		}
	}
}

/*
 * Helper which executes freezing of one or more heap tuples on a page on
 * behalf of caller.  Caller passes an array of tuple plans from
 * heap_prepare_freeze_tuple.  Caller must set 'offset' in each plan for us.
 * Must be called in a critical section that also marks the buffer dirty and,
 * if needed, emits WAL.
 */
void
heap_freeze_prepared_tuples(Buffer buffer, HeapTupleFreeze *tuples, int ntuples)
{
	Page		page = BufferGetPage(buffer);

	for (int i = 0; i < ntuples; i++)
	{
		HeapTupleFreeze *frz = tuples + i;
		ItemId		itemid = PageGetItemId(page, frz->offset);
		HeapTupleHeader htup;

		htup = (HeapTupleHeader) PageGetItem(page, itemid);
		heap_execute_freeze_tuple(htup, frz);
	}
}

/*
 * heap_freeze_tuple
 *		Freeze tuple in place, without WAL logging.
 *
 * Useful for callers like CLUSTER that perform their own WAL logging.
 */
bool
heap_freeze_tuple(HeapTupleHeader tuple,
				  TransactionId relfrozenxid, TransactionId relminmxid,
				  TransactionId FreezeLimit, TransactionId MultiXactCutoff)
{
	HeapTupleFreeze frz;
	bool		do_freeze;
	bool		totally_frozen;
	struct VacuumCutoffs cutoffs;
	HeapPageFreeze pagefrz;

	cutoffs.relfrozenxid = relfrozenxid;
	cutoffs.relminmxid = relminmxid;
	cutoffs.OldestXmin = FreezeLimit;
	cutoffs.OldestMxact = MultiXactCutoff;
	cutoffs.FreezeLimit = FreezeLimit;
	cutoffs.MultiXactCutoff = MultiXactCutoff;

	pagefrz.freeze_required = true;
	pagefrz.FreezePageRelfrozenXid = FreezeLimit;
	pagefrz.FreezePageRelminMxid = MultiXactCutoff;
	pagefrz.NoFreezePageRelfrozenXid = FreezeLimit;
	pagefrz.NoFreezePageRelminMxid = MultiXactCutoff;

	do_freeze = heap_prepare_freeze_tuple(tuple, &cutoffs,
										  &pagefrz, &frz, &totally_frozen);

	/*
	 * Note that because this is not a WAL-logged operation, we don't need to
	 * fill in the offset in the freeze record.
	 */

	if (do_freeze)
		heap_execute_freeze_tuple(tuple, &frz);
	return do_freeze;
}

/*
 * For a given MultiXactId, return the hint bits that should be set in the
 * tuple's infomask.
 *
 * Normally this should be called for a multixact that was just created, and
 * so is on our local cache, so the GetMembers call is fast.
 */
static void
GetMultiXactIdHintBits(MultiXactId multi, uint16 *new_infomask,
					   uint16 *new_infomask2)
{
	int			nmembers;
	MultiXactMember *members;
	int			i;
	uint16		bits = HEAP_XMAX_IS_MULTI;
	uint16		bits2 = 0;
	bool		has_update = false;
	LockTupleMode strongest = LockTupleKeyShare;

	/*
	 * We only use this in multis we just created, so they cannot be values
	 * pre-pg_upgrade.
	 */
	nmembers = GetMultiXactIdMembers(multi, &members, false, false);

	for (i = 0; i < nmembers; i++)
	{
		LockTupleMode mode;

		/*
		 * Remember the strongest lock mode held by any member of the
		 * multixact.
		 */
		mode = TUPLOCK_from_mxstatus(members[i].status);
		if (mode > strongest)
			strongest = mode;

		/* See what other bits we need */
		switch (members[i].status)
		{
			case MultiXactStatusForKeyShare:
			case MultiXactStatusForShare:
			case MultiXactStatusForNoKeyUpdate:
				break;

			case MultiXactStatusForUpdate:
				bits2 |= HEAP_KEYS_UPDATED;
				break;

			case MultiXactStatusNoKeyUpdate:
				has_update = true;
				break;

			case MultiXactStatusUpdate:
				bits2 |= HEAP_KEYS_UPDATED;
				has_update = true;
				break;
		}
	}

	if (strongest == LockTupleExclusive ||
		strongest == LockTupleNoKeyExclusive)
		bits |= HEAP_XMAX_EXCL_LOCK;
	else if (strongest == LockTupleShare)
		bits |= HEAP_XMAX_SHR_LOCK;
	else if (strongest == LockTupleKeyShare)
		bits |= HEAP_XMAX_KEYSHR_LOCK;

	if (!has_update)
		bits |= HEAP_XMAX_LOCK_ONLY;

	if (nmembers > 0)
		pfree(members);

	*new_infomask = bits;
	*new_infomask2 = bits2;
}

/*
 * MultiXactIdGetUpdateXid
 *
 * Given a multixact Xmax and corresponding infomask, which does not have the
 * HEAP_XMAX_LOCK_ONLY bit set, obtain and return the Xid of the updating
 * transaction.
 *
 * Caller is expected to check the status of the updating transaction, if
 * necessary.
 */
static TransactionId
MultiXactIdGetUpdateXid(TransactionId xmax, uint16 t_infomask)
{
	TransactionId update_xact = InvalidTransactionId;
	MultiXactMember *members;
	int			nmembers;

	Assert(!(t_infomask & HEAP_XMAX_LOCK_ONLY));
	Assert(t_infomask & HEAP_XMAX_IS_MULTI);

	/*
	 * Since we know the LOCK_ONLY bit is not set, this cannot be a multi from
	 * pre-pg_upgrade.
	 */
	nmembers = GetMultiXactIdMembers(xmax, &members, false, false);

	if (nmembers > 0)
	{
		int			i;

		for (i = 0; i < nmembers; i++)
		{
			/* Ignore lockers */
			if (!ISUPDATE_from_mxstatus(members[i].status))
				continue;

			/* there can be at most one updater */
			Assert(update_xact == InvalidTransactionId);
			update_xact = members[i].xid;
#ifndef USE_ASSERT_CHECKING

			/*
			 * in an assert-enabled build, walk the whole array to ensure
			 * there's no other updater.
			 */
			break;
#endif
		}

		pfree(members);
	}

	return update_xact;
}

/*
 * HeapTupleGetUpdateXid
 *		As above, but use a HeapTupleHeader
 *
 * See also HeapTupleHeaderGetUpdateXid, which can be used without previously
 * checking the hint bits.
 */
TransactionId
HeapTupleGetUpdateXid(const HeapTupleHeaderData *tup)
{
	return MultiXactIdGetUpdateXid(HeapTupleHeaderGetRawXmax(tup),
								   tup->t_infomask);
}

/*
 * Does the given multixact conflict with the current transaction grabbing a
 * tuple lock of the given strength?
 *
 * The passed infomask pairs up with the given multixact in the tuple header.
 *
 * If current_is_member is not NULL, it is set to 'true' if the current
 * transaction is a member of the given multixact.
 */
static bool
DoesMultiXactIdConflict(MultiXactId multi, uint16 infomask,
						LockTupleMode lockmode, bool *current_is_member)
{
	int			nmembers;
	MultiXactMember *members;
	bool		result = false;
	LOCKMODE	wanted = tupleLockExtraInfo[lockmode].hwlock;

	if (HEAP_LOCKED_UPGRADED(infomask))
		return false;

	nmembers = GetMultiXactIdMembers(multi, &members, false,
									 HEAP_XMAX_IS_LOCKED_ONLY(infomask));
	if (nmembers >= 0)
	{
		int			i;

		for (i = 0; i < nmembers; i++)
		{
			TransactionId memxid;
			LOCKMODE	memlockmode;

			if (result && (current_is_member == NULL || *current_is_member))
				break;

			memlockmode = LOCKMODE_from_mxstatus(members[i].status);

			/* ignore members from current xact (but track their presence) */
			memxid = members[i].xid;
			if (TransactionIdIsCurrentTransactionId(memxid))
			{
				if (current_is_member != NULL)
					*current_is_member = true;
				continue;
			}
			else if (result)
				continue;

			/* ignore members that don't conflict with the lock we want */
			if (!DoLockModesConflict(memlockmode, wanted))
				continue;

			if (ISUPDATE_from_mxstatus(members[i].status))
			{
				/* ignore aborted updaters */
				if (TransactionIdDidAbort(memxid))
					continue;
			}
			else
			{
				/* ignore lockers-only that are no longer in progress */
				if (!TransactionIdIsInProgress(memxid))
					continue;
			}

			/*
			 * Whatever remains are either live lockers that conflict with our
			 * wanted lock, and updaters that are not aborted.  Those conflict
			 * with what we want.  Set up to return true, but keep going to
			 * look for the current transaction among the multixact members,
			 * if needed.
			 */
			result = true;
		}
		pfree(members);
	}

	return result;
}

/*
 * Do_MultiXactIdWait
 *		Actual implementation for the two functions below.
 *
 * 'multi', 'status' and 'infomask' indicate what to sleep on (the status is
 * needed to ensure we only sleep on conflicting members, and the infomask is
 * used to optimize multixact access in case it's a lock-only multi); 'nowait'
 * indicates whether to use conditional lock acquisition, to allow callers to
 * fail if lock is unavailable.  'rel', 'ctid' and 'oper' are used to set up
 * context information for error messages.  'remaining', if not NULL, receives
 * the number of members that are still running, including any (non-aborted)
 * subtransactions of our own transaction.  'logLockFailure' indicates whether
 * to log details when a lock acquisition fails with 'nowait' enabled.
 *
 * We do this by sleeping on each member using XactLockTableWait.  Any
 * members that belong to the current backend are *not* waited for, however;
 * this would not merely be useless but would lead to Assert failure inside
 * XactLockTableWait.  By the time this returns, it is certain that all
 * transactions *of other backends* that were members of the MultiXactId
 * that conflict with the requested status are dead (and no new ones can have
 * been added, since it is not legal to add members to an existing
 * MultiXactId).
 *
 * But by the time we finish sleeping, someone else may have changed the Xmax
 * of the containing tuple, so the caller needs to iterate on us somehow.
 *
 * Note that in case we return false, the number of remaining members is
 * not to be trusted.
 */
static bool
Do_MultiXactIdWait(MultiXactId multi, MultiXactStatus status,
				   uint16 infomask, bool nowait,
				   Relation rel, ItemPointer ctid, XLTW_Oper oper,
				   int *remaining, bool logLockFailure)
{
	bool		result = true;
	MultiXactMember *members;
	int			nmembers;
	int			remain = 0;

	/* for pre-pg_upgrade tuples, no need to sleep at all */
	nmembers = HEAP_LOCKED_UPGRADED(infomask) ? -1 :
		GetMultiXactIdMembers(multi, &members, false,
							  HEAP_XMAX_IS_LOCKED_ONLY(infomask));

	if (nmembers >= 0)
	{
		int			i;

		for (i = 0; i < nmembers; i++)
		{
			TransactionId memxid = members[i].xid;
			MultiXactStatus memstatus = members[i].status;

			if (TransactionIdIsCurrentTransactionId(memxid))
			{
				remain++;
				continue;
			}

			if (!DoLockModesConflict(LOCKMODE_from_mxstatus(memstatus),
									 LOCKMODE_from_mxstatus(status)))
			{
				if (remaining && TransactionIdIsInProgress(memxid))
					remain++;
				continue;
			}

			/*
			 * This member conflicts with our multi, so we have to sleep (or
			 * return failure, if asked to avoid waiting.)
			 *
			 * Note that we don't set up an error context callback ourselves,
			 * but instead we pass the info down to XactLockTableWait.  This
			 * might seem a bit wasteful because the context is set up and
			 * tore down for each member of the multixact, but in reality it
			 * should be barely noticeable, and it avoids duplicate code.
			 */
			if (nowait)
			{
				result = ConditionalXactLockTableWait(memxid, logLockFailure);
				if (!result)
					break;
			}
			else
				XactLockTableWait(memxid, rel, ctid, oper);
		}

		pfree(members);
	}

	if (remaining)
		*remaining = remain;

	return result;
}

/*
 * MultiXactIdWait
 *		Sleep on a MultiXactId.
 *
 * By the time we finish sleeping, someone else may have changed the Xmax
 * of the containing tuple, so the caller needs to iterate on us somehow.
 *
 * We return (in *remaining, if not NULL) the number of members that are still
 * running, including any (non-aborted) subtransactions of our own transaction.
 */
static void
MultiXactIdWait(MultiXactId multi, MultiXactStatus status, uint16 infomask,
				Relation rel, ItemPointer ctid, XLTW_Oper oper,
				int *remaining)
{
	(void) Do_MultiXactIdWait(multi, status, infomask, false,
							  rel, ctid, oper, remaining, false);
}

/*
 * ConditionalMultiXactIdWait
 *		As above, but only lock if we can get the lock without blocking.
 *
 * By the time we finish sleeping, someone else may have changed the Xmax
 * of the containing tuple, so the caller needs to iterate on us somehow.
 *
 * If the multixact is now all gone, return true.  Returns false if some
 * transactions might still be running.
 *
 * We return (in *remaining, if not NULL) the number of members that are still
 * running, including any (non-aborted) subtransactions of our own transaction.
 */
static bool
ConditionalMultiXactIdWait(MultiXactId multi, MultiXactStatus status,
						   uint16 infomask, Relation rel, int *remaining,
						   bool logLockFailure)
{
	return Do_MultiXactIdWait(multi, status, infomask, true,
							  rel, NULL, XLTW_None, remaining, logLockFailure);
}

/*
 * heap_tuple_needs_eventual_freeze
 *
 * Check to see whether any of the XID fields of a tuple (xmin, xmax, xvac)
 * will eventually require freezing (if tuple isn't removed by pruning first).
 */
bool
heap_tuple_needs_eventual_freeze(HeapTupleHeader tuple)
{
	TransactionId xid;

	/*
	 * If xmin is a normal transaction ID, this tuple is definitely not
	 * frozen.
	 */
	xid = HeapTupleHeaderGetXmin(tuple);
	if (TransactionIdIsNormal(xid))
		return true;

	/*
	 * If xmax is a valid xact or multixact, this tuple is also not frozen.
	 */
	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		MultiXactId multi;

		multi = HeapTupleHeaderGetRawXmax(tuple);
		if (MultiXactIdIsValid(multi))
			return true;
	}
	else
	{
		xid = HeapTupleHeaderGetRawXmax(tuple);
		if (TransactionIdIsNormal(xid))
			return true;
	}

	if (tuple->t_infomask & HEAP_MOVED)
	{
		xid = HeapTupleHeaderGetXvac(tuple);
		if (TransactionIdIsNormal(xid))
			return true;
	}

	return false;
}

/*
 * heap_tuple_should_freeze
 *
 * Return value indicates if heap_prepare_freeze_tuple sibling function would
 * (or should) force freezing of the heap page that contains caller's tuple.
 * Tuple header XIDs/MXIDs < FreezeLimit/MultiXactCutoff trigger freezing.
 * This includes (xmin, xmax, xvac) fields, as well as MultiXact member XIDs.
 *
 * The *NoFreezePageRelfrozenXid and *NoFreezePageRelminMxid input/output
 * arguments help VACUUM track the oldest extant XID/MXID remaining in rel.
 * Our working assumption is that caller won't decide to freeze this tuple.
 * It's up to caller to only ratchet back its own top-level trackers after the
 * point that it fully commits to not freezing the tuple/page in question.
 */
bool
heap_tuple_should_freeze(HeapTupleHeader tuple,
						 const struct VacuumCutoffs *cutoffs,
						 TransactionId *NoFreezePageRelfrozenXid,
						 MultiXactId *NoFreezePageRelminMxid)
{
	TransactionId xid;
	MultiXactId multi;
	bool		freeze = false;

	/* First deal with xmin */
	xid = HeapTupleHeaderGetXmin(tuple);
	if (TransactionIdIsNormal(xid))
	{
		Assert(TransactionIdPrecedesOrEquals(cutoffs->relfrozenxid, xid));
		if (TransactionIdPrecedes(xid, *NoFreezePageRelfrozenXid))
			*NoFreezePageRelfrozenXid = xid;
		if (TransactionIdPrecedes(xid, cutoffs->FreezeLimit))
			freeze = true;
	}

	/* Now deal with xmax */
	xid = InvalidTransactionId;
	multi = InvalidMultiXactId;
	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
		multi = HeapTupleHeaderGetRawXmax(tuple);
	else
		xid = HeapTupleHeaderGetRawXmax(tuple);

	if (TransactionIdIsNormal(xid))
	{
		Assert(TransactionIdPrecedesOrEquals(cutoffs->relfrozenxid, xid));
		/* xmax is a non-permanent XID */
		if (TransactionIdPrecedes(xid, *NoFreezePageRelfrozenXid))
			*NoFreezePageRelfrozenXid = xid;
		if (TransactionIdPrecedes(xid, cutoffs->FreezeLimit))
			freeze = true;
	}
	else if (!MultiXactIdIsValid(multi))
	{
		/* xmax is a permanent XID or invalid MultiXactId/XID */
	}
	else if (HEAP_LOCKED_UPGRADED(tuple->t_infomask))
	{
		/* xmax is a pg_upgrade'd MultiXact, which can't have updater XID */
		if (MultiXactIdPrecedes(multi, *NoFreezePageRelminMxid))
			*NoFreezePageRelminMxid = multi;
		/* heap_prepare_freeze_tuple always freezes pg_upgrade'd xmax */
		freeze = true;
	}
	else
	{
		/* xmax is a MultiXactId that may have an updater XID */
		MultiXactMember *members;
		int			nmembers;

		Assert(MultiXactIdPrecedesOrEquals(cutoffs->relminmxid, multi));
		if (MultiXactIdPrecedes(multi, *NoFreezePageRelminMxid))
			*NoFreezePageRelminMxid = multi;
		if (MultiXactIdPrecedes(multi, cutoffs->MultiXactCutoff))
			freeze = true;

		/* need to check whether any member of the mxact is old */
		nmembers = GetMultiXactIdMembers(multi, &members, false,
										 HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask));

		for (int i = 0; i < nmembers; i++)
		{
			xid = members[i].xid;
			Assert(TransactionIdPrecedesOrEquals(cutoffs->relfrozenxid, xid));
			if (TransactionIdPrecedes(xid, *NoFreezePageRelfrozenXid))
				*NoFreezePageRelfrozenXid = xid;
			if (TransactionIdPrecedes(xid, cutoffs->FreezeLimit))
				freeze = true;
		}
		if (nmembers > 0)
			pfree(members);
	}

	if (tuple->t_infomask & HEAP_MOVED)
	{
		xid = HeapTupleHeaderGetXvac(tuple);
		if (TransactionIdIsNormal(xid))
		{
			Assert(TransactionIdPrecedesOrEquals(cutoffs->relfrozenxid, xid));
			if (TransactionIdPrecedes(xid, *NoFreezePageRelfrozenXid))
				*NoFreezePageRelfrozenXid = xid;
			/* heap_prepare_freeze_tuple forces xvac freezing */
			freeze = true;
		}
	}

	return freeze;
}

/*
 * Maintain snapshotConflictHorizon for caller by ratcheting forward its value
 * using any committed XIDs contained in 'tuple', an obsolescent heap tuple
 * that caller is in the process of physically removing, e.g. via HOT pruning
 * or index deletion.
 *
 * Caller must initialize its value to InvalidTransactionId, which is
 * generally interpreted as "definitely no need for a recovery conflict".
 * Final value must reflect all heap tuples that caller will physically remove
 * (or remove TID references to) via its ongoing pruning/deletion operation.
 * ResolveRecoveryConflictWithSnapshot() is passed the final value (taken from
 * caller's WAL record) by REDO routine when it replays caller's operation.
 */
void
HeapTupleHeaderAdvanceConflictHorizon(HeapTupleHeader tuple,
									  TransactionId *snapshotConflictHorizon)
{
	TransactionId xmin = HeapTupleHeaderGetXmin(tuple);
	TransactionId xmax = HeapTupleHeaderGetUpdateXid(tuple);
	TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

	if (tuple->t_infomask & HEAP_MOVED)
	{
		if (TransactionIdPrecedes(*snapshotConflictHorizon, xvac))
			*snapshotConflictHorizon = xvac;
	}

	/*
	 * Ignore tuples inserted by an aborted transaction or if the tuple was
	 * updated/deleted by the inserting transaction.
	 *
	 * Look for a committed hint bit, or if no xmin bit is set, check clog.
	 */
	if (HeapTupleHeaderXminCommitted(tuple) ||
		(!HeapTupleHeaderXminInvalid(tuple) && TransactionIdDidCommit(xmin)))
	{
		if (xmax != xmin &&
			TransactionIdFollows(xmax, *snapshotConflictHorizon))
			*snapshotConflictHorizon = xmax;
	}
}

#ifdef USE_PREFETCH
/*
 * Helper function for heap_index_delete_tuples.  Issues prefetch requests for
 * prefetch_count buffers.  The prefetch_state keeps track of all the buffers
 * we can prefetch, and which have already been prefetched; each call to this
 * function picks up where the previous call left off.
 *
 * Note: we expect the deltids array to be sorted in an order that groups TIDs
 * by heap block, with all TIDs for each block appearing together in exactly
 * one group.
 */
static void
index_delete_prefetch_buffer(Relation rel,
							 IndexDeletePrefetchState *prefetch_state,
							 int prefetch_count)
{
	BlockNumber cur_hblkno = prefetch_state->cur_hblkno;
	int			count = 0;
	int			i;
	int			ndeltids = prefetch_state->ndeltids;
	TM_IndexDelete *deltids = prefetch_state->deltids;

	for (i = prefetch_state->next_item;
		 i < ndeltids && count < prefetch_count;
		 i++)
	{
		ItemPointer htid = &deltids[i].tid;

		if (cur_hblkno == InvalidBlockNumber ||
			ItemPointerGetBlockNumber(htid) != cur_hblkno)
		{
			cur_hblkno = ItemPointerGetBlockNumber(htid);
			PrefetchBuffer(rel, MAIN_FORKNUM, cur_hblkno);
			count++;
		}
	}

	/*
	 * Save the prefetch position so that next time we can continue from that
	 * position.
	 */
	prefetch_state->next_item = i;
	prefetch_state->cur_hblkno = cur_hblkno;
}
#endif

/*
 * Helper function for heap_index_delete_tuples.  Checks for index corruption
 * involving an invalid TID in index AM caller's index page.
 *
 * This is an ideal place for these checks.  The index AM must hold a buffer
 * lock on the index page containing the TIDs we examine here, so we don't
 * have to worry about concurrent VACUUMs at all.  We can be sure that the
 * index is corrupt when htid points directly to an LP_UNUSED item or
 * heap-only tuple, which is not the case during standard index scans.
 */
static inline void
index_delete_check_htid(TM_IndexDeleteOp *delstate,
						Page page, OffsetNumber maxoff,
						ItemPointer htid, TM_IndexStatus *istatus)
{
	OffsetNumber indexpagehoffnum = ItemPointerGetOffsetNumber(htid);
	ItemId		iid;

	Assert(OffsetNumberIsValid(istatus->idxoffnum));

	if (unlikely(indexpagehoffnum > maxoff))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg_internal("heap tid from index tuple (%u,%u) points past end of heap page line pointer array at offset %u of block %u in index \"%s\"",
								 ItemPointerGetBlockNumber(htid),
								 indexpagehoffnum,
								 istatus->idxoffnum, delstate->iblknum,
								 RelationGetRelationName(delstate->irel))));

	iid = PageGetItemId(page, indexpagehoffnum);
	if (unlikely(!ItemIdIsUsed(iid)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg_internal("heap tid from index tuple (%u,%u) points to unused heap page item at offset %u of block %u in index \"%s\"",
								 ItemPointerGetBlockNumber(htid),
								 indexpagehoffnum,
								 istatus->idxoffnum, delstate->iblknum,
								 RelationGetRelationName(delstate->irel))));

	if (ItemIdHasStorage(iid))
	{
		HeapTupleHeader htup;

		Assert(ItemIdIsNormal(iid));
		htup = (HeapTupleHeader) PageGetItem(page, iid);

		if (unlikely(HeapTupleHeaderIsHeapOnly(htup)))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg_internal("heap tid from index tuple (%u,%u) points to heap-only tuple at offset %u of block %u in index \"%s\"",
									 ItemPointerGetBlockNumber(htid),
									 indexpagehoffnum,
									 istatus->idxoffnum, delstate->iblknum,
									 RelationGetRelationName(delstate->irel))));
	}
}

/*
 * heapam implementation of tableam's index_delete_tuples interface.
 *
 * This helper function is called by index AMs during index tuple deletion.
 * See tableam header comments for an explanation of the interface implemented
 * here and a general theory of operation.  Note that each call here is either
 * a simple index deletion call, or a bottom-up index deletion call.
 *
 * It's possible for this to generate a fair amount of I/O, since we may be
 * deleting hundreds of tuples from a single index block.  To amortize that
 * cost to some degree, this uses prefetching and combines repeat accesses to
 * the same heap block.
 */
TransactionId
heap_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	/* Initial assumption is that earlier pruning took care of conflict */
	TransactionId snapshotConflictHorizon = InvalidTransactionId;
	BlockNumber blkno = InvalidBlockNumber;
	Buffer		buf = InvalidBuffer;
	Page		page = NULL;
	OffsetNumber maxoff = InvalidOffsetNumber;
	TransactionId priorXmax;
#ifdef USE_PREFETCH
	IndexDeletePrefetchState prefetch_state;
	int			prefetch_distance;
#endif
	SnapshotData SnapshotNonVacuumable;
	int			finalndeltids = 0,
				nblocksaccessed = 0;

	/* State that's only used in bottom-up index deletion case */
	int			nblocksfavorable = 0;
	int			curtargetfreespace = delstate->bottomupfreespace,
				lastfreespace = 0,
				actualfreespace = 0;
	bool		bottomup_final_block = false;

	InitNonVacuumableSnapshot(SnapshotNonVacuumable, GlobalVisTestFor(rel));

	/* Sort caller's deltids array by TID for further processing */
	index_delete_sort(delstate);

	/*
	 * Bottom-up case: resort deltids array in an order attuned to where the
	 * greatest number of promising TIDs are to be found, and determine how
	 * many blocks from the start of sorted array should be considered
	 * favorable.  This will also shrink the deltids array in order to
	 * eliminate completely unfavorable blocks up front.
	 */
	if (delstate->bottomup)
		nblocksfavorable = bottomup_sort_and_shrink(delstate);

#ifdef USE_PREFETCH
	/* Initialize prefetch state. */
	prefetch_state.cur_hblkno = InvalidBlockNumber;
	prefetch_state.next_item = 0;
	prefetch_state.ndeltids = delstate->ndeltids;
	prefetch_state.deltids = delstate->deltids;

	/*
	 * Determine the prefetch distance that we will attempt to maintain.
	 *
	 * Since the caller holds a buffer lock somewhere in rel, we'd better make
	 * sure that isn't a catalog relation before we call code that does
	 * syscache lookups, to avoid risk of deadlock.
	 */
	if (IsCatalogRelation(rel))
		prefetch_distance = maintenance_io_concurrency;
	else
		prefetch_distance =
			get_tablespace_maintenance_io_concurrency(rel->rd_rel->reltablespace);

	/* Cap initial prefetch distance for bottom-up deletion caller */
	if (delstate->bottomup)
	{
		Assert(nblocksfavorable >= 1);
		Assert(nblocksfavorable <= BOTTOMUP_MAX_NBLOCKS);
		prefetch_distance = Min(prefetch_distance, nblocksfavorable);
	}

	/* Start prefetching. */
	index_delete_prefetch_buffer(rel, &prefetch_state, prefetch_distance);
#endif

	/* Iterate over deltids, determine which to delete, check their horizon */
	Assert(delstate->ndeltids > 0);
	for (int i = 0; i < delstate->ndeltids; i++)
	{
		TM_IndexDelete *ideltid = &delstate->deltids[i];
		TM_IndexStatus *istatus = delstate->status + ideltid->id;
		ItemPointer htid = &ideltid->tid;
		OffsetNumber offnum;

		/*
		 * Read buffer, and perform required extra steps each time a new block
		 * is encountered.  Avoid refetching if it's the same block as the one
		 * from the last htid.
		 */
		if (blkno == InvalidBlockNumber ||
			ItemPointerGetBlockNumber(htid) != blkno)
		{
			/*
			 * Consider giving up early for bottom-up index deletion caller
			 * first. (Only prefetch next-next block afterwards, when it
			 * becomes clear that we're at least going to access the next
			 * block in line.)
			 *
			 * Sometimes the first block frees so much space for bottom-up
			 * caller that the deletion process can end without accessing any
			 * more blocks.  It is usually necessary to access 2 or 3 blocks
			 * per bottom-up deletion operation, though.
			 */
			if (delstate->bottomup)
			{
				/*
				 * We often allow caller to delete a few additional items
				 * whose entries we reached after the point that space target
				 * from caller was satisfied.  The cost of accessing the page
				 * was already paid at that point, so it made sense to finish
				 * it off.  When that happened, we finalize everything here
				 * (by finishing off the whole bottom-up deletion operation
				 * without needlessly paying the cost of accessing any more
				 * blocks).
				 */
				if (bottomup_final_block)
					break;

				/*
				 * Give up when we didn't enable our caller to free any
				 * additional space as a result of processing the page that we
				 * just finished up with.  This rule is the main way in which
				 * we keep the cost of bottom-up deletion under control.
				 */
				if (nblocksaccessed >= 1 && actualfreespace == lastfreespace)
					break;
				lastfreespace = actualfreespace;	/* for next time */

				/*
				 * Deletion operation (which is bottom-up) will definitely
				 * access the next block in line.  Prepare for that now.
				 *
				 * Decay target free space so that we don't hang on for too
				 * long with a marginal case. (Space target is only truly
				 * helpful when it allows us to recognize that we don't need
				 * to access more than 1 or 2 blocks to satisfy caller due to
				 * agreeable workload characteristics.)
				 *
				 * We are a bit more patient when we encounter contiguous
				 * blocks, though: these are treated as favorable blocks.  The
				 * decay process is only applied when the next block in line
				 * is not a favorable/contiguous block.  This is not an
				 * exception to the general rule; we still insist on finding
				 * at least one deletable item per block accessed.  See
				 * bottomup_nblocksfavorable() for full details of the theory
				 * behind favorable blocks and heap block locality in general.
				 *
				 * Note: The first block in line is always treated as a
				 * favorable block, so the earliest possible point that the
				 * decay can be applied is just before we access the second
				 * block in line.  The Assert() verifies this for us.
				 */
				Assert(nblocksaccessed > 0 || nblocksfavorable > 0);
				if (nblocksfavorable > 0)
					nblocksfavorable--;
				else
					curtargetfreespace /= 2;
			}

			/* release old buffer */
			if (BufferIsValid(buf))
				UnlockReleaseBuffer(buf);

			blkno = ItemPointerGetBlockNumber(htid);
			buf = ReadBuffer(rel, blkno);
			nblocksaccessed++;
			Assert(!delstate->bottomup ||
				   nblocksaccessed <= BOTTOMUP_MAX_NBLOCKS);

#ifdef USE_PREFETCH

			/*
			 * To maintain the prefetch distance, prefetch one more page for
			 * each page we read.
			 */
			index_delete_prefetch_buffer(rel, &prefetch_state, 1);
#endif

			LockBuffer(buf, BUFFER_LOCK_SHARE);

			page = BufferGetPage(buf);
			maxoff = PageGetMaxOffsetNumber(page);
		}

		/*
		 * In passing, detect index corruption involving an index page with a
		 * TID that points to a location in the heap that couldn't possibly be
		 * correct.  We only do this with actual TIDs from caller's index page
		 * (not items reached by traversing through a HOT chain).
		 */
		index_delete_check_htid(delstate, page, maxoff, htid, istatus);

		if (istatus->knowndeletable)
			Assert(!delstate->bottomup && !istatus->promising);
		else
		{
			ItemPointerData tmp = *htid;
			HeapTupleData heapTuple;

			/* Are any tuples from this HOT chain non-vacuumable? */
			if (heap_hot_search_buffer(&tmp, rel, buf, &SnapshotNonVacuumable,
									   &heapTuple, NULL, true))
				continue;		/* can't delete entry */

			/* Caller will delete, since whole HOT chain is vacuumable */
			istatus->knowndeletable = true;

			/* Maintain index free space info for bottom-up deletion case */
			if (delstate->bottomup)
			{
				Assert(istatus->freespace > 0);
				actualfreespace += istatus->freespace;
				if (actualfreespace >= curtargetfreespace)
					bottomup_final_block = true;
			}
		}

		/*
		 * Maintain snapshotConflictHorizon value for deletion operation as a
		 * whole by advancing current value using heap tuple headers.  This is
		 * loosely based on the logic for pruning a HOT chain.
		 */
		offnum = ItemPointerGetOffsetNumber(htid);
		priorXmax = InvalidTransactionId;	/* cannot check first XMIN */
		for (;;)
		{
			ItemId		lp;
			HeapTupleHeader htup;

			/* Sanity check (pure paranoia) */
			if (offnum < FirstOffsetNumber)
				break;

			/*
			 * An offset past the end of page's line pointer array is possible
			 * when the array was truncated
			 */
			if (offnum > maxoff)
				break;

			lp = PageGetItemId(page, offnum);
			if (ItemIdIsRedirected(lp))
			{
				offnum = ItemIdGetRedirect(lp);
				continue;
			}

			/*
			 * We'll often encounter LP_DEAD line pointers (especially with an
			 * entry marked knowndeletable by our caller up front).  No heap
			 * tuple headers get examined for an htid that leads us to an
			 * LP_DEAD item.  This is okay because the earlier pruning
			 * operation that made the line pointer LP_DEAD in the first place
			 * must have considered the original tuple header as part of
			 * generating its own snapshotConflictHorizon value.
			 *
			 * Relying on XLOG_HEAP2_PRUNE_VACUUM_SCAN records like this is
			 * the same strategy that index vacuuming uses in all cases. Index
			 * VACUUM WAL records don't even have a snapshotConflictHorizon
			 * field of their own for this reason.
			 */
			if (!ItemIdIsNormal(lp))
				break;

			htup = (HeapTupleHeader) PageGetItem(page, lp);

			/*
			 * Check the tuple XMIN against prior XMAX, if any
			 */
			if (TransactionIdIsValid(priorXmax) &&
				!TransactionIdEquals(HeapTupleHeaderGetXmin(htup), priorXmax))
				break;

			HeapTupleHeaderAdvanceConflictHorizon(htup,
												  &snapshotConflictHorizon);

			/*
			 * If the tuple is not HOT-updated, then we are at the end of this
			 * HOT-chain.  No need to visit later tuples from the same update
			 * chain (they get their own index entries) -- just move on to
			 * next htid from index AM caller.
			 */
			if (!HeapTupleHeaderIsHotUpdated(htup))
				break;

			/* Advance to next HOT chain member */
			Assert(ItemPointerGetBlockNumber(&htup->t_ctid) == blkno);
			offnum = ItemPointerGetOffsetNumber(&htup->t_ctid);
			priorXmax = HeapTupleHeaderGetUpdateXid(htup);
		}

		/* Enable further/final shrinking of deltids for caller */
		finalndeltids = i + 1;
	}

	UnlockReleaseBuffer(buf);

	/*
	 * Shrink deltids array to exclude non-deletable entries at the end.  This
	 * is not just a minor optimization.  Final deltids array size might be
	 * zero for a bottom-up caller.  Index AM is explicitly allowed to rely on
	 * ndeltids being zero in all cases with zero total deletable entries.
	 */
	Assert(finalndeltids > 0 || delstate->bottomup);
	delstate->ndeltids = finalndeltids;

	return snapshotConflictHorizon;
}

/*
 * Specialized inlineable comparison function for index_delete_sort()
 */
static inline int
index_delete_sort_cmp(TM_IndexDelete *deltid1, TM_IndexDelete *deltid2)
{
	ItemPointer tid1 = &deltid1->tid;
	ItemPointer tid2 = &deltid2->tid;

	{
		BlockNumber blk1 = ItemPointerGetBlockNumber(tid1);
		BlockNumber blk2 = ItemPointerGetBlockNumber(tid2);

		if (blk1 != blk2)
			return (blk1 < blk2) ? -1 : 1;
	}
	{
		OffsetNumber pos1 = ItemPointerGetOffsetNumber(tid1);
		OffsetNumber pos2 = ItemPointerGetOffsetNumber(tid2);

		if (pos1 != pos2)
			return (pos1 < pos2) ? -1 : 1;
	}

	Assert(false);

	return 0;
}

/*
 * Sort deltids array from delstate by TID.  This prepares it for further
 * processing by heap_index_delete_tuples().
 *
 * This operation becomes a noticeable consumer of CPU cycles with some
 * workloads, so we go to the trouble of specialization/micro optimization.
 * We use shellsort for this because it's easy to specialize, compiles to
 * relatively few instructions, and is adaptive to presorted inputs/subsets
 * (which are typical here).
 */
static void
index_delete_sort(TM_IndexDeleteOp *delstate)
{
	TM_IndexDelete *deltids = delstate->deltids;
	int			ndeltids = delstate->ndeltids;

	/*
	 * Shellsort gap sequence (taken from Sedgewick-Incerpi paper).
	 *
	 * This implementation is fast with array sizes up to ~4500.  This covers
	 * all supported BLCKSZ values.
	 */
	const int	gaps[9] = {1968, 861, 336, 112, 48, 21, 7, 3, 1};

	/* Think carefully before changing anything here -- keep swaps cheap */
	StaticAssertDecl(sizeof(TM_IndexDelete) <= 8,
					 "element size exceeds 8 bytes");

	for (int g = 0; g < lengthof(gaps); g++)
	{
		for (int hi = gaps[g], i = hi; i < ndeltids; i++)
		{
			TM_IndexDelete d = deltids[i];
			int			j = i;

			while (j >= hi && index_delete_sort_cmp(&deltids[j - hi], &d) >= 0)
			{
				deltids[j] = deltids[j - hi];
				j -= hi;
			}
			deltids[j] = d;
		}
	}
}

/*
 * Returns how many blocks should be considered favorable/contiguous for a
 * bottom-up index deletion pass.  This is a number of heap blocks that starts
 * from and includes the first block in line.
 *
 * There is always at least one favorable block during bottom-up index
 * deletion.  In the worst case (i.e. with totally random heap blocks) the
 * first block in line (the only favorable block) can be thought of as a
 * degenerate array of contiguous blocks that consists of a single block.
 * heap_index_delete_tuples() will expect this.
 *
 * Caller passes blockgroups, a description of the final order that deltids
 * will be sorted in for heap_index_delete_tuples() bottom-up index deletion
 * processing.  Note that deltids need not actually be sorted just yet (caller
 * only passes deltids to us so that we can interpret blockgroups).
 *
 * You might guess that the existence of contiguous blocks cannot matter much,
 * since in general the main factor that determines which blocks we visit is
 * the number of promising TIDs, which is a fixed hint from the index AM.
 * We're not really targeting the general case, though -- the actual goal is
 * to adapt our behavior to a wide variety of naturally occurring conditions.
 * The effects of most of the heuristics we apply are only noticeable in the
 * aggregate, over time and across many _related_ bottom-up index deletion
 * passes.
 *
 * Deeming certain blocks favorable allows heapam to recognize and adapt to
 * workloads where heap blocks visited during bottom-up index deletion can be
 * accessed contiguously, in the sense that each newly visited block is the
 * neighbor of the block that bottom-up deletion just finished processing (or
 * close enough to it).  It will likely be cheaper to access more favorable
 * blocks sooner rather than later (e.g. in this pass, not across a series of
 * related bottom-up passes).  Either way it is probably only a matter of time
 * (or a matter of further correlated version churn) before all blocks that
 * appear together as a single large batch of favorable blocks get accessed by
 * _some_ bottom-up pass.  Large batches of favorable blocks tend to either
 * appear almost constantly or not even once (it all depends on per-index
 * workload characteristics).
 *
 * Note that the blockgroups sort order applies a power-of-two bucketing
 * scheme that creates opportunities for contiguous groups of blocks to get
 * batched together, at least with workloads that are naturally amenable to
 * being driven by heap block locality.  This doesn't just enhance the spatial
 * locality of bottom-up heap block processing in the obvious way.  It also
 * enables temporal locality of access, since sorting by heap block number
 * naturally tends to make the bottom-up processing order deterministic.
 *
 * Consider the following example to get a sense of how temporal locality
 * might matter: There is a heap relation with several indexes, each of which
 * is low to medium cardinality.  It is subject to constant non-HOT updates.
 * The updates are skewed (in one part of the primary key, perhaps).  None of
 * the indexes are logically modified by the UPDATE statements (if they were
 * then bottom-up index deletion would not be triggered in the first place).
 * Naturally, each new round of index tuples (for each heap tuple that gets a
 * heap_update() call) will have the same heap TID in each and every index.
 * Since these indexes are low cardinality and never get logically modified,
 * heapam processing during bottom-up deletion passes will access heap blocks
 * in approximately sequential order.  Temporal locality of access occurs due
 * to bottom-up deletion passes behaving very similarly across each of the
 * indexes at any given moment.  This keeps the number of buffer misses needed
 * to visit heap blocks to a minimum.
 */
static int
bottomup_nblocksfavorable(IndexDeleteCounts *blockgroups, int nblockgroups,
						  TM_IndexDelete *deltids)
{
	int64		lastblock = -1;
	int			nblocksfavorable = 0;

	Assert(nblockgroups >= 1);
	Assert(nblockgroups <= BOTTOMUP_MAX_NBLOCKS);

	/*
	 * We tolerate heap blocks that will be accessed only slightly out of
	 * physical order.  Small blips occur when a pair of almost-contiguous
	 * blocks happen to fall into different buckets (perhaps due only to a
	 * small difference in npromisingtids that the bucketing scheme didn't
	 * quite manage to ignore).  We effectively ignore these blips by applying
	 * a small tolerance.  The precise tolerance we use is a little arbitrary,
	 * but it works well enough in practice.
	 */
	for (int b = 0; b < nblockgroups; b++)
	{
		IndexDeleteCounts *group = blockgroups + b;
		TM_IndexDelete *firstdtid = deltids + group->ifirsttid;
		BlockNumber block = ItemPointerGetBlockNumber(&firstdtid->tid);

		if (lastblock != -1 &&
			((int64) block < lastblock - BOTTOMUP_TOLERANCE_NBLOCKS ||
			 (int64) block > lastblock + BOTTOMUP_TOLERANCE_NBLOCKS))
			break;

		nblocksfavorable++;
		lastblock = block;
	}

	/* Always indicate that there is at least 1 favorable block */
	Assert(nblocksfavorable >= 1);

	return nblocksfavorable;
}

/*
 * qsort comparison function for bottomup_sort_and_shrink()
 */
static int
bottomup_sort_and_shrink_cmp(const void *arg1, const void *arg2)
{
	const IndexDeleteCounts *group1 = (const IndexDeleteCounts *) arg1;
	const IndexDeleteCounts *group2 = (const IndexDeleteCounts *) arg2;

	/*
	 * Most significant field is npromisingtids (which we invert the order of
	 * so as to sort in desc order).
	 *
	 * Caller should have already normalized npromisingtids fields into
	 * power-of-two values (buckets).
	 */
	if (group1->npromisingtids > group2->npromisingtids)
		return -1;
	if (group1->npromisingtids < group2->npromisingtids)
		return 1;

	/*
	 * Tiebreak: desc ntids sort order.
	 *
	 * We cannot expect power-of-two values for ntids fields.  We should
	 * behave as if they were already rounded up for us instead.
	 */
	if (group1->ntids != group2->ntids)
	{
		uint32		ntids1 = pg_nextpower2_32((uint32) group1->ntids);
		uint32		ntids2 = pg_nextpower2_32((uint32) group2->ntids);

		if (ntids1 > ntids2)
			return -1;
		if (ntids1 < ntids2)
			return 1;
	}

	/*
	 * Tiebreak: asc offset-into-deltids-for-block (offset to first TID for
	 * block in deltids array) order.
	 *
	 * This is equivalent to sorting in ascending heap block number order
	 * (among otherwise equal subsets of the array).  This approach allows us
	 * to avoid accessing the out-of-line TID.  (We rely on the assumption
	 * that the deltids array was sorted in ascending heap TID order when
	 * these offsets to the first TID from each heap block group were formed.)
	 */
	if (group1->ifirsttid > group2->ifirsttid)
		return 1;
	if (group1->ifirsttid < group2->ifirsttid)
		return -1;

	pg_unreachable();

	return 0;
}

/*
 * heap_index_delete_tuples() helper function for bottom-up deletion callers.
 *
 * Sorts deltids array in the order needed for useful processing by bottom-up
 * deletion.  The array should already be sorted in TID order when we're
 * called.  The sort process groups heap TIDs from deltids into heap block
 * groupings.  Earlier/more-promising groups/blocks are usually those that are
 * known to have the most "promising" TIDs.
 *
 * Sets new size of deltids array (ndeltids) in state.  deltids will only have
 * TIDs from the BOTTOMUP_MAX_NBLOCKS most promising heap blocks when we
 * return.  This often means that deltids will be shrunk to a small fraction
 * of its original size (we eliminate many heap blocks from consideration for
 * caller up front).
 *
 * Returns the number of "favorable" blocks.  See bottomup_nblocksfavorable()
 * for a definition and full details.
 */
static int
bottomup_sort_and_shrink(TM_IndexDeleteOp *delstate)
{
	IndexDeleteCounts *blockgroups;
	TM_IndexDelete *reordereddeltids;
	BlockNumber curblock = InvalidBlockNumber;
	int			nblockgroups = 0;
	int			ncopied = 0;
	int			nblocksfavorable = 0;

	Assert(delstate->bottomup);
	Assert(delstate->ndeltids > 0);

	/* Calculate per-heap-block count of TIDs */
	blockgroups = palloc(sizeof(IndexDeleteCounts) * delstate->ndeltids);
	for (int i = 0; i < delstate->ndeltids; i++)
	{
		TM_IndexDelete *ideltid = &delstate->deltids[i];
		TM_IndexStatus *istatus = delstate->status + ideltid->id;
		ItemPointer htid = &ideltid->tid;
		bool		promising = istatus->promising;

		if (curblock != ItemPointerGetBlockNumber(htid))
		{
			/* New block group */
			nblockgroups++;

			Assert(curblock < ItemPointerGetBlockNumber(htid) ||
				   !BlockNumberIsValid(curblock));

			curblock = ItemPointerGetBlockNumber(htid);
			blockgroups[nblockgroups - 1].ifirsttid = i;
			blockgroups[nblockgroups - 1].ntids = 1;
			blockgroups[nblockgroups - 1].npromisingtids = 0;
		}
		else
		{
			blockgroups[nblockgroups - 1].ntids++;
		}

		if (promising)
			blockgroups[nblockgroups - 1].npromisingtids++;
	}

	/*
	 * We're about ready to sort block groups to determine the optimal order
	 * for visiting heap blocks.  But before we do, round the number of
	 * promising tuples for each block group up to the next power-of-two,
	 * unless it is very low (less than 4), in which case we round up to 4.
	 * npromisingtids is far too noisy to trust when choosing between a pair
	 * of block groups that both have very low values.
	 *
	 * This scheme divides heap blocks/block groups into buckets.  Each bucket
	 * contains blocks that have _approximately_ the same number of promising
	 * TIDs as each other.  The goal is to ignore relatively small differences
	 * in the total number of promising entries, so that the whole process can
	 * give a little weight to heapam factors (like heap block locality)
	 * instead.  This isn't a trade-off, really -- we have nothing to lose. It
	 * would be foolish to interpret small differences in npromisingtids
	 * values as anything more than noise.
	 *
	 * We tiebreak on nhtids when sorting block group subsets that have the
	 * same npromisingtids, but this has the same issues as npromisingtids,
	 * and so nhtids is subject to the same power-of-two bucketing scheme. The
	 * only reason that we don't fix nhtids in the same way here too is that
	 * we'll need accurate nhtids values after the sort.  We handle nhtids
	 * bucketization dynamically instead (in the sort comparator).
	 *
	 * See bottomup_nblocksfavorable() for a full explanation of when and how
	 * heap locality/favorable blocks can significantly influence when and how
	 * heap blocks are accessed.
	 */
	for (int b = 0; b < nblockgroups; b++)
	{
		IndexDeleteCounts *group = blockgroups + b;

		/* Better off falling back on nhtids with low npromisingtids */
		if (group->npromisingtids <= 4)
			group->npromisingtids = 4;
		else
			group->npromisingtids =
				pg_nextpower2_32((uint32) group->npromisingtids);
	}

	/* Sort groups and rearrange caller's deltids array */
	qsort(blockgroups, nblockgroups, sizeof(IndexDeleteCounts),
		  bottomup_sort_and_shrink_cmp);
	reordereddeltids = palloc(delstate->ndeltids * sizeof(TM_IndexDelete));

	nblockgroups = Min(BOTTOMUP_MAX_NBLOCKS, nblockgroups);
	/* Determine number of favorable blocks at the start of final deltids */
	nblocksfavorable = bottomup_nblocksfavorable(blockgroups, nblockgroups,
												 delstate->deltids);

	for (int b = 0; b < nblockgroups; b++)
	{
		IndexDeleteCounts *group = blockgroups + b;
		TM_IndexDelete *firstdtid = delstate->deltids + group->ifirsttid;

		memcpy(reordereddeltids + ncopied, firstdtid,
			   sizeof(TM_IndexDelete) * group->ntids);
		ncopied += group->ntids;
	}

	/* Copy final grouped and sorted TIDs back into start of caller's array */
	memcpy(delstate->deltids, reordereddeltids,
		   sizeof(TM_IndexDelete) * ncopied);
	delstate->ndeltids = ncopied;

	pfree(reordereddeltids);
	pfree(blockgroups);

	return nblocksfavorable;
}

/*
 * Perform XLogInsert for a heap-visible operation.  'block' is the block
 * being marked all-visible, and vm_buffer is the buffer containing the
 * corresponding visibility map block.  Both should have already been modified
 * and dirtied.
 *
 * snapshotConflictHorizon comes from the largest xmin on the page being
 * marked all-visible.  REDO routine uses it to generate recovery conflicts.
 *
 * If checksums or wal_log_hints are enabled, we may also generate a full-page
 * image of heap_buffer. Otherwise, we optimize away the FPI (by specifying
 * REGBUF_NO_IMAGE for the heap buffer), in which case the caller should *not*
 * update the heap page's LSN.
 */
XLogRecPtr
log_heap_visible(Relation rel, Buffer heap_buffer, Buffer vm_buffer,
				 TransactionId snapshotConflictHorizon, uint8 vmflags)
{
	xl_heap_visible xlrec;
	XLogRecPtr	recptr;
	uint8		flags;

	Assert(BufferIsValid(heap_buffer));
	Assert(BufferIsValid(vm_buffer));

	xlrec.snapshotConflictHorizon = snapshotConflictHorizon;
	xlrec.flags = vmflags;
	if (RelationIsAccessibleInLogicalDecoding(rel))
		xlrec.flags |= VISIBILITYMAP_XLOG_CATALOG_REL;
	XLogBeginInsert();
	XLogRegisterData(&xlrec, SizeOfHeapVisible);

	XLogRegisterBuffer(0, vm_buffer, 0);

	flags = REGBUF_STANDARD;
	if (!XLogHintBitIsNeeded())
		flags |= REGBUF_NO_IMAGE;
	XLogRegisterBuffer(1, heap_buffer, flags);

	recptr = XLogInsert(RM_HEAP2_ID, XLOG_HEAP2_VISIBLE);

	return recptr;
}

/*
 * Perform XLogInsert for a heap-update operation.  Caller must already
 * have modified the buffer(s) and marked them dirty.
 */
static XLogRecPtr
log_heap_update(Relation reln, Buffer oldbuf,
				Buffer newbuf, HeapTuple oldtup, HeapTuple newtup,
				HeapTuple old_key_tuple,
				bool all_visible_cleared, bool new_all_visible_cleared)
{
	xl_heap_update xlrec;
	xl_heap_header xlhdr;
	xl_heap_header xlhdr_idx;
	uint8		info;
	uint16		prefix_suffix[2];
	uint16		prefixlen = 0,
				suffixlen = 0;
	XLogRecPtr	recptr;
	Page		page = BufferGetPage(newbuf);
	bool		need_tuple_data = RelationIsLogicallyLogged(reln);
	bool		init;
	int			bufflags;

	/* Caller should not call me on a non-WAL-logged relation */
	Assert(RelationNeedsWAL(reln));

	XLogBeginInsert();

	if (HeapTupleIsHeapOnly(newtup))
		info = XLOG_HEAP_HOT_UPDATE;
	else
		info = XLOG_HEAP_UPDATE;

	/*
	 * If the old and new tuple are on the same page, we only need to log the
	 * parts of the new tuple that were changed.  That saves on the amount of
	 * WAL we need to write.  Currently, we just count any unchanged bytes in
	 * the beginning and end of the tuple.  That's quick to check, and
	 * perfectly covers the common case that only one field is updated.
	 *
	 * We could do this even if the old and new tuple are on different pages,
	 * but only if we don't make a full-page image of the old page, which is
	 * difficult to know in advance.  Also, if the old tuple is corrupt for
	 * some reason, it would allow the corruption to propagate the new page,
	 * so it seems best to avoid.  Under the general assumption that most
	 * updates tend to create the new tuple version on the same page, there
	 * isn't much to be gained by doing this across pages anyway.
	 *
	 * Skip this if we're taking a full-page image of the new page, as we
	 * don't include the new tuple in the WAL record in that case.  Also
	 * disable if wal_level='logical', as logical decoding needs to be able to
	 * read the new tuple in whole from the WAL record alone.
	 */
	if (oldbuf == newbuf && !need_tuple_data &&
		!XLogCheckBufferNeedsBackup(newbuf))
	{
		char	   *oldp = (char *) oldtup->t_data + oldtup->t_data->t_hoff;
		char	   *newp = (char *) newtup->t_data + newtup->t_data->t_hoff;
		int			oldlen = oldtup->t_len - oldtup->t_data->t_hoff;
		int			newlen = newtup->t_len - newtup->t_data->t_hoff;

		/* Check for common prefix between old and new tuple */
		for (prefixlen = 0; prefixlen < Min(oldlen, newlen); prefixlen++)
		{
			if (newp[prefixlen] != oldp[prefixlen])
				break;
		}

		/*
		 * Storing the length of the prefix takes 2 bytes, so we need to save
		 * at least 3 bytes or there's no point.
		 */
		if (prefixlen < 3)
			prefixlen = 0;

		/* Same for suffix */
		for (suffixlen = 0; suffixlen < Min(oldlen, newlen) - prefixlen; suffixlen++)
		{
			if (newp[newlen - suffixlen - 1] != oldp[oldlen - suffixlen - 1])
				break;
		}
		if (suffixlen < 3)
			suffixlen = 0;
	}

	/* Prepare main WAL data chain */
	xlrec.flags = 0;
	if (all_visible_cleared)
		xlrec.flags |= XLH_UPDATE_OLD_ALL_VISIBLE_CLEARED;
	if (new_all_visible_cleared)
		xlrec.flags |= XLH_UPDATE_NEW_ALL_VISIBLE_CLEARED;
	if (prefixlen > 0)
		xlrec.flags |= XLH_UPDATE_PREFIX_FROM_OLD;
	if (suffixlen > 0)
		xlrec.flags |= XLH_UPDATE_SUFFIX_FROM_OLD;
	if (need_tuple_data)
	{
		xlrec.flags |= XLH_UPDATE_CONTAINS_NEW_TUPLE;
		if (old_key_tuple)
		{
			if (reln->rd_rel->relreplident == REPLICA_IDENTITY_FULL)
				xlrec.flags |= XLH_UPDATE_CONTAINS_OLD_TUPLE;
			else
				xlrec.flags |= XLH_UPDATE_CONTAINS_OLD_KEY;
		}
	}

	/* If new tuple is the single and first tuple on page... */
	if (ItemPointerGetOffsetNumber(&(newtup->t_self)) == FirstOffsetNumber &&
		PageGetMaxOffsetNumber(page) == FirstOffsetNumber)
	{
		info |= XLOG_HEAP_INIT_PAGE;
		init = true;
	}
	else
		init = false;

	/* Prepare WAL data for the old page */
	xlrec.old_offnum = ItemPointerGetOffsetNumber(&oldtup->t_self);
	xlrec.old_xmax = HeapTupleHeaderGetRawXmax(oldtup->t_data);
	xlrec.old_infobits_set = compute_infobits(oldtup->t_data->t_infomask,
											  oldtup->t_data->t_infomask2);

	/* Prepare WAL data for the new page */
	xlrec.new_offnum = ItemPointerGetOffsetNumber(&newtup->t_self);
	xlrec.new_xmax = HeapTupleHeaderGetRawXmax(newtup->t_data);

	bufflags = REGBUF_STANDARD;
	if (init)
		bufflags |= REGBUF_WILL_INIT;
	if (need_tuple_data)
		bufflags |= REGBUF_KEEP_DATA;

	XLogRegisterBuffer(0, newbuf, bufflags);
	if (oldbuf != newbuf)
		XLogRegisterBuffer(1, oldbuf, REGBUF_STANDARD);

	XLogRegisterData(&xlrec, SizeOfHeapUpdate);

	/*
	 * Prepare WAL data for the new tuple.
	 */
	if (prefixlen > 0 || suffixlen > 0)
	{
		if (prefixlen > 0 && suffixlen > 0)
		{
			prefix_suffix[0] = prefixlen;
			prefix_suffix[1] = suffixlen;
			XLogRegisterBufData(0, &prefix_suffix, sizeof(uint16) * 2);
		}
		else if (prefixlen > 0)
		{
			XLogRegisterBufData(0, &prefixlen, sizeof(uint16));
		}
		else
		{
			XLogRegisterBufData(0, &suffixlen, sizeof(uint16));
		}
	}

	xlhdr.t_infomask2 = newtup->t_data->t_infomask2;
	xlhdr.t_infomask = newtup->t_data->t_infomask;
	xlhdr.t_hoff = newtup->t_data->t_hoff;
	Assert(SizeofHeapTupleHeader + prefixlen + suffixlen <= newtup->t_len);

	/*
	 * PG73FORMAT: write bitmap [+ padding] [+ oid] + data
	 *
	 * The 'data' doesn't include the common prefix or suffix.
	 */
	XLogRegisterBufData(0, &xlhdr, SizeOfHeapHeader);
	if (prefixlen == 0)
	{
		XLogRegisterBufData(0,
							(char *) newtup->t_data + SizeofHeapTupleHeader,
							newtup->t_len - SizeofHeapTupleHeader - suffixlen);
	}
	else
	{
		/*
		 * Have to write the null bitmap and data after the common prefix as
		 * two separate rdata entries.
		 */
		/* bitmap [+ padding] [+ oid] */
		if (newtup->t_data->t_hoff - SizeofHeapTupleHeader > 0)
		{
			XLogRegisterBufData(0,
								(char *) newtup->t_data + SizeofHeapTupleHeader,
								newtup->t_data->t_hoff - SizeofHeapTupleHeader);
		}

		/* data after common prefix */
		XLogRegisterBufData(0,
							(char *) newtup->t_data + newtup->t_data->t_hoff + prefixlen,
							newtup->t_len - newtup->t_data->t_hoff - prefixlen - suffixlen);
	}

	/* We need to log a tuple identity */
	if (need_tuple_data && old_key_tuple)
	{
		/* don't really need this, but its more comfy to decode */
		xlhdr_idx.t_infomask2 = old_key_tuple->t_data->t_infomask2;
		xlhdr_idx.t_infomask = old_key_tuple->t_data->t_infomask;
		xlhdr_idx.t_hoff = old_key_tuple->t_data->t_hoff;

		XLogRegisterData(&xlhdr_idx, SizeOfHeapHeader);

		/* PG73FORMAT: write bitmap [+ padding] [+ oid] + data */
		XLogRegisterData((char *) old_key_tuple->t_data + SizeofHeapTupleHeader,
						 old_key_tuple->t_len - SizeofHeapTupleHeader);
	}

	/* filtering by origin on a row level is much more efficient */
	XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);

	recptr = XLogInsert(RM_HEAP_ID, info);

	return recptr;
}

/*
 * Perform XLogInsert of an XLOG_HEAP2_NEW_CID record
 *
 * This is only used in wal_level >= WAL_LEVEL_LOGICAL, and only for catalog
 * tuples.
 */
static XLogRecPtr
log_heap_new_cid(Relation relation, HeapTuple tup)
{
	xl_heap_new_cid xlrec;

	XLogRecPtr	recptr;
	HeapTupleHeader hdr = tup->t_data;

	Assert(ItemPointerIsValid(&tup->t_self));
	Assert(tup->t_tableOid != InvalidOid);

	xlrec.top_xid = GetTopTransactionId();
	xlrec.target_locator = relation->rd_locator;
	xlrec.target_tid = tup->t_self;

	/*
	 * If the tuple got inserted & deleted in the same TX we definitely have a
	 * combo CID, set cmin and cmax.
	 */
	if (hdr->t_infomask & HEAP_COMBOCID)
	{
		Assert(!(hdr->t_infomask & HEAP_XMAX_INVALID));
		Assert(!HeapTupleHeaderXminInvalid(hdr));
		xlrec.cmin = HeapTupleHeaderGetCmin(hdr);
		xlrec.cmax = HeapTupleHeaderGetCmax(hdr);
		xlrec.combocid = HeapTupleHeaderGetRawCommandId(hdr);
	}
	/* No combo CID, so only cmin or cmax can be set by this TX */
	else
	{
		/*
		 * Tuple inserted.
		 *
		 * We need to check for LOCK ONLY because multixacts might be
		 * transferred to the new tuple in case of FOR KEY SHARE updates in
		 * which case there will be an xmax, although the tuple just got
		 * inserted.
		 */
		if (hdr->t_infomask & HEAP_XMAX_INVALID ||
			HEAP_XMAX_IS_LOCKED_ONLY(hdr->t_infomask))
		{
			xlrec.cmin = HeapTupleHeaderGetRawCommandId(hdr);
			xlrec.cmax = InvalidCommandId;
		}
		/* Tuple from a different tx updated or deleted. */
		else
		{
			xlrec.cmin = InvalidCommandId;
			xlrec.cmax = HeapTupleHeaderGetRawCommandId(hdr);
		}
		xlrec.combocid = InvalidCommandId;
	}

	/*
	 * Note that we don't need to register the buffer here, because this
	 * operation does not modify the page. The insert/update/delete that
	 * called us certainly did, but that's WAL-logged separately.
	 */
	XLogBeginInsert();
	XLogRegisterData(&xlrec, SizeOfHeapNewCid);

	/* will be looked at irrespective of origin */

	recptr = XLogInsert(RM_HEAP2_ID, XLOG_HEAP2_NEW_CID);

	return recptr;
}

/*
 * Build a heap tuple representing the configured REPLICA IDENTITY to represent
 * the old tuple in an UPDATE or DELETE.
 *
 * Returns NULL if there's no need to log an identity or if there's no suitable
 * key defined.
 *
 * Pass key_required true if any replica identity columns changed value, or if
 * any of them have any external data.  Delete must always pass true.
 *
 * *copy is set to true if the returned tuple is a modified copy rather than
 * the same tuple that was passed in.
 */
static HeapTuple
ExtractReplicaIdentity(Relation relation, HeapTuple tp, bool key_required,
					   bool *copy)
{
	TupleDesc	desc = RelationGetDescr(relation);
	char		replident = relation->rd_rel->relreplident;
	Bitmapset  *idattrs;
	HeapTuple	key_tuple;
	bool		nulls[MaxHeapAttributeNumber];
	Datum		values[MaxHeapAttributeNumber];

	*copy = false;

	if (!RelationIsLogicallyLogged(relation))
		return NULL;

	if (replident == REPLICA_IDENTITY_NOTHING)
		return NULL;

	if (replident == REPLICA_IDENTITY_FULL)
	{
		/*
		 * When logging the entire old tuple, it very well could contain
		 * toasted columns. If so, force them to be inlined.
		 */
		if (HeapTupleHasExternal(tp))
		{
			*copy = true;
			tp = toast_flatten_tuple(tp, desc);
		}
		return tp;
	}

	/* if the key isn't required and we're only logging the key, we're done */
	if (!key_required)
		return NULL;

	/* find out the replica identity columns */
	idattrs = RelationGetIndexAttrBitmap(relation,
										 INDEX_ATTR_BITMAP_IDENTITY_KEY);

	/*
	 * If there's no defined replica identity columns, treat as !key_required.
	 * (This case should not be reachable from heap_update, since that should
	 * calculate key_required accurately.  But heap_delete just passes
	 * constant true for key_required, so we can hit this case in deletes.)
	 */
	if (bms_is_empty(idattrs))
		return NULL;

	/*
	 * Construct a new tuple containing only the replica identity columns,
	 * with nulls elsewhere.  While we're at it, assert that the replica
	 * identity columns aren't null.
	 */
	heap_deform_tuple(tp, desc, values, nulls);

	for (int i = 0; i < desc->natts; i++)
	{
		if (bms_is_member(i + 1 - FirstLowInvalidHeapAttributeNumber,
						  idattrs))
			Assert(!nulls[i]);
		else
			nulls[i] = true;
	}

	key_tuple = heap_form_tuple(desc, values, nulls);
	*copy = true;

	bms_free(idattrs);

	/*
	 * If the tuple, which by here only contains indexed columns, still has
	 * toasted columns, force them to be inlined. This is somewhat unlikely
	 * since there's limits on the size of indexed columns, so we don't
	 * duplicate toast_flatten_tuple()s functionality in the above loop over
	 * the indexed columns, even if it would be more efficient.
	 */
	if (HeapTupleHasExternal(key_tuple))
	{
		HeapTuple	oldtup = key_tuple;

		key_tuple = toast_flatten_tuple(oldtup, desc);
		heap_freetuple(oldtup);
	}

	return key_tuple;
}

/*
 * HeapCheckForSerializableConflictOut
 *		We are reading a tuple.  If it's not visible, there may be a
 *		rw-conflict out with the inserter.  Otherwise, if it is visible to us
 *		but has been deleted, there may be a rw-conflict out with the deleter.
 *
 * We will determine the top level xid of the writing transaction with which
 * we may be in conflict, and ask CheckForSerializableConflictOut() to check
 * for overlap with our own transaction.
 *
 * This function should be called just about anywhere in heapam.c where a
 * tuple has been read. The caller must hold at least a shared lock on the
 * buffer, because this function might set hint bits on the tuple. There is
 * currently no known reason to call this function from an index AM.
 */
void
HeapCheckForSerializableConflictOut(bool visible, Relation relation,
									HeapTuple tuple, Buffer buffer,
									Snapshot snapshot)
{
	TransactionId xid;
	HTSV_Result htsvResult;

	if (!CheckForSerializableConflictOutNeeded(relation, snapshot))
		return;

	/*
	 * Check to see whether the tuple has been written to by a concurrent
	 * transaction, either to create it not visible to us, or to delete it
	 * while it is visible to us.  The "visible" bool indicates whether the
	 * tuple is visible to us, while HeapTupleSatisfiesVacuum checks what else
	 * is going on with it.
	 *
	 * In the event of a concurrently inserted tuple that also happens to have
	 * been concurrently updated (by a separate transaction), the xmin of the
	 * tuple will be used -- not the updater's xid.
	 */
	htsvResult = HeapTupleSatisfiesVacuum(tuple, TransactionXmin, buffer);
	switch (htsvResult)
	{
		case HEAPTUPLE_LIVE:
			if (visible)
				return;
			xid = HeapTupleHeaderGetXmin(tuple->t_data);
			break;
		case HEAPTUPLE_RECENTLY_DEAD:
		case HEAPTUPLE_DELETE_IN_PROGRESS:
			if (visible)
				xid = HeapTupleHeaderGetUpdateXid(tuple->t_data);
			else
				xid = HeapTupleHeaderGetXmin(tuple->t_data);

			if (TransactionIdPrecedes(xid, TransactionXmin))
			{
				/* This is like the HEAPTUPLE_DEAD case */
				Assert(!visible);
				return;
			}
			break;
		case HEAPTUPLE_INSERT_IN_PROGRESS:
			xid = HeapTupleHeaderGetXmin(tuple->t_data);
			break;
		case HEAPTUPLE_DEAD:
			Assert(!visible);
			return;
		default:

			/*
			 * The only way to get to this default clause is if a new value is
			 * added to the enum type without adding it to this switch
			 * statement.  That's a bug, so elog.
			 */
			elog(ERROR, "unrecognized return value from HeapTupleSatisfiesVacuum: %u", htsvResult);

			/*
			 * In spite of having all enum values covered and calling elog on
			 * this default, some compilers think this is a code path which
			 * allows xid to be used below without initialization. Silence
			 * that warning.
			 */
			xid = InvalidTransactionId;
	}

	Assert(TransactionIdIsValid(xid));
	Assert(TransactionIdFollowsOrEquals(xid, TransactionXmin));

	/*
	 * Find top level xid.  Bail out if xid is too early to be a conflict, or
	 * if it's our own xid.
	 */
	if (TransactionIdEquals(xid, GetTopTransactionIdIfAny()))
		return;
	xid = SubTransGetTopmostTransaction(xid);
	if (TransactionIdPrecedes(xid, TransactionXmin))
		return;

	CheckForSerializableConflictOut(relation, xid, snapshot);
}
