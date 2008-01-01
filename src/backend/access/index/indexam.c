/*-------------------------------------------------------------------------
 *
 * indexam.c
 *	  general index access method routines
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/index/indexam.c,v 1.101 2008/01/01 19:45:46 momjian Exp $
 *
 * INTERFACE ROUTINES
 *		index_open		- open an index relation by relation OID
 *		index_close		- close an index relation
 *		index_beginscan - start a scan of an index with amgettuple
 *		index_beginscan_multi - start a scan of an index with amgetmulti
 *		index_rescan	- restart a scan of an index
 *		index_endscan	- end a scan
 *		index_insert	- insert an index tuple into a relation
 *		index_markpos	- mark a scan position
 *		index_restrpos	- restore a scan position
 *		index_getnext	- get the next tuple from a scan
 *		index_getmulti	- get multiple tuples from a scan
 *		index_bulk_delete	- bulk deletion of index tuples
 *		index_vacuum_cleanup	- post-deletion cleanup of an index
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

#include "access/genam.h"
#include "access/heapam.h"
#include "access/transam.h"
#include "pgstat.h"
#include "utils/relcache.h"


/* ----------------------------------------------------------------
 *					macros used in index_ routines
 * ----------------------------------------------------------------
 */
#define RELATION_CHECKS \
( \
	AssertMacro(RelationIsValid(indexRelation)), \
	AssertMacro(PointerIsValid(indexRelation->rd_am)) \
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
						 int nkeys, ScanKey key);


/* ----------------------------------------------------------------
 *				   index_ interface functions
 * ----------------------------------------------------------------
 */

/* ----------------
 *		index_open - open an index relation by relation OID
 *
 *		If lockmode is not "NoLock", the specified kind of lock is
 *		obtained on the index.	(Generally, NoLock should only be
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
			 bool check_uniqueness)
{
	FmgrInfo   *procedure;

	RELATION_CHECKS;
	GET_REL_PROCEDURE(aminsert);

	/*
	 * have the am's insert proc do all the work.
	 */
	return DatumGetBool(FunctionCall6(procedure,
									  PointerGetDatum(indexRelation),
									  PointerGetDatum(values),
									  PointerGetDatum(isnull),
									  PointerGetDatum(heap_t_ctid),
									  PointerGetDatum(heapRelation),
									  BoolGetDatum(check_uniqueness)));
}

/*
 * index_beginscan - start a scan of an index with amgettuple
 *
 * Note: heapRelation may be NULL if there is no intention of calling
 * index_getnext on this scan; index_getnext_indexitem will not use the
 * heapRelation link (nor the snapshot).  However, the caller had better
 * be holding some kind of lock on the heap relation in any case, to ensure
 * no one deletes it (or the index) out from under us.	Caller must also
 * be holding a lock on the index.
 */
IndexScanDesc
index_beginscan(Relation heapRelation,
				Relation indexRelation,
				Snapshot snapshot,
				int nkeys, ScanKey key)
{
	IndexScanDesc scan;

	scan = index_beginscan_internal(indexRelation, nkeys, key);

	/*
	 * Save additional parameters into the scandesc.  Everything else was set
	 * up by RelationGetIndexScan.
	 */
	scan->is_multiscan = false;
	scan->heapRelation = heapRelation;
	scan->xs_snapshot = snapshot;

	return scan;
}

/*
 * index_beginscan_multi - start a scan of an index with amgetmulti
 *
 * As above, caller had better be holding some lock on the parent heap
 * relation, even though it's not explicitly mentioned here.
 */
IndexScanDesc
index_beginscan_multi(Relation indexRelation,
					  Snapshot snapshot,
					  int nkeys, ScanKey key)
{
	IndexScanDesc scan;

	scan = index_beginscan_internal(indexRelation, nkeys, key);

	/*
	 * Save additional parameters into the scandesc.  Everything else was set
	 * up by RelationGetIndexScan.
	 */
	scan->is_multiscan = true;
	scan->xs_snapshot = snapshot;

	return scan;
}

/*
 * index_beginscan_internal --- common code for index_beginscan variants
 */
static IndexScanDesc
index_beginscan_internal(Relation indexRelation,
						 int nkeys, ScanKey key)
{
	IndexScanDesc scan;
	FmgrInfo   *procedure;

	RELATION_CHECKS;
	GET_REL_PROCEDURE(ambeginscan);

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
									  PointerGetDatum(key)));

	return scan;
}

