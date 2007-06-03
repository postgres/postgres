/*-------------------------------------------------------------------------
 *
 * nodeHashjoin.h
 *	  prototypes for nodeHashjoin.c
 *
 *
 * Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeHashjoin.h,v 1.35 2007/06/03 17:08:32 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEHASHJOIN_H
#define NODEHASHJOIN_H

#include "nodes/execnodes.h"
#include "storage/buffile.h"

extern int	ExecCountSlotsHashJoin(HashJoin *node);
extern HashJoinState *ExecInitHashJoin(HashJoin *node, EState *estate, int eflags);
extern TupleTableSlot *ExecHashJoin(HashJoinState *node);
extern void ExecEndHashJoin(HashJoinState *node);
extern void ExecReScanHashJoin(HashJoinState *node, ExprContext *exprCtxt);

extern void ExecHashJoinSaveTuple(HashJoinTable hashtable,
								  MinimalTuple tuple, uint32 hashvalue,
								  BufFile **fileptr);

#endif   /* NODEHASHJOIN_H */
