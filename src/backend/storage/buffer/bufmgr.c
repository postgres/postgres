/*-------------------------------------------------------------------------
 *
 * bufmgr.c
 *	  buffer manager interface routines
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/buffer/bufmgr.c,v 1.173 2004/07/17 03:28:49 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * ReadBuffer() -- find or create a buffer holding the requested page,
 *		and pin it so that no one can destroy it while this process
 *		is using it.
 *
 * ReleaseBuffer() -- unpin the buffer
 *
 * WriteNoReleaseBuffer() -- mark the buffer contents as "dirty"
 *		but don't unpin.  The disk IO is delayed until buffer
 *		replacement.
 *
 * WriteBuffer() -- WriteNoReleaseBuffer() + ReleaseBuffer()
 *
 * BufferSync() -- flush all (or some) dirty buffers in the buffer pool.
 *
 * InitBufferPool() -- Init the buffer module.
 *
 * See other files:
 *		freelist.c -- chooses victim for buffer replacement
 *		buf_table.c -- manages the buffer lookup table
 */
#include "postgres.h"

#include <sys/file.h>
#include <unistd.h>

#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/proc.h"
#include "storage/smgr.h"
#include "utils/relcache.h"
#include "utils/resowner.h"
#include "pgstat.h"


#define BufferGetLSN(bufHdr)	\
	(*((XLogRecPtr*) MAKE_PTR((bufHdr)->data)))


/* GUC variable */
bool		zero_damaged_pages = false;

#ifdef NOT_USED
bool			ShowPinTrace = false;
#endif

long		NDirectFileRead;	/* some I/O's are direct file access.
								 * bypass bufmgr */
long		NDirectFileWrite;	/* e.g., I/O in psort and hashjoin. */


static void PinBuffer(BufferDesc *buf, bool fixOwner);
static void UnpinBuffer(BufferDesc *buf, bool fixOwner);
static void WaitIO(BufferDesc *buf);
static void StartBufferIO(BufferDesc *buf, bool forInput);
static void TerminateBufferIO(BufferDesc *buf, int err_flag);
static void ContinueBufferIO(BufferDesc *buf, bool forInput);
static void buffer_write_error_callback(void *arg);
static Buffer ReadBufferInternal(Relation reln, BlockNumber blockNum,
				   bool bufferLockHeld);
static BufferDesc *BufferAlloc(Relation reln, BlockNumber blockNum,
			bool *foundPtr);
static void FlushBuffer(BufferDesc *buf, SMgrRelation reln);
static void write_buffer(Buffer buffer, bool unpin);


/*
 * ReadBuffer -- returns a buffer containing the requested
 *		block of the requested relation.  If the blknum
 *		requested is P_NEW, extend the relation file and
 *		allocate a new block.  (Caller is responsible for
 *		ensuring that only one backend tries to extend a
 *		relation at the same time!)
 *
 * Returns: the buffer number for the buffer containing
 *		the block read.  The returned buffer has been pinned.
 *		Does not return on error --- elog's instead.
 *
 * Assume when this function is called, that reln has been
 *		opened already.
 */
Buffer
ReadBuffer(Relation reln, BlockNumber blockNum)
{
	ResourceOwnerEnlargeBuffers(CurrentResourceOwner);
	return ReadBufferInternal(reln, blockNum, false);
}

/*
 * ReadBufferInternal -- internal version of ReadBuffer with more options
 *
 * bufferLockHeld: if true, caller already acquired the bufmgr lock.
 * (This is assumed never to be true if dealing with a local buffer!)
 *
 * The caller must have done ResourceOwnerEnlargeBuffers(CurrentResourceOwner)
 */
static Buffer
ReadBufferInternal(Relation reln, BlockNumber blockNum,
				   bool bufferLockHeld)
{
	BufferDesc *bufHdr;
	bool		found;
	bool		isExtend;
	bool		isLocalBuf;

	isExtend = (blockNum == P_NEW);
	isLocalBuf = reln->rd_istemp;

	/* Open it at the smgr level if not already done */
	if (reln->rd_smgr == NULL)
		reln->rd_smgr = smgropen(reln->rd_node);

	/* Substitute proper block number if caller asked for P_NEW */
	if (isExtend)
		blockNum = smgrnblocks(reln->rd_smgr);

	if (isLocalBuf)
	{
		ReadLocalBufferCount++;
		pgstat_count_buffer_read(&reln->pgstat_info, reln);
		bufHdr = LocalBufferAlloc(reln, blockNum, &found);
		if (found)
			LocalBufferHitCount++;
	}
	else
	{
		ReadBufferCount++;
		pgstat_count_buffer_read(&reln->pgstat_info, reln);
		/*
		 * lookup the buffer.  IO_IN_PROGRESS is set if the requested
		 * block is not currently in memory.
		 */
		if (!bufferLockHeld)
			LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);
		bufHdr = BufferAlloc(reln, blockNum, &found);
		if (found)
			BufferHitCount++;
	}

	/* At this point we do NOT hold the bufmgr lock. */

	/* if it was already in the buffer pool, we're done */
	if (found)
	{
		/* Just need to update stats before we exit */
		pgstat_count_buffer_hit(&reln->pgstat_info, reln);

		if (VacuumCostActive)
			VacuumCostBalance += VacuumCostPageHit;

		return BufferDescriptorGetBuffer(bufHdr);
	}

	/*
	 * if we have gotten to this point, we have allocated a buffer for the
	 * page but its contents are not yet valid.  IO_IN_PROGRESS is set for
	 * it, if it's a shared buffer.
	 *
	 * Note: if smgrextend fails, we will end up with a buffer that is
	 * allocated but not marked BM_VALID.  P_NEW will still select the same
	 * block number (because the relation didn't get any longer on disk)
	 * and so future attempts to extend the relation will find the same
	 * buffer (if it's not been recycled) but come right back here to try
	 * smgrextend again.
	 */
	Assert(!(bufHdr->flags & BM_VALID));

	if (isExtend)
	{
		/* new buffers are zero-filled */
		MemSet((char *) MAKE_PTR(bufHdr->data), 0, BLCKSZ);
		smgrextend(reln->rd_smgr, blockNum, (char *) MAKE_PTR(bufHdr->data),
				   reln->rd_istemp);
	}
	else
	{
		smgrread(reln->rd_smgr, blockNum, (char *) MAKE_PTR(bufHdr->data));
		/* check for garbage data */
		if (!PageHeaderIsValid((PageHeader) MAKE_PTR(bufHdr->data)))
		{
			/*
			 * During WAL recovery, the first access to any data page should
			 * overwrite the whole page from the WAL; so a clobbered page
			 * header is not reason to fail.  Hence, when InRecovery we may
			 * always act as though zero_damaged_pages is ON.
			 */
			if (zero_damaged_pages || InRecovery)
			{
				ereport(WARNING,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("invalid page header in block %u of relation \"%s\"; zeroing out page",
							  blockNum, RelationGetRelationName(reln))));
				MemSet((char *) MAKE_PTR(bufHdr->data), 0, BLCKSZ);
			}
			else
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
					  errmsg("invalid page header in block %u of relation \"%s\"",
							 blockNum, RelationGetRelationName(reln))));
		}
	}

	if (isLocalBuf)
	{
		/* Only need to adjust flags */
		bufHdr->flags |= BM_VALID;
	}
	else
	{
		/* lock buffer manager again to update IO IN PROGRESS */
		LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);

		/* IO Succeeded, so mark data valid */
		bufHdr->flags |= BM_VALID;

		/* If anyone was waiting for IO to complete, wake them up now */
		TerminateBufferIO(bufHdr, 0);

		LWLockRelease(BufMgrLock);
	}

	if (VacuumCostActive)
		VacuumCostBalance += VacuumCostPageMiss;

	return BufferDescriptorGetBuffer(bufHdr);
}

