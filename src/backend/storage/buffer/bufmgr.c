/*-------------------------------------------------------------------------
 *
 * bufmgr.c
 *	  buffer manager interface routines
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/buffer/bufmgr.c,v 1.88 2000/10/20 11:01:07 vadim Exp $
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
 * FlushBuffer() -- Write buffer immediately.  Can unpin, or not,
 *		depending on parameter.
 *
 * BufferSync() -- flush all dirty buffers in the buffer pool.
 *
 * InitBufferPool() -- Init the buffer module.
 *
 * See other files:
 *		freelist.c -- chooses victim for buffer replacement
 *		buf_table.c -- manages the buffer lookup table
 */
#include <sys/types.h>
#include <sys/file.h>
#include <math.h>
#include <signal.h>

#include "postgres.h"
#include "executor/execdebug.h"
#include "miscadmin.h"
#include "storage/s_lock.h"
#include "storage/smgr.h"
#include "utils/relcache.h"

extern SPINLOCK BufMgrLock;
extern long int ReadBufferCount;
extern long int ReadLocalBufferCount;
extern long int BufferHitCount;
extern long int LocalBufferHitCount;
extern long int BufferFlushCount;
extern long int LocalBufferFlushCount;

/*
 * It's used to avoid disk writes for read-only transactions
 * (i.e. when no one shared buffer was changed by transaction).
 * We set it to true in WriteBuffer/WriteNoReleaseBuffer when
 * marking shared buffer as dirty. We set it to false in xact.c
 * after transaction is committed/aborted.
 */
bool		SharedBufferChanged = false;

static void WaitIO(BufferDesc *buf, SPINLOCK spinlock);
static void StartBufferIO(BufferDesc *buf, bool forInput);
static void TerminateBufferIO(BufferDesc *buf);
static void ContinueBufferIO(BufferDesc *buf, bool forInput);
extern void AbortBufferIO(void);

/*
 * Macro : BUFFER_IS_BROKEN
 *		Note that write error doesn't mean the buffer broken
*/
#define BUFFER_IS_BROKEN(buf) ((buf->flags & BM_IO_ERROR) && !(buf->flags & BM_DIRTY))

#ifndef HAS_TEST_AND_SET
static void SignalIO(BufferDesc *buf);
extern long *NWaitIOBackendP;	/* defined in buf_init.c */

#endif	 /* HAS_TEST_AND_SET */

static Buffer ReadBufferWithBufferLock(Relation relation, BlockNumber blockNum,
						 bool bufferLockHeld);
static BufferDesc *BufferAlloc(Relation reln, BlockNumber blockNum,
			bool *foundPtr, bool bufferLockHeld);
static void SetBufferDirtiedByMe(Buffer buffer, BufferDesc *bufHdr);
static void ClearBufferDirtiedByMe(Buffer buffer, BufferDesc *bufHdr);
static void BufferSync(void);
static int	BufferReplace(BufferDesc *bufHdr);
void		PrintBufferDescs(void);

/* ---------------------------------------------------
 * RelationGetBufferWithBuffer
 *		see if the given buffer is what we want
 *		if yes, we don't need to bother the buffer manager
 * ---------------------------------------------------
 */
Buffer
RelationGetBufferWithBuffer(Relation relation,
							BlockNumber blockNumber,
							Buffer buffer)
{
	BufferDesc *bufHdr;

	if (BufferIsValid(buffer))
	{
		if (!BufferIsLocal(buffer))
		{
			bufHdr = &BufferDescriptors[buffer - 1];
			SpinAcquire(BufMgrLock);
			if (bufHdr->tag.blockNum == blockNumber &&
				RelFileNodeEquals(bufHdr->tag.rnode, relation->rd_node))
			{
				SpinRelease(BufMgrLock);
				return buffer;
			}
			return ReadBufferWithBufferLock(relation, blockNumber, true);
		}
		else
		{
			bufHdr = &LocalBufferDescriptors[-buffer - 1];
			if (bufHdr->tag.blockNum == blockNumber &&
				RelFileNodeEquals(bufHdr->tag.rnode, relation->rd_node))
				return buffer;
		}
	}
	return ReadBuffer(relation, blockNumber);
}

/*
 * ReadBuffer -- returns a buffer containing the requested
 *		block of the requested relation.  If the blknum
 *		requested is P_NEW, extend the relation file and
 *		allocate a new block.
 *
 * Returns: the buffer number for the buffer containing
 *		the block read or NULL on an error.
 *
 * Assume when this function is called, that reln has been
 *		opened already.
 */

#undef ReadBuffer				/* conflicts with macro when BUFMGR_DEBUG
								 * defined */

/*
 * ReadBuffer
 *
 */
Buffer
ReadBuffer(Relation reln, BlockNumber blockNum)
{
	return ReadBufferWithBufferLock(reln, blockNum, false);
}

/*
 * is_userbuffer
 *
 * XXX caller must have already acquired BufMgrLock
 */
#ifdef NOT_USED
static bool
is_userbuffer(Buffer buffer)
{
	BufferDesc *buf = &BufferDescriptors[buffer - 1];

	if (IsSystemRelationName(buf->blind.relname))
		return false;
	return true;
}

#endif

#ifdef NOT_USED
Buffer
ReadBuffer_Debug(char *file,
				 int line,
				 Relation reln,
				 BlockNumber blockNum)
{
	Buffer		buffer;

	buffer = ReadBufferWithBufferLock(reln, blockNum, false);
	if (ShowPinTrace && !BufferIsLocal(buffer) && is_userbuffer(buffer))
	{
		BufferDesc *buf = &BufferDescriptors[buffer - 1];

		fprintf(stderr, "PIN(RD) %ld relname = %s, blockNum = %d, \
refcount = %ld, file: %s, line: %d\n",
				buffer, buf->blind.relname, buf->tag.blockNum,
				PrivateRefCount[buffer - 1], file, line);
	}
	return buffer;
}

#endif

/*
 * ReadBufferWithBufferLock -- does the work of
 *		ReadBuffer() but with the possibility that
 *		the buffer lock has already been held. this
 *		is yet another effort to reduce the number of
 *		semops in the system.
 */
