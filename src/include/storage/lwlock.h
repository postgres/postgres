/*-------------------------------------------------------------------------
 *
 * lwlock.h
 *	  Lightweight lock manager
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/lwlock.h,v 1.32 2006/10/15 22:04:07 tgl Exp $
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

/*
 * We have a number of predefined LWLocks, plus a bunch of LWLocks that are
 * dynamically assigned (e.g., for shared buffers).  The LWLock structures
 * live in shared memory (since they contain shared data) and are identified
 * by values of this enumerated type.  We abuse the notion of an enum somewhat
 * by allowing values not listed in the enum declaration to be assigned.
 * The extra value MaxDynamicLWLock is there to keep the compiler from
 * deciding that the enum can be represented as char or short ...
 */
typedef enum LWLockId
{
	BufFreelistLock,
	ShmemIndexLock,
	OidGenLock,
	XidGenLock,
	ProcArrayLock,
	SInvalLock,
	FreeSpaceLock,
	WALInsertLock,
	WALWriteLock,
	ControlFileLock,
	CheckpointLock,
	CheckpointStartLock,
	CLogControlLock,
	SubtransControlLock,
	MultiXactGenLock,
	MultiXactOffsetControlLock,
	MultiXactMemberControlLock,
	RelCacheInitLock,
	BgWriterCommLock,
	TwoPhaseStateLock,
	TablespaceCreateLock,
	BtreeVacuumLock,
	AddinShmemInitLock,
	FirstBufMappingLock,
	FirstLockMgrLock = FirstBufMappingLock + NUM_BUFFER_PARTITIONS,

	/* must be last except for MaxDynamicLWLock: */
	NumFixedLWLocks = FirstLockMgrLock + NUM_LOCK_PARTITIONS,

	MaxDynamicLWLock = 1000000000
} LWLockId;


typedef enum LWLockMode
{
	LW_EXCLUSIVE,
	LW_SHARED
} LWLockMode;


#ifdef LOCK_DEBUG
extern bool Trace_lwlocks;
#endif

extern LWLockId LWLockAssign(void);
extern void LWLockAcquire(LWLockId lockid, LWLockMode mode);
extern bool LWLockConditionalAcquire(LWLockId lockid, LWLockMode mode);
extern void LWLockRelease(LWLockId lockid);
extern void LWLockReleaseAll(void);
extern bool LWLockHeldByMe(LWLockId lockid);

extern int	NumLWLocks(void);
extern Size LWLockShmemSize(void);
extern void CreateLWLocks(void);

extern void RequestAddinLWLocks(int n);

#endif   /* LWLOCK_H */