/*
 * BufferAlloc -- subroutine for ReadBuffer.  Handles lookup of a shared
 *		buffer.  If no buffer exists already, selects a replacement
 *		victim and evicts the old page, but does NOT read in new page.
 *
 * The returned buffer is pinned and is already marked as holding the
 * desired page.  If it already did have the desired page, *foundPtr is
 * set TRUE.  Otherwise, *foundPtr is set FALSE and the buffer is marked
 * as IO_IN_PROGRESS; ReadBuffer will now need to do I/O to fill it.
 *
 * *foundPtr is actually redundant with the buffer's BM_VALID flag, but
 * we keep it for simplicity in ReadBuffer.
 *
 * BufMgrLock must be held at entry.  When this routine returns,
 * the BufMgrLock is guaranteed NOT to be held.
 */
static BufferDesc *
BufferAlloc(Relation reln,
			BlockNumber blockNum,
			bool *foundPtr)
{
	BufferTag	newTag;			/* identity of requested block */
	BufferDesc *buf,
			   *buf2;
	int			cdb_found_index,
				cdb_replace_index;
	bool		inProgress;		/* did we already do StartBufferIO? */

	/* create a tag so we can lookup the buffer */
	INIT_BUFFERTAG(newTag, reln, blockNum);

	/* see if the block is in the buffer pool already */
	buf = StrategyBufferLookup(&newTag, false, &cdb_found_index);
	if (buf != NULL)
	{
		/*
		 * Found it.  Now, pin the buffer so no one can steal it from the
		 * buffer pool, and check to see if someone else is still reading
		 * data into the buffer.  (Formerly, we'd always block here if
		 * IO_IN_PROGRESS is set, but there's no need to wait when someone
		 * is writing rather than reading.)
		 */
		*foundPtr = TRUE;

		PinBuffer(buf, true);

		if (!(buf->flags & BM_VALID))
		{
			if (buf->flags & BM_IO_IN_PROGRESS)
			{
				/* someone else is reading it, wait for them */
				WaitIO(buf);
			}
			if (!(buf->flags & BM_VALID))
			{
				/*
				 * If we get here, previous attempts to read the buffer
				 * must have failed ... but we shall bravely try again.
				 */
				*foundPtr = FALSE;
				StartBufferIO(buf, true);
			}
		}

		LWLockRelease(BufMgrLock);

		return buf;
	}

	*foundPtr = FALSE;

	/*
	 * Didn't find it in the buffer pool.  We'll have to initialize a new
	 * buffer.	First, grab one from the free list.  If it's dirty, flush
	 * it to disk. Remember to unlock BufMgrLock while doing the IO.
	 */
	inProgress = FALSE;
	do
	{
		buf = StrategyGetBuffer(&cdb_replace_index);

		/* StrategyGetBuffer will elog if it can't find a free buffer */
		Assert(buf);

		/*
		 * There should be exactly one pin on the buffer after it is
		 * allocated -- ours.  If it had a pin it wouldn't have been on
		 * the free list.  No one else could have pinned it between
		 * StrategyGetBuffer and here because we have the BufMgrLock.
		 */
		Assert(buf->refcount == 0);
		buf->refcount = 1;
		PrivateRefCount[BufferDescriptorGetBuffer(buf) - 1] = 1;

		ResourceOwnerRememberBuffer(CurrentResourceOwner,
									BufferDescriptorGetBuffer(buf));

		if ((buf->flags & BM_VALID) &&
			(buf->flags & BM_DIRTY || buf->cntxDirty))
		{
			/*
			 * Set BM_IO_IN_PROGRESS to show the buffer is being written.
			 * It cannot already be set because the buffer would be pinned
			 * if someone were writing it.
			 *
			 * Note: it's okay to grab the io_in_progress lock while holding
			 * BufMgrLock.  All code paths that acquire this lock pin the
			 * buffer first; since no one had it pinned (it just came off the
			 * free list), no one else can have the lock.
			 */
			StartBufferIO(buf, false);

			inProgress = TRUE;

			/*
			 * Write the buffer out, being careful to release BufMgrLock
			 * while doing the I/O.
			 */
			FlushBuffer(buf, NULL);

			/*
			 * Somebody could have allocated another buffer for the same
			 * block we are about to read in. While we flush out the
			 * dirty buffer, we don't hold the lock and someone could have
			 * allocated another buffer for the same block. The problem is
			 * we haven't yet inserted the new tag into the buffer table.
			 * So we need to check here.		-ay 3/95
			 *
			 * Another reason we have to do this is to update cdb_found_index,
			 * since the CDB could have disappeared from B1/B2 list while
			 * we were writing.
			 */
			buf2 = StrategyBufferLookup(&newTag, true, &cdb_found_index);
			if (buf2 != NULL)
			{
				/*
				 * Found it. Someone has already done what we were about to
				 * do. We'll just handle this as if it were found in the
				 * buffer pool in the first place.  First, give up the
				 * buffer we were planning to use.
				 */
				TerminateBufferIO(buf, 0);
				UnpinBuffer(buf, true);

				buf = buf2;

				/* remaining code should match code at top of routine */

				*foundPtr = TRUE;

				PinBuffer(buf, true);

				if (!(buf->flags & BM_VALID))
				{
					if (buf->flags & BM_IO_IN_PROGRESS)
					{
						/* someone else is reading it, wait for them */
						WaitIO(buf);
					}
					if (!(buf->flags & BM_VALID))
					{
						/*
						 * If we get here, previous attempts to read the buffer
						 * must have failed ... but we shall bravely try again.
						 */
						*foundPtr = FALSE;
						StartBufferIO(buf, true);
					}
				}

				LWLockRelease(BufMgrLock);

				return buf;
			}

			/*
			 * Somebody could have pinned the buffer while we were doing
			 * the I/O and had given up the BufMgrLock.  If so, we can't
			 * recycle this buffer --- we need to clear the I/O flags,
			 * remove our pin and choose a new victim buffer.  Similarly,
			 * we have to start over if somebody re-dirtied the buffer.
			 */
			if (buf->refcount > 1 || buf->flags & BM_DIRTY || buf->cntxDirty)
			{
				TerminateBufferIO(buf, 0);
				UnpinBuffer(buf, true);
				inProgress = FALSE;
				buf = NULL;
			}
		}
	} while (buf == NULL);

	/*
	 * At this point we should have the sole pin on a non-dirty buffer and
	 * we may or may not already have the BM_IO_IN_PROGRESS flag set.
	 */

	/*
	 * Tell the buffer replacement strategy that we are replacing the
	 * buffer content. Then rename the buffer.  Clearing BM_VALID here
	 * is necessary, clearing the dirtybits is just paranoia.
	 */
	StrategyReplaceBuffer(buf, &newTag, cdb_found_index, cdb_replace_index);
	buf->tag = newTag;
	buf->flags &= ~(BM_VALID | BM_DIRTY | BM_JUST_DIRTIED | BM_IO_ERROR);
	buf->cntxDirty = false;

	/*
	 * Buffer contents are currently invalid.  Have to mark IO IN PROGRESS
	 * so no one fiddles with them until the read completes.  We may have
	 * already marked it, in which case we just flip from write to read
	 * status.
	 */
	if (!inProgress)
		StartBufferIO(buf, true);
	else
		ContinueBufferIO(buf, true);

	LWLockRelease(BufMgrLock);

	return buf;
}

/*
 * write_buffer -- common functionality for
 *				   WriteBuffer and WriteNoReleaseBuffer
 */
static void
write_buffer(Buffer buffer, bool release)
{
	BufferDesc *bufHdr;

	if (BufferIsLocal(buffer))
	{
		WriteLocalBuffer(buffer, release);
		return;
	}

	if (BAD_BUFFER_ID(buffer))
		elog(ERROR, "bad buffer id: %d", buffer);

	bufHdr = &BufferDescriptors[buffer - 1];

	Assert(PrivateRefCount[buffer - 1] > 0);

	LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);
	Assert(bufHdr->refcount > 0);

	/*
	 * If the buffer was not dirty already, do vacuum cost accounting.
	 */
	if (!(bufHdr->flags & BM_DIRTY) && VacuumCostActive)
		VacuumCostBalance += VacuumCostPageDirty;

	bufHdr->flags |= (BM_DIRTY | BM_JUST_DIRTIED);

	if (release)
		UnpinBuffer(bufHdr, true);
	LWLockRelease(BufMgrLock);
}

