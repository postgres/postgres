/*-------------------------------------------------------------------------
 *
 * indexam.c
 *	  general index access method routines
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/index/indexam.c
 *
 * INTERFACE ROUTINES
 *		index_open		- open an index relation by relation OID
 *		index_close		- close an index relation
 *		index_beginscan - start a scan of an index with amgettuple
 *		index_beginscan_bitmap - start a scan of an index with amgetbitmap
 *		index_rescan	- restart a scan of an index
 *		index_endscan	- end a scan
 *		index_insert	- insert an index tuple into a relation
 *		index_markpos	- mark a scan position
 *		index_restrpos	- restore a scan position
 *		index_getnext_tid	- get the next TID from a scan
 *		index_fetch_heap		- get the scan's next heap tuple
 *		index_getnext	- get the next heap tuple from a scan
 *		index_getbitmap - get all tuples from a scan
 *		index_bulk_delete	- bulk deletion of index tuples
 *		index_vacuum_cleanup	- post-deletion cleanup of an index
 *		index_can_return	- does index support index-only scans?
 *		index_getprocid - get a support procedure OID
 *		index_getprocinfo - get a support procedure's lookup info
 *
 * NOTES
 *		This file contains the index_ routines which used
 *		to be a scattered collection of stuff in access/genam.
 *
 *
 * old comments
 *		Scans are implemented as follows:
 *
 *		`0' represents an invalid item pointer.
 *		`-' represents an unknown item pointer.
 *		`X' represents a known item pointers.
 *		`+' represents known or invalid item pointers.
 *		`*' represents any item pointers.
 *
 *		State is represented by a triple of these symbols in the order of
 *		previous, current, next.  Note that the case of reverse scans works
 *		identically.
 *
 *				State	Result
 *		(1)		+ + -	+ 0 0			(if the next item pointer is invalid)
 *		(2)				+ X -			(otherwise)
 *		(3)		* 0 0	* 0 0			(no change)
 *		(4)		+ X 0	X 0 0			(shift)
 *		(5)		* + X	+ X -			(shift, add unknown)
 *
 *		All other states cannot occur.
 *
 *		Note: It would be possible to cache the status of the previous and
 *			  next item pointer using the flags.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/relscan.h"
#include "access/transam.h"
#include "access/xlog.h"

#include "catalog/index.h"
#include "catalog/catalog.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "utils/snapmgr.h"
#include "utils/tqual.h"


/* ----------------------------------------------------------------
 *					macros used in index_ routines
 *
 * Note: the ReindexIsProcessingIndex() check in RELATION_CHECKS is there
 * to check that we don't try to scan or do retail insertions into an index
 * that is currently being rebuilt or pending rebuild.  This helps to catch
 * things that don't work when reindexing system catalogs.  The assertion
 * doesn't prevent the actual rebuild because we don't use RELATION_CHECKS
 * when calling the index AM's ambuild routine, and there is no reason for
 * ambuild to call its subsidiary routines through this file.
 * ----------------------------------------------------------------
 */
#define RELATION_CHECKS \
( \
	AssertMacro(RelationIsValid(indexRelation)), \
	AssertMacro(PointerIsValid(indexRelation->rd_am)), \
	AssertMacro(!ReindexIsProcessingIndex(RelationGetRelid(indexRelation))) \
)

#define SCAN_CHECKS \
( \
	AssertMacro(IndexScanIsValid(scan)), \
	AssertMacro(RelationIsValid(scan->indexRelation)), \
	AssertMacro(PointerIsValid(scan->indexRelation->rd_am)) \
)

#define GET_REL_PROCEDURE(pname) \
do { \
	procedure = &indexRelation->rd_aminfo->pname; \
	if (!OidIsValid(procedure->fn_oid)) \
	{ \
		RegProcedure	procOid = indexRelation->rd_am->pname; \
		if (!RegProcedureIsValid(procOid)) \
			elog(ERROR, "invalid %s regproc", CppAsString(pname)); \
		fmgr_info_cxt(procOid, procedure, indexRelation->rd_indexcxt); \
	} \
} while(0)

#define GET_UNCACHED_REL_PROCEDURE(pname) \
do { \
	if (!RegProcedureIsValid(indexRelation->rd_am->pname)) \
		elog(ERROR, "invalid %s regproc", CppAsString(pname)); \
	fmgr_info(indexRelation->rd_am->pname, &procedure); \
} while(0)

