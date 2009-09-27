/*-------------------------------------------------------------------------
 *
 * nodeBitmapIndexscan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeBitmapIndexscan.h,v 1.7 2009/09/27 21:10:53 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEBITMAPINDEXSCAN_H
#define NODEBITMAPINDEXSCAN_H

#include "nodes/execnodes.h"

extern BitmapIndexScanState *ExecInitBitmapIndexScan(BitmapIndexScan *node, EState *estate, int eflags);
extern Node *MultiExecBitmapIndexScan(BitmapIndexScanState *node);
extern void ExecEndBitmapIndexScan(BitmapIndexScanState *node);
extern void ExecBitmapIndexReScan(BitmapIndexScanState *node, ExprContext *exprCtxt);

#endif   /* NODEBITMAPINDEXSCAN_H */
