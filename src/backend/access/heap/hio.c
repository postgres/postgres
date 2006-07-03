/*-------------------------------------------------------------------------
 *
 * hio.c
 *	  POSTGRES heap access method input/output code.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/heap/hio.c,v 1.63 2006/07/03 22:45:37 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/hio.h"
#include "storage/freespace.h"


/*
 * RelationPutHeapTuple - place tuple at specified page
 *
 * !!! EREPORT(ERROR) IS DISALLOWED HERE !!!  Must PANIC on failure!!!
 *
 * Note - caller must hold BUFFER_LOCK_EXCLUSIVE on the buffer.
 */
void
RelationPutHeapTuple(Relation relation,
					 Buffer buffer,
					 HeapTuple tuple)
{
	Page		pageHeader;
	OffsetNumber offnum;
	ItemId		itemId;
	Item		item;

	/* Add the tuple to the page */
	pageHeader = BufferGetPage(buffer);

	offnum = PageAddItem(pageHeader, (Item) tuple->t_data,
						 tuple->t_len, InvalidOffsetNumber, LP_USED);

	if (offnum == InvalidOffsetNumber)
		elog(PANIC, "failed to add tuple to page");

	/* Update tuple->t_self to the actual position where it was stored */
	ItemPointerSet(&(tuple->t_self), BufferGetBlockNumber(buffer), offnum);

	/* Insert the correct position into CTID of the stored tuple, too */
	itemId = PageGetItemId(pageHeader, offnum);
	item = PageGetItem(pageHeader, itemId);
	((HeapTupleHeader) item)->t_ctid = tuple->t_self;
}

/*
 * RelationGetBufferForTuple
 *
 *	Returns pinned and exclusive-locked buffer of a page in given relation
 *	with free space >= given len.
 *
 *	If otherBuffer is not InvalidBuffer, then it references a previously
 *	pinned buffer of another page in the same relation; on return, this
 *	buffer will also be exclusive-locked.  (This case is used by heap_update;
 *	the otherBuffer contains the tuple being updated.)
 *
 *	The reason for passing otherBuffer is that if two backends are doing
 *	concurrent heap_update operations, a deadlock could occur if they try
 *	to lock the same two buffers in opposite orders.  To ensure that this
 *	can't happen, we impose the rule that buffers of a relation must be
 *	locked in increasing page number order.  This is most conveniently done
 *	by having RelationGetBufferForTuple lock them both, with suitable care
 *	for ordering.
 *
 *	NOTE: it is unlikely, but not quite impossible, for otherBuffer to be the
 *	same buffer we select for insertion of the new tuple (this could only
 *	happen if space is freed in that page after heap_update finds there's not
 *	enough there).	In that case, the page will be pinned and locked only once.
 *
 *	If use_fsm is true (the normal case), we use FSM to help us find free
 *	space.	If use_fsm is false, we always append a new empty page to the
 *	end of the relation if the tuple won't fit on the current target page.
 *	This can save some cycles when we know the relation is new and doesn't
 *	contain useful amounts of free space.
 *
 *	The use_fsm = false case is also useful for non-WAL-logged additions to a
 *	relation, if the caller holds exclusive lock and is careful to invalidate
 *	relation->rd_targblock before the first insertion --- that ensures that
 *	all insertions will occur into newly added pages and not be intermixed
 *	with tuples from other transactions.  That way, a crash can't risk losing
 *	any committed data of other transactions.  (See heap_insert's comments
 *	for additional constraints needed for safe usage of this behavior.)
 *
 *	We always try to avoid filling existing pages further than the fillfactor.
 *	This is OK since this routine is not consulted when updating a tuple and
 *	keeping it on the same page, which is the scenario fillfactor is meant
 *	to reserve space for.
 *
 *	ereport(ERROR) is allowed here, so this routine *must* be called
 *	before any (unlogged) changes are made in buffer pool.
 */
