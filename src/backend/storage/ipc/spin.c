/*-------------------------------------------------------------------------
 *
 * spin.c
 *	  routines for managing spin locks
 *
 * POSTGRES has two kinds of locks: semaphores (which put the
 * process to sleep) and spinlocks (which are supposed to be
 * short term locks).  Spinlocks are implemented via test-and-set (TAS)
 * instructions if possible, else via semaphores.  The semaphore method
 * is too slow to be useful :-(
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/ipc/Attic/spin.c,v 1.32 2001/03/22 03:59:45 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>
#if !defined(HAS_TEST_AND_SET) && defined(HAVE_SYS_SEM_H)
#include <sys/sem.h>
#endif

#include "miscadmin.h"
#include "storage/proc.h"
#include "storage/s_lock.h"


/* Probably should move these to an appropriate header file */
extern SPINLOCK ShmemLock;
extern SPINLOCK ShmemIndexLock;
extern SPINLOCK BufMgrLock;
extern SPINLOCK LockMgrLock;
extern SPINLOCK ProcStructLock;
extern SPINLOCK SInvalLock;
extern SPINLOCK OidGenLockId;
extern SPINLOCK XidGenLockId;
extern SPINLOCK ControlFileLockId;

#ifdef STABLE_MEMORY_STORAGE
extern SPINLOCK MMCacheLock;

#endif


/*
 * Initialize identifiers for permanent spinlocks during startup
 *
 * The same identifiers are used for both TAS and semaphore implementations,
 * although in one case they are indexes into a shmem array and in the other
 * they are semaphore numbers.
 */
static void
InitSpinLockIDs(void)
{
	ShmemLock = (SPINLOCK) SHMEMLOCKID;
	ShmemIndexLock = (SPINLOCK) SHMEMINDEXLOCKID;
	BufMgrLock = (SPINLOCK) BUFMGRLOCKID;
	LockMgrLock = (SPINLOCK) LOCKMGRLOCKID;
	ProcStructLock = (SPINLOCK) PROCSTRUCTLOCKID;
	SInvalLock = (SPINLOCK) SINVALLOCKID;
	OidGenLockId = (SPINLOCK) OIDGENLOCKID;
	XidGenLockId = (SPINLOCK) XIDGENLOCKID;
	ControlFileLockId = (SPINLOCK) CNTLFILELOCKID;

#ifdef STABLE_MEMORY_STORAGE
	MMCacheLock = (SPINLOCK) MMCACHELOCKID;
#endif
}


#ifdef HAS_TEST_AND_SET

/* real spin lock implementation */

typedef struct slock
{
	slock_t		shlock;
} SLock;

#ifdef LOCK_DEBUG
bool		Trace_spinlocks = false;

inline static void
PRINT_SLDEBUG(const char *where, SPINLOCK lockid, const SLock *lock)
{
	if (Trace_spinlocks)
		elog(DEBUG, "%s: id=%d", where, lockid);
}

#else							/* not LOCK_DEBUG */
#define PRINT_SLDEBUG(a,b,c)
#endif	 /* not LOCK_DEBUG */


static SLock *SLockArray = NULL;

#define SLOCKMEMORYSIZE		((int) MAX_SPINS * sizeof(SLock))

/*
 * SLockShmemSize --- return shared-memory space needed
 */
int
SLockShmemSize(void)
{
	return MAXALIGN(SLOCKMEMORYSIZE);
}

/*
 * CreateSpinlocks --- create and initialize spinlocks during startup
 */
void
CreateSpinlocks(PGShmemHeader *seghdr)
{
	int			id;

	/*
	 * We must allocate the space "by hand" because shmem.c isn't up yet
	 */
	SLockArray = (SLock *) (((char *) seghdr) + seghdr->freeoffset);
	seghdr->freeoffset += MAXALIGN(SLOCKMEMORYSIZE);
	Assert(seghdr->freeoffset <= seghdr->totalsize);

	/*
	 * Initialize all spinlocks to "unlocked" state
	 */
	for (id = 0; id < (int) MAX_SPINS; id++)
	{
		SLock	   *slckP = &(SLockArray[id]);

		S_INIT_LOCK(&(slckP->shlock));
	}

	/*
	 * Assign indexes for fixed spinlocks
	 */
	InitSpinLockIDs();
}

