/*-------------------------------------------------------------------------
 *
 * itemptr.h
 *	  POSTGRES disk item pointer definitions.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/itemptr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ITEMPTR_H
#define ITEMPTR_H

#include "storage/block.h"
#include "storage/off.h"

/*
 * ItemPointer:
 *
 * This is a pointer to an item within a disk page of a known file
 * (for example, a cross-link from an index to its parent table).
 * ip_blkid tells us which block, ip_posid tells us which entry in
 * the linp (ItemIdData) array we want.
 *
 * Note: because there is an item pointer in each tuple header and index
 * tuple header on disk, it's very important not to waste space with
 * structure padding bytes.  The struct is designed to be six bytes long
 * (it contains three int16 fields) but a few compilers will pad it to
 * eight bytes unless coerced.  We apply appropriate persuasion where
 * possible.  If your compiler can't be made to play along, you'll waste
 * lots of space.
 */
typedef struct ItemPointerData
{
	BlockIdData ip_blkid;
	OffsetNumber ip_posid;
}

/* If compiler understands packed and aligned pragmas, use those */
#if defined(pg_attribute_packed) && defined(pg_attribute_aligned)
			pg_attribute_packed()
			pg_attribute_aligned(2)
#endif
ItemPointerData;

typedef ItemPointerData *ItemPointer;

/* ----------------
 *		special values used in heap tuples (t_ctid)
 * ----------------
 */

/*
 * If a heap tuple holds a speculative insertion token rather than a real
 * TID, ip_posid is set to SpecTokenOffsetNumber, and the token is stored in
 * ip_blkid. SpecTokenOffsetNumber must be higher than MaxOffsetNumber, so
 * that it can be distinguished from a valid offset number in a regular item
 * pointer.
 */
#define SpecTokenOffsetNumber		0xfffe

/*
 * When a tuple is moved to a different partition by UPDATE, the t_ctid of
 * the old tuple version is set to this magic value.
 */
#define MovedPartitionsOffsetNumber 0xfffd
#define MovedPartitionsBlockNumber	InvalidBlockNumber


/* ----------------
 *		support functions
 * ----------------
 */

/*
 * ItemPointerIsValid
 *		True iff the disk item pointer is not NULL.
 */
static inline bool
ItemPointerIsValid(const ItemPointerData *pointer)
{
	return PointerIsValid(pointer) && pointer->ip_posid != 0;
}

/*
 * ItemPointerGetBlockNumberNoCheck
 *		Returns the block number of a disk item pointer.
 */
static inline BlockNumber
ItemPointerGetBlockNumberNoCheck(const ItemPointerData *pointer)
{
	return BlockIdGetBlockNumber(&pointer->ip_blkid);
}

/*
 * ItemPointerGetBlockNumber
 *		As above, but verifies that the item pointer looks valid.
 */
static inline BlockNumber
ItemPointerGetBlockNumber(const ItemPointerData *pointer)
{
	Assert(ItemPointerIsValid(pointer));
	return ItemPointerGetBlockNumberNoCheck(pointer);
}

/*
 * ItemPointerGetOffsetNumberNoCheck
 *		Returns the offset number of a disk item pointer.
 */
static inline OffsetNumber
ItemPointerGetOffsetNumberNoCheck(const ItemPointerData *pointer)
{
	return pointer->ip_posid;
}

/*
 * ItemPointerGetOffsetNumber
 *		As above, but verifies that the item pointer looks valid.
 */
static inline OffsetNumber
ItemPointerGetOffsetNumber(const ItemPointerData *pointer)
{
	Assert(ItemPointerIsValid(pointer));
	return ItemPointerGetOffsetNumberNoCheck(pointer);
}

/*
 * ItemPointerSet
 *		Sets a disk item pointer to the specified block and offset.
 */
static inline void
ItemPointerSet(ItemPointerData *pointer, BlockNumber blockNumber, OffsetNumber offNum)
{
	Assert(PointerIsValid(pointer));
	BlockIdSet(&pointer->ip_blkid, blockNumber);
	pointer->ip_posid = offNum;
}

/*
 * ItemPointerSetBlockNumber
 *		Sets a disk item pointer to the specified block.
 */
static inline void
ItemPointerSetBlockNumber(ItemPointerData *pointer, BlockNumber blockNumber)
{
	Assert(PointerIsValid(pointer));
	BlockIdSet(&pointer->ip_blkid, blockNumber);
}

/*
 * ItemPointerSetOffsetNumber
 *		Sets a disk item pointer to the specified offset.
 */
static inline void
ItemPointerSetOffsetNumber(ItemPointerData *pointer, OffsetNumber offsetNumber)
{
	Assert(PointerIsValid(pointer));
	pointer->ip_posid = offsetNumber;
}

/*
 * ItemPointerCopy
 *		Copies the contents of one disk item pointer to another.
 *
 * Should there ever be padding in an ItemPointer this would need to be handled
 * differently as it's used as hash key.
 */
static inline void
ItemPointerCopy(const ItemPointerData *fromPointer, ItemPointerData *toPointer)
{
	Assert(PointerIsValid(toPointer));
	Assert(PointerIsValid(fromPointer));
	*toPointer = *fromPointer;
}

/*
 * ItemPointerSetInvalid
 *		Sets a disk item pointer to be invalid.
 */
static inline void
ItemPointerSetInvalid(ItemPointerData *pointer)
{
	Assert(PointerIsValid(pointer));
	BlockIdSet(&pointer->ip_blkid, InvalidBlockNumber);
	pointer->ip_posid = InvalidOffsetNumber;
}

/*
 * ItemPointerIndicatesMovedPartitions
 *		True iff the block number indicates the tuple has moved to another
 *		partition.
 */
static inline bool
ItemPointerIndicatesMovedPartitions(const ItemPointerData *pointer)
{
	return
		ItemPointerGetOffsetNumber(pointer) == MovedPartitionsOffsetNumber &&
		ItemPointerGetBlockNumberNoCheck(pointer) == MovedPartitionsBlockNumber;
}

/*
 * ItemPointerSetMovedPartitions
 *		Indicate that the item referenced by the itempointer has moved into a
 *		different partition.
 */
static inline void
ItemPointerSetMovedPartitions(ItemPointerData *pointer)
{
	ItemPointerSet(pointer, MovedPartitionsBlockNumber, MovedPartitionsOffsetNumber);
}

/* ----------------
 *		externs
 * ----------------
 */

extern bool ItemPointerEquals(ItemPointer pointer1, ItemPointer pointer2);
extern int32 ItemPointerCompare(ItemPointer arg1, ItemPointer arg2);
extern void ItemPointerInc(ItemPointer pointer);
extern void ItemPointerDec(ItemPointer pointer);

/* ----------------
 *		Datum conversion functions
 * ----------------
 */

static inline ItemPointer
DatumGetItemPointer(Datum X)
{
	return (ItemPointer) DatumGetPointer(X);
}

static inline Datum
ItemPointerGetDatum(const ItemPointerData *X)
{
	return PointerGetDatum(X);
}

#define PG_GETARG_ITEMPOINTER(n) DatumGetItemPointer(PG_GETARG_DATUM(n))
#define PG_RETURN_ITEMPOINTER(x) return ItemPointerGetDatum(x)

#endif							/* ITEMPTR_H */
