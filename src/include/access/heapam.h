/*-------------------------------------------------------------------------
 *
 * heapam.h--
 *	  POSTGRES heap access method definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: heapam.h,v 1.36 1998/09/01 03:27:32 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HEAPAM_H
#define HEAPAM_H

#include <access/tupmacs.h>
#include <access/htup.h>
#include <access/relscan.h>
#include <storage/block.h>
#include <utils/rel.h>
#include <utils/tqual.h>

/* ----------------------------------------------------------------
 *				heap access method statistics
 * ----------------------------------------------------------------
 */

typedef struct HeapAccessStatisticsData
{
	time_t		init_global_timestamp;	/* time global statistics started */
	time_t		local_reset_timestamp;	/* last time local reset was done */
	time_t		last_request_timestamp; /* last time stats were requested */

	int			global_open;
	int			global_openr;
	int			global_close;
	int			global_beginscan;
	int			global_rescan;
	int			global_endscan;
	int			global_getnext;
	int			global_fetch;
	int			global_insert;
	int			global_delete;
	int			global_replace;
	int			global_markpos;
	int			global_restrpos;
	int			global_BufferGetRelation;
	int			global_RelationIdGetRelation;
	int			global_RelationIdGetRelation_Buf;
	int			global_RelationNameGetRelation;
	int			global_getreldesc;
	int			global_heapgettup;
	int			global_RelationPutHeapTuple;
	int			global_RelationPutLongHeapTuple;

	int			local_open;
	int			local_openr;
	int			local_close;
	int			local_beginscan;
	int			local_rescan;
	int			local_endscan;
	int			local_getnext;
	int			local_fetch;
	int			local_insert;
	int			local_delete;
	int			local_replace;
	int			local_markpos;
	int			local_restrpos;
	int			local_BufferGetRelation;
	int			local_RelationIdGetRelation;
	int			local_RelationIdGetRelation_Buf;
	int			local_RelationNameGetRelation;
	int			local_getreldesc;
	int			local_heapgettup;
	int			local_RelationPutHeapTuple;
	int			local_RelationPutLongHeapTuple;
} HeapAccessStatisticsData;

typedef HeapAccessStatisticsData *HeapAccessStatistics;

#define IncrHeapAccessStat(x) \
	(heap_access_stats == NULL ? 0 : (heap_access_stats->x)++)

/* ----------------
 *		fastgetattr
 *
 *		This gets called many times, so we macro the cacheable and NULL
 *		lookups, and call noncachegetattr() for the rest.
 *
 * ----------------
 */
#if !defined(DISABLE_COMPLEX_MACRO)

#define fastgetattr(tup, attnum, tupleDesc, isnull) \
( \
	AssertMacro((attnum) > 0), \
	((isnull) ? (*(isnull) = false) : (dummyret)NULL), \
	HeapTupleNoNulls(tup) ? \
	( \
		((tupleDesc)->attrs[(attnum)-1]->attcacheoff != -1 || \
		 (attnum) == 1) ? \
		( \
			(Datum)fetchatt(&((tupleDesc)->attrs[(attnum)-1]), \
				(char *) (tup) + (tup)->t_hoff + \
				( \
					((attnum) != 1) ? \
						(tupleDesc)->attrs[(attnum)-1]->attcacheoff \
					: \
						0 \
				) \
			) \
		) \
		: \
			nocachegetattr((tup), (attnum), (tupleDesc), (isnull)) \
	) \
	: \
	( \
		att_isnull((attnum)-1, (tup)->t_bits) ? \
		( \
			((isnull) ? (*(isnull) = true) : (dummyret)NULL), \
			(Datum)NULL \
		) \
		: \
		( \
			nocachegetattr((tup), (attnum), (tupleDesc), (isnull)) \
		) \
	) \
)

#else /* !defined(DISABLE_COMPLEX_MACRO) */

extern Datum nocachegetattr(HeapTuple tup, int attnum,
						 TupleDesc att, bool *isnull);

static Datum fastgetattr(HeapTuple tup, int attnum, TupleDesc tupleDesc,
						 bool *isnull)
{
    return (
		(attnum) > 0 ?
		(
			((isnull) ? (*(isnull) = false) : (dummyret)NULL),
			HeapTupleNoNulls(tup) ?
			(
				((tupleDesc)->attrs[(attnum)-1]->attcacheoff != -1 ||
				 (attnum) == 1) ?
				(
					(Datum)fetchatt(&((tupleDesc)->attrs[(attnum)-1]),
						(char *) (tup) + (tup)->t_hoff +
						(
							((attnum) != 1) ?
								(tupleDesc)->attrs[(attnum)-1]->attcacheoff
							:
								0
						)
					)
				)
				:
					nocachegetattr((tup), (attnum), (tupleDesc), (isnull))
			)
			:
			(
				att_isnull((attnum)-1, (tup)->t_bits) ?
				(
					((isnull) ? (*(isnull) = true) : (dummyret)NULL),
					(Datum)NULL
				)
				:
				(
					nocachegetattr((tup), (attnum), (tupleDesc), (isnull))
				)
			)
		)
		:
		(
			 (Datum)NULL
		)
	);
}

