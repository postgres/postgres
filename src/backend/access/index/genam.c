/*-------------------------------------------------------------------------
 *
 * genam.c
 *	  general index access method routines
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
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

#include "access/genam.h"
#include "access/heapam.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "catalog/index.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


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
 *		routine does that.  (We can't do it in the AM because index_endscan
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
	scan->xs_heapfetch = NULL;
	scan->indexRelation = indexRelation;
	scan->xs_snapshot = InvalidSnapshot;	/* caller must initialize this */
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
	scan->xs_hitup = NULL;
	scan->xs_hitupdesc = NULL;

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
 * for building unique-constraint and exclusion-constraint error messages,
 * so only key columns of the index are checked and printed.
 *
 * Note that if the user does not have permissions to view all of the
 * columns involved then a NULL is returned.  Returning a partial key seems
 * unlikely to be useful and we have no way to know which of the columns the
 * user provided (unlike in ExecBuildSlotValueDescription).
 *
 * The passed-in values/nulls arrays are the "raw" input to the index AM,
 * e.g. results of FormIndexDatum --- this is not necessarily what is stored
 * in the index, but it's what the user perceives to be stored.
 *
 * Note: if you change anything here, check whether
 * ExecBuildSlotPartitionKeyDescription() in execMain.c needs a similar
 * change.
 */
