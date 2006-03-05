/*-------------------------------------------------------------------------
 *
 * buf_init.c
 *	  buffer manager initialization routines
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/buffer/buf_init.c,v 1.78 2006/03/05 15:58:36 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/bufmgr.h"
#include "storage/buf_internals.h"


BufferDesc *BufferDescriptors;
char	   *BufferBlocks;
int32	   *PrivateRefCount;

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
 * IO_IN_PROGRESS -- this is a flag in the buffer descriptor.
 *		It must be set when an IO is initiated and cleared at
 *		the end of the IO.	It is there to make sure that one
 *		process doesn't start to use a buffer while another is
 *		faulting it in.  see WaitIO and related routines.
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
	bool		foundBufs,
				foundDescs;

	BufferDescriptors = (BufferDesc *)
		ShmemInitStruct("Buffer Descriptors",
						NBuffers * sizeof(BufferDesc), &foundDescs);

	BufferBlocks = (char *)
		ShmemInitStruct("Buffer Blocks",
						NBuffers * (Size) BLCKSZ, &foundBufs);

	if (foundDescs || foundBufs)
	{
		/* both should be present or neither */
		Assert(foundDescs && foundBufs);
		/* note: this path is only taken in EXEC_BACKEND case */
	}
	else
	{
		BufferDesc *buf;
		int			i;

		buf = BufferDescriptors;

		/*
		 * Initialize all the buffer headers.
		 */
		for (i = 0; i < NBuffers; buf++, i++)
		{
			CLEAR_BUFFERTAG(buf->tag);
			buf->flags = 0;
			buf->usage_count = 0;
			buf->refcount = 0;
			buf->wait_backend_pid = 0;

			SpinLockInit(&buf->buf_hdr_lock);

			buf->buf_id = i;

			/*
			 * Initially link all the buffers together as unused. Subsequent
			 * management of this list is done by freelist.c.
			 */
			buf->freeNext = i + 1;

			buf->io_in_progress_lock = LWLockAssign();
			buf->content_lock = LWLockAssign();
		}

		/* Correct last entry of linked list */
		BufferDescriptors[NBuffers - 1].freeNext = FREENEXT_END_OF_LIST;
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
 * cannot do LWLockAcquire; hence we can't actually access stuff in
 * shared memory yet.  We are only initializing local data here.
 * (See also InitBufferPoolBackend, over in bufmgr.c.)
 */
void
InitBufferPoolAccess(void)
{
	/*
	 * Allocate and zero local arrays of per-buffer info.
	 */
	PrivateRefCount = (int32 *) calloc(NBuffers, sizeof(int32));
	if (!PrivateRefCount)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
}

/*
 * BufferShmemSize
 *
 * compute the size of shared memory for the buffer pool including
 * data pages, buffer descriptors, hash tables, etc.
 */
Size
BufferShmemSize(void)
{
	Size		size = 0;

	/* size of buffer descriptors */
	size = add_size(size, mul_size(NBuffers, sizeof(BufferDesc)));

	/* size of data pages */
	size = add_size(size, mul_size(NBuffers, BLCKSZ));

	/* size of stuff controlled by freelist.c */
	size = add_size(size, StrategyShmemSize());

	return size;
}
