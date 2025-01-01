/*-------------------------------------------------------------------------
 *
 * pruneheap.c
 *	  heap page pruning and HOT-chain management code
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/pruneheap.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/heapam_xlog.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "commands/vacuum.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

/* Working data for heap_page_prune_and_freeze() and subroutines */
typedef struct
{
	/*-------------------------------------------------------
	 * Arguments passed to heap_page_prune_and_freeze()
	 *-------------------------------------------------------
	 */

	/* tuple visibility test, initialized for the relation */
	GlobalVisState *vistest;
	/* whether or not dead items can be set LP_UNUSED during pruning */
	bool		mark_unused_now;
	/* whether to attempt freezing tuples */
	bool		freeze;
	struct VacuumCutoffs *cutoffs;

	/*-------------------------------------------------------
	 * Fields describing what to do to the page
	 *-------------------------------------------------------
	 */
	TransactionId new_prune_xid;	/* new prune hint value */
	TransactionId latest_xid_removed;
	int			nredirected;	/* numbers of entries in arrays below */
	int			ndead;
	int			nunused;
	int			nfrozen;
	/* arrays that accumulate indexes of items to be changed */
	OffsetNumber redirected[MaxHeapTuplesPerPage * 2];
	OffsetNumber nowdead[MaxHeapTuplesPerPage];
	OffsetNumber nowunused[MaxHeapTuplesPerPage];
	HeapTupleFreeze frozen[MaxHeapTuplesPerPage];

	/*-------------------------------------------------------
	 * Working state for HOT chain processing
	 *-------------------------------------------------------
	 */

	/*
	 * 'root_items' contains offsets of all LP_REDIRECT line pointers and
	 * normal non-HOT tuples.  They can be stand-alone items or the first item
	 * in a HOT chain.  'heaponly_items' contains heap-only tuples which can
	 * only be removed as part of a HOT chain.
	 */
	int			nroot_items;
	OffsetNumber root_items[MaxHeapTuplesPerPage];
	int			nheaponly_items;
	OffsetNumber heaponly_items[MaxHeapTuplesPerPage];

	/*
	 * processed[offnum] is true if item at offnum has been processed.
	 *
	 * This needs to be MaxHeapTuplesPerPage + 1 long as FirstOffsetNumber is
	 * 1. Otherwise every access would need to subtract 1.
	 */
	bool		processed[MaxHeapTuplesPerPage + 1];

	/*
	 * Tuple visibility is only computed once for each tuple, for correctness
	 * and efficiency reasons; see comment in heap_page_prune_and_freeze() for
	 * details.  This is of type int8[], instead of HTSV_Result[], so we can
	 * use -1 to indicate no visibility has been computed, e.g. for LP_DEAD
	 * items.
	 *
	 * This needs to be MaxHeapTuplesPerPage + 1 long as FirstOffsetNumber is
	 * 1. Otherwise every access would need to subtract 1.
	 */
	int8		htsv[MaxHeapTuplesPerPage + 1];

	/*
	 * Freezing-related state.
	 */
	HeapPageFreeze pagefrz;

	/*-------------------------------------------------------
	 * Information about what was done
	 *
	 * These fields are not used by pruning itself for the most part, but are
	 * used to collect information about what was pruned and what state the
	 * page is in after pruning, for the benefit of the caller.  They are
	 * copied to the caller's PruneFreezeResult at the end.
	 * -------------------------------------------------------
	 */

	int			ndeleted;		/* Number of tuples deleted from the page */

	/* Number of live and recently dead tuples, after pruning */
	int			live_tuples;
	int			recently_dead_tuples;

	/* Whether or not the page makes rel truncation unsafe */
	bool		hastup;

	/*
	 * LP_DEAD items on the page after pruning.  Includes existing LP_DEAD
	 * items
	 */
	int			lpdead_items;	/* number of items in the array */
	OffsetNumber *deadoffsets;	/* points directly to presult->deadoffsets */

	/*
	 * all_visible and all_frozen indicate if the all-visible and all-frozen
	 * bits in the visibility map can be set for this page after pruning.
	 *
	 * visibility_cutoff_xid is the newest xmin of live tuples on the page.
	 * The caller can use it as the conflict horizon, when setting the VM
	 * bits.  It is only valid if we froze some tuples, and all_frozen is
	 * true.
	 *
	 * NOTE: all_visible and all_frozen don't include LP_DEAD items.  That's
	 * convenient for heap_page_prune_and_freeze(), to use them to decide
	 * whether to freeze the page or not.  The all_visible and all_frozen
	 * values returned to the caller are adjusted to include LP_DEAD items at
	 * the end.
	 *
	 * all_frozen should only be considered valid if all_visible is also set;
	 * we don't bother to clear the all_frozen flag every time we clear the
	 * all_visible flag.
	 */
	bool		all_visible;
	bool		all_frozen;
	TransactionId visibility_cutoff_xid;
} PruneState;

/* Local functions */
static HTSV_Result heap_prune_satisfies_vacuum(PruneState *prstate,
											   HeapTuple tup,
											   Buffer buffer);
static inline HTSV_Result htsv_get_valid_status(int status);
static void heap_prune_chain(Page page, BlockNumber blockno, OffsetNumber maxoff,
							 OffsetNumber rootoffnum, PruneState *prstate);
static void heap_prune_record_prunable(PruneState *prstate, TransactionId xid);
static void heap_prune_record_redirect(PruneState *prstate,
									   OffsetNumber offnum, OffsetNumber rdoffnum,
									   bool was_normal);
static void heap_prune_record_dead(PruneState *prstate, OffsetNumber offnum,
								   bool was_normal);
static void heap_prune_record_dead_or_unused(PruneState *prstate, OffsetNumber offnum,
											 bool was_normal);
static void heap_prune_record_unused(PruneState *prstate, OffsetNumber offnum, bool was_normal);

static void heap_prune_record_unchanged_lp_unused(Page page, PruneState *prstate, OffsetNumber offnum);
static void heap_prune_record_unchanged_lp_normal(Page page, PruneState *prstate, OffsetNumber offnum);
static void heap_prune_record_unchanged_lp_dead(Page page, PruneState *prstate, OffsetNumber offnum);
static void heap_prune_record_unchanged_lp_redirect(PruneState *prstate, OffsetNumber offnum);

static void page_verify_redirects(Page page);


/*
 * Optionally prune and repair fragmentation in the specified page.
 *
 * This is an opportunistic function.  It will perform housekeeping
 * only if the page heuristically looks like a candidate for pruning and we
 * can acquire buffer cleanup lock without blocking.
 *
 * Note: this is called quite often.  It's important that it fall out quickly
 * if there's not any use in pruning.
 *
 * Caller must have pin on the buffer, and must *not* have a lock on it.
 */
void
heap_page_prune_opt(Relation relation, Buffer buffer)
{
	Page		page = BufferGetPage(buffer);
	TransactionId prune_xid;
	GlobalVisState *vistest;
	Size		minfree;

	/*
	 * We can't write WAL in recovery mode, so there's no point trying to
	 * clean the page. The primary will likely issue a cleaning WAL record
	 * soon anyway, so this is no particular loss.
	 */
	if (RecoveryInProgress())
		return;

	/*
	 * First check whether there's any chance there's something to prune,
	 * determining the appropriate horizon is a waste if there's no prune_xid
	 * (i.e. no updates/deletes left potentially dead tuples around).
	 */
	prune_xid = ((PageHeader) page)->pd_prune_xid;
	if (!TransactionIdIsValid(prune_xid))
		return;

	/*
	 * Check whether prune_xid indicates that there may be dead rows that can
	 * be cleaned up.
	 */
	vistest = GlobalVisTestFor(relation);

	if (!GlobalVisTestIsRemovableXid(vistest, prune_xid))
		return;

	/*
	 * We prune when a previous UPDATE failed to find enough space on the page
	 * for a new tuple version, or when free space falls below the relation's
	 * fill-factor target (but not less than 10%).
	 *
	 * Checking free space here is questionable since we aren't holding any
	 * lock on the buffer; in the worst case we could get a bogus answer. It's
	 * unlikely to be *seriously* wrong, though, since reading either pd_lower
	 * or pd_upper is probably atomic.  Avoiding taking a lock seems more
	 * important than sometimes getting a wrong answer in what is after all
	 * just a heuristic estimate.
	 */
	minfree = RelationGetTargetPageFreeSpace(relation,
											 HEAP_DEFAULT_FILLFACTOR);
	minfree = Max(minfree, BLCKSZ / 10);

	if (PageIsFull(page) || PageGetHeapFreeSpace(page) < minfree)
	{
		/* OK, try to get exclusive buffer lock */
		if (!ConditionalLockBufferForCleanup(buffer))
			return;

		/*
		 * Now that we have buffer lock, get accurate information about the
		 * page's free space, and recheck the heuristic about whether to
		 * prune.
		 */
		if (PageIsFull(page) || PageGetHeapFreeSpace(page) < minfree)
		{
			OffsetNumber dummy_off_loc;
			PruneFreezeResult presult;

			/*
			 * For now, pass mark_unused_now as false regardless of whether or
			 * not the relation has indexes, since we cannot safely determine
			 * that during on-access pruning with the current implementation.
			 */
			heap_page_prune_and_freeze(relation, buffer, vistest, 0,
									   NULL, &presult, PRUNE_ON_ACCESS, &dummy_off_loc, NULL, NULL);

			/*
			 * Report the number of tuples reclaimed to pgstats.  This is
			 * presult.ndeleted minus the number of newly-LP_DEAD-set items.
			 *
			 * We derive the number of dead tuples like this to avoid totally
			 * forgetting about items that were set to LP_DEAD, since they
			 * still need to be cleaned up by VACUUM.  We only want to count
			 * heap-only tuples that just became LP_UNUSED in our report,
			 * which don't.
			 *
			 * VACUUM doesn't have to compensate in the same way when it
			 * tracks ndeleted, since it will set the same LP_DEAD items to
			 * LP_UNUSED separately.
			 */
			if (presult.ndeleted > presult.nnewlpdead)
				pgstat_update_heap_dead_tuples(relation,
											   presult.ndeleted - presult.nnewlpdead);
		}

		/* And release buffer lock */
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

		/*
		 * We avoid reuse of any free space created on the page by unrelated
		 * UPDATEs/INSERTs by opting to not update the FSM at this point.  The
		 * free space should be reused by UPDATEs to *this* page.
		 */
	}
}


