/*-------------------------------------------------------------------------
 *
 * localbuf.c
 *	  local buffer manager. Fast buffer manager for temporary tables
 *	  or special cases when the operation is not visible to other backends.
 *
 *	  When a relation is being created, the descriptor will have rd_islocal
 *	  set to indicate that the local buffer manager should be used. During
 *	  the same transaction the relation is being created, any inserts or
 *	  selects from the newly created relation will use the local buffer
 *	  pool. rd_islocal is reset at the end of a transaction (commit/abort).
 *	  This is useful for queries like SELECT INTO TABLE and create index.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/types.h>
#include <sys/file.h>
#include <math.h>
#include <signal.h>

#include "executor/execdebug.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "utils/relcache.h"

extern long int LocalBufferFlushCount;

int			NLocBuffer = 64;
BufferDesc *LocalBufferDescriptors = NULL;
Block	   *LocalBufferBlockPointers = NULL;
long	   *LocalRefCount = NULL;

static int	nextFreeLocalBuf = 0;

/*#define LBDEBUG*/

/*
 * LocalBufferAlloc -
 *	  allocate a local buffer. We do round robin allocation for now.
 */
BufferDesc *
LocalBufferAlloc(Relation reln, BlockNumber blockNum, bool *foundPtr)
{
	int			i;
	BufferDesc *bufHdr = (BufferDesc *) NULL;

	if (blockNum == P_NEW)
	{
		blockNum = reln->rd_nblocks;
		reln->rd_nblocks++;
	}

	/* a low tech search for now -- not optimized for scans */
	for (i = 0; i < NLocBuffer; i++)
	{
		if (LocalBufferDescriptors[i].tag.rnode.relNode == 
			reln->rd_node.relNode &&
			LocalBufferDescriptors[i].tag.blockNum == blockNum)
		{

#ifdef LBDEBUG
			fprintf(stderr, "LB ALLOC (%u,%d) %d\n",
					RelationGetRelid(reln), blockNum, -i - 1);
#endif
			LocalRefCount[i]++;
			*foundPtr = TRUE;
			return &LocalBufferDescriptors[i];
		}
	}

#ifdef LBDEBUG
	fprintf(stderr, "LB ALLOC (%u,%d) %d\n",
			RelationGetRelid(reln), blockNum, -nextFreeLocalBuf - 1);
#endif

	/* need to get a new buffer (round robin for now) */
	for (i = 0; i < NLocBuffer; i++)
	{
		int			b = (nextFreeLocalBuf + i) % NLocBuffer;

		if (LocalRefCount[b] == 0)
		{
			bufHdr = &LocalBufferDescriptors[b];
			LocalRefCount[b]++;
			nextFreeLocalBuf = (b + 1) % NLocBuffer;
			break;
		}
	}
	if (bufHdr == NULL)
		elog(ERROR, "no empty local buffer.");

	/*
	 * this buffer is not referenced but it might still be dirty (the last
	 * transaction to touch it doesn't need its contents but has not
	 * flushed it).  if that's the case, write it out before reusing it!
	 */
	if (bufHdr->flags & BM_DIRTY || bufHdr->cntxDirty)
	{
		Relation	bufrel = RelationNodeCacheGetRelation(bufHdr->tag.rnode);

		Assert(bufrel != NULL);

		/* flush this page */
		smgrwrite(DEFAULT_SMGR, bufrel, bufHdr->tag.blockNum,
				  (char *) MAKE_PTR(bufHdr->data));
		LocalBufferFlushCount++;

		/*
		 * drop relcache refcount incremented by
		 * RelationIdCacheGetRelation
		 */
		RelationDecrementReferenceCount(bufrel);
	}

	/*
	 * it's all ours now.
	 *
	 * We need not in tblNode currently but will in future I think,
	 * when we'll give up rel->rd_fd to fmgr cache.
	 */
	bufHdr->tag.rnode = reln->rd_node;
	bufHdr->tag.blockNum = blockNum;
	bufHdr->flags &= ~BM_DIRTY;
	bufHdr->cntxDirty = false;

	/*
	 * lazy memory allocation: allocate space on first use of a buffer.
	 */
	if (bufHdr->data == (SHMEM_OFFSET) 0)
	{
		char	   *data = (char *) malloc(BLCKSZ);

		if (data == NULL)
			elog(FATAL, "Out of memory in LocalBufferAlloc");
		/*
		 * This is a bit of a hack: bufHdr->data needs to be a shmem offset
		 * for consistency with the shared-buffer case, so make it one
		 * even though it's not really a valid shmem offset.
		 */
		bufHdr->data = MAKE_OFFSET(data);
		/*
		 * Set pointer for use by BufferGetBlock() macro.
		 */
		LocalBufferBlockPointers[-(bufHdr->buf_id + 2)] = (Block) data;
	}

	*foundPtr = FALSE;
	return bufHdr;
}

