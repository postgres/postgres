/*-------------------------------------------------------------------------
 *
 * lock.c
 *	  POSTGRES low-level lock mechanism
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/lmgr/lock.c,v 1.83 2001/02/22 23:20:06 momjian Exp $
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

#include <sys/types.h>
#include <unistd.h>
#include <signal.h>

#include "access/xact.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"

static int	WaitOnLock(LOCKMETHOD lockmethod, LOCKMODE lockmode,
					   LOCK *lock, HOLDER *holder);
static void LockCountMyLocks(SHMEM_OFFSET lockOffset, PROC *proc,
							 int *myHolding);

static char *lock_mode_names[] =
{
	"INVALID",
	"AccessShareLock",
	"RowShareLock",
	"RowExclusiveLock",
	"ShareLock",
	"ShareRowExclusiveLock",
	"ExclusiveLock",
	"AccessExclusiveLock"
};

static char *DeadLockMessage = "Deadlock detected.\n\tSee the lock(l) manual page for a possible cause.";


#ifdef LOCK_DEBUG

/*------
 * The following configuration options are available for lock debugging:
 *
 *     TRACE_LOCKS      -- give a bunch of output what's going on in this file
 *     TRACE_USERLOCKS  -- same but for user locks
 *     TRACE_LOCK_OIDMIN-- do not trace locks for tables below this oid
 *                         (use to avoid output on system tables)
 *     TRACE_LOCK_TABLE -- trace locks on this table (oid) unconditionally
 *     DEBUG_DEADLOCKS  -- currently dumps locks at untimely occasions ;)
 *
 * Furthermore, but in storage/ipc/spin.c:
 *     TRACE_SPINLOCKS  -- trace spinlocks (pretty useless)
 *
 * Define LOCK_DEBUG at compile time to get all these enabled.
 * --------
 */

int  Trace_lock_oidmin  = BootstrapObjectIdData;
bool Trace_locks        = false;
bool Trace_userlocks    = false;
int  Trace_lock_table   = 0;
bool Debug_deadlocks    = false;


inline static bool
LOCK_DEBUG_ENABLED(const LOCK * lock)
{
	return
		(((LOCK_LOCKMETHOD(*lock) == DEFAULT_LOCKMETHOD && Trace_locks)
		  || (LOCK_LOCKMETHOD(*lock) == USER_LOCKMETHOD && Trace_userlocks))
		 && (lock->tag.relId >= (Oid) Trace_lock_oidmin))
		|| (Trace_lock_table && (lock->tag.relId == Trace_lock_table));
}


