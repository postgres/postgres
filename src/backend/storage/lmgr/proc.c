/*-------------------------------------------------------------------------
 *
 * proc.c--
 *	  routines to manage per-process shared memory data structure
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/lmgr/proc.c,v 1.41 1998/08/25 21:20:29 scrappy Exp $
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
 * ProcReleaseLocks -- frees the locks associated with this process,
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
 * $Header: /cvsroot/pgsql/src/backend/storage/lmgr/proc.c,v 1.41 1998/08/25 21:20:29 scrappy Exp $
 */
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>

#if defined(solaris_sparc)
#include <sys/ipc.h>
#include <sys/sem.h>
#endif

#include "postgres.h"
#include "miscadmin.h"
#include "libpq/pqsignal.h"

#include "access/xact.h"
#include "utils/hsearch.h"

#include "storage/ipc.h"
/* In Ultrix, sem.h must be included after ipc.h */
#include <sys/sem.h>
#include "storage/buf.h"
#include "storage/lock.h"
#include "storage/lmgr.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "storage/proc.h"
#include "utils/trace.h"

static void HandleDeadLock(int sig);
static PROC *ProcWakeup(PROC *proc, int errType);

#define DeadlockCheckTimer pg_options[OPT_DEADLOCKTIMEOUT]

/* --------------------
 * Spin lock for manipulating the shared process data structure:
 * ProcGlobal.... Adding an extra spin lock seemed like the smallest
 * hack to get around reading and updating this structure in shared
 * memory. -mer 17 July 1991
 * --------------------
 */
SPINLOCK	ProcStructLock;

/*
 * For cleanup routines.  Don't cleanup if the initialization
 * has not happened.
 */
static bool ProcInitialized = FALSE;

static PROC_HDR *ProcGlobal = NULL;

PROC	   *MyProc = NULL;

static void ProcKill(int exitStatus, int pid);
static void ProcGetNewSemKeyAndNum(IPCKey *key, int *semNum);
static void ProcFreeSem(IpcSemaphoreKey semKey, int semNum);

/*
 * InitProcGlobal -
 *	  initializes the global process table. We put it here so that
 *	  the postmaster can do this initialization. (ProcFreeAllSem needs
 *	  to read this table on exiting the postmaster. If we have the first
 *	  backend do this, starting up and killing the postmaster without
 *	  starting any backends will be a problem.)
 */
void
InitProcGlobal(IPCKey key)
{
	bool		found = false;

	/* attach to the free list */
	ProcGlobal = (PROC_HDR *)
		ShmemInitStruct("Proc Header", (unsigned) sizeof(PROC_HDR), &found);

	/* --------------------
	 * We're the first - initialize.
	 * --------------------
	 */
	if (!found)
	{
		int			i;

		ProcGlobal->freeProcs = INVALID_OFFSET;
		ProcGlobal->currKey = IPCGetProcessSemaphoreInitKey(key);
		for (i = 0; i < MAX_PROC_SEMS / PROC_NSEMS_PER_SET; i++)
			ProcGlobal->freeSemMap[i] = 0;
	}
}

/* ------------------------
 * InitProc -- create a per-process data structure for this process
 * used by the lock manager on semaphore queues.
 * ------------------------
 */
