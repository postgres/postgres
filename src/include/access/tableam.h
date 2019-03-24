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


struct BulkInsertStateData;


/*
 * Result codes for table_{update,delete,lock}_tuple, and for visibility
 * routines inside table AMs.
 */
typedef enum TM_Result
{
	/*
	 * Signals that the action succeeded (i.e. update/delete performed, lock
	 * was acquired)
	 */
	TM_Ok,

	/* The affected tuple wasn't visible to the relevant snapshot */
	TM_Invisible,

	/* The affected tuple was already modified by the calling backend */
	TM_SelfModified,

	/*
	 * The affected tuple was updated by another transaction. This includes
	 * the case where tuple was moved to another partition.
	 */
	TM_Updated,

	/* The affected tuple was deleted by another transaction */
	TM_Deleted,

	/*
	 * The affected tuple is currently being modified by another session. This
	 * will only be returned if (update/delete/lock)_tuple are instructed not
	 * to wait.
	 */
	TM_BeingModified,

	/* lock couldn't be acquired, action skipped. Only used by lock_tuple */
	TM_WouldBlock
} TM_Result;


/*
 * When table_update, table_delete, or table_lock_tuple fail because the target
 * tuple is already outdated, they fill in this struct to provide information
 * to the caller about what happened.
 * ctid is the target's ctid link: it is the same as the target's TID if the
 * target was deleted, or the location of the replacement tuple if the target
 * was updated.
 * xmax is the outdating transaction's XID.  If the caller wants to visit the
 * replacement tuple, it must check that this matches before believing the
 * replacement is really a match.
 * cmax is the outdating command's CID, but only when the failure code is
 * TM_SelfModified (i.e., something in the current transaction outdated the
 * tuple); otherwise cmax is zero.  (We make this restriction because
 * HeapTupleHeaderGetCmax doesn't work for tuples outdated in other
 * transactions.)
 */
typedef struct TM_FailureData
{
	ItemPointerData ctid;
	TransactionId xmax;
	CommandId	cmax;
	bool		traversed;
} TM_FailureData;

/* "options" flag bits for table_insert */
#define TABLE_INSERT_SKIP_WAL		0x0001
#define TABLE_INSERT_SKIP_FSM		0x0002
#define TABLE_INSERT_FROZEN			0x0004
#define TABLE_INSERT_NO_LOGICAL		0x0008

/* flag bits fortable_lock_tuple */
/* Follow tuples whose update is in progress if lock modes don't conflict  */
#define TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS	(1 << 0)
/* Follow update chain and lock lastest version of tuple */
#define TUPLE_LOCK_FLAG_FIND_LAST_VERSION		(1 << 1)


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

	/* ------------------------------------------------------------------------
	 * Manipulations of physical tuples.
	 * ------------------------------------------------------------------------
	 */

	/* see table_insert() for reference about parameters */
	void		(*tuple_insert) (Relation rel, TupleTableSlot *slot, CommandId cid,
								 int options, struct BulkInsertStateData *bistate);

	/* see table_insert() for reference about parameters */
	void		(*tuple_insert_speculative) (Relation rel,
											 TupleTableSlot *slot,
											 CommandId cid,
											 int options,
											 struct BulkInsertStateData *bistate,
											 uint32 specToken);

	/* see table_insert() for reference about parameters */
	void		(*tuple_complete_speculative) (Relation rel,
											   TupleTableSlot *slot,
											   uint32 specToken,
											   bool succeeded);

	/* see table_insert() for reference about parameters */
	TM_Result	(*tuple_delete) (Relation rel,
								 ItemPointer tid,
								 CommandId cid,
								 Snapshot snapshot,
								 Snapshot crosscheck,
								 bool wait,
								 TM_FailureData *tmfd,
								 bool changingPart);

	/* see table_insert() for reference about parameters */
	TM_Result	(*tuple_update) (Relation rel,
								 ItemPointer otid,
								 TupleTableSlot *slot,
								 CommandId cid,
								 Snapshot snapshot,
								 Snapshot crosscheck,
								 bool wait,
								 TM_FailureData *tmfd,
								 LockTupleMode *lockmode,
								 bool *update_indexes);

	/* see table_insert() for reference about parameters */
	TM_Result	(*tuple_lock) (Relation rel,
							   ItemPointer tid,
							   Snapshot snapshot,
							   TupleTableSlot *slot,
							   CommandId cid,
							   LockTupleMode mode,
							   LockWaitPolicy wait_policy,
							   uint8 flags,
							   TM_FailureData *tmfd);

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
 *  Functions for manipulations of physical tuples.
 * ----------------------------------------------------------------------------
 */

