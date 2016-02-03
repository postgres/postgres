/*-------------------------------------------------------------------------
 *
 * rewriteheap.h
 *	  Declarations for heap rewrite support functions
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * src/include/access/rewriteheap.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITE_HEAP_H
#define REWRITE_HEAP_H

#include "access/htup.h"
#include "storage/itemptr.h"
#include "storage/relfilenode.h"
#include "utils/relcache.h"

/* struct definition is private to rewriteheap.c */
typedef struct RewriteStateData *RewriteState;

extern RewriteState begin_heap_rewrite(Relation OldHeap, Relation NewHeap,
				   TransactionId OldestXmin, TransactionId FreezeXid,
				   MultiXactId MultiXactCutoff, bool use_wal);
extern void end_heap_rewrite(RewriteState state);
extern void rewrite_heap_tuple(RewriteState state, HeapTuple oldTuple,
				   HeapTuple newTuple);
extern bool rewrite_heap_dead_tuple(RewriteState state, HeapTuple oldTuple);

/*
 * On-Disk data format for an individual logical rewrite mapping.
 */
typedef struct LogicalRewriteMappingData
{
	RelFileNode old_node;
	RelFileNode new_node;
	ItemPointerData old_tid;
	ItemPointerData new_tid;
} LogicalRewriteMappingData;

/* ---
 * The filename consists of the following, dash separated,
 * components:
 * 1) database oid or InvalidOid for shared relations
 * 2) the oid of the relation
 * 3) xid we are mapping for
 * 4) upper 32bit of the LSN at which a rewrite started
 * 5) lower 32bit of the LSN at which a rewrite started
 * 6) xid of the xact performing the mapping
 * ---
 */
#define LOGICAL_REWRITE_FORMAT "map-%x-%x-%X_%X-%x-%x"
void		CheckPointLogicalRewriteHeap(void);

#endif   /* REWRITE_HEAP_H */
