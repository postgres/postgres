/*-------------------------------------------------------------------------
 *
 * aset.c
 *	  Allocation set definitions.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/mmgr/aset.c,v 1.24 2000/01/31 04:35:53 tgl Exp $
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


#undef AllocSetReset
#undef malloc
#undef free
#undef realloc


/*--------------------
 * Chunk freelist k holds chunks of size 1 << (k + ALLOC_MINBITS),
 * for k = 0 .. ALLOCSET_NUM_FREELISTS-2.
 * The last freelist holds all larger free chunks.  Those chunks come in
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
 * The first block allocated for an allocset has size ALLOC_MIN_BLOCK_SIZE.
 * Each time we have to allocate another block, we double the block size
 * (if possible, and without exceeding ALLOC_MAX_BLOCK_SIZE), so as to reduce
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
 * We must have ALLOC_MIN_BLOCK_SIZE > ALLOC_SMALLCHUNK_LIMIT and
 * ALLOC_BIGCHUNK_LIMIT > ALLOC_SMALLCHUNK_LIMIT.
 *--------------------
 */

#define ALLOC_MIN_BLOCK_SIZE	(8 * 1024)
#define ALLOC_MAX_BLOCK_SIZE	(8 * 1024 * 1024)

#define ALLOC_BIGCHUNK_LIMIT	(64 * 1024)
/* Chunks >= ALLOC_BIGCHUNK_LIMIT are immediately free()d by pfree() */

#define ALLOC_BLOCKHDRSZ	MAXALIGN(sizeof(AllocBlockData))
#define ALLOC_CHUNKHDRSZ	MAXALIGN(sizeof(AllocChunkData))

#define AllocPointerGetChunk(ptr)	\
					((AllocChunk)(((char *)(ptr)) - ALLOC_CHUNKHDRSZ))
#define AllocChunkGetPointer(chk)	\
					((AllocPointer)(((char *)(chk)) + ALLOC_CHUNKHDRSZ))
#define AllocPointerGetAset(ptr)	((AllocSet)(AllocPointerGetChunk(ptr)->aset))
#define AllocPointerGetSize(ptr)	(AllocPointerGetChunk(ptr)->size)



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
 * AllocSetInit
 *		Initializes given allocation set.
 *
 * Note:
 *		The semantics of the mode are explained above.	Limit is ignored
 *		for dynamic and static modes.
 *
 * Exceptions:
 *		BadArg if set is invalid pointer.
 *		BadArg if mode is invalid.
 */
void
AllocSetInit(AllocSet set, AllocMode mode, Size limit)
{
	AssertArg(PointerIsValid(set));
	AssertArg((int) DynamicAllocMode <= (int) mode);
	AssertArg((int) mode <= (int) BoundedAllocMode);

	/*
	 * XXX mode is currently ignored and treated as DynamicAllocMode. XXX
	 * limit is also ignored.  This affects this whole file.
	 */

	memset(set, 0, sizeof(AllocSetData));
}


/*
 * AllocSetReset
 *		Frees all memory which is allocated in the given set.
 *
 * Exceptions:
 *		BadArg if set is invalid.
 */
void
AllocSetReset(AllocSet set)
{
	AllocBlock	block = set->blocks;
	AllocBlock	next;

	AssertArg(AllocSetIsValid(set));

	while (block != NULL)
	{
		next = block->next;
		free(block);
		block = next;
	}

	memset(set, 0, sizeof(AllocSetData));
}

/*
 * AllocSetContains
 *		True iff allocation set contains given allocation element.
 *
 * Exceptions:
 *		BadArg if set is invalid.
 *		BadArg if pointer is invalid.
 */
bool
AllocSetContains(AllocSet set, AllocPointer pointer)
{
	AssertArg(AllocSetIsValid(set));
	AssertArg(AllocPointerIsValid(pointer));

	return (AllocPointerGetAset(pointer) == set);
}

/*
 * AllocSetAlloc
 *		Returns pointer to allocated memory of given size; memory is added
 *		to the set.
 *
 * Exceptions:
 *		BadArg if set is invalid.
 *		MemoryExhausted if allocation fails.
 */
