/*-------------------------------------------------------------------------
 *
 * visibilitymap.c
 *	  bitmap for tracking visibility of heap tuples
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/heap/visibilitymap.c,v 1.5.2.1 2009/08/24 02:18:40 tgl Exp $
 *
 * INTERFACE ROUTINES
 *		visibilitymap_clear - clear a bit in the visibility map
 *		visibilitymap_pin	- pin a map page for setting a bit
 *		visibilitymap_set	- set a bit in a previously pinned page
 *		visibilitymap_test	- test if a bit is set
 *
 * NOTES
 *
 * The visibility map is a bitmap with one bit per heap page. A set bit means
 * that all tuples on the page are known visible to all transactions, and 
 * therefore the page doesn't need to be vacuumed. The map is conservative in
 * the sense that we make sure that whenever a bit is set, we know the
 * condition is true, but if a bit is not set, it might or might not be true.
 *
 * There's no explicit WAL logging in the functions in this file. The callers
 * must make sure that whenever a bit is cleared, the bit is cleared on WAL
 * replay of the updating operation as well. Setting bits during recovery
 * isn't necessary for correctness.
 *
 * Currently, the visibility map is only used as a hint, to speed up VACUUM.
 * A corrupted visibility map won't cause data corruption, although it can
 * make VACUUM skip pages that need vacuuming, until the next anti-wraparound
 * vacuum. The visibility map is not used for anti-wraparound vacuums, because
 * an anti-wraparound vacuum needs to freeze tuples and observe the latest xid
 * present in the table, even on pages that don't have any dead tuples.
 *
 * Although the visibility map is just a hint at the moment, the PD_ALL_VISIBLE
 * flag on heap pages *must* be correct, because it is used to skip visibility
 * checking.
 *
 * LOCKING
 *
 * In heapam.c, whenever a page is modified so that not all tuples on the
 * page are visible to everyone anymore, the corresponding bit in the
 * visibility map is cleared. The bit in the visibility map is cleared
 * after releasing the lock on the heap page, to avoid holding the lock
 * over possible I/O to read in the visibility map page.
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
 * TODO
 *
 * It would be nice to use the visibility map to skip visibility checks in
 * index scans.
 *
 * Currently, the visibility map is not 100% correct all the time.
 * During updates, the bit in the visibility map is cleared after releasing
 * the lock on the heap page. During the window between releasing the lock
 * and clearing the bit in the visibility map, the bit in the visibility map
 * is set, but the new insertion or deletion is not yet visible to other
 * backends.
 *
 * That might actually be OK for the index scans, though. The newly inserted
 * tuple wouldn't have an index pointer yet, so all tuples reachable from an
 * index would still be visible to all other backends, and deletions wouldn't
 * be visible to other backends yet.  (But HOT breaks that argument, no?)
 *
 * There's another hole in the way the PD_ALL_VISIBLE flag is set. When
 * vacuum observes that all tuples are visible to all, it sets the flag on
 * the heap page, and also sets the bit in the visibility map. If we then
 * crash, and only the visibility map page was flushed to disk, we'll have
 * a bit set in the visibility map, but the corresponding flag on the heap
 * page is not set. If the heap page is then updated, the updater won't
 * know to clear the bit in the visibility map.  (Isn't that prevented by
 * the LSN interlock?)
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/visibilitymap.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
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

/* Number of bits allocated for each heap block. */
#define BITS_PER_HEAPBLOCK 1

/* Number of heap blocks we can represent in one byte. */
#define HEAPBLOCKS_PER_BYTE 8

/* Number of heap blocks we can represent in one visibility map page. */
#define HEAPBLOCKS_PER_PAGE (MAPSIZE * HEAPBLOCKS_PER_BYTE)

/* Mapping from heap block number to the right bit in the visibility map */
#define HEAPBLK_TO_MAPBLOCK(x) ((x) / HEAPBLOCKS_PER_PAGE)
#define HEAPBLK_TO_MAPBYTE(x) (((x) % HEAPBLOCKS_PER_PAGE) / HEAPBLOCKS_PER_BYTE)
#define HEAPBLK_TO_MAPBIT(x) ((x) % HEAPBLOCKS_PER_BYTE)

