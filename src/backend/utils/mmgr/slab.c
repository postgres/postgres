/*-------------------------------------------------------------------------
 *
 * slab.c
 *	  SLAB allocator definitions.
 *
 * SLAB is a MemoryContext implementation designed for cases where large
 * numbers of equally-sized objects can be allocated and freed efficiently
 * with minimal memory wastage and fragmentation.
 *
 *
 * Portions Copyright (c) 2017-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/mmgr/slab.c
 *
 *
 * NOTE:
 *	The constant allocation size allows significant simplification and various
 *	optimizations over more general purpose allocators. The blocks are carved
 *	into chunks of exactly the right size, wasting only the space required to
 *	MAXALIGN the allocated chunks.
 *
 *	Slab can also help reduce memory fragmentation in cases where longer-lived
 *	chunks remain stored on blocks while most of the other chunks have already
 *	been pfree'd.  We give priority to putting new allocations into the
 *	"fullest" block.  This help avoid having too many sparsely used blocks
 *	around and allows blocks to more easily become completely unused which
 *	allows them to be eventually free'd.
 *
 *	We identify the "fullest" block to put new allocations on by using a block
 *	from the lowest populated element of the context's "blocklist" array.
 *	This is an array of dlists containing blocks which we partition by the
 *	number of free chunks which block has.  Blocks with fewer free chunks are
 *	stored in a lower indexed dlist array slot.  Full blocks go on the 0th
 *	element of the blocklist array.  So that we don't have to have too many
 *	elements in the array, each dlist in the array is responsible for a range
 *	of free chunks.  When a chunk is palloc'd or pfree'd we may need to move
 *	the block onto another dlist if the number of free chunks crosses the
 *	range boundary that the current list is responsible for.  Having just a
 *	few blocklist elements reduces the number of times we must move the block
 *	onto another dlist element.
 *
 *	We keep track of free chunks within each block by using a block-level free
 *	list.  We consult this list when we allocate a new chunk in the block.
 *	The free list is a linked list, the head of which is pointed to with
 *	SlabBlock's freehead field.  Each subsequent list item is stored in the
 *	free chunk's memory.  We ensure chunks are large enough to store this
 *	address.
 *
 *	When we allocate a new block, technically all chunks are free, however, to
 *	avoid having to write out the entire block to set the linked list for the
 *	free chunks for every chunk in the block, we instead store a pointer to
 *	the next "unused" chunk on the block and keep track of how many of these
 *	unused chunks there are.  When a new block is malloc'd, all chunks are
 *	unused.  The unused pointer starts with the first chunk on the block and
 *	as chunks are allocated, the unused pointer is incremented.  As chunks are
 *	pfree'd, the unused pointer never goes backwards.  The unused pointer can
 *	be thought of as a high watermark for the maximum number of chunks in the
 *	block which have been in use concurrently.  When a chunk is pfree'd the
 *	chunk is put onto the head of the free list and the unused pointer is not
 *	changed.  We only consume more unused chunks if we run out of free chunks
 *	on the free list.  This method effectively gives priority to using
 *	previously used chunks over previously unused chunks, which should perform
 *	better due to CPU caching effects.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "lib/ilist.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"
#include "utils/memutils_internal.h"
#include "utils/memutils_memorychunk.h"

#define Slab_BLOCKHDRSZ	MAXALIGN(sizeof(SlabBlock))

#ifdef MEMORY_CONTEXT_CHECKING
/*
 * Size of the memory required to store the SlabContext.
 * MEMORY_CONTEXT_CHECKING builds need some extra memory for the isChunkFree
 * array.
 */
#define Slab_CONTEXT_HDRSZ(chunksPerBlock)	\
	(sizeof(SlabContext) + ((chunksPerBlock) * sizeof(bool)))
#else
#define Slab_CONTEXT_HDRSZ(chunksPerBlock)	sizeof(SlabContext)
#endif

/*
 * The number of partitions to divide the blocklist into based their number of
 * free chunks.  There must be at least 2.
 */
#define SLAB_BLOCKLIST_COUNT 3

/* The maximum number of completely empty blocks to keep around for reuse. */
#define SLAB_MAXIMUM_EMPTY_BLOCKS 10

/*
 * SlabContext is a specialized implementation of MemoryContext.
 */
typedef struct SlabContext
{
	MemoryContextData header;	/* Standard memory-context fields */
	/* Allocation parameters for this context: */
	uint32		chunkSize;		/* the requested (non-aligned) chunk size */
	uint32		fullChunkSize;	/* chunk size with chunk header and alignment */
	uint32		blockSize;		/* the size to make each block of chunks */
	int32		chunksPerBlock; /* number of chunks that fit in 1 block */
	int32		curBlocklistIndex;	/* index into the blocklist[] element
									 * containing the fullest, blocks */
#ifdef MEMORY_CONTEXT_CHECKING
	bool	   *isChunkFree;	/* array to mark free chunks in a block during
								 * SlabCheck */
#endif

	int32		blocklist_shift;	/* number of bits to shift the nfree count
									 * by to get the index into blocklist[] */
	dclist_head emptyblocks;	/* empty blocks to use up first instead of
								 * mallocing new blocks */

	/*
	 * Blocks with free space, grouped by the number of free chunks they
	 * contain.  Completely full blocks are stored in the 0th element.
	 * Completely empty blocks are stored in emptyblocks or free'd if we have
	 * enough empty blocks already.
	 */
	dlist_head	blocklist[SLAB_BLOCKLIST_COUNT];
} SlabContext;

