/*-------------------------------------------------------------------------
 *
 * proc.h
 *	  per-process shared memory data structures
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: proc.h,v 1.39 2001/01/25 03:31:16 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PROC_H_
#define _PROC_H_

#include "access/xlog.h"
#include "storage/lock.h"

/* configurable option */
extern int DeadlockTimeout;

typedef struct
{
	IpcSemaphoreId semId;		/* SysV semaphore set ID */
	int			semNum;			/* semaphore number within set */
} SEMA;

/*
 * Each backend has a PROC struct in shared memory.  There is also a list of
 * currently-unused PROC structs that will be reallocated to new backends.
 *
 * links: list link for any list the PROC is in.  When waiting for a lock,
 * the PROC is linked into that lock's waitProcs queue.  A recycled PROC
 * is linked into ProcGlobal's freeProcs list.
 */
struct proc
{
	/* proc->links MUST BE FIRST IN STRUCT (see ProcSleep,ProcWakeup,etc) */

	SHM_QUEUE	links;			/* list link if process is in a list */

	SEMA		sem;			/* ONE semaphore to sleep on */
	int			errType;		/* STATUS_OK or STATUS_ERROR after wakeup */

	TransactionId xid;			/* transaction currently being executed by
								 * this proc */

	TransactionId xmin;			/* minimal running XID as it was when we
								 * were starting our xact: vacuum must not
								 * remove tuples deleted by xid >= xmin ! */

	XLogRecPtr	logRec;

	/* Info about lock the process is currently waiting for, if any. */
	/* waitLock and waitHolder are NULL if not currently waiting. */
	LOCK	   *waitLock;		/* Lock object we're sleeping on ... */
	HOLDER	   *waitHolder;		/* Per-holder info for awaited lock */
	LOCKMODE	waitLockMode;	/* type of lock we're waiting for */
	LOCKMASK	heldLocks;		/* bitmask for lock types already held on
								 * this lock object by this backend */

	int			pid;			/* This backend's process id */
	Oid			databaseId;		/* OID of database this backend is using */

	short		sLocks[MAX_SPINS];		/* Spin lock stats */
	SHM_QUEUE	procHolders;	/* list of HOLDER objects for locks held or
								 * awaited by this backend */
};

/* NOTE: "typedef struct proc PROC" appears in storage/lock.h. */


extern PROC *MyProc;

extern SPINLOCK ProcStructLock;


#define PROC_INCR_SLOCK(lock) \
do { \
	if (MyProc) (MyProc->sLocks[(lock)])++; \
} while (0)

#define PROC_DECR_SLOCK(lock) \
do { \
	if (MyProc) (MyProc->sLocks[(lock)])--; \
} while (0)


/*
 * There is one ProcGlobal struct for the whole installation.
 *
 * PROC_NSEMS_PER_SET is the number of semaphores in each sys-V semaphore set
 * we allocate.  It must be no more than 32 (or however many bits in an int
 * on your machine), or our free-semaphores bitmap won't work.  It also must
 * be *less than* your kernel's SEMMSL (max semaphores per set) parameter,
 * which is often around 25.  (Less than, because we allocate one extra sema
 * in each set for identification purposes.)
 *
 * PROC_SEM_MAP_ENTRIES is the number of semaphore sets we need to allocate
 * to keep track of up to MAXBACKENDS backends.
 */
#define  PROC_NSEMS_PER_SET		16
#define  PROC_SEM_MAP_ENTRIES	((MAXBACKENDS-1)/PROC_NSEMS_PER_SET+1)

typedef struct procglobal
{
	/* Head of list of free PROC structures */
	SHMEM_OFFSET freeProcs;

	/* Info about semaphore sets used for per-process semaphores */
	IpcSemaphoreId procSemIds[PROC_SEM_MAP_ENTRIES];
	int32		freeSemMap[PROC_SEM_MAP_ENTRIES];

	/*
	 * In each freeSemMap entry, bit i is set if the i'th semaphore of the
	 * set is allocated to a process.  (i counts from 0 at the LSB)
	 */
} PROC_HDR;

/*
 * Function Prototypes
 */
extern void InitProcGlobal(int maxBackends);
extern void InitProcess(void);
extern void ProcReleaseLocks(bool isCommit);
extern bool ProcRemove(int pid);

extern void ProcQueueInit(PROC_QUEUE *queue);
extern int ProcSleep(LOCKMETHODTABLE *lockMethodTable, LOCKMODE lockmode,
					 LOCK *lock, HOLDER *holder);
extern PROC *ProcWakeup(PROC *proc, int errType);
extern void ProcLockWakeup(LOCKMETHODTABLE *lockMethodTable, LOCK *lock);
extern void ProcReleaseSpins(PROC *proc);
extern bool LockWaitCancel(void);
extern void HandleDeadLock(SIGNAL_ARGS);

#endif	 /* PROC_H */
