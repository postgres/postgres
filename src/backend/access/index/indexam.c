/*-------------------------------------------------------------------------
 *
 * indexam.c
 *	  general index access method routines
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/index/indexam.c,v 1.71 2003/09/25 06:57:57 petere Exp $
 *
 * INTERFACE ROUTINES
 *		index_open		- open an index relation by relation OID
 *		index_openrv	- open an index relation specified by a RangeVar
 *		index_openr		- open a system index relation by name
 *		index_close		- close an index relation
 *		index_beginscan - start a scan of an index
 *		index_rescan	- restart a scan of an index
 *		index_endscan	- end a scan
 *		index_insert	- insert an index tuple into a relation
 *		index_markpos	- mark a scan position
 *		index_restrpos	- restore a scan position
 *		index_getnext	- get the next tuple from a scan
 *		index_bulk_delete	- bulk deletion of index tuples
 *		index_vacuum_cleanup	- post-deletion cleanup of an index
 *		index_cost_estimator	- fetch amcostestimate procedure OID
 *		index_getprocid - get a support procedure OID
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
#include "utils/relcache.h"

#include "pgstat.h"

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

#define GET_REL_PROCEDURE(x,y) \
( \
	procedure = indexRelation->rd_am->y, \
	(!RegProcedureIsValid(procedure)) ? \
		elog(ERROR, "index_%s: invalid %s regproc", \
			 CppAsString(x), CppAsString(y)) \
	: (void)NULL \
)

#define GET_SCAN_PROCEDURE(x,y) \
( \
	procedure = scan->indexRelation->rd_am->y, \
	(!RegProcedureIsValid(procedure)) ? \
		elog(ERROR, "index_%s: invalid %s regproc", \
			 CppAsString(x), CppAsString(y)) \
	: (void)NULL \
)


/* ----------------------------------------------------------------
 *				   index_ interface functions
 * ----------------------------------------------------------------
 */

/* ----------------
 *		index_open - open an index relation by relation OID
 *
 *		Note: we acquire no lock on the index.	An AccessShareLock is
 *		acquired by index_beginscan (and released by index_endscan).
 *		Generally, the caller should already hold some type of lock on
 *		the parent relation to ensure that the index doesn't disappear.
 *
 *		This is a convenience routine adapted for indexscan use.
 *		Some callers may prefer to use relation_open directly.
 * ----------------
 */
Relation
index_open(Oid relationId)
{
	Relation	r;

	r = relation_open(relationId, NoLock);

	if (r->rd_rel->relkind != RELKIND_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index",
						RelationGetRelationName(r))));

	pgstat_initstats(&r->pgstat_info, r);

	return r;
}

/* ----------------
 *		index_openrv - open an index relation specified
 *		by a RangeVar node
 *
 *		As above, but relation is specified by a RangeVar.
 * ----------------
 */
Relation
index_openrv(const RangeVar *relation)
{
	Relation	r;

	r = relation_openrv(relation, NoLock);

	if (r->rd_rel->relkind != RELKIND_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index",
						RelationGetRelationName(r))));

	pgstat_initstats(&r->pgstat_info, r);

	return r;
}

/* ----------------
 *		index_openr - open a system index relation specified by name.
 *
 *		As above, but the relation is specified by an unqualified name;
 *		it is assumed to live in the system catalog namespace.
 * ----------------
 */
Relation
index_openr(const char *sysRelationName)
{
	Relation	r;

	r = relation_openr(sysRelationName, NoLock);

	if (r->rd_rel->relkind != RELKIND_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index",
						RelationGetRelationName(r))));

	pgstat_initstats(&r->pgstat_info, r);

	return r;
}

/* ----------------
 *		index_close - close a index relation
 *
 *		presently the relcache routines do all the work we need
 *		to open/close index relations.
 * ----------------
 */
void
index_close(Relation relation)
{
	RelationClose(relation);
}

/* ----------------
 *		index_insert - insert an index tuple into a relation
 * ----------------
 */
InsertIndexResult
index_insert(Relation indexRelation,
			 Datum *datums,
			 char *nulls,
			 ItemPointer heap_t_ctid,
			 Relation heapRelation,
			 bool check_uniqueness)
{
	RegProcedure procedure;
	InsertIndexResult specificResult;

	RELATION_CHECKS;
	GET_REL_PROCEDURE(insert, aminsert);

	/*
	 * have the am's insert proc do all the work.
	 */
	specificResult = (InsertIndexResult)
		DatumGetPointer(OidFunctionCall6(procedure,
										 PointerGetDatum(indexRelation),
										 PointerGetDatum(datums),
										 PointerGetDatum(nulls),
										 PointerGetDatum(heap_t_ctid),
										 PointerGetDatum(heapRelation),
										 BoolGetDatum(check_uniqueness)));

	/* must be pfree'ed */
	return specificResult;
}

