/*-------------------------------------------------------------------------
 *
 * htup.h--
 *	  POSTGRES heap tuple definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: htup.h,v 1.11 1998/11/27 19:33:31 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HTUP_H
#define HTUP_H

#include <utils/nabstime.h>
#include <storage/itemptr.h>

#define MinHeapTupleBitmapSize	32		/* 8 * 4 */

/* check these, they are likely to be more severely limited by t_hoff */

#define MaxHeapAttributeNumber	1600	/* 8 * 200 */

/*
 * to avoid wasting space, the attributes should be layed out in such a
 * way to reduce structure padding.
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

	uint16		t_infomask;		/* various infos */

	uint8		t_hoff;			/* sizeof tuple header */

	bits8		t_bits[MinHeapTupleBitmapSize / 8];
	/* bit map of domains */

	/* MORE DATA FOLLOWS AT END OF STRUCT */
} HeapTupleHeaderData;

typedef HeapTupleHeaderData *HeapTupleHeader;

#define SelfItemPointerAttributeNumber			(-1)
#define ObjectIdAttributeNumber					(-2)
#define MinTransactionIdAttributeNumber			(-3)
#define MinCommandIdAttributeNumber				(-4)
#define MaxTransactionIdAttributeNumber			(-5)
#define MaxCommandIdAttributeNumber				(-6)
#define FirstLowInvalidHeapAttributeNumber		(-7)

/* If you make any changes above, the order off offsets in this must change */
extern long heap_sysoffset[];

/*
 * This new HeapTuple for version >= 6.5 and this is why it was changed:
 *
 * 1. t_len moved off on-disk tuple data - ItemIdData is used to get len;
 * 2. t_ctid above is not self tuple TID now - it may point to
 *    updated version of tuple (required by MVCC);
 * 3. someday someone let tuple to cross block boundaries - 
 *    he have to add something below...
 */
typedef struct HeapTupleData
{
	uint32				t_len;		/* length of *t_data */
	ItemPointerData		t_self;		/* SelfItemPointer */
	HeapTupleHeader		t_data;		/* */
} HeapTupleData;
	
typedef HeapTupleData *HeapTuple;

#define	HEAPTUPLESIZE	DOUBLEALIGN(sizeof(HeapTupleData))
 
 
/* ----------------
 *		support macros
 * ----------------
 */
#define GETSTRUCT(TUP) (((char *)((HeapTuple)(TUP))->t_data) + \
						((HeapTuple)(TUP))->t_data->t_hoff)


/*
 * BITMAPLEN(NATTS) -
 *		Computes minimum size of bitmap given number of domains.
 */
#define BITMAPLEN(NATTS) \
		((((((int)(NATTS) - 1) >> 3) + 4 - (MinHeapTupleBitmapSize >> 3)) \
		  & ~03) + (MinHeapTupleBitmapSize >> 3))

/*
 * HeapTupleIsValid
 *		True iff the heap tuple is valid.
 */
#define HeapTupleIsValid(tuple) PointerIsValid(tuple)

/*
 * information stored in t_infomask:
 */
#define HEAP_HASNULL			0x0001	/* has null attribute(s) */
#define HEAP_HASVARLENA			0x0002	/* has variable length
										 * attribute(s) */
#define HEAP_XMIN_COMMITTED		0x0100	/* t_xmin committed */
#define HEAP_XMIN_INVALID		0x0200	/* t_xmin invalid/aborted */
#define HEAP_XMAX_COMMITTED		0x0400	/* t_xmax committed */
#define HEAP_XMAX_INVALID		0x0800	/* t_xmax invalid/aborted */

#define HEAP_XACT_MASK			0x0F00	/* */

#define HeapTupleNoNulls(tuple) \
		(!(((HeapTuple) (tuple))->t_data->t_infomask & HEAP_HASNULL))

#define HeapTupleAllFixed(tuple) \
		(!(((HeapTuple) (tuple))->t_data->t_infomask & HEAP_HASVARLENA))

#endif	 /* HTUP_H */
