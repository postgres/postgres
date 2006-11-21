/*-------------------------------------------------------------------------
 *
 * proc.c
 *	  routines to manage per-process shared memory data structure
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/lmgr/proc.c,v 1.181 2006/11/21 20:59:52 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * Interface (a):
 *		ProcSleep(), ProcWakeup(),
 *		ProcQueueAlloc() -- create a shm queue for sleeping processes
 *		ProcQueueInit() -- create a queue without allocing memory
 *
 * Waiting for a lock causes the backend to be put to sleep.  Whoever releases
 * the lock wakes the process up again (and gives it an error code so it knows
 * whether it was awoken on an error condition).
 *
 * Interface (b):
 *
 * ProcReleaseLocks -- frees the locks associated with current transaction
 *
 * ProcKill -- destroys the shared memory state (and locks)
 * associated with the process.
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>
#include <sys/time.h>

#include "access/transam.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/spin.h"


/* GUC variables */
int			DeadlockTimeout = 1000;
int			StatementTimeout = 0;

/* Pointer to this process's PGPROC struct, if any */
PGPROC	   *MyProc = NULL;

/*
 * This spinlock protects the freelist of recycled PGPROC structures.
 * We cannot use an LWLock because the LWLock manager depends on already
 * having a PGPROC and a wait semaphore!  But these structures are touched
 * relatively infrequently (only at backend startup or shutdown) and not for
 * very long, so a spinlock is okay.
 */
NON_EXEC_STATIC slock_t *ProcStructLock = NULL;

/* Pointers to shared-memory structures */
NON_EXEC_STATIC PROC_HDR *ProcGlobal = NULL;
NON_EXEC_STATIC PGPROC *DummyProcs = NULL;

/* If we are waiting for a lock, this points to the associated LOCALLOCK */
static LOCALLOCK *lockAwaited = NULL;

/* Mark these volatile because they can be changed by signal handler */
static volatile bool statement_timeout_active = false;
static volatile bool deadlock_timeout_active = false;
volatile bool cancel_from_timeout = false;

/* statement_fin_time is valid only if statement_timeout_active is true */
static TimestampTz statement_fin_time;


static void RemoveProcFromArray(int code, Datum arg);
static void ProcKill(int code, Datum arg);
static void DummyProcKill(int code, Datum arg);
static bool CheckStatementTimeout(void);


/*
 * Report shared-memory space needed by InitProcGlobal.
 */
Size
ProcGlobalShmemSize(void)
{
	Size		size = 0;

	/* ProcGlobal */
	size = add_size(size, sizeof(PROC_HDR));
	/* DummyProcs */
	size = add_size(size, mul_size(NUM_DUMMY_PROCS, sizeof(PGPROC)));
	/* MyProcs */
	size = add_size(size, mul_size(MaxBackends, sizeof(PGPROC)));
	/* ProcStructLock */
	size = add_size(size, sizeof(slock_t));

	return size;
}

/*
 * Report number of semaphores needed by InitProcGlobal.
 */
int
ProcGlobalSemas(void)
{
	/* We need a sema per backend, plus one for each dummy process. */
	return MaxBackends + NUM_DUMMY_PROCS;
}

/*
 * InitProcGlobal -
 *	  Initialize the global process table during postmaster or standalone
 *	  backend startup.
 *
 *	  We also create all the per-process semaphores we will need to support
 *	  the requested number of backends.  We used to allocate semaphores
 *	  only when backends were actually started up, but that is bad because
 *	  it lets Postgres fail under load --- a lot of Unix systems are
 *	  (mis)configured with small limits on the number of semaphores, and
 *	  running out when trying to start another backend is a common failure.
 *	  So, now we grab enough semaphores to support the desired max number
 *	  of backends immediately at initialization --- if the sysadmin has set
 *	  MaxBackends higher than his kernel will support, he'll find out sooner
 *	  rather than later.
 *
 *	  Another reason for creating semaphores here is that the semaphore
 *	  implementation typically requires us to create semaphores in the
 *	  postmaster, not in backends.
 *
 * Note: this is NOT called by individual backends under a postmaster,
 * not even in the EXEC_BACKEND case.  The ProcGlobal and DummyProcs
 * pointers must be propagated specially for EXEC_BACKEND operation.
 */
void
InitProcGlobal(void)
{
	PGPROC	   *procs;
	int			i;
	bool		found;

	/* Create the ProcGlobal shared structure */
	ProcGlobal = (PROC_HDR *)
		ShmemInitStruct("Proc Header", sizeof(PROC_HDR), &found);
	Assert(!found);

	/*
	 * Create the PGPROC structures for dummy (bgwriter) processes, too. These
	 * do not get linked into the freeProcs list.
	 */
	DummyProcs = (PGPROC *)
		ShmemInitStruct("DummyProcs", NUM_DUMMY_PROCS * sizeof(PGPROC),
						&found);
	Assert(!found);

	/*
	 * Initialize the data structures.
	 */
	ProcGlobal->freeProcs = INVALID_OFFSET;

	ProcGlobal->spins_per_delay = DEFAULT_SPINS_PER_DELAY;

	/*
	 * Pre-create the PGPROC structures and create a semaphore for each.
	 */
	procs = (PGPROC *) ShmemAlloc(MaxBackends * sizeof(PGPROC));
	if (!procs)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory")));
	MemSet(procs, 0, MaxBackends * sizeof(PGPROC));
	for (i = 0; i < MaxBackends; i++)
	{
		PGSemaphoreCreate(&(procs[i].sem));
		procs[i].links.next = ProcGlobal->freeProcs;
		ProcGlobal->freeProcs = MAKE_OFFSET(&procs[i]);
	}

	MemSet(DummyProcs, 0, NUM_DUMMY_PROCS * sizeof(PGPROC));
	for (i = 0; i < NUM_DUMMY_PROCS; i++)
	{
		DummyProcs[i].pid = 0;	/* marks dummy proc as not in use */
		PGSemaphoreCreate(&(DummyProcs[i].sem));
	}

	/* Create ProcStructLock spinlock, too */
	ProcStructLock = (slock_t *) ShmemAlloc(sizeof(slock_t));
	SpinLockInit(ProcStructLock);
}

