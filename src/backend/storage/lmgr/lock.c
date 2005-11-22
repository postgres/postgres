/*-------------------------------------------------------------------------
 *
 * lock.c
 *	  POSTGRES low-level lock mechanism
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/lmgr/lock.c,v 1.159.2.1 2005/11/22 18:23:18 momjian Exp $
 *
 * NOTES
 *	  Outside modules can create a lock table and acquire/release
 *	  locks.  A lock table is a shared memory hash table.  When
 *	  a process tries to acquire a lock of a type that conflicts
 *	  with existing locks, it is put to sleep using the routines
 *	  in storage/lmgr/proc.c.
 *
 *	  For the most part, this code should be invoked via lmgr.c
 *	  or another lock-management module, not directly.
 *
 *	Interface:
 *
 *	LockAcquire(), LockRelease(), LockMethodTableInit(),
 *	LockMethodTableRename(), LockReleaseAll(),
 *	LockCheckConflicts(), GrantLock()
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>

#include "access/twophase.h"
#include "access/twophase_rmgr.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/resowner.h"


/* This configuration variable is used to set the lock table size */
int			max_locks_per_xact; /* set by guc.c */

#define NLOCKENTS() \
	mul_size(max_locks_per_xact, add_size(MaxBackends, max_prepared_xacts))


/* Record that's written to 2PC state file when a lock is persisted */
typedef struct TwoPhaseLockRecord
{
	LOCKTAG		locktag;
	LOCKMODE	lockmode;
} TwoPhaseLockRecord;


/*
 * map from lock method id to the lock table data structures
 */
static LockMethod LockMethods[MAX_LOCK_METHODS];
static HTAB *LockMethodLockHash[MAX_LOCK_METHODS];
static HTAB *LockMethodProcLockHash[MAX_LOCK_METHODS];
static HTAB *LockMethodLocalHash[MAX_LOCK_METHODS];

/* exported so lmgr.c can initialize it */
int			NumLockMethods;


/* private state for GrantAwaitedLock */
static LOCALLOCK *awaitedLock;
static ResourceOwner awaitedOwner;


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
LOCK_DEBUG_ENABLED(const LOCK *lock)
{
	return
		(((Trace_locks && LOCK_LOCKMETHOD(*lock) == DEFAULT_LOCKMETHOD)
		  || (Trace_userlocks && LOCK_LOCKMETHOD(*lock) == USER_LOCKMETHOD))
		 && ((Oid) lock->tag.locktag_field2 >= (Oid) Trace_lock_oidmin))
		|| (Trace_lock_table
			&& (lock->tag.locktag_field2 == Trace_lock_table));
}


inline static void
LOCK_PRINT(const char *where, const LOCK *lock, LOCKMODE type)
{
	if (LOCK_DEBUG_ENABLED(lock))
		elog(LOG,
			 "%s: lock(%lx) id(%u,%u,%u,%u,%u,%u) grantMask(%x) "
			 "req(%d,%d,%d,%d,%d,%d,%d)=%d "
			 "grant(%d,%d,%d,%d,%d,%d,%d)=%d wait(%d) type(%s)",
			 where, MAKE_OFFSET(lock),
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
			 lock->waitProcs.size, lock_mode_names[type]);
}


inline static void
PROCLOCK_PRINT(const char *where, const PROCLOCK *proclockP)
{
	if (LOCK_DEBUG_ENABLED((LOCK *) MAKE_PTR(proclockP->tag.lock)))
		elog(LOG,
			 "%s: proclock(%lx) lock(%lx) method(%u) proc(%lx) hold(%x)",
			 where, MAKE_OFFSET(proclockP), proclockP->tag.lock,
			 PROCLOCK_LOCKMETHOD(*(proclockP)),
			 proclockP->tag.proc, (int) proclockP->holdMask);
}
#else							/* not LOCK_DEBUG */

#define LOCK_PRINT(where, lock, type)
#define PROCLOCK_PRINT(where, proclockP)
#endif   /* not LOCK_DEBUG */


static void RemoveLocalLock(LOCALLOCK *locallock);
static void GrantLockLocal(LOCALLOCK *locallock, ResourceOwner owner);
static void WaitOnLock(LOCKMETHODID lockmethodid, LOCALLOCK *locallock,
		   ResourceOwner owner);
static bool UnGrantLock(LOCK *lock, LOCKMODE lockmode,
			PROCLOCK *proclock, LockMethod lockMethodTable);
static void CleanUpLock(LOCKMETHODID lockmethodid, LOCK *lock,
			PROCLOCK *proclock, bool wakeupNeeded);


/*
 * InitLocks -- Init the lock module.  Nothing to do here at present.
 */
void
InitLocks(void)
{
	/* NOP */
}


/*
 * Fetch the lock method table associated with a given lock
 */
LockMethod
GetLocksMethodTable(LOCK *lock)
{
	LOCKMETHODID lockmethodid = LOCK_LOCKMETHOD(*lock);

	Assert(0 < lockmethodid && lockmethodid < NumLockMethods);
	return LockMethods[lockmethodid];
}


/*
 * LockMethodInit -- initialize the lock table's lock type
 *		structures
 *
 * Notes: just copying.  Should only be called once.
 */
static void
LockMethodInit(LockMethod lockMethodTable,
			   const LOCKMASK *conflictsP,
			   int numModes)
{
	int			i;

	lockMethodTable->numLockModes = numModes;
	/* copies useless zero element as well as the N lockmodes */
	for (i = 0; i <= numModes; i++)
		lockMethodTable->conflictTab[i] = conflictsP[i];
}

