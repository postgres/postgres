/*-------------------------------------------------------------------------
 *
 * localbuf.c--
 *    local buffer manager. Fast buffer manager for temporary tables
 *    or special cases when the operation is not visible to other backends.
 *
 *    When a relation is being created, the descriptor will have rd_islocal
 *    set to indicate that the local buffer manager should be used. During
 *    the same transaction the relation is being created, any inserts or
 *    selects from the newly created relation will use the local buffer
 *    pool. rd_islocal is reset at the end of a transaction (commit/abort).
 *    This is useful for queries like SELECT INTO TABLE and create index.
 *
 * Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/storage/buffer/localbuf.c,v 1.1.1.1 1996/07/09 06:21:54 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/file.h>
#include <stdio.h>
#include <math.h>
#include <signal.h>

/* declarations split between these three files */
#include "storage/buf.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"

#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "storage/smgr.h"
#include "storage/lmgr.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/elog.h"
#include "utils/memutils.h"
#include "executor/execdebug.h"	/* for NDirectFileRead */
#include "catalog/catalog.h"

int NLocBuffer = 64;
BufferDesc *LocalBufferDescriptors = NULL;
long *LocalRefCount = NULL;

static int nextFreeLocalBuf = 0;

/*#define LBDEBUG*/

/*
 * LocalBufferAlloc -
 *    allocate a local buffer. We do round robin allocation for now.
 */
BufferDesc *
LocalBufferAlloc(Relation reln, BlockNumber blockNum, bool *foundPtr)
{
    int i;
    BufferDesc *bufHdr = (BufferDesc *) NULL;

    if (blockNum == P_NEW) {
	blockNum = reln->rd_nblocks;
	reln->rd_nblocks++;
    } 

    /* a low tech search for now -- not optimized for scans */
    for (i=0; i < NLocBuffer; i++) {
	if (LocalBufferDescriptors[i].tag.relId.relId == reln->rd_id &&
	    LocalBufferDescriptors[i].tag.blockNum == blockNum) {

#ifdef LBDEBUG
	    fprintf(stderr, "LB ALLOC (%d,%d) %d\n",
		    reln->rd_id, blockNum, -i-1);
#endif    
	    LocalRefCount[i]++;
	    *foundPtr = TRUE;
	    return &LocalBufferDescriptors[i];
	}
    }

#ifdef LBDEBUG
    fprintf(stderr, "LB ALLOC (%d,%d) %d\n",
	    reln->rd_id, blockNum, -nextFreeLocalBuf-1);
#endif    
    
    /* need to get a new buffer (round robin for now) */
    for(i=0; i < NLocBuffer; i++) {
	int b = (nextFreeLocalBuf + i) % NLocBuffer;

	if (LocalRefCount[b]==0) {
	    bufHdr = &LocalBufferDescriptors[b];
	    LocalRefCount[b]++;
	    nextFreeLocalBuf = (b + 1) % NLocBuffer;
	    break;
	}
    }
    if (bufHdr==NULL)
	elog(WARN, "no empty local buffer.");

    /*
     * this buffer is not referenced but it might still be dirty (the
     * last transaction to touch it doesn't need its contents but has
     * not flushed it).  if that's the case, write it out before
     * reusing it!
     */
    if (bufHdr->flags & BM_DIRTY) {
	Relation bufrel = RelationIdCacheGetRelation(bufHdr->tag.relId.relId);

	Assert(bufrel != NULL);
	
	/* flush this page */
	smgrwrite(bufrel->rd_rel->relsmgr, bufrel, bufHdr->tag.blockNum,
		  (char *) MAKE_PTR(bufHdr->data));
    }

    /*
     * it's all ours now.
     */
    bufHdr->tag.relId.relId = reln->rd_id;
    bufHdr->tag.blockNum = blockNum;
    bufHdr->flags &= ~BM_DIRTY;

    /*
     * lazy memory allocation. (see MAKE_PTR for why we need to do 
     * MAKE_OFFSET.)
     */
    if (bufHdr->data == (SHMEM_OFFSET)0) {
	char *data = (char *)malloc(BLCKSZ);

	bufHdr->data = MAKE_OFFSET(data);
    }
    
    *foundPtr = FALSE;
    return bufHdr;
}

/*
 * WriteLocalBuffer -
 *    writes out a local buffer
 */
