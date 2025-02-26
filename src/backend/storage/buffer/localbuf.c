/*-------------------------------------------------------------------------
 *
 * localbuf.c
 *	  local buffer manager. Fast buffer manager for temporary tables,
 *	  which never need to be WAL-logged or checkpointed, etc.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/localbuf.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/parallel.h"
#include "executor/instrument.h"
#include "pgstat.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "utils/guc_hooks.h"
#include "utils/memutils.h"
#include "utils/resowner.h"


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

static int	nextFreeLocalBufId = 0;

static HTAB *LocalBufHash = NULL;

/* number of local buffers pinned at least once */
static int	NLocalPinnedBuffers = 0;


static void InitLocalBuffers(void);
static Block GetLocalBufferStorage(void);
static Buffer GetLocalVictimBuffer(void);


/*
 * PrefetchLocalBuffer -
 *	  initiate asynchronous read of a block of a relation
 *
 * Do PrefetchBuffer's work for temporary relations.
 * No-op if prefetching isn't compiled in.
 */
PrefetchBufferResult
PrefetchLocalBuffer(SMgrRelation smgr, ForkNumber forkNum,
					BlockNumber blockNum)
{
	PrefetchBufferResult result = {InvalidBuffer, false};
	BufferTag	newTag;			/* identity of requested block */
	LocalBufferLookupEnt *hresult;

	InitBufferTag(&newTag, &smgr->smgr_rlocator.locator, forkNum, blockNum);

	/* Initialize local buffers if first request in this session */
	if (LocalBufHash == NULL)
		InitLocalBuffers();

	/* See if the desired buffer already exists */
	hresult = (LocalBufferLookupEnt *)
		hash_search(LocalBufHash, &newTag, HASH_FIND, NULL);

	if (hresult)
	{
		/* Yes, so nothing to do */
		result.recent_buffer = -hresult->id - 1;
	}
	else
	{
#ifdef USE_PREFETCH
		/* Not in buffers, so initiate prefetch */
		if ((io_direct_flags & IO_DIRECT_DATA) == 0 &&
			smgrprefetch(smgr, forkNum, blockNum, 1))
		{
			result.initiated_io = true;
		}
#endif							/* USE_PREFETCH */
	}

	return result;
}


/*
 * LocalBufferAlloc -
 *	  Find or create a local buffer for the given page of the given relation.
 *
 * API is similar to bufmgr.c's BufferAlloc, except that we do not need to do
 * any locking since this is all local.  We support only default access
 * strategy (hence, usage_count is always advanced).
 */
BufferDesc *
LocalBufferAlloc(SMgrRelation smgr, ForkNumber forkNum, BlockNumber blockNum,
				 bool *foundPtr)
{
	BufferTag	newTag;			/* identity of requested block */
	LocalBufferLookupEnt *hresult;
	BufferDesc *bufHdr;
	Buffer		victim_buffer;
	int			bufid;
	bool		found;

	InitBufferTag(&newTag, &smgr->smgr_rlocator.locator, forkNum, blockNum);

	/* Initialize local buffers if first request in this session */
	if (LocalBufHash == NULL)
		InitLocalBuffers();

	ResourceOwnerEnlarge(CurrentResourceOwner);

	/* See if the desired buffer already exists */
	hresult = (LocalBufferLookupEnt *)
		hash_search(LocalBufHash, &newTag, HASH_FIND, NULL);

	if (hresult)
	{
		bufid = hresult->id;
		bufHdr = GetLocalBufferDescriptor(bufid);
		Assert(BufferTagsEqual(&bufHdr->tag, &newTag));

		*foundPtr = PinLocalBuffer(bufHdr, true);
	}
	else
	{
		uint32		buf_state;

		victim_buffer = GetLocalVictimBuffer();
		bufid = -victim_buffer - 1;
		bufHdr = GetLocalBufferDescriptor(bufid);

		hresult = (LocalBufferLookupEnt *)
			hash_search(LocalBufHash, &newTag, HASH_ENTER, &found);
		if (found)				/* shouldn't happen */
			elog(ERROR, "local buffer hash table corrupted");
		hresult->id = bufid;

		/*
		 * it's all ours now.
		 */
		bufHdr->tag = newTag;

		buf_state = pg_atomic_read_u32(&bufHdr->state);
		buf_state &= ~(BUF_FLAG_MASK | BUF_USAGECOUNT_MASK);
		buf_state |= BM_TAG_VALID | BUF_USAGECOUNT_ONE;
		pg_atomic_unlocked_write_u32(&bufHdr->state, buf_state);

		*foundPtr = false;
	}

	return bufHdr;
}

