/*-------------------------------------------------------------------------
 *
 * lock.c--
 *	  simple lock acquisition
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/lmgr/lock.c,v 1.32 1998/06/30 02:33:31 momjian Exp $
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
 *	LockAcquire(), LockRelease(), LockMethodTableInit().
 *
 *	LockReplace() is called only within this module and by the
 *		lkchain module.  It releases a lock without looking
 *		the lock up in the lock table.
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

#include "postgres.h"
#include "miscadmin.h"
#include "storage/shmem.h"
#include "storage/sinvaladt.h"
#include "storage/spin.h"
#include "storage/proc.h"
#include "storage/lock.h"
#include "utils/dynahash.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/palloc.h"
#include "access/xact.h"
#include "access/transam.h"

static int
WaitOnLock(LOCKMETHOD lockmethod, LOCK *lock, LOCKMODE lockmode);

/*#define LOCK_MGR_DEBUG*/

#ifndef LOCK_MGR_DEBUG

#define LOCK_PRINT(where,tag,type)
#define LOCK_DUMP(where,lock,type)
#define LOCK_DUMP_AUX(where,lock,type)
#define XID_PRINT(where,xidentP)

#else							/* LOCK_MGR_DEBUG */

int			lockDebug = 0;
unsigned int lock_debug_oid_min = BootstrapObjectIdData;
static char *lock_types[] = {
	"NONE",
	"WRITE",
	"READ",
	"WRITE INTENT",
	"READ INTENT",
	"EXTEND"
};

#define LOCK_PRINT(where,tag,type)\
	if ((lockDebug >= 1) && (tag->relId >= lock_debug_oid_min)) \
		elog(DEBUG, \
			 "%s: pid (%d) rel (%d) dbid (%d) tid (%d,%d) type (%s)",where, \
			 MyProcPid,\
			 tag->relId, tag->dbId, \
			 ((tag->tupleId.ip_blkid.bi_hi<<16)+\
			  tag->tupleId.ip_blkid.bi_lo),\
			 tag->tupleId.ip_posid, \
			 lock_types[type])

#define LOCK_DUMP(where,lock,type)\
	if ((lockDebug >= 1) && (lock->tag.relId >= lock_debug_oid_min)) \
		LOCK_DUMP_AUX(where,lock,type)

#define LOCK_DUMP_AUX(where,lock,type)\
		elog(DEBUG, \
			 "%s: pid (%d) rel (%d) dbid (%d) tid (%d,%d) nHolding (%d) "\
			 "holders (%d,%d,%d,%d,%d) type (%s)",where, \
			 MyProcPid,\
			 lock->tag.relId, lock->tag.dbId, \
			 ((lock->tag.tupleId.ip_blkid.bi_hi<<16)+\
			  lock->tag.tupleId.ip_blkid.bi_lo),\
			 lock->tag.tupleId.ip_posid, \
			 lock->nHolding,\
			 lock->holders[1],\
			 lock->holders[2],\
			 lock->holders[3],\
			 lock->holders[4],\
			 lock->holders[5],\
			 lock_types[type])

#define XID_PRINT(where,xidentP)\
	if ((lockDebug >= 2) && \
		(((LOCK *)MAKE_PTR(xidentP->tag.lock))->tag.relId \
		 >= lock_debug_oid_min)) \
		elog(DEBUG,\
			 "%s: pid (%d) xid (%d) pid (%d) lock (%x) nHolding (%d) "\
			 "holders (%d,%d,%d,%d,%d)",\
			 where,\
			 MyProcPid,\
			 xidentP->tag.xid,\
			 xidentP->tag.pid,\
			 xidentP->tag.lock,\
			 xidentP->nHolding,\
			 xidentP->holders[1],\
			 xidentP->holders[2],\
			 xidentP->holders[3],\
			 xidentP->holders[4],\
			 xidentP->holders[5])

