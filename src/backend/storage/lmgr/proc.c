/*-------------------------------------------------------------------------
 *
 * proc.c--
 *    routines to manage per-process shared memory data structure
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/storage/lmgr/proc.c,v 1.9 1996/11/08 05:58:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 *  Each postgres backend gets one of these.  We'll use it to
 *  clean up after the process should the process suddenly die.
 *
 *
 * Interface (a):
 *	ProcSleep(), ProcWakeup(), ProcWakeupNext(),
 * 	ProcQueueAlloc() -- create a shm queue for sleeping processes
 * 	ProcQueueInit() -- create a queue without allocing memory
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
 *	associated with the process.
 *
 * 5/15/91 -- removed the buffer pool based lock chain in favor
 *	of a shared memory lock chain.  The write-protection is
 *	more expensive if the lock chain is in the buffer pool.
 *	The only reason I kept the lock chain in the buffer pool
 *	in the first place was to allow the lock table to grow larger
 *	than available shared memory and that isn't going to work
 *	without a lot of unimplemented support anyway.
 *
 * 4/7/95 -- instead of allocating a set of 1 semaphore per process, we
 *      allocate a semaphore from a set of PROC_NSEMS_PER_SET semaphores
 *      shared among backends (we keep a few sets of semaphores around).
 *      This is so that we can support more backends. (system-wide semaphore
 *      sets run out pretty fast.)                -ay 4/95
 *
 * $Header: /cvsroot/pgsql/src/backend/storage/lmgr/proc.c,v 1.9 1996/11/08 05:58:59 momjian Exp $
 */
#include <sys/time.h>
#ifndef WIN32
#include <unistd.h>
#endif /* WIN32 */
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>

#if defined(sparc_solaris)
#include <sys/ipc.h>
#include <sys/sem.h>
#endif

#include "postgres.h"
#include "miscadmin.h"
#include "libpq/pqsignal.h"	/* substitute for <signal.h> */

#include "access/xact.h"
#include "utils/hsearch.h"

#include "storage/buf.h"	
#include "storage/lock.h"
#include "storage/lmgr.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "storage/proc.h"

/*
 * timeout (in seconds) for resolving possible deadlock
 */
#ifndef DEADLOCK_TIMEOUT
#define DEADLOCK_TIMEOUT	60
#endif

/* --------------------
 * Spin lock for manipulating the shared process data structure:
 * ProcGlobal.... Adding an extra spin lock seemed like the smallest
 * hack to get around reading and updating this structure in shared
 * memory. -mer 17 July 1991
 * --------------------
 */
SPINLOCK ProcStructLock;

/*
 * For cleanup routines.  Don't cleanup if the initialization
 * has not happened.
 */
static bool	ProcInitialized = FALSE;

static PROC_HDR *ProcGlobal = NULL;

PROC 	*MyProc = NULL;

static void ProcKill(int exitStatus, int pid);
static void ProcGetNewSemKeyAndNum(IPCKey *key, int *semNum);
static void ProcFreeSem(IpcSemaphoreKey semKey, int semNum);
/*
 * InitProcGlobal -
 *    initializes the global process table. We put it here so that
 *    the postmaster can do this initialization. (ProcFreeAllSem needs
 *    to read this table on exiting the postmaster. If we have the first
 *    backend do this, starting up and killing the postmaster without
 *    starting any backends will be a problem.)
 */
