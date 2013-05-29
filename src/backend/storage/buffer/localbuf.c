/*-------------------------------------------------------------------------
 *
 * localbuf.c
 *	  local buffer manager. Fast buffer manager for temporary tables,
 *	  which never need to be WAL-logged or checkpointed, etc.
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/localbuf.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/catalog.h"
#include "common/relpath.h"
#include "executor/instrument.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/resowner_private.h"


/*#define LBDEBUG*/

/* entry for buffer lookup hashtable */
typedef struct
{
	BufferTag	key;			/* Tag of a disk page */
	int			id;				/* Associated local buffer's index */
} LocalBufferLookupEnt;

/* Note: this macro only works on local buffers, not shared ones! */
#define LocalBufHdrGetBlock(bufHdr) \
	LocalBufferBlockPointers[-((bufHdr)->buf_id + 2)]

int			NLocBuffer = 0;		/* until buffers are initialized */

BufferDesc *LocalBufferDescriptors = NULL;
Block	   *LocalBufferBlockPointers = NULL;
int32	   *LocalRefCount = NULL;

static int	nextFreeLocalBuf = 0;

static HTAB *LocalBufHash = NULL;


static void InitLocalBuffers(void);
static Block GetLocalBufferStorage(void);


/*
 * LocalPrefetchBuffer -
 *	  initiate asynchronous read of a block of a relation
 *
 * Do PrefetchBuffer's work for temporary relations.
 * No-op if prefetching isn't compiled in.
 */
void
LocalPrefetchBuffer(SMgrRelation smgr, ForkNumber forkNum,
					BlockNumber blockNum)
{
#ifdef USE_PREFETCH
	BufferTag	newTag;			/* identity of requested block */
	LocalBufferLookupEnt *hresult;

	INIT_BUFFERTAG(newTag, smgr->smgr_rnode.node, forkNum, blockNum);

	/* Initialize local buffers if first request in this session */
	if (LocalBufHash == NULL)
		InitLocalBuffers();

	/* See if the desired buffer already exists */
	hresult = (LocalBufferLookupEnt *)
		hash_search(LocalBufHash, (void *) &newTag, HASH_FIND, NULL);

	if (hresult)
	{
		/* Yes, so nothing to do */
		return;
	}

	/* Not in buffers, so initiate prefetch */
	smgrprefetch(smgr, forkNum, blockNum);
#endif   /* USE_PREFETCH */
}


/*
 * LocalBufferAlloc -
 *	  Find or create a local buffer for the given page of the given relation.
 *
 * API is similar to bufmgr.c's BufferAlloc, except that we do not need
 * to do any locking since this is all local.	Also, IO_IN_PROGRESS
 * does not get set.  Lastly, we support only default access strategy
 * (hence, usage_count is always advanced).
 */
