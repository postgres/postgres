/*-------------------------------------------------------------------------
 *
 * itemptr.c
 *	  POSTGRES disk item pointer code.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/page/itemptr.c,v 1.19 2006/10/04 00:29:58 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/itemptr.h"


/*
 * ItemPointerEquals
 *	Returns true if both item pointers point to the same item,
 *	 otherwise returns false.
 *
 * Note:
 *	Asserts that the disk item pointers are both valid!
 */
bool
ItemPointerEquals(ItemPointer pointer1, ItemPointer pointer2)
{
	if (ItemPointerGetBlockNumber(pointer1) ==
		ItemPointerGetBlockNumber(pointer2) &&
		ItemPointerGetOffsetNumber(pointer1) ==
		ItemPointerGetOffsetNumber(pointer2))
		return true;
	else
		return false;
}

/*
 * ItemPointerCompare
 *		Generic btree-style comparison for item pointers.
 */
int32
ItemPointerCompare(ItemPointer arg1, ItemPointer arg2)
{
	/*
	 * Don't use ItemPointerGetBlockNumber or ItemPointerGetOffsetNumber here,
	 * because they assert ip_posid != 0 which might not be true for a
	 * user-supplied TID.
	 */
	BlockNumber b1 = BlockIdGetBlockNumber(&(arg1->ip_blkid));
	BlockNumber b2 = BlockIdGetBlockNumber(&(arg2->ip_blkid));

	if (b1 < b2)
		return -1;
	else if (b1 > b2)
		return 1;
	else if (arg1->ip_posid < arg2->ip_posid)
		return -1;
	else if (arg1->ip_posid > arg2->ip_posid)
		return 1;
	else
		return 0;
}
