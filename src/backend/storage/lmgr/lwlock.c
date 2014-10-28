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
 * In addition to exclusive and shared modes, lightweight locks can be used
 * to wait until a variable changes value.  The variable is initially set
 * when the lock is acquired with LWLockAcquireWithVar, and can be updated
 * without releasing the lock by calling LWLockUpdateVar.  LWLockWaitForVar
 * waits for the variable to be updated, or until the lock is free.  The
 * meaning of the variable is up to the caller, the lightweight lock code
 * just assigns and compares it.
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/lmgr/lwlock.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/clog.h"
#include "access/multixact.h"
#include "access/subtrans.h"
#include "commands/async.h"
#include "miscadmin.h"
#include "pg_trace.h"
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

static int	num_held_lwlocks = 0;
static LWLock *held_lwlocks[MAX_SIMUL_LWLOCKS];

static int	lock_addin_request = 0;
static bool lock_addin_request_allowed = true;

static inline bool LWLockAcquireCommon(LWLock *l, LWLockMode mode,
					uint64 *valptr, uint64 val);

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
	int			spin_delay_count;
}	lwlock_stats;

static HTAB *lwlock_stats_htab;
static lwlock_stats lwlock_stats_dummy;
#endif

#ifdef LOCK_DEBUG
bool		Trace_lwlocks = false;

inline static void
PRINT_LWDEBUG(const char *where, const LWLock *lock)
{
	if (Trace_lwlocks)
		elog(LOG, "%s(%s %d): excl %d shared %d head %p rOK %d",
			 where, T_NAME(lock), T_ID(lock),
			 (int) lock->exclusive, lock->shared, lock->head,
			 (int) lock->releaseOK);
}

inline static void
LOG_LWDEBUG(const char *where, const char *name, int index, const char *msg)
{
	if (Trace_lwlocks)
		elog(LOG, "%s(%s %d): %s", where, name, index, msg);
}
#else							/* not LOCK_DEBUG */
#define PRINT_LWDEBUG(a,b)
#define LOG_LWDEBUG(a,b,c,d)
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
	ctl.hash = tag_hash;
	ctl.hcxt = lwlock_stats_cxt;
	lwlock_stats_htab = hash_create("lwlock stats", 16384, &ctl,
									HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);
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
			  "PID %d lwlock %s %d: shacq %u exacq %u blk %u spindelay %u\n",
				MyProcPid, LWLockTrancheArray[lwstats->key.tranche]->name,
				lwstats->key.instance, lwstats->sh_acquire_count,
				lwstats->ex_acquire_count, lwstats->block_count,
				lwstats->spin_delay_count);
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
	 * Possibly this logic should be spread out among the affected modules,
	 * the same way that shmem space estimation is done.  But for now, there
	 * are few enough users of LWLocks that we can get away with just keeping
	 * the knowledge here.
	 */

	/* Predefined LWLocks */
	numLocks = NUM_FIXED_LWLOCKS;

	/* bufmgr.c needs two for each shared buffer */
	numLocks += 2 * NBuffers;

	/* proc.c needs one for each backend or auxiliary process */
	numLocks += MaxBackends + NUM_AUXILIARY_PROCS;

	/* clog.c needs one per CLOG buffer */
	numLocks += CLOGShmemBuffers();

	/* subtrans.c needs one per SubTrans buffer */
	numLocks += NUM_SUBTRANS_BUFFERS;

	/* multixact.c needs two SLRU areas */
	numLocks += NUM_MXACTOFFSET_BUFFERS + NUM_MXACTMEMBER_BUFFERS;

	/* async.c needs one per Async buffer */
	numLocks += NUM_ASYNC_BUFFERS;

	/* predicate.c needs one per old serializable xid buffer */
	numLocks += NUM_OLDSERXID_BUFFERS;

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
			LWLockInitialize(&lock->lock, 0);

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
		LWLockCounter[2] = 1;	/* 0 is the main array */
	}

	if (LWLockTrancheArray == NULL)
	{
		LWLockTranchesAllocated = 16;
		LWLockTrancheArray = (LWLockTranche **)
			MemoryContextAlloc(TopMemoryContext,
						  LWLockTranchesAllocated * sizeof(LWLockTranche *));
	}

	MainLWLockTranche.name = "main";
	MainLWLockTranche.array_base = MainLWLockArray;
	MainLWLockTranche.array_stride = sizeof(LWLockPadded);
	LWLockRegisterTranche(0, &MainLWLockTranche);
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
	lock->releaseOK = true;
	lock->exclusive = 0;
	lock->shared = 0;
	lock->tranche = tranche_id;
	lock->head = NULL;
	lock->tail = NULL;
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
LWLockAcquire(LWLock *l, LWLockMode mode)
{
	return LWLockAcquireCommon(l, mode, NULL, 0);
}