/*
 * LockMethodTableInit -- initialize a lock table structure
 *
 * NOTE: data structures allocated here are allocated permanently, using
 * TopMemoryContext and shared memory.	We don't ever release them anyway,
 * and in normal multi-backend operation the lock table structures set up
 * by the postmaster are inherited by each backend, so they must be in
 * TopMemoryContext.
 */
LOCKMETHODID
LockMethodTableInit(const char *tabName,
					const LOCKMASK *conflictsP,
					int numModes)
{
	LockMethod	newLockMethod;
	LOCKMETHODID lockmethodid;
	char	   *shmemName;
	HASHCTL		info;
	int			hash_flags;
	bool		found;
	long		init_table_size,
				max_table_size;

	if (numModes >= MAX_LOCKMODES)
		elog(ERROR, "too many lock types %d (limit is %d)",
			 numModes, MAX_LOCKMODES - 1);

	/* Compute init/max size to request for lock hashtables */
	max_table_size = NLOCKENTS();
	init_table_size = max_table_size / 2;

	/* Allocate a string for the shmem index table lookups. */
	/* This is just temp space in this routine, so palloc is OK. */
	shmemName = (char *) palloc(strlen(tabName) + 32);

	/* each lock table has a header in shared memory */
	sprintf(shmemName, "%s (lock method table)", tabName);
	newLockMethod = (LockMethod)
		ShmemInitStruct(shmemName, sizeof(LockMethodData), &found);

	if (!newLockMethod)
		elog(FATAL, "could not initialize lock table \"%s\"", tabName);

	/*
	 * we're first - initialize
	 */
	if (!found)
	{
		MemSet(newLockMethod, 0, sizeof(LockMethodData));
		newLockMethod->masterLock = LockMgrLock;
		LockMethodInit(newLockMethod, conflictsP, numModes);
	}

	/*
	 * other modules refer to the lock table by a lockmethod ID
	 */
	Assert(NumLockMethods < MAX_LOCK_METHODS);
	lockmethodid = NumLockMethods++;
	LockMethods[lockmethodid] = newLockMethod;

	/*
	 * allocate a hash table for LOCK structs.	This is used to store
	 * per-locked-object information.
	 */
	MemSet(&info, 0, sizeof(info));
	info.keysize = sizeof(LOCKTAG);
	info.entrysize = sizeof(LOCK);
	info.hash = tag_hash;
	hash_flags = (HASH_ELEM | HASH_FUNCTION);

	sprintf(shmemName, "%s (lock hash)", tabName);
	LockMethodLockHash[lockmethodid] = ShmemInitHash(shmemName,
													 init_table_size,
													 max_table_size,
													 &info,
													 hash_flags);

	if (!LockMethodLockHash[lockmethodid])
		elog(FATAL, "could not initialize lock table \"%s\"", tabName);

	/*
	 * allocate a hash table for PROCLOCK structs.	This is used to store
	 * per-lock-holder information.
	 */
	info.keysize = sizeof(PROCLOCKTAG);
	info.entrysize = sizeof(PROCLOCK);
	info.hash = tag_hash;
	hash_flags = (HASH_ELEM | HASH_FUNCTION);

	sprintf(shmemName, "%s (proclock hash)", tabName);
	LockMethodProcLockHash[lockmethodid] = ShmemInitHash(shmemName,
														 init_table_size,
														 max_table_size,
														 &info,
														 hash_flags);

	if (!LockMethodProcLockHash[lockmethodid])
		elog(FATAL, "could not initialize lock table \"%s\"", tabName);

	/*
	 * allocate a non-shared hash table for LOCALLOCK structs.	This is used
	 * to store lock counts and resource owner information.
	 *
	 * The non-shared table could already exist in this process (this occurs
	 * when the postmaster is recreating shared memory after a backend crash).
	 * If so, delete and recreate it.  (We could simply leave it, since it
	 * ought to be empty in the postmaster, but for safety let's zap it.)
	 */
	if (LockMethodLocalHash[lockmethodid])
		hash_destroy(LockMethodLocalHash[lockmethodid]);

	info.keysize = sizeof(LOCALLOCKTAG);
	info.entrysize = sizeof(LOCALLOCK);
	info.hash = tag_hash;
	hash_flags = (HASH_ELEM | HASH_FUNCTION);

	sprintf(shmemName, "%s (locallock hash)", tabName);
	LockMethodLocalHash[lockmethodid] = hash_create(shmemName,
													128,
													&info,
													hash_flags);

	pfree(shmemName);

	return lockmethodid;
}

/*
 * LockMethodTableRename -- allocate another lockmethod ID to the same
 *		lock table.
 *
 * NOTES: This function makes it possible to have different lockmethodids,
 *		and hence different locking semantics, while still storing all
 *		the data in one shared-memory hashtable.
 */

LOCKMETHODID
LockMethodTableRename(LOCKMETHODID lockmethodid)
{
	LOCKMETHODID newLockMethodId;

	if (NumLockMethods >= MAX_LOCK_METHODS)
		return INVALID_LOCKMETHOD;
	if (LockMethods[lockmethodid] == INVALID_LOCKMETHOD)
		return INVALID_LOCKMETHOD;

	/* other modules refer to the lock table by a lockmethod ID */
	newLockMethodId = NumLockMethods;
	NumLockMethods++;

	LockMethods[newLockMethodId] = LockMethods[lockmethodid];
	LockMethodLockHash[newLockMethodId] = LockMethodLockHash[lockmethodid];
	LockMethodProcLockHash[newLockMethodId] = LockMethodProcLockHash[lockmethodid];
	LockMethodLocalHash[newLockMethodId] = LockMethodLocalHash[lockmethodid];

	return newLockMethodId;
}