/*
 * Prune and repair fragmentation and potentially freeze tuples on the
 * specified page.
 *
 * Caller must have pin and buffer cleanup lock on the page.  Note that we
 * don't update the FSM information for page on caller's behalf.  Caller might
 * also need to account for a reduction in the length of the line pointer
 * array following array truncation by us.
 *
 * If the HEAP_PRUNE_FREEZE option is set, we will also freeze tuples if it's
 * required in order to advance relfrozenxid / relminmxid, or if it's
 * considered advantageous for overall system performance to do so now.  The
 * 'cutoffs', 'presult', 'new_relfrozen_xid' and 'new_relmin_mxid' arguments
 * are required when freezing.  When HEAP_PRUNE_FREEZE option is set, we also
 * set presult->all_visible and presult->all_frozen on exit, to indicate if
 * the VM bits can be set.  They are always set to false when the
 * HEAP_PRUNE_FREEZE option is not set, because at the moment only callers
 * that also freeze need that information.
 *
 * vistest is used to distinguish whether tuples are DEAD or RECENTLY_DEAD
 * (see heap_prune_satisfies_vacuum).
 *
 * options:
 *   MARK_UNUSED_NOW indicates that dead items can be set LP_UNUSED during
 *   pruning.
 *
 *   FREEZE indicates that we will also freeze tuples, and will return
 *   'all_visible', 'all_frozen' flags to the caller.
 *
 * cutoffs contains the freeze cutoffs, established by VACUUM at the beginning
 * of vacuuming the relation.  Required if HEAP_PRUNE_FREEZE option is set.
 * cutoffs->OldestXmin is also used to determine if dead tuples are
 * HEAPTUPLE_RECENTLY_DEAD or HEAPTUPLE_DEAD.
 *
 * presult contains output parameters needed by callers, such as the number of
 * tuples removed and the offsets of dead items on the page after pruning.
 * heap_page_prune_and_freeze() is responsible for initializing it.  Required
 * by all callers.
 *
 * reason indicates why the pruning is performed.  It is included in the WAL
 * record for debugging and analysis purposes, but otherwise has no effect.
 *
 * off_loc is the offset location required by the caller to use in error
 * callback.
 *
 * new_relfrozen_xid and new_relmin_mxid must provided by the caller if the
 * HEAP_PRUNE_FREEZE option is set.  On entry, they contain the oldest XID and
 * multi-XID seen on the relation so far.  They will be updated with oldest
 * values present on the page after pruning.  After processing the whole
 * relation, VACUUM can use these values as the new relfrozenxid/relminmxid
 * for the relation.
 */