BufferDesc *
LocalBufferAlloc(SMgrRelation smgr, ForkNumber forkNum, BlockNumber blockNum,
				 bool *foundPtr)
{
	BufferTag	newTag;			/* identity of requested block */
	LocalBufferLookupEnt *hresult;
	BufferDesc *bufHdr;
	int			b;
	int			trycounter;
	bool		found;

	INIT_BUFFERTAG(newTag, smgr->smgr_rnode.node, forkNum, blockNum);

	/* Initialize local buffers if first request in this session */
	if (LocalBufHash == NULL)
		InitLocalBuffers();

	/* See if the desired buffer already exists */
	hresult = (LocalBufferLookupEnt *)
		hash_search(LocalBufHash, (void *) &newTag, HASH_FIND, NULL);

	if (hresult)
	{
		b = hresult->id;
		bufHdr = &LocalBufferDescriptors[b];
		Assert(BUFFERTAGS_EQUAL(bufHdr->tag, newTag));
#ifdef LBDEBUG
		fprintf(stderr, "LB ALLOC (%u,%d,%d) %d\n",
				smgr->smgr_rnode.node.relNode, forkNum, blockNum, -b - 1);
#endif
		/* this part is equivalent to PinBuffer for a shared buffer */
		if (LocalRefCount[b] == 0)
		{
			if (bufHdr->usage_count < BM_MAX_USAGE_COUNT)
				bufHdr->usage_count++;
		}
		LocalRefCount[b]++;
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

#ifdef LBDEBUG
	fprintf(stderr, "LB ALLOC (%u,%d,%d) %d\n",
			smgr->smgr_rnode.node.relNode, forkNum, blockNum,
			-nextFreeLocalBuf - 1);
#endif

	/*
	 * Need to get a new buffer.  We use a clock sweep algorithm (essentially
	 * the same as what freelist.c does now...)
	 */
	trycounter = NLocBuffer;
	for (;;)
	{
		b = nextFreeLocalBuf;

		if (++nextFreeLocalBuf >= NLocBuffer)
			nextFreeLocalBuf = 0;

		bufHdr = &LocalBufferDescriptors[b];

		if (LocalRefCount[b] == 0)
		{
			if (bufHdr->usage_count > 0)
			{
				bufHdr->usage_count--;
				trycounter = NLocBuffer;
			}
			else
			{
				/* Found a usable buffer */
				LocalRefCount[b]++;
				ResourceOwnerRememberBuffer(CurrentResourceOwner,
										  BufferDescriptorGetBuffer(bufHdr));
				break;
			}
		}
		else if (--trycounter == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
					 errmsg("no empty local buffer available")));
	}

	/*
	 * this buffer is not referenced but it might still be dirty. if that's
	 * the case, write it out before reusing it!
	 */
	if (bufHdr->flags & BM_DIRTY)
	{
		SMgrRelation oreln;
		Page		localpage = (char *) LocalBufHdrGetBlock(bufHdr);

		/* Find smgr relation for buffer */
		oreln = smgropen(bufHdr->tag.rnode, MyBackendId);

		PageSetChecksumInplace(localpage, bufHdr->tag.blockNum);

		/* And write... */
		smgrwrite(oreln,
				  bufHdr->tag.forkNum,
				  bufHdr->tag.blockNum,
				  localpage,
				  false);

		/* Mark not-dirty now in case we error out below */
		bufHdr->flags &= ~BM_DIRTY;

		pgBufferUsage.local_blks_written++;
	}

	/*
	 * lazy memory allocation: allocate space on first use of a buffer.
	 */
	if (LocalBufHdrGetBlock(bufHdr) == NULL)
	{
		/* Set pointer for use by BufferGetBlock() macro */
		LocalBufHdrGetBlock(bufHdr) = GetLocalBufferStorage();
	}

	/*
	 * Update the hash table: remove old entry, if any, and make new one.
	 */
	if (bufHdr->flags & BM_TAG_VALID)
	{
		hresult = (LocalBufferLookupEnt *)
			hash_search(LocalBufHash, (void *) &bufHdr->tag,
						HASH_REMOVE, NULL);
		if (!hresult)			/* shouldn't happen */
			elog(ERROR, "local buffer hash table corrupted");
		/* mark buffer invalid just in case hash insert fails */
		CLEAR_BUFFERTAG(bufHdr->tag);
		bufHdr->flags &= ~(BM_VALID | BM_TAG_VALID);
	}

	hresult = (LocalBufferLookupEnt *)
		hash_search(LocalBufHash, (void *) &newTag, HASH_ENTER, &found);
	if (found)					/* shouldn't happen */
		elog(ERROR, "local buffer hash table corrupted");
	hresult->id = b;

	/*
	 * it's all ours now.
	 */
	bufHdr->tag = newTag;
	bufHdr->flags &= ~(BM_VALID | BM_DIRTY | BM_JUST_DIRTIED | BM_IO_ERROR);
	bufHdr->flags |= BM_TAG_VALID;
	bufHdr->usage_count = 1;

	*foundPtr = FALSE;
	return bufHdr;
}

/*
 * MarkLocalBufferDirty -
 *	  mark a local buffer dirty
 */
void
MarkLocalBufferDirty(Buffer buffer)
{
	int			bufid;
	BufferDesc *bufHdr;

	Assert(BufferIsLocal(buffer));

#ifdef LBDEBUG
	fprintf(stderr, "LB DIRTY %d\n", buffer);
#endif

	bufid = -(buffer + 1);

	Assert(LocalRefCount[bufid] > 0);

	bufHdr = &LocalBufferDescriptors[bufid];

	if (!(bufHdr->flags & BM_DIRTY))
		pgBufferUsage.local_blks_dirtied++;

	bufHdr->flags |= BM_DIRTY;
}

