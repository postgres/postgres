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
 *	  $Header: /cvsroot/pgsql/src/backend/storage/buffer/bufmgr.c,v 1.141.2.1 2003/12/01 16:53:30 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 *
 * BufferAlloc() -- lookup a buffer in the buffer table.  If
 *		it isn't there add it, but do not read data into memory.
 *		This is used when we are about to reinitialize the
 *		buffer so don't care what the current disk contents are.
 *		BufferAlloc() also pins the new buffer in memory.
 *
 * ReadBuffer() -- like BufferAlloc() but reads the data
 *		on a buffer cache miss.
 *
 * ReleaseBuffer() -- unpin the buffer
 *
 * WriteNoReleaseBuffer() -- mark the buffer contents as "dirty"
 *		but don't unpin.  The disk IO is delayed until buffer
 *		replacement.
 *
 * WriteBuffer() -- WriteNoReleaseBuffer() + ReleaseBuffer()
 *
 * BufferSync() -- flush all dirty buffers in the buffer pool.
 *
 * InitBufferPool() -- Init the buffer module.
 *
 * See other files:
 *		freelist.c -- chooses victim for buffer replacement
 *		buf_table.c -- manages the buffer lookup table
 */
#include "postgres.h"

#include <sys/file.h>
#include <math.h>
#include <signal.h>

#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/proc.h"
#include "storage/smgr.h"
#include "utils/relcache.h"

#include "pgstat.h"

#define BufferGetLSN(bufHdr)	\
	(*((XLogRecPtr*) MAKE_PTR((bufHdr)->data)))


/* GUC variable */
bool		zero_damaged_pages = false;


static void WaitIO(BufferDesc *buf);
static void StartBufferIO(BufferDesc *buf, bool forInput);
static void TerminateBufferIO(BufferDesc *buf);
static void ContinueBufferIO(BufferDesc *buf, bool forInput);
static void buffer_write_error_callback(void *arg);

/*
 * Macro : BUFFER_IS_BROKEN
 *		Note that write error doesn't mean the buffer broken
*/
#define BUFFER_IS_BROKEN(buf) ((buf->flags & BM_IO_ERROR) && !(buf->flags & BM_DIRTY))

static Buffer ReadBufferInternal(Relation reln, BlockNumber blockNum,
				   bool bufferLockHeld);
static BufferDesc *BufferAlloc(Relation reln, BlockNumber blockNum,
			bool *foundPtr);
static int	BufferReplace(BufferDesc *bufHdr);

#ifdef NOT_USED
void		PrintBufferDescs(void);
#endif

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
 *		the block read, or NULL on an error.  If successful,
 *		the returned buffer has been pinned.
 *
 * Assume when this function is called, that reln has been
 *		opened already.
 *
 * Note: a side effect of a P_NEW call is to update reln->rd_nblocks.
 */

#undef ReadBuffer				/* conflicts with macro when BUFMGR_DEBUG
								 * defined */

/*
 * ReadBuffer
 */
Buffer
ReadBuffer(Relation reln, BlockNumber blockNum)
{
	return ReadBufferInternal(reln, blockNum, false);
}

/*
 * ReadBufferInternal -- internal version of ReadBuffer with more options
 *
 * bufferLockHeld: if true, caller already acquired the bufmgr lock.
 * (This is assumed never to be true if dealing with a local buffer!)
 */
static Buffer
ReadBufferInternal(Relation reln, BlockNumber blockNum,
				   bool bufferLockHeld)
{
	BufferDesc *bufHdr;
	int			status;
	bool		found;
	bool		isExtend;
	bool		isLocalBuf;

	isExtend = (blockNum == P_NEW);
	isLocalBuf = reln->rd_istemp;

	if (isLocalBuf)
	{
		ReadLocalBufferCount++;
		pgstat_count_buffer_read(&reln->pgstat_info, reln);
		/* Substitute proper block number if caller asked for P_NEW */
		if (isExtend)
		{
			blockNum = reln->rd_nblocks;
			reln->rd_nblocks++;
		}
		bufHdr = LocalBufferAlloc(reln, blockNum, &found);
		if (found)
		{
			LocalBufferHitCount++;
			pgstat_count_buffer_hit(&reln->pgstat_info, reln);
		}
	}
	else
	{
		ReadBufferCount++;
		pgstat_count_buffer_read(&reln->pgstat_info, reln);
		/* Substitute proper block number if caller asked for P_NEW */
		if (isExtend)
		{
			/* must be sure we have accurate file length! */
			blockNum = reln->rd_nblocks = smgrnblocks(DEFAULT_SMGR, reln);
			reln->rd_nblocks++;
		}

		/*
		 * lookup the buffer.  IO_IN_PROGRESS is set if the requested
		 * block is not currently in memory.
		 */
		if (!bufferLockHeld)
			LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);
		bufHdr = BufferAlloc(reln, blockNum, &found);
		if (found)
		{
			BufferHitCount++;
			pgstat_count_buffer_hit(&reln->pgstat_info, reln);
		}
	}

	/* At this point we do NOT hold the bufmgr lock. */

	if (!bufHdr)
		return InvalidBuffer;

	/* if it's already in the buffer pool, we're done */
	if (found)
	{
		/* That is, we're done if we expected to be able to find it ... */
		if (!isExtend)
			return BufferDescriptorGetBuffer(bufHdr);

		/*
		 * If we found a buffer when we were expecting to extend the
		 * relation, the implication is that a buffer was already created
		 * for the next page position, but then smgrextend failed to write
		 * the page. We'd better try the smgrextend again.  But since
		 * BufferAlloc won't have done StartBufferIO, we must do that
		 * first.
		 */
		if (!isLocalBuf)
		{
			LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);
			StartBufferIO(bufHdr, false);
			LWLockRelease(BufMgrLock);
		}
	}

	/*
	 * if we have gotten to this point, the reln pointer must be ok and
	 * the relation file must be open.
	 */
	if (isExtend)
	{
		/* new buffers are zero-filled */
		MemSet((char *) MAKE_PTR(bufHdr->data), 0, BLCKSZ);
		status = smgrextend(DEFAULT_SMGR, reln, blockNum,
							(char *) MAKE_PTR(bufHdr->data));
	}
	else
	{
		status = smgrread(DEFAULT_SMGR, reln, blockNum,
						  (char *) MAKE_PTR(bufHdr->data));
		/* check for garbage data */
		if (status == SM_SUCCESS &&
			!PageHeaderIsValid((PageHeader) MAKE_PTR(bufHdr->data)))
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
		/* No shared buffer state to update... */
		if (status == SM_FAIL)
		{
			bufHdr->flags |= BM_IO_ERROR;
			return InvalidBuffer;
		}
		return BufferDescriptorGetBuffer(bufHdr);
	}

	/* lock buffer manager again to update IO IN PROGRESS */
	LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);

	if (status == SM_FAIL)
	{
		/* IO Failed.  cleanup the data structures and go home */

		if (!BufTableDelete(bufHdr))
		{
			LWLockRelease(BufMgrLock);
			elog(FATAL, "buffer table broken after I/O error");
		}
		/* remember that BufferAlloc() pinned the buffer */
		UnpinBuffer(bufHdr);

		/*
		 * Have to reset the flag so that anyone waiting for the buffer
		 * can tell that the contents are invalid.
		 */
		bufHdr->flags |= BM_IO_ERROR;
		bufHdr->flags &= ~BM_IO_IN_PROGRESS;
	}
	else
	{
		/* IO Succeeded.  clear the flags, finish buffer update */

		bufHdr->flags &= ~(BM_IO_ERROR | BM_IO_IN_PROGRESS);
	}

	/* If anyone was waiting for IO to complete, wake them up now */
	TerminateBufferIO(bufHdr);

	LWLockRelease(BufMgrLock);

	if (status == SM_FAIL)
		return InvalidBuffer;

	return BufferDescriptorGetBuffer(bufHdr);
}

