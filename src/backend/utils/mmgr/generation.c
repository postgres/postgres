/*-------------------------------------------------------------------------
 *
 * generation.c
 *	  Generational allocator definitions.
 *
 * Generation is a custom MemoryContext implementation designed for cases of
 * chunks with similar lifespan.
 *
 * Portions Copyright (c) 2017-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/mmgr/generation.c
 *
 *
 *	This memory context is based on the assumption that the chunks are freed
 *	roughly in the same order as they were allocated (FIFO), or in groups with
 *	similar lifespan (generations - hence the name of the context). This is
 *	typical for various queue-like use cases, i.e. when tuples are constructed,
 *	processed and then thrown away.
 *
 *	The memory context uses a very simple approach to free space management.
 *	Instead of a complex global freelist, each block tracks a number
 *	of allocated and freed chunks.  The block is classed as empty when the
 *	number of free chunks is equal to the number of allocated chunks.  When
 *	this occurs, instead of freeing the block, we try to "recycle" it, i.e.
 *	reuse it for new allocations.  This is done by setting the block in the
 *	context's 'freeblock' field.  If the freeblock field is already occupied
 *	by another free block we simply return the newly empty block to malloc.
 *
 *	This approach to free blocks requires fewer malloc/free calls for truly
 *	first allocated, first free'd allocation patterns.
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


#define Generation_BLOCKHDRSZ	MAXALIGN(sizeof(GenerationBlock))
#define Generation_CHUNKHDRSZ	sizeof(MemoryChunk)

#define Generation_CHUNK_FRACTION	8

typedef struct GenerationBlock GenerationBlock; /* forward reference */

typedef void *GenerationPointer;

/*
 * GenerationContext is a simple memory context not reusing allocated chunks,
 * and freeing blocks once all chunks are freed.
 */
typedef struct GenerationContext
{
	MemoryContextData header;	/* Standard memory-context fields */

	/* Generational context parameters */
	Size		initBlockSize;	/* initial block size */
	Size		maxBlockSize;	/* maximum block size */
	Size		nextBlockSize;	/* next block size to allocate */
	Size		allocChunkLimit;	/* effective chunk size limit */

	GenerationBlock *block;		/* current (most recently allocated) block, or
								 * NULL if we've just freed the most recent
								 * block */
	GenerationBlock *freeblock; /* pointer to a block that's being recycled,
								 * or NULL if there's no such block. */
	GenerationBlock *keeper;	/* keep this block over resets */
	dlist_head	blocks;			/* list of blocks */
} GenerationContext;

/*
 * GenerationBlock
 *		GenerationBlock is the unit of memory that is obtained by generation.c
 *		from malloc().  It contains zero or more MemoryChunks, which are the
 *		units requested by palloc() and freed by pfree().  MemoryChunks cannot
 *		be returned to malloc() individually, instead pfree() updates the free
 *		counter of the block and when all chunks in a block are free the whole
 *		block can be returned to malloc().
 *
 *		GenerationBlock is the header data for a block --- the usable space
 *		within the block begins at the next alignment boundary.
 */
struct GenerationBlock
{
	dlist_node	node;			/* doubly-linked list of blocks */
	GenerationContext *context; /* pointer back to the owning context */
	Size		blksize;		/* allocated size of this block */
	int			nchunks;		/* number of chunks in the block */
	int			nfree;			/* number of free chunks */
	char	   *freeptr;		/* start of free space in this block */
	char	   *endptr;			/* end of space in this block */
};

/*
 * GenerationIsValid
 *		True iff set is valid generation set.
 */
#define GenerationIsValid(set) \
	(PointerIsValid(set) && IsA(set, GenerationContext))

/*
 * GenerationBlockIsValid
 *		True iff block is valid block of generation set.
 */
#define GenerationBlockIsValid(block) \
	(PointerIsValid(block) && GenerationIsValid((block)->context))

/*
 * We always store external chunks on a dedicated block.  This makes fetching
 * the block from an external chunk easy since it's always the first and only
 * chunk on the block.
 */
#define ExternalChunkGetBlock(chunk) \
	(GenerationBlock *) ((char *) chunk - Generation_BLOCKHDRSZ)

/* Inlined helper functions */
static inline void GenerationBlockInit(GenerationContext *context,
									   GenerationBlock *block,
									   Size blksize);
static inline bool GenerationBlockIsEmpty(GenerationBlock *block);
static inline void GenerationBlockMarkEmpty(GenerationBlock *block);
static inline Size GenerationBlockFreeBytes(GenerationBlock *block);
static inline void GenerationBlockFree(GenerationContext *set,
									   GenerationBlock *block);


