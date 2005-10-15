/*-------------------------------------------------------------------------
 *
 * nodeIndexscan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeIndexscan.h,v 1.24 2005/10/15 02:49:44 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEINDEXSCAN_H
#define NODEINDEXSCAN_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsIndexScan(IndexScan *node);
extern IndexScanState *ExecInitIndexScan(IndexScan *node, EState *estate);
extern TupleTableSlot *ExecIndexScan(IndexScanState *node);
extern void ExecEndIndexScan(IndexScanState *node);
extern void ExecIndexMarkPos(IndexScanState *node);
extern void ExecIndexRestrPos(IndexScanState *node);
extern void ExecIndexReScan(IndexScanState *node, ExprContext *exprCtxt);

/* routines exported to share code with nodeBitmapIndexscan.c */
extern bool ExecIndexBuildScanKeys(PlanState *planstate, List *quals,
					   List *strategies, List *subtypes,
					   ExprState ***runtimeKeyInfo,
					   ScanKey *scanKeys, int *numScanKeys);
extern void ExecIndexEvalRuntimeKeys(ExprContext *econtext,
						 ExprState **run_keys,
						 ScanKey scan_keys,
						 int n_keys);

#endif   /* NODEINDEXSCAN_H */
