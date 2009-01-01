/*-------------------------------------------------------------------------
 *
 * nodeLimit.h
 *
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeLimit.h,v 1.16 2009/01/01 17:23:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODELIMIT_H
#define NODELIMIT_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsLimit(Limit *node);
extern LimitState *ExecInitLimit(Limit *node, EState *estate, int eflags);
extern TupleTableSlot *ExecLimit(LimitState *node);
extern void ExecEndLimit(LimitState *node);
extern void ExecReScanLimit(LimitState *node, ExprContext *exprCtxt);

#endif   /* NODELIMIT_H */
