/*-------------------------------------------------------------------------
 *
 * aset.c
 *	  Allocation set definitions.
 *
 * AllocSet is our standard implementation of the abstract MemoryContext
 * type.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/mmgr/aset.c,v 1.31 2000/07/12 05:15:20 tgl Exp $
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
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/memutils.h"

/* Define this to detail debug alloc information 
 */
/*#define HAVE_ALLOCINFO	1*/

/*
 * AllocSetContext is defined in nodes/memnodes.h.
 */
typedef AllocSetContext *AllocSet;

/*
 * AllocPointer
 *		Aligned pointer which may be a member of an allocation set.
 */
typedef void *AllocPointer;

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
	void	*aset;
	/* size is always the size of the usable space in the chunk */
	Size	size;
#ifdef MEMORY_CONTEXT_CHECKING
	Size	data_size;	
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

/*--------------------
 * Chunk freelist k holds chunks of size 1 << (k + ALLOC_MINBITS),
 * for k = 0 .. ALLOCSET_NUM_FREELISTS-2.
 * The last freelist holds all larger free chunks.	Those chunks come in
 * varying sizes depending on the request size, whereas smaller chunks are
 * coerced to powers of 2 to improve their "recyclability".
 *
 * CAUTION: ALLOC_MINBITS must be large enough so that
 * 1<<ALLOC_MINBITS is at least MAXALIGN,
 * or we may fail to align the smallest chunks adequately.
 * 16-byte alignment is enough on all currently known machines.
 *--------------------
 */

#define ALLOC_MINBITS		4	/* smallest chunk size is 16 bytes */
#define ALLOC_SMALLCHUNK_LIMIT	(1 << (ALLOCSET_NUM_FREELISTS-2+ALLOC_MINBITS))
/* Size of largest chunk that we use a fixed size for */

/*--------------------
 * The first block allocated for an allocset has size initBlockSize.
 * Each time we have to allocate another block, we double the block size
 * (if possible, and without exceeding maxBlockSize), so as to reduce
 * the bookkeeping load on malloc().
 *
 * Blocks allocated to hold oversize chunks do not follow this rule, however;
 * they are just however big they need to be to hold that single chunk.
 * AllocSetAlloc has some freedom about whether to consider a chunk larger
 * than ALLOC_SMALLCHUNK_LIMIT to be "oversize".  We require all chunks
 * >= ALLOC_BIGCHUNK_LIMIT to be allocated as single-chunk blocks; those
 * chunks are treated specially by AllocSetFree and AllocSetRealloc.  For
 * request sizes between ALLOC_SMALLCHUNK_LIMIT and ALLOC_BIGCHUNK_LIMIT,
 * AllocSetAlloc has discretion whether to put the request into an existing
 * block or make a single-chunk block.
 *
 * We must have initBlockSize > ALLOC_SMALLCHUNK_LIMIT and
 * ALLOC_BIGCHUNK_LIMIT > ALLOC_SMALLCHUNK_LIMIT.
 *--------------------
 */

#define ALLOC_BIGCHUNK_LIMIT	(64 * 1024)
/* Chunks >= ALLOC_BIGCHUNK_LIMIT are immediately free()d by pfree() */

#define ALLOC_BLOCKHDRSZ	MAXALIGN(sizeof(AllocBlockData))
#define ALLOC_CHUNKHDRSZ	MAXALIGN(sizeof(AllocChunkData))

/* Min safe value of allocation block size */
#define ALLOC_MIN_BLOCK_SIZE  \
	(ALLOC_SMALLCHUNK_LIMIT + ALLOC_CHUNKHDRSZ + ALLOC_BLOCKHDRSZ)

#define AllocPointerGetChunk(ptr)	\
					((AllocChunk)(((char *)(ptr)) - ALLOC_CHUNKHDRSZ))
#define AllocChunkGetPointer(chk)	\
					((AllocPointer)(((char *)(chk)) + ALLOC_CHUNKHDRSZ))
#define AllocPointerGetAset(ptr)	((AllocSet)(AllocPointerGetChunk(ptr)->aset))
#define AllocPointerGetSize(ptr)	(AllocPointerGetChunk(ptr)->size)

/*
 * These functions implement the MemoryContext API for AllocSet contexts.
 */
