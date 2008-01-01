/*-------------------------------------------------------------------------
 *
 * lock.c
 *	  POSTGRES primary lock mechanism
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/lmgr/lock.c,v 1.180 2008/01/01 19:45:52 momjian Exp $
 *
 * NOTES
 *	  A lock table is a shared memory hash table.  When
 *	  a process tries to acquire a lock of a type that conflicts
 *	  with existing locks, it is put to sleep using the routines
 *	  in storage/lmgr/proc.c.
 *
 *	  For the most part, this code should be invoked via lmgr.c
 *	  or another lock-management module, not directly.
 *
 *	Interface:
 *
 *	InitLocks(), GetLocksMethodTable(),
 *	LockAcquire(), LockRelease(), LockReleaseAll(),
 *	LockCheckConflicts(), GrantLock()
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>

#include "access/transam.h"
#include "access/twophase.h"
#include "access/twophase_rmgr.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/resowner.h"


/* This configuration variable is used to set the lock table size */
int			max_locks_per_xact; /* set by guc.c */

#define NLOCKENTS() \
	mul_size(max_locks_per_xact, add_size(MaxBackends, max_prepared_xacts))


/*
 * Data structures defining the semantics of the standard lock methods.
 *
 * The conflict table defines the semantics of the various lock modes.
 */
static const LOCKMASK LockConflicts[] = {
	0,

	/* AccessShareLock */
	(1 << AccessExclusiveLock),

	/* RowShareLock */
	(1 << ExclusiveLock) | (1 << AccessExclusiveLock),

	/* RowExclusiveLock */
	(1 << ShareLock) | (1 << ShareRowExclusiveLock) |
	(1 << ExclusiveLock) | (1 << AccessExclusiveLock),

	/* ShareUpdateExclusiveLock */
	(1 << ShareUpdateExclusiveLock) |
	(1 << ShareLock) | (1 << ShareRowExclusiveLock) |
	(1 << ExclusiveLock) | (1 << AccessExclusiveLock),

	/* ShareLock */
	(1 << RowExclusiveLock) | (1 << ShareUpdateExclusiveLock) |
	(1 << ShareRowExclusiveLock) |
	(1 << ExclusiveLock) | (1 << AccessExclusiveLock),

	/* ShareRowExclusiveLock */
	(1 << RowExclusiveLock) | (1 << ShareUpdateExclusiveLock) |
	(1 << ShareLock) | (1 << ShareRowExclusiveLock) |
	(1 << ExclusiveLock) | (1 << AccessExclusiveLock),

	/* ExclusiveLock */
	(1 << RowShareLock) |
	(1 << RowExclusiveLock) | (1 << ShareUpdateExclusiveLock) |
	(1 << ShareLock) | (1 << ShareRowExclusiveLock) |
	(1 << ExclusiveLock) | (1 << AccessExclusiveLock),

	/* AccessExclusiveLock */
	(1 << AccessShareLock) | (1 << RowShareLock) |
	(1 << RowExclusiveLock) | (1 << ShareUpdateExclusiveLock) |
	(1 << ShareLock) | (1 << ShareRowExclusiveLock) |
	(1 << ExclusiveLock) | (1 << AccessExclusiveLock)

};

/* Names of lock modes, for debug printouts */
static const char *const lock_mode_names[] =
{
	"INVALID",
	"AccessShareLock",
	"RowShareLock",
	"RowExclusiveLock",
	"ShareUpdateExclusiveLock",
	"ShareLock",
	"ShareRowExclusiveLock",
	"ExclusiveLock",
	"AccessExclusiveLock"
};

#ifndef LOCK_DEBUG
static bool Dummy_trace = false;
#endif

static const LockMethodData default_lockmethod = {
	AccessExclusiveLock,		/* highest valid lock mode number */
	true,
	LockConflicts,
	lock_mode_names,
#ifdef LOCK_DEBUG
	&Trace_locks
#else
	&Dummy_trace
#endif
};

static const LockMethodData user_lockmethod = {
	AccessExclusiveLock,		/* highest valid lock mode number */
	false,
	LockConflicts,
	lock_mode_names,
#ifdef LOCK_DEBUG
	&Trace_userlocks
#else
	&Dummy_trace
#endif
};

/*
 * map from lock method id to the lock table data structures
 */
static const LockMethod LockMethods[] = {
	NULL,
	&default_lockmethod,
	&user_lockmethod
};


/* Record that's written to 2PC state file when a lock is persisted */
typedef struct TwoPhaseLockRecord
{
	LOCKTAG		locktag;
	LOCKMODE	lockmode;
} TwoPhaseLockRecord;


/*
 * Pointers to hash tables containing lock state
 *
 * The LockMethodLockHash and LockMethodProcLockHash hash tables are in
 * shared memory; LockMethodLocalHash is local to each backend.
 */
static HTAB *LockMethodLockHash;
static HTAB *LockMethodProcLockHash;
static HTAB *LockMethodLocalHash;


/* private state for GrantAwaitedLock */
static LOCALLOCK *awaitedLock;
static ResourceOwner awaitedOwner;


#ifdef LOCK_DEBUG

/*------
 * The following configuration options are available for lock debugging:
 *
 *	   TRACE_LOCKS		-- give a bunch of output what's going on in this file
 *	   TRACE_USERLOCKS	-- same but for user locks
 *	   TRACE_LOCK_OIDMIN-- do not trace locks for tables below this oid
 *						   (use to avoid output on system tables)
 *	   TRACE_LOCK_TABLE -- trace locks on this table (oid) unconditionally
 *	   DEBUG_DEADLOCKS	-- currently dumps locks at untimely occasions ;)
 *
 * Furthermore, but in storage/lmgr/lwlock.c:
 *	   TRACE_LWLOCKS	-- trace lightweight locks (pretty useless)
 *
 * Define LOCK_DEBUG at compile time to get all these enabled.
 * --------
 */

int			Trace_lock_oidmin = FirstNormalObjectId;
bool		Trace_locks = false;
bool		Trace_userlocks = false;
int			Trace_lock_table = 0;
bool		Debug_deadlocks = false;


inline static bool
LOCK_DEBUG_ENABLED(const LOCKTAG *tag)
{
	return
		(*(LockMethods[tag->locktag_lockmethodid]->trace_flag) &&
		 ((Oid) tag->locktag_field2 >= (Oid) Trace_lock_oidmin))
		|| (Trace_lock_table &&
			(tag->locktag_field2 == Trace_lock_table));
}


inline static void
LOCK_PRINT(const char *where, const LOCK *lock, LOCKMODE type)
{
	if (LOCK_DEBUG_ENABLED(&lock->tag))
		elog(LOG,
			 "%s: lock(%p) id(%u,%u,%u,%u,%u,%u) grantMask(%x) "
			 "req(%d,%d,%d,%d,%d,%d,%d)=%d "
			 "grant(%d,%d,%d,%d,%d,%d,%d)=%d wait(%d) type(%s)",
			 where, lock,
			 lock->tag.locktag_field1, lock->tag.locktag_field2,
			 lock->tag.locktag_field3, lock->tag.locktag_field4,
			 lock->tag.locktag_type, lock->tag.locktag_lockmethodid,
			 lock->grantMask,
			 lock->requested[1], lock->requested[2], lock->requested[3],
			 lock->requested[4], lock->requested[5], lock->requested[6],
			 lock->requested[7], lock->nRequested,
			 lock->granted[1], lock->granted[2], lock->granted[3],
			 lock->granted[4], lock->granted[5], lock->granted[6],
			 lock->granted[7], lock->nGranted,
			 lock->waitProcs.size,
			 LockMethods[LOCK_LOCKMETHOD(*lock)]->lockModeNames[type]);
}


inline static void
PROCLOCK_PRINT(const char *where, const PROCLOCK *proclockP)
{
	if (LOCK_DEBUG_ENABLED(&proclockP->tag.myLock->tag))
		elog(LOG,
			 "%s: proclock(%p) lock(%p) method(%u) proc(%p) hold(%x)",
			 where, proclockP, proclockP->tag.myLock,
			 PROCLOCK_LOCKMETHOD(*(proclockP)),
			 proclockP->tag.myProc, (int) proclockP->holdMask);
}
#else							/* not LOCK_DEBUG */

#define LOCK_PRINT(where, lock, type)
#define PROCLOCK_PRINT(where, proclockP)
#endif   /* not LOCK_DEBUG */


static uint32 proclock_hash(const void *key, Size keysize);
static void RemoveLocalLock(LOCALLOCK *locallock);
static void GrantLockLocal(LOCALLOCK *locallock, ResourceOwner owner);
static void WaitOnLock(LOCALLOCK *locallock, ResourceOwner owner);
static bool UnGrantLock(LOCK *lock, LOCKMODE lockmode,
			PROCLOCK *proclock, LockMethod lockMethodTable);
static void CleanUpLock(LOCK *lock, PROCLOCK *proclock,
			LockMethod lockMethodTable, uint32 hashcode,
			bool wakeupNeeded);


/*
 * InitLocks -- Initialize the lock manager's data structures.
 *
 * This is called from CreateSharedMemoryAndSemaphores(), which see for
 * more comments.  In the normal postmaster case, the shared hash tables
 * are created here, as well as a locallock hash table that will remain
 * unused and empty in the postmaster itself.  Backends inherit the pointers
 * to the shared tables via fork(), and also inherit an image of the locallock
 * hash table, which they proceed to use.  In the EXEC_BACKEND case, each
 * backend re-executes this code to obtain pointers to the already existing
 * shared hash tables and to create its locallock hash table.
 */
