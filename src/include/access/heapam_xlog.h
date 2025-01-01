/*-------------------------------------------------------------------------
 *
 * heapam_xlog.h
 *	  POSTGRES heap access XLOG definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/heapam_xlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HEAPAM_XLOG_H
#define HEAPAM_XLOG_H

#include "access/htup.h"
#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/buf.h"
#include "storage/bufpage.h"
#include "storage/relfilelocator.h"
#include "storage/sinval.h"
#include "utils/relcache.h"


/*
 * WAL record definitions for heapam.c's WAL operations
 *
 * XLOG allows to store some information in high 4 bits of log
 * record xl_info field.  We use 3 for opcode and one for init bit.
 */
#define XLOG_HEAP_INSERT		0x00
#define XLOG_HEAP_DELETE		0x10
#define XLOG_HEAP_UPDATE		0x20
#define XLOG_HEAP_TRUNCATE		0x30
#define XLOG_HEAP_HOT_UPDATE	0x40
#define XLOG_HEAP_CONFIRM		0x50
#define XLOG_HEAP_LOCK			0x60
#define XLOG_HEAP_INPLACE		0x70

#define XLOG_HEAP_OPMASK		0x70
/*
 * When we insert 1st item on new page in INSERT, UPDATE, HOT_UPDATE,
 * or MULTI_INSERT, we can (and we do) restore entire page in redo
 */
#define XLOG_HEAP_INIT_PAGE		0x80
/*
 * We ran out of opcodes, so heapam.c now has a second RmgrId.  These opcodes
 * are associated with RM_HEAP2_ID, but are not logically different from
 * the ones above associated with RM_HEAP_ID.  XLOG_HEAP_OPMASK applies to
 * these, too.
 *
 * There's no difference between XLOG_HEAP2_PRUNE_ON_ACCESS,
 * XLOG_HEAP2_PRUNE_VACUUM_SCAN and XLOG_HEAP2_PRUNE_VACUUM_CLEANUP records.
 * They have separate opcodes just for debugging and analysis purposes, to
 * indicate why the WAL record was emitted.
 */
#define XLOG_HEAP2_REWRITE		0x00
#define XLOG_HEAP2_PRUNE_ON_ACCESS		0x10
#define XLOG_HEAP2_PRUNE_VACUUM_SCAN	0x20
#define XLOG_HEAP2_PRUNE_VACUUM_CLEANUP	0x30
#define XLOG_HEAP2_VISIBLE		0x40
#define XLOG_HEAP2_MULTI_INSERT 0x50
#define XLOG_HEAP2_LOCK_UPDATED 0x60
#define XLOG_HEAP2_NEW_CID		0x70

/*
 * xl_heap_insert/xl_heap_multi_insert flag values, 8 bits are available.
 */
/* PD_ALL_VISIBLE was cleared */
#define XLH_INSERT_ALL_VISIBLE_CLEARED			(1<<0)
#define XLH_INSERT_LAST_IN_MULTI				(1<<1)
#define XLH_INSERT_IS_SPECULATIVE				(1<<2)
#define XLH_INSERT_CONTAINS_NEW_TUPLE			(1<<3)
#define XLH_INSERT_ON_TOAST_RELATION			(1<<4)

/* all_frozen_set always implies all_visible_set */
#define XLH_INSERT_ALL_FROZEN_SET				(1<<5)

/*
 * xl_heap_update flag values, 8 bits are available.
 */
/* PD_ALL_VISIBLE was cleared */
#define XLH_UPDATE_OLD_ALL_VISIBLE_CLEARED		(1<<0)
/* PD_ALL_VISIBLE was cleared in the 2nd page */
#define XLH_UPDATE_NEW_ALL_VISIBLE_CLEARED		(1<<1)
#define XLH_UPDATE_CONTAINS_OLD_TUPLE			(1<<2)
#define XLH_UPDATE_CONTAINS_OLD_KEY				(1<<3)
#define XLH_UPDATE_CONTAINS_NEW_TUPLE			(1<<4)
#define XLH_UPDATE_PREFIX_FROM_OLD				(1<<5)
#define XLH_UPDATE_SUFFIX_FROM_OLD				(1<<6)

