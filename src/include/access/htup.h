 /*-------------------------------------------------------------------------
 *
 * htup.h
 *	  POSTGRES heap tuple definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: htup.h,v 1.53 2002/06/15 19:54:24 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HTUP_H
#define HTUP_H

#include "storage/bufpage.h"
#include "storage/relfilenode.h"
#include "access/transam.h"


/*
 * MaxTupleAttributeNumber limits the number of (user) columns in a tuple.
 * The key limit on this value is that the size of the fixed overhead for
 * a tuple, plus the size of the null-values bitmap (at 1 bit per column),
 * plus MAXALIGN alignment, must fit into t_hoff which is uint8.  On most
 * machines the upper limit without making t_hoff wider would be a little
 * over 1700.  We use round numbers here and for MaxHeapAttributeNumber
 * so that alterations in HeapTupleHeaderData layout won't change the
 * supported max number of columns.
 */
#define MaxTupleAttributeNumber	1664	/* 8 * 208 */

/*----------
 * MaxHeapAttributeNumber limits the number of (user) columns in a table.
 * This should be somewhat less than MaxTupleAttributeNumber.  It must be
 * at least one less, else we will fail to do UPDATEs on a maximal-width
 * table (because UPDATE has to form working tuples that include CTID).
 * In practice we want some additional daylight so that we can gracefully
 * support operations that add hidden "resjunk" columns, for example
 * SELECT * FROM wide_table ORDER BY foo, bar, baz.
 * In any case, depending on column data types you will likely be running
 * into the disk-block-based limit on overall tuple size if you have more
 * than a thousand or so columns.  TOAST won't help.
 *----------
 */
#define MaxHeapAttributeNumber	1600	/* 8 * 200 */

/*
 * On-disk heap tuple header.  Currently this is also used as the header
 * format for tuples formed in memory, although in principle they could
 * be different.
 *
 * To avoid wasting space, the attributes should be layed out in such a
 * way to reduce structure padding.  Note that t_hoff is the offset to
 * the start of the user data, and so must be a multiple of MAXALIGN.
 * Also note that we omit the nulls bitmap if t_infomask shows that there
 * are no nulls in the tuple.
 */
typedef struct HeapTupleHeaderData
{
	Oid			t_oid;			/* OID of this tuple -- 4 bytes */

	CommandId	t_cmin;			/* insert CID stamp -- 4 bytes each */
	CommandId	t_cmax;			/* delete CommandId stamp */

	TransactionId t_xmin;		/* insert XID stamp -- 4 bytes each */
	TransactionId t_xmax;		/* delete XID stamp */

	ItemPointerData t_ctid;		/* current TID of this or newer tuple */

	int16		t_natts;		/* number of attributes */

	uint16		t_infomask;		/* various flag bits, see below */

	uint8		t_hoff;			/* sizeof header incl. bitmap, padding */

	/* ^ - 31 bytes - ^ */

	bits8		t_bits[1];		/* bitmap of NULLs -- VARIABLE LENGTH */

	/* MORE DATA FOLLOWS AT END OF STRUCT */
} HeapTupleHeaderData;

typedef HeapTupleHeaderData *HeapTupleHeader;

/*
 * information stored in t_infomask:
 */
#define HEAP_HASNULL			0x0001	/* has null attribute(s) */
#define HEAP_HASVARLENA			0x0002	/* has variable length
										 * attribute(s) */
#define HEAP_HASEXTERNAL		0x0004	/* has external stored
										 * attribute(s) */
#define HEAP_HASCOMPRESSED		0x0008	/* has compressed stored
										 * attribute(s) */
#define HEAP_HASEXTENDED		0x000C	/* the two above combined */

#define HEAP_XMAX_UNLOGGED		0x0080	/* to lock tuple for update */
										/* without logging */
#define HEAP_XMIN_COMMITTED		0x0100	/* t_xmin committed */
#define HEAP_XMIN_INVALID		0x0200	/* t_xmin invalid/aborted */
#define HEAP_XMAX_COMMITTED		0x0400	/* t_xmax committed */
#define HEAP_XMAX_INVALID		0x0800	/* t_xmax invalid/aborted */
#define HEAP_MARKED_FOR_UPDATE	0x1000	/* marked for UPDATE */
#define HEAP_UPDATED			0x2000	/* this is UPDATEd version of row */
#define HEAP_MOVED_OFF			0x4000	/* moved to another place by
										 * vacuum */
