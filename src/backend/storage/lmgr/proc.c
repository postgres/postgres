/*-------------------------------------------------------------------------
 *
 * proc.c
 *	  routines to manage per-process shared memory data structure
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/lmgr/proc.c,v 1.136 2003/10/16 20:59:35 tgl Exp $
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
 */
#include "postgres.h"

#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>

#include "miscadmin.h"
#include "access/xact.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/sinval.h"
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
static slock_t *ProcStructLock = NULL;

static PROC_HDR *ProcGlobal = NULL;

static PGPROC *DummyProc = NULL;

static bool waitingForLock = false;
static bool waitingForSignal = false;

/* Mark these volatile because they can be changed by signal handler */
static volatile bool statement_timeout_active = false;
static volatile bool deadlock_timeout_active = false;

/* statement_fin_time is valid only if statement_timeout_active is true */
static struct timeval statement_fin_time;


static void ProcKill(void);
static void DummyProcKill(void);
static bool CheckStatementTimeout(void);


/*
 * Report number of semaphores needed by InitProcGlobal.
 */
int
ProcGlobalSemas(int maxBackends)
{
	/* We need a sema per backend, plus one for the dummy process. */
	return maxBackends + 1;
}

/*
 * InitProcGlobal -
 *	  initializes the global process table. We put it here so that
 *	  the postmaster can do this initialization.
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
 */
void
InitProcGlobal(int maxBackends)
{
	bool		found = false;

	/* Create or attach to the ProcGlobal shared structure */
	ProcGlobal = (PROC_HDR *)
		ShmemInitStruct("Proc Header", sizeof(PROC_HDR), &found);

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

		/*
		 * Pre-create the PGPROC structures and create a semaphore for
		 * each.
		 */
		for (i = 0; i < maxBackends; i++)
		{
			PGPROC	   *proc;

			proc = (PGPROC *) ShmemAlloc(sizeof(PGPROC));
			if (!proc)
				ereport(FATAL,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of shared memory")));
			MemSet(proc, 0, sizeof(PGPROC));
			PGSemaphoreCreate(&proc->sem);
			proc->links.next = ProcGlobal->freeProcs;
			ProcGlobal->freeProcs = MAKE_OFFSET(proc);
		}

		/*
		 * Pre-allocate a PGPROC structure for dummy (checkpoint)
		 * processes, too.	This does not get linked into the freeProcs
		 * list.
		 */
		DummyProc = (PGPROC *) ShmemAlloc(sizeof(PGPROC));
		if (!DummyProc)
			ereport(FATAL,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of shared memory")));
		MemSet(DummyProc, 0, sizeof(PGPROC));
		DummyProc->pid = 0;		/* marks DummyProc as not in use */
		PGSemaphoreCreate(&DummyProc->sem);

		/* Create ProcStructLock spinlock, too */
		ProcStructLock = (slock_t *) ShmemAlloc(sizeof(slock_t));
		SpinLockInit(ProcStructLock);
	}
}

/*
 * InitProcess -- initialize a per-process data structure for this backend
 */
void
InitProcess(void)
{
	SHMEM_OFFSET myOffset;

	/* use volatile pointer to prevent code rearrangement */
	volatile PROC_HDR *procglobal = ProcGlobal;

	/*
	 * ProcGlobal should be set by a previous call to InitProcGlobal (if
	 * we are a backend, we inherit this by fork() from the postmaster).
	 */
	if (procglobal == NULL)
		elog(PANIC, "proc header uninitialized");

	if (MyProc != NULL)
		elog(ERROR, "you already exist");

	/*
	 * Try to get a proc struct from the free list.  If this fails, we
	 * must be out of PGPROC structures (not to mention semaphores).
	 */
	SpinLockAcquire(ProcStructLock);

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
		 * If we reach here, all the PGPROCs are in use.  This is one of
		 * the possible places to detect "too many backends", so give the
		 * standard error message.
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
	 * We might be reusing a semaphore that belonged to a failed process.
	 * So be careful and reinitialize its value here.
	 */
	PGSemaphoreReset(&MyProc->sem);

	/*
	 * Now that we have a PGPROC, we could try to acquire locks, so
	 * initialize the deadlock checker.
	 */
	InitDeadLockChecking();
}

/*
 * InitDummyProcess -- create a dummy per-process data structure
 *
 * This is called by checkpoint processes so that they will have a MyProc
 * value that's real enough to let them wait for LWLocks.  The PGPROC and
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
		elog(PANIC, "proc header uninitialized");

	if (MyProc != NULL)
		elog(ERROR, "you already exist");

	/*
	 * DummyProc should not presently be in use by anyone else
	 */
	if (DummyProc->pid != 0)
		elog(FATAL, "DummyProc is in use by PID %d", DummyProc->pid);
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
	PGSemaphoreReset(&MyProc->sem);
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
	/* Nothing to do if we weren't waiting for a lock */
	if (!waitingForLock)
		return false;

	waitingForLock = false;

	/* Turn off the deadlock timer, if it's still running (see ProcSleep) */
	disable_sig_alarm(false);

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
	PGSemaphoreReset(&MyProc->sem);

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
	/* use volatile pointer to prevent code rearrangement */
	volatile PROC_HDR *procglobal = ProcGlobal;

	Assert(MyProc != NULL);

	/* Release any LW locks I am holding */
	LWLockReleaseAll();

	/*
	 * Make real sure we release any buffer locks and pins we might be
	 * holding, too.  It is pretty ugly to do this here and not in a
	 * shutdown callback registered by the bufmgr ... but we must do this
	 * *after* LWLockReleaseAll and *before* zapping MyProc.
	 */
	AbortBufferIO();
	UnlockBuffers();
	AtEOXact_Buffers(false);

	/* Get off any wait queue I might be on */
	LockWaitCancel();

	/* Remove from the standard lock table */
	LockReleaseAll(DEFAULT_LOCKMETHOD, MyProc, true, InvalidTransactionId);

#ifdef USER_LOCKS
	/* Remove from the user lock table */
	LockReleaseAll(USER_LOCKMETHOD, MyProc, true, InvalidTransactionId);
#endif

	SpinLockAcquire(ProcStructLock);

	/* Return PGPROC structure (and semaphore) to freelist */
	MyProc->links.next = procglobal->freeProcs;
	procglobal->freeProcs = MAKE_OFFSET(MyProc);

	/* PGPROC struct isn't mine anymore */
	MyProc = NULL;

	SpinLockRelease(ProcStructLock);
}

