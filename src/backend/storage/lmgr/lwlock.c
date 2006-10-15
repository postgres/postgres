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
 * LWLocks to protect its shared state.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/lmgr/lwlock.c,v 1.47 2006/10/15 22:04:07 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/clog.h"
#include "access/multixact.h"
#include "access/subtrans.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/spin.h"


/* We use the ShmemLock spinlock to protect LWLockAssign */
extern slock_t *ShmemLock;


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
 * All the LWLock structs are allocated as an array in shared memory.
 * (LWLockIds are indexes into the array.)	We force the array stride to
 * be a power of 2, which saves a few cycles in indexing, but more
 * importantly also ensures that individual LWLocks don't cross cache line
 * boundaries.	This reduces cache contention problems, especially on AMD
 * Opterons.  (Of course, we have to also ensure that the array start
 * address is suitably aligned.)
 *
 * LWLock is between 16 and 32 bytes on all known platforms, so these two
 * cases are sufficient.
 */
#define LWLOCK_PADDED_SIZE	(sizeof(LWLock) <= 16 ? 16 : 32)

typedef union LWLockPadded
{
	LWLock		lock;
	char		pad[LWLOCK_PADDED_SIZE];
} LWLockPadded;

/*
 * This points to the array of LWLocks in shared memory.  Backends inherit
 * the pointer by fork from the postmaster (except in the EXEC_BACKEND case,
 * where we have special measures to pass it down).
 */
NON_EXEC_STATIC LWLockPadded *LWLockArray = NULL;


/*
 * We use this structure to keep track of locked LWLocks for release
 * during error recovery.  The maximum size could be determined at runtime
 * if necessary, but it seems unlikely that more than a few locks could
 * ever be held simultaneously.
 */
#define MAX_SIMUL_LWLOCKS	100

static int	num_held_lwlocks = 0;
static LWLockId held_lwlocks[MAX_SIMUL_LWLOCKS];

static int	lock_addin_request = 0;
static bool lock_addin_request_allowed = true;

#ifdef LWLOCK_STATS
static int	counts_for_pid = 0;
static int *sh_acquire_counts;
static int *ex_acquire_counts;
static int *block_counts;
#endif

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

#ifdef LWLOCK_STATS

static void
print_lwlock_stats(int code, Datum arg)
{
	int			i;
	int		   *LWLockCounter = (int *) ((char *) LWLockArray - 2 * sizeof(int));
	int			numLocks = LWLockCounter[1];

	/* Grab an LWLock to keep different backends from mixing reports */
	LWLockAcquire(0, LW_EXCLUSIVE);

	for (i = 0; i < numLocks; i++)
	{
		if (sh_acquire_counts[i] || ex_acquire_counts[i] || block_counts[i])
			fprintf(stderr, "PID %d lwlock %d: shacq %u exacq %u blk %u\n",
					MyProcPid, i, sh_acquire_counts[i], ex_acquire_counts[i],
					block_counts[i]);
	}

	LWLockRelease(0);
}
#endif   /* LWLOCK_STATS */


/*
 * Compute number of LWLocks to allocate.
 */
int
NumLWLocks(void)
{
	int			numLocks;

	/*
	 * Possibly this logic should be spread out among the affected modules,
	 * the same way that shmem space estimation is done.  But for now, there
	 * are few enough users of LWLocks that we can get away with just keeping
	 * the knowledge here.
	 */

	/* Predefined LWLocks */
	numLocks = (int) NumFixedLWLocks;

	/* bufmgr.c needs two for each shared buffer */
	numLocks += 2 * NBuffers;

	/* clog.c needs one per CLOG buffer */
	numLocks += NUM_CLOG_BUFFERS;

	/* subtrans.c needs one per SubTrans buffer */
	numLocks += NUM_SUBTRANS_BUFFERS;

	/* multixact.c needs two SLRU areas */
	numLocks += NUM_MXACTOFFSET_BUFFERS + NUM_MXACTMEMBER_BUFFERS;

	/*
	 * Add any requested by loadable modules; for backwards-compatibility
	 * reasons, allocate at least NUM_USER_DEFINED_LWLOCKS of them even
	 * if there are no explicit requests.
	 */
	lock_addin_request_allowed = false;
	numLocks += Max(lock_addin_request, NUM_USER_DEFINED_LWLOCKS);

	return numLocks;
}


/*
 * RequestAddinLWLocks
 *		Request that extra LWLocks be allocated for use by
 *		a loadable module.
 *
 * This is only useful if called from the _PG_init hook of a library that
 * is loaded into the postmaster via shared_preload_libraries.  Once
 * shared memory has been allocated, calls will be ignored.  (We could
 * raise an error, but it seems better to make it a no-op, so that
 * libraries containing such calls can be reloaded if needed.)
 */
void
RequestAddinLWLocks(int n)
{
	if (IsUnderPostmaster || !lock_addin_request_allowed)
		return;					/* too late */
	lock_addin_request += n;
}


/*
 * Compute shmem space needed for LWLocks.
 */