/* ----------------
 *		index_rescan  - (re)start a scan of an index
 *
 * The caller may specify a new set of scankeys (but the number of keys
 * cannot change).	To restart the scan without changing keys, pass NULL
 * for the key array.
 *
 * Note that this is also called when first starting an indexscan;
 * see RelationGetIndexScan.  Keys *must* be passed in that case,
 * unless scan->numberOfKeys is zero.
 * ----------------
 */
void
index_rescan(IndexScanDesc scan, ScanKey key)
{
	FmgrInfo   *procedure;

	SCAN_CHECKS;
	GET_SCAN_PROCEDURE(amrescan);

	/* Release any held pin on a heap page */
	if (BufferIsValid(scan->xs_cbuf))
	{
		ReleaseBuffer(scan->xs_cbuf);
		scan->xs_cbuf = InvalidBuffer;
	}

	scan->xs_next_hot = InvalidOffsetNumber;

	scan->kill_prior_tuple = false;		/* for safety */

	FunctionCall2(procedure,
				  PointerGetDatum(scan),
				  PointerGetDatum(key));
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
 * that merge-join plans only work for MVCC snapshots.	This could be fixed
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

	scan->xs_next_hot = InvalidOffsetNumber;

	scan->kill_prior_tuple = false;		/* for safety */

	FunctionCall1(procedure, PointerGetDatum(scan));
}

/* ----------------
 *		index_getnext - get the next heap tuple from a scan
 *
 * The result is the next heap tuple satisfying the scan keys and the
 * snapshot, or NULL if no more matching tuples exist.	On success,
 * the buffer containing the heap tuple is pinned (the pin will be dropped
 * at the next index_getnext or index_endscan).
 * ----------------
 */
