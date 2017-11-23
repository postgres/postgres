/*-------------------------------------------------------------------------
 *
 * generation.c
 *	  Generational allocator definitions.
 *
 * Generation is a custom MemoryContext implementation designed for cases of
 * chunks with similar lifespan.
 *
 * Portions Copyright (c) 2017, PostgreSQL Global Development Group
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
 *	of allocated and freed chunks. Freed chunks are not reused, and once all
 *	chunks in a block are freed, the whole block is thrown away. When the
 *	chunks allocated in the same block have similar lifespan, this works
 *	very well and is very cheap.
 *
 *	The current implementation only uses a fixed block size - maybe it should
 *	adapt a min/max block size range, and grow the blocks automatically.
 *	It already uses dedicated blocks for oversized chunks.
 *
 *	XXX It might be possible to improve this by keeping a small freelist for
 *	only a small number of recent blocks, but it's not clear it's worth the
 *	additional complexity.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "lib/ilist.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"


#define Generation_BLOCKHDRSZ	MAXALIGN(sizeof(GenerationBlock))
#define Generation_CHUNKHDRSZ	sizeof(GenerationChunk)

/* Portion of Generation_CHUNKHDRSZ examined outside generation.c. */
#define Generation_CHUNK_PUBLIC	\
	(offsetof(GenerationChunk, size) + sizeof(Size))

/* Portion of Generation_CHUNKHDRSZ excluding trailing padding. */
#ifdef MEMORY_CONTEXT_CHECKING
#define Generation_CHUNK_USED	\
	(offsetof(GenerationChunk, requested_size) + sizeof(Size))
#else
#define Generation_CHUNK_USED	\
	(offsetof(GenerationChunk, size) + sizeof(Size))
#endif

typedef struct GenerationBlock GenerationBlock;	/* forward reference */
typedef struct GenerationChunk GenerationChunk;

typedef void *GenerationPointer;

/*
 * GenerationContext is a simple memory context not reusing allocated chunks,
 * and freeing blocks once all chunks are freed.
 */
typedef struct GenerationContext
{
	MemoryContextData header;	/* Standard memory-context fields */

	/* Generational context parameters */
	Size		blockSize;		/* block size */

	GenerationBlock	*block;		/* current (most recently allocated) block */
	dlist_head	blocks;			/* list of blocks */
}	GenerationContext;

/*
 * GenerationBlock
 *		GenerationBlock is the unit of memory that is obtained by generation.c
 *		from malloc().  It contains one or more GenerationChunks, which are
 *		the units requested by palloc() and freed by pfree().  GenerationChunks
 *		cannot be returned to malloc() individually, instead pfree()
 *		updates the free counter of the block and when all chunks in a block
 *		are free the whole block is returned to malloc().
 *
 *		GenerationBlock is the header data for a block --- the usable space
 *		within the block begins at the next alignment boundary.
 */
struct GenerationBlock
{
	dlist_node	node;			/* doubly-linked list of blocks */
	int			nchunks;		/* number of chunks in the block */
	int			nfree;			/* number of free chunks */
	char	   *freeptr;		/* start of free space in this block */
	char	   *endptr;			/* end of space in this block */
};

/*
 * GenerationChunk
 *		The prefix of each piece of memory in a GenerationBlock
 */
struct GenerationChunk
{
	/* block owning this chunk */
	void	   *block;

	/* size is always the size of the usable space in the chunk */
	Size		size;
#ifdef MEMORY_CONTEXT_CHECKING
	/* when debugging memory usage, also store actual requested size */
	/* this is zero in a free chunk */
	Size		requested_size;
#define GENERATIONCHUNK_RAWSIZE  (SIZEOF_VOID_P * 2 + SIZEOF_SIZE_T * 2)
#else
#define GENERATIONCHUNK_RAWSIZE  (SIZEOF_VOID_P * 2 + SIZEOF_SIZE_T)
#endif							/* MEMORY_CONTEXT_CHECKING */

	/* ensure proper alignment by adding padding if needed */
#if (GENERATIONCHUNK_RAWSIZE % MAXIMUM_ALIGNOF) != 0
	char		padding[MAXIMUM_ALIGNOF - (GENERATIONCHUNK_RAWSIZE % MAXIMUM_ALIGNOF)];
#endif

	GenerationContext *context; /* owning context */
	/* there must not be any padding to reach a MAXALIGN boundary here! */
};


/*
 * GenerationIsValid
 *		True iff set is valid allocation set.
 */
#define GenerationIsValid(set) PointerIsValid(set)

