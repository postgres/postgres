/*-------------------------------------------------------------------------
 *
 * nodeMergejoin.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeMergejoin.h,v 1.3 1997/08/19 21:38:22 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	NODEMERGEJOIN_H
#define	NODEMERGEJOIN_H

extern TupleTableSlot *ExecMergeJoin(MergeJoin *node);

extern bool ExecInitMergeJoin(MergeJoin *node, EState *estate, Plan *parent);

extern int ExecCountSlotsMergeJoin(MergeJoin *node);

extern void ExecEndMergeJoin(MergeJoin *node);

#endif	/* NODEMERGEJOIN_H; */
