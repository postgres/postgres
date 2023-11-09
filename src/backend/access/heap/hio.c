/*-------------------------------------------------------------------------
 *
 * hio.c
 *	  POSTGRES heap access method input/output code.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/hio.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/hio.h"
#include "access/htup_details.h"
#include "access/visibilitymap.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"


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
					 HeapTuple tuple,
					 bool token)
{
	Page		pageHeader;
	OffsetNumber offnum;

	/*
	 * A tuple that's being inserted speculatively should already have its
	 * token set.
	 */
	Assert(!token || HeapTupleHeaderIsSpeculative(tuple->t_data));

	/*
	 * Do not allow tuples with invalid combinations of hint bits to be placed
	 * on a page.  This combination is detected as corruption by the
	 * contrib/amcheck logic, so if you disable this assertion, make
	 * corresponding changes there.
	 */
	Assert(!((tuple->t_data->t_infomask & HEAP_XMAX_COMMITTED) &&
			 (tuple->t_data->t_infomask & HEAP_XMAX_IS_MULTI)));

	/* Add the tuple to the page */
	pageHeader = BufferGetPage(buffer);

	offnum = PageAddItem(pageHeader, (Item) tuple->t_data,
						 tuple->t_len, InvalidOffsetNumber, false, true);

	if (offnum == InvalidOffsetNumber)
		elog(PANIC, "failed to add tuple to page");

	/* Update tuple->t_self to the actual position where it was stored */
	ItemPointerSet(&(tuple->t_self), BufferGetBlockNumber(buffer), offnum);

	/*
	 * Insert the correct position into CTID of the stored tuple, too (unless
	 * this is a speculative insertion, in which case the token is held in
	 * CTID field instead)
	 */
	if (!token)
	{
		ItemId		itemId = PageGetItemId(pageHeader, offnum);
		HeapTupleHeader item = (HeapTupleHeader) PageGetItem(pageHeader, itemId);

		item->t_ctid = tuple->t_self;
	}
}

/*
 * Read in a buffer in mode, using bulk-insert strategy if bistate isn't NULL.
 */
static Buffer
ReadBufferBI(Relation relation, BlockNumber targetBlock,
			 ReadBufferMode mode, BulkInsertState bistate)
{
	Buffer		buffer;

	/* If not bulk-insert, exactly like ReadBuffer */
	if (!bistate)
		return ReadBufferExtended(relation, MAIN_FORKNUM, targetBlock,
								  mode, NULL);

	/* If we have the desired block already pinned, re-pin and return it */
	if (bistate->current_buf != InvalidBuffer)
	{
		if (BufferGetBlockNumber(bistate->current_buf) == targetBlock)
		{
			/*
			 * Currently the LOCK variants are only used for extending
			 * relation, which should never reach this branch.
			 */
			Assert(mode != RBM_ZERO_AND_LOCK &&
				   mode != RBM_ZERO_AND_CLEANUP_LOCK);

			IncrBufferRefCount(bistate->current_buf);
			return bistate->current_buf;
		}
		/* ... else drop the old buffer */
		ReleaseBuffer(bistate->current_buf);
		bistate->current_buf = InvalidBuffer;
	}

	/* Perform a read using the buffer strategy */
	buffer = ReadBufferExtended(relation, MAIN_FORKNUM, targetBlock,
								mode, bistate->strategy);

	/* Save the selected block as target for future inserts */
	IncrBufferRefCount(buffer);
	bistate->current_buf = buffer;

	return buffer;
}

/*
 * For each heap page which is all-visible, acquire a pin on the appropriate
 * visibility map page, if we haven't already got one.
 *
 * To avoid complexity in the callers, either buffer1 or buffer2 may be
 * InvalidBuffer if only one buffer is involved. For the same reason, block2
 * may be smaller than block1.
 *
 * Returns whether buffer locks were temporarily released.
 */
