/*-------------------------------------------------------------------------
 *
 * heapam.h
 *	  POSTGRES heap access method definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/heapam.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HEAPAM_H
#define HEAPAM_H

#include "access/heapam_xlog.h"
#include "access/relation.h"	/* for backward compatibility */
#include "access/relscan.h"
#include "access/sdir.h"
#include "access/skey.h"
#include "access/table.h"		/* for backward compatibility */
#include "access/tableam.h"
#include "nodes/lockoptions.h"
#include "nodes/primnodes.h"
#include "storage/bufpage.h"
#include "storage/dsm.h"
#include "storage/lockdefs.h"
#include "storage/read_stream.h"
#include "storage/shm_toc.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"


/* "options" flag bits for heap_insert */
#define HEAP_INSERT_SKIP_FSM	TABLE_INSERT_SKIP_FSM
#define HEAP_INSERT_FROZEN		TABLE_INSERT_FROZEN
#define HEAP_INSERT_NO_LOGICAL	TABLE_INSERT_NO_LOGICAL
#define HEAP_INSERT_SPECULATIVE 0x0010

/* "options" flag bits for heap_page_prune_and_freeze */
#define HEAP_PAGE_PRUNE_MARK_UNUSED_NOW		(1 << 0)
#define HEAP_PAGE_PRUNE_FREEZE				(1 << 1)

typedef struct BulkInsertStateData *BulkInsertState;
struct TupleTableSlot;
struct VacuumCutoffs;

#define MaxLockTupleMode	LockTupleExclusive

/*
 * Descriptor for heap table scans.
 */
typedef struct HeapScanDescData
{
	TableScanDescData rs_base;	/* AM independent part of the descriptor */

	/* state set up at initscan time */
	BlockNumber rs_nblocks;		/* total number of blocks in rel */
	BlockNumber rs_startblock;	/* block # to start at */
	BlockNumber rs_numblocks;	/* max number of blocks to scan */
	/* rs_numblocks is usually InvalidBlockNumber, meaning "scan whole rel" */

	/* scan current state */
	bool		rs_inited;		/* false = scan not init'd yet */
	OffsetNumber rs_coffset;	/* current offset # in non-page-at-a-time mode */
	BlockNumber rs_cblock;		/* current block # in scan, if any */
	Buffer		rs_cbuf;		/* current buffer in scan, if any */
	/* NB: if rs_cbuf is not InvalidBuffer, we hold a pin on that buffer */

	BufferAccessStrategy rs_strategy;	/* access strategy for reads */

	HeapTupleData rs_ctup;		/* current tuple in scan, if any */

	/* For scans that stream reads */
	ReadStream *rs_read_stream;

	/*
	 * For sequential scans and TID range scans to stream reads. The read
	 * stream is allocated at the beginning of the scan and reset on rescan or
	 * when the scan direction changes. The scan direction is saved each time
	 * a new page is requested. If the scan direction changes from one page to
	 * the next, the read stream releases all previously pinned buffers and
	 * resets the prefetch block.
	 */
	ScanDirection rs_dir;
	BlockNumber rs_prefetch_block;

	/*
	 * For parallel scans to store page allocation data.  NULL when not
	 * performing a parallel scan.
	 */
	ParallelBlockTableScanWorkerData *rs_parallelworkerdata;

	/* these fields only used in page-at-a-time mode and for bitmap scans */
	uint32		rs_cindex;		/* current tuple's index in vistuples */
	uint32		rs_ntuples;		/* number of visible tuples on page */
	OffsetNumber rs_vistuples[MaxHeapTuplesPerPage];	/* their offsets */
} HeapScanDescData;
typedef struct HeapScanDescData *HeapScanDesc;

typedef struct BitmapHeapScanDescData
{
	HeapScanDescData rs_heap_base;

	/* Holds no data */
}			BitmapHeapScanDescData;
typedef struct BitmapHeapScanDescData *BitmapHeapScanDesc;

/*
 * Descriptor for fetches from heap via an index.
 */