void
InitLocks(void)
{
	HASHCTL		info;
	int			hash_flags;
	long		init_table_size,
				max_table_size;

	/*
	 * Compute init/max size to request for lock hashtables.  Note these
	 * calculations must agree with LockShmemSize!
	 */
	max_table_size = NLOCKENTS();
	init_table_size = max_table_size / 2;

	/*
	 * Allocate hash table for LOCK structs.  This stores per-locked-object
	 * information.
	 */
	MemSet(&info, 0, sizeof(info));
	info.keysize = sizeof(LOCKTAG);
	info.entrysize = sizeof(LOCK);
	info.hash = tag_hash;
	info.num_partitions = NUM_LOCK_PARTITIONS;
	hash_flags = (HASH_ELEM | HASH_FUNCTION | HASH_PARTITION);

	LockMethodLockHash = ShmemInitHash("LOCK hash",
									   init_table_size,
									   max_table_size,
									   &info,
									   hash_flags);
	if (!LockMethodLockHash)
		elog(FATAL, "could not initialize lock hash table");

	/* Assume an average of 2 holders per lock */
	max_table_size *= 2;
	init_table_size *= 2;

	/*
	 * Allocate hash table for PROCLOCK structs.  This stores
	 * per-lock-per-holder information.
	 */
	info.keysize = sizeof(PROCLOCKTAG);
	info.entrysize = sizeof(PROCLOCK);
	info.hash = proclock_hash;
	info.num_partitions = NUM_LOCK_PARTITIONS;
	hash_flags = (HASH_ELEM | HASH_FUNCTION | HASH_PARTITION);

	LockMethodProcLockHash = ShmemInitHash("PROCLOCK hash",
										   init_table_size,
										   max_table_size,
										   &info,
										   hash_flags);
	if (!LockMethodProcLockHash)
		elog(FATAL, "could not initialize proclock hash table");

	/*
	 * Allocate non-shared hash table for LOCALLOCK structs.  This stores lock
	 * counts and resource owner information.
	 *
	 * The non-shared table could already exist in this process (this occurs
	 * when the postmaster is recreating shared memory after a backend crash).
	 * If so, delete and recreate it.  (We could simply leave it, since it
	 * ought to be empty in the postmaster, but for safety let's zap it.)
	 */
	if (LockMethodLocalHash)
		hash_destroy(LockMethodLocalHash);

	info.keysize = sizeof(LOCALLOCKTAG);
	info.entrysize = sizeof(LOCALLOCK);
	info.hash = tag_hash;
	hash_flags = (HASH_ELEM | HASH_FUNCTION);

	LockMethodLocalHash = hash_create("LOCALLOCK hash",
									  128,
									  &info,
									  hash_flags);
}


/*
 * Fetch the lock method table associated with a given lock
 */
LockMethod
GetLocksMethodTable(const LOCK *lock)
{
	LOCKMETHODID lockmethodid = LOCK_LOCKMETHOD(*lock);

	Assert(0 < lockmethodid && lockmethodid < lengthof(LockMethods));
	return LockMethods[lockmethodid];
}


/*
 * Compute the hash code associated with a LOCKTAG.
 *
 * To avoid unnecessary recomputations of the hash code, we try to do this
 * just once per function, and then pass it around as needed.  Aside from
 * passing the hashcode to hash_search_with_hash_value(), we can extract
 * the lock partition number from the hashcode.
 */
uint32
LockTagHashCode(const LOCKTAG *locktag)
{
	return get_hash_value(LockMethodLockHash, (const void *) locktag);
}

/*
 * Compute the hash code associated with a PROCLOCKTAG.
 *
 * Because we want to use just one set of partition locks for both the
 * LOCK and PROCLOCK hash tables, we have to make sure that PROCLOCKs
 * fall into the same partition number as their associated LOCKs.
 * dynahash.c expects the partition number to be the low-order bits of
 * the hash code, and therefore a PROCLOCKTAG's hash code must have the
 * same low-order bits as the associated LOCKTAG's hash code.  We achieve
 * this with this specialized hash function.
 */
static uint32
proclock_hash(const void *key, Size keysize)
{
	const PROCLOCKTAG *proclocktag = (const PROCLOCKTAG *) key;
	uint32		lockhash;
	Datum		procptr;

	Assert(keysize == sizeof(PROCLOCKTAG));

	/* Look into the associated LOCK object, and compute its hash code */
	lockhash = LockTagHashCode(&proclocktag->myLock->tag);

	/*
	 * To make the hash code also depend on the PGPROC, we xor the proc
	 * struct's address into the hash code, left-shifted so that the
	 * partition-number bits don't change.  Since this is only a hash, we
	 * don't care if we lose high-order bits of the address; use an
	 * intermediate variable to suppress cast-pointer-to-int warnings.
	 */
	procptr = PointerGetDatum(proclocktag->myProc);
	lockhash ^= ((uint32) procptr) << LOG2_NUM_LOCK_PARTITIONS;

	return lockhash;
}

/*
 * Compute the hash code associated with a PROCLOCKTAG, given the hashcode
 * for its underlying LOCK.
 *
 * We use this just to avoid redundant calls of LockTagHashCode().
 */
static inline uint32
ProcLockHashCode(const PROCLOCKTAG *proclocktag, uint32 hashcode)
{
	uint32		lockhash = hashcode;
	Datum		procptr;

	/*
	 * This must match proclock_hash()!
	 */
	procptr = PointerGetDatum(proclocktag->myProc);
	lockhash ^= ((uint32) procptr) << LOG2_NUM_LOCK_PARTITIONS;

	return lockhash;
}


/*
 * LockAcquire -- Check for lock conflicts, sleep if conflict found,
 *		set lock if/when no conflicts.
 *
 * Inputs:
 *	locktag: unique identifier for the lockable object
 *	lockmode: lock mode to acquire
 *	sessionLock: if true, acquire lock for session not current transaction
 *	dontWait: if true, don't wait to acquire lock
 *
 * Returns one of:
 *		LOCKACQUIRE_NOT_AVAIL		lock not available, and dontWait=true
 *		LOCKACQUIRE_OK				lock successfully acquired
 *		LOCKACQUIRE_ALREADY_HELD	incremented count for lock already held
 *
 * In the normal case where dontWait=false and the caller doesn't need to
 * distinguish a freshly acquired lock from one already taken earlier in
 * this same transaction, there is no need to examine the return value.
 *
 * Side Effects: The lock is acquired and recorded in lock tables.
 *
 * NOTE: if we wait for the lock, there is no way to abort the wait
 * short of aborting the transaction.
 */
LockAcquireResult
LockAcquire(const LOCKTAG *locktag,
			LOCKMODE lockmode,
			bool sessionLock,
			bool dontWait)
{
	LOCKMETHODID lockmethodid = locktag->locktag_lockmethodid;
	LockMethod	lockMethodTable;
	LOCALLOCKTAG localtag;
	LOCALLOCK  *locallock;
	LOCK	   *lock;
	PROCLOCK   *proclock;
	PROCLOCKTAG proclocktag;
	bool		found;
	ResourceOwner owner;
	uint32		hashcode;
	uint32		proclock_hashcode;
	int			partition;
	LWLockId	partitionLock;
	int			status;

	if (lockmethodid <= 0 || lockmethodid >= lengthof(LockMethods))
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);
	lockMethodTable = LockMethods[lockmethodid];
	if (lockmode <= 0 || lockmode > lockMethodTable->numLockModes)
		elog(ERROR, "unrecognized lock mode: %d", lockmode);

#ifdef LOCK_DEBUG
	if (LOCK_DEBUG_ENABLED(locktag))
		elog(LOG, "LockAcquire: lock [%u,%u] %s",
			 locktag->locktag_field1, locktag->locktag_field2,
			 lockMethodTable->lockModeNames[lockmode]);