/*
 * BufferAlloc -- Get a buffer from the buffer pool but don't
 *		read it.  If successful, the returned buffer is pinned.
 *
 * Returns: descriptor for buffer
 *
 * BufMgrLock must be held at entry.  When this routine returns,
 * the BufMgrLock is guaranteed NOT to be held.
 */
static BufferDesc *
BufferAlloc(Relation reln,
			BlockNumber blockNum,
			bool *foundPtr)
{
	BufferDesc *buf,
			   *buf2;
	BufferTag	newTag;			/* identity of requested block */
	bool		inProgress;		/* buffer undergoing IO */

	/* create a new tag so we can lookup the buffer */
	/* assume that the relation is already open */
	INIT_BUFFERTAG(&newTag, reln, blockNum);

	/* see if the block is in the buffer pool already */
	buf = BufTableLookup(&newTag);
	if (buf != NULL)
	{
		/*
		 * Found it.  Now, (a) pin the buffer so no one steals it from the
		 * buffer pool, (b) check IO_IN_PROGRESS, someone may be faulting
		 * the buffer into the buffer pool.
		 */

		PinBuffer(buf);
		inProgress = (buf->flags & BM_IO_IN_PROGRESS);

		*foundPtr = TRUE;
		if (inProgress)			/* confirm end of IO */
		{
			WaitIO(buf);
			inProgress = (buf->flags & BM_IO_IN_PROGRESS);
		}
		if (BUFFER_IS_BROKEN(buf))
		{
			/*
			 * I couldn't understand the following old comment. If there's
			 * no IO for the buffer and the buffer is BROKEN, it should be
			 * read again. So start a new buffer IO here.
			 *
			 * wierd race condition:
			 *
			 * We were waiting for someone else to read the buffer. While we
			 * were waiting, the reader boof'd in some way, so the
			 * contents of the buffer are still invalid.  By saying that
			 * we didn't find it, we can make the caller reinitialize the
			 * buffer.	If two processes are waiting for this block, both
			 * will read the block.  The second one to finish may
			 * overwrite any updates made by the first.  (Assume higher
			 * level synchronization prevents this from happening).
			 *
			 * This is never going to happen, don't worry about it.
			 */
			*foundPtr = FALSE;
		}
#ifdef BMTRACE
		_bm_trace((reln->rd_rel->relisshared ? 0 : MyDatabaseId), RelationGetRelid(reln), blockNum, BufferDescriptorGetBuffer(buf), BMT_ALLOCFND);
#endif   /* BMTRACE */

		if (!(*foundPtr))
			StartBufferIO(buf, true);
		LWLockRelease(BufMgrLock);

		return buf;
	}

	*foundPtr = FALSE;

	/*
	 * Didn't find it in the buffer pool.  We'll have to initialize a new
	 * buffer.	First, grab one from the free list.  If it's dirty, flush
	 * it to disk. Remember to unlock BufMgrLock while doing the IOs.
	 */
	inProgress = FALSE;
	for (buf = (BufferDesc *) NULL; buf == (BufferDesc *) NULL;)
	{
		buf = GetFreeBuffer();

		/* GetFreeBuffer will abort if it can't find a free buffer */
		Assert(buf);

		/*
		 * There should be exactly one pin on the buffer after it is
		 * allocated -- ours.  If it had a pin it wouldn't have been on
		 * the free list.  No one else could have pinned it between
		 * GetFreeBuffer and here because we have the BufMgrLock.
		 */
		Assert(buf->refcount == 0);
		buf->refcount = 1;
		PrivateRefCount[BufferDescriptorGetBuffer(buf) - 1] = 1;

		if (buf->flags & BM_DIRTY || buf->cntxDirty)
		{
			bool		smok;

			/*
			 * skip write error buffers
			 */
			if ((buf->flags & BM_IO_ERROR) != 0)
			{
				UnpinBuffer(buf);
				buf = (BufferDesc *) NULL;
				continue;
			}

			/*
			 * Set BM_IO_IN_PROGRESS to keep anyone from doing anything
			 * with the contents of the buffer while we write it out. We
			 * don't really care if they try to read it, but if they can
			 * complete a BufferAlloc on it they can then scribble into
			 * it, and we'd really like to avoid that while we are
			 * flushing the buffer.  Setting this flag should block them
			 * in WaitIO until we're done.
			 */
			inProgress = TRUE;

			/*
			 * All code paths that acquire this lock pin the buffer first;
			 * since no one had it pinned (it just came off the free
			 * list), no one else can have this lock.
			 */
			StartBufferIO(buf, false);

			/*
			 * Write the buffer out, being careful to release BufMgrLock
			 * before starting the I/O.
			 */
			smok = BufferReplace(buf);

			if (smok == FALSE)
			{
				ereport(WARNING,
						(errcode(ERRCODE_IO_ERROR),
						 errmsg("could not write block %u of %u/%u",
								buf->tag.blockNum,
								buf->tag.rnode.tblNode,
								buf->tag.rnode.relNode)));
				inProgress = FALSE;
				buf->flags |= BM_IO_ERROR;
				buf->flags &= ~BM_IO_IN_PROGRESS;
				TerminateBufferIO(buf);
				UnpinBuffer(buf);
				buf = (BufferDesc *) NULL;
			}
			else
			{
				/*
				 * BM_JUST_DIRTIED cleared by BufferReplace and shouldn't
				 * be set by anyone.		- vadim 01/17/97
				 */
				if (buf->flags & BM_JUST_DIRTIED)
				{
					elog(PANIC, "content of block %u of %u/%u changed while flushing",
						 buf->tag.blockNum,
						 buf->tag.rnode.tblNode, buf->tag.rnode.relNode);
				}
				else
					buf->flags &= ~BM_DIRTY;
				buf->cntxDirty = false;
			}

			/*
			 * Somebody could have pinned the buffer while we were doing
			 * the I/O and had given up the BufMgrLock (though they would
			 * be waiting for us to clear the BM_IO_IN_PROGRESS flag).
			 * That's why this is a loop -- if so, we need to clear the
			 * I/O flags, remove our pin and start all over again.
			 *
			 * People may be making buffers free at any time, so there's no
			 * reason to think that we have an immediate disaster on our
			 * hands.
			 */
			if (buf && buf->refcount > 1)
			{
				inProgress = FALSE;
				buf->flags &= ~BM_IO_IN_PROGRESS;
				TerminateBufferIO(buf);
				UnpinBuffer(buf);
				buf = (BufferDesc *) NULL;
			}

			/*
			 * Somebody could have allocated another buffer for the same
			 * block we are about to read in. (While we flush out the
			 * dirty buffer, we don't hold the lock and someone could have
			 * allocated another buffer for the same block. The problem is
			 * we haven't gotten around to insert the new tag into the
			 * buffer table. So we need to check here.		-ay 3/95
			 */
			buf2 = BufTableLookup(&newTag);
			if (buf2 != NULL)
			{
				/*
				 * Found it. Someone has already done what we're about to
				 * do. We'll just handle this as if it were found in the
				 * buffer pool in the first place.
				 */
				if (buf != NULL)
				{
					buf->flags &= ~BM_IO_IN_PROGRESS;
					TerminateBufferIO(buf);
					/* give up old buffer since we don't need it any more */
					UnpinBuffer(buf);
				}

				PinBuffer(buf2);
				inProgress = (buf2->flags & BM_IO_IN_PROGRESS);

				*foundPtr = TRUE;
				if (inProgress)
				{
					WaitIO(buf2);
					inProgress = (buf2->flags & BM_IO_IN_PROGRESS);
				}
				if (BUFFER_IS_BROKEN(buf2))
					*foundPtr = FALSE;

				if (!(*foundPtr))
					StartBufferIO(buf2, true);
				LWLockRelease(BufMgrLock);

				return buf2;
			}
		}
	}

	/*
	 * At this point we should have the sole pin on a non-dirty buffer and
	 * we may or may not already have the BM_IO_IN_PROGRESS flag set.
	 */

	/*
	 * Change the name of the buffer in the lookup table:
	 *
	 * Need to update the lookup table before the read starts. If someone
	 * comes along looking for the buffer while we are reading it in, we
	 * don't want them to allocate a new buffer.  For the same reason, we
	 * didn't want to erase the buf table entry for the buffer we were
	 * writing back until now, either.
	 */

	if (!BufTableDelete(buf))
	{
		LWLockRelease(BufMgrLock);
		elog(FATAL, "buffer wasn't in the buffer hash table");
	}

	INIT_BUFFERTAG(&(buf->tag), reln, blockNum);

	if (!BufTableInsert(buf))
	{
		LWLockRelease(BufMgrLock);
		elog(FATAL, "buffer in buffer hash table twice");
	}

	/*
	 * Buffer contents are currently invalid.  Have to mark IO IN PROGRESS
	 * so no one fiddles with them until the read completes.  If this
	 * routine has been called simply to allocate a buffer, no io will be
	 * attempted, so the flag isnt set.
	 */
	if (!inProgress)
		StartBufferIO(buf, true);
	else
		ContinueBufferIO(buf, true);

#ifdef BMTRACE
	_bm_trace((reln->rd_rel->relisshared ? 0 : MyDatabaseId), RelationGetRelid(reln), blockNum, BufferDescriptorGetBuffer(buf), BMT_ALLOCNOTFND);
#endif   /* BMTRACE */

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

	LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);
	Assert(bufHdr->refcount > 0);

	bufHdr->flags |= (BM_DIRTY | BM_JUST_DIRTIED);

	if (release)
		UnpinBuffer(bufHdr);
	LWLockRelease(BufMgrLock);
}