#endif							/* LOCK_MGR_DEBUG */

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
	LOCKMETHODTABLE    *lockMethodTable;
	char	   *shmemName;
	HASHCTL		info;
	int			hash_flags;
	bool		found;
	int			status = TRUE;

	if (numModes > MAX_LOCKMODES)
	{
		elog(NOTICE, "LockMethodTableInit: too many lock types %d greater than %d",
			 numModes, MAX_LOCKMODES);
		return (INVALID_TABLEID);
	}

	/* allocate a string for the shmem index table lookup */
	shmemName = (char *) palloc((unsigned) (strlen(tabName) + 32));
	if (!shmemName)
	{
		elog(NOTICE, "LockMethodTableInit: couldn't malloc string %s \n", tabName);
		return (INVALID_TABLEID);
	}

	/* each lock table has a non-shared header */
	lockMethodTable = (LOCKMETHODTABLE *) palloc((unsigned) sizeof(LOCKMETHODTABLE));
	if (!lockMethodTable)
	{
		elog(NOTICE, "LockMethodTableInit: couldn't malloc lock table %s\n", tabName);
		pfree(shmemName);
		return (INVALID_TABLEID);
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
	info.keysize = sizeof(LOCKTAG);
	info.datasize = sizeof(LOCK);
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
	info.keysize = XID_TAGSIZE;
	info.datasize = sizeof(XIDLookupEnt);
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
		return (lockMethodTable->ctl->lockmethod);
	else
		return (INVALID_TABLEID);
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
#ifdef NOT_USED
LOCKMETHOD
LockMethodTableRename(LOCKMETHOD lockmethod)
{
	LOCKMETHOD newLockMethod;

	if (NumLockMethods >= MAX_LOCK_METHODS)
		return (INVALID_TABLEID);
	if (LockMethodTable[lockmethod] == INVALID_TABLEID)
		return (INVALID_TABLEID);

	/* other modules refer to the lock table by a lockmethod */
	newLockMethod = NumLockMethods;
	NumLockMethods++;

	LockMethodTable[newLockMethod] = LockMethodTable[lockmethod];
	return (newLockMethod);
}
#endif

/*
 * LockAcquire -- Check for lock conflicts, sleep if conflict found,
 *		set lock if/when no conflicts.
 *
 * Returns: TRUE if parameters are correct, FALSE otherwise.
 *
 * Side Effects: The lock is always acquired.  No way to abort
 *		a lock acquisition other than aborting the transaction.
 *		Lock is recorded in the lkchain.
#ifdef USER_LOCKS
 * Note on User Locks:
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
 *		They are indicated by a dummy lockmethod 0 which doesn't have
 *		any table allocated but uses the normal lock table, and are
 *		distinguished from normal locks for the following differences:
 *
 *										normal lock		user lock
 *
 *		lockmethod						1				0
 *		tag.relId						rel oid			0
 *		tag.ItemPointerData.ip_blkid	block id		lock id2
 *		tag.ItemPointerData.ip_posid	tuple offset	lock id1
 *		xid.pid							0				backend pid
 *		xid.xid							current xid		0
 *		persistence						transaction		user or backend
 *
 *		The lockmode parameter can have the same values for normal locks
 *		although probably only WRITE_LOCK can have some practical use.
 *
 *														DZ - 4 Oct 1996
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
	LOCKMETHODTABLE    *lockMethodTable;
	int			status;
	TransactionId myXid;

#ifdef USER_LOCKS
	int			is_user_lock;

	is_user_lock = (lockmethod == 0);
	if (is_user_lock)
	{
		lockmethod = 1;
#ifdef USER_LOCKS_DEBUG
		elog(NOTICE, "LockAcquire: user lock tag [%u,%u] %d",
			 locktag->tupleId.ip_posid,
			 ((locktag->tupleId.ip_blkid.bi_hi << 16) +
			  locktag->tupleId.ip_blkid.bi_lo),
			 lockmode);
#endif
	}
#endif

	Assert(lockmethod < NumLockMethods);
	lockMethodTable = LockMethodTable[lockmethod];
	if (!lockMethodTable)
	{
		elog(NOTICE, "LockAcquire: bad lock table %d", lockmethod);
		return (FALSE);
	}

	if (LockingIsDisabled)
		return (TRUE);

	LOCK_PRINT("Acquire", locktag, lockmode);
	masterLock = lockMethodTable->ctl->masterLock;

	SpinAcquire(masterLock);

	Assert(lockMethodTable->lockHash->hash == tag_hash);
	lock = (LOCK *) hash_search(lockMethodTable->lockHash, (Pointer) locktag, HASH_ENTER, &found);

	if (!lock)
	{
		SpinRelease(masterLock);
		elog(FATAL, "LockAcquire: lock table %d is corrupted", lockmethod);
		return (FALSE);
	}

	/* --------------------
	 * if there was nothing else there, complete initialization
	 * --------------------
	 */
	if (!found)
	{
		lock->mask = 0;
		ProcQueueInit(&(lock->waitProcs));
		MemSet((char *) lock->holders, 0, sizeof(int) * MAX_LOCKMODES);
		MemSet((char *) lock->activeHolders, 0, sizeof(int) * MAX_LOCKMODES);
		lock->nHolding = 0;
		lock->nActive = 0;

		Assert(BlockIdEquals(&(lock->tag.tupleId.ip_blkid),
							 &(locktag->tupleId.ip_blkid)));

	}

	/* ------------------
	 * add an element to the lock queue so that we can clear the
	 * locks at end of transaction.
	 * ------------------
	 */
	xidTable = lockMethodTable->xidHash;
	myXid = GetCurrentTransactionId();

	/* ------------------
	 * Zero out all of the tag bytes (this clears the padding bytes for long
	 * word alignment and ensures hashing consistency).
	 * ------------------
	 */
	MemSet(&item, 0, XID_TAGSIZE);		/* must clear padding, needed */
	TransactionIdStore(myXid, &item.tag.xid);
	item.tag.lock = MAKE_OFFSET(lock);
#if 0
	item.tag.pid = MyPid;
#endif

#ifdef USER_LOCKS
	if (is_user_lock)
	{
		item.tag.pid = MyProcPid;
		item.tag.xid = myXid = 0;
#ifdef USER_LOCKS_DEBUG
		elog(NOTICE, "LockAcquire: user lock xid [%d,%d,%d]",
			 item.tag.lock, item.tag.pid, item.tag.xid);
#endif
	}
#endif

	result = (XIDLookupEnt *) hash_search(xidTable, (Pointer) &item, HASH_ENTER, &found);
	if (!result)
	{
		elog(NOTICE, "LockAcquire: xid table corrupted");
		return (STATUS_ERROR);
	}
	if (!found)
	{
		XID_PRINT("LockAcquire: queueing XidEnt", result);
		ProcAddLock(&result->queue);
		result->nHolding = 0;
		MemSet((char *) result->holders, 0, sizeof(int) * MAX_LOCKMODES);
	}

	/* ----------------
	 * lock->nholding tells us how many processes have _tried_ to
	 * acquire this lock,  Regardless of whether they succeeded or
	 * failed in doing so.
	 * ----------------
	 */
	lock->nHolding++;
	lock->holders[lockmode]++;

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
		GrantLock(lock, lockmode);
		SpinRelease(masterLock);
		return (TRUE);
	}

	Assert(result->nHolding <= lock->nActive);

	status = LockResolveConflicts(lockmethod, lock, lockmode, myXid);

	if (status == STATUS_OK)
		GrantLock(lock, lockmode);
	else if (status == STATUS_FOUND)
	{
#ifdef USER_LOCKS

		/*
		 * User locks are non blocking. If we can't acquire a lock remove
		 * the xid entry and return FALSE without waiting.
		 */
		if (is_user_lock)
		{
			if (!result->nHolding)
			{
				SHMQueueDelete(&result->queue);
				hash_search(xidTable, (Pointer) &item, HASH_REMOVE, &found);
			}
			lock->nHolding--;
			lock->holders[lockmode]--;
			SpinRelease(masterLock);
#ifdef USER_LOCKS_DEBUG
			elog(NOTICE, "LockAcquire: user lock failed");
#endif
			return (FALSE);
		}
#endif
		status = WaitOnLock(lockmethod, lock, lockmode);
		XID_PRINT("Someone granted me the lock", result);
	}

	SpinRelease(masterLock);

	return (status == STATUS_OK);
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
					 TransactionId xid)
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

	numLockModes = LockMethodTable[lockmethod]->ctl->numLockModes;
	xidTable = LockMethodTable[lockmethod]->xidHash;

	/* ---------------------
	 * read my own statistics from the xid table.  If there
	 * isn't an entry, then we'll just add one.
	 *
	 * Zero out the tag, this clears the padding bytes for long
	 * word alignment and ensures hashing consistency.
	 * ------------------
	 */
	MemSet(&item, 0, XID_TAGSIZE);
	TransactionIdStore(xid, &item.tag.xid);
	item.tag.lock = MAKE_OFFSET(lock);