/*
 * Public routines
 */


/*
 * GenerationContextCreate
 *		Create a new Generation context.
 *
 * parent: parent context, or NULL if top-level context
 * name: name of context (must be statically allocated)
 * minContextSize: minimum context size
 * initBlockSize: initial allocation block size
 * maxBlockSize: maximum allocation block size
 */
MemoryContext
GenerationContextCreate(MemoryContext parent,
						const char *name,
						Size minContextSize,
						Size initBlockSize,
						Size maxBlockSize)
{
	Size		firstBlockSize;
	Size		allocSize;
	GenerationContext *set;
	GenerationBlock *block;

	/* ensure MemoryChunk's size is properly maxaligned */
	StaticAssertDecl(Generation_CHUNKHDRSZ == MAXALIGN(Generation_CHUNKHDRSZ),
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
	allocSize = MAXALIGN(sizeof(GenerationContext)) +
		Generation_BLOCKHDRSZ + Generation_CHUNKHDRSZ;
	if (minContextSize != 0)
		allocSize = Max(allocSize, minContextSize);
	else
		allocSize = Max(allocSize, initBlockSize);

	/*
	 * Allocate the initial block.  Unlike other generation.c blocks, it
	 * starts with the context header and its block header follows that.
	 */
	set = (GenerationContext *) malloc(allocSize);
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
	 * we'd leak the header if we ereport in this stretch.
	 */
	dlist_init(&set->blocks);

	/* Fill in the initial block's block header */
	block = (GenerationBlock *) (((char *) set) + MAXALIGN(sizeof(GenerationContext)));
	/* determine the block size and initialize it */
	firstBlockSize = allocSize - MAXALIGN(sizeof(GenerationContext));
	GenerationBlockInit(set, block, firstBlockSize);

	/* add it to the doubly-linked list of blocks */
	dlist_push_head(&set->blocks, &block->node);

	/* use it as the current allocation block */
	set->block = block;

	/* No free block, yet */
	set->freeblock = NULL;

	/* Mark block as not to be released at reset time */
	set->keeper = block;

	/* Fill in GenerationContext-specific header fields */
	set->initBlockSize = initBlockSize;
	set->maxBlockSize = maxBlockSize;
	set->nextBlockSize = initBlockSize;

	/*
	 * Compute the allocation chunk size limit for this context.
	 *
	 * Limit the maximum size a non-dedicated chunk can be so that we can fit
	 * at least Generation_CHUNK_FRACTION of chunks this big onto the maximum
	 * sized block.  We must further limit this value so that it's no more
	 * than MEMORYCHUNK_MAX_VALUE.  We're unable to have non-external chunks
	 * larger than that value as we store the chunk size in the MemoryChunk
	 * 'value' field in the call to MemoryChunkSetHdrMask().
	 */
	set->allocChunkLimit = Min(maxBlockSize, MEMORYCHUNK_MAX_VALUE);
	while ((Size) (set->allocChunkLimit + Generation_CHUNKHDRSZ) >
		   (Size) ((Size) (maxBlockSize - Generation_BLOCKHDRSZ) / Generation_CHUNK_FRACTION))
		set->allocChunkLimit >>= 1;

	/* Finally, do the type-independent part of context creation */
	MemoryContextCreate((MemoryContext) set,
						T_GenerationContext,
						MCTX_GENERATION_ID,
						parent,
						name);

	((MemoryContext) set)->mem_allocated = firstBlockSize;

	return (MemoryContext) set;
}

/*
 * GenerationReset
 *		Frees all memory which is allocated in the given set.
 *
 * The code simply frees all the blocks in the context - we don't keep any
 * keeper blocks or anything like that.
 */
void
GenerationReset(MemoryContext context)
{
	GenerationContext *set = (GenerationContext *) context;
	dlist_mutable_iter miter;

	Assert(GenerationIsValid(set));

#ifdef MEMORY_CONTEXT_CHECKING
	/* Check for corruption and leaks before freeing */
	GenerationCheck(context);
#endif

	/*
	 * NULLify the free block pointer.  We must do this before calling
	 * GenerationBlockFree as that function never expects to free the
	 * freeblock.
	 */
	set->freeblock = NULL;

	dlist_foreach_modify(miter, &set->blocks)
	{
		GenerationBlock *block = dlist_container(GenerationBlock, node, miter.cur);

		if (block == set->keeper)
			GenerationBlockMarkEmpty(block);
		else
			GenerationBlockFree(set, block);
	}

	/* set it so new allocations to make use of the keeper block */
	set->block = set->keeper;

	/* Reset block size allocation sequence, too */
	set->nextBlockSize = set->initBlockSize;

	/* Ensure there is only 1 item in the dlist */
	Assert(!dlist_is_empty(&set->blocks));
	Assert(!dlist_has_next(&set->blocks, dlist_head_node(&set->blocks)));
}

/*
 * GenerationDelete
 *		Free all memory which is allocated in the given context.
 */
void
GenerationDelete(MemoryContext context)
{
	/* Reset to release all releasable GenerationBlocks */
	GenerationReset(context);
	/* And free the context header and keeper block */
	free(context);
}

/*
 * GenerationAlloc
 *		Returns pointer to allocated memory of given size or NULL if
 *		request could not be completed; memory is added to the set.
 *
 * No request may exceed:
 *		MAXALIGN_DOWN(SIZE_MAX) - Generation_BLOCKHDRSZ - Generation_CHUNKHDRSZ
 * All callers use a much-lower limit.
 *
 * Note: when using valgrind, it doesn't matter how the returned allocation
 * is marked, as mcxt.c will set it to UNDEFINED.  In some paths we will
 * return space that is marked NOACCESS - GenerationRealloc has to beware!
 */
void *
GenerationAlloc(MemoryContext context, Size size)
{
	GenerationContext *set = (GenerationContext *) context;
	GenerationBlock *block;
	MemoryChunk *chunk;
	Size		chunk_size;
	Size		required_size;

	Assert(GenerationIsValid(set));

#ifdef MEMORY_CONTEXT_CHECKING
	/* ensure there's always space for the sentinel byte */
	chunk_size = MAXALIGN(size + 1);
#else
	chunk_size = MAXALIGN(size);
#endif
	required_size = chunk_size + Generation_CHUNKHDRSZ;

	/* is it an over-sized chunk? if yes, allocate special block */
	if (chunk_size > set->allocChunkLimit)
	{
		Size		blksize = required_size + Generation_BLOCKHDRSZ;

		block = (GenerationBlock *) malloc(blksize);
		if (block == NULL)
			return NULL;

		context->mem_allocated += blksize;

		/* block with a single (used) chunk */
		block->context = set;
		block->blksize = blksize;
		block->nchunks = 1;
		block->nfree = 0;

		/* the block is completely full */
		block->freeptr = block->endptr = ((char *) block) + blksize;

		chunk = (MemoryChunk *) (((char *) block) + Generation_BLOCKHDRSZ);

		/* mark the MemoryChunk as externally managed */
		MemoryChunkSetHdrMaskExternal(chunk, MCTX_GENERATION_ID);

#ifdef MEMORY_CONTEXT_CHECKING
		chunk->requested_size = size;
		/* set mark to catch clobber of "unused" space */
		Assert(size < chunk_size);
		set_sentinel(MemoryChunkGetPointer(chunk), size);
#endif
#ifdef RANDOMIZE_ALLOCATED_MEMORY
		/* fill the allocated space with junk */
		randomize_mem((char *) MemoryChunkGetPointer(chunk), size);
#endif

		/* add the block to the list of allocated blocks */
		dlist_push_head(&set->blocks, &block->node);

		/* Ensure any padding bytes are marked NOACCESS. */
		VALGRIND_MAKE_MEM_NOACCESS((char *) MemoryChunkGetPointer(chunk) + size,
								   chunk_size - size);

		/* Disallow access to the chunk header. */
		VALGRIND_MAKE_MEM_NOACCESS(chunk, Generation_CHUNKHDRSZ);

		return MemoryChunkGetPointer(chunk);
	}

	/*
	 * Not an oversized chunk.  We try to first make use of the current block,
	 * but if there's not enough space in it, instead of allocating a new
	 * block, we look to see if the freeblock is empty and has enough space.
	 * If not, we'll also try the same using the keeper block.  The keeper
	 * block may have become empty and we have no other way to reuse it again
	 * if we don't try to use it explicitly here.
	 *
	 * We don't want to start filling the freeblock before the current block
	 * is full, otherwise we may cause fragmentation in FIFO type workloads.
	 * We only switch to using the freeblock or keeper block if those blocks
	 * are completely empty.  If we didn't do that we could end up fragmenting
	 * consecutive allocations over multiple blocks which would be a problem
	 * that would compound over time.
	 */
	block = set->block;

	if (block == NULL ||
		GenerationBlockFreeBytes(block) < required_size)
	{
		Size		blksize;
		GenerationBlock *freeblock = set->freeblock;

		if (freeblock != NULL &&
			GenerationBlockIsEmpty(freeblock) &&
			GenerationBlockFreeBytes(freeblock) >= required_size)
		{
			block = freeblock;

			/*
			 * Zero out the freeblock as we'll set this to the current block
			 * below
			 */
			set->freeblock = NULL;
		}
		else if (GenerationBlockIsEmpty(set->keeper) &&
				 GenerationBlockFreeBytes(set->keeper) >= required_size)
		{
			block = set->keeper;
		}
		else
		{
			/*
			 * The first such block has size initBlockSize, and we double the
			 * space in each succeeding block, but not more than maxBlockSize.
			 */
			blksize = set->nextBlockSize;
			set->nextBlockSize <<= 1;
			if (set->nextBlockSize > set->maxBlockSize)
				set->nextBlockSize = set->maxBlockSize;

			/* we'll need a block hdr too, so add that to the required size */
			required_size += Generation_BLOCKHDRSZ;

			/* round the size up to the next power of 2 */
			if (blksize < required_size)
				blksize = pg_nextpower2_size_t(required_size);

			block = (GenerationBlock *) malloc(blksize);

			if (block == NULL)
				return NULL;

			context->mem_allocated += blksize;

			/* initialize the new block */
			GenerationBlockInit(set, block, blksize);

			/* add it to the doubly-linked list of blocks */
			dlist_push_head(&set->blocks, &block->node);

			/* Zero out the freeblock in case it's become full */
			set->freeblock = NULL;
		}

		/* and also use it as the current allocation block */
		set->block = block;
	}

	/* we're supposed to have a block with enough free space now */
	Assert(block != NULL);
	Assert((block->endptr - block->freeptr) >= Generation_CHUNKHDRSZ + chunk_size);

	chunk = (MemoryChunk *) block->freeptr;

	/* Prepare to initialize the chunk header. */
	VALGRIND_MAKE_MEM_UNDEFINED(chunk, Generation_CHUNKHDRSZ);

	block->nchunks += 1;
	block->freeptr += (Generation_CHUNKHDRSZ + chunk_size);

	Assert(block->freeptr <= block->endptr);

	MemoryChunkSetHdrMask(chunk, block, chunk_size, MCTX_GENERATION_ID);
#ifdef MEMORY_CONTEXT_CHECKING
	chunk->requested_size = size;
	/* set mark to catch clobber of "unused" space */
	Assert(size < chunk_size);
	set_sentinel(MemoryChunkGetPointer(chunk), size);
#endif
#ifdef RANDOMIZE_ALLOCATED_MEMORY
	/* fill the allocated space with junk */
	randomize_mem((char *) MemoryChunkGetPointer(chunk), size);
#endif

	/* Ensure any padding bytes are marked NOACCESS. */
	VALGRIND_MAKE_MEM_NOACCESS((char *) MemoryChunkGetPointer(chunk) + size,
							   chunk_size - size);

	/* Disallow access to the chunk header. */
	VALGRIND_MAKE_MEM_NOACCESS(chunk, Generation_CHUNKHDRSZ);

	return MemoryChunkGetPointer(chunk);
}

/*
 * GenerationBlockInit
 *		Initializes 'block' assuming 'blksize'.  Does not update the context's
 *		mem_allocated field.
 */
static inline void
GenerationBlockInit(GenerationContext *context, GenerationBlock *block,
					Size blksize)
{
	block->context = context;
	block->blksize = blksize;
	block->nchunks = 0;
	block->nfree = 0;

	block->freeptr = ((char *) block) + Generation_BLOCKHDRSZ;
	block->endptr = ((char *) block) + blksize;

	/* Mark unallocated space NOACCESS. */
	VALGRIND_MAKE_MEM_NOACCESS(block->freeptr,
							   blksize - Generation_BLOCKHDRSZ);
}

/*
 * GenerationBlockIsEmpty
 *		Returns true iff 'block' contains no chunks
 */
static inline bool
GenerationBlockIsEmpty(GenerationBlock *block)
{
	return (block->nchunks == 0);
}

/*
 * GenerationBlockMarkEmpty
 *		Set a block as empty.  Does not free the block.
 */
static inline void
GenerationBlockMarkEmpty(GenerationBlock *block)
{
#if defined(USE_VALGRIND) || defined(CLOBBER_FREED_MEMORY)
	char	   *datastart = ((char *) block) + Generation_BLOCKHDRSZ;
#endif

#ifdef CLOBBER_FREED_MEMORY
	wipe_mem(datastart, block->freeptr - datastart);
#else
	/* wipe_mem() would have done this */
	VALGRIND_MAKE_MEM_NOACCESS(datastart, block->freeptr - datastart);
#endif

	/* Reset the block, but don't return it to malloc */
	block->nchunks = 0;
	block->nfree = 0;
	block->freeptr = ((char *) block) + Generation_BLOCKHDRSZ;
}

/*
 * GenerationBlockFreeBytes
 *		Returns the number of bytes free in 'block'
 */
static inline Size
GenerationBlockFreeBytes(GenerationBlock *block)
{
	return (block->endptr - block->freeptr);
}

/*
 * GenerationBlockFree
 *		Remove 'block' from 'set' and release the memory consumed by it.
 */
static inline void
GenerationBlockFree(GenerationContext *set, GenerationBlock *block)
{
	/* Make sure nobody tries to free the keeper block */
	Assert(block != set->keeper);
	/* We shouldn't be freeing the freeblock either */
	Assert(block != set->freeblock);

	/* release the block from the list of blocks */
	dlist_delete(&block->node);

	((MemoryContext) set)->mem_allocated -= block->blksize;

#ifdef CLOBBER_FREED_MEMORY
	wipe_mem(block, block->blksize);
#endif

	free(block);
}

/*
 * GenerationFree
 *		Update number of chunks in the block, and if all chunks in the block
 *		are now free then discard the block.
 */
void
GenerationFree(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	GenerationBlock *block;
	GenerationContext *set;
#if (defined(MEMORY_CONTEXT_CHECKING) && defined(USE_ASSERT_CHECKING)) \
	|| defined(CLOBBER_FREED_MEMORY)
	Size		chunksize;
#endif

	/* Allow access to the chunk header. */
	VALGRIND_MAKE_MEM_DEFINED(chunk, Generation_CHUNKHDRSZ);

	if (MemoryChunkIsExternal(chunk))
	{
		block = ExternalChunkGetBlock(chunk);

		/*
		 * Try to verify that we have a sane block pointer: the block header
		 * should reference a generation context.
		 */
		if (!GenerationBlockIsValid(block))
			elog(ERROR, "could not find block containing chunk %p", chunk);

#if (defined(MEMORY_CONTEXT_CHECKING) && defined(USE_ASSERT_CHECKING)) \
	|| defined(CLOBBER_FREED_MEMORY)
		chunksize = block->endptr - (char *) pointer;
#endif
	}
	else
	{
		block = MemoryChunkGetBlock(chunk);

		/*
		 * In this path, for speed reasons we just Assert that the referenced
		 * block is good.  Future field experience may show that this Assert
		 * had better become a regular runtime test-and-elog check.
		 */
		Assert(GenerationBlockIsValid(block));

#if (defined(MEMORY_CONTEXT_CHECKING) && defined(USE_ASSERT_CHECKING)) \
	|| defined(CLOBBER_FREED_MEMORY)
		chunksize = MemoryChunkGetValue(chunk);
#endif
	}

#ifdef MEMORY_CONTEXT_CHECKING
	/* Test for someone scribbling on unused space in chunk */
	Assert(chunk->requested_size < chunksize);
	if (!sentinel_ok(pointer, chunk->requested_size))
		elog(WARNING, "detected write past chunk end in %s %p",
			 ((MemoryContext) block->context)->name, chunk);
#endif

#ifdef CLOBBER_FREED_MEMORY
	wipe_mem(pointer, chunksize);
#endif

#ifdef MEMORY_CONTEXT_CHECKING
	/* Reset requested_size to InvalidAllocSize in freed chunks */
	chunk->requested_size = InvalidAllocSize;
#endif

	block->nfree += 1;

	Assert(block->nchunks > 0);
	Assert(block->nfree <= block->nchunks);

	/* If there are still allocated chunks in the block, we're done. */
	if (block->nfree < block->nchunks)
		return;

	set = block->context;

	/* Don't try to free the keeper block, just mark it empty */
	if (block == set->keeper)
	{
		GenerationBlockMarkEmpty(block);
		return;
	}

	/*
	 * If there is no freeblock set or if this is the freeblock then instead
	 * of freeing this memory, we keep it around so that new allocations have
	 * the option of recycling it.
	 */
	if (set->freeblock == NULL || set->freeblock == block)
	{
		/* XXX should we only recycle maxBlockSize sized blocks? */
		set->freeblock = block;
		GenerationBlockMarkEmpty(block);
		return;
	}

	/* Also make sure the block is not marked as the current block. */
	if (set->block == block)
		set->block = NULL;

	/*
	 * The block is empty, so let's get rid of it. First remove it from the
	 * list of blocks, then return it to malloc().
	 */
	dlist_delete(&block->node);

	set->header.mem_allocated -= block->blksize;
	free(block);
}

/*
 * GenerationRealloc
 *		When handling repalloc, we simply allocate a new chunk, copy the data
 *		and discard the old one. The only exception is when the new size fits
 *		into the old chunk - in that case we just update chunk header.
 */
void *
GenerationRealloc(void *pointer, Size size)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	GenerationContext *set;
	GenerationBlock *block;
	GenerationPointer newPointer;
	Size		oldsize;

	/* Allow access to the chunk header. */
	VALGRIND_MAKE_MEM_DEFINED(chunk, Generation_CHUNKHDRSZ);

	if (MemoryChunkIsExternal(chunk))
	{
		block = ExternalChunkGetBlock(chunk);

		/*
		 * Try to verify that we have a sane block pointer: the block header
		 * should reference a generation context.
		 */
		if (!GenerationBlockIsValid(block))
			elog(ERROR, "could not find block containing chunk %p", chunk);

		oldsize = block->endptr - (char *) pointer;
	}
	else
	{
		block = MemoryChunkGetBlock(chunk);

		/*
		 * In this path, for speed reasons we just Assert that the referenced
		 * block is good.  Future field experience may show that this Assert
		 * had better become a regular runtime test-and-elog check.
		 */
		Assert(GenerationBlockIsValid(block));

		oldsize = MemoryChunkGetValue(chunk);
	}

	set = block->context;

#ifdef MEMORY_CONTEXT_CHECKING
	/* Test for someone scribbling on unused space in chunk */
	Assert(chunk->requested_size < oldsize);
	if (!sentinel_ok(pointer, chunk->requested_size))
		elog(WARNING, "detected write past chunk end in %s %p",
			 ((MemoryContext) set)->name, chunk);
#endif

	/*
	 * Maybe the allocated area already is >= the new size.  (In particular,
	 * we always fall out here if the requested size is a decrease.)
	 *
	 * This memory context does not use power-of-2 chunk sizing and instead
	 * carves the chunks to be as small as possible, so most repalloc() calls
	 * will end up in the palloc/memcpy/pfree branch.
	 *
	 * XXX Perhaps we should annotate this condition with unlikely()?
	 */
	if (oldsize >= size)
	{
#ifdef MEMORY_CONTEXT_CHECKING
		Size		oldrequest = chunk->requested_size;

#ifdef RANDOMIZE_ALLOCATED_MEMORY
		/* We can only fill the extra space if we know the prior request */
		if (size > oldrequest)
			randomize_mem((char *) pointer + oldrequest,
						  size - oldrequest);
#endif

		chunk->requested_size = size;

		/*
		 * If this is an increase, mark any newly-available part UNDEFINED.
		 * Otherwise, mark the obsolete part NOACCESS.
		 */
		if (size > oldrequest)
			VALGRIND_MAKE_MEM_UNDEFINED((char *) pointer + oldrequest,
										size - oldrequest);
		else
			VALGRIND_MAKE_MEM_NOACCESS((char *) pointer + size,
									   oldsize - size);

		/* set mark to catch clobber of "unused" space */
		set_sentinel(pointer, size);
#else							/* !MEMORY_CONTEXT_CHECKING */

		/*
		 * We don't have the information to determine whether we're growing
		 * the old request or shrinking it, so we conservatively mark the
		 * entire new allocation DEFINED.
		 */
		VALGRIND_MAKE_MEM_NOACCESS(pointer, oldsize);
		VALGRIND_MAKE_MEM_DEFINED(pointer, size);
#endif

		/* Disallow access to the chunk header. */
		VALGRIND_MAKE_MEM_NOACCESS(chunk, Generation_CHUNKHDRSZ);

		return pointer;
	}

	/* allocate new chunk */
	newPointer = GenerationAlloc((MemoryContext) set, size);

	/* leave immediately if request was not completed */
	if (newPointer == NULL)
	{
		/* Disallow access to the chunk header. */
		VALGRIND_MAKE_MEM_NOACCESS(chunk, Generation_CHUNKHDRSZ);
		return NULL;
	}

	/*
	 * GenerationAlloc() may have returned a region that is still NOACCESS.
	 * Change it to UNDEFINED for the moment; memcpy() will then transfer
	 * definedness from the old allocation to the new.  If we know the old
	 * allocation, copy just that much.  Otherwise, make the entire old chunk
	 * defined to avoid errors as we copy the currently-NOACCESS trailing
	 * bytes.
	 */
	VALGRIND_MAKE_MEM_UNDEFINED(newPointer, size);
#ifdef MEMORY_CONTEXT_CHECKING
	oldsize = chunk->requested_size;
#else
	VALGRIND_MAKE_MEM_DEFINED(pointer, oldsize);
#endif

	/* transfer existing data (certain to fit) */
	memcpy(newPointer, pointer, oldsize);

	/* free old chunk */
	GenerationFree(pointer);

	return newPointer;
}

