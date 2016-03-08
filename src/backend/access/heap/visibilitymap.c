/*-------------------------------------------------------------------------
 *
 * visibilitymap.c
 *	  bitmap for tracking visibility of heap tuples
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/visibilitymap.c
 *
 * INTERFACE ROUTINES
 *		visibilitymap_clear  - clear a bit in the visibility map
 *		visibilitymap_pin	 - pin a map page for setting a bit
 *		visibilitymap_pin_ok - check whether correct map page is already pinned
 *		visibilitymap_set	 - set a bit in a previously pinned page
 *		visibilitymap_get_status - get status of bits
 *		visibilitymap_count  - count number of bits set in visibility map
 *		visibilitymap_truncate	- truncate the visibility map
 *
 * NOTES
 *
 * The visibility map is a bitmap with two bits (all-visible and all-frozen)
 * per heap page. A set all-visible bit means that all tuples on the page are
 * known visible to all transactions, and therefore the page doesn't need to
 * be vacuumed. A set all-frozen bit means that all tuples on the page are
 * completely frozen, and therefore the page doesn't need to be vacuumed even
 * if whole table scanning vacuum is required (e.g. anti-wraparound vacuum).
 * The all-frozen bit must be set only when the page is already all-visible.
 *
 * The map is conservative in the sense that we make sure that whenever a bit
 * is set, we know the condition is true, but if a bit is not set, it might or
 * might not be true.
 *
 * Clearing both visibility map bits is not separately WAL-logged.  The callers
 * must make sure that whenever a bit is cleared, the bit is cleared on WAL
 * replay of the updating operation as well.
 *
 * When we *set* a visibility map during VACUUM, we must write WAL.  This may
 * seem counterintuitive, since the bit is basically a hint: if it is clear,
 * it may still be the case that every tuple on the page is visible to all
 * transactions; we just don't know that for certain.  The difficulty is that
 * there are two bits which are typically set together: the PD_ALL_VISIBLE bit
 * on the page itself, and the visibility map bit.  If a crash occurs after the
 * visibility map page makes it to disk and before the updated heap page makes
 * it to disk, redo must set the bit on the heap page.  Otherwise, the next
 * insert, update, or delete on the heap page will fail to realize that the
 * visibility map bit must be cleared, possibly causing index-only scans to
 * return wrong answers.
 *
 * VACUUM will normally skip pages for which the visibility map bit is set;
 * such pages can't contain any dead tuples and therefore don't need vacuuming.
 *
 * LOCKING
 *
 * In heapam.c, whenever a page is modified so that not all tuples on the
 * page are visible to everyone anymore, the corresponding bit in the
 * visibility map is cleared. In order to be crash-safe, we need to do this
 * while still holding a lock on the heap page and in the same critical
 * section that logs the page modification. However, we don't want to hold
 * the buffer lock over any I/O that may be required to read in the visibility
 * map page.  To avoid this, we examine the heap page before locking it;
 * if the page-level PD_ALL_VISIBLE bit is set, we pin the visibility map
 * bit.  Then, we lock the buffer.  But this creates a race condition: there
 * is a possibility that in the time it takes to lock the buffer, the
 * PD_ALL_VISIBLE bit gets set.  If that happens, we have to unlock the
 * buffer, pin the visibility map page, and relock the buffer.  This shouldn't
 * happen often, because only VACUUM currently sets visibility map bits,
 * and the race will only occur if VACUUM processes a given page at almost
 * exactly the same time that someone tries to further modify it.
 *
 * To set a bit, you need to hold a lock on the heap page. That prevents
 * the race condition where VACUUM sees that all tuples on the page are
 * visible to everyone, but another backend modifies the page before VACUUM
 * sets the bit in the visibility map.
 *
 * When a bit is set, the LSN of the visibility map page is updated to make
 * sure that the visibility map update doesn't get written to disk before the
 * WAL record of the changes that made it possible to set the bit is flushed.
 * But when a bit is cleared, we don't have to do that because it's always
 * safe to clear a bit in the map from correctness point of view.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam_xlog.h"
#include "access/visibilitymap.h"
#include "access/xlog.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "utils/inval.h"


/*#define TRACE_VISIBILITYMAP */