/*
 * LockAcquire -- Check for lock conflicts, sleep if conflict found,
 *		set lock if/when no conflicts.
 *
 * Inputs:
 *	lockmethodid: identifies which lock table to use
 *	locktag: unique identifier for the lockable object
 *	isTempObject: is the lockable object a temporary object?  (Under 2PC,
 *		such locks cannot be persisted)
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
 *
 *
 * Note on User Locks:
 *
 *		User locks are handled totally on the application side as
 *		long term cooperative locks which extend beyond the normal
 *		transaction boundaries.  Their purpose is to indicate to an
 *		application that someone is `working' on an item.  So it is
 *		possible to put an user lock on a tuple's oid, retrieve the
 *		tuple, work on it for an hour and then update it and remove
 *		the lock.  While the lock is active other clients can still
 *		read and write the tuple but they can be aware that it has
 *		been locked at the application level by someone.
 *
 *		User locks and normal locks are completely orthogonal and
 *		they don't interfere with each other.
 *
 *		User locks are always non blocking, therefore they are never
 *		acquired if already held by another process.  They must be
 *		released explicitly by the application but they are released
 *		automatically when a backend terminates.
 *		They are indicated by a lockmethod 2 which is an alias for the
 *		normal lock table.
 *
 *		The lockmode parameter can have the same values for normal locks
 *		although probably only WRITE_LOCK can have some practical use.
 *
 *														DZ - 22 Nov 1997
 */
LockAcquireResult
LockAcquire(LOCKMETHODID lockmethodid,
			LOCKTAG *locktag,
			bool isTempObject,
			LOCKMODE lockmode,
			bool sessionLock,
			bool dontWait)
{
	LOCALLOCKTAG localtag;
	LOCALLOCK  *locallock;
	LOCK	   *lock;
	PROCLOCK   *proclock;
	PROCLOCKTAG proclocktag;
	bool		found;
	ResourceOwner owner;
	LWLockId	masterLock;
	LockMethod	lockMethodTable;
	int			status;

#ifdef LOCK_DEBUG
	if (Trace_userlocks && lockmethodid == USER_LOCKMETHOD)
		elog(LOG, "LockAcquire: user lock [%u,%u] %s",
			 locktag->locktag_field1, locktag->locktag_field2,
			 lock_mode_names[lockmode]);
#endif

	/* ugly */
	locktag->locktag_lockmethodid = lockmethodid;

	Assert(lockmethodid < NumLockMethods);
	lockMethodTable = LockMethods[lockmethodid];
	if (!lockMethodTable)
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);

	/* Session locks and user locks are not transactional */
	if (!sessionLock && lockmethodid == DEFAULT_LOCKMETHOD)
		owner = CurrentResourceOwner;
	else
		owner = NULL;

	/*
	 * Find or create a LOCALLOCK entry for this lock and lockmode
	 */
	MemSet(&localtag, 0, sizeof(localtag));		/* must clear padding */
	localtag.lock = *locktag;
	localtag.mode = lockmode;

	locallock = (LOCALLOCK *) hash_search(LockMethodLocalHash[lockmethodid],
										  (void *) &localtag,
										  HASH_ENTER, &found);

	/*
	 * if it's a new locallock object, initialize it
	 */
	if (!found)
	{
		locallock->lock = NULL;
		locallock->proclock = NULL;
		locallock->isTempObject = isTempObject;
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
		Assert(locallock->isTempObject == isTempObject);

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
	masterLock = lockMethodTable->masterLock;

	LWLockAcquire(masterLock, LW_EXCLUSIVE);

	/*
	 * Find or create a lock with this tag.
	 *
	 * Note: if the locallock object already existed, it might have a pointer
	 * to the lock already ... but we probably should not assume that that
	 * pointer is valid, since a lock object with no locks can go away
	 * anytime.
	 */
	lock = (LOCK *) hash_search(LockMethodLockHash[lockmethodid],
								(void *) locktag,
								HASH_ENTER_NULL, &found);
	if (!lock)
	{
		LWLockRelease(masterLock);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory"),
			errhint("You may need to increase max_locks_per_transaction.")));
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
	MemSet(&proclocktag, 0, sizeof(PROCLOCKTAG));		/* must clear padding */
	proclocktag.lock = MAKE_OFFSET(lock);
	proclocktag.proc = MAKE_OFFSET(MyProc);

	/*
	 * Find or create a proclock entry with this tag
	 */
	proclock = (PROCLOCK *) hash_search(LockMethodProcLockHash[lockmethodid],
										(void *) &proclocktag,
										HASH_ENTER_NULL, &found);
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
			if (!hash_search(LockMethodLockHash[lockmethodid],
							 (void *) &(lock->tag),
							 HASH_REMOVE, NULL))
				elog(PANIC, "lock table corrupted");
		}
		LWLockRelease(masterLock);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory"),
			errhint("You may need to increase max_locks_per_transaction.")));
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
		SHMQueueInsertBefore(&MyProc->procLocks, &proclock->procLink);
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
						 lock_mode_names[i], lock_mode_names[lockmode],
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
			 lock_mode_names[lockmode],
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
				if (!hash_search(LockMethodProcLockHash[lockmethodid],
								 (void *) &(proclock->tag),
								 HASH_REMOVE, NULL))
					elog(PANIC, "proclock table corrupted");
			}
			else
				PROCLOCK_PRINT("LockAcquire: NOWAIT", proclock);
			lock->nRequested--;
			lock->requested[lockmode]--;
			LOCK_PRINT("LockAcquire: conditional lock failed", lock, lockmode);
			Assert((lock->nRequested > 0) && (lock->requested[lockmode] >= 0));
			Assert(lock->nGranted <= lock->nRequested);
			LWLockRelease(masterLock);
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
		WaitOnLock(lockmethodid, locallock, owner);

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
			LWLockRelease(masterLock);
			elog(ERROR, "LockAcquire failed");
		}
		PROCLOCK_PRINT("LockAcquire: granted", proclock);
		LOCK_PRINT("LockAcquire: granted", lock, lockmode);
	}

	LWLockRelease(masterLock);

	return LOCKACQUIRE_OK;
}

