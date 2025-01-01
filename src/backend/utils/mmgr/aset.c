/*-------------------------------------------------------------------------
 *
 * aset.c
 *	  Allocation set definitions.
 *
 * AllocSet is our standard implementation of the abstract MemoryContext
 * type.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/mmgr/aset.c
 *
 * NOTE:
 *	This is a new (Feb. 05, 1999) implementation of the allocation set
 *	routines. AllocSet...() does not use OrderedSet...() any more.
 *	Instead it manages allocations in a block pool by itself, combining
 *	many small allocations in a few bigger blocks. AllocSetFree() normally
 *	doesn't free() memory really. It just add's the free'd area to some
 *	list for later reuse by AllocSetAlloc(). All memory blocks are free()'d
 *	at once on AllocSetReset(), which happens when the memory context gets
 *	destroyed.
 *				Jan Wieck
 *
 *	Performance improvement from Tom Lane, 8/99: for extremely large request
 *	sizes, we do want to be able to give the memory back to free() as soon
 *	as it is pfree()'d.  Otherwise we risk tying up a lot of memory in
 *	freelist entries that might never be usable.  This is specially needed
 *	when the caller is repeatedly repalloc()'ing a block bigger and bigger;
 *	the previous instances of the block were guaranteed to be wasted until
 *	AllocSetReset() under the old way.
 *
 *	Further improvement 12/00: as the code stood, request sizes in the
 *	midrange between "small" and "large" were handled very inefficiently,
 *	because any sufficiently large free chunk would be used to satisfy a
 *	request, even if it was much larger than necessary.  This led to more
 *	and more wasted space in allocated chunks over time.  To fix, get rid
 *	of the midrange behavior: we now handle only "small" power-of-2-size
 *	chunks as chunks.  Anything "large" is passed off to malloc().  Change
 *	the number of freelists to change the small/large boundary.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "port/pg_bitutils.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"
#include "utils/memutils_internal.h"
#include "utils/memutils_memorychunk.h"

/*--------------------
 * Chunk freelist k holds chunks of size 1 << (k + ALLOC_MINBITS),
 * for k = 0 .. ALLOCSET_NUM_FREELISTS-1.
 *
 * Note that all chunks in the freelists have power-of-2 sizes.  This
 * improves recyclability: we may waste some space, but the wasted space
 * should stay pretty constant as requests are made and released.
 *
 * A request too large for the last freelist is handled by allocating a
 * dedicated block from malloc().  The block still has a block header and
 * chunk header, but when the chunk is freed we'll return the whole block
 * to malloc(), not put it on our freelists.
 *
 * CAUTION: ALLOC_MINBITS must be large enough so that
 * 1<<ALLOC_MINBITS is at least MAXALIGN,
 * or we may fail to align the smallest chunks adequately.
 * 8-byte alignment is enough on all currently known machines.  This 8-byte
 * minimum also allows us to store a pointer to the next freelist item within
 * the chunk of memory itself.
 *
 * With the current parameters, request sizes up to 8K are treated as chunks,
 * larger requests go into dedicated blocks.  Change ALLOCSET_NUM_FREELISTS
 * to adjust the boundary point; and adjust ALLOCSET_SEPARATE_THRESHOLD in
 * memutils.h to agree.  (Note: in contexts with small maxBlockSize, we may
 * set the allocChunkLimit to less than 8K, so as to avoid space wastage.)
 *--------------------
 */

#define ALLOC_MINBITS		3	/* smallest chunk size is 8 bytes */
#define ALLOCSET_NUM_FREELISTS	11
#define ALLOC_CHUNK_LIMIT	(1 << (ALLOCSET_NUM_FREELISTS-1+ALLOC_MINBITS))
/* Size of largest chunk that we use a fixed size for */
#define ALLOC_CHUNK_FRACTION	4
/* We allow chunks to be at most 1/4 of maxBlockSize (less overhead) */

/*--------------------
 * The first block allocated for an allocset has size initBlockSize.
 * Each time we have to allocate another block, we double the block size
 * (if possible, and without exceeding maxBlockSize), so as to reduce
 * the bookkeeping load on malloc().
 *
 * Blocks allocated to hold oversize chunks do not follow this rule, however;
 * they are just however big they need to be to hold that single chunk.
 *
 * Also, if a minContextSize is specified, the first block has that size,
 * and then initBlockSize is used for the next one.
 *--------------------
 */

#define ALLOC_BLOCKHDRSZ	MAXALIGN(sizeof(AllocBlockData))
#define ALLOC_CHUNKHDRSZ	sizeof(MemoryChunk)

typedef struct AllocBlockData *AllocBlock;	/* forward reference */

/*
 * AllocPointer
 *		Aligned pointer which may be a member of an allocation set.
 */
typedef void *AllocPointer;

/*
 * AllocFreeListLink
 *		When pfreeing memory, if we maintain a freelist for the given chunk's
 *		size then we use a AllocFreeListLink to point to the current item in
 *		the AllocSetContext's freelist and then set the given freelist element
 *		to point to the chunk being freed.
 */
typedef struct AllocFreeListLink
{
	MemoryChunk *next;
} AllocFreeListLink;

/*
 * Obtain a AllocFreeListLink for the given chunk.  Allocation sizes are
 * always at least sizeof(AllocFreeListLink), so we reuse the pointer's memory
 * itself to store the freelist link.
 */
#define GetFreeListLink(chkptr) \
	(AllocFreeListLink *) ((char *) (chkptr) + ALLOC_CHUNKHDRSZ)

/* Validate a freelist index retrieved from a chunk header */
#define FreeListIdxIsValid(fidx) \
	((fidx) >= 0 && (fidx) < ALLOCSET_NUM_FREELISTS)

/* Determine the size of the chunk based on the freelist index */
#define GetChunkSizeFromFreeListIdx(fidx) \
	((((Size) 1) << ALLOC_MINBITS) << (fidx))

/*
 * AllocSetContext is our standard implementation of MemoryContext.
 *
 * Note: header.isReset means there is nothing for AllocSetReset to do.
 * This is different from the aset being physically empty (empty blocks list)
 * because we will still have a keeper block.  It's also different from the set
 * being logically empty, because we don't attempt to detect pfree'ing the
 * last active chunk.
 */
typedef struct AllocSetContext
{
	MemoryContextData header;	/* Standard memory-context fields */
	/* Info about storage allocated in this context: */
	AllocBlock	blocks;			/* head of list of blocks in this set */
	MemoryChunk *freelist[ALLOCSET_NUM_FREELISTS];	/* free chunk lists */
	/* Allocation parameters for this context: */
	uint32		initBlockSize;	/* initial block size */
	uint32		maxBlockSize;	/* maximum block size */
	uint32		nextBlockSize;	/* next block size to allocate */
	uint32		allocChunkLimit;	/* effective chunk size limit */
	/* freelist this context could be put in, or -1 if not a candidate: */
	int			freeListIndex;	/* index in context_freelists[], or -1 */
} AllocSetContext;

typedef AllocSetContext *AllocSet;

/*
 * AllocBlock
 *		An AllocBlock is the unit of memory that is obtained by aset.c
 *		from malloc().  It contains one or more MemoryChunks, which are
 *		the units requested by palloc() and freed by pfree(). MemoryChunks
 *		cannot be returned to malloc() individually, instead they are put
 *		on freelists by pfree() and re-used by the next palloc() that has
 *		a matching request size.
 *
 *		AllocBlockData is the header data for a block --- the usable space
 *		within the block begins at the next alignment boundary.
 */
