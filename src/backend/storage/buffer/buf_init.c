/*-------------------------------------------------------------------------
 *
 * buf_init.c
 *	  buffer manager initialization routines
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/buffer/buf_init.c,v 1.68 2004/08/29 05:06:47 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/bufmgr.h"
#include "storage/buf_internals.h"


BufferDesc *BufferDescriptors;
Block	   *BufferBlockPointers;

int32	   *PrivateRefCount;	/* also used in freelist.c */
bits8	   *BufferLocks;		/* flag bits showing locks I have set */

/* statistics counters */
long int	ReadBufferCount;
long int	ReadLocalBufferCount;
long int	BufferHitCount;
long int	LocalBufferHitCount;
long int	BufferFlushCount;
long int	LocalBufferFlushCount;


/*
 * Data Structures:
 *		buffers live in a freelist and a lookup data structure.
 *
 *
 * Buffer Lookup:
 *		Two important notes.  First, the buffer has to be
 *		available for lookup BEFORE an IO begins.  Otherwise
 *		a second process trying to read the buffer will
 *		allocate its own copy and the buffer pool will
 *		become inconsistent.
 *
 * Buffer Replacement:
 *		see freelist.c.  A buffer cannot be replaced while in
 *		use either by data manager or during IO.
 *
 *
 * Synchronization/Locking:
 *
 * BufMgrLock lock -- must be acquired before manipulating the
 *		buffer search datastructures (lookup/freelist, as well as the
 *		flag bits of any buffer).  Must be released
 *		before exit and before doing any IO.
 *
 * IO_IN_PROGRESS -- this is a flag in the buffer descriptor.
 *		It must be set when an IO is initiated and cleared at
 *		the end of the IO.	It is there to make sure that one
 *		process doesn't start to use a buffer while another is
 *		faulting it in.  see IOWait/IOSignal.
 *
 * refcount --	Counts the number of processes holding pins on a buffer.
 *		A buffer is pinned during IO and immediately after a BufferAlloc().
 *		Pins must be released before end of transaction.
 *
 * PrivateRefCount -- Each buffer also has a private refcount that keeps
 *		track of the number of times the buffer is pinned in the current
 *		process.	This is used for two purposes: first, if we pin a
 *		a buffer more than once, we only need to change the shared refcount
 *		once, thus only lock the shared state once; second, when a transaction
 *		aborts, it should only unpin the buffers exactly the number of times it
 *		has pinned them, so that it will not blow away buffers of another
 *		backend.
 *
 */


/*
 * Initialize shared buffer pool
 *
 * This is called once during shared-memory initialization (either in the
 * postmaster, or in a standalone backend).
 */
void
InitBufferPool(void)
{
	char	   *BufferBlocks;
	bool		foundBufs,
				foundDescs;
	int			i;

	BufferDescriptors = (BufferDesc *)
		ShmemInitStruct("Buffer Descriptors",
						NBuffers * sizeof(BufferDesc), &foundDescs);

	BufferBlocks = (char *)
		ShmemInitStruct("Buffer Blocks",
						NBuffers * BLCKSZ, &foundBufs);

	if (foundDescs || foundBufs)
	{
		/* both should be present or neither */
		Assert(foundDescs && foundBufs);
	}
	else
	{
		BufferDesc *buf;
		char	   *block;

		/*
		 * It's probably not really necessary to grab the lock --- if
		 * there's anyone else attached to the shmem at this point, we've
		 * got problems.
		 */
		LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);

		buf = BufferDescriptors;
		block = BufferBlocks;

		/*
		 * link the buffers into a single linked list. This will become
		 * the LIFO list of unused buffers returned by
		 * StrategyGetBuffer().
		 */
		for (i = 0; i < NBuffers; block += BLCKSZ, buf++, i++)
		{
			Assert(ShmemIsValid((unsigned long) block));

			buf->bufNext = i + 1;

			CLEAR_BUFFERTAG(buf->tag);
			buf->buf_id = i;

			buf->data = MAKE_OFFSET(block);
			buf->flags = 0;
			buf->refcount = 0;
			buf->io_in_progress_lock = LWLockAssign();
			buf->cntx_lock = LWLockAssign();
			buf->cntxDirty = false;
			buf->wait_backend_id = 0;
		}

		/* Correct last entry */
		BufferDescriptors[NBuffers - 1].bufNext = -1;

		LWLockRelease(BufMgrLock);
	}

	/* Init other shared buffer-management stuff */
	StrategyInitialize(!foundDescs);
}

/*
 * Initialize access to shared buffer pool
 *
 * This is called during backend startup (whether standalone or under the
 * postmaster).  It sets up for this backend's access to the already-existing
 * buffer pool.
 *
 * NB: this is called before InitProcess(), so we do not have a PGPROC and
 * cannot do LWLockAcquire; hence we can't actually access the bufmgr's
 * shared memory yet.  We are only initializing local data here.
 */
void
InitBufferPoolAccess(void)
{
	int			i;

	/*
	 * Allocate and zero local arrays of per-buffer info.
	 */
	BufferBlockPointers = (Block *) calloc(NBuffers,
										   sizeof(*BufferBlockPointers));
	PrivateRefCount = (int32 *) calloc(NBuffers,
									   sizeof(*PrivateRefCount));
	BufferLocks = (bits8 *) calloc(NBuffers, sizeof(*BufferLocks));

	/*
	 * Convert shmem offsets into addresses as seen by this process. This
	 * is just to speed up the BufferGetBlock() macro.
	 */
	for (i = 0; i < NBuffers; i++)
		BufferBlockPointers[i] = (Block) MAKE_PTR(BufferDescriptors[i].data);
}

/*
 * BufferShmemSize
 *
 * compute the size of shared memory for the buffer pool including
 * data pages, buffer descriptors, hash tables, etc.
 */
int
BufferShmemSize(void)
{
	int			size = 0;

	/* size of buffer descriptors */
	size += MAXALIGN(NBuffers * sizeof(BufferDesc));

	/* size of data pages */
	size += NBuffers * MAXALIGN(BLCKSZ);

	/* size of buffer hash table */
	size += hash_estimate_size(NBuffers * 2, sizeof(BufferLookupEnt));

	/* size of the shared replacement strategy control block */
	size += MAXALIGN(sizeof(BufferStrategyControl));

	/* size of the ARC directory blocks */
	size += MAXALIGN(NBuffers * 2 * sizeof(BufferStrategyCDB));

	return size;
}
