/*-------------------------------------------------------------------------
 *
 * pruneheap.c
 *	  heap page pruning and HOT-chain management code
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/heap/pruneheap.c,v 1.6 2008/01/01 19:45:46 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/transam.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/inval.h"


/* Local functions */
static int heap_prune_chain(Relation relation, Buffer buffer,
				 OffsetNumber rootoffnum,
				 TransactionId OldestXmin,
				 OffsetNumber *redirected, int *nredirected,
				 OffsetNumber *nowdead, int *ndead,
				 OffsetNumber *nowunused, int *nunused,
				 bool redirect_move);
static void heap_prune_record_redirect(OffsetNumber *redirected,
						   int *nredirected,
						   OffsetNumber offnum,
						   OffsetNumber rdoffnum);
static void heap_prune_record_dead(OffsetNumber *nowdead, int *ndead,
					   OffsetNumber offnum);
static void heap_prune_record_unused(OffsetNumber *nowunused, int *nunused,
						 OffsetNumber offnum);


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
 *
 * OldestXmin is the cutoff XID used to distinguish whether tuples are DEAD
 * or RECENTLY_DEAD (see HeapTupleSatisfiesVacuum).
 */
void
heap_page_prune_opt(Relation relation, Buffer buffer, TransactionId OldestXmin)
{
	PageHeader	dp = (PageHeader) BufferGetPage(buffer);
	Size		minfree;

	/*
	 * Let's see if we really need pruning.
	 *
	 * Forget it if page is not hinted to contain something prunable that's
	 * older than OldestXmin.
	 */
	if (!PageIsPrunable(dp, OldestXmin))
		return;

	/*
	 * We prune when a previous UPDATE failed to find enough space on the page
	 * for a new tuple version, or when free space falls below the relation's
	 * fill-factor target (but not less than 10%).
	 *
	 * Checking free space here is questionable since we aren't holding any
	 * lock on the buffer; in the worst case we could get a bogus answer. It's
	 * unlikely to be *seriously* wrong, though, since reading either pd_lower
	 * or pd_upper is probably atomic.	Avoiding taking a lock seems better
	 * than sometimes getting a wrong answer in what is after all just a
	 * heuristic estimate.
	 */
	minfree = RelationGetTargetPageFreeSpace(relation,
											 HEAP_DEFAULT_FILLFACTOR);
	minfree = Max(minfree, BLCKSZ / 10);

	if (PageIsFull(dp) || PageGetHeapFreeSpace((Page) dp) < minfree)
	{
		/* OK, try to get exclusive buffer lock */
		if (!ConditionalLockBufferForCleanup(buffer))
			return;

		/*
		 * Now that we have buffer lock, get accurate information about the
		 * page's free space, and recheck the heuristic about whether to
		 * prune. (We needn't recheck PageIsPrunable, since no one else could
		 * have pruned while we hold pin.)
		 */
		if (PageIsFull(dp) || PageGetHeapFreeSpace((Page) dp) < minfree)
		{
			/* OK to prune (though not to remove redirects) */
			(void) heap_page_prune(relation, buffer, OldestXmin, false, true);
		}

		/* And release buffer lock */
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	}
}


/*
 * Prune and repair fragmentation in the specified page.
 *
 * Caller must have pin and buffer cleanup lock on the page.
 *
 * OldestXmin is the cutoff XID used to distinguish whether tuples are DEAD
 * or RECENTLY_DEAD (see HeapTupleSatisfiesVacuum).
 *
 * If redirect_move is set, we remove redirecting line pointers by
 * updating the root line pointer to point directly to the first non-dead
 * tuple in the chain.	NOTE: eliminating the redirect changes the first
 * tuple's effective CTID, and is therefore unsafe except within VACUUM FULL.
 * The only reason we support this capability at all is that by using it,
 * VACUUM FULL need not cope with LP_REDIRECT items at all; which seems a
 * good thing since VACUUM FULL is overly complicated already.
 *
 * If report_stats is true then we send the number of reclaimed heap-only
 * tuples to pgstats.  (This must be FALSE during vacuum, since vacuum will
 * send its own new total to pgstats, and we don't want this delta applied
 * on top of that.)
 *
 * Returns the number of tuples deleted from the page.
 */