/*
 * Size of the bitmap on each visibility map page, in bytes. There's no
 * extra headers, so the whole page minus the standard page header is
 * used for the bitmap.
 */
#define MAPSIZE (BLCKSZ - MAXALIGN(SizeOfPageHeaderData))

/* Number of heap blocks we can represent in one visibility map page. */
#define HEAPBLOCKS_PER_PAGE (MAPSIZE * HEAPBLOCKS_PER_BYTE)

/* Mapping from heap block number to the right bit in the visibility map */
#define HEAPBLK_TO_MAPBLOCK(x) ((x) / HEAPBLOCKS_PER_PAGE)
#define HEAPBLK_TO_MAPBYTE(x) (((x) % HEAPBLOCKS_PER_PAGE) / HEAPBLOCKS_PER_BYTE)
#define HEAPBLK_TO_MAPBIT(x) (((x) % HEAPBLOCKS_PER_BYTE) * BITS_PER_HEAPBLOCK)

/* tables for fast counting of set bits for visible and frozen */
static const uint8 number_of_ones_for_visible[256] = {
	0, 1, 0, 1, 1, 2, 1, 2, 0, 1, 0, 1, 1, 2, 1, 2,
	1, 2, 1, 2, 2, 3, 2, 3, 1, 2, 1, 2, 2, 3, 2, 3,
	0, 1, 0, 1, 1, 2, 1, 2, 0, 1, 0, 1, 1, 2, 1, 2,
	1, 2, 1, 2, 2, 3, 2, 3, 1, 2, 1, 2, 2, 3, 2, 3,
	1, 2, 1, 2, 2, 3, 2, 3, 1, 2, 1, 2, 2, 3, 2, 3,
	2, 3, 2, 3, 3, 4, 3, 4, 2, 3, 2, 3, 3, 4, 3, 4,
	1, 2, 1, 2, 2, 3, 2, 3, 1, 2, 1, 2, 2, 3, 2, 3,
	2, 3, 2, 3, 3, 4, 3, 4, 2, 3, 2, 3, 3, 4, 3, 4,
	0, 1, 0, 1, 1, 2, 1, 2, 0, 1, 0, 1, 1, 2, 1, 2,
	1, 2, 1, 2, 2, 3, 2, 3, 1, 2, 1, 2, 2, 3, 2, 3,
	0, 1, 0, 1, 1, 2, 1, 2, 0, 1, 0, 1, 1, 2, 1, 2,
	1, 2, 1, 2, 2, 3, 2, 3, 1, 2, 1, 2, 2, 3, 2, 3,
	1, 2, 1, 2, 2, 3, 2, 3, 1, 2, 1, 2, 2, 3, 2, 3,
	2, 3, 2, 3, 3, 4, 3, 4, 2, 3, 2, 3, 3, 4, 3, 4,
	1, 2, 1, 2, 2, 3, 2, 3, 1, 2, 1, 2, 2, 3, 2, 3,
	2, 3, 2, 3, 3, 4, 3, 4, 2, 3, 2, 3, 3, 4, 3, 4
};
static const uint8 number_of_ones_for_frozen[256] = {
	0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 2, 2, 1, 1, 2, 2,
	0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 2, 2, 1, 1, 2, 2,
	1, 1, 2, 2, 1, 1, 2, 2, 2, 2, 3, 3, 2, 2, 3, 3,
	1, 1, 2, 2, 1, 1, 2, 2, 2, 2, 3, 3, 2, 2, 3, 3,
	0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 2, 2, 1, 1, 2, 2,
	0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 2, 2, 1, 1, 2, 2,
	1, 1, 2, 2, 1, 1, 2, 2, 2, 2, 3, 3, 2, 2, 3, 3,
	1, 1, 2, 2, 1, 1, 2, 2, 2, 2, 3, 3, 2, 2, 3, 3,
	1, 1, 2, 2, 1, 1, 2, 2, 2, 2, 3, 3, 2, 2, 3, 3,
	1, 1, 2, 2, 1, 1, 2, 2, 2, 2, 3, 3, 2, 2, 3, 3,
	2, 2, 3, 3, 2, 2, 3, 3, 3, 3, 4, 4, 3, 3, 4, 4,
	2, 2, 3, 3, 2, 2, 3, 3, 3, 3, 4, 4, 3, 3, 4, 4,
	1, 1, 2, 2, 1, 1, 2, 2, 2, 2, 3, 3, 2, 2, 3, 3,
	1, 1, 2, 2, 1, 1, 2, 2, 2, 2, 3, 3, 2, 2, 3, 3,
	2, 2, 3, 3, 2, 2, 3, 3, 3, 3, 4, 4, 3, 3, 4, 4,
	2, 2, 3, 3, 2, 2, 3, 3, 3, 3, 4, 4, 3, 3, 4, 4
};