static bool
GetVisibilityMapPins(Relation relation, Buffer buffer1, Buffer buffer2,
					 BlockNumber block1, BlockNumber block2,
					 Buffer *vmbuffer1, Buffer *vmbuffer2)
{
	bool		need_to_pin_buffer1;
	bool		need_to_pin_buffer2;
	bool		released_locks = false;

	/*
	 * Swap buffers around to handle case of a single block/buffer, and to
	 * handle if lock ordering rules require to lock block2 first.
	 */
	if (!BufferIsValid(buffer1) ||
		(BufferIsValid(buffer2) && block1 > block2))
	{
		Buffer		tmpbuf = buffer1;
		Buffer	   *tmpvmbuf = vmbuffer1;
		BlockNumber tmpblock = block1;

		buffer1 = buffer2;
		vmbuffer1 = vmbuffer2;
		block1 = block2;

		buffer2 = tmpbuf;
		vmbuffer2 = tmpvmbuf;
		block2 = tmpblock;
	}

	Assert(BufferIsValid(buffer1));
	Assert(buffer2 == InvalidBuffer || block1 <= block2);

	while (1)
	{
		/* Figure out which pins we need but don't have. */
		need_to_pin_buffer1 = PageIsAllVisible(BufferGetPage(buffer1))
			&& !visibilitymap_pin_ok(block1, *vmbuffer1);
		need_to_pin_buffer2 = buffer2 != InvalidBuffer
			&& PageIsAllVisible(BufferGetPage(buffer2))
			&& !visibilitymap_pin_ok(block2, *vmbuffer2);
		if (!need_to_pin_buffer1 && !need_to_pin_buffer2)
			break;

		/* We must unlock both buffers before doing any I/O. */
		released_locks = true;
		LockBuffer(buffer1, BUFFER_LOCK_UNLOCK);
		if (buffer2 != InvalidBuffer && buffer2 != buffer1)
			LockBuffer(buffer2, BUFFER_LOCK_UNLOCK);

		/* Get pins. */
		if (need_to_pin_buffer1)
			visibilitymap_pin(relation, block1, vmbuffer1);
		if (need_to_pin_buffer2)
			visibilitymap_pin(relation, block2, vmbuffer2);

		/* Relock buffers. */
		LockBuffer(buffer1, BUFFER_LOCK_EXCLUSIVE);
		if (buffer2 != InvalidBuffer && buffer2 != buffer1)
			LockBuffer(buffer2, BUFFER_LOCK_EXCLUSIVE);

		/*
		 * If there are two buffers involved and we pinned just one of them,
		 * it's possible that the second one became all-visible while we were
		 * busy pinning the first one.  If it looks like that's a possible
		 * scenario, we'll need to make a second pass through this loop.
		 */
		if (buffer2 == InvalidBuffer || buffer1 == buffer2
			|| (need_to_pin_buffer1 && need_to_pin_buffer2))
			break;
	}

	return released_locks;
}

/*
 * Extend the relation. By multiple pages, if beneficial.
 *
 * If the caller needs multiple pages (num_pages > 1), we always try to extend
 * by at least that much.
 *
 * If there is contention on the extension lock, we don't just extend "for
 * ourselves", but we try to help others. We can do so by adding empty pages
 * into the FSM. Typically there is no contention when we can't use the FSM.
 *
 * We do have to limit the number of pages to extend by to some value, as the
 * buffers for all the extended pages need to, temporarily, be pinned. For now
 * we define MAX_BUFFERS_TO_EXTEND_BY to be 64 buffers, it's hard to see
 * benefits with higher numbers. This partially is because copyfrom.c's
 * MAX_BUFFERED_TUPLES / MAX_BUFFERED_BYTES prevents larger multi_inserts.
 *
 * Returns a buffer for a newly extended block. If possible, the buffer is
 * returned exclusively locked. *did_unlock is set to true if the lock had to
 * be released, false otherwise.
 *
 *
 * XXX: It would likely be beneficial for some workloads to extend more
 * aggressively, e.g. using a heuristic based on the relation size.
 */
