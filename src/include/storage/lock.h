/*-------------------------------------------------------------------------
 *
 * lock.h
 *	  POSTGRES low-level lock mechanism
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/lock.h,v 1.80 2004/08/26 17:22:28 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOCK_H_
#define LOCK_H_

#include "storage/itemptr.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"


/* originally in procq.h */
typedef struct PROC_QUEUE
{
	SHM_QUEUE	links;			/* head of list of PGPROC objects */
	int			size;			/* number of entries in list */
} PROC_QUEUE;

/* struct PGPROC is declared in proc.h, but must forward-reference it */
typedef struct PGPROC PGPROC;

/* GUC variables */
extern int	max_locks_per_xact;

#ifdef LOCK_DEBUG
extern int	Trace_lock_oidmin;
extern bool Trace_locks;
extern bool Trace_userlocks;
extern int	Trace_lock_table;
extern bool Debug_deadlocks;
#endif   /* LOCK_DEBUG */


/*
 * LOCKMODE is an integer (1..N) indicating a lock type.  LOCKMASK is a bit
 * mask indicating a set of held or requested lock types (the bit 1<<mode
 * corresponds to a particular lock mode).
 */
typedef int LOCKMASK;
typedef int LOCKMODE;
/* MAX_LOCKMODES cannot be larger than the # of bits in LOCKMASK */
#define MAX_LOCKMODES		10

#define LOCKBIT_ON(lockmode) (1 << (lockmode))
#define LOCKBIT_OFF(lockmode) (~(1 << (lockmode)))

/*
 * There is normally only one lock method, the default one.
 * If user locks are enabled, an additional lock method is present.
 * Lock methods are identified by LOCKMETHODID.
 */
typedef uint16 LOCKMETHODID;
/* MAX_LOCK_METHODS is the number of distinct lock control tables allowed */
#define MAX_LOCK_METHODS	3

#define INVALID_LOCKMETHOD	0
#define DEFAULT_LOCKMETHOD	1
#define USER_LOCKMETHOD		2

#define LockMethodIsValid(lockmethodid) ((lockmethodid) != INVALID_LOCKMETHOD)

/*
 * This is the control structure for a lock table. It lives in shared
 * memory.  Currently, none of these fields change after startup.  In addition
 * to the LockMethodData, a lock table has a "lockHash" table holding
 * per-locked-object lock information, and a "proclockHash" table holding
 * per-lock-holder/waiter lock information.
 *
 * lockmethodid -- the handle used by the lock table's clients to
 *		refer to the type of lock table being used.
 *
 * numLockModes -- number of lock types (READ,WRITE,etc) that
 *		are defined on this lock table
 *
 * conflictTab -- this is an array of bitmasks showing lock
 *		type conflicts. conflictTab[i] is a mask with the j-th bit
 *		turned on if lock types i and j conflict.
 *
 * masterLock -- LWLock used to synchronize access to the table
 */
typedef struct LockMethodData
{
	LOCKMETHODID	lockmethodid;
	int				numLockModes;
	LOCKMASK		conflictTab[MAX_LOCKMODES];
	LWLockId		masterLock;
} LockMethodData;
typedef LockMethodData *LockMethod;


/*
 * LOCKTAG is the key information needed to look up a LOCK item in the
 * lock hashtable.	A LOCKTAG value uniquely identifies a lockable object.
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

	LOCKMETHODID lockmethodid;		/* needed by userlocks */
} LOCKTAG;


/*
 * Per-locked-object lock information:
 *
 * tag -- uniquely identifies the object being locked
 * grantMask -- bitmask for all lock types currently granted on this object.
 * waitMask -- bitmask for all lock types currently awaited on this object.
 * lockHolders -- list of PROCLOCK objects for this lock.
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
	LOCKMASK	grantMask;		/* bitmask for lock types already granted */
	LOCKMASK	waitMask;		/* bitmask for lock types awaited */
	SHM_QUEUE	lockHolders;	/* list of PROCLOCK objects assoc. with
								 * lock */
	PROC_QUEUE	waitProcs;		/* list of PGPROC objects waiting on lock */
	int			requested[MAX_LOCKMODES];		/* counts of requested
												 * locks */
	int			nRequested;		/* total of requested[] array */
	int			granted[MAX_LOCKMODES]; /* counts of granted locks */
	int			nGranted;		/* total of granted[] array */
} LOCK;

#define LOCK_LOCKMETHOD(lock) ((lock).tag.lockmethodid)