/*
 * WriteBuffer
 *
 *		Marks buffer contents as dirty (actual write happens later).
 *
 * Assume that buffer is pinned.  Assume that reln is
 *		valid.
 *
 * Side Effects:
 *		Pin count is decremented.
 */

#undef WriteBuffer

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


#undef ReleaseAndReadBuffer
/*
 * ReleaseAndReadBuffer -- combine ReleaseBuffer() and ReadBuffer()
 *		to save a lock release/acquire.
 *
 * Also, if the passed buffer is valid and already contains the desired block
 * number, we simply return it without ever acquiring the lock at all.
 * Since the passed buffer must be pinned, it's OK to examine its block
 * number without getting the lock first.
 *
 * Note: it is OK to pass buffer = InvalidBuffer, indicating that no old
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
			LocalRefCount[-buffer - 1]--;
		}
		else
		{
			Assert(PrivateRefCount[buffer - 1] > 0);
			bufHdr = &BufferDescriptors[buffer - 1];
			if (bufHdr->tag.blockNum == blockNum &&
				RelFileNodeEquals(bufHdr->tag.rnode, relation->rd_node))
				return buffer;
			if (PrivateRefCount[buffer - 1] > 1)
				PrivateRefCount[buffer - 1]--;
			else
			{
				LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);
				UnpinBuffer(bufHdr);
				return ReadBufferInternal(relation, blockNum, true);
			}
		}
	}

	return ReadBufferInternal(relation, blockNum, false);
}

/*
 * BufferSync -- Write all dirty buffers in the pool.
 *
 * This is called at checkpoint time and writes out all dirty shared buffers.
 */
