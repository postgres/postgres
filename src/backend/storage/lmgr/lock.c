/*-------------------------------------------------------------------------
 *
 * lock.c
 *	  simple lock acquisition
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/lmgr/lock.c,v 1.72 2000/11/08 22:10:00 tgl Exp $
 *
 * NOTES
 *	  Outside modules can create a lock table and acquire/release
 *	  locks.  A lock table is a shared memory hash table.  When
 *	  a process tries to acquire a lock of a type that conflictRs
 *	  with existing locks, it is put to sleep using the routines
 *	  in storage/lmgr/proc.c.
 *
 *	Interface:
 *
 *	LockAcquire(), LockRelease(), LockMethodTableInit(),
 *	LockMethodTableRename(), LockReleaseAll,
 *	LockResolveConflicts(), GrantLock()
 *
 *	NOTE: This module is used to define new lock tables.  The
 *		multi-level lock table (multi.c) used by the heap
 *		access methods calls these routines.  See multi.c for
 *		examples showing how to use this interface.
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

static int	WaitOnLock(LOCKMETHOD lockmethod, LOCK *lock, LOCKMODE lockmode);

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
         && (lock->tag.relId >= Trace_lock_oidmin))
        || (Trace_lock_table && (lock->tag.relId == Trace_lock_table));
}


inline static void
LOCK_PRINT(const char * where, const LOCK * lock, LOCKMODE type)
{
	if (LOCK_DEBUG_ENABLED(lock))
        elog(DEBUG,
             "%s: lock(%lx) tbl(%d) rel(%u) db(%u) obj(%u) mask(%x) "
             "hold(%d,%d,%d,%d,%d,%d,%d)=%d "
             "act(%d,%d,%d,%d,%d,%d,%d)=%d wait(%d) type(%s)",
             where, MAKE_OFFSET(lock),
             lock->tag.lockmethod, lock->tag.relId, lock->tag.dbId,
             lock->tag.objId.blkno, lock->mask,
             lock->holders[1], lock->holders[2], lock->holders[3], lock->holders[4],
             lock->holders[5], lock->holders[6], lock->holders[7], lock->nHolding,
             lock->activeHolders[1], lock->activeHolders[2], lock->activeHolders[3],
             lock->activeHolders[4], lock->activeHolders[5], lock->activeHolders[6],
             lock->activeHolders[7], lock->nActive,
             lock->waitProcs.size, lock_types[type]);
}


inline static void
XID_PRINT(const char * where, const XIDLookupEnt * xidentP)
{
	if (
        (((XIDENT_LOCKMETHOD(*xidentP) == DEFAULT_LOCKMETHOD && Trace_locks)
          || (XIDENT_LOCKMETHOD(*xidentP) == USER_LOCKMETHOD && Trace_userlocks))
         && (((LOCK *)MAKE_PTR(xidentP->tag.lock))->tag.relId >= Trace_lock_oidmin))
		|| (Trace_lock_table && (((LOCK *)MAKE_PTR(xidentP->tag.lock))->tag.relId == Trace_lock_table))
        )
        elog(DEBUG,
             "%s: xid(%lx) lock(%lx) tbl(%d) pid(%d) xid(%u) hold(%d,%d,%d,%d,%d,%d,%d)=%d",
             where, MAKE_OFFSET(xidentP), xidentP->tag.lock, XIDENT_LOCKMETHOD(*(xidentP)),
             xidentP->tag.pid, xidentP->tag.xid,
             xidentP->holders[1], xidentP->holders[2], xidentP->holders[3], xidentP->holders[4],
             xidentP->holders[5], xidentP->holders[6], xidentP->holders[7], xidentP->nHolding);
}

#else  /* not LOCK_DEBUG */

#define LOCK_PRINT(where, lock, type)
#define XID_PRINT(where, xidentP)

#endif /* not LOCK_DEBUG */



SPINLOCK	LockMgrLock;		/* in Shmem or created in
								 * CreateSpinlocks() */

/* This is to simplify/speed up some bit arithmetic */

static LOCKMASK BITS_OFF[MAX_LOCKMODES];
static LOCKMASK BITS_ON[MAX_LOCKMODES];

/* -----------------
 * XXX Want to move this to this file
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
InitLocks()
{
	int			i;
	int			bit;

	bit = 1;
	/* -------------------
	 * remember 0th lockmode is invalid
	 * -------------------
	 */
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
LockDisable(int status)
{
	LockingIsDisabled = status;
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
					int numModes)
{
	LOCKMETHODTABLE *lockMethodTable;
	char	   *shmemName;
	HASHCTL		info;
	int			hash_flags;
	bool		found;
	int			status = TRUE;

	if (numModes > MAX_LOCKMODES)
	{
		elog(NOTICE, "LockMethodTableInit: too many lock types %d greater than %d",
			 numModes, MAX_LOCKMODES);
		return INVALID_LOCKMETHOD;
	}

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
	{
		elog(FATAL, "LockMethodTableInit: couldn't initialize %s", tabName);
		status = FALSE;
	}

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
	 * other modules refer to the lock table by a lockmethod
	 * --------------------
	 */
	LockMethodTable[NumLockMethods] = lockMethodTable;
	NumLockMethods++;
	Assert(NumLockMethods <= MAX_LOCK_METHODS);

	/* ----------------------
	 * allocate a hash table for the lock tags.  This is used
	 * to find the different locks.
	 * ----------------------
	 */
	info.keysize = SHMEM_LOCKTAB_KEYSIZE;
	info.datasize = SHMEM_LOCKTAB_DATASIZE;
	info.hash = tag_hash;
	hash_flags = (HASH_ELEM | HASH_FUNCTION);

	sprintf(shmemName, "%s (lock hash)", tabName);
	lockMethodTable->lockHash = (HTAB *) ShmemInitHash(shmemName,
										 INIT_TABLE_SIZE, MAX_TABLE_SIZE,
													   &info, hash_flags);

	Assert(lockMethodTable->lockHash->hash == tag_hash);
	if (!lockMethodTable->lockHash)
	{
		elog(FATAL, "LockMethodTableInit: couldn't initialize %s", tabName);
		status = FALSE;
	}

	/* -------------------------
	 * allocate an xid table.  When different transactions hold
	 * the same lock, additional information must be saved (locks per tx).
	 * -------------------------
	 */
	info.keysize = SHMEM_XIDTAB_KEYSIZE;
	info.datasize = SHMEM_XIDTAB_DATASIZE;
	info.hash = tag_hash;
	hash_flags = (HASH_ELEM | HASH_FUNCTION);

	sprintf(shmemName, "%s (xid hash)", tabName);
	lockMethodTable->xidHash = (HTAB *) ShmemInitHash(shmemName,
										 INIT_TABLE_SIZE, MAX_TABLE_SIZE,
													  &info, hash_flags);

	if (!lockMethodTable->xidHash)
	{
		elog(FATAL, "LockMethodTableInit: couldn't initialize %s", tabName);
		status = FALSE;
	}

	/* init ctl data structures */
	LockMethodInit(lockMethodTable, conflictsP, prioP, numModes);

	SpinRelease(LockMgrLock);

	pfree(shmemName);

	if (status)
		return lockMethodTable->ctl->lockmethod;
	else
		return INVALID_LOCKMETHOD;
}

