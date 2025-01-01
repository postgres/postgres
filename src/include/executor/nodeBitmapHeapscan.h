/*-------------------------------------------------------------------------
 *
 * nodeBitmapHeapscan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeBitmapHeapscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEBITMAPHEAPSCAN_H
#define NODEBITMAPHEAPSCAN_H

#include "access/parallel.h"
#include "nodes/execnodes.h"

extern BitmapHeapScanState *ExecInitBitmapHeapScan(BitmapHeapScan *node, EState *estate, int eflags);
extern void ExecEndBitmapHeapScan(BitmapHeapScanState *node);
extern void ExecReScanBitmapHeapScan(BitmapHeapScanState *node);
extern void ExecBitmapHeapEstimate(BitmapHeapScanState *node,
								   ParallelContext *pcxt);
extern void ExecBitmapHeapInitializeDSM(BitmapHeapScanState *node,
										ParallelContext *pcxt);
extern void ExecBitmapHeapReInitializeDSM(BitmapHeapScanState *node,
										  ParallelContext *pcxt);
extern void ExecBitmapHeapInitializeWorker(BitmapHeapScanState *node,
										   ParallelWorkerContext *pwcxt);
extern void ExecBitmapHeapRetrieveInstrumentation(BitmapHeapScanState *node);

#endif							/* NODEBITMAPHEAPSCAN_H */
