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
 * LWLocks to protect its shared state.
 *
 * In addition to exclusive and shared modes, lightweight locks can be used to
 * wait until a variable changes value.  The variable is initially not set
 * when the lock is acquired with LWLockAcquire, i.e. it remains set to the
 * value it was set to when the lock was released last, and can be updated
 * without releasing the lock by calling LWLockUpdateVar.  LWLockWaitForVar
 * waits for the variable to be updated, or until the lock is free.  When
 * releasing the lock with LWLockReleaseClearVar() the value can be set to an
 * appropriate value for a free lock.  The meaning of the variable is up to
 * the caller, the lightweight lock code just assigns and compares it.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/lmgr/lwlock.c
 *
 * NOTES:
 *
 * This used to be a pretty straight forward reader-writer lock
 * implementation, in which the internal state was protected by a
 * spinlock. Unfortunately the overhead of taking the spinlock proved to be
 * too high for workloads/locks that were taken in shared mode very
 * frequently. Often we were spinning in the (obviously exclusive) spinlock,
 * while trying to acquire a shared lock that was actually free.
 *
 * Thus a new implementation was devised that provides wait-free shared lock
 * acquisition for locks that aren't exclusively locked.
 *
 * The basic idea is to have a single atomic variable 'lockcount' instead of
 * the formerly separate shared and exclusive counters and to use atomic
 * operations to acquire the lock. That's fairly easy to do for plain
 * rw-spinlocks, but a lot harder for something like LWLocks that want to wait
 * in the OS.
 *
 * For lock acquisition we use an atomic compare-and-exchange on the lockcount
 * variable. For exclusive lock we swap in a sentinel value
 * (LW_VAL_EXCLUSIVE), for shared locks we count the number of holders.
 *
 * To release the lock we use an atomic decrement to release the lock. If the
 * new value is zero (we get that atomically), we know we can/have to release
 * waiters.
 *
 * Obviously it is important that the sentinel value for exclusive locks
 * doesn't conflict with the maximum number of possible share lockers -
 * luckily MAX_BACKENDS makes that easily possible.
 *
 *
 * The attentive reader might have noticed that naively doing the above has a
 * glaring race condition: We try to lock using the atomic operations and
 * notice that we have to wait. Unfortunately by the time we have finished
 * queuing, the former locker very well might have already finished it's
 * work. That's problematic because we're now stuck waiting inside the OS.

 * To mitigate those races we use a two phased attempt at locking:
 *	 Phase 1: Try to do it atomically, if we succeed, nice
 *	 Phase 2: Add ourselves to the waitqueue of the lock
 *	 Phase 3: Try to grab the lock again, if we succeed, remove ourselves from
 *			  the queue
 *	 Phase 4: Sleep till wake-up, goto Phase 1
 *
 * This protects us against the problem from above as nobody can release too
 *	  quick, before we're queued, since after Phase 2 we're already queued.
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "pg_trace.h"
#include "postmaster/postmaster.h"
#include "replication/slot.h"
#include "storage/ipc.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/spin.h"
#include "utils/memutils.h"

#ifdef LWLOCK_STATS
#include "utils/hsearch.h"
#endif


/* We use the ShmemLock spinlock to protect LWLockAssign */
extern slock_t *ShmemLock;

#define LW_FLAG_HAS_WAITERS			((uint32) 1 << 30)
#define LW_FLAG_RELEASE_OK			((uint32) 1 << 29)

#define LW_VAL_EXCLUSIVE			((uint32) 1 << 24)
#define LW_VAL_SHARED				1

#define LW_LOCK_MASK				((uint32) ((1 << 25)-1))
/* Must be greater than MAX_BACKENDS - which is 2^23-1, so we're fine. */
#define LW_SHARED_MASK				((uint32) ((1 << 24)-1))

/*
 * This is indexed by tranche ID and stores metadata for all tranches known
 * to the current backend.
 */
static LWLockTranche **LWLockTrancheArray = NULL;
static int	LWLockTranchesAllocated = 0;

#define T_NAME(lock) \
	(LWLockTrancheArray[(lock)->tranche]->name)
#define T_ID(lock) \
	((int) ((((char *) lock) - \
		((char *) LWLockTrancheArray[(lock)->tranche]->array_base)) / \
		LWLockTrancheArray[(lock)->tranche]->array_stride))

/*
 * This points to the main array of LWLocks in shared memory.  Backends inherit
 * the pointer by fork from the postmaster (except in the EXEC_BACKEND case,
 * where we have special measures to pass it down).
 */
LWLockPadded *MainLWLockArray = NULL;
static LWLockTranche MainLWLockTranche;

/*
 * We use this structure to keep track of locked LWLocks for release
 * during error recovery.  Normally, only a few will be held at once, but
 * occasionally the number can be much higher; for example, the pg_buffercache
 * extension locks all buffer partitions simultaneously.
 */
#define MAX_SIMUL_LWLOCKS	200

/* struct representing the LWLocks we're holding */
typedef struct LWLockHandle
{
	LWLock	   *lock;
	LWLockMode	mode;
} LWLockHandle;

static int	num_held_lwlocks = 0;
static LWLockHandle held_lwlocks[MAX_SIMUL_LWLOCKS];

static int	lock_addin_request = 0;
static bool lock_addin_request_allowed = true;

#ifdef LWLOCK_STATS
typedef struct lwlock_stats_key
{
	int			tranche;
	int			instance;
}	lwlock_stats_key;

typedef struct lwlock_stats
{
	lwlock_stats_key key;
	int			sh_acquire_count;
	int			ex_acquire_count;
	int			block_count;
	int			dequeue_self_count;
	int			spin_delay_count;
}	lwlock_stats;

static HTAB *lwlock_stats_htab;
static lwlock_stats lwlock_stats_dummy;
#endif

#ifdef LOCK_DEBUG
bool		Trace_lwlocks = false;

