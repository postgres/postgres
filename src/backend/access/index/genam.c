/*-------------------------------------------------------------------------
 *
 * genam.c--
 *    general index access method routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/access/index/genam.c,v 1.1.1.1 1996/07/09 06:21:11 scrappy Exp $
 *
 * NOTES
 *    many of the old access method routines have been turned into
 *    macros and moved to genam.h -cim 4/30/91
 *
 *-------------------------------------------------------------------------
 */
/*
 * OLD COMMENTS
 * Scans are implemented as follows:
 *
 * `0' represents an invalid item pointer.
 * `-' represents an unknown item pointer.
 * `X' represents a known item pointers.
 * `+' represents known or invalid item pointers.
 * `*' represents any item pointers.
 *
 * State is represented by a triple of these symbols in the order of
 * previous, current, next.  Note that the case of reverse scans works
 * identically.
 *
 *	State	Result
 * (1)	+ + -	+ 0 0		(if the next item pointer is invalid)
 * (2)		+ X -		(otherwise)
 * (3)	* 0 0	* 0 0		(no change)
 * (4)	+ X 0	X 0 0		(shift)
 * (5)	* + X	+ X -		(shift, add unknown)
 *
 * All other states cannot occur.
 *
 * Note:
 *It would be possible to cache the status of the previous and
 *	next item pointer using the flags.
 * ----------------------------------------------------------------
 */
#include "postgres.h"

#include "access/attnum.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/itup.h"
#include "access/relscan.h"
#include "access/sdir.h"
#include "access/skey.h"

#include "storage/bufmgr.h"
#include "utils/elog.h"
#include "utils/palloc.h"
#include "utils/rel.h"

#include "catalog/catname.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_index.h"
#include "catalog/pg_proc.h"

#include "catalog/index.h"

/* ----------------------------------------------------------------
 *	general access method routines
 *
 *	All indexed access methods use an identical scan structure.
 *	We don't know how the various AMs do locking, however, so we don't
 *	do anything about that here.
 *
 *	The intent is that an AM implementor will define a front-end routine
 *	that calls this one, to fill in the scan, and then does whatever kind
 *	of locking he wants.
 * ----------------------------------------------------------------
 */

/* ----------------
 *  RelationGetIndexScan -- Create and fill an IndexScanDesc.
 *
 *	This routine creates an index scan structure and sets its contents
 *	up correctly. This routine calls AMrescan to set up the scan with
 *	the passed key.
 *
 *	Parameters:
 *		relation -- index relation for scan.
 *		scanFromEnd -- if true, begin scan at one of the index's
 *			       endpoints.
 *		numberOfKeys -- count of scan keys (more than one won't
 *				necessarily do anything useful, yet).
 *		key -- the ScanKey for the starting position of the scan.
 *
 *	Returns:
 *		An initialized IndexScanDesc.
 *
 *	Side Effects:
 *		Bumps the ref count on the relation to keep it in the cache.
 *	
 * ----------------
 */
IndexScanDesc
RelationGetIndexScan(Relation relation,
		     bool scanFromEnd,
		     uint16 numberOfKeys,
		     ScanKey key)
{
    IndexScanDesc	scan;
    
    if (! RelationIsValid(relation))
	elog(WARN, "RelationGetIndexScan: relation invalid");
    
    scan = (IndexScanDesc) palloc(sizeof(IndexScanDescData));
    
    scan->relation = relation;
    scan->opaque = NULL;
    scan->numberOfKeys = numberOfKeys;
    
    ItemPointerSetInvalid(&scan->previousItemData);
    ItemPointerSetInvalid(&scan->currentItemData);
    ItemPointerSetInvalid(&scan->nextItemData);
    ItemPointerSetInvalid(&scan->previousMarkData);
    ItemPointerSetInvalid(&scan->currentMarkData);
    ItemPointerSetInvalid(&scan->nextMarkData);

    if (numberOfKeys > 0) {
	scan->keyData = (ScanKey) palloc(sizeof(ScanKeyData) * numberOfKeys);
    } else {
	scan->keyData = NULL;
    }

    index_rescan(scan, scanFromEnd, key);
    
    return (scan);
}

