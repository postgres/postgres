/*-------------------------------------------------------------------------
 *
 * localbuf.c
 *	  local buffer manager. Fast buffer manager for temporary tables,
 *	  which never need to be WAL-logged or checkpointed, etc.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/buffer/localbuf.c,v 1.49 2003/08/04 02:40:03 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "utils/relcache.h"


/*#define LBDEBUG*/

/* should be a GUC parameter some day */
int			NLocBuffer = 64;

BufferDesc *LocalBufferDescriptors = NULL;
Block	   *LocalBufferBlockPointers = NULL;
long	   *LocalRefCount = NULL;

static int	nextFreeLocalBuf = 0;


/*
 * LocalBufferAlloc -
 *	  allocate a local buffer. We do round robin allocation for now.
 */
BufferDesc *
LocalBufferAlloc(Relation reln, BlockNumber blockNum, bool *foundPtr)
{
	int			i;
	BufferDesc *bufHdr = (BufferDesc *) NULL;

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
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("no empty local buffer available")));

	/*
	 * this buffer is not referenced but it might still be dirty. if
	 * that's the case, write it out before reusing it!
	 */
	if (bufHdr->flags & BM_DIRTY || bufHdr->cntxDirty)
	{
		Relation	bufrel = RelationNodeCacheGetRelation(bufHdr->tag.rnode);

		/* flush this page */
		if (bufrel == (Relation) NULL)
		{
			smgrblindwrt(DEFAULT_SMGR,
						 bufHdr->tag.rnode,
						 bufHdr->tag.blockNum,
						 (char *) MAKE_PTR(bufHdr->data));
		}
		else
		{
			smgrwrite(DEFAULT_SMGR, bufrel,
					  bufHdr->tag.blockNum,
					  (char *) MAKE_PTR(bufHdr->data));
			/* drop refcount incremented by RelationNodeCacheGetRelation */
			RelationDecrementReferenceCount(bufrel);
		}

		LocalBufferFlushCount++;
	}

	/*
	 * lazy memory allocation: allocate space on first use of a buffer.
	 *
	 * Note this path cannot be taken for a buffer that was previously in
	 * use, so it's okay to do it (and possibly error out) before marking
	 * the buffer as valid.
	 */
	if (bufHdr->data == (SHMEM_OFFSET) 0)
	{
		char	   *data = (char *) malloc(BLCKSZ);

		if (data == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));

		/*
		 * This is a bit of a hack: bufHdr->data needs to be a shmem
		 * offset for consistency with the shared-buffer case, so make it
		 * one even though it's not really a valid shmem offset.
		 */
		bufHdr->data = MAKE_OFFSET(data);

		/*
		 * Set pointer for use by BufferGetBlock() macro.
		 */
		LocalBufferBlockPointers[-(bufHdr->buf_id + 2)] = (Block) data;
	}

	/*
	 * it's all ours now.
	 *
	 * We need not in tblNode currently but will in future I think, when
	 * we'll give up rel->rd_fd to fmgr cache.
	 */
	bufHdr->tag.rnode = reln->rd_node;
	bufHdr->tag.blockNum = blockNum;
	bufHdr->flags &= ~BM_DIRTY;
	bufHdr->cntxDirty = false;

	*foundPtr = FALSE;
	return bufHdr;
}

/*
 * WriteLocalBuffer -
 *	  writes out a local buffer (actually, just marks it dirty)
 */
void
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
}

/*
 * InitLocalBuffer -
 *	  init the local buffer cache. Since most queries (esp. multi-user ones)
 *	  don't involve local buffers, we delay allocating actual memory for the
 *	  buffers until we need them; just make the buffer headers here.
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
 * AtEOXact_LocalBuffers - clean up at end of transaction.
 *
 * This is just like AtEOXact_Buffers, but for local buffers.
 */
void
AtEOXact_LocalBuffers(bool isCommit)
{
	int			i;

	for (i = 0; i < NLocBuffer; i++)
	{
		if (LocalRefCount[i] != 0)
		{
			BufferDesc *buf = &(LocalBufferDescriptors[i]);

			if (isCommit)
				elog(WARNING,
					 "local buffer leak: [%03d] (rel=%u/%u, blockNum=%u, flags=0x%x, refcount=%d %ld)",
					 i,
					 buf->tag.rnode.tblNode, buf->tag.rnode.relNode,
					 buf->tag.blockNum, buf->flags,
					 buf->refcount, LocalRefCount[i]);

			LocalRefCount[i] = 0;
		}
	}
}
