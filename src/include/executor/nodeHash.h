/*-------------------------------------------------------------------------
 *
 * nodeHash.h
 *	  prototypes for nodeHash.c
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeHash.h,v 1.37 2005/04/16 20:07:35 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEHASH_H
#define NODEHASH_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsHash(Hash *node);
extern HashState *ExecInitHash(Hash *node, EState *estate);
extern TupleTableSlot *ExecHash(HashState *node);
extern Node *MultiExecHash(HashState *node);
extern void ExecEndHash(HashState *node);
extern void ExecReScanHash(HashState *node, ExprContext *exprCtxt);

extern HashJoinTable ExecHashTableCreate(Hash *node, List *hashOperators);
extern void ExecHashTableDestroy(HashJoinTable hashtable);
extern void ExecHashTableInsert(HashJoinTable hashtable,
								HeapTuple tuple,
								uint32 hashvalue);
extern uint32 ExecHashGetHashValue(HashJoinTable hashtable,
								   ExprContext *econtext,
								   List *hashkeys);
extern void ExecHashGetBucketAndBatch(HashJoinTable hashtable,
									  uint32 hashvalue,
									  int *bucketno,
									  int *batchno);
extern HeapTuple ExecScanHashBucket(HashJoinState *hjstate,
									ExprContext *econtext);
extern void ExecHashTableReset(HashJoinTable hashtable);
extern void ExecChooseHashTableSize(double ntuples, int tupwidth,
						int *numbuckets,
						int *numbatches);

#endif   /* NODEHASH_H */
