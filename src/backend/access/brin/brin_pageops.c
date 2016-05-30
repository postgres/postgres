/*
 * brin_pageops.c
 *		Page-handling routines for BRIN indexes
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/brin/brin_pageops.c
 */
#include "postgres.h"

#include "access/brin_pageops.h"
#include "access/brin_page.h"
#include "access/brin_revmap.h"
#include "access/brin_xlog.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "utils/rel.h"


/*
 * Maximum size of an entry in a BRIN_PAGETYPE_REGULAR page.  We can tolerate
 * a single item per page, unlike other index AMs.
 */
#define BrinMaxItemSize \
	MAXALIGN_DOWN(BLCKSZ - \
				  (MAXALIGN(SizeOfPageHeaderData + \
							sizeof(ItemIdData)) + \
				   MAXALIGN(sizeof(BrinSpecialSpace))))

static Buffer brin_getinsertbuffer(Relation irel, Buffer oldbuf, Size itemsz,
					 bool *extended);
static Size br_page_get_freespace(Page page);
static void brin_initialize_empty_new_buffer(Relation idxrel, Buffer buffer);


/*
 * Update tuple origtup (size origsz), located in offset oldoff of buffer
 * oldbuf, to newtup (size newsz) as summary tuple for the page range starting
 * at heapBlk.  oldbuf must not be locked on entry, and is not locked at exit.
 *
 * If samepage is true, attempt to put the new tuple in the same page, but if
 * there's no room, use some other one.
 *
 * If the update is successful, return true; the revmap is updated to point to
 * the new tuple.  If the update is not done for whatever reason, return false.
 * Caller may retry the update if this happens.
 */
