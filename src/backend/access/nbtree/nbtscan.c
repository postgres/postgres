/*-------------------------------------------------------------------------
 *
 * btscan.c--
 *	  manage scans on btrees.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/nbtree/Attic/nbtscan.c,v 1.10 1997/09/08 20:54:24 momjian Exp $
 *
 *
 * NOTES
 *	 Because we can be doing an index scan on a relation while we update
 *	 it, we need to avoid missing data that moves around in the index.
 *	 The routines and global variables in this file guarantee that all
 *	 scans in the local address space stay correctly positioned.  This
 *	 is all we need to worry about, since write locking guarantees that
 *	 no one else will be on the same page at the same time as we are.
 *
 *	 The scheme is to manage a list of active scans in the current backend.
 *	 Whenever we add or remove records from an index, or whenever we
 *	 split a leaf page, we check the list of active scans to see if any
 *	 has been affected.  A scan is affected only if it is on the same
 *	 relation, and the same page, as the update.
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <storage/bufpage.h>
#include <access/nbtree.h>

typedef struct BTScanListData
{
	IndexScanDesc btsl_scan;
	struct BTScanListData *btsl_next;
} BTScanListData;

typedef BTScanListData *BTScanList;

static BTScanList BTScans = (BTScanList) NULL;

static void _bt_scandel(IndexScanDesc scan, int op, BlockNumber blkno, OffsetNumber offno);
static bool _bt_scantouched(IndexScanDesc scan, BlockNumber blkno, OffsetNumber offno);

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
	{
		last = chk;
	}

	if (chk == (BTScanList) NULL)
		elog(WARN, "btree scan list trashed; can't find 0x%lx", scan);

	if (last == (BTScanList) NULL)
		BTScans = chk->btsl_next;
	else
		last->btsl_next = chk->btsl_next;

	pfree(chk);
}

/*
 *	_bt_adjscans() -- adjust all scans in the scan list to compensate
 *					  for a given deletion or insertion
 */
void
_bt_adjscans(Relation rel, ItemPointer tid, int op)
{
	BTScanList	l;
	Oid			relid;

	relid = rel->rd_id;
	for (l = BTScans; l != (BTScanList) NULL; l = l->btsl_next)
	{
		if (relid == l->btsl_scan->relation->rd_id)
			_bt_scandel(l->btsl_scan, op,
						ItemPointerGetBlockNumber(tid),
						ItemPointerGetOffsetNumber(tid));
	}
}

/*
 *	_bt_scandel() -- adjust a single scan
 *
 * because each index page is always maintained as an ordered array of
 * index tuples, the index tuples on a given page shift beneath any
 * given scan.	an index modification "behind" a scan position (i.e.,
 * same page, lower or equal offset number) will therefore force us to
 * adjust the scan in the following ways:
 *
 * - on insertion, we shift the scan forward by one item.
 * - on deletion, we shift the scan backward by one item.
 *
 * note that:
 *
 * - we need not worry about the actual ScanDirection of the scan
 * itself, since the problem is that the "current" scan position has
 * shifted.
 * - modifications "ahead" of our scan position do not change the
 * array index of the current scan position and so can be ignored.
 */
static void
_bt_scandel(IndexScanDesc scan, int op, BlockNumber blkno, OffsetNumber offno)
{
	ItemPointer current;
	Buffer		buf;
	BTScanOpaque so;

	if (!_bt_scantouched(scan, blkno, offno))
		return;

	so = (BTScanOpaque) scan->opaque;
	buf = so->btso_curbuf;

	current = &(scan->currentItemData);
	if (ItemPointerIsValid(current)
		&& ItemPointerGetBlockNumber(current) == blkno
		&& ItemPointerGetOffsetNumber(current) >= offno)
	{
		switch (op)
		{
			case BT_INSERT:
				_bt_step(scan, &buf, ForwardScanDirection);
				break;
			case BT_DELETE:
				_bt_step(scan, &buf, BackwardScanDirection);
				break;
			default:
				elog(WARN, "_bt_scandel: bad operation '%d'", op);
				/* NOTREACHED */
		}
		so->btso_curbuf = buf;
	}

	current = &(scan->currentMarkData);
	if (ItemPointerIsValid(current)
		&& ItemPointerGetBlockNumber(current) == blkno
		&& ItemPointerGetOffsetNumber(current) >= offno)
	{
		ItemPointerData tmp;

		tmp = *current;
		*current = scan->currentItemData;
		scan->currentItemData = tmp;
		switch (op)
		{
			case BT_INSERT:
				_bt_step(scan, &buf, ForwardScanDirection);
				break;
			case BT_DELETE:
				_bt_step(scan, &buf, BackwardScanDirection);
				break;
			default:
				elog(WARN, "_bt_scandel: bad operation '%d'", op);
				/* NOTREACHED */
		}
		so->btso_mrkbuf = buf;
		tmp = *current;
		*current = scan->currentItemData;
		scan->currentItemData = tmp;
	}
}

/*
 *	_bt_scantouched() -- check to see if a scan is affected by a given
 *						 change to the index
 */
static bool
_bt_scantouched(IndexScanDesc scan, BlockNumber blkno, OffsetNumber offno)
{
	ItemPointer current;

	current = &(scan->currentItemData);
	if (ItemPointerIsValid(current)
		&& ItemPointerGetBlockNumber(current) == blkno
		&& ItemPointerGetOffsetNumber(current) >= offno)
		return (true);

	current = &(scan->currentMarkData);
	if (ItemPointerIsValid(current)
		&& ItemPointerGetBlockNumber(current) == blkno
		&& ItemPointerGetOffsetNumber(current) >= offno)
		return (true);

	return (false);
}