#define HEAP_MOVED_IN			0x8000	/* moved from another place by
										 * vacuum */

#define HEAP_XACT_MASK			0xFFF0	/* visibility-related bits */



/* HeapTupleHeader accessor macros */

#define HeapTupleHeaderGetXmin(tup) \
	((tup)->t_xmin)

#define HeapTupleHeaderGetXmax(tup) \
	((tup)->t_xmax)

/* no AssertMacro, because this is read as a system-defined attribute also */
#define HeapTupleHeaderGetCmin(tup) \
( \
	(tup)->t_cmin \
)

#define HeapTupleHeaderGetCmax(tup) \
	((tup)->t_cmax)

#define HeapTupleHeaderGetXvac(tup) \
( \
	AssertMacro((tup)->t_infomask & (HEAP_MOVED_IN | HEAP_MOVED_OFF)), \
	(TransactionId) (tup)->t_cmin \
)


#define HeapTupleHeaderSetXmin(tup, xid) \
	(TransactionIdStore((xid), &(tup)->t_xmin))

#define HeapTupleHeaderSetXminInvalid(tup) \
	(StoreInvalidTransactionId(&(tup)->t_xmin))

#define HeapTupleHeaderSetXmax(tup, xid) \
	(TransactionIdStore((xid), &(tup)->t_xmax))

#define HeapTupleHeaderSetXmaxInvalid(tup) \
	(StoreInvalidTransactionId(&(tup)->t_xmax))

#define HeapTupleHeaderSetCmin(tup, cid) \
( \
	AssertMacro(!((tup)->t_infomask & (HEAP_MOVED_IN | HEAP_MOVED_OFF))), \
	(tup)->t_cmin = (cid) \
)

#define HeapTupleHeaderSetCmax(tup, cid) \
	((tup)->t_cmax = (cid))

#define HeapTupleHeaderSetXvac(tup, xid) \
( \
	AssertMacro((tup)->t_infomask & (HEAP_MOVED_IN | HEAP_MOVED_OFF)), \
	TransactionIdStore((xid), (TransactionId *) &((tup)->t_cmin)) \
)


/*
 * XLOG allows to store some information in high 4 bits of log
 * record xl_info field
 */
#define XLOG_HEAP_INSERT	0x00
#define XLOG_HEAP_DELETE	0x10
#define XLOG_HEAP_UPDATE	0x20
#define XLOG_HEAP_MOVE		0x30
#define XLOG_HEAP_CLEAN		0x40
#define XLOG_HEAP_OPMASK	0x70
/*
 * When we insert 1st item on new page in INSERT/UPDATE
 * we can (and we do) restore entire page in redo
 */
#define XLOG_HEAP_INIT_PAGE 0x80

/*
 * All what we need to find changed tuple (18 bytes)
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
} xl_heap_delete;

#define SizeOfHeapDelete	(offsetof(xl_heap_delete, target) + SizeOfHeapTid)

typedef struct xl_heap_header
{
	Oid			t_oid;
	int16		t_natts;
	uint8		t_hoff;
	uint8		mask;			/* low 8 bits of t_infomask */
} xl_heap_header;

#define SizeOfHeapHeader	(offsetof(xl_heap_header, mask) + sizeof(uint8))

/* This is what we need to know about insert */
typedef struct xl_heap_insert
{
	xl_heaptid	target;			/* inserted tuple id */
	/* xl_heap_header & TUPLE DATA FOLLOWS AT END OF STRUCT */
} xl_heap_insert;

#define SizeOfHeapInsert	(offsetof(xl_heap_insert, target) + SizeOfHeapTid)

/* This is what we need to know about update|move */
typedef struct xl_heap_update
{
	xl_heaptid	target;			/* deleted tuple id */
	ItemPointerData newtid;		/* new inserted tuple id */
	/* NEW TUPLE xl_heap_header (PLUS xmax & xmin IF MOVE OP) */
	/* and TUPLE DATA FOLLOWS AT END OF STRUCT */
} xl_heap_update;

#define SizeOfHeapUpdate	(offsetof(xl_heap_update, newtid) + SizeOfIptrData)

/* This is what we need to know about page cleanup */
typedef struct xl_heap_clean
{
	RelFileNode node;
	BlockNumber block;
	/* UNUSED OFFSET NUMBERS FOLLOW AT THE END */
} xl_heap_clean;

