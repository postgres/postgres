/*-------------------------------------------------------------------------
 *
 * genam.c
 *	  general index access method routines
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/index/genam.c,v 1.41 2003/09/24 18:54:01 tgl Exp $
 *
 * NOTES
 *	  many of the old access method routines have been turned into
 *	  macros and moved to genam.h -cim 4/30/91
 *
 *-------------------------------------------------------------------------
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
 *				indexRelation -- index relation for scan.
 *				nkeys -- count of scan keys.
 *				key -- array of scan keys to restrict the index scan.
 *
 *		Returns:
 *				An initialized IndexScanDesc.
 * ----------------
 */
IndexScanDesc
RelationGetIndexScan(Relation indexRelation,
					 int nkeys, ScanKey key)
{
	IndexScanDesc scan;

	scan = (IndexScanDesc) palloc(sizeof(IndexScanDescData));

	scan->heapRelation = NULL;	/* may be set later */
	scan->indexRelation = indexRelation;
	scan->xs_snapshot = SnapshotNow;	/* may be set later */
	scan->numberOfKeys = nkeys;

	/*
	 * We allocate the key space here, but the AM is responsible for
	 * actually filling it from the passed key array.
	 */
	if (nkeys > 0)
		scan->keyData = (ScanKey) palloc(sizeof(ScanKeyData) * nkeys);
	else
		scan->keyData = NULL;

	scan->kill_prior_tuple = false;
	scan->ignore_killed_tuples = true;	/* default setting */
	scan->keys_are_unique = false;		/* may be set by index AM */
	scan->got_tuple = false;

	scan->opaque = NULL;

	ItemPointerSetInvalid(&scan->currentItemData);
	ItemPointerSetInvalid(&scan->currentMarkData);

	ItemPointerSetInvalid(&scan->xs_ctup.t_self);
	scan->xs_ctup.t_datamcxt = NULL;
	scan->xs_ctup.t_data = NULL;
	scan->xs_cbuf = InvalidBuffer;

	/* mark cached function lookup data invalid; it will be set later */
	scan->fn_getnext.fn_oid = InvalidOid;

	scan->unique_tuple_pos = 0;
	scan->unique_tuple_mark = 0;

	pgstat_initstats(&scan->xs_pgstat_info, indexRelation);

	/*
	 * Let the AM fill in the key and any opaque data it wants.
	 */
	index_rescan(scan, key);

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
	if (scan->keyData != NULL)
		pfree(scan->keyData);

	pfree(scan);
}


/* ----------------------------------------------------------------
 *		heap-or-index-scan access to system catalogs
 *
 *		These functions support system catalog accesses that normally use
 *		an index but need to be capable of being switched to heap scans
 *		if the system indexes are unavailable.
 *
 *		The specified scan keys must be compatible with the named index.
 *		Generally this means that they must constrain either all columns
 *		of the index, or the first K columns of an N-column index.
 *
 *		These routines could work with non-system tables, actually,
 *		but they're only useful when there is a known index to use with
 *		the given scan keys; so in practice they're only good for
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
systable_beginscan(Relation heapRelation,
				   const char *indexRelname,
				   bool indexOK,
				   Snapshot snapshot,
				   int nkeys, ScanKey key)
{
	SysScanDesc sysscan;
	Relation	irel;

	if (indexOK && !IsIgnoringSystemIndexes())
	{
		/* We assume it's a system index, so index_openr is OK */
		irel = index_openr(indexRelname);

		if (ReindexIsProcessingIndex(RelationGetRelid(irel)))
		{
			/* oops, can't use index that's being rebuilt */
			index_close(irel);
			irel = NULL;
		}
	}
	else
		irel = NULL;

	sysscan = (SysScanDesc) palloc(sizeof(SysScanDescData));

	sysscan->heap_rel = heapRelation;
	sysscan->irel = irel;

	if (irel)
	{
		int			i;

		/*
		 * Change attribute numbers to be index column numbers.
		 *
		 * This code could be generalized to search for the index key numbers
		 * to substitute, but for now there's no need.
		 */
		for (i = 0; i < nkeys; i++)
		{
			Assert(key[i].sk_attno == irel->rd_index->indkey[i]);
			key[i].sk_attno = i + 1;
		}

		sysscan->iscan = index_beginscan(heapRelation, irel, snapshot,
										 nkeys, key);
		sysscan->scan = NULL;
	}
	else
	{
		sysscan->scan = heap_beginscan(heapRelation, snapshot, nkeys, key);
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
	HeapTuple	htup;

	if (sysscan->irel)
		htup = index_getnext(sysscan->iscan, ForwardScanDirection);
	else
		htup = heap_getnext(sysscan->scan, ForwardScanDirection);

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
		index_endscan(sysscan->iscan);
		index_close(sysscan->irel);
	}
	else
		heap_endscan(sysscan->scan);

	pfree(sysscan);
}