typedef struct IndexFetchHeapData
{
	IndexFetchTableData xs_base;	/* AM independent part of the descriptor */

	Buffer		xs_cbuf;		/* current heap buffer in scan, if any */
	/* NB: if xs_cbuf is not InvalidBuffer, we hold a pin on that buffer */
} IndexFetchHeapData;

/* Result codes for HeapTupleSatisfiesVacuum */
typedef enum
{
	HEAPTUPLE_DEAD,				/* tuple is dead and deletable */
	HEAPTUPLE_LIVE,				/* tuple is live (committed, no deleter) */
	HEAPTUPLE_RECENTLY_DEAD,	/* tuple is dead, but not deletable yet */
	HEAPTUPLE_INSERT_IN_PROGRESS,	/* inserting xact is still in progress */
	HEAPTUPLE_DELETE_IN_PROGRESS,	/* deleting xact is still in progress */
} HTSV_Result;

/*
 * heap_prepare_freeze_tuple may request that heap_freeze_execute_prepared
 * check any tuple's to-be-frozen xmin and/or xmax status using pg_xact
 */
#define		HEAP_FREEZE_CHECK_XMIN_COMMITTED	0x01
#define		HEAP_FREEZE_CHECK_XMAX_ABORTED		0x02

/* heap_prepare_freeze_tuple state describing how to freeze a tuple */
typedef struct HeapTupleFreeze
{
	/* Fields describing how to process tuple */
	TransactionId xmax;
	uint16		t_infomask2;
	uint16		t_infomask;
	uint8		frzflags;

	/* xmin/xmax check flags */
	uint8		checkflags;
	/* Page offset number for tuple */
	OffsetNumber offset;
} HeapTupleFreeze;

/*
 * State used by VACUUM to track the details of freezing all eligible tuples
 * on a given heap page.
 *
 * VACUUM prepares freeze plans for each page via heap_prepare_freeze_tuple
 * calls (every tuple with storage gets its own call).  This page-level freeze
 * state is updated across each call, which ultimately determines whether or
 * not freezing the page is required.
 *
 * Aside from the basic question of whether or not freezing will go ahead, the
 * state also tracks the oldest extant XID/MXID in the table as a whole, for
 * the purposes of advancing relfrozenxid/relminmxid values in pg_class later
 * on.  Each heap_prepare_freeze_tuple call pushes NewRelfrozenXid and/or
 * NewRelminMxid back as required to avoid unsafe final pg_class values.  Any
 * and all unfrozen XIDs or MXIDs that remain after VACUUM finishes _must_
 * have values >= the final relfrozenxid/relminmxid values in pg_class.  This
 * includes XIDs that remain as MultiXact members from any tuple's xmax.
 *
 * When 'freeze_required' flag isn't set after all tuples are examined, the
 * final choice on freezing is made by vacuumlazy.c.  It can decide to trigger
 * freezing based on whatever criteria it deems appropriate.  However, it is
 * recommended that vacuumlazy.c avoid early freezing when freezing does not
 * enable setting the target page all-frozen in the visibility map afterwards.
 */
