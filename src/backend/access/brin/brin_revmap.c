/*
 * brin_revmap.c
 *		Range map for BRIN indexes
 *
 * The range map (revmap) is a translation structure for BRIN indexes: for each
 * page range there is one summary tuple, and its location is tracked by the
 * revmap.  Whenever a new tuple is inserted into a table that violates the
 * previously recorded summary values, a new tuple is inserted into the index
 * and the revmap is updated to point to it.
 *
 * The revmap is stored in the first pages of the index, immediately following
 * the metapage.  When the revmap needs to be expanded, all tuples on the
 * regular BRIN page at that block (if any) are moved out of the way.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/brin/brin_revmap.c
 */
#include "postgres.h"

#include "access/brin_page.h"
#include "access/brin_pageops.h"
#include "access/brin_revmap.h"
#include "access/brin_tuple.h"
#include "access/brin_xlog.h"
#include "access/rmgr.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/rel.h"


/*
 * In revmap pages, each item stores an ItemPointerData.  These defines let one
 * find the logical revmap page number and index number of the revmap item for
 * the given heap block number.
 */
#define HEAPBLK_TO_REVMAP_BLK(pagesPerRange, heapBlk) \
	((heapBlk / pagesPerRange) / REVMAP_PAGE_MAXITEMS)
#define HEAPBLK_TO_REVMAP_INDEX(pagesPerRange, heapBlk) \
	((heapBlk / pagesPerRange) % REVMAP_PAGE_MAXITEMS)


struct BrinRevmap
{
	Relation	rm_irel;
	BlockNumber rm_pagesPerRange;
	BlockNumber rm_lastRevmapPage;		/* cached from the metapage */
	Buffer		rm_metaBuf;
	Buffer		rm_currBuf;
};

/* typedef appears in brin_revmap.h */


static BlockNumber revmap_get_blkno(BrinRevmap *revmap,
				 BlockNumber heapBlk);
static Buffer revmap_get_buffer(BrinRevmap *revmap, BlockNumber heapBlk);
static BlockNumber revmap_extend_and_get_blkno(BrinRevmap *revmap,
							BlockNumber heapBlk);
static void revmap_physical_extend(BrinRevmap *revmap);

/*
 * Initialize an access object for a range map.  This must be freed by
 * brinRevmapTerminate when caller is done with it.
 */
BrinRevmap *
brinRevmapInitialize(Relation idxrel, BlockNumber *pagesPerRange,
					 Snapshot snapshot)
{
	BrinRevmap *revmap;
	Buffer		meta;
	BrinMetaPageData *metadata;
	Page		page;

	meta = ReadBuffer(idxrel, BRIN_METAPAGE_BLKNO);
	LockBuffer(meta, BUFFER_LOCK_SHARE);
	page = BufferGetPage(meta);
	TestForOldSnapshot(snapshot, idxrel, page);
	metadata = (BrinMetaPageData *) PageGetContents(page);

	revmap = palloc(sizeof(BrinRevmap));
	revmap->rm_irel = idxrel;
	revmap->rm_pagesPerRange = metadata->pagesPerRange;
	revmap->rm_lastRevmapPage = metadata->lastRevmapPage;
	revmap->rm_metaBuf = meta;
	revmap->rm_currBuf = InvalidBuffer;

	*pagesPerRange = metadata->pagesPerRange;

	LockBuffer(meta, BUFFER_LOCK_UNLOCK);

	return revmap;
}

/*
 * Release resources associated with a revmap access object.
 */
void
brinRevmapTerminate(BrinRevmap *revmap)
{
	ReleaseBuffer(revmap->rm_metaBuf);
	if (revmap->rm_currBuf != InvalidBuffer)
		ReleaseBuffer(revmap->rm_currBuf);
	pfree(revmap);
}

/*
 * Extend the revmap to cover the given heap block number.
 */
void
brinRevmapExtend(BrinRevmap *revmap, BlockNumber heapBlk)
{
	BlockNumber mapBlk PG_USED_FOR_ASSERTS_ONLY;

	mapBlk = revmap_extend_and_get_blkno(revmap, heapBlk);

	/* Ensure the buffer we got is in the expected range */
	Assert(mapBlk != InvalidBlockNumber &&
		   mapBlk != BRIN_METAPAGE_BLKNO &&
		   mapBlk <= revmap->rm_lastRevmapPage);
}

