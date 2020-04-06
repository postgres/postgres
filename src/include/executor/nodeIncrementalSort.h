/*-------------------------------------------------------------------------
 *
 * nodeIncrementalSort.h
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeIncrementalSort.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEINCREMENTALSORT_H
#define NODEINCREMENTALSORT_H

#include "access/parallel.h"
#include "nodes/execnodes.h"

extern IncrementalSortState *ExecInitIncrementalSort(IncrementalSort *node, EState *estate, int eflags);
extern void ExecEndIncrementalSort(IncrementalSortState *node);
extern void ExecReScanIncrementalSort(IncrementalSortState *node);

/* parallel instrumentation support */
extern void ExecIncrementalSortEstimate(IncrementalSortState *node, ParallelContext *pcxt);
extern void ExecIncrementalSortInitializeDSM(IncrementalSortState *node, ParallelContext *pcxt);
extern void ExecIncrementalSortInitializeWorker(IncrementalSortState *node, ParallelWorkerContext *pcxt);
extern void ExecIncrementalSortRetrieveInstrumentation(IncrementalSortState *node);

#endif							/* NODEINCREMENTALSORT_H */