void
heap_page_prune_and_freeze(Relation relation, Buffer buffer,
						   GlobalVisState *vistest,
						   int options,
						   struct VacuumCutoffs *cutoffs,
						   PruneFreezeResult *presult,
						   PruneReason reason,
						   OffsetNumber *off_loc,
						   TransactionId *new_relfrozen_xid,
						   MultiXactId *new_relmin_mxid)
{
	Page		page = BufferGetPage(buffer);
	BlockNumber blockno = BufferGetBlockNumber(buffer);
	OffsetNumber offnum,
				maxoff;
	PruneState	prstate;
	HeapTupleData tup;
	bool		do_freeze;
	bool		do_prune;
	bool		do_hint;
	bool		hint_bit_fpi;
	int64		fpi_before = pgWalUsage.wal_fpi;

	/* Copy parameters to prstate */
	prstate.vistest = vistest;
	prstate.mark_unused_now = (options & HEAP_PAGE_PRUNE_MARK_UNUSED_NOW) != 0;
	prstate.freeze = (options & HEAP_PAGE_PRUNE_FREEZE) != 0;
	prstate.cutoffs = cutoffs;

	/*
	 * Our strategy is to scan the page and make lists of items to change,
	 * then apply the changes within a critical section.  This keeps as much
	 * logic as possible out of the critical section, and also ensures that
	 * WAL replay will work the same as the normal case.
	 *
	 * First, initialize the new pd_prune_xid value to zero (indicating no
	 * prunable tuples).  If we find any tuples which may soon become
	 * prunable, we will save the lowest relevant XID in new_prune_xid. Also
	 * initialize the rest of our working state.
	 */
	prstate.new_prune_xid = InvalidTransactionId;
	prstate.latest_xid_removed = InvalidTransactionId;
	prstate.nredirected = prstate.ndead = prstate.nunused = prstate.nfrozen = 0;
	prstate.nroot_items = 0;
	prstate.nheaponly_items = 0;

	/* initialize page freezing working state */
	prstate.pagefrz.freeze_required = false;
	if (prstate.freeze)
	{
		Assert(new_relfrozen_xid && new_relmin_mxid);
		prstate.pagefrz.FreezePageRelfrozenXid = *new_relfrozen_xid;
		prstate.pagefrz.NoFreezePageRelfrozenXid = *new_relfrozen_xid;
		prstate.pagefrz.FreezePageRelminMxid = *new_relmin_mxid;
		prstate.pagefrz.NoFreezePageRelminMxid = *new_relmin_mxid;
	}
	else
	{
		Assert(new_relfrozen_xid == NULL && new_relmin_mxid == NULL);
		prstate.pagefrz.FreezePageRelminMxid = InvalidMultiXactId;
		prstate.pagefrz.NoFreezePageRelminMxid = InvalidMultiXactId;
		prstate.pagefrz.FreezePageRelfrozenXid = InvalidTransactionId;
		prstate.pagefrz.NoFreezePageRelfrozenXid = InvalidTransactionId;
	}

	prstate.ndeleted = 0;
	prstate.live_tuples = 0;
	prstate.recently_dead_tuples = 0;
	prstate.hastup = false;
	prstate.lpdead_items = 0;
	prstate.deadoffsets = presult->deadoffsets;

	/*
	 * Caller may update the VM after we're done.  We can keep track of
	 * whether the page will be all-visible and all-frozen after pruning and
	 * freezing to help the caller to do that.
	 *
	 * Currently, only VACUUM sets the VM bits.  To save the effort, only do
	 * the bookkeeping if the caller needs it.  Currently, that's tied to
	 * HEAP_PAGE_PRUNE_FREEZE, but it could be a separate flag if you wanted
	 * to update the VM bits without also freezing or freeze without also
	 * setting the VM bits.
	 *
	 * In addition to telling the caller whether it can set the VM bit, we
	 * also use 'all_visible' and 'all_frozen' for our own decision-making. If
	 * the whole page would become frozen, we consider opportunistically
	 * freezing tuples.  We will not be able to freeze the whole page if there
	 * are tuples present that are not visible to everyone or if there are
	 * dead tuples which are not yet removable.  However, dead tuples which
	 * will be removed by the end of vacuuming should not preclude us from
	 * opportunistically freezing.  Because of that, we do not clear
	 * all_visible when we see LP_DEAD items.  We fix that at the end of the
	 * function, when we return the value to the caller, so that the caller
	 * doesn't set the VM bit incorrectly.
	 */
	if (prstate.freeze)
	{
		prstate.all_visible = true;
		prstate.all_frozen = true;
	}
	else
	{
		/*
		 * Initializing to false allows skipping the work to update them in
		 * heap_prune_record_unchanged_lp_normal().
		 */
		prstate.all_visible = false;
		prstate.all_frozen = false;
	}

	/*
	 * The visibility cutoff xid is the newest xmin of live tuples on the
	 * page.  In the common case, this will be set as the conflict horizon the
	 * caller can use for updating the VM.  If, at the end of freezing and
	 * pruning, the page is all-frozen, there is no possibility that any
	 * running transaction on the standby does not see tuples on the page as
	 * all-visible, so the conflict horizon remains InvalidTransactionId.
	 */
	prstate.visibility_cutoff_xid = InvalidTransactionId;

	maxoff = PageGetMaxOffsetNumber(page);
	tup.t_tableOid = RelationGetRelid(relation);

	/*
	 * Determine HTSV for all tuples, and queue them up for processing as HOT
	 * chain roots or as heap-only items.
	 *
	 * Determining HTSV only once for each tuple is required for correctness,
	 * to deal with cases where running HTSV twice could result in different
	 * results.  For example, RECENTLY_DEAD can turn to DEAD if another
	 * checked item causes GlobalVisTestIsRemovableFullXid() to update the
	 * horizon, or INSERT_IN_PROGRESS can change to DEAD if the inserting
	 * transaction aborts.
	 *
	 * It's also good for performance. Most commonly tuples within a page are
	 * stored at decreasing offsets (while the items are stored at increasing
	 * offsets). When processing all tuples on a page this leads to reading
	 * memory at decreasing offsets within a page, with a variable stride.
	 * That's hard for CPU prefetchers to deal with. Processing the items in
	 * reverse order (and thus the tuples in increasing order) increases
	 * prefetching efficiency significantly / decreases the number of cache
	 * misses.
	 */
	for (offnum = maxoff;
		 offnum >= FirstOffsetNumber;
		 offnum = OffsetNumberPrev(offnum))
	{
		ItemId		itemid = PageGetItemId(page, offnum);
		HeapTupleHeader htup;

		/*
		 * Set the offset number so that we can display it along with any
		 * error that occurred while processing this tuple.
		 */
		*off_loc = offnum;

		prstate.processed[offnum] = false;
		prstate.htsv[offnum] = -1;

		/* Nothing to do if slot doesn't contain a tuple */
		if (!ItemIdIsUsed(itemid))
		{
			heap_prune_record_unchanged_lp_unused(page, &prstate, offnum);
			continue;
		}

		if (ItemIdIsDead(itemid))
		{
			/*
			 * If the caller set mark_unused_now true, we can set dead line
			 * pointers LP_UNUSED now.
			 */
			if (unlikely(prstate.mark_unused_now))
				heap_prune_record_unused(&prstate, offnum, false);
			else
				heap_prune_record_unchanged_lp_dead(page, &prstate, offnum);
			continue;
		}

		if (ItemIdIsRedirected(itemid))
		{
			/* This is the start of a HOT chain */
			prstate.root_items[prstate.nroot_items++] = offnum;
			continue;
		}

		Assert(ItemIdIsNormal(itemid));

		/*
		 * Get the tuple's visibility status and queue it up for processing.
		 */
		htup = (HeapTupleHeader) PageGetItem(page, itemid);
		tup.t_data = htup;
		tup.t_len = ItemIdGetLength(itemid);
		ItemPointerSet(&tup.t_self, blockno, offnum);

		prstate.htsv[offnum] = heap_prune_satisfies_vacuum(&prstate, &tup,
														   buffer);

		if (!HeapTupleHeaderIsHeapOnly(htup))
			prstate.root_items[prstate.nroot_items++] = offnum;
		else
			prstate.heaponly_items[prstate.nheaponly_items++] = offnum;
	}

	/*
	 * If checksums are enabled, heap_prune_satisfies_vacuum() may have caused
	 * an FPI to be emitted.
	 */
	hint_bit_fpi = fpi_before != pgWalUsage.wal_fpi;

	/*
	 * Process HOT chains.
	 *
	 * We added the items to the array starting from 'maxoff', so by
	 * processing the array in reverse order, we process the items in
	 * ascending offset number order.  The order doesn't matter for
	 * correctness, but some quick micro-benchmarking suggests that this is
	 * faster.  (Earlier PostgreSQL versions, which scanned all the items on
	 * the page instead of using the root_items array, also did it in
	 * ascending offset number order.)
	 */
	for (int i = prstate.nroot_items - 1; i >= 0; i--)
	{
		offnum = prstate.root_items[i];

		/* Ignore items already processed as part of an earlier chain */
		if (prstate.processed[offnum])
			continue;

		/* see preceding loop */
		*off_loc = offnum;

		/* Process this item or chain of items */
		heap_prune_chain(page, blockno, maxoff, offnum, &prstate);
	}

	/*
	 * Process any heap-only tuples that were not already processed as part of
	 * a HOT chain.
	 */
	for (int i = prstate.nheaponly_items - 1; i >= 0; i--)
	{
		offnum = prstate.heaponly_items[i];

		if (prstate.processed[offnum])
			continue;

		/* see preceding loop */
		*off_loc = offnum;

		/*
		 * If the tuple is DEAD and doesn't chain to anything else, mark it
		 * unused.  (If it does chain, we can only remove it as part of
		 * pruning its chain.)
		 *
		 * We need this primarily to handle aborted HOT updates, that is,
		 * XMIN_INVALID heap-only tuples.  Those might not be linked to by any
		 * chain, since the parent tuple might be re-updated before any
		 * pruning occurs.  So we have to be able to reap them separately from
		 * chain-pruning.  (Note that HeapTupleHeaderIsHotUpdated will never
		 * return true for an XMIN_INVALID tuple, so this code will work even
		 * when there were sequential updates within the aborted transaction.)
		 */
		if (prstate.htsv[offnum] == HEAPTUPLE_DEAD)
		{
			ItemId		itemid = PageGetItemId(page, offnum);
			HeapTupleHeader htup = (HeapTupleHeader) PageGetItem(page, itemid);

			if (likely(!HeapTupleHeaderIsHotUpdated(htup)))
			{
				HeapTupleHeaderAdvanceConflictHorizon(htup,
													  &prstate.latest_xid_removed);
				heap_prune_record_unused(&prstate, offnum, true);
			}
			else
			{
				/*
				 * This tuple should've been processed and removed as part of
				 * a HOT chain, so something's wrong.  To preserve evidence,
				 * we don't dare to remove it.  We cannot leave behind a DEAD
				 * tuple either, because that will cause VACUUM to error out.
				 * Throwing an error with a distinct error message seems like
				 * the least bad option.
				 */
				elog(ERROR, "dead heap-only tuple (%u, %d) is not linked to from any HOT chain",
					 blockno, offnum);
			}
		}
		else
			heap_prune_record_unchanged_lp_normal(page, &prstate, offnum);
	}

	/* We should now have processed every tuple exactly once  */
#ifdef USE_ASSERT_CHECKING
	for (offnum = FirstOffsetNumber;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		*off_loc = offnum;

		Assert(prstate.processed[offnum]);
	}
#endif

	/* Clear the offset information once we have processed the given page. */
	*off_loc = InvalidOffsetNumber;

	do_prune = prstate.nredirected > 0 ||
		prstate.ndead > 0 ||
		prstate.nunused > 0;

	/*
	 * Even if we don't prune anything, if we found a new value for the
	 * pd_prune_xid field or the page was marked full, we will update the hint
	 * bit.
	 */
	do_hint = ((PageHeader) page)->pd_prune_xid != prstate.new_prune_xid ||
		PageIsFull(page);

	/*
	 * Decide if we want to go ahead with freezing according to the freeze
	 * plans we prepared, or not.
	 */
	do_freeze = false;
	if (prstate.freeze)
	{
		if (prstate.pagefrz.freeze_required)
		{
			/*
			 * heap_prepare_freeze_tuple indicated that at least one XID/MXID
			 * from before FreezeLimit/MultiXactCutoff is present.  Must
			 * freeze to advance relfrozenxid/relminmxid.
			 */
			do_freeze = true;
		}
		else
		{
			/*
			 * Opportunistically freeze the page if we are generating an FPI
			 * anyway and if doing so means that we can set the page
			 * all-frozen afterwards (might not happen until VACUUM's final
			 * heap pass).
			 *
			 * XXX: Previously, we knew if pruning emitted an FPI by checking
			 * pgWalUsage.wal_fpi before and after pruning.  Once the freeze
			 * and prune records were combined, this heuristic couldn't be
			 * used anymore.  The opportunistic freeze heuristic must be
			 * improved; however, for now, try to approximate the old logic.
			 */
			if (prstate.all_visible && prstate.all_frozen && prstate.nfrozen > 0)
			{
				/*
				 * Freezing would make the page all-frozen.  Have already
				 * emitted an FPI or will do so anyway?
				 */
				if (RelationNeedsWAL(relation))
				{
					if (hint_bit_fpi)
						do_freeze = true;
					else if (do_prune)
					{
						if (XLogCheckBufferNeedsBackup(buffer))
							do_freeze = true;
					}
					else if (do_hint)
					{
						if (XLogHintBitIsNeeded() && XLogCheckBufferNeedsBackup(buffer))
							do_freeze = true;
					}
				}
			}
		}
	}

	if (do_freeze)
	{
		/*
		 * Validate the tuples we will be freezing before entering the
		 * critical section.
		 */
		heap_pre_freeze_checks(buffer, prstate.frozen, prstate.nfrozen);
	}
	else if (prstate.nfrozen > 0)
	{
		/*
		 * The page contained some tuples that were not already frozen, and we
		 * chose not to freeze them now.  The page won't be all-frozen then.
		 */
		Assert(!prstate.pagefrz.freeze_required);

		prstate.all_frozen = false;
		prstate.nfrozen = 0;	/* avoid miscounts in instrumentation */
	}
	else
	{
		/*
		 * We have no freeze plans to execute.  The page might already be
		 * all-frozen (perhaps only following pruning), though.  Such pages
		 * can be marked all-frozen in the VM by our caller, even though none
		 * of its tuples were newly frozen here.
		 */
	}

	/* Any error while applying the changes is critical */
	START_CRIT_SECTION();

	if (do_hint)
	{
		/*
		 * Update the page's pd_prune_xid field to either zero, or the lowest
		 * XID of any soon-prunable tuple.
		 */
		((PageHeader) page)->pd_prune_xid = prstate.new_prune_xid;

		/*
		 * Also clear the "page is full" flag, since there's no point in
		 * repeating the prune/defrag process until something else happens to
		 * the page.
		 */
		PageClearFull(page);

		/*
		 * If that's all we had to do to the page, this is a non-WAL-logged
		 * hint.  If we are going to freeze or prune the page, we will mark
		 * the buffer dirty below.
		 */
		if (!do_freeze && !do_prune)
			MarkBufferDirtyHint(buffer, true);
	}

	if (do_prune || do_freeze)
	{
		/* Apply the planned item changes and repair page fragmentation. */
		if (do_prune)
		{
			heap_page_prune_execute(buffer, false,
									prstate.redirected, prstate.nredirected,
									prstate.nowdead, prstate.ndead,
									prstate.nowunused, prstate.nunused);
		}

		if (do_freeze)
			heap_freeze_prepared_tuples(buffer, prstate.frozen, prstate.nfrozen);

		MarkBufferDirty(buffer);

		/*
		 * Emit a WAL XLOG_HEAP2_PRUNE_FREEZE record showing what we did
		 */
		if (RelationNeedsWAL(relation))
		{
			/*
			 * The snapshotConflictHorizon for the whole record should be the
			 * most conservative of all the horizons calculated for any of the
			 * possible modifications.  If this record will prune tuples, any
			 * transactions on the standby older than the youngest xmax of the
			 * most recently removed tuple this record will prune will
			 * conflict.  If this record will freeze tuples, any transactions
			 * on the standby with xids older than the youngest tuple this
			 * record will freeze will conflict.
			 */
			TransactionId frz_conflict_horizon = InvalidTransactionId;
			TransactionId conflict_xid;

			/*
			 * We can use the visibility_cutoff_xid as our cutoff for
			 * conflicts when the whole page is eligible to become all-frozen
			 * in the VM once we're done with it.  Otherwise we generate a
			 * conservative cutoff by stepping back from OldestXmin.
			 */
			if (do_freeze)
			{
				if (prstate.all_visible && prstate.all_frozen)
					frz_conflict_horizon = prstate.visibility_cutoff_xid;
				else
				{
					/* Avoids false conflicts when hot_standby_feedback in use */
					frz_conflict_horizon = prstate.cutoffs->OldestXmin;
					TransactionIdRetreat(frz_conflict_horizon);
				}
			}

			if (TransactionIdFollows(frz_conflict_horizon, prstate.latest_xid_removed))
				conflict_xid = frz_conflict_horizon;
			else
				conflict_xid = prstate.latest_xid_removed;

			log_heap_prune_and_freeze(relation, buffer,
									  conflict_xid,
									  true, reason,
									  prstate.frozen, prstate.nfrozen,
									  prstate.redirected, prstate.nredirected,
									  prstate.nowdead, prstate.ndead,
									  prstate.nowunused, prstate.nunused);
		}
	}

	END_CRIT_SECTION();

	/* Copy information back for caller */
	presult->ndeleted = prstate.ndeleted;
	presult->nnewlpdead = prstate.ndead;
	presult->nfrozen = prstate.nfrozen;
	presult->live_tuples = prstate.live_tuples;
	presult->recently_dead_tuples = prstate.recently_dead_tuples;

	/*
	 * It was convenient to ignore LP_DEAD items in all_visible earlier on to
	 * make the choice of whether or not to freeze the page unaffected by the
	 * short-term presence of LP_DEAD items.  These LP_DEAD items were
	 * effectively assumed to be LP_UNUSED items in the making.  It doesn't
	 * matter which vacuum heap pass (initial pass or final pass) ends up
	 * setting the page all-frozen, as long as the ongoing VACUUM does it.
	 *
	 * Now that freezing has been finalized, unset all_visible if there are
	 * any LP_DEAD items on the page.  It needs to reflect the present state
	 * of the page, as expected by our caller.
	 */
	if (prstate.all_visible && prstate.lpdead_items == 0)
	{
		presult->all_visible = prstate.all_visible;
		presult->all_frozen = prstate.all_frozen;
	}
	else
	{
		presult->all_visible = false;
		presult->all_frozen = false;
	}

	presult->hastup = prstate.hastup;

	/*
	 * For callers planning to update the visibility map, the conflict horizon
	 * for that record must be the newest xmin on the page.  However, if the
	 * page is completely frozen, there can be no conflict and the
	 * vm_conflict_horizon should remain InvalidTransactionId.  This includes
	 * the case that we just froze all the tuples; the prune-freeze record
	 * included the conflict XID already so the caller doesn't need it.
	 */
	if (presult->all_frozen)
		presult->vm_conflict_horizon = InvalidTransactionId;
	else
		presult->vm_conflict_horizon = prstate.visibility_cutoff_xid;

	presult->lpdead_items = prstate.lpdead_items;
	/* the presult->deadoffsets array was already filled in */

	if (prstate.freeze)
	{
		if (presult->nfrozen > 0)
		{
			*new_relfrozen_xid = prstate.pagefrz.FreezePageRelfrozenXid;
			*new_relmin_mxid = prstate.pagefrz.FreezePageRelminMxid;
		}
		else
		{
			*new_relfrozen_xid = prstate.pagefrz.NoFreezePageRelfrozenXid;
			*new_relmin_mxid = prstate.pagefrz.NoFreezePageRelminMxid;
		}
	}
}