inline static void
PRINT_LWDEBUG(const char *where, LWLock *lock, LWLockMode mode)
{
	/* hide statement & context here, otherwise the log is just too verbose */
	if (Trace_lwlocks)
	{
		uint32		state = pg_atomic_read_u32(&lock->state);
		int			id = T_ID(lock);

		if (lock->tranche == 0 && id < NUM_INDIVIDUAL_LWLOCKS)
			ereport(LOG,
					(errhidestmt(true),
					 errhidecontext(true),
					 errmsg_internal("%d: %s(%s): excl %u shared %u haswaiters %u waiters %u rOK %d",
							MyProcPid,
							where, MainLWLockNames[id],
							!!(state & LW_VAL_EXCLUSIVE),
							state & LW_SHARED_MASK,
							!!(state & LW_FLAG_HAS_WAITERS),
							pg_atomic_read_u32(&lock->nwaiters),
							!!(state & LW_FLAG_RELEASE_OK))));
		else
			ereport(LOG,
					(errhidestmt(true),
					 errhidecontext(true),
					 errmsg_internal("%d: %s(%s %d): excl %u shared %u haswaiters %u waiters %u rOK %d",
							MyProcPid,
							where, T_NAME(lock), id,
							!!(state & LW_VAL_EXCLUSIVE),
							state & LW_SHARED_MASK,
							!!(state & LW_FLAG_HAS_WAITERS),
							pg_atomic_read_u32(&lock->nwaiters),
							!!(state & LW_FLAG_RELEASE_OK))));
	}
}

inline static void
LOG_LWDEBUG(const char *where, LWLock *lock, const char *msg)
{
	/* hide statement & context here, otherwise the log is just too verbose */
	if (Trace_lwlocks)
	{
		int			id = T_ID(lock);

		if (lock->tranche == 0 && id < NUM_INDIVIDUAL_LWLOCKS)
			ereport(LOG,
					(errhidestmt(true),
					 errhidecontext(true),
					 errmsg_internal("%s(%s): %s", where,
							MainLWLockNames[id], msg)));
		else
			ereport(LOG,
					(errhidestmt(true),
					 errhidecontext(true),
					 errmsg_internal("%s(%s %d): %s", where,
							T_NAME(lock), id, msg)));
	}
}

#else							/* not LOCK_DEBUG */
#define PRINT_LWDEBUG(a,b,c) ((void)0)
#define LOG_LWDEBUG(a,b,c) ((void)0)
#endif   /* LOCK_DEBUG */

#ifdef LWLOCK_STATS

static void init_lwlock_stats(void);
static void print_lwlock_stats(int code, Datum arg);
static lwlock_stats *get_lwlock_stats_entry(LWLock *lockid);

