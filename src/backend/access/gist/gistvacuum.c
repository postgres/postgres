/*-------------------------------------------------------------------------
 *
 * gistvacuum.c
 *	  vacuuming routines for the postgres GiST index access method.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/gist/gistvacuum.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/gist_private.h"
#include "access/transam.h"
#include "commands/vacuum.h"
#include "lib/integerset.h"
#include "miscadmin.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"

/* Working state needed by gistbulkdelete */
typedef struct
{
	IndexVacuumInfo *info;
	IndexBulkDeleteResult *stats;
	IndexBulkDeleteCallback callback;
	void	   *callback_state;
	GistNSN		startNSN;

	/*
	 * These are used to memorize all internal and empty leaf pages.  They are
	 * used for deleting all the empty pages.
	 */
	IntegerSet *internal_page_set;
	IntegerSet *empty_leaf_set;
	MemoryContext page_set_context;
} GistVacState;

static void gistvacuumscan(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
						   IndexBulkDeleteCallback callback, void *callback_state);
static void gistvacuumpage(GistVacState *vstate, BlockNumber blkno,
						   BlockNumber orig_blkno);
static void gistvacuum_delete_empty_pages(IndexVacuumInfo *info,
										  GistVacState *vstate);
static bool gistdeletepage(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
						   Buffer buffer, OffsetNumber downlink,
						   Buffer leafBuffer);

/*
 * VACUUM bulkdelete stage: remove index entries.
 */
IndexBulkDeleteResult *
gistbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			   IndexBulkDeleteCallback callback, void *callback_state)
{
	/* allocate stats if first time through, else re-use existing struct */
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	gistvacuumscan(info, stats, callback, callback_state);

	return stats;
}

/*
 * VACUUM cleanup stage: delete empty pages, and update index statistics.
 */
IndexBulkDeleteResult *
gistvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	/* No-op in ANALYZE ONLY mode */
	if (info->analyze_only)
		return stats;

	/*
	 * If gistbulkdelete was called, we need not do anything, just return the
	 * stats from the latest gistbulkdelete call.  If it wasn't called, we
	 * still need to do a pass over the index, to obtain index statistics.
	 */
	if (stats == NULL)
	{
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
		gistvacuumscan(info, stats, NULL, NULL);
	}

	/*
	 * It's quite possible for us to be fooled by concurrent page splits into
	 * double-counting some index tuples, so disbelieve any total that exceeds
	 * the underlying heap's count ... if we know that accurately.  Otherwise
	 * this might just make matters worse.
	 */
	if (!info->estimated_count)
	{
		if (stats->num_index_tuples > info->num_heap_tuples)
			stats->num_index_tuples = info->num_heap_tuples;
	}

	return stats;
}

/*
 * gistvacuumscan --- scan the index for VACUUMing purposes
 *
 * This scans the index for leaf tuples that are deletable according to the
 * vacuum callback, and updates the stats.  Both btbulkdelete and
 * btvacuumcleanup invoke this (the latter only if no btbulkdelete call
 * occurred).
 *
 * This also makes note of any empty leaf pages, as well as all internal
 * pages while looping over all index pages.  After scanning all the pages, we
 * remove the empty pages so that they can be reused.  Any deleted pages are
 * added directly to the free space map.  (They should've been added there
 * when they were originally deleted, already, but it's possible that the FSM
 * was lost at a crash, for example.)
 *
 * The caller is responsible for initially allocating/zeroing a stats struct.
 */
