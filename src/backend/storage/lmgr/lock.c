/*-------------------------------------------------------------------------
 *
 * lock.c--
 *	  simple lock acquisition
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/lmgr/lock.c,v 1.38.2.1 1999/03/07 02:00:49 tgl Exp $
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
 *	LockMethodTableRename(), LockReleaseAll, LockOwners()
 *	LockResolveConflicts(), GrantLock()
 *
 *	NOTE: This module is used to define new lock tables.  The
 *		multi-level lock table (multi.c) used by the heap
 *		access methods calls these routines.  See multi.c for
 *		examples showing how to use this interface.
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>				/* for sprintf() */
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>

#include "postgres.h"
#include "miscadmin.h"
#include "storage/shmem.h"
#include "storage/sinvaladt.h"
#include "storage/spin.h"
#include "storage/proc.h"
#include "storage/lock.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/palloc.h"
#include "access/xact.h"
#include "access/transam.h"
#include "utils/trace.h"
#include "utils/ps_status.h"

static int WaitOnLock(LOCKMETHOD lockmethod, LOCK *lock, LOCKMODE lockmode,
		   TransactionId xid);

/*
 * lockDebugRelation can be used to trace unconditionally a single relation,
 * for example pg_listener, if you suspect there are locking problems.
 *
 * lockDebugOidMin is is used to avoid tracing postgres relations, which
 * would produce a lot of output. Unfortunately most system relations are
 * created after bootstrap and have oid greater than BootstrapObjectIdData.
 * If you are using tprintf you should specify a value greater than the max
 * oid of system relations, which can be found with the following query:
 *
 *	 select max(int4in(int4out(oid))) from pg_class where relname ~ '^pg_';
 *
 * To get a useful lock trace you can use the following pg_options:
 *
 *	 -T "verbose,query,locks,userlocks,lock_debug_oidmin=17500"
 */
#define LOCKDEBUG(lockmethod)	(pg_options[TRACE_SHORTLOCKS+lockmethod])
#define lockDebugRelation		(pg_options[TRACE_LOCKRELATION])
#define lockDebugOidMin			(pg_options[TRACE_LOCKOIDMIN])
#define lockReadPriority		(pg_options[OPT_LOCKREADPRIORITY])

#ifdef LOCK_MGR_DEBUG
#define LOCK_PRINT(where,lock,type) \
	if (((LOCKDEBUG(LOCK_LOCKMETHOD(*(lock))) >= 1) \
		 && (lock->tag.relId >= lockDebugOidMin)) \
		|| (lock->tag.relId == lockDebugRelation)) \
		LOCK_PRINT_AUX(where,lock,type)

#define LOCK_PRINT_AUX(where,lock,type) \
	TPRINTF(TRACE_ALL, \
		 "%s: lock(%x) tbl(%d) rel(%d) db(%d) tid(%d,%d) mask(%x) " \
		 "hold(%d,%d,%d,%d,%d)=%d " \
		 "act(%d,%d,%d,%d,%d)=%d wait(%d) type(%s)", \
		 where, \
		 MAKE_OFFSET(lock), \
		 lock->tag.lockmethod, \
		 lock->tag.relId, \
		 lock->tag.dbId, \
		 ((lock->tag.tupleId.ip_blkid.bi_hi<<16)+ \
		  lock->tag.tupleId.ip_blkid.bi_lo), \
		 lock->tag.tupleId.ip_posid, \
		 lock->mask, \
		 lock->holders[1], \
		 lock->holders[2], \
		 lock->holders[3], \
		 lock->holders[4], \
		 lock->holders[5], \
		 lock->nHolding, \
		 lock->activeHolders[1], \
		 lock->activeHolders[2], \
		 lock->activeHolders[3], \
		 lock->activeHolders[4], \
		 lock->activeHolders[5], \
		 lock->nActive, \
		 lock->waitProcs.size, \
		 lock_types[type])

#define XID_PRINT(where,xidentP) \
	if (((LOCKDEBUG(XIDENT_LOCKMETHOD(*(xidentP))) >= 1) \
		 && (((LOCK *)MAKE_PTR(xidentP->tag.lock))->tag.relId \
			 >= lockDebugOidMin)) \
		|| (((LOCK *)MAKE_PTR(xidentP->tag.lock))->tag.relId \
			== lockDebugRelation)) \
		XID_PRINT_AUX(where,xidentP)

#define XID_PRINT_AUX(where,xidentP) \
	TPRINTF(TRACE_ALL, \
		 "%s: xid(%x) lock(%x) tbl(%d) pid(%d) xid(%d) " \
		 "hold(%d,%d,%d,%d,%d)=%d", \
		 where, \
		 MAKE_OFFSET(xidentP), \
		 xidentP->tag.lock, \
		 XIDENT_LOCKMETHOD(*(xidentP)), \
		 xidentP->tag.pid, \
		 xidentP->tag.xid, \
		 xidentP->holders[1], \
		 xidentP->holders[2], \
		 xidentP->holders[3], \
		 xidentP->holders[4], \
		 xidentP->holders[5], \
		 xidentP->nHolding)