int
heap_page_prune(Relation relation, Buffer buffer, TransactionId OldestXmin,
				bool redirect_move, bool report_stats)
{
	int			ndeleted = 0;
	Page		page = BufferGetPage(buffer);
	OffsetNumber offnum,
				maxoff;
	OffsetNumber redirected[MaxHeapTuplesPerPage * 2];
	OffsetNumber nowdead[MaxHeapTuplesPerPage];
	OffsetNumber nowunused[MaxHeapTuplesPerPage];
	int			nredirected = 0;
	int			ndead = 0;
	int			nunused = 0;
	bool		page_was_full = false;
	TransactionId save_prune_xid;

	START_CRIT_SECTION();

	/*
	 * Save the current pd_prune_xid and mark the page as clear of prunable
	 * tuples. If we find a tuple which may soon become prunable, we shall set
	 * the hint again.
	 */
	save_prune_xid = ((PageHeader) page)->pd_prune_xid;
	PageClearPrunable(page);

	/*
	 * Also clear the "page is full" flag if it is set, since there's no point
	 * in repeating the prune/defrag process until something else happens to
	 * the page.
	 */
	if (PageIsFull(page))
	{
		PageClearFull(page);
		page_was_full = true;
	}

	/* Scan the page */
	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = FirstOffsetNumber;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemid = PageGetItemId(page, offnum);

		/* Nothing to do if slot is empty or already dead */
		if (!ItemIdIsUsed(itemid) || ItemIdIsDead(itemid))
			continue;

		/* Process this item or chain of items */
		ndeleted += heap_prune_chain(relation, buffer, offnum,
									 OldestXmin,
									 redirected, &nredirected,
									 nowdead, &ndead,
									 nowunused, &nunused,
									 redirect_move);
	}

	/* Have we pruned any items? */
	if (nredirected > 0 || ndead > 0 || nunused > 0)
	{
		/*
		 * Repair page fragmentation, and update the page's hint bit about
		 * whether it has free line pointers.
		 */
		PageRepairFragmentation((Page) page);

		MarkBufferDirty(buffer);

		/*
		 * Emit a WAL HEAP_CLEAN or HEAP_CLEAN_MOVE record showing what we did
		 */
		if (!relation->rd_istemp)
		{
			XLogRecPtr	recptr;

			recptr = log_heap_clean(relation, buffer,
									redirected, nredirected,
									nowdead, ndead,
									nowunused, nunused,
									redirect_move);
			PageSetTLI(BufferGetPage(buffer), ThisTimeLineID);
			PageSetLSN(BufferGetPage(buffer), recptr);
		}
	}
	else
	{
		/*
		 * If we didn't prune anything, but have updated either the
		 * pd_prune_xid field or the "page is full" flag, mark the buffer
		 * dirty.  This is treated as a non-WAL-logged hint.
		 */
		if (((PageHeader) page)->pd_prune_xid != save_prune_xid ||
			page_was_full)
			SetBufferCommitInfoNeedsSave(buffer);
	}

	END_CRIT_SECTION();

	/*
	 * If requested, report the number of tuples reclaimed to pgstats. This is
	 * ndeleted minus ndead, because we don't want to count a now-DEAD root
	 * item as a deletion for this purpose.
	 */
	if (report_stats && ndeleted > ndead)
		pgstat_update_heap_dead_tuples(relation, ndeleted - ndead);

	/*
	 * XXX Should we update the FSM information of this page ?
	 *
	 * There are two schools of thought here. We may not want to update FSM
	 * information so that the page is not used for unrelated UPDATEs/INSERTs
	 * and any free space in this page will remain available for further
	 * UPDATEs in *this* page, thus improving chances for doing HOT updates.
	 *
	 * But for a large table and where a page does not receive further UPDATEs
	 * for a long time, we might waste this space by not updating the FSM
	 * information. The relation may get extended and fragmented further.
	 *
	 * One possibility is to leave "fillfactor" worth of space in this page
	 * and update FSM with the remaining space.
	 *
	 * In any case, the current FSM implementation doesn't accept
	 * one-page-at-a-time updates, so this is all academic for now.
	 */

	return ndeleted;
}


