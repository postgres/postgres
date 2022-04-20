/*-------------------------------------------------------------------------
 *
 * nodeMergejoin.h
 *
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeMergejoin.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEMERGEJOIN_H
#define NODEMERGEJOIN_H

#include "nodes/execnodes.h"

extern MergeJoinState *ExecInitMergeJoin(MergeJoin *node, EState *estate, int eflags);
extern void ExecEndMergeJoin(MergeJoinState *node);
extern void ExecReScanMergeJoin(MergeJoinState *node);

#endif							/* NODEMERGEJOIN_H */