static Buffer
GetLocalVictimBuffer(void)
{
	int			victim_bufid;
	int			trycounter;
	uint32		buf_state;
	BufferDesc *bufHdr;

	ResourceOwnerEnlarge(CurrentResourceOwner);

	/*
	 * Need to get a new buffer.  We use a clock sweep algorithm (essentially
	 * the same as what freelist.c does now...)
	 */
	trycounter = NLocBuffer;
	for (;;)
	{
		victim_bufid = nextFreeLocalBufId;

		if (++nextFreeLocalBufId >= NLocBuffer)
			nextFreeLocalBufId = 0;

		bufHdr = GetLocalBufferDescriptor(victim_bufid);

		if (LocalRefCount[victim_bufid] == 0)
		{
			buf_state = pg_atomic_read_u32(&bufHdr->state);

			if (BUF_STATE_GET_USAGECOUNT(buf_state) > 0)
			{
				buf_state -= BUF_USAGECOUNT_ONE;
				pg_atomic_unlocked_write_u32(&bufHdr->state, buf_state);
				trycounter = NLocBuffer;
			}
			else
			{
				/* Found a usable buffer */
				PinLocalBuffer(bufHdr, false);
				break;
			}
		}
		else if (--trycounter == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
					 errmsg("no empty local buffer available")));
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
	 * this buffer is not referenced but it might still be dirty. if that's
	 * the case, write it out before reusing it!
	 */
	if (buf_state & BM_DIRTY)
	{
		instr_time	io_start;
		SMgrRelation oreln;
		Page		localpage = (char *) LocalBufHdrGetBlock(bufHdr);

		/* Find smgr relation for buffer */
		oreln = smgropen(BufTagGetRelFileLocator(&bufHdr->tag), MyProcNumber);

		PageSetChecksumInplace(localpage, bufHdr->tag.blockNum);

		io_start = pgstat_prepare_io_time(track_io_timing);

		/* And write... */
		smgrwrite(oreln,
				  BufTagGetForkNum(&bufHdr->tag),
				  bufHdr->tag.blockNum,
				  localpage,
				  false);

		/* Temporary table I/O does not use Buffer Access Strategies */
		pgstat_count_io_op_time(IOOBJECT_TEMP_RELATION, IOCONTEXT_NORMAL,
								IOOP_WRITE, io_start, 1, BLCKSZ);

		/* Mark not-dirty now in case we error out below */
		buf_state &= ~BM_DIRTY;
		pg_atomic_unlocked_write_u32(&bufHdr->state, buf_state);

		pgBufferUsage.local_blks_written++;
	}

	/*
	 * Remove the victim buffer from the hashtable and mark as invalid.
	 */
	if (buf_state & BM_TAG_VALID)
	{
		LocalBufferLookupEnt *hresult;

		hresult = (LocalBufferLookupEnt *)
			hash_search(LocalBufHash, &bufHdr->tag, HASH_REMOVE, NULL);
		if (!hresult)			/* shouldn't happen */
			elog(ERROR, "local buffer hash table corrupted");
		/* mark buffer invalid just in case hash insert fails */
		ClearBufferTag(&bufHdr->tag);
		buf_state &= ~(BUF_FLAG_MASK | BUF_USAGECOUNT_MASK);
		pg_atomic_unlocked_write_u32(&bufHdr->state, buf_state);

		pgstat_count_io_op(IOOBJECT_TEMP_RELATION, IOCONTEXT_NORMAL, IOOP_EVICT, 1, 0);
	}

	return BufferDescriptorGetBuffer(bufHdr);
}