static Buffer
ReadBufferWithBufferLock(Relation reln,
						 BlockNumber blockNum,
						 bool bufferLockHeld)
{
	BufferDesc *bufHdr;
	int			extend;			/* extending the file by one block */
	int			status;
	bool		found;
	bool		isLocalBuf;

	extend = (blockNum == P_NEW);
	isLocalBuf = reln->rd_myxactonly;

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
		 * lookup the buffer.  IO_IN_PROGRESS is set if the requested
		 * block is not currently in memory.
		 */
		bufHdr = BufferAlloc(reln, blockNum, &found, bufferLockHeld);
		if (found)
			BufferHitCount++;
	}

	if (!bufHdr)
		return InvalidBuffer;

	/* if it's already in the buffer pool, we're done */
	if (found)
	{

		/*
		 * This happens when a bogus buffer was returned previously and is
		 * floating around in the buffer pool.	A routine calling this
		 * would want this extended.
		 */
		if (extend)
		{
			/* new buffers are zero-filled */
			MemSet((char *) MAKE_PTR(bufHdr->data), 0, BLCKSZ);
			smgrextend(DEFAULT_SMGR, reln,
					   (char *) MAKE_PTR(bufHdr->data));
		}
		return BufferDescriptorGetBuffer(bufHdr);

	}

	/*
	 * if we have gotten to this point, the reln pointer must be ok and
	 * the relation file must be open.
	 */
	if (extend)
	{
		/* new buffers are zero-filled */
		MemSet((char *) MAKE_PTR(bufHdr->data), 0, BLCKSZ);
		status = smgrextend(DEFAULT_SMGR, reln,
							(char *) MAKE_PTR(bufHdr->data));
	}
	else
	{
		status = smgrread(DEFAULT_SMGR, reln, blockNum,
						  (char *) MAKE_PTR(bufHdr->data));
	}

	if (isLocalBuf)
		return BufferDescriptorGetBuffer(bufHdr);

	/* lock buffer manager again to update IO IN PROGRESS */
	SpinAcquire(BufMgrLock);

	if (status == SM_FAIL)
	{
		/* IO Failed.  cleanup the data structures and go home */

		if (!BufTableDelete(bufHdr))
		{
			SpinRelease(BufMgrLock);
			elog(FATAL, "BufRead: buffer table broken after IO error\n");
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

	SpinRelease(BufMgrLock);

	if (status == SM_FAIL)
		return InvalidBuffer;

	return BufferDescriptorGetBuffer(bufHdr);
}

/*
 * BufferAlloc -- Get a buffer from the buffer pool but dont
 *		read it.
 *
 * Returns: descriptor for buffer
 *
 * When this routine returns, the BufMgrLock is guaranteed NOT be held.
 */
static BufferDesc *
BufferAlloc(Relation reln,
			BlockNumber blockNum,
			bool *foundPtr,
			bool bufferLockHeld)
{
	BufferDesc *buf,
			   *buf2;
	BufferTag	newTag;			/* identity of requested block */
	bool		inProgress;		/* buffer undergoing IO */
	bool		newblock = FALSE;

	/* create a new tag so we can lookup the buffer */
	/* assume that the relation is already open */
	if (blockNum == P_NEW)
	{
		newblock = TRUE;
		blockNum = smgrnblocks(DEFAULT_SMGR, reln);
	}

	INIT_BUFFERTAG(&newTag, reln, blockNum);

	if (!bufferLockHeld)
		SpinAcquire(BufMgrLock);

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
			WaitIO(buf, BufMgrLock);
			inProgress = (buf->flags & BM_IO_IN_PROGRESS);
		}
		if (BUFFER_IS_BROKEN(buf))
		{

			/*
			 * I couldn't understand the following old comment. If there's
			 * no IO for the buffer and the buffer is BROKEN,it should be
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
#endif	 /* BMTRACE */

		if (!(*foundPtr))
			StartBufferIO(buf, true);
		SpinRelease(BufMgrLock);

		return buf;
	}

	*foundPtr = FALSE;

	/*
	 * Didn't find it in the buffer pool.  We'll have to initialize a new
	 * buffer.	First, grab one from the free list.  If it's dirty, flush
	 * it to disk. Remember to unlock BufMgr spinlock while doing the IOs.
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

		if (buf->flags & BM_DIRTY)
		{
			bool		smok;

			/*
			 *	skip write error buffers 
			 */
			if ((buf->flags & BM_IO_ERROR) != 0)
			{
				PrivateRefCount[BufferDescriptorGetBuffer(buf) - 1] = 0;
				buf->refcount--;
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
				elog(NOTICE, "BufferAlloc: cannot write block %u for %s/%s",
				buf->tag.blockNum, buf->blind.dbname, buf->blind.relname);
				inProgress = FALSE;
				buf->flags |= BM_IO_ERROR;
				buf->flags &= ~BM_IO_IN_PROGRESS;
				TerminateBufferIO(buf);
				PrivateRefCount[BufferDescriptorGetBuffer(buf) - 1] = 0;
				Assert(buf->refcount > 0);
				buf->refcount--;
				if (buf->refcount == 0)
				{
					AddBufferToFreelist(buf);
					buf->flags |= BM_FREE;
				}
				buf = (BufferDesc *) NULL;
			}
			else
			{

				/*
				 * BM_JUST_DIRTIED cleared by BufferReplace and shouldn't
				 * be setted by anyone.		- vadim 01/17/97
				 */
				if (buf->flags & BM_JUST_DIRTIED)
				{
					elog(FATAL, "BufferAlloc: content of block %u (%s) changed while flushing",
						 buf->tag.blockNum, buf->blind.relname);
				}
				else
					buf->flags &= ~BM_DIRTY;
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
				PrivateRefCount[BufferDescriptorGetBuffer(buf) - 1] = 0;
				buf->refcount--;
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
					/* give up the buffer since we don't need it any more */
					PrivateRefCount[BufferDescriptorGetBuffer(buf) - 1] = 0;
					Assert(buf->refcount > 0);
					buf->refcount--;
					if (buf->refcount == 0)
					{
						AddBufferToFreelist(buf);
						buf->flags |= BM_FREE;
					}
				}

				PinBuffer(buf2);
				inProgress = (buf2->flags & BM_IO_IN_PROGRESS);

				*foundPtr = TRUE;
				if (inProgress)
				{
					WaitIO(buf2, BufMgrLock);
					inProgress = (buf2->flags & BM_IO_IN_PROGRESS);
				}
				if (BUFFER_IS_BROKEN(buf2))
					*foundPtr = FALSE;

				if (!(*foundPtr))
					StartBufferIO(buf2, true);
				SpinRelease(BufMgrLock);

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
		SpinRelease(BufMgrLock);
		elog(FATAL, "buffer wasn't in the buffer table\n");
	}

	/* record the database name and relation name for this buffer */
	strcpy(buf->blind.dbname, DatabaseName);
	strcpy(buf->blind.relname, RelationGetPhysicalRelationName(reln));
	buf->relId = reln->rd_lockInfo.lockRelId;

	INIT_BUFFERTAG(&(buf->tag), reln, blockNum);
	if (!BufTableInsert(buf))
	{
		SpinRelease(BufMgrLock);
		elog(FATAL, "Buffer in lookup table twice \n");
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
#endif	 /* BMTRACE */

	SpinRelease(BufMgrLock);

	return buf;
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

int
WriteBuffer(Buffer buffer)
{
	BufferDesc *bufHdr;

	if (BufferIsLocal(buffer))
		return WriteLocalBuffer(buffer, TRUE);

	if (BAD_BUFFER_ID(buffer))
		return FALSE;

	bufHdr = &BufferDescriptors[buffer - 1];

	SharedBufferChanged = true;

	SpinAcquire(BufMgrLock);
	Assert(bufHdr->refcount > 0);
	bufHdr->flags |= (BM_DIRTY | BM_JUST_DIRTIED);
	SetBufferDirtiedByMe(buffer, bufHdr);
	UnpinBuffer(bufHdr);
	SpinRelease(BufMgrLock);

	return TRUE;
}

#ifdef NOT_USED
void
WriteBuffer_Debug(char *file, int line, Buffer buffer)
{
	WriteBuffer(buffer);
	if (ShowPinTrace && BufferIsLocal(buffer) && is_userbuffer(buffer))
	{
		BufferDesc *buf;

		buf = &BufferDescriptors[buffer - 1];
		fprintf(stderr, "UNPIN(WR) %ld relname = %s, blockNum = %d, \
refcount = %ld, file: %s, line: %d\n",
				buffer, buf->blind.relname, buf->tag.blockNum,
				PrivateRefCount[buffer - 1], file, line);
	}
}

#endif

/*
 * FlushBuffer -- like WriteBuffer, but force the page to disk.
 *
 * 'buffer' is known to be dirty/pinned, so there should not be a
 * problem reading the BufferDesc members without the BufMgrLock
 * (nobody should be able to change tags out from under us).
 *
 * Unpin if 'release' is TRUE.
 */
int
FlushBuffer(Buffer buffer, bool release)
{
	BufferDesc *bufHdr;
	Oid			bufdb;
	Relation	bufrel;
	int			status;

	if (BufferIsLocal(buffer))
		return FlushLocalBuffer(buffer, release) ? STATUS_OK : STATUS_ERROR;

	if (BAD_BUFFER_ID(buffer))
		return STATUS_ERROR;

	Assert(PrivateRefCount[buffer - 1] > 0);	/* else caller didn't pin */

	bufHdr = &BufferDescriptors[buffer - 1];

	bufdb = bufHdr->relId.dbId;

	Assert(bufdb == MyDatabaseId || bufdb == (Oid) NULL);
	bufrel = RelationIdCacheGetRelation(bufHdr->relId.relId);

	Assert(bufrel != (Relation) NULL);

	SharedBufferChanged = true;

	/* To check if block content changed while flushing. - vadim 01/17/97 */
	SpinAcquire(BufMgrLock);
	WaitIO(bufHdr, BufMgrLock); /* confirm end of IO */
	bufHdr->flags &= ~BM_JUST_DIRTIED;
	StartBufferIO(bufHdr, false);		/* output IO start */

	SpinRelease(BufMgrLock);

	/*
	 * Grab a read lock on the buffer to ensure that no
	 * other backend changes its contents while we write it;
	 * see comments in BufferSync().
	 */
	LockBuffer(BufferDescriptorGetBuffer(bufHdr), BUFFER_LOCK_SHARE);

	status = smgrflush(DEFAULT_SMGR, bufrel, bufHdr->tag.blockNum,
					   (char *) MAKE_PTR(bufHdr->data));

	LockBuffer(BufferDescriptorGetBuffer(bufHdr), BUFFER_LOCK_UNLOCK);

	/* drop relcache refcnt incremented by RelationIdCacheGetRelation */
	RelationDecrementReferenceCount(bufrel);

	if (status == SM_FAIL)
	{
		elog(ERROR, "FlushBuffer: cannot flush block %u of the relation %s",
			 bufHdr->tag.blockNum, bufHdr->blind.relname);
		return STATUS_ERROR;
	}
	BufferFlushCount++;

	SpinAcquire(BufMgrLock);
	bufHdr->flags &= ~BM_IO_IN_PROGRESS;		/* mark IO finished */
	TerminateBufferIO(bufHdr);	/* output IO finished */

	/*
	 * If this buffer was marked by someone as DIRTY while we were
	 * flushing it out we must not clear shared DIRTY flag - vadim
	 * 01/17/97
	 *
	 * ... but we can clear BufferDirtiedByMe anyway - tgl 3/31/00
	 */
	if (bufHdr->flags & BM_JUST_DIRTIED)
	{
		elog(NOTICE, "FlushBuffer: content of block %u (%s) changed while flushing",
			 bufHdr->tag.blockNum, bufHdr->blind.relname);
	}
	else
		bufHdr->flags &= ~BM_DIRTY;
	ClearBufferDirtiedByMe(buffer, bufHdr);
	if (release)
		UnpinBuffer(bufHdr);
	SpinRelease(BufMgrLock);

	return STATUS_OK;
}

/*
 * WriteNoReleaseBuffer -- like WriteBuffer, but do not unpin the buffer
 *						   when the operation is complete.
 */
int
WriteNoReleaseBuffer(Buffer buffer)
{
	BufferDesc *bufHdr;

	if (BufferIsLocal(buffer))
		return WriteLocalBuffer(buffer, FALSE);

	if (BAD_BUFFER_ID(buffer))
		return STATUS_ERROR;

	bufHdr = &BufferDescriptors[buffer - 1];

	SharedBufferChanged = true;

	SpinAcquire(BufMgrLock);
	Assert(bufHdr->refcount > 0);
	bufHdr->flags |= (BM_DIRTY | BM_JUST_DIRTIED);
	SetBufferDirtiedByMe(buffer, bufHdr);
	SpinRelease(BufMgrLock);

	return STATUS_OK;
}


#undef ReleaseAndReadBuffer
/*
 * ReleaseAndReadBuffer -- combine ReleaseBuffer() and ReadBuffer()
 *		so that only one semop needs to be called.
 *
 */
Buffer
ReleaseAndReadBuffer(Buffer buffer,
					 Relation relation,
					 BlockNumber blockNum)
{
	BufferDesc *bufHdr;
	Buffer		retbuf;

	if (BufferIsLocal(buffer))
	{
		Assert(LocalRefCount[-buffer - 1] > 0);
		LocalRefCount[-buffer - 1]--;
	}
	else
	{
		if (BufferIsValid(buffer))
		{
			bufHdr = &BufferDescriptors[buffer - 1];
			Assert(PrivateRefCount[buffer - 1] > 0);
			PrivateRefCount[buffer - 1]--;
			if (PrivateRefCount[buffer - 1] == 0)
			{
				SpinAcquire(BufMgrLock);
				Assert(bufHdr->refcount > 0);
				bufHdr->refcount--;
				if (bufHdr->refcount == 0)
				{
					AddBufferToFreelist(bufHdr);
					bufHdr->flags |= BM_FREE;
				}
				retbuf = ReadBufferWithBufferLock(relation, blockNum, true);
				return retbuf;
			}
		}
	}

	return ReadBuffer(relation, blockNum);
}

/*
 * SetBufferDirtiedByMe -- mark a shared buffer as being dirtied by this xact
 *
 * This flag essentially remembers that we need to write and fsync this buffer
 * before we can commit the transaction.  The write might end up getting done
 * by another backend, but we must do the fsync ourselves (else we could
 * commit before the data actually reaches disk).  We do not issue fsync
 * instantly upon write; the storage manager keeps track of which files need
 * to be fsync'd before commit can occur.  A key aspect of this data structure
 * is that we will be able to notify the storage manager that an fsync is
 * needed even after another backend has done the physical write and replaced
 * the buffer contents with something else!
 *
 * NB: we must be holding the bufmgr lock at entry, and the buffer must be
 * pinned so that no other backend can take it away from us.
 */
static void
SetBufferDirtiedByMe(Buffer buffer, BufferDesc *bufHdr)
{
	BufferTag  *tagLastDirtied = &BufferTagLastDirtied[buffer - 1];
	Relation	reln;
	int			status;

	/*
	 * If the flag is already set, check to see whether the buffertag is
	 * the same.  If not, some other backend already wrote the buffer data
	 * that we dirtied.  We must tell the storage manager to make an fsync
	 * pending on that file before we can overwrite the old tag value.
	 */
	if (BufferDirtiedByMe[buffer - 1])
	{
		if (RelFileNodeEquals(bufHdr->tag.rnode, tagLastDirtied->rnode) &&
			bufHdr->tag.blockNum == tagLastDirtied->blockNum)
			return;				/* Same tag already dirtied, so no work */

#ifndef OPTIMIZE_SINGLE
		SpinRelease(BufMgrLock);
#endif	 /* OPTIMIZE_SINGLE */

		reln = RelationIdCacheGetRelation(BufferRelidLastDirtied[buffer - 1].relId);

		if (reln == (Relation) NULL)
		{
			status = smgrblindmarkdirty(DEFAULT_SMGR,
							tagLastDirtied->rnode,
							tagLastDirtied->blockNum);
		}
		else
		{
			Assert(RelFileNodeEquals(tagLastDirtied->rnode, reln->rd_node));
			status = smgrmarkdirty(DEFAULT_SMGR, reln,
								   tagLastDirtied->blockNum);

			/*
			 * drop relcache refcnt incremented by
			 * RelationIdCacheGetRelation
			 */
			RelationDecrementReferenceCount(reln);
		}
		if (status == SM_FAIL)
		{
			elog(ERROR, "SetBufferDirtiedByMe: cannot mark %u for %s",
				 tagLastDirtied->blockNum,
				 BufferBlindLastDirtied[buffer - 1].relname);
		}

#ifndef OPTIMIZE_SINGLE
		SpinAcquire(BufMgrLock);
#endif	 /* OPTIMIZE_SINGLE */

	}

	*tagLastDirtied = bufHdr->tag;
	BufferRelidLastDirtied[buffer - 1] = bufHdr->relId;
	BufferBlindLastDirtied[buffer - 1] = bufHdr->blind;
	BufferDirtiedByMe[buffer - 1] = true;
}

/*
 * ClearBufferDirtiedByMe -- mark a shared buffer as no longer needing fsync
 *
 * If we write out a buffer ourselves, then the storage manager will set its
 * needs-fsync flag for that file automatically, and so we can clear our own
 * flag that says it needs to be done later.
 *
 * NB: we must be holding the bufmgr lock at entry.
 */
static void
ClearBufferDirtiedByMe(Buffer buffer, BufferDesc *bufHdr)
{
	BufferTag  *tagLastDirtied = &BufferTagLastDirtied[buffer - 1];

	/*
	 * Do *not* clear the flag if it refers to some other buffertag than
	 * the data we just wrote.	This is unlikely, but possible if some
	 * other backend replaced the buffer contents since we set our flag.
	 */
	if (RelFileNodeEquals(bufHdr->tag.rnode, tagLastDirtied->rnode) &&
			bufHdr->tag.blockNum == tagLastDirtied->blockNum)
		BufferDirtiedByMe[buffer - 1] = false;
}

/*
 * BufferSync -- Flush all dirty buffers in the pool.
 *
 *		This is called at transaction commit time.	We find all buffers
 *		that have been dirtied by the current xact and flush them to disk.
 *		We do *not* flush dirty buffers that have been dirtied by other xacts.
 *		(This is a substantial change from pre-7.0 behavior.)
 */
static void
BufferSync()
{
	int			i;
	BufferDesc *bufHdr;
	int			status;
	Relation	reln;
	bool		didwrite;

	for (i = 0, bufHdr = BufferDescriptors; i < NBuffers; i++, bufHdr++)
	{
		/* Ignore buffers that were not dirtied by me */
		if (!BufferDirtiedByMe[i])
			continue;

		SpinAcquire(BufMgrLock);

		/*
		 * We only need to write if the buffer is still dirty and still
		 * contains the same disk page that it contained when we dirtied
		 * it. Otherwise, someone else has already written our changes for
		 * us, and we need only fsync.
		 *
		 * (NOTE: it's still possible to do an unnecessary write, if other
		 * xacts have written and then re-dirtied the page since our last
		 * change to it.  But that should be pretty uncommon, and there's
		 * no easy way to detect it anyway.)
		 */
		reln = NULL;
		didwrite = false;
		if ((bufHdr->flags & BM_VALID) && (bufHdr->flags & BM_DIRTY))
		{
			if (RelFileNodeEquals(bufHdr->tag.rnode, BufferTagLastDirtied[i].rnode) &&
				bufHdr->tag.blockNum == BufferTagLastDirtied[i].blockNum)
			{
				Oid		bufrel = bufHdr->relId.relId;

				/*
				 * Try to find relation for buf.  This could fail, if the
				 * rel has been flushed from the relcache since we dirtied
				 * the page.  That should be uncommon, so paying the extra
				 * cost of a blind write when it happens seems OK.
				 */
				reln = RelationIdCacheGetRelation(bufrel);

				/*
				 * We have to pin buffer to keep anyone from stealing it
				 * from the buffer pool while we are flushing it or
				 * waiting in WaitIO. It's bad for GetFreeBuffer in
				 * BufferAlloc, but there is no other way to prevent
				 * writing into disk block data from some other buffer,
				 * getting smgr status of some other block and clearing
				 * BM_DIRTY of ...			  - VAdim 09/16/96
				 */
				PinBuffer(bufHdr);
				if (bufHdr->flags & BM_IO_IN_PROGRESS)
				{
					WaitIO(bufHdr, BufMgrLock);
					UnpinBuffer(bufHdr);
					if (bufHdr->flags & BM_IO_ERROR)
					{
						elog(ERROR, "BufferSync: write error %u for %s",
							 bufHdr->tag.blockNum, bufHdr->blind.relname);
					}
				}
				else
				{

					/*
					 * To check if block content changed while flushing
					 * (see below). - vadim 01/17/97
					 */
					WaitIO(bufHdr, BufMgrLock); /* confirm end of IO */
					bufHdr->flags &= ~BM_JUST_DIRTIED;
					StartBufferIO(bufHdr, false);		/* output IO start */

					SpinRelease(BufMgrLock);

					/*
					 * Grab a read lock on the buffer to ensure that no
					 * other backend changes its contents while we write it;
					 * otherwise we could write a non-self-consistent page
					 * image to disk, which'd be bad news if the other
					 * transaction aborts before writing its changes.
					 *
					 * Note that we still need the BM_JUST_DIRTIED mechanism
					 * in case someone dirties the buffer just before we
					 * grab this lock or just after we release it.
					 */
					LockBuffer(BufferDescriptorGetBuffer(bufHdr),
							   BUFFER_LOCK_SHARE);

					/*
					 * If we didn't have the reldesc in our local cache,
					 * write this page out using the 'blind write' storage
					 * manager routine.  If we did find it, use the
					 * standard interface.
					 */
					if (reln == (Relation) NULL)
					{
						status = smgrblindwrt(DEFAULT_SMGR,
											bufHdr->tag.rnode,
											bufHdr->tag.blockNum,
											(char *) MAKE_PTR(bufHdr->data),
											true);	/* must fsync */
					}
					else
					{
						Assert(RelFileNodeEquals(reln->rd_node,
									BufferTagLastDirtied[i].rnode));
						status = smgrwrite(DEFAULT_SMGR, reln,
										   bufHdr->tag.blockNum,
										(char *) MAKE_PTR(bufHdr->data));
					}

					/*
					 * Release the per-buffer readlock, reacquire BufMgrLock.
					 */
					LockBuffer(BufferDescriptorGetBuffer(bufHdr),
							   BUFFER_LOCK_UNLOCK);

					SpinAcquire(BufMgrLock);

					UnpinBuffer(bufHdr);
					if (status == SM_FAIL)
					{
						bufHdr->flags |= BM_IO_ERROR;
						elog(ERROR, "BufferSync: cannot write %u for %s",
							 bufHdr->tag.blockNum, bufHdr->blind.relname);
					}
					bufHdr->flags &= ~BM_IO_IN_PROGRESS;		/* mark IO finished */
					TerminateBufferIO(bufHdr);	/* Sync IO finished */
					BufferFlushCount++;
					didwrite = true;

					/*
					 * If this buffer was marked by someone as DIRTY while
					 * we were flushing it out we must not clear DIRTY
					 * flag - vadim 01/17/97
					 *
					 * but it is OK to clear BufferDirtiedByMe - tgl 3/31/00
					 */
					if (!(bufHdr->flags & BM_JUST_DIRTIED))
						bufHdr->flags &= ~BM_DIRTY;
				}

				/* drop refcnt obtained by RelationIdCacheGetRelation */
				if (reln != (Relation) NULL)
					RelationDecrementReferenceCount(reln);
			}
		}

		/*
		 * If we did not write the buffer (because someone else did), we
		 * must still fsync the file containing it, to ensure that the
		 * write is down to disk before we commit.
		 */
		if (!didwrite)
		{
#ifndef OPTIMIZE_SINGLE
			SpinRelease(BufMgrLock);
#endif	 /* OPTIMIZE_SINGLE */

			reln = RelationIdCacheGetRelation(BufferRelidLastDirtied[i].relId);
			if (reln == (Relation) NULL)
			{
				status = smgrblindmarkdirty(DEFAULT_SMGR,
									BufferTagLastDirtied[i].rnode,
									BufferTagLastDirtied[i].blockNum);
			}
			else
			{
				Assert(RelFileNodeEquals(reln->rd_node,
							BufferTagLastDirtied[i].rnode));
				status = smgrmarkdirty(DEFAULT_SMGR, reln,
									   BufferTagLastDirtied[i].blockNum);

				/*
				 * drop relcache refcnt incremented by
				 * RelationIdCacheGetRelation
				 */
				RelationDecrementReferenceCount(reln);

			}
#ifndef OPTIMIZE_SINGLE
			SpinAcquire(BufMgrLock);
#endif	 /* OPTIMIZE_SINGLE */
		}

		BufferDirtiedByMe[i] = false;

		SpinRelease(BufMgrLock);
	}

	LocalBufferSync();
}


/*
 * WaitIO -- Block until the IO_IN_PROGRESS flag on 'buf' is cleared.
 *
 * Should be entered with buffer manager spinlock held; releases it before
 * waiting and re-acquires it afterwards.
 *
 * OLD NOTES:
 *		Because IO_IN_PROGRESS conflicts are
 *		expected to be rare, there is only one BufferIO
 *		lock in the entire system.	All processes block
 *		on this semaphore when they try to use a buffer
 *		that someone else is faulting in.  Whenever a
 *		process finishes an IO and someone is waiting for
 *		the buffer, BufferIO is signaled (SignalIO).  All
 *		waiting processes then wake up and check to see
 *		if their buffer is now ready.  This implementation
 *		is simple, but efficient enough if WaitIO is
 *		rarely called by multiple processes simultaneously.
 *
 * NEW NOTES:
 *		The above is true only on machines without test-and-set
 *		semaphores (which we hope are few, these days).  On better
 *		hardware, each buffer has a spinlock that we can wait on.
 */
#ifdef HAS_TEST_AND_SET

static void
WaitIO(BufferDesc *buf, SPINLOCK spinlock)
{

	/*
	 * Changed to wait until there's no IO - Inoue 01/13/2000
	 */
	while ((buf->flags & BM_IO_IN_PROGRESS) != 0)
	{
		SpinRelease(spinlock);
		S_LOCK(&(buf->io_in_progress_lock));
		S_UNLOCK(&(buf->io_in_progress_lock));
		SpinAcquire(spinlock);
	}
}

#else							/* !HAS_TEST_AND_SET */

IpcSemaphoreId WaitIOSemId;
IpcSemaphoreId WaitCLSemId;

static void
WaitIO(BufferDesc *buf, SPINLOCK spinlock)
{
	bool		inProgress;

	for (;;)
	{

		/* wait until someone releases IO lock */
		(*NWaitIOBackendP)++;
		SpinRelease(spinlock);
		IpcSemaphoreLock(WaitIOSemId, 0, 1);
		SpinAcquire(spinlock);
		inProgress = (buf->flags & BM_IO_IN_PROGRESS);
		if (!inProgress)
			break;
	}
}

/*
 * SignalIO
 */
static void
SignalIO(BufferDesc *buf)
{
	/* somebody better be waiting. */
	Assert(buf->refcount > 1);
	IpcSemaphoreUnlock(WaitIOSemId, 0, *NWaitIOBackendP);
	*NWaitIOBackendP = 0;
}

#endif	 /* HAS_TEST_AND_SET */

long		NDirectFileRead;	/* some I/O's are direct file access.
								 * bypass bufmgr */
long		NDirectFileWrite;	/* e.g., I/O in psort and hashjoin.					*/

void
PrintBufferUsage(FILE *statfp)
{
	float		hitrate;
	float		localhitrate;

	if (ReadBufferCount == 0)
		hitrate = 0.0;
	else
		hitrate = (float) BufferHitCount *100.0 / ReadBufferCount;

	if (ReadLocalBufferCount == 0)
		localhitrate = 0.0;
	else
		localhitrate = (float) LocalBufferHitCount *100.0 / ReadLocalBufferCount;

	fprintf(statfp, "!\tShared blocks: %10ld read, %10ld written, buffer hit rate = %.2f%%\n",
			ReadBufferCount - BufferHitCount, BufferFlushCount, hitrate);
	fprintf(statfp, "!\tLocal  blocks: %10ld read, %10ld written, buffer hit rate = %.2f%%\n",
			ReadLocalBufferCount - LocalBufferHitCount, LocalBufferFlushCount, localhitrate);
	fprintf(statfp, "!\tDirect blocks: %10ld read, %10ld written\n",
			NDirectFileRead, NDirectFileWrite);
}

void
ResetBufferUsage()
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

/* ----------------------------------------------
 *		ResetBufferPool
 *
 *		This routine is supposed to be called when a transaction aborts.
 *		it will release all the buffer pins held by the transaction.
 *		Currently, we also call it during commit if BufferPoolCheckLeak
 *		detected a problem --- in that case, isCommit is TRUE, and we
 *		only clean up buffer pin counts.
 *
 * During abort, we also forget any pending fsync requests.  Dirtied buffers
 * will still get written, eventually, but there will be no fsync for them.
 *
 * ----------------------------------------------
 */
void
ResetBufferPool(bool isCommit)
{
	int			i;

	for (i = 0; i < NBuffers; i++)
	{
		if (PrivateRefCount[i] != 0)
		{
			BufferDesc *buf = &BufferDescriptors[i];

			SpinAcquire(BufMgrLock);
			Assert(buf->refcount > 0);
			buf->refcount--;
			if (buf->refcount == 0)
			{
				AddBufferToFreelist(buf);
				buf->flags |= BM_FREE;
			}
			SpinRelease(BufMgrLock);
		}
		PrivateRefCount[i] = 0;

		if (!isCommit)
			BufferDirtiedByMe[i] = false;
	}

	ResetLocalBufferPool();

	if (!isCommit)
		smgrabort();
}

/* -----------------------------------------------
 *		BufferPoolCheckLeak
 *
 *		check if there is buffer leak
 *
 * -----------------------------------------------
 */
int
BufferPoolCheckLeak()
{
	int			i;
	int			result = 0;

	for (i = 1; i <= NBuffers; i++)
	{
		if (PrivateRefCount[i - 1] != 0)
		{
			BufferDesc *buf = &(BufferDescriptors[i - 1]);

			elog(NOTICE,
				 "Buffer Leak: [%03d] (freeNext=%ld, freePrev=%ld, \
relname=%s, blockNum=%d, flags=0x%x, refcount=%d %ld)",
				 i - 1, buf->freeNext, buf->freePrev,
				 buf->blind.relname, buf->tag.blockNum, buf->flags,
				 buf->refcount, PrivateRefCount[i - 1]);
			result = 1;
		}
	}
	return result;
}

/* ------------------------------------------------
 *		FlushBufferPool
 *
 *		flush all dirty blocks in buffer pool to disk
 *
 * ------------------------------------------------
 */
void
FlushBufferPool(void)
{
	BufferSync();
	smgrcommit();
}

/*
 * BufferGetBlockNumber
 *		Returns the block number associated with a buffer.
 *
 * Note:
 *		Assumes that the buffer is valid.
 */
BlockNumber
BufferGetBlockNumber(Buffer buffer)
{
	Assert(BufferIsValid(buffer));

	/* XXX should be a critical section */
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
 * This routine used to flush the data to disk (ie, force immediate fsync)
 * but that's no longer necessary because BufferSync is smarter than before.
 *
 * BufMgrLock must be held at entry, and the buffer must be pinned.
 */
static int
BufferReplace(BufferDesc *bufHdr)
{
	Relation	reln;
	Oid			bufdb,
				bufrel;
	int			status;

	/*
	 * first try to find the reldesc in the cache, if no luck, don't
	 * bother to build the reldesc from scratch, just do a blind write.
	 */
	bufdb = bufHdr->relId.dbId;
	bufrel = bufHdr->relId.relId;

	if (bufdb == MyDatabaseId || bufdb == (Oid) NULL)
		reln = RelationIdCacheGetRelation(bufrel);
	else
		reln = (Relation) NULL;

	/* To check if block content changed while flushing. - vadim 01/17/97 */
	bufHdr->flags &= ~BM_JUST_DIRTIED;

	SpinRelease(BufMgrLock);

	/*
	 * Grab a read lock on the buffer to ensure that no
	 * other backend changes its contents while we write it;
	 * see comments in BufferSync().
	 */
	LockBuffer(BufferDescriptorGetBuffer(bufHdr), BUFFER_LOCK_SHARE);

	if (reln != (Relation) NULL)
	{
		Assert(RelFileNodeEquals(bufHdr->tag.rnode, reln->rd_node));
		status = smgrwrite(DEFAULT_SMGR, reln, bufHdr->tag.blockNum,
						   (char *) MAKE_PTR(bufHdr->data));
	}
	else
	{
		status = smgrblindwrt(DEFAULT_SMGR, bufHdr->tag.rnode,
							  bufHdr->tag.blockNum,
							  (char *) MAKE_PTR(bufHdr->data),
							  false);	/* no fsync */
	}

	LockBuffer(BufferDescriptorGetBuffer(bufHdr), BUFFER_LOCK_UNLOCK);

	SpinAcquire(BufMgrLock);

	/* drop relcache refcnt incremented by RelationIdCacheGetRelation */
	if (reln != (Relation) NULL)
		RelationDecrementReferenceCount(reln);

	if (status == SM_FAIL)
		return FALSE;

	/*
	 * If we had marked this buffer as needing to be fsync'd, we can
	 * forget about that, because it's now the storage manager's
	 * responsibility (but only if we called smgrwrite, not smgrblindwrt).
	 */
	if (reln != (Relation) NULL)
		ClearBufferDirtiedByMe(BufferDescriptorGetBuffer(bufHdr), bufHdr);

	BufferFlushCount++;

	return TRUE;
}

/*
 * RelationGetNumberOfBlocks
 *		Returns the buffer descriptor associated with a page in a relation.
 *
 * Note:
 *		XXX may fail for huge relations.
 *		XXX should be elsewhere.
 *		XXX maybe should be hidden
 */
BlockNumber
RelationGetNumberOfBlocks(Relation relation)
{
	return ((relation->rd_myxactonly) ? relation->rd_nblocks :
			smgrnblocks(DEFAULT_SMGR, relation));
}

/* ---------------------------------------------------------------------
 *		ReleaseRelationBuffers
 *
 *		This function removes all the buffered pages for a relation
 *		from the buffer pool.  Dirty pages are simply dropped, without
 *		bothering to write them out first.  This is used when the
 *		relation is about to be deleted.  We assume that the caller
 *		holds an exclusive lock on the relation, which should assure
 *		that no new buffers will be acquired for the rel meanwhile.
 *
 *		XXX currently it sequentially searches the buffer pool, should be
 *		changed to more clever ways of searching.
 * --------------------------------------------------------------------
 */
void
ReleaseRelationBuffers(Relation rel)
{
	Oid			relid = RelationGetRelid(rel);
	int			i;
	BufferDesc *bufHdr;

	if (rel->rd_myxactonly)
	{
		for (i = 0; i < NLocBuffer; i++)
		{
			bufHdr = &LocalBufferDescriptors[i];
			if (RelFileNodeEquals(bufHdr->tag.rnode, rel->rd_node))
			{
				bufHdr->flags &= ~(BM_DIRTY | BM_JUST_DIRTIED);
				LocalRefCount[i] = 0;
				bufHdr->tag.rnode.relNode = InvalidOid;
			}
			else
			{
				Assert(bufHdr->relId.relId != relid);
			}
		}
		return;
	}

	SpinAcquire(BufMgrLock);
	for (i = 1; i <= NBuffers; i++)
	{
		bufHdr = &BufferDescriptors[i - 1];
recheck:
		if (RelFileNodeEquals(bufHdr->tag.rnode, rel->rd_node))
		{

			/*
			 * If there is I/O in progress, better wait till it's done;
			 * don't want to delete the relation out from under someone
			 * who's just trying to flush the buffer!
			 */
			if (bufHdr->flags & BM_IO_IN_PROGRESS)
			{
				WaitIO(bufHdr, BufMgrLock);

				/*
				 * By now, the buffer very possibly belongs to some other
				 * rel, so check again before proceeding.
				 */
				goto recheck;
			}
			/* Now we can do what we came for */
			bufHdr->flags &= ~(BM_DIRTY | BM_JUST_DIRTIED);
			BufferDirtiedByMe[i - 1] = false;

			/*
			 * Release any refcount we may have.
			 *
			 * This is very probably dead code, and if it isn't then it's
			 * probably wrong.	I added the Assert to find out --- tgl
			 * 11/99.
			 */
			if (!(bufHdr->flags & BM_FREE))
			{
				/* Assert checks that buffer will actually get freed! */
				Assert(PrivateRefCount[i - 1] == 1 &&
					   bufHdr->refcount == 1);
				/* ReleaseBuffer expects we do not hold the lock at entry */
				SpinRelease(BufMgrLock);
				ReleaseBuffer(i);
				SpinAcquire(BufMgrLock);
			}
			/*
			 * And mark the buffer as no longer occupied by this rel.
			 */
			BufTableDelete(bufHdr);
		}
		else
		{
			Assert(bufHdr->relId.relId != relid ||
			(bufHdr->relId.dbId != MyDatabaseId &&
			 bufHdr->relId.dbId != InvalidOid));
			if (RelFileNodeEquals(rel->rd_node, 
					BufferTagLastDirtied[i - 1].rnode))
				BufferDirtiedByMe[i - 1] = false;
		}
	}

	SpinRelease(BufMgrLock);
}

/* ---------------------------------------------------------------------
 *		DropBuffers
 *
 *		This function removes all the buffers in the buffer cache for a
 *		particular database.  Dirty pages are simply dropped, without
 *		bothering to write them out first.  This is used when we destroy a
 *		database, to avoid trying to flush data to disk when the directory
 *		tree no longer exists.	Implementation is pretty similar to
 *		ReleaseRelationBuffers() which is for destroying just one relation.
 * --------------------------------------------------------------------
 */
void
DropBuffers(Oid dbid)
{
	int			i;
	BufferDesc *bufHdr;

	SpinAcquire(BufMgrLock);
	for (i = 1; i <= NBuffers; i++)
	{
		bufHdr = &BufferDescriptors[i - 1];
recheck:
		/*
		 * We know that currently database OID is tblNode but
		 * this probably will be changed in future and this
		 * func will be used to drop tablespace buffers.
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
				WaitIO(bufHdr, BufMgrLock);

				/*
				 * By now, the buffer very possibly belongs to some other
				 * DB, so check again before proceeding.
				 */
				goto recheck;
			}
			/* Now we can do what we came for */
			bufHdr->flags &= ~(BM_DIRTY | BM_JUST_DIRTIED);
			BufferDirtiedByMe[i - 1] = false;

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
		else
		{
			Assert(bufHdr->relId.dbId != dbid);
			if (BufferTagLastDirtied[i - 1].rnode.tblNode == dbid)
				BufferDirtiedByMe[i - 1] = false;
		}
	}
	SpinRelease(BufMgrLock);
}

/* -----------------------------------------------------------------
 *		PrintBufferDescs
 *
 *		this function prints all the buffer descriptors, for debugging
 *		use only.
 * -----------------------------------------------------------------
 */
void
PrintBufferDescs()
{
	int			i;
	BufferDesc *buf = BufferDescriptors;

	if (IsUnderPostmaster)
	{
		SpinAcquire(BufMgrLock);
		for (i = 0; i < NBuffers; ++i, ++buf)
		{
			elog(DEBUG, "[%02d] (freeNext=%ld, freePrev=%ld, relname=%s, \
blockNum=%d, flags=0x%x, refcount=%d %ld)",
				 i, buf->freeNext, buf->freePrev,
				 buf->blind.relname, buf->tag.blockNum, buf->flags,
				 buf->refcount, PrivateRefCount[i]);
		}
		SpinRelease(BufMgrLock);
	}
	else
	{
		/* interactive backend */
		for (i = 0; i < NBuffers; ++i, ++buf)
		{
			printf("[%-2d] (%s, %d) flags=0x%x, refcnt=%d %ld)\n",
					i, buf->blind.relname, buf->tag.blockNum,
					buf->flags, buf->refcount, PrivateRefCount[i]);
		}
	}
}

void
PrintPinnedBufs()
{
	int			i;
	BufferDesc *buf = BufferDescriptors;

	SpinAcquire(BufMgrLock);
	for (i = 0; i < NBuffers; ++i, ++buf)
	{
		if (PrivateRefCount[i] > 0)
			elog(NOTICE, "[%02d] (freeNext=%ld, freePrev=%ld, relname=%s, \
blockNum=%d, flags=0x%x, refcount=%d %ld)\n",
				 i, buf->freeNext, buf->freePrev, buf->blind.relname,
				 buf->tag.blockNum, buf->flags,
				 buf->refcount, PrivateRefCount[i]);
	}
	SpinRelease(BufMgrLock);
}

/*
 * BufferPoolBlowaway
 *
 * this routine is solely for the purpose of experiments -- sometimes
 * you may want to blowaway whatever is left from the past in buffer
 * pool and start measuring some performance with a clean empty buffer
 * pool.
 */
#ifdef NOT_USED
void
BufferPoolBlowaway()
{
	int			i;

	BufferSync();
	for (i = 1; i <= NBuffers; i++)
	{
		if (BufferIsValid(i))
		{
			while (BufferIsValid(i))
				ReleaseBuffer(i);
		}
		BufTableDelete(&BufferDescriptors[i - 1]);
	}
}

#endif

/* ---------------------------------------------------------------------
 *		FlushRelationBuffers
 *
 *		This function flushes all dirty pages of a relation out to disk.
 *		Furthermore, pages that have blocknumber >= firstDelBlock are
 *		actually removed from the buffer pool.  An error code is returned
 *		if we fail to dump a dirty buffer or if we find one of
 *		the target pages is pinned into the cache.
 *
 *		This is used by VACUUM before truncating the relation to the given
 *		number of blocks.  (TRUNCATE TABLE also uses it in the same way.)
 *		It might seem unnecessary to flush dirty pages before firstDelBlock,
 *		since VACUUM should already have committed its changes.  However,
 *		it is possible for there still to be dirty pages: if some page
 *		had unwritten on-row tuple status updates from a prior transaction,
 *		and VACUUM had no additional changes to make to that page, then
 *		VACUUM won't have written it.  This is harmless in most cases but
 *		will break pg_upgrade, which relies on VACUUM to ensure that *all*
 *		tuples have correct on-row status.  So, we check and flush all
 *		dirty pages of the rel regardless of block number.
 *
 *		This is also used by RENAME TABLE (with firstDelBlock = 0)
 *		to clear out the buffer cache before renaming the physical files of
 *		a relation.  Without that, some other backend might try to do a
 *		blind write of a buffer page (relying on the BlindId of the buffer)
 *		and fail because it's not got the right filename anymore.
 *
 *		In all cases, the caller should be holding AccessExclusiveLock on
 *		the target relation to ensure that no other backend is busy reading
 *		more blocks of the relation.
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
	Oid			relid = RelationGetRelid(rel);
	int			i;
	BufferDesc *bufHdr;

	if (rel->rd_myxactonly)
	{
		for (i = 0; i < NLocBuffer; i++)
		{
			bufHdr = &LocalBufferDescriptors[i];
			if (RelFileNodeEquals(bufHdr->tag.rnode, rel->rd_node))
			{
				if (bufHdr->flags & BM_DIRTY)
				{
					if (FlushBuffer(-i - 1, false) != STATUS_OK)
					{
						elog(NOTICE, "FlushRelationBuffers(%s (local), %u): block %u is dirty, could not flush it",
							 RelationGetRelationName(rel), firstDelBlock,
							 bufHdr->tag.blockNum);
						return -1;
					}
				}
				if (LocalRefCount[i] > 0)
				{
					elog(NOTICE, "FlushRelationBuffers(%s (local), %u): block %u is referenced (%ld)",
						 RelationGetRelationName(rel), firstDelBlock,
						 bufHdr->tag.blockNum, LocalRefCount[i]);
					return -2;
				}
				if (bufHdr->tag.blockNum >= firstDelBlock)
				{
					bufHdr->tag.rnode.relNode = InvalidOid;
				}
			}
			else
			{
				Assert(bufHdr->relId.relId != relid);
			}
		}
		return 0;
	}

	SpinAcquire(BufMgrLock);
	for (i = 0; i < NBuffers; i++)
	{
		bufHdr = &BufferDescriptors[i];
recheck:
		if (RelFileNodeEquals(bufHdr->tag.rnode, rel->rd_node))
		{
			if (bufHdr->flags & BM_DIRTY)
			{
				PinBuffer(bufHdr);
				SpinRelease(BufMgrLock);
				if (FlushBuffer(i + 1, true) != STATUS_OK)
				{
					elog(NOTICE, "FlushRelationBuffers(%s, %u): block %u is dirty (private %ld, global %d), could not flush it",
						 RelationGetRelationName(rel), firstDelBlock,
						 bufHdr->tag.blockNum,
						 PrivateRefCount[i], bufHdr->refcount);
					return -1;
				}
				SpinAcquire(BufMgrLock);

				/*
				 * Buffer could already be reassigned, so must recheck
				 * whether it still belongs to rel before freeing it!
				 */
				goto recheck;
			}
			if (!(bufHdr->flags & BM_FREE))
			{
				SpinRelease(BufMgrLock);
				elog(NOTICE, "FlushRelationBuffers(%s, %u): block %u is referenced (private %ld, global %d)",
					 RelationGetRelationName(rel), firstDelBlock,
					 bufHdr->tag.blockNum,
					 PrivateRefCount[i], bufHdr->refcount);
				return -2;
			}
			if (bufHdr->tag.blockNum >= firstDelBlock)
			{
				BufTableDelete(bufHdr);
			}
		}
		else
		{
			Assert(bufHdr->relId.relId != relid ||
				(bufHdr->relId.dbId != MyDatabaseId &&
				bufHdr->relId.dbId != InvalidOid));
		}
	}
	SpinRelease(BufMgrLock);
	return 0;
}