void
InitProcess(IPCKey key)
{
	bool		found = false;
	int			semstat;
	unsigned long location,
				myOffset;

	/* ------------------
	 * Routine called if deadlock timer goes off. See ProcSleep()
	 * ------------------
	 */
	pqsignal(SIGALRM, HandleDeadLock);

	SpinAcquire(ProcStructLock);

	/* attach to the free list */
	ProcGlobal = (PROC_HDR *)
		ShmemInitStruct("Proc Header", (unsigned) sizeof(PROC_HDR), &found);
	if (!found)
	{
		/* this should not happen. InitProcGlobal() is called before this. */
		elog(ERROR, "InitProcess: Proc Header uninitialized");
	}

	if (MyProc != NULL)
	{
		SpinRelease(ProcStructLock);
		elog(ERROR, "ProcInit: you already exist");
		return;
	}

	/* try to get a proc from the free list first */

	myOffset = ProcGlobal->freeProcs;

	if (myOffset != INVALID_OFFSET)
	{
		MyProc = (PROC *) MAKE_PTR(myOffset);
		ProcGlobal->freeProcs = MyProc->links.next;
	}
	else
	{

		/*
		 * have to allocate one.  We can't use the normal shmem index table
		 * mechanism because the proc structure is stored by PID instead
		 * of by a global name (need to look it up by PID when we cleanup
		 * dead processes).
		 */

		MyProc = (PROC *) ShmemAlloc((unsigned) sizeof(PROC));
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
		IPCKey		semKey;
		int			semNum;
		int			semId;
		union semun semun;

		ProcGetNewSemKeyAndNum(&semKey, &semNum);

		semId = IpcSemaphoreCreate(semKey,
								   PROC_NSEMS_PER_SET,
								   IPCProtection,
								   IpcSemaphoreDefaultStartValue,
								   0,
								   &semstat);

		/*
		 * we might be reusing a semaphore that belongs to a dead backend.
		 * So be careful and reinitialize its value here.
		 */
		semun.val = IpcSemaphoreDefaultStartValue;
		semctl(semId, semNum, SETVAL, semun);

		IpcSemaphoreLock(semId, semNum, IpcExclusiveLock);
		MyProc->sem.semId = semId;
		MyProc->sem.semNum = semNum;
		MyProc->sem.semKey = semKey;
	}
	else
		MyProc->sem.semId = -1;

	/* ----------------------
	 * Release the lock.
	 * ----------------------
	 */
	SpinRelease(ProcStructLock);

	MyProc->pid = MyProcPid;
	MyProc->xid = InvalidTransactionId;
#ifdef LowLevelLocking
	MyProc->xmin = InvalidTransactionId;
#endif

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
		elog(FATAL, "InitProc: ShmemPID table broken");

	MyProc->errType = NO_ERROR;
	SHMQueueElemInit(&(MyProc->links));

	on_shmem_exit(ProcKill, (caddr_t) MyProcPid);

	ProcInitialized = TRUE;
}

/*
 * ProcReleaseLocks() -- release all locks associated with this process
 *
 */
void
ProcReleaseLocks()
{
	if (!MyProc)
		return;
	LockReleaseAll(1, &MyProc->lockQueue);
}

/*
 * ProcRemove -
 *	  used by the postmaster to clean up the global tables. This also frees
 *	  up the semaphore used for the lmgr of the process. (We have to do
 *	  this is the postmaster instead of doing a IpcSemaphoreKill on exiting
 *	  the process because the semaphore set is shared among backends and
 *	  we don't want to remove other's semaphores on exit.)
 */
bool
ProcRemove(int pid)
{
	SHMEM_OFFSET location;
	PROC	   *proc;

	location = INVALID_OFFSET;

	location = ShmemPIDDestroy(pid);
	if (location == INVALID_OFFSET)
		return (FALSE);
	proc = (PROC *) MAKE_PTR(location);

	SpinAcquire(ProcStructLock);

	ProcFreeSem(proc->sem.semKey, proc->sem.semNum);

	proc->links.next = ProcGlobal->freeProcs;
	ProcGlobal->freeProcs = MAKE_OFFSET(proc);

	SpinRelease(ProcStructLock);

	return (TRUE);
}

/*
 * ProcKill() -- Destroy the per-proc data structure for
 *		this process. Release any of its held spin locks.
 */
static void
ProcKill(int exitStatus, int pid)
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

	Assert(proc == MyProc || pid != MyProcPid);

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
	LockLockTable();
	if (proc->links.next != INVALID_OFFSET)
	{
		Assert(proc->waitLock->waitProcs.size > 0);
		SHMQueueDelete(&(proc->links));
		--proc->waitLock->waitProcs.size;
	}
	SHMQueueElemInit(&(proc->links));
	UnlockLockTable();

	return;
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
	ShmemInitStruct(name, (unsigned) sizeof(PROC_QUEUE), &found);

	if (!queue)
		return (NULL);
	if (!found)
		ProcQueueInit(queue);
	return (queue);
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
 * P() on the semaphore should put us to sleep.  The process
 * semaphore is cleared by default, so the first time we try
 * to acquire it, we sleep.
 *
 * ASSUME: that no one will fiddle with the queue until after
 *		we release the spin lock.
 *
 * NOTES: The process queue is now a priority queue for locking.
 */
