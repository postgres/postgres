/*-------------------------------------------------------------------------
 *
 * itemid.h--
 *	  Standard POSTGRES buffer page item identifier definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: itemid.h,v 1.5 1998/01/13 04:05:12 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ITEMID_H
#define ITEMID_H

typedef uint16 ItemOffset;
typedef uint16 ItemLength;

typedef bits16 ItemIdFlags;



typedef struct ItemIdData
{								/* line pointers */
	unsigned	lp_off:15,		/* offset to find tup */
	/* can be reduced by 2 if necc. */
				lp_flags:2,		/* flags on tuple */
				lp_len:15;		/* length of tuple */
} ItemIdData;

typedef struct ItemIdData *ItemId;

#ifndef LP_USED
#define LP_USED			0x01	/* this line pointer is being used */
#endif

/* ----------------
 *		support macros
 * ----------------
 */
/*
 *		ItemIdGetLength
 */
#define ItemIdGetLength(itemId) \
   ((itemId)->lp_len)

/*
 *		ItemIdGetOffset
 */
#define ItemIdGetOffset(itemId) \
   ((itemId)->lp_off)

/*
 *		ItemIdGetFlags
 */
#define ItemIdGetFlags(itemId) \
   ((itemId)->lp_flags)

/*
 * ItemIdIsValid --
 *		True iff disk item identifier is valid.
 */
#define ItemIdIsValid(itemId)	PointerIsValid(itemId)

/*
 * ItemIdIsUsed --
 *		True iff disk item identifier is in use.
 *
 * Note:
 *		Assumes disk item identifier is valid.
 */
#define ItemIdIsUsed(itemId) \
	(AssertMacro(ItemIdIsValid(itemId)) ? \
	 (bool) (((itemId)->lp_flags & LP_USED) != 0) : false)

#endif							/* ITEMID_H */