#endif

	/* Session locks are never transactional, else check table */
	if (!sessionLock && lockMethodTable->transactional)
		owner = CurrentResourceOwner;
	else
		owner = NULL;

	/*
	 * Find or create a LOCALLOCK entry for this lock and lockmode
	 */
	MemSet(&localtag, 0, sizeof(localtag));		/* must clear padding */
	localtag.lock = *locktag;
	localtag.mode = lockmode;

	locallock = (LOCALLOCK *) hash_search(LockMethodLocalHash,
										  (void *) &localtag,
										  HASH_ENTER, &found);

	/*
	 * if it's a new locallock object, initialize it
	 */
	if (!found)
	{
		locallock->lock = NULL;
		locallock->proclock = NULL;
		locallock->hashcode = LockTagHashCode(&(localtag.lock));
		locallock->nLocks = 0;
		locallock->numLockOwners = 0;
		locallock->maxLockOwners = 8;
		locallock->lockOwners = NULL;
		locallock->lockOwners = (LOCALLOCKOWNER *)
			MemoryContextAlloc(TopMemoryContext,
						  locallock->maxLockOwners * sizeof(LOCALLOCKOWNER));
	}
	else
	{
		/* Make sure there will be room to remember the lock */
		if (locallock->numLockOwners >= locallock->maxLockOwners)
		{
			int			newsize = locallock->maxLockOwners * 2;

			locallock->lockOwners = (LOCALLOCKOWNER *)
				repalloc(locallock->lockOwners,
						 newsize * sizeof(LOCALLOCKOWNER));
			locallock->maxLockOwners = newsize;
		}
	}

	/*
	 * If we already hold the lock, we can just increase the count locally.
	 */
	if (locallock->nLocks > 0)
	{
		GrantLockLocal(locallock, owner);
		return LOCKACQUIRE_ALREADY_HELD;
	}

	/*
	 * Otherwise we've got to mess with the shared lock table.
	 */
	hashcode = locallock->hashcode;
	partition = LockHashPartition(hashcode);
	partitionLock = LockHashPartitionLock(hashcode);

	LWLockAcquire(partitionLock, LW_EXCLUSIVE);

	/*
	 * Find or create a lock with this tag.
	 *
	 * Note: if the locallock object already existed, it might have a pointer
	 * to the lock already ... but we probably should not assume that that
	 * pointer is valid, since a lock object with no locks can go away
	 * anytime.
	 */
	lock = (LOCK *) hash_search_with_hash_value(LockMethodLockHash,
												(void *) locktag,
												hashcode,
												HASH_ENTER_NULL,
												&found);
	if (!lock)
	{
		LWLockRelease(partitionLock);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory"),
		  errhint("You might need to increase max_locks_per_transaction.")));
	}
	locallock->lock = lock;

	/*
	 * if it's a new lock object, initialize it
	 */
	if (!found)
	{
		lock->grantMask = 0;
		lock->waitMask = 0;
		SHMQueueInit(&(lock->procLocks));
		ProcQueueInit(&(lock->waitProcs));
		lock->nRequested = 0;
		lock->nGranted = 0;
		MemSet(lock->requested, 0, sizeof(int) * MAX_LOCKMODES);
		MemSet(lock->granted, 0, sizeof(int) * MAX_LOCKMODES);
		LOCK_PRINT("LockAcquire: new", lock, lockmode);
	}
	else
	{
		LOCK_PRINT("LockAcquire: found", lock, lockmode);
		Assert((lock->nRequested >= 0) && (lock->requested[lockmode] >= 0));
		Assert((lock->nGranted >= 0) && (lock->granted[lockmode] >= 0));
		Assert(lock->nGranted <= lock->nRequested);
	}

	/*
	 * Create the hash key for the proclock table.
	 */
	proclocktag.myLock = lock;
	proclocktag.myProc = MyProc;

	proclock_hashcode = ProcLockHashCode(&proclocktag, hashcode);

	/*
	 * Find or create a proclock entry with this tag
	 */
	proclock = (PROCLOCK *) hash_search_with_hash_value(LockMethodProcLockHash,
														(void *) &proclocktag,
														proclock_hashcode,
														HASH_ENTER_NULL,
														&found);
	if (!proclock)
	{
		/* Ooops, not enough shmem for the proclock */
		if (lock->nRequested == 0)
		{
			/*
			 * There are no other requestors of this lock, so garbage-collect
			 * the lock object.  We *must* do this to avoid a permanent leak
			 * of shared memory, because there won't be anything to cause
			 * anyone to release the lock object later.
			 */
			Assert(SHMQueueEmpty(&(lock->procLocks)));
			if (!hash_search_with_hash_value(LockMethodLockHash,
											 (void *) &(lock->tag),
											 hashcode,
											 HASH_REMOVE,
											 NULL))
				elog(PANIC, "lock table corrupted");
		}
		LWLockRelease(partitionLock);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory"),
		  errhint("You might need to increase max_locks_per_transaction.")));
	}
	locallock->proclock = proclock;

	/*
	 * If new, initialize the new entry
	 */
	if (!found)
	{
		proclock->holdMask = 0;
		proclock->releaseMask = 0;
		/* Add proclock to appropriate lists */
		SHMQueueInsertBefore(&lock->procLocks, &proclock->lockLink);
		SHMQueueInsertBefore(&(MyProc->myProcLocks[partition]),
							 &proclock->procLink);
		PROCLOCK_PRINT("LockAcquire: new", proclock);
	}
	else
	{
		PROCLOCK_PRINT("LockAcquire: found", proclock);
		Assert((proclock->holdMask & ~lock->grantMask) == 0);

#ifdef CHECK_DEADLOCK_RISK

		/*
		 * Issue warning if we already hold a lower-level lock on this object
		 * and do not hold a lock of the requested level or higher. This
		 * indicates a deadlock-prone coding practice (eg, we'd have a
		 * deadlock if another backend were following the same code path at
		 * about the same time).
		 *
		 * This is not enabled by default, because it may generate log entries
		 * about user-level coding practices that are in fact safe in context.
		 * It can be enabled to help find system-level problems.
		 *
		 * XXX Doing numeric comparison on the lockmodes is a hack; it'd be
		 * better to use a table.  For now, though, this works.
		 */
		{
			int			i;

			for (i = lockMethodTable->numLockModes; i > 0; i--)
			{
				if (proclock->holdMask & LOCKBIT_ON(i))
				{
					if (i >= (int) lockmode)
						break;	/* safe: we have a lock >= req level */
					elog(LOG, "deadlock risk: raising lock level"
						 " from %s to %s on object %u/%u/%u",
						 lockMethodTable->lockModeNames[i],
						 lockMethodTable->lockModeNames[lockmode],
						 lock->tag.locktag_field1, lock->tag.locktag_field2,
						 lock->tag.locktag_field3);
					break;
				}
			}
		}
#endif   /* CHECK_DEADLOCK_RISK */
	}

	/*
	 * lock->nRequested and lock->requested[] count the total number of
	 * requests, whether granted or waiting, so increment those immediately.
	 * The other counts don't increment till we get the lock.
	 */
	lock->nRequested++;
	lock->requested[lockmode]++;
	Assert((lock->nRequested > 0) && (lock->requested[lockmode] > 0));

	/*
	 * We shouldn't already hold the desired lock; else locallock table is
	 * broken.
	 */
	if (proclock->holdMask & LOCKBIT_ON(lockmode))
		elog(ERROR, "lock %s on object %u/%u/%u is already held",
			 lockMethodTable->lockModeNames[lockmode],
			 lock->tag.locktag_field1, lock->tag.locktag_field2,
			 lock->tag.locktag_field3);

	/*
	 * If lock requested conflicts with locks requested by waiters, must join
	 * wait queue.	Otherwise, check for conflict with already-held locks.
	 * (That's last because most complex check.)
	 */
	if (lockMethodTable->conflictTab[lockmode] & lock->waitMask)
		status = STATUS_FOUND;
	else
		status = LockCheckConflicts(lockMethodTable, lockmode,
									lock, proclock, MyProc);

	if (status == STATUS_OK)
	{
		/* No conflict with held or previously requested locks */
		GrantLock(lock, proclock, lockmode);
		GrantLockLocal(locallock, owner);
	}
	else
	{
		Assert(status == STATUS_FOUND);

		/*
		 * We can't acquire the lock immediately.  If caller specified no
		 * blocking, remove useless table entries and return NOT_AVAIL without
		 * waiting.
		 */
		if (dontWait)
		{
			if (proclock->holdMask == 0)
			{
				SHMQueueDelete(&proclock->lockLink);
				SHMQueueDelete(&proclock->procLink);
				if (!hash_search_with_hash_value(LockMethodProcLockHash,
												 (void *) &(proclock->tag),
												 proclock_hashcode,
												 HASH_REMOVE,
												 NULL))
					elog(PANIC, "proclock table corrupted");
			}
			else
				PROCLOCK_PRINT("LockAcquire: NOWAIT", proclock);
			lock->nRequested--;
			lock->requested[lockmode]--;
			LOCK_PRINT("LockAcquire: conditional lock failed", lock, lockmode);
			Assert((lock->nRequested > 0) && (lock->requested[lockmode] >= 0));
			Assert(lock->nGranted <= lock->nRequested);
			LWLockRelease(partitionLock);
			if (locallock->nLocks == 0)
				RemoveLocalLock(locallock);
			return LOCKACQUIRE_NOT_AVAIL;
		}

		/*
		 * Set bitmask of locks this process already holds on this object.
		 */
		MyProc->heldLocks = proclock->holdMask;

		/*
		 * Sleep till someone wakes me up.
		 */

		PG_TRACE2(lock__startwait, locktag->locktag_field2, lockmode);

		WaitOnLock(locallock, owner);

		PG_TRACE2(lock__endwait, locktag->locktag_field2, lockmode);

		/*
		 * NOTE: do not do any material change of state between here and
		 * return.	All required changes in locktable state must have been
		 * done when the lock was granted to us --- see notes in WaitOnLock.
		 */

		/*
		 * Check the proclock entry status, in case something in the ipc
		 * communication doesn't work correctly.
		 */
		if (!(proclock->holdMask & LOCKBIT_ON(lockmode)))
		{
			PROCLOCK_PRINT("LockAcquire: INCONSISTENT", proclock);
			LOCK_PRINT("LockAcquire: INCONSISTENT", lock, lockmode);
			/* Should we retry ? */
			LWLockRelease(partitionLock);
			elog(ERROR, "LockAcquire failed");
		}
		PROCLOCK_PRINT("LockAcquire: granted", proclock);
		LOCK_PRINT("LockAcquire: granted", lock, lockmode);
	}

	LWLockRelease(partitionLock);

	return LOCKACQUIRE_OK;
}

/*
 * Subroutine to free a locallock entry
 */
static void
RemoveLocalLock(LOCALLOCK *locallock)
{
	pfree(locallock->lockOwners);
	locallock->lockOwners = NULL;
	if (!hash_search(LockMethodLocalHash,
					 (void *) &(locallock->tag),
					 HASH_REMOVE, NULL))
		elog(WARNING, "locallock table corrupted");
}

/*
 * LockCheckConflicts -- test whether requested lock conflicts
 *		with those already granted
 *
 * Returns STATUS_FOUND if conflict, STATUS_OK if no conflict.
 *
 * NOTES:
 *		Here's what makes this complicated: one process's locks don't
 * conflict with one another, no matter what purpose they are held for
 * (eg, session and transaction locks do not conflict).
 * So, we must subtract off our own locks when determining whether the
 * requested new lock conflicts with those already held.
 */
int
LockCheckConflicts(LockMethod lockMethodTable,
				   LOCKMODE lockmode,
				   LOCK *lock,
				   PROCLOCK *proclock,
				   PGPROC *proc)
{
	int			numLockModes = lockMethodTable->numLockModes;
	LOCKMASK	myLocks;
	LOCKMASK	otherLocks;
	int			i;

	/*
	 * first check for global conflicts: If no locks conflict with my request,
	 * then I get the lock.
	 *
	 * Checking for conflict: lock->grantMask represents the types of
	 * currently held locks.  conflictTable[lockmode] has a bit set for each
	 * type of lock that conflicts with request.   Bitwise compare tells if
	 * there is a conflict.
	 */
	if (!(lockMethodTable->conflictTab[lockmode] & lock->grantMask))
	{
		PROCLOCK_PRINT("LockCheckConflicts: no conflict", proclock);
		return STATUS_OK;
	}

	/*
	 * Rats.  Something conflicts.	But it could still be my own lock. We have
	 * to construct a conflict mask that does not reflect our own locks, but
	 * only lock types held by other processes.
	 */
	myLocks = proclock->holdMask;
	otherLocks = 0;
	for (i = 1; i <= numLockModes; i++)
	{
		int			myHolding = (myLocks & LOCKBIT_ON(i)) ? 1 : 0;

		if (lock->granted[i] > myHolding)
			otherLocks |= LOCKBIT_ON(i);
	}

	/*
	 * now check again for conflicts.  'otherLocks' describes the types of
	 * locks held by other processes.  If one of these conflicts with the kind
	 * of lock that I want, there is a conflict and I have to sleep.
	 */
	if (!(lockMethodTable->conflictTab[lockmode] & otherLocks))
	{
		/* no conflict. OK to get the lock */
		PROCLOCK_PRINT("LockCheckConflicts: resolved", proclock);
		return STATUS_OK;
	}

	PROCLOCK_PRINT("LockCheckConflicts: conflicting", proclock);
	return STATUS_FOUND;
}

/*
 * GrantLock -- update the lock and proclock data structures to show
 *		the lock request has been granted.
 *
 * NOTE: if proc was blocked, it also needs to be removed from the wait list
 * and have its waitLock/waitProcLock fields cleared.  That's not done here.
 *
 * NOTE: the lock grant also has to be recorded in the associated LOCALLOCK
 * table entry; but since we may be awaking some other process, we can't do
 * that here; it's done by GrantLockLocal, instead.
 */