/* ----------------
 *		index_beginscan - start a scan of an index
 *
 * Note: heapRelation may be NULL if there is no intention of calling
 * index_getnext on this scan; index_getnext_indexitem will not use the
 * heapRelation link (nor the snapshot).  However, the caller had better
 * be holding some kind of lock on the heap relation in any case, to ensure
 * no one deletes it (or the index) out from under us.
 * ----------------
 */
IndexScanDesc
index_beginscan(Relation heapRelation,
				Relation indexRelation,
				Snapshot snapshot,
				int nkeys, ScanKey key)
{
	IndexScanDesc scan;
	RegProcedure procedure;

	RELATION_CHECKS;
	GET_REL_PROCEDURE(beginscan, ambeginscan);

	RelationIncrementReferenceCount(indexRelation);

	/*
	 * Acquire AccessShareLock for the duration of the scan
	 *
	 * Note: we could get an SI inval message here and consequently have to
	 * rebuild the relcache entry.	The refcount increment above ensures
	 * that we will rebuild it and not just flush it...
	 */
	LockRelation(indexRelation, AccessShareLock);

	/*
	 * Tell the AM to open a scan.
	 */
	scan = (IndexScanDesc)
		DatumGetPointer(OidFunctionCall3(procedure,
										 PointerGetDatum(indexRelation),
										 Int32GetDatum(nkeys),
										 PointerGetDatum(key)));

	/*
	 * Save additional parameters into the scandesc.  Everything else was
	 * set up by RelationGetIndexScan.
	 */
	scan->heapRelation = heapRelation;
	scan->xs_snapshot = snapshot;

	/*
	 * We want to look up the amgettuple procedure just once per scan, not
	 * once per index_getnext call.  So do it here and save the fmgr info
	 * result in the scan descriptor.
	 */
	GET_SCAN_PROCEDURE(beginscan, amgettuple);
	fmgr_info(procedure, &scan->fn_getnext);

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
	RegProcedure procedure;

	SCAN_CHECKS;
	GET_SCAN_PROCEDURE(rescan, amrescan);

	scan->kill_prior_tuple = false;		/* for safety */
	scan->keys_are_unique = false;		/* may be set by index AM */
	scan->got_tuple = false;
	scan->unique_tuple_pos = 0;
	scan->unique_tuple_mark = 0;

	OidFunctionCall2(procedure,
					 PointerGetDatum(scan),
					 PointerGetDatum(key));

	pgstat_reset_index_scan(&scan->xs_pgstat_info);
}

/* ----------------
 *		index_endscan - end a scan
 * ----------------
 */
void
index_endscan(IndexScanDesc scan)
{
	RegProcedure procedure;

	SCAN_CHECKS;
	GET_SCAN_PROCEDURE(endscan, amendscan);

	/* Release any held pin on a heap page */
	if (BufferIsValid(scan->xs_cbuf))
	{
		ReleaseBuffer(scan->xs_cbuf);
		scan->xs_cbuf = InvalidBuffer;
	}

	/* End the AM's scan */
	OidFunctionCall1(procedure, PointerGetDatum(scan));

	/* Release index lock and refcount acquired by index_beginscan */

	UnlockRelation(scan->indexRelation, AccessShareLock);

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
	RegProcedure procedure;

	SCAN_CHECKS;
	GET_SCAN_PROCEDURE(markpos, ammarkpos);

	scan->unique_tuple_mark = scan->unique_tuple_pos;

	OidFunctionCall1(procedure, PointerGetDatum(scan));
}

/* ----------------
 *		index_restrpos	- restore a scan position
 * ----------------
 */
void
index_restrpos(IndexScanDesc scan)
{
	RegProcedure procedure;

	SCAN_CHECKS;
	GET_SCAN_PROCEDURE(restrpos, amrestrpos);

	scan->kill_prior_tuple = false;		/* for safety */

	/*
	 * We do not reset got_tuple; so if the scan is actually being
	 * short-circuited by index_getnext, the effective position
	 * restoration is done by restoring unique_tuple_pos.
	 */
	scan->unique_tuple_pos = scan->unique_tuple_mark;

	OidFunctionCall1(procedure, PointerGetDatum(scan));
}

