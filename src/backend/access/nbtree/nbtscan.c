/*-------------------------------------------------------------------------
 *
 * btscan.c--
 *    manage scans on btrees.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/access/nbtree/Attic/nbtscan.c,v 1.2 1996/10/20 10:53:10 scrappy Exp $
 *
 *
 * NOTES
 *   Because we can be doing an index scan on a relation while we update
 *   it, we need to avoid missing data that moves around in the index.
 *   The routines and global variables in this file guarantee that all
 *   scans in the local address space stay correctly positioned.  This
 *   is all we need to worry about, since write locking guarantees that
 *   no one else will be on the same page at the same time as we are.
 *
 *   The scheme is to manage a list of active scans in the current backend.
 *   Whenever we add or remove records from an index, or whenever we
 *   split a leaf page, we check the list of active scans to see if any
 *   has been affected.  A scan is affected only if it is on the same
 *   relation, and the same page, as the update.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_attribute.h"
#include "access/attnum.h"
#include "nodes/pg_list.h"
#include "access/tupdesc.h"
#include "storage/fd.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "nodes/nodes.h"
#include "rewrite/prs2lock.h"
#include "access/skey.h"
#include "access/strat.h"
#include "utils/rel.h"

#include "storage/block.h"
#include "storage/off.h"
#include "storage/itemptr.h"
#include "access/itup.h"
#include "access/funcindex.h"
#include "storage/itemid.h"
#include "storage/item.h"
#include "storage/buf.h"
#include "storage/bufpage.h"
#include <time.h>
#include "utils/nabstime.h"
#include "access/htup.h"
#include "utils/tqual.h"
#include "access/relscan.h"
#include "access/sdir.h"
#include "access/nbtree.h"


typedef struct BTScanListData {
    IndexScanDesc		btsl_scan;
    struct BTScanListData	*btsl_next;
} BTScanListData;

typedef BTScanListData	*BTScanList;

static BTScanList	BTScans = (BTScanList) NULL;
     
/*
 *  _bt_regscan() -- register a new scan.
 */
void
_bt_regscan(IndexScanDesc scan)
{
    BTScanList new_el;
    
    new_el = (BTScanList) palloc(sizeof(BTScanListData));
    new_el->btsl_scan = scan;
    new_el->btsl_next = BTScans;
    BTScans = new_el;
}

/*
 *  _bt_dropscan() -- drop a scan from the scan list
 */
void
_bt_dropscan(IndexScanDesc scan)
{
    BTScanList chk, last;
    
    last = (BTScanList) NULL;
    for (chk = BTScans;
	 chk != (BTScanList) NULL && chk->btsl_scan != scan;
	 chk = chk->btsl_next) {
	last = chk;
    }
    
    if (chk == (BTScanList) NULL)
	elog(WARN, "btree scan list trashed; can't find 0x%lx", scan);
    
    if (last == (BTScanList) NULL)
	BTScans = chk->btsl_next;
    else
	last->btsl_next = chk->btsl_next;
    
#ifdef PERFECT_MEM
    pfree (chk);
#endif /* PERFECT_MEM */
}

void
_bt_adjscans(Relation rel, ItemPointer tid)
{
    BTScanList l;
    Oid relid;
    
    relid = rel->rd_id;
    for (l = BTScans; l != (BTScanList) NULL; l = l->btsl_next) {
	if (relid == l->btsl_scan->relation->rd_id)
	    _bt_scandel(l->btsl_scan, ItemPointerGetBlockNumber(tid),
			ItemPointerGetOffsetNumber(tid));
    }
}

void
_bt_scandel(IndexScanDesc scan, BlockNumber blkno, OffsetNumber offno)
{
    ItemPointer current;
    Buffer buf;
    BTScanOpaque so;
    
    if (!_bt_scantouched(scan, blkno, offno))
	return;
    
    so = (BTScanOpaque) scan->opaque;
    buf = so->btso_curbuf;
    
    current = &(scan->currentItemData);
    if (ItemPointerIsValid(current)
	&& ItemPointerGetBlockNumber(current) == blkno
	&& ItemPointerGetOffsetNumber(current) >= offno) {
	_bt_step(scan, &buf, BackwardScanDirection);
	so->btso_curbuf = buf;
    }
    
    current = &(scan->currentMarkData);
    if (ItemPointerIsValid(current)
	&& ItemPointerGetBlockNumber(current) == blkno
	&& ItemPointerGetOffsetNumber(current) >= offno) {
	ItemPointerData tmp;
	tmp = *current;
	*current = scan->currentItemData;
	scan->currentItemData = tmp;
	_bt_step(scan, &buf, BackwardScanDirection);
	so->btso_mrkbuf = buf;
	tmp = *current;
	*current = scan->currentItemData;
	scan->currentItemData = tmp;
    }
}

bool
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