/*
 * DropRelFileNodeLocalBuffers
 *		This function removes from the buffer pool all the pages of the
 *		specified relation that have block numbers >= firstDelBlock.
 *		(In particular, with firstDelBlock = 0, all pages are removed.)
 *		Dirty pages are simply dropped, without bothering to write them
 *		out first.	Therefore, this is NOT rollback-able, and so should be
 *		used only with extreme caution!
 *
 *		See DropRelFileNodeBuffers in bufmgr.c for more notes.
 */
void
DropRelFileNodeLocalBuffers(RelFileNode rnode, ForkNumber forkNum,
							BlockNumber firstDelBlock)
{
	int			i;

	for (i = 0; i < NLocBuffer; i++)
	{
		BufferDesc *bufHdr = &LocalBufferDescriptors[i];
		LocalBufferLookupEnt *hresult;

		if ((bufHdr->flags & BM_TAG_VALID) &&
			RelFileNodeEquals(bufHdr->tag.rnode, rnode) &&
			bufHdr->tag.forkNum == forkNum &&
			bufHdr->tag.blockNum >= firstDelBlock)
		{
			if (LocalRefCount[i] != 0)
				elog(ERROR, "block %u of %s is still referenced (local %u)",
					 bufHdr->tag.blockNum,
					 relpathbackend(bufHdr->tag.rnode, MyBackendId,
									bufHdr->tag.forkNum),
					 LocalRefCount[i]);
			/* Remove entry from hashtable */
			hresult = (LocalBufferLookupEnt *)
				hash_search(LocalBufHash, (void *) &bufHdr->tag,
							HASH_REMOVE, NULL);
			if (!hresult)		/* shouldn't happen */
				elog(ERROR, "local buffer hash table corrupted");
			/* Mark buffer invalid */
			CLEAR_BUFFERTAG(bufHdr->tag);
			bufHdr->flags = 0;
			bufHdr->usage_count = 0;
		}
	}
}

/*
 * DropRelFileNodeAllLocalBuffers
 *		This function removes from the buffer pool all pages of all forks
 *		of the specified relation.
 *
 *		See DropRelFileNodeAllBuffers in bufmgr.c for more notes.
 */
void
DropRelFileNodeAllLocalBuffers(RelFileNode rnode)
{
	int			i;

	for (i = 0; i < NLocBuffer; i++)
	{
		BufferDesc *bufHdr = &LocalBufferDescriptors[i];
		LocalBufferLookupEnt *hresult;

		if ((bufHdr->flags & BM_TAG_VALID) &&
			RelFileNodeEquals(bufHdr->tag.rnode, rnode))
		{
			if (LocalRefCount[i] != 0)
				elog(ERROR, "block %u of %s is still referenced (local %u)",
					 bufHdr->tag.blockNum,
					 relpathbackend(bufHdr->tag.rnode, MyBackendId,
									bufHdr->tag.forkNum),
					 LocalRefCount[i]);
			/* Remove entry from hashtable */
			hresult = (LocalBufferLookupEnt *)
				hash_search(LocalBufHash, (void *) &bufHdr->tag,
							HASH_REMOVE, NULL);
			if (!hresult)		/* shouldn't happen */
				elog(ERROR, "local buffer hash table corrupted");
			/* Mark buffer invalid */
			CLEAR_BUFFERTAG(bufHdr->tag);
			bufHdr->flags = 0;
			bufHdr->usage_count = 0;
		}
	}
}

/*
 * InitLocalBuffers -
 *	  init the local buffer cache. Since most queries (esp. multi-user ones)
 *	  don't involve local buffers, we delay allocating actual memory for the
 *	  buffers until we need them; just make the buffer headers here.
 */