#define GenerationPointerGetChunk(ptr) \
	((GenerationChunk *)(((char *)(ptr)) - Generation_CHUNKHDRSZ))
#define GenerationChunkGetPointer(chk) \
	((GenerationPointer *)(((char *)(chk)) + Generation_CHUNKHDRSZ))

/*
 * These functions implement the MemoryContext API for Generation contexts.
 */
static void *GenerationAlloc(MemoryContext context, Size size);
static void GenerationFree(MemoryContext context, void *pointer);
static void *GenerationRealloc(MemoryContext context, void *pointer, Size size);
static void GenerationInit(MemoryContext context);
static void GenerationReset(MemoryContext context);
static void GenerationDelete(MemoryContext context);
static Size GenerationGetChunkSpace(MemoryContext context, void *pointer);
static bool GenerationIsEmpty(MemoryContext context);
static void GenerationStats(MemoryContext context, int level, bool print,
		 MemoryContextCounters *totals);

#ifdef MEMORY_CONTEXT_CHECKING
static void GenerationCheck(MemoryContext context);
#endif

/*
 * This is the virtual function table for Generation contexts.
 */
static MemoryContextMethods GenerationMethods = {
	GenerationAlloc,
	GenerationFree,
	GenerationRealloc,
	GenerationInit,
	GenerationReset,
	GenerationDelete,
	GenerationGetChunkSpace,
	GenerationIsEmpty,
	GenerationStats
#ifdef MEMORY_CONTEXT_CHECKING
	,GenerationCheck
#endif
};

/* ----------
 * Debug macros
 * ----------
 */
#ifdef HAVE_ALLOCINFO
#define GenerationFreeInfo(_cxt, _chunk) \
			fprintf(stderr, "GenerationFree: %s: %p, %lu\n", \
				(_cxt)->name, (_chunk), (_chunk)->size)
#define GenerationAllocInfo(_cxt, _chunk) \
			fprintf(stderr, "GenerationAlloc: %s: %p, %lu\n", \
				(_cxt)->name, (_chunk), (_chunk)->size)
#else
#define GenerationFreeInfo(_cxt, _chunk)
#define GenerationAllocInfo(_cxt, _chunk)
#endif


/*
 * Public routines
 */


/*
 * GenerationContextCreate
 *		Create a new Generation context.
 */
MemoryContext
GenerationContextCreate(MemoryContext parent,
				 const char *name,
				 Size blockSize)
{
	GenerationContext  *set;

	StaticAssertStmt(offsetof(GenerationChunk, context) + sizeof(MemoryContext) ==
					 MAXALIGN(sizeof(GenerationChunk)),
					 "padding calculation in GenerationChunk is wrong");

	/*
	 * First, validate allocation parameters.  (If we're going to throw an
	 * error, we should do so before the context is created, not after.)  We
	 * somewhat arbitrarily enforce a minimum 1K block size, mostly because
	 * that's what AllocSet does.
	 */
	if (blockSize != MAXALIGN(blockSize) ||
		blockSize < 1024 ||
		!AllocHugeSizeIsValid(blockSize))
		elog(ERROR, "invalid blockSize for memory context: %zu",
			 blockSize);

	/* Do the type-independent part of context creation */
	set = (GenerationContext *) MemoryContextCreate(T_GenerationContext,
									sizeof(GenerationContext),
									&GenerationMethods,
									parent,
									name);

	set->blockSize = blockSize;
	set->block = NULL;

	return (MemoryContext) set;
}

/*
 * GenerationInit
 *		Context-type-specific initialization routine.
 */
static void
GenerationInit(MemoryContext context)
{
	GenerationContext  *set = (GenerationContext *) context;

	dlist_init(&set->blocks);
}

/*
 * GenerationReset
 *		Frees all memory which is allocated in the given set.
 *
 * The code simply frees all the blocks in the context - we don't keep any
 * keeper blocks or anything like that.
 */
static void
GenerationReset(MemoryContext context)
{
	GenerationContext  *set = (GenerationContext *) context;
	dlist_mutable_iter miter;

	AssertArg(GenerationIsValid(set));

#ifdef MEMORY_CONTEXT_CHECKING
	/* Check for corruption and leaks before freeing */
	GenerationCheck(context);
#endif

	dlist_foreach_modify(miter, &set->blocks)
	{
		GenerationBlock *block = dlist_container(GenerationBlock, node, miter.cur);

		dlist_delete(miter.cur);

		/* Normal case, release the block */
#ifdef CLOBBER_FREED_MEMORY
		wipe_mem(block, set->blockSize);
#endif

		free(block);
	}

	set->block = NULL;

	Assert(dlist_is_empty(&set->blocks));
}