typedef struct HeapPageFreeze
{
	/* Is heap_prepare_freeze_tuple caller required to freeze page? */
	bool		freeze_required;

	/*
	 * "Freeze" NewRelfrozenXid/NewRelminMxid trackers.
	 *
	 * Trackers used when heap_freeze_execute_prepared freezes, or when there
	 * are zero freeze plans for a page.  It is always valid for vacuumlazy.c
	 * to freeze any page, by definition.  This even includes pages that have
	 * no tuples with storage to consider in the first place.  That way the
	 * 'totally_frozen' results from heap_prepare_freeze_tuple can always be
	 * used in the same way, even when no freeze plans need to be executed to
	 * "freeze the page".  Only the "freeze" path needs to consider the need
	 * to set pages all-frozen in the visibility map under this scheme.
	 *
	 * When we freeze a page, we generally freeze all XIDs < OldestXmin, only
	 * leaving behind XIDs that are ineligible for freezing, if any.  And so
	 * you might wonder why these trackers are necessary at all; why should
	 * _any_ page that VACUUM freezes _ever_ be left with XIDs/MXIDs that
	 * ratchet back the top-level NewRelfrozenXid/NewRelminMxid trackers?
	 *
	 * It is useful to use a definition of "freeze the page" that does not
	 * overspecify how MultiXacts are affected.  heap_prepare_freeze_tuple
	 * generally prefers to remove Multis eagerly, but lazy processing is used
	 * in cases where laziness allows VACUUM to avoid allocating a new Multi.
	 * The "freeze the page" trackers enable this flexibility.
	 */
	TransactionId FreezePageRelfrozenXid;
	MultiXactId FreezePageRelminMxid;

	/*
	 * "No freeze" NewRelfrozenXid/NewRelminMxid trackers.
	 *
	 * These trackers are maintained in the same way as the trackers used when
	 * VACUUM scans a page that isn't cleanup locked.  Both code paths are
	 * based on the same general idea (do less work for this page during the
	 * ongoing VACUUM, at the cost of having to accept older final values).
	 */
	TransactionId NoFreezePageRelfrozenXid;
	MultiXactId NoFreezePageRelminMxid;

} HeapPageFreeze;

/*
 * Per-page state returned by heap_page_prune_and_freeze()
 */
typedef struct PruneFreezeResult
{
	int			ndeleted;		/* Number of tuples deleted from the page */
	int			nnewlpdead;		/* Number of newly LP_DEAD items */
	int			nfrozen;		/* Number of tuples we froze */

	/* Number of live and recently dead tuples on the page, after pruning */
	int			live_tuples;
	int			recently_dead_tuples;

	/*
	 * all_visible and all_frozen indicate if the all-visible and all-frozen
	 * bits in the visibility map can be set for this page, after pruning.
	 *
	 * vm_conflict_horizon is the newest xmin of live tuples on the page.  The
	 * caller can use it as the conflict horizon when setting the VM bits.  It
	 * is only valid if we froze some tuples (nfrozen > 0), and all_frozen is
	 * true.
	 *
	 * These are only set if the HEAP_PRUNE_FREEZE option is set.
	 */
	bool		all_visible;
	bool		all_frozen;
	TransactionId vm_conflict_horizon;

	/*
	 * Whether or not the page makes rel truncation unsafe.  This is set to
	 * 'true', even if the page contains LP_DEAD items.  VACUUM will remove
	 * them before attempting to truncate.
	 */
	bool		hastup;

	/*
	 * LP_DEAD items on the page after pruning.  Includes existing LP_DEAD
	 * items.
	 */
	int			lpdead_items;
	OffsetNumber deadoffsets[MaxHeapTuplesPerPage];
} PruneFreezeResult;

/* 'reason' codes for heap_page_prune_and_freeze() */
typedef enum
{
	PRUNE_ON_ACCESS,			/* on-access pruning */
	PRUNE_VACUUM_SCAN,			/* VACUUM 1st heap pass */
	PRUNE_VACUUM_CLEANUP,		/* VACUUM 2nd heap pass */
} PruneReason;

/* ----------------
 *		function prototypes for heap access method
 *
 * heap_create, heap_create_with_catalog, and heap_drop_with_catalog
 * are declared in catalog/heap.h
 * ----------------
 */


/*
 * HeapScanIsValid
 *		True iff the heap scan is valid.
 */
#define HeapScanIsValid(scan) PointerIsValid(scan)

extern TableScanDesc heap_beginscan(Relation relation, Snapshot snapshot,
									int nkeys, ScanKey key,
									ParallelTableScanDesc parallel_scan,
									uint32 flags);
extern void heap_setscanlimits(TableScanDesc sscan, BlockNumber startBlk,
							   BlockNumber numBlks);
extern void heap_prepare_pagescan(TableScanDesc sscan);
extern void heap_rescan(TableScanDesc sscan, ScanKey key, bool set_params,
						bool allow_strat, bool allow_sync, bool allow_pagemode);