/* prototypes for internal routines */
static Buffer vm_readbuf(Relation rel, BlockNumber blkno, bool extend);
static void vm_extend(Relation rel, BlockNumber nvmblocks);


/*
 *	visibilitymap_clear - clear all bits in visibility map
 *
 * You must pass a buffer containing the correct map page to this function.
 * Call visibilitymap_pin first to pin the right one. This function doesn't do
 * any I/O.
 */
void
visibilitymap_clear(Relation rel, BlockNumber heapBlk, Buffer buf)
{
	BlockNumber mapBlock = HEAPBLK_TO_MAPBLOCK(heapBlk);
	int			mapByte = HEAPBLK_TO_MAPBYTE(heapBlk);
	int			mapBit = HEAPBLK_TO_MAPBIT(heapBlk);
	uint8		mask = VISIBILITYMAP_VALID_BITS << mapBit;
	char	   *map;

#ifdef TRACE_VISIBILITYMAP
	elog(DEBUG1, "vm_clear %s %d", RelationGetRelationName(rel), heapBlk);
#endif

	if (!BufferIsValid(buf) || BufferGetBlockNumber(buf) != mapBlock)
		elog(ERROR, "wrong buffer passed to visibilitymap_clear");

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	map = PageGetContents(BufferGetPage(buf));

	if (map[mapByte] & mask)
	{
		map[mapByte] &= ~mask;

		MarkBufferDirty(buf);
	}

	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
}

/*
 *	visibilitymap_pin - pin a map page for setting a bit
 *
 * Setting a bit in the visibility map is a two-phase operation. First, call
 * visibilitymap_pin, to pin the visibility map page containing the bit for
 * the heap page. Because that can require I/O to read the map page, you
 * shouldn't hold a lock on the heap page while doing that. Then, call
 * visibilitymap_set to actually set the bit.
 *
 * On entry, *buf should be InvalidBuffer or a valid buffer returned by
 * an earlier call to visibilitymap_pin or visibilitymap_get_status on the same
 * relation. On return, *buf is a valid buffer with the map page containing
 * the bit for heapBlk.
 *
 * If the page doesn't exist in the map file yet, it is extended.
 */
void
visibilitymap_pin(Relation rel, BlockNumber heapBlk, Buffer *buf)
{
	BlockNumber mapBlock = HEAPBLK_TO_MAPBLOCK(heapBlk);

	/* Reuse the old pinned buffer if possible */
	if (BufferIsValid(*buf))
	{
		if (BufferGetBlockNumber(*buf) == mapBlock)
			return;

		ReleaseBuffer(*buf);
	}
	*buf = vm_readbuf(rel, mapBlock, true);
}

/*
 *	visibilitymap_pin_ok - do we already have the correct page pinned?
 *
 * On entry, buf should be InvalidBuffer or a valid buffer returned by
 * an earlier call to visibilitymap_pin or visibilitymap_get_status on the same
 * relation.  The return value indicates whether the buffer covers the
 * given heapBlk.
 */
