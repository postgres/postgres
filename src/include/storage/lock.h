/*-------------------------------------------------------------------------
 *
 * lock.h
 *	  POSTGRES low-level lock mechanism
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: lock.h,v 1.42 2001/01/22 22:30:06 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOCK_H_
#define LOCK_H_

#include "storage/ipc.h"
#include "storage/itemptr.h"
#include "storage/shmem.h"


/* originally in procq.h */
typedef struct PROC_QUEUE
{
	SHM_QUEUE	links;			/* head of list of PROC objects */
	int			size;			/* number of entries in list */
} PROC_QUEUE;

/* struct proc is declared in storage/proc.h, but must forward-reference it */
typedef struct proc PROC;


extern SPINLOCK LockMgrLock;

#ifdef LOCK_DEBUG
extern int  Trace_lock_oidmin;
extern bool Trace_locks;
extern bool Trace_userlocks;
extern int  Trace_lock_table;
extern bool Debug_deadlocks;
#endif /* LOCK_DEBUG */


/* ----------------------
 * The following defines are used to estimate how much shared
 * memory the lock manager is going to require.
 * See LockShmemSize() in lock.c.
 *
 * NLOCKS_PER_XACT - The number of unique objects locked in a transaction
 *					 (this should be configurable!)
 * NLOCKENTS - The maximum number of lock entries in the lock table.
 * ----------------------
 */
#define NLOCKS_PER_XACT			64
#define NLOCKENTS(maxBackends)	(NLOCKS_PER_XACT*(maxBackends))

typedef int LOCKMASK;

typedef int LOCKMODE;
typedef int LOCKMETHOD;

/* MAX_LOCKMODES cannot be larger than the # of bits in LOCKMASK */
#define MAX_LOCKMODES	8

/*
 * MAX_LOCK_METHODS corresponds to the number of spin locks allocated in
 * CreateSpinLocks() or the number of shared memory locations allocated
 * for lock table spin locks in the case of machines with TAS instructions.
 */
#define MAX_LOCK_METHODS	3

#define INVALID_TABLEID		0

#define INVALID_LOCKMETHOD	INVALID_TABLEID
#define DEFAULT_LOCKMETHOD	1
#define USER_LOCKMETHOD		2
#define MIN_LOCKMETHOD		DEFAULT_LOCKMETHOD


/*
 * This is the control structure for a lock table.	It
 * lives in shared memory:
 *
 * lockmethod -- the handle used by the lock table's clients to
 *		refer to the type of lock table being used.
 *
 * numLockModes -- number of lock types (READ,WRITE,etc) that
 *		are defined on this lock table
 *
 * conflictTab -- this is an array of bitmasks showing lock
 *		type conflicts. conflictTab[i] is a mask with the j-th bit
 *		turned on if lock types i and j conflict.
 *
 * prio -- each lockmode has a priority, so, for example, waiting
 *		writers can be given priority over readers (to avoid
 *		starvation).
 *
 * masterlock -- synchronizes access to the table
 */
typedef struct LOCKMETHODCTL
{
	LOCKMETHOD	lockmethod;
	int			numLockModes;
	int			conflictTab[MAX_LOCKMODES];
	int			prio[MAX_LOCKMODES];
	SPINLOCK	masterLock;
} LOCKMETHODCTL;

/*
 * Non-shared header for a lock table.
 *
 * lockHash -- hash table holding per-locked-object lock information
 * holderHash -- hash table holding per-lock-holder lock information
 * ctl - shared control structure described above.
 */
typedef struct LOCKMETHODTABLE
{
	HTAB	   *lockHash;
	HTAB	   *holderHash;
	LOCKMETHODCTL *ctl;
} LOCKMETHODTABLE;


/*
 * LOCKTAG is the key information needed to look up a LOCK item in the
 * lock hashtable.  A LOCKTAG value uniquely identifies a lockable object.
 */
typedef struct LOCKTAG
{
	Oid			relId;
	Oid			dbId;
	union
	{
		BlockNumber blkno;
		TransactionId xid;
	}			objId;

	/*
	 * offnum should be part of objId.tupleId above, but would increase
	 * sizeof(LOCKTAG) and so moved here; currently used by userlocks
	 * only.
	 */
	OffsetNumber offnum;

	uint16		lockmethod;		/* needed by userlocks */
} LOCKTAG;


/*
 * Per-locked-object lock information:
 *
 * tag -- uniquely identifies the object being locked
 * grantMask -- bitmask for all lock types currently granted on this object.
 * waitMask -- bitmask for all lock types currently awaited on this object.
 * lockHolders -- list of HOLDER objects for this lock.
 * waitProcs -- queue of processes waiting for this lock.
 * requested -- count of each lock type currently requested on the lock
 *		(includes requests already granted!!).
 * nRequested -- total requested locks of all types.
 * granted -- count of each lock type currently granted on the lock.
 * nGranted -- total granted locks of all types.
 */
