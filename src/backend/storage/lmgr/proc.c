/*-------------------------------------------------------------------------
 *
 * proc.c
 *	  routines to manage per-process shared memory data structure
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/lmgr/proc.c,v 1.87 2000/12/18 00:44:47 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 *	Each postgres backend gets one of these.  We'll use it to
 *	clean up after the process should the process suddenly die.
 *
 *
 * Interface (a):
 *		ProcSleep(), ProcWakeup(), ProcWakeupNext(),
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
 *
 * $Header: /cvsroot/pgsql/src/backend/storage/lmgr/proc.c,v 1.87 2000/12/18 00:44:47 tgl Exp $
 */
#include "postgres.h"

#include <sys/time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>

#if defined(solaris_sparc) || defined(__CYGWIN__)
#include <sys/ipc.h>
#include <sys/sem.h>
#endif

#include "miscadmin.h"

#if defined(__darwin__)
#include "port/darwin/sem.h"
#endif

/* In Ultrix and QNX, sem.h must be included after ipc.h */
#ifdef HAVE_SYS_SEM_H
#include <sys/sem.h>
#endif

#include "storage/proc.h"



void		HandleDeadLock(SIGNAL_ARGS);
static void ProcFreeAllSemaphores(void);
static bool GetOffWaitqueue(PROC *);

int DeadlockTimeout = 1000;

/* --------------------
 * Spin lock for manipulating the shared process data structure:
 * ProcGlobal.... Adding an extra spin lock seemed like the smallest
 * hack to get around reading and updating this structure in shared
 * memory. -mer 17 July 1991
 * --------------------
 */
SPINLOCK	ProcStructLock;

static PROC_HDR *ProcGlobal = NULL;

PROC	   *MyProc = NULL;

static void ProcKill(int exitStatus, Datum pid);
static void ProcGetNewSemIdAndNum(IpcSemaphoreId *semId, int *semNum);
static void ProcFreeSem(IpcSemaphoreId semId, int semNum);

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
	bool		found = false;

	/* attach to the free list */
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
		for (i = 0; i < PROC_SEM_MAP_ENTRIES; i++)
		{
			ProcGlobal->procSemIds[i] = -1;
			ProcGlobal->freeSemMap[i] = 0;
		}

		/*
		 * Arrange to delete semas on exit --- set this up now so that we
		 * will clean up if pre-allocation fails.  We use our own freeproc,
		 * rather than IpcSemaphoreCreate's removeOnExit option, because
		 * we don't want to fill up the on_shmem_exit list with a separate
		 * entry for each semaphore set.
		 */
		on_shmem_exit(ProcFreeAllSemaphores, 0);

		/*
		 * Pre-create the semaphores for the first maxBackends processes.
		 */
		Assert(maxBackends > 0 && maxBackends <= MAXBACKENDS);

		for (i = 0; i < ((maxBackends-1)/PROC_NSEMS_PER_SET+1); i++)
		{
			IpcSemaphoreId		semId;

			semId = IpcSemaphoreCreate(PROC_NSEMS_PER_SET,
									   IPCProtection,
									   1,
									   false);
			ProcGlobal->procSemIds[i] = semId;
		}
	}
}

/* ------------------------
 * InitProc -- create a per-process data structure for this process
 * used by the lock manager on semaphore queues.
 * ------------------------
 */
