/*-------------------------------------------------------------------------
 *
 * buf_init.c
 *	  buffer manager initialization routines
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/buffer/buf_init.c,v 1.38 2000/11/28 23:27:55 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>
#include <sys/file.h>
#include <math.h>
#include <signal.h>

#include "postgres.h"

#include "catalog/catalog.h"
#include "executor/execdebug.h"
#include "miscadmin.h"
#include "storage/buf.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/s_lock.h"
#include "storage/shmem.h"
#include "storage/smgr.h"
#include "storage/spin.h"
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
#endif	 /* BMTRACE */
int			ShowPinTrace = 0;

int			Data_Descriptors;
int			Free_List_Descriptor;
int			Lookup_List_Descriptor;
int			Num_Descriptors;

BufferDesc *BufferDescriptors;
BufferBlock BufferBlocks;

long	   *PrivateRefCount;	/* also used in freelist.c */
bits8	   *BufferLocks;		/* flag bits showing locks I have set */
BufferTag  *BufferTagLastDirtied;		/* tag buffer had when last
										 * dirtied by me */
BufferBlindId  *BufferBlindLastDirtied;
bool		   *BufferDirtiedByMe;	/* T if buf has been dirtied in cur xact */


/*
 * Data Structures:
 *		buffers live in a freelist and a lookup data structure.
 *
 *
 * Buffer Lookup:
 *		Two important notes.  First, the buffer has to be
 *		available for lookup BEFORE an IO begins.  Otherwise
 *		a second process trying to read the buffer will
 *		allocate its own copy and the buffeer pool will
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

SPINLOCK	BufMgrLock;

long int	ReadBufferCount;
long int	ReadLocalBufferCount;
long int	BufferHitCount;
long int	LocalBufferHitCount;
long int	BufferFlushCount;
long int	LocalBufferFlushCount;


/*
 * Initialize module:
 *
 * should calculate size of pool dynamically based on the
 * amount of available memory.
 */
void
InitBufferPool(void)
{
	bool		foundBufs,
				foundDescs;
	int			i;

	Data_Descriptors = NBuffers;
	Free_List_Descriptor = Data_Descriptors;
	Lookup_List_Descriptor = Data_Descriptors + 1;
	Num_Descriptors = Data_Descriptors + 1;

	SpinAcquire(BufMgrLock);

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

	BufferBlocks = (BufferBlock)
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
		unsigned long block;

		buf = BufferDescriptors;
		block = (unsigned long) BufferBlocks;

		/*
		 * link the buffers into a circular, doubly-linked list to
		 * initialize free list.  Still don't know anything about
		 * replacement strategy in this file.
		 */
		for (i = 0; i < Data_Descriptors; block += BLCKSZ, buf++, i++)
		{
			Assert(ShmemIsValid((unsigned long) block));

			buf->freeNext = i + 1;
			buf->freePrev = i - 1;

			CLEAR_BUFFERTAG(&(buf->tag));
			buf->data = MAKE_OFFSET(block);
			buf->flags = (BM_DELETED | BM_FREE | BM_VALID);
			buf->refcount = 0;
			buf->buf_id = i;
			S_INIT_LOCK(&(buf->io_in_progress_lock));
			S_INIT_LOCK(&(buf->cntx_lock));
		}

		/* close the circular queue */
		BufferDescriptors[0].freePrev = Data_Descriptors - 1;
		BufferDescriptors[Data_Descriptors - 1].freeNext = 0;
	}

	/* Init the rest of the module */
	InitBufTable();
	InitFreeList(!foundDescs);

	SpinRelease(BufMgrLock);

	PrivateRefCount = (long *) calloc(NBuffers, sizeof(long));
	BufferLocks = (bits8 *) calloc(NBuffers, sizeof(bits8));
	BufferTagLastDirtied = (BufferTag *) calloc(NBuffers, sizeof(BufferTag));
	BufferBlindLastDirtied = (BufferBlindId *) calloc(NBuffers, sizeof(BufferBlindId));
	BufferDirtiedByMe = (bool *) calloc(NBuffers, sizeof(bool));
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
	size += hash_estimate_size(SHMEM_INDEX_SIZE,
							   SHMEM_INDEX_KEYSIZE,
							   SHMEM_INDEX_DATASIZE);

	/* size of buffer descriptors */
	size += MAXALIGN((NBuffers + 1) * sizeof(BufferDesc));

	/* size of data pages */
	size += NBuffers * MAXALIGN(BLCKSZ);

	/* size of buffer hash table */
	size += hash_estimate_size(NBuffers,
							   sizeof(BufferTag),
							   sizeof(Buffer));

#ifdef BMTRACE
	size += (BMT_LIMIT * sizeof(bmtrace)) + sizeof(long);
#endif

	return size;
}
