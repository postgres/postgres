/*-------------------------------------------------------------------------
 *
 * aset.c
 *	  Allocation set definitions.
 *
 * AllocSet is our standard implementation of the abstract MemoryContext
 * type.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/mmgr/aset.c,v 1.53 2003/09/13 22:25:38 tgl Exp $
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
 *	chunks as chunks.  Anything "large" is passed off to malloc().	Change
 *	the number of freelists to change the small/large boundary.
 *
 *
 *	About CLOBBER_FREED_MEMORY:
 *
 *	If this symbol is defined, all freed memory is overwritten with 0x7F's.
 *	This is useful for catching places that reference already-freed memory.
 *
 *	About MEMORY_CONTEXT_CHECKING:
 *
 *	Since we usually round request sizes up to the next power of 2, there
 *	is often some unused space immediately after a requested data area.
 *	Thus, if someone makes the common error of writing past what they've
 *	requested, the problem is likely to go unnoticed ... until the day when
 *	there *isn't* any wasted space, perhaps because of different memory
 *	alignment on a new platform, or some other effect.	To catch this sort
 *	of problem, the MEMORY_CONTEXT_CHECKING option stores 0x7E just beyond
 *	the requested space whenever the request is less than the actual chunk
 *	size, and verifies that the byte is undamaged when the chunk is freed.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/memutils.h"

/* Define this to detail debug alloc information */
/* #define HAVE_ALLOCINFO */

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
 * 16-byte alignment is enough on all currently known machines.
 *
 * With the current parameters, request sizes up to 8K are treated as chunks,
 * larger requests go into dedicated blocks.  Change ALLOCSET_NUM_FREELISTS
 * to adjust the boundary point.
 *--------------------
 */

#define ALLOC_MINBITS		4	/* smallest chunk size is 16 bytes */
#define ALLOCSET_NUM_FREELISTS	10
#define ALLOC_CHUNK_LIMIT	(1 << (ALLOCSET_NUM_FREELISTS-1+ALLOC_MINBITS))
/* Size of largest chunk that we use a fixed size for */

/*--------------------
 * The first block allocated for an allocset has size initBlockSize.
 * Each time we have to allocate another block, we double the block size
 * (if possible, and without exceeding maxBlockSize), so as to reduce
 * the bookkeeping load on malloc().
 *
 * Blocks allocated to hold oversize chunks do not follow this rule, however;
 * they are just however big they need to be to hold that single chunk.
 *--------------------
 */

#define ALLOC_BLOCKHDRSZ	MAXALIGN(sizeof(AllocBlockData))
#define ALLOC_CHUNKHDRSZ	MAXALIGN(sizeof(AllocChunkData))

typedef struct AllocBlockData *AllocBlock;		/* forward reference */
typedef struct AllocChunkData *AllocChunk;

/*
 * AllocPointer
 *		Aligned pointer which may be a member of an allocation set.
 */
typedef void *AllocPointer;

/*
 * AllocSetContext is our standard implementation of MemoryContext.
 */
typedef struct AllocSetContext
{
	MemoryContextData header;	/* Standard memory-context fields */
	/* Info about storage allocated in this context: */
	AllocBlock	blocks;			/* head of list of blocks in this set */
	AllocChunk	freelist[ALLOCSET_NUM_FREELISTS];		/* free chunk lists */
	/* Allocation parameters for this context: */
	Size		initBlockSize;	/* initial block size */
	Size		maxBlockSize;	/* maximum block size */
	AllocBlock	keeper;			/* if not NULL, keep this block over
								 * resets */
} AllocSetContext;

typedef AllocSetContext *AllocSet;

/*
 * AllocBlock
 *		An AllocBlock is the unit of memory that is obtained by aset.c
 *		from malloc().	It contains one or more AllocChunks, which are
 *		the units requested by palloc() and freed by pfree().  AllocChunks
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
	AllocBlock	next;			/* next block in aset's blocks list */
	char	   *freeptr;		/* start of free space in this block */
	char	   *endptr;			/* end of space in this block */
} AllocBlockData;

/*
 * AllocChunk
 *		The prefix of each piece of memory in an AllocBlock
 *
 * NB: this MUST match StandardChunkHeader as defined by utils/memutils.h.
 */
