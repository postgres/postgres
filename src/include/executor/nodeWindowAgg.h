/*-------------------------------------------------------------------------
 *
 * nodeWindowAgg.h
 *	  prototypes for nodeWindowAgg.c
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeWindowAgg.h,v 1.2 2009/01/01 17:23:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEWINDOWAGG_H
#define NODEWINDOWAGG_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsWindowAgg(WindowAgg *node);
extern WindowAggState *ExecInitWindowAgg(WindowAgg *node, EState *estate, int eflags);
extern TupleTableSlot *ExecWindowAgg(WindowAggState *node);
extern void ExecEndWindowAgg(WindowAggState *node);
extern void ExecReScanWindowAgg(WindowAggState *node, ExprContext *exprCtxt);

#endif   /* NODEWINDOWAGG_H */
