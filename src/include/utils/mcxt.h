/*-------------------------------------------------------------------------
 *
 * mcxt.h
 *	  POSTGRES memory context definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: mcxt.h,v 1.13 1999/03/07 23:03:31 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef MCXT_H
#define MCXT_H

/* These types are declared in nodes/memnodes.h, but most users of memory
 * allocation should just treat them as abstract types, so we do not provide
 * the struct contents here.
 */

typedef struct MemoryContextData *MemoryContext;
typedef struct GlobalMemoryData *GlobalMemory;


extern MemoryContext CurrentMemoryContext;
extern MemoryContext TopMemoryContext;


/*
 * MaxAllocSize 
 *		Arbitrary limit on size of allocations.
 *
 * Note:
 *		There is no guarantee that allocations smaller than MaxAllocSize
 *		will succeed.  Allocation requests larger than MaxAllocSize will
 *		be summarily denied.
 *
 *		This value should not be referenced except in one place in the code.
 *
 * XXX This should be defined in a file of tunable constants.
 */
#define MaxAllocSize	(0xfffffff)		/* 16G - 1 */

/*
 * prototypes for functions in mcxt.c
 */
extern void EnableMemoryContext(bool on);
extern Pointer MemoryContextAlloc(MemoryContext context, Size size);
extern Pointer MemoryContextRealloc(MemoryContext context,
					 Pointer pointer,
					 Size size);
extern void MemoryContextFree(MemoryContext context, Pointer pointer);
extern MemoryContext MemoryContextSwitchTo(MemoryContext context);
extern GlobalMemory CreateGlobalMemory(char *name);
extern void GlobalMemoryDestroy(GlobalMemory context);


#endif	 /* MCXT_H */