AllocPointer
AllocSetAlloc(AllocSet set, Size size)
{
	AllocBlock	block;
	AllocChunk	chunk;
	AllocChunk	priorfree = NULL;
	int			fidx;
	Size		chunk_size;
	Size		blksize;

	AssertArg(AllocSetIsValid(set));

	/*
	 * Lookup in the corresponding free list if there is a free chunk we
	 * could reuse
	 *
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
	 *
	 */
	if (chunk != NULL)
	{
		if (priorfree == NULL)
			set->freelist[fidx] = (AllocChunk) chunk->aset;
		else
			priorfree->aset = chunk->aset;

		chunk->aset = (void *) set;
		return AllocChunkGetPointer(chunk);
	}

	/*
	 * Choose the actual chunk size to allocate.
	 */
	if (size > ALLOC_SMALLCHUNK_LIMIT)
		chunk_size = MAXALIGN(size);
	else
		chunk_size = 1 << (fidx + ALLOC_MINBITS);
	Assert(chunk_size >= size);

	/*
	 * If there is enough room in the active allocation block, *and*
	 * the chunk is less than ALLOC_BIGCHUNK_LIMIT, put the chunk
	 * into the active allocation block.
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
			elog(FATAL, "Memory exhausted in AllocSetAlloc()");
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

		return AllocChunkGetPointer(chunk);
	}

	/*
	 * Time to create a new regular (multi-chunk) block?
	 */
	if (block == NULL)
	{
		if (set->blocks == NULL)
		{
			blksize = ALLOC_MIN_BLOCK_SIZE;
			block = (AllocBlock) malloc(blksize);
		}
		else
		{
			/* Get size of prior block */
			blksize = set->blocks->endptr - ((char *) set->blocks);

			/*
			 * Special case: if very first allocation was for a large
			 * chunk, could have a funny-sized top block.  Do something
			 * reasonable.
			 */
			if (blksize < ALLOC_MIN_BLOCK_SIZE)
				blksize = ALLOC_MIN_BLOCK_SIZE;
			/* Crank it up, but not past max */
			blksize <<= 1;
			if (blksize > ALLOC_MAX_BLOCK_SIZE)
				blksize = ALLOC_MAX_BLOCK_SIZE;
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
			elog(FATAL, "Memory exhausted in AllocSetAlloc()");
		block->aset = set;
		block->freeptr = ((char *) block) + ALLOC_BLOCKHDRSZ;
		block->endptr = ((char *) block) + blksize;
		block->next = set->blocks;

		set->blocks = block;
	}

	/*
	 * OK, do the allocation
	 */
	chunk = (AllocChunk) (block->freeptr);
	chunk->aset = (void *) set;
	chunk->size = chunk_size;
	block->freeptr += (chunk_size + ALLOC_CHUNKHDRSZ);
	Assert(block->freeptr <= block->endptr);

	return AllocChunkGetPointer(chunk);
}

/*
 * AllocSetFree
 *		Frees allocated memory; memory is removed from the set.
 *
 * Exceptions:
 *		BadArg if set is invalid.
 *		BadArg if pointer is invalid.
 *		BadArg if pointer is not member of set.
 */
void
AllocSetFree(AllocSet set, AllocPointer pointer)
{
	AllocChunk	chunk;

	/* AssertArg(AllocSetIsValid(set)); */
	/* AssertArg(AllocPointerIsValid(pointer)); */
	AssertArg(AllocSetContains(set, pointer));

	chunk = AllocPointerGetChunk(pointer);

#ifdef CLOBBER_FREED_MEMORY
	/* Wipe freed memory for debugging purposes */
	memset(pointer, 0x7F, chunk->size);
#endif

	if (chunk->size >= ALLOC_BIGCHUNK_LIMIT)
	{
		/* Big chunks are certain to have been allocated as single-chunk
		 * blocks.  Find the containing block and return it to malloc().
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
		free(block);
	}
	else
	{
		/* Normal case, put the chunk into appropriate freelist */
		int			fidx = AllocSetFreeIndex(chunk->size);

		chunk->aset = (void *) set->freelist[fidx];
		set->freelist[fidx] = chunk;
	}
}

/*
 * AllocSetRealloc
 *		Returns new pointer to allocated memory of given size; this memory
 *		is added to the set.  Memory associated with given pointer is copied
 *		into the new memory, and the old memory is freed.
 *
 * Exceptions:
 *		BadArg if set is invalid.
 *		BadArg if pointer is invalid.
 *		BadArg if pointer is not member of set.
 *		MemoryExhausted if allocation fails.
 */
AllocPointer
AllocSetRealloc(AllocSet set, AllocPointer pointer, Size size)
{
	Size		oldsize;

	/* AssertArg(AllocSetIsValid(set)); */
	/* AssertArg(AllocPointerIsValid(pointer)); */
	AssertArg(AllocSetContains(set, pointer));

	/*
	 * Chunk sizes are aligned to power of 2 on AllocSetAlloc(). Maybe the
	 * allocated area already is >= the new size.  (In particular, we
	 * always fall out here if the requested size is a decrease.)
	 */
	oldsize = AllocPointerGetSize(pointer);
	if (oldsize >= size)
		return pointer;

	if (oldsize >= ALLOC_BIGCHUNK_LIMIT)
	{
		/*
		 * If the chunk is already >= bigchunk limit, then it must have been
		 * allocated as a single-chunk block.  Find the containing block and
		 * use realloc() to make it bigger with minimum space wastage.
		 */
		AllocChunk	chunk = AllocPointerGetChunk(pointer);
		AllocBlock	block = set->blocks;
		AllocBlock	prevblock = NULL;
		Size		blksize;

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
			elog(FATAL, "Memory exhausted in AllocSetReAlloc()");
		block->freeptr = block->endptr = ((char *) block) + blksize;

		/* Update pointers since block has likely been moved */
		chunk = (AllocChunk) (((char *) block) + ALLOC_BLOCKHDRSZ);
		if (prevblock == NULL)
			set->blocks = block;
		else
			prevblock->next = block;
		chunk->size = size;
		return AllocChunkGetPointer(chunk);
	}
	else
	{
		/* Normal small-chunk case: just do it by brute force. */

		/* allocate new chunk */
		AllocPointer newPointer = AllocSetAlloc(set, size);

		/* transfer existing data (certain to fit) */
		memcpy(newPointer, pointer, oldsize);

		/* free old chunk */
		AllocSetFree(set, pointer);

		return newPointer;
	}
}

/*
 * AllocSetDump
 *		Displays allocated set.
 */
void
AllocSetDump(AllocSet set)
{
	elog(DEBUG, "Currently unable to dump AllocSet");
}
