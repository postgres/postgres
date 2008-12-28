/*-------------------------------------------------------------------------
 *
 * nodeWindowAgg.h
 *	  prototypes for nodeWindowAgg.c
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeWindowAgg.h,v 1.1 2008/12/28 18:54:00 tgl Exp $
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
