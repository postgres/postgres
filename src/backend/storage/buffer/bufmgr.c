/*-------------------------------------------------------------------------
 *
 * bufmgr.c
 *	  buffer manager interface routines
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/buffer/bufmgr.c,v 1.213 2006/10/22 20:34:54 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * ReadBuffer() -- find or create a buffer holding the requested page,
 *		and pin it so that no one can destroy it while this process
 *		is using it.
 *
 * ReleaseBuffer() -- unpin a buffer
 *
 * MarkBufferDirty() -- mark a pinned buffer's contents as "dirty".
 *		The disk write is delayed until buffer replacement or checkpoint.
 *
 * BufferSync() -- flush all dirty buffers in the buffer pool.
 *
 * BgBufferSync() -- flush some dirty buffers in the buffer pool.
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

#include "miscadmin.h"
#include "postmaster/bgwriter.h"
#include "storage/buf_internals.h"
#include "storage/bufpage.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/smgr.h"
#include "utils/resowner.h"
#include "pgstat.h"


/* Note: these two macros only work on shared buffers, not local ones! */
#define BufHdrGetBlock(bufHdr)	((Block) (BufferBlocks + ((Size) (bufHdr)->buf_id) * BLCKSZ))
#define BufferGetLSN(bufHdr)	(*((XLogRecPtr*) BufHdrGetBlock(bufHdr)))

/* Note: this macro only works on local buffers, not shared ones! */
#define LocalBufHdrGetBlock(bufHdr) \
	LocalBufferBlockPointers[-((bufHdr)->buf_id + 2)]

/* interval for calling AbsorbFsyncRequests in BufferSync */
#define WRITES_PER_ABSORB		1000


/* GUC variables */
bool		zero_damaged_pages = false;
double		bgwriter_lru_percent = 1.0;
double		bgwriter_all_percent = 0.333;
int			bgwriter_lru_maxpages = 5;
int			bgwriter_all_maxpages = 5;


long		NDirectFileRead;	/* some I/O's are direct file access. bypass
								 * bufmgr */
long		NDirectFileWrite;	/* e.g., I/O in psort and hashjoin. */


/* local state for StartBufferIO and related functions */
static volatile BufferDesc *InProgressBuf = NULL;
static bool IsForInput;

/* local state for LockBufferForCleanup */
static volatile BufferDesc *PinCountWaitBuf = NULL;


static bool PinBuffer(volatile BufferDesc *buf);
static void PinBuffer_Locked(volatile BufferDesc *buf);
static void UnpinBuffer(volatile BufferDesc *buf,
			bool fixOwner, bool normalAccess);
static bool SyncOneBuffer(int buf_id, bool skip_pinned);
static void WaitIO(volatile BufferDesc *buf);
static bool StartBufferIO(volatile BufferDesc *buf, bool forInput);
static void TerminateBufferIO(volatile BufferDesc *buf, bool clear_dirty,
				  int set_flag_bits);
static void buffer_write_error_callback(void *arg);
static volatile BufferDesc *BufferAlloc(Relation reln, BlockNumber blockNum,
			bool *foundPtr);