/*
 * GenerationGetChunkContext
 *		Return the MemoryContext that 'pointer' belongs to.
 */
MemoryContext
GenerationGetChunkContext(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	GenerationBlock *block;

	/* Allow access to the chunk header. */
	VALGRIND_MAKE_MEM_DEFINED(chunk, Generation_CHUNKHDRSZ);

	if (MemoryChunkIsExternal(chunk))
		block = ExternalChunkGetBlock(chunk);
	else
		block = (GenerationBlock *) MemoryChunkGetBlock(chunk);

	/* Disallow access to the chunk header. */
	VALGRIND_MAKE_MEM_NOACCESS(chunk, Generation_CHUNKHDRSZ);

	Assert(GenerationBlockIsValid(block));
	return &block->context->header;
}

/*
 * GenerationGetChunkSpace
 *		Given a currently-allocated chunk, determine the total space
 *		it occupies (including all memory-allocation overhead).
 */
Size
GenerationGetChunkSpace(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	Size		chunksize;

	/* Allow access to the chunk header. */
	VALGRIND_MAKE_MEM_DEFINED(chunk, Generation_CHUNKHDRSZ);

	if (MemoryChunkIsExternal(chunk))
	{
		GenerationBlock *block = ExternalChunkGetBlock(chunk);

		Assert(GenerationBlockIsValid(block));
		chunksize = block->endptr - (char *) pointer;
	}
	else
		chunksize = MemoryChunkGetValue(chunk);

	/* Disallow access to the chunk header. */
	VALGRIND_MAKE_MEM_NOACCESS(chunk, Generation_CHUNKHDRSZ);

	return Generation_CHUNKHDRSZ + chunksize;
}