/*
 * Insert a tuple from a slot into table AM routine.
 *
 * The options bitmask allows to specify options that allow to change the
 * behaviour of the AM. Several options might be ignored by AMs not supporting
 * them.
 *
 * If the TABLE_INSERT_SKIP_WAL option is specified, the new tuple will not
 * necessarily logged to WAL, even for a non-temp relation. It is the AMs
 * choice whether this optimization is supported.
 *
 * If the TABLE_INSERT_SKIP_FSM option is specified, AMs are free to not reuse
 * free space in the relation. This can save some cycles when we know the
 * relation is new and doesn't contain useful amounts of free space.  It's
 * commonly passed directly to RelationGetBufferForTuple, see for more info.
 *
 * TABLE_INSERT_FROZEN should only be specified for inserts into
 * relfilenodes created during the current subtransaction and when
 * there are no prior snapshots or pre-existing portals open.
 * This causes rows to be frozen, which is an MVCC violation and
 * requires explicit options chosen by user.
 *
 * TABLE_INSERT_NO_LOGICAL force-disables the emitting of logical decoding
 * information for the tuple. This should solely be used during table rewrites
 * where RelationIsLogicallyLogged(relation) is not yet accurate for the new
 * relation.
 *
 * Note that most of these options will be applied when inserting into the
 * heap's TOAST table, too, if the tuple requires any out-of-line data
 *
 *
 * The BulkInsertState object (if any; bistate can be NULL for default
 * behavior) is also just passed through to RelationGetBufferForTuple.
 *
 * On return the slot's tts_tid and tts_tableOid are updated to reflect the
 * insertion. But note that any toasting of fields within the slot is NOT
 * reflected in the slots contents.
 */
static inline void
table_insert(Relation rel, TupleTableSlot *slot, CommandId cid,
			 int options, struct BulkInsertStateData *bistate)
{
	rel->rd_tableam->tuple_insert(rel, slot, cid, options,
								  bistate);
}

/*
 * Perform a "speculative insertion". These can be backed out afterwards
 * without aborting the whole transaction.  Other sessions can wait for the
 * speculative insertion to be confirmed, turning it into a regular tuple, or
 * aborted, as if it never existed.  Speculatively inserted tuples behave as
 * "value locks" of short duration, used to implement INSERT .. ON CONFLICT.
 *
 * A transaction having performed a speculative insertion has to either abort,
 * or finish the speculative insertion with
 * table_complete_speculative(succeeded = ...).
 */
static inline void
table_insert_speculative(Relation rel, TupleTableSlot *slot, CommandId cid,
						 int options, struct BulkInsertStateData *bistate, uint32 specToken)
{
	rel->rd_tableam->tuple_insert_speculative(rel, slot, cid, options,
											  bistate, specToken);
}

/*
 * Complete "speculative insertion" started in the same transaction. If
 * succeeded is true, the tuple is fully inserted, if false, it's removed.
 */
static inline void
table_complete_speculative(Relation rel, TupleTableSlot *slot, uint32 specToken,
						   bool succeeded)
{
	rel->rd_tableam->tuple_complete_speculative(rel, slot, specToken,
												succeeded);
}

/*
 * Delete a tuple.
 *
 * NB: do not call this directly unless prepared to deal with
 * concurrent-update conditions.  Use simple_table_delete instead.
 *
 * Input parameters:
 *	relation - table to be modified (caller must hold suitable lock)
 *	tid - TID of tuple to be deleted
 *	cid - delete command ID (used for visibility test, and stored into
 *		cmax if successful)
 *	crosscheck - if not InvalidSnapshot, also check tuple against this
 *	wait - true if should wait for any conflicting update to commit/abort
 * Output parameters:
 *	tmfd - filled in failure cases (see below)
 *	changingPart - true iff the tuple is being moved to another partition
 *		table due to an update of the partition key. Otherwise, false.
 *
 * Normal, successful return value is TM_Ok, which
 * actually means we did delete it.  Failure return codes are
 * TM_SelfModified, TM_Updated, or TM_BeingModified
 * (the last only possible if wait == false).
 *
 * In the failure cases, the routine fills *tmfd with the tuple's t_ctid,
 * t_xmax, and, if possible, and, if possible, t_cmax.  See comments for
 * struct TM_FailureData for additional info.
 */