/*
 * GenerationDelete
 *		Frees all memory which is allocated in the given set, in preparation
 *		for deletion of the set. We simply call GenerationReset() which does all the
 *		dirty work.
 */
static void
GenerationDelete(MemoryContext context)
{
	/* just reset (although not really necessary) */
	GenerationReset(context);
}

/*
 * GenerationAlloc
 *		Returns pointer to allocated memory of given size or NULL if
 *		request could not be completed; memory is added to the set.
 *
 * No request may exceed:
 *		MAXALIGN_DOWN(SIZE_MAX) - Generation_BLOCKHDRSZ - Generation_CHUNKHDRSZ
 * All callers use a much-lower limit.
 */
static void *
GenerationAlloc(MemoryContext context, Size size)
{
	GenerationContext  *set = (GenerationContext *) context;
	GenerationBlock	   *block;
	GenerationChunk	   *chunk;

	Size		chunk_size = MAXALIGN(size);

	/* is it an over-sized chunk? if yes, allocate special block */
	if (chunk_size > set->blockSize / 8)
	{
		Size		blksize = chunk_size + Generation_BLOCKHDRSZ + Generation_CHUNKHDRSZ;

		block = (GenerationBlock *) malloc(blksize);
		if (block == NULL)
			return NULL;

		/* block with a single (used) chunk */
		block->nchunks = 1;
		block->nfree = 0;

		/* the block is completely full */
		block->freeptr = block->endptr = ((char *) block) + blksize;

		chunk = (GenerationChunk *) (((char *) block) + Generation_BLOCKHDRSZ);
		chunk->context = set;
		chunk->size = chunk_size;

#ifdef MEMORY_CONTEXT_CHECKING
		/* Valgrind: Will be made NOACCESS below. */
		chunk->requested_size = size;
		/* set mark to catch clobber of "unused" space */
		if (size < chunk_size)
			set_sentinel(GenerationChunkGetPointer(chunk), size);
#endif
#ifdef RANDOMIZE_ALLOCATED_MEMORY
		/* fill the allocated space with junk */
		randomize_mem((char *) GenerationChunkGetPointer(chunk), size);
#endif

		/* add the block to the list of allocated blocks */
		dlist_push_head(&set->blocks, &block->node);

		GenerationAllocInfo(set, chunk);

		/*
		 * Chunk header public fields remain DEFINED.  The requested
		 * allocation itself can be NOACCESS or UNDEFINED; our caller will
		 * soon make it UNDEFINED.  Make extra space at the end of the chunk,
		 * if any, NOACCESS.
		 */
		VALGRIND_MAKE_MEM_NOACCESS((char *) chunk + Generation_CHUNK_PUBLIC,
							 chunk_size + Generation_CHUNKHDRSZ - Generation_CHUNK_PUBLIC);

		return GenerationChunkGetPointer(chunk);
	}

	/*
	 * Not an over-sized chunk. Is there enough space on the current block? If
	 * not, allocate a new "regular" block.
	 */
	block = set->block;

	if ((block == NULL) ||
		(block->endptr - block->freeptr) < Generation_CHUNKHDRSZ + chunk_size)
	{
		Size		blksize = set->blockSize;

		block = (GenerationBlock *) malloc(blksize);

		if (block == NULL)
			return NULL;

		block->nchunks = 0;
		block->nfree = 0;

		block->freeptr = ((char *) block) + Generation_BLOCKHDRSZ;
		block->endptr = ((char *) block) + blksize;

		/* Mark unallocated space NOACCESS. */
		VALGRIND_MAKE_MEM_NOACCESS(block->freeptr,
								   blksize - Generation_BLOCKHDRSZ);

		/* add it to the doubly-linked list of blocks */
		dlist_push_head(&set->blocks, &block->node);

		/* and also use it as the current allocation block */
		set->block = block;
	}

	/* we're supposed to have a block with enough free space now */
	Assert(block != NULL);
	Assert((block->endptr - block->freeptr) >= Generation_CHUNKHDRSZ + chunk_size);

	chunk = (GenerationChunk *) block->freeptr;

	block->nchunks += 1;
	block->freeptr += (Generation_CHUNKHDRSZ + chunk_size);

	chunk->block = block;

	chunk->context = set;
	chunk->size = chunk_size;

#ifdef MEMORY_CONTEXT_CHECKING
	/* Valgrind: Free list requested_size should be DEFINED. */
	chunk->requested_size = size;
	VALGRIND_MAKE_MEM_NOACCESS(&chunk->requested_size,
							   sizeof(chunk->requested_size));
	/* set mark to catch clobber of "unused" space */
	if (size < chunk->size)
		set_sentinel(GenerationChunkGetPointer(chunk), size);
#endif
#ifdef RANDOMIZE_ALLOCATED_MEMORY
	/* fill the allocated space with junk */
	randomize_mem((char *) GenerationChunkGetPointer(chunk), size);
#endif

	GenerationAllocInfo(set, chunk);
	return GenerationChunkGetPointer(chunk);
}