typedef struct AllocChunkData
{
	/* aset is the owning aset if allocated, or the freelist link if free */
	void	   *aset;
	/* size is always the size of the usable space in the chunk */
	Size		size;
#ifdef MEMORY_CONTEXT_CHECKING
	/* when debugging memory usage, also store actual requested size */
	/* this is zero in a free chunk */
	Size		requested_size;
#endif
} AllocChunkData;

/*
 * AllocPointerIsValid
 *		True iff pointer is valid allocation pointer.
 */
#define AllocPointerIsValid(pointer) PointerIsValid(pointer)

/*
 * AllocSetIsValid
 *		True iff set is valid allocation set.
 */
#define AllocSetIsValid(set) PointerIsValid(set)

#define AllocPointerGetChunk(ptr)	\
					((AllocChunk)(((char *)(ptr)) - ALLOC_CHUNKHDRSZ))
#define AllocChunkGetPointer(chk)	\
					((AllocPointer)(((char *)(chk)) + ALLOC_CHUNKHDRSZ))

/*
 * These functions implement the MemoryContext API for AllocSet contexts.
 */
static void *AllocSetAlloc(MemoryContext context, Size size);
static void AllocSetFree(MemoryContext context, void *pointer);
static void *AllocSetRealloc(MemoryContext context, void *pointer, Size size);
static void AllocSetInit(MemoryContext context);
static void AllocSetReset(MemoryContext context);
static void AllocSetDelete(MemoryContext context);
static Size AllocSetGetChunkSpace(MemoryContext context, void *pointer);
static void AllocSetStats(MemoryContext context);

#ifdef MEMORY_CONTEXT_CHECKING
static void AllocSetCheck(MemoryContext context);
#endif

/*
 * This is the virtual function table for AllocSet contexts.
 */
static MemoryContextMethods AllocSetMethods = {
	AllocSetAlloc,
	AllocSetFree,
	AllocSetRealloc,
	AllocSetInit,
	AllocSetReset,
	AllocSetDelete,
	AllocSetGetChunkSpace,
	AllocSetStats
#ifdef MEMORY_CONTEXT_CHECKING
	,AllocSetCheck
#endif
};


/* ----------
 * Debug macros
 * ----------
 */
#ifdef HAVE_ALLOCINFO
#define AllocFreeInfo(_cxt, _chunk) \
			fprintf(stderr, "AllocFree: %s: %p, %d\n", \
				(_cxt)->header.name, (_chunk), (_chunk)->size)
#define AllocAllocInfo(_cxt, _chunk) \
			fprintf(stderr, "AllocAlloc: %s: %p, %d\n", \
				(_cxt)->header.name, (_chunk), (_chunk)->size)
#else
#define AllocFreeInfo(_cxt, _chunk)
#define AllocAllocInfo(_cxt, _chunk)
#endif

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
	int			idx = 0;

	if (size > 0)
	{
		size = (size - 1) >> ALLOC_MINBITS;
		while (size != 0)
		{
			idx++;
			size >>= 1;
		}
		Assert(idx < ALLOCSET_NUM_FREELISTS);
	}

	return idx;
}


/*
 * Public routines
 */


/*
 * AllocSetContextCreate
 *		Create a new AllocSet context.
 *
 * parent: parent context, or NULL if top-level context
 * name: name of context (for debugging --- string will be copied)
 * minContextSize: minimum context size
 * initBlockSize: initial allocation block size
 * maxBlockSize: maximum allocation block size
 */
