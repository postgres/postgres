/*-------------------------------------------------------------------------
 *
 * fdwapi.h
 *	  API for foreign-data wrappers
 *
 * Copyright (c) 2010-2011, PostgreSQL Global Development Group
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
 * FdwPlan is the information returned to the planner by PlanForeignScan.
 */
typedef struct FdwPlan
{
	NodeTag		type;

	/*
	 * Cost estimation info. The startup_cost is time before retrieving the
	 * first row, so it should include costs of connecting to the remote host,
	 * sending over the query, etc.  Note that PlanForeignScan also ought to
	 * set baserel->rows and baserel->width if it can produce any usable
	 * estimates of those values.
	 */
	Cost		startup_cost;	/* cost expended before fetching any tuples */
	Cost		total_cost;		/* total cost (assuming all tuples fetched) */

	/*
	 * FDW private data, which will be available at execution time.
	 *
	 * Note that everything in this list must be copiable by copyObject(). One
	 * way to store an arbitrary blob of bytes is to represent it as a bytea
	 * Const.  Usually, though, you'll be better off choosing a representation
	 * that can be dumped usefully by nodeToString().
	 */
	List	   *fdw_private;
} FdwPlan;


/*
 * Callback function signatures --- see fdwhandler.sgml for more info.
 */

typedef FdwPlan *(*PlanForeignScan_function) (Oid foreigntableid,
														  PlannerInfo *root,
														RelOptInfo *baserel);

typedef void (*ExplainForeignScan_function) (ForeignScanState *node,
													struct ExplainState *es);

typedef void (*BeginForeignScan_function) (ForeignScanState *node,
													   int eflags);

typedef TupleTableSlot *(*IterateForeignScan_function) (ForeignScanState *node);

typedef void (*ReScanForeignScan_function) (ForeignScanState *node);

typedef void (*EndForeignScan_function) (ForeignScanState *node);


/*
 * FdwRoutine is the struct returned by a foreign-data wrapper's handler
 * function.  It provides pointers to the callback functions needed by the
 * planner and executor.
 *
 * Currently, all functions must be supplied.  Later there may be optional
 * additions.  It's recommended that the handler initialize the struct with
 * makeNode(FdwRoutine) so that all fields are set to zero.
 */
typedef struct FdwRoutine
{
	NodeTag		type;

	PlanForeignScan_function PlanForeignScan;
	ExplainForeignScan_function ExplainForeignScan;
	BeginForeignScan_function BeginForeignScan;
	IterateForeignScan_function IterateForeignScan;
	ReScanForeignScan_function ReScanForeignScan;
	EndForeignScan_function EndForeignScan;
} FdwRoutine;


/* Functions in foreign/foreign.c */
extern FdwRoutine *GetFdwRoutine(Oid fdwhandler);
extern FdwRoutine *GetFdwRoutineByRelId(Oid relid);

#endif   /* FDWAPI_H */