extern void heap_endscan(TableScanDesc sscan);
extern HeapTuple heap_getnext(TableScanDesc sscan, ScanDirection direction);
extern bool heap_getnextslot(TableScanDesc sscan,
							 ScanDirection direction, struct TupleTableSlot *slot);
extern void heap_set_tidrange(TableScanDesc sscan, ItemPointer mintid,
							  ItemPointer maxtid);
extern bool heap_getnextslot_tidrange(TableScanDesc sscan,
									  ScanDirection direction,
									  TupleTableSlot *slot);
extern bool heap_fetch(Relation relation, Snapshot snapshot,
					   HeapTuple tuple, Buffer *userbuf, bool keep_buf);
extern bool heap_hot_search_buffer(ItemPointer tid, Relation relation,
								   Buffer buffer, Snapshot snapshot, HeapTuple heapTuple,
								   bool *all_dead, bool first_call);

extern void heap_get_latest_tid(TableScanDesc sscan, ItemPointer tid);

extern BulkInsertState GetBulkInsertState(void);
extern void FreeBulkInsertState(BulkInsertState);
extern void ReleaseBulkInsertStatePin(BulkInsertState bistate);

extern void heap_insert(Relation relation, HeapTuple tup, CommandId cid,
						int options, BulkInsertState bistate);
extern void heap_multi_insert(Relation relation, struct TupleTableSlot **slots,
							  int ntuples, CommandId cid, int options,
							  BulkInsertState bistate);
extern TM_Result heap_delete(Relation relation, ItemPointer tid,
							 CommandId cid, Snapshot crosscheck, bool wait,
							 struct TM_FailureData *tmfd, bool changingPart);
extern void heap_finish_speculative(Relation relation, ItemPointer tid);
extern void heap_abort_speculative(Relation relation, ItemPointer tid);
extern TM_Result heap_update(Relation relation, ItemPointer otid,
							 HeapTuple newtup,
							 CommandId cid, Snapshot crosscheck, bool wait,
							 struct TM_FailureData *tmfd, LockTupleMode *lockmode,
							 TU_UpdateIndexes *update_indexes);
extern TM_Result heap_lock_tuple(Relation relation, HeapTuple tuple,
								 CommandId cid, LockTupleMode mode, LockWaitPolicy wait_policy,
								 bool follow_updates,
								 Buffer *buffer, struct TM_FailureData *tmfd);

extern bool heap_inplace_lock(Relation relation,
							  HeapTuple oldtup_ptr, Buffer buffer,
							  void (*release_callback) (void *), void *arg);
extern void heap_inplace_update_and_unlock(Relation relation,
										   HeapTuple oldtup, HeapTuple tuple,
										   Buffer buffer);
extern void heap_inplace_unlock(Relation relation,
								HeapTuple oldtup, Buffer buffer);
extern bool heap_prepare_freeze_tuple(HeapTupleHeader tuple,
									  const struct VacuumCutoffs *cutoffs,
									  HeapPageFreeze *pagefrz,
									  HeapTupleFreeze *frz, bool *totally_frozen);

extern void heap_pre_freeze_checks(Buffer buffer,
								   HeapTupleFreeze *tuples, int ntuples);
extern void heap_freeze_prepared_tuples(Buffer buffer,
										HeapTupleFreeze *tuples, int ntuples);
extern bool heap_freeze_tuple(HeapTupleHeader tuple,
							  TransactionId relfrozenxid, TransactionId relminmxid,
							  TransactionId FreezeLimit, TransactionId MultiXactCutoff);
extern bool heap_tuple_should_freeze(HeapTupleHeader tuple,
									 const struct VacuumCutoffs *cutoffs,
									 TransactionId *NoFreezePageRelfrozenXid,
									 MultiXactId *NoFreezePageRelminMxid);
extern bool heap_tuple_needs_eventual_freeze(HeapTupleHeader tuple);