/*
 * SlabBlock
 *		Structure of a single slab block.
 *
 * slab: pointer back to the owning MemoryContext
 * nfree: number of chunks on the block which are unallocated
 * nunused: number of chunks on the block unallocated and not on the block's
 * freelist.
 * freehead: linked-list header storing a pointer to the first free chunk on
 * the block.  Subsequent pointers are stored in the chunk's memory.  NULL
 * indicates the end of the list.
 * unused: pointer to the next chunk which has yet to be used.
 * node: doubly-linked list node for the context's blocklist
 */
typedef struct SlabBlock
{
	SlabContext *slab;			/* owning context */
	int32		nfree;			/* number of chunks on free + unused chunks */
	int32		nunused;		/* number of unused chunks */
	MemoryChunk *freehead;		/* pointer to the first free chunk */
	MemoryChunk *unused;		/* pointer to the next unused chunk */
	dlist_node	node;			/* doubly-linked list for blocklist[] */
} SlabBlock;


#define Slab_CHUNKHDRSZ sizeof(MemoryChunk)
#define SlabChunkGetPointer(chk)	\
	((void *) (((char *) (chk)) + sizeof(MemoryChunk)))

/*
 * SlabBlockGetChunk
 *		Obtain a pointer to the nth (0-based) chunk in the block
 */
#define SlabBlockGetChunk(slab, block, n) \
	((MemoryChunk *) ((char *) (block) + Slab_BLOCKHDRSZ	\
					+ ((n) * (slab)->fullChunkSize)))

#if defined(MEMORY_CONTEXT_CHECKING) || defined(USE_ASSERT_CHECKING)

/*
 * SlabChunkIndex
 *		Get the 0-based index of how many chunks into the block the given
 *		chunk is.
*/
#define SlabChunkIndex(slab, block, chunk)	\
	(((char *) (chunk) - (char *) SlabBlockGetChunk(slab, block, 0)) / \
	(slab)->fullChunkSize)

/*
 * SlabChunkMod
 *		A MemoryChunk should always be at an address which is a multiple of
 *		fullChunkSize starting from the 0th chunk position.  This will return
 *		non-zero if it's not.
 */
#define SlabChunkMod(slab, block, chunk)	\
	(((char *) (chunk) - (char *) SlabBlockGetChunk(slab, block, 0)) % \
	(slab)->fullChunkSize)

#endif

/*
 * SlabIsValid
 *		True iff set is a valid slab allocation set.
 */
#define SlabIsValid(set) (PointerIsValid(set) && IsA(set, SlabContext))

/*
 * SlabBlockIsValid
 *		True iff block is a valid block of slab allocation set.
 */
#define SlabBlockIsValid(block) \
	(PointerIsValid(block) && SlabIsValid((block)->slab))

/*
 * SlabBlocklistIndex
 *		Determine the blocklist index that a block should be in for the given
 *		number of free chunks.
 */
static inline int32
SlabBlocklistIndex(SlabContext *slab, int nfree)
{
	int32		index;
	int32		blocklist_shift = slab->blocklist_shift;

	Assert(nfree >= 0 && nfree <= slab->chunksPerBlock);

	/*
	 * Determine the blocklist index based on the number of free chunks.  We
	 * must ensure that 0 free chunks is dedicated to index 0.  Everything
	 * else must be >= 1 and < SLAB_BLOCKLIST_COUNT.
	 *
	 * To make this as efficient as possible, we exploit some two's complement
	 * arithmetic where we reverse the sign before bit shifting.  This results
	 * in an nfree of 0 using index 0 and anything non-zero staying non-zero.
	 * This is exploiting 0 and -0 being the same in two's complement.  When
	 * we're done, we just need to flip the sign back over again for a
	 * positive index.
	 */
	index = -((-nfree) >> blocklist_shift);

	if (nfree == 0)
		Assert(index == 0);
	else
		Assert(index >= 1 && index < SLAB_BLOCKLIST_COUNT);

	return index;
}

/*
 * SlabFindNextBlockListIndex
 *		Search blocklist for blocks which have free chunks and return the
 *		index of the blocklist found containing at least 1 block with free
 *		chunks.  If no block can be found we return 0.
 *
 * Note: We give priority to fuller blocks so that these are filled before
 * emptier blocks.  This is done to increase the chances that mostly-empty
 * blocks will eventually become completely empty so they can be free'd.
 */
static int32
SlabFindNextBlockListIndex(SlabContext *slab)
{
	/* start at 1 as blocklist[0] is for full blocks. */
	for (int i = 1; i < SLAB_BLOCKLIST_COUNT; i++)
	{
		/* return the first found non-empty index */
		if (!dlist_is_empty(&slab->blocklist[i]))
			return i;
	}

	/* no blocks with free space */
	return 0;
}

