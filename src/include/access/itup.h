/*-------------------------------------------------------------------------
 *
 * itup.h
 *	  POSTGRES index tuple definitions.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: itup.h,v 1.38 2003/08/04 02:40:10 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ITUP_H
#define ITUP_H

#include "access/ibit.h"
#include "access/tupdesc.h"
#include "access/tupmacs.h"
#include "storage/itemptr.h"


typedef struct IndexTupleData
{
	ItemPointerData t_tid;		/* reference TID to heap tuple */

	/* ---------------
	 * t_info is layed out in the following fashion:
	 *
	 * 15th (high) bit: has nulls
	 * 14th bit: has var-width attributes
	 * 13th bit: unused
	 * 12-0 bit: size of tuple
	 * ---------------
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


/* ----------------
 *		externs
 * ----------------
 */

#define INDEX_SIZE_MASK 0x1FFF
#define INDEX_NULL_MASK 0x8000
#define INDEX_VAR_MASK	0x4000
/* bit 0x2000 is not used */

#define IndexTupleSize(itup)		((Size) (((IndexTuple) (itup))->t_info & INDEX_SIZE_MASK))
#define IndexTupleDSize(itup)		((Size) ((itup).t_info & INDEX_SIZE_MASK))
#define IndexTupleHasNulls(itup)	((((IndexTuple) (itup))->t_info & INDEX_NULL_MASK))
#define IndexTupleHasVarwidths(itup) ((((IndexTuple) (itup))->t_info & INDEX_VAR_MASK))

#define IndexTupleHasMinHeader(itup) (!IndexTupleHasNulls(itup))

/*
 * Takes an infomask as argument (primarily because this needs to be usable
 * at index_formtuple time so enough space is allocated).
 *
 * Change me if adding an attribute to IndexTuples!!!!!!!!!!!
 */
#define IndexInfoFindDataOffset(t_info) \
( \
	(!((unsigned short)(t_info) & INDEX_NULL_MASK)) ? \
	( \
		(Size)sizeof(IndexTupleData) \
	) \
	: \
	( \
		(Size)MAXALIGN(sizeof(IndexTupleData) + sizeof(IndexAttributeBitMapData)) \
	) \
)

/* ----------------
 *		index_getattr
 *
 *		This gets called many times, so we macro the cacheable and NULL
 *		lookups, and call noncachegetattr() for the rest.
 *
 * ----------------
 */
#define index_getattr(tup, attnum, tupleDesc, isnull) \
( \
	AssertMacro(PointerIsValid(isnull) && (attnum) > 0), \
	*(isnull) = false, \
	!IndexTupleHasNulls(tup) ? \
	( \
		(tupleDesc)->attrs[(attnum)-1]->attcacheoff >= 0 ? \
		( \
			fetchatt((tupleDesc)->attrs[(attnum)-1], \
			(char *) (tup) + \
			( \
				IndexTupleHasMinHeader(tup) ? \
						sizeof (*(tup)) \
					: \
						IndexInfoFindDataOffset((tup)->t_info) \
			) \
			+ (tupleDesc)->attrs[(attnum)-1]->attcacheoff) \
		) \
		: \
			nocache_index_getattr((tup), (attnum), (tupleDesc), (isnull)) \
	) \
	: \
	( \
		(att_isnull((attnum)-1, (char *)(tup) + sizeof(*(tup)))) ? \
		( \
			*(isnull) = true, \
			(Datum)NULL \
		) \
		: \
		( \
			nocache_index_getattr((tup), (attnum), (tupleDesc), (isnull)) \
		) \
	) \
)


/* routines in indextuple.c */
extern IndexTuple index_formtuple(TupleDesc tupleDescriptor,
				Datum *value, char *null);
extern Datum nocache_index_getattr(IndexTuple tup, int attnum,
					  TupleDesc tupleDesc, bool *isnull);
extern IndexTuple CopyIndexTuple(IndexTuple source);

#endif   /* ITUP_H */