/*
 * WriteBuffer
 *
 *		Marks buffer contents as dirty (actual write happens later).
 *
 * Assume that buffer is pinned.  Assume that reln is valid.
 *
 * Side Effects:
 *		Pin count is decremented.
 */
void
WriteBuffer(Buffer buffer)
{
	write_buffer(buffer, true);
}

/*
 * WriteNoReleaseBuffer -- like WriteBuffer, but do not unpin the buffer
 *						   when the operation is complete.
 */
void
WriteNoReleaseBuffer(Buffer buffer)
{
	write_buffer(buffer, false);
}

/*
 * ReleaseAndReadBuffer -- combine ReleaseBuffer() and ReadBuffer()
 *		to save a lock release/acquire.
 *
 * Also, if the passed buffer is valid and already contains the desired block
 * number, we simply return it without ever acquiring the lock at all.
 * Since the passed buffer must be pinned, it's OK to examine its block
 * number without getting the lock first.
 *
 * Note: it is OK to pass buffer == InvalidBuffer, indicating that no old
 * buffer actually needs to be released.  This case is the same as ReadBuffer,
 * but can save some tests in the caller.
 *
 * Also note: while it will work to call this routine with blockNum == P_NEW,
 * it's best to avoid doing so, since that would result in calling
 * smgrnblocks() while holding the bufmgr lock, hence some loss of
 * concurrency.
 */
Buffer
ReleaseAndReadBuffer(Buffer buffer,
					 Relation relation,
					 BlockNumber blockNum)
{
	BufferDesc *bufHdr;

	if (BufferIsValid(buffer))
	{
		if (BufferIsLocal(buffer))
		{
			Assert(LocalRefCount[-buffer - 1] > 0);
			bufHdr = &LocalBufferDescriptors[-buffer - 1];
			if (bufHdr->tag.blockNum == blockNum &&
				RelFileNodeEquals(bufHdr->tag.rnode, relation->rd_node))
				return buffer;
			ResourceOwnerForgetBuffer(CurrentResourceOwner, buffer);
			/* owner now has a free slot, so no need for Enlarge() */
			LocalRefCount[-buffer - 1]--;
		}
		else
		{
			Assert(PrivateRefCount[buffer - 1] > 0);
			bufHdr = &BufferDescriptors[buffer - 1];
			if (bufHdr->tag.blockNum == blockNum &&
				RelFileNodeEquals(bufHdr->tag.rnode, relation->rd_node))
				return buffer;
			ResourceOwnerForgetBuffer(CurrentResourceOwner, buffer);
			/* owner now has a free slot, so no need for Enlarge() */
			if (PrivateRefCount[buffer - 1] > 1)
				PrivateRefCount[buffer - 1]--;
			else
			{
				LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);
				UnpinBuffer(bufHdr, false);
				return ReadBufferInternal(relation, blockNum, true);
			}
		}
	}
	else
		ResourceOwnerEnlargeBuffers(CurrentResourceOwner);

	return ReadBufferInternal(relation, blockNum, false);
}

/*
 * PinBuffer -- make buffer unavailable for replacement.
 *
 * This should be applied only to shared buffers, never local ones.
 * Bufmgr lock must be held by caller.
 *
 * Most but not all callers want CurrentResourceOwner to be adjusted.
 * Note that ResourceOwnerEnlargeBuffers must have been done already.
 */
static void
PinBuffer(BufferDesc *buf, bool fixOwner)
{
	int			b = BufferDescriptorGetBuffer(buf) - 1;

	if (PrivateRefCount[b] == 0)
		buf->refcount++;
	PrivateRefCount[b]++;
	Assert(PrivateRefCount[b] > 0);
	if (fixOwner)
		ResourceOwnerRememberBuffer(CurrentResourceOwner,
									BufferDescriptorGetBuffer(buf));
}

/*
 * UnpinBuffer -- make buffer available for replacement.
 *
 * This should be applied only to shared buffers, never local ones.
 * Bufmgr lock must be held by caller.
 *
 * Most but not all callers want CurrentResourceOwner to be adjusted.
 */
static void
UnpinBuffer(BufferDesc *buf, bool fixOwner)
{
	int			b = BufferDescriptorGetBuffer(buf) - 1;

	if (fixOwner)
		ResourceOwnerForgetBuffer(CurrentResourceOwner,
								  BufferDescriptorGetBuffer(buf));

	Assert(buf->refcount > 0);
	Assert(PrivateRefCount[b] > 0);
	PrivateRefCount[b]--;
	if (PrivateRefCount[b] == 0)
	{
		buf->refcount--;
		/* I'd better not still hold any locks on the buffer */
		Assert(!LWLockHeldByMe(buf->cntx_lock));
		Assert(!LWLockHeldByMe(buf->io_in_progress_lock));
	}

	if ((buf->flags & BM_PIN_COUNT_WAITER) != 0 &&
		buf->refcount == 1)
	{
		/* we just released the last pin other than the waiter's */
		buf->flags &= ~BM_PIN_COUNT_WAITER;
		ProcSendSignal(buf->wait_backend_id);
	}
	else
	{
		/* do nothing */
	}
}

/*
 * BufferSync -- Write out dirty buffers in the pool.
 *
 * This is called at checkpoint time to write out all dirty shared buffers,
 * and by the background writer process to write out some of the dirty blocks.
 * percent/maxpages should be zero in the former case, and nonzero limit
 * values in the latter.
 */
int
BufferSync(int percent, int maxpages)
{
	BufferDesc **dirty_buffers;
	BufferTag  *buftags;
	int			num_buffer_dirty;
	int			i;

	/*
	 * Get a list of all currently dirty buffers and how many there are.
	 * We do not flush buffers that get dirtied after we started. They
	 * have to wait until the next checkpoint.
	 */
	dirty_buffers = (BufferDesc **) palloc(NBuffers * sizeof(BufferDesc *));
	buftags = (BufferTag *) palloc(NBuffers * sizeof(BufferTag));

	LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);
	num_buffer_dirty = StrategyDirtyBufferList(dirty_buffers, buftags,
											   NBuffers);

	/*
	 * If called by the background writer, we are usually asked to
	 * only write out some portion of dirty buffers now, to prevent
	 * the IO storm at checkpoint time.
	 */
	if (percent > 0)
	{
		Assert(percent <= 100);
		num_buffer_dirty = (num_buffer_dirty * percent + 99) / 100;
	}
	if (maxpages > 0 && num_buffer_dirty > maxpages)
		num_buffer_dirty = maxpages;

	/* Make sure we can handle the pin inside the loop */
	ResourceOwnerEnlargeBuffers(CurrentResourceOwner);

	/*
	 * Loop over buffers to be written.  Note the BufMgrLock is held at
	 * loop top, but is released and reacquired within FlushBuffer,
	 * so we aren't holding it long.
	 */
	for (i = 0; i < num_buffer_dirty; i++)
	{
		BufferDesc *bufHdr = dirty_buffers[i];

		/*
		 * Check it is still the same page and still needs writing.
		 *
		 * We can check bufHdr->cntxDirty here *without* holding any lock
		 * on buffer context as long as we set this flag in access methods
		 * *before* logging changes with XLogInsert(): if someone will set
		 * cntxDirty just after our check we don't worry because of our
		 * checkpoint.redo points before log record for upcoming changes
		 * and so we are not required to write such dirty buffer.
		 */
		if (!(bufHdr->flags & BM_VALID))
			continue;
		if (!BUFFERTAGS_EQUAL(bufHdr->tag, buftags[i]))
			continue;
		if (!(bufHdr->flags & BM_DIRTY || bufHdr->cntxDirty))
			continue;

		/*
		 * IO synchronization. Note that we do it with unpinned buffer to
		 * avoid conflicts with FlushRelationBuffers.
		 */
		if (bufHdr->flags & BM_IO_IN_PROGRESS)
		{
			WaitIO(bufHdr);
			/* Still need writing? */
			if (!(bufHdr->flags & BM_VALID))
				continue;
			if (!BUFFERTAGS_EQUAL(bufHdr->tag, buftags[i]))
				continue;
			if (!(bufHdr->flags & BM_DIRTY || bufHdr->cntxDirty))
				continue;
		}

		/*
		 * Here: no one doing IO for this buffer and it's dirty. Pin
		 * buffer now and set IO state for it *before* acquiring shlock to
		 * avoid conflicts with FlushRelationBuffers.
		 */
		PinBuffer(bufHdr, true);
		StartBufferIO(bufHdr, false);

		FlushBuffer(bufHdr, NULL);

		TerminateBufferIO(bufHdr, 0);
		UnpinBuffer(bufHdr, true);
	}

	LWLockRelease(BufMgrLock);

	pfree(dirty_buffers);
	pfree(buftags);

	return num_buffer_dirty;
}