Size
LWLockShmemSize(void)
{
	Size		size;
	int			numLocks = NumLWLocks();

	/* Space for the LWLock array. */
	size = mul_size(numLocks, sizeof(LWLockPadded));

	/* Space for dynamic allocation counter, plus room for alignment. */
	size = add_size(size, 2 * sizeof(int) + LWLOCK_PADDED_SIZE);

	return size;
}


/*
 * Allocate shmem space for LWLocks and initialize the locks.
 */
void
CreateLWLocks(void)
{
	int			numLocks = NumLWLocks();
	Size		spaceLocks = LWLockShmemSize();
	LWLockPadded *lock;
	int		   *LWLockCounter;
	char	   *ptr;
	int			id;

	/* Allocate space */
	ptr = (char *) ShmemAlloc(spaceLocks);

	/* Leave room for dynamic allocation counter */
	ptr += 2 * sizeof(int);

	/* Ensure desired alignment of LWLock array */
	ptr += LWLOCK_PADDED_SIZE - ((unsigned long) ptr) % LWLOCK_PADDED_SIZE;

	LWLockArray = (LWLockPadded *) ptr;

	/*
	 * Initialize all LWLocks to "unlocked" state
	 */
	for (id = 0, lock = LWLockArray; id < numLocks; id++, lock++)
	{
		SpinLockInit(&lock->lock.mutex);
		lock->lock.releaseOK = true;
		lock->lock.exclusive = 0;
		lock->lock.shared = 0;
		lock->lock.head = NULL;
		lock->lock.tail = NULL;
	}

	/*
	 * Initialize the dynamic-allocation counter, which is stored just before
	 * the first LWLock.
	 */
	LWLockCounter = (int *) ((char *) LWLockArray - 2 * sizeof(int));
	LWLockCounter[0] = (int) NumFixedLWLocks;
	LWLockCounter[1] = numLocks;
}


/*
 * LWLockAssign - assign a dynamically-allocated LWLock number
 *
 * We interlock this using the same spinlock that is used to protect
 * ShmemAlloc().  Interlocking is not really necessary during postmaster
 * startup, but it is needed if any user-defined code tries to allocate
 * LWLocks after startup.
 */