typedef struct LOCK
{
	/* hash key */
	LOCKTAG		tag;			/* unique identifier of lockable object */

	/* data */
	int			grantMask;		/* bitmask for lock types already granted */
	int			waitMask;		/* bitmask for lock types awaited */
	SHM_QUEUE	lockHolders;	/* list of HOLDER objects assoc. with lock */
	PROC_QUEUE	waitProcs;		/* list of PROC objects waiting on lock */
	int			requested[MAX_LOCKMODES]; /* counts of requested locks */
	int			nRequested;		/* total of requested[] array */
	int			granted[MAX_LOCKMODES];	/* counts of granted locks */
	int			nGranted;		/* total of granted[] array */
} LOCK;

#define SHMEM_LOCKTAB_KEYSIZE  sizeof(LOCKTAG)
#define SHMEM_LOCKTAB_DATASIZE (sizeof(LOCK) - SHMEM_LOCKTAB_KEYSIZE)

#define LOCK_LOCKMETHOD(lock) ((lock).tag.lockmethod)


/*
 * We may have several different transactions holding or awaiting locks
 * on the same lockable object.  We need to store some per-holder information
 * for each such holder (or would-be holder).
 *
 * HOLDERTAG is the key information needed to look up a HOLDER item in the
 * holder hashtable.  A HOLDERTAG value uniquely identifies a lock holder.
 *
 * There are two possible kinds of holder tags: a transaction (identified
 * both by the PROC of the backend running it, and the xact's own ID) and
 * a session (identified by backend PROC, with xid = InvalidTransactionId).
 *
 * Currently, session holders are used for user locks and for cross-xact
 * locks obtained for VACUUM.  We assume that a session lock never conflicts
 * with per-transaction locks obtained by the same backend.
 *
 * The holding[] array counts the granted locks (of each type) represented
 * by this holder.  Note that there will be a holder object, possibly with
 * zero holding[], for any lock that the process is currently waiting on.
 * Otherwise, holder objects whose counts have gone to zero are recycled
 * as soon as convenient.
 *
 * Each HOLDER object is linked into lists for both the associated LOCK object
 * and the owning PROC object.  Note that the HOLDER is entered into these
 * lists as soon as it is created, even if no lock has yet been granted.
 * A PROC that is waiting for a lock to be granted will also be linked into
 * the lock's waitProcs queue.
 */
typedef struct HOLDERTAG
{
	SHMEM_OFFSET lock;			/* link to per-lockable-object information */
	SHMEM_OFFSET proc;			/* link to PROC of owning backend */
	TransactionId xid;			/* xact ID, or InvalidTransactionId */
} HOLDERTAG;

typedef struct HOLDER
{
	/* tag */
	HOLDERTAG	tag;			/* unique identifier of holder object */

	/* data */
	int			holding[MAX_LOCKMODES];	/* count of locks currently held */
	int			nHolding;		/* total of holding[] array */
	SHM_QUEUE	lockLink;		/* list link for lock's list of holders */
	SHM_QUEUE	procLink;		/* list link for process's list of holders */
} HOLDER;

#define SHMEM_HOLDERTAB_KEYSIZE  sizeof(HOLDERTAG)
#define SHMEM_HOLDERTAB_DATASIZE (sizeof(HOLDER) - SHMEM_HOLDERTAB_KEYSIZE)

#define HOLDER_LOCKMETHOD(holder) \
		(((LOCK *) MAKE_PTR((holder).tag.lock))->tag.lockmethod)



#define LockLockTable() SpinAcquire(LockMgrLock)
#define UnlockLockTable() SpinRelease(LockMgrLock)


/*
 * function prototypes
 */
extern void InitLocks(void);
extern void LockDisable(bool status);
extern bool LockingDisabled(void);
extern LOCKMETHOD LockMethodTableInit(char *tabName, LOCKMASK *conflictsP,
					int *prioP, int numModes, int maxBackends);
extern LOCKMETHOD LockMethodTableRename(LOCKMETHOD lockmethod);
extern bool LockAcquire(LOCKMETHOD lockmethod, LOCKTAG *locktag,
						TransactionId xid, LOCKMODE lockmode);
extern bool LockRelease(LOCKMETHOD lockmethod, LOCKTAG *locktag,
						TransactionId xid, LOCKMODE lockmode);
extern bool LockReleaseAll(LOCKMETHOD lockmethod, PROC *proc,
						   bool allxids, TransactionId xid);
extern int LockResolveConflicts(LOCKMETHOD lockmethod, LOCKMODE lockmode,
								LOCK *lock, HOLDER *holder, PROC *proc,
								int *myHolding);
extern void GrantLock(LOCK *lock, HOLDER *holder, LOCKMODE lockmode);
extern int	LockShmemSize(int maxBackends);
extern bool DeadLockCheck(PROC *thisProc, LOCK *findlock);

#ifdef LOCK_DEBUG
extern void DumpLocks(void);
extern void DumpAllLocks(void);
#endif

#endif	 /* LOCK_H */