typedef struct AllocBlockData
{
	AllocSet	aset;			/* aset that owns this block */
	AllocBlock	prev;			/* prev block in aset's blocks list, if any */
	AllocBlock	next;			/* next block in aset's blocks list, if any */
	char	   *freeptr;		/* start of free space in this block */
	char	   *endptr;			/* end of space in this block */
}			AllocBlockData;

/*
 * AllocPointerIsValid
 *		True iff pointer is valid allocation pointer.
 */
#define AllocPointerIsValid(pointer) PointerIsValid(pointer)

/*
 * AllocSetIsValid
 *		True iff set is valid allocation set.
 */
#define AllocSetIsValid(set) \
	(PointerIsValid(set) && IsA(set, AllocSetContext))

/*
 * AllocBlockIsValid
 *		True iff block is valid block of allocation set.
 */
#define AllocBlockIsValid(block) \
	(PointerIsValid(block) && AllocSetIsValid((block)->aset))

/*
 * We always store external chunks on a dedicated block.  This makes fetching
 * the block from an external chunk easy since it's always the first and only
 * chunk on the block.
 */
#define ExternalChunkGetBlock(chunk) \
	(AllocBlock) ((char *) chunk - ALLOC_BLOCKHDRSZ)

/*
 * Rather than repeatedly creating and deleting memory contexts, we keep some
 * freed contexts in freelists so that we can hand them out again with little
 * work.  Before putting a context in a freelist, we reset it so that it has
 * only its initial malloc chunk and no others.  To be a candidate for a
 * freelist, a context must have the same minContextSize/initBlockSize as
 * other contexts in the list; but its maxBlockSize is irrelevant since that
 * doesn't affect the size of the initial chunk.
 *
 * We currently provide one freelist for ALLOCSET_DEFAULT_SIZES contexts
 * and one for ALLOCSET_SMALL_SIZES contexts; the latter works for
 * ALLOCSET_START_SMALL_SIZES too, since only the maxBlockSize differs.
 *
 * Ordinarily, we re-use freelist contexts in last-in-first-out order, in
 * hopes of improving locality of reference.  But if there get to be too
 * many contexts in the list, we'd prefer to drop the most-recently-created
 * contexts in hopes of keeping the process memory map compact.
 * We approximate that by simply deleting all existing entries when the list
 * overflows, on the assumption that queries that allocate a lot of contexts
 * will probably free them in more or less reverse order of allocation.
 *
 * Contexts in a freelist are chained via their nextchild pointers.
 */
#define MAX_FREE_CONTEXTS 100	/* arbitrary limit on freelist length */

/* Obtain the keeper block for an allocation set */
#define KeeperBlock(set) \
	((AllocBlock) (((char *) set) + MAXALIGN(sizeof(AllocSetContext))))

/* Check if the block is the keeper block of the given allocation set */
#define IsKeeperBlock(set, block) ((block) == (KeeperBlock(set)))

typedef struct AllocSetFreeList
{
	int			num_free;		/* current list length */
	AllocSetContext *first_free;	/* list header */
} AllocSetFreeList;

/* context_freelists[0] is for default params, [1] for small params */
static AllocSetFreeList context_freelists[2] =
{
	{
		0, NULL
	},
	{
		0, NULL
	}
};


/* ----------
 * AllocSetFreeIndex -
 *
 *		Depending on the size of an allocation compute which freechunk
 *		list of the alloc set it belongs to.  Caller must have verified
 *		that size <= ALLOC_CHUNK_LIMIT.
 * ----------
 */
static inline int
AllocSetFreeIndex(Size size)
{
	int			idx;

	if (size > (1 << ALLOC_MINBITS))
	{
		/*----------
		 * At this point we must compute ceil(log2(size >> ALLOC_MINBITS)).
		 * This is the same as
		 *		pg_leftmost_one_pos32((size - 1) >> ALLOC_MINBITS) + 1
		 * or equivalently
		 *		pg_leftmost_one_pos32(size - 1) - ALLOC_MINBITS + 1
		 *
		 * However, for platforms without intrinsic support, we duplicate the
		 * logic here, allowing an additional optimization.  It's reasonable
		 * to assume that ALLOC_CHUNK_LIMIT fits in 16 bits, so we can unroll
		 * the byte-at-a-time loop in pg_leftmost_one_pos32 and just handle
		 * the last two bytes.
		 *
		 * Yes, this function is enough of a hot-spot to make it worth this
		 * much trouble.
		 *----------
		 */
#ifdef HAVE_BITSCAN_REVERSE
		idx = pg_leftmost_one_pos32((uint32) size - 1) - ALLOC_MINBITS + 1;
#else
		uint32		t,
					tsize;

		/* Statically assert that we only have a 16-bit input value. */
		StaticAssertDecl(ALLOC_CHUNK_LIMIT < (1 << 16),
						 "ALLOC_CHUNK_LIMIT must be less than 64kB");

		tsize = size - 1;
		t = tsize >> 8;
		idx = t ? pg_leftmost_one_pos[t] + 8 : pg_leftmost_one_pos[tsize];
		idx -= ALLOC_MINBITS - 1;
#endif

		Assert(idx < ALLOCSET_NUM_FREELISTS);
	}
	else
		idx = 0;

	return idx;
}


/*
 * Public routines
 */


/*
 * AllocSetContextCreateInternal
 *		Create a new AllocSet context.
 *
 * parent: parent context, or NULL if top-level context
 * name: name of context (must be statically allocated)
 * minContextSize: minimum context size
 * initBlockSize: initial allocation block size
 * maxBlockSize: maximum allocation block size
 *
 * Most callers should abstract the context size parameters using a macro
 * such as ALLOCSET_DEFAULT_SIZES.
 *
 * Note: don't call this directly; go through the wrapper macro
 * AllocSetContextCreate.
 */