bool
brin_doupdate(Relation idxrel, BlockNumber pagesPerRange,
			  BrinRevmap *revmap, BlockNumber heapBlk,
			  Buffer oldbuf, OffsetNumber oldoff,
			  const BrinTuple *origtup, Size origsz,
			  const BrinTuple *newtup, Size newsz,
			  bool samepage)
{
	Page		oldpage;
	ItemId		oldlp;
	BrinTuple  *oldtup;
	Size		oldsz;
	Buffer		newbuf;
	bool		extended;

	Assert(newsz == MAXALIGN(newsz));

	/* If the item is oversized, don't bother. */
	if (newsz > BrinMaxItemSize)
	{
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			errmsg("index row size %lu exceeds maximum %lu for index \"%s\"",
				   (unsigned long) newsz,
				   (unsigned long) BrinMaxItemSize,
				   RelationGetRelationName(idxrel))));
		return false;			/* keep compiler quiet */
	}

	/* make sure the revmap is long enough to contain the entry we need */
	brinRevmapExtend(revmap, heapBlk);

	if (!samepage)
	{
		/* need a page on which to put the item */
		newbuf = brin_getinsertbuffer(idxrel, oldbuf, newsz, &extended);
		if (!BufferIsValid(newbuf))
		{
			Assert(!extended);
			return false;
		}

		/*
		 * Note: it's possible (though unlikely) that the returned newbuf is
		 * the same as oldbuf, if brin_getinsertbuffer determined that the old
		 * buffer does in fact have enough space.
		 */
		if (newbuf == oldbuf)
		{
			Assert(!extended);
			newbuf = InvalidBuffer;
		}
	}
	else
	{
		LockBuffer(oldbuf, BUFFER_LOCK_EXCLUSIVE);
		newbuf = InvalidBuffer;
		extended = false;
	}
	oldpage = BufferGetPage(oldbuf);
	oldlp = PageGetItemId(oldpage, oldoff);

	/*
	 * Check that the old tuple wasn't updated concurrently: it might have
	 * moved someplace else entirely ...
	 */
	if (!ItemIdIsNormal(oldlp))
	{
		LockBuffer(oldbuf, BUFFER_LOCK_UNLOCK);

		/*
		 * If this happens, and the new buffer was obtained by extending the
		 * relation, then we need to ensure we don't leave it uninitialized or
		 * forget about it.
		 */
		if (BufferIsValid(newbuf))
		{
			if (extended)
				brin_initialize_empty_new_buffer(idxrel, newbuf);
			UnlockReleaseBuffer(newbuf);
			if (extended)
				FreeSpaceMapVacuum(idxrel);
		}
		return false;
	}

	oldsz = ItemIdGetLength(oldlp);
	oldtup = (BrinTuple *) PageGetItem(oldpage, oldlp);

	/*
	 * ... or it might have been updated in place to different contents.
	 */
	if (!brin_tuples_equal(oldtup, oldsz, origtup, origsz))
	{
		LockBuffer(oldbuf, BUFFER_LOCK_UNLOCK);
		if (BufferIsValid(newbuf))
		{
			if (extended)
				brin_initialize_empty_new_buffer(idxrel, newbuf);
			UnlockReleaseBuffer(newbuf);
			if (extended)
				FreeSpaceMapVacuum(idxrel);
		}
		return false;
	}

	/*
	 * Great, the old tuple is intact.  We can proceed with the update.
	 *
	 * If there's enough room in the old page for the new tuple, replace it.
	 *
	 * Note that there might now be enough space on the page even though the
	 * caller told us there isn't, if a concurrent update moved another tuple
	 * elsewhere or replaced a tuple with a smaller one.
	 */
	if (((BrinPageFlags(oldpage) & BRIN_EVACUATE_PAGE) == 0) &&
		brin_can_do_samepage_update(oldbuf, origsz, newsz))
	{
		if (BufferIsValid(newbuf))
		{
			/* as above */
			if (extended)
				brin_initialize_empty_new_buffer(idxrel, newbuf);
			UnlockReleaseBuffer(newbuf);
		}

		START_CRIT_SECTION();
		PageIndexDeleteNoCompact(oldpage, &oldoff, 1);
		if (PageAddItemExtended(oldpage, (Item) newtup, newsz, oldoff,
				PAI_OVERWRITE | PAI_ALLOW_FAR_OFFSET) == InvalidOffsetNumber)
			elog(ERROR, "failed to add BRIN tuple");
		MarkBufferDirty(oldbuf);

		/* XLOG stuff */
		if (RelationNeedsWAL(idxrel))
		{
			xl_brin_samepage_update xlrec;
			XLogRecPtr	recptr;
			uint8		info = XLOG_BRIN_SAMEPAGE_UPDATE;

			xlrec.offnum = oldoff;

			XLogBeginInsert();
			XLogRegisterData((char *) &xlrec, SizeOfBrinSamepageUpdate);

			XLogRegisterBuffer(0, oldbuf, REGBUF_STANDARD);
			XLogRegisterBufData(0, (char *) newtup, newsz);

			recptr = XLogInsert(RM_BRIN_ID, info);

			PageSetLSN(oldpage, recptr);
		}

		END_CRIT_SECTION();

		LockBuffer(oldbuf, BUFFER_LOCK_UNLOCK);

		if (extended)
			FreeSpaceMapVacuum(idxrel);

		return true;
	}
	else if (newbuf == InvalidBuffer)
	{
		/*
		 * Not enough space, but caller said that there was. Tell them to
		 * start over.
		 */
		LockBuffer(oldbuf, BUFFER_LOCK_UNLOCK);
		return false;
	}
	else
	{
		/*
		 * Not enough free space on the oldpage. Put the new tuple on the new
		 * page, and update the revmap.
		 */
		Page		newpage = BufferGetPage(newbuf);
		Buffer		revmapbuf;
		ItemPointerData newtid;
		OffsetNumber newoff;
		BlockNumber newblk = InvalidBlockNumber;
		Size		freespace = 0;

		revmapbuf = brinLockRevmapPageForUpdate(revmap, heapBlk);

		START_CRIT_SECTION();

		/*
		 * We need to initialize the page if it's newly obtained.  Note we
		 * will WAL-log the initialization as part of the update, so we don't
		 * need to do that here.
		 */
		if (extended)
			brin_page_init(BufferGetPage(newbuf), BRIN_PAGETYPE_REGULAR);

		PageIndexDeleteNoCompact(oldpage, &oldoff, 1);
		newoff = PageAddItem(newpage, (Item) newtup, newsz,
							 InvalidOffsetNumber, false, false);
		if (newoff == InvalidOffsetNumber)
			elog(ERROR, "failed to add BRIN tuple to new page");
		MarkBufferDirty(oldbuf);
		MarkBufferDirty(newbuf);

		/* needed to update FSM below */
		if (extended)
		{
			newblk = BufferGetBlockNumber(newbuf);
			freespace = br_page_get_freespace(newpage);
		}

		ItemPointerSet(&newtid, BufferGetBlockNumber(newbuf), newoff);
		brinSetHeapBlockItemptr(revmapbuf, pagesPerRange, heapBlk, newtid);
		MarkBufferDirty(revmapbuf);

		/* XLOG stuff */
		if (RelationNeedsWAL(idxrel))
		{
			xl_brin_update xlrec;
			XLogRecPtr	recptr;
			uint8		info;

			info = XLOG_BRIN_UPDATE | (extended ? XLOG_BRIN_INIT_PAGE : 0);

			xlrec.insert.offnum = newoff;
			xlrec.insert.heapBlk = heapBlk;
			xlrec.insert.pagesPerRange = pagesPerRange;
			xlrec.oldOffnum = oldoff;

			XLogBeginInsert();

			/* new page */
			XLogRegisterData((char *) &xlrec, SizeOfBrinUpdate);

			XLogRegisterBuffer(0, newbuf, REGBUF_STANDARD | (extended ? REGBUF_WILL_INIT : 0));
			XLogRegisterBufData(0, (char *) newtup, newsz);

			/* revmap page */
			XLogRegisterBuffer(1, revmapbuf, REGBUF_STANDARD);

			/* old page */
			XLogRegisterBuffer(2, oldbuf, REGBUF_STANDARD);

			recptr = XLogInsert(RM_BRIN_ID, info);

			PageSetLSN(oldpage, recptr);
			PageSetLSN(newpage, recptr);
			PageSetLSN(BufferGetPage(revmapbuf), recptr);
		}

		END_CRIT_SECTION();

		LockBuffer(revmapbuf, BUFFER_LOCK_UNLOCK);
		LockBuffer(oldbuf, BUFFER_LOCK_UNLOCK);
		UnlockReleaseBuffer(newbuf);

		if (extended)
		{
			Assert(BlockNumberIsValid(newblk));
			RecordPageWithFreeSpace(idxrel, newblk, freespace);
			FreeSpaceMapVacuum(idxrel);
		}

		return true;
	}
}

