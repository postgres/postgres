/*-------------------------------------------------------------------------
 *
 * lwlock.c
 *	  Lightweight lock manager
 *
 * Lightweight locks are intended primarily to provide mutual exclusion of
 * access to shared-memory data structures.  Therefore, they offer both
 * exclusive and shared lock modes (to support read/write and read-only
 * access to a shared object).	There are few other frammishes.  User-level
 * locking should be done with the full lock manager --- which depends on
 * an LWLock to protect its shared state.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/lmgr/lwlock.c,v 1.17 2003/08/04 02:40:03 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/clog.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/spin.h"


typedef struct LWLock
{
	slock_t		mutex;			/* Protects LWLock and queue of PGPROCs */
	bool		releaseOK;		/* T if ok to release waiters */
	char		exclusive;		/* # of exclusive holders (0 or 1) */
	int			shared;			/* # of shared holders (0..MaxBackends) */
	PGPROC	   *head;			/* head of list of waiting PGPROCs */
	PGPROC	   *tail;			/* tail of list of waiting PGPROCs */
	/* tail is undefined when head is NULL */
} LWLock;

/*
 * This points to the array of LWLocks in shared memory.  Backends inherit
 * the pointer by fork from the postmaster.  LWLockIds are indexes into
 * the array.
 */
static LWLock *LWLockArray = NULL;

/* shared counter for dynamic allocation of LWLockIds */
static int *LWLockCounter;


/*
 * We use this structure to keep track of locked LWLocks for release
 * during error recovery.  The maximum size could be determined at runtime
 * if necessary, but it seems unlikely that more than a few locks could
 * ever be held simultaneously.
 */
#define MAX_SIMUL_LWLOCKS	100

static int	num_held_lwlocks = 0;
static LWLockId held_lwlocks[MAX_SIMUL_LWLOCKS];


#ifdef LOCK_DEBUG
bool		Trace_lwlocks = false;

inline static void
PRINT_LWDEBUG(const char *where, LWLockId lockid, const volatile LWLock *lock)
{
	if (Trace_lwlocks)
		elog(LOG, "%s(%d): excl %d shared %d head %p rOK %d",
			 where, (int) lockid,
			 (int) lock->exclusive, lock->shared, lock->head,
			 (int) lock->releaseOK);
}

inline static void
LOG_LWDEBUG(const char *where, LWLockId lockid, const char *msg)
{
	if (Trace_lwlocks)
		elog(LOG, "%s(%d): %s", where, (int) lockid, msg);
}

#else							/* not LOCK_DEBUG */
#define PRINT_LWDEBUG(a,b,c)
#define LOG_LWDEBUG(a,b,c)
#endif   /* LOCK_DEBUG */


/*
 * Compute number of LWLocks to allocate.
 */
int
NumLWLocks(void)
{
	int			numLocks;

	/*
	 * Possibly this logic should be spread out among the affected
	 * modules, the same way that shmem space estimation is done.  But for
	 * now, there are few enough users of LWLocks that we can get away
	 * with just keeping the knowledge here.
	 */

	/* Predefined LWLocks */
	numLocks = (int) NumFixedLWLocks;

	/* bufmgr.c needs two for each shared buffer */
	numLocks += 2 * NBuffers;

	/* clog.c needs one per CLOG buffer + one control lock */
	numLocks += NUM_CLOG_BUFFERS + 1;

	/* Perhaps create a few more for use by user-defined modules? */

	return numLocks;
}


/*
 * Compute shmem space needed for LWLocks.
 */
int
LWLockShmemSize(void)
{
	int			numLocks = NumLWLocks();
	uint32		spaceLocks;

	/* Allocate the LWLocks plus space for shared allocation counter. */
	spaceLocks = numLocks * sizeof(LWLock) + 2 * sizeof(int);
	spaceLocks = MAXALIGN(spaceLocks);

	return (int) spaceLocks;
}


