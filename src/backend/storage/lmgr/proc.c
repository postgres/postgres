/*-------------------------------------------------------------------------
 *
 * proc.c
 *	  routines to manage per-process shared memory data structure
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/lmgr/proc.c,v 1.116 2001/11/08 20:37:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * Interface (a):
 *		ProcSleep(), ProcWakeup(),
 *		ProcQueueAlloc() -- create a shm queue for sleeping processes
 *		ProcQueueInit() -- create a queue without allocing memory
 *
 * Locking and waiting for buffers can cause the backend to be
 * put to sleep.  Whoever releases the lock, etc. wakes the
 * process up again (and gives it an error code so it knows
 * whether it was awoken on an error condition).
 *
 * Interface (b):
 *
 * ProcReleaseLocks -- frees the locks associated with current transaction
 *
 * ProcKill -- destroys the shared memory state (and locks)
 *		associated with the process.
 *
 * 5/15/91 -- removed the buffer pool based lock chain in favor
 *		of a shared memory lock chain.	The write-protection is
 *		more expensive if the lock chain is in the buffer pool.
 *		The only reason I kept the lock chain in the buffer pool
 *		in the first place was to allow the lock table to grow larger
 *		than available shared memory and that isn't going to work
 *		without a lot of unimplemented support anyway.
 *
 * 4/7/95 -- instead of allocating a set of 1 semaphore per process, we
 *		allocate a semaphore from a set of PROC_NSEMS_PER_SET semaphores
 *		shared among backends (we keep a few sets of semaphores around).
 *		This is so that we can support more backends. (system-wide semaphore
 *		sets run out pretty fast.)				  -ay 4/95
 */
#include "postgres.h"

#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>

#include "storage/ipc.h"
/* In Ultrix, sem.h and shm.h must be included AFTER ipc.h */
#ifdef HAVE_SYS_SEM_H
#include <sys/sem.h>
#endif

#if defined(__darwin__)
#include "port/darwin/sem.h"
#endif

#include "miscadmin.h"
#include "access/xact.h"
#include "storage/proc.h"
#include "storage/sinval.h"
#include "storage/spin.h"


int			DeadlockTimeout = 1000;

PROC	   *MyProc = NULL;

/*
 * This spinlock protects the freelist of recycled PROC structures and the
 * bitmap of free semaphores.  We cannot use an LWLock because the LWLock
 * manager depends on already having a PROC and a wait semaphore!  But these
 * structures are touched relatively infrequently (only at backend startup
 * or shutdown) and not for very long, so a spinlock is okay.
 */
static slock_t *ProcStructLock = NULL;

static PROC_HDR *ProcGlobal = NULL;

static PROC *DummyProc = NULL;

static bool waitingForLock = false;
static bool waitingForSignal = false;

static void ProcKill(void);
static void DummyProcKill(void);
static void ProcGetNewSemIdAndNum(IpcSemaphoreId *semId, int *semNum);
static void ProcFreeSem(IpcSemaphoreId semId, int semNum);
static void ZeroProcSemaphore(PROC *proc);
static void ProcFreeAllSemaphores(void);


/*
 * InitProcGlobal -
 *	  initializes the global process table. We put it here so that
 *	  the postmaster can do this initialization. (ProcFreeAllSemaphores needs
 *	  to read this table on exiting the postmaster. If we have the first
 *	  backend do this, starting up and killing the postmaster without
 *	  starting any backends will be a problem.)
 *
 *	  We also allocate all the per-process semaphores we will need to support
 *	  the requested number of backends.  We used to allocate semaphores
 *	  only when backends were actually started up, but that is bad because
 *	  it lets Postgres fail under load --- a lot of Unix systems are
 *	  (mis)configured with small limits on the number of semaphores, and
 *	  running out when trying to start another backend is a common failure.
 *	  So, now we grab enough semaphores to support the desired max number
 *	  of backends immediately at initialization --- if the sysadmin has set
 *	  MaxBackends higher than his kernel will support, he'll find out sooner
 *	  rather than later.
 */