HeapTuple
index_getnext(IndexScanDesc scan, ScanDirection direction)
{
	HeapTuple	heapTuple = &scan->xs_ctup;
	ItemPointer tid = &heapTuple->t_self;
	FmgrInfo   *procedure;

	SCAN_CHECKS;
	GET_SCAN_PROCEDURE(amgettuple);

	/*
	 * We always reset xs_hot_dead; if we are here then either we are just
	 * starting the scan, or we previously returned a visible tuple, and in
	 * either case it's inappropriate to kill the prior index entry.
	 */
	scan->xs_hot_dead = false;

	for (;;)
	{
		OffsetNumber offnum;
		bool		at_chain_start;
		Page		dp;

		if (scan->xs_next_hot != InvalidOffsetNumber)
		{
			/*
			 * We are resuming scan of a HOT chain after having returned an
			 * earlier member.	Must still hold pin on current heap page.
			 */
			Assert(BufferIsValid(scan->xs_cbuf));
			Assert(ItemPointerGetBlockNumber(tid) ==
				   BufferGetBlockNumber(scan->xs_cbuf));
			Assert(TransactionIdIsValid(scan->xs_prev_xmax));
			offnum = scan->xs_next_hot;
			at_chain_start = false;
			scan->xs_next_hot = InvalidOffsetNumber;
		}
		else
		{
			bool		found;
			Buffer		prev_buf;

			/*
			 * If we scanned a whole HOT chain and found only dead tuples,
			 * tell index AM to kill its entry for that TID.
			 */
			scan->kill_prior_tuple = scan->xs_hot_dead;

			/*
			 * The AM's gettuple proc finds the next index entry matching the
			 * scan keys, and puts the TID in xs_ctup.t_self (ie, *tid).
			 */
			found = DatumGetBool(FunctionCall2(procedure,
											   PointerGetDatum(scan),
											   Int32GetDatum(direction)));

			/* Reset kill flag immediately for safety */
			scan->kill_prior_tuple = false;

			/* If we're out of index entries, break out of outer loop */
			if (!found)
				break;

			pgstat_count_index_tuples(scan->indexRelation, 1);

			/* Switch to correct buffer if we don't have it already */
			prev_buf = scan->xs_cbuf;
			scan->xs_cbuf = ReleaseAndReadBuffer(scan->xs_cbuf,
												 scan->heapRelation,
											 ItemPointerGetBlockNumber(tid));

			/*
			 * Prune page, but only if we weren't already on this page
			 */
			if (prev_buf != scan->xs_cbuf)
				heap_page_prune_opt(scan->heapRelation, scan->xs_cbuf,
									RecentGlobalXmin);

			/* Prepare to scan HOT chain starting at index-referenced offnum */
			offnum = ItemPointerGetOffsetNumber(tid);
			at_chain_start = true;

			/* We don't know what the first tuple's xmin should be */
			scan->xs_prev_xmax = InvalidTransactionId;

			/* Initialize flag to detect if all entries are dead */
			scan->xs_hot_dead = true;
		}

		/* Obtain share-lock on the buffer so we can examine visibility */
		LockBuffer(scan->xs_cbuf, BUFFER_LOCK_SHARE);

		dp = (Page) BufferGetPage(scan->xs_cbuf);

		/* Scan through possible multiple members of HOT-chain */
		for (;;)
		{
			ItemId		lp;
			ItemPointer ctid;

			/* check for bogus TID */
			if (offnum < FirstOffsetNumber ||
				offnum > PageGetMaxOffsetNumber(dp))
				break;

			lp = PageGetItemId(dp, offnum);

			/* check for unused, dead, or redirected items */
			if (!ItemIdIsNormal(lp))
			{
				/* We should only see a redirect at start of chain */
				if (ItemIdIsRedirected(lp) && at_chain_start)
				{
					/* Follow the redirect */
					offnum = ItemIdGetRedirect(lp);
					at_chain_start = false;
					continue;
				}
				/* else must be end of chain */
				break;
			}

			/*
			 * We must initialize all of *heapTuple (ie, scan->xs_ctup) since
			 * it is returned to the executor on success.
			 */
			heapTuple->t_data = (HeapTupleHeader) PageGetItem(dp, lp);
			heapTuple->t_len = ItemIdGetLength(lp);
			ItemPointerSetOffsetNumber(tid, offnum);
			heapTuple->t_tableOid = RelationGetRelid(scan->heapRelation);
			ctid = &heapTuple->t_data->t_ctid;

			/*
			 * Shouldn't see a HEAP_ONLY tuple at chain start.  (This test
			 * should be unnecessary, since the chain root can't be removed
			 * while we have pin on the index entry, but let's make it
			 * anyway.)
			 */
			if (at_chain_start && HeapTupleIsHeapOnly(heapTuple))
				break;

			/*
			 * The xmin should match the previous xmax value, else chain is
			 * broken.	(Note: this test is not optional because it protects
			 * us against the case where the prior chain member's xmax aborted
			 * since we looked at it.)
			 */
			if (TransactionIdIsValid(scan->xs_prev_xmax) &&
				!TransactionIdEquals(scan->xs_prev_xmax,
								  HeapTupleHeaderGetXmin(heapTuple->t_data)))
				break;

			/* If it's visible per the snapshot, we must return it */
			if (HeapTupleSatisfiesVisibility(heapTuple, scan->xs_snapshot,
											 scan->xs_cbuf))
			{
				/*
				 * If the snapshot is MVCC, we know that it could accept at
				 * most one member of the HOT chain, so we can skip examining
				 * any more members.  Otherwise, check for continuation of the
				 * HOT-chain, and set state for next time.
				 */
				if (IsMVCCSnapshot(scan->xs_snapshot))
					scan->xs_next_hot = InvalidOffsetNumber;
				else if (HeapTupleIsHotUpdated(heapTuple))
				{
					Assert(ItemPointerGetBlockNumber(ctid) ==
						   ItemPointerGetBlockNumber(tid));
					scan->xs_next_hot = ItemPointerGetOffsetNumber(ctid);
					scan->xs_prev_xmax = HeapTupleHeaderGetXmax(heapTuple->t_data);
				}
				else
					scan->xs_next_hot = InvalidOffsetNumber;

				LockBuffer(scan->xs_cbuf, BUFFER_LOCK_UNLOCK);

				pgstat_count_heap_fetch(scan->indexRelation);

				return heapTuple;
			}

			/*
			 * If we can't see it, maybe no one else can either.  Check to see
			 * if the tuple is dead to all transactions.  If we find that all
			 * the tuples in the HOT chain are dead, we'll signal the index AM
			 * to not return that TID on future indexscans.
			 */
			if (scan->xs_hot_dead &&
				HeapTupleSatisfiesVacuum(heapTuple->t_data, RecentGlobalXmin,
										 scan->xs_cbuf) != HEAPTUPLE_DEAD)
				scan->xs_hot_dead = false;

			/*
			 * Check to see if HOT chain continues past this tuple; if so
			 * fetch the next offnum (we don't bother storing it into
			 * xs_next_hot, but must store xs_prev_xmax), and loop around.
			 */
			if (HeapTupleIsHotUpdated(heapTuple))
			{
				Assert(ItemPointerGetBlockNumber(ctid) ==
					   ItemPointerGetBlockNumber(tid));
				offnum = ItemPointerGetOffsetNumber(ctid);
				at_chain_start = false;
				scan->xs_prev_xmax = HeapTupleHeaderGetXmax(heapTuple->t_data);
			}
			else
				break;			/* end of chain */
		}						/* loop over a single HOT chain */

		LockBuffer(scan->xs_cbuf, BUFFER_LOCK_UNLOCK);

		/* Loop around to ask index AM for another TID */
		scan->xs_next_hot = InvalidOffsetNumber;
	}

	/* Release any held pin on a heap page */
	if (BufferIsValid(scan->xs_cbuf))
	{
		ReleaseBuffer(scan->xs_cbuf);
		scan->xs_cbuf = InvalidBuffer;
	}

	return NULL;				/* failure exit */
}

