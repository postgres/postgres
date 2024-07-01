/*-------------------------------------------------------------------------
 *
 * mcxt.c
 *	  POSTGRES memory context management code.
 *
 * This module handles context management operations that are independent
 * of the particular kind of context being operated on.  It calls
 * context-type-specific operations via the function pointers in a
 * context's MemoryContextMethods struct.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/mmgr/mcxt.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"
#include "utils/memutils_internal.h"
#include "utils/memutils_memorychunk.h"


static void BogusFree(void *pointer);
static void *BogusRealloc(void *pointer, Size size, int flags);
static MemoryContext BogusGetChunkContext(void *pointer);
static Size BogusGetChunkSpace(void *pointer);

/*****************************************************************************
 *	  GLOBAL MEMORY															 *
 *****************************************************************************/
#define BOGUS_MCTX(id) \
	[id].free_p = BogusFree, \
	[id].realloc = BogusRealloc, \
	[id].get_chunk_context = BogusGetChunkContext, \
	[id].get_chunk_space = BogusGetChunkSpace

static const MemoryContextMethods mcxt_methods[] = {
	/* aset.c */
	[MCTX_ASET_ID].alloc = AllocSetAlloc,
	[MCTX_ASET_ID].free_p = AllocSetFree,
	[MCTX_ASET_ID].realloc = AllocSetRealloc,
	[MCTX_ASET_ID].reset = AllocSetReset,
	[MCTX_ASET_ID].delete_context = AllocSetDelete,
	[MCTX_ASET_ID].get_chunk_context = AllocSetGetChunkContext,
	[MCTX_ASET_ID].get_chunk_space = AllocSetGetChunkSpace,
	[MCTX_ASET_ID].is_empty = AllocSetIsEmpty,
	[MCTX_ASET_ID].stats = AllocSetStats,
#ifdef MEMORY_CONTEXT_CHECKING
	[MCTX_ASET_ID].check = AllocSetCheck,
#endif

	/* generation.c */
	[MCTX_GENERATION_ID].alloc = GenerationAlloc,
	[MCTX_GENERATION_ID].free_p = GenerationFree,
	[MCTX_GENERATION_ID].realloc = GenerationRealloc,
	[MCTX_GENERATION_ID].reset = GenerationReset,
	[MCTX_GENERATION_ID].delete_context = GenerationDelete,
	[MCTX_GENERATION_ID].get_chunk_context = GenerationGetChunkContext,
	[MCTX_GENERATION_ID].get_chunk_space = GenerationGetChunkSpace,
	[MCTX_GENERATION_ID].is_empty = GenerationIsEmpty,
	[MCTX_GENERATION_ID].stats = GenerationStats,
#ifdef MEMORY_CONTEXT_CHECKING
	[MCTX_GENERATION_ID].check = GenerationCheck,
#endif

	/* slab.c */
	[MCTX_SLAB_ID].alloc = SlabAlloc,
	[MCTX_SLAB_ID].free_p = SlabFree,
	[MCTX_SLAB_ID].realloc = SlabRealloc,
	[MCTX_SLAB_ID].reset = SlabReset,
	[MCTX_SLAB_ID].delete_context = SlabDelete,
	[MCTX_SLAB_ID].get_chunk_context = SlabGetChunkContext,
	[MCTX_SLAB_ID].get_chunk_space = SlabGetChunkSpace,
	[MCTX_SLAB_ID].is_empty = SlabIsEmpty,
	[MCTX_SLAB_ID].stats = SlabStats,
#ifdef MEMORY_CONTEXT_CHECKING
	[MCTX_SLAB_ID].check = SlabCheck,
#endif

	/* alignedalloc.c */
	[MCTX_ALIGNED_REDIRECT_ID].alloc = NULL,	/* not required */
	[MCTX_ALIGNED_REDIRECT_ID].free_p = AlignedAllocFree,
	[MCTX_ALIGNED_REDIRECT_ID].realloc = AlignedAllocRealloc,
	[MCTX_ALIGNED_REDIRECT_ID].reset = NULL,	/* not required */
	[MCTX_ALIGNED_REDIRECT_ID].delete_context = NULL,	/* not required */
	[MCTX_ALIGNED_REDIRECT_ID].get_chunk_context = AlignedAllocGetChunkContext,
	[MCTX_ALIGNED_REDIRECT_ID].get_chunk_space = AlignedAllocGetChunkSpace,
	[MCTX_ALIGNED_REDIRECT_ID].is_empty = NULL, /* not required */
	[MCTX_ALIGNED_REDIRECT_ID].stats = NULL,	/* not required */
#ifdef MEMORY_CONTEXT_CHECKING
	[MCTX_ALIGNED_REDIRECT_ID].check = NULL,	/* not required */
#endif

	/* bump.c */
	[MCTX_BUMP_ID].alloc = BumpAlloc,
	[MCTX_BUMP_ID].free_p = BumpFree,
	[MCTX_BUMP_ID].realloc = BumpRealloc,
	[MCTX_BUMP_ID].reset = BumpReset,
	[MCTX_BUMP_ID].delete_context = BumpDelete,
	[MCTX_BUMP_ID].get_chunk_context = BumpGetChunkContext,
	[MCTX_BUMP_ID].get_chunk_space = BumpGetChunkSpace,
	[MCTX_BUMP_ID].is_empty = BumpIsEmpty,
	[MCTX_BUMP_ID].stats = BumpStats,
#ifdef MEMORY_CONTEXT_CHECKING
	[MCTX_BUMP_ID].check = BumpCheck,
#endif


	/*
	 * Reserved and unused IDs should have dummy entries here.  This allows us
	 * to fail cleanly if a bogus pointer is passed to pfree or the like.  It
	 * seems sufficient to provide routines for the methods that might get
	 * invoked from inspection of a chunk (see MCXT_METHOD calls below).
	 */
	BOGUS_MCTX(MCTX_1_RESERVED_GLIBC_ID),
	BOGUS_MCTX(MCTX_2_RESERVED_GLIBC_ID),
	BOGUS_MCTX(MCTX_8_UNUSED_ID),
	BOGUS_MCTX(MCTX_9_UNUSED_ID),
	BOGUS_MCTX(MCTX_10_UNUSED_ID),
	BOGUS_MCTX(MCTX_11_UNUSED_ID),
	BOGUS_MCTX(MCTX_12_UNUSED_ID),
	BOGUS_MCTX(MCTX_13_UNUSED_ID),
	BOGUS_MCTX(MCTX_14_UNUSED_ID),
	BOGUS_MCTX(MCTX_0_RESERVED_UNUSEDMEM_ID),
	BOGUS_MCTX(MCTX_15_RESERVED_WIPEDMEM_ID)
};

#undef BOGUS_MCTX

/*
 * CurrentMemoryContext
 *		Default memory context for allocations.
 */
MemoryContext CurrentMemoryContext = NULL;

