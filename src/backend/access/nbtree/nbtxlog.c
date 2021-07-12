/*-------------------------------------------------------------------------
 *
 * nbtxlog.c
 *	  WAL replay logic for btrees.
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtxlog.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/bufmask.h"
#include "access/nbtree.h"
#include "access/nbtxlog.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "storage/procarray.h"
#include "utils/memutils.h"

static MemoryContext opCtx;		/* working memory for operations */

/*
 * _bt_restore_page -- re-enter all the index tuples on a page
 *
 * The page is freshly init'd, and *from (length len) is a copy of what
 * had been its upper part (pd_upper to pd_special).  We assume that the
 * tuples had been added to the page in item-number order, and therefore
 * the one with highest item number appears first (lowest on the page).
 */
static void
_bt_restore_page(Page page, char *from, int len)
{
	IndexTupleData itupdata;
	Size		itemsz;
	char	   *end = from + len;
	Item		items[MaxIndexTuplesPerPage];
	uint16		itemsizes[MaxIndexTuplesPerPage];
	int			i;
	int			nitems;

	/*
	 * To get the items back in the original order, we add them to the page in
	 * reverse.  To figure out where one tuple ends and another begins, we
	 * have to scan them in forward order first.
	 */
	i = 0;
	while (from < end)
	{
		/*
		 * As we step through the items, 'from' won't always be properly
		 * aligned, so we need to use memcpy().  Further, we use Item (which
		 * is just a char*) here for our items array for the same reason;
		 * wouldn't want the compiler or anyone thinking that an item is
		 * aligned when it isn't.
		 */
		memcpy(&itupdata, from, sizeof(IndexTupleData));
		itemsz = IndexTupleSize(&itupdata);
		itemsz = MAXALIGN(itemsz);

		items[i] = (Item) from;
		itemsizes[i] = itemsz;
		i++;

		from += itemsz;
	}
	nitems = i;

	for (i = nitems - 1; i >= 0; i--)
	{
		if (PageAddItem(page, items[i], itemsizes[i], nitems - i,
						false, false) == InvalidOffsetNumber)
			elog(PANIC, "_bt_restore_page: cannot add item to page");
	}
}

static void
_bt_restore_meta(XLogReaderState *record, uint8 block_id)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	Buffer		metabuf;
	Page		metapg;
	BTMetaPageData *md;
	BTPageOpaque pageop;
	xl_btree_metadata *xlrec;
	char	   *ptr;
	Size		len;

	metabuf = XLogInitBufferForRedo(record, block_id);
	ptr = XLogRecGetBlockData(record, block_id, &len);

	Assert(len == sizeof(xl_btree_metadata));
	Assert(BufferGetBlockNumber(metabuf) == BTREE_METAPAGE);
	xlrec = (xl_btree_metadata *) ptr;
	metapg = BufferGetPage(metabuf);

	_bt_pageinit(metapg, BufferGetPageSize(metabuf));

	md = BTPageGetMeta(metapg);
	md->btm_magic = BTREE_MAGIC;
	md->btm_version = xlrec->version;
	md->btm_root = xlrec->root;
	md->btm_level = xlrec->level;
	md->btm_fastroot = xlrec->fastroot;
	md->btm_fastlevel = xlrec->fastlevel;
	/* Cannot log BTREE_MIN_VERSION index metapage without upgrade */
	Assert(md->btm_version >= BTREE_NOVAC_VERSION);
	md->btm_oldest_btpo_xact = xlrec->oldest_btpo_xact;
	md->btm_last_cleanup_num_heap_tuples = xlrec->last_cleanup_num_heap_tuples;
	md->btm_allequalimage = xlrec->allequalimage;

	pageop = (BTPageOpaque) PageGetSpecialPointer(metapg);
	pageop->btpo_flags = BTP_META;

	/*
	 * Set pd_lower just past the end of the metadata.  This is essential,
	 * because without doing so, metadata will be lost if xlog.c compresses
	 * the page.
	 */
	((PageHeader) metapg)->pd_lower =
		((char *) md + sizeof(BTMetaPageData)) - (char *) metapg;

	PageSetLSN(metapg, lsn);
	MarkBufferDirty(metabuf);
	UnlockReleaseBuffer(metabuf);
}

/*
 * _bt_clear_incomplete_split -- clear INCOMPLETE_SPLIT flag on a page
 *
 * This is a common subroutine of the redo functions of all the WAL record
 * types that can insert a downlink: insert, split, and newroot.
 */
