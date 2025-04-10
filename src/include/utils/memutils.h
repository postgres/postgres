/*-------------------------------------------------------------------------
 *
 * memutils.h
 *	  This file contains declarations for memory allocation utility
 *	  functions.  These are functions that are not quite widely used
 *	  enough to justify going in utils/palloc.h, but are still part
 *	  of the API of the memory management subsystem.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/memutils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MEMUTILS_H
#define MEMUTILS_H

#include "nodes/memnodes.h"
#include "storage/condition_variable.h"
#include "storage/lmgr.h"
#include "utils/dsa.h"


/*
 * MaxAllocSize, MaxAllocHugeSize
 *		Quasi-arbitrary limits on size of allocations.
 *
 * Note:
 *		There is no guarantee that smaller allocations will succeed, but
 *		larger requests will be summarily denied.
 *
 * palloc() enforces MaxAllocSize, chosen to correspond to the limiting size
 * of varlena objects under TOAST.  See VARSIZE_4B() and related macros in
 * postgres.h.  Many datatypes assume that any allocatable size can be
 * represented in a varlena header.  This limit also permits a caller to use
 * an "int" variable for an index into or length of an allocation.  Callers
 * careful to avoid these hazards can access the higher limit with
 * MemoryContextAllocHuge().  Both limits permit code to assume that it may
 * compute twice an allocation's size without overflow.
 */
#define MaxAllocSize	((Size) 0x3fffffff) /* 1 gigabyte - 1 */

#define AllocSizeIsValid(size)	((Size) (size) <= MaxAllocSize)

/* Must be less than SIZE_MAX */
#define MaxAllocHugeSize	(SIZE_MAX / 2)

#define InvalidAllocSize	SIZE_MAX

#define AllocHugeSizeIsValid(size)	((Size) (size) <= MaxAllocHugeSize)

/*
 * Memory Context reporting size limits.
 */

/* Max length of context name and ident */
#define MEMORY_CONTEXT_IDENT_SHMEM_SIZE 64
/* Maximum size (in bytes) of DSA area per process */
#define MEMORY_CONTEXT_REPORT_MAX_PER_BACKEND  ((size_t) (1 * 1024 * 1024))

/*
 * Maximum size per context. Actual size may be lower as this assumes the worst
 * case of deepest path and longest identifiers (name and ident, thus the
 * multiplication by 2). The path depth is limited to 100 like for memory
 * context logging.
 */
#define MAX_MEMORY_CONTEXT_STATS_SIZE (sizeof(MemoryStatsEntry) + \
	(100 * sizeof(int)) + (2 * MEMORY_CONTEXT_IDENT_SHMEM_SIZE))

/*
 * Standard top-level memory contexts.
 *
 * Only TopMemoryContext and ErrorContext are initialized by
 * MemoryContextInit() itself.
 */
extern PGDLLIMPORT MemoryContext TopMemoryContext;
extern PGDLLIMPORT MemoryContext ErrorContext;
extern PGDLLIMPORT MemoryContext PostmasterContext;
extern PGDLLIMPORT MemoryContext CacheMemoryContext;
extern PGDLLIMPORT MemoryContext MessageContext;
extern PGDLLIMPORT MemoryContext TopTransactionContext;
extern PGDLLIMPORT MemoryContext CurTransactionContext;

/* This is a transient link to the active portal's memory context: */
extern PGDLLIMPORT MemoryContext PortalContext;


/*
 * Memory-context-type-independent functions in mcxt.c
 */
extern void MemoryContextInit(void);
extern void MemoryContextReset(MemoryContext context);
extern void MemoryContextDelete(MemoryContext context);
extern void MemoryContextResetOnly(MemoryContext context);
extern void MemoryContextResetChildren(MemoryContext context);
extern void MemoryContextDeleteChildren(MemoryContext context);
extern void MemoryContextSetIdentifier(MemoryContext context, const char *id);
extern void MemoryContextSetParent(MemoryContext context,
								   MemoryContext new_parent);
extern MemoryContext GetMemoryChunkContext(void *pointer);
extern Size GetMemoryChunkSpace(void *pointer);
extern MemoryContext MemoryContextGetParent(MemoryContext context);
extern bool MemoryContextIsEmpty(MemoryContext context);
extern Size MemoryContextMemAllocated(MemoryContext context, bool recurse);
extern void MemoryContextMemConsumed(MemoryContext context,
									 MemoryContextCounters *consumed);
extern void MemoryContextStats(MemoryContext context);
extern void MemoryContextStatsDetail(MemoryContext context,
									 int max_level, int max_children,
									 bool print_to_stderr);
extern void MemoryContextAllowInCriticalSection(MemoryContext context,
												bool allow);

#ifdef MEMORY_CONTEXT_CHECKING
extern void MemoryContextCheck(MemoryContext context);
#endif