/*
 * InitProcess -- initialize a per-process data structure for this backend
 */
void
InitProcess(void)
{
	/* use volatile pointer to prevent code rearrangement */
	volatile PROC_HDR *procglobal = ProcGlobal;
	SHMEM_OFFSET myOffset;
	int			i;

	/*
	 * ProcGlobal should be set up already (if we are a backend, we inherit
	 * this by fork() or EXEC_BACKEND mechanism from the postmaster).
	 */
	if (procglobal == NULL)
		elog(PANIC, "proc header uninitialized");

	if (MyProc != NULL)
		elog(ERROR, "you already exist");

	/*
	 * Try to get a proc struct from the free list.  If this fails, we must be
	 * out of PGPROC structures (not to mention semaphores).
	 *
	 * While we are holding the ProcStructLock, also copy the current shared
	 * estimate of spins_per_delay to local storage.
	 */
	SpinLockAcquire(ProcStructLock);

	set_spins_per_delay(procglobal->spins_per_delay);

	myOffset = procglobal->freeProcs;

	if (myOffset != INVALID_OFFSET)
	{
		MyProc = (PGPROC *) MAKE_PTR(myOffset);
		procglobal->freeProcs = MyProc->links.next;
		SpinLockRelease(ProcStructLock);
	}
	else
	{
		/*
		 * If we reach here, all the PGPROCs are in use.  This is one of the
		 * possible places to detect "too many backends", so give the standard
		 * error message.
		 */
		SpinLockRelease(ProcStructLock);
		ereport(FATAL,
				(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
				 errmsg("sorry, too many clients already")));
	}

	/*
	 * Initialize all fields of MyProc, except for the semaphore which was
	 * prepared for us by InitProcGlobal.
	 */
	SHMQueueElemInit(&(MyProc->links));
	MyProc->waitStatus = STATUS_OK;
	MyProc->xid = InvalidTransactionId;
	MyProc->xmin = InvalidTransactionId;
	MyProc->pid = MyProcPid;
	/* databaseId and roleId will be filled in later */
	MyProc->databaseId = InvalidOid;
	MyProc->roleId = InvalidOid;
	MyProc->inVacuum = false;
	MyProc->lwWaiting = false;
	MyProc->lwExclusive = false;
	MyProc->lwWaitLink = NULL;
	MyProc->waitLock = NULL;
	MyProc->waitProcLock = NULL;
	for (i = 0; i < NUM_LOCK_PARTITIONS; i++)
		SHMQueueInit(&(MyProc->myProcLocks[i]));

	/*
	 * We might be reusing a semaphore that belonged to a failed process. So
	 * be careful and reinitialize its value here.	(This is not strictly
	 * necessary anymore, but seems like a good idea for cleanliness.)
	 */
	PGSemaphoreReset(&MyProc->sem);

	/*
	 * Arrange to clean up at backend exit.
	 */
	on_shmem_exit(ProcKill, 0);

	/*
	 * Now that we have a PGPROC, we could try to acquire locks, so initialize
	 * the deadlock checker.
	 */
	InitDeadLockChecking();
}

/*
 * InitProcessPhase2 -- make MyProc visible in the shared ProcArray.
 *
 * This is separate from InitProcess because we can't acquire LWLocks until
 * we've created a PGPROC, but in the EXEC_BACKEND case there is a good deal
 * of stuff to be done before this step that will require LWLock access.
 */
void
InitProcessPhase2(void)
{
	Assert(MyProc != NULL);

	/*
	 * We should now know what database we're in, so advertise that.  (We need
	 * not do any locking here, since no other backend can yet see our
	 * PGPROC.)
	 */
	Assert(OidIsValid(MyDatabaseId));
	MyProc->databaseId = MyDatabaseId;

	/*
	 * Add our PGPROC to the PGPROC array in shared memory.
	 */
	ProcArrayAdd(MyProc);

	/*
	 * Arrange to clean that up at backend exit.
	 */
	on_shmem_exit(RemoveProcFromArray, 0);
}

/*
 * InitDummyProcess -- create a dummy per-process data structure
 *
 * This is called by bgwriter and similar processes so that they will have a
 * MyProc value that's real enough to let them wait for LWLocks.  The PGPROC
 * and sema that are assigned are one of the extra ones created during
 * InitProcGlobal.
 *
 * Dummy processes are presently not expected to wait for real (lockmgr)
 * locks, so we need not set up the deadlock checker.  They are never added
 * to the ProcArray or the sinval messaging mechanism, either.
 */
