/*-------------------------------------------------------------------------
 *
 * spin.c
 *	  routines for managing spin locks
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/ipc/Attic/spin.c,v 1.23 2000/04/12 04:58:09 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * POSTGRES has two kinds of locks: semaphores (which put the
 * process to sleep) and spinlocks (which are supposed to be
 * short term locks).  Currently both are implemented as SysV
 * semaphores, but presumably this can change if we move to
 * a machine with a test-and-set (TAS) instruction.  Its probably
 * a good idea to think about (and allocate) short term and long
 * term semaphores separately anyway.
 *
 * NOTE: These routines are not supposed to be widely used in Postgres.
 *		 They are preserved solely for the purpose of porting Mark Sullivan's
 *		 buffer manager to Postgres.
 */
#include <errno.h>
#include "postgres.h"

#ifndef HAS_TEST_AND_SET
#include <sys/sem.h>
#endif

#include "storage/proc.h"
#include "storage/s_lock.h"

/* globals used in this file */
IpcSemaphoreId SpinLockId;

#ifdef HAS_TEST_AND_SET
/* real spin lock implementations */

void
CreateSpinlocks(IPCKey key)
{
	/* the spin lock shared memory must have been created by now */
	return;
}

void
InitSpinLocks(void)
{
	extern SPINLOCK ShmemLock;
	extern SPINLOCK ShmemIndexLock;
	extern SPINLOCK BufMgrLock;
	extern SPINLOCK LockMgrLock;
	extern SPINLOCK ProcStructLock;
	extern SPINLOCK SInvalLock;
	extern SPINLOCK OidGenLockId;
	extern SPINLOCK XidGenLockId;
	extern SPINLOCK	ControlFileLockId;
#ifdef STABLE_MEMORY_STORAGE
	extern SPINLOCK MMCacheLock;

#endif

	/* These six spinlocks have fixed location is shmem */
	ShmemLock = (SPINLOCK) SHMEMLOCKID;
	ShmemIndexLock = (SPINLOCK) SHMEMINDEXLOCKID;
	BufMgrLock = (SPINLOCK) BUFMGRLOCKID;
	LockMgrLock = (SPINLOCK) LOCKMGRLOCKID;
	ProcStructLock = (SPINLOCK) PROCSTRUCTLOCKID;
	SInvalLock = (SPINLOCK) SINVALLOCKID;
	OidGenLockId = (SPINLOCK) OIDGENLOCKID;
	XidGenLockId = (SPINLOCK) XIDGENLOCKID;
	ControlFileLockId = (SPINLOCK) CNTLFILELOCKID;

#ifdef STABLE_MEMORY_STORAGE
	MMCacheLock = (SPINLOCK) MMCACHELOCKID;
#endif

	return;
}

#ifdef LOCKDEBUG
#define PRINT_LOCK(LOCK) \
	TPRINTF(TRACE_SPINLOCKS, \
			"(locklock = %d, flag = %d, nshlocks = %d, shlock = %d, " \
			"exlock =%d)\n", LOCK->locklock, \
			LOCK->flag, LOCK->nshlocks, LOCK->shlock, \
			LOCK->exlock)
#endif

/* from ipc.c */
extern SLock *SLockArray;

void
SpinAcquire(SPINLOCK lockid)
{
	SLock	   *slckP;

	/* This used to be in ipc.c, but move here to reduce function calls */
	slckP = &(SLockArray[lockid]);
#ifdef LOCKDEBUG
	TPRINTF(TRACE_SPINLOCKS, "SpinAcquire: %d", lockid);
	PRINT_LOCK(slckP);
#endif
ex_try_again:
	S_LOCK(&(slckP->locklock));
	switch (slckP->flag)
	{
		case NOLOCK:
			slckP->flag = EXCLUSIVELOCK;
			S_LOCK(&(slckP->exlock));
			S_LOCK(&(slckP->shlock));
			S_UNLOCK(&(slckP->locklock));
#ifdef LOCKDEBUG
			TPRINTF(TRACE_SPINLOCKS, "OUT: ");
			PRINT_LOCK(slckP);
#endif
			break;
		case SHAREDLOCK:
		case EXCLUSIVELOCK:
			S_UNLOCK(&(slckP->locklock));
			S_LOCK(&(slckP->exlock));
			S_UNLOCK(&(slckP->exlock));
			goto ex_try_again;
	}
	PROC_INCR_SLOCK(lockid);
#ifdef LOCKDEBUG
	TPRINTF(TRACE_SPINLOCKS, "SpinAcquire: got %d", lockid);
#endif
}