/*
 * LockMethodTableRename -- allocate another lockmethod to the same
 *		lock table.
 *
 * NOTES: Both the lock module and the lock chain (lchain.c)
 *		module use table id's to distinguish between different
 *		kinds of locks.  Short term and long term locks look
 *		the same to the lock table, but are handled differently
 *		by the lock chain manager.	This function allows the
 *		client to use different lockmethods when acquiring/releasing
 *		short term and long term locks.
 */

LOCKMETHOD
LockMethodTableRename(LOCKMETHOD lockmethod)
{
	LOCKMETHOD	newLockMethod;

	if (NumLockMethods >= MAX_LOCK_METHODS)
		return INVALID_LOCKMETHOD;
	if (LockMethodTable[lockmethod] == INVALID_LOCKMETHOD)
		return INVALID_LOCKMETHOD;

	/* other modules refer to the lock table by a lockmethod */
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
#ifdef USER_LOCKS
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
 *		for the following differences:
 *
 *										normal lock		user lock
 *
 *		lockmethod						1				2
 *		tag.relId						rel oid			0
 *		tag.ItemPointerData.ip_blkid	block id		lock id2
 *		tag.ItemPointerData.ip_posid	tuple offset	lock id1
 *		xid.pid							0				backend pid
 *		xid.xid							xid or 0		0
 *		persistence						transaction		user or backend
 *
 *		The lockmode parameter can have the same values for normal locks
 *		although probably only WRITE_LOCK can have some practical use.
 *
 *														DZ - 22 Nov 1997
#endif
 */

bool
LockAcquire(LOCKMETHOD lockmethod, LOCKTAG *locktag, LOCKMODE lockmode)
{
	XIDLookupEnt *xident,
				item;
	HTAB	   *xidTable;
	bool		found;
	LOCK	   *lock = NULL;
	SPINLOCK	masterLock;
	LOCKMETHODTABLE *lockMethodTable;
	int			status;
	TransactionId xid;

#ifdef LOCK_DEBUG
	if (lockmethod == USER_LOCKMETHOD && Trace_userlocks)
		elog(DEBUG, "LockAcquire: user lock [%u] %s", locktag->objId.blkno, lock_types[lockmode]);
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
	 * if there was nothing else there, complete initialization
	 * --------------------
	 */
	if (!found)
	{
		lock->mask = 0;
		lock->nHolding = 0;
		lock->nActive = 0;
		MemSet((char *) lock->holders, 0, sizeof(int) * MAX_LOCKMODES);
		MemSet((char *) lock->activeHolders, 0, sizeof(int) * MAX_LOCKMODES);
		ProcQueueInit(&(lock->waitProcs));
		Assert(lock->tag.objId.blkno == locktag->objId.blkno);
		LOCK_PRINT("LockAcquire: new", lock, lockmode);
	}
	else
	{
		LOCK_PRINT("LockAcquire: found", lock, lockmode);
		Assert((lock->nHolding > 0) && (lock->holders[lockmode] >= 0));
		Assert((lock->nActive > 0) && (lock->activeHolders[lockmode] >= 0));
		Assert(lock->nActive <= lock->nHolding);
	}

	/* ------------------
	 * add an element to the lock queue so that we can clear the
	 * locks at end of transaction.
	 * ------------------
	 */
	xidTable = lockMethodTable->xidHash;

	/* ------------------
	 * Zero out all of the tag bytes (this clears the padding bytes for long
	 * word alignment and ensures hashing consistency).
	 * ------------------
	 */
	MemSet(&item, 0, XID_TAGSIZE);		/* must clear padding, needed */
	item.tag.lock = MAKE_OFFSET(lock);
#ifdef USE_XIDTAG_LOCKMETHOD
	item.tag.lockmethod = lockmethod;
#endif

#ifdef USER_LOCKS
	if (lockmethod == USER_LOCKMETHOD)
	{
		item.tag.pid = MyProcPid;
		item.tag.xid = xid = 0;
	}
	else
	{
		xid = GetCurrentTransactionId();
		TransactionIdStore(xid, &item.tag.xid);
	}
#else  /* not USER_LOCKS */
	xid = GetCurrentTransactionId();
	TransactionIdStore(xid, &item.tag.xid);
#endif /* not USER_LOCKS */

	/*
	 * Find or create an xid entry with this tag
	 */
	xident = (XIDLookupEnt *) hash_search(xidTable, (Pointer) &item,
										  HASH_ENTER, &found);
	if (!xident)
	{
		SpinRelease(masterLock);
		elog(NOTICE, "LockAcquire: xid table corrupted");
		return FALSE;
	}

	/*
	 * If not found initialize the new entry
	 */
	if (!found)
	{
		xident->nHolding = 0;
		MemSet((char *) xident->holders, 0, sizeof(int) * MAX_LOCKMODES);
		ProcAddLock(&xident->queue);
		XID_PRINT("LockAcquire: new", xident);
	}
	else
	{
		int			i;

		XID_PRINT("LockAcquire: found", xident);
		Assert((xident->nHolding > 0) && (xident->holders[lockmode] >= 0));
		Assert(xident->nHolding <= lock->nActive);
		/*
		 * Issue warning if we already hold a lower-level lock on this
		 * object and do not hold a lock of the requested level or higher.
		 * This indicates a deadlock-prone coding practice (eg, we'd have
		 * a deadlock if another backend were following the same code path
		 * at about the same time).
		 *
		 * XXX Doing numeric comparison on the lockmodes is a hack;
		 * it'd be better to use a table.  For now, though, this works.
		 */
		for (i = lockMethodTable->ctl->numLockModes; i > 0; i--)
		{
			if (xident->holders[i] > 0)
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
	}

	/* ----------------
	 * lock->nholding tells us how many processes have _tried_ to
	 * acquire this lock,  Regardless of whether they succeeded or
	 * failed in doing so.
	 * ----------------
	 */
	lock->nHolding++;
	lock->holders[lockmode]++;
	Assert((lock->nHolding > 0) && (lock->holders[lockmode] > 0));

	/* --------------------
	 * If I'm the only one holding a lock, then there
	 * cannot be a conflict. The same is true if we already
	 * hold this lock.
	 * --------------------
	 */
	if (xident->nHolding == lock->nActive || xident->holders[lockmode] != 0)
	{
		xident->holders[lockmode]++;
		xident->nHolding++;
		XID_PRINT("LockAcquire: owning", xident);
		Assert((xident->nHolding > 0) && (xident->holders[lockmode] > 0));
		GrantLock(lock, lockmode);
		SpinRelease(masterLock);
		return TRUE;
	}

	/*
	 * If lock requested conflicts with locks requested by waiters...
	 */
	if (lockMethodTable->ctl->conflictTab[lockmode] & lock->waitMask)
	{
		int			i = 1;

		/*
		 * If I don't hold locks or my locks don't conflict with waiters
		 * then force to sleep.
		 */
		if (xident->nHolding > 0)
		{
			for (; i <= lockMethodTable->ctl->numLockModes; i++)
			{
				if (xident->holders[i] > 0 &&
					lockMethodTable->ctl->conflictTab[i] & lock->waitMask)
					break;		/* conflict */
			}
		}

		if (xident->nHolding == 0 || i > lockMethodTable->ctl->numLockModes)
		{
			XID_PRINT("LockAcquire: higher priority proc waiting",
					  xident);
			status = STATUS_FOUND;
		}
		else
			status = LockResolveConflicts(lockmethod, lock, lockmode, xid, xident);
	}
	else
		status = LockResolveConflicts(lockmethod, lock, lockmode, xid, xident);

	if (status == STATUS_OK)
		GrantLock(lock, lockmode);
	else if (status == STATUS_FOUND)
	{
#ifdef USER_LOCKS

		/*
		 * User locks are non blocking. If we can't acquire a lock we must
		 * remove the xid entry and return FALSE without waiting.
		 */
		if (lockmethod == USER_LOCKMETHOD)
		{
			if (!xident->nHolding)
			{
				SHMQueueDelete(&xident->queue);
				xident = (XIDLookupEnt *) hash_search(xidTable,
													  (Pointer) xident,
													HASH_REMOVE, &found);
				if (!xident || !found)
					elog(NOTICE, "LockAcquire: remove xid, table corrupted");
			}
			else
				XID_PRINT("LockAcquire: NHOLDING", xident);
			lock->nHolding--;
			lock->holders[lockmode]--;
			LOCK_PRINT("LockAcquire: user lock failed", lock, lockmode);
			Assert((lock->nHolding > 0) && (lock->holders[lockmode] >= 0));
			Assert(lock->nActive <= lock->nHolding);
			SpinRelease(masterLock);
			return FALSE;
		}
#endif /* USER_LOCKS */

		/*
		 * Construct bitmask of locks we hold before going to sleep.
		 */
		MyProc->holdLock = 0;
		if (xident->nHolding > 0)
		{
			int			i,
						tmpMask = 2;

			for (i = 1; i <= lockMethodTable->ctl->numLockModes;
				 i++, tmpMask <<= 1)
			{
				if (xident->holders[i] > 0)
					MyProc->holdLock |= tmpMask;
			}
			Assert(MyProc->holdLock != 0);
		}

		status = WaitOnLock(lockmethod, lock, lockmode);

		/*
		 * Check the xid entry status, in case something in the ipc
		 * communication doesn't work correctly.
		 */
		if (!((xident->nHolding > 0) && (xident->holders[lockmode] > 0)))
		{
			XID_PRINT("LockAcquire: INCONSISTENT", xident);
			LOCK_PRINT("LockAcquire: INCONSISTENT", lock, lockmode);
			/* Should we retry ? */
			SpinRelease(masterLock);
			return FALSE;
		}
		XID_PRINT("LockAcquire: granted", xident);
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
 * ----------------------------
 */
int
LockResolveConflicts(LOCKMETHOD lockmethod,
					 LOCK *lock,
					 LOCKMODE lockmode,
					 TransactionId xid,
					 XIDLookupEnt *xidentP)		/* xident ptr or NULL */
{
	XIDLookupEnt *xident,
				item;
	int		   *myHolders;
	int			numLockModes;
	HTAB	   *xidTable;
	bool		found;
	int			bitmask;
	int			i,
				tmpMask;

	numLockModes = LockMethodTable[lockmethod]->ctl->numLockModes;
	xidTable = LockMethodTable[lockmethod]->xidHash;

	if (xidentP)
	{

		/*
		 * A pointer to the xid entry was supplied from the caller.
		 * Actually only LockAcquire can do it.
		 */
		xident = xidentP;
	}
	else
	{
		/* ---------------------
		 * read my own statistics from the xid table.  If there
		 * isn't an entry, then we'll just add one.
		 *
		 * Zero out the tag, this clears the padding bytes for long
		 * word alignment and ensures hashing consistency.
		 * ------------------
		 */
		MemSet(&item, 0, XID_TAGSIZE);
		item.tag.lock = MAKE_OFFSET(lock);
#ifdef USE_XIDTAG_LOCKMETHOD
		item.tag.lockmethod = lockmethod;
#endif
#ifdef USER_LOCKS
		if (lockmethod == USER_LOCKMETHOD)
		{
			item.tag.pid = MyProcPid;
			item.tag.xid = 0;
		}
		else
#endif
            TransactionIdStore(xid, &item.tag.xid);

		/*
		 * Find or create an xid entry with this tag
		 */
		xident = (XIDLookupEnt *) hash_search(xidTable, (Pointer) &item,
											  HASH_ENTER, &found);
		if (!xident)
		{
			elog(NOTICE, "LockResolveConflicts: xid table corrupted");
			return STATUS_ERROR;
		}

		/*
		 * If not found initialize the new entry. THIS SHOULD NEVER
		 * HAPPEN, if we are trying to resolve a conflict we must already
		 * have allocated an xid entry for this lock.	 dz 21-11-1997
		 */
		if (!found)
		{
			/* ---------------
			 * we're not holding any type of lock yet.  Clear
			 * the lock stats.
			 * ---------------
			 */
			MemSet(xident->holders, 0, numLockModes * sizeof(*(lock->holders)));
			xident->nHolding = 0;
			XID_PRINT("LockResolveConflicts: NOT FOUND", xident);
		}
		else
			XID_PRINT("LockResolveConflicts: found", xident);
	}
	Assert((xident->nHolding >= 0) && (xident->holders[lockmode] >= 0));

	/* ----------------------------
	 * first check for global conflicts: If no locks conflict
	 * with mine, then I get the lock.
	 *
	 * Checking for conflict: lock->mask represents the types of
	 * currently held locks.  conflictTable[lockmode] has a bit
	 * set for each type of lock that conflicts with mine.	Bitwise
	 * compare tells if there is a conflict.
	 * ----------------------------
	 */
	if (!(LockMethodTable[lockmethod]->ctl->conflictTab[lockmode] & lock->mask))
	{
		xident->holders[lockmode]++;
		xident->nHolding++;
		XID_PRINT("LockResolveConflicts: no conflict", xident);
		Assert((xident->nHolding > 0) && (xident->holders[lockmode] > 0));
		return STATUS_OK;
	}

	/* ------------------------
	 * Rats.  Something conflicts. But it could still be my own
	 * lock.  We have to construct a conflict mask
	 * that does not reflect our own locks.
	 * ------------------------
	 */
	myHolders = xident->holders;
	bitmask = 0;
	tmpMask = 2;
	for (i = 1; i <= numLockModes; i++, tmpMask <<= 1)
	{
		if (lock->activeHolders[i] != myHolders[i])
			bitmask |= tmpMask;
	}

	/* ------------------------
	 * now check again for conflicts.  'bitmask' describes the types
	 * of locks held by other processes.  If one of these
	 * conflicts with the kind of lock that I want, there is a
	 * conflict and I have to sleep.
	 * ------------------------
	 */
	if (!(LockMethodTable[lockmethod]->ctl->conflictTab[lockmode] & bitmask))
	{
		/* no conflict. Get the lock and go on */
		xident->holders[lockmode]++;
		xident->nHolding++;
		XID_PRINT("LockResolveConflicts: resolved", xident);
		Assert((xident->nHolding > 0) && (xident->holders[lockmode] > 0));
		return STATUS_OK;
	}

	XID_PRINT("LockResolveConflicts: conflicting", xident);
	return STATUS_FOUND;
}

/*
 * GrantLock -- update the lock data structure to show
 *		the new lock holder.
 */
void
GrantLock(LOCK *lock, LOCKMODE lockmode)
{
	lock->nActive++;
	lock->activeHolders[lockmode]++;
	lock->mask |= BITS_ON[lockmode];
	LOCK_PRINT("GrantLock", lock, lockmode);
	Assert((lock->nActive > 0) && (lock->activeHolders[lockmode] > 0));
	Assert(lock->nActive <= lock->nHolding);
}

static int
WaitOnLock(LOCKMETHOD lockmethod, LOCK *lock, LOCKMODE lockmode)
{
	PROC_QUEUE *waitQueue = &(lock->waitProcs);
	LOCKMETHODTABLE *lockMethodTable = LockMethodTable[lockmethod];
	char		*new_status, *old_status;

	Assert(lockmethod < NumLockMethods);

	/*
	 * the waitqueue is ordered by priority. I insert myself according to
	 * the priority of the lock I am acquiring.
	 *
	 * SYNC NOTE: I am assuming that the lock table spinlock is sufficient
	 * synchronization for this queue.	That will not be true if/when
	 * people can be deleted from the queue by a SIGINT or something.
	 */
	LOCK_PRINT("WaitOnLock: sleeping on lock", lock, lockmode);

	old_status = pstrdup(get_ps_display());
	new_status = (char *) palloc(strlen(get_ps_display()) + 10);
	strcpy(new_status, get_ps_display());
	strcat(new_status, " waiting");
	set_ps_display(new_status);

	if (ProcSleep(waitQueue,
				  lockMethodTable->ctl,
				  lockmode,
				  lock) != NO_ERROR)
	{
		/* -------------------
		 * This could have happend as a result of a deadlock,
		 * see HandleDeadLock().
		 * Decrement the lock nHolding and holders fields as
		 * we are no longer waiting on this lock.
		 * -------------------
		 */
		lock->nHolding--;
		lock->holders[lockmode]--;
		LOCK_PRINT("WaitOnLock: aborting on lock", lock, lockmode);
		Assert((lock->nHolding >= 0) && (lock->holders[lockmode] >= 0));
		Assert(lock->nActive <= lock->nHolding);
		if (lock->activeHolders[lockmode] == lock->holders[lockmode])
			lock->waitMask &= BITS_OFF[lockmode];
		SpinRelease(lockMethodTable->ctl->masterLock);
		elog(ERROR, "WaitOnLock: error on wakeup - Aborting this transaction");

		/* not reached */
	}

	if (lock->activeHolders[lockmode] == lock->holders[lockmode])
		lock->waitMask &= BITS_OFF[lockmode];

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
 *		come along and request the lock).
 */
bool
LockRelease(LOCKMETHOD lockmethod, LOCKTAG *locktag, LOCKMODE lockmode)
{
	LOCK	   *lock = NULL;
	SPINLOCK	masterLock;
	bool		found;
	LOCKMETHODTABLE *lockMethodTable;
	XIDLookupEnt *xident,
				item;
	HTAB	   *xidTable;
	TransactionId xid;
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
	Assert((lock->nHolding > 0) && (lock->holders[lockmode] >= 0));
	Assert((lock->nActive > 0) && (lock->activeHolders[lockmode] >= 0));
	Assert(lock->nActive <= lock->nHolding);


	/* ------------------
	 * Zero out all of the tag bytes (this clears the padding bytes for long
	 * word alignment and ensures hashing consistency).
	 * ------------------
	 */
	MemSet(&item, 0, XID_TAGSIZE);
	item.tag.lock = MAKE_OFFSET(lock);
#ifdef USE_XIDTAG_LOCKMETHOD
	item.tag.lockmethod = lockmethod;
#endif
#ifdef USER_LOCKS
	if (lockmethod == USER_LOCKMETHOD)
	{
		item.tag.pid = MyProcPid;
		item.tag.xid = xid = 0;
	}
	else
	{
		xid = GetCurrentTransactionId();
		TransactionIdStore(xid, &item.tag.xid);
	}
#else
	xid = GetCurrentTransactionId();
	TransactionIdStore(xid, &item.tag.xid);
#endif

	/*
	 * Find an xid entry with this tag
	 */
	xidTable = lockMethodTable->xidHash;
	xident = (XIDLookupEnt *) hash_search(xidTable, (Pointer) &item,
										  HASH_FIND_SAVE, &found);
	if (!xident || !found)
	{
		SpinRelease(masterLock);
#ifdef USER_LOCKS
		if (!found && lockmethod == USER_LOCKMETHOD)
            elog(NOTICE, "LockRelease: no lock with this tag");
		else
#endif
			elog(NOTICE, "LockRelease: xid table corrupted");
		return FALSE;
	}
	XID_PRINT("LockRelease: found", xident);
	Assert(xident->tag.lock == MAKE_OFFSET(lock));

	/*
	 * Check that we are actually holding a lock of the type we want to
	 * release.
	 */
	if (!(xident->holders[lockmode] > 0))
	{
		SpinRelease(masterLock);
		XID_PRINT("LockAcquire: WRONGTYPE", xident);
		elog(NOTICE, "LockRelease: you don't own a lock of type %s",
			 lock_types[lockmode]);
		Assert(xident->holders[lockmode] >= 0);
		return FALSE;
	}
	Assert(xident->nHolding > 0);

	/*
	 * fix the general lock stats
	 */
	lock->nHolding--;
	lock->holders[lockmode]--;
	lock->nActive--;
	lock->activeHolders[lockmode]--;

#ifdef NOT_USED
	/* --------------------------
	 * If there are still active locks of the type I just released, no one
	 * should be woken up.	Whoever is asleep will still conflict
	 * with the remaining locks.
	 * --------------------------
	 */
	if (lock->activeHolders[lockmode])
		wakeupNeeded = false;
	else
#endif

		/*
		 * Above is not valid any more (due to MVCC lock modes). Actually
		 * we should compare activeHolders[lockmode] with number of
		 * waiters holding lock of this type and try to wakeup only if
		 * these numbers are equal (and lock released conflicts with locks
		 * requested by waiters). For the moment we only check the last
		 * condition.
		 */
	if (lockMethodTable->ctl->conflictTab[lockmode] & lock->waitMask)
		wakeupNeeded = true;

	if (!(lock->activeHolders[lockmode]))
	{
		/* change the conflict mask.  No more of this lock type. */
		lock->mask &= BITS_OFF[lockmode];
	}

	LOCK_PRINT("LockRelease: updated", lock, lockmode);
	Assert((lock->nHolding >= 0) && (lock->holders[lockmode] >= 0));
	Assert((lock->nActive >= 0) && (lock->activeHolders[lockmode] >= 0));
	Assert(lock->nActive <= lock->nHolding);

	if (!lock->nHolding)
	{
		/* ------------------
		 * if there's no one waiting in the queue,
		 * we just released the last lock.
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
	 * now check to see if I have any private locks.  If I do, decrement
	 * the counts associated with them.
	 */
	xident->holders[lockmode]--;
	xident->nHolding--;
	XID_PRINT("LockRelease: updated", xident);
	Assert((xident->nHolding >= 0) && (xident->holders[lockmode] >= 0));

	/*
	 * If this was my last hold on this lock, delete my entry in the XID
	 * table.
	 */
	if (!xident->nHolding)
	{
		if (xident->queue.prev == INVALID_OFFSET)
			elog(NOTICE, "LockRelease: xid.prev == INVALID_OFFSET");
		if (xident->queue.next == INVALID_OFFSET)
			elog(NOTICE, "LockRelease: xid.next == INVALID_OFFSET");
		if (xident->queue.next != INVALID_OFFSET)
			SHMQueueDelete(&xident->queue);
		XID_PRINT("LockRelease: deleting", xident);
		xident = (XIDLookupEnt *) hash_search(xidTable, (Pointer) &xident,
											  HASH_REMOVE_SAVED, &found);
		if (!xident || !found)
		{
			SpinRelease(masterLock);
			elog(NOTICE, "LockRelease: remove xid, table corrupted");
			return FALSE;
		}
	}

	if (wakeupNeeded)
		ProcLockWakeup(&(lock->waitProcs), lockmethod, lock);
#ifdef LOCK_DEBUG
	else if (LOCK_DEBUG_ENABLED(lock))
        elog(DEBUG, "LockRelease: no wakeup needed");
#endif

	SpinRelease(masterLock);
	return TRUE;
}

/*
 * LockReleaseAll -- Release all locks in a process lock queue.
 */
bool
LockReleaseAll(LOCKMETHOD lockmethod, SHM_QUEUE *lockQueue)
{
	PROC_QUEUE *waitQueue;
	int			done;
	XIDLookupEnt *xidLook = NULL;
	XIDLookupEnt *tmp = NULL;
	XIDLookupEnt *xident;
	SHMEM_OFFSET end = MAKE_OFFSET(lockQueue);
	SPINLOCK	masterLock;
	LOCKMETHODTABLE *lockMethodTable;
	int			i,
				numLockModes;
	LOCK	   *lock;
	bool		found;
	int			xidtag_lockmethod,
				nleft;

#ifdef LOCK_DEBUG
	if (lockmethod == USER_LOCKMETHOD ? Trace_userlocks : Trace_locks)
		elog(DEBUG, "LockReleaseAll: lockmethod=%d, pid=%d", lockmethod, MyProcPid);
#endif

	nleft = 0;

	Assert(lockmethod < NumLockMethods);
	lockMethodTable = LockMethodTable[lockmethod];
	if (!lockMethodTable)
	{
		elog(NOTICE, "LockAcquire: bad lockmethod %d", lockmethod);
		return FALSE;
	}

	if (SHMQueueEmpty(lockQueue))
		return TRUE;

	numLockModes = lockMethodTable->ctl->numLockModes;
	masterLock = lockMethodTable->ctl->masterLock;

	SpinAcquire(masterLock);
	SHMQueueFirst(lockQueue, (Pointer *) &xidLook, &xidLook->queue);

	for (;;)
	{
		bool		wakeupNeeded = false;

		/* ---------------------------
		 * XXX Here we assume the shared memory queue is circular and
		 * that we know its internal structure.  Should have some sort of
		 * macros to allow one to walk it.	mer 20 July 1991
		 * ---------------------------
		 */
		done = (xidLook->queue.next == end);
		lock = (LOCK *) MAKE_PTR(xidLook->tag.lock);

		xidtag_lockmethod = XIDENT_LOCKMETHOD(*xidLook);
		if (xidtag_lockmethod == lockmethod)
		{
			XID_PRINT("LockReleaseAll", xidLook);
			LOCK_PRINT("LockReleaseAll", lock, 0);
		}

#ifdef USE_XIDTAG_LOCKMETHOD
		if (xidtag_lockmethod != LOCK_LOCKMETHOD(*lock))
			elog(NOTICE, "LockReleaseAll: xid/lock method mismatch: %d != %d",
				 xidtag_lockmethod, lock->tag.lockmethod);
#endif
		if (xidtag_lockmethod != lockmethod)
		{
			nleft++;
			goto next_item;
		}

		Assert(lock->nHolding > 0);
		Assert(lock->nActive > 0);
		Assert(lock->nActive <= lock->nHolding);
		Assert(xidLook->nHolding >= 0);
		Assert(xidLook->nHolding <= lock->nHolding);

#ifdef USER_LOCKS
		if (lockmethod == USER_LOCKMETHOD)
		{
			if ((xidLook->tag.pid == 0) || (xidLook->tag.xid != 0))
			{
#ifdef LOCK_DEBUG
                if (Trace_userlocks)
                    elog(DEBUG, "LockReleaseAll: skiping normal lock [%ld,%d,%d]",
                         xidLook->tag.lock, xidLook->tag.pid, xidLook->tag.xid);
#endif /* LOCK_DEBUG */
				nleft++;
				goto next_item;
			}
			if (xidLook->tag.pid != MyProcPid)
			{
				/* Should never happen */
				elog(NOTICE,
					 "LockReleaseAll: INVALID PID: [%u] [%ld,%d,%d]",
					 lock->tag.objId.blkno,
				  xidLook->tag.lock, xidLook->tag.pid, xidLook->tag.xid);
				nleft++;
				goto next_item;
			}
#ifdef LOCK_DEBUG
            if (Trace_userlocks)
                elog(DEBUG, "LockReleaseAll: releasing user lock [%u] [%ld,%d,%d]",
                     lock->tag.objId.blkno, xidLook->tag.lock, xidLook->tag.pid, xidLook->tag.xid);
#endif /* LOCK_DEBUG */
		}
		else
		{
			/*
			 * Can't check xidLook->tag.xid, can be 0 also for normal locks
			 */
			if (xidLook->tag.pid != 0)
			{
#ifdef LOCK_DEBUG
                if (Trace_userlocks)
                    elog(DEBUG, "LockReleaseAll: skiping user lock [%u] [%ld,%d,%d]",
                         lock->tag.objId.blkno, xidLook->tag.lock, xidLook->tag.pid, xidLook->tag.xid);
#endif /* LOCK_DEBUG */
				nleft++;
				goto next_item;
			}
		}
#endif /* USER_LOCKS */

		/* ------------------
		 * fix the general lock stats
		 * ------------------
		 */
		if (lock->nHolding != xidLook->nHolding)
		{
			for (i = 1; i <= numLockModes; i++)
			{
				Assert(xidLook->holders[i] >= 0);
				lock->holders[i] -= xidLook->holders[i];
				lock->activeHolders[i] -= xidLook->holders[i];
				Assert((lock->holders[i] >= 0) \
					   &&(lock->activeHolders[i] >= 0));
				if (!lock->activeHolders[i])
					lock->mask &= BITS_OFF[i];

				/*
				 * Read comments in LockRelease
				 */
				if (!wakeupNeeded && xidLook->holders[i] > 0 &&
					lockMethodTable->ctl->conflictTab[i] & lock->waitMask)
					wakeupNeeded = true;
			}
			lock->nHolding -= xidLook->nHolding;
			lock->nActive -= xidLook->nHolding;
			Assert((lock->nHolding >= 0) && (lock->nActive >= 0));
			Assert(lock->nActive <= lock->nHolding);
		}
		else
		{
			/* --------------
			 * set nHolding to zero so that we can garbage collect the lock
			 * down below...
			 * --------------
			 */
			lock->nHolding = 0;
			/* Fix the lock status, just for next LOCK_PRINT message. */
			for (i = 1; i <= numLockModes; i++)
			{
				Assert(lock->holders[i] == lock->activeHolders[i]);
				lock->holders[i] = lock->activeHolders[i] = 0;
			}
		}
		LOCK_PRINT("LockReleaseAll: updated", lock, 0);

		/*
		 * Remove the xid from the process lock queue
		 */
		SHMQueueDelete(&xidLook->queue);

		/* ----------------
		 * always remove the xidLookup entry, we're done with it now
		 * ----------------
		 */

		XID_PRINT("LockReleaseAll: deleting", xidLook);
		xident = (XIDLookupEnt *) hash_search(lockMethodTable->xidHash,
											  (Pointer) xidLook,
											  HASH_REMOVE,
											  &found);
		if (!xident || !found)
		{
			SpinRelease(masterLock);
			elog(NOTICE, "LockReleaseAll: xid table corrupted");
			return FALSE;
		}

		if (!lock->nHolding)
		{
			/* --------------------
			 * if there's no one waiting in the queue, we've just released
			 * the last lock.
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
		{
			waitQueue = &(lock->waitProcs);
			ProcLockWakeup(waitQueue, lockmethod, lock);
		}

next_item:
		if (done)
			break;
		SHMQueueFirst(&xidLook->queue, (Pointer *) &tmp, &tmp->queue);
		xidLook = tmp;
	}

	/*
	 * Reinitialize the queue only if nothing has been left in.
	 */
	if (nleft == 0)
	{
#ifdef LOCK_DEBUG
        if (lockmethod == USER_LOCKMETHOD ? Trace_userlocks : Trace_locks)
            elog(DEBUG, "LockReleaseAll: reinitializing lockQueue");
#endif
		SHMQueueInit(lockQueue);
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

	/* xidHash table */
	size += hash_estimate_size(NLOCKENTS(maxBackends),
							   SHMEM_XIDTAB_KEYSIZE,
							   SHMEM_XIDTAB_DATASIZE);

	/*
	 * Since the lockHash entry count above is only an estimate, add 10%
	 * safety margin.
	 */
	size += size / 10;

	return size;
}

/* -----------------
 * Boolean function to determine current locking status
 * -----------------
 */
bool
LockingDisabled()
{
	return LockingIsDisabled;
}

/*
 * DeadlockCheck -- Checks for deadlocks for a given process
 *
 * We can't block on user locks, so no sense testing for deadlock
 * because there is no blocking, and no timer for the block.
 *
 * This code takes a list of locks a process holds, and the lock that
 * the process is sleeping on, and tries to find if any of the processes
 * waiting on its locks hold the lock it is waiting for.  If no deadlock
 * is found, it goes on to look at all the processes waiting on their locks.
 *
 * We have already locked the master lock before being called.
 */
bool
DeadLockCheck(void *proc, LOCK *findlock)
{
	XIDLookupEnt *xidLook = NULL;
	XIDLookupEnt *tmp = NULL;
	PROC	   *thisProc = (PROC *) proc,
			   *waitProc;
	SHM_QUEUE  *lockQueue = &(thisProc->lockQueue);
	SHMEM_OFFSET end = MAKE_OFFSET(lockQueue);
	LOCK	   *lock;
	PROC_QUEUE *waitQueue;
	int			i,
				j;
	bool		first_run = (thisProc == MyProc),
				done;

	static PROC *checked_procs[MAXBACKENDS];
	static int	nprocs;

	/* initialize at start of recursion */
	if (first_run)
	{
		checked_procs[0] = MyProc;
		nprocs = 1;
	}

	if (SHMQueueEmpty(lockQueue))
		return false;

	SHMQueueFirst(lockQueue, (Pointer *) &xidLook, &xidLook->queue);

	XID_PRINT("DeadLockCheck", xidLook);

	for (;;)
	{
		done = (xidLook->queue.next == end);
		lock = (LOCK *) MAKE_PTR(xidLook->tag.lock);

		LOCK_PRINT("DeadLockCheck", lock, 0);

		if (lock->tag.relId == 0)		/* user' lock */
			goto nxtl;

		/*
		 * waitLock is always in lockQueue of waiting proc, if !first_run
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
			LOCKMETHODCTL *lockctl =
			LockMethodTable[DEFAULT_LOCKMETHOD]->ctl;
			int			lm;

			Assert(xidLook->nHolding > 0);
			for (lm = 1; lm <= lockctl->numLockModes; lm++)
			{
				if (xidLook->holders[lm] > 0 &&
					lockctl->conflictTab[lm] & findlock->waitMask)
					return true;
			}

			/*
			 * Else - get the next lock from thisProc's lockQueue
			 */
			goto nxtl;
		}

		waitQueue = &(lock->waitProcs);
		waitProc = (PROC *) MAKE_PTR(waitQueue->links.prev);

		for (i = 0; i < waitQueue->size; i++)
		{
			if (waitProc == thisProc)
			{
				Assert(waitProc->waitLock == lock);
				Assert(waitProc == MyProc);
				waitProc = (PROC *) MAKE_PTR(waitProc->links.prev);
				continue;
			}
			if (lock == findlock)		/* first_run also true */
			{
				LOCKMETHODCTL *lockctl =
				LockMethodTable[DEFAULT_LOCKMETHOD]->ctl;

				/*
				 * If me blocked by his holdlock...
				 */
				if (lockctl->conflictTab[MyProc->token] & waitProc->holdLock)
				{
					/* and he blocked by me -> deadlock */
					if (lockctl->conflictTab[waitProc->token] & MyProc->holdLock)
						return true;
					/* we shouldn't look at lockQueue of our blockers */
					waitProc = (PROC *) MAKE_PTR(waitProc->links.prev);
					continue;
				}

				/*
				 * If he isn't blocked by me and we request
				 * non-conflicting lock modes - no deadlock here because
				 * of he isn't blocked by me in any sence (explicitle or
				 * implicitly). Note that we don't do like test if
				 * !first_run (when thisProc is holder and non-waiter on
				 * lock) and so we call DeadLockCheck below for every
				 * waitProc in thisProc->lockQueue, even for waitProc-s
				 * un-blocked by thisProc. Should we? This could save us
				 * some time...
				 */
				if (!(lockctl->conflictTab[waitProc->token] & MyProc->holdLock) &&
					!(lockctl->conflictTab[waitProc->token] & (1 << MyProc->token)))
				{
					waitProc = (PROC *) MAKE_PTR(waitProc->links.prev);
					continue;
				}
			}

			/*
			 * Look in lockQueue of this waitProc, if didn't do this
			 * before.
			 */
			for (j = 0; j < nprocs; j++)
			{
				if (checked_procs[j] == waitProc)
					break;
			}
			if (j >= nprocs)
			{
				Assert(nprocs < MAXBACKENDS);
				checked_procs[nprocs++] = waitProc;

				if (DeadLockCheck(waitProc, findlock))
				{
					LOCKMETHODCTL *lockctl =
					LockMethodTable[DEFAULT_LOCKMETHOD]->ctl;
					int			holdLock;

					/*
					 * Ok, but is waitProc waiting for me (thisProc) ?
					 */
					if (thisProc->waitLock == lock)
					{
						Assert(first_run);
						holdLock = thisProc->holdLock;
					}
					else
/* should we cache holdLock ? */
					{
						int			lm,
									tmpMask = 2;

						Assert(xidLook->nHolding > 0);
						for (holdLock = 0, lm = 1;
							 lm <= lockctl->numLockModes;
							 lm++, tmpMask <<= 1)
						{
							if (xidLook->holders[lm] > 0)
								holdLock |= tmpMask;
						}
						Assert(holdLock != 0);
					}
					if (lockctl->conflictTab[waitProc->token] & holdLock)
					{

						/*
						 * Last attempt to avoid deadlock - try to wakeup
						 * myself.
						 */
						if (first_run)
						{
							if (LockResolveConflicts(DEFAULT_LOCKMETHOD,
													 MyProc->waitLock,
													 MyProc->token,
													 MyProc->xid,
													 NULL) == STATUS_OK)
							{
								SetWaitingForLock(false);
								GrantLock(MyProc->waitLock, MyProc->token);
								(MyProc->waitLock->waitProcs.size)--;
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
											 lock,
											 waitProc->token,
											 waitProc->xid,
											 NULL) != STATUS_OK)
					{

						/*
						 * Blocked by others - no deadlock...
						 */
						LOCK_PRINT("DeadLockCheck: blocked by others", lock, waitProc->token);
						waitProc = (PROC *) MAKE_PTR(waitProc->links.prev);
						continue;
					}

					/*
					 * Well - wakeup this guy! This is the case of
					 * implicit blocking: thisProc blocked someone who
					 * blocked waitProc by the fact that he/someone is
					 * already waiting for lock.  We do this for
					 * anti-starving.
					 */
					GrantLock(lock, waitProc->token);
					waitQueue->size--;
					waitProc = ProcWakeup(waitProc, NO_ERROR);
					continue;
				}
			}
			waitProc = (PROC *) MAKE_PTR(waitProc->links.prev);
		}

nxtl:	;
		if (done)
			break;
		SHMQueueFirst(&xidLook->queue, (Pointer *) &tmp, &tmp->queue);
		xidLook = tmp;
	}

	/* if we got here, no deadlock */
	return false;
}

#ifdef LOCK_DEBUG
/*
 * Dump all locks in the proc->lockQueue. Must have already acquired
 * the masterLock.
 */
void
DumpLocks()
{
	SHMEM_OFFSET location;
	PROC	   *proc;
	SHM_QUEUE  *lockQueue;
	int			done;
	XIDLookupEnt *xidLook = NULL;
	XIDLookupEnt *tmp = NULL;
	SHMEM_OFFSET end;
	LOCK	   *lock;
	int			count = 0;
	int			lockmethod = DEFAULT_LOCKMETHOD;
	LOCKMETHODTABLE *lockMethodTable;

	ShmemPIDLookup(MyProcPid, &location);
	if (location == INVALID_OFFSET)
		return;
	proc = (PROC *) MAKE_PTR(location);
	if (proc != MyProc)
		return;
	lockQueue = &proc->lockQueue;

	Assert(lockmethod < NumLockMethods);
	lockMethodTable = LockMethodTable[lockmethod];
	if (!lockMethodTable)
		return;

	if (SHMQueueEmpty(lockQueue))
		return;

	SHMQueueFirst(lockQueue, (Pointer *) &xidLook, &xidLook->queue);
	end = MAKE_OFFSET(lockQueue);

	if (MyProc->waitLock)
		LOCK_PRINT("DumpLocks: waiting on", MyProc->waitLock, 0);

	for (;;)
	{
		if (count++ > 2000)
		{
			elog(NOTICE, "DumpLocks: xid loop detected, giving up");
			break;
		}

		/* ---------------------------
		 * XXX Here we assume the shared memory queue is circular and
		 * that we know its internal structure.  Should have some sort of
		 * macros to allow one to walk it.	mer 20 July 1991
		 * ---------------------------
		 */
		done = (xidLook->queue.next == end);
		lock = (LOCK *) MAKE_PTR(xidLook->tag.lock);

		XID_PRINT("DumpLocks", xidLook);
		LOCK_PRINT("DumpLocks", lock, 0);

		if (done)
			break;

		SHMQueueFirst(&xidLook->queue, (Pointer *) &tmp, &tmp->queue);
		xidLook = tmp;
	}
}

/*
 * Dump all postgres locks. Must have already acquired the masterLock.
 */
void
DumpAllLocks()
{
	SHMEM_OFFSET location;
	PROC	   *proc;
	XIDLookupEnt *xidLook = NULL;
	LOCK	   *lock;
	int			pid;
	int			count = 0;
	int			lockmethod = DEFAULT_LOCKMETHOD;
	LOCKMETHODTABLE *lockMethodTable;
	HTAB	   *xidTable;

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

	xidTable = lockMethodTable->xidHash;

	if (MyProc->waitLock)
		LOCK_PRINT("DumpAllLocks: waiting on", MyProc->waitLock, 0);

	hash_seq(NULL);
	while ((xidLook = (XIDLookupEnt *) hash_seq(xidTable)) &&
		   (xidLook != (XIDLookupEnt *) TRUE))
	{
		XID_PRINT("DumpAllLocks", xidLook);

		if (xidLook->tag.lock)
		{
			lock = (LOCK *) MAKE_PTR(xidLook->tag.lock);
			LOCK_PRINT("DumpAllLocks", lock, 0);
		}
		else
			elog(DEBUG, "DumpAllLocks: xidLook->tag.lock = NULL");

		if (count++ > 2000)
		{
			elog(NOTICE, "DumpAllLocks: possible loop, giving up");
			break;
		}
	}
}

#endif /* LOCK_DEBUG */