/*
 * Prepare to insert an entry into the revmap; the revmap buffer in which the
 * entry is to reside is locked and returned.  Most callers should call
 * brinRevmapExtend beforehand, as this routine does not extend the revmap if
 * it's not long enough.
 *
 * The returned buffer is also recorded in the revmap struct; finishing that
 * releases the buffer, therefore the caller needn't do it explicitly.
 */
Buffer
brinLockRevmapPageForUpdate(BrinRevmap *revmap, BlockNumber heapBlk)
{
	Buffer		rmBuf;

	rmBuf = revmap_get_buffer(revmap, heapBlk);
	LockBuffer(rmBuf, BUFFER_LOCK_EXCLUSIVE);

	return rmBuf;
}

/*
 * In the given revmap buffer (locked appropriately by caller), which is used
 * in a BRIN index of pagesPerRange pages per range, set the element
 * corresponding to heap block number heapBlk to the given TID.
 *
 * Once the operation is complete, the caller must update the LSN on the
 * returned buffer.
 *
 * This is used both in regular operation and during WAL replay.
 */
void
brinSetHeapBlockItemptr(Buffer buf, BlockNumber pagesPerRange,
						BlockNumber heapBlk, ItemPointerData tid)
{
	RevmapContents *contents;
	ItemPointerData *iptr;
	Page		page;

	/* The correct page should already be pinned and locked */
	page = BufferGetPage(buf);
	contents = (RevmapContents *) PageGetContents(page);
	iptr = (ItemPointerData *) contents->rm_tids;
	iptr += HEAPBLK_TO_REVMAP_INDEX(pagesPerRange, heapBlk);

	ItemPointerSet(iptr,
				   ItemPointerGetBlockNumber(&tid),
				   ItemPointerGetOffsetNumber(&tid));
}

/*
 * Fetch the BrinTuple for a given heap block.
 *
 * The buffer containing the tuple is locked, and returned in *buf. As an
 * optimization, the caller can pass a pinned buffer *buf on entry, which will
 * avoid a pin-unpin cycle when the next tuple is on the same page as a
 * previous one.
 *
 * If no tuple is found for the given heap range, returns NULL. In that case,
 * *buf might still be updated, but it's not locked.
 *
 * The output tuple offset within the buffer is returned in *off, and its size
 * is returned in *size.
 */
BrinTuple *
brinGetTupleForHeapBlock(BrinRevmap *revmap, BlockNumber heapBlk,
						 Buffer *buf, OffsetNumber *off, Size *size, int mode,
						 Snapshot snapshot)
{
	Relation	idxRel = revmap->rm_irel;
	BlockNumber mapBlk;
	RevmapContents *contents;
	ItemPointerData *iptr;
	BlockNumber blk;
	Page		page;
	ItemId		lp;
	BrinTuple  *tup;
	ItemPointerData previptr;

	/* normalize the heap block number to be the first page in the range */
	heapBlk = (heapBlk / revmap->rm_pagesPerRange) * revmap->rm_pagesPerRange;

	/* Compute the revmap page number we need */
	mapBlk = revmap_get_blkno(revmap, heapBlk);
	if (mapBlk == InvalidBlockNumber)
	{
		*off = InvalidOffsetNumber;
		return NULL;
	}

	ItemPointerSetInvalid(&previptr);
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		if (revmap->rm_currBuf == InvalidBuffer ||
			BufferGetBlockNumber(revmap->rm_currBuf) != mapBlk)
		{
			if (revmap->rm_currBuf != InvalidBuffer)
				ReleaseBuffer(revmap->rm_currBuf);

			Assert(mapBlk != InvalidBlockNumber);
			revmap->rm_currBuf = ReadBuffer(revmap->rm_irel, mapBlk);
		}

		LockBuffer(revmap->rm_currBuf, BUFFER_LOCK_SHARE);

		contents = (RevmapContents *)
			PageGetContents(BufferGetPage(revmap->rm_currBuf));
		iptr = contents->rm_tids;
		iptr += HEAPBLK_TO_REVMAP_INDEX(revmap->rm_pagesPerRange, heapBlk);

		if (!ItemPointerIsValid(iptr))
		{
			LockBuffer(revmap->rm_currBuf, BUFFER_LOCK_UNLOCK);
			return NULL;
		}

		/*
		 * Check the TID we got in a previous iteration, if any, and save the
		 * current TID we got from the revmap; if we loop, we can sanity-check
		 * that the next one we get is different.  Otherwise we might be stuck
		 * looping forever if the revmap is somehow badly broken.
		 */
		if (ItemPointerIsValid(&previptr) && ItemPointerEquals(&previptr, iptr))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
			errmsg_internal("corrupted BRIN index: inconsistent range map")));
		previptr = *iptr;

		blk = ItemPointerGetBlockNumber(iptr);
		*off = ItemPointerGetOffsetNumber(iptr);

		LockBuffer(revmap->rm_currBuf, BUFFER_LOCK_UNLOCK);

		/* Ok, got a pointer to where the BrinTuple should be. Fetch it. */
		if (!BufferIsValid(*buf) || BufferGetBlockNumber(*buf) != blk)
		{
			if (BufferIsValid(*buf))
				ReleaseBuffer(*buf);
			*buf = ReadBuffer(idxRel, blk);
		}
		LockBuffer(*buf, mode);
		page = BufferGetPage(*buf);
		TestForOldSnapshot(snapshot, idxRel, page);

		/* If we land on a revmap page, start over */
		if (BRIN_IS_REGULAR_PAGE(page))
		{
			if (*off > PageGetMaxOffsetNumber(page))
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg_internal("corrupted BRIN index: inconsistent range map")));
			lp = PageGetItemId(page, *off);
			if (ItemIdIsUsed(lp))
			{
				tup = (BrinTuple *) PageGetItem(page, lp);

				if (tup->bt_blkno == heapBlk)
				{
					if (size)
						*size = ItemIdGetLength(lp);
					/* found it! */
					return tup;
				}
			}
		}

		/*
		 * No luck. Assume that the revmap was updated concurrently.
		 */
		LockBuffer(*buf, BUFFER_LOCK_UNLOCK);
	}
	/* not reached, but keep compiler quiet */
	return NULL;
}