MemoryContext
AllocSetContextCreateInternal(MemoryContext parent,
							  const char *name,
							  Size minContextSize,
							  Size initBlockSize,
							  Size maxBlockSize)
{
	int			freeListIndex;
	Size		firstBlockSize;
	AllocSet	set;
	AllocBlock	block;

	/* ensure MemoryChunk's size is properly maxaligned */
	StaticAssertDecl(ALLOC_CHUNKHDRSZ == MAXALIGN(ALLOC_CHUNKHDRSZ),
					 "sizeof(MemoryChunk) is not maxaligned");
	/* check we have enough space to store the freelist link */
	StaticAssertDecl(sizeof(AllocFreeListLink) <= (1 << ALLOC_MINBITS),
					 "sizeof(AllocFreeListLink) larger than minimum allocation size");

	/*
	 * First, validate allocation parameters.  Once these were regular runtime
	 * tests and elog's, but in practice Asserts seem sufficient because
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

	/*
	 * Check whether the parameters match either available freelist.  We do
	 * not need to demand a match of maxBlockSize.
	 */
	if (minContextSize == ALLOCSET_DEFAULT_MINSIZE &&
		initBlockSize == ALLOCSET_DEFAULT_INITSIZE)
		freeListIndex = 0;
	else if (minContextSize == ALLOCSET_SMALL_MINSIZE &&
			 initBlockSize == ALLOCSET_SMALL_INITSIZE)
		freeListIndex = 1;
	else
		freeListIndex = -1;

	/*
	 * If a suitable freelist entry exists, just recycle that context.
	 */
	if (freeListIndex >= 0)
	{
		AllocSetFreeList *freelist = &context_freelists[freeListIndex];

		if (freelist->first_free != NULL)
		{
			/* Remove entry from freelist */
			set = freelist->first_free;
			freelist->first_free = (AllocSet) set->header.nextchild;
			freelist->num_free--;

			/* Update its maxBlockSize; everything else should be OK */
			set->maxBlockSize = maxBlockSize;

			/* Reinitialize its header, installing correct name and parent */
			MemoryContextCreate((MemoryContext) set,
								T_AllocSetContext,
								MCTX_ASET_ID,
								parent,
								name);

			((MemoryContext) set)->mem_allocated =
				KeeperBlock(set)->endptr - ((char *) set);

			return (MemoryContext) set;
		}
	}

	/* Determine size of initial block */
	firstBlockSize = MAXALIGN(sizeof(AllocSetContext)) +
		ALLOC_BLOCKHDRSZ + ALLOC_CHUNKHDRSZ;
	if (minContextSize != 0)
		firstBlockSize = Max(firstBlockSize, minContextSize);
	else
		firstBlockSize = Max(firstBlockSize, initBlockSize);

	/*
	 * Allocate the initial block.  Unlike other aset.c blocks, it starts with
	 * the context header and its block header follows that.
	 */
	set = (AllocSet) malloc(firstBlockSize);
	if (set == NULL)
	{
		if (TopMemoryContext)
			MemoryContextStats(TopMemoryContext);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while creating memory context \"%s\".",
						   name)));
	}

	/*
	 * Avoid writing code that can fail between here and MemoryContextCreate;
	 * we'd leak the header/initial block if we ereport in this stretch.
	 */

	/* Fill in the initial block's block header */
	block = KeeperBlock(set);
	block->aset = set;
	block->freeptr = ((char *) block) + ALLOC_BLOCKHDRSZ;
	block->endptr = ((char *) set) + firstBlockSize;
	block->prev = NULL;
	block->next = NULL;

	/* Mark unallocated space NOACCESS; leave the block header alone. */
	VALGRIND_MAKE_MEM_NOACCESS(block->freeptr, block->endptr - block->freeptr);

	/* Remember block as part of block list */
	set->blocks = block;

	/* Finish filling in aset-specific parts of the context header */
	MemSetAligned(set->freelist, 0, sizeof(set->freelist));

	set->initBlockSize = (uint32) initBlockSize;
	set->maxBlockSize = (uint32) maxBlockSize;
	set->nextBlockSize = (uint32) initBlockSize;
	set->freeListIndex = freeListIndex;

	/*
	 * Compute the allocation chunk size limit for this context.  It can't be
	 * more than ALLOC_CHUNK_LIMIT because of the fixed number of freelists.
	 * If maxBlockSize is small then requests exceeding the maxBlockSize, or
	 * even a significant fraction of it, should be treated as large chunks
	 * too.  For the typical case of maxBlockSize a power of 2, the chunk size
	 * limit will be at most 1/8th maxBlockSize, so that given a stream of
	 * requests that are all the maximum chunk size we will waste at most
	 * 1/8th of the allocated space.
	 *
	 * Also, allocChunkLimit must not exceed ALLOCSET_SEPARATE_THRESHOLD.
	 */
	StaticAssertStmt(ALLOC_CHUNK_LIMIT == ALLOCSET_SEPARATE_THRESHOLD,
					 "ALLOC_CHUNK_LIMIT != ALLOCSET_SEPARATE_THRESHOLD");

	/*
	 * Determine the maximum size that a chunk can be before we allocate an
	 * entire AllocBlock dedicated for that chunk.  We set the absolute limit
	 * of that size as ALLOC_CHUNK_LIMIT but we reduce it further so that we
	 * can fit about ALLOC_CHUNK_FRACTION chunks this size on a maximally
	 * sized block.  (We opt to keep allocChunkLimit a power-of-2 value
	 * primarily for legacy reasons rather than calculating it so that exactly
	 * ALLOC_CHUNK_FRACTION chunks fit on a maximally sized block.)
	 */
	set->allocChunkLimit = ALLOC_CHUNK_LIMIT;
	while ((Size) (set->allocChunkLimit + ALLOC_CHUNKHDRSZ) >
		   (Size) ((maxBlockSize - ALLOC_BLOCKHDRSZ) / ALLOC_CHUNK_FRACTION))
		set->allocChunkLimit >>= 1;

	/* Finally, do the type-independent part of context creation */
	MemoryContextCreate((MemoryContext) set,
						T_AllocSetContext,
						MCTX_ASET_ID,
						parent,
						name);

	((MemoryContext) set)->mem_allocated = firstBlockSize;

	return (MemoryContext) set;
}

/*
 * AllocSetReset
 *		Frees all memory which is allocated in the given set.
 *
 * Actually, this routine has some discretion about what to do.
 * It should mark all allocated chunks freed, but it need not necessarily
 * give back all the resources the set owns.  Our actual implementation is
 * that we give back all but the "keeper" block (which we must keep, since
 * it shares a malloc chunk with the context header).  In this way, we don't
 * thrash malloc() when a context is repeatedly reset after small allocations,
 * which is typical behavior for per-tuple contexts.
 */
void
AllocSetReset(MemoryContext context)
{
	AllocSet	set = (AllocSet) context;
	AllocBlock	block;
	Size		keepersize PG_USED_FOR_ASSERTS_ONLY;

	Assert(AllocSetIsValid(set));

#ifdef MEMORY_CONTEXT_CHECKING
	/* Check for corruption and leaks before freeing */
	AllocSetCheck(context);
#endif

	/* Remember keeper block size for Assert below */
	keepersize = KeeperBlock(set)->endptr - ((char *) set);

	/* Clear chunk freelists */
	MemSetAligned(set->freelist, 0, sizeof(set->freelist));

	block = set->blocks;

	/* New blocks list will be just the keeper block */
	set->blocks = KeeperBlock(set);

	while (block != NULL)
	{
		AllocBlock	next = block->next;

		if (IsKeeperBlock(set, block))
		{
			/* Reset the block, but don't return it to malloc */
			char	   *datastart = ((char *) block) + ALLOC_BLOCKHDRSZ;

#ifdef CLOBBER_FREED_MEMORY
			wipe_mem(datastart, block->freeptr - datastart);
#else
			/* wipe_mem() would have done this */
			VALGRIND_MAKE_MEM_NOACCESS(datastart, block->freeptr - datastart);
#endif
			block->freeptr = datastart;
			block->prev = NULL;
			block->next = NULL;
		}
		else
		{
			/* Normal case, release the block */
			context->mem_allocated -= block->endptr - ((char *) block);

#ifdef CLOBBER_FREED_MEMORY
			wipe_mem(block, block->freeptr - ((char *) block));
#endif
			free(block);
		}
		block = next;
	}

	Assert(context->mem_allocated == keepersize);

	/* Reset block size allocation sequence, too */
	set->nextBlockSize = set->initBlockSize;
}

/*
 * AllocSetDelete
 *		Frees all memory which is allocated in the given set,
 *		in preparation for deletion of the set.
 *
 * Unlike AllocSetReset, this *must* free all resources of the set.
 */
