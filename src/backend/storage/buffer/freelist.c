/*-------------------------------------------------------------------------
 *
 * freelist.c--
 *	  routines for manipulating the buffer pool's replacement strategy
 *	  freelist.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/buffer/freelist.c,v 1.11 1998/06/15 19:29:10 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * OLD COMMENTS
 *
 * Data Structures:
 *		SharedFreeList is a circular queue.  Notice that this
 *		is a shared memory queue so the next/prev "ptrs" are
 *		buffer ids, not addresses.
 *
 * Sync: all routines in this file assume that the buffer
 *		semaphore has been acquired by the caller.
 */
#include <stdio.h>

#include "postgres.h"

#include "storage/bufmgr.h"
#include "storage/buf_internals.h"		/* where declarations go */
#include "storage/spin.h"


static BufferDesc *SharedFreeList;

/* only actually used in debugging.  The lock
 * should be acquired before calling the freelist manager.
 */
extern SPINLOCK BufMgrLock;

#define IsInQueue(bf) \
( \
	AssertMacro((bf->freeNext != INVALID_DESCRIPTOR)), \
	AssertMacro((bf->freePrev != INVALID_DESCRIPTOR)), \
	AssertMacro((bf->flags & BM_FREE)) \
)

#define NotInQueue(bf) \
( \
	AssertMacro((bf->freeNext == INVALID_DESCRIPTOR)), \
	AssertMacro((bf->freePrev == INVALID_DESCRIPTOR)), \
	AssertMacro(! (bf->flags & BM_FREE)) \
)


/*
 * AddBufferToFreelist --
 *
 * In theory, this is the only routine that needs to be changed
 * if the buffer replacement strategy changes.	Just change
 * the manner in which buffers are added to the freelist queue.
 * Currently, they are added on an LRU basis.
 */
void
AddBufferToFreelist(BufferDesc *bf)
{
#ifdef BMTRACE
	_bm_trace(bf->tag.relId.dbId, bf->tag.relId.relId, bf->tag.blockNum,
			  BufferDescriptorGetBuffer(bf), BMT_DEALLOC);
#endif							/* BMTRACE */
	NotInQueue(bf);

	/* change bf so it points to inFrontOfNew and its successor */
	bf->freePrev = SharedFreeList->freePrev;
	bf->freeNext = Free_List_Descriptor;

	/* insert new into chain */
	BufferDescriptors[bf->freeNext].freePrev = bf->buf_id;
	BufferDescriptors[bf->freePrev].freeNext = bf->buf_id;
}

#undef PinBuffer

/*
 * PinBuffer -- make buffer unavailable for replacement.
 */
void
PinBuffer(BufferDesc *buf)
{
	long		b;

	/* Assert (buf->refcount < 25); */

	if (buf->refcount == 0)
	{
		IsInQueue(buf);

		/* remove from freelist queue */
		BufferDescriptors[buf->freeNext].freePrev = buf->freePrev;
		BufferDescriptors[buf->freePrev].freeNext = buf->freeNext;
		buf->freeNext = buf->freePrev = INVALID_DESCRIPTOR;

		/* mark buffer as no longer free */
		buf->flags &= ~BM_FREE;
	}
	else
		NotInQueue(buf);

	b = BufferDescriptorGetBuffer(buf) - 1;
	Assert(PrivateRefCount[b] >= 0);
	if (PrivateRefCount[b] == 0 && LastRefCount[b] == 0)
		buf->refcount++;
	PrivateRefCount[b]++;
}

#ifdef NOT_USED
void
PinBuffer_Debug(char *file, int line, BufferDesc *buf)
{
	PinBuffer(buf);
	if (ShowPinTrace)
	{
		Buffer		buffer = BufferDescriptorGetBuffer(buf);

		fprintf(stderr, "PIN(Pin) %ld relname = %s, blockNum = %d, \
refcount = %ld, file: %s, line: %d\n",
				buffer, buf->sb_relname, buf->tag.blockNum,
				PrivateRefCount[buffer - 1], file, line);
	}
}

#endif

#undef UnpinBuffer

/*
 * UnpinBuffer -- make buffer available for replacement.
 */
void
UnpinBuffer(BufferDesc *buf)
{
	long		b = BufferDescriptorGetBuffer(buf) - 1;

	Assert(buf->refcount);
	Assert(PrivateRefCount[b] > 0);
	PrivateRefCount[b]--;
	if (PrivateRefCount[b] == 0 && LastRefCount[b] == 0)
		buf->refcount--;
	NotInQueue(buf);

	if (buf->refcount == 0)
	{
		AddBufferToFreelist(buf);
		buf->flags |= BM_FREE;
	}
	else
	{
		/* do nothing */
	}
}