/*
 * SlabGetNextFreeChunk
 *		Return the next free chunk in block and update the block to account
 *		for the returned chunk now being used.
 */
static inline MemoryChunk *
SlabGetNextFreeChunk(SlabContext *slab, SlabBlock *block)
{
	MemoryChunk *chunk;

	Assert(block->nfree > 0);

	if (block->freehead != NULL)
	{
		chunk = block->freehead;

		/*
		 * Pop the chunk from the linked list of free chunks.  The pointer to
		 * the next free chunk is stored in the chunk itself.
		 */
		VALGRIND_MAKE_MEM_DEFINED(SlabChunkGetPointer(chunk), sizeof(MemoryChunk *));
		block->freehead = *(MemoryChunk **) SlabChunkGetPointer(chunk);

		/* check nothing stomped on the free chunk's memory */
		Assert(block->freehead == NULL ||
			   (block->freehead >= SlabBlockGetChunk(slab, block, 0) &&
				block->freehead <= SlabBlockGetChunk(slab, block, slab->chunksPerBlock - 1) &&
				SlabChunkMod(slab, block, block->freehead) == 0));
	}
	else
	{
		Assert(block->nunused > 0);

		chunk = block->unused;
		block->unused = (MemoryChunk *) (((char *) block->unused) + slab->fullChunkSize);
		block->nunused--;
	}

	block->nfree--;

	return chunk;
}

/*
 * SlabContextCreate
 *		Create a new Slab context.
 *
 * parent: parent context, or NULL if top-level context
 * name: name of context (must be statically allocated)
 * blockSize: allocation block size
 * chunkSize: allocation chunk size
 *
 * The Slab_CHUNKHDRSZ + MAXALIGN(chunkSize + 1) may not exceed
 * MEMORYCHUNK_MAX_VALUE.
 * 'blockSize' may not exceed MEMORYCHUNK_MAX_BLOCKOFFSET.
 */
MemoryContext
SlabContextCreate(MemoryContext parent,
				  const char *name,
				  Size blockSize,
				  Size chunkSize)
{
	int			chunksPerBlock;
	Size		fullChunkSize;
	SlabContext *slab;
	int			i;

	/* ensure MemoryChunk's size is properly maxaligned */
	StaticAssertDecl(Slab_CHUNKHDRSZ == MAXALIGN(Slab_CHUNKHDRSZ),
					 "sizeof(MemoryChunk) is not maxaligned");
	Assert(blockSize <= MEMORYCHUNK_MAX_BLOCKOFFSET);

	/*
	 * Ensure there's enough space to store the pointer to the next free chunk
	 * in the memory of the (otherwise) unused allocation.
	 */
	if (chunkSize < sizeof(MemoryChunk *))
		chunkSize = sizeof(MemoryChunk *);

	/* length of the maxaligned chunk including the chunk header  */
#ifdef MEMORY_CONTEXT_CHECKING
	/* ensure there's always space for the sentinel byte */
	fullChunkSize = Slab_CHUNKHDRSZ + MAXALIGN(chunkSize + 1);
#else
	fullChunkSize = Slab_CHUNKHDRSZ + MAXALIGN(chunkSize);
#endif

	Assert(fullChunkSize <= MEMORYCHUNK_MAX_VALUE);

	/* compute the number of chunks that will fit on each block */
	chunksPerBlock = (blockSize - Slab_BLOCKHDRSZ) / fullChunkSize;

	/* Make sure the block can store at least one chunk. */
	if (chunksPerBlock == 0)
		elog(ERROR, "block size %zu for slab is too small for %zu-byte chunks",
			 blockSize, chunkSize);



	slab = (SlabContext *) malloc(Slab_CONTEXT_HDRSZ(chunksPerBlock));
	if (slab == NULL)
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

	/* Fill in SlabContext-specific header fields */
	slab->chunkSize = (uint32) chunkSize;
	slab->fullChunkSize = (uint32) fullChunkSize;
	slab->blockSize = (uint32) blockSize;
	slab->chunksPerBlock = chunksPerBlock;
	slab->curBlocklistIndex = 0;

	/*
	 * Compute a shift that guarantees that shifting chunksPerBlock with it is
	 * < SLAB_BLOCKLIST_COUNT - 1.  The reason that we subtract 1 from
	 * SLAB_BLOCKLIST_COUNT in this calculation is that we reserve the 0th
	 * blocklist element for blocks which have no free chunks.
	 *
	 * We calculate the number of bits to shift by rather than a divisor to
	 * divide by as performing division each time we need to find the
	 * blocklist index would be much slower.
	 */
	slab->blocklist_shift = 0;
	while ((slab->chunksPerBlock >> slab->blocklist_shift) >= (SLAB_BLOCKLIST_COUNT - 1))
		slab->blocklist_shift++;

	/* initialize the list to store empty blocks to be reused */
	dclist_init(&slab->emptyblocks);

	/* initialize each blocklist slot */
	for (i = 0; i < SLAB_BLOCKLIST_COUNT; i++)
		dlist_init(&slab->blocklist[i]);

#ifdef MEMORY_CONTEXT_CHECKING
	/* set the isChunkFree pointer right after the end of the context */
	slab->isChunkFree = (bool *) ((char *) slab + sizeof(SlabContext));
#endif

	/* Finally, do the type-independent part of context creation */
	MemoryContextCreate((MemoryContext) slab,
						T_SlabContext,
						MCTX_SLAB_ID,
						parent,
						name);

	return (MemoryContext) slab;
}

