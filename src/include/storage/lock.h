/*-------------------------------------------------------------------------
 *
 * lock.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: lock.h,v 1.24 1999/03/06 21:17:43 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOCK_H_
#define LOCK_H_

#include <storage/shmem.h>
#include <storage/itemptr.h>
#include <storage/sinvaladt.h>
#include <utils/array.h>

extern SPINLOCK LockMgrLock;
typedef int MASK;

#define INIT_TABLE_SIZE			100
#define MAX_TABLE_SIZE			1000


/* ----------------------
 * The following defines are used to estimate how much shared
 * memory the lock manager is going to require.
 * See LockShmemSize() in lock.c.
 *
 * NLOCKS_PER_XACT - The number of unique locks acquired in a transaction
 * NLOCKENTS - The maximum number of lock entries in the lock table.
 * ----------------------
 */
#define NLOCKS_PER_XACT			40
#define NLOCKENTS(maxBackends)	(NLOCKS_PER_XACT*(maxBackends))

typedef int LOCKMODE;
typedef int LOCKMETHOD;

/* MAX_LOCKMODES cannot be larger than the bits in MASK */
#define MAX_LOCKMODES	9

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


typedef struct LTAG
{
	Oid			relId;
	Oid			dbId;
	union
	{
		BlockNumber		blkno;
		TransactionId	xid;
	}			objId;
	uint16		lockmethod;		/* needed by user locks */
} LOCKTAG;

#define TAGSIZE (sizeof(LOCKTAG))
#define LOCKTAG_LOCKMETHOD(locktag) ((locktag).lockmethod)

/* This is the control structure for a lock table.	It
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
 *
 */
typedef struct LOCKMETHODCTL
{
	LOCKMETHOD	lockmethod;
	int			numLockModes;
	int			conflictTab[MAX_LOCKMODES];
	int			prio[MAX_LOCKMODES];
	SPINLOCK	masterLock;
}			LOCKMETHODCTL;

/*
 * lockHash -- hash table on lock Ids,
 * xidHash -- hash on xid and lockId in case
 *		multiple processes are holding the lock
 * ctl - control structure described above.
 */
typedef struct LOCKMETHODTABLE
{
	HTAB	   *lockHash;
	HTAB	   *xidHash;
	LOCKMETHODCTL *ctl;
}			LOCKMETHODTABLE;

/* -----------------------
 * A transaction never conflicts with its own locks.  Hence, if
 * multiple transactions hold non-conflicting locks on the same
 * data, private per-transaction information must be stored in the
 * XID table.  The tag is XID + shared memory lock address so that
 * all locks can use the same XID table.  The private information
 * we store is the number of locks of each type (holders) and the
 * total number of locks (nHolding) held by the transaction.
 *
 * NOTE: 
 * There were some problems with the fact that currently TransactionIdData
 * is a 5 byte entity and compilers long word aligning of structure fields.
 * If the 3 byte padding is put in front of the actual xid data then the
 * hash function (which uses XID_TAGSIZE when deciding how many bytes of a
 * struct to look at for the key) might only see the last two bytes of the xid.
 *
 * Clearly this is not good since its likely that these bytes will be the
 * same for many transactions and hence they will share the same entry in
 * hash table causing the entry to be corrupted.  For this long-winded
 * reason I have put the tag in a struct of its own to ensure that the
 * XID_TAGSIZE is computed correctly.  It used to be sizeof (SHMEM_OFFSET) +
 * sizeof(TransactionIdData) which != sizeof(XIDTAG).
 *
 * Finally since the hash function will now look at all 12 bytes of the tag
 * the padding bytes MUST be zero'd before use in hash_search() as they
 * will have random values otherwise.  Jeff 22 July 1991.
 * -----------------------
 */

typedef struct XIDTAG
{
	SHMEM_OFFSET lock;
	int			pid;
	TransactionId xid;
#ifdef USE_XIDTAG_LOCKMETHOD
	uint16		lockmethod;		/* for debug or consistency checking */
#endif
} XIDTAG;