void
InitProcess(void)
{
	bool		found = false;
	unsigned long location,
				myOffset;

	SpinAcquire(ProcStructLock);

	/* attach to the ProcGlobal structure */
	ProcGlobal = (PROC_HDR *)
		ShmemInitStruct("Proc Header", sizeof(PROC_HDR), &found);
	if (!found)
	{
		/* this should not happen. InitProcGlobal() is called before this. */
		elog(STOP, "InitProcess: Proc Header uninitialized");
	}

	if (MyProc != NULL)
	{
		SpinRelease(ProcStructLock);
		elog(ERROR, "ProcInit: you already exist");
	}

	/* try to get a proc struct from the free list first */

	myOffset = ProcGlobal->freeProcs;

	if (myOffset != INVALID_OFFSET)
	{
		MyProc = (PROC *) MAKE_PTR(myOffset);
		ProcGlobal->freeProcs = MyProc->links.next;
	}
	else
	{

		/*
		 * have to allocate one.  We can't use the normal shmem index
		 * table mechanism because the proc structure is stored by PID
		 * instead of by a global name (need to look it up by PID when we
		 * cleanup dead processes).
		 */

		MyProc = (PROC *) ShmemAlloc(sizeof(PROC));
		if (!MyProc)
		{
			SpinRelease(ProcStructLock);
			elog(FATAL, "cannot create new proc: out of memory");
		}

		/* this cannot be initialized until after the buffer pool */
		SHMQueueInit(&(MyProc->lockQueue));
	}

	/*
	 * zero out the spin lock counts and set the sLocks field for
	 * ProcStructLock to 1 as we have acquired this spinlock above but
	 * didn't record it since we didn't have MyProc until now.
	 */
	MemSet(MyProc->sLocks, 0, sizeof(MyProc->sLocks));
	MyProc->sLocks[ProcStructLock] = 1;


	if (IsUnderPostmaster)
	{
		IpcSemaphoreId	semId;
		int				semNum;
		union semun		semun;

		ProcGetNewSemIdAndNum(&semId, &semNum);

		/*
		 * we might be reusing a semaphore that belongs to a dead backend.
		 * So be careful and reinitialize its value here.
		 */
		semun.val = 1;
		semctl(semId, semNum, SETVAL, semun);

		IpcSemaphoreLock(semId, semNum);
		MyProc->sem.semId = semId;
		MyProc->sem.semNum = semNum;
	}
	else
		MyProc->sem.semId = -1;

	/* ----------------------
	 * Release the lock.
	 * ----------------------
	 */
	SpinRelease(ProcStructLock);

	MyProc->pid = MyProcPid;
	MyProc->databaseId = MyDatabaseId;
	MyProc->xid = InvalidTransactionId;
	MyProc->xmin = InvalidTransactionId;

	/* ----------------
	 * Start keeping spin lock stats from here on.	Any botch before
	 * this initialization is forever botched
	 * ----------------
	 */
	MemSet(MyProc->sLocks, 0, MAX_SPINS * sizeof(*MyProc->sLocks));

	/* -------------------------
	 * Install ourselves in the shmem index table.	The name to
	 * use is determined by the OS-assigned process id.  That
	 * allows the cleanup process to find us after any untimely
	 * exit.
	 * -------------------------
	 */
	location = MAKE_OFFSET(MyProc);
	if ((!ShmemPIDLookup(MyProcPid, &location)) || (location != MAKE_OFFSET(MyProc)))
		elog(STOP, "InitProcess: ShmemPID table broken");

	MyProc->errType = NO_ERROR;
	SHMQueueElemInit(&(MyProc->links));

	on_shmem_exit(ProcKill, (Datum) MyProcPid);
}

/* -----------------------
 * get off the wait queue
 * -----------------------
 */
static bool
GetOffWaitqueue(PROC *proc)
{
	bool		getoffed = false;

	LockLockTable();
	if (proc->links.next != INVALID_OFFSET)
	{
		int			lockmode = proc->token;
		LOCK	*waitLock = proc->waitLock;

		Assert(waitLock);
		Assert(waitLock->waitProcs.size > 0);
		SHMQueueDelete(&(proc->links));
		--waitLock->waitProcs.size;
		Assert(waitLock->nHolding > 0);
		Assert(waitLock->nHolding > proc->waitLock->nActive);
		--waitLock->nHolding;
		Assert(waitLock->holders[lockmode] > 0);
		--waitLock->holders[lockmode];
		if (waitLock->activeHolders[lockmode] == waitLock->holders[lockmode])
			waitLock->waitMask &= ~(1 << lockmode);
		ProcLockWakeup(&(waitLock->waitProcs), LOCK_LOCKMETHOD(*waitLock), waitLock);
		getoffed = true;
	}
	SHMQueueElemInit(&(proc->links));
	UnlockLockTable();

	return getoffed;
}

/*
 * ProcReleaseLocks() -- release all locks associated with current transaction
 *
 */
void
ProcReleaseLocks()
{
	if (!MyProc)
		return;
	LockReleaseAll(DEFAULT_LOCKMETHOD, &MyProc->lockQueue);
	GetOffWaitqueue(MyProc);
}

/*
 * ProcRemove -
 *	  used by the postmaster to clean up the global tables. This also frees
 *	  up the semaphore used for the lmgr of the process.
 */