/* ----------------
 *		index_getnext - get the next heap tuple from a scan
 *
 * The result is the next heap tuple satisfying the scan keys and the
 * snapshot, or NULL if no more matching tuples exist.	On success,
 * the buffer containing the heap tuple is pinned (the pin will be dropped
 * at the next index_getnext or index_endscan).  The index TID corresponding
 * to the heap tuple can be obtained if needed from scan->currentItemData.
 * ----------------
 */
HeapTuple
index_getnext(IndexScanDesc scan, ScanDirection direction)
{
	HeapTuple	heapTuple = &scan->xs_ctup;

	SCAN_CHECKS;

	/* Release any previously held pin */
	if (BufferIsValid(scan->xs_cbuf))
	{
		ReleaseBuffer(scan->xs_cbuf);
		scan->xs_cbuf = InvalidBuffer;
	}

	/*
	 * If we already got a tuple and it must be unique, there's no need to
	 * make the index AM look through any additional tuples.  (This can
	 * save a useful amount of work in scenarios where there are many dead
	 * tuples due to heavy update activity.)
	 *
	 * To do this we must keep track of the logical scan position
	 * (before/on/after tuple).  Also, we have to be sure to release scan
	 * resources before returning NULL; if we fail to do so then a
	 * multi-index scan can easily run the system out of free buffers.	We
	 * can release index-level resources fairly cheaply by calling
	 * index_rescan.  This means there are two persistent states as far as
	 * the index AM is concerned: on-tuple and rescanned.  If we are
	 * actually asked to re-fetch the single tuple, we have to go through
	 * a fresh indexscan startup, which penalizes that (infrequent) case.
	 */
	if (scan->keys_are_unique && scan->got_tuple)
	{
		int			new_tuple_pos = scan->unique_tuple_pos;

		if (ScanDirectionIsForward(direction))
		{
			if (new_tuple_pos <= 0)
				new_tuple_pos++;
		}
		else
		{
			if (new_tuple_pos >= 0)
				new_tuple_pos--;
		}
		if (new_tuple_pos == 0)
		{
			/*
			 * We are moving onto the unique tuple from having been off
			 * it. We just fall through and let the index AM do the work.
			 * Note we should get the right answer regardless of scan
			 * direction.
			 */
			scan->unique_tuple_pos = 0; /* need to update position */
		}
		else
		{
			/*
			 * Moving off the tuple; must do amrescan to release
			 * index-level pins before we return NULL.	Since index_rescan
			 * will reset my state, must save and restore...
			 */
			int			unique_tuple_mark = scan->unique_tuple_mark;

			index_rescan(scan, NULL /* no change to key */ );

			scan->keys_are_unique = true;
			scan->got_tuple = true;
			scan->unique_tuple_pos = new_tuple_pos;
			scan->unique_tuple_mark = unique_tuple_mark;

			return NULL;
		}
	}

	/* just make sure this is false... */
	scan->kill_prior_tuple = false;

	for (;;)
	{
		bool		found;
		uint16		sv_infomask;

		pgstat_count_index_scan(&scan->xs_pgstat_info);

		/*
		 * The AM's gettuple proc finds the next tuple matching the scan
		 * keys.  index_beginscan already set up fn_getnext.
		 */
		found = DatumGetBool(FunctionCall2(&scan->fn_getnext,
										   PointerGetDatum(scan),
										   Int32GetDatum(direction)));

		/* Reset kill flag immediately for safety */
		scan->kill_prior_tuple = false;

		if (!found)
			return NULL;		/* failure exit */

		/*
		 * Fetch the heap tuple and see if it matches the snapshot.
		 */
		if (heap_fetch(scan->heapRelation, scan->xs_snapshot,
					   heapTuple, &scan->xs_cbuf, true,
					   &scan->xs_pgstat_info))
			break;

		/* Skip if no tuple at this location */
		if (heapTuple->t_data == NULL)
			continue;			/* should we raise an error instead? */

		/*
		 * If we can't see it, maybe no one else can either.  Check to see
		 * if the tuple is dead to all transactions.  If so, signal the
		 * index AM to not return it on future indexscans.
		 *
		 * We told heap_fetch to keep a pin on the buffer, so we can
		 * re-access the tuple here.  But we must re-lock the buffer
		 * first. Also, it's just barely possible for an update of hint
		 * bits to occur here.
		 */
		LockBuffer(scan->xs_cbuf, BUFFER_LOCK_SHARE);
		sv_infomask = heapTuple->t_data->t_infomask;

		if (HeapTupleSatisfiesVacuum(heapTuple->t_data, RecentGlobalXmin) ==
			HEAPTUPLE_DEAD)
			scan->kill_prior_tuple = true;

		if (sv_infomask != heapTuple->t_data->t_infomask)
			SetBufferCommitInfoNeedsSave(scan->xs_cbuf);
		LockBuffer(scan->xs_cbuf, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(scan->xs_cbuf);
		scan->xs_cbuf = InvalidBuffer;
	}

	/* Success exit */
	scan->got_tuple = true;

	/*
	 * If we just fetched a known-unique tuple, then subsequent calls will
	 * go through the short-circuit code above.  unique_tuple_pos has been
	 * initialized to 0, which is the correct state ("on row").
	 */

	pgstat_count_index_getnext(&scan->xs_pgstat_info);

	return heapTuple;
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
 * On success (TRUE return), the found index TID is in scan->currentItemData,
 * and its heap TID is in scan->xs_ctup.t_self.  scan->xs_cbuf is untouched.
 * ----------------
 */
bool
index_getnext_indexitem(IndexScanDesc scan,
						ScanDirection direction)
{
	bool		found;

	SCAN_CHECKS;

	/* just make sure this is false... */
	scan->kill_prior_tuple = false;

	/*
	 * have the am's gettuple proc do all the work. index_beginscan
	 * already set up fn_getnext.
	 */
	found = DatumGetBool(FunctionCall2(&scan->fn_getnext,
									   PointerGetDatum(scan),
									   Int32GetDatum(direction)));

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
index_bulk_delete(Relation indexRelation,
				  IndexBulkDeleteCallback callback,
				  void *callback_state)
{
	RegProcedure procedure;
	IndexBulkDeleteResult *result;

	RELATION_CHECKS;
	GET_REL_PROCEDURE(bulk_delete, ambulkdelete);

	result = (IndexBulkDeleteResult *)
		DatumGetPointer(OidFunctionCall3(procedure,
										 PointerGetDatum(indexRelation),
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
index_vacuum_cleanup(Relation indexRelation,
					 IndexVacuumCleanupInfo *info,
					 IndexBulkDeleteResult *stats)
{
	RegProcedure procedure;
	IndexBulkDeleteResult *result;

	RELATION_CHECKS;

	/* It's okay for an index AM not to have a vacuumcleanup procedure */
	if (!RegProcedureIsValid(indexRelation->rd_am->amvacuumcleanup))
		return stats;

	GET_REL_PROCEDURE(vacuum_cleanup, amvacuumcleanup);

	result = (IndexBulkDeleteResult *)
		DatumGetPointer(OidFunctionCall3(procedure,
										 PointerGetDatum(indexRelation),
										 PointerGetDatum((Pointer) info),
									  PointerGetDatum((Pointer) stats)));

	return result;
}

/* ----------------
 *		index_cost_estimator
 *
 *		Fetch the amcostestimate procedure OID for an index.
 *
 *		We could combine fetching and calling the procedure,
 *		as index_insert does for example; but that would require
 *		importing a bunch of planner/optimizer stuff into this file.
 * ----------------
 */
RegProcedure
index_cost_estimator(Relation indexRelation)
{
	RegProcedure procedure;

	RELATION_CHECKS;
	GET_REL_PROCEDURE(cost_estimator, amcostestimate);

	return procedure;
}

/* ----------------
 *		index_getprocid
 *
 *		Some indexed access methods may require support routines that are
 *		not in the operator class/operator model imposed by pg_am.	These
 *		access methods may store the OIDs of registered procedures they
 *		need in pg_amproc.	These registered procedure OIDs are ordered in
 *		a way that makes sense to the access method, and used only by the
 *		access method.	The general index code doesn't know anything about
 *		the routines involved; it just builds an ordered list of them for
 *		each attribute on which an index is defined.
 *
 *		This routine returns the requested procedure OID for a particular
 *		indexed attribute.
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
 *		support procs in the relcache.
 * ----------------
 */
struct FmgrInfo *
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
		 * Complain if function was not found during
		 * IndexSupportInitialize. This should not happen unless the
		 * system tables contain bogus entries for the index opclass.  (If
		 * an AM wants to allow a support function to be optional, it can
		 * use index_getprocid.)
		 */
		if (!RegProcedureIsValid(procId))
			elog(ERROR, "missing support function %d for attribute %d of index \"%s\"",
				 procnum, attnum, RelationGetRelationName(irel));

		fmgr_info_cxt(procId, locinfo, irel->rd_indexcxt);
	}

	return locinfo;
}