#define GET_SCAN_PROCEDURE(pname) \
do { \
	procedure = &scan->indexRelation->rd_aminfo->pname; \
	if (!OidIsValid(procedure->fn_oid)) \
	{ \
		RegProcedure	procOid = scan->indexRelation->rd_am->pname; \
		if (!RegProcedureIsValid(procOid)) \
			elog(ERROR, "invalid %s regproc", CppAsString(pname)); \
		fmgr_info_cxt(procOid, procedure, scan->indexRelation->rd_indexcxt); \
	} \
} while(0)

static IndexScanDesc index_beginscan_internal(Relation indexRelation,
						 int nkeys, int norderbys, Snapshot snapshot);


/* ----------------------------------------------------------------
 *				   index_ interface functions
 * ----------------------------------------------------------------
 */

/* ----------------
 *		index_open - open an index relation by relation OID
 *
 *		If lockmode is not "NoLock", the specified kind of lock is
 *		obtained on the index.  (Generally, NoLock should only be
 *		used if the caller knows it has some appropriate lock on the
 *		index already.)
 *
 *		An error is raised if the index does not exist.
 *
 *		This is a convenience routine adapted for indexscan use.
 *		Some callers may prefer to use relation_open directly.
 * ----------------
 */
Relation
index_open(Oid relationId, LOCKMODE lockmode)
{
	Relation	r;

	r = relation_open(relationId, lockmode);

	if (r->rd_rel->relkind != RELKIND_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index",
						RelationGetRelationName(r))));

	return r;
}

/* ----------------
 *		index_close - close an index relation
 *
 *		If lockmode is not "NoLock", we then release the specified lock.
 *
 *		Note that it is often sensible to hold a lock beyond index_close;
 *		in that case, the lock is released automatically at xact end.
 * ----------------
 */
void
index_close(Relation relation, LOCKMODE lockmode)
{
	LockRelId	relid = relation->rd_lockInfo.lockRelId;

	Assert(lockmode >= NoLock && lockmode < MAX_LOCKMODES);

	/* The relcache does the real work... */
	RelationClose(relation);

	if (lockmode != NoLock)
		UnlockRelationId(&relid, lockmode);
}

/* ----------------
 *		index_insert - insert an index tuple into a relation
 * ----------------
 */
bool
index_insert(Relation indexRelation,
			 Datum *values,
			 bool *isnull,
			 ItemPointer heap_t_ctid,
			 Relation heapRelation,
			 IndexUniqueCheck checkUnique)
{
	FmgrInfo   *procedure;

	RELATION_CHECKS;
	GET_REL_PROCEDURE(aminsert);

	if (!(indexRelation->rd_am->ampredlocks))
		CheckForSerializableConflictIn(indexRelation,
									   (HeapTuple) NULL,
									   InvalidBuffer);

	/*
	 * have the am's insert proc do all the work.
	 */
	return DatumGetBool(FunctionCall6(procedure,
									  PointerGetDatum(indexRelation),
									  PointerGetDatum(values),
									  PointerGetDatum(isnull),
									  PointerGetDatum(heap_t_ctid),
									  PointerGetDatum(heapRelation),
									  Int32GetDatum((int32) checkUnique)));
}

/*
 * index_beginscan - start a scan of an index with amgettuple
 *
 * Caller must be holding suitable locks on the heap and the index.
 */
IndexScanDesc
index_beginscan(Relation heapRelation,
				Relation indexRelation,
				Snapshot snapshot,
				int nkeys, int norderbys)
{
	IndexScanDesc scan;

	scan = index_beginscan_internal(indexRelation, nkeys, norderbys, snapshot);

	/*
	 * Save additional parameters into the scandesc.  Everything else was set
	 * up by RelationGetIndexScan.
	 */
	scan->heapRelation = heapRelation;
	scan->xs_snapshot = snapshot;

	return scan;
}

/*
 * index_beginscan_bitmap - start a scan of an index with amgetbitmap
 *
 * As above, caller had better be holding some lock on the parent heap
 * relation, even though it's not explicitly mentioned here.
 */
IndexScanDesc
index_beginscan_bitmap(Relation indexRelation,
					   Snapshot snapshot,
					   int nkeys)
{
	IndexScanDesc scan;

	scan = index_beginscan_internal(indexRelation, nkeys, 0, snapshot);

	/*
	 * Save additional parameters into the scandesc.  Everything else was set
	 * up by RelationGetIndexScan.
	 */
	scan->xs_snapshot = snapshot;

	return scan;
}

/*
 * index_beginscan_internal --- common code for index_beginscan variants
 */