/*
 * Standard top-level contexts. For a description of the purpose of each
 * of these contexts, refer to src/backend/utils/mmgr/README
 */
MemoryContext TopMemoryContext = NULL;
MemoryContext ErrorContext = NULL;
MemoryContext PostmasterContext = NULL;
MemoryContext CacheMemoryContext = NULL;
MemoryContext MessageContext = NULL;
MemoryContext TopTransactionContext = NULL;
MemoryContext CurTransactionContext = NULL;

/* This is a transient link to the active portal's memory context: */
MemoryContext PortalContext = NULL;

static void MemoryContextDeleteOnly(MemoryContext context);
static void MemoryContextCallResetCallbacks(MemoryContext context);
static void MemoryContextStatsInternal(MemoryContext context, int level,
									   int max_level, int max_children,
									   MemoryContextCounters *totals,
									   bool print_to_stderr);
static void MemoryContextStatsPrint(MemoryContext context, void *passthru,
									const char *stats_string,
									bool print_to_stderr);

/*
 * You should not do memory allocations within a critical section, because
 * an out-of-memory error will be escalated to a PANIC. To enforce that
 * rule, the allocation functions Assert that.
 */
#define AssertNotInCriticalSection(context) \
	Assert(CritSectionCount == 0 || (context)->allowInCritSection)

/*
 * Call the given function in the MemoryContextMethods for the memory context
 * type that 'pointer' belongs to.
 */
#define MCXT_METHOD(pointer, method) \
	mcxt_methods[GetMemoryChunkMethodID(pointer)].method

/*
 * GetMemoryChunkMethodID
 *		Return the MemoryContextMethodID from the uint64 chunk header which
 *		directly precedes 'pointer'.
 */
static inline MemoryContextMethodID
GetMemoryChunkMethodID(const void *pointer)
{
	uint64		header;

	/*
	 * Try to detect bogus pointers handed to us, poorly though we can.
	 * Presumably, a pointer that isn't MAXALIGNED isn't pointing at an
	 * allocated chunk.
	 */
	Assert(pointer == (const void *) MAXALIGN(pointer));

	/* Allow access to the uint64 header */
	VALGRIND_MAKE_MEM_DEFINED((char *) pointer - sizeof(uint64), sizeof(uint64));

	header = *((const uint64 *) ((const char *) pointer - sizeof(uint64)));

	/* Disallow access to the uint64 header */
	VALGRIND_MAKE_MEM_NOACCESS((char *) pointer - sizeof(uint64), sizeof(uint64));

	return (MemoryContextMethodID) (header & MEMORY_CONTEXT_METHODID_MASK);
}

/*
 * GetMemoryChunkHeader
 *		Return the uint64 chunk header which directly precedes 'pointer'.
 *
 * This is only used after GetMemoryChunkMethodID, so no need for error checks.
 */
static inline uint64
GetMemoryChunkHeader(const void *pointer)
{
	uint64		header;

	/* Allow access to the uint64 header */
	VALGRIND_MAKE_MEM_DEFINED((char *) pointer - sizeof(uint64), sizeof(uint64));

	header = *((const uint64 *) ((const char *) pointer - sizeof(uint64)));

	/* Disallow access to the uint64 header */
	VALGRIND_MAKE_MEM_NOACCESS((char *) pointer - sizeof(uint64), sizeof(uint64));

	return header;
}

/*
 * MemoryContextTraverseNext
 *		Helper function to traverse all descendants of a memory context
 *		without recursion.
 *
 * Recursion could lead to out-of-stack errors with deep context hierarchies,
 * which would be unpleasant in error cleanup code paths.
 *
 * To process 'context' and all its descendants, use a loop like this:
 *
 *     <process 'context'>
 *     for (MemoryContext curr = context->firstchild;
 *          curr != NULL;
 *          curr = MemoryContextTraverseNext(curr, context))
 *     {
 *         <process 'curr'>
 *     }
 *
 * This visits all the contexts in pre-order, that is a node is visited
 * before its children.
 */
static MemoryContext
MemoryContextTraverseNext(MemoryContext curr, MemoryContext top)
{
	/* After processing a node, traverse to its first child if any */
	if (curr->firstchild != NULL)
		return curr->firstchild;

	/*
	 * After processing a childless node, traverse to its next sibling if
	 * there is one.  If there isn't, traverse back up to the parent (which
	 * has already been visited, and now so have all its descendants).  We're
	 * done if that is "top", otherwise traverse to its next sibling if any,
	 * otherwise repeat moving up.
	 */
	while (curr->nextchild == NULL)
	{
		curr = curr->parent;
		if (curr == top)
			return NULL;
	}
	return curr->nextchild;
}

/*
 * Support routines to trap use of invalid memory context method IDs
 * (from calling pfree or the like on a bogus pointer).  As a possible
 * aid in debugging, we report the header word along with the pointer
 * address (if we got here, there must be an accessible header word).
 */
static void
BogusFree(void *pointer)
{
	elog(ERROR, "pfree called with invalid pointer %p (header 0x%016llx)",
		 pointer, (unsigned long long) GetMemoryChunkHeader(pointer));
}

static void *
BogusRealloc(void *pointer, Size size, int flags)
{
	elog(ERROR, "repalloc called with invalid pointer %p (header 0x%016llx)",
		 pointer, (unsigned long long) GetMemoryChunkHeader(pointer));
	return NULL;				/* keep compiler quiet */
}

static MemoryContext
BogusGetChunkContext(void *pointer)
{
	elog(ERROR, "GetMemoryChunkContext called with invalid pointer %p (header 0x%016llx)",
		 pointer, (unsigned long long) GetMemoryChunkHeader(pointer));
	return NULL;				/* keep compiler quiet */
}

static Size
BogusGetChunkSpace(void *pointer)
{
	elog(ERROR, "GetMemoryChunkSpace called with invalid pointer %p (header 0x%016llx)",
		 pointer, (unsigned long long) GetMemoryChunkHeader(pointer));
	return 0;					/* keep compiler quiet */
}


/*****************************************************************************
 *	  EXPORTED ROUTINES														 *
 *****************************************************************************/


/*
 * MemoryContextInit
 *		Start up the memory-context subsystem.
 *
 * This must be called before creating contexts or allocating memory in
 * contexts.  TopMemoryContext and ErrorContext are initialized here;
 * other contexts must be created afterwards.
 *
 * In normal multi-backend operation, this is called once during
 * postmaster startup, and not at all by individual backend startup
 * (since the backends inherit an already-initialized context subsystem
 * by virtue of being forked off the postmaster).  But in an EXEC_BACKEND
 * build, each process must do this for itself.
 *
 * In a standalone backend this must be called during backend startup.
 */
