/*-------------------------------------------------------------------------
 *
 * hashscan.c--
 *    manage scans on hash tables
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/access/hash/hashscan.c,v 1.5 1996/10/31 08:24:42 scrappy Exp $
 *
 * NOTES
 *    Because we can be doing an index scan on a relation while we
 *    update it, we need to avoid missing data that moves around in
 *    the index.  The routines and global variables in this file
 *    guarantee that all scans in the local address space stay
 *    correctly positioned.  This is all we need to worry about, since
 *    write locking guarantees that no one else will be on the same
 *    page at the same time as we are.
 *
 *    The scheme is to manage a list of active scans in the current
 *    backend.  Whenever we add or remove records from an index, we
 *    check the list of active scans to see if any has been affected.
 *    A scan is affected only if it is on the same relation, and the
 *    same page, as the update.
 *
 *-------------------------------------------------------------------------
 */

#include <time.h>

#include "postgres.h"
 
#include "catalog/pg_attribute.h"
#include "access/attnum.h"
#include "nodes/nodes.h"
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
#include "utils/nabstime.h"
#include "access/htup.h"
#include "access/itup.h"   
#include "storage/itemid.h"
#include "storage/item.h"
#include "storage/buf.h"
#include "storage/page.h"
#include "storage/bufpage.h"
#include "access/sdir.h"
#include "access/funcindex.h"
#include "utils/tqual.h"
#include "access/relscan.h"
#include "access/hash.h"

#include "utils/palloc.h"

static void _hash_scandel(IndexScanDesc scan, BlockNumber blkno, OffsetNumber offno);
static bool _hash_scantouched(IndexScanDesc scan, BlockNumber blkno, OffsetNumber offno);

typedef struct HashScanListData {
    IndexScanDesc		hashsl_scan;
    struct HashScanListData	*hashsl_next;
} HashScanListData;

typedef HashScanListData	*HashScanList;

static HashScanList	HashScans = (HashScanList) NULL;

/*
 *  _Hash_regscan() -- register a new scan.
 */
void
_hash_regscan(IndexScanDesc scan)
{
    HashScanList new_el;
    
    new_el = (HashScanList) palloc(sizeof(HashScanListData));
    new_el->hashsl_scan = scan;
    new_el->hashsl_next = HashScans;
    HashScans = new_el;
}

/*
 *  _hash_dropscan() -- drop a scan from the scan list
 */
void
_hash_dropscan(IndexScanDesc scan)
{
    HashScanList chk, last;
    
    last = (HashScanList) NULL;
    for (chk = HashScans;
	 chk != (HashScanList) NULL && chk->hashsl_scan != scan;
	 chk = chk->hashsl_next) {
	last = chk;
    }
    
    if (chk == (HashScanList) NULL)
	elog(WARN, "hash scan list trashed; can't find 0x%lx", scan);
    
    if (last == (HashScanList) NULL)
	HashScans = chk->hashsl_next;
    else
	last->hashsl_next = chk->hashsl_next;
    
#ifdef PERFECT_MEM
    pfree (chk);
#endif /* PERFECT_MEM */
}

void
_hash_adjscans(Relation rel, ItemPointer tid)
{
    HashScanList l;
    Oid relid;
    
    relid = rel->rd_id;
    for (l = HashScans; l != (HashScanList) NULL; l = l->hashsl_next) {
	if (relid == l->hashsl_scan->relation->rd_id)
	    _hash_scandel(l->hashsl_scan, ItemPointerGetBlockNumber(tid),
			  ItemPointerGetOffsetNumber(tid));
    }
}

static void
_hash_scandel(IndexScanDesc scan, BlockNumber blkno, OffsetNumber offno)
{
    ItemPointer current;
    Buffer buf;
    Buffer metabuf;
    HashScanOpaque so;
    
    if (!_hash_scantouched(scan, blkno, offno))
	return;
    
    metabuf = _hash_getbuf(scan->relation, HASH_METAPAGE, HASH_READ);
    
    so = (HashScanOpaque) scan->opaque;
    buf = so->hashso_curbuf;
    
    current = &(scan->currentItemData);
    if (ItemPointerIsValid(current)
	&& ItemPointerGetBlockNumber(current) == blkno
	&& ItemPointerGetOffsetNumber(current) >= offno) {
	_hash_step(scan, &buf, BackwardScanDirection, metabuf);
	so->hashso_curbuf = buf;
    }
    
    current = &(scan->currentMarkData);
    if (ItemPointerIsValid(current)
	&& ItemPointerGetBlockNumber(current) == blkno
	&& ItemPointerGetOffsetNumber(current) >= offno) {
	ItemPointerData tmp;
	tmp = *current;
	*current = scan->currentItemData;
	scan->currentItemData = tmp;
	_hash_step(scan, &buf, BackwardScanDirection, metabuf);
	so->hashso_mrkbuf = buf;
	tmp = *current;
	*current = scan->currentItemData;
	scan->currentItemData = tmp;
    }
}

static bool
_hash_scantouched(IndexScanDesc scan,
		  BlockNumber blkno,
		  OffsetNumber offno)
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