static void
InitLocalBuffers(void)
{
	int			nbufs = num_temp_buffers;
	HASHCTL		info;
	int			i;

	/* Allocate and zero buffer headers and auxiliary arrays */
	LocalBufferDescriptors = (BufferDesc *) calloc(nbufs, sizeof(BufferDesc));
	LocalBufferBlockPointers = (Block *) calloc(nbufs, sizeof(Block));
	LocalRefCount = (int32 *) calloc(nbufs, sizeof(int32));
	if (!LocalBufferDescriptors || !LocalBufferBlockPointers || !LocalRefCount)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	nextFreeLocalBuf = 0;

	/* initialize fields that need to start off nonzero */
	for (i = 0; i < nbufs; i++)
	{
		BufferDesc *buf = &LocalBufferDescriptors[i];

		/*
		 * negative to indicate local buffer. This is tricky: shared buffers
		 * start with 0. We have to start with -2. (Note that the routine
		 * BufferDescriptorGetBuffer adds 1 to buf_id so our first buffer id
		 * is -1.)
		 */
		buf->buf_id = -i - 2;
	}

	/* Create the lookup hash table */
	MemSet(&info, 0, sizeof(info));
	info.keysize = sizeof(BufferTag);
	info.entrysize = sizeof(LocalBufferLookupEnt);
	info.hash = tag_hash;

	LocalBufHash = hash_create("Local Buffer Lookup Table",
							   nbufs,
							   &info,
							   HASH_ELEM | HASH_FUNCTION);

	if (!LocalBufHash)
		elog(ERROR, "could not initialize local buffer hash table");

	/* Initialization done, mark buffers allocated */
	NLocBuffer = nbufs;
}

/*
 * GetLocalBufferStorage - allocate memory for a local buffer
 *
 * The idea of this function is to aggregate our requests for storage
 * so that the memory manager doesn't see a whole lot of relatively small
 * requests.  Since we'll never give back a local buffer once it's created
 * within a particular process, no point in burdening memmgr with separately
 * managed chunks.
 */
static Block
GetLocalBufferStorage(void)
{
	static char *cur_block = NULL;
	static int	next_buf_in_block = 0;
	static int	num_bufs_in_block = 0;
	static int	total_bufs_allocated = 0;
	static MemoryContext LocalBufferContext = NULL;

	char	   *this_buf;

	Assert(total_bufs_allocated < NLocBuffer);

	if (next_buf_in_block >= num_bufs_in_block)
	{
		/* Need to make a new request to memmgr */
		int			num_bufs;

		/*
		 * We allocate local buffers in a context of their own, so that the
		 * space eaten for them is easily recognizable in MemoryContextStats
		 * output.	Create the context on first use.
		 */
		if (LocalBufferContext == NULL)
			LocalBufferContext =
				AllocSetContextCreate(TopMemoryContext,
									  "LocalBufferContext",
									  ALLOCSET_DEFAULT_MINSIZE,
									  ALLOCSET_DEFAULT_INITSIZE,
									  ALLOCSET_DEFAULT_MAXSIZE);

		/* Start with a 16-buffer request; subsequent ones double each time */
		num_bufs = Max(num_bufs_in_block * 2, 16);
		/* But not more than what we need for all remaining local bufs */
		num_bufs = Min(num_bufs, NLocBuffer - total_bufs_allocated);
		/* And don't overflow MaxAllocSize, either */
		num_bufs = Min(num_bufs, MaxAllocSize / BLCKSZ);

		cur_block = (char *) MemoryContextAlloc(LocalBufferContext,
												num_bufs * BLCKSZ);
		next_buf_in_block = 0;
		num_bufs_in_block = num_bufs;
	}

	/* Allocate next buffer in current memory block */
	this_buf = cur_block + next_buf_in_block * BLCKSZ;
	next_buf_in_block++;
	total_bufs_allocated++;

	return (Block) this_buf;
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
	if (assert_enabled && LocalRefCount)
	{
		int			RefCountErrors = 0;
		int			i;

		for (i = 0; i < NLocBuffer; i++)
		{
			if (LocalRefCount[i] != 0)
			{
				Buffer		b = -i - 1;

				PrintBufferLeakWarning(b);
				RefCountErrors++;
			}
		}
		Assert(RefCountErrors == 0);
	}
#endif
}

/*
 * AtProcExit_LocalBuffers - ensure we have dropped pins during backend exit.
 *
 * This is just like AtProcExit_Buffers, but for local buffers.  We shouldn't
 * be holding any remaining pins; if we are, and assertions aren't enabled,
 * we'll fail later in DropRelFileNodeBuffers while trying to drop the temp
 * rels.
 */
void
AtProcExit_LocalBuffers(void)
{
#ifdef USE_ASSERT_CHECKING
	if (assert_enabled && LocalRefCount)
	{
		int			RefCountErrors = 0;
		int			i;

		for (i = 0; i < NLocBuffer; i++)
		{
			if (LocalRefCount[i] != 0)
			{
				Buffer		b = -i - 1;

				PrintBufferLeakWarning(b);
				RefCountErrors++;
			}
		}
		Assert(RefCountErrors == 0);
	}
#endif
}