bool
ProcRemove(int pid)
{
	SHMEM_OFFSET location;
	PROC	   *proc;

	location = INVALID_OFFSET;

	location = ShmemPIDDestroy(pid);
	if (location == INVALID_OFFSET)
		return FALSE;
	proc = (PROC *) MAKE_PTR(location);

	SpinAcquire(ProcStructLock);

	ProcFreeSem(proc->sem.semId, proc->sem.semNum);

	proc->links.next = ProcGlobal->freeProcs;
	ProcGlobal->freeProcs = MAKE_OFFSET(proc);

	SpinRelease(ProcStructLock);

	return TRUE;
}

/*
 * ProcKill() -- Destroy the per-proc data structure for
 *		this process. Release any of its held spin locks.
 */
static void
ProcKill(int exitStatus, Datum pid)
{
	PROC	   *proc;
	SHMEM_OFFSET location;

	/* --------------------
	 * If this is a FATAL exit the postmaster will have to kill all the
	 * existing backends and reinitialize shared memory.  So all we don't
	 * need to do anything here.
	 * --------------------
	 */
	if (exitStatus != 0)
		return;

	ShmemPIDLookup(MyProcPid, &location);
	if (location == INVALID_OFFSET)
		return;

	proc = (PROC *) MAKE_PTR(location);

	Assert(proc == MyProc || (int)pid != MyProcPid);

	MyProc = NULL;

	/* ---------------
	 * Assume one lock table.
	 * ---------------
	 */
	ProcReleaseSpins(proc);
	LockReleaseAll(DEFAULT_LOCKMETHOD, &proc->lockQueue);

#ifdef USER_LOCKS

	/*
	 * Assume we have a second lock table.
	 */
	LockReleaseAll(USER_LOCKMETHOD, &proc->lockQueue);
#endif

	/* ----------------
	 * get off the wait queue
	 * ----------------
	 */
	GetOffWaitqueue(proc);
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
 *	Handling cancel request while waiting for lock
 *
 */
static bool lockWaiting = false;

void
SetWaitingForLock(bool waiting)
{
	if (waiting == lockWaiting)
		return;
	lockWaiting = waiting;
	if (lockWaiting)
	{
		/* The lock was already released ? */
		if (MyProc->links.next == INVALID_OFFSET)
		{
			lockWaiting = false;
			return;
		}
		if (QueryCancel)		/* cancel request pending */
		{
			if (GetOffWaitqueue(MyProc))
			{
				lockWaiting = false;
				elog(ERROR, "Query cancel requested while waiting lock");
			}
		}
	}
}

void
LockWaitCancel(void)
{
#ifndef __BEOS__ 	
	struct itimerval timeval,
				dummy;

	if (!lockWaiting)
		return;
	lockWaiting = false;
	/* Deadlock timer off */
	MemSet(&timeval, 0, sizeof(struct itimerval));
	setitimer(ITIMER_REAL, &timeval, &dummy);
#else
	/* BeOS doesn't have setitimer, but has set_alarm */
	if (!lockWaiting)
		return;
	lockWaiting = false;
	/* Deadlock timer off */
    set_alarm(B_INFINITE_TIMEOUT, B_PERIODIC_ALARM);
#endif /* __BEOS__ */
        
	if (GetOffWaitqueue(MyProc))
		elog(ERROR, "Query cancel requested while waiting lock");
}

/*
 * ProcSleep -- put a process to sleep
 *
 * P() on the semaphore should put us to sleep.  The process
 * semaphore is cleared by default, so the first time we try
 * to acquire it, we sleep.
 *
 * Result is NO_ERROR if we acquired the lock, STATUS_ERROR if not (deadlock).
 *
 * ASSUME: that no one will fiddle with the queue until after
 *		we release the spin lock.
 *
 * NOTES: The process queue is now a priority queue for locking.
 */
int
ProcSleep(PROC_QUEUE *waitQueue,/* lock->waitProcs */
		  LOCKMETHODCTL *lockctl,
		  int token,			/* lockmode */
		  LOCK *lock)
{
	int			i;
	SPINLOCK	spinlock = lockctl->masterLock;
	PROC	   *proc;
	int			myMask = (1 << token);
	int			waitMask = lock->waitMask;
	int			aheadHolders[MAX_LOCKMODES];
	bool		selfConflict = (lockctl->conflictTab[token] & myMask),
				prevSame = false;
#ifndef __BEOS__
	struct itimerval timeval,
				dummy;
#else
    bigtime_t time_interval;
#endif

	MyProc->token = token;
	MyProc->waitLock = lock;

	proc = (PROC *) MAKE_PTR(waitQueue->links.prev);

	/* if we don't conflict with any waiter - be first in queue */
	if (!(lockctl->conflictTab[token] & waitMask))
		goto ins;

	for (i = 1; i < MAX_LOCKMODES; i++)
		aheadHolders[i] = lock->activeHolders[i];
	(aheadHolders[token])++;

	for (i = 0; i < waitQueue->size; i++)
	{
		/* am I waiting for him ? */
		if (lockctl->conflictTab[token] & proc->holdLock)
		{
			/* is he waiting for me ? */
			if (lockctl->conflictTab[proc->token] & MyProc->holdLock)
			{
				/* Yes, report deadlock failure */
				MyProc->errType = STATUS_ERROR;
				goto rt;
			}
			/* being waiting for him - go past */
		}
		/* if he waits for me */
		else if (lockctl->conflictTab[proc->token] & MyProc->holdLock)
			break;
		/* if conflicting locks requested */
		else if (lockctl->conflictTab[proc->token] & myMask)
		{

			/*
			 * If I request non self-conflicting lock and there are others
			 * requesting the same lock just before me - stay here.
			 */
			if (!selfConflict && prevSame)
				break;
		}

		/*
		 * Last attempt to don't move any more: if we don't conflict with
		 * rest waiters in queue.
		 */
		else if (!(lockctl->conflictTab[token] & waitMask))
			break;

		prevSame = (proc->token == token);
		(aheadHolders[proc->token])++;
		if (aheadHolders[proc->token] == lock->holders[proc->token])
			waitMask &= ~(1 << proc->token);
		proc = (PROC *) MAKE_PTR(proc->links.prev);
	}

ins:;
	/* -------------------
	 * assume that these two operations are atomic (because
	 * of the spinlock).
	 * -------------------
	 */
	SHMQueueInsertTL(&(proc->links), &(MyProc->links));
	waitQueue->size++;

	lock->waitMask |= myMask;
	SpinRelease(spinlock);

	MyProc->errType = NO_ERROR;		/* initialize result for success */

	/* --------------
	 * Set timer so we can wake up after awhile and check for a deadlock.
	 * If a deadlock is detected, the handler releases the process's
	 * semaphore and sets MyProc->errType = STATUS_ERROR, allowing us to
	 * know that we must report failure rather than success.
	 *
	 * By delaying the check until we've waited for a bit, we can avoid
	 * running the rather expensive deadlock-check code in most cases.
	 *
	 * Need to zero out struct to set the interval and the micro seconds fields
	 * to 0.
	 * --------------
	 */
#ifndef __BEOS__
	MemSet(&timeval, 0, sizeof(struct itimerval));
	timeval.it_value.tv_sec = DeadlockTimeout / 1000;
	timeval.it_value.tv_usec = (DeadlockTimeout % 1000) * 1000;
	if (setitimer(ITIMER_REAL, &timeval, &dummy))
		elog(FATAL, "ProcSleep: Unable to set timer for process wakeup");
#else
    time_interval = DeadlockTimeout * 1000000; /* usecs */
	if (set_alarm(time_interval, B_ONE_SHOT_RELATIVE_ALARM) < 0)
		elog(FATAL, "ProcSleep: Unable to set timer for process wakeup");
#endif

	SetWaitingForLock(true);

	/* --------------
	 * If someone wakes us between SpinRelease and IpcSemaphoreLock,
	 * IpcSemaphoreLock will not block.  The wakeup is "saved" by
	 * the semaphore implementation.  Note also that if HandleDeadLock
	 * is invoked but does not detect a deadlock, IpcSemaphoreLock()
	 * will continue to wait.  There used to be a loop here, but it
	 * was useless code...
	 * --------------
	 */
	IpcSemaphoreLock(MyProc->sem.semId, MyProc->sem.semNum);

	lockWaiting = false;

	/* ---------------
	 * Disable the timer, if it's still running
	 * ---------------
	 */
#ifndef __BEOS__
	timeval.it_value.tv_sec = 0;
	timeval.it_value.tv_usec = 0;
	if (setitimer(ITIMER_REAL, &timeval, &dummy))
		elog(FATAL, "ProcSleep: Unable to disable timer for process wakeup");
#else
    if (set_alarm(B_INFINITE_TIMEOUT, B_PERIODIC_ALARM) < 0)
		elog(FATAL, "ProcSleep: Unable to disable timer for process wakeup");
#endif

	/* ----------------
	 * We were assumed to be in a critical section when we went
	 * to sleep.
	 * ----------------
	 */
	SpinAcquire(spinlock);

rt:;

#ifdef LOCK_DEBUG
	/* Just to get meaningful debug messages from DumpLocks() */
	MyProc->waitLock = (LOCK *) NULL;
#endif

	return MyProc->errType;
}


/*
 * ProcWakeup -- wake up a process by releasing its private semaphore.
 *
 *	 remove the process from the wait queue and set its links invalid.
 *	 RETURN: the next process in the wait queue.
 */
PROC *
ProcWakeup(PROC *proc, int errType)
{
	PROC	   *retProc;

	/* assume that spinlock has been acquired */

	if (proc->links.prev == INVALID_OFFSET ||
		proc->links.next == INVALID_OFFSET)
		return (PROC *) NULL;

	retProc = (PROC *) MAKE_PTR(proc->links.prev);

	/* you have to update waitLock->waitProcs.size yourself */
	SHMQueueDelete(&(proc->links));
	SHMQueueElemInit(&(proc->links));

	proc->errType = errType;

	IpcSemaphoreUnlock(proc->sem.semId, proc->sem.semNum);

	return retProc;
}

/*
 * ProcLockWakeup -- routine for waking up processes when a lock is
 *		released.
 */
int
ProcLockWakeup(PROC_QUEUE *queue, LOCKMETHOD lockmethod, LOCK *lock)
{
	PROC	   *proc;
	int			count = 0;
	int			last_locktype = 0;
	int			queue_size = queue->size;

	Assert(queue->size >= 0);

	if (!queue->size)
		return STATUS_NOT_FOUND;

	proc = (PROC *) MAKE_PTR(queue->links.prev);
	while ((queue_size--) && (proc))
	{

		/*
		 * This proc will conflict as the previous one did, don't even
		 * try.
		 */
		if (proc->token == last_locktype)
			continue;

		/*
		 * Does this proc conflict with locks held by others ?
		 */
		if (LockResolveConflicts(lockmethod,
								 lock,
								 proc->token,
								 proc->xid,
								 (XIDLookupEnt *) NULL) != STATUS_OK)
		{
			if (count != 0)
				break;
			last_locktype = proc->token;
			continue;
		}

		/*
		 * there was a waiting process, grant it the lock before waking it
		 * up.	This will prevent another process from seizing the lock
		 * between the time we release the lock master (spinlock) and the
		 * time that the awoken process begins executing again.
		 */
		GrantLock(lock, proc->token);

		/*
		 * ProcWakeup removes proc from the lock waiting process queue and
		 * returns the next proc in chain.
		 */

		count++;
		queue->size--;
		proc = ProcWakeup(proc, NO_ERROR);
	}

	Assert(queue->size >= 0);

	if (count)
		return STATUS_OK;
	else
	{
		/* Something is still blocking us.	May have deadlocked. */
#ifdef LOCK_DEBUG
		if (lock->tag.lockmethod == USER_LOCKMETHOD ? Trace_userlocks : Trace_locks)
		{
			elog(DEBUG, "ProcLockWakeup: lock(%lx) can't wake up any process", MAKE_OFFSET(lock));
			if (Debug_deadlocks)
			DumpAllLocks();
		}
#endif
		return STATUS_NOT_FOUND;
	}
}

void
ProcAddLock(SHM_QUEUE *elem)
{
	SHMQueueInsertTL(&MyProc->lockQueue, elem);
}

/* --------------------
 * We only get to this routine if we got SIGALRM after DeadlockTimeout
 * while waiting for a lock to be released by some other process.  If we have
 * a real deadlock, we must also indicate that I'm no longer waiting
 * on a lock so that other processes don't try to wake me up and screw
 * up my semaphore.
 * --------------------
 */
void
HandleDeadLock(SIGNAL_ARGS)
{
	LOCK	   *mywaitlock;

	LockLockTable();

	/* ---------------------
	 * Check to see if we've been awoken by anyone in the interim.
	 *
	 * If we have we can return and resume our transaction -- happy day.
	 * Before we are awoken the process releasing the lock grants it to
	 * us so we know that we don't have to wait anymore.
	 *
	 * We check by looking to see if we've been unlinked from the wait queue.
	 * This is quicker than checking our semaphore's state, since no kernel
	 * call is needed, and it is safe because we hold the locktable lock.
	 * ---------------------
	 */
	if (MyProc->links.prev == INVALID_OFFSET ||
		MyProc->links.next == INVALID_OFFSET)
	{
		UnlockLockTable();
		return;
	}

#ifdef LOCK_DEBUG
    if (Debug_deadlocks)
        DumpAllLocks();
#endif

	if (!DeadLockCheck(MyProc, MyProc->waitLock))
	{
		/* No deadlock, so keep waiting */
		UnlockLockTable();
		return;
	}

	/* ------------------------
	 * Get this process off the lock's wait queue
	 * ------------------------
	 */
	mywaitlock = MyProc->waitLock;
	Assert(mywaitlock->waitProcs.size > 0);
	lockWaiting = false;
	--mywaitlock->waitProcs.size;
	SHMQueueDelete(&(MyProc->links));
	SHMQueueElemInit(&(MyProc->links));

	/* ------------------
	 * Unlock my semaphore so that the interrupted ProcSleep() call can finish.
	 * ------------------
	 */
	IpcSemaphoreUnlock(MyProc->sem.semId, MyProc->sem.semNum);

	/* -------------
	 * Set MyProc->errType to STATUS_ERROR so that we abort after
	 * returning from this handler.
	 * -------------
	 */
	MyProc->errType = STATUS_ERROR;

	/*
	 * if this doesn't follow the IpcSemaphoreUnlock then we get lock
	 * table corruption ("LockReplace: xid table corrupted") due to race
	 * conditions.	i don't claim to understand this...
	 */
	UnlockLockTable();
}

void
ProcReleaseSpins(PROC *proc)
{
	int			i;

	if (!proc)
		proc = MyProc;

	if (!proc)
		return;
	for (i = 0; i < (int) MAX_SPINS; i++)
	{
		if (proc->sLocks[i])
		{
			Assert(proc->sLocks[i] == 1);
			SpinRelease(i);
		}
	}
	AbortBufferIO();
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
	IpcSemaphoreId *procSemIds = ProcGlobal->procSemIds;
	int32	   *freeSemMap = ProcGlobal->freeSemMap;
	int32		fullmask = (1 << PROC_NSEMS_PER_SET) - 1;

	/*
	 * we hold ProcStructLock when entering this routine. We scan through
	 * the bitmap to look for a free semaphore.
	 */

	for (i = 0; i < PROC_SEM_MAP_ENTRIES; i++)
	{
		int			mask = 1;
		int			j;

		if (freeSemMap[i] == fullmask)
			continue;			/* this set is fully allocated */
		if (procSemIds[i] < 0)
			continue;			/* this set hasn't been initialized */

		for (j = 0; j < PROC_NSEMS_PER_SET; j++)
		{
			if ((freeSemMap[i] & mask) == 0)
			{

				/*
				 * a free semaphore found. Mark it as allocated.
				 */
				freeSemMap[i] |= mask;

				*semId = procSemIds[i];
				*semNum = j;
				return;
			}
			mask <<= 1;
		}
	}

	/* if we reach here, all the semaphores are in use. */
	elog(ERROR, "ProcGetNewSemIdAndNum: cannot allocate a free semaphore");
}

/*
 * ProcFreeSem -
 *	  free up our semaphore in the semaphore set.
 */
static void
ProcFreeSem(IpcSemaphoreId semId, int semNum)
{
	int32		mask;
	int			i;

	mask = ~(1 << semNum);

	for (i = 0; i < PROC_SEM_MAP_ENTRIES; i++)
	{
		if (ProcGlobal->procSemIds[i] == semId)
		{
			ProcGlobal->freeSemMap[i] &= mask;
			return;
		}
	}
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

	for (i = 0; i < PROC_SEM_MAP_ENTRIES; i++)
	{
		if (ProcGlobal->procSemIds[i] >= 0)
			IpcSemaphoreKill(ProcGlobal->procSemIds[i]);
	}
}
