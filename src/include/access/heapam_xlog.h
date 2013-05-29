/*-------------------------------------------------------------------------
 *
 * heapam_xlog.h
 *	  POSTGRES heap access XLOG definitions.
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
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
 * We ran out of opcodes, so heapam.c now has a second RmgrId.	These opcodes
 * are associated with RM_HEAP2_ID, but are not logically different from
 * the ones above associated with RM_HEAP_ID.  XLOG_HEAP_OPMASK applies to
 * these, too.
 */
#define XLOG_HEAP2_FREEZE		0x00
#define XLOG_HEAP2_CLEAN		0x10
/* 0x20 is free, was XLOG_HEAP2_CLEAN_MOVE */
#define XLOG_HEAP2_CLEANUP_INFO 0x30
#define XLOG_HEAP2_VISIBLE		0x40
#define XLOG_HEAP2_MULTI_INSERT 0x50
#define XLOG_HEAP2_LOCK_UPDATED 0x60

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
	bool		all_visible_cleared;	/* PD_ALL_VISIBLE was cleared */
} xl_heap_delete;

#define SizeOfHeapDelete	(offsetof(xl_heap_delete, all_visible_cleared) + sizeof(bool))

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

/* This is what we need to know about insert */
typedef struct xl_heap_insert
{
	xl_heaptid	target;			/* inserted tuple id */
	bool		all_visible_cleared;	/* PD_ALL_VISIBLE was cleared */
	/* xl_heap_header & TUPLE DATA FOLLOWS AT END OF STRUCT */
} xl_heap_insert;

#define SizeOfHeapInsert	(offsetof(xl_heap_insert, all_visible_cleared) + sizeof(bool))

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
	bool		all_visible_cleared;
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
	bool		all_visible_cleared;	/* PD_ALL_VISIBLE was cleared */
	bool		new_all_visible_cleared;		/* same for the page of newtid */
	/* NEW TUPLE xl_heap_header AND TUPLE DATA FOLLOWS AT END OF STRUCT */
} xl_heap_update;

#define SizeOfHeapUpdate	(offsetof(xl_heap_update, new_all_visible_cleared) + sizeof(bool))

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
	/* entire page contents follow at end of record */
} xl_heap_newpage;

#define SizeOfHeapNewpage	(offsetof(xl_heap_newpage, blkno) + sizeof(BlockNumber))

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

/* This is what we need to know about tuple freezing during vacuum */
typedef struct xl_heap_freeze
{
	RelFileNode node;
	BlockNumber block;
	TransactionId cutoff_xid;
	MultiXactId cutoff_multi;
	/* TUPLE OFFSET NUMBERS FOLLOW AT THE END */
} xl_heap_freeze;

#define SizeOfHeapFreeze (offsetof(xl_heap_freeze, cutoff_multi) + sizeof(MultiXactId))

/* This is what we need to know about setting a visibility map bit */
typedef struct xl_heap_visible
{
	RelFileNode node;
	BlockNumber block;
	TransactionId cutoff_xid;
} xl_heap_visible;

#define SizeOfHeapVisible (offsetof(xl_heap_visible, cutoff_xid) + sizeof(TransactionId))

extern void HeapTupleHeaderAdvanceLatestRemovedXid(HeapTupleHeader tuple,
									   TransactionId *latestRemovedXid);

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
				TransactionId cutoff_xid, MultiXactId cutoff_multi,
				OffsetNumber *offsets, int offcnt);
extern XLogRecPtr log_heap_visible(RelFileNode rnode, Buffer heap_buffer,
				 Buffer vm_buffer, TransactionId cutoff_xid);
extern XLogRecPtr log_newpage(RelFileNode *rnode, ForkNumber forkNum,
			BlockNumber blk, Page page);
extern XLogRecPtr log_newpage_buffer(Buffer buffer);

#endif   /* HEAPAM_XLOG_H */