void
InitDummyProcess(void)
{
	PGPROC	   *dummyproc;
	int			proctype;
	int			i;

	/*
	 * ProcGlobal should be set up already (if we are a backend, we inherit
	 * this by fork() or EXEC_BACKEND mechanism from the postmaster).
	 */
	if (ProcGlobal == NULL || DummyProcs == NULL)
		elog(PANIC, "proc header uninitialized");

	if (MyProc != NULL)
		elog(ERROR, "you already exist");

	/*
	 * We use the ProcStructLock to protect assignment and releasing of
	 * DummyProcs entries.
	 *
	 * While we are holding the ProcStructLock, also copy the current shared
	 * estimate of spins_per_delay to local storage.
	 */
	SpinLockAcquire(ProcStructLock);

	set_spins_per_delay(ProcGlobal->spins_per_delay);

	/*
	 * Find a free dummyproc ... *big* trouble if there isn't one ...
	 */
	for (proctype = 0; proctype < NUM_DUMMY_PROCS; proctype++)
	{
		dummyproc = &DummyProcs[proctype];
		if (dummyproc->pid == 0)
			break;
	}
	if (proctype >= NUM_DUMMY_PROCS)
	{
		SpinLockRelease(ProcStructLock);
		elog(FATAL, "all DummyProcs are in use");
	}

	/* Mark dummy proc as in use by me */
	/* use volatile pointer to prevent code rearrangement */
	((volatile PGPROC *) dummyproc)->pid = MyProcPid;

	MyProc = dummyproc;

	SpinLockRelease(ProcStructLock);

	/*
	 * Initialize all fields of MyProc, except for the semaphore which was
	 * prepared for us by InitProcGlobal.
	 */
	SHMQueueElemInit(&(MyProc->links));
	MyProc->waitStatus = STATUS_OK;
	MyProc->xid = InvalidTransactionId;
	MyProc->xmin = InvalidTransactionId;
	MyProc->databaseId = InvalidOid;
	MyProc->roleId = InvalidOid;
	MyProc->inVacuum = false;
	MyProc->lwWaiting = false;
	MyProc->lwExclusive = false;
	MyProc->lwWaitLink = NULL;
	MyProc->waitLock = NULL;
	MyProc->waitProcLock = NULL;
	for (i = 0; i < NUM_LOCK_PARTITIONS; i++)
		SHMQueueInit(&(MyProc->myProcLocks[i]));

	/*
	 * We might be reusing a semaphore that belonged to a failed process. So
	 * be careful and reinitialize its value here.	(This is not strictly
	 * necessary anymore, but seems like a good idea for cleanliness.)
	 */
	PGSemaphoreReset(&MyProc->sem);

	/*
	 * Arrange to clean up at process exit.
	 */
	on_shmem_exit(DummyProcKill, Int32GetDatum(proctype));
}

/*
 * Check whether there are at least N free PGPROC objects.
 *
 * Note: this is designed on the assumption that N will generally be small.
 */
bool
HaveNFreeProcs(int n)
{
	SHMEM_OFFSET offset;
	PGPROC	   *proc;

	/* use volatile pointer to prevent code rearrangement */
	volatile PROC_HDR *procglobal = ProcGlobal;

	SpinLockAcquire(ProcStructLock);

	offset = procglobal->freeProcs;

	while (n > 0 && offset != INVALID_OFFSET)
	{
		proc = (PGPROC *) MAKE_PTR(offset);
		offset = proc->links.next;
		n--;
	}

	SpinLockRelease(ProcStructLock);

	return (n <= 0);
}

/*
 * Cancel any pending wait for lock, when aborting a transaction.
 *
 * Returns true if we had been waiting for a lock, else false.
 *
 * (Normally, this would only happen if we accept a cancel/die
 * interrupt while waiting; but an ereport(ERROR) while waiting is
 * within the realm of possibility, too.)
 */
bool
LockWaitCancel(void)
{
	LWLockId	partitionLock;

	/* Nothing to do if we weren't waiting for a lock */
	if (lockAwaited == NULL)
		return false;

	/* Turn off the deadlock timer, if it's still running (see ProcSleep) */
	disable_sig_alarm(false);

	/* Unlink myself from the wait queue, if on it (might not be anymore!) */
	partitionLock = LockHashPartitionLock(lockAwaited->hashcode);
	LWLockAcquire(partitionLock, LW_EXCLUSIVE);

	if (MyProc->links.next != INVALID_OFFSET)
	{
		/* We could not have been granted the lock yet */
		RemoveFromWaitQueue(MyProc, lockAwaited->hashcode);
	}
	else
	{
		/*
		 * Somebody kicked us off the lock queue already.  Perhaps they
		 * granted us the lock, or perhaps they detected a deadlock. If they
		 * did grant us the lock, we'd better remember it in our local lock
		 * table.
		 */
		if (MyProc->waitStatus == STATUS_OK)
			GrantAwaitedLock();
	}

	lockAwaited = NULL;

	LWLockRelease(partitionLock);

	/*
	 * We used to do PGSemaphoreReset() here to ensure that our proc's wait
	 * semaphore is reset to zero.	This prevented a leftover wakeup signal
	 * from remaining in the semaphore if someone else had granted us the lock
	 * we wanted before we were able to remove ourselves from the wait-list.
	 * However, now that ProcSleep loops until waitStatus changes, a leftover
	 * wakeup signal isn't harmful, and it seems not worth expending cycles to
	 * get rid of a signal that most likely isn't there.
	 */

	/*
	 * Return true even if we were kicked off the lock before we were able to
	 * remove ourselves.
	 */
	return true;
}


