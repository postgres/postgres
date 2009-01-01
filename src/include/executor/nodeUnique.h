/*-------------------------------------------------------------------------
 *
 * nodeUnique.h
 *
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeUnique.h,v 1.25 2009/01/01 17:23:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEUNIQUE_H
#define NODEUNIQUE_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsUnique(Unique *node);
extern UniqueState *ExecInitUnique(Unique *node, EState *estate, int eflags);
extern TupleTableSlot *ExecUnique(UniqueState *node);
extern void ExecEndUnique(UniqueState *node);
extern void ExecReScanUnique(UniqueState *node, ExprContext *exprCtxt);

#endif   /* NODEUNIQUE_H */
