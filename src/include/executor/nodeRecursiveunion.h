/*-------------------------------------------------------------------------
 *
 * nodeRecursiveunion.h
 *
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeRecursiveunion.h,v 1.1 2008/10/04 21:56:55 tgl Exp $
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
