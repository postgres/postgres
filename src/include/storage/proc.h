/*-------------------------------------------------------------------------
 *
 * proc.h
 *	  per-process shared memory data structures
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: proc.h,v 1.32 2000/11/28 23:27:57 tgl Exp $
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
	int			sleeplock;
	IpcSemaphoreId semId;
	int			semNum;
} SEMA;

/*
 * Each backend has:
 */
typedef struct proc
{
	/* proc->links MUST BE THE FIRST ELEMENT OF STRUCT (see ProcWakeup()) */

	SHM_QUEUE	links;			/* proc can be waiting for one event(lock) */
	SEMA		sem;			/* ONE semaphore to sleep on */
	int			errType;		/* error code tells why we woke up */

	int			critSects;		/* If critSects > 0, we are in sensitive
								 * routines that cannot be recovered when
								 * the process fails. */

	int			prio;			/* priority for sleep queue */

	TransactionId xid;			/* transaction currently being executed by
								 * this proc */

	TransactionId xmin;			/* minimal running XID as it was when we
								 * were starting our xact: vacuum must not
								 * remove tuples deleted by xid >= xmin ! */
	XLogRecPtr	logRec;
	LOCK	   *waitLock;		/* Lock we're sleeping on ... */
	int			token;			/* type of lock we sleeping for */
	int			holdLock;		/* while holding these locks */
	int			pid;			/* This backend's process id */
	Oid			databaseId;		/* OID of database this backend is using */
	short		sLocks[MAX_SPINS];		/* Spin lock stats */
	SHM_QUEUE	lockQueue;		/* locks associated with current
								 * transaction */
} PROC;

extern PROC *MyProc;

#define PROC_INCR_SLOCK(lock) \
do { \
	if (MyProc) (MyProc->sLocks[(lock)])++; \
} while (0)

#define PROC_DECR_SLOCK(lock) \
do { \
	if (MyProc) (MyProc->sLocks[(lock)])--; \
} while (0)

/*
 * flags explaining why process woke up
 */
#define NO_ERROR		0
#define ERR_TIMEOUT		1
#define ERR_BUFFER_IO	2

#define MAX_PRIO		50
#define MIN_PRIO		(-1)

extern SPINLOCK ProcStructLock;


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
extern void ProcReleaseLocks(void);
extern bool ProcRemove(int pid);

extern void ProcQueueInit(PROC_QUEUE *queue);
extern int ProcSleep(PROC_QUEUE *queue, LOCKMETHODCTL *lockctl, int token,
		  LOCK *lock);
extern PROC *ProcWakeup(PROC *proc, int errType);
extern int ProcLockWakeup(PROC_QUEUE *queue, LOCKMETHOD lockmethod,
			   LOCK *lock);
extern void ProcAddLock(SHM_QUEUE *elem);
extern void ProcReleaseSpins(PROC *proc);
extern void LockWaitCancel(void);

#endif	 /* PROC_H */