/*
 * Allocate shmem space for LWLocks and initialize the locks.
 */
void
CreateLWLocks(void)
{
	int			numLocks = NumLWLocks();
	uint32		spaceLocks = LWLockShmemSize();
	LWLock	   *lock;
	int			id;

	/* Allocate space */
	LWLockArray = (LWLock *) ShmemAlloc(spaceLocks);

	/*
	 * Initialize all LWLocks to "unlocked" state
	 */
	for (id = 0, lock = LWLockArray; id < numLocks; id++, lock++)
	{
		SpinLockInit(&lock->mutex);
		lock->releaseOK = true;
		lock->exclusive = 0;
		lock->shared = 0;
		lock->head = NULL;
		lock->tail = NULL;
	}

	/*
	 * Initialize the dynamic-allocation counter at the end of the array
	 */
	LWLockCounter = (int *) lock;
	LWLockCounter[0] = (int) NumFixedLWLocks;
	LWLockCounter[1] = numLocks;
}


/*
 * LWLockAssign - assign a dynamically-allocated LWLock number
 *
 * NB: we do not currently try to interlock this.  Could perhaps use
 * ShmemLock spinlock if there were any need to assign LWLockIds after
 * shmem setup.
 */
LWLockId
LWLockAssign(void)
{
	if (LWLockCounter[0] >= LWLockCounter[1])
		elog(FATAL, "no more LWLockIds available");
	return (LWLockId) (LWLockCounter[0]++);
}


/*
 * LWLockAcquire - acquire a lightweight lock in the specified mode
 *
 * If the lock is not available, sleep until it is.
 *
 * Side effect: cancel/die interrupts are held off until lock release.
 */