static Buffer
RelationAddBlocks(Relation relation, BulkInsertState bistate,
				  int num_pages, bool use_fsm, bool *did_unlock)
{
#define MAX_BUFFERS_TO_EXTEND_BY 64
	Buffer		victim_buffers[MAX_BUFFERS_TO_EXTEND_BY];
	BlockNumber first_block = InvalidBlockNumber;
	BlockNumber last_block = InvalidBlockNumber;
	uint32		extend_by_pages;
	uint32		not_in_fsm_pages;
	Buffer		buffer;
	Page		page;

	/*
	 * Determine by how many pages to try to extend by.
	 */
	if (bistate == NULL && !use_fsm)
	{
		/*
		 * If we have neither bistate, nor can use the FSM, we can't bulk
		 * extend - there'd be no way to find the additional pages.
		 */
		extend_by_pages = 1;
	}
	else
	{
		uint32		waitcount;

		/*
		 * Try to extend at least by the number of pages the caller needs. We
		 * can remember the additional pages (either via FSM or bistate).
		 */
		extend_by_pages = num_pages;

		if (!RELATION_IS_LOCAL(relation))
			waitcount = RelationExtensionLockWaiterCount(relation);
		else
			waitcount = 0;

		/*
		 * Multiply the number of pages to extend by the number of waiters. Do
		 * this even if we're not using the FSM, as it still relieves
		 * contention, by deferring the next time this backend needs to
		 * extend. In that case the extended pages will be found via
		 * bistate->next_free.
		 */
		extend_by_pages += extend_by_pages * waitcount;

		/* ---
		 * If we previously extended using the same bistate, it's very likely
		 * we'll extend some more. Try to extend by as many pages as
		 * before. This can be important for performance for several reasons,
		 * including:
		 *
		 * - It prevents mdzeroextend() switching between extending the
		 *   relation in different ways, which is inefficient for some
		 *   filesystems.
		 *
		 * - Contention is often intermittent. Even if we currently don't see
		 *   other waiters (see above), extending by larger amounts can
		 *   prevent future contention.
		 * ---
		 */
		if (bistate)
			extend_by_pages = Max(extend_by_pages, bistate->already_extended_by);

		/*
		 * Can't extend by more than MAX_BUFFERS_TO_EXTEND_BY, we need to pin
		 * them all concurrently.
		 */
		extend_by_pages = Min(extend_by_pages, MAX_BUFFERS_TO_EXTEND_BY);
	}

	/*
	 * How many of the extended pages should be entered into the FSM?
	 *
	 * If we have a bistate, only enter pages that we don't need ourselves
	 * into the FSM.  Otherwise every other backend will immediately try to
	 * use the pages this backend needs for itself, causing unnecessary
	 * contention.  If we don't have a bistate, we can't avoid the FSM.
	 *
	 * Never enter the page returned into the FSM, we'll immediately use it.
	 */
	if (num_pages > 1 && bistate == NULL)
		not_in_fsm_pages = 1;
	else
		not_in_fsm_pages = num_pages;

	/* prepare to put another buffer into the bistate */
	if (bistate && bistate->current_buf != InvalidBuffer)
	{
		ReleaseBuffer(bistate->current_buf);
		bistate->current_buf = InvalidBuffer;
	}

	/*
	 * Extend the relation. We ask for the first returned page to be locked,
	 * so that we are sure that nobody has inserted into the page
	 * concurrently.
	 *
	 * With the current MAX_BUFFERS_TO_EXTEND_BY there's no danger of
	 * [auto]vacuum trying to truncate later pages as REL_TRUNCATE_MINIMUM is
	 * way larger.
	 */
	first_block = ExtendBufferedRelBy(BMR_REL(relation), MAIN_FORKNUM,
									  bistate ? bistate->strategy : NULL,
									  EB_LOCK_FIRST,
									  extend_by_pages,
									  victim_buffers,
									  &extend_by_pages);
	buffer = victim_buffers[0]; /* the buffer the function will return */
	last_block = first_block + (extend_by_pages - 1);
	Assert(first_block == BufferGetBlockNumber(buffer));

	/*
	 * Relation is now extended. Initialize the page. We do this here, before
	 * potentially releasing the lock on the page, because it allows us to
	 * double check that the page contents are empty (this should never
	 * happen, but if it does we don't want to risk wiping out valid data).
	 */
	page = BufferGetPage(buffer);
	if (!PageIsNew(page))
		elog(ERROR, "page %u of relation \"%s\" should be empty but is not",
			 first_block,
			 RelationGetRelationName(relation));

	PageInit(page, BufferGetPageSize(buffer), 0);
	MarkBufferDirty(buffer);

	/*
	 * If we decided to put pages into the FSM, release the buffer lock (but
	 * not pin), we don't want to do IO while holding a buffer lock. This will
	 * necessitate a bit more extensive checking in our caller.
	 */
	if (use_fsm && not_in_fsm_pages < extend_by_pages)
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		*did_unlock = true;
	}
	else
		*did_unlock = false;

	/*
	 * Relation is now extended. Release pins on all buffers, except for the
	 * first (which we'll return).  If we decided to put pages into the FSM,
	 * we can do that as part of the same loop.
	 */
	for (uint32 i = 1; i < extend_by_pages; i++)
	{
		BlockNumber curBlock = first_block + i;

		Assert(curBlock == BufferGetBlockNumber(victim_buffers[i]));
		Assert(BlockNumberIsValid(curBlock));

		ReleaseBuffer(victim_buffers[i]);

		if (use_fsm && i >= not_in_fsm_pages)
		{
			Size		freespace = BufferGetPageSize(victim_buffers[i]) -
				SizeOfPageHeaderData;

			RecordPageWithFreeSpace(relation, curBlock, freespace);
		}
	}

	if (use_fsm && not_in_fsm_pages < extend_by_pages)
	{
		BlockNumber first_fsm_block = first_block + not_in_fsm_pages;

		FreeSpaceMapVacuumRange(relation, first_fsm_block, last_block);
	}

	if (bistate)
	{
		/*
		 * Remember the additional pages we extended by, so we later can use
		 * them without looking into the FSM.
		 */
		if (extend_by_pages > 1)
		{
			bistate->next_free = first_block + 1;
			bistate->last_free = last_block;
		}
		else
		{
			bistate->next_free = InvalidBlockNumber;
			bistate->last_free = InvalidBlockNumber;
		}

		/* maintain bistate->current_buf */
		IncrBufferRefCount(buffer);
		bistate->current_buf = buffer;
		bistate->already_extended_by += extend_by_pages;
	}

	return buffer;
