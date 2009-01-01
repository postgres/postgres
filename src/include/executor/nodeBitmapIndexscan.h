/*-------------------------------------------------------------------------
 *
 * nodeBitmapIndexscan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeBitmapIndexscan.h,v 1.6 2009/01/01 17:23:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEBITMAPINDEXSCAN_H
#define NODEBITMAPINDEXSCAN_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsBitmapIndexScan(BitmapIndexScan *node);
extern BitmapIndexScanState *ExecInitBitmapIndexScan(BitmapIndexScan *node, EState *estate, int eflags);
extern Node *MultiExecBitmapIndexScan(BitmapIndexScanState *node);
extern void ExecEndBitmapIndexScan(BitmapIndexScanState *node);
extern void ExecBitmapIndexReScan(BitmapIndexScanState *node, ExprContext *exprCtxt);

#endif   /* NODEBITMAPINDEXSCAN_H */