static inline TM_Result
table_delete(Relation rel, ItemPointer tid, CommandId cid,
			 Snapshot snapshot, Snapshot crosscheck, bool wait,
			 TM_FailureData *tmfd, bool changingPart)
{
	return rel->rd_tableam->tuple_delete(rel, tid, cid,
										 snapshot, crosscheck,
										 wait, tmfd, changingPart);
}

/*
 * Update a tuple.
 *
 * NB: do not call this directly unless you are prepared to deal with
 * concurrent-update conditions.  Use simple_table_update instead.
 *
 * Input parameters:
 *	relation - table to be modified (caller must hold suitable lock)
 *	otid - TID of old tuple to be replaced
 *	newtup - newly constructed tuple data to store
 *	cid - update command ID (used for visibility test, and stored into
 *		cmax/cmin if successful)
 *	crosscheck - if not InvalidSnapshot, also check old tuple against this
 *	wait - true if should wait for any conflicting update to commit/abort
 * Output parameters:
 *	tmfd - filled in failure cases (see below)
 *	lockmode - filled with lock mode acquired on tuple
 *  update_indexes - in success cases this is set to true if new index entries
 *		are required for this tuple
 *
 * Normal, successful return value is TM_Ok, which
 * actually means we *did* update it.  Failure return codes are
 * TM_SelfModified, TM_Updated, or TM_BeingModified
 * (the last only possible if wait == false).
 *
 * On success, the header fields of *newtup are updated to match the new
 * stored tuple; in particular, newtup->t_self is set to the TID where the
 * new tuple was inserted, and its HEAP_ONLY_TUPLE flag is set iff a HOT
 * update was done.  However, any TOAST changes in the new tuple's
 * data are not reflected into *newtup.
 *
 * In the failure cases, the routine fills *tmfd with the tuple's t_ctid,
 * t_xmax, and, if possible, t_cmax.  See comments for struct TM_FailureData
 * for additional info.
 */
static inline TM_Result
table_update(Relation rel, ItemPointer otid, TupleTableSlot *slot,
			 CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait,
			 TM_FailureData *tmfd, LockTupleMode *lockmode,
			 bool *update_indexes)
{
	return rel->rd_tableam->tuple_update(rel, otid, slot,
										 cid, snapshot, crosscheck,
										 wait, tmfd,
										 lockmode, update_indexes);
}

/*
 * Lock a tuple in the specified mode.
 *
 * Input parameters:
 *	relation: relation containing tuple (caller must hold suitable lock)
 *	tid: TID of tuple to lock
 *	snapshot: snapshot to use for visibility determinations
 *	cid: current command ID (used for visibility test, and stored into
 *		tuple's cmax if lock is successful)
 *	mode: lock mode desired
 *	wait_policy: what to do if tuple lock is not available
 *	flags:
 *		If TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS, follow the update chain to
 *		also lock descendant tuples if lock modes don't conflict.
 *		If TUPLE_LOCK_FLAG_FIND_LAST_VERSION, update chain and lock lastest
 *		version.
 *
 * Output parameters:
 *	*slot: contains the target tuple
 *	*tmfd: filled in failure cases (see below)
 *
 * Function result may be:
 *	TM_Ok: lock was successfully acquired
 *	TM_Invisible: lock failed because tuple was never visible to us
 *	TM_SelfModified: lock failed because tuple updated by self
 *	TM_Updated: lock failed because tuple updated by other xact
 *	TM_Deleted: lock failed because tuple deleted by other xact
 *	TM_WouldBlock: lock couldn't be acquired and wait_policy is skip
 *
 * In the failure cases other than TM_Invisible, the routine fills *tmfd with
 * the tuple's t_ctid, t_xmax, and, if possible, t_cmax.  See comments for
 * struct TM_FailureData for additional info.
 */
static inline TM_Result
table_lock_tuple(Relation rel, ItemPointer tid, Snapshot snapshot,
				 TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
				 LockWaitPolicy wait_policy, uint8 flags,
				 TM_FailureData *tmfd)
{
	return rel->rd_tableam->tuple_lock(rel, tid, snapshot, slot,
									   cid, mode, wait_policy,
									   flags, tmfd);
}


/* ----------------------------------------------------------------------------
 * Functions to make modifications a bit simpler.
 * ----------------------------------------------------------------------------
 */

extern void simple_table_insert(Relation rel, TupleTableSlot *slot);
extern void simple_table_delete(Relation rel, ItemPointer tid,
					Snapshot snapshot);
extern void simple_table_update(Relation rel, ItemPointer otid,
					TupleTableSlot *slot, Snapshot snapshot,
					bool *update_indexes);


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