/*
 * SlabReset
 *		Frees all memory which is allocated in the given set.
 *
 * The code simply frees all the blocks in the context - we don't keep any
 * keeper blocks or anything like that.
 */
void
SlabReset(MemoryContext context)
{
	SlabContext *slab = (SlabContext *) context;
	dlist_mutable_iter miter;
	int			i;

	Assert(SlabIsValid(slab));

#ifdef MEMORY_CONTEXT_CHECKING
	/* Check for corruption and leaks before freeing */
	SlabCheck(context);
#endif

	/* release any retained empty blocks */
	dclist_foreach_modify(miter, &slab->emptyblocks)
	{
		SlabBlock  *block = dlist_container(SlabBlock, node, miter.cur);

		dclist_delete_from(&slab->emptyblocks, miter.cur);

#ifdef CLOBBER_FREED_MEMORY
		wipe_mem(block, slab->blockSize);
#endif
		free(block);
		context->mem_allocated -= slab->blockSize;
	}

	/* walk over blocklist and free the blocks */
	for (i = 0; i < SLAB_BLOCKLIST_COUNT; i++)
	{
		dlist_foreach_modify(miter, &slab->blocklist[i])
		{
			SlabBlock  *block = dlist_container(SlabBlock, node, miter.cur);

			dlist_delete(miter.cur);

#ifdef CLOBBER_FREED_MEMORY
			wipe_mem(block, slab->blockSize);
#endif
			free(block);
			context->mem_allocated -= slab->blockSize;
		}
	}

	slab->curBlocklistIndex = 0;

	Assert(context->mem_allocated == 0);
}

/*
 * SlabDelete
 *		Free all memory which is allocated in the given context.
 */
void
SlabDelete(MemoryContext context)
{
	/* Reset to release all the SlabBlocks */
	SlabReset(context);
	/* And free the context header */
	free(context);
}

/*
 * Small helper for allocating a new chunk from a chunk, to avoid duplicating
 * the code between SlabAlloc() and SlabAllocFromNewBlock().
 */
static inline void *
SlabAllocSetupNewChunk(MemoryContext context, SlabBlock *block,
					   MemoryChunk *chunk, Size size)
{
	SlabContext *slab = (SlabContext *) context;

	/*
	 * Check that the chunk pointer is actually somewhere on the block and is
	 * aligned as expected.
	 */
	Assert(chunk >= SlabBlockGetChunk(slab, block, 0));
	Assert(chunk <= SlabBlockGetChunk(slab, block, slab->chunksPerBlock - 1));
	Assert(SlabChunkMod(slab, block, chunk) == 0);

	/* Prepare to initialize the chunk header. */
	VALGRIND_MAKE_MEM_UNDEFINED(chunk, Slab_CHUNKHDRSZ);

	MemoryChunkSetHdrMask(chunk, block, MAXALIGN(slab->chunkSize), MCTX_SLAB_ID);

#ifdef MEMORY_CONTEXT_CHECKING
	/* slab mark to catch clobber of "unused" space */
	Assert(slab->chunkSize < (slab->fullChunkSize - Slab_CHUNKHDRSZ));
	set_sentinel(MemoryChunkGetPointer(chunk), size);
	VALGRIND_MAKE_MEM_NOACCESS(((char *) chunk) + Slab_CHUNKHDRSZ +
							   slab->chunkSize,
							   slab->fullChunkSize -
							   (slab->chunkSize + Slab_CHUNKHDRSZ));
#endif

#ifdef RANDOMIZE_ALLOCATED_MEMORY
	/* fill the allocated space with junk */
	randomize_mem((char *) MemoryChunkGetPointer(chunk), size);
#endif

	/* Disallow access to the chunk header. */
	VALGRIND_MAKE_MEM_NOACCESS(chunk, Slab_CHUNKHDRSZ);

	return MemoryChunkGetPointer(chunk);
}