static void
gistvacuumscan(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			   IndexBulkDeleteCallback callback, void *callback_state)
{
	Relation	rel = info->index;
	GistVacState vstate;
	BlockNumber num_pages;
	bool		needLock;
	BlockNumber blkno;
	MemoryContext oldctx;

	/*
	 * Reset fields that track information about the entire index now.  This
	 * avoids double-counting in the case where a single VACUUM command
	 * requires multiple scans of the index.
	 *
	 * Avoid resetting the tuples_removed and pages_newly_deleted fields here,
	 * since they track information about the VACUUM command, and so must last
	 * across each call to gistvacuumscan().
	 *
	 * (Note that pages_free is treated as state about the whole index, not
	 * the current VACUUM.  This is appropriate because RecordFreeIndexPage()
	 * calls are idempotent, and get repeated for the same deleted pages in
	 * some scenarios.  The point for us is to track the number of recyclable
	 * pages in the index at the end of the VACUUM command.)
	 */
	stats->num_pages = 0;
	stats->estimated_count = false;
	stats->num_index_tuples = 0;
	stats->pages_deleted = 0;
	stats->pages_free = 0;

	/*
	 * Create the integer sets to remember all the internal and the empty leaf
	 * pages in page_set_context.  Internally, the integer set will remember
	 * this context so that the subsequent allocations for these integer sets
	 * will be done from the same context.
	 */
	vstate.page_set_context = GenerationContextCreate(CurrentMemoryContext,
													  "GiST VACUUM page set context",
													  16 * 1024);
	oldctx = MemoryContextSwitchTo(vstate.page_set_context);
	vstate.internal_page_set = intset_create();
	vstate.empty_leaf_set = intset_create();
	MemoryContextSwitchTo(oldctx);

	/* Set up info to pass down to gistvacuumpage */
	vstate.info = info;
	vstate.stats = stats;
	vstate.callback = callback;
	vstate.callback_state = callback_state;
	if (RelationNeedsWAL(rel))
		vstate.startNSN = GetInsertRecPtr();
	else
		vstate.startNSN = gistGetFakeLSN(rel);

	/*
	 * The outer loop iterates over all index pages, in physical order (we
	 * hope the kernel will cooperate in providing read-ahead for speed).  It
	 * is critical that we visit all leaf pages, including ones added after we
	 * start the scan, else we might fail to delete some deletable tuples.
	 * Hence, we must repeatedly check the relation length.  We must acquire
	 * the relation-extension lock while doing so to avoid a race condition:
	 * if someone else is extending the relation, there is a window where
	 * bufmgr/smgr have created a new all-zero page but it hasn't yet been
	 * write-locked by gistNewBuffer().  If we manage to scan such a page
	 * here, we'll improperly assume it can be recycled.  Taking the lock
	 * synchronizes things enough to prevent a problem: either num_pages won't
	 * include the new page, or gistNewBuffer already has write lock on the
	 * buffer and it will be fully initialized before we can examine it.  (See
	 * also vacuumlazy.c, which has the same issue.)  Also, we need not worry
	 * if a page is added immediately after we look; the page splitting code
	 * already has write-lock on the left page before it adds a right page, so
	 * we must already have processed any tuples due to be moved into such a
	 * page.
	 *
	 * We can skip locking for new or temp relations, however, since no one
	 * else could be accessing them.
	 */
	needLock = !RELATION_IS_LOCAL(rel);

	blkno = GIST_ROOT_BLKNO;
	for (;;)
	{
		/* Get the current relation length */
		if (needLock)
			LockRelationForExtension(rel, ExclusiveLock);
		num_pages = RelationGetNumberOfBlocks(rel);
		if (needLock)
			UnlockRelationForExtension(rel, ExclusiveLock);

		/* Quit if we've scanned the whole relation */
		if (blkno >= num_pages)
			break;
		/* Iterate over pages, then loop back to recheck length */
		for (; blkno < num_pages; blkno++)
			gistvacuumpage(&vstate, blkno, blkno);
	}

	/*
	 * If we found any recyclable pages (and recorded them in the FSM), then
	 * forcibly update the upper-level FSM pages to ensure that searchers can
	 * find them.  It's possible that the pages were also found during
	 * previous scans and so this is a waste of time, but it's cheap enough
	 * relative to scanning the index that it shouldn't matter much, and
	 * making sure that free pages are available sooner not later seems
	 * worthwhile.
	 *
	 * Note that if no recyclable pages exist, we don't bother vacuuming the
	 * FSM at all.
	 */
	if (stats->pages_free > 0)
		IndexFreeSpaceMapVacuum(rel);

	/* update statistics */
	stats->num_pages = num_pages;

	/*
	 * If we saw any empty pages, try to unlink them from the tree so that
	 * they can be reused.
	 */
	gistvacuum_delete_empty_pages(info, &vstate);

	/* we don't need the internal and empty page sets anymore */
	MemoryContextDelete(vstate.page_set_context);
	vstate.page_set_context = NULL;
	vstate.internal_page_set = NULL;
	vstate.empty_leaf_set = NULL;
}

/*
 * gistvacuumpage --- VACUUM one page
 *
 * This processes a single page for gistbulkdelete().  In some cases we
 * must go back and re-examine previously-scanned pages; this routine
 * recurses when necessary to handle that case.
 *
 * blkno is the page to process.  orig_blkno is the highest block number
 * reached by the outer gistvacuumscan loop (the same as blkno, unless we
 * are recursing to re-examine a previous page).
 */
