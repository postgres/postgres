/*-------------------------------------------------------------------------
 *
 * genam.h
 *	  POSTGRES generalized index access method definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: genam.h,v 1.34 2002/05/20 23:51:43 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef GENAM_H
#define GENAM_H

#include "access/itup.h"
#include "access/relscan.h"
#include "access/sdir.h"
#include "nodes/primnodes.h"


/* Struct for statistics returned by bulk-delete operation */
typedef struct IndexBulkDeleteResult
{
	BlockNumber num_pages;		/* pages remaining in index */
	double		tuples_removed; /* # removed by bulk-delete operation */
	double		num_index_tuples;		/* # remaining */
} IndexBulkDeleteResult;

/* Typedef for callback function to determine if a tuple is bulk-deletable */
typedef bool (*IndexBulkDeleteCallback) (ItemPointer itemptr, void *state);


/* Struct for heap-or-index scans of system tables */
typedef struct SysScanDescData
{
	Relation	heap_rel;		/* catalog being scanned */
	Relation	irel;			/* NULL if doing heap scan */
	HeapScanDesc scan;			/* only valid in heap-scan case */
	IndexScanDesc iscan;		/* only valid in index-scan case */
} SysScanDescData;

typedef SysScanDescData *SysScanDesc;


/*
 * generalized index_ interface routines (in indexam.c)
 */
extern Relation index_open(Oid relationId);
extern Relation index_openrv(const RangeVar *relation);
extern Relation index_openr(const char *sysRelationName);
extern void index_close(Relation relation);
extern InsertIndexResult index_insert(Relation indexRelation,
			 Datum *datums, char *nulls,
			 ItemPointer heap_t_ctid,
			 Relation heapRelation);

extern IndexScanDesc index_beginscan(Relation heapRelation,
									 Relation indexRelation,
									 Snapshot snapshot,
									 int nkeys, ScanKey key);
extern void index_rescan(IndexScanDesc scan, ScanKey key);
extern void index_endscan(IndexScanDesc scan);
extern void index_markpos(IndexScanDesc scan);
extern void index_restrpos(IndexScanDesc scan);
extern HeapTuple index_getnext(IndexScanDesc scan, ScanDirection direction);
extern bool index_getnext_indexitem(IndexScanDesc scan,
									ScanDirection direction);

extern IndexBulkDeleteResult *index_bulk_delete(Relation indexRelation,
				  IndexBulkDeleteCallback callback,
				  void *callback_state);
extern RegProcedure index_cost_estimator(Relation indexRelation);
extern RegProcedure index_getprocid(Relation irel, AttrNumber attnum,
				uint16 procnum);
extern struct FmgrInfo *index_getprocinfo(Relation irel, AttrNumber attnum,
				  uint16 procnum);

/*
 * index access method support routines (in genam.c)
 */
extern IndexScanDesc RelationGetIndexScan(Relation indexRelation,
										  int nkeys, ScanKey key);
extern void IndexScanEnd(IndexScanDesc scan);

/*
 * heap-or-index access to system catalogs (in genam.c)
 */
extern SysScanDesc systable_beginscan(Relation heapRelation,
									  const char *indexRelname,
									  bool indexOK,
									  Snapshot snapshot,
									  int nkeys, ScanKey key);
extern HeapTuple systable_getnext(SysScanDesc sysscan);
extern void systable_endscan(SysScanDesc sysscan);

#endif   /* GENAM_H */
