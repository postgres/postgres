/*-------------------------------------------------------------------------
 *
 * proc.h
 *	  per-process shared memory data structures
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: proc.h,v 1.55 2002/05/05 00:03:29 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PROC_H_
#define _PROC_H_

#include "access/xlog.h"
#include "storage/backendid.h"
#include "storage/lock.h"
#include "storage/pg_sema.h"


/*
 * Each backend has a PROC struct in shared memory.  There is also a list of
 * currently-unused PROC structs that will be reallocated to new backends.
 *
 * links: list link for any list the PROC is in.  When waiting for a lock,
 * the PROC is linked into that lock's waitProcs queue.  A recycled PROC
 * is linked into ProcGlobal's freeProcs list.
 */
struct PROC
{
	/* proc->links MUST BE FIRST IN STRUCT (see ProcSleep,ProcWakeup,etc) */
	SHM_QUEUE	links;			/* list link if process is in a list */

	PGSemaphoreData sem;		/* ONE semaphore to sleep on */
	int			errType;		/* STATUS_OK or STATUS_ERROR after wakeup */

	TransactionId xid;			/* transaction currently being executed by
								 * this proc */

	TransactionId xmin;			/* minimal running XID as it was when we
								 * were starting our xact: vacuum must not
								 * remove tuples deleted by xid >= xmin ! */

	int			pid;			/* This backend's process id */
	Oid			databaseId;		/* OID of database this backend is using */

	/*
	 * XLOG location of first XLOG record written by this backend's
	 * current transaction.  If backend is not in a transaction or hasn't
	 * yet modified anything, logRec.xrecoff is zero.
	 */
	XLogRecPtr	logRec;

	/* Info about LWLock the process is currently waiting for, if any. */
	bool		lwWaiting;		/* true if waiting for an LW lock */
	bool		lwExclusive;	/* true if waiting for exclusive access */
	struct PROC *lwWaitLink;	/* next waiter for same LW lock */

	/* Info about lock the process is currently waiting for, if any. */
	/* waitLock and waitHolder are NULL if not currently waiting. */
	LOCK	   *waitLock;		/* Lock object we're sleeping on ... */
	HOLDER	   *waitHolder;		/* Per-holder info for awaited lock */
	LOCKMODE	waitLockMode;	/* type of lock we're waiting for */
	LOCKMASK	heldLocks;		/* bitmask for lock types already held on
								 * this lock object by this backend */

	SHM_QUEUE	procHolders;	/* list of HOLDER objects for locks held
								 * or awaited by this backend */
};

/* NOTE: "typedef struct PROC PROC" appears in storage/lock.h. */


extern PROC *MyProc;


/*
 * There is one ProcGlobal struct for the whole installation.
 */
typedef struct PROC_HDR
{
	/* Head of list of free PROC structures */
	SHMEM_OFFSET freeProcs;
} PROC_HDR;


/* configurable option */
extern int	DeadlockTimeout;


/*
 * Function Prototypes
 */
extern int	ProcGlobalSemas(int maxBackends);
extern void InitProcGlobal(int maxBackends);
extern void InitProcess(void);
extern void InitDummyProcess(void);
extern void ProcReleaseLocks(bool isCommit);

extern void ProcQueueInit(PROC_QUEUE *queue);
extern int ProcSleep(LOCKMETHODTABLE *lockMethodTable, LOCKMODE lockmode,
		  LOCK *lock, HOLDER *holder);
extern PROC *ProcWakeup(PROC *proc, int errType);
extern void ProcLockWakeup(LOCKMETHODTABLE *lockMethodTable, LOCK *lock);
extern bool LockWaitCancel(void);
extern void HandleDeadLock(SIGNAL_ARGS);

extern void ProcWaitForSignal(void);
extern void ProcCancelWaitForSignal(void);
extern void ProcSendSignal(BackendId procId);

extern bool enable_sigalrm_interrupt(int delayms);
extern bool disable_sigalrm_interrupt(void);

#endif   /* PROC_H */