/* see LimitAdditionalPins() */
void
LimitAdditionalLocalPins(uint32 *additional_pins)
{
	uint32		max_pins;

	if (*additional_pins <= 1)
		return;

	/*
	 * In contrast to LimitAdditionalPins() other backends don't play a role
	 * here. We can allow up to NLocBuffer pins in total, but it might not be
	 * initialized yet so read num_temp_buffers.
	 */
	max_pins = (num_temp_buffers - NLocalPinnedBuffers);

	if (*additional_pins >= max_pins)
		*additional_pins = max_pins;
}

/*
 * Implementation of ExtendBufferedRelBy() and ExtendBufferedRelTo() for
 * temporary buffers.
 */
BlockNumber
ExtendBufferedRelLocal(BufferManagerRelation bmr,
					   ForkNumber fork,
					   uint32 flags,
					   uint32 extend_by,
					   BlockNumber extend_upto,
					   Buffer *buffers,
					   uint32 *extended_by)
{
	BlockNumber first_block;
	instr_time	io_start;

	/* Initialize local buffers if first request in this session */
	if (LocalBufHash == NULL)
		InitLocalBuffers();

	LimitAdditionalLocalPins(&extend_by);

	for (uint32 i = 0; i < extend_by; i++)
	{
		BufferDesc *buf_hdr;
		Block		buf_block;

		buffers[i] = GetLocalVictimBuffer();
		buf_hdr = GetLocalBufferDescriptor(-buffers[i] - 1);
		buf_block = LocalBufHdrGetBlock(buf_hdr);

		/* new buffers are zero-filled */
		MemSet(buf_block, 0, BLCKSZ);
	}

	first_block = smgrnblocks(bmr.smgr, fork);

	if (extend_upto != InvalidBlockNumber)
	{
		/*
		 * In contrast to shared relations, nothing could change the relation
		 * size concurrently. Thus we shouldn't end up finding that we don't
		 * need to do anything.
		 */
		Assert(first_block <= extend_upto);

		Assert((uint64) first_block + extend_by <= extend_upto);
	}

	/* Fail if relation is already at maximum possible length */
	if ((uint64) first_block + extend_by >= MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("cannot extend relation %s beyond %u blocks",
						relpath(bmr.smgr->smgr_rlocator, fork).str,
						MaxBlockNumber)));

	for (uint32 i = 0; i < extend_by; i++)
	{
		int			victim_buf_id;
		BufferDesc *victim_buf_hdr;
		BufferTag	tag;
		LocalBufferLookupEnt *hresult;
		bool		found;

		victim_buf_id = -buffers[i] - 1;
		victim_buf_hdr = GetLocalBufferDescriptor(victim_buf_id);

		/* in case we need to pin an existing buffer below */
		ResourceOwnerEnlarge(CurrentResourceOwner);

		InitBufferTag(&tag, &bmr.smgr->smgr_rlocator.locator, fork, first_block + i);

		hresult = (LocalBufferLookupEnt *)
			hash_search(LocalBufHash, &tag, HASH_ENTER, &found);
		if (found)
		{
			BufferDesc *existing_hdr;
			uint32		buf_state;

			UnpinLocalBuffer(BufferDescriptorGetBuffer(victim_buf_hdr));

			existing_hdr = GetLocalBufferDescriptor(hresult->id);
			PinLocalBuffer(existing_hdr, false);
			buffers[i] = BufferDescriptorGetBuffer(existing_hdr);

			buf_state = pg_atomic_read_u32(&existing_hdr->state);
			Assert(buf_state & BM_TAG_VALID);
			Assert(!(buf_state & BM_DIRTY));
			buf_state &= ~BM_VALID;
			pg_atomic_unlocked_write_u32(&existing_hdr->state, buf_state);
		}
		else
		{
			uint32		buf_state = pg_atomic_read_u32(&victim_buf_hdr->state);

			Assert(!(buf_state & (BM_VALID | BM_TAG_VALID | BM_DIRTY | BM_JUST_DIRTIED)));

			victim_buf_hdr->tag = tag;

			buf_state |= BM_TAG_VALID | BUF_USAGECOUNT_ONE;

			pg_atomic_unlocked_write_u32(&victim_buf_hdr->state, buf_state);

			hresult->id = victim_buf_id;
		}
	}

	io_start = pgstat_prepare_io_time(track_io_timing);

	/* actually extend relation */
	smgrzeroextend(bmr.smgr, fork, first_block, extend_by, false);

	pgstat_count_io_op_time(IOOBJECT_TEMP_RELATION, IOCONTEXT_NORMAL, IOOP_EXTEND,
							io_start, 1, extend_by * BLCKSZ);

	for (uint32 i = 0; i < extend_by; i++)
	{
		Buffer		buf = buffers[i];
		BufferDesc *buf_hdr;
		uint32		buf_state;

		buf_hdr = GetLocalBufferDescriptor(-buf - 1);

		buf_state = pg_atomic_read_u32(&buf_hdr->state);
		buf_state |= BM_VALID;
		pg_atomic_unlocked_write_u32(&buf_hdr->state, buf_state);
	}

	*extended_by = extend_by;

	pgBufferUsage.local_blks_written += extend_by;

	return first_block;
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
	uint32		buf_state;

	Assert(BufferIsLocal(buffer));

