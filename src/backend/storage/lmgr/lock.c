/*-------------------------------------------------------------------------
 *
 * lock.c
 *	  POSTGRES low-level lock mechanism
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/lmgr/lock.c,v 1.78 2001/01/16 06:11:34 tgl Exp $
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
 *	LockResolveConflicts(), GrantLock()
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>

#include "postgres.h"

#include "access/xact.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"

static int	WaitOnLock(LOCKMETHOD lockmethod, LOCKMODE lockmode,
					   LOCK *lock, HOLDER *holder);
static void LockCountMyLocks(SHMEM_OFFSET lockOffset, PROC *proc,
							 int *myHolding);
static int LockGetMyHeldLocks(SHMEM_OFFSET lockOffset, PROC *proc);

static char *lock_types[] =
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
 *     trace_locks      -- give a bunch of output what's going on in this file
 *     trace_userlocks  -- same but for user locks
 *     trace_lock_oidmin-- do not trace locks for tables below this oid
 *                         (use to avoid output on system tables)
 *     trace_lock_table -- trace locks on this table (oid) unconditionally
 *     debug_deadlocks  -- currently dumps locks at untimely occasions ;)
 * Furthermore, but in storage/ipc/spin.c:
 *     trace_spinlocks  -- trace spinlocks (pretty useless)
 *
 * Define LOCK_DEBUG at compile time to get all this enabled.
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
             lock->waitProcs.size, lock_types[type]);
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
             "%s: holder(%lx) lock(%lx) tbl(%d) pid(%d) xid(%u) hold(%d,%d,%d,%d,%d,%d,%d)=%d",
             where, MAKE_OFFSET(holderP), holderP->tag.lock,
			 HOLDER_LOCKMETHOD(*(holderP)),
             holderP->tag.pid, holderP->tag.xid,
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

/* -----------------
 * Disable flag
 * -----------------
 */
static bool LockingIsDisabled;

/* -------------------
 * map from lockmethod to the lock table structure
 * -------------------
 */
static LOCKMETHODTABLE *LockMethodTable[MAX_LOCK_METHODS];

static int	NumLockMethods;

/* -------------------
 * InitLocks -- Init the lock module.  Create a private data
 *		structure for constructing conflict masks.
 * -------------------
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

/* -------------------
 * LockDisable -- sets LockingIsDisabled flag to TRUE or FALSE.
 * ------------------
 */
void
LockDisable(bool status)
{
	LockingIsDisabled = status;
}

/* -----------------
 * Boolean function to determine current locking status
 * -----------------
 */