/*
 * Return whether brin_doupdate can do a samepage update.
 */
bool
brin_can_do_samepage_update(Buffer buffer, Size origsz, Size newsz)
{
	return
		((newsz <= origsz) ||
		 PageGetExactFreeSpace(BufferGetPage(buffer)) >= (newsz - origsz));
}

/*
 * Insert an index tuple into the index relation.  The revmap is updated to
 * mark the range containing the given page as pointing to the inserted entry.
 * A WAL record is written.
 *
 * The buffer, if valid, is first checked for free space to insert the new
 * entry; if there isn't enough, a new buffer is obtained and pinned.  No
 * buffer lock must be held on entry, no buffer lock is held on exit.
 *
 * Return value is the offset number where the tuple was inserted.
 */
OffsetNumber
brin_doinsert(Relation idxrel, BlockNumber pagesPerRange,
			  BrinRevmap *revmap, Buffer *buffer, BlockNumber heapBlk,
			  BrinTuple *tup, Size itemsz)
{
	Page		page;
	BlockNumber blk;
	OffsetNumber off;
	Buffer		revmapbuf;
	ItemPointerData tid;
	bool		extended;

	Assert(itemsz == MAXALIGN(itemsz));

	/* If the item is oversized, don't even bother. */
	if (itemsz > BrinMaxItemSize)
	{
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			errmsg("index row size %lu exceeds maximum %lu for index \"%s\"",
				   (unsigned long) itemsz,
				   (unsigned long) BrinMaxItemSize,
				   RelationGetRelationName(idxrel))));
		return InvalidOffsetNumber;		/* keep compiler quiet */
	}

	/* Make sure the revmap is long enough to contain the entry we need */
	brinRevmapExtend(revmap, heapBlk);

	/*
	 * Acquire lock on buffer supplied by caller, if any.  If it doesn't have
	 * enough space, unpin it to obtain a new one below.
	 */
	if (BufferIsValid(*buffer))
	{
		/*
		 * It's possible that another backend (or ourselves!) extended the
		 * revmap over the page we held a pin on, so we cannot assume that
		 * it's still a regular page.
		 */
		LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
		if (br_page_get_freespace(BufferGetPage(*buffer)) < itemsz)
		{
			UnlockReleaseBuffer(*buffer);
			*buffer = InvalidBuffer;
		}
	}

	/*
	 * If we still don't have a usable buffer, have brin_getinsertbuffer
	 * obtain one for us.
	 */
	if (!BufferIsValid(*buffer))
	{
		do
			*buffer = brin_getinsertbuffer(idxrel, InvalidBuffer, itemsz, &extended);
		while (!BufferIsValid(*buffer));
	}
	else
		extended = false;

	/* Now obtain lock on revmap buffer */
	revmapbuf = brinLockRevmapPageForUpdate(revmap, heapBlk);

	page = BufferGetPage(*buffer);
	blk = BufferGetBlockNumber(*buffer);

	/* Execute the actual insertion */
	START_CRIT_SECTION();
	if (extended)
		brin_page_init(BufferGetPage(*buffer), BRIN_PAGETYPE_REGULAR);
	off = PageAddItem(page, (Item) tup, itemsz, InvalidOffsetNumber,
					  false, false);
	if (off == InvalidOffsetNumber)
		elog(ERROR, "could not insert new index tuple to page");
	MarkBufferDirty(*buffer);

	BRIN_elog((DEBUG2, "inserted tuple (%u,%u) for range starting at %u",
			   blk, off, heapBlk));

	ItemPointerSet(&tid, blk, off);
	brinSetHeapBlockItemptr(revmapbuf, pagesPerRange, heapBlk, tid);
	MarkBufferDirty(revmapbuf);

	/* XLOG stuff */
	if (RelationNeedsWAL(idxrel))
	{
		xl_brin_insert xlrec;
		XLogRecPtr	recptr;
		uint8		info;

		info = XLOG_BRIN_INSERT | (extended ? XLOG_BRIN_INIT_PAGE : 0);
		xlrec.heapBlk = heapBlk;
		xlrec.pagesPerRange = pagesPerRange;
		xlrec.offnum = off;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfBrinInsert);

		XLogRegisterBuffer(0, *buffer, REGBUF_STANDARD | (extended ? REGBUF_WILL_INIT : 0));
		XLogRegisterBufData(0, (char *) tup, itemsz);

		XLogRegisterBuffer(1, revmapbuf, 0);

		recptr = XLogInsert(RM_BRIN_ID, info);

		PageSetLSN(page, recptr);
		PageSetLSN(BufferGetPage(revmapbuf), recptr);
	}

	END_CRIT_SECTION();

	/* Tuple is firmly on buffer; we can release our locks */
	LockBuffer(*buffer, BUFFER_LOCK_UNLOCK);
	LockBuffer(revmapbuf, BUFFER_LOCK_UNLOCK);

	if (extended)
		FreeSpaceMapVacuum(idxrel);

	return off;
}