/*
 * WaitIO -- Block until the IO_IN_PROGRESS flag on 'buf' is cleared.
 *
 * Should be entered with buffer manager lock held; releases it before
 * waiting and re-acquires it afterwards.
 */
static void
WaitIO(BufferDesc *buf)
{
	/*
	 * Changed to wait until there's no IO - Inoue 01/13/2000
	 *
	 * Note this is *necessary* because an error abort in the process doing
	 * I/O could release the io_in_progress_lock prematurely. See
	 * AbortBufferIO.
	 */
	while ((buf->flags & BM_IO_IN_PROGRESS) != 0)
	{
		LWLockRelease(BufMgrLock);
		LWLockAcquire(buf->io_in_progress_lock, LW_SHARED);
		LWLockRelease(buf->io_in_progress_lock);
		LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);
	}
}


/*
 * Return a palloc'd string containing buffer usage statistics.
 */
char *
ShowBufferUsage(void)
{
	StringInfoData str;
	float		hitrate;
	float		localhitrate;

	initStringInfo(&str);

	if (ReadBufferCount == 0)
		hitrate = 0.0;
	else
		hitrate = (float) BufferHitCount *100.0 / ReadBufferCount;

	if (ReadLocalBufferCount == 0)
		localhitrate = 0.0;
	else
		localhitrate = (float) LocalBufferHitCount *100.0 / ReadLocalBufferCount;

	appendStringInfo(&str,
					 "!\tShared blocks: %10ld read, %10ld written, buffer hit rate = %.2f%%\n",
			ReadBufferCount - BufferHitCount, BufferFlushCount, hitrate);
	appendStringInfo(&str,
					 "!\tLocal  blocks: %10ld read, %10ld written, buffer hit rate = %.2f%%\n",
					 ReadLocalBufferCount - LocalBufferHitCount, LocalBufferFlushCount, localhitrate);
	appendStringInfo(&str,
					 "!\tDirect blocks: %10ld read, %10ld written\n",
					 NDirectFileRead, NDirectFileWrite);

	return str.data;
}

void
ResetBufferUsage(void)
{
	BufferHitCount = 0;
	ReadBufferCount = 0;
	BufferFlushCount = 0;
	LocalBufferHitCount = 0;
	ReadLocalBufferCount = 0;
	LocalBufferFlushCount = 0;
	NDirectFileRead = 0;
	NDirectFileWrite = 0;
}

/*
 *		AtEOXact_Buffers - clean up at end of transaction.
 *
 *		During abort, we need to release any buffer pins we're holding
 *		(this cleans up in case ereport interrupted a routine that pins a
 *		buffer).  During commit, we shouldn't need to do that, but check
 *		anyway to see if anyone leaked a buffer reference count.
 */
void
AtEOXact_Buffers(bool isCommit)
{
	int			i;

	for (i = 0; i < NBuffers; i++)
	{
		if (PrivateRefCount[i] != 0)
		{
			BufferDesc *buf = &(BufferDescriptors[i]);

			if (isCommit)
				elog(WARNING,
					 "buffer refcount leak: [%03d] "
					 "(rel=%u/%u/%u, blockNum=%u, flags=0x%x, refcount=%u %d)",
					 i,
					 buf->tag.rnode.spcNode, buf->tag.rnode.dbNode,
					 buf->tag.rnode.relNode,
					 buf->tag.blockNum, buf->flags,
					 buf->refcount, PrivateRefCount[i]);

			/*
			 * We don't worry about updating the ResourceOwner structures;
			 * resowner.c will clear them for itself.
			 */
			PrivateRefCount[i] = 1;		/* make sure we release shared pin */
			LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);
			UnpinBuffer(buf, false);
			LWLockRelease(BufMgrLock);
			Assert(PrivateRefCount[i] == 0);
		}
	}

	AtEOXact_LocalBuffers(isCommit);
}

/*
 * FlushBufferPool
 *
 * Flush all dirty blocks in buffer pool to disk at the checkpoint time.
 * Local relations do not participate in checkpoints, so they don't need to be
 * flushed.
 */
void
FlushBufferPool(void)
{
	BufferSync(-1, -1);
	smgrsync();
}


/*
 * Do whatever is needed to prepare for commit at the bufmgr and smgr levels
 */
void
BufmgrCommit(void)
{
	/* Nothing to do in bufmgr anymore... */

	smgrcommit();
}

/*
 * BufferGetBlockNumber
 *		Returns the block number associated with a buffer.
 *
 * Note:
 *		Assumes that the buffer is valid and pinned, else the
 *		value may be obsolete immediately...
 */
BlockNumber
BufferGetBlockNumber(Buffer buffer)
{
	Assert(BufferIsPinned(buffer));

	if (BufferIsLocal(buffer))
		return LocalBufferDescriptors[-buffer - 1].tag.blockNum;
	else
		return BufferDescriptors[buffer - 1].tag.blockNum;
}

/*
 * BufferGetFileNode
 *		Returns the relation ID (RelFileNode) associated with a buffer.
 *
 * This should make the same checks as BufferGetBlockNumber, but since the
 * two are generally called together, we don't bother.
 */
RelFileNode
BufferGetFileNode(Buffer buffer)
{
	BufferDesc *bufHdr;

	if (BufferIsLocal(buffer))
		bufHdr = &(LocalBufferDescriptors[-buffer - 1]);
	else
		bufHdr = &BufferDescriptors[buffer - 1];

	return (bufHdr->tag.rnode);
}

/*
 * FlushBuffer
 *		Physically write out a shared buffer.
 *
 * NOTE: this actually just passes the buffer contents to the kernel; the
 * real write to disk won't happen until the kernel feels like it.  This
 * is okay from our point of view since we can redo the changes from WAL.
 * However, we will need to force the changes to disk via fsync before
 * we can checkpoint WAL.
 *
 * BufMgrLock must be held at entry, and the buffer must be pinned.  The
 * caller is also responsible for doing StartBufferIO/TerminateBufferIO.
 *
 * If the caller has an smgr reference for the buffer's relation, pass it
 * as the second parameter.  If not, pass NULL.  (Do not open relation
 * while holding BufMgrLock!)
 */
static void
FlushBuffer(BufferDesc *buf, SMgrRelation reln)
{
	Buffer		buffer;
	XLogRecPtr	recptr;
	ErrorContextCallback errcontext;

	/* Transpose cntxDirty into flags while holding BufMgrLock */
	buf->cntxDirty = false;
	buf->flags |= BM_DIRTY;

	/* To check if block content changed while flushing. - vadim 01/17/97 */
	buf->flags &= ~BM_JUST_DIRTIED;

	/* Release BufMgrLock while doing xlog work */
	LWLockRelease(BufMgrLock);

	/* Setup error traceback support for ereport() */
	errcontext.callback = buffer_write_error_callback;
	errcontext.arg = buf;
	errcontext.previous = error_context_stack;
	error_context_stack = &errcontext;

	/* Find smgr relation for buffer while holding minimal locks */
	if (reln == NULL)
		reln = smgropen(buf->tag.rnode);

	buffer = BufferDescriptorGetBuffer(buf);

	/*
	 * Protect buffer content against concurrent update.  (Note that
	 * hint-bit updates can still occur while the write is in progress,
	 * but we assume that that will not invalidate the data written.)
	 */
	LockBuffer(buffer, BUFFER_LOCK_SHARE);

	/*
	 * Force XLOG flush for buffer' LSN.  This implements the basic WAL
	 * rule that log updates must hit disk before any of the data-file
	 * changes they describe do.
	 */
	recptr = BufferGetLSN(buf);
	XLogFlush(recptr);

	/*
	 * Now it's safe to write buffer to disk. Note that no one else
	 * should have been able to write it while we were busy with
	 * locking and log flushing because caller has set the IO flag.
	 *
	 * It would be better to clear BM_JUST_DIRTIED right here, but we'd
	 * have to reacquire the BufMgrLock and it doesn't seem worth it.
	 */
	smgrwrite(reln,
			  buf->tag.blockNum,
			  (char *) MAKE_PTR(buf->data),
			  false);

	/* Pop the error context stack */
	error_context_stack = errcontext.previous;

	/*
	 * Release the per-buffer readlock, reacquire BufMgrLock.
	 */
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);

	BufferFlushCount++;

	/*
	 * If this buffer was marked by someone as DIRTY while we were
	 * flushing it out we must not clear DIRTY flag - vadim 01/17/97
	 */
	if (!(buf->flags & BM_JUST_DIRTIED))
		buf->flags &= ~BM_DIRTY;
}