/*
 * Perform visibility checks for heap pruning.
 */
static HTSV_Result
heap_prune_satisfies_vacuum(PruneState *prstate, HeapTuple tup, Buffer buffer)
{
	HTSV_Result res;
	TransactionId dead_after;

	res = HeapTupleSatisfiesVacuumHorizon(tup, buffer, &dead_after);

	if (res != HEAPTUPLE_RECENTLY_DEAD)
		return res;

	/*
	 * For VACUUM, we must be sure to prune tuples with xmax older than
	 * OldestXmin -- a visibility cutoff determined at the beginning of
	 * vacuuming the relation. OldestXmin is used for freezing determination
	 * and we cannot freeze dead tuples' xmaxes.
	 */
	if (prstate->cutoffs &&
		TransactionIdIsValid(prstate->cutoffs->OldestXmin) &&
		NormalTransactionIdPrecedes(dead_after, prstate->cutoffs->OldestXmin))
		return HEAPTUPLE_DEAD;

	/*
	 * Determine whether or not the tuple is considered dead when compared
	 * with the provided GlobalVisState. On-access pruning does not provide
	 * VacuumCutoffs. And for vacuum, even if the tuple's xmax is not older
	 * than OldestXmin, GlobalVisTestIsRemovableXid() could find the row dead
	 * if the GlobalVisState has been updated since the beginning of vacuuming
	 * the relation.
	 */
	if (GlobalVisTestIsRemovableXid(prstate->vistest, dead_after))
		return HEAPTUPLE_DEAD;

	return res;
}


/*
 * Pruning calculates tuple visibility once and saves the results in an array
 * of int8.  See PruneState.htsv for details.  This helper function is meant
 * to guard against examining visibility status array members which have not
 * yet been computed.
 */
static inline HTSV_Result
htsv_get_valid_status(int status)
{
	Assert(status >= HEAPTUPLE_DEAD &&
		   status <= HEAPTUPLE_DELETE_IN_PROGRESS);
	return (HTSV_Result) status;
}

/*
 * Prune specified line pointer or a HOT chain originating at line pointer.
 *
 * Tuple visibility information is provided in prstate->htsv.
 *
 * If the item is an index-referenced tuple (i.e. not a heap-only tuple),
 * the HOT chain is pruned by removing all DEAD tuples at the start of the HOT
 * chain.  We also prune any RECENTLY_DEAD tuples preceding a DEAD tuple.
 * This is OK because a RECENTLY_DEAD tuple preceding a DEAD tuple is really
 * DEAD, our visibility test is just too coarse to detect it.
 *
 * Pruning must never leave behind a DEAD tuple that still has tuple storage.
 * VACUUM isn't prepared to deal with that case.
 *
 * The root line pointer is redirected to the tuple immediately after the
 * latest DEAD tuple.  If all tuples in the chain are DEAD, the root line
 * pointer is marked LP_DEAD.  (This includes the case of a DEAD simple
 * tuple, which we treat as a chain of length 1.)
 *
 * We don't actually change the page here. We just add entries to the arrays in
 * prstate showing the changes to be made.  Items to be redirected are added
 * to the redirected[] array (two entries per redirection); items to be set to
 * LP_DEAD state are added to nowdead[]; and items to be set to LP_UNUSED
 * state are added to nowunused[].  We perform bookkeeping of live tuples,
 * visibility etc. based on what the page will look like after the changes
 * applied.  All that bookkeeping is performed in the heap_prune_record_*()
 * subroutines.  The division of labor is that heap_prune_chain() decides the
 * fate of each tuple, ie. whether it's going to be removed, redirected or
 * left unchanged, and the heap_prune_record_*() subroutines update PruneState
 * based on that outcome.
 */