#endif

/* ----------------
 *		heap_getattr
 *
 *		Find a particular field in a row represented as a heap tuple.
 *		We return a pointer into that heap tuple, which points to the
 *		first byte of the value of the field in question.
 *
 *		If the field in question has a NULL value, we return a null
 *		pointer and return <*isnull> == true.  Otherwise, we return
 *		<*isnull> == false.
 *
 *		<tup> is the pointer to the heap tuple.  <attnum> is the attribute
 *		number of the column (field) caller wants.	<tupleDesc> is a
 *		pointer to the structure describing the row and all its fields.
 *
 *		Because this macro is often called with constants, it generates
 *		compiler warnings about 'left-hand comma expression has no effect.
 *
 * ----------------
 */
#define heap_getattr(tup, attnum, tupleDesc, isnull) \
( \
	AssertMacro((tup) != NULL && \
		(attnum) > FirstLowInvalidHeapAttributeNumber && \
		(attnum) != 0), \
	((attnum) > (int) (tup)->t_natts) ? \
	( \
		((isnull) ? (*(isnull) = true) : (dummyret)NULL), \
		(Datum)NULL \
	) \
	: \
	( \
		((attnum) > 0) ? \
		( \
			fastgetattr((tup), (attnum), (tupleDesc), (isnull)) \
		) \
		: \
		( \
			((isnull) ? (*(isnull) = false) : (dummyret)NULL), \
			((attnum) == SelfItemPointerAttributeNumber) ? \
			( \
				(Datum)((char *)(tup) + \
					heap_sysoffset[-SelfItemPointerAttributeNumber-1]) \
			) \
			: \
			( \
				(Datum)*(unsigned int *) \
					((char *)(tup) + heap_sysoffset[-(attnum)-1]) \
			) \
		) \
	) \
)

extern HeapAccessStatistics heap_access_stats;	/* in stats.c */

/* ----------------
 *		function prototypes for heap access method
 * ----------------
 */
/* heap_create, heap_creatr, and heap_destroy are declared in catalog/heap.h */

/* heapam.c */
extern void doinsert(Relation relation, HeapTuple tup);

extern Relation heap_open(Oid relationId);
extern Relation heap_openr(char *relationName);
extern void heap_close(Relation relation);
extern HeapScanDesc
heap_beginscan(Relation relation, int atend,
			   Snapshot snapshot, unsigned nkeys, ScanKey key);
extern void heap_rescan(HeapScanDesc scan, bool scanFromEnd, ScanKey key);
extern void heap_endscan(HeapScanDesc scan);
extern HeapTuple heap_getnext(HeapScanDesc scandesc, int backw);
extern HeapTuple heap_fetch(Relation relation, Snapshot snapshot, ItemPointer tid, Buffer *userbuf);
extern Oid	heap_insert(Relation relation, HeapTuple tup);
extern int	heap_delete(Relation relation, ItemPointer tid);
extern int
heap_replace(Relation relation, ItemPointer otid,
			 HeapTuple tup);
extern void heap_markpos(HeapScanDesc scan);
extern void heap_restrpos(HeapScanDesc scan);

/* in common/heaptuple.c */
extern Size ComputeDataSize(TupleDesc tupleDesc, Datum *value, char *nulls);
extern void
DataFill(char *data, TupleDesc tupleDesc,
		 Datum *value, char *nulls, uint16 *infomask,
		 bits8 *bit);
extern int	heap_attisnull(HeapTuple tup, int attnum);
extern int	heap_sysattrlen(AttrNumber attno);
extern bool heap_sysattrbyval(AttrNumber attno);
extern Datum heap_getsysattr(HeapTuple tup, Buffer b, int attnum);
extern Datum
nocachegetattr(HeapTuple tup, int attnum,
			   TupleDesc att, bool *isnull);
extern HeapTuple heap_copytuple(HeapTuple tuple);
extern HeapTuple
heap_formtuple(TupleDesc tupleDescriptor,
			   Datum *value, char *nulls);
extern HeapTuple
heap_modifytuple(HeapTuple tuple,
		Relation relation, Datum *replValue, char *replNull, char *repl);
HeapTuple	heap_addheader(uint32 natts, int structlen, char *structure);

/* in common/heap/stats.c */
extern void PrintHeapAccessStatistics(HeapAccessStatistics stats);
extern void initam(void);

/* hio.c */
extern void
RelationPutHeapTuple(Relation relation, BlockNumber blockIndex,
					 HeapTuple tuple);
extern void RelationPutHeapTupleAtEnd(Relation relation, HeapTuple tuple);

#endif							/* HEAPAM_H */
