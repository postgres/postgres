/*-------------------------------------------------------------------------
 *
 * proc.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: proc.h,v 1.9 1998/01/23 06:01:25 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PROC_H_
#define _PROC_H_

#include <storage/lock.h>

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

	int			procId;			/* unique number for this structure NOT
								 * unique per backend, these things are
								 * reused after the backend dies. */

	int			critSects;		/* If critSects > 0, we are in sensitive
								 * routines that cannot be recovered when
								 * the process fails. */

	int			prio;			/* priority for sleep queue */

	TransactionId xid;			/* transaction currently being executed by
								 * this proc */

	LOCK	   *waitLock;		/* Lock we're sleeping on */
	int			token;			/* info for proc wakeup routines */
	int			pid;			/* This procs process id */
	short		sLocks[MAX_SPINS];		/* Spin lock stats */
	SHM_QUEUE	lockQueue;		/* locks associated with current
								 * transaction */
} PROC;


/*
 * MAX_PROC_SEMS is the maximum number of per-process semaphores (those used
 * by the lock mgr) we can keep track of. PROC_NSEMS_PER_SET is the number
 * of semaphores in each (sys-V) semaphore set allocated. (Be careful not
 * to set it to greater 32. Otherwise, the bitmap will overflow.)
 */
#define  MAX_PROC_SEMS			128
#define  PROC_NSEMS_PER_SET		16

typedef struct procglobal
{
	SHMEM_OFFSET freeProcs;
	int			numProcs;
	IPCKey		currKey;
	int32		freeSemMap[MAX_PROC_SEMS / PROC_NSEMS_PER_SET];
} PROC_HDR;

extern PROC *MyProc;

#define PROC_INCR_SLOCK(lock) if (MyProc) (MyProc->sLocks[(lock)])++
#define PROC_DECR_SLOCK(lock) if (MyProc) (MyProc->sLocks[(lock)])--

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
extern int ProcSleep(PROC_QUEUE *queue, SPINLOCK spinlock, int token,
		  int prio, LOCK *lock);
extern int	ProcLockWakeup(PROC_QUEUE *queue, char *ltable, char *lock);
extern void ProcAddLock(SHM_QUEUE *elem);
extern void ProcReleaseSpins(PROC *proc);
extern void ProcFreeAllSemaphores(void);

#endif							/* PROC_H */