void
InitProcGlobal(int maxBackends)
{
	int			semMapEntries;
	Size		procGlobalSize;
	bool		found = false;

	/*
	 * Compute size for ProcGlobal structure.  Note we need one more sema
	 * besides those used for regular backends; this is accounted for in
	 * the PROC_SEM_MAP_ENTRIES macro.	(We do it that way so that other
	 * modules that use PROC_SEM_MAP_ENTRIES(maxBackends) to size data
	 * structures don't have to know about this explicitly.)
	 */
	Assert(maxBackends > 0);
	semMapEntries = PROC_SEM_MAP_ENTRIES(maxBackends);
	procGlobalSize = sizeof(PROC_HDR) + (semMapEntries - 1) *sizeof(SEM_MAP_ENTRY);

	/* Create or attach to the ProcGlobal shared structure */
	ProcGlobal = (PROC_HDR *)
		ShmemInitStruct("Proc Header", procGlobalSize, &found);

	/* --------------------
	 * We're the first - initialize.
	 * XXX if found should ever be true, it is a sign of impending doom ...
	 * ought to complain if so?
	 * --------------------
	 */
	if (!found)
	{
		int			i;

		ProcGlobal->freeProcs = INVALID_OFFSET;
		ProcGlobal->semMapEntries = semMapEntries;

		for (i = 0; i < semMapEntries; i++)
		{
			ProcGlobal->procSemMap[i].procSemId = -1;
			ProcGlobal->procSemMap[i].freeSemMap = 0;
		}

		/*
		 * Arrange to delete semas on exit --- set this up now so that we
		 * will clean up if pre-allocation fails.  We use our own
		 * freeproc, rather than IpcSemaphoreCreate's removeOnExit option,
		 * because we don't want to fill up the on_shmem_exit list with a
		 * separate entry for each semaphore set.
		 */
		on_shmem_exit(ProcFreeAllSemaphores, 0);

		/*
		 * Pre-create the semaphores.
		 */
		for (i = 0; i < semMapEntries; i++)
		{
			IpcSemaphoreId semId;

			semId = IpcSemaphoreCreate(PROC_NSEMS_PER_SET,
									   IPCProtection,
									   1,
									   false);
			ProcGlobal->procSemMap[i].procSemId = semId;
		}

		/*
		 * Pre-allocate a PROC structure for dummy (checkpoint) processes,
		 * and reserve the last sema of the precreated semas for it.
		 */
		DummyProc = (PROC *) ShmemAlloc(sizeof(PROC));
		DummyProc->pid = 0;		/* marks DummyProc as not in use */
		i = semMapEntries - 1;
		ProcGlobal->procSemMap[i].freeSemMap |= 1 << (PROC_NSEMS_PER_SET - 1);
		DummyProc->sem.semId = ProcGlobal->procSemMap[i].procSemId;
		DummyProc->sem.semNum = PROC_NSEMS_PER_SET - 1;

		/* Create ProcStructLock spinlock, too */
		ProcStructLock = (slock_t *) ShmemAlloc(sizeof(slock_t));
		SpinLockInit(ProcStructLock);
	}
}

/*
 * InitProcess -- create a per-process data structure for this backend
 */
void
InitProcess(void)
{
	SHMEM_OFFSET myOffset;

	/*
	 * ProcGlobal should be set by a previous call to InitProcGlobal (if
	 * we are a backend, we inherit this by fork() from the postmaster).
	 */
	if (ProcGlobal == NULL)
		elog(STOP, "InitProcess: Proc Header uninitialized");

	if (MyProc != NULL)
		elog(ERROR, "InitProcess: you already exist");

	/*
	 * try to get a proc struct from the free list first
	 */
	SpinLockAcquire(ProcStructLock);

	myOffset = ProcGlobal->freeProcs;

	if (myOffset != INVALID_OFFSET)
	{
		MyProc = (PROC *) MAKE_PTR(myOffset);
		ProcGlobal->freeProcs = MyProc->links.next;
		SpinLockRelease(ProcStructLock);
	}
	else
	{
		/*
		 * have to allocate a new one.
		 */
		SpinLockRelease(ProcStructLock);
		MyProc = (PROC *) ShmemAlloc(sizeof(PROC));
		if (!MyProc)
			elog(FATAL, "cannot create new proc: out of memory");
	}

	/*
	 * Initialize all fields of MyProc.
	 */
	SHMQueueElemInit(&(MyProc->links));
	MyProc->sem.semId = -1;		/* no wait-semaphore acquired yet */
	MyProc->sem.semNum = -1;
	MyProc->errType = STATUS_OK;
	MyProc->xid = InvalidTransactionId;
	MyProc->xmin = InvalidTransactionId;
	MyProc->pid = MyProcPid;
	MyProc->databaseId = MyDatabaseId;
	MyProc->logRec.xrecoff = 0;
	MyProc->lwWaiting = false;
	MyProc->lwExclusive = false;
	MyProc->lwWaitLink = NULL;
	MyProc->waitLock = NULL;
	MyProc->waitHolder = NULL;
	SHMQueueInit(&(MyProc->procHolders));

	/*
	 * Arrange to clean up at backend exit.
	 */
	on_shmem_exit(ProcKill, 0);

	/*
	 * Set up a wait-semaphore for the proc.  (We rely on ProcKill to
	 * clean up MyProc if this fails.)
	 */
	if (IsUnderPostmaster)
		ProcGetNewSemIdAndNum(&MyProc->sem.semId, &MyProc->sem.semNum);

	/*
	 * We might be reusing a semaphore that belonged to a failed process.
	 * So be careful and reinitialize its value here.
	 */
	if (MyProc->sem.semId >= 0)
		ZeroProcSemaphore(MyProc);

	/*
	 * Now that we have a PROC, we could try to acquire locks, so
	 * initialize the deadlock checker.
	 */
	InitDeadLockChecking();
}

