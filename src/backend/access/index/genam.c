/*-------------------------------------------------------------------------
 *
 * genam.c
 *	  general index access method routines
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/index/genam.c
 *
 * NOTES
 *	  many of the old access method routines have been turned into
 *	  macros and moved to genam.h -cim 4/30/91
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/relscan.h"
#include "access/transam.h"
#include "catalog/index.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/tqual.h"


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
 *		This routine creates an index scan structure and sets up initial
 *		contents for it.
 *
 *		Parameters:
 *				indexRelation -- index relation for scan.
 *				nkeys -- count of scan keys (index qual conditions).
 *				norderbys -- count of index order-by operators.
 *
 *		Returns:
 *				An initialized IndexScanDesc.
 * ----------------
 */
IndexScanDesc
RelationGetIndexScan(Relation indexRelation, int nkeys, int norderbys)
{
	IndexScanDesc scan;

	scan = (IndexScanDesc) palloc(sizeof(IndexScanDescData));

	scan->heapRelation = NULL;	/* may be set later */
	scan->indexRelation = indexRelation;
	scan->xs_snapshot = SnapshotNow;	/* may be set later */
	scan->numberOfKeys = nkeys;
	scan->numberOfOrderBys = norderbys;

	/*
	 * We allocate key workspace here, but it won't get filled until amrescan.
	 */
	if (nkeys > 0)
		scan->keyData = (ScanKey) palloc(sizeof(ScanKeyData) * nkeys);
	else
		scan->keyData = NULL;
	if (norderbys > 0)
		scan->orderByData = (ScanKey) palloc(sizeof(ScanKeyData) * norderbys);
	else
		scan->orderByData = NULL;

	scan->xs_want_itup = false; /* may be set later */

	/*
	 * During recovery we ignore killed tuples and don't bother to kill them
	 * either. We do this because the xmin on the primary node could easily be
	 * later than the xmin on the standby node, so that what the primary
	 * thinks is killed is supposed to be visible on standby. So for correct
	 * MVCC for queries during recovery we must ignore these hints and check
	 * all tuples. Do *not* set ignore_killed_tuples to true when running in a
	 * transaction that was started during recovery. xactStartedInRecovery
	 * should not be altered by index AMs.
	 */
	scan->kill_prior_tuple = false;
	scan->xactStartedInRecovery = TransactionStartedDuringRecovery();
	scan->ignore_killed_tuples = !scan->xactStartedInRecovery;

	scan->opaque = NULL;

	scan->xs_itup = NULL;
	scan->xs_itupdesc = NULL;

	ItemPointerSetInvalid(&scan->xs_ctup.t_self);
	scan->xs_ctup.t_data = NULL;
	scan->xs_cbuf = InvalidBuffer;
	scan->xs_continue_hot = false;

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
	if (scan->orderByData != NULL)
		pfree(scan->orderByData);

	pfree(scan);
}

/*
 * BuildIndexValueDescription
 *
 * Construct a string describing the contents of an index entry, in the
 * form "(key_name, ...)=(key_value, ...)".  This is currently used
 * for building unique-constraint and exclusion-constraint error messages.
 *
 * The passed-in values/nulls arrays are the "raw" input to the index AM,
 * e.g. results of FormIndexDatum --- this is not necessarily what is stored
 * in the index, but it's what the user perceives to be stored.
 */
