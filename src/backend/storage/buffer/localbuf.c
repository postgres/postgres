/*-------------------------------------------------------------------------
 *
 * localbuf.c
 *	  local buffer manager. Fast buffer manager for temporary tables,
 *	  which never need to be WAL-logged or checkpointed, etc.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/buffer/localbuf.c,v 1.63 2005/03/04 20:21:06 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "utils/relcache.h"
#include "utils/resowner.h"


/*#define LBDEBUG*/

/* Note: this macro only works on local buffers, not shared ones! */
#define LocalBufHdrGetBlock(bufHdr)	\
	LocalBufferBlockPointers[-((bufHdr)->buf_id + 2)]

/* should be a GUC parameter some day */
int			NLocBuffer = 64;

BufferDesc *LocalBufferDescriptors = NULL;
Block	   *LocalBufferBlockPointers = NULL;
int32	   *LocalRefCount = NULL;

static int	nextFreeLocalBuf = 0;


/*
 * LocalBufferAlloc -
 *	  allocate a local buffer. We do round robin allocation for now.
 *
 * API is similar to bufmgr.c's BufferAlloc, except that we do not need
 * to do any locking since this is all local.	Also, IO_IN_PROGRESS
 * does not get set.
 */
BufferDesc *
LocalBufferAlloc(Relation reln, BlockNumber blockNum, bool *foundPtr)
{
	BufferTag	newTag;			/* identity of requested block */
	int			i;
	int			trycounter;
	BufferDesc *bufHdr;

	INIT_BUFFERTAG(newTag, reln, blockNum);

	/* a low tech search for now -- should use a hashtable */
	for (i = 0; i < NLocBuffer; i++)
	{
		bufHdr = &LocalBufferDescriptors[i];
		if (BUFFERTAGS_EQUAL(bufHdr->tag, newTag))
		{
#ifdef LBDEBUG
			fprintf(stderr, "LB ALLOC (%u,%d) %d\n",
					RelationGetRelid(reln), blockNum, -i - 1);
#endif

			LocalRefCount[i]++;
			ResourceOwnerRememberBuffer(CurrentResourceOwner,
									  BufferDescriptorGetBuffer(bufHdr));
			if (bufHdr->flags & BM_VALID)
				*foundPtr = TRUE;
			else
			{
				/* Previous read attempt must have failed; try again */
				*foundPtr = FALSE;
			}
			return bufHdr;
		}
	}

#ifdef LBDEBUG
	fprintf(stderr, "LB ALLOC (%u,%d) %d\n",
			RelationGetRelid(reln), blockNum, -nextFreeLocalBuf - 1);
#endif

	/*
	 * Need to get a new buffer.  We use a clock sweep algorithm
	 * (essentially the same as what freelist.c does now...)
	 */
	trycounter = NLocBuffer;
	for (;;)
	{
		int			b = nextFreeLocalBuf;

		if (++nextFreeLocalBuf >= NLocBuffer)
			nextFreeLocalBuf = 0;

		bufHdr = &LocalBufferDescriptors[b];

		if (LocalRefCount[b] == 0 && bufHdr->usage_count == 0)
		{
			LocalRefCount[b]++;
			ResourceOwnerRememberBuffer(CurrentResourceOwner,
										BufferDescriptorGetBuffer(bufHdr));
			break;
		}

		if (bufHdr->usage_count > 0)
		{
			bufHdr->usage_count--;
			trycounter = NLocBuffer;
		}
		else if (--trycounter == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
					 errmsg("no empty local buffer available")));
	}

	/*
	 * this buffer is not referenced but it might still be dirty. if
	 * that's the case, write it out before reusing it!
	 */
	if (bufHdr->flags & BM_DIRTY)
	{
		SMgrRelation oreln;

		/* Find smgr relation for buffer */
		oreln = smgropen(bufHdr->tag.rnode);

		/* And write... */
		smgrwrite(oreln,
				  bufHdr->tag.blockNum,
				  (char *) LocalBufHdrGetBlock(bufHdr),
				  true);

		LocalBufferFlushCount++;
	}

	/*
	 * lazy memory allocation: allocate space on first use of a buffer.
	 *
	 * Note this path cannot be taken for a buffer that was previously in
	 * use, so it's okay to do it (and possibly error out) before marking
	 * the buffer as not dirty.
	 */
	if (LocalBufHdrGetBlock(bufHdr) == NULL)
	{
		char	   *data = (char *) malloc(BLCKSZ);

		if (data == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));

		/*
		 * Set pointer for use by BufferGetBlock() macro.
		 */
		LocalBufHdrGetBlock(bufHdr) = (Block) data;
	}

	/*
	 * it's all ours now.
	 */
	bufHdr->tag = newTag;
	bufHdr->flags &= ~(BM_VALID | BM_DIRTY | BM_JUST_DIRTIED | BM_IO_ERROR);
	bufHdr->flags |= BM_TAG_VALID;
	bufHdr->usage_count = 0;

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
	BufferDesc *bufHdr;

	Assert(BufferIsLocal(buffer));

#ifdef LBDEBUG
	fprintf(stderr, "LB WRITE %d\n", buffer);
#endif

	bufid = -(buffer + 1);

	Assert(LocalRefCount[bufid] > 0);

	bufHdr = &LocalBufferDescriptors[bufid];
	bufHdr->flags |= BM_DIRTY;

	if (release)
	{
		LocalRefCount[bufid]--;
		if (LocalRefCount[bufid] == 0 &&
			bufHdr->usage_count < BM_MAX_USAGE_COUNT)
			bufHdr->usage_count++;
		ResourceOwnerForgetBuffer(CurrentResourceOwner, buffer);
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
	LocalBufferDescriptors = (BufferDesc *)
		calloc(NLocBuffer, sizeof(*LocalBufferDescriptors));
	LocalBufferBlockPointers = (Block *)
		calloc(NLocBuffer, sizeof(*LocalBufferBlockPointers));
	LocalRefCount = (int32 *)
		calloc(NLocBuffer, sizeof(*LocalRefCount));
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
#ifdef USE_ASSERT_CHECKING
	int			i;

	for (i = 0; i < NLocBuffer; i++)
	{
		Assert(LocalRefCount[i] == 0);
	}
#endif
}