MemoryContext
AllocSetContextCreate(MemoryContext parent,
					  const char *name,
					  Size minContextSize,
					  Size initBlockSize,
					  Size maxBlockSize)
{
	AllocSet	context;

	/* Do the type-independent part of context creation */
	context = (AllocSet) MemoryContextCreate(T_AllocSetContext,
											 sizeof(AllocSetContext),
											 &AllocSetMethods,
											 parent,
											 name);

	/*
	 * Make sure alloc parameters are reasonable, and save them.
	 *
	 * We somewhat arbitrarily enforce a minimum 1K block size.
	 */
	initBlockSize = MAXALIGN(initBlockSize);
	if (initBlockSize < 1024)
		initBlockSize = 1024;
	maxBlockSize = MAXALIGN(maxBlockSize);
	if (maxBlockSize < initBlockSize)
		maxBlockSize = initBlockSize;
	context->initBlockSize = initBlockSize;
	context->maxBlockSize = maxBlockSize;

	/*
	 * Grab always-allocated space, if requested
	 */
	if (minContextSize > ALLOC_BLOCKHDRSZ + ALLOC_CHUNKHDRSZ)
	{
		Size		blksize = MAXALIGN(minContextSize);
		AllocBlock	block;

		block = (AllocBlock) malloc(blksize);
		if (block == NULL)
		{
			MemoryContextStats(TopMemoryContext);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory"),
				errdetail("Failed while creating memory context \"%s\".",
						  name)));
		}
		block->aset = context;
		block->freeptr = ((char *) block) + ALLOC_BLOCKHDRSZ;
		block->endptr = ((char *) block) + blksize;
		block->next = context->blocks;
		context->blocks = block;
		/* Mark block as not to be released at reset time */
		context->keeper = block;
	}

	return (MemoryContext) context;
}

/*
 * AllocSetInit
 *		Context-type-specific initialization routine.
 *
 * This is called by MemoryContextCreate() after setting up the
 * generic MemoryContext fields and before linking the new context
 * into the context tree.  We must do whatever is needed to make the
 * new context minimally valid for deletion.  We must *not* risk
 * failure --- thus, for example, allocating more memory is not cool.
 * (AllocSetContextCreate can allocate memory when it gets control
 * back, however.)
 */
static void
AllocSetInit(MemoryContext context)
{
	/*
	 * Since MemoryContextCreate already zeroed the context node, we don't
	 * have to do anything here: it's already OK.
	 */
}

/*
 * AllocSetReset
 *		Frees all memory which is allocated in the given set.
 *
 * Actually, this routine has some discretion about what to do.
 * It should mark all allocated chunks freed, but it need not necessarily
 * give back all the resources the set owns.  Our actual implementation is
 * that we hang onto any "keeper" block specified for the set.	In this way,
 * we don't thrash malloc() when a context is repeatedly reset after small
 * allocations, which is typical behavior for per-tuple contexts.
 */
static void
AllocSetReset(MemoryContext context)
{
	AllocSet	set = (AllocSet) context;
	AllocBlock	block = set->blocks;

	AssertArg(AllocSetIsValid(set));

#ifdef MEMORY_CONTEXT_CHECKING
	/* Check for corruption and leaks before freeing */
	AllocSetCheck(context);
#endif

	/* Clear chunk freelists */
	MemSet(set->freelist, 0, sizeof(set->freelist));
	/* New blocks list is either empty or just the keeper block */
	set->blocks = set->keeper;

	while (block != NULL)
	{
		AllocBlock	next = block->next;

		if (block == set->keeper)
		{
			/* Reset the block, but don't return it to malloc */
			char	   *datastart = ((char *) block) + ALLOC_BLOCKHDRSZ;

#ifdef CLOBBER_FREED_MEMORY
			/* Wipe freed memory for debugging purposes */
			memset(datastart, 0x7F, block->freeptr - datastart);
#endif
			block->freeptr = datastart;
			block->next = NULL;
		}
		else
		{
			/* Normal case, release the block */
#ifdef CLOBBER_FREED_MEMORY
			/* Wipe freed memory for debugging purposes */
			memset(block, 0x7F, block->freeptr - ((char *) block));
#endif
			free(block);
		}
		block = next;
	}
}

/*
 * AllocSetDelete
 *		Frees all memory which is allocated in the given set,
 *		in preparation for deletion of the set.
 *
 * Unlike AllocSetReset, this *must* free all resources of the set.
 * But note we are not responsible for deleting the context node itself.
 */
static void
AllocSetDelete(MemoryContext context)
{
	AllocSet	set = (AllocSet) context;
	AllocBlock	block = set->blocks;

	AssertArg(AllocSetIsValid(set));

#ifdef MEMORY_CONTEXT_CHECKING
	/* Check for corruption and leaks before freeing */
	AllocSetCheck(context);
#endif

	/* Make it look empty, just in case... */
	MemSet(set->freelist, 0, sizeof(set->freelist));
	set->blocks = NULL;
	set->keeper = NULL;

	while (block != NULL)
	{
		AllocBlock	next = block->next;

#ifdef CLOBBER_FREED_MEMORY
		/* Wipe freed memory for debugging purposes */
		memset(block, 0x7F, block->freeptr - ((char *) block));
#endif
		free(block);
		block = next;
	}
}