char *
BuildIndexValueDescription(Relation indexRelation,
						   Datum *values, bool *isnull)
{
	StringInfoData buf;
	int			natts = indexRelation->rd_rel->relnatts;
	int			i;

	initStringInfo(&buf);
	appendStringInfo(&buf, "(%s)=(",
					 pg_get_indexdef_columns(RelationGetRelid(indexRelation),
											 true));

	for (i = 0; i < natts; i++)
	{
		char	   *val;

		if (isnull[i])
			val = "null";
		else
		{
			Oid			foutoid;
			bool		typisvarlena;

			/*
			 * The provided data is not necessarily of the type stored in the
			 * index; rather it is of the index opclass's input type. So look
			 * at rd_opcintype not the index tupdesc.
			 *
			 * Note: this is a bit shaky for opclasses that have pseudotype
			 * input types such as ANYARRAY or RECORD.	Currently, the
			 * typoutput functions associated with the pseudotypes will work
			 * okay, but we might have to try harder in future.
			 */
			getTypeOutputInfo(indexRelation->rd_opcintype[i],
							  &foutoid, &typisvarlena);
			val = OidOutputFunctionCall(foutoid, values[i]);
		}

		if (i > 0)
			appendStringInfoString(&buf, ", ");
		appendStringInfoString(&buf, val);
	}

	appendStringInfoChar(&buf, ')');

	return buf.data;
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
 *	indexId: OID of index to conditionally use
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
				   Oid indexId,
				   bool indexOK,
				   Snapshot snapshot,
				   int nkeys, ScanKey key)
{
	SysScanDesc sysscan;
	Relation	irel;

	if (indexOK &&
		!IgnoreSystemIndexes &&
		!ReindexIsProcessingIndex(indexId))
		irel = index_open(indexId, AccessShareLock);
	else
		irel = NULL;

	sysscan = (SysScanDesc) palloc(sizeof(SysScanDescData));

	sysscan->heap_rel = heapRelation;
	sysscan->irel = irel;

	if (irel)
	{
		int			i;

		/* Change attribute numbers to be index column numbers. */
		for (i = 0; i < nkeys; i++)
		{
			int			j;

			for (j = 0; j < irel->rd_index->indnatts; j++)
			{
				if (key[i].sk_attno == irel->rd_index->indkey.values[j])
				{
					key[i].sk_attno = j + 1;
					break;
				}
			}
			if (j == irel->rd_index->indnatts)
				elog(ERROR, "column is not in index");
		}

		sysscan->iscan = index_beginscan(heapRelation, irel,
										 snapshot, nkeys, 0);
		index_rescan(sysscan->iscan, key, nkeys, NULL, 0);
		sysscan->scan = NULL;
	}
	else
	{
		/*
		 * We disallow synchronized scans when forced to use a heapscan on a
		 * catalog.  In most cases the desired rows are near the front, so
		 * that the unpredictable start point of a syncscan is a serious
		 * disadvantage; and there are no compensating advantages, because
		 * it's unlikely that such scans will occur in parallel.
		 */
		sysscan->scan = heap_beginscan_strat(heapRelation, snapshot,
											 nkeys, key,
											 true, false);
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
	{
		htup = index_getnext(sysscan->iscan, ForwardScanDirection);

		/*
		 * We currently don't need to support lossy index operators for any
		 * system catalog scan.  It could be done here, using the scan keys to
		 * drive the operator calls, if we arranged to save the heap attnums
		 * during systable_beginscan(); this is practical because we still
		 * wouldn't need to support indexes on expressions.
		 */
		if (htup && sysscan->iscan->xs_recheck)
			elog(ERROR, "system catalog scans with lossy index conditions are not implemented");
	}
	else
		htup = heap_getnext(sysscan->scan, ForwardScanDirection);

	return htup;
}

/*
 * systable_recheck_tuple --- recheck visibility of most-recently-fetched tuple
 *
 * This is useful to test whether an object was deleted while we waited to
 * acquire lock on it.
 *
 * Note: we don't actually *need* the tuple to be passed in, but it's a
 * good crosscheck that the caller is interested in the right tuple.
 */
bool
systable_recheck_tuple(SysScanDesc sysscan, HeapTuple tup)
{
	bool		result;

	if (sysscan->irel)
	{
		IndexScanDesc scan = sysscan->iscan;

		Assert(tup == &scan->xs_ctup);
		Assert(BufferIsValid(scan->xs_cbuf));
		/* must hold a buffer lock to call HeapTupleSatisfiesVisibility */
		LockBuffer(scan->xs_cbuf, BUFFER_LOCK_SHARE);
		result = HeapTupleSatisfiesVisibility(tup, scan->xs_snapshot,
											  scan->xs_cbuf);
		LockBuffer(scan->xs_cbuf, BUFFER_LOCK_UNLOCK);
	}
	else
	{
		HeapScanDesc scan = sysscan->scan;

		Assert(tup == &scan->rs_ctup);
		Assert(BufferIsValid(scan->rs_cbuf));
		/* must hold a buffer lock to call HeapTupleSatisfiesVisibility */
		LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);
		result = HeapTupleSatisfiesVisibility(tup, scan->rs_snapshot,
											  scan->rs_cbuf);
		LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);
	}
	return result;
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
		index_close(sysscan->irel, AccessShareLock);
	}
	else
		heap_endscan(sysscan->scan);

	pfree(sysscan);
}


