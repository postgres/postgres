/*-------------------------------------------------------------------------
 *
 * bufmgr.h
 *	  POSTGRES buffer manager definitions.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: bufmgr.h,v 1.70 2003/08/10 19:48:08 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUFMGR_H
#define BUFMGR_H

#include "access/xlogdefs.h"
#include "storage/buf.h"
#include "storage/lock.h"
#include "storage/relfilenode.h"
#include "utils/rel.h"

typedef void *Block;

/* in globals.c ... this duplicates miscadmin.h */
extern DLLIMPORT int NBuffers;

/* in bufmgr.c */
extern bool zero_damaged_pages;

/* in buf_init.c */
extern DLLIMPORT Block *BufferBlockPointers;
extern long *PrivateRefCount;

/* in localbuf.c */
extern DLLIMPORT int NLocBuffer;
extern DLLIMPORT Block *LocalBufferBlockPointers;
extern long *LocalRefCount;

/* special pageno for bget */
#define P_NEW	InvalidBlockNumber		/* grow the file to get a new page */

/*
 * Buffer context lock modes
 */
#define BUFFER_LOCK_UNLOCK		0
#define BUFFER_LOCK_SHARE		1
#define BUFFER_LOCK_EXCLUSIVE	2

/*
 * These routines are beaten on quite heavily, hence the macroization.
 */

#define BAD_BUFFER_ID(bid) ((bid) < 1 || (bid) > NBuffers)
#define INVALID_DESCRIPTOR (-3)

#define UnlockAndReleaseBuffer(buffer)	\
( \
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK), \
	ReleaseBuffer(buffer) \
)

#define UnlockAndWriteBuffer(buffer)	\
( \
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK), \
	WriteBuffer(buffer) \
)

/*
 * BufferIsValid
 *		True iff the given buffer number is valid (either as a shared
 *		or local buffer).
 *
 * Note: For a long time this was defined the same as BufferIsPinned,
 * that is it would say False if you didn't hold a pin on the buffer.
 * I believe this was bogus and served only to mask logic errors.
 * Code should always know whether it has a buffer reference,
 * independently of the pin state.
 */
#define BufferIsValid(bufnum) \
( \
	BufferIsLocal(bufnum) ? \
		((bufnum) >= -NLocBuffer) \
	: \
		(! BAD_BUFFER_ID(bufnum)) \
)

/*
 * BufferIsPinned
 *		True iff the buffer is pinned (also checks for valid buffer number).
 *
 *		NOTE: what we check here is that *this* backend holds a pin on
 *		the buffer.  We do not care whether some other backend does.
 */
#define BufferIsPinned(bufnum) \
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

/*
 * IncrBufferRefCount
 *		Increment the pin count on a buffer that we have *already* pinned
 *		at least once.
 *
 *		This macro cannot be used on a buffer we do not have pinned,
 *		because it doesn't change the shared buffer state.  Therefore the
 *		Assert checks are for refcount > 0.  Someone got this wrong once...
 */
#define IncrBufferRefCount(buffer) \
( \
	BufferIsLocal(buffer) ? \
	( \
		(void) AssertMacro((buffer) >= -NLocBuffer), \
		(void) AssertMacro(LocalRefCount[-(buffer) - 1] > 0), \
		(void) LocalRefCount[-(buffer) - 1]++ \
	) \
	: \
	( \
		(void) AssertMacro(!BAD_BUFFER_ID(buffer)), \
		(void) AssertMacro(PrivateRefCount[(buffer) - 1] > 0), \
		(void) PrivateRefCount[(buffer) - 1]++ \
	) \
)

/*
 * BufferGetBlock
 *		Returns a reference to a disk page image associated with a buffer.
 *
 * Note:
 *		Assumes buffer is valid.
 */
#define BufferGetBlock(buffer) \
( \
	AssertMacro(BufferIsValid(buffer)), \
	BufferIsLocal(buffer) ? \
		LocalBufferBlockPointers[-(buffer) - 1] \
	: \
		BufferBlockPointers[(buffer) - 1] \
)

/*
 * prototypes for functions in bufmgr.c
 */
extern Buffer ReadBuffer(Relation reln, BlockNumber blockNum);
extern int	ReleaseBuffer(Buffer buffer);
extern void WriteBuffer(Buffer buffer);
extern void WriteNoReleaseBuffer(Buffer buffer);
extern Buffer ReleaseAndReadBuffer(Buffer buffer, Relation relation,
					 BlockNumber blockNum);

extern void InitBufferPool(void);
extern void InitBufferPoolAccess(void);
extern char *ShowBufferUsage(void);
extern void ResetBufferUsage(void);
extern void AtEOXact_Buffers(bool isCommit);
extern void FlushBufferPool(void);
extern BlockNumber BufferGetBlockNumber(Buffer buffer);
extern BlockNumber RelationGetNumberOfBlocks(Relation relation);
extern void RelationUpdateNumberOfBlocks(Relation relation);
extern int	FlushRelationBuffers(Relation rel, BlockNumber firstDelBlock);
extern void DropRelationBuffers(Relation rel);
extern void DropRelFileNodeBuffers(RelFileNode rnode, bool istemp);
extern void DropBuffers(Oid dbid);

#ifdef NOT_USED
extern void PrintPinnedBufs(void);
#endif
extern int	BufferShmemSize(void);
extern RelFileNode BufferGetFileNode(Buffer buffer);

extern void SetBufferCommitInfoNeedsSave(Buffer buffer);

extern void UnlockBuffers(void);
extern void LockBuffer(Buffer buffer, int mode);
extern bool ConditionalLockBuffer(Buffer buffer);
extern void LockBufferForCleanup(Buffer buffer);

extern void AbortBufferIO(void);

extern void BufmgrCommit(void);
extern void BufferSync(void);

extern void InitLocalBuffer(void);

#endif