void
AllocSetDelete(MemoryContext context)
{
	AllocSet	set = (AllocSet) context;
	AllocBlock	block = set->blocks;
	Size		keepersize PG_USED_FOR_ASSERTS_ONLY;

	Assert(AllocSetIsValid(set));

#ifdef MEMORY_CONTEXT_CHECKING
	/* Check for corruption and leaks before freeing */
	AllocSetCheck(context);
#endif

	/* Remember keeper block size for Assert below */
	keepersize = KeeperBlock(set)->endptr - ((char *) set);

	/*
	 * If the context is a candidate for a freelist, put it into that freelist
	 * instead of destroying it.
	 */
	if (set->freeListIndex >= 0)
	{
		AllocSetFreeList *freelist = &context_freelists[set->freeListIndex];

		/*
		 * Reset the context, if it needs it, so that we aren't hanging on to
		 * more than the initial malloc chunk.
		 */
		if (!context->isReset)
			MemoryContextResetOnly(context);

		/*
		 * If the freelist is full, just discard what's already in it.  See
		 * comments with context_freelists[].
		 */
		if (freelist->num_free >= MAX_FREE_CONTEXTS)
		{
			while (freelist->first_free != NULL)
			{
				AllocSetContext *oldset = freelist->first_free;

				freelist->first_free = (AllocSetContext *) oldset->header.nextchild;
				freelist->num_free--;

				/* All that remains is to free the header/initial block */
				free(oldset);
			}
			Assert(freelist->num_free == 0);
		}

		/* Now add the just-deleted context to the freelist. */
		set->header.nextchild = (MemoryContext) freelist->first_free;
		freelist->first_free = set;
		freelist->num_free++;

		return;
	}

	/* Free all blocks, except the keeper which is part of context header */
	while (block != NULL)
	{
		AllocBlock	next = block->next;

		if (!IsKeeperBlock(set, block))
			context->mem_allocated -= block->endptr - ((char *) block);

#ifdef CLOBBER_FREED_MEMORY
		wipe_mem(block, block->freeptr - ((char *) block));
#endif

		if (!IsKeeperBlock(set, block))
			free(block);

		block = next;
	}

	Assert(context->mem_allocated == keepersize);

	/* Finally, free the context header, including the keeper block */
	free(set);
}

/*
 * Helper for AllocSetAlloc() that allocates an entire block for the chunk.
 *
 * AllocSetAlloc()'s comment explains why this is separate.
 */
pg_noinline
static void *
AllocSetAllocLarge(MemoryContext context, Size size, int flags)
{
	AllocSet	set = (AllocSet) context;
	AllocBlock	block;
	MemoryChunk *chunk;
	Size		chunk_size;
	Size		blksize;

	/* validate 'size' is within the limits for the given 'flags' */
	MemoryContextCheckSize(context, size, flags);

#ifdef MEMORY_CONTEXT_CHECKING
	/* ensure there's always space for the sentinel byte */
	chunk_size = MAXALIGN(size + 1);
#else
	chunk_size = MAXALIGN(size);
#endif

	blksize = chunk_size + ALLOC_BLOCKHDRSZ + ALLOC_CHUNKHDRSZ;
	block = (AllocBlock) malloc(blksize);
	if (block == NULL)
		return MemoryContextAllocationFailure(context, size, flags);

	context->mem_allocated += blksize;

	block->aset = set;
	block->freeptr = block->endptr = ((char *) block) + blksize;

	chunk = (MemoryChunk *) (((char *) block) + ALLOC_BLOCKHDRSZ);

	/* mark the MemoryChunk as externally managed */
	MemoryChunkSetHdrMaskExternal(chunk, MCTX_ASET_ID);

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

	/*
	 * Stick the new block underneath the active allocation block, if any, so
	 * that we don't lose the use of the space remaining therein.
	 */
	if (set->blocks != NULL)
	{
		block->prev = set->blocks;
		block->next = set->blocks->next;
		if (block->next)
			block->next->prev = block;
		set->blocks->next = block;
	}
	else
	{
		block->prev = NULL;
		block->next = NULL;
		set->blocks = block;
	}

	/* Ensure any padding bytes are marked NOACCESS. */
	VALGRIND_MAKE_MEM_NOACCESS((char *) MemoryChunkGetPointer(chunk) + size,
							   chunk_size - size);

	/* Disallow access to the chunk header. */
	VALGRIND_MAKE_MEM_NOACCESS(chunk, ALLOC_CHUNKHDRSZ);

	return MemoryChunkGetPointer(chunk);
}

/*
 * Small helper for allocating a new chunk from a chunk, to avoid duplicating
 * the code between AllocSetAlloc() and AllocSetAllocFromNewBlock().
 */
static inline void *
AllocSetAllocChunkFromBlock(MemoryContext context, AllocBlock block,
							Size size, Size chunk_size, int fidx)
{
	MemoryChunk *chunk;

	chunk = (MemoryChunk *) (block->freeptr);

	/* Prepare to initialize the chunk header. */
	VALGRIND_MAKE_MEM_UNDEFINED(chunk, ALLOC_CHUNKHDRSZ);

	block->freeptr += (chunk_size + ALLOC_CHUNKHDRSZ);
	Assert(block->freeptr <= block->endptr);

	/* store the free list index in the value field */
	MemoryChunkSetHdrMask(chunk, block, fidx, MCTX_ASET_ID);

#ifdef MEMORY_CONTEXT_CHECKING
	chunk->requested_size = size;
	/* set mark to catch clobber of "unused" space */
	if (size < chunk_size)
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
	VALGRIND_MAKE_MEM_NOACCESS(chunk, ALLOC_CHUNKHDRSZ);

	return MemoryChunkGetPointer(chunk);
}

/*
 * Helper for AllocSetAlloc() that allocates a new block and returns a chunk
 * allocated from it.
 *
 * AllocSetAlloc()'s comment explains why this is separate.
 */