/*
 * AllocSetAlloc
 *		Returns pointer to allocated memory of given size; memory is added
 *		to the set.
 */
static void *
AllocSetAlloc(MemoryContext context, Size size)
{
	AllocSet	set = (AllocSet) context;
	AllocBlock	block;
	AllocChunk	chunk;
	AllocChunk	priorfree;
	int			fidx;
	Size		chunk_size;
	Size		blksize;

	AssertArg(AllocSetIsValid(set));

	/*
	 * If requested size exceeds maximum for chunks, allocate an entire
	 * block for this request.
	 */
	if (size > ALLOC_CHUNK_LIMIT)
	{
		chunk_size = MAXALIGN(size);
		blksize = chunk_size + ALLOC_BLOCKHDRSZ + ALLOC_CHUNKHDRSZ;
		block = (AllocBlock) malloc(blksize);
		if (block == NULL)
		{
			MemoryContextStats(TopMemoryContext);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory"),
					 errdetail("Failed on request of size %lu.",
							   (unsigned long) size)));
		}
		block->aset = set;
		block->freeptr = block->endptr = ((char *) block) + blksize;

		chunk = (AllocChunk) (((char *) block) + ALLOC_BLOCKHDRSZ);
		chunk->aset = set;
		chunk->size = chunk_size;
#ifdef MEMORY_CONTEXT_CHECKING
		chunk->requested_size = size;
		/* set mark to catch clobber of "unused" space */
		if (size < chunk_size)
			((char *) AllocChunkGetPointer(chunk))[size] = 0x7E;