pg_noinline
static void *
SlabAllocFromNewBlock(MemoryContext context, Size size, int flags)
{
	SlabContext *slab = (SlabContext *) context;
	SlabBlock  *block;
	MemoryChunk *chunk;
	dlist_head *blocklist;
	int			blocklist_idx;

	/* to save allocating a new one, first check the empty blocks list */
	if (dclist_count(&slab->emptyblocks) > 0)
	{
		dlist_node *node = dclist_pop_head_node(&slab->emptyblocks);

		block = dlist_container(SlabBlock, node, node);

		/*
		 * SlabFree() should have left this block in a valid state with all
		 * chunks free.  Ensure that's the case.
		 */
		Assert(block->nfree == slab->chunksPerBlock);

		/* fetch the next chunk from this block */
		chunk = SlabGetNextFreeChunk(slab, block);
	}
	else
	{
		block = (SlabBlock *) malloc(slab->blockSize);

		if (unlikely(block == NULL))
			return MemoryContextAllocationFailure(context, size, flags);

		block->slab = slab;
		context->mem_allocated += slab->blockSize;

		/* use the first chunk in the new block */
		chunk = SlabBlockGetChunk(slab, block, 0);

		block->nfree = slab->chunksPerBlock - 1;
		block->unused = SlabBlockGetChunk(slab, block, 1);
		block->freehead = NULL;
		block->nunused = slab->chunksPerBlock - 1;
	}

	/* find the blocklist element for storing blocks with 1 used chunk */
	blocklist_idx = SlabBlocklistIndex(slab, block->nfree);
	blocklist = &slab->blocklist[blocklist_idx];

	/* this better be empty.  We just added a block thinking it was */
	Assert(dlist_is_empty(blocklist));

	dlist_push_head(blocklist, &block->node);

	slab->curBlocklistIndex = blocklist_idx;

	return SlabAllocSetupNewChunk(context, block, chunk, size);
}

/*
 * SlabAllocInvalidSize
 *		Handle raising an ERROR for an invalid size request.  We don't do this
 *		in slab alloc as calling the elog functions would force the compiler
 *		to setup the stack frame in SlabAlloc.  For performance reasons, we
 *		want to avoid that.
 */
pg_noinline
static void
pg_attribute_noreturn()
SlabAllocInvalidSize(MemoryContext context, Size size)
{
	SlabContext *slab = (SlabContext *) context;

	elog(ERROR, "unexpected alloc chunk size %zu (expected %u)", size,
		 slab->chunkSize);
}

/*
 * SlabAlloc
 *		Returns a pointer to a newly allocated memory chunk or raises an ERROR
 *		on allocation failure, or returns NULL when flags contains
 *		MCXT_ALLOC_NO_OOM.  'size' must be the same size as was specified
 *		during SlabContextCreate().
 *
 * This function should only contain the most common code paths.  Everything
 * else should be in pg_noinline helper functions, thus avoiding the overhead
 * of creating a stack frame for the common cases.  Allocating memory is often
 * a bottleneck in many workloads, so avoiding stack frame setup is
 * worthwhile.  Helper functions should always directly return the newly
 * allocated memory so that we can just return that address directly as a tail
 * call.
 */
void *
SlabAlloc(MemoryContext context, Size size, int flags)
{
	SlabContext *slab = (SlabContext *) context;
	SlabBlock  *block;
	MemoryChunk *chunk;

	Assert(SlabIsValid(slab));

	/* sanity check that this is pointing to a valid blocklist */
	Assert(slab->curBlocklistIndex >= 0);
	Assert(slab->curBlocklistIndex <= SlabBlocklistIndex(slab, slab->chunksPerBlock));

	/*
	 * Make sure we only allow correct request size.  This doubles as the
	 * MemoryContextCheckSize check.
	 */
	if (unlikely(size != slab->chunkSize))
		SlabAllocInvalidSize(context, size);

	if (unlikely(slab->curBlocklistIndex == 0))
	{
		/*
		 * Handle the case when there are no partially filled blocks
		 * available.  This happens either when the last allocation took the
		 * last chunk in the block, or when SlabFree() free'd the final block.
		 */
		return SlabAllocFromNewBlock(context, size, flags);
	}
	else
	{
		dlist_head *blocklist = &slab->blocklist[slab->curBlocklistIndex];
		int			new_blocklist_idx;

		Assert(!dlist_is_empty(blocklist));

		/* grab the block from the blocklist */
		block = dlist_head_element(SlabBlock, node, blocklist);

		/* make sure we actually got a valid block, with matching nfree */
		Assert(block != NULL);
		Assert(slab->curBlocklistIndex == SlabBlocklistIndex(slab, block->nfree));
		Assert(block->nfree > 0);

		/* fetch the next chunk from this block */
		chunk = SlabGetNextFreeChunk(slab, block);

		/* get the new blocklist index based on the new free chunk count */
		new_blocklist_idx = SlabBlocklistIndex(slab, block->nfree);

		/*
		 * Handle the case where the blocklist index changes.  This also deals
		 * with blocks becoming full as only full blocks go at index 0.
		 */
		if (unlikely(slab->curBlocklistIndex != new_blocklist_idx))
		{
			dlist_delete_from(blocklist, &block->node);
			dlist_push_head(&slab->blocklist[new_blocklist_idx], &block->node);

			if (dlist_is_empty(blocklist))
				slab->curBlocklistIndex = SlabFindNextBlockListIndex(slab);
		}
	}

	return SlabAllocSetupNewChunk(context, block, chunk, size);
}

/*
 * SlabFree
 *		Frees allocated memory; memory is removed from the slab.
 */