void
InitProcGlobal(IPCKey key)
{
    bool found = false;

    /* attach to the free list */
    ProcGlobal = (PROC_HDR *)
	ShmemInitStruct("Proc Header",(unsigned)sizeof(PROC_HDR),&found);

    /* --------------------
     * We're the first - initialize.
     * --------------------
     */
    if (! found)
	{
	    int i;

	    ProcGlobal->numProcs = 0;
	    ProcGlobal->freeProcs = INVALID_OFFSET;
	    ProcGlobal->currKey = IPCGetProcessSemaphoreInitKey(key);
	    for (i=0; i < MAX_PROC_SEMS/PROC_NSEMS_PER_SET; i++)
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
    bool found = false;
    int pid;
    int semstat;
    unsigned long location, myOffset;
    
    /* ------------------
     * Routine called if deadlock timer goes off. See ProcSleep()
     * ------------------
     */
#ifndef WIN32
    signal(SIGALRM, HandleDeadLock);
#endif /* WIN32 we'll have to figure out how to handle this later */

    SpinAcquire(ProcStructLock);
    
    /* attach to the free list */
    ProcGlobal = (PROC_HDR *)
	ShmemInitStruct("Proc Header",(unsigned)sizeof(PROC_HDR),&found);
    if (!found) {
	/* this should not happen. InitProcGlobal() is called before this. */
	elog(WARN, "InitProcess: Proc Header uninitialized");
    }
    
    if (MyProc != NULL)
	{
	    SpinRelease(ProcStructLock);
	    elog(WARN,"ProcInit: you already exist");
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
	    /* have to allocate one.  We can't use the normal binding
	     * table mechanism because the proc structure is stored
	     * by PID instead of by a global name (need to look it
	     * up by PID when we cleanup dead processes).
	     */
	    
	    MyProc = (PROC *) ShmemAlloc((unsigned)sizeof(PROC));
	    if (! MyProc)
		{
		    SpinRelease(ProcStructLock);
		    elog (FATAL,"cannot create new proc: out of memory");
		}
	    
	    /* this cannot be initialized until after the buffer pool */
	    SHMQueueInit(&(MyProc->lockQueue));
	    MyProc->procId = ProcGlobal->numProcs;
	    ProcGlobal->numProcs++;
	}
    
    /*
     * zero out the spin lock counts and set the sLocks field for
     * ProcStructLock to 1 as we have acquired this spinlock above but 
     * didn't record it since we didn't have MyProc until now.
     */
    memset(MyProc->sLocks, 0, sizeof(MyProc->sLocks));
    MyProc->sLocks[ProcStructLock] = 1;


    if (IsUnderPostmaster) {
	IPCKey semKey;
	int semNum;
	int semId;
	union semun semun;

	ProcGetNewSemKeyAndNum(&semKey, &semNum);
	
	semId = IpcSemaphoreCreate(semKey,
				   PROC_NSEMS_PER_SET,
				   IPCProtection,
				   IpcSemaphoreDefaultStartValue,
				   0,
				   &semstat);
	/*
	 * we might be reusing a semaphore that belongs to a dead
	 * backend. So be careful and reinitialize its value here.
	 */
	semun.val = IpcSemaphoreDefaultStartValue;
	semctl(semId, semNum, SETVAL, semun);

	IpcSemaphoreLock(semId, semNum, IpcExclusiveLock);
	MyProc->sem.semId = semId;
	MyProc->sem.semNum = semNum;
	MyProc->sem.semKey = semKey;
    } else {
	MyProc->sem.semId = -1;
    }
    
    /* ----------------------
     * Release the lock.
     * ----------------------
     */
    SpinRelease(ProcStructLock);
    
    MyProc->pid = 0;
#if 0
    MyProc->pid = MyPid;
#endif
    
    /* ----------------
     * Start keeping spin lock stats from here on.  Any botch before
     * this initialization is forever botched
     * ----------------
     */
    memset(MyProc->sLocks, 0, MAX_SPINS*sizeof(*MyProc->sLocks));
    
    /* -------------------------
     * Install ourselves in the binding table.  The name to
     * use is determined by the OS-assigned process id.  That
     * allows the cleanup process to find us after any untimely
     * exit.
     * -------------------------
     */
    pid = getpid();
    location = MAKE_OFFSET(MyProc);
    if ((! ShmemPIDLookup(pid,&location)) || (location != MAKE_OFFSET(MyProc)))
	{
	    elog(FATAL,"InitProc: ShmemPID table broken");
	}
    
    MyProc->errType = NO_ERROR;
    SHMQueueElemInit(&(MyProc->links));
    
    on_exitpg(ProcKill, (caddr_t)pid);
    
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
    LockReleaseAll(1,&MyProc->lockQueue);
}

/*
 * ProcRemove -
 *    used by the postmaster to clean up the global tables. This also frees
 *    up the semaphore used for the lmgr of the process. (We have to do
 *    this is the postmaster instead of doing a IpcSemaphoreKill on exiting
 *    the process because the semaphore set is shared among backends and
 *    we don't want to remove other's semaphores on exit.)
 */
bool
ProcRemove(int pid)
{
    SHMEM_OFFSET  location;
    PROC *proc;
    
    location = INVALID_OFFSET;
    
    location = ShmemPIDDestroy(pid);
    if (location == INVALID_OFFSET)
	return(FALSE);
    proc = (PROC *) MAKE_PTR(location);

    SpinAcquire(ProcStructLock);
    
    ProcFreeSem(proc->sem.semKey, proc->sem.semNum);

    proc->links.next =  ProcGlobal->freeProcs;
    ProcGlobal->freeProcs = MAKE_OFFSET(proc);
    
    SpinRelease(ProcStructLock);

    return(TRUE);
}

/*
 * ProcKill() -- Destroy the per-proc data structure for
 *	this process. Release any of its held spin locks.
 */
static void
ProcKill(int exitStatus, int pid)
{
    PROC 		*proc;
    SHMEM_OFFSET	location;
    
    /* -------------------- 
     * If this is a FATAL exit the postmaster will have to kill all the
     * existing backends and reinitialize shared memory.  So all we don't 
     * need to do anything here.
     * --------------------
     */
    if (exitStatus != 0)
	return;
    
    if (! pid)
	{
	    pid = getpid();
	}
    
    ShmemPIDLookup(pid,&location);
    if (location == INVALID_OFFSET)
	return;
    
    proc = (PROC *) MAKE_PTR(location);
    
    if (proc != MyProc) {
	Assert( pid != getpid() );
    } else
	MyProc = NULL;
    
    /* ---------------
     * Assume one lock table.
     * ---------------
     */
    ProcReleaseSpins(proc);
    LockReleaseAll(1,&proc->lockQueue);
    
#ifdef USER_LOCKS
    LockReleaseAll(0,&proc->lockQueue);
#endif

    /* ----------------
     * get off the wait queue
     * ----------------
     */
    LockLockTable();
    if (proc->links.next != INVALID_OFFSET) {
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
 * 	and  waking them up
 */

/*
 * ProcQueueAlloc -- alloc/attach to a shared memory process queue
 *
 * Returns: a pointer to the queue or NULL
 * Side Effects: Initializes the queue if we allocated one
 */
PROC_QUEUE *
ProcQueueAlloc(char *name)
{
    bool	found;
    PROC_QUEUE *queue = (PROC_QUEUE *)
	ShmemInitStruct(name,(unsigned)sizeof(PROC_QUEUE),&found);
    
    if (! queue)
	{
	    return(NULL);
	}
    if (! found)
	{
	    ProcQueueInit(queue);
	}
    return(queue);
}

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
 * 	we release the spin lock.
 *
 * NOTES: The process queue is now a priority queue for locking.
 */
int
ProcSleep(PROC_QUEUE *queue,
	  SPINLOCK spinlock,
	  int token,
	  int prio,
	  LOCK *lock)
{
    int 	i;
    PROC	*proc;
#ifndef WIN32 /* figure this out later */
    struct itimerval timeval, dummy;
#endif /* WIN32 */
    
    proc = (PROC *) MAKE_PTR(queue->links.prev);
    for (i=0;i<queue->size;i++)
	{
	    if (proc->prio < prio)
		proc = (PROC *) MAKE_PTR(proc->links.prev);
	    else
		break;
	}
    
    MyProc->token = token;
    MyProc->waitLock = lock;
    
    /* -------------------
     * currently, we only need this for the ProcWakeup routines
     * -------------------
     */
    TransactionIdStore((TransactionId) GetCurrentTransactionId(), &MyProc->xid);
    
    /* -------------------
     * assume that these two operations are atomic (because
     * of the spinlock).
     * -------------------
     */
    SHMQueueInsertTL(&(proc->links),&(MyProc->links));
    queue->size++;
    
    SpinRelease(spinlock);
    
    /* --------------
     * Postgres does not have any deadlock detection code and for this 
     * reason we must set a timer to wake up the process in the event of
     * a deadlock.  For now the timer is set for 1 minute and we assume that
     * any process which sleeps for this amount of time is deadlocked and will 
     * receive a SIGALRM signal.  The handler should release the processes
     * semaphore and abort the current transaction.
     *
     * Need to zero out struct to set the interval and the micro seconds fields
     * to 0.
     * --------------
     */
#ifndef WIN32
    memset(&timeval, 0, sizeof(struct itimerval));
    timeval.it_value.tv_sec = DEADLOCK_TIMEOUT;
    
    if (setitimer(ITIMER_REAL, &timeval, &dummy))
	elog(FATAL, "ProcSleep: Unable to set timer for process wakeup");
#endif /* WIN32 */
    
    /* --------------
     * if someone wakes us between SpinRelease and IpcSemaphoreLock,
     * IpcSemaphoreLock will not block.  The wakeup is "saved" by
     * the semaphore implementation.
     * --------------
     */
    IpcSemaphoreLock(MyProc->sem.semId, MyProc->sem.semNum, IpcExclusiveLock);
    
    /* ---------------
     * We were awoken before a timeout - now disable the timer
     * ---------------
     */
#ifndef WIN32
    timeval.it_value.tv_sec = 0;
    
    
    if (setitimer(ITIMER_REAL, &timeval, &dummy))
	elog(FATAL, "ProcSleep: Unable to diable timer for process wakeup");
#endif /* WIN32 */
    
    /* ----------------
     * We were assumed to be in a critical section when we went
     * to sleep.
     * ----------------
     */
    SpinAcquire(spinlock);
    
    return(MyProc->errType);
}


/*
 * ProcWakeup -- wake up a process by releasing its private semaphore.
 *
 *   remove the process from the wait queue and set its links invalid.
 *   RETURN: the next process in the wait queue.
 */
PROC *
ProcWakeup(PROC *proc, int errType)
{
    PROC *retProc;
    /* assume that spinlock has been acquired */
    
    if (proc->links.prev == INVALID_OFFSET ||
	proc->links.next == INVALID_OFFSET)
	return((PROC *) NULL);
    
    retProc = (PROC *) MAKE_PTR(proc->links.prev);
    
    /* you have to update waitLock->waitProcs.size yourself */
    SHMQueueDelete(&(proc->links));
    SHMQueueElemInit(&(proc->links));
    
    proc->errType = errType;
    
    IpcSemaphoreUnlock(proc->sem.semId, proc->sem.semNum, IpcExclusiveLock);
    
    return retProc;
}


/*
 * ProcGetId --
 */
int
ProcGetId()
{
    return( MyProc->procId );
}

/*
 * ProcLockWakeup -- routine for waking up processes when a lock is
 * 	released.
 */
int
ProcLockWakeup(PROC_QUEUE *queue, char *ltable, char *lock)
{
    PROC	*proc;
    int	count;
    
    if (! queue->size)
	return(STATUS_NOT_FOUND);
    
    proc = (PROC *) MAKE_PTR(queue->links.prev);
    count = 0;
    while ((LockResolveConflicts ((LOCKTAB *) ltable,
				  (LOCK *) lock,
				  proc->token,
				  proc->xid) == STATUS_OK))
	{
	    /* there was a waiting process, grant it the lock before waking it
	     * up.  This will prevent another process from seizing the lock
	     * between the time we release the lock master (spinlock) and
	     * the time that the awoken process begins executing again.
	     */
	    GrantLock((LOCK *) lock, proc->token);
	    queue->size--;
	    
	    /*
	     * ProcWakeup removes proc from the lock waiting process queue and
	     * returns the next proc in chain.  If a writer just dropped
	     * its lock and there are several waiting readers, wake them all up.
	     */
	    proc = ProcWakeup(proc, NO_ERROR);
	    
	    count++;
	    if (!proc || queue->size == 0)
		break;
	}
    
    if (count)
	return(STATUS_OK);
    else
	/* Something is still blocking us.  May have deadlocked. */
	return(STATUS_NOT_FOUND);
}

void
ProcAddLock(SHM_QUEUE *elem)
{
    SHMQueueInsertTL(&MyProc->lockQueue,elem);
}

/* --------------------
 * We only get to this routine if we got SIGALRM after DEADLOCK_TIMEOUT
 * while waiting for a lock to be released by some other process.  After
 * the one minute deadline we assume we have a deadlock and must abort
 * this transaction.  We must also indicate that I'm no longer waiting
 * on a lock so that other processes don't try to wake me up and screw 
 * up my semaphore.
 * --------------------
 */
void
HandleDeadLock(int sig)
{
    LOCK *lock;
    int size;
    
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
	IpcSemaphoreDefaultStartValue) {
	UnlockLockTable();
	return;
    }
    
    /*
     * you would think this would be unnecessary, but...
     *
     * this also means we've been removed already.  in some ports
     * (e.g., sparc and aix) the semop(2) implementation is such that
     * we can actually end up in this handler after someone has removed
     * us from the queue and bopped the semaphore *but the test above
     * fails to detect the semaphore update* (presumably something weird
     * having to do with the order in which the semaphore wakeup signal
     * and SIGALRM get handled).
     */
    if (MyProc->links.prev == INVALID_OFFSET ||
	MyProc->links.next == INVALID_OFFSET) {
	UnlockLockTable();
	return;
    }
    
    lock = MyProc->waitLock;
    size = lock->waitProcs.size; /* so we can look at this in the core */
    
    /* ------------------------
     * Get this process off the lock's wait queue
     * ------------------------
     */
    Assert(lock->waitProcs.size > 0);
    --lock->waitProcs.size;
    SHMQueueDelete(&(MyProc->links));
    SHMQueueElemInit(&(MyProc->links));
    
    /* ------------------
     * Unlock my semaphore so that the count is right for next time.
     * I was awoken by a signal, not by someone unlocking my semaphore.
     * ------------------
     */
    IpcSemaphoreUnlock(MyProc->sem.semId, MyProc->sem.semNum, IpcExclusiveLock);
    
    /* -------------
     * Set MyProc->errType to STATUS_ERROR so that we abort after
     * returning from this handler.
     * -------------
     */
    MyProc->errType = STATUS_ERROR;
    
    /*
     * if this doesn't follow the IpcSemaphoreUnlock then we get lock
     * table corruption ("LockReplace: xid table corrupted") due to
     * race conditions.  i don't claim to understand this...
     */
    UnlockLockTable();
    
    elog(NOTICE, "Timeout -- possible deadlock");
    return;
}

void
ProcReleaseSpins(PROC *proc)
{
    int i;
    
    if (!proc)
	proc = MyProc;
    
    if (!proc)
	return;
    for (i=0; i < (int)MAX_SPINS; i++)
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
 *    scan the free semaphore bitmap and allocate a single semaphore from
 *    a semaphore set. (If the semaphore set doesn't exist yet,
 *    IpcSemaphoreCreate will create it. Otherwise, we use the existing
 *    semaphore set.)
 */
static void
ProcGetNewSemKeyAndNum(IPCKey *key, int *semNum)
{
    int i;
    int32 *freeSemMap = ProcGlobal->freeSemMap;
    unsigned int fullmask;

    /*
     * we hold ProcStructLock when entering this routine. We scan through
     * the bitmap to look for a free semaphore.
     */
    fullmask = ~0 >> (32 - PROC_NSEMS_PER_SET);
    for(i=0; i < MAX_PROC_SEMS/PROC_NSEMS_PER_SET; i++) {
	int mask = 1;
	int j;

	if (freeSemMap[i] == fullmask)
	    continue; /* none free for this set */

	for(j = 0; j < PROC_NSEMS_PER_SET; j++) {
	    if ((freeSemMap[i] & mask) == 0) {
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
    elog(WARN, "InitProc: cannot allocate a free semaphore");
}

/*
 * ProcFreeSem -
 *    free up our semaphore in the semaphore set. If we're the last one
 *    in the set, also remove the semaphore set.
 */
static void
ProcFreeSem(IpcSemaphoreKey semKey, int semNum)
{
    int mask;
    int i;
    int32 *freeSemMap = ProcGlobal->freeSemMap;

    i = semKey - ProcGlobal->currKey;
    mask = ~(1 << semNum);
    freeSemMap[i] &= mask;

    if (freeSemMap[i]==0)
	IpcSemaphoreKill(semKey);
}

/*
 * ProcFreeAllSemaphores -
 *    on exiting the postmaster, we free up all the semaphores allocated
 *    to the lmgrs of the backends.
 */
void
ProcFreeAllSemaphores()
{
    int i;
    int32 *freeSemMap = ProcGlobal->freeSemMap;

    for(i=0; i < MAX_PROC_SEMS/PROC_NSEMS_PER_SET; i++) {
	if (freeSemMap[i]!=0)
	    IpcSemaphoreKill(ProcGlobal->currKey + i);
    }
}