static void
gistvacuumpage(GistVacState *vstate, BlockNumber blkno, BlockNumber orig_blkno)
{
	IndexVacuumInfo *info = vstate->info;
	IndexBulkDeleteCallback callback = vstate->callback;
	void	   *callback_state = vstate->callback_state;
	Relation	rel = info->index;
	Buffer		buffer;
	Page		page;
	BlockNumber recurse_to;

restart:
	recurse_to = InvalidBlockNumber;

	/* call vacuum_delay_point while not holding any buffer lock */
	vacuum_delay_point();

	buffer = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL,
								info->strategy);

	/*
	 * We are not going to stay here for a long time, aggressively grab an
	 * exclusive lock.
	 */
	LockBuffer(buffer, GIST_EXCLUSIVE);
	page = (Page) BufferGetPage(buffer);

	if (gistPageRecyclable(page))
	{
		/* Okay to recycle this page */
		RecordFreeIndexPage(rel, blkno);
		vstate->stats->pages_deleted++;
		vstate->stats->pages_free++;
	}
	else if (GistPageIsDeleted(page))
	{
		/* Already deleted, but can't recycle yet */
		vstate->stats->pages_deleted++;
	}
	else if (GistPageIsLeaf(page))
	{
		OffsetNumber todelete[MaxOffsetNumber];
		int			ntodelete = 0;
		int			nremain;
		GISTPageOpaque opaque = GistPageGetOpaque(page);
		OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

		/*
		 * Check whether we need to recurse back to earlier pages.  What we
		 * are concerned about is a page split that happened since we started
		 * the vacuum scan.  If the split moved some tuples to a lower page
		 * then we might have missed 'em.  If so, set up for tail recursion.
		 *
		 * This is similar to the checks we do during searches, when following
		 * a downlink, but we don't need to jump to higher-numbered pages,
		 * because we will process them later, anyway.
		 */
		if ((GistFollowRight(page) ||
			 vstate->startNSN < GistPageGetNSN(page)) &&
			(opaque->rightlink != InvalidBlockNumber) &&
			(opaque->rightlink < orig_blkno))
		{
			recurse_to = opaque->rightlink;
		}

		/*
		 * Scan over all items to see which ones need to be deleted according
		 * to the callback function.
		 */
		if (callback)
		{
			OffsetNumber off;

			for (off = FirstOffsetNumber;
				 off <= maxoff;
				 off = OffsetNumberNext(off))
			{
				ItemId		iid = PageGetItemId(page, off);
				IndexTuple	idxtuple = (IndexTuple) PageGetItem(page, iid);

				if (callback(&(idxtuple->t_tid), callback_state))
					todelete[ntodelete++] = off;
			}
		}

		/*
		 * Apply any needed deletes.  We issue just one WAL record per page,
		 * so as to minimize WAL traffic.
		 */
		if (ntodelete > 0)
		{
			START_CRIT_SECTION();

			MarkBufferDirty(buffer);

			PageIndexMultiDelete(page, todelete, ntodelete);
			GistMarkTuplesDeleted(page);

			if (RelationNeedsWAL(rel))
			{
				XLogRecPtr	recptr;

				recptr = gistXLogUpdate(buffer,
										todelete, ntodelete,
										NULL, 0, InvalidBuffer);
				PageSetLSN(page, recptr);
			}
			else
				PageSetLSN(page, gistGetFakeLSN(rel));

			END_CRIT_SECTION();

			vstate->stats->tuples_removed += ntodelete;
			/* must recompute maxoff */
			maxoff = PageGetMaxOffsetNumber(page);
		}

		nremain = maxoff - FirstOffsetNumber + 1;
		if (nremain == 0)
		{
			/*
			 * The page is now completely empty.  Remember its block number,
			 * so that we will try to delete the page in the second stage.
			 *
			 * Skip this when recursing, because IntegerSet requires that the
			 * values are added in ascending order.  The next VACUUM will pick
			 * it up.
			 */
			if (blkno == orig_blkno)
				intset_add_member(vstate->empty_leaf_set, blkno);
		}
		else
			vstate->stats->num_index_tuples += nremain;
	}
	else
	{
		/*
		 * On an internal page, check for "invalid tuples", left behind by an
		 * incomplete page split on PostgreSQL 9.0 or below.  These are not
		 * created by newer PostgreSQL versions, but unfortunately, there is
		 * no version number anywhere in a GiST index, so we don't know
		 * whether this index might still contain invalid tuples or not.
		 */
		OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
		OffsetNumber off;

		for (off = FirstOffsetNumber;
			 off <= maxoff;
			 off = OffsetNumberNext(off))
		{
			ItemId		iid = PageGetItemId(page, off);
			IndexTuple	idxtuple = (IndexTuple) PageGetItem(page, iid);

			if (GistTupleIsInvalid(idxtuple))
				ereport(LOG,
						(errmsg("index \"%s\" contains an inner tuple marked as invalid",
								RelationGetRelationName(rel)),
						 errdetail("This is caused by an incomplete page split at crash recovery before upgrading to PostgreSQL 9.1."),
						 errhint("Please REINDEX it.")));
		}

		/*
		 * Remember the block number of this page, so that we can revisit it
		 * later in gistvacuum_delete_empty_pages(), when we search for
		 * parents of empty leaf pages.
		 */
		if (blkno == orig_blkno)
			intset_add_member(vstate->internal_page_set, blkno);
	}

	UnlockReleaseBuffer(buffer);

	/*
	 * This is really tail recursion, but if the compiler is too stupid to
	 * optimize it as such, we'd eat an uncomfortably large amount of stack
	 * space per recursion level (due to the deletable[] array).  A failure is
	 * improbable since the number of levels isn't likely to be large ... but
	 * just in case, let's hand-optimize into a loop.
	 */
	if (recurse_to != InvalidBlockNumber)
	{
		blkno = recurse_to;
		goto restart;
	}
}