bool
LockingDisabled(void)
{
	return LockingIsDisabled;
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

	/* ------------------------
	 * find/acquire the spinlock for the table
	 * ------------------------
	 */
	SpinAcquire(LockMgrLock);

	/* -----------------------
	 * allocate a control structure from shared memory or attach to it
	 * if it already exists.
	 * -----------------------
	 */
	sprintf(shmemName, "%s (ctl)", tabName);
	lockMethodTable->ctl = (LOCKMETHODCTL *)
		ShmemInitStruct(shmemName, sizeof(LOCKMETHODCTL), &found);

	if (!lockMethodTable->ctl)
		elog(FATAL, "LockMethodTableInit: couldn't initialize %s", tabName);

	/* -------------------
	 * no zero-th table
	 * -------------------
	 */
	NumLockMethods = 1;

	/* ----------------
	 * we're first - initialize
	 * ----------------
	 */
	if (!found)
	{
		MemSet(lockMethodTable->ctl, 0, sizeof(LOCKMETHODCTL));
		lockMethodTable->ctl->masterLock = LockMgrLock;
		lockMethodTable->ctl->lockmethod = NumLockMethods;
	}

	/* --------------------
	 * other modules refer to the lock table by a lockmethod ID
	 * --------------------
	 */
	LockMethodTable[NumLockMethods] = lockMethodTable;
	NumLockMethods++;
	Assert(NumLockMethods <= MAX_LOCK_METHODS);

	/* ----------------------
	 * allocate a hash table for LOCK structs.  This is used
	 * to store per-locked-object information.
	 * ----------------------
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

	/* -------------------------
	 * allocate a hash table for HOLDER structs.  This is used
	 * to store per-lock-holder information.
	 * -------------------------
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
 *		xid.pid							backend pid		backend pid
 *		xid.xid							xid or 0		0
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
			 locktag->objId.blkno, lock_types[lockmode]);
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

	/* --------------------
	 * if it's a new lock object, initialize it
	 * --------------------
	 */
	if (!found)
	{
		lock->grantMask = 0;
		lock->waitMask = 0;
		lock->nRequested = 0;
		lock->nGranted = 0;
		MemSet((char *) lock->requested, 0, sizeof(int) * MAX_LOCKMODES);
		MemSet((char *) lock->granted, 0, sizeof(int) * MAX_LOCKMODES);
		ProcQueueInit(&(lock->waitProcs));
		LOCK_PRINT("LockAcquire: new", lock, lockmode);
	}
	else
	{
		LOCK_PRINT("LockAcquire: found", lock, lockmode);
		Assert((lock->nRequested >= 0) && (lock->requested[lockmode] >= 0));
		Assert((lock->nGranted >= 0) && (lock->granted[lockmode] >= 0));
		Assert(lock->nGranted <= lock->nRequested);
	}

	/* ------------------
	 * Create the hash key for the holder table.
	 * ------------------
	 */
	MemSet(&holdertag, 0, sizeof(HOLDERTAG)); /* must clear padding, needed */
	holdertag.lock = MAKE_OFFSET(lock);
	holdertag.pid = MyProcPid;
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
		elog(NOTICE, "LockAcquire: holder table corrupted");
		return FALSE;
	}

	/*
	 * If new, initialize the new entry
	 */
	if (!found)
	{
		holder->nHolding = 0;
		MemSet((char *) holder->holding, 0, sizeof(int) * MAX_LOCKMODES);
		ProcAddLock(&holder->queue);
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
					 lock_types[i], lock_types[lockmode],
					 lock->tag.relId, lock->tag.dbId, lock->tag.objId.blkno);
				break;
			}
		}