static void
init_lwlock_stats(void)
{
	HASHCTL		ctl;
	static MemoryContext lwlock_stats_cxt = NULL;
	static bool exit_registered = false;

	if (lwlock_stats_cxt != NULL)
		MemoryContextDelete(lwlock_stats_cxt);

	/*
	 * The LWLock stats will be updated within a critical section, which
	 * requires allocating new hash entries. Allocations within a critical
	 * section are normally not allowed because running out of memory would
	 * lead to a PANIC, but LWLOCK_STATS is debugging code that's not normally
	 * turned on in production, so that's an acceptable risk. The hash entries
	 * are small, so the risk of running out of memory is minimal in practice.
	 */
	lwlock_stats_cxt = AllocSetContextCreate(TopMemoryContext,
											 "LWLock stats",
											 ALLOCSET_DEFAULT_MINSIZE,
											 ALLOCSET_DEFAULT_INITSIZE,
											 ALLOCSET_DEFAULT_MAXSIZE);
	MemoryContextAllowInCriticalSection(lwlock_stats_cxt, true);

	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(lwlock_stats_key);
	ctl.entrysize = sizeof(lwlock_stats);
	ctl.hcxt = lwlock_stats_cxt;
	lwlock_stats_htab = hash_create("lwlock stats", 16384, &ctl,
									HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	if (!exit_registered)
	{
		on_shmem_exit(print_lwlock_stats, 0);
		exit_registered = true;
	}
}

static void
print_lwlock_stats(int code, Datum arg)
{
	HASH_SEQ_STATUS scan;
	lwlock_stats *lwstats;

	hash_seq_init(&scan, lwlock_stats_htab);

	/* Grab an LWLock to keep different backends from mixing reports */
	LWLockAcquire(&MainLWLockArray[0].lock, LW_EXCLUSIVE);

	while ((lwstats = (lwlock_stats *) hash_seq_search(&scan)) != NULL)
	{
		fprintf(stderr,
				"PID %d lwlock %s %d: shacq %u exacq %u blk %u spindelay %u dequeue self %u\n",
				MyProcPid, LWLockTrancheArray[lwstats->key.tranche]->name,
				lwstats->key.instance, lwstats->sh_acquire_count,
				lwstats->ex_acquire_count, lwstats->block_count,
				lwstats->spin_delay_count, lwstats->dequeue_self_count);
	}

	LWLockRelease(&MainLWLockArray[0].lock);
}

static lwlock_stats *
get_lwlock_stats_entry(LWLock *lock)
{
	lwlock_stats_key key;
	lwlock_stats *lwstats;
	bool		found;

	/*
	 * During shared memory initialization, the hash table doesn't exist yet.
	 * Stats of that phase aren't very interesting, so just collect operations
	 * on all locks in a single dummy entry.
	 */
	if (lwlock_stats_htab == NULL)
		return &lwlock_stats_dummy;

	/* Fetch or create the entry. */
	key.tranche = lock->tranche;
	key.instance = T_ID(lock);
	lwstats = hash_search(lwlock_stats_htab, &key, HASH_ENTER, &found);
	if (!found)
	{
		lwstats->sh_acquire_count = 0;
		lwstats->ex_acquire_count = 0;
		lwstats->block_count = 0;
		lwstats->dequeue_self_count = 0;
		lwstats->spin_delay_count = 0;
	}
	return lwstats;
}
#endif   /* LWLOCK_STATS */


/*
 * Compute number of LWLocks to allocate in the main array.
 */
static int
NumLWLocks(void)
{
	int			numLocks;

	/*
	 * Many users of LWLocks no longer reserve space in the main array here,
	 * but instead allocate separate tranches.  The latter approach has the
	 * advantage of allowing LWLOCK_STATS and LOCK_DEBUG output to produce
	 * more useful output.
	 */

	/* Predefined LWLocks */
	numLocks = NUM_FIXED_LWLOCKS;

	/* proc.c needs one for each backend or auxiliary process */
	numLocks += MaxBackends + NUM_AUXILIARY_PROCS;

	/* slot.c needs one for each slot */
	numLocks += max_replication_slots;

	/*
	 * Add any requested by loadable modules; for backwards-compatibility
	 * reasons, allocate at least NUM_USER_DEFINED_LWLOCKS of them even if
	 * there are no explicit requests.
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
	size = add_size(size, 3 * sizeof(int) + LWLOCK_PADDED_SIZE);

	return size;
}


/*
 * Allocate shmem space for the main LWLock array and initialize it.  We also
 * register the main tranch here.
 */
void
CreateLWLocks(void)
{
	StaticAssertExpr(LW_VAL_EXCLUSIVE > (uint32) MAX_BACKENDS,
					 "MAX_BACKENDS too big for lwlock.c");

	StaticAssertExpr(sizeof(LWLock) <= LWLOCK_MINIMAL_SIZE &&
					 sizeof(LWLock) <= LWLOCK_PADDED_SIZE,
					 "Miscalculated LWLock padding");

	if (!IsUnderPostmaster)
	{
		int			numLocks = NumLWLocks();
		Size		spaceLocks = LWLockShmemSize();
		LWLockPadded *lock;
		int		   *LWLockCounter;
		char	   *ptr;
		int			id;

		/* Allocate space */
		ptr = (char *) ShmemAlloc(spaceLocks);

		/* Leave room for dynamic allocation of locks and tranches */
		ptr += 3 * sizeof(int);

		/* Ensure desired alignment of LWLock array */
		ptr += LWLOCK_PADDED_SIZE - ((uintptr_t) ptr) % LWLOCK_PADDED_SIZE;

		MainLWLockArray = (LWLockPadded *) ptr;

		/* Initialize all LWLocks in main array */
		for (id = 0, lock = MainLWLockArray; id < numLocks; id++, lock++)
			LWLockInitialize(&lock->lock, LWTRANCHE_MAIN);

		/*
		 * Initialize the dynamic-allocation counters, which are stored just
		 * before the first LWLock.  LWLockCounter[0] is the allocation
		 * counter for lwlocks, LWLockCounter[1] is the maximum number that
		 * can be allocated from the main array, and LWLockCounter[2] is the
		 * allocation counter for tranches.
		 */
		LWLockCounter = (int *) ((char *) MainLWLockArray - 3 * sizeof(int));
		LWLockCounter[0] = NUM_FIXED_LWLOCKS;
		LWLockCounter[1] = numLocks;
		LWLockCounter[2] = LWTRANCHE_FIRST_USER_DEFINED;
	}

	if (LWLockTrancheArray == NULL)
	{
		LWLockTranchesAllocated = 16;
		LWLockTrancheArray = (LWLockTranche **)
			MemoryContextAlloc(TopMemoryContext,
						  LWLockTranchesAllocated * sizeof(LWLockTranche *));
		Assert(LWLockTranchesAllocated >= LWTRANCHE_FIRST_USER_DEFINED);
	}

	MainLWLockTranche.name = "main";
	MainLWLockTranche.array_base = MainLWLockArray;
	MainLWLockTranche.array_stride = sizeof(LWLockPadded);
	LWLockRegisterTranche(LWTRANCHE_MAIN, &MainLWLockTranche);
}

/*
 * InitLWLockAccess - initialize backend-local state needed to hold LWLocks
 */
void
InitLWLockAccess(void)
{
#ifdef LWLOCK_STATS
	init_lwlock_stats();
#endif
}

/*
 * LWLockAssign - assign a dynamically-allocated LWLock number
 *
 * We interlock this using the same spinlock that is used to protect
 * ShmemAlloc().  Interlocking is not really necessary during postmaster
 * startup, but it is needed if any user-defined code tries to allocate
 * LWLocks after startup.
 */
LWLock *
LWLockAssign(void)
{
	LWLock	   *result;
	int		   *LWLockCounter;

	LWLockCounter = (int *) ((char *) MainLWLockArray - 3 * sizeof(int));
	SpinLockAcquire(ShmemLock);
	if (LWLockCounter[0] >= LWLockCounter[1])
	{
		SpinLockRelease(ShmemLock);
		elog(ERROR, "no more LWLocks available");
	}
	result = &MainLWLockArray[LWLockCounter[0]++].lock;
	SpinLockRelease(ShmemLock);
	return result;
}

/*
 * Allocate a new tranche ID.
 */
int
LWLockNewTrancheId(void)
{
	int			result;
	int		   *LWLockCounter;

	LWLockCounter = (int *) ((char *) MainLWLockArray - 3 * sizeof(int));
	SpinLockAcquire(ShmemLock);
	result = LWLockCounter[2]++;
	SpinLockRelease(ShmemLock);

	return result;
}

/*
 * Register a tranche ID in the lookup table for the current process.  This
 * routine will save a pointer to the tranche object passed as an argument,
 * so that object should be allocated in a backend-lifetime context
 * (TopMemoryContext, static variable, or similar).
 */
void
LWLockRegisterTranche(int tranche_id, LWLockTranche *tranche)
{
	Assert(LWLockTrancheArray != NULL);

	if (tranche_id >= LWLockTranchesAllocated)
	{
		int			i = LWLockTranchesAllocated;

		while (i <= tranche_id)
			i *= 2;

		LWLockTrancheArray = (LWLockTranche **)
			repalloc(LWLockTrancheArray,
					 i * sizeof(LWLockTranche *));
		LWLockTranchesAllocated = i;
	}

	LWLockTrancheArray[tranche_id] = tranche;
}

/*
 * LWLockInitialize - initialize a new lwlock; it's initially unlocked
 */
void
LWLockInitialize(LWLock *lock, int tranche_id)
{
	SpinLockInit(&lock->mutex);
	pg_atomic_init_u32(&lock->state, LW_FLAG_RELEASE_OK);
#ifdef LOCK_DEBUG
	pg_atomic_init_u32(&lock->nwaiters, 0);
#endif
	lock->tranche = tranche_id;
	dlist_init(&lock->waiters);
}

/*
 * Internal function that tries to atomically acquire the lwlock in the passed
 * in mode.
 *
 * This function will not block waiting for a lock to become free - that's the
 * callers job.
 *
 * Returns true if the lock isn't free and we need to wait.
 */
static bool
LWLockAttemptLock(LWLock *lock, LWLockMode mode)
{
	uint32		old_state;

	AssertArg(mode == LW_EXCLUSIVE || mode == LW_SHARED);

	/*
	 * Read once outside the loop, later iterations will get the newer value
	 * via compare & exchange.
	 */
	old_state = pg_atomic_read_u32(&lock->state);

	/* loop until we've determined whether we could acquire the lock or not */
	while (true)
	{
		uint32		desired_state;
		bool		lock_free;

		desired_state = old_state;

		if (mode == LW_EXCLUSIVE)
		{
			lock_free = (old_state & LW_LOCK_MASK) == 0;
			if (lock_free)
				desired_state += LW_VAL_EXCLUSIVE;
		}
		else
		{
			lock_free = (old_state & LW_VAL_EXCLUSIVE) == 0;
			if (lock_free)
				desired_state += LW_VAL_SHARED;
		}

		/*
		 * Attempt to swap in the state we are expecting. If we didn't see
		 * lock to be free, that's just the old value. If we saw it as free,
		 * we'll attempt to mark it acquired. The reason that we always swap
		 * in the value is that this doubles as a memory barrier. We could try
		 * to be smarter and only swap in values if we saw the lock as free,
		 * but benchmark haven't shown it as beneficial so far.
		 *
		 * Retry if the value changed since we last looked at it.
		 */
		if (pg_atomic_compare_exchange_u32(&lock->state,
										   &old_state, desired_state))
		{
			if (lock_free)
			{
				/* Great! Got the lock. */
#ifdef LOCK_DEBUG
				if (mode == LW_EXCLUSIVE)
					lock->owner = MyProc;
#endif
				return false;
			}
			else
				return true;	/* someobdy else has the lock */
		}
	}
	pg_unreachable();
}

/*
 * Wakeup all the lockers that currently have a chance to acquire the lock.
 */
static void
LWLockWakeup(LWLock *lock)
{
	bool		new_release_ok;
	bool		wokeup_somebody = false;
	dlist_head	wakeup;
	dlist_mutable_iter iter;
#ifdef LWLOCK_STATS
	lwlock_stats *lwstats;

	lwstats = get_lwlock_stats_entry(lock);
#endif

	dlist_init(&wakeup);

	new_release_ok = true;

	/* Acquire mutex.  Time spent holding mutex should be short! */
#ifdef LWLOCK_STATS
	lwstats->spin_delay_count += SpinLockAcquire(&lock->mutex);
#else
	SpinLockAcquire(&lock->mutex);
#endif

	dlist_foreach_modify(iter, &lock->waiters)
	{
		PGPROC	   *waiter = dlist_container(PGPROC, lwWaitLink, iter.cur);

		if (wokeup_somebody && waiter->lwWaitMode == LW_EXCLUSIVE)
			continue;

		dlist_delete(&waiter->lwWaitLink);
		dlist_push_tail(&wakeup, &waiter->lwWaitLink);

		if (waiter->lwWaitMode != LW_WAIT_UNTIL_FREE)
		{
			/*
			 * Prevent additional wakeups until retryer gets to run. Backends
			 * that are just waiting for the lock to become free don't retry
			 * automatically.
			 */
			new_release_ok = false;

			/*
			 * Don't wakeup (further) exclusive locks.
			 */
			wokeup_somebody = true;
		}

		/*
		 * Once we've woken up an exclusive lock, there's no point in waking
		 * up anybody else.
		 */
		if (waiter->lwWaitMode == LW_EXCLUSIVE)
			break;
	}

	Assert(dlist_is_empty(&wakeup) || pg_atomic_read_u32(&lock->state) & LW_FLAG_HAS_WAITERS);

	/* Unset both flags at once if required */
	if (!new_release_ok && dlist_is_empty(&wakeup))
		pg_atomic_fetch_and_u32(&lock->state,
								~(LW_FLAG_RELEASE_OK | LW_FLAG_HAS_WAITERS));
	else if (!new_release_ok)
		pg_atomic_fetch_and_u32(&lock->state, ~LW_FLAG_RELEASE_OK);
	else if (dlist_is_empty(&wakeup))
		pg_atomic_fetch_and_u32(&lock->state, ~LW_FLAG_HAS_WAITERS);
	else if (new_release_ok)
		pg_atomic_fetch_or_u32(&lock->state, LW_FLAG_RELEASE_OK);

	/* We are done updating the shared state of the lock queue. */
	SpinLockRelease(&lock->mutex);

	/* Awaken any waiters I removed from the queue. */
	dlist_foreach_modify(iter, &wakeup)
	{
		PGPROC	   *waiter = dlist_container(PGPROC, lwWaitLink, iter.cur);

		LOG_LWDEBUG("LWLockRelease", lock, "release waiter");
		dlist_delete(&waiter->lwWaitLink);

		/*
		 * Guarantee that lwWaiting being unset only becomes visible once the
		 * unlink from the link has completed. Otherwise the target backend
		 * could be woken up for other reason and enqueue for a new lock - if
		 * that happens before the list unlink happens, the list would end up
		 * being corrupted.
		 *
		 * The barrier pairs with the SpinLockAcquire() when enqueing for
		 * another lock.
		 */
		pg_write_barrier();
		waiter->lwWaiting = false;
		PGSemaphoreUnlock(&waiter->sem);
	}
}

/*
 * Add ourselves to the end of the queue.
 *
 * NB: Mode can be LW_WAIT_UNTIL_FREE here!
 */
static void
LWLockQueueSelf(LWLock *lock, LWLockMode mode)
{
#ifdef LWLOCK_STATS
	lwlock_stats *lwstats;

	lwstats = get_lwlock_stats_entry(lock);
#endif

	/*
	 * If we don't have a PGPROC structure, there's no way to wait. This
	 * should never occur, since MyProc should only be null during shared
	 * memory initialization.
	 */
	if (MyProc == NULL)
		elog(PANIC, "cannot wait without a PGPROC structure");

	if (MyProc->lwWaiting)
		elog(PANIC, "queueing for lock while waiting on another one");

#ifdef LWLOCK_STATS
	lwstats->spin_delay_count += SpinLockAcquire(&lock->mutex);
#else
	SpinLockAcquire(&lock->mutex);
#endif

	/* setting the flag is protected by the spinlock */
	pg_atomic_fetch_or_u32(&lock->state, LW_FLAG_HAS_WAITERS);

	MyProc->lwWaiting = true;
	MyProc->lwWaitMode = mode;

	/* LW_WAIT_UNTIL_FREE waiters are always at the front of the queue */
	if (mode == LW_WAIT_UNTIL_FREE)
		dlist_push_head(&lock->waiters, &MyProc->lwWaitLink);
	else
		dlist_push_tail(&lock->waiters, &MyProc->lwWaitLink);

	/* Can release the mutex now */
	SpinLockRelease(&lock->mutex);

#ifdef LOCK_DEBUG
	pg_atomic_fetch_add_u32(&lock->nwaiters, 1);
#endif

}

/*
 * Remove ourselves from the waitlist.
 *
 * This is used if we queued ourselves because we thought we needed to sleep
 * but, after further checking, we discovered that we don't actually need to
 * do so.
 */
static void
LWLockDequeueSelf(LWLock *lock)
{
	bool		found = false;
	dlist_mutable_iter iter;

#ifdef LWLOCK_STATS
	lwlock_stats *lwstats;

	lwstats = get_lwlock_stats_entry(lock);

	lwstats->dequeue_self_count++;
#endif

#ifdef LWLOCK_STATS
	lwstats->spin_delay_count += SpinLockAcquire(&lock->mutex);
#else
	SpinLockAcquire(&lock->mutex);
#endif

	/*
	 * Can't just remove ourselves from the list, but we need to iterate over
	 * all entries as somebody else could have unqueued us.
	 */
	dlist_foreach_modify(iter, &lock->waiters)
	{
		PGPROC	   *proc = dlist_container(PGPROC, lwWaitLink, iter.cur);

		if (proc == MyProc)
		{
			found = true;
			dlist_delete(&proc->lwWaitLink);
			break;
		}
	}

	if (dlist_is_empty(&lock->waiters) &&
		(pg_atomic_read_u32(&lock->state) & LW_FLAG_HAS_WAITERS) != 0)
	{
		pg_atomic_fetch_and_u32(&lock->state, ~LW_FLAG_HAS_WAITERS);
	}

	SpinLockRelease(&lock->mutex);

	/* clear waiting state again, nice for debugging */
	if (found)
		MyProc->lwWaiting = false;
	else
	{
		int			extraWaits = 0;

		/*
		 * Somebody else dequeued us and has or will wake us up. Deal with the
		 * superfluous absorption of a wakeup.
		 */

		/*
		 * Reset releaseOk if somebody woke us before we removed ourselves -
		 * they'll have set it to false.
		 */
		pg_atomic_fetch_or_u32(&lock->state, LW_FLAG_RELEASE_OK);

		/*
		 * Now wait for the scheduled wakeup, otherwise our ->lwWaiting would
		 * get reset at some inconvenient point later. Most of the time this
		 * will immediately return.
		 */
		for (;;)
		{
			PGSemaphoreLock(&MyProc->sem);
			if (!MyProc->lwWaiting)
				break;
			extraWaits++;
		}

		/*
		 * Fix the process wait semaphore's count for any absorbed wakeups.
		 */
		while (extraWaits-- > 0)
			PGSemaphoreUnlock(&MyProc->sem);
	}

#ifdef LOCK_DEBUG
	{
		/* not waiting anymore */
		uint32 nwaiters PG_USED_FOR_ASSERTS_ONLY = pg_atomic_fetch_sub_u32(&lock->nwaiters, 1);

		Assert(nwaiters < MAX_BACKENDS);
	}
#endif
}

/*
 * LWLockAcquire - acquire a lightweight lock in the specified mode
 *
 * If the lock is not available, sleep until it is.  Returns true if the lock
 * was available immediately, false if we had to sleep.
 *
 * Side effect: cancel/die interrupts are held off until lock release.
 */
bool
LWLockAcquire(LWLock *lock, LWLockMode mode)
{
	PGPROC	   *proc = MyProc;
	bool		result = true;
	int			extraWaits = 0;
#ifdef LWLOCK_STATS
	lwlock_stats *lwstats;

	lwstats = get_lwlock_stats_entry(lock);
#endif

	AssertArg(mode == LW_SHARED || mode == LW_EXCLUSIVE);

	PRINT_LWDEBUG("LWLockAcquire", lock, mode);

#ifdef LWLOCK_STATS
	/* Count lock acquisition attempts */
	if (mode == LW_EXCLUSIVE)
		lwstats->ex_acquire_count++;
	else
		lwstats->sh_acquire_count++;
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
	 * to run.  See pgsql-hackers archives for 29-Dec-01.
	 */
	for (;;)
	{
		bool		mustwait;

		/*
		 * Try to grab the lock the first time, we're not in the waitqueue
		 * yet/anymore.
		 */
		mustwait = LWLockAttemptLock(lock, mode);

		if (!mustwait)
		{
			LOG_LWDEBUG("LWLockAcquire", lock, "immediately acquired lock");
			break;				/* got the lock */
		}

		/*
		 * Ok, at this point we couldn't grab the lock on the first try. We
		 * cannot simply queue ourselves to the end of the list and wait to be
		 * woken up because by now the lock could long have been released.
		 * Instead add us to the queue and try to grab the lock again. If we
		 * succeed we need to revert the queuing and be happy, otherwise we
		 * recheck the lock. If we still couldn't grab it, we know that the
		 * other lock will see our queue entries when releasing since they
		 * existed before we checked for the lock.
		 */

		/* add to the queue */
		LWLockQueueSelf(lock, mode);

		/* we're now guaranteed to be woken up if necessary */
		mustwait = LWLockAttemptLock(lock, mode);

		/* ok, grabbed the lock the second time round, need to undo queueing */
		if (!mustwait)
		{
			LOG_LWDEBUG("LWLockAcquire", lock, "acquired, undoing queue");

			LWLockDequeueSelf(lock);
			break;
		}

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
		LOG_LWDEBUG("LWLockAcquire", lock, "waiting");

#ifdef LWLOCK_STATS
		lwstats->block_count++;
#endif

		TRACE_POSTGRESQL_LWLOCK_WAIT_START(T_NAME(lock), T_ID(lock), mode);

		for (;;)
		{
			PGSemaphoreLock(&proc->sem);
			if (!proc->lwWaiting)
				break;
			extraWaits++;
		}

		/* Retrying, allow LWLockRelease to release waiters again. */
		pg_atomic_fetch_or_u32(&lock->state, LW_FLAG_RELEASE_OK);

#ifdef LOCK_DEBUG
		{
			/* not waiting anymore */
			uint32 nwaiters PG_USED_FOR_ASSERTS_ONLY = pg_atomic_fetch_sub_u32(&lock->nwaiters, 1);

			Assert(nwaiters < MAX_BACKENDS);
		}
#endif

		TRACE_POSTGRESQL_LWLOCK_WAIT_DONE(T_NAME(lock), T_ID(lock), mode);

		LOG_LWDEBUG("LWLockAcquire", lock, "awakened");

		/* Now loop back and try to acquire lock again. */
		result = false;
	}

	TRACE_POSTGRESQL_LWLOCK_ACQUIRE(T_NAME(lock), T_ID(lock), mode);

	/* Add lock to list of locks held by this backend */
	held_lwlocks[num_held_lwlocks].lock = lock;
	held_lwlocks[num_held_lwlocks++].mode = mode;

	/*
	 * Fix the process wait semaphore's count for any absorbed wakeups.
	 */
	while (extraWaits-- > 0)
		PGSemaphoreUnlock(&proc->sem);

	return result;
}

/*
 * LWLockConditionalAcquire - acquire a lightweight lock in the specified mode
 *
 * If the lock is not available, return FALSE with no side-effects.
 *
 * If successful, cancel/die interrupts are held off until lock release.
 */
bool
LWLockConditionalAcquire(LWLock *lock, LWLockMode mode)
{
	bool		mustwait;

	AssertArg(mode == LW_SHARED || mode == LW_EXCLUSIVE);

	PRINT_LWDEBUG("LWLockConditionalAcquire", lock, mode);

	/* Ensure we will have room to remember the lock */
	if (num_held_lwlocks >= MAX_SIMUL_LWLOCKS)
		elog(ERROR, "too many LWLocks taken");

	/*
	 * Lock out cancel/die interrupts until we exit the code section protected
	 * by the LWLock.  This ensures that interrupts will not interfere with
	 * manipulations of data structures in shared memory.
	 */
	HOLD_INTERRUPTS();

	/* Check for the lock */
	mustwait = LWLockAttemptLock(lock, mode);

	if (mustwait)
	{
		/* Failed to get lock, so release interrupt holdoff */
		RESUME_INTERRUPTS();

		LOG_LWDEBUG("LWLockConditionalAcquire", lock, "failed");
		TRACE_POSTGRESQL_LWLOCK_CONDACQUIRE_FAIL(T_NAME(lock), T_ID(lock), mode);
	}
	else
	{
		/* Add lock to list of locks held by this backend */
		held_lwlocks[num_held_lwlocks].lock = lock;
		held_lwlocks[num_held_lwlocks++].mode = mode;
		TRACE_POSTGRESQL_LWLOCK_CONDACQUIRE(T_NAME(lock), T_ID(lock), mode);
	}
	return !mustwait;
}

/*
 * LWLockAcquireOrWait - Acquire lock, or wait until it's free
 *
 * The semantics of this function are a bit funky.  If the lock is currently
 * free, it is acquired in the given mode, and the function returns true.  If
 * the lock isn't immediately free, the function waits until it is released
 * and returns false, but does not acquire the lock.
 *
 * This is currently used for WALWriteLock: when a backend flushes the WAL,
 * holding WALWriteLock, it can flush the commit records of many other
 * backends as a side-effect.  Those other backends need to wait until the
 * flush finishes, but don't need to acquire the lock anymore.  They can just
 * wake up, observe that their records have already been flushed, and return.
 */
bool
LWLockAcquireOrWait(LWLock *lock, LWLockMode mode)
{
	PGPROC	   *proc = MyProc;
	bool		mustwait;
	int			extraWaits = 0;
#ifdef LWLOCK_STATS
	lwlock_stats *lwstats;

	lwstats = get_lwlock_stats_entry(lock);
#endif

	Assert(mode == LW_SHARED || mode == LW_EXCLUSIVE);

	PRINT_LWDEBUG("LWLockAcquireOrWait", lock, mode);

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
	 * NB: We're using nearly the same twice-in-a-row lock acquisition
	 * protocol as LWLockAcquire(). Check its comments for details.
	 */
	mustwait = LWLockAttemptLock(lock, mode);

	if (mustwait)
	{
		LWLockQueueSelf(lock, LW_WAIT_UNTIL_FREE);

		mustwait = LWLockAttemptLock(lock, mode);

		if (mustwait)
		{
			/*
			 * Wait until awakened.  Like in LWLockAcquire, be prepared for
			 * bogus wakeups, because we share the semaphore with
			 * ProcWaitForSignal.
			 */
			LOG_LWDEBUG("LWLockAcquireOrWait", lock, "waiting");

#ifdef LWLOCK_STATS
			lwstats->block_count++;
#endif
			TRACE_POSTGRESQL_LWLOCK_WAIT_START(T_NAME(lock), T_ID(lock), mode);

			for (;;)
			{
				PGSemaphoreLock(&proc->sem);
				if (!proc->lwWaiting)
					break;
				extraWaits++;
			}

#ifdef LOCK_DEBUG
			{
				/* not waiting anymore */
				uint32 nwaiters PG_USED_FOR_ASSERTS_ONLY = pg_atomic_fetch_sub_u32(&lock->nwaiters, 1);

				Assert(nwaiters < MAX_BACKENDS);
			}
#endif
			TRACE_POSTGRESQL_LWLOCK_WAIT_DONE(T_NAME(lock), T_ID(lock), mode);

			LOG_LWDEBUG("LWLockAcquireOrWait", lock, "awakened");
		}
		else
		{
			LOG_LWDEBUG("LWLockAcquireOrWait", lock, "acquired, undoing queue");

			/*
			 * Got lock in the second attempt, undo queueing. We need to treat
			 * this as having successfully acquired the lock, otherwise we'd
			 * not necessarily wake up people we've prevented from acquiring
			 * the lock.
			 */
			LWLockDequeueSelf(lock);
		}
	}

	/*
	 * Fix the process wait semaphore's count for any absorbed wakeups.
	 */
	while (extraWaits-- > 0)
		PGSemaphoreUnlock(&proc->sem);

	if (mustwait)
	{
		/* Failed to get lock, so release interrupt holdoff */
		RESUME_INTERRUPTS();
		LOG_LWDEBUG("LWLockAcquireOrWait", lock, "failed");
		TRACE_POSTGRESQL_LWLOCK_ACQUIRE_OR_WAIT_FAIL(T_NAME(lock), T_ID(lock),
													 mode);
	}
	else
	{
		LOG_LWDEBUG("LWLockAcquireOrWait", lock, "succeeded");
		/* Add lock to list of locks held by this backend */
		held_lwlocks[num_held_lwlocks].lock = lock;
		held_lwlocks[num_held_lwlocks++].mode = mode;
		TRACE_POSTGRESQL_LWLOCK_ACQUIRE_OR_WAIT(T_NAME(lock), T_ID(lock), mode);
	}

	return !mustwait;
}

/*
 * Does the lwlock in its current state need to wait for the variable value to
 * change?
 *
 * If we don't need to wait, and it's because the value of the variable has
 * changed, store the current value in newval.
 *
 * *result is set to true if the lock was free, and false otherwise.
 */
static bool
LWLockConflictsWithVar(LWLock *lock,
					   uint64 *valptr, uint64 oldval, uint64 *newval,
					   bool *result)
{
	bool		mustwait;
	uint64		value;
#ifdef LWLOCK_STATS
	lwlock_stats *lwstats;

	lwstats = get_lwlock_stats_entry(lock);
#endif

	/*
	 * Test first to see if it the slot is free right now.
	 *
	 * XXX: the caller uses a spinlock before this, so we don't need a memory
	 * barrier here as far as the current usage is concerned.  But that might
	 * not be safe in general.
	 */
	mustwait = (pg_atomic_read_u32(&lock->state) & LW_VAL_EXCLUSIVE) != 0;

	if (!mustwait)
	{
		*result = true;
		return false;
	}

	*result = false;

	/*
	 * Read value using spinlock as we can't rely on atomic 64 bit
	 * reads/stores.  TODO: On platforms with a way to do atomic 64 bit
	 * reads/writes the spinlock could be optimized away.
	 */
#ifdef LWLOCK_STATS
	lwstats->spin_delay_count += SpinLockAcquire(&lock->mutex);
#else
	SpinLockAcquire(&lock->mutex);
#endif
	value = *valptr;
	SpinLockRelease(&lock->mutex);

	if (value != oldval)
	{
		mustwait = false;
		*newval = value;
	}
	else
	{
		mustwait = true;
	}

	return mustwait;
}

/*
 * LWLockWaitForVar - Wait until lock is free, or a variable is updated.
 *
 * If the lock is held and *valptr equals oldval, waits until the lock is
 * either freed, or the lock holder updates *valptr by calling
 * LWLockUpdateVar.  If the lock is free on exit (immediately or after
 * waiting), returns true.  If the lock is still held, but *valptr no longer
 * matches oldval, returns false and sets *newval to the current value in
 * *valptr.
 *
 * Note: this function ignores shared lock holders; if the lock is held
 * in shared mode, returns 'true'.
 */
bool
LWLockWaitForVar(LWLock *lock, uint64 *valptr, uint64 oldval, uint64 *newval)
{
	PGPROC	   *proc = MyProc;
	int			extraWaits = 0;
	bool		result = false;
#ifdef LWLOCK_STATS
	lwlock_stats *lwstats;

	lwstats = get_lwlock_stats_entry(lock);
#endif

	PRINT_LWDEBUG("LWLockWaitForVar", lock, LW_WAIT_UNTIL_FREE);

	/*
	 * Lock out cancel/die interrupts while we sleep on the lock.  There is no
	 * cleanup mechanism to remove us from the wait queue if we got
	 * interrupted.
	 */
	HOLD_INTERRUPTS();

	/*
	 * Loop here to check the lock's status after each time we are signaled.
	 */
	for (;;)
	{
		bool		mustwait;

		mustwait = LWLockConflictsWithVar(lock, valptr, oldval, newval,
										  &result);

		if (!mustwait)
			break;				/* the lock was free or value didn't match */

		/*
		 * Add myself to wait queue. Note that this is racy, somebody else
		 * could wakeup before we're finished queuing. NB: We're using nearly
		 * the same twice-in-a-row lock acquisition protocol as
		 * LWLockAcquire(). Check its comments for details. The only
		 * difference is that we also have to check the variable's values when
		 * checking the state of the lock.
		 */
		LWLockQueueSelf(lock, LW_WAIT_UNTIL_FREE);

		/*
		 * Set RELEASE_OK flag, to make sure we get woken up as soon as the
		 * lock is released.
		 */
		pg_atomic_fetch_or_u32(&lock->state, LW_FLAG_RELEASE_OK);

		/*
		 * We're now guaranteed to be woken up if necessary. Recheck the lock
		 * and variables state.
		 */
		mustwait = LWLockConflictsWithVar(lock, valptr, oldval, newval,
										  &result);

		/* Ok, no conflict after we queued ourselves. Undo queueing. */
		if (!mustwait)
		{
			LOG_LWDEBUG("LWLockWaitForVar", lock, "free, undoing queue");

			LWLockDequeueSelf(lock);
			break;
		}

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
		LOG_LWDEBUG("LWLockWaitForVar", lock, "waiting");

#ifdef LWLOCK_STATS
		lwstats->block_count++;
#endif

		TRACE_POSTGRESQL_LWLOCK_WAIT_START(T_NAME(lock), T_ID(lock),
										   LW_EXCLUSIVE);

		for (;;)
		{
			PGSemaphoreLock(&proc->sem);
			if (!proc->lwWaiting)
				break;
			extraWaits++;
		}

#ifdef LOCK_DEBUG
		{
			/* not waiting anymore */
			uint32 nwaiters PG_USED_FOR_ASSERTS_ONLY = pg_atomic_fetch_sub_u32(&lock->nwaiters, 1);

			Assert(nwaiters < MAX_BACKENDS);
		}
#endif

		TRACE_POSTGRESQL_LWLOCK_WAIT_DONE(T_NAME(lock), T_ID(lock),
										  LW_EXCLUSIVE);

		LOG_LWDEBUG("LWLockWaitForVar", lock, "awakened");

		/* Now loop back and check the status of the lock again. */
	}

	TRACE_POSTGRESQL_LWLOCK_ACQUIRE(T_NAME(lock), T_ID(lock), LW_EXCLUSIVE);

	/*
	 * Fix the process wait semaphore's count for any absorbed wakeups.
	 */
	while (extraWaits-- > 0)
		PGSemaphoreUnlock(&proc->sem);

	/*
	 * Now okay to allow cancel/die interrupts.
	 */
	RESUME_INTERRUPTS();

	return result;
}


/*
 * LWLockUpdateVar - Update a variable and wake up waiters atomically
 *
 * Sets *valptr to 'val', and wakes up all processes waiting for us with
 * LWLockWaitForVar().  Setting the value and waking up the processes happen
 * atomically so that any process calling LWLockWaitForVar() on the same lock
 * is guaranteed to see the new value, and act accordingly.
 *
 * The caller must be holding the lock in exclusive mode.
 */
void
LWLockUpdateVar(LWLock *lock, uint64 *valptr, uint64 val)
{
	dlist_head	wakeup;
	dlist_mutable_iter iter;
#ifdef LWLOCK_STATS
	lwlock_stats *lwstats;

	lwstats = get_lwlock_stats_entry(lock);
#endif

	PRINT_LWDEBUG("LWLockUpdateVar", lock, LW_EXCLUSIVE);

	dlist_init(&wakeup);

	/* Acquire mutex.  Time spent holding mutex should be short! */
#ifdef LWLOCK_STATS
	lwstats->spin_delay_count += SpinLockAcquire(&lock->mutex);
#else
	SpinLockAcquire(&lock->mutex);
#endif

	Assert(pg_atomic_read_u32(&lock->state) & LW_VAL_EXCLUSIVE);

	/* Update the lock's value */
	*valptr = val;

	/*
	 * See if there are any LW_WAIT_UNTIL_FREE waiters that need to be woken
	 * up. They are always in the front of the queue.
	 */
	dlist_foreach_modify(iter, &lock->waiters)
	{
		PGPROC	   *waiter = dlist_container(PGPROC, lwWaitLink, iter.cur);

		if (waiter->lwWaitMode != LW_WAIT_UNTIL_FREE)
			break;

		dlist_delete(&waiter->lwWaitLink);
		dlist_push_tail(&wakeup, &waiter->lwWaitLink);
	}

	/* We are done updating shared state of the lock itself. */
	SpinLockRelease(&lock->mutex);

	/*
	 * Awaken any waiters I removed from the queue.
	 */
	dlist_foreach_modify(iter, &wakeup)
	{
		PGPROC	   *waiter = dlist_container(PGPROC, lwWaitLink, iter.cur);

		dlist_delete(&waiter->lwWaitLink);
		/* check comment in LWLockWakeup() about this barrier */
		pg_write_barrier();
		waiter->lwWaiting = false;
		PGSemaphoreUnlock(&waiter->sem);
	}
}


/*
 * LWLockRelease - release a previously acquired lock
 */
void
LWLockRelease(LWLock *lock)
{
	LWLockMode	mode;
	uint32		oldstate;
	bool		check_waiters;
	int			i;

	/*
	 * Remove lock from list of locks held.  Usually, but not always, it will
	 * be the latest-acquired lock; so search array backwards.
	 */
	for (i = num_held_lwlocks; --i >= 0;)
	{
		if (lock == held_lwlocks[i].lock)
		{
			mode = held_lwlocks[i].mode;
			break;
		}
	}
	if (i < 0)
		elog(ERROR, "lock %s %d is not held", T_NAME(lock), T_ID(lock));
	num_held_lwlocks--;
	for (; i < num_held_lwlocks; i++)
		held_lwlocks[i] = held_lwlocks[i + 1];

	PRINT_LWDEBUG("LWLockRelease", lock, mode);

	/*
	 * Release my hold on lock, after that it can immediately be acquired by
	 * others, even if we still have to wakeup other waiters.
	 */
	if (mode == LW_EXCLUSIVE)
		oldstate = pg_atomic_sub_fetch_u32(&lock->state, LW_VAL_EXCLUSIVE);
	else
		oldstate = pg_atomic_sub_fetch_u32(&lock->state, LW_VAL_SHARED);

	/* nobody else can have that kind of lock */
	Assert(!(oldstate & LW_VAL_EXCLUSIVE));


	/*
	 * We're still waiting for backends to get scheduled, don't wake them up
	 * again.
	 */
	if ((oldstate & (LW_FLAG_HAS_WAITERS | LW_FLAG_RELEASE_OK)) ==
		(LW_FLAG_HAS_WAITERS | LW_FLAG_RELEASE_OK) &&
		(oldstate & LW_LOCK_MASK) == 0)
		check_waiters = true;
	else
		check_waiters = false;

	/*
	 * As waking up waiters requires the spinlock to be acquired, only do so
	 * if necessary.
	 */
	if (check_waiters)
	{
		/* XXX: remove before commit? */
		LOG_LWDEBUG("LWLockRelease", lock, "releasing waiters");
		LWLockWakeup(lock);
	}

	TRACE_POSTGRESQL_LWLOCK_RELEASE(T_NAME(lock), T_ID(lock));

	/*
	 * Now okay to allow cancel/die interrupts.
	 */
	RESUME_INTERRUPTS();
}

/*
 * LWLockReleaseClearVar - release a previously acquired lock, reset variable
 */
void
LWLockReleaseClearVar(LWLock *lock, uint64 *valptr, uint64 val)
{
#ifdef LWLOCK_STATS
	lwlock_stats *lwstats;

	lwstats = get_lwlock_stats_entry(lock);
	lwstats->spin_delay_count += SpinLockAcquire(&lock->mutex);
#else
	SpinLockAcquire(&lock->mutex);
#endif
	/*
	 * Set the variable's value before releasing the lock, that prevents race
	 * a race condition wherein a new locker acquires the lock, but hasn't yet
	 * set the variables value.
	 */
	*valptr = val;
	SpinLockRelease(&lock->mutex);

	LWLockRelease(lock);
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

		LWLockRelease(held_lwlocks[num_held_lwlocks - 1].lock);
	}
}


/*
 * LWLockHeldByMe - test whether my process currently holds a lock
 *
 * This is meant as debug support only.  We currently do not distinguish
 * whether the lock is held shared or exclusive.
 */
bool
LWLockHeldByMe(LWLock *l)
{
	int			i;

	for (i = 0; i < num_held_lwlocks; i++)
	{
		if (held_lwlocks[i].lock == l)
			return true;
	}
	return false;
}
