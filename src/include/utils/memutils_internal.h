/*-------------------------------------------------------------------------
 *
 * memutils_internal.h
 *	  This file contains declarations for memory allocation utility
 *	  functions for internal use.
 *
 *
 * Portions Copyright (c) 2022-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/memutils_internal.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef MEMUTILS_INTERNAL_H
#define MEMUTILS_INTERNAL_H

#include "utils/memutils.h"

/* These functions implement the MemoryContext API for AllocSet context. */
extern void *AllocSetAlloc(MemoryContext context, Size size);
extern void AllocSetFree(void *pointer);
extern void *AllocSetRealloc(void *pointer, Size size);
extern void AllocSetReset(MemoryContext context);
extern void AllocSetDelete(MemoryContext context);
extern MemoryContext AllocSetGetChunkContext(void *pointer);
extern Size AllocSetGetChunkSpace(void *pointer);
extern bool AllocSetIsEmpty(MemoryContext context);
extern void AllocSetStats(MemoryContext context,
						  MemoryStatsPrintFunc printfunc, void *passthru,
						  MemoryContextCounters *totals,
						  bool print_to_stderr);
#ifdef MEMORY_CONTEXT_CHECKING
extern void AllocSetCheck(MemoryContext context);
#endif

/* These functions implement the MemoryContext API for Generation context. */
extern void *GenerationAlloc(MemoryContext context, Size size);
extern void GenerationFree(void *pointer);
extern void *GenerationRealloc(void *pointer, Size size);
extern void GenerationReset(MemoryContext context);
extern void GenerationDelete(MemoryContext context);
extern MemoryContext GenerationGetChunkContext(void *pointer);
extern Size GenerationGetChunkSpace(void *pointer);
extern bool GenerationIsEmpty(MemoryContext context);
extern void GenerationStats(MemoryContext context,
							MemoryStatsPrintFunc printfunc, void *passthru,
							MemoryContextCounters *totals,
							bool print_to_stderr);
#ifdef MEMORY_CONTEXT_CHECKING
extern void GenerationCheck(MemoryContext context);
#endif


/* These functions implement the MemoryContext API for Slab context. */
extern void *SlabAlloc(MemoryContext context, Size size);
extern void SlabFree(void *pointer);
extern void *SlabRealloc(void *pointer, Size size);
extern void SlabReset(MemoryContext context);
extern void SlabDelete(MemoryContext context);
extern MemoryContext SlabGetChunkContext(void *pointer);
extern Size SlabGetChunkSpace(void *pointer);
extern bool SlabIsEmpty(MemoryContext context);
extern void SlabStats(MemoryContext context,
					  MemoryStatsPrintFunc printfunc, void *passthru,
					  MemoryContextCounters *totals,
					  bool print_to_stderr);
#ifdef MEMORY_CONTEXT_CHECKING
extern void SlabCheck(MemoryContext context);
#endif

/*
 * These functions support the implementation of palloc_aligned() and are not
 * part of a fully-fledged MemoryContext type.
 */
extern void AlignedAllocFree(void *pointer);
extern void *AlignedAllocRealloc(void *pointer, Size size);
extern MemoryContext AlignedAllocGetChunkContext(void *pointer);
extern Size AlignedAllocGetChunkSpace(void *pointer);

/*
 * How many extra bytes do we need to request in order to ensure that we can
 * align a pointer to 'alignto'.  Since palloc'd pointers are already aligned
 * to MAXIMUM_ALIGNOF we can subtract that amount.  We also need to make sure
 * there is enough space for the redirection MemoryChunk.
 */
#define PallocAlignedExtraBytes(alignto) \
	((alignto) + (sizeof(MemoryChunk) - MAXIMUM_ALIGNOF))

/*
 * MemoryContextMethodID
 *		A unique identifier for each MemoryContext implementation which
 *		indicates the index into the mcxt_methods[] array. See mcxt.c.
 *
 * For robust error detection, ensure that MemoryContextMethodID has a value
 * for each possible bit-pattern of MEMORY_CONTEXT_METHODID_MASK, and make
 * dummy entries for unused IDs in the mcxt_methods[] array.  We also try
 * to avoid using bit-patterns as valid IDs if they are likely to occur in
 * garbage data, or if they could falsely match on chunks that are really from
 * malloc not palloc.  (We can't tell that for most malloc implementations,
 * but it happens that glibc stores flag bits in the same place where we put
 * the MemoryContextMethodID, so the possible values are predictable for it.)
 */
typedef enum MemoryContextMethodID
{
	MCTX_UNUSED1_ID,			/* 000 occurs in never-used memory */
	MCTX_UNUSED2_ID,			/* glibc malloc'd chunks usually match 001 */
	MCTX_UNUSED3_ID,			/* glibc malloc'd chunks > 128kB match 010 */
	MCTX_ASET_ID,
	MCTX_GENERATION_ID,
	MCTX_SLAB_ID,
	MCTX_ALIGNED_REDIRECT_ID,
	MCTX_UNUSED4_ID				/* 111 occurs in wipe_mem'd memory */
} MemoryContextMethodID;

/*
 * The number of bits that 8-byte memory chunk headers can use to encode the
 * MemoryContextMethodID.
 */
#define MEMORY_CONTEXT_METHODID_BITS 3
#define MEMORY_CONTEXT_METHODID_MASK \
	((((uint64) 1) << MEMORY_CONTEXT_METHODID_BITS) - 1)

/*
 * This routine handles the context-type-independent part of memory
 * context creation.  It's intended to be called from context-type-
 * specific creation routines, and noplace else.
 */
extern void MemoryContextCreate(MemoryContext node,
								NodeTag tag,
								MemoryContextMethodID method_id,
								MemoryContext parent,
								const char *name);

#endif							/* MEMUTILS_INTERNAL_H */
