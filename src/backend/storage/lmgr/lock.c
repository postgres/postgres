/*-------------------------------------------------------------------------
 *
 * lock.c
 *	  POSTGRES low-level lock mechanism
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/lmgr/lock.c,v 1.128.2.1 2005/03/01 21:15:26 tgl Exp $
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
 *	LockMethodTableRename(), LockReleaseAll,
 *	LockCheckConflicts(), GrantLock()
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <signal.h>

#include "access/xact.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"


/* This configuration variable is used to set the lock table size */
int			max_locks_per_xact; /* set by guc.c */

#define NLOCKENTS(maxBackends)	(max_locks_per_xact * (maxBackends))


static int WaitOnLock(LOCKMETHOD lockmethod, LOCKMODE lockmode,
		   LOCK *lock, PROCLOCK *proclock);
static void LockCountMyLocks(SHMEM_OFFSET lockOffset, PGPROC *proc,
				 int *myHolding);

static char *lock_mode_names[] =
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

int			Trace_lock_oidmin = BootstrapObjectIdData;
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
			 lock->tag.lockmethod, lock->tag.relId, lock->tag.dbId,
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
			 "%s: proclock(%lx) lock(%lx) tbl(%d) proc(%lx) xid(%u) hold(%d,%d,%d,%d,%d,%d,%d)=%d",
			 where, MAKE_OFFSET(proclockP), proclockP->tag.lock,
			 PROCLOCK_LOCKMETHOD(*(proclockP)),
			 proclockP->tag.proc, proclockP->tag.xid,
			 proclockP->holding[1], proclockP->holding[2], proclockP->holding[3],
			 proclockP->holding[4], proclockP->holding[5], proclockP->holding[6],
			 proclockP->holding[7], proclockP->nHolding);
}

#else							/* not LOCK_DEBUG */

#define LOCK_PRINT(where, lock, type)
#define PROCLOCK_PRINT(where, proclockP)
#endif   /* not LOCK_DEBUG */


/*
 * These are to simplify/speed up some bit arithmetic.
 *
 * XXX is a fetch from a static array really faster than a shift?
 * Wouldn't bet on it...
 */

static LOCKMASK BITS_OFF[MAX_LOCKMODES];
static LOCKMASK BITS_ON[MAX_LOCKMODES];

/*
 * map from lockmethod to the lock table structure
 */
static LOCKMETHODTABLE *LockMethodTable[MAX_LOCK_METHODS];

static int	NumLockMethods;

/*
 * InitLocks -- Init the lock module.  Create a private data
 *		structure for constructing conflict masks.
 */
void
InitLocks(void)
{
	int			i;
	int			bit;

	bit = 1;
	for (i = 0; i < MAX_LOCKMODES; i++, bit <<= 1)
	{
		BITS_ON[i] = bit;
		BITS_OFF[i] = ~bit;
	}
}


/*
 * Fetch the lock method table associated with a given lock
 */
LOCKMETHODTABLE *
GetLocksMethodTable(LOCK *lock)
{
	LOCKMETHOD	lockmethod = LOCK_LOCKMETHOD(*lock);

	Assert(lockmethod > 0 && lockmethod < NumLockMethods);
	return LockMethodTable[lockmethod];
}


/*
 * LockMethodInit -- initialize the lock table's lock type
 *		structures
 *
 * Notes: just copying.  Should only be called once.
 */