/*
 * Initialize a page with the given type.
 *
 * Caller is responsible for marking it dirty, as appropriate.
 */
void
brin_page_init(Page page, uint16 type)
{
	PageInit(page, BLCKSZ, sizeof(BrinSpecialSpace));

	BrinPageType(page) = type;
}

/*
 * Initialize a new BRIN index' metapage.
 */
void
brin_metapage_init(Page page, BlockNumber pagesPerRange, uint16 version)
{
	BrinMetaPageData *metadata;

	brin_page_init(page, BRIN_PAGETYPE_META);

	metadata = (BrinMetaPageData *) PageGetContents(page);

	metadata->brinMagic = BRIN_META_MAGIC;
	metadata->brinVersion = version;
	metadata->pagesPerRange = pagesPerRange;

	/*
	 * Note we cheat here a little.  0 is not a valid revmap block number
	 * (because it's the metapage buffer), but doing this enables the first
	 * revmap page to be created when the index is.
	 */
	metadata->lastRevmapPage = 0;
}

/*
 * Initiate page evacuation protocol.
 *
 * The page must be locked in exclusive mode by the caller.
 *
 * If the page is not yet initialized or empty, return false without doing
 * anything; it can be used for revmap without any further changes.  If it
 * contains tuples, mark it for evacuation and return true.
 */