static void
heap_prune_chain(Page page, BlockNumber blockno, OffsetNumber maxoff,
				 OffsetNumber rootoffnum, PruneState *prstate)
{
	TransactionId priorXmax = InvalidTransactionId;
	ItemId		rootlp;
	OffsetNumber offnum;
	OffsetNumber chainitems[MaxHeapTuplesPerPage];

	/*
	 * After traversing the HOT chain, ndeadchain is the index in chainitems
	 * of the first live successor after the last dead item.
	 */
	int			ndeadchain = 0,
				nchain = 0;

	rootlp = PageGetItemId(page, rootoffnum);

	/* Start from the root tuple */
	offnum = rootoffnum;

	/* while not end of the chain */
	for (;;)
	{
		HeapTupleHeader htup;
		ItemId		lp;

		/* Sanity check (pure paranoia) */
		if (offnum < FirstOffsetNumber)
			break;

		/*
		 * An offset past the end of page's line pointer array is possible
		 * when the array was truncated (original item must have been unused)
		 */
		if (offnum > maxoff)
			break;

		/* If item is already processed, stop --- it must not be same chain */
		if (prstate->processed[offnum])
			break;

		lp = PageGetItemId(page, offnum);

		/*
		 * Unused item obviously isn't part of the chain. Likewise, a dead
		 * line pointer can't be part of the chain.  Both of those cases were
		 * already marked as processed.
		 */
		Assert(ItemIdIsUsed(lp));
		Assert(!ItemIdIsDead(lp));

		/*
		 * If we are looking at the redirected root line pointer, jump to the
		 * first normal tuple in the chain.  If we find a redirect somewhere
		 * else, stop --- it must not be same chain.
		 */
		if (ItemIdIsRedirected(lp))
		{
			if (nchain > 0)
				break;			/* not at start of chain */
			chainitems[nchain++] = offnum;
			offnum = ItemIdGetRedirect(rootlp);
			continue;
		}

		Assert(ItemIdIsNormal(lp));

		htup = (HeapTupleHeader) PageGetItem(page, lp);

		/*
		 * Check the tuple XMIN against prior XMAX, if any
		 */
		if (TransactionIdIsValid(priorXmax) &&
			!TransactionIdEquals(HeapTupleHeaderGetXmin(htup), priorXmax))
			break;

		/*
		 * OK, this tuple is indeed a member of the chain.
		 */
		chainitems[nchain++] = offnum;

		switch (htsv_get_valid_status(prstate->htsv[offnum]))
		{
			case HEAPTUPLE_DEAD:

				/* Remember the last DEAD tuple seen */
				ndeadchain = nchain;
				HeapTupleHeaderAdvanceConflictHorizon(htup,
													  &prstate->latest_xid_removed);
				/* Advance to next chain member */
				break;

			case HEAPTUPLE_RECENTLY_DEAD:

				/*
				 * We don't need to advance the conflict horizon for
				 * RECENTLY_DEAD tuples, even if we are removing them.  This
				 * is because we only remove RECENTLY_DEAD tuples if they
				 * precede a DEAD tuple, and the DEAD tuple must have been
				 * inserted by a newer transaction than the RECENTLY_DEAD
				 * tuple by virtue of being later in the chain.  We will have
				 * advanced the conflict horizon for the DEAD tuple.
				 */

				/*
				 * Advance past RECENTLY_DEAD tuples just in case there's a
				 * DEAD one after them.  We have to make sure that we don't
				 * miss any DEAD tuples, since DEAD tuples that still have
				 * tuple storage after pruning will confuse VACUUM.
				 */
				break;

			case HEAPTUPLE_DELETE_IN_PROGRESS:
			case HEAPTUPLE_LIVE:
			case HEAPTUPLE_INSERT_IN_PROGRESS:
				goto process_chain;

			default:
				elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
				goto process_chain;
		}

		/*
		 * If the tuple is not HOT-updated, then we are at the end of this
		 * HOT-update chain.
		 */
		if (!HeapTupleHeaderIsHotUpdated(htup))
			goto process_chain;

		/* HOT implies it can't have moved to different partition */
		Assert(!HeapTupleHeaderIndicatesMovedPartitions(htup));

		/*
		 * Advance to next chain member.
		 */
		Assert(ItemPointerGetBlockNumber(&htup->t_ctid) == blockno);
		offnum = ItemPointerGetOffsetNumber(&htup->t_ctid);
		priorXmax = HeapTupleHeaderGetUpdateXid(htup);
	}

	if (ItemIdIsRedirected(rootlp) && nchain < 2)
	{
		/*
		 * We found a redirect item that doesn't point to a valid follow-on
		 * item.  This can happen if the loop in heap_page_prune_and_freeze()
		 * caused us to visit the dead successor of a redirect item before
		 * visiting the redirect item.  We can clean up by setting the
		 * redirect item to LP_DEAD state or LP_UNUSED if the caller
		 * indicated.
		 */
		heap_prune_record_dead_or_unused(prstate, rootoffnum, false);
		return;
	}

process_chain:

	if (ndeadchain == 0)
	{
		/*
		 * No DEAD tuple was found, so the chain is entirely composed of
		 * normal, unchanged tuples.  Leave it alone.
		 */
		int			i = 0;

		if (ItemIdIsRedirected(rootlp))
		{
			heap_prune_record_unchanged_lp_redirect(prstate, rootoffnum);
			i++;
		}
		for (; i < nchain; i++)
			heap_prune_record_unchanged_lp_normal(page, prstate, chainitems[i]);
	}
	else if (ndeadchain == nchain)
	{
		/*
		 * The entire chain is dead.  Mark the root line pointer LP_DEAD, and
		 * fully remove the other tuples in the chain.
		 */
		heap_prune_record_dead_or_unused(prstate, rootoffnum, ItemIdIsNormal(rootlp));
		for (int i = 1; i < nchain; i++)
			heap_prune_record_unused(prstate, chainitems[i], true);
	}
	else
	{
		/*
		 * We found a DEAD tuple in the chain.  Redirect the root line pointer
		 * to the first non-DEAD tuple, and mark as unused each intermediate
		 * item that we are able to remove from the chain.
		 */
		heap_prune_record_redirect(prstate, rootoffnum, chainitems[ndeadchain],
								   ItemIdIsNormal(rootlp));
		for (int i = 1; i < ndeadchain; i++)
			heap_prune_record_unused(prstate, chainitems[i], true);

		/* the rest of tuples in the chain are normal, unchanged tuples */
		for (int i = ndeadchain; i < nchain; i++)
			heap_prune_record_unchanged_lp_normal(page, prstate, chainitems[i]);
	}
}

/* Record lowest soon-prunable XID */
static void
heap_prune_record_prunable(PruneState *prstate, TransactionId xid)
{
	/*
	 * This should exactly match the PageSetPrunable macro.  We can't store
	 * directly into the page header yet, so we update working state.
	 */
	Assert(TransactionIdIsNormal(xid));
	if (!TransactionIdIsValid(prstate->new_prune_xid) ||
		TransactionIdPrecedes(xid, prstate->new_prune_xid))
		prstate->new_prune_xid = xid;
}

/* Record line pointer to be redirected */
static void
heap_prune_record_redirect(PruneState *prstate,
						   OffsetNumber offnum, OffsetNumber rdoffnum,
						   bool was_normal)
{
	Assert(!prstate->processed[offnum]);
	prstate->processed[offnum] = true;

	/*
	 * Do not mark the redirect target here.  It needs to be counted
	 * separately as an unchanged tuple.
	 */

	Assert(prstate->nredirected < MaxHeapTuplesPerPage);
	prstate->redirected[prstate->nredirected * 2] = offnum;
	prstate->redirected[prstate->nredirected * 2 + 1] = rdoffnum;

	prstate->nredirected++;

	/*
	 * If the root entry had been a normal tuple, we are deleting it, so count
	 * it in the result.  But changing a redirect (even to DEAD state) doesn't
	 * count.
	 */
	if (was_normal)
		prstate->ndeleted++;

	prstate->hastup = true;
}

/* Record line pointer to be marked dead */
static void
heap_prune_record_dead(PruneState *prstate, OffsetNumber offnum,
					   bool was_normal)
{
	Assert(!prstate->processed[offnum]);
	prstate->processed[offnum] = true;

	Assert(prstate->ndead < MaxHeapTuplesPerPage);
	prstate->nowdead[prstate->ndead] = offnum;
	prstate->ndead++;

	/*
	 * Deliberately delay unsetting all_visible until later during pruning.
	 * Removable dead tuples shouldn't preclude freezing the page.
	 */

	/* Record the dead offset for vacuum */
	prstate->deadoffsets[prstate->lpdead_items++] = offnum;

	/*
	 * If the root entry had been a normal tuple, we are deleting it, so count
	 * it in the result.  But changing a redirect (even to DEAD state) doesn't
	 * count.
	 */
	if (was_normal)
		prstate->ndeleted++;
}

/*
 * Depending on whether or not the caller set mark_unused_now to true, record that a
 * line pointer should be marked LP_DEAD or LP_UNUSED. There are other cases in
 * which we will mark line pointers LP_UNUSED, but we will not mark line
 * pointers LP_DEAD if mark_unused_now is true.
 */
static void
heap_prune_record_dead_or_unused(PruneState *prstate, OffsetNumber offnum,
								 bool was_normal)
{
	/*
	 * If the caller set mark_unused_now to true, we can remove dead tuples
	 * during pruning instead of marking their line pointers dead. Set this
	 * tuple's line pointer LP_UNUSED. We hint that this option is less
	 * likely.
	 */
	if (unlikely(prstate->mark_unused_now))
		heap_prune_record_unused(prstate, offnum, was_normal);
	else
		heap_prune_record_dead(prstate, offnum, was_normal);
}

/* Record line pointer to be marked unused */
static void
heap_prune_record_unused(PruneState *prstate, OffsetNumber offnum, bool was_normal)
{
	Assert(!prstate->processed[offnum]);
	prstate->processed[offnum] = true;

	Assert(prstate->nunused < MaxHeapTuplesPerPage);
	prstate->nowunused[prstate->nunused] = offnum;
	prstate->nunused++;

	/*
	 * If the root entry had been a normal tuple, we are deleting it, so count
	 * it in the result.  But changing a redirect (even to DEAD state) doesn't
	 * count.
	 */
	if (was_normal)
		prstate->ndeleted++;
}

/*
 * Record an unused line pointer that is left unchanged.
 */
static void
heap_prune_record_unchanged_lp_unused(Page page, PruneState *prstate, OffsetNumber offnum)
{
	Assert(!prstate->processed[offnum]);
	prstate->processed[offnum] = true;
}

/*
 * Record line pointer that is left unchanged.  We consider freezing it, and
 * update bookkeeping of tuple counts and page visibility.
 */
