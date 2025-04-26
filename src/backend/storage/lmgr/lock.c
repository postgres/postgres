/*-------------------------------------------------------------------------
 *
 * lock.c
 *	  POSTGRES primary lock mechanism
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/lmgr/lock.c
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
 *	LockManagerShmemInit(), GetLocksMethodTable(), GetLockTagsMethodTable(),
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
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/spin.h"
#include "storage/standby.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/resowner.h"


/* GUC variables */
int			max_locks_per_xact; /* used to set the lock table size */
bool		log_lock_failure = false;

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
	LOCKBIT_ON(AccessExclusiveLock),

	/* RowShareLock */
	LOCKBIT_ON(ExclusiveLock) | LOCKBIT_ON(AccessExclusiveLock),

	/* RowExclusiveLock */
	LOCKBIT_ON(ShareLock) | LOCKBIT_ON(ShareRowExclusiveLock) |
	LOCKBIT_ON(ExclusiveLock) | LOCKBIT_ON(AccessExclusiveLock),

	/* ShareUpdateExclusiveLock */
	LOCKBIT_ON(ShareUpdateExclusiveLock) |
	LOCKBIT_ON(ShareLock) | LOCKBIT_ON(ShareRowExclusiveLock) |
	LOCKBIT_ON(ExclusiveLock) | LOCKBIT_ON(AccessExclusiveLock),

	/* ShareLock */
	LOCKBIT_ON(RowExclusiveLock) | LOCKBIT_ON(ShareUpdateExclusiveLock) |
	LOCKBIT_ON(ShareRowExclusiveLock) |
	LOCKBIT_ON(ExclusiveLock) | LOCKBIT_ON(AccessExclusiveLock),

	/* ShareRowExclusiveLock */
	LOCKBIT_ON(RowExclusiveLock) | LOCKBIT_ON(ShareUpdateExclusiveLock) |
	LOCKBIT_ON(ShareLock) | LOCKBIT_ON(ShareRowExclusiveLock) |
	LOCKBIT_ON(ExclusiveLock) | LOCKBIT_ON(AccessExclusiveLock),

	/* ExclusiveLock */
	LOCKBIT_ON(RowShareLock) |
	LOCKBIT_ON(RowExclusiveLock) | LOCKBIT_ON(ShareUpdateExclusiveLock) |
	LOCKBIT_ON(ShareLock) | LOCKBIT_ON(ShareRowExclusiveLock) |
	LOCKBIT_ON(ExclusiveLock) | LOCKBIT_ON(AccessExclusiveLock),

	/* AccessExclusiveLock */
	LOCKBIT_ON(AccessShareLock) | LOCKBIT_ON(RowShareLock) |
	LOCKBIT_ON(RowExclusiveLock) | LOCKBIT_ON(ShareUpdateExclusiveLock) |
	LOCKBIT_ON(ShareLock) | LOCKBIT_ON(ShareRowExclusiveLock) |
	LOCKBIT_ON(ExclusiveLock) | LOCKBIT_ON(AccessExclusiveLock)

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
	MaxLockMode,
	LockConflicts,
	lock_mode_names,
#ifdef LOCK_DEBUG
	&Trace_locks
#else
	&Dummy_trace
#endif
};

static const LockMethodData user_lockmethod = {
	MaxLockMode,
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
 * Count of the number of fast path lock slots we believe to be used.  This
 * might be higher than the real number if another backend has transferred
 * our locks to the primary lock table, but it can never be lower than the
 * real value, since only we can acquire locks on our own behalf.
 *
 * XXX Allocate a static array of the maximum size. We could use a pointer
 * and then allocate just the right size to save a couple kB, but then we
 * would have to initialize that, while for the static array that happens
 * automatically. Doesn't seem worth the extra complexity.
 */
static int	FastPathLocalUseCounts[FP_LOCK_GROUPS_PER_BACKEND_MAX];

/*
 * Flag to indicate if the relation extension lock is held by this backend.
 * This flag is used to ensure that while holding the relation extension lock
 * we don't try to acquire a heavyweight lock on any other object.  This
 * restriction implies that the relation extension lock won't ever participate
 * in the deadlock cycle because we can never wait for any other heavyweight
 * lock after acquiring this lock.
 *
 * Such a restriction is okay for relation extension locks as unlike other
 * heavyweight locks these are not held till the transaction end.  These are
 * taken for a short duration to extend a particular relation and then
 * released.
 */
static bool IsRelationExtensionLockHeld PG_USED_FOR_ASSERTS_ONLY = false;

/*
 * Number of fast-path locks per backend - size of the arrays in PGPROC.
 * This is set only once during start, before initializing shared memory,
 * and remains constant after that.
 *
 * We set the limit based on max_locks_per_transaction GUC, because that's
 * the best information about expected number of locks per backend we have.
 * See InitializeFastPathLocks() for details.
 */
int			FastPathLockGroupsPerBackend = 0;

/*
 * Macros to calculate the fast-path group and index for a relation.
 *
 * The formula is a simple hash function, designed to spread the OIDs a bit,
 * so that even contiguous values end up in different groups. In most cases
 * there will be gaps anyway, but the multiplication should help a bit.
 *
 * The selected constant (49157) is a prime not too close to 2^k, and it's
 * small enough to not cause overflows (in 64-bit).
 *
 * We can assume that FastPathLockGroupsPerBackend is a power-of-two per
 * InitializeFastPathLocks().
 */
#define FAST_PATH_REL_GROUP(rel) \
	(((uint64) (rel) * 49157) & (FastPathLockGroupsPerBackend - 1))

/*
 * Given the group/slot indexes, calculate the slot index in the whole array
 * of fast-path lock slots.
 */
#define FAST_PATH_SLOT(group, index) \
	(AssertMacro((uint32) (group) < FastPathLockGroupsPerBackend), \
	 AssertMacro((uint32) (index) < FP_LOCK_SLOTS_PER_GROUP), \
	 ((group) * FP_LOCK_SLOTS_PER_GROUP + (index)))

/*
 * Given a slot index (into the whole per-backend array), calculated using
 * the FAST_PATH_SLOT macro, split it into group and index (in the group).
 */
#define FAST_PATH_GROUP(index)	\
	(AssertMacro((uint32) (index) < FastPathLockSlotsPerBackend()), \
	 ((index) / FP_LOCK_SLOTS_PER_GROUP))
#define FAST_PATH_INDEX(index)	\
	(AssertMacro((uint32) (index) < FastPathLockSlotsPerBackend()), \
	 ((index) % FP_LOCK_SLOTS_PER_GROUP))

/* Macros for manipulating proc->fpLockBits */
#define FAST_PATH_BITS_PER_SLOT			3
#define FAST_PATH_LOCKNUMBER_OFFSET		1
#define FAST_PATH_MASK					((1 << FAST_PATH_BITS_PER_SLOT) - 1)
#define FAST_PATH_BITS(proc, n)			(proc)->fpLockBits[FAST_PATH_GROUP(n)]
#define FAST_PATH_GET_BITS(proc, n) \
	((FAST_PATH_BITS(proc, n) >> (FAST_PATH_BITS_PER_SLOT * FAST_PATH_INDEX(n))) & FAST_PATH_MASK)
#define FAST_PATH_BIT_POSITION(n, l) \
	(AssertMacro((l) >= FAST_PATH_LOCKNUMBER_OFFSET), \
	 AssertMacro((l) < FAST_PATH_BITS_PER_SLOT+FAST_PATH_LOCKNUMBER_OFFSET), \
	 AssertMacro((n) < FastPathLockSlotsPerBackend()), \
	 ((l) - FAST_PATH_LOCKNUMBER_OFFSET + FAST_PATH_BITS_PER_SLOT * (FAST_PATH_INDEX(n))))
#define FAST_PATH_SET_LOCKMODE(proc, n, l) \
	 FAST_PATH_BITS(proc, n) |= UINT64CONST(1) << FAST_PATH_BIT_POSITION(n, l)
#define FAST_PATH_CLEAR_LOCKMODE(proc, n, l) \
	 FAST_PATH_BITS(proc, n) &= ~(UINT64CONST(1) << FAST_PATH_BIT_POSITION(n, l))
#define FAST_PATH_CHECK_LOCKMODE(proc, n, l) \
	 (FAST_PATH_BITS(proc, n) & (UINT64CONST(1) << FAST_PATH_BIT_POSITION(n, l)))

/*
 * The fast-path lock mechanism is concerned only with relation locks on
 * unshared relations by backends bound to a database.  The fast-path
 * mechanism exists mostly to accelerate acquisition and release of locks
 * that rarely conflict.  Because ShareUpdateExclusiveLock is
 * self-conflicting, it can't use the fast-path mechanism; but it also does
 * not conflict with any of the locks that do, so we can ignore it completely.
 */
#define EligibleForRelationFastPath(locktag, mode) \
	((locktag)->locktag_lockmethodid == DEFAULT_LOCKMETHOD && \
	(locktag)->locktag_type == LOCKTAG_RELATION && \
	(locktag)->locktag_field1 == MyDatabaseId && \
	MyDatabaseId != InvalidOid && \
	(mode) < ShareUpdateExclusiveLock)
#define ConflictsWithRelationFastPath(locktag, mode) \
	((locktag)->locktag_lockmethodid == DEFAULT_LOCKMETHOD && \
	(locktag)->locktag_type == LOCKTAG_RELATION && \
	(locktag)->locktag_field1 != InvalidOid && \
	(mode) > ShareUpdateExclusiveLock)

static bool FastPathGrantRelationLock(Oid relid, LOCKMODE lockmode);
static bool FastPathUnGrantRelationLock(Oid relid, LOCKMODE lockmode);
static bool FastPathTransferRelationLocks(LockMethod lockMethodTable,
										  const LOCKTAG *locktag, uint32 hashcode);
static PROCLOCK *FastPathGetRelationLockEntry(LOCALLOCK *locallock);

/*
 * To make the fast-path lock mechanism work, we must have some way of
 * preventing the use of the fast-path when a conflicting lock might be present.
 * We partition* the locktag space into FAST_PATH_STRONG_LOCK_HASH_PARTITIONS,
 * and maintain an integer count of the number of "strong" lockers
 * in each partition.  When any "strong" lockers are present (which is
 * hopefully not very often), the fast-path mechanism can't be used, and we
 * must fall back to the slower method of pushing matching locks directly
 * into the main lock tables.
 *
 * The deadlock detector does not know anything about the fast path mechanism,
 * so any locks that might be involved in a deadlock must be transferred from
 * the fast-path queues to the main lock table.
 */

#define FAST_PATH_STRONG_LOCK_HASH_BITS			10
#define FAST_PATH_STRONG_LOCK_HASH_PARTITIONS \
	(1 << FAST_PATH_STRONG_LOCK_HASH_BITS)
#define FastPathStrongLockHashPartition(hashcode) \
	((hashcode) % FAST_PATH_STRONG_LOCK_HASH_PARTITIONS)

typedef struct
{
	slock_t		mutex;
	uint32		count[FAST_PATH_STRONG_LOCK_HASH_PARTITIONS];
} FastPathStrongRelationLockData;

static volatile FastPathStrongRelationLockData *FastPathStrongRelationLocks;


/*
 * Pointers to hash tables containing lock state
 *
 * The LockMethodLockHash and LockMethodProcLockHash hash tables are in
 * shared memory; LockMethodLocalHash is local to each backend.
 */
static HTAB *LockMethodLockHash;
static HTAB *LockMethodProcLockHash;
static HTAB *LockMethodLocalHash;


/* private state for error cleanup */
static LOCALLOCK *StrongLockInProgress;
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
			 dclist_count(&lock->waitProcs),
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

#define LOCK_PRINT(where, lock, type)  ((void) 0)
#define PROCLOCK_PRINT(where, proclockP)  ((void) 0)
#endif							/* not LOCK_DEBUG */


static uint32 proclock_hash(const void *key, Size keysize);
static void RemoveLocalLock(LOCALLOCK *locallock);
static PROCLOCK *SetupLockInTable(LockMethod lockMethodTable, PGPROC *proc,
								  const LOCKTAG *locktag, uint32 hashcode, LOCKMODE lockmode);
static void GrantLockLocal(LOCALLOCK *locallock, ResourceOwner owner);
static void BeginStrongLockAcquire(LOCALLOCK *locallock, uint32 fasthashcode);
static void FinishStrongLockAcquire(void);
static ProcWaitStatus WaitOnLock(LOCALLOCK *locallock, ResourceOwner owner);
static void ReleaseLockIfHeld(LOCALLOCK *locallock, bool sessionLock);
static void LockReassignOwner(LOCALLOCK *locallock, ResourceOwner parent);
static bool UnGrantLock(LOCK *lock, LOCKMODE lockmode,
						PROCLOCK *proclock, LockMethod lockMethodTable);
static void CleanUpLock(LOCK *lock, PROCLOCK *proclock,
						LockMethod lockMethodTable, uint32 hashcode,
						bool wakeupNeeded);
static void LockRefindAndRelease(LockMethod lockMethodTable, PGPROC *proc,
								 LOCKTAG *locktag, LOCKMODE lockmode,
								 bool decrement_strong_lock_count);
static void GetSingleProcBlockerStatusData(PGPROC *blocked_proc,
										   BlockedProcsData *data);


/*
 * Initialize the lock manager's shmem data structures.
 *
 * This is called from CreateSharedMemoryAndSemaphores(), which see for more
 * comments.  In the normal postmaster case, the shared hash tables are
 * created here, and backends inherit pointers to them via fork().  In the
 * EXEC_BACKEND case, each backend re-executes this code to obtain pointers to
 * the already existing shared hash tables.  In either case, each backend must
 * also call InitLockManagerAccess() to create the locallock hash table.
 */
void
LockManagerShmemInit(void)
{
	HASHCTL		info;
	long		init_table_size,
				max_table_size;
	bool		found;

	/*
	 * Compute init/max size to request for lock hashtables.  Note these
	 * calculations must agree with LockManagerShmemSize!
	 */
	max_table_size = NLOCKENTS();
	init_table_size = max_table_size / 2;

	/*
	 * Allocate hash table for LOCK structs.  This stores per-locked-object
	 * information.
	 */
	info.keysize = sizeof(LOCKTAG);
	info.entrysize = sizeof(LOCK);
	info.num_partitions = NUM_LOCK_PARTITIONS;

	LockMethodLockHash = ShmemInitHash("LOCK hash",
									   init_table_size,
									   max_table_size,
									   &info,
									   HASH_ELEM | HASH_BLOBS | HASH_PARTITION);

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

	LockMethodProcLockHash = ShmemInitHash("PROCLOCK hash",
										   init_table_size,
										   max_table_size,
										   &info,
										   HASH_ELEM | HASH_FUNCTION | HASH_PARTITION);

	/*
	 * Allocate fast-path structures.
	 */
	FastPathStrongRelationLocks =
		ShmemInitStruct("Fast Path Strong Relation Lock Data",
						sizeof(FastPathStrongRelationLockData), &found);
	if (!found)
		SpinLockInit(&FastPathStrongRelationLocks->mutex);
}

/*
 * Initialize the lock manager's backend-private data structures.
 */
void
InitLockManagerAccess(void)
{
	/*
	 * Allocate non-shared hash table for LOCALLOCK structs.  This stores lock
	 * counts and resource owner information.
	 */
	HASHCTL		info;

	info.keysize = sizeof(LOCALLOCKTAG);
	info.entrysize = sizeof(LOCALLOCK);

	LockMethodLocalHash = hash_create("LOCALLOCK hash",
									  16,
									  &info,
									  HASH_ELEM | HASH_BLOBS);
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
 * Fetch the lock method table associated with a given locktag
 */
LockMethod
GetLockTagsMethodTable(const LOCKTAG *locktag)
{
	LOCKMETHODID lockmethodid = (LOCKMETHODID) locktag->locktag_lockmethodid;

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
	return get_hash_value(LockMethodLockHash, locktag);
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
 * Given two lock modes, return whether they would conflict.
 */
bool
DoLockModesConflict(LOCKMODE mode1, LOCKMODE mode2)
{
	LockMethod	lockMethodTable = LockMethods[DEFAULT_LOCKMETHOD];

	if (lockMethodTable->conflictTab[mode1] & LOCKBIT_ON(mode2))
		return true;

	return false;
}

/*
 * LockHeldByMe -- test whether lock 'locktag' is held by the current
 *		transaction
 *
 * Returns true if current transaction holds a lock on 'tag' of mode
 * 'lockmode'.  If 'orstronger' is true, a stronger lockmode is also OK.
 * ("Stronger" is defined as "numerically higher", which is a bit
 * semantically dubious but is OK for the purposes we use this for.)
 */
bool
LockHeldByMe(const LOCKTAG *locktag,
			 LOCKMODE lockmode, bool orstronger)
{
	LOCALLOCKTAG localtag;
	LOCALLOCK  *locallock;

	/*
	 * See if there is a LOCALLOCK entry for this lock and lockmode
	 */
	MemSet(&localtag, 0, sizeof(localtag)); /* must clear padding */
	localtag.lock = *locktag;
	localtag.mode = lockmode;

	locallock = (LOCALLOCK *) hash_search(LockMethodLocalHash,
										  &localtag,
										  HASH_FIND, NULL);

	if (locallock && locallock->nLocks > 0)
		return true;

	if (orstronger)
	{
		LOCKMODE	slockmode;

		for (slockmode = lockmode + 1;
			 slockmode <= MaxLockMode;
			 slockmode++)
		{
			if (LockHeldByMe(locktag, slockmode, false))
				return true;
		}
	}

	return false;
}

#ifdef USE_ASSERT_CHECKING
/*
 * GetLockMethodLocalHash -- return the hash of local locks, for modules that
 *		evaluate assertions based on all locks held.
 */
HTAB *
GetLockMethodLocalHash(void)
{
	return LockMethodLocalHash;
}
#endif

/*
 * LockHasWaiters -- look up 'locktag' and check if releasing this
 *		lock would wake up other processes waiting for it.
 */
bool
LockHasWaiters(const LOCKTAG *locktag, LOCKMODE lockmode, bool sessionLock)
{
	LOCKMETHODID lockmethodid = locktag->locktag_lockmethodid;
	LockMethod	lockMethodTable;
	LOCALLOCKTAG localtag;
	LOCALLOCK  *locallock;
	LOCK	   *lock;
	PROCLOCK   *proclock;
	LWLock	   *partitionLock;
	bool		hasWaiters = false;

	if (lockmethodid <= 0 || lockmethodid >= lengthof(LockMethods))
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);
	lockMethodTable = LockMethods[lockmethodid];
	if (lockmode <= 0 || lockmode > lockMethodTable->numLockModes)
		elog(ERROR, "unrecognized lock mode: %d", lockmode);

#ifdef LOCK_DEBUG
	if (LOCK_DEBUG_ENABLED(locktag))
		elog(LOG, "LockHasWaiters: lock [%u,%u] %s",
			 locktag->locktag_field1, locktag->locktag_field2,
			 lockMethodTable->lockModeNames[lockmode]);
#endif

	/*
	 * Find the LOCALLOCK entry for this lock and lockmode
	 */
	MemSet(&localtag, 0, sizeof(localtag)); /* must clear padding */
	localtag.lock = *locktag;
	localtag.mode = lockmode;

	locallock = (LOCALLOCK *) hash_search(LockMethodLocalHash,
										  &localtag,
										  HASH_FIND, NULL);

	/*
	 * let the caller print its own error message, too. Do not ereport(ERROR).
	 */
	if (!locallock || locallock->nLocks <= 0)
	{
		elog(WARNING, "you don't own a lock of type %s",
			 lockMethodTable->lockModeNames[lockmode]);
		return false;
	}

	/*
	 * Check the shared lock table.
	 */
	partitionLock = LockHashPartitionLock(locallock->hashcode);

	LWLockAcquire(partitionLock, LW_SHARED);

	/*
	 * We don't need to re-find the lock or proclock, since we kept their
	 * addresses in the locallock table, and they couldn't have been removed
	 * while we were holding a lock on them.
	 */
	lock = locallock->lock;
	LOCK_PRINT("LockHasWaiters: found", lock, lockmode);
	proclock = locallock->proclock;
	PROCLOCK_PRINT("LockHasWaiters: found", proclock);

	/*
	 * Double-check that we are actually holding a lock of the type we want to
	 * release.
	 */
	if (!(proclock->holdMask & LOCKBIT_ON(lockmode)))
	{
		PROCLOCK_PRINT("LockHasWaiters: WRONGTYPE", proclock);
		LWLockRelease(partitionLock);
		elog(WARNING, "you don't own a lock of type %s",
			 lockMethodTable->lockModeNames[lockmode]);
		RemoveLocalLock(locallock);
		return false;
	}

	/*
	 * Do the checking.
	 */
	if ((lockMethodTable->conflictTab[lockmode] & lock->waitMask) != 0)
		hasWaiters = true;

	LWLockRelease(partitionLock);

	return hasWaiters;
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
 *		LOCKACQUIRE_ALREADY_CLEAR	incremented count for lock already clear
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
	return LockAcquireExtended(locktag, lockmode, sessionLock, dontWait,
							   true, NULL, false);
}

/*
 * LockAcquireExtended - allows us to specify additional options
 *
 * reportMemoryError specifies whether a lock request that fills the lock
 * table should generate an ERROR or not.  Passing "false" allows the caller
 * to attempt to recover from lock-table-full situations, perhaps by forcibly
 * canceling other lock holders and then retrying.  Note, however, that the
 * return code for that is LOCKACQUIRE_NOT_AVAIL, so that it's unsafe to use
 * in combination with dontWait = true, as the cause of failure couldn't be
 * distinguished.
 *
 * If locallockp isn't NULL, *locallockp receives a pointer to the LOCALLOCK
 * table entry if a lock is successfully acquired, or NULL if not.
 *
 * logLockFailure indicates whether to log details when a lock acquisition
 * fails with dontWait = true.
 */
LockAcquireResult
LockAcquireExtended(const LOCKTAG *locktag,
					LOCKMODE lockmode,
					bool sessionLock,
					bool dontWait,
					bool reportMemoryError,
					LOCALLOCK **locallockp,
					bool logLockFailure)
{
	LOCKMETHODID lockmethodid = locktag->locktag_lockmethodid;
	LockMethod	lockMethodTable;
	LOCALLOCKTAG localtag;
	LOCALLOCK  *locallock;
	LOCK	   *lock;
	PROCLOCK   *proclock;
	bool		found;
	ResourceOwner owner;
	uint32		hashcode;
	LWLock	   *partitionLock;
	bool		found_conflict;
	ProcWaitStatus waitResult;
	bool		log_lock = false;

	if (lockmethodid <= 0 || lockmethodid >= lengthof(LockMethods))
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);
	lockMethodTable = LockMethods[lockmethodid];
	if (lockmode <= 0 || lockmode > lockMethodTable->numLockModes)
		elog(ERROR, "unrecognized lock mode: %d", lockmode);

	if (RecoveryInProgress() && !InRecovery &&
		(locktag->locktag_type == LOCKTAG_OBJECT ||
		 locktag->locktag_type == LOCKTAG_RELATION) &&
		lockmode > RowExclusiveLock)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot acquire lock mode %s on database objects while recovery is in progress",
						lockMethodTable->lockModeNames[lockmode]),
				 errhint("Only RowExclusiveLock or less can be acquired on database objects during recovery.")));