/*
 * ProcReleaseLocks() -- release locks associated with current transaction
 *			at main transaction commit or abort
 *
 * At main transaction commit, we release all locks except session locks.
 * At main transaction abort, we release all locks including session locks;
 * this lets us clean up after a VACUUM FULL failure.
 *
 * At subtransaction commit, we don't release any locks (so this func is not
 * needed at all); we will defer the releasing to the parent transaction.
 * At subtransaction abort, we release all locks held by the subtransaction;
 * this is implemented by retail releasing of the locks under control of
 * the ResourceOwner mechanism.
 *
 * Note that user locks are not released in any case.
 */
void
ProcReleaseLocks(bool isCommit)
{
	if (!MyProc)
		return;
	/* If waiting, get off wait queue (should only be needed after error) */
	LockWaitCancel();
	/* Release locks */
	LockReleaseAll(DEFAULT_LOCKMETHOD, !isCommit);
}


/*
 * RemoveProcFromArray() -- Remove this process from the shared ProcArray.
 */
static void
RemoveProcFromArray(int code, Datum arg)
{
	Assert(MyProc != NULL);
	ProcArrayRemove(MyProc);
}

/*
 * ProcKill() -- Destroy the per-proc data structure for
 *		this process. Release any of its held LW locks.
 */
static void
ProcKill(int code, Datum arg)
{
	/* use volatile pointer to prevent code rearrangement */
	volatile PROC_HDR *procglobal = ProcGlobal;

	Assert(MyProc != NULL);

	/*
	 * Release any LW locks I am holding.  There really shouldn't be any, but
	 * it's cheap to check again before we cut the knees off the LWLock
	 * facility by releasing our PGPROC ...
	 */
	LWLockReleaseAll();

	SpinLockAcquire(ProcStructLock);

	/* Return PGPROC structure (and semaphore) to freelist */
	MyProc->links.next = procglobal->freeProcs;
	procglobal->freeProcs = MAKE_OFFSET(MyProc);

	/* PGPROC struct isn't mine anymore */
	MyProc = NULL;

	/* Update shared estimate of spins_per_delay */
	procglobal->spins_per_delay = update_spins_per_delay(procglobal->spins_per_delay);

	SpinLockRelease(ProcStructLock);
}

/*
 * DummyProcKill() -- Cut-down version of ProcKill for dummy (bgwriter)
 *		processes.	The PGPROC and sema are not released, only marked
 *		as not-in-use.
 */
static void
DummyProcKill(int code, Datum arg)
{
	int			proctype = DatumGetInt32(arg);
	PGPROC	   *dummyproc;

	Assert(proctype >= 0 && proctype < NUM_DUMMY_PROCS);

	dummyproc = &DummyProcs[proctype];

	Assert(MyProc == dummyproc);

	/* Release any LW locks I am holding (see notes above) */
	LWLockReleaseAll();

	SpinLockAcquire(ProcStructLock);

	/* Mark dummy proc no longer in use */
	MyProc->pid = 0;

	/* PGPROC struct isn't mine anymore */
	MyProc = NULL;

	/* Update shared estimate of spins_per_delay */
	ProcGlobal->spins_per_delay = update_spins_per_delay(ProcGlobal->spins_per_delay);

	SpinLockRelease(ProcStructLock);
}


/*
 * ProcQueue package: routines for putting processes to sleep
 *		and  waking them up
 */

/*
 * ProcQueueAlloc -- alloc/attach to a shared memory process queue
 *
 * Returns: a pointer to the queue or NULL
 * Side Effects: Initializes the queue if we allocated one
 */
#ifdef NOT_USED
PROC_QUEUE *
ProcQueueAlloc(char *name)
{
	bool		found;
	PROC_QUEUE *queue = (PROC_QUEUE *)
	ShmemInitStruct(name, sizeof(PROC_QUEUE), &found);

	if (!queue)
		return NULL;
	if (!found)
		ProcQueueInit(queue);
	return queue;
}
#endif

/*
 * ProcQueueInit -- initialize a shared memory process queue
 */
void
ProcQueueInit(PROC_QUEUE *queue)
{
	SHMQueueInit(&(queue->links));
	queue->size = 0;
}


/*
 * ProcSleep -- put a process to sleep on the specified lock
 *
 * Caller must have set MyProc->heldLocks to reflect locks already held
 * on the lockable object by this process (under all XIDs).
 *
 * The lock table's partition lock must be held at entry, and will be held
 * at exit.
 *
 * Result: STATUS_OK if we acquired the lock, STATUS_ERROR if not (deadlock).
 *
 * ASSUME: that no one will fiddle with the queue until after
 *		we release the partition lock.
 *
 * NOTES: The process queue is now a priority queue for locking.
 *
 * P() on the semaphore should put us to sleep.  The process
 * semaphore is normally zero, so when we try to acquire it, we sleep.
 */
