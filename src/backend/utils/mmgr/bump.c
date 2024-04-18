/*-------------------------------------------------------------------------
 *
 * bump.c
 *	  Bump allocator definitions.
 *
 * Bump is a MemoryContext implementation designed for memory usages which
 * require allocating a large number of chunks, none of which ever need to be
 * pfree'd or realloc'd.  Chunks allocated by this context have no chunk header
 * and operations which ordinarily require looking at the chunk header cannot
 * be performed.  For example, pfree, realloc, GetMemoryChunkSpace and
 * GetMemoryChunkContext are all not possible with bump allocated chunks.  The
 * only way to release memory allocated by this context type is to reset or
 * delete the context.
 *
 * Portions Copyright (c) 2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/mmgr/bump.c
 *
 *
 *	Bump is best suited to cases which require a large number of short-lived
 *	chunks where performance matters.  Because bump allocated chunks don't
 *	have a chunk header, it can fit more chunks on each block.  This means we
 *	can do more with less memory and fewer cache lines.  The reason it's best
 *	suited for short-lived usages of memory is that ideally, pointers to bump
 *	allocated chunks won't be visible to a large amount of code.  The more
 *	code that operates on memory allocated by this allocator, the more chances
 *	that some code will try to perform a pfree or one of the other operations
 *	which are made impossible due to the lack of chunk header.  In order to
 *	detect accidental usage of the various disallowed operations, we do add a
 *	MemoryChunk chunk header in MEMORY_CONTEXT_CHECKING builds and have the
 *	various disallowed functions raise an ERROR.
 *
 *	Allocations are MAXALIGNed.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "lib/ilist.h"
#include "port/pg_bitutils.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"
#include "utils/memutils_memorychunk.h"
#include "utils/memutils_internal.h"

#define Bump_BLOCKHDRSZ	MAXALIGN(sizeof(BumpBlock))

/* No chunk header unless built with MEMORY_CONTEXT_CHECKING */
#ifdef MEMORY_CONTEXT_CHECKING
#define Bump_CHUNKHDRSZ	sizeof(MemoryChunk)
#else
#define Bump_CHUNKHDRSZ	0
#endif

#define Bump_CHUNK_FRACTION	8

/* The keeper block is allocated in the same allocation as the set */
#define KeeperBlock(set) ((BumpBlock *) ((char *) (set) + \
			MAXALIGN(sizeof(BumpContext))))
#define IsKeeperBlock(set, blk) (KeeperBlock(set) == (blk))

typedef struct BumpBlock BumpBlock; /* forward reference */

typedef struct BumpContext
{
	MemoryContextData header;	/* Standard memory-context fields */

	/* Bump context parameters */
	uint32		initBlockSize;	/* initial block size */
	uint32		maxBlockSize;	/* maximum block size */
	uint32		nextBlockSize;	/* next block size to allocate */
	uint32		allocChunkLimit;	/* effective chunk size limit */

	dlist_head	blocks;			/* list of blocks with the block currently
								 * being filled at the head */
} BumpContext;

/*
 * BumpBlock
 *		BumpBlock is the unit of memory that is obtained by bump.c from
 *		malloc().  It contains zero or more allocations, which are the
 *		units requested by palloc().
 */
struct BumpBlock
{
	dlist_node	node;			/* doubly-linked list of blocks */
#ifdef MEMORY_CONTEXT_CHECKING
	BumpContext *context;		/* pointer back to the owning context */
#endif
	char	   *freeptr;		/* start of free space in this block */
	char	   *endptr;			/* end of space in this block */
};

/*
 * BumpIsValid
 *		True iff set is valid bump context.
 */
#define BumpIsValid(set) \
	(PointerIsValid(set) && IsA(set, BumpContext))

/*
 * We always store external chunks on a dedicated block.  This makes fetching
 * the block from an external chunk easy since it's always the first and only
 * chunk on the block.
 */
