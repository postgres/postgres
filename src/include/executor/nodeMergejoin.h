/*-------------------------------------------------------------------------
 *
 * nodeMergejoin.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeMergejoin.h,v 1.9 1998/09/01 04:36:02 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEMERGEJOIN_H
#define NODEMERGEJOIN_H

#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"

extern TupleTableSlot *ExecMergeJoin(MergeJoin *node);
extern bool ExecInitMergeJoin(MergeJoin *node, EState *estate, Plan *parent);
extern int	ExecCountSlotsMergeJoin(MergeJoin *node);
extern void ExecEndMergeJoin(MergeJoin *node);
extern void ExecReScanMergeJoin(MergeJoin *node, ExprContext *exprCtxt, Plan *parent);

#endif	 /* NODEMERGEJOIN_H; */