static void
_bt_clear_incomplete_split(XLogReaderState *record, uint8 block_id)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	Buffer		buf;

	if (XLogReadBufferForRedo(record, block_id, &buf) == BLK_NEEDS_REDO)
	{
		Page		page = (Page) BufferGetPage(buf);
		BTPageOpaque pageop = (BTPageOpaque) PageGetSpecialPointer(page);

		Assert(P_INCOMPLETE_SPLIT(pageop));
		pageop->btpo_flags &= ~BTP_INCOMPLETE_SPLIT;

		PageSetLSN(page, lsn);
		MarkBufferDirty(buf);
	}
	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
}

static void
btree_xlog_insert(bool isleaf, bool ismeta, bool posting,
				  XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_btree_insert *xlrec = (xl_btree_insert *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;

	/*
	 * Insertion to an internal page finishes an incomplete split at the child
	 * level.  Clear the incomplete-split flag in the child.  Note: during
	 * normal operation, the child and parent pages are locked at the same
	 * time, so that clearing the flag and inserting the downlink appear
	 * atomic to other backends.  We don't bother with that during replay,
	 * because readers don't care about the incomplete-split flag and there
	 * cannot be updates happening.
	 */
	if (!isleaf)
		_bt_clear_incomplete_split(record, 1);
	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		Size		datalen;
		char	   *datapos = XLogRecGetBlockData(record, 0, &datalen);

		page = BufferGetPage(buffer);

		if (!posting)
		{
			/* Simple retail insertion */
			if (PageAddItem(page, (Item) datapos, datalen, xlrec->offnum,
							false, false) == InvalidOffsetNumber)
				elog(PANIC, "failed to add new item");
		}
		else
		{
			ItemId		itemid;
			IndexTuple	oposting,
						newitem,
						nposting;
			uint16		postingoff;

			/*
			 * A posting list split occurred during leaf page insertion.  WAL
			 * record data will start with an offset number representing the
			 * point in an existing posting list that a split occurs at.
			 *
			 * Use _bt_swap_posting() to repeat posting list split steps from
			 * primary.  Note that newitem from WAL record is 'orignewitem',
			 * not the final version of newitem that is actually inserted on
			 * page.
			 */
			postingoff = *((uint16 *) datapos);
			datapos += sizeof(uint16);
			datalen -= sizeof(uint16);

			itemid = PageGetItemId(page, OffsetNumberPrev(xlrec->offnum));
			oposting = (IndexTuple) PageGetItem(page, itemid);

			/* Use mutable, aligned newitem copy in _bt_swap_posting() */
			Assert(isleaf && postingoff > 0);
			newitem = CopyIndexTuple((IndexTuple) datapos);
			nposting = _bt_swap_posting(newitem, oposting, postingoff);

			/* Replace existing posting list with post-split version */
			memcpy(oposting, nposting, MAXALIGN(IndexTupleSize(nposting)));

			/* Insert "final" new item (not orignewitem from WAL stream) */
			Assert(IndexTupleSize(newitem) == datalen);
			if (PageAddItem(page, (Item) newitem, datalen, xlrec->offnum,
							false, false) == InvalidOffsetNumber)
				elog(PANIC, "failed to add posting split new item");
		}

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	/*
	 * Note: in normal operation, we'd update the metapage while still holding
	 * lock on the page we inserted into.  But during replay it's not
	 * necessary to hold that lock, since no other index updates can be
	 * happening concurrently, and readers will cope fine with following an
	 * obsolete link from the metapage.
	 */
	if (ismeta)
		_bt_restore_meta(record, 2);
}

static void
btree_xlog_split(bool newitemonleft, XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_btree_split *xlrec = (xl_btree_split *) XLogRecGetData(record);
	bool		isleaf = (xlrec->level == 0);
	Buffer		lbuf;
	Buffer		rbuf;
	Page		rpage;
	BTPageOpaque ropaque;
	char	   *datapos;
	Size		datalen;
	BlockNumber leftsib;
	BlockNumber rightsib;
	BlockNumber rnext;

	XLogRecGetBlockTag(record, 0, NULL, NULL, &leftsib);
	XLogRecGetBlockTag(record, 1, NULL, NULL, &rightsib);
	if (!XLogRecGetBlockTag(record, 2, NULL, NULL, &rnext))
		rnext = P_NONE;

	/*
	 * Clear the incomplete split flag on the left sibling of the child page
	 * this is a downlink for.  (Like in btree_xlog_insert, this can be done
	 * before locking the other pages)
	 */
	if (!isleaf)
		_bt_clear_incomplete_split(record, 3);

	/* Reconstruct right (new) sibling page from scratch */
	rbuf = XLogInitBufferForRedo(record, 1);
	datapos = XLogRecGetBlockData(record, 1, &datalen);
	rpage = (Page) BufferGetPage(rbuf);

	_bt_pageinit(rpage, BufferGetPageSize(rbuf));
	ropaque = (BTPageOpaque) PageGetSpecialPointer(rpage);

	ropaque->btpo_prev = leftsib;
	ropaque->btpo_next = rnext;
	ropaque->btpo.level = xlrec->level;
	ropaque->btpo_flags = isleaf ? BTP_LEAF : 0;
	ropaque->btpo_cycleid = 0;

	_bt_restore_page(rpage, datapos, datalen);

	PageSetLSN(rpage, lsn);
	MarkBufferDirty(rbuf);

	/* Now reconstruct left (original) sibling page */
	if (XLogReadBufferForRedo(record, 0, &lbuf) == BLK_NEEDS_REDO)
	{
		/*
		 * To retain the same physical order of the tuples that they had, we
		 * initialize a temporary empty page for the left page and add all the
		 * items to that in item number order.  This mirrors how _bt_split()
		 * works.  Retaining the same physical order makes WAL consistency
		 * checking possible.  See also _bt_restore_page(), which does the
		 * same for the right page.
		 */
		Page		lpage = (Page) BufferGetPage(lbuf);
		BTPageOpaque lopaque = (BTPageOpaque) PageGetSpecialPointer(lpage);
		OffsetNumber off;
		IndexTuple	newitem = NULL,
					left_hikey = NULL,
					nposting = NULL;
		Size		newitemsz = 0,
					left_hikeysz = 0;
		Page		newlpage;
		OffsetNumber leftoff,
					replacepostingoff = InvalidOffsetNumber;

		datapos = XLogRecGetBlockData(record, 0, &datalen);

		if (newitemonleft || xlrec->postingoff != 0)
		{
			newitem = (IndexTuple) datapos;
			newitemsz = MAXALIGN(IndexTupleSize(newitem));
			datapos += newitemsz;
			datalen -= newitemsz;

			if (xlrec->postingoff != 0)
			{
				ItemId		itemid;
				IndexTuple	oposting;

				/* Posting list must be at offset number before new item's */
				replacepostingoff = OffsetNumberPrev(xlrec->newitemoff);

				/* Use mutable, aligned newitem copy in _bt_swap_posting() */
				newitem = CopyIndexTuple(newitem);
				itemid = PageGetItemId(lpage, replacepostingoff);
				oposting = (IndexTuple) PageGetItem(lpage, itemid);
				nposting = _bt_swap_posting(newitem, oposting,
											xlrec->postingoff);
			}
		}

		/*
		 * Extract left hikey and its size.  We assume that 16-bit alignment
		 * is enough to apply IndexTupleSize (since it's fetching from a
		 * uint16 field).
		 */
		left_hikey = (IndexTuple) datapos;
		left_hikeysz = MAXALIGN(IndexTupleSize(left_hikey));
		datapos += left_hikeysz;
		datalen -= left_hikeysz;

		Assert(datalen == 0);

		newlpage = PageGetTempPageCopySpecial(lpage);

		/* Set high key */
		leftoff = P_HIKEY;
		if (PageAddItem(newlpage, (Item) left_hikey, left_hikeysz,
						P_HIKEY, false, false) == InvalidOffsetNumber)
			elog(PANIC, "failed to add high key to left page after split");
		leftoff = OffsetNumberNext(leftoff);

		for (off = P_FIRSTDATAKEY(lopaque); off < xlrec->firstrightoff; off++)
		{
			ItemId		itemid;
			Size		itemsz;
			IndexTuple	item;

			/* Add replacement posting list when required */
			if (off == replacepostingoff)
			{
				Assert(newitemonleft ||
					   xlrec->firstrightoff == xlrec->newitemoff);
				if (PageAddItem(newlpage, (Item) nposting,
								MAXALIGN(IndexTupleSize(nposting)), leftoff,
								false, false) == InvalidOffsetNumber)
					elog(ERROR, "failed to add new posting list item to left page after split");
				leftoff = OffsetNumberNext(leftoff);
				continue;		/* don't insert oposting */
			}

			/* add the new item if it was inserted on left page */
			else if (newitemonleft && off == xlrec->newitemoff)
			{
				if (PageAddItem(newlpage, (Item) newitem, newitemsz, leftoff,
								false, false) == InvalidOffsetNumber)
					elog(ERROR, "failed to add new item to left page after split");
				leftoff = OffsetNumberNext(leftoff);
			}

			itemid = PageGetItemId(lpage, off);
			itemsz = ItemIdGetLength(itemid);
			item = (IndexTuple) PageGetItem(lpage, itemid);
			if (PageAddItem(newlpage, (Item) item, itemsz, leftoff,
							false, false) == InvalidOffsetNumber)
				elog(ERROR, "failed to add old item to left page after split");
			leftoff = OffsetNumberNext(leftoff);
		}

		/* cope with possibility that newitem goes at the end */
		if (newitemonleft && off == xlrec->newitemoff)
		{
			if (PageAddItem(newlpage, (Item) newitem, newitemsz, leftoff,
							false, false) == InvalidOffsetNumber)
				elog(ERROR, "failed to add new item to left page after split");
			leftoff = OffsetNumberNext(leftoff);
		}

		PageRestoreTempPage(newlpage, lpage);

		/* Fix opaque fields */
		lopaque->btpo_flags = BTP_INCOMPLETE_SPLIT;
		if (isleaf)
			lopaque->btpo_flags |= BTP_LEAF;
		lopaque->btpo_next = rightsib;
		lopaque->btpo_cycleid = 0;

		PageSetLSN(lpage, lsn);
		MarkBufferDirty(lbuf);
	}

	/*
	 * We no longer need the buffers.  They must be released together, so that
	 * readers cannot observe two inconsistent halves.
	 */
	if (BufferIsValid(lbuf))
		UnlockReleaseBuffer(lbuf);
	UnlockReleaseBuffer(rbuf);

	/*
	 * Fix left-link of the page to the right of the new right sibling.
	 *
	 * Note: in normal operation, we do this while still holding lock on the
	 * two split pages.  However, that's not necessary for correctness in WAL
	 * replay, because no other index update can be in progress, and readers
	 * will cope properly when following an obsolete left-link.
	 */
	if (rnext != P_NONE)
	{
		Buffer		buffer;

		if (XLogReadBufferForRedo(record, 2, &buffer) == BLK_NEEDS_REDO)
		{
			Page		page = (Page) BufferGetPage(buffer);
			BTPageOpaque pageop = (BTPageOpaque) PageGetSpecialPointer(page);

			pageop->btpo_prev = rightsib;

			PageSetLSN(page, lsn);
			MarkBufferDirty(buffer);
		}
		if (BufferIsValid(buffer))
			UnlockReleaseBuffer(buffer);
	}
}

static void
btree_xlog_dedup(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_btree_dedup *xlrec = (xl_btree_dedup *) XLogRecGetData(record);
	Buffer		buf;

	if (XLogReadBufferForRedo(record, 0, &buf) == BLK_NEEDS_REDO)
	{
		char	   *ptr = XLogRecGetBlockData(record, 0, NULL);
		Page		page = (Page) BufferGetPage(buf);
		BTPageOpaque opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		OffsetNumber offnum,
					minoff,
					maxoff;
		BTDedupState state;
		BTDedupInterval *intervals;
		Page		newpage;

		state = (BTDedupState) palloc(sizeof(BTDedupStateData));
		state->deduplicate = true;	/* unused */
		state->nmaxitems = 0;	/* unused */
		/* Conservatively use larger maxpostingsize than primary */
		state->maxpostingsize = BTMaxItemSize(page);
		state->base = NULL;
		state->baseoff = InvalidOffsetNumber;
		state->basetupsize = 0;
		state->htids = palloc(state->maxpostingsize);
		state->nhtids = 0;
		state->nitems = 0;
		state->phystupsize = 0;
		state->nintervals = 0;

		minoff = P_FIRSTDATAKEY(opaque);
		maxoff = PageGetMaxOffsetNumber(page);
		newpage = PageGetTempPageCopySpecial(page);

		if (!P_RIGHTMOST(opaque))
		{
			ItemId		itemid = PageGetItemId(page, P_HIKEY);
			Size		itemsz = ItemIdGetLength(itemid);
			IndexTuple	item = (IndexTuple) PageGetItem(page, itemid);

			if (PageAddItem(newpage, (Item) item, itemsz, P_HIKEY,
							false, false) == InvalidOffsetNumber)
				elog(ERROR, "deduplication failed to add highkey");
		}

		intervals = (BTDedupInterval *) ptr;
		for (offnum = minoff;
			 offnum <= maxoff;
			 offnum = OffsetNumberNext(offnum))
		{
			ItemId		itemid = PageGetItemId(page, offnum);
			IndexTuple	itup = (IndexTuple) PageGetItem(page, itemid);

			if (offnum == minoff)
				_bt_dedup_start_pending(state, itup, offnum);
			else if (state->nintervals < xlrec->nintervals &&
					 state->baseoff == intervals[state->nintervals].baseoff &&
					 state->nitems < intervals[state->nintervals].nitems)
			{
				if (!_bt_dedup_save_htid(state, itup))
					elog(ERROR, "deduplication failed to add heap tid to pending posting list");
			}
			else
			{
				_bt_dedup_finish_pending(newpage, state);
				_bt_dedup_start_pending(state, itup, offnum);
			}
		}

		_bt_dedup_finish_pending(newpage, state);
		Assert(state->nintervals == xlrec->nintervals);
		Assert(memcmp(state->intervals, intervals,
					  state->nintervals * sizeof(BTDedupInterval)) == 0);

		if (P_HAS_GARBAGE(opaque))
		{
			BTPageOpaque nopaque = (BTPageOpaque) PageGetSpecialPointer(newpage);

			nopaque->btpo_flags &= ~BTP_HAS_GARBAGE;
		}

		PageRestoreTempPage(newpage, page);
		PageSetLSN(page, lsn);
		MarkBufferDirty(buf);
	}

	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
}

static void
btree_xlog_vacuum(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_btree_vacuum *xlrec = (xl_btree_vacuum *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	BTPageOpaque opaque;

	/*
	 * We need to take a cleanup lock here, just like btvacuumpage(). However,
	 * it isn't necessary to exhaustively get a cleanup lock on every block in
	 * the index during recovery (just getting a cleanup lock on pages with
	 * items to kill suffices).  See nbtree/README for details.
	 */
	if (XLogReadBufferForRedoExtended(record, 0, RBM_NORMAL, true, &buffer)
		== BLK_NEEDS_REDO)
	{
		char	   *ptr = XLogRecGetBlockData(record, 0, NULL);

		page = (Page) BufferGetPage(buffer);

		if (xlrec->nupdated > 0)
		{
			OffsetNumber *updatedoffsets;
			xl_btree_update *updates;

			updatedoffsets = (OffsetNumber *)
				(ptr + xlrec->ndeleted * sizeof(OffsetNumber));
			updates = (xl_btree_update *) ((char *) updatedoffsets +
										   xlrec->nupdated *
										   sizeof(OffsetNumber));

			for (int i = 0; i < xlrec->nupdated; i++)
			{
				BTVacuumPosting vacposting;
				IndexTuple	origtuple;
				ItemId		itemid;
				Size		itemsz;

				itemid = PageGetItemId(page, updatedoffsets[i]);
				origtuple = (IndexTuple) PageGetItem(page, itemid);

				vacposting = palloc(offsetof(BTVacuumPostingData, deletetids) +
									updates->ndeletedtids * sizeof(uint16));
				vacposting->updatedoffset = updatedoffsets[i];
				vacposting->itup = origtuple;
				vacposting->ndeletedtids = updates->ndeletedtids;
				memcpy(vacposting->deletetids,
					   (char *) updates + SizeOfBtreeUpdate,
					   updates->ndeletedtids * sizeof(uint16));

				_bt_update_posting(vacposting);

				/* Overwrite updated version of tuple */
				itemsz = MAXALIGN(IndexTupleSize(vacposting->itup));
				if (!PageIndexTupleOverwrite(page, updatedoffsets[i],
											 (Item) vacposting->itup, itemsz))
					elog(PANIC, "failed to update partially dead item");

				pfree(vacposting->itup);
				pfree(vacposting);

				/* advance to next xl_btree_update from array */
				updates = (xl_btree_update *)
					((char *) updates + SizeOfBtreeUpdate +
					 updates->ndeletedtids * sizeof(uint16));
			}
		}

		if (xlrec->ndeleted > 0)
			PageIndexMultiDelete(page, (OffsetNumber *) ptr, xlrec->ndeleted);

		/*
		 * Mark the page as not containing any LP_DEAD items --- see comments
		 * in _bt_delitems_vacuum().
		 */
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		opaque->btpo_flags &= ~BTP_HAS_GARBAGE;

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
btree_xlog_delete(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_btree_delete *xlrec = (xl_btree_delete *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	BTPageOpaque opaque;

	/*
	 * If we have any conflict processing to do, it must happen before we
	 * update the page
	 */
	if (InHotStandby)
	{
		RelFileNode rnode;

		XLogRecGetBlockTag(record, 0, &rnode, NULL, NULL);

		ResolveRecoveryConflictWithSnapshot(xlrec->latestRemovedXid, rnode);
	}

	/*
	 * We don't need to take a cleanup lock to apply these changes. See
	 * nbtree/README for details.
	 */
	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		char	   *ptr = XLogRecGetBlockData(record, 0, NULL);

		page = (Page) BufferGetPage(buffer);

		PageIndexMultiDelete(page, (OffsetNumber *) ptr, xlrec->ndeleted);

		/* Mark the page as not containing any LP_DEAD items */
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		opaque->btpo_flags &= ~BTP_HAS_GARBAGE;

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
btree_xlog_mark_page_halfdead(uint8 info, XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_btree_mark_page_halfdead *xlrec = (xl_btree_mark_page_halfdead *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	BTPageOpaque pageop;
	IndexTupleData trunctuple;

	/*
	 * In normal operation, we would lock all the pages this WAL record
	 * touches before changing any of them.  In WAL replay, it should be okay
	 * to lock just one page at a time, since no concurrent index updates can
	 * be happening, and readers should not care whether they arrive at the
	 * target page or not (since it's surely empty).
	 */

	/* to-be-deleted subtree's parent page */
	if (XLogReadBufferForRedo(record, 1, &buffer) == BLK_NEEDS_REDO)
	{
		OffsetNumber poffset;
		ItemId		itemid;
		IndexTuple	itup;
		OffsetNumber nextoffset;
		BlockNumber rightsib;

		page = (Page) BufferGetPage(buffer);
		pageop = (BTPageOpaque) PageGetSpecialPointer(page);

		poffset = xlrec->poffset;

		nextoffset = OffsetNumberNext(poffset);
		itemid = PageGetItemId(page, nextoffset);
		itup = (IndexTuple) PageGetItem(page, itemid);
		rightsib = BTreeTupleGetDownLink(itup);

		itemid = PageGetItemId(page, poffset);
		itup = (IndexTuple) PageGetItem(page, itemid);
		BTreeTupleSetDownLink(itup, rightsib);
		nextoffset = OffsetNumberNext(poffset);
		PageIndexTupleDelete(page, nextoffset);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	/* Rewrite the leaf page as a halfdead page */
	buffer = XLogInitBufferForRedo(record, 0);
	page = (Page) BufferGetPage(buffer);

	_bt_pageinit(page, BufferGetPageSize(buffer));
	pageop = (BTPageOpaque) PageGetSpecialPointer(page);

	pageop->btpo_prev = xlrec->leftblk;
	pageop->btpo_next = xlrec->rightblk;
	pageop->btpo.level = 0;
	pageop->btpo_flags = BTP_HALF_DEAD | BTP_LEAF;
	pageop->btpo_cycleid = 0;

	/*
	 * Construct a dummy high key item that points to top parent page (value
	 * is InvalidBlockNumber when the top parent page is the leaf page itself)
	 */
	MemSet(&trunctuple, 0, sizeof(IndexTupleData));
	trunctuple.t_info = sizeof(IndexTupleData);
	BTreeTupleSetTopParent(&trunctuple, xlrec->topparent);

	if (PageAddItem(page, (Item) &trunctuple, sizeof(IndexTupleData), P_HIKEY,
					false, false) == InvalidOffsetNumber)
		elog(ERROR, "could not add dummy high key to half-dead page");

	PageSetLSN(page, lsn);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
}


static void
btree_xlog_unlink_page(uint8 info, XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_btree_unlink_page *xlrec = (xl_btree_unlink_page *) XLogRecGetData(record);
	BlockNumber leftsib;
	BlockNumber rightsib;
	Buffer		buffer;
	Page		page;
	BTPageOpaque pageop;

	leftsib = xlrec->leftsib;
	rightsib = xlrec->rightsib;

	/*
	 * In normal operation, we would lock all the pages this WAL record
	 * touches before changing any of them.  In WAL replay, it should be okay
	 * to lock just one page at a time, since no concurrent index updates can
	 * be happening, and readers should not care whether they arrive at the
	 * target page or not (since it's surely empty).
	 */

	/* Fix left-link of right sibling */
	if (XLogReadBufferForRedo(record, 2, &buffer) == BLK_NEEDS_REDO)
	{
		page = (Page) BufferGetPage(buffer);
		pageop = (BTPageOpaque) PageGetSpecialPointer(page);
		pageop->btpo_prev = leftsib;

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	/* Fix right-link of left sibling, if any */
	if (leftsib != P_NONE)
	{
		if (XLogReadBufferForRedo(record, 1, &buffer) == BLK_NEEDS_REDO)
		{
			page = (Page) BufferGetPage(buffer);
			pageop = (BTPageOpaque) PageGetSpecialPointer(page);
			pageop->btpo_next = rightsib;

			PageSetLSN(page, lsn);
			MarkBufferDirty(buffer);
		}
		if (BufferIsValid(buffer))
			UnlockReleaseBuffer(buffer);
	}

	/* Rewrite target page as empty deleted page */
	buffer = XLogInitBufferForRedo(record, 0);
	page = (Page) BufferGetPage(buffer);

	_bt_pageinit(page, BufferGetPageSize(buffer));
	pageop = (BTPageOpaque) PageGetSpecialPointer(page);

	pageop->btpo_prev = leftsib;
	pageop->btpo_next = rightsib;
	pageop->btpo.xact = xlrec->btpo_xact;
	pageop->btpo_flags = BTP_DELETED;
	pageop->btpo_cycleid = 0;

	PageSetLSN(page, lsn);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);

	/*
	 * If we deleted a parent of the targeted leaf page, instead of the leaf
	 * itself, update the leaf to point to the next remaining child in the
	 * to-be-deleted subtree
	 */
	if (XLogRecHasBlockRef(record, 3))
	{
		/*
		 * There is no real data on the page, so we just re-create it from
		 * scratch using the information from the WAL record.
		 */
		IndexTupleData trunctuple;

		buffer = XLogInitBufferForRedo(record, 3);
		page = (Page) BufferGetPage(buffer);

		_bt_pageinit(page, BufferGetPageSize(buffer));
		pageop = (BTPageOpaque) PageGetSpecialPointer(page);

		pageop->btpo_flags = BTP_HALF_DEAD | BTP_LEAF;
		pageop->btpo_prev = xlrec->leafleftsib;
		pageop->btpo_next = xlrec->leafrightsib;
		pageop->btpo.level = 0;
		pageop->btpo_cycleid = 0;

		/* Add a dummy hikey item */
		MemSet(&trunctuple, 0, sizeof(IndexTupleData));
		trunctuple.t_info = sizeof(IndexTupleData);
		BTreeTupleSetTopParent(&trunctuple, xlrec->topparent);

		if (PageAddItem(page, (Item) &trunctuple, sizeof(IndexTupleData), P_HIKEY,
						false, false) == InvalidOffsetNumber)
			elog(ERROR, "could not add dummy high key to half-dead page");

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
		UnlockReleaseBuffer(buffer);
	}

	/* Update metapage if needed */
	if (info == XLOG_BTREE_UNLINK_PAGE_META)
		_bt_restore_meta(record, 4);
}

static void
btree_xlog_newroot(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_btree_newroot *xlrec = (xl_btree_newroot *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	BTPageOpaque pageop;
	char	   *ptr;
	Size		len;

	buffer = XLogInitBufferForRedo(record, 0);
	page = (Page) BufferGetPage(buffer);

	_bt_pageinit(page, BufferGetPageSize(buffer));
	pageop = (BTPageOpaque) PageGetSpecialPointer(page);

	pageop->btpo_flags = BTP_ROOT;
	pageop->btpo_prev = pageop->btpo_next = P_NONE;
	pageop->btpo.level = xlrec->level;
	if (xlrec->level == 0)
		pageop->btpo_flags |= BTP_LEAF;
	pageop->btpo_cycleid = 0;

	if (xlrec->level > 0)
	{
		ptr = XLogRecGetBlockData(record, 0, &len);
		_bt_restore_page(page, ptr, len);

		/* Clear the incomplete-split flag in left child */
		_bt_clear_incomplete_split(record, 1);
	}

	PageSetLSN(page, lsn);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);

	_bt_restore_meta(record, 2);
}

static void
btree_xlog_reuse_page(XLogReaderState *record)
{
	xl_btree_reuse_page *xlrec = (xl_btree_reuse_page *) XLogRecGetData(record);

	/*
	 * Btree reuse_page records exist to provide a conflict point when we
	 * reuse pages in the index via the FSM.  That's all they do though.
	 *
	 * latestRemovedXid was the page's btpo.xact.  The btpo.xact <
	 * RecentGlobalXmin test in _bt_page_recyclable() conceptually mirrors the
	 * pgxact->xmin > limitXmin test in GetConflictingVirtualXIDs().
	 * Consequently, one XID value achieves the same exclusion effect on
	 * master and standby.
	 */
	if (InHotStandby)
	{
		ResolveRecoveryConflictWithSnapshot(xlrec->latestRemovedXid,
											xlrec->node);
	}
}

void
btree_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	MemoryContext oldCtx;

	oldCtx = MemoryContextSwitchTo(opCtx);
	switch (info)
	{
		case XLOG_BTREE_INSERT_LEAF:
			btree_xlog_insert(true, false, false, record);
			break;
		case XLOG_BTREE_INSERT_UPPER:
			btree_xlog_insert(false, false, false, record);
			break;
		case XLOG_BTREE_INSERT_META:
			btree_xlog_insert(false, true, false, record);
			break;
		case XLOG_BTREE_SPLIT_L:
			btree_xlog_split(true, record);
			break;
		case XLOG_BTREE_SPLIT_R:
			btree_xlog_split(false, record);
			break;
		case XLOG_BTREE_INSERT_POST:
			btree_xlog_insert(true, false, true, record);
			break;
		case XLOG_BTREE_DEDUP:
			btree_xlog_dedup(record);
			break;
		case XLOG_BTREE_VACUUM:
			btree_xlog_vacuum(record);
			break;
		case XLOG_BTREE_DELETE:
			btree_xlog_delete(record);
			break;
		case XLOG_BTREE_MARK_PAGE_HALFDEAD:
			btree_xlog_mark_page_halfdead(info, record);
			break;
		case XLOG_BTREE_UNLINK_PAGE:
		case XLOG_BTREE_UNLINK_PAGE_META:
			btree_xlog_unlink_page(info, record);
			break;
		case XLOG_BTREE_NEWROOT:
			btree_xlog_newroot(record);
			break;
		case XLOG_BTREE_REUSE_PAGE:
			btree_xlog_reuse_page(record);
			break;
		case XLOG_BTREE_META_CLEANUP:
			_bt_restore_meta(record, 0);
			break;
		default:
			elog(PANIC, "btree_redo: unknown op code %u", info);
	}
	MemoryContextSwitchTo(oldCtx);
	MemoryContextReset(opCtx);
}

void
btree_xlog_startup(void)
{
	opCtx = AllocSetContextCreate(CurrentMemoryContext,
								  "Btree recovery temporary context",
								  ALLOCSET_DEFAULT_SIZES);
}

void
btree_xlog_cleanup(void)
{
	MemoryContextDelete(opCtx);
	opCtx = NULL;
}

/*
 * Mask a btree page before performing consistency checks on it.
 */
void
btree_mask(char *pagedata, BlockNumber blkno)
{
	Page		page = (Page) pagedata;
	BTPageOpaque maskopaq;

	mask_page_lsn_and_checksum(page);

	mask_page_hint_bits(page);
	mask_unused_space(page);

	maskopaq = (BTPageOpaque) PageGetSpecialPointer(page);

	if (P_ISDELETED(maskopaq))
	{
		/*
		 * Mask page content on a DELETED page since it will be re-initialized
		 * during replay. See btree_xlog_unlink_page() for details.
		 */
		mask_page_content(page);
	}
	else if (P_ISLEAF(maskopaq))
	{
		/*
		 * In btree leaf pages, it is possible to modify the LP_FLAGS without
		 * emitting any WAL record. Hence, mask the line pointer flags. See
		 * _bt_killitems(), _bt_check_unique() for details.
		 */
		mask_lp_flags(page);
	}

	/*
	 * BTP_HAS_GARBAGE is just an un-logged hint bit. So, mask it. See
	 * _bt_killitems(), _bt_check_unique() for details.
	 */
	maskopaq->btpo_flags &= ~BTP_HAS_GARBAGE;

	/*
	 * During replay of a btree page split, we don't set the BTP_SPLIT_END
	 * flag of the right sibling and initialize the cycle_id to 0 for the same
	 * page. See btree_xlog_split() for details.
	 */
	maskopaq->btpo_flags &= ~BTP_SPLIT_END;
	maskopaq->btpo_cycleid = 0;
}
