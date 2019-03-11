/*-------------------------------------------------------------------------
 *
 * tableam.h
 *	  POSTGRES table access method definitions.
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/tableam.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TABLEAM_H
#define TABLEAM_H

#include "access/relscan.h"
#include "access/sdir.h"
#include "utils/guc.h"
#include "utils/rel.h"
#include "utils/snapshot.h"


#define DEFAULT_TABLE_ACCESS_METHOD	"heap"

extern char *default_table_access_method;
extern bool synchronize_seqscans;


/*
 * API struct for a table AM.  Note this must be allocated in a
 * server-lifetime manner, typically as a static const struct, which then gets
 * returned by FormData_pg_am.amhandler.
 *
 * I most cases it's not appropriate to directly call the callbacks directly,
 * instead use the table_* wrapper functions.
 *
 * GetTableAmRoutine() asserts that required callbacks are filled in, remember
 * to update when adding a callback.
 */
typedef struct TableAmRoutine
{
	/* this must be set to T_TableAmRoutine */
	NodeTag		type;


	/* ------------------------------------------------------------------------
	 * Slot related callbacks.
	 * ------------------------------------------------------------------------
	 */

	/*
	 * Return slot implementation suitable for storing a tuple of this AM.
	 */
	const TupleTableSlotOps *(*slot_callbacks) (Relation rel);


	/* ------------------------------------------------------------------------
	 * Table scan callbacks.
	 * ------------------------------------------------------------------------
	 */

	/*
	 * Start a scan of `rel`.  The callback has to return a TableScanDesc,
	 * which will typically be embedded in a larger, AM specific, struct.
	 *
	 * If nkeys != 0, the results need to be filtered by those scan keys.
	 *
	 * pscan, if not NULL, will have already been initialized with
	 * parallelscan_initialize(), and has to be for the same relation. Will
	 * only be set coming from table_beginscan_parallel().
	 *
	 * allow_{strat, sync, pagemode} specify whether a scan strategy,
	 * synchronized scans, or page mode may be used (although not every AM
	 * will support those).
	 *
	 * is_{bitmapscan, samplescan} specify whether the scan is inteded to
	 * support those types of scans.
	 *
	 * if temp_snap is true, the snapshot will need to be deallocated at
	 * scan_end.
	 */
	TableScanDesc (*scan_begin) (Relation rel,
								 Snapshot snapshot,
								 int nkeys, struct ScanKeyData *key,
								 ParallelTableScanDesc pscan,
								 bool allow_strat,
								 bool allow_sync,
								 bool allow_pagemode,
								 bool is_bitmapscan,
								 bool is_samplescan,
								 bool temp_snap);

	/*
	 * Release resources and deallocate scan. If TableScanDesc.temp_snap,
	 * TableScanDesc.rs_snapshot needs to be unregistered.
	 */
	void		(*scan_end) (TableScanDesc scan);

	/*
	 * Restart relation scan.  If set_params is set to true, allow{strat,
	 * sync, pagemode} (see scan_begin) changes should be taken into account.
	 */
	void		(*scan_rescan) (TableScanDesc scan, struct ScanKeyData *key, bool set_params,
								bool allow_strat, bool allow_sync, bool allow_pagemode);

	/*
	 * Return next tuple from `scan`, store in slot.
	 */
	bool		(*scan_getnextslot) (TableScanDesc scan,
									 ScanDirection direction, TupleTableSlot *slot);


	/* ------------------------------------------------------------------------
	 * Parallel table scan related functions.
	 * ------------------------------------------------------------------------
	 */

	/*
	 * Estimate the size of shared memory needed for a parallel scan of this
	 * relation. The snapshot does not need to be accounted for.
	 */
	Size		(*parallelscan_estimate) (Relation rel);

	/*
	 * Initialize ParallelTableScanDesc for a parallel scan of this relation.
	 * pscan will be sized according to parallelscan_estimate() for the same
	 * relation.
	 */
	Size		(*parallelscan_initialize) (Relation rel, ParallelTableScanDesc pscan);

	/*
	 * Reinitilize `pscan` for a new scan. `rel` will be the same relation as
	 * when `pscan` was initialized by parallelscan_initialize.
	 */
	void		(*parallelscan_reinitialize) (Relation rel, ParallelTableScanDesc pscan);


	/* ------------------------------------------------------------------------
	 * Index Scan Callbacks
	 * ------------------------------------------------------------------------
	 */

	/*
	 * Prepare to fetch tuples from the relation, as needed when fetching
	 * tuples for an index scan.  The callback has to return a
	 * IndexFetchTableData, which the AM will typically embed in a larger
	 * structure with additional information.
	 *
	 * Tuples for an index scan can then be fetched via index_fetch_tuple.
	 */
	struct IndexFetchTableData *(*index_fetch_begin) (Relation rel);

	/*
	 * Reset index fetch. Typically this will release cross index fetch
	 * resources held in IndexFetchTableData.
	 */
	void		(*index_fetch_reset) (struct IndexFetchTableData *data);

	/*
	 * Release resources and deallocate index fetch.
	 */
	void		(*index_fetch_end) (struct IndexFetchTableData *data);

	/*
	 * Fetch tuple at `tid` into `slot`, after doing a visibility test
	 * according to `snapshot`. If a tuple was found and passed the visibility
	 * test, return true, false otherwise.
	 *
	 * Note that AMs that do not necessarily update indexes when indexed
	 * columns do not change, need to return the current/correct version of a
	 * tuple as appropriate, even if the tid points to an older version of the
	 * tuple.
	 *
	 * *call_again is false on the first call to index_fetch_tuple for a tid.
	 * If there potentially is another tuple matching the tid, *call_again
	 * needs be set to true by index_fetch_tuple, signalling to the caller
	 * that index_fetch_tuple should be called again for the same tid.
	 *
	 * *all_dead should be set to true by index_fetch_tuple iff it is
	 * guaranteed that no backend needs to see that tuple. Index AMs can use
	 * that do avoid returning that tid in future searches.
	 */
	bool		(*index_fetch_tuple) (struct IndexFetchTableData *scan,
									  ItemPointer tid,
									  Snapshot snapshot,
									  TupleTableSlot *slot,
									  bool *call_again, bool *all_dead);

	/* ------------------------------------------------------------------------
	 * Callbacks for non-modifying operations on individual tuples
	 * ------------------------------------------------------------------------
	 */

	/*
	 * Does the tuple in `slot` satisfy `snapshot`?  The slot needs to be of
	 * the appropriate type for the AM.
	 */
	bool		(*tuple_satisfies_snapshot) (Relation rel,
											 TupleTableSlot *slot,
											 Snapshot snapshot);

} TableAmRoutine;


