/*-------------------------------------------------------------------------
 *
 * memnodes.h
 *	  POSTGRES memory context node definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/memnodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MEMNODES_H
#define MEMNODES_H

#include "nodes/nodes.h"

/*
 * MemoryContextCounters
 *		Summarization state for MemoryContextStats collection.
 *
 * The set of counters in this struct is biased towards AllocSet; if we ever
 * add any context types that are based on fundamentally different approaches,
 * we might need more or different counters here.  A possible API spec then
 * would be to print only nonzero counters, but for now we just summarize in
 * the format historically used by AllocSet.
 */
typedef struct MemoryContextCounters
{
	Size		nblocks;		/* Total number of malloc blocks */
	Size		freechunks;		/* Total number of free chunks */
	Size		totalspace;		/* Total bytes requested from malloc */
	Size		freespace;		/* The unused portion of totalspace */
} MemoryContextCounters;

/*
 * MemoryContext
 *		A logical context in which memory allocations occur.
 *
 * MemoryContext itself is an abstract type that can have multiple
 * implementations.
 * The function pointers in MemoryContextMethods define one specific
 * implementation of MemoryContext --- they are a virtual function table
 * in C++ terms.
 *
 * Node types that are actual implementations of memory contexts must
 * begin with the same fields as MemoryContextData.
 *
 * Note: for largely historical reasons, typedef MemoryContext is a pointer
 * to the context struct rather than the struct type itself.
 */

typedef void (*MemoryStatsPrintFunc) (MemoryContext context, void *passthru,
									  const char *stats_string,
									  bool print_to_stderr);

typedef struct MemoryContextMethods
{
	/*
	 * Function to handle memory allocation requests of 'size' to allocate
	 * memory into the given 'context'.  The function must handle flags
	 * MCXT_ALLOC_HUGE and MCXT_ALLOC_NO_OOM.  MCXT_ALLOC_ZERO is handled by
	 * the calling function.
	 */
	void	   *(*alloc) (MemoryContext context, Size size, int flags);

	/* call this free_p in case someone #define's free() */
	void		(*free_p) (void *pointer);

	/*
	 * Function to handle a size change request for an existing allocation.
	 * The implementation must handle flags MCXT_ALLOC_HUGE and
	 * MCXT_ALLOC_NO_OOM.  MCXT_ALLOC_ZERO is handled by the calling function.
	 */
	void	   *(*realloc) (void *pointer, Size size, int flags);

	/*
	 * Invalidate all previous allocations in the given memory context and
	 * prepare the context for a new set of allocations.  Implementations may
	 * optionally free() excess memory back to the OS during this time.
	 */
	void		(*reset) (MemoryContext context);

	/* Free all memory consumed by the given MemoryContext. */
	void		(*delete_context) (MemoryContext context);

	/* Return the MemoryContext that the given pointer belongs to. */
	MemoryContext (*get_chunk_context) (void *pointer);

	/*
	 * Return the number of bytes consumed by the given pointer within its
	 * memory context, including the overhead of alignment and chunk headers.
	 */
	Size		(*get_chunk_space) (void *pointer);

	/*
	 * Return true if the given MemoryContext has not had any allocations
	 * since it was created or last reset.
	 */
	bool		(*is_empty) (MemoryContext context);
	void		(*stats) (MemoryContext context,
						  MemoryStatsPrintFunc printfunc, void *passthru,
						  MemoryContextCounters *totals,
						  bool print_to_stderr);
#ifdef MEMORY_CONTEXT_CHECKING

	/*
	 * Perform validation checks on the given context and raise any discovered
	 * anomalies as WARNINGs.
	 */
	void		(*check) (MemoryContext context);
#endif
} MemoryContextMethods;


typedef struct MemoryContextData
{
	pg_node_attr(abstract)		/* there are no nodes of this type */

	NodeTag		type;			/* identifies exact kind of context */
	/* these two fields are placed here to minimize alignment wastage: */
	bool		isReset;		/* T = no space alloced since last reset */
	bool		allowInCritSection; /* allow palloc in critical section */
	Size		mem_allocated;	/* track memory allocated for this context */
	const MemoryContextMethods *methods;	/* virtual function table */
	MemoryContext parent;		/* NULL if no parent (toplevel context) */
	MemoryContext firstchild;	/* head of linked list of children */
	MemoryContext prevchild;	/* previous child of same parent */
	MemoryContext nextchild;	/* next child of same parent */
	const char *name;			/* context name */
	const char *ident;			/* context ID if any */
	MemoryContextCallback *reset_cbs;	/* list of reset/delete callbacks */
} MemoryContextData;

/* utils/palloc.h contains typedef struct MemoryContextData *MemoryContext */


/*
 * MemoryContextIsValid
 *		True iff memory context is valid.
 *
 * Add new context types to the set accepted by this macro.
 */
#define MemoryContextIsValid(context) \
	((context) != NULL && \
	 (IsA((context), AllocSetContext) || \
	  IsA((context), SlabContext) || \
	  IsA((context), GenerationContext) || \
	  IsA((context), BumpContext)))

#endif							/* MEMNODES_H */