void
LWLockAcquire(LWLockId lockid, LWLockMode mode)
{
	volatile LWLock *lock = LWLockArray + lockid;
	PGPROC	   *proc = MyProc;
	bool		retry = false;
	int			extraWaits = 0;

	PRINT_LWDEBUG("LWLockAcquire", lockid, lock);

	/*
	 * We can't wait if we haven't got a PGPROC.  This should only occur
	 * during bootstrap or shared memory initialization.  Put an Assert
	 * here to catch unsafe coding practices.
	 */
	Assert(!(proc == NULL && IsUnderPostmaster));

	/*
	 * Lock out cancel/die interrupts until we exit the code section
	 * protected by the LWLock.  This ensures that interrupts will not
	 * interfere with manipulations of data structures in shared memory.
	 */
	HOLD_INTERRUPTS();

	/*
	 * Loop here to try to acquire lock after each time we are signaled by
	 * LWLockRelease.
	 *
	 * NOTE: it might seem better to have LWLockRelease actually grant us the
	 * lock, rather than retrying and possibly having to go back to sleep.
	 * But in practice that is no good because it means a process swap for
	 * every lock acquisition when two or more processes are contending
	 * for the same lock.  Since LWLocks are normally used to protect
	 * not-very-long sections of computation, a process needs to be able
	 * to acquire and release the same lock many times during a single CPU
	 * time slice, even in the presence of contention.	The efficiency of
	 * being able to do that outweighs the inefficiency of sometimes
	 * wasting a process dispatch cycle because the lock is not free when
	 * a released waiter finally gets to run.  See pgsql-hackers archives
	 * for 29-Dec-01.
	 */
	for (;;)
	{
		bool		mustwait;

		/* Acquire mutex.  Time spent holding mutex should be short! */
		SpinLockAcquire_NoHoldoff(&lock->mutex);

		/* If retrying, allow LWLockRelease to release waiters again */
		if (retry)
			lock->releaseOK = true;

		/* If I can get the lock, do so quickly. */
		if (mode == LW_EXCLUSIVE)
		{
			if (lock->exclusive == 0 && lock->shared == 0)
			{
				lock->exclusive++;
				mustwait = false;
			}
			else
				mustwait = true;
		}
		else
		{
			if (lock->exclusive == 0)
			{
				lock->shared++;
				mustwait = false;
			}
			else
				mustwait = true;
		}

		if (!mustwait)
			break;				/* got the lock */

		/*
		 * Add myself to wait queue.
		 *
		 * If we don't have a PGPROC structure, there's no way to wait. This
		 * should never occur, since MyProc should only be null during
		 * shared memory initialization.
		 */
		if (proc == NULL)
			elog(FATAL, "cannot wait without a PGPROC structure");

		proc->lwWaiting = true;
		proc->lwExclusive = (mode == LW_EXCLUSIVE);
		proc->lwWaitLink = NULL;
		if (lock->head == NULL)
			lock->head = proc;
		else
			lock->tail->lwWaitLink = proc;
		lock->tail = proc;

		/* Can release the mutex now */
		SpinLockRelease_NoHoldoff(&lock->mutex);

		/*
		 * Wait until awakened.
		 *
		 * Since we share the process wait semaphore with the regular lock
		 * manager and ProcWaitForSignal, and we may need to acquire an
		 * LWLock while one of those is pending, it is possible that we
		 * get awakened for a reason other than being signaled by
		 * LWLockRelease. If so, loop back and wait again.	Once we've
		 * gotten the LWLock, re-increment the sema by the number of
		 * additional signals received, so that the lock manager or signal
		 * manager will see the received signal when it next waits.
		 */
		LOG_LWDEBUG("LWLockAcquire", lockid, "waiting");

		for (;;)
		{
			/* "false" means cannot accept cancel/die interrupt here. */
			PGSemaphoreLock(&proc->sem, false);
			if (!proc->lwWaiting)
				break;
			extraWaits++;
		}

		LOG_LWDEBUG("LWLockAcquire", lockid, "awakened");

		/* Now loop back and try to acquire lock again. */
		retry = true;
	}

	/* We are done updating shared state of the lock itself. */
	SpinLockRelease_NoHoldoff(&lock->mutex);

	/* Add lock to list of locks held by this backend */
	Assert(num_held_lwlocks < MAX_SIMUL_LWLOCKS);
	held_lwlocks[num_held_lwlocks++] = lockid;

	/*
	 * Fix the process wait semaphore's count for any absorbed wakeups.
	 */
	while (extraWaits-- > 0)
		PGSemaphoreUnlock(&proc->sem);
}

/*
 * LWLockConditionalAcquire - acquire a lightweight lock in the specified mode
 *
 * If the lock is not available, return FALSE with no side-effects.
 *
 * If successful, cancel/die interrupts are held off until lock release.
 */
bool
LWLockConditionalAcquire(LWLockId lockid, LWLockMode mode)
{
	volatile LWLock *lock = LWLockArray + lockid;
	bool		mustwait;

	PRINT_LWDEBUG("LWLockConditionalAcquire", lockid, lock);

	/*
	 * Lock out cancel/die interrupts until we exit the code section
	 * protected by the LWLock.  This ensures that interrupts will not
	 * interfere with manipulations of data structures in shared memory.
	 */
	HOLD_INTERRUPTS();

	/* Acquire mutex.  Time spent holding mutex should be short! */
	SpinLockAcquire_NoHoldoff(&lock->mutex);

	/* If I can get the lock, do so quickly. */
	if (mode == LW_EXCLUSIVE)
	{
		if (lock->exclusive == 0 && lock->shared == 0)
		{
			lock->exclusive++;
			mustwait = false;
		}
		else
			mustwait = true;
	}
	else
	{
		if (lock->exclusive == 0)
		{
			lock->shared++;
			mustwait = false;
		}
		else
			mustwait = true;
	}

	/* We are done updating shared state of the lock itself. */
	SpinLockRelease_NoHoldoff(&lock->mutex);

	if (mustwait)
	{
		/* Failed to get lock, so release interrupt holdoff */
		RESUME_INTERRUPTS();
		LOG_LWDEBUG("LWLockConditionalAcquire", lockid, "failed");
	}
	else
	{
		/* Add lock to list of locks held by this backend */
		Assert(num_held_lwlocks < MAX_SIMUL_LWLOCKS);
		held_lwlocks[num_held_lwlocks++] = lockid;
	}

	return !mustwait;
}