#undef ReleaseBuffer

/*
 * ReleaseBuffer -- remove the pin on a buffer without
 *		marking it dirty.
 *
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
	PrivateRefCount[buffer - 1]--;
	if (PrivateRefCount[buffer - 1] == 0)
	{
		SpinAcquire(BufMgrLock);
		Assert(bufHdr->refcount > 0);
		bufHdr->refcount--;
		if (bufHdr->refcount == 0)
		{
			AddBufferToFreelist(bufHdr);
			bufHdr->flags |= BM_FREE;
		}
		SpinRelease(BufMgrLock);
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

		fprintf(stderr, "PIN(Incr) %ld relname = %s, blockNum = %d, \
refcount = %ld, file: %s, line: %d\n",
				buffer, buf->blind.relname, buf->tag.blockNum,
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

		fprintf(stderr, "UNPIN(Rel) %ld relname = %s, blockNum = %d, \
refcount = %ld, file: %s, line: %d\n",
				buffer, buf->blind.relname, buf->tag.blockNum,
				PrivateRefCount[buffer - 1], file, line);
	}
}

#endif

#ifdef NOT_USED
int
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

		fprintf(stderr, "UNPIN(Rel&Rd) %ld relname = %s, blockNum = %d, \
refcount = %ld, file: %s, line: %d\n",
				buffer, buf->blind.relname, buf->tag.blockNum,
				PrivateRefCount[buffer - 1], file, line);
	}
	if (ShowPinTrace && BufferIsLocal(buffer) && is_userbuffer(buffer))
	{
		BufferDesc *buf = &BufferDescriptors[b - 1];

		fprintf(stderr, "PIN(Rel&Rd) %ld relname = %s, blockNum = %d, \
refcount = %ld, file: %s, line: %d\n",
				b, buf->blind.relname, buf->tag.blockNum,
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
			fprintf(fp, "     [%3d]%spid %d buf %2d for <%d,%u,%d> ",
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

#endif	 /* BMTRACE */