/*
 * Subroutine to free a locallock entry
 */
static void
RemoveLocalLock(LOCALLOCK *locallock)
{
	LOCKMETHODID lockmethodid = LOCALLOCK_LOCKMETHOD(*locallock);

	pfree(locallock->lockOwners);
	locallock->lockOwners = NULL;
	if (!hash_search(LockMethodLocalHash[lockmethodid],
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
 * The locktable's masterLock must be held at entry, and will be
 * held at exit.
 */
static void
CleanUpLock(LOCKMETHODID lockmethodid, LOCK *lock, PROCLOCK *proclock,
			bool wakeupNeeded)
{
	/*
	 * If this was my last hold on this lock, delete my entry in the proclock
	 * table.
	 */
	if (proclock->holdMask == 0)
	{
		PROCLOCK_PRINT("CleanUpLock: deleting", proclock);
		SHMQueueDelete(&proclock->lockLink);
		SHMQueueDelete(&proclock->procLink);
		if (!hash_search(LockMethodProcLockHash[lockmethodid],
						 (void *) &(proclock->tag),
						 HASH_REMOVE, NULL))
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
		if (!hash_search(LockMethodLockHash[lockmethodid],
						 (void *) &(lock->tag),
						 HASH_REMOVE, NULL))
			elog(PANIC, "lock table corrupted");
	}
	else if (wakeupNeeded)
	{
		/* There are waiters on this lock, so wake them up. */
		ProcLockWakeup(LockMethods[lockmethodid], lock);
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
 * The locktable's masterLock must be held at entry.
 */
static void
WaitOnLock(LOCKMETHODID lockmethodid, LOCALLOCK *locallock,
		   ResourceOwner owner)
{
	LockMethod	lockMethodTable = LockMethods[lockmethodid];
	const char *old_status;
	char	   *new_status;
	int			len;

	Assert(lockmethodid < NumLockMethods);

	LOCK_PRINT("WaitOnLock: sleeping on lock",
			   locallock->lock, locallock->tag.mode);

	old_status = get_ps_display(&len);
	new_status = (char *) palloc(len + 8 + 1);
	memcpy(new_status, old_status, len);
	strcpy(new_status + len, " waiting");
	set_ps_display(new_status);
	new_status[len] = '\0';		/* truncate off " waiting" */

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

	if (ProcSleep(lockMethodTable,
				  locallock->tag.mode,
				  locallock->lock,
				  locallock->proclock) != STATUS_OK)
	{
		/*
		 * We failed as a result of a deadlock, see CheckDeadLock(). Quit now.
		 */
		awaitedLock = NULL;
		LOCK_PRINT("WaitOnLock: aborting on lock",
				   locallock->lock, locallock->tag.mode);
		LWLockRelease(lockMethodTable->masterLock);

		/*
		 * Now that we aren't holding the LockMgrLock, we can give an error
		 * report including details about the detected deadlock.
		 */
		DeadLockReport();
		/* not reached */
	}

	awaitedLock = NULL;

	set_ps_display(new_status);
	pfree(new_status);

	LOCK_PRINT("WaitOnLock: wakeup on lock",
			   locallock->lock, locallock->tag.mode);
}

/*
 * Remove a proc from the wait-queue it is on
 * (caller must know it is on one).
 *
 * Locktable lock must be held by caller.
 *
 * NB: this does not clean up any locallock object that may exist for the lock.
 */
void
RemoveFromWaitQueue(PGPROC *proc)
{
	LOCK	   *waitLock = proc->waitLock;
	PROCLOCK   *proclock = proc->waitProcLock;
	LOCKMODE	lockmode = proc->waitLockMode;
	LOCKMETHODID lockmethodid = LOCK_LOCKMETHOD(*waitLock);

	/* Make sure proc is waiting */
	Assert(proc->links.next != INVALID_OFFSET);
	Assert(waitLock);
	Assert(waitLock->waitProcs.size > 0);
	Assert(0 < lockmethodid && lockmethodid < NumLockMethods);

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

	/* Clean up the proc's own state */
	proc->waitLock = NULL;
	proc->waitProcLock = NULL;

	/*
	 * Delete the proclock immediately if it represents no already-held locks.
	 * (This must happen now because if the owner of the lock decides to
	 * release it, and the requested/granted counts then go to zero,
	 * LockRelease expects there to be no remaining proclocks.) Then see if
	 * any other waiters for the lock can be woken up now.
	 */
	CleanUpLock(lockmethodid, waitLock, proclock, true);
}

/*
 * LockRelease -- look up 'locktag' in lock table 'lockmethodid' and
 *		release one 'lockmode' lock on it.	Release a session lock if
 *		'sessionLock' is true, else release a regular transaction lock.
 *
 * Side Effects: find any waiting processes that are now wakable,
 *		grant them their requested locks and awaken them.
 *		(We have to grant the lock here to avoid a race between
 *		the waking process and any new process to
 *		come along and request the lock.)
 */
bool
LockRelease(LOCKMETHODID lockmethodid, LOCKTAG *locktag,
			LOCKMODE lockmode, bool sessionLock)
{
	LOCALLOCKTAG localtag;
	LOCALLOCK  *locallock;
	LOCK	   *lock;
	PROCLOCK   *proclock;
	LWLockId	masterLock;
	LockMethod	lockMethodTable;
	bool		wakeupNeeded;

#ifdef LOCK_DEBUG
	if (Trace_userlocks && lockmethodid == USER_LOCKMETHOD)
		elog(LOG, "LockRelease: user lock [%u,%u] %s",
			 locktag->locktag_field1, locktag->locktag_field2,
			 lock_mode_names[lockmode]);
#endif

	/* ugly */
	locktag->locktag_lockmethodid = lockmethodid;

	Assert(lockmethodid < NumLockMethods);
	lockMethodTable = LockMethods[lockmethodid];
	if (!lockMethodTable)
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);

	/*
	 * Find the LOCALLOCK entry for this lock and lockmode
	 */
	MemSet(&localtag, 0, sizeof(localtag));		/* must clear padding */
	localtag.lock = *locktag;
	localtag.mode = lockmode;

	locallock = (LOCALLOCK *) hash_search(LockMethodLocalHash[lockmethodid],
										  (void *) &localtag,
										  HASH_FIND, NULL);

	/*
	 * let the caller print its own error message, too. Do not ereport(ERROR).
	 */
	if (!locallock || locallock->nLocks <= 0)
	{
		elog(WARNING, "you don't own a lock of type %s",
			 lock_mode_names[lockmode]);
		return FALSE;
	}

	/*
	 * Decrease the count for the resource owner.
	 */
	{
		LOCALLOCKOWNER *lockOwners = locallock->lockOwners;
		ResourceOwner owner;
		int			i;

		/* Session locks and user locks are not transactional */
		if (!sessionLock && lockmethodid == DEFAULT_LOCKMETHOD)
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
				 lock_mode_names[lockmode]);
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
	masterLock = lockMethodTable->masterLock;

	LWLockAcquire(masterLock, LW_EXCLUSIVE);

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
		LWLockRelease(masterLock);
		elog(WARNING, "you don't own a lock of type %s",
			 lock_mode_names[lockmode]);
		RemoveLocalLock(locallock);
		return FALSE;
	}

	/*
	 * Do the releasing.  CleanUpLock will waken any now-wakable waiters.
	 */
	wakeupNeeded = UnGrantLock(lock, lockmode, proclock, lockMethodTable);

	CleanUpLock(lockmethodid, lock, proclock, wakeupNeeded);

	LWLockRelease(masterLock);

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
	SHM_QUEUE  *procLocks = &(MyProc->procLocks);
	LWLockId	masterLock;
	LockMethod	lockMethodTable;
	int			i,
				numLockModes;
	LOCALLOCK  *locallock;
	PROCLOCK   *proclock;
	LOCK	   *lock;

#ifdef LOCK_DEBUG
	if (lockmethodid == USER_LOCKMETHOD ? Trace_userlocks : Trace_locks)
		elog(LOG, "LockReleaseAll: lockmethod=%d", lockmethodid);
#endif

	Assert(lockmethodid < NumLockMethods);
	lockMethodTable = LockMethods[lockmethodid];
	if (!lockMethodTable)
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);

	numLockModes = lockMethodTable->numLockModes;
	masterLock = lockMethodTable->masterLock;

	/*
	 * First we run through the locallock table and get rid of unwanted
	 * entries, then we scan the process's proclocks and get rid of those. We
	 * do this separately because we may have multiple locallock entries
	 * pointing to the same proclock, and we daren't end up with any dangling
	 * pointers.
	 */
	hash_seq_init(&status, LockMethodLocalHash[lockmethodid]);

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

	LWLockAcquire(masterLock, LW_EXCLUSIVE);

	proclock = (PROCLOCK *) SHMQueueNext(procLocks, procLocks,
										 offsetof(PROCLOCK, procLink));

	while (proclock)
	{
		bool		wakeupNeeded = false;
		PROCLOCK   *nextplock;

		/* Get link first, since we may unlink/delete this proclock */
		nextplock = (PROCLOCK *) SHMQueueNext(procLocks, &proclock->procLink,
											  offsetof(PROCLOCK, procLink));

		Assert(proclock->tag.proc == MAKE_OFFSET(MyProc));

		lock = (LOCK *) MAKE_PTR(proclock->tag.lock);

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
		CleanUpLock(lockmethodid, lock, proclock, wakeupNeeded);

next_item:
		proclock = nextplock;
	}

	LWLockRelease(masterLock);

#ifdef LOCK_DEBUG
	if (lockmethodid == USER_LOCKMETHOD ? Trace_userlocks : Trace_locks)
		elog(LOG, "LockReleaseAll done");
#endif
}

/*
 * LockReleaseCurrentOwner
 *		Release all locks belonging to CurrentResourceOwner
 *
 * Only DEFAULT_LOCKMETHOD locks can belong to a resource owner.
 */
void
LockReleaseCurrentOwner(void)
{
	HASH_SEQ_STATUS status;
	LOCALLOCK  *locallock;
	LOCALLOCKOWNER *lockOwners;
	int			i;

	hash_seq_init(&status, LockMethodLocalHash[DEFAULT_LOCKMETHOD]);

	while ((locallock = (LOCALLOCK *) hash_seq_search(&status)) != NULL)
	{
		/* Ignore items that must be nontransactional */
		if (LOCALLOCK_LOCKMETHOD(*locallock) != DEFAULT_LOCKMETHOD)
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
					if (!LockRelease(DEFAULT_LOCKMETHOD,
									 &locallock->tag.lock,
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

	hash_seq_init(&status, LockMethodLocalHash[DEFAULT_LOCKMETHOD]);

	while ((locallock = (LOCALLOCK *) hash_seq_search(&status)) != NULL)
	{
		int			i;
		int			ic = -1;
		int			ip = -1;

		/* Ignore items that must be nontransactional */
		if (LOCALLOCK_LOCKMETHOD(*locallock) != DEFAULT_LOCKMETHOD)
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
 * AtPrepare_Locks
 *		Do the preparatory work for a PREPARE: make 2PC state file records
 *		for all locks currently held.
 *
 * User locks are non-transactional and are therefore ignored.
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
	LOCKMETHODID lockmethodid = DEFAULT_LOCKMETHOD;
	HASH_SEQ_STATUS status;
	LOCALLOCK  *locallock;

	/*
	 * We don't need to touch shared memory for this --- all the necessary
	 * state information is in the locallock table.
	 */
	hash_seq_init(&status, LockMethodLocalHash[lockmethodid]);

	while ((locallock = (LOCALLOCK *) hash_seq_search(&status)) != NULL)
	{
		TwoPhaseLockRecord record;
		LOCALLOCKOWNER *lockOwners = locallock->lockOwners;
		int			i;

		/* Ignore items that are not of the lockmethod to be processed */
		if (LOCALLOCK_LOCKMETHOD(*locallock) != lockmethodid)
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
		if (locallock->isTempObject)
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
	LOCKMETHODID lockmethodid = DEFAULT_LOCKMETHOD;
	HASH_SEQ_STATUS status;
	SHM_QUEUE  *procLocks = &(MyProc->procLocks);
	LWLockId	masterLock;
	LockMethod	lockMethodTable;
	int			numLockModes;
	LOCALLOCK  *locallock;
	PROCLOCK   *proclock;
	PROCLOCKTAG proclocktag;
	bool		found;
	LOCK	   *lock;

	/* This is a critical section: any error means big trouble */
	START_CRIT_SECTION();

	lockMethodTable = LockMethods[lockmethodid];
	if (!lockMethodTable)
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);

	numLockModes = lockMethodTable->numLockModes;
	masterLock = lockMethodTable->masterLock;

	/*
	 * First we run through the locallock table and get rid of unwanted
	 * entries, then we scan the process's proclocks and transfer them to the
	 * target proc.
	 *
	 * We do this separately because we may have multiple locallock entries
	 * pointing to the same proclock, and we daren't end up with any dangling
	 * pointers.
	 */
	hash_seq_init(&status, LockMethodLocalHash[lockmethodid]);

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

		/* We already checked there are no session locks */

		/* Mark the proclock to show we need to release this lockmode */
		if (locallock->nLocks > 0)
			locallock->proclock->releaseMask |= LOCKBIT_ON(locallock->tag.mode);

		/* And remove the locallock hashtable entry */
		RemoveLocalLock(locallock);
	}

	LWLockAcquire(masterLock, LW_EXCLUSIVE);

	proclock = (PROCLOCK *) SHMQueueNext(procLocks, procLocks,
										 offsetof(PROCLOCK, procLink));

	while (proclock)
	{
		PROCLOCK   *nextplock;
		LOCKMASK	holdMask;
		PROCLOCK   *newproclock;

		/* Get link first, since we may unlink/delete this proclock */
		nextplock = (PROCLOCK *) SHMQueueNext(procLocks, &proclock->procLink,
											  offsetof(PROCLOCK, procLink));

		Assert(proclock->tag.proc == MAKE_OFFSET(MyProc));

		lock = (LOCK *) MAKE_PTR(proclock->tag.lock);

		/* Ignore items that are not of the lockmethod to be removed */
		if (LOCK_LOCKMETHOD(*lock) != lockmethodid)
			goto next_item;

		PROCLOCK_PRINT("PostPrepare_Locks", proclock);
		LOCK_PRINT("PostPrepare_Locks", lock, 0);
		Assert(lock->nRequested >= 0);
		Assert(lock->nGranted >= 0);
		Assert(lock->nGranted <= lock->nRequested);
		Assert((proclock->holdMask & ~lock->grantMask) == 0);

		/*
		 * Since there were no session locks, we should be releasing all locks
		 */
		if (proclock->releaseMask != proclock->holdMask)
			elog(PANIC, "we seem to have dropped a bit somewhere");

		holdMask = proclock->holdMask;

		/*
		 * We cannot simply modify proclock->tag.proc to reassign ownership of
		 * the lock, because that's part of the hash key and the proclock
		 * would then be in the wrong hash chain.  So, unlink and delete the
		 * old proclock; create a new one with the right contents; and link it
		 * into place.	We do it in this order to be certain we won't run out
		 * of shared memory (the way dynahash.c works, the deleted object is
		 * certain to be available for reallocation).
		 */
		SHMQueueDelete(&proclock->lockLink);
		SHMQueueDelete(&proclock->procLink);
		if (!hash_search(LockMethodProcLockHash[lockmethodid],
						 (void *) &(proclock->tag),
						 HASH_REMOVE, NULL))
			elog(PANIC, "proclock table corrupted");

		/*
		 * Create the hash key for the new proclock table.
		 */
		MemSet(&proclocktag, 0, sizeof(PROCLOCKTAG));
		proclocktag.lock = MAKE_OFFSET(lock);
		proclocktag.proc = MAKE_OFFSET(newproc);

		newproclock = (PROCLOCK *) hash_search(LockMethodProcLockHash[lockmethodid],
											   (void *) &proclocktag,
											   HASH_ENTER_NULL, &found);
		if (!newproclock)
			ereport(PANIC,		/* should not happen */
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
			SHMQueueInsertBefore(&newproc->procLocks, &newproclock->procLink);
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
	}

	LWLockRelease(masterLock);

	END_CRIT_SECTION();
}


/*
 * Estimate shared-memory space used for lock tables
 */
Size
LockShmemSize(void)
{
	Size		size;
	long		max_table_size = NLOCKENTS();

	/* lock method headers */
	size = MAX_LOCK_METHODS * MAXALIGN(sizeof(LockMethodData));

	/* lockHash table */
	size = add_size(size, hash_estimate_size(max_table_size, sizeof(LOCK)));

	/* proclockHash table */
	size = add_size(size, hash_estimate_size(max_table_size, sizeof(PROCLOCK)));

	/*
	 * Note we count only one pair of hash tables, since the userlocks table
	 * actually overlays the main one.
	 *
	 * Since the lockHash entry count above is only an estimate, add 10%
	 * safety margin.
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
 * The design goal is to hold the LockMgrLock for as short a time as possible;
 * thus, this function simply makes a copy of the necessary data and releases
 * the lock, allowing the caller to contemplate and format the data for as
 * long as it pleases.
 */
LockData *
GetLockStatusData(void)
{
	LockData   *data;
	HTAB	   *proclockTable;
	PROCLOCK   *proclock;
	HASH_SEQ_STATUS seqstat;
	int			i;

	data = (LockData *) palloc(sizeof(LockData));

	LWLockAcquire(LockMgrLock, LW_EXCLUSIVE);

	proclockTable = LockMethodProcLockHash[DEFAULT_LOCKMETHOD];

	data->nelements = i = proclockTable->hctl->nentries;

	data->proclockaddrs = (SHMEM_OFFSET *) palloc(sizeof(SHMEM_OFFSET) * i);
	data->proclocks = (PROCLOCK *) palloc(sizeof(PROCLOCK) * i);
	data->procs = (PGPROC *) palloc(sizeof(PGPROC) * i);
	data->locks = (LOCK *) palloc(sizeof(LOCK) * i);

	hash_seq_init(&seqstat, proclockTable);

	i = 0;
	while ((proclock = hash_seq_search(&seqstat)))
	{
		PGPROC	   *proc = (PGPROC *) MAKE_PTR(proclock->tag.proc);
		LOCK	   *lock = (LOCK *) MAKE_PTR(proclock->tag.lock);

		data->proclockaddrs[i] = MAKE_OFFSET(proclock);
		memcpy(&(data->proclocks[i]), proclock, sizeof(PROCLOCK));
		memcpy(&(data->procs[i]), proc, sizeof(PGPROC));
		memcpy(&(data->locks[i]), lock, sizeof(LOCK));

		i++;
	}

	LWLockRelease(LockMgrLock);

	Assert(i == data->nelements);

	return data;
}

/* Provide the textual name of any lock mode */
const char *
GetLockmodeName(LOCKMODE mode)
{
	Assert(mode <= MAX_LOCKMODES);
	return lock_mode_names[mode];
}

#ifdef LOCK_DEBUG
/*
 * Dump all locks in the given proc's procLocks list.
 *
 * Must have already acquired the masterLock.
 */
void
DumpLocks(PGPROC *proc)
{
	SHM_QUEUE  *procLocks;
	PROCLOCK   *proclock;
	LOCK	   *lock;
	int			lockmethodid = DEFAULT_LOCKMETHOD;
	LockMethod	lockMethodTable;

	if (proc == NULL)
		return;

	procLocks = &proc->procLocks;

	Assert(lockmethodid < NumLockMethods);
	lockMethodTable = LockMethods[lockmethodid];
	if (!lockMethodTable)
		return;

	if (proc->waitLock)
		LOCK_PRINT("DumpLocks: waiting on", proc->waitLock, 0);

	proclock = (PROCLOCK *) SHMQueueNext(procLocks, procLocks,
										 offsetof(PROCLOCK, procLink));

	while (proclock)
	{
		Assert(proclock->tag.proc == MAKE_OFFSET(proc));

		lock = (LOCK *) MAKE_PTR(proclock->tag.lock);

		PROCLOCK_PRINT("DumpLocks", proclock);
		LOCK_PRINT("DumpLocks", lock, 0);

		proclock = (PROCLOCK *) SHMQueueNext(procLocks, &proclock->procLink,
											 offsetof(PROCLOCK, procLink));
	}
}

/*
 * Dump all postgres locks. Must have already acquired the masterLock.
 */
void
DumpAllLocks(void)
{
	PGPROC	   *proc;
	PROCLOCK   *proclock;
	LOCK	   *lock;
	int			lockmethodid = DEFAULT_LOCKMETHOD;
	LockMethod	lockMethodTable;
	HTAB	   *proclockTable;
	HASH_SEQ_STATUS status;

	proc = MyProc;
	if (proc == NULL)
		return;

	Assert(lockmethodid < NumLockMethods);
	lockMethodTable = LockMethods[lockmethodid];
	if (!lockMethodTable)
		return;

	proclockTable = LockMethodProcLockHash[lockmethodid];

	if (proc->waitLock)
		LOCK_PRINT("DumpAllLocks: waiting on", proc->waitLock, 0);

	hash_seq_init(&status, proclockTable);
	while ((proclock = (PROCLOCK *) hash_seq_search(&status)) != NULL)
	{
		PROCLOCK_PRINT("DumpAllLocks", proclock);

		if (proclock->tag.lock)
		{
			lock = (LOCK *) MAKE_PTR(proclock->tag.lock);
			LOCK_PRINT("DumpAllLocks", lock, 0);
		}
		else
			elog(LOG, "DumpAllLocks: proclock->tag.lock = NULL");
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
	LWLockId	masterLock;
	LockMethod	lockMethodTable;

	Assert(len == sizeof(TwoPhaseLockRecord));
	locktag = &rec->locktag;
	lockmode = rec->lockmode;
	lockmethodid = locktag->locktag_lockmethodid;

	Assert(lockmethodid < NumLockMethods);
	lockMethodTable = LockMethods[lockmethodid];
	if (!lockMethodTable)
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);

	masterLock = lockMethodTable->masterLock;

	LWLockAcquire(masterLock, LW_EXCLUSIVE);

	/*
	 * Find or create a lock with this tag.
	 */
	lock = (LOCK *) hash_search(LockMethodLockHash[lockmethodid],
								(void *) locktag,
								HASH_ENTER_NULL, &found);
	if (!lock)
	{
		LWLockRelease(masterLock);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory"),
			errhint("You may need to increase max_locks_per_transaction.")));
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
	MemSet(&proclocktag, 0, sizeof(PROCLOCKTAG));		/* must clear padding */
	proclocktag.lock = MAKE_OFFSET(lock);
	proclocktag.proc = MAKE_OFFSET(proc);

	/*
	 * Find or create a proclock entry with this tag
	 */
	proclock = (PROCLOCK *) hash_search(LockMethodProcLockHash[lockmethodid],
										(void *) &proclocktag,
										HASH_ENTER_NULL, &found);
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
			if (!hash_search(LockMethodLockHash[lockmethodid],
							 (void *) &(lock->tag),
							 HASH_REMOVE, NULL))
				elog(PANIC, "lock table corrupted");
		}
		LWLockRelease(masterLock);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory"),
			errhint("You may need to increase max_locks_per_transaction.")));
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
		SHMQueueInsertBefore(&proc->procLocks, &proclock->procLink);
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
			 lock_mode_names[lockmode],
			 lock->tag.locktag_field1, lock->tag.locktag_field2,
			 lock->tag.locktag_field3);

	/*
	 * We ignore any possible conflicts and just grant ourselves the lock.
	 */
	GrantLock(lock, proclock, lockmode);

	LWLockRelease(masterLock);
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
	PROCLOCKTAG proclocktag;
	LOCK	   *lock;
	PROCLOCK   *proclock;
	LWLockId	masterLock;
	LockMethod	lockMethodTable;
	bool		wakeupNeeded;

	Assert(len == sizeof(TwoPhaseLockRecord));
	locktag = &rec->locktag;
	lockmode = rec->lockmode;
	lockmethodid = locktag->locktag_lockmethodid;

	Assert(lockmethodid < NumLockMethods);
	lockMethodTable = LockMethods[lockmethodid];
	if (!lockMethodTable)
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);

	masterLock = lockMethodTable->masterLock;

	LWLockAcquire(masterLock, LW_EXCLUSIVE);

	/*
	 * Re-find the lock object (it had better be there).
	 */
	lock = (LOCK *) hash_search(LockMethodLockHash[lockmethodid],
								(void *) locktag,
								HASH_FIND, NULL);
	if (!lock)
		elog(PANIC, "failed to re-find shared lock object");

	/*
	 * Re-find the proclock object (ditto).
	 */
	MemSet(&proclocktag, 0, sizeof(PROCLOCKTAG));		/* must clear padding */
	proclocktag.lock = MAKE_OFFSET(lock);
	proclocktag.proc = MAKE_OFFSET(proc);
	proclock = (PROCLOCK *) hash_search(LockMethodProcLockHash[lockmethodid],
										(void *) &proclocktag,
										HASH_FIND, NULL);
	if (!proclock)
		elog(PANIC, "failed to re-find shared proclock object");

	/*
	 * Double-check that we are actually holding a lock of the type we want to
	 * release.
	 */
	if (!(proclock->holdMask & LOCKBIT_ON(lockmode)))
	{
		PROCLOCK_PRINT("lock_twophase_postcommit: WRONGTYPE", proclock);
		LWLockRelease(masterLock);
		elog(WARNING, "you don't own a lock of type %s",
			 lock_mode_names[lockmode]);
		return;
	}

	/*
	 * Do the releasing.  CleanUpLock will waken any now-wakable waiters.
	 */
	wakeupNeeded = UnGrantLock(lock, lockmode, proclock, lockMethodTable);

	CleanUpLock(lockmethodid, lock, proclock, wakeupNeeded);

	LWLockRelease(masterLock);
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