/*
 * Given a heap block number, find the corresponding physical revmap block
 * number and return it.  If the revmap page hasn't been allocated yet, return
 * InvalidBlockNumber.
 */
static BlockNumber
revmap_get_blkno(BrinRevmap *revmap, BlockNumber heapBlk)
{
	BlockNumber targetblk;

	/* obtain revmap block number, skip 1 for metapage block */
	targetblk = HEAPBLK_TO_REVMAP_BLK(revmap->rm_pagesPerRange, heapBlk) + 1;

	/* Normal case: the revmap page is already allocated */
	if (targetblk <= revmap->rm_lastRevmapPage)
		return targetblk;

	return InvalidBlockNumber;
}

/*
 * Obtain and return a buffer containing the revmap page for the given heap
 * page.  The revmap must have been previously extended to cover that page.
 * The returned buffer is also recorded in the revmap struct; finishing that
 * releases the buffer, therefore the caller needn't do it explicitly.
 */
static Buffer
revmap_get_buffer(BrinRevmap *revmap, BlockNumber heapBlk)
{
	BlockNumber mapBlk;

	/* Translate the heap block number to physical index location. */
	mapBlk = revmap_get_blkno(revmap, heapBlk);

	if (mapBlk == InvalidBlockNumber)
		elog(ERROR, "revmap does not cover heap block %u", heapBlk);

	/* Ensure the buffer we got is in the expected range */
	Assert(mapBlk != BRIN_METAPAGE_BLKNO &&
		   mapBlk <= revmap->rm_lastRevmapPage);

	/*
	 * Obtain the buffer from which we need to read.  If we already have the
	 * correct buffer in our access struct, use that; otherwise, release that,
	 * (if valid) and read the one we need.
	 */
	if (revmap->rm_currBuf == InvalidBuffer ||
		mapBlk != BufferGetBlockNumber(revmap->rm_currBuf))
	{
		if (revmap->rm_currBuf != InvalidBuffer)
			ReleaseBuffer(revmap->rm_currBuf);

		revmap->rm_currBuf = ReadBuffer(revmap->rm_irel, mapBlk);
	}

	return revmap->rm_currBuf;
}

/*
 * Given a heap block number, find the corresponding physical revmap block
 * number and return it. If the revmap page hasn't been allocated yet, extend
 * the revmap until it is.
 */
static BlockNumber
revmap_extend_and_get_blkno(BrinRevmap *revmap, BlockNumber heapBlk)
{
	BlockNumber targetblk;

	/* obtain revmap block number, skip 1 for metapage block */
	targetblk = HEAPBLK_TO_REVMAP_BLK(revmap->rm_pagesPerRange, heapBlk) + 1;

	/* Extend the revmap, if necessary */
	while (targetblk > revmap->rm_lastRevmapPage)
	{
		CHECK_FOR_INTERRUPTS();
		revmap_physical_extend(revmap);
	}

	return targetblk;
}

