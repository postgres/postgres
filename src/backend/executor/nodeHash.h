/*-------------------------------------------------------------------------
 *
 * nodeHash.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeHash.h,v 1.1.1.1 1996/07/09 06:21:26 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	NODEHASH_H
#define	NODEHASH_H

extern TupleTableSlot *ExecHash(Hash *node);
extern bool ExecInitHash(Hash *node, EState *estate, Plan *parent);
extern int ExecCountSlotsHash(Hash *node);
extern void ExecEndHash(Hash *node);
extern RelativeAddr hashTableAlloc(int size, HashJoinTable hashtable);
extern HashJoinTable ExecHashTableCreate(Hash *node);
extern void ExecHashTableInsert(HashJoinTable hashtable, ExprContext *econtext,
				Var *hashkey, File *batches);
extern void ExecHashTableDestroy(HashJoinTable hashtable);
extern int ExecHashGetBucket(HashJoinTable hashtable, ExprContext *econtext,
			     Var *hashkey);
extern void ExecHashOverflowInsert(HashJoinTable hashtable, HashBucket bucket,
				   HeapTuple heapTuple);
extern HeapTuple ExecScanHashBucket(HashJoinState *hjstate, HashBucket bucket,
				    HeapTuple curtuple, List *hjclauses,
				    ExprContext *econtext);
extern int ExecHashPartition(Hash *node);
extern void ExecHashTableReset(HashJoinTable hashtable, int ntuples);

#endif	/* NODEHASH_H */