bool
visibilitymap_pin_ok(BlockNumber heapBlk, Buffer buf)
{
	BlockNumber mapBlock = HEAPBLK_TO_MAPBLOCK(heapBlk);

	return BufferIsValid(buf) && BufferGetBlockNumber(buf) == mapBlock;
}

/*
 *	visibilitymap_set - set bit(s) on a previously pinned page
 *
 * recptr is the LSN of the XLOG record we're replaying, if we're in recovery,
 * or InvalidXLogRecPtr in normal running.  The page LSN is advanced to the
 * one provided; in normal running, we generate a new XLOG record and set the
 * page LSN to that value.  cutoff_xid is the largest xmin on the page being
 * marked all-visible; it is needed for Hot Standby, and can be
 * InvalidTransactionId if the page contains no tuples.  It can also be set
 * to InvalidTransactionId when a page that is already all-visible is being
 * marked all-frozen.
 *
 * Caller is expected to set the heap page's PD_ALL_VISIBLE bit before calling
 * this function. Except in recovery, caller should also pass the heap
 * buffer. When checksums are enabled and we're not in recovery, we must add
 * the heap buffer to the WAL chain to protect it from being torn.
 *
 * You must pass a buffer containing the correct map page to this function.
 * Call visibilitymap_pin first to pin the right one. This function doesn't do
 * any I/O.
 */
void
visibilitymap_set(Relation rel, BlockNumber heapBlk, Buffer heapBuf,
				  XLogRecPtr recptr, Buffer vmBuf, TransactionId cutoff_xid,
				  uint8 flags)
{
	BlockNumber mapBlock = HEAPBLK_TO_MAPBLOCK(heapBlk);
	uint32		mapByte = HEAPBLK_TO_MAPBYTE(heapBlk);
	uint8		mapBit = HEAPBLK_TO_MAPBIT(heapBlk);
	Page		page;
	uint8		*map;

#ifdef TRACE_VISIBILITYMAP
	elog(DEBUG1, "vm_set %s %d", RelationGetRelationName(rel), heapBlk);
#endif

	Assert(InRecovery || XLogRecPtrIsInvalid(recptr));
	Assert(InRecovery || BufferIsValid(heapBuf));
	Assert(flags & VISIBILITYMAP_VALID_BITS);

	/* Check that we have the right heap page pinned, if present */
	if (BufferIsValid(heapBuf) && BufferGetBlockNumber(heapBuf) != heapBlk)
		elog(ERROR, "wrong heap buffer passed to visibilitymap_set");

	/* Check that we have the right VM page pinned */
	if (!BufferIsValid(vmBuf) || BufferGetBlockNumber(vmBuf) != mapBlock)
		elog(ERROR, "wrong VM buffer passed to visibilitymap_set");

	page = BufferGetPage(vmBuf);
	map = (uint8 *)PageGetContents(page);
	LockBuffer(vmBuf, BUFFER_LOCK_EXCLUSIVE);

	if (flags != (map[mapByte] >> mapBit & VISIBILITYMAP_VALID_BITS))
	{
		START_CRIT_SECTION();

		map[mapByte] |= (flags << mapBit);
		MarkBufferDirty(vmBuf);

		if (RelationNeedsWAL(rel))
		{
			if (XLogRecPtrIsInvalid(recptr))
			{
				Assert(!InRecovery);
				recptr = log_heap_visible(rel->rd_node, heapBuf, vmBuf,
										  cutoff_xid, flags);

				/*
				 * If data checksums are enabled (or wal_log_hints=on), we
				 * need to protect the heap page from being torn.
				 */
				if (XLogHintBitIsNeeded())
				{
					Page		heapPage = BufferGetPage(heapBuf);

					/* caller is expected to set PD_ALL_VISIBLE first */
					Assert(PageIsAllVisible(heapPage));
					PageSetLSN(heapPage, recptr);
				}
			}
			PageSetLSN(page, recptr);
		}

		END_CRIT_SECTION();
	}

	LockBuffer(vmBuf, BUFFER_LOCK_UNLOCK);
}

