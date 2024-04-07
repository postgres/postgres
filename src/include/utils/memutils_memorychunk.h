/*-------------------------------------------------------------------------
 *
 * memutils_memorychunk.h
 *	  Here we define a struct named MemoryChunk which implementations of
 *	  MemoryContexts may use as a header for chunks of memory they allocate.
 *
 * MemoryChunk provides a lightweight header that a MemoryContext can use to
 * store a reference back to the block which the given chunk is allocated on
 * and also an additional 30-bits to store another value such as the size of
 * the allocated chunk.
 *
 * Although MemoryChunks are used by each of our MemoryContexts, future
 * implementations may choose to implement their own method for storing chunk
 * headers.  The only requirement is that the header ends with an 8-byte value
 * which the least significant 4-bits of are set to the MemoryContextMethodID
 * of the given context.
 *
 * By default, a MemoryChunk is 8 bytes in size, however, when
 * MEMORY_CONTEXT_CHECKING is defined the header becomes 16 bytes in size due
 * to the additional requested_size field.  The MemoryContext may use this
 * field for whatever they wish, but it is intended to be used for additional
 * checks which are only done in MEMORY_CONTEXT_CHECKING builds.
 *
 * The MemoryChunk contains a uint64 field named 'hdrmask'.  This field is
 * used to encode 4 separate pieces of information.  Starting with the least
 * significant bits of 'hdrmask', the bit space is reserved as follows:
 *
 * 1.	4-bits to indicate the MemoryContextMethodID as defined by
 *		MEMORY_CONTEXT_METHODID_MASK
 * 2.	1-bit to denote an "external" chunk (see below)
 * 3.	30-bits reserved for the MemoryContext to use for anything it
 *		requires.  Most MemoryContexts likely want to store the size of the
 *		chunk here.
 * 4.	30-bits for the number of bytes that must be subtracted from the chunk
 *		to obtain the address of the block that the chunk is stored on.
 *
 * If you're paying close attention, you'll notice this adds up to 65 bits
 * rather than 64 bits.  This is because the highest-order bit of #3 is the
 * same bit as the lowest-order bit of #4.  We can do this as we insist that
 * the chunk and block pointers are both MAXALIGNed, therefore the relative
 * offset between those will always be a MAXALIGNed value which means the
 * lowest order bit is always 0.  When fetching the chunk to block offset we
 * mask out the lowest-order bit to ensure it's still zero.
 *
 * In some cases, for example when memory allocations become large, it's
 * possible fields 3 and 4 above are not large enough to store the values
 * required for the chunk.  In this case, the MemoryContext can choose to mark
 * the chunk as "external" by calling the MemoryChunkSetHdrMaskExternal()
 * function.  When this is done, fields 3 and 4 are unavailable for use by the
 * MemoryContext and it's up to the MemoryContext itself to devise its own
 * method for getting the reference to the block.
 *
 * Interface:
 *
 * MemoryChunkSetHdrMask:
 *		Used to set up a non-external MemoryChunk.
 *
 * MemoryChunkSetHdrMaskExternal:
 *		Used to set up an externally managed MemoryChunk.
 *
 * MemoryChunkIsExternal:
 *		Determine if the given MemoryChunk is externally managed, i.e.
 *		MemoryChunkSetHdrMaskExternal() was called on the chunk.
 *
 * MemoryChunkGetValue:
 *		For non-external chunks, return the stored 30-bit value as it was set
 *		in the call to MemoryChunkSetHdrMask().
 *
 * MemoryChunkGetBlock:
 *		For non-external chunks, return a pointer to the block as it was set
 *		in the call to MemoryChunkSetHdrMask().
 *
 * Also exports:
 *		MEMORYCHUNK_MAX_VALUE
 *		MEMORYCHUNK_MAX_BLOCKOFFSET
 *		PointerGetMemoryChunk
 *		MemoryChunkGetPointer
 *
 * Portions Copyright (c) 2022-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/memutils_memorychunk.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef MEMUTILS_MEMORYCHUNK_H
#define MEMUTILS_MEMORYCHUNK_H

#include "utils/memutils_internal.h"

 /*
  * The maximum allowed value that MemoryContexts can store in the value
  * field.  Must be 1 less than a power of 2.
  */
#define MEMORYCHUNK_MAX_VALUE			UINT64CONST(0x3FFFFFFF)

/*
 * The maximum distance in bytes that a MemoryChunk can be offset from the
 * block that is storing the chunk.  Must be 1 less than a power of 2.
 */
#define MEMORYCHUNK_MAX_BLOCKOFFSET		UINT64CONST(0x3FFFFFFF)

/*
 * As above, but mask out the lowest-order (always zero) bit as this is shared
 * with the MemoryChunkGetValue field.
 */
#define MEMORYCHUNK_BLOCKOFFSET_MASK 	UINT64CONST(0x3FFFFFFE)

/* define the least significant base-0 bit of each portion of the hdrmask */
#define MEMORYCHUNK_EXTERNAL_BASEBIT	MEMORY_CONTEXT_METHODID_BITS
#define MEMORYCHUNK_VALUE_BASEBIT		(MEMORYCHUNK_EXTERNAL_BASEBIT + 1)
#define MEMORYCHUNK_BLOCKOFFSET_BASEBIT	(MEMORYCHUNK_VALUE_BASEBIT + 29)

/*
 * A magic number for storing in the free bits of an external chunk.  This
 * must mask out the bits used for storing the MemoryContextMethodID and the
 * external bit.
 */
