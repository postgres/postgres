/*-------------------------------------------------------------------------
 *
 * rewriteheap.h
 *	  Declarations for heap rewrite support functions
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
#include "storage/relfilelocator.h"
#include "utils/relcache.h"

/* struct definition is private to rewriteheap.c */
typedef struct RewriteStateData *RewriteState;

extern RewriteState begin_heap_rewrite(Relation old_heap, Relation new_heap,
									   TransactionId oldest_xmin, TransactionId freeze_xid,
									   MultiXactId cutoff_multi);
extern void end_heap_rewrite(RewriteState state);
extern void rewrite_heap_tuple(RewriteState state, HeapTuple old_tuple,
							   HeapTuple new_tuple);
extern bool rewrite_heap_dead_tuple(RewriteState state, HeapTuple old_tuple);

/*
 * On-Disk data format for an individual logical rewrite mapping.
 */
typedef struct LogicalRewriteMappingData
{
	RelFileLocator old_locator;
	RelFileLocator new_locator;
	ItemPointerData old_tid;
	ItemPointerData new_tid;
} LogicalRewriteMappingData;

/* ---
 * The filename consists of the following, dash separated,
 * components:
 * 1) database oid or InvalidOid for shared relations
 * 2) the oid of the relation
 * 3) upper 32bit of the LSN at which a rewrite started
 * 4) lower 32bit of the LSN at which a rewrite started
 * 5) xid we are mapping for
 * 6) xid of the xact performing the mapping
 * ---
 */
#define LOGICAL_REWRITE_FORMAT "map-%x-%x-%X_%X-%x-%x"
extern void CheckPointLogicalRewriteHeap(void);

#endif							/* REWRITE_HEAP_H */