int
ProcSleep(LOCALLOCK *locallock, LockMethod lockMethodTable)
{
	LOCKMODE	lockmode = locallock->tag.mode;
	LOCK	   *lock = locallock->lock;
	PROCLOCK   *proclock = locallock->proclock;
	uint32		hashcode = locallock->hashcode;
	LWLockId	partitionLock = LockHashPartitionLock(hashcode);
	PROC_QUEUE *waitQueue = &(lock->waitProcs);
	LOCKMASK	myHeldLocks = MyProc->heldLocks;
	bool		early_deadlock = false;
	PGPROC	   *proc;
	int			i;

	/*
	 * Determine where to add myself in the wait queue.
	 *
	 * Normally I should go at the end of the queue.  However, if I already
	 * hold locks that conflict with the request of any previous waiter, put
	 * myself in the queue just in front of the first such waiter. This is not
	 * a necessary step, since deadlock detection would move me to before that
	 * waiter anyway; but it's relatively cheap to detect such a conflict
	 * immediately, and avoid delaying till deadlock timeout.
	 *
	 * Special case: if I find I should go in front of some waiter, check to
	 * see if I conflict with already-held locks or the requests before that
	 * waiter.	If not, then just grant myself the requested lock immediately.
	 * This is the same as the test for immediate grant in LockAcquire, except
	 * we are only considering the part of the wait queue before my insertion
	 * point.
	 */
	if (myHeldLocks != 0)
	{
		LOCKMASK	aheadRequests = 0;

		proc = (PGPROC *) MAKE_PTR(waitQueue->links.next);
		for (i = 0; i < waitQueue->size; i++)
		{
			/* Must he wait for me? */
			if (lockMethodTable->conflictTab[proc->waitLockMode] & myHeldLocks)
			{
				/* Must I wait for him ? */
				if (lockMethodTable->conflictTab[lockmode] & proc->heldLocks)
				{
					/*
					 * Yes, so we have a deadlock.	Easiest way to clean up
					 * correctly is to call RemoveFromWaitQueue(), but we
					 * can't do that until we are *on* the wait queue. So, set
					 * a flag to check below, and break out of loop.  Also,
					 * record deadlock info for later message.
					 */
					RememberSimpleDeadLock(MyProc, lockmode, lock, proc);
					early_deadlock = true;
					break;
				}
				/* I must go before this waiter.  Check special case. */
				if ((lockMethodTable->conflictTab[lockmode] & aheadRequests) == 0 &&
					LockCheckConflicts(lockMethodTable,
									   lockmode,
									   lock,
									   proclock,
									   MyProc) == STATUS_OK)
				{
					/* Skip the wait and just grant myself the lock. */
					GrantLock(lock, proclock, lockmode);
					GrantAwaitedLock();
					return STATUS_OK;
				}
				/* Break out of loop to put myself before him */
				break;
			}
			/* Nope, so advance to next waiter */
			aheadRequests |= LOCKBIT_ON(proc->waitLockMode);
			proc = (PGPROC *) MAKE_PTR(proc->links.next);
		}

		/*
		 * If we fall out of loop normally, proc points to waitQueue head, so
		 * we will insert at tail of queue as desired.
		 */
	}
	else
	{
		/* I hold no locks, so I can't push in front of anyone. */
		proc = (PGPROC *) &(waitQueue->links);
	}

	/*
	 * Insert self into queue, ahead of the given proc (or at tail of queue).
	 */
	SHMQueueInsertBefore(&(proc->links), &(MyProc->links));
	waitQueue->size++;

	lock->waitMask |= LOCKBIT_ON(lockmode);

	/* Set up wait information in PGPROC object, too */
	MyProc->waitLock = lock;
	MyProc->waitProcLock = proclock;
	MyProc->waitLockMode = lockmode;

	MyProc->waitStatus = STATUS_WAITING;

	/*
	 * If we detected deadlock, give up without waiting.  This must agree with
	 * CheckDeadLock's recovery code, except that we shouldn't release the
	 * semaphore since we haven't tried to lock it yet.
	 */
	if (early_deadlock)
	{
		RemoveFromWaitQueue(MyProc, hashcode);
		return STATUS_ERROR;
	}

	/* mark that we are waiting for a lock */
	lockAwaited = locallock;

	/*
	 * Release the lock table's partition lock.
	 *
	 * NOTE: this may also cause us to exit critical-section state, possibly
	 * allowing a cancel/die interrupt to be accepted. This is OK because we
	 * have recorded the fact that we are waiting for a lock, and so
	 * LockWaitCancel will clean up if cancel/die happens.
	 */
	LWLockRelease(partitionLock);

	/*
	 * Set timer so we can wake up after awhile and check for a deadlock. If a
	 * deadlock is detected, the handler releases the process's semaphore and
	 * sets MyProc->waitStatus = STATUS_ERROR, allowing us to know that we
	 * must report failure rather than success.
	 *
	 * By delaying the check until we've waited for a bit, we can avoid
	 * running the rather expensive deadlock-check code in most cases.
	 */
	if (!enable_sig_alarm(DeadlockTimeout, false))
		elog(FATAL, "could not set timer for process wakeup");

	/*
	 * If someone wakes us between LWLockRelease and PGSemaphoreLock,
	 * PGSemaphoreLock will not block.	The wakeup is "saved" by the semaphore
	 * implementation.	While this is normally good, there are cases where a
	 * saved wakeup might be leftover from a previous operation (for example,
	 * we aborted ProcWaitForSignal just before someone did ProcSendSignal).
	 * So, loop to wait again if the waitStatus shows we haven't been granted
	 * nor denied the lock yet.
	 *
	 * We pass interruptOK = true, which eliminates a window in which
	 * cancel/die interrupts would be held off undesirably.  This is a promise
	 * that we don't mind losing control to a cancel/die interrupt here.  We
	 * don't, because we have no shared-state-change work to do after being
	 * granted the lock (the grantor did it all).  We do have to worry about
	 * updating the locallock table, but if we lose control to an error,
	 * LockWaitCancel will fix that up.
	 */
	do
	{
		PGSemaphoreLock(&MyProc->sem, true);
	} while (MyProc->waitStatus == STATUS_WAITING);

	/*
	 * Disable the timer, if it's still running
	 */
	if (!disable_sig_alarm(false))
		elog(FATAL, "could not disable timer for process wakeup");

	/*
	 * Re-acquire the lock table's partition lock.  We have to do this to hold
	 * off cancel/die interrupts before we can mess with lockAwaited (else we
	 * might have a missed or duplicated locallock update).
	 */
	LWLockAcquire(partitionLock, LW_EXCLUSIVE);

	/*
	 * We no longer want LockWaitCancel to do anything.
	 */
	lockAwaited = NULL;

	/*
	 * If we got the lock, be sure to remember it in the locallock table.
	 */
	if (MyProc->waitStatus == STATUS_OK)
		GrantAwaitedLock();

	/*
	 * We don't have to do anything else, because the awaker did all the
	 * necessary update of the lock table and MyProc.
	 */
	return MyProc->waitStatus;
}