/*
 * systable_beginscan_ordered --- set up for ordered catalog scan
 *
 * These routines have essentially the same API as systable_beginscan etc,
 * except that they guarantee to return multiple matching tuples in
 * index order.  Also, for largely historical reasons, the index to use
 * is opened and locked by the caller, not here.
 *
 * Currently we do not support non-index-based scans here.	(In principle
 * we could do a heapscan and sort, but the uses are in places that
 * probably don't need to still work with corrupted catalog indexes.)
 * For the moment, therefore, these functions are merely the thinnest of
 * wrappers around index_beginscan/index_getnext.  The main reason for their
 * existence is to centralize possible future support of lossy operators
 * in catalog scans.
 */
SysScanDesc
systable_beginscan_ordered(Relation heapRelation,
						   Relation indexRelation,
						   Snapshot snapshot,
						   int nkeys, ScanKey key)
{
	SysScanDesc sysscan;
	int			i;

	/* REINDEX can probably be a hard error here ... */
	if (ReindexIsProcessingIndex(RelationGetRelid(indexRelation)))
		elog(ERROR, "cannot do ordered scan on index \"%s\", because it is being reindexed",
			 RelationGetRelationName(indexRelation));
	/* ... but we only throw a warning about violating IgnoreSystemIndexes */
	if (IgnoreSystemIndexes)
		elog(WARNING, "using index \"%s\" despite IgnoreSystemIndexes",
			 RelationGetRelationName(indexRelation));

	sysscan = (SysScanDesc) palloc(sizeof(SysScanDescData));

	sysscan->heap_rel = heapRelation;
	sysscan->irel = indexRelation;

	/* Change attribute numbers to be index column numbers. */
	for (i = 0; i < nkeys; i++)
	{
		int			j;

		for (j = 0; j < indexRelation->rd_index->indnatts; j++)
		{
			if (key[i].sk_attno == indexRelation->rd_index->indkey.values[j])
			{
				key[i].sk_attno = j + 1;
				break;
			}
		}
		if (j == indexRelation->rd_index->indnatts)
			elog(ERROR, "column is not in index");
	}

	sysscan->iscan = index_beginscan(heapRelation, indexRelation,
									 snapshot, nkeys, 0);
	index_rescan(sysscan->iscan, key, nkeys, NULL, 0);
	sysscan->scan = NULL;

	return sysscan;
}

/*
 * systable_getnext_ordered --- get next tuple in an ordered catalog scan
 */
HeapTuple
systable_getnext_ordered(SysScanDesc sysscan, ScanDirection direction)
{
	HeapTuple	htup;

	Assert(sysscan->irel);
	htup = index_getnext(sysscan->iscan, direction);
	/* See notes in systable_getnext */
	if (htup && sysscan->iscan->xs_recheck)
		elog(ERROR, "system catalog scans with lossy index conditions are not implemented");

	return htup;
}

/*
 * systable_endscan_ordered --- close scan, release resources
 */
void
systable_endscan_ordered(SysScanDesc sysscan)
{
	Assert(sysscan->irel);
	index_endscan(sysscan->iscan);
	pfree(sysscan);
}