/*
 * InitDummyProcess -- create a dummy per-process data structure
 *
 * This is called by checkpoint processes so that they will have a MyProc
 * value that's real enough to let them wait for LWLocks.  The PROC and
 * sema that are assigned are the extra ones created during InitProcGlobal.
 */
void
InitDummyProcess(void)
{
	/*
	 * ProcGlobal should be set by a previous call to InitProcGlobal (we
	 * inherit this by fork() from the postmaster).
	 */
	if (ProcGlobal == NULL || DummyProc == NULL)
		elog(STOP, "InitDummyProcess: Proc Header uninitialized");

	if (MyProc != NULL)
		elog(ERROR, "InitDummyProcess: you already exist");

	/*
	 * DummyProc should not presently be in use by anyone else
	 */
	if (DummyProc->pid != 0)
		elog(FATAL, "InitDummyProcess: DummyProc is in use by PID %d",
			 DummyProc->pid);
	MyProc = DummyProc;

	/*
	 * Initialize all fields of MyProc, except MyProc->sem which was set
	 * up by InitProcGlobal.
	 */
	MyProc->pid = MyProcPid;	/* marks DummyProc as in use by me */
	SHMQueueElemInit(&(MyProc->links));
	MyProc->errType = STATUS_OK;
	MyProc->xid = InvalidTransactionId;
	MyProc->xmin = InvalidTransactionId;
	MyProc->databaseId = MyDatabaseId;
	MyProc->logRec.xrecoff = 0;
	MyProc->lwWaiting = false;
	MyProc->lwExclusive = false;
	MyProc->lwWaitLink = NULL;
	MyProc->waitLock = NULL;
	MyProc->waitHolder = NULL;
	SHMQueueInit(&(MyProc->procHolders));

	/*
	 * Arrange to clean up at process exit.
	 */
	on_shmem_exit(DummyProcKill, 0);

	/*
	 * We might be reusing a semaphore that belonged to a failed process.
	 * So be careful and reinitialize its value here.
	 */
	if (MyProc->sem.semId >= 0)
		ZeroProcSemaphore(MyProc);
}

/*
 * Initialize the proc's wait-semaphore to count zero.
 */
static void
ZeroProcSemaphore(PROC *proc)
{
	union semun semun;

	semun.val = 0;
	if (semctl(proc->sem.semId, proc->sem.semNum, SETVAL, semun) < 0)
	{
		fprintf(stderr, "ZeroProcSemaphore: semctl(id=%d,SETVAL) failed: %s\n",
				proc->sem.semId, strerror(errno));
		proc_exit(255);
	}
}

/*
 * Cancel any pending wait for lock, when aborting a transaction.
 *
 * Returns true if we had been waiting for a lock, else false.
 *
 * (Normally, this would only happen if we accept a cancel/die
 * interrupt while waiting; but an elog(ERROR) while waiting is
 * within the realm of possibility, too.)
 */
