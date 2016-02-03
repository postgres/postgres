/*-------------------------------------------------------------------------
 *
 * amapi.h
 *	  API for Postgres index access methods.
 *
 * Copyright (c) 2015-2016, PostgreSQL Global Development Group
 *
 * src/include/access/amapi.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AMAPI_H
#define AMAPI_H

#include "access/genam.h"

/*
 * We don't wish to include planner header files here, since most of an index
 * AM's implementation isn't concerned with those data structures.  To allow
 * declaring amcostestimate_function here, use forward struct references.
 */
struct PlannerInfo;
struct IndexPath;

/* Likewise, this file shouldn't depend on execnodes.h. */
struct IndexInfo;


/*
 * Callback function signatures --- see indexam.sgml for more info.
 */

/* build new index */
typedef IndexBuildResult *(*ambuild_function) (Relation heapRelation,
													  Relation indexRelation,
												struct IndexInfo *indexInfo);

/* build empty index */
typedef void (*ambuildempty_function) (Relation indexRelation);

/* insert this tuple */
typedef bool (*aminsert_function) (Relation indexRelation,
											   Datum *values,
											   bool *isnull,
											   ItemPointer heap_tid,
											   Relation heapRelation,
											   IndexUniqueCheck checkUnique);

/* bulk delete */
typedef IndexBulkDeleteResult *(*ambulkdelete_function) (IndexVacuumInfo *info,
												IndexBulkDeleteResult *stats,
											IndexBulkDeleteCallback callback,
													   void *callback_state);

/* post-VACUUM cleanup */
typedef IndexBulkDeleteResult *(*amvacuumcleanup_function) (IndexVacuumInfo *info,
											   IndexBulkDeleteResult *stats);

/* can indexscan return IndexTuples? */
typedef bool (*amcanreturn_function) (Relation indexRelation, int attno);

/* estimate cost of an indexscan */
typedef void (*amcostestimate_function) (struct PlannerInfo *root,
													 struct IndexPath *path,
													 double loop_count,
													 Cost *indexStartupCost,
													 Cost *indexTotalCost,
											   Selectivity *indexSelectivity,
												   double *indexCorrelation);

/* parse index reloptions */
typedef bytea *(*amoptions_function) (Datum reloptions,
												  bool validate);

/* validate definition of an opclass for this AM */
typedef bool (*amvalidate_function) (Oid opclassoid);

/* prepare for index scan */
typedef IndexScanDesc (*ambeginscan_function) (Relation indexRelation,
														   int nkeys,
														   int norderbys);

/* (re)start index scan */
typedef void (*amrescan_function) (IndexScanDesc scan,
											   ScanKey keys,
											   int nkeys,
											   ScanKey orderbys,
											   int norderbys);

/* next valid tuple */
typedef bool (*amgettuple_function) (IndexScanDesc scan,
												 ScanDirection direction);

/* fetch all valid tuples */
typedef int64 (*amgetbitmap_function) (IndexScanDesc scan,
												   TIDBitmap *tbm);

/* end index scan */
typedef void (*amendscan_function) (IndexScanDesc scan);

/* mark current scan position */
typedef void (*ammarkpos_function) (IndexScanDesc scan);

/* restore marked scan position */
typedef void (*amrestrpos_function) (IndexScanDesc scan);


/*
 * API struct for an index AM.  Note this must be stored in a single palloc'd
 * chunk of memory.
 */
typedef struct IndexAmRoutine
{
	NodeTag		type;

	/*
	 * Total number of strategies (operators) by which we can traverse/search
	 * this AM.  Zero if AM does not have a fixed set of strategy assignments.
	 */
	uint16		amstrategies;
	/* total number of support functions that this AM uses */
	uint16		amsupport;
	/* does AM support ORDER BY indexed column's value? */
	bool		amcanorder;
	/* does AM support ORDER BY result of an operator on indexed column? */
	bool		amcanorderbyop;
	/* does AM support backward scanning? */
	bool		amcanbackward;
	/* does AM support UNIQUE indexes? */
	bool		amcanunique;
	/* does AM support multi-column indexes? */
	bool		amcanmulticol;
	/* does AM require scans to have a constraint on the first index column? */
	bool		amoptionalkey;
	/* does AM handle ScalarArrayOpExpr quals? */
	bool		amsearcharray;
	/* does AM handle IS NULL/IS NOT NULL quals? */
	bool		amsearchnulls;
	/* can index storage data type differ from column data type? */
	bool		amstorage;
	/* can an index of this type be clustered on? */
	bool		amclusterable;
	/* does AM handle predicate locks? */
	bool		ampredlocks;
	/* type of data stored in index, or InvalidOid if variable */
	Oid			amkeytype;

	/* interface functions */
	ambuild_function ambuild;
	ambuildempty_function ambuildempty;
	aminsert_function aminsert;
	ambulkdelete_function ambulkdelete;
	amvacuumcleanup_function amvacuumcleanup;
	amcanreturn_function amcanreturn;	/* can be NULL */
	amcostestimate_function amcostestimate;
	amoptions_function amoptions;
	amvalidate_function amvalidate;
	ambeginscan_function ambeginscan;
	amrescan_function amrescan;
	amgettuple_function amgettuple;		/* can be NULL */
	amgetbitmap_function amgetbitmap;	/* can be NULL */
	amendscan_function amendscan;
	ammarkpos_function ammarkpos;		/* can be NULL */
	amrestrpos_function amrestrpos;		/* can be NULL */
} IndexAmRoutine;


/* Functions in access/index/amapi.c */
extern IndexAmRoutine *GetIndexAmRoutine(Oid amhandler);
extern IndexAmRoutine *GetIndexAmRoutineByAmId(Oid amoid);

extern Datum amvalidate(PG_FUNCTION_ARGS);

#endif   /* AMAPI_H */
