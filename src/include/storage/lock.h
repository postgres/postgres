/*-------------------------------------------------------------------------
 *
 * lock.h
 *	  POSTGRES low-level lock mechanism
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/lock.h,v 1.84 2004/12/31 22:03:42 pgsql Exp $
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

extern int	NumLockMethods;


/*
 * This is the control structure for a lock table. It lives in shared
 * memory.	Currently, none of these fields change after startup.  In addition
 * to the LockMethodData, a lock table has a shared "lockHash" table holding
 * per-locked-object lock information, and a shared "proclockHash" table
 * holding per-lock-holder/waiter lock information.
 *
 * masterLock -- LWLock used to synchronize access to the table
 *
 * numLockModes -- number of lock types (READ,WRITE,etc) that
 *		are defined on this lock table
 *
 * conflictTab -- this is an array of bitmasks showing lock
 *		type conflicts. conflictTab[i] is a mask with the j-th bit
 *		turned on if lock types i and j conflict.
 */
typedef struct LockMethodData
{
	LWLockId	masterLock;
	int			numLockModes;
	LOCKMASK	conflictTab[MAX_LOCKMODES];
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
	 * offnum should be part of objId union above, but doing that would
	 * increase sizeof(LOCKTAG) due to padding.  Currently used by
	 * userlocks only.
	 */
	OffsetNumber offnum;

	LOCKMETHODID lockmethodid;	/* needed by userlocks */
} LOCKTAG;


/*
 * Per-locked-object lock information:
 *
 * tag -- uniquely identifies the object being locked
 * grantMask -- bitmask for all lock types currently granted on this object.
 * waitMask -- bitmask for all lock types currently awaited on this object.
 * procLocks -- list of PROCLOCK objects for this lock.
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
	SHM_QUEUE	procLocks;		/* list of PROCLOCK objects assoc. with
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
 * There are two possible kinds of proclock owners: a transaction (identified
 * both by the PGPROC of the backend running it, and the xact's own ID) and
 * a session (identified by backend PGPROC, with XID = InvalidTransactionId).
 *
 * Currently, session proclocks are used for user locks and for cross-xact
 * locks obtained for VACUUM.  Note that a single backend can hold locks
 * under several different XIDs at once (including session locks).	We treat
 * such locks as never conflicting (a backend can never block itself).
 *
 * The holdMask field shows the already-granted locks represented by this
 * proclock.  Note that there will be a proclock object, possibly with
 * zero holdMask, for any lock that the process is currently waiting on.
 * Otherwise, proclock objects whose holdMasks are zero are recycled
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
	LOCKMASK	holdMask;		/* bitmask for lock types currently held */
	SHM_QUEUE	lockLink;		/* list link for lock's list of proclocks */
	SHM_QUEUE	procLink;		/* list link for process's list of
								 * proclocks */
} PROCLOCK;

#define PROCLOCK_LOCKMETHOD(proclock) \
		(((LOCK *) MAKE_PTR((proclock).tag.lock))->tag.lockmethodid)

/*
 * Each backend also maintains a local hash table with information about each
 * lock it is currently interested in.	In particular the local table counts
 * the number of times that lock has been acquired.  This allows multiple
 * requests for the same lock to be executed without additional accesses to
 * shared memory.  We also track the number of lock acquisitions per
 * ResourceOwner, so that we can release just those locks belonging to a
 * particular ResourceOwner.
 */
typedef struct LOCALLOCKTAG
{
	LOCKTAG		lock;			/* identifies the lockable object */
	TransactionId xid;			/* xact ID, or InvalidTransactionId */
	LOCKMODE	mode;			/* lock mode for this table entry */
} LOCALLOCKTAG;

typedef struct LOCALLOCKOWNER
{
	/*
	 * Note: owner can be NULL to indicate a non-transactional lock. Must
	 * use a forward struct reference to avoid circularity.
	 */
	struct ResourceOwnerData *owner;
	int			nLocks;			/* # of times held by this owner */
} LOCALLOCKOWNER;

typedef struct LOCALLOCK
{
	/* tag */
	LOCALLOCKTAG tag;			/* unique identifier of locallock entry */

	/* data */
	LOCK	   *lock;			/* associated LOCK object in shared mem */
	PROCLOCK   *proclock;		/* associated PROCLOCK object in shmem */
	int			nLocks;			/* total number of times lock is held */
	int			numLockOwners;	/* # of relevant ResourceOwners */
	int			maxLockOwners;	/* allocated size of array */
	LOCALLOCKOWNER *lockOwners; /* dynamically resizable array */
} LOCALLOCK;

#define LOCALLOCK_LOCKMETHOD(llock) ((llock).tag.lock.lockmethodid)


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
extern bool LockReleaseAll(LOCKMETHODID lockmethodid, bool allxids);
extern void LockReleaseCurrentOwner(void);
extern void LockReassignCurrentOwner(void);
extern int LockCheckConflicts(LockMethod lockMethodTable,
				   LOCKMODE lockmode,
				   LOCK *lock, PROCLOCK *proclock, PGPROC *proc,
				   int *myHolding);
extern void GrantLock(LOCK *lock, PROCLOCK *proclock, LOCKMODE lockmode);
extern void GrantAwaitedLock(void);
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