static IndexScanDesc
index_beginscan_internal(Relation indexRelation,
						 int nkeys, int norderbys, Snapshot snapshot)
{
	IndexScanDesc scan;
	FmgrInfo   *procedure;

	RELATION_CHECKS;
	GET_REL_PROCEDURE(ambeginscan);

	if (!(indexRelation->rd_am->ampredlocks))
		PredicateLockRelation(indexRelation, snapshot);

	/*
	 * We hold a reference count to the relcache entry throughout the scan.
	 */
	RelationIncrementReferenceCount(indexRelation);

	/*
	 * Tell the AM to open a scan.
	 */
	scan = (IndexScanDesc)
		DatumGetPointer(FunctionCall3(procedure,
									  PointerGetDatum(indexRelation),
									  Int32GetDatum(nkeys),
									  Int32GetDatum(norderbys)));

	return scan;
}

/* ----------------
 *		index_rescan  - (re)start a scan of an index
 *
 * During a restart, the caller may specify a new set of scankeys and/or
 * orderbykeys; but the number of keys cannot differ from what index_beginscan
 * was told.  (Later we might relax that to "must not exceed", but currently
 * the index AMs tend to assume that scan->numberOfKeys is what to believe.)
 * To restart the scan without changing keys, pass NULL for the key arrays.
 * (Of course, keys *must* be passed on the first call, unless
 * scan->numberOfKeys is zero.)
 * ----------------
 */
void
index_rescan(IndexScanDesc scan,
			 ScanKey keys, int nkeys,
			 ScanKey orderbys, int norderbys)
{
	FmgrInfo   *procedure;

	SCAN_CHECKS;
	GET_SCAN_PROCEDURE(amrescan);

	Assert(nkeys == scan->numberOfKeys);
	Assert(norderbys == scan->numberOfOrderBys);

	/* Release any held pin on a heap page */
	if (BufferIsValid(scan->xs_cbuf))
	{
		ReleaseBuffer(scan->xs_cbuf);
		scan->xs_cbuf = InvalidBuffer;
	}

	scan->xs_continue_hot = false;

	scan->kill_prior_tuple = false;		/* for safety */

	FunctionCall5(procedure,
				  PointerGetDatum(scan),
				  PointerGetDatum(keys),
				  Int32GetDatum(nkeys),
				  PointerGetDatum(orderbys),
				  Int32GetDatum(norderbys));
}

/* ----------------
 *		index_endscan - end a scan
 * ----------------
 */
void
index_endscan(IndexScanDesc scan)
{
	FmgrInfo   *procedure;

	SCAN_CHECKS;
	GET_SCAN_PROCEDURE(amendscan);

	/* Release any held pin on a heap page */
	if (BufferIsValid(scan->xs_cbuf))
	{
		ReleaseBuffer(scan->xs_cbuf);
		scan->xs_cbuf = InvalidBuffer;
	}

	/* End the AM's scan */
	FunctionCall1(procedure, PointerGetDatum(scan));

	/* Release index refcount acquired by index_beginscan */
	RelationDecrementReferenceCount(scan->indexRelation);

	/* Release the scan data structure itself */
	IndexScanEnd(scan);
}

/* ----------------
 *		index_markpos  - mark a scan position
 * ----------------
 */
void
index_markpos(IndexScanDesc scan)
{
	FmgrInfo   *procedure;

	SCAN_CHECKS;
	GET_SCAN_PROCEDURE(ammarkpos);

	FunctionCall1(procedure, PointerGetDatum(scan));
}

/* ----------------
 *		index_restrpos	- restore a scan position
 *
 * NOTE: this only restores the internal scan state of the index AM.
 * The current result tuple (scan->xs_ctup) doesn't change.  See comments
 * for ExecRestrPos().
 *
 * NOTE: in the presence of HOT chains, mark/restore only works correctly
 * if the scan's snapshot is MVCC-safe; that ensures that there's at most one
 * returnable tuple in each HOT chain, and so restoring the prior state at the
 * granularity of the index AM is sufficient.  Since the only current user
 * of mark/restore functionality is nodeMergejoin.c, this effectively means
 * that merge-join plans only work for MVCC snapshots.  This could be fixed
 * if necessary, but for now it seems unimportant.
 * ----------------
 */
void
index_restrpos(IndexScanDesc scan)
{
	FmgrInfo   *procedure;

	Assert(IsMVCCSnapshot(scan->xs_snapshot));

	SCAN_CHECKS;
	GET_SCAN_PROCEDURE(amrestrpos);

	scan->xs_continue_hot = false;

	scan->kill_prior_tuple = false;		/* for safety */

	FunctionCall1(procedure, PointerGetDatum(scan));
}