int
WriteLocalBuffer(Buffer buffer, bool release)
{
    int bufid;

    Assert(BufferIsLocal(buffer));

#ifdef LBDEBUG
    fprintf(stderr, "LB WRITE %d\n", buffer);
#endif    
    
    bufid = - (buffer + 1);
    LocalBufferDescriptors[bufid].flags |= BM_DIRTY;

    if (release) {
	Assert(LocalRefCount[bufid] > 0);
	LocalRefCount[bufid]--;
    }

    return true;
}

/*
 * FlushLocalBuffer -
 *    flushes a local buffer
 */
int
FlushLocalBuffer(Buffer buffer)
{
    int bufid;
    Relation bufrel;
    BufferDesc *bufHdr;

    Assert(BufferIsLocal(buffer));

#ifdef LBDEBUG
    fprintf(stderr, "LB FLUSH %d\n", buffer);
#endif    

    bufid = - (buffer + 1);
    bufHdr = &LocalBufferDescriptors[bufid];
    bufHdr->flags &= ~BM_DIRTY;
    bufrel = RelationIdCacheGetRelation(bufHdr->tag.relId.relId);

    Assert(bufrel != NULL);
    smgrflush(bufrel->rd_rel->relsmgr, bufrel, bufHdr->tag.blockNum,
	      (char *) MAKE_PTR(bufHdr->data));

    Assert(LocalRefCount[bufid] > 0);
    LocalRefCount[bufid]--;
    
    return true;
}

/*
 * InitLocalBuffer -
 *    init the local buffer cache. Since most queries (esp. multi-user ones)
 *    don't involve local buffers, we delay allocating memory for actual the
 *    buffer until we need it.
 */
void
InitLocalBuffer()
{
    int i;
    
    /*
     * these aren't going away. I'm not gonna use palloc.
     */
    LocalBufferDescriptors =
	(BufferDesc *)malloc(sizeof(BufferDesc) * NLocBuffer);
    memset(LocalBufferDescriptors, 0, sizeof(BufferDesc) * NLocBuffer);
    nextFreeLocalBuf = 0;

    for (i = 0; i < NLocBuffer; i++) {
	BufferDesc *buf = &LocalBufferDescriptors[i];

	/*
	 * negative to indicate local buffer. This is tricky: shared buffers
	 * start with 0. We have to start with -2. (Note that the routine
	 * BufferDescriptorGetBuffer adds 1 to buf_id so our first buffer id
	 * is -1.)
	 */
	buf->buf_id = - i - 2;	
    }

    LocalRefCount =
	(long *)malloc(sizeof(long) * NLocBuffer);
    memset(LocalRefCount, 0, sizeof(long) * NLocBuffer);
}

/*
 * LocalBufferSync -
 *    flush all dirty buffers in the local buffer cache. Since the buffer
 *    cache is only used for keeping relations visible during a transaction,
 *    we will not need these buffers again.
 */
void
LocalBufferSync()
{
    int i;
    
    for (i = 0; i < NLocBuffer; i++) {
	BufferDesc *buf = &LocalBufferDescriptors[i];
	Relation bufrel;

	if (buf->flags & BM_DIRTY) {
#ifdef LBDEBUG
	    fprintf(stderr, "LB SYNC %d\n", -i-1);
#endif	    
	    bufrel = RelationIdCacheGetRelation(buf->tag.relId.relId);

	    Assert(bufrel != NULL);
	    
	    smgrwrite(bufrel->rd_rel->relsmgr, bufrel, buf->tag.blockNum,
		      (char *) MAKE_PTR(buf->data));

	    buf->tag.relId.relId = InvalidOid;
	    buf->flags &= ~BM_DIRTY;
	}
    }

    memset(LocalRefCount, 0, sizeof(long) * NLocBuffer);
}

void
ResetLocalBufferPool()
{
    int i;

    memset(LocalBufferDescriptors, 0, sizeof(BufferDesc) * NLocBuffer);
    nextFreeLocalBuf = 0;

    for (i = 0; i < NLocBuffer; i++) {
	BufferDesc *buf = &LocalBufferDescriptors[i];

	/* just like InitLocalBuffer() */
	buf->buf_id = - i - 2;	
    }

    memset(LocalRefCount, 0, sizeof(long) * NLocBuffer);
}