/* convenience macro for checking whether any form of old tuple was logged */
#define XLH_UPDATE_CONTAINS_OLD						\
	(XLH_UPDATE_CONTAINS_OLD_TUPLE | XLH_UPDATE_CONTAINS_OLD_KEY)

/*
 * xl_heap_delete flag values, 8 bits are available.
 */
/* PD_ALL_VISIBLE was cleared */
#define XLH_DELETE_ALL_VISIBLE_CLEARED			(1<<0)
#define XLH_DELETE_CONTAINS_OLD_TUPLE			(1<<1)
#define XLH_DELETE_CONTAINS_OLD_KEY				(1<<2)
#define XLH_DELETE_IS_SUPER						(1<<3)
#define XLH_DELETE_IS_PARTITION_MOVE			(1<<4)

/* convenience macro for checking whether any form of old tuple was logged */
#define XLH_DELETE_CONTAINS_OLD						\
	(XLH_DELETE_CONTAINS_OLD_TUPLE | XLH_DELETE_CONTAINS_OLD_KEY)

/* This is what we need to know about delete */
typedef struct xl_heap_delete
{
	TransactionId xmax;			/* xmax of the deleted tuple */
	OffsetNumber offnum;		/* deleted tuple's offset */
	uint8		infobits_set;	/* infomask bits */
	uint8		flags;
} xl_heap_delete;

#define SizeOfHeapDelete	(offsetof(xl_heap_delete, flags) + sizeof(uint8))

/*
 * xl_heap_truncate flag values, 8 bits are available.
 */
#define XLH_TRUNCATE_CASCADE					(1<<0)
#define XLH_TRUNCATE_RESTART_SEQS				(1<<1)

/*
 * For truncate we list all truncated relids in an array, followed by all
 * sequence relids that need to be restarted, if any.
 * All rels are always within the same database, so we just list dbid once.
 */
typedef struct xl_heap_truncate
{
	Oid			dbId;
	uint32		nrelids;
	uint8		flags;
	Oid			relids[FLEXIBLE_ARRAY_MEMBER];
} xl_heap_truncate;

#define SizeOfHeapTruncate	(offsetof(xl_heap_truncate, relids))

/*
 * We don't store the whole fixed part (HeapTupleHeaderData) of an inserted
 * or updated tuple in WAL; we can save a few bytes by reconstructing the
 * fields that are available elsewhere in the WAL record, or perhaps just
 * plain needn't be reconstructed.  These are the fields we must store.
 */
typedef struct xl_heap_header
{
	uint16		t_infomask2;
	uint16		t_infomask;
	uint8		t_hoff;
} xl_heap_header;

#define SizeOfHeapHeader	(offsetof(xl_heap_header, t_hoff) + sizeof(uint8))

/* This is what we need to know about insert */
typedef struct xl_heap_insert
{
	OffsetNumber offnum;		/* inserted tuple's offset */
	uint8		flags;

	/* xl_heap_header & TUPLE DATA in backup block 0 */
} xl_heap_insert;

#define SizeOfHeapInsert	(offsetof(xl_heap_insert, flags) + sizeof(uint8))

/*
 * This is what we need to know about a multi-insert.
 *
 * The main data of the record consists of this xl_heap_multi_insert header.
 * 'offsets' array is omitted if the whole page is reinitialized
 * (XLOG_HEAP_INIT_PAGE).
 *
 * In block 0's data portion, there is an xl_multi_insert_tuple struct,
 * followed by the tuple data for each tuple. There is padding to align
 * each xl_multi_insert_tuple struct.
 */
typedef struct xl_heap_multi_insert
{
	uint8		flags;
	uint16		ntuples;
	OffsetNumber offsets[FLEXIBLE_ARRAY_MEMBER];
} xl_heap_multi_insert;

#define SizeOfHeapMultiInsert	offsetof(xl_heap_multi_insert, offsets)

typedef struct xl_multi_insert_tuple
{
	uint16		datalen;		/* size of tuple data that follows */
	uint16		t_infomask2;
	uint16		t_infomask;
	uint8		t_hoff;
	/* TUPLE DATA FOLLOWS AT END OF STRUCT */
} xl_multi_insert_tuple;

#define SizeOfMultiInsertTuple	(offsetof(xl_multi_insert_tuple, t_hoff) + sizeof(uint8))

