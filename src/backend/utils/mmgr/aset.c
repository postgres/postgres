/*-------------------------------------------------------------------------
 *
 * aset.c
 *	  Allocation set definitions.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/mmgr/aset.c,v 1.20 1999/07/17 20:18:13 momjian Exp $
 *
 * NOTE:
 *	This is a new (Feb. 05, 1999) implementation of the allocation set
 *	routines. AllocSet...() does not use OrderedSet...() any more.
 *	Instead it manages allocations in a block pool by itself, combining
 *	many small allocations in a few bigger blocks. AllocSetFree() does
 *	never free() memory really. It just add's the free'd area to some
 *	list for later reuse by AllocSetAlloc(). All memory blocks are free()'d
 *	at once on AllocSetReset(), which happens when the memory context gets
 *	destroyed.
 *				Jan Wieck
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
 * The last freelist holds all larger chunks.
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
 * the load on "malloc".
 *
 * Blocks allocated to hold oversize chunks do not follow this rule, however;
 * they are just however big they need to be.
 *--------------------
 */

#define ALLOC_MIN_BLOCK_SIZE	8192
#define ALLOC_MAX_BLOCK_SIZE	(8 * 1024 * 1024)


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
 *		AllocPointerIsValid(pointer)
 *		AllocSetIsValid(set)
 *
 *				.. are now macros in aset.h -cim 4/27/91
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
 *		Frees memory which is allocated in the given set.
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
	AllocChunk	freeref = NULL;
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
		freeref = chunk;
	}

	/*
	 * If one is found, remove it from the free list, make it again a
	 * member of the alloc set and return it's data address.
	 *
	 */
	if (chunk != NULL)
	{
		if (freeref == NULL)
			set->freelist[fidx] = (AllocChunk) chunk->aset;
		else
			freeref->aset = chunk->aset;

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
	 * If there is enough room in the active allocation block, always
	 * allocate the chunk there.
	 */

	if ((block = set->blocks) != NULL)
	{
		Size		have_free = block->endptr - block->freeptr;

		if (have_free < (chunk_size + ALLOC_CHUNKHDRSZ))
			block = NULL;
	}

	/*
	 * Otherwise, if requested size exceeds smallchunk limit, allocate an
	 * entire separate block for this allocation
	 *
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
	 * Time to create a new regular block?
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
	int			fidx;
	AllocChunk	chunk;

	/* AssertArg(AllocSetIsValid(set)); */
	/* AssertArg(AllocPointerIsValid(pointer)); */
	AssertArg(AllocSetContains(set, pointer));

	chunk = AllocPointerGetChunk(pointer);
	fidx = AllocSetFreeIndex(chunk->size);

	chunk->aset = (void *) set->freelist[fidx];
	set->freelist[fidx] = chunk;
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
	AllocPointer newPointer;
	Size		oldsize;

	/* AssertArg(AllocSetIsValid(set)); */
	/* AssertArg(AllocPointerIsValid(pointer)); */
	AssertArg(AllocSetContains(set, pointer));

	/*
	 * Chunk sizes are aligned to power of 2 on AllocSetAlloc(). Maybe the
	 * allocated area already is >= the new size.
	 *
	 */
	oldsize = AllocPointerGetSize(pointer);
	if (oldsize >= size)
		return pointer;

	/* allocate new pointer */
	newPointer = AllocSetAlloc(set, size);

	/* fill new memory */
	memmove(newPointer, pointer, (oldsize < size) ? oldsize : size);

	/* free old pointer */
	AllocSetFree(set, pointer);

	return newPointer;
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