static void *AllocSetAlloc(MemoryContext context, Size size);
static void AllocSetFree(MemoryContext context, void *pointer);
static void *AllocSetRealloc(MemoryContext context, void *pointer, Size size);
static void AllocSetInit(MemoryContext context);
static void AllocSetReset(MemoryContext context);
static void AllocSetDelete(MemoryContext context);

#ifdef MEMORY_CONTEXT_CHECKING
static void AllocSetCheck(MemoryContext context);
#endif

static void AllocSetStats(MemoryContext context);

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
#ifdef MEMORY_CONTEXT_CHECKING
	AllocSetCheck,
#endif
	AllocSetStats
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
 *		list of the alloc set it belongs to.
 * ----------
 */
static inline int
AllocSetFreeIndex(Size size)
{
	int			idx = 0;

	if (size > 0)
	{
		size = (size - 1) >> ALLOC_MINBITS;
		while (size != 0 && idx < ALLOCSET_NUM_FREELISTS - 1)
		{
			idx++;
			size >>= 1;
		}
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
	 * Make sure alloc parameters are safe, and save them
	 */
	initBlockSize = MAXALIGN(initBlockSize);
	if (initBlockSize < ALLOC_MIN_BLOCK_SIZE)
		initBlockSize = ALLOC_MIN_BLOCK_SIZE;
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
			elog(ERROR, "Memory exhausted in AllocSetContextCreate()");
		block->aset = context;
		block->freeptr = ((char *) block) + ALLOC_BLOCKHDRSZ;
		block->endptr = ((char *) block) + blksize;
		block->next = context->blocks;
		context->blocks = block;
		/* Mark block as not to be released at reset time */
		context->keeper = block;

#ifdef MEMORY_CONTEXT_CHECKING
		/* mark memory for memory leak searching */
		memset(block->freeptr, 0x7F, blksize - ALLOC_BLOCKHDRSZ);
#endif
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
	 * Since MemoryContextCreate already zeroed the context node,
	 * we don't have to do anything here: it's already OK.
	 */
}

/*
 * AllocSetReset
 *		Frees all memory which is allocated in the given set.
 *
 * Actually, this routine has some discretion about what to do.
 * It should mark all allocated chunks freed, but it need not
 * necessarily give back all the resources the set owns.  Our
 * actual implementation is that we hang on to any "keeper"
 * block specified for the set.
 */
static void
AllocSetReset(MemoryContext context)
{
	AllocSet	set = (AllocSet) context;
	AllocBlock	block = set->blocks;

	AssertArg(AllocSetIsValid(set));

	while (block != NULL)
	{
		AllocBlock	next = block->next;

		if (block == set->keeper)
		{
			/* Reset the block, but don't return it to malloc */
			char   *datastart = ((char *) block) + ALLOC_BLOCKHDRSZ;

#ifdef CLOBBER_FREED_MEMORY
			/* Wipe freed memory for debugging purposes */
			memset(datastart, 0x7F, ((char *) block->freeptr) - datastart);
#endif
			block->freeptr = datastart;
			block->next = NULL;
		}
		else
		{
			/* Normal case, release the block */
#ifdef CLOBBER_FREED_MEMORY
			/* Wipe freed memory for debugging purposes */
			memset(block, 0x7F, ((char *) block->freeptr) - ((char *) block));
#endif
			free(block);
		}
		block = next;
	}

	/* Now blocks list is either empty or just the keeper block */
	set->blocks = set->keeper;
	/* Clear chunk freelists in any case */
	MemSet(set->freelist, 0, sizeof(set->freelist));
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

	while (block != NULL)
	{
		AllocBlock	next = block->next;

#ifdef CLOBBER_FREED_MEMORY
		/* Wipe freed memory for debugging purposes */
		memset(block, 0x7F, ((char *) block->endptr) - ((char *) block));
#endif
		free(block);
		block = next;
	}

	/* Make it look empty, just in case... */
	set->blocks = NULL;
	MemSet(set->freelist, 0, sizeof(set->freelist));
	set->keeper = NULL;
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
	AllocChunk	priorfree = NULL;
	int		fidx;
	Size		chunk_size;
	Size		blksize;

	AssertArg(AllocSetIsValid(set));

	/*
	 * Small size can be in free list 
	 */
	if (size < ALLOC_BIGCHUNK_LIMIT)
	{
		/*
		 * Lookup in the corresponding free list if there is a free chunk we
		 * could reuse
		 */
		fidx = AllocSetFreeIndex(size);
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
			chunk->data_size = size;
#endif
			AllocAllocInfo(set, chunk);
			return AllocChunkGetPointer(chunk);
		}
	} 
	else
		/* Big chunk
		 */
		fidx = ALLOCSET_NUM_FREELISTS - 1;

	/*
	 * Choose the actual chunk size to allocate.
	 */
	if (size > ALLOC_SMALLCHUNK_LIMIT)
		chunk_size = MAXALIGN(size);
	else
		chunk_size = 1 << (fidx + ALLOC_MINBITS);
	Assert(chunk_size >= size);

	/*
	 * If there is enough room in the active allocation block, *and* the
	 * chunk is less than ALLOC_BIGCHUNK_LIMIT, put the chunk into the
	 * active allocation block.
	 */
	if ((block = set->blocks) != NULL)
	{
		Size		have_free = block->endptr - block->freeptr;

		if (have_free < (chunk_size + ALLOC_CHUNKHDRSZ) ||
			chunk_size >= ALLOC_BIGCHUNK_LIMIT)
			block = NULL;
	}

	/*
	 * Otherwise, if requested size exceeds smallchunk limit, allocate an
	 * entire separate block for this allocation.  In particular, we will
	 * always take this path if the requested size exceeds bigchunk limit.
	 */
	if (block == NULL && size > ALLOC_SMALLCHUNK_LIMIT)
	{
		blksize = chunk_size + ALLOC_BLOCKHDRSZ + ALLOC_CHUNKHDRSZ;
		block = (AllocBlock) malloc(blksize);
		if (block == NULL)
			elog(ERROR, "Memory exhausted in AllocSetAlloc()");
		block->aset = set;
		block->freeptr = block->endptr = ((char *) block) + blksize;

		chunk = (AllocChunk) (((char *) block) + ALLOC_BLOCKHDRSZ);
		chunk->aset = set;
		chunk->size = chunk_size;

		/*
		 * Try to stick the block underneath the active allocation block,
		 * so that we don't lose the use of the space remaining therein.
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
		
#ifdef MEMORY_CONTEXT_CHECKING
		chunk->data_size = size;
		/* mark memory for memory leak searching */
		memset(AllocChunkGetPointer(chunk), 0x7F, chunk->size);		
#endif
		AllocAllocInfo(set, chunk);
		return AllocChunkGetPointer(chunk);
	}

	/*
	 * Time to create a new regular (multi-chunk) block?
	 */
	if (block == NULL)
	{
		if (set->blocks == NULL)
		{
			blksize = set->initBlockSize;
			block = (AllocBlock) malloc(blksize);
		}
		else
		{
			/* Get size of prior block */
			blksize = set->blocks->endptr - ((char *) set->blocks);

			/*
			 * Special case: if very first allocation was for a large
			 * chunk (or we have a small "keeper" block), could have an
			 * undersized top block.  Do something reasonable.
			 */
			if (blksize < set->initBlockSize)
				blksize = set->initBlockSize;
			else
			{
				/* Crank it up, but not past max */
				blksize <<= 1;
				if (blksize > set->maxBlockSize)
					blksize = set->maxBlockSize;
			}
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
				block = (AllocBlock) malloc(blksize);
			}
		}

		if (block == NULL)
			elog(ERROR, "Memory exhausted in AllocSetAlloc()");
			
		block->aset = set;
		block->freeptr = ((char *) block) + ALLOC_BLOCKHDRSZ;
		block->endptr = ((char *) block) + blksize;

#ifdef MEMORY_CONTEXT_CHECKING
		/* mark memory for memory leak searching */
		memset(block->freeptr, 0x7F, blksize - ALLOC_BLOCKHDRSZ);
#endif
		block->next = set->blocks;

		set->blocks = block;
	}

	/*
	 * OK, do the allocation
	 */
	chunk = (AllocChunk) (block->freeptr);
	chunk->aset = (void *) set;
	chunk->size = chunk_size;
	