static void
heap_prune_record_unchanged_lp_normal(Page page, PruneState *prstate, OffsetNumber offnum)
{
	HeapTupleHeader htup;

	Assert(!prstate->processed[offnum]);
	prstate->processed[offnum] = true;

	prstate->hastup = true;		/* the page is not empty */

	/*
	 * The criteria for counting a tuple as live in this block need to match
	 * what analyze.c's acquire_sample_rows() does, otherwise VACUUM and
	 * ANALYZE may produce wildly different reltuples values, e.g. when there
	 * are many recently-dead tuples.
	 *
	 * The logic here is a bit simpler than acquire_sample_rows(), as VACUUM
	 * can't run inside a transaction block, which makes some cases impossible
	 * (e.g. in-progress insert from the same transaction).
	 *
	 * HEAPTUPLE_DEAD are handled by the other heap_prune_record_*()
	 * subroutines.  They don't count dead items like acquire_sample_rows()
	 * does, because we assume that all dead items will become LP_UNUSED
	 * before VACUUM finishes.  This difference is only superficial.  VACUUM
	 * effectively agrees with ANALYZE about DEAD items, in the end.  VACUUM
	 * won't remember LP_DEAD items, but only because they're not supposed to
	 * be left behind when it is done. (Cases where we bypass index vacuuming
	 * will violate this optimistic assumption, but the overall impact of that
	 * should be negligible.)
	 */
	htup = (HeapTupleHeader) PageGetItem(page, PageGetItemId(page, offnum));

	switch (prstate->htsv[offnum])
	{
		case HEAPTUPLE_LIVE:

			/*
			 * Count it as live.  Not only is this natural, but it's also what
			 * acquire_sample_rows() does.
			 */
			prstate->live_tuples++;

			/*
			 * Is the tuple definitely visible to all transactions?
			 *
			 * NB: Like with per-tuple hint bits, we can't set the
			 * PD_ALL_VISIBLE flag if the inserter committed asynchronously.
			 * See SetHintBits for more info.  Check that the tuple is hinted
			 * xmin-committed because of that.
			 */
			if (prstate->all_visible)
			{
				TransactionId xmin;

				if (!HeapTupleHeaderXminCommitted(htup))
				{
					prstate->all_visible = false;
					break;
				}

				/*
				 * The inserter definitely committed.  But is it old enough
				 * that everyone sees it as committed?  A FrozenTransactionId
				 * is seen as committed to everyone.  Otherwise, we check if
				 * there is a snapshot that considers this xid to still be
				 * running, and if so, we don't consider the page all-visible.
				 */
				xmin = HeapTupleHeaderGetXmin(htup);

				/*
				 * For now always use prstate->cutoffs for this test, because
				 * we only update 'all_visible' when freezing is requested. We
				 * could use GlobalVisTestIsRemovableXid instead, if a
				 * non-freezing caller wanted to set the VM bit.
				 */
				Assert(prstate->cutoffs);
				if (!TransactionIdPrecedes(xmin, prstate->cutoffs->OldestXmin))
				{
					prstate->all_visible = false;
					break;
				}

				/* Track newest xmin on page. */
				if (TransactionIdFollows(xmin, prstate->visibility_cutoff_xid) &&
					TransactionIdIsNormal(xmin))
					prstate->visibility_cutoff_xid = xmin;
			}
			break;

		case HEAPTUPLE_RECENTLY_DEAD:
			prstate->recently_dead_tuples++;
			prstate->all_visible = false;

			/*
			 * This tuple will soon become DEAD.  Update the hint field so
			 * that the page is reconsidered for pruning in future.
			 */
			heap_prune_record_prunable(prstate,
									   HeapTupleHeaderGetUpdateXid(htup));
			break;

		case HEAPTUPLE_INSERT_IN_PROGRESS:

			/*
			 * We do not count these rows as live, because we expect the
			 * inserting transaction to update the counters at commit, and we
			 * assume that will happen only after we report our results.  This
			 * assumption is a bit shaky, but it is what acquire_sample_rows()
			 * does, so be consistent.
			 */
			prstate->all_visible = false;

			/*
			 * If we wanted to optimize for aborts, we might consider marking
			 * the page prunable when we see INSERT_IN_PROGRESS.  But we
			 * don't.  See related decisions about when to mark the page
			 * prunable in heapam.c.
			 */
			break;

		case HEAPTUPLE_DELETE_IN_PROGRESS:

			/*
			 * This an expected case during concurrent vacuum.  Count such
			 * rows as live.  As above, we assume the deleting transaction
			 * will commit and update the counters after we report.
			 */
			prstate->live_tuples++;
			prstate->all_visible = false;

			/*
			 * This tuple may soon become DEAD.  Update the hint field so that
			 * the page is reconsidered for pruning in future.
			 */
			heap_prune_record_prunable(prstate,
									   HeapTupleHeaderGetUpdateXid(htup));
			break;

		default:

			/*
			 * DEAD tuples should've been passed to heap_prune_record_dead()
			 * or heap_prune_record_unused() instead.
			 */
			elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result %d",
				 prstate->htsv[offnum]);
			break;
	}

	/* Consider freezing any normal tuples which will not be removed */
	if (prstate->freeze)
	{
		bool		totally_frozen;

		if ((heap_prepare_freeze_tuple(htup,
									   prstate->cutoffs,
									   &prstate->pagefrz,
									   &prstate->frozen[prstate->nfrozen],
									   &totally_frozen)))
		{
			/* Save prepared freeze plan for later */
			prstate->frozen[prstate->nfrozen++].offset = offnum;
		}

		/*
		 * If any tuple isn't either totally frozen already or eligible to
		 * become totally frozen (according to its freeze plan), then the page
		 * definitely cannot be set all-frozen in the visibility map later on.
		 */
		if (!totally_frozen)
			prstate->all_frozen = false;
	}
}


/*
 * Record line pointer that was already LP_DEAD and is left unchanged.
 */
static void
heap_prune_record_unchanged_lp_dead(Page page, PruneState *prstate, OffsetNumber offnum)
{
	Assert(!prstate->processed[offnum]);
	prstate->processed[offnum] = true;

	/*
	 * Deliberately don't set hastup for LP_DEAD items.  We make the soft
	 * assumption that any LP_DEAD items encountered here will become
	 * LP_UNUSED later on, before count_nondeletable_pages is reached.  If we
	 * don't make this assumption then rel truncation will only happen every
	 * other VACUUM, at most.  Besides, VACUUM must treat
	 * hastup/nonempty_pages as provisional no matter how LP_DEAD items are
	 * handled (handled here, or handled later on).
	 *
	 * Similarly, don't unset all_visible until later, at the end of
	 * heap_page_prune_and_freeze().  This will allow us to attempt to freeze
	 * the page after pruning.  As long as we unset it before updating the
	 * visibility map, this will be correct.
	 */

	/* Record the dead offset for vacuum */
	prstate->deadoffsets[prstate->lpdead_items++] = offnum;
}

/*
 * Record LP_REDIRECT that is left unchanged.
 */
static void
heap_prune_record_unchanged_lp_redirect(PruneState *prstate, OffsetNumber offnum)
{
	/*
	 * A redirect line pointer doesn't count as a live tuple.
	 *
	 * If we leave a redirect line pointer in place, there will be another
	 * tuple on the page that it points to.  We will do the bookkeeping for
	 * that separately.  So we have nothing to do here, except remember that
	 * we processed this item.
	 */
	Assert(!prstate->processed[offnum]);
	prstate->processed[offnum] = true;
}

/*
 * Perform the actual page changes needed by heap_page_prune_and_freeze().
 *
 * If 'lp_truncate_only' is set, we are merely marking LP_DEAD line pointers
 * as unused, not redirecting or removing anything else.  The
 * PageRepairFragmentation() call is skipped in that case.
 *
 * If 'lp_truncate_only' is not set, the caller must hold a cleanup lock on
 * the buffer.  If it is set, an ordinary exclusive lock suffices.
 */