void
MemoryContextInit(void)
{
	Assert(TopMemoryContext == NULL);

	/*
	 * First, initialize TopMemoryContext, which is the parent of all others.
	 */
	TopMemoryContext = AllocSetContextCreate((MemoryContext) NULL,
											 "TopMemoryContext",
											 ALLOCSET_DEFAULT_SIZES);

	/*
	 * Not having any other place to point CurrentMemoryContext, make it point
	 * to TopMemoryContext.  Caller should change this soon!
	 */
	CurrentMemoryContext = TopMemoryContext;

	/*
	 * Initialize ErrorContext as an AllocSetContext with slow growth rate ---
	 * we don't really expect much to be allocated in it. More to the point,
	 * require it to contain at least 8K at all times. This is the only case
	 * where retained memory in a context is *essential* --- we want to be
	 * sure ErrorContext still has some memory even if we've run out
	 * elsewhere! Also, allow allocations in ErrorContext within a critical
	 * section. Otherwise a PANIC will cause an assertion failure in the error
	 * reporting code, before printing out the real cause of the failure.
	 *
	 * This should be the last step in this function, as elog.c assumes memory
	 * management works once ErrorContext is non-null.
	 */
	ErrorContext = AllocSetContextCreate(TopMemoryContext,
										 "ErrorContext",
										 8 * 1024,
										 8 * 1024,
										 8 * 1024);
	MemoryContextAllowInCriticalSection(ErrorContext, true);
}

/*
 * MemoryContextReset
 *		Release all space allocated within a context and delete all its
 *		descendant contexts (but not the named context itself).
 */
void
MemoryContextReset(MemoryContext context)
{
	Assert(MemoryContextIsValid(context));

	/* save a function call in common case where there are no children */
	if (context->firstchild != NULL)
		MemoryContextDeleteChildren(context);

	/* save a function call if no pallocs since startup or last reset */
	if (!context->isReset)
		MemoryContextResetOnly(context);
}

/*
 * MemoryContextResetOnly
 *		Release all space allocated within a context.
 *		Nothing is done to the context's descendant contexts.
 */
void
MemoryContextResetOnly(MemoryContext context)
{
	Assert(MemoryContextIsValid(context));

	/* Nothing to do if no pallocs since startup or last reset */
	if (!context->isReset)
	{
		MemoryContextCallResetCallbacks(context);

		/*
		 * If context->ident points into the context's memory, it will become
		 * a dangling pointer.  We could prevent that by setting it to NULL
		 * here, but that would break valid coding patterns that keep the
		 * ident elsewhere, e.g. in a parent context.  So for now we assume
		 * the programmer got it right.
		 */

		context->methods->reset(context);
		context->isReset = true;
		VALGRIND_DESTROY_MEMPOOL(context);
		VALGRIND_CREATE_MEMPOOL(context, 0, false);
	}
}

/*
 * MemoryContextResetChildren
 *		Release all space allocated within a context's descendants,
 *		but don't delete the contexts themselves.  The named context
 *		itself is not touched.
 */
void
MemoryContextResetChildren(MemoryContext context)
{
	Assert(MemoryContextIsValid(context));

	for (MemoryContext curr = context->firstchild;
		 curr != NULL;
		 curr = MemoryContextTraverseNext(curr, context))
	{
		MemoryContextResetOnly(curr);
	}
}

/*
 * MemoryContextDelete
 *		Delete a context and its descendants, and release all space
 *		allocated therein.
 *
 * The type-specific delete routine removes all storage for the context,
 * but we have to deal with descendant nodes here.
 */
void
MemoryContextDelete(MemoryContext context)
{
	MemoryContext curr;

	Assert(MemoryContextIsValid(context));

	/*
	 * Delete subcontexts from the bottom up.
	 *
	 * Note: Do not use recursion here.  A "stack depth limit exceeded" error
	 * would be unpleasant if we're already in the process of cleaning up from
	 * transaction abort.  We also cannot use MemoryContextTraverseNext() here
	 * because we modify the tree as we go.
	 */
	curr = context;
	for (;;)
	{
		MemoryContext parent;

		/* Descend down until we find a leaf context with no children */
		while (curr->firstchild != NULL)
			curr = curr->firstchild;

		/*
		 * We're now at a leaf with no children. Free it and continue from the
		 * parent.  Or if this was the original node, we're all done.
		 */
		parent = curr->parent;
		MemoryContextDeleteOnly(curr);

		if (curr == context)
			break;
		curr = parent;
	}
}

/*
 * Subroutine of MemoryContextDelete,
 * to delete a context that has no children.
 * We must also delink the context from its parent, if it has one.
 */
static void
MemoryContextDeleteOnly(MemoryContext context)
{
	Assert(MemoryContextIsValid(context));
	/* We had better not be deleting TopMemoryContext ... */
	Assert(context != TopMemoryContext);
	/* And not CurrentMemoryContext, either */
	Assert(context != CurrentMemoryContext);
	/* All the children should've been deleted already */
	Assert(context->firstchild == NULL);

	/*
	 * It's not entirely clear whether 'tis better to do this before or after
	 * delinking the context; but an error in a callback will likely result in
	 * leaking the whole context (if it's not a root context) if we do it
	 * after, so let's do it before.
	 */
	MemoryContextCallResetCallbacks(context);

	/*
	 * We delink the context from its parent before deleting it, so that if
	 * there's an error we won't have deleted/busted contexts still attached
	 * to the context tree.  Better a leak than a crash.
	 */
	MemoryContextSetParent(context, NULL);

	/*
	 * Also reset the context's ident pointer, in case it points into the
	 * context.  This would only matter if someone tries to get stats on the
	 * (already unlinked) context, which is unlikely, but let's be safe.
	 */
	context->ident = NULL;

	context->methods->delete_context(context);

	VALGRIND_DESTROY_MEMPOOL(context);
}

/*
 * MemoryContextDeleteChildren
 *		Delete all the descendants of the named context and release all
 *		space allocated therein.  The named context itself is not touched.
 */
void
MemoryContextDeleteChildren(MemoryContext context)
{
	Assert(MemoryContextIsValid(context));

	/*
	 * MemoryContextDelete will delink the child from me, so just iterate as
	 * long as there is a child.
	 */
	while (context->firstchild != NULL)
		MemoryContextDelete(context->firstchild);
}

/*
 * MemoryContextRegisterResetCallback
 *		Register a function to be called before next context reset/delete.
 *		Such callbacks will be called in reverse order of registration.
 *
 * The caller is responsible for allocating a MemoryContextCallback struct
 * to hold the info about this callback request, and for filling in the
 * "func" and "arg" fields in the struct to show what function to call with
 * what argument.  Typically the callback struct should be allocated within
 * the specified context, since that means it will automatically be freed
 * when no longer needed.
 *
 * There is no API for deregistering a callback once registered.  If you
 * want it to not do anything anymore, adjust the state pointed to by its
 * "arg" to indicate that.
 */
