/*-------------------------------------------------------------------------
 *
 * nodeMergejoin.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeMergejoin.h,v 1.7 1997/11/26 01:12:55 momjian Exp $
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

#endif							/* NODEMERGEJOIN_H; */