/*
 * GenerationFree
 *		Update number of chunks on the block, and if all chunks on the block
 *		are freeed then discard the block.
 */
static void
GenerationFree(MemoryContext context, void *pointer)
{
	GenerationContext  *set = (GenerationContext *) context;
	GenerationChunk	   *chunk = GenerationPointerGetChunk(pointer);
	GenerationBlock	   *block = chunk->block;

#ifdef MEMORY_CONTEXT_CHECKING
	VALGRIND_MAKE_MEM_DEFINED(&chunk->requested_size,
							  sizeof(chunk->requested_size));
	/* Test for someone scribbling on unused space in chunk */
	if (chunk->requested_size < chunk->size)
		if (!sentinel_ok(pointer, chunk->requested_size))
			elog(WARNING, "detected write past chunk end in %s %p",
				 ((MemoryContext)set)->name, chunk);
#endif

#ifdef CLOBBER_FREED_MEMORY
	wipe_mem(pointer, chunk->size);
#endif

#ifdef MEMORY_CONTEXT_CHECKING
	/* Reset requested_size to 0 in chunks that are on freelist */
	chunk->requested_size = 0;
#endif

	block->nfree += 1;

	Assert(block->nchunks > 0);
	Assert(block->nfree <= block->nchunks);

	/* If there are still allocated chunks on the block, we're done. */
	if (block->nfree < block->nchunks)
		return;

	/*
	 * The block is empty, so let's get rid of it. First remove it from the
	 * list of blocks, then return it to malloc().
	 */
	dlist_delete(&block->node);

	/* Also make sure the block is not marked as the current block. */
	if (set->block == block)
		set->block = NULL;

	free(block);
}

/*
 * GenerationRealloc
 *		When handling repalloc, we simply allocate a new chunk, copy the data
 *		and discard the old one. The only exception is when the new size fits
 *		into the old chunk - in that case we just update chunk header.
 */