void
BufferSync(void)
{
	int			i;
	BufferDesc *bufHdr;
	ErrorContextCallback errcontext;

	/* Setup error traceback support for ereport() */
	errcontext.callback = buffer_write_error_callback;
	errcontext.arg = NULL;
	errcontext.previous = error_context_stack;
	error_context_stack = &errcontext;

	for (i = 0, bufHdr = BufferDescriptors; i < NBuffers; i++, bufHdr++)
	{
		Buffer		buffer;
		int			status;
		RelFileNode rnode;
		XLogRecPtr	recptr;
		Relation	reln;

		errcontext.arg = bufHdr;

		LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);

		if (!(bufHdr->flags & BM_VALID))
		{
			LWLockRelease(BufMgrLock);
			continue;
		}

		/*
		 * We can check bufHdr->cntxDirty here *without* holding any lock
		 * on buffer context as long as we set this flag in access methods
		 * *before* logging changes with XLogInsert(): if someone will set
		 * cntxDirty just after our check we don't worry because of our
		 * checkpoint.redo points before log record for upcoming changes
		 * and so we are not required to write such dirty buffer.
		 */
		if (!(bufHdr->flags & BM_DIRTY) && !(bufHdr->cntxDirty))
		{
			LWLockRelease(BufMgrLock);
			continue;
		}

		/*
		 * IO synchronization. Note that we do it with unpinned buffer to
		 * avoid conflicts with FlushRelationBuffers.
		 */
		if (bufHdr->flags & BM_IO_IN_PROGRESS)
		{
			WaitIO(bufHdr);
			if (!(bufHdr->flags & BM_VALID) ||
				(!(bufHdr->flags & BM_DIRTY) && !(bufHdr->cntxDirty)))
			{
				LWLockRelease(BufMgrLock);
				continue;
			}
		}

		/*
		 * Here: no one doing IO for this buffer and it's dirty. Pin
		 * buffer now and set IO state for it *before* acquiring shlock to
		 * avoid conflicts with FlushRelationBuffers.
		 */
		PinBuffer(bufHdr);
		StartBufferIO(bufHdr, false);	/* output IO start */

		buffer = BufferDescriptorGetBuffer(bufHdr);
		rnode = bufHdr->tag.rnode;

		LWLockRelease(BufMgrLock);

		/*
		 * Try to find relation for buffer
		 */
		reln = RelationNodeCacheGetRelation(rnode);

		/*
		 * Protect buffer content against concurrent update
		 */
		LockBuffer(buffer, BUFFER_LOCK_SHARE);

		/*
		 * Force XLOG flush for buffer' LSN
		 */
		recptr = BufferGetLSN(bufHdr);
		XLogFlush(recptr);

		/*
		 * Now it's safe to write buffer to disk. Note that no one else
		 * should not be able to write it while we were busy with locking
		 * and log flushing because of we setted IO flag.
		 */
		LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);
		Assert(bufHdr->flags & BM_DIRTY || bufHdr->cntxDirty);
		bufHdr->flags &= ~BM_JUST_DIRTIED;
		LWLockRelease(BufMgrLock);

		if (reln == (Relation) NULL)
		{
			status = smgrblindwrt(DEFAULT_SMGR,
								  bufHdr->tag.rnode,
								  bufHdr->tag.blockNum,
								  (char *) MAKE_PTR(bufHdr->data));
		}
		else
		{
			status = smgrwrite(DEFAULT_SMGR, reln,
							   bufHdr->tag.blockNum,
							   (char *) MAKE_PTR(bufHdr->data));
		}

		if (status == SM_FAIL)	/* disk failure ?! */
			ereport(PANIC,
					(errcode(ERRCODE_IO_ERROR),
					 errmsg("could not write block %u of %u/%u",
							bufHdr->tag.blockNum,
							bufHdr->tag.rnode.tblNode,
							bufHdr->tag.rnode.relNode)));

		/*
		 * Note that it's safe to change cntxDirty here because of we
		 * protect it from upper writers by share lock and from other
		 * bufmgr routines by BM_IO_IN_PROGRESS
		 */
		bufHdr->cntxDirty = false;

		/*
		 * Release the per-buffer readlock, reacquire BufMgrLock.
		 */
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		BufferFlushCount++;

		LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);

		bufHdr->flags &= ~BM_IO_IN_PROGRESS;	/* mark IO finished */
		TerminateBufferIO(bufHdr);		/* Sync IO finished */

		/*
		 * If this buffer was marked by someone as DIRTY while we were
		 * flushing it out we must not clear DIRTY flag - vadim 01/17/97
		 */
		if (!(bufHdr->flags & BM_JUST_DIRTIED))
			bufHdr->flags &= ~BM_DIRTY;
		UnpinBuffer(bufHdr);
		LWLockRelease(BufMgrLock);

		/* drop refcnt obtained by RelationNodeCacheGetRelation */
		if (reln != (Relation) NULL)
			RelationDecrementReferenceCount(reln);
	}

	/* Pop the error context stack */
	error_context_stack = errcontext.previous;
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


