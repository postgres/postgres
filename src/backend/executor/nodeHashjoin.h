/*-------------------------------------------------------------------------
 *
 * nodeHashjoin.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeHashjoin.h,v 1.1.1.1 1996/07/09 06:21:26 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	NODEHASHJOIN_H
#define	NODEHASHJOIN_H

extern TupleTableSlot *ExecHashJoin(HashJoin *node);

extern bool ExecInitHashJoin(HashJoin *node, EState *estate, Plan *parent);

extern int ExecCountSlotsHashJoin(HashJoin *node);

extern void ExecEndHashJoin(HashJoin *node);

extern int ExecHashJoinNewBatch(HashJoinState *hjstate);

extern char *ExecHashJoinSaveTuple(HeapTuple heapTuple, char *buffer,
				   File file, char *position);

extern int ExecHashJoinGetBatch(int bucketno, HashJoinTable hashtable,
				int nbatch);


#endif	/* NODEHASHJOIN_H */