/*
 * This is what we need to know about update|hot_update
 *
 * Backup blk 0: new page
 *
 * If XLH_UPDATE_PREFIX_FROM_OLD or XLH_UPDATE_SUFFIX_FROM_OLD flags are set,
 * the prefix and/or suffix come first, as one or two uint16s.
 *
 * After that, xl_heap_header and new tuple data follow.  The new tuple
 * data doesn't include the prefix and suffix, which are copied from the
 * old tuple on replay.
 *
 * If XLH_UPDATE_CONTAINS_NEW_TUPLE flag is given, the tuple data is
 * included even if a full-page image was taken.
 *
 * Backup blk 1: old page, if different. (no data, just a reference to the blk)
 */
typedef struct xl_heap_update
{
	TransactionId old_xmax;		/* xmax of the old tuple */
	OffsetNumber old_offnum;	/* old tuple's offset */
	uint8		old_infobits_set;	/* infomask bits to set on old tuple */
	uint8		flags;
	TransactionId new_xmax;		/* xmax of the new tuple */
	OffsetNumber new_offnum;	/* new tuple's offset */

	/*
	 * If XLH_UPDATE_CONTAINS_OLD_TUPLE or XLH_UPDATE_CONTAINS_OLD_KEY flags
	 * are set, xl_heap_header and tuple data for the old tuple follow.
	 */
} xl_heap_update;

#define SizeOfHeapUpdate	(offsetof(xl_heap_update, new_offnum) + sizeof(OffsetNumber))

/*
 * These structures and flags encode VACUUM pruning and freezing and on-access
 * pruning page modifications.
 *
 * xl_heap_prune is the main record.  The XLHP_HAS_* flags indicate which
 * "sub-records" are included and the other XLHP_* flags provide additional
 * information about the conditions for replay.
 *
 * The data for block reference 0 contains "sub-records" depending on which of
 * the XLHP_HAS_* flags are set.  See xlhp_* struct definitions below.  The
 * sub-records appear in the same order as the XLHP_* flags.  An example
 * record with every sub-record included:
 *
 *-----------------------------------------------------------------------------
 * Main data section:
 *
 *	xl_heap_prune
 *		uint8				flags
 *	TransactionId			snapshot_conflict_horizon
 *
 * Block 0 data section:
 *
 *	xlhp_freeze_plans
 *		uint16				nplans
 *		[2 bytes of padding]
 *		xlhp_freeze_plan	plans[nplans]
 *
 *	xlhp_prune_items
 *		uint16				nredirected
 *		OffsetNumber		redirected[2 * nredirected]
 *
 *	xlhp_prune_items
 *		uint16				ndead
 *		OffsetNumber		nowdead[ndead]
 *
 *	xlhp_prune_items
 *		uint16				nunused
 *		OffsetNumber		nowunused[nunused]
 *
 *	OffsetNumber			frz_offsets[sum([plan.ntuples for plan in plans])]
 *-----------------------------------------------------------------------------
 *
 * NOTE: because the record data is assembled from many optional parts, we
 * have to pay close attention to alignment.  In the main data section,
 * 'snapshot_conflict_horizon' is stored unaligned after 'flags', to save
 * space.  In the block 0 data section, the freeze plans appear first, because
 * they contain TransactionId fields that require 4-byte alignment.  All the
 * other fields require only 2-byte alignment.  This is also the reason that
 * 'frz_offsets' is stored separately from the xlhp_freeze_plan structs.
 */
typedef struct xl_heap_prune
{
	uint8		reason;
	uint8		flags;

	/*
	 * If XLHP_HAS_CONFLICT_HORIZON is set, the conflict horizon XID follows,
	 * unaligned
	 */
} xl_heap_prune;

#define SizeOfHeapPrune (offsetof(xl_heap_prune, flags) + sizeof(uint8))

/* to handle recovery conflict during logical decoding on standby */
#define		XLHP_IS_CATALOG_REL			(1 << 1)

/*
 * Does replaying the record require a cleanup-lock?
 *
 * Pruning, in VACUUM's first pass or when otherwise accessing a page,
 * requires a cleanup lock.  For freezing, and VACUUM's second pass which
 * marks LP_DEAD line pointers as unused without moving any tuple data, an
 * ordinary exclusive lock is sufficient.
 */
