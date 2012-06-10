/*-------------------------------------------------------------------------
 *
 * lwlock.h
 *	  Lightweight lock manager
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/lwlock.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LWLOCK_H
#define LWLOCK_H

/*
 * It's a bit odd to declare NUM_BUFFER_PARTITIONS and NUM_LOCK_PARTITIONS
 * here, but we need them to set up enum LWLockId correctly, and having
 * this file include lock.h or bufmgr.h would be backwards.
 */

/* Number of partitions of the shared buffer mapping hashtable */
#define NUM_BUFFER_PARTITIONS  16

/* Number of partitions the shared lock tables are divided into */
#define LOG2_NUM_LOCK_PARTITIONS  4
#define NUM_LOCK_PARTITIONS  (1 << LOG2_NUM_LOCK_PARTITIONS)

/* Number of partitions the shared predicate lock tables are divided into */
#define LOG2_NUM_PREDICATELOCK_PARTITIONS  4
#define NUM_PREDICATELOCK_PARTITIONS  (1 << LOG2_NUM_PREDICATELOCK_PARTITIONS)

/*
 * We have a number of predefined LWLocks, plus a bunch of LWLocks that are
 * dynamically assigned (e.g., for shared buffers).  The LWLock structures
 * live in shared memory (since they contain shared data) and are identified
 * by values of this enumerated type.  We abuse the notion of an enum somewhat
 * by allowing values not listed in the enum declaration to be assigned.
 * The extra value MaxDynamicLWLock is there to keep the compiler from
 * deciding that the enum can be represented as char or short ...
 *
 * If you remove a lock, please replace it with a placeholder. This retains
 * the lock numbering, which is helpful for DTrace and other external
 * debugging scripts.
 */
typedef enum LWLockId
{
	BufFreelistLock,
	ShmemIndexLock,
	OidGenLock,
	XidGenLock,
	ProcArrayLock,
	SInvalReadLock,
	SInvalWriteLock,
	WALInsertLock,
	WALWriteLock,
	ControlFileLock,
	CheckpointLock,
	CLogControlLock,
	SubtransControlLock,
	MultiXactGenLock,
	MultiXactOffsetControlLock,
	MultiXactMemberControlLock,
	RelCacheInitLock,
	CheckpointerCommLock,
	TwoPhaseStateLock,
	TablespaceCreateLock,
	BtreeVacuumLock,
	AddinShmemInitLock,
	AutovacuumLock,
	AutovacuumScheduleLock,
	SyncScanLock,
	RelationMappingLock,
	AsyncCtlLock,
	AsyncQueueLock,
	SerializableXactHashLock,
	SerializableFinishedListLock,
	SerializablePredicateLockListLock,
	OldSerXidLock,
	SyncRepLock,
	/* Individual lock IDs end here */
	FirstBufMappingLock,
	FirstLockMgrLock = FirstBufMappingLock + NUM_BUFFER_PARTITIONS,
	FirstPredicateLockMgrLock = FirstLockMgrLock + NUM_LOCK_PARTITIONS,

	/* must be last except for MaxDynamicLWLock: */
	NumFixedLWLocks = FirstPredicateLockMgrLock + NUM_PREDICATELOCK_PARTITIONS,

	MaxDynamicLWLock = 1000000000
} LWLockId;


typedef enum LWLockMode
{
	LW_EXCLUSIVE,
	LW_SHARED,
	LW_WAIT_UNTIL_FREE			/* A special mode used in PGPROC->lwlockMode,
								 * when waiting for lock to become free. Not
								 * to be used as LWLockAcquire argument */
} LWLockMode;


#ifdef LOCK_DEBUG
extern bool Trace_lwlocks;
#endif

extern LWLockId LWLockAssign(void);
extern void LWLockAcquire(LWLockId lockid, LWLockMode mode);
extern bool LWLockConditionalAcquire(LWLockId lockid, LWLockMode mode);
extern bool LWLockAcquireOrWait(LWLockId lockid, LWLockMode mode);
extern void LWLockRelease(LWLockId lockid);
extern void LWLockReleaseAll(void);
extern bool LWLockHeldByMe(LWLockId lockid);

extern int	NumLWLocks(void);
extern Size LWLockShmemSize(void);
extern void CreateLWLocks(void);

extern void RequestAddinLWLocks(int n);

#endif   /* LWLOCK_H */