/*
 *	visibilitymap_get_status - get status of bits
 *
 * Are all tuples on heapBlk visible to all or are marked frozen, according
 * to the visibility map?
 *
 * On entry, *buf should be InvalidBuffer or a valid buffer returned by an
 * earlier call to visibilitymap_pin or visibilitymap_get_status on the same
 * relation. On return, *buf is a valid buffer with the map page containing
 * the bit for heapBlk, or InvalidBuffer. The caller is responsible for
 * releasing *buf after it's done testing and setting bits, and must pass flags
 * for which it needs to check the value in visibility map.
 *
 * NOTE: This function is typically called without a lock on the heap page,
 * so somebody else could change the bit just after we look at it.  In fact,
 * since we don't lock the visibility map page either, it's even possible that
 * someone else could have changed the bit just before we look at it, but yet
 * we might see the old value.  It is the caller's responsibility to deal with
 * all concurrency issues!
 */
uint8
visibilitymap_get_status(Relation rel, BlockNumber heapBlk, Buffer *buf)
{
	BlockNumber mapBlock = HEAPBLK_TO_MAPBLOCK(heapBlk);
	uint32		mapByte = HEAPBLK_TO_MAPBYTE(heapBlk);
	uint8		mapBit = HEAPBLK_TO_MAPBIT(heapBlk);
	char	   *map;

#ifdef TRACE_VISIBILITYMAP
	elog(DEBUG1, "vm_get_status %s %d", RelationGetRelationName(rel), heapBlk);
#endif

	/* Reuse the old pinned buffer if possible */
	if (BufferIsValid(*buf))
	{
		if (BufferGetBlockNumber(*buf) != mapBlock)
		{
			ReleaseBuffer(*buf);
			*buf = InvalidBuffer;
		}
	}

	if (!BufferIsValid(*buf))
	{
		*buf = vm_readbuf(rel, mapBlock, false);
		if (!BufferIsValid(*buf))
			return false;
	}

	map = PageGetContents(BufferGetPage(*buf));

	/*
	 * A single byte read is atomic.  There could be memory-ordering effects
	 * here, but for performance reasons we make it the caller's job to worry
	 * about that.
	 */
	return ((map[mapByte] >> mapBit) & VISIBILITYMAP_VALID_BITS);
}

/*
 *	visibilitymap_count  - count number of bits set in visibility map
 *
 * Note: we ignore the possibility of race conditions when the table is being
 * extended concurrently with the call.  New pages added to the table aren't
 * going to be marked all-visible or all-frozen, so they won't affect the result.
 */
void
visibilitymap_count(Relation rel, BlockNumber *all_visible, BlockNumber *all_frozen)
{
	BlockNumber mapBlock;

	/* all_visible must be specified */
	Assert(all_visible);

	*all_visible = 0;
	if (all_frozen)
		*all_frozen = 0;

	for (mapBlock = 0;; mapBlock++)
	{
		Buffer		mapBuffer;
		unsigned char *map;
		int			i;

		/*
		 * Read till we fall off the end of the map.  We assume that any extra
		 * bytes in the last page are zeroed, so we don't bother excluding
		 * them from the count.
		 */
		mapBuffer = vm_readbuf(rel, mapBlock, false);
		if (!BufferIsValid(mapBuffer))
			break;

		/*
		 * We choose not to lock the page, since the result is going to be
		 * immediately stale anyway if anyone is concurrently setting or
		 * clearing bits, and we only really need an approximate value.
		 */
		map = (unsigned char *) PageGetContents(BufferGetPage(mapBuffer));

		for (i = 0; i < MAPSIZE; i++)
		{
			*all_visible += number_of_ones_for_visible[map[i]];
			if (all_frozen)
				*all_frozen += number_of_ones_for_frozen[map[i]];
		}

		ReleaseBuffer(mapBuffer);
	}
}

