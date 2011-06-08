/*-------------------------------------------------------------------------
 *
 * predicate.h
 *	  POSTGRES public predicate locking definitions.
 *
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/predicate.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PREDICATE_H
#define PREDICATE_H

#include "utils/relcache.h"
#include "utils/snapshot.h"


/*
 * GUC variables
 */
extern int	max_predicate_locks_per_xact;


/* Number of SLRU buffers to use for predicate locking */
#define NUM_OLDSERXID_BUFFERS	16


/*
 * function prototypes
 */

/* housekeeping for shared memory predicate lock structures */
extern void InitPredicateLocks(void);
extern Size PredicateLockShmemSize(void);

extern void CheckPointPredicate(void);

/* predicate lock reporting */
extern bool PageIsPredicateLocked(const Relation relation, const BlockNumber blkno);

/* predicate lock maintenance */
extern Snapshot RegisterSerializableTransaction(Snapshot snapshot);
extern void RegisterPredicateLockingXid(const TransactionId xid);
extern void PredicateLockRelation(const Relation relation);
extern void PredicateLockPage(const Relation relation, const BlockNumber blkno);
extern void PredicateLockTuple(const Relation relation, const HeapTuple tuple);
extern void PredicateLockPageSplit(const Relation relation, const BlockNumber oldblkno, const BlockNumber newblkno);
extern void PredicateLockPageCombine(const Relation relation, const BlockNumber oldblkno, const BlockNumber newblkno);
extern void TransferPredicateLocksToHeapRelation(const Relation relation);
extern void ReleasePredicateLocks(const bool isCommit);

/* conflict detection (may also trigger rollback) */
extern void CheckForSerializableConflictOut(const bool valid, const Relation relation, const HeapTuple tuple, const Buffer buffer);
extern void CheckForSerializableConflictIn(const Relation relation, const HeapTuple tuple, const Buffer buffer);
extern void CheckTableForSerializableConflictIn(const Relation relation);

/* final rollback checking */
extern void PreCommit_CheckForSerializationFailure(void);

/* two-phase commit support */
extern void AtPrepare_PredicateLocks(void);
extern void PostPrepare_PredicateLocks(TransactionId xid);
extern void PredicateLockTwoPhaseFinish(TransactionId xid, bool isCommit);
extern void predicatelock_twophase_recover(TransactionId xid, uint16 info,
							   void *recdata, uint32 len);

#endif   /* PREDICATE_H */
