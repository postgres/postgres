/*-------------------------------------------------------------------------
 *
 * itemptr.c--
 *	  POSTGRES disk item pointer code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/page/itemptr.c,v 1.4 1998/09/01 03:25:29 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/block.h"
#include "storage/off.h"
#include "storage/itemptr.h"
#include "storage/bufpage.h"

/*
 * ItemPointerEquals --
 *	Returns true if both item pointers point to the same item,
 *	 otherwise returns false.
 *
 * Note:
 *	Assumes that the disk item pointers are not NULL.
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