pg_noinline
static void *
AllocSetAllocFromNewBlock(MemoryContext context, Size size, int flags,
						  int fidx)
{
	AllocSet	set = (AllocSet) context;
	AllocBlock	block;
	Size		availspace;
	Size		blksize;
	Size		required_size;
	Size		chunk_size;

	/* due to the keeper block set->blocks should always be valid */
	Assert(set->blocks != NULL);
	block = set->blocks;
	availspace = block->endptr - block->freeptr;

	/*
	 * The existing active (top) block does not have enough room for the
	 * requested allocation, but it might still have a useful amount of space
	 * in it.  Once we push it down in the block list, we'll never try to
	 * allocate more space from it. So, before we do that, carve up its free
	 * space into chunks that we can put on the set's freelists.
	 *
	 * Because we can only get here when there's less than ALLOC_CHUNK_LIMIT
	 * left in the block, this loop cannot iterate more than
	 * ALLOCSET_NUM_FREELISTS-1 times.
	 */
	while (availspace >= ((1 << ALLOC_MINBITS) + ALLOC_CHUNKHDRSZ))
	{
		AllocFreeListLink *link;
		MemoryChunk *chunk;
		Size		availchunk = availspace - ALLOC_CHUNKHDRSZ;
		int			a_fidx = AllocSetFreeIndex(availchunk);

		/*
		 * In most cases, we'll get back the index of the next larger freelist
		 * than the one we need to put this chunk on.  The exception is when
		 * availchunk is exactly a power of 2.
		 */
		if (availchunk != GetChunkSizeFromFreeListIdx(a_fidx))
		{
			a_fidx--;
			Assert(a_fidx >= 0);
			availchunk = GetChunkSizeFromFreeListIdx(a_fidx);
		}

		chunk = (MemoryChunk *) (block->freeptr);

		/* Prepare to initialize the chunk header. */
		VALGRIND_MAKE_MEM_UNDEFINED(chunk, ALLOC_CHUNKHDRSZ);
		block->freeptr += (availchunk + ALLOC_CHUNKHDRSZ);
		availspace -= (availchunk + ALLOC_CHUNKHDRSZ);

		/* store the freelist index in the value field */
		MemoryChunkSetHdrMask(chunk, block, a_fidx, MCTX_ASET_ID);
#ifdef MEMORY_CONTEXT_CHECKING
		chunk->requested_size = InvalidAllocSize;	/* mark it free */
#endif
		/* push this chunk onto the free list */
		link = GetFreeListLink(chunk);

		VALGRIND_MAKE_MEM_DEFINED(link, sizeof(AllocFreeListLink));
		link->next = set->freelist[a_fidx];
		VALGRIND_MAKE_MEM_NOACCESS(link, sizeof(AllocFreeListLink));

		set->freelist[a_fidx] = chunk;
	}

	/*
	 * The first such block has size initBlockSize, and we double the space in
	 * each succeeding block, but not more than maxBlockSize.
	 */
	blksize = set->nextBlockSize;
	set->nextBlockSize <<= 1;
	if (set->nextBlockSize > set->maxBlockSize)
		set->nextBlockSize = set->maxBlockSize;

	/* Choose the actual chunk size to allocate */
	chunk_size = GetChunkSizeFromFreeListIdx(fidx);
	Assert(chunk_size >= size);

	/*
	 * If initBlockSize is less than ALLOC_CHUNK_LIMIT, we could need more
	 * space... but try to keep it a power of 2.
	 */
	required_size = chunk_size + ALLOC_BLOCKHDRSZ + ALLOC_CHUNKHDRSZ;
	while (blksize < required_size)
		blksize <<= 1;

	/* Try to allocate it */
	block = (AllocBlock) malloc(blksize);

	/*
	 * We could be asking for pretty big blocks here, so cope if malloc fails.
	 * But give up if there's less than 1 MB or so available...
	 */
	while (block == NULL && blksize > 1024 * 1024)
	{
		blksize >>= 1;
		if (blksize < required_size)
			break;
		block = (AllocBlock) malloc(blksize);
	}

	if (block == NULL)
		return MemoryContextAllocationFailure(context, size, flags);

	context->mem_allocated += blksize;

	block->aset = set;
	block->freeptr = ((char *) block) + ALLOC_BLOCKHDRSZ;
	block->endptr = ((char *) block) + blksize;

	/* Mark unallocated space NOACCESS. */
	VALGRIND_MAKE_MEM_NOACCESS(block->freeptr,
							   blksize - ALLOC_BLOCKHDRSZ);

	block->prev = NULL;
	block->next = set->blocks;
	if (block->next)
		block->next->prev = block;
	set->blocks = block;

	return AllocSetAllocChunkFromBlock(context, block, size, chunk_size, fidx);
}

/*
 * AllocSetAlloc
 *		Returns a pointer to allocated memory of given size or raises an ERROR
 *		on allocation failure, or returns NULL when flags contains
 *		MCXT_ALLOC_NO_OOM.
 *
 * No request may exceed:
 *		MAXALIGN_DOWN(SIZE_MAX) - ALLOC_BLOCKHDRSZ - ALLOC_CHUNKHDRSZ
 * All callers use a much-lower limit.
 *
 * Note: when using valgrind, it doesn't matter how the returned allocation
 * is marked, as mcxt.c will set it to UNDEFINED.  In some paths we will
 * return space that is marked NOACCESS - AllocSetRealloc has to beware!
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
AllocSetAlloc(MemoryContext context, Size size, int flags)
{
	AllocSet	set = (AllocSet) context;
	AllocBlock	block;
	MemoryChunk *chunk;
	int			fidx;
	Size		chunk_size;
	Size		availspace;

	Assert(AllocSetIsValid(set));

	/* due to the keeper block set->blocks should never be NULL */
	Assert(set->blocks != NULL);

	/*
	 * If requested size exceeds maximum for chunks we hand the request off to
	 * AllocSetAllocLarge().
	 */
	if (size > set->allocChunkLimit)
		return AllocSetAllocLarge(context, size, flags);

	/*
	 * Request is small enough to be treated as a chunk.  Look in the
	 * corresponding free list to see if there is a free chunk we could reuse.
	 * If one is found, remove it from the free list, make it again a member
	 * of the alloc set and return its data address.
	 *
	 * Note that we don't attempt to ensure there's space for the sentinel
	 * byte here.  We expect a large proportion of allocations to be for sizes
	 * which are already a power of 2.  If we were to always make space for a
	 * sentinel byte in MEMORY_CONTEXT_CHECKING builds, then we'd end up
	 * doubling the memory requirements for such allocations.
	 */
	fidx = AllocSetFreeIndex(size);
	chunk = set->freelist[fidx];
	if (chunk != NULL)
	{
		AllocFreeListLink *link = GetFreeListLink(chunk);

		/* Allow access to the chunk header. */
		VALGRIND_MAKE_MEM_DEFINED(chunk, ALLOC_CHUNKHDRSZ);

		Assert(fidx == MemoryChunkGetValue(chunk));

		/* pop this chunk off the freelist */
		VALGRIND_MAKE_MEM_DEFINED(link, sizeof(AllocFreeListLink));
		set->freelist[fidx] = link->next;
		VALGRIND_MAKE_MEM_NOACCESS(link, sizeof(AllocFreeListLink));

#ifdef MEMORY_CONTEXT_CHECKING
		chunk->requested_size = size;
		/* set mark to catch clobber of "unused" space */
		if (size < GetChunkSizeFromFreeListIdx(fidx))
			set_sentinel(MemoryChunkGetPointer(chunk), size);
#endif
#ifdef RANDOMIZE_ALLOCATED_MEMORY
		/* fill the allocated space with junk */
		randomize_mem((char *) MemoryChunkGetPointer(chunk), size);
#endif

		/* Ensure any padding bytes are marked NOACCESS. */
		VALGRIND_MAKE_MEM_NOACCESS((char *) MemoryChunkGetPointer(chunk) + size,
								   GetChunkSizeFromFreeListIdx(fidx) - size);

		/* Disallow access to the chunk header. */
		VALGRIND_MAKE_MEM_NOACCESS(chunk, ALLOC_CHUNKHDRSZ);

		return MemoryChunkGetPointer(chunk);
	}

	/*
	 * Choose the actual chunk size to allocate.
	 */
	chunk_size = GetChunkSizeFromFreeListIdx(fidx);
	Assert(chunk_size >= size);

	block = set->blocks;
	availspace = block->endptr - block->freeptr;

	/*
	 * If there is enough room in the active allocation block, we will put the
	 * chunk into that block.  Else must start a new one.
	 */
	if (unlikely(availspace < (chunk_size + ALLOC_CHUNKHDRSZ)))
		return AllocSetAllocFromNewBlock(context, size, flags, fidx);

	/* There's enough space on the current block, so allocate from that */
	return AllocSetAllocChunkFromBlock(context, block, size, chunk_size, fidx);
}