#endif

		/*
		 * Stick the new block underneath the active allocation block, so
		 * that we don't lose the use of the space remaining therein.
		 */
		if (set->blocks != NULL)
		{
			block->next = set->blocks->next;
			set->blocks->next = block;
		}
		else
		{
			block->next = NULL;
			set->blocks = block;
		}

		AllocAllocInfo(set, chunk);
		return AllocChunkGetPointer(chunk);
	}

	/*
	 * Request is small enough to be treated as a chunk.  Look in the
	 * corresponding free list to see if there is a free chunk we could
	 * reuse.
	 */
	fidx = AllocSetFreeIndex(size);
	priorfree = NULL;
	for (chunk = set->freelist[fidx]; chunk; chunk = (AllocChunk) chunk->aset)
	{
		if (chunk->size >= size)
			break;
		priorfree = chunk;
	}

	/*
	 * If one is found, remove it from the free list, make it again a
	 * member of the alloc set and return its data address.
	 */
	if (chunk != NULL)
	{
		if (priorfree == NULL)
			set->freelist[fidx] = (AllocChunk) chunk->aset;
		else
			priorfree->aset = chunk->aset;

		chunk->aset = (void *) set;

#ifdef MEMORY_CONTEXT_CHECKING
		chunk->requested_size = size;
		/* set mark to catch clobber of "unused" space */
		if (size < chunk->size)
			((char *) AllocChunkGetPointer(chunk))[size] = 0x7E;
#endif

		AllocAllocInfo(set, chunk);
		return AllocChunkGetPointer(chunk);
	}

	/*
	 * Choose the actual chunk size to allocate.
	 */
	chunk_size = 1 << (fidx + ALLOC_MINBITS);
	Assert(chunk_size >= size);

	/*
	 * If there is enough room in the active allocation block, we will put
	 * the chunk into that block.  Else must start a new one.
	 */
	if ((block = set->blocks) != NULL)
	{
		Size		availspace = block->endptr - block->freeptr;

		if (availspace < (chunk_size + ALLOC_CHUNKHDRSZ))
		{
			/*
			 * The existing active (top) block does not have enough room
			 * for the requested allocation, but it might still have a
			 * useful amount of space in it.  Once we push it down in the
			 * block list, we'll never try to allocate more space from it.
			 * So, before we do that, carve up its free space into chunks
			 * that we can put on the set's freelists.
			 *
			 * Because we can only get here when there's less than
			 * ALLOC_CHUNK_LIMIT left in the block, this loop cannot
			 * iterate more than ALLOCSET_NUM_FREELISTS-1 times.
			 */
			while (availspace >= ((1 << ALLOC_MINBITS) + ALLOC_CHUNKHDRSZ))
			{
				Size		availchunk = availspace - ALLOC_CHUNKHDRSZ;
				int			a_fidx = AllocSetFreeIndex(availchunk);

				/*
				 * In most cases, we'll get back the index of the next
				 * larger freelist than the one we need to put this chunk
				 * on.	The exception is when availchunk is exactly a
				 * power of 2.
				 */
				if (availchunk != (1 << (a_fidx + ALLOC_MINBITS)))
				{
					a_fidx--;
					Assert(a_fidx >= 0);
					availchunk = (1 << (a_fidx + ALLOC_MINBITS));
				}

				chunk = (AllocChunk) (block->freeptr);

				block->freeptr += (availchunk + ALLOC_CHUNKHDRSZ);
				availspace -= (availchunk + ALLOC_CHUNKHDRSZ);

				chunk->size = availchunk;
#ifdef MEMORY_CONTEXT_CHECKING
				chunk->requested_size = 0;		/* mark it free */
#endif
				chunk->aset = (void *) set->freelist[a_fidx];
				set->freelist[a_fidx] = chunk;
			}

			/* Mark that we need to create a new block */
			block = NULL;
		}
	}

	/*
	 * Time to create a new regular (multi-chunk) block?
	 */
	if (block == NULL)
	{
		Size		required_size;

		if (set->blocks == NULL)
		{
			/* First block of the alloc set, use initBlockSize */
			blksize = set->initBlockSize;
		}
		else
		{
			/*
			 * Use first power of 2 that is larger than previous block,
			 * but not more than the allowed limit.  (We don't simply double
			 * the prior block size, because in some cases this could be a
			 * funny size, eg if very first allocation was for an odd-sized
			 * large chunk.)
			 */
			Size	pblksize = set->blocks->endptr - ((char *) set->blocks);

			blksize = set->initBlockSize;
			while (blksize <= pblksize)
				blksize <<= 1;
			if (blksize > set->maxBlockSize)
				blksize = set->maxBlockSize;
		}

		/*
		 * If initBlockSize is less than ALLOC_CHUNK_LIMIT, we could need
		 * more space... but try to keep it a power of 2.
		 */
		required_size = chunk_size + ALLOC_BLOCKHDRSZ + ALLOC_CHUNKHDRSZ;
		while (blksize < required_size)
			blksize <<= 1;

		/* Try to allocate it */
		block = (AllocBlock) malloc(blksize);

		/*
		 * We could be asking for pretty big blocks here, so cope if
		 * malloc fails.  But give up if there's less than a meg or so
		 * available...
		 */
		while (block == NULL && blksize > 1024 * 1024)
		{
			blksize >>= 1;
			if (blksize < required_size)
				break;
			block = (AllocBlock) malloc(blksize);
		}

		if (block == NULL)
		{
			MemoryContextStats(TopMemoryContext);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory"),
					 errdetail("Failed on request of size %lu.",
							   (unsigned long) size)));
		}

		block->aset = set;
		block->freeptr = ((char *) block) + ALLOC_BLOCKHDRSZ;
		block->endptr = ((char *) block) + blksize;

		/*
		 * If this is the first block of the set, make it the "keeper"
		 * block. Formerly, a keeper block could only be created during
		 * context creation, but allowing it to happen here lets us have
		 * fast reset cycling even for contexts created with
		 * minContextSize = 0; that way we don't have to force space to be
		 * allocated in contexts that might never need any space.  Don't
		 * mark an oversize block as a keeper, however.
		 */
		if (set->blocks == NULL && blksize == set->initBlockSize)
		{
			Assert(set->keeper == NULL);
			set->keeper = block;
		}

		block->next = set->blocks;
		set->blocks = block;
	}

	/*
	 * OK, do the allocation
	 */
	chunk = (AllocChunk) (block->freeptr);

	block->freeptr += (chunk_size + ALLOC_CHUNKHDRSZ);
	Assert(block->freeptr <= block->endptr);

	chunk->aset = (void *) set;
	chunk->size = chunk_size;