#if 0
	item.tag.pid = pid;
#endif

	if (!(result = (XIDLookupEnt *)
		  hash_search(xidTable, (Pointer) &item, HASH_ENTER, &found)))
	{
		elog(NOTICE, "LockResolveConflicts: xid table corrupted");
		return (STATUS_ERROR);
	}
	myHolders = result->holders;

	if (!found)
	{
		/* ---------------
		 * we're not holding any type of lock yet.  Clear
		 * the lock stats.
		 * ---------------
		 */
		MemSet(result->holders, 0, numLockModes * sizeof(*(lock->holders)));
		result->nHolding = 0;
	}

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
			return STATUS_FOUND;
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

		XID_PRINT("Conflict Resolved: updated xid entry stats", result);

		return (STATUS_OK);
	}

	/* ------------------------
	 * Rats.  Something conflicts. But it could still be my own
	 * lock.  We have to construct a conflict mask
	 * that does not reflect our own locks.
	 * ------------------------
	 */
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

		XID_PRINT("Conflict Resolved: updated xid entry stats", result);

		return (STATUS_OK);

	}

	return (STATUS_FOUND);
}

static int
WaitOnLock(LOCKMETHOD lockmethod, LOCK *lock, LOCKMODE lockmode)
{
	PROC_QUEUE *waitQueue = &(lock->waitProcs);
	LOCKMETHODTABLE    *lockMethodTable = LockMethodTable[lockmethod];
	int			prio = lockMethodTable->ctl->prio[lockmode];

	Assert(lockmethod < NumLockMethods);

	/*
	 * the waitqueue is ordered by priority. I insert myself according to
	 * the priority of the lock I am acquiring.
	 *
	 * SYNC NOTE: I am assuming that the lock table spinlock is sufficient
	 * synchronization for this queue.	That will not be true if/when
	 * people can be deleted from the queue by a SIGINT or something.
	 */
	LOCK_DUMP_AUX("WaitOnLock: sleeping on lock", lock, lockmode);
	if (ProcSleep(waitQueue,
				  lockMethodTable->ctl->masterLock,
				  lockmode,
				  prio,
				  lock) != NO_ERROR)
	{
		/* -------------------
		 * This could have happend as a result of a deadlock, see HandleDeadLock()
		 * Decrement the lock nHolding and holders fields as we are no longer
		 * waiting on this lock.
		 * -------------------
		 */
		lock->nHolding--;
		lock->holders[lockmode]--;
		LOCK_DUMP_AUX("WaitOnLock: aborting on lock", lock, lockmode);
		SpinRelease(lockMethodTable->ctl->masterLock);
		elog(ERROR, "WaitOnLock: error on wakeup - Aborting this transaction");
	}

	LOCK_DUMP_AUX("WaitOnLock: wakeup on lock", lock, lockmode);
	return (STATUS_OK);
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
	LOCKMETHODTABLE    *lockMethodTable;
	XIDLookupEnt *result,
				item;
	HTAB	   *xidTable;
	bool		wakeupNeeded = true;

#ifdef USER_LOCKS
	int			is_user_lock;

	is_user_lock = (lockmethod == 0);
	if (is_user_lock)
	{
		lockmethod = 1;
#ifdef USER_LOCKS_DEBUG
		elog(NOTICE, "LockRelease: user lock tag [%u,%u] %d",
			 locktag->tupleId.ip_posid,
			 ((locktag->tupleId.ip_blkid.bi_hi << 16) +
			  locktag->tupleId.ip_blkid.bi_lo),
			 lockmode);
#endif
	}
#endif

	Assert(lockmethod < NumLockMethods);
	lockMethodTable = LockMethodTable[lockmethod];
	if (!lockMethodTable)
	{
		elog(NOTICE, "lockMethodTable is null in LockRelease");
		return (FALSE);
	}

	if (LockingIsDisabled)
		return (TRUE);

	LOCK_PRINT("Release", locktag, lockmode);

	masterLock = lockMethodTable->ctl->masterLock;
	xidTable = lockMethodTable->xidHash;

	SpinAcquire(masterLock);

	Assert(lockMethodTable->lockHash->hash == tag_hash);
	lock = (LOCK *)
		hash_search(lockMethodTable->lockHash, (Pointer) locktag, HASH_FIND_SAVE, &found);

#ifdef USER_LOCKS

	/*
	 * If the entry is not found hash_search returns TRUE instead of NULL,
	 * so we must check it explicitly.
	 */
	if ((is_user_lock) && (lock == (LOCK *) TRUE))
	{
		SpinRelease(masterLock);
		elog(NOTICE, "LockRelease: there are no locks with this tag");
		return (FALSE);
	}
#endif

	/*
	 * let the caller print its own error message, too. Do not
	 * elog(ERROR).
	 */
	if (!lock)
	{
		SpinRelease(masterLock);
		elog(NOTICE, "LockRelease: locktable corrupted");
		return (FALSE);
	}

	if (!found)
	{
		SpinRelease(masterLock);
		elog(NOTICE, "LockRelease: locktable lookup failed, no lock");
		return (FALSE);
	}

	Assert(lock->nHolding > 0);

#ifdef USER_LOCKS

	/*
	 * If this is an user lock it can be removed only after checking that
	 * it was acquired by the current process, so this code is skipped and
	 * executed later.
	 */
	if (!is_user_lock)
	{
#endif

		/*
		 * fix the general lock stats
		 */
		lock->nHolding--;
		lock->holders[lockmode]--;
		lock->nActive--;
		lock->activeHolders[lockmode]--;

		Assert(lock->nActive >= 0);

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
										HASH_REMOVE_SAVED,
										&found);
			Assert(lock && found);
			wakeupNeeded = false;
		}