/*
 * LWLockAcquireWithVar - like LWLockAcquire, but also sets *valptr = val
 *
 * The lock is always acquired in exclusive mode with this function.
 */
bool
LWLockAcquireWithVar(LWLock *l, uint64 *valptr, uint64 val)
{
	return LWLockAcquireCommon(l, LW_EXCLUSIVE, valptr, val);
}

/* internal function to implement LWLockAcquire and LWLockAcquireWithVar */
static inline bool
LWLockAcquireCommon(LWLock *lock, LWLockMode mode, uint64 *valptr, uint64 val)
{
	PGPROC	   *proc = MyProc;
	bool		retry = false;
	bool		result = true;
	int			extraWaits = 0;
#ifdef LWLOCK_STATS
	lwlock_stats *lwstats;
#endif

	PRINT_LWDEBUG("LWLockAcquire", lock);

#ifdef LWLOCK_STATS
	lwstats = get_lwlock_stats_entry(lock);

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

		/* Acquire mutex.  Time spent holding mutex should be short! */
#ifdef LWLOCK_STATS
		lwstats->spin_delay_count += SpinLockAcquire(&lock->mutex);
#else
		SpinLockAcquire(&lock->mutex);
#endif

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
		proc->lwWaitMode = mode;
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
		LOG_LWDEBUG("LWLockAcquire", T_NAME(lock), T_ID(lock), "waiting");

#ifdef LWLOCK_STATS
		lwstats->block_count++;
#endif

		TRACE_POSTGRESQL_LWLOCK_WAIT_START(T_NAME(lock), T_ID(lock), mode);

		for (;;)
		{
			/* "false" means cannot accept cancel/die interrupt here. */
			PGSemaphoreLock(&proc->sem, false);
			if (!proc->lwWaiting)
				break;
			extraWaits++;
		}

		TRACE_POSTGRESQL_LWLOCK_WAIT_DONE(T_NAME(lock), T_ID(lock), mode);

		LOG_LWDEBUG("LWLockAcquire", T_NAME(lock), T_ID(lock), "awakened");

		/* Now loop back and try to acquire lock again. */
		retry = true;
		result = false;
	}

	/* If there's a variable associated with this lock, initialize it */
	if (valptr)
		*valptr = val;

	/* We are done updating shared state of the lock itself. */
	SpinLockRelease(&lock->mutex);

	TRACE_POSTGRESQL_LWLOCK_ACQUIRE(T_NAME(lock), T_ID(lock), mode);

	/* Add lock to list of locks held by this backend */
	held_lwlocks[num_held_lwlocks++] = lock;

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

	PRINT_LWDEBUG("LWLockConditionalAcquire", lock);

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
		LOG_LWDEBUG("LWLockConditionalAcquire",
					T_NAME(lock), T_ID(lock), "failed");
		TRACE_POSTGRESQL_LWLOCK_CONDACQUIRE_FAIL(T_NAME(lock),
												 T_ID(lock), mode);
	}
	else
	{
		/* Add lock to list of locks held by this backend */
		held_lwlocks[num_held_lwlocks++] = lock;
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
#endif

	PRINT_LWDEBUG("LWLockAcquireOrWait", lock);

#ifdef LWLOCK_STATS
	lwstats = get_lwlock_stats_entry(lock);
#endif

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

	if (mustwait)
	{
		/*
		 * Add myself to wait queue.
		 *
		 * If we don't have a PGPROC structure, there's no way to wait.  This
		 * should never occur, since MyProc should only be null during shared
		 * memory initialization.
		 */
		if (proc == NULL)
			elog(PANIC, "cannot wait without a PGPROC structure");

		proc->lwWaiting = true;
		proc->lwWaitMode = LW_WAIT_UNTIL_FREE;
		proc->lwWaitLink = NULL;
		if (lock->head == NULL)
			lock->head = proc;
		else
			lock->tail->lwWaitLink = proc;
		lock->tail = proc;

		/* Can release the mutex now */
		SpinLockRelease(&lock->mutex);

		/*
		 * Wait until awakened.  Like in LWLockAcquire, be prepared for bogus
		 * wakups, because we share the semaphore with ProcWaitForSignal.
		 */
		LOG_LWDEBUG("LWLockAcquireOrWait", T_NAME(lock), T_ID(lock),
					"waiting");

#ifdef LWLOCK_STATS
		lwstats->block_count++;
#endif

		TRACE_POSTGRESQL_LWLOCK_WAIT_START(T_NAME(lock), T_ID(lock), mode);

		for (;;)
		{
			/* "false" means cannot accept cancel/die interrupt here. */
			PGSemaphoreLock(&proc->sem, false);
			if (!proc->lwWaiting)
				break;
			extraWaits++;
		}

		TRACE_POSTGRESQL_LWLOCK_WAIT_DONE(T_NAME(lock), T_ID(lock), mode);

		LOG_LWDEBUG("LWLockAcquireOrWait", T_NAME(lock), T_ID(lock),
					"awakened");
	}
	else
	{
		/* We are done updating shared state of the lock itself. */
		SpinLockRelease(&lock->mutex);
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
		LOG_LWDEBUG("LWLockAcquireOrWait", T_NAME(lock), T_ID(lock), "failed");
		TRACE_POSTGRESQL_LWLOCK_ACQUIRE_OR_WAIT_FAIL(T_NAME(lock), T_ID(lock),
													 mode);
	}
	else
	{
		/* Add lock to list of locks held by this backend */
		held_lwlocks[num_held_lwlocks++] = lock;
		TRACE_POSTGRESQL_LWLOCK_ACQUIRE_OR_WAIT(T_NAME(lock), T_ID(lock),
												mode);
	}

	return !mustwait;
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
 * It's possible that the lock holder releases the lock, but another backend
 * acquires it again before we get a chance to observe that the lock was
 * momentarily released.  We wouldn't need to wait for the new lock holder,
 * but we cannot distinguish that case, so we will have to wait.
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
#endif

	PRINT_LWDEBUG("LWLockWaitForVar", lock);

#ifdef LWLOCK_STATS
	lwstats = get_lwlock_stats_entry(lock);
#endif   /* LWLOCK_STATS */

	/*
	 * Quick test first to see if it the slot is free right now.
	 *
	 * XXX: the caller uses a spinlock before this, so we don't need a memory
	 * barrier here as far as the current usage is concerned.  But that might
	 * not be safe in general.
	 */
	if (lock->exclusive == 0)
		return true;

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
		uint64		value;

		/* Acquire mutex.  Time spent holding mutex should be short! */
#ifdef LWLOCK_STATS
		lwstats->spin_delay_count += SpinLockAcquire(&lock->mutex);
#else
		SpinLockAcquire(&lock->mutex);
#endif

		/* Is the lock now free, and if not, does the value match? */
		if (lock->exclusive == 0)
		{
			result = true;
			mustwait = false;
		}
		else
		{
			value = *valptr;
			if (value != oldval)
			{
				result = false;
				mustwait = false;
				*newval = value;
			}
			else
				mustwait = true;
		}

		if (!mustwait)
			break;				/* the lock was free or value didn't match */

		/*
		 * Add myself to wait queue.
		 */
		proc->lwWaiting = true;
		proc->lwWaitMode = LW_WAIT_UNTIL_FREE;
		/* waiters are added to the front of the queue */
		proc->lwWaitLink = lock->head;
		if (lock->head == NULL)
			lock->tail = proc;
		lock->head = proc;

		/*
		 * Set releaseOK, to make sure we get woken up as soon as the lock is
		 * released.
		 */
		lock->releaseOK = true;

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
		LOG_LWDEBUG("LWLockWaitForVar", T_NAME(lock), T_ID(lock), "waiting");

#ifdef LWLOCK_STATS
		lwstats->block_count++;
#endif

		TRACE_POSTGRESQL_LWLOCK_WAIT_START(T_NAME(lock), T_ID(lock),
										   LW_EXCLUSIVE);

		for (;;)
		{
			/* "false" means cannot accept cancel/die interrupt here. */
			PGSemaphoreLock(&proc->sem, false);
			if (!proc->lwWaiting)
				break;
			extraWaits++;
		}

		TRACE_POSTGRESQL_LWLOCK_WAIT_DONE(T_NAME(lock), T_ID(lock),
										  LW_EXCLUSIVE);

		LOG_LWDEBUG("LWLockWaitForVar", T_NAME(lock), T_ID(lock), "awakened");

		/* Now loop back and check the status of the lock again. */
	}

	/* We are done updating shared state of the lock itself. */
	SpinLockRelease(&lock->mutex);

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
	PGPROC	   *head;
	PGPROC	   *proc;
	PGPROC	   *next;

	/* Acquire mutex.  Time spent holding mutex should be short! */
	SpinLockAcquire(&lock->mutex);

	/* we should hold the lock */
	Assert(lock->exclusive == 1);

	/* Update the lock's value */
	*valptr = val;

	/*
	 * See if there are any LW_WAIT_UNTIL_FREE waiters that need to be woken
	 * up. They are always in the front of the queue.
	 */
	head = lock->head;

	if (head != NULL && head->lwWaitMode == LW_WAIT_UNTIL_FREE)
	{
		proc = head;
		next = proc->lwWaitLink;
		while (next && next->lwWaitMode == LW_WAIT_UNTIL_FREE)
		{
			proc = next;
			next = next->lwWaitLink;
		}

		/* proc is now the last PGPROC to be released */
		lock->head = next;
		proc->lwWaitLink = NULL;
	}
	else
		head = NULL;

	/* We are done updating shared state of the lock itself. */
	SpinLockRelease(&lock->mutex);

	/*
	 * Awaken any waiters I removed from the queue.
	 */
	while (head != NULL)
	{
		proc = head;
		head = proc->lwWaitLink;
		proc->lwWaitLink = NULL;
		proc->lwWaiting = false;
		PGSemaphoreUnlock(&proc->sem);
	}
}