bool
LockWaitCancel(void)
{
	/* Nothing to do if we weren't waiting for a lock */
	if (!waitingForLock)
		return false;

	waitingForLock = false;

	/* Turn off the deadlock timer, if it's still running (see ProcSleep) */
	disable_sigalrm_interrupt();

	/* Unlink myself from the wait queue, if on it (might not be anymore!) */
	LWLockAcquire(LockMgrLock, LW_EXCLUSIVE);
	if (MyProc->links.next != INVALID_OFFSET)
		RemoveFromWaitQueue(MyProc);
	LWLockRelease(LockMgrLock);

	/*
	 * Reset the proc wait semaphore to zero.  This is necessary in the
	 * scenario where someone else granted us the lock we wanted before we
	 * were able to remove ourselves from the wait-list.  The semaphore
	 * will have been bumped to 1 by the would-be grantor, and since we
	 * are no longer going to wait on the sema, we have to force it back
	 * to zero. Otherwise, our next attempt to wait for a lock will fall
	 * through prematurely.
	 */
	ZeroProcSemaphore(MyProc);

	/*
	 * Return true even if we were kicked off the lock before we were able
	 * to remove ourselves.
	 */
	return true;
}


/*
 * ProcReleaseLocks() -- release locks associated with current transaction
 *			at transaction commit or abort
 *
 * At commit, we release only locks tagged with the current transaction's XID,
 * leaving those marked with XID 0 (ie, session locks) undisturbed.  At abort,
 * we release all locks including XID 0, because we need to clean up after
 * a failure.  This logic will need extension if we ever support nested
 * transactions.
 *
 * Note that user locks are not released in either case.
 */
void
ProcReleaseLocks(bool isCommit)
{
	if (!MyProc)
		return;
	/* If waiting, get off wait queue (should only be needed after error) */
	LockWaitCancel();
	/* Release locks */
	LockReleaseAll(DEFAULT_LOCKMETHOD, MyProc,
				   !isCommit, GetCurrentTransactionId());
}


/*
 * ProcKill() -- Destroy the per-proc data structure for
 *		this process. Release any of its held LW locks.
 */
static void
ProcKill(void)
{
	Assert(MyProc != NULL);

	/* Release any LW locks I am holding */
	LWLockReleaseAll();

	/* Abort any buffer I/O in progress */
	AbortBufferIO();

	/* Get off any wait queue I might be on */
	LockWaitCancel();

	/* Remove from the standard lock table */
	LockReleaseAll(DEFAULT_LOCKMETHOD, MyProc, true, InvalidTransactionId);

#ifdef USER_LOCKS
	/* Remove from the user lock table */
	LockReleaseAll(USER_LOCKMETHOD, MyProc, true, InvalidTransactionId);
#endif

	SpinLockAcquire(ProcStructLock);

	/* Free up my wait semaphore, if I got one */
	if (MyProc->sem.semId >= 0)
		ProcFreeSem(MyProc->sem.semId, MyProc->sem.semNum);

	/* Add PROC struct to freelist so space can be recycled in future */
	MyProc->links.next = ProcGlobal->freeProcs;
	ProcGlobal->freeProcs = MAKE_OFFSET(MyProc);

	/* PROC struct isn't mine anymore */
	MyProc = NULL;

	SpinLockRelease(ProcStructLock);
}

/*
 * DummyProcKill() -- Cut-down version of ProcKill for dummy (checkpoint)
 *		processes.	The PROC and sema are not released, only marked
 *		as not-in-use.
 */