/* ----------------
 * index_getnext_tid - get the next TID from a scan
 *
 * The result is the next TID satisfying the scan keys,
 * or NULL if no more matching tuples exist.
 * ----------------
 */
ItemPointer
index_getnext_tid(IndexScanDesc scan, ScanDirection direction)
{
	FmgrInfo   *procedure;
	bool		found;

	SCAN_CHECKS;
	GET_SCAN_PROCEDURE(amgettuple);

	Assert(TransactionIdIsValid(RecentGlobalXmin));

	/*
	 * The AM's amgettuple proc finds the next index entry matching the scan
	 * keys, and puts the TID into scan->xs_ctup.t_self.  It should also set
	 * scan->xs_recheck and possibly scan->xs_itup, though we pay no attention
	 * to those fields here.
	 */
	found = DatumGetBool(FunctionCall2(procedure,
									   PointerGetDatum(scan),
									   Int32GetDatum(direction)));

	/* Reset kill flag immediately for safety */
	scan->kill_prior_tuple = false;

	/* If we're out of index entries, we're done */
	if (!found)
	{
		/* ... but first, release any held pin on a heap page */
		if (BufferIsValid(scan->xs_cbuf))
		{
			ReleaseBuffer(scan->xs_cbuf);
			scan->xs_cbuf = InvalidBuffer;
		}
		return NULL;
	}

	pgstat_count_index_tuples(scan->indexRelation, 1);

	/* Return the TID of the tuple we found. */
	return &scan->xs_ctup.t_self;
}

/* ----------------
 *		index_fetch_heap - get the scan's next heap tuple
 *
 * The result is a visible heap tuple associated with the index TID most
 * recently fetched by index_getnext_tid, or NULL if no more matching tuples
 * exist.  (There can be more than one matching tuple because of HOT chains,
 * although when using an MVCC snapshot it should be impossible for more than
 * one such tuple to exist.)
 *
 * On success, the buffer containing the heap tup is pinned (the pin will be
 * dropped in a future index_getnext_tid, index_fetch_heap or index_endscan
 * call).
 *
 * Note: caller must check scan->xs_recheck, and perform rechecking of the
 * scan keys if required.  We do not do that here because we don't have
 * enough information to do it efficiently in the general case.
 * ----------------
 */
HeapTuple
index_fetch_heap(IndexScanDesc scan)
{
	ItemPointer tid = &scan->xs_ctup.t_self;
	bool		all_dead = false;
	bool		got_heap_tuple;

	/* We can skip the buffer-switching logic if we're in mid-HOT chain. */
	if (!scan->xs_continue_hot)
	{
		/* Switch to correct buffer if we don't have it already */
		Buffer		prev_buf = scan->xs_cbuf;

		scan->xs_cbuf = ReleaseAndReadBuffer(scan->xs_cbuf,
											 scan->heapRelation,
											 ItemPointerGetBlockNumber(tid));

		/*
		 * Prune page, but only if we weren't already on this page
		 */
		if (prev_buf != scan->xs_cbuf)
			heap_page_prune_opt(scan->heapRelation, scan->xs_cbuf);
	}

	/* Obtain share-lock on the buffer so we can examine visibility */
	LockBuffer(scan->xs_cbuf, BUFFER_LOCK_SHARE);
	got_heap_tuple = heap_hot_search_buffer(tid, scan->heapRelation,
											scan->xs_cbuf,
											scan->xs_snapshot,
											&scan->xs_ctup,
											&all_dead,
											!scan->xs_continue_hot);
	LockBuffer(scan->xs_cbuf, BUFFER_LOCK_UNLOCK);

	if (got_heap_tuple)
	{
		/*
		 * Only in a non-MVCC snapshot can more than one member of the HOT
		 * chain be visible.
		 */
		scan->xs_continue_hot = !IsMVCCSnapshot(scan->xs_snapshot);
		pgstat_count_heap_fetch(scan->indexRelation);
		return &scan->xs_ctup;
	}

	/* We've reached the end of the HOT chain. */
	scan->xs_continue_hot = false;

	/*
	 * If we scanned a whole HOT chain and found only dead tuples, tell index
	 * AM to kill its entry for that TID (this will take effect in the next
	 * amgettuple call, in index_getnext_tid).  We do not do this when in
	 * recovery because it may violate MVCC to do so.  See comments in
	 * RelationGetIndexScan().
	 */
	if (!scan->xactStartedInRecovery)
		scan->kill_prior_tuple = all_dead;

	return NULL;
}

