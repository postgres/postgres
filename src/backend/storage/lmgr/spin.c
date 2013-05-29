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
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/lmgr/spin.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "replication/walsender.h"
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
	int			nsemas;

	/*
	 * It would be cleaner to distribute this logic into the affected modules,
	 * similar to the way shmem space estimation is handled.
	 *
	 * For now, though, there are few enough users of spinlocks that we just
	 * keep the knowledge here.
	 */
	nsemas = NumLWLocks();		/* one for each lwlock */
	nsemas += NBuffers;			/* one for each buffer header */
	nsemas += max_wal_senders;	/* one for each wal sender process */
	nsemas += 30;				/* plus a bunch for other small-scale use */

	return nsemas;
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
