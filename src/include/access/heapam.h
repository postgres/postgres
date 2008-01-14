/*-------------------------------------------------------------------------
 *
 * heapam.h
 *	  POSTGRES heap access method definitions.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/heapam.h,v 1.130 2008/01/14 01:39:09 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HEAPAM_H
#define HEAPAM_H

#include "access/htup.h"
#include "access/relscan.h"
#include "access/sdir.h"
#include "access/tupmacs.h"
#include "access/xlogutils.h"
#include "nodes/primnodes.h"
#include "storage/block.h"
#include "storage/lmgr.h"
#include "utils/rel.h"
#include "utils/tqual.h"

/* ----------------
 *		fastgetattr
 *
 *		Fetch a user attribute's value as a Datum (might be either a
 *		value, or a pointer into the data area of the tuple).
 *
 *		This must not be used when a system attribute might be requested.
 *		Furthermore, the passed attnum MUST be valid.  Use heap_getattr()
 *		instead, if in doubt.
 *
 *		This gets called many times, so we macro the cacheable and NULL
 *		lookups, and call nocachegetattr() for the rest.
 * ----------------
 */

#if !defined(DISABLE_COMPLEX_MACRO)

#define fastgetattr(tup, attnum, tupleDesc, isnull)					\
(																	\
	AssertMacro((attnum) > 0),										\
	(((isnull) != NULL) ? (*(isnull) = false) : (dummyret)NULL),				\
	HeapTupleNoNulls(tup) ?											\
	(																\
		(tupleDesc)->attrs[(attnum)-1]->attcacheoff >= 0 ?			\
		(															\
			fetchatt((tupleDesc)->attrs[(attnum)-1],				\
				(char *) (tup)->t_data + (tup)->t_data->t_hoff +	\
					(tupleDesc)->attrs[(attnum)-1]->attcacheoff)	\
		)															\
		:															\
			nocachegetattr((tup), (attnum), (tupleDesc), (isnull))	\
	)																\
	:																\
	(																\
		att_isnull((attnum)-1, (tup)->t_data->t_bits) ?				\
		(															\
			(((isnull) != NULL) ? (*(isnull) = true) : (dummyret)NULL),		\
			(Datum)NULL												\
		)															\
		:															\
		(															\
			nocachegetattr((tup), (attnum), (tupleDesc), (isnull))	\
		)															\
	)																\
)
#else							/* defined(DISABLE_COMPLEX_MACRO) */

extern Datum fastgetattr(HeapTuple tup, int attnum, TupleDesc tupleDesc,
			bool *isnull);
#endif   /* defined(DISABLE_COMPLEX_MACRO) */


/* ----------------
 *		heap_getattr
 *
 *		Extract an attribute of a heap tuple and return it as a Datum.
 *		This works for either system or user attributes.  The given attnum
 *		is properly range-checked.
 *
 *		If the field in question has a NULL value, we return a zero Datum
 *		and set *isnull == true.  Otherwise, we set *isnull == false.
 *
 *		<tup> is the pointer to the heap tuple.  <attnum> is the attribute
 *		number of the column (field) caller wants.	<tupleDesc> is a
 *		pointer to the structure describing the row and all its fields.
 * ----------------
 */
#define heap_getattr(tup, attnum, tupleDesc, isnull) \
( \
	AssertMacro((tup) != NULL), \
	( \
		((attnum) > 0) ? \
		( \
			((attnum) > (int) HeapTupleHeaderGetNatts((tup)->t_data)) ? \
			( \
				(((isnull) != NULL) ? (*(isnull) = true) : (dummyret)NULL), \
				(Datum)NULL \
			) \
			: \
				fastgetattr((tup), (attnum), (tupleDesc), (isnull)) \
		) \
		: \
			heap_getsysattr((tup), (attnum), (tupleDesc), (isnull)) \
	) \
)


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
extern Relation relation_open_nowait(Oid relationId, LOCKMODE lockmode);
extern Relation relation_openrv(const RangeVar *relation, LOCKMODE lockmode);
extern void relation_close(Relation relation, LOCKMODE lockmode);

extern Relation heap_open(Oid relationId, LOCKMODE lockmode);
extern Relation heap_openrv(const RangeVar *relation, LOCKMODE lockmode);

#define heap_close(r,l)  relation_close(r,l)

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
extern bool heap_release_fetch(Relation relation, Snapshot snapshot,
				   HeapTuple tuple, Buffer *userbuf, bool keep_buf,
				   Relation stats_relation);