/* Handy macro for copying and assigning context ID ... but note double eval */
#define MemoryContextCopyAndSetIdentifier(cxt, id) \
	MemoryContextSetIdentifier(cxt, MemoryContextStrdup(cxt, id))

extern void HandleLogMemoryContextInterrupt(void);
extern void ProcessLogMemoryContextInterrupt(void);

/*
 * Memory-context-type-specific functions
 */

/* aset.c */
extern MemoryContext AllocSetContextCreateInternal(MemoryContext parent,
												   const char *name,
												   Size minContextSize,
												   Size initBlockSize,
												   Size maxBlockSize);

/*
 * This wrapper macro exists to check for non-constant strings used as context
 * names; that's no longer supported.  (Use MemoryContextSetIdentifier if you
 * want to provide a variable identifier.)
 */
#ifdef HAVE__BUILTIN_CONSTANT_P
#define AllocSetContextCreate(parent, name, ...) \
	(StaticAssertExpr(__builtin_constant_p(name), \
					  "memory context names must be constant strings"), \
	 AllocSetContextCreateInternal(parent, name, __VA_ARGS__))
#else
#define AllocSetContextCreate \
	AllocSetContextCreateInternal
#endif

/* slab.c */
extern MemoryContext SlabContextCreate(MemoryContext parent,
									   const char *name,
									   Size blockSize,
									   Size chunkSize);

/* generation.c */
extern MemoryContext GenerationContextCreate(MemoryContext parent,
											 const char *name,
											 Size minContextSize,
											 Size initBlockSize,
											 Size maxBlockSize);

/* bump.c */
extern MemoryContext BumpContextCreate(MemoryContext parent,
									   const char *name,
									   Size minContextSize,
									   Size initBlockSize,
									   Size maxBlockSize);

/*
 * Recommended default alloc parameters, suitable for "ordinary" contexts
 * that might hold quite a lot of data.
 */
#define ALLOCSET_DEFAULT_MINSIZE   0
#define ALLOCSET_DEFAULT_INITSIZE  (8 * 1024)
#define ALLOCSET_DEFAULT_MAXSIZE   (8 * 1024 * 1024)
#define ALLOCSET_DEFAULT_SIZES \
	ALLOCSET_DEFAULT_MINSIZE, ALLOCSET_DEFAULT_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE

/*
 * Recommended alloc parameters for "small" contexts that are never expected
 * to contain much data (for example, a context to contain a query plan).
 */
#define ALLOCSET_SMALL_MINSIZE	 0
#define ALLOCSET_SMALL_INITSIZE  (1 * 1024)
#define ALLOCSET_SMALL_MAXSIZE	 (8 * 1024)
#define ALLOCSET_SMALL_SIZES \
	ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE, ALLOCSET_SMALL_MAXSIZE

/*
 * Recommended alloc parameters for contexts that should start out small,
 * but might sometimes grow big.
 */
#define ALLOCSET_START_SMALL_SIZES \
	ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE


/*
 * Threshold above which a request in an AllocSet context is certain to be
 * allocated separately (and thereby have constant allocation overhead).
 * Few callers should be interested in this, but tuplesort/tuplestore need
 * to know it.
 */
#define ALLOCSET_SEPARATE_THRESHOLD  8192

#define SLAB_DEFAULT_BLOCK_SIZE		(8 * 1024)
#define SLAB_LARGE_BLOCK_SIZE		(8 * 1024 * 1024)

/*
 * pg_memory_is_all_zeros
 *
 * Test if a memory region starting at "ptr" and of size "len" is full of
 * zeroes.
 *
 * The test is divided into multiple cases for safety reason and multiple
 * phases for efficiency.
 *
 * Case 1: len < sizeof(size_t) bytes, then byte-by-byte comparison.
 * Case 2: len < (sizeof(size_t) * 8 - 1) bytes:
 *       - Phase 1: byte-by-byte comparison, until the pointer is aligned.
 *       - Phase 2: size_t comparisons, with aligned pointers, up to the last
 *                  location possible.
 *       - Phase 3: byte-by-byte comparison, until the end location.
 * Case 3: len >= (sizeof(size_t) * 8) bytes, same as case 2 except that an
 *         additional phase is placed between Phase 1 and Phase 2, with
 *         (8 * sizeof(size_t)) comparisons using bitwise OR to encourage
 *         compilers to use SIMD instructions if available, up to the last
 *         aligned location possible.
 *
 * Case 1 and Case 2 are mandatory to ensure that we won't read beyond the
 * memory area.  This is portable for 32-bit and 64-bit architectures.
 *
 * Caller must ensure that "ptr" is not NULL.
 */