#ifdef USER_LOCKS
	}
#endif

	/* ------------------
	 * Zero out all of the tag bytes (this clears the padding bytes for long
	 * word alignment and ensures hashing consistency).
	 * ------------------
	 */
	MemSet(&item, 0, XID_TAGSIZE);

	TransactionIdStore(GetCurrentTransactionId(), &item.tag.xid);
	item.tag.lock = MAKE_OFFSET(lock);
#if 0
	item.tag.pid = MyPid;
#endif

#ifdef USER_LOCKS
	if (is_user_lock)
	{
		item.tag.pid = MyProcPid;
		item.tag.xid = 0;
#ifdef USER_LOCKS_DEBUG
		elog(NOTICE, "LockRelease: user lock xid [%d,%d,%d]",
			 item.tag.lock, item.tag.pid, item.tag.xid);
#endif
	}
#endif

	if (!(result = (XIDLookupEnt *) hash_search(xidTable,
												(Pointer) &item,
												HASH_FIND_SAVE,
												&found))
		|| !found)
	{
		SpinRelease(masterLock);
#ifdef USER_LOCKS
		if ((is_user_lock) && (result))
			elog(NOTICE, "LockRelease: you don't have a lock on this tag");
		else
			elog(NOTICE, "LockRelease: find xid, table corrupted");
#else
		elog(NOTICE, "LockReplace: xid table corrupted");
#endif
		return (FALSE);
	}

	/*
	 * now check to see if I have any private locks.  If I do, decrement
	 * the counts associated with them.
	 */
	result->holders[lockmode]--;
	result->nHolding--;

	XID_PRINT("LockRelease updated xid stats", result);

	/*
	 * If this was my last hold on this lock, delete my entry in the XID
	 * table.
	 */
	if (!result->nHolding)
	{
#ifdef USER_LOCKS
		if (result->queue.prev == INVALID_OFFSET)
			elog(NOTICE, "LockRelease: xid.prev == INVALID_OFFSET");
		if (result->queue.next == INVALID_OFFSET)
			elog(NOTICE, "LockRelease: xid.next == INVALID_OFFSET");
#endif
		if (result->queue.next != INVALID_OFFSET)
			SHMQueueDelete(&result->queue);
		if (!(result = (XIDLookupEnt *)
			  hash_search(xidTable, (Pointer) &item, HASH_REMOVE_SAVED, &found)) ||
			!found)
		{
			SpinRelease(masterLock);
#ifdef USER_LOCKS
			elog(NOTICE, "LockRelease: remove xid, table corrupted");
#else
			elog(NOTICE, "LockReplace: xid table corrupted");
#endif
			return (FALSE);
		}
	}