/* ----------------------------------------------------------------------------
 * Slot functions.
 * ----------------------------------------------------------------------------
 */

/*
 * Returns slot callbacks suitable for holding tuples of the appropriate type
 * for the relation.  Works for tables, views, foreign tables and partitioned
 * tables.
 */
extern const TupleTableSlotOps *table_slot_callbacks(Relation rel);

/*
 * Returns slot using the callbacks returned by table_slot_callbacks(), and
 * registers it on *reglist.
 */
extern TupleTableSlot *table_slot_create(Relation rel, List **reglist);


/* ----------------------------------------------------------------------------
 * Table scan functions.
 * ----------------------------------------------------------------------------
 */

/*
 * Start a scan of `rel`. Returned tuples pass a visibility test of
 * `snapshot`, and if nkeys != 0, the results are filtered by those scan keys.
 */
static inline TableScanDesc
table_beginscan(Relation rel, Snapshot snapshot,
				int nkeys, struct ScanKeyData *key)
{
	return rel->rd_tableam->scan_begin(rel, snapshot, nkeys, key, NULL,
									   true, true, true, false, false, false);
}

/*
 * Like table_beginscan(), but for scanning catalog. It'll automatically use a
 * snapshot appropriate for scanning catalog relations.
 */
extern TableScanDesc table_beginscan_catalog(Relation rel, int nkeys,
						struct ScanKeyData *key);

/*
 * Like table_beginscan(), but table_beginscan_strat() offers an extended API
 * that lets the caller control whether a nondefault buffer access strategy
 * can be used, and whether syncscan can be chosen (possibly resulting in the
 * scan not starting from block zero).  Both of these default to true with
 * plain table_beginscan.
 */