static void FlushBuffer(volatile BufferDesc *buf, SMgrRelation reln);
static void AtProcExit_Buffers(int code, Datum arg);


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
	volatile BufferDesc *bufHdr;
	Block		bufBlock;
	bool		found;
	bool		isExtend;
	bool		isLocalBuf;

	/* Make sure we will have room to remember the buffer pin */
	ResourceOwnerEnlargeBuffers(CurrentResourceOwner);

	isExtend = (blockNum == P_NEW);
	isLocalBuf = reln->rd_istemp;

	/* Open it at the smgr level if not already done */
	RelationOpenSmgr(reln);

	/* Substitute proper block number if caller asked for P_NEW */
	if (isExtend)
		blockNum = smgrnblocks(reln->rd_smgr);

	pgstat_count_buffer_read(&reln->pgstat_info, reln);

	if (isLocalBuf)
	{
		ReadLocalBufferCount++;
		bufHdr = LocalBufferAlloc(reln, blockNum, &found);
		if (found)
			LocalBufferHitCount++;
	}
	else
	{
		ReadBufferCount++;

		/*
		 * lookup the buffer.  IO_IN_PROGRESS is set if the requested block is
		 * not currently in memory.
		 */
		bufHdr = BufferAlloc(reln, blockNum, &found);
		if (found)
			BufferHitCount++;
	}

	/* At this point we do NOT hold any locks. */

	/* if it was already in the buffer pool, we're done */
	if (found)
	{
		if (!isExtend)
		{
			/* Just need to update stats before we exit */
			pgstat_count_buffer_hit(&reln->pgstat_info, reln);

			if (VacuumCostActive)
				VacuumCostBalance += VacuumCostPageHit;

			return BufferDescriptorGetBuffer(bufHdr);
		}

		/*
		 * We get here only in the corner case where we are trying to extend
		 * the relation but we found a pre-existing buffer marked BM_VALID.
		 * This can happen because mdread doesn't complain about reads beyond
		 * EOF --- which is arguably bogus, but changing it seems tricky ---
		 * and so a previous attempt to read a block just beyond EOF could
		 * have left a "valid" zero-filled buffer.	Unfortunately, we have
		 * also seen this case occurring because of buggy Linux kernels that
		 * sometimes return an lseek(SEEK_END) result that doesn't account for
		 * a recent write.	In that situation, the pre-existing buffer would
		 * contain valid data that we don't want to overwrite.  Since the
		 * legitimate cases should always have left a zero-filled buffer,
		 * complain if not PageIsNew.
		 */
		bufBlock = isLocalBuf ? LocalBufHdrGetBlock(bufHdr) : BufHdrGetBlock(bufHdr);
		if (!PageIsNew((PageHeader) bufBlock))
			ereport(ERROR,
					(errmsg("unexpected data beyond EOF in block %u of relation \"%s\"",
							blockNum, RelationGetRelationName(reln)),
					 errhint("This has been seen to occur with buggy kernels; consider updating your system.")));

		/*
		 * We *must* do smgrextend before succeeding, else the page will not
		 * be reserved by the kernel, and the next P_NEW call will decide to
		 * return the same page.  Clear the BM_VALID bit, do the StartBufferIO
		 * call that BufferAlloc didn't, and proceed.
		 */
		if (isLocalBuf)
		{
			/* Only need to adjust flags */
			Assert(bufHdr->flags & BM_VALID);
			bufHdr->flags &= ~BM_VALID;
		}
		else
		{
			/*
			 * Loop to handle the very small possibility that someone re-sets
			 * BM_VALID between our clearing it and StartBufferIO inspecting
			 * it.
			 */
			do
			{
				LockBufHdr(bufHdr);
				Assert(bufHdr->flags & BM_VALID);
				bufHdr->flags &= ~BM_VALID;
				UnlockBufHdr(bufHdr);
			} while (!StartBufferIO(bufHdr, true));
		}
	}

	/*
	 * if we have gotten to this point, we have allocated a buffer for the
	 * page but its contents are not yet valid.  IO_IN_PROGRESS is set for it,
	 * if it's a shared buffer.
	 *
	 * Note: if smgrextend fails, we will end up with a buffer that is
	 * allocated but not marked BM_VALID.  P_NEW will still select the same
	 * block number (because the relation didn't get any longer on disk) and
	 * so future attempts to extend the relation will find the same buffer (if
	 * it's not been recycled) but come right back here to try smgrextend
	 * again.
	 */
	Assert(!(bufHdr->flags & BM_VALID));		/* spinlock not needed */

	bufBlock = isLocalBuf ? LocalBufHdrGetBlock(bufHdr) : BufHdrGetBlock(bufHdr);

	if (isExtend)
	{
		/* new buffers are zero-filled */
		MemSet((char *) bufBlock, 0, BLCKSZ);
		smgrextend(reln->rd_smgr, blockNum, (char *) bufBlock,
				   reln->rd_istemp);
	}
	else
	{
		smgrread(reln->rd_smgr, blockNum, (char *) bufBlock);
		/* check for garbage data */
		if (!PageHeaderIsValid((PageHeader) bufBlock))
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
				MemSet((char *) bufBlock, 0, BLCKSZ);
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
		/* Set BM_VALID, terminate IO, and wake up any waiters */
		TerminateBufferIO(bufHdr, false, BM_VALID);
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
 * No locks are held either at entry or exit.
 */
static volatile BufferDesc *
BufferAlloc(Relation reln,
			BlockNumber blockNum,
			bool *foundPtr)
{
	BufferTag	newTag;			/* identity of requested block */
	uint32		newHash;		/* hash value for newTag */
	LWLockId	newPartitionLock;		/* buffer partition lock for it */
	BufferTag	oldTag;			/* previous identity of selected buffer */
	uint32		oldHash;		/* hash value for oldTag */
	LWLockId	oldPartitionLock;		/* buffer partition lock for it */
	BufFlags	oldFlags;
	int			buf_id;
	volatile BufferDesc *buf;
	bool		valid;

	/* create a tag so we can lookup the buffer */
	INIT_BUFFERTAG(newTag, reln, blockNum);

	/* determine its hash code and partition lock ID */
	newHash = BufTableHashCode(&newTag);
	newPartitionLock = BufMappingPartitionLock(newHash);

	/* see if the block is in the buffer pool already */
	LWLockAcquire(newPartitionLock, LW_SHARED);
	buf_id = BufTableLookup(&newTag, newHash);
	if (buf_id >= 0)
	{
		/*
		 * Found it.  Now, pin the buffer so no one can steal it from the
		 * buffer pool, and check to see if the correct data has been loaded
		 * into the buffer.
		 */
		buf = &BufferDescriptors[buf_id];

		valid = PinBuffer(buf);

		/* Can release the mapping lock as soon as we've pinned it */
		LWLockRelease(newPartitionLock);

		*foundPtr = TRUE;

		if (!valid)
		{
			/*
			 * We can only get here if (a) someone else is still reading in
			 * the page, or (b) a previous read attempt failed.  We have to
			 * wait for any active read attempt to finish, and then set up our
			 * own read attempt if the page is still not BM_VALID.
			 * StartBufferIO does it all.
			 */
			if (StartBufferIO(buf, true))
			{
				/*
				 * If we get here, previous attempts to read the buffer must
				 * have failed ... but we shall bravely try again.
				 */
				*foundPtr = FALSE;
			}
		}

		return buf;
	}

	/*
	 * Didn't find it in the buffer pool.  We'll have to initialize a new
	 * buffer.	Remember to unlock the mapping lock while doing the work.
	 */
	LWLockRelease(newPartitionLock);

	/* Loop here in case we have to try another victim buffer */
	for (;;)
	{
		/*
		 * Select a victim buffer.	The buffer is returned with its header
		 * spinlock still held!  Also the BufFreelistLock is still held, since
		 * it would be bad to hold the spinlock while possibly waking up other
		 * processes.
		 */
		buf = StrategyGetBuffer();

		Assert(buf->refcount == 0);

		/* Must copy buffer flags while we still hold the spinlock */
		oldFlags = buf->flags;

		/* Pin the buffer and then release the buffer spinlock */
		PinBuffer_Locked(buf);

		/* Now it's safe to release the freelist lock */
		LWLockRelease(BufFreelistLock);

		/*
		 * If the buffer was dirty, try to write it out.  There is a race
		 * condition here, in that someone might dirty it after we released it
		 * above, or even while we are writing it out (since our share-lock
		 * won't prevent hint-bit updates).  We will recheck the dirty bit
		 * after re-locking the buffer header.
		 */
		if (oldFlags & BM_DIRTY)
		{
			/*
			 * We need a share-lock on the buffer contents to write it out
			 * (else we might write invalid data, eg because someone else is
			 * compacting the page contents while we write).  We must use a
			 * conditional lock acquisition here to avoid deadlock.  Even
			 * though the buffer was not pinned (and therefore surely not
			 * locked) when StrategyGetBuffer returned it, someone else could
			 * have pinned and exclusive-locked it by the time we get here. If
			 * we try to get the lock unconditionally, we'd block waiting for
			 * them; if they later block waiting for us, deadlock ensues.
			 * (This has been observed to happen when two backends are both
			 * trying to split btree index pages, and the second one just
			 * happens to be trying to split the page the first one got from
			 * StrategyGetBuffer.)
			 */
			if (LWLockConditionalAcquire(buf->content_lock, LW_SHARED))
			{
				FlushBuffer(buf, NULL);
				LWLockRelease(buf->content_lock);
			}
			else
			{
				/*
				 * Someone else has pinned the buffer, so give it up and loop
				 * back to get another one.
				 */
				UnpinBuffer(buf, true, false /* evidently recently used */ );
				continue;
			}
		}

		/*
		 * To change the association of a valid buffer, we'll need to have
		 * exclusive lock on both the old and new mapping partitions.
		 */
		if (oldFlags & BM_TAG_VALID)
		{
			/*
			 * Need to compute the old tag's hashcode and partition lock ID.
			 * XXX is it worth storing the hashcode in BufferDesc so we need
			 * not recompute it here?  Probably not.
			 */
			oldTag = buf->tag;
			oldHash = BufTableHashCode(&oldTag);
			oldPartitionLock = BufMappingPartitionLock(oldHash);

			/*
			 * Must lock the lower-numbered partition first to avoid
			 * deadlocks.
			 */
			if (oldPartitionLock < newPartitionLock)
			{
				LWLockAcquire(oldPartitionLock, LW_EXCLUSIVE);
				LWLockAcquire(newPartitionLock, LW_EXCLUSIVE);
			}
			else if (oldPartitionLock > newPartitionLock)
			{
				LWLockAcquire(newPartitionLock, LW_EXCLUSIVE);
				LWLockAcquire(oldPartitionLock, LW_EXCLUSIVE);
			}
			else
			{
				/* only one partition, only one lock */
				LWLockAcquire(newPartitionLock, LW_EXCLUSIVE);
			}
		}
		else
		{
			/* if it wasn't valid, we need only the new partition */
			LWLockAcquire(newPartitionLock, LW_EXCLUSIVE);
			/* these just keep the compiler quiet about uninit variables */
			oldHash = 0;
			oldPartitionLock = 0;
		}

		/*
		 * Try to make a hashtable entry for the buffer under its new tag.
		 * This could fail because while we were writing someone else
		 * allocated another buffer for the same block we want to read in.
		 * Note that we have not yet removed the hashtable entry for the old
		 * tag.
		 */
		buf_id = BufTableInsert(&newTag, newHash, buf->buf_id);

		if (buf_id >= 0)
		{
			/*
			 * Got a collision. Someone has already done what we were about to
			 * do. We'll just handle this as if it were found in the buffer
			 * pool in the first place.  First, give up the buffer we were
			 * planning to use.  Don't allow it to be thrown in the free list
			 * (we don't want to hold freelist and mapping locks at once).
			 */
			UnpinBuffer(buf, true, false);

			/* Can give up that buffer's mapping partition lock now */
			if ((oldFlags & BM_TAG_VALID) &&
				oldPartitionLock != newPartitionLock)
				LWLockRelease(oldPartitionLock);

			/* remaining code should match code at top of routine */

			buf = &BufferDescriptors[buf_id];

			valid = PinBuffer(buf);

			/* Can release the mapping lock as soon as we've pinned it */
			LWLockRelease(newPartitionLock);

			*foundPtr = TRUE;

			if (!valid)
			{
				/*
				 * We can only get here if (a) someone else is still reading
				 * in the page, or (b) a previous read attempt failed.	We
				 * have to wait for any active read attempt to finish, and
				 * then set up our own read attempt if the page is still not
				 * BM_VALID.  StartBufferIO does it all.
				 */
				if (StartBufferIO(buf, true))
				{
					/*
					 * If we get here, previous attempts to read the buffer
					 * must have failed ... but we shall bravely try again.
					 */
					*foundPtr = FALSE;
				}
			}

			return buf;
		}

		/*
		 * Need to lock the buffer header too in order to change its tag.
		 */
		LockBufHdr(buf);

		/*
		 * Somebody could have pinned or re-dirtied the buffer while we were
		 * doing the I/O and making the new hashtable entry.  If so, we can't
		 * recycle this buffer; we must undo everything we've done and start
		 * over with a new victim buffer.
		 */
		oldFlags = buf->flags;
		if (buf->refcount == 1 && !(oldFlags & BM_DIRTY))
			break;

		UnlockBufHdr(buf);
		BufTableDelete(&newTag, newHash);
		if ((oldFlags & BM_TAG_VALID) &&
			oldPartitionLock != newPartitionLock)
			LWLockRelease(oldPartitionLock);
		LWLockRelease(newPartitionLock);
		UnpinBuffer(buf, true, false /* evidently recently used */ );
	}

	/*
	 * Okay, it's finally safe to rename the buffer.
	 *
	 * Clearing BM_VALID here is necessary, clearing the dirtybits is just
	 * paranoia.  We also clear the usage_count since any recency of use of
	 * the old content is no longer relevant.
	 */
	buf->tag = newTag;
	buf->flags &= ~(BM_VALID | BM_DIRTY | BM_JUST_DIRTIED | BM_IO_ERROR);
	buf->flags |= BM_TAG_VALID;
	buf->usage_count = 0;

	UnlockBufHdr(buf);

	if (oldFlags & BM_TAG_VALID)
	{
		BufTableDelete(&oldTag, oldHash);
		if (oldPartitionLock != newPartitionLock)
			LWLockRelease(oldPartitionLock);
	}

	LWLockRelease(newPartitionLock);

	/*
	 * Buffer contents are currently invalid.  Try to get the io_in_progress
	 * lock.  If StartBufferIO returns false, then someone else managed to
	 * read it before we did, so there's nothing left for BufferAlloc() to do.
	 */
	if (StartBufferIO(buf, true))
		*foundPtr = FALSE;
	else
		*foundPtr = TRUE;

	return buf;
}

/*
 * InvalidateBuffer -- mark a shared buffer invalid and return it to the
 * freelist.
 *
 * The buffer header spinlock must be held at entry.  We drop it before
 * returning.  (This is sane because the caller must have locked the
 * buffer in order to be sure it should be dropped.)
 *
 * This is used only in contexts such as dropping a relation.  We assume
 * that no other backend could possibly be interested in using the page,
 * so the only reason the buffer might be pinned is if someone else is
 * trying to write it out.	We have to let them finish before we can
 * reclaim the buffer.
 *
 * The buffer could get reclaimed by someone else while we are waiting
 * to acquire the necessary locks; if so, don't mess it up.
 */
static void
InvalidateBuffer(volatile BufferDesc *buf)
{
	BufferTag	oldTag;
	uint32		oldHash;		/* hash value for oldTag */
	LWLockId	oldPartitionLock;		/* buffer partition lock for it */
	BufFlags	oldFlags;

	/* Save the original buffer tag before dropping the spinlock */
	oldTag = buf->tag;

	UnlockBufHdr(buf);

	/*
	 * Need to compute the old tag's hashcode and partition lock ID. XXX is it
	 * worth storing the hashcode in BufferDesc so we need not recompute it
	 * here?  Probably not.
	 */
	oldHash = BufTableHashCode(&oldTag);
	oldPartitionLock = BufMappingPartitionLock(oldHash);

retry:

	/*
	 * Acquire exclusive mapping lock in preparation for changing the buffer's
	 * association.
	 */
	LWLockAcquire(oldPartitionLock, LW_EXCLUSIVE);

	/* Re-lock the buffer header */
	LockBufHdr(buf);

	/* If it's changed while we were waiting for lock, do nothing */
	if (!BUFFERTAGS_EQUAL(buf->tag, oldTag))
	{
		UnlockBufHdr(buf);
		LWLockRelease(oldPartitionLock);
		return;
	}

	/*
	 * We assume the only reason for it to be pinned is that someone else is
	 * flushing the page out.  Wait for them to finish.  (This could be an
	 * infinite loop if the refcount is messed up... it would be nice to time
	 * out after awhile, but there seems no way to be sure how many loops may
	 * be needed.  Note that if the other guy has pinned the buffer but not
	 * yet done StartBufferIO, WaitIO will fall through and we'll effectively
	 * be busy-looping here.)
	 */
	if (buf->refcount != 0)
	{
		UnlockBufHdr(buf);
		LWLockRelease(oldPartitionLock);
		/* safety check: should definitely not be our *own* pin */
		if (PrivateRefCount[buf->buf_id] != 0)
			elog(ERROR, "buffer is pinned in InvalidateBuffer");
		WaitIO(buf);
		goto retry;
	}

	/*
	 * Clear out the buffer's tag and flags.  We must do this to ensure that
	 * linear scans of the buffer array don't think the buffer is valid.
	 */
	oldFlags = buf->flags;
	CLEAR_BUFFERTAG(buf->tag);
	buf->flags = 0;
	buf->usage_count = 0;

	UnlockBufHdr(buf);

	/*
	 * Remove the buffer from the lookup hashtable, if it was in there.
	 */
	if (oldFlags & BM_TAG_VALID)
		BufTableDelete(&oldTag, oldHash);

	/*
	 * Done with mapping lock.
	 */
	LWLockRelease(oldPartitionLock);

	/*
	 * Insert the buffer at the head of the list of free buffers.
	 */
	StrategyFreeBuffer(buf, true);
}

/*
 * MarkBufferDirty
 *
 *		Marks buffer contents as dirty (actual write happens later).
 *
 * Buffer must be pinned and exclusive-locked.	(If caller does not hold
 * exclusive lock, then somebody could be in process of writing the buffer,
 * leading to risk of bad data written to disk.)
 */
void
MarkBufferDirty(Buffer buffer)
{
	volatile BufferDesc *bufHdr;

	if (!BufferIsValid(buffer))
		elog(ERROR, "bad buffer id: %d", buffer);

	if (BufferIsLocal(buffer))
	{
		MarkLocalBufferDirty(buffer);
		return;
	}

	bufHdr = &BufferDescriptors[buffer - 1];

	Assert(PrivateRefCount[buffer - 1] > 0);
	/* unfortunately we can't check if the lock is held exclusively */
	Assert(LWLockHeldByMe(bufHdr->content_lock));

	LockBufHdr(bufHdr);

	Assert(bufHdr->refcount > 0);

	/*
	 * If the buffer was not dirty already, do vacuum cost accounting.
	 */
	if (!(bufHdr->flags & BM_DIRTY) && VacuumCostActive)
		VacuumCostBalance += VacuumCostPageDirty;

	bufHdr->flags |= (BM_DIRTY | BM_JUST_DIRTIED);

	UnlockBufHdr(bufHdr);
}

/*
 * ReleaseAndReadBuffer -- combine ReleaseBuffer() and ReadBuffer()
 *
 * Formerly, this saved one cycle of acquiring/releasing the BufMgrLock
 * compared to calling the two routines separately.  Now it's mainly just
 * a convenience function.	However, if the passed buffer is valid and
 * already contains the desired block, we just return it as-is; and that
 * does save considerable work compared to a full release and reacquire.
 *
 * Note: it is OK to pass buffer == InvalidBuffer, indicating that no old
 * buffer actually needs to be released.  This case is the same as ReadBuffer,
 * but can save some tests in the caller.
 */
Buffer
ReleaseAndReadBuffer(Buffer buffer,
					 Relation relation,
					 BlockNumber blockNum)
{
	volatile BufferDesc *bufHdr;

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
			LocalRefCount[-buffer - 1]--;
			if (LocalRefCount[-buffer - 1] == 0 &&
				bufHdr->usage_count < BM_MAX_USAGE_COUNT)
				bufHdr->usage_count++;
		}
		else
		{
			Assert(PrivateRefCount[buffer - 1] > 0);
			bufHdr = &BufferDescriptors[buffer - 1];
			/* we have pin, so it's ok to examine tag without spinlock */
			if (bufHdr->tag.blockNum == blockNum &&
				RelFileNodeEquals(bufHdr->tag.rnode, relation->rd_node))
				return buffer;
			UnpinBuffer(bufHdr, true, true);
		}
	}

	return ReadBuffer(relation, blockNum);
}