/*
 * SetBufferCommitInfoNeedsSave
 *
 *	Mark a buffer dirty when we have updated tuple commit-status bits in it.
 *
 * This is similar to WriteNoReleaseBuffer, except that we do not set
 * SharedBufferChanged or BufferDirtiedByMe, because we have not made a
 * critical change that has to be flushed to disk before xact commit --- the
 * status-bit update could be redone by someone else just as easily.  The
 * buffer will be marked dirty, but it will not be written to disk until
 * there is another reason to write it.
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
		return;

	if (BAD_BUFFER_ID(buffer))
		return;

	bufHdr = &BufferDescriptors[buffer - 1];

	if ((bufHdr->flags & (BM_DIRTY | BM_JUST_DIRTIED)) !=
		(BM_DIRTY | BM_JUST_DIRTIED))
	{
		SpinAcquire(BufMgrLock);
		Assert(bufHdr->refcount > 0);
		bufHdr->flags |= (BM_DIRTY | BM_JUST_DIRTIED);
		SpinRelease(BufMgrLock);
	}
}

void
UnlockBuffers()
{
	BufferDesc *buf;
	int			i;

	for (i = 0; i < NBuffers; i++)
	{
		if (BufferLocks[i] == 0)
			continue;

		Assert(BufferIsValid(i + 1));
		buf = &(BufferDescriptors[i]);

#ifdef HAS_TEST_AND_SET
		S_LOCK(&(buf->cntx_lock));
#else
		IpcSemaphoreLock(WaitCLSemId, 0, IpcExclusiveLock);
#endif

		if (BufferLocks[i] & BL_R_LOCK)
		{
			Assert(buf->r_locks > 0);
			(buf->r_locks)--;
		}
		if (BufferLocks[i] & BL_RI_LOCK)
		{

			/*
			 * Someone else could remove our RI lock when acquiring W
			 * lock. This is possible if we came here from elog(ERROR)
			 * from IpcSemaphore{Lock|Unlock}(WaitCLSemId). And so we
			 * don't do Assert(buf->ri_lock) here.
			 */
			buf->ri_lock = false;
		}
		if (BufferLocks[i] & BL_W_LOCK)
		{
			Assert(buf->w_lock);
			buf->w_lock = false;
		}