#define ExternalChunkGetBlock(chunk) \
	(BumpBlock *) ((char *) chunk - Bump_BLOCKHDRSZ)

/* Inlined helper functions */
static inline void BumpBlockInit(BumpContext *context, BumpBlock *block,
								 Size blksize);
static inline bool BumpBlockIsEmpty(BumpBlock *block);
static inline void BumpBlockMarkEmpty(BumpBlock *block);
static inline Size BumpBlockFreeBytes(BumpBlock *block);
static inline void BumpBlockFree(BumpContext *set, BumpBlock *block);


/*
* BumpContextCreate
*		Create a new Bump context.
*
* parent: parent context, or NULL if top-level context
* name: name of context (must be statically allocated)
* minContextSize: minimum context size
* initBlockSize: initial allocation block size
* maxBlockSize: maximum allocation block size
*/
MemoryContext
BumpContextCreate(MemoryContext parent, const char *name, Size minContextSize,
				  Size initBlockSize, Size maxBlockSize)
{
	Size		firstBlockSize;
	Size		allocSize;
	BumpContext *set;
	BumpBlock  *block;

	/* ensure MemoryChunk's size is properly maxaligned */
	StaticAssertDecl(Bump_CHUNKHDRSZ == MAXALIGN(Bump_CHUNKHDRSZ),
					 "sizeof(MemoryChunk) is not maxaligned");

	/*
	 * First, validate allocation parameters.  Asserts seem sufficient because
	 * nobody varies their parameters at runtime.  We somewhat arbitrarily
	 * enforce a minimum 1K block size.  We restrict the maximum block size to
	 * MEMORYCHUNK_MAX_BLOCKOFFSET as MemoryChunks are limited to this in
	 * regards to addressing the offset between the chunk and the block that
	 * the chunk is stored on.  We would be unable to store the offset between
	 * the chunk and block for any chunks that were beyond
	 * MEMORYCHUNK_MAX_BLOCKOFFSET bytes into the block if the block was to be
	 * larger than this.
	 */
	Assert(initBlockSize == MAXALIGN(initBlockSize) &&
		   initBlockSize >= 1024);
	Assert(maxBlockSize == MAXALIGN(maxBlockSize) &&
		   maxBlockSize >= initBlockSize &&
		   AllocHugeSizeIsValid(maxBlockSize)); /* must be safe to double */
	Assert(minContextSize == 0 ||
		   (minContextSize == MAXALIGN(minContextSize) &&
			minContextSize >= 1024 &&
			minContextSize <= maxBlockSize));
	Assert(maxBlockSize <= MEMORYCHUNK_MAX_BLOCKOFFSET);

	/* Determine size of initial block */
	allocSize = MAXALIGN(sizeof(BumpContext)) + Bump_BLOCKHDRSZ +
		Bump_CHUNKHDRSZ;
	if (minContextSize != 0)
		allocSize = Max(allocSize, minContextSize);
	else
		allocSize = Max(allocSize, initBlockSize);

	/*
	 * Allocate the initial block.  Unlike other bump.c blocks, it starts with
	 * the context header and its block header follows that.
	 */
	set = (BumpContext *) malloc(allocSize);
	if (set == NULL)
	{
		MemoryContextStats(TopMemoryContext);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while creating memory context \"%s\".",
						   name)));
	}

	/*
	 * Avoid writing code that can fail between here and MemoryContextCreate;
	 * we'd leak the header and initial block if we ereport in this stretch.
	 */
	dlist_init(&set->blocks);

	/* Fill in the initial block's block header */
	block = KeeperBlock(set);
	/* determine the block size and initialize it */
	firstBlockSize = allocSize - MAXALIGN(sizeof(BumpContext));
	BumpBlockInit(set, block, firstBlockSize);

	/* add it to the doubly-linked list of blocks */
	dlist_push_head(&set->blocks, &block->node);

	/*
	 * Fill in BumpContext-specific header fields.  The Asserts above should
	 * ensure that these all fit inside a uint32.
	 */
	set->initBlockSize = (uint32) initBlockSize;
	set->maxBlockSize = (uint32) maxBlockSize;
	set->nextBlockSize = (uint32) initBlockSize;

	/*
	 * Compute the allocation chunk size limit for this context.
	 *
	 * Limit the maximum size a non-dedicated chunk can be so that we can fit
	 * at least Bump_CHUNK_FRACTION of chunks this big onto the maximum sized
	 * block.  We must further limit this value so that it's no more than
	 * MEMORYCHUNK_MAX_VALUE.  We're unable to have non-external chunks larger
	 * than that value as we store the chunk size in the MemoryChunk 'value'
	 * field in the call to MemoryChunkSetHdrMask().
	 */
	set->allocChunkLimit = Min(maxBlockSize, MEMORYCHUNK_MAX_VALUE);
	while ((Size) (set->allocChunkLimit + Bump_CHUNKHDRSZ) >
		   (Size) ((Size) (maxBlockSize - Bump_BLOCKHDRSZ) / Bump_CHUNK_FRACTION))
		set->allocChunkLimit >>= 1;

	/* Finally, do the type-independent part of context creation */
	MemoryContextCreate((MemoryContext) set, T_BumpContext, MCTX_BUMP_ID,
						parent, name);

	((MemoryContext) set)->mem_allocated = allocSize;

	return (MemoryContext) set;
}