/*
 * PinBuffer -- make buffer unavailable for replacement.
 *
 * This should be applied only to shared buffers, never local ones.
 *
 * Note that ResourceOwnerEnlargeBuffers must have been done already.
 *
 * Returns TRUE if buffer is BM_VALID, else FALSE.	This provision allows
 * some callers to avoid an extra spinlock cycle.
 */
static bool
PinBuffer(volatile BufferDesc *buf)
{
	int			b = buf->buf_id;
	bool		result;

	if (PrivateRefCount[b] == 0)
	{
		LockBufHdr(buf);
		buf->refcount++;
		result = (buf->flags & BM_VALID) != 0;
		UnlockBufHdr(buf);
	}
	else
	{
		/* If we previously pinned the buffer, it must surely be valid */
		result = true;
	}
	PrivateRefCount[b]++;
	Assert(PrivateRefCount[b] > 0);
	ResourceOwnerRememberBuffer(CurrentResourceOwner,
								BufferDescriptorGetBuffer(buf));
	return result;
}

/*
 * PinBuffer_Locked -- as above, but caller already locked the buffer header.
 * The spinlock is released before return.
 *
 * Note: use of this routine is frequently mandatory, not just an optimization
 * to save a spin lock/unlock cycle, because we need to pin a buffer before
 * its state can change under us.
 */