/*
 * RelationGetNumberOfBlocks
 *		Determines the current number of pages in the relation.
 */
BlockNumber
RelationGetNumberOfBlocks(Relation relation)
{
	/* Open it at the smgr level if not already done */
	if (relation->rd_smgr == NULL)
		relation->rd_smgr = smgropen(relation->rd_node);

	return smgrnblocks(relation->rd_smgr);
}

/*
 * RelationTruncate
 *		Physically truncate a relation to the specified number of blocks.
 *
 * Caller should already have done something to flush any buffered pages
 * that are to be dropped.
 */
void
RelationTruncate(Relation rel, BlockNumber nblocks)
{
	/* Open it at the smgr level if not already done */
	if (rel->rd_smgr == NULL)
		rel->rd_smgr = smgropen(rel->rd_node);

	/* Make sure rd_targblock isn't pointing somewhere past end */
	rel->rd_targblock = InvalidBlockNumber;

	/* Do the real work */
	smgrtruncate(rel->rd_smgr, nblocks, rel->rd_istemp);
}

/* ---------------------------------------------------------------------
 *		DropRelationBuffers
 *
 *		This function removes all the buffered pages for a relation
 *		from the buffer pool.  Dirty pages are simply dropped, without
 *		bothering to write them out first.	This is NOT rollback-able,
 *		and so should be used only with extreme caution!
 *
 *		There is no particularly good reason why this doesn't have a
 *		firstDelBlock parameter, except that current callers don't need it.
 *
 *		We assume that the caller holds an exclusive lock on the relation,
 *		which should assure that no new buffers will be acquired for the rel
 *		meanwhile.
 * --------------------------------------------------------------------
 */
void
DropRelationBuffers(Relation rel)
{
	DropRelFileNodeBuffers(rel->rd_node, rel->rd_istemp, 0);
}

/* ---------------------------------------------------------------------
 *		DropRelFileNodeBuffers
 *
 *		This is the same as DropRelationBuffers, except that the target
 *		relation is specified by RelFileNode and temp status, and one
 *		may specify the first block to drop.
 *
 *		This is NOT rollback-able.	One legitimate use is to clear the
 *		buffer cache of buffers for a relation that is being deleted
 *		during transaction abort.
 * --------------------------------------------------------------------
 */
void
DropRelFileNodeBuffers(RelFileNode rnode, bool istemp,
					   BlockNumber firstDelBlock)
{
	int			i;
	BufferDesc *bufHdr;

	if (istemp)
	{
		for (i = 0; i < NLocBuffer; i++)
		{
			bufHdr = &LocalBufferDescriptors[i];
			if (RelFileNodeEquals(bufHdr->tag.rnode, rnode) &&
				bufHdr->tag.blockNum >= firstDelBlock)
			{
				if (LocalRefCount[i] != 0)
					elog(FATAL, "block %u of %u/%u/%u is still referenced (local %u)",
						 bufHdr->tag.blockNum,
						 bufHdr->tag.rnode.spcNode,
						 bufHdr->tag.rnode.dbNode,
						 bufHdr->tag.rnode.relNode,
						 LocalRefCount[i]);
				bufHdr->flags &= ~(BM_DIRTY | BM_JUST_DIRTIED);
				bufHdr->cntxDirty = false;
				bufHdr->tag.rnode.relNode = InvalidOid;
			}
		}
		return;
	}

	LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);

	for (i = 1; i <= NBuffers; i++)
	{
		bufHdr = &BufferDescriptors[i - 1];
recheck:
		if (RelFileNodeEquals(bufHdr->tag.rnode, rnode) &&
			bufHdr->tag.blockNum >= firstDelBlock)
		{
			/*
			 * If there is I/O in progress, better wait till it's done;
			 * don't want to delete the relation out from under someone
			 * who's just trying to flush the buffer!
			 */
			if (bufHdr->flags & BM_IO_IN_PROGRESS)
			{
				WaitIO(bufHdr);

				/*
				 * By now, the buffer very possibly belongs to some other
				 * rel, so check again before proceeding.
				 */
				goto recheck;
			}

			/*
			 * There should be no pin on the buffer.
			 */
			if (bufHdr->refcount != 0)
				elog(FATAL, "block %u of %u/%u/%u is still referenced (private %d, global %u)",
					 bufHdr->tag.blockNum,
					 bufHdr->tag.rnode.spcNode,
					 bufHdr->tag.rnode.dbNode,
					 bufHdr->tag.rnode.relNode,
					 PrivateRefCount[i - 1], bufHdr->refcount);

			/* Now we can do what we came for */
			bufHdr->flags &= ~(BM_DIRTY | BM_JUST_DIRTIED);
			bufHdr->cntxDirty = false;

			/*
			 * And mark the buffer as no longer occupied by this rel.
			 */
			StrategyInvalidateBuffer(bufHdr);
		}
	}

	LWLockRelease(BufMgrLock);
}

/* ---------------------------------------------------------------------
 *		DropBuffers
 *
 *		This function removes all the buffers in the buffer cache for a
 *		particular database.  Dirty pages are simply dropped, without
 *		bothering to write them out first.	This is used when we destroy a
 *		database, to avoid trying to flush data to disk when the directory
 *		tree no longer exists.	Implementation is pretty similar to
 *		DropRelationBuffers() which is for destroying just one relation.
 * --------------------------------------------------------------------
 */
void
DropBuffers(Oid dbid)
{
	int			i;
	BufferDesc *bufHdr;

	LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);

	for (i = 1; i <= NBuffers; i++)
	{
		bufHdr = &BufferDescriptors[i - 1];
recheck:
		if (bufHdr->tag.rnode.dbNode == dbid)
		{
			/*
			 * If there is I/O in progress, better wait till it's done;
			 * don't want to delete the database out from under someone
			 * who's just trying to flush the buffer!
			 */
			if (bufHdr->flags & BM_IO_IN_PROGRESS)
			{
				WaitIO(bufHdr);

				/*
				 * By now, the buffer very possibly belongs to some other
				 * DB, so check again before proceeding.
				 */
				goto recheck;
			}
			/* Now we can do what we came for */
			bufHdr->flags &= ~(BM_DIRTY | BM_JUST_DIRTIED);
			bufHdr->cntxDirty = false;

			/*
			 * The thing should be free, if caller has checked that no
			 * backends are running in that database.
			 */
			Assert(bufHdr->refcount == 0);

			/*
			 * And mark the buffer as no longer occupied by this page.
			 */
			StrategyInvalidateBuffer(bufHdr);
		}
	}

	LWLockRelease(BufMgrLock);
}

/* -----------------------------------------------------------------
 *		PrintBufferDescs
 *
 *		this function prints all the buffer descriptors, for debugging
 *		use only.
 * -----------------------------------------------------------------
 */