#ifdef MEMORY_CONTEXT_CHECKING
	chunk->requested_size = size;
	/* set mark to catch clobber of "unused" space */
	if (size < chunk->size)
		((char *) AllocChunkGetPointer(chunk))[size] = 0x7E;
#endif

	AllocAllocInfo(set, chunk);
	return AllocChunkGetPointer(chunk);
}

/*
 * AllocSetFree
 *		Frees allocated memory; memory is removed from the set.
 */
static void
AllocSetFree(MemoryContext context, void *pointer)
{
	AllocSet	set = (AllocSet) context;
	AllocChunk	chunk = AllocPointerGetChunk(pointer);

	AllocFreeInfo(set, chunk);

#ifdef MEMORY_CONTEXT_CHECKING
	/* Test for someone scribbling on unused space in chunk */
	if (chunk->requested_size < chunk->size)
		if (((char *) pointer)[chunk->requested_size] != 0x7E)
			elog(WARNING, "detected write past chunk end in %s %p",
				 set->header.name, chunk);
#endif

	if (chunk->size > ALLOC_CHUNK_LIMIT)
	{
		/*
		 * Big chunks are certain to have been allocated as single-chunk
		 * blocks.	Find the containing block and return it to malloc().
		 */
		AllocBlock	block = set->blocks;
		AllocBlock	prevblock = NULL;

		while (block != NULL)
		{
			if (chunk == (AllocChunk) (((char *) block) + ALLOC_BLOCKHDRSZ))
				break;
			prevblock = block;
			block = block->next;
		}
		if (block == NULL)
			elog(ERROR, "could not find block containing chunk %p", chunk);
		/* let's just make sure chunk is the only one in the block */
		Assert(block->freeptr == ((char *) block) +
			   (chunk->size + ALLOC_BLOCKHDRSZ + ALLOC_CHUNKHDRSZ));

		/* OK, remove block from aset's list and free it */
		if (prevblock == NULL)
			set->blocks = block->next;
		else
			prevblock->next = block->next;
#ifdef CLOBBER_FREED_MEMORY
		/* Wipe freed memory for debugging purposes */
		memset(block, 0x7F, block->freeptr - ((char *) block));
#endif
		free(block);
	}
	else
	{
		/* Normal case, put the chunk into appropriate freelist */
		int			fidx = AllocSetFreeIndex(chunk->size);

		chunk->aset = (void *) set->freelist[fidx];

#ifdef CLOBBER_FREED_MEMORY
		/* Wipe freed memory for debugging purposes */
		memset(pointer, 0x7F, chunk->size);
#endif

#ifdef MEMORY_CONTEXT_CHECKING
		/* Reset requested_size to 0 in chunks that are on freelist */
		chunk->requested_size = 0;
#endif
		set->freelist[fidx] = chunk;
	}
}

/*
 * AllocSetRealloc
 *		Returns new pointer to allocated memory of given size; this memory
 *		is added to the set.  Memory associated with given pointer is copied
 *		into the new memory, and the old memory is freed.
 */