/*
 * BumpReset
 *		Frees all memory which is allocated in the given set.
 *
 * The code simply frees all the blocks in the context apart from the keeper
 * block.
 */
void
BumpReset(MemoryContext context)
{
	BumpContext *set = (BumpContext *) context;
	dlist_mutable_iter miter;

	Assert(BumpIsValid(set));

#ifdef MEMORY_CONTEXT_CHECKING
	/* Check for corruption and leaks before freeing */
	BumpCheck(context);
#endif

	dlist_foreach_modify(miter, &set->blocks)
	{
		BumpBlock  *block = dlist_container(BumpBlock, node, miter.cur);

		if (IsKeeperBlock(set, block))
			BumpBlockMarkEmpty(block);
		else
			BumpBlockFree(set, block);
	}

	/* Reset block size allocation sequence, too */
	set->nextBlockSize = set->initBlockSize;

	/* Ensure there is only 1 item in the dlist */
	Assert(!dlist_is_empty(&set->blocks));
	Assert(!dlist_has_next(&set->blocks, dlist_head_node(&set->blocks)));
}

/*
 * BumpDelete
 *		Free all memory which is allocated in the given context.
 */
void
BumpDelete(MemoryContext context)
{
	/* Reset to release all releasable BumpBlocks */
	BumpReset(context);
	/* And free the context header and keeper block */
	free(context);
}

/*
 * Helper for BumpAlloc() that allocates an entire block for the chunk.
 *
 * BumpAlloc()'s comment explains why this is separate.
 */