#ifdef MEMORY_CONTEXT_CHECKING
	chunk->data_size = size;
#endif	
	block->freeptr += (chunk_size + ALLOC_CHUNKHDRSZ);
	Assert(block->freeptr <= block->endptr);

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

#if defined(CLOBBER_FREED_MEMORY) || defined(MEMORY_CONTEXT_CHECKING)
	/* Wipe freed memory for debugging purposes or for memory leak 
	 * searching (in freelist[] must be mark memory
	 */
	memset(pointer, 0x7F, chunk->size);
#endif

	AllocFreeInfo(set, chunk);

	if (chunk->size >= ALLOC_BIGCHUNK_LIMIT)
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
			elog(ERROR, "AllocSetFree: cannot find block containing chunk");
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
		memset(block, 0x7F, ((char *) block->endptr) - ((char *) block));
#endif
		free(block);
	}
	else
	{
		/* Normal case, put the chunk into appropriate freelist */
		int			fidx = AllocSetFreeIndex(chunk->size);

		chunk->aset = (void *) set->freelist[fidx];

#ifdef MEMORY_CONTEXT_CHECKING
		chunk->data_size = 0;
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
	Size		oldsize;

	/*
	 * Chunk sizes are aligned to power of 2 in AllocSetAlloc(). Maybe the
	 * allocated area already is >= the new size.  (In particular, we
	 * always fall out here if the requested size is a decrease.)
	 */
	oldsize = AllocPointerGetSize(pointer);
	if (oldsize >= size)
	{
#ifdef MEMORY_CONTEXT_CHECKING		
		AllocChunk	chunk = AllocPointerGetChunk(pointer);

		/* mark memory for memory leak searching */
		memset(((char *) chunk) + (ALLOC_CHUNKHDRSZ + size), 
				0x7F, chunk->size - size);
		chunk->data_size = size;
#endif
		return pointer;
	}

	if (oldsize >= ALLOC_BIGCHUNK_LIMIT)
	{

		/*
		 * If the chunk is already >= bigchunk limit, then it must have
		 * been allocated as a single-chunk block.	Find the containing
		 * block and use realloc() to make it bigger with minimum space
		 * wastage.
		 */
		AllocChunk	chunk = AllocPointerGetChunk(pointer);
		AllocBlock	block = set->blocks;
		AllocBlock	prevblock = NULL;
		Size		blksize;
#ifdef MEMORY_CONTEXT_CHECKING		
		Size		data_size = size;
#endif

		while (block != NULL)
		{
			if (chunk == (AllocChunk) (((char *) block) + ALLOC_BLOCKHDRSZ))
				break;
			prevblock = block;
			block = block->next;
		}
		if (block == NULL)
			elog(ERROR, "AllocSetRealloc: cannot find block containing chunk");
		/* let's just make sure chunk is the only one in the block */
		Assert(block->freeptr == ((char *) block) +
			   (chunk->size + ALLOC_BLOCKHDRSZ + ALLOC_CHUNKHDRSZ));

		/* Do the realloc */
		size = MAXALIGN(size);
		blksize = size + ALLOC_BLOCKHDRSZ + ALLOC_CHUNKHDRSZ;
		block = (AllocBlock) realloc(block, blksize);
		if (block == NULL)
			elog(ERROR, "Memory exhausted in AllocSetReAlloc()");
		block->freeptr = block->endptr = ((char *) block) + blksize;

		/* Update pointers since block has likely been moved */
		chunk = (AllocChunk) (((char *) block) + ALLOC_BLOCKHDRSZ);
		if (prevblock == NULL)
			set->blocks = block;
		else
			prevblock->next = block;
		chunk->size = size;
		
#ifdef MEMORY_CONTEXT_CHECKING
		/* mark memory for memory leak searching */
		memset(((char *) chunk) + (ALLOC_CHUNKHDRSZ + data_size), 
				0x7F, size - data_size);
		chunk->data_size = data_size;
#endif
		return AllocChunkGetPointer(chunk);
	}
	else
	{
		/* Normal small-chunk case: just do it by brute force. */

		/* allocate new chunk */
		AllocPointer newPointer = AllocSetAlloc((MemoryContext) set, size);

		/* transfer existing data (certain to fit) */
		memcpy(newPointer, pointer, oldsize);

		/* free old chunk */
		AllocSetFree((MemoryContext) set, pointer);

		return newPointer;
	}
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


/* 
 * AllocSetCheck
 *		Walk on chunks and check consistence of memory. 
 */
#ifdef MEMORY_CONTEXT_CHECKING

static void 
AllocSetCheck(MemoryContext context)
{
	AllocSet	set = (AllocSet) context;	
	AllocBlock	block = NULL;
	AllocChunk	chunk = NULL;
	char		*name = set->header.name;

	for (block = set->blocks; block != NULL; block = block->next)
	{	
		char	*bpoz = ((char *) block) + ALLOC_BLOCKHDRSZ;
	/*	long	blk_size = block->endptr - ((char *) block);*/
		long	blk_free = block->endptr - block->freeptr; 
		long	blk_used = block->freeptr - bpoz;
		long	blk_data = 0;
		long	nchunks  = 0;

		/*
		 * Empty block - empty can be keeper-block only
		 */
		if (!blk_used)
		{
			if (set->keeper == block)
				continue;
			else	
				elog(ERROR, "AllocSetCheck(): %s: empty block %p", 
						name, block);
		}	
		
		/*
		 * Chunk walker
		 */	
		do {
			Size	chsize,
				dsize;
			char	*chdata_end,
				*chend;
				
			chunk = (AllocChunk) bpoz;
			
			chsize = chunk->size;		/* align chunk size */
			dsize = chunk->data_size;	/* real data */
			
			chdata_end = ((char *) chunk) + (ALLOC_CHUNKHDRSZ + dsize);
			chend = ((char *) chunk) + (ALLOC_CHUNKHDRSZ + chsize);
			
			if (!dsize && chsize < dsize)
				elog(ERROR, "AllocSetCheck(): %s: internal error for chunk %p in block %p",
						name, chunk, block);			
			/*
			 * Check chunk size
			 */
			if (chsize < (1 << ALLOC_MINBITS))
				elog(ERROR, "AllocSetCheck(): %s: bad size '%d' for chunk %p in block %p",
						name, chsize, chunk, block);
						
			/* single-chunk block */
			if (chsize >= ALLOC_BIGCHUNK_LIMIT &&
			    chsize + ALLOC_CHUNKHDRSZ != blk_used)
				elog(ERROR, "AllocSetCheck(): %s: bad singel-chunk %p in block %p",
						name, chunk, block);
						
			/*
			 * Check in-chunk leak
			 */		
			if (dsize < chsize && *chdata_end != 0x7F)
			{
				fprintf(stderr, "\n--- Leak %p ---\n", chdata_end);
				fprintf(stderr, "Chunk dump size: %ld (chunk-header %ld + chunk-size: %d), data must be: %d\n--- dump begin ---\n", 
					chsize + ALLOC_CHUNKHDRSZ, 
					ALLOC_CHUNKHDRSZ, chsize, dsize);
					
				fwrite((void *) chunk, chsize+ALLOC_CHUNKHDRSZ, sizeof(char), stderr);
				fputs("\n--- dump end ---\n", stderr);
				
				elog(ERROR, "AllocSetCheck(): %s: found in-chunk memory leak (block %p; chunk %p; leak at %p",
						name, block, chunk, chdata_end);
			}
			
			/*
			 * Check block-freeptr leak 
			 */
			if (chend == block->freeptr && blk_free && 
							*chdata_end != 0x7F) {
				
				fprintf(stderr, "\n--- Leak %p ---\n", chdata_end);
				fprintf(stderr, "Dump size: %ld (chunk-header %ld + chunk-size: %d + block-freespace: %ld), data must be: %d\n--- dump begin ---\n", 
					chsize + ALLOC_CHUNKHDRSZ + blk_free, 
					ALLOC_CHUNKHDRSZ, chsize, blk_free, dsize);
					
				fwrite((void *) chunk, chsize+ALLOC_CHUNKHDRSZ+blk_free, sizeof(char), stderr);
				fputs("\n--- dump end ---\n", stderr);
				
				elog(ERROR, "AllocSetCheck(): %s: found block-freeptr memory leak (block %p; chunk %p; leak at %p",
						name, block, chunk, chdata_end);
			}			
			
			blk_data += chsize;
			nchunks++;
			
			if (chend < block->freeptr)
				bpoz += ALLOC_CHUNKHDRSZ + chsize;
			else
				break;

		} while(block->freeptr > bpoz); /* chunk walker */		
	
		
		if ((blk_data + (nchunks * ALLOC_CHUNKHDRSZ)) != blk_used)

			elog(ERROR, "AllocSetCheck(): %s: found non-consistent memory block %p",
					name, block);
	}
}
#endif
