/*-------------------------------------------------------------------------
 *
 * nodeRecursiveunion.h
 *
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeRecursiveunion.h,v 1.2 2009/01/01 17:23:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODERECURSIVEUNION_H
#define NODERECURSIVEUNION_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsRecursiveUnion(RecursiveUnion *node);
extern RecursiveUnionState *ExecInitRecursiveUnion(RecursiveUnion *node, EState *estate, int eflags);
extern TupleTableSlot *ExecRecursiveUnion(RecursiveUnionState *node);
extern void ExecEndRecursiveUnion(RecursiveUnionState *node);
extern void ExecRecursiveUnionReScan(RecursiveUnionState *node, ExprContext *exprCtxt);

#endif   /* NODERECURSIVEUNION_H */