/*
 * LWLockRelease - release a previously acquired lock
 */
void
LWLockRelease(LWLock *lock)
{
	PGPROC	   *head;
	PGPROC	   *proc;
	int			i;

	PRINT_LWDEBUG("LWLockRelease", lock);

	/*
	 * Remove lock from list of locks held.  Usually, but not always, it will
	 * be the latest-acquired lock; so search array backwards.
	 */
	for (i = num_held_lwlocks; --i >= 0;)
	{
		if (lock == held_lwlocks[i])
			break;
	}
	if (i < 0)
		elog(ERROR, "lock %s %d is not held", T_NAME(lock), T_ID(lock));
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
			 * Remove the to-be-awakened PGPROCs from the queue.
			 */
			bool		releaseOK = true;

			proc = head;

			/*
			 * First wake up any backends that want to be woken up without
			 * acquiring the lock.
			 */
			while (proc->lwWaitMode == LW_WAIT_UNTIL_FREE && proc->lwWaitLink)
				proc = proc->lwWaitLink;

			/*
			 * If the front waiter wants exclusive lock, awaken him only.
			 * Otherwise awaken as many waiters as want shared access.
			 */
			if (proc->lwWaitMode != LW_EXCLUSIVE)
			{
				while (proc->lwWaitLink != NULL &&
					   proc->lwWaitLink->lwWaitMode != LW_EXCLUSIVE)
				{
					if (proc->lwWaitMode != LW_WAIT_UNTIL_FREE)
						releaseOK = false;
					proc = proc->lwWaitLink;
				}
			}
			/* proc is now the last PGPROC to be released */
			lock->head = proc->lwWaitLink;
			proc->lwWaitLink = NULL;

			/*
			 * Prevent additional wakeups until retryer gets to run. Backends
			 * that are just waiting for the lock to become free don't retry
			 * automatically.
			 */
			if (proc->lwWaitMode != LW_WAIT_UNTIL_FREE)
				releaseOK = false;

			lock->releaseOK = releaseOK;
		}
		else
		{
			/* lock is still held, can't awaken anything */
			head = NULL;
		}
	}

	/* We are done updating shared state of the lock itself. */
	SpinLockRelease(&lock->mutex);

	TRACE_POSTGRESQL_LWLOCK_RELEASE(T_NAME(lock), T_ID(lock));

	/*
	 * Awaken any waiters I removed from the queue.
	 */
	while (head != NULL)
	{
		LOG_LWDEBUG("LWLockRelease", T_NAME(lock), T_ID(lock),
					"release waiter");
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
LWLockHeldByMe(LWLock *l)
{
	int			i;

	for (i = 0; i < num_held_lwlocks; i++)
	{
		if (held_lwlocks[i] == l)
			return true;
	}
	return false;
}
