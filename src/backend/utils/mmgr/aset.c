/*-------------------------------------------------------------------------
 *
 * aset.c--
 *	  Allocation set definitions.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/mmgr/aset.c,v 1.9 1998/06/15 19:29:51 momjian Exp $
 *
 * NOTE
 *	  XXX This is a preliminary implementation which lacks fail-fast
 *	  XXX validity checking of arguments.
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include "postgres.h"
#include "utils/excid.h"		/* for ExhaustedMemory */
#include "utils/memutils.h"		/* where funnction declarations go */
#ifndef HAVE_MEMMOVE
#include <regex/utils.h>
#else
#include <string.h>
#endif

static void AllocPointerDump(AllocPointer pointer);
static int
AllocSetIterate(AllocSet set,
				void (*function) (AllocPointer pointer));

#undef AllocSetReset
#undef malloc
#undef free

/*
 * Internal type definitions
 */

/*
 * AllocElem --
 *		Allocation element.
 */
typedef struct AllocElemData
{
	OrderedElemData elemData;	/* elem in AllocSet */
	Size		size;
} AllocElemData;

typedef AllocElemData *AllocElem;


/*
 * Private method definitions
 */

/*
 * AllocPointerGetAllocElem --
 *		Returns allocation (internal) elem given (external) pointer.
 */
#define AllocPointerGetAllocElem(pointer)		(&((AllocElem)(pointer))[-1])

/*
 * AllocElemGetAllocPointer --
 *		Returns allocation (external) pointer given (internal) elem.
 */
#define AllocElemGetAllocPointer(alloc) ((AllocPointer)&(alloc)[1])

/*
 * AllocElemIsValid --
 *		True iff alloc is valid.
 */
#define AllocElemIsValid(alloc) PointerIsValid(alloc)

/* non-export function prototypes */
static AllocPointer AllocSetGetFirst(AllocSet set);
static AllocPointer AllocPointerGetNext(AllocPointer pointer);

/*
 * Public routines
 */

/*
 *		AllocPointerIsValid(pointer)
 *		AllocSetIsValid(set)
 *
 *				.. are now macros in aset.h -cim 4/27/91
 */

/*
 * AllocSetInit --
 *		Initializes given allocation set.
 *
 * Note:
 *		The semantics of the mode are explained above.	Limit is ignored
 *		for dynamic and static modes.
 *
 * Exceptions:
 *		BadArg if set is invalid pointer.
 *		BadArg if mode is invalid.
 */
void
AllocSetInit(AllocSet set, AllocMode mode, Size limit)
{
	AssertArg(PointerIsValid(set));
	AssertArg((int) DynamicAllocMode <= (int) mode);
	AssertArg((int) mode <= (int) BoundedAllocMode);

	/*
	 * XXX mode is currently ignored and treated as DynamicAllocMode. XXX
	 * limit is also ignored.  This affects this whole file.
	 */

	OrderedSetInit(&set->setData, offsetof(AllocElemData, elemData));
}

/*
 * AllocSetReset --
 *		Frees memory which is allocated in the given set.
 *
 * Exceptions:
 *		BadArg if set is invalid.
 */
void
AllocSetReset(AllocSet set)
{
	AllocPointer pointer;

	AssertArg(AllocSetIsValid(set));

	while (AllocPointerIsValid(pointer = AllocSetGetFirst(set)))
		AllocSetFree(set, pointer);
}

#ifdef NOT_USED
void
AllocSetReset_debug(char *file, int line, AllocSet set)
{
	AllocPointer pointer;

	AssertArg(AllocSetIsValid(set));

	while (AllocPointerIsValid(pointer = AllocSetGetFirst(set)))
		AllocSetFree(set, pointer);
}

#endif

/*
 * AllocSetContains --
 *		True iff allocation set contains given allocation element.
 *
 * Exceptions:
 *		BadArg if set is invalid.
 *		BadArg if pointer is invalid.
 */
bool
AllocSetContains(AllocSet set, AllocPointer pointer)
{
	AssertArg(AllocSetIsValid(set));
	AssertArg(AllocPointerIsValid(pointer));

	return (OrderedSetContains(&set->setData,
						  &AllocPointerGetAllocElem(pointer)->elemData));
}

/*
 * AllocSetAlloc --
 *		Returns pointer to allocated memory of given size; memory is added
 *		to the set.
 *
 * Exceptions:
 *		BadArg if set is invalid.
 *		MemoryExhausted if allocation fails.
 */
AllocPointer
AllocSetAlloc(AllocSet set, Size size)
{
	AllocElem	alloc;

	AssertArg(AllocSetIsValid(set));

	/* allocate */
	alloc = (AllocElem) malloc(sizeof(*alloc) + size);

	if (!PointerIsValid(alloc))
		elog(FATAL, "palloc failure: memory exhausted");

	/* add to allocation list */
	OrderedElemPushInto(&alloc->elemData, &set->setData);

	/* set size */
	alloc->size = size;

	return (AllocElemGetAllocPointer(alloc));
}