void
GrantLock(LOCK *lock, PROCLOCK *proclock, LOCKMODE lockmode)
{
	lock->nGranted++;
	lock->granted[lockmode]++;
	lock->grantMask |= LOCKBIT_ON(lockmode);
	if (lock->granted[lockmode] == lock->requested[lockmode])
		lock->waitMask &= LOCKBIT_OFF(lockmode);
	proclock->holdMask |= LOCKBIT_ON(lockmode);
	LOCK_PRINT("GrantLock", lock, lockmode);
	Assert((lock->nGranted > 0) && (lock->granted[lockmode] > 0));
	Assert(lock->nGranted <= lock->nRequested);
}

/*
 * UnGrantLock -- opposite of GrantLock.
 *
 * Updates the lock and proclock data structures to show that the lock
 * is no longer held nor requested by the current holder.
 *
 * Returns true if there were any waiters waiting on the lock that
 * should now be woken up with ProcLockWakeup.
 */
static bool
UnGrantLock(LOCK *lock, LOCKMODE lockmode,
			PROCLOCK *proclock, LockMethod lockMethodTable)
{
	bool		wakeupNeeded = false;

	Assert((lock->nRequested > 0) && (lock->requested[lockmode] > 0));
	Assert((lock->nGranted > 0) && (lock->granted[lockmode] > 0));
	Assert(lock->nGranted <= lock->nRequested);

	/*
	 * fix the general lock stats
	 */
	lock->nRequested--;
	lock->requested[lockmode]--;
	lock->nGranted--;
	lock->granted[lockmode]--;

	if (lock->granted[lockmode] == 0)
	{
		/* change the conflict mask.  No more of this lock type. */
		lock->grantMask &= LOCKBIT_OFF(lockmode);
	}

	LOCK_PRINT("UnGrantLock: updated", lock, lockmode);

	/*
	 * We need only run ProcLockWakeup if the released lock conflicts with at
	 * least one of the lock types requested by waiter(s).	Otherwise whatever
	 * conflict made them wait must still exist.  NOTE: before MVCC, we could
	 * skip wakeup if lock->granted[lockmode] was still positive. But that's
	 * not true anymore, because the remaining granted locks might belong to
	 * some waiter, who could now be awakened because he doesn't conflict with
	 * his own locks.
	 */
	if (lockMethodTable->conflictTab[lockmode] & lock->waitMask)
		wakeupNeeded = true;

	/*
	 * Now fix the per-proclock state.
	 */
	proclock->holdMask &= LOCKBIT_OFF(lockmode);
	PROCLOCK_PRINT("UnGrantLock: updated", proclock);

	return wakeupNeeded;
}

/*
 * CleanUpLock -- clean up after releasing a lock.	We garbage-collect the
 * proclock and lock objects if possible, and call ProcLockWakeup if there
 * are remaining requests and the caller says it's OK.  (Normally, this
 * should be called after UnGrantLock, and wakeupNeeded is the result from
 * UnGrantLock.)
 *
 * The appropriate partition lock must be held at entry, and will be
 * held at exit.
 */
static void
CleanUpLock(LOCK *lock, PROCLOCK *proclock,
			LockMethod lockMethodTable, uint32 hashcode,
			bool wakeupNeeded)
{
	/*
	 * If this was my last hold on this lock, delete my entry in the proclock
	 * table.
	 */
	if (proclock->holdMask == 0)
	{
		uint32		proclock_hashcode;

		PROCLOCK_PRINT("CleanUpLock: deleting", proclock);
		SHMQueueDelete(&proclock->lockLink);
		SHMQueueDelete(&proclock->procLink);
		proclock_hashcode = ProcLockHashCode(&proclock->tag, hashcode);
		if (!hash_search_with_hash_value(LockMethodProcLockHash,
										 (void *) &(proclock->tag),
										 proclock_hashcode,
										 HASH_REMOVE,
										 NULL))
			elog(PANIC, "proclock table corrupted");
	}

	if (lock->nRequested == 0)
	{
		/*
		 * The caller just released the last lock, so garbage-collect the lock
		 * object.
		 */
		LOCK_PRINT("CleanUpLock: deleting", lock, 0);
		Assert(SHMQueueEmpty(&(lock->procLocks)));
		if (!hash_search_with_hash_value(LockMethodLockHash,
										 (void *) &(lock->tag),
										 hashcode,
										 HASH_REMOVE,
										 NULL))
			elog(PANIC, "lock table corrupted");
	}
	else if (wakeupNeeded)
	{
		/* There are waiters on this lock, so wake them up. */
		ProcLockWakeup(lockMethodTable, lock);
	}
}

/*
 * GrantLockLocal -- update the locallock data structures to show
 *		the lock request has been granted.
 *
 * We expect that LockAcquire made sure there is room to add a new
 * ResourceOwner entry.
 */
static void
GrantLockLocal(LOCALLOCK *locallock, ResourceOwner owner)
{
	LOCALLOCKOWNER *lockOwners = locallock->lockOwners;
	int			i;

	Assert(locallock->numLockOwners < locallock->maxLockOwners);
	/* Count the total */
	locallock->nLocks++;
	/* Count the per-owner lock */
	for (i = 0; i < locallock->numLockOwners; i++)
	{
		if (lockOwners[i].owner == owner)
		{
			lockOwners[i].nLocks++;
			return;
		}
	}
	lockOwners[i].owner = owner;
	lockOwners[i].nLocks = 1;
	locallock->numLockOwners++;
}

/*
 * GrantAwaitedLock -- call GrantLockLocal for the lock we are doing
 *		WaitOnLock on.
 *
 * proc.c needs this for the case where we are booted off the lock by
 * timeout, but discover that someone granted us the lock anyway.
 *
 * We could just export GrantLockLocal, but that would require including
 * resowner.h in lock.h, which creates circularity.
 */
void
GrantAwaitedLock(void)
{
	GrantLockLocal(awaitedLock, awaitedOwner);
}

/*
 * WaitOnLock -- wait to acquire a lock
 *
 * Caller must have set MyProc->heldLocks to reflect locks already held
 * on the lockable object by this process.
 *
 * The appropriate partition lock must be held at entry.
 */
static void
WaitOnLock(LOCALLOCK *locallock, ResourceOwner owner)
{
	LOCKMETHODID lockmethodid = LOCALLOCK_LOCKMETHOD(*locallock);
	LockMethod	lockMethodTable = LockMethods[lockmethodid];
	const char *old_status;
	char	   *new_status = NULL;
	int			len;

	LOCK_PRINT("WaitOnLock: sleeping on lock",
			   locallock->lock, locallock->tag.mode);

	/* Report change to waiting status */
	if (update_process_title)
	{
		old_status = get_ps_display(&len);
		new_status = (char *) palloc(len + 8 + 1);
		memcpy(new_status, old_status, len);
		strcpy(new_status + len, " waiting");
		set_ps_display(new_status, false);
		new_status[len] = '\0'; /* truncate off " waiting" */
	}
	pgstat_report_waiting(true);

	awaitedLock = locallock;
	awaitedOwner = owner;

	/*
	 * NOTE: Think not to put any shared-state cleanup after the call to
	 * ProcSleep, in either the normal or failure path.  The lock state must
	 * be fully set by the lock grantor, or by CheckDeadLock if we give up
	 * waiting for the lock.  This is necessary because of the possibility
	 * that a cancel/die interrupt will interrupt ProcSleep after someone else
	 * grants us the lock, but before we've noticed it. Hence, after granting,
	 * the locktable state must fully reflect the fact that we own the lock;
	 * we can't do additional work on return. Contrariwise, if we fail, any
	 * cleanup must happen in xact abort processing, not here, to ensure it
	 * will also happen in the cancel/die case.
	 */

	if (ProcSleep(locallock, lockMethodTable) != STATUS_OK)
	{
		/*
		 * We failed as a result of a deadlock, see CheckDeadLock(). Quit now.
		 */
		awaitedLock = NULL;
		LOCK_PRINT("WaitOnLock: aborting on lock",
				   locallock->lock, locallock->tag.mode);
		LWLockRelease(LockHashPartitionLock(locallock->hashcode));

		/*
		 * Now that we aren't holding the partition lock, we can give an error
		 * report including details about the detected deadlock.
		 */
		DeadLockReport();
		/* not reached */
	}

	awaitedLock = NULL;

	/* Report change to non-waiting status */
	if (update_process_title)
	{
		set_ps_display(new_status, false);
		pfree(new_status);
	}
	pgstat_report_waiting(false);

	LOCK_PRINT("WaitOnLock: wakeup on lock",
			   locallock->lock, locallock->tag.mode);
}

/*
 * Remove a proc from the wait-queue it is on (caller must know it is on one).
 * This is only used when the proc has failed to get the lock, so we set its
 * waitStatus to STATUS_ERROR.
 *
 * Appropriate partition lock must be held by caller.  Also, caller is
 * responsible for signaling the proc if needed.
 *
 * NB: this does not clean up any locallock object that may exist for the lock.
 */
void
RemoveFromWaitQueue(PGPROC *proc, uint32 hashcode)
{
	LOCK	   *waitLock = proc->waitLock;
	PROCLOCK   *proclock = proc->waitProcLock;
	LOCKMODE	lockmode = proc->waitLockMode;
	LOCKMETHODID lockmethodid = LOCK_LOCKMETHOD(*waitLock);

	/* Make sure proc is waiting */
	Assert(proc->waitStatus == STATUS_WAITING);
	Assert(proc->links.next != INVALID_OFFSET);
	Assert(waitLock);
	Assert(waitLock->waitProcs.size > 0);
	Assert(0 < lockmethodid && lockmethodid < lengthof(LockMethods));

	/* Remove proc from lock's wait queue */
	SHMQueueDelete(&(proc->links));
	waitLock->waitProcs.size--;

	/* Undo increments of request counts by waiting process */
	Assert(waitLock->nRequested > 0);
	Assert(waitLock->nRequested > proc->waitLock->nGranted);
	waitLock->nRequested--;
	Assert(waitLock->requested[lockmode] > 0);
	waitLock->requested[lockmode]--;
	/* don't forget to clear waitMask bit if appropriate */
	if (waitLock->granted[lockmode] == waitLock->requested[lockmode])
		waitLock->waitMask &= LOCKBIT_OFF(lockmode);

	/* Clean up the proc's own state, and pass it the ok/fail signal */
	proc->waitLock = NULL;
	proc->waitProcLock = NULL;
	proc->waitStatus = STATUS_ERROR;

	/*
	 * Delete the proclock immediately if it represents no already-held locks.
	 * (This must happen now because if the owner of the lock decides to
	 * release it, and the requested/granted counts then go to zero,
	 * LockRelease expects there to be no remaining proclocks.) Then see if
	 * any other waiters for the lock can be woken up now.
	 */
	CleanUpLock(waitLock, proclock,
				LockMethods[lockmethodid], hashcode,
				true);
}

