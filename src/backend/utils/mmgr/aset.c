/*-------------------------------------------------------------------------
 *
 * aset.c--
 *	  Allocation set definitions.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/mmgr/aset.c,v 1.13 1999/02/07 13:37:56 wieck Exp $
 *
 * NOTE:
 *	This is a new (Feb. 05, 1999) implementation of the allocation set
 *	routines. AllocSet...() does not use OrderedSet...() any more.
 *	Instead it manages allocations in a block pool by itself, combining
 *	many small allocations in a few bigger blocks. AllocSetFree() does
 *	never free() memory really. It just add's the free'd area to some
 *	list for later reuse by AllocSetAlloc(). All memory blocks are free()'d
 *	on AllocSetReset() at once, what happens when the memory context gets
 *	destroyed.
 *				Jan Wieck
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include "postgres.h"
#include "utils/excid.h"		/* for ExhaustedMemory */
#include "utils/memutils.h"		/* where funnction declarations go */
#ifndef HAVE_MEMMOVE
#include <regex/utils.h>
#else
#include <string.h>
#endif


#undef AllocSetReset
#undef malloc
#undef free
#undef realloc


#define	ALLOC_BLOCK_SIZE	8192
#define	ALLOC_CHUNK_LIMIT	512

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
	int idx = 0;

	size = (size - 1) >> 4;
	while (size != 0 && idx < 7)
	{
		idx++;
		size >>= 1;
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
 * AllocSetInit --
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
 * AllocSetReset --
 *		Frees memory which is allocated in the given set.
 *
 * Exceptions:
 *		BadArg if set is invalid.
 */
void
AllocSetReset(AllocSet set)
{
	AllocBlock		block = set->blocks;
	AllocBlock		next;

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
 * AllocSetContains --
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
 * AllocSetAlloc --
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
	AllocBlock		block;
	AllocChunk		chunk;
	AllocChunk		freeref = NULL;
	int				fidx;
	Size			chunk_size;

	AssertArg(AllocSetIsValid(set));

	/*
	 * Lookup in the corresponding free list if there is a
	 * free chunk we could reuse
	 *
	 */
	fidx = AllocSetFreeIndex(size);
	for (chunk = set->freelist[fidx]; chunk; chunk = (AllocChunk)chunk->aset)
	{
		if (chunk->size >= size)
			break;
		freeref = chunk;
	}

	/*
	 * If found, remove it from the free list, make it again
	 * a member of the alloc set and return it's data address.
	 *
	 */
	if (chunk != NULL)
	{
		if (freeref == NULL)
			set->freelist[fidx] = (AllocChunk)chunk->aset;
		else
			freeref->aset = chunk->aset;

		chunk->aset = (void *)set;
		return AllocChunkGetPointer(chunk);
	}

	/*
	 * If requested size exceeds smallchunk limit, allocate a separate,
	 * entire used block for this allocation
	 *
	 */
	if (size > ALLOC_CHUNK_LIMIT)
	{
		Size	blksize;

		chunk_size = MAXALIGN(size);
		blksize = chunk_size + ALLOC_BLOCKHDRSZ + ALLOC_CHUNKHDRSZ;
		block = (AllocBlock) malloc(blksize);
		if (block == NULL)
			elog(FATAL, "Memory exhausted in AllocSetAlloc()");
		block->aset = set;
		block->freeptr = block->endptr = ((char *)block) + ALLOC_BLOCKHDRSZ;

		chunk = (AllocChunk) (((char *)block) + ALLOC_BLOCKHDRSZ);
		chunk->aset = set;
		chunk->size = chunk_size;

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

	chunk_size = 16 << fidx;

	if ((block = set->blocks) != NULL)
	{
		Size		have_free = block->endptr - block->freeptr;

		if (have_free < (chunk_size + ALLOC_CHUNKHDRSZ))
			block = NULL;
	}

	if (block == NULL)
	{
		block = (AllocBlock) malloc(ALLOC_BLOCK_SIZE);
		if (block == NULL)
			elog(FATAL, "Memory exhausted in AllocSetAlloc()");
		block->aset = set;
		block->next = set->blocks;
		block->freeptr = ((char *)block) + ALLOC_BLOCKHDRSZ;
		block->endptr = ((char *)block) + ALLOC_BLOCK_SIZE;

		set->blocks = block;
	}

	chunk = (AllocChunk)(block->freeptr);
	chunk->aset = (void *)set;
	chunk->size = chunk_size;
	block->freeptr += (chunk_size + ALLOC_CHUNKHDRSZ);

	return AllocChunkGetPointer(chunk);
}

/*
 * AllocSetFree --
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
	int				fidx;
	AllocChunk		chunk;

	/* AssertArg(AllocSetIsValid(set)); */
	/* AssertArg(AllocPointerIsValid(pointer)); */
	AssertArg(AllocSetContains(set, pointer));

	chunk = AllocPointerGetChunk(pointer);
	fidx = AllocSetFreeIndex(chunk->size);

	chunk->aset = (void *)set->freelist[fidx];
	set->freelist[fidx] = chunk;
}

/*
 * AllocSetRealloc --
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
	 * Chunk sizes are aligned to power of 2 on AllocSetAlloc().
	 * Maybe the allocated area already is >= the new size.
	 *
	 */
	if (AllocPointerGetSize(pointer) >= size)
		return pointer;

	/* allocate new pointer */
	newPointer = AllocSetAlloc(set, size);

	/* fill new memory */
	oldsize = AllocPointerGetSize(pointer);
	memmove(newPointer, pointer, (oldsize < size) ? oldsize : size);

	/* free old pointer */
	AllocSetFree(set, pointer);

	return newPointer;
}

/*
 * AllocSetDump --
 *		Displays allocated set.
 */
void
AllocSetDump(AllocSet set)
{
	elog(DEBUG, "Currently unable to dump AllocSet");
}
