/*-------------------------------------------------------------------------
 *
 * nodeLockRows.h
 *
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeLockRows.h,v 1.3 2010/07/12 17:01:06 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODELOCKROWS_H
#define NODELOCKROWS_H

#include "nodes/execnodes.h"

extern LockRowsState *ExecInitLockRows(LockRows *node, EState *estate, int eflags);
extern TupleTableSlot *ExecLockRows(LockRowsState *node);
extern void ExecEndLockRows(LockRowsState *node);
extern void ExecReScanLockRows(LockRowsState *node);

#endif   /* NODELOCKROWS_H */
