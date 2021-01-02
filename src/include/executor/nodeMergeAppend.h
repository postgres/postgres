/*-------------------------------------------------------------------------
 *
 * nodeMergeAppend.h
 *
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeMergeAppend.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEMERGEAPPEND_H
#define NODEMERGEAPPEND_H

#include "nodes/execnodes.h"

extern MergeAppendState *ExecInitMergeAppend(MergeAppend *node, EState *estate, int eflags);
extern void ExecEndMergeAppend(MergeAppendState *node);
extern void ExecReScanMergeAppend(MergeAppendState *node);

#endif							/* NODEMERGEAPPEND_H */