/*
 * We may have several different transactions holding or awaiting locks
 * on the same lockable object.  We need to store some per-holder/waiter
 * information for each such holder (or would-be holder).  This is kept in
 * a PROCLOCK struct.
 *
 * PROCLOCKTAG is the key information needed to look up a PROCLOCK item in the
 * proclock hashtable.	A PROCLOCKTAG value uniquely identifies the combination
 * of a lockable object and a holder/waiter for that object.
 *
 * There are two possible kinds of proclock tags: a transaction (identified
 * both by the PGPROC of the backend running it, and the xact's own ID) and
 * a session (identified by backend PGPROC, with XID = InvalidTransactionId).
 *
 * Currently, session proclocks are used for user locks and for cross-xact
 * locks obtained for VACUUM.  Note that a single backend can hold locks
 * under several different XIDs at once (including session locks).  We treat
 * such locks as never conflicting (a backend can never block itself).
 *
 * The holding[] array counts the granted locks (of each type) represented
 * by this proclock. Note that there will be a proclock object, possibly with
 * zero holding[], for any lock that the process is currently waiting on.
 * Otherwise, proclock objects whose counts have gone to zero are recycled
 * as soon as convenient.
 *
 * Each PROCLOCK object is linked into lists for both the associated LOCK
 * object and the owning PGPROC object.  Note that the PROCLOCK is entered
 * into these lists as soon as it is created, even if no lock has yet been
 * granted.  A PGPROC that is waiting for a lock to be granted will also be
 * linked into the lock's waitProcs queue.
 */
typedef struct PROCLOCKTAG
{
	SHMEM_OFFSET lock;			/* link to per-lockable-object information */
	SHMEM_OFFSET proc;			/* link to PGPROC of owning backend */
	TransactionId xid;			/* xact ID, or InvalidTransactionId */
} PROCLOCKTAG;

typedef struct PROCLOCK
{
	/* tag */
	PROCLOCKTAG tag;			/* unique identifier of proclock object */

	/* data */
	int			holding[MAX_LOCKMODES]; /* count of locks currently held */
	int			nHolding;		/* total of holding[] array */
	SHM_QUEUE	lockLink;		/* list link for lock's list of proclocks */
	SHM_QUEUE	procLink;		/* list link for process's list of
								 * proclocks */
} PROCLOCK;

#define PROCLOCK_LOCKMETHOD(proclock) \
		(((LOCK *) MAKE_PTR((proclock).tag.lock))->tag.lockmethodid)

/*
 * This struct holds information passed from lmgr internals to the lock
 * listing user-level functions (lockfuncs.c).	For each PROCLOCK in the
 * system, the SHMEM_OFFSET, PROCLOCK itself, and associated PGPROC and
 * LOCK objects are stored.  (Note there will often be multiple copies
 * of the same PGPROC or LOCK.)  We do not store the SHMEM_OFFSET of the
 * PGPROC or LOCK separately, since they're in the PROCLOCK's tag fields.
 */
typedef struct
{
	int			nelements;		/* The length of each of the arrays */
	SHMEM_OFFSET *proclockaddrs;
	PROCLOCK   *proclocks;
	PGPROC	   *procs;
	LOCK	   *locks;
} LockData;

extern int NumLockMethods;

/*
 * function prototypes
 */
extern void InitLocks(void);
extern LockMethod GetLocksMethodTable(LOCK *lock);
extern LOCKMETHODID LockMethodTableInit(const char *tabName,
										const LOCKMASK *conflictsP,
										int numModes, int maxBackends);
extern LOCKMETHODID LockMethodTableRename(LOCKMETHODID lockmethodid);
extern bool LockAcquire(LOCKMETHODID lockmethodid, LOCKTAG *locktag,
			TransactionId xid, LOCKMODE lockmode, bool dontWait);
extern bool LockRelease(LOCKMETHODID lockmethodid, LOCKTAG *locktag,
			TransactionId xid, LOCKMODE lockmode);
extern bool LockReleaseAll(LOCKMETHODID lockmethodid, PGPROC *proc,
						   bool allxids);
extern int LockCheckConflicts(LockMethod lockMethodTable,
				   LOCKMODE lockmode,
				   LOCK *lock, PROCLOCK *proclock, PGPROC *proc,
				   int *myHolding);
extern void GrantLock(LOCK *lock, PROCLOCK *proclock, LOCKMODE lockmode);
extern void RemoveFromWaitQueue(PGPROC *proc);
extern int	LockShmemSize(int maxBackends);
extern bool DeadLockCheck(PGPROC *proc);
extern void DeadLockReport(void);
extern void RememberSimpleDeadLock(PGPROC *proc1,
					   LOCKMODE lockmode,
					   LOCK *lock,
					   PGPROC *proc2);
extern void InitDeadLockChecking(void);
extern LockData *GetLockStatusData(void);
extern const char *GetLockmodeName(LOCKMODE mode);

#ifdef LOCK_DEBUG
extern void DumpLocks(void);
extern void DumpAllLocks(void);
#endif

#endif   /* LOCK_H */