static void
PinBuffer_Locked(volatile BufferDesc *buf)
{
	int			b = buf->buf_id;

	if (PrivateRefCount[b] == 0)
		buf->refcount++;
	UnlockBufHdr(buf);
	PrivateRefCount[b]++;
	Assert(PrivateRefCount[b] > 0);
	ResourceOwnerRememberBuffer(CurrentResourceOwner,
								BufferDescriptorGetBuffer(buf));
}

/*
 * UnpinBuffer -- make buffer available for replacement.
 *
 * This should be applied only to shared buffers, never local ones.
 *
 * Most but not all callers want CurrentResourceOwner to be adjusted.
 * Those that don't should pass fixOwner = FALSE.
 *
 * normalAccess indicates that we are finishing a "normal" page access,
 * that is, one requested by something outside the buffer subsystem.
 * Passing FALSE means it's an internal access that should not update the
 * buffer's usage count nor cause a change in the freelist.
 *
 * If we are releasing a buffer during VACUUM, and it's not been otherwise
 * used recently, and normalAccess is true, we send the buffer to the freelist.
 */
static void
UnpinBuffer(volatile BufferDesc *buf, bool fixOwner, bool normalAccess)
{
	int			b = buf->buf_id;

	if (fixOwner)
		ResourceOwnerForgetBuffer(CurrentResourceOwner,
								  BufferDescriptorGetBuffer(buf));

	Assert(PrivateRefCount[b] > 0);
	PrivateRefCount[b]--;
	if (PrivateRefCount[b] == 0)
	{
		bool		immed_free_buffer = false;

		/* I'd better not still hold any locks on the buffer */
		Assert(!LWLockHeldByMe(buf->content_lock));
		Assert(!LWLockHeldByMe(buf->io_in_progress_lock));

		LockBufHdr(buf);

		/* Decrement the shared reference count */
		Assert(buf->refcount > 0);
		buf->refcount--;

		/* Update buffer usage info, unless this is an internal access */
		if (normalAccess)
		{
			if (!strategy_hint_vacuum)
			{
				if (buf->usage_count < BM_MAX_USAGE_COUNT)
					buf->usage_count++;
			}
			else
			{
				/* VACUUM accesses don't bump usage count, instead... */
				if (buf->refcount == 0 && buf->usage_count == 0)
					immed_free_buffer = true;
			}
		}

		if ((buf->flags & BM_PIN_COUNT_WAITER) &&
			buf->refcount == 1)
		{
			/* we just released the last pin other than the waiter's */
			int			wait_backend_pid = buf->wait_backend_pid;

			buf->flags &= ~BM_PIN_COUNT_WAITER;
			UnlockBufHdr(buf);
			ProcSendSignal(wait_backend_pid);
		}
		else
			UnlockBufHdr(buf);

		/*
		 * If VACUUM is releasing an otherwise-unused buffer, send it to the
		 * freelist for near-term reuse.  We put it at the tail so that it
		 * won't be used before any invalid buffers that may exist.
		 */
		if (immed_free_buffer)
			StrategyFreeBuffer(buf, false);
	}
}

/*
 * BufferSync -- Write out all dirty buffers in the pool.
 *
 * This is called at checkpoint time to write out all dirty shared buffers.
 */
void
BufferSync(void)
{
	int			buf_id;
	int			num_to_scan;
	int			absorb_counter;

	/*
	 * Find out where to start the circular scan.
	 */
	buf_id = StrategySyncStart();

	/* Make sure we can handle the pin inside SyncOneBuffer */
	ResourceOwnerEnlargeBuffers(CurrentResourceOwner);

	/*
	 * Loop over all buffers.
	 */
	num_to_scan = NBuffers;
	absorb_counter = WRITES_PER_ABSORB;
	while (num_to_scan-- > 0)
	{
		if (SyncOneBuffer(buf_id, false))
		{
			/*
			 * If in bgwriter, absorb pending fsync requests after each
			 * WRITES_PER_ABSORB write operations, to prevent overflow of the
			 * fsync request queue.  If not in bgwriter process, this is a
			 * no-op.
			 */
			if (--absorb_counter <= 0)
			{
				AbsorbFsyncRequests();
				absorb_counter = WRITES_PER_ABSORB;
			}
		}
		if (++buf_id >= NBuffers)
			buf_id = 0;
	}
}

/*
 * BgBufferSync -- Write out some dirty buffers in the pool.
 *
 * This is called periodically by the background writer process.
 */
void
BgBufferSync(void)
{
	static int	buf_id1 = 0;
	int			buf_id2;
	int			num_to_scan;
	int			num_written;

	/* Make sure we can handle the pin inside SyncOneBuffer */
	ResourceOwnerEnlargeBuffers(CurrentResourceOwner);

	/*
	 * To minimize work at checkpoint time, we want to try to keep all the
	 * buffers clean; this motivates a scan that proceeds sequentially through
	 * all buffers.  But we are also charged with ensuring that buffers that
	 * will be recycled soon are clean when needed; these buffers are the ones
	 * just ahead of the StrategySyncStart point.  We make a separate scan
	 * through those.
	 */

	/*
	 * This loop runs over all buffers, including pinned ones.	The starting
	 * point advances through the buffer pool on successive calls.
	 *
	 * Note that we advance the static counter *before* trying to write. This
	 * ensures that, if we have a persistent write failure on a dirty buffer,
	 * we'll still be able to make progress writing other buffers. (The
	 * bgwriter will catch the error and just call us again later.)
	 */
	if (bgwriter_all_percent > 0.0 && bgwriter_all_maxpages > 0)
	{
		num_to_scan = (int) ((NBuffers * bgwriter_all_percent + 99) / 100);
		num_written = 0;

		while (num_to_scan-- > 0)
		{
			if (++buf_id1 >= NBuffers)
				buf_id1 = 0;
			if (SyncOneBuffer(buf_id1, false))
			{
				if (++num_written >= bgwriter_all_maxpages)
					break;
			}
		}
	}

	/*
	 * This loop considers only unpinned buffers close to the clock sweep
	 * point.
	 */
	if (bgwriter_lru_percent > 0.0 && bgwriter_lru_maxpages > 0)
	{
		num_to_scan = (int) ((NBuffers * bgwriter_lru_percent + 99) / 100);
		num_written = 0;

		buf_id2 = StrategySyncStart();

		while (num_to_scan-- > 0)
		{
			if (SyncOneBuffer(buf_id2, true))
			{
				if (++num_written >= bgwriter_lru_maxpages)
					break;
			}
			if (++buf_id2 >= NBuffers)
				buf_id2 = 0;
		}
	}
}

