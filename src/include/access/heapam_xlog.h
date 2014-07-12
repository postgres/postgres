/*-------------------------------------------------------------------------
 *
 * heapam_xlog.h
 *	  POSTGRES heap access XLOG definitions.
 *
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/heapam_xlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HEAPAM_XLOG_H
#define HEAPAM_XLOG_H

#include "access/htup.h"
#include "access/xlog.h"
#include "storage/bufpage.h"
#include "storage/relfilenode.h"
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
/* 0x030 is free, was XLOG_HEAP_MOVE */
#define XLOG_HEAP_HOT_UPDATE	0x40
#define XLOG_HEAP_NEWPAGE		0x50
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
 */
#define XLOG_HEAP2_REWRITE		0x00
#define XLOG_HEAP2_CLEAN		0x10
#define XLOG_HEAP2_FREEZE_PAGE	0x20
#define XLOG_HEAP2_CLEANUP_INFO 0x30
#define XLOG_HEAP2_VISIBLE		0x40
#define XLOG_HEAP2_MULTI_INSERT 0x50
#define XLOG_HEAP2_LOCK_UPDATED 0x60
#define XLOG_HEAP2_NEW_CID		0x70

/*
 * xl_heap_* ->flag values, 8 bits are available.
 */
/* PD_ALL_VISIBLE was cleared */
#define XLOG_HEAP_ALL_VISIBLE_CLEARED		(1<<0)
/* PD_ALL_VISIBLE was cleared in the 2nd page */
#define XLOG_HEAP_NEW_ALL_VISIBLE_CLEARED	(1<<1)
#define XLOG_HEAP_CONTAINS_OLD_TUPLE		(1<<2)
#define XLOG_HEAP_CONTAINS_OLD_KEY			(1<<3)
#define XLOG_HEAP_CONTAINS_NEW_TUPLE		(1<<4)
#define XLOG_HEAP_PREFIX_FROM_OLD			(1<<5)
#define XLOG_HEAP_SUFFIX_FROM_OLD			(1<<6)
/* last xl_heap_multi_insert record for one heap_multi_insert() call */
#define XLOG_HEAP_LAST_MULTI_INSERT			(1<<7)

/* convenience macro for checking whether any form of old tuple was logged */
#define XLOG_HEAP_CONTAINS_OLD						\
	(XLOG_HEAP_CONTAINS_OLD_TUPLE | XLOG_HEAP_CONTAINS_OLD_KEY)

/*
 * All what we need to find changed tuple
 *
 * NB: on most machines, sizeof(xl_heaptid) will include some trailing pad
 * bytes for alignment.  We don't want to store the pad space in the XLOG,
 * so use SizeOfHeapTid for space calculations.  Similar comments apply for
 * the other xl_FOO structs.
 */
typedef struct xl_heaptid
{
	RelFileNode node;
	ItemPointerData tid;		/* changed tuple id */
} xl_heaptid;

#define SizeOfHeapTid		(offsetof(xl_heaptid, tid) + SizeOfIptrData)

/* This is what we need to know about delete */
typedef struct xl_heap_delete
{
	xl_heaptid	target;			/* deleted tuple id */
	TransactionId xmax;			/* xmax of the deleted tuple */
	uint8		infobits_set;	/* infomask bits */
	uint8		flags;
} xl_heap_delete;

#define SizeOfHeapDelete	(offsetof(xl_heap_delete, flags) + sizeof(uint8))

/*
 * We don't store the whole fixed part (HeapTupleHeaderData) of an inserted
 * or updated tuple in WAL; we can save a few bytes by reconstructing the
 * fields that are available elsewhere in the WAL record, or perhaps just
 * plain needn't be reconstructed.  These are the fields we must store.
 * NOTE: t_hoff could be recomputed, but we may as well store it because
 * it will come for free due to alignment considerations.
 */
typedef struct xl_heap_header
{
	uint16		t_infomask2;
	uint16		t_infomask;
	uint8		t_hoff;
} xl_heap_header;

#define SizeOfHeapHeader	(offsetof(xl_heap_header, t_hoff) + sizeof(uint8))

/*
 * Variant of xl_heap_header that contains the length of the tuple, which is
 * useful if the length of the tuple cannot be computed using the overall
 * record length. E.g. because there are several tuples inside a single
 * record.
 */
typedef struct xl_heap_header_len
{
	uint16		t_len;
	xl_heap_header header;
} xl_heap_header_len;

#define SizeOfHeapHeaderLen (offsetof(xl_heap_header_len, header) + SizeOfHeapHeader)

/* This is what we need to know about insert */
typedef struct xl_heap_insert
{
	xl_heaptid	target;			/* inserted tuple id */
	uint8		flags;
	/* xl_heap_header & TUPLE DATA FOLLOWS AT END OF STRUCT */
} xl_heap_insert;