pg_noinline
static void *
BumpAllocLarge(MemoryContext context, Size size, int flags)
{
	BumpContext *set = (BumpContext *) context;
	BumpBlock  *block;
#ifdef MEMORY_CONTEXT_CHECKING
	MemoryChunk *chunk;
#endif
	Size		chunk_size;
	Size		required_size;
	Size		blksize;

	/* validate 'size' is within the limits for the given 'flags' */
	MemoryContextCheckSize(context, size, flags);

#ifdef MEMORY_CONTEXT_CHECKING
	/* ensure there's always space for the sentinel byte */
	chunk_size = MAXALIGN(size + 1);
#else
	chunk_size = MAXALIGN(size);
#endif

	required_size = chunk_size + Bump_CHUNKHDRSZ;
	blksize = required_size + Bump_BLOCKHDRSZ;

	block = (BumpBlock *) malloc(blksize);
	if (block == NULL)
		return NULL;

	context->mem_allocated += blksize;

	/* the block is completely full */
	block->freeptr = block->endptr = ((char *) block) + blksize;

#ifdef MEMORY_CONTEXT_CHECKING
	/* block with a single (used) chunk */
	block->context = set;

	chunk = (MemoryChunk *) (((char *) block) + Bump_BLOCKHDRSZ);

	/* mark the MemoryChunk as externally managed */
	MemoryChunkSetHdrMaskExternal(chunk, MCTX_BUMP_ID);

	chunk->requested_size = size;
	/* set mark to catch clobber of "unused" space */
	Assert(size < chunk_size);
	set_sentinel(MemoryChunkGetPointer(chunk), size);
#endif
#ifdef RANDOMIZE_ALLOCATED_MEMORY
	/* fill the allocated space with junk */
	randomize_mem((char *) MemoryChunkGetPointer(chunk), size);
#endif

	/*
	 * Add the block to the tail of allocated blocks list.  The current block
	 * is left at the head of the list as it may still have space for
	 * non-large allocations.
	 */
	dlist_push_tail(&set->blocks, &block->node);

#ifdef MEMORY_CONTEXT_CHECKING
	/* Ensure any padding bytes are marked NOACCESS. */
	VALGRIND_MAKE_MEM_NOACCESS((char *) MemoryChunkGetPointer(chunk) + size,
							   chunk_size - size);

	/* Disallow access to the chunk header. */
	VALGRIND_MAKE_MEM_NOACCESS(chunk, Bump_CHUNKHDRSZ);

	return MemoryChunkGetPointer(chunk);
#else
	return (void *) (((char *) block) + Bump_BLOCKHDRSZ);
#endif
}

/*
 * Small helper for allocating a new chunk from a chunk, to avoid duplicating
 * the code between BumpAlloc() and BumpAllocFromNewBlock().
 */
static inline void *
BumpAllocChunkFromBlock(MemoryContext context, BumpBlock *block, Size size,
						Size chunk_size)
{
#ifdef MEMORY_CONTEXT_CHECKING
	MemoryChunk *chunk;
#else
	void	   *ptr;
#endif

	/* validate we've been given a block with enough free space */
	Assert(block != NULL);
	Assert((block->endptr - block->freeptr) >= Bump_CHUNKHDRSZ + chunk_size);

#ifdef MEMORY_CONTEXT_CHECKING
	chunk = (MemoryChunk *) block->freeptr;
#else
	ptr = (void *) block->freeptr;
#endif

	/* point the freeptr beyond this chunk */
	block->freeptr += (Bump_CHUNKHDRSZ + chunk_size);
	Assert(block->freeptr <= block->endptr);

#ifdef MEMORY_CONTEXT_CHECKING
	/* Prepare to initialize the chunk header. */
	VALGRIND_MAKE_MEM_UNDEFINED(chunk, Bump_CHUNKHDRSZ);

	MemoryChunkSetHdrMask(chunk, block, chunk_size, MCTX_BUMP_ID);
	chunk->requested_size = size;
	/* set mark to catch clobber of "unused" space */
	Assert(size < chunk_size);
	set_sentinel(MemoryChunkGetPointer(chunk), size);

#ifdef RANDOMIZE_ALLOCATED_MEMORY
	/* fill the allocated space with junk */
	randomize_mem((char *) MemoryChunkGetPointer(chunk), size);
#endif

	/* Ensure any padding bytes are marked NOACCESS. */
	VALGRIND_MAKE_MEM_NOACCESS((char *) MemoryChunkGetPointer(chunk) + size,
							   chunk_size - size);

	/* Disallow access to the chunk header. */
	VALGRIND_MAKE_MEM_NOACCESS(chunk, Bump_CHUNKHDRSZ);

	return MemoryChunkGetPointer(chunk);
#else
	return ptr;
#endif							/* MEMORY_CONTEXT_CHECKING */
}

/*
 * Helper for BumpAlloc() that allocates a new block and returns a chunk
 * allocated from it.
 *
 * BumpAlloc()'s comment explains why this is separate.
 */
