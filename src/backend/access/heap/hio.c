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
 *	  $Id: hio.c,v 1.35 2001/01/24 19:42:48 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/hio.h"

/*
 * RelationPutHeapTuple	- place tuple at specified page
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

	/* ----------------
	 *	increment access statistics
	 * ----------------
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
 * Returns (locked) buffer with free space >= given len.
 *
 * Note that we use LockPage to lock relation for extension. We can 
 * do this as long as in all other places we use page-level locking
 * for indices only. Alternatively, we could define pseudo-table as
 * we do for transactions with XactLockTable.
 *
 * ELOG(ERROR) is allowed here, so this routine *must* be called
 * before any (unlogged) changes are made in buffer pool.
 *
 */
Buffer
RelationGetBufferForTuple(Relation relation, Size len)
{
	Buffer		buffer;
	Page		pageHeader;
	BlockNumber lastblock;

	len = MAXALIGN(len);		/* be conservative */

	/*
	 * If we're gonna fail for oversize tuple, do it right away
	 */
	if (len > MaxTupleSize)
		elog(ERROR, "Tuple is too big: size %lu, max size %ld",
			 (unsigned long)len, MaxTupleSize);

	if (!relation->rd_myxactonly)
		LockPage(relation, 0, ExclusiveLock);

	/*
	 * XXX This does an lseek - VERY expensive - but at the moment it is
	 * the only way to accurately determine how many blocks are in a
	 * relation.  A good optimization would be to get this to actually
	 * work properly.
	 */
	lastblock = RelationGetNumberOfBlocks(relation);

	/*
	 * Get the last existing page --- may need to create the first one if
	 * this is a virgin relation.
	 */
	if (lastblock == 0)
	{
		buffer = ReadBuffer(relation, P_NEW);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		pageHeader = (Page) BufferGetPage(buffer);
		Assert(PageIsNew((PageHeader) pageHeader));
		PageInit(pageHeader, BufferGetPageSize(buffer), 0);
	}
	else
	{
		buffer = ReadBuffer(relation, lastblock - 1);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		pageHeader = (Page) BufferGetPage(buffer);
	}

	/*
	 * Is there room on the last existing page?
	 */
	if (len > PageGetFreeSpace(pageHeader))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		buffer = ReleaseAndReadBuffer(buffer, relation, P_NEW);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		pageHeader = (Page) BufferGetPage(buffer);
		Assert(PageIsNew((PageHeader) pageHeader));
		PageInit(pageHeader, BufferGetPageSize(buffer), 0);

		if (len > PageGetFreeSpace(pageHeader))
		{
			/* We should not get here given the test at the top */
			elog(STOP, "Tuple is too big: size %lu",
				(unsigned long)len);
		}
	}

	if (!relation->rd_myxactonly)
		UnlockPage(relation, 0, ExclusiveLock);

	return(buffer);

}