int
ProcSleep(PROC_QUEUE *waitQueue,		/* lock->waitProcs */
		  SPINLOCK spinlock,
		  int token,					/* lockmode */
		  int prio,
		  LOCK *lock,
		  TransactionId xid)		    /* needed by user locks, see below */
{
	int			i;
	PROC	   *proc;
	struct itimerval timeval,
				dummy;

	/*
	 * If the first entries in the waitQueue have a greater priority than
	 * we have, we must be a reader, and they must be a writers, and we
	 * must be here because the current holder is a writer or a reader but
	 * we don't share shared locks if a writer is waiting. We put
	 * ourselves after the writers.  This way, we have a FIFO, but keep
	 * the readers together to give them decent priority, and no one
	 * starves.  Because we group all readers together, a non-empty queue
	 * only has a few possible configurations:
	 *
	 * [readers] [writers] [readers][writers] [writers][readers]
	 * [writers][readers][writers]
	 *
	 * In a full queue, we would have a reader holding a lock, then a writer
	 * gets the lock, then a bunch of readers, made up of readers who
	 * could not share the first readlock because a writer was waiting,
	 * and new readers arriving while the writer had the lock.
	 *
	 */
	proc = (PROC *) MAKE_PTR(waitQueue->links.prev);

	/* If we are a reader, and they are writers, skip past them */
	for (i = 0; i < waitQueue->size && proc->prio > prio; i++)
		proc = (PROC *) MAKE_PTR(proc->links.prev);

	/* The rest of the queue is FIFO, with readers first, writers last */
	for (; i < waitQueue->size && proc->prio <= prio; i++)
		proc = (PROC *) MAKE_PTR(proc->links.prev);

	MyProc->prio = prio;
	MyProc->token = token;
	MyProc->waitLock = lock;

#ifdef USER_LOCKS
	/* -------------------
	 * Currently, we only need this for the ProcWakeup routines.
	 * This must be 0 for user lock, so we can't just use the value
	 * from GetCurrentTransactionId().
	 * -------------------
	 */
	TransactionIdStore(xid, &MyProc->xid);
#else
#ifndef LowLevelLocking
	/* -------------------
	 * currently, we only need this for the ProcWakeup routines
	 * -------------------
	 */
	TransactionIdStore((TransactionId) GetCurrentTransactionId(), &MyProc->xid);
#endif
#endif

	/* -------------------
	 * assume that these two operations are atomic (because
	 * of the spinlock).
	 * -------------------
	 */
	SHMQueueInsertTL(&(proc->links), &(MyProc->links));
	waitQueue->size++;

	SpinRelease(spinlock);

	/* --------------
	 * We set this so we can wake up periodically and check for a deadlock.
	 * If a deadlock is detected, the handler releases the processes
	 * semaphore and aborts the current transaction.
	 *
	 * Need to zero out struct to set the interval and the micro seconds fields
	 * to 0.
	 * --------------
	 */
	MemSet(&timeval, 0, sizeof(struct itimerval));
	timeval.it_value.tv_sec = \
		(DeadlockCheckTimer ? DeadlockCheckTimer : DEADLOCK_CHECK_TIMER);

	do
	{
		MyProc->errType = NO_ERROR;		/* reset flag after deadlock check */

		if (setitimer(ITIMER_REAL, &timeval, &dummy))
			elog(FATAL, "ProcSleep: Unable to set timer for process wakeup");

		/* --------------
		 * if someone wakes us between SpinRelease and IpcSemaphoreLock,
		 * IpcSemaphoreLock will not block.  The wakeup is "saved" by
		 * the semaphore implementation.
		 * --------------
		 */
		IpcSemaphoreLock(MyProc->sem.semId, MyProc->sem.semNum,
						 IpcExclusiveLock);
	} while (MyProc->errType == STATUS_NOT_FOUND);		/* sleep after deadlock
														 * check */

	/* ---------------
	 * We were awoken before a timeout - now disable the timer
	 * ---------------
	 */
	timeval.it_value.tv_sec = 0;
	if (setitimer(ITIMER_REAL, &timeval, &dummy))
		elog(FATAL, "ProcSleep: Unable to diable timer for process wakeup");

	/* ----------------
	 * We were assumed to be in a critical section when we went
	 * to sleep.
	 * ----------------
	 */
	SpinAcquire(spinlock);

#ifdef LOCK_MGR_DEBUG
	/* Just to get meaningful debug messages from DumpLocks() */
	MyProc->waitLock = (LOCK *)NULL;
#endif

	return (MyProc->errType);
}


