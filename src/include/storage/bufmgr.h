/*-------------------------------------------------------------------------
 *
 * bufmgr.h
 *	  POSTGRES buffer manager definitions.
 *
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/bufmgr.h,v 1.86 2004/08/29 05:06:58 momjian Exp $
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
extern int32 *PrivateRefCount;

/* in localbuf.c */
extern DLLIMPORT int NLocBuffer;
extern DLLIMPORT Block *LocalBufferBlockPointers;
extern int32 *LocalRefCount;

/* special block number for ReadBuffer() */
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
extern void ReleaseBuffer(Buffer buffer);
extern void IncrBufferRefCount(Buffer buffer);
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
extern void RelationTruncate(Relation rel, BlockNumber nblocks);
extern void FlushRelationBuffers(Relation rel, BlockNumber firstDelBlock);
extern void DropRelationBuffers(Relation rel);
extern void DropRelFileNodeBuffers(RelFileNode rnode, bool istemp,
					   BlockNumber firstDelBlock);
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
extern int	BufferSync(int percent, int maxpages);

extern void InitLocalBuffer(void);

#endif
