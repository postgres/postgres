/*-------------------------------------------------------------------------
 *
 * nodeHash.h
 *
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeHash.h,v 1.18 2001/01/24 19:43:23 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEHASH_H
#define NODEHASH_H

#include "nodes/plannodes.h"

/* NTUP_PER_BUCKET is exported because planner wants to see it */
#define NTUP_PER_BUCKET			10

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

#endif	 /* NODEHASH_H */
