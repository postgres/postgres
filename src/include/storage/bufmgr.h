/*-------------------------------------------------------------------------
 *
 * bufmgr.h--
 *	  POSTGRES buffer manager definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: bufmgr.h,v 1.21 1998/08/19 02:03:57 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUFMGR_H
#define BUFMGR_H

#include <stdio.h>

#include <storage/ipc.h>
#include <storage/block.h>
#include <storage/buf.h>
#include <storage/buf_internals.h>
#include <utils/rel.h>

/*
 * the maximum size of a disk block for any possible installation.
 *
 * in theory this could be anything, but in practice this is actually
 * limited to 2^13 bytes because we have limited ItemIdData.lp_off and
 * ItemIdData.lp_len to 13 bits (see itemid.h).
 *
 * limit is now 2^15.  Took four bits from ItemIdData.lp_flags and gave
 * two apiece to ItemIdData.lp_len and lp_off. darrenk 01/06/98
 *
 */

#define MAXBLCKSZ		32768

typedef void *Block;

/* special pageno for bget */
#define P_NEW	InvalidBlockNumber		/* grow the file to get a new page */

typedef bits16 BufferLock;

/**********************************************************************

  the rest is function defns in the bufmgr that are externally callable

 **********************************************************************/

/*
 * These routines are beaten on quite heavily, hence the macroization.
 * See buf_internals.h for a related comment.
 */
#define BufferDescriptorGetBuffer(bdesc) ((bdesc)->buf_id + 1)

/*
 * BufferIsPinned --
 *		True iff the buffer is pinned (and therefore valid)
 *
 * Note:
 *		Smenatics are identical to BufferIsValid
 *		XXX - need to remove either one eventually.
 */
#define BufferIsPinned BufferIsValid


extern int	ShowPinTrace;

/*
 * BufferWriteModes (settable via SetBufferWriteMode)
 */
#define BUFFER_FLUSH_WRITE		0		/* immediate write */
#define BUFFER_LATE_WRITE		1		/* delayed write: mark as DIRTY */

/*
 * BufferIsValid --
 *		True iff the refcnt of the local buffer is > 0
 * Note:
 *		BufferIsValid(InvalidBuffer) is False.
 *		BufferIsValid(UnknownBuffer) is False.
 */
#define BufferIsValid(bufnum) \
( \
	BufferIsLocal(bufnum) ? \
		((bufnum) >= -NLocBuffer && LocalRefCount[-(bufnum) - 1] > 0) \
	: \
	( \
		BAD_BUFFER_ID(bufnum) ? \
			false \
		: \
			(PrivateRefCount[(bufnum) - 1] > 0) \
	) \
)

#define IncrBufferRefCount(buffer) \
( \
	BufferIsLocal(buffer) ? \
	( \
		(void)AssertMacro(LocalRefCount[-(buffer) - 1] >= 0), \
		(void)LocalRefCount[-(buffer) - 1]++ \
	) \
	: \
	( \
		(void)AssertMacro(!BAD_BUFFER_ID(buffer)), \
		(void)AssertMacro(PrivateRefCount[(buffer) - 1] >= 0), \
		(void)PrivateRefCount[(buffer) - 1]++ \
	) \
)

/*
 * BufferGetBlock --
 *		Returns a reference to a disk page image associated with a buffer.
 *
 * Note:
 *		Assumes buffer is valid.
 */
#define BufferGetBlock(buffer) \
( \
	AssertMacro(BufferIsValid(buffer)), \
	BufferIsLocal(buffer) ? \
		((Block) MAKE_PTR(LocalBufferDescriptors[-(buffer) - 1].data)) \
	: \
		((Block) MAKE_PTR(BufferDescriptors[(buffer) - 1].data)) \
)


/*
 * prototypes for functions in bufmgr.c
 */
extern Buffer
RelationGetBufferWithBuffer(Relation relation,
							BlockNumber blockNumber, Buffer buffer);
extern Buffer ReadBuffer(Relation reln, BlockNumber blockNum);
extern int	WriteBuffer(Buffer buffer);
extern int	WriteNoReleaseBuffer(Buffer buffer);
extern Buffer
ReleaseAndReadBuffer(Buffer buffer, Relation relation,
					 BlockNumber blockNum);

extern void InitBufferPool(IPCKey key);
extern void PrintBufferUsage(FILE *statfp);
extern void ResetBufferUsage(void);
extern void ResetBufferPool(void);
extern int	BufferPoolCheckLeak(void);
extern void FlushBufferPool(int StableMainMemoryFlag);
extern BlockNumber BufferGetBlockNumber(Buffer buffer);
extern Relation BufferGetRelation(Buffer buffer);
extern BlockNumber RelationGetNumberOfBlocks(Relation relation);
extern void ReleaseRelationBuffers(Relation rel);
extern void DropBuffers(Oid dbid);
extern void PrintBufferDescs(void);
extern void PrintPinnedBufs(void);
extern int	BufferShmemSize(void);
extern int	ReleaseBuffer(Buffer buffer);

extern void BufferRefCountReset(int *refcountsave);
extern void BufferRefCountRestore(int *refcountsave);
extern int	SetBufferWriteMode(int mode);
extern void SetBufferCommitInfoNeedsSave(Buffer buffer);

#endif							/* !defined(BufMgrIncluded) */
