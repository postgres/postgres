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
 *	  $PostgreSQL: pgsql/src/backend/storage/lmgr/lock.c,v 1.149 2005/04/13 18:54:56 tgl Exp $
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

#include "access/xact.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/resowner.h"


/* This configuration variable is used to set the lock table size */
int			max_locks_per_xact; /* set by guc.c */

#define NLOCKENTS(maxBackends)	(max_locks_per_xact * (maxBackends))


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
		(((LOCK_LOCKMETHOD(*lock) == DEFAULT_LOCKMETHOD && Trace_locks)
	   || (LOCK_LOCKMETHOD(*lock) == USER_LOCKMETHOD && Trace_userlocks))
		 && (lock->tag.relId >= (Oid) Trace_lock_oidmin))
		|| (Trace_lock_table && (lock->tag.relId == Trace_lock_table));
}


inline static void
LOCK_PRINT(const char *where, const LOCK *lock, LOCKMODE type)
{
	if (LOCK_DEBUG_ENABLED(lock))
		elog(LOG,
			 "%s: lock(%lx) tbl(%d) rel(%u) db(%u) obj(%u) grantMask(%x) "
			 "req(%d,%d,%d,%d,%d,%d,%d)=%d "
			 "grant(%d,%d,%d,%d,%d,%d,%d)=%d wait(%d) type(%s)",
			 where, MAKE_OFFSET(lock),
			 lock->tag.lockmethodid, lock->tag.relId, lock->tag.dbId,
			 lock->tag.objId.blkno, lock->grantMask,
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
	if (
		(((PROCLOCK_LOCKMETHOD(*proclockP) == DEFAULT_LOCKMETHOD && Trace_locks)
		  || (PROCLOCK_LOCKMETHOD(*proclockP) == USER_LOCKMETHOD && Trace_userlocks))
		 && (((LOCK *) MAKE_PTR(proclockP->tag.lock))->tag.relId >= (Oid) Trace_lock_oidmin))
		|| (Trace_lock_table && (((LOCK *) MAKE_PTR(proclockP->tag.lock))->tag.relId == Trace_lock_table))
		)
		elog(LOG,
		"%s: proclock(%lx) lock(%lx) tbl(%d) proc(%lx) xid(%u) hold(%x)",
			 where, MAKE_OFFSET(proclockP), proclockP->tag.lock,
			 PROCLOCK_LOCKMETHOD(*(proclockP)),
			 proclockP->tag.proc, proclockP->tag.xid,
			 (int) proclockP->holdMask);
}

#else							/* not LOCK_DEBUG */

#define LOCK_PRINT(where, lock, type)
#define PROCLOCK_PRINT(where, proclockP)
#endif   /* not LOCK_DEBUG */


static void RemoveLocalLock(LOCALLOCK *locallock);
static void GrantLockLocal(LOCALLOCK *locallock, ResourceOwner owner);
static int WaitOnLock(LOCKMETHODID lockmethodid, LOCALLOCK *locallock,
		   ResourceOwner owner);
static void LockCountMyLocks(SHMEM_OFFSET lockOffset, PGPROC *proc,
				 int *myHolding);
static bool UnGrantLock(LOCK *lock, LOCKMODE lockmode,
						PROCLOCK *proclock, LockMethod lockMethodTable);


/*
 * InitLocks -- Init the lock module.  Create a private data
 *		structure for constructing conflict masks.
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
					int numModes,
					int maxBackends)
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
	max_table_size = NLOCKENTS(maxBackends);
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
	 * allocate a non-shared hash table for LOCALLOCK structs.	This is
	 * used to store lock counts and resource owner information.
	 *
	 * The non-shared table could already exist in this process (this occurs
	 * when the postmaster is recreating shared memory after a backend
	 * crash). If so, delete and recreate it.  (We could simply leave it,
	 * since it ought to be empty in the postmaster, but for safety let's
	 * zap it.)
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
 * NOTES: Both the lock module and the lock chain (lchain.c)
 *		module use table id's to distinguish between different
 *		kinds of locks.  Short term and long term locks look
 *		the same to the lock table, but are handled differently
 *		by the lock chain manager.	This function allows the
 *		client to use different lockmethods when acquiring/releasing
 *		short term and long term locks, yet store them all in one hashtable.
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
 * Returns: TRUE if lock was acquired, FALSE otherwise.  Note that
 *		a FALSE return is to be expected if dontWait is TRUE;
 *		but if dontWait is FALSE, only a parameter error can cause
 *		a FALSE return.  (XXX probably we should just ereport on parameter
 *		errors, instead of conflating this with failure to acquire lock?)
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
 *		User locks use lock tags made of an uint16 and an uint32, for
 *		example 0 and a tuple oid, or any other arbitrary pair of
 *		numbers following a convention established by the application.
 *		In this sense tags don't refer to tuples or database entities.
 *		User locks and normal locks are completely orthogonal and
 *		they don't interfere with each other, so it is possible
 *		to acquire a normal lock on an user-locked tuple or user-lock
 *		a tuple for which a normal write lock already exists.
 *		User locks are always non blocking, therefore they are never
 *		acquired if already held by another process.  They must be
 *		released explicitly by the application but they are released
 *		automatically when a backend terminates.
 *		They are indicated by a lockmethod 2 which is an alias for the
 *		normal lock table, and are distinguished from normal locks
 *		by the following differences:
 *
 *										normal lock		user lock
 *
 *		lockmethodid					1				2
 *		tag.dbId						database oid	database oid
 *		tag.relId						rel oid or 0	0
 *		tag.objId						block id		lock id2
 *										or xact id
 *		tag.offnum						0				lock id1
 *		proclock.xid					xid or 0		0
 *		persistence						transaction		user or backend
 *										or backend
 *
 *		The lockmode parameter can have the same values for normal locks
 *		although probably only WRITE_LOCK can have some practical use.
 *
 *														DZ - 22 Nov 1997
 */

bool
LockAcquire(LOCKMETHODID lockmethodid, LOCKTAG *locktag,
			TransactionId xid, LOCKMODE lockmode, bool dontWait)
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
	int			myHolding[MAX_LOCKMODES];
	int			i;

#ifdef LOCK_DEBUG
	if (lockmethodid == USER_LOCKMETHOD && Trace_userlocks)
		elog(LOG, "LockAcquire: user lock [%u] %s",
			 locktag->objId.blkno, lock_mode_names[lockmode]);
#endif

	/* ???????? This must be changed when short term locks will be used */
	locktag->lockmethodid = lockmethodid;

	Assert(lockmethodid < NumLockMethods);
	lockMethodTable = LockMethods[lockmethodid];
	if (!lockMethodTable)
	{
		elog(WARNING, "bad lock table id: %d", lockmethodid);
		return FALSE;
	}

	/* Session locks and user locks are not transactional */
	if (xid != InvalidTransactionId &&
		lockmethodid == DEFAULT_LOCKMETHOD)
		owner = CurrentResourceOwner;
	else
		owner = NULL;

	/*
	 * Find or create a LOCALLOCK entry for this lock and lockmode
	 */
	MemSet(&localtag, 0, sizeof(localtag));		/* must clear padding */
	localtag.lock = *locktag;
	localtag.xid = xid;
	localtag.mode = lockmode;

	locallock = (LOCALLOCK *) hash_search(LockMethodLocalHash[lockmethodid],
										  (void *) &localtag,
										  HASH_ENTER, &found);
	if (!locallock)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	/*
	 * if it's a new locallock object, initialize it
	 */
	if (!found)
	{
		locallock->lock = NULL;
		locallock->proclock = NULL;
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
	 * If we already hold the lock, we can just increase the count
	 * locally.
	 */
	if (locallock->nLocks > 0)
	{
		GrantLockLocal(locallock, owner);
		return TRUE;
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
								HASH_ENTER, &found);
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
		MemSet((char *) lock->requested, 0, sizeof(int) * MAX_LOCKMODES);
		MemSet((char *) lock->granted, 0, sizeof(int) * MAX_LOCKMODES);
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
	TransactionIdStore(xid, &proclocktag.xid);

	/*
	 * Find or create a proclock entry with this tag
	 */
	proclock = (PROCLOCK *) hash_search(LockMethodProcLockHash[lockmethodid],
										(void *) &proclocktag,
										HASH_ENTER, &found);
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
			lock = (LOCK *) hash_search(LockMethodLockHash[lockmethodid],
										(void *) &(lock->tag),
										HASH_REMOVE, NULL);
		}
		LWLockRelease(masterLock);
		if (!lock)				/* hash remove failed? */
			elog(WARNING, "lock table corrupted");
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
		 * Issue warning if we already hold a lower-level lock on this
		 * object and do not hold a lock of the requested level or higher.
		 * This indicates a deadlock-prone coding practice (eg, we'd have
		 * a deadlock if another backend were following the same code path
		 * at about the same time).
		 *
		 * This is not enabled by default, because it may generate log
		 * entries about user-level coding practices that are in fact safe
		 * in context. It can be enabled to help find system-level
		 * problems.
		 *
		 * XXX Doing numeric comparison on the lockmodes is a hack; it'd be
		 * better to use a table.  For now, though, this works.
		 */
		for (i = lockMethodTable->numLockModes; i > 0; i--)
		{
			if (proclock->holdMask & LOCKBIT_ON(i))
			{
				if (i >= (int) lockmode)
					break;		/* safe: we have a lock >= req level */
				elog(LOG, "deadlock risk: raising lock level"
					 " from %s to %s on object %u/%u/%u",
					 lock_mode_names[i], lock_mode_names[lockmode],
				 lock->tag.relId, lock->tag.dbId, lock->tag.objId.blkno);
				break;
			}
		}
#endif   /* CHECK_DEADLOCK_RISK */
	}

	/*
	 * lock->nRequested and lock->requested[] count the total number of
	 * requests, whether granted or waiting, so increment those
	 * immediately. The other counts don't increment till we get the lock.
	 */
	lock->nRequested++;
	lock->requested[lockmode]++;
	Assert((lock->nRequested > 0) && (lock->requested[lockmode] > 0));

	/*
	 * If this process (under any XID) is a holder of the lock, just grant
	 * myself another one without blocking.
	 */
	LockCountMyLocks(proclock->tag.lock, MyProc, myHolding);
	if (myHolding[lockmode] > 0)
	{
		GrantLock(lock, proclock, lockmode);
		GrantLockLocal(locallock, owner);
		PROCLOCK_PRINT("LockAcquire: my other XID owning", proclock);
		LWLockRelease(masterLock);
		return TRUE;
	}

	/*
	 * If lock requested conflicts with locks requested by waiters, must
	 * join wait queue.  Otherwise, check for conflict with already-held
	 * locks.  (That's last because most complex check.)
	 */
	if (lockMethodTable->conflictTab[lockmode] & lock->waitMask)
		status = STATUS_FOUND;
	else
		status = LockCheckConflicts(lockMethodTable, lockmode,
									lock, proclock,
									MyProc, myHolding);

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
		 * blocking, remove useless table entries and return FALSE without
		 * waiting.
		 */
		if (dontWait)
		{
			if (proclock->holdMask == 0)
			{
				SHMQueueDelete(&proclock->lockLink);
				SHMQueueDelete(&proclock->procLink);
				proclock = (PROCLOCK *) hash_search(LockMethodProcLockHash[lockmethodid],
											   (void *) &(proclock->tag),
													HASH_REMOVE, NULL);
				if (!proclock)
					elog(WARNING, "proclock table corrupted");
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
			return FALSE;
		}

		/*
		 * Construct bitmask of locks this process holds on this object.
		 */
		{
			LOCKMASK	heldLocks = 0;

			for (i = 1; i <= lockMethodTable->numLockModes; i++)
			{
				if (myHolding[i] > 0)
					heldLocks |= LOCKBIT_ON(i);
			}
			MyProc->heldLocks = heldLocks;
		}

		/*
		 * Sleep till someone wakes me up.
		 */
		status = WaitOnLock(lockmethodid, locallock, owner);

		/*
		 * NOTE: do not do any material change of state between here and
		 * return.	All required changes in locktable state must have been
		 * done when the lock was granted to us --- see notes in
		 * WaitOnLock.
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
			return FALSE;
		}
		PROCLOCK_PRINT("LockAcquire: granted", proclock);
		LOCK_PRINT("LockAcquire: granted", lock, lockmode);
	}

	LWLockRelease(masterLock);

	return status == STATUS_OK;
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
	locallock = (LOCALLOCK *) hash_search(LockMethodLocalHash[lockmethodid],
										  (void *) &(locallock->tag),
										  HASH_REMOVE, NULL);
	if (!locallock)
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
 * conflict with one another, even if they are held under different
 * transaction IDs (eg, session and xact locks do not conflict).
 * So, we must subtract off our own locks when determining whether the
 * requested new lock conflicts with those already held.
 *
 * The caller can optionally pass the process's total holding counts, if
 * known.  If NULL is passed then these values will be computed internally.
 */
int
LockCheckConflicts(LockMethod lockMethodTable,
				   LOCKMODE lockmode,
				   LOCK *lock,
				   PROCLOCK *proclock,
				   PGPROC *proc,
				   int *myHolding)		/* myHolding[] array or NULL */
{
	int			numLockModes = lockMethodTable->numLockModes;
	LOCKMASK	bitmask;
	int			i;
	int			localHolding[MAX_LOCKMODES];

	/*
	 * first check for global conflicts: If no locks conflict with my
	 * request, then I get the lock.
	 *
	 * Checking for conflict: lock->grantMask represents the types of
	 * currently held locks.  conflictTable[lockmode] has a bit set for
	 * each type of lock that conflicts with request.	Bitwise compare
	 * tells if there is a conflict.
	 */
	if (!(lockMethodTable->conflictTab[lockmode] & lock->grantMask))
	{
		PROCLOCK_PRINT("LockCheckConflicts: no conflict", proclock);
		return STATUS_OK;
	}

	/*
	 * Rats.  Something conflicts. But it could still be my own lock.  We
	 * have to construct a conflict mask that does not reflect our own
	 * locks.  Locks held by the current process under another XID also
	 * count as "our own locks".
	 */
	if (myHolding == NULL)
	{
		/* Caller didn't do calculation of total holding for me */
		LockCountMyLocks(proclock->tag.lock, proc, localHolding);
		myHolding = localHolding;
	}

	/* Compute mask of lock types held by other processes */
	bitmask = 0;
	for (i = 1; i <= numLockModes; i++)
	{
		if (lock->granted[i] != myHolding[i])
			bitmask |= LOCKBIT_ON(i);
	}

	/*
	 * now check again for conflicts.  'bitmask' describes the types of
	 * locks held by other processes.  If one of these conflicts with the
	 * kind of lock that I want, there is a conflict and I have to sleep.
	 */
	if (!(lockMethodTable->conflictTab[lockmode] & bitmask))
	{
		/* no conflict. OK to get the lock */
		PROCLOCK_PRINT("LockCheckConflicts: resolved", proclock);
		return STATUS_OK;
	}

	PROCLOCK_PRINT("LockCheckConflicts: conflicting", proclock);
	return STATUS_FOUND;
}

/*
 * LockCountMyLocks --- Count total number of locks held on a given lockable
 *		object by a given process (under any transaction ID).
 *
 * XXX This could be rather slow if the process holds a large number of locks.
 * Perhaps it could be sped up if we kept yet a third hashtable of per-
 * process lock information.  However, for the normal case where a transaction
 * doesn't hold a large number of locks, keeping such a table would probably
 * be a net slowdown.
 */
static void
LockCountMyLocks(SHMEM_OFFSET lockOffset, PGPROC *proc, int *myHolding)
{
	SHM_QUEUE  *procLocks = &(proc->procLocks);
	PROCLOCK   *proclock;

	MemSet(myHolding, 0, MAX_LOCKMODES * sizeof(int));

	proclock = (PROCLOCK *) SHMQueueNext(procLocks, procLocks,
										 offsetof(PROCLOCK, procLink));

	while (proclock)
	{
		if (lockOffset == proclock->tag.lock)
		{
			LOCKMASK	holdMask = proclock->holdMask;
			int			i;

			for (i = 1; i < MAX_LOCKMODES; i++)
			{
				if (holdMask & LOCKBIT_ON(i))
					myHolding[i]++;
			}
		}

		proclock = (PROCLOCK *) SHMQueueNext(procLocks, &proclock->procLink,
										   offsetof(PROCLOCK, procLink));
	}
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
	bool wakeupNeeded = false;

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
	 * We need only run ProcLockWakeup if the released lock conflicts with
	 * at least one of the lock types requested by waiter(s).  Otherwise
	 * whatever conflict made them wait must still exist.  NOTE: before
	 * MVCC, we could skip wakeup if lock->granted[lockmode] was still
	 * positive. But that's not true anymore, because the remaining
	 * granted locks might belong to some waiter, who could now be
	 * awakened because he doesn't conflict with his own locks.
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
 * on the lockable object by this process (under all XIDs).
 *
 * The locktable's masterLock must be held at entry.
 */
static int
WaitOnLock(LOCKMETHODID lockmethodid, LOCALLOCK *locallock,
		   ResourceOwner owner)
{
	LockMethod	lockMethodTable = LockMethods[lockmethodid];
	char	   *new_status,
			   *old_status;
	size_t		len;

	Assert(lockmethodid < NumLockMethods);

	LOCK_PRINT("WaitOnLock: sleeping on lock",
			   locallock->lock, locallock->tag.mode);

	old_status = pstrdup(get_ps_display());
	len = strlen(old_status);
	new_status = (char *) palloc(len + 8 + 1);
	memcpy(new_status, old_status, len);
	strcpy(new_status + len, " waiting");
	set_ps_display(new_status);

	awaitedLock = locallock;
	awaitedOwner = owner;

	/*
	 * NOTE: Think not to put any shared-state cleanup after the call to
	 * ProcSleep, in either the normal or failure path.  The lock state
	 * must be fully set by the lock grantor, or by CheckDeadLock if we
	 * give up waiting for the lock.  This is necessary because of the
	 * possibility that a cancel/die interrupt will interrupt ProcSleep
	 * after someone else grants us the lock, but before we've noticed it.
	 * Hence, after granting, the locktable state must fully reflect the
	 * fact that we own the lock; we can't do additional work on return.
	 * Contrariwise, if we fail, any cleanup must happen in xact abort
	 * processing, not here, to ensure it will also happen in the
	 * cancel/die case.
	 */

	if (ProcSleep(lockMethodTable,
				  locallock->tag.mode,
				  locallock->lock,
				  locallock->proclock) != STATUS_OK)
	{
		/*
		 * We failed as a result of a deadlock, see CheckDeadLock(). Quit
		 * now.
		 */
		awaitedLock = NULL;
		LOCK_PRINT("WaitOnLock: aborting on lock",
				   locallock->lock, locallock->tag.mode);
		LWLockRelease(lockMethodTable->masterLock);

		/*
		 * Now that we aren't holding the LockMgrLock, we can give an
		 * error report including details about the detected deadlock.
		 */
		DeadLockReport();
		/* not reached */
	}

	awaitedLock = NULL;

	set_ps_display(old_status);
	pfree(old_status);
	pfree(new_status);

	LOCK_PRINT("WaitOnLock: wakeup on lock",
			   locallock->lock, locallock->tag.mode);
	return STATUS_OK;
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
	 * This must happen now because if the owner of the lock decides to release
	 * it, and the requested/granted counts then go to zero, LockRelease
	 * expects there to be no remaining proclocks.
	 */
	if (proclock->holdMask == 0)
	{
		PROCLOCK_PRINT("RemoveFromWaitQueue: deleting proclock", proclock);
		SHMQueueDelete(&proclock->lockLink);
		SHMQueueDelete(&proclock->procLink);
		proclock = (PROCLOCK *) hash_search(LockMethodProcLockHash[lockmethodid],
											(void *) &(proclock->tag),
											HASH_REMOVE, NULL);
		if (!proclock)
			elog(WARNING, "proclock table corrupted");
	}

	/*
	 * There should still be some requests for the lock ... else what were
	 * we waiting for?  Therefore no need to delete the lock object.
	 */
	Assert(waitLock->nRequested > 0);

	/* See if any other waiters for the lock can be woken up now */
	ProcLockWakeup(LockMethods[lockmethodid], waitLock);
}

/*
 * LockRelease -- look up 'locktag' in lock table 'lockmethodid' and
 *		release one 'lockmode' lock on it.
 *
 * Side Effects: find any waiting processes that are now wakable,
 *		grant them their requested locks and awaken them.
 *		(We have to grant the lock here to avoid a race between
 *		the waking process and any new process to
 *		come along and request the lock.)
 */
bool
LockRelease(LOCKMETHODID lockmethodid, LOCKTAG *locktag,
			TransactionId xid, LOCKMODE lockmode)
{
	LOCALLOCKTAG localtag;
	LOCALLOCK  *locallock;
	LOCK	   *lock;
	PROCLOCK   *proclock;
	LWLockId	masterLock;
	LockMethod	lockMethodTable;
	bool		wakeupNeeded;

#ifdef LOCK_DEBUG
	if (lockmethodid == USER_LOCKMETHOD && Trace_userlocks)
		elog(LOG, "LockRelease: user lock tag [%u] %d", locktag->objId.blkno, lockmode);
#endif

	/* ???????? This must be changed when short term locks will be used */
	locktag->lockmethodid = lockmethodid;

	Assert(lockmethodid < NumLockMethods);
	lockMethodTable = LockMethods[lockmethodid];
	if (!lockMethodTable)
	{
		elog(WARNING, "lockMethodTable is null in LockRelease");
		return FALSE;
	}

	/*
	 * Find the LOCALLOCK entry for this lock and lockmode
	 */
	MemSet(&localtag, 0, sizeof(localtag));		/* must clear padding */
	localtag.lock = *locktag;
	localtag.xid = xid;
	localtag.mode = lockmode;

	locallock = (LOCALLOCK *) hash_search(LockMethodLocalHash[lockmethodid],
										  (void *) &localtag,
										  HASH_FIND, NULL);

	/*
	 * let the caller print its own error message, too. Do not
	 * ereport(ERROR).
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
		if (xid != InvalidTransactionId &&
			lockmethodid == DEFAULT_LOCKMETHOD)
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
	 * Decrease the total local count.	If we're still holding the lock,
	 * we're done.
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
	 * addresses in the locallock table, and they couldn't have been
	 * removed while we were holding a lock on them.
	 */
	lock = locallock->lock;
	LOCK_PRINT("LockRelease: found", lock, lockmode);
	proclock = locallock->proclock;
	PROCLOCK_PRINT("LockRelease: found", proclock);

	/*
	 * Double-check that we are actually holding a lock of the type we
	 * want to release.
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

	wakeupNeeded = UnGrantLock(lock, lockmode, proclock, lockMethodTable);

	/*
	 * If this was my last hold on this lock, delete my entry in the
	 * proclock table.
	 */
	if (proclock->holdMask == 0)
	{
		PROCLOCK_PRINT("LockRelease: deleting proclock", proclock);
		SHMQueueDelete(&proclock->lockLink);
		SHMQueueDelete(&proclock->procLink);
		proclock = (PROCLOCK *) hash_search(LockMethodProcLockHash[lockmethodid],
											(void *) &(proclock->tag),
											HASH_REMOVE, NULL);
		if (!proclock)
		{
			LWLockRelease(masterLock);
			elog(WARNING, "proclock table corrupted");
			RemoveLocalLock(locallock);
			return FALSE;
		}
	}

	if (lock->nRequested == 0)
	{
		/*
		 * We've just released the last lock, so garbage-collect the lock
		 * object.
		 */
		LOCK_PRINT("LockRelease: deleting lock", lock, lockmode);
		Assert(SHMQueueEmpty(&(lock->procLocks)));
		lock = (LOCK *) hash_search(LockMethodLockHash[lockmethodid],
									(void *) &(lock->tag),
									HASH_REMOVE, NULL);
		if (!lock)
		{
			LWLockRelease(masterLock);
			elog(WARNING, "lock table corrupted");
			RemoveLocalLock(locallock);
			return FALSE;
		}
	}
	else
	{
		/*
		 * Wake up waiters if needed.
		 */
		if (wakeupNeeded)
			ProcLockWakeup(lockMethodTable, lock);
	}

	LWLockRelease(masterLock);

	RemoveLocalLock(locallock);
	return TRUE;
}

/*
 * LockReleaseAll -- Release all locks of the specified lock method that
 *		are held by the current process.
 *
 * Well, not necessarily *all* locks.  The available behaviors are:
 *
 * allxids == true: release all locks regardless of transaction
 * affiliation.
 *
 * allxids == false: release all locks with Xid != 0
 * (zero is the Xid used for "session" locks).
 */
bool
LockReleaseAll(LOCKMETHODID lockmethodid, bool allxids)
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
	{
		elog(WARNING, "bad lock method: %d", lockmethodid);
		return FALSE;
	}

	numLockModes = lockMethodTable->numLockModes;
	masterLock = lockMethodTable->masterLock;

	/*
	 * First we run through the locallock table and get rid of unwanted
	 * entries, then we scan the process's proclocks and get rid of those.
	 * We do this separately because we may have multiple locallock
	 * entries pointing to the same proclock, and we daren't end up with
	 * any dangling pointers.
	 */
	hash_seq_init(&status, LockMethodLocalHash[lockmethodid]);

	while ((locallock = (LOCALLOCK *) hash_seq_search(&status)) != NULL)
	{
		if (locallock->proclock == NULL || locallock->lock == NULL)
		{
			/*
			 * We must've run out of shared memory while trying to set up
			 * this lock.  Just forget the local entry.
			 */
			Assert(locallock->nLocks == 0);
			RemoveLocalLock(locallock);
			continue;
		}

		/* Ignore items that are not of the lockmethod to be removed */
		if (LOCALLOCK_LOCKMETHOD(*locallock) != lockmethodid)
			continue;

		/*
		 * Ignore locks with Xid=0 unless we are asked to release all
		 * locks
		 */
		if (TransactionIdEquals(locallock->tag.xid, InvalidTransactionId)
			&& !allxids)
			continue;

		RemoveLocalLock(locallock);
	}

	LWLockAcquire(masterLock, LW_EXCLUSIVE);

	proclock = (PROCLOCK *) SHMQueueNext(procLocks, procLocks,
										 offsetof(PROCLOCK, procLink));

	while (proclock)
	{
		bool		wakeupNeeded = false;
		PROCLOCK   *nextHolder;

		/* Get link first, since we may unlink/delete this proclock */
		nextHolder = (PROCLOCK *) SHMQueueNext(procLocks, &proclock->procLink,
										   offsetof(PROCLOCK, procLink));

		Assert(proclock->tag.proc == MAKE_OFFSET(MyProc));

		lock = (LOCK *) MAKE_PTR(proclock->tag.lock);

		/* Ignore items that are not of the lockmethod to be removed */
		if (LOCK_LOCKMETHOD(*lock) != lockmethodid)
			goto next_item;

		/*
		 * Ignore locks with Xid=0 unless we are asked to release all
		 * locks
		 */
		if (TransactionIdEquals(proclock->tag.xid, InvalidTransactionId)
			&& !allxids)
			goto next_item;

		PROCLOCK_PRINT("LockReleaseAll", proclock);
		LOCK_PRINT("LockReleaseAll", lock, 0);
		Assert(lock->nRequested >= 0);
		Assert(lock->nGranted >= 0);
		Assert(lock->nGranted <= lock->nRequested);
		Assert((proclock->holdMask & ~lock->grantMask) == 0);

		/*
		 * fix the general lock stats
		 */
		if (proclock->holdMask)
		{
			for (i = 1; i <= numLockModes; i++)
			{
				if (proclock->holdMask & LOCKBIT_ON(i))
					wakeupNeeded |= UnGrantLock(lock, i, proclock,
												lockMethodTable);
			}
		}
		Assert((lock->nRequested >= 0) && (lock->nGranted >= 0));
		Assert(lock->nGranted <= lock->nRequested);
		LOCK_PRINT("LockReleaseAll: updated", lock, 0);

		PROCLOCK_PRINT("LockReleaseAll: deleting", proclock);

		/*
		 * Remove the proclock entry from the linked lists
		 */
		SHMQueueDelete(&proclock->lockLink);
		SHMQueueDelete(&proclock->procLink);

		/*
		 * remove the proclock entry from the hashtable
		 */
		proclock = (PROCLOCK *) hash_search(LockMethodProcLockHash[lockmethodid],
											(void *) &(proclock->tag),
											HASH_REMOVE,
											NULL);
		if (!proclock)
		{
			LWLockRelease(masterLock);
			elog(WARNING, "proclock table corrupted");
			return FALSE;
		}

		if (lock->nRequested == 0)
		{
			/*
			 * We've just released the last lock, so garbage-collect the
			 * lock object.
			 */
			LOCK_PRINT("LockReleaseAll: deleting", lock, 0);
			Assert(SHMQueueEmpty(&(lock->procLocks)));
			lock = (LOCK *) hash_search(LockMethodLockHash[lockmethodid],
										(void *) &(lock->tag),
										HASH_REMOVE, NULL);
			if (!lock)
			{
				LWLockRelease(masterLock);
				elog(WARNING, "lock table corrupted");
				return FALSE;
			}
		}
		else if (wakeupNeeded)
			ProcLockWakeup(lockMethodTable, lock);

next_item:
		proclock = nextHolder;
	}

	LWLockRelease(masterLock);

#ifdef LOCK_DEBUG
	if (lockmethodid == USER_LOCKMETHOD ? Trace_userlocks : Trace_locks)
		elog(LOG, "LockReleaseAll done");
#endif

	return TRUE;
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
		if (TransactionIdEquals(locallock->tag.xid, InvalidTransactionId))
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
									 locallock->tag.xid,
									 locallock->tag.mode))
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
		if (TransactionIdEquals(locallock->tag.xid, InvalidTransactionId))
			continue;

		/*
		 * Scan to see if there are any locks belonging to current owner
		 * or its parent
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
 * Estimate shared-memory space used for lock tables
 */
int
LockShmemSize(int maxBackends)
{
	int			size = 0;
	long		max_table_size = NLOCKENTS(maxBackends);

	/* lock method headers */
	size += MAX_LOCK_METHODS * MAXALIGN(sizeof(LockMethodData));

	/* lockHash table */
	size += hash_estimate_size(max_table_size, sizeof(LOCK));

	/* proclockHash table */
	size += hash_estimate_size(max_table_size, sizeof(PROCLOCK));

	/*
	 * Note we count only one pair of hash tables, since the userlocks
	 * table actually overlays the main one.
	 *
	 * Since the lockHash entry count above is only an estimate, add 10%
	 * safety margin.
	 */
	size += size / 10;

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
 * Dump all locks in the MyProc->procLocks list.
 *
 * Must have already acquired the masterLock.
 */
void
DumpLocks(void)
{
	PGPROC	   *proc;
	SHM_QUEUE  *procLocks;
	PROCLOCK   *proclock;
	LOCK	   *lock;
	int			lockmethodid = DEFAULT_LOCKMETHOD;
	LockMethod	lockMethodTable;

	proc = MyProc;
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
