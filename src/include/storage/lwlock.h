/*-------------------------------------------------------------------------
 *
 * lwlock.h
 *	  Lightweight lock manager
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/lwlock.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LWLOCK_H
#define LWLOCK_H

#ifdef FRONTEND
#error "lwlock.h may not be included from frontend code"
#endif

#include "storage/proclist_types.h"
#include "storage/s_lock.h"
#include "port/atomics.h"

struct PGPROC;

/*
 * Code outside of lwlock.c should not manipulate the contents of this
 * structure directly, but we have to declare it here to allow LWLocks to be
 * incorporated into other data structures.
 */
typedef struct LWLock
{
	uint16		tranche;		/* tranche ID */
	pg_atomic_uint32 state;		/* state of exclusive/nonexclusive lockers */
	proclist_head waiters;		/* list of waiting PGPROCs */
#ifdef LOCK_DEBUG
	pg_atomic_uint32 nwaiters;	/* number of waiters */
	struct PGPROC *owner;		/* last exclusive owner of the lock */
#endif
} LWLock;

/*
 * In most cases, it's desirable to force each tranche of LWLocks to be aligned
 * on a cache line boundary and make the array stride a power of 2.  This saves
 * a few cycles in indexing, but more importantly ensures that individual
 * LWLocks don't cross cache line boundaries.  This reduces cache contention
 * problems, especially on AMD Opterons.  In some cases, it's useful to add
 * even more padding so that each LWLock takes up an entire cache line; this is
 * useful, for example, in the main LWLock array, where the overall number of
 * locks is small but some are heavily contended.
 *
 * When allocating a tranche that contains data other than LWLocks, it is
 * probably best to include a bare LWLock and then pad the resulting structure
 * as necessary for performance.  For an array that contains only LWLocks,
 * LWLockMinimallyPadded can be used for cases where we just want to ensure
 * that we don't cross cache line boundaries within a single lock, while
 * LWLockPadded can be used for cases where we want each lock to be an entire
 * cache line.
 *
 * An LWLockMinimallyPadded might contain more than the absolute minimum amount
 * of padding required to keep a lock from crossing a cache line boundary,
 * because an unpadded LWLock will normally fit into 16 bytes.  We ignore that
 * possibility when determining the minimal amount of padding.  Older releases
 * had larger LWLocks, so 32 really was the minimum, and packing them in
 * tighter might hurt performance.
 *
 * LWLOCK_MINIMAL_SIZE should be 32 on basically all common platforms, but
 * because pg_atomic_uint32 is more than 4 bytes on some obscure platforms, we
 * allow for the possibility that it might be 64.  Even on those platforms,
 * we probably won't exceed 32 bytes unless LOCK_DEBUG is defined.
 */
#define LWLOCK_PADDED_SIZE	PG_CACHE_LINE_SIZE
#define LWLOCK_MINIMAL_SIZE (sizeof(LWLock) <= 32 ? 32 : 64)

/* LWLock, padded to a full cache line size */
typedef union LWLockPadded
{
	LWLock		lock;
	char		pad[LWLOCK_PADDED_SIZE];
} LWLockPadded;

/* LWLock, minimally padded */
typedef union LWLockMinimallyPadded
{
	LWLock		lock;
	char		pad[LWLOCK_MINIMAL_SIZE];
} LWLockMinimallyPadded;

extern PGDLLIMPORT LWLockPadded *MainLWLockArray;
extern const char *const MainLWLockNames[];

/* struct for storing named tranche information */
typedef struct NamedLWLockTranche
{
	int			trancheId;
	char	   *trancheName;
} NamedLWLockTranche;

extern PGDLLIMPORT NamedLWLockTranche *NamedLWLockTrancheArray;
extern PGDLLIMPORT int NamedLWLockTrancheRequests;

/* Names for fixed lwlocks */
#include "storage/lwlocknames.h"

/*
 * It's a bit odd to declare NUM_BUFFER_PARTITIONS and NUM_LOCK_PARTITIONS
 * here, but we need them to figure out offsets within MainLWLockArray, and
 * having this file include lock.h or bufmgr.h would be backwards.
 */

/* Number of partitions of the shared buffer mapping hashtable */
#define NUM_BUFFER_PARTITIONS  128

/* Number of partitions the shared lock tables are divided into */
#define LOG2_NUM_LOCK_PARTITIONS  4
#define NUM_LOCK_PARTITIONS  (1 << LOG2_NUM_LOCK_PARTITIONS)

/* Number of partitions the shared predicate lock tables are divided into */
#define LOG2_NUM_PREDICATELOCK_PARTITIONS  4
#define NUM_PREDICATELOCK_PARTITIONS  (1 << LOG2_NUM_PREDICATELOCK_PARTITIONS)

/* Offsets for various chunks of preallocated lwlocks. */
#define BUFFER_MAPPING_LWLOCK_OFFSET	NUM_INDIVIDUAL_LWLOCKS
#define LOCK_MANAGER_LWLOCK_OFFSET		\
	(BUFFER_MAPPING_LWLOCK_OFFSET + NUM_BUFFER_PARTITIONS)
#define PREDICATELOCK_MANAGER_LWLOCK_OFFSET \
	(LOCK_MANAGER_LWLOCK_OFFSET + NUM_LOCK_PARTITIONS)
