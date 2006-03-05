/*-------------------------------------------------------------------------
 *
 * nodeAppend.h
 *
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeAppend.h,v 1.25 2006/03/05 15:58:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEAPPEND_H
#define NODEAPPEND_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsAppend(Append *node);
extern AppendState *ExecInitAppend(Append *node, EState *estate, int eflags);
extern TupleTableSlot *ExecAppend(AppendState *node);
extern void ExecEndAppend(AppendState *node);
extern void ExecReScanAppend(AppendState *node, ExprContext *exprCtxt);

#endif   /* NODEAPPEND_H */