#undef MAX_BUFFERS_TO_EXTEND_BY
}

/*
 * RelationGetBufferForTuple
 *
 *	Returns pinned and exclusive-locked buffer of a page in given relation
 *	with free space >= given len.
 *
 *	If num_pages is > 1, we will try to extend the relation by at least that
 *	many pages when we decide to extend the relation. This is more efficient
 *	for callers that know they will need multiple pages
 *	(e.g. heap_multi_insert()).
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
 *	enough there).  In that case, the page will be pinned and locked only once.
 *
 *	We also handle the possibility that the all-visible flag will need to be
 *	cleared on one or both pages.  If so, pin on the associated visibility map
 *	page must be acquired before acquiring buffer lock(s), to avoid possibly
 *	doing I/O while holding buffer locks.  The pins are passed back to the
 *	caller using the input-output arguments vmbuffer and vmbuffer_other.
 *	Note that in some cases the caller might have already acquired such pins,
 *	which is indicated by these arguments not being InvalidBuffer on entry.
 *
 *	We normally use FSM to help us find free space.  However,
 *	if HEAP_INSERT_SKIP_FSM is specified, we just append a new empty page to
 *	the end of the relation if the tuple won't fit on the current target page.
 *	This can save some cycles when we know the relation is new and doesn't
 *	contain useful amounts of free space.
 *
 *	HEAP_INSERT_SKIP_FSM is also useful for non-WAL-logged additions to a
 *	relation, if the caller holds exclusive lock and is careful to invalidate
 *	relation's smgr_targblock before the first insertion --- that ensures that
 *	all insertions will occur into newly added pages and not be intermixed
 *	with tuples from other transactions.  That way, a crash can't risk losing
 *	any committed data of other transactions.  (See heap_insert's comments
 *	for additional constraints needed for safe usage of this behavior.)
 *
 *	The caller can also provide a BulkInsertState object to optimize many
 *	insertions into the same relation.  This keeps a pin on the current
 *	insertion target page (to save pin/unpin cycles) and also passes a
 *	BULKWRITE buffer selection strategy object to the buffer manager.
 *	Passing NULL for bistate selects the default behavior.
 *
 *	We don't fill existing pages further than the fillfactor, except for large
 *	tuples in nearly-empty pages.  This is OK since this routine is not
 *	consulted when updating a tuple and keeping it on the same page, which is
 *	the scenario fillfactor is meant to reserve space for.
 *
 *	ereport(ERROR) is allowed here, so this routine *must* be called
 *	before any (unlogged) changes are made in buffer pool.
 */