/*
 * AllocSetFree
 *		Frees allocated memory; memory is removed from the set.
 */
void
AllocSetFree(void *pointer)
{
	AllocSet	set;
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);

	/* Allow access to the chunk header. */
	VALGRIND_MAKE_MEM_DEFINED(chunk, ALLOC_CHUNKHDRSZ);

	if (MemoryChunkIsExternal(chunk))
	{
		/* Release single-chunk block. */
		AllocBlock	block = ExternalChunkGetBlock(chunk);

		/*
		 * Try to verify that we have a sane block pointer: the block header
		 * should reference an aset and the freeptr should match the endptr.
		 */
		if (!AllocBlockIsValid(block) || block->freeptr != block->endptr)
			elog(ERROR, "could not find block containing chunk %p", chunk);

		set = block->aset;

#ifdef MEMORY_CONTEXT_CHECKING
		{
			/* Test for someone scribbling on unused space in chunk */
			Assert(chunk->requested_size < (block->endptr - (char *) pointer));
			if (!sentinel_ok(pointer, chunk->requested_size))
				elog(WARNING, "detected write past chunk end in %s %p",
					 set->header.name, chunk);
		}
#endif

		/* OK, remove block from aset's list and free it */
		if (block->prev)
			block->prev->next = block->next;
		else
			set->blocks = block->next;
		if (block->next)
			block->next->prev = block->prev;

		set->header.mem_allocated -= block->endptr - ((char *) block);

#ifdef CLOBBER_FREED_MEMORY
		wipe_mem(block, block->freeptr - ((char *) block));
#endif
		free(block);
	}
	else
	{
		AllocBlock	block = MemoryChunkGetBlock(chunk);
		int			fidx;
		AllocFreeListLink *link;

		/*
		 * In this path, for speed reasons we just Assert that the referenced
		 * block is good.  We can also Assert that the value field is sane.
		 * Future field experience may show that these Asserts had better
		 * become regular runtime test-and-elog checks.
		 */
		Assert(AllocBlockIsValid(block));
		set = block->aset;

		fidx = MemoryChunkGetValue(chunk);
		Assert(FreeListIdxIsValid(fidx));
		link = GetFreeListLink(chunk);

#ifdef MEMORY_CONTEXT_CHECKING
		/* Test for someone scribbling on unused space in chunk */
		if (chunk->requested_size < GetChunkSizeFromFreeListIdx(fidx))
			if (!sentinel_ok(pointer, chunk->requested_size))
				elog(WARNING, "detected write past chunk end in %s %p",
					 set->header.name, chunk);
#endif

#ifdef CLOBBER_FREED_MEMORY
		wipe_mem(pointer, GetChunkSizeFromFreeListIdx(fidx));
#endif
		/* push this chunk onto the top of the free list */
		VALGRIND_MAKE_MEM_DEFINED(link, sizeof(AllocFreeListLink));
		link->next = set->freelist[fidx];
		VALGRIND_MAKE_MEM_NOACCESS(link, sizeof(AllocFreeListLink));
		set->freelist[fidx] = chunk;

#ifdef MEMORY_CONTEXT_CHECKING

		/*
		 * Reset requested_size to InvalidAllocSize in chunks that are on free
		 * list.
		 */
		chunk->requested_size = InvalidAllocSize;
#endif
	}
}

/*
 * AllocSetRealloc
 *		Returns new pointer to allocated memory of given size or NULL if
 *		request could not be completed; this memory is added to the set.
 *		Memory associated with given pointer is copied into the new memory,
 *		and the old memory is freed.
 *
 * Without MEMORY_CONTEXT_CHECKING, we don't know the old request size.  This
 * makes our Valgrind client requests less-precise, hazarding false negatives.
 * (In principle, we could use VALGRIND_GET_VBITS() to rediscover the old
 * request size.)
 */
void *
AllocSetRealloc(void *pointer, Size size, int flags)
{
	AllocBlock	block;
	AllocSet	set;
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	Size		oldchksize;
	int			fidx;

	/* Allow access to the chunk header. */
	VALGRIND_MAKE_MEM_DEFINED(chunk, ALLOC_CHUNKHDRSZ);

	if (MemoryChunkIsExternal(chunk))
	{
		/*
		 * The chunk must have been allocated as a single-chunk block.  Use
		 * realloc() to make the containing block bigger, or smaller, with
		 * minimum space wastage.
		 */
		Size		chksize;
		Size		blksize;
		Size		oldblksize;

		block = ExternalChunkGetBlock(chunk);

		/*
		 * Try to verify that we have a sane block pointer: the block header
		 * should reference an aset and the freeptr should match the endptr.
		 */
		if (!AllocBlockIsValid(block) || block->freeptr != block->endptr)
			elog(ERROR, "could not find block containing chunk %p", chunk);

		set = block->aset;

		/* only check size in paths where the limits could be hit */
		MemoryContextCheckSize((MemoryContext) set, size, flags);

		oldchksize = block->endptr - (char *) pointer;

#ifdef MEMORY_CONTEXT_CHECKING
		/* Test for someone scribbling on unused space in chunk */
		Assert(chunk->requested_size < oldchksize);
		if (!sentinel_ok(pointer, chunk->requested_size))
			elog(WARNING, "detected write past chunk end in %s %p",
				 set->header.name, chunk);
#endif

#ifdef MEMORY_CONTEXT_CHECKING
		/* ensure there's always space for the sentinel byte */
		chksize = MAXALIGN(size + 1);
#else
		chksize = MAXALIGN(size);
#endif

		/* Do the realloc */
		blksize = chksize + ALLOC_BLOCKHDRSZ + ALLOC_CHUNKHDRSZ;
		oldblksize = block->endptr - ((char *) block);

		block = (AllocBlock) realloc(block, blksize);
		if (block == NULL)
		{
			/* Disallow access to the chunk header. */
			VALGRIND_MAKE_MEM_NOACCESS(chunk, ALLOC_CHUNKHDRSZ);
			return MemoryContextAllocationFailure(&set->header, size, flags);
		}

		/* updated separately, not to underflow when (oldblksize > blksize) */
		set->header.mem_allocated -= oldblksize;
		set->header.mem_allocated += blksize;

		block->freeptr = block->endptr = ((char *) block) + blksize;

		/* Update pointers since block has likely been moved */
		chunk = (MemoryChunk *) (((char *) block) + ALLOC_BLOCKHDRSZ);
		pointer = MemoryChunkGetPointer(chunk);
		if (block->prev)
			block->prev->next = block;
		else
			set->blocks = block;
		if (block->next)
			block->next->prev = block;

#ifdef MEMORY_CONTEXT_CHECKING
#ifdef RANDOMIZE_ALLOCATED_MEMORY

		/*
		 * We can only randomize the extra space if we know the prior request.
		 * When using Valgrind, randomize_mem() also marks memory UNDEFINED.
		 */
		if (size > chunk->requested_size)
			randomize_mem((char *) pointer + chunk->requested_size,
						  size - chunk->requested_size);
#else

		/*
		 * If this is an increase, realloc() will have marked any
		 * newly-allocated part (from oldchksize to chksize) UNDEFINED, but we
		 * also need to adjust trailing bytes from the old allocation (from
		 * chunk->requested_size to oldchksize) as they are marked NOACCESS.
		 * Make sure not to mark too many bytes in case chunk->requested_size
		 * < size < oldchksize.
		 */
#ifdef USE_VALGRIND
		if (Min(size, oldchksize) > chunk->requested_size)
			VALGRIND_MAKE_MEM_UNDEFINED((char *) pointer + chunk->requested_size,
										Min(size, oldchksize) - chunk->requested_size);
#endif
#endif

		chunk->requested_size = size;
		/* set mark to catch clobber of "unused" space */
		Assert(size < chksize);
		set_sentinel(pointer, size);
#else							/* !MEMORY_CONTEXT_CHECKING */

		/*
		 * We may need to adjust marking of bytes from the old allocation as
		 * some of them may be marked NOACCESS.  We don't know how much of the
		 * old chunk size was the requested size; it could have been as small
		 * as one byte.  We have to be conservative and just mark the entire
		 * old portion DEFINED.  Make sure not to mark memory beyond the new
		 * allocation in case it's smaller than the old one.
		 */
		VALGRIND_MAKE_MEM_DEFINED(pointer, Min(size, oldchksize));
#endif

		/* Ensure any padding bytes are marked NOACCESS. */
		VALGRIND_MAKE_MEM_NOACCESS((char *) pointer + size, chksize - size);

		/* Disallow access to the chunk header . */
		VALGRIND_MAKE_MEM_NOACCESS(chunk, ALLOC_CHUNKHDRSZ);

		return pointer;
	}

	block = MemoryChunkGetBlock(chunk);

	/*
	 * In this path, for speed reasons we just Assert that the referenced
	 * block is good. We can also Assert that the value field is sane. Future
	 * field experience may show that these Asserts had better become regular
	 * runtime test-and-elog checks.
	 */
	Assert(AllocBlockIsValid(block));
	set = block->aset;

	fidx = MemoryChunkGetValue(chunk);
	Assert(FreeListIdxIsValid(fidx));
	oldchksize = GetChunkSizeFromFreeListIdx(fidx);

#ifdef MEMORY_CONTEXT_CHECKING
	/* Test for someone scribbling on unused space in chunk */
	if (chunk->requested_size < oldchksize)
		if (!sentinel_ok(pointer, chunk->requested_size))
			elog(WARNING, "detected write past chunk end in %s %p",
				 set->header.name, chunk);
#endif

	/*
	 * Chunk sizes are aligned to power of 2 in AllocSetAlloc().  Maybe the
	 * allocated area already is >= the new size.  (In particular, we will
	 * fall out here if the requested size is a decrease.)
	 */
	if (oldchksize >= size)
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
									   oldchksize - size);

		/* set mark to catch clobber of "unused" space */
		if (size < oldchksize)
			set_sentinel(pointer, size);