long		NDirectFileRead;	/* some I/O's are direct file access.
								 * bypass bufmgr */
long		NDirectFileWrite;	/* e.g., I/O in psort and hashjoin. */


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
				"buffer refcount leak: [%03d] (freeNext=%d, freePrev=%d, "
				  "rel=%u/%u, blockNum=%u, flags=0x%x, refcount=%d %ld)",
					 i, buf->freeNext, buf->freePrev,
					 buf->tag.rnode.tblNode, buf->tag.rnode.relNode,
					 buf->tag.blockNum, buf->flags,
					 buf->refcount, PrivateRefCount[i]);

			PrivateRefCount[i] = 1;		/* make sure we release shared pin */
			LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);
			UnpinBuffer(buf);
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
	Assert(BufferIsPinned(buffer));

	if (BufferIsLocal(buffer))
		return LocalBufferDescriptors[-buffer - 1].tag.blockNum;
	else
		return BufferDescriptors[buffer - 1].tag.blockNum;
}

/*
 * BufferReplace
 *
 * Write out the buffer corresponding to 'bufHdr'
 *
 * BufMgrLock must be held at entry, and the buffer must be pinned.
 */
static int
BufferReplace(BufferDesc *bufHdr)
{
	Relation	reln;
	XLogRecPtr	recptr;
	int			status;
	ErrorContextCallback errcontext;

	/* To check if block content changed while flushing. - vadim 01/17/97 */
	bufHdr->flags &= ~BM_JUST_DIRTIED;

	LWLockRelease(BufMgrLock);

	/* Setup error traceback support for ereport() */
	errcontext.callback = buffer_write_error_callback;
	errcontext.arg = bufHdr;
	errcontext.previous = error_context_stack;
	error_context_stack = &errcontext;

	/*
	 * No need to lock buffer context - no one should be able to end
	 * ReadBuffer
	 */
	recptr = BufferGetLSN(bufHdr);
	XLogFlush(recptr);

	reln = RelationNodeCacheGetRelation(bufHdr->tag.rnode);

	if (reln != (Relation) NULL)
	{
		status = smgrwrite(DEFAULT_SMGR, reln,
						   bufHdr->tag.blockNum,
						   (char *) MAKE_PTR(bufHdr->data));
	}
	else
	{
		status = smgrblindwrt(DEFAULT_SMGR, bufHdr->tag.rnode,
							  bufHdr->tag.blockNum,
							  (char *) MAKE_PTR(bufHdr->data));
	}

	/* drop relcache refcnt incremented by RelationNodeCacheGetRelation */
	if (reln != (Relation) NULL)
		RelationDecrementReferenceCount(reln);

	/* Pop the error context stack */
	error_context_stack = errcontext.previous;

	LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);

	if (status == SM_FAIL)
		return FALSE;

	BufferFlushCount++;

	return TRUE;
}

/*
 * RelationGetNumberOfBlocks
 *		Determines the current number of pages in the relation.
 *		Side effect: relation->rd_nblocks is updated.
 */
