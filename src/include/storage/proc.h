/*-------------------------------------------------------------------------
 *
 * proc.h
 *
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: proc.h,v 1.28 2000/01/26 05:58:33 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PROC_H_
#define _PROC_H_

#include "access/xlog.h"
#include "storage/lock.h"

typedef struct
{
	int			sleeplock;
	int			semNum;
	IpcSemaphoreId semId;
	IpcSemaphoreKey semKey;
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


/*
 * PROC_NSEMS_PER_SET is the number of semaphores in each sys-V semaphore set
 * we allocate.  It must be *less than* 32 (or however many bits in an int
 * on your machine), or our free-semaphores bitmap won't work.  You also must
 * not set it higher than your kernel's SEMMSL (max semaphores per set)
 * parameter, which is often around 25.
 *
 * MAX_PROC_SEMS is the maximum number of per-process semaphores (those used
 * by the lock mgr) we can keep track of.  It must be a multiple of
 * PROC_NSEMS_PER_SET.
 */
#define  PROC_NSEMS_PER_SET		16
#define  MAX_PROC_SEMS			(((MAXBACKENDS-1)/PROC_NSEMS_PER_SET+1)*PROC_NSEMS_PER_SET)

typedef struct procglobal
{
	SHMEM_OFFSET freeProcs;
	IPCKey		currKey;
	int32		freeSemMap[MAX_PROC_SEMS / PROC_NSEMS_PER_SET];

	/*
	 * In each freeSemMap entry, the PROC_NSEMS_PER_SET least-significant bits
	 * flag whether individual semaphores are in use, and the next higher bit
	 * is set to show that the entire set is allocated.
	 */
} PROC_HDR;

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
 * Function Prototypes
 */
extern void InitProcess(IPCKey key);
extern void ProcReleaseLocks(void);
extern bool ProcRemove(int pid);

/* extern bool ProcKill(int exitStatus, int pid); */
/* make static in storage/lmgr/proc.c -- jolly */

extern void ProcQueueInit(PROC_QUEUE *queue);
extern int ProcSleep(PROC_QUEUE *queue, LOCKMETHODCTL *lockctl, int token,
		  LOCK *lock);
extern PROC *ProcWakeup(PROC *proc, int errType);
extern int ProcLockWakeup(PROC_QUEUE *queue, LOCKMETHOD lockmethod,
			   LOCK *lock);
extern void ProcAddLock(SHM_QUEUE *elem);
extern void ProcReleaseSpins(PROC *proc);

#endif	 /* PROC_H */