void
SpinAcquire(SPINLOCK lockid)
{
	SLock	   *slckP = &(SLockArray[lockid]);

	PRINT_SLDEBUG("SpinAcquire", lockid, slckP);

	/*
	 * Acquire the lock, then record that we have done so (for recovery in
	 * case of elog(ERROR) while holding the lock).  Note we assume here
	 * that S_LOCK will not accept cancel/die interrupts once it has
	 * acquired the lock.  However, interrupts should be accepted while
	 * waiting, if InterruptHoldoffCount is zero.
	 */
	S_LOCK(&(slckP->shlock));
	PROC_INCR_SLOCK(lockid);

	/*
	 * Lock out cancel/die interrupts until we exit the code section
	 * protected by the spinlock.  This ensures that interrupts will not
	 * interfere with manipulations of data structures in shared memory.
	 */
	HOLD_INTERRUPTS();

	PRINT_SLDEBUG("SpinAcquire/done", lockid, slckP);
}

void
SpinRelease(SPINLOCK lockid)
{
	SLock	   *slckP = &(SLockArray[lockid]);

	PRINT_SLDEBUG("SpinRelease", lockid, slckP);

	/*
	 * Check that we are actually holding the lock we are releasing. This
	 * can be done only after MyProc has been initialized.
	 */
	Assert(!MyProc || MyProc->sLocks[lockid] > 0);

	/*
	 * Record that we no longer hold the spinlock, and release it.
	 */
	PROC_DECR_SLOCK(lockid);
	S_UNLOCK(&(slckP->shlock));

	/*
	 * Exit the interrupt holdoff entered in SpinAcquire().
	 */
	RESUME_INTERRUPTS();

	PRINT_SLDEBUG("SpinRelease/done", lockid, slckP);
}

#else							/* !HAS_TEST_AND_SET */

/*
 * No TAS, so spinlocks are implemented using SysV semaphores.
 *
 * We support two slightly different APIs here: SpinAcquire/SpinRelease
 * work with SPINLOCK integer indexes for the permanent spinlocks, which
 * are all assumed to live in the first spinlock semaphore set.  There
 * is also an emulation of the s_lock.h TAS-spinlock macros; for that case,
 * typedef slock_t stores the semId and sem number of the sema to use.
 * The semas needed are created by CreateSpinlocks and doled out by
 * s_init_lock_sema.
 *
 * Since many systems have a rather small SEMMSL limit on semas per set,
 * we allocate the semaphores required in sets of SPINLOCKS_PER_SET semas.
 * This value is deliberately made equal to PROC_NSEMS_PER_SET so that all
 * sema sets allocated by Postgres will be the same size; that eases the
 * semaphore-recycling logic in IpcSemaphoreCreate().
 *
 * Note that the SpinLockIds array is not in shared memory; it is filled
 * by the postmaster and then inherited through fork() by backends.  This
 * is OK because its contents do not change after shmem initialization.
 */

#define SPINLOCKS_PER_SET  PROC_NSEMS_PER_SET

static IpcSemaphoreId *SpinLockIds = NULL;

static int	numSpinSets = 0;	/* number of sema sets used */
static int	numSpinLocks = 0;	/* total number of semas allocated */
static int	nextSpinLock = 0;	/* next free spinlock index */

static void SpinFreeAllSemaphores(void);

/*
 * SLockShmemSize --- return shared-memory space needed
 */
int
SLockShmemSize(void)
{
	return 0;
}

/*
 * CreateSpinlocks --- create and initialize spinlocks during startup
 */