#ifdef USER_LOCKS

	/*
	 * If this is an user lock remove it now, after the corresponding xid
	 * entry has been found and deleted.
	 */
	if (is_user_lock)
	{

		/*
		 * fix the general lock stats
		 */
		lock->nHolding--;
		lock->holders[lockmode]--;
		lock->nActive--;
		lock->activeHolders[lockmode]--;

		Assert(lock->nActive >= 0);

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
	}
#endif

	/* --------------------------
	 * If there are still active locks of the type I just released, no one
	 * should be woken up.	Whoever is asleep will still conflict
	 * with the remaining locks.
	 * --------------------------
	 */
	if (!(lock->activeHolders[lockmode]))
	{
		/* change the conflict mask.  No more of this lock type. */
		lock->mask &= BITS_OFF[lockmode];
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

	SpinRelease(masterLock);
	return (TRUE);
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
}

#ifdef USER_LOCKS
/*
 * LockReleaseAll -- Release all locks in a process lock queue.
 *
 * Note: This code is a little complicated by the presence in the
 *		 same queue of user locks which can't be removed from the
 *		 normal lock queue at the end of a transaction. They must
 *		 however be removed when the backend exits.
 *		 A dummy lockmethod 0 is used to indicate that we are releasing
 *		 the user locks, from the code added to ProcKill().
 */
