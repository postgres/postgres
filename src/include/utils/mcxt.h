/*-------------------------------------------------------------------------
 *
 * mcxt.h--
 *    POSTGRES memory context definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: mcxt.h,v 1.2 1996/10/31 09:51:24 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	MCXT_H
#define MCXT_H


#include "nodes/memnodes.h"
#include "nodes/nodes.h"

extern MemoryContext	CurrentMemoryContext;
extern MemoryContext	TopMemoryContext;


/*
 * MaxAllocSize --
 *	Arbitrary limit on size of allocations.
 *
 * Note:
 *	There is no guarantee that allocations smaller than MaxAllocSize
 *	will succeed.  Allocation requests larger than MaxAllocSize will
 *	be summarily denied.
 *
 *	This value should not be referenced except in one place in the code.
 *
 * XXX This should be defined in a file of tunable constants.
 */
#define MaxAllocSize	(0xfffffff)	/* 16G - 1 */

/*
 * prototypes for functions in mcxt.c
 */
extern void EnableMemoryContext(bool on);
extern Pointer MemoryContextAlloc(MemoryContext context, Size size);
extern Pointer MemoryContextRealloc(MemoryContext context,
				    Pointer pointer,
				    Size size);
extern void MemoryContextFree(MemoryContext context, Pointer pointer);
extern char *MemoryContextGetName(MemoryContext context);
extern Size PointerGetAllocSize(Pointer pointer);
extern MemoryContext MemoryContextSwitchTo(MemoryContext context);
extern GlobalMemory CreateGlobalMemory(char *name);
extern void GlobalMemoryDestroy(GlobalMemory context);


#endif	/* MCXT_H */