/*
 * ProcWakeup -- wake up a process by releasing its private semaphore.
 *
 *	 Also remove the process from the wait queue and set its links invalid.
 *	 RETURN: the next process in the wait queue.
 *
 * The appropriate lock partition lock must be held by caller.
 *
 * XXX: presently, this code is only used for the "success" case, and only
 * works correctly for that case.  To clean up in failure case, would need
 * to twiddle the lock's request counts too --- see RemoveFromWaitQueue.
 * Hence, in practice the waitStatus parameter must be STATUS_OK.
 */
PGPROC *
ProcWakeup(PGPROC *proc, int waitStatus)
{
	PGPROC	   *retProc;

	/* Proc should be sleeping ... */
	if (proc->links.prev == INVALID_OFFSET ||
		proc->links.next == INVALID_OFFSET)
		return NULL;
	Assert(proc->waitStatus == STATUS_WAITING);

	/* Save next process before we zap the list link */
	retProc = (PGPROC *) MAKE_PTR(proc->links.next);

	/* Remove process from wait queue */
	SHMQueueDelete(&(proc->links));
	(proc->waitLock->waitProcs.size)--;

	/* Clean up process' state and pass it the ok/fail signal */
	proc->waitLock = NULL;
	proc->waitProcLock = NULL;
	proc->waitStatus = waitStatus;

	/* And awaken it */
	PGSemaphoreUnlock(&proc->sem);

	return retProc;
}

/*
 * ProcLockWakeup -- routine for waking up processes when a lock is
 *		released (or a prior waiter is aborted).  Scan all waiters
 *		for lock, waken any that are no longer blocked.
 *
 * The appropriate lock partition lock must be held by caller.
 */
void
ProcLockWakeup(LockMethod lockMethodTable, LOCK *lock)
{
	PROC_QUEUE *waitQueue = &(lock->waitProcs);
	int			queue_size = waitQueue->size;
	PGPROC	   *proc;
	LOCKMASK	aheadRequests = 0;

	Assert(queue_size >= 0);

	if (queue_size == 0)
		return;

	proc = (PGPROC *) MAKE_PTR(waitQueue->links.next);

	while (queue_size-- > 0)
	{
		LOCKMODE	lockmode = proc->waitLockMode;

		/*
		 * Waken if (a) doesn't conflict with requests of earlier waiters, and
		 * (b) doesn't conflict with already-held locks.
		 */
		if ((lockMethodTable->conflictTab[lockmode] & aheadRequests) == 0 &&
			LockCheckConflicts(lockMethodTable,
							   lockmode,
							   lock,
							   proc->waitProcLock,
							   proc) == STATUS_OK)
		{
			/* OK to waken */
			GrantLock(lock, proc->waitProcLock, lockmode);
			proc = ProcWakeup(proc, STATUS_OK);

			/*
			 * ProcWakeup removes proc from the lock's waiting process queue
			 * and returns the next proc in chain; don't use proc's next-link,
			 * because it's been cleared.
			 */
		}
		else
		{
			/*
			 * Cannot wake this guy. Remember his request for later checks.
			 */
			aheadRequests |= LOCKBIT_ON(lockmode);
			proc = (PGPROC *) MAKE_PTR(proc->links.next);
		}
	}

	Assert(waitQueue->size >= 0);
}

/*
 * CheckDeadLock
 *
 * We only get to this routine if we got SIGALRM after DeadlockTimeout
 * while waiting for a lock to be released by some other process.  Look
 * to see if there's a deadlock; if not, just return and continue waiting.
 * If we have a real deadlock, remove ourselves from the lock's wait queue
 * and signal an error to ProcSleep.
 */