inline static void
LOCK_PRINT(const char * where, const LOCK * lock, LOCKMODE type)
{
	if (LOCK_DEBUG_ENABLED(lock))
		elog(DEBUG,
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
HOLDER_PRINT(const char * where, const HOLDER * holderP)
{
	if (
		(((HOLDER_LOCKMETHOD(*holderP) == DEFAULT_LOCKMETHOD && Trace_locks)
		  || (HOLDER_LOCKMETHOD(*holderP) == USER_LOCKMETHOD && Trace_userlocks))
		 && (((LOCK *)MAKE_PTR(holderP->tag.lock))->tag.relId >= (Oid) Trace_lock_oidmin))
		|| (Trace_lock_table && (((LOCK *)MAKE_PTR(holderP->tag.lock))->tag.relId == Trace_lock_table))
		)
		elog(DEBUG,
			 "%s: holder(%lx) lock(%lx) tbl(%d) proc(%lx) xid(%u) hold(%d,%d,%d,%d,%d,%d,%d)=%d",
			 where, MAKE_OFFSET(holderP), holderP->tag.lock,
			 HOLDER_LOCKMETHOD(*(holderP)),
			 holderP->tag.proc, holderP->tag.xid,
			 holderP->holding[1], holderP->holding[2], holderP->holding[3],
			 holderP->holding[4], holderP->holding[5], holderP->holding[6],
			 holderP->holding[7], holderP->nHolding);
}

#else  /* not LOCK_DEBUG */

#define LOCK_PRINT(where, lock, type)
#define HOLDER_PRINT(where, holderP)

#endif /* not LOCK_DEBUG */



SPINLOCK	LockMgrLock;		/* in Shmem or created in
								 * CreateSpinlocks() */

/*
 * These are to simplify/speed up some bit arithmetic.
 *
 * XXX is a fetch from a static array really faster than a shift?
 * Wouldn't bet on it...
 */

static LOCKMASK BITS_OFF[MAX_LOCKMODES];
static LOCKMASK BITS_ON[MAX_LOCKMODES];

/*
 * Disable flag
 *
 */
static bool LockingIsDisabled;

/*
 * map from lockmethod to the lock table structure
 *
 */
static LOCKMETHODTABLE *LockMethodTable[MAX_LOCK_METHODS];

static int	NumLockMethods;

/*
 * InitLocks -- Init the lock module.  Create a private data
 *		structure for constructing conflict masks.
 *
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
 * LockDisable -- sets LockingIsDisabled flag to TRUE or FALSE.
 *
 */
void
LockDisable(bool status)
{
	LockingIsDisabled = status;
}

/*
 * Boolean function to determine current locking status
 *
 */
bool
LockingDisabled(void)
{
	return LockingIsDisabled;
}

/*
 * Fetch the lock method table associated with a given lock
 */
LOCKMETHODTABLE *
GetLocksMethodTable(LOCK *lock)
{
	LOCKMETHOD lockmethod = LOCK_LOCKMETHOD(*lock);

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
			   int *prioP,
			   int numModes)
{
	int			i;

	lockMethodTable->ctl->numLockModes = numModes;
	numModes++;
	for (i = 0; i < numModes; i++, prioP++, conflictsP++)
	{
		lockMethodTable->ctl->conflictTab[i] = *conflictsP;
		lockMethodTable->ctl->prio[i] = *prioP;
	}
}

/*
 * LockMethodTableInit -- initialize a lock table structure
 *
 * Notes:
 *		(a) a lock table has four separate entries in the shmem index
 *		table.	This is because every shared hash table and spinlock
 *		has its name stored in the shmem index at its creation.  It
 *		is wasteful, in this case, but not much space is involved.
 *
 * NOTE: data structures allocated here are allocated permanently, using
 * TopMemoryContext and shared memory.  We don't ever release them anyway,
 * and in normal multi-backend operation the lock table structures set up
 * by the postmaster are inherited by each backend, so they must be in
 * TopMemoryContext.
 */
LOCKMETHOD
LockMethodTableInit(char *tabName,
					LOCKMASK *conflictsP,
					int *prioP,
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

	if (numModes > MAX_LOCKMODES)
	{
		elog(NOTICE, "LockMethodTableInit: too many lock types %d greater than %d",
			 numModes, MAX_LOCKMODES);
		return INVALID_LOCKMETHOD;
	}

	/* Compute init/max size to request for lock hashtables */
	max_table_size = NLOCKENTS(maxBackends);
	init_table_size = max_table_size / 10;

	/* Allocate a string for the shmem index table lookups. */
	/* This is just temp space in this routine, so palloc is OK. */
	shmemName = (char *) palloc(strlen(tabName) + 32);

	/* each lock table has a non-shared, permanent header */
	lockMethodTable = (LOCKMETHODTABLE *)
		MemoryContextAlloc(TopMemoryContext, sizeof(LOCKMETHODTABLE));

	/*
	 * find/acquire the spinlock for the table
	 *
	 */
	SpinAcquire(LockMgrLock);

	/*
	 * allocate a control structure from shared memory or attach to it
	 * if it already exists.
	 *
	 */
	sprintf(shmemName, "%s (ctl)", tabName);
	lockMethodTable->ctl = (LOCKMETHODCTL *)
		ShmemInitStruct(shmemName, sizeof(LOCKMETHODCTL), &found);

	if (!lockMethodTable->ctl)
		elog(FATAL, "LockMethodTableInit: couldn't initialize %s", tabName);

	/*
	 * no zero-th table
	 *
	 */
	NumLockMethods = 1;

	/*
	 * we're first - initialize
	 *
	 */
	if (!found)
	{
		MemSet(lockMethodTable->ctl, 0, sizeof(LOCKMETHODCTL));
		lockMethodTable->ctl->masterLock = LockMgrLock;
		lockMethodTable->ctl->lockmethod = NumLockMethods;
	}

	/*
	 * other modules refer to the lock table by a lockmethod ID
	 *
	 */
	LockMethodTable[NumLockMethods] = lockMethodTable;
	NumLockMethods++;
	Assert(NumLockMethods <= MAX_LOCK_METHODS);

	/*
	 * allocate a hash table for LOCK structs.  This is used
	 * to store per-locked-object information.
	 *
	 */
	info.keysize = SHMEM_LOCKTAB_KEYSIZE;
	info.datasize = SHMEM_LOCKTAB_DATASIZE;
	info.hash = tag_hash;
	hash_flags = (HASH_ELEM | HASH_FUNCTION);

	sprintf(shmemName, "%s (lock hash)", tabName);
	lockMethodTable->lockHash = ShmemInitHash(shmemName,
											  init_table_size,
											  max_table_size,
											  &info,
											  hash_flags);

	if (!lockMethodTable->lockHash)
		elog(FATAL, "LockMethodTableInit: couldn't initialize %s", tabName);
	Assert(lockMethodTable->lockHash->hash == tag_hash);

	/*
	 * allocate a hash table for HOLDER structs.  This is used
	 * to store per-lock-holder information.
	 *
	 */
	info.keysize = SHMEM_HOLDERTAB_KEYSIZE;
	info.datasize = SHMEM_HOLDERTAB_DATASIZE;
	info.hash = tag_hash;
	hash_flags = (HASH_ELEM | HASH_FUNCTION);

	sprintf(shmemName, "%s (holder hash)", tabName);
	lockMethodTable->holderHash = ShmemInitHash(shmemName,
												init_table_size,
												max_table_size,
												&info,
												hash_flags);

	if (!lockMethodTable->holderHash)
		elog(FATAL, "LockMethodTableInit: couldn't initialize %s", tabName);

	/* init ctl data structures */
	LockMethodInit(lockMethodTable, conflictsP, prioP, numModes);

	SpinRelease(LockMgrLock);

	pfree(shmemName);

	return lockMethodTable->ctl->lockmethod;
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
 * Returns: TRUE if parameters are correct, FALSE otherwise.
 *
 * Side Effects: The lock is always acquired.  No way to abort
 *		a lock acquisition other than aborting the transaction.
 *		Lock is recorded in the lkchain.
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
 *		holder.xid						xid or 0		0
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
			TransactionId xid, LOCKMODE lockmode)
{
	HOLDER	   *holder;
	HOLDERTAG	holdertag;
	HTAB	   *holderTable;
	bool		found;
	LOCK	   *lock;
	SPINLOCK	masterLock;
	LOCKMETHODTABLE *lockMethodTable;
	int			status;
	int			myHolding[MAX_LOCKMODES];
	int			i;

#ifdef LOCK_DEBUG
	if (lockmethod == USER_LOCKMETHOD && Trace_userlocks)
		elog(DEBUG, "LockAcquire: user lock [%u] %s",
			 locktag->objId.blkno, lock_mode_names[lockmode]);
#endif

	/* ???????? This must be changed when short term locks will be used */
	locktag->lockmethod = lockmethod;

	Assert(lockmethod < NumLockMethods);
	lockMethodTable = LockMethodTable[lockmethod];
	if (!lockMethodTable)
	{
		elog(NOTICE, "LockAcquire: bad lock table %d", lockmethod);
		return FALSE;
	}

	if (LockingIsDisabled)
		return TRUE;

	masterLock = lockMethodTable->ctl->masterLock;

	SpinAcquire(masterLock);

	/*
	 * Find or create a lock with this tag
	 */
	Assert(lockMethodTable->lockHash->hash == tag_hash);
	lock = (LOCK *) hash_search(lockMethodTable->lockHash, (Pointer) locktag,
								HASH_ENTER, &found);
	if (!lock)
	{
		SpinRelease(masterLock);
		elog(FATAL, "LockAcquire: lock table %d is corrupted", lockmethod);
		return FALSE;
	}

	/*
	 * if it's a new lock object, initialize it
	 *
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
	 * Create the hash key for the holder table.
	 *
	 */
	MemSet(&holdertag, 0, sizeof(HOLDERTAG)); /* must clear padding, needed */
	holdertag.lock = MAKE_OFFSET(lock);
	holdertag.proc = MAKE_OFFSET(MyProc);
	TransactionIdStore(xid, &holdertag.xid);

	/*
	 * Find or create a holder entry with this tag
	 */
	holderTable = lockMethodTable->holderHash;
	holder = (HOLDER *) hash_search(holderTable, (Pointer) &holdertag,
									HASH_ENTER, &found);
	if (!holder)
	{
		SpinRelease(masterLock);
		elog(FATAL, "LockAcquire: holder table corrupted");
		return FALSE;
	}

	/*
	 * If new, initialize the new entry
	 */
	if (!found)
	{
		holder->nHolding = 0;
		MemSet((char *) holder->holding, 0, sizeof(int) * MAX_LOCKMODES);
		/* Add holder to appropriate lists */
		SHMQueueInsertBefore(&lock->lockHolders, &holder->lockLink);
		SHMQueueInsertBefore(&MyProc->procHolders, &holder->procLink);
		HOLDER_PRINT("LockAcquire: new", holder);
	}
	else
	{
		HOLDER_PRINT("LockAcquire: found", holder);
		Assert((holder->nHolding >= 0) && (holder->holding[lockmode] >= 0));
		Assert(holder->nHolding <= lock->nGranted);

#ifdef CHECK_DEADLOCK_RISK
		/*
		 * Issue warning if we already hold a lower-level lock on this
		 * object and do not hold a lock of the requested level or higher.
		 * This indicates a deadlock-prone coding practice (eg, we'd have
		 * a deadlock if another backend were following the same code path
		 * at about the same time).
		 *
		 * This is not enabled by default, because it may generate log entries
		 * about user-level coding practices that are in fact safe in context.
		 * It can be enabled to help find system-level problems.
		 *
		 * XXX Doing numeric comparison on the lockmodes is a hack;
		 * it'd be better to use a table.  For now, though, this works.
		 */
		for (i = lockMethodTable->ctl->numLockModes; i > 0; i--)
		{
			if (holder->holding[i] > 0)
			{
				if (i >= (int) lockmode)
					break;		/* safe: we have a lock >= req level */
				elog(DEBUG, "Deadlock risk: raising lock level"
					 " from %s to %s on object %u/%u/%u",
					 lock_mode_names[i], lock_mode_names[lockmode],
					 lock->tag.relId, lock->tag.dbId, lock->tag.objId.blkno);
				break;
			}
		}
#endif /* CHECK_DEADLOCK_RISK */
	}

	/*
	 * lock->nRequested and lock->requested[] count the total number of
	 * requests, whether granted or waiting, so increment those immediately.
	 * The other counts don't increment till we get the lock.
	 *
	 */
	lock->nRequested++;
	lock->requested[lockmode]++;
	Assert((lock->nRequested > 0) && (lock->requested[lockmode] > 0));

	/*
	 * If I already hold one or more locks of the requested type,
	 * just grant myself another one without blocking.
	 *
	 */
	if (holder->holding[lockmode] > 0)
	{
		GrantLock(lock, holder, lockmode);
		HOLDER_PRINT("LockAcquire: owning", holder);
		SpinRelease(masterLock);
		return TRUE;
	}

	/*
	 * If this process (under any XID) is a holder of the lock,
	 * also grant myself another one without blocking.
	 *
	 */
	LockCountMyLocks(holder->tag.lock, MyProc, myHolding);
	if (myHolding[lockmode] > 0)
	{
		GrantLock(lock, holder, lockmode);
		HOLDER_PRINT("LockAcquire: my other XID owning", holder);
		SpinRelease(masterLock);
		return TRUE;
	}

	/*
	 * If lock requested conflicts with locks requested by waiters,
	 * must join wait queue.  Otherwise, check for conflict with
	 * already-held locks.  (That's last because most complex check.)
	 *
	 */
	if (lockMethodTable->ctl->conflictTab[lockmode] & lock->waitMask)
		status = STATUS_FOUND;
	else
		status = LockCheckConflicts(lockMethodTable, lockmode,
									lock, holder,
									MyProc, myHolding);

	if (status == STATUS_OK)
	{
		/* No conflict with held or previously requested locks */
		GrantLock(lock, holder, lockmode);
	}
	else
	{
		Assert(status == STATUS_FOUND);
#ifdef USER_LOCKS

		/*
		 * User locks are non blocking. If we can't acquire a lock we must
		 * remove the holder entry and return FALSE without waiting.
		 */
		if (lockmethod == USER_LOCKMETHOD)
		{
			if (holder->nHolding == 0)
			{
				SHMQueueDelete(&holder->lockLink);
				SHMQueueDelete(&holder->procLink);
				holder = (HOLDER *) hash_search(holderTable,
												(Pointer) holder,
												HASH_REMOVE, &found);
				if (!holder || !found)
					elog(NOTICE, "LockAcquire: remove holder, table corrupted");
			}
			else
				HOLDER_PRINT("LockAcquire: NHOLDING", holder);
			lock->nRequested--;
			lock->requested[lockmode]--;
			LOCK_PRINT("LockAcquire: user lock failed", lock, lockmode);
			Assert((lock->nRequested > 0) && (lock->requested[lockmode] >= 0));
			Assert(lock->nGranted <= lock->nRequested);
			SpinRelease(masterLock);
			return FALSE;
		}
#endif /* USER_LOCKS */

		/*
		 * Construct bitmask of locks this process holds on this object.
		 */
		{
			int			heldLocks = 0;
			int			tmpMask;

			for (i = 1, tmpMask = 2;
				 i <= lockMethodTable->ctl->numLockModes;
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
		status = WaitOnLock(lockmethod, lockmode, lock, holder);

		/*
		 * NOTE: do not do any material change of state between here and
		 * return.  All required changes in locktable state must have been
		 * done when the lock was granted to us --- see notes in WaitOnLock.
		 */

		/*
		 * Check the holder entry status, in case something in the ipc
		 * communication doesn't work correctly.
		 */
		if (!((holder->nHolding > 0) && (holder->holding[lockmode] > 0)))
		{
			HOLDER_PRINT("LockAcquire: INCONSISTENT", holder);
			LOCK_PRINT("LockAcquire: INCONSISTENT", lock, lockmode);
			/* Should we retry ? */
			SpinRelease(masterLock);
			return FALSE;
		}
		HOLDER_PRINT("LockAcquire: granted", holder);
		LOCK_PRINT("LockAcquire: granted", lock, lockmode);
	}

	SpinRelease(masterLock);

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
 *
 */
int
LockCheckConflicts(LOCKMETHODTABLE *lockMethodTable,
				   LOCKMODE lockmode,
				   LOCK *lock,
				   HOLDER *holder,
				   PROC *proc,
				   int *myHolding)		/* myHolding[] array or NULL */
{
	LOCKMETHODCTL *lockctl = lockMethodTable->ctl;
	int			numLockModes = lockctl->numLockModes;
	int			bitmask;
	int			i,
				tmpMask;
	int			localHolding[MAX_LOCKMODES];

	/*
	 * first check for global conflicts: If no locks conflict
	 * with my request, then I get the lock.
	 *
	 * Checking for conflict: lock->grantMask represents the types of
	 * currently held locks.  conflictTable[lockmode] has a bit
	 * set for each type of lock that conflicts with request.	Bitwise
	 * compare tells if there is a conflict.
	 *
	 */
	if (!(lockctl->conflictTab[lockmode] & lock->grantMask))
	{
		HOLDER_PRINT("LockCheckConflicts: no conflict", holder);
		return STATUS_OK;
	}

	/*
	 * Rats.  Something conflicts. But it could still be my own
	 * lock.  We have to construct a conflict mask
	 * that does not reflect our own locks.  Locks held by the current
	 * process under another XID also count as "our own locks".
	 *
	 */
	if (myHolding == NULL)
	{
		/* Caller didn't do calculation of total holding for me */
		LockCountMyLocks(holder->tag.lock, proc, localHolding);
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
	 * now check again for conflicts.  'bitmask' describes the types
	 * of locks held by other processes.  If one of these
	 * conflicts with the kind of lock that I want, there is a
	 * conflict and I have to sleep.
	 *
	 */
	if (!(lockctl->conflictTab[lockmode] & bitmask))
	{
		/* no conflict. OK to get the lock */
		HOLDER_PRINT("LockCheckConflicts: resolved", holder);
		return STATUS_OK;
	}

	HOLDER_PRINT("LockCheckConflicts: conflicting", holder);
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
LockCountMyLocks(SHMEM_OFFSET lockOffset, PROC *proc, int *myHolding)
{
	SHM_QUEUE  *procHolders = &(proc->procHolders);
	HOLDER	   *holder;
	int			i;

	MemSet(myHolding, 0, MAX_LOCKMODES * sizeof(int));

	holder = (HOLDER *) SHMQueueNext(procHolders, procHolders,
									 offsetof(HOLDER, procLink));

	while (holder)
	{
		if (lockOffset == holder->tag.lock)
		{
			for (i = 1; i < MAX_LOCKMODES; i++)
			{
				myHolding[i] += holder->holding[i];
			}
		}

		holder = (HOLDER *) SHMQueueNext(procHolders, &holder->procLink,
										 offsetof(HOLDER, procLink));
	}
}

/*
 * GrantLock -- update the lock and holder data structures to show
 *		the lock request has been granted.
 *
 * NOTE: if proc was blocked, it also needs to be removed from the wait list
 * and have its waitLock/waitHolder fields cleared.  That's not done here.
 */
void
GrantLock(LOCK *lock, HOLDER *holder, LOCKMODE lockmode)
{
	lock->nGranted++;
	lock->granted[lockmode]++;
	lock->grantMask |= BITS_ON[lockmode];
	if (lock->granted[lockmode] == lock->requested[lockmode])
		lock->waitMask &= BITS_OFF[lockmode];
	LOCK_PRINT("GrantLock", lock, lockmode);
	Assert((lock->nGranted > 0) && (lock->granted[lockmode] > 0));
	Assert(lock->nGranted <= lock->nRequested);
	holder->holding[lockmode]++;
	holder->nHolding++;
	Assert((holder->nHolding > 0) && (holder->holding[lockmode] > 0));
}

/*
 * WaitOnLock -- wait to acquire a lock
 *
 * Caller must have set MyProc->heldLocks to reflect locks already held
 * on the lockable object by this process (under all XIDs).
 *
 * The locktable spinlock must be held at entry.
 */
static int
WaitOnLock(LOCKMETHOD lockmethod, LOCKMODE lockmode,
		   LOCK *lock, HOLDER *holder)
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
	 * NOTE: Think not to put any lock state cleanup after the call to
	 * ProcSleep, in either the normal or failure path.  The lock state
	 * must be fully set by the lock grantor, or by HandleDeadLock if we
	 * give up waiting for the lock.  This is necessary because of the
	 * possibility that a cancel/die interrupt will interrupt ProcSleep
	 * after someone else grants us the lock, but before we've noticed it.
	 * Hence, after granting, the locktable state must fully reflect the
	 * fact that we own the lock; we can't do additional work on return.
	 *
	 */

	if (ProcSleep(lockMethodTable,
				  lockmode,
				  lock,
				  holder) != STATUS_OK)
	{
		/*
		 * We failed as a result of a deadlock, see HandleDeadLock().
		 * Quit now.  Removal of the holder and lock objects, if no longer
		 * needed, will happen in xact cleanup (see above for motivation).
		 *
		 */
		LOCK_PRINT("WaitOnLock: aborting on lock", lock, lockmode);
		SpinRelease(lockMethodTable->ctl->masterLock);
		elog(ERROR, DeadLockMessage);
		/* not reached */
	}

	set_ps_display(old_status);
	pfree(old_status);
	pfree(new_status);

	LOCK_PRINT("WaitOnLock: wakeup on lock", lock, lockmode);
	return STATUS_OK;
}

/*--------------------
 * Remove a proc from the wait-queue it is on
 * (caller must know it is on one).
 *
 * Locktable lock must be held by caller.
 *
 * NB: this does not remove the process' holder object, nor the lock object,
 * even though their counts might now have gone to zero.  That will happen
 * during a subsequent LockReleaseAll call, which we expect will happen
 * during transaction cleanup.  (Removal of a proc from its wait queue by
 * this routine can only happen if we are aborting the transaction.)
 *--------------------
 */
void
RemoveFromWaitQueue(PROC *proc)
{
	LOCK   *waitLock = proc->waitLock;
	LOCKMODE lockmode = proc->waitLockMode;

	/* Make sure proc is waiting */
	Assert(proc->links.next != INVALID_OFFSET);
	Assert(waitLock);
	Assert(waitLock->waitProcs.size > 0);

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

	/* See if any other waiters for the lock can be woken up now */
	ProcLockWakeup(GetLocksMethodTable(waitLock), waitLock);
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
	SPINLOCK	masterLock;
	bool		found;
	LOCKMETHODTABLE *lockMethodTable;
	HOLDER	   *holder;
	HOLDERTAG	holdertag;
	HTAB	   *holderTable;
	bool		wakeupNeeded = false;

#ifdef LOCK_DEBUG
	if (lockmethod == USER_LOCKMETHOD && Trace_userlocks)
		elog(DEBUG, "LockRelease: user lock tag [%u] %d", locktag->objId.blkno, lockmode);
#endif

	/* ???????? This must be changed when short term locks will be used */
	locktag->lockmethod = lockmethod;

	Assert(lockmethod < NumLockMethods);
	lockMethodTable = LockMethodTable[lockmethod];
	if (!lockMethodTable)
	{
		elog(NOTICE, "lockMethodTable is null in LockRelease");
		return FALSE;
	}

	if (LockingIsDisabled)
		return TRUE;

	masterLock = lockMethodTable->ctl->masterLock;
	SpinAcquire(masterLock);

	/*
	 * Find a lock with this tag
	 */
	Assert(lockMethodTable->lockHash->hash == tag_hash);
	lock = (LOCK *) hash_search(lockMethodTable->lockHash, (Pointer) locktag,
								HASH_FIND, &found);

	/*
	 * let the caller print its own error message, too. Do not
	 * elog(ERROR).
	 */
	if (!lock)
	{
		SpinRelease(masterLock);
		elog(NOTICE, "LockRelease: locktable corrupted");
		return FALSE;
	}

	if (!found)
	{
		SpinRelease(masterLock);
		elog(NOTICE, "LockRelease: no such lock");
		return FALSE;
	}
	LOCK_PRINT("LockRelease: found", lock, lockmode);

	/*
	 * Find the holder entry for this holder.
	 */
	MemSet(&holdertag, 0, sizeof(HOLDERTAG)); /* must clear padding, needed */
	holdertag.lock = MAKE_OFFSET(lock);
	holdertag.proc = MAKE_OFFSET(MyProc);
	TransactionIdStore(xid, &holdertag.xid);

	holderTable = lockMethodTable->holderHash;
	holder = (HOLDER *) hash_search(holderTable, (Pointer) &holdertag,
									HASH_FIND_SAVE, &found);
	if (!holder || !found)
	{
		SpinRelease(masterLock);
#ifdef USER_LOCKS
		if (!found && lockmethod == USER_LOCKMETHOD)
			elog(NOTICE, "LockRelease: no lock with this tag");
		else
#endif
			elog(NOTICE, "LockRelease: holder table corrupted");
		return FALSE;
	}
	HOLDER_PRINT("LockRelease: found", holder);

	/*
	 * Check that we are actually holding a lock of the type we want to
	 * release.
	 */
	if (!(holder->holding[lockmode] > 0))
	{
		HOLDER_PRINT("LockRelease: WRONGTYPE", holder);
		Assert(holder->holding[lockmode] >= 0);
		SpinRelease(masterLock);
		elog(NOTICE, "LockRelease: you don't own a lock of type %s",
			 lock_mode_names[lockmode]);
		return FALSE;
	}
	Assert(holder->nHolding > 0);
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
	 * whatever conflict made them wait must still exist.  NOTE: before MVCC,
	 * we could skip wakeup if lock->granted[lockmode] was still positive.
	 * But that's not true anymore, because the remaining granted locks might
	 * belong to some waiter, who could now be awakened because he doesn't
	 * conflict with his own locks.
	 *
	 */
	if (lockMethodTable->ctl->conflictTab[lockmode] & lock->waitMask)
		wakeupNeeded = true;

	if (lock->nRequested == 0)
	{
		/*
		 * if there's no one waiting in the queue,
		 * we just released the last lock on this object.
		 * Delete it from the lock table.
		 *
		 */
		Assert(lockMethodTable->lockHash->hash == tag_hash);
		lock = (LOCK *) hash_search(lockMethodTable->lockHash,
									(Pointer) &(lock->tag),
									HASH_REMOVE,
									&found);
		if (!lock || !found)
		{
			SpinRelease(masterLock);
			elog(NOTICE, "LockRelease: remove lock, table corrupted");
			return FALSE;
		}
		wakeupNeeded = false;	/* should be false, but make sure */
	}

	/*
	 * Now fix the per-holder lock stats.
	 */
	holder->holding[lockmode]--;
	holder->nHolding--;
	HOLDER_PRINT("LockRelease: updated", holder);
	Assert((holder->nHolding >= 0) && (holder->holding[lockmode] >= 0));

	/*
	 * If this was my last hold on this lock, delete my entry in the holder
	 * table.
	 */
	if (holder->nHolding == 0)
	{
		HOLDER_PRINT("LockRelease: deleting", holder);
		SHMQueueDelete(&holder->lockLink);
		SHMQueueDelete(&holder->procLink);
		holder = (HOLDER *) hash_search(holderTable, (Pointer) &holder,
										HASH_REMOVE_SAVED, &found);
		if (!holder || !found)
		{
			SpinRelease(masterLock);
			elog(NOTICE, "LockRelease: remove holder, table corrupted");
			return FALSE;
		}
	}

	/*
	 * Wake up waiters if needed.
	 */
	if (wakeupNeeded)
		ProcLockWakeup(lockMethodTable, lock);

	SpinRelease(masterLock);
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
LockReleaseAll(LOCKMETHOD lockmethod, PROC *proc,
			   bool allxids, TransactionId xid)
{
	SHM_QUEUE  *procHolders = &(proc->procHolders);
	HOLDER	   *holder;
	HOLDER	   *nextHolder;
	SPINLOCK	masterLock;
	LOCKMETHODTABLE *lockMethodTable;
	int			i,
				numLockModes;
	LOCK	   *lock;
	bool		found;

#ifdef LOCK_DEBUG
	if (lockmethod == USER_LOCKMETHOD ? Trace_userlocks : Trace_locks)
		elog(DEBUG, "LockReleaseAll: lockmethod=%d, pid=%d",
			 lockmethod, proc->pid);
#endif

	Assert(lockmethod < NumLockMethods);
	lockMethodTable = LockMethodTable[lockmethod];
	if (!lockMethodTable)
	{
		elog(NOTICE, "LockReleaseAll: bad lockmethod %d", lockmethod);
		return FALSE;
	}

	numLockModes = lockMethodTable->ctl->numLockModes;
	masterLock = lockMethodTable->ctl->masterLock;

	SpinAcquire(masterLock);

	holder = (HOLDER *) SHMQueueNext(procHolders, procHolders,
									 offsetof(HOLDER, procLink));

	while (holder)
	{
		bool		wakeupNeeded = false;

		/* Get link first, since we may unlink/delete this holder */
		nextHolder = (HOLDER *) SHMQueueNext(procHolders, &holder->procLink,
											 offsetof(HOLDER, procLink));

		Assert(holder->tag.proc == MAKE_OFFSET(proc));

		lock = (LOCK *) MAKE_PTR(holder->tag.lock);

		/* Ignore items that are not of the lockmethod to be removed */
		if (LOCK_LOCKMETHOD(*lock) != lockmethod)
			goto next_item;

		/* If not allxids, ignore items that are of the wrong xid */
		if (!allxids && xid != holder->tag.xid)
			goto next_item;

		HOLDER_PRINT("LockReleaseAll", holder);
		LOCK_PRINT("LockReleaseAll", lock, 0);
		Assert(lock->nRequested >= 0);
		Assert(lock->nGranted >= 0);
		Assert(lock->nGranted <= lock->nRequested);
		Assert(holder->nHolding >= 0);
		Assert(holder->nHolding <= lock->nRequested);

		/*
		 * fix the general lock stats
		 *
		 */
		if (lock->nRequested != holder->nHolding)
		{
			for (i = 1; i <= numLockModes; i++)
			{
				Assert(holder->holding[i] >= 0);
				if (holder->holding[i] > 0)
				{
					lock->requested[i] -= holder->holding[i];
					lock->granted[i] -= holder->holding[i];
					Assert(lock->requested[i] >= 0 && lock->granted[i] >= 0);
					if (lock->granted[i] == 0)
						lock->grantMask &= BITS_OFF[i];
					/*
					 * Read comments in LockRelease
					 */
					if (!wakeupNeeded &&
						lockMethodTable->ctl->conflictTab[i] & lock->waitMask)
						wakeupNeeded = true;
				}
			}
			lock->nRequested -= holder->nHolding;
			lock->nGranted -= holder->nHolding;
			Assert((lock->nRequested >= 0) && (lock->nGranted >= 0));
			Assert(lock->nGranted <= lock->nRequested);
		}
		else
		{
			/*
			 * This holder accounts for all the requested locks on the object,
			 * so we can be lazy and just zero things out.
			 *
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

		HOLDER_PRINT("LockReleaseAll: deleting", holder);

		/*
		 * Remove the holder entry from the linked lists
		 */
		SHMQueueDelete(&holder->lockLink);
		SHMQueueDelete(&holder->procLink);

		/*
		 * remove the holder entry from the hashtable
		 */
		holder = (HOLDER *) hash_search(lockMethodTable->holderHash,
										(Pointer) holder,
										HASH_REMOVE,
										&found);
		if (!holder || !found)
		{
			SpinRelease(masterLock);
			elog(NOTICE, "LockReleaseAll: holder table corrupted");
			return FALSE;
		}

		if (lock->nRequested == 0)
		{
			/*
			 * We've just released the last lock, so garbage-collect the
			 * lock object.
			 *
			 */
			LOCK_PRINT("LockReleaseAll: deleting", lock, 0);
			Assert(lockMethodTable->lockHash->hash == tag_hash);
			lock = (LOCK *) hash_search(lockMethodTable->lockHash,
										(Pointer) &(lock->tag),
										HASH_REMOVE, &found);
			if (!lock || !found)
			{
				SpinRelease(masterLock);
				elog(NOTICE, "LockReleaseAll: cannot remove lock from HTAB");
				return FALSE;
			}
		}
		else if (wakeupNeeded)
			ProcLockWakeup(lockMethodTable, lock);

next_item:
		holder = nextHolder;
	}

	SpinRelease(masterLock);

#ifdef LOCK_DEBUG
	if (lockmethod == USER_LOCKMETHOD ? Trace_userlocks : Trace_locks)
		elog(DEBUG, "LockReleaseAll: done");
#endif

	return TRUE;
}

int
LockShmemSize(int maxBackends)
{
	int			size = 0;

	size += MAXALIGN(sizeof(PROC_HDR)); /* ProcGlobal */
	size += MAXALIGN(maxBackends * sizeof(PROC));		/* each MyProc */
	size += MAXALIGN(maxBackends * sizeof(LOCKMETHODCTL));		/* each
																 * lockMethodTable->ctl */

	/* lockHash table */
	size += hash_estimate_size(NLOCKENTS(maxBackends),
							   SHMEM_LOCKTAB_KEYSIZE,
							   SHMEM_LOCKTAB_DATASIZE);

	/* holderHash table */
	size += hash_estimate_size(NLOCKENTS(maxBackends),
							   SHMEM_HOLDERTAB_KEYSIZE,
							   SHMEM_HOLDERTAB_DATASIZE);

	/*
	 * Since the lockHash entry count above is only an estimate, add 10%
	 * safety margin.
	 */
	size += size / 10;

	return size;
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
	SHMEM_OFFSET location;
	PROC	   *proc;
	SHM_QUEUE  *procHolders;
	HOLDER	   *holder;
	LOCK	   *lock;
	int			lockmethod = DEFAULT_LOCKMETHOD;
	LOCKMETHODTABLE *lockMethodTable;

	ShmemPIDLookup(MyProcPid, &location);
	if (location == INVALID_OFFSET)
		return;
	proc = (PROC *) MAKE_PTR(location);
	if (proc != MyProc)
		return;
	procHolders = &proc->procHolders;

	Assert(lockmethod < NumLockMethods);
	lockMethodTable = LockMethodTable[lockmethod];
	if (!lockMethodTable)
		return;

	if (proc->waitLock)
		LOCK_PRINT("DumpLocks: waiting on", proc->waitLock, 0);

	holder = (HOLDER *) SHMQueueNext(procHolders, procHolders,
									 offsetof(HOLDER, procLink));

	while (holder)
	{
		Assert(holder->tag.proc == MAKE_OFFSET(proc));

		lock = (LOCK *) MAKE_PTR(holder->tag.lock);

		HOLDER_PRINT("DumpLocks", holder);
		LOCK_PRINT("DumpLocks", lock, 0);

		holder = (HOLDER *) SHMQueueNext(procHolders, &holder->procLink,
										 offsetof(HOLDER, procLink));
	}
}

/*
 * Dump all postgres locks. Must have already acquired the masterLock.
 */
void
DumpAllLocks(void)
{
	SHMEM_OFFSET location;
	PROC	   *proc;
	HOLDER	   *holder = NULL;
	LOCK	   *lock;
	int			pid;
	int			lockmethod = DEFAULT_LOCKMETHOD;
	LOCKMETHODTABLE *lockMethodTable;
	HTAB	   *holderTable;
	HASH_SEQ_STATUS status;

	pid = getpid();
	ShmemPIDLookup(pid, &location);
	if (location == INVALID_OFFSET)
		return;
	proc = (PROC *) MAKE_PTR(location);
	if (proc != MyProc)
		return;

	Assert(lockmethod < NumLockMethods);
	lockMethodTable = LockMethodTable[lockmethod];
	if (!lockMethodTable)
		return;

	holderTable = lockMethodTable->holderHash;

	if (proc->waitLock)
		LOCK_PRINT("DumpAllLocks: waiting on", proc->waitLock, 0);

	hash_seq_init(&status, holderTable);
	while ((holder = (HOLDER *) hash_seq_search(&status)) &&
		   (holder != (HOLDER *) TRUE))
	{
		HOLDER_PRINT("DumpAllLocks", holder);

		if (holder->tag.lock)
		{
			lock = (LOCK *) MAKE_PTR(holder->tag.lock);
			LOCK_PRINT("DumpAllLocks", lock, 0);
		}
		else
			elog(DEBUG, "DumpAllLocks: holder->tag.lock = NULL");
	}
}

#endif /* LOCK_DEBUG */
