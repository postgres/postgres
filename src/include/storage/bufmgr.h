/*-------------------------------------------------------------------------
 *
 * bufmgr.h--
 *    POSTGRES buffer manager definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: bufmgr.h,v 1.4 1996/10/26 04:15:05 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	BUFMGR_H
#define BUFMGR_H

#include <stdio.h>

#include "storage/ipc.h"
#include "storage/buf.h"
#include "utils/rel.h"

/*
 * the maximum size of a disk block for any possible installation.
 *
 * in theory this could be anything, but in practice this is actually
 * limited to 2^13 bytes because we have limited ItemIdData.lp_off and
 * ItemIdData.lp_len to 13 bits (see itemid.h).
 */
#define	MAXBLCKSZ	8192

typedef void *Block;


/* special pageno for bget */
#define P_NEW	InvalidBlockNumber	/* grow the file to get a new page */

typedef bits16	BufferLock;

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
 *	True iff the buffer is pinned (and therefore valid)
 *
 * Note:
 *	Smenatics are identical to BufferIsValid 
 *      XXX - need to remove either one eventually.
 */
#define BufferIsPinned BufferIsValid


extern int ShowPinTrace;

/*
 * prototypes for functions in bufmgr.c 
 */
extern Buffer RelationGetBufferWithBuffer(Relation relation,
		  BlockNumber blockNumber, Buffer buffer);
extern Buffer ReadBuffer(Relation reln, BlockNumber blockNum);
extern Buffer ReadBuffer_Debug(char *file, int line, Relation reln,
			       BlockNumber blockNum);
extern int WriteBuffer(Buffer buffer);
extern void WriteBuffer_Debug(char *file, int line, Buffer buffer);
extern void DirtyBufferCopy(Oid dbid, Oid relid, BlockNumber blkno,
			    char *dest);
extern int WriteNoReleaseBuffer(Buffer buffer);
extern Buffer ReleaseAndReadBuffer(Buffer buffer, Relation relation,
				   BlockNumber blockNum);

extern void InitBufferPool(IPCKey key);
extern void PrintBufferUsage(FILE *statfp);
extern void ResetBufferUsage(void);
extern void ResetBufferPool(void);
extern int BufferPoolCheckLeak(void);
extern void FlushBufferPool(int StableMainMemoryFlag);
extern bool BufferIsValid(Buffer bufnum);
extern BlockNumber BufferGetBlockNumber(Buffer buffer);
extern Relation BufferGetRelation(Buffer buffer);
extern BlockNumber RelationGetNumberOfBlocks(Relation relation);
extern Block BufferGetBlock(Buffer buffer);
extern void ReleaseTmpRelBuffers(Relation tempreldesc);
extern void DropBuffers(Oid dbid);
extern void PrintBufferDescs(void);
extern void PrintPinnedBufs(void);
extern int BufferShmemSize(void);
extern void BufferPoolBlowaway(void);
extern void IncrBufferRefCount(Buffer buffer);
extern int ReleaseBuffer(Buffer buffer);

extern void IncrBufferRefCount_Debug(char *file, int line, Buffer buffer);
extern void ReleaseBuffer_Debug(char *file, int line, Buffer buffer);
extern int ReleaseAndReadBuffer_Debug(char *file,
				int line,
				Buffer buffer,
				Relation relation,
				BlockNumber blockNum);
extern void BufferRefCountReset(int *refcountsave);
extern void BufferRefCountRestore(int *refcountsave);

#endif	/* !defined(BufMgrIncluded) */