pg_noinline
static void *
BumpAllocFromNewBlock(MemoryContext context, Size size, int flags,
					  Size chunk_size)
{
	BumpContext *set = (BumpContext *) context;
	BumpBlock  *block;
	Size		blksize;
	Size		required_size;

	/*
	 * The first such block has size initBlockSize, and we double the space in
	 * each succeeding block, but not more than maxBlockSize.
	 */
	blksize = set->nextBlockSize;
	set->nextBlockSize <<= 1;
	if (set->nextBlockSize > set->maxBlockSize)
		set->nextBlockSize = set->maxBlockSize;

	/* we'll need space for the chunk, chunk hdr and block hdr */
	required_size = chunk_size + Bump_CHUNKHDRSZ + Bump_BLOCKHDRSZ;
	/* round the size up to the next power of 2 */
	if (blksize < required_size)
		blksize = pg_nextpower2_size_t(required_size);

	block = (BumpBlock *) malloc(blksize);

	if (block == NULL)
		return MemoryContextAllocationFailure(context, size, flags);

	context->mem_allocated += blksize;

	/* initialize the new block */
	BumpBlockInit(set, block, blksize);

	/* add it to the doubly-linked list of blocks */
	dlist_push_head(&set->blocks, &block->node);

	return BumpAllocChunkFromBlock(context, block, size, chunk_size);
}

/*
 * BumpAlloc
 *		Returns a pointer to allocated memory of given size or raises an ERROR
 *		on allocation failure, or returns NULL when flags contains
 *		MCXT_ALLOC_NO_OOM.
 *
 * No request may exceed:
 *		MAXALIGN_DOWN(SIZE_MAX) - Bump_BLOCKHDRSZ - Bump_CHUNKHDRSZ
 * All callers use a much-lower limit.
 *
 *
 * Note: when using valgrind, it doesn't matter how the returned allocation
 * is marked, as mcxt.c will set it to UNDEFINED.
 * This function should only contain the most common code paths.  Everything
 * else should be in pg_noinline helper functions, thus avoiding the overhead
 * of creating a stack frame for the common cases.  Allocating memory is often
 * a bottleneck in many workloads, so avoiding stack frame setup is
 * worthwhile.  Helper functions should always directly return the newly
 * allocated memory so that we can just return that address directly as a tail
 * call.
 */
void *
BumpAlloc(MemoryContext context, Size size, int flags)
{
	BumpContext *set = (BumpContext *) context;
	BumpBlock  *block;
	Size		chunk_size;
	Size		required_size;

	Assert(BumpIsValid(set));

#ifdef MEMORY_CONTEXT_CHECKING
	/* ensure there's always space for the sentinel byte */
	chunk_size = MAXALIGN(size + 1);
#else
	chunk_size = MAXALIGN(size);
#endif

	/*
	 * If requested size exceeds maximum for chunks we hand the request off to
	 * BumpAllocLarge().
	 */
	if (chunk_size > set->allocChunkLimit)
		return BumpAllocLarge(context, size, flags);

	required_size = chunk_size + Bump_CHUNKHDRSZ;

	/*
	 * Not an oversized chunk.  We try to first make use of the latest block,
	 * but if there's not enough space in it we must allocate a new block.
	 */
	block = dlist_container(BumpBlock, node, dlist_head_node(&set->blocks));

	if (BumpBlockFreeBytes(block) < required_size)
		return BumpAllocFromNewBlock(context, size, flags, chunk_size);

	/* The current block has space, so just allocate chunk there. */
	return BumpAllocChunkFromBlock(context, block, size, chunk_size);
}

/*
 * BumpBlockInit
 *		Initializes 'block' assuming 'blksize'.  Does not update the context's
 *		mem_allocated field.
 */
