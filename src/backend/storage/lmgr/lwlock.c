/*-------------------------------------------------------------------------
 *
 * lwlock.c
 *	  Lightweight lock manager
 *
 * Lightweight locks are intended primarily to provide mutual exclusion of
 * access to shared-memory data structures.  Therefore, they offer both
 * exclusive and shared lock modes (to support read/write and read-only
 * access to a shared object).  There are few other frammishes.  User-level
 * locking should be done with the full lock manager --- which depends on
 * an LWLock to protect its shared state.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/lmgr/lwlock.c,v 1.1 2001/09/29 04:02:24 tgl Exp $
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
	slock_t		mutex;			/* Protects LWLock and queue of PROCs */
	char		exclusive;		/* # of exclusive holders (0 or 1) */
	int			shared;			/* # of shared holders (0..MaxBackends) */
	PROC	   *head;			/* head of list of waiting PROCs */
	PROC	   *tail;			/* tail of list of waiting PROCs */
	/* tail is undefined when head is NULL */
} LWLock;

/*
 * This points to the array of LWLocks in shared memory.  Backends inherit
 * the pointer by fork from the postmaster.  LWLockIds are indexes into
 * the array.
 */
static LWLock *LWLockArray = NULL;
/* shared counter for dynamic allocation of LWLockIds */
static int	  *LWLockCounter;


/*
 * We use this structure to keep track of locked LWLocks for release
 * during error recovery.  The maximum size could be determined at runtime
 * if necessary, but it seems unlikely that more than a few locks could
 * ever be held simultaneously.
 */
#define MAX_SIMUL_LWLOCKS	100

static int		num_held_lwlocks = 0;
static LWLockId	held_lwlocks[MAX_SIMUL_LWLOCKS];


#ifdef LOCK_DEBUG
bool		Trace_lwlocks = false;

inline static void
PRINT_LWDEBUG(const char *where, LWLockId lockid, const LWLock *lock)
{
	if (Trace_lwlocks)
		elog(DEBUG, "%s(%d): excl %d shared %d head %p",
			 where, (int) lockid,
			 (int) lock->exclusive, lock->shared, lock->head);
}

#else	/* not LOCK_DEBUG */
#define PRINT_LWDEBUG(a,b,c)
#endif	/* LOCK_DEBUG */


/*
 * Compute number of LWLocks to allocate.
 */
int
NumLWLocks(void)
{
	int		numLocks;

	/*
	 * Possibly this logic should be spread out among the affected modules,
	 * the same way that shmem space estimation is done.  But for now,
	 * there are few enough users of LWLocks that we can get away with
	 * just keeping the knowledge here.
	 */

	/* Predefined LWLocks */
	numLocks = (int) NumFixedLWLocks;

	/* bufmgr.c needs two for each shared buffer */
	numLocks += 2 * NBuffers;

	/* clog.c needs one per CLOG buffer */
	numLocks += NUM_CLOG_BUFFERS;

	/* Perhaps create a few more for use by user-defined modules? */

	return numLocks;
}


/*
 * Compute shmem space needed for LWLocks.
 */
