/*-------------------------------------------------------------------------
 *
 * nodeIndexscan.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeIndexscan.h,v 1.7 1998/09/01 04:35:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEINDEXSCAN_H
#define NODEINDEXSCAN_H

#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"

extern TupleTableSlot *ExecIndexScan(IndexScan *node);
extern void ExecIndexReScan(IndexScan *node, ExprContext *exprCtxt, Plan *parent);
extern void ExecEndIndexScan(IndexScan *node);
extern void ExecIndexMarkPos(IndexScan *node);
extern void ExecIndexRestrPos(IndexScan *node);
extern void ExecUpdateIndexScanKeys(IndexScan *node, ExprContext *econtext);
extern bool ExecInitIndexScan(IndexScan *node, EState *estate, Plan *parent);
extern int	ExecCountSlotsIndexScan(IndexScan *node);
extern void ExecIndexReScan(IndexScan *node, ExprContext *exprCtxt, Plan *parent);

#endif	 /* NODEINDEXSCAN_H */