/* ----------------
 *  IndexScanRestart -- Restart an index scan.
 *
 *	This routine isn't used by any existing access method.  It's
 *	appropriate if relation level locks are what you want.
 *
 *  Returns:
 *	None.
 *
 *  Side Effects:
 *	None.
 * ----------------
 */
void
IndexScanRestart(IndexScanDesc scan,
		 bool scanFromEnd,
		 ScanKey key)
{
    if (! IndexScanIsValid(scan))
	elog(WARN, "IndexScanRestart: invalid scan");
    
    ItemPointerSetInvalid(&scan->previousItemData);
    ItemPointerSetInvalid(&scan->currentItemData);
    ItemPointerSetInvalid(&scan->nextItemData);
    
    if (RelationGetNumberOfBlocks(scan->relation) == 0) 
	scan->flags = ScanUnmarked;
    else if (scanFromEnd)
	scan->flags = ScanUnmarked | ScanUncheckedPrevious;
    else
	scan->flags = ScanUnmarked | ScanUncheckedNext;
    
    scan->scanFromEnd = (bool) scanFromEnd;
    
    if (scan->numberOfKeys > 0)
	memmove(scan->keyData,
		key,
		scan->numberOfKeys * sizeof(ScanKeyData));
}

/* ----------------
 *  IndexScanEnd -- End and index scan.
 *
 *	This routine is not used by any existing access method, but is
 *	suitable for use if you don't want to do sophisticated locking.
 *
 *  Returns:
 *	None.
 *
 *  Side Effects:
 *	None.
 * ----------------
 */
void
IndexScanEnd(IndexScanDesc scan)
{
    if (! IndexScanIsValid(scan))
	elog(WARN, "IndexScanEnd: invalid scan");
    
    pfree(scan);
}

/* ----------------
 *  IndexScanMarkPosition -- Mark current position in a scan.
 *
 *	This routine isn't used by any existing access method, but is the
 *	one that AM implementors should use, if they don't want to do any
 *	special locking.  If relation-level locking is sufficient, this is
 *	the routine for you.
 *
 *  Returns:
 *	None.
 *
 *  Side Effects:
 *	None.
 * ----------------
 */
void
IndexScanMarkPosition(IndexScanDesc scan)
{
    RetrieveIndexResult	result;
    
    if (scan->flags & ScanUncheckedPrevious) {
	result = 
	    index_getnext(scan, BackwardScanDirection);
	
	if (result != NULL) {
	    scan->previousItemData = result->index_iptr;
	} else {
	    ItemPointerSetInvalid(&scan->previousItemData);
	}
	
    } else if (scan->flags & ScanUncheckedNext) {
	result = (RetrieveIndexResult)
	    index_getnext(scan, ForwardScanDirection);
	
	if (result != NULL) {
	    scan->nextItemData = result->index_iptr;
	} else {
	    ItemPointerSetInvalid(&scan->nextItemData);
	}
    }
    
    scan->previousMarkData = scan->previousItemData;
    scan->currentMarkData = scan->currentItemData;
    scan->nextMarkData = scan->nextItemData;
    
    scan->flags = 0x0;	/* XXX should have a symbolic name */
}

/* ----------------
 *  IndexScanRestorePosition -- Restore position on a marked scan.
 *
 *	This routine isn't used by any existing access method, but is the
 *	one that AM implementors should use if they don't want to do any
 *	special locking.  If relation-level locking is sufficient, then
 *	this is the one you want.
 *
 *  Returns:
 *	None.
 *
 *  Side Effects:
 *	None.
 * ----------------
 */
void
IndexScanRestorePosition(IndexScanDesc scan)
{	
    if (scan->flags & ScanUnmarked) 
	elog(WARN, "IndexScanRestorePosition: no mark to restore");
    
    scan->previousItemData = scan->previousMarkData;
    scan->currentItemData = scan->currentMarkData;
    scan->nextItemData = scan->nextMarkData;
    
    scan->flags = 0x0;	/* XXX should have a symbolic name */
}
