/*-------------------------------------------------------------------------
 *
 * nodeMergejoin.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeMergejoin.h,v 1.6 1997/09/08 21:52:09 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEMERGEJOIN_H
#define NODEMERGEJOIN_H

extern TupleTableSlot *ExecMergeJoin(MergeJoin *node);

extern bool ExecInitMergeJoin(MergeJoin *node, EState *estate, Plan *parent);

extern int	ExecCountSlotsMergeJoin(MergeJoin *node);

extern void ExecEndMergeJoin(MergeJoin *node);

#endif							/* NODEMERGEJOIN_H; */