static void
CheckDeadLock(void)
{
	int			i;

	/*
	 * Acquire exclusive lock on the entire shared lock data structures. Must
	 * grab LWLocks in partition-number order to avoid LWLock deadlock.
	 *
	 * Note that the deadlock check interrupt had better not be enabled
	 * anywhere that this process itself holds lock partition locks, else this
	 * will wait forever.  Also note that LWLockAcquire creates a critical
	 * section, so that this routine cannot be interrupted by cancel/die
	 * interrupts.
	 */
	for (i = 0; i < NUM_LOCK_PARTITIONS; i++)
		LWLockAcquire(FirstLockMgrLock + i, LW_EXCLUSIVE);

	/*
	 * Check to see if we've been awoken by anyone in the interim.
	 *
	 * If we have we can return and resume our transaction -- happy day.
	 * Before we are awoken the process releasing the lock grants it to us so
	 * we know that we don't have to wait anymore.
	 *
	 * We check by looking to see if we've been unlinked from the wait queue.
	 * This is quicker than checking our semaphore's state, since no kernel
	 * call is needed, and it is safe because we hold the lock partition lock.
	 */
	if (MyProc->links.prev == INVALID_OFFSET ||
		MyProc->links.next == INVALID_OFFSET)
		goto check_done;

#ifdef LOCK_DEBUG
	if (Debug_deadlocks)
		DumpAllLocks();
#endif

	if (!DeadLockCheck(MyProc))
	{
		/* No deadlock, so keep waiting */
		goto check_done;
	}

	/*
	 * Oops.  We have a deadlock.
	 *
	 * Get this process out of wait state.	(Note: we could do this more
	 * efficiently by relying on lockAwaited, but use this coding to preserve
	 * the flexibility to kill some other transaction than the one detecting
	 * the deadlock.)
	 *
	 * RemoveFromWaitQueue sets MyProc->waitStatus to STATUS_ERROR, so
	 * ProcSleep will report an error after we return from the signal handler.
	 */
	Assert(MyProc->waitLock != NULL);
	RemoveFromWaitQueue(MyProc, LockTagHashCode(&(MyProc->waitLock->tag)));

	/*
	 * Unlock my semaphore so that the interrupted ProcSleep() call can
	 * finish.
	 */
	PGSemaphoreUnlock(&MyProc->sem);

	/*
	 * We're done here.  Transaction abort caused by the error that ProcSleep
	 * will raise will cause any other locks we hold to be released, thus
	 * allowing other processes to wake up; we don't need to do that here.
	 * NOTE: an exception is that releasing locks we hold doesn't consider the
	 * possibility of waiters that were blocked behind us on the lock we just
	 * failed to get, and might now be wakable because we're not in front of
	 * them anymore.  However, RemoveFromWaitQueue took care of waking up any
	 * such processes.
	 */

	/*
	 * Release locks acquired at head of routine.  Order is not critical, so
	 * do it back-to-front to avoid waking another CheckDeadLock instance
	 * before it can get all the locks.
	 */
check_done:
	for (i = NUM_LOCK_PARTITIONS; --i >= 0;)
		LWLockRelease(FirstLockMgrLock + i);
}


/*
 * ProcWaitForSignal - wait for a signal from another backend.
 *
 * This can share the semaphore normally used for waiting for locks,
 * since a backend could never be waiting for a lock and a signal at
 * the same time.  As with locks, it's OK if the signal arrives just
 * before we actually reach the waiting state.	Also as with locks,
 * it's necessary that the caller be robust against bogus wakeups:
 * always check that the desired state has occurred, and wait again
 * if not.	This copes with possible "leftover" wakeups.
 */
void
ProcWaitForSignal(void)
{
	PGSemaphoreLock(&MyProc->sem, true);
}

/*
 * ProcSendSignal - send a signal to a backend identified by PID
 */
void
ProcSendSignal(int pid)
{
	PGPROC	   *proc = BackendPidGetProc(pid);

	if (proc != NULL)
		PGSemaphoreUnlock(&proc->sem);
}


/*****************************************************************************
 * SIGALRM interrupt support
 *
 * Maybe these should be in pqsignal.c?
 *****************************************************************************/

/*
 * Enable the SIGALRM interrupt to fire after the specified delay
 *
 * Delay is given in milliseconds.	Caller should be sure a SIGALRM
 * signal handler is installed before this is called.
 *
 * This code properly handles nesting of deadlock timeout alarms within
 * statement timeout alarms.
 *
 * Returns TRUE if okay, FALSE on failure.
 */