#ifdef HAS_TEST_AND_SET
		S_UNLOCK(&(buf->cntx_lock));
#else
		IpcSemaphoreUnlock(WaitCLSemId, 0, IpcExclusiveLock);
#endif
		BufferLocks[i] = 0;
	}
}

void
LockBuffer(Buffer buffer, int mode)
{
	BufferDesc *buf;
	bits8	   *buflock;

	Assert(BufferIsValid(buffer));
	if (BufferIsLocal(buffer))
		return;

	buf = &(BufferDescriptors[buffer - 1]);
	buflock = &(BufferLocks[buffer - 1]);

#ifdef HAS_TEST_AND_SET
	S_LOCK(&(buf->cntx_lock));
#else
	IpcSemaphoreLock(WaitCLSemId, 0, IpcExclusiveLock);
#endif

	if (mode == BUFFER_LOCK_UNLOCK)
	{
		if (*buflock & BL_R_LOCK)
		{
			Assert(buf->r_locks > 0);
			Assert(!(buf->w_lock));
			Assert(!(*buflock & (BL_W_LOCK | BL_RI_LOCK)));
			(buf->r_locks)--;
			*buflock &= ~BL_R_LOCK;
		}
		else if (*buflock & BL_W_LOCK)
		{
			Assert(buf->w_lock);
			Assert(buf->r_locks == 0);
			Assert(!(*buflock & (BL_R_LOCK | BL_RI_LOCK)));
			buf->w_lock = false;
			*buflock &= ~BL_W_LOCK;
		}
		else
			elog(ERROR, "UNLockBuffer: buffer %lu is not locked", buffer);
	}
	else if (mode == BUFFER_LOCK_SHARE)
	{
		unsigned	i = 0;

		Assert(!(*buflock & (BL_R_LOCK | BL_W_LOCK | BL_RI_LOCK)));
		while (buf->ri_lock || buf->w_lock)
		{
#ifdef HAS_TEST_AND_SET
			S_UNLOCK(&(buf->cntx_lock));
			s_lock_sleep(i++);
			S_LOCK(&(buf->cntx_lock));
#else
			IpcSemaphoreUnlock(WaitCLSemId, 0, IpcExclusiveLock);
			s_lock_sleep(i++);
			IpcSemaphoreLock(WaitCLSemId, 0, IpcExclusiveLock);
#endif
		}
		(buf->r_locks)++;
		*buflock |= BL_R_LOCK;
	}
	else if (mode == BUFFER_LOCK_EXCLUSIVE)
	{
		unsigned	i = 0;

		Assert(!(*buflock & (BL_R_LOCK | BL_W_LOCK | BL_RI_LOCK)));
		while (buf->r_locks > 0 || buf->w_lock)
		{
			if (buf->r_locks > 3 || (*buflock & BL_RI_LOCK))
			{

				/*
				 * Our RI lock might be removed by concurrent W lock
				 * acquiring (see what we do with RI locks below when our
				 * own W acquiring succeeded) and so we set RI lock again
				 * if we already did this.
				 */
				*buflock |= BL_RI_LOCK;
				buf->ri_lock = true;
			}
#ifdef HAS_TEST_AND_SET
			S_UNLOCK(&(buf->cntx_lock));
			s_lock_sleep(i++);
			S_LOCK(&(buf->cntx_lock));
#else
			IpcSemaphoreUnlock(WaitCLSemId, 0, IpcExclusiveLock);
			s_lock_sleep(i++);
			IpcSemaphoreLock(WaitCLSemId, 0, IpcExclusiveLock);
#endif
		}
		buf->w_lock = true;
		*buflock |= BL_W_LOCK;
		if (*buflock & BL_RI_LOCK)
		{

			/*
			 * It's possible to remove RI locks acquired by another W
			 * lockers here, but they'll take care about it.
			 */
			buf->ri_lock = false;
			*buflock &= ~BL_RI_LOCK;
		}
	}
	else
		elog(ERROR, "LockBuffer: unknown lock mode %d", mode);

#ifdef HAS_TEST_AND_SET
	S_UNLOCK(&(buf->cntx_lock));
#else
	IpcSemaphoreUnlock(WaitCLSemId, 0, IpcExclusiveLock);
#endif

}

