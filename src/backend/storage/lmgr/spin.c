/*-------------------------------------------------------------------------
 *
 * spin.c
 *	   Hardware-independent implementation of spinlocks.
 *
 *
 * For machines that have test-and-set (TAS) instructions, s_lock.h/.c
 * define the spinlock implementation.	This file contains only a stub
 * implementation for spinlocks using PGSemaphores.  Unless semaphores
 * are implemented in a way that doesn't involve a kernel call, this
 * is too slow to be very useful :-(
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/lmgr/spin.c,v 1.21 2006/07/22 21:04:40 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "storage/lwlock.h"
#include "storage/spin.h"


#ifdef HAVE_SPINLOCKS

/*
 * Report number of semaphores needed to support spinlocks.
 */
int
SpinlockSemas(void)
{
	return 0;
}
#else							/* !HAVE_SPINLOCKS */

/*
 * No TAS, so spinlocks are implemented as PGSemaphores.
 */


/*
 * Report number of semaphores needed to support spinlocks.
 */
int
SpinlockSemas(void)
{
	/*
	 * It would be cleaner to distribute this logic into the affected modules,
	 * similar to the way shmem space estimation is handled.
	 *
	 * For now, though, we just need a few spinlocks (10 should be plenty)
	 * plus one for each LWLock and one for each buffer header.
	 */
	return NumLWLocks() + NBuffers + 10;
}

/*
 * s_lock.h hardware-spinlock emulation
 */

void
s_init_lock_sema(volatile slock_t *lock)
{
	PGSemaphoreCreate((PGSemaphore) lock);
}

void
s_unlock_sema(volatile slock_t *lock)
{
	PGSemaphoreUnlock((PGSemaphore) lock);
}

bool
s_lock_free_sema(volatile slock_t *lock)
{
	/* We don't currently use S_LOCK_FREE anyway */
	elog(ERROR, "spin.c does not support S_LOCK_FREE()");
	return false;
}

int
tas_sema(volatile slock_t *lock)
{
	/* Note that TAS macros return 0 if *success* */
	return !PGSemaphoreTryLock((PGSemaphore) lock);
}

#endif   /* !HAVE_SPINLOCKS */