void
MemoryContextRegisterResetCallback(MemoryContext context,
								   MemoryContextCallback *cb)
{
	Assert(MemoryContextIsValid(context));

	/* Push onto head so this will be called before older registrants. */
	cb->next = context->reset_cbs;
	context->reset_cbs = cb;
	/* Mark the context as non-reset (it probably is already). */
	context->isReset = false;
}

/*
 * MemoryContextCallResetCallbacks
 *		Internal function to call all registered callbacks for context.
 */
static void
MemoryContextCallResetCallbacks(MemoryContext context)
{
	MemoryContextCallback *cb;

	/*
	 * We pop each callback from the list before calling.  That way, if an
	 * error occurs inside the callback, we won't try to call it a second time
	 * in the likely event that we reset or delete the context later.
	 */
	while ((cb = context->reset_cbs) != NULL)
	{
		context->reset_cbs = cb->next;
		cb->func(cb->arg);
	}
}

/*
 * MemoryContextSetIdentifier
 *		Set the identifier string for a memory context.
 *
 * An identifier can be provided to help distinguish among different contexts
 * of the same kind in memory context stats dumps.  The identifier string
 * must live at least as long as the context it is for; typically it is
 * allocated inside that context, so that it automatically goes away on
 * context deletion.  Pass id = NULL to forget any old identifier.
 */
void
MemoryContextSetIdentifier(MemoryContext context, const char *id)
{
	Assert(MemoryContextIsValid(context));
	context->ident = id;
}

/*
 * MemoryContextSetParent
 *		Change a context to belong to a new parent (or no parent).
 *
 * We provide this as an API function because it is sometimes useful to
 * change a context's lifespan after creation.  For example, a context
 * might be created underneath a transient context, filled with data,
 * and then reparented underneath CacheMemoryContext to make it long-lived.
 * In this way no special effort is needed to get rid of the context in case
 * a failure occurs before its contents are completely set up.
 *
 * Callers often assume that this function cannot fail, so don't put any
 * elog(ERROR) calls in it.
 *
 * A possible caller error is to reparent a context under itself, creating
 * a loop in the context graph.  We assert here that context != new_parent,
 * but checking for multi-level loops seems more trouble than it's worth.
 */
void
MemoryContextSetParent(MemoryContext context, MemoryContext new_parent)
{
	Assert(MemoryContextIsValid(context));
	Assert(context != new_parent);

	/* Fast path if it's got correct parent already */
	if (new_parent == context->parent)
		return;

	/* Delink from existing parent, if any */
	if (context->parent)
	{
		MemoryContext parent = context->parent;

		if (context->prevchild != NULL)
			context->prevchild->nextchild = context->nextchild;
		else
		{
			Assert(parent->firstchild == context);
			parent->firstchild = context->nextchild;
		}

		if (context->nextchild != NULL)
			context->nextchild->prevchild = context->prevchild;
	}

	/* And relink */
	if (new_parent)
	{
		Assert(MemoryContextIsValid(new_parent));
		context->parent = new_parent;
		context->prevchild = NULL;
		context->nextchild = new_parent->firstchild;
		if (new_parent->firstchild != NULL)
			new_parent->firstchild->prevchild = context;
		new_parent->firstchild = context;
	}
	else
	{
		context->parent = NULL;
		context->prevchild = NULL;
		context->nextchild = NULL;
	}
}

/*
 * MemoryContextAllowInCriticalSection
 *		Allow/disallow allocations in this memory context within a critical
 *		section.
 *
 * Normally, memory allocations are not allowed within a critical section,
 * because a failure would lead to PANIC.  There are a few exceptions to
 * that, like allocations related to debugging code that is not supposed to
 * be enabled in production.  This function can be used to exempt specific
 * memory contexts from the assertion in palloc().
 */
void
MemoryContextAllowInCriticalSection(MemoryContext context, bool allow)
{
	Assert(MemoryContextIsValid(context));

	context->allowInCritSection = allow;
}

/*
 * GetMemoryChunkContext
 *		Given a currently-allocated chunk, determine the MemoryContext that
 *		the chunk belongs to.
 */
MemoryContext
GetMemoryChunkContext(void *pointer)
{
	return MCXT_METHOD(pointer, get_chunk_context) (pointer);
}

/*
 * GetMemoryChunkSpace
 *		Given a currently-allocated chunk, determine the total space
 *		it occupies (including all memory-allocation overhead).
 *
 * This is useful for measuring the total space occupied by a set of
 * allocated chunks.
 */
Size
GetMemoryChunkSpace(void *pointer)
{
	return MCXT_METHOD(pointer, get_chunk_space) (pointer);
}

/*
 * MemoryContextGetParent
 *		Get the parent context (if any) of the specified context
 */
MemoryContext
MemoryContextGetParent(MemoryContext context)
{
	Assert(MemoryContextIsValid(context));

	return context->parent;
}

/*
 * MemoryContextIsEmpty
 *		Is a memory context empty of any allocated space?
 */
bool
MemoryContextIsEmpty(MemoryContext context)
{
	Assert(MemoryContextIsValid(context));

	/*
	 * For now, we consider a memory context nonempty if it has any children;
	 * perhaps this should be changed later.
	 */
	if (context->firstchild != NULL)
		return false;
	/* Otherwise use the type-specific inquiry */
	return context->methods->is_empty(context);
}

/*
 * Find the memory allocated to blocks for this memory context. If recurse is
 * true, also include children.
 */
Size
MemoryContextMemAllocated(MemoryContext context, bool recurse)
{
	Size		total = context->mem_allocated;

	Assert(MemoryContextIsValid(context));

	if (recurse)
	{
		for (MemoryContext curr = context->firstchild;
			 curr != NULL;
			 curr = MemoryContextTraverseNext(curr, context))
		{
			total += curr->mem_allocated;
		}
	}

	return total;
}

/*
 * Return the memory consumption statistics about the given context and its
 * children.
 */
void
MemoryContextMemConsumed(MemoryContext context,
						 MemoryContextCounters *consumed)
{
	Assert(MemoryContextIsValid(context));

	memset(consumed, 0, sizeof(*consumed));

	/* Examine the context itself */
	context->methods->stats(context, NULL, NULL, consumed, false);

	/* Examine children, using iteration not recursion */
	for (MemoryContext curr = context->firstchild;
		 curr != NULL;
		 curr = MemoryContextTraverseNext(curr, context))
	{
		curr->methods->stats(curr, NULL, NULL, consumed, false);
	}
}

/*
 * MemoryContextStats
 *		Print statistics about the named context and all its descendants.
 *
 * This is just a debugging utility, so it's not very fancy.  However, we do
 * make some effort to summarize when the output would otherwise be very long.
 * The statistics are sent to stderr.
 */
void
MemoryContextStats(MemoryContext context)
{
	/* Hard-wired limits are usually good enough */
	MemoryContextStatsDetail(context, 100, 100, true);
}