/*
 * LWLockRelease - release a previously acquired lock
 */
void
LWLockRelease(LWLockId lockid)
{
	volatile LWLock *lock = LWLockArray + lockid;
	PGPROC	   *head;
	PGPROC	   *proc;
	int			i;

	PRINT_LWDEBUG("LWLockRelease", lockid, lock);

	/*
	 * Remove lock from list of locks held.  Usually, but not always, it
	 * will be the latest-acquired lock; so search array backwards.
	 */
	for (i = num_held_lwlocks; --i >= 0;)
	{
		if (lockid == held_lwlocks[i])
			break;
	}
	if (i < 0)
		elog(ERROR, "lock %d is not held", (int) lockid);
	num_held_lwlocks--;
	for (; i < num_held_lwlocks; i++)
		held_lwlocks[i] = held_lwlocks[i + 1];

	/* Acquire mutex.  Time spent holding mutex should be short! */
	SpinLockAcquire_NoHoldoff(&lock->mutex);

	/* Release my hold on lock */
	if (lock->exclusive > 0)
		lock->exclusive--;
	else
	{
		Assert(lock->shared > 0);
		lock->shared--;
	}

	/*
	 * See if I need to awaken any waiters.  If I released a non-last
	 * shared hold, there cannot be anything to do.  Also, do not awaken
	 * any waiters if someone has already awakened waiters that haven't
	 * yet acquired the lock.
	 */
	head = lock->head;
	if (head != NULL)
	{
		if (lock->exclusive == 0 && lock->shared == 0 && lock->releaseOK)
		{
			/*
			 * Remove the to-be-awakened PGPROCs from the queue.  If the
			 * front waiter wants exclusive lock, awaken him only.
			 * Otherwise awaken as many waiters as want shared access.
			 */
			proc = head;
			if (!proc->lwExclusive)
			{
				while (proc->lwWaitLink != NULL &&
					   !proc->lwWaitLink->lwExclusive)
					proc = proc->lwWaitLink;
			}
			/* proc is now the last PGPROC to be released */
			lock->head = proc->lwWaitLink;
			proc->lwWaitLink = NULL;
			/* prevent additional wakeups until retryer gets to run */
			lock->releaseOK = false;
		}
		else
		{
			/* lock is still held, can't awaken anything */
			head = NULL;
		}
	}

	/* We are done updating shared state of the lock itself. */
	SpinLockRelease_NoHoldoff(&lock->mutex);

	/*
	 * Awaken any waiters I removed from the queue.
	 */
	while (head != NULL)
	{
		LOG_LWDEBUG("LWLockRelease", lockid, "release waiter");
		proc = head;
		head = proc->lwWaitLink;
		proc->lwWaitLink = NULL;
		proc->lwWaiting = false;
		PGSemaphoreUnlock(&proc->sem);
	}

	/*
	 * Now okay to allow cancel/die interrupts.
	 */
	RESUME_INTERRUPTS();
}


/*
 * LWLockReleaseAll - release all currently-held locks
 *
 * Used to clean up after ereport(ERROR). An important difference between this
 * function and retail LWLockRelease calls is that InterruptHoldoffCount is
 * unchanged by this operation.  This is necessary since InterruptHoldoffCount
 * has been set to an appropriate level earlier in error recovery. We could
 * decrement it below zero if we allow it to drop for each released lock!
 */
void
LWLockReleaseAll(void)
{
	while (num_held_lwlocks > 0)
	{
		HOLD_INTERRUPTS();		/* match the upcoming RESUME_INTERRUPTS */

		LWLockRelease(held_lwlocks[num_held_lwlocks - 1]);
	}
}
