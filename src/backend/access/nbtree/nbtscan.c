/*-------------------------------------------------------------------------
 *
 * btscan.c
 *	  manage scans on btrees.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/nbtree/Attic/nbtscan.c,v 1.33 2001/01/24 19:42:48 momjian Exp $
 *
 *
 * NOTES
 *	 Because we can be doing an index scan on a relation while we update
 *	 it, we need to avoid missing data that moves around in the index.
 *	 Insertions and page splits are no problem because _bt_restscan()
 *	 can figure out where the current item moved to, but if a deletion
 *	 happens at or before the current scan position, we'd better do
 *	 something to stay in sync.
 *
 *	 The routines in this file handle the problem for deletions issued
 *	 by the current backend.  Currently, that's all we need, since
 *	 deletions are only done by VACUUM and it gets an exclusive lock.
 *
 *	 The scheme is to manage a list of active scans in the current backend.
 *	 Whenever we remove a record from an index, we check the list of active
 *	 scans to see if any has been affected.  A scan is affected only if it
 *	 is on the same relation, and the same page, as the update.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/nbtree.h"

typedef struct BTScanListData
{
	IndexScanDesc btsl_scan;
	struct BTScanListData *btsl_next;
} BTScanListData;

typedef BTScanListData *BTScanList;

static BTScanList BTScans = (BTScanList) NULL;

static void _bt_scandel(IndexScanDesc scan, BlockNumber blkno, OffsetNumber offno);

/*
 * AtEOXact_nbtree() --- clean up nbtree subsystem at xact abort or commit.
 *
 * This is here because it needs to touch this module's static var BTScans.
 */
void
AtEOXact_nbtree(void)
{

	/*
	 * Note: these actions should only be necessary during xact abort; but
	 * they can't hurt during a commit.
	 */

	/*
	 * Reset the active-scans list to empty. We do not need to free the
	 * list elements, because they're all palloc()'d, so they'll go away
	 * at end of transaction anyway.
	 */
	BTScans = NULL;

	/* If we were building a btree, we ain't anymore. */
	BuildingBtree = false;
}

/*
 *	_bt_regscan() -- register a new scan.
 */
void
_bt_regscan(IndexScanDesc scan)
{
	BTScanList	new_el;

	new_el = (BTScanList) palloc(sizeof(BTScanListData));
	new_el->btsl_scan = scan;
	new_el->btsl_next = BTScans;
	BTScans = new_el;
}

/*
 *	_bt_dropscan() -- drop a scan from the scan list
 */
void
_bt_dropscan(IndexScanDesc scan)
{
	BTScanList	chk,
				last;

	last = (BTScanList) NULL;
	for (chk = BTScans;
		 chk != (BTScanList) NULL && chk->btsl_scan != scan;
		 chk = chk->btsl_next)
		last = chk;

	if (chk == (BTScanList) NULL)
		elog(ERROR, "btree scan list trashed; can't find 0x%p", (void *) scan);

	if (last == (BTScanList) NULL)
		BTScans = chk->btsl_next;
	else
		last->btsl_next = chk->btsl_next;

	pfree(chk);
}

/*
 *	_bt_adjscans() -- adjust all scans in the scan list to compensate
 *					  for a given deletion
 */
void
_bt_adjscans(Relation rel, ItemPointer tid)
{
	BTScanList	l;
	Oid			relid;

	relid = RelationGetRelid(rel);
	for (l = BTScans; l != (BTScanList) NULL; l = l->btsl_next)
	{
		if (relid == RelationGetRelid(l->btsl_scan->relation))
			_bt_scandel(l->btsl_scan,
						ItemPointerGetBlockNumber(tid),
						ItemPointerGetOffsetNumber(tid));
	}
}

/*
 *	_bt_scandel() -- adjust a single scan on deletion
 *
 */
static void
_bt_scandel(IndexScanDesc scan, BlockNumber blkno, OffsetNumber offno)
{
	ItemPointer current;
	Buffer		buf;
	BTScanOpaque so;
	OffsetNumber start;
	Page		page;
	BTPageOpaque opaque;

	so = (BTScanOpaque) scan->opaque;
	buf = so->btso_curbuf;

	current = &(scan->currentItemData);
	if (ItemPointerIsValid(current)
		&& ItemPointerGetBlockNumber(current) == blkno
		&& ItemPointerGetOffsetNumber(current) >= offno)
	{
		page = BufferGetPage(buf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		start = P_FIRSTDATAKEY(opaque);
		if (ItemPointerGetOffsetNumber(current) == start)
			ItemPointerSetInvalid(&(so->curHeapIptr));
		else
		{

			/*
			 * We have to lock buffer before _bt_step and unlock it after
			 * that.
			 */
			LockBuffer(buf, BT_READ);
			_bt_step(scan, &buf, BackwardScanDirection);
			if (ItemPointerIsValid(current))
			{
				Page		pg = BufferGetPage(buf);
				BTItem		btitem = (BTItem) PageGetItem(pg,
				 PageGetItemId(pg, ItemPointerGetOffsetNumber(current)));

				so->curHeapIptr = btitem->bti_itup.t_tid;
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			}
		}
	}

	current = &(scan->currentMarkData);
	if (ItemPointerIsValid(current)
		&& ItemPointerGetBlockNumber(current) == blkno
		&& ItemPointerGetOffsetNumber(current) >= offno)
	{
		page = BufferGetPage(so->btso_mrkbuf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		start = P_FIRSTDATAKEY(opaque);

		if (ItemPointerGetOffsetNumber(current) == start)
			ItemPointerSetInvalid(&(so->mrkHeapIptr));
		else
		{
			ItemPointerData tmp;

			tmp = *current;
			*current = scan->currentItemData;
			scan->currentItemData = tmp;
			so->btso_curbuf = so->btso_mrkbuf;
			so->btso_mrkbuf = buf;
			buf = so->btso_curbuf;
			LockBuffer(buf, BT_READ);	/* as above */

			_bt_step(scan, &buf, BackwardScanDirection);

			so->btso_curbuf = so->btso_mrkbuf;
			so->btso_mrkbuf = buf;
			tmp = *current;
			*current = scan->currentItemData;
			scan->currentItemData = tmp;
			if (ItemPointerIsValid(current))
			{
				Page		pg = BufferGetPage(buf);
				BTItem		btitem = (BTItem) PageGetItem(pg,
				 PageGetItemId(pg, ItemPointerGetOffsetNumber(current)));

				so->mrkHeapIptr = btitem->bti_itup.t_tid;
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);	/* as above */
			}
		}
	}
}