#ifdef NOT_USED
void
PrintBufferDescs(void)
{
	int			i;
	BufferDesc *buf = BufferDescriptors;

	if (IsUnderPostmaster)
	{
		LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);
		for (i = 0; i < NBuffers; ++i, ++buf)
		{
			elog(LOG,
				 "[%02d] (freeNext=%d, freePrev=%d, rel=%u/%u/%u, "
				 "blockNum=%u, flags=0x%x, refcount=%u %d)",
				 i, buf->freeNext, buf->freePrev,
				 buf->tag.rnode.spcNode, buf->tag.rnode.dbNode,
				 buf->tag.rnode.relNode,
				 buf->tag.blockNum, buf->flags,
				 buf->refcount, PrivateRefCount[i]);
		}
		LWLockRelease(BufMgrLock);
	}
	else
	{
		/* interactive backend */
		for (i = 0; i < NBuffers; ++i, ++buf)
		{
			printf("[%-2d] (%u/%u/%u, %u) flags=0x%x, refcount=%u %d)\n",
				   i, buf->tag.rnode.spcNode, buf->tag.rnode.dbNode,
				   buf->tag.rnode.relNode, buf->tag.blockNum,
				   buf->flags, buf->refcount, PrivateRefCount[i]);
		}
	}
}
#endif

#ifdef NOT_USED
void
PrintPinnedBufs(void)
{
	int			i;
	BufferDesc *buf = BufferDescriptors;

	LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);
	for (i = 0; i < NBuffers; ++i, ++buf)
	{
		if (PrivateRefCount[i] > 0)
			elog(NOTICE,
				 "[%02d] (freeNext=%d, freePrev=%d, rel=%u/%u/%u, "
				 "blockNum=%u, flags=0x%x, refcount=%u %d)",
				 i, buf->freeNext, buf->freePrev,
				 buf->tag.rnode.spcNode, buf->tag.rnode.dbNode,
				 buf->tag.rnode.relNode,
				 buf->tag.blockNum, buf->flags,
				 buf->refcount, PrivateRefCount[i]);
	}
	LWLockRelease(BufMgrLock);
}
#endif

/* ---------------------------------------------------------------------
 *		FlushRelationBuffers
 *
 *		This function writes all dirty pages of a relation out to disk.
 *		Furthermore, pages that have blocknumber >= firstDelBlock are
 *		actually removed from the buffer pool.
 *
 *		This is called by DROP TABLE to clear buffers for the relation
 *		from the buffer pool.  Note that we must write dirty buffers,
 *		rather than just dropping the changes, because our transaction
 *		might abort later on; we want to roll back safely in that case.
 *
 *		This is also called by VACUUM before truncating the relation to the
 *		given number of blocks.  It might seem unnecessary for VACUUM to
 *		write dirty pages before firstDelBlock, since VACUUM should already
 *		have committed its changes.  However, it is possible for there still
 *		to be dirty pages: if some page had unwritten on-row tuple status
 *		updates from a prior transaction, and VACUUM had no additional
 *		changes to make to that page, then VACUUM won't have written it.
 *		This is harmless in most cases but will break pg_upgrade, which
 *		relies on VACUUM to ensure that *all* tuples have correct on-row
 *		status.  So, we check and flush all dirty pages of the rel
 *		regardless of block number.
 *
 *		In all cases, the caller should be holding AccessExclusiveLock on
 *		the target relation to ensure that no other backend is busy reading
 *		more blocks of the relation (or might do so before we commit).
 *		This should also ensure that no one is busy dirtying these blocks.
 *
 *		Formerly, we considered it an error condition if we found dirty
 *		buffers here.	However, since BufferSync no longer forces out all
 *		dirty buffers at every xact commit, it's possible for dirty buffers
 *		to still be present in the cache due to failure of an earlier
 *		transaction.  So, must flush dirty buffers without complaint.
 *
 *		XXX currently it sequentially searches the buffer pool, should be
 *		changed to more clever ways of searching.
 * --------------------------------------------------------------------
 */
void
FlushRelationBuffers(Relation rel, BlockNumber firstDelBlock)
{
	int			i;
	BufferDesc *bufHdr;

	if (rel->rd_istemp)
	{
		for (i = 0; i < NLocBuffer; i++)
		{
			bufHdr = &LocalBufferDescriptors[i];
			if (RelFileNodeEquals(bufHdr->tag.rnode, rel->rd_node))
			{
				if ((bufHdr->flags & BM_VALID) &&
					(bufHdr->flags & BM_DIRTY || bufHdr->cntxDirty))
				{
					ErrorContextCallback errcontext;

					/* Setup error traceback support for ereport() */
					errcontext.callback = buffer_write_error_callback;
					errcontext.arg = bufHdr;
					errcontext.previous = error_context_stack;
					error_context_stack = &errcontext;

					/* Open rel at the smgr level if not already done */
					if (rel->rd_smgr == NULL)
						rel->rd_smgr = smgropen(rel->rd_node);

					smgrwrite(rel->rd_smgr,
							  bufHdr->tag.blockNum,
							  (char *) MAKE_PTR(bufHdr->data),
							  true);

					bufHdr->flags &= ~(BM_DIRTY | BM_JUST_DIRTIED);
					bufHdr->cntxDirty = false;

					/* Pop the error context stack */
					error_context_stack = errcontext.previous;
				}
				if (LocalRefCount[i] > 0)
					elog(ERROR, "FlushRelationBuffers(\"%s\" (local), %u): block %u is referenced (%d)",
						 RelationGetRelationName(rel), firstDelBlock,
						 bufHdr->tag.blockNum, LocalRefCount[i]);
				if (bufHdr->tag.blockNum >= firstDelBlock)
					bufHdr->tag.rnode.relNode = InvalidOid;
			}
		}

		return;
	}

	/* Make sure we can handle the pin inside the loop */
	ResourceOwnerEnlargeBuffers(CurrentResourceOwner);

	LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);

	for (i = 0; i < NBuffers; i++)
	{
		bufHdr = &BufferDescriptors[i];
		if (RelFileNodeEquals(bufHdr->tag.rnode, rel->rd_node))
		{
			if ((bufHdr->flags & BM_VALID) &&
				(bufHdr->flags & BM_DIRTY || bufHdr->cntxDirty))
			{
				PinBuffer(bufHdr, true);
				/* Someone else might be flushing buffer */
				if (bufHdr->flags & BM_IO_IN_PROGRESS)
					WaitIO(bufHdr);
				/* Still dirty? */
				if (bufHdr->flags & BM_DIRTY || bufHdr->cntxDirty)
				{
					StartBufferIO(bufHdr, false);

					FlushBuffer(bufHdr, rel->rd_smgr);

					TerminateBufferIO(bufHdr, 0);
				}
				UnpinBuffer(bufHdr, true);
				if (bufHdr->flags & BM_DIRTY || bufHdr->cntxDirty)
					elog(ERROR, "FlushRelationBuffers(\"%s\", %u): block %u was re-dirtied",
						 RelationGetRelationName(rel), firstDelBlock,
						 bufHdr->tag.blockNum);
			}
			if (bufHdr->refcount != 0)
				elog(ERROR, "FlushRelationBuffers(\"%s\", %u): block %u is referenced (private %d, global %u)",
					 RelationGetRelationName(rel), firstDelBlock,
					 bufHdr->tag.blockNum,
					 PrivateRefCount[i], bufHdr->refcount);
			if (bufHdr->tag.blockNum >= firstDelBlock)
				StrategyInvalidateBuffer(bufHdr);
		}
	}

	LWLockRelease(BufMgrLock);
}

/*
 * ReleaseBuffer -- remove the pin on a buffer without
 *		marking it dirty.
 */
void
ReleaseBuffer(Buffer buffer)
{
	BufferDesc *bufHdr;

	ResourceOwnerForgetBuffer(CurrentResourceOwner, buffer);

	if (BufferIsLocal(buffer))
	{
		Assert(LocalRefCount[-buffer - 1] > 0);
		LocalRefCount[-buffer - 1]--;
		return;
	}

	if (BAD_BUFFER_ID(buffer))
		elog(ERROR, "bad buffer id: %d", buffer);

	bufHdr = &BufferDescriptors[buffer - 1];

	Assert(PrivateRefCount[buffer - 1] > 0);

	if (PrivateRefCount[buffer - 1] > 1)
		PrivateRefCount[buffer - 1]--;
	else
	{
		LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);
		UnpinBuffer(bufHdr, false);
		LWLockRelease(BufMgrLock);
	}
}