#define MEMORYCHUNK_MAGIC		(UINT64CONST(0xB1A8DB858EB6EFBA) >> \
								 MEMORYCHUNK_VALUE_BASEBIT << \
								 MEMORYCHUNK_VALUE_BASEBIT)

typedef struct MemoryChunk
{
#ifdef MEMORY_CONTEXT_CHECKING
	Size		requested_size;
#endif

	/* bitfield for storing details about the chunk */
	uint64		hdrmask;		/* must be last */
} MemoryChunk;

/* Get the MemoryChunk from the pointer */
#define PointerGetMemoryChunk(p) \
	((MemoryChunk *) ((char *) (p) - sizeof(MemoryChunk)))
/* Get the pointer from the MemoryChunk */
#define MemoryChunkGetPointer(c) \
	((void *) ((char *) (c) + sizeof(MemoryChunk)))

/* private macros for making the inline functions below more simple */
#define HdrMaskIsExternal(hdrmask) \
	((hdrmask) & (((uint64) 1) << MEMORYCHUNK_EXTERNAL_BASEBIT))
#define HdrMaskGetValue(hdrmask) \
	(((hdrmask) >> MEMORYCHUNK_VALUE_BASEBIT) & MEMORYCHUNK_MAX_VALUE)

/*
 * Shift the block offset down to the 0th bit position and mask off the single
 * bit that's shared with the MemoryChunkGetValue field.
 */
#define HdrMaskBlockOffset(hdrmask) \
	(((hdrmask) >> MEMORYCHUNK_BLOCKOFFSET_BASEBIT) & MEMORYCHUNK_BLOCKOFFSET_MASK)

/* For external chunks only, check the magic number matches */
#define HdrMaskCheckMagic(hdrmask) \
	(MEMORYCHUNK_MAGIC == \
	 ((hdrmask) >> MEMORYCHUNK_VALUE_BASEBIT << MEMORYCHUNK_VALUE_BASEBIT))
/*
 * MemoryChunkSetHdrMask
 *		Store the given 'block', 'chunk_size' and 'methodid' in the given
 *		MemoryChunk.
 *
 * The number of bytes between 'block' and 'chunk' must be <=
 * MEMORYCHUNK_MAX_BLOCKOFFSET.
 * 'value' must be <= MEMORYCHUNK_MAX_VALUE.
 * Both 'chunk' and 'block' must be MAXALIGNed pointers.
 */
static inline void
MemoryChunkSetHdrMask(MemoryChunk *chunk, void *block,
					  Size value, MemoryContextMethodID methodid)
{
	Size		blockoffset = (char *) chunk - (char *) block;

	Assert((char *) chunk >= (char *) block);
	Assert((blockoffset & MEMORYCHUNK_BLOCKOFFSET_MASK) == blockoffset);
	Assert(value <= MEMORYCHUNK_MAX_VALUE);
	Assert((int) methodid <= MEMORY_CONTEXT_METHODID_MASK);

	chunk->hdrmask = (((uint64) blockoffset) << MEMORYCHUNK_BLOCKOFFSET_BASEBIT) |
		(((uint64) value) << MEMORYCHUNK_VALUE_BASEBIT) |
		methodid;
}

/*
 * MemoryChunkSetHdrMaskExternal
 *		Set 'chunk' as an externally managed chunk.  Here we only record the
 *		MemoryContextMethodID and set the external chunk bit.
 */
static inline void
MemoryChunkSetHdrMaskExternal(MemoryChunk *chunk,
							  MemoryContextMethodID methodid)
{
	Assert((int) methodid <= MEMORY_CONTEXT_METHODID_MASK);

	chunk->hdrmask = MEMORYCHUNK_MAGIC | (((uint64) 1) << MEMORYCHUNK_EXTERNAL_BASEBIT) |
		methodid;
}

/*
 * MemoryChunkIsExternal
 *		Return true if 'chunk' is marked as external.
 */
static inline bool
MemoryChunkIsExternal(MemoryChunk *chunk)
{
	/*
	 * External chunks should always store MEMORYCHUNK_MAGIC in the upper
	 * portion of the hdrmask, check that nothing has stomped on that.
	 */
	Assert(!HdrMaskIsExternal(chunk->hdrmask) ||
		   HdrMaskCheckMagic(chunk->hdrmask));

	return HdrMaskIsExternal(chunk->hdrmask);
}

/*
 * MemoryChunkGetValue
 *		For non-external chunks, returns the value field as it was set in
 *		MemoryChunkSetHdrMask.
 */
static inline Size
MemoryChunkGetValue(MemoryChunk *chunk)
{
	Assert(!HdrMaskIsExternal(chunk->hdrmask));

	return HdrMaskGetValue(chunk->hdrmask);
}

/*
 * MemoryChunkGetBlock
 *		For non-external chunks, returns the pointer to the block as was set
 *		in MemoryChunkSetHdrMask.
 */
static inline void *
MemoryChunkGetBlock(MemoryChunk *chunk)
{
	Assert(!HdrMaskIsExternal(chunk->hdrmask));

	return (void *) ((char *) chunk - HdrMaskBlockOffset(chunk->hdrmask));
}

/* cleanup all internal definitions */
#undef MEMORYCHUNK_BLOCKOFFSET_MASK
#undef MEMORYCHUNK_EXTERNAL_BASEBIT
#undef MEMORYCHUNK_VALUE_BASEBIT
#undef MEMORYCHUNK_BLOCKOFFSET_BASEBIT
#undef MEMORYCHUNK_MAGIC
#undef HdrMaskIsExternal
#undef HdrMaskGetValue
#undef HdrMaskBlockOffset
#undef HdrMaskCheckMagic

#endif							/* MEMUTILS_MEMORYCHUNK_H */