/*
 * Prune specified item pointer or a HOT chain originating at that item.
 *
 * If the item is an index-referenced tuple (i.e. not a heap-only tuple),
 * the HOT chain is pruned by removing all DEAD tuples at the start of the HOT
 * chain.  We also prune any RECENTLY_DEAD tuples preceding a DEAD tuple.
 * This is OK because a RECENTLY_DEAD tuple preceding a DEAD tuple is really
 * DEAD, the OldestXmin test is just too coarse to detect it.
 *
 * The root line pointer is redirected to the tuple immediately after the
 * latest DEAD tuple.  If all tuples in the chain are DEAD, the root line
 * pointer is marked LP_DEAD.  (This includes the case of a DEAD simple
 * tuple, which we treat as a chain of length 1.)
 *
 * OldestXmin is the cutoff XID used to identify dead tuples.
 *
 * Redirected items are added to the redirected[] array (two entries per
 * redirection); items set to LP_DEAD state are added to nowdead[]; and
 * items set to LP_UNUSED state are added to nowunused[].  (These arrays
 * will be used to generate a WAL record after all chains are pruned.)
 *
 * If redirect_move is true, we get rid of redirecting line pointers.
 *
 * Returns the number of tuples deleted from the page.
 */
static int
heap_prune_chain(Relation relation, Buffer buffer, OffsetNumber rootoffnum,
				 TransactionId OldestXmin,
				 OffsetNumber *redirected, int *nredirected,
				 OffsetNumber *nowdead, int *ndead,
				 OffsetNumber *nowunused, int *nunused,
				 bool redirect_move)
{
	int			ndeleted = 0;
	Page		dp = (Page) BufferGetPage(buffer);
	TransactionId priorXmax = InvalidTransactionId;
	ItemId		rootlp;
	HeapTupleHeader htup;
	OffsetNumber latestdead = InvalidOffsetNumber,
				maxoff = PageGetMaxOffsetNumber(dp),
				offnum;
	OffsetNumber chainitems[MaxHeapTuplesPerPage];
	int			nchain = 0,
				i;

	rootlp = PageGetItemId(dp, rootoffnum);

	/*
	 * If it's a heap-only tuple, then it is not the start of a HOT chain.
	 */
	if (ItemIdIsNormal(rootlp))
	{
		htup = (HeapTupleHeader) PageGetItem(dp, rootlp);
		if (HeapTupleHeaderIsHeapOnly(htup))
		{
			/*
			 * If the tuple is DEAD and doesn't chain to anything else, mark
			 * it unused immediately.  (If it does chain, we can only remove
			 * it as part of pruning its chain.)
			 *
			 * We need this primarily to handle aborted HOT updates, that is,
			 * XMIN_INVALID heap-only tuples.  Those might not be linked to by
			 * any chain, since the parent tuple might be re-updated before
			 * any pruning occurs.	So we have to be able to reap them
			 * separately from chain-pruning.
			 *
			 * Note that we might first arrive at a dead heap-only tuple
			 * either here or while following a chain below.  Whichever path
			 * gets there first will mark the tuple unused.
			 */
			if (HeapTupleSatisfiesVacuum(htup, OldestXmin, buffer)
				== HEAPTUPLE_DEAD && !HeapTupleHeaderIsHotUpdated(htup))
			{
				ItemIdSetUnused(rootlp);
				heap_prune_record_unused(nowunused, nunused, rootoffnum);
				ndeleted++;
			}

			/* Nothing more to do */
			return ndeleted;
		}
	}

	/* Start from the root tuple */
	offnum = rootoffnum;

	/* while not end of the chain */
	for (;;)
	{
		ItemId		lp;
		bool		tupdead,
					recent_dead;

		/* Some sanity checks */
		if (offnum < FirstOffsetNumber || offnum > maxoff)
			break;

		lp = PageGetItemId(dp, offnum);

		if (!ItemIdIsUsed(lp))
			break;

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

		/*
		 * Likewise, a dead item pointer can't be part of the chain. (We
		 * already eliminated the case of dead root tuple outside this
		 * function.)
		 */
		if (ItemIdIsDead(lp))
			break;

		Assert(ItemIdIsNormal(lp));
		htup = (HeapTupleHeader) PageGetItem(dp, lp);

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

		/*
		 * Check tuple's visibility status.
		 */
		tupdead = recent_dead = false;

		switch (HeapTupleSatisfiesVacuum(htup, OldestXmin, buffer))
		{
			case HEAPTUPLE_DEAD:
				tupdead = true;
				break;

			case HEAPTUPLE_RECENTLY_DEAD:
				recent_dead = true;

				/*
				 * This tuple may soon become DEAD.  Update the hint field so
				 * that the page is reconsidered for pruning in future.
				 */
				PageSetPrunable(dp, HeapTupleHeaderGetXmax(htup));
				break;

			case HEAPTUPLE_DELETE_IN_PROGRESS:

				/*
				 * This tuple may soon become DEAD.  Update the hint field so
				 * that the page is reconsidered for pruning in future.
				 */
				PageSetPrunable(dp, HeapTupleHeaderGetXmax(htup));
				break;

			case HEAPTUPLE_LIVE:
			case HEAPTUPLE_INSERT_IN_PROGRESS:

				/*
				 * If we wanted to optimize for aborts, we might consider
				 * marking the page prunable when we see INSERT_IN_PROGRESS.
				 * But we don't.  See related decisions about when to mark the
				 * page prunable in heapam.c.
				 */
				break;

			default:
				elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
				break;
		}

		/*
		 * Remember the last DEAD tuple seen.  We will advance past
		 * RECENTLY_DEAD tuples just in case there's a DEAD one after them;
		 * but we can't advance past anything else.  (XXX is it really worth
		 * continuing to scan beyond RECENTLY_DEAD?  The case where we will
		 * find another DEAD tuple is a fairly unusual corner case.)
		 */
		if (tupdead)
			latestdead = offnum;
		else if (!recent_dead)
			break;

		/*
		 * If the tuple is not HOT-updated, then we are at the end of this
		 * HOT-update chain.
		 */
		if (!HeapTupleHeaderIsHotUpdated(htup))
			break;

		/*
		 * Advance to next chain member.
		 */
		Assert(ItemPointerGetBlockNumber(&htup->t_ctid) ==
			   BufferGetBlockNumber(buffer));
		offnum = ItemPointerGetOffsetNumber(&htup->t_ctid);
		priorXmax = HeapTupleHeaderGetXmax(htup);
	}

	/*
	 * If we found a DEAD tuple in the chain, adjust the HOT chain so that all
	 * the DEAD tuples at the start of the chain are removed and the root line
	 * pointer is appropriately redirected.
	 */
	if (OffsetNumberIsValid(latestdead))
	{
		/*
		 * Mark as unused each intermediate item that we are able to remove
		 * from the chain.
		 *
		 * When the previous item is the last dead tuple seen, we are at the
		 * right candidate for redirection.
		 */
		for (i = 1; (i < nchain) && (chainitems[i - 1] != latestdead); i++)
		{
			ItemId		lp = PageGetItemId(dp, chainitems[i]);

			ItemIdSetUnused(lp);
			heap_prune_record_unused(nowunused, nunused, chainitems[i]);
			ndeleted++;
		}

		/*
		 * If the root entry had been a normal tuple, we are deleting it, so
		 * count it in the result.	But changing a redirect (even to DEAD
		 * state) doesn't count.
		 */
		if (ItemIdIsNormal(rootlp))
			ndeleted++;

		/*
		 * If the DEAD tuple is at the end of the chain, the entire chain is
		 * dead and the root line pointer can be marked dead.  Otherwise just
		 * redirect the root to the correct chain member.
		 */
		if (i >= nchain)
		{
			ItemIdSetDead(rootlp);
			heap_prune_record_dead(nowdead, ndead, rootoffnum);
		}
		else
		{
			ItemIdSetRedirect(rootlp, chainitems[i]);
			heap_prune_record_redirect(redirected, nredirected,
									   rootoffnum,
									   chainitems[i]);
		}
	}
	else if (nchain < 2 && ItemIdIsRedirected(rootlp))
	{
		/*
		 * We found a redirect item that doesn't point to a valid follow-on
		 * item.  This can happen if the loop in heap_page_prune caused us to
		 * visit the dead successor of a redirect item before visiting the
		 * redirect item.  We can clean up by setting the redirect item to
		 * DEAD state.
		 */
		ItemIdSetDead(rootlp);
		heap_prune_record_dead(nowdead, ndead, rootoffnum);
	}

	/*
	 * If requested, eliminate LP_REDIRECT items by moving tuples.	Note that
	 * if the root item is LP_REDIRECT and doesn't point to a valid follow-on
	 * item, we already killed it above.
	 */
	if (redirect_move && ItemIdIsRedirected(rootlp))
	{
		OffsetNumber firstoffnum = ItemIdGetRedirect(rootlp);
		ItemId		firstlp = PageGetItemId(dp, firstoffnum);
		HeapTupleData firsttup;

		Assert(ItemIdIsNormal(firstlp));
		/* Set up firsttup to reference the tuple at its existing CTID */
		firsttup.t_data = (HeapTupleHeader) PageGetItem(dp, firstlp);
		firsttup.t_len = ItemIdGetLength(firstlp);
		ItemPointerSet(&firsttup.t_self,
					   BufferGetBlockNumber(buffer),
					   firstoffnum);
		firsttup.t_tableOid = RelationGetRelid(relation);

		/*
		 * Mark the tuple for invalidation.  Needed because we're changing its
		 * CTID.
		 */
		CacheInvalidateHeapTuple(relation, &firsttup);

		/*
		 * Change heap-only status of the tuple because after the line pointer
		 * manipulation, it's no longer a heap-only tuple, but is directly
		 * pointed to by index entries.
		 */
		Assert(HeapTupleIsHeapOnly(&firsttup));
		HeapTupleClearHeapOnly(&firsttup);

		/* Now move the item pointer */
		*rootlp = *firstlp;
		ItemIdSetUnused(firstlp);

		/*
		 * If latestdead is valid, we have already recorded the redirection
		 * above.  Otherwise, do it now.
		 *
		 * We don't record firstlp in the nowunused[] array, since the
		 * redirection entry is enough to tell heap_xlog_clean what to do.
		 */
		if (!OffsetNumberIsValid(latestdead))
			heap_prune_record_redirect(redirected, nredirected, rootoffnum,
									   firstoffnum);
	}

	return ndeleted;
}