/*
 * LockRelease -- look up 'locktag' and release one 'lockmode' lock on it.
 *		Release a session lock if 'sessionLock' is true, else release a
 *		regular transaction lock.
 *
 * Side Effects: find any waiting processes that are now wakable,
 *		grant them their requested locks and awaken them.
 *		(We have to grant the lock here to avoid a race between
 *		the waking process and any new process to
 *		come along and request the lock.)
 */
bool
LockRelease(const LOCKTAG *locktag, LOCKMODE lockmode, bool sessionLock)
{
	LOCKMETHODID lockmethodid = locktag->locktag_lockmethodid;
	LockMethod	lockMethodTable;
	LOCALLOCKTAG localtag;
	LOCALLOCK  *locallock;
	LOCK	   *lock;
	PROCLOCK   *proclock;
	LWLockId	partitionLock;
	bool		wakeupNeeded;

	if (lockmethodid <= 0 || lockmethodid >= lengthof(LockMethods))
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);
	lockMethodTable = LockMethods[lockmethodid];
	if (lockmode <= 0 || lockmode > lockMethodTable->numLockModes)
		elog(ERROR, "unrecognized lock mode: %d", lockmode);

#ifdef LOCK_DEBUG
	if (LOCK_DEBUG_ENABLED(locktag))
		elog(LOG, "LockRelease: lock [%u,%u] %s",
			 locktag->locktag_field1, locktag->locktag_field2,
			 lockMethodTable->lockModeNames[lockmode]);
#endif

	/*
	 * Find the LOCALLOCK entry for this lock and lockmode
	 */
	MemSet(&localtag, 0, sizeof(localtag));		/* must clear padding */
	localtag.lock = *locktag;
	localtag.mode = lockmode;

	locallock = (LOCALLOCK *) hash_search(LockMethodLocalHash,
										  (void *) &localtag,
										  HASH_FIND, NULL);

	/*
	 * let the caller print its own error message, too. Do not ereport(ERROR).
	 */
	if (!locallock || locallock->nLocks <= 0)
	{
		elog(WARNING, "you don't own a lock of type %s",
			 lockMethodTable->lockModeNames[lockmode]);
		return FALSE;
	}

	/*
	 * Decrease the count for the resource owner.
	 */
	{
		LOCALLOCKOWNER *lockOwners = locallock->lockOwners;
		ResourceOwner owner;
		int			i;

		/* Session locks are never transactional, else check table */
		if (!sessionLock && lockMethodTable->transactional)
			owner = CurrentResourceOwner;
		else
			owner = NULL;

		for (i = locallock->numLockOwners - 1; i >= 0; i--)
		{
			if (lockOwners[i].owner == owner)
			{
				Assert(lockOwners[i].nLocks > 0);
				if (--lockOwners[i].nLocks == 0)
				{
					/* compact out unused slot */
					locallock->numLockOwners--;
					if (i < locallock->numLockOwners)
						lockOwners[i] = lockOwners[locallock->numLockOwners];
				}
				break;
			}
		}
		if (i < 0)
		{
			/* don't release a lock belonging to another owner */
			elog(WARNING, "you don't own a lock of type %s",
				 lockMethodTable->lockModeNames[lockmode]);
			return FALSE;
		}
	}

	/*
	 * Decrease the total local count.	If we're still holding the lock, we're
	 * done.
	 */
	locallock->nLocks--;

	if (locallock->nLocks > 0)
		return TRUE;

	/*
	 * Otherwise we've got to mess with the shared lock table.
	 */
	partitionLock = LockHashPartitionLock(locallock->hashcode);

	LWLockAcquire(partitionLock, LW_EXCLUSIVE);

	/*
	 * We don't need to re-find the lock or proclock, since we kept their
	 * addresses in the locallock table, and they couldn't have been removed
	 * while we were holding a lock on them.
	 */
	lock = locallock->lock;
	LOCK_PRINT("LockRelease: found", lock, lockmode);
	proclock = locallock->proclock;
	PROCLOCK_PRINT("LockRelease: found", proclock);

	/*
	 * Double-check that we are actually holding a lock of the type we want to
	 * release.
	 */
	if (!(proclock->holdMask & LOCKBIT_ON(lockmode)))
	{
		PROCLOCK_PRINT("LockRelease: WRONGTYPE", proclock);
		LWLockRelease(partitionLock);
		elog(WARNING, "you don't own a lock of type %s",
			 lockMethodTable->lockModeNames[lockmode]);
		RemoveLocalLock(locallock);
		return FALSE;
	}

	/*
	 * Do the releasing.  CleanUpLock will waken any now-wakable waiters.
	 */
	wakeupNeeded = UnGrantLock(lock, lockmode, proclock, lockMethodTable);

	CleanUpLock(lock, proclock,
				lockMethodTable, locallock->hashcode,
				wakeupNeeded);

	LWLockRelease(partitionLock);

	RemoveLocalLock(locallock);
	return TRUE;
}

/*
 * LockReleaseAll -- Release all locks of the specified lock method that
 *		are held by the current process.
 *
 * Well, not necessarily *all* locks.  The available behaviors are:
 *		allLocks == true: release all locks including session locks.
 *		allLocks == false: release all non-session locks.
 */
void
LockReleaseAll(LOCKMETHODID lockmethodid, bool allLocks)
{
	HASH_SEQ_STATUS status;
	LockMethod	lockMethodTable;
	int			i,
				numLockModes;
	LOCALLOCK  *locallock;
	LOCK	   *lock;
	PROCLOCK   *proclock;
	int			partition;

	if (lockmethodid <= 0 || lockmethodid >= lengthof(LockMethods))
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);
	lockMethodTable = LockMethods[lockmethodid];

#ifdef LOCK_DEBUG
	if (*(lockMethodTable->trace_flag))
		elog(LOG, "LockReleaseAll: lockmethod=%d", lockmethodid);
#endif

	numLockModes = lockMethodTable->numLockModes;

	/*
	 * First we run through the locallock table and get rid of unwanted
	 * entries, then we scan the process's proclocks and get rid of those. We
	 * do this separately because we may have multiple locallock entries
	 * pointing to the same proclock, and we daren't end up with any dangling
	 * pointers.
	 */
	hash_seq_init(&status, LockMethodLocalHash);

	while ((locallock = (LOCALLOCK *) hash_seq_search(&status)) != NULL)
	{
		if (locallock->proclock == NULL || locallock->lock == NULL)
		{
			/*
			 * We must've run out of shared memory while trying to set up this
			 * lock.  Just forget the local entry.
			 */
			Assert(locallock->nLocks == 0);
			RemoveLocalLock(locallock);
			continue;
		}

		/* Ignore items that are not of the lockmethod to be removed */
		if (LOCALLOCK_LOCKMETHOD(*locallock) != lockmethodid)
			continue;

		/*
		 * If we are asked to release all locks, we can just zap the entry.
		 * Otherwise, must scan to see if there are session locks. We assume
		 * there is at most one lockOwners entry for session locks.
		 */
		if (!allLocks)
		{
			LOCALLOCKOWNER *lockOwners = locallock->lockOwners;

			/* If it's above array position 0, move it down to 0 */
			for (i = locallock->numLockOwners - 1; i > 0; i--)
			{
				if (lockOwners[i].owner == NULL)
				{
					lockOwners[0] = lockOwners[i];
					break;
				}
			}

			if (locallock->numLockOwners > 0 &&
				lockOwners[0].owner == NULL &&
				lockOwners[0].nLocks > 0)
			{
				/* Fix the locallock to show just the session locks */
				locallock->nLocks = lockOwners[0].nLocks;
				locallock->numLockOwners = 1;
				/* We aren't deleting this locallock, so done */
				continue;
			}
		}

		/* Mark the proclock to show we need to release this lockmode */
		if (locallock->nLocks > 0)
			locallock->proclock->releaseMask |= LOCKBIT_ON(locallock->tag.mode);

		/* And remove the locallock hashtable entry */
		RemoveLocalLock(locallock);
	}

	/*
	 * Now, scan each lock partition separately.
	 */
	for (partition = 0; partition < NUM_LOCK_PARTITIONS; partition++)
	{
		LWLockId	partitionLock = FirstLockMgrLock + partition;
		SHM_QUEUE  *procLocks = &(MyProc->myProcLocks[partition]);

		proclock = (PROCLOCK *) SHMQueueNext(procLocks, procLocks,
											 offsetof(PROCLOCK, procLink));

		if (!proclock)
			continue;			/* needn't examine this partition */

		LWLockAcquire(partitionLock, LW_EXCLUSIVE);

		while (proclock)
		{
			bool		wakeupNeeded = false;
			PROCLOCK   *nextplock;

			/* Get link first, since we may unlink/delete this proclock */
			nextplock = (PROCLOCK *)
				SHMQueueNext(procLocks, &proclock->procLink,
							 offsetof(PROCLOCK, procLink));

			Assert(proclock->tag.myProc == MyProc);

			lock = proclock->tag.myLock;

			/* Ignore items that are not of the lockmethod to be removed */
			if (LOCK_LOCKMETHOD(*lock) != lockmethodid)
				goto next_item;

			/*
			 * In allLocks mode, force release of all locks even if locallock
			 * table had problems
			 */
			if (allLocks)
				proclock->releaseMask = proclock->holdMask;
			else
				Assert((proclock->releaseMask & ~proclock->holdMask) == 0);

			/*
			 * Ignore items that have nothing to be released, unless they have
			 * holdMask == 0 and are therefore recyclable
			 */
			if (proclock->releaseMask == 0 && proclock->holdMask != 0)
				goto next_item;

			PROCLOCK_PRINT("LockReleaseAll", proclock);
			LOCK_PRINT("LockReleaseAll", lock, 0);
			Assert(lock->nRequested >= 0);
			Assert(lock->nGranted >= 0);
			Assert(lock->nGranted <= lock->nRequested);
			Assert((proclock->holdMask & ~lock->grantMask) == 0);

			/*
			 * Release the previously-marked lock modes
			 */
			for (i = 1; i <= numLockModes; i++)
			{
				if (proclock->releaseMask & LOCKBIT_ON(i))
					wakeupNeeded |= UnGrantLock(lock, i, proclock,
												lockMethodTable);
			}
			Assert((lock->nRequested >= 0) && (lock->nGranted >= 0));
			Assert(lock->nGranted <= lock->nRequested);
			LOCK_PRINT("LockReleaseAll: updated", lock, 0);

			proclock->releaseMask = 0;

			/* CleanUpLock will wake up waiters if needed. */
			CleanUpLock(lock, proclock,
						lockMethodTable,
						LockTagHashCode(&lock->tag),
						wakeupNeeded);

	next_item:
			proclock = nextplock;
		}						/* loop over PROCLOCKs within this partition */

		LWLockRelease(partitionLock);
	}							/* loop over partitions */

