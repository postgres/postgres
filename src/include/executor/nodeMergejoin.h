/*-------------------------------------------------------------------------
 *
 * nodeMergejoin.h
 *
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeMergejoin.h,v 1.22 2004/12/31 22:03:29 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEMERGEJOIN_H
#define NODEMERGEJOIN_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsMergeJoin(MergeJoin *node);
extern MergeJoinState *ExecInitMergeJoin(MergeJoin *node, EState *estate);
extern TupleTableSlot *ExecMergeJoin(MergeJoinState *node);
extern void ExecEndMergeJoin(MergeJoinState *node);
extern void ExecReScanMergeJoin(MergeJoinState *node, ExprContext *exprCtxt);

#endif   /* NODEMERGEJOIN_H */