void
SlabFree(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	SlabBlock  *block;
	SlabContext *slab;
	int			curBlocklistIdx;
	int			newBlocklistIdx;

	/* Allow access to the chunk header. */
	VALGRIND_MAKE_MEM_DEFINED(chunk, Slab_CHUNKHDRSZ);

	block = MemoryChunkGetBlock(chunk);

	/*
	 * For speed reasons we just Assert that the referenced block is good.
	 * Future field experience may show that this Assert had better become a
	 * regular runtime test-and-elog check.
	 */
	Assert(SlabBlockIsValid(block));
	slab = block->slab;

#ifdef MEMORY_CONTEXT_CHECKING
	/* Test for someone scribbling on unused space in chunk */
	Assert(slab->chunkSize < (slab->fullChunkSize - Slab_CHUNKHDRSZ));
	if (!sentinel_ok(pointer, slab->chunkSize))
		elog(WARNING, "detected write past chunk end in %s %p",
			 slab->header.name, chunk);
#endif

	/* push this chunk onto the head of the block's free list */
	*(MemoryChunk **) pointer = block->freehead;
	block->freehead = chunk;

	block->nfree++;

	Assert(block->nfree > 0);
	Assert(block->nfree <= slab->chunksPerBlock);

#ifdef CLOBBER_FREED_MEMORY
	/* don't wipe the free list MemoryChunk pointer stored in the chunk */
	wipe_mem((char *) pointer + sizeof(MemoryChunk *),
			 slab->chunkSize - sizeof(MemoryChunk *));
#endif

	curBlocklistIdx = SlabBlocklistIndex(slab, block->nfree - 1);
	newBlocklistIdx = SlabBlocklistIndex(slab, block->nfree);

	/*
	 * Check if the block needs to be moved to another element on the
	 * blocklist based on it now having 1 more free chunk.
	 */
	if (unlikely(curBlocklistIdx != newBlocklistIdx))
	{
		/* do the move */
		dlist_delete_from(&slab->blocklist[curBlocklistIdx], &block->node);
		dlist_push_head(&slab->blocklist[newBlocklistIdx], &block->node);

		/*
		 * The blocklist[curBlocklistIdx] may now be empty or we may now be
		 * able to use a lower-element blocklist.  We'll need to redetermine
		 * what the slab->curBlocklistIndex is if the current blocklist was
		 * changed or if a lower element one was changed.  We must ensure we
		 * use the list with the fullest block(s).
		 */
		if (slab->curBlocklistIndex >= curBlocklistIdx)
		{
			slab->curBlocklistIndex = SlabFindNextBlockListIndex(slab);

			/*
			 * We know there must be a block with at least 1 unused chunk as
			 * we just pfree'd one.  Ensure curBlocklistIndex reflects this.
			 */
			Assert(slab->curBlocklistIndex > 0);
		}
	}

	/* Handle when a block becomes completely empty */
	if (unlikely(block->nfree == slab->chunksPerBlock))
	{
		/* remove the block */
		dlist_delete_from(&slab->blocklist[newBlocklistIdx], &block->node);

		/*
		 * To avoid thrashing malloc/free, we keep a list of empty blocks that
		 * we can reuse again instead of having to malloc a new one.
		 */
		if (dclist_count(&slab->emptyblocks) < SLAB_MAXIMUM_EMPTY_BLOCKS)
			dclist_push_head(&slab->emptyblocks, &block->node);
		else
		{
			/*
			 * When we have enough empty blocks stored already, we actually
			 * free the block.
			 */
#ifdef CLOBBER_FREED_MEMORY
			wipe_mem(block, slab->blockSize);
#endif
			free(block);
			slab->header.mem_allocated -= slab->blockSize;
		}

		/*
		 * Check if we need to reset the blocklist index.  This is required
		 * when the blocklist this block is on has become completely empty.
		 */
		if (slab->curBlocklistIndex == newBlocklistIdx &&
			dlist_is_empty(&slab->blocklist[newBlocklistIdx]))
			slab->curBlocklistIndex = SlabFindNextBlockListIndex(slab);
	}
}

/*
 * SlabRealloc
 *		Change the allocated size of a chunk.
 *
 * As Slab is designed for allocating equally-sized chunks of memory, it can't
 * do an actual chunk size change.  We try to be gentle and allow calls with
 * exactly the same size, as in that case we can simply return the same
 * chunk.  When the size differs, we throw an error.
 *
 * We could also allow requests with size < chunkSize.  That however seems
 * rather pointless - Slab is meant for chunks of constant size, and moreover
 * realloc is usually used to enlarge the chunk.
 */
void *
SlabRealloc(void *pointer, Size size, int flags)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	SlabBlock  *block;
	SlabContext *slab;

	/* Allow access to the chunk header. */
	VALGRIND_MAKE_MEM_DEFINED(chunk, Slab_CHUNKHDRSZ);

	block = MemoryChunkGetBlock(chunk);

	/* Disallow access to the chunk header. */
	VALGRIND_MAKE_MEM_NOACCESS(chunk, Slab_CHUNKHDRSZ);

	/*
	 * Try to verify that we have a sane block pointer: the block header
	 * should reference a slab context.  (We use a test-and-elog, not just
	 * Assert, because it seems highly likely that we're here in error in the
	 * first place.)
	 */
	if (!SlabBlockIsValid(block))
		elog(ERROR, "could not find block containing chunk %p", chunk);
	slab = block->slab;

	/* can't do actual realloc with slab, but let's try to be gentle */
	if (size == slab->chunkSize)
		return pointer;

	elog(ERROR, "slab allocator does not support realloc()");
	return NULL;				/* keep compiler quiet */
}

