/*-------------------------------------------------------------------------
 *
 * itup.h--
 *	  POSTGRES index tuple definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: itup.h,v 1.8 1997/09/08 21:50:53 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ITUP_H
#define ITUP_H

#include <access/tupdesc.h>
#include <storage/itemptr.h>

#define MaxIndexAttributeNumber 7

typedef struct IndexTupleData
{
	ItemPointerData t_tid;		/* reference TID to base tuple */

	/*
	 * t_info is layed out in the following fashion:
	 *
	 * 15th (leftmost) bit: "has nulls" bit 14th bit: "has varlenas" bit 13th
	 * bit: "has rules" bit - (removed ay 11/94) bits 12-0 bit: size of
	 * tuple.
	 */

	unsigned short t_info;		/* various info about tuple */

	/*
	 * please make sure sizeof(IndexTupleData) is MAXALIGN'ed. See
	 * IndexInfoFindDataOffset() for the reason.
	 */

} IndexTupleData;				/* MORE DATA FOLLOWS AT END OF STRUCT */

typedef IndexTupleData *IndexTuple;


typedef struct InsertIndexResultData
{
	ItemPointerData pointerData;
} InsertIndexResultData;

typedef InsertIndexResultData *InsertIndexResult;


typedef struct RetrieveIndexResultData
{
	ItemPointerData index_iptr;
	ItemPointerData heap_iptr;
} RetrieveIndexResultData;

typedef RetrieveIndexResultData *RetrieveIndexResult;


/*-----------------
 * PredInfo -
 *	  used for partial indices
 *-----------------
 */
typedef struct PredInfo
{
	Node	   *pred;
	Node	   *oldPred;
} PredInfo;


/* ----------------
 *		externs
 * ----------------
 */

#define INDEX_SIZE_MASK 0x1FFF
#define INDEX_NULL_MASK 0x8000
#define INDEX_VAR_MASK	0x4000

#define IndexTupleSize(itup)	   (((IndexTuple) (itup))->t_info & 0x1FFF)
#define IndexTupleDSize(itup)					   ((itup).t_info & 0x1FFF)
#define IndexTupleNoNulls(itup)  (!(((IndexTuple) (itup))->t_info & 0x8000))
#define IndexTupleAllFixed(itup) (!(((IndexTuple) (itup))->t_info & 0x4000))

#define IndexTupleHasMinHeader(itup) (IndexTupleNoNulls(itup))


/* indextuple.h */
extern IndexTuple
index_formtuple(TupleDesc tupleDescriptor,
				Datum value[], char null[]);
extern Datum
index_getattr(IndexTuple tuple, AttrNumber attNum,
			  TupleDesc tupDesc, bool *isNullOutP);
extern RetrieveIndexResult
FormRetrieveIndexResult(ItemPointer indexItemPointer,
						ItemPointer heapItemPointer);
extern void CopyIndexTuple(IndexTuple source, IndexTuple *target);


#endif							/* ITUP_H */
