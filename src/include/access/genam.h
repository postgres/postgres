/*-------------------------------------------------------------------------
 *
 * genam.h
 *	  POSTGRES generalized index access method definitions.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/genam.h,v 1.53 2005/10/15 02:49:42 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef GENAM_H
#define GENAM_H

#include "access/itup.h"
#include "access/relscan.h"
#include "access/sdir.h"
#include "nodes/primnodes.h"


/*
 * Struct for statistics returned by bulk-delete operation
 *
 * This is now also passed to the index AM's vacuum-cleanup operation,
 * if it has one, which can modify the results as needed.  Note that
 * an index AM could choose to have bulk-delete return a larger struct
 * of which this is just the first field; this provides a way for bulk-delete
 * to communicate additional private data to vacuum-cleanup.
 *
 * Note: pages_removed is the amount by which the index physically shrank,
 * if any (ie the change in its total size on disk).  pages_deleted and
 * pages_free refer to free space within the index file.
 */
typedef struct IndexBulkDeleteResult
{
	BlockNumber num_pages;		/* pages remaining in index */
	BlockNumber pages_removed;	/* # removed by bulk-delete operation */
	double		num_index_tuples;		/* tuples remaining */
	double		tuples_removed; /* # removed by bulk-delete operation */
	BlockNumber pages_deleted;	/* # unused pages in index */
	BlockNumber pages_free;		/* # pages available for reuse */
} IndexBulkDeleteResult;

/* Typedef for callback function to determine if a tuple is bulk-deletable */
typedef bool (*IndexBulkDeleteCallback) (ItemPointer itemptr, void *state);

/* Struct for additional arguments passed to vacuum-cleanup operation */
typedef struct IndexVacuumCleanupInfo
{
	bool		vacuum_full;	/* VACUUM FULL (we have exclusive lock) */
	int			message_level;	/* ereport level for progress messages */
} IndexVacuumCleanupInfo;

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
extern void index_close(Relation relation);
extern bool index_insert(Relation indexRelation,
			 Datum *values, bool *isnull,
			 ItemPointer heap_t_ctid,
			 Relation heapRelation,
			 bool check_uniqueness);

extern IndexScanDesc index_beginscan(Relation heapRelation,
				Relation indexRelation,
				Snapshot snapshot,
				int nkeys, ScanKey key);
extern IndexScanDesc index_beginscan_multi(Relation indexRelation,
					  Snapshot snapshot,
					  int nkeys, ScanKey key);
extern void index_rescan(IndexScanDesc scan, ScanKey key);
extern void index_endscan(IndexScanDesc scan);
extern void index_markpos(IndexScanDesc scan);
extern void index_restrpos(IndexScanDesc scan);
extern HeapTuple index_getnext(IndexScanDesc scan, ScanDirection direction);
extern bool index_getnext_indexitem(IndexScanDesc scan,
						ScanDirection direction);
extern bool index_getmulti(IndexScanDesc scan,
			   ItemPointer tids, int32 max_tids,
			   int32 *returned_tids);

extern IndexBulkDeleteResult *index_bulk_delete(Relation indexRelation,
				  IndexBulkDeleteCallback callback,
				  void *callback_state);
extern IndexBulkDeleteResult *index_vacuum_cleanup(Relation indexRelation,
					 IndexVacuumCleanupInfo *info,
					 IndexBulkDeleteResult *stats);
extern RegProcedure index_getprocid(Relation irel, AttrNumber attnum,
				uint16 procnum);
extern FmgrInfo *index_getprocinfo(Relation irel, AttrNumber attnum,
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
				   Oid indexId,
				   bool indexOK,
				   Snapshot snapshot,
				   int nkeys, ScanKey key);
extern HeapTuple systable_getnext(SysScanDesc sysscan);
extern void systable_endscan(SysScanDesc sysscan);

#endif   /* GENAM_H */