#ifdef LOCK_DEBUG
	if (*(lockMethodTable->trace_flag))
		elog(LOG, "LockReleaseAll done");
#endif
}

/*
 * LockReleaseCurrentOwner
 *		Release all locks belonging to CurrentResourceOwner
 */
void
LockReleaseCurrentOwner(void)
{
	HASH_SEQ_STATUS status;
	LOCALLOCK  *locallock;
	LOCALLOCKOWNER *lockOwners;
	int			i;

	hash_seq_init(&status, LockMethodLocalHash);

	while ((locallock = (LOCALLOCK *) hash_seq_search(&status)) != NULL)
	{
		/* Ignore items that must be nontransactional */
		if (!LockMethods[LOCALLOCK_LOCKMETHOD(*locallock)]->transactional)
			continue;

		/* Scan to see if there are any locks belonging to current owner */
		lockOwners = locallock->lockOwners;
		for (i = locallock->numLockOwners - 1; i >= 0; i--)
		{
			if (lockOwners[i].owner == CurrentResourceOwner)
			{
				Assert(lockOwners[i].nLocks > 0);
				if (lockOwners[i].nLocks < locallock->nLocks)
				{
					/*
					 * We will still hold this lock after forgetting this
					 * ResourceOwner.
					 */
					locallock->nLocks -= lockOwners[i].nLocks;
					/* compact out unused slot */
					locallock->numLockOwners--;
					if (i < locallock->numLockOwners)
						lockOwners[i] = lockOwners[locallock->numLockOwners];
				}
				else
				{
					Assert(lockOwners[i].nLocks == locallock->nLocks);
					/* We want to call LockRelease just once */
					lockOwners[i].nLocks = 1;
					locallock->nLocks = 1;
					if (!LockRelease(&locallock->tag.lock,
									 locallock->tag.mode,
									 false))
						elog(WARNING, "LockReleaseCurrentOwner: failed??");
				}
				break;
			}
		}
	}
}

/*
 * LockReassignCurrentOwner
 *		Reassign all locks belonging to CurrentResourceOwner to belong
 *		to its parent resource owner
 */
void
LockReassignCurrentOwner(void)
{
	ResourceOwner parent = ResourceOwnerGetParent(CurrentResourceOwner);
	HASH_SEQ_STATUS status;
	LOCALLOCK  *locallock;
	LOCALLOCKOWNER *lockOwners;

	Assert(parent != NULL);

	hash_seq_init(&status, LockMethodLocalHash);

	while ((locallock = (LOCALLOCK *) hash_seq_search(&status)) != NULL)
	{
		int			i;
		int			ic = -1;
		int			ip = -1;

		/* Ignore items that must be nontransactional */
		if (!LockMethods[LOCALLOCK_LOCKMETHOD(*locallock)]->transactional)
			continue;

		/*
		 * Scan to see if there are any locks belonging to current owner or
		 * its parent
		 */
		lockOwners = locallock->lockOwners;
		for (i = locallock->numLockOwners - 1; i >= 0; i--)
		{
			if (lockOwners[i].owner == CurrentResourceOwner)
				ic = i;
			else if (lockOwners[i].owner == parent)
				ip = i;
		}

		if (ic < 0)
			continue;			/* no current locks */

		if (ip < 0)
		{
			/* Parent has no slot, so just give it child's slot */
			lockOwners[ic].owner = parent;
		}
		else
		{
			/* Merge child's count with parent's */
			lockOwners[ip].nLocks += lockOwners[ic].nLocks;
			/* compact out unused slot */
			locallock->numLockOwners--;
			if (ic < locallock->numLockOwners)
				lockOwners[ic] = lockOwners[locallock->numLockOwners];
		}
	}
}


/*
 * GetLockConflicts
 *		Get an array of VirtualTransactionIds of xacts currently holding locks
 *		that would conflict with the specified lock/lockmode.
 *		xacts merely awaiting such a lock are NOT reported.
 *
 * The result array is palloc'd and is terminated with an invalid VXID.
 *
 * Of course, the result could be out of date by the time it's returned,
 * so use of this function has to be thought about carefully.
 *
 * Note we never include the current xact's vxid in the result array,
 * since an xact never blocks itself.  Also, prepared transactions are
 * ignored, which is a bit more debatable but is appropriate for current
 * uses of the result.
 */
VirtualTransactionId *
GetLockConflicts(const LOCKTAG *locktag, LOCKMODE lockmode)
{
	VirtualTransactionId *vxids;
	LOCKMETHODID lockmethodid = locktag->locktag_lockmethodid;
	LockMethod	lockMethodTable;
	LOCK	   *lock;
	LOCKMASK	conflictMask;
	SHM_QUEUE  *procLocks;
	PROCLOCK   *proclock;
	uint32		hashcode;
	LWLockId	partitionLock;
	int			count = 0;

	if (lockmethodid <= 0 || lockmethodid >= lengthof(LockMethods))
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);
	lockMethodTable = LockMethods[lockmethodid];
	if (lockmode <= 0 || lockmode > lockMethodTable->numLockModes)
		elog(ERROR, "unrecognized lock mode: %d", lockmode);

	/*
	 * Allocate memory to store results, and fill with InvalidVXID.  We only
	 * need enough space for MaxBackends + a terminator, since prepared xacts
	 * don't count.
	 */
	vxids = (VirtualTransactionId *)
		palloc0(sizeof(VirtualTransactionId) * (MaxBackends + 1));

	/*
	 * Look up the lock object matching the tag.
	 */
	hashcode = LockTagHashCode(locktag);
	partitionLock = LockHashPartitionLock(hashcode);

	LWLockAcquire(partitionLock, LW_SHARED);

	lock = (LOCK *) hash_search_with_hash_value(LockMethodLockHash,
												(void *) locktag,
												hashcode,
												HASH_FIND,
												NULL);
	if (!lock)
	{
		/*
		 * If the lock object doesn't exist, there is nothing holding a lock
		 * on this lockable object.
		 */
		LWLockRelease(partitionLock);
		return vxids;
	}

	/*
	 * Examine each existing holder (or awaiter) of the lock.
	 */
	conflictMask = lockMethodTable->conflictTab[lockmode];

	procLocks = &(lock->procLocks);

	proclock = (PROCLOCK *) SHMQueueNext(procLocks, procLocks,
										 offsetof(PROCLOCK, lockLink));

	while (proclock)
	{
		if (conflictMask & proclock->holdMask)
		{
			PGPROC	   *proc = proclock->tag.myProc;

			/* A backend never blocks itself */
			if (proc != MyProc)
			{
				VirtualTransactionId vxid;

				GET_VXID_FROM_PGPROC(vxid, *proc);

				/*
				 * If we see an invalid VXID, then either the xact has already
				 * committed (or aborted), or it's a prepared xact.  In either
				 * case we may ignore it.
				 */
				if (VirtualTransactionIdIsValid(vxid))
					vxids[count++] = vxid;
			}
		}

		proclock = (PROCLOCK *) SHMQueueNext(procLocks, &proclock->lockLink,
											 offsetof(PROCLOCK, lockLink));
	}

	LWLockRelease(partitionLock);

	if (count > MaxBackends)	/* should never happen */
		elog(PANIC, "too many conflicting locks found");

	return vxids;
}


/*
 * AtPrepare_Locks
 *		Do the preparatory work for a PREPARE: make 2PC state file records
 *		for all locks currently held.
 *
 * Non-transactional locks are ignored, as are VXID locks.
 *
 * There are some special cases that we error out on: we can't be holding
 * any session locks (should be OK since only VACUUM uses those) and we
 * can't be holding any locks on temporary objects (since that would mess
 * up the current backend if it tries to exit before the prepared xact is
 * committed).
 */
void
AtPrepare_Locks(void)
{
	HASH_SEQ_STATUS status;
	LOCALLOCK  *locallock;

	/*
	 * We don't need to touch shared memory for this --- all the necessary
	 * state information is in the locallock table.
	 */
	hash_seq_init(&status, LockMethodLocalHash);

	while ((locallock = (LOCALLOCK *) hash_seq_search(&status)) != NULL)
	{
		TwoPhaseLockRecord record;
		LOCALLOCKOWNER *lockOwners = locallock->lockOwners;
		int			i;

		/* Ignore nontransactional locks */
		if (!LockMethods[LOCALLOCK_LOCKMETHOD(*locallock)]->transactional)
			continue;

		/*
		 * Ignore VXID locks.  We don't want those to be held by prepared
		 * transactions, since they aren't meaningful after a restart.
		 */
		if (locallock->tag.lock.locktag_type == LOCKTAG_VIRTUALTRANSACTION)
			continue;

		/* Ignore it if we don't actually hold the lock */
		if (locallock->nLocks <= 0)
			continue;

		/* Scan to verify there are no session locks */
		for (i = locallock->numLockOwners - 1; i >= 0; i--)
		{
			/* elog not ereport since this should not happen */
			if (lockOwners[i].owner == NULL)
				elog(ERROR, "cannot PREPARE when session locks exist");
		}

		/* Can't handle it if the lock is on a temporary object */
		if (LockTagIsTemp(&locallock->tag.lock))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot PREPARE a transaction that has operated on temporary tables")));

		/*
		 * Create a 2PC record.
		 */
		memcpy(&(record.locktag), &(locallock->tag.lock), sizeof(LOCKTAG));
		record.lockmode = locallock->tag.mode;

		RegisterTwoPhaseRecord(TWOPHASE_RM_LOCK_ID, 0,
							   &record, sizeof(TwoPhaseLockRecord));
	}
}

/*
 * PostPrepare_Locks
 *		Clean up after successful PREPARE
 *
 * Here, we want to transfer ownership of our locks to a dummy PGPROC
 * that's now associated with the prepared transaction, and we want to
 * clean out the corresponding entries in the LOCALLOCK table.
 *
 * Note: by removing the LOCALLOCK entries, we are leaving dangling
 * pointers in the transaction's resource owner.  This is OK at the
 * moment since resowner.c doesn't try to free locks retail at a toplevel
 * transaction commit or abort.  We could alternatively zero out nLocks
 * and leave the LOCALLOCK entries to be garbage-collected by LockReleaseAll,
 * but that probably costs more cycles.
 */