#ifdef LOCK_DEBUG
	if (LOCK_DEBUG_ENABLED(locktag))
		elog(LOG, "LockAcquire: lock [%u,%u] %s",
			 locktag->locktag_field1, locktag->locktag_field2,
			 lockMethodTable->lockModeNames[lockmode]);
#endif

	/* Identify owner for lock */
	if (sessionLock)
		owner = NULL;
	else
		owner = CurrentResourceOwner;

	/*
	 * Find or create a LOCALLOCK entry for this lock and lockmode
	 */
	MemSet(&localtag, 0, sizeof(localtag)); /* must clear padding */
	localtag.lock = *locktag;
	localtag.mode = lockmode;

	locallock = (LOCALLOCK *) hash_search(LockMethodLocalHash,
										  &localtag,
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
		locallock->holdsStrongLockCount = false;
		locallock->lockCleared = false;
		locallock->numLockOwners = 0;
		locallock->maxLockOwners = 8;
		locallock->lockOwners = NULL;	/* in case next line fails */
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
	hashcode = locallock->hashcode;

	if (locallockp)
		*locallockp = locallock;

	/*
	 * If we already hold the lock, we can just increase the count locally.
	 *
	 * If lockCleared is already set, caller need not worry about absorbing
	 * sinval messages related to the lock's object.
	 */
	if (locallock->nLocks > 0)
	{
		GrantLockLocal(locallock, owner);
		if (locallock->lockCleared)
			return LOCKACQUIRE_ALREADY_CLEAR;
		else
			return LOCKACQUIRE_ALREADY_HELD;
	}

	/*
	 * We don't acquire any other heavyweight lock while holding the relation
	 * extension lock.  We do allow to acquire the same relation extension
	 * lock more than once but that case won't reach here.
	 */
	Assert(!IsRelationExtensionLockHeld);

	/*
	 * Prepare to emit a WAL record if acquisition of this lock needs to be
	 * replayed in a standby server.
	 *
	 * Here we prepare to log; after lock is acquired we'll issue log record.
	 * This arrangement simplifies error recovery in case the preparation step
	 * fails.
	 *
	 * Only AccessExclusiveLocks can conflict with lock types that read-only
	 * transactions can acquire in a standby server. Make sure this definition
	 * matches the one in GetRunningTransactionLocks().
	 */
	if (lockmode >= AccessExclusiveLock &&
		locktag->locktag_type == LOCKTAG_RELATION &&
		!RecoveryInProgress() &&
		XLogStandbyInfoActive())
	{
		LogAccessExclusiveLockPrepare();
		log_lock = true;
	}

	/*
	 * Attempt to take lock via fast path, if eligible.  But if we remember
	 * having filled up the fast path array, we don't attempt to make any
	 * further use of it until we release some locks.  It's possible that some
	 * other backend has transferred some of those locks to the shared hash
	 * table, leaving space free, but it's not worth acquiring the LWLock just
	 * to check.  It's also possible that we're acquiring a second or third
	 * lock type on a relation we have already locked using the fast-path, but
	 * for now we don't worry about that case either.
	 */
	if (EligibleForRelationFastPath(locktag, lockmode) &&
		FastPathLocalUseCounts[FAST_PATH_REL_GROUP(locktag->locktag_field2)] < FP_LOCK_SLOTS_PER_GROUP)
	{
		uint32		fasthashcode = FastPathStrongLockHashPartition(hashcode);
		bool		acquired;

		/*
		 * LWLockAcquire acts as a memory sequencing point, so it's safe to
		 * assume that any strong locker whose increment to
		 * FastPathStrongRelationLocks->counts becomes visible after we test
		 * it has yet to begin to transfer fast-path locks.
		 */
		LWLockAcquire(&MyProc->fpInfoLock, LW_EXCLUSIVE);
		if (FastPathStrongRelationLocks->count[fasthashcode] != 0)
			acquired = false;
		else
			acquired = FastPathGrantRelationLock(locktag->locktag_field2,
												 lockmode);
		LWLockRelease(&MyProc->fpInfoLock);
		if (acquired)
		{
			/*
			 * The locallock might contain stale pointers to some old shared
			 * objects; we MUST reset these to null before considering the
			 * lock to be acquired via fast-path.
			 */
			locallock->lock = NULL;
			locallock->proclock = NULL;
			GrantLockLocal(locallock, owner);
			return LOCKACQUIRE_OK;
		}
	}

	/*
	 * If this lock could potentially have been taken via the fast-path by
	 * some other backend, we must (temporarily) disable further use of the
	 * fast-path for this lock tag, and migrate any locks already taken via
	 * this method to the main lock table.
	 */
	if (ConflictsWithRelationFastPath(locktag, lockmode))
	{
		uint32		fasthashcode = FastPathStrongLockHashPartition(hashcode);

		BeginStrongLockAcquire(locallock, fasthashcode);
		if (!FastPathTransferRelationLocks(lockMethodTable, locktag,
										   hashcode))
		{
			AbortStrongLockAcquire();
			if (locallock->nLocks == 0)
				RemoveLocalLock(locallock);
			if (locallockp)
				*locallockp = NULL;
			if (reportMemoryError)
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of shared memory"),
						 errhint("You might need to increase \"%s\".", "max_locks_per_transaction")));
			else
				return LOCKACQUIRE_NOT_AVAIL;
		}
	}

	/*
	 * We didn't find the lock in our LOCALLOCK table, and we didn't manage to
	 * take it via the fast-path, either, so we've got to mess with the shared
	 * lock table.
	 */
	partitionLock = LockHashPartitionLock(hashcode);

	LWLockAcquire(partitionLock, LW_EXCLUSIVE);

	/*
	 * Find or create lock and proclock entries with this tag
	 *
	 * Note: if the locallock object already existed, it might have a pointer
	 * to the lock already ... but we should not assume that that pointer is
	 * valid, since a lock object with zero hold and request counts can go
	 * away anytime.  So we have to use SetupLockInTable() to recompute the
	 * lock and proclock pointers, even if they're already set.
	 */
	proclock = SetupLockInTable(lockMethodTable, MyProc, locktag,
								hashcode, lockmode);
	if (!proclock)
	{
		AbortStrongLockAcquire();
		LWLockRelease(partitionLock);
		if (locallock->nLocks == 0)
			RemoveLocalLock(locallock);
		if (locallockp)
			*locallockp = NULL;
		if (reportMemoryError)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of shared memory"),
					 errhint("You might need to increase \"%s\".", "max_locks_per_transaction")));
		else
			return LOCKACQUIRE_NOT_AVAIL;
	}
	locallock->proclock = proclock;
	lock = proclock->tag.myLock;
	locallock->lock = lock;

	/*
	 * If lock requested conflicts with locks requested by waiters, must join
	 * wait queue.  Otherwise, check for conflict with already-held locks.
	 * (That's last because most complex check.)
	 */
	if (lockMethodTable->conflictTab[lockmode] & lock->waitMask)
		found_conflict = true;
	else
		found_conflict = LockCheckConflicts(lockMethodTable, lockmode,
											lock, proclock);

	if (!found_conflict)
	{
		/* No conflict with held or previously requested locks */
		GrantLock(lock, proclock, lockmode);
		waitResult = PROC_WAIT_STATUS_OK;
	}
	else
	{
		/*
		 * Join the lock's wait queue.  We call this even in the dontWait
		 * case, because JoinWaitQueue() may discover that we can acquire the
		 * lock immediately after all.
		 */
		waitResult = JoinWaitQueue(locallock, lockMethodTable, dontWait);
	}

	if (waitResult == PROC_WAIT_STATUS_ERROR)
	{
		/*
		 * We're not getting the lock because a deadlock was detected already
		 * while trying to join the wait queue, or because we would have to
		 * wait but the caller requested no blocking.
		 *
		 * Undo the changes to shared entries before releasing the partition
		 * lock.
		 */
		AbortStrongLockAcquire();

		if (proclock->holdMask == 0)
		{
			uint32		proclock_hashcode;

			proclock_hashcode = ProcLockHashCode(&proclock->tag,
												 hashcode);
			dlist_delete(&proclock->lockLink);
			dlist_delete(&proclock->procLink);
			if (!hash_search_with_hash_value(LockMethodProcLockHash,
											 &(proclock->tag),
											 proclock_hashcode,
											 HASH_REMOVE,
											 NULL))
				elog(PANIC, "proclock table corrupted");
		}
		else
			PROCLOCK_PRINT("LockAcquire: did not join wait queue", proclock);
		lock->nRequested--;
		lock->requested[lockmode]--;
		LOCK_PRINT("LockAcquire: did not join wait queue",
				   lock, lockmode);
		Assert((lock->nRequested > 0) &&
			   (lock->requested[lockmode] >= 0));
		Assert(lock->nGranted <= lock->nRequested);
		LWLockRelease(partitionLock);
		if (locallock->nLocks == 0)
			RemoveLocalLock(locallock);

		if (dontWait)
		{
			/*
			 * Log lock holders and waiters as a detail log message if
			 * logLockFailure = true and lock acquisition fails with dontWait
			 * = true
			 */
			if (logLockFailure)
			{
				StringInfoData buf,
							lock_waiters_sbuf,
							lock_holders_sbuf;
				const char *modename;
				int			lockHoldersNum = 0;

				initStringInfo(&buf);
				initStringInfo(&lock_waiters_sbuf);
				initStringInfo(&lock_holders_sbuf);

				DescribeLockTag(&buf, &locallock->tag.lock);
				modename = GetLockmodeName(locallock->tag.lock.locktag_lockmethodid,
										   lockmode);

				/* Gather a list of all lock holders and waiters */
				LWLockAcquire(partitionLock, LW_SHARED);
				GetLockHoldersAndWaiters(locallock, &lock_holders_sbuf,
										 &lock_waiters_sbuf, &lockHoldersNum);
				LWLockRelease(partitionLock);

				ereport(LOG,
						(errmsg("process %d could not obtain %s on %s",
								MyProcPid, modename, buf.data),
						 errdetail_log_plural(
											  "Process holding the lock: %s, Wait queue: %s.",
											  "Processes holding the lock: %s, Wait queue: %s.",
											  lockHoldersNum,
											  lock_holders_sbuf.data,
											  lock_waiters_sbuf.data)));

				pfree(buf.data);
				pfree(lock_holders_sbuf.data);
				pfree(lock_waiters_sbuf.data);
			}
			if (locallockp)
				*locallockp = NULL;
			return LOCKACQUIRE_NOT_AVAIL;
		}
		else
		{
			DeadLockReport();
			/* DeadLockReport() will not return */
		}
	}

	/*
	 * We are now in the lock queue, or the lock was already granted.  If
	 * queued, go to sleep.
	 */
	if (waitResult == PROC_WAIT_STATUS_WAITING)
	{
		Assert(!dontWait);
		PROCLOCK_PRINT("LockAcquire: sleeping on lock", proclock);
		LOCK_PRINT("LockAcquire: sleeping on lock", lock, lockmode);
		LWLockRelease(partitionLock);

		waitResult = WaitOnLock(locallock, owner);

		/*
		 * NOTE: do not do any material change of state between here and
		 * return.  All required changes in locktable state must have been
		 * done when the lock was granted to us --- see notes in WaitOnLock.
		 */

		if (waitResult == PROC_WAIT_STATUS_ERROR)
		{
			/*
			 * We failed as a result of a deadlock, see CheckDeadLock(). Quit
			 * now.
			 */
			Assert(!dontWait);
			DeadLockReport();
			/* DeadLockReport() will not return */
		}
	}
	else
		LWLockRelease(partitionLock);
	Assert(waitResult == PROC_WAIT_STATUS_OK);

	/* The lock was granted to us.  Update the local lock entry accordingly */
	Assert((proclock->holdMask & LOCKBIT_ON(lockmode)) != 0);
	GrantLockLocal(locallock, owner);

	/*
	 * Lock state is fully up-to-date now; if we error out after this, no
	 * special error cleanup is required.
	 */
	FinishStrongLockAcquire();

	/*
	 * Emit a WAL record if acquisition of this lock needs to be replayed in a
	 * standby server.
	 */
	if (log_lock)
	{
		/*
		 * Decode the locktag back to the original values, to avoid sending
		 * lots of empty bytes with every message.  See lock.h to check how a
		 * locktag is defined for LOCKTAG_RELATION
		 */
		LogAccessExclusiveLock(locktag->locktag_field1,
							   locktag->locktag_field2);
	}

	return LOCKACQUIRE_OK;
}