#ifdef NOT_USED
void
UnpinBuffer_Debug(char *file, int line, BufferDesc *buf)
{
	UnpinBuffer(buf);
	if (ShowPinTrace)
	{
		Buffer		buffer = BufferDescriptorGetBuffer(buf);

		fprintf(stderr, "UNPIN(Unpin) %ld relname = %s, blockNum = %d, \
refcount = %ld, file: %s, line: %d\n",
				buffer, buf->sb_relname, buf->tag.blockNum,
				PrivateRefCount[buffer - 1], file, line);
	}
}

#endif

/*
 * GetFreeBuffer() -- get the 'next' buffer from the freelist.
 *
 */
BufferDesc *
GetFreeBuffer()
{
	BufferDesc *buf;

	if (Free_List_Descriptor == SharedFreeList->freeNext)
	{

		/* queue is empty. All buffers in the buffer pool are pinned. */
		elog(ERROR, "out of free buffers: time to abort !\n");
		return (NULL);
	}
	buf = &(BufferDescriptors[SharedFreeList->freeNext]);

	/* remove from freelist queue */
	BufferDescriptors[buf->freeNext].freePrev = buf->freePrev;
	BufferDescriptors[buf->freePrev].freeNext = buf->freeNext;
	buf->freeNext = buf->freePrev = INVALID_DESCRIPTOR;

	buf->flags &= ~(BM_FREE);

	return (buf);
}

/*
 * InitFreeList -- initialize the dummy buffer descriptor used
 *		as a freelist head.
 *
 * Assume: All of the buffers are already linked in a circular
 *		queue.	 Only called by postmaster and only during
 *		initialization.
 */
void
InitFreeList(bool init)
{
	SharedFreeList = &(BufferDescriptors[Free_List_Descriptor]);

	if (init)
	{
		/* we only do this once, normally the postmaster */
		SharedFreeList->data = INVALID_OFFSET;
		SharedFreeList->flags = 0;
		SharedFreeList->flags &= ~(BM_VALID | BM_DELETED | BM_FREE);
		SharedFreeList->buf_id = Free_List_Descriptor;

		/* insert it into a random spot in the circular queue */
		SharedFreeList->freeNext = BufferDescriptors[0].freeNext;
		SharedFreeList->freePrev = 0;
		BufferDescriptors[SharedFreeList->freeNext].freePrev =
			BufferDescriptors[SharedFreeList->freePrev].freeNext =
			Free_List_Descriptor;
	}
}


/*
 * print out the free list and check for breaks.
 */
#ifdef NOT_USED
void
DBG_FreeListCheck(int nfree)
{
	int			i;
	BufferDesc *buf;

	buf = &(BufferDescriptors[SharedFreeList->freeNext]);
	for (i = 0; i < nfree; i++, buf = &(BufferDescriptors[buf->freeNext]))
	{

		if (!(buf->flags & (BM_FREE)))
		{
			if (buf != SharedFreeList)
			{
				printf("\tfree list corrupted: %d flags %x\n",
					   buf->buf_id, buf->flags);
			}
			else
			{
				printf("\tfree list corrupted: too short -- %d not %d\n",
					   i, nfree);

			}


		}
		if ((BufferDescriptors[buf->freeNext].freePrev != buf->buf_id) ||
			(BufferDescriptors[buf->freePrev].freeNext != buf->buf_id))
		{
			printf("\tfree list links corrupted: %d %ld %ld\n",
				   buf->buf_id, buf->freePrev, buf->freeNext);
		}

	}
	if (buf != SharedFreeList)
	{
		printf("\tfree list corrupted: %d-th buffer is %d\n",
			   nfree, buf->buf_id);

	}
}

#endif

#ifdef NOT_USED
/*
 * PrintBufferFreeList -
 *	  prints the buffer free list, for debugging
 */
static void
PrintBufferFreeList()
{
	BufferDesc *buf;

	if (SharedFreeList->freeNext == Free_List_Descriptor)
	{
		printf("free list is empty.\n");
		return;
	}

	buf = &(BufferDescriptors[SharedFreeList->freeNext]);
	for (;;)
	{
		int			i = (buf - BufferDescriptors);

		printf("[%-2d] (%s, %d) flags=0x%x, refcnt=%d %ld, nxt=%ld prv=%ld)\n",
			   i, buf->sb_relname, buf->tag.blockNum,
			   buf->flags, buf->refcount, PrivateRefCount[i],
			   buf->freeNext, buf->freePrev);

		if (buf->freeNext == Free_List_Descriptor)
			break;

		buf = &(BufferDescriptors[buf->freeNext]);
	}
}

#endif