/*
 * AllocSetFree --
 *		Frees allocated memory; memory is removed from the set.
 *
 * Exceptions:
 *		BadArg if set is invalid.
 *		BadArg if pointer is invalid.
 *		BadArg if pointer is not member of set.
 */
void
AllocSetFree(AllocSet set, AllocPointer pointer)
{
	AllocElem	alloc;

	/* AssertArg(AllocSetIsValid(set)); */
	/* AssertArg(AllocPointerIsValid(pointer)); */
	AssertArg(AllocSetContains(set, pointer));

	alloc = AllocPointerGetAllocElem(pointer);

	/* remove from allocation set */
	OrderedElemPop(&alloc->elemData);

	/* free storage */
	free(alloc);
}

/*
 * AllocSetRealloc --
 *		Returns new pointer to allocated memory of given size; this memory
 *		is added to the set.  Memory associated with given pointer is copied
 *		into the new memory, and the old memory is freed.
 *
 * Exceptions:
 *		BadArg if set is invalid.
 *		BadArg if pointer is invalid.
 *		BadArg if pointer is not member of set.
 *		MemoryExhausted if allocation fails.
 */
AllocPointer
AllocSetRealloc(AllocSet set, AllocPointer pointer, Size size)
{
	AllocPointer newPointer;
	AllocElem	alloc;

	/* AssertArg(AllocSetIsValid(set)); */
	/* AssertArg(AllocPointerIsValid(pointer)); */
	AssertArg(AllocSetContains(set, pointer));

	/*
	 * Calling realloc(3) directly is not be possible (unless we use our
	 * own hacked version of malloc) since we must keep the allocations in
	 * the allocation set.
	 */

	alloc = AllocPointerGetAllocElem(pointer);

	/* allocate new pointer */
	newPointer = AllocSetAlloc(set, size);

	/* fill new memory */
	memmove(newPointer, pointer, (alloc->size < size) ? alloc->size : size);

	/* free old pointer */
	AllocSetFree(set, pointer);

	return (newPointer);
}

/*
 * AllocSetIterate --
 *		Returns size of set.  Iterates through set elements calling function
 *		(if valid) on each.
 *
 * Note:
 *		This was written as an aid to debugging.  It is intended for
 *		debugging use only.
 *
 * Exceptions:
 *		BadArg if set is invalid.
 */
static int
AllocSetIterate(AllocSet set,
				void (*function) (AllocPointer pointer))
{
	int			count = 0;
	AllocPointer pointer;

	AssertArg(AllocSetIsValid(set));

	for (pointer = AllocSetGetFirst(set);
		 AllocPointerIsValid(pointer);
		 pointer = AllocPointerGetNext(pointer))
	{

		if (PointerIsValid(function))
			(*function) (pointer);
		count += 1;
	}

	return (count);
}

#ifdef NOT_USED
int
AllocSetCount(AllocSet set)
{
	int			count = 0;
	AllocPointer pointer;

	AssertArg(AllocSetIsValid(set));

	for (pointer = AllocSetGetFirst(set);
		 AllocPointerIsValid(pointer);
		 pointer = AllocPointerGetNext(pointer))
		count++;
	return count;
}

#endif

/*
 * Private routines
 */

/*
 * AllocSetGetFirst --
 *		Returns "first" allocation pointer in a set.
 *
 * Note:
 *		Assumes set is valid.
 */
static AllocPointer
AllocSetGetFirst(AllocSet set)
{
	AllocElem	alloc;

	alloc = (AllocElem) OrderedSetGetHead(&set->setData);

	if (!AllocElemIsValid(alloc))
		return (NULL);

	return (AllocElemGetAllocPointer(alloc));
}

/*
 * AllocPointerGetNext --
 *		Returns "successor" allocation pointer.
 *
 * Note:
 *		Assumes pointer is valid.
 */
static AllocPointer
AllocPointerGetNext(AllocPointer pointer)
{
	AllocElem	alloc;

	alloc = (AllocElem)
		OrderedElemGetSuccessor(&AllocPointerGetAllocElem(pointer)->elemData);

	if (!AllocElemIsValid(alloc))
		return (NULL);

	return (AllocElemGetAllocPointer(alloc));
}


/*
 * Debugging routines
 */

/*
 * XXX AllocPointerDump --
 *		Displays allocated pointer.
 */
static void
AllocPointerDump(AllocPointer pointer)
{
	printf("\t%-10ld@ %0#lx\n", ((long *) pointer)[-1], (long) pointer);		/* XXX */
}

/*
 * AllocSetDump --
 *		Displays allocated set.
 */
void
AllocSetDump(AllocSet set)
{
	int			count;

	count = AllocSetIterate(set, AllocPointerDump);
	printf("\ttotal %d allocations\n", count);
}
