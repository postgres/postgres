/*-------------------------------------------------------------------------
 *
 * proc.h
 *	  per-process shared memory data structures
 *
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/proc.h,v 1.75 2004/08/29 05:06:58 momjian Exp $
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
 * Each backend advertises up to PGPROC_MAX_CACHED_SUBXIDS TransactionIds
 * for non-aborted subtransactions of its current top transaction.	These
 * have to be treated as running XIDs by other backends.
 *
 * We also keep track of whether the cache overflowed (ie, the transaction has
 * generated at least one subtransaction that didn't fit in the cache).
 * If none of the caches have overflowed, we can assume that an XID that's not
 * listed anywhere in the PGPROC array is not a running transaction.  Else we
 * have to look at pg_subtrans.
 */
#define PGPROC_MAX_CACHED_SUBXIDS 64	/* XXX guessed-at value */

struct XidCache
{
	bool		overflowed;
	int			nxids;
	TransactionId xids[PGPROC_MAX_CACHED_SUBXIDS];
};

/*
 * Each backend has a PGPROC struct in shared memory.  There is also a list of
 * currently-unused PGPROC structs that will be reallocated to new backends.
 *
 * links: list link for any list the PGPROC is in.	When waiting for a lock,
 * the PGPROC is linked into that lock's waitProcs queue.  A recycled PGPROC
 * is linked into ProcGlobal's freeProcs list.
 */
struct PGPROC
{
	/* proc->links MUST BE FIRST IN STRUCT (see ProcSleep,ProcWakeup,etc) */
	SHM_QUEUE	links;			/* list link if process is in a list */

	PGSemaphoreData sem;		/* ONE semaphore to sleep on */
	int			waitStatus;		/* STATUS_OK or STATUS_ERROR after wakeup */

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
	struct PGPROC *lwWaitLink;	/* next waiter for same LW lock */

	/* Info about lock the process is currently waiting for, if any. */
	/* waitLock and waitProcLock are NULL if not currently waiting. */
	LOCK	   *waitLock;		/* Lock object we're sleeping on ... */
	PROCLOCK   *waitProcLock;	/* Per-holder info for awaited lock */
	LOCKMODE	waitLockMode;	/* type of lock we're waiting for */
	LOCKMASK	heldLocks;		/* bitmask for lock types already held on
								 * this lock object by this backend */

	SHM_QUEUE	procLocks;		/* list of PROCLOCK objects for locks held
								 * or awaited by this backend */

	struct XidCache subxids;	/* cache for subtransaction XIDs */
};

/* NOTE: "typedef struct PGPROC PGPROC" appears in storage/lock.h. */


extern DLLIMPORT PGPROC *MyProc;


/*
 * There is one ProcGlobal struct for the whole installation.
 */
typedef struct PROC_HDR
{
	/* Head of list of free PGPROC structures */
	SHMEM_OFFSET freeProcs;
} PROC_HDR;


#define DUMMY_PROC_DEFAULT	0
#define DUMMY_PROC_BGWRITER 1
#define NUM_DUMMY_PROCS		2


/* configurable options */
extern int	DeadlockTimeout;
extern int	StatementTimeout;


/*
 * Function Prototypes
 */
extern int	ProcGlobalSemas(int maxBackends);
extern void InitProcGlobal(int maxBackends);
extern void InitProcess(void);
extern void InitDummyProcess(int proctype);
extern void ProcReleaseLocks(bool isCommit);

extern void ProcQueueInit(PROC_QUEUE *queue);
extern int ProcSleep(LockMethod lockMethodTable, LOCKMODE lockmode,
		  LOCK *lock, PROCLOCK *proclock);
extern PGPROC *ProcWakeup(PGPROC *proc, int waitStatus);
extern void ProcLockWakeup(LockMethod lockMethodTable, LOCK *lock);
extern bool LockWaitCancel(void);

extern void ProcWaitForSignal(void);
extern void ProcCancelWaitForSignal(void);
extern void ProcSendSignal(BackendId procId);

extern bool enable_sig_alarm(int delayms, bool is_statement_timeout);
extern bool disable_sig_alarm(bool is_statement_timeout);
extern void handle_sig_alarm(SIGNAL_ARGS);

#endif   /* PROC_H */
