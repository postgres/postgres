/*-------------------------------------------------------------------------
 *
 * lock.c--
 *    simple lock acquisition
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/storage/lmgr/lock.c,v 1.1.1.1 1996/07/09 06:21:56 scrappy Exp $
 *
 * NOTES
 *    Outside modules can create a lock table and acquire/release
 *    locks.  A lock table is a shared memory hash table.  When
 *    a process tries to acquire a lock of a type that conflicts
 *    with existing locks, it is put to sleep using the routines
 *    in storage/lmgr/proc.c.
 *
 *  Interface:
 *
 *  LockAcquire(), LockRelease(), LockTabInit().
 *
 *  LockReplace() is called only within this module and by the
 *  	lkchain module.  It releases a lock without looking
 * 	the lock up in the lock table.
 *
 *  NOTE: This module is used to define new lock tables.  The
 *	multi-level lock table (multi.c) used by the heap
 *	access methods calls these routines.  See multi.c for
 *	examples showing how to use this interface.
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>		/* for sprintf() */
#include "storage/shmem.h"
#include "storage/spin.h"
#include "storage/proc.h"
#include "storage/lock.h"
#include "utils/hsearch.h"
#include "utils/elog.h"
#include "utils/palloc.h"
#include "access/xact.h"

/*#define LOCK_MGR_DEBUG*/

#ifndef LOCK_MGR_DEBUG

#define LOCK_PRINT(where,tag,type)
#define LOCK_DUMP(where,lock,type)
#define XID_PRINT(where,xidentP)

#else /* LOCK_MGR_DEBUG */

#define LOCK_PRINT(where,tag,type)\
  elog(NOTICE, "%s: rel (%d) dbid (%d) tid (%d,%d) type (%d)\n",where, \
	 tag->relId, tag->dbId, \
	 ( (tag->tupleId.ip_blkid.data[0] >= 0) ? \
		BlockIdGetBlockNumber(&tag->tupleId.ip_blkid) : -1 ), \
	 tag->tupleId.ip_posid, \
	 type);

#define LOCK_DUMP(where,lock,type)\
  elog(NOTICE, "%s: rel (%d) dbid (%d) tid (%d,%d) nHolding (%d) holders (%d,%d,%d,%d,%d) type (%d)\n",where, \
       lock->tag.relId, lock->tag.dbId, \
       ((lock->tag.tupleId.ip_blkid.data[0] >= 0) ? \
	BlockIdGetBlockNumber(&lock->tag.tupleId.ip_blkid) : -1 ), \
       lock->tag.tupleId.ip_posid, \
       lock->nHolding,\
       lock->holders[1],\
       lock->holders[2],\
       lock->holders[3],\
       lock->holders[4],\
       lock->holders[5],\
       type);

#define XID_PRINT(where,xidentP)\
  elog(NOTICE,\
       "%s:xid (%d) pid (%d) lock (%x) nHolding (%d) holders (%d,%d,%d,%d,%d)",\
       where,\
       xidentP->tag.xid,\
       xidentP->tag.pid,\
       xidentP->tag.lock,\
       xidentP->nHolding,\
       xidentP->holders[1],\
       xidentP->holders[2],\
       xidentP->holders[3],\
       xidentP->holders[4],\
       xidentP->holders[5]);

#endif /* LOCK_MGR_DEBUG */

SPINLOCK LockMgrLock;		/* in Shmem or created in CreateSpinlocks() */

/* This is to simplify/speed up some bit arithmetic */

static MASK	BITS_OFF[MAX_LOCKTYPES];
static MASK	BITS_ON[MAX_LOCKTYPES];

/* -----------------
 * XXX Want to move this to this file
 * -----------------
 */
static bool LockingIsDisabled;

/* ------------------
 * from storage/ipc/shmem.c
 * ------------------
 */
extern HTAB *ShmemInitHash();

/* -------------------
 * map from tableId to the lock table structure
 * -------------------
 */
static LOCKTAB *AllTables[MAX_TABLES];

/* -------------------
 * no zero-th table
 * -------------------
 */
static int	NumTables = 1;

/* -------------------
 * InitLocks -- Init the lock module.  Create a private data
 *	structure for constructing conflict masks.
 * -------------------
 */