extern bool heap_hot_search_buffer(ItemPointer tid, Buffer buffer,
					   Snapshot snapshot, bool *all_dead);
extern bool heap_hot_search(ItemPointer tid, Relation relation,
				Snapshot snapshot, bool *all_dead);

extern void heap_get_latest_tid(Relation relation, Snapshot snapshot,
					ItemPointer tid);
extern void setLastTid(const ItemPointer tid);

extern Oid heap_insert(Relation relation, HeapTuple tup, CommandId cid,
			bool use_wal, bool use_fsm);
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

extern XLogRecPtr log_heap_move(Relation reln, Buffer oldbuf,
			  ItemPointerData from,
			  Buffer newbuf, HeapTuple newtup);
extern XLogRecPtr log_heap_clean(Relation reln, Buffer buffer,
			   OffsetNumber *redirected, int nredirected,
			   OffsetNumber *nowdead, int ndead,
			   OffsetNumber *nowunused, int nunused,
			   bool redirect_move);
extern XLogRecPtr log_heap_freeze(Relation reln, Buffer buffer,
				TransactionId cutoff_xid,
				OffsetNumber *offsets, int offcnt);
extern XLogRecPtr log_newpage(RelFileNode *rnode, BlockNumber blk, Page page);

/* in common/heaptuple.c */
extern Size heap_compute_data_size(TupleDesc tupleDesc,
					   Datum *values, bool *isnull);
extern void heap_fill_tuple(TupleDesc tupleDesc,
				Datum *values, bool *isnull,
				char *data, Size data_size,
				uint16 *infomask, bits8 *bit);
extern bool heap_attisnull(HeapTuple tup, int attnum);
extern Datum nocachegetattr(HeapTuple tup, int attnum,
			   TupleDesc att, bool *isnull);
extern Datum heap_getsysattr(HeapTuple tup, int attnum, TupleDesc tupleDesc,
				bool *isnull);
extern HeapTuple heap_copytuple(HeapTuple tuple);
extern void heap_copytuple_with_tuple(HeapTuple src, HeapTuple dest);
extern HeapTuple heap_form_tuple(TupleDesc tupleDescriptor,
				Datum *values, bool *isnull);
extern HeapTuple heap_formtuple(TupleDesc tupleDescriptor,
			   Datum *values, char *nulls);
extern HeapTuple heap_modify_tuple(HeapTuple tuple,
				  TupleDesc tupleDesc,
				  Datum *replValues,
				  bool *replIsnull,
				  bool *doReplace);
extern HeapTuple heap_modifytuple(HeapTuple tuple,
				 TupleDesc tupleDesc,
				 Datum *replValues,
				 char *replNulls,
				 char *replActions);
extern void heap_deform_tuple(HeapTuple tuple, TupleDesc tupleDesc,
				  Datum *values, bool *isnull);
extern void heap_deformtuple(HeapTuple tuple, TupleDesc tupleDesc,
				 Datum *values, char *nulls);
extern void heap_freetuple(HeapTuple htup);
extern MinimalTuple heap_form_minimal_tuple(TupleDesc tupleDescriptor,
						Datum *values, bool *isnull);
extern void heap_free_minimal_tuple(MinimalTuple mtup);
extern MinimalTuple heap_copy_minimal_tuple(MinimalTuple mtup);
extern HeapTuple heap_tuple_from_minimal_tuple(MinimalTuple mtup);
extern MinimalTuple minimal_tuple_from_heap_tuple(HeapTuple htup);
extern HeapTuple heap_addheader(int natts, bool withoid,
			   Size structlen, void *structure);

/* in heap/pruneheap.c */
extern void heap_page_prune_opt(Relation relation, Buffer buffer,
					TransactionId OldestXmin);
extern int heap_page_prune(Relation relation, Buffer buffer,
				TransactionId OldestXmin,
				bool redirect_move, bool report_stats);
extern void heap_get_root_tuples(Page page, OffsetNumber *root_offsets);

/* in heap/syncscan.c */
extern void ss_report_location(Relation rel, BlockNumber location);
extern BlockNumber ss_get_location(Relation rel, BlockNumber relnblocks);
extern void SyncScanShmemInit(void);
extern Size SyncScanShmemSize(void);

#endif   /* HEAPAM_H */
