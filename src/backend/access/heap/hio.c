/*-------------------------------------------------------------------------
 *
 * hio.c--
 *	  POSTGRES heap access method input/output code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Id: hio.c,v 1.16 1999/02/02 03:43:57 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <storage/bufpage.h>
#include <access/hio.h>
#include <access/heapam.h>
#include <storage/bufmgr.h>
#include <utils/memutils.h>

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
	Page			pageHeader;
	OffsetNumber	offnum;
	unsigned int	len;
	ItemId			itemId;
	Item			item;

	/* ----------------
	 *	increment access statistics
	 * ----------------
	 */
	IncrHeapAccessStat(local_RelationPutHeapTuple);
	IncrHeapAccessStat(global_RelationPutHeapTuple);

	pageHeader = (Page) BufferGetPage(buffer);
	len = (unsigned) DOUBLEALIGN(tuple->t_len); /* be conservative */
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
	WriteBuffer(buffer);
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

	if (!relation->rd_myxactonly)
		LockRelation(relation, ExtendLock);

	/*
	 * XXX This does an lseek - VERY expensive - but at the moment it is
	 * the only way to accurately determine how many blocks are in a
	 * relation.  A good optimization would be to get this to actually
	 * work properly.
	 */

	lastblock = RelationGetNumberOfBlocks(relation);

	if (lastblock == 0)
	{
		buffer = ReadBuffer(relation, lastblock);
		pageHeader = (Page) BufferGetPage(buffer);
		/*
		 * There was IF instead of ASSERT here ?!
		 */
		Assert(PageIsNew((PageHeader) pageHeader));
		buffer = ReleaseAndReadBuffer(buffer, relation, P_NEW);
		pageHeader = (Page) BufferGetPage(buffer);
		PageInit(pageHeader, BufferGetPageSize(buffer), 0);
	}
	else
		buffer = ReadBuffer(relation, lastblock - 1);

	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	pageHeader = (Page) BufferGetPage(buffer);
	len = (unsigned) DOUBLEALIGN(tuple->t_len); /* be conservative */

	/*
	 * Note that this is true if the above returned a bogus page, which it
	 * will do for a completely empty relation.
	 */

	if (len > PageGetFreeSpace(pageHeader))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		buffer = ReleaseAndReadBuffer(buffer, relation, P_NEW);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		pageHeader = (Page) BufferGetPage(buffer);
		PageInit(pageHeader, BufferGetPageSize(buffer), 0);

		if (len > PageGetFreeSpace(pageHeader))
			elog(ERROR, "Tuple is too big: size %d", len);
	}

	if (!relation->rd_myxactonly)
		UnlockRelation(relation, ExtendLock);

	offnum = PageAddItem((Page) pageHeader, (Item) tuple->t_data,
						 tuple->t_len, InvalidOffsetNumber, LP_USED);

	itemId = PageGetItemId((Page) pageHeader, offnum);
	item = PageGetItem((Page) pageHeader, itemId);

	lastblock = BufferGetBlockNumber(buffer);

	ItemPointerSet(&((HeapTupleHeader) item)->t_ctid, lastblock, offnum);

	/* return an accurate tuple */
	ItemPointerSet(&tuple->t_self, lastblock, offnum);

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	WriteBuffer(buffer);

}
