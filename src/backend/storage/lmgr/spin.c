/*-------------------------------------------------------------------------
 *
 * spin.c
 *	   Hardware-independent implementation of spinlocks.
 *
 *
 * For machines that have test-and-set (TAS) instructions, s_lock.h/.c
 * define the spinlock implementation.  This file contains only a stub
 * implementation for spinlocks using SysV semaphores.  The semaphore method
 * is too slow to be very useful :-(
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/lmgr/spin.c,v 1.4 2001/10/01 18:16:35 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>

#include "storage/ipc.h"
/* In Ultrix, sem.h and shm.h must be included AFTER ipc.h */
#ifdef HAVE_SYS_SEM_H
#include <sys/sem.h>
#endif

#if defined(__darwin__)
#include "port/darwin/sem.h"
#endif

#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/spin.h"


#ifdef HAS_TEST_AND_SET

/*
 * CreateSpinlocks --- create and initialize spinlocks during startup
 */
void
CreateSpinlocks(void)
{
	/* no-op when we have TAS spinlocks */
}

#else							/* !HAS_TEST_AND_SET */

/*
 * No TAS, so spinlocks are implemented using SysV semaphores.
 *
 * Typedef slock_t stores the semId and sem number of the sema to use.
 * The semas needed are created by CreateSpinlocks and doled out by
 * s_init_lock_sema.
 *
 * Since many systems have a rather small SEMMSL limit on semas per set,
 * we allocate the semaphores required in sets of SPINLOCKS_PER_SET semas.
 * This value is deliberately made equal to PROC_NSEMS_PER_SET so that all
 * sema sets allocated by Postgres will be the same size; that eases the
 * semaphore-recycling logic in IpcSemaphoreCreate().
 *
 * Note that the SpinLockIds array is not in shared memory; it is filled
 * by the postmaster and then inherited through fork() by backends.  This
 * is OK because its contents do not change after shmem initialization.
 */

#define SPINLOCKS_PER_SET  PROC_NSEMS_PER_SET

static IpcSemaphoreId *SpinLockIds = NULL;

static int	numSpinSets = 0;	/* number of sema sets used */
static int	numSpinLocks = 0;	/* total number of semas allocated */
static int	nextSpinLock = 0;	/* next free spinlock index */

static void SpinFreeAllSemaphores(void);


/*
 * CreateSpinlocks --- create and initialize spinlocks during startup
 */
void
CreateSpinlocks(void)
{
	int			i;

	if (SpinLockIds == NULL)
	{
		/*
		 * Compute number of spinlocks needed.	It would be cleaner to
		 * distribute this logic into the affected modules,
		 * similar to the way shmem space estimation is handled.
		 *
		 * For now, though, we just need a few spinlocks (10 should be
		 * plenty) plus one for each LWLock.
		 */
		numSpinLocks = NumLWLocks() + 10;

		/* might as well round up to a multiple of SPINLOCKS_PER_SET */
		numSpinSets = (numSpinLocks - 1) / SPINLOCKS_PER_SET + 1;
		numSpinLocks = numSpinSets * SPINLOCKS_PER_SET;

		SpinLockIds = (IpcSemaphoreId *)
			malloc(numSpinSets * sizeof(IpcSemaphoreId));
		Assert(SpinLockIds != NULL);
	}

	for (i = 0; i < numSpinSets; i++)
		SpinLockIds[i] = -1;

	/*
	 * Arrange to delete semas on exit --- set this up now so that we will
	 * clean up if allocation fails.  We use our own freeproc, rather than
	 * IpcSemaphoreCreate's removeOnExit option, because we don't want to
	 * fill up the on_shmem_exit list with a separate entry for each
	 * semaphore set.
	 */
	on_shmem_exit(SpinFreeAllSemaphores, 0);

	/* Create sema sets and set all semas to count 1 */
	for (i = 0; i < numSpinSets; i++)
	{
		SpinLockIds[i] = IpcSemaphoreCreate(SPINLOCKS_PER_SET,
											IPCProtection,
											1,
											false);
	}

	/* Init counter for allocating dynamic spinlocks */
	nextSpinLock = 0;
}

/*
 * SpinFreeAllSemaphores -
 *	  called at shmem_exit time, ie when exiting the postmaster or
 *	  destroying shared state for a failed set of backends.
 *	  Free up all the semaphores allocated for spinlocks.
 */
static void
SpinFreeAllSemaphores(void)
{
	int			i;

	for (i = 0; i < numSpinSets; i++)
	{
		if (SpinLockIds[i] >= 0)
			IpcSemaphoreKill(SpinLockIds[i]);
	}
	free(SpinLockIds);
	SpinLockIds = NULL;
}

/*
 * s_lock.h hardware-spinlock emulation
 */

void
s_init_lock_sema(volatile slock_t *lock)
{
	if (nextSpinLock >= numSpinLocks)
		elog(FATAL, "s_init_lock_sema: not enough semaphores");
	lock->semId = SpinLockIds[nextSpinLock / SPINLOCKS_PER_SET];
	lock->sem = nextSpinLock % SPINLOCKS_PER_SET;
	nextSpinLock++;
}

void
s_unlock_sema(volatile slock_t *lock)
{
	IpcSemaphoreUnlock(lock->semId, lock->sem);
}

bool
s_lock_free_sema(volatile slock_t *lock)
{
	return IpcSemaphoreGetValue(lock->semId, lock->sem) > 0;
}

int
tas_sema(volatile slock_t *lock)
{
	/* Note that TAS macros return 0 if *success* */
	return !IpcSemaphoreTryLock(lock->semId, lock->sem);
}

#endif	 /* !HAS_TEST_AND_SET */
