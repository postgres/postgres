/*-------------------------------------------------------------------------
 *
 * nodeMergejoin.h
 *
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeMergejoin.h,v 1.18 2002/12/05 15:50:38 tgl Exp $
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
