/*-------------------------------------------------------------------------
 *
 * spin.h
 *	   Hardware-independent implementation of spinlocks.
 *
 *
 *	The hardware-independent interface to spinlocks is defined by the
 *	typedef "slock_t" and these macros:
 *
 *	void SpinLockInit(slock_t *lock)
 *		Initialize a spinlock (to the unlocked state).
 *
 *	void SpinLockAcquire(slock_t *lock)
 *		Acquire a spinlock, waiting if necessary.
 *		Time out and abort() if unable to acquire the lock in a
 *		"reasonable" amount of time --- typically ~ 1 minute.
 *		Cancel/die interrupts are held off until the lock is released.
 *
 *	void SpinLockRelease(slock_t *lock)
 *		Unlock a previously acquired lock.
 *		Release the cancel/die interrupt holdoff.
 *
 *	void SpinLockAcquire_NoHoldoff(slock_t *lock)
 *	void SpinLockRelease_NoHoldoff(slock_t *lock)
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
 *	The macros are implemented in terms of hardware-dependent macros
 *	supplied by s_lock.h.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: spin.h,v 1.22 2003/08/04 02:40:15 momjian Exp $
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
