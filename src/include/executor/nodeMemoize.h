/*-------------------------------------------------------------------------
 *
 * nodeMemoize.h
 *
 *
 *
 * Portions Copyright (c) 2021-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeMemoize.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEMEMOIZE_H
#define NODEMEMOIZE_H

#include "access/parallel.h"
#include "nodes/execnodes.h"

extern MemoizeState *ExecInitMemoize(Memoize *node, EState *estate, int eflags);
extern void ExecEndMemoize(MemoizeState *node);
extern void ExecReScanMemoize(MemoizeState *node);
extern double ExecEstimateCacheEntryOverheadBytes(double ntuples);
extern void ExecMemoizeEstimate(MemoizeState *node,
								ParallelContext *pcxt);
extern void ExecMemoizeInitializeDSM(MemoizeState *node,
									 ParallelContext *pcxt);
extern void ExecMemoizeInitializeWorker(MemoizeState *node,
										ParallelWorkerContext *pwcxt);
extern void ExecMemoizeRetrieveInstrumentation(MemoizeState *node);

#endif							/* NODEMEMOIZE_H */