bool
brin_start_evacuating_page(Relation idxRel, Buffer buf)
{
	OffsetNumber off;
	OffsetNumber maxoff;
	Page		page;

	page = BufferGetPage(buf);

	if (PageIsNew(page))
		return false;

	maxoff = PageGetMaxOffsetNumber(page);
	for (off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		lp;

		lp = PageGetItemId(page, off);
		if (ItemIdIsUsed(lp))
		{
			/* prevent other backends from adding more stuff to this page */
			BrinPageFlags(page) |= BRIN_EVACUATE_PAGE;
			MarkBufferDirtyHint(buf, true);

			return true;
		}
	}
	return false;
}

/*
 * Move all tuples out of a page.
 *
 * The caller must hold lock on the page. The lock and pin are released.
 */
void
brin_evacuate_page(Relation idxRel, BlockNumber pagesPerRange,
				   BrinRevmap *revmap, Buffer buf)
{
	OffsetNumber off;
	OffsetNumber maxoff;
	Page		page;

	page = BufferGetPage(buf);

	Assert(BrinPageFlags(page) & BRIN_EVACUATE_PAGE);

	maxoff = PageGetMaxOffsetNumber(page);
	for (off = FirstOffsetNumber; off <= maxoff; off++)
	{
		BrinTuple  *tup;
		Size		sz;
		ItemId		lp;

		CHECK_FOR_INTERRUPTS();

		lp = PageGetItemId(page, off);
		if (ItemIdIsUsed(lp))
		{
			sz = ItemIdGetLength(lp);
			tup = (BrinTuple *) PageGetItem(page, lp);
			tup = brin_copy_tuple(tup, sz);

			LockBuffer(buf, BUFFER_LOCK_UNLOCK);

			if (!brin_doupdate(idxRel, pagesPerRange, revmap, tup->bt_blkno,
							   buf, off, tup, sz, tup, sz, false))
				off--;			/* retry */

			LockBuffer(buf, BUFFER_LOCK_SHARE);

			/* It's possible that someone extended the revmap over this page */
			if (!BRIN_IS_REGULAR_PAGE(page))
				break;
		}
	}

	UnlockReleaseBuffer(buf);
}

/*
 * Given a BRIN index page, initialize it if necessary, and record it into the
 * FSM if necessary.  Return value is true if the FSM itself needs "vacuuming".
 * The main use for this is when, during vacuuming, an uninitialized page is
 * found, which could be the result of relation extension followed by a crash
 * before the page can be used.
 */
bool
brin_page_cleanup(Relation idxrel, Buffer buf)
{
	Page		page = BufferGetPage(buf);
	Size		freespace;

	/*
	 * If a page was left uninitialized, initialize it now; also record it in
	 * FSM.
	 *
	 * Somebody else might be extending the relation concurrently.  To avoid
	 * re-initializing the page before they can grab the buffer lock, we
	 * acquire the extension lock momentarily.  Since they hold the extension
	 * lock from before getting the page and after its been initialized, we're
	 * sure to see their initialization.
	 */
	if (PageIsNew(page))
	{
		LockRelationForExtension(idxrel, ShareLock);
		UnlockRelationForExtension(idxrel, ShareLock);

		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		if (PageIsNew(page))
		{
			brin_initialize_empty_new_buffer(idxrel, buf);
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			return true;
		}
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	}

	/* Nothing to be done for non-regular index pages */
	if (BRIN_IS_META_PAGE(BufferGetPage(buf)) ||
		BRIN_IS_REVMAP_PAGE(BufferGetPage(buf)))
		return false;

	/* Measure free space and record it */
	freespace = br_page_get_freespace(page);
	if (freespace > GetRecordedFreeSpace(idxrel, BufferGetBlockNumber(buf)))
	{
		RecordPageWithFreeSpace(idxrel, BufferGetBlockNumber(buf), freespace);
		return true;
	}

	return false;
}