/*
 * MemoryContextStatsDetail
 *
 * Entry point for use if you want to vary the number of child contexts shown.
 *
 * If print_to_stderr is true, print statistics about the memory contexts
 * with fprintf(stderr), otherwise use ereport().
 */
void
MemoryContextStatsDetail(MemoryContext context,
						 int max_level, int max_children,
						 bool print_to_stderr)
{
	MemoryContextCounters grand_totals;

	memset(&grand_totals, 0, sizeof(grand_totals));

	MemoryContextStatsInternal(context, 0, max_level, max_children,
							   &grand_totals, print_to_stderr);

	if (print_to_stderr)
		fprintf(stderr,
				"Grand total: %zu bytes in %zu blocks; %zu free (%zu chunks); %zu used\n",
				grand_totals.totalspace, grand_totals.nblocks,
				grand_totals.freespace, grand_totals.freechunks,
				grand_totals.totalspace - grand_totals.freespace);
	else
	{
		/*
		 * Use LOG_SERVER_ONLY to prevent the memory contexts from being sent
		 * to the connected client.
		 *
		 * We don't buffer the information about all memory contexts in a
		 * backend into StringInfo and log it as one message.  That would
		 * require the buffer to be enlarged, risking an OOM as there could be
		 * a large number of memory contexts in a backend.  Instead, we log
		 * one message per memory context.
		 */
		ereport(LOG_SERVER_ONLY,
				(errhidestmt(true),
				 errhidecontext(true),
				 errmsg_internal("Grand total: %zu bytes in %zu blocks; %zu free (%zu chunks); %zu used",
								 grand_totals.totalspace, grand_totals.nblocks,
								 grand_totals.freespace, grand_totals.freechunks,
								 grand_totals.totalspace - grand_totals.freespace)));
	}
}

/*
 * MemoryContextStatsInternal
 *		One recursion level for MemoryContextStats
 *
 * Print stats for this context if possible, but in any case accumulate counts
 * into *totals (if not NULL).
 */
static void
MemoryContextStatsInternal(MemoryContext context, int level,
						   int max_level, int max_children,
						   MemoryContextCounters *totals,
						   bool print_to_stderr)
{
	MemoryContext child;
	int			ichild;

	Assert(MemoryContextIsValid(context));

	/* Examine the context itself */
	context->methods->stats(context,
							MemoryContextStatsPrint,
							(void *) &level,
							totals, print_to_stderr);

	/*
	 * Examine children.
	 *
	 * If we are past the recursion depth limit or already running low on
	 * stack, do not print them explicitly but just summarize them. Similarly,
	 * if there are more than max_children of them, we do not print the rest
	 * explicitly, but just summarize them.
	 */
	child = context->firstchild;
	ichild = 0;
	if (level < max_level && !stack_is_too_deep())
	{
		for (; child != NULL && ichild < max_children;
			 child = child->nextchild, ichild++)
		{
			MemoryContextStatsInternal(child, level + 1,
									   max_level, max_children,
									   totals,
									   print_to_stderr);
		}
	}

	if (child != NULL)
	{
		/* Summarize the rest of the children, avoiding recursion. */
		MemoryContextCounters local_totals;

		memset(&local_totals, 0, sizeof(local_totals));

		ichild = 0;
		while (child != NULL)
		{
			child->methods->stats(child, NULL, NULL, &local_totals, false);
			ichild++;
			child = MemoryContextTraverseNext(child, context);
		}

		if (print_to_stderr)
		{
			for (int i = 0; i <= level; i++)
				fprintf(stderr, "  ");
			fprintf(stderr,
					"%d more child contexts containing %zu total in %zu blocks; %zu free (%zu chunks); %zu used\n",
					ichild,
					local_totals.totalspace,
					local_totals.nblocks,
					local_totals.freespace,
					local_totals.freechunks,
					local_totals.totalspace - local_totals.freespace);
		}
		else
			ereport(LOG_SERVER_ONLY,
					(errhidestmt(true),
					 errhidecontext(true),
					 errmsg_internal("level: %d; %d more child contexts containing %zu total in %zu blocks; %zu free (%zu chunks); %zu used",
									 level,
									 ichild,
									 local_totals.totalspace,
									 local_totals.nblocks,
									 local_totals.freespace,
									 local_totals.freechunks,
									 local_totals.totalspace - local_totals.freespace)));

		if (totals)
		{
			totals->nblocks += local_totals.nblocks;
			totals->freechunks += local_totals.freechunks;
			totals->totalspace += local_totals.totalspace;
			totals->freespace += local_totals.freespace;
		}
	}
}

/*
 * MemoryContextStatsPrint
 *		Print callback used by MemoryContextStatsInternal
 *
 * For now, the passthru pointer just points to "int level"; later we might
 * make that more complicated.
 */
static void
MemoryContextStatsPrint(MemoryContext context, void *passthru,
						const char *stats_string,
						bool print_to_stderr)
{
	int			level = *(int *) passthru;
	const char *name = context->name;
	const char *ident = context->ident;
	char		truncated_ident[110];
	int			i;

	/*
	 * It seems preferable to label dynahash contexts with just the hash table
	 * name.  Those are already unique enough, so the "dynahash" part isn't
	 * very helpful, and this way is more consistent with pre-v11 practice.
	 */
	if (ident && strcmp(name, "dynahash") == 0)
	{
		name = ident;
		ident = NULL;
	}

	truncated_ident[0] = '\0';

	if (ident)
	{
		/*
		 * Some contexts may have very long identifiers (e.g., SQL queries).
		 * Arbitrarily truncate at 100 bytes, but be careful not to break
		 * multibyte characters.  Also, replace ASCII control characters, such
		 * as newlines, with spaces.
		 */
		int			idlen = strlen(ident);
		bool		truncated = false;

		strcpy(truncated_ident, ": ");
		i = strlen(truncated_ident);

		if (idlen > 100)
		{
			idlen = pg_mbcliplen(ident, idlen, 100);
			truncated = true;
		}

		while (idlen-- > 0)
		{
			unsigned char c = *ident++;

			if (c < ' ')
				c = ' ';
			truncated_ident[i++] = c;
		}
		truncated_ident[i] = '\0';

		if (truncated)
			strcat(truncated_ident, "...");
	}

	if (print_to_stderr)
	{
		for (i = 0; i < level; i++)
			fprintf(stderr, "  ");
		fprintf(stderr, "%s: %s%s\n", name, stats_string, truncated_ident);
	}
	else
		ereport(LOG_SERVER_ONLY,
				(errhidestmt(true),
				 errhidecontext(true),
				 errmsg_internal("level: %d; %s: %s%s",
								 level, name, stats_string, truncated_ident)));
}

/*
 * MemoryContextCheck
 *		Check all chunks in the named context and its children.
 *
 * This is just a debugging utility, so it's not fancy.
 */