/*
 * SlabGetChunkContext
 *		Return the MemoryContext that 'pointer' belongs to.
 */
MemoryContext
SlabGetChunkContext(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	SlabBlock  *block;

	/* Allow access to the chunk header. */
	VALGRIND_MAKE_MEM_DEFINED(chunk, Slab_CHUNKHDRSZ);

	block = MemoryChunkGetBlock(chunk);

	/* Disallow access to the chunk header. */
	VALGRIND_MAKE_MEM_NOACCESS(chunk, Slab_CHUNKHDRSZ);

	Assert(SlabBlockIsValid(block));

	return &block->slab->header;
}

/*
 * SlabGetChunkSpace
 *		Given a currently-allocated chunk, determine the total space
 *		it occupies (including all memory-allocation overhead).
 */
Size
SlabGetChunkSpace(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	SlabBlock  *block;
	SlabContext *slab;

	/* Allow access to the chunk header. */
	VALGRIND_MAKE_MEM_DEFINED(chunk, Slab_CHUNKHDRSZ);

	block = MemoryChunkGetBlock(chunk);

	/* Disallow access to the chunk header. */
	VALGRIND_MAKE_MEM_NOACCESS(chunk, Slab_CHUNKHDRSZ);

	Assert(SlabBlockIsValid(block));
	slab = block->slab;

	return slab->fullChunkSize;
}

/*
 * SlabIsEmpty
 *		Is the slab empty of any allocated space?
 */
bool
SlabIsEmpty(MemoryContext context)
{
	Assert(SlabIsValid((SlabContext *) context));

	return (context->mem_allocated == 0);
}

/*
 * SlabStats
 *		Compute stats about memory consumption of a Slab context.
 *
 * printfunc: if not NULL, pass a human-readable stats string to this.
 * passthru: pass this pointer through to printfunc.
 * totals: if not NULL, add stats about this context into *totals.
 * print_to_stderr: print stats to stderr if true, elog otherwise.
 */
void
SlabStats(MemoryContext context,
		  MemoryStatsPrintFunc printfunc, void *passthru,
		  MemoryContextCounters *totals,
		  bool print_to_stderr)
{
	SlabContext *slab = (SlabContext *) context;
	Size		nblocks = 0;
	Size		freechunks = 0;
	Size		totalspace;
	Size		freespace = 0;
	int			i;

	Assert(SlabIsValid(slab));

	/* Include context header in totalspace */
	totalspace = Slab_CONTEXT_HDRSZ(slab->chunksPerBlock);

	/* Add the space consumed by blocks in the emptyblocks list */
	totalspace += dclist_count(&slab->emptyblocks) * slab->blockSize;

	for (i = 0; i < SLAB_BLOCKLIST_COUNT; i++)
	{
		dlist_iter	iter;

		dlist_foreach(iter, &slab->blocklist[i])
		{
			SlabBlock  *block = dlist_container(SlabBlock, node, iter.cur);

			nblocks++;
			totalspace += slab->blockSize;
			freespace += slab->fullChunkSize * block->nfree;
			freechunks += block->nfree;
		}
	}

	if (printfunc)
	{
		char		stats_string[200];

		/* XXX should we include free chunks on empty blocks? */
		snprintf(stats_string, sizeof(stats_string),
				 "%zu total in %zu blocks; %u empty blocks; %zu free (%zu chunks); %zu used",
				 totalspace, nblocks, dclist_count(&slab->emptyblocks),
				 freespace, freechunks, totalspace - freespace);
		printfunc(context, passthru, stats_string, print_to_stderr);
	}

	if (totals)
	{
		totals->nblocks += nblocks;
		totals->freechunks += freechunks;
		totals->totalspace += totalspace;
		totals->freespace += freespace;
	}
}


#ifdef MEMORY_CONTEXT_CHECKING

/*
 * SlabCheck
 *		Walk through all blocks looking for inconsistencies.
 *
 * NOTE: report errors as WARNING, *not* ERROR or FATAL.  Otherwise you'll
 * find yourself in an infinite loop when trouble occurs, because this
 * routine will be entered again when elog cleanup tries to release memory!
 */
