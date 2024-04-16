/*-------------------------------------------------------------------------
 *
 * fdwapi.h
 *	  API for foreign-data wrappers
 *
 * Copyright (c) 2010-2024, PostgreSQL Global Development Group
 *
 * src/include/foreign/fdwapi.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FDWAPI_H
#define FDWAPI_H

#include "access/parallel.h"
#include "nodes/execnodes.h"
#include "nodes/pathnodes.h"

/* To avoid including explain.h here, reference ExplainState thus: */
struct ExplainState;


/*
 * Callback function signatures --- see fdwhandler.sgml for more info.
 */

typedef void (*GetForeignRelSize_function) (PlannerInfo *root,
											RelOptInfo *baserel,
											Oid foreigntableid);

typedef void (*GetForeignPaths_function) (PlannerInfo *root,
										  RelOptInfo *baserel,
										  Oid foreigntableid);

typedef ForeignScan *(*GetForeignPlan_function) (PlannerInfo *root,
												 RelOptInfo *baserel,
												 Oid foreigntableid,
												 ForeignPath *best_path,
												 List *tlist,
												 List *scan_clauses,
												 Plan *outer_plan);

typedef void (*BeginForeignScan_function) (ForeignScanState *node,
										   int eflags);

typedef TupleTableSlot *(*IterateForeignScan_function) (ForeignScanState *node);

typedef bool (*RecheckForeignScan_function) (ForeignScanState *node,
											 TupleTableSlot *slot);

typedef void (*ReScanForeignScan_function) (ForeignScanState *node);

typedef void (*EndForeignScan_function) (ForeignScanState *node);

typedef void (*GetForeignJoinPaths_function) (PlannerInfo *root,
											  RelOptInfo *joinrel,
											  RelOptInfo *outerrel,
											  RelOptInfo *innerrel,
											  JoinType jointype,
											  JoinPathExtraData *extra);

typedef void (*GetForeignUpperPaths_function) (PlannerInfo *root,
											   UpperRelationKind stage,
											   RelOptInfo *input_rel,
											   RelOptInfo *output_rel,
											   void *extra);

typedef void (*AddForeignUpdateTargets_function) (PlannerInfo *root,
												  Index rtindex,
												  RangeTblEntry *target_rte,
												  Relation target_relation);

typedef List *(*PlanForeignModify_function) (PlannerInfo *root,
											 ModifyTable *plan,
											 Index resultRelation,
											 int subplan_index);

typedef void (*BeginForeignModify_function) (ModifyTableState *mtstate,
											 ResultRelInfo *rinfo,
											 List *fdw_private,
											 int subplan_index,
											 int eflags);

typedef TupleTableSlot *(*ExecForeignInsert_function) (EState *estate,
													   ResultRelInfo *rinfo,
													   TupleTableSlot *slot,
													   TupleTableSlot *planSlot);

typedef TupleTableSlot **(*ExecForeignBatchInsert_function) (EState *estate,
															 ResultRelInfo *rinfo,
															 TupleTableSlot **slots,
															 TupleTableSlot **planSlots,
															 int *numSlots);

typedef int (*GetForeignModifyBatchSize_function) (ResultRelInfo *rinfo);

typedef TupleTableSlot *(*ExecForeignUpdate_function) (EState *estate,
													   ResultRelInfo *rinfo,
													   TupleTableSlot *slot,
													   TupleTableSlot *planSlot);

typedef TupleTableSlot *(*ExecForeignDelete_function) (EState *estate,
													   ResultRelInfo *rinfo,
													   TupleTableSlot *slot,
													   TupleTableSlot *planSlot);

typedef void (*EndForeignModify_function) (EState *estate,
										   ResultRelInfo *rinfo);

typedef void (*BeginForeignInsert_function) (ModifyTableState *mtstate,
											 ResultRelInfo *rinfo);

typedef void (*EndForeignInsert_function) (EState *estate,
										   ResultRelInfo *rinfo);