#ifdef MEMORY_CONTEXT_CHECKING
void
MemoryContextCheck(MemoryContext context)
{
	Assert(MemoryContextIsValid(context));
	context->methods->check(context);

	for (MemoryContext curr = context->firstchild;
		 curr != NULL;
		 curr = MemoryContextTraverseNext(curr, context))
	{
		Assert(MemoryContextIsValid(curr));
		curr->methods->check(curr);
	}
}
#endif

/*
 * MemoryContextCreate
 *		Context-type-independent part of context creation.
 *
 * This is only intended to be called by context-type-specific
 * context creation routines, not by the unwashed masses.
 *
 * The memory context creation procedure goes like this:
 *	1.  Context-type-specific routine makes some initial space allocation,
 *		including enough space for the context header.  If it fails,
 *		it can ereport() with no damage done.
 *	2.	Context-type-specific routine sets up all type-specific fields of
 *		the header (those beyond MemoryContextData proper), as well as any
 *		other management fields it needs to have a fully valid context.
 *		Usually, failure in this step is impossible, but if it's possible
 *		the initial space allocation should be freed before ereport'ing.
 *	3.	Context-type-specific routine calls MemoryContextCreate() to fill in
 *		the generic header fields and link the context into the context tree.
 *	4.  We return to the context-type-specific routine, which finishes
 *		up type-specific initialization.  This routine can now do things
 *		that might fail (like allocate more memory), so long as it's
 *		sure the node is left in a state that delete will handle.
 *
 * node: the as-yet-uninitialized common part of the context header node.
 * tag: NodeTag code identifying the memory context type.
 * method_id: MemoryContextMethodID of the context-type being created.
 * parent: parent context, or NULL if this will be a top-level context.
 * name: name of context (must be statically allocated).
 *
 * Context routines generally assume that MemoryContextCreate can't fail,
 * so this can contain Assert but not elog/ereport.
 */
void
MemoryContextCreate(MemoryContext node,
					NodeTag tag,
					MemoryContextMethodID method_id,
					MemoryContext parent,
					const char *name)
{
	/* Creating new memory contexts is not allowed in a critical section */
	Assert(CritSectionCount == 0);

	/* Initialize all standard fields of memory context header */
	node->type = tag;
	node->isReset = true;
	node->methods = &mcxt_methods[method_id];
	node->parent = parent;
	node->firstchild = NULL;
	node->mem_allocated = 0;
	node->prevchild = NULL;
	node->name = name;
	node->ident = NULL;
	node->reset_cbs = NULL;

	/* OK to link node into context tree */
	if (parent)
	{
		node->nextchild = parent->firstchild;
		if (parent->firstchild != NULL)
			parent->firstchild->prevchild = node;
		parent->firstchild = node;
		/* inherit allowInCritSection flag from parent */
		node->allowInCritSection = parent->allowInCritSection;
	}
	else
	{
		node->nextchild = NULL;
		node->allowInCritSection = false;
	}

	VALGRIND_CREATE_MEMPOOL(node, 0, false);
}

/*
 * MemoryContextAllocationFailure
 *		For use by MemoryContextMethods implementations to handle when malloc
 *		returns NULL.  The behavior is specific to whether MCXT_ALLOC_NO_OOM
 *		is in 'flags'.
 */
void *
MemoryContextAllocationFailure(MemoryContext context, Size size, int flags)
{
	if ((flags & MCXT_ALLOC_NO_OOM) == 0)
	{
		if (TopMemoryContext)
			MemoryContextStats(TopMemoryContext);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed on request of size %zu in memory context \"%s\".",
						   size, context->name)));
	}
	return NULL;
}

/*
 * MemoryContextSizeFailure
 *		For use by MemoryContextMethods implementations to handle invalid
 *		memory allocation request sizes.
 */
void
MemoryContextSizeFailure(MemoryContext context, Size size, int flags)
{
	elog(ERROR, "invalid memory alloc request size %zu", size);
}

/*
 * MemoryContextAlloc
 *		Allocate space within the specified context.
 *
 * This could be turned into a macro, but we'd have to import
 * nodes/memnodes.h into postgres.h which seems a bad idea.
 */
void *
MemoryContextAlloc(MemoryContext context, Size size)
{
	void	   *ret;

	Assert(MemoryContextIsValid(context));
	AssertNotInCriticalSection(context);

	context->isReset = false;

	/*
	 * For efficiency reasons, we purposefully offload the handling of
	 * allocation failures to the MemoryContextMethods implementation as this
	 * allows these checks to be performed only when an actual malloc needs to
	 * be done to request more memory from the OS.  Additionally, not having
	 * to execute any instructions after this call allows the compiler to use
	 * the sibling call optimization.  If you're considering adding code after
	 * this call, consider making it the responsibility of the 'alloc'
	 * function instead.
	 */
	ret = context->methods->alloc(context, size, 0);

	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

	return ret;
}

/*
 * MemoryContextAllocZero
 *		Like MemoryContextAlloc, but clears allocated memory
 *
 *	We could just call MemoryContextAlloc then clear the memory, but this
 *	is a very common combination, so we provide the combined operation.
 */
void *
MemoryContextAllocZero(MemoryContext context, Size size)
{
	void	   *ret;

	Assert(MemoryContextIsValid(context));
	AssertNotInCriticalSection(context);

	context->isReset = false;

	ret = context->methods->alloc(context, size, 0);

	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

	MemSetAligned(ret, 0, size);

	return ret;
}

/*
 * MemoryContextAllocExtended
 *		Allocate space within the specified context using the given flags.
 */
void *
MemoryContextAllocExtended(MemoryContext context, Size size, int flags)
{
	void	   *ret;

	Assert(MemoryContextIsValid(context));
	AssertNotInCriticalSection(context);

	if (!((flags & MCXT_ALLOC_HUGE) != 0 ? AllocHugeSizeIsValid(size) :
		  AllocSizeIsValid(size)))
		elog(ERROR, "invalid memory alloc request size %zu", size);

	context->isReset = false;

	ret = context->methods->alloc(context, size, flags);
	if (unlikely(ret == NULL))
		return NULL;

	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

	if ((flags & MCXT_ALLOC_ZERO) != 0)
		MemSetAligned(ret, 0, size);

	return ret;
}

/*
 * HandleLogMemoryContextInterrupt
 *		Handle receipt of an interrupt indicating logging of memory
 *		contexts.
 *
 * All the actual work is deferred to ProcessLogMemoryContextInterrupt(),
 * because we cannot safely emit a log message inside the signal handler.
 */
void
HandleLogMemoryContextInterrupt(void)
{
	InterruptPending = true;
	LogMemoryContextPending = true;
	/* latch will be set by procsignal_sigusr1_handler */
}

/*
 * ProcessLogMemoryContextInterrupt
 * 		Perform logging of memory contexts of this backend process.
 *
 * Any backend that participates in ProcSignal signaling must arrange
 * to call this function if we see LogMemoryContextPending set.
 * It is called from CHECK_FOR_INTERRUPTS(), which is enough because
 * the target process for logging of memory contexts is a backend.
 */