#endif
bool
LockReleaseAll(LOCKMETHOD lockmethod, SHM_QUEUE *lockQueue)
{
	PROC_QUEUE *waitQueue;
	int			done;
	XIDLookupEnt *xidLook = NULL;
	XIDLookupEnt *tmp = NULL;
	SHMEM_OFFSET end = MAKE_OFFSET(lockQueue);
	SPINLOCK	masterLock;
	LOCKMETHODTABLE    *lockMethodTable;
	int			i,
				numLockModes;
	LOCK	   *lock;
	bool		found;

#ifdef USER_LOCKS
	int			is_user_lock_table,
				count,
				nskip;

	is_user_lock_table = (lockmethod == 0);
#ifdef USER_LOCKS_DEBUG
	elog(NOTICE, "LockReleaseAll: lockmethod=%d, pid=%d", lockmethod, MyProcPid);
#endif
	if (is_user_lock_table)
		lockmethod = 1;
#endif

	Assert(lockmethod < NumLockMethods);
	lockMethodTable = LockMethodTable[lockmethod];
	if (!lockMethodTable)
		return (FALSE);

	numLockModes = lockMethodTable->ctl->numLockModes;
	masterLock = lockMethodTable->ctl->masterLock;

	if (SHMQueueEmpty(lockQueue))
		return TRUE;

#ifdef USER_LOCKS
	SpinAcquire(masterLock);
#endif
	SHMQueueFirst(lockQueue, (Pointer *) &xidLook, &xidLook->queue);

	XID_PRINT("LockReleaseAll", xidLook);

#ifndef USER_LOCKS
	SpinAcquire(masterLock);
#else
	count = nskip = 0;
#endif
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

		LOCK_PRINT("LockReleaseAll", (&lock->tag), 0);

#ifdef USER_LOCKS

		/*
		 * Sometimes the queue appears to be messed up.
		 */
		if (count++ > 2000)
		{
			elog(NOTICE, "LockReleaseAll: xid loop detected, giving up");
			nskip = 0;
			break;
		}
		if (is_user_lock_table)
		{
			if ((xidLook->tag.pid == 0) || (xidLook->tag.xid != 0))
			{
#ifdef USER_LOCKS_DEBUG
				elog(NOTICE, "LockReleaseAll: skip normal lock [%d,%d,%d]",
				  xidLook->tag.lock, xidLook->tag.pid, xidLook->tag.xid);
#endif
				nskip++;
				goto next_item;
			}
			if (xidLook->tag.pid != MyProcPid)
			{
				/* This should never happen */
#ifdef USER_LOCKS_DEBUG
				elog(NOTICE,
					 "LockReleaseAll: skip other pid [%u,%u] [%d,%d,%d]",
					 lock->tag.tupleId.ip_posid,
					 ((lock->tag.tupleId.ip_blkid.bi_hi << 16) +
					  lock->tag.tupleId.ip_blkid.bi_lo),
				  xidLook->tag.lock, xidLook->tag.pid, xidLook->tag.xid);
#endif
				nskip++;
				goto next_item;
			}
#ifdef USER_LOCKS_DEBUG
			elog(NOTICE,
				 "LockReleaseAll: release user lock [%u,%u] [%d,%d,%d]",
				 lock->tag.tupleId.ip_posid,
				 ((lock->tag.tupleId.ip_blkid.bi_hi << 16) +
				  lock->tag.tupleId.ip_blkid.bi_lo),
				 xidLook->tag.lock, xidLook->tag.pid, xidLook->tag.xid);
#endif
		}
		else
		{
			if ((xidLook->tag.pid != 0) || (xidLook->tag.xid == 0))
			{
#ifdef USER_LOCKS_DEBUG
				elog(NOTICE,
					 "LockReleaseAll: skip user lock [%u,%u] [%d,%d,%d]",
					 lock->tag.tupleId.ip_posid,
					 ((lock->tag.tupleId.ip_blkid.bi_hi << 16) +
					  lock->tag.tupleId.ip_blkid.bi_lo),
				  xidLook->tag.lock, xidLook->tag.pid, xidLook->tag.xid);
#endif
				nskip++;
				goto next_item;
			}
#ifdef USER_LOCKS_DEBUG
			elog(NOTICE, "LockReleaseAll: release normal lock [%d,%d,%d]",
				 xidLook->tag.lock, xidLook->tag.pid, xidLook->tag.xid);
#endif
		}
#endif

		/* ------------------
		 * fix the general lock stats
		 * ------------------
		 */
		if (lock->nHolding != xidLook->nHolding)
		{
			lock->nHolding -= xidLook->nHolding;
			lock->nActive -= xidLook->nHolding;
			Assert(lock->nActive >= 0);
			for (i = 1; i <= numLockModes; i++)
			{
				lock->holders[i] -= xidLook->holders[i];
				lock->activeHolders[i] -= xidLook->holders[i];
				if (!lock->activeHolders[i])
					lock->mask &= BITS_OFF[i];
			}
		}
		else
		{
			/* --------------
			 * set nHolding to zero so that we can garbage collect the lock
			 * down below...
			 * --------------
			 */
			lock->nHolding = 0;
		}
		/* ----------------
		 * always remove the xidLookup entry, we're done with it now
		 * ----------------
		 */
#ifdef USER_LOCKS
		SHMQueueDelete(&xidLook->queue);
#endif
		if ((!hash_search(lockMethodTable->xidHash, (Pointer) xidLook, HASH_REMOVE, &found))
			|| !found)
		{
			SpinRelease(masterLock);
			elog(NOTICE, "LockReleaseAll: xid table corrupted");
			return (FALSE);
		}

		if (!lock->nHolding)
		{
			/* --------------------
			 * if there's no one waiting in the queue, we've just released
			 * the last lock.
			 * --------------------
			 */

			Assert(lockMethodTable->lockHash->hash == tag_hash);
			lock = (LOCK *)
				hash_search(lockMethodTable->lockHash, (Pointer) &(lock->tag), HASH_REMOVE, &found);
			if ((!lock) || (!found))
			{
				SpinRelease(masterLock);
				elog(NOTICE, "LockReleaseAll: cannot remove lock from HTAB");
				return (FALSE);
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
	SpinRelease(masterLock);
#ifdef USER_LOCKS

	/*
	 * Reinitialize the queue only if nothing has been left in.
	 */
	if (nskip == 0)
#endif
		SHMQueueInit(lockQueue);
	return TRUE;
}

int
LockShmemSize()
{
	int			size = 0;
	int			nLockBuckets,
				nLockSegs;
	int			nXidBuckets,
				nXidSegs;

	nLockBuckets = 1 << (int) my_log2((NLOCKENTS - 1) / DEF_FFACTOR + 1);
	nLockSegs = 1 << (int) my_log2((nLockBuckets - 1) / DEF_SEGSIZE + 1);

	nXidBuckets = 1 << (int) my_log2((NLOCKS_PER_XACT - 1) / DEF_FFACTOR + 1);
	nXidSegs = 1 << (int) my_log2((nLockBuckets - 1) / DEF_SEGSIZE + 1);

	size += MAXALIGN(NBACKENDS * sizeof(PROC)); /* each MyProc */
	size += MAXALIGN(NBACKENDS * sizeof(LOCKMETHODCTL)); /* each lockMethodTable->ctl */
	size += MAXALIGN(sizeof(PROC_HDR)); /* ProcGlobal */

	size += MAXALIGN(my_log2(NLOCKENTS) * sizeof(void *));
	size += MAXALIGN(sizeof(HHDR));
	size += nLockSegs * MAXALIGN(DEF_SEGSIZE * sizeof(SEGMENT));
	size += NLOCKENTS *			/* XXX not multiple of BUCKET_ALLOC_INCR? */
		(MAXALIGN(sizeof(BUCKET_INDEX)) +
		 MAXALIGN(sizeof(LOCK)));		/* contains hash key */

	size += MAXALIGN(my_log2(NBACKENDS) * sizeof(void *));
	size += MAXALIGN(sizeof(HHDR));
	size += nXidSegs * MAXALIGN(DEF_SEGSIZE * sizeof(SEGMENT));
	size += NBACKENDS *			/* XXX not multiple of BUCKET_ALLOC_INCR? */
		(MAXALIGN(sizeof(BUCKET_INDEX)) +
		 MAXALIGN(sizeof(XIDLookupEnt)));		/* contains hash key */

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

	LOCKMETHODTABLE    *lockMethodTable;
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

		lockMethodTable = LockMethodTable[1];
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

		LOCK_PRINT("DeadLockCheck", (&lock->tag), 0);

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

#ifdef DEADLOCK_DEBUG
/*
 * Dump all locks. Must have already acquired the masterLock.
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

	count;
	int			lockmethod = 1;
	LOCKMETHODTABLE    *lockMethodTable;

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

	LOCK_DUMP("DumpLocks", MyProc->waitLock, 0);
	XID_PRINT("DumpLocks", xidLook);

	for (count = 0;;)
	{
		/* ---------------------------
		 * XXX Here we assume the shared memory queue is circular and
		 * that we know its internal structure.  Should have some sort of
		 * macros to allow one to walk it.	mer 20 July 1991
		 * ---------------------------
		 */
		done = (xidLook->queue.next == end);
		lock = (LOCK *) MAKE_PTR(xidLook->tag.lock);

		LOCK_DUMP("DumpLocks", lock, 0);

		if (count++ > 2000)
		{
			elog(NOTICE, "DumpLocks: xid loop detected, giving up");
			break;
		}

		if (done)
			break;
		SHMQueueFirst(&xidLook->queue, (Pointer *) &tmp, &tmp->queue);
		xidLook = tmp;
	}
}

#endif