static void *
AllocSetRealloc(MemoryContext context, void *pointer, Size size)
{
	AllocSet	set = (AllocSet) context;
	AllocChunk	chunk = AllocPointerGetChunk(pointer);
	Size		oldsize = chunk->size;

#ifdef MEMORY_CONTEXT_CHECKING
	/* Test for someone scribbling on unused space in chunk */
	if (chunk->requested_size < oldsize)
		if (((char *) pointer)[chunk->requested_size] != 0x7E)
			elog(WARNING, "detected write past chunk end in %s %p",
				 set->header.name, chunk);
#endif

	/*
	 * Chunk sizes are aligned to power of 2 in AllocSetAlloc(). Maybe the
	 * allocated area already is >= the new size.  (In particular, we
	 * always fall out here if the requested size is a decrease.)
	 */
	if (oldsize >= size)
	{
#ifdef MEMORY_CONTEXT_CHECKING
		chunk->requested_size = size;
		/* set mark to catch clobber of "unused" space */
		if (size < oldsize)
			((char *) pointer)[size] = 0x7E;
#endif
		return pointer;
	}

	if (oldsize > ALLOC_CHUNK_LIMIT)
	{
		/*
		 * The chunk must been allocated as a single-chunk block.  Find
		 * the containing block and use realloc() to make it bigger with
		 * minimum space wastage.
		 */
		AllocBlock	block = set->blocks;
		AllocBlock	prevblock = NULL;
		Size		chksize;
		Size		blksize;

		while (block != NULL)
		{
			if (chunk == (AllocChunk) (((char *) block) + ALLOC_BLOCKHDRSZ))
				break;
			prevblock = block;
			block = block->next;
		}
		if (block == NULL)
			elog(ERROR, "could not find block containing chunk %p", chunk);
		/* let's just make sure chunk is the only one in the block */
		Assert(block->freeptr == ((char *) block) +
			   (chunk->size + ALLOC_BLOCKHDRSZ + ALLOC_CHUNKHDRSZ));

		/* Do the realloc */
		chksize = MAXALIGN(size);
		blksize = chksize + ALLOC_BLOCKHDRSZ + ALLOC_CHUNKHDRSZ;
		block = (AllocBlock) realloc(block, blksize);
		if (block == NULL)
		{
			MemoryContextStats(TopMemoryContext);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory"),
					 errdetail("Failed on request of size %lu.",
							   (unsigned long) size)));
		}
		block->freeptr = block->endptr = ((char *) block) + blksize;

		/* Update pointers since block has likely been moved */
		chunk = (AllocChunk) (((char *) block) + ALLOC_BLOCKHDRSZ);
		if (prevblock == NULL)
			set->blocks = block;
		else
			prevblock->next = block;
		chunk->size = chksize;

#ifdef MEMORY_CONTEXT_CHECKING
		chunk->requested_size = size;
		/* set mark to catch clobber of "unused" space */
		if (size < chunk->size)
			((char *) AllocChunkGetPointer(chunk))[size] = 0x7E;
#endif

		return AllocChunkGetPointer(chunk);
	}
	else
	{
		/*
		 * Small-chunk case.  If the chunk is the last one in its block,
		 * there might be enough free space after it that we can just
		 * enlarge the chunk in-place.	It's relatively painful to find
		 * the containing block in the general case, but we can detect
		 * last-ness quite cheaply for the typical case where the chunk is
		 * in the active (topmost) allocation block.  (At least with the
		 * regression tests and code as of 1/2001, realloc'ing the last
		 * chunk of a non-topmost block hardly ever happens, so it's not
		 * worth scanning the block list to catch that case.)
		 *
		 * NOTE: must be careful not to create a chunk of a size that
		 * AllocSetAlloc would not create, else we'll get confused later.
		 */
		AllocPointer newPointer;

		if (size <= ALLOC_CHUNK_LIMIT)
		{
			AllocBlock	block = set->blocks;
			char	   *chunk_end;

			chunk_end = (char *) chunk + (oldsize + ALLOC_CHUNKHDRSZ);
			if (chunk_end == block->freeptr)
			{
				/* OK, it's last in block ... is there room? */
				Size		freespace = block->endptr - block->freeptr;
				int			fidx;
				Size		newsize;
				Size		delta;

				fidx = AllocSetFreeIndex(size);
				newsize = 1 << (fidx + ALLOC_MINBITS);
				Assert(newsize >= oldsize);
				delta = newsize - oldsize;
				if (freespace >= delta)
				{
					/* Yes, so just enlarge the chunk. */
					block->freeptr += delta;
					chunk->size += delta;
#ifdef MEMORY_CONTEXT_CHECKING
					chunk->requested_size = size;
					/* set mark to catch clobber of "unused" space */
					if (size < chunk->size)
						((char *) pointer)[size] = 0x7E;
#endif
					return pointer;
				}
			}
		}

		/* Normal small-chunk case: just do it by brute force. */

		/* allocate new chunk */
		newPointer = AllocSetAlloc((MemoryContext) set, size);

		/* transfer existing data (certain to fit) */
		memcpy(newPointer, pointer, oldsize);

		/* free old chunk */
		AllocSetFree((MemoryContext) set, pointer);

		return newPointer;
	}
}

/*
 * AllocSetGetChunkSpace
 *		Given a currently-allocated chunk, determine the total space
 *		it occupies (including all memory-allocation overhead).
 */