void
ProcessLogMemoryContextInterrupt(void)
{
	LogMemoryContextPending = false;

	/*
	 * Use LOG_SERVER_ONLY to prevent this message from being sent to the
	 * connected client.
	 */
	ereport(LOG_SERVER_ONLY,
			(errhidestmt(true),
			 errhidecontext(true),
			 errmsg("logging memory contexts of PID %d", MyProcPid)));

	/*
	 * When a backend process is consuming huge memory, logging all its memory
	 * contexts might overrun available disk space. To prevent this, we limit
	 * the depth of the hierarchy, as well as the number of child contexts to
	 * log per parent to 100.
	 *
	 * As with MemoryContextStats(), we suppose that practical cases where the
	 * dump gets long will typically be huge numbers of siblings under the
	 * same parent context; while the additional debugging value from seeing
	 * details about individual siblings beyond 100 will not be large.
	 */
	MemoryContextStatsDetail(TopMemoryContext, 100, 100, false);
}

void *
palloc(Size size)
{
	/* duplicates MemoryContextAlloc to avoid increased overhead */
	void	   *ret;
	MemoryContext context = CurrentMemoryContext;

	Assert(MemoryContextIsValid(context));
	AssertNotInCriticalSection(context);

	context->isReset = false;

	/*
	 * For efficiency reasons, we purposefully offload the handling of
	 * allocation failures to the MemoryContextMethods implementation as this
	 * allows these checks to be performed only when an actual malloc needs to
	 * be done to request more memory from the OS.  Additionally, not having
	 * to execute any instructions after this call allows the compiler to use
	 * the sibling call optimization.  If you're considering adding code after
	 * this call, consider making it the responsibility of the 'alloc'
	 * function instead.
	 */
	ret = context->methods->alloc(context, size, 0);
	/* We expect OOM to be handled by the alloc function */
	Assert(ret != NULL);
	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

	return ret;
}

void *
palloc0(Size size)
{
	/* duplicates MemoryContextAllocZero to avoid increased overhead */
	void	   *ret;
	MemoryContext context = CurrentMemoryContext;

	Assert(MemoryContextIsValid(context));
	AssertNotInCriticalSection(context);

	context->isReset = false;

	ret = context->methods->alloc(context, size, 0);

	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

	MemSetAligned(ret, 0, size);

	return ret;
}

void *
palloc_extended(Size size, int flags)
{
	/* duplicates MemoryContextAllocExtended to avoid increased overhead */
	void	   *ret;
	MemoryContext context = CurrentMemoryContext;

	Assert(MemoryContextIsValid(context));
	AssertNotInCriticalSection(context);

	context->isReset = false;

	ret = context->methods->alloc(context, size, flags);
	if (unlikely(ret == NULL))
	{
		return NULL;
	}

	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

	if ((flags & MCXT_ALLOC_ZERO) != 0)
		MemSetAligned(ret, 0, size);

	return ret;
}

/*
 * MemoryContextAllocAligned
 *		Allocate 'size' bytes of memory in 'context' aligned to 'alignto'
 *		bytes.
 *
 * Currently, we align addresses by requesting additional bytes from the
 * MemoryContext's standard allocator function and then aligning the returned
 * address by the required alignment.  This means that the given MemoryContext
 * must support providing us with a chunk of memory that's larger than 'size'.
 * For allocators such as Slab, that's not going to work, as slab only allows
 * chunks of the size that's specified when the context is created.
 *
 * 'alignto' must be a power of 2.
 * 'flags' may be 0 or set the same as MemoryContextAllocExtended().
 */
void *
MemoryContextAllocAligned(MemoryContext context,
						  Size size, Size alignto, int flags)
{
	MemoryChunk *alignedchunk;
	Size		alloc_size;
	void	   *unaligned;
	void	   *aligned;

	/* wouldn't make much sense to waste that much space */
	Assert(alignto < (128 * 1024 * 1024));

	/* ensure alignto is a power of 2 */
	Assert((alignto & (alignto - 1)) == 0);

	/*
	 * If the alignment requirements are less than what we already guarantee
	 * then just use the standard allocation function.
	 */
	if (unlikely(alignto <= MAXIMUM_ALIGNOF))
		return MemoryContextAllocExtended(context, size, flags);

	/*
	 * We implement aligned pointers by simply allocating enough memory for
	 * the requested size plus the alignment and an additional "redirection"
	 * MemoryChunk.  This additional MemoryChunk is required for operations
	 * such as pfree when used on the pointer returned by this function.  We
	 * use this redirection MemoryChunk in order to find the pointer to the
	 * memory that was returned by the MemoryContextAllocExtended call below.
	 * We do that by "borrowing" the block offset field and instead of using
	 * that to find the offset into the owning block, we use it to find the
	 * original allocated address.
	 *
	 * Here we must allocate enough extra memory so that we can still align
	 * the pointer returned by MemoryContextAllocExtended and also have enough
	 * space for the redirection MemoryChunk.  Since allocations will already
	 * be at least aligned by MAXIMUM_ALIGNOF, we can subtract that amount
	 * from the allocation size to save a little memory.
	 */
	alloc_size = size + PallocAlignedExtraBytes(alignto);

#ifdef MEMORY_CONTEXT_CHECKING
	/* ensure there's space for a sentinel byte */
	alloc_size += 1;
#endif

	/* perform the actual allocation */
	unaligned = MemoryContextAllocExtended(context, alloc_size, flags);

	/* set the aligned pointer */
	aligned = (void *) TYPEALIGN(alignto, (char *) unaligned +
								 sizeof(MemoryChunk));

	alignedchunk = PointerGetMemoryChunk(aligned);

	/*
	 * We set the redirect MemoryChunk so that the block offset calculation is
	 * used to point back to the 'unaligned' allocated chunk.  This allows us
	 * to use MemoryChunkGetBlock() to find the unaligned chunk when we need
	 * to perform operations such as pfree() and repalloc().
	 *
	 * We store 'alignto' in the MemoryChunk's 'value' so that we know what
	 * the alignment was set to should we ever be asked to realloc this
	 * pointer.
	 */
	MemoryChunkSetHdrMask(alignedchunk, unaligned, alignto,
						  MCTX_ALIGNED_REDIRECT_ID);

	/* double check we produced a correctly aligned pointer */
	Assert((void *) TYPEALIGN(alignto, aligned) == aligned);

#ifdef MEMORY_CONTEXT_CHECKING
	alignedchunk->requested_size = size;
	/* set mark to catch clobber of "unused" space */
	set_sentinel(aligned, size);
#endif

	/* Mark the bytes before the redirection header as noaccess */
	VALGRIND_MAKE_MEM_NOACCESS(unaligned,
							   (char *) alignedchunk - (char *) unaligned);

	/* Disallow access to the redirection chunk header. */
	VALGRIND_MAKE_MEM_NOACCESS(alignedchunk, sizeof(MemoryChunk));

	return aligned;
}

