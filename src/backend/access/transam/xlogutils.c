/*-------------------------------------------------------------------------
 *
 * xlog.c
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xlog.h"
#include "access/xact.h"

#ifdef XLOG

/*
 * Check if specified heap tuple was inserted by given
 * xaction/command and return
 *
 * - -1 if not
 * - 0  if there is no tuple at all
 * - 1  if yes
 */
int
XLogIsOwnerOfTuple(RelFileNode hnode, ItemPointer iptr, 
					TransactionId xid, CommandId cid)
{
	Relation		reln;
	Buffer			buffer;
	Page			page;
	ItemId			lp;
	HeapTupleHeader	htup;

	reln = XLogOpenRelation(false, RM_HEAP_ID, hnode);
	if (!RelationIsValid(reln))
		return(0);

	buffer = ReadBuffer(reln, ItemPointerGetBlockNumber(iptr));
	if (!BufferIsValid(buffer))
		return(0);

	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = (Page) BufferGetPage(buffer);
	if (PageIsNew((PageHeader) page) ||
		ItemPointerGetOffsetNumber(iptr) > PageGetMaxOffsetNumber(page))
	{
		UnlockAndReleaseBuffer(buffer);
		return(0);
	}
	lp = PageGetItemId(page, ItemPointerGetOffsetNumber(iptr));
	if (!ItemIdIsUsed(lp) || ItemIdDeleted(lp))
	{
		UnlockAndReleaseBuffer(buffer);
		return(0);
	}

	htup = (HeapTupleHeader) PageGetItem(page, lp);

	if (PageGetSUI(page) != ThisStartUpID || htup->t_xmin != xid || htup->t_cmin != cid)
	{
		UnlockAndReleaseBuffer(buffer);
		return(-1);
	}

	UnlockAndReleaseBuffer(buffer);
	return(1);
}

/*
 * Check if exists valid (inserted by not aborted xaction) heap tuple
 * for given item pointer
 */
bool
XLogIsValidTuple(RelFileNode hnode, ItemPointer iptr)
{
	Relation		reln;
	Buffer			buffer;
	Page			page;
	ItemId			lp;
	HeapTupleHeader	htup;

	reln = XLogOpenRelation(false, RM_HEAP_ID, hnode);
	if (!RelationIsValid(reln))
		return(false);

	buffer = ReadBuffer(reln, ItemPointerGetBlockNumber(iptr));
	if (!BufferIsValid(buffer))
		return(false);

	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = (Page) BufferGetPage(buffer);
	if (PageIsNew((PageHeader) page) ||
		ItemPointerGetOffsetNumber(iptr) > PageGetMaxOffsetNumber(page))
	{
		UnlockAndReleaseBuffer(buffer);
		return(false);
	}
	lp = PageGetItemId(page, ItemPointerGetOffsetNumber(iptr));
	if (!ItemIdIsUsed(lp) || ItemIdDeleted(lp))
	{
		UnlockAndReleaseBuffer(buffer);
		return(false);
	}

	htup = (HeapTupleHeader) PageGetItem(page, lp);

	if (XLogIsAborted(PageGetSUI(page), htup->t_xmin))
	{
		UnlockAndReleaseBuffer(buffer);
		return(false);
	}

	UnlockAndReleaseBuffer(buffer);
	return(true);
}

#endif
