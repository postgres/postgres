/*-------------------------------------------------------------------------
 *
 * indexam.c
 *	  general index access method routines
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/index/indexam.c,v 1.53 2001/10/06 23:21:43 tgl Exp $
 *
 * INTERFACE ROUTINES
 *		index_open		- open an index relation by relationId
 *		index_openr		- open a index relation by name
 *		index_close		- close a index relation
 *		index_beginscan - start a scan of an index
 *		index_rescan	- restart a scan of an index
 *		index_endscan	- end a scan
 *		index_insert	- insert an index tuple into a relation
 *		index_markpos	- mark a scan position
 *		index_restrpos	- restore a scan position
 *		index_getnext	- get the next tuple from a scan
 *		index_bulk_delete	- bulk deletion of index tuples
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
	AssertMacro(RelationIsValid(relation)), \
	AssertMacro(PointerIsValid(relation->rd_am)) \
)

#define SCAN_CHECKS \
( \
	AssertMacro(IndexScanIsValid(scan)), \
	AssertMacro(RelationIsValid(scan->relation)), \
	AssertMacro(PointerIsValid(scan->relation->rd_am)) \
)

#define GET_REL_PROCEDURE(x,y) \
( \
	procedure = relation->rd_am->y, \
	(!RegProcedureIsValid(procedure)) ? \
		elog(ERROR, "index_%s: invalid %s regproc", \
			CppAsString(x), CppAsString(y)) \
	: (void)NULL \
)

#define GET_SCAN_PROCEDURE(x,y) \
( \
	procedure = scan->relation->rd_am->y, \
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
 *		index_open - open an index relation by relationId
 *
 *		presently the relcache routines do all the work we need
 *		to open/close index relations.	However, callers of index_open
 *		expect it to succeed, so we need to check for a failure return.
 *
 *		Note: we acquire no lock on the index.	An AccessShareLock is
 *		acquired by index_beginscan (and released by index_endscan).
 * ----------------
 */
Relation
index_open(Oid relationId)
{
	Relation	r;

	r = RelationIdGetRelation(relationId);

	if (!RelationIsValid(r))
		elog(ERROR, "Index %u does not exist", relationId);

	if (r->rd_rel->relkind != RELKIND_INDEX)
		elog(ERROR, "%s is not an index relation", RelationGetRelationName(r));

	pgstat_initstats(&r->pgstat_info, r);

	return r;
}

/* ----------------
 *		index_openr - open a index relation by name
 *
 *		As above, but lookup by name instead of OID.
 * ----------------
 */
Relation
index_openr(char *relationName)
{
	Relation	r;

	r = RelationNameGetRelation(relationName);

	if (!RelationIsValid(r))
		elog(ERROR, "Index '%s' does not exist", relationName);

	if (r->rd_rel->relkind != RELKIND_INDEX)
		elog(ERROR, "%s is not an index relation", RelationGetRelationName(r));

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
index_insert(Relation relation,
			 Datum *datum,
			 char *nulls,
			 ItemPointer heap_t_ctid,
			 Relation heapRel)
{
	RegProcedure procedure;
	InsertIndexResult specificResult;

	RELATION_CHECKS;
	GET_REL_PROCEDURE(insert, aminsert);

	/*
	 * have the am's insert proc do all the work.
	 */
	specificResult = (InsertIndexResult)
		DatumGetPointer(OidFunctionCall5(procedure,
										 PointerGetDatum(relation),
										 PointerGetDatum(datum),
										 PointerGetDatum(nulls),
										 PointerGetDatum(heap_t_ctid),
										 PointerGetDatum(heapRel)));

	/* must be pfree'ed */
	return specificResult;
}

/* ----------------
 *		index_beginscan - start a scan of an index
 * ----------------
 */
IndexScanDesc
index_beginscan(Relation relation,
				bool scanFromEnd,
				uint16 numberOfKeys,
				ScanKey key)
{
	IndexScanDesc scan;
	RegProcedure procedure;

	RELATION_CHECKS;
	GET_REL_PROCEDURE(beginscan, ambeginscan);

	RelationIncrementReferenceCount(relation);

	/*
	 * Acquire AccessShareLock for the duration of the scan
	 *
	 * Note: we could get an SI inval message here and consequently have to
	 * rebuild the relcache entry.	The refcount increment above ensures
	 * that we will rebuild it and not just flush it...
	 */
	LockRelation(relation, AccessShareLock);

	scan = (IndexScanDesc)
		DatumGetPointer(OidFunctionCall4(procedure,
										 PointerGetDatum(relation),
										 BoolGetDatum(scanFromEnd),
										 UInt16GetDatum(numberOfKeys),
										 PointerGetDatum(key)));

	pgstat_initstats(&scan->xs_pgstat_info, relation);

	/*
	 * We want to look up the amgettuple procedure just once per scan,
	 * not once per index_getnext call.  So do it here and save
	 * the fmgr info result in the scan descriptor.
	 */
	GET_SCAN_PROCEDURE(beginscan, amgettuple);
	fmgr_info(procedure, &scan->fn_getnext);

	return scan;
}

/* ----------------
 *		index_rescan  - restart a scan of an index
 * ----------------
 */
void
index_rescan(IndexScanDesc scan, bool scanFromEnd, ScanKey key)
{
	RegProcedure procedure;

	SCAN_CHECKS;
	GET_SCAN_PROCEDURE(rescan, amrescan);

	OidFunctionCall3(procedure,
					 PointerGetDatum(scan),
					 BoolGetDatum(scanFromEnd),
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

	OidFunctionCall1(procedure, PointerGetDatum(scan));

	/* Release lock and refcount acquired by index_beginscan */

	UnlockRelation(scan->relation, AccessShareLock);

	RelationDecrementReferenceCount(scan->relation);

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

	OidFunctionCall1(procedure, PointerGetDatum(scan));
}

/* ----------------
 *		index_getnext - get the next tuple from a scan
 *
 *		A RetrieveIndexResult is a index tuple/heap tuple pair
 * ----------------
 */
RetrieveIndexResult
index_getnext(IndexScanDesc scan,
			  ScanDirection direction)
{
	RetrieveIndexResult result;

	SCAN_CHECKS;

	pgstat_count_index_scan(&scan->xs_pgstat_info);

	/*
	 * have the am's gettuple proc do all the work.
	 * index_beginscan already set up fn_getnext.
	 */
	result = (RetrieveIndexResult)
		DatumGetPointer(FunctionCall2(&scan->fn_getnext,
									  PointerGetDatum(scan),
									  Int32GetDatum(direction)));

	if (result != NULL)
		pgstat_count_index_getnext(&scan->xs_pgstat_info);
	return result;
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
index_bulk_delete(Relation relation,
				  IndexBulkDeleteCallback callback,
				  void *callback_state)
{
	RegProcedure procedure;
	IndexBulkDeleteResult *result;

	RELATION_CHECKS;
	GET_REL_PROCEDURE(bulk_delete, ambulkdelete);

	result = (IndexBulkDeleteResult *)
		DatumGetPointer(OidFunctionCall3(procedure,
										 PointerGetDatum(relation),
										 PointerGetDatum((Pointer) callback),
										 PointerGetDatum(callback_state)));

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
index_cost_estimator(Relation relation)
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

		Assert(loc != NULL);

		fmgr_info_cxt(loc[procindex], locinfo, irel->rd_indexcxt);
	}

	return locinfo;
}