/* ----------------
 *		index_getnext_indexitem - get the next index tuple from a scan
 *
 * Finds the next index tuple satisfying the scan keys.  Note that the
 * corresponding heap tuple is not accessed, and thus no time qual (snapshot)
 * check is done, other than the index AM's internal check for killed tuples
 * (which most callers of this routine will probably want to suppress by
 * setting scan->ignore_killed_tuples = false).
 *
 * On success (TRUE return), the heap TID of the found index entry is in
 * scan->xs_ctup.t_self.  scan->xs_cbuf is untouched.
 * ----------------
 */
bool
index_getnext_indexitem(IndexScanDesc scan,
						ScanDirection direction)
{
	FmgrInfo   *procedure;
	bool		found;

	SCAN_CHECKS;
	GET_SCAN_PROCEDURE(amgettuple);

	/* just make sure this is false... */
	scan->kill_prior_tuple = false;

	/*
	 * have the am's gettuple proc do all the work.
	 */
	found = DatumGetBool(FunctionCall2(procedure,
									   PointerGetDatum(scan),
									   Int32GetDatum(direction)));

	if (found)
		pgstat_count_index_tuples(scan->indexRelation, 1);

	return found;
}

/* ----------------
 *		index_getmulti - get multiple tuples from an index scan
 *
 * Collects the TIDs of multiple heap tuples satisfying the scan keys.
 * Since there's no interlock between the index scan and the eventual heap
 * access, this is only safe to use with MVCC-based snapshots: the heap
 * item slot could have been replaced by a newer tuple by the time we get
 * to it.
 *
 * A TRUE result indicates more calls should occur; a FALSE result says the
 * scan is done.  *returned_tids could be zero or nonzero in either case.
 * ----------------
 */
bool
index_getmulti(IndexScanDesc scan,
			   ItemPointer tids, int32 max_tids,
			   int32 *returned_tids)
{
	FmgrInfo   *procedure;
	bool		found;

	SCAN_CHECKS;
	GET_SCAN_PROCEDURE(amgetmulti);

	/* just make sure this is false... */
	scan->kill_prior_tuple = false;

	/*
	 * have the am's getmulti proc do all the work.
	 */
	found = DatumGetBool(FunctionCall4(procedure,
									   PointerGetDatum(scan),
									   PointerGetDatum(tids),
									   Int32GetDatum(max_tids),
									   PointerGetDatum(returned_tids)));

	pgstat_count_index_tuples(scan->indexRelation, *returned_tids);

	return found;
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
	FmgrInfo   *procedure;
	IndexBulkDeleteResult *result;

	RELATION_CHECKS;
	GET_REL_PROCEDURE(ambulkdelete);

	result = (IndexBulkDeleteResult *)
		DatumGetPointer(FunctionCall4(procedure,
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
	FmgrInfo   *procedure;
	IndexBulkDeleteResult *result;

	RELATION_CHECKS;
	GET_REL_PROCEDURE(amvacuumcleanup);

	result = (IndexBulkDeleteResult *)
		DatumGetPointer(FunctionCall2(procedure,
									  PointerGetDatum(info),
									  PointerGetDatum(stats)));

	return result;
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
 *		type instead).	Only the default functions are stored in relcache
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
 *		support procs in the relcache.	As above, only the "default"
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
