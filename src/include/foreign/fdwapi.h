/*-------------------------------------------------------------------------
 *
 * fdwapi.h
 *	  API for foreign-data wrappers
 *
 * Copyright (c) 2010-2013, PostgreSQL Global Development Group
 *
 * src/include/foreign/fdwapi.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FDWAPI_H
#define FDWAPI_H

#include "nodes/execnodes.h"
#include "nodes/relation.h"

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
														 List *scan_clauses);

typedef void (*BeginForeignScan_function) (ForeignScanState *node,
													   int eflags);

typedef TupleTableSlot *(*IterateForeignScan_function) (ForeignScanState *node);

typedef void (*ReScanForeignScan_function) (ForeignScanState *node);

typedef void (*EndForeignScan_function) (ForeignScanState *node);

typedef void (*AddForeignUpdateTargets_function) (Query *parsetree,
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

typedef int (*IsForeignRelUpdatable_function) (Relation rel);

typedef void (*ExplainForeignScan_function) (ForeignScanState *node,
													struct ExplainState *es);

typedef void (*ExplainForeignModify_function) (ModifyTableState *mtstate,
														ResultRelInfo *rinfo,
														   List *fdw_private,
														   int subplan_index,
													struct ExplainState *es);

typedef int (*AcquireSampleRowsFunc) (Relation relation, int elevel,
											   HeapTuple *rows, int targrows,
												  double *totalrows,
												  double *totaldeadrows);

typedef bool (*AnalyzeForeignTable_function) (Relation relation,
												 AcquireSampleRowsFunc *func,
													BlockNumber *totalpages);

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

	/* Functions for updating foreign tables */
	AddForeignUpdateTargets_function AddForeignUpdateTargets;
	PlanForeignModify_function PlanForeignModify;
	BeginForeignModify_function BeginForeignModify;
	ExecForeignInsert_function ExecForeignInsert;
	ExecForeignUpdate_function ExecForeignUpdate;
	ExecForeignDelete_function ExecForeignDelete;
	EndForeignModify_function EndForeignModify;
	IsForeignRelUpdatable_function IsForeignRelUpdatable;

	/* Support functions for EXPLAIN */
	ExplainForeignScan_function ExplainForeignScan;
	ExplainForeignModify_function ExplainForeignModify;

	/* Support functions for ANALYZE */
	AnalyzeForeignTable_function AnalyzeForeignTable;
} FdwRoutine;


/* Functions in foreign/foreign.c */
extern FdwRoutine *GetFdwRoutine(Oid fdwhandler);
extern FdwRoutine *GetFdwRoutineByRelId(Oid relid);
extern FdwRoutine *GetFdwRoutineForRelation(Relation relation, bool makecopy);

#endif   /* FDWAPI_H */
