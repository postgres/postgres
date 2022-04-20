/*-------------------------------------------------------------------------
 *
 * nodeLockRows.h
 *
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeLockRows.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODELOCKROWS_H
#define NODELOCKROWS_H

#include "nodes/execnodes.h"

extern LockRowsState *ExecInitLockRows(LockRows *node, EState *estate, int eflags);
extern void ExecEndLockRows(LockRowsState *node);
extern void ExecReScanLockRows(LockRowsState *node);

#endif							/* NODELOCKROWS_H */