extern void simple_heap_insert(Relation relation, HeapTuple tup);
extern void simple_heap_delete(Relation relation, ItemPointer tid);
extern void simple_heap_update(Relation relation, ItemPointer otid,
							   HeapTuple tup, TU_UpdateIndexes *update_indexes);

extern TransactionId heap_index_delete_tuples(Relation rel,
											  TM_IndexDeleteOp *delstate);

/* in heap/pruneheap.c */
struct GlobalVisState;
extern void heap_page_prune_opt(Relation relation, Buffer buffer);
extern void heap_page_prune_and_freeze(Relation relation, Buffer buffer,
									   struct GlobalVisState *vistest,
									   int options,
									   struct VacuumCutoffs *cutoffs,
									   PruneFreezeResult *presult,
									   PruneReason reason,
									   OffsetNumber *off_loc,
									   TransactionId *new_relfrozen_xid,
									   MultiXactId *new_relmin_mxid);
extern void heap_page_prune_execute(Buffer buffer, bool lp_truncate_only,
									OffsetNumber *redirected, int nredirected,
									OffsetNumber *nowdead, int ndead,
									OffsetNumber *nowunused, int nunused);
extern void heap_get_root_tuples(Page page, OffsetNumber *root_offsets);
extern void log_heap_prune_and_freeze(Relation relation, Buffer buffer,
									  TransactionId conflict_xid,
									  bool cleanup_lock,
									  PruneReason reason,
									  HeapTupleFreeze *frozen, int nfrozen,
									  OffsetNumber *redirected, int nredirected,
									  OffsetNumber *dead, int ndead,
									  OffsetNumber *unused, int nunused);

/* in heap/vacuumlazy.c */
struct VacuumParams;
extern void heap_vacuum_rel(Relation rel,
							struct VacuumParams *params, BufferAccessStrategy bstrategy);

/* in heap/heapam_visibility.c */
extern bool HeapTupleSatisfiesVisibility(HeapTuple htup, Snapshot snapshot,
										 Buffer buffer);
extern TM_Result HeapTupleSatisfiesUpdate(HeapTuple htup, CommandId curcid,
										  Buffer buffer);
extern HTSV_Result HeapTupleSatisfiesVacuum(HeapTuple htup, TransactionId OldestXmin,
											Buffer buffer);
extern HTSV_Result HeapTupleSatisfiesVacuumHorizon(HeapTuple htup, Buffer buffer,
												   TransactionId *dead_after);
extern void HeapTupleSetHintBits(HeapTupleHeader tuple, Buffer buffer,
								 uint16 infomask, TransactionId xid);
extern bool HeapTupleHeaderIsOnlyLocked(HeapTupleHeader tuple);
extern bool HeapTupleIsSurelyDead(HeapTuple htup,
								  struct GlobalVisState *vistest);

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
extern void HeapCheckForSerializableConflictOut(bool visible, Relation relation, HeapTuple tuple,
												Buffer buffer, Snapshot snapshot);

/*
 * heap_execute_freeze_tuple
 *		Execute the prepared freezing of a tuple with caller's freeze plan.
 *
 * Caller is responsible for ensuring that no other backend can access the
 * storage underlying this tuple, either by holding an exclusive lock on the
 * buffer containing it (which is what lazy VACUUM does), or by having it be
 * in private storage (which is what CLUSTER and friends do).
 */
static inline void
heap_execute_freeze_tuple(HeapTupleHeader tuple, HeapTupleFreeze *frz)
{
	HeapTupleHeaderSetXmax(tuple, frz->xmax);

	if (frz->frzflags & XLH_FREEZE_XVAC)
		HeapTupleHeaderSetXvac(tuple, FrozenTransactionId);

	if (frz->frzflags & XLH_INVALID_XVAC)
		HeapTupleHeaderSetXvac(tuple, InvalidTransactionId);

	tuple->t_infomask = frz->t_infomask;
	tuple->t_infomask2 = frz->t_infomask2;
}

#endif							/* HEAPAM_H */
