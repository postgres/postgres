/*-------------------------------------------------------------------------
 *
 * hio.c
 *	  POSTGRES heap access method input/output code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Id: hio.c,v 1.28 2000/01/15 02:59:20 petere Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/hio.h"

/*
 * amputunique	- place tuple at tid
 *	 Currently on errors, calls elog.  Perhaps should return -1?
 *	 Possible errors include the addition of a tuple to the page
 *	 between the time the linep is chosen and the page is L_UP'd.
 *
 *	 This should be coordinated with the B-tree code.
 *	 Probably needs to have an amdelunique to allow for
 *	 internal index records to be deleted and reordered as needed.
 *	 For the heap AM, this should never be needed.
 *
 *	 Note - we assume that caller hold BUFFER_LOCK_EXCLUSIVE on the buffer.
 *
 */
void
RelationPutHeapTuple(Relation relation,
					 Buffer buffer,
					 HeapTuple tuple)
{
	Page		pageHeader;
	OffsetNumber offnum;
	unsigned int len;
	ItemId		itemId;
	Item		item;

	/* ----------------
	 *	increment access statistics
	 * ----------------
	 */
	IncrHeapAccessStat(local_RelationPutHeapTuple);
	IncrHeapAccessStat(global_RelationPutHeapTuple);

	pageHeader = (Page) BufferGetPage(buffer);
	len = (unsigned) MAXALIGN(tuple->t_len); /* be conservative */
	Assert((int) len <= PageGetFreeSpace(pageHeader));

	offnum = PageAddItem((Page) pageHeader, (Item) tuple->t_data,
						 tuple->t_len, InvalidOffsetNumber, LP_USED);

	itemId = PageGetItemId((Page) pageHeader, offnum);
	item = PageGetItem((Page) pageHeader, itemId);

	ItemPointerSet(&((HeapTupleHeader) item)->t_ctid,
				   BufferGetBlockNumber(buffer), offnum);

	/*
	 * Let the caller do this!
	 *
	 * WriteBuffer(buffer);
	 */

	/* return an accurate tuple */
	ItemPointerSet(&tuple->t_self, BufferGetBlockNumber(buffer), offnum);
}

/*
 * This routine is another in the series of attempts to reduce the number
 * of I/O's and system calls executed in the various benchmarks.  In
 * particular, this routine is used to append data to the end of a relation
 * file without excessive lseeks.  This code should do no more than 2 semops
 * in the ideal case.
 *
 * Eventually, we should cache the number of blocks in a relation somewhere.
 * Until that time, this code will have to do an lseek to determine the number
 * of blocks in a relation.
 *
 * This code should ideally do at most 4 semops, 1 lseek, and possibly 1 write
 * to do an append; it's possible to eliminate 2 of the semops if we do direct
 * buffer stuff (!); the lseek and the write can go if we get
 * RelationGetNumberOfBlocks to be useful.
 *
 * NOTE: This code presumes that we have a write lock on the relation.
 * Not now - we use extend locking...
 *
 * Also note that this routine probably shouldn't have to exist, and does
 * screw up the call graph rather badly, but we are wasting so much time and
 * system resources being massively general that we are losing badly in our
 * performance benchmarks.
 */
void
RelationPutHeapTupleAtEnd(Relation relation, HeapTuple tuple)
{
	Buffer		buffer;
	Page		pageHeader;
	BlockNumber lastblock;
	OffsetNumber offnum;
	unsigned int len;
	ItemId		itemId;
	Item		item;

	len = (unsigned) MAXALIGN(tuple->t_len); /* be conservative */

	/*
	 * If we're gonna fail for oversize tuple, do it right away...
	 * this code should go away eventually.
	 */
	if (len > MaxTupleSize)
		elog(ERROR, "Tuple is too big: size %d, max size %ld",
			 len, MaxTupleSize);

	/*
	 * Lock relation for extension. We can use LockPage here as long as in
	 * all other places we use page-level locking for indices only.
	 * Alternatively, we could define pseudo-table as we do for
	 * transactions with XactLockTable.
	 */
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
	 * Get the last existing page --- may need to create the first one
	 * if this is a virgin relation.
	 */
	if (lastblock == 0)
	{
		/* what exactly is this all about??? */
		buffer = ReadBuffer(relation, lastblock);
		pageHeader = (Page) BufferGetPage(buffer);
		Assert(PageIsNew((PageHeader) pageHeader));
		buffer = ReleaseAndReadBuffer(buffer, relation, P_NEW);
		pageHeader = (Page) BufferGetPage(buffer);
		PageInit(pageHeader, BufferGetPageSize(buffer), 0);
	}
	else
		buffer = ReadBuffer(relation, lastblock - 1);

	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	pageHeader = (Page) BufferGetPage(buffer);

	/*
	 * Is there room on the last existing page?
	 */
	if (len > PageGetFreeSpace(pageHeader))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		buffer = ReleaseAndReadBuffer(buffer, relation, P_NEW);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		pageHeader = (Page) BufferGetPage(buffer);
		PageInit(pageHeader, BufferGetPageSize(buffer), 0);

		if (len > PageGetFreeSpace(pageHeader))
		{
			/*
			 * BUG: by elog'ing here, we leave the new buffer locked and not
			 * marked dirty, which may result in an invalid page header
			 * being left on disk.  But we should not get here given the
			 * test at the top of the routine, and the whole deal should
			 * go away when we implement tuple splitting anyway...
			 */
			elog(ERROR, "Tuple is too big: size %d", len);
		}
	}

	if (!relation->rd_myxactonly)
		UnlockPage(relation, 0, ExclusiveLock);

	offnum = PageAddItem((Page) pageHeader, (Item) tuple->t_data,
						 tuple->t_len, InvalidOffsetNumber, LP_USED);

	itemId = PageGetItemId((Page) pageHeader, offnum);
	item = PageGetItem((Page) pageHeader, itemId);

	lastblock = BufferGetBlockNumber(buffer);

	ItemPointerSet(&((HeapTupleHeader) item)->t_ctid, lastblock, offnum);

	/* return an accurate tuple self-pointer */
	ItemPointerSet(&tuple->t_self, lastblock, offnum);

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	WriteBuffer(buffer);

}