/*
 * Scan all internal pages, and try to delete their empty child pages.
 */
static void
gistvacuum_delete_empty_pages(IndexVacuumInfo *info, GistVacState *vstate)
{
	Relation	rel = info->index;
	BlockNumber empty_pages_remaining;
	uint64		blkno;

	/*
	 * Rescan all inner pages to find those that have empty child pages.
	 */
	empty_pages_remaining = intset_num_entries(vstate->empty_leaf_set);
	intset_begin_iterate(vstate->internal_page_set);
	while (empty_pages_remaining > 0 &&
		   intset_iterate_next(vstate->internal_page_set, &blkno))
	{
		Buffer		buffer;
		Page		page;
		OffsetNumber off,
					maxoff;
		OffsetNumber todelete[MaxOffsetNumber];
		BlockNumber leafs_to_delete[MaxOffsetNumber];
		int			ntodelete;
		int			deleted;

		buffer = ReadBufferExtended(rel, MAIN_FORKNUM, (BlockNumber) blkno,
									RBM_NORMAL, info->strategy);

		LockBuffer(buffer, GIST_SHARE);
		page = (Page) BufferGetPage(buffer);

		if (PageIsNew(page) || GistPageIsDeleted(page) || GistPageIsLeaf(page))
		{
			/*
			 * This page was an internal page earlier, but now it's something
			 * else. Shouldn't happen...
			 */
			Assert(false);
			UnlockReleaseBuffer(buffer);
			continue;
		}

		/*
		 * Scan all the downlinks, and see if any of them point to empty leaf
		 * pages.
		 */
		maxoff = PageGetMaxOffsetNumber(page);
		ntodelete = 0;
		for (off = FirstOffsetNumber;
			 off <= maxoff && ntodelete < maxoff - 1;
			 off = OffsetNumberNext(off))
		{
			ItemId		iid = PageGetItemId(page, off);
			IndexTuple	idxtuple = (IndexTuple) PageGetItem(page, iid);
			BlockNumber leafblk;

			leafblk = ItemPointerGetBlockNumber(&(idxtuple->t_tid));
			if (intset_is_member(vstate->empty_leaf_set, leafblk))
			{
				leafs_to_delete[ntodelete] = leafblk;
				todelete[ntodelete++] = off;
			}
		}

		/*
		 * In order to avoid deadlock, child page must be locked before
		 * parent, so we must release the lock on the parent, lock the child,
		 * and then re-acquire the lock the parent.  (And we wouldn't want to
		 * do I/O, while holding a lock, anyway.)
		 *
		 * At the instant that we're not holding a lock on the parent, the
		 * downlink might get moved by a concurrent insert, so we must
		 * re-check that it still points to the same child page after we have
		 * acquired both locks.  Also, another backend might have inserted a
		 * tuple to the page, so that it is no longer empty.  gistdeletepage()
		 * re-checks all these conditions.
		 */
		LockBuffer(buffer, GIST_UNLOCK);

		deleted = 0;
		for (int i = 0; i < ntodelete; i++)
		{
			Buffer		leafbuf;

			/*
			 * Don't remove the last downlink from the parent.  That would
			 * confuse the insertion code.
			 */
			if (PageGetMaxOffsetNumber(page) == FirstOffsetNumber)
				break;

			leafbuf = ReadBufferExtended(rel, MAIN_FORKNUM, leafs_to_delete[i],
										 RBM_NORMAL, info->strategy);
			LockBuffer(leafbuf, GIST_EXCLUSIVE);
			gistcheckpage(rel, leafbuf);

			LockBuffer(buffer, GIST_EXCLUSIVE);
			if (gistdeletepage(info, vstate->stats,
							   buffer, todelete[i] - deleted,
							   leafbuf))
				deleted++;
			LockBuffer(buffer, GIST_UNLOCK);

			UnlockReleaseBuffer(leafbuf);
		}

		ReleaseBuffer(buffer);

		/*
		 * We can stop the scan as soon as we have seen the downlinks, even if
		 * we were not able to remove them all.
		 */
		empty_pages_remaining -= ntodelete;
	}
}

