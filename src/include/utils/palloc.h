/*-------------------------------------------------------------------------
 *
 * palloc.h
 *	  POSTGRES memory allocator definitions.
 *
 * This file contains the basic memory allocation interface that is
 * needed by almost every backend module.  It is included directly by
 * postgres.h, so the definitions here are automatically available
 * everywhere.  Keep it lean!
 *
 * Memory allocation occurs within "contexts".  Every chunk obtained from
 * palloc()/MemoryContextAlloc() is allocated within a specific context.
 * The entire contents of a context can be freed easily and quickly by
 * resetting or deleting the context --- this is both faster and less
 * prone to memory-leakage bugs than releasing chunks individually.
 * We organize contexts into context trees to allow fine-grain control
 * over chunk lifetime while preserving the certainty that we will free
 * everything that should be freed.  See utils/mmgr/README for more info.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: palloc.h,v 1.14 2001/01/24 19:43:28 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PALLOC_H
#define PALLOC_H

/*
 * Type MemoryContextData is declared in nodes/memnodes.h.  Most users
 * of memory allocation should just treat it as an abstract type, so we
 * do not provide the struct contents here.
 */
typedef struct MemoryContextData *MemoryContext;

/*
 * CurrentMemoryContext is the default allocation context for palloc().
 * We declare it here so that palloc() can be a macro.  Avoid accessing it
 * directly!  Instead, use MemoryContextSwitchTo() to change the setting.
 */
extern DLLIMPORT MemoryContext CurrentMemoryContext;

/*
 * Fundamental memory-allocation operations (more are in utils/memutils.h)
 */
extern void *MemoryContextAlloc(MemoryContext context, Size size);

#define palloc(sz)  MemoryContextAlloc(CurrentMemoryContext, (sz))

extern void pfree(void *pointer);

extern void *repalloc(void *pointer, Size size);

extern MemoryContext MemoryContextSwitchTo(MemoryContext context);

/*
 * These are like standard strdup() except the copied string is
 * allocated in a context, not with malloc().
 */
extern char *MemoryContextStrdup(MemoryContext context, const char *string);

#define pstrdup(str)  MemoryContextStrdup(CurrentMemoryContext, (str))


/* ----------------
 * Alignment macros: align a length or address appropriately for a given type.
 *
 * There used to be some incredibly crufty platform-dependent hackery here,
 * but now we rely on the configure script to get the info for us. Much nicer.
 *
 * NOTE: TYPEALIGN will not work if ALIGNVAL is not a power of 2.
 * That case seems extremely unlikely to occur in practice, however.
 * ----------------
 */

#define TYPEALIGN(ALIGNVAL,LEN) (((long)(LEN) + (ALIGNVAL-1)) & ~(ALIGNVAL-1))

#define SHORTALIGN(LEN)			TYPEALIGN(ALIGNOF_SHORT, (LEN))
#define INTALIGN(LEN)			TYPEALIGN(ALIGNOF_INT, (LEN))
#define LONGALIGN(LEN)			TYPEALIGN(ALIGNOF_LONG, (LEN))
#define DOUBLEALIGN(LEN)		TYPEALIGN(ALIGNOF_DOUBLE, (LEN))
#define MAXALIGN(LEN)			TYPEALIGN(MAXIMUM_ALIGNOF, (LEN))


#endif	 /* PALLOC_H */