void
InitLocks()
{
    int i;
    int bit;
    
    bit = 1;
    /* -------------------
     * remember 0th locktype is invalid
     * -------------------
     */
    for (i=0;i<MAX_LOCKTYPES;i++,bit <<= 1)
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
 * LockTypeInit -- initialize the lock table's lock type
 *	structures
 *
 * Notes: just copying.  Should only be called once.
 */
static void
LockTypeInit(LOCKTAB *ltable,
	     MASK *conflictsP,
	     int *prioP,
	     int ntypes)
{
    int	i;
    
    ltable->ctl->nLockTypes = ntypes;
    ntypes++;
    for (i=0;i<ntypes;i++,prioP++,conflictsP++)
	{
	    ltable->ctl->conflictTab[i] = *conflictsP;
	    ltable->ctl->prio[i] = *prioP;
	}
}

/*
 * LockTabInit -- initialize a lock table structure
 *
 * Notes:
 *	(a) a lock table has four separate entries in the binding
 *	table.  This is because every shared hash table and spinlock
 *	has its name stored in the binding table at its creation.  It
 *	is wasteful, in this case, but not much space is involved.
 *
 */
LockTableId
LockTabInit(char *tabName,
	    MASK *conflictsP,
	    int *prioP,
	    int ntypes)
{
    LOCKTAB *ltable;
    char *shmemName;
    HASHCTL info;
    int hash_flags;
    bool	found;
    int status = TRUE;
    
    if (ntypes > MAX_LOCKTYPES)
	{
	    elog(NOTICE,"LockTabInit: too many lock types %d greater than %d",
		 ntypes,MAX_LOCKTYPES);
	    return(INVALID_TABLEID);
	}
    
    if (NumTables > MAX_TABLES)
	{
	    elog(NOTICE,
		 "LockTabInit: system limit of MAX_TABLES (%d) lock tables",
		 MAX_TABLES);
	    return(INVALID_TABLEID);
	}
    
    /* allocate a string for the binding table lookup */
    shmemName = (char *) palloc((unsigned)(strlen(tabName)+32));
    if (! shmemName)
	{
	    elog(NOTICE,"LockTabInit: couldn't malloc string %s \n",tabName);
	    return(INVALID_TABLEID);
	}
    
    /* each lock table has a non-shared header */
    ltable = (LOCKTAB *) palloc((unsigned) sizeof(LOCKTAB));
    if (! ltable)
	{
	    elog(NOTICE,"LockTabInit: couldn't malloc lock table %s\n",tabName);
	    (void) pfree (shmemName);
	    return(INVALID_TABLEID);
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
    sprintf(shmemName,"%s (ctl)",tabName);
    ltable->ctl = (LOCKCTL *)
	ShmemInitStruct(shmemName,(unsigned)sizeof(LOCKCTL),&found);
    
    if (! ltable->ctl)
	{
	    elog(FATAL,"LockTabInit: couldn't initialize %s",tabName);
	    status = FALSE;
	}
    
    /* ----------------
     * we're first - initialize
     * ----------------
     */
    if (! found)
	{
	    memset(ltable->ctl, 0, sizeof(LOCKCTL)); 
	    ltable->ctl->masterLock = LockMgrLock;
	    ltable->ctl->tableId = NumTables;
	}
    
    /* --------------------
     * other modules refer to the lock table by a tableId
     * --------------------
     */
    AllTables[NumTables] = ltable;
    NumTables++;
    Assert(NumTables <= MAX_TABLES);
    
    /* ----------------------
     * allocate a hash table for the lock tags.  This is used
     * to find the different locks.
     * ----------------------
     */
    info.keysize =  sizeof(LOCKTAG);
    info.datasize = sizeof(LOCK);
    info.hash = tag_hash;
    hash_flags = (HASH_ELEM | HASH_FUNCTION);
    
    sprintf(shmemName,"%s (lock hash)",tabName);
    ltable->lockHash = (HTAB *) ShmemInitHash(shmemName,
					      INIT_TABLE_SIZE,MAX_TABLE_SIZE,
					      &info,hash_flags);
    
    Assert( ltable->lockHash->hash == tag_hash);
    if (! ltable->lockHash)
	{
	    elog(FATAL,"LockTabInit: couldn't initialize %s",tabName);
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
    
    sprintf(shmemName,"%s (xid hash)",tabName);
    ltable->xidHash = (HTAB *) ShmemInitHash(shmemName,
					     INIT_TABLE_SIZE,MAX_TABLE_SIZE,
					     &info,hash_flags);
    
    if (! ltable->xidHash)
	{
	    elog(FATAL,"LockTabInit: couldn't initialize %s",tabName);
	    status = FALSE;
	}
    
    /* init ctl data structures */
    LockTypeInit(ltable, conflictsP, prioP, ntypes);
    
    SpinRelease(LockMgrLock);
    
    (void) pfree (shmemName);
    
    if (status)
	return(ltable->ctl->tableId);
    else
	return(INVALID_TABLEID);
}

/*
 * LockTabRename -- allocate another tableId to the same
 *	lock table.
 *
 * NOTES: Both the lock module and the lock chain (lchain.c)
 *	module use table id's to distinguish between different
 *	kinds of locks.  Short term and long term locks look
 *	the same to the lock table, but are handled differently
 *	by the lock chain manager.  This function allows the
 *	client to use different tableIds when acquiring/releasing
 *	short term and long term locks.
 */
LockTableId
LockTabRename(LockTableId tableId)
{
    LockTableId	newTableId;
    
    if (NumTables >= MAX_TABLES)
	{
	    return(INVALID_TABLEID);
	}
    if (AllTables[tableId] == INVALID_TABLEID)
	{
	    return(INVALID_TABLEID);
	}
    
    /* other modules refer to the lock table by a tableId */
    newTableId = NumTables;
    NumTables++;
    
    AllTables[newTableId] = AllTables[tableId];
    return(newTableId);
}

/*
 * LockAcquire -- Check for lock conflicts, sleep if conflict found,
 *	set lock if/when no conflicts.
 *
 * Returns: TRUE if parameters are correct, FALSE otherwise.
 *
 * Side Effects: The lock is always acquired.  No way to abort
 *	a lock acquisition other than aborting the transaction.
 *	Lock is recorded in the lkchain.
 */
bool
LockAcquire(LockTableId tableId, LOCKTAG *lockName, LOCKT lockt)
{
    XIDLookupEnt	*result,item;
    HTAB		*xidTable;
    bool	found;
    LOCK		*lock = NULL;
    SPINLOCK 	masterLock;
    LOCKTAB 	*ltable;
    int 		status;
    TransactionId	myXid;
    
    Assert (tableId < NumTables);
    ltable = AllTables[tableId];
    if (!ltable)
	{
	    elog(NOTICE,"LockAcquire: bad lock table %d",tableId);
	    return  (FALSE);
	}
    
    if (LockingIsDisabled)
	{
	    return(TRUE);
	}
    
    LOCK_PRINT("Acquire",lockName,lockt);
    masterLock = ltable->ctl->masterLock;
    
    SpinAcquire(masterLock);
    
    Assert( ltable->lockHash->hash == tag_hash);
    lock = (LOCK *)hash_search(ltable->lockHash,(Pointer)lockName,HASH_ENTER,&found);
    
    if (! lock)
	{
	    SpinRelease(masterLock);
	    elog(FATAL,"LockAcquire: lock table %d is corrupted",tableId);
	    return(FALSE);
	}
    
    /* --------------------
     * if there was nothing else there, complete initialization
     * --------------------
     */
    if  (! found)
	{
	    lock->mask = 0;
	    ProcQueueInit(&(lock->waitProcs));
	    memset((char *)lock->holders, 0, sizeof(int)*MAX_LOCKTYPES);
	    memset((char *)lock->activeHolders, 0, sizeof(int)*MAX_LOCKTYPES);
	    lock->nHolding = 0;
	    lock->nActive = 0;
	    
	    Assert(BlockIdEquals(&(lock->tag.tupleId.ip_blkid),
				 &(lockName->tupleId.ip_blkid)));
	    
	}
    
    /* ------------------
     * add an element to the lock queue so that we can clear the
     * locks at end of transaction.
     * ------------------
     */
    xidTable = ltable->xidHash;
    myXid = GetCurrentTransactionId();
    
    /* ------------------
     * Zero out all of the tag bytes (this clears the padding bytes for long
     * word alignment and ensures hashing consistency).
     * ------------------
     */
    memset(&item, 0, XID_TAGSIZE); 
    TransactionIdStore(myXid, &item.tag.xid);
    item.tag.lock = MAKE_OFFSET(lock);
#if 0
    item.tag.pid = MyPid;
#endif
    
    result = (XIDLookupEnt *)hash_search(xidTable, (Pointer)&item, HASH_ENTER, &found);
    if (!result)
	{
	    elog(NOTICE,"LockAcquire: xid table corrupted");
	    return(STATUS_ERROR);
	}
    if (!found)
	{
	    XID_PRINT("queueing XidEnt LockAcquire:", result);
	    ProcAddLock(&result->queue);
	    result->nHolding = 0;
	    memset((char *)result->holders, 0, sizeof(int)*MAX_LOCKTYPES);
	}
    
    /* ----------------
     * lock->nholding tells us how many processes have _tried_ to
     * acquire this lock,  Regardless of whether they succeeded or
     * failed in doing so.
     * ----------------
     */
    lock->nHolding++;
    lock->holders[lockt]++;
    
    /* --------------------
     * If I'm the only one holding a lock, then there
     * cannot be a conflict.  Need to subtract one from the
     * lock's count since we just bumped the count up by 1 
     * above.
     * --------------------
     */
    if (result->nHolding == lock->nActive)
	{
	    result->holders[lockt]++;
	    result->nHolding++;
	    GrantLock(lock, lockt);
	    SpinRelease(masterLock);
	    return(TRUE);
	}
    
    Assert(result->nHolding <= lock->nActive);
    
    status = LockResolveConflicts(ltable, lock, lockt, myXid);
    
    if (status == STATUS_OK)
	{
	    GrantLock(lock, lockt);
	}
    else if (status == STATUS_FOUND)
	{
	    status = WaitOnLock(ltable, tableId, lock, lockt);
	    XID_PRINT("Someone granted me the lock", result);
	}
    
    SpinRelease(masterLock);
    
    return(status == STATUS_OK);
}

/* ----------------------------
 * LockResolveConflicts -- test for lock conflicts
 *
 * NOTES:
 * 	Here's what makes this complicated: one transaction's
 * locks don't conflict with one another.  When many processes
 * hold locks, each has to subtract off the other's locks when
 * determining whether or not any new lock acquired conflicts with
 * the old ones.
 *
 *  For example, if I am already holding a WRITE_INTENT lock,
 *  there will not be a conflict with my own READ_LOCK.  If I
 *  don't consider the intent lock when checking for conflicts,
 *  I find no conflict.
 * ----------------------------
 */
int
LockResolveConflicts(LOCKTAB *ltable,
		     LOCK *lock,
		     LOCKT lockt,
		     TransactionId xid)
{
    XIDLookupEnt	*result,item;
    int		*myHolders;
    int		nLockTypes;
    HTAB		*xidTable;
    bool	found;
    int		bitmask;
    int 		i,tmpMask;
    
    nLockTypes = ltable->ctl->nLockTypes;
    xidTable = ltable->xidHash;
    
    /* ---------------------
     * read my own statistics from the xid table.  If there
     * isn't an entry, then we'll just add one.
     *
     * Zero out the tag, this clears the padding bytes for long
     * word alignment and ensures hashing consistency.
     * ------------------
     */
    memset(&item, 0, XID_TAGSIZE);
    TransactionIdStore(xid, &item.tag.xid);
    item.tag.lock = MAKE_OFFSET(lock);
#if 0
    item.tag.pid = pid;
#endif
    
    if (! (result = (XIDLookupEnt *)
	   hash_search(xidTable, (Pointer)&item, HASH_ENTER, &found)))
	{
	    elog(NOTICE,"LockResolveConflicts: xid table corrupted");
	    return(STATUS_ERROR);
	}
    myHolders = result->holders;
    
    if (! found)
	{
	    /* ---------------
	     * we're not holding any type of lock yet.  Clear
	     * the lock stats.
	     * ---------------
	     */
	    memset(result->holders, 0, nLockTypes * sizeof(*(lock->holders))); 
	    result->nHolding = 0;
	}
    
    /* ----------------------------
     * first check for global conflicts: If no locks conflict
     * with mine, then I get the lock.
     *
     * Checking for conflict: lock->mask represents the types of
     * currently held locks.  conflictTable[lockt] has a bit
     * set for each type of lock that conflicts with mine.  Bitwise
     * compare tells if there is a conflict.
     * ----------------------------
     */
    if (! (ltable->ctl->conflictTab[lockt] & lock->mask))
	{
	    
	    result->holders[lockt]++;
	    result->nHolding++;
	    
	    XID_PRINT("Conflict Resolved: updated xid entry stats", result);
	    
	    return(STATUS_OK);
	}
    
    /* ------------------------
     * Rats.  Something conflicts. But it could still be my own
     * lock.  We have to construct a conflict mask
     * that does not reflect our own locks.
     * ------------------------
     */
    bitmask = 0;
    tmpMask = 2;
    for (i=1;i<=nLockTypes;i++, tmpMask <<= 1)
	{
	    if (lock->activeHolders[i] - myHolders[i])
		{
		    bitmask |= tmpMask;
		}
	}
    
    /* ------------------------
     * now check again for conflicts.  'bitmask' describes the types
     * of locks held by other processes.  If one of these
     * conflicts with the kind of lock that I want, there is a
     * conflict and I have to sleep.
     * ------------------------
     */
    if (! (ltable->ctl->conflictTab[lockt] & bitmask))
	{
	    
	    /* no conflict. Get the lock and go on */
	    
	    result->holders[lockt]++;
	    result->nHolding++;
	    
	    XID_PRINT("Conflict Resolved: updated xid entry stats", result);
	    
	    return(STATUS_OK);
	    
	}
    
    return(STATUS_FOUND);
}

int
WaitOnLock(LOCKTAB *ltable, LockTableId tableId, LOCK *lock, LOCKT lockt)
{
    PROC_QUEUE *waitQueue = &(lock->waitProcs);
    
    int prio = ltable->ctl->prio[lockt];
    
    /* the waitqueue is ordered by priority. I insert myself
     * according to the priority of the lock I am acquiring.
     *
     * SYNC NOTE: I am assuming that the lock table spinlock
     * is sufficient synchronization for this queue.  That
     * will not be true if/when people can be deleted from
     * the queue by a SIGINT or something.
     */
    LOCK_DUMP("WaitOnLock: sleeping on lock", lock, lockt);
    if (ProcSleep(waitQueue,
		  ltable->ctl->masterLock,
		  lockt,
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
	    lock->holders[lockt]--;
	    LOCK_DUMP("WaitOnLock: aborting on lock", lock, lockt);
	    SpinRelease(ltable->ctl->masterLock);
	    elog(WARN,"WaitOnLock: error on wakeup - Aborting this transaction");
	}
    
    return(STATUS_OK);
}

/*
 * LockRelease -- look up 'lockName' in lock table 'tableId' and
 *	release it.
 *
 * Side Effects: if the lock no longer conflicts with the highest
 *	priority waiting process, that process is granted the lock
 *	and awoken. (We have to grant the lock here to avoid a
 *	race between the waking process and any new process to
 *	come along and request the lock).
 */
bool
LockRelease(LockTableId tableId, LOCKTAG *lockName, LOCKT lockt)
{
    LOCK		*lock = NULL;
    SPINLOCK 	masterLock;
    bool	found;
    LOCKTAB 	*ltable;
    XIDLookupEnt	*result,item;
    HTAB 		*xidTable;
    bool		wakeupNeeded = true;
    
    Assert (tableId < NumTables);
    ltable = AllTables[tableId];
    if (!ltable) {
	elog(NOTICE, "ltable is null in LockRelease");
	return (FALSE);
    }
    
    if (LockingIsDisabled)
	{
	    return(TRUE);
	}
    
    LOCK_PRINT("Release",lockName,lockt);
    
    masterLock = ltable->ctl->masterLock;
    xidTable = ltable->xidHash;
    
    SpinAcquire(masterLock);
    
    Assert( ltable->lockHash->hash == tag_hash);
    lock = (LOCK *)
	hash_search(ltable->lockHash,(Pointer)lockName,HASH_FIND_SAVE,&found);
    
    /* let the caller print its own error message, too.
     * Do not elog(WARN).
     */
    if (! lock)
	{
	    SpinRelease(masterLock);
	    elog(NOTICE,"LockRelease: locktable corrupted");
	    return(FALSE);
	}
    
    if (! found)
	{
	    SpinRelease(masterLock);
	    elog(NOTICE,"LockRelease: locktable lookup failed, no lock");
	    return(FALSE);
	}
    
    Assert(lock->nHolding > 0);
    
    /*
     * fix the general lock stats
     */
    lock->nHolding--;
    lock->holders[lockt]--;
    lock->nActive--;
    lock->activeHolders[lockt]--;
    
    Assert(lock->nActive >= 0);
    
    if (! lock->nHolding)
	{
	    /* ------------------
	     * if there's no one waiting in the queue,
	     * we just released the last lock.
	     * Delete it from the lock table.
	     * ------------------
	     */
	    Assert( ltable->lockHash->hash == tag_hash);
	    lock = (LOCK *) hash_search(ltable->lockHash,
					(Pointer) &(lock->tag),
					HASH_REMOVE_SAVED,
					&found);
	    Assert(lock && found);
	    wakeupNeeded = false;
	}
    
    /* ------------------
     * Zero out all of the tag bytes (this clears the padding bytes for long
     * word alignment and ensures hashing consistency).
     * ------------------
     */
    memset(&item, 0, XID_TAGSIZE);
    
    TransactionIdStore(GetCurrentTransactionId(), &item.tag.xid);
    item.tag.lock = MAKE_OFFSET(lock);
#if 0
    item.tag.pid = MyPid;
#endif
    
    if (! ( result = (XIDLookupEnt *) hash_search(xidTable,
						  (Pointer)&item,
						  HASH_FIND_SAVE,
						  &found) )
	|| !found)
	{
	    SpinRelease(masterLock);
	    elog(NOTICE,"LockReplace: xid table corrupted");
	    return(FALSE);
	}
    /*
     * now check to see if I have any private locks.  If I do,
     * decrement the counts associated with them.
     */
    result->holders[lockt]--;
    result->nHolding--;
    
    XID_PRINT("LockRelease updated xid stats", result);
    
    /*
     * If this was my last hold on this lock, delete my entry
     * in the XID table.
     */
    if (! result->nHolding)
	{
	    if (result->queue.next != INVALID_OFFSET)
		SHMQueueDelete(&result->queue);
	    if (! (result = (XIDLookupEnt *)
		   hash_search(xidTable, (Pointer)&item, HASH_REMOVE_SAVED, &found)) ||
		! found)
		{
		    SpinRelease(masterLock);
		    elog(NOTICE,"LockReplace: xid table corrupted");
		    return(FALSE);
		}
	}
    
    /* --------------------------
     * If there are still active locks of the type I just released, no one
     * should be woken up.  Whoever is asleep will still conflict
     * with the remaining locks.
     * --------------------------
     */
    if (! (lock->activeHolders[lockt]))
	{
	    /* change the conflict mask.  No more of this lock type. */
	    lock->mask &= BITS_OFF[lockt];
	}
    
    if (wakeupNeeded)
	{
	    /* --------------------------
	     * Wake the first waiting process and grant him the lock if it
	     * doesn't conflict.  The woken process must record the lock
	     * himself.
	     * --------------------------
	     */
	    (void) ProcLockWakeup(&(lock->waitProcs), (char *) ltable, (char *) lock);
	}
    
    SpinRelease(masterLock);
    return(TRUE);
}

/*
 * GrantLock -- udpate the lock data structure to show
 *	the new lock holder.
 */
void
GrantLock(LOCK *lock, LOCKT lockt)
{
    lock->nActive++;
    lock->activeHolders[lockt]++;
    lock->mask |= BITS_ON[lockt];
}

bool
LockReleaseAll(LockTableId tableId, SHM_QUEUE *lockQueue)
{
    PROC_QUEUE 	*waitQueue;
    int		done;
    XIDLookupEnt	*xidLook = NULL;
    XIDLookupEnt	*tmp = NULL;
    SHMEM_OFFSET 	end = MAKE_OFFSET(lockQueue);
    SPINLOCK 	masterLock;
    LOCKTAB 	*ltable;
    int		i,nLockTypes;
    LOCK		*lock;
    bool	found;
    
    Assert (tableId < NumTables);
    ltable = AllTables[tableId];
    if (!ltable)
	return (FALSE);
    
    nLockTypes = ltable->ctl->nLockTypes;
    masterLock = ltable->ctl->masterLock;
    
    if (SHMQueueEmpty(lockQueue))
	return TRUE;
    
    SHMQueueFirst(lockQueue,(Pointer*)&xidLook,&xidLook->queue);
    
    XID_PRINT("LockReleaseAll:", xidLook);
    
    SpinAcquire(masterLock);
    for (;;)
	{
	    /* ---------------------------
	     * XXX Here we assume the shared memory queue is circular and
	     * that we know its internal structure.  Should have some sort of
	     * macros to allow one to walk it.  mer 20 July 1991
	     * ---------------------------
	     */
	    done = (xidLook->queue.next == end);
	    lock = (LOCK *) MAKE_PTR(xidLook->tag.lock);
	    
	    LOCK_PRINT("ReleaseAll",(&lock->tag),0);
	    
	    /* ------------------
	     * fix the general lock stats
	     * ------------------
	     */
	    if (lock->nHolding != xidLook->nHolding)
		{
		    lock->nHolding -= xidLook->nHolding;
		    lock->nActive -= xidLook->nHolding;
		    Assert(lock->nActive >= 0);
		    for (i=1; i<=nLockTypes; i++)
			{
			    lock->holders[i] -= xidLook->holders[i];
			    lock->activeHolders[i] -= xidLook->holders[i];
			    if (! lock->activeHolders[i])
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
	    if ((! hash_search(ltable->xidHash, (Pointer)xidLook, HASH_REMOVE, &found))
		|| !found)
		{
		    SpinRelease(masterLock);
		    elog(NOTICE,"LockReplace: xid table corrupted");
		    return(FALSE);
		}
	    
	    if (! lock->nHolding)
		{
		    /* --------------------
		     * if there's no one waiting in the queue, we've just released
		     * the last lock.
		     * --------------------
		     */
		    
		    Assert( ltable->lockHash->hash == tag_hash);
		    lock = (LOCK *)
			hash_search(ltable->lockHash,(Pointer)&(lock->tag),HASH_REMOVE, &found);
		    if ((! lock) || (!found))
			{
			    SpinRelease(masterLock);
			    elog(NOTICE,"LockReplace: cannot remove lock from HTAB");
			    return(FALSE);
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
		    (void) ProcLockWakeup(waitQueue, (char *) ltable, (char *) lock);
		}
	    
	    if (done)
		break;
	    SHMQueueFirst(&xidLook->queue,(Pointer*)&tmp,&tmp->queue);
	    xidLook = tmp;
	}
    SpinRelease(masterLock);
    SHMQueueInit(lockQueue);
    return TRUE;
}

int
LockShmemSize()
{
    int size = 0;
    int nLockBuckets, nLockSegs;
    int nXidBuckets, nXidSegs;
    
    nLockBuckets = 1 << (int)my_log2((NLOCKENTS - 1) / DEF_FFACTOR + 1);
    nLockSegs = 1 << (int)my_log2((nLockBuckets - 1) / DEF_SEGSIZE + 1);
    
    nXidBuckets = 1 << (int)my_log2((NLOCKS_PER_XACT-1) / DEF_FFACTOR + 1);
    nXidSegs = 1 << (int)my_log2((nLockBuckets - 1) / DEF_SEGSIZE + 1);
    
    size += MAXALIGN(NBACKENDS * sizeof(PROC));	/* each MyProc */
    size += MAXALIGN(NBACKENDS * sizeof(LOCKCTL));	/* each ltable->ctl */
    size += MAXALIGN(sizeof(PROC_HDR));		/* ProcGlobal */
    
    size += MAXALIGN(my_log2(NLOCKENTS) * sizeof(void *));
    size += MAXALIGN(sizeof(HHDR));
    size += nLockSegs * MAXALIGN(DEF_SEGSIZE * sizeof(SEGMENT));
    size += NLOCKENTS * /* XXX not multiple of BUCKET_ALLOC_INCR? */
	(MAXALIGN(sizeof(BUCKET_INDEX)) +
	 MAXALIGN(sizeof(LOCK))); /* contains hash key */
    
    size += MAXALIGN(my_log2(NBACKENDS) * sizeof(void *));
    size += MAXALIGN(sizeof(HHDR));
    size += nXidSegs * MAXALIGN(DEF_SEGSIZE * sizeof(SEGMENT));
    size += NBACKENDS * /* XXX not multiple of BUCKET_ALLOC_INCR? */
	(MAXALIGN(sizeof(BUCKET_INDEX)) +
	 MAXALIGN(sizeof(XIDLookupEnt))); /* contains hash key */
    
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
