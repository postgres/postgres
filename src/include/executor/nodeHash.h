/*-------------------------------------------------------------------------
 *
 * nodeHash.h
 *
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeHash.h,v 1.25 2002/11/06 22:31:24 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEHASH_H
#define NODEHASH_H

#include "nodes/plannodes.h"

extern TupleTableSlot *ExecHash(Hash *node);
extern bool ExecInitHash(Hash *node, EState *estate, Plan *parent);
extern int	ExecCountSlotsHash(Hash *node);
extern void ExecEndHash(Hash *node);
extern HashJoinTable ExecHashTableCreate(Hash *node);
extern void ExecHashTableDestroy(HashJoinTable hashtable);
extern void ExecHashTableInsert(HashJoinTable hashtable,
					ExprContext *econtext,
					Node *hashkey);
extern int ExecHashGetBucket(HashJoinTable hashtable,
				  ExprContext *econtext,
				  Node *hashkey);
extern HeapTuple ExecScanHashBucket(HashJoinState *hjstate, List *hjclauses,
				   ExprContext *econtext);
extern void ExecHashTableReset(HashJoinTable hashtable, long ntuples);
extern void ExecReScanHash(Hash *node, ExprContext *exprCtxt, Plan *parent);
extern void ExecChooseHashTableSize(double ntuples, int tupwidth,
						int *virtualbuckets,
						int *physicalbuckets,
						int *numbatches);
extern uint32 ComputeHashFunc(Datum key, int typLen, bool byVal);

#endif   /* NODEHASH_H */
