/*-------------------------------------------------------------------------
 *
 * nodeTidrangescan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeTidrangescan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODETIDRANGESCAN_H
#define NODETIDRANGESCAN_H

#include "access/parallel.h"
#include "nodes/execnodes.h"

extern TidRangeScanState *ExecInitTidRangeScan(TidRangeScan *node,
											   EState *estate, int eflags);
extern void ExecEndTidRangeScan(TidRangeScanState *node);
extern void ExecReScanTidRangeScan(TidRangeScanState *node);

/* parallel scan support */
extern void ExecTidRangeScanEstimate(TidRangeScanState *node, ParallelContext *pcxt);
extern void ExecTidRangeScanInitializeDSM(TidRangeScanState *node, ParallelContext *pcxt);
extern void ExecTidRangeScanReInitializeDSM(TidRangeScanState *node, ParallelContext *pcxt);
extern void ExecTidRangeScanInitializeWorker(TidRangeScanState *node, ParallelWorkerContext *pwcxt);

#endif							/* NODETIDRANGESCAN_H */
