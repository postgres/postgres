/*-------------------------------------------------------------------------
 *
 * fstack.c--
 *    Fixed format stack definitions.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/lib/Attic/fstack.c,v 1.1.1.1 1996/07/09 06:21:28 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"
#include "lib/fstack.h"

/*
 * Internal function definitions
 */

/*
 * FixedItemIsValid --
 *	True iff item is valid.
 */
#define FixedItemIsValid(item)	PointerIsValid(item)

/*
 * FixedStackGetItemBase --
 *	Returns base of enclosing structure.
 */
#define FixedStackGetItemBase(stack, item) \
	((Pointer)((char *)(item) - (stack)->offset))

/*
 * FixedStackGetItem --
 *	Returns item of given pointer to enclosing structure.
 */
#define FixedStackGetItem(stack, pointer) \
	((FixedItem)((char *)(pointer) + (stack)->offset))

/*
 * External functions
 */

/*
 * FixedStackIsValid --
 *	True iff stack is valid.
 */
static bool
FixedStackIsValid(FixedStack stack)
{
    return ((bool)PointerIsValid(stack));
}


void
FixedStackInit(FixedStack stack, Offset offset)
{
    AssertArg(PointerIsValid(stack));
    
    stack->top = NULL;
    stack->offset = offset;
}

Pointer
FixedStackPop(FixedStack stack)
{
    Pointer	pointer;
    
    AssertArg(FixedStackIsValid(stack));
    
    if (!PointerIsValid(stack->top)) {
	return (NULL);
    }
    
    pointer = FixedStackGetItemBase(stack, stack->top);
    stack->top = stack->top->next;
    
    return (pointer);
}

void
FixedStackPush(FixedStack stack, Pointer pointer)
{
    FixedItem	item = FixedStackGetItem(stack, pointer);
    
    AssertArg(FixedStackIsValid(stack));
    AssertArg(PointerIsValid(pointer));
    
    item->next = stack->top;
    stack->top = item;
}


/*
 * FixedStackContains --
 *	True iff ordered stack contains given element.
 *
 * Note:
 *	This is inefficient.  It is intended for debugging use only.
 *
 * Exceptions:
 *	BadArg if stack is invalid.
 *	BadArg if pointer is invalid.
 */
static bool
FixedStackContains(FixedStack stack, Pointer pointer)
{
    FixedItem	next;
    FixedItem	item;
    
    AssertArg(FixedStackIsValid(stack));
    AssertArg(PointerIsValid(pointer));
    
    item = FixedStackGetItem(stack, pointer);
    
    for (next = stack->top; FixedItemIsValid(next); next = next->next) {
	if (next == item) {
	    return (true);
	}
    }
    return (false);
}

Pointer
FixedStackGetTop(FixedStack stack)
{
    AssertArg(FixedStackIsValid(stack));
    
    if (!PointerIsValid(stack->top)) {
	return (NULL);
    }
    
    return (FixedStackGetItemBase(stack, stack->top));
}

Pointer
FixedStackGetNext(FixedStack stack, Pointer pointer)
{
    FixedItem	item;
    
    /* AssertArg(FixedStackIsValid(stack)); */
    /* AssertArg(PointerIsValid(pointer)); */
    AssertArg(FixedStackContains(stack, pointer));
    
    item = FixedStackGetItem(stack, pointer)->next;
    
    if (!PointerIsValid(item)) {
	return (NULL);
    }
    
    return(FixedStackGetItemBase(stack, item));
}