Buffer
RelationGetBufferForTuple(Relation relation, Size len,
						  Buffer otherBuffer, int options,
						  BulkInsertState bistate,
						  Buffer *vmbuffer, Buffer *vmbuffer_other,
						  int num_pages)
{
	bool		use_fsm = !(options & HEAP_INSERT_SKIP_FSM);
	Buffer		buffer = InvalidBuffer;
	Page		page;
	Size		nearlyEmptyFreeSpace,
				pageFreeSpace = 0,
				saveFreeSpace = 0,
				targetFreeSpace = 0;
	BlockNumber targetBlock,
				otherBlock;
	bool		unlockedTargetBuffer;
	bool		recheckVmPins;

	len = MAXALIGN(len);		/* be conservative */

	/* if the caller doesn't know by how many pages to extend, extend by 1 */
	if (num_pages <= 0)
		num_pages = 1;

	/* Bulk insert is not supported for updates, only inserts. */
	Assert(otherBuffer == InvalidBuffer || !bistate);

	/*
	 * If we're gonna fail for oversize tuple, do it right away
	 */
	if (len > MaxHeapTupleSize)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("row is too big: size %zu, maximum size %zu",
						len, MaxHeapTupleSize)));

	/* Compute desired extra freespace due to fillfactor option */
	saveFreeSpace = RelationGetTargetPageFreeSpace(relation,
												   HEAP_DEFAULT_FILLFACTOR);

	/*
	 * Since pages without tuples can still have line pointers, we consider
	 * pages "empty" when the unavailable space is slight.  This threshold is
	 * somewhat arbitrary, but it should prevent most unnecessary relation
	 * extensions while inserting large tuples into low-fillfactor tables.
	 */
	nearlyEmptyFreeSpace = MaxHeapTupleSize -
		(MaxHeapTuplesPerPage / 8 * sizeof(ItemIdData));
	if (len + saveFreeSpace > nearlyEmptyFreeSpace)
		targetFreeSpace = Max(len, nearlyEmptyFreeSpace);
	else
		targetFreeSpace = len + saveFreeSpace;

	if (otherBuffer != InvalidBuffer)
		otherBlock = BufferGetBlockNumber(otherBuffer);
	else
		otherBlock = InvalidBlockNumber;	/* just to keep compiler quiet */

	/*
	 * We first try to put the tuple on the same page we last inserted a tuple
	 * on, as cached in the BulkInsertState or relcache entry.  If that
	 * doesn't work, we ask the Free Space Map to locate a suitable page.
	 * Since the FSM's info might be out of date, we have to be prepared to
	 * loop around and retry multiple times. (To ensure this isn't an infinite
	 * loop, we must update the FSM with the correct amount of free space on
	 * each page that proves not to be suitable.)  If the FSM has no record of
	 * a page with enough free space, we give up and extend the relation.
	 *
	 * When use_fsm is false, we either put the tuple onto the existing target
	 * page or extend the relation.
	 */
	if (bistate && bistate->current_buf != InvalidBuffer)
		targetBlock = BufferGetBlockNumber(bistate->current_buf);
	else
		targetBlock = RelationGetTargetBlock(relation);

	if (targetBlock == InvalidBlockNumber && use_fsm)
	{
		/*
		 * We have no cached target page, so ask the FSM for an initial
		 * target.
		 */
		targetBlock = GetPageWithFreeSpace(relation, targetFreeSpace);
	}

	/*
	 * If the FSM knows nothing of the rel, try the last page before we give
	 * up and extend.  This avoids one-tuple-per-page syndrome during
	 * bootstrapping or in a recently-started system.
	 */
	if (targetBlock == InvalidBlockNumber)
	{
		BlockNumber nblocks = RelationGetNumberOfBlocks(relation);

		if (nblocks > 0)
			targetBlock = nblocks - 1;
	}

