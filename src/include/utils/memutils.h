/*-------------------------------------------------------------------------
 *
 * memutils.h
 *	  this file contains general memory alignment, allocation
 *	  and manipulation stuff that used to be spread out
 *	  between the following files:
 *
 *		align.h							alignment macros
 *		aset.h							memory allocation set stuff
 *		oset.h							  (used by aset.h)
 *		(bit.h							bit array type / extern)
 *		clib.h							mem routines
 *		limit.h							max bits/byte, etc.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: memutils.h,v 1.22 1999/03/25 03:49:34 tgl Exp $
 *
 * NOTES
 *	  some of the information in this file will be moved to
 *	  other files, (like MaxHeapTupleSize and MaxAttributeSize).
 *
 *-------------------------------------------------------------------------
 */
#ifndef MEMUTILS_H
#define MEMUTILS_H


/* ----------------
 * Alignment macros: align a length or address appropriately for a given type.
 *
 * It'd be best to use offsetof to check how the compiler aligns stuff,
 * but not all compilers support that (still true)?  So we make the
 * conservative assumption that a type must be aligned on a boundary equal
 * to its own size, except on a few architectures where we know better.
 *
 * CAUTION: for the system tables, the struct declarations found in
 * src/include/pg_*.h had better be interpreted by the compiler in a way
 * that agrees with the workings of these macros.  In practice that means
 * being careful to lay out the columns of a system table in a way that avoids
 * wasted pad space.
 *
 * CAUTION: _ALIGN will not work if sizeof(TYPE) is not a power of 2.
 * There are machines where sizeof(double) is not, for example.
 * But such a size is almost certainly not an alignment boundary anyway.
 * ----------------
 */

#define _ALIGN(TYPE,LEN) \
		(((long)(LEN) + (sizeof(TYPE) - 1)) & ~(sizeof(TYPE) - 1))

#define SHORTALIGN(LEN)			_ALIGN(short, (LEN))

#if defined(m68k)
#define INTALIGN(LEN)			_ALIGN(short, (LEN))
#else
#define INTALIGN(LEN)			_ALIGN(int, (LEN))
#endif

#if (defined(sun) && ! defined(sparc)) || defined(m68k)
#define LONGALIGN(LEN)			_ALIGN(short, (LEN))
#else
#define LONGALIGN(LEN)			_ALIGN(long, (LEN))
#endif

#if defined(m68k)
#define DOUBLEALIGN(LEN)		_ALIGN(short, (LEN))
#define MAXALIGN(LEN)			_ALIGN(short, (LEN))
#elif defined(sco)
#define DOUBLEALIGN(LEN)		_ALIGN(int, (LEN))
#define MAXALIGN(LEN)			_ALIGN(int, (LEN))
#else
#define DOUBLEALIGN(LEN)		_ALIGN(double, (LEN))
#define MAXALIGN(LEN)			_ALIGN(double, (LEN))
#endif

/*****************************************************************************
 *	  oset.h --			Fixed format ordered set definitions.				 *
 *****************************************************************************/
/* Note:
 *		Fixed format ordered sets are <EXPLAIN>.
 *		XXX This is a preliminary version.	Work is needed to explain
 *		XXX semantics of the external definitions.	Otherwise, the
 *		XXX functional interface should not change.
 *
 */

typedef struct OrderedElemData OrderedElemData;
typedef OrderedElemData *OrderedElem;

typedef struct OrderedSetData OrderedSetData;
typedef OrderedSetData *OrderedSet;

struct OrderedElemData
{
	OrderedElem next;			/* Next elem or &this->set->dummy		*/
	OrderedElem prev;			/* Previous elem or &this->set->head	*/
	OrderedSet	set;			/* Parent set							*/
};

struct OrderedSetData
{
	OrderedElem head;			/* First elem or &this->dummy			*/
	OrderedElem dummy;			/* (hack) Terminator == NULL			*/
	OrderedElem tail;			/* Last elem or &this->head				*/
	Offset		offset;			/* Offset from struct base to elem		*/
	/* this could be signed short int! */
};

extern void OrderedSetInit(OrderedSet set, Offset offset);
extern bool OrderedSetContains(OrderedSet set, OrderedElem elem);
extern Pointer OrderedSetGetHead(OrderedSet set);
extern Pointer OrderedElemGetPredecessor(OrderedElem elem);
extern Pointer OrderedElemGetSuccessor(OrderedElem elem);
extern void OrderedElemPop(OrderedElem elem);
extern void OrderedElemPushInto(OrderedElem elem, OrderedSet Set);

/*****************************************************************************
 *	  aset.h --			Allocation set definitions.							 *
 *****************************************************************************/