/*
 * SyncOneBuffer -- process a single buffer during syncing.
 *
 * If skip_pinned is true, we don't write currently-pinned buffers, nor
 * buffers marked recently used, as these are not replacement candidates.
 *
 * Returns true if buffer was written, else false.	(This could be in error
 * if FlushBuffers finds the buffer clean after locking it, but we don't
 * care all that much.)
 *
 * Note: caller must have done ResourceOwnerEnlargeBuffers.
 */
static bool
SyncOneBuffer(int buf_id, bool skip_pinned)
{
	volatile BufferDesc *bufHdr = &BufferDescriptors[buf_id];

	/*
	 * Check whether buffer needs writing.
	 *
	 * We can make this check without taking the buffer content lock so long
	 * as we mark pages dirty in access methods *before* logging changes with
	 * XLogInsert(): if someone marks the buffer dirty just after our check we
	 * don't worry because our checkpoint.redo points before log record for
	 * upcoming changes and so we are not required to write such dirty buffer.
	 */
	LockBufHdr(bufHdr);
	if (!(bufHdr->flags & BM_VALID) || !(bufHdr->flags & BM_DIRTY))
	{
		UnlockBufHdr(bufHdr);
		return false;
	}
	if (skip_pinned &&
		(bufHdr->refcount != 0 || bufHdr->usage_count != 0))
	{
		UnlockBufHdr(bufHdr);
		return false;
	}

	/*
	 * Pin it, share-lock it, write it.  (FlushBuffer will do nothing if the
	 * buffer is clean by the time we've locked it.)
	 */
	PinBuffer_Locked(bufHdr);
	LWLockAcquire(bufHdr->content_lock, LW_SHARED);

	FlushBuffer(bufHdr, NULL);

	LWLockRelease(bufHdr->content_lock);
	UnpinBuffer(bufHdr, true, false /* don't change freelist */ );

	return true;
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
 *		As of PostgreSQL 8.0, buffer pins should get released by the
 *		ResourceOwner mechanism.  This routine is just a debugging
 *		cross-check that no pins remain.
 */
void
AtEOXact_Buffers(bool isCommit)
{
#ifdef USE_ASSERT_CHECKING
	if (assert_enabled)
	{
		int			i;

		for (i = 0; i < NBuffers; i++)
		{
			Assert(PrivateRefCount[i] == 0);
		}
	}
#endif

	AtEOXact_LocalBuffers(isCommit);
}

/*
 * InitBufferPoolBackend --- second-stage initialization of a new backend
 *
 * This is called after we have acquired a PGPROC and so can safely get
 * LWLocks.  We don't currently need to do anything at this stage ...
 * except register a shmem-exit callback.  AtProcExit_Buffers needs LWLock
 * access, and thereby has to be called at the corresponding phase of
 * backend shutdown.
 */
void
InitBufferPoolBackend(void)
{
	on_shmem_exit(AtProcExit_Buffers, 0);
}

/*
 * Ensure we have released all shared-buffer locks and pins during backend exit
 */
static void
AtProcExit_Buffers(int code, Datum arg)
{
	int			i;

	AbortBufferIO();
	UnlockBuffers();

	for (i = 0; i < NBuffers; i++)
	{
		if (PrivateRefCount[i] != 0)
		{
			volatile BufferDesc *buf = &(BufferDescriptors[i]);

			/*
			 * We don't worry about updating ResourceOwner; if we even got
			 * here, it suggests that ResourceOwners are messed up.
			 */
			PrivateRefCount[i] = 1;		/* make sure we release shared pin */
			UnpinBuffer(buf, false, false /* don't change freelist */ );
			Assert(PrivateRefCount[i] == 0);
		}
	}

	/* localbuf.c needs a chance too */
	AtProcExit_LocalBuffers();
}

/*
 * Helper routine to issue warnings when a buffer is unexpectedly pinned
 */
void
PrintBufferLeakWarning(Buffer buffer)
{
	volatile BufferDesc *buf;
	int32		loccount;

	Assert(BufferIsValid(buffer));
	if (BufferIsLocal(buffer))
	{
		buf = &LocalBufferDescriptors[-buffer - 1];
		loccount = LocalRefCount[-buffer - 1];
	}
	else
	{
		buf = &BufferDescriptors[buffer - 1];
		loccount = PrivateRefCount[buffer - 1];
	}

	/* theoretically we should lock the bufhdr here */
	elog(WARNING,
		 "buffer refcount leak: [%03d] "
		 "(rel=%u/%u/%u, blockNum=%u, flags=0x%x, refcount=%u %d)",
		 buffer,
		 buf->tag.rnode.spcNode, buf->tag.rnode.dbNode,
		 buf->tag.rnode.relNode,
		 buf->tag.blockNum, buf->flags,
		 buf->refcount, loccount);
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
	BufferSync();
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
	volatile BufferDesc *bufHdr;

	Assert(BufferIsPinned(buffer));

	if (BufferIsLocal(buffer))
		bufHdr = &(LocalBufferDescriptors[-buffer - 1]);
	else
		bufHdr = &BufferDescriptors[buffer - 1];

	/* pinned, so OK to read tag without spinlock */
	return bufHdr->tag.blockNum;
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
	volatile BufferDesc *bufHdr;

	if (BufferIsLocal(buffer))
		bufHdr = &(LocalBufferDescriptors[-buffer - 1]);
	else
		bufHdr = &BufferDescriptors[buffer - 1];

	return bufHdr->tag.rnode;
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
 * The caller must hold a pin on the buffer and have share-locked the
 * buffer contents.  (Note: a share-lock does not prevent updates of
 * hint bits in the buffer, so the page could change while the write
 * is in progress, but we assume that that will not invalidate the data
 * written.)
 *
 * If the caller has an smgr reference for the buffer's relation, pass it
 * as the second parameter.  If not, pass NULL.
 */
static void
FlushBuffer(volatile BufferDesc *buf, SMgrRelation reln)
{
	XLogRecPtr	recptr;
	ErrorContextCallback errcontext;

	/*
	 * Acquire the buffer's io_in_progress lock.  If StartBufferIO returns
	 * false, then someone else flushed the buffer before we could, so we need
	 * not do anything.
	 */
	if (!StartBufferIO(buf, false))
		return;

	/* Setup error traceback support for ereport() */
	errcontext.callback = buffer_write_error_callback;
	errcontext.arg = (void *) buf;
	errcontext.previous = error_context_stack;
	error_context_stack = &errcontext;

	/* Find smgr relation for buffer */
	if (reln == NULL)
		reln = smgropen(buf->tag.rnode);

	/*
	 * Force XLOG flush up to buffer's LSN.  This implements the basic WAL
	 * rule that log updates must hit disk before any of the data-file changes
	 * they describe do.
	 */
	recptr = BufferGetLSN(buf);
	XLogFlush(recptr);

	/*
	 * Now it's safe to write buffer to disk. Note that no one else should
	 * have been able to write it while we were busy with log flushing because
	 * we have the io_in_progress lock.
	 */

	/* To check if block content changes while flushing. - vadim 01/17/97 */
	LockBufHdr(buf);
	buf->flags &= ~BM_JUST_DIRTIED;
	UnlockBufHdr(buf);

	smgrwrite(reln,
			  buf->tag.blockNum,
			  (char *) BufHdrGetBlock(buf),
			  false);

	BufferFlushCount++;

	/*
	 * Mark the buffer as clean (unless BM_JUST_DIRTIED has become set) and
	 * end the io_in_progress state.
	 */
	TerminateBufferIO(buf, true, 0);

	/* Pop the error context stack */
	error_context_stack = errcontext.previous;
}

/*
 * RelationGetNumberOfBlocks
 *		Determines the current number of pages in the relation.
 */
BlockNumber
RelationGetNumberOfBlocks(Relation relation)
{
	/* Open it at the smgr level if not already done */
	RelationOpenSmgr(relation);

	return smgrnblocks(relation->rd_smgr);
}

/*
 * RelationTruncate
 *		Physically truncate a relation to the specified number of blocks.
 *
 * As of Postgres 8.1, this includes getting rid of any buffers for the
 * blocks that are to be dropped; previously, callers had to do that.
 */
void
RelationTruncate(Relation rel, BlockNumber nblocks)
{
	/* Open it at the smgr level if not already done */
	RelationOpenSmgr(rel);

	/* Make sure rd_targblock isn't pointing somewhere past end */
	rel->rd_targblock = InvalidBlockNumber;

	/* Do the real work */
	smgrtruncate(rel->rd_smgr, nblocks, rel->rd_istemp);
}

/* ---------------------------------------------------------------------
 *		DropRelFileNodeBuffers
 *
 *		This function removes from the buffer pool all the pages of the
 *		specified relation that have block numbers >= firstDelBlock.
 *		(In particular, with firstDelBlock = 0, all pages are removed.)
 *		Dirty pages are simply dropped, without bothering to write them
 *		out first.	Therefore, this is NOT rollback-able, and so should be
 *		used only with extreme caution!
 *
 *		Currently, this is called only from smgr.c when the underlying file
 *		is about to be deleted or truncated (firstDelBlock is needed for
 *		the truncation case).  The data in the affected pages would therefore
 *		be deleted momentarily anyway, and there is no point in writing it.
 *		It is the responsibility of higher-level code to ensure that the
 *		deletion or truncation does not lose any data that could be needed
 *		later.	It is also the responsibility of higher-level code to ensure
 *		that no other process could be trying to load more pages of the
 *		relation into buffers.
 *
 *		XXX currently it sequentially searches the buffer pool, should be
 *		changed to more clever ways of searching.  However, this routine
 *		is used only in code paths that aren't very performance-critical,
 *		and we shouldn't slow down the hot paths to make it faster ...
 * --------------------------------------------------------------------
 */
void
DropRelFileNodeBuffers(RelFileNode rnode, bool istemp,
					   BlockNumber firstDelBlock)
{
	int			i;

	if (istemp)
	{
		DropRelFileNodeLocalBuffers(rnode, firstDelBlock);
		return;
	}

	for (i = 0; i < NBuffers; i++)
	{
		volatile BufferDesc *bufHdr = &BufferDescriptors[i];

		LockBufHdr(bufHdr);
		if (RelFileNodeEquals(bufHdr->tag.rnode, rnode) &&
			bufHdr->tag.blockNum >= firstDelBlock)
			InvalidateBuffer(bufHdr);	/* releases spinlock */
		else
			UnlockBufHdr(bufHdr);
	}
}

/* ---------------------------------------------------------------------
 *		DropDatabaseBuffers
 *
 *		This function removes all the buffers in the buffer cache for a
 *		particular database.  Dirty pages are simply dropped, without
 *		bothering to write them out first.	This is used when we destroy a
 *		database, to avoid trying to flush data to disk when the directory
 *		tree no longer exists.	Implementation is pretty similar to
 *		DropRelFileNodeBuffers() which is for destroying just one relation.
 * --------------------------------------------------------------------
 */
void
DropDatabaseBuffers(Oid dbid)
{
	int			i;
	volatile BufferDesc *bufHdr;

	/*
	 * We needn't consider local buffers, since by assumption the target
	 * database isn't our own.
	 */

	for (i = 0; i < NBuffers; i++)
	{
		bufHdr = &BufferDescriptors[i];
		LockBufHdr(bufHdr);
		if (bufHdr->tag.rnode.dbNode == dbid)
			InvalidateBuffer(bufHdr);	/* releases spinlock */
		else
			UnlockBufHdr(bufHdr);
	}
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
	volatile BufferDesc *buf = BufferDescriptors;

	for (i = 0; i < NBuffers; ++i, ++buf)
	{
		/* theoretically we should lock the bufhdr here */
		elog(LOG,
			 "[%02d] (freeNext=%d, rel=%u/%u/%u, "
			 "blockNum=%u, flags=0x%x, refcount=%u %d)",
			 i, buf->freeNext,
			 buf->tag.rnode.spcNode, buf->tag.rnode.dbNode,
			 buf->tag.rnode.relNode,
			 buf->tag.blockNum, buf->flags,
			 buf->refcount, PrivateRefCount[i]);
	}
}
#endif

#ifdef NOT_USED
void
PrintPinnedBufs(void)
{
	int			i;
	volatile BufferDesc *buf = BufferDescriptors;

	for (i = 0; i < NBuffers; ++i, ++buf)
	{
		if (PrivateRefCount[i] > 0)
		{
			/* theoretically we should lock the bufhdr here */
			elog(LOG,
				 "[%02d] (freeNext=%d, rel=%u/%u/%u, "
				 "blockNum=%u, flags=0x%x, refcount=%u %d)",
				 i, buf->freeNext,
				 buf->tag.rnode.spcNode, buf->tag.rnode.dbNode,
				 buf->tag.rnode.relNode,
				 buf->tag.blockNum, buf->flags,
				 buf->refcount, PrivateRefCount[i]);
		}
	}
}
#endif

/* ---------------------------------------------------------------------
 *		FlushRelationBuffers
 *
 *		This function writes all dirty pages of a relation out to disk
 *		(or more accurately, out to kernel disk buffers), ensuring that the
 *		kernel has an up-to-date view of the relation.
 *
 *		Generally, the caller should be holding AccessExclusiveLock on the
 *		target relation to ensure that no other backend is busy dirtying
 *		more blocks of the relation; the effects can't be expected to last
 *		after the lock is released.
 *
 *		XXX currently it sequentially searches the buffer pool, should be
 *		changed to more clever ways of searching.  This routine is not
 *		used in any performance-critical code paths, so it's not worth
 *		adding additional overhead to normal paths to make it go faster;
 *		but see also DropRelFileNodeBuffers.
 * --------------------------------------------------------------------
 */
void
FlushRelationBuffers(Relation rel)
{
	int			i;
	volatile BufferDesc *bufHdr;

	/* Open rel at the smgr level if not already done */
	RelationOpenSmgr(rel);

	if (rel->rd_istemp)
	{
		for (i = 0; i < NLocBuffer; i++)
		{
			bufHdr = &LocalBufferDescriptors[i];
			if (RelFileNodeEquals(bufHdr->tag.rnode, rel->rd_node) &&
				(bufHdr->flags & BM_VALID) && (bufHdr->flags & BM_DIRTY))
			{
				ErrorContextCallback errcontext;

				/* Setup error traceback support for ereport() */
				errcontext.callback = buffer_write_error_callback;
				errcontext.arg = (void *) bufHdr;
				errcontext.previous = error_context_stack;
				error_context_stack = &errcontext;

				smgrwrite(rel->rd_smgr,
						  bufHdr->tag.blockNum,
						  (char *) LocalBufHdrGetBlock(bufHdr),
						  true);

				bufHdr->flags &= ~(BM_DIRTY | BM_JUST_DIRTIED);

				/* Pop the error context stack */
				error_context_stack = errcontext.previous;
			}
		}

		return;
	}

	/* Make sure we can handle the pin inside the loop */
	ResourceOwnerEnlargeBuffers(CurrentResourceOwner);

	for (i = 0; i < NBuffers; i++)
	{
		bufHdr = &BufferDescriptors[i];
		LockBufHdr(bufHdr);
		if (RelFileNodeEquals(bufHdr->tag.rnode, rel->rd_node) &&
			(bufHdr->flags & BM_VALID) && (bufHdr->flags & BM_DIRTY))
		{
			PinBuffer_Locked(bufHdr);
			LWLockAcquire(bufHdr->content_lock, LW_SHARED);
			FlushBuffer(bufHdr, rel->rd_smgr);
			LWLockRelease(bufHdr->content_lock);
			UnpinBuffer(bufHdr, true, false /* no freelist change */ );
		}
		else
			UnlockBufHdr(bufHdr);
	}
}

/*
 * ReleaseBuffer -- release the pin on a buffer
 */
void
ReleaseBuffer(Buffer buffer)
{
	volatile BufferDesc *bufHdr;

	if (!BufferIsValid(buffer))
		elog(ERROR, "bad buffer id: %d", buffer);

	ResourceOwnerForgetBuffer(CurrentResourceOwner, buffer);

	if (BufferIsLocal(buffer))
	{
		Assert(LocalRefCount[-buffer - 1] > 0);
		bufHdr = &LocalBufferDescriptors[-buffer - 1];
		LocalRefCount[-buffer - 1]--;
		if (LocalRefCount[-buffer - 1] == 0 &&
			bufHdr->usage_count < BM_MAX_USAGE_COUNT)
			bufHdr->usage_count++;
		return;
	}

	bufHdr = &BufferDescriptors[buffer - 1];

	Assert(PrivateRefCount[buffer - 1] > 0);

	if (PrivateRefCount[buffer - 1] > 1)
		PrivateRefCount[buffer - 1]--;
	else
		UnpinBuffer(bufHdr, false, true);
}

/*
 * UnlockReleaseBuffer -- release the content lock and pin on a buffer
 *
 * This is just a shorthand for a common combination.
 */
void
UnlockReleaseBuffer(Buffer buffer)
{
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buffer);
}

/*
 * IncrBufferRefCount
 *		Increment the pin count on a buffer that we have *already* pinned
 *		at least once.
 *
 *		This function cannot be used on a buffer we do not have pinned,
 *		because it doesn't change the shared buffer state.
 */
void
IncrBufferRefCount(Buffer buffer)
{
	Assert(BufferIsPinned(buffer));
	ResourceOwnerEnlargeBuffers(CurrentResourceOwner);
	ResourceOwnerRememberBuffer(CurrentResourceOwner, buffer);
	if (BufferIsLocal(buffer))
		LocalRefCount[-buffer - 1]++;
	else
		PrivateRefCount[buffer - 1]++;
}

/*
 * SetBufferCommitInfoNeedsSave
 *
 *	Mark a buffer dirty when we have updated tuple commit-status bits in it.
 *
 * This is essentially the same as MarkBufferDirty, except that the caller
 * might have only share-lock instead of exclusive-lock on the buffer's
 * content lock.  We preserve the distinction mainly as a way of documenting
 * that the caller has not made a critical data change --- the status-bit
 * update could be redone by someone else just as easily.  Therefore, no WAL
 * log record need be generated, whereas calls to MarkBufferDirty really ought
 * to be associated with a WAL-entry-creating action.
 */
void
SetBufferCommitInfoNeedsSave(Buffer buffer)
{
	volatile BufferDesc *bufHdr;

	if (!BufferIsValid(buffer))
		elog(ERROR, "bad buffer id: %d", buffer);

	if (BufferIsLocal(buffer))
	{
		MarkLocalBufferDirty(buffer);
		return;
	}

	bufHdr = &BufferDescriptors[buffer - 1];

	Assert(PrivateRefCount[buffer - 1] > 0);
	/* here, either share or exclusive lock is OK */
	Assert(LWLockHeldByMe(bufHdr->content_lock));

	/*
	 * This routine might get called many times on the same page, if we are
	 * making the first scan after commit of an xact that added/deleted many
	 * tuples.	So, be as quick as we can if the buffer is already dirty.  We
	 * do this by not acquiring spinlock if it looks like the status bits are
	 * already OK.	(Note it is okay if someone else clears BM_JUST_DIRTIED
	 * immediately after we look, because the buffer content update is already
	 * done and will be reflected in the I/O.)
	 */
	if ((bufHdr->flags & (BM_DIRTY | BM_JUST_DIRTIED)) !=
		(BM_DIRTY | BM_JUST_DIRTIED))
	{
		LockBufHdr(bufHdr);
		Assert(bufHdr->refcount > 0);
		if (!(bufHdr->flags & BM_DIRTY) && VacuumCostActive)
			VacuumCostBalance += VacuumCostPageDirty;
		bufHdr->flags |= (BM_DIRTY | BM_JUST_DIRTIED);
		UnlockBufHdr(bufHdr);
	}
}

/*
 * Release buffer content locks for shared buffers.
 *
 * Used to clean up after errors.
 *
 * Currently, we can expect that lwlock.c's LWLockReleaseAll() took care
 * of releasing buffer content locks per se; the only thing we need to deal
 * with here is clearing any PIN_COUNT request that was in progress.
 */
void
UnlockBuffers(void)
{
	volatile BufferDesc *buf = PinCountWaitBuf;

	if (buf)
	{
		LockBufHdr(buf);

		/*
		 * Don't complain if flag bit not set; it could have been reset but we
		 * got a cancel/die interrupt before getting the signal.
		 */
		if ((buf->flags & BM_PIN_COUNT_WAITER) != 0 &&
			buf->wait_backend_pid == MyProcPid)
			buf->flags &= ~BM_PIN_COUNT_WAITER;

		UnlockBufHdr(buf);

		PinCountWaitBuf = NULL;
	}
}

/*
 * Acquire or release the content_lock for the buffer.
 */
void
LockBuffer(Buffer buffer, int mode)
{
	volatile BufferDesc *buf;

	Assert(BufferIsValid(buffer));
	if (BufferIsLocal(buffer))
		return;					/* local buffers need no lock */

	buf = &(BufferDescriptors[buffer - 1]);

	if (mode == BUFFER_LOCK_UNLOCK)
		LWLockRelease(buf->content_lock);
	else if (mode == BUFFER_LOCK_SHARE)
		LWLockAcquire(buf->content_lock, LW_SHARED);
	else if (mode == BUFFER_LOCK_EXCLUSIVE)
		LWLockAcquire(buf->content_lock, LW_EXCLUSIVE);
	else
		elog(ERROR, "unrecognized buffer lock mode: %d", mode);
}

/*
 * Acquire the content_lock for the buffer, but only if we don't have to wait.
 *
 * This assumes the caller wants BUFFER_LOCK_EXCLUSIVE mode.
 */
bool
ConditionalLockBuffer(Buffer buffer)
{
	volatile BufferDesc *buf;

	Assert(BufferIsValid(buffer));
	if (BufferIsLocal(buffer))
		return true;			/* act as though we got it */

	buf = &(BufferDescriptors[buffer - 1]);

	return LWLockConditionalAcquire(buf->content_lock, LW_EXCLUSIVE);
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
	volatile BufferDesc *bufHdr;

	Assert(BufferIsValid(buffer));
	Assert(PinCountWaitBuf == NULL);

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

	for (;;)
	{
		/* Try to acquire lock */
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		LockBufHdr(bufHdr);
		Assert(bufHdr->refcount > 0);
		if (bufHdr->refcount == 1)
		{
			/* Successfully acquired exclusive lock with pincount 1 */
			UnlockBufHdr(bufHdr);
			return;
		}
		/* Failed, so mark myself as waiting for pincount 1 */
		if (bufHdr->flags & BM_PIN_COUNT_WAITER)
		{
			UnlockBufHdr(bufHdr);
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			elog(ERROR, "multiple backends attempting to wait for pincount 1");
		}
		bufHdr->wait_backend_pid = MyProcPid;
		bufHdr->flags |= BM_PIN_COUNT_WAITER;
		PinCountWaitBuf = bufHdr;
		UnlockBufHdr(bufHdr);
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		/* Wait to be signaled by UnpinBuffer() */
		ProcWaitForSignal();
		PinCountWaitBuf = NULL;
		/* Loop back and try again */
	}
}

/*
 *	Functions for buffer I/O handling
 *
 *	Note: We assume that nested buffer I/O never occurs.
 *	i.e at most one io_in_progress lock is held per proc.
 *
 *	Also note that these are used only for shared buffers, not local ones.
 */

/*
 * WaitIO -- Block until the IO_IN_PROGRESS flag on 'buf' is cleared.
 */
static void
WaitIO(volatile BufferDesc *buf)
{
	/*
	 * Changed to wait until there's no IO - Inoue 01/13/2000
	 *
	 * Note this is *necessary* because an error abort in the process doing
	 * I/O could release the io_in_progress_lock prematurely. See
	 * AbortBufferIO.
	 */
	for (;;)
	{
		BufFlags	sv_flags;

		/*
		 * It may not be necessary to acquire the spinlock to check the flag
		 * here, but since this test is essential for correctness, we'd better
		 * play it safe.
		 */
		LockBufHdr(buf);
		sv_flags = buf->flags;
		UnlockBufHdr(buf);
		if (!(sv_flags & BM_IO_IN_PROGRESS))
			break;
		LWLockAcquire(buf->io_in_progress_lock, LW_SHARED);
		LWLockRelease(buf->io_in_progress_lock);
	}
}

/*
 * StartBufferIO: begin I/O on this buffer
 *	(Assumptions)
 *	My process is executing no IO
 *	The buffer is Pinned
 *
 * In some scenarios there are race conditions in which multiple backends
 * could attempt the same I/O operation concurrently.  If someone else
 * has already started I/O on this buffer then we will block on the
 * io_in_progress lock until he's done.
 *
 * Input operations are only attempted on buffers that are not BM_VALID,
 * and output operations only on buffers that are BM_VALID and BM_DIRTY,
 * so we can always tell if the work is already done.
 *
 * Returns TRUE if we successfully marked the buffer as I/O busy,
 * FALSE if someone else already did the work.
 */
static bool
StartBufferIO(volatile BufferDesc *buf, bool forInput)
{
	Assert(!InProgressBuf);

	for (;;)
	{
		/*
		 * Grab the io_in_progress lock so that other processes can wait for
		 * me to finish the I/O.
		 */
		LWLockAcquire(buf->io_in_progress_lock, LW_EXCLUSIVE);

		LockBufHdr(buf);

		if (!(buf->flags & BM_IO_IN_PROGRESS))
			break;

		/*
		 * The only way BM_IO_IN_PROGRESS could be set when the io_in_progress
		 * lock isn't held is if the process doing the I/O is recovering from
		 * an error (see AbortBufferIO).  If that's the case, we must wait for
		 * him to get unwedged.
		 */
		UnlockBufHdr(buf);
		LWLockRelease(buf->io_in_progress_lock);
		WaitIO(buf);
	}

	/* Once we get here, there is definitely no I/O active on this buffer */

	if (forInput ? (buf->flags & BM_VALID) : !(buf->flags & BM_DIRTY))
	{
		/* someone else already did the I/O */
		UnlockBufHdr(buf);
		LWLockRelease(buf->io_in_progress_lock);
		return false;
	}

	buf->flags |= BM_IO_IN_PROGRESS;

	UnlockBufHdr(buf);

	InProgressBuf = buf;
	IsForInput = forInput;

	return true;
}

/*
 * TerminateBufferIO: release a buffer we were doing I/O on
 *	(Assumptions)
 *	My process is executing IO for the buffer
 *	BM_IO_IN_PROGRESS bit is set for the buffer
 *	We hold the buffer's io_in_progress lock
 *	The buffer is Pinned
 *
 * If clear_dirty is TRUE and BM_JUST_DIRTIED is not set, we clear the
 * buffer's BM_DIRTY flag.  This is appropriate when terminating a
 * successful write.  The check on BM_JUST_DIRTIED is necessary to avoid
 * marking the buffer clean if it was re-dirtied while we were writing.
 *
 * set_flag_bits gets ORed into the buffer's flags.  It must include
 * BM_IO_ERROR in a failure case.  For successful completion it could
 * be 0, or BM_VALID if we just finished reading in the page.
 */
static void
TerminateBufferIO(volatile BufferDesc *buf, bool clear_dirty,
				  int set_flag_bits)
{
	Assert(buf == InProgressBuf);

	LockBufHdr(buf);

	Assert(buf->flags & BM_IO_IN_PROGRESS);
	buf->flags &= ~(BM_IO_IN_PROGRESS | BM_IO_ERROR);
	if (clear_dirty && !(buf->flags & BM_JUST_DIRTIED))
		buf->flags &= ~BM_DIRTY;
	buf->flags |= set_flag_bits;

	UnlockBufHdr(buf);

	InProgressBuf = NULL;

	LWLockRelease(buf->io_in_progress_lock);
}

/*
 * AbortBufferIO: Clean up any active buffer I/O after an error.
 *
 *	All LWLocks we might have held have been released,
 *	but we haven't yet released buffer pins, so the buffer is still pinned.
 *
 *	If I/O was in progress, we always set BM_IO_ERROR, even though it's
 *	possible the error condition wasn't related to the I/O.
 */
void
AbortBufferIO(void)
{
	volatile BufferDesc *buf = InProgressBuf;

	if (buf)
	{
		/*
		 * Since LWLockReleaseAll has already been called, we're not holding
		 * the buffer's io_in_progress_lock. We have to re-acquire it so that
		 * we can use TerminateBufferIO. Anyone who's executing WaitIO on the
		 * buffer will be in a busy spin until we succeed in doing this.
		 */
		LWLockAcquire(buf->io_in_progress_lock, LW_EXCLUSIVE);

		LockBufHdr(buf);
		Assert(buf->flags & BM_IO_IN_PROGRESS);
		if (IsForInput)
		{
			Assert(!(buf->flags & BM_DIRTY));
			/* We'd better not think buffer is valid yet */
			Assert(!(buf->flags & BM_VALID));
			UnlockBufHdr(buf);
		}
		else
		{
			BufFlags	sv_flags;

			sv_flags = buf->flags;
			Assert(sv_flags & BM_DIRTY);
			UnlockBufHdr(buf);
			/* Issue notice if this is not the first failure... */
			if (sv_flags & BM_IO_ERROR)
			{
				/* Buffer is pinned, so we can read tag without spinlock */
				ereport(WARNING,
						(errcode(ERRCODE_IO_ERROR),
						 errmsg("could not write block %u of %u/%u/%u",
								buf->tag.blockNum,
								buf->tag.rnode.spcNode,
								buf->tag.rnode.dbNode,
								buf->tag.rnode.relNode),
						 errdetail("Multiple failures --- write error may be permanent.")));
			}
		}
		TerminateBufferIO(buf, false, BM_IO_ERROR);
	}
}

/*
 * Error context callback for errors occurring during buffer writes.
 */
static void
buffer_write_error_callback(void *arg)
{
	volatile BufferDesc *bufHdr = (volatile BufferDesc *) arg;

	/* Buffer is pinned, so we can read the tag without locking the spinlock */
	if (bufHdr != NULL)
		errcontext("writing block %u of relation %u/%u/%u",
				   bufHdr->tag.blockNum,
				   bufHdr->tag.rnode.spcNode,
				   bufHdr->tag.rnode.dbNode,
				   bufHdr->tag.rnode.relNode);
}
