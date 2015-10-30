/*--------------------------------------------------------------------
 * execParallel.h
 *		POSTGRES parallel execution interface
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
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

typedef struct SharedExecutorInstrumentation SharedExecutorInstrumentation;

typedef struct ParallelExecutorInfo
{
	PlanState *planstate;
	ParallelContext *pcxt;
	BufferUsage *buffer_usage;
	SharedExecutorInstrumentation *instrumentation;
	shm_mq_handle **tqueue;
}	ParallelExecutorInfo;

extern ParallelExecutorInfo *ExecInitParallelPlan(PlanState *planstate,
					 EState *estate, int nworkers);
extern void ExecParallelFinish(ParallelExecutorInfo *pei);
extern void ExecParallelCleanup(ParallelExecutorInfo *pei);
extern shm_mq_handle **ExecParallelReinitializeTupleQueues(ParallelContext *pcxt);

#endif   /* EXECPARALLEL_H */