static inline TableScanDesc
table_beginscan_strat(Relation rel, Snapshot snapshot,
					  int nkeys, struct ScanKeyData *key,
					  bool allow_strat, bool allow_sync)
{
	return rel->rd_tableam->scan_begin(rel, snapshot, nkeys, key, NULL,
									   allow_strat, allow_sync, true,
									   false, false, false);
}


/*
 * table_beginscan_bm is an alternative entry point for setting up a
 * TableScanDesc for a bitmap heap scan.  Although that scan technology is
 * really quite unlike a standard seqscan, there is just enough commonality to
 * make it worth using the same data structure.
 */
static inline TableScanDesc
table_beginscan_bm(Relation rel, Snapshot snapshot,
				   int nkeys, struct ScanKeyData *key)
{
	return rel->rd_tableam->scan_begin(rel, snapshot, nkeys, key, NULL,
									   false, false, true, true, false, false);
}

/*
 * table_beginscan_sampling is an alternative entry point for setting up a
 * TableScanDesc for a TABLESAMPLE scan.  As with bitmap scans, it's worth
 * using the same data structure although the behavior is rather different.
 * In addition to the options offered by table_beginscan_strat, this call
 * also allows control of whether page-mode visibility checking is used.
 */
static inline TableScanDesc
table_beginscan_sampling(Relation rel, Snapshot snapshot,
						 int nkeys, struct ScanKeyData *key,
						 bool allow_strat, bool allow_sync, bool allow_pagemode)
{
	return rel->rd_tableam->scan_begin(rel, snapshot, nkeys, key, NULL,
									   allow_strat, allow_sync, allow_pagemode,
									   false, true, false);
}

/*
 * table_beginscan_analyze is an alternative entry point for setting up a
 * TableScanDesc for an ANALYZE scan.  As with bitmap scans, it's worth using
 * the same data structure although the behavior is rather different.
 */
static inline TableScanDesc
table_beginscan_analyze(Relation rel)
{
	return rel->rd_tableam->scan_begin(rel, NULL, 0, NULL, NULL,
									   true, false, true,
									   false, true, false);
}

/*
 * End relation scan.
 */
static inline void
table_endscan(TableScanDesc scan)
{
	scan->rs_rd->rd_tableam->scan_end(scan);
}


/*
 * Restart a relation scan.
 */
static inline void
table_rescan(TableScanDesc scan,
			 struct ScanKeyData *key)
{
	scan->rs_rd->rd_tableam->scan_rescan(scan, key, false, false, false, false);
}

/*
 * Restart a relation scan after changing params.
 *
 * This call allows changing the buffer strategy, syncscan, and pagemode
 * options before starting a fresh scan.  Note that although the actual use of
 * syncscan might change (effectively, enabling or disabling reporting), the
 * previously selected startblock will be kept.
 */
static inline void
table_rescan_set_params(TableScanDesc scan, struct ScanKeyData *key,
						bool allow_strat, bool allow_sync, bool allow_pagemode)
{
	scan->rs_rd->rd_tableam->scan_rescan(scan, key, true,
										 allow_strat, allow_sync,
										 allow_pagemode);
}

/*
 * Update snapshot used by the scan.
 */
extern void table_scan_update_snapshot(TableScanDesc scan, Snapshot snapshot);


/*
 * Return next tuple from `scan`, store in slot.
 */
static inline bool
table_scan_getnextslot(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot)
{
	slot->tts_tableOid = RelationGetRelid(sscan->rs_rd);
	return sscan->rs_rd->rd_tableam->scan_getnextslot(sscan, direction, slot);
}


/* ----------------------------------------------------------------------------
 * Parallel table scan related functions.
 * ----------------------------------------------------------------------------
 */

/*
 * Estimate the size of shared memory needed for a parallel scan of this
 * relation.
 */
extern Size table_parallelscan_estimate(Relation rel, Snapshot snapshot);

/*
 * Initialize ParallelTableScanDesc for a parallel scan of this
 * relation. `pscan` needs to be sized according to parallelscan_estimate()
 * for the same relation.  Call this just once in the leader process; then,
 * individual workers attach via table_beginscan_parallel.
 */
extern void table_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan, Snapshot snapshot);