#define		XLHP_CLEANUP_LOCK	       (1 << 2)

/*
 * If we remove or freeze any entries that contain xids, we need to include a
 * snapshot conflict horizon.  It's used in Hot Standby mode to ensure that
 * there are no queries running for which the removed tuples are still
 * visible, or which still consider the frozen XIDs as running.
 */
#define		XLHP_HAS_CONFLICT_HORIZON   (1 << 3)

/*
 * Indicates that an xlhp_freeze_plans sub-record and one or more
 * xlhp_freeze_plan sub-records are present.
 */
#define		XLHP_HAS_FREEZE_PLANS		(1 << 4)

/*
 * XLHP_HAS_REDIRECTIONS, XLHP_HAS_DEAD_ITEMS, and XLHP_HAS_NOW_UNUSED_ITEMS
 * indicate that xlhp_prune_items sub-records with redirected, dead, and
 * unused item offsets are present.
 */
#define		XLHP_HAS_REDIRECTIONS		(1 << 5)
#define		XLHP_HAS_DEAD_ITEMS	        (1 << 6)
#define		XLHP_HAS_NOW_UNUSED_ITEMS   (1 << 7)

/*
 * xlhp_freeze_plan describes how to freeze a group of one or more heap tuples
 * (appears in xl_heap_prune's xlhp_freeze_plans sub-record)
 */
/* 0x01 was XLH_FREEZE_XMIN */
#define		XLH_FREEZE_XVAC		0x02
#define		XLH_INVALID_XVAC	0x04

typedef struct xlhp_freeze_plan
{
	TransactionId xmax;
	uint16		t_infomask2;
	uint16		t_infomask;
	uint8		frzflags;

	/* Length of individual page offset numbers array for this plan */
	uint16		ntuples;
} xlhp_freeze_plan;

/*
 * This is what we need to know about a block being frozen during vacuum
 *
 * The backup block's data contains an array of xlhp_freeze_plan structs (with
 * nplans elements).  The individual item offsets are located in an array at
 * the end of the entire record with nplans * (each plan's ntuples) members
 * Those offsets are in the same order as the plans.  The REDO routine uses
 * the offsets to freeze the corresponding heap tuples.
 *
 * (As of PostgreSQL 17, XLOG_HEAP2_PRUNE_VACUUM_SCAN records replace the
 * separate XLOG_HEAP2_FREEZE_PAGE records.)
 */
typedef struct xlhp_freeze_plans
{
	uint16		nplans;
	xlhp_freeze_plan plans[FLEXIBLE_ARRAY_MEMBER];
} xlhp_freeze_plans;

/*
 * Generic sub-record type contained in block reference 0 of an xl_heap_prune
 * record and used for redirect, dead, and unused items if any of
 * XLHP_HAS_REDIRECTIONS/XLHP_HAS_DEAD_ITEMS/XLHP_HAS_NOW_UNUSED_ITEMS are
 * set.  Note that in the XLHP_HAS_REDIRECTIONS variant, there are actually 2
 * * length number of OffsetNumbers in the data.
 */
typedef struct xlhp_prune_items
{
	uint16		ntargets;
	OffsetNumber data[FLEXIBLE_ARRAY_MEMBER];
} xlhp_prune_items;


/* flags for infobits_set */
#define XLHL_XMAX_IS_MULTI		0x01
#define XLHL_XMAX_LOCK_ONLY		0x02
#define XLHL_XMAX_EXCL_LOCK		0x04
#define XLHL_XMAX_KEYSHR_LOCK	0x08
#define XLHL_KEYS_UPDATED		0x10

/* flag bits for xl_heap_lock / xl_heap_lock_updated's flag field */
#define XLH_LOCK_ALL_FROZEN_CLEARED		0x01

/* This is what we need to know about lock */
typedef struct xl_heap_lock
{
	TransactionId xmax;			/* might be a MultiXactId */
	OffsetNumber offnum;		/* locked tuple's offset on page */
	uint8		infobits_set;	/* infomask and infomask2 bits to set */
	uint8		flags;			/* XLH_LOCK_* flag bits */
} xl_heap_lock;

#define SizeOfHeapLock	(offsetof(xl_heap_lock, flags) + sizeof(uint8))