typedef int (*IsForeignRelUpdatable_function) (Relation rel);

typedef bool (*PlanDirectModify_function) (PlannerInfo *root,
										   ModifyTable *plan,
										   Index resultRelation,
										   int subplan_index);

typedef void (*BeginDirectModify_function) (ForeignScanState *node,
											int eflags);

typedef TupleTableSlot *(*IterateDirectModify_function) (ForeignScanState *node);

typedef void (*EndDirectModify_function) (ForeignScanState *node);

typedef RowMarkType (*GetForeignRowMarkType_function) (RangeTblEntry *rte,
													   LockClauseStrength strength);

typedef void (*RefetchForeignRow_function) (EState *estate,
											ExecRowMark *erm,
											Datum rowid,
											TupleTableSlot *slot,
											bool *updated);

typedef void (*ExplainForeignScan_function) (ForeignScanState *node,
											 struct ExplainState *es);

typedef void (*ExplainForeignModify_function) (ModifyTableState *mtstate,
											   ResultRelInfo *rinfo,
											   List *fdw_private,
											   int subplan_index,
											   struct ExplainState *es);

typedef void (*ExplainDirectModify_function) (ForeignScanState *node,
											  struct ExplainState *es);

typedef int (*AcquireSampleRowsFunc) (Relation relation, int elevel,
									  HeapTuple *rows, int targrows,
									  double *totalrows,
									  double *totaldeadrows);

typedef bool (*AnalyzeForeignTable_function) (Relation relation,
											  AcquireSampleRowsFunc *func,
											  BlockNumber *totalpages);

typedef List *(*ImportForeignSchema_function) (ImportForeignSchemaStmt *stmt,
											   Oid serverOid);

typedef void (*ExecForeignTruncate_function) (List *rels,
											  DropBehavior behavior,
											  bool restart_seqs);

typedef Size (*EstimateDSMForeignScan_function) (ForeignScanState *node,
												 ParallelContext *pcxt);
typedef void (*InitializeDSMForeignScan_function) (ForeignScanState *node,
												   ParallelContext *pcxt,
												   void *coordinate);
typedef void (*ReInitializeDSMForeignScan_function) (ForeignScanState *node,
													 ParallelContext *pcxt,
													 void *coordinate);
typedef void (*InitializeWorkerForeignScan_function) (ForeignScanState *node,
													  shm_toc *toc,
													  void *coordinate);
typedef void (*ShutdownForeignScan_function) (ForeignScanState *node);
typedef bool (*IsForeignScanParallelSafe_function) (PlannerInfo *root,
													RelOptInfo *rel,
													RangeTblEntry *rte);
typedef List *(*ReparameterizeForeignPathByChild_function) (PlannerInfo *root,
															List *fdw_private,
															RelOptInfo *child_rel);

typedef bool (*IsForeignPathAsyncCapable_function) (ForeignPath *path);

typedef void (*ForeignAsyncRequest_function) (AsyncRequest *areq);

typedef void (*ForeignAsyncConfigureWait_function) (AsyncRequest *areq);

typedef void (*ForeignAsyncNotify_function) (AsyncRequest *areq);

/*
 * FdwRoutine is the struct returned by a foreign-data wrapper's handler
 * function.  It provides pointers to the callback functions needed by the
 * planner and executor.
 *
 * More function pointers are likely to be added in the future.  Therefore
 * it's recommended that the handler initialize the struct with
 * makeNode(FdwRoutine) so that all fields are set to NULL.  This will
 * ensure that no fields are accidentally left undefined.
 */