char *
BuildIndexValueDescription(Relation indexRelation,
						   Datum *values, bool *isnull)
{
	StringInfoData buf;
	Form_pg_index idxrec;
	int			indnkeyatts;
	int			i;
	int			keyno;
	Oid			indexrelid = RelationGetRelid(indexRelation);
	Oid			indrelid;
	AclResult	aclresult;

	indnkeyatts = IndexRelationGetNumberOfKeyAttributes(indexRelation);

	/*
	 * Check permissions- if the user does not have access to view all of the
	 * key columns then return NULL to avoid leaking data.
	 *
	 * First check if RLS is enabled for the relation.  If so, return NULL to
	 * avoid leaking data.
	 *
	 * Next we need to check table-level SELECT access and then, if there is
	 * no access there, check column-level permissions.
	 */
	idxrec = indexRelation->rd_index;
	indrelid = idxrec->indrelid;
	Assert(indexrelid == idxrec->indexrelid);

	/* RLS check- if RLS is enabled then we don't return anything. */
	if (check_enable_rls(indrelid, InvalidOid, true) == RLS_ENABLED)
		return NULL;

	/* Table-level SELECT is enough, if the user has it */
	aclresult = pg_class_aclcheck(indrelid, GetUserId(), ACL_SELECT);
	if (aclresult != ACLCHECK_OK)
	{
		/*
		 * No table-level access, so step through the columns in the index and
		 * make sure the user has SELECT rights on all of them.
		 */
		for (keyno = 0; keyno < indnkeyatts; keyno++)
		{
			AttrNumber	attnum = idxrec->indkey.values[keyno];

			/*
			 * Note that if attnum == InvalidAttrNumber, then this is an index
			 * based on an expression and we return no detail rather than try
			 * to figure out what column(s) the expression includes and if the
			 * user has SELECT rights on them.
			 */
			if (attnum == InvalidAttrNumber ||
				pg_attribute_aclcheck(indrelid, attnum, GetUserId(),
									  ACL_SELECT) != ACLCHECK_OK)
			{
				/* No access, so clean up and return */
				return NULL;
			}
		}
	}

	initStringInfo(&buf);
	appendStringInfo(&buf, "(%s)=(",
					 pg_get_indexdef_columns(indexrelid, true));

	for (i = 0; i < indnkeyatts; i++)
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
			 * input types such as ANYARRAY or RECORD.  Currently, the
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

/*
 * Get the latestRemovedXid from the table entries pointed at by the index
 * tuples being deleted using an AM-generic approach.
 *
 * This is a table_index_delete_tuples() shim used by index AMs that have
 * simple requirements.  These callers only need to consult the tableam to get
 * a latestRemovedXid value, and only expect to delete tuples that are already
 * known deletable.  When a latestRemovedXid value isn't needed in index AM's
 * deletion WAL record, it is safe for it to skip calling here entirely.
 *
 * We assume that caller index AM uses the standard IndexTuple representation,
 * with table TIDs stored in the t_tid field.  We also expect (and assert)
 * that the line pointers on page for 'itemnos' offsets are already marked
 * LP_DEAD.
 */
TransactionId
index_compute_xid_horizon_for_tuples(Relation irel,
									 Relation hrel,
									 Buffer ibuf,
									 OffsetNumber *itemnos,
									 int nitems)
{
	TM_IndexDeleteOp delstate;
	TransactionId latestRemovedXid = InvalidTransactionId;
	Page		ipage = BufferGetPage(ibuf);
	IndexTuple	itup;

	Assert(nitems > 0);

	delstate.bottomup = false;
	delstate.bottomupfreespace = 0;
	delstate.ndeltids = 0;
	delstate.deltids = palloc(nitems * sizeof(TM_IndexDelete));
	delstate.status = palloc(nitems * sizeof(TM_IndexStatus));

	/* identify what the index tuples about to be deleted point to */
	for (int i = 0; i < nitems; i++)
	{
		ItemId		iitemid;

		iitemid = PageGetItemId(ipage, itemnos[i]);
		itup = (IndexTuple) PageGetItem(ipage, iitemid);

		Assert(ItemIdIsDead(iitemid));

		ItemPointerCopy(&itup->t_tid, &delstate.deltids[i].tid);
		delstate.deltids[i].id = delstate.ndeltids;
		delstate.status[i].idxoffnum = InvalidOffsetNumber; /* unused */
		delstate.status[i].knowndeletable = true;	/* LP_DEAD-marked */
		delstate.status[i].promising = false;	/* unused */
		delstate.status[i].freespace = 0;	/* unused */

		delstate.ndeltids++;
	}

	/* determine the actual xid horizon */
	latestRemovedXid = table_index_delete_tuples(hrel, &delstate);

	/* assert tableam agrees that all items are deletable */
	Assert(delstate.ndeltids == nitems);

	pfree(delstate.deltids);
	pfree(delstate.status);

	return latestRemovedXid;
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
 *	snapshot: time qual to use (NULL for a recent catalog snapshot)
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
	sysscan->slot = table_slot_create(heapRelation, NULL);

	if (snapshot == NULL)
	{
		Oid			relid = RelationGetRelid(heapRelation);

		snapshot = RegisterSnapshot(GetCatalogSnapshot(relid));
		sysscan->snapshot = snapshot;
	}
	else
	{
		/* Caller is responsible for any snapshot. */
		sysscan->snapshot = NULL;
	}

	if (irel)
	{
		int			i;

		/* Change attribute numbers to be index column numbers. */
		for (i = 0; i < nkeys; i++)
		{
			int			j;

			for (j = 0; j < IndexRelationGetNumberOfAttributes(irel); j++)
			{
				if (key[i].sk_attno == irel->rd_index->indkey.values[j])
				{
					key[i].sk_attno = j + 1;
					break;
				}
			}
			if (j == IndexRelationGetNumberOfAttributes(irel))
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
		sysscan->scan = table_beginscan_strat(heapRelation, snapshot,
											  nkeys, key,
											  true, false);
		sysscan->iscan = NULL;
	}

	/*
	 * If CheckXidAlive is set then set a flag to indicate that system table
	 * scan is in-progress.  See detailed comments in xact.c where these
	 * variables are declared.
	 */
	if (TransactionIdIsValid(CheckXidAlive))
		bsysscan = true;

	return sysscan;
}

/*
 * HandleConcurrentAbort - Handle concurrent abort of the CheckXidAlive.
 *
 * Error out, if CheckXidAlive is aborted. We can't directly use
 * TransactionIdDidAbort as after crash such transaction might not have been
 * marked as aborted.  See detailed comments in xact.c where the variable
 * is declared.
 */
static inline void
HandleConcurrentAbort()
{
	if (TransactionIdIsValid(CheckXidAlive) &&
		!TransactionIdIsInProgress(CheckXidAlive) &&
		!TransactionIdDidCommit(CheckXidAlive))
		ereport(ERROR,
				(errcode(ERRCODE_TRANSACTION_ROLLBACK),
				 errmsg("transaction aborted during system catalog scan")));
}

/*
 * systable_getnext --- get next tuple in a heap-or-index scan
 *
 * Returns NULL if no more tuples available.
 *
 * Note that returned tuple is a reference to data in a disk buffer;
 * it must not be modified, and should be presumed inaccessible after
 * next getnext() or endscan() call.
 *
 * XXX: It'd probably make sense to offer a slot based interface, at least
 * optionally.
 */
HeapTuple
systable_getnext(SysScanDesc sysscan)
{
	HeapTuple	htup = NULL;

	if (sysscan->irel)
	{
		if (index_getnext_slot(sysscan->iscan, ForwardScanDirection, sysscan->slot))
		{
			bool		shouldFree;

			htup = ExecFetchSlotHeapTuple(sysscan->slot, false, &shouldFree);
			Assert(!shouldFree);

			/*
			 * We currently don't need to support lossy index operators for
			 * any system catalog scan.  It could be done here, using the scan
			 * keys to drive the operator calls, if we arranged to save the
			 * heap attnums during systable_beginscan(); this is practical
			 * because we still wouldn't need to support indexes on
			 * expressions.
			 */
			if (sysscan->iscan->xs_recheck)
				elog(ERROR, "system catalog scans with lossy index conditions are not implemented");
		}
	}
	else
	{
		if (table_scan_getnextslot(sysscan->scan, ForwardScanDirection, sysscan->slot))
		{
			bool		shouldFree;

			htup = ExecFetchSlotHeapTuple(sysscan->slot, false, &shouldFree);
			Assert(!shouldFree);
		}
	}

	/*
	 * Handle the concurrent abort while fetching the catalog tuple during
	 * logical streaming of a transaction.
	 */
	HandleConcurrentAbort();

	return htup;
}

/*
 * systable_recheck_tuple --- recheck visibility of most-recently-fetched tuple
 *
 * In particular, determine if this tuple would be visible to a catalog scan
 * that started now.  We don't handle the case of a non-MVCC scan snapshot,
 * because no caller needs that yet.
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
	Snapshot	freshsnap;
	bool		result;

	Assert(tup == ExecFetchSlotHeapTuple(sysscan->slot, false, NULL));

	/*
	 * Trust that table_tuple_satisfies_snapshot() and its subsidiaries
	 * (commonly LockBuffer() and HeapTupleSatisfiesMVCC()) do not themselves
	 * acquire snapshots, so we need not register the snapshot.  Those
	 * facilities are too low-level to have any business scanning tables.
	 */
	freshsnap = GetCatalogSnapshot(RelationGetRelid(sysscan->heap_rel));

	result = table_tuple_satisfies_snapshot(sysscan->heap_rel,
											sysscan->slot,
											freshsnap);

	/*
	 * Handle the concurrent abort while fetching the catalog tuple during
	 * logical streaming of a transaction.
	 */
	HandleConcurrentAbort();

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
	if (sysscan->slot)
	{
		ExecDropSingleTupleTableSlot(sysscan->slot);
		sysscan->slot = NULL;
	}

	if (sysscan->irel)
	{
		index_endscan(sysscan->iscan);
		index_close(sysscan->irel, AccessShareLock);
	}
	else
		table_endscan(sysscan->scan);

	if (sysscan->snapshot)
		UnregisterSnapshot(sysscan->snapshot);

	/*
	 * Reset the bsysscan flag at the end of the systable scan.  See
	 * detailed comments in xact.c where these variables are declared.
	 */
	if (TransactionIdIsValid(CheckXidAlive))
		bsysscan = false;

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
 * Currently we do not support non-index-based scans here.  (In principle
 * we could do a heapscan and sort, but the uses are in places that
 * probably don't need to still work with corrupted catalog indexes.)
 * For the moment, therefore, these functions are merely the thinnest of
 * wrappers around index_beginscan/index_getnext_slot.  The main reason for
 * their existence is to centralize possible future support of lossy operators
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
	sysscan->slot = table_slot_create(heapRelation, NULL);

	if (snapshot == NULL)
	{
		Oid			relid = RelationGetRelid(heapRelation);

		snapshot = RegisterSnapshot(GetCatalogSnapshot(relid));
		sysscan->snapshot = snapshot;
	}
	else
	{
		/* Caller is responsible for any snapshot. */
		sysscan->snapshot = NULL;
	}

	/* Change attribute numbers to be index column numbers. */
	for (i = 0; i < nkeys; i++)
	{
		int			j;

		for (j = 0; j < IndexRelationGetNumberOfAttributes(indexRelation); j++)
		{
			if (key[i].sk_attno == indexRelation->rd_index->indkey.values[j])
			{
				key[i].sk_attno = j + 1;
				break;
			}
		}
		if (j == IndexRelationGetNumberOfAttributes(indexRelation))
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
	HeapTuple	htup = NULL;

	Assert(sysscan->irel);
	if (index_getnext_slot(sysscan->iscan, direction, sysscan->slot))
		htup = ExecFetchSlotHeapTuple(sysscan->slot, false, NULL);

	/* See notes in systable_getnext */
	if (htup && sysscan->iscan->xs_recheck)
		elog(ERROR, "system catalog scans with lossy index conditions are not implemented");

	/*
	 * Handle the concurrent abort while fetching the catalog tuple during
	 * logical streaming of a transaction.
	 */
	HandleConcurrentAbort();

	return htup;
}

/*
 * systable_endscan_ordered --- close scan, release resources
 */
void
systable_endscan_ordered(SysScanDesc sysscan)
{
	if (sysscan->slot)
	{
		ExecDropSingleTupleTableSlot(sysscan->slot);
		sysscan->slot = NULL;
	}

	Assert(sysscan->irel);
	index_endscan(sysscan->iscan);
	if (sysscan->snapshot)
		UnregisterSnapshot(sysscan->snapshot);
	pfree(sysscan);
}