static inline bool
pg_memory_is_all_zeros(const void *ptr, size_t len)
{
	const unsigned char *p = (const unsigned char *) ptr;
	const unsigned char *end = &p[len];
	const unsigned char *aligned_end = (const unsigned char *)
		((uintptr_t) end & (~(sizeof(size_t) - 1)));

	if (len < sizeof(size_t))
	{
		while (p < end)
		{
			if (*p++ != 0)
				return false;
		}
		return true;
	}

	/* "len" in the [sizeof(size_t), sizeof(size_t) * 8 - 1] range */
	if (len < sizeof(size_t) * 8)
	{
		/* Compare bytes until the pointer "p" is aligned */
		while (((uintptr_t) p & (sizeof(size_t) - 1)) != 0)
		{
			if (p == end)
				return true;
			if (*p++ != 0)
				return false;
		}

		/*
		 * Compare remaining size_t-aligned chunks.
		 *
		 * There is no risk to read beyond the memory area, as "aligned_end"
		 * cannot be higher than "end".
		 */
		for (; p < aligned_end; p += sizeof(size_t))
		{
			if (*(size_t *) p != 0)
				return false;
		}

		/* Compare remaining bytes until the end */
		while (p < end)
		{
			if (*p++ != 0)
				return false;
		}
		return true;
	}

	/* "len" in the [sizeof(size_t) * 8, inf) range */

	/* Compare bytes until the pointer "p" is aligned */
	while (((uintptr_t) p & (sizeof(size_t) - 1)) != 0)
	{
		if (p == end)
			return true;

		if (*p++ != 0)
			return false;
	}

	/*
	 * Compare 8 * sizeof(size_t) chunks at once.
	 *
	 * For performance reasons, we manually unroll this loop and purposefully
	 * use bitwise-ORs to combine each comparison.  This prevents boolean
	 * short-circuiting and lets the compiler know that it's safe to access
	 * all 8 elements regardless of the result of the other comparisons.  This
	 * seems to be enough to coax a few compilers into using SIMD
	 * instructions.
	 */
	for (; p < aligned_end - (sizeof(size_t) * 7); p += sizeof(size_t) * 8)
	{
		if ((((size_t *) p)[0] != 0) | (((size_t *) p)[1] != 0) |
			(((size_t *) p)[2] != 0) | (((size_t *) p)[3] != 0) |
			(((size_t *) p)[4] != 0) | (((size_t *) p)[5] != 0) |
			(((size_t *) p)[6] != 0) | (((size_t *) p)[7] != 0))
			return false;
	}

	/*
	 * Compare remaining size_t-aligned chunks.
	 *
	 * There is no risk to read beyond the memory area, as "aligned_end"
	 * cannot be higher than "end".
	 */
	for (; p < aligned_end; p += sizeof(size_t))
	{
		if (*(size_t *) p != 0)
			return false;
	}

	/* Compare remaining bytes until the end */
	while (p < end)
	{
		if (*p++ != 0)
			return false;
	}

	return true;
}

/* Dynamic shared memory state for statistics per context */
typedef struct MemoryStatsEntry
{
	dsa_pointer name;
	dsa_pointer ident;
	dsa_pointer path;
	NodeTag		type;
	int			path_length;
	int			levels;
	int64		totalspace;
	int64		nblocks;
	int64		freespace;
	int64		freechunks;
	int			num_agg_stats;
} MemoryStatsEntry;

/*
 * Static shared memory state representing the DSA area created for memory
 * context statistics reporting.  A single DSA area is created and used by all
 * the processes, each having its specific DSA allocations for sharing memory
 * statistics, tracked by per backend static shared memory state.
 */
typedef struct MemoryStatsCtl
{
	dsa_handle	memstats_dsa_handle;
	LWLock		lw_lock;
} MemoryStatsCtl;

/*
 * Per backend static shared memory state for memory context statistics
 * reporting.
 */
typedef struct MemoryStatsBackendState
{
	ConditionVariable memcxt_cv;
	LWLock		lw_lock;
	int			proc_id;
	int			total_stats;
	bool		summary;
	dsa_pointer memstats_dsa_pointer;
	TimestampTz stats_timestamp;
} MemoryStatsBackendState;


/*
 * Used for storage of transient identifiers for pg_get_backend_memory_contexts
 */
typedef struct MemoryStatsContextId
{
	MemoryContext context;
	int			context_id;
} MemoryStatsContextId;

extern PGDLLIMPORT MemoryStatsBackendState *memCxtState;
extern PGDLLIMPORT MemoryStatsCtl *memCxtArea;
extern PGDLLIMPORT dsa_area *MemoryStatsDsaArea;
extern void ProcessGetMemoryContextInterrupt(void);
extern const char *ContextTypeToString(NodeTag type);
extern void HandleGetMemoryContextInterrupt(void);
extern Size MemoryContextReportingShmemSize(void);
extern void MemoryContextReportingShmemInit(void);
extern void AtProcExit_memstats_cleanup(int code, Datum arg);
#endif							/* MEMUTILS_H */
