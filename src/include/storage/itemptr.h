/*-------------------------------------------------------------------------
 *
 * itemptr.h--
 *	  POSTGRES disk item pointer definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: itemptr.h,v 1.8 1998/06/15 18:40:03 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ITEMPTR_H
#define ITEMPTR_H

#include <storage/off.h>
#include <storage/block.h>

/*
 * ItemPointer:
 *
 * this is a pointer to an item on another disk page in the same file.
 * blkid tells us which block, posid tells us which entry in the linp
 * (ItemIdData) array we want.
 */
typedef struct ItemPointerData
{
	BlockIdData ip_blkid;
	OffsetNumber ip_posid;
} ItemPointerData;

typedef ItemPointerData *ItemPointer;

/* ----------------
 *		support macros
 * ----------------
 */

/*
 * ItemPointerIsValid --
 *		True iff the disk item pointer is not NULL.
 */
#define ItemPointerIsValid(pointer) \
	((bool) (PointerIsValid(pointer) && ((pointer)->ip_posid != 0)))

/*
 * ItemPointerGetBlockNumber --
 *		Returns the block number of a disk item pointer.
 */
#define ItemPointerGetBlockNumber(pointer) \
( \
	AssertMacro(ItemPointerIsValid(pointer)), \
	BlockIdGetBlockNumber(&(pointer)->ip_blkid) \
)

/*
 * ItemPointerGetOffsetNumber --
 *		Returns the offset number of a disk item pointer.
 */
#define ItemPointerGetOffsetNumber(pointer) \
( \
	AssertMacro(ItemPointerIsValid(pointer)), \
	(pointer)->ip_posid \
)

/*
 * ItemPointerSet --
 *		Sets a disk item pointer to the specified block and offset.
 */
#define ItemPointerSet(pointer, blockNumber, offNum) \
( \
	AssertMacro(PointerIsValid(pointer)), \
	BlockIdSet(&((pointer)->ip_blkid), blockNumber), \
	(pointer)->ip_posid = offNum \
)

/*
 * ItemPointerSetBlockNumber --
 *		Sets a disk item pointer to the specified block.
 */
#define ItemPointerSetBlockNumber(pointer, blockNumber) \
( \
	AssertMacro(PointerIsValid(pointer)), \
	BlockIdSet(&((pointer)->ip_blkid), blockNumber) \
)

/*
 * ItemPointerSetOffsetNumber --
 *		Sets a disk item pointer to the specified offset.
 */
#define ItemPointerSetOffsetNumber(pointer, offsetNumber) \
( \
	AssertMacro(PointerIsValid(pointer)), \
	(pointer)->ip_posid = (offsetNumber) \
)

/*
 * ItemPointerCopy --
 *		Copies the contents of one disk item pointer to another.
 */
#define ItemPointerCopy(fromPointer, toPointer) \
( \
	AssertMacro(PointerIsValid(toPointer)), \
	AssertMacro(PointerIsValid(fromPointer)), \
	*(toPointer) = *(fromPointer) \
)

/*
 * ItemPointerSetInvalid --
 *		Sets a disk item pointer to be invalid.
 */
#define ItemPointerSetInvalid(pointer) \
( \
	AssertMacro(PointerIsValid(pointer)), \
	BlockIdSet(&((pointer)->ip_blkid), InvalidBlockNumber), \
	(pointer)->ip_posid = InvalidOffsetNumber \
)

/* ----------------
 *		externs
 * ----------------
 */

extern bool ItemPointerEquals(ItemPointer pointer1, ItemPointer pointer2);

#endif							/* ITEMPTR_H */