#ifdef USE_XIDTAG_LOCKMETHOD
#define XIDTAG_LOCKMETHOD(xidtag) ((xidtag).lockmethod)
#else
#define XIDTAG_LOCKMETHOD(xidtag) \
		(((LOCK*) MAKE_PTR((xidtag).lock))->tag.lockmethod)
#endif

typedef struct XIDLookupEnt
{
	/* tag */
	XIDTAG		tag;

	/* data */
	int			holders[MAX_LOCKMODES];
	int			nHolding;
	SHM_QUEUE	queue;
} XIDLookupEnt;

#define SHMEM_XIDTAB_KEYSIZE  sizeof(XIDTAG)
#define SHMEM_XIDTAB_DATASIZE (sizeof(XIDLookupEnt) - SHMEM_XIDTAB_KEYSIZE)

#define XID_TAGSIZE (sizeof(XIDTAG))
#define XIDENT_LOCKMETHOD(xident) (XIDTAG_LOCKMETHOD((xident).tag))

/* originally in procq.h */
typedef struct PROC_QUEUE
{
	SHM_QUEUE	links;
	int			size;
} PROC_QUEUE;


/*
 * lock information:
 *
 * tag -- uniquely identifies the object being locked
 * mask -- union of the conflict masks of all lock types
 *		currently held on this object.
 * waitProcs -- queue of processes waiting for this lock
 * holders -- count of each lock type currently held on the
 *		lock.
 * nHolding -- total locks of all types.
 */
typedef struct LOCK
{
	/* hash key */
	LOCKTAG		tag;

	/* data */
	int			mask;
	PROC_QUEUE	waitProcs;
	int			holders[MAX_LOCKMODES];
	int			nHolding;
	int			activeHolders[MAX_LOCKMODES];
	int			nActive;
} LOCK;

#define SHMEM_LOCKTAB_KEYSIZE  sizeof(LOCKTAG)
#define SHMEM_LOCKTAB_DATASIZE (sizeof(LOCK) - SHMEM_LOCKTAB_KEYSIZE)

#define LOCK_LOCKMETHOD(lock) (LOCKTAG_LOCKMETHOD((lock).tag))

#define LockGetLock_nHolders(l) l->nHolders
#ifdef NOT_USED
#define LockDecrWaitHolders(lock, lockmode) \
( \
  lock->nHolding--, \
  lock->holders[lockmode]-- \
)
#endif
#define LockLockTable() SpinAcquire(LockMgrLock);
#define UnlockLockTable() SpinRelease(LockMgrLock);

extern SPINLOCK LockMgrLock;

/*
 * function prototypes
 */
extern void InitLocks(void);
extern void LockDisable(int status);
extern LOCKMETHOD LockMethodTableInit(char *tabName, MASK *conflictsP,
					int *prioP, int numModes);
extern LOCKMETHOD LockMethodTableRename(LOCKMETHOD lockmethod);
extern bool LockAcquire(LOCKMETHOD lockmethod, LOCKTAG *locktag,
			LOCKMODE lockmode);
extern int LockResolveConflicts(LOCKMETHOD lockmethod, LOCK *lock,
					 LOCKMODE lockmode, TransactionId xid,
					 XIDLookupEnt *xidentP);
extern bool LockRelease(LOCKMETHOD lockmethod, LOCKTAG *locktag,
			LOCKMODE lockmode);
extern void GrantLock(LOCK *lock, LOCKMODE lockmode);
extern bool LockReleaseAll(LOCKMETHOD lockmethod, SHM_QUEUE *lockQueue);
extern int	LockShmemSize(int maxBackends);
extern bool LockingDisabled(void);
extern bool DeadLockCheck(SHM_QUEUE *lockQueue, LOCK *findlock,
			  bool skip_check);

#ifdef DEADLOCK_DEBUG
extern void DumpLocks(void);
extern void DumpAllLocks(void);

#endif

#endif	 /* LOCK_H */