BlockNumber
RelationGetNumberOfBlocks(Relation relation)
{
	/*
	 * relation->rd_nblocks should be accurate already if the relation is
	 * new or temp, because no one else should be modifying it.  Otherwise
	 * we need to ask the smgr for the current physical file length.
	 *
	 * Don't call smgr on a view, either.
	 */
	if (relation->rd_rel->relkind == RELKIND_VIEW)
		relation->rd_nblocks = 0;
	else if (relation->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
		relation->rd_nblocks = 0;
	else if (!relation->rd_isnew && !relation->rd_istemp)
		relation->rd_nblocks = smgrnblocks(DEFAULT_SMGR, relation);
	return relation->rd_nblocks;
}

/*
 * RelationUpdateNumberOfBlocks
 *		Forcibly update relation->rd_nblocks.
 *
 * If the relcache drops an entry for a temp relation, it must call this
 * routine after recreating the relcache entry, so that rd_nblocks is
 * re-sync'd with reality.  See RelationGetNumberOfBlocks.
 */
void
RelationUpdateNumberOfBlocks(Relation relation)
{
	if (relation->rd_rel->relkind == RELKIND_VIEW)
		relation->rd_nblocks = 0;
	else if (relation->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
		relation->rd_nblocks = 0;
	else
		relation->rd_nblocks = smgrnblocks(DEFAULT_SMGR, relation);
}

/* ---------------------------------------------------------------------
 *		DropRelationBuffers
 *
 *		This function removes all the buffered pages for a relation
 *		from the buffer pool.  Dirty pages are simply dropped, without
 *		bothering to write them out first.	This is NOT rollback-able,
 *		and so should be used only with extreme caution!
 *
 *		We assume that the caller holds an exclusive lock on the relation,
 *		which should assure that no new buffers will be acquired for the rel
 *		meanwhile.
 * --------------------------------------------------------------------
 */
void
DropRelationBuffers(Relation rel)
{
	DropRelFileNodeBuffers(rel->rd_node, rel->rd_istemp);
}

/* ---------------------------------------------------------------------
 *		DropRelFileNodeBuffers
 *
 *		This is the same as DropRelationBuffers, except that the target
 *		relation is specified by RelFileNode and temp status.
 *
 *		This is NOT rollback-able.	One legitimate use is to clear the
 *		buffer cache of buffers for a relation that is being deleted
 *		during transaction abort.
 * --------------------------------------------------------------------
 */
void
DropRelFileNodeBuffers(RelFileNode rnode, bool istemp)
{
	int			i;
	BufferDesc *bufHdr;

	if (istemp)
	{
		for (i = 0; i < NLocBuffer; i++)
		{
			bufHdr = &LocalBufferDescriptors[i];
			if (RelFileNodeEquals(bufHdr->tag.rnode, rnode))
			{
				bufHdr->flags &= ~(BM_DIRTY | BM_JUST_DIRTIED);
				bufHdr->cntxDirty = false;
				LocalRefCount[i] = 0;
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
		if (RelFileNodeEquals(bufHdr->tag.rnode, rnode))
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
			/* Now we can do what we came for */
			bufHdr->flags &= ~(BM_DIRTY | BM_JUST_DIRTIED);
			bufHdr->cntxDirty = false;

			/*
			 * Release any refcount we may have.  If someone else has a
			 * pin on the buffer, we got trouble.
			 */
			if (!(bufHdr->flags & BM_FREE))
			{
				/* the sole pin should be ours */
				if (bufHdr->refcount != 1 || PrivateRefCount[i - 1] == 0)
					elog(FATAL, "block %u of %u/%u is still referenced (private %ld, global %d)",
						 bufHdr->tag.blockNum,
						 bufHdr->tag.rnode.tblNode,
						 bufHdr->tag.rnode.relNode,
						 PrivateRefCount[i - 1], bufHdr->refcount);
				/* Make sure it will be released */
				PrivateRefCount[i - 1] = 1;
				UnpinBuffer(bufHdr);
			}

			/*
			 * And mark the buffer as no longer occupied by this rel.
			 */
			BufTableDelete(bufHdr);
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

		/*
		 * We know that currently database OID is tblNode but this
		 * probably will be changed in future and this func will be used
		 * to drop tablespace buffers.
		 */
		if (bufHdr->tag.rnode.tblNode == dbid)
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
			Assert(bufHdr->flags & BM_FREE);

			/*
			 * And mark the buffer as no longer occupied by this page.
			 */
			BufTableDelete(bufHdr);
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
			elog(LOG, "[%02d] (freeNext=%d, freePrev=%d, rel=%u/%u, \
blockNum=%u, flags=0x%x, refcount=%d %ld)",
				 i, buf->freeNext, buf->freePrev,
				 buf->tag.rnode.tblNode, buf->tag.rnode.relNode,
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
			printf("[%-2d] (%u/%u, %u) flags=0x%x, refcnt=%d %ld)\n",
				   i, buf->tag.rnode.tblNode, buf->tag.rnode.relNode,
				   buf->tag.blockNum,
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
			elog(WARNING, "[%02d] (freeNext=%d, freePrev=%d, rel=%u/%u, \
blockNum=%u, flags=0x%x, refcount=%d %ld)",
				 i, buf->freeNext, buf->freePrev,
				 buf->tag.rnode.tblNode, buf->tag.rnode.relNode,
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
 *		actually removed from the buffer pool.	An error code is returned
 *		if we fail to dump a dirty buffer or if we find one of
 *		the target pages is pinned into the cache.
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
 *
 *		Formerly, we considered it an error condition if we found dirty
 *		buffers here.	However, since BufferSync no longer forces out all
 *		dirty buffers at every xact commit, it's possible for dirty buffers
 *		to still be present in the cache due to failure of an earlier
 *		transaction.  So, must flush dirty buffers without complaint.
 *
 *		Returns: 0 - Ok, -1 - FAILED TO WRITE DIRTY BUFFER, -2 - PINNED
 *
 *		XXX currently it sequentially searches the buffer pool, should be
 *		changed to more clever ways of searching.
 * --------------------------------------------------------------------
 */
int
FlushRelationBuffers(Relation rel, BlockNumber firstDelBlock)
{
	int			i;
	BufferDesc *bufHdr;
	XLogRecPtr	recptr;
	int			status;
	ErrorContextCallback errcontext;

	/* Setup error traceback support for ereport() */
	errcontext.callback = buffer_write_error_callback;
	errcontext.arg = NULL;
	errcontext.previous = error_context_stack;
	error_context_stack = &errcontext;

	if (rel->rd_istemp)
	{
		for (i = 0; i < NLocBuffer; i++)
		{
			bufHdr = &LocalBufferDescriptors[i];
			errcontext.arg = bufHdr;
			if (RelFileNodeEquals(bufHdr->tag.rnode, rel->rd_node))
			{
				if (bufHdr->flags & BM_DIRTY || bufHdr->cntxDirty)
				{
					status = smgrwrite(DEFAULT_SMGR, rel,
									   bufHdr->tag.blockNum,
									   (char *) MAKE_PTR(bufHdr->data));
					if (status == SM_FAIL)
					{
						error_context_stack = errcontext.previous;
						elog(WARNING, "FlushRelationBuffers(\"%s\" (local), %u): block %u is dirty, could not flush it",
							 RelationGetRelationName(rel), firstDelBlock,
							 bufHdr->tag.blockNum);
						return (-1);
					}
					bufHdr->flags &= ~(BM_DIRTY | BM_JUST_DIRTIED);
					bufHdr->cntxDirty = false;
				}
				if (LocalRefCount[i] > 0)
				{
					error_context_stack = errcontext.previous;
					elog(WARNING, "FlushRelationBuffers(\"%s\" (local), %u): block %u is referenced (%ld)",
						 RelationGetRelationName(rel), firstDelBlock,
						 bufHdr->tag.blockNum, LocalRefCount[i]);
					return (-2);
				}
				if (bufHdr->tag.blockNum >= firstDelBlock)
					bufHdr->tag.rnode.relNode = InvalidOid;
			}
		}

		/* Pop the error context stack */
		error_context_stack = errcontext.previous;

		return 0;
	}

	LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);

	for (i = 0; i < NBuffers; i++)
	{
		bufHdr = &BufferDescriptors[i];
		errcontext.arg = bufHdr;
		if (RelFileNodeEquals(bufHdr->tag.rnode, rel->rd_node))
		{
			if (bufHdr->flags & BM_DIRTY || bufHdr->cntxDirty)
			{
				PinBuffer(bufHdr);
				if (bufHdr->flags & BM_IO_IN_PROGRESS)
					WaitIO(bufHdr);
				LWLockRelease(BufMgrLock);

				/*
				 * Force XLOG flush for buffer' LSN
				 */
				recptr = BufferGetLSN(bufHdr);
				XLogFlush(recptr);

				/*
				 * Now it's safe to write buffer to disk
				 */

				LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);
				if (bufHdr->flags & BM_IO_IN_PROGRESS)
					WaitIO(bufHdr);

				if (bufHdr->flags & BM_DIRTY || bufHdr->cntxDirty)
				{
					bufHdr->flags &= ~BM_JUST_DIRTIED;
					StartBufferIO(bufHdr, false);		/* output IO start */

					LWLockRelease(BufMgrLock);

					status = smgrwrite(DEFAULT_SMGR, rel,
									   bufHdr->tag.blockNum,
									   (char *) MAKE_PTR(bufHdr->data));

					if (status == SM_FAIL)		/* disk failure ?! */
						ereport(PANIC,
								(errcode(ERRCODE_IO_ERROR),
							  errmsg("could not write block %u of %u/%u",
									 bufHdr->tag.blockNum,
									 bufHdr->tag.rnode.tblNode,
									 bufHdr->tag.rnode.relNode)));

					BufferFlushCount++;

					LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);
					bufHdr->flags &= ~BM_IO_IN_PROGRESS;
					TerminateBufferIO(bufHdr);
					Assert(!(bufHdr->flags & BM_JUST_DIRTIED));
					bufHdr->flags &= ~BM_DIRTY;

					/*
					 * Note that it's safe to change cntxDirty here
					 * because of we protect it from upper writers by
					 * AccessExclusiveLock and from other bufmgr routines
					 * by BM_IO_IN_PROGRESS
					 */
					bufHdr->cntxDirty = false;
				}
				UnpinBuffer(bufHdr);
			}
			if (!(bufHdr->flags & BM_FREE))
			{
				LWLockRelease(BufMgrLock);
				error_context_stack = errcontext.previous;
				elog(WARNING, "FlushRelationBuffers(\"%s\", %u): block %u is referenced (private %ld, global %d)",
					 RelationGetRelationName(rel), firstDelBlock,
					 bufHdr->tag.blockNum,
					 PrivateRefCount[i], bufHdr->refcount);
				return -2;
			}
			if (bufHdr->tag.blockNum >= firstDelBlock)
				BufTableDelete(bufHdr);
		}
	}

	LWLockRelease(BufMgrLock);

	/* Pop the error context stack */
	error_context_stack = errcontext.previous;

	return 0;
}

#undef ReleaseBuffer

/*
 * ReleaseBuffer -- remove the pin on a buffer without
 *		marking it dirty.
 */
int
ReleaseBuffer(Buffer buffer)
{
	BufferDesc *bufHdr;

	if (BufferIsLocal(buffer))
	{
		Assert(LocalRefCount[-buffer - 1] > 0);
		LocalRefCount[-buffer - 1]--;
		return STATUS_OK;
	}

	if (BAD_BUFFER_ID(buffer))
		return STATUS_ERROR;

	bufHdr = &BufferDescriptors[buffer - 1];

	Assert(PrivateRefCount[buffer - 1] > 0);
	if (PrivateRefCount[buffer - 1] > 1)
		PrivateRefCount[buffer - 1]--;
	else
	{
		LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);
		UnpinBuffer(bufHdr);
		LWLockRelease(BufMgrLock);
	}

	return STATUS_OK;
}

#ifdef NOT_USED
void
IncrBufferRefCount_Debug(char *file, int line, Buffer buffer)
{
	IncrBufferRefCount(buffer);
	if (ShowPinTrace && !BufferIsLocal(buffer) && is_userbuffer(buffer))
	{
		BufferDesc *buf = &BufferDescriptors[buffer - 1];

		fprintf(stderr, "PIN(Incr) %d rel = %u/%u, blockNum = %u, \
refcount = %ld, file: %s, line: %d\n",
				buffer,
				buf->tag.rnode.tblNode, buf->tag.rnode.relNode,
				buf->tag.blockNum,
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

		fprintf(stderr, "UNPIN(Rel) %d rel = %u/%u, blockNum = %u, \
refcount = %ld, file: %s, line: %d\n",
				buffer,
				buf->tag.rnode.tblNode, buf->tag.rnode.relNode,
				buf->tag.blockNum,
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

		fprintf(stderr, "UNPIN(Rel&Rd) %d rel = %u/%u, blockNum = %u, \
refcount = %ld, file: %s, line: %d\n",
				buffer,
				buf->tag.rnode.tblNode, buf->tag.rnode.relNode,
				buf->tag.blockNum,
				PrivateRefCount[buffer - 1], file, line);
	}
	if (ShowPinTrace && BufferIsLocal(buffer) && is_userbuffer(buffer))
	{
		BufferDesc *buf = &BufferDescriptors[b - 1];

		fprintf(stderr, "PIN(Rel&Rd) %d rel = %u/%u, blockNum = %u, \
refcount = %ld, file: %s, line: %d\n",
				b,
				buf->tag.rnode.tblNode, buf->tag.rnode.relNode,
				buf->tag.blockNum,
				PrivateRefCount[b - 1], file, line);
	}
	return b;
}
#endif

#ifdef BMTRACE

/*
 *	trace allocations and deallocations in a circular buffer in
 *	shared memory.	check the buffer before doing the allocation,
 *	and die if there's anything fishy.
 */

void
_bm_trace(Oid dbId, Oid relId, int blkNo, int bufNo, int allocType)
{
	long		start,
				cur;
	bmtrace    *tb;

	start = *CurTraceBuf;

	if (start > 0)
		cur = start - 1;
	else
		cur = BMT_LIMIT - 1;

	for (;;)
	{
		tb = &TraceBuf[cur];
		if (tb->bmt_op != BMT_NOTUSED)
		{
			if (tb->bmt_buf == bufNo)
			{
				if ((tb->bmt_op == BMT_DEALLOC)
					|| (tb->bmt_dbid == dbId && tb->bmt_relid == relId
						&& tb->bmt_blkno == blkNo))
					goto okay;

				/* die holding the buffer lock */
				_bm_die(dbId, relId, blkNo, bufNo, allocType, start, cur);
			}
		}

		if (cur == start)
			goto okay;

		if (cur == 0)
			cur = BMT_LIMIT - 1;
		else
			cur--;
	}

okay:
	tb = &TraceBuf[start];
	tb->bmt_pid = MyProcPid;
	tb->bmt_buf = bufNo;
	tb->bmt_dbid = dbId;
	tb->bmt_relid = relId;
	tb->bmt_blkno = blkNo;
	tb->bmt_op = allocType;

	*CurTraceBuf = (start + 1) % BMT_LIMIT;
}

void
_bm_die(Oid dbId, Oid relId, int blkNo, int bufNo,
		int allocType, long start, long cur)
{
	FILE	   *fp;
	bmtrace    *tb;
	int			i;

	tb = &TraceBuf[cur];

	if ((fp = AllocateFile("/tmp/death_notice", "w")) == NULL)
		elog(FATAL, "buffer alloc trace error and can't open log file");

	fprintf(fp, "buffer alloc trace detected the following error:\n\n");
	fprintf(fp, "    buffer %d being %s inconsistently with a previous %s\n\n",
		 bufNo, (allocType == BMT_DEALLOC ? "deallocated" : "allocated"),
			(tb->bmt_op == BMT_DEALLOC ? "deallocation" : "allocation"));

	fprintf(fp, "the trace buffer contains:\n");

	i = start;
	for (;;)
	{
		tb = &TraceBuf[i];
		if (tb->bmt_op != BMT_NOTUSED)
		{
			fprintf(fp, "     [%3d]%spid %d buf %2d for <%u,%u,%u> ",
					i, (i == cur ? " ---> " : "\t"),
					tb->bmt_pid, tb->bmt_buf,
					tb->bmt_dbid, tb->bmt_relid, tb->bmt_blkno);

			switch (tb->bmt_op)
			{
				case BMT_ALLOCFND:
					fprintf(fp, "allocate (found)\n");
					break;

				case BMT_ALLOCNOTFND:
					fprintf(fp, "allocate (not found)\n");
					break;

				case BMT_DEALLOC:
					fprintf(fp, "deallocate\n");
					break;

				default:
					fprintf(fp, "unknown op type %d\n", tb->bmt_op);
					break;
			}
		}

		i = (i + 1) % BMT_LIMIT;
		if (i == start)
			break;
	}

	fprintf(fp, "\noperation causing error:\n");
	fprintf(fp, "\tpid %d buf %d for <%d,%u,%d> ",
			getpid(), bufNo, dbId, relId, blkNo);

	switch (allocType)
	{
		case BMT_ALLOCFND:
			fprintf(fp, "allocate (found)\n");
			break;

		case BMT_ALLOCNOTFND:
			fprintf(fp, "allocate (not found)\n");
			break;

		case BMT_DEALLOC:
			fprintf(fp, "deallocate\n");
			break;

		default:
			fprintf(fp, "unknown op type %d\n", allocType);
			break;
	}

	FreeFile(fp);

	kill(getpid(), SIGILL);
}
#endif   /* BMTRACE */

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
 * So, be as quick as we can if the buffer is already dirty.
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
			elog(ERROR, "incorrect local pin count: %ld",
				 LocalRefCount[-buffer - 1]);
		/* Nobody else to wait for */
		return;
	}

	/* There should be exactly one local pin */
	if (PrivateRefCount[buffer - 1] != 1)
		elog(ERROR, "incorrect local pin count: %ld",
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
static BufferDesc *InProgressBuf = (BufferDesc *) NULL;
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
 *	The buffer is Pinned
 *
 * Because BufMgrLock is held, we are already in an interrupt holdoff here,
 * and do not need another.
 */
static void
TerminateBufferIO(BufferDesc *buf)
{
	Assert(buf == InProgressBuf);
	LWLockRelease(buf->io_in_progress_lock);
	InProgressBuf = (BufferDesc *) NULL;
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
	InProgressBuf = (BufferDesc *) NULL;
}
#endif

/*
 *	Clean up any active buffer I/O after an error.
 *	BufMgrLock isn't held when this function is called.
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
			Assert(!(buf->flags & BM_DIRTY) && !(buf->cntxDirty));
		else
		{
			Assert(buf->flags & BM_DIRTY || buf->cntxDirty);
			/* Issue notice if this is not the first failure... */
			if (buf->flags & BM_IO_ERROR)
			{
				ereport(WARNING,
						(errcode(ERRCODE_IO_ERROR),
						 errmsg("could not write block %u of %u/%u",
								buf->tag.blockNum,
								buf->tag.rnode.tblNode,
								buf->tag.rnode.relNode),
						 errdetail("Multiple failures --- write error may be permanent.")));
			}
			buf->flags |= BM_DIRTY;
		}
		buf->flags |= BM_IO_ERROR;
		buf->flags &= ~BM_IO_IN_PROGRESS;
		TerminateBufferIO(buf);
		LWLockRelease(BufMgrLock);
	}
}

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
 * Error context callback for errors occurring during buffer writes.
 */
static void
buffer_write_error_callback(void *arg)
{
	BufferDesc *bufHdr = (BufferDesc *) arg;

	if (bufHdr != NULL)
		errcontext("writing block %u of relation %u/%u",
				   bufHdr->tag.blockNum,
				   bufHdr->tag.rnode.tblNode, bufHdr->tag.rnode.relNode);
}
