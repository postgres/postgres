/*-------------------------------------------------------------------------
 *
 * genam.c
 *	  general index access method routines
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/index/genam.c,v 1.32 2002/03/29 22:10:32 tgl Exp $
 *
 * NOTES
 *	  many of the old access method routines have been turned into
 *	  macros and moved to genam.h -cim 4/30/91
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
 *		State	Result
 * (1)	+ + -	+ 0 0			(if the next item pointer is invalid)
 * (2)			+ X -			(otherwise)
 * (3)	* 0 0	* 0 0			(no change)
 * (4)	+ X 0	X 0 0			(shift)
 * (5)	* + X	+ X -			(shift, add unknown)
 *
 * All other states cannot occur.
 *
 * Note:
 *		It would be possible to cache the status of the previous and
 *		next item pointer using the flags.
 * ----------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "miscadmin.h"
#include "pgstat.h"


/* ----------------------------------------------------------------
 *		general access method routines
 *
 *		All indexed access methods use an identical scan structure.
 *		We don't know how the various AMs do locking, however, so we don't
 *		do anything about that here.
 *
 *		The intent is that an AM implementor will define a beginscan routine
 *		that calls RelationGetIndexScan, to fill in the scan, and then does
 *		whatever kind of locking he wants.
 *
 *		At the end of a scan, the AM's endscan routine undoes the locking,
 *		but does *not* call IndexScanEnd --- the higher-level index_endscan
 *		routine does that.	(We can't do it in the AM because index_endscan
 *		still needs to touch the IndexScanDesc after calling the AM.)
 *
 *		Because of this, the AM does not have a choice whether to call
 *		RelationGetIndexScan or not; its beginscan routine must return an
 *		object made by RelationGetIndexScan.  This is kinda ugly but not
 *		worth cleaning up now.
 * ----------------------------------------------------------------
 */

/* ----------------
 *	RelationGetIndexScan -- Create and fill an IndexScanDesc.
 *
 *		This routine creates an index scan structure and sets its contents
 *		up correctly. This routine calls AMrescan to set up the scan with
 *		the passed key.
 *
 *		Parameters:
 *				relation -- index relation for scan.
 *				scanFromEnd -- if true, begin scan at one of the index's
 *							   endpoints.
 *				numberOfKeys -- count of scan keys.
 *				key -- the ScanKey for the starting position of the scan.
 *
 *		Returns:
 *				An initialized IndexScanDesc.
 * ----------------
 */
IndexScanDesc
RelationGetIndexScan(Relation relation,
					 bool scanFromEnd,
					 uint16 numberOfKeys,
					 ScanKey key)
{
	IndexScanDesc scan;

	if (!RelationIsValid(relation))
		elog(ERROR, "RelationGetIndexScan: relation invalid");

	scan = (IndexScanDesc) palloc(sizeof(IndexScanDescData));

	scan->relation = relation;
	scan->opaque = NULL;
	scan->numberOfKeys = numberOfKeys;

	ItemPointerSetInvalid(&scan->currentItemData);
	ItemPointerSetInvalid(&scan->currentMarkData);

	pgstat_initstats(&scan->xs_pgstat_info, relation);

	/*
	 * mark cached function lookup data invalid; it will be set on first
	 * use
	 */
	scan->fn_getnext.fn_oid = InvalidOid;

	if (numberOfKeys > 0)
		scan->keyData = (ScanKey) palloc(sizeof(ScanKeyData) * numberOfKeys);
	else
		scan->keyData = NULL;

	index_rescan(scan, scanFromEnd, key);

	return scan;
}

/* ----------------
 *	IndexScanEnd -- End an index scan.
 *
 *		This routine just releases the storage acquired by
 *		RelationGetIndexScan().  Any AM-level resources are
 *		assumed to already have been released by the AM's
 *		endscan routine.
 *
 *	Returns:
 *		None.
 * ----------------
 */
void
IndexScanEnd(IndexScanDesc scan)
{
	if (!IndexScanIsValid(scan))
		elog(ERROR, "IndexScanEnd: invalid scan");

	if (scan->keyData != NULL)
		pfree(scan->keyData);

	pfree(scan);
}

#ifdef NOT_USED
/* ----------------
 *	IndexScanRestart -- Restart an index scan.
 *
 *		This routine isn't used by any existing access method.  It's
 *		appropriate if relation level locks are what you want.
 *
 *	Returns:
 *		None.
 *
 *	Side Effects:
 *		None.
 * ----------------
 */