static void
DummyProcKill(void)
{
	Assert(MyProc != NULL && MyProc == DummyProc);

	/* Release any LW locks I am holding */
	LWLockReleaseAll();

	/* Abort any buffer I/O in progress */
	AbortBufferIO();

	/* I can't be on regular lock queues, so needn't check */

	/* Mark DummyProc no longer in use */
	MyProc->pid = 0;

	/* PROC struct isn't mine anymore */
	MyProc = NULL;
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
 * ProcSleep -- put a process to sleep
 *
 * Caller must have set MyProc->heldLocks to reflect locks already held
 * on the lockable object by this process (under all XIDs).
 *
 * Locktable's masterLock must be held at entry, and will be held
 * at exit.
 *
 * Result: STATUS_OK if we acquired the lock, STATUS_ERROR if not (deadlock).
 *
 * ASSUME: that no one will fiddle with the queue until after
 *		we release the masterLock.
 *
 * NOTES: The process queue is now a priority queue for locking.
 *
 * P() on the semaphore should put us to sleep.  The process
 * semaphore is normally zero, so when we try to acquire it, we sleep.
 */
int
ProcSleep(LOCKMETHODTABLE *lockMethodTable,
		  LOCKMODE lockmode,
		  LOCK *lock,
		  HOLDER *holder)
{
	LOCKMETHODCTL *lockctl = lockMethodTable->ctl;
	LWLockId	masterLock = lockctl->masterLock;
	PROC_QUEUE *waitQueue = &(lock->waitProcs);
	int			myHeldLocks = MyProc->heldLocks;
	bool		early_deadlock = false;
	PROC	   *proc;
	int			i;

	/*
	 * Determine where to add myself in the wait queue.
	 *
	 * Normally I should go at the end of the queue.  However, if I already
	 * hold locks that conflict with the request of any previous waiter,
	 * put myself in the queue just in front of the first such waiter.
	 * This is not a necessary step, since deadlock detection would move
	 * me to before that waiter anyway; but it's relatively cheap to
	 * detect such a conflict immediately, and avoid delaying till
	 * deadlock timeout.
	 *
	 * Special case: if I find I should go in front of some waiter, check to
	 * see if I conflict with already-held locks or the requests before
	 * that waiter.  If not, then just grant myself the requested lock
	 * immediately.  This is the same as the test for immediate grant in
	 * LockAcquire, except we are only considering the part of the wait
	 * queue before my insertion point.
	 */
	if (myHeldLocks != 0)
	{
		int			aheadRequests = 0;

		proc = (PROC *) MAKE_PTR(waitQueue->links.next);
		for (i = 0; i < waitQueue->size; i++)
		{
			/* Must he wait for me? */
			if (lockctl->conflictTab[proc->waitLockMode] & myHeldLocks)
			{
				/* Must I wait for him ? */
				if (lockctl->conflictTab[lockmode] & proc->heldLocks)
				{
					/*
					 * Yes, so we have a deadlock.	Easiest way to clean
					 * up correctly is to call RemoveFromWaitQueue(), but
					 * we can't do that until we are *on* the wait queue.
					 * So, set a flag to check below, and break out of
					 * loop.
					 */
					early_deadlock = true;
					break;
				}
				/* I must go before this waiter.  Check special case. */
				if ((lockctl->conflictTab[lockmode] & aheadRequests) == 0 &&
					LockCheckConflicts(lockMethodTable,
									   lockmode,
									   lock,
									   holder,
									   MyProc,
									   NULL) == STATUS_OK)
				{
					/* Skip the wait and just grant myself the lock. */
					GrantLock(lock, holder, lockmode);
					return STATUS_OK;
				}
				/* Break out of loop to put myself before him */
				break;
			}
			/* Nope, so advance to next waiter */
			aheadRequests |= (1 << proc->waitLockMode);
			proc = (PROC *) MAKE_PTR(proc->links.next);
		}

		/*
		 * If we fall out of loop normally, proc points to waitQueue head,
		 * so we will insert at tail of queue as desired.
		 */
	}
	else
	{
		/* I hold no locks, so I can't push in front of anyone. */
		proc = (PROC *) &(waitQueue->links);
	}

	/*
	 * Insert self into queue, ahead of the given proc (or at tail of
	 * queue).
	 */
	SHMQueueInsertBefore(&(proc->links), &(MyProc->links));
	waitQueue->size++;

	lock->waitMask |= (1 << lockmode);

	/* Set up wait information in PROC object, too */
	MyProc->waitLock = lock;
	MyProc->waitHolder = holder;
	MyProc->waitLockMode = lockmode;

	MyProc->errType = STATUS_OK;	/* initialize result for success */

	/*
	 * If we detected deadlock, give up without waiting.  This must agree
	 * with HandleDeadLock's recovery code, except that we shouldn't
	 * release the semaphore since we haven't tried to lock it yet.
	 */
	if (early_deadlock)
	{
		RemoveFromWaitQueue(MyProc);
		MyProc->errType = STATUS_ERROR;
		return STATUS_ERROR;
	}

	/* mark that we are waiting for a lock */
	waitingForLock = true;

	/*
	 * Release the locktable's masterLock.
	 *
	 * NOTE: this may also cause us to exit critical-section state, possibly
	 * allowing a cancel/die interrupt to be accepted. This is OK because
	 * we have recorded the fact that we are waiting for a lock, and so
	 * LockWaitCancel will clean up if cancel/die happens.
	 */
	LWLockRelease(masterLock);

	/*
	 * Set timer so we can wake up after awhile and check for a deadlock.
	 * If a deadlock is detected, the handler releases the process's
	 * semaphore and sets MyProc->errType = STATUS_ERROR, allowing us to
	 * know that we must report failure rather than success.
	 *
	 * By delaying the check until we've waited for a bit, we can avoid
	 * running the rather expensive deadlock-check code in most cases.
	 */
	if (!enable_sigalrm_interrupt(DeadlockTimeout))
		elog(FATAL, "ProcSleep: Unable to set timer for process wakeup");

	/*
	 * If someone wakes us between LWLockRelease and IpcSemaphoreLock,
	 * IpcSemaphoreLock will not block.  The wakeup is "saved" by the
	 * semaphore implementation.  Note also that if HandleDeadLock is
	 * invoked but does not detect a deadlock, IpcSemaphoreLock() will
	 * continue to wait.  There used to be a loop here, but it was useless
	 * code...
	 *
	 * We pass interruptOK = true, which eliminates a window in which
	 * cancel/die interrupts would be held off undesirably.  This is a
	 * promise that we don't mind losing control to a cancel/die interrupt
	 * here.  We don't, because we have no state-change work to do after
	 * being granted the lock (the grantor did it all).
	 */
	IpcSemaphoreLock(MyProc->sem.semId, MyProc->sem.semNum, true);

	/*
	 * Disable the timer, if it's still running
	 */
	if (!disable_sigalrm_interrupt())
		elog(FATAL, "ProcSleep: Unable to disable timer for process wakeup");

	/*
	 * Now there is nothing for LockWaitCancel to do.
	 */
	waitingForLock = false;

	/*
	 * Re-acquire the locktable's masterLock.
	 */
	LWLockAcquire(masterLock, LW_EXCLUSIVE);

	/*
	 * We don't have to do anything else, because the awaker did all the
	 * necessary update of the lock table and MyProc.
	 */
	return MyProc->errType;
}


/*
 * ProcWakeup -- wake up a process by releasing its private semaphore.
 *
 *	 Also remove the process from the wait queue and set its links invalid.
 *	 RETURN: the next process in the wait queue.
 *
 * XXX: presently, this code is only used for the "success" case, and only
 * works correctly for that case.  To clean up in failure case, would need
 * to twiddle the lock's request counts too --- see RemoveFromWaitQueue.
 */
PROC *
ProcWakeup(PROC *proc, int errType)
{
	PROC	   *retProc;

	/* assume that masterLock has been acquired */

	/* Proc should be sleeping ... */
	if (proc->links.prev == INVALID_OFFSET ||
		proc->links.next == INVALID_OFFSET)
		return (PROC *) NULL;

	/* Save next process before we zap the list link */
	retProc = (PROC *) MAKE_PTR(proc->links.next);

	/* Remove process from wait queue */
	SHMQueueDelete(&(proc->links));
	(proc->waitLock->waitProcs.size)--;

	/* Clean up process' state and pass it the ok/fail signal */
	proc->waitLock = NULL;
	proc->waitHolder = NULL;
	proc->errType = errType;

	/* And awaken it */
	IpcSemaphoreUnlock(proc->sem.semId, proc->sem.semNum);

	return retProc;
}

/*
 * ProcLockWakeup -- routine for waking up processes when a lock is
 *		released (or a prior waiter is aborted).  Scan all waiters
 *		for lock, waken any that are no longer blocked.
 */
void
ProcLockWakeup(LOCKMETHODTABLE *lockMethodTable, LOCK *lock)
{
	LOCKMETHODCTL *lockctl = lockMethodTable->ctl;
	PROC_QUEUE *waitQueue = &(lock->waitProcs);
	int			queue_size = waitQueue->size;
	PROC	   *proc;
	int			aheadRequests = 0;

	Assert(queue_size >= 0);

	if (queue_size == 0)
		return;

	proc = (PROC *) MAKE_PTR(waitQueue->links.next);

	while (queue_size-- > 0)
	{
		LOCKMODE	lockmode = proc->waitLockMode;

		/*
		 * Waken if (a) doesn't conflict with requests of earlier waiters,
		 * and (b) doesn't conflict with already-held locks.
		 */
		if ((lockctl->conflictTab[lockmode] & aheadRequests) == 0 &&
			LockCheckConflicts(lockMethodTable,
							   lockmode,
							   lock,
							   proc->waitHolder,
							   proc,
							   NULL) == STATUS_OK)
		{
			/* OK to waken */
			GrantLock(lock, proc->waitHolder, lockmode);
			proc = ProcWakeup(proc, STATUS_OK);

			/*
			 * ProcWakeup removes proc from the lock's waiting process
			 * queue and returns the next proc in chain; don't use proc's
			 * next-link, because it's been cleared.
			 */
		}
		else
		{
			/*
			 * Cannot wake this guy. Remember his request for later
			 * checks.
			 */
			aheadRequests |= (1 << lockmode);
			proc = (PROC *) MAKE_PTR(proc->links.next);
		}
	}

	Assert(waitQueue->size >= 0);
}

/* --------------------
 * We only get to this routine if we got SIGALRM after DeadlockTimeout
 * while waiting for a lock to be released by some other process.  Look
 * to see if there's a deadlock; if not, just return and continue waiting.
 * If we have a real deadlock, remove ourselves from the lock's wait queue
 * and signal an error to ProcSleep.
 * --------------------
 */
void
HandleDeadLock(SIGNAL_ARGS)
{
	int			save_errno = errno;

	/*
	 * Acquire locktable lock.	Note that the SIGALRM interrupt had better
	 * not be enabled anywhere that this process itself holds the
	 * locktable lock, else this will wait forever.  Also note that
	 * LWLockAcquire creates a critical section, so that this routine
	 * cannot be interrupted by cancel/die interrupts.
	 */
	LWLockAcquire(LockMgrLock, LW_EXCLUSIVE);

	/*
	 * Check to see if we've been awoken by anyone in the interim.
	 *
	 * If we have we can return and resume our transaction -- happy day.
	 * Before we are awoken the process releasing the lock grants it to us
	 * so we know that we don't have to wait anymore.
	 *
	 * We check by looking to see if we've been unlinked from the wait queue.
	 * This is quicker than checking our semaphore's state, since no
	 * kernel call is needed, and it is safe because we hold the locktable
	 * lock.
	 *
	 */
	if (MyProc->links.prev == INVALID_OFFSET ||
		MyProc->links.next == INVALID_OFFSET)
	{
		LWLockRelease(LockMgrLock);
		errno = save_errno;
		return;
	}

#ifdef LOCK_DEBUG
	if (Debug_deadlocks)
		DumpAllLocks();
#endif

	if (!DeadLockCheck(MyProc))
	{
		/* No deadlock, so keep waiting */
		LWLockRelease(LockMgrLock);
		errno = save_errno;
		return;
	}

	/*
	 * Oops.  We have a deadlock.
	 *
	 * Get this process out of wait state.
	 */
	RemoveFromWaitQueue(MyProc);

	/*
	 * Set MyProc->errType to STATUS_ERROR so that ProcSleep will report
	 * an error after we return from this signal handler.
	 */
	MyProc->errType = STATUS_ERROR;

	/*
	 * Unlock my semaphore so that the interrupted ProcSleep() call can
	 * finish.
	 */
	IpcSemaphoreUnlock(MyProc->sem.semId, MyProc->sem.semNum);

	/*
	 * We're done here.  Transaction abort caused by the error that
	 * ProcSleep will raise will cause any other locks we hold to be
	 * released, thus allowing other processes to wake up; we don't need
	 * to do that here. NOTE: an exception is that releasing locks we hold
	 * doesn't consider the possibility of waiters that were blocked
	 * behind us on the lock we just failed to get, and might now be
	 * wakable because we're not in front of them anymore.  However,
	 * RemoveFromWaitQueue took care of waking up any such processes.
	 */
	LWLockRelease(LockMgrLock);
	errno = save_errno;
}


/*
 * ProcWaitForSignal - wait for a signal from another backend.
 *
 * This can share the semaphore normally used for waiting for locks,
 * since a backend could never be waiting for a lock and a signal at
 * the same time.  As with locks, it's OK if the signal arrives just
 * before we actually reach the waiting state.
 */
void
ProcWaitForSignal(void)
{
	waitingForSignal = true;
	IpcSemaphoreLock(MyProc->sem.semId, MyProc->sem.semNum, true);
	waitingForSignal = false;
}

/*
 * ProcCancelWaitForSignal - clean up an aborted wait for signal
 *
 * We need this in case the signal arrived after we aborted waiting,
 * or if it arrived but we never reached ProcWaitForSignal() at all.
 * Caller should call this after resetting the signal request status.
 */
void
ProcCancelWaitForSignal(void)
{
	ZeroProcSemaphore(MyProc);
	waitingForSignal = false;
}

/*
 * ProcSendSignal - send a signal to a backend identified by BackendId
 */
void
ProcSendSignal(BackendId procId)
{
	PROC	   *proc = BackendIdGetProc(procId);

	if (proc != NULL)
		IpcSemaphoreUnlock(proc->sem.semId, proc->sem.semNum);
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
 * Returns TRUE if okay, FALSE on failure.
 */
bool
enable_sigalrm_interrupt(int delayms)
{
#ifndef __BEOS__
	struct itimerval timeval,
				dummy;

	MemSet(&timeval, 0, sizeof(struct itimerval));
	timeval.it_value.tv_sec = delayms / 1000;
	timeval.it_value.tv_usec = (delayms % 1000) * 1000;
	if (setitimer(ITIMER_REAL, &timeval, &dummy))
		return false;
#else
	/* BeOS doesn't have setitimer, but has set_alarm */
	bigtime_t	time_interval;

	time_interval = delayms * 1000;		/* usecs */
	if (set_alarm(time_interval, B_ONE_SHOT_RELATIVE_ALARM) < 0)
		return false;
#endif

	return true;
}

/*
 * Disable the SIGALRM interrupt, if it has not yet fired
 *
 * Returns TRUE if okay, FALSE on failure.
 */
bool
disable_sigalrm_interrupt(void)
{
#ifndef __BEOS__
	struct itimerval timeval,
				dummy;

	MemSet(&timeval, 0, sizeof(struct itimerval));
	if (setitimer(ITIMER_REAL, &timeval, &dummy))
		return false;
#else
	/* BeOS doesn't have setitimer, but has set_alarm */
	if (set_alarm(B_INFINITE_TIMEOUT, B_PERIODIC_ALARM) < 0)
		return false;
#endif

	return true;
}


/*****************************************************************************
 *
 *****************************************************************************/

/*
 * ProcGetNewSemIdAndNum -
 *	  scan the free semaphore bitmap and allocate a single semaphore from
 *	  a semaphore set.
 */
static void
ProcGetNewSemIdAndNum(IpcSemaphoreId *semId, int *semNum)
{
	int			i;
	int			semMapEntries = ProcGlobal->semMapEntries;
	SEM_MAP_ENTRY *procSemMap = ProcGlobal->procSemMap;
	int32		fullmask = (1 << PROC_NSEMS_PER_SET) - 1;

	SpinLockAcquire(ProcStructLock);

	for (i = 0; i < semMapEntries; i++)
	{
		int			mask = 1;
		int			j;

		if (procSemMap[i].freeSemMap == fullmask)
			continue;			/* this set is fully allocated */
		if (procSemMap[i].procSemId < 0)
			continue;			/* this set hasn't been initialized */

		for (j = 0; j < PROC_NSEMS_PER_SET; j++)
		{
			if ((procSemMap[i].freeSemMap & mask) == 0)
			{
				/* A free semaphore found. Mark it as allocated. */
				procSemMap[i].freeSemMap |= mask;

				*semId = procSemMap[i].procSemId;
				*semNum = j;

				SpinLockRelease(ProcStructLock);

				return;
			}
			mask <<= 1;
		}
	}

	SpinLockRelease(ProcStructLock);

	/*
	 * If we reach here, all the semaphores are in use.  This is one of
	 * the possible places to detect "too many backends", so give the
	 * standard error message.	(Whether we detect it here or in sinval.c
	 * depends on whether MaxBackends is a multiple of
	 * PROC_NSEMS_PER_SET.)
	 */
	elog(FATAL, "Sorry, too many clients already");
}

/*
 * ProcFreeSem -
 *	  free up our semaphore in the semaphore set.
 *
 * Caller is assumed to hold ProcStructLock.
 */
static void
ProcFreeSem(IpcSemaphoreId semId, int semNum)
{
	int32		mask;
	int			i;
	int			semMapEntries = ProcGlobal->semMapEntries;

	mask = ~(1 << semNum);

	for (i = 0; i < semMapEntries; i++)
	{
		if (ProcGlobal->procSemMap[i].procSemId == semId)
		{
			ProcGlobal->procSemMap[i].freeSemMap &= mask;
			return;
		}
	}
	/* can't elog here!!! */
	fprintf(stderr, "ProcFreeSem: no ProcGlobal entry for semId %d\n", semId);
}

/*
 * ProcFreeAllSemaphores -
 *	  called at shmem_exit time, ie when exiting the postmaster or
 *	  destroying shared state for a failed set of backends.
 *	  Free up all the semaphores allocated to the lmgrs of the backends.
 */
static void
ProcFreeAllSemaphores(void)
{
	int			i;

	for (i = 0; i < ProcGlobal->semMapEntries; i++)
	{
		if (ProcGlobal->procSemMap[i].procSemId >= 0)
			IpcSemaphoreKill(ProcGlobal->procSemMap[i].procSemId);
	}
}