/*
 *	visibilitymap_truncate - truncate the visibility map
 *
 * The caller must hold AccessExclusiveLock on the relation, to ensure that
 * other backends receive the smgr invalidation event that this function sends
 * before they access the VM again.
 *
 * nheapblocks is the new size of the heap.
 */
void
visibilitymap_truncate(Relation rel, BlockNumber nheapblocks)
{
	BlockNumber newnblocks;

	/* last remaining block, byte, and bit */
	BlockNumber truncBlock = HEAPBLK_TO_MAPBLOCK(nheapblocks);
	uint32		truncByte = HEAPBLK_TO_MAPBYTE(nheapblocks);
	uint8		truncBit = HEAPBLK_TO_MAPBIT(nheapblocks);

#ifdef TRACE_VISIBILITYMAP
	elog(DEBUG1, "vm_truncate %s %d", RelationGetRelationName(rel), nheapblocks);
#endif

	RelationOpenSmgr(rel);

	/*
	 * If no visibility map has been created yet for this relation, there's
	 * nothing to truncate.
	 */
	if (!smgrexists(rel->rd_smgr, VISIBILITYMAP_FORKNUM))
		return;

	/*
	 * Unless the new size is exactly at a visibility map page boundary, the
	 * tail bits in the last remaining map page, representing truncated heap
	 * blocks, need to be cleared. This is not only tidy, but also necessary
	 * because we don't get a chance to clear the bits if the heap is extended
	 * again.
	 */
	if (truncByte != 0 || truncBit != 0)
	{
		Buffer		mapBuffer;
		Page		page;
		char	   *map;

		newnblocks = truncBlock + 1;

		mapBuffer = vm_readbuf(rel, truncBlock, false);
		if (!BufferIsValid(mapBuffer))
		{
			/* nothing to do, the file was already smaller */
			return;
		}

		page = BufferGetPage(mapBuffer);
		map = PageGetContents(page);

		LockBuffer(mapBuffer, BUFFER_LOCK_EXCLUSIVE);

		/* Clear out the unwanted bytes. */
		MemSet(&map[truncByte + 1], 0, MAPSIZE - (truncByte + 1));

		/*----
		 * Mask out the unwanted bits of the last remaining byte.
		 *
		 * ((1 << 0) - 1) = 00000000
		 * ((1 << 1) - 1) = 00000001
		 * ...
		 * ((1 << 6) - 1) = 00111111
		 * ((1 << 7) - 1) = 01111111
		 *----
		 */
		map[truncByte] &= (1 << truncBit) - 1;

		MarkBufferDirty(mapBuffer);
		UnlockReleaseBuffer(mapBuffer);
	}
	else
		newnblocks = truncBlock;

	if (smgrnblocks(rel->rd_smgr, VISIBILITYMAP_FORKNUM) <= newnblocks)
	{
		/* nothing to do, the file was already smaller than requested size */
		return;
	}

	/* Truncate the unused VM pages, and send smgr inval message */
	smgrtruncate(rel->rd_smgr, VISIBILITYMAP_FORKNUM, newnblocks);

	/*
	 * We might as well update the local smgr_vm_nblocks setting. smgrtruncate
	 * sent an smgr cache inval message, which will cause other backends to
	 * invalidate their copy of smgr_vm_nblocks, and this one too at the next
	 * command boundary.  But this ensures it isn't outright wrong until then.
	 */
	if (rel->rd_smgr)
		rel->rd_smgr->smgr_vm_nblocks = newnblocks;
}

/*
 * Read a visibility map page.
 *
 * If the page doesn't exist, InvalidBuffer is returned, or if 'extend' is
 * true, the visibility map file is extended.
 */