#ifdef LBDEBUG
	fprintf(stderr, "LB DIRTY %d\n", buffer);
#endif

	bufid = -buffer - 1;

	Assert(LocalRefCount[bufid] > 0);

	bufHdr = GetLocalBufferDescriptor(bufid);

	buf_state = pg_atomic_read_u32(&bufHdr->state);

	if (!(buf_state & BM_DIRTY))
		pgBufferUsage.local_blks_dirtied++;

	buf_state |= BM_DIRTY;

	pg_atomic_unlocked_write_u32(&bufHdr->state, buf_state);
}

/*
 * DropRelationLocalBuffers
 *		This function removes from the buffer pool all the pages of the
 *		specified relation that have block numbers >= firstDelBlock.
 *		(In particular, with firstDelBlock = 0, all pages are removed.)
 *		Dirty pages are simply dropped, without bothering to write them
 *		out first.  Therefore, this is NOT rollback-able, and so should be
 *		used only with extreme caution!
 *
 *		See DropRelationBuffers in bufmgr.c for more notes.
 */
void
DropRelationLocalBuffers(RelFileLocator rlocator, ForkNumber forkNum,
						 BlockNumber firstDelBlock)
{
	int			i;

	for (i = 0; i < NLocBuffer; i++)
	{
		BufferDesc *bufHdr = GetLocalBufferDescriptor(i);
		LocalBufferLookupEnt *hresult;
		uint32		buf_state;

		buf_state = pg_atomic_read_u32(&bufHdr->state);

		if ((buf_state & BM_TAG_VALID) &&
			BufTagMatchesRelFileLocator(&bufHdr->tag, &rlocator) &&
			BufTagGetForkNum(&bufHdr->tag) == forkNum &&
			bufHdr->tag.blockNum >= firstDelBlock)
		{
			if (LocalRefCount[i] != 0)
				elog(ERROR, "block %u of %s is still referenced (local %u)",
					 bufHdr->tag.blockNum,
					 relpathbackend(BufTagGetRelFileLocator(&bufHdr->tag),
									MyProcNumber,
									BufTagGetForkNum(&bufHdr->tag)).str,
					 LocalRefCount[i]);

			/* Remove entry from hashtable */
			hresult = (LocalBufferLookupEnt *)
				hash_search(LocalBufHash, &bufHdr->tag, HASH_REMOVE, NULL);
			if (!hresult)		/* shouldn't happen */
				elog(ERROR, "local buffer hash table corrupted");
			/* Mark buffer invalid */
			ClearBufferTag(&bufHdr->tag);
			buf_state &= ~BUF_FLAG_MASK;
			buf_state &= ~BUF_USAGECOUNT_MASK;
			pg_atomic_unlocked_write_u32(&bufHdr->state, buf_state);
		}
	}
}

/*
 * DropRelationAllLocalBuffers
 *		This function removes from the buffer pool all pages of all forks
 *		of the specified relation.
 *
 *		See DropRelationsAllBuffers in bufmgr.c for more notes.
 */
