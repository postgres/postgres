/*-------------------------------------------------------------------------
 *
 * nodeSetOp.h
 *
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeSetOp.h,v 1.15 2008/01/01 19:45:57 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODESETOP_H
#define NODESETOP_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsSetOp(SetOp *node);
extern SetOpState *ExecInitSetOp(SetOp *node, EState *estate, int eflags);
extern TupleTableSlot *ExecSetOp(SetOpState *node);
extern void ExecEndSetOp(SetOpState *node);
extern void ExecReScanSetOp(SetOpState *node, ExprContext *exprCtxt);

#endif   /* NODESETOP_H */
