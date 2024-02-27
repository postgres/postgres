/*-------------------------------------------------------------------------
 *
 * alignedalloc.c
 *	  Allocator functions to implement palloc_aligned
 *
 * This is not a fully-fledged MemoryContext type as there is no means to
 * create a MemoryContext of this type.  The code here only serves to allow
 * operations such as pfree() and repalloc() to work correctly on a memory
 * chunk that was allocated by palloc_aligned().
 *
 * Portions Copyright (c) 2022-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/mmgr/alignedalloc.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/memdebug.h"
#include "utils/memutils_memorychunk.h"

/*
 * AlignedAllocFree
*		Frees allocated memory; memory is removed from its owning context.
*/
void
AlignedAllocFree(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	void	   *unaligned;

	VALGRIND_MAKE_MEM_DEFINED(chunk, sizeof(MemoryChunk));

	Assert(!MemoryChunkIsExternal(chunk));

	/* obtain the original (unaligned) allocated pointer */
	unaligned = MemoryChunkGetBlock(chunk);

#ifdef MEMORY_CONTEXT_CHECKING
	/* Test for someone scribbling on unused space in chunk */
	if (!sentinel_ok(pointer, chunk->requested_size))
		elog(WARNING, "detected write past chunk end in %s %p",
			 GetMemoryChunkContext(unaligned)->name, chunk);
#endif

	pfree(unaligned);
}

/*
 * AlignedAllocRealloc
 *		Change the allocated size of a chunk and return possibly a different
 *		pointer to a memory address aligned to the same boundary as the
 *		originally requested alignment.  The contents of 'pointer' will be
 *		copied into the returned pointer up until 'size'.  Any additional
 *		memory will be uninitialized.
 */
void *
AlignedAllocRealloc(void *pointer, Size size, int flags)
{
	MemoryChunk *redirchunk = PointerGetMemoryChunk(pointer);
	Size		alignto;
	void	   *unaligned;
	MemoryContext ctx;
	Size		old_size;
	void	   *newptr;

	VALGRIND_MAKE_MEM_DEFINED(redirchunk, sizeof(MemoryChunk));

	alignto = MemoryChunkGetValue(redirchunk);
	unaligned = MemoryChunkGetBlock(redirchunk);

	/* sanity check this is a power of 2 value */
	Assert((alignto & (alignto - 1)) == 0);

	/*
	 * Determine the size of the original allocation.  We can't determine this
	 * exactly as GetMemoryChunkSpace() returns the total space used for the
	 * allocation, which for contexts like aset includes rounding up to the
	 * next power of 2.  However, this value is just used to memcpy() the old
	 * data into the new allocation, so we only need to concern ourselves with
	 * not reading beyond the end of the original allocation's memory.  The
	 * drawback here is that we may copy more bytes than we need to, which
	 * only amounts to wasted effort.  We can safely subtract the extra bytes
	 * that we requested to allow us to align the pointer.  We must also
	 * subtract the space for the unaligned pointer's MemoryChunk since
	 * GetMemoryChunkSpace should have included that.  This does assume that
	 * all context types use MemoryChunk as a chunk header.
	 */
	old_size = GetMemoryChunkSpace(unaligned) -
		PallocAlignedExtraBytes(alignto) - sizeof(MemoryChunk);

#ifdef MEMORY_CONTEXT_CHECKING
	/* check that GetMemoryChunkSpace returned something realistic */
	Assert(old_size >= redirchunk->requested_size);
#endif

	ctx = GetMemoryChunkContext(unaligned);
	newptr = MemoryContextAllocAligned(ctx, size, alignto, flags);

	/*
	 * We may memcpy beyond the end of the original allocation request size,
	 * so we must mark the entire allocation as defined.
	 */
	if (likely(newptr != NULL))
	{
		VALGRIND_MAKE_MEM_DEFINED(pointer, old_size);
		memcpy(newptr, pointer, Min(size, old_size));
	}
	pfree(unaligned);

	return newptr;
}

/*
 * AlignedAllocGetChunkContext
 *		Return the MemoryContext that 'pointer' belongs to.
 */
MemoryContext
AlignedAllocGetChunkContext(void *pointer)
{
	MemoryChunk *redirchunk = PointerGetMemoryChunk(pointer);
	MemoryContext cxt;

	VALGRIND_MAKE_MEM_DEFINED(redirchunk, sizeof(MemoryChunk));

	Assert(!MemoryChunkIsExternal(redirchunk));

	cxt = GetMemoryChunkContext(MemoryChunkGetBlock(redirchunk));

	VALGRIND_MAKE_MEM_NOACCESS(redirchunk, sizeof(MemoryChunk));

	return cxt;
}

/*
 * AlignedAllocGetChunkSpace
 *		Given a currently-allocated chunk, determine the total space
 *		it occupies (including all memory-allocation overhead).
 */
Size
AlignedAllocGetChunkSpace(void *pointer)
{
	MemoryChunk *redirchunk = PointerGetMemoryChunk(pointer);
	void	   *unaligned;
	Size		space;

	VALGRIND_MAKE_MEM_DEFINED(redirchunk, sizeof(MemoryChunk));

	unaligned = MemoryChunkGetBlock(redirchunk);
	space = GetMemoryChunkSpace(unaligned);

	VALGRIND_MAKE_MEM_NOACCESS(redirchunk, sizeof(MemoryChunk));

	return space;
}