static inline void
BumpBlockInit(BumpContext *context, BumpBlock *block, Size blksize)
{
#ifdef MEMORY_CONTEXT_CHECKING
	block->context = context;
#endif
	block->freeptr = ((char *) block) + Bump_BLOCKHDRSZ;
	block->endptr = ((char *) block) + blksize;

	/* Mark unallocated space NOACCESS. */
	VALGRIND_MAKE_MEM_NOACCESS(block->freeptr, blksize - Bump_BLOCKHDRSZ);
}

/*
 * BumpBlockIsEmpty
 *		Returns true iff 'block' contains no chunks
 */
static inline bool
BumpBlockIsEmpty(BumpBlock *block)
{
	/* it's empty if the freeptr has not moved */
	return (block->freeptr == ((char *) block + Bump_BLOCKHDRSZ));
}

/*
 * BumpBlockMarkEmpty
 *		Set a block as empty.  Does not free the block.
 */
static inline void
BumpBlockMarkEmpty(BumpBlock *block)
{
#if defined(USE_VALGRIND) || defined(CLOBBER_FREED_MEMORY)
	char	   *datastart = ((char *) block) + Bump_BLOCKHDRSZ;
#endif

#ifdef CLOBBER_FREED_MEMORY
	wipe_mem(datastart, block->freeptr - datastart);
#else
	/* wipe_mem() would have done this */
	VALGRIND_MAKE_MEM_NOACCESS(datastart, block->freeptr - datastart);
#endif

	/* Reset the block, but don't return it to malloc */
	block->freeptr = ((char *) block) + Bump_BLOCKHDRSZ;
}

/*
 * BumpBlockFreeBytes
 *		Returns the number of bytes free in 'block'
 */
static inline Size
BumpBlockFreeBytes(BumpBlock *block)
{
	return (block->endptr - block->freeptr);
}

/*
 * BumpBlockFree
 *		Remove 'block' from 'set' and release the memory consumed by it.
 */
static inline void
BumpBlockFree(BumpContext *set, BumpBlock *block)
{
	/* Make sure nobody tries to free the keeper block */
	Assert(!IsKeeperBlock(set, block));

	/* release the block from the list of blocks */
	dlist_delete(&block->node);

	((MemoryContext) set)->mem_allocated -= ((char *) block->endptr - (char *) block);

#ifdef CLOBBER_FREED_MEMORY
	wipe_mem(block, ((char *) block->endptr - (char *) block));
#endif

	free(block);
}

/*
 * BumpFree
 *		Unsupported.
 */
void
BumpFree(void *pointer)
{
	elog(ERROR, "%s is not supported by the bump memory allocator", "pfree");
}

/*
 * BumpRealloc
 *		Unsupported.
 */
void *
BumpRealloc(void *pointer, Size size, int flags)
{
	elog(ERROR, "%s is not supported by the bump memory allocator", "realloc");
	return NULL;				/* keep compiler quiet */
}

/*
 * BumpGetChunkContext
 *		Unsupported.
 */
MemoryContext
BumpGetChunkContext(void *pointer)
{
	elog(ERROR, "%s is not supported by the bump memory allocator", "GetMemoryChunkContext");
	return NULL;				/* keep compiler quiet */
}

/*
 * BumpGetChunkSpace
 *		Unsupported.
 */
Size
BumpGetChunkSpace(void *pointer)
{
	elog(ERROR, "%s is not supported by the bump memory allocator", "GetMemoryChunkSpace");
	return 0;					/* keep compiler quiet */
}

/*
 * BumpIsEmpty
 *		Is a BumpContext empty of any allocated space?
 */
bool
BumpIsEmpty(MemoryContext context)
{
	BumpContext *set = (BumpContext *) context;
	dlist_iter	iter;

	Assert(BumpIsValid(set));

	dlist_foreach(iter, &set->blocks)
	{
		BumpBlock  *block = dlist_container(BumpBlock, node, iter.cur);

		if (!BumpBlockIsEmpty(block))
			return false;
	}

	return true;
}