/* ----------------
 *		index_getnext - get the next heap tuple from a scan
 *
 * The result is the next heap tuple satisfying the scan keys and the
 * snapshot, or NULL if no more matching tuples exist.
 *
 * On success, the buffer containing the heap tup is pinned (the pin will be
 * dropped in a future index_getnext_tid, index_fetch_heap or index_endscan
 * call).
 *
 * Note: caller must check scan->xs_recheck, and perform rechecking of the
 * scan keys if required.  We do not do that here because we don't have
 * enough information to do it efficiently in the general case.
 * ----------------
 */
HeapTuple
index_getnext(IndexScanDesc scan, ScanDirection direction)
{
	HeapTuple	heapTuple;
	ItemPointer tid;

	for (;;)
	{
		if (scan->xs_continue_hot)
		{
			/*
			 * We are resuming scan of a HOT chain after having returned an
			 * earlier member.  Must still hold pin on current heap page.
			 */
			Assert(BufferIsValid(scan->xs_cbuf));
			Assert(ItemPointerGetBlockNumber(&scan->xs_ctup.t_self) ==
				   BufferGetBlockNumber(scan->xs_cbuf));
		}
		else
		{
			/* Time to fetch the next TID from the index */
			tid = index_getnext_tid(scan, direction);

			/* If we're out of index entries, we're done */
			if (tid == NULL)
				break;
		}

		/*
		 * Fetch the next (or only) visible heap tuple for this index entry.
		 * If we don't find anything, loop around and grab the next TID from
		 * the index.
		 */
		heapTuple = index_fetch_heap(scan);
		if (heapTuple != NULL)
			return heapTuple;
	}

	return NULL;				/* failure exit */
}

/* ----------------
 *		index_getbitmap - get all tuples at once from an index scan
 *
 * Adds the TIDs of all heap tuples satisfying the scan keys to a bitmap.
 * Since there's no interlock between the index scan and the eventual heap
 * access, this is only safe to use with MVCC-based snapshots: the heap
 * item slot could have been replaced by a newer tuple by the time we get
 * to it.
 *
 * Returns the number of matching tuples found.  (Note: this might be only
 * approximate, so it should only be used for statistical purposes.)
 * ----------------
 */
int64
index_getbitmap(IndexScanDesc scan, TIDBitmap *bitmap)
{
	FmgrInfo   *procedure;
	int64		ntids;
	Datum		d;

	SCAN_CHECKS;
	GET_SCAN_PROCEDURE(amgetbitmap);

	/* just make sure this is false... */
	scan->kill_prior_tuple = false;

	/*
	 * have the am's getbitmap proc do all the work.
	 */
	d = FunctionCall2(procedure,
					  PointerGetDatum(scan),
					  PointerGetDatum(bitmap));

	ntids = DatumGetInt64(d);

	/* If int8 is pass-by-ref, must free the result to avoid memory leak */
#ifndef USE_FLOAT8_BYVAL
	pfree(DatumGetPointer(d));
#endif

	pgstat_count_index_tuples(scan->indexRelation, ntids);

	return ntids;
}

/* ----------------
 *		index_bulk_delete - do mass deletion of index entries
 *
 *		callback routine tells whether a given main-heap tuple is
 *		to be deleted
 *
 *		return value is an optional palloc'd struct of statistics
 * ----------------
 */
IndexBulkDeleteResult *
index_bulk_delete(IndexVacuumInfo *info,
				  IndexBulkDeleteResult *stats,
				  IndexBulkDeleteCallback callback,
				  void *callback_state)
{
	Relation	indexRelation = info->index;
	FmgrInfo	procedure;
	IndexBulkDeleteResult *result;

	RELATION_CHECKS;
	GET_UNCACHED_REL_PROCEDURE(ambulkdelete);

	result = (IndexBulkDeleteResult *)
		DatumGetPointer(FunctionCall4(&procedure,
									  PointerGetDatum(info),
									  PointerGetDatum(stats),
									  PointerGetDatum((Pointer) callback),
									  PointerGetDatum(callback_state)));

	return result;
}

/* ----------------
 *		index_vacuum_cleanup - do post-deletion cleanup of an index
 *
 *		return value is an optional palloc'd struct of statistics
 * ----------------
 */
