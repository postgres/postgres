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
 * $Id: memutils.h,v 1.31 1999/08/24 20:11:19 tgl Exp $
 *
 * NOTES
 *	  some of the information in this file will be moved to
 *	  other files, (like MaxHeapTupleSize and MaxAttributeSize).
 *
 *-------------------------------------------------------------------------
 */
#ifndef MEMUTILS_H
#define MEMUTILS_H

/*
 *	This is not needed by this include file, but by almost every file
 *	that includes this file.
 */

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

#define TYPEALIGN(ALIGNVAL,LEN)	(((long)(LEN) + (ALIGNVAL-1)) & ~(ALIGNVAL-1))

#define SHORTALIGN(LEN)			TYPEALIGN(ALIGNOF_SHORT, (LEN))
#define INTALIGN(LEN)			TYPEALIGN(ALIGNOF_INT, (LEN))
#define LONGALIGN(LEN)			TYPEALIGN(ALIGNOF_LONG, (LEN))
#define DOUBLEALIGN(LEN)		TYPEALIGN(ALIGNOF_DOUBLE, (LEN))
#define MAXALIGN(LEN)			TYPEALIGN(MAXIMUM_ALIGNOF, (LEN))

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
 *		will cause all memory allocated within it to be freed.
 *
 *		XXX The following material about allocation modes is all OUT OF DATE.
 *		aset.c currently implements only one allocation strategy,
 *		DynamicAllocMode, and that's the only one anyone ever requests anyway.
 *		If we ever did have more strategies, the new ones might or might
 *		not look like what is described here...
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
 *		"Static" mode attempts to allocate space as efficiently as possible
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
 *		There may be other modes in the future.
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
 *		See above for a description of the various modes.
 */
typedef enum AllocMode
{
	DynamicAllocMode,			/* always dynamically allocate */
	StaticAllocMode,			/* always "statically" allocate */
	TunableAllocMode,			/* allocations are "tuned" */
	BoundedAllocMode			/* allocations bounded to fixed usage */
} AllocMode;

#define DefaultAllocMode		DynamicAllocMode

typedef struct AllocSetData *AllocSet;
typedef struct AllocBlockData *AllocBlock;
typedef struct AllocChunkData *AllocChunk;

/*
 * AllocSet
 *		Allocation set.
 */
typedef struct AllocSetData
{
	AllocBlock	blocks;			/* head of list of blocks in this set */
#define ALLOCSET_NUM_FREELISTS	8
	AllocChunk	freelist[ALLOCSET_NUM_FREELISTS]; /* free chunk lists */
	/* Note: this will change in the future to support other modes */
} AllocSetData;

/*
 * AllocBlock
 *		An AllocBlock is the unit of memory that is obtained by aset.c
 *		from malloc().  It contains one or more AllocChunks, which are
 *		the units requested by palloc() and freed by pfree().  AllocChunks
 *		cannot be returned to malloc() individually, instead they are put
 *		on freelists by pfree() and re-used by the next palloc() that has
 *		a matching request size.
 *
 *		AllocBlockData is the header data for a block --- the usable space
 *		within the block begins at the next alignment boundary.
 */
typedef struct AllocBlockData
{
	AllocSet	aset;			/* aset that owns this block */
	AllocBlock	next;			/* next block in aset's blocks list */
	char	   *freeptr;		/* start of free space in this block */
	char	   *endptr;			/* end of space in this block */
} AllocBlockData;

/*
 * AllocChunk
 *		The prefix of each piece of memory in an AllocBlock
 */
typedef struct AllocChunkData
{
	/* aset is the owning aset if allocated, or the freelist link if free */
	void	   *aset;
	/* size is always the size of the usable space in the chunk */
	Size		size;
} AllocChunkData;

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