/*
 * Find or create LOCK and PROCLOCK objects as needed for a new lock
 * request.
 *
 * Returns the PROCLOCK object, or NULL if we failed to create the objects
 * for lack of shared memory.
 *
 * The appropriate partition lock must be held at entry, and will be
 * held at exit.
 */
static PROCLOCK *
SetupLockInTable(LockMethod lockMethodTable, PGPROC *proc,
				 const LOCKTAG *locktag, uint32 hashcode, LOCKMODE lockmode)
{
	LOCK	   *lock;
	PROCLOCK   *proclock;
	PROCLOCKTAG proclocktag;
	uint32		proclock_hashcode;
	bool		found;

	/*
	 * Find or create a lock with this tag.
	 */
	lock = (LOCK *) hash_search_with_hash_value(LockMethodLockHash,
												locktag,
												hashcode,
												HASH_ENTER_NULL,
												&found);
	if (!lock)
		return NULL;

	/*
	 * if it's a new lock object, initialize it
	 */
	if (!found)
	{
		lock->grantMask = 0;
		lock->waitMask = 0;
		dlist_init(&lock->procLocks);
		dclist_init(&lock->waitProcs);
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
	proclocktag.myProc = proc;

	proclock_hashcode = ProcLockHashCode(&proclocktag, hashcode);

	/*
	 * Find or create a proclock entry with this tag
	 */
	proclock = (PROCLOCK *) hash_search_with_hash_value(LockMethodProcLockHash,
														&proclocktag,
														proclock_hashcode,
														HASH_ENTER_NULL,
														&found);
	if (!proclock)
	{
		/* Oops, not enough shmem for the proclock */
		if (lock->nRequested == 0)
		{
			/*
			 * There are no other requestors of this lock, so garbage-collect
			 * the lock object.  We *must* do this to avoid a permanent leak
			 * of shared memory, because there won't be anything to cause
			 * anyone to release the lock object later.
			 */
			Assert(dlist_is_empty(&(lock->procLocks)));
			if (!hash_search_with_hash_value(LockMethodLockHash,
											 &(lock->tag),
											 hashcode,
											 HASH_REMOVE,
											 NULL))
				elog(PANIC, "lock table corrupted");
		}
		return NULL;
	}

	/*
	 * If new, initialize the new entry
	 */
	if (!found)
	{
		uint32		partition = LockHashPartition(hashcode);

		/*
		 * It might seem unsafe to access proclock->groupLeader without a
		 * lock, but it's not really.  Either we are initializing a proclock
		 * on our own behalf, in which case our group leader isn't changing
		 * because the group leader for a process can only ever be changed by
		 * the process itself; or else we are transferring a fast-path lock to
		 * the main lock table, in which case that process can't change its
		 * lock group leader without first releasing all of its locks (and in
		 * particular the one we are currently transferring).
		 */
		proclock->groupLeader = proc->lockGroupLeader != NULL ?
			proc->lockGroupLeader : proc;
		proclock->holdMask = 0;
		proclock->releaseMask = 0;
		/* Add proclock to appropriate lists */
		dlist_push_tail(&lock->procLocks, &proclock->lockLink);
		dlist_push_tail(&proc->myProcLocks[partition], &proclock->procLink);
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
#endif							/* CHECK_DEADLOCK_RISK */
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

	return proclock;
}

/*
 * Check and set/reset the flag that we hold the relation extension lock.
 *
 * It is callers responsibility that this function is called after
 * acquiring/releasing the relation extension lock.
 *
 * Pass acquired as true if lock is acquired, false otherwise.
 */
static inline void
CheckAndSetLockHeld(LOCALLOCK *locallock, bool acquired)
{
#ifdef USE_ASSERT_CHECKING
	if (LOCALLOCK_LOCKTAG(*locallock) == LOCKTAG_RELATION_EXTEND)
		IsRelationExtensionLockHeld = acquired;
#endif
}

/*
 * Subroutine to free a locallock entry
 */
static void
RemoveLocalLock(LOCALLOCK *locallock)
{
	int			i;

	for (i = locallock->numLockOwners - 1; i >= 0; i--)
	{
		if (locallock->lockOwners[i].owner != NULL)
			ResourceOwnerForgetLock(locallock->lockOwners[i].owner, locallock);
	}
	locallock->numLockOwners = 0;
	if (locallock->lockOwners != NULL)
		pfree(locallock->lockOwners);
	locallock->lockOwners = NULL;

	if (locallock->holdsStrongLockCount)
	{
		uint32		fasthashcode;

		fasthashcode = FastPathStrongLockHashPartition(locallock->hashcode);

		SpinLockAcquire(&FastPathStrongRelationLocks->mutex);
		Assert(FastPathStrongRelationLocks->count[fasthashcode] > 0);
		FastPathStrongRelationLocks->count[fasthashcode]--;
		locallock->holdsStrongLockCount = false;
		SpinLockRelease(&FastPathStrongRelationLocks->mutex);
	}

	if (!hash_search(LockMethodLocalHash,
					 &(locallock->tag),
					 HASH_REMOVE, NULL))
		elog(WARNING, "locallock table corrupted");

	/*
	 * Indicate that the lock is released for certain types of locks
	 */
	CheckAndSetLockHeld(locallock, false);
}

/*
 * LockCheckConflicts -- test whether requested lock conflicts
 *		with those already granted
 *
 * Returns true if conflict, false if no conflict.
 *
 * NOTES:
 *		Here's what makes this complicated: one process's locks don't
 * conflict with one another, no matter what purpose they are held for
 * (eg, session and transaction locks do not conflict).  Nor do the locks
 * of one process in a lock group conflict with those of another process in
 * the same group.  So, we must subtract off these locks when determining
 * whether the requested new lock conflicts with those already held.
 */
bool
LockCheckConflicts(LockMethod lockMethodTable,
				   LOCKMODE lockmode,
				   LOCK *lock,
				   PROCLOCK *proclock)
{
	int			numLockModes = lockMethodTable->numLockModes;
	LOCKMASK	myLocks;
	int			conflictMask = lockMethodTable->conflictTab[lockmode];
	int			conflictsRemaining[MAX_LOCKMODES];
	int			totalConflictsRemaining = 0;
	dlist_iter	proclock_iter;
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
	if (!(conflictMask & lock->grantMask))
	{
		PROCLOCK_PRINT("LockCheckConflicts: no conflict", proclock);
		return false;
	}

	/*
	 * Rats.  Something conflicts.  But it could still be my own lock, or a
	 * lock held by another member of my locking group.  First, figure out how
	 * many conflicts remain after subtracting out any locks I hold myself.
	 */
	myLocks = proclock->holdMask;
	for (i = 1; i <= numLockModes; i++)
	{
		if ((conflictMask & LOCKBIT_ON(i)) == 0)
		{
			conflictsRemaining[i] = 0;
			continue;
		}
		conflictsRemaining[i] = lock->granted[i];
		if (myLocks & LOCKBIT_ON(i))
			--conflictsRemaining[i];
		totalConflictsRemaining += conflictsRemaining[i];
	}

	/* If no conflicts remain, we get the lock. */
	if (totalConflictsRemaining == 0)
	{
		PROCLOCK_PRINT("LockCheckConflicts: resolved (simple)", proclock);
		return false;
	}

	/* If no group locking, it's definitely a conflict. */
	if (proclock->groupLeader == MyProc && MyProc->lockGroupLeader == NULL)
	{
		Assert(proclock->tag.myProc == MyProc);
		PROCLOCK_PRINT("LockCheckConflicts: conflicting (simple)",
					   proclock);
		return true;
	}

	/*
	 * The relation extension lock conflict even between the group members.
	 */
	if (LOCK_LOCKTAG(*lock) == LOCKTAG_RELATION_EXTEND)
	{
		PROCLOCK_PRINT("LockCheckConflicts: conflicting (group)",
					   proclock);
		return true;
	}

	/*
	 * Locks held in conflicting modes by members of our own lock group are
	 * not real conflicts; we can subtract those out and see if we still have
	 * a conflict.  This is O(N) in the number of processes holding or
	 * awaiting locks on this object.  We could improve that by making the
	 * shared memory state more complex (and larger) but it doesn't seem worth
	 * it.
	 */
	dlist_foreach(proclock_iter, &lock->procLocks)
	{
		PROCLOCK   *otherproclock =
			dlist_container(PROCLOCK, lockLink, proclock_iter.cur);

		if (proclock != otherproclock &&
			proclock->groupLeader == otherproclock->groupLeader &&
			(otherproclock->holdMask & conflictMask) != 0)
		{
			int			intersectMask = otherproclock->holdMask & conflictMask;

			for (i = 1; i <= numLockModes; i++)
			{
				if ((intersectMask & LOCKBIT_ON(i)) != 0)
				{
					if (conflictsRemaining[i] <= 0)
						elog(PANIC, "proclocks held do not match lock");
					conflictsRemaining[i]--;
					totalConflictsRemaining--;
				}
			}

			if (totalConflictsRemaining == 0)
			{
				PROCLOCK_PRINT("LockCheckConflicts: resolved (group)",
							   proclock);
				return false;
			}
		}
	}

	/* Nope, it's a real conflict. */
	PROCLOCK_PRINT("LockCheckConflicts: conflicting (group)", proclock);
	return true;
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
	 * least one of the lock types requested by waiter(s).  Otherwise whatever
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
 * CleanUpLock -- clean up after releasing a lock.  We garbage-collect the
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
		dlist_delete(&proclock->lockLink);
		dlist_delete(&proclock->procLink);
		proclock_hashcode = ProcLockHashCode(&proclock->tag, hashcode);
		if (!hash_search_with_hash_value(LockMethodProcLockHash,
										 &(proclock->tag),
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
		Assert(dlist_is_empty(&lock->procLocks));
		if (!hash_search_with_hash_value(LockMethodLockHash,
										 &(lock->tag),
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
	if (owner != NULL)
		ResourceOwnerRememberLock(owner, locallock);

	/* Indicate that the lock is acquired for certain types of locks. */
	CheckAndSetLockHeld(locallock, true);
}

/*
 * BeginStrongLockAcquire - inhibit use of fastpath for a given LOCALLOCK,
 * and arrange for error cleanup if it fails
 */
static void
BeginStrongLockAcquire(LOCALLOCK *locallock, uint32 fasthashcode)
{
	Assert(StrongLockInProgress == NULL);
	Assert(locallock->holdsStrongLockCount == false);

	/*
	 * Adding to a memory location is not atomic, so we take a spinlock to
	 * ensure we don't collide with someone else trying to bump the count at
	 * the same time.
	 *
	 * XXX: It might be worth considering using an atomic fetch-and-add
	 * instruction here, on architectures where that is supported.
	 */

	SpinLockAcquire(&FastPathStrongRelationLocks->mutex);
	FastPathStrongRelationLocks->count[fasthashcode]++;
	locallock->holdsStrongLockCount = true;
	StrongLockInProgress = locallock;
	SpinLockRelease(&FastPathStrongRelationLocks->mutex);
}

/*
 * FinishStrongLockAcquire - cancel pending cleanup for a strong lock
 * acquisition once it's no longer needed
 */
static void
FinishStrongLockAcquire(void)
{
	StrongLockInProgress = NULL;
}

/*
 * AbortStrongLockAcquire - undo strong lock state changes performed by
 * BeginStrongLockAcquire.
 */
void
AbortStrongLockAcquire(void)
{
	uint32		fasthashcode;
	LOCALLOCK  *locallock = StrongLockInProgress;

	if (locallock == NULL)
		return;

	fasthashcode = FastPathStrongLockHashPartition(locallock->hashcode);
	Assert(locallock->holdsStrongLockCount == true);
	SpinLockAcquire(&FastPathStrongRelationLocks->mutex);
	Assert(FastPathStrongRelationLocks->count[fasthashcode] > 0);
	FastPathStrongRelationLocks->count[fasthashcode]--;
	locallock->holdsStrongLockCount = false;
	StrongLockInProgress = NULL;
	SpinLockRelease(&FastPathStrongRelationLocks->mutex);
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
 * GetAwaitedLock -- Return the lock we're currently doing WaitOnLock on.
 */
LOCALLOCK *
GetAwaitedLock(void)
{
	return awaitedLock;
}

/*
 * ResetAwaitedLock -- Forget that we are waiting on a lock.
 */
void
ResetAwaitedLock(void)
{
	awaitedLock = NULL;
}

/*
 * MarkLockClear -- mark an acquired lock as "clear"
 *
 * This means that we know we have absorbed all sinval messages that other
 * sessions generated before we acquired this lock, and so we can confidently
 * assume we know about any catalog changes protected by this lock.
 */
void
MarkLockClear(LOCALLOCK *locallock)
{
	Assert(locallock->nLocks > 0);
	locallock->lockCleared = true;
}

/*
 * WaitOnLock -- wait to acquire a lock
 *
 * This is a wrapper around ProcSleep, with extra tracing and bookkeeping.
 */
static ProcWaitStatus
WaitOnLock(LOCALLOCK *locallock, ResourceOwner owner)
{
	ProcWaitStatus result;

	TRACE_POSTGRESQL_LOCK_WAIT_START(locallock->tag.lock.locktag_field1,
									 locallock->tag.lock.locktag_field2,
									 locallock->tag.lock.locktag_field3,
									 locallock->tag.lock.locktag_field4,
									 locallock->tag.lock.locktag_type,
									 locallock->tag.mode);

	/* adjust the process title to indicate that it's waiting */
	set_ps_display_suffix("waiting");

	/*
	 * Record the fact that we are waiting for a lock, so that
	 * LockErrorCleanup will clean up if cancel/die happens.
	 */
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
	 * we can't do additional work on return.
	 *
	 * We can and do use a PG_TRY block to try to clean up after failure, but
	 * this still has a major limitation: elog(FATAL) can occur while waiting
	 * (eg, a "die" interrupt), and then control won't come back here. So all
	 * cleanup of essential state should happen in LockErrorCleanup, not here.
	 * We can use PG_TRY to clear the "waiting" status flags, since doing that
	 * is unimportant if the process exits.
	 */
	PG_TRY();
	{
		result = ProcSleep(locallock);
	}
	PG_CATCH();
	{
		/* In this path, awaitedLock remains set until LockErrorCleanup */

		/* reset ps display to remove the suffix */
		set_ps_display_remove_suffix();

		/* and propagate the error */
		PG_RE_THROW();
	}
	PG_END_TRY();

	/*
	 * We no longer want LockErrorCleanup to do anything.
	 */
	awaitedLock = NULL;

	/* reset ps display to remove the suffix */
	set_ps_display_remove_suffix();

	TRACE_POSTGRESQL_LOCK_WAIT_DONE(locallock->tag.lock.locktag_field1,
									locallock->tag.lock.locktag_field2,
									locallock->tag.lock.locktag_field3,
									locallock->tag.lock.locktag_field4,
									locallock->tag.lock.locktag_type,
									locallock->tag.mode);

	return result;
}

/*
 * Remove a proc from the wait-queue it is on (caller must know it is on one).
 * This is only used when the proc has failed to get the lock, so we set its
 * waitStatus to PROC_WAIT_STATUS_ERROR.
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
	Assert(proc->waitStatus == PROC_WAIT_STATUS_WAITING);
	Assert(proc->links.next != NULL);
	Assert(waitLock);
	Assert(!dclist_is_empty(&waitLock->waitProcs));
	Assert(0 < lockmethodid && lockmethodid < lengthof(LockMethods));

	/* Remove proc from lock's wait queue */
	dclist_delete_from_thoroughly(&waitLock->waitProcs, &proc->links);

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
	proc->waitStatus = PROC_WAIT_STATUS_ERROR;

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
	LWLock	   *partitionLock;
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
	MemSet(&localtag, 0, sizeof(localtag)); /* must clear padding */
	localtag.lock = *locktag;
	localtag.mode = lockmode;

	locallock = (LOCALLOCK *) hash_search(LockMethodLocalHash,
										  &localtag,
										  HASH_FIND, NULL);

	/*
	 * let the caller print its own error message, too. Do not ereport(ERROR).
	 */
	if (!locallock || locallock->nLocks <= 0)
	{
		elog(WARNING, "you don't own a lock of type %s",
			 lockMethodTable->lockModeNames[lockmode]);
		return false;
	}

	/*
	 * Decrease the count for the resource owner.
	 */
	{
		LOCALLOCKOWNER *lockOwners = locallock->lockOwners;
		ResourceOwner owner;
		int			i;

		/* Identify owner for lock */
		if (sessionLock)
			owner = NULL;
		else
			owner = CurrentResourceOwner;

		for (i = locallock->numLockOwners - 1; i >= 0; i--)
		{
			if (lockOwners[i].owner == owner)
			{
				Assert(lockOwners[i].nLocks > 0);
				if (--lockOwners[i].nLocks == 0)
				{
					if (owner != NULL)
						ResourceOwnerForgetLock(owner, locallock);
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
			return false;
		}
	}

	/*
	 * Decrease the total local count.  If we're still holding the lock, we're
	 * done.
	 */
	locallock->nLocks--;

	if (locallock->nLocks > 0)
		return true;

	/*
	 * At this point we can no longer suppose we are clear of invalidation
	 * messages related to this lock.  Although we'll delete the LOCALLOCK
	 * object before any intentional return from this routine, it seems worth
	 * the trouble to explicitly reset lockCleared right now, just in case
	 * some error prevents us from deleting the LOCALLOCK.
	 */
	locallock->lockCleared = false;

	/* Attempt fast release of any lock eligible for the fast path. */
	if (EligibleForRelationFastPath(locktag, lockmode) &&
		FastPathLocalUseCounts[FAST_PATH_REL_GROUP(locktag->locktag_field2)] > 0)
	{
		bool		released;

		/*
		 * We might not find the lock here, even if we originally entered it
		 * here.  Another backend may have moved it to the main table.
		 */
		LWLockAcquire(&MyProc->fpInfoLock, LW_EXCLUSIVE);
		released = FastPathUnGrantRelationLock(locktag->locktag_field2,
											   lockmode);
		LWLockRelease(&MyProc->fpInfoLock);
		if (released)
		{
			RemoveLocalLock(locallock);
			return true;
		}
	}

	/*
	 * Otherwise we've got to mess with the shared lock table.
	 */
	partitionLock = LockHashPartitionLock(locallock->hashcode);

	LWLockAcquire(partitionLock, LW_EXCLUSIVE);

	/*
	 * Normally, we don't need to re-find the lock or proclock, since we kept
	 * their addresses in the locallock table, and they couldn't have been
	 * removed while we were holding a lock on them.  But it's possible that
	 * the lock was taken fast-path and has since been moved to the main hash
	 * table by another backend, in which case we will need to look up the
	 * objects here.  We assume the lock field is NULL if so.
	 */
	lock = locallock->lock;
	if (!lock)
	{
		PROCLOCKTAG proclocktag;

		Assert(EligibleForRelationFastPath(locktag, lockmode));
		lock = (LOCK *) hash_search_with_hash_value(LockMethodLockHash,
													locktag,
													locallock->hashcode,
													HASH_FIND,
													NULL);
		if (!lock)
			elog(ERROR, "failed to re-find shared lock object");
		locallock->lock = lock;

		proclocktag.myLock = lock;
		proclocktag.myProc = MyProc;
		locallock->proclock = (PROCLOCK *) hash_search(LockMethodProcLockHash,
													   &proclocktag,
													   HASH_FIND,
													   NULL);
		if (!locallock->proclock)
			elog(ERROR, "failed to re-find shared proclock object");
	}
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
		return false;
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
	return true;
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
	int			partition;
	bool		have_fast_path_lwlock = false;

	if (lockmethodid <= 0 || lockmethodid >= lengthof(LockMethods))
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);
	lockMethodTable = LockMethods[lockmethodid];

#ifdef LOCK_DEBUG
	if (*(lockMethodTable->trace_flag))
		elog(LOG, "LockReleaseAll: lockmethod=%d", lockmethodid);
#endif

	/*
	 * Get rid of our fast-path VXID lock, if appropriate.  Note that this is
	 * the only way that the lock we hold on our own VXID can ever get
	 * released: it is always and only released when a toplevel transaction
	 * ends.
	 */
	if (lockmethodid == DEFAULT_LOCKMETHOD)
		VirtualXactLockTableCleanup();

	numLockModes = lockMethodTable->numLockModes;

	/*
	 * First we run through the locallock table and get rid of unwanted
	 * entries, then we scan the process's proclocks and get rid of those. We
	 * do this separately because we may have multiple locallock entries
	 * pointing to the same proclock, and we daren't end up with any dangling
	 * pointers.  Fast-path locks are cleaned up during the locallock table
	 * scan, though.
	 */
	hash_seq_init(&status, LockMethodLocalHash);

	while ((locallock = (LOCALLOCK *) hash_seq_search(&status)) != NULL)
	{
		/*
		 * If the LOCALLOCK entry is unused, something must've gone wrong
		 * while trying to acquire this lock.  Just forget the local entry.
		 */
		if (locallock->nLocks == 0)
		{
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

			/* If session lock is above array position 0, move it down to 0 */
			for (i = 0; i < locallock->numLockOwners; i++)
			{
				if (lockOwners[i].owner == NULL)
					lockOwners[0] = lockOwners[i];
				else
					ResourceOwnerForgetLock(lockOwners[i].owner, locallock);
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
			else
				locallock->numLockOwners = 0;
		}

#ifdef USE_ASSERT_CHECKING

		/*
		 * Tuple locks are currently held only for short durations within a
		 * transaction. Check that we didn't forget to release one.
		 */
		if (LOCALLOCK_LOCKTAG(*locallock) == LOCKTAG_TUPLE && !allLocks)
			elog(WARNING, "tuple lock held at commit");
#endif

		/*
		 * If the lock or proclock pointers are NULL, this lock was taken via
		 * the relation fast-path (and is not known to have been transferred).
		 */
		if (locallock->proclock == NULL || locallock->lock == NULL)
		{
			LOCKMODE	lockmode = locallock->tag.mode;
			Oid			relid;

			/* Verify that a fast-path lock is what we've got. */
			if (!EligibleForRelationFastPath(&locallock->tag.lock, lockmode))
				elog(PANIC, "locallock table corrupted");

			/*
			 * If we don't currently hold the LWLock that protects our
			 * fast-path data structures, we must acquire it before attempting
			 * to release the lock via the fast-path.  We will continue to
			 * hold the LWLock until we're done scanning the locallock table,
			 * unless we hit a transferred fast-path lock.  (XXX is this
			 * really such a good idea?  There could be a lot of entries ...)
			 */
			if (!have_fast_path_lwlock)
			{
				LWLockAcquire(&MyProc->fpInfoLock, LW_EXCLUSIVE);
				have_fast_path_lwlock = true;
			}

			/* Attempt fast-path release. */
			relid = locallock->tag.lock.locktag_field2;
			if (FastPathUnGrantRelationLock(relid, lockmode))
			{
				RemoveLocalLock(locallock);
				continue;
			}

			/*
			 * Our lock, originally taken via the fast path, has been
			 * transferred to the main lock table.  That's going to require
			 * some extra work, so release our fast-path lock before starting.
			 */
			LWLockRelease(&MyProc->fpInfoLock);
			have_fast_path_lwlock = false;

			/*
			 * Now dump the lock.  We haven't got a pointer to the LOCK or
			 * PROCLOCK in this case, so we have to handle this a bit
			 * differently than a normal lock release.  Unfortunately, this
			 * requires an extra LWLock acquire-and-release cycle on the
			 * partitionLock, but hopefully it shouldn't happen often.
			 */
			LockRefindAndRelease(lockMethodTable, MyProc,
								 &locallock->tag.lock, lockmode, false);
			RemoveLocalLock(locallock);
			continue;
		}

		/* Mark the proclock to show we need to release this lockmode */
		if (locallock->nLocks > 0)
			locallock->proclock->releaseMask |= LOCKBIT_ON(locallock->tag.mode);

		/* And remove the locallock hashtable entry */
		RemoveLocalLock(locallock);
	}

	/* Done with the fast-path data structures */
	if (have_fast_path_lwlock)
		LWLockRelease(&MyProc->fpInfoLock);

	/*
	 * Now, scan each lock partition separately.
	 */
	for (partition = 0; partition < NUM_LOCK_PARTITIONS; partition++)
	{
		LWLock	   *partitionLock;
		dlist_head *procLocks = &MyProc->myProcLocks[partition];
		dlist_mutable_iter proclock_iter;

		partitionLock = LockHashPartitionLockByIndex(partition);

		/*
		 * If the proclock list for this partition is empty, we can skip
		 * acquiring the partition lock.  This optimization is trickier than
		 * it looks, because another backend could be in process of adding
		 * something to our proclock list due to promoting one of our
		 * fast-path locks.  However, any such lock must be one that we
		 * decided not to delete above, so it's okay to skip it again now;
		 * we'd just decide not to delete it again.  We must, however, be
		 * careful to re-fetch the list header once we've acquired the
		 * partition lock, to be sure we have a valid, up-to-date pointer.
		 * (There is probably no significant risk if pointer fetch/store is
		 * atomic, but we don't wish to assume that.)
		 *
		 * XXX This argument assumes that the locallock table correctly
		 * represents all of our fast-path locks.  While allLocks mode
		 * guarantees to clean up all of our normal locks regardless of the
		 * locallock situation, we lose that guarantee for fast-path locks.
		 * This is not ideal.
		 */
		if (dlist_is_empty(procLocks))
			continue;			/* needn't examine this partition */

		LWLockAcquire(partitionLock, LW_EXCLUSIVE);

		dlist_foreach_modify(proclock_iter, procLocks)
		{
			PROCLOCK   *proclock = dlist_container(PROCLOCK, procLink, proclock_iter.cur);
			bool		wakeupNeeded = false;

			Assert(proclock->tag.myProc == MyProc);

			lock = proclock->tag.myLock;

			/* Ignore items that are not of the lockmethod to be removed */
			if (LOCK_LOCKMETHOD(*lock) != lockmethodid)
				continue;

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
				continue;

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
		}						/* loop over PROCLOCKs within this partition */

		LWLockRelease(partitionLock);
	}							/* loop over partitions */

#ifdef LOCK_DEBUG
	if (*(lockMethodTable->trace_flag))
		elog(LOG, "LockReleaseAll done");
#endif
}

/*
 * LockReleaseSession -- Release all session locks of the specified lock method
 *		that are held by the current process.
 */
void
LockReleaseSession(LOCKMETHODID lockmethodid)
{
	HASH_SEQ_STATUS status;
	LOCALLOCK  *locallock;

	if (lockmethodid <= 0 || lockmethodid >= lengthof(LockMethods))
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);

	hash_seq_init(&status, LockMethodLocalHash);

	while ((locallock = (LOCALLOCK *) hash_seq_search(&status)) != NULL)
	{
		/* Ignore items that are not of the specified lock method */
		if (LOCALLOCK_LOCKMETHOD(*locallock) != lockmethodid)
			continue;

		ReleaseLockIfHeld(locallock, true);
	}
}

/*
 * LockReleaseCurrentOwner
 *		Release all locks belonging to CurrentResourceOwner
 *
 * If the caller knows what those locks are, it can pass them as an array.
 * That speeds up the call significantly, when a lot of locks are held.
 * Otherwise, pass NULL for locallocks, and we'll traverse through our hash
 * table to find them.
 */
void
LockReleaseCurrentOwner(LOCALLOCK **locallocks, int nlocks)
{
	if (locallocks == NULL)
	{
		HASH_SEQ_STATUS status;
		LOCALLOCK  *locallock;

		hash_seq_init(&status, LockMethodLocalHash);

		while ((locallock = (LOCALLOCK *) hash_seq_search(&status)) != NULL)
			ReleaseLockIfHeld(locallock, false);
	}
	else
	{
		int			i;

		for (i = nlocks - 1; i >= 0; i--)
			ReleaseLockIfHeld(locallocks[i], false);
	}
}

/*
 * ReleaseLockIfHeld
 *		Release any session-level locks on this lockable object if sessionLock
 *		is true; else, release any locks held by CurrentResourceOwner.
 *
 * It is tempting to pass this a ResourceOwner pointer (or NULL for session
 * locks), but without refactoring LockRelease() we cannot support releasing
 * locks belonging to resource owners other than CurrentResourceOwner.
 * If we were to refactor, it'd be a good idea to fix it so we don't have to
 * do a hashtable lookup of the locallock, too.  However, currently this
 * function isn't used heavily enough to justify refactoring for its
 * convenience.
 */
static void
ReleaseLockIfHeld(LOCALLOCK *locallock, bool sessionLock)
{
	ResourceOwner owner;
	LOCALLOCKOWNER *lockOwners;
	int			i;

	/* Identify owner for lock (must match LockRelease!) */
	if (sessionLock)
		owner = NULL;
	else
		owner = CurrentResourceOwner;

	/* Scan to see if there are any locks belonging to the target owner */
	lockOwners = locallock->lockOwners;
	for (i = locallock->numLockOwners - 1; i >= 0; i--)
	{
		if (lockOwners[i].owner == owner)
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
				if (owner != NULL)
					ResourceOwnerForgetLock(owner, locallock);
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
								 sessionLock))
					elog(WARNING, "ReleaseLockIfHeld: failed??");
			}
			break;
		}
	}
}

/*
 * LockReassignCurrentOwner
 *		Reassign all locks belonging to CurrentResourceOwner to belong
 *		to its parent resource owner.
 *
 * If the caller knows what those locks are, it can pass them as an array.
 * That speeds up the call significantly, when a lot of locks are held
 * (e.g pg_dump with a large schema).  Otherwise, pass NULL for locallocks,
 * and we'll traverse through our hash table to find them.
 */
void
LockReassignCurrentOwner(LOCALLOCK **locallocks, int nlocks)
{
	ResourceOwner parent = ResourceOwnerGetParent(CurrentResourceOwner);

	Assert(parent != NULL);

	if (locallocks == NULL)
	{
		HASH_SEQ_STATUS status;
		LOCALLOCK  *locallock;

		hash_seq_init(&status, LockMethodLocalHash);

		while ((locallock = (LOCALLOCK *) hash_seq_search(&status)) != NULL)
			LockReassignOwner(locallock, parent);
	}
	else
	{
		int			i;

		for (i = nlocks - 1; i >= 0; i--)
			LockReassignOwner(locallocks[i], parent);
	}
}

/*
 * Subroutine of LockReassignCurrentOwner. Reassigns a given lock belonging to
 * CurrentResourceOwner to its parent.
 */
static void
LockReassignOwner(LOCALLOCK *locallock, ResourceOwner parent)
{
	LOCALLOCKOWNER *lockOwners;
	int			i;
	int			ic = -1;
	int			ip = -1;

	/*
	 * Scan to see if there are any locks belonging to current owner or its
	 * parent
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
		return;					/* no current locks */

	if (ip < 0)
	{
		/* Parent has no slot, so just give it the child's slot */
		lockOwners[ic].owner = parent;
		ResourceOwnerRememberLock(parent, locallock);
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
	ResourceOwnerForgetLock(CurrentResourceOwner, locallock);
}

/*
 * FastPathGrantRelationLock
 *		Grant lock using per-backend fast-path array, if there is space.
 */
static bool
FastPathGrantRelationLock(Oid relid, LOCKMODE lockmode)
{
	uint32		i;
	uint32		unused_slot = FastPathLockSlotsPerBackend();

	/* fast-path group the lock belongs to */
	uint32		group = FAST_PATH_REL_GROUP(relid);

	/* Scan for existing entry for this relid, remembering empty slot. */
	for (i = 0; i < FP_LOCK_SLOTS_PER_GROUP; i++)
	{
		/* index into the whole per-backend array */
		uint32		f = FAST_PATH_SLOT(group, i);

		if (FAST_PATH_GET_BITS(MyProc, f) == 0)
			unused_slot = f;
		else if (MyProc->fpRelId[f] == relid)
		{
			Assert(!FAST_PATH_CHECK_LOCKMODE(MyProc, f, lockmode));
			FAST_PATH_SET_LOCKMODE(MyProc, f, lockmode);
			return true;
		}
	}

	/* If no existing entry, use any empty slot. */
	if (unused_slot < FastPathLockSlotsPerBackend())
	{
		MyProc->fpRelId[unused_slot] = relid;
		FAST_PATH_SET_LOCKMODE(MyProc, unused_slot, lockmode);
		++FastPathLocalUseCounts[group];
		return true;
	}

	/* No existing entry, and no empty slot. */
	return false;
}

/*
 * FastPathUnGrantRelationLock
 *		Release fast-path lock, if present.  Update backend-private local
 *		use count, while we're at it.
 */
static bool
FastPathUnGrantRelationLock(Oid relid, LOCKMODE lockmode)
{
	uint32		i;
	bool		result = false;

	/* fast-path group the lock belongs to */
	uint32		group = FAST_PATH_REL_GROUP(relid);

	FastPathLocalUseCounts[group] = 0;
	for (i = 0; i < FP_LOCK_SLOTS_PER_GROUP; i++)
	{
		/* index into the whole per-backend array */
		uint32		f = FAST_PATH_SLOT(group, i);

		if (MyProc->fpRelId[f] == relid
			&& FAST_PATH_CHECK_LOCKMODE(MyProc, f, lockmode))
		{
			Assert(!result);
			FAST_PATH_CLEAR_LOCKMODE(MyProc, f, lockmode);
			result = true;
			/* we continue iterating so as to update FastPathLocalUseCount */
		}
		if (FAST_PATH_GET_BITS(MyProc, f) != 0)
			++FastPathLocalUseCounts[group];
	}
	return result;
}

/*
 * FastPathTransferRelationLocks
 *		Transfer locks matching the given lock tag from per-backend fast-path
 *		arrays to the shared hash table.
 *
 * Returns true if successful, false if ran out of shared memory.
 */
static bool
FastPathTransferRelationLocks(LockMethod lockMethodTable, const LOCKTAG *locktag,
							  uint32 hashcode)
{
	LWLock	   *partitionLock = LockHashPartitionLock(hashcode);
	Oid			relid = locktag->locktag_field2;
	uint32		i;

	/* fast-path group the lock belongs to */
	uint32		group = FAST_PATH_REL_GROUP(relid);

	/*
	 * Every PGPROC that can potentially hold a fast-path lock is present in
	 * ProcGlobal->allProcs.  Prepared transactions are not, but any
	 * outstanding fast-path locks held by prepared transactions are
	 * transferred to the main lock table.
	 */
	for (i = 0; i < ProcGlobal->allProcCount; i++)
	{
		PGPROC	   *proc = &ProcGlobal->allProcs[i];
		uint32		j;

		LWLockAcquire(&proc->fpInfoLock, LW_EXCLUSIVE);

		/*
		 * If the target backend isn't referencing the same database as the
		 * lock, then we needn't examine the individual relation IDs at all;
		 * none of them can be relevant.
		 *
		 * proc->databaseId is set at backend startup time and never changes
		 * thereafter, so it might be safe to perform this test before
		 * acquiring &proc->fpInfoLock.  In particular, it's certainly safe to
		 * assume that if the target backend holds any fast-path locks, it
		 * must have performed a memory-fencing operation (in particular, an
		 * LWLock acquisition) since setting proc->databaseId.  However, it's
		 * less clear that our backend is certain to have performed a memory
		 * fencing operation since the other backend set proc->databaseId.  So
		 * for now, we test it after acquiring the LWLock just to be safe.
		 *
		 * Also skip groups without any registered fast-path locks.
		 */
		if (proc->databaseId != locktag->locktag_field1 ||
			proc->fpLockBits[group] == 0)
		{
			LWLockRelease(&proc->fpInfoLock);
			continue;
		}

		for (j = 0; j < FP_LOCK_SLOTS_PER_GROUP; j++)
		{
			uint32		lockmode;

			/* index into the whole per-backend array */
			uint32		f = FAST_PATH_SLOT(group, j);

			/* Look for an allocated slot matching the given relid. */
			if (relid != proc->fpRelId[f] || FAST_PATH_GET_BITS(proc, f) == 0)
				continue;

			/* Find or create lock object. */
			LWLockAcquire(partitionLock, LW_EXCLUSIVE);
			for (lockmode = FAST_PATH_LOCKNUMBER_OFFSET;
				 lockmode < FAST_PATH_LOCKNUMBER_OFFSET + FAST_PATH_BITS_PER_SLOT;
				 ++lockmode)
			{
				PROCLOCK   *proclock;

				if (!FAST_PATH_CHECK_LOCKMODE(proc, f, lockmode))
					continue;
				proclock = SetupLockInTable(lockMethodTable, proc, locktag,
											hashcode, lockmode);
				if (!proclock)
				{
					LWLockRelease(partitionLock);
					LWLockRelease(&proc->fpInfoLock);
					return false;
				}
				GrantLock(proclock->tag.myLock, proclock, lockmode);
				FAST_PATH_CLEAR_LOCKMODE(proc, f, lockmode);
			}
			LWLockRelease(partitionLock);

			/* No need to examine remaining slots. */
			break;
		}
		LWLockRelease(&proc->fpInfoLock);
	}
	return true;
}

/*
 * FastPathGetRelationLockEntry
 *		Return the PROCLOCK for a lock originally taken via the fast-path,
 *		transferring it to the primary lock table if necessary.
 *
 * Note: caller takes care of updating the locallock object.
 */
static PROCLOCK *
FastPathGetRelationLockEntry(LOCALLOCK *locallock)
{
	LockMethod	lockMethodTable = LockMethods[DEFAULT_LOCKMETHOD];
	LOCKTAG    *locktag = &locallock->tag.lock;
	PROCLOCK   *proclock = NULL;
	LWLock	   *partitionLock = LockHashPartitionLock(locallock->hashcode);
	Oid			relid = locktag->locktag_field2;
	uint32		i,
				group;

	/* fast-path group the lock belongs to */
	group = FAST_PATH_REL_GROUP(relid);

	LWLockAcquire(&MyProc->fpInfoLock, LW_EXCLUSIVE);

	for (i = 0; i < FP_LOCK_SLOTS_PER_GROUP; i++)
	{
		uint32		lockmode;

		/* index into the whole per-backend array */
		uint32		f = FAST_PATH_SLOT(group, i);

		/* Look for an allocated slot matching the given relid. */
		if (relid != MyProc->fpRelId[f] || FAST_PATH_GET_BITS(MyProc, f) == 0)
			continue;

		/* If we don't have a lock of the given mode, forget it! */
		lockmode = locallock->tag.mode;
		if (!FAST_PATH_CHECK_LOCKMODE(MyProc, f, lockmode))
			break;

		/* Find or create lock object. */
		LWLockAcquire(partitionLock, LW_EXCLUSIVE);

		proclock = SetupLockInTable(lockMethodTable, MyProc, locktag,
									locallock->hashcode, lockmode);
		if (!proclock)
		{
			LWLockRelease(partitionLock);
			LWLockRelease(&MyProc->fpInfoLock);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of shared memory"),
					 errhint("You might need to increase \"%s\".", "max_locks_per_transaction")));
		}
		GrantLock(proclock->tag.myLock, proclock, lockmode);
		FAST_PATH_CLEAR_LOCKMODE(MyProc, f, lockmode);

		LWLockRelease(partitionLock);

		/* No need to examine remaining slots. */
		break;
	}

	LWLockRelease(&MyProc->fpInfoLock);

	/* Lock may have already been transferred by some other backend. */
	if (proclock == NULL)
	{
		LOCK	   *lock;
		PROCLOCKTAG proclocktag;
		uint32		proclock_hashcode;

		LWLockAcquire(partitionLock, LW_SHARED);

		lock = (LOCK *) hash_search_with_hash_value(LockMethodLockHash,
													locktag,
													locallock->hashcode,
													HASH_FIND,
													NULL);
		if (!lock)
			elog(ERROR, "failed to re-find shared lock object");

		proclocktag.myLock = lock;
		proclocktag.myProc = MyProc;

		proclock_hashcode = ProcLockHashCode(&proclocktag, locallock->hashcode);
		proclock = (PROCLOCK *)
			hash_search_with_hash_value(LockMethodProcLockHash,
										&proclocktag,
										proclock_hashcode,
										HASH_FIND,
										NULL);
		if (!proclock)
			elog(ERROR, "failed to re-find shared proclock object");
		LWLockRelease(partitionLock);
	}

	return proclock;
}

/*
 * GetLockConflicts
 *		Get an array of VirtualTransactionIds of xacts currently holding locks
 *		that would conflict with the specified lock/lockmode.
 *		xacts merely awaiting such a lock are NOT reported.
 *
 * The result array is palloc'd and is terminated with an invalid VXID.
 * *countp, if not null, is updated to the number of items set.
 *
 * Of course, the result could be out of date by the time it's returned, so
 * use of this function has to be thought about carefully.  Similarly, a
 * PGPROC with no "lxid" will be considered non-conflicting regardless of any
 * lock it holds.  Existing callers don't care about a locker after that
 * locker's pg_xact updates complete.  CommitTransaction() clears "lxid" after
 * pg_xact updates and before releasing locks.
 *
 * Note we never include the current xact's vxid in the result array,
 * since an xact never blocks itself.
 */
VirtualTransactionId *
GetLockConflicts(const LOCKTAG *locktag, LOCKMODE lockmode, int *countp)
{
	static VirtualTransactionId *vxids;
	LOCKMETHODID lockmethodid = locktag->locktag_lockmethodid;
	LockMethod	lockMethodTable;
	LOCK	   *lock;
	LOCKMASK	conflictMask;
	dlist_iter	proclock_iter;
	PROCLOCK   *proclock;
	uint32		hashcode;
	LWLock	   *partitionLock;
	int			count = 0;
	int			fast_count = 0;

	if (lockmethodid <= 0 || lockmethodid >= lengthof(LockMethods))
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);
	lockMethodTable = LockMethods[lockmethodid];
	if (lockmode <= 0 || lockmode > lockMethodTable->numLockModes)
		elog(ERROR, "unrecognized lock mode: %d", lockmode);

	/*
	 * Allocate memory to store results, and fill with InvalidVXID.  We only
	 * need enough space for MaxBackends + max_prepared_xacts + a terminator.
	 * InHotStandby allocate once in TopMemoryContext.
	 */
	if (InHotStandby)
	{
		if (vxids == NULL)
			vxids = (VirtualTransactionId *)
				MemoryContextAlloc(TopMemoryContext,
								   sizeof(VirtualTransactionId) *
								   (MaxBackends + max_prepared_xacts + 1));
	}
	else
		vxids = (VirtualTransactionId *)
			palloc0(sizeof(VirtualTransactionId) *
					(MaxBackends + max_prepared_xacts + 1));

	/* Compute hash code and partition lock, and look up conflicting modes. */
	hashcode = LockTagHashCode(locktag);
	partitionLock = LockHashPartitionLock(hashcode);
	conflictMask = lockMethodTable->conflictTab[lockmode];

	/*
	 * Fast path locks might not have been entered in the primary lock table.
	 * If the lock we're dealing with could conflict with such a lock, we must
	 * examine each backend's fast-path array for conflicts.
	 */
	if (ConflictsWithRelationFastPath(locktag, lockmode))
	{
		int			i;
		Oid			relid = locktag->locktag_field2;
		VirtualTransactionId vxid;

		/* fast-path group the lock belongs to */
		uint32		group = FAST_PATH_REL_GROUP(relid);

		/*
		 * Iterate over relevant PGPROCs.  Anything held by a prepared
		 * transaction will have been transferred to the primary lock table,
		 * so we need not worry about those.  This is all a bit fuzzy, because
		 * new locks could be taken after we've visited a particular
		 * partition, but the callers had better be prepared to deal with that
		 * anyway, since the locks could equally well be taken between the
		 * time we return the value and the time the caller does something
		 * with it.
		 */
		for (i = 0; i < ProcGlobal->allProcCount; i++)
		{
			PGPROC	   *proc = &ProcGlobal->allProcs[i];
			uint32		j;

			/* A backend never blocks itself */
			if (proc == MyProc)
				continue;

			LWLockAcquire(&proc->fpInfoLock, LW_SHARED);

			/*
			 * If the target backend isn't referencing the same database as
			 * the lock, then we needn't examine the individual relation IDs
			 * at all; none of them can be relevant.
			 *
			 * See FastPathTransferRelationLocks() for discussion of why we do
			 * this test after acquiring the lock.
			 *
			 * Also skip groups without any registered fast-path locks.
			 */
			if (proc->databaseId != locktag->locktag_field1 ||
				proc->fpLockBits[group] == 0)
			{
				LWLockRelease(&proc->fpInfoLock);
				continue;
			}

			for (j = 0; j < FP_LOCK_SLOTS_PER_GROUP; j++)
			{
				uint32		lockmask;

				/* index into the whole per-backend array */
				uint32		f = FAST_PATH_SLOT(group, j);

				/* Look for an allocated slot matching the given relid. */
				if (relid != proc->fpRelId[f])
					continue;
				lockmask = FAST_PATH_GET_BITS(proc, f);
				if (!lockmask)
					continue;
				lockmask <<= FAST_PATH_LOCKNUMBER_OFFSET;

				/*
				 * There can only be one entry per relation, so if we found it
				 * and it doesn't conflict, we can skip the rest of the slots.
				 */
				if ((lockmask & conflictMask) == 0)
					break;

				/* Conflict! */
				GET_VXID_FROM_PGPROC(vxid, *proc);

				if (VirtualTransactionIdIsValid(vxid))
					vxids[count++] = vxid;
				/* else, xact already committed or aborted */

				/* No need to examine remaining slots. */
				break;
			}

			LWLockRelease(&proc->fpInfoLock);
		}
	}

	/* Remember how many fast-path conflicts we found. */
	fast_count = count;

	/*
	 * Look up the lock object matching the tag.
	 */
	LWLockAcquire(partitionLock, LW_SHARED);

	lock = (LOCK *) hash_search_with_hash_value(LockMethodLockHash,
												locktag,
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
		vxids[count].procNumber = INVALID_PROC_NUMBER;
		vxids[count].localTransactionId = InvalidLocalTransactionId;
		if (countp)
			*countp = count;
		return vxids;
	}

	/*
	 * Examine each existing holder (or awaiter) of the lock.
	 */
	dlist_foreach(proclock_iter, &lock->procLocks)
	{
		proclock = dlist_container(PROCLOCK, lockLink, proclock_iter.cur);

		if (conflictMask & proclock->holdMask)
		{
			PGPROC	   *proc = proclock->tag.myProc;

			/* A backend never blocks itself */
			if (proc != MyProc)
			{
				VirtualTransactionId vxid;

				GET_VXID_FROM_PGPROC(vxid, *proc);

				if (VirtualTransactionIdIsValid(vxid))
				{
					int			i;

					/* Avoid duplicate entries. */
					for (i = 0; i < fast_count; ++i)
						if (VirtualTransactionIdEquals(vxids[i], vxid))
							break;
					if (i >= fast_count)
						vxids[count++] = vxid;
				}
				/* else, xact already committed or aborted */
			}
		}
	}

	LWLockRelease(partitionLock);

	if (count > MaxBackends + max_prepared_xacts)	/* should never happen */
		elog(PANIC, "too many conflicting locks found");

	vxids[count].procNumber = INVALID_PROC_NUMBER;
	vxids[count].localTransactionId = InvalidLocalTransactionId;
	if (countp)
		*countp = count;
	return vxids;
}

/*
 * Find a lock in the shared lock table and release it.  It is the caller's
 * responsibility to verify that this is a sane thing to do.  (For example, it
 * would be bad to release a lock here if there might still be a LOCALLOCK
 * object with pointers to it.)
 *
 * We currently use this in two situations: first, to release locks held by
 * prepared transactions on commit (see lock_twophase_postcommit); and second,
 * to release locks taken via the fast-path, transferred to the main hash
 * table, and then released (see LockReleaseAll).
 */
static void
LockRefindAndRelease(LockMethod lockMethodTable, PGPROC *proc,
					 LOCKTAG *locktag, LOCKMODE lockmode,
					 bool decrement_strong_lock_count)
{
	LOCK	   *lock;
	PROCLOCK   *proclock;
	PROCLOCKTAG proclocktag;
	uint32		hashcode;
	uint32		proclock_hashcode;
	LWLock	   *partitionLock;
	bool		wakeupNeeded;

	hashcode = LockTagHashCode(locktag);
	partitionLock = LockHashPartitionLock(hashcode);

	LWLockAcquire(partitionLock, LW_EXCLUSIVE);

	/*
	 * Re-find the lock object (it had better be there).
	 */
	lock = (LOCK *) hash_search_with_hash_value(LockMethodLockHash,
												locktag,
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
														&proclocktag,
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

	/*
	 * Decrement strong lock count.  This logic is needed only for 2PC.
	 */
	if (decrement_strong_lock_count
		&& ConflictsWithRelationFastPath(locktag, lockmode))
	{
		uint32		fasthashcode = FastPathStrongLockHashPartition(hashcode);

		SpinLockAcquire(&FastPathStrongRelationLocks->mutex);
		Assert(FastPathStrongRelationLocks->count[fasthashcode] > 0);
		FastPathStrongRelationLocks->count[fasthashcode]--;
		SpinLockRelease(&FastPathStrongRelationLocks->mutex);
	}
}

/*
 * CheckForSessionAndXactLocks
 *		Check to see if transaction holds both session-level and xact-level
 *		locks on the same object; if so, throw an error.
 *
 * If we have both session- and transaction-level locks on the same object,
 * PREPARE TRANSACTION must fail.  This should never happen with regular
 * locks, since we only take those at session level in some special operations
 * like VACUUM.  It's possible to hit this with advisory locks, though.
 *
 * It would be nice if we could keep the session hold and give away the
 * transactional hold to the prepared xact.  However, that would require two
 * PROCLOCK objects, and we cannot be sure that another PROCLOCK will be
 * available when it comes time for PostPrepare_Locks to do the deed.
 * So for now, we error out while we can still do so safely.
 *
 * Since the LOCALLOCK table stores a separate entry for each lockmode,
 * we can't implement this check by examining LOCALLOCK entries in isolation.
 * We must build a transient hashtable that is indexed by locktag only.
 */
static void
CheckForSessionAndXactLocks(void)
{
	typedef struct
	{
		LOCKTAG		lock;		/* identifies the lockable object */
		bool		sessLock;	/* is any lockmode held at session level? */
		bool		xactLock;	/* is any lockmode held at xact level? */
	} PerLockTagEntry;

	HASHCTL		hash_ctl;
	HTAB	   *lockhtab;
	HASH_SEQ_STATUS status;
	LOCALLOCK  *locallock;

	/* Create a local hash table keyed by LOCKTAG only */
	hash_ctl.keysize = sizeof(LOCKTAG);
	hash_ctl.entrysize = sizeof(PerLockTagEntry);
	hash_ctl.hcxt = CurrentMemoryContext;

	lockhtab = hash_create("CheckForSessionAndXactLocks table",
						   256, /* arbitrary initial size */
						   &hash_ctl,
						   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/* Scan local lock table to find entries for each LOCKTAG */
	hash_seq_init(&status, LockMethodLocalHash);

	while ((locallock = (LOCALLOCK *) hash_seq_search(&status)) != NULL)
	{
		LOCALLOCKOWNER *lockOwners = locallock->lockOwners;
		PerLockTagEntry *hentry;
		bool		found;
		int			i;

		/*
		 * Ignore VXID locks.  We don't want those to be held by prepared
		 * transactions, since they aren't meaningful after a restart.
		 */
		if (locallock->tag.lock.locktag_type == LOCKTAG_VIRTUALTRANSACTION)
			continue;

		/* Ignore it if we don't actually hold the lock */
		if (locallock->nLocks <= 0)
			continue;

		/* Otherwise, find or make an entry in lockhtab */
		hentry = (PerLockTagEntry *) hash_search(lockhtab,
												 &locallock->tag.lock,
												 HASH_ENTER, &found);
		if (!found)				/* initialize, if newly created */
			hentry->sessLock = hentry->xactLock = false;

		/* Scan to see if we hold lock at session or xact level or both */
		for (i = locallock->numLockOwners - 1; i >= 0; i--)
		{
			if (lockOwners[i].owner == NULL)
				hentry->sessLock = true;
			else
				hentry->xactLock = true;
		}

		/*
		 * We can throw error immediately when we see both types of locks; no
		 * need to wait around to see if there are more violations.
		 */
		if (hentry->sessLock && hentry->xactLock)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot PREPARE while holding both session-level and transaction-level locks on the same object")));
	}

	/* Success, so clean up */
	hash_destroy(lockhtab);
}

/*
 * AtPrepare_Locks
 *		Do the preparatory work for a PREPARE: make 2PC state file records
 *		for all locks currently held.
 *
 * Session-level locks are ignored, as are VXID locks.
 *
 * For the most part, we don't need to touch shared memory for this ---
 * all the necessary state information is in the locallock table.
 * Fast-path locks are an exception, however: we move any such locks to
 * the main table before allowing PREPARE TRANSACTION to succeed.
 */
void
AtPrepare_Locks(void)
{
	HASH_SEQ_STATUS status;
	LOCALLOCK  *locallock;

	/* First, verify there aren't locks of both xact and session level */
	CheckForSessionAndXactLocks();

	/* Now do the per-locallock cleanup work */
	hash_seq_init(&status, LockMethodLocalHash);

	while ((locallock = (LOCALLOCK *) hash_seq_search(&status)) != NULL)
	{
		TwoPhaseLockRecord record;
		LOCALLOCKOWNER *lockOwners = locallock->lockOwners;
		bool		haveSessionLock;
		bool		haveXactLock;
		int			i;

		/*
		 * Ignore VXID locks.  We don't want those to be held by prepared
		 * transactions, since they aren't meaningful after a restart.
		 */
		if (locallock->tag.lock.locktag_type == LOCKTAG_VIRTUALTRANSACTION)
			continue;

		/* Ignore it if we don't actually hold the lock */
		if (locallock->nLocks <= 0)
			continue;

		/* Scan to see whether we hold it at session or transaction level */
		haveSessionLock = haveXactLock = false;
		for (i = locallock->numLockOwners - 1; i >= 0; i--)
		{
			if (lockOwners[i].owner == NULL)
				haveSessionLock = true;
			else
				haveXactLock = true;
		}

		/* Ignore it if we have only session lock */
		if (!haveXactLock)
			continue;

		/* This can't happen, because we already checked it */
		if (haveSessionLock)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot PREPARE while holding both session-level and transaction-level locks on the same object")));

		/*
		 * If the local lock was taken via the fast-path, we need to move it
		 * to the primary lock table, or just get a pointer to the existing
		 * primary lock table entry if by chance it's already been
		 * transferred.
		 */
		if (locallock->proclock == NULL)
		{
			locallock->proclock = FastPathGetRelationLockEntry(locallock);
			locallock->lock = locallock->proclock->tag.myLock;
		}

		/*
		 * Arrange to not release any strong lock count held by this lock
		 * entry.  We must retain the count until the prepared transaction is
		 * committed or rolled back.
		 */
		locallock->holdsStrongLockCount = false;

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
	PGPROC	   *newproc = TwoPhaseGetDummyProc(xid, false);
	HASH_SEQ_STATUS status;
	LOCALLOCK  *locallock;
	LOCK	   *lock;
	PROCLOCK   *proclock;
	PROCLOCKTAG proclocktag;
	int			partition;

	/* Can't prepare a lock group follower. */
	Assert(MyProc->lockGroupLeader == NULL ||
		   MyProc->lockGroupLeader == MyProc);

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
		LOCALLOCKOWNER *lockOwners = locallock->lockOwners;
		bool		haveSessionLock;
		bool		haveXactLock;
		int			i;

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

		/* Ignore VXID locks */
		if (locallock->tag.lock.locktag_type == LOCKTAG_VIRTUALTRANSACTION)
			continue;

		/* Scan to see whether we hold it at session or transaction level */
		haveSessionLock = haveXactLock = false;
		for (i = locallock->numLockOwners - 1; i >= 0; i--)
		{
			if (lockOwners[i].owner == NULL)
				haveSessionLock = true;
			else
				haveXactLock = true;
		}

		/* Ignore it if we have only session lock */
		if (!haveXactLock)
			continue;

		/* This can't happen, because we already checked it */
		if (haveSessionLock)
			ereport(PANIC,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot PREPARE while holding both session-level and transaction-level locks on the same object")));

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
		LWLock	   *partitionLock;
		dlist_head *procLocks = &(MyProc->myProcLocks[partition]);
		dlist_mutable_iter proclock_iter;

		partitionLock = LockHashPartitionLockByIndex(partition);

		/*
		 * If the proclock list for this partition is empty, we can skip
		 * acquiring the partition lock.  This optimization is safer than the
		 * situation in LockReleaseAll, because we got rid of any fast-path
		 * locks during AtPrepare_Locks, so there cannot be any case where
		 * another backend is adding something to our lists now.  For safety,
		 * though, we code this the same way as in LockReleaseAll.
		 */
		if (dlist_is_empty(procLocks))
			continue;			/* needn't examine this partition */

		LWLockAcquire(partitionLock, LW_EXCLUSIVE);

		dlist_foreach_modify(proclock_iter, procLocks)
		{
			proclock = dlist_container(PROCLOCK, procLink, proclock_iter.cur);

			Assert(proclock->tag.myProc == MyProc);

			lock = proclock->tag.myLock;

			/* Ignore VXID locks */
			if (lock->tag.locktag_type == LOCKTAG_VIRTUALTRANSACTION)
				continue;

			PROCLOCK_PRINT("PostPrepare_Locks", proclock);
			LOCK_PRINT("PostPrepare_Locks", lock, 0);
			Assert(lock->nRequested >= 0);
			Assert(lock->nGranted >= 0);
			Assert(lock->nGranted <= lock->nRequested);
			Assert((proclock->holdMask & ~lock->grantMask) == 0);

			/* Ignore it if nothing to release (must be a session lock) */
			if (proclock->releaseMask == 0)
				continue;

			/* Else we should be releasing all locks */
			if (proclock->releaseMask != proclock->holdMask)
				elog(PANIC, "we seem to have dropped a bit somewhere");

			/*
			 * We cannot simply modify proclock->tag.myProc to reassign
			 * ownership of the lock, because that's part of the hash key and
			 * the proclock would then be in the wrong hash chain.  Instead
			 * use hash_update_hash_key.  (We used to create a new hash entry,
			 * but that risks out-of-memory failure if other processes are
			 * busy making proclocks too.)	We must unlink the proclock from
			 * our procLink chain and put it into the new proc's chain, too.
			 *
			 * Note: the updated proclock hash key will still belong to the
			 * same hash partition, cf proclock_hash().  So the partition lock
			 * we already hold is sufficient for this.
			 */
			dlist_delete(&proclock->procLink);

			/*
			 * Create the new hash key for the proclock.
			 */
			proclocktag.myLock = lock;
			proclocktag.myProc = newproc;

			/*
			 * Update groupLeader pointer to point to the new proc.  (We'd
			 * better not be a member of somebody else's lock group!)
			 */
			Assert(proclock->groupLeader == proclock->tag.myProc);
			proclock->groupLeader = newproc;

			/*
			 * Update the proclock.  We should not find any existing entry for
			 * the same hash key, since there can be only one entry for any
			 * given lock with my own proc.
			 */
			if (!hash_update_hash_key(LockMethodProcLockHash,
									  proclock,
									  &proclocktag))
				elog(PANIC, "duplicate entry found while reassigning a prepared transaction's locks");

			/* Re-link into the new proc's proclock list */
			dlist_push_tail(&newproc->myProcLocks[partition], &proclock->procLink);

			PROCLOCK_PRINT("PostPrepare_Locks: updated", proclock);
		}						/* loop over PROCLOCKs within this partition */

		LWLockRelease(partitionLock);
	}							/* loop over partitions */

	END_CRIT_SECTION();
}


/*
 * Estimate shared-memory space used for lock tables
 */
Size
LockManagerShmemSize(void)
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
 * The return data consists of an array of LockInstanceData objects,
 * which are a lightly abstracted version of the PROCLOCK data structures,
 * i.e. there is one entry for each unique lock and interested PGPROC.
 * It is the caller's responsibility to match up related items (such as
 * references to the same lockable object or PGPROC) if wanted.
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

	/* Guess how much space we'll need. */
	els = MaxBackends;
	el = 0;
	data->locks = (LockInstanceData *) palloc(sizeof(LockInstanceData) * els);

	/*
	 * First, we iterate through the per-backend fast-path arrays, locking
	 * them one at a time.  This might produce an inconsistent picture of the
	 * system state, but taking all of those LWLocks at the same time seems
	 * impractical (in particular, note MAX_SIMUL_LWLOCKS).  It shouldn't
	 * matter too much, because none of these locks can be involved in lock
	 * conflicts anyway - anything that might must be present in the main lock
	 * table.  (For the same reason, we don't sweat about making leaderPid
	 * completely valid.  We cannot safely dereference another backend's
	 * lockGroupLeader field without holding all lock partition locks, and
	 * it's not worth that.)
	 */
	for (i = 0; i < ProcGlobal->allProcCount; ++i)
	{
		PGPROC	   *proc = &ProcGlobal->allProcs[i];

		/* Skip backends with pid=0, as they don't hold fast-path locks */
		if (proc->pid == 0)
			continue;

		LWLockAcquire(&proc->fpInfoLock, LW_SHARED);

		for (uint32 g = 0; g < FastPathLockGroupsPerBackend; g++)
		{
			/* Skip groups without registered fast-path locks */
			if (proc->fpLockBits[g] == 0)
				continue;

			for (int j = 0; j < FP_LOCK_SLOTS_PER_GROUP; j++)
			{
				LockInstanceData *instance;
				uint32		f = FAST_PATH_SLOT(g, j);
				uint32		lockbits = FAST_PATH_GET_BITS(proc, f);

				/* Skip unallocated slots */
				if (!lockbits)
					continue;

				if (el >= els)
				{
					els += MaxBackends;
					data->locks = (LockInstanceData *)
						repalloc(data->locks, sizeof(LockInstanceData) * els);
				}

				instance = &data->locks[el];
				SET_LOCKTAG_RELATION(instance->locktag, proc->databaseId,
									 proc->fpRelId[f]);
				instance->holdMask = lockbits << FAST_PATH_LOCKNUMBER_OFFSET;
				instance->waitLockMode = NoLock;
				instance->vxid.procNumber = proc->vxid.procNumber;
				instance->vxid.localTransactionId = proc->vxid.lxid;
				instance->pid = proc->pid;
				instance->leaderPid = proc->pid;
				instance->fastpath = true;

				/*
				 * Successfully taking fast path lock means there were no
				 * conflicting locks.
				 */
				instance->waitStart = 0;

				el++;
			}
		}

		if (proc->fpVXIDLock)
		{
			VirtualTransactionId vxid;
			LockInstanceData *instance;

			if (el >= els)
			{
				els += MaxBackends;
				data->locks = (LockInstanceData *)
					repalloc(data->locks, sizeof(LockInstanceData) * els);
			}

			vxid.procNumber = proc->vxid.procNumber;
			vxid.localTransactionId = proc->fpLocalTransactionId;

			instance = &data->locks[el];
			SET_LOCKTAG_VIRTUALTRANSACTION(instance->locktag, vxid);
			instance->holdMask = LOCKBIT_ON(ExclusiveLock);
			instance->waitLockMode = NoLock;
			instance->vxid.procNumber = proc->vxid.procNumber;
			instance->vxid.localTransactionId = proc->vxid.lxid;
			instance->pid = proc->pid;
			instance->leaderPid = proc->pid;
			instance->fastpath = true;
			instance->waitStart = 0;

			el++;
		}

		LWLockRelease(&proc->fpInfoLock);
	}

	/*
	 * Next, acquire lock on the entire shared lock data structure.  We do
	 * this so that, at least for locks in the primary lock table, the state
	 * will be self-consistent.
	 *
	 * Since this is a read-only operation, we take shared instead of
	 * exclusive lock.  There's not a whole lot of point to this, because all
	 * the normal operations require exclusive lock, but it doesn't hurt
	 * anything either. It will at least allow two backends to do
	 * GetLockStatusData in parallel.
	 *
	 * Must grab LWLocks in partition-number order to avoid LWLock deadlock.
	 */
	for (i = 0; i < NUM_LOCK_PARTITIONS; i++)
		LWLockAcquire(LockHashPartitionLockByIndex(i), LW_SHARED);

	/* Now we can safely count the number of proclocks */
	data->nelements = el + hash_get_num_entries(LockMethodProcLockHash);
	if (data->nelements > els)
	{
		els = data->nelements;
		data->locks = (LockInstanceData *)
			repalloc(data->locks, sizeof(LockInstanceData) * els);
	}

	/* Now scan the tables to copy the data */
	hash_seq_init(&seqstat, LockMethodProcLockHash);

	while ((proclock = (PROCLOCK *) hash_seq_search(&seqstat)))
	{
		PGPROC	   *proc = proclock->tag.myProc;
		LOCK	   *lock = proclock->tag.myLock;
		LockInstanceData *instance = &data->locks[el];

		memcpy(&instance->locktag, &lock->tag, sizeof(LOCKTAG));
		instance->holdMask = proclock->holdMask;
		if (proc->waitLock == proclock->tag.myLock)
			instance->waitLockMode = proc->waitLockMode;
		else
			instance->waitLockMode = NoLock;
		instance->vxid.procNumber = proc->vxid.procNumber;
		instance->vxid.localTransactionId = proc->vxid.lxid;
		instance->pid = proc->pid;
		instance->leaderPid = proclock->groupLeader->pid;
		instance->fastpath = false;
		instance->waitStart = (TimestampTz) pg_atomic_read_u64(&proc->waitStart);

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
		LWLockRelease(LockHashPartitionLockByIndex(i));

	Assert(el == data->nelements);

	return data;
}

/*
 * GetBlockerStatusData - Return a summary of the lock manager's state
 * concerning locks that are blocking the specified PID or any member of
 * the PID's lock group, for use in a user-level reporting function.
 *
 * For each PID within the lock group that is awaiting some heavyweight lock,
 * the return data includes an array of LockInstanceData objects, which are
 * the same data structure used by GetLockStatusData; but unlike that function,
 * this one reports only the PROCLOCKs associated with the lock that that PID
 * is blocked on.  (Hence, all the locktags should be the same for any one
 * blocked PID.)  In addition, we return an array of the PIDs of those backends
 * that are ahead of the blocked PID in the lock's wait queue.  These can be
 * compared with the PIDs in the LockInstanceData objects to determine which
 * waiters are ahead of or behind the blocked PID in the queue.
 *
 * If blocked_pid isn't a valid backend PID or nothing in its lock group is
 * waiting on any heavyweight lock, return empty arrays.
 *
 * The design goal is to hold the LWLocks for as short a time as possible;
 * thus, this function simply makes a copy of the necessary data and releases
 * the locks, allowing the caller to contemplate and format the data for as
 * long as it pleases.
 */
BlockedProcsData *
GetBlockerStatusData(int blocked_pid)
{
	BlockedProcsData *data;
	PGPROC	   *proc;
	int			i;

	data = (BlockedProcsData *) palloc(sizeof(BlockedProcsData));

	/*
	 * Guess how much space we'll need, and preallocate.  Most of the time
	 * this will avoid needing to do repalloc while holding the LWLocks.  (We
	 * assume, but check with an Assert, that MaxBackends is enough entries
	 * for the procs[] array; the other two could need enlargement, though.)
	 */
	data->nprocs = data->nlocks = data->npids = 0;
	data->maxprocs = data->maxlocks = data->maxpids = MaxBackends;
	data->procs = (BlockedProcData *) palloc(sizeof(BlockedProcData) * data->maxprocs);
	data->locks = (LockInstanceData *) palloc(sizeof(LockInstanceData) * data->maxlocks);
	data->waiter_pids = (int *) palloc(sizeof(int) * data->maxpids);

	/*
	 * In order to search the ProcArray for blocked_pid and assume that that
	 * entry won't immediately disappear under us, we must hold ProcArrayLock.
	 * In addition, to examine the lock grouping fields of any other backend,
	 * we must hold all the hash partition locks.  (Only one of those locks is
	 * actually relevant for any one lock group, but we can't know which one
	 * ahead of time.)	It's fairly annoying to hold all those locks
	 * throughout this, but it's no worse than GetLockStatusData(), and it
	 * does have the advantage that we're guaranteed to return a
	 * self-consistent instantaneous state.
	 */
	LWLockAcquire(ProcArrayLock, LW_SHARED);

	proc = BackendPidGetProcWithLock(blocked_pid);

	/* Nothing to do if it's gone */
	if (proc != NULL)
	{
		/*
		 * Acquire lock on the entire shared lock data structure.  See notes
		 * in GetLockStatusData().
		 */
		for (i = 0; i < NUM_LOCK_PARTITIONS; i++)
			LWLockAcquire(LockHashPartitionLockByIndex(i), LW_SHARED);

		if (proc->lockGroupLeader == NULL)
		{
			/* Easy case, proc is not a lock group member */
			GetSingleProcBlockerStatusData(proc, data);
		}
		else
		{
			/* Examine all procs in proc's lock group */
			dlist_iter	iter;

			dlist_foreach(iter, &proc->lockGroupLeader->lockGroupMembers)
			{
				PGPROC	   *memberProc;

				memberProc = dlist_container(PGPROC, lockGroupLink, iter.cur);
				GetSingleProcBlockerStatusData(memberProc, data);
			}
		}

		/*
		 * And release locks.  See notes in GetLockStatusData().
		 */
		for (i = NUM_LOCK_PARTITIONS; --i >= 0;)
			LWLockRelease(LockHashPartitionLockByIndex(i));

		Assert(data->nprocs <= data->maxprocs);
	}

	LWLockRelease(ProcArrayLock);

	return data;
}

/* Accumulate data about one possibly-blocked proc for GetBlockerStatusData */
static void
GetSingleProcBlockerStatusData(PGPROC *blocked_proc, BlockedProcsData *data)
{
	LOCK	   *theLock = blocked_proc->waitLock;
	BlockedProcData *bproc;
	dlist_iter	proclock_iter;
	dlist_iter	proc_iter;
	dclist_head *waitQueue;
	int			queue_size;

	/* Nothing to do if this proc is not blocked */
	if (theLock == NULL)
		return;

	/* Set up a procs[] element */
	bproc = &data->procs[data->nprocs++];
	bproc->pid = blocked_proc->pid;
	bproc->first_lock = data->nlocks;
	bproc->first_waiter = data->npids;

	/*
	 * We may ignore the proc's fast-path arrays, since nothing in those could
	 * be related to a contended lock.
	 */

	/* Collect all PROCLOCKs associated with theLock */
	dlist_foreach(proclock_iter, &theLock->procLocks)
	{
		PROCLOCK   *proclock =
			dlist_container(PROCLOCK, lockLink, proclock_iter.cur);
		PGPROC	   *proc = proclock->tag.myProc;
		LOCK	   *lock = proclock->tag.myLock;
		LockInstanceData *instance;

		if (data->nlocks >= data->maxlocks)
		{
			data->maxlocks += MaxBackends;
			data->locks = (LockInstanceData *)
				repalloc(data->locks, sizeof(LockInstanceData) * data->maxlocks);
		}

		instance = &data->locks[data->nlocks];
		memcpy(&instance->locktag, &lock->tag, sizeof(LOCKTAG));
		instance->holdMask = proclock->holdMask;
		if (proc->waitLock == lock)
			instance->waitLockMode = proc->waitLockMode;
		else
			instance->waitLockMode = NoLock;
		instance->vxid.procNumber = proc->vxid.procNumber;
		instance->vxid.localTransactionId = proc->vxid.lxid;
		instance->pid = proc->pid;
		instance->leaderPid = proclock->groupLeader->pid;
		instance->fastpath = false;
		data->nlocks++;
	}

	/* Enlarge waiter_pids[] if it's too small to hold all wait queue PIDs */
	waitQueue = &(theLock->waitProcs);
	queue_size = dclist_count(waitQueue);

	if (queue_size > data->maxpids - data->npids)
	{
		data->maxpids = Max(data->maxpids + MaxBackends,
							data->npids + queue_size);
		data->waiter_pids = (int *) repalloc(data->waiter_pids,
											 sizeof(int) * data->maxpids);
	}

	/* Collect PIDs from the lock's wait queue, stopping at blocked_proc */
	dclist_foreach(proc_iter, waitQueue)
	{
		PGPROC	   *queued_proc = dlist_container(PGPROC, links, proc_iter.cur);

		if (queued_proc == blocked_proc)
			break;
		data->waiter_pids[data->npids++] = queued_proc->pid;
		queued_proc = (PGPROC *) queued_proc->links.next;
	}

	bproc->num_locks = data->nlocks - bproc->first_lock;
	bproc->num_waiters = data->npids - bproc->first_waiter;
}

/*
 * Returns a list of currently held AccessExclusiveLocks, for use by
 * LogStandbySnapshot().  The result is a palloc'd array,
 * with the number of elements returned into *nlocks.
 *
 * XXX This currently takes a lock on all partitions of the lock table,
 * but it's possible to do better.  By reference counting locks and storing
 * the value in the ProcArray entry for each backend we could tell if any
 * locks need recording without having to acquire the partition locks and
 * scan the lock table.  Whether that's worth the additional overhead
 * is pretty dubious though.
 */
xl_standby_lock *
GetRunningTransactionLocks(int *nlocks)
{
	xl_standby_lock *accessExclusiveLocks;
	PROCLOCK   *proclock;
	HASH_SEQ_STATUS seqstat;
	int			i;
	int			index;
	int			els;

	/*
	 * Acquire lock on the entire shared lock data structure.
	 *
	 * Must grab LWLocks in partition-number order to avoid LWLock deadlock.
	 */
	for (i = 0; i < NUM_LOCK_PARTITIONS; i++)
		LWLockAcquire(LockHashPartitionLockByIndex(i), LW_SHARED);

	/* Now we can safely count the number of proclocks */
	els = hash_get_num_entries(LockMethodProcLockHash);

	/*
	 * Allocating enough space for all locks in the lock table is overkill,
	 * but it's more convenient and faster than having to enlarge the array.
	 */
	accessExclusiveLocks = palloc(els * sizeof(xl_standby_lock));

	/* Now scan the tables to copy the data */
	hash_seq_init(&seqstat, LockMethodProcLockHash);

	/*
	 * If lock is a currently granted AccessExclusiveLock then it will have
	 * just one proclock holder, so locks are never accessed twice in this
	 * particular case. Don't copy this code for use elsewhere because in the
	 * general case this will give you duplicate locks when looking at
	 * non-exclusive lock types.
	 */
	index = 0;
	while ((proclock = (PROCLOCK *) hash_seq_search(&seqstat)))
	{
		/* make sure this definition matches the one used in LockAcquire */
		if ((proclock->holdMask & LOCKBIT_ON(AccessExclusiveLock)) &&
			proclock->tag.myLock->tag.locktag_type == LOCKTAG_RELATION)
		{
			PGPROC	   *proc = proclock->tag.myProc;
			LOCK	   *lock = proclock->tag.myLock;
			TransactionId xid = proc->xid;

			/*
			 * Don't record locks for transactions if we know they have
			 * already issued their WAL record for commit but not yet released
			 * lock. It is still possible that we see locks held by already
			 * complete transactions, if they haven't yet zeroed their xids.
			 */
			if (!TransactionIdIsValid(xid))
				continue;

			accessExclusiveLocks[index].xid = xid;
			accessExclusiveLocks[index].dbOid = lock->tag.locktag_field1;
			accessExclusiveLocks[index].relOid = lock->tag.locktag_field2;

			index++;
		}
	}

	Assert(index <= els);

	/*
	 * And release locks.  We do this in reverse order for two reasons: (1)
	 * Anyone else who needs more than one of the locks will be trying to lock
	 * them in increasing order; we don't want to release the other process
	 * until it can get all the locks it needs. (2) This avoids O(N^2)
	 * behavior inside LWLockRelease.
	 */
	for (i = NUM_LOCK_PARTITIONS; --i >= 0;)
		LWLockRelease(LockHashPartitionLockByIndex(i));

	*nlocks = index;
	return accessExclusiveLocks;
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
	int			i;

	if (proc == NULL)
		return;

	if (proc->waitLock)
		LOCK_PRINT("DumpLocks: waiting on", proc->waitLock, 0);

	for (i = 0; i < NUM_LOCK_PARTITIONS; i++)
	{
		dlist_head *procLocks = &proc->myProcLocks[i];
		dlist_iter	iter;

		dlist_foreach(iter, procLocks)
		{
			PROCLOCK   *proclock = dlist_container(PROCLOCK, procLink, iter.cur);
			LOCK	   *lock = proclock->tag.myLock;

			Assert(proclock->tag.myProc == proc);
			PROCLOCK_PRINT("DumpLocks", proclock);
			LOCK_PRINT("DumpLocks", lock, 0);
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
#endif							/* LOCK_DEBUG */

/*
 * LOCK 2PC resource manager's routines
 */

/*
 * Re-acquire a lock belonging to a transaction that was prepared.
 *
 * Because this function is run at db startup, re-acquiring the locks should
 * never conflict with running transactions because there are none.  We
 * assume that the lock state represented by the stored 2PC files is legal.
 *
 * When switching from Hot Standby mode to normal operation, the locks will
 * be already held by the startup process. The locks are acquired for the new
 * procs without checking for conflicts, so we don't get a conflict between the
 * startup process and the dummy procs, even though we will momentarily have
 * a situation where two procs are holding the same AccessExclusiveLock,
 * which isn't normally possible because the conflict. If we're in standby
 * mode, but a recovery snapshot hasn't been established yet, it's possible
 * that some but not all of the locks are already held by the startup process.
 *
 * This approach is simple, but also a bit dangerous, because if there isn't
 * enough shared memory to acquire the locks, an error will be thrown, which
 * is promoted to FATAL and recovery will abort, bringing down postmaster.
 * A safer approach would be to transfer the locks like we do in
 * AtPrepare_Locks, but then again, in hot standby mode it's possible for
 * read-only backends to use up all the shared lock memory anyway, so that
 * replaying the WAL record that needs to acquire a lock will throw an error
 * and PANIC anyway.
 */
void
lock_twophase_recover(TransactionId xid, uint16 info,
					  void *recdata, uint32 len)
{
	TwoPhaseLockRecord *rec = (TwoPhaseLockRecord *) recdata;
	PGPROC	   *proc = TwoPhaseGetDummyProc(xid, false);
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
	LWLock	   *partitionLock;
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
												locktag,
												hashcode,
												HASH_ENTER_NULL,
												&found);
	if (!lock)
	{
		LWLockRelease(partitionLock);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory"),
				 errhint("You might need to increase \"%s\".", "max_locks_per_transaction")));
	}

	/*
	 * if it's a new lock object, initialize it
	 */
	if (!found)
	{
		lock->grantMask = 0;
		lock->waitMask = 0;
		dlist_init(&lock->procLocks);
		dclist_init(&lock->waitProcs);
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
														&proclocktag,
														proclock_hashcode,
														HASH_ENTER_NULL,
														&found);
	if (!proclock)
	{
		/* Oops, not enough shmem for the proclock */
		if (lock->nRequested == 0)
		{
			/*
			 * There are no other requestors of this lock, so garbage-collect
			 * the lock object.  We *must* do this to avoid a permanent leak
			 * of shared memory, because there won't be anything to cause
			 * anyone to release the lock object later.
			 */
			Assert(dlist_is_empty(&lock->procLocks));
			if (!hash_search_with_hash_value(LockMethodLockHash,
											 &(lock->tag),
											 hashcode,
											 HASH_REMOVE,
											 NULL))
				elog(PANIC, "lock table corrupted");
		}
		LWLockRelease(partitionLock);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory"),
				 errhint("You might need to increase \"%s\".", "max_locks_per_transaction")));
	}

	/*
	 * If new, initialize the new entry
	 */
	if (!found)
	{
		Assert(proc->lockGroupLeader == NULL);
		proclock->groupLeader = proc;
		proclock->holdMask = 0;
		proclock->releaseMask = 0;
		/* Add proclock to appropriate lists */
		dlist_push_tail(&lock->procLocks, &proclock->lockLink);
		dlist_push_tail(&proc->myProcLocks[partition],
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
	 * We ignore any possible conflicts and just grant ourselves the lock. Not
	 * only because we don't bother, but also to avoid deadlocks when
	 * switching from standby to normal mode. See function comment.
	 */
	GrantLock(lock, proclock, lockmode);

	/*
	 * Bump strong lock count, to make sure any fast-path lock requests won't
	 * be granted without consulting the primary lock table.
	 */
	if (ConflictsWithRelationFastPath(&lock->tag, lockmode))
	{
		uint32		fasthashcode = FastPathStrongLockHashPartition(hashcode);

		SpinLockAcquire(&FastPathStrongRelationLocks->mutex);
		FastPathStrongRelationLocks->count[fasthashcode]++;
		SpinLockRelease(&FastPathStrongRelationLocks->mutex);
	}

	LWLockRelease(partitionLock);
}

/*
 * Re-acquire a lock belonging to a transaction that was prepared, when
 * starting up into hot standby mode.
 */
void
lock_twophase_standby_recover(TransactionId xid, uint16 info,
							  void *recdata, uint32 len)
{
	TwoPhaseLockRecord *rec = (TwoPhaseLockRecord *) recdata;
	LOCKTAG    *locktag;
	LOCKMODE	lockmode;
	LOCKMETHODID lockmethodid;

	Assert(len == sizeof(TwoPhaseLockRecord));
	locktag = &rec->locktag;
	lockmode = rec->lockmode;
	lockmethodid = locktag->locktag_lockmethodid;

	if (lockmethodid <= 0 || lockmethodid >= lengthof(LockMethods))
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);

	if (lockmode == AccessExclusiveLock &&
		locktag->locktag_type == LOCKTAG_RELATION)
	{
		StandbyAcquireAccessExclusiveLock(xid,
										  locktag->locktag_field1 /* dboid */ ,
										  locktag->locktag_field2 /* reloid */ );
	}
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
	PGPROC	   *proc = TwoPhaseGetDummyProc(xid, true);
	LOCKTAG    *locktag;
	LOCKMETHODID lockmethodid;
	LockMethod	lockMethodTable;

	Assert(len == sizeof(TwoPhaseLockRecord));
	locktag = &rec->locktag;
	lockmethodid = locktag->locktag_lockmethodid;

	if (lockmethodid <= 0 || lockmethodid >= lengthof(LockMethods))
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);
	lockMethodTable = LockMethods[lockmethodid];

	LockRefindAndRelease(lockMethodTable, proc, locktag, rec->lockmode, true);
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

/*
 *		VirtualXactLockTableInsert
 *
 *		Take vxid lock via the fast-path.  There can't be any pre-existing
 *		lockers, as we haven't advertised this vxid via the ProcArray yet.
 *
 *		Since MyProc->fpLocalTransactionId will normally contain the same data
 *		as MyProc->vxid.lxid, you might wonder if we really need both.  The
 *		difference is that MyProc->vxid.lxid is set and cleared unlocked, and
 *		examined by procarray.c, while fpLocalTransactionId is protected by
 *		fpInfoLock and is used only by the locking subsystem.  Doing it this
 *		way makes it easier to verify that there are no funny race conditions.
 *
 *		We don't bother recording this lock in the local lock table, since it's
 *		only ever released at the end of a transaction.  Instead,
 *		LockReleaseAll() calls VirtualXactLockTableCleanup().
 */
void
VirtualXactLockTableInsert(VirtualTransactionId vxid)
{
	Assert(VirtualTransactionIdIsValid(vxid));

	LWLockAcquire(&MyProc->fpInfoLock, LW_EXCLUSIVE);

	Assert(MyProc->vxid.procNumber == vxid.procNumber);
	Assert(MyProc->fpLocalTransactionId == InvalidLocalTransactionId);
	Assert(MyProc->fpVXIDLock == false);

	MyProc->fpVXIDLock = true;
	MyProc->fpLocalTransactionId = vxid.localTransactionId;

	LWLockRelease(&MyProc->fpInfoLock);
}

/*
 *		VirtualXactLockTableCleanup
 *
 *		Check whether a VXID lock has been materialized; if so, release it,
 *		unblocking waiters.
 */
void
VirtualXactLockTableCleanup(void)
{
	bool		fastpath;
	LocalTransactionId lxid;

	Assert(MyProc->vxid.procNumber != INVALID_PROC_NUMBER);

	/*
	 * Clean up shared memory state.
	 */
	LWLockAcquire(&MyProc->fpInfoLock, LW_EXCLUSIVE);

	fastpath = MyProc->fpVXIDLock;
	lxid = MyProc->fpLocalTransactionId;
	MyProc->fpVXIDLock = false;
	MyProc->fpLocalTransactionId = InvalidLocalTransactionId;

	LWLockRelease(&MyProc->fpInfoLock);

	/*
	 * If fpVXIDLock has been cleared without touching fpLocalTransactionId,
	 * that means someone transferred the lock to the main lock table.
	 */
	if (!fastpath && LocalTransactionIdIsValid(lxid))
	{
		VirtualTransactionId vxid;
		LOCKTAG		locktag;

		vxid.procNumber = MyProcNumber;
		vxid.localTransactionId = lxid;
		SET_LOCKTAG_VIRTUALTRANSACTION(locktag, vxid);

		LockRefindAndRelease(LockMethods[DEFAULT_LOCKMETHOD], MyProc,
							 &locktag, ExclusiveLock, false);
	}
}

/*
 *		XactLockForVirtualXact
 *
 * If TransactionIdIsValid(xid), this is essentially XactLockTableWait(xid,
 * NULL, NULL, XLTW_None) or ConditionalXactLockTableWait(xid).  Unlike those
 * functions, it assumes "xid" is never a subtransaction and that "xid" is
 * prepared, committed, or aborted.
 *
 * If !TransactionIdIsValid(xid), this locks every prepared XID having been
 * known as "vxid" before its PREPARE TRANSACTION.
 */
static bool
XactLockForVirtualXact(VirtualTransactionId vxid,
					   TransactionId xid, bool wait)
{
	bool		more = false;

	/* There is no point to wait for 2PCs if you have no 2PCs. */
	if (max_prepared_xacts == 0)
		return true;

	do
	{
		LockAcquireResult lar;
		LOCKTAG		tag;

		/* Clear state from previous iterations. */
		if (more)
		{
			xid = InvalidTransactionId;
			more = false;
		}

		/* If we have no xid, try to find one. */
		if (!TransactionIdIsValid(xid))
			xid = TwoPhaseGetXidByVirtualXID(vxid, &more);
		if (!TransactionIdIsValid(xid))
		{
			Assert(!more);
			return true;
		}

		/* Check or wait for XID completion. */
		SET_LOCKTAG_TRANSACTION(tag, xid);
		lar = LockAcquire(&tag, ShareLock, false, !wait);
		if (lar == LOCKACQUIRE_NOT_AVAIL)
			return false;
		LockRelease(&tag, ShareLock, false);
	} while (more);

	return true;
}

/*
 *		VirtualXactLock
 *
 * If wait = true, wait as long as the given VXID or any XID acquired by the
 * same transaction is still running.  Then, return true.
 *
 * If wait = false, just check whether that VXID or one of those XIDs is still
 * running, and return true or false.
 */
bool
VirtualXactLock(VirtualTransactionId vxid, bool wait)
{
	LOCKTAG		tag;
	PGPROC	   *proc;
	TransactionId xid = InvalidTransactionId;

	Assert(VirtualTransactionIdIsValid(vxid));

	if (VirtualTransactionIdIsRecoveredPreparedXact(vxid))
		/* no vxid lock; localTransactionId is a normal, locked XID */
		return XactLockForVirtualXact(vxid, vxid.localTransactionId, wait);

	SET_LOCKTAG_VIRTUALTRANSACTION(tag, vxid);

	/*
	 * If a lock table entry must be made, this is the PGPROC on whose behalf
	 * it must be done.  Note that the transaction might end or the PGPROC
	 * might be reassigned to a new backend before we get around to examining
	 * it, but it doesn't matter.  If we find upon examination that the
	 * relevant lxid is no longer running here, that's enough to prove that
	 * it's no longer running anywhere.
	 */
	proc = ProcNumberGetProc(vxid.procNumber);
	if (proc == NULL)
		return XactLockForVirtualXact(vxid, InvalidTransactionId, wait);

	/*
	 * We must acquire this lock before checking the procNumber and lxid
	 * against the ones we're waiting for.  The target backend will only set
	 * or clear lxid while holding this lock.
	 */
	LWLockAcquire(&proc->fpInfoLock, LW_EXCLUSIVE);

	if (proc->vxid.procNumber != vxid.procNumber
		|| proc->fpLocalTransactionId != vxid.localTransactionId)
	{
		/* VXID ended */
		LWLockRelease(&proc->fpInfoLock);
		return XactLockForVirtualXact(vxid, InvalidTransactionId, wait);
	}

	/*
	 * If we aren't asked to wait, there's no need to set up a lock table
	 * entry.  The transaction is still in progress, so just return false.
	 */
	if (!wait)
	{
		LWLockRelease(&proc->fpInfoLock);
		return false;
	}

	/*
	 * OK, we're going to need to sleep on the VXID.  But first, we must set
	 * up the primary lock table entry, if needed (ie, convert the proc's
	 * fast-path lock on its VXID to a regular lock).
	 */
	if (proc->fpVXIDLock)
	{
		PROCLOCK   *proclock;
		uint32		hashcode;
		LWLock	   *partitionLock;

		hashcode = LockTagHashCode(&tag);

		partitionLock = LockHashPartitionLock(hashcode);
		LWLockAcquire(partitionLock, LW_EXCLUSIVE);

		proclock = SetupLockInTable(LockMethods[DEFAULT_LOCKMETHOD], proc,
									&tag, hashcode, ExclusiveLock);
		if (!proclock)
		{
			LWLockRelease(partitionLock);
			LWLockRelease(&proc->fpInfoLock);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of shared memory"),
					 errhint("You might need to increase \"%s\".", "max_locks_per_transaction")));
		}
		GrantLock(proclock->tag.myLock, proclock, ExclusiveLock);

		LWLockRelease(partitionLock);

		proc->fpVXIDLock = false;
	}

	/*
	 * If the proc has an XID now, we'll avoid a TwoPhaseGetXidByVirtualXID()
	 * search.  The proc might have assigned this XID but not yet locked it,
	 * in which case the proc will lock this XID before releasing the VXID.
	 * The fpInfoLock critical section excludes VirtualXactLockTableCleanup(),
	 * so we won't save an XID of a different VXID.  It doesn't matter whether
	 * we save this before or after setting up the primary lock table entry.
	 */
	xid = proc->xid;

	/* Done with proc->fpLockBits */
	LWLockRelease(&proc->fpInfoLock);

	/* Time to wait. */
	(void) LockAcquire(&tag, ShareLock, false, false);

	LockRelease(&tag, ShareLock, false);
	return XactLockForVirtualXact(vxid, xid, wait);
}

/*
 * LockWaiterCount
 *
 * Find the number of lock requester on this locktag
 */
int
LockWaiterCount(const LOCKTAG *locktag)
{
	LOCKMETHODID lockmethodid = locktag->locktag_lockmethodid;
	LOCK	   *lock;
	bool		found;
	uint32		hashcode;
	LWLock	   *partitionLock;
	int			waiters = 0;

	if (lockmethodid <= 0 || lockmethodid >= lengthof(LockMethods))
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);

	hashcode = LockTagHashCode(locktag);
	partitionLock = LockHashPartitionLock(hashcode);
	LWLockAcquire(partitionLock, LW_EXCLUSIVE);

	lock = (LOCK *) hash_search_with_hash_value(LockMethodLockHash,
												locktag,
												hashcode,
												HASH_FIND,
												&found);
	if (found)
	{
		Assert(lock != NULL);
		waiters = lock->nRequested;
	}
	LWLockRelease(partitionLock);

	return waiters;
}
