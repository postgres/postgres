/*--------------------------------------------------------------------
 * execParallel.h
 *		POSTGRES parallel execution interface
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/executor/execParallel.h
 *--------------------------------------------------------------------
 */

#ifndef EXECPARALLEL_H
#define EXECPARALLEL_H

#include "access/parallel.h"
#include "nodes/execnodes.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "utils/dsa.h"

typedef struct SharedExecutorInstrumentation SharedExecutorInstrumentation;

typedef struct ParallelExecutorInfo
{
	PlanState  *planstate;		/* plan subtree we're running in parallel */
	ParallelContext *pcxt;		/* parallel context we're using */
	BufferUsage *buffer_usage;	/* points to bufusage area in DSM */
	SharedExecutorInstrumentation *instrumentation; /* optional */
	dsa_area   *area;			/* points to DSA area in DSM */
	bool		finished;		/* set true by ExecParallelFinish */
	/* These two arrays have pcxt->nworkers_launched entries: */
	shm_mq_handle **tqueue;		/* tuple queues for worker output */
	struct TupleQueueReader **reader;	/* tuple reader/writer support */
} ParallelExecutorInfo;

extern ParallelExecutorInfo *ExecInitParallelPlan(PlanState *planstate,
					 EState *estate, int nworkers);
extern void ExecParallelCreateReaders(ParallelExecutorInfo *pei,
						  TupleDesc tupDesc);
extern void ExecParallelFinish(ParallelExecutorInfo *pei);
extern void ExecParallelCleanup(ParallelExecutorInfo *pei);
extern void ExecParallelReinitialize(PlanState *planstate,
						 ParallelExecutorInfo *pei);

extern void ParallelQueryMain(dsm_segment *seg, shm_toc *toc);

#endif							/* EXECPARALLEL_H */