#define NUM_FIXED_LWLOCKS \
	(PREDICATELOCK_MANAGER_LWLOCK_OFFSET + NUM_PREDICATELOCK_PARTITIONS)

typedef enum LWLockMode
{
	LW_EXCLUSIVE,
	LW_SHARED,
	LW_WAIT_UNTIL_FREE			/* A special mode used in PGPROC->lwlockMode,
								 * when waiting for lock to become free. Not
								 * to be used as LWLockAcquire argument */
} LWLockMode;


#ifdef LOCK_DEBUG
extern bool Trace_lwlocks;
#endif

extern bool LWLockAcquire(LWLock *lock, LWLockMode mode);
extern bool LWLockConditionalAcquire(LWLock *lock, LWLockMode mode);
extern bool LWLockAcquireOrWait(LWLock *lock, LWLockMode mode);
extern void LWLockRelease(LWLock *lock);
extern void LWLockReleaseClearVar(LWLock *lock, uint64 *valptr, uint64 val);
extern void LWLockReleaseAll(void);
extern bool LWLockHeldByMe(LWLock *lock);
extern bool LWLockHeldByMeInMode(LWLock *lock, LWLockMode mode);

extern bool LWLockWaitForVar(LWLock *lock, uint64 *valptr, uint64 oldval, uint64 *newval);
extern void LWLockUpdateVar(LWLock *lock, uint64 *valptr, uint64 value);

extern Size LWLockShmemSize(void);
extern void CreateLWLocks(void);
extern void InitLWLockAccess(void);

extern const char *GetLWLockIdentifier(uint32 classId, uint16 eventId);

/*
 * Extensions (or core code) can obtain an LWLocks by calling
 * RequestNamedLWLockTranche() during postmaster startup.  Subsequently,
 * call GetNamedLWLockTranche() to obtain a pointer to an array containing
 * the number of LWLocks requested.
 */
extern void RequestNamedLWLockTranche(const char *tranche_name, int num_lwlocks);
extern LWLockPadded *GetNamedLWLockTranche(const char *tranche_name);

/*
 * There is another, more flexible method of obtaining lwlocks. First, call
 * LWLockNewTrancheId just once to obtain a tranche ID; this allocates from
 * a shared counter.  Next, each individual process using the tranche should
 * call LWLockRegisterTranche() to associate that tranche ID with a name.
 * Finally, LWLockInitialize should be called just once per lwlock, passing
 * the tranche ID as an argument.
 *
 * It may seem strange that each process using the tranche must register it
 * separately, but dynamic shared memory segments aren't guaranteed to be
 * mapped at the same address in all coordinating backends, so storing the
 * registration in the main shared memory segment wouldn't work for that case.
 */
extern int	LWLockNewTrancheId(void);
extern void LWLockRegisterTranche(int tranche_id, const char *tranche_name);
extern void LWLockInitialize(LWLock *lock, int tranche_id);

/*
 * Every tranche ID less than NUM_INDIVIDUAL_LWLOCKS is reserved; also,
 * we reserve additional tranche IDs for builtin tranches not included in
 * the set of individual LWLocks.  A call to LWLockNewTrancheId will never
 * return a value less than LWTRANCHE_FIRST_USER_DEFINED.
 */
typedef enum BuiltinTrancheIds
{
	LWTRANCHE_CLOG_BUFFERS = NUM_INDIVIDUAL_LWLOCKS,
	LWTRANCHE_COMMITTS_BUFFERS,
	LWTRANCHE_SUBTRANS_BUFFERS,
	LWTRANCHE_MXACTOFFSET_BUFFERS,
	LWTRANCHE_MXACTMEMBER_BUFFERS,
	LWTRANCHE_ASYNC_BUFFERS,
	LWTRANCHE_OLDSERXID_BUFFERS,
	LWTRANCHE_WAL_INSERT,
	LWTRANCHE_BUFFER_CONTENT,
	LWTRANCHE_BUFFER_IO_IN_PROGRESS,
	LWTRANCHE_REPLICATION_ORIGIN,
	LWTRANCHE_REPLICATION_SLOT_IO_IN_PROGRESS,
	LWTRANCHE_PROC,
	LWTRANCHE_BUFFER_MAPPING,
	LWTRANCHE_LOCK_MANAGER,
	LWTRANCHE_PREDICATE_LOCK_MANAGER,
	LWTRANCHE_PARALLEL_HASH_JOIN,
	LWTRANCHE_PARALLEL_QUERY_DSA,
	LWTRANCHE_SESSION_DSA,
	LWTRANCHE_SESSION_RECORD_TABLE,
	LWTRANCHE_SESSION_TYPMOD_TABLE,
	LWTRANCHE_SHARED_TUPLESTORE,
	LWTRANCHE_TBM,
	LWTRANCHE_PARALLEL_APPEND,
	LWTRANCHE_SXACT,
	LWTRANCHE_FIRST_USER_DEFINED
}			BuiltinTrancheIds;

/*
 * Prior to PostgreSQL 9.4, we used an enum type called LWLockId to refer
 * to LWLocks.  New code should instead use LWLock *.  However, for the
 * convenience of third-party code, we include the following typedef.
 */
typedef LWLock *LWLockId;

#endif							/* LWLOCK_H */