void
SpinRelease(SPINLOCK lockid)
{
	SLock	   *slckP;

	/* This used to be in ipc.c, but move here to reduce function calls */
	slckP = &(SLockArray[lockid]);

#ifdef USE_ASSERT_CHECKING

	/*
	 * Check that we are actually holding the lock we are releasing. This
	 * can be done only after MyProc has been initialized.
	 */
	if (MyProc)
		Assert(MyProc->sLocks[lockid] > 0);
	Assert(slckP->flag != NOLOCK);
#endif

	PROC_DECR_SLOCK(lockid);

#ifdef LOCKDEBUG
	TPRINTF("SpinRelease: %d\n", lockid);
	PRINT_LOCK(slckP);
#endif
	S_LOCK(&(slckP->locklock));
	/* -------------
	 *	give favor to read processes
	 * -------------
	 */
	slckP->flag = NOLOCK;
	if (slckP->nshlocks > 0)
	{
		while (slckP->nshlocks > 0)
		{
			S_UNLOCK(&(slckP->shlock));
			S_LOCK(&(slckP->comlock));
		}
		S_UNLOCK(&(slckP->shlock));
	}
	else
		S_UNLOCK(&(slckP->shlock));
	S_UNLOCK(&(slckP->exlock));
	S_UNLOCK(&(slckP->locklock));
#ifdef LOCKDEBUG
	TPRINTF(TRACE_SPINLOCKS, "SpinRelease: released %d", lockid);
	PRINT_LOCK(slckP);
#endif
}

#else							/* HAS_TEST_AND_SET */
/* Spinlocks are implemented using SysV semaphores */

static bool AttachSpinLocks(IPCKey key);
static bool SpinIsLocked(SPINLOCK lock);

/*
 * SpinAcquire -- try to grab a spinlock
 *
 * FAILS if the semaphore is corrupted.
 */
void
SpinAcquire(SPINLOCK lock)
{
	IpcSemaphoreLock(SpinLockId, lock, IpcExclusiveLock);
	PROC_INCR_SLOCK(lock);
}

/*
 * SpinRelease -- release a spin lock
 *
 * FAILS if the semaphore is corrupted
 */
void
SpinRelease(SPINLOCK lock)
{
	Assert(SpinIsLocked(lock));
	PROC_DECR_SLOCK(lock);
	IpcSemaphoreUnlock(SpinLockId, lock, IpcExclusiveLock);
}

static bool
SpinIsLocked(SPINLOCK lock)
{
	int			semval;

	semval = IpcSemaphoreGetValue(SpinLockId, lock);
	return semval < IpcSemaphoreDefaultStartValue;
}

/*
 * CreateSpinlocks -- Create a sysV semaphore array for
 *		the spinlocks
 *
 */
void
CreateSpinlocks(IPCKey key)
{

	SpinLockId = IpcSemaphoreCreate(key, MAX_SPINS, IPCProtection,
							   IpcSemaphoreDefaultStartValue, 1);

	if (SpinLockId <= 0)
		elog(STOP, "CreateSpinlocks: cannot create spin locks");

	return;
}

/*
 * InitSpinLocks -- Spinlock bootstrapping
 *
 * We need several spinlocks for bootstrapping:
 * ShmemIndexLock (for the shmem index table) and
 * ShmemLock (for the shmem allocator), BufMgrLock (for buffer
 * pool exclusive access), LockMgrLock (for the lock table), and
 * ProcStructLock (a spin lock for the shared process structure).
 * If there's a Sony WORM drive attached, we also have a spinlock
 * (SJCacheLock) for it.  Same story for the main memory storage mgr.
 *
 */
void
InitSpinLocks(void)
{
	extern SPINLOCK ShmemLock;
	extern SPINLOCK ShmemIndexLock;
	extern SPINLOCK BufMgrLock;
	extern SPINLOCK LockMgrLock;
	extern SPINLOCK ProcStructLock;
	extern SPINLOCK SInvalLock;
	extern SPINLOCK OidGenLockId;
	extern SPINLOCK XidGenLockId;
	extern SPINLOCK	ControlFileLockId;

#ifdef STABLE_MEMORY_STORAGE
	extern SPINLOCK MMCacheLock;

#endif

	/* These five (or six) spinlocks have fixed location is shmem */
	ShmemLock = (SPINLOCK) SHMEMLOCKID;
	ShmemIndexLock = (SPINLOCK) SHMEMINDEXLOCKID;
	BufMgrLock = (SPINLOCK) BUFMGRLOCKID;
	LockMgrLock = (SPINLOCK) LOCKMGRLOCKID;
	ProcStructLock = (SPINLOCK) PROCSTRUCTLOCKID;
	SInvalLock = (SPINLOCK) SINVALLOCKID;
	OidGenLockId = (SPINLOCK) OIDGENLOCKID;
	XidGenLockId = (SPINLOCK) XIDGENLOCKID;
	ControlFileLockId = (SPINLOCK) CNTLFILELOCKID;

#ifdef STABLE_MEMORY_STORAGE
	MMCacheLock = (SPINLOCK) MMCACHELOCKID;
#endif

	return;
}

#endif	 /* HAS_TEST_AND_SET */