/*
 * BumpStats
 *		Compute stats about memory consumption of a Bump context.
 *
 * printfunc: if not NULL, pass a human-readable stats string to this.
 * passthru: pass this pointer through to printfunc.
 * totals: if not NULL, add stats about this context into *totals.
 * print_to_stderr: print stats to stderr if true, elog otherwise.
 */
void
BumpStats(MemoryContext context, MemoryStatsPrintFunc printfunc,
		  void *passthru, MemoryContextCounters *totals, bool print_to_stderr)
{
	BumpContext *set = (BumpContext *) context;
	Size		nblocks = 0;
	Size		totalspace = 0;
	Size		freespace = 0;
	dlist_iter	iter;

	Assert(BumpIsValid(set));

	dlist_foreach(iter, &set->blocks)
	{
		BumpBlock  *block = dlist_container(BumpBlock, node, iter.cur);

		nblocks++;
		totalspace += (block->endptr - (char *) block);
		freespace += (block->endptr - block->freeptr);
	}

	if (printfunc)
	{
		char		stats_string[200];

		snprintf(stats_string, sizeof(stats_string),
				 "%zu total in %zu blocks; %zu free; %zu used",
				 totalspace, nblocks, freespace, totalspace - freespace);
		printfunc(context, passthru, stats_string, print_to_stderr);
	}

	if (totals)
	{
		totals->nblocks += nblocks;
		totals->totalspace += totalspace;
		totals->freespace += freespace;
	}
}


#ifdef MEMORY_CONTEXT_CHECKING

/*
 * BumpCheck
 *		Walk through chunks and check consistency of memory.
 *
 * NOTE: report errors as WARNING, *not* ERROR or FATAL.  Otherwise you'll
 * find yourself in an infinite loop when trouble occurs, because this
 * routine will be entered again when elog cleanup tries to release memory!
 */
void
BumpCheck(MemoryContext context)
{
	BumpContext *bump = (BumpContext *) context;
	const char *name = context->name;
	dlist_iter	iter;
	Size		total_allocated = 0;

	/* walk all blocks in this context */
	dlist_foreach(iter, &bump->blocks)
	{
		BumpBlock  *block = dlist_container(BumpBlock, node, iter.cur);
		int			nchunks;
		char	   *ptr;
		bool		has_external_chunk = false;

		if (IsKeeperBlock(bump, block))
			total_allocated += block->endptr - (char *) bump;
		else
			total_allocated += block->endptr - (char *) block;

		/* check block belongs to the correct context */
		if (block->context != bump)
			elog(WARNING, "problem in Bump %s: bogus context link in block %p",
				 name, block);

		/* now walk through the chunks and count them */
		nchunks = 0;
		ptr = ((char *) block) + Bump_BLOCKHDRSZ;

		while (ptr < block->freeptr)
		{
			MemoryChunk *chunk = (MemoryChunk *) ptr;
			BumpBlock  *chunkblock;
			Size		chunksize;

			/* allow access to the chunk header */
			VALGRIND_MAKE_MEM_DEFINED(chunk, Bump_CHUNKHDRSZ);

			if (MemoryChunkIsExternal(chunk))
			{
				chunkblock = ExternalChunkGetBlock(chunk);
				chunksize = block->endptr - (char *) MemoryChunkGetPointer(chunk);
				has_external_chunk = true;
			}
			else
			{
				chunkblock = MemoryChunkGetBlock(chunk);
				chunksize = MemoryChunkGetValue(chunk);
			}

			/* move to the next chunk */
			ptr += (chunksize + Bump_CHUNKHDRSZ);

			nchunks += 1;

			/* chunks have both block and context pointers, so check both */
			if (chunkblock != block)
				elog(WARNING, "problem in Bump %s: bogus block link in block %p, chunk %p",
					 name, block, chunk);
		}

		if (has_external_chunk && nchunks > 1)
			elog(WARNING, "problem in Bump %s: external chunk on non-dedicated block %p",
				 name, block);

	}

	Assert(total_allocated == context->mem_allocated);
}

#endif							/* MEMORY_CONTEXT_CHECKING */
