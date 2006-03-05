/*-------------------------------------------------------------------------
 *
 * nodeNestloop.h
 *
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeNestloop.h,v 1.25 2006/03/05 15:58:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODENESTLOOP_H
#define NODENESTLOOP_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsNestLoop(NestLoop *node);
extern NestLoopState *ExecInitNestLoop(NestLoop *node, EState *estate, int eflags);
extern TupleTableSlot *ExecNestLoop(NestLoopState *node);
extern void ExecEndNestLoop(NestLoopState *node);
extern void ExecReScanNestLoop(NestLoopState *node, ExprContext *exprCtxt);

#endif   /* NODENESTLOOP_H */