typedef struct FdwRoutine
{
	NodeTag		type;

	/* Functions for scanning foreign tables */
	GetForeignRelSize_function GetForeignRelSize;
	GetForeignPaths_function GetForeignPaths;
	GetForeignPlan_function GetForeignPlan;
	BeginForeignScan_function BeginForeignScan;
	IterateForeignScan_function IterateForeignScan;
	ReScanForeignScan_function ReScanForeignScan;
	EndForeignScan_function EndForeignScan;

	/*
	 * Remaining functions are optional.  Set the pointer to NULL for any that
	 * are not provided.
	 */

	/* Functions for remote-join planning */
	GetForeignJoinPaths_function GetForeignJoinPaths;

	/* Functions for remote upper-relation (post scan/join) planning */
	GetForeignUpperPaths_function GetForeignUpperPaths;

	/* Functions for updating foreign tables */
	AddForeignUpdateTargets_function AddForeignUpdateTargets;
	PlanForeignModify_function PlanForeignModify;
	BeginForeignModify_function BeginForeignModify;
	ExecForeignInsert_function ExecForeignInsert;
	ExecForeignBatchInsert_function ExecForeignBatchInsert;
	GetForeignModifyBatchSize_function GetForeignModifyBatchSize;
	ExecForeignUpdate_function ExecForeignUpdate;
	ExecForeignDelete_function ExecForeignDelete;
	EndForeignModify_function EndForeignModify;
	BeginForeignInsert_function BeginForeignInsert;
	EndForeignInsert_function EndForeignInsert;
	IsForeignRelUpdatable_function IsForeignRelUpdatable;
	PlanDirectModify_function PlanDirectModify;
	BeginDirectModify_function BeginDirectModify;
	IterateDirectModify_function IterateDirectModify;
	EndDirectModify_function EndDirectModify;

	/* Functions for SELECT FOR UPDATE/SHARE row locking */
	GetForeignRowMarkType_function GetForeignRowMarkType;
	RefetchForeignRow_function RefetchForeignRow;
	RecheckForeignScan_function RecheckForeignScan;

	/* Support functions for EXPLAIN */
	ExplainForeignScan_function ExplainForeignScan;
	ExplainForeignModify_function ExplainForeignModify;
	ExplainDirectModify_function ExplainDirectModify;

	/* Support functions for ANALYZE */
	AnalyzeForeignTable_function AnalyzeForeignTable;

	/* Support functions for IMPORT FOREIGN SCHEMA */
	ImportForeignSchema_function ImportForeignSchema;

	/* Support functions for TRUNCATE */
	ExecForeignTruncate_function ExecForeignTruncate;

	/* Support functions for parallelism under Gather node */
	IsForeignScanParallelSafe_function IsForeignScanParallelSafe;
	EstimateDSMForeignScan_function EstimateDSMForeignScan;
	InitializeDSMForeignScan_function InitializeDSMForeignScan;
	ReInitializeDSMForeignScan_function ReInitializeDSMForeignScan;
	InitializeWorkerForeignScan_function InitializeWorkerForeignScan;
	ShutdownForeignScan_function ShutdownForeignScan;

	/* Support functions for path reparameterization. */
	ReparameterizeForeignPathByChild_function ReparameterizeForeignPathByChild;

	/* Support functions for asynchronous execution */
	IsForeignPathAsyncCapable_function IsForeignPathAsyncCapable;
	ForeignAsyncRequest_function ForeignAsyncRequest;
	ForeignAsyncConfigureWait_function ForeignAsyncConfigureWait;
	ForeignAsyncNotify_function ForeignAsyncNotify;
} FdwRoutine;


/* Functions in foreign/foreign.c */
extern FdwRoutine *GetFdwRoutine(Oid fdwhandler);
extern Oid	GetForeignServerIdByRelId(Oid relid);
extern FdwRoutine *GetFdwRoutineByServerId(Oid serverid);
extern FdwRoutine *GetFdwRoutineByRelId(Oid relid);
extern FdwRoutine *GetFdwRoutineForRelation(Relation relation, bool makecopy);
extern bool IsImportableForeignTable(const char *tablename,
									 ImportForeignSchemaStmt *stmt);
extern Path *GetExistingLocalJoinPath(RelOptInfo *joinrel);

#endif							/* FDWAPI_H */