int
LWLockShmemSize(void)
{
	int		numLocks = NumLWLocks();
	uint32	spaceLocks;

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
	int		numLocks = NumLWLocks();
	uint32	spaceLocks = LWLockShmemSize();
	LWLock *lock;
	int		id;

	/* Allocate space */
	LWLockArray = (LWLock *) ShmemAlloc(spaceLocks);

	/*
	 * Initialize all LWLocks to "unlocked" state
	 */
	for (id = 0, lock = LWLockArray; id < numLocks; id++, lock++)
	{
		SpinLockInit(&lock->mutex);
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
		elog(FATAL, "No more LWLockIds available");
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
	LWLock *lock = LWLockArray + lockid;
	bool	mustwait;

	PRINT_LWDEBUG("LWLockAcquire", lockid, lock);

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
		/*
		 * If there is someone waiting (presumably for exclusive access),
		 * queue up behind him even though I could get the lock.  This
		 * prevents a stream of read locks from starving a writer.
		 */
		if (lock->exclusive == 0 && lock->head == NULL)
		{
			lock->shared++;
			mustwait = false;
		}
		else
			mustwait = true;
	}

	if (mustwait)
	{
		/* Add myself to wait queue */
		PROC	*proc = MyProc;
		int		extraWaits = 0;

		/*
		 * If we don't have a PROC structure, there's no way to wait.
		 * This should never occur, since MyProc should only be null
		 * during shared memory initialization.
		 */
		if (proc == NULL)
			elog(FATAL, "LWLockAcquire: can't wait without a PROC structure");

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
		 * manager and ProcWaitForSignal, and we may need to acquire an LWLock
		 * while one of those is pending, it is possible that we get awakened
		 * for a reason other than being granted the LWLock.  If so, loop back
		 * and wait again.  Once we've gotten the lock, re-increment the sema
		 * by the number of additional signals received, so that the lock
		 * manager or signal manager will see the received signal when it
		 * next waits.
		 */
		for (;;)
		{
			/* "false" means cannot accept cancel/die interrupt here. */
			IpcSemaphoreLock(proc->sem.semId, proc->sem.semNum, false);
			if (!proc->lwWaiting)
				break;
			extraWaits++;
		}
		/*
		 * The awakener already updated the lock struct's state, so we
		 * don't need to do anything more to it.  Just need to fix the
		 * semaphore count.
		 */
		while (extraWaits-- > 0)
			IpcSemaphoreUnlock(proc->sem.semId, proc->sem.semNum);
	}
	else
	{
		/* Got the lock without waiting */
		SpinLockRelease_NoHoldoff(&lock->mutex);
	}

	/* Add lock to list of locks held by this backend */
	Assert(num_held_lwlocks < MAX_SIMUL_LWLOCKS);
	held_lwlocks[num_held_lwlocks++] = lockid;
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
	LWLock *lock = LWLockArray + lockid;
	bool	mustwait;

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
		/*
		 * If there is someone waiting (presumably for exclusive access),
		 * queue up behind him even though I could get the lock.  This
		 * prevents a stream of read locks from starving a writer.
		 */
		if (lock->exclusive == 0 && lock->head == NULL)
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
	LWLock *lock = LWLockArray + lockid;
	PROC	*head;
	PROC	*proc;
	int		i;

	PRINT_LWDEBUG("LWLockRelease", lockid, lock);

	/*
	 * Remove lock from list of locks held.  Usually, but not always,
	 * it will be the latest-acquired lock; so search array backwards.
	 */
	for (i = num_held_lwlocks; --i >= 0; )
	{
		if (lockid == held_lwlocks[i])
			break;
	}
	if (i < 0)
		elog(ERROR, "LWLockRelease: lock %d is not held", (int) lockid);
	num_held_lwlocks--;
	for (; i < num_held_lwlocks; i++)
		held_lwlocks[i] = held_lwlocks[i+1];

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
	 * See if I need to awaken any waiters.  If I released a non-last shared
	 * hold, there cannot be anything to do.
	 */
	head = lock->head;
	if (head != NULL)
	{
		if (lock->exclusive == 0 && lock->shared == 0)
		{
			/*
			 * Remove the to-be-awakened PROCs from the queue, and update the
			 * lock state to show them as holding the lock.
			 */
			proc = head;
			if (proc->lwExclusive)
			{
				lock->exclusive++;
			}
			else
			{
				lock->shared++;
				while (proc->lwWaitLink != NULL &&
					   !proc->lwWaitLink->lwExclusive)
				{
					proc = proc->lwWaitLink;
					lock->shared++;
				}
			}
			/* proc is now the last PROC to be released */
			lock->head = proc->lwWaitLink;
			proc->lwWaitLink = NULL;
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
		proc = head;
		head = proc->lwWaitLink;
		proc->lwWaitLink = NULL;
		proc->lwWaiting = false;
		IpcSemaphoreUnlock(proc->sem.semId, proc->sem.semNum);
	}

	/*
	 * Now okay to allow cancel/die interrupts.
	 */
	RESUME_INTERRUPTS();
}


/*
 * LWLockReleaseAll - release all currently-held locks
 *
 * Used to clean up after elog(ERROR).  An important difference between this
 * function and retail LWLockRelease calls is that InterruptHoldoffCount is
 * unchanged by this operation.  This is necessary since InterruptHoldoffCount
 * has been set to an appropriate level earlier in error recovery.  We could
 * decrement it below zero if we allow it to drop for each released lock!
 */
void
LWLockReleaseAll(void)
{
	while (num_held_lwlocks > 0)
	{
		HOLD_INTERRUPTS();		/* match the upcoming RESUME_INTERRUPTS */

		LWLockRelease(held_lwlocks[num_held_lwlocks-1]);
	}
}