static void
LockMethodInit(LOCKMETHODTABLE *lockMethodTable,
			   LOCKMASK *conflictsP,
			   int numModes)
{
	int			i;

	lockMethodTable->numLockModes = numModes;
	/* copies useless zero element as well as the N lockmodes */
	for (i = 0; i <= numModes; i++, conflictsP++)
		lockMethodTable->conflictTab[i] = *conflictsP;
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
LOCKMETHOD
LockMethodTableInit(char *tabName,
					LOCKMASK *conflictsP,
					int numModes,
					int maxBackends)
{
	LOCKMETHODTABLE *lockMethodTable;
	char	   *shmemName;
	HASHCTL		info;
	int			hash_flags;
	bool		found;
	long		init_table_size,
				max_table_size;

	if (numModes >= MAX_LOCKMODES)
		elog(ERROR, "too many lock types %d (limit is %d)",
			 numModes, MAX_LOCKMODES-1);

	/* Compute init/max size to request for lock hashtables */
	max_table_size = NLOCKENTS(maxBackends);
	init_table_size = max_table_size / 10;

	/* Allocate a string for the shmem index table lookups. */
	/* This is just temp space in this routine, so palloc is OK. */
	shmemName = (char *) palloc(strlen(tabName) + 32);

	/* each lock table has a header in shared memory */
	sprintf(shmemName, "%s (lock method table)", tabName);
	lockMethodTable = (LOCKMETHODTABLE *)
		ShmemInitStruct(shmemName, sizeof(LOCKMETHODTABLE), &found);

	if (!lockMethodTable)
		elog(FATAL, "could not initialize lock table \"%s\"", tabName);

	/*
	 * Lock the LWLock for the table (probably not necessary here)
	 */
	LWLockAcquire(LockMgrLock, LW_EXCLUSIVE);

	/*
	 * no zero-th table
	 */
	NumLockMethods = 1;

	/*
	 * we're first - initialize
	 */
	if (!found)
	{
		MemSet(lockMethodTable, 0, sizeof(LOCKMETHODTABLE));
		lockMethodTable->masterLock = LockMgrLock;
		lockMethodTable->lockmethod = NumLockMethods;
	}

	/*
	 * other modules refer to the lock table by a lockmethod ID
	 */
	LockMethodTable[NumLockMethods] = lockMethodTable;
	NumLockMethods++;
	Assert(NumLockMethods <= MAX_LOCK_METHODS);

	/*
	 * allocate a hash table for LOCK structs.	This is used to store
	 * per-locked-object information.
	 */
	info.keysize = sizeof(LOCKTAG);
	info.entrysize = sizeof(LOCK);
	info.hash = tag_hash;
	hash_flags = (HASH_ELEM | HASH_FUNCTION);

	sprintf(shmemName, "%s (lock hash)", tabName);
	lockMethodTable->lockHash = ShmemInitHash(shmemName,
											  init_table_size,
											  max_table_size,
											  &info,
											  hash_flags);

	if (!lockMethodTable->lockHash)
		elog(FATAL, "could not initialize lock table \"%s\"", tabName);
	Assert(lockMethodTable->lockHash->hash == tag_hash);

	/*
	 * allocate a hash table for PROCLOCK structs.	This is used to store
	 * per-lock-proclock information.
	 */
	info.keysize = sizeof(PROCLOCKTAG);
	info.entrysize = sizeof(PROCLOCK);
	info.hash = tag_hash;
	hash_flags = (HASH_ELEM | HASH_FUNCTION);

	sprintf(shmemName, "%s (proclock hash)", tabName);
	lockMethodTable->proclockHash = ShmemInitHash(shmemName,
												  init_table_size,
												  max_table_size,
												  &info,
												  hash_flags);

	if (!lockMethodTable->proclockHash)
		elog(FATAL, "could not initialize lock table \"%s\"", tabName);

	/* init data structures */
	LockMethodInit(lockMethodTable, conflictsP, numModes);

	LWLockRelease(LockMgrLock);

	pfree(shmemName);

	return lockMethodTable->lockmethod;
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

LOCKMETHOD
LockMethodTableRename(LOCKMETHOD lockmethod)
{
	LOCKMETHOD	newLockMethod;

	if (NumLockMethods >= MAX_LOCK_METHODS)
		return INVALID_LOCKMETHOD;
	if (LockMethodTable[lockmethod] == INVALID_LOCKMETHOD)
		return INVALID_LOCKMETHOD;

	/* other modules refer to the lock table by a lockmethod ID */
	newLockMethod = NumLockMethods;
	NumLockMethods++;

	LockMethodTable[newLockMethod] = LockMethodTable[lockmethod];
	return newLockMethod;
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
 *		lockmethod						1				2
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
LockAcquire(LOCKMETHOD lockmethod, LOCKTAG *locktag,
			TransactionId xid, LOCKMODE lockmode, bool dontWait)
{
	PROCLOCK   *proclock;
	PROCLOCKTAG proclocktag;
	HTAB	   *proclockTable;
	bool		found;
	LOCK	   *lock;
	LWLockId	masterLock;
	LOCKMETHODTABLE *lockMethodTable;
	int			status;
	int			myHolding[MAX_LOCKMODES];
	int			i;

#ifdef LOCK_DEBUG
	if (lockmethod == USER_LOCKMETHOD && Trace_userlocks)
		elog(LOG, "LockAcquire: user lock [%u] %s",
			 locktag->objId.blkno, lock_mode_names[lockmode]);
#endif

	/* ???????? This must be changed when short term locks will be used */
	locktag->lockmethod = lockmethod;

	Assert(lockmethod < NumLockMethods);
	lockMethodTable = LockMethodTable[lockmethod];
	if (!lockMethodTable)
	{
		elog(WARNING, "bad lock table id: %d", lockmethod);
		return FALSE;
	}

	masterLock = lockMethodTable->masterLock;

	LWLockAcquire(masterLock, LW_EXCLUSIVE);

	/*
	 * Find or create a lock with this tag
	 */
	Assert(lockMethodTable->lockHash->hash == tag_hash);
	lock = (LOCK *) hash_search(lockMethodTable->lockHash,
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

	/*
	 * if it's a new lock object, initialize it
	 */
	if (!found)
	{
		lock->grantMask = 0;
		lock->waitMask = 0;
		SHMQueueInit(&(lock->lockHolders));
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
	MemSet(&proclocktag, 0, sizeof(PROCLOCKTAG));		/* must clear padding,
														 * needed */
	proclocktag.lock = MAKE_OFFSET(lock);
	proclocktag.proc = MAKE_OFFSET(MyProc);
	TransactionIdStore(xid, &proclocktag.xid);

	/*
	 * Find or create a proclock entry with this tag
	 */
	proclockTable = lockMethodTable->proclockHash;
	proclock = (PROCLOCK *) hash_search(proclockTable,
										(void *) &proclocktag,
										HASH_ENTER, &found);
	if (!proclock)
	{
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
		proclock->nHolding = 0;
		MemSet((char *) proclock->holding, 0, sizeof(int) * MAX_LOCKMODES);
		/* Add proclock to appropriate lists */
		SHMQueueInsertBefore(&lock->lockHolders, &proclock->lockLink);
		SHMQueueInsertBefore(&MyProc->procHolders, &proclock->procLink);
		PROCLOCK_PRINT("LockAcquire: new", proclock);
	}
	else
	{
		PROCLOCK_PRINT("LockAcquire: found", proclock);
		Assert((proclock->nHolding >= 0) && (proclock->holding[lockmode] >= 0));
		Assert(proclock->nHolding <= lock->nGranted);

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
			if (proclock->holding[i] > 0)
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
	 * If I already hold one or more locks of the requested type, just
	 * grant myself another one without blocking.
	 */
	if (proclock->holding[lockmode] > 0)
	{
		GrantLock(lock, proclock, lockmode);
		PROCLOCK_PRINT("LockAcquire: owning", proclock);
		LWLockRelease(masterLock);
		return TRUE;
	}

	/*
	 * If this process (under any XID) is a proclock of the lock, also
	 * grant myself another one without blocking.
	 */
	LockCountMyLocks(proclock->tag.lock, MyProc, myHolding);
	if (myHolding[lockmode] > 0)
	{
		GrantLock(lock, proclock, lockmode);
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
	}
	else
	{
		Assert(status == STATUS_FOUND);

		/*
		 * We can't acquire the lock immediately.  If caller specified no
		 * blocking, remove the proclock entry and return FALSE without
		 * waiting.
		 */
		if (dontWait)
		{
			if (proclock->nHolding == 0)
			{
				SHMQueueDelete(&proclock->lockLink);
				SHMQueueDelete(&proclock->procLink);
				proclock = (PROCLOCK *) hash_search(proclockTable,
													(void *) proclock,
													HASH_REMOVE, NULL);
				if (!proclock)
					elog(WARNING, "proclock table corrupted");
			}
			else
				PROCLOCK_PRINT("LockAcquire: NHOLDING", proclock);
			lock->nRequested--;
			lock->requested[lockmode]--;
			LOCK_PRINT("LockAcquire: conditional lock failed", lock, lockmode);
			Assert((lock->nRequested > 0) && (lock->requested[lockmode] >= 0));
			Assert(lock->nGranted <= lock->nRequested);
			LWLockRelease(masterLock);
			return FALSE;
		}

		/*
		 * Construct bitmask of locks this process holds on this object.
		 */
		{
			int			heldLocks = 0;
			int			tmpMask;

			for (i = 1, tmpMask = 2;
				 i <= lockMethodTable->numLockModes;
				 i++, tmpMask <<= 1)
			{
				if (myHolding[i] > 0)
					heldLocks |= tmpMask;
			}
			MyProc->heldLocks = heldLocks;
		}

		/*
		 * Sleep till someone wakes me up.
		 */
		status = WaitOnLock(lockmethod, lockmode, lock, proclock);

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
		if (!((proclock->nHolding > 0) && (proclock->holding[lockmode] > 0)))
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
LockCheckConflicts(LOCKMETHODTABLE *lockMethodTable,
				   LOCKMODE lockmode,
				   LOCK *lock,
				   PROCLOCK *proclock,
				   PGPROC *proc,
				   int *myHolding)		/* myHolding[] array or NULL */
{
	int			numLockModes = lockMethodTable->numLockModes;
	int			bitmask;
	int			i,
				tmpMask;
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
	tmpMask = 2;
	for (i = 1; i <= numLockModes; i++, tmpMask <<= 1)
	{
		if (lock->granted[i] != myHolding[i])
			bitmask |= tmpMask;
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
	SHM_QUEUE  *procHolders = &(proc->procHolders);
	PROCLOCK   *proclock;
	int			i;

	MemSet(myHolding, 0, MAX_LOCKMODES * sizeof(int));

	proclock = (PROCLOCK *) SHMQueueNext(procHolders, procHolders,
										 offsetof(PROCLOCK, procLink));

	while (proclock)
	{
		if (lockOffset == proclock->tag.lock)
		{
			for (i = 1; i < MAX_LOCKMODES; i++)
				myHolding[i] += proclock->holding[i];
		}

		proclock = (PROCLOCK *) SHMQueueNext(procHolders, &proclock->procLink,
										   offsetof(PROCLOCK, procLink));
	}
}

/*
 * GrantLock -- update the lock and proclock data structures to show
 *		the lock request has been granted.
 *
 * NOTE: if proc was blocked, it also needs to be removed from the wait list
 * and have its waitLock/waitHolder fields cleared.  That's not done here.
 */
void
GrantLock(LOCK *lock, PROCLOCK *proclock, LOCKMODE lockmode)
{
	lock->nGranted++;
	lock->granted[lockmode]++;
	lock->grantMask |= BITS_ON[lockmode];
	if (lock->granted[lockmode] == lock->requested[lockmode])
		lock->waitMask &= BITS_OFF[lockmode];
	LOCK_PRINT("GrantLock", lock, lockmode);
	Assert((lock->nGranted > 0) && (lock->granted[lockmode] > 0));
	Assert(lock->nGranted <= lock->nRequested);
	proclock->holding[lockmode]++;
	proclock->nHolding++;
	Assert((proclock->nHolding > 0) && (proclock->holding[lockmode] > 0));
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
WaitOnLock(LOCKMETHOD lockmethod, LOCKMODE lockmode,
		   LOCK *lock, PROCLOCK *proclock)
{
	LOCKMETHODTABLE *lockMethodTable = LockMethodTable[lockmethod];
	char	   *new_status,
			   *old_status;

	Assert(lockmethod < NumLockMethods);

	LOCK_PRINT("WaitOnLock: sleeping on lock", lock, lockmode);

	old_status = pstrdup(get_ps_display());
	new_status = (char *) palloc(strlen(old_status) + 10);
	strcpy(new_status, old_status);
	strcat(new_status, " waiting");
	set_ps_display(new_status);

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
				  lockmode,
				  lock,
				  proclock) != STATUS_OK)
	{
		/*
		 * We failed as a result of a deadlock, see CheckDeadLock(). Quit
		 * now.
		 */
		LOCK_PRINT("WaitOnLock: aborting on lock", lock, lockmode);
		LWLockRelease(lockMethodTable->masterLock);

		/*
		 * Now that we aren't holding the LockMgrLock, we can give an
		 * error report including details about the detected deadlock.
		 */
		DeadLockReport();
		/* not reached */
	}

	set_ps_display(old_status);
	pfree(old_status);
	pfree(new_status);

	LOCK_PRINT("WaitOnLock: wakeup on lock", lock, lockmode);
	return STATUS_OK;
}

/*
 * Remove a proc from the wait-queue it is on
 * (caller must know it is on one).
 *
 * Locktable lock must be held by caller.
 */
void
RemoveFromWaitQueue(PGPROC *proc)
{
	LOCK	   *waitLock = proc->waitLock;
	PROCLOCK   *proclock = proc->waitHolder;
	LOCKMODE	lockmode = proc->waitLockMode;
	LOCKMETHOD	lockmethod = LOCK_LOCKMETHOD(*waitLock);

	/* Make sure proc is waiting */
	Assert(proc->links.next != INVALID_OFFSET);
	Assert(waitLock);
	Assert(waitLock->waitProcs.size > 0);
	Assert(lockmethod > 0 && lockmethod < NumLockMethods);

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
		waitLock->waitMask &= BITS_OFF[lockmode];

	/* Clean up the proc's own state */
	proc->waitLock = NULL;
	proc->waitHolder = NULL;

	/*
	 * Delete the proclock immediately if it represents no already-held locks.
	 * This must happen now because if the owner of the lock decides to release
	 * it, and the requested/granted counts then go to zero, LockRelease
	 * expects there to be no remaining proclocks.
	 */
	if (proclock->nHolding == 0)
	{
		SHMQueueDelete(&proclock->lockLink);
		SHMQueueDelete(&proclock->procLink);
		proclock = (PROCLOCK *) hash_search(LockMethodTable[lockmethod]->proclockHash,
											(void *) proclock,
											HASH_REMOVE, NULL);
		if (!proclock)
			elog(WARNING, "proclock table corrupted");
	}

	/* See if any other waiters for the lock can be woken up now */
	ProcLockWakeup(LockMethodTable[lockmethod], waitLock);
}

/*
 * LockRelease -- look up 'locktag' in lock table 'lockmethod' and
 *		release one 'lockmode' lock on it.
 *
 * Side Effects: find any waiting processes that are now wakable,
 *		grant them their requested locks and awaken them.
 *		(We have to grant the lock here to avoid a race between
 *		the waking process and any new process to
 *		come along and request the lock.)
 */
bool
LockRelease(LOCKMETHOD lockmethod, LOCKTAG *locktag,
			TransactionId xid, LOCKMODE lockmode)
{
	LOCK	   *lock;
	LWLockId	masterLock;
	LOCKMETHODTABLE *lockMethodTable;
	PROCLOCK   *proclock;
	PROCLOCKTAG proclocktag;
	HTAB	   *proclockTable;
	bool		wakeupNeeded = false;

#ifdef LOCK_DEBUG
	if (lockmethod == USER_LOCKMETHOD && Trace_userlocks)
		elog(LOG, "LockRelease: user lock tag [%u] %d", locktag->objId.blkno, lockmode);
#endif

	/* ???????? This must be changed when short term locks will be used */
	locktag->lockmethod = lockmethod;

	Assert(lockmethod < NumLockMethods);
	lockMethodTable = LockMethodTable[lockmethod];
	if (!lockMethodTable)
	{
		elog(WARNING, "lockMethodTable is null in LockRelease");
		return FALSE;
	}

	masterLock = lockMethodTable->masterLock;
	LWLockAcquire(masterLock, LW_EXCLUSIVE);

	/*
	 * Find a lock with this tag
	 */
	Assert(lockMethodTable->lockHash->hash == tag_hash);
	lock = (LOCK *) hash_search(lockMethodTable->lockHash,
								(void *) locktag,
								HASH_FIND, NULL);

	/*
	 * let the caller print its own error message, too. Do not
	 * ereport(ERROR).
	 */
	if (!lock)
	{
		LWLockRelease(masterLock);
		elog(WARNING, "no such lock");
		return FALSE;
	}
	LOCK_PRINT("LockRelease: found", lock, lockmode);

	/*
	 * Find the proclock entry for this proclock.
	 */
	MemSet(&proclocktag, 0, sizeof(PROCLOCKTAG));		/* must clear padding,
														 * needed */
	proclocktag.lock = MAKE_OFFSET(lock);
	proclocktag.proc = MAKE_OFFSET(MyProc);
	TransactionIdStore(xid, &proclocktag.xid);

	proclockTable = lockMethodTable->proclockHash;
	proclock = (PROCLOCK *) hash_search(proclockTable,
										(void *) &proclocktag,
										HASH_FIND_SAVE, NULL);
	if (!proclock)
	{
		LWLockRelease(masterLock);
#ifdef USER_LOCKS
		if (lockmethod == USER_LOCKMETHOD)
			elog(WARNING, "no lock with this tag");
		else
#endif
			elog(WARNING, "proclock table corrupted");
		return FALSE;
	}
	PROCLOCK_PRINT("LockRelease: found", proclock);

	/*
	 * Check that we are actually holding a lock of the type we want to
	 * release.
	 */
	if (!(proclock->holding[lockmode] > 0))
	{
		PROCLOCK_PRINT("LockRelease: WRONGTYPE", proclock);
		Assert(proclock->holding[lockmode] >= 0);
		LWLockRelease(masterLock);
		elog(WARNING, "you don't own a lock of type %s",
			 lock_mode_names[lockmode]);
		return FALSE;
	}
	Assert(proclock->nHolding > 0);
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
		lock->grantMask &= BITS_OFF[lockmode];
	}

	LOCK_PRINT("LockRelease: updated", lock, lockmode);
	Assert((lock->nRequested >= 0) && (lock->requested[lockmode] >= 0));
	Assert((lock->nGranted >= 0) && (lock->granted[lockmode] >= 0));
	Assert(lock->nGranted <= lock->nRequested);

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

	if (lock->nRequested == 0)
	{
		/*
		 * if there's no one waiting in the queue, we just released the
		 * last lock on this object. Delete it from the lock table.
		 */
		Assert(lockMethodTable->lockHash->hash == tag_hash);
		lock = (LOCK *) hash_search(lockMethodTable->lockHash,
									(void *) &(lock->tag),
									HASH_REMOVE,
									NULL);
		if (!lock)
		{
			LWLockRelease(masterLock);
			elog(WARNING, "lock table corrupted");
			return FALSE;
		}
		wakeupNeeded = false;	/* should be false, but make sure */
	}

	/*
	 * Now fix the per-proclock lock stats.
	 */
	proclock->holding[lockmode]--;
	proclock->nHolding--;
	PROCLOCK_PRINT("LockRelease: updated", proclock);
	Assert((proclock->nHolding >= 0) && (proclock->holding[lockmode] >= 0));

	/*
	 * If this was my last hold on this lock, delete my entry in the
	 * proclock table.
	 */
	if (proclock->nHolding == 0)
	{
		PROCLOCK_PRINT("LockRelease: deleting", proclock);
		SHMQueueDelete(&proclock->lockLink);
		SHMQueueDelete(&proclock->procLink);
		proclock = (PROCLOCK *) hash_search(proclockTable,
											(void *) &proclock,
											HASH_REMOVE_SAVED, NULL);
		if (!proclock)
		{
			LWLockRelease(masterLock);
			elog(WARNING, "proclock table corrupted");
			return FALSE;
		}
	}

	/*
	 * Wake up waiters if needed.
	 */
	if (wakeupNeeded)
		ProcLockWakeup(lockMethodTable, lock);

	LWLockRelease(masterLock);
	return TRUE;
}

/*
 * LockReleaseAll -- Release all locks in a process's lock list.
 *
 * Well, not really *all* locks.
 *
 * If 'allxids' is TRUE, all locks of the specified lock method are
 * released, regardless of transaction affiliation.
 *
 * If 'allxids' is FALSE, all locks of the specified lock method and
 * specified XID are released.
 */
bool
LockReleaseAll(LOCKMETHOD lockmethod, PGPROC *proc,
			   bool allxids, TransactionId xid)
{
	SHM_QUEUE  *procHolders = &(proc->procHolders);
	PROCLOCK   *proclock;
	PROCLOCK   *nextHolder;
	LWLockId	masterLock;
	LOCKMETHODTABLE *lockMethodTable;
	int			i,
				numLockModes;
	LOCK	   *lock;

#ifdef LOCK_DEBUG
	if (lockmethod == USER_LOCKMETHOD ? Trace_userlocks : Trace_locks)
		elog(LOG, "LockReleaseAll: lockmethod=%d, pid=%d",
			 lockmethod, proc->pid);
#endif

	Assert(lockmethod < NumLockMethods);
	lockMethodTable = LockMethodTable[lockmethod];
	if (!lockMethodTable)
	{
		elog(WARNING, "bad lock method: %d", lockmethod);
		return FALSE;
	}

	numLockModes = lockMethodTable->numLockModes;
	masterLock = lockMethodTable->masterLock;

	LWLockAcquire(masterLock, LW_EXCLUSIVE);

	proclock = (PROCLOCK *) SHMQueueNext(procHolders, procHolders,
										 offsetof(PROCLOCK, procLink));

	while (proclock)
	{
		bool		wakeupNeeded = false;

		/* Get link first, since we may unlink/delete this proclock */
		nextHolder = (PROCLOCK *) SHMQueueNext(procHolders, &proclock->procLink,
										   offsetof(PROCLOCK, procLink));

		Assert(proclock->tag.proc == MAKE_OFFSET(proc));

		lock = (LOCK *) MAKE_PTR(proclock->tag.lock);

		/* Ignore items that are not of the lockmethod to be removed */
		if (LOCK_LOCKMETHOD(*lock) != lockmethod)
			goto next_item;

		/* If not allxids, ignore items that are of the wrong xid */
		if (!allxids && !TransactionIdEquals(xid, proclock->tag.xid))
			goto next_item;

		PROCLOCK_PRINT("LockReleaseAll", proclock);
		LOCK_PRINT("LockReleaseAll", lock, 0);
		Assert(lock->nRequested >= 0);
		Assert(lock->nGranted >= 0);
		Assert(lock->nGranted <= lock->nRequested);
		Assert(proclock->nHolding >= 0);
		Assert(proclock->nHolding <= lock->nRequested);

		/*
		 * fix the general lock stats
		 */
		if (lock->nRequested != proclock->nHolding)
		{
			for (i = 1; i <= numLockModes; i++)
			{
				Assert(proclock->holding[i] >= 0);
				if (proclock->holding[i] > 0)
				{
					lock->requested[i] -= proclock->holding[i];
					lock->granted[i] -= proclock->holding[i];
					Assert(lock->requested[i] >= 0 && lock->granted[i] >= 0);
					if (lock->granted[i] == 0)
						lock->grantMask &= BITS_OFF[i];

					/*
					 * Read comments in LockRelease
					 */
					if (!wakeupNeeded &&
						lockMethodTable->conflictTab[i] & lock->waitMask)
						wakeupNeeded = true;
				}
			}
			lock->nRequested -= proclock->nHolding;
			lock->nGranted -= proclock->nHolding;
			Assert((lock->nRequested >= 0) && (lock->nGranted >= 0));
			Assert(lock->nGranted <= lock->nRequested);
		}
		else
		{
			/*
			 * This proclock accounts for all the requested locks on the
			 * object, so we can be lazy and just zero things out.
			 */
			lock->nRequested = 0;
			lock->nGranted = 0;
			/* Fix the lock status, just for next LOCK_PRINT message. */
			for (i = 1; i <= numLockModes; i++)
			{
				Assert(lock->requested[i] == lock->granted[i]);
				lock->requested[i] = lock->granted[i] = 0;
			}
		}
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
		proclock = (PROCLOCK *) hash_search(lockMethodTable->proclockHash,
											(void *) proclock,
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
			Assert(lockMethodTable->lockHash->hash == tag_hash);
			lock = (LOCK *) hash_search(lockMethodTable->lockHash,
										(void *) &(lock->tag),
										HASH_REMOVE, NULL);
			if (!lock)
			{
				LWLockRelease(masterLock);
				elog(WARNING, "cannot remove lock from HTAB");
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
	if (lockmethod == USER_LOCKMETHOD ? Trace_userlocks : Trace_locks)
		elog(LOG, "LockReleaseAll done");
#endif

	return TRUE;
}

int
LockShmemSize(int maxBackends)
{
	int			size = 0;
	long		max_table_size = NLOCKENTS(maxBackends);

	size += MAXALIGN(sizeof(PROC_HDR)); /* ProcGlobal */
	size += maxBackends * MAXALIGN(sizeof(PGPROC));		/* each MyProc */
	size += MAX_LOCK_METHODS * MAXALIGN(sizeof(LOCKMETHODTABLE));		/* each lockMethodTable */

	/* lockHash table */
	size += hash_estimate_size(max_table_size, sizeof(LOCK));

	/* proclockHash table */
	size += hash_estimate_size(max_table_size, sizeof(PROCLOCK));

	/*
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

	proclockTable = LockMethodTable[DEFAULT_LOCKMETHOD]->proclockHash;

	data->nelements = i = proclockTable->hctl->nentries;

	if (i == 0)
		i = 1;					/* avoid palloc(0) if empty table */

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
 * Dump all locks in the proc->procHolders list.
 *
 * Must have already acquired the masterLock.
 */
void
DumpLocks(void)
{
	PGPROC	   *proc;
	SHM_QUEUE  *procHolders;
	PROCLOCK   *proclock;
	LOCK	   *lock;
	int			lockmethod = DEFAULT_LOCKMETHOD;
	LOCKMETHODTABLE *lockMethodTable;

	proc = MyProc;
	if (proc == NULL)
		return;

	procHolders = &proc->procHolders;

	Assert(lockmethod < NumLockMethods);
	lockMethodTable = LockMethodTable[lockmethod];
	if (!lockMethodTable)
		return;

	if (proc->waitLock)
		LOCK_PRINT("DumpLocks: waiting on", proc->waitLock, 0);

	proclock = (PROCLOCK *) SHMQueueNext(procHolders, procHolders,
										 offsetof(PROCLOCK, procLink));

	while (proclock)
	{
		Assert(proclock->tag.proc == MAKE_OFFSET(proc));

		lock = (LOCK *) MAKE_PTR(proclock->tag.lock);

		PROCLOCK_PRINT("DumpLocks", proclock);
		LOCK_PRINT("DumpLocks", lock, 0);

		proclock = (PROCLOCK *) SHMQueueNext(procHolders, &proclock->procLink,
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
	int			lockmethod = DEFAULT_LOCKMETHOD;
	LOCKMETHODTABLE *lockMethodTable;
	HTAB	   *proclockTable;
	HASH_SEQ_STATUS status;

	proc = MyProc;
	if (proc == NULL)
		return;

	Assert(lockmethod < NumLockMethods);
	lockMethodTable = LockMethodTable[lockmethod];
	if (!lockMethodTable)
		return;

	proclockTable = lockMethodTable->proclockHash;

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