bool
enable_sig_alarm(int delayms, bool is_statement_timeout)
{
	TimestampTz fin_time;
	struct itimerval timeval;

	if (is_statement_timeout)
	{
		/*
		 * Begin statement-level timeout
		 *
		 * Note that we compute statement_fin_time with reference to the
		 * statement_timestamp, but apply the specified delay without any
		 * correction; that is, we ignore whatever time has elapsed since
		 * statement_timestamp was set.  In the normal case only a small
		 * interval will have elapsed and so this doesn't matter, but there
		 * are corner cases (involving multi-statement query strings with
		 * embedded COMMIT or ROLLBACK) where we might re-initialize the
		 * statement timeout long after initial receipt of the message. In
		 * such cases the enforcement of the statement timeout will be a bit
		 * inconsistent.  This annoyance is judged not worth the cost of
		 * performing an additional gettimeofday() here.
		 */
		Assert(!deadlock_timeout_active);
		fin_time = GetCurrentStatementStartTimestamp();
		fin_time = TimestampTzPlusMilliseconds(fin_time, delayms);
		statement_fin_time = fin_time;
		cancel_from_timeout = false;
		statement_timeout_active = true;
	}
	else if (statement_timeout_active)
	{
		/*
		 * Begin deadlock timeout with statement-level timeout active
		 *
		 * Here, we want to interrupt at the closer of the two timeout times.
		 * If fin_time >= statement_fin_time then we need not touch the
		 * existing timer setting; else set up to interrupt at the deadlock
		 * timeout time.
		 *
		 * NOTE: in this case it is possible that this routine will be
		 * interrupted by the previously-set timer alarm.  This is okay
		 * because the signal handler will do only what it should do according
		 * to the state variables.	The deadlock checker may get run earlier
		 * than normal, but that does no harm.
		 */
		fin_time = GetCurrentTimestamp();
		fin_time = TimestampTzPlusMilliseconds(fin_time, delayms);
		deadlock_timeout_active = true;
		if (fin_time >= statement_fin_time)
			return true;
	}
	else
	{
		/* Begin deadlock timeout with no statement-level timeout */
		deadlock_timeout_active = true;
	}

	/* If we reach here, okay to set the timer interrupt */
	MemSet(&timeval, 0, sizeof(struct itimerval));
	timeval.it_value.tv_sec = delayms / 1000;
	timeval.it_value.tv_usec = (delayms % 1000) * 1000;
	if (setitimer(ITIMER_REAL, &timeval, NULL))
		return false;
	return true;
}

/*
 * Cancel the SIGALRM timer, either for a deadlock timeout or a statement
 * timeout.  If a deadlock timeout is canceled, any active statement timeout
 * remains in force.
 *
 * Returns TRUE if okay, FALSE on failure.
 */
bool
disable_sig_alarm(bool is_statement_timeout)
{
	/*
	 * Always disable the interrupt if it is active; this avoids being
	 * interrupted by the signal handler and thereby possibly getting
	 * confused.
	 *
	 * We will re-enable the interrupt if necessary in CheckStatementTimeout.
	 */
	if (statement_timeout_active || deadlock_timeout_active)
	{
		struct itimerval timeval;

		MemSet(&timeval, 0, sizeof(struct itimerval));
		if (setitimer(ITIMER_REAL, &timeval, NULL))
		{
			statement_timeout_active = false;
			cancel_from_timeout = false;
			deadlock_timeout_active = false;
			return false;
		}
	}

	/* Always cancel deadlock timeout, in case this is error cleanup */
	deadlock_timeout_active = false;

	/* Cancel or reschedule statement timeout */
	if (is_statement_timeout)
	{
		statement_timeout_active = false;
		cancel_from_timeout = false;
	}
	else if (statement_timeout_active)
	{
		if (!CheckStatementTimeout())
			return false;
	}
	return true;
}


/*
 * Check for statement timeout.  If the timeout time has come,
 * trigger a query-cancel interrupt; if not, reschedule the SIGALRM
 * interrupt to occur at the right time.
 *
 * Returns true if okay, false if failed to set the interrupt.
 */
static bool
CheckStatementTimeout(void)
{
	TimestampTz now;

	if (!statement_timeout_active)
		return true;			/* do nothing if not active */

	now = GetCurrentTimestamp();

	if (now >= statement_fin_time)
	{
		/* Time to die */
		statement_timeout_active = false;
		cancel_from_timeout = true;
#ifdef HAVE_SETSID
		/* try to signal whole process group */
		kill(-MyProcPid, SIGINT);
#endif
		kill(MyProcPid, SIGINT);
	}
	else
	{
		/* Not time yet, so (re)schedule the interrupt */
		long		secs;
		int			usecs;
		struct itimerval timeval;

		TimestampDifference(now, statement_fin_time,
							&secs, &usecs);

		/*
		 * It's possible that the difference is less than a microsecond;
		 * ensure we don't cancel, rather than set, the interrupt.
		 */
		if (secs == 0 && usecs == 0)
			usecs = 1;
		MemSet(&timeval, 0, sizeof(struct itimerval));
		timeval.it_value.tv_sec = secs;
		timeval.it_value.tv_usec = usecs;
		if (setitimer(ITIMER_REAL, &timeval, NULL))
			return false;
	}

	return true;
}


/*
 * Signal handler for SIGALRM
 *
 * Process deadlock check and/or statement timeout check, as needed.
 * To avoid various edge cases, we must be careful to do nothing
 * when there is nothing to be done.  We also need to be able to
 * reschedule the timer interrupt if called before end of statement.
 */
void
handle_sig_alarm(SIGNAL_ARGS)
{
	int			save_errno = errno;

	if (deadlock_timeout_active)
	{
		deadlock_timeout_active = false;
		CheckDeadLock();
	}

	if (statement_timeout_active)
		(void) CheckStatementTimeout();

	errno = save_errno;
}