void
PostPrepare_Locks(TransactionId xid)
{
	PGPROC	   *newproc = TwoPhaseGetDummyProc(xid);
	HASH_SEQ_STATUS status;
	LOCALLOCK  *locallock;
	LOCK	   *lock;
	PROCLOCK   *proclock;
	PROCLOCKTAG proclocktag;
	bool		found;
	int			partition;

	/* This is a critical section: any error means big trouble */
	START_CRIT_SECTION();

	/*
	 * First we run through the locallock table and get rid of unwanted
	 * entries, then we scan the process's proclocks and transfer them to the
	 * target proc.
	 *
	 * We do this separately because we may have multiple locallock entries
	 * pointing to the same proclock, and we daren't end up with any dangling
	 * pointers.
	 */
	hash_seq_init(&status, LockMethodLocalHash);

	while ((locallock = (LOCALLOCK *) hash_seq_search(&status)) != NULL)
	{
		if (locallock->proclock == NULL || locallock->lock == NULL)
		{
			/*
			 * We must've run out of shared memory while trying to set up this
			 * lock.  Just forget the local entry.
			 */
			Assert(locallock->nLocks == 0);
			RemoveLocalLock(locallock);
			continue;
		}

		/* Ignore nontransactional locks */
		if (!LockMethods[LOCALLOCK_LOCKMETHOD(*locallock)]->transactional)
			continue;

		/* Ignore VXID locks */
		if (locallock->tag.lock.locktag_type == LOCKTAG_VIRTUALTRANSACTION)
			continue;

		/* We already checked there are no session locks */

		/* Mark the proclock to show we need to release this lockmode */
		if (locallock->nLocks > 0)
			locallock->proclock->releaseMask |= LOCKBIT_ON(locallock->tag.mode);

		/* And remove the locallock hashtable entry */
		RemoveLocalLock(locallock);
	}

	/*
	 * Now, scan each lock partition separately.
	 */
	for (partition = 0; partition < NUM_LOCK_PARTITIONS; partition++)
	{
		LWLockId	partitionLock = FirstLockMgrLock + partition;
		SHM_QUEUE  *procLocks = &(MyProc->myProcLocks[partition]);

		proclock = (PROCLOCK *) SHMQueueNext(procLocks, procLocks,
											 offsetof(PROCLOCK, procLink));

		if (!proclock)
			continue;			/* needn't examine this partition */

		LWLockAcquire(partitionLock, LW_EXCLUSIVE);

		while (proclock)
		{
			PROCLOCK   *nextplock;
			LOCKMASK	holdMask;
			PROCLOCK   *newproclock;

			/* Get link first, since we may unlink/delete this proclock */
			nextplock = (PROCLOCK *)
				SHMQueueNext(procLocks, &proclock->procLink,
							 offsetof(PROCLOCK, procLink));

			Assert(proclock->tag.myProc == MyProc);

			lock = proclock->tag.myLock;

			/* Ignore nontransactional locks */
			if (!LockMethods[LOCK_LOCKMETHOD(*lock)]->transactional)
				goto next_item;

			/* Ignore VXID locks */
			if (lock->tag.locktag_type == LOCKTAG_VIRTUALTRANSACTION)
				goto next_item;

			PROCLOCK_PRINT("PostPrepare_Locks", proclock);
			LOCK_PRINT("PostPrepare_Locks", lock, 0);
			Assert(lock->nRequested >= 0);
			Assert(lock->nGranted >= 0);
			Assert(lock->nGranted <= lock->nRequested);
			Assert((proclock->holdMask & ~lock->grantMask) == 0);

			/*
			 * Since there were no session locks, we should be releasing all
			 * locks
			 */
			if (proclock->releaseMask != proclock->holdMask)
				elog(PANIC, "we seem to have dropped a bit somewhere");

			holdMask = proclock->holdMask;

			/*
			 * We cannot simply modify proclock->tag.myProc to reassign
			 * ownership of the lock, because that's part of the hash key and
			 * the proclock would then be in the wrong hash chain.	So, unlink
			 * and delete the old proclock; create a new one with the right
			 * contents; and link it into place.  We do it in this order to be
			 * certain we won't run out of shared memory (the way dynahash.c
			 * works, the deleted object is certain to be available for
			 * reallocation).
			 */
			SHMQueueDelete(&proclock->lockLink);
			SHMQueueDelete(&proclock->procLink);
			if (!hash_search(LockMethodProcLockHash,
							 (void *) &(proclock->tag),
							 HASH_REMOVE, NULL))
				elog(PANIC, "proclock table corrupted");

			/*
			 * Create the hash key for the new proclock table.
			 */
			proclocktag.myLock = lock;
			proclocktag.myProc = newproc;

			newproclock = (PROCLOCK *) hash_search(LockMethodProcLockHash,
												   (void *) &proclocktag,
												   HASH_ENTER_NULL, &found);
			if (!newproclock)
				ereport(PANIC,	/* should not happen */
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of shared memory"),
						 errdetail("Not enough memory for reassigning the prepared transaction's locks.")));

			/*
			 * If new, initialize the new entry
			 */
			if (!found)
			{
				newproclock->holdMask = 0;
				newproclock->releaseMask = 0;
				/* Add new proclock to appropriate lists */
				SHMQueueInsertBefore(&lock->procLocks, &newproclock->lockLink);
				SHMQueueInsertBefore(&(newproc->myProcLocks[partition]),
									 &newproclock->procLink);
				PROCLOCK_PRINT("PostPrepare_Locks: new", newproclock);
			}
			else
			{
				PROCLOCK_PRINT("PostPrepare_Locks: found", newproclock);
				Assert((newproclock->holdMask & ~lock->grantMask) == 0);
			}

			/*
			 * Pass over the identified lock ownership.
			 */
			Assert((newproclock->holdMask & holdMask) == 0);
			newproclock->holdMask |= holdMask;

	next_item:
			proclock = nextplock;
		}						/* loop over PROCLOCKs within this partition */

		LWLockRelease(partitionLock);
	}							/* loop over partitions */

	END_CRIT_SECTION();
}


/*
 * Estimate shared-memory space used for lock tables
 */
Size
LockShmemSize(void)
{
	Size		size = 0;
	long		max_table_size;

	/* lock hash table */
	max_table_size = NLOCKENTS();
	size = add_size(size, hash_estimate_size(max_table_size, sizeof(LOCK)));

	/* proclock hash table */
	max_table_size *= 2;
	size = add_size(size, hash_estimate_size(max_table_size, sizeof(PROCLOCK)));

	/*
	 * Since NLOCKENTS is only an estimate, add 10% safety margin.
	 */
	size = add_size(size, size / 10);

	return size;
}

/*
 * GetLockStatusData - Return a summary of the lock manager's internal
 * status, for use in a user-level reporting function.
 *
 * The return data consists of an array of PROCLOCK objects, with the
 * associated PGPROC and LOCK objects for each.  Note that multiple
 * copies of the same PGPROC and/or LOCK objects are likely to appear.
 * It is the caller's responsibility to match up duplicates if wanted.
 *
 * The design goal is to hold the LWLocks for as short a time as possible;
 * thus, this function simply makes a copy of the necessary data and releases
 * the locks, allowing the caller to contemplate and format the data for as
 * long as it pleases.
 */
LockData *
GetLockStatusData(void)
{
	LockData   *data;
	PROCLOCK   *proclock;
	HASH_SEQ_STATUS seqstat;
	int			els;
	int			el;
	int			i;

	data = (LockData *) palloc(sizeof(LockData));

	/*
	 * Acquire lock on the entire shared lock data structure.  We can't
	 * operate one partition at a time if we want to deliver a self-consistent
	 * view of the state.
	 *
	 * Since this is a read-only operation, we take shared instead of
	 * exclusive lock.	There's not a whole lot of point to this, because all
	 * the normal operations require exclusive lock, but it doesn't hurt
	 * anything either. It will at least allow two backends to do
	 * GetLockStatusData in parallel.
	 *
	 * Must grab LWLocks in partition-number order to avoid LWLock deadlock.
	 */
	for (i = 0; i < NUM_LOCK_PARTITIONS; i++)
		LWLockAcquire(FirstLockMgrLock + i, LW_SHARED);

	/* Now we can safely count the number of proclocks */
	els = hash_get_num_entries(LockMethodProcLockHash);

	data->nelements = els;
	data->proclocks = (PROCLOCK *) palloc(sizeof(PROCLOCK) * els);
	data->procs = (PGPROC *) palloc(sizeof(PGPROC) * els);
	data->locks = (LOCK *) palloc(sizeof(LOCK) * els);

	/* Now scan the tables to copy the data */
	hash_seq_init(&seqstat, LockMethodProcLockHash);

	el = 0;
	while ((proclock = (PROCLOCK *) hash_seq_search(&seqstat)))
	{
		PGPROC	   *proc = proclock->tag.myProc;
		LOCK	   *lock = proclock->tag.myLock;

		memcpy(&(data->proclocks[el]), proclock, sizeof(PROCLOCK));
		memcpy(&(data->procs[el]), proc, sizeof(PGPROC));
		memcpy(&(data->locks[el]), lock, sizeof(LOCK));

		el++;
	}

	/*
	 * And release locks.  We do this in reverse order for two reasons: (1)
	 * Anyone else who needs more than one of the locks will be trying to lock
	 * them in increasing order; we don't want to release the other process
	 * until it can get all the locks it needs. (2) This avoids O(N^2)
	 * behavior inside LWLockRelease.
	 */
	for (i = NUM_LOCK_PARTITIONS; --i >= 0;)
		LWLockRelease(FirstLockMgrLock + i);

	Assert(el == data->nelements);

	return data;
}

/* Provide the textual name of any lock mode */
const char *
GetLockmodeName(LOCKMETHODID lockmethodid, LOCKMODE mode)
{
	Assert(lockmethodid > 0 && lockmethodid < lengthof(LockMethods));
	Assert(mode > 0 && mode <= LockMethods[lockmethodid]->numLockModes);
	return LockMethods[lockmethodid]->lockModeNames[mode];
}

#ifdef LOCK_DEBUG
/*
 * Dump all locks in the given proc's myProcLocks lists.
 *
 * Caller is responsible for having acquired appropriate LWLocks.
 */