LWLockId
LWLockAssign(void)
{
	LWLockId	result;

	/* use volatile pointer to prevent code rearrangement */
	volatile int *LWLockCounter;

	LWLockCounter = (int *) ((char *) LWLockArray - 2 * sizeof(int));
	SpinLockAcquire(ShmemLock);
	if (LWLockCounter[0] >= LWLockCounter[1])
	{
		SpinLockRelease(ShmemLock);
		elog(ERROR, "no more LWLockIds available");
	}
	result = (LWLockId) (LWLockCounter[0]++);
	SpinLockRelease(ShmemLock);
	return result;
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
	volatile LWLock *lock = &(LWLockArray[lockid].lock);
	PGPROC	   *proc = MyProc;
	bool		retry = false;
	int			extraWaits = 0;

	PRINT_LWDEBUG("LWLockAcquire", lockid, lock);

#ifdef LWLOCK_STATS
	/* Set up local count state first time through in a given process */
	if (counts_for_pid != MyProcPid)
	{
		int		   *LWLockCounter = (int *) ((char *) LWLockArray - 2 * sizeof(int));
		int			numLocks = LWLockCounter[1];

		sh_acquire_counts = calloc(numLocks, sizeof(int));
		ex_acquire_counts = calloc(numLocks, sizeof(int));
		block_counts = calloc(numLocks, sizeof(int));
		counts_for_pid = MyProcPid;
		on_shmem_exit(print_lwlock_stats, 0);
	}
	/* Count lock acquisition attempts */
	if (mode == LW_EXCLUSIVE)
		ex_acquire_counts[lockid]++;
	else
		sh_acquire_counts[lockid]++;
#endif   /* LWLOCK_STATS */

	/*
	 * We can't wait if we haven't got a PGPROC.  This should only occur
	 * during bootstrap or shared memory initialization.  Put an Assert here
	 * to catch unsafe coding practices.
	 */
	Assert(!(proc == NULL && IsUnderPostmaster));

	/* Ensure we will have room to remember the lock */
	if (num_held_lwlocks >= MAX_SIMUL_LWLOCKS)
		elog(ERROR, "too many LWLocks taken");

	/*
	 * Lock out cancel/die interrupts until we exit the code section protected
	 * by the LWLock.  This ensures that interrupts will not interfere with
	 * manipulations of data structures in shared memory.
	 */
	HOLD_INTERRUPTS();

	/*
	 * Loop here to try to acquire lock after each time we are signaled by
	 * LWLockRelease.
	 *
	 * NOTE: it might seem better to have LWLockRelease actually grant us the
	 * lock, rather than retrying and possibly having to go back to sleep. But
	 * in practice that is no good because it means a process swap for every
	 * lock acquisition when two or more processes are contending for the same
	 * lock.  Since LWLocks are normally used to protect not-very-long
	 * sections of computation, a process needs to be able to acquire and
	 * release the same lock many times during a single CPU time slice, even
	 * in the presence of contention.  The efficiency of being able to do that
	 * outweighs the inefficiency of sometimes wasting a process dispatch
	 * cycle because the lock is not free when a released waiter finally gets
	 * to run.	See pgsql-hackers archives for 29-Dec-01.
	 */
	for (;;)
	{
		bool		mustwait;

		/* Acquire mutex.  Time spent holding mutex should be short! */
		SpinLockAcquire(&lock->mutex);

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
		 * should never occur, since MyProc should only be null during shared
		 * memory initialization.
		 */
		if (proc == NULL)
			elog(PANIC, "cannot wait without a PGPROC structure");

		proc->lwWaiting = true;
		proc->lwExclusive = (mode == LW_EXCLUSIVE);
		proc->lwWaitLink = NULL;
		if (lock->head == NULL)
			lock->head = proc;
		else
			lock->tail->lwWaitLink = proc;
		lock->tail = proc;

		/* Can release the mutex now */
		SpinLockRelease(&lock->mutex);

		/*
		 * Wait until awakened.
		 *
		 * Since we share the process wait semaphore with the regular lock
		 * manager and ProcWaitForSignal, and we may need to acquire an LWLock
		 * while one of those is pending, it is possible that we get awakened
		 * for a reason other than being signaled by LWLockRelease. If so,
		 * loop back and wait again.  Once we've gotten the LWLock,
		 * re-increment the sema by the number of additional signals received,
		 * so that the lock manager or signal manager will see the received
		 * signal when it next waits.
		 */
		LOG_LWDEBUG("LWLockAcquire", lockid, "waiting");

#ifdef LWLOCK_STATS
		block_counts[lockid]++;
#endif

		PG_TRACE2(lwlock__startwait, lockid, mode);

		for (;;)
		{
			/* "false" means cannot accept cancel/die interrupt here. */
			PGSemaphoreLock(&proc->sem, false);
			if (!proc->lwWaiting)
				break;
			extraWaits++;
		}

		PG_TRACE2(lwlock__endwait, lockid, mode);

		LOG_LWDEBUG("LWLockAcquire", lockid, "awakened");

		/* Now loop back and try to acquire lock again. */
		retry = true;
	}

	/* We are done updating shared state of the lock itself. */
	SpinLockRelease(&lock->mutex);

	PG_TRACE2(lwlock__acquire, lockid, mode);

	/* Add lock to list of locks held by this backend */
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
	volatile LWLock *lock = &(LWLockArray[lockid].lock);
	bool		mustwait;

	PRINT_LWDEBUG("LWLockConditionalAcquire", lockid, lock);

	/* Ensure we will have room to remember the lock */
	if (num_held_lwlocks >= MAX_SIMUL_LWLOCKS)
		elog(ERROR, "too many LWLocks taken");

	/*
	 * Lock out cancel/die interrupts until we exit the code section protected
	 * by the LWLock.  This ensures that interrupts will not interfere with
	 * manipulations of data structures in shared memory.
	 */
	HOLD_INTERRUPTS();

	/* Acquire mutex.  Time spent holding mutex should be short! */
	SpinLockAcquire(&lock->mutex);

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
	SpinLockRelease(&lock->mutex);

	if (mustwait)
	{
		/* Failed to get lock, so release interrupt holdoff */
		RESUME_INTERRUPTS();
		LOG_LWDEBUG("LWLockConditionalAcquire", lockid, "failed");
		PG_TRACE2(lwlock__condacquire__fail, lockid, mode);
	}
	else
	{
		/* Add lock to list of locks held by this backend */
		held_lwlocks[num_held_lwlocks++] = lockid;
		PG_TRACE2(lwlock__condacquire, lockid, mode);
	}

	return !mustwait;
}

/*
 * LWLockRelease - release a previously acquired lock
 */
void
LWLockRelease(LWLockId lockid)
{
	volatile LWLock *lock = &(LWLockArray[lockid].lock);
	PGPROC	   *head;
	PGPROC	   *proc;
	int			i;

	PRINT_LWDEBUG("LWLockRelease", lockid, lock);

	/*
	 * Remove lock from list of locks held.  Usually, but not always, it will
	 * be the latest-acquired lock; so search array backwards.
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
	SpinLockAcquire(&lock->mutex);

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
	 * hold, there cannot be anything to do.  Also, do not awaken any waiters
	 * if someone has already awakened waiters that haven't yet acquired the
	 * lock.
	 */
	head = lock->head;
	if (head != NULL)
	{
		if (lock->exclusive == 0 && lock->shared == 0 && lock->releaseOK)
		{
			/*
			 * Remove the to-be-awakened PGPROCs from the queue.  If the front
			 * waiter wants exclusive lock, awaken him only. Otherwise awaken
			 * as many waiters as want shared access.
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
	SpinLockRelease(&lock->mutex);

	PG_TRACE1(lwlock__release, lockid);

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


/*
 * LWLockHeldByMe - test whether my process currently holds a lock
 *
 * This is meant as debug support only.  We do not distinguish whether the
 * lock is held shared or exclusive.
 */
bool
LWLockHeldByMe(LWLockId lockid)
{
	int			i;

	for (i = 0; i < num_held_lwlocks; i++)
	{
		if (held_lwlocks[i] == lockid)
			return true;
	}
	return false;
}
