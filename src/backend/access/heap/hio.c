/*-------------------------------------------------------------------------
 *
 * hio.c
 *	  POSTGRES heap access method input/output code.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Id: hio.c,v 1.39 2001/05/16 22:35:12 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/hio.h"

/*
 * RelationPutHeapTuple - place tuple at specified page
 *
 * !!! ELOG(ERROR) IS DISALLOWED HERE !!!
 *
 * Note - we assume that caller hold BUFFER_LOCK_EXCLUSIVE on the buffer.
 *
 */
void
RelationPutHeapTuple(Relation relation,
					 Buffer buffer,
					 HeapTuple tuple)
{
	Page		pageHeader;
	OffsetNumber offnum;
	Size		len;
	ItemId		itemId;
	Item		item;

	/*
	 * increment access statistics
	 */
	IncrHeapAccessStat(local_RelationPutHeapTuple);
	IncrHeapAccessStat(global_RelationPutHeapTuple);

	pageHeader = (Page) BufferGetPage(buffer);
	len = MAXALIGN(tuple->t_len);		/* be conservative */
	Assert(len <= PageGetFreeSpace(pageHeader));

	offnum = PageAddItem((Page) pageHeader, (Item) tuple->t_data,
						 tuple->t_len, InvalidOffsetNumber, LP_USED);

	if (offnum == InvalidOffsetNumber)
		elog(STOP, "RelationPutHeapTuple: failed to add tuple");

	itemId = PageGetItemId((Page) pageHeader, offnum);
	item = PageGetItem((Page) pageHeader, itemId);

	ItemPointerSet(&((HeapTupleHeader) item)->t_ctid,
				   BufferGetBlockNumber(buffer), offnum);

	/* return an accurate tuple */
	ItemPointerSet(&tuple->t_self, BufferGetBlockNumber(buffer), offnum);
}

/*
 * RelationGetBufferForTuple
 *
 *	Returns exclusive-locked buffer with free space >= given len,
 *	being careful to select only a page at or beyond minblocknum
 *	in the relation.
 *
 *	The minblocknum parameter is needed to prevent deadlock between
 *	concurrent heap_update operations; see heap_update for details.
 *	Pass zero if you don't particularly care which page you get.
 *
 *	Note that we use LockPage to lock relation for extension. We can
 *	do this as long as in all other places we use page-level locking
 *	for indices only. Alternatively, we could define pseudo-table as
 *	we do for transactions with XactLockTable.
 *
 *	ELOG(ERROR) is allowed here, so this routine *must* be called
 *	before any (unlogged) changes are made in buffer pool.
 */
Buffer
RelationGetBufferForTuple(Relation relation, Size len,
						  BlockNumber minblocknum)
{
	Buffer		buffer = InvalidBuffer;
	Page		pageHeader;
	BlockNumber lastblock,
				oldnblocks;

	len = MAXALIGN(len);		/* be conservative */

	/*
	 * If we're gonna fail for oversize tuple, do it right away
	 */
	if (len > MaxTupleSize)
		elog(ERROR, "Tuple is too big: size %lu, max size %ld",
			 (unsigned long) len, MaxTupleSize);

	/*
	 * First, use relcache's record of table length to guess where the
	 * last page is, and try to put the tuple there.  This cached value
	 * may be out of date, in which case we'll be inserting into a non-last
	 * page, but that should be OK.  Note that in a newly created relcache
	 * entry, rd_nblocks may be zero; if so, we'll set it correctly below.
	 */
	if (relation->rd_nblocks > 0)
	{
		lastblock = relation->rd_nblocks - 1;
		if (lastblock >= minblocknum)
		{
			buffer = ReadBuffer(relation, lastblock);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
			pageHeader = (Page) BufferGetPage(buffer);
			if (len <= PageGetFreeSpace(pageHeader))
				return buffer;
			/*
			 * Doesn't fit, so we'll have to try someplace else.
			 */
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			/* buffer release will happen below... */
		}
	}

	/*
	 * Before extending relation, make sure no one else has done
	 * so more recently than our last rd_nblocks update.  (If we
	 * blindly extend the relation here, then probably most of the
	 * page the other guy added will end up going to waste.)
	 *
	 * We have to use a lock to ensure no one else is extending the
	 * rel at the same time, else we will both try to initialize the
	 * same new page.
	 */
	if (!relation->rd_myxactonly)
		LockPage(relation, 0, ExclusiveLock);

	oldnblocks = relation->rd_nblocks;
	/*
	 * XXX This does an lseek - rather expensive - but at the moment it is
	 * the only way to accurately determine how many blocks are in a
	 * relation.  Is it worth keeping an accurate file length in shared
	 * memory someplace, rather than relying on the kernel to do it for us?
	 */
	relation->rd_nblocks = RelationGetNumberOfBlocks(relation);

	if ((BlockNumber) relation->rd_nblocks > oldnblocks)
	{
		/*
		 * Someone else has indeed extended the relation recently.
		 * Try to fit our tuple into the new last page.
		 */
		lastblock = relation->rd_nblocks - 1;
		if (lastblock >= minblocknum)
		{
			buffer = ReleaseAndReadBuffer(buffer, relation, lastblock, false);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
			pageHeader = (Page) BufferGetPage(buffer);
			if (len <= PageGetFreeSpace(pageHeader))
			{
				/* OK, we don't need to extend again. */
				if (!relation->rd_myxactonly)
					UnlockPage(relation, 0, ExclusiveLock);
				return buffer;
			}
			/*
			 * Doesn't fit, so we'll have to extend the relation (again).
			 */
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			/* buffer release will happen below... */
		}
	}

	/*
	 * Extend the relation by one page and update rd_nblocks for next time.
	 *
	 * Note: at this point minblocknum is ignored; we won't extend by more
	 * than one block...
	 */
	lastblock = relation->rd_nblocks;
	buffer = ReleaseAndReadBuffer(buffer, relation, lastblock, true);
	relation->rd_nblocks = lastblock + 1;

	/*
	 * We need to initialize the empty new page.
	 */
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	pageHeader = (Page) BufferGetPage(buffer);
	Assert(PageIsNew((PageHeader) pageHeader));
	PageInit(pageHeader, BufferGetPageSize(buffer), 0);

	/*
	 * Release the file-extension lock; it's now OK for someone else
	 * to extend the relation some more.
	 */
	if (!relation->rd_myxactonly)
		UnlockPage(relation, 0, ExclusiveLock);

	if (len > PageGetFreeSpace(pageHeader))
	{
		/* We should not get here given the test at the top */
		elog(STOP, "Tuple is too big: size %lu",
			 (unsigned long) len);
	}

	return buffer;
}
