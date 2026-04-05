/*-------------------------------------------------------------------------
 *
 * buf_init.c
 *	  buffer manager initialization routines
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/buf_init.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/aio.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/proclist.h"
#include "storage/shmem.h"
#include "storage/subsystems.h"

BufferDescPadded *BufferDescriptors;
char	   *BufferBlocks;
ConditionVariableMinimallyPadded *BufferIOCVArray;
WritebackContext BackendWritebackContext;
CkptSortItem *CkptBufferIds;

static void BufferManagerShmemRequest(void *arg);
static void BufferManagerShmemInit(void *arg);
static void BufferManagerShmemAttach(void *arg);

const ShmemCallbacks BufferManagerShmemCallbacks = {
	.request_fn = BufferManagerShmemRequest,
	.init_fn = BufferManagerShmemInit,
	.attach_fn = BufferManagerShmemAttach,
};

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
 * IO_IN_PROGRESS -- this is a flag in the buffer descriptor.
 *		It must be set when an IO is initiated and cleared at
 *		the end of the IO.  It is there to make sure that one
 *		process doesn't start to use a buffer while another is
 *		faulting it in.  see WaitIO and related routines.
 *
 * refcount --	Counts the number of processes holding pins on a buffer.
 *		A buffer is pinned during IO and immediately after a BufferAlloc().
 *		Pins must be released before end of transaction.  For efficiency the
 *		shared refcount isn't increased if an individual backend pins a buffer
 *		multiple times. Check the PrivateRefCount infrastructure in bufmgr.c.
 */


/*
 * Register shared memory area for the buffer pool.
 */
static void
BufferManagerShmemRequest(void *arg)
{
	ShmemRequestStruct(.name = "Buffer Descriptors",
					   .size = NBuffers * sizeof(BufferDescPadded),
	/* Align descriptors to a cacheline boundary. */
					   .alignment = PG_CACHE_LINE_SIZE,
					   .ptr = (void **) &BufferDescriptors,
		);

	ShmemRequestStruct(.name = "Buffer Blocks",
					   .size = NBuffers * (Size) BLCKSZ,
	/* Align buffer pool on IO page size boundary. */
					   .alignment = PG_IO_ALIGN_SIZE,
					   .ptr = (void **) &BufferBlocks,
		);

	ShmemRequestStruct(.name = "Buffer IO Condition Variables",
					   .size = NBuffers * sizeof(ConditionVariableMinimallyPadded),
	/* Align descriptors to a cacheline boundary. */
					   .alignment = PG_CACHE_LINE_SIZE,
					   .ptr = (void **) &BufferIOCVArray,
		);

	/*
	 * The array used to sort to-be-checkpointed buffer ids is located in
	 * shared memory, to avoid having to allocate significant amounts of
	 * memory at runtime. As that'd be in the middle of a checkpoint, or when
	 * the checkpointer is restarted, memory allocation failures would be
	 * painful.
	 */
	ShmemRequestStruct(.name = "Checkpoint BufferIds",
					   .size = NBuffers * sizeof(CkptSortItem),
					   .ptr = (void **) &CkptBufferIds,
		);
}

/*
 * Initialize shared buffer pool
 *
 * This is called once during shared-memory initialization (either in the
 * postmaster, or in a standalone backend).
 */
static void
BufferManagerShmemInit(void *arg)
{
	/*
	 * Initialize all the buffer headers.
	 */
	for (int i = 0; i < NBuffers; i++)
	{
		BufferDesc *buf = GetBufferDescriptor(i);

		ClearBufferTag(&buf->tag);

		pg_atomic_init_u64(&buf->state, 0);
		buf->wait_backend_pgprocno = INVALID_PROC_NUMBER;

		buf->buf_id = i;

		pgaio_wref_clear(&buf->io_wref);

		proclist_init(&buf->lock_waiters);
		ConditionVariableInit(BufferDescriptorGetIOCV(buf));
	}

	/* Initialize per-backend file flush context */
	WritebackContextInit(&BackendWritebackContext,
						 &backend_flush_after);
}

static void
BufferManagerShmemAttach(void *arg)
{
	/* Initialize per-backend file flush context */
	WritebackContextInit(&BackendWritebackContext,
						 &backend_flush_after);
}