#else							/* !MEMORY_CONTEXT_CHECKING */

		/*
		 * We don't have the information to determine whether we're growing
		 * the old request or shrinking it, so we conservatively mark the
		 * entire new allocation DEFINED.
		 */
		VALGRIND_MAKE_MEM_NOACCESS(pointer, oldchksize);
		VALGRIND_MAKE_MEM_DEFINED(pointer, size);
#endif

		/* Disallow access to the chunk header. */
		VALGRIND_MAKE_MEM_NOACCESS(chunk, ALLOC_CHUNKHDRSZ);

		return pointer;
	}
	else
	{
		/*
		 * Enlarge-a-small-chunk case.  We just do this by brute force, ie,
		 * allocate a new chunk and copy the data.  Since we know the existing
		 * data isn't huge, this won't involve any great memcpy expense, so
		 * it's not worth being smarter.  (At one time we tried to avoid
		 * memcpy when it was possible to enlarge the chunk in-place, but that
		 * turns out to misbehave unpleasantly for repeated cycles of
		 * palloc/repalloc/pfree: the eventually freed chunks go into the
		 * wrong freelist for the next initial palloc request, and so we leak
		 * memory indefinitely.  See pgsql-hackers archives for 2007-08-11.)
		 */
		AllocPointer newPointer;
		Size		oldsize;

		/* allocate new chunk (this also checks size is valid) */
		newPointer = AllocSetAlloc((MemoryContext) set, size, flags);

		/* leave immediately if request was not completed */
		if (newPointer == NULL)
		{
			/* Disallow access to the chunk header. */
			VALGRIND_MAKE_MEM_NOACCESS(chunk, ALLOC_CHUNKHDRSZ);
			return MemoryContextAllocationFailure((MemoryContext) set, size, flags);
		}

		/*
		 * AllocSetAlloc() may have returned a region that is still NOACCESS.
		 * Change it to UNDEFINED for the moment; memcpy() will then transfer
		 * definedness from the old allocation to the new.  If we know the old
		 * allocation, copy just that much.  Otherwise, make the entire old
		 * chunk defined to avoid errors as we copy the currently-NOACCESS
		 * trailing bytes.
		 */
		VALGRIND_MAKE_MEM_UNDEFINED(newPointer, size);
#ifdef MEMORY_CONTEXT_CHECKING
		oldsize = chunk->requested_size;
#else
		oldsize = oldchksize;
		VALGRIND_MAKE_MEM_DEFINED(pointer, oldsize);
#endif

		/* transfer existing data (certain to fit) */
		memcpy(newPointer, pointer, oldsize);

		/* free old chunk */
		AllocSetFree(pointer);

		return newPointer;
	}
}

/*
 * AllocSetGetChunkContext
 *		Return the MemoryContext that 'pointer' belongs to.
 */
MemoryContext
AllocSetGetChunkContext(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	AllocBlock	block;
	AllocSet	set;

	/* Allow access to the chunk header. */
	VALGRIND_MAKE_MEM_DEFINED(chunk, ALLOC_CHUNKHDRSZ);

	if (MemoryChunkIsExternal(chunk))
		block = ExternalChunkGetBlock(chunk);
	else
		block = (AllocBlock) MemoryChunkGetBlock(chunk);

	/* Disallow access to the chunk header. */
	VALGRIND_MAKE_MEM_NOACCESS(chunk, ALLOC_CHUNKHDRSZ);

	Assert(AllocBlockIsValid(block));
	set = block->aset;

	return &set->header;
}

/*
 * AllocSetGetChunkSpace
 *		Given a currently-allocated chunk, determine the total space
 *		it occupies (including all memory-allocation overhead).
 */
Size
AllocSetGetChunkSpace(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	int			fidx;

	/* Allow access to the chunk header. */
	VALGRIND_MAKE_MEM_DEFINED(chunk, ALLOC_CHUNKHDRSZ);

	if (MemoryChunkIsExternal(chunk))
	{
		AllocBlock	block = ExternalChunkGetBlock(chunk);

		/* Disallow access to the chunk header. */
		VALGRIND_MAKE_MEM_NOACCESS(chunk, ALLOC_CHUNKHDRSZ);

		Assert(AllocBlockIsValid(block));

		return block->endptr - (char *) chunk;
	}

	fidx = MemoryChunkGetValue(chunk);
	Assert(FreeListIdxIsValid(fidx));

	/* Disallow access to the chunk header. */
	VALGRIND_MAKE_MEM_NOACCESS(chunk, ALLOC_CHUNKHDRSZ);

	return GetChunkSizeFromFreeListIdx(fidx) + ALLOC_CHUNKHDRSZ;
}

/*
 * AllocSetIsEmpty
 *		Is an allocset empty of any allocated space?
 */