/*
 * DummyProcKill() -- Cut-down version of ProcKill for dummy (checkpoint)
 *		processes.	The PGPROC and sema are not released, only marked
 *		as not-in-use.
 */
static void
DummyProcKill(void)
{
	Assert(MyProc != NULL && MyProc == DummyProc);

	/* Release any LW locks I am holding */
	LWLockReleaseAll();

	/* Release buffer locks and pins, too */
	AbortBufferIO();
	UnlockBuffers();
	AtEOXact_Buffers(false);

	/* I can't be on regular lock queues, so needn't check */

	/* Mark DummyProc no longer in use */
	MyProc->pid = 0;

	/* PGPROC struct isn't mine anymore */
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
		  PROCLOCK *proclock)
{
	LWLockId	masterLock = lockMethodTable->masterLock;
	PROC_QUEUE *waitQueue = &(lock->waitProcs);
	int			myHeldLocks = MyProc->heldLocks;
	bool		early_deadlock = false;
	PGPROC	   *proc;
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
					 * Yes, so we have a deadlock.	Easiest way to clean
					 * up correctly is to call RemoveFromWaitQueue(), but
					 * we can't do that until we are *on* the wait queue.
					 * So, set a flag to check below, and break out of
					 * loop.  Also, record deadlock info for later
					 * message.
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
									   MyProc,
									   NULL) == STATUS_OK)
				{
					/* Skip the wait and just grant myself the lock. */
					GrantLock(lock, proclock, lockmode);
					return STATUS_OK;
				}
				/* Break out of loop to put myself before him */
				break;
			}
			/* Nope, so advance to next waiter */
			aheadRequests |= (1 << proc->waitLockMode);
			proc = (PGPROC *) MAKE_PTR(proc->links.next);
		}

		/*
		 * If we fall out of loop normally, proc points to waitQueue head,
		 * so we will insert at tail of queue as desired.
		 */
	}
	else
	{
		/* I hold no locks, so I can't push in front of anyone. */
		proc = (PGPROC *) &(waitQueue->links);
	}

	/*
	 * Insert self into queue, ahead of the given proc (or at tail of
	 * queue).
	 */
	SHMQueueInsertBefore(&(proc->links), &(MyProc->links));
	waitQueue->size++;

	lock->waitMask |= (1 << lockmode);

	/* Set up wait information in PGPROC object, too */
	MyProc->waitLock = lock;
	MyProc->waitHolder = proclock;
	MyProc->waitLockMode = lockmode;

	MyProc->errType = STATUS_OK;	/* initialize result for success */

	/*
	 * If we detected deadlock, give up without waiting.  This must agree
	 * with CheckDeadLock's recovery code, except that we shouldn't
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
	if (!enable_sig_alarm(DeadlockTimeout, false))
		elog(FATAL, "could not set timer for process wakeup");

	/*
	 * If someone wakes us between LWLockRelease and PGSemaphoreLock,
	 * PGSemaphoreLock will not block.	The wakeup is "saved" by the
	 * semaphore implementation.  Note also that if CheckDeadLock is
	 * invoked but does not detect a deadlock, PGSemaphoreLock() will
	 * continue to wait.  There used to be a loop here, but it was useless
	 * code...
	 *
	 * We pass interruptOK = true, which eliminates a window in which
	 * cancel/die interrupts would be held off undesirably.  This is a
	 * promise that we don't mind losing control to a cancel/die interrupt
	 * here.  We don't, because we have no state-change work to do after
	 * being granted the lock (the grantor did it all).
	 */
	PGSemaphoreLock(&MyProc->sem, true);

	/*
	 * Disable the timer, if it's still running
	 */
	if (!disable_sig_alarm(false))
		elog(FATAL, "could not disable timer for process wakeup");

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
PGPROC *
ProcWakeup(PGPROC *proc, int errType)
{
	PGPROC	   *retProc;

	/* assume that masterLock has been acquired */

	/* Proc should be sleeping ... */
	if (proc->links.prev == INVALID_OFFSET ||
		proc->links.next == INVALID_OFFSET)
		return (PGPROC *) NULL;

	/* Save next process before we zap the list link */
	retProc = (PGPROC *) MAKE_PTR(proc->links.next);

	/* Remove process from wait queue */
	SHMQueueDelete(&(proc->links));
	(proc->waitLock->waitProcs.size)--;

	/* Clean up process' state and pass it the ok/fail signal */
	proc->waitLock = NULL;
	proc->waitHolder = NULL;
	proc->errType = errType;

	/* And awaken it */
	PGSemaphoreUnlock(&proc->sem);

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
	PROC_QUEUE *waitQueue = &(lock->waitProcs);
	int			queue_size = waitQueue->size;
	PGPROC	   *proc;
	int			aheadRequests = 0;

	Assert(queue_size >= 0);

	if (queue_size == 0)
		return;

	proc = (PGPROC *) MAKE_PTR(waitQueue->links.next);

	while (queue_size-- > 0)
	{
		LOCKMODE	lockmode = proc->waitLockMode;

		/*
		 * Waken if (a) doesn't conflict with requests of earlier waiters,
		 * and (b) doesn't conflict with already-held locks.
		 */
		if ((lockMethodTable->conflictTab[lockmode] & aheadRequests) == 0 &&
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
			proc = (PGPROC *) MAKE_PTR(proc->links.next);
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
static void
CheckDeadLock(void)
{
	/*
	 * Acquire locktable lock.	Note that the deadlock check interrupt had
	 * better not be enabled anywhere that this process itself holds the
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
	 */
	if (MyProc->links.prev == INVALID_OFFSET ||
		MyProc->links.next == INVALID_OFFSET)
	{
		LWLockRelease(LockMgrLock);
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
	 * an error after we return from the signal handler.
	 */
	MyProc->errType = STATUS_ERROR;

	/*
	 * Unlock my semaphore so that the interrupted ProcSleep() call can
	 * finish.
	 */
	PGSemaphoreUnlock(&MyProc->sem);

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
	PGSemaphoreLock(&MyProc->sem, true);
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
	PGSemaphoreReset(&MyProc->sem);
	waitingForSignal = false;
}

/*
 * ProcSendSignal - send a signal to a backend identified by BackendId
 */
void
ProcSendSignal(BackendId procId)
{
	PGPROC	   *proc = BackendIdGetProc(procId);

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
#ifdef WIN32
#warning add Win32 timer
#else
	struct timeval fin_time;

#ifndef __BEOS__
	struct itimerval timeval;

#else
	bigtime_t	time_interval;
#endif

	/* Compute target timeout time if we will need it */
	if (is_statement_timeout || statement_timeout_active)
	{
		gettimeofday(&fin_time, NULL);
		fin_time.tv_sec += delayms / 1000;
		fin_time.tv_usec += (delayms % 1000) * 1000;
		if (fin_time.tv_usec >= 1000000)
		{
			fin_time.tv_sec++;
			fin_time.tv_usec -= 1000000;
		}
	}

	if (is_statement_timeout)
	{
		/* Begin statement-level timeout */
		Assert(!deadlock_timeout_active);
		statement_fin_time = fin_time;
		statement_timeout_active = true;
	}
	else if (statement_timeout_active)
	{
		/*
		 * Begin deadlock timeout with statement-level timeout active
		 *
		 * Here, we want to interrupt at the closer of the two timeout times.
		 * If fin_time >= statement_fin_time then we need not touch the
		 * existing timer setting; else set up to interrupt at the
		 * deadlock timeout time.
		 *
		 * NOTE: in this case it is possible that this routine will be
		 * interrupted by the previously-set timer alarm.  This is okay
		 * because the signal handler will do only what it should do
		 * according to the state variables.  The deadlock checker may get
		 * run earlier than normal, but that does no harm.
		 */
		deadlock_timeout_active = true;
		if (fin_time.tv_sec > statement_fin_time.tv_sec ||
			(fin_time.tv_sec == statement_fin_time.tv_sec &&
			 fin_time.tv_usec >= statement_fin_time.tv_usec))
			return true;
	}
	else
	{
		/* Begin deadlock timeout with no statement-level timeout */
		deadlock_timeout_active = true;
	}

	/* If we reach here, okay to set the timer interrupt */
#ifndef __BEOS__
	MemSet(&timeval, 0, sizeof(struct itimerval));
	timeval.it_value.tv_sec = delayms / 1000;
	timeval.it_value.tv_usec = (delayms % 1000) * 1000;
	if (setitimer(ITIMER_REAL, &timeval, NULL))
		return false;
#else
	/* BeOS doesn't have setitimer, but has set_alarm */
	time_interval = delayms * 1000;		/* usecs */
	if (set_alarm(time_interval, B_ONE_SHOT_RELATIVE_ALARM) < 0)
		return false;
#endif
#endif
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
#ifdef WIN32
#warning add Win32 timer
#else

	/*
	 * Always disable the interrupt if it is active; this avoids being
	 * interrupted by the signal handler and thereby possibly getting
	 * confused.
	 *
	 * We will re-enable the interrupt if necessary in CheckStatementTimeout.
	 */
	if (statement_timeout_active || deadlock_timeout_active)
	{
#ifndef __BEOS__
		struct itimerval timeval;

		MemSet(&timeval, 0, sizeof(struct itimerval));
		if (setitimer(ITIMER_REAL, &timeval, NULL))
		{
			statement_timeout_active = deadlock_timeout_active = false;
			return false;
		}
#else
		/* BeOS doesn't have setitimer, but has set_alarm */
		if (set_alarm(B_INFINITE_TIMEOUT, B_PERIODIC_ALARM) < 0)
		{
			statement_timeout_active = deadlock_timeout_active = false;
			return false;
		}
#endif
	}

	/* Always cancel deadlock timeout, in case this is error cleanup */
	deadlock_timeout_active = false;

	/* Cancel or reschedule statement timeout */
	if (is_statement_timeout)
		statement_timeout_active = false;
	else if (statement_timeout_active)
	{
		if (!CheckStatementTimeout())
			return false;
	}
#endif
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
	struct timeval now;

	if (!statement_timeout_active)
		return true;			/* do nothing if not active */

	gettimeofday(&now, NULL);

	if (now.tv_sec > statement_fin_time.tv_sec ||
		(now.tv_sec == statement_fin_time.tv_sec &&
		 now.tv_usec >= statement_fin_time.tv_usec))
	{
		/* Time to die */
		statement_timeout_active = false;
		kill(MyProcPid, SIGINT);
	}
	else
	{
		/* Not time yet, so (re)schedule the interrupt */
#ifdef WIN32
#warning add win32 timer
#else
#ifndef __BEOS__
		struct itimerval timeval;

		MemSet(&timeval, 0, sizeof(struct itimerval));
		timeval.it_value.tv_sec = statement_fin_time.tv_sec - now.tv_sec;
		timeval.it_value.tv_usec = statement_fin_time.tv_usec - now.tv_usec;
		if (timeval.it_value.tv_usec < 0)
		{
			timeval.it_value.tv_sec--;
			timeval.it_value.tv_usec += 1000000;
		}
		if (setitimer(ITIMER_REAL, &timeval, NULL))
			return false;
#else
		/* BeOS doesn't have setitimer, but has set_alarm */
		bigtime_t	time_interval;

		time_interval =
			(statement_fin_time.tv_sec - now.tv_sec) * 1000000 +
			(statement_fin_time.tv_usec - now.tv_usec);
		if (set_alarm(time_interval, B_ONE_SHOT_RELATIVE_ALARM) < 0)
			return false;
#endif
#endif
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