#define SizeOfHeapInsert	(offsetof(xl_heap_insert, flags) + sizeof(uint8))

/*
 * This is what we need to know about a multi-insert. The record consists of
 * xl_heap_multi_insert header, followed by a xl_multi_insert_tuple and tuple
 * data for each tuple. 'offsets' array is omitted if the whole page is
 * reinitialized (XLOG_HEAP_INIT_PAGE)
 */
typedef struct xl_heap_multi_insert
{
	RelFileNode node;
	BlockNumber blkno;
	uint8		flags;
	uint16		ntuples;
	OffsetNumber offsets[1];

	/* TUPLE DATA (xl_multi_insert_tuples) FOLLOW AT END OF STRUCT */
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

/* This is what we need to know about update|hot_update */
typedef struct xl_heap_update
{
	xl_heaptid	target;			/* deleted tuple id */
	TransactionId old_xmax;		/* xmax of the old tuple */
	TransactionId new_xmax;		/* xmax of the new tuple */
	ItemPointerData newtid;		/* new inserted tuple id */
	uint8		old_infobits_set;		/* infomask bits to set on old tuple */
	uint8		flags;

	/*
	 * If XLOG_HEAP_PREFIX_FROM_OLD or XLOG_HEAP_SUFFIX_FROM_OLD flags are
	 * set, the prefix and/or suffix come next, as one or two uint16s.
	 *
	 * After that, xl_heap_header_len and new tuple data follow.  The new
	 * tuple data and length don't include the prefix and suffix, which are
	 * copied from the old tuple on replay.  The new tuple data is omitted if
	 * a full-page image of the page was taken (unless the
	 * XLOG_HEAP_CONTAINS_NEW_TUPLE flag is set, in which case it's included
	 * anyway).
	 *
	 * If XLOG_HEAP_CONTAINS_OLD_TUPLE or XLOG_HEAP_CONTAINS_OLD_KEY flags are
	 * set, another xl_heap_header_len struct and tuple data for the old tuple
	 * follows.
	 */
} xl_heap_update;

#define SizeOfHeapUpdate	(offsetof(xl_heap_update, flags) + sizeof(uint8))

/*
 * This is what we need to know about vacuum page cleanup/redirect
 *
 * The array of OffsetNumbers following the fixed part of the record contains:
 *	* for each redirected item: the item offset, then the offset redirected to
 *	* for each now-dead item: the item offset
 *	* for each now-unused item: the item offset
 * The total number of OffsetNumbers is therefore 2*nredirected+ndead+nunused.
 * Note that nunused is not explicitly stored, but may be found by reference
 * to the total record length.
 */
typedef struct xl_heap_clean
{
	RelFileNode node;
	BlockNumber block;
	TransactionId latestRemovedXid;
	uint16		nredirected;
	uint16		ndead;
	/* OFFSET NUMBERS FOLLOW */
} xl_heap_clean;

#define SizeOfHeapClean (offsetof(xl_heap_clean, ndead) + sizeof(uint16))

/*
 * Cleanup_info is required in some cases during a lazy VACUUM.
 * Used for reporting the results of HeapTupleHeaderAdvanceLatestRemovedXid()
 * see vacuumlazy.c for full explanation
 */
typedef struct xl_heap_cleanup_info
{
	RelFileNode node;
	TransactionId latestRemovedXid;
} xl_heap_cleanup_info;

#define SizeOfHeapCleanupInfo (sizeof(xl_heap_cleanup_info))

/* This is for replacing a page's contents in toto */
/* NB: this is used for indexes as well as heaps */
typedef struct xl_heap_newpage
{
	RelFileNode node;
	ForkNumber	forknum;
	BlockNumber blkno;			/* location of new page */
	uint16		hole_offset;	/* number of bytes before "hole" */
	uint16		hole_length;	/* number of bytes in "hole" */
	/* entire page contents (minus the hole) follow at end of record */
} xl_heap_newpage;

#define SizeOfHeapNewpage	(offsetof(xl_heap_newpage, hole_length) + sizeof(uint16))

/* flags for infobits_set */
#define XLHL_XMAX_IS_MULTI		0x01
#define XLHL_XMAX_LOCK_ONLY		0x02
#define XLHL_XMAX_EXCL_LOCK		0x04
#define XLHL_XMAX_KEYSHR_LOCK	0x08
#define XLHL_KEYS_UPDATED		0x10

/* This is what we need to know about lock */
typedef struct xl_heap_lock
{
	xl_heaptid	target;			/* locked tuple id */
	TransactionId locking_xid;	/* might be a MultiXactId not xid */
	int8		infobits_set;	/* infomask and infomask2 bits to set */
} xl_heap_lock;

#define SizeOfHeapLock	(offsetof(xl_heap_lock, infobits_set) + sizeof(int8))

/* This is what we need to know about locking an updated version of a row */
typedef struct xl_heap_lock_updated
{
	xl_heaptid	target;
	TransactionId xmax;
	uint8		infobits_set;
} xl_heap_lock_updated;

#define SizeOfHeapLockUpdated	(offsetof(xl_heap_lock_updated, infobits_set) + sizeof(uint8))

/* This is what we need to know about in-place update */
typedef struct xl_heap_inplace
{
	xl_heaptid	target;			/* updated tuple id */
	/* TUPLE DATA FOLLOWS AT END OF STRUCT */
} xl_heap_inplace;

#define SizeOfHeapInplace	(offsetof(xl_heap_inplace, target) + SizeOfHeapTid)

/*
 * This struct represents a 'freeze plan', which is what we need to know about
 * a single tuple being frozen during vacuum.
 */
/* 0x01 was XLH_FREEZE_XMIN */
#define		XLH_FREEZE_XVAC		0x02
#define		XLH_INVALID_XVAC	0x04

typedef struct xl_heap_freeze_tuple
{
	TransactionId xmax;
	OffsetNumber offset;
	uint16		t_infomask2;
	uint16		t_infomask;
	uint8		frzflags;
} xl_heap_freeze_tuple;

/*
 * This is what we need to know about a block being frozen during vacuum
 */
typedef struct xl_heap_freeze_page
{
	RelFileNode node;
	BlockNumber block;
	TransactionId cutoff_xid;
	uint16		ntuples;
	xl_heap_freeze_tuple tuples[FLEXIBLE_ARRAY_MEMBER];
} xl_heap_freeze_page;

#define SizeOfHeapFreezePage offsetof(xl_heap_freeze_page, tuples)

/* This is what we need to know about setting a visibility map bit */
typedef struct xl_heap_visible
{
	RelFileNode node;
	BlockNumber block;
	TransactionId cutoff_xid;
} xl_heap_visible;

#define SizeOfHeapVisible (offsetof(xl_heap_visible, cutoff_xid) + sizeof(TransactionId))

typedef struct xl_heap_new_cid
{
	/*
	 * store toplevel xid so we don't have to merge cids from different
	 * transactions
	 */
	TransactionId top_xid;
	CommandId	cmin;
	CommandId	cmax;

	/*
	 * don't really need the combocid since we have the actual values right in
	 * this struct, but the padding makes it free and its useful for
	 * debugging.
	 */
	CommandId	combocid;

	/*
	 * Store the relfilenode/ctid pair to facilitate lookups.
	 */
	xl_heaptid	target;
} xl_heap_new_cid;

#define SizeOfHeapNewCid (offsetof(xl_heap_new_cid, target) + SizeOfHeapTid)

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

extern void HeapTupleHeaderAdvanceLatestRemovedXid(HeapTupleHeader tuple,
									   TransactionId *latestRemovedXid);

extern void heap_redo(XLogRecPtr lsn, XLogRecord *rptr);
extern void heap_desc(StringInfo buf, uint8 xl_info, char *rec);
extern void heap2_redo(XLogRecPtr lsn, XLogRecord *rptr);
extern void heap2_desc(StringInfo buf, uint8 xl_info, char *rec);
extern void heap_xlog_logical_rewrite(XLogRecPtr lsn, XLogRecord *r);

extern XLogRecPtr log_heap_cleanup_info(RelFileNode rnode,
					  TransactionId latestRemovedXid);
extern XLogRecPtr log_heap_clean(Relation reln, Buffer buffer,
			   OffsetNumber *redirected, int nredirected,
			   OffsetNumber *nowdead, int ndead,
			   OffsetNumber *nowunused, int nunused,
			   TransactionId latestRemovedXid);
extern XLogRecPtr log_heap_freeze(Relation reln, Buffer buffer,
				TransactionId cutoff_xid, xl_heap_freeze_tuple *tuples,
				int ntuples);
extern bool heap_prepare_freeze_tuple(HeapTupleHeader tuple,
						  TransactionId cutoff_xid,
						  TransactionId cutoff_multi,
						  xl_heap_freeze_tuple *frz);
extern void heap_execute_freeze_tuple(HeapTupleHeader tuple,
						  xl_heap_freeze_tuple *xlrec_tp);
extern XLogRecPtr log_heap_visible(RelFileNode rnode, Buffer heap_buffer,
				 Buffer vm_buffer, TransactionId cutoff_xid);
extern XLogRecPtr log_newpage(RelFileNode *rnode, ForkNumber forkNum,
			BlockNumber blk, Page page, bool page_std);
extern XLogRecPtr log_newpage_buffer(Buffer buffer, bool page_std);

#endif   /* HEAPAM_XLOG_H */