/*
 * Return a pinned and exclusively locked buffer which can be used to insert an
 * index item of size itemsz (caller must ensure not to request sizes
 * impossible to fulfill).  If oldbuf is a valid buffer, it is also locked (in
 * an order determined to avoid deadlocks.)
 *
 * If we find that the old page is no longer a regular index page (because
 * of a revmap extension), the old buffer is unlocked and we return
 * InvalidBuffer.
 *
 * If there's no existing page with enough free space to accommodate the new
 * item, the relation is extended.  If this happens, *extended is set to true,
 * and it is the caller's responsibility to initialize the page (and WAL-log
 * that fact) prior to use.
 *
 * Note that in some corner cases it is possible for this routine to extend the
 * relation and then not return the buffer.  It is this routine's
 * responsibility to WAL-log the page initialization and to record the page in
 * FSM if that happens.  Such a buffer may later be reused by this routine.
 */
static Buffer
brin_getinsertbuffer(Relation irel, Buffer oldbuf, Size itemsz,
					 bool *extended)
{
	BlockNumber oldblk;
	BlockNumber newblk;
	Page		page;
	int			freespace;

	/* callers must have checked */
	Assert(itemsz <= BrinMaxItemSize);

	*extended = false;

	if (BufferIsValid(oldbuf))
		oldblk = BufferGetBlockNumber(oldbuf);
	else
		oldblk = InvalidBlockNumber;

	/*
	 * Loop until we find a page with sufficient free space.  By the time we
	 * return to caller out of this loop, both buffers are valid and locked;
	 * if we have to restart here, neither buffer is locked and buf is not a
	 * pinned buffer.
	 */
	newblk = RelationGetTargetBlock(irel);
	if (newblk == InvalidBlockNumber)
		newblk = GetPageWithFreeSpace(irel, itemsz);
	for (;;)
	{
		Buffer		buf;
		bool		extensionLockHeld = false;

		CHECK_FOR_INTERRUPTS();

		if (newblk == InvalidBlockNumber)
		{
			/*
			 * There's not enough free space in any existing index page,
			 * according to the FSM: extend the relation to obtain a shiny new
			 * page.
			 */
			if (!RELATION_IS_LOCAL(irel))
			{
				LockRelationForExtension(irel, ExclusiveLock);
				extensionLockHeld = true;
			}
			buf = ReadBuffer(irel, P_NEW);
			newblk = BufferGetBlockNumber(buf);
			*extended = true;

			BRIN_elog((DEBUG2, "brin_getinsertbuffer: extending to page %u",
					   BufferGetBlockNumber(buf)));
		}
		else if (newblk == oldblk)
		{
			/*
			 * There's an odd corner-case here where the FSM is out-of-date,
			 * and gave us the old page.
			 */
			buf = oldbuf;
		}
		else
		{
			buf = ReadBuffer(irel, newblk);
		}

		/*
		 * We lock the old buffer first, if it's earlier than the new one; but
		 * before we do, we need to check that it hasn't been turned into a
		 * revmap page concurrently; if we detect that it happened, give up
		 * and tell caller to start over.
		 */
		if (BufferIsValid(oldbuf) && oldblk < newblk)
		{
			LockBuffer(oldbuf, BUFFER_LOCK_EXCLUSIVE);
			if (!BRIN_IS_REGULAR_PAGE(BufferGetPage(oldbuf)))
			{
				LockBuffer(oldbuf, BUFFER_LOCK_UNLOCK);

				/*
				 * It is possible that the new page was obtained from
				 * extending the relation.  In that case, we must be sure to
				 * record it in the FSM before leaving, because otherwise the
				 * space would be lost forever.  However, we cannot let an
				 * uninitialized page get in the FSM, so we need to initialize
				 * it first.
				 */
				if (*extended)
				{
					brin_initialize_empty_new_buffer(irel, buf);
					/* shouldn't matter, but don't confuse caller */
					*extended = false;
				}

				if (extensionLockHeld)
					UnlockRelationForExtension(irel, ExclusiveLock);

				ReleaseBuffer(buf);
				return InvalidBuffer;
			}
		}

		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

		if (extensionLockHeld)
			UnlockRelationForExtension(irel, ExclusiveLock);

		page = BufferGetPage(buf);

		/*
		 * We have a new buffer to insert into.  Check that the new page has
		 * enough free space, and return it if it does; otherwise start over.
		 * Note that we allow for the FSM to be out of date here, and in that
		 * case we update it and move on.
		 *
		 * (br_page_get_freespace also checks that the FSM didn't hand us a
		 * page that has since been repurposed for the revmap.)
		 */
		freespace = *extended ?
			BrinMaxItemSize : br_page_get_freespace(page);
		if (freespace >= itemsz)
		{
			RelationSetTargetBlock(irel, BufferGetBlockNumber(buf));

			/*
			 * Since the target block specification can get lost on cache
			 * invalidations, make sure we update the more permanent FSM with
			 * data about it before going away.
			 */
			if (*extended)
				RecordPageWithFreeSpace(irel, BufferGetBlockNumber(buf),
										freespace);

			/*
			 * Lock the old buffer if not locked already.  Note that in this
			 * case we know for sure it's a regular page: it's later than the
			 * new page we just got, which is not a revmap page, and revmap
			 * pages are always consecutive.
			 */
			if (BufferIsValid(oldbuf) && oldblk > newblk)
			{
				LockBuffer(oldbuf, BUFFER_LOCK_EXCLUSIVE);
				Assert(BRIN_IS_REGULAR_PAGE(BufferGetPage(oldbuf)));
			}

			return buf;
		}

		/* This page is no good. */

		/*
		 * If an entirely new page does not contain enough free space for the
		 * new item, then surely that item is oversized.  Complain loudly; but
		 * first make sure we initialize the page and record it as free, for
		 * next time.
		 */
		if (*extended)
		{
			brin_initialize_empty_new_buffer(irel, buf);

			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			errmsg("index row size %lu exceeds maximum %lu for index \"%s\"",
				   (unsigned long) itemsz,
				   (unsigned long) freespace,
				   RelationGetRelationName(irel))));
			return InvalidBuffer;		/* keep compiler quiet */
		}

		if (newblk != oldblk)
			UnlockReleaseBuffer(buf);
		if (BufferIsValid(oldbuf) && oldblk <= newblk)
			LockBuffer(oldbuf, BUFFER_LOCK_UNLOCK);

		newblk = RecordAndGetPageWithFreeSpace(irel, newblk, freespace, itemsz);
	}
}

