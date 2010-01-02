/*-------------------------------------------------------------------------
 *
 * nodeLockRows.h
 *
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeLockRows.h,v 1.2 2010/01/02 16:58:03 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODELOCKROWS_H
#define NODELOCKROWS_H

#include "nodes/execnodes.h"

extern LockRowsState *ExecInitLockRows(LockRows *node, EState *estate, int eflags);
extern TupleTableSlot *ExecLockRows(LockRowsState *node);
extern void ExecEndLockRows(LockRowsState *node);
extern void ExecReScanLockRows(LockRowsState *node, ExprContext *exprCtxt);

#endif   /* NODELOCKROWS_H */