IndexBulkDeleteResult *
index_vacuum_cleanup(IndexVacuumInfo *info,
					 IndexBulkDeleteResult *stats)
{
	Relation	indexRelation = info->index;
	FmgrInfo	procedure;
	IndexBulkDeleteResult *result;

	RELATION_CHECKS;
	GET_UNCACHED_REL_PROCEDURE(amvacuumcleanup);

	result = (IndexBulkDeleteResult *)
		DatumGetPointer(FunctionCall2(&procedure,
									  PointerGetDatum(info),
									  PointerGetDatum(stats)));

	return result;
}

/* ----------------
 *		index_can_return
 *
 *		Does the index access method support index-only scans for the given
 *		column?
 * ----------------
 */
bool
index_can_return(Relation indexRelation, int attno)
{
	FmgrInfo   *procedure;

	RELATION_CHECKS;

	/* amcanreturn is optional; assume FALSE if not provided by AM */
	if (!RegProcedureIsValid(indexRelation->rd_am->amcanreturn))
		return false;

	GET_REL_PROCEDURE(amcanreturn);

	return DatumGetBool(FunctionCall2(procedure,
									  PointerGetDatum(indexRelation),
									  Int32GetDatum(attno)));
}

/* ----------------
 *		index_getprocid
 *
 *		Index access methods typically require support routines that are
 *		not directly the implementation of any WHERE-clause query operator
 *		and so cannot be kept in pg_amop.  Instead, such routines are kept
 *		in pg_amproc.  These registered procedure OIDs are assigned numbers
 *		according to a convention established by the access method.
 *		The general index code doesn't know anything about the routines
 *		involved; it just builds an ordered list of them for
 *		each attribute on which an index is defined.
 *
 *		As of Postgres 8.3, support routines within an operator family
 *		are further subdivided by the "left type" and "right type" of the
 *		query operator(s) that they support.  The "default" functions for a
 *		particular indexed attribute are those with both types equal to
 *		the index opclass' opcintype (note that this is subtly different
 *		from the indexed attribute's own type: it may be a binary-compatible
 *		type instead).  Only the default functions are stored in relcache
 *		entries --- access methods can use the syscache to look up non-default
 *		functions.
 *
 *		This routine returns the requested default procedure OID for a
 *		particular indexed attribute.
 * ----------------
 */
RegProcedure
index_getprocid(Relation irel,
				AttrNumber attnum,
				uint16 procnum)
{
	RegProcedure *loc;
	int			nproc;
	int			procindex;

	nproc = irel->rd_am->amsupport;

	Assert(procnum > 0 && procnum <= (uint16) nproc);

	procindex = (nproc * (attnum - 1)) + (procnum - 1);

	loc = irel->rd_support;

	Assert(loc != NULL);

	return loc[procindex];
}

/* ----------------
 *		index_getprocinfo
 *
 *		This routine allows index AMs to keep fmgr lookup info for
 *		support procs in the relcache.  As above, only the "default"
 *		functions for any particular indexed attribute are cached.
 *
 * Note: the return value points into cached data that will be lost during
 * any relcache rebuild!  Therefore, either use the callinfo right away,
 * or save it only after having acquired some type of lock on the index rel.
 * ----------------
 */
FmgrInfo *
index_getprocinfo(Relation irel,
				  AttrNumber attnum,
				  uint16 procnum)
{
	FmgrInfo   *locinfo;
	int			nproc;
	int			procindex;

	nproc = irel->rd_am->amsupport;

	Assert(procnum > 0 && procnum <= (uint16) nproc);

	procindex = (nproc * (attnum - 1)) + (procnum - 1);

	locinfo = irel->rd_supportinfo;

	Assert(locinfo != NULL);

	locinfo += procindex;

	/* Initialize the lookup info if first time through */
	if (locinfo->fn_oid == InvalidOid)
	{
		RegProcedure *loc = irel->rd_support;
		RegProcedure procId;

		Assert(loc != NULL);

		procId = loc[procindex];

		/*
		 * Complain if function was not found during IndexSupportInitialize.
		 * This should not happen unless the system tables contain bogus
		 * entries for the index opclass.  (If an AM wants to allow a support
		 * function to be optional, it can use index_getprocid.)
		 */
		if (!RegProcedureIsValid(procId))
			elog(ERROR, "missing support function %d for attribute %d of index \"%s\"",
				 procnum, attnum, RelationGetRelationName(irel));

		fmgr_info_cxt(procId, locinfo, irel->rd_indexcxt);
	}

	return locinfo;
}
