/*-------------------------------------------------------------------------
 *
 * fstack.h--
 *    Fixed format stack definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: fstack.h,v 1.1 1996/08/28 07:22:37 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * Note:
 *	Fixed format stacks assist in the construction of FIFO stacks of
 *	fixed format structures.  Structures which are to be stackable
 *	should contain a FixedItemData component.  A stack is initilized
 *	with the offset of the FixedItemData component of the structure
 *	it will hold.  By doing so, push and pop operations are simplified
 *	for the callers.  All references to stackable items are pointers
 *	to the base of the structure instead of pointers to the
 *	FixedItemData component.
 *
 */
#ifndef	FSTACK_H
#define FSTACK_H

#include "c.h"

/*
 * FixedItem --
 *	Fixed format stackable item chain component.
 *
 * Note:
 *	Structures must contain one FixedItemData component per stack in
 *	which it will be an item.
 */
typedef struct FixedItemData	FixedItemData;
typedef FixedItemData		*FixedItem;

struct FixedItemData {
	FixedItem	next;	/* next item or NULL */
};

/*
 * FixedStack --
 *	Fixed format stack.
 */
typedef struct FixedStackData {
	FixedItem	top;	/* Top item on the stack or NULL */
	Offset		offset;	/* Offset from struct base to item */
	/* this could be signed short int! */
} FixedStackData;

typedef FixedStackData		*FixedStack;

/*
 * FixedStackInit --
 *	Iniitializes stack for structures with given fixed component offset.
 *
 * Exceptions:
 *	BadArg if stack is invalid pointer.
 */
extern void FixedStackInit(FixedStack stack, Offset offset);

/*
 * FixedStackPop --
 *	Returns pointer to top structure on stack or NULL if empty stack.
 *
 * Exceptions:
 *	BadArg if stack is invalid.
 */
Pointer FixedStackPop(FixedStack stack);

/*
 * FixedStackPush --
 *	Places structure associated with pointer onto top of stack.
 *
 * Exceptions:
 *	BadArg if stack is invalid.
 *	BadArg if pointer is invalid.
 */
extern void FixedStackPush(FixedStack stack, Pointer pointer);

/*
 * FixedStackGetTop --
 *	Returns pointer to top structure of a stack.  This item is not poped.
 *
 * Note:
 *	This is not part of the normal stack interface.  It is intended for
 *	 debugging use only.
 *
 * Exceptions:
 *	BadArg if stack is invalid.
 */
extern Pointer FixedStackGetTop(FixedStack stack);

/*
 * FixedStackGetNext --
 *	Returns pointer to next structure after pointer of a stack.
 *
 * Note:
 *	This is not part of the normal stack interface.  It is intended for
 *	 debugging use only.
 *
 * Exceptions:
 *	BadArg if stack is invalid.
 *	BadArg if pointer is invalid.
 *	BadArg if stack does not contain pointer.
 */
extern Pointer FixedStackGetNext(FixedStack stack, Pointer pointer);

#endif	/* FSTACK_H */