void
DumpLocks(PGPROC *proc)
{
	SHM_QUEUE  *procLocks;
	PROCLOCK   *proclock;
	LOCK	   *lock;
	int			i;

	if (proc == NULL)
		return;

	if (proc->waitLock)
		LOCK_PRINT("DumpLocks: waiting on", proc->waitLock, 0);

	for (i = 0; i < NUM_LOCK_PARTITIONS; i++)
	{
		procLocks = &(proc->myProcLocks[i]);

		proclock = (PROCLOCK *) SHMQueueNext(procLocks, procLocks,
											 offsetof(PROCLOCK, procLink));

		while (proclock)
		{
			Assert(proclock->tag.myProc == proc);

			lock = proclock->tag.myLock;

			PROCLOCK_PRINT("DumpLocks", proclock);
			LOCK_PRINT("DumpLocks", lock, 0);

			proclock = (PROCLOCK *)
				SHMQueueNext(procLocks, &proclock->procLink,
							 offsetof(PROCLOCK, procLink));
		}
	}
}

/*
 * Dump all lmgr locks.
 *
 * Caller is responsible for having acquired appropriate LWLocks.
 */
void
DumpAllLocks(void)
{
	PGPROC	   *proc;
	PROCLOCK   *proclock;
	LOCK	   *lock;
	HASH_SEQ_STATUS status;

	proc = MyProc;

	if (proc && proc->waitLock)
		LOCK_PRINT("DumpAllLocks: waiting on", proc->waitLock, 0);

	hash_seq_init(&status, LockMethodProcLockHash);

	while ((proclock = (PROCLOCK *) hash_seq_search(&status)) != NULL)
	{
		PROCLOCK_PRINT("DumpAllLocks", proclock);

		lock = proclock->tag.myLock;
		if (lock)
			LOCK_PRINT("DumpAllLocks", lock, 0);
		else
			elog(LOG, "DumpAllLocks: proclock->tag.myLock = NULL");
	}
}
#endif   /* LOCK_DEBUG */

/*
 * LOCK 2PC resource manager's routines
 */

/*
 * Re-acquire a lock belonging to a transaction that was prepared.
 *
 * Because this function is run at db startup, re-acquiring the locks should
 * never conflict with running transactions because there are none.  We
 * assume that the lock state represented by the stored 2PC files is legal.
 */
void
lock_twophase_recover(TransactionId xid, uint16 info,
					  void *recdata, uint32 len)
{
	TwoPhaseLockRecord *rec = (TwoPhaseLockRecord *) recdata;
	PGPROC	   *proc = TwoPhaseGetDummyProc(xid);
	LOCKTAG    *locktag;
	LOCKMODE	lockmode;
	LOCKMETHODID lockmethodid;
	LOCK	   *lock;
	PROCLOCK   *proclock;
	PROCLOCKTAG proclocktag;
	bool		found;
	uint32		hashcode;
	uint32		proclock_hashcode;
	int			partition;
	LWLockId	partitionLock;
	LockMethod	lockMethodTable;

	Assert(len == sizeof(TwoPhaseLockRecord));
	locktag = &rec->locktag;
	lockmode = rec->lockmode;
	lockmethodid = locktag->locktag_lockmethodid;

	if (lockmethodid <= 0 || lockmethodid >= lengthof(LockMethods))
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);
	lockMethodTable = LockMethods[lockmethodid];

	hashcode = LockTagHashCode(locktag);
	partition = LockHashPartition(hashcode);
	partitionLock = LockHashPartitionLock(hashcode);

	LWLockAcquire(partitionLock, LW_EXCLUSIVE);

	/*
	 * Find or create a lock with this tag.
	 */
	lock = (LOCK *) hash_search_with_hash_value(LockMethodLockHash,
												(void *) locktag,
												hashcode,
												HASH_ENTER_NULL,
												&found);
	if (!lock)
	{
		LWLockRelease(partitionLock);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory"),
		  errhint("You might need to increase max_locks_per_transaction.")));
	}

	/*
	 * if it's a new lock object, initialize it
	 */
	if (!found)
	{
		lock->grantMask = 0;
		lock->waitMask = 0;
		SHMQueueInit(&(lock->procLocks));
		ProcQueueInit(&(lock->waitProcs));
		lock->nRequested = 0;
		lock->nGranted = 0;
		MemSet(lock->requested, 0, sizeof(int) * MAX_LOCKMODES);
		MemSet(lock->granted, 0, sizeof(int) * MAX_LOCKMODES);
		LOCK_PRINT("lock_twophase_recover: new", lock, lockmode);
	}
	else
	{
		LOCK_PRINT("lock_twophase_recover: found", lock, lockmode);
		Assert((lock->nRequested >= 0) && (lock->requested[lockmode] >= 0));
		Assert((lock->nGranted >= 0) && (lock->granted[lockmode] >= 0));
		Assert(lock->nGranted <= lock->nRequested);
	}

	/*
	 * Create the hash key for the proclock table.
	 */
	proclocktag.myLock = lock;
	proclocktag.myProc = proc;

	proclock_hashcode = ProcLockHashCode(&proclocktag, hashcode);

	/*
	 * Find or create a proclock entry with this tag
	 */
	proclock = (PROCLOCK *) hash_search_with_hash_value(LockMethodProcLockHash,
														(void *) &proclocktag,
														proclock_hashcode,
														HASH_ENTER_NULL,
														&found);
	if (!proclock)
	{
		/* Ooops, not enough shmem for the proclock */
		if (lock->nRequested == 0)
		{
			/*
			 * There are no other requestors of this lock, so garbage-collect
			 * the lock object.  We *must* do this to avoid a permanent leak
			 * of shared memory, because there won't be anything to cause
			 * anyone to release the lock object later.
			 */
			Assert(SHMQueueEmpty(&(lock->procLocks)));
			if (!hash_search_with_hash_value(LockMethodLockHash,
											 (void *) &(lock->tag),
											 hashcode,
											 HASH_REMOVE,
											 NULL))
				elog(PANIC, "lock table corrupted");
		}
		LWLockRelease(partitionLock);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory"),
		  errhint("You might need to increase max_locks_per_transaction.")));
	}

	/*
	 * If new, initialize the new entry
	 */
	if (!found)
	{
		proclock->holdMask = 0;
		proclock->releaseMask = 0;
		/* Add proclock to appropriate lists */
		SHMQueueInsertBefore(&lock->procLocks, &proclock->lockLink);
		SHMQueueInsertBefore(&(proc->myProcLocks[partition]),
							 &proclock->procLink);
		PROCLOCK_PRINT("lock_twophase_recover: new", proclock);
	}
	else
	{
		PROCLOCK_PRINT("lock_twophase_recover: found", proclock);
		Assert((proclock->holdMask & ~lock->grantMask) == 0);
	}

	/*
	 * lock->nRequested and lock->requested[] count the total number of
	 * requests, whether granted or waiting, so increment those immediately.
	 */
	lock->nRequested++;
	lock->requested[lockmode]++;
	Assert((lock->nRequested > 0) && (lock->requested[lockmode] > 0));

	/*
	 * We shouldn't already hold the desired lock.
	 */
	if (proclock->holdMask & LOCKBIT_ON(lockmode))
		elog(ERROR, "lock %s on object %u/%u/%u is already held",
			 lockMethodTable->lockModeNames[lockmode],
			 lock->tag.locktag_field1, lock->tag.locktag_field2,
			 lock->tag.locktag_field3);

	/*
	 * We ignore any possible conflicts and just grant ourselves the lock.
	 */
	GrantLock(lock, proclock, lockmode);

	LWLockRelease(partitionLock);
}

/*
 * 2PC processing routine for COMMIT PREPARED case.
 *
 * Find and release the lock indicated by the 2PC record.
 */
void
lock_twophase_postcommit(TransactionId xid, uint16 info,
						 void *recdata, uint32 len)
{
	TwoPhaseLockRecord *rec = (TwoPhaseLockRecord *) recdata;
	PGPROC	   *proc = TwoPhaseGetDummyProc(xid);
	LOCKTAG    *locktag;
	LOCKMODE	lockmode;
	LOCKMETHODID lockmethodid;
	LOCK	   *lock;
	PROCLOCK   *proclock;
	PROCLOCKTAG proclocktag;
	uint32		hashcode;
	uint32		proclock_hashcode;
	LWLockId	partitionLock;
	LockMethod	lockMethodTable;
	bool		wakeupNeeded;

	Assert(len == sizeof(TwoPhaseLockRecord));
	locktag = &rec->locktag;
	lockmode = rec->lockmode;
	lockmethodid = locktag->locktag_lockmethodid;

	if (lockmethodid <= 0 || lockmethodid >= lengthof(LockMethods))
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);
	lockMethodTable = LockMethods[lockmethodid];

	hashcode = LockTagHashCode(locktag);
	partitionLock = LockHashPartitionLock(hashcode);

	LWLockAcquire(partitionLock, LW_EXCLUSIVE);

	/*
	 * Re-find the lock object (it had better be there).
	 */
	lock = (LOCK *) hash_search_with_hash_value(LockMethodLockHash,
												(void *) locktag,
												hashcode,
												HASH_FIND,
												NULL);
	if (!lock)
		elog(PANIC, "failed to re-find shared lock object");

	/*
	 * Re-find the proclock object (ditto).
	 */
	proclocktag.myLock = lock;
	proclocktag.myProc = proc;

	proclock_hashcode = ProcLockHashCode(&proclocktag, hashcode);

	proclock = (PROCLOCK *) hash_search_with_hash_value(LockMethodProcLockHash,
														(void *) &proclocktag,
														proclock_hashcode,
														HASH_FIND,
														NULL);
	if (!proclock)
		elog(PANIC, "failed to re-find shared proclock object");

	/*
	 * Double-check that we are actually holding a lock of the type we want to
	 * release.
	 */
	if (!(proclock->holdMask & LOCKBIT_ON(lockmode)))
	{
		PROCLOCK_PRINT("lock_twophase_postcommit: WRONGTYPE", proclock);
		LWLockRelease(partitionLock);
		elog(WARNING, "you don't own a lock of type %s",
			 lockMethodTable->lockModeNames[lockmode]);
		return;
	}

	/*
	 * Do the releasing.  CleanUpLock will waken any now-wakable waiters.
	 */
	wakeupNeeded = UnGrantLock(lock, lockmode, proclock, lockMethodTable);

	CleanUpLock(lock, proclock,
				lockMethodTable, hashcode,
				wakeupNeeded);

	LWLockRelease(partitionLock);
}

/*
 * 2PC processing routine for ROLLBACK PREPARED case.
 *
 * This is actually just the same as the COMMIT case.
 */
void
lock_twophase_postabort(TransactionId xid, uint16 info,
						void *recdata, uint32 len)
{
	lock_twophase_postcommit(xid, info, recdata, len);
}