/*
 * WriteLocalBuffer -
 *	  writes out a local buffer
 */
int
WriteLocalBuffer(Buffer buffer, bool release)
{
	int			bufid;

	Assert(BufferIsLocal(buffer));

#ifdef LBDEBUG
	fprintf(stderr, "LB WRITE %d\n", buffer);
#endif

	bufid = -(buffer + 1);
	LocalBufferDescriptors[bufid].flags |= BM_DIRTY;

	if (release)
	{
		Assert(LocalRefCount[bufid] > 0);
		LocalRefCount[bufid]--;
	}

	return true;
}

/*
 * InitLocalBuffer -
 *	  init the local buffer cache. Since most queries (esp. multi-user ones)
 *	  don't involve local buffers, we delay allocating actual memory for the
 *	  buffer until we need it.
 */
void
InitLocalBuffer(void)
{
	int			i;

	/*
	 * these aren't going away. I'm not gonna use palloc.
	 */
	LocalBufferDescriptors =
		(BufferDesc *) calloc(NLocBuffer, sizeof(BufferDesc));
	LocalBufferBlockPointers = (Block *) calloc(NLocBuffer, sizeof(Block));
	LocalRefCount = (long *) calloc(NLocBuffer, sizeof(long));
	nextFreeLocalBuf = 0;

	for (i = 0; i < NLocBuffer; i++)
	{
		BufferDesc *buf = &LocalBufferDescriptors[i];

		/*
		 * negative to indicate local buffer. This is tricky: shared
		 * buffers start with 0. We have to start with -2. (Note that the
		 * routine BufferDescriptorGetBuffer adds 1 to buf_id so our first
		 * buffer id is -1.)
		 */
		buf->buf_id = -i - 2;
	}
}

/*
 * LocalBufferSync
 *
 * Flush all dirty buffers in the local buffer cache at commit time.
 * Since the buffer cache is only used for keeping relations visible
 * during a transaction, we will not need these buffers again.
 *
 * Note that we have to *flush* local buffers because of them are not
 * visible to checkpoint makers. But we can skip XLOG flush check.
 */
void
LocalBufferSync(void)
{
	int			i;

	for (i = 0; i < NLocBuffer; i++)
	{
		BufferDesc *buf = &LocalBufferDescriptors[i];
		Relation	bufrel;

		if (buf->flags & BM_DIRTY || buf->cntxDirty)
		{
#ifdef LBDEBUG
			fprintf(stderr, "LB SYNC %d\n", -i - 1);
#endif
			bufrel = RelationNodeCacheGetRelation(buf->tag.rnode);

			Assert(bufrel != NULL);

			smgrwrite(DEFAULT_SMGR, bufrel, buf->tag.blockNum,
					  (char *) MAKE_PTR(buf->data));
			smgrmarkdirty(DEFAULT_SMGR, bufrel, buf->tag.blockNum);
			LocalBufferFlushCount++;

			/* drop relcache refcount from RelationIdCacheGetRelation */
			RelationDecrementReferenceCount(bufrel);

			buf->flags &= ~BM_DIRTY;
			buf->cntxDirty = false;
		}
	}

	MemSet(LocalRefCount, 0, sizeof(long) * NLocBuffer);
	nextFreeLocalBuf = 0;
}

void
ResetLocalBufferPool(void)
{
	int			i;

	for (i = 0; i < NLocBuffer; i++)
	{
		BufferDesc *buf = &LocalBufferDescriptors[i];

		buf->tag.rnode.relNode = InvalidOid;
		buf->flags &= ~BM_DIRTY;
		buf->cntxDirty = false;
	}

	MemSet(LocalRefCount, 0, sizeof(long) * NLocBuffer);
	nextFreeLocalBuf = 0;
}
