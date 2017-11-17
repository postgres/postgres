/*-------------------------------------------------------------------------
 *
 * nodeSort.h
 *
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeSort.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODESORT_H
#define NODESORT_H

#include "access/parallel.h"
#include "nodes/execnodes.h"

extern SortState *ExecInitSort(Sort *node, EState *estate, int eflags);
extern void ExecEndSort(SortState *node);
extern void ExecSortMarkPos(SortState *node);
extern void ExecSortRestrPos(SortState *node);
extern void ExecReScanSort(SortState *node);

/* parallel instrumentation support */
extern void ExecSortEstimate(SortState *node, ParallelContext *pcxt);
extern void ExecSortInitializeDSM(SortState *node, ParallelContext *pcxt);
extern void ExecSortReInitializeDSM(SortState *node, ParallelContext *pcxt);
extern void ExecSortInitializeWorker(SortState *node, ParallelWorkerContext *pwcxt);
extern void ExecSortRetrieveInstrumentation(SortState *node);

#endif							/* NODESORT_H */