/* Record newly-redirected item pointer */
static void
heap_prune_record_redirect(OffsetNumber *redirected, int *nredirected,
						   OffsetNumber offnum, OffsetNumber rdoffnum)
{
	Assert(*nredirected < MaxHeapTuplesPerPage);
	redirected[*nredirected * 2] = offnum;
	redirected[*nredirected * 2 + 1] = rdoffnum;
	(*nredirected)++;
}

/* Record newly-dead item pointer */
static void
heap_prune_record_dead(OffsetNumber *nowdead, int *ndead,
					   OffsetNumber offnum)
{
	Assert(*ndead < MaxHeapTuplesPerPage);
	nowdead[*ndead] = offnum;
	(*ndead)++;
}

/* Record newly-unused item pointer */
static void
heap_prune_record_unused(OffsetNumber *nowunused, int *nunused,
						 OffsetNumber offnum)
{
	Assert(*nunused < MaxHeapTuplesPerPage);
	nowunused[*nunused] = offnum;
	(*nunused)++;
}


/*
 * For all items in this page, find their respective root line pointers.
 * If item k is part of a HOT-chain with root at item j, then we set
 * root_offsets[k - 1] = j.
 *
 * The passed-in root_offsets array must have MaxHeapTuplesPerPage entries.
 * We zero out all unused entries.
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

	MemSet(root_offsets, 0, MaxHeapTuplesPerPage * sizeof(OffsetNumber));

	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = FirstOffsetNumber; offnum <= maxoff; offnum++)
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
			priorXmax = HeapTupleHeaderGetXmax(htup);
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

			nextoffnum = ItemPointerGetOffsetNumber(&htup->t_ctid);
			priorXmax = HeapTupleHeaderGetXmax(htup);
		}
	}
}