void
IndexScanRestart(IndexScanDesc scan,
				 bool scanFromEnd,
				 ScanKey key)
{
	if (!IndexScanIsValid(scan))
		elog(ERROR, "IndexScanRestart: invalid scan");

	ItemPointerSetInvalid(&scan->currentItemData);

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
 *	IndexScanMarkPosition -- Mark current position in a scan.
 *
 *		This routine isn't used by any existing access method, but is the
 *		one that AM implementors should use, if they don't want to do any
 *		special locking.  If relation-level locking is sufficient, this is
 *		the routine for you.
 *
 *	Returns:
 *		None.
 *
 *	Side Effects:
 *		None.
 * ----------------
 */
void
IndexScanMarkPosition(IndexScanDesc scan)
{
	scan->currentMarkData = scan->currentItemData;

	scan->flags = 0x0;			/* XXX should have a symbolic name */
}

/* ----------------
 *	IndexScanRestorePosition -- Restore position on a marked scan.
 *
 *		This routine isn't used by any existing access method, but is the
 *		one that AM implementors should use if they don't want to do any
 *		special locking.  If relation-level locking is sufficient, then
 *		this is the one you want.
 *
 *	Returns:
 *		None.
 *
 *	Side Effects:
 *		None.
 * ----------------
 */
void
IndexScanRestorePosition(IndexScanDesc scan)
{
	if (scan->flags & ScanUnmarked)
		elog(ERROR, "IndexScanRestorePosition: no mark to restore");

	scan->currentItemData = scan->currentMarkData;

	scan->flags = 0x0;			/* XXX should have a symbolic name */
}

#endif


/* ----------------------------------------------------------------
 *		heap-or-index-scan access to system catalogs
 *
 *		These functions support system catalog accesses that normally use
 *		an index but need to be capable of being switched to heap scans
 *		if the system indexes are unavailable.  The interface is
 *		as easy to use as a heap scan, and hides all the extra cruft of
 *		the present indexscan API.
 *
 *		The specified scan keys must be compatible with the named index.
 *		Generally this means that they must constrain either all columns
 *		of the index, or the first K columns of an N-column index.
 *
 *		These routines would work fine with non-system tables, actually,
 *		but they're only useful when there is a known index to use with
 *		the given scan keys, so in practice they're only good for
 *		predetermined types of scans of system catalogs.
 * ----------------------------------------------------------------
 */

/*
 * systable_beginscan --- set up for heap-or-index scan
 *
 *	rel: catalog to scan, already opened and suitably locked
 *	indexRelname: name of index to conditionally use
 *	indexOK: if false, forces a heap scan (see notes below)
 *	snapshot: time qual to use (usually should be SnapshotNow)
 *	nkeys, key: scan keys
 *
 * The attribute numbers in the scan key should be set for the heap case.
 * If we choose to index, we reset them to 1..n to reference the index
 * columns.  Note this means there must be one scankey qualification per
 * index column!  This is checked by the Asserts in the normal, index-using
 * case, but won't be checked if the heapscan path is taken.
 *
 * The routine checks the normal cases for whether an indexscan is safe,
 * but caller can make additional checks and pass indexOK=false if needed.
 * In standard case indexOK can simply be constant TRUE.
 */
SysScanDesc
systable_beginscan(Relation rel,
				   const char *indexRelname,
				   bool indexOK,
				   Snapshot snapshot,
				   unsigned nkeys, ScanKey key)
{
	SysScanDesc sysscan;

	sysscan = (SysScanDesc) palloc(sizeof(SysScanDescData));
	sysscan->heap_rel = rel;
	sysscan->snapshot = snapshot;
	sysscan->tuple.t_datamcxt = NULL;
	sysscan->tuple.t_data = NULL;
	sysscan->buffer = InvalidBuffer;

	if (indexOK &&
		rel->rd_rel->relhasindex &&
		!IsIgnoringSystemIndexes())
	{
		Relation	irel;
		unsigned	i;

		/* We assume it's a system index, so index_openr is OK */
		sysscan->irel = irel = index_openr(indexRelname);
		/*
		 * Change attribute numbers to be index column numbers.
		 *
		 * This code could be generalized to search for the index key numbers
		 * to substitute, but for now there's no need.
		 */
		for (i = 0; i < nkeys; i++)
		{
			Assert(key[i].sk_attno == irel->rd_index->indkey[i]);
			key[i].sk_attno = i+1;
		}
		sysscan->iscan = index_beginscan(irel, false, nkeys, key);
		sysscan->scan = NULL;
	}
	else
	{
		sysscan->irel = (Relation) NULL;
		sysscan->scan = heap_beginscan(rel, false, snapshot, nkeys, key);
		sysscan->iscan = NULL;
	}

	return sysscan;
}

/*
 * systable_getnext --- get next tuple in a heap-or-index scan
 *
 * Returns NULL if no more tuples available.
 *
 * Note that returned tuple is a reference to data in a disk buffer;
 * it must not be modified, and should be presumed inaccessible after
 * next getnext() or endscan() call.
 */
HeapTuple
systable_getnext(SysScanDesc sysscan)
{
	HeapTuple	htup = (HeapTuple) NULL;

	if (sysscan->irel)
	{
		RetrieveIndexResult indexRes;

		if (BufferIsValid(sysscan->buffer))
		{
			ReleaseBuffer(sysscan->buffer);
			sysscan->buffer = InvalidBuffer;
		}

		while ((indexRes = index_getnext(sysscan->iscan, ForwardScanDirection)) != NULL)
		{
			sysscan->tuple.t_self = indexRes->heap_iptr;
			pfree(indexRes);
			heap_fetch(sysscan->heap_rel, sysscan->snapshot,
					   &sysscan->tuple, &sysscan->buffer,
					   sysscan->iscan);
			if (sysscan->tuple.t_data != NULL)
			{
				htup = &sysscan->tuple;
				break;
			}
		}
	}
	else
		htup = heap_getnext(sysscan->scan, 0);

	return htup;
}

/*
 * systable_endscan --- close scan, release resources
 *
 * Note that it's still up to the caller to close the heap relation.
 */
void
systable_endscan(SysScanDesc sysscan)
{
	if (sysscan->irel)
	{
		if (BufferIsValid(sysscan->buffer))
			ReleaseBuffer(sysscan->buffer);
		index_endscan(sysscan->iscan);
		index_close(sysscan->irel);
	}
	else
		heap_endscan(sysscan->scan);

	pfree(sysscan);
}