/*
 * IncrBufferRefCount
 *		Increment the pin count on a buffer that we have *already* pinned
 *		at least once.
 *
 *		This function cannot be used on a buffer we do not have pinned,
 *		because it doesn't change the shared buffer state.  Therefore the
 *		Assert checks are for refcount > 0.  Someone got this wrong once...
 */
void
IncrBufferRefCount(Buffer buffer)
{
	ResourceOwnerEnlargeBuffers(CurrentResourceOwner);
	ResourceOwnerRememberBuffer(CurrentResourceOwner, buffer);
	if (BufferIsLocal(buffer))
	{
		Assert(buffer >= -NLocBuffer);
		Assert(LocalRefCount[-buffer - 1] > 0);
		LocalRefCount[-buffer - 1]++;
	}
	else
	{
		Assert(!BAD_BUFFER_ID(buffer));
		Assert(PrivateRefCount[buffer - 1] > 0);
		PrivateRefCount[buffer - 1]++;
	}
}

#ifdef NOT_USED
void
IncrBufferRefCount_Debug(char *file, int line, Buffer buffer)
{
	IncrBufferRefCount(buffer);
	if (ShowPinTrace && !BufferIsLocal(buffer) && is_userbuffer(buffer))
	{
		BufferDesc *buf = &BufferDescriptors[buffer - 1];

		fprintf(stderr,
				"PIN(Incr) %d rel = %u/%u/%u, blockNum = %u, "
				"refcount = %d, file: %s, line: %d\n",
				buffer,
				buf->tag.rnode.spcNode, buf->tag.rnode.dbNode,
				buf->tag.rnode.relNode, buf->tag.blockNum,
				PrivateRefCount[buffer - 1], file, line);
	}
}
#endif

#ifdef NOT_USED
void
ReleaseBuffer_Debug(char *file, int line, Buffer buffer)
{
	ReleaseBuffer(buffer);
	if (ShowPinTrace && !BufferIsLocal(buffer) && is_userbuffer(buffer))
	{
		BufferDesc *buf = &BufferDescriptors[buffer - 1];

		fprintf(stderr,
				"UNPIN(Rel) %d rel = %u/%u/%u, blockNum = %u, "
				"refcount = %d, file: %s, line: %d\n",
				buffer,
				buf->tag.rnode.spcNode, buf->tag.rnode.dbNode,
				buf->tag.rnode.relNode, buf->tag.blockNum,
				PrivateRefCount[buffer - 1], file, line);
	}
}
#endif

#ifdef NOT_USED
Buffer
ReleaseAndReadBuffer_Debug(char *file,
						   int line,
						   Buffer buffer,
						   Relation relation,
						   BlockNumber blockNum)
{
	bool		bufferValid;
	Buffer		b;

	bufferValid = BufferIsValid(buffer);
	b = ReleaseAndReadBuffer(buffer, relation, blockNum);
	if (ShowPinTrace && bufferValid && BufferIsLocal(buffer)
		&& is_userbuffer(buffer))
	{
		BufferDesc *buf = &BufferDescriptors[buffer - 1];

		fprintf(stderr,
				"UNPIN(Rel&Rd) %d rel = %u/%u/%u, blockNum = %u, "
				"refcount = %d, file: %s, line: %d\n",
				buffer,
				buf->tag.rnode.spcNode, buf->tag.rnode.dbNode,
				buf->tag.rnode.relNode, buf->tag.blockNum,
				PrivateRefCount[buffer - 1], file, line);
	}
	if (ShowPinTrace && BufferIsLocal(buffer) && is_userbuffer(buffer))
	{
		BufferDesc *buf = &BufferDescriptors[b - 1];

		fprintf(stderr,
				"PIN(Rel&Rd) %d rel = %u/%u/%u, blockNum = %u, "
				"refcount = %d, file: %s, line: %d\n",
				b,
				buf->tag.rnode.spcNode, buf->tag.rnode.dbNode,
				buf->tag.rnode.relNode, buf->tag.blockNum,
				PrivateRefCount[b - 1], file, line);
	}
	return b;
}
#endif

/*
 * SetBufferCommitInfoNeedsSave
 *
 *	Mark a buffer dirty when we have updated tuple commit-status bits in it.
 *
 * This is similar to WriteNoReleaseBuffer, except that we have not made a
 * critical change that has to be flushed to disk before xact commit --- the
 * status-bit update could be redone by someone else just as easily.
 *
 * This routine might get called many times on the same page, if we are making
 * the first scan after commit of an xact that added/deleted many tuples.
 * So, be as quick as we can if the buffer is already dirty.  We do this by
 * not acquiring BufMgrLock if it looks like the status bits are already OK.
 * (Note it is okay if someone else clears BM_JUST_DIRTIED immediately after
 * we look, because the buffer content update is already done and will be
 * reflected in the I/O.)
 */
void
SetBufferCommitInfoNeedsSave(Buffer buffer)
{
	BufferDesc *bufHdr;

	if (BufferIsLocal(buffer))
	{
		WriteLocalBuffer(buffer, false);
		return;
	}

	if (BAD_BUFFER_ID(buffer))
		elog(ERROR, "bad buffer id: %d", buffer);

	bufHdr = &BufferDescriptors[buffer - 1];

	if ((bufHdr->flags & (BM_DIRTY | BM_JUST_DIRTIED)) !=
		(BM_DIRTY | BM_JUST_DIRTIED))
	{
		LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);
		Assert(bufHdr->refcount > 0);
		bufHdr->flags |= (BM_DIRTY | BM_JUST_DIRTIED);
		LWLockRelease(BufMgrLock);
	}
}

/*
 * Release buffer context locks for shared buffers.
 *
 * Used to clean up after errors.
 */
void
UnlockBuffers(void)
{
	BufferDesc *buf;
	int			i;

	for (i = 0; i < NBuffers; i++)
	{
		bits8		buflocks = BufferLocks[i];

		if (buflocks == 0)
			continue;

		Assert(BufferIsValid(i + 1));
		buf = &(BufferDescriptors[i]);

		HOLD_INTERRUPTS();		/* don't want to die() partway through... */

		/*
		 * The buffer's cntx_lock has already been released by lwlock.c.
		 */

		if (buflocks & BL_PIN_COUNT_LOCK)
		{
			LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);

			/*
			 * Don't complain if flag bit not set; it could have been
			 * reset but we got a cancel/die interrupt before getting the
			 * signal.
			 */
			if ((buf->flags & BM_PIN_COUNT_WAITER) != 0 &&
				buf->wait_backend_id == MyBackendId)
				buf->flags &= ~BM_PIN_COUNT_WAITER;
			LWLockRelease(BufMgrLock);
			ProcCancelWaitForSignal();
		}

		BufferLocks[i] = 0;

		RESUME_INTERRUPTS();
	}
}

/*
 * Acquire or release the cntx_lock for the buffer.
 */
void
LockBuffer(Buffer buffer, int mode)
{
	BufferDesc *buf;

	Assert(BufferIsValid(buffer));
	if (BufferIsLocal(buffer))
		return;

	buf = &(BufferDescriptors[buffer - 1]);

	if (mode == BUFFER_LOCK_UNLOCK)
		LWLockRelease(buf->cntx_lock);
	else if (mode == BUFFER_LOCK_SHARE)
		LWLockAcquire(buf->cntx_lock, LW_SHARED);
	else if (mode == BUFFER_LOCK_EXCLUSIVE)
	{
		LWLockAcquire(buf->cntx_lock, LW_EXCLUSIVE);

		/*
		 * This is not the best place to set cntxDirty flag (eg indices do
		 * not always change buffer they lock in excl mode). But please
		 * remember that it's critical to set cntxDirty *before* logging
		 * changes with XLogInsert() - see comments in BufferSync().
		 */
		buf->cntxDirty = true;
	}
	else
		elog(ERROR, "unrecognized buffer lock mode: %d", mode);
}

/*
 * Acquire the cntx_lock for the buffer, but only if we don't have to wait.
 *
 * This assumes the caller wants BUFFER_LOCK_EXCLUSIVE mode.
 */
