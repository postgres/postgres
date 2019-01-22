/*-------------------------------------------------------------------------
 *
 * heapam.h
 *	  POSTGRES heap access method definitions.
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/heapam.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HEAPAM_H
#define HEAPAM_H

#include "access/relation.h"	/* for backward compatibility */
#include "access/sdir.h"
#include "access/skey.h"
#include "access/table.h"		/* for backward compatibility */
#include "nodes/lockoptions.h"
#include "nodes/primnodes.h"
#include "storage/bufpage.h"
#include "storage/lockdefs.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"


/* "options" flag bits for heap_insert */
#define HEAP_INSERT_SKIP_WAL	0x0001
#define HEAP_INSERT_SKIP_FSM	0x0002
#define HEAP_INSERT_FROZEN		0x0004
#define HEAP_INSERT_SPECULATIVE 0x0008
#define HEAP_INSERT_NO_LOGICAL	0x0010

typedef struct BulkInsertStateData *BulkInsertState;

#define MaxLockTupleMode	LockTupleExclusive

/*
 * When heap_update, heap_delete, or heap_lock_tuple fail because the target
 * tuple is already outdated, they fill in this struct to provide information
 * to the caller about what happened.
 * ctid is the target's ctid link: it is the same as the target's TID if the
 * target was deleted, or the location of the replacement tuple if the target
 * was updated.
 * xmax is the outdating transaction's XID.  If the caller wants to visit the
 * replacement tuple, it must check that this matches before believing the
 * replacement is really a match.
 * cmax is the outdating command's CID, but only when the failure code is
 * HeapTupleSelfUpdated (i.e., something in the current transaction outdated
 * the tuple); otherwise cmax is zero.  (We make this restriction because
 * HeapTupleHeaderGetCmax doesn't work for tuples outdated in other
 * transactions.)
 */
typedef struct HeapUpdateFailureData
{
	ItemPointerData ctid;
	TransactionId xmax;
	CommandId	cmax;
} HeapUpdateFailureData;

/* Result codes for HeapTupleSatisfiesVacuum */
typedef enum
{
	HEAPTUPLE_DEAD,				/* tuple is dead and deletable */
	HEAPTUPLE_LIVE,				/* tuple is live (committed, no deleter) */
	HEAPTUPLE_RECENTLY_DEAD,	/* tuple is dead, but not deletable yet */
	HEAPTUPLE_INSERT_IN_PROGRESS,	/* inserting xact is still in progress */
	HEAPTUPLE_DELETE_IN_PROGRESS	/* deleting xact is still in progress */
} HTSV_Result;

/* ----------------
 *		function prototypes for heap access method
 *
 * heap_create, heap_create_with_catalog, and heap_drop_with_catalog
 * are declared in catalog/heap.h
 * ----------------
 */


/* struct definitions appear in relscan.h */
typedef struct HeapScanDescData *HeapScanDesc;
typedef struct ParallelHeapScanDescData *ParallelHeapScanDesc;

/*
 * HeapScanIsValid
 *		True iff the heap scan is valid.
 */
#define HeapScanIsValid(scan) PointerIsValid(scan)

extern HeapScanDesc heap_beginscan(Relation relation, Snapshot snapshot,
			   int nkeys, ScanKey key);
extern HeapScanDesc heap_beginscan_catalog(Relation relation, int nkeys,
					   ScanKey key);
extern HeapScanDesc heap_beginscan_strat(Relation relation, Snapshot snapshot,
					 int nkeys, ScanKey key,
					 bool allow_strat, bool allow_sync);
extern HeapScanDesc heap_beginscan_bm(Relation relation, Snapshot snapshot,
				  int nkeys, ScanKey key);
extern HeapScanDesc heap_beginscan_sampling(Relation relation,
						Snapshot snapshot, int nkeys, ScanKey key,
						bool allow_strat, bool allow_sync, bool allow_pagemode);
extern void heap_setscanlimits(HeapScanDesc scan, BlockNumber startBlk,
				   BlockNumber endBlk);
extern void heapgetpage(HeapScanDesc scan, BlockNumber page);
extern void heap_rescan(HeapScanDesc scan, ScanKey key);
extern void heap_rescan_set_params(HeapScanDesc scan, ScanKey key,
					   bool allow_strat, bool allow_sync, bool allow_pagemode);
extern void heap_endscan(HeapScanDesc scan);
extern HeapTuple heap_getnext(HeapScanDesc scan, ScanDirection direction);

extern Size heap_parallelscan_estimate(Snapshot snapshot);
extern void heap_parallelscan_initialize(ParallelHeapScanDesc target,
							 Relation relation, Snapshot snapshot);
extern void heap_parallelscan_reinitialize(ParallelHeapScanDesc parallel_scan);
extern HeapScanDesc heap_beginscan_parallel(Relation, ParallelHeapScanDesc);

extern bool heap_fetch(Relation relation, Snapshot snapshot,
		   HeapTuple tuple, Buffer *userbuf, bool keep_buf,
		   Relation stats_relation);
