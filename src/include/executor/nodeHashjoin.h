/*-------------------------------------------------------------------------
 *
 * nodeHashjoin.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeHashjoin.h,v 1.7 1998/01/24 22:49:04 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEHASHJOIN_H
#define NODEHASHJOIN_H

#include "nodes/plannodes.h"
#include "nodes/execnodes.h"
#include "utils/syscache.h"

extern TupleTableSlot *ExecHashJoin(HashJoin *node);
extern bool ExecInitHashJoin(HashJoin *node, EState *estate, Plan *parent);
extern int	ExecCountSlotsHashJoin(HashJoin *node);
extern void ExecEndHashJoin(HashJoin *node);
extern char * ExecHashJoinSaveTuple(HeapTuple heapTuple, char *buffer,
					  File file, char *position);


#endif							/* NODEHASHJOIN_H */