/*
 * GenerationIsEmpty
 *		Is a GenerationContext empty of any allocated space?
 */
bool
GenerationIsEmpty(MemoryContext context)
{
	GenerationContext *set = (GenerationContext *) context;
	dlist_iter	iter;

	Assert(GenerationIsValid(set));

	dlist_foreach(iter, &set->blocks)
	{
		GenerationBlock *block = dlist_container(GenerationBlock, node, iter.cur);

		if (block->nchunks > 0)
			return false;
	}

	return true;
}

/*
 * GenerationStats
 *		Compute stats about memory consumption of a Generation context.
 *
 * printfunc: if not NULL, pass a human-readable stats string to this.
 * passthru: pass this pointer through to printfunc.
 * totals: if not NULL, add stats about this context into *totals.
 * print_to_stderr: print stats to stderr if true, elog otherwise.
 *
 * XXX freespace only accounts for empty space at the end of the block, not
 * space of freed chunks (which is unknown).
 */
void
GenerationStats(MemoryContext context,
				MemoryStatsPrintFunc printfunc, void *passthru,
				MemoryContextCounters *totals, bool print_to_stderr)
{
	GenerationContext *set = (GenerationContext *) context;
	Size		nblocks = 0;
	Size		nchunks = 0;
	Size		nfreechunks = 0;
	Size		totalspace;
	Size		freespace = 0;
	dlist_iter	iter;

	Assert(GenerationIsValid(set));

	/* Include context header in totalspace */
	totalspace = MAXALIGN(sizeof(GenerationContext));

	dlist_foreach(iter, &set->blocks)
	{
		GenerationBlock *block = dlist_container(GenerationBlock, node, iter.cur);

		nblocks++;
		nchunks += block->nchunks;
		nfreechunks += block->nfree;
		totalspace += block->blksize;
		freespace += (block->endptr - block->freeptr);
	}

	if (printfunc)
	{
		char		stats_string[200];

		snprintf(stats_string, sizeof(stats_string),
				 "%zu total in %zu blocks (%zu chunks); %zu free (%zu chunks); %zu used",
				 totalspace, nblocks, nchunks, freespace,
				 nfreechunks, totalspace - freespace);
		printfunc(context, passthru, stats_string, print_to_stderr);
	}

	if (totals)
	{
		totals->nblocks += nblocks;
		totals->freechunks += nfreechunks;
		totals->totalspace += totalspace;
		totals->freespace += freespace;
	}
}