static Size
AllocSetGetChunkSpace(MemoryContext context, void *pointer)
{
	AllocChunk	chunk = AllocPointerGetChunk(pointer);

	return chunk->size + ALLOC_CHUNKHDRSZ;
}

/*
 * AllocSetStats
 *		Displays stats about memory consumption of an allocset.
 */
static void
AllocSetStats(MemoryContext context)
{
	AllocSet	set = (AllocSet) context;
	long		nblocks = 0;
	long		nchunks = 0;
	long		totalspace = 0;
	long		freespace = 0;
	AllocBlock	block;
	AllocChunk	chunk;
	int			fidx;

	for (block = set->blocks; block != NULL; block = block->next)
	{
		nblocks++;
		totalspace += block->endptr - ((char *) block);
		freespace += block->endptr - block->freeptr;
	}
	for (fidx = 0; fidx < ALLOCSET_NUM_FREELISTS; fidx++)
	{
		for (chunk = set->freelist[fidx]; chunk != NULL;
			 chunk = (AllocChunk) chunk->aset)
		{
			nchunks++;
			freespace += chunk->size + ALLOC_CHUNKHDRSZ;
		}
	}
	fprintf(stderr,
		"%s: %ld total in %ld blocks; %ld free (%ld chunks); %ld used\n",
			set->header.name, totalspace, nblocks, freespace, nchunks,
			totalspace - freespace);
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
static void
AllocSetCheck(MemoryContext context)
{
	AllocSet	set = (AllocSet) context;
	char	   *name = set->header.name;
	AllocBlock	block;

	for (block = set->blocks; block != NULL; block = block->next)
	{
		char	   *bpoz = ((char *) block) + ALLOC_BLOCKHDRSZ;
		long		blk_used = block->freeptr - bpoz;
		long		blk_data = 0;
		long		nchunks = 0;

		/*
		 * Empty block - empty can be keeper-block only
		 */
		if (!blk_used)
		{
			if (set->keeper != block)
				elog(WARNING, "problem in alloc set %s: empty block %p",
					 name, block);
		}

		/*
		 * Chunk walker
		 */
		while (bpoz < block->freeptr)
		{
			AllocChunk	chunk = (AllocChunk) bpoz;
			Size		chsize,
						dsize;
			char	   *chdata_end;

			chsize = chunk->size;		/* aligned chunk size */
			dsize = chunk->requested_size;		/* real data */
			chdata_end = ((char *) chunk) + (ALLOC_CHUNKHDRSZ + dsize);

			/*
			 * Check chunk size
			 */
			if (dsize > chsize)
				elog(WARNING, "problem in alloc set %s: req size > alloc size for chunk %p in block %p",
					 name, chunk, block);
			if (chsize < (1 << ALLOC_MINBITS))
				elog(WARNING, "problem in alloc set %s: bad size %lu for chunk %p in block %p",
					 name, (unsigned long) chsize, chunk, block);

			/* single-chunk block? */
			if (chsize > ALLOC_CHUNK_LIMIT &&
				chsize + ALLOC_CHUNKHDRSZ != blk_used)
				elog(WARNING, "problem in alloc set %s: bad single-chunk %p in block %p",
					 name, chunk, block);

			/*
			 * If chunk is allocated, check for correct aset pointer. (If
			 * it's free, the aset is the freelist pointer, which we can't
			 * check as easily...)
			 */
			if (dsize > 0 && chunk->aset != (void *) set)
				elog(WARNING, "problem in alloc set %s: bogus aset link in block %p, chunk %p",
					 name, block, chunk);

			/*
			 * Check for overwrite of "unallocated" space in chunk
			 */
			if (dsize > 0 && dsize < chsize && *chdata_end != 0x7E)
				elog(WARNING, "problem in alloc set %s: detected write past chunk end in block %p, chunk %p",
					 name, block, chunk);

			blk_data += chsize;
			nchunks++;

			bpoz += ALLOC_CHUNKHDRSZ + chsize;
		}

		if ((blk_data + (nchunks * ALLOC_CHUNKHDRSZ)) != blk_used)
			elog(WARNING, "problem in alloc set %s: found inconsistent memory block %p",
				 name, block);
	}
}

#endif   /* MEMORY_CONTEXT_CHECKING */