/*
 * Try to extend the revmap by one page.  This might not happen for a number of
 * reasons; caller is expected to retry until the expected outcome is obtained.
 */
static void
revmap_physical_extend(BrinRevmap *revmap)
{
	Buffer		buf;
	Page		page;
	Page		metapage;
	BrinMetaPageData *metadata;
	BlockNumber mapBlk;
	BlockNumber nblocks;
	Relation	irel = revmap->rm_irel;
	bool		needLock = !RELATION_IS_LOCAL(irel);

	/*
	 * Lock the metapage. This locks out concurrent extensions of the revmap,
	 * but note that we still need to grab the relation extension lock because
	 * another backend can extend the index with regular BRIN pages.
	 */
	LockBuffer(revmap->rm_metaBuf, BUFFER_LOCK_EXCLUSIVE);
	metapage = BufferGetPage(revmap->rm_metaBuf);
	metadata = (BrinMetaPageData *) PageGetContents(metapage);

	/*
	 * Check that our cached lastRevmapPage value was up-to-date; if it
	 * wasn't, update the cached copy and have caller start over.
	 */
	if (metadata->lastRevmapPage != revmap->rm_lastRevmapPage)
	{
		revmap->rm_lastRevmapPage = metadata->lastRevmapPage;
		LockBuffer(revmap->rm_metaBuf, BUFFER_LOCK_UNLOCK);
		return;
	}
	mapBlk = metadata->lastRevmapPage + 1;

	nblocks = RelationGetNumberOfBlocks(irel);
	if (mapBlk < nblocks)
	{
		buf = ReadBuffer(irel, mapBlk);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
	}
	else
	{
		if (needLock)
			LockRelationForExtension(irel, ExclusiveLock);

		buf = ReadBuffer(irel, P_NEW);
		if (BufferGetBlockNumber(buf) != mapBlk)
		{
			/*
			 * Very rare corner case: somebody extended the relation
			 * concurrently after we read its length.  If this happens, give
			 * up and have caller start over.  We will have to evacuate that
			 * page from under whoever is using it.
			 */
			if (needLock)
				UnlockRelationForExtension(irel, ExclusiveLock);
			LockBuffer(revmap->rm_metaBuf, BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(buf);
			return;
		}
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);

		if (needLock)
			UnlockRelationForExtension(irel, ExclusiveLock);
	}

	/* Check that it's a regular block (or an empty page) */
	if (!PageIsNew(page) && !BRIN_IS_REGULAR_PAGE(page))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
		  errmsg("unexpected page type 0x%04X in BRIN index \"%s\" block %u",
				 BrinPageType(page),
				 RelationGetRelationName(irel),
				 BufferGetBlockNumber(buf))));

	/* If the page is in use, evacuate it and restart */
	if (brin_start_evacuating_page(irel, buf))
	{
		LockBuffer(revmap->rm_metaBuf, BUFFER_LOCK_UNLOCK);
		brin_evacuate_page(irel, revmap->rm_pagesPerRange, revmap, buf);

		/* have caller start over */
		return;
	}

	/*
	 * Ok, we have now locked the metapage and the target block. Re-initialize
	 * it as a revmap page.
	 */
	START_CRIT_SECTION();

	/* the rm_tids array is initialized to all invalid by PageInit */
	brin_page_init(page, BRIN_PAGETYPE_REVMAP);
	MarkBufferDirty(buf);

	metadata->lastRevmapPage = mapBlk;
	MarkBufferDirty(revmap->rm_metaBuf);

	if (RelationNeedsWAL(revmap->rm_irel))
	{
		xl_brin_revmap_extend xlrec;
		XLogRecPtr	recptr;

		xlrec.targetBlk = mapBlk;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfBrinRevmapExtend);
		XLogRegisterBuffer(0, revmap->rm_metaBuf, 0);

		XLogRegisterBuffer(1, buf, REGBUF_WILL_INIT);

		recptr = XLogInsert(RM_BRIN_ID, XLOG_BRIN_REVMAP_EXTEND);
		PageSetLSN(metapage, recptr);
		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	LockBuffer(revmap->rm_metaBuf, BUFFER_LOCK_UNLOCK);

	UnlockReleaseBuffer(buf);
}
