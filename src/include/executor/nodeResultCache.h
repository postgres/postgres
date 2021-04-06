/*-------------------------------------------------------------------------
 *
 * nodeResultCache.h
 *
 *
 *
 * Portions Copyright (c) 2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeResultCache.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODERESULTCACHE_H
#define NODERESULTCACHE_H

#include "access/parallel.h"
#include "nodes/execnodes.h"

extern ResultCacheState *ExecInitResultCache(ResultCache *node, EState *estate, int eflags);
extern void ExecEndResultCache(ResultCacheState *node);
extern void ExecReScanResultCache(ResultCacheState *node);
extern double ExecEstimateCacheEntryOverheadBytes(double ntuples);
extern void ExecResultCacheEstimate(ResultCacheState *node,
									ParallelContext *pcxt);
extern void ExecResultCacheInitializeDSM(ResultCacheState *node,
										 ParallelContext *pcxt);
extern void ExecResultCacheInitializeWorker(ResultCacheState *node,
											ParallelWorkerContext *pwcxt);
extern void ExecResultCacheRetrieveInstrumentation(ResultCacheState *node);

#endif							/* NODERESULTCACHE_H */