bool
ConditionalLockBuffer(Buffer buffer)
{
	BufferDesc *buf;

	Assert(BufferIsValid(buffer));
	if (BufferIsLocal(buffer))
		return true;			/* act as though we got it */

	buf = &(BufferDescriptors[buffer - 1]);

	if (LWLockConditionalAcquire(buf->cntx_lock, LW_EXCLUSIVE))
	{
		/*
		 * This is not the best place to set cntxDirty flag (eg indices do
		 * not always change buffer they lock in excl mode). But please
		 * remember that it's critical to set cntxDirty *before* logging
		 * changes with XLogInsert() - see comments in BufferSync().
		 */
		buf->cntxDirty = true;

		return true;
	}
	return false;
}

/*
 * LockBufferForCleanup - lock a buffer in preparation for deleting items
 *
 * Items may be deleted from a disk page only when the caller (a) holds an
 * exclusive lock on the buffer and (b) has observed that no other backend
 * holds a pin on the buffer.  If there is a pin, then the other backend
 * might have a pointer into the buffer (for example, a heapscan reference
 * to an item --- see README for more details).  It's OK if a pin is added
 * after the cleanup starts, however; the newly-arrived backend will be
 * unable to look at the page until we release the exclusive lock.
 *
 * To implement this protocol, a would-be deleter must pin the buffer and
 * then call LockBufferForCleanup().  LockBufferForCleanup() is similar to
 * LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE), except that it loops until
 * it has successfully observed pin count = 1.
 */
void
LockBufferForCleanup(Buffer buffer)
{
	BufferDesc *bufHdr;
	bits8	   *buflock;

	Assert(BufferIsValid(buffer));

	if (BufferIsLocal(buffer))
	{
		/* There should be exactly one pin */
		if (LocalRefCount[-buffer - 1] != 1)
			elog(ERROR, "incorrect local pin count: %d",
				 LocalRefCount[-buffer - 1]);
		/* Nobody else to wait for */
		return;
	}

	/* There should be exactly one local pin */
	if (PrivateRefCount[buffer - 1] != 1)
		elog(ERROR, "incorrect local pin count: %d",
			 PrivateRefCount[buffer - 1]);

	bufHdr = &BufferDescriptors[buffer - 1];
	buflock = &(BufferLocks[buffer - 1]);

	for (;;)
	{
		/* Try to acquire lock */
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);
		Assert(bufHdr->refcount > 0);
		if (bufHdr->refcount == 1)
		{
			/* Successfully acquired exclusive lock with pincount 1 */
			LWLockRelease(BufMgrLock);
			return;
		}
		/* Failed, so mark myself as waiting for pincount 1 */
		if (bufHdr->flags & BM_PIN_COUNT_WAITER)
		{
			LWLockRelease(BufMgrLock);
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			elog(ERROR, "multiple backends attempting to wait for pincount 1");
		}
		bufHdr->wait_backend_id = MyBackendId;
		bufHdr->flags |= BM_PIN_COUNT_WAITER;
		*buflock |= BL_PIN_COUNT_LOCK;
		LWLockRelease(BufMgrLock);
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		/* Wait to be signaled by UnpinBuffer() */
		ProcWaitForSignal();
		*buflock &= ~BL_PIN_COUNT_LOCK;
		/* Loop back and try again */
	}
}

/*
 *	Functions for IO error handling
 *
 *	Note : We assume that nested buffer IO never occur.
 *	i.e at most one io_in_progress lock is held per proc.
*/
static BufferDesc *InProgressBuf = NULL;
static bool IsForInput;

/*
 * Function:StartBufferIO
 *	(Assumptions)
 *	My process is executing no IO
 *	BufMgrLock is held
 *	BM_IO_IN_PROGRESS mask is not set for the buffer
 *	The buffer is Pinned
 *
 * Because BufMgrLock is held, we are already in an interrupt holdoff here,
 * and do not need another.
 */
static void
StartBufferIO(BufferDesc *buf, bool forInput)
{
	Assert(!InProgressBuf);
	Assert(!(buf->flags & BM_IO_IN_PROGRESS));
	buf->flags |= BM_IO_IN_PROGRESS;

	LWLockAcquire(buf->io_in_progress_lock, LW_EXCLUSIVE);

	InProgressBuf = buf;
	IsForInput = forInput;
}

/*
 * Function:TerminateBufferIO
 *	(Assumptions)
 *	My process is executing IO for the buffer
 *	BufMgrLock is held
 *	BM_IO_IN_PROGRESS mask is set for the buffer
 *	The buffer is Pinned
 *
 * err_flag must be 0 for successful completion and BM_IO_ERROR for failure.
 *
 * Because BufMgrLock is held, we are already in an interrupt holdoff here,
 * and do not need another.
 */
static void
TerminateBufferIO(BufferDesc *buf, int err_flag)
{
	Assert(buf == InProgressBuf);
	Assert(buf->flags & BM_IO_IN_PROGRESS);
	buf->flags &= ~(BM_IO_IN_PROGRESS | BM_IO_ERROR);
	buf->flags |= err_flag;

	LWLockRelease(buf->io_in_progress_lock);

	InProgressBuf = NULL;
}

/*
 * Function:ContinueBufferIO
 *	(Assumptions)
 *	My process is executing IO for the buffer
 *	BufMgrLock is held
 *	The buffer is Pinned
 *
 * Because BufMgrLock is held, we are already in an interrupt holdoff here,
 * and do not need another.
 */
static void
ContinueBufferIO(BufferDesc *buf, bool forInput)
{
	Assert(buf == InProgressBuf);
	Assert(buf->flags & BM_IO_IN_PROGRESS);
	IsForInput = forInput;
}

#ifdef NOT_USED
void
InitBufferIO(void)
{
	InProgressBuf = NULL;
}
#endif

/*
 *	Clean up any active buffer I/O after an error.
 *	BufMgrLock isn't held when this function is called,
 *	but we haven't yet released buffer pins, so the buffer is still pinned.
 *
 *	If I/O was in progress, we always set BM_IO_ERROR.
 */
void
AbortBufferIO(void)
{
	BufferDesc *buf = InProgressBuf;

	if (buf)
	{
		/*
		 * Since LWLockReleaseAll has already been called, we're not
		 * holding the buffer's io_in_progress_lock. We have to re-acquire
		 * it so that we can use TerminateBufferIO. Anyone who's executing
		 * WaitIO on the buffer will be in a busy spin until we succeed in
		 * doing this.
		 */
		LWLockAcquire(buf->io_in_progress_lock, LW_EXCLUSIVE);

		LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);
		Assert(buf->flags & BM_IO_IN_PROGRESS);
		if (IsForInput)
		{
			Assert(!(buf->flags & BM_DIRTY || buf->cntxDirty));
			/* We'd better not think buffer is valid yet */
			Assert(!(buf->flags & BM_VALID));
		}
		else
		{
			Assert(buf->flags & BM_DIRTY || buf->cntxDirty);
			/* Issue notice if this is not the first failure... */
			if (buf->flags & BM_IO_ERROR)
			{
				ereport(WARNING,
						(errcode(ERRCODE_IO_ERROR),
						 errmsg("could not write block %u of %u/%u/%u",
								buf->tag.blockNum,
								buf->tag.rnode.spcNode,
								buf->tag.rnode.dbNode,
								buf->tag.rnode.relNode),
						 errdetail("Multiple failures --- write error may be permanent.")));
			}
			buf->flags |= BM_DIRTY;
		}
		TerminateBufferIO(buf, BM_IO_ERROR);
		LWLockRelease(BufMgrLock);
	}
}

/*
 * Error context callback for errors occurring during buffer writes.
 */
static void
buffer_write_error_callback(void *arg)
{
	BufferDesc *bufHdr = (BufferDesc *) arg;

	if (bufHdr != NULL)
		errcontext("writing block %u of relation %u/%u/%u",
				   bufHdr->tag.blockNum,
				   bufHdr->tag.rnode.spcNode,
				   bufHdr->tag.rnode.dbNode,
				   bufHdr->tag.rnode.relNode);
}