void
DropRelationAllLocalBuffers(RelFileLocator rlocator)
{
	int			i;

	for (i = 0; i < NLocBuffer; i++)
	{
		BufferDesc *bufHdr = GetLocalBufferDescriptor(i);
		LocalBufferLookupEnt *hresult;
		uint32		buf_state;

		buf_state = pg_atomic_read_u32(&bufHdr->state);

		if ((buf_state & BM_TAG_VALID) &&
			BufTagMatchesRelFileLocator(&bufHdr->tag, &rlocator))
		{
			if (LocalRefCount[i] != 0)
				elog(ERROR, "block %u of %s is still referenced (local %u)",
					 bufHdr->tag.blockNum,
					 relpathbackend(BufTagGetRelFileLocator(&bufHdr->tag),
									MyProcNumber,
									BufTagGetForkNum(&bufHdr->tag)).str,
					 LocalRefCount[i]);
			/* Remove entry from hashtable */
			hresult = (LocalBufferLookupEnt *)
				hash_search(LocalBufHash, &bufHdr->tag, HASH_REMOVE, NULL);
			if (!hresult)		/* shouldn't happen */
				elog(ERROR, "local buffer hash table corrupted");
			/* Mark buffer invalid */
			ClearBufferTag(&bufHdr->tag);
			buf_state &= ~BUF_FLAG_MASK;
			buf_state &= ~BUF_USAGECOUNT_MASK;
			pg_atomic_unlocked_write_u32(&bufHdr->state, buf_state);
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

	/*
	 * Parallel workers can't access data in temporary tables, because they
	 * have no visibility into the local buffers of their leader.  This is a
	 * convenient, low-cost place to provide a backstop check for that.  Note
	 * that we don't wish to prevent a parallel worker from accessing catalog
	 * metadata about a temp table, so checks at higher levels would be
	 * inappropriate.
	 */
	if (IsParallelWorker())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot access temporary tables during a parallel operation")));

	/* Allocate and zero buffer headers and auxiliary arrays */
	LocalBufferDescriptors = (BufferDesc *) calloc(nbufs, sizeof(BufferDesc));
	LocalBufferBlockPointers = (Block *) calloc(nbufs, sizeof(Block));
	LocalRefCount = (int32 *) calloc(nbufs, sizeof(int32));
	if (!LocalBufferDescriptors || !LocalBufferBlockPointers || !LocalRefCount)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	nextFreeLocalBufId = 0;

	/* initialize fields that need to start off nonzero */
	for (i = 0; i < nbufs; i++)
	{
		BufferDesc *buf = GetLocalBufferDescriptor(i);

		/*
		 * negative to indicate local buffer. This is tricky: shared buffers
		 * start with 0. We have to start with -2. (Note that the routine
		 * BufferDescriptorGetBuffer adds 1 to buf_id so our first buffer id
		 * is -1.)
		 */
		buf->buf_id = -i - 2;

		/*
		 * Intentionally do not initialize the buffer's atomic variable
		 * (besides zeroing the underlying memory above). That way we get
		 * errors on platforms without atomics, if somebody (re-)introduces
		 * atomic operations for local buffers.
		 */
	}

	/* Create the lookup hash table */
	info.keysize = sizeof(BufferTag);
	info.entrysize = sizeof(LocalBufferLookupEnt);

	LocalBufHash = hash_create("Local Buffer Lookup Table",
							   nbufs,
							   &info,
							   HASH_ELEM | HASH_BLOBS);

	if (!LocalBufHash)
		elog(ERROR, "could not initialize local buffer hash table");

	/* Initialization done, mark buffers allocated */
	NLocBuffer = nbufs;
}

/*
 * XXX: We could have a slightly more efficient version of PinLocalBuffer()
 * that does not support adjusting the usagecount - but so far it does not
 * seem worth the trouble.
 *
 * Note that ResourceOwnerEnlarge() must have been done already.
 */
bool
PinLocalBuffer(BufferDesc *buf_hdr, bool adjust_usagecount)
{
	uint32		buf_state;
	Buffer		buffer = BufferDescriptorGetBuffer(buf_hdr);
	int			bufid = -buffer - 1;

	buf_state = pg_atomic_read_u32(&buf_hdr->state);

	if (LocalRefCount[bufid] == 0)
	{
		NLocalPinnedBuffers++;
		if (adjust_usagecount &&
			BUF_STATE_GET_USAGECOUNT(buf_state) < BM_MAX_USAGE_COUNT)
		{
			buf_state += BUF_USAGECOUNT_ONE;
			pg_atomic_unlocked_write_u32(&buf_hdr->state, buf_state);
		}
	}
	LocalRefCount[bufid]++;
	ResourceOwnerRememberBuffer(CurrentResourceOwner,
								BufferDescriptorGetBuffer(buf_hdr));

	return buf_state & BM_VALID;
}

void
UnpinLocalBuffer(Buffer buffer)
{
	UnpinLocalBufferNoOwner(buffer);
	ResourceOwnerForgetBuffer(CurrentResourceOwner, buffer);
}

void
UnpinLocalBufferNoOwner(Buffer buffer)
{
	int			buffid = -buffer - 1;

	Assert(BufferIsLocal(buffer));
	Assert(LocalRefCount[buffid] > 0);
	Assert(NLocalPinnedBuffers > 0);

	if (--LocalRefCount[buffid] == 0)
		NLocalPinnedBuffers--;
}

/*
 * GUC check_hook for temp_buffers
 */
bool
check_temp_buffers(int *newval, void **extra, GucSource source)
{
	/*
	 * Once local buffers have been initialized, it's too late to change this.
	 * However, if this is only a test call, allow it.
	 */
	if (source != PGC_S_TEST && NLocBuffer && NLocBuffer != *newval)
	{
		GUC_check_errdetail("\"temp_buffers\" cannot be changed after any temporary tables have been accessed in the session.");
		return false;
	}
	return true;
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
		 * output.  Create the context on first use.
		 */
		if (LocalBufferContext == NULL)
			LocalBufferContext =
				AllocSetContextCreate(TopMemoryContext,
									  "LocalBufferContext",
									  ALLOCSET_DEFAULT_SIZES);

		/* Start with a 16-buffer request; subsequent ones double each time */
		num_bufs = Max(num_bufs_in_block * 2, 16);
		/* But not more than what we need for all remaining local bufs */
		num_bufs = Min(num_bufs, NLocBuffer - total_bufs_allocated);
		/* And don't overflow MaxAllocSize, either */
		num_bufs = Min(num_bufs, MaxAllocSize / BLCKSZ);

		/* Buffers should be I/O aligned. */
		cur_block = (char *)
			TYPEALIGN(PG_IO_ALIGN_SIZE,
					  MemoryContextAlloc(LocalBufferContext,
										 num_bufs * BLCKSZ + PG_IO_ALIGN_SIZE));
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
 * CheckForLocalBufferLeaks - ensure this backend holds no local buffer pins
 *
 * This is just like CheckForBufferLeaks(), but for local buffers.
 */
static void
CheckForLocalBufferLeaks(void)
{
#ifdef USE_ASSERT_CHECKING
	if (LocalRefCount)
	{
		int			RefCountErrors = 0;
		int			i;

		for (i = 0; i < NLocBuffer; i++)
		{
			if (LocalRefCount[i] != 0)
			{
				Buffer		b = -i - 1;
				char	   *s;

				s = DebugPrintBufferRefcount(b);
				elog(WARNING, "local buffer refcount leak: %s", s);
				pfree(s);

				RefCountErrors++;
			}
		}
		Assert(RefCountErrors == 0);
	}
#endif
}

/*
 * AtEOXact_LocalBuffers - clean up at end of transaction.
 *
 * This is just like AtEOXact_Buffers, but for local buffers.
 */
void
AtEOXact_LocalBuffers(bool isCommit)
{
	CheckForLocalBufferLeaks();
}

/*
 * AtProcExit_LocalBuffers - ensure we have dropped pins during backend exit.
 *
 * This is just like AtProcExit_Buffers, but for local buffers.
 */
void
AtProcExit_LocalBuffers(void)
{
	/*
	 * We shouldn't be holding any remaining pins; if we are, and assertions
	 * aren't enabled, we'll fail later in DropRelationBuffers while trying to
	 * drop the temp rels.
	 */
	CheckForLocalBufferLeaks();
}
