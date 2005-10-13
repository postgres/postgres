/*-------------------------------------------------------------------------
 *
 * spin.h
 *	   Hardware-independent implementation of spinlocks.
 *
 *
 *	The hardware-independent interface to spinlocks is defined by the
 *	typedef "slock_t" and these macros:
 *
 *	void SpinLockInit(volatile slock_t *lock)
 *		Initialize a spinlock (to the unlocked state).
 *
 *	void SpinLockAcquire(volatile slock_t *lock)
 *		Acquire a spinlock, waiting if necessary.
 *		Time out and abort() if unable to acquire the lock in a
 *		"reasonable" amount of time --- typically ~ 1 minute.
 *		Cancel/die interrupts are held off until the lock is released.
 *
 *	void SpinLockRelease(volatile slock_t *lock)
 *		Unlock a previously acquired lock.
 *		Release the cancel/die interrupt holdoff.
 *
 *	void SpinLockAcquire_NoHoldoff(volatile slock_t *lock)
 *	void SpinLockRelease_NoHoldoff(volatile slock_t *lock)
 *		Same as above, except no interrupt holdoff processing is done.
 *		This pair of macros may be used when there is a surrounding
 *		interrupt holdoff.
 *
 *	bool SpinLockFree(slock_t *lock)
 *		Tests if the lock is free. Returns TRUE if free, FALSE if locked.
 *		This does *not* change the state of the lock.
 *
 *	Callers must beware that the macro argument may be evaluated multiple
 *	times!
 *
 *	CAUTION: Care must be taken to ensure that loads and stores of
 *	shared memory values are not rearranged around spinlock acquire
 *	and release. This is done using the "volatile" qualifier: the C
 *	standard states that loads and stores of volatile objects cannot
 *	be rearranged *with respect to other volatile objects*. The
 *	spinlock is always written through a volatile pointer by the
 *	spinlock macros, but this is not sufficient by itself: code that
 *	protects shared data with a spinlock MUST reference that shared
 *	data through a volatile pointer.
 *
 *	These macros are implemented in terms of hardware-dependent macros
 *	supplied by s_lock.h.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/spin.h,v 1.26 2005/10/13 06:17:34 neilc Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPIN_H
#define SPIN_H

#include "storage/s_lock.h"
#include "miscadmin.h"


#define SpinLockInit(lock)	S_INIT_LOCK(lock)

#define SpinLockAcquire(lock) \
	do { \
		HOLD_INTERRUPTS(); \
		S_LOCK(lock); \
	} while (0)

#define SpinLockAcquire_NoHoldoff(lock) S_LOCK(lock)

#define SpinLockRelease(lock) \
	do { \
		S_UNLOCK(lock); \
		RESUME_INTERRUPTS(); \
	} while (0)

#define SpinLockRelease_NoHoldoff(lock) S_UNLOCK(lock)

#define SpinLockFree(lock)	S_LOCK_FREE(lock)


extern int	SpinlockSemas(void);

#endif   /* SPIN_H */