/*
 * Begin a parallel scan. `pscan` needs to have been initialized with
 * table_parallelscan_initialize(), for the same relation. The initialization
 * does not need to have happened in this backend.
 *
 * Caller must hold a suitable lock on the correct relation.
 */
extern TableScanDesc table_beginscan_parallel(Relation rel, ParallelTableScanDesc pscan);

/*
 * Restart a parallel scan.  Call this in the leader process.  Caller is
 * responsible for making sure that all workers have finished the scan
 * beforehand.
 */
static inline void
table_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
	rel->rd_tableam->parallelscan_reinitialize(rel, pscan);
}


/* ----------------------------------------------------------------------------
 *  Index scan related functions.
 * ----------------------------------------------------------------------------
 */

/*
 * Prepare to fetch tuples from the relation, as needed when fetching tuples
 * for an index scan.
 *
 * Tuples for an index scan can then be fetched via table_index_fetch_tuple().
 */
static inline IndexFetchTableData *
table_index_fetch_begin(Relation rel)
{
	return rel->rd_tableam->index_fetch_begin(rel);
}

/*
 * Reset index fetch. Typically this will release cross index fetch resources
 * held in IndexFetchTableData.
 */
static inline void
table_index_fetch_reset(struct IndexFetchTableData *scan)
{
	scan->rel->rd_tableam->index_fetch_reset(scan);
}

/*
 * Release resources and deallocate index fetch.
 */
static inline void
table_index_fetch_end(struct IndexFetchTableData *scan)
{
	scan->rel->rd_tableam->index_fetch_end(scan);
}

/*
 * Fetches tuple at `tid` into `slot`, after doing a visibility test according
 * to `snapshot`. If a tuple was found and passed the visibility test, returns
 * true, false otherwise.
 *
 * *call_again needs to be false on the first call to table_index_fetch_tuple() for
 * a tid. If there potentially is another tuple matching the tid, *call_again
 * will be set to true, signalling that table_index_fetch_tuple() should be called
 * again for the same tid.
 *
 * *all_dead will be set to true by table_index_fetch_tuple() iff it is guaranteed
 * that no backend needs to see that tuple. Index AMs can use that do avoid
 * returning that tid in future searches.
 */
static inline bool
table_index_fetch_tuple(struct IndexFetchTableData *scan,
						ItemPointer tid,
						Snapshot snapshot,
						TupleTableSlot *slot,
						bool *call_again, bool *all_dead)
{

	return scan->rel->rd_tableam->index_fetch_tuple(scan, tid, snapshot,
													slot, call_again,
													all_dead);
}


/* ------------------------------------------------------------------------
 * Functions for non-modifying operations on individual tuples
 * ------------------------------------------------------------------------
 */

/*
 * Return true iff tuple in slot satisfies the snapshot.
 *
 * This assumes the slot's tuple is valid, and of the appropriate type for the
 * AM.
 *
 * Some AMs might modify the data underlying the tuple as a side-effect. If so
 * they ought to mark the relevant buffer dirty.
 */
static inline bool
table_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot, Snapshot snapshot)
{
	return rel->rd_tableam->tuple_satisfies_snapshot(rel, slot, snapshot);
}


/* ----------------------------------------------------------------------------
 * Helper functions to implement parallel scans for block oriented AMs.
 * ----------------------------------------------------------------------------
 */

extern Size table_block_parallelscan_estimate(Relation rel);
extern Size table_block_parallelscan_initialize(Relation rel,
									ParallelTableScanDesc pscan);
extern void table_block_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan);
extern BlockNumber table_block_parallelscan_nextpage(Relation rel, ParallelBlockTableScanDesc pbscan);
extern void table_block_parallelscan_startblock_init(Relation rel, ParallelBlockTableScanDesc pbscan);


/* ----------------------------------------------------------------------------
 * Functions in tableamapi.c
 * ----------------------------------------------------------------------------
 */

extern const TableAmRoutine *GetTableAmRoutine(Oid amhandler);
extern const TableAmRoutine *GetTableAmRoutineByAmId(Oid amoid);
extern const TableAmRoutine *GetHeapamTableAmRoutine(void);
extern bool check_default_table_access_method(char **newval, void **extra,
								  GucSource source);

#endif							/* TABLEAM_H */
