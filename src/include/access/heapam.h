/*-------------------------------------------------------------------------
 *
 * heapam.h--
 *    POSTGRES heap access method definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: heapam.h,v 1.1 1996/08/27 21:50:13 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	HEAPAM_H
#define HEAPAM_H

#include <sys/types.h>

#include "postgres.h"

#include "access/attnum.h"
#include "access/htup.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "utils/tqual.h"
#include "access/tupdesc.h"
#include "storage/smgr.h"
#include "utils/rel.h"

/* ----------------------------------------------------------------
 *		heap access method statistics
 * ----------------------------------------------------------------
 */

typedef struct HeapAccessStatisticsData {
    time_t  init_global_timestamp;	/* time global statistics started */
    time_t  local_reset_timestamp;	/* last time local reset was done */
    time_t  last_request_timestamp;	/* last time stats were requested */

    int global_open;			
    int global_openr;
    int global_close;
    int global_beginscan;
    int global_rescan;
    int global_endscan;
    int global_getnext;
    int global_fetch;
    int global_insert;
    int global_delete;
    int global_replace; 
    int global_markpos; 
    int global_restrpos;
    int global_BufferGetRelation;
    int global_RelationIdGetRelation;
    int global_RelationIdGetRelation_Buf;
    int global_RelationNameGetRelation;
    int global_getreldesc;
    int global_heapgettup;
    int global_RelationPutHeapTuple;
    int global_RelationPutLongHeapTuple;

    int local_open;			
    int local_openr;
    int local_close;
    int local_beginscan;
    int local_rescan;
    int local_endscan;
    int local_getnext;
    int local_fetch;
    int local_insert;
    int local_delete;
    int local_replace; 
    int local_markpos; 
    int local_restrpos;
    int local_BufferGetRelation;
    int local_RelationIdGetRelation;
    int local_RelationIdGetRelation_Buf;
    int local_RelationNameGetRelation;
    int local_getreldesc;
    int local_heapgettup;
    int local_RelationPutHeapTuple;
    int local_RelationPutLongHeapTuple;
} HeapAccessStatisticsData;

typedef HeapAccessStatisticsData *HeapAccessStatistics;

#define IncrHeapAccessStat(x) \
    (heap_access_stats == NULL ? 0 : (heap_access_stats->x)++)

extern HeapAccessStatistics heap_access_stats;	/* in stats.c */

/* ----------------
 *	function prototypes for heap access method
 * ----------------
 */
/* heap_create, heap_creatr, and heap_destroy are declared in catalog/heap.h */
#include "catalog/heap.h"

/* heapam.c */
extern void doinsert(Relation relation, HeapTuple tup);
extern void SetHeapAccessMethodImmediateInvalidation(bool on);

extern Relation heap_open(Oid relationId);
extern Relation heap_openr(char *relationName);
extern void heap_close(Relation relation);
extern HeapScanDesc heap_beginscan(Relation relation, int atend,
			    TimeQual timeQual, unsigned nkeys, ScanKey key);
extern void heap_rescan(HeapScanDesc sdesc, bool scanFromEnd, ScanKey key);
extern void heap_endscan(HeapScanDesc sdesc);
extern HeapTuple heap_getnext(HeapScanDesc scandesc, int backw, Buffer *b);
extern HeapTuple heap_fetch(Relation relation, TimeQual timeQual,
			    ItemPointer tid, Buffer *b);
extern Oid heap_insert(Relation relation, HeapTuple tup);
extern void heap_delete(Relation relation, ItemPointer tid);
extern int heap_replace(Relation relation, ItemPointer otid,
			HeapTuple tup);
extern void heap_markpos(HeapScanDesc sdesc);
extern void heap_restrpos(HeapScanDesc sdesc);

/* in common/heaptuple.c */
extern Size ComputeDataSize(TupleDesc tupleDesc, Datum value[], char nulls[]);
extern void DataFill(char *data, TupleDesc tupleDesc,
		     Datum value[], char nulls[], char *infomask,
		     bits8 *bit);
extern int heap_attisnull(HeapTuple tup, int attnum);
extern int heap_sysattrlen(AttrNumber attno);
extern bool heap_sysattrbyval(AttrNumber attno);
extern char *heap_getsysattr(HeapTuple tup, Buffer b, int attnum);
extern char *fastgetattr(HeapTuple tup, unsigned attnum,
			 TupleDesc att, bool *isnull);
extern char *heap_getattr(HeapTuple tup, Buffer b, int attnum,
			  TupleDesc att, bool *isnull);
extern HeapTuple heap_copytuple(HeapTuple tuple);
extern void heap_deformtuple(HeapTuple tuple, TupleDesc tdesc,
			     Datum values[], char nulls[]);
extern HeapTuple heap_formtuple(TupleDesc tupleDescriptor, 
				Datum value[], char nulls[]);
extern HeapTuple heap_modifytuple(HeapTuple tuple, Buffer buffer,
	Relation relation, Datum replValue[], char replNull[], char repl[]);
HeapTuple heap_addheader(uint32	natts, int structlen, char *structure);

/* in common/heap/stats.c */
extern void InitHeapAccessStatistics(void);
extern void ResetHeapAccessStatistics(void);
extern HeapAccessStatistics GetHeapAccessStatistics(void);
extern void PrintHeapAccessStatistics(HeapAccessStatistics stats);
extern void PrintAndFreeHeapAccessStatistics(HeapAccessStatistics stats);
extern void initam(void);

#endif	/* HEAPAM_H */