#define SizeOfHeapClean (offsetof(xl_heap_clean, block) + sizeof(BlockNumber))

/*
 * MaxTupleSize is the maximum allowed size of a tuple, including header and
 * MAXALIGN alignment padding.	Basically it's BLCKSZ minus the other stuff
 * that has to be on a disk page.  The "other stuff" includes access-method-
 * dependent "special space", which we assume will be no more than
 * MaxSpecialSpace bytes (currently, on heap pages it's actually zero).
 *
 * NOTE: we do not need to count an ItemId for the tuple because
 * sizeof(PageHeaderData) includes the first ItemId on the page.
 */
#define MaxSpecialSpace  32

#define MaxTupleSize	\
	(BLCKSZ - MAXALIGN(sizeof(PageHeaderData) + MaxSpecialSpace))

/*
 * MaxAttrSize is a somewhat arbitrary upper limit on the declared size of
 * data fields of char(n) and similar types.  It need not have anything
 * directly to do with the *actual* upper limit of varlena values, which
 * is currently 1Gb (see struct varattrib in postgres.h).  I've set it
 * at 10Mb which seems like a reasonable number --- tgl 8/6/00.
 */
#define MaxAttrSize		(10 * 1024 * 1024)


/*
 * Attribute numbers for the system-defined attributes
 */
#define SelfItemPointerAttributeNumber			(-1)
#define ObjectIdAttributeNumber					(-2)
#define MinTransactionIdAttributeNumber			(-3)
#define MinCommandIdAttributeNumber				(-4)
#define MaxTransactionIdAttributeNumber			(-5)
#define MaxCommandIdAttributeNumber				(-6)
#define TableOidAttributeNumber					(-7)
#define FirstLowInvalidHeapAttributeNumber		(-8)

/*
 * HeapTupleData is an in-memory data structure that points to a tuple.
 *
 * This new HeapTuple for version >= 6.5 and this is why it was changed:
 *
 * 1. t_len moved off on-disk tuple data - ItemIdData is used to get len;
 * 2. t_ctid above is not self tuple TID now - it may point to
 *	  updated version of tuple (required by MVCC);
 * 3. someday someone let tuple to cross block boundaries -
 *	  he have to add something below...
 *
 * Change for 7.0:
 *	  Up to now t_data could be NULL, the memory location directly following
 *	  HeapTupleData, or pointing into a buffer. Now, it could also point to
 *	  a separate allocation that was done in the t_datamcxt memory context.
 */
typedef struct HeapTupleData
{
	uint32		t_len;			/* length of *t_data */
	ItemPointerData t_self;		/* SelfItemPointer */
	Oid			t_tableOid;		/* table the tuple came from */
	MemoryContext t_datamcxt;	/* memory context of allocation */
	HeapTupleHeader t_data;		/* -> tuple header and data */
} HeapTupleData;

typedef HeapTupleData *HeapTuple;

#define HEAPTUPLESIZE	MAXALIGN(sizeof(HeapTupleData))


/* ----------------
 *		support macros
 * ----------------
 */
#define GETSTRUCT(TUP) (((char *)((HeapTuple)(TUP))->t_data) + \
						((HeapTuple)(TUP))->t_data->t_hoff)


/*
 * BITMAPLEN(NATTS) -
 *		Computes size of null bitmap given number of data columns.
 */
#define BITMAPLEN(NATTS)	(((int)(NATTS) + 7) / 8)

/*
 * HeapTupleIsValid
 *		True iff the heap tuple is valid.
 */
#define HeapTupleIsValid(tuple) PointerIsValid(tuple)

#define HeapTupleNoNulls(tuple) \
		(!(((HeapTuple) (tuple))->t_data->t_infomask & HEAP_HASNULL))

#define HeapTupleAllFixed(tuple) \
		(!(((HeapTuple) (tuple))->t_data->t_infomask & HEAP_HASVARLENA))

#define HeapTupleHasExternal(tuple) \
		((((HeapTuple)(tuple))->t_data->t_infomask & HEAP_HASEXTERNAL) != 0)

#define HeapTupleHasCompressed(tuple) \
		((((HeapTuple)(tuple))->t_data->t_infomask & HEAP_HASCOMPRESSED) != 0)

#define HeapTupleHasExtended(tuple) \
		((((HeapTuple)(tuple))->t_data->t_infomask & HEAP_HASEXTENDED) != 0)

#endif   /* HTUP_H */
