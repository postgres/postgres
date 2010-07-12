/*-------------------------------------------------------------------------
 *
 * nodeMergejoin.h
 *
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeMergejoin.h,v 1.30 2010/07/12 17:01:06 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEMERGEJOIN_H
#define NODEMERGEJOIN_H

#include "nodes/execnodes.h"

extern MergeJoinState *ExecInitMergeJoin(MergeJoin *node, EState *estate, int eflags);
extern TupleTableSlot *ExecMergeJoin(MergeJoinState *node);
extern void ExecEndMergeJoin(MergeJoinState *node);
extern void ExecReScanMergeJoin(MergeJoinState *node);

#endif   /* NODEMERGEJOIN_H */