#else							/* !LOCK_MGR_DEBUG */
#define LOCK_PRINT(where,lock,type)
#define LOCK_PRINT_AUX(where,lock,type)
#define XID_PRINT(where,xidentP)
#define XID_PRINT_AUX(where,xidentP)
#endif	 /* !LOCK_MGR_DEBUG */

static char *lock_types[] = {
	"",
	"WRITE",
	"READ",
	"WRITE INTENT",
	"READ INTENT",
	"EXTEND"
};

SPINLOCK	LockMgrLock;		/* in Shmem or created in
								 * CreateSpinlocks() */

/* This is to simplify/speed up some bit arithmetic */

static MASK BITS_OFF[MAX_LOCKMODES];
static MASK BITS_ON[MAX_LOCKMODES];

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

#ifdef LOCK_MGR_DEBUG

	/*
	 * If lockDebugOidMin value has not been specified in pg_options set a
	 * default value.
	 */
	if (!lockDebugOidMin)
		lockDebugOidMin = BootstrapObjectIdData;
#endif
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
LockMethodInit(LOCKMETHODTABLE * lockMethodTable,
			   MASK *conflictsP,
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
 */
LOCKMETHOD
LockMethodTableInit(char *tabName,
					MASK *conflictsP,
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

	/* allocate a string for the shmem index table lookup */
	shmemName = (char *) palloc((unsigned) (strlen(tabName) + 32));
	if (!shmemName)
	{
		elog(NOTICE, "LockMethodTableInit: couldn't malloc string %s \n", tabName);
		return INVALID_LOCKMETHOD;
	}

	/* each lock table has a non-shared header */
	lockMethodTable = (LOCKMETHODTABLE *) palloc((unsigned) sizeof(LOCKMETHODTABLE));
	if (!lockMethodTable)
	{
		elog(NOTICE, "LockMethodTableInit: couldn't malloc lock table %s\n", tabName);
		pfree(shmemName);
		return INVALID_LOCKMETHOD;
	}

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
		ShmemInitStruct(shmemName, (unsigned) sizeof(LOCKMETHODCTL), &found);

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
	XIDLookupEnt *result,
				item;
	HTAB	   *xidTable;
	bool		found;
	LOCK	   *lock = NULL;
	SPINLOCK	masterLock;
	LOCKMETHODTABLE *lockMethodTable;
	int			status;
	TransactionId xid;

#ifdef USER_LOCKS
	int			is_user_lock;

	is_user_lock = (lockmethod == USER_LOCKMETHOD);
	if (is_user_lock)
	{
#ifdef USER_LOCKS_DEBUG
		TPRINTF(TRACE_USERLOCKS, "LockAcquire: user lock [%u,%u] %s",
				locktag->tupleId.ip_posid,
				((locktag->tupleId.ip_blkid.bi_hi << 16) +
				 locktag->tupleId.ip_blkid.bi_lo),
				lock_types[lockmode]);
#endif
	}
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
		Assert(BlockIdEquals(&(lock->tag.tupleId.ip_blkid),
							 &(locktag->tupleId.ip_blkid)));
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
	if (is_user_lock)
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
	 * Find or create an xid entry with this tag
	 */
	result = (XIDLookupEnt *) hash_search(xidTable, (Pointer) &item,
										  HASH_ENTER, &found);
	if (!result)
	{
		elog(NOTICE, "LockAcquire: xid table corrupted");
		return STATUS_ERROR;
	}

	/*
	 * If not found initialize the new entry
	 */
	if (!found)
	{
		result->nHolding = 0;
		MemSet((char *) result->holders, 0, sizeof(int) * MAX_LOCKMODES);
		ProcAddLock(&result->queue);
		XID_PRINT("LockAcquire: new", result);
	}
	else
	{
		XID_PRINT("LockAcquire: found", result);
		Assert((result->nHolding > 0) && (result->holders[lockmode] >= 0));
		Assert(result->nHolding <= lock->nActive);
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
	 * cannot be a conflict.  Need to subtract one from the
	 * lock's count since we just bumped the count up by 1
	 * above.
	 * --------------------
	 */
	if (result->nHolding == lock->nActive)
	{
		result->holders[lockmode]++;
		result->nHolding++;
		XID_PRINT("LockAcquire: owning", result);
		Assert((result->nHolding > 0) && (result->holders[lockmode] > 0));
		GrantLock(lock, lockmode);
		SpinRelease(masterLock);
		return TRUE;
	}

	status = LockResolveConflicts(lockmethod, lock, lockmode, xid, result);
	if (status == STATUS_OK)
		GrantLock(lock, lockmode);
	else if (status == STATUS_FOUND)
	{
#ifdef USER_LOCKS

		/*
		 * User locks are non blocking. If we can't acquire a lock we must
		 * remove the xid entry and return FALSE without waiting.
		 */
		if (is_user_lock)
		{
			if (!result->nHolding)
			{
				SHMQueueDelete(&result->queue);
				result = (XIDLookupEnt *) hash_search(xidTable,
													  (Pointer) result,
													HASH_REMOVE, &found);
				if (!result || !found)
					elog(NOTICE, "LockAcquire: remove xid, table corrupted");
			}
			else
				XID_PRINT_AUX("LockAcquire: NHOLDING", result);
			lock->nHolding--;
			lock->holders[lockmode]--;
			LOCK_PRINT("LockAcquire: user lock failed", lock, lockmode);
			Assert((lock->nHolding > 0) && (lock->holders[lockmode] >= 0));
			Assert(lock->nActive <= lock->nHolding);
			SpinRelease(masterLock);
			return FALSE;
		}
#endif
		status = WaitOnLock(lockmethod, lock, lockmode, xid);

		/*
		 * Check the xid entry status, in case something in the ipc
		 * communication doesn't work correctly.
		 */
		if (!((result->nHolding > 0) && (result->holders[lockmode] > 0)))
		{
			XID_PRINT_AUX("LockAcquire: INCONSISTENT ", result);
			LOCK_PRINT_AUX("LockAcquire: INCONSISTENT ", lock, lockmode);
			/* Should we retry ? */
			return FALSE;
		}
		XID_PRINT("LockAcquire: granted", result);
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
 *	For example, if I am already holding a WRITE_INTENT lock,
 *	there will not be a conflict with my own READ_LOCK.  If I
 *	don't consider the intent lock when checking for conflicts,
 *	I find no conflict.
 * ----------------------------
 */
int
LockResolveConflicts(LOCKMETHOD lockmethod,
					 LOCK *lock,
					 LOCKMODE lockmode,
					 TransactionId xid,
					 XIDLookupEnt *xidentP)		/* xident ptr or NULL */
{
	XIDLookupEnt *result,
				item;
	int		   *myHolders;
	int			numLockModes;
	HTAB	   *xidTable;
	bool		found;
	int			bitmask;
	int			i,
				tmpMask;

#ifdef USER_LOCKS
	int			is_user_lock;

#endif

	numLockModes = LockMethodTable[lockmethod]->ctl->numLockModes;
	xidTable = LockMethodTable[lockmethod]->xidHash;

	if (xidentP)
	{

		/*
		 * A pointer to the xid entry was supplied from the caller.
		 * Actually only LockAcquire can do it.
		 */
		result = xidentP;
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
		is_user_lock = (lockmethod == 2);
		if (is_user_lock)
		{
			item.tag.pid = MyProcPid;
			item.tag.xid = 0;
		}
		else
			TransactionIdStore(xid, &item.tag.xid);
#else
		TransactionIdStore(xid, &item.tag.xid);
#endif

		/*
		 * Find or create an xid entry with this tag
		 */
		result = (XIDLookupEnt *) hash_search(xidTable, (Pointer) &item,
											  HASH_ENTER, &found);
		if (!result)
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
			MemSet(result->holders, 0, numLockModes * sizeof(*(lock->holders)));
			result->nHolding = 0;
			XID_PRINT_AUX("LockResolveConflicts: NOT FOUND", result);
		}
		else
			XID_PRINT("LockResolveConflicts: found", result);
	}
	Assert((result->nHolding >= 0) && (result->holders[lockmode] >= 0));

	/*
	 * We can control runtime this option. Default is lockReadPriority=0
	 */
	if (!lockReadPriority)
	{
		/* ------------------------
		 * If someone with a greater priority is waiting for the lock,
		 * do not continue and share the lock, even if we can.	bjm
		 * ------------------------
		 */
		int			myprio = LockMethodTable[lockmethod]->ctl->prio[lockmode];
		PROC_QUEUE *waitQueue = &(lock->waitProcs);
		PROC	   *topproc = (PROC *) MAKE_PTR(waitQueue->links.prev);

		if (waitQueue->size && topproc->prio > myprio)
		{
			XID_PRINT("LockResolveConflicts: higher priority proc waiting",
					  result);
			return STATUS_FOUND;
		}
	}

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
		result->holders[lockmode]++;
		result->nHolding++;
		XID_PRINT("LockResolveConflicts: no conflict", result);
		Assert((result->nHolding > 0) && (result->holders[lockmode] > 0));
		return STATUS_OK;
	}

	/* ------------------------
	 * Rats.  Something conflicts. But it could still be my own
	 * lock.  We have to construct a conflict mask
	 * that does not reflect our own locks.
	 * ------------------------
	 */
	myHolders = result->holders;
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
		result->holders[lockmode]++;
		result->nHolding++;
		XID_PRINT("LockResolveConflicts: resolved", result);
		Assert((result->nHolding > 0) && (result->holders[lockmode] > 0));
		return STATUS_OK;
	}

	XID_PRINT("LockResolveConflicts: conflicting", result);
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
WaitOnLock(LOCKMETHOD lockmethod, LOCK *lock, LOCKMODE lockmode,
		   TransactionId xid)
{
	PROC_QUEUE *waitQueue = &(lock->waitProcs);
	LOCKMETHODTABLE *lockMethodTable = LockMethodTable[lockmethod];
	int			prio = lockMethodTable->ctl->prio[lockmode];
	char		old_status[64],
				new_status[64];

	Assert(lockmethod < NumLockMethods);

	/*
	 * the waitqueue is ordered by priority. I insert myself according to
	 * the priority of the lock I am acquiring.
	 *
	 * SYNC NOTE: I am assuming that the lock table spinlock is sufficient
	 * synchronization for this queue.	That will not be true if/when
	 * people can be deleted from the queue by a SIGINT or something.
	 */
	LOCK_PRINT_AUX("WaitOnLock: sleeping on lock", lock, lockmode);
	strcpy(old_status, PS_STATUS);
	strcpy(new_status, PS_STATUS);
	strcat(new_status, " waiting");
	PS_SET_STATUS(new_status);
	if (ProcSleep(waitQueue,
				  lockMethodTable->ctl->masterLock,
				  lockmode,
				  prio,
				  lock,
				  xid) != NO_ERROR)
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
		LOCK_PRINT_AUX("WaitOnLock: aborting on lock", lock, lockmode);
		Assert((lock->nHolding >= 0) && (lock->holders[lockmode] >= 0));
		Assert(lock->nActive <= lock->nHolding);
		SpinRelease(lockMethodTable->ctl->masterLock);
		elog(ERROR, "WaitOnLock: error on wakeup - Aborting this transaction");

		/* not reached */
	}

	PS_SET_STATUS(old_status);
	LOCK_PRINT_AUX("WaitOnLock: wakeup on lock", lock, lockmode);
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
	XIDLookupEnt *result,
				item;
	HTAB	   *xidTable;
	TransactionId xid;
	bool		wakeupNeeded = true;
	int			trace_flag;

#ifdef USER_LOCKS
	int			is_user_lock;

	is_user_lock = (lockmethod == USER_LOCKMETHOD);
	if (is_user_lock)
	{
		TPRINTF(TRACE_USERLOCKS, "LockRelease: user lock tag [%u,%u] %d",
				locktag->tupleId.ip_posid,
				((locktag->tupleId.ip_blkid.bi_hi << 16) +
				 locktag->tupleId.ip_blkid.bi_lo),
				lockmode);
	}
#endif

	/* ???????? This must be changed when short term locks will be used */
	locktag->lockmethod = lockmethod;

#ifdef USER_LOCKS
	trace_flag = \
		(lockmethod == USER_LOCKMETHOD) ? TRACE_USERLOCKS : TRACE_LOCKS;
#else
	trace_flag = TRACE_LOCKS;
#endif

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
#ifdef USER_LOCKS
		if (is_user_lock)
		{
			TPRINTF(TRACE_USERLOCKS, "LockRelease: no lock with this tag");
			return FALSE;
		}
#endif
		elog(NOTICE, "LockRelease: locktable lookup failed, no lock");
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
	if (is_user_lock)
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
	result = (XIDLookupEnt *) hash_search(xidTable, (Pointer) &item,
										  HASH_FIND_SAVE, &found);
	if (!result || !found)
	{
		SpinRelease(masterLock);
#ifdef USER_LOCKS
		if (!found && is_user_lock)
			TPRINTF(TRACE_USERLOCKS, "LockRelease: no lock with this tag");
		else
#endif
			elog(NOTICE, "LockReplace: xid table corrupted");
		return FALSE;
	}
	XID_PRINT("LockRelease: found", result);
	Assert(result->tag.lock == MAKE_OFFSET(lock));

	/*
	 * Check that we are actually holding a lock of the type we want to
	 * release.
	 */
	if (!(result->holders[lockmode] > 0))
	{
		SpinRelease(masterLock);
		XID_PRINT_AUX("LockAcquire: WRONGTYPE", result);
		elog(NOTICE, "LockRelease: you don't own a lock of type %s",
			 lock_types[lockmode]);
		Assert(result->holders[lockmode] >= 0);
		return FALSE;
	}
	Assert(result->nHolding > 0);

	/*
	 * fix the general lock stats
	 */
	lock->nHolding--;
	lock->holders[lockmode]--;
	lock->nActive--;
	lock->activeHolders[lockmode]--;

	/* --------------------------
	 * If there are still active locks of the type I just released, no one
	 * should be woken up.	Whoever is asleep will still conflict
	 * with the remaining locks.
	 * --------------------------
	 */
	if (lock->activeHolders[lockmode])
		wakeupNeeded = false;
	else
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
	result->holders[lockmode]--;
	result->nHolding--;
	XID_PRINT("LockRelease: updated", result);
	Assert((result->nHolding >= 0) && (result->holders[lockmode] >= 0));

	/*
	 * If this was my last hold on this lock, delete my entry in the XID
	 * table.
	 */
	if (!result->nHolding)
	{
		if (result->queue.prev == INVALID_OFFSET)
			elog(NOTICE, "LockRelease: xid.prev == INVALID_OFFSET");
		if (result->queue.next == INVALID_OFFSET)
			elog(NOTICE, "LockRelease: xid.next == INVALID_OFFSET");
		if (result->queue.next != INVALID_OFFSET)
			SHMQueueDelete(&result->queue);
		XID_PRINT("LockRelease: deleting", result);
		result = (XIDLookupEnt *) hash_search(xidTable, (Pointer) &result,
											  HASH_REMOVE_SAVED, &found);
		if (!result || !found)
		{
			SpinRelease(masterLock);
			elog(NOTICE, "LockRelease: remove xid, table corrupted");
			return FALSE;
		}
	}

	if (wakeupNeeded)
	{
		/* --------------------------
		 * Wake the first waiting process and grant him the lock if it
		 * doesn't conflict.  The woken process must record the lock
		 * himself.
		 * --------------------------
		 */
		ProcLockWakeup(&(lock->waitProcs), lockmethod, lock);
	}
	else
	{
		if (((LOCKDEBUG(LOCK_LOCKMETHOD(*(lock))) >= 1) \
			 && (lock->tag.relId >= lockDebugOidMin)) \
			|| (lock->tag.relId == lockDebugRelation))
			TPRINTF(TRACE_ALL, "LockRelease: no wakeup needed");
	}

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
	XIDLookupEnt *result;
	SHMEM_OFFSET end = MAKE_OFFSET(lockQueue);
	SPINLOCK	masterLock;
	LOCKMETHODTABLE *lockMethodTable;
	int			i,
				numLockModes;
	LOCK	   *lock;
	bool		found;
	int			trace_flag;
	int			xidtag_lockmethod;

#ifdef USER_LOCKS
	int			is_user_lock_table,
				count,
				nleft;

	count = nleft = 0;

	is_user_lock_table = (lockmethod == USER_LOCKMETHOD);
	trace_flag = (lockmethod == 2) ? TRACE_USERLOCKS : TRACE_LOCKS;
#else
	trace_flag = TRACE_LOCKS;
#endif
	TPRINTF(trace_flag, "LockReleaseAll: lockmethod=%d, pid=%d",
			lockmethod, MyProcPid);

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

		/*
		 * Sometimes the queue appears to be messed up.
		 */
		if (count++ > 1000)
		{
			elog(NOTICE, "LockReleaseAll: xid loop detected, giving up");
			nleft = 0;
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

		xidtag_lockmethod = XIDENT_LOCKMETHOD(*xidLook);
		if ((xidtag_lockmethod == lockmethod) || (trace_flag >= 2))
		{
			XID_PRINT("LockReleaseAll", xidLook);
			LOCK_PRINT("LockReleaseAll", lock, 0);
		}

#ifdef USE_XIDTAG_LOCKMETHOD
		if (xidtag_lockmethod != LOCK_LOCKMETHOD(*lock))
			elog(NOTICE, "LockReleaseAll: xid/lock method mismatch: %d != %d",
				 xidtag_lockmethod, lock->tag.lockmethod);
#endif
		if ((xidtag_lockmethod != lockmethod) && (trace_flag >= 2))
		{
			TPRINTF(trace_flag, "LockReleaseAll: skipping other table");
			nleft++;
			goto next_item;
		}

		Assert(lock->nHolding > 0);
		Assert(lock->nActive > 0);
		Assert(lock->nActive <= lock->nHolding);
		Assert(xidLook->nHolding >= 0);
		Assert(xidLook->nHolding <= lock->nHolding);

#ifdef USER_LOCKS
		if (is_user_lock_table)
		{
			if ((xidLook->tag.pid == 0) || (xidLook->tag.xid != 0))
			{
				TPRINTF(TRACE_USERLOCKS,
						"LockReleaseAll: skiping normal lock [%d,%d,%d]",
				  xidLook->tag.lock, xidLook->tag.pid, xidLook->tag.xid);
				nleft++;
				goto next_item;
			}
			if (xidLook->tag.pid != MyProcPid)
			{
				/* Should never happen */
				elog(NOTICE,
					 "LockReleaseAll: INVALID PID: [%u,%u] [%d,%d,%d]",
					 lock->tag.tupleId.ip_posid,
					 ((lock->tag.tupleId.ip_blkid.bi_hi << 16) +
					  lock->tag.tupleId.ip_blkid.bi_lo),
				  xidLook->tag.lock, xidLook->tag.pid, xidLook->tag.xid);
				nleft++;
				goto next_item;
			}
			TPRINTF(TRACE_USERLOCKS,
				"LockReleaseAll: releasing user lock [%u,%u] [%d,%d,%d]",
					lock->tag.tupleId.ip_posid,
					((lock->tag.tupleId.ip_blkid.bi_hi << 16) +
					 lock->tag.tupleId.ip_blkid.bi_lo),
				  xidLook->tag.lock, xidLook->tag.pid, xidLook->tag.xid);
		}
		else
		{

			/*
			 * Can't check xidLook->tag.xid, can be 0 also for normal
			 * locks
			 */
			if (xidLook->tag.pid != 0)
			{
				TPRINTF(TRACE_LOCKS,
				  "LockReleaseAll: skiping user lock [%u,%u] [%d,%d,%d]",
						lock->tag.tupleId.ip_posid,
						((lock->tag.tupleId.ip_blkid.bi_hi << 16) +
						 lock->tag.tupleId.ip_blkid.bi_lo),
				  xidLook->tag.lock, xidLook->tag.pid, xidLook->tag.xid);
				nleft++;
				goto next_item;
			}
		}
#endif

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
		result = (XIDLookupEnt *) hash_search(lockMethodTable->xidHash,
											  (Pointer) xidLook,
											  HASH_REMOVE,
											  &found);
		if (!result || !found)
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
		else
		{
			/* --------------------
			 * Wake the first waiting process and grant him the lock if it
			 * doesn't conflict.  The woken process must record the lock
			 * him/herself.
			 * --------------------
			 */
			waitQueue = &(lock->waitProcs);
			ProcLockWakeup(waitQueue, lockmethod, lock);
		}

#ifdef USER_LOCKS
next_item:
#endif
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
		TPRINTF(trace_flag, "LockReleaseAll: reinitializing lockQueue");
		SHMQueueInit(lockQueue);
	}

	SpinRelease(masterLock);
	TPRINTF(trace_flag, "LockReleaseAll: done");

	return TRUE;
}

int
LockShmemSize()
{
	int			size = 0;

	size += MAXALIGN(sizeof(PROC_HDR)); /* ProcGlobal */
	size += MAXALIGN(MaxBackendId * sizeof(PROC)); /* each MyProc */
	size += MAXALIGN(MaxBackendId * sizeof(LOCKMETHODCTL));		/* each
																 * lockMethodTable->ctl */

	/* lockHash table */
	size += hash_estimate_size(NLOCKENTS,
							   SHMEM_LOCKTAB_KEYSIZE,
							   SHMEM_LOCKTAB_DATASIZE);

	/* xidHash table */
	size += hash_estimate_size(MaxBackendId,
							   SHMEM_XIDTAB_KEYSIZE,
							   SHMEM_XIDTAB_DATASIZE);

	/* Since the lockHash entry count above is only an estimate,
	 * add 10% safety margin.
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
DeadLockCheck(SHM_QUEUE *lockQueue, LOCK *findlock, bool skip_check)
{
	int			done;
	XIDLookupEnt *xidLook = NULL;
	XIDLookupEnt *tmp = NULL;
	SHMEM_OFFSET end = MAKE_OFFSET(lockQueue);
	LOCK	   *lock;

	LOCKMETHODTABLE *lockMethodTable;
	XIDLookupEnt *result,
				item;
	HTAB	   *xidTable;
	bool		found;

	static PROC *checked_procs[MaxBackendId];
	static int	nprocs;
	static bool MyNHolding;

	/* initialize at start of recursion */
	if (skip_check)
	{
		checked_procs[0] = MyProc;
		nprocs = 1;

		lockMethodTable = LockMethodTable[DEFAULT_LOCKMETHOD];
		xidTable = lockMethodTable->xidHash;

		MemSet(&item, 0, XID_TAGSIZE);
		TransactionIdStore(MyProc->xid, &item.tag.xid);
		item.tag.lock = MAKE_OFFSET(findlock);
#if 0
		item.tag.pid = pid;
#endif

		if (!(result = (XIDLookupEnt *)
			  hash_search(xidTable, (Pointer) &item, HASH_FIND, &found)) || !found)
		{
			elog(NOTICE, "LockAcquire: xid table corrupted");
			return true;
		}
		MyNHolding = result->nHolding;
	}
	if (SHMQueueEmpty(lockQueue))
		return false;

	SHMQueueFirst(lockQueue, (Pointer *) &xidLook, &xidLook->queue);

	XID_PRINT("DeadLockCheck", xidLook);

	for (;;)
	{
		/* ---------------------------
		 * XXX Here we assume the shared memory queue is circular and
		 * that we know its internal structure.  Should have some sort of
		 * macros to allow one to walk it.	mer 20 July 1991
		 * ---------------------------
		 */
		done = (xidLook->queue.next == end);
		lock = (LOCK *) MAKE_PTR(xidLook->tag.lock);

		LOCK_PRINT("DeadLockCheck", lock, 0);

		/*
		 * This is our only check to see if we found the lock we want.
		 *
		 * The lock we are waiting for is already in MyProc->lockQueue so we
		 * need to skip it here.  We are trying to find it in someone
		 * else's lockQueue.
		 */
		if (lock == findlock && !skip_check)
			return true;

		{
			PROC_QUEUE *waitQueue = &(lock->waitProcs);
			PROC	   *proc;
			int			i;
			int			j;

			proc = (PROC *) MAKE_PTR(waitQueue->links.prev);
			for (i = 0; i < waitQueue->size; i++)
			{
				if (proc != MyProc &&
					lock == findlock && /* skip_check also true */
					MyNHolding) /* I already hold some lock on it */
				{

					/*
					 * For findlock's wait queue, we are interested in
					 * procs who are blocked waiting for a write-lock on
					 * the table we are waiting on, and already hold a
					 * lock on it. We first check to see if there is an
					 * escalation deadlock, where we hold a readlock and
					 * want a writelock, and someone else holds readlock
					 * on the same table, and wants a writelock.
					 *
					 * Basically, the test is, "Do we both hold some lock on
					 * findlock, and we are both waiting in the lock
					 * queue?"
					 */

					Assert(skip_check);
					Assert(MyProc->prio == 2);

					lockMethodTable = LockMethodTable[1];
					xidTable = lockMethodTable->xidHash;

					MemSet(&item, 0, XID_TAGSIZE);
					TransactionIdStore(proc->xid, &item.tag.xid);
					item.tag.lock = MAKE_OFFSET(findlock);
#if 0
					item.tag.pid = pid;
#endif

					if (!(result = (XIDLookupEnt *)
						  hash_search(xidTable, (Pointer) &item, HASH_FIND, &found)) || !found)
					{
						elog(NOTICE, "LockAcquire: xid table corrupted");
						return true;
					}
					if (result->nHolding)
						return true;
				}

				/*
				 * No sense in looking at the wait queue of the lock we
				 * are looking for. If lock == findlock, and I got here,
				 * skip_check must be true too.
				 */
				if (lock != findlock)
				{
					for (j = 0; j < nprocs; j++)
						if (checked_procs[j] == proc)
							break;
					if (j >= nprocs && lock != findlock)
					{
						checked_procs[nprocs++] = proc;
						Assert(nprocs <= MaxBackendId);

						/*
						 * For non-MyProc entries, we are looking only
						 * waiters, not necessarily people who already
						 * hold locks and are waiting. Now we check for
						 * cases where we have two or more tables in a
						 * deadlock.  We do this by continuing to search
						 * for someone holding a lock
						 */
						if (DeadLockCheck(&(proc->lockQueue), findlock, false))
							return true;
					}
				}
				proc = (PROC *) MAKE_PTR(proc->links.prev);
			}
		}

		if (done)
			break;
		SHMQueueFirst(&xidLook->queue, (Pointer *) &tmp, &tmp->queue);
		xidLook = tmp;
	}

	/* if we got here, no deadlock */
	return false;
}

#ifdef NOT_USED
/*
 * Return an array with the pids of all processes owning a lock.
 * This works only for user locks because normal locks have no
 * pid information in the corresponding XIDLookupEnt.
 */
ArrayType  *
LockOwners(LOCKMETHOD lockmethod, LOCKTAG *locktag)
{
	XIDLookupEnt *xidLook = NULL;
	SPINLOCK	masterLock;
	LOCK	   *lock;
	SHMEM_OFFSET lock_offset;
	int			count = 0;
	LOCKMETHODTABLE *lockMethodTable;
	HTAB	   *xidTable;
	bool		found;
	int			ndims,
				nitems,
				hdrlen,
				size;
	int			lbounds[1],
				hbounds[1];
	ArrayType  *array;
	int		   *data_ptr;

	/* Assume that no one will modify the result */
	static int	empty_array[] = {20, 1, 0, 0, 0};

#ifdef USER_LOCKS
	int			is_user_lock;

	is_user_lock = (lockmethod == USER_LOCKMETHOD);
	if (is_user_lock)
	{
		TPRINTF(TRACE_USERLOCKS, "LockOwners: user lock tag [%u,%u]",
				locktag->tupleId.ip_posid,
				((locktag->tupleId.ip_blkid.bi_hi << 16) +
				 locktag->tupleId.ip_blkid.bi_lo));
	}
#endif

	/* This must be changed when short term locks will be used */
	locktag->lockmethod = lockmethod;

	Assert((lockmethod >= MIN_LOCKMETHOD) && (lockmethod < NumLockMethods));
	lockMethodTable = LockMethodTable[lockmethod];
	if (!lockMethodTable)
	{
		elog(NOTICE, "lockMethodTable is null in LockOwners");
		return (ArrayType *) &empty_array;
	}

	if (LockingIsDisabled)
		return (ArrayType *) &empty_array;

	masterLock = lockMethodTable->ctl->masterLock;
	SpinAcquire(masterLock);

	/*
	 * Find a lock with this tag
	 */
	Assert(lockMethodTable->lockHash->hash == tag_hash);
	lock = (LOCK *) hash_search(lockMethodTable->lockHash, (Pointer) locktag,
								HASH_FIND, &found);

	/*
	 * let the caller print its own error message, too. Do not elog(WARN).
	 */
	if (!lock)
	{
		SpinRelease(masterLock);
		elog(NOTICE, "LockOwners: locktable corrupted");
		return (ArrayType *) &empty_array;
	}

	if (!found)
	{
		SpinRelease(masterLock);
#ifdef USER_LOCKS
		if (is_user_lock)
		{
			TPRINTF(TRACE_USERLOCKS, "LockOwners: no lock with this tag");
			return (ArrayType *) &empty_array;
		}
#endif
		elog(NOTICE, "LockOwners: locktable lookup failed, no lock");
		return (ArrayType *) &empty_array;
	}
	LOCK_PRINT("LockOwners: found", lock, 0);
	Assert((lock->nHolding > 0) && (lock->nActive > 0));
	Assert(lock->nActive <= lock->nHolding);
	lock_offset = MAKE_OFFSET(lock);

	/* Construct a 1-dimensional array */
	ndims = 1;
	hdrlen = ARR_OVERHEAD(ndims);
	lbounds[0] = 0;
	hbounds[0] = lock->nActive;
	size = hdrlen + sizeof(int) * hbounds[0];
	array = (ArrayType *) palloc(size);
	MemSet(array, 0, size);
	memmove((char *) array, (char *) &size, sizeof(int));
	memmove((char *) ARR_NDIM_PTR(array), (char *) &ndims, sizeof(int));
	memmove((char *) ARR_DIMS(array), (char *) hbounds, ndims * sizeof(int));
	memmove((char *) ARR_LBOUND(array), (char *) lbounds, ndims * sizeof(int));
	SET_LO_FLAG(false, array);
	data_ptr = (int *) ARR_DATA_PTR(array);

	xidTable = lockMethodTable->xidHash;
	hash_seq(NULL);
	nitems = 0;
	while ((xidLook = (XIDLookupEnt *) hash_seq(xidTable)) &&
		   (xidLook != (XIDLookupEnt *) TRUE))
	{
		if (count++ > 1000)
		{
			elog(NOTICE, "LockOwners: possible loop, giving up");
			break;
		}

		if (xidLook->tag.pid == 0)
		{
			XID_PRINT("LockOwners: no pid", xidLook);
			continue;
		}

		if (!xidLook->tag.lock)
		{
			XID_PRINT("LockOwners: NULL LOCK", xidLook);
			continue;
		}

		if (xidLook->tag.lock != lock_offset)
		{
			XID_PRINT("LockOwners: different lock", xidLook);
			continue;
		}

		if (LOCK_LOCKMETHOD(*lock) != lockmethod)
		{
			XID_PRINT("LockOwners: other table", xidLook);
			continue;
		}

		if (xidLook->nHolding <= 0)
		{
			XID_PRINT("LockOwners: not holding", xidLook);
			continue;
		}

		if (nitems >= hbounds[0])
		{
			elog(NOTICE, "LockOwners: array size exceeded");
			break;
		}

		/*
		 * Check that the holding process is still alive by sending him an
		 * unused (ignored) signal. If the kill fails the process is not
		 * alive.
		 */
		if ((xidLook->tag.pid != MyProcPid) \
			&&(kill(xidLook->tag.pid, SIGCHLD)) != 0)
		{
			/* Return a negative pid to signal that process is dead */
			data_ptr[nitems++] = -(xidLook->tag.pid);
			XID_PRINT("LockOwners: not alive", xidLook);
			/* XXX - TODO: remove this entry and update lock stats */
			continue;
		}

		/* Found a process holding the lock */
		XID_PRINT("LockOwners: holding", xidLook);
		data_ptr[nitems++] = xidLook->tag.pid;
	}

	SpinRelease(masterLock);

	/* Adjust the actual size of the array */
	hbounds[0] = nitems;
	size = hdrlen + sizeof(int) * hbounds[0];
	memmove((char *) array, (char *) &size, sizeof(int));
	memmove((char *) ARR_DIMS(array), (char *) hbounds, ndims * sizeof(int));

	return array;
}
#endif

#ifdef DEADLOCK_DEBUG
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
	SPINLOCK	masterLock;
	int			numLockModes;
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

	numLockModes = lockMethodTable->ctl->numLockModes;
	masterLock = lockMethodTable->ctl->masterLock;

	if (SHMQueueEmpty(lockQueue))
		return;

	SHMQueueFirst(lockQueue, (Pointer *) &xidLook, &xidLook->queue);
	end = MAKE_OFFSET(lockQueue);

	if (MyProc->waitLock)
		LOCK_PRINT_AUX("DumpLocks: waiting on", MyProc->waitLock, 0);

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

		XID_PRINT_AUX("DumpLocks", xidLook);
		LOCK_PRINT_AUX("DumpLocks", lock, 0);

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
		LOCK_PRINT_AUX("DumpAllLocks: waiting on", MyProc->waitLock, 0);

	hash_seq(NULL);
	while ((xidLook = (XIDLookupEnt *) hash_seq(xidTable)) &&
		   (xidLook != (XIDLookupEnt *) TRUE))
	{
		XID_PRINT_AUX("DumpAllLocks", xidLook);

		if (xidLook->tag.lock)
		{
			lock = (LOCK *) MAKE_PTR(xidLook->tag.lock);
			LOCK_PRINT_AUX("DumpAllLocks", lock, 0);
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

#endif