/*
 *	Functions for IO error handling
 *
 *	Note : We assume that nested buffer IO never occur.
 *	i.e at most one io_in_progress spinlock is held
 *	per proc.
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
*/
static void
StartBufferIO(BufferDesc *buf, bool forInput)
{
	Assert(!InProgressBuf);
	Assert(!(buf->flags & BM_IO_IN_PROGRESS));
	buf->flags |= BM_IO_IN_PROGRESS;
#ifdef	HAS_TEST_AND_SET

	/*
	 * There used to be
	 *
	 * Assert(S_LOCK_FREE(&(buf->io_in_progress_lock)));
	 *
	 * here, but that's wrong because of the way WaitIO works: someone else
	 * waiting for the I/O to complete will succeed in grabbing the lock
	 * for a few instructions, and if we context-swap back to here the
	 * Assert could fail.  Tiny window for failure, but I've seen it
	 * happen -- tgl
	 */
	S_LOCK(&(buf->io_in_progress_lock));
#endif	 /* HAS_TEST_AND_SET */
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
*/
static void
TerminateBufferIO(BufferDesc *buf)
{
	Assert(buf == InProgressBuf);
#ifdef	HAS_TEST_AND_SET
	S_UNLOCK(&(buf->io_in_progress_lock));
#else
	if (buf->refcount > 1)
		SignalIO(buf);
#endif	 /* HAS_TEST_AND_SET */
	InProgressBuf = (BufferDesc *) 0;
}

/*
 * Function:ContinueBufferIO
 *	(Assumptions)
 *	My process is executing IO for the buffer
 *	BufMgrLock is held
 *	The buffer is Pinned
 *
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
	InProgressBuf = (BufferDesc *) 0;
}
#endif

/*
 *	This function is called from ProcReleaseSpins().
 *	BufMgrLock isn't held when this function is called.
 *	BM_IO_ERROR is always set. If BM_IO_ERROR was already
 *	set in case of output,this routine would kill all
 *	backends and reset postmaster.
 */
void
AbortBufferIO(void)
{
	BufferDesc *buf = InProgressBuf;

	if (buf)
	{
		Assert(buf->flags & BM_IO_IN_PROGRESS);
		SpinAcquire(BufMgrLock);
		if (IsForInput)
			Assert(!(buf->flags & BM_DIRTY));
		else
		{
			Assert((buf->flags & BM_DIRTY) != 0);
			if ((buf->flags & BM_IO_ERROR) != 0)
			{
				elog(NOTICE, "write error may be permanent: cannot write block %u for %s/%s",
				buf->tag.blockNum, buf->blind.dbname, buf->blind.relname);
			}
			buf->flags |= BM_DIRTY;
		}
		buf->flags |= BM_IO_ERROR;
		buf->flags &= ~BM_IO_IN_PROGRESS;
		TerminateBufferIO(buf);
		SpinRelease(BufMgrLock);
	}
}

/*
 * Cleanup buffer or mark it for cleanup. Buffer may be cleaned
 * up if it's pinned only once.
 *
 * NOTE: buffer must be excl locked.
 */
void
MarkBufferForCleanup(Buffer buffer, void (*CleanupFunc)(Buffer))
{
	BufferDesc *bufHdr = &BufferDescriptors[buffer - 1];

	Assert(PrivateRefCount[buffer - 1] > 0);

	if (PrivateRefCount[buffer - 1] > 1)
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		PrivateRefCount[buffer - 1]--;
		SpinAcquire(BufMgrLock);
		Assert(bufHdr->refcount > 0);
		bufHdr->flags |= (BM_DIRTY | BM_JUST_DIRTIED);
		bufHdr->CleanupFunc = CleanupFunc;
		SpinRelease(BufMgrLock);
		return;
	}

	SpinAcquire(BufMgrLock);
	Assert(bufHdr->refcount > 0);
	if (bufHdr->refcount == 1)
	{
		SpinRelease(BufMgrLock);
		CleanupFunc(buffer);
		CleanupFunc = NULL;
	}
	else
		SpinRelease(BufMgrLock);

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	PrivateRefCount[buffer - 1]--;

	SpinAcquire(BufMgrLock);
	Assert(bufHdr->refcount > 0);
	bufHdr->flags |= (BM_DIRTY | BM_JUST_DIRTIED);
	bufHdr->CleanupFunc = CleanupFunc;
	bufHdr->refcount--;
	if (bufHdr->refcount == 0)
	{
		AddBufferToFreelist(bufHdr);
		bufHdr->flags |= BM_FREE;
	}
	SpinRelease(BufMgrLock);
	return;
}
