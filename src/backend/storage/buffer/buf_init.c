/*-------------------------------------------------------------------------
 *
 * buf_init.c--
 *	  buffer manager initialization routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/buffer/buf_init.c,v 1.14 1997/09/18 20:21:16 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>
#include <sys/file.h>
#include <stdio.h>
#include <math.h>
#include <signal.h>

#include "postgres.h"

/* declarations split between these three files */
#include "storage/buf.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"

#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/s_lock.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "storage/smgr.h"
#include "storage/lmgr.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/dynahash.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "executor/execdebug.h" /* for NDirectFileRead */
#include "catalog/catalog.h"

/*
 *	if BMTRACE is defined, we trace the last 200 buffer allocations and
 *	deallocations in a circular buffer in shared memory.
 */
#ifdef	BMTRACE
bmtrace    *TraceBuf;
long	   *CurTraceBuf;

#define BMT_LIMIT		200
#endif							/* BMTRACE */
int			ShowPinTrace = 0;

int			NBuffers = NDBUFS;	/* NDBUFS defined in miscadmin.h */
int			Data_Descriptors;
int			Free_List_Descriptor;
int			Lookup_List_Descriptor;
int			Num_Descriptors;

BufferDesc *BufferDescriptors;
BufferBlock BufferBlocks;

#ifndef HAS_TEST_AND_SET
long	   *NWaitIOBackendP;

#endif

extern IpcSemaphoreId WaitIOSemId;

long	   *PrivateRefCount;	/* also used in freelist.c */
long	   *LastRefCount;		/* refcounts of last ExecMain level */
long	   *CommitInfoNeedsSave;/* to write buffers where we have filled
								 * in */

 /* t_tmin (or t_tmax)						 */

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
InitBufferPool(IPCKey key)
{
	bool		foundBufs,
				foundDescs;
	int			i;

	/* check padding of BufferDesc and BufferHdr */

	/*
	 * we need both checks because a sbufdesc_padded >
	 * PADDED_SBUFDESC_SIZE will shrink sbufdesc to the required size,
	 * which is bad
	 */
	if (sizeof(struct sbufdesc) != PADDED_SBUFDESC_SIZE ||
		sizeof(struct sbufdesc_unpadded) > PADDED_SBUFDESC_SIZE)
		elog(WARN, "Internal error:  sbufdesc does not have the proper size, "
			 "contact the Postgres developers");
	if (sizeof(struct sbufdesc_unpadded) <= PADDED_SBUFDESC_SIZE / 2)
		elog(WARN, "Internal error:  sbufdesc is greatly over-sized, "
			 "contact the Postgres developers");

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

#ifndef HAS_TEST_AND_SET
	{
		bool		foundNWaitIO;

		NWaitIOBackendP = (long *) ShmemInitStruct("#Backends Waiting IO",
												   sizeof(long),
												   &foundNWaitIO);
		if (!foundNWaitIO)
			*NWaitIOBackendP = 0;
	}
#endif

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
#ifdef HAS_TEST_AND_SET
			S_INIT_LOCK(&(buf->io_in_progress_lock));
#endif
		}

		/* close the circular queue */
		BufferDescriptors[0].freePrev = Data_Descriptors - 1;
		BufferDescriptors[Data_Descriptors - 1].freeNext = 0;
	}

	/* Init the rest of the module */
	InitBufTable();
	InitFreeList(!foundDescs);

	SpinRelease(BufMgrLock);

#ifndef HAS_TEST_AND_SET
	{
		int			status;

		WaitIOSemId = IpcSemaphoreCreate(IPCKeyGetWaitIOSemaphoreKey(key),
										 1, IPCProtection, 0, 1, &status);
	}
#endif
	PrivateRefCount = (long *) calloc(NBuffers, sizeof(long));
	LastRefCount = (long *) calloc(NBuffers, sizeof(long));
	CommitInfoNeedsSave = (long *) calloc(NBuffers, sizeof(long));
}

/* -----------------------------------------------------
 * BufferShmemSize
 *
 * compute the size of shared memory for the buffer pool including
 * data pages, buffer descriptors, hash tables, etc.
 * ----------------------------------------------------
 */
int
BufferShmemSize()
{
	int			size = 0;
	int			nbuckets;
	int			nsegs;
	int			tmp;

	nbuckets = 1 << (int) my_log2((NBuffers - 1) / DEF_FFACTOR + 1);
	nsegs = 1 << (int) my_log2((nbuckets - 1) / DEF_SEGSIZE + 1);

	/* size of shmem binding table */
	size += MAXALIGN(my_log2(BTABLE_SIZE) * sizeof(void *));	/* HTAB->dir */
	size += MAXALIGN(sizeof(HHDR));		/* HTAB->hctl */
	size += MAXALIGN(DEF_SEGSIZE * sizeof(SEGMENT));
	size += BUCKET_ALLOC_INCR *
		(MAXALIGN(sizeof(BUCKET_INDEX)) +
		 MAXALIGN(BTABLE_KEYSIZE) +
		 MAXALIGN(BTABLE_DATASIZE));

	/* size of buffer descriptors */
	size += MAXALIGN((NBuffers + 1) * sizeof(BufferDesc));

	/* size of data pages */
	size += NBuffers * MAXALIGN(BLCKSZ);

	/* size of buffer hash table */
	size += MAXALIGN(my_log2(NBuffers) * sizeof(void *));		/* HTAB->dir */
	size += MAXALIGN(sizeof(HHDR));		/* HTAB->hctl */
	size += nsegs * MAXALIGN(DEF_SEGSIZE * sizeof(SEGMENT));
	tmp = (int) ceil((double) NBuffers / BUCKET_ALLOC_INCR);
	size += tmp * BUCKET_ALLOC_INCR *
		(MAXALIGN(sizeof(BUCKET_INDEX)) +
		 MAXALIGN(sizeof(BufferTag)) +
		 MAXALIGN(sizeof(Buffer)));

#ifdef BMTRACE
	size += (BMT_LIMIT * sizeof(bmtrace)) + sizeof(long);
#endif
	return size;
}