Buffer
RelationGetBufferForTuple(Relation relation, Size len,
						  Buffer otherBuffer, bool use_fsm)
{
	Buffer		buffer = InvalidBuffer;
	Page		pageHeader;
	Size		pageFreeSpace,
				saveFreeSpace;
	BlockNumber targetBlock,
				otherBlock;
	bool		needLock;

	len = MAXALIGN(len);		/* be conservative */

	/*
	 * If we're gonna fail for oversize tuple, do it right away
	 */
	if (len > MaxTupleSize)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("row is too big: size %lu, maximum size %lu",
						(unsigned long) len,
						(unsigned long) MaxTupleSize)));

	/* Compute desired extra freespace due to fillfactor option */
	saveFreeSpace = RelationGetTargetPageFreeSpace(relation,
												   HEAP_DEFAULT_FILLFACTOR);

	if (otherBuffer != InvalidBuffer)
		otherBlock = BufferGetBlockNumber(otherBuffer);
	else
		otherBlock = InvalidBlockNumber;		/* just to keep compiler quiet */

	/*
	 * We first try to put the tuple on the same page we last inserted a tuple
	 * on, as cached in the relcache entry.  If that doesn't work, we ask the
	 * shared Free Space Map to locate a suitable page.  Since the FSM's info
	 * might be out of date, we have to be prepared to loop around and retry
	 * multiple times.	(To insure this isn't an infinite loop, we must update
	 * the FSM with the correct amount of free space on each page that proves
	 * not to be suitable.)  If the FSM has no record of a page with enough
	 * free space, we give up and extend the relation.
	 *
	 * When use_fsm is false, we either put the tuple onto the existing target
	 * page or extend the relation.
	 */
	if (len + saveFreeSpace <= MaxTupleSize)
		targetBlock = relation->rd_targblock;
	else
	{
		/* can't fit, don't screw up FSM request tracking by trying */
		targetBlock = InvalidBlockNumber;
		use_fsm = false;
	}

	if (targetBlock == InvalidBlockNumber && use_fsm)
	{
		/*
		 * We have no cached target page, so ask the FSM for an initial
		 * target.
		 */
		targetBlock = GetPageWithFreeSpace(&relation->rd_node,
										   len + saveFreeSpace);

		/*
		 * If the FSM knows nothing of the rel, try the last page before we
		 * give up and extend.	This avoids one-tuple-per-page syndrome during
		 * bootstrapping or in a recently-started system.
		 */
		if (targetBlock == InvalidBlockNumber)
		{
			BlockNumber nblocks = RelationGetNumberOfBlocks(relation);

			if (nblocks > 0)
				targetBlock = nblocks - 1;
		}
	}

	while (targetBlock != InvalidBlockNumber)
	{
		/*
		 * Read and exclusive-lock the target block, as well as the other
		 * block if one was given, taking suitable care with lock ordering and
		 * the possibility they are the same block.
		 */
		if (otherBuffer == InvalidBuffer)
		{
			/* easy case */
			buffer = ReadBuffer(relation, targetBlock);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		}
		else if (otherBlock == targetBlock)
		{
			/* also easy case */
			buffer = otherBuffer;
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		}
		else if (otherBlock < targetBlock)
		{
			/* lock other buffer first */
			buffer = ReadBuffer(relation, targetBlock);
			LockBuffer(otherBuffer, BUFFER_LOCK_EXCLUSIVE);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		}
		else
		{
			/* lock target buffer first */
			buffer = ReadBuffer(relation, targetBlock);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
			LockBuffer(otherBuffer, BUFFER_LOCK_EXCLUSIVE);
		}

		/*
		 * Now we can check to see if there's enough free space here. If so,
		 * we're done.
		 */
		pageHeader = (Page) BufferGetPage(buffer);
		pageFreeSpace = PageGetFreeSpace(pageHeader);
		if (len + saveFreeSpace <= pageFreeSpace)
		{
			/* use this page as future insert target, too */
			relation->rd_targblock = targetBlock;
			return buffer;
		}

		/*
		 * Not enough space, so we must give up our page locks and pin (if
		 * any) and prepare to look elsewhere.	We don't care which order we
		 * unlock the two buffers in, so this can be slightly simpler than the
		 * code above.
		 */
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		if (otherBuffer == InvalidBuffer)
			ReleaseBuffer(buffer);
		else if (otherBlock != targetBlock)
		{
			LockBuffer(otherBuffer, BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(buffer);
		}

		/* Without FSM, always fall out of the loop and extend */
		if (!use_fsm)
			break;

		/*
		 * Update FSM as to condition of this page, and ask for another page
		 * to try.
		 */
		targetBlock = RecordAndGetPageWithFreeSpace(&relation->rd_node,
													targetBlock,
													pageFreeSpace,
													len + saveFreeSpace);
	}

	/*
	 * Have to extend the relation.
	 *
	 * We have to use a lock to ensure no one else is extending the rel at the
	 * same time, else we will both try to initialize the same new page.  We
	 * can skip locking for new or temp relations, however, since no one else
	 * could be accessing them.
	 */
	needLock = !RELATION_IS_LOCAL(relation);

	if (needLock)
		LockRelationForExtension(relation, ExclusiveLock);

	/*
	 * XXX This does an lseek - rather expensive - but at the moment it is the
	 * only way to accurately determine how many blocks are in a relation.	Is
	 * it worth keeping an accurate file length in shared memory someplace,
	 * rather than relying on the kernel to do it for us?
	 */
	buffer = ReadBuffer(relation, P_NEW);

	/*
	 * We can be certain that locking the otherBuffer first is OK, since it
	 * must have a lower page number.
	 */
	if (otherBuffer != InvalidBuffer)
		LockBuffer(otherBuffer, BUFFER_LOCK_EXCLUSIVE);

	/*
	 * Now acquire lock on the new page.
	 */
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	/*
	 * Release the file-extension lock; it's now OK for someone else to extend
	 * the relation some more.	Note that we cannot release this lock before
	 * we have buffer lock on the new page, or we risk a race condition
	 * against vacuumlazy.c --- see comments therein.
	 */
	if (needLock)
		UnlockRelationForExtension(relation, ExclusiveLock);

	/*
	 * We need to initialize the empty new page.  Double-check that it really
	 * is empty (this should never happen, but if it does we don't want to
	 * risk wiping out valid data).
	 */
	pageHeader = (Page) BufferGetPage(buffer);

	if (!PageIsNew((PageHeader) pageHeader))
		elog(ERROR, "page %u of relation \"%s\" should be empty but is not",
			 BufferGetBlockNumber(buffer),
			 RelationGetRelationName(relation));

	PageInit(pageHeader, BufferGetPageSize(buffer), 0);

	if (len > PageGetFreeSpace(pageHeader))
	{
		/* We should not get here given the test at the top */
		elog(PANIC, "tuple is too big: size %lu", (unsigned long) len);
	}

	/*
	 * Remember the new page as our target for future insertions.
	 *
	 * XXX should we enter the new page into the free space map immediately,
	 * or just keep it for this backend's exclusive use in the short run
	 * (until VACUUM sees it)?	Seems to depend on whether you expect the
	 * current backend to make more insertions or not, which is probably a
	 * good bet most of the time.  So for now, don't add it to FSM yet.
	 */
	relation->rd_targblock = BufferGetBlockNumber(buffer);

	return buffer;
}