void
CreateSpinlocks(PGShmemHeader *seghdr)
{
	int			i;

	if (SpinLockIds == NULL)
	{

		/*
		 * Compute number of spinlocks needed.	If this logic gets any
		 * more complicated, it should be distributed into the affected
		 * modules, similar to the way shmem space estimation is handled.
		 *
		 * For now, though, we just need the fixed spinlocks (MAX_SPINS), two
		 * spinlocks per shared disk buffer, and four spinlocks for XLOG.
		 */
		numSpinLocks = (int) MAX_SPINS + 2 * NBuffers + 4;

		/* might as well round up to a multiple of SPINLOCKS_PER_SET */
		numSpinSets = (numSpinLocks - 1) / SPINLOCKS_PER_SET + 1;
		numSpinLocks = numSpinSets * SPINLOCKS_PER_SET;

		SpinLockIds = (IpcSemaphoreId *)
			malloc(numSpinSets * sizeof(IpcSemaphoreId));
		Assert(SpinLockIds != NULL);
	}

	for (i = 0; i < numSpinSets; i++)
		SpinLockIds[i] = -1;

	/*
	 * Arrange to delete semas on exit --- set this up now so that we will
	 * clean up if allocation fails.  We use our own freeproc, rather than
	 * IpcSemaphoreCreate's removeOnExit option, because we don't want to
	 * fill up the on_shmem_exit list with a separate entry for each
	 * semaphore set.
	 */
	on_shmem_exit(SpinFreeAllSemaphores, 0);

	/* Create sema sets and set all semas to count 1 */
	for (i = 0; i < numSpinSets; i++)
	{
		SpinLockIds[i] = IpcSemaphoreCreate(SPINLOCKS_PER_SET,
											IPCProtection,
											1,
											false);
	}

	/*
	 * Assign indexes for fixed spinlocks
	 */
	Assert(MAX_SPINS <= SPINLOCKS_PER_SET);
	InitSpinLockIDs();

	/* Init counter for allocating dynamic spinlocks */
	nextSpinLock = MAX_SPINS;
}

/*
 * SpinFreeAllSemaphores -
 *	  called at shmem_exit time, ie when exiting the postmaster or
 *	  destroying shared state for a failed set of backends.
 *	  Free up all the semaphores allocated for spinlocks.
 */
static void
SpinFreeAllSemaphores(void)
{
	int			i;

	for (i = 0; i < numSpinSets; i++)
	{
		if (SpinLockIds[i] >= 0)
			IpcSemaphoreKill(SpinLockIds[i]);
	}
	free(SpinLockIds);
	SpinLockIds = NULL;
}

/*
 * SpinAcquire -- grab a fixed spinlock
 *
 * FAILS if the semaphore is corrupted.
 */
void
SpinAcquire(SPINLOCK lock)
{

	/*
	 * See the TAS() version of this routine for primary commentary.
	 *
	 * NOTE we must pass interruptOK = false to IpcSemaphoreLock, to ensure
	 * that a cancel/die interrupt cannot prevent us from recording
	 * ownership of a lock we have just acquired.
	 */
	IpcSemaphoreLock(SpinLockIds[0], lock, false);
	PROC_INCR_SLOCK(lock);
	HOLD_INTERRUPTS();
}

/*
 * SpinRelease -- release a fixed spin lock
 *
 * FAILS if the semaphore is corrupted
 */
void
SpinRelease(SPINLOCK lock)
{
	/* See the TAS() version of this routine for commentary */
#ifdef USE_ASSERT_CHECKING
	/* Check it's locked */
	int			semval;

	semval = IpcSemaphoreGetValue(SpinLockIds[0], lock);
	Assert(semval < 1);
#endif
	Assert(!MyProc || MyProc->sLocks[lockid] > 0);
	PROC_DECR_SLOCK(lock);
	IpcSemaphoreUnlock(SpinLockIds[0], lock);
	RESUME_INTERRUPTS();
}

/*
 * s_lock.h hardware-spinlock emulation
 */

void
s_init_lock_sema(volatile slock_t *lock)
{
	if (nextSpinLock >= numSpinLocks)
		elog(FATAL, "s_init_lock_sema: not enough semaphores");
	lock->semId = SpinLockIds[nextSpinLock / SPINLOCKS_PER_SET];
	lock->sem = nextSpinLock % SPINLOCKS_PER_SET;
	nextSpinLock++;
}

void
s_unlock_sema(volatile slock_t *lock)
{
	IpcSemaphoreUnlock(lock->semId, lock->sem);
}

bool
s_lock_free_sema(volatile slock_t *lock)
{
	return IpcSemaphoreGetValue(lock->semId, lock->sem) > 0;
}

int
tas_sema(volatile slock_t *lock)
{
	/* Note that TAS macros return 0 if *success* */
	return !IpcSemaphoreTryLock(lock->semId, lock->sem);
}

#endif	 /* !HAS_TEST_AND_SET */