#ifdef MEMORY_CONTEXT_CHECKING

/*
 * GenerationCheck
 *		Walk through chunks and check consistency of memory.
 *
 * NOTE: report errors as WARNING, *not* ERROR or FATAL.  Otherwise you'll
 * find yourself in an infinite loop when trouble occurs, because this
 * routine will be entered again when elog cleanup tries to release memory!
 */
void
GenerationCheck(MemoryContext context)
{
	GenerationContext *gen = (GenerationContext *) context;
	const char *name = context->name;
	dlist_iter	iter;
	Size		total_allocated = 0;

	/* walk all blocks in this context */
	dlist_foreach(iter, &gen->blocks)
	{
		GenerationBlock *block = dlist_container(GenerationBlock, node, iter.cur);
		int			nfree,
					nchunks;
		char	   *ptr;
		bool		has_external_chunk = false;

		total_allocated += block->blksize;

		/*
		 * nfree > nchunks is surely wrong.  Equality is allowed as the block
		 * might completely empty if it's the freeblock.
		 */
		if (block->nfree > block->nchunks)
			elog(WARNING, "problem in Generation %s: number of free chunks %d in block %p exceeds %d allocated",
				 name, block->nfree, block, block->nchunks);

		/* check block belongs to the correct context */
		if (block->context != gen)
			elog(WARNING, "problem in Generation %s: bogus context link in block %p",
				 name, block);

		/* Now walk through the chunks and count them. */
		nfree = 0;
		nchunks = 0;
		ptr = ((char *) block) + Generation_BLOCKHDRSZ;

		while (ptr < block->freeptr)
		{
			MemoryChunk *chunk = (MemoryChunk *) ptr;
			GenerationBlock *chunkblock;
			Size		chunksize;

			/* Allow access to the chunk header. */
			VALGRIND_MAKE_MEM_DEFINED(chunk, Generation_CHUNKHDRSZ);

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
			ptr += (chunksize + Generation_CHUNKHDRSZ);

			nchunks += 1;

			/* chunks have both block and context pointers, so check both */
			if (chunkblock != block)
				elog(WARNING, "problem in Generation %s: bogus block link in block %p, chunk %p",
					 name, block, chunk);


			/* is chunk allocated? */
			if (chunk->requested_size != InvalidAllocSize)
			{
				/* now make sure the chunk size is correct */
				if (chunksize < chunk->requested_size ||
					chunksize != MAXALIGN(chunksize))
					elog(WARNING, "problem in Generation %s: bogus chunk size in block %p, chunk %p",
						 name, block, chunk);

				/* check sentinel */
				Assert(chunk->requested_size < chunksize);
				if (!sentinel_ok(chunk, Generation_CHUNKHDRSZ + chunk->requested_size))
					elog(WARNING, "problem in Generation %s: detected write past chunk end in block %p, chunk %p",
						 name, block, chunk);
			}
			else
				nfree += 1;

			/* if chunk is allocated, disallow access to the chunk header */
			if (chunk->requested_size != InvalidAllocSize)
				VALGRIND_MAKE_MEM_NOACCESS(chunk, Generation_CHUNKHDRSZ);
		}

		/*
		 * Make sure we got the expected number of allocated and free chunks
		 * (as tracked in the block header).
		 */
		if (nchunks != block->nchunks)
			elog(WARNING, "problem in Generation %s: number of allocated chunks %d in block %p does not match header %d",
				 name, nchunks, block, block->nchunks);

		if (nfree != block->nfree)
			elog(WARNING, "problem in Generation %s: number of free chunks %d in block %p does not match header %d",
				 name, nfree, block, block->nfree);

		if (has_external_chunk && nchunks > 1)
			elog(WARNING, "problem in Generation %s: external chunk on non-dedicated block %p",
				 name, block);

	}

	Assert(total_allocated == context->mem_allocated);
}

#endif							/* MEMORY_CONTEXT_CHECKING */