bool
AllocSetIsEmpty(MemoryContext context)
{
	Assert(AllocSetIsValid(context));

	/*
	 * For now, we say "empty" only if the context is new or just reset. We
	 * could examine the freelists to determine if all space has been freed,
	 * but it's not really worth the trouble for present uses of this
	 * functionality.
	 */
	if (context->isReset)
		return true;
	return false;
}

/*
 * AllocSetStats
 *		Compute stats about memory consumption of an allocset.
 *
 * printfunc: if not NULL, pass a human-readable stats string to this.
 * passthru: pass this pointer through to printfunc.
 * totals: if not NULL, add stats about this context into *totals.
 * print_to_stderr: print stats to stderr if true, elog otherwise.
 */
void
AllocSetStats(MemoryContext context,
			  MemoryStatsPrintFunc printfunc, void *passthru,
			  MemoryContextCounters *totals, bool print_to_stderr)
{
	AllocSet	set = (AllocSet) context;
	Size		nblocks = 0;
	Size		freechunks = 0;
	Size		totalspace;
	Size		freespace = 0;
	AllocBlock	block;
	int			fidx;

	Assert(AllocSetIsValid(set));

	/* Include context header in totalspace */
	totalspace = MAXALIGN(sizeof(AllocSetContext));

	for (block = set->blocks; block != NULL; block = block->next)
	{
		nblocks++;
		totalspace += block->endptr - ((char *) block);
		freespace += block->endptr - block->freeptr;
	}
	for (fidx = 0; fidx < ALLOCSET_NUM_FREELISTS; fidx++)
	{
		Size		chksz = GetChunkSizeFromFreeListIdx(fidx);
		MemoryChunk *chunk = set->freelist[fidx];

		while (chunk != NULL)
		{
			AllocFreeListLink *link = GetFreeListLink(chunk);

			/* Allow access to the chunk header. */
			VALGRIND_MAKE_MEM_DEFINED(chunk, ALLOC_CHUNKHDRSZ);
			Assert(MemoryChunkGetValue(chunk) == fidx);
			VALGRIND_MAKE_MEM_NOACCESS(chunk, ALLOC_CHUNKHDRSZ);

			freechunks++;
			freespace += chksz + ALLOC_CHUNKHDRSZ;

			VALGRIND_MAKE_MEM_DEFINED(link, sizeof(AllocFreeListLink));
			chunk = link->next;
			VALGRIND_MAKE_MEM_NOACCESS(link, sizeof(AllocFreeListLink));
		}
	}

	if (printfunc)
	{
		char		stats_string[200];

		snprintf(stats_string, sizeof(stats_string),
				 "%zu total in %zu blocks; %zu free (%zu chunks); %zu used",
				 totalspace, nblocks, freespace, freechunks,
				 totalspace - freespace);
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
 * AllocSetCheck
 *		Walk through chunks and check consistency of memory.
 *
 * NOTE: report errors as WARNING, *not* ERROR or FATAL.  Otherwise you'll
 * find yourself in an infinite loop when trouble occurs, because this
 * routine will be entered again when elog cleanup tries to release memory!
 */
void
AllocSetCheck(MemoryContext context)
{
	AllocSet	set = (AllocSet) context;
	const char *name = set->header.name;
	AllocBlock	prevblock;
	AllocBlock	block;
	Size		total_allocated = 0;

	for (prevblock = NULL, block = set->blocks;
		 block != NULL;
		 prevblock = block, block = block->next)
	{
		char	   *bpoz = ((char *) block) + ALLOC_BLOCKHDRSZ;
		long		blk_used = block->freeptr - bpoz;
		long		blk_data = 0;
		long		nchunks = 0;
		bool		has_external_chunk = false;

		if (IsKeeperBlock(set, block))
			total_allocated += block->endptr - ((char *) set);
		else
			total_allocated += block->endptr - ((char *) block);

		/*
		 * Empty block - empty can be keeper-block only
		 */
		if (!blk_used)
		{
			if (!IsKeeperBlock(set, block))
				elog(WARNING, "problem in alloc set %s: empty block %p",
					 name, block);
		}

		/*
		 * Check block header fields
		 */
		if (block->aset != set ||
			block->prev != prevblock ||
			block->freeptr < bpoz ||
			block->freeptr > block->endptr)
			elog(WARNING, "problem in alloc set %s: corrupt header in block %p",
				 name, block);

		/*
		 * Chunk walker
		 */
		while (bpoz < block->freeptr)
		{
			MemoryChunk *chunk = (MemoryChunk *) bpoz;
			Size		chsize,
						dsize;

			/* Allow access to the chunk header. */
			VALGRIND_MAKE_MEM_DEFINED(chunk, ALLOC_CHUNKHDRSZ);

			if (MemoryChunkIsExternal(chunk))
			{
				chsize = block->endptr - (char *) MemoryChunkGetPointer(chunk); /* aligned chunk size */
				has_external_chunk = true;

				/* make sure this chunk consumes the entire block */
				if (chsize + ALLOC_CHUNKHDRSZ != blk_used)
					elog(WARNING, "problem in alloc set %s: bad single-chunk %p in block %p",
						 name, chunk, block);
			}
			else
			{
				int			fidx = MemoryChunkGetValue(chunk);

				if (!FreeListIdxIsValid(fidx))
					elog(WARNING, "problem in alloc set %s: bad chunk size for chunk %p in block %p",
						 name, chunk, block);

				chsize = GetChunkSizeFromFreeListIdx(fidx); /* aligned chunk size */

				/*
				 * Check the stored block offset correctly references this
				 * block.
				 */
				if (block != MemoryChunkGetBlock(chunk))
					elog(WARNING, "problem in alloc set %s: bad block offset for chunk %p in block %p",
						 name, chunk, block);
			}
			dsize = chunk->requested_size;	/* real data */

			/* an allocated chunk's requested size must be <= the chsize */
			if (dsize != InvalidAllocSize && dsize > chsize)
				elog(WARNING, "problem in alloc set %s: req size > alloc size for chunk %p in block %p",
					 name, chunk, block);

			/* chsize must not be smaller than the first freelist's size */
			if (chsize < (1 << ALLOC_MINBITS))
				elog(WARNING, "problem in alloc set %s: bad size %zu for chunk %p in block %p",
					 name, chsize, chunk, block);

			/*
			 * Check for overwrite of padding space in an allocated chunk.
			 */
			if (dsize != InvalidAllocSize && dsize < chsize &&
				!sentinel_ok(chunk, ALLOC_CHUNKHDRSZ + dsize))
				elog(WARNING, "problem in alloc set %s: detected write past chunk end in block %p, chunk %p",
					 name, block, chunk);

			/* if chunk is allocated, disallow access to the chunk header */
			if (dsize != InvalidAllocSize)
				VALGRIND_MAKE_MEM_NOACCESS(chunk, ALLOC_CHUNKHDRSZ);

			blk_data += chsize;
			nchunks++;

			bpoz += ALLOC_CHUNKHDRSZ + chsize;
		}

		if ((blk_data + (nchunks * ALLOC_CHUNKHDRSZ)) != blk_used)
			elog(WARNING, "problem in alloc set %s: found inconsistent memory block %p",
				 name, block);

		if (has_external_chunk && nchunks > 1)
			elog(WARNING, "problem in alloc set %s: external chunk on non-dedicated block %p",
				 name, block);
	}

	Assert(total_allocated == context->mem_allocated);
}

#endif							/* MEMORY_CONTEXT_CHECKING */