/* prototypes for internal routines */
static Buffer vm_readbuf(Relation rel, BlockNumber blkno, bool extend);
static void vm_extend(Relation rel, BlockNumber nvmblocks);


/*
 *	visibilitymap_clear - clear a bit in visibility map
 *
 * Clear a bit in the visibility map, marking that not all tuples are
 * visible to all transactions anymore.
 */
void
visibilitymap_clear(Relation rel, BlockNumber heapBlk)
{
	BlockNumber mapBlock = HEAPBLK_TO_MAPBLOCK(heapBlk);
	int			mapByte = HEAPBLK_TO_MAPBYTE(heapBlk);
	int			mapBit = HEAPBLK_TO_MAPBIT(heapBlk);
	uint8		mask = 1 << mapBit;
	Buffer		mapBuffer;
	char	   *map;

#ifdef TRACE_VISIBILITYMAP
	elog(DEBUG1, "vm_clear %s %d", RelationGetRelationName(rel), heapBlk);
#endif

	mapBuffer = vm_readbuf(rel, mapBlock, false);
	if (!BufferIsValid(mapBuffer))
		return;					/* nothing to do */

	LockBuffer(mapBuffer, BUFFER_LOCK_EXCLUSIVE);
	map = PageGetContents(BufferGetPage(mapBuffer));

	if (map[mapByte] & mask)
	{
		map[mapByte] &= ~mask;

		MarkBufferDirty(mapBuffer);
	}

	UnlockReleaseBuffer(mapBuffer);
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
 * an earlier call to visibilitymap_pin or visibilitymap_test on the same
 * relation. On return, *buf is a valid buffer with the map page containing
 * the the bit for heapBlk.
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
 *	visibilitymap_set - set a bit on a previously pinned page
 *
 * recptr is the LSN of the heap page. The LSN of the visibility map page is
 * advanced to that, to make sure that the visibility map doesn't get flushed
 * to disk before the update to the heap page that made all tuples visible.
 *
 * This is an opportunistic function. It does nothing, unless *buf
 * contains the bit for heapBlk. Call visibilitymap_pin first to pin
 * the right map page. This function doesn't do any I/O.
 */
void
visibilitymap_set(Relation rel, BlockNumber heapBlk, XLogRecPtr recptr,
				  Buffer *buf)
{
	BlockNumber mapBlock = HEAPBLK_TO_MAPBLOCK(heapBlk);
	uint32		mapByte = HEAPBLK_TO_MAPBYTE(heapBlk);
	uint8		mapBit = HEAPBLK_TO_MAPBIT(heapBlk);
	Page		page;
	char	   *map;

#ifdef TRACE_VISIBILITYMAP
	elog(DEBUG1, "vm_set %s %d", RelationGetRelationName(rel), heapBlk);
#endif

	/* Check that we have the right page pinned */
	if (!BufferIsValid(*buf) || BufferGetBlockNumber(*buf) != mapBlock)
		return;

	page = BufferGetPage(*buf);
	map = PageGetContents(page);
	LockBuffer(*buf, BUFFER_LOCK_EXCLUSIVE);

	if (!(map[mapByte] & (1 << mapBit)))
	{
		map[mapByte] |= (1 << mapBit);

		if (XLByteLT(PageGetLSN(page), recptr))
			PageSetLSN(page, recptr);
		PageSetTLI(page, ThisTimeLineID);
		MarkBufferDirty(*buf);
	}

	LockBuffer(*buf, BUFFER_LOCK_UNLOCK);
}

/*
 *	visibilitymap_test - test if a bit is set
 *
 * Are all tuples on heapBlk visible to all, according to the visibility map?
 *
 * On entry, *buf should be InvalidBuffer or a valid buffer returned by an
 * earlier call to visibilitymap_pin or visibilitymap_test on the same
 * relation. On return, *buf is a valid buffer with the map page containing
 * the the bit for heapBlk, or InvalidBuffer. The caller is responsible for
 * releasing *buf after it's done testing and setting bits.
 */
bool
visibilitymap_test(Relation rel, BlockNumber heapBlk, Buffer *buf)
{
	BlockNumber mapBlock = HEAPBLK_TO_MAPBLOCK(heapBlk);
	uint32		mapByte = HEAPBLK_TO_MAPBYTE(heapBlk);
	uint8		mapBit = HEAPBLK_TO_MAPBIT(heapBlk);
	bool		result;
	char	   *map;

#ifdef TRACE_VISIBILITYMAP
	elog(DEBUG1, "vm_test %s %d", RelationGetRelationName(rel), heapBlk);
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
	 * We don't need to lock the page, as we're only looking at a single bit.
	 */
	result = (map[mapByte] & (1 << mapBit)) ? true : false;

	return result;
}

/*
 *	visibilitymap_test - truncate the visibility map
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

		/*
		 * Mask out the unwanted bits of the last remaining byte.
		 *
		 * ((1 << 0) - 1) = 00000000 ((1 << 1) - 1) = 00000001 ... ((1 << 6) -
		 * 1) = 00111111 ((1 << 7) - 1) = 01111111
		 */
		map[truncByte] &= (1 << truncBit) - 1;

		MarkBufferDirty(mapBuffer);
		UnlockReleaseBuffer(mapBuffer);
	}
	else
		newnblocks = truncBlock;

	if (smgrnblocks(rel->rd_smgr, VISIBILITYMAP_FORKNUM) < newnblocks)
	{
		/* nothing to do, the file was already smaller than requested size */
		return;
	}

	smgrtruncate(rel->rd_smgr, VISIBILITYMAP_FORKNUM, newnblocks,
				 rel->rd_istemp);

	/*
	 * Need to invalidate the relcache entry, because rd_vm_nblocks seen by
	 * other backends is no longer valid.
	 */
	if (!InRecovery)
		CacheInvalidateRelcache(rel);

	rel->rd_vm_nblocks = newnblocks;
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

	RelationOpenSmgr(rel);

	/*
	 * The current size of the visibility map fork is kept in relcache, to
	 * avoid reading beyond EOF. If we haven't cached the size of the map yet,
	 * do that first.
	 */
	if (rel->rd_vm_nblocks == InvalidBlockNumber)
	{
		if (smgrexists(rel->rd_smgr, VISIBILITYMAP_FORKNUM))
			rel->rd_vm_nblocks = smgrnblocks(rel->rd_smgr,
											 VISIBILITYMAP_FORKNUM);
		else
			rel->rd_vm_nblocks = 0;
	}

	/* Handle requests beyond EOF */
	if (blkno >= rel->rd_vm_nblocks)
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
	 * before we get the lock.
	 */
	LockRelationForExtension(rel, ExclusiveLock);

	/* Create the file first if it doesn't exist */
	if ((rel->rd_vm_nblocks == 0 || rel->rd_vm_nblocks == InvalidBlockNumber)
		&& !smgrexists(rel->rd_smgr, VISIBILITYMAP_FORKNUM))
	{
		smgrcreate(rel->rd_smgr, VISIBILITYMAP_FORKNUM, false);
		vm_nblocks_now = 0;
	}
	else
		vm_nblocks_now = smgrnblocks(rel->rd_smgr, VISIBILITYMAP_FORKNUM);

	while (vm_nblocks_now < vm_nblocks)
	{
		smgrextend(rel->rd_smgr, VISIBILITYMAP_FORKNUM, vm_nblocks_now,
				   (char *) pg, rel->rd_istemp);
		vm_nblocks_now++;
	}

	UnlockRelationForExtension(rel, ExclusiveLock);

	pfree(pg);

	/* Update the relcache with the up-to-date size */
	if (!InRecovery)
		CacheInvalidateRelcache(rel);
	rel->rd_vm_nblocks = vm_nblocks_now;
}
