/*-------------------------------------------------------------------------
 *
 * nodeHashjoin.h
 *	  prototypes for nodeHashjoin.c
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeHashjoin.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEHASHJOIN_H
#define NODEHASHJOIN_H

#include "access/parallel.h"
#include "nodes/execnodes.h"
#include "storage/buffile.h"

extern HashJoinState *ExecInitHashJoin(HashJoin *node, EState *estate, int eflags);
extern void ExecEndHashJoin(HashJoinState *node);
extern void ExecReScanHashJoin(HashJoinState *node);
extern void ExecShutdownHashJoin(HashJoinState *node);
extern void ExecHashJoinEstimate(HashJoinState *state, ParallelContext *pcxt);
extern void ExecHashJoinInitializeDSM(HashJoinState *state, ParallelContext *pcxt);
extern void ExecHashJoinReInitializeDSM(HashJoinState *state, ParallelContext *pcxt);
extern void ExecHashJoinInitializeWorker(HashJoinState *state,
										 ParallelWorkerContext *pwcxt);

extern void ExecHashJoinSaveTuple(MinimalTuple tuple, uint32 hashvalue,
								  BufFile **fileptr, HashJoinTable hashtable);

#endif							/* NODEHASHJOIN_H */