void
heap_page_prune_execute(Buffer buffer, bool lp_truncate_only,
						OffsetNumber *redirected, int nredirected,
						OffsetNumber *nowdead, int ndead,
						OffsetNumber *nowunused, int nunused)
{
	Page		page = (Page) BufferGetPage(buffer);
	OffsetNumber *offnum;
	HeapTupleHeader htup PG_USED_FOR_ASSERTS_ONLY;

	/* Shouldn't be called unless there's something to do */
	Assert(nredirected > 0 || ndead > 0 || nunused > 0);

	/* If 'lp_truncate_only', we can only remove already-dead line pointers */
	Assert(!lp_truncate_only || (nredirected == 0 && ndead == 0));

	/* Update all redirected line pointers */
	offnum = redirected;
	for (int i = 0; i < nredirected; i++)
	{
		OffsetNumber fromoff = *offnum++;
		OffsetNumber tooff = *offnum++;
		ItemId		fromlp = PageGetItemId(page, fromoff);
		ItemId		tolp PG_USED_FOR_ASSERTS_ONLY;

#ifdef USE_ASSERT_CHECKING

		/*
		 * Any existing item that we set as an LP_REDIRECT (any 'from' item)
		 * must be the first item from a HOT chain.  If the item has tuple
		 * storage then it can't be a heap-only tuple.  Otherwise we are just
		 * maintaining an existing LP_REDIRECT from an existing HOT chain that
		 * has been pruned at least once before now.
		 */
		if (!ItemIdIsRedirected(fromlp))
		{
			Assert(ItemIdHasStorage(fromlp) && ItemIdIsNormal(fromlp));

			htup = (HeapTupleHeader) PageGetItem(page, fromlp);
			Assert(!HeapTupleHeaderIsHeapOnly(htup));
		}
		else
		{
			/* We shouldn't need to redundantly set the redirect */
			Assert(ItemIdGetRedirect(fromlp) != tooff);
		}

		/*
		 * The item that we're about to set as an LP_REDIRECT (the 'from'
		 * item) will point to an existing item (the 'to' item) that is
		 * already a heap-only tuple.  There can be at most one LP_REDIRECT
		 * item per HOT chain.
		 *
		 * We need to keep around an LP_REDIRECT item (after original
		 * non-heap-only root tuple gets pruned away) so that it's always
		 * possible for VACUUM to easily figure out what TID to delete from
		 * indexes when an entire HOT chain becomes dead.  A heap-only tuple
		 * can never become LP_DEAD; an LP_REDIRECT item or a regular heap
		 * tuple can.
		 *
		 * This check may miss problems, e.g. the target of a redirect could
		 * be marked as unused subsequently. The page_verify_redirects() check
		 * below will catch such problems.
		 */
		tolp = PageGetItemId(page, tooff);
		Assert(ItemIdHasStorage(tolp) && ItemIdIsNormal(tolp));
		htup = (HeapTupleHeader) PageGetItem(page, tolp);
		Assert(HeapTupleHeaderIsHeapOnly(htup));
#endif

		ItemIdSetRedirect(fromlp, tooff);
	}

	/* Update all now-dead line pointers */
	offnum = nowdead;
	for (int i = 0; i < ndead; i++)
	{
		OffsetNumber off = *offnum++;
		ItemId		lp = PageGetItemId(page, off);

#ifdef USE_ASSERT_CHECKING

		/*
		 * An LP_DEAD line pointer must be left behind when the original item
		 * (which is dead to everybody) could still be referenced by a TID in
		 * an index.  This should never be necessary with any individual
		 * heap-only tuple item, though. (It's not clear how much of a problem
		 * that would be, but there is no reason to allow it.)
		 */
		if (ItemIdHasStorage(lp))
		{
			Assert(ItemIdIsNormal(lp));
			htup = (HeapTupleHeader) PageGetItem(page, lp);
			Assert(!HeapTupleHeaderIsHeapOnly(htup));
		}
		else
		{
			/* Whole HOT chain becomes dead */
			Assert(ItemIdIsRedirected(lp));
		}
#endif

		ItemIdSetDead(lp);
	}

	/* Update all now-unused line pointers */
	offnum = nowunused;
	for (int i = 0; i < nunused; i++)
	{
		OffsetNumber off = *offnum++;
		ItemId		lp = PageGetItemId(page, off);

#ifdef USE_ASSERT_CHECKING

		if (lp_truncate_only)
		{
			/* Setting LP_DEAD to LP_UNUSED in vacuum's second pass */
			Assert(ItemIdIsDead(lp) && !ItemIdHasStorage(lp));
		}
		else
		{
			/*
			 * When heap_page_prune_and_freeze() was called, mark_unused_now
			 * may have been passed as true, which allows would-be LP_DEAD
			 * items to be made LP_UNUSED instead.  This is only possible if
			 * the relation has no indexes.  If there are any dead items, then
			 * mark_unused_now was not true and every item being marked
			 * LP_UNUSED must refer to a heap-only tuple.
			 */
			if (ndead > 0)
			{
				Assert(ItemIdHasStorage(lp) && ItemIdIsNormal(lp));
				htup = (HeapTupleHeader) PageGetItem(page, lp);
				Assert(HeapTupleHeaderIsHeapOnly(htup));
			}
			else
				Assert(ItemIdIsUsed(lp));
		}

#endif

		ItemIdSetUnused(lp);
	}

	if (lp_truncate_only)
		PageTruncateLinePointerArray(page);
	else
	{
		/*
		 * Finally, repair any fragmentation, and update the page's hint bit
		 * about whether it has free pointers.
		 */
		PageRepairFragmentation(page);

		/*
		 * Now that the page has been modified, assert that redirect items
		 * still point to valid targets.
		 */
		page_verify_redirects(page);
	}
}


/*
 * If built with assertions, verify that all LP_REDIRECT items point to a
 * valid item.
 *
 * One way that bugs related to HOT pruning show is redirect items pointing to
 * removed tuples. It's not trivial to reliably check that marking an item
 * unused will not orphan a redirect item during heap_prune_chain() /
 * heap_page_prune_execute(), so we additionally check the whole page after
 * pruning. Without this check such bugs would typically only cause asserts
 * later, potentially well after the corruption has been introduced.
 *
 * Also check comments in heap_page_prune_execute()'s redirection loop.
 */
static void
page_verify_redirects(Page page)
{
#ifdef USE_ASSERT_CHECKING
	OffsetNumber offnum;
	OffsetNumber maxoff;

	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = FirstOffsetNumber;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemid = PageGetItemId(page, offnum);
		OffsetNumber targoff;
		ItemId		targitem;
		HeapTupleHeader htup;

		if (!ItemIdIsRedirected(itemid))
			continue;

		targoff = ItemIdGetRedirect(itemid);
		targitem = PageGetItemId(page, targoff);

		Assert(ItemIdIsUsed(targitem));
		Assert(ItemIdIsNormal(targitem));
		Assert(ItemIdHasStorage(targitem));
		htup = (HeapTupleHeader) PageGetItem(page, targitem);
		Assert(HeapTupleHeaderIsHeapOnly(htup));
	}
#endif
}


/*
 * For all items in this page, find their respective root line pointers.
 * If item k is part of a HOT-chain with root at item j, then we set
 * root_offsets[k - 1] = j.
 *
 * The passed-in root_offsets array must have MaxHeapTuplesPerPage entries.
 * Unused entries are filled with InvalidOffsetNumber (zero).
 *
 * The function must be called with at least share lock on the buffer, to
 * prevent concurrent prune operations.
 *
 * Note: The information collected here is valid only as long as the caller
 * holds a pin on the buffer. Once pin is released, a tuple might be pruned
 * and reused by a completely unrelated tuple.
 */
void
heap_get_root_tuples(Page page, OffsetNumber *root_offsets)
{
	OffsetNumber offnum,
				maxoff;

	MemSet(root_offsets, InvalidOffsetNumber,
		   MaxHeapTuplesPerPage * sizeof(OffsetNumber));

	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = FirstOffsetNumber; offnum <= maxoff; offnum = OffsetNumberNext(offnum))
	{
		ItemId		lp = PageGetItemId(page, offnum);
		HeapTupleHeader htup;
		OffsetNumber nextoffnum;
		TransactionId priorXmax;

		/* skip unused and dead items */
		if (!ItemIdIsUsed(lp) || ItemIdIsDead(lp))
			continue;

		if (ItemIdIsNormal(lp))
		{
			htup = (HeapTupleHeader) PageGetItem(page, lp);

			/*
			 * Check if this tuple is part of a HOT-chain rooted at some other
			 * tuple. If so, skip it for now; we'll process it when we find
			 * its root.
			 */
			if (HeapTupleHeaderIsHeapOnly(htup))
				continue;

			/*
			 * This is either a plain tuple or the root of a HOT-chain.
			 * Remember it in the mapping.
			 */
			root_offsets[offnum - 1] = offnum;

			/* If it's not the start of a HOT-chain, we're done with it */
			if (!HeapTupleHeaderIsHotUpdated(htup))
				continue;

			/* Set up to scan the HOT-chain */
			nextoffnum = ItemPointerGetOffsetNumber(&htup->t_ctid);
			priorXmax = HeapTupleHeaderGetUpdateXid(htup);
		}
		else
		{
			/* Must be a redirect item. We do not set its root_offsets entry */
			Assert(ItemIdIsRedirected(lp));
			/* Set up to scan the HOT-chain */
			nextoffnum = ItemIdGetRedirect(lp);
			priorXmax = InvalidTransactionId;
		}

		/*
		 * Now follow the HOT-chain and collect other tuples in the chain.
		 *
		 * Note: Even though this is a nested loop, the complexity of the
		 * function is O(N) because a tuple in the page should be visited not
		 * more than twice, once in the outer loop and once in HOT-chain
		 * chases.
		 */
		for (;;)
		{
			/* Sanity check (pure paranoia) */
			if (offnum < FirstOffsetNumber)
				break;

			/*
			 * An offset past the end of page's line pointer array is possible
			 * when the array was truncated
			 */
			if (offnum > maxoff)
				break;

			lp = PageGetItemId(page, nextoffnum);

			/* Check for broken chains */
			if (!ItemIdIsNormal(lp))
				break;

			htup = (HeapTupleHeader) PageGetItem(page, lp);

			if (TransactionIdIsValid(priorXmax) &&
				!TransactionIdEquals(priorXmax, HeapTupleHeaderGetXmin(htup)))
				break;

			/* Remember the root line pointer for this item */
			root_offsets[nextoffnum - 1] = offnum;

			/* Advance to next chain member, if any */
			if (!HeapTupleHeaderIsHotUpdated(htup))
				break;

			/* HOT implies it can't have moved to different partition */
			Assert(!HeapTupleHeaderIndicatesMovedPartitions(htup));

			nextoffnum = ItemPointerGetOffsetNumber(&htup->t_ctid);
			priorXmax = HeapTupleHeaderGetUpdateXid(htup);
		}
	}
}


/*
 * Compare fields that describe actions required to freeze tuple with caller's
 * open plan.  If everything matches then the frz tuple plan is equivalent to
 * caller's plan.
 */