/*
 * Description:
 *		An allocation set is a set containing allocated elements.  When
 *		an allocation is requested for a set, memory is allocated and a
 *		pointer is returned.  Subsequently, this memory may be freed or
 *		reallocated.  In addition, an allocation set may be reset which
 *		will cause all allocated memory to be freed.
 *
 *		Allocations may occur in four different modes.	The mode of
 *		allocation does not affect the behavior of allocations except in
 *		terms of performance.  The allocation mode is set at the time of
 *		set initialization.  Once the mode is chosen, it cannot be changed
 *		unless the set is reinitialized.
 *
 *		"Dynamic" mode forces all allocations to occur in a heap.  This
 *		is a good mode to use when small memory segments are allocated
 *		and freed very frequently.	This is a good choice when allocation
 *		characteristics are unknown.  This is the default mode.
 *
 *		"Static" mode attemts to allocate space as efficiently as possible
 *		without regard to freeing memory.  This mode should be chosen only
 *		when it is known that many allocations will occur but that very
 *		little of the allocated memory will be explicitly freed.
 *
 *		"Tunable" mode is a hybrid of dynamic and static modes.  The
 *		tunable mode will use static mode allocation except when the
 *		allocation request exceeds a size limit supplied at the time of set
 *		initialization.  "Big" objects are allocated using dynamic mode.
 *
 *		"Bounded" mode attempts to allocate space efficiently given a limit
 *		on space consumed by the allocation set.  This restriction can be
 *		considered a "soft" restriction, because memory segments will
 *		continue to be returned after the limit is exceeded.  The limit is
 *		specified at the time of set initialization like for tunable mode.
 *
 * Note:
 *		Allocation sets are not automatically reset on a system reset.
 *		Higher level code is responsible for cleaning up.
 *
 *		There may other modes in the future.
 */

/*
 * AllocPointer 
 *		Aligned pointer which may be a member of an allocation set.
 */
typedef Pointer AllocPointer;

/*
 * AllocMode 
 *		Mode of allocation for an allocation set.
 *
 * Note:
 *		See above for a description of the various nodes.
 */
typedef enum AllocMode
{
	DynamicAllocMode,			/* always dynamically allocate */
	StaticAllocMode,			/* always "statically" allocate */
	TunableAllocMode,			/* allocations are "tuned" */
	BoundedAllocMode			/* allocations bounded to fixed usage */
} AllocMode;

#define DefaultAllocMode		DynamicAllocMode

/*
 * AllocBlock 
 *		Small pieces of memory are taken from bigger blocks of
 *		memory with a size aligned to a power of two. These
 *		pieces are not free's separately, instead they are reused
 *		for the next allocation of a fitting size.
 */
typedef struct AllocBlockData {
	struct AllocSetData			*aset;
	struct AllocBlockData		*next;
	char						*freeptr;
	char						*endptr;
} AllocBlockData;

typedef AllocBlockData *AllocBlock;

/*
 * AllocChunk 
 *		The prefix of each piece of memory in an AllocBlock
 */
typedef struct AllocChunkData {
	void						*aset;
	Size						size;
} AllocChunkData;

typedef AllocChunkData *AllocChunk;

/*
 * AllocSet 
 *		Allocation set.
 */
typedef struct AllocSetData
{
	struct AllocBlockData		*blocks;
	struct AllocChunkData		*freelist[8];
	/* Note: this will change in the future to support other modes */
} AllocSetData;

typedef AllocSetData *AllocSet;

/*
 * AllocPointerIsValid 
 *		True iff pointer is valid allocation pointer.
 */
#define AllocPointerIsValid(pointer) PointerIsValid(pointer)

/*
 * AllocSetIsValid 
 *		True iff set is valid allocation set.
 */
#define AllocSetIsValid(set) PointerIsValid(set)

extern void AllocSetInit(AllocSet set, AllocMode mode, Size limit);

extern void AllocSetReset(AllocSet set);

extern bool AllocSetContains(AllocSet set, AllocPointer pointer);
extern AllocPointer AllocSetAlloc(AllocSet set, Size size);
extern void AllocSetFree(AllocSet set, AllocPointer pointer);
extern AllocPointer AllocSetRealloc(AllocSet set, AllocPointer pointer,
				Size size);

extern void AllocSetDump(AllocSet set);

/*****************************************************************************
 *	  clib.h --			Standard C library definitions						 *
 *****************************************************************************/
/*
 * Note:
 *		This file is OPERATING SYSTEM dependent!!!
 *
 */

/*
 *		LibCCopyLength is only used within this file. -cim 6/12/90
 *
 */
typedef int LibCCopyLength;

/*
 * MemoryCopy 
 *		Copies fixed length block of memory to another.
 */
#define MemoryCopy(toBuffer, fromBuffer, length)\
	memcpy(toBuffer, fromBuffer, length)

/*****************************************************************************
 *	  limit.h --		POSTGRES limit definitions.							 *
 *****************************************************************************/

#define MaxBitsPerByte	8

typedef uint32 AttributeSize;	/* XXX should be defined elsewhere */

#define MaxHeapTupleSize		0x7fffffff
#define MaxAttributeSize		0x7fffffff

#define MaxIndexAttributeNumber 7


#endif	 /* MEMUTILS_H */