static void *
GenerationRealloc(MemoryContext context, void *pointer, Size size)
{
	GenerationContext  *set = (GenerationContext *) context;
	GenerationChunk	   *chunk = GenerationPointerGetChunk(pointer);
	GenerationPointer	newPointer;
	Size		oldsize = chunk->size;

#ifdef MEMORY_CONTEXT_CHECKING
	VALGRIND_MAKE_MEM_DEFINED(&chunk->requested_size,
							  sizeof(chunk->requested_size));
	/* Test for someone scribbling on unused space in chunk */
	if (chunk->requested_size < oldsize)
		if (!sentinel_ok(pointer, chunk->requested_size))
			elog(WARNING, "detected write past chunk end in %s %p",
				 ((MemoryContext)set)->name, chunk);
#endif

	/*
	 * Maybe the allocated area already is >= the new size.  (In particular,
	 * we always fall out here if the requested size is a decrease.)
	 *
	 * This memory context is not use the power-of-2 chunk sizing and instead
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
		VALGRIND_MAKE_MEM_NOACCESS(&chunk->requested_size,
								   sizeof(chunk->requested_size));

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
		if (size < oldsize)
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

		return pointer;
	}

	/* allocate new chunk */
	newPointer = GenerationAlloc((MemoryContext) set, size);

	/* leave immediately if request was not completed */
	if (newPointer == NULL)
		return NULL;

	/*
	 * GenerationSetAlloc() just made the region NOACCESS.  Change it to UNDEFINED
	 * for the moment; memcpy() will then transfer definedness from the old
	 * allocation to the new.  If we know the old allocation, copy just that
	 * much.  Otherwise, make the entire old chunk defined to avoid errors as
	 * we copy the currently-NOACCESS trailing bytes.
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
	GenerationFree((MemoryContext) set, pointer);

	return newPointer;
}

/*
 * GenerationGetChunkSpace
 *		Given a currently-allocated chunk, determine the total space
 *		it occupies (including all memory-allocation overhead).
 */
static Size
GenerationGetChunkSpace(MemoryContext context, void *pointer)
{
	GenerationChunk *chunk = GenerationPointerGetChunk(pointer);

	return chunk->size + Generation_CHUNKHDRSZ;
}

/*
 * GenerationIsEmpty
 *		Is an Generation empty of any allocated space?
 */
static bool
GenerationIsEmpty(MemoryContext context)
{
	GenerationContext  *set = (GenerationContext *) context;

	return dlist_is_empty(&set->blocks);
}

/*
 * GenerationStats
 *		Compute stats about memory consumption of an Generation.
 *
 * level: recursion level (0 at top level); used for print indentation.
 * print: true to print stats to stderr.
 * totals: if not NULL, add stats about this Generation into *totals.
 *
 * XXX freespace only accounts for empty space at the end of the block, not
 * space of freed chunks (which is unknown).
 */
static void
GenerationStats(MemoryContext context, int level, bool print,
		 MemoryContextCounters *totals)
{
	GenerationContext  *set = (GenerationContext *) context;

	Size		nblocks = 0;
	Size		nchunks = 0;
	Size		nfreechunks = 0;
	Size		totalspace = 0;
	Size		freespace = 0;

	dlist_iter	iter;

	dlist_foreach(iter, &set->blocks)
	{
		GenerationBlock *block = dlist_container(GenerationBlock, node, iter.cur);

		nblocks++;
		nchunks += block->nchunks;
		nfreechunks += block->nfree;
		totalspace += set->blockSize;
		freespace += (block->endptr - block->freeptr);
	}

	if (print)
	{
		int			i;

		for (i = 0; i < level; i++)
			fprintf(stderr, "  ");
		fprintf(stderr,
			"Generation: %s: %zu total in %zd blocks (%zd chunks); %zu free (%zd chunks); %zu used\n",
				((MemoryContext)set)->name, totalspace, nblocks, nchunks, freespace,
				nfreechunks, totalspace - freespace);
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
static void
GenerationCheck(MemoryContext context)
{
	GenerationContext  *gen = (GenerationContext *) context;
	char	   *name = context->name;
	dlist_iter	iter;

	/* walk all blocks in this context */
	dlist_foreach(iter, &gen->blocks)
	{
		int			nfree,
					nchunks;
		char	   *ptr;
		GenerationBlock *block = dlist_container(GenerationBlock, node, iter.cur);

		/* We can't free more chunks than allocated. */
		if (block->nfree <= block->nchunks)
			elog(WARNING, "problem in Generation %s: number of free chunks %d in block %p exceeds %d allocated",
				 name, block->nfree, block, block->nchunks);

		/* Now walk through the chunks and count them. */
		nfree = 0;
		nchunks = 0;
		ptr = ((char *) block) + Generation_BLOCKHDRSZ;

		while (ptr < block->freeptr)
		{
			GenerationChunk *chunk = (GenerationChunk *) ptr;

			/* move to the next chunk */
			ptr += (chunk->size + Generation_CHUNKHDRSZ);

			/* chunks have both block and context pointers, so check both */
			if (chunk->block != block)
				elog(WARNING, "problem in Generation %s: bogus block link in block %p, chunk %p",
					 name, block, chunk);

			if (chunk->context != gen)
				elog(WARNING, "problem in Generation %s: bogus context link in block %p, chunk %p",
					 name, block, chunk);

			nchunks += 1;

			/* if requested_size==0, the chunk was freed */
			if (chunk->requested_size > 0)
			{
				/* if the chunk was not freed, we can trigger valgrind checks */
				VALGRIND_MAKE_MEM_DEFINED(&chunk->requested_size,
									   sizeof(chunk->requested_size));

				/* we're in a no-freelist branch */
				VALGRIND_MAKE_MEM_NOACCESS(&chunk->requested_size,
									   sizeof(chunk->requested_size));

				/* now make sure the chunk size is correct */
				if (chunk->size != MAXALIGN(chunk->requested_size))
					elog(WARNING, "problem in Generation %s: bogus chunk size in block %p, chunk %p",
						 name, block, chunk);

				/* there might be sentinel (thanks to alignment) */
				if (chunk->requested_size < chunk->size &&
					!sentinel_ok(chunk, Generation_CHUNKHDRSZ + chunk->requested_size))
					elog(WARNING, "problem in Generation %s: detected write past chunk end in block %p, chunk %p",
						 name, block, chunk);
			}
			else
				nfree += 1;
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
	}
}

#endif   /* MEMORY_CONTEXT_CHECKING */
