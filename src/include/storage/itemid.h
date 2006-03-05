/*-------------------------------------------------------------------------
 *
 * itemid.h
 *	  Standard POSTGRES buffer page item identifier definitions.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/itemid.h,v 1.26 2006/03/05 15:58:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ITEMID_H
#define ITEMID_H

/*
 * An item pointer (also called line pointer) on a buffer page
 */
typedef struct ItemIdData
{								/* line pointers */
	unsigned	lp_off:15,		/* offset to start of tuple */
				lp_flags:2,		/* flags for tuple */
				lp_len:15;		/* length of tuple */
} ItemIdData;

typedef ItemIdData *ItemId;

/*
 * lp_flags contains these flags:
 */
#define LP_USED			0x01	/* this line pointer is being used */

#define LP_DELETE		0x02	/* item is to be deleted */

#define ItemIdDeleted(itemId) \
	(((itemId)->lp_flags & LP_DELETE) != 0)

/*
 * This bit may be passed to PageAddItem together with
 * LP_USED & LP_DELETE bits to specify overwrite mode
 */
#define OverwritePageMode	0x10

/*
 * Item offsets, lengths, and flags are represented by these types when
 * they're not actually stored in an ItemIdData.
 */
typedef uint16 ItemOffset;
typedef uint16 ItemLength;

typedef bits16 ItemIdFlags;


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
 * ItemIdIsValid
 *		True iff disk item identifier is valid.
 */
#define ItemIdIsValid(itemId)	PointerIsValid(itemId)

/*
 * ItemIdIsUsed
 *		True iff disk item identifier is in use.
 *
 * Note:
 *		Assumes disk item identifier is valid.
 */
#define ItemIdIsUsed(itemId) \
( \
	AssertMacro(ItemIdIsValid(itemId)), \
	(bool) (((itemId)->lp_flags & LP_USED) != 0) \
)

#endif   /* ITEMID_H */
