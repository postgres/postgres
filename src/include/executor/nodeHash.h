/*-------------------------------------------------------------------------
 *
 * nodeHash.h
 *
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeHash.h,v 1.27 2002/12/05 15:50:37 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEHASH_H
#define NODEHASH_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsHash(Hash *node);
extern HashState *ExecInitHash(Hash *node, EState *estate);
extern TupleTableSlot *ExecHash(HashState *node);
extern void ExecEndHash(HashState *node);
extern void ExecReScanHash(HashState *node, ExprContext *exprCtxt);

extern HashJoinTable ExecHashTableCreate(Hash *node);
extern void ExecHashTableDestroy(HashJoinTable hashtable);
extern void ExecHashTableInsert(HashJoinTable hashtable,
					ExprContext *econtext,
					List *hashkeys);
extern int ExecHashGetBucket(HashJoinTable hashtable,
				  ExprContext *econtext,
				  List *hashkeys);
extern HeapTuple ExecScanHashBucket(HashJoinState *hjstate, List *hjclauses,
				   ExprContext *econtext);
extern void ExecHashTableReset(HashJoinTable hashtable, long ntuples);
extern void ExecChooseHashTableSize(double ntuples, int tupwidth,
						int *virtualbuckets,
						int *physicalbuckets,
						int *numbatches);
extern uint32 ComputeHashFunc(Datum key, int typLen, bool byVal);

#endif   /* NODEHASH_H */