extern bool heap_hot_search_buffer(ItemPointer tid, Relation relation,
					   Buffer buffer, Snapshot snapshot, HeapTuple heapTuple,
					   bool *all_dead, bool first_call);
extern bool heap_hot_search(ItemPointer tid, Relation relation,
				Snapshot snapshot, bool *all_dead);

extern void heap_get_latest_tid(Relation relation, Snapshot snapshot,
					ItemPointer tid);
extern void setLastTid(const ItemPointer tid);

extern BulkInsertState GetBulkInsertState(void);
extern void FreeBulkInsertState(BulkInsertState);
extern void ReleaseBulkInsertStatePin(BulkInsertState bistate);

extern void heap_insert(Relation relation, HeapTuple tup, CommandId cid,
			int options, BulkInsertState bistate);
extern void heap_multi_insert(Relation relation, HeapTuple *tuples, int ntuples,
				  CommandId cid, int options, BulkInsertState bistate);
extern HTSU_Result heap_delete(Relation relation, ItemPointer tid,
			CommandId cid, Snapshot crosscheck, bool wait,
			HeapUpdateFailureData *hufd, bool changingPart);
extern void heap_finish_speculative(Relation relation, HeapTuple tuple);
extern void heap_abort_speculative(Relation relation, HeapTuple tuple);
extern HTSU_Result heap_update(Relation relation, ItemPointer otid,
			HeapTuple newtup,
			CommandId cid, Snapshot crosscheck, bool wait,
			HeapUpdateFailureData *hufd, LockTupleMode *lockmode);
extern HTSU_Result heap_lock_tuple(Relation relation, HeapTuple tuple,
				CommandId cid, LockTupleMode mode, LockWaitPolicy wait_policy,
				bool follow_update,
				Buffer *buffer, HeapUpdateFailureData *hufd);
extern void heap_inplace_update(Relation relation, HeapTuple tuple);
extern bool heap_freeze_tuple(HeapTupleHeader tuple,
				  TransactionId relfrozenxid, TransactionId relminmxid,
				  TransactionId cutoff_xid, TransactionId cutoff_multi);
extern bool heap_tuple_needs_freeze(HeapTupleHeader tuple, TransactionId cutoff_xid,
						MultiXactId cutoff_multi, Buffer buf);
extern bool heap_tuple_needs_eventual_freeze(HeapTupleHeader tuple);

extern void simple_heap_insert(Relation relation, HeapTuple tup);
extern void simple_heap_delete(Relation relation, ItemPointer tid);
extern void simple_heap_update(Relation relation, ItemPointer otid,
				   HeapTuple tup);

extern void heap_sync(Relation relation);
extern void heap_update_snapshot(HeapScanDesc scan, Snapshot snapshot);

/* in heap/pruneheap.c */
extern void heap_page_prune_opt(Relation relation, Buffer buffer);
extern int heap_page_prune(Relation relation, Buffer buffer,
				TransactionId OldestXmin,
				bool report_stats, TransactionId *latestRemovedXid);
extern void heap_page_prune_execute(Buffer buffer,
						OffsetNumber *redirected, int nredirected,
						OffsetNumber *nowdead, int ndead,
						OffsetNumber *nowunused, int nunused);
extern void heap_get_root_tuples(Page page, OffsetNumber *root_offsets);

/* in heap/syncscan.c */
extern void ss_report_location(Relation rel, BlockNumber location);
extern BlockNumber ss_get_location(Relation rel, BlockNumber relnblocks);
extern void SyncScanShmemInit(void);
extern Size SyncScanShmemSize(void);

/* in heap/vacuumlazy.c */
struct VacuumParams;
extern void heap_vacuum_rel(Relation onerel, int options,
				struct VacuumParams *params, BufferAccessStrategy bstrategy);

/* in heap/heapam_visibility.c */
extern bool HeapTupleSatisfiesVisibility(HeapTuple stup, Snapshot snapshot,
										 Buffer buffer);
extern HTSU_Result HeapTupleSatisfiesUpdate(HeapTuple stup, CommandId curcid,
						 Buffer buffer);
extern HTSV_Result HeapTupleSatisfiesVacuum(HeapTuple stup, TransactionId OldestXmin,
						 Buffer buffer);
extern void HeapTupleSetHintBits(HeapTupleHeader tuple, Buffer buffer,
					 uint16 infomask, TransactionId xid);
extern bool HeapTupleHeaderIsOnlyLocked(HeapTupleHeader tuple);
extern bool XidInMVCCSnapshot(TransactionId xid, Snapshot snapshot);
extern bool HeapTupleIsSurelyDead(HeapTuple htup, TransactionId OldestXmin);

/*
 * To avoid leaking too much knowledge about reorderbuffer implementation
 * details this is implemented in reorderbuffer.c not heapam_visibility.c
 */
struct HTAB;
extern bool ResolveCminCmaxDuringDecoding(struct HTAB *tuplecid_data,
							  Snapshot snapshot,
							  HeapTuple htup,
							  Buffer buffer,
							  CommandId *cmin, CommandId *cmax);

#endif							/* HEAPAM_H */