static Buffer
vm_readbuf(Relation rel, BlockNumber blkno, bool extend)
{
	Buffer		buf;

	/*
	 * We might not have opened the relation at the smgr level yet, or we
	 * might have been forced to close it by a sinval message.  The code below
	 * won't necessarily notice relation extension immediately when extend =
	 * false, so we rely on sinval messages to ensure that our ideas about the
	 * size of the map aren't too far out of date.
	 */
	RelationOpenSmgr(rel);

	/*
	 * If we haven't cached the size of the visibility map fork yet, check it
	 * first.
	 */
	if (rel->rd_smgr->smgr_vm_nblocks == InvalidBlockNumber)
	{
		if (smgrexists(rel->rd_smgr, VISIBILITYMAP_FORKNUM))
			rel->rd_smgr->smgr_vm_nblocks = smgrnblocks(rel->rd_smgr,
													  VISIBILITYMAP_FORKNUM);
		else
			rel->rd_smgr->smgr_vm_nblocks = 0;
	}

	/* Handle requests beyond EOF */
	if (blkno >= rel->rd_smgr->smgr_vm_nblocks)
	{
		if (extend)
			vm_extend(rel, blkno + 1);
		else
			return InvalidBuffer;
	}

	/*
	 * Use ZERO_ON_ERROR mode, and initialize the page if necessary. It's
	 * always safe to clear bits, so it's better to clear corrupt pages than
	 * error out.
	 */
	buf = ReadBufferExtended(rel, VISIBILITYMAP_FORKNUM, blkno,
							 RBM_ZERO_ON_ERROR, NULL);
	if (PageIsNew(BufferGetPage(buf)))
		PageInit(BufferGetPage(buf), BLCKSZ, 0);
	return buf;
}

/*
 * Ensure that the visibility map fork is at least vm_nblocks long, extending
 * it if necessary with zeroed pages.
 */
static void
vm_extend(Relation rel, BlockNumber vm_nblocks)
{
	BlockNumber vm_nblocks_now;
	Page		pg;

	pg = (Page) palloc(BLCKSZ);
	PageInit(pg, BLCKSZ, 0);

	/*
	 * We use the relation extension lock to lock out other backends trying to
	 * extend the visibility map at the same time. It also locks out extension
	 * of the main fork, unnecessarily, but extending the visibility map
	 * happens seldom enough that it doesn't seem worthwhile to have a
	 * separate lock tag type for it.
	 *
	 * Note that another backend might have extended or created the relation
	 * by the time we get the lock.
	 */
	LockRelationForExtension(rel, ExclusiveLock);

	/* Might have to re-open if a cache flush happened */
	RelationOpenSmgr(rel);

	/*
	 * Create the file first if it doesn't exist.  If smgr_vm_nblocks is
	 * positive then it must exist, no need for an smgrexists call.
	 */
	if ((rel->rd_smgr->smgr_vm_nblocks == 0 ||
		 rel->rd_smgr->smgr_vm_nblocks == InvalidBlockNumber) &&
		!smgrexists(rel->rd_smgr, VISIBILITYMAP_FORKNUM))
		smgrcreate(rel->rd_smgr, VISIBILITYMAP_FORKNUM, false);

	vm_nblocks_now = smgrnblocks(rel->rd_smgr, VISIBILITYMAP_FORKNUM);

	/* Now extend the file */
	while (vm_nblocks_now < vm_nblocks)
	{
		PageSetChecksumInplace(pg, vm_nblocks_now);

		smgrextend(rel->rd_smgr, VISIBILITYMAP_FORKNUM, vm_nblocks_now,
				   (char *) pg, false);
		vm_nblocks_now++;
	}

	/*
	 * Send a shared-inval message to force other backends to close any smgr
	 * references they may have for this rel, which we are about to change.
	 * This is a useful optimization because it means that backends don't have
	 * to keep checking for creation or extension of the file, which happens
	 * infrequently.
	 */
	CacheInvalidateSmgr(rel->rd_smgr->smgr_rnode);

	/* Update local cache with the up-to-date size */
	rel->rd_smgr->smgr_vm_nblocks = vm_nblocks_now;

	UnlockRelationForExtension(rel, ExclusiveLock);

	pfree(pg);
}