/*
 * Initialize a page as an empty regular BRIN page, WAL-log this, and record
 * the page in FSM.
 *
 * There are several corner situations in which we extend the relation to
 * obtain a new page and later find that we cannot use it immediately.  When
 * that happens, we don't want to leave the page go unrecorded in FSM, because
 * there is no mechanism to get the space back and the index would bloat.
 * Also, because we would not WAL-log the action that would initialize the
 * page, the page would go uninitialized in a standby (or after recovery).
 */
static void
brin_initialize_empty_new_buffer(Relation idxrel, Buffer buffer)
{
	Page		page;

	BRIN_elog((DEBUG2,
			   "brin_initialize_empty_new_buffer: initializing blank page %u",
			   BufferGetBlockNumber(buffer)));

	START_CRIT_SECTION();
	page = BufferGetPage(buffer);
	brin_page_init(page, BRIN_PAGETYPE_REGULAR);
	MarkBufferDirty(buffer);
	log_newpage_buffer(buffer, true);
	END_CRIT_SECTION();

	/*
	 * We update the FSM for this page, but this is not WAL-logged.  This is
	 * acceptable because VACUUM will scan the index and update the FSM with
	 * pages whose FSM records were forgotten in a crash.
	 */
	RecordPageWithFreeSpace(idxrel, BufferGetBlockNumber(buffer),
							br_page_get_freespace(page));
}


/*
 * Return the amount of free space on a regular BRIN index page.
 *
 * If the page is not a regular page, or has been marked with the
 * BRIN_EVACUATE_PAGE flag, returns 0.
 */
static Size
br_page_get_freespace(Page page)
{
	if (!BRIN_IS_REGULAR_PAGE(page) ||
		(BrinPageFlags(page) & BRIN_EVACUATE_PAGE) != 0)
		return 0;
	else
		return PageGetFreeSpace(page);
}
