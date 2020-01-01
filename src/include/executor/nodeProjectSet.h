/*-------------------------------------------------------------------------
 *
 * nodeProjectSet.h
 *
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeProjectSet.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEPROJECTSET_H
#define NODEPROJECTSET_H

#include "nodes/execnodes.h"

extern ProjectSetState *ExecInitProjectSet(ProjectSet *node, EState *estate, int eflags);
extern void ExecEndProjectSet(ProjectSetState *node);
extern void ExecReScanProjectSet(ProjectSetState *node);

#endif							/* NODEPROJECTSET_H */