#endif /* CHECK_DEADLOCK_RISK */
	}

	/* ----------------
	 * lock->nRequested and lock->requested[] count the total number of
	 * requests, whether granted or waiting, so increment those immediately.
	 * The other counts don't increment till we get the lock.
	 * ----------------
	 */
	lock->nRequested++;
	lock->requested[lockmode]++;
	Assert((lock->nRequested > 0) && (lock->requested[lockmode] > 0));

	/* --------------------
	 * If I'm the only one holding any lock on this object, then there
	 * cannot be a conflict. The same is true if I already hold this lock.
	 * --------------------
	 */
	if (holder->nHolding == lock->nGranted || holder->holding[lockmode] != 0)
	{
		GrantLock(lock, holder, lockmode);
		HOLDER_PRINT("LockAcquire: owning", holder);
		SpinRelease(masterLock);
		return TRUE;
	}

	/* --------------------
	 * If this process (under any XID) is a holder of the lock,
	 * then there is no conflict, either.
	 * --------------------
	 */
	LockCountMyLocks(holder->tag.lock, MyProc, myHolding);
	if (myHolding[lockmode] != 0)
	{
		GrantLock(lock, holder, lockmode);
		HOLDER_PRINT("LockAcquire: my other XID owning", holder);
		SpinRelease(masterLock);
		return TRUE;
	}

	/*
	 * If lock requested conflicts with locks requested by waiters...
	 */
	if (lockMethodTable->ctl->conflictTab[lockmode] & lock->waitMask)
	{
		/*
		 * If my process doesn't hold any locks that conflict with waiters
		 * then force to sleep, so that prior waiters get first chance.
		 */
		for (i = 1; i <= lockMethodTable->ctl->numLockModes; i++)
		{
			if (myHolding[i] > 0 &&
				lockMethodTable->ctl->conflictTab[i] & lock->waitMask)
				break;			/* yes, there is a conflict */
		}

		if (i > lockMethodTable->ctl->numLockModes)
		{
			HOLDER_PRINT("LockAcquire: another proc already waiting",
						 holder);
			status = STATUS_FOUND;
		}
		else
			status = LockResolveConflicts(lockmethod, lockmode,
										  lock, holder,
										  MyProc, myHolding);
	}
	else
		status = LockResolveConflicts(lockmethod, lockmode,
									  lock, holder,
									  MyProc, myHolding);

	if (status == STATUS_OK)
		GrantLock(lock, holder, lockmode);
	else if (status == STATUS_FOUND)
	{
#ifdef USER_LOCKS

		/*
		 * User locks are non blocking. If we can't acquire a lock we must
		 * remove the holder entry and return FALSE without waiting.
		 */
		if (lockmethod == USER_LOCKMETHOD)
		{
			if (holder->nHolding == 0)
			{
				SHMQueueDelete(&holder->queue);
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

/* ----------------------------
 * LockResolveConflicts -- test for lock conflicts
 *
 * NOTES:
 *		Here's what makes this complicated: one transaction's
 * locks don't conflict with one another.  When many processes
 * hold locks, each has to subtract off the other's locks when
 * determining whether or not any new lock acquired conflicts with
 * the old ones.
 *
 * The caller can optionally pass the process's total holding counts, if
 * known.  If NULL is passed then these values will be computed internally.
 * ----------------------------
 */
int
LockResolveConflicts(LOCKMETHOD lockmethod,
					 LOCKMODE lockmode,
					 LOCK *lock,
					 HOLDER *holder,
					 PROC *proc,
					 int *myHolding)		/* myHolding[] array or NULL */
{
	LOCKMETHODCTL *lockctl = LockMethodTable[lockmethod]->ctl;
	int			numLockModes = lockctl->numLockModes;
	int			bitmask;
	int			i,
				tmpMask;
	int			localHolding[MAX_LOCKMODES];

	Assert((holder->nHolding >= 0) && (holder->holding[lockmode] >= 0));

	/* ----------------------------
	 * first check for global conflicts: If no locks conflict
	 * with mine, then I get the lock.
	 *
	 * Checking for conflict: lock->grantMask represents the types of
	 * currently held locks.  conflictTable[lockmode] has a bit
	 * set for each type of lock that conflicts with mine.	Bitwise
	 * compare tells if there is a conflict.
	 * ----------------------------
	 */
	if (!(lockctl->conflictTab[lockmode] & lock->grantMask))
	{
		HOLDER_PRINT("LockResolveConflicts: no conflict", holder);
		return STATUS_OK;
	}

	/* ------------------------
	 * Rats.  Something conflicts. But it could still be my own
	 * lock.  We have to construct a conflict mask
	 * that does not reflect our own locks.  Locks held by the current
	 * process under another XID also count as "our own locks".
	 * ------------------------
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

	/* ------------------------
	 * now check again for conflicts.  'bitmask' describes the types
	 * of locks held by other processes.  If one of these
	 * conflicts with the kind of lock that I want, there is a
	 * conflict and I have to sleep.
	 * ------------------------
	 */
	if (!(lockctl->conflictTab[lockmode] & bitmask))
	{
		/* no conflict. OK to get the lock */
		HOLDER_PRINT("LockResolveConflicts: resolved", holder);
		return STATUS_OK;
	}

	HOLDER_PRINT("LockResolveConflicts: conflicting", holder);
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
	HOLDER	   *holder = NULL;
	HOLDER	   *nextHolder = NULL;
	SHM_QUEUE  *holderQueue = &(proc->holderQueue);
	SHMEM_OFFSET end = MAKE_OFFSET(holderQueue);
	int			i;

	MemSet(myHolding, 0, MAX_LOCKMODES * sizeof(int));

	if (SHMQueueEmpty(holderQueue))
		return;

	SHMQueueFirst(holderQueue, (Pointer *) &holder, &holder->queue);

	do
	{
		/* ---------------------------
		 * XXX Here we assume the shared memory queue is circular and
		 * that we know its internal structure.  Should have some sort of
		 * macros to allow one to walk it.	mer 20 July 1991
		 * ---------------------------
		 */
		if (holder->queue.next == end)
			nextHolder = NULL;
		else
			SHMQueueFirst(&holder->queue,
						  (Pointer *) &nextHolder, &nextHolder->queue);

		if (lockOffset == holder->tag.lock)
		{
			for (i = 1; i < MAX_LOCKMODES; i++)
			{
				myHolding[i] += holder->holding[i];
			}
		}

		holder = nextHolder;
	} while (holder);
}

/*
 * LockGetMyHeldLocks -- compute bitmask of lock types held by a process
 *		for a given lockable object.
 */
static int
LockGetMyHeldLocks(SHMEM_OFFSET lockOffset, PROC *proc)
{
	int			myHolding[MAX_LOCKMODES];
	int			heldLocks = 0;
	int			i,
				tmpMask;

	LockCountMyLocks(lockOffset, proc, myHolding);

	for (i = 1, tmpMask = 2;
		 i < MAX_LOCKMODES;
		 i++, tmpMask <<= 1)
	{
		if (myHolding[i] > 0)
			heldLocks |= tmpMask;
	}
	return heldLocks;
}

/*
 * GrantLock -- update the lock and holder data structures to show
 *		the lock request has been granted.
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
	 */

	if (ProcSleep(lockMethodTable->ctl,
				  lockmode,
				  lock,
				  holder) != NO_ERROR)
	{
		/* -------------------
		 * We failed as a result of a deadlock, see HandleDeadLock().
		 * Quit now.  Removal of the holder and lock objects, if no longer
		 * needed, will happen in xact cleanup (see above for motivation).
		 * -------------------
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

/*
 * LockRelease -- look up 'locktag' in lock table 'lockmethod' and
 *		release it.
 *
 * Side Effects: if the lock no longer conflicts with the highest
 *		priority waiting process, that process is granted the lock
 *		and awoken. (We have to grant the lock here to avoid a
 *		race between the waking process and any new process to
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
	bool		wakeupNeeded = true;

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
	holdertag.pid = MyProcPid;
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
	Assert(holder->tag.lock == MAKE_OFFSET(lock));

	/*
	 * Check that we are actually holding a lock of the type we want to
	 * release.
	 */
	if (!(holder->holding[lockmode] > 0))
	{
		SpinRelease(masterLock);
		HOLDER_PRINT("LockRelease: WRONGTYPE", holder);
		elog(NOTICE, "LockRelease: you don't own a lock of type %s",
			 lock_types[lockmode]);
		Assert(holder->holding[lockmode] >= 0);
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

#ifdef NOT_USED
	/* --------------------------
	 * If there are still active locks of the type I just released, no one
	 * should be woken up.	Whoever is asleep will still conflict
	 * with the remaining locks.
	 * --------------------------
	 */
	if (lock->granted[lockmode])
		wakeupNeeded = false;
	else
#endif

		/*
		 * Above is not valid any more (due to MVCC lock modes). Actually
		 * we should compare granted[lockmode] with number of
		 * waiters holding lock of this type and try to wakeup only if
		 * these numbers are equal (and lock released conflicts with locks
		 * requested by waiters). For the moment we only check the last
		 * condition.
		 */
	if (lockMethodTable->ctl->conflictTab[lockmode] & lock->waitMask)
		wakeupNeeded = true;

	LOCK_PRINT("LockRelease: updated", lock, lockmode);
	Assert((lock->nRequested >= 0) && (lock->requested[lockmode] >= 0));
	Assert((lock->nGranted >= 0) && (lock->granted[lockmode] >= 0));
	Assert(lock->nGranted <= lock->nRequested);

	if (!lock->nRequested)
	{
		/* ------------------
		 * if there's no one waiting in the queue,
		 * we just released the last lock on this object.
		 * Delete it from the lock table.
		 * ------------------
		 */
		Assert(lockMethodTable->lockHash->hash == tag_hash);
		lock = (LOCK *) hash_search(lockMethodTable->lockHash,
									(Pointer) &(lock->tag),
									HASH_REMOVE,
									&found);
		Assert(lock && found);
		wakeupNeeded = false;
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
	if (!holder->nHolding)
	{
		if (holder->queue.prev == INVALID_OFFSET)
			elog(NOTICE, "LockRelease: holder.prev == INVALID_OFFSET");
		if (holder->queue.next == INVALID_OFFSET)
			elog(NOTICE, "LockRelease: holder.next == INVALID_OFFSET");
		if (holder->queue.next != INVALID_OFFSET)
			SHMQueueDelete(&holder->queue);
		HOLDER_PRINT("LockRelease: deleting", holder);
		holder = (HOLDER *) hash_search(holderTable, (Pointer) &holder,
										HASH_REMOVE_SAVED, &found);
		if (!holder || !found)
		{
			SpinRelease(masterLock);
			elog(NOTICE, "LockRelease: remove holder, table corrupted");
			return FALSE;
		}
	}

	if (wakeupNeeded)
		ProcLockWakeup(lockmethod, lock);
#ifdef LOCK_DEBUG
	else if (LOCK_DEBUG_ENABLED(lock))
        elog(DEBUG, "LockRelease: no wakeup needed");
#endif

	SpinRelease(masterLock);
	return TRUE;
}

/*
 * LockReleaseAll -- Release all locks in a process's lock queue.
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
	HOLDER	   *holder = NULL;
	HOLDER	   *nextHolder = NULL;
	SHM_QUEUE  *holderQueue = &(proc->holderQueue);
	SHMEM_OFFSET end = MAKE_OFFSET(holderQueue);
	SPINLOCK	masterLock;
	LOCKMETHODTABLE *lockMethodTable;
	int			i,
				numLockModes;
	LOCK	   *lock;
	bool		found;
	int			nleft;

#ifdef LOCK_DEBUG
	if (lockmethod == USER_LOCKMETHOD ? Trace_userlocks : Trace_locks)
		elog(DEBUG, "LockReleaseAll: lockmethod=%d, pid=%d",
			 lockmethod, MyProcPid);
#endif

	Assert(lockmethod < NumLockMethods);
	lockMethodTable = LockMethodTable[lockmethod];
	if (!lockMethodTable)
	{
		elog(NOTICE, "LockReleaseAll: bad lockmethod %d", lockmethod);
		return FALSE;
	}

	if (SHMQueueEmpty(holderQueue))
		return TRUE;

	numLockModes = lockMethodTable->ctl->numLockModes;
	masterLock = lockMethodTable->ctl->masterLock;

	SpinAcquire(masterLock);

	SHMQueueFirst(holderQueue, (Pointer *) &holder, &holder->queue);

	nleft = 0;

	do
	{
		bool		wakeupNeeded = false;

		/* ---------------------------
		 * XXX Here we assume the shared memory queue is circular and
		 * that we know its internal structure.  Should have some sort of
		 * macros to allow one to walk it.	mer 20 July 1991
		 * ---------------------------
		 */
		if (holder->queue.next == end)
			nextHolder = NULL;
		else
			SHMQueueFirst(&holder->queue,
						  (Pointer *) &nextHolder, &nextHolder->queue);

		Assert(holder->tag.pid == proc->pid);

		lock = (LOCK *) MAKE_PTR(holder->tag.lock);

		/* Ignore items that are not of the lockmethod to be removed */
		if (LOCK_LOCKMETHOD(*lock) != lockmethod)
		{
			nleft++;
			goto next_item;
		}

		/* If not allxids, ignore items that are of the wrong xid */
		if (!allxids && xid != holder->tag.xid)
		{
			nleft++;
			goto next_item;
		}

		HOLDER_PRINT("LockReleaseAll", holder);
		LOCK_PRINT("LockReleaseAll", lock, 0);
		Assert(lock->nRequested >= 0);
		Assert(lock->nGranted >= 0);
		Assert(lock->nGranted <= lock->nRequested);
		Assert(holder->nHolding >= 0);
		Assert(holder->nHolding <= lock->nRequested);

		/* ------------------
		 * fix the general lock stats
		 * ------------------
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
			/* --------------
			 * set nRequested to zero so that we can garbage collect the lock
			 * down below...
			 * --------------
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
		 * Remove the holder entry from the process' lock queue
		 */
		SHMQueueDelete(&holder->queue);

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

		if (!lock->nRequested)
		{
			/* --------------------
			 * We've just released the last lock, so garbage-collect the
			 * lock object.
			 * --------------------
			 */
			LOCK_PRINT("LockReleaseAll: deleting", lock, 0);
			Assert(lockMethodTable->lockHash->hash == tag_hash);
			lock = (LOCK *) hash_search(lockMethodTable->lockHash,
										(Pointer) &(lock->tag),
										HASH_REMOVE, &found);
			if ((!lock) || (!found))
			{
				SpinRelease(masterLock);
				elog(NOTICE, "LockReleaseAll: cannot remove lock from HTAB");
				return FALSE;
			}
		}
		else if (wakeupNeeded)
			ProcLockWakeup(lockmethod, lock);

next_item:
		holder = nextHolder;
	} while (holder);

	/*
	 * Reinitialize the queue only if nothing has been left in.
	 */
	if (nleft == 0)
	{
#ifdef LOCK_DEBUG
        if (lockmethod == USER_LOCKMETHOD ? Trace_userlocks : Trace_locks)
            elog(DEBUG, "LockReleaseAll: reinitializing holderQueue");
#endif
		SHMQueueInit(holderQueue);
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

/*
 * DeadLockCheck -- Checks for deadlocks for a given process
 *
 * This code takes a list of locks a process holds, and the lock that
 * the process is sleeping on, and tries to find if any of the processes
 * waiting on its locks hold the lock it is waiting for.  If no deadlock
 * is found, it goes on to look at all the processes waiting on their locks.
 *
 * We can't block on user locks, so no sense testing for deadlock
 * because there is no blocking, and no timer for the block.  So,
 * only look at regular locks.
 *
 * We have already locked the master lock before being called.
 */
bool
DeadLockCheck(PROC *thisProc, LOCK *findlock)
{
	HOLDER	   *holder = NULL;
	HOLDER	   *nextHolder = NULL;
	PROC	   *waitProc;
	PROC_QUEUE *waitQueue;
	SHM_QUEUE  *holderQueue = &(thisProc->holderQueue);
	SHMEM_OFFSET end = MAKE_OFFSET(holderQueue);
	LOCKMETHODCTL *lockctl = LockMethodTable[DEFAULT_LOCKMETHOD]->ctl;
	LOCK	   *lock;
	int			i,
				j;
	bool		first_run = (thisProc == MyProc);

	static PROC *checked_procs[MAXBACKENDS];
	static int	nprocs;

	/* initialize at start of recursion */
	if (first_run)
	{
		checked_procs[0] = thisProc;
		nprocs = 1;
	}

	/*
	 * Scan over all the locks held/awaited by thisProc.
	 */
	if (SHMQueueEmpty(holderQueue))
		return false;

	SHMQueueFirst(holderQueue, (Pointer *) &holder, &holder->queue);

	do
	{
		/* ---------------------------
		 * XXX Here we assume the shared memory queue is circular and
		 * that we know its internal structure.  Should have some sort of
		 * macros to allow one to walk it.	mer 20 July 1991
		 * ---------------------------
		 */
		if (holder->queue.next == end)
			nextHolder = NULL;
		else
			SHMQueueFirst(&holder->queue,
						  (Pointer *) &nextHolder, &nextHolder->queue);

		Assert(holder->tag.pid == thisProc->pid);

		lock = (LOCK *) MAKE_PTR(holder->tag.lock);

		/* Ignore user locks */
		if (lock->tag.lockmethod != DEFAULT_LOCKMETHOD)
			goto nxtl;

		HOLDER_PRINT("DeadLockCheck", holder);
		LOCK_PRINT("DeadLockCheck", lock, 0);

		/*
		 * waitLock is always in holderQueue of waiting proc, if !first_run
		 * then upper caller will handle waitProcs queue of waitLock.
		 */
		if (thisProc->waitLock == lock && !first_run)
			goto nxtl;

		/*
		 * If we found proc holding findlock and sleeping on some my other
		 * lock then we have to check does it block me or another waiters.
		 */
		if (lock == findlock && !first_run)
		{
			int			lm;

			Assert(holder->nHolding > 0);
			for (lm = 1; lm <= lockctl->numLockModes; lm++)
			{
				if (holder->holding[lm] > 0 &&
					lockctl->conflictTab[lm] & findlock->waitMask)
					return true;
			}

			/*
			 * Else - get the next lock from thisProc's holderQueue
			 */
			goto nxtl;
		}

		waitQueue = &(lock->waitProcs);
		waitProc = (PROC *) MAKE_PTR(waitQueue->links.prev);

		/*
		 * Inner loop scans over all processes waiting for this lock.
		 *
		 * NOTE: loop must count down because we want to examine each item
		 * in the queue even if waitQueue->size decreases due to waking up
		 * some of the processes.
		 */
		for (i = waitQueue->size; --i >= 0; )
		{
			Assert(waitProc->waitLock == lock);
			if (waitProc == thisProc)
			{
				/* This should only happen at first level */
				Assert(waitProc == MyProc);
				goto nextWaitProc;
			}
			if (lock == findlock)		/* first_run also true */
			{
				/*
				 * If I'm blocked by his heldLocks...
				 */
				if (lockctl->conflictTab[MyProc->waitLockMode] & waitProc->heldLocks)
				{
					/* and he blocked by me -> deadlock */
					if (lockctl->conflictTab[waitProc->waitLockMode] & MyProc->heldLocks)
						return true;
					/* we shouldn't look at holderQueue of our blockers */
					goto nextWaitProc;
				}

				/*
				 * If he isn't blocked by me and we request
				 * non-conflicting lock modes - no deadlock here because
				 * he isn't blocked by me in any sense (explicitly or
				 * implicitly). Note that we don't do like test if
				 * !first_run (when thisProc is holder and non-waiter on
				 * lock) and so we call DeadLockCheck below for every
				 * waitProc in thisProc->holderQueue, even for waitProc-s
				 * un-blocked by thisProc. Should we? This could save us
				 * some time...
				 */
				if (!(lockctl->conflictTab[waitProc->waitLockMode] & MyProc->heldLocks) &&
					!(lockctl->conflictTab[waitProc->waitLockMode] & (1 << MyProc->waitLockMode)))
					goto nextWaitProc;
			}

			/*
			 * Skip this waiter if already checked.
			 */
			for (j = 0; j < nprocs; j++)
			{
				if (checked_procs[j] == waitProc)
					goto nextWaitProc;
			}

			/* Recursively check this process's holderQueue. */
			Assert(nprocs < MAXBACKENDS);
			checked_procs[nprocs++] = waitProc;

			if (DeadLockCheck(waitProc, findlock))
			{
				int			heldLocks;

				/*
				 * Ok, but is waitProc waiting for me (thisProc) ?
				 */
				if (thisProc->waitLock == lock)
				{
					Assert(first_run);
					heldLocks = thisProc->heldLocks;
				}
				else
				{
					/* should we cache heldLocks to speed this up? */
					heldLocks = LockGetMyHeldLocks(holder->tag.lock, thisProc);
					Assert(heldLocks != 0);
				}
				if (lockctl->conflictTab[waitProc->waitLockMode] & heldLocks)
				{
					/*
					 * Last attempt to avoid deadlock: try to wakeup myself.
					 */
					if (first_run)
					{
						if (LockResolveConflicts(DEFAULT_LOCKMETHOD,
												 MyProc->waitLockMode,
												 MyProc->waitLock,
												 MyProc->waitHolder,
												 MyProc,
												 NULL) == STATUS_OK)
						{
							GrantLock(MyProc->waitLock,
									  MyProc->waitHolder,
									  MyProc->waitLockMode);
							ProcWakeup(MyProc, NO_ERROR);
							return false;
						}
					}
					return true;
				}

				/*
				 * Hell! Is he blocked by any (other) holder ?
				 */
				if (LockResolveConflicts(DEFAULT_LOCKMETHOD,
										 waitProc->waitLockMode,
										 lock,
										 waitProc->waitHolder,
										 waitProc,
										 NULL) != STATUS_OK)
				{
					/*
					 * Blocked by others - no deadlock...
					 */
					LOCK_PRINT("DeadLockCheck: blocked by others",
							   lock, waitProc->waitLockMode);
					goto nextWaitProc;
				}

				/*
				 * Well - wakeup this guy! This is the case of
				 * implicit blocking: thisProc blocked someone who
				 * blocked waitProc by the fact that he/someone is
				 * already waiting for lock.  We do this for
				 * anti-starving.
				 */
				GrantLock(lock, waitProc->waitHolder, waitProc->waitLockMode);
				waitProc = ProcWakeup(waitProc, NO_ERROR);
				/*
				 * Use next-proc link returned by ProcWakeup, since this
				 * proc's own links field is now cleared.
				 */
				continue;
			}

nextWaitProc:
			waitProc = (PROC *) MAKE_PTR(waitProc->links.prev);
		}

nxtl:
		holder = nextHolder;
	} while (holder);

	/* if we got here, no deadlock */
	return false;
}

#ifdef LOCK_DEBUG
/*
 * Dump all locks in the proc->holderQueue. Must have already acquired
 * the masterLock.
 */
void
DumpLocks(void)
{
	SHMEM_OFFSET location;
	PROC	   *proc;
	SHM_QUEUE  *holderQueue;
	HOLDER	   *holder = NULL;
	HOLDER	   *nextHolder = NULL;
	SHMEM_OFFSET end;
	LOCK	   *lock;
	int			lockmethod = DEFAULT_LOCKMETHOD;
	LOCKMETHODTABLE *lockMethodTable;

	ShmemPIDLookup(MyProcPid, &location);
	if (location == INVALID_OFFSET)
		return;
	proc = (PROC *) MAKE_PTR(location);
	if (proc != MyProc)
		return;
	holderQueue = &proc->holderQueue;
	end = MAKE_OFFSET(holderQueue);

	Assert(lockmethod < NumLockMethods);
	lockMethodTable = LockMethodTable[lockmethod];
	if (!lockMethodTable)
		return;

	if (proc->waitLock)
		LOCK_PRINT("DumpLocks: waiting on", proc->waitLock, 0);

	if (SHMQueueEmpty(holderQueue))
		return;

	SHMQueueFirst(holderQueue, (Pointer *) &holder, &holder->queue);

	do
	{
		/* ---------------------------
		 * XXX Here we assume the shared memory queue is circular and
		 * that we know its internal structure.  Should have some sort of
		 * macros to allow one to walk it.	mer 20 July 1991
		 * ---------------------------
		 */
		if (holder->queue.next == end)
			nextHolder = NULL;
		else
			SHMQueueFirst(&holder->queue,
						  (Pointer *) &nextHolder, &nextHolder->queue);

		Assert(holder->tag.pid == proc->pid);

		lock = (LOCK *) MAKE_PTR(holder->tag.lock);

		HOLDER_PRINT("DumpLocks", holder);
		LOCK_PRINT("DumpLocks", lock, 0);

		holder = nextHolder;
	} while (holder);
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