/* This is what we need to know about locking an updated version of a row */
typedef struct xl_heap_lock_updated
{
	TransactionId xmax;
	OffsetNumber offnum;
	uint8		infobits_set;
	uint8		flags;
} xl_heap_lock_updated;

#define SizeOfHeapLockUpdated	(offsetof(xl_heap_lock_updated, flags) + sizeof(uint8))

/* This is what we need to know about confirmation of speculative insertion */
typedef struct xl_heap_confirm
{
	OffsetNumber offnum;		/* confirmed tuple's offset on page */
} xl_heap_confirm;

#define SizeOfHeapConfirm	(offsetof(xl_heap_confirm, offnum) + sizeof(OffsetNumber))

/* This is what we need to know about in-place update */
typedef struct xl_heap_inplace
{
	OffsetNumber offnum;		/* updated tuple's offset on page */
	Oid			dbId;			/* MyDatabaseId */
	Oid			tsId;			/* MyDatabaseTableSpace */
	bool		relcacheInitFileInval;	/* invalidate relcache init files */
	int			nmsgs;			/* number of shared inval msgs */
	SharedInvalidationMessage msgs[FLEXIBLE_ARRAY_MEMBER];
} xl_heap_inplace;

#define MinSizeOfHeapInplace	(offsetof(xl_heap_inplace, nmsgs) + sizeof(int))

/*
 * This is what we need to know about setting a visibility map bit
 *
 * Backup blk 0: visibility map buffer
 * Backup blk 1: heap buffer
 */
typedef struct xl_heap_visible
{
	TransactionId snapshotConflictHorizon;
	uint8		flags;
} xl_heap_visible;

#define SizeOfHeapVisible (offsetof(xl_heap_visible, flags) + sizeof(uint8))

typedef struct xl_heap_new_cid
{
	/*
	 * store toplevel xid so we don't have to merge cids from different
	 * transactions
	 */
	TransactionId top_xid;
	CommandId	cmin;
	CommandId	cmax;
	CommandId	combocid;		/* just for debugging */

	/*
	 * Store the relfilelocator/ctid pair to facilitate lookups.
	 */
	RelFileLocator target_locator;
	ItemPointerData target_tid;
} xl_heap_new_cid;

#define SizeOfHeapNewCid (offsetof(xl_heap_new_cid, target_tid) + sizeof(ItemPointerData))

/* logical rewrite xlog record header */
typedef struct xl_heap_rewrite_mapping
{
	TransactionId mapped_xid;	/* xid that might need to see the row */
	Oid			mapped_db;		/* DbOid or InvalidOid for shared rels */
	Oid			mapped_rel;		/* Oid of the mapped relation */
	off_t		offset;			/* How far have we written so far */
	uint32		num_mappings;	/* Number of in-memory mappings */
	XLogRecPtr	start_lsn;		/* Insert LSN at begin of rewrite */
} xl_heap_rewrite_mapping;

extern void HeapTupleHeaderAdvanceConflictHorizon(HeapTupleHeader tuple,
												  TransactionId *snapshotConflictHorizon);

extern void heap_redo(XLogReaderState *record);
extern void heap_desc(StringInfo buf, XLogReaderState *record);
extern const char *heap_identify(uint8 info);
extern void heap_mask(char *pagedata, BlockNumber blkno);
extern void heap2_redo(XLogReaderState *record);
extern void heap2_desc(StringInfo buf, XLogReaderState *record);
extern const char *heap2_identify(uint8 info);
extern void heap_xlog_logical_rewrite(XLogReaderState *r);

extern XLogRecPtr log_heap_visible(Relation rel, Buffer heap_buffer,
								   Buffer vm_buffer,
								   TransactionId snapshotConflictHorizon,
								   uint8 vmflags);

/* in heapdesc.c, so it can be shared between frontend/backend code */
extern void heap_xlog_deserialize_prune_and_freeze(char *cursor, uint8 flags,
												   int *nplans, xlhp_freeze_plan **plans,
												   OffsetNumber **frz_offsets,
												   int *nredirected, OffsetNumber **redirected,
												   int *ndead, OffsetNumber **nowdead,
												   int *nunused, OffsetNumber **nowunused);

#endif							/* HEAPAM_XLOG_H */
