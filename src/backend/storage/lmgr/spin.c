/*-------------------------------------------------------------------------
 *
 * spin.c
 *	   Hardware-independent implementation of spinlocks.
 *
 *
 * For machines that have test-and-set (TAS) instructions, s_lock.h/.c
 * define the spinlock implementation.  This file contains only a stub
 * implementation for spinlocks using PGSemaphores.  Unless semaphores
 * are implemented in a way that doesn't involve a kernel call, this
 * is too slow to be very useful :-(
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/lmgr/spin.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/pg_sema.h"
#include "storage/shmem.h"
#include "storage/spin.h"


#ifndef HAVE_SPINLOCKS

/*
 * No TAS, so spinlocks are implemented as PGSemaphores.
 */

#ifndef HAVE_ATOMICS
#define NUM_EMULATION_SEMAPHORES (NUM_SPINLOCK_SEMAPHORES + NUM_ATOMICS_SEMAPHORES)
#else
#define NUM_EMULATION_SEMAPHORES (NUM_SPINLOCK_SEMAPHORES)
#endif /* DISABLE_ATOMICS */

PGSemaphore *SpinlockSemaArray;

#else							/* !HAVE_SPINLOCKS */

#define NUM_EMULATION_SEMAPHORES 0

#endif							/* HAVE_SPINLOCKS */

/*
 * Report the amount of shared memory needed to store semaphores for spinlock
 * support.
 */
Size
SpinlockSemaSize(void)
{
	return NUM_EMULATION_SEMAPHORES * sizeof(PGSemaphore);
}

/*
 * Report number of semaphores needed to support spinlocks.
 */
int
SpinlockSemas(void)
{
	return NUM_EMULATION_SEMAPHORES;
}

#ifndef HAVE_SPINLOCKS

/*
 * Initialize spinlock emulation.
 *
 * This must be called after PGReserveSemaphores().
 */
void
SpinlockSemaInit(void)
{
	PGSemaphore *spinsemas;
	int			nsemas = SpinlockSemas();
	int			i;

	/*
	 * We must use ShmemAllocUnlocked(), since the spinlock protecting
	 * ShmemAlloc() obviously can't be ready yet.
	 */
	spinsemas = (PGSemaphore *) ShmemAllocUnlocked(SpinlockSemaSize());
	for (i = 0; i < nsemas; ++i)
		spinsemas[i] = PGSemaphoreCreate();
	SpinlockSemaArray = spinsemas;
}

/*
 * s_lock.h hardware-spinlock emulation using semaphores
 *
 * We map all spinlocks onto NUM_EMULATION_SEMAPHORES semaphores.  It's okay to
 * map multiple spinlocks onto one semaphore because no process should ever
 * hold more than one at a time.  We just need enough semaphores so that we
 * aren't adding too much extra contention from that.
 *
 * There is one exception to the restriction of only holding one spinlock at a
 * time, which is that it's ok if emulated atomic operations are nested inside
 * spinlocks. To avoid the danger of spinlocks and atomic using the same sema,
 * we make sure "normal" spinlocks and atomics backed by spinlocks use
 * distinct semaphores (see the nested argument to s_init_lock_sema).
 *
 * slock_t is just an int for this implementation; it holds the spinlock
 * number from 1..NUM_EMULATION_SEMAPHORES.  We intentionally ensure that 0
 * is not a valid value, so that testing with this code can help find
 * failures to initialize spinlocks.
 */

static inline void
s_check_valid(int lockndx)
{
	if (unlikely(lockndx <= 0 || lockndx > NUM_EMULATION_SEMAPHORES))
		elog(ERROR, "invalid spinlock number: %d", lockndx);
}

void
s_init_lock_sema(volatile slock_t *lock, bool nested)
{
	static uint32 counter = 0;
	uint32		offset;
	uint32		sema_total;
	uint32		idx;

	if (nested)
	{
		/*
		 * To allow nesting atomics inside spinlocked sections, use a
		 * different spinlock. See comment above.
		 */
		offset = 1 + NUM_SPINLOCK_SEMAPHORES;
		sema_total = NUM_ATOMICS_SEMAPHORES;
	}
	else
	{
		offset = 1;
		sema_total = NUM_SPINLOCK_SEMAPHORES;
	}

	idx = (counter++ % sema_total) + offset;

	/* double check we did things correctly */
	s_check_valid(idx);

	*lock = idx;
}

void
s_unlock_sema(volatile slock_t *lock)
{
	int			lockndx = *lock;

	s_check_valid(lockndx);

	PGSemaphoreUnlock(SpinlockSemaArray[lockndx - 1]);
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
	int			lockndx = *lock;

	s_check_valid(lockndx);

	/* Note that TAS macros return 0 if *success* */
	return !PGSemaphoreTryLock(SpinlockSemaArray[lockndx - 1]);
}

#endif							/* !HAVE_SPINLOCKS */
