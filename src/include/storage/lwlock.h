/*-------------------------------------------------------------------------
 *
 * lwlock.h
 *	  Lightweight lock manager
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: lwlock.h,v 1.8.4.1 2004/08/11 04:08:02 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef LWLOCK_H
#define LWLOCK_H

/*
 * We have a number of predefined LWLocks, plus a bunch of LWLocks that are
 * dynamically assigned (for shared buffers).  The LWLock structures live
 * in shared memory (since they contain shared data) and are identified by
 * values of this enumerated type.	We abuse the notion of an enum somewhat
 * by allowing values not listed in the enum declaration to be assigned.
 * The extra value MaxDynamicLWLock is there to keep the compiler from
 * deciding that the enum can be represented as char or short ...
 */
typedef enum LWLockId
{
	BufMgrLock,
	LockMgrLock,
	OidGenLock,
	XidGenLock,
	ShmemIndexLock,
	SInvalLock,
	FreeSpaceLock,
	MMCacheLock,
	WALInsertLock,
	WALWriteLock,
	ControlFileLock,
	CheckpointLock,
	CheckpointStartLock,
	RelCacheInitLock,

	NumFixedLWLocks,			/* must be last except for
								 * MaxDynamicLWLock */

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

extern int	NumLWLocks(void);
extern int	LWLockShmemSize(void);
extern void CreateLWLocks(void);

#endif   /* LWLOCK_H */