static inline bool
heap_log_freeze_eq(xlhp_freeze_plan *plan, HeapTupleFreeze *frz)
{
	if (plan->xmax == frz->xmax &&
		plan->t_infomask2 == frz->t_infomask2 &&
		plan->t_infomask == frz->t_infomask &&
		plan->frzflags == frz->frzflags)
		return true;

	/* Caller must call heap_log_freeze_new_plan again for frz */
	return false;
}

/*
 * Comparator used to deduplicate the freeze plans used in WAL records.
 */
static int
heap_log_freeze_cmp(const void *arg1, const void *arg2)
{
	HeapTupleFreeze *frz1 = (HeapTupleFreeze *) arg1;
	HeapTupleFreeze *frz2 = (HeapTupleFreeze *) arg2;

	if (frz1->xmax < frz2->xmax)
		return -1;
	else if (frz1->xmax > frz2->xmax)
		return 1;

	if (frz1->t_infomask2 < frz2->t_infomask2)
		return -1;
	else if (frz1->t_infomask2 > frz2->t_infomask2)
		return 1;

	if (frz1->t_infomask < frz2->t_infomask)
		return -1;
	else if (frz1->t_infomask > frz2->t_infomask)
		return 1;

	if (frz1->frzflags < frz2->frzflags)
		return -1;
	else if (frz1->frzflags > frz2->frzflags)
		return 1;

	/*
	 * heap_log_freeze_eq would consider these tuple-wise plans to be equal.
	 * (So the tuples will share a single canonical freeze plan.)
	 *
	 * We tiebreak on page offset number to keep each freeze plan's page
	 * offset number array individually sorted. (Unnecessary, but be tidy.)
	 */
	if (frz1->offset < frz2->offset)
		return -1;
	else if (frz1->offset > frz2->offset)
		return 1;

	Assert(false);
	return 0;
}

/*
 * Start new plan initialized using tuple-level actions.  At least one tuple
 * will have steps required to freeze described by caller's plan during REDO.
 */
static inline void
heap_log_freeze_new_plan(xlhp_freeze_plan *plan, HeapTupleFreeze *frz)
{
	plan->xmax = frz->xmax;
	plan->t_infomask2 = frz->t_infomask2;
	plan->t_infomask = frz->t_infomask;
	plan->frzflags = frz->frzflags;
	plan->ntuples = 1;			/* for now */
}

/*
 * Deduplicate tuple-based freeze plans so that each distinct set of
 * processing steps is only stored once in the WAL record.
 * Called during original execution of freezing (for logged relations).
 *
 * Return value is number of plans set in *plans_out for caller.  Also writes
 * an array of offset numbers into *offsets_out output argument for caller
 * (actually there is one array per freeze plan, but that's not of immediate
 * concern to our caller).
 */
static int
heap_log_freeze_plan(HeapTupleFreeze *tuples, int ntuples,
					 xlhp_freeze_plan *plans_out,
					 OffsetNumber *offsets_out)
{
	int			nplans = 0;

	/* Sort tuple-based freeze plans in the order required to deduplicate */
	qsort(tuples, ntuples, sizeof(HeapTupleFreeze), heap_log_freeze_cmp);

	for (int i = 0; i < ntuples; i++)
	{
		HeapTupleFreeze *frz = tuples + i;

		if (i == 0)
		{
			/* New canonical freeze plan starting with first tup */
			heap_log_freeze_new_plan(plans_out, frz);
			nplans++;
		}
		else if (heap_log_freeze_eq(plans_out, frz))
		{
			/* tup matches open canonical plan -- include tup in it */
			Assert(offsets_out[i - 1] < frz->offset);
			plans_out->ntuples++;
		}
		else
		{
			/* Tup doesn't match current plan -- done with it now */
			plans_out++;

			/* New canonical freeze plan starting with this tup */
			heap_log_freeze_new_plan(plans_out, frz);
			nplans++;
		}

		/*
		 * Save page offset number in dedicated buffer in passing.
		 *
		 * REDO routine relies on the record's offset numbers array grouping
		 * offset numbers by freeze plan.  The sort order within each grouping
		 * is ascending offset number order, just to keep things tidy.
		 */
		offsets_out[i] = frz->offset;
	}

	Assert(nplans > 0 && nplans <= ntuples);

	return nplans;
}

/*
 * Write an XLOG_HEAP2_PRUNE_FREEZE WAL record
 *
 * This is used for several different page maintenance operations:
 *
 * - Page pruning, in VACUUM's 1st pass or on access: Some items are
 *   redirected, some marked dead, and some removed altogether.
 *
 * - Freezing: Items are marked as 'frozen'.
 *
 * - Vacuum, 2nd pass: Items that are already LP_DEAD are marked as unused.
 *
 * They have enough commonalities that we use a single WAL record for them
 * all.
 *
 * If replaying the record requires a cleanup lock, pass cleanup_lock = true.
 * Replaying 'redirected' or 'dead' items always requires a cleanup lock, but
 * replaying 'unused' items depends on whether they were all previously marked
 * as dead.
 *
 * Note: This function scribbles on the 'frozen' array.
 *
 * Note: This is called in a critical section, so careful what you do here.
 */
void
log_heap_prune_and_freeze(Relation relation, Buffer buffer,
						  TransactionId conflict_xid,
						  bool cleanup_lock,
						  PruneReason reason,
						  HeapTupleFreeze *frozen, int nfrozen,
						  OffsetNumber *redirected, int nredirected,
						  OffsetNumber *dead, int ndead,
						  OffsetNumber *unused, int nunused)
{
	xl_heap_prune xlrec;
	XLogRecPtr	recptr;
	uint8		info;

	/* The following local variables hold data registered in the WAL record: */
	xlhp_freeze_plan plans[MaxHeapTuplesPerPage];
	xlhp_freeze_plans freeze_plans;
	xlhp_prune_items redirect_items;
	xlhp_prune_items dead_items;
	xlhp_prune_items unused_items;
	OffsetNumber frz_offsets[MaxHeapTuplesPerPage];

	xlrec.flags = 0;

	/*
	 * Prepare data for the buffer.  The arrays are not actually in the
	 * buffer, but we pretend that they are.  When XLogInsert stores a full
	 * page image, the arrays can be omitted.
	 */
	XLogBeginInsert();
	XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
	if (nfrozen > 0)
	{
		int			nplans;

		xlrec.flags |= XLHP_HAS_FREEZE_PLANS;

		/*
		 * Prepare deduplicated representation for use in the WAL record. This
		 * destructively sorts frozen tuples array in-place.
		 */
		nplans = heap_log_freeze_plan(frozen, nfrozen, plans, frz_offsets);

		freeze_plans.nplans = nplans;
		XLogRegisterBufData(0, (char *) &freeze_plans,
							offsetof(xlhp_freeze_plans, plans));
		XLogRegisterBufData(0, (char *) plans,
							sizeof(xlhp_freeze_plan) * nplans);
	}
	if (nredirected > 0)
	{
		xlrec.flags |= XLHP_HAS_REDIRECTIONS;

		redirect_items.ntargets = nredirected;
		XLogRegisterBufData(0, (char *) &redirect_items,
							offsetof(xlhp_prune_items, data));
		XLogRegisterBufData(0, (char *) redirected,
							sizeof(OffsetNumber[2]) * nredirected);
	}
	if (ndead > 0)
	{
		xlrec.flags |= XLHP_HAS_DEAD_ITEMS;

		dead_items.ntargets = ndead;
		XLogRegisterBufData(0, (char *) &dead_items,
							offsetof(xlhp_prune_items, data));
		XLogRegisterBufData(0, (char *) dead,
							sizeof(OffsetNumber) * ndead);
	}
	if (nunused > 0)
	{
		xlrec.flags |= XLHP_HAS_NOW_UNUSED_ITEMS;

		unused_items.ntargets = nunused;
		XLogRegisterBufData(0, (char *) &unused_items,
							offsetof(xlhp_prune_items, data));
		XLogRegisterBufData(0, (char *) unused,
							sizeof(OffsetNumber) * nunused);
	}
	if (nfrozen > 0)
		XLogRegisterBufData(0, (char *) frz_offsets,
							sizeof(OffsetNumber) * nfrozen);

	/*
	 * Prepare the main xl_heap_prune record.  We already set the XLHP_HAS_*
	 * flag above.
	 */
	if (RelationIsAccessibleInLogicalDecoding(relation))
		xlrec.flags |= XLHP_IS_CATALOG_REL;
	if (TransactionIdIsValid(conflict_xid))
		xlrec.flags |= XLHP_HAS_CONFLICT_HORIZON;
	if (cleanup_lock)
		xlrec.flags |= XLHP_CLEANUP_LOCK;
	else
	{
		Assert(nredirected == 0 && ndead == 0);
		/* also, any items in 'unused' must've been LP_DEAD previously */
	}
	XLogRegisterData((char *) &xlrec, SizeOfHeapPrune);
	if (TransactionIdIsValid(conflict_xid))
		XLogRegisterData((char *) &conflict_xid, sizeof(TransactionId));

	switch (reason)
	{
		case PRUNE_ON_ACCESS:
			info = XLOG_HEAP2_PRUNE_ON_ACCESS;
			break;
		case PRUNE_VACUUM_SCAN:
			info = XLOG_HEAP2_PRUNE_VACUUM_SCAN;
			break;
		case PRUNE_VACUUM_CLEANUP:
			info = XLOG_HEAP2_PRUNE_VACUUM_CLEANUP;
			break;
		default:
			elog(ERROR, "unrecognized prune reason: %d", (int) reason);
			break;
	}
	recptr = XLogInsert(RM_HEAP2_ID, info);

	PageSetLSN(BufferGetPage(buffer), recptr);
}