/*
 * palloc_aligned
 *		Allocate 'size' bytes returning a pointer that's aligned to the
 *		'alignto' boundary.
 *
 * Currently, we align addresses by requesting additional bytes from the
 * MemoryContext's standard allocator function and then aligning the returned
 * address by the required alignment.  This means that the given MemoryContext
 * must support providing us with a chunk of memory that's larger than 'size'.
 * For allocators such as Slab, that's not going to work, as slab only allows
 * chunks of the size that's specified when the context is created.
 *
 * 'alignto' must be a power of 2.
 * 'flags' may be 0 or set the same as MemoryContextAllocExtended().
 */
void *
palloc_aligned(Size size, Size alignto, int flags)
{
	return MemoryContextAllocAligned(CurrentMemoryContext, size, alignto, flags);
}

/*
 * pfree
 *		Release an allocated chunk.
 */
void
pfree(void *pointer)
{
#ifdef USE_VALGRIND
	MemoryContextMethodID method = GetMemoryChunkMethodID(pointer);
	MemoryContext context = GetMemoryChunkContext(pointer);
#endif

	MCXT_METHOD(pointer, free_p) (pointer);

#ifdef USE_VALGRIND
	if (method != MCTX_ALIGNED_REDIRECT_ID)
		VALGRIND_MEMPOOL_FREE(context, pointer);
#endif
}

/*
 * repalloc
 *		Adjust the size of a previously allocated chunk.
 */
void *
repalloc(void *pointer, Size size)
{
#ifdef USE_VALGRIND
	MemoryContextMethodID method = GetMemoryChunkMethodID(pointer);
#endif
#if defined(USE_ASSERT_CHECKING) || defined(USE_VALGRIND)
	MemoryContext context = GetMemoryChunkContext(pointer);
#endif
	void	   *ret;

	AssertNotInCriticalSection(context);

	/* isReset must be false already */
	Assert(!context->isReset);

	/*
	 * For efficiency reasons, we purposefully offload the handling of
	 * allocation failures to the MemoryContextMethods implementation as this
	 * allows these checks to be performed only when an actual malloc needs to
	 * be done to request more memory from the OS.  Additionally, not having
	 * to execute any instructions after this call allows the compiler to use
	 * the sibling call optimization.  If you're considering adding code after
	 * this call, consider making it the responsibility of the 'realloc'
	 * function instead.
	 */
	ret = MCXT_METHOD(pointer, realloc) (pointer, size, 0);

#ifdef USE_VALGRIND
	if (method != MCTX_ALIGNED_REDIRECT_ID)
		VALGRIND_MEMPOOL_CHANGE(context, pointer, ret, size);
#endif

	return ret;
}

/*
 * repalloc_extended
 *		Adjust the size of a previously allocated chunk,
 *		with HUGE and NO_OOM options.
 */
void *
repalloc_extended(void *pointer, Size size, int flags)
{
#if defined(USE_ASSERT_CHECKING) || defined(USE_VALGRIND)
	MemoryContext context = GetMemoryChunkContext(pointer);
#endif
	void	   *ret;

	AssertNotInCriticalSection(context);

	/* isReset must be false already */
	Assert(!context->isReset);

	/*
	 * For efficiency reasons, we purposefully offload the handling of
	 * allocation failures to the MemoryContextMethods implementation as this
	 * allows these checks to be performed only when an actual malloc needs to
	 * be done to request more memory from the OS.  Additionally, not having
	 * to execute any instructions after this call allows the compiler to use
	 * the sibling call optimization.  If you're considering adding code after
	 * this call, consider making it the responsibility of the 'realloc'
	 * function instead.
	 */
	ret = MCXT_METHOD(pointer, realloc) (pointer, size, flags);
	if (unlikely(ret == NULL))
		return NULL;

	VALGRIND_MEMPOOL_CHANGE(context, pointer, ret, size);

	return ret;
}

/*
 * repalloc0
 *		Adjust the size of a previously allocated chunk and zero out the added
 *		space.
 */
void *
repalloc0(void *pointer, Size oldsize, Size size)
{
	void	   *ret;

	/* catch wrong argument order */
	if (unlikely(oldsize > size))
		elog(ERROR, "invalid repalloc0 call: oldsize %zu, new size %zu",
			 oldsize, size);

	ret = repalloc(pointer, size);
	memset((char *) ret + oldsize, 0, (size - oldsize));
	return ret;
}

/*
 * MemoryContextAllocHuge
 *		Allocate (possibly-expansive) space within the specified context.
 *
 * See considerations in comment at MaxAllocHugeSize.
 */
void *
MemoryContextAllocHuge(MemoryContext context, Size size)
{
	void	   *ret;

	Assert(MemoryContextIsValid(context));
	AssertNotInCriticalSection(context);

	context->isReset = false;

	/*
	 * For efficiency reasons, we purposefully offload the handling of
	 * allocation failures to the MemoryContextMethods implementation as this
	 * allows these checks to be performed only when an actual malloc needs to
	 * be done to request more memory from the OS.  Additionally, not having
	 * to execute any instructions after this call allows the compiler to use
	 * the sibling call optimization.  If you're considering adding code after
	 * this call, consider making it the responsibility of the 'alloc'
	 * function instead.
	 */
	ret = context->methods->alloc(context, size, MCXT_ALLOC_HUGE);

	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

	return ret;
}

/*
 * repalloc_huge
 *		Adjust the size of a previously allocated chunk, permitting a large
 *		value.  The previous allocation need not have been "huge".
 */
void *
repalloc_huge(void *pointer, Size size)
{
	/* this one seems not worth its own implementation */
	return repalloc_extended(pointer, size, MCXT_ALLOC_HUGE);
}

/*
 * MemoryContextStrdup
 *		Like strdup(), but allocate from the specified context
 */
char *
MemoryContextStrdup(MemoryContext context, const char *string)
{
	char	   *nstr;
	Size		len = strlen(string) + 1;

	nstr = (char *) MemoryContextAlloc(context, len);

	memcpy(nstr, string, len);

	return nstr;
}

char *
pstrdup(const char *in)
{
	return MemoryContextStrdup(CurrentMemoryContext, in);
}

/*
 * pnstrdup
 *		Like pstrdup(), but append null byte to a
 *		not-necessarily-null-terminated input string.
 */
char *
pnstrdup(const char *in, Size len)
{
	char	   *out;

	len = strnlen(in, len);

	out = palloc(len + 1);
	memcpy(out, in, len);
	out[len] = '\0';

	return out;
}

/*
 * Make copy of string with all trailing newline characters removed.
 */
char *
pchomp(const char *in)
{
	size_t		n;

	n = strlen(in);
	while (n > 0 && in[n - 1] == '\n')
		n--;
	return pnstrdup(in, n);
}