/*
 * gistdeletepage takes a leaf page, and its parent, and tries to delete the
 * leaf.  Both pages must be locked.
 *
 * Even if the page was empty when we first saw it, a concurrent inserter might
 * have added a tuple to it since.  Similarly, the downlink might have moved.
 * We re-check all the conditions, to make sure the page is still deletable,
 * before modifying anything.
 *
 * Returns true, if the page was deleted, and false if a concurrent update
 * prevented it.
 */
static bool
gistdeletepage(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			   Buffer parentBuffer, OffsetNumber downlink,
			   Buffer leafBuffer)
{
	Page		parentPage = BufferGetPage(parentBuffer);
	Page		leafPage = BufferGetPage(leafBuffer);
	ItemId		iid;
	IndexTuple	idxtuple;
	XLogRecPtr	recptr;
	FullTransactionId txid;

	/*
	 * Check that the leaf is still empty and deletable.
	 */
	if (!GistPageIsLeaf(leafPage))
	{
		/* a leaf page should never become a non-leaf page */
		Assert(false);
		return false;
	}

	if (GistFollowRight(leafPage))
		return false;			/* don't mess with a concurrent page split */

	if (PageGetMaxOffsetNumber(leafPage) != InvalidOffsetNumber)
		return false;			/* not empty anymore */

	/*
	 * Ok, the leaf is deletable.  Is the downlink in the parent page still
	 * valid?  It might have been moved by a concurrent insert.  We could try
	 * to re-find it by scanning the page again, possibly moving right if the
	 * was split.  But for now, let's keep it simple and just give up.  The
	 * next VACUUM will pick it up.
	 */
	if (PageIsNew(parentPage) || GistPageIsDeleted(parentPage) ||
		GistPageIsLeaf(parentPage))
	{
		/* shouldn't happen, internal pages are never deleted */
		Assert(false);
		return false;
	}

	if (PageGetMaxOffsetNumber(parentPage) < downlink
		|| PageGetMaxOffsetNumber(parentPage) <= FirstOffsetNumber)
		return false;

	iid = PageGetItemId(parentPage, downlink);
	idxtuple = (IndexTuple) PageGetItem(parentPage, iid);
	if (BufferGetBlockNumber(leafBuffer) !=
		ItemPointerGetBlockNumber(&(idxtuple->t_tid)))
		return false;

	/*
	 * All good, proceed with the deletion.
	 *
	 * The page cannot be immediately recycled, because in-progress scans that
	 * saw the downlink might still visit it.  Mark the page with the current
	 * next-XID counter, so that we know when it can be recycled.  Once that
	 * XID becomes older than GlobalXmin, we know that all scans that are
	 * currently in progress must have ended.  (That's much more conservative
	 * than needed, but let's keep it safe and simple.)
	 */
	txid = ReadNextFullTransactionId();

	START_CRIT_SECTION();

	/* mark the page as deleted */
	MarkBufferDirty(leafBuffer);
	GistPageSetDeleted(leafPage, txid);
	stats->pages_newly_deleted++;
	stats->pages_deleted++;

	/* remove the downlink from the parent */
	MarkBufferDirty(parentBuffer);
	PageIndexTupleDelete(parentPage, downlink);

	if (RelationNeedsWAL(info->index))
		recptr = gistXLogPageDelete(leafBuffer, txid, parentBuffer, downlink);
	else
		recptr = gistGetFakeLSN(info->index);
	PageSetLSN(parentPage, recptr);
	PageSetLSN(leafPage, recptr);

	END_CRIT_SECTION();

	return true;
}
