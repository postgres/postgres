/*-------------------------------------------------------------------------
 *
 * buf_init.c
 *	  buffer manager initialization routines
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/buffer/buf_init.c,v 1.54 2003/08/04 02:40:03 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/file.h>
#include <math.h>
#include <signal.h>

#include "catalog/catalog.h"
#include "executor/execdebug.h"
#include "miscadmin.h"
#include "storage/buf.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/shmem.h"
#include "storage/smgr.h"
#include "storage/lwlock.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"


/*
 *	if BMTRACE is defined, we trace the last 200 buffer allocations and
 *	deallocations in a circular buffer in shared memory.
 */
#ifdef	BMTRACE
bmtrace    *TraceBuf;
long	   *CurTraceBuf;

#define BMT_LIMIT		200
#endif   /* BMTRACE */
int			ShowPinTrace = 0;

int			Data_Descriptors;
int			Free_List_Descriptor;
int			Lookup_List_Descriptor;
int			Num_Descriptors;

BufferDesc *BufferDescriptors;
Block	   *BufferBlockPointers;

long	   *PrivateRefCount;	/* also used in freelist.c */
bits8	   *BufferLocks;		/* flag bits showing locks I have set */


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
 * WriteBufferBack:
 *		currently, a buffer is only written back at the time
 *		it is selected for replacement.  It should
 *		be done sooner if possible to reduce latency of
 *		BufferAlloc().	Maybe there should be a daemon process.
 *
 * Synchronization/Locking:
 *
 * BufMgrLock lock -- must be acquired before manipulating the
 *		buffer queues (lookup/freelist).  Must be released
 *		before exit and before doing any IO.
 *
 * IO_IN_PROGRESS -- this is a flag in the buffer descriptor.
 *		It must be set when an IO is initiated and cleared at
 *		the end of	the IO.  It is there to make sure that one
 *		process doesn't start to use a buffer while another is
 *		faulting it in.  see IOWait/IOSignal.
 *
 * refcount --	A buffer is pinned during IO and immediately
 *		after a BufferAlloc().	A buffer is always either pinned
 *		or on the freelist but never both.	The buffer must be
 *		released, written, or flushed before the end of
 *		transaction.
 *
 * PrivateRefCount -- Each buffer also has a private refcount the keeps
 *		track of the number of times the buffer is pinned in the current
 *		processes.	This is used for two purposes, first, if we pin a
 *		a buffer more than once, we only need to change the shared refcount
 *		once, thus only lock the buffer pool once, second, when a transaction
 *		aborts, it should only unpin the buffers exactly the number of times it
 *		has pinned them, so that it will not blow away buffers of another
 *		backend.
 *
 */

long int	ReadBufferCount;
long int	ReadLocalBufferCount;
long int	BufferHitCount;
long int	LocalBufferHitCount;
long int	BufferFlushCount;
long int	LocalBufferFlushCount;


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

	Data_Descriptors = NBuffers;
	Free_List_Descriptor = Data_Descriptors;
	Lookup_List_Descriptor = Data_Descriptors + 1;
	Num_Descriptors = Data_Descriptors + 1;

	/*
	 * It's probably not really necessary to grab the lock --- if there's
	 * anyone else attached to the shmem at this point, we've got
	 * problems.
	 */
	LWLockAcquire(BufMgrLock, LW_EXCLUSIVE);

#ifdef BMTRACE
	CurTraceBuf = (long *) ShmemInitStruct("Buffer trace",
							(BMT_LIMIT * sizeof(bmtrace)) + sizeof(long),
										   &foundDescs);
	if (!foundDescs)
		MemSet(CurTraceBuf, 0, (BMT_LIMIT * sizeof(bmtrace)) + sizeof(long));

	TraceBuf = (bmtrace *) & (CurTraceBuf[1]);
#endif

	BufferDescriptors = (BufferDesc *)
		ShmemInitStruct("Buffer Descriptors",
					  Num_Descriptors * sizeof(BufferDesc), &foundDescs);

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

		buf = BufferDescriptors;
		block = BufferBlocks;

		/*
		 * link the buffers into a circular, doubly-linked list to
		 * initialize free list, and initialize the buffer headers. Still
		 * don't know anything about replacement strategy in this file.
		 */
		for (i = 0; i < Data_Descriptors; block += BLCKSZ, buf++, i++)
		{
			Assert(ShmemIsValid((unsigned long) block));

			buf->freeNext = i + 1;
			buf->freePrev = i - 1;

			CLEAR_BUFFERTAG(&(buf->tag));
			buf->buf_id = i;

			buf->data = MAKE_OFFSET(block);
			buf->flags = (BM_DELETED | BM_FREE | BM_VALID);
			buf->refcount = 0;
			buf->io_in_progress_lock = LWLockAssign();
			buf->cntx_lock = LWLockAssign();
			buf->cntxDirty = false;
			buf->wait_backend_id = 0;
		}

		/* close the circular queue */
		BufferDescriptors[0].freePrev = Data_Descriptors - 1;
		BufferDescriptors[Data_Descriptors - 1].freeNext = 0;
	}

	/* Init other shared buffer-management stuff */
	InitBufTable();
	InitFreeList(!foundDescs);

	LWLockRelease(BufMgrLock);
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
	BufferBlockPointers = (Block *) calloc(NBuffers, sizeof(Block));
	PrivateRefCount = (long *) calloc(NBuffers, sizeof(long));
	BufferLocks = (bits8 *) calloc(NBuffers, sizeof(bits8));

	/*
	 * Convert shmem offsets into addresses as seen by this process. This
	 * is just to speed up the BufferGetBlock() macro.
	 */
	for (i = 0; i < NBuffers; i++)
		BufferBlockPointers[i] = (Block) MAKE_PTR(BufferDescriptors[i].data);
}

/* -----------------------------------------------------
 * BufferShmemSize
 *
 * compute the size of shared memory for the buffer pool including
 * data pages, buffer descriptors, hash tables, etc.
 * ----------------------------------------------------
 */
int
BufferShmemSize(void)
{
	int			size = 0;

	/* size of shmem index hash table */
	size += hash_estimate_size(SHMEM_INDEX_SIZE, sizeof(ShmemIndexEnt));

	/* size of buffer descriptors */
	size += MAXALIGN((NBuffers + 1) * sizeof(BufferDesc));

	/* size of data pages */
	size += NBuffers * MAXALIGN(BLCKSZ);

	/* size of buffer hash table */
	size += hash_estimate_size(NBuffers, sizeof(BufferLookupEnt));

#ifdef BMTRACE
	size += (BMT_LIMIT * sizeof(bmtrace)) + sizeof(long);
#endif

	return size;
}