/*
 * ProcWakeup -- wake up a process by releasing its private semaphore.
 *
 *	 remove the process from the wait queue and set its links invalid.
 *	 RETURN: the next process in the wait queue.
 */
static PROC *
ProcWakeup(PROC *proc, int errType)
{
	PROC	   *retProc;

	/* assume that spinlock has been acquired */

	if (proc->links.prev == INVALID_OFFSET ||
		proc->links.next == INVALID_OFFSET)
		return ((PROC *) NULL);

	retProc = (PROC *) MAKE_PTR(proc->links.prev);

	/* you have to update waitLock->waitProcs.size yourself */
	SHMQueueDelete(&(proc->links));
	SHMQueueElemInit(&(proc->links));

	proc->errType = errType;

	IpcSemaphoreUnlock(proc->sem.semId, proc->sem.semNum, IpcExclusiveLock);

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
	int			count;
	int			trace_flag;
	int			last_locktype = -1;
	int			queue_size = queue->size;

	Assert(queue->size >= 0);

	if (!queue->size)
		return (STATUS_NOT_FOUND);

	proc = (PROC *) MAKE_PTR(queue->links.prev);
	count = 0;
	while ((queue_size--) && (proc))
	{
		/*
		 * This proc will conflict as the previous one did, don't even try.
		 */
		if (proc->token == last_locktype)
		{
			continue;
		}

		/*
		 * This proc conflicts with locks held by others, ignored.
		 */
		if (LockResolveConflicts(lockmethod,
								 lock,
								 proc->token,
								 proc->xid,
								 (XIDLookupEnt *) NULL) != STATUS_OK)
		{
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
		return (STATUS_OK);
	else {
		/* Something is still blocking us.	May have deadlocked. */
		trace_flag = (lock->tag.lockmethod == USER_LOCKMETHOD) ? \
			TRACE_USERLOCKS : TRACE_LOCKS;
		TPRINTF(trace_flag,
				"ProcLockWakeup: lock(%x) can't wake up any process",
				MAKE_OFFSET(lock));
#ifdef DEADLOCK_DEBUG
		if (pg_options[trace_flag] >= 2)
			DumpAllLocks();
#endif
		return (STATUS_NOT_FOUND);
	}
}

void
ProcAddLock(SHM_QUEUE *elem)
{
	SHMQueueInsertTL(&MyProc->lockQueue, elem);
}

/* --------------------
 * We only get to this routine if we got SIGALRM after DEADLOCK_CHECK_TIMER
 * while waiting for a lock to be released by some other process.  If we have
 * a real deadlock, we must also indicate that I'm no longer waiting
 * on a lock so that other processes don't try to wake me up and screw
 * up my semaphore.
 * --------------------
 */
static void
HandleDeadLock(int sig)
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
	 * Damn these names are LONG! -mer
	 * ---------------------
	 */
	if (IpcSemaphoreGetCount(MyProc->sem.semId, MyProc->sem.semNum) ==
		IpcSemaphoreDefaultStartValue)
	{
		UnlockLockTable();
		return;
	}

	/*
	 * you would think this would be unnecessary, but...
	 *
	 * this also means we've been removed already.  in some ports (e.g.,
	 * sparc and aix) the semop(2) implementation is such that we can
	 * actually end up in this handler after someone has removed us from
	 * the queue and bopped the semaphore *but the test above fails to
	 * detect the semaphore update* (presumably something weird having to
	 * do with the order in which the semaphore wakeup signal and SIGALRM
	 * get handled).
	 */
	if (MyProc->links.prev == INVALID_OFFSET ||
		MyProc->links.next == INVALID_OFFSET)
	{
		UnlockLockTable();
		return;
	}

#ifdef DEADLOCK_DEBUG
	DumpAllLocks();
#endif

	if (!DeadLockCheck(&(MyProc->lockQueue), MyProc->waitLock, true))
	{
		UnlockLockTable();
		MyProc->errType = STATUS_NOT_FOUND;
		return;
	}

	mywaitlock = MyProc->waitLock;

	/* ------------------------
	 * Get this process off the lock's wait queue
	 * ------------------------
	 */
	Assert(mywaitlock->waitProcs.size > 0);
	--mywaitlock->waitProcs.size;
	SHMQueueDelete(&(MyProc->links));
	SHMQueueElemInit(&(MyProc->links));

	/* ------------------
	 * Unlock my semaphore so that the count is right for next time.
	 * I was awoken by a signal, not by someone unlocking my semaphore.
	 * ------------------
	 */
	IpcSemaphoreUnlock(MyProc->sem.semId, MyProc->sem.semNum,
					   IpcExclusiveLock);

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

	elog(NOTICE, "Deadlock detected -- See the lock(l) manual page for a possible cause.");
	return;
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
}

/*****************************************************************************
 *
 *****************************************************************************/

/*
 * ProcGetNewSemKeyAndNum -
 *	  scan the free semaphore bitmap and allocate a single semaphore from
 *	  a semaphore set. (If the semaphore set doesn't exist yet,
 *	  IpcSemaphoreCreate will create it. Otherwise, we use the existing
 *	  semaphore set.)
 */
static void
ProcGetNewSemKeyAndNum(IPCKey *key, int *semNum)
{
	int			i;
	int32	   *freeSemMap = ProcGlobal->freeSemMap;
	unsigned int fullmask;

	/*
	 * we hold ProcStructLock when entering this routine. We scan through
	 * the bitmap to look for a free semaphore.
	 */
	fullmask = ~0 >> (32 - PROC_NSEMS_PER_SET);
	for (i = 0; i < MAX_PROC_SEMS / PROC_NSEMS_PER_SET; i++)
	{
		int			mask = 1;
		int			j;

		if (freeSemMap[i] == fullmask)
			continue;			/* none free for this set */

		for (j = 0; j < PROC_NSEMS_PER_SET; j++)
		{
			if ((freeSemMap[i] & mask) == 0)
			{

				/*
				 * a free semaphore found. Mark it as allocated.
				 */
				freeSemMap[i] |= mask;

				*key = ProcGlobal->currKey + i;
				*semNum = j;
				return;
			}
			mask <<= 1;
		}
	}

	/* if we reach here, all the semaphores are in use. */
	elog(ERROR, "InitProc: cannot allocate a free semaphore");
}

/*
 * ProcFreeSem -
 *	  free up our semaphore in the semaphore set. If we're the last one
 *	  in the set, also remove the semaphore set.
 */
static void
ProcFreeSem(IpcSemaphoreKey semKey, int semNum)
{
	int			mask;
	int			i;
	int32	   *freeSemMap = ProcGlobal->freeSemMap;

	i = semKey - ProcGlobal->currKey;
	mask = ~(1 << semNum);
	freeSemMap[i] &= mask;

	if (freeSemMap[i] == 0)
		IpcSemaphoreKill(semKey);
}

/*
 * ProcFreeAllSemaphores -
 *	  on exiting the postmaster, we free up all the semaphores allocated
 *	  to the lmgrs of the backends.
 */
void
ProcFreeAllSemaphores()
{
	int			i;
	int32	   *freeSemMap = ProcGlobal->freeSemMap;

	for (i = 0; i < MAX_PROC_SEMS / PROC_NSEMS_PER_SET; i++)
	{
		if (freeSemMap[i] != 0)
			IpcSemaphoreKill(ProcGlobal->currKey + i);
	}
}