loop:
	while (targetBlock != InvalidBlockNumber)
	{
		/*
		 * Read and exclusive-lock the target block, as well as the other
		 * block if one was given, taking suitable care with lock ordering and
		 * the possibility they are the same block.
		 *
		 * If the page-level all-visible flag is set, caller will need to
		 * clear both that and the corresponding visibility map bit.  However,
		 * by the time we return, we'll have x-locked the buffer, and we don't
		 * want to do any I/O while in that state.  So we check the bit here
		 * before taking the lock, and pin the page if it appears necessary.
		 * Checking without the lock creates a risk of getting the wrong
		 * answer, so we'll have to recheck after acquiring the lock.
		 */
		if (otherBuffer == InvalidBuffer)
		{
			/* easy case */
			buffer = ReadBufferBI(relation, targetBlock, RBM_NORMAL, bistate);
			if (PageIsAllVisible(BufferGetPage(buffer)))
				visibilitymap_pin(relation, targetBlock, vmbuffer);

			/*
			 * If the page is empty, pin vmbuffer to set all_frozen bit later.
			 */
			if ((options & HEAP_INSERT_FROZEN) &&
				(PageGetMaxOffsetNumber(BufferGetPage(buffer)) == 0))
				visibilitymap_pin(relation, targetBlock, vmbuffer);

			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		}
		else if (otherBlock == targetBlock)
		{
			/* also easy case */
			buffer = otherBuffer;
			if (PageIsAllVisible(BufferGetPage(buffer)))
				visibilitymap_pin(relation, targetBlock, vmbuffer);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		}
		else if (otherBlock < targetBlock)
		{
			/* lock other buffer first */
			buffer = ReadBuffer(relation, targetBlock);
			if (PageIsAllVisible(BufferGetPage(buffer)))
				visibilitymap_pin(relation, targetBlock, vmbuffer);
			LockBuffer(otherBuffer, BUFFER_LOCK_EXCLUSIVE);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		}
		else
		{
			/* lock target buffer first */
			buffer = ReadBuffer(relation, targetBlock);
			if (PageIsAllVisible(BufferGetPage(buffer)))
				visibilitymap_pin(relation, targetBlock, vmbuffer);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
			LockBuffer(otherBuffer, BUFFER_LOCK_EXCLUSIVE);
		}

		/*
		 * We now have the target page (and the other buffer, if any) pinned
		 * and locked.  However, since our initial PageIsAllVisible checks
		 * were performed before acquiring the lock, the results might now be
		 * out of date, either for the selected victim buffer, or for the
		 * other buffer passed by the caller.  In that case, we'll need to
		 * give up our locks, go get the pin(s) we failed to get earlier, and
		 * re-lock.  That's pretty painful, but hopefully shouldn't happen
		 * often.
		 *
		 * Note that there's a small possibility that we didn't pin the page
		 * above but still have the correct page pinned anyway, either because
		 * we've already made a previous pass through this loop, or because
		 * caller passed us the right page anyway.
		 *
		 * Note also that it's possible that by the time we get the pin and
		 * retake the buffer locks, the visibility map bit will have been
		 * cleared by some other backend anyway.  In that case, we'll have
		 * done a bit of extra work for no gain, but there's no real harm
		 * done.
		 */
		GetVisibilityMapPins(relation, buffer, otherBuffer,
							 targetBlock, otherBlock, vmbuffer,
							 vmbuffer_other);

		/*
		 * Now we can check to see if there's enough free space here. If so,
		 * we're done.
		 */
		page = BufferGetPage(buffer);

		/*
		 * If necessary initialize page, it'll be used soon.  We could avoid
		 * dirtying the buffer here, and rely on the caller to do so whenever
		 * it puts a tuple onto the page, but there seems not much benefit in
		 * doing so.
		 */
		if (PageIsNew(page))
		{
			PageInit(page, BufferGetPageSize(buffer), 0);
			MarkBufferDirty(buffer);
		}

		pageFreeSpace = PageGetHeapFreeSpace(page);
		if (targetFreeSpace <= pageFreeSpace)
		{
			/* use this page as future insert target, too */
			RelationSetTargetBlock(relation, targetBlock);
			return buffer;
		}

		/*
		 * Not enough space, so we must give up our page locks and pin (if
		 * any) and prepare to look elsewhere.  We don't care which order we
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

		/* Is there an ongoing bulk extension? */
		if (bistate && bistate->next_free != InvalidBlockNumber)
		{
			Assert(bistate->next_free <= bistate->last_free);

			/*
			 * We bulk extended the relation before, and there are still some
			 * unused pages from that extension, so we don't need to look in
			 * the FSM for a new page. But do record the free space from the
			 * last page, somebody might insert narrower tuples later.
			 */
			if (use_fsm)
				RecordPageWithFreeSpace(relation, targetBlock, pageFreeSpace);

			targetBlock = bistate->next_free;
			if (bistate->next_free >= bistate->last_free)
			{
				bistate->next_free = InvalidBlockNumber;
				bistate->last_free = InvalidBlockNumber;
			}
			else
				bistate->next_free++;
		}
		else if (!use_fsm)
		{
			/* Without FSM, always fall out of the loop and extend */
			break;
		}
		else
		{
			/*
			 * Update FSM as to condition of this page, and ask for another
			 * page to try.
			 */
			targetBlock = RecordAndGetPageWithFreeSpace(relation,
														targetBlock,
														pageFreeSpace,
														targetFreeSpace);
		}
	}

	/* Have to extend the relation */
	buffer = RelationAddBlocks(relation, bistate, num_pages, use_fsm,
							   &unlockedTargetBuffer);

	targetBlock = BufferGetBlockNumber(buffer);
	page = BufferGetPage(buffer);

	/*
	 * The page is empty, pin vmbuffer to set all_frozen bit. We don't want to
	 * do IO while the buffer is locked, so we unlock the page first if IO is
	 * needed (necessitating checks below).
	 */
	if (options & HEAP_INSERT_FROZEN)
	{
		Assert(PageGetMaxOffsetNumber(page) == 0);

		if (!visibilitymap_pin_ok(targetBlock, *vmbuffer))
		{
			if (!unlockedTargetBuffer)
				LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			unlockedTargetBuffer = true;
			visibilitymap_pin(relation, targetBlock, vmbuffer);
		}
	}

	/*
	 * Reacquire locks if necessary.
	 *
	 * If the target buffer was unlocked above, or is unlocked while
	 * reacquiring the lock on otherBuffer below, it's unlikely, but possible,
	 * that another backend used space on this page. We check for that below,
	 * and retry if necessary.
	 */
	recheckVmPins = false;
	if (unlockedTargetBuffer)
	{
		/* released lock on target buffer above */
		if (otherBuffer != InvalidBuffer)
			LockBuffer(otherBuffer, BUFFER_LOCK_EXCLUSIVE);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		recheckVmPins = true;
	}
	else if (otherBuffer != InvalidBuffer)
	{
		/*
		 * We did not release the target buffer, and otherBuffer is valid,
		 * need to lock the other buffer. It's guaranteed to be of a lower
		 * page number than the new page.  To conform with the deadlock
		 * prevent rules, we ought to lock otherBuffer first, but that would
		 * give other backends a chance to put tuples on our page. To reduce
		 * the likelihood of that, attempt to lock the other buffer
		 * conditionally, that's very likely to work.
		 *
		 * Alternatively, we could acquire the lock on otherBuffer before
		 * extending the relation, but that'd require holding the lock while
		 * performing IO, which seems worse than an unlikely retry.
		 */
		Assert(otherBuffer != buffer);
		Assert(targetBlock > otherBlock);

		if (unlikely(!ConditionalLockBuffer(otherBuffer)))
		{
			unlockedTargetBuffer = true;
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			LockBuffer(otherBuffer, BUFFER_LOCK_EXCLUSIVE);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		}
		recheckVmPins = true;
	}

	/*
	 * If one of the buffers was unlocked (always the case if otherBuffer is
	 * valid), it's possible, although unlikely, that an all-visible flag
	 * became set.  We can use GetVisibilityMapPins to deal with that. It's
	 * possible that GetVisibilityMapPins() might need to temporarily release
	 * buffer locks, in which case we'll need to check if there's still enough
	 * space on the page below.
	 */
	if (recheckVmPins)
	{
		if (GetVisibilityMapPins(relation, otherBuffer, buffer,
								 otherBlock, targetBlock, vmbuffer_other,
								 vmbuffer))
			unlockedTargetBuffer = true;
	}

	/*
	 * If the target buffer was temporarily unlocked since the relation
	 * extension, it's possible, although unlikely, that all the space on the
	 * page was already used. If so, we just retry from the start.  If we
	 * didn't unlock, something has gone wrong if there's not enough space -
	 * the test at the top should have prevented reaching this case.
	 */
	pageFreeSpace = PageGetHeapFreeSpace(page);
	if (len > pageFreeSpace)
	{
		if (unlockedTargetBuffer)
		{
			if (otherBuffer != InvalidBuffer)
				LockBuffer(otherBuffer, BUFFER_LOCK_UNLOCK);
			UnlockReleaseBuffer(buffer);

			goto loop;
		}
		elog(PANIC, "tuple is too big: size %zu", len);
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
	RelationSetTargetBlock(relation, targetBlock);

	return buffer;
}
