/*-------------------------------------------------------------------------
 *
 * nbtxlog.c
 *	  WAL replay logic for btrees.
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtxlog.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam_xlog.h"
#include "access/nbtree.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "storage/procarray.h"
#include "miscadmin.h"

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
		/* Need to copy tuple header due to alignment considerations */
		memcpy(&itupdata, from, sizeof(IndexTupleData));
		itemsz = IndexTupleDSize(itupdata);
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
		from += itemsz;
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
	md->btm_version = BTREE_VERSION;
	md->btm_root = xlrec->root;
	md->btm_level = xlrec->level;
	md->btm_fastroot = xlrec->fastroot;
	md->btm_fastlevel = xlrec->fastlevel;

	pageop = (BTPageOpaque) PageGetSpecialPointer(metapg);
	pageop->btpo_flags = BTP_META;

	/*
	 * Set pd_lower just past the end of the metadata.  This is not essential
	 * but it makes the page look compressible to xlog.c.
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

		Assert((pageop->btpo_flags & BTP_INCOMPLETE_SPLIT) != 0);
		pageop->btpo_flags &= ~BTP_INCOMPLETE_SPLIT;

		PageSetLSN(page, lsn);
		MarkBufferDirty(buf);
	}
	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
}

static void
btree_xlog_insert(bool isleaf, bool ismeta, XLogReaderState *record)
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

		if (PageAddItem(page, (Item) datapos, datalen, xlrec->offnum,
						false, false) == InvalidOffsetNumber)
			elog(PANIC, "btree_insert_redo: failed to add item");

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
btree_xlog_split(bool onleft, bool isroot, XLogReaderState *record)
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
	Item		left_hikey = NULL;
	Size		left_hikeysz = 0;
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

	/*
	 * On leaf level, the high key of the left page is equal to the first key
	 * on the right page.
	 */
	if (isleaf)
	{
		ItemId		hiItemId = PageGetItemId(rpage, P_FIRSTDATAKEY(ropaque));

		left_hikey = PageGetItem(rpage, hiItemId);
		left_hikeysz = ItemIdGetLength(hiItemId);
	}

	PageSetLSN(rpage, lsn);
	MarkBufferDirty(rbuf);

	/* don't release the buffer yet; we touch right page's first item below */

	/* Now reconstruct left (original) sibling page */
	if (XLogReadBufferForRedo(record, 0, &lbuf) == BLK_NEEDS_REDO)
	{
		/*
		 * To retain the same physical order of the tuples that they had, we
		 * initialize a temporary empty page for the left page and add all the
		 * items to that in item number order.  This mirrors how _bt_split()
		 * works.  It's not strictly required to retain the same physical
		 * order, as long as the items are in the correct item number order,
		 * but it helps debugging.  See also _bt_restore_page(), which does
		 * the same for the right page.
		 */
		Page		lpage = (Page) BufferGetPage(lbuf);
		BTPageOpaque lopaque = (BTPageOpaque) PageGetSpecialPointer(lpage);
		OffsetNumber off;
		Item		newitem = NULL;
		Size		newitemsz = 0;
		Page		newlpage;
		OffsetNumber leftoff;

		datapos = XLogRecGetBlockData(record, 0, &datalen);

		if (onleft)
		{
			newitem = (Item) datapos;
			newitemsz = MAXALIGN(IndexTupleSize(newitem));
			datapos += newitemsz;
			datalen -= newitemsz;
		}

		/* Extract left hikey and its size (assuming 16-bit alignment) */
		if (!isleaf)
		{
			left_hikey = (Item) datapos;
			left_hikeysz = MAXALIGN(IndexTupleSize(left_hikey));
			datapos += left_hikeysz;
			datalen -= left_hikeysz;
		}
		Assert(datalen == 0);

		newlpage = PageGetTempPageCopySpecial(lpage);

		/* Set high key */
		leftoff = P_HIKEY;
		if (PageAddItem(newlpage, left_hikey, left_hikeysz,
						P_HIKEY, false, false) == InvalidOffsetNumber)
			elog(PANIC, "failed to add high key to left page after split");
		leftoff = OffsetNumberNext(leftoff);

		for (off = P_FIRSTDATAKEY(lopaque); off < xlrec->firstright; off++)
		{
			ItemId		itemid;
			Size		itemsz;
			Item		item;

			/* add the new item if it was inserted on left page */
			if (onleft && off == xlrec->newitemoff)
			{
				if (PageAddItem(newlpage, newitem, newitemsz, leftoff,
								false, false) == InvalidOffsetNumber)
					elog(ERROR, "failed to add new item to left page after split");
				leftoff = OffsetNumberNext(leftoff);
			}

			itemid = PageGetItemId(lpage, off);
			itemsz = ItemIdGetLength(itemid);
			item = PageGetItem(lpage, itemid);
			if (PageAddItem(newlpage, item, itemsz, leftoff,
							false, false) == InvalidOffsetNumber)
				elog(ERROR, "failed to add old item to left page after split");
			leftoff = OffsetNumberNext(leftoff);
		}

		/* cope with possibility that newitem goes at the end */
		if (onleft && off == xlrec->newitemoff)
		{
			if (PageAddItem(newlpage, newitem, newitemsz, leftoff,
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

	/* We no longer need the buffers */
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
btree_xlog_vacuum(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	Buffer		buffer;
	Page		page;
	BTPageOpaque opaque;
#ifdef UNUSED
	xl_btree_vacuum *xlrec = (xl_btree_vacuum *) XLogRecGetData(record);

	/*
	 * This section of code is thought to be no longer needed, after analysis
	 * of the calling paths. It is retained to allow the code to be reinstated
	 * if a flaw is revealed in that thinking.
	 *
	 * If we are running non-MVCC scans using this index we need to do some
	 * additional work to ensure correctness, which is known as a "pin scan"
	 * described in more detail in next paragraphs. We used to do the extra
	 * work in all cases, whereas we now avoid that work in most cases. If
	 * lastBlockVacuumed is set to InvalidBlockNumber then we skip the
	 * additional work required for the pin scan.
	 *
	 * Avoiding this extra work is important since it requires us to touch
	 * every page in the index, so is an O(N) operation. Worse, it is an
	 * operation performed in the foreground during redo, so it delays
	 * replication directly.
	 *
	 * If queries might be active then we need to ensure every leaf page is
	 * unpinned between the lastBlockVacuumed and the current block, if there
	 * are any.  This prevents replay of the VACUUM from reaching the stage of
	 * removing heap tuples while there could still be indexscans "in flight"
	 * to those particular tuples for those scans which could be confused by
	 * finding new tuples at the old TID locations (see nbtree/README).
	 *
	 * It might be worth checking if there are actually any backends running;
	 * if not, we could just skip this.
	 *
	 * Since VACUUM can visit leaf pages out-of-order, it might issue records
	 * with lastBlockVacuumed >= block; that's not an error, it just means
	 * nothing to do now.
	 *
	 * Note: since we touch all pages in the range, we will lock non-leaf
	 * pages, and also any empty (all-zero) pages that may be in the index. It
	 * doesn't seem worth the complexity to avoid that.  But it's important
	 * that HotStandbyActiveInReplay() will not return true if the database
	 * isn't yet consistent; so we need not fear reading still-corrupt blocks
	 * here during crash recovery.
	 */
	if (HotStandbyActiveInReplay() && BlockNumberIsValid(xlrec->lastBlockVacuumed))
	{
		RelFileNode thisrnode;
		BlockNumber thisblkno;
		BlockNumber blkno;

		XLogRecGetBlockTag(record, 0, &thisrnode, NULL, &thisblkno);

		for (blkno = xlrec->lastBlockVacuumed + 1; blkno < thisblkno; blkno++)
		{
			/*
			 * We use RBM_NORMAL_NO_LOG mode because it's not an error
			 * condition to see all-zero pages.  The original btvacuumpage
			 * scan would have skipped over all-zero pages, noting them in FSM
			 * but not bothering to initialize them just yet; so we mustn't
			 * throw an error here.  (We could skip acquiring the cleanup lock
			 * if PageIsNew, but it's probably not worth the cycles to test.)
			 *
			 * XXX we don't actually need to read the block, we just need to
			 * confirm it is unpinned. If we had a special call into the
			 * buffer manager we could optimise this so that if the block is
			 * not in shared_buffers we confirm it as unpinned. Optimizing
			 * this is now moot, since in most cases we avoid the scan.
			 */
			buffer = XLogReadBufferExtended(thisrnode, MAIN_FORKNUM, blkno,
											RBM_NORMAL_NO_LOG);
			if (BufferIsValid(buffer))
			{
				LockBufferForCleanup(buffer);
				UnlockReleaseBuffer(buffer);
			}
		}
	}
#endif

	/*
	 * Like in btvacuumpage(), we need to take a cleanup lock on every leaf
	 * page. See nbtree/README for details.
	 */
	if (XLogReadBufferForRedoExtended(record, 0, RBM_NORMAL, true, &buffer)
		== BLK_NEEDS_REDO)
	{
		char	   *ptr;
		Size		len;

		ptr = XLogRecGetBlockData(record, 0, &len);

		page = (Page) BufferGetPage(buffer);

		if (len > 0)
		{
			OffsetNumber *unused;
			OffsetNumber *unend;

			unused = (OffsetNumber *) ptr;
			unend = (OffsetNumber *) ((char *) ptr + len);

			if ((unend - unused) > 0)
				PageIndexMultiDelete(page, unused, unend - unused);
		}

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

/*
 * Get the latestRemovedXid from the heap pages pointed at by the index
 * tuples being deleted. This puts the work for calculating latestRemovedXid
 * into the recovery path rather than the primary path.
 *
 * It's possible that this generates a fair amount of I/O, since an index
 * block may have hundreds of tuples being deleted. Repeat accesses to the
 * same heap blocks are common, though are not yet optimised.
 *
 * XXX optimise later with something like XLogPrefetchBuffer()
 */
static TransactionId
btree_xlog_delete_get_latestRemovedXid(XLogReaderState *record)
{
	xl_btree_delete *xlrec = (xl_btree_delete *) XLogRecGetData(record);
	OffsetNumber *unused;
	Buffer		ibuffer,
				hbuffer;
	Page		ipage,
				hpage;
	RelFileNode rnode;
	BlockNumber blkno;
	ItemId		iitemid,
				hitemid;
	IndexTuple	itup;
	HeapTupleHeader htuphdr;
	BlockNumber hblkno;
	OffsetNumber hoffnum;
	TransactionId latestRemovedXid = InvalidTransactionId;
	int			i;

	/*
	 * If there's nothing running on the standby we don't need to derive a
	 * full latestRemovedXid value, so use a fast path out of here.  This
	 * returns InvalidTransactionId, and so will conflict with all HS
	 * transactions; but since we just worked out that that's zero people,
	 * it's OK.
	 *
	 * XXX There is a race condition here, which is that a new backend might
	 * start just after we look.  If so, it cannot need to conflict, but this
	 * coding will result in throwing a conflict anyway.
	 */
	if (CountDBBackends(InvalidOid) == 0)
		return latestRemovedXid;

	/*
	 * In what follows, we have to examine the previous state of the index
	 * page, as well as the heap page(s) it points to.  This is only valid if
	 * WAL replay has reached a consistent database state; which means that
	 * the preceding check is not just an optimization, but is *necessary*. We
	 * won't have let in any user sessions before we reach consistency.
	 */
	if (!reachedConsistency)
		elog(PANIC, "btree_xlog_delete_get_latestRemovedXid: cannot operate with inconsistent data");

	/*
	 * Get index page.  If the DB is consistent, this should not fail, nor
	 * should any of the heap page fetches below.  If one does, we return
	 * InvalidTransactionId to cancel all HS transactions.  That's probably
	 * overkill, but it's safe, and certainly better than panicking here.
	 */
	XLogRecGetBlockTag(record, 0, &rnode, NULL, &blkno);
	ibuffer = XLogReadBufferExtended(rnode, MAIN_FORKNUM, blkno, RBM_NORMAL);
	if (!BufferIsValid(ibuffer))
		return InvalidTransactionId;
	LockBuffer(ibuffer, BT_READ);
	ipage = (Page) BufferGetPage(ibuffer);

	/*
	 * Loop through the deleted index items to obtain the TransactionId from
	 * the heap items they point to.
	 */
	unused = (OffsetNumber *) ((char *) xlrec + SizeOfBtreeDelete);

	for (i = 0; i < xlrec->nitems; i++)
	{
		/*
		 * Identify the index tuple about to be deleted
		 */
		iitemid = PageGetItemId(ipage, unused[i]);
		itup = (IndexTuple) PageGetItem(ipage, iitemid);

		/*
		 * Locate the heap page that the index tuple points at
		 */
		hblkno = ItemPointerGetBlockNumber(&(itup->t_tid));
		hbuffer = XLogReadBufferExtended(xlrec->hnode, MAIN_FORKNUM, hblkno, RBM_NORMAL);
		if (!BufferIsValid(hbuffer))
		{
			UnlockReleaseBuffer(ibuffer);
			return InvalidTransactionId;
		}
		LockBuffer(hbuffer, BUFFER_LOCK_SHARE);
		hpage = (Page) BufferGetPage(hbuffer);

		/*
		 * Look up the heap tuple header that the index tuple points at by
		 * using the heap node supplied with the xlrec. We can't use
		 * heap_fetch, since it uses ReadBuffer rather than XLogReadBuffer.
		 * Note that we are not looking at tuple data here, just headers.
		 */
		hoffnum = ItemPointerGetOffsetNumber(&(itup->t_tid));
		hitemid = PageGetItemId(hpage, hoffnum);

		/*
		 * Follow any redirections until we find something useful.
		 */
		while (ItemIdIsRedirected(hitemid))
		{
			hoffnum = ItemIdGetRedirect(hitemid);
			hitemid = PageGetItemId(hpage, hoffnum);
			CHECK_FOR_INTERRUPTS();
		}

		/*
		 * If the heap item has storage, then read the header and use that to
		 * set latestRemovedXid.
		 *
		 * Some LP_DEAD items may not be accessible, so we ignore them.
		 */
		if (ItemIdHasStorage(hitemid))
		{
			htuphdr = (HeapTupleHeader) PageGetItem(hpage, hitemid);

			HeapTupleHeaderAdvanceLatestRemovedXid(htuphdr, &latestRemovedXid);
		}
		else if (ItemIdIsDead(hitemid))
		{
			/*
			 * Conjecture: if hitemid is dead then it had xids before the xids
			 * marked on LP_NORMAL items. So we just ignore this item and move
			 * onto the next, for the purposes of calculating
			 * latestRemovedxids.
			 */
		}
		else
			Assert(!ItemIdIsUsed(hitemid));

		UnlockReleaseBuffer(hbuffer);
	}

	UnlockReleaseBuffer(ibuffer);

	/*
	 * If all heap tuples were LP_DEAD then we will be returning
	 * InvalidTransactionId here, which avoids conflicts. This matches
	 * existing logic which assumes that LP_DEAD tuples must already be older
	 * than the latestRemovedXid on the cleanup record that set them as
	 * LP_DEAD, hence must already have generated a conflict.
	 */
	return latestRemovedXid;
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
	 * update the page.
	 *
	 * Btree delete records can conflict with standby queries.  You might
	 * think that vacuum records would conflict as well, but we've handled
	 * that already.  XLOG_HEAP2_CLEANUP_INFO records provide the highest xid
	 * cleaned by the vacuum of the heap and so we can resolve any conflicts
	 * just once when that arrives.  After that we know that no conflicts
	 * exist from individual btree vacuum records on that index.
	 */
	if (InHotStandby)
	{
		TransactionId latestRemovedXid = btree_xlog_delete_get_latestRemovedXid(record);
		RelFileNode rnode;

		XLogRecGetBlockTag(record, 0, &rnode, NULL, NULL);

		ResolveRecoveryConflictWithSnapshot(latestRemovedXid, rnode);
	}

	/*
	 * We don't need to take a cleanup lock to apply these changes. See
	 * nbtree/README for details.
	 */
	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		page = (Page) BufferGetPage(buffer);

		if (XLogRecGetDataLen(record) > SizeOfBtreeDelete)
		{
			OffsetNumber *unused;

			unused = (OffsetNumber *) ((char *) xlrec + SizeOfBtreeDelete);

			PageIndexMultiDelete(page, unused, xlrec->nitems);
		}

		/*
		 * Mark the page as not containing any LP_DEAD items --- see comments
		 * in _bt_delitems_delete().
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

	/* parent page */
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
		rightsib = ItemPointerGetBlockNumber(&itup->t_tid);

		itemid = PageGetItemId(page, poffset);
		itup = (IndexTuple) PageGetItem(page, itemid);
		ItemPointerSet(&(itup->t_tid), rightsib, P_HIKEY);
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
	 * Construct a dummy hikey item that points to the next parent to be
	 * deleted (if any).
	 */
	MemSet(&trunctuple, 0, sizeof(IndexTupleData));
	trunctuple.t_info = sizeof(IndexTupleData);
	if (xlrec->topparent != InvalidBlockNumber)
		ItemPointerSet(&trunctuple.t_tid, xlrec->topparent, P_HIKEY);
	else
		ItemPointerSetInvalid(&trunctuple.t_tid);
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
	 * branch.
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
		if (xlrec->topparent != InvalidBlockNumber)
			ItemPointerSet(&trunctuple.t_tid, xlrec->topparent, P_HIKEY);
		else
			ItemPointerSetInvalid(&trunctuple.t_tid);
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

	switch (info)
	{
		case XLOG_BTREE_INSERT_LEAF:
			btree_xlog_insert(true, false, record);
			break;
		case XLOG_BTREE_INSERT_UPPER:
			btree_xlog_insert(false, false, record);
			break;
		case XLOG_BTREE_INSERT_META:
			btree_xlog_insert(false, true, record);
			break;
		case XLOG_BTREE_SPLIT_L:
			btree_xlog_split(true, false, record);
			break;
		case XLOG_BTREE_SPLIT_R:
			btree_xlog_split(false, false, record);
			break;
		case XLOG_BTREE_SPLIT_L_ROOT:
			btree_xlog_split(true, true, record);
			break;
		case XLOG_BTREE_SPLIT_R_ROOT:
			btree_xlog_split(false, true, record);
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
		default:
			elog(PANIC, "btree_redo: unknown op code %u", info);
	}
}
