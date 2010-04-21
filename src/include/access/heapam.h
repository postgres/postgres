/*-------------------------------------------------------------------------
 *
 * heapam.h
 *	  POSTGRES heap access method definitions.
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/heapam.h,v 1.149 2010/04/21 17:20:56 sriggs Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HEAPAM_H
#define HEAPAM_H

#include "access/htup.h"
#include "access/sdir.h"
#include "access/skey.h"
#include "access/xlog.h"
#include "nodes/primnodes.h"
#include "storage/bufpage.h"
#include "storage/lock.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"


/* "options" flag bits for heap_insert */
#define HEAP_INSERT_SKIP_WAL	0x0001
#define HEAP_INSERT_SKIP_FSM	0x0002

typedef struct BulkInsertStateData *BulkInsertState;

typedef enum
{
	LockTupleShared,
	LockTupleExclusive
} LockTupleMode;


/* ----------------
 *		function prototypes for heap access method
 *
 * heap_create, heap_create_with_catalog, and heap_drop_with_catalog
 * are declared in catalog/heap.h
 * ----------------
 */

/* in heap/heapam.c */
extern Relation relation_open(Oid relationId, LOCKMODE lockmode);
extern Relation try_relation_open(Oid relationId, LOCKMODE lockmode);
extern Relation relation_openrv(const RangeVar *relation, LOCKMODE lockmode);
extern Relation try_relation_openrv(const RangeVar *relation, LOCKMODE lockmode);
extern void relation_close(Relation relation, LOCKMODE lockmode);

extern Relation heap_open(Oid relationId, LOCKMODE lockmode);
extern Relation heap_openrv(const RangeVar *relation, LOCKMODE lockmode);
extern Relation try_heap_openrv(const RangeVar *relation, LOCKMODE lockmode);

#define heap_close(r,l)  relation_close(r,l)

/* struct definition appears in relscan.h */
typedef struct HeapScanDescData *HeapScanDesc;

/*
 * HeapScanIsValid
 *		True iff the heap scan is valid.
 */
#define HeapScanIsValid(scan) PointerIsValid(scan)

extern HeapScanDesc heap_beginscan(Relation relation, Snapshot snapshot,
			   int nkeys, ScanKey key);
extern HeapScanDesc heap_beginscan_strat(Relation relation, Snapshot snapshot,
					 int nkeys, ScanKey key,
					 bool allow_strat, bool allow_sync);
extern HeapScanDesc heap_beginscan_bm(Relation relation, Snapshot snapshot,
				  int nkeys, ScanKey key);
extern void heap_rescan(HeapScanDesc scan, ScanKey key);
extern void heap_endscan(HeapScanDesc scan);
extern HeapTuple heap_getnext(HeapScanDesc scan, ScanDirection direction);

extern bool heap_fetch(Relation relation, Snapshot snapshot,
		   HeapTuple tuple, Buffer *userbuf, bool keep_buf,
		   Relation stats_relation);
extern bool heap_hot_search_buffer(ItemPointer tid, Buffer buffer,
					   Snapshot snapshot, bool *all_dead);
extern bool heap_hot_search(ItemPointer tid, Relation relation,
				Snapshot snapshot, bool *all_dead);

extern void heap_get_latest_tid(Relation relation, Snapshot snapshot,
					ItemPointer tid);
extern void setLastTid(const ItemPointer tid);

extern BulkInsertState GetBulkInsertState(void);
extern void FreeBulkInsertState(BulkInsertState);

extern Oid heap_insert(Relation relation, HeapTuple tup, CommandId cid,
			int options, BulkInsertState bistate);
extern HTSU_Result heap_delete(Relation relation, ItemPointer tid,
			ItemPointer ctid, TransactionId *update_xmax,
			CommandId cid, Snapshot crosscheck, bool wait);
extern HTSU_Result heap_update(Relation relation, ItemPointer otid,
			HeapTuple newtup,
			ItemPointer ctid, TransactionId *update_xmax,
			CommandId cid, Snapshot crosscheck, bool wait);
extern HTSU_Result heap_lock_tuple(Relation relation, HeapTuple tuple,
				Buffer *buffer, ItemPointer ctid,
				TransactionId *update_xmax, CommandId cid,
				LockTupleMode mode, bool nowait);
extern void heap_inplace_update(Relation relation, HeapTuple tuple);
extern bool heap_freeze_tuple(HeapTupleHeader tuple, TransactionId cutoff_xid,
				  Buffer buf);

extern Oid	simple_heap_insert(Relation relation, HeapTuple tup);
extern void simple_heap_delete(Relation relation, ItemPointer tid);
extern void simple_heap_update(Relation relation, ItemPointer otid,
				   HeapTuple tup);

extern void heap_markpos(HeapScanDesc scan);
extern void heap_restrpos(HeapScanDesc scan);

extern void heap_sync(Relation relation);

extern void heap_redo(XLogRecPtr lsn, XLogRecord *rptr);
extern void heap_desc(StringInfo buf, uint8 xl_info, char *rec);
extern void heap2_redo(XLogRecPtr lsn, XLogRecord *rptr);
extern void heap2_desc(StringInfo buf, uint8 xl_info, char *rec);

extern XLogRecPtr log_heap_cleanup_info(RelFileNode rnode,
					  TransactionId latestRemovedXid);
extern XLogRecPtr log_heap_clean(Relation reln, Buffer buffer,
			   OffsetNumber *redirected, int nredirected,
			   OffsetNumber *nowdead, int ndead,
			   OffsetNumber *nowunused, int nunused,
			   TransactionId latestRemovedXid);
extern XLogRecPtr log_heap_freeze(Relation reln, Buffer buffer,
				TransactionId cutoff_xid,
				OffsetNumber *offsets, int offcnt);
extern XLogRecPtr log_newpage(RelFileNode *rnode, ForkNumber forkNum,
			BlockNumber blk, Page page);

/* in heap/pruneheap.c */
extern void heap_page_prune_opt(Relation relation, Buffer buffer,
					TransactionId OldestXmin);
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

#endif   /* HEAPAM_H */