void
SlabCheck(MemoryContext context)
{
	SlabContext *slab = (SlabContext *) context;
	int			i;
	int			nblocks = 0;
	const char *name = slab->header.name;
	dlist_iter	iter;

	Assert(SlabIsValid(slab));
	Assert(slab->chunksPerBlock > 0);

	/*
	 * Have a look at the empty blocks.  These should have all their chunks
	 * marked as free.  Ensure that's the case.
	 */
	dclist_foreach(iter, &slab->emptyblocks)
	{
		SlabBlock  *block = dlist_container(SlabBlock, node, iter.cur);

		if (block->nfree != slab->chunksPerBlock)
			elog(WARNING, "problem in slab %s: empty block %p should have %d free chunks but has %d chunks free",
				 name, block, slab->chunksPerBlock, block->nfree);
	}

	/* walk the non-empty block lists */
	for (i = 0; i < SLAB_BLOCKLIST_COUNT; i++)
	{
		int			j,
					nfree;

		/* walk all blocks on this blocklist */
		dlist_foreach(iter, &slab->blocklist[i])
		{
			SlabBlock  *block = dlist_container(SlabBlock, node, iter.cur);
			MemoryChunk *cur_chunk;

			/*
			 * Make sure the number of free chunks (in the block header)
			 * matches the position in the blocklist.
			 */
			if (SlabBlocklistIndex(slab, block->nfree) != i)
				elog(WARNING, "problem in slab %s: block %p is on blocklist %d but should be on blocklist %d",
					 name, block, i, SlabBlocklistIndex(slab, block->nfree));

			/* make sure the block is not empty */
			if (block->nfree >= slab->chunksPerBlock)
				elog(WARNING, "problem in slab %s: empty block %p incorrectly stored on blocklist element %d",
					 name, block, i);

			/* make sure the slab pointer correctly points to this context */
			if (block->slab != slab)
				elog(WARNING, "problem in slab %s: bogus slab link in block %p",
					 name, block);

			/* reset the array of free chunks for this block */
			memset(slab->isChunkFree, 0, (slab->chunksPerBlock * sizeof(bool)));
			nfree = 0;

			/* walk through the block's free list chunks */
			cur_chunk = block->freehead;
			while (cur_chunk != NULL)
			{
				int			chunkidx = SlabChunkIndex(slab, block, cur_chunk);

				/*
				 * Ensure the free list link points to something on the block
				 * at an address aligned according to the full chunk size.
				 */
				if (cur_chunk < SlabBlockGetChunk(slab, block, 0) ||
					cur_chunk > SlabBlockGetChunk(slab, block, slab->chunksPerBlock - 1) ||
					SlabChunkMod(slab, block, cur_chunk) != 0)
					elog(WARNING, "problem in slab %s: bogus free list link %p in block %p",
						 name, cur_chunk, block);

				/* count the chunk and mark it free on the free chunk array */
				nfree++;
				slab->isChunkFree[chunkidx] = true;

				/* read pointer of the next free chunk */
				VALGRIND_MAKE_MEM_DEFINED(MemoryChunkGetPointer(cur_chunk), sizeof(MemoryChunk *));
				cur_chunk = *(MemoryChunk **) SlabChunkGetPointer(cur_chunk);
			}

			/* check that the unused pointer matches what nunused claims */
			if (SlabBlockGetChunk(slab, block, slab->chunksPerBlock - block->nunused) !=
				block->unused)
				elog(WARNING, "problem in slab %s: mismatch detected between nunused chunks and unused pointer in block %p",
					 name, block);

			/*
			 * count the remaining free chunks that have yet to make it onto
			 * the block's free list.
			 */
			cur_chunk = block->unused;
			for (j = 0; j < block->nunused; j++)
			{
				int			chunkidx = SlabChunkIndex(slab, block, cur_chunk);


				/* count the chunk as free and mark it as so in the array */
				nfree++;
				if (chunkidx < slab->chunksPerBlock)
					slab->isChunkFree[chunkidx] = true;

				/* move forward 1 chunk */
				cur_chunk = (MemoryChunk *) (((char *) cur_chunk) + slab->fullChunkSize);
			}

			for (j = 0; j < slab->chunksPerBlock; j++)
			{
				if (!slab->isChunkFree[j])
				{
					MemoryChunk *chunk = SlabBlockGetChunk(slab, block, j);
					SlabBlock  *chunkblock;

					/* Allow access to the chunk header. */
					VALGRIND_MAKE_MEM_DEFINED(chunk, Slab_CHUNKHDRSZ);

					chunkblock = (SlabBlock *) MemoryChunkGetBlock(chunk);

					/* Disallow access to the chunk header. */
					VALGRIND_MAKE_MEM_NOACCESS(chunk, Slab_CHUNKHDRSZ);

					/*
					 * check the chunk's blockoffset correctly points back to
					 * the block
					 */
					if (chunkblock != block)
						elog(WARNING, "problem in slab %s: bogus block link in block %p, chunk %p",
							 name, block, chunk);

					/* check the sentinel byte is intact */
					Assert(slab->chunkSize < (slab->fullChunkSize - Slab_CHUNKHDRSZ));
					if (!sentinel_ok(chunk, Slab_CHUNKHDRSZ + slab->chunkSize))
						elog(WARNING, "problem in slab %s: detected write past chunk end in block %p, chunk %p",
							 name, block, chunk);
				}
			}

			/*
			 * Make sure we got the expected number of free chunks (as tracked
			 * in the block header).
			 */
			if (nfree != block->nfree)
				elog(WARNING, "problem in slab %s: nfree in block %p is %d but %d chunk were found as free",
					 name, block, block->nfree, nfree);

			nblocks++;
		}
	}

	/* the stored empty blocks are tracked in mem_allocated too */
	nblocks += dclist_count(&slab->emptyblocks);

	Assert(nblocks * slab->blockSize == context->mem_allocated);
}

#endif							/* MEMORY_CONTEXT_CHECKING */
