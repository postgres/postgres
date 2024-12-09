/*-------------------------------------------------------------------------
 *
 * procarray.c
 *	  POSTGRES process array code.
 *
 *
 * This module maintains arrays of PGPROC substructures, as well as associated
 * arrays in ProcGlobal, for all active backends.  Although there are several
 * uses for this, the principal one is as a means of determining the set of
 * currently running transactions.
 *
 * Because of various subtle race conditions it is critical that a backend
 * hold the correct locks while setting or clearing its xid (in
 * ProcGlobal->xids[]/MyProc->xid).  See notes in
 * src/backend/access/transam/README.
 *
 * The process arrays now also include structures representing prepared
 * transactions.  The xid and subxids fields of these are valid, as are the
 * myProcLocks lists.  They can be distinguished from regular backend PGPROCs
 * at need by checking for pid == 0.
 *
 * During hot standby, we also keep a list of XIDs representing transactions
 * that are known to be running on the primary (or more precisely, were running
 * as of the current point in the WAL stream).  This list is kept in the
 * KnownAssignedXids array, and is updated by watching the sequence of
 * arriving XIDs.  This is necessary because if we leave those XIDs out of
 * snapshots taken for standby queries, then they will appear to be already
 * complete, leading to MVCC failures.  Note that in hot standby, the PGPROC
 * array represents standby processes, which by definition are not running
 * transactions that have XIDs.
 *
 * It is perhaps possible for a backend on the primary to terminate without
 * writing an abort record for its transaction.  While that shouldn't really
 * happen, it would tie up KnownAssignedXids indefinitely, so we protect
 * ourselves by pruning the array when a valid list of running XIDs arrives.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/procarray.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>

#include "access/subtrans.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "access/xlogutils.h"
#include "catalog/catalog.h"
#include "catalog/pg_authid.h"
#include "commands/dbcommands.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/pg_lfind.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#define UINT32_ACCESS_ONCE(var)		 ((uint32)(*((volatile uint32 *)&(var))))

/* Our shared memory area */
typedef struct ProcArrayStruct
{
	int			numProcs;		/* number of valid procs entries */
	int			maxProcs;		/* allocated size of procs array */

	/*
	 * Known assigned XIDs handling
	 */
	int			maxKnownAssignedXids;	/* allocated size of array */
	int			numKnownAssignedXids;	/* current # of valid entries */
	int			tailKnownAssignedXids;	/* index of oldest valid element */
	int			headKnownAssignedXids;	/* index of newest element, + 1 */

	/*
	 * Highest subxid that has been removed from KnownAssignedXids array to
	 * prevent overflow; or InvalidTransactionId if none.  We track this for
	 * similar reasons to tracking overflowing cached subxids in PGPROC
	 * entries.  Must hold exclusive ProcArrayLock to change this, and shared
	 * lock to read it.
	 */
	TransactionId lastOverflowedXid;

	/* oldest xmin of any replication slot */
	TransactionId replication_slot_xmin;
	/* oldest catalog xmin of any replication slot */
	TransactionId replication_slot_catalog_xmin;

	/* indexes into allProcs[], has PROCARRAY_MAXPROCS entries */
	int			pgprocnos[FLEXIBLE_ARRAY_MEMBER];
} ProcArrayStruct;

/*
 * State for the GlobalVisTest* family of functions. Those functions can
 * e.g. be used to decide if a deleted row can be removed without violating
 * MVCC semantics: If the deleted row's xmax is not considered to be running
 * by anyone, the row can be removed.
 *
 * To avoid slowing down GetSnapshotData(), we don't calculate a precise
 * cutoff XID while building a snapshot (looking at the frequently changing
 * xmins scales badly). Instead we compute two boundaries while building the
 * snapshot:
 *
 * 1) definitely_needed, indicating that rows deleted by XIDs >=
 *    definitely_needed are definitely still visible.
 *
 * 2) maybe_needed, indicating that rows deleted by XIDs < maybe_needed can
 *    definitely be removed
 *
 * When testing an XID that falls in between the two (i.e. XID >= maybe_needed
 * && XID < definitely_needed), the boundaries can be recomputed (using
 * ComputeXidHorizons()) to get a more accurate answer. This is cheaper than
 * maintaining an accurate value all the time.
 *
 * As it is not cheap to compute accurate boundaries, we limit the number of
 * times that happens in short succession. See GlobalVisTestShouldUpdate().
 *
 *
 * There are three backend lifetime instances of this struct, optimized for
 * different types of relations. As e.g. a normal user defined table in one
 * database is inaccessible to backends connected to another database, a test
 * specific to a relation can be more aggressive than a test for a shared
 * relation.  Currently we track four different states:
 *
 * 1) GlobalVisSharedRels, which only considers an XID's
 *    effects visible-to-everyone if neither snapshots in any database, nor a
 *    replication slot's xmin, nor a replication slot's catalog_xmin might
 *    still consider XID as running.
 *
 * 2) GlobalVisCatalogRels, which only considers an XID's
 *    effects visible-to-everyone if neither snapshots in the current
 *    database, nor a replication slot's xmin, nor a replication slot's
 *    catalog_xmin might still consider XID as running.
 *
 *    I.e. the difference to GlobalVisSharedRels is that
 *    snapshot in other databases are ignored.
 *
 * 3) GlobalVisDataRels, which only considers an XID's
 *    effects visible-to-everyone if neither snapshots in the current
 *    database, nor a replication slot's xmin consider XID as running.
 *
 *    I.e. the difference to GlobalVisCatalogRels is that
 *    replication slot's catalog_xmin is not taken into account.
 *
 * 4) GlobalVisTempRels, which only considers the current session, as temp
 *    tables are not visible to other sessions.
 *
 * GlobalVisTestFor(relation) returns the appropriate state
 * for the relation.
 *
 * The boundaries are FullTransactionIds instead of TransactionIds to avoid
 * wraparound dangers. There e.g. would otherwise exist no procarray state to
 * prevent maybe_needed to become old enough after the GetSnapshotData()
 * call.
 *
 * The typedef is in the header.
 */
struct GlobalVisState
{
	/* XIDs >= are considered running by some backend */
	FullTransactionId definitely_needed;

	/* XIDs < are not considered to be running by any backend */
	FullTransactionId maybe_needed;
};

/*
 * Result of ComputeXidHorizons().
 */
typedef struct ComputeXidHorizonsResult
{
	/*
	 * The value of TransamVariables->latestCompletedXid when
	 * ComputeXidHorizons() held ProcArrayLock.
	 */
	FullTransactionId latest_completed;

	/*
	 * The same for procArray->replication_slot_xmin and
	 * procArray->replication_slot_catalog_xmin.
	 */
	TransactionId slot_xmin;
	TransactionId slot_catalog_xmin;

	/*
	 * Oldest xid that any backend might still consider running. This needs to
	 * include processes running VACUUM, in contrast to the normal visibility
	 * cutoffs, as vacuum needs to be able to perform pg_subtrans lookups when
	 * determining visibility, but doesn't care about rows above its xmin to
	 * be removed.
	 *
	 * This likely should only be needed to determine whether pg_subtrans can
	 * be truncated. It currently includes the effects of replication slots,
	 * for historical reasons. But that could likely be changed.
	 */
	TransactionId oldest_considered_running;

	/*
	 * Oldest xid for which deleted tuples need to be retained in shared
	 * tables.
	 *
	 * This includes the effects of replication slots. If that's not desired,
	 * look at shared_oldest_nonremovable_raw;
	 */
	TransactionId shared_oldest_nonremovable;

	/*
	 * Oldest xid that may be necessary to retain in shared tables. This is
	 * the same as shared_oldest_nonremovable, except that is not affected by
	 * replication slot's catalog_xmin.
	 *
	 * This is mainly useful to be able to send the catalog_xmin to upstream
	 * streaming replication servers via hot_standby_feedback, so they can
	 * apply the limit only when accessing catalog tables.
	 */
	TransactionId shared_oldest_nonremovable_raw;

	/*
	 * Oldest xid for which deleted tuples need to be retained in non-shared
	 * catalog tables.
	 */
	TransactionId catalog_oldest_nonremovable;

	/*
	 * Oldest xid for which deleted tuples need to be retained in normal user
	 * defined tables.
	 */
	TransactionId data_oldest_nonremovable;

	/*
	 * Oldest xid for which deleted tuples need to be retained in this
	 * session's temporary tables.
	 */
	TransactionId temp_oldest_nonremovable;
} ComputeXidHorizonsResult;

/*
 * Return value for GlobalVisHorizonKindForRel().
 */
typedef enum GlobalVisHorizonKind
{
	VISHORIZON_SHARED,
	VISHORIZON_CATALOG,
	VISHORIZON_DATA,
	VISHORIZON_TEMP,
} GlobalVisHorizonKind;

/*
 * Reason codes for KnownAssignedXidsCompress().
 */
typedef enum KAXCompressReason
{
	KAX_NO_SPACE,				/* need to free up space at array end */
	KAX_PRUNE,					/* we just pruned old entries */
	KAX_TRANSACTION_END,		/* we just committed/removed some XIDs */
	KAX_STARTUP_PROCESS_IDLE,	/* startup process is about to sleep */
} KAXCompressReason;


static ProcArrayStruct *procArray;

static PGPROC *allProcs;

/*
 * Cache to reduce overhead of repeated calls to TransactionIdIsInProgress()
 */
static TransactionId cachedXidIsNotInProgress = InvalidTransactionId;

/*
 * Bookkeeping for tracking emulated transactions in recovery
 */
static TransactionId *KnownAssignedXids;
static bool *KnownAssignedXidsValid;
static TransactionId latestObservedXid = InvalidTransactionId;

/*
 * If we're in STANDBY_SNAPSHOT_PENDING state, standbySnapshotPendingXmin is
 * the highest xid that might still be running that we don't have in
 * KnownAssignedXids.
 */
static TransactionId standbySnapshotPendingXmin;

/*
 * State for visibility checks on different types of relations. See struct
 * GlobalVisState for details. As shared, catalog, normal and temporary
 * relations can have different horizons, one such state exists for each.
 */
static GlobalVisState GlobalVisSharedRels;
static GlobalVisState GlobalVisCatalogRels;
static GlobalVisState GlobalVisDataRels;
static GlobalVisState GlobalVisTempRels;

/*
 * This backend's RecentXmin at the last time the accurate xmin horizon was
 * recomputed, or InvalidTransactionId if it has not. Used to limit how many
 * times accurate horizons are recomputed. See GlobalVisTestShouldUpdate().
 */
static TransactionId ComputeXidHorizonsResultLastXmin;

#ifdef XIDCACHE_DEBUG

/* counters for XidCache measurement */
static long xc_by_recent_xmin = 0;
static long xc_by_known_xact = 0;
static long xc_by_my_xact = 0;
static long xc_by_latest_xid = 0;
static long xc_by_main_xid = 0;
static long xc_by_child_xid = 0;
static long xc_by_known_assigned = 0;
static long xc_no_overflow = 0;
static long xc_slow_answer = 0;

#define xc_by_recent_xmin_inc()		(xc_by_recent_xmin++)
#define xc_by_known_xact_inc()		(xc_by_known_xact++)
#define xc_by_my_xact_inc()			(xc_by_my_xact++)
#define xc_by_latest_xid_inc()		(xc_by_latest_xid++)
#define xc_by_main_xid_inc()		(xc_by_main_xid++)
#define xc_by_child_xid_inc()		(xc_by_child_xid++)
#define xc_by_known_assigned_inc()	(xc_by_known_assigned++)
#define xc_no_overflow_inc()		(xc_no_overflow++)
#define xc_slow_answer_inc()		(xc_slow_answer++)

static void DisplayXidCache(void);
#else							/* !XIDCACHE_DEBUG */

#define xc_by_recent_xmin_inc()		((void) 0)
#define xc_by_known_xact_inc()		((void) 0)
#define xc_by_my_xact_inc()			((void) 0)
#define xc_by_latest_xid_inc()		((void) 0)
#define xc_by_main_xid_inc()		((void) 0)
#define xc_by_child_xid_inc()		((void) 0)
#define xc_by_known_assigned_inc()	((void) 0)
#define xc_no_overflow_inc()		((void) 0)
#define xc_slow_answer_inc()		((void) 0)
#endif							/* XIDCACHE_DEBUG */

/* Primitives for KnownAssignedXids array handling for standby */
static void KnownAssignedXidsCompress(KAXCompressReason reason, bool haveLock);
static void KnownAssignedXidsAdd(TransactionId from_xid, TransactionId to_xid,
								 bool exclusive_lock);
static bool KnownAssignedXidsSearch(TransactionId xid, bool remove);
static bool KnownAssignedXidExists(TransactionId xid);
static void KnownAssignedXidsRemove(TransactionId xid);
static void KnownAssignedXidsRemoveTree(TransactionId xid, int nsubxids,
										TransactionId *subxids);
static void KnownAssignedXidsRemovePreceding(TransactionId removeXid);
static int	KnownAssignedXidsGet(TransactionId *xarray, TransactionId xmax);
static int	KnownAssignedXidsGetAndSetXmin(TransactionId *xarray,
										   TransactionId *xmin,
										   TransactionId xmax);
static TransactionId KnownAssignedXidsGetOldestXmin(void);
static void KnownAssignedXidsDisplay(int trace_level);
static void KnownAssignedXidsReset(void);
static inline void ProcArrayEndTransactionInternal(PGPROC *proc, TransactionId latestXid);
static void ProcArrayGroupClearXid(PGPROC *proc, TransactionId latestXid);
static void MaintainLatestCompletedXid(TransactionId latestXid);
static void MaintainLatestCompletedXidRecovery(TransactionId latestXid);

static inline FullTransactionId FullXidRelativeTo(FullTransactionId rel,
												  TransactionId xid);
static void GlobalVisUpdateApply(ComputeXidHorizonsResult *horizons);

/*
 * Report shared-memory space needed by ProcArrayShmemInit
 */
Size
ProcArrayShmemSize(void)
{
	Size		size;

	/* Size of the ProcArray structure itself */
#define PROCARRAY_MAXPROCS	(MaxBackends + max_prepared_xacts)

	size = offsetof(ProcArrayStruct, pgprocnos);
	size = add_size(size, mul_size(sizeof(int), PROCARRAY_MAXPROCS));

	/*
	 * During Hot Standby processing we have a data structure called
	 * KnownAssignedXids, created in shared memory. Local data structures are
	 * also created in various backends during GetSnapshotData(),
	 * TransactionIdIsInProgress() and GetRunningTransactionData(). All of the
	 * main structures created in those functions must be identically sized,
	 * since we may at times copy the whole of the data structures around. We
	 * refer to this size as TOTAL_MAX_CACHED_SUBXIDS.
	 *
	 * Ideally we'd only create this structure if we were actually doing hot
	 * standby in the current run, but we don't know that yet at the time
	 * shared memory is being set up.
	 */
#define TOTAL_MAX_CACHED_SUBXIDS \
	((PGPROC_MAX_CACHED_SUBXIDS + 1) * PROCARRAY_MAXPROCS)

	if (EnableHotStandby)
	{
		size = add_size(size,
						mul_size(sizeof(TransactionId),
								 TOTAL_MAX_CACHED_SUBXIDS));
		size = add_size(size,
						mul_size(sizeof(bool), TOTAL_MAX_CACHED_SUBXIDS));
	}

	return size;
}

/*
 * Initialize the shared PGPROC array during postmaster startup.
 */
void
ProcArrayShmemInit(void)
{
	bool		found;

	/* Create or attach to the ProcArray shared structure */
	procArray = (ProcArrayStruct *)
		ShmemInitStruct("Proc Array",
						add_size(offsetof(ProcArrayStruct, pgprocnos),
								 mul_size(sizeof(int),
										  PROCARRAY_MAXPROCS)),
						&found);

	if (!found)
	{
		/*
		 * We're the first - initialize.
		 */
		procArray->numProcs = 0;
		procArray->maxProcs = PROCARRAY_MAXPROCS;
		procArray->maxKnownAssignedXids = TOTAL_MAX_CACHED_SUBXIDS;
		procArray->numKnownAssignedXids = 0;
		procArray->tailKnownAssignedXids = 0;
		procArray->headKnownAssignedXids = 0;
		procArray->lastOverflowedXid = InvalidTransactionId;
		procArray->replication_slot_xmin = InvalidTransactionId;
		procArray->replication_slot_catalog_xmin = InvalidTransactionId;
		TransamVariables->xactCompletionCount = 1;
	}

	allProcs = ProcGlobal->allProcs;

	/* Create or attach to the KnownAssignedXids arrays too, if needed */
	if (EnableHotStandby)
	{
		KnownAssignedXids = (TransactionId *)
			ShmemInitStruct("KnownAssignedXids",
							mul_size(sizeof(TransactionId),
									 TOTAL_MAX_CACHED_SUBXIDS),
							&found);
		KnownAssignedXidsValid = (bool *)
			ShmemInitStruct("KnownAssignedXidsValid",
							mul_size(sizeof(bool), TOTAL_MAX_CACHED_SUBXIDS),
							&found);
	}
}

/*
 * Add the specified PGPROC to the shared array.
 */
void
ProcArrayAdd(PGPROC *proc)
{
	int			pgprocno = GetNumberFromPGProc(proc);
	ProcArrayStruct *arrayP = procArray;
	int			index;
	int			movecount;

	/* See ProcGlobal comment explaining why both locks are held */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
	LWLockAcquire(XidGenLock, LW_EXCLUSIVE);

	if (arrayP->numProcs >= arrayP->maxProcs)
	{
		/*
		 * Oops, no room.  (This really shouldn't happen, since there is a
		 * fixed supply of PGPROC structs too, and so we should have failed
		 * earlier.)
		 */
		ereport(FATAL,
				(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
				 errmsg("sorry, too many clients already")));
	}

	/*
	 * Keep the procs array sorted by (PGPROC *) so that we can utilize
	 * locality of references much better. This is useful while traversing the
	 * ProcArray because there is an increased likelihood of finding the next
	 * PGPROC structure in the cache.
	 *
	 * Since the occurrence of adding/removing a proc is much lower than the
	 * access to the ProcArray itself, the overhead should be marginal
	 */
	for (index = 0; index < arrayP->numProcs; index++)
	{
		int			this_procno = arrayP->pgprocnos[index];

		Assert(this_procno >= 0 && this_procno < (arrayP->maxProcs + NUM_AUXILIARY_PROCS));
		Assert(allProcs[this_procno].pgxactoff == index);

		/* If we have found our right position in the array, break */
		if (this_procno > pgprocno)
			break;
	}

	movecount = arrayP->numProcs - index;
	memmove(&arrayP->pgprocnos[index + 1],
			&arrayP->pgprocnos[index],
			movecount * sizeof(*arrayP->pgprocnos));
	memmove(&ProcGlobal->xids[index + 1],
			&ProcGlobal->xids[index],
			movecount * sizeof(*ProcGlobal->xids));
	memmove(&ProcGlobal->subxidStates[index + 1],
			&ProcGlobal->subxidStates[index],
			movecount * sizeof(*ProcGlobal->subxidStates));
	memmove(&ProcGlobal->statusFlags[index + 1],
			&ProcGlobal->statusFlags[index],
			movecount * sizeof(*ProcGlobal->statusFlags));

	arrayP->pgprocnos[index] = GetNumberFromPGProc(proc);
	proc->pgxactoff = index;
	ProcGlobal->xids[index] = proc->xid;
	ProcGlobal->subxidStates[index] = proc->subxidStatus;
	ProcGlobal->statusFlags[index] = proc->statusFlags;

	arrayP->numProcs++;

	/* adjust pgxactoff for all following PGPROCs */
	index++;
	for (; index < arrayP->numProcs; index++)
	{
		int			procno = arrayP->pgprocnos[index];

		Assert(procno >= 0 && procno < (arrayP->maxProcs + NUM_AUXILIARY_PROCS));
		Assert(allProcs[procno].pgxactoff == index - 1);

		allProcs[procno].pgxactoff = index;
	}

	/*
	 * Release in reversed acquisition order, to reduce frequency of having to
	 * wait for XidGenLock while holding ProcArrayLock.
	 */
	LWLockRelease(XidGenLock);
	LWLockRelease(ProcArrayLock);
}

/*
 * Remove the specified PGPROC from the shared array.
 *
 * When latestXid is a valid XID, we are removing a live 2PC gxact from the
 * array, and thus causing it to appear as "not running" anymore.  In this
 * case we must advance latestCompletedXid.  (This is essentially the same
 * as ProcArrayEndTransaction followed by removal of the PGPROC, but we take
 * the ProcArrayLock only once, and don't damage the content of the PGPROC;
 * twophase.c depends on the latter.)
 */
void
ProcArrayRemove(PGPROC *proc, TransactionId latestXid)
{
	ProcArrayStruct *arrayP = procArray;
	int			myoff;
	int			movecount;

#ifdef XIDCACHE_DEBUG
	/* dump stats at backend shutdown, but not prepared-xact end */
	if (proc->pid != 0)
		DisplayXidCache();
#endif

	/* See ProcGlobal comment explaining why both locks are held */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
	LWLockAcquire(XidGenLock, LW_EXCLUSIVE);

	myoff = proc->pgxactoff;

	Assert(myoff >= 0 && myoff < arrayP->numProcs);
	Assert(ProcGlobal->allProcs[arrayP->pgprocnos[myoff]].pgxactoff == myoff);

	if (TransactionIdIsValid(latestXid))
	{
		Assert(TransactionIdIsValid(ProcGlobal->xids[myoff]));

		/* Advance global latestCompletedXid while holding the lock */
		MaintainLatestCompletedXid(latestXid);

		/* Same with xactCompletionCount  */
		TransamVariables->xactCompletionCount++;

		ProcGlobal->xids[myoff] = InvalidTransactionId;
		ProcGlobal->subxidStates[myoff].overflowed = false;
		ProcGlobal->subxidStates[myoff].count = 0;
	}
	else
	{
		/* Shouldn't be trying to remove a live transaction here */
		Assert(!TransactionIdIsValid(ProcGlobal->xids[myoff]));
	}

	Assert(!TransactionIdIsValid(ProcGlobal->xids[myoff]));
	Assert(ProcGlobal->subxidStates[myoff].count == 0);
	Assert(ProcGlobal->subxidStates[myoff].overflowed == false);

	ProcGlobal->statusFlags[myoff] = 0;

	/* Keep the PGPROC array sorted. See notes above */
	movecount = arrayP->numProcs - myoff - 1;
	memmove(&arrayP->pgprocnos[myoff],
			&arrayP->pgprocnos[myoff + 1],
			movecount * sizeof(*arrayP->pgprocnos));
	memmove(&ProcGlobal->xids[myoff],
			&ProcGlobal->xids[myoff + 1],
			movecount * sizeof(*ProcGlobal->xids));
	memmove(&ProcGlobal->subxidStates[myoff],
			&ProcGlobal->subxidStates[myoff + 1],
			movecount * sizeof(*ProcGlobal->subxidStates));
	memmove(&ProcGlobal->statusFlags[myoff],
			&ProcGlobal->statusFlags[myoff + 1],
			movecount * sizeof(*ProcGlobal->statusFlags));

	arrayP->pgprocnos[arrayP->numProcs - 1] = -1;	/* for debugging */
	arrayP->numProcs--;

	/*
	 * Adjust pgxactoff of following procs for removed PGPROC (note that
	 * numProcs already has been decremented).
	 */
	for (int index = myoff; index < arrayP->numProcs; index++)
	{
		int			procno = arrayP->pgprocnos[index];

		Assert(procno >= 0 && procno < (arrayP->maxProcs + NUM_AUXILIARY_PROCS));
		Assert(allProcs[procno].pgxactoff - 1 == index);

		allProcs[procno].pgxactoff = index;
	}

	/*
	 * Release in reversed acquisition order, to reduce frequency of having to
	 * wait for XidGenLock while holding ProcArrayLock.
	 */
	LWLockRelease(XidGenLock);
	LWLockRelease(ProcArrayLock);
}


/*
 * ProcArrayEndTransaction -- mark a transaction as no longer running
 *
 * This is used interchangeably for commit and abort cases.  The transaction
 * commit/abort must already be reported to WAL and pg_xact.
 *
 * proc is currently always MyProc, but we pass it explicitly for flexibility.
 * latestXid is the latest Xid among the transaction's main XID and
 * subtransactions, or InvalidTransactionId if it has no XID.  (We must ask
 * the caller to pass latestXid, instead of computing it from the PGPROC's
 * contents, because the subxid information in the PGPROC might be
 * incomplete.)
 */
void
ProcArrayEndTransaction(PGPROC *proc, TransactionId latestXid)
{
	if (TransactionIdIsValid(latestXid))
	{
		/*
		 * We must lock ProcArrayLock while clearing our advertised XID, so
		 * that we do not exit the set of "running" transactions while someone
		 * else is taking a snapshot.  See discussion in
		 * src/backend/access/transam/README.
		 */
		Assert(TransactionIdIsValid(proc->xid));

		/*
		 * If we can immediately acquire ProcArrayLock, we clear our own XID
		 * and release the lock.  If not, use group XID clearing to improve
		 * efficiency.
		 */
		if (LWLockConditionalAcquire(ProcArrayLock, LW_EXCLUSIVE))
		{
			ProcArrayEndTransactionInternal(proc, latestXid);
			LWLockRelease(ProcArrayLock);
		}
		else
			ProcArrayGroupClearXid(proc, latestXid);
	}
	else
	{
		/*
		 * If we have no XID, we don't need to lock, since we won't affect
		 * anyone else's calculation of a snapshot.  We might change their
		 * estimate of global xmin, but that's OK.
		 */
		Assert(!TransactionIdIsValid(proc->xid));
		Assert(proc->subxidStatus.count == 0);
		Assert(!proc->subxidStatus.overflowed);

		proc->vxid.lxid = InvalidLocalTransactionId;
		proc->xmin = InvalidTransactionId;

		/* be sure this is cleared in abort */
		proc->delayChkptFlags = 0;

		proc->recoveryConflictPending = false;

		/* must be cleared with xid/xmin: */
		/* avoid unnecessarily dirtying shared cachelines */
		if (proc->statusFlags & PROC_VACUUM_STATE_MASK)
		{
			Assert(!LWLockHeldByMe(ProcArrayLock));
			LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
			Assert(proc->statusFlags == ProcGlobal->statusFlags[proc->pgxactoff]);
			proc->statusFlags &= ~PROC_VACUUM_STATE_MASK;
			ProcGlobal->statusFlags[proc->pgxactoff] = proc->statusFlags;
			LWLockRelease(ProcArrayLock);
		}
	}
}

/*
 * Mark a write transaction as no longer running.
 *
 * We don't do any locking here; caller must handle that.
 */
static inline void
ProcArrayEndTransactionInternal(PGPROC *proc, TransactionId latestXid)
{
	int			pgxactoff = proc->pgxactoff;

	/*
	 * Note: we need exclusive lock here because we're going to change other
	 * processes' PGPROC entries.
	 */
	Assert(LWLockHeldByMeInMode(ProcArrayLock, LW_EXCLUSIVE));
	Assert(TransactionIdIsValid(ProcGlobal->xids[pgxactoff]));
	Assert(ProcGlobal->xids[pgxactoff] == proc->xid);

	ProcGlobal->xids[pgxactoff] = InvalidTransactionId;
	proc->xid = InvalidTransactionId;
	proc->vxid.lxid = InvalidLocalTransactionId;
	proc->xmin = InvalidTransactionId;

	/* be sure this is cleared in abort */
	proc->delayChkptFlags = 0;

	proc->recoveryConflictPending = false;

	/* must be cleared with xid/xmin: */
	/* avoid unnecessarily dirtying shared cachelines */
	if (proc->statusFlags & PROC_VACUUM_STATE_MASK)
	{
		proc->statusFlags &= ~PROC_VACUUM_STATE_MASK;
		ProcGlobal->statusFlags[proc->pgxactoff] = proc->statusFlags;
	}

	/* Clear the subtransaction-XID cache too while holding the lock */
	Assert(ProcGlobal->subxidStates[pgxactoff].count == proc->subxidStatus.count &&
		   ProcGlobal->subxidStates[pgxactoff].overflowed == proc->subxidStatus.overflowed);
	if (proc->subxidStatus.count > 0 || proc->subxidStatus.overflowed)
	{
		ProcGlobal->subxidStates[pgxactoff].count = 0;
		ProcGlobal->subxidStates[pgxactoff].overflowed = false;
		proc->subxidStatus.count = 0;
		proc->subxidStatus.overflowed = false;
	}

	/* Also advance global latestCompletedXid while holding the lock */
	MaintainLatestCompletedXid(latestXid);

	/* Same with xactCompletionCount  */
	TransamVariables->xactCompletionCount++;
}

/*
 * ProcArrayGroupClearXid -- group XID clearing
 *
 * When we cannot immediately acquire ProcArrayLock in exclusive mode at
 * commit time, add ourselves to a list of processes that need their XIDs
 * cleared.  The first process to add itself to the list will acquire
 * ProcArrayLock in exclusive mode and perform ProcArrayEndTransactionInternal
 * on behalf of all group members.  This avoids a great deal of contention
 * around ProcArrayLock when many processes are trying to commit at once,
 * since the lock need not be repeatedly handed off from one committing
 * process to the next.
 */
static void
ProcArrayGroupClearXid(PGPROC *proc, TransactionId latestXid)
{
	int			pgprocno = GetNumberFromPGProc(proc);
	PROC_HDR   *procglobal = ProcGlobal;
	uint32		nextidx;
	uint32		wakeidx;

	/* We should definitely have an XID to clear. */
	Assert(TransactionIdIsValid(proc->xid));

	/* Add ourselves to the list of processes needing a group XID clear. */
	proc->procArrayGroupMember = true;
	proc->procArrayGroupMemberXid = latestXid;
	nextidx = pg_atomic_read_u32(&procglobal->procArrayGroupFirst);
	while (true)
	{
		pg_atomic_write_u32(&proc->procArrayGroupNext, nextidx);

		if (pg_atomic_compare_exchange_u32(&procglobal->procArrayGroupFirst,
										   &nextidx,
										   (uint32) pgprocno))
			break;
	}

	/*
	 * If the list was not empty, the leader will clear our XID.  It is
	 * impossible to have followers without a leader because the first process
	 * that has added itself to the list will always have nextidx as
	 * INVALID_PROC_NUMBER.
	 */
	if (nextidx != INVALID_PROC_NUMBER)
	{
		int			extraWaits = 0;

		/* Sleep until the leader clears our XID. */
		pgstat_report_wait_start(WAIT_EVENT_PROCARRAY_GROUP_UPDATE);
		for (;;)
		{
			/* acts as a read barrier */
			PGSemaphoreLock(proc->sem);
			if (!proc->procArrayGroupMember)
				break;
			extraWaits++;
		}
		pgstat_report_wait_end();

		Assert(pg_atomic_read_u32(&proc->procArrayGroupNext) == INVALID_PROC_NUMBER);

		/* Fix semaphore count for any absorbed wakeups */
		while (extraWaits-- > 0)
			PGSemaphoreUnlock(proc->sem);
		return;
	}

	/* We are the leader.  Acquire the lock on behalf of everyone. */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	/*
	 * Now that we've got the lock, clear the list of processes waiting for
	 * group XID clearing, saving a pointer to the head of the list.  Trying
	 * to pop elements one at a time could lead to an ABA problem.
	 */
	nextidx = pg_atomic_exchange_u32(&procglobal->procArrayGroupFirst,
									 INVALID_PROC_NUMBER);

	/* Remember head of list so we can perform wakeups after dropping lock. */
	wakeidx = nextidx;

	/* Walk the list and clear all XIDs. */
	while (nextidx != INVALID_PROC_NUMBER)
	{
		PGPROC	   *nextproc = &allProcs[nextidx];

		ProcArrayEndTransactionInternal(nextproc, nextproc->procArrayGroupMemberXid);

		/* Move to next proc in list. */
		nextidx = pg_atomic_read_u32(&nextproc->procArrayGroupNext);
	}

	/* We're done with the lock now. */
	LWLockRelease(ProcArrayLock);

	/*
	 * Now that we've released the lock, go back and wake everybody up.  We
	 * don't do this under the lock so as to keep lock hold times to a
	 * minimum.  The system calls we need to perform to wake other processes
	 * up are probably much slower than the simple memory writes we did while
	 * holding the lock.
	 */
	while (wakeidx != INVALID_PROC_NUMBER)
	{
		PGPROC	   *nextproc = &allProcs[wakeidx];

		wakeidx = pg_atomic_read_u32(&nextproc->procArrayGroupNext);
		pg_atomic_write_u32(&nextproc->procArrayGroupNext, INVALID_PROC_NUMBER);

		/* ensure all previous writes are visible before follower continues. */
		pg_write_barrier();

		nextproc->procArrayGroupMember = false;

		if (nextproc != MyProc)
			PGSemaphoreUnlock(nextproc->sem);
	}
}

/*
 * ProcArrayClearTransaction -- clear the transaction fields
 *
 * This is used after successfully preparing a 2-phase transaction.  We are
 * not actually reporting the transaction's XID as no longer running --- it
 * will still appear as running because the 2PC's gxact is in the ProcArray
 * too.  We just have to clear out our own PGPROC.
 */
void
ProcArrayClearTransaction(PGPROC *proc)
{
	int			pgxactoff;

	/*
	 * Currently we need to lock ProcArrayLock exclusively here, as we
	 * increment xactCompletionCount below. We also need it at least in shared
	 * mode for pgproc->pgxactoff to stay the same below.
	 *
	 * We could however, as this action does not actually change anyone's view
	 * of the set of running XIDs (our entry is duplicate with the gxact that
	 * has already been inserted into the ProcArray), lower the lock level to
	 * shared if we were to make xactCompletionCount an atomic variable. But
	 * that doesn't seem worth it currently, as a 2PC commit is heavyweight
	 * enough for this not to be the bottleneck.  If it ever becomes a
	 * bottleneck it may also be worth considering to combine this with the
	 * subsequent ProcArrayRemove()
	 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	pgxactoff = proc->pgxactoff;

	ProcGlobal->xids[pgxactoff] = InvalidTransactionId;
	proc->xid = InvalidTransactionId;

	proc->vxid.lxid = InvalidLocalTransactionId;
	proc->xmin = InvalidTransactionId;
	proc->recoveryConflictPending = false;

	Assert(!(proc->statusFlags & PROC_VACUUM_STATE_MASK));
	Assert(!proc->delayChkptFlags);

	/*
	 * Need to increment completion count even though transaction hasn't
	 * really committed yet. The reason for that is that GetSnapshotData()
	 * omits the xid of the current transaction, thus without the increment we
	 * otherwise could end up reusing the snapshot later. Which would be bad,
	 * because it might not count the prepared transaction as running.
	 */
	TransamVariables->xactCompletionCount++;

	/* Clear the subtransaction-XID cache too */
	Assert(ProcGlobal->subxidStates[pgxactoff].count == proc->subxidStatus.count &&
		   ProcGlobal->subxidStates[pgxactoff].overflowed == proc->subxidStatus.overflowed);
	if (proc->subxidStatus.count > 0 || proc->subxidStatus.overflowed)
	{
		ProcGlobal->subxidStates[pgxactoff].count = 0;
		ProcGlobal->subxidStates[pgxactoff].overflowed = false;
		proc->subxidStatus.count = 0;
		proc->subxidStatus.overflowed = false;
	}

	LWLockRelease(ProcArrayLock);
}

/*
 * Update TransamVariables->latestCompletedXid to point to latestXid if
 * currently older.
 */
static void
MaintainLatestCompletedXid(TransactionId latestXid)
{
	FullTransactionId cur_latest = TransamVariables->latestCompletedXid;

	Assert(FullTransactionIdIsValid(cur_latest));
	Assert(!RecoveryInProgress());
	Assert(LWLockHeldByMe(ProcArrayLock));

	if (TransactionIdPrecedes(XidFromFullTransactionId(cur_latest), latestXid))
	{
		TransamVariables->latestCompletedXid =
			FullXidRelativeTo(cur_latest, latestXid);
	}

	Assert(IsBootstrapProcessingMode() ||
		   FullTransactionIdIsNormal(TransamVariables->latestCompletedXid));
}

/*
 * Same as MaintainLatestCompletedXid, except for use during WAL replay.
 */
static void
MaintainLatestCompletedXidRecovery(TransactionId latestXid)
{
	FullTransactionId cur_latest = TransamVariables->latestCompletedXid;
	FullTransactionId rel;

	Assert(AmStartupProcess() || !IsUnderPostmaster);
	Assert(LWLockHeldByMe(ProcArrayLock));

	/*
	 * Need a FullTransactionId to compare latestXid with. Can't rely on
	 * latestCompletedXid to be initialized in recovery. But in recovery it's
	 * safe to access nextXid without a lock for the startup process.
	 */
	rel = TransamVariables->nextXid;
	Assert(FullTransactionIdIsValid(TransamVariables->nextXid));

	if (!FullTransactionIdIsValid(cur_latest) ||
		TransactionIdPrecedes(XidFromFullTransactionId(cur_latest), latestXid))
	{
		TransamVariables->latestCompletedXid =
			FullXidRelativeTo(rel, latestXid);
	}

	Assert(FullTransactionIdIsNormal(TransamVariables->latestCompletedXid));
}

/*
 * ProcArrayInitRecovery -- initialize recovery xid mgmt environment
 *
 * Remember up to where the startup process initialized the CLOG and subtrans
 * so we can ensure it's initialized gaplessly up to the point where necessary
 * while in recovery.
 */
void
ProcArrayInitRecovery(TransactionId initializedUptoXID)
{
	Assert(standbyState == STANDBY_INITIALIZED);
	Assert(TransactionIdIsNormal(initializedUptoXID));

	/*
	 * we set latestObservedXid to the xid SUBTRANS has been initialized up
	 * to, so we can extend it from that point onwards in
	 * RecordKnownAssignedTransactionIds, and when we get consistent in
	 * ProcArrayApplyRecoveryInfo().
	 */
	latestObservedXid = initializedUptoXID;
	TransactionIdRetreat(latestObservedXid);
}

/*
 * ProcArrayApplyRecoveryInfo -- apply recovery info about xids
 *
 * Takes us through 3 states: Initialized, Pending and Ready.
 * Normal case is to go all the way to Ready straight away, though there
 * are atypical cases where we need to take it in steps.
 *
 * Use the data about running transactions on the primary to create the initial
 * state of KnownAssignedXids. We also use these records to regularly prune
 * KnownAssignedXids because we know it is possible that some transactions
 * with FATAL errors fail to write abort records, which could cause eventual
 * overflow.
 *
 * See comments for LogStandbySnapshot().
 */
void
ProcArrayApplyRecoveryInfo(RunningTransactions running)
{
	TransactionId *xids;
	TransactionId advanceNextXid;
	int			nxids;
	int			i;

	Assert(standbyState >= STANDBY_INITIALIZED);
	Assert(TransactionIdIsValid(running->nextXid));
	Assert(TransactionIdIsValid(running->oldestRunningXid));
	Assert(TransactionIdIsNormal(running->latestCompletedXid));

	/*
	 * Remove stale transactions, if any.
	 */
	ExpireOldKnownAssignedTransactionIds(running->oldestRunningXid);

	/*
	 * Adjust TransamVariables->nextXid before StandbyReleaseOldLocks(),
	 * because we will need it up to date for accessing two-phase transactions
	 * in StandbyReleaseOldLocks().
	 */
	advanceNextXid = running->nextXid;
	TransactionIdRetreat(advanceNextXid);
	AdvanceNextFullTransactionIdPastXid(advanceNextXid);
	Assert(FullTransactionIdIsValid(TransamVariables->nextXid));

	/*
	 * Remove stale locks, if any.
	 */
	StandbyReleaseOldLocks(running->oldestRunningXid);

	/*
	 * If our snapshot is already valid, nothing else to do...
	 */
	if (standbyState == STANDBY_SNAPSHOT_READY)
		return;

	/*
	 * If our initial RunningTransactionsData had an overflowed snapshot then
	 * we knew we were missing some subxids from our snapshot. If we continue
	 * to see overflowed snapshots then we might never be able to start up, so
	 * we make another test to see if our snapshot is now valid. We know that
	 * the missing subxids are equal to or earlier than nextXid. After we
	 * initialise we continue to apply changes during recovery, so once the
	 * oldestRunningXid is later than the nextXid from the initial snapshot we
	 * know that we no longer have missing information and can mark the
	 * snapshot as valid.
	 */
	if (standbyState == STANDBY_SNAPSHOT_PENDING)
	{
		/*
		 * If the snapshot isn't overflowed or if its empty we can reset our
		 * pending state and use this snapshot instead.
		 */
		if (running->subxid_status != SUBXIDS_MISSING || running->xcnt == 0)
		{
			/*
			 * If we have already collected known assigned xids, we need to
			 * throw them away before we apply the recovery snapshot.
			 */
			KnownAssignedXidsReset();
			standbyState = STANDBY_INITIALIZED;
		}
		else
		{
			if (TransactionIdPrecedes(standbySnapshotPendingXmin,
									  running->oldestRunningXid))
			{
				standbyState = STANDBY_SNAPSHOT_READY;
				elog(DEBUG1,
					 "recovery snapshots are now enabled");
			}
			else
				elog(DEBUG1,
					 "recovery snapshot waiting for non-overflowed snapshot or "
					 "until oldest active xid on standby is at least %u (now %u)",
					 standbySnapshotPendingXmin,
					 running->oldestRunningXid);
			return;
		}
	}

	Assert(standbyState == STANDBY_INITIALIZED);

	/*
	 * NB: this can be reached at least twice, so make sure new code can deal
	 * with that.
	 */

	/*
	 * Nobody else is running yet, but take locks anyhow
	 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	/*
	 * KnownAssignedXids is sorted so we cannot just add the xids, we have to
	 * sort them first.
	 *
	 * Some of the new xids are top-level xids and some are subtransactions.
	 * We don't call SubTransSetParent because it doesn't matter yet. If we
	 * aren't overflowed then all xids will fit in snapshot and so we don't
	 * need subtrans. If we later overflow, an xid assignment record will add
	 * xids to subtrans. If RunningTransactionsData is overflowed then we
	 * don't have enough information to correctly update subtrans anyway.
	 */

	/*
	 * Allocate a temporary array to avoid modifying the array passed as
	 * argument.
	 */
	xids = palloc(sizeof(TransactionId) * (running->xcnt + running->subxcnt));

	/*
	 * Add to the temp array any xids which have not already completed.
	 */
	nxids = 0;
	for (i = 0; i < running->xcnt + running->subxcnt; i++)
	{
		TransactionId xid = running->xids[i];

		/*
		 * The running-xacts snapshot can contain xids that were still visible
		 * in the procarray when the snapshot was taken, but were already
		 * WAL-logged as completed. They're not running anymore, so ignore
		 * them.
		 */
		if (TransactionIdDidCommit(xid) || TransactionIdDidAbort(xid))
			continue;

		xids[nxids++] = xid;
	}

	if (nxids > 0)
	{
		if (procArray->numKnownAssignedXids != 0)
		{
			LWLockRelease(ProcArrayLock);
			elog(ERROR, "KnownAssignedXids is not empty");
		}

		/*
		 * Sort the array so that we can add them safely into
		 * KnownAssignedXids.
		 *
		 * We have to sort them logically, because in KnownAssignedXidsAdd we
		 * call TransactionIdFollowsOrEquals and so on. But we know these XIDs
		 * come from RUNNING_XACTS, which means there are only normal XIDs
		 * from the same epoch, so this is safe.
		 */
		qsort(xids, nxids, sizeof(TransactionId), xidLogicalComparator);

		/*
		 * Add the sorted snapshot into KnownAssignedXids.  The running-xacts
		 * snapshot may include duplicated xids because of prepared
		 * transactions, so ignore them.
		 */
		for (i = 0; i < nxids; i++)
		{
			if (i > 0 && TransactionIdEquals(xids[i - 1], xids[i]))
			{
				elog(DEBUG1,
					 "found duplicated transaction %u for KnownAssignedXids insertion",
					 xids[i]);
				continue;
			}
			KnownAssignedXidsAdd(xids[i], xids[i], true);
		}

		KnownAssignedXidsDisplay(DEBUG3);
	}

	pfree(xids);

	/*
	 * latestObservedXid is at least set to the point where SUBTRANS was
	 * started up to (cf. ProcArrayInitRecovery()) or to the biggest xid
	 * RecordKnownAssignedTransactionIds() was called for.  Initialize
	 * subtrans from thereon, up to nextXid - 1.
	 *
	 * We need to duplicate parts of RecordKnownAssignedTransactionId() here,
	 * because we've just added xids to the known assigned xids machinery that
	 * haven't gone through RecordKnownAssignedTransactionId().
	 */
	Assert(TransactionIdIsNormal(latestObservedXid));
	TransactionIdAdvance(latestObservedXid);
	while (TransactionIdPrecedes(latestObservedXid, running->nextXid))
	{
		ExtendSUBTRANS(latestObservedXid);
		TransactionIdAdvance(latestObservedXid);
	}
	TransactionIdRetreat(latestObservedXid);	/* = running->nextXid - 1 */

	/* ----------
	 * Now we've got the running xids we need to set the global values that
	 * are used to track snapshots as they evolve further.
	 *
	 * - latestCompletedXid which will be the xmax for snapshots
	 * - lastOverflowedXid which shows whether snapshots overflow
	 * - nextXid
	 *
	 * If the snapshot overflowed, then we still initialise with what we know,
	 * but the recovery snapshot isn't fully valid yet because we know there
	 * are some subxids missing. We don't know the specific subxids that are
	 * missing, so conservatively assume the last one is latestObservedXid.
	 * ----------
	 */
	if (running->subxid_status == SUBXIDS_MISSING)
	{
		standbyState = STANDBY_SNAPSHOT_PENDING;

		standbySnapshotPendingXmin = latestObservedXid;
		procArray->lastOverflowedXid = latestObservedXid;
	}
	else
	{
		standbyState = STANDBY_SNAPSHOT_READY;

		standbySnapshotPendingXmin = InvalidTransactionId;

		/*
		 * If the 'xids' array didn't include all subtransactions, we have to
		 * mark any snapshots taken as overflowed.
		 */
		if (running->subxid_status == SUBXIDS_IN_SUBTRANS)
			procArray->lastOverflowedXid = latestObservedXid;
		else
		{
			Assert(running->subxid_status == SUBXIDS_IN_ARRAY);
			procArray->lastOverflowedXid = InvalidTransactionId;
		}
	}

	/*
	 * If a transaction wrote a commit record in the gap between taking and
	 * logging the snapshot then latestCompletedXid may already be higher than
	 * the value from the snapshot, so check before we use the incoming value.
	 * It also might not yet be set at all.
	 */
	MaintainLatestCompletedXidRecovery(running->latestCompletedXid);

	/*
	 * NB: No need to increment TransamVariables->xactCompletionCount here,
	 * nobody can see it yet.
	 */

	LWLockRelease(ProcArrayLock);

	KnownAssignedXidsDisplay(DEBUG3);
	if (standbyState == STANDBY_SNAPSHOT_READY)
		elog(DEBUG1, "recovery snapshots are now enabled");
	else
		elog(DEBUG1,
			 "recovery snapshot waiting for non-overflowed snapshot or "
			 "until oldest active xid on standby is at least %u (now %u)",
			 standbySnapshotPendingXmin,
			 running->oldestRunningXid);
}

/*
 * ProcArrayApplyXidAssignment
 *		Process an XLOG_XACT_ASSIGNMENT WAL record
 */
void
ProcArrayApplyXidAssignment(TransactionId topxid,
							int nsubxids, TransactionId *subxids)
{
	TransactionId max_xid;
	int			i;

	Assert(standbyState >= STANDBY_INITIALIZED);

	max_xid = TransactionIdLatest(topxid, nsubxids, subxids);

	/*
	 * Mark all the subtransactions as observed.
	 *
	 * NOTE: This will fail if the subxid contains too many previously
	 * unobserved xids to fit into known-assigned-xids. That shouldn't happen
	 * as the code stands, because xid-assignment records should never contain
	 * more than PGPROC_MAX_CACHED_SUBXIDS entries.
	 */
	RecordKnownAssignedTransactionIds(max_xid);

	/*
	 * Notice that we update pg_subtrans with the top-level xid, rather than
	 * the parent xid. This is a difference between normal processing and
	 * recovery, yet is still correct in all cases. The reason is that
	 * subtransaction commit is not marked in clog until commit processing, so
	 * all aborted subtransactions have already been clearly marked in clog.
	 * As a result we are able to refer directly to the top-level
	 * transaction's state rather than skipping through all the intermediate
	 * states in the subtransaction tree. This should be the first time we
	 * have attempted to SubTransSetParent().
	 */
	for (i = 0; i < nsubxids; i++)
		SubTransSetParent(subxids[i], topxid);

	/* KnownAssignedXids isn't maintained yet, so we're done for now */
	if (standbyState == STANDBY_INITIALIZED)
		return;

	/*
	 * Uses same locking as transaction commit
	 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	/*
	 * Remove subxids from known-assigned-xacts.
	 */
	KnownAssignedXidsRemoveTree(InvalidTransactionId, nsubxids, subxids);

	/*
	 * Advance lastOverflowedXid to be at least the last of these subxids.
	 */
	if (TransactionIdPrecedes(procArray->lastOverflowedXid, max_xid))
		procArray->lastOverflowedXid = max_xid;

	LWLockRelease(ProcArrayLock);
}

/*
 * TransactionIdIsInProgress -- is given transaction running in some backend
 *
 * Aside from some shortcuts such as checking RecentXmin and our own Xid,
 * there are four possibilities for finding a running transaction:
 *
 * 1. The given Xid is a main transaction Id.  We will find this out cheaply
 * by looking at ProcGlobal->xids.
 *
 * 2. The given Xid is one of the cached subxact Xids in the PGPROC array.
 * We can find this out cheaply too.
 *
 * 3. In Hot Standby mode, we must search the KnownAssignedXids list to see
 * if the Xid is running on the primary.
 *
 * 4. Search the SubTrans tree to find the Xid's topmost parent, and then see
 * if that is running according to ProcGlobal->xids[] or KnownAssignedXids.
 * This is the slowest way, but sadly it has to be done always if the others
 * failed, unless we see that the cached subxact sets are complete (none have
 * overflowed).
 *
 * ProcArrayLock has to be held while we do 1, 2, 3.  If we save the top Xids
 * while doing 1 and 3, we can release the ProcArrayLock while we do 4.
 * This buys back some concurrency (and we can't retrieve the main Xids from
 * ProcGlobal->xids[] again anyway; see GetNewTransactionId).
 */
bool
TransactionIdIsInProgress(TransactionId xid)
{
	static TransactionId *xids = NULL;
	static TransactionId *other_xids;
	XidCacheStatus *other_subxidstates;
	int			nxids = 0;
	ProcArrayStruct *arrayP = procArray;
	TransactionId topxid;
	TransactionId latestCompletedXid;
	int			mypgxactoff;
	int			numProcs;
	int			j;

	/*
	 * Don't bother checking a transaction older than RecentXmin; it could not
	 * possibly still be running.  (Note: in particular, this guarantees that
	 * we reject InvalidTransactionId, FrozenTransactionId, etc as not
	 * running.)
	 */
	if (TransactionIdPrecedes(xid, RecentXmin))
	{
		xc_by_recent_xmin_inc();
		return false;
	}

	/*
	 * We may have just checked the status of this transaction, so if it is
	 * already known to be completed, we can fall out without any access to
	 * shared memory.
	 */
	if (TransactionIdEquals(cachedXidIsNotInProgress, xid))
	{
		xc_by_known_xact_inc();
		return false;
	}

	/*
	 * Also, we can handle our own transaction (and subtransactions) without
	 * any access to shared memory.
	 */
	if (TransactionIdIsCurrentTransactionId(xid))
	{
		xc_by_my_xact_inc();
		return true;
	}

	/*
	 * If first time through, get workspace to remember main XIDs in. We
	 * malloc it permanently to avoid repeated palloc/pfree overhead.
	 */
	if (xids == NULL)
	{
		/*
		 * In hot standby mode, reserve enough space to hold all xids in the
		 * known-assigned list. If we later finish recovery, we no longer need
		 * the bigger array, but we don't bother to shrink it.
		 */
		int			maxxids = RecoveryInProgress() ? TOTAL_MAX_CACHED_SUBXIDS : arrayP->maxProcs;

		xids = (TransactionId *) malloc(maxxids * sizeof(TransactionId));
		if (xids == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
	}

	other_xids = ProcGlobal->xids;
	other_subxidstates = ProcGlobal->subxidStates;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	/*
	 * Now that we have the lock, we can check latestCompletedXid; if the
	 * target Xid is after that, it's surely still running.
	 */
	latestCompletedXid =
		XidFromFullTransactionId(TransamVariables->latestCompletedXid);
	if (TransactionIdPrecedes(latestCompletedXid, xid))
	{
		LWLockRelease(ProcArrayLock);
		xc_by_latest_xid_inc();
		return true;
	}

	/* No shortcuts, gotta grovel through the array */
	mypgxactoff = MyProc->pgxactoff;
	numProcs = arrayP->numProcs;
	for (int pgxactoff = 0; pgxactoff < numProcs; pgxactoff++)
	{
		int			pgprocno;
		PGPROC	   *proc;
		TransactionId pxid;
		int			pxids;

		/* Ignore ourselves --- dealt with it above */
		if (pgxactoff == mypgxactoff)
			continue;

		/* Fetch xid just once - see GetNewTransactionId */
		pxid = UINT32_ACCESS_ONCE(other_xids[pgxactoff]);

		if (!TransactionIdIsValid(pxid))
			continue;

		/*
		 * Step 1: check the main Xid
		 */
		if (TransactionIdEquals(pxid, xid))
		{
			LWLockRelease(ProcArrayLock);
			xc_by_main_xid_inc();
			return true;
		}

		/*
		 * We can ignore main Xids that are younger than the target Xid, since
		 * the target could not possibly be their child.
		 */
		if (TransactionIdPrecedes(xid, pxid))
			continue;

		/*
		 * Step 2: check the cached child-Xids arrays
		 */
		pxids = other_subxidstates[pgxactoff].count;
		pg_read_barrier();		/* pairs with barrier in GetNewTransactionId() */
		pgprocno = arrayP->pgprocnos[pgxactoff];
		proc = &allProcs[pgprocno];
		for (j = pxids - 1; j >= 0; j--)
		{
			/* Fetch xid just once - see GetNewTransactionId */
			TransactionId cxid = UINT32_ACCESS_ONCE(proc->subxids.xids[j]);

			if (TransactionIdEquals(cxid, xid))
			{
				LWLockRelease(ProcArrayLock);
				xc_by_child_xid_inc();
				return true;
			}
		}

		/*
		 * Save the main Xid for step 4.  We only need to remember main Xids
		 * that have uncached children.  (Note: there is no race condition
		 * here because the overflowed flag cannot be cleared, only set, while
		 * we hold ProcArrayLock.  So we can't miss an Xid that we need to
		 * worry about.)
		 */
		if (other_subxidstates[pgxactoff].overflowed)
			xids[nxids++] = pxid;
	}

	/*
	 * Step 3: in hot standby mode, check the known-assigned-xids list.  XIDs
	 * in the list must be treated as running.
	 */
	if (RecoveryInProgress())
	{
		/* none of the PGPROC entries should have XIDs in hot standby mode */
		Assert(nxids == 0);

		if (KnownAssignedXidExists(xid))
		{
			LWLockRelease(ProcArrayLock);
			xc_by_known_assigned_inc();
			return true;
		}

		/*
		 * If the KnownAssignedXids overflowed, we have to check pg_subtrans
		 * too.  Fetch all xids from KnownAssignedXids that are lower than
		 * xid, since if xid is a subtransaction its parent will always have a
		 * lower value.  Note we will collect both main and subXIDs here, but
		 * there's no help for it.
		 */
		if (TransactionIdPrecedesOrEquals(xid, procArray->lastOverflowedXid))
			nxids = KnownAssignedXidsGet(xids, xid);
	}

	LWLockRelease(ProcArrayLock);

	/*
	 * If none of the relevant caches overflowed, we know the Xid is not
	 * running without even looking at pg_subtrans.
	 */
	if (nxids == 0)
	{
		xc_no_overflow_inc();
		cachedXidIsNotInProgress = xid;
		return false;
	}

	/*
	 * Step 4: have to check pg_subtrans.
	 *
	 * At this point, we know it's either a subtransaction of one of the Xids
	 * in xids[], or it's not running.  If it's an already-failed
	 * subtransaction, we want to say "not running" even though its parent may
	 * still be running.  So first, check pg_xact to see if it's been aborted.
	 */
	xc_slow_answer_inc();

	if (TransactionIdDidAbort(xid))
	{
		cachedXidIsNotInProgress = xid;
		return false;
	}

	/*
	 * It isn't aborted, so check whether the transaction tree it belongs to
	 * is still running (or, more precisely, whether it was running when we
	 * held ProcArrayLock).
	 */
	topxid = SubTransGetTopmostTransaction(xid);
	Assert(TransactionIdIsValid(topxid));
	if (!TransactionIdEquals(topxid, xid) &&
		pg_lfind32(topxid, xids, nxids))
		return true;

	cachedXidIsNotInProgress = xid;
	return false;
}

/*
 * TransactionIdIsActive -- is xid the top-level XID of an active backend?
 *
 * This differs from TransactionIdIsInProgress in that it ignores prepared
 * transactions, as well as transactions running on the primary if we're in
 * hot standby.  Also, we ignore subtransactions since that's not needed
 * for current uses.
 */
bool
TransactionIdIsActive(TransactionId xid)
{
	bool		result = false;
	ProcArrayStruct *arrayP = procArray;
	TransactionId *other_xids = ProcGlobal->xids;
	int			i;

	/*
	 * Don't bother checking a transaction older than RecentXmin; it could not
	 * possibly still be running.
	 */
	if (TransactionIdPrecedes(xid, RecentXmin))
		return false;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (i = 0; i < arrayP->numProcs; i++)
	{
		int			pgprocno = arrayP->pgprocnos[i];
		PGPROC	   *proc = &allProcs[pgprocno];
		TransactionId pxid;

		/* Fetch xid just once - see GetNewTransactionId */
		pxid = UINT32_ACCESS_ONCE(other_xids[i]);

		if (!TransactionIdIsValid(pxid))
			continue;

		if (proc->pid == 0)
			continue;			/* ignore prepared transactions */

		if (TransactionIdEquals(pxid, xid))
		{
			result = true;
			break;
		}
	}

	LWLockRelease(ProcArrayLock);

	return result;
}


/*
 * Determine XID horizons.
 *
 * This is used by wrapper functions like GetOldestNonRemovableTransactionId()
 * (for VACUUM), GetReplicationHorizons() (for hot_standby_feedback), etc as
 * well as "internally" by GlobalVisUpdate() (see comment above struct
 * GlobalVisState).
 *
 * See the definition of ComputeXidHorizonsResult for the various computed
 * horizons.
 *
 * For VACUUM separate horizons (used to decide which deleted tuples must
 * be preserved), for shared and non-shared tables are computed.  For shared
 * relations backends in all databases must be considered, but for non-shared
 * relations that's not required, since only backends in my own database could
 * ever see the tuples in them. Also, we can ignore concurrently running lazy
 * VACUUMs because (a) they must be working on other tables, and (b) they
 * don't need to do snapshot-based lookups.
 *
 * This also computes a horizon used to truncate pg_subtrans. For that
 * backends in all databases have to be considered, and concurrently running
 * lazy VACUUMs cannot be ignored, as they still may perform pg_subtrans
 * accesses.
 *
 * Note: we include all currently running xids in the set of considered xids.
 * This ensures that if a just-started xact has not yet set its snapshot,
 * when it does set the snapshot it cannot set xmin less than what we compute.
 * See notes in src/backend/access/transam/README.
 *
 * Note: despite the above, it's possible for the calculated values to move
 * backwards on repeated calls. The calculated values are conservative, so
 * that anything older is definitely not considered as running by anyone
 * anymore, but the exact values calculated depend on a number of things. For
 * example, if there are no transactions running in the current database, the
 * horizon for normal tables will be latestCompletedXid. If a transaction
 * begins after that, its xmin will include in-progress transactions in other
 * databases that started earlier, so another call will return a lower value.
 * Nonetheless it is safe to vacuum a table in the current database with the
 * first result.  There are also replication-related effects: a walsender
 * process can set its xmin based on transactions that are no longer running
 * on the primary but are still being replayed on the standby, thus possibly
 * making the values go backwards.  In this case there is a possibility that
 * we lose data that the standby would like to have, but unless the standby
 * uses a replication slot to make its xmin persistent there is little we can
 * do about that --- data is only protected if the walsender runs continuously
 * while queries are executed on the standby.  (The Hot Standby code deals
 * with such cases by failing standby queries that needed to access
 * already-removed data, so there's no integrity bug.)
 *
 * Note: the approximate horizons (see definition of GlobalVisState) are
 * updated by the computations done here. That's currently required for
 * correctness and a small optimization. Without doing so it's possible that
 * heap vacuum's call to heap_page_prune_and_freeze() uses a more conservative
 * horizon than later when deciding which tuples can be removed - which the
 * code doesn't expect (breaking HOT).
 */
static void
ComputeXidHorizons(ComputeXidHorizonsResult *h)
{
	ProcArrayStruct *arrayP = procArray;
	TransactionId kaxmin;
	bool		in_recovery = RecoveryInProgress();
	TransactionId *other_xids = ProcGlobal->xids;

	/* inferred after ProcArrayLock is released */
	h->catalog_oldest_nonremovable = InvalidTransactionId;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	h->latest_completed = TransamVariables->latestCompletedXid;

	/*
	 * We initialize the MIN() calculation with latestCompletedXid + 1. This
	 * is a lower bound for the XIDs that might appear in the ProcArray later,
	 * and so protects us against overestimating the result due to future
	 * additions.
	 */
	{
		TransactionId initial;

		initial = XidFromFullTransactionId(h->latest_completed);
		Assert(TransactionIdIsValid(initial));
		TransactionIdAdvance(initial);

		h->oldest_considered_running = initial;
		h->shared_oldest_nonremovable = initial;
		h->data_oldest_nonremovable = initial;

		/*
		 * Only modifications made by this backend affect the horizon for
		 * temporary relations. Instead of a check in each iteration of the
		 * loop over all PGPROCs it is cheaper to just initialize to the
		 * current top-level xid any.
		 *
		 * Without an assigned xid we could use a horizon as aggressive as
		 * GetNewTransactionId(), but we can get away with the much cheaper
		 * latestCompletedXid + 1: If this backend has no xid there, by
		 * definition, can't be any newer changes in the temp table than
		 * latestCompletedXid.
		 */
		if (TransactionIdIsValid(MyProc->xid))
			h->temp_oldest_nonremovable = MyProc->xid;
		else
			h->temp_oldest_nonremovable = initial;
	}

	/*
	 * Fetch slot horizons while ProcArrayLock is held - the
	 * LWLockAcquire/LWLockRelease are a barrier, ensuring this happens inside
	 * the lock.
	 */
	h->slot_xmin = procArray->replication_slot_xmin;
	h->slot_catalog_xmin = procArray->replication_slot_catalog_xmin;

	for (int index = 0; index < arrayP->numProcs; index++)
	{
		int			pgprocno = arrayP->pgprocnos[index];
		PGPROC	   *proc = &allProcs[pgprocno];
		int8		statusFlags = ProcGlobal->statusFlags[index];
		TransactionId xid;
		TransactionId xmin;

		/* Fetch xid just once - see GetNewTransactionId */
		xid = UINT32_ACCESS_ONCE(other_xids[index]);
		xmin = UINT32_ACCESS_ONCE(proc->xmin);

		/*
		 * Consider both the transaction's Xmin, and its Xid.
		 *
		 * We must check both because a transaction might have an Xmin but not
		 * (yet) an Xid; conversely, if it has an Xid, that could determine
		 * some not-yet-set Xmin.
		 */
		xmin = TransactionIdOlder(xmin, xid);

		/* if neither is set, this proc doesn't influence the horizon */
		if (!TransactionIdIsValid(xmin))
			continue;

		/*
		 * Don't ignore any procs when determining which transactions might be
		 * considered running.  While slots should ensure logical decoding
		 * backends are protected even without this check, it can't hurt to
		 * include them here as well..
		 */
		h->oldest_considered_running =
			TransactionIdOlder(h->oldest_considered_running, xmin);

		/*
		 * Skip over backends either vacuuming (which is ok with rows being
		 * removed, as long as pg_subtrans is not truncated) or doing logical
		 * decoding (which manages xmin separately, check below).
		 */
		if (statusFlags & (PROC_IN_VACUUM | PROC_IN_LOGICAL_DECODING))
			continue;

		/* shared tables need to take backends in all databases into account */
		h->shared_oldest_nonremovable =
			TransactionIdOlder(h->shared_oldest_nonremovable, xmin);

		/*
		 * Normally sessions in other databases are ignored for anything but
		 * the shared horizon.
		 *
		 * However, include them when MyDatabaseId is not (yet) set.  A
		 * backend in the process of starting up must not compute a "too
		 * aggressive" horizon, otherwise we could end up using it to prune
		 * still-needed data away.  If the current backend never connects to a
		 * database this is harmless, because data_oldest_nonremovable will
		 * never be utilized.
		 *
		 * Also, sessions marked with PROC_AFFECTS_ALL_HORIZONS should always
		 * be included.  (This flag is used for hot standby feedback, which
		 * can't be tied to a specific database.)
		 *
		 * Also, while in recovery we cannot compute an accurate per-database
		 * horizon, as all xids are managed via the KnownAssignedXids
		 * machinery.
		 */
		if (proc->databaseId == MyDatabaseId ||
			MyDatabaseId == InvalidOid ||
			(statusFlags & PROC_AFFECTS_ALL_HORIZONS) ||
			in_recovery)
		{
			h->data_oldest_nonremovable =
				TransactionIdOlder(h->data_oldest_nonremovable, xmin);
		}
	}

	/*
	 * If in recovery fetch oldest xid in KnownAssignedXids, will be applied
	 * after lock is released.
	 */
	if (in_recovery)
		kaxmin = KnownAssignedXidsGetOldestXmin();

	/*
	 * No other information from shared state is needed, release the lock
	 * immediately. The rest of the computations can be done without a lock.
	 */
	LWLockRelease(ProcArrayLock);

	if (in_recovery)
	{
		h->oldest_considered_running =
			TransactionIdOlder(h->oldest_considered_running, kaxmin);
		h->shared_oldest_nonremovable =
			TransactionIdOlder(h->shared_oldest_nonremovable, kaxmin);
		h->data_oldest_nonremovable =
			TransactionIdOlder(h->data_oldest_nonremovable, kaxmin);
		/* temp relations cannot be accessed in recovery */
	}

	Assert(TransactionIdPrecedesOrEquals(h->oldest_considered_running,
										 h->shared_oldest_nonremovable));
	Assert(TransactionIdPrecedesOrEquals(h->shared_oldest_nonremovable,
										 h->data_oldest_nonremovable));

	/*
	 * Check whether there are replication slots requiring an older xmin.
	 */
	h->shared_oldest_nonremovable =
		TransactionIdOlder(h->shared_oldest_nonremovable, h->slot_xmin);
	h->data_oldest_nonremovable =
		TransactionIdOlder(h->data_oldest_nonremovable, h->slot_xmin);

	/*
	 * The only difference between catalog / data horizons is that the slot's
	 * catalog xmin is applied to the catalog one (so catalogs can be accessed
	 * for logical decoding). Initialize with data horizon, and then back up
	 * further if necessary. Have to back up the shared horizon as well, since
	 * that also can contain catalogs.
	 */
	h->shared_oldest_nonremovable_raw = h->shared_oldest_nonremovable;
	h->shared_oldest_nonremovable =
		TransactionIdOlder(h->shared_oldest_nonremovable,
						   h->slot_catalog_xmin);
	h->catalog_oldest_nonremovable = h->data_oldest_nonremovable;
	h->catalog_oldest_nonremovable =
		TransactionIdOlder(h->catalog_oldest_nonremovable,
						   h->slot_catalog_xmin);

	/*
	 * It's possible that slots backed up the horizons further than
	 * oldest_considered_running. Fix.
	 */
	h->oldest_considered_running =
		TransactionIdOlder(h->oldest_considered_running,
						   h->shared_oldest_nonremovable);
	h->oldest_considered_running =
		TransactionIdOlder(h->oldest_considered_running,
						   h->catalog_oldest_nonremovable);
	h->oldest_considered_running =
		TransactionIdOlder(h->oldest_considered_running,
						   h->data_oldest_nonremovable);

	/*
	 * shared horizons have to be at least as old as the oldest visible in
	 * current db
	 */
	Assert(TransactionIdPrecedesOrEquals(h->shared_oldest_nonremovable,
										 h->data_oldest_nonremovable));
	Assert(TransactionIdPrecedesOrEquals(h->shared_oldest_nonremovable,
										 h->catalog_oldest_nonremovable));

	/*
	 * Horizons need to ensure that pg_subtrans access is still possible for
	 * the relevant backends.
	 */
	Assert(TransactionIdPrecedesOrEquals(h->oldest_considered_running,
										 h->shared_oldest_nonremovable));
	Assert(TransactionIdPrecedesOrEquals(h->oldest_considered_running,
										 h->catalog_oldest_nonremovable));
	Assert(TransactionIdPrecedesOrEquals(h->oldest_considered_running,
										 h->data_oldest_nonremovable));
	Assert(TransactionIdPrecedesOrEquals(h->oldest_considered_running,
										 h->temp_oldest_nonremovable));
	Assert(!TransactionIdIsValid(h->slot_xmin) ||
		   TransactionIdPrecedesOrEquals(h->oldest_considered_running,
										 h->slot_xmin));
	Assert(!TransactionIdIsValid(h->slot_catalog_xmin) ||
		   TransactionIdPrecedesOrEquals(h->oldest_considered_running,
										 h->slot_catalog_xmin));

	/* update approximate horizons with the computed horizons */
	GlobalVisUpdateApply(h);
}

/*
 * Determine what kind of visibility horizon needs to be used for a
 * relation. If rel is NULL, the most conservative horizon is used.
 */
static inline GlobalVisHorizonKind
GlobalVisHorizonKindForRel(Relation rel)
{
	/*
	 * Other relkinds currently don't contain xids, nor always the necessary
	 * logical decoding markers.
	 */
	Assert(!rel ||
		   rel->rd_rel->relkind == RELKIND_RELATION ||
		   rel->rd_rel->relkind == RELKIND_MATVIEW ||
		   rel->rd_rel->relkind == RELKIND_TOASTVALUE);

	if (rel == NULL || rel->rd_rel->relisshared || RecoveryInProgress())
		return VISHORIZON_SHARED;
	else if (IsCatalogRelation(rel) ||
			 RelationIsAccessibleInLogicalDecoding(rel))
		return VISHORIZON_CATALOG;
	else if (!RELATION_IS_LOCAL(rel))
		return VISHORIZON_DATA;
	else
		return VISHORIZON_TEMP;
}

/*
 * Return the oldest XID for which deleted tuples must be preserved in the
 * passed table.
 *
 * If rel is not NULL the horizon may be considerably more recent than
 * otherwise (i.e. fewer tuples will be removable). In the NULL case a horizon
 * that is correct (but not optimal) for all relations will be returned.
 *
 * This is used by VACUUM to decide which deleted tuples must be preserved in
 * the passed in table.
 */
TransactionId
GetOldestNonRemovableTransactionId(Relation rel)
{
	ComputeXidHorizonsResult horizons;

	ComputeXidHorizons(&horizons);

	switch (GlobalVisHorizonKindForRel(rel))
	{
		case VISHORIZON_SHARED:
			return horizons.shared_oldest_nonremovable;
		case VISHORIZON_CATALOG:
			return horizons.catalog_oldest_nonremovable;
		case VISHORIZON_DATA:
			return horizons.data_oldest_nonremovable;
		case VISHORIZON_TEMP:
			return horizons.temp_oldest_nonremovable;
	}

	/* just to prevent compiler warnings */
	return InvalidTransactionId;
}

/*
 * Return the oldest transaction id any currently running backend might still
 * consider running. This should not be used for visibility / pruning
 * determinations (see GetOldestNonRemovableTransactionId()), but for
 * decisions like up to where pg_subtrans can be truncated.
 */
TransactionId
GetOldestTransactionIdConsideredRunning(void)
{
	ComputeXidHorizonsResult horizons;

	ComputeXidHorizons(&horizons);

	return horizons.oldest_considered_running;
}

/*
 * Return the visibility horizons for a hot standby feedback message.
 */
void
GetReplicationHorizons(TransactionId *xmin, TransactionId *catalog_xmin)
{
	ComputeXidHorizonsResult horizons;

	ComputeXidHorizons(&horizons);

	/*
	 * Don't want to use shared_oldest_nonremovable here, as that contains the
	 * effect of replication slot's catalog_xmin. We want to send a separate
	 * feedback for the catalog horizon, so the primary can remove data table
	 * contents more aggressively.
	 */
	*xmin = horizons.shared_oldest_nonremovable_raw;
	*catalog_xmin = horizons.slot_catalog_xmin;
}

/*
 * GetMaxSnapshotXidCount -- get max size for snapshot XID array
 *
 * We have to export this for use by snapmgr.c.
 */
int
GetMaxSnapshotXidCount(void)
{
	return procArray->maxProcs;
}

/*
 * GetMaxSnapshotSubxidCount -- get max size for snapshot sub-XID array
 *
 * We have to export this for use by snapmgr.c.
 */
int
GetMaxSnapshotSubxidCount(void)
{
	return TOTAL_MAX_CACHED_SUBXIDS;
}

/*
 * Helper function for GetSnapshotData() that checks if the bulk of the
 * visibility information in the snapshot is still valid. If so, it updates
 * the fields that need to change and returns true. Otherwise it returns
 * false.
 *
 * This very likely can be evolved to not need ProcArrayLock held (at very
 * least in the case we already hold a snapshot), but that's for another day.
 */
static bool
GetSnapshotDataReuse(Snapshot snapshot)
{
	uint64		curXactCompletionCount;

	Assert(LWLockHeldByMe(ProcArrayLock));

	if (unlikely(snapshot->snapXactCompletionCount == 0))
		return false;

	curXactCompletionCount = TransamVariables->xactCompletionCount;
	if (curXactCompletionCount != snapshot->snapXactCompletionCount)
		return false;

	/*
	 * If the current xactCompletionCount is still the same as it was at the
	 * time the snapshot was built, we can be sure that rebuilding the
	 * contents of the snapshot the hard way would result in the same snapshot
	 * contents:
	 *
	 * As explained in transam/README, the set of xids considered running by
	 * GetSnapshotData() cannot change while ProcArrayLock is held. Snapshot
	 * contents only depend on transactions with xids and xactCompletionCount
	 * is incremented whenever a transaction with an xid finishes (while
	 * holding ProcArrayLock exclusively). Thus the xactCompletionCount check
	 * ensures we would detect if the snapshot would have changed.
	 *
	 * As the snapshot contents are the same as it was before, it is safe to
	 * re-enter the snapshot's xmin into the PGPROC array. None of the rows
	 * visible under the snapshot could already have been removed (that'd
	 * require the set of running transactions to change) and it fulfills the
	 * requirement that concurrent GetSnapshotData() calls yield the same
	 * xmin.
	 */
	if (!TransactionIdIsValid(MyProc->xmin))
		MyProc->xmin = TransactionXmin = snapshot->xmin;

	RecentXmin = snapshot->xmin;
	Assert(TransactionIdPrecedesOrEquals(TransactionXmin, RecentXmin));

	snapshot->curcid = GetCurrentCommandId(false);
	snapshot->active_count = 0;
	snapshot->regd_count = 0;
	snapshot->copied = false;

	return true;
}

/*
 * GetSnapshotData -- returns information about running transactions.
 *
 * The returned snapshot includes xmin (lowest still-running xact ID),
 * xmax (highest completed xact ID + 1), and a list of running xact IDs
 * in the range xmin <= xid < xmax.  It is used as follows:
 *		All xact IDs < xmin are considered finished.
 *		All xact IDs >= xmax are considered still running.
 *		For an xact ID xmin <= xid < xmax, consult list to see whether
 *		it is considered running or not.
 * This ensures that the set of transactions seen as "running" by the
 * current xact will not change after it takes the snapshot.
 *
 * All running top-level XIDs are included in the snapshot, except for lazy
 * VACUUM processes.  We also try to include running subtransaction XIDs,
 * but since PGPROC has only a limited cache area for subxact XIDs, full
 * information may not be available.  If we find any overflowed subxid arrays,
 * we have to mark the snapshot's subxid data as overflowed, and extra work
 * *may* need to be done to determine what's running (see XidInMVCCSnapshot()).
 *
 * We also update the following backend-global variables:
 *		TransactionXmin: the oldest xmin of any snapshot in use in the
 *			current transaction (this is the same as MyProc->xmin).
 *		RecentXmin: the xmin computed for the most recent snapshot.  XIDs
 *			older than this are known not running any more.
 *
 * And try to advance the bounds of GlobalVis{Shared,Catalog,Data,Temp}Rels
 * for the benefit of the GlobalVisTest* family of functions.
 *
 * Note: this function should probably not be called with an argument that's
 * not statically allocated (see xip allocation below).
 */
Snapshot
GetSnapshotData(Snapshot snapshot)
{
	ProcArrayStruct *arrayP = procArray;
	TransactionId *other_xids = ProcGlobal->xids;
	TransactionId xmin;
	TransactionId xmax;
	int			count = 0;
	int			subcount = 0;
	bool		suboverflowed = false;
	FullTransactionId latest_completed;
	TransactionId oldestxid;
	int			mypgxactoff;
	TransactionId myxid;
	uint64		curXactCompletionCount;

	TransactionId replication_slot_xmin = InvalidTransactionId;
	TransactionId replication_slot_catalog_xmin = InvalidTransactionId;

	Assert(snapshot != NULL);

	/*
	 * Allocating space for maxProcs xids is usually overkill; numProcs would
	 * be sufficient.  But it seems better to do the malloc while not holding
	 * the lock, so we can't look at numProcs.  Likewise, we allocate much
	 * more subxip storage than is probably needed.
	 *
	 * This does open a possibility for avoiding repeated malloc/free: since
	 * maxProcs does not change at runtime, we can simply reuse the previous
	 * xip arrays if any.  (This relies on the fact that all callers pass
	 * static SnapshotData structs.)
	 */
	if (snapshot->xip == NULL)
	{
		/*
		 * First call for this snapshot. Snapshot is same size whether or not
		 * we are in recovery, see later comments.
		 */
		snapshot->xip = (TransactionId *)
			malloc(GetMaxSnapshotXidCount() * sizeof(TransactionId));
		if (snapshot->xip == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		Assert(snapshot->subxip == NULL);
		snapshot->subxip = (TransactionId *)
			malloc(GetMaxSnapshotSubxidCount() * sizeof(TransactionId));
		if (snapshot->subxip == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
	}

	/*
	 * It is sufficient to get shared lock on ProcArrayLock, even if we are
	 * going to set MyProc->xmin.
	 */
	LWLockAcquire(ProcArrayLock, LW_SHARED);

	if (GetSnapshotDataReuse(snapshot))
	{
		LWLockRelease(ProcArrayLock);
		return snapshot;
	}

	latest_completed = TransamVariables->latestCompletedXid;
	mypgxactoff = MyProc->pgxactoff;
	myxid = other_xids[mypgxactoff];
	Assert(myxid == MyProc->xid);

	oldestxid = TransamVariables->oldestXid;
	curXactCompletionCount = TransamVariables->xactCompletionCount;

	/* xmax is always latestCompletedXid + 1 */
	xmax = XidFromFullTransactionId(latest_completed);
	TransactionIdAdvance(xmax);
	Assert(TransactionIdIsNormal(xmax));

	/* initialize xmin calculation with xmax */
	xmin = xmax;

	/* take own xid into account, saves a check inside the loop */
	if (TransactionIdIsNormal(myxid) && NormalTransactionIdPrecedes(myxid, xmin))
		xmin = myxid;

	snapshot->takenDuringRecovery = RecoveryInProgress();

	if (!snapshot->takenDuringRecovery)
	{
		int			numProcs = arrayP->numProcs;
		TransactionId *xip = snapshot->xip;
		int		   *pgprocnos = arrayP->pgprocnos;
		XidCacheStatus *subxidStates = ProcGlobal->subxidStates;
		uint8	   *allStatusFlags = ProcGlobal->statusFlags;

		/*
		 * First collect set of pgxactoff/xids that need to be included in the
		 * snapshot.
		 */
		for (int pgxactoff = 0; pgxactoff < numProcs; pgxactoff++)
		{
			/* Fetch xid just once - see GetNewTransactionId */
			TransactionId xid = UINT32_ACCESS_ONCE(other_xids[pgxactoff]);
			uint8		statusFlags;

			Assert(allProcs[arrayP->pgprocnos[pgxactoff]].pgxactoff == pgxactoff);

			/*
			 * If the transaction has no XID assigned, we can skip it; it
			 * won't have sub-XIDs either.
			 */
			if (likely(xid == InvalidTransactionId))
				continue;

			/*
			 * We don't include our own XIDs (if any) in the snapshot. It
			 * needs to be included in the xmin computation, but we did so
			 * outside the loop.
			 */
			if (pgxactoff == mypgxactoff)
				continue;

			/*
			 * The only way we are able to get here with a non-normal xid is
			 * during bootstrap - with this backend using
			 * BootstrapTransactionId. But the above test should filter that
			 * out.
			 */
			Assert(TransactionIdIsNormal(xid));

			/*
			 * If the XID is >= xmax, we can skip it; such transactions will
			 * be treated as running anyway (and any sub-XIDs will also be >=
			 * xmax).
			 */
			if (!NormalTransactionIdPrecedes(xid, xmax))
				continue;

			/*
			 * Skip over backends doing logical decoding which manages xmin
			 * separately (check below) and ones running LAZY VACUUM.
			 */
			statusFlags = allStatusFlags[pgxactoff];
			if (statusFlags & (PROC_IN_LOGICAL_DECODING | PROC_IN_VACUUM))
				continue;

			if (NormalTransactionIdPrecedes(xid, xmin))
				xmin = xid;

			/* Add XID to snapshot. */
			xip[count++] = xid;

			/*
			 * Save subtransaction XIDs if possible (if we've already
			 * overflowed, there's no point).  Note that the subxact XIDs must
			 * be later than their parent, so no need to check them against
			 * xmin.  We could filter against xmax, but it seems better not to
			 * do that much work while holding the ProcArrayLock.
			 *
			 * The other backend can add more subxids concurrently, but cannot
			 * remove any.  Hence it's important to fetch nxids just once.
			 * Should be safe to use memcpy, though.  (We needn't worry about
			 * missing any xids added concurrently, because they must postdate
			 * xmax.)
			 *
			 * Again, our own XIDs are not included in the snapshot.
			 */
			if (!suboverflowed)
			{

				if (subxidStates[pgxactoff].overflowed)
					suboverflowed = true;
				else
				{
					int			nsubxids = subxidStates[pgxactoff].count;

					if (nsubxids > 0)
					{
						int			pgprocno = pgprocnos[pgxactoff];
						PGPROC	   *proc = &allProcs[pgprocno];

						pg_read_barrier();	/* pairs with GetNewTransactionId */

						memcpy(snapshot->subxip + subcount,
							   proc->subxids.xids,
							   nsubxids * sizeof(TransactionId));
						subcount += nsubxids;
					}
				}
			}
		}
	}
	else
	{
		/*
		 * We're in hot standby, so get XIDs from KnownAssignedXids.
		 *
		 * We store all xids directly into subxip[]. Here's why:
		 *
		 * In recovery we don't know which xids are top-level and which are
		 * subxacts, a design choice that greatly simplifies xid processing.
		 *
		 * It seems like we would want to try to put xids into xip[] only, but
		 * that is fairly small. We would either need to make that bigger or
		 * to increase the rate at which we WAL-log xid assignment; neither is
		 * an appealing choice.
		 *
		 * We could try to store xids into xip[] first and then into subxip[]
		 * if there are too many xids. That only works if the snapshot doesn't
		 * overflow because we do not search subxip[] in that case. A simpler
		 * way is to just store all xids in the subxip array because this is
		 * by far the bigger array. We just leave the xip array empty.
		 *
		 * Either way we need to change the way XidInMVCCSnapshot() works
		 * depending upon when the snapshot was taken, or change normal
		 * snapshot processing so it matches.
		 *
		 * Note: It is possible for recovery to end before we finish taking
		 * the snapshot, and for newly assigned transaction ids to be added to
		 * the ProcArray.  xmax cannot change while we hold ProcArrayLock, so
		 * those newly added transaction ids would be filtered away, so we
		 * need not be concerned about them.
		 */
		subcount = KnownAssignedXidsGetAndSetXmin(snapshot->subxip, &xmin,
												  xmax);

		if (TransactionIdPrecedesOrEquals(xmin, procArray->lastOverflowedXid))
			suboverflowed = true;
	}


	/*
	 * Fetch into local variable while ProcArrayLock is held - the
	 * LWLockRelease below is a barrier, ensuring this happens inside the
	 * lock.
	 */
	replication_slot_xmin = procArray->replication_slot_xmin;
	replication_slot_catalog_xmin = procArray->replication_slot_catalog_xmin;

	if (!TransactionIdIsValid(MyProc->xmin))
		MyProc->xmin = TransactionXmin = xmin;

	LWLockRelease(ProcArrayLock);

	/* maintain state for GlobalVis* */
	{
		TransactionId def_vis_xid;
		TransactionId def_vis_xid_data;
		FullTransactionId def_vis_fxid;
		FullTransactionId def_vis_fxid_data;
		FullTransactionId oldestfxid;

		/*
		 * Converting oldestXid is only safe when xid horizon cannot advance,
		 * i.e. holding locks. While we don't hold the lock anymore, all the
		 * necessary data has been gathered with lock held.
		 */
		oldestfxid = FullXidRelativeTo(latest_completed, oldestxid);

		/* Check whether there's a replication slot requiring an older xmin. */
		def_vis_xid_data =
			TransactionIdOlder(xmin, replication_slot_xmin);

		/*
		 * Rows in non-shared, non-catalog tables possibly could be vacuumed
		 * if older than this xid.
		 */
		def_vis_xid = def_vis_xid_data;

		/*
		 * Check whether there's a replication slot requiring an older catalog
		 * xmin.
		 */
		def_vis_xid =
			TransactionIdOlder(replication_slot_catalog_xmin, def_vis_xid);

		def_vis_fxid = FullXidRelativeTo(latest_completed, def_vis_xid);
		def_vis_fxid_data = FullXidRelativeTo(latest_completed, def_vis_xid_data);

		/*
		 * Check if we can increase upper bound. As a previous
		 * GlobalVisUpdate() might have computed more aggressive values, don't
		 * overwrite them if so.
		 */
		GlobalVisSharedRels.definitely_needed =
			FullTransactionIdNewer(def_vis_fxid,
								   GlobalVisSharedRels.definitely_needed);
		GlobalVisCatalogRels.definitely_needed =
			FullTransactionIdNewer(def_vis_fxid,
								   GlobalVisCatalogRels.definitely_needed);
		GlobalVisDataRels.definitely_needed =
			FullTransactionIdNewer(def_vis_fxid_data,
								   GlobalVisDataRels.definitely_needed);
		/* See temp_oldest_nonremovable computation in ComputeXidHorizons() */
		if (TransactionIdIsNormal(myxid))
			GlobalVisTempRels.definitely_needed =
				FullXidRelativeTo(latest_completed, myxid);
		else
		{
			GlobalVisTempRels.definitely_needed = latest_completed;
			FullTransactionIdAdvance(&GlobalVisTempRels.definitely_needed);
		}

		/*
		 * Check if we know that we can initialize or increase the lower
		 * bound. Currently the only cheap way to do so is to use
		 * TransamVariables->oldestXid as input.
		 *
		 * We should definitely be able to do better. We could e.g. put a
		 * global lower bound value into TransamVariables.
		 */
		GlobalVisSharedRels.maybe_needed =
			FullTransactionIdNewer(GlobalVisSharedRels.maybe_needed,
								   oldestfxid);
		GlobalVisCatalogRels.maybe_needed =
			FullTransactionIdNewer(GlobalVisCatalogRels.maybe_needed,
								   oldestfxid);
		GlobalVisDataRels.maybe_needed =
			FullTransactionIdNewer(GlobalVisDataRels.maybe_needed,
								   oldestfxid);
		/* accurate value known */
		GlobalVisTempRels.maybe_needed = GlobalVisTempRels.definitely_needed;
	}

	RecentXmin = xmin;
	Assert(TransactionIdPrecedesOrEquals(TransactionXmin, RecentXmin));

	snapshot->xmin = xmin;
	snapshot->xmax = xmax;
	snapshot->xcnt = count;
	snapshot->subxcnt = subcount;
	snapshot->suboverflowed = suboverflowed;
	snapshot->snapXactCompletionCount = curXactCompletionCount;

	snapshot->curcid = GetCurrentCommandId(false);

	/*
	 * This is a new snapshot, so set both refcounts are zero, and mark it as
	 * not copied in persistent memory.
	 */
	snapshot->active_count = 0;
	snapshot->regd_count = 0;
	snapshot->copied = false;

	return snapshot;
}

/*
 * ProcArrayInstallImportedXmin -- install imported xmin into MyProc->xmin
 *
 * This is called when installing a snapshot imported from another
 * transaction.  To ensure that OldestXmin doesn't go backwards, we must
 * check that the source transaction is still running, and we'd better do
 * that atomically with installing the new xmin.
 *
 * Returns true if successful, false if source xact is no longer running.
 */
bool
ProcArrayInstallImportedXmin(TransactionId xmin,
							 VirtualTransactionId *sourcevxid)
{
	bool		result = false;
	ProcArrayStruct *arrayP = procArray;
	int			index;

	Assert(TransactionIdIsNormal(xmin));
	if (!sourcevxid)
		return false;

	/* Get lock so source xact can't end while we're doing this */
	LWLockAcquire(ProcArrayLock, LW_SHARED);

	/*
	 * Find the PGPROC entry of the source transaction. (This could use
	 * GetPGProcByNumber(), unless it's a prepared xact.  But this isn't
	 * performance critical.)
	 */
	for (index = 0; index < arrayP->numProcs; index++)
	{
		int			pgprocno = arrayP->pgprocnos[index];
		PGPROC	   *proc = &allProcs[pgprocno];
		int			statusFlags = ProcGlobal->statusFlags[index];
		TransactionId xid;

		/* Ignore procs running LAZY VACUUM */
		if (statusFlags & PROC_IN_VACUUM)
			continue;

		/* We are only interested in the specific virtual transaction. */
		if (proc->vxid.procNumber != sourcevxid->procNumber)
			continue;
		if (proc->vxid.lxid != sourcevxid->localTransactionId)
			continue;

		/*
		 * We check the transaction's database ID for paranoia's sake: if it's
		 * in another DB then its xmin does not cover us.  Caller should have
		 * detected this already, so we just treat any funny cases as
		 * "transaction not found".
		 */
		if (proc->databaseId != MyDatabaseId)
			continue;

		/*
		 * Likewise, let's just make real sure its xmin does cover us.
		 */
		xid = UINT32_ACCESS_ONCE(proc->xmin);
		if (!TransactionIdIsNormal(xid) ||
			!TransactionIdPrecedesOrEquals(xid, xmin))
			continue;

		/*
		 * We're good.  Install the new xmin.  As in GetSnapshotData, set
		 * TransactionXmin too.  (Note that because snapmgr.c called
		 * GetSnapshotData first, we'll be overwriting a valid xmin here, so
		 * we don't check that.)
		 */
		MyProc->xmin = TransactionXmin = xmin;

		result = true;
		break;
	}

	LWLockRelease(ProcArrayLock);

	return result;
}

/*
 * ProcArrayInstallRestoredXmin -- install restored xmin into MyProc->xmin
 *
 * This is like ProcArrayInstallImportedXmin, but we have a pointer to the
 * PGPROC of the transaction from which we imported the snapshot, rather than
 * an XID.
 *
 * Note that this function also copies statusFlags from the source `proc` in
 * order to avoid the case where MyProc's xmin needs to be skipped for
 * computing xid horizon.
 *
 * Returns true if successful, false if source xact is no longer running.
 */
bool
ProcArrayInstallRestoredXmin(TransactionId xmin, PGPROC *proc)
{
	bool		result = false;
	TransactionId xid;

	Assert(TransactionIdIsNormal(xmin));
	Assert(proc != NULL);

	/*
	 * Get an exclusive lock so that we can copy statusFlags from source proc.
	 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	/*
	 * Be certain that the referenced PGPROC has an advertised xmin which is
	 * no later than the one we're installing, so that the system-wide xmin
	 * can't go backwards.  Also, make sure it's running in the same database,
	 * so that the per-database xmin cannot go backwards.
	 */
	xid = UINT32_ACCESS_ONCE(proc->xmin);
	if (proc->databaseId == MyDatabaseId &&
		TransactionIdIsNormal(xid) &&
		TransactionIdPrecedesOrEquals(xid, xmin))
	{
		/*
		 * Install xmin and propagate the statusFlags that affect how the
		 * value is interpreted by vacuum.
		 */
		MyProc->xmin = TransactionXmin = xmin;
		MyProc->statusFlags = (MyProc->statusFlags & ~PROC_XMIN_FLAGS) |
			(proc->statusFlags & PROC_XMIN_FLAGS);
		ProcGlobal->statusFlags[MyProc->pgxactoff] = MyProc->statusFlags;

		result = true;
	}

	LWLockRelease(ProcArrayLock);

	return result;
}

/*
 * GetRunningTransactionData -- returns information about running transactions.
 *
 * Similar to GetSnapshotData but returns more information. We include
 * all PGPROCs with an assigned TransactionId, even VACUUM processes and
 * prepared transactions.
 *
 * We acquire XidGenLock and ProcArrayLock, but the caller is responsible for
 * releasing them. Acquiring XidGenLock ensures that no new XIDs enter the proc
 * array until the caller has WAL-logged this snapshot, and releases the
 * lock. Acquiring ProcArrayLock ensures that no transactions commit until the
 * lock is released.
 *
 * The returned data structure is statically allocated; caller should not
 * modify it, and must not assume it is valid past the next call.
 *
 * This is never executed during recovery so there is no need to look at
 * KnownAssignedXids.
 *
 * Dummy PGPROCs from prepared transaction are included, meaning that this
 * may return entries with duplicated TransactionId values coming from
 * transaction finishing to prepare.  Nothing is done about duplicated
 * entries here to not hold on ProcArrayLock more than necessary.
 *
 * We don't worry about updating other counters, we want to keep this as
 * simple as possible and leave GetSnapshotData() as the primary code for
 * that bookkeeping.
 *
 * Note that if any transaction has overflowed its cached subtransactions
 * then there is no real need include any subtransactions.
 */
RunningTransactions
GetRunningTransactionData(void)
{
	/* result workspace */
	static RunningTransactionsData CurrentRunningXactsData;

	ProcArrayStruct *arrayP = procArray;
	TransactionId *other_xids = ProcGlobal->xids;
	RunningTransactions CurrentRunningXacts = &CurrentRunningXactsData;
	TransactionId latestCompletedXid;
	TransactionId oldestRunningXid;
	TransactionId oldestDatabaseRunningXid;
	TransactionId *xids;
	int			index;
	int			count;
	int			subcount;
	bool		suboverflowed;

	Assert(!RecoveryInProgress());

	/*
	 * Allocating space for maxProcs xids is usually overkill; numProcs would
	 * be sufficient.  But it seems better to do the malloc while not holding
	 * the lock, so we can't look at numProcs.  Likewise, we allocate much
	 * more subxip storage than is probably needed.
	 *
	 * Should only be allocated in bgwriter, since only ever executed during
	 * checkpoints.
	 */
	if (CurrentRunningXacts->xids == NULL)
	{
		/*
		 * First call
		 */
		CurrentRunningXacts->xids = (TransactionId *)
			malloc(TOTAL_MAX_CACHED_SUBXIDS * sizeof(TransactionId));
		if (CurrentRunningXacts->xids == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
	}

	xids = CurrentRunningXacts->xids;

	count = subcount = 0;
	suboverflowed = false;

	/*
	 * Ensure that no xids enter or leave the procarray while we obtain
	 * snapshot.
	 */
	LWLockAcquire(ProcArrayLock, LW_SHARED);
	LWLockAcquire(XidGenLock, LW_SHARED);

	latestCompletedXid =
		XidFromFullTransactionId(TransamVariables->latestCompletedXid);
	oldestDatabaseRunningXid = oldestRunningXid =
		XidFromFullTransactionId(TransamVariables->nextXid);

	/*
	 * Spin over procArray collecting all xids
	 */
	for (index = 0; index < arrayP->numProcs; index++)
	{
		TransactionId xid;

		/* Fetch xid just once - see GetNewTransactionId */
		xid = UINT32_ACCESS_ONCE(other_xids[index]);

		/*
		 * We don't need to store transactions that don't have a TransactionId
		 * yet because they will not show as running on a standby server.
		 */
		if (!TransactionIdIsValid(xid))
			continue;

		/*
		 * Be careful not to exclude any xids before calculating the values of
		 * oldestRunningXid and suboverflowed, since these are used to clean
		 * up transaction information held on standbys.
		 */
		if (TransactionIdPrecedes(xid, oldestRunningXid))
			oldestRunningXid = xid;

		/*
		 * Also, update the oldest running xid within the current database. As
		 * fetching pgprocno and PGPROC could cause cache misses, we do cheap
		 * TransactionId comparison first.
		 */
		if (TransactionIdPrecedes(xid, oldestDatabaseRunningXid))
		{
			int			pgprocno = arrayP->pgprocnos[index];
			PGPROC	   *proc = &allProcs[pgprocno];

			if (proc->databaseId == MyDatabaseId)
				oldestDatabaseRunningXid = xid;
		}

		if (ProcGlobal->subxidStates[index].overflowed)
			suboverflowed = true;

		/*
		 * If we wished to exclude xids this would be the right place for it.
		 * Procs with the PROC_IN_VACUUM flag set don't usually assign xids,
		 * but they do during truncation at the end when they get the lock and
		 * truncate, so it is not much of a problem to include them if they
		 * are seen and it is cleaner to include them.
		 */

		xids[count++] = xid;
	}

	/*
	 * Spin over procArray collecting all subxids, but only if there hasn't
	 * been a suboverflow.
	 */
	if (!suboverflowed)
	{
		XidCacheStatus *other_subxidstates = ProcGlobal->subxidStates;

		for (index = 0; index < arrayP->numProcs; index++)
		{
			int			pgprocno = arrayP->pgprocnos[index];
			PGPROC	   *proc = &allProcs[pgprocno];
			int			nsubxids;

			/*
			 * Save subtransaction XIDs. Other backends can't add or remove
			 * entries while we're holding XidGenLock.
			 */
			nsubxids = other_subxidstates[index].count;
			if (nsubxids > 0)
			{
				/* barrier not really required, as XidGenLock is held, but ... */
				pg_read_barrier();	/* pairs with GetNewTransactionId */

				memcpy(&xids[count], proc->subxids.xids,
					   nsubxids * sizeof(TransactionId));
				count += nsubxids;
				subcount += nsubxids;

				/*
				 * Top-level XID of a transaction is always less than any of
				 * its subxids, so we don't need to check if any of the
				 * subxids are smaller than oldestRunningXid
				 */
			}
		}
	}

	/*
	 * It's important *not* to include the limits set by slots here because
	 * snapbuild.c uses oldestRunningXid to manage its xmin horizon. If those
	 * were to be included here the initial value could never increase because
	 * of a circular dependency where slots only increase their limits when
	 * running xacts increases oldestRunningXid and running xacts only
	 * increases if slots do.
	 */

	CurrentRunningXacts->xcnt = count - subcount;
	CurrentRunningXacts->subxcnt = subcount;
	CurrentRunningXacts->subxid_status = suboverflowed ? SUBXIDS_IN_SUBTRANS : SUBXIDS_IN_ARRAY;
	CurrentRunningXacts->nextXid = XidFromFullTransactionId(TransamVariables->nextXid);
	CurrentRunningXacts->oldestRunningXid = oldestRunningXid;
	CurrentRunningXacts->oldestDatabaseRunningXid = oldestDatabaseRunningXid;
	CurrentRunningXacts->latestCompletedXid = latestCompletedXid;

	Assert(TransactionIdIsValid(CurrentRunningXacts->nextXid));
	Assert(TransactionIdIsValid(CurrentRunningXacts->oldestRunningXid));
	Assert(TransactionIdIsNormal(CurrentRunningXacts->latestCompletedXid));

	/* We don't release the locks here, the caller is responsible for that */

	return CurrentRunningXacts;
}

/*
 * GetOldestActiveTransactionId()
 *
 * Similar to GetSnapshotData but returns just oldestActiveXid. We include
 * all PGPROCs with an assigned TransactionId, even VACUUM processes.
 * We look at all databases, though there is no need to include WALSender
 * since this has no effect on hot standby conflicts.
 *
 * This is never executed during recovery so there is no need to look at
 * KnownAssignedXids.
 *
 * We don't worry about updating other counters, we want to keep this as
 * simple as possible and leave GetSnapshotData() as the primary code for
 * that bookkeeping.
 */
TransactionId
GetOldestActiveTransactionId(void)
{
	ProcArrayStruct *arrayP = procArray;
	TransactionId *other_xids = ProcGlobal->xids;
	TransactionId oldestRunningXid;
	int			index;

	Assert(!RecoveryInProgress());

	/*
	 * Read nextXid, as the upper bound of what's still active.
	 *
	 * Reading a TransactionId is atomic, but we must grab the lock to make
	 * sure that all XIDs < nextXid are already present in the proc array (or
	 * have already completed), when we spin over it.
	 */
	LWLockAcquire(XidGenLock, LW_SHARED);
	oldestRunningXid = XidFromFullTransactionId(TransamVariables->nextXid);
	LWLockRelease(XidGenLock);

	/*
	 * Spin over procArray collecting all xids and subxids.
	 */
	LWLockAcquire(ProcArrayLock, LW_SHARED);
	for (index = 0; index < arrayP->numProcs; index++)
	{
		TransactionId xid;

		/* Fetch xid just once - see GetNewTransactionId */
		xid = UINT32_ACCESS_ONCE(other_xids[index]);

		if (!TransactionIdIsNormal(xid))
			continue;

		if (TransactionIdPrecedes(xid, oldestRunningXid))
			oldestRunningXid = xid;

		/*
		 * Top-level XID of a transaction is always less than any of its
		 * subxids, so we don't need to check if any of the subxids are
		 * smaller than oldestRunningXid
		 */
	}
	LWLockRelease(ProcArrayLock);

	return oldestRunningXid;
}

/*
 * GetOldestSafeDecodingTransactionId -- lowest xid not affected by vacuum
 *
 * Returns the oldest xid that we can guarantee not to have been affected by
 * vacuum, i.e. no rows >= that xid have been vacuumed away unless the
 * transaction aborted. Note that the value can (and most of the time will) be
 * much more conservative than what really has been affected by vacuum, but we
 * currently don't have better data available.
 *
 * This is useful to initialize the cutoff xid after which a new changeset
 * extraction replication slot can start decoding changes.
 *
 * Must be called with ProcArrayLock held either shared or exclusively,
 * although most callers will want to use exclusive mode since it is expected
 * that the caller will immediately use the xid to peg the xmin horizon.
 */
TransactionId
GetOldestSafeDecodingTransactionId(bool catalogOnly)
{
	ProcArrayStruct *arrayP = procArray;
	TransactionId oldestSafeXid;
	int			index;
	bool		recovery_in_progress = RecoveryInProgress();

	Assert(LWLockHeldByMe(ProcArrayLock));

	/*
	 * Acquire XidGenLock, so no transactions can acquire an xid while we're
	 * running. If no transaction with xid were running concurrently a new xid
	 * could influence the RecentXmin et al.
	 *
	 * We initialize the computation to nextXid since that's guaranteed to be
	 * a safe, albeit pessimal, value.
	 */
	LWLockAcquire(XidGenLock, LW_SHARED);
	oldestSafeXid = XidFromFullTransactionId(TransamVariables->nextXid);

	/*
	 * If there's already a slot pegging the xmin horizon, we can start with
	 * that value, it's guaranteed to be safe since it's computed by this
	 * routine initially and has been enforced since.  We can always use the
	 * slot's general xmin horizon, but the catalog horizon is only usable
	 * when only catalog data is going to be looked at.
	 */
	if (TransactionIdIsValid(procArray->replication_slot_xmin) &&
		TransactionIdPrecedes(procArray->replication_slot_xmin,
							  oldestSafeXid))
		oldestSafeXid = procArray->replication_slot_xmin;

	if (catalogOnly &&
		TransactionIdIsValid(procArray->replication_slot_catalog_xmin) &&
		TransactionIdPrecedes(procArray->replication_slot_catalog_xmin,
							  oldestSafeXid))
		oldestSafeXid = procArray->replication_slot_catalog_xmin;

	/*
	 * If we're not in recovery, we walk over the procarray and collect the
	 * lowest xid. Since we're called with ProcArrayLock held and have
	 * acquired XidGenLock, no entries can vanish concurrently, since
	 * ProcGlobal->xids[i] is only set with XidGenLock held and only cleared
	 * with ProcArrayLock held.
	 *
	 * In recovery we can't lower the safe value besides what we've computed
	 * above, so we'll have to wait a bit longer there. We unfortunately can
	 * *not* use KnownAssignedXidsGetOldestXmin() since the KnownAssignedXids
	 * machinery can miss values and return an older value than is safe.
	 */
	if (!recovery_in_progress)
	{
		TransactionId *other_xids = ProcGlobal->xids;

		/*
		 * Spin over procArray collecting min(ProcGlobal->xids[i])
		 */
		for (index = 0; index < arrayP->numProcs; index++)
		{
			TransactionId xid;

			/* Fetch xid just once - see GetNewTransactionId */
			xid = UINT32_ACCESS_ONCE(other_xids[index]);

			if (!TransactionIdIsNormal(xid))
				continue;

			if (TransactionIdPrecedes(xid, oldestSafeXid))
				oldestSafeXid = xid;
		}
	}

	LWLockRelease(XidGenLock);

	return oldestSafeXid;
}

/*
 * GetVirtualXIDsDelayingChkpt -- Get the VXIDs of transactions that are
 * delaying checkpoint because they have critical actions in progress.
 *
 * Constructs an array of VXIDs of transactions that are currently in commit
 * critical sections, as shown by having specified delayChkptFlags bits set
 * in their PGPROC.
 *
 * Returns a palloc'd array that should be freed by the caller.
 * *nvxids is the number of valid entries.
 *
 * Note that because backends set or clear delayChkptFlags without holding any
 * lock, the result is somewhat indeterminate, but we don't really care.  Even
 * in a multiprocessor with delayed writes to shared memory, it should be
 * certain that setting of delayChkptFlags will propagate to shared memory
 * when the backend takes a lock, so we cannot fail to see a virtual xact as
 * delayChkptFlags if it's already inserted its commit record.  Whether it
 * takes a little while for clearing of delayChkptFlags to propagate is
 * unimportant for correctness.
 */
VirtualTransactionId *
GetVirtualXIDsDelayingChkpt(int *nvxids, int type)
{
	VirtualTransactionId *vxids;
	ProcArrayStruct *arrayP = procArray;
	int			count = 0;
	int			index;

	Assert(type != 0);

	/* allocate what's certainly enough result space */
	vxids = (VirtualTransactionId *)
		palloc(sizeof(VirtualTransactionId) * arrayP->maxProcs);

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		int			pgprocno = arrayP->pgprocnos[index];
		PGPROC	   *proc = &allProcs[pgprocno];

		if ((proc->delayChkptFlags & type) != 0)
		{
			VirtualTransactionId vxid;

			GET_VXID_FROM_PGPROC(vxid, *proc);
			if (VirtualTransactionIdIsValid(vxid))
				vxids[count++] = vxid;
		}
	}

	LWLockRelease(ProcArrayLock);

	*nvxids = count;
	return vxids;
}

/*
 * HaveVirtualXIDsDelayingChkpt -- Are any of the specified VXIDs delaying?
 *
 * This is used with the results of GetVirtualXIDsDelayingChkpt to see if any
 * of the specified VXIDs are still in critical sections of code.
 *
 * Note: this is O(N^2) in the number of vxacts that are/were delaying, but
 * those numbers should be small enough for it not to be a problem.
 */
bool
HaveVirtualXIDsDelayingChkpt(VirtualTransactionId *vxids, int nvxids, int type)
{
	bool		result = false;
	ProcArrayStruct *arrayP = procArray;
	int			index;

	Assert(type != 0);

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		int			pgprocno = arrayP->pgprocnos[index];
		PGPROC	   *proc = &allProcs[pgprocno];
		VirtualTransactionId vxid;

		GET_VXID_FROM_PGPROC(vxid, *proc);

		if ((proc->delayChkptFlags & type) != 0 &&
			VirtualTransactionIdIsValid(vxid))
		{
			int			i;

			for (i = 0; i < nvxids; i++)
			{
				if (VirtualTransactionIdEquals(vxid, vxids[i]))
				{
					result = true;
					break;
				}
			}
			if (result)
				break;
		}
	}

	LWLockRelease(ProcArrayLock);

	return result;
}

/*
 * ProcNumberGetProc -- get a backend's PGPROC given its proc number
 *
 * The result may be out of date arbitrarily quickly, so the caller
 * must be careful about how this information is used.  NULL is
 * returned if the backend is not active.
 */
PGPROC *
ProcNumberGetProc(ProcNumber procNumber)
{
	PGPROC	   *result;

	if (procNumber < 0 || procNumber >= ProcGlobal->allProcCount)
		return NULL;
	result = GetPGProcByNumber(procNumber);

	if (result->pid == 0)
		return NULL;

	return result;
}

/*
 * ProcNumberGetTransactionIds -- get a backend's transaction status
 *
 * Get the xid, xmin, nsubxid and overflow status of the backend.  The
 * result may be out of date arbitrarily quickly, so the caller must be
 * careful about how this information is used.
 */
void
ProcNumberGetTransactionIds(ProcNumber procNumber, TransactionId *xid,
							TransactionId *xmin, int *nsubxid, bool *overflowed)
{
	PGPROC	   *proc;

	*xid = InvalidTransactionId;
	*xmin = InvalidTransactionId;
	*nsubxid = 0;
	*overflowed = false;

	if (procNumber < 0 || procNumber >= ProcGlobal->allProcCount)
		return;
	proc = GetPGProcByNumber(procNumber);

	/* Need to lock out additions/removals of backends */
	LWLockAcquire(ProcArrayLock, LW_SHARED);

	if (proc->pid != 0)
	{
		*xid = proc->xid;
		*xmin = proc->xmin;
		*nsubxid = proc->subxidStatus.count;
		*overflowed = proc->subxidStatus.overflowed;
	}

	LWLockRelease(ProcArrayLock);
}

/*
 * BackendPidGetProc -- get a backend's PGPROC given its PID
 *
 * Returns NULL if not found.  Note that it is up to the caller to be
 * sure that the question remains meaningful for long enough for the
 * answer to be used ...
 */
PGPROC *
BackendPidGetProc(int pid)
{
	PGPROC	   *result;

	if (pid == 0)				/* never match dummy PGPROCs */
		return NULL;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	result = BackendPidGetProcWithLock(pid);

	LWLockRelease(ProcArrayLock);

	return result;
}

/*
 * BackendPidGetProcWithLock -- get a backend's PGPROC given its PID
 *
 * Same as above, except caller must be holding ProcArrayLock.  The found
 * entry, if any, can be assumed to be valid as long as the lock remains held.
 */
PGPROC *
BackendPidGetProcWithLock(int pid)
{
	PGPROC	   *result = NULL;
	ProcArrayStruct *arrayP = procArray;
	int			index;

	if (pid == 0)				/* never match dummy PGPROCs */
		return NULL;

	for (index = 0; index < arrayP->numProcs; index++)
	{
		PGPROC	   *proc = &allProcs[arrayP->pgprocnos[index]];

		if (proc->pid == pid)
		{
			result = proc;
			break;
		}
	}

	return result;
}

/*
 * BackendXidGetPid -- get a backend's pid given its XID
 *
 * Returns 0 if not found or it's a prepared transaction.  Note that
 * it is up to the caller to be sure that the question remains
 * meaningful for long enough for the answer to be used ...
 *
 * Only main transaction Ids are considered.  This function is mainly
 * useful for determining what backend owns a lock.
 *
 * Beware that not every xact has an XID assigned.  However, as long as you
 * only call this using an XID found on disk, you're safe.
 */
int
BackendXidGetPid(TransactionId xid)
{
	int			result = 0;
	ProcArrayStruct *arrayP = procArray;
	TransactionId *other_xids = ProcGlobal->xids;
	int			index;

	if (xid == InvalidTransactionId)	/* never match invalid xid */
		return 0;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		if (other_xids[index] == xid)
		{
			int			pgprocno = arrayP->pgprocnos[index];
			PGPROC	   *proc = &allProcs[pgprocno];

			result = proc->pid;
			break;
		}
	}

	LWLockRelease(ProcArrayLock);

	return result;
}

/*
 * IsBackendPid -- is a given pid a running backend
 *
 * This is not called by the backend, but is called by external modules.
 */
bool
IsBackendPid(int pid)
{
	return (BackendPidGetProc(pid) != NULL);
}


/*
 * GetCurrentVirtualXIDs -- returns an array of currently active VXIDs.
 *
 * The array is palloc'd. The number of valid entries is returned into *nvxids.
 *
 * The arguments allow filtering the set of VXIDs returned.  Our own process
 * is always skipped.  In addition:
 *	If limitXmin is not InvalidTransactionId, skip processes with
 *		xmin > limitXmin.
 *	If excludeXmin0 is true, skip processes with xmin = 0.
 *	If allDbs is false, skip processes attached to other databases.
 *	If excludeVacuum isn't zero, skip processes for which
 *		(statusFlags & excludeVacuum) is not zero.
 *
 * Note: the purpose of the limitXmin and excludeXmin0 parameters is to
 * allow skipping backends whose oldest live snapshot is no older than
 * some snapshot we have.  Since we examine the procarray with only shared
 * lock, there are race conditions: a backend could set its xmin just after
 * we look.  Indeed, on multiprocessors with weak memory ordering, the
 * other backend could have set its xmin *before* we look.  We know however
 * that such a backend must have held shared ProcArrayLock overlapping our
 * own hold of ProcArrayLock, else we would see its xmin update.  Therefore,
 * any snapshot the other backend is taking concurrently with our scan cannot
 * consider any transactions as still running that we think are committed
 * (since backends must hold ProcArrayLock exclusive to commit).
 */
VirtualTransactionId *
GetCurrentVirtualXIDs(TransactionId limitXmin, bool excludeXmin0,
					  bool allDbs, int excludeVacuum,
					  int *nvxids)
{
	VirtualTransactionId *vxids;
	ProcArrayStruct *arrayP = procArray;
	int			count = 0;
	int			index;

	/* allocate what's certainly enough result space */
	vxids = (VirtualTransactionId *)
		palloc(sizeof(VirtualTransactionId) * arrayP->maxProcs);

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		int			pgprocno = arrayP->pgprocnos[index];
		PGPROC	   *proc = &allProcs[pgprocno];
		uint8		statusFlags = ProcGlobal->statusFlags[index];

		if (proc == MyProc)
			continue;

		if (excludeVacuum & statusFlags)
			continue;

		if (allDbs || proc->databaseId == MyDatabaseId)
		{
			/* Fetch xmin just once - might change on us */
			TransactionId pxmin = UINT32_ACCESS_ONCE(proc->xmin);

			if (excludeXmin0 && !TransactionIdIsValid(pxmin))
				continue;

			/*
			 * InvalidTransactionId precedes all other XIDs, so a proc that
			 * hasn't set xmin yet will not be rejected by this test.
			 */
			if (!TransactionIdIsValid(limitXmin) ||
				TransactionIdPrecedesOrEquals(pxmin, limitXmin))
			{
				VirtualTransactionId vxid;

				GET_VXID_FROM_PGPROC(vxid, *proc);
				if (VirtualTransactionIdIsValid(vxid))
					vxids[count++] = vxid;
			}
		}
	}

	LWLockRelease(ProcArrayLock);

	*nvxids = count;
	return vxids;
}

/*
 * GetConflictingVirtualXIDs -- returns an array of currently active VXIDs.
 *
 * Usage is limited to conflict resolution during recovery on standby servers.
 * limitXmin is supplied as either a cutoff with snapshotConflictHorizon
 * semantics, or InvalidTransactionId in cases where caller cannot accurately
 * determine a safe snapshotConflictHorizon value.
 *
 * If limitXmin is InvalidTransactionId then we want to kill everybody,
 * so we're not worried if they have a snapshot or not, nor does it really
 * matter what type of lock we hold.  Caller must avoid calling here with
 * snapshotConflictHorizon style cutoffs that were set to InvalidTransactionId
 * during original execution, since that actually indicates that there is
 * definitely no need for a recovery conflict (the snapshotConflictHorizon
 * convention for InvalidTransactionId values is the opposite of our own!).
 *
 * All callers that are checking xmins always now supply a valid and useful
 * value for limitXmin. The limitXmin is always lower than the lowest
 * numbered KnownAssignedXid that is not already a FATAL error. This is
 * because we only care about cleanup records that are cleaning up tuple
 * versions from committed transactions. In that case they will only occur
 * at the point where the record is less than the lowest running xid. That
 * allows us to say that if any backend takes a snapshot concurrently with
 * us then the conflict assessment made here would never include the snapshot
 * that is being derived. So we take LW_SHARED on the ProcArray and allow
 * concurrent snapshots when limitXmin is valid. We might think about adding
 *	 Assert(limitXmin < lowest(KnownAssignedXids))
 * but that would not be true in the case of FATAL errors lagging in array,
 * but we already know those are bogus anyway, so we skip that test.
 *
 * If dbOid is valid we skip backends attached to other databases.
 *
 * Be careful to *not* pfree the result from this function. We reuse
 * this array sufficiently often that we use malloc for the result.
 */
VirtualTransactionId *
GetConflictingVirtualXIDs(TransactionId limitXmin, Oid dbOid)
{
	static VirtualTransactionId *vxids;
	ProcArrayStruct *arrayP = procArray;
	int			count = 0;
	int			index;

	/*
	 * If first time through, get workspace to remember main XIDs in. We
	 * malloc it permanently to avoid repeated palloc/pfree overhead. Allow
	 * result space, remembering room for a terminator.
	 */
	if (vxids == NULL)
	{
		vxids = (VirtualTransactionId *)
			malloc(sizeof(VirtualTransactionId) * (arrayP->maxProcs + 1));
		if (vxids == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
	}

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		int			pgprocno = arrayP->pgprocnos[index];
		PGPROC	   *proc = &allProcs[pgprocno];

		/* Exclude prepared transactions */
		if (proc->pid == 0)
			continue;

		if (!OidIsValid(dbOid) ||
			proc->databaseId == dbOid)
		{
			/* Fetch xmin just once - can't change on us, but good coding */
			TransactionId pxmin = UINT32_ACCESS_ONCE(proc->xmin);

			/*
			 * We ignore an invalid pxmin because this means that backend has
			 * no snapshot currently. We hold a Share lock to avoid contention
			 * with users taking snapshots.  That is not a problem because the
			 * current xmin is always at least one higher than the latest
			 * removed xid, so any new snapshot would never conflict with the
			 * test here.
			 */
			if (!TransactionIdIsValid(limitXmin) ||
				(TransactionIdIsValid(pxmin) && !TransactionIdFollows(pxmin, limitXmin)))
			{
				VirtualTransactionId vxid;

				GET_VXID_FROM_PGPROC(vxid, *proc);
				if (VirtualTransactionIdIsValid(vxid))
					vxids[count++] = vxid;
			}
		}
	}

	LWLockRelease(ProcArrayLock);

	/* add the terminator */
	vxids[count].procNumber = INVALID_PROC_NUMBER;
	vxids[count].localTransactionId = InvalidLocalTransactionId;

	return vxids;
}

/*
 * CancelVirtualTransaction - used in recovery conflict processing
 *
 * Returns pid of the process signaled, or 0 if not found.
 */
pid_t
CancelVirtualTransaction(VirtualTransactionId vxid, ProcSignalReason sigmode)
{
	return SignalVirtualTransaction(vxid, sigmode, true);
}

pid_t
SignalVirtualTransaction(VirtualTransactionId vxid, ProcSignalReason sigmode,
						 bool conflictPending)
{
	ProcArrayStruct *arrayP = procArray;
	int			index;
	pid_t		pid = 0;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		int			pgprocno = arrayP->pgprocnos[index];
		PGPROC	   *proc = &allProcs[pgprocno];
		VirtualTransactionId procvxid;

		GET_VXID_FROM_PGPROC(procvxid, *proc);

		if (procvxid.procNumber == vxid.procNumber &&
			procvxid.localTransactionId == vxid.localTransactionId)
		{
			proc->recoveryConflictPending = conflictPending;
			pid = proc->pid;
			if (pid != 0)
			{
				/*
				 * Kill the pid if it's still here. If not, that's what we
				 * wanted so ignore any errors.
				 */
				(void) SendProcSignal(pid, sigmode, vxid.procNumber);
			}
			break;
		}
	}

	LWLockRelease(ProcArrayLock);

	return pid;
}

/*
 * MinimumActiveBackends --- count backends (other than myself) that are
 *		in active transactions.  Return true if the count exceeds the
 *		minimum threshold passed.  This is used as a heuristic to decide if
 *		a pre-XLOG-flush delay is worthwhile during commit.
 *
 * Do not count backends that are blocked waiting for locks, since they are
 * not going to get to run until someone else commits.
 */
bool
MinimumActiveBackends(int min)
{
	ProcArrayStruct *arrayP = procArray;
	int			count = 0;
	int			index;

	/* Quick short-circuit if no minimum is specified */
	if (min == 0)
		return true;

	/*
	 * Note: for speed, we don't acquire ProcArrayLock.  This is a little bit
	 * bogus, but since we are only testing fields for zero or nonzero, it
	 * should be OK.  The result is only used for heuristic purposes anyway...
	 */
	for (index = 0; index < arrayP->numProcs; index++)
	{
		int			pgprocno = arrayP->pgprocnos[index];
		PGPROC	   *proc = &allProcs[pgprocno];

		/*
		 * Since we're not holding a lock, need to be prepared to deal with
		 * garbage, as someone could have incremented numProcs but not yet
		 * filled the structure.
		 *
		 * If someone just decremented numProcs, 'proc' could also point to a
		 * PGPROC entry that's no longer in the array. It still points to a
		 * PGPROC struct, though, because freed PGPROC entries just go to the
		 * free list and are recycled. Its contents are nonsense in that case,
		 * but that's acceptable for this function.
		 */
		if (pgprocno == -1)
			continue;			/* do not count deleted entries */
		if (proc == MyProc)
			continue;			/* do not count myself */
		if (proc->xid == InvalidTransactionId)
			continue;			/* do not count if no XID assigned */
		if (proc->pid == 0)
			continue;			/* do not count prepared xacts */
		if (proc->waitLock != NULL)
			continue;			/* do not count if blocked on a lock */
		count++;
		if (count >= min)
			break;
	}

	return count >= min;
}

/*
 * CountDBBackends --- count backends that are using specified database
 */
int
CountDBBackends(Oid databaseid)
{
	ProcArrayStruct *arrayP = procArray;
	int			count = 0;
	int			index;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		int			pgprocno = arrayP->pgprocnos[index];
		PGPROC	   *proc = &allProcs[pgprocno];

		if (proc->pid == 0)
			continue;			/* do not count prepared xacts */
		if (!OidIsValid(databaseid) ||
			proc->databaseId == databaseid)
			count++;
	}

	LWLockRelease(ProcArrayLock);

	return count;
}

/*
 * CountDBConnections --- counts database backends ignoring any background
 *		worker processes
 */
int
CountDBConnections(Oid databaseid)
{
	ProcArrayStruct *arrayP = procArray;
	int			count = 0;
	int			index;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		int			pgprocno = arrayP->pgprocnos[index];
		PGPROC	   *proc = &allProcs[pgprocno];

		if (proc->pid == 0)
			continue;			/* do not count prepared xacts */
		if (proc->isBackgroundWorker)
			continue;			/* do not count background workers */
		if (!OidIsValid(databaseid) ||
			proc->databaseId == databaseid)
			count++;
	}

	LWLockRelease(ProcArrayLock);

	return count;
}

/*
 * CancelDBBackends --- cancel backends that are using specified database
 */
void
CancelDBBackends(Oid databaseid, ProcSignalReason sigmode, bool conflictPending)
{
	ProcArrayStruct *arrayP = procArray;
	int			index;

	/* tell all backends to die */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		int			pgprocno = arrayP->pgprocnos[index];
		PGPROC	   *proc = &allProcs[pgprocno];

		if (databaseid == InvalidOid || proc->databaseId == databaseid)
		{
			VirtualTransactionId procvxid;
			pid_t		pid;

			GET_VXID_FROM_PGPROC(procvxid, *proc);

			proc->recoveryConflictPending = conflictPending;
			pid = proc->pid;
			if (pid != 0)
			{
				/*
				 * Kill the pid if it's still here. If not, that's what we
				 * wanted so ignore any errors.
				 */
				(void) SendProcSignal(pid, sigmode, procvxid.procNumber);
			}
		}
	}

	LWLockRelease(ProcArrayLock);
}

/*
 * CountUserBackends --- count backends that are used by specified user
 */
int
CountUserBackends(Oid roleid)
{
	ProcArrayStruct *arrayP = procArray;
	int			count = 0;
	int			index;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		int			pgprocno = arrayP->pgprocnos[index];
		PGPROC	   *proc = &allProcs[pgprocno];

		if (proc->pid == 0)
			continue;			/* do not count prepared xacts */
		if (proc->isBackgroundWorker)
			continue;			/* do not count background workers */
		if (proc->roleId == roleid)
			count++;
	}

	LWLockRelease(ProcArrayLock);

	return count;
}

/*
 * CountOtherDBBackends -- check for other backends running in the given DB
 *
 * If there are other backends in the DB, we will wait a maximum of 5 seconds
 * for them to exit.  Autovacuum backends are encouraged to exit early by
 * sending them SIGTERM, but normal user backends are just waited for.
 *
 * The current backend is always ignored; it is caller's responsibility to
 * check whether the current backend uses the given DB, if it's important.
 *
 * Returns true if there are (still) other backends in the DB, false if not.
 * Also, *nbackends and *nprepared are set to the number of other backends
 * and prepared transactions in the DB, respectively.
 *
 * This function is used to interlock DROP DATABASE and related commands
 * against there being any active backends in the target DB --- dropping the
 * DB while active backends remain would be a Bad Thing.  Note that we cannot
 * detect here the possibility of a newly-started backend that is trying to
 * connect to the doomed database, so additional interlocking is needed during
 * backend startup.  The caller should normally hold an exclusive lock on the
 * target DB before calling this, which is one reason we mustn't wait
 * indefinitely.
 */
bool
CountOtherDBBackends(Oid databaseId, int *nbackends, int *nprepared)
{
	ProcArrayStruct *arrayP = procArray;

#define MAXAUTOVACPIDS	10		/* max autovacs to SIGTERM per iteration */
	int			autovac_pids[MAXAUTOVACPIDS];
	int			tries;

	/* 50 tries with 100ms sleep between tries makes 5 sec total wait */
	for (tries = 0; tries < 50; tries++)
	{
		int			nautovacs = 0;
		bool		found = false;
		int			index;

		CHECK_FOR_INTERRUPTS();

		*nbackends = *nprepared = 0;

		LWLockAcquire(ProcArrayLock, LW_SHARED);

		for (index = 0; index < arrayP->numProcs; index++)
		{
			int			pgprocno = arrayP->pgprocnos[index];
			PGPROC	   *proc = &allProcs[pgprocno];
			uint8		statusFlags = ProcGlobal->statusFlags[index];

			if (proc->databaseId != databaseId)
				continue;
			if (proc == MyProc)
				continue;

			found = true;

			if (proc->pid == 0)
				(*nprepared)++;
			else
			{
				(*nbackends)++;
				if ((statusFlags & PROC_IS_AUTOVACUUM) &&
					nautovacs < MAXAUTOVACPIDS)
					autovac_pids[nautovacs++] = proc->pid;
			}
		}

		LWLockRelease(ProcArrayLock);

		if (!found)
			return false;		/* no conflicting backends, so done */

		/*
		 * Send SIGTERM to any conflicting autovacuums before sleeping. We
		 * postpone this step until after the loop because we don't want to
		 * hold ProcArrayLock while issuing kill(). We have no idea what might
		 * block kill() inside the kernel...
		 */
		for (index = 0; index < nautovacs; index++)
			(void) kill(autovac_pids[index], SIGTERM);	/* ignore any error */

		/* sleep, then try again */
		pg_usleep(100 * 1000L); /* 100ms */
	}

	return true;				/* timed out, still conflicts */
}

/*
 * Terminate existing connections to the specified database. This routine
 * is used by the DROP DATABASE command when user has asked to forcefully
 * drop the database.
 *
 * The current backend is always ignored; it is caller's responsibility to
 * check whether the current backend uses the given DB, if it's important.
 *
 * If the target database has a prepared transaction or permissions checks
 * fail for a connection, this fails without terminating anything.
 */
void
TerminateOtherDBBackends(Oid databaseId)
{
	ProcArrayStruct *arrayP = procArray;
	List	   *pids = NIL;
	int			nprepared = 0;
	int			i;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (i = 0; i < procArray->numProcs; i++)
	{
		int			pgprocno = arrayP->pgprocnos[i];
		PGPROC	   *proc = &allProcs[pgprocno];

		if (proc->databaseId != databaseId)
			continue;
		if (proc == MyProc)
			continue;

		if (proc->pid != 0)
			pids = lappend_int(pids, proc->pid);
		else
			nprepared++;
	}

	LWLockRelease(ProcArrayLock);

	if (nprepared > 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("database \"%s\" is being used by prepared transactions",
						get_database_name(databaseId)),
				 errdetail_plural("There is %d prepared transaction using the database.",
								  "There are %d prepared transactions using the database.",
								  nprepared,
								  nprepared)));

	if (pids)
	{
		ListCell   *lc;

		/*
		 * Permissions checks relax the pg_terminate_backend checks in two
		 * ways, both by omitting the !OidIsValid(proc->roleId) check:
		 *
		 * - Accept terminating autovacuum workers, since DROP DATABASE
		 * without FORCE terminates them.
		 *
		 * - Accept terminating bgworkers.  For bgworker authors, it's
		 * convenient to be able to recommend FORCE if a worker is blocking
		 * DROP DATABASE unexpectedly.
		 *
		 * Unlike pg_terminate_backend, we don't raise some warnings - like
		 * "PID %d is not a PostgreSQL server process", because for us already
		 * finished session is not a problem.
		 */
		foreach(lc, pids)
		{
			int			pid = lfirst_int(lc);
			PGPROC	   *proc = BackendPidGetProc(pid);

			if (proc != NULL)
			{
				if (superuser_arg(proc->roleId) && !superuser())
					ereport(ERROR,
							(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
							 errmsg("permission denied to terminate process"),
							 errdetail("Only roles with the %s attribute may terminate processes of roles with the %s attribute.",
									   "SUPERUSER", "SUPERUSER")));

				if (!has_privs_of_role(GetUserId(), proc->roleId) &&
					!has_privs_of_role(GetUserId(), ROLE_PG_SIGNAL_BACKEND))
					ereport(ERROR,
							(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
							 errmsg("permission denied to terminate process"),
							 errdetail("Only roles with privileges of the role whose process is being terminated or with privileges of the \"%s\" role may terminate this process.",
									   "pg_signal_backend")));
			}
		}

		/*
		 * There's a race condition here: once we release the ProcArrayLock,
		 * it's possible for the session to exit before we issue kill.  That
		 * race condition possibility seems too unlikely to worry about.  See
		 * pg_signal_backend.
		 */
		foreach(lc, pids)
		{
			int			pid = lfirst_int(lc);
			PGPROC	   *proc = BackendPidGetProc(pid);

			if (proc != NULL)
			{
				/*
				 * If we have setsid(), signal the backend's whole process
				 * group
				 */
#ifdef HAVE_SETSID
				(void) kill(-pid, SIGTERM);
#else
				(void) kill(pid, SIGTERM);
#endif
			}
		}
	}
}

/*
 * ProcArraySetReplicationSlotXmin
 *
 * Install limits to future computations of the xmin horizon to prevent vacuum
 * and HOT pruning from removing affected rows still needed by clients with
 * replication slots.
 */
void
ProcArraySetReplicationSlotXmin(TransactionId xmin, TransactionId catalog_xmin,
								bool already_locked)
{
	Assert(!already_locked || LWLockHeldByMe(ProcArrayLock));

	if (!already_locked)
		LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	procArray->replication_slot_xmin = xmin;
	procArray->replication_slot_catalog_xmin = catalog_xmin;

	if (!already_locked)
		LWLockRelease(ProcArrayLock);

	elog(DEBUG1, "xmin required by slots: data %u, catalog %u",
		 xmin, catalog_xmin);
}

/*
 * ProcArrayGetReplicationSlotXmin
 *
 * Return the current slot xmin limits. That's useful to be able to remove
 * data that's older than those limits.
 */
void
ProcArrayGetReplicationSlotXmin(TransactionId *xmin,
								TransactionId *catalog_xmin)
{
	LWLockAcquire(ProcArrayLock, LW_SHARED);

	if (xmin != NULL)
		*xmin = procArray->replication_slot_xmin;

	if (catalog_xmin != NULL)
		*catalog_xmin = procArray->replication_slot_catalog_xmin;

	LWLockRelease(ProcArrayLock);
}

/*
 * XidCacheRemoveRunningXids
 *
 * Remove a bunch of TransactionIds from the list of known-running
 * subtransactions for my backend.  Both the specified xid and those in
 * the xids[] array (of length nxids) are removed from the subxids cache.
 * latestXid must be the latest XID among the group.
 */
void
XidCacheRemoveRunningXids(TransactionId xid,
						  int nxids, const TransactionId *xids,
						  TransactionId latestXid)
{
	int			i,
				j;
	XidCacheStatus *mysubxidstat;

	Assert(TransactionIdIsValid(xid));

	/*
	 * We must hold ProcArrayLock exclusively in order to remove transactions
	 * from the PGPROC array.  (See src/backend/access/transam/README.)  It's
	 * possible this could be relaxed since we know this routine is only used
	 * to abort subtransactions, but pending closer analysis we'd best be
	 * conservative.
	 *
	 * Note that we do not have to be careful about memory ordering of our own
	 * reads wrt. GetNewTransactionId() here - only this process can modify
	 * relevant fields of MyProc/ProcGlobal->xids[].  But we do have to be
	 * careful about our own writes being well ordered.
	 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	mysubxidstat = &ProcGlobal->subxidStates[MyProc->pgxactoff];

	/*
	 * Under normal circumstances xid and xids[] will be in increasing order,
	 * as will be the entries in subxids.  Scan backwards to avoid O(N^2)
	 * behavior when removing a lot of xids.
	 */
	for (i = nxids - 1; i >= 0; i--)
	{
		TransactionId anxid = xids[i];

		for (j = MyProc->subxidStatus.count - 1; j >= 0; j--)
		{
			if (TransactionIdEquals(MyProc->subxids.xids[j], anxid))
			{
				MyProc->subxids.xids[j] = MyProc->subxids.xids[MyProc->subxidStatus.count - 1];
				pg_write_barrier();
				mysubxidstat->count--;
				MyProc->subxidStatus.count--;
				break;
			}
		}

		/*
		 * Ordinarily we should have found it, unless the cache has
		 * overflowed. However it's also possible for this routine to be
		 * invoked multiple times for the same subtransaction, in case of an
		 * error during AbortSubTransaction.  So instead of Assert, emit a
		 * debug warning.
		 */
		if (j < 0 && !MyProc->subxidStatus.overflowed)
			elog(WARNING, "did not find subXID %u in MyProc", anxid);
	}

	for (j = MyProc->subxidStatus.count - 1; j >= 0; j--)
	{
		if (TransactionIdEquals(MyProc->subxids.xids[j], xid))
		{
			MyProc->subxids.xids[j] = MyProc->subxids.xids[MyProc->subxidStatus.count - 1];
			pg_write_barrier();
			mysubxidstat->count--;
			MyProc->subxidStatus.count--;
			break;
		}
	}
	/* Ordinarily we should have found it, unless the cache has overflowed */
	if (j < 0 && !MyProc->subxidStatus.overflowed)
		elog(WARNING, "did not find subXID %u in MyProc", xid);

	/* Also advance global latestCompletedXid while holding the lock */
	MaintainLatestCompletedXid(latestXid);

	/* ... and xactCompletionCount */
	TransamVariables->xactCompletionCount++;

	LWLockRelease(ProcArrayLock);
}

#ifdef XIDCACHE_DEBUG

/*
 * Print stats about effectiveness of XID cache
 */
static void
DisplayXidCache(void)
{
	fprintf(stderr,
			"XidCache: xmin: %ld, known: %ld, myxact: %ld, latest: %ld, mainxid: %ld, childxid: %ld, knownassigned: %ld, nooflo: %ld, slow: %ld\n",
			xc_by_recent_xmin,
			xc_by_known_xact,
			xc_by_my_xact,
			xc_by_latest_xid,
			xc_by_main_xid,
			xc_by_child_xid,
			xc_by_known_assigned,
			xc_no_overflow,
			xc_slow_answer);
}
#endif							/* XIDCACHE_DEBUG */

/*
 * If rel != NULL, return test state appropriate for relation, otherwise
 * return state usable for all relations.  The latter may consider XIDs as
 * not-yet-visible-to-everyone that a state for a specific relation would
 * already consider visible-to-everyone.
 *
 * This needs to be called while a snapshot is active or registered, otherwise
 * there are wraparound and other dangers.
 *
 * See comment for GlobalVisState for details.
 */
GlobalVisState *
GlobalVisTestFor(Relation rel)
{
	GlobalVisState *state = NULL;

	/* XXX: we should assert that a snapshot is pushed or registered */
	Assert(RecentXmin);

	switch (GlobalVisHorizonKindForRel(rel))
	{
		case VISHORIZON_SHARED:
			state = &GlobalVisSharedRels;
			break;
		case VISHORIZON_CATALOG:
			state = &GlobalVisCatalogRels;
			break;
		case VISHORIZON_DATA:
			state = &GlobalVisDataRels;
			break;
		case VISHORIZON_TEMP:
			state = &GlobalVisTempRels;
			break;
	}

	Assert(FullTransactionIdIsValid(state->definitely_needed) &&
		   FullTransactionIdIsValid(state->maybe_needed));

	return state;
}

/*
 * Return true if it's worth updating the accurate maybe_needed boundary.
 *
 * As it is somewhat expensive to determine xmin horizons, we don't want to
 * repeatedly do so when there is a low likelihood of it being beneficial.
 *
 * The current heuristic is that we update only if RecentXmin has changed
 * since the last update. If the oldest currently running transaction has not
 * finished, it is unlikely that recomputing the horizon would be useful.
 */
static bool
GlobalVisTestShouldUpdate(GlobalVisState *state)
{
	/* hasn't been updated yet */
	if (!TransactionIdIsValid(ComputeXidHorizonsResultLastXmin))
		return true;

	/*
	 * If the maybe_needed/definitely_needed boundaries are the same, it's
	 * unlikely to be beneficial to refresh boundaries.
	 */
	if (FullTransactionIdFollowsOrEquals(state->maybe_needed,
										 state->definitely_needed))
		return false;

	/* does the last snapshot built have a different xmin? */
	return RecentXmin != ComputeXidHorizonsResultLastXmin;
}

static void
GlobalVisUpdateApply(ComputeXidHorizonsResult *horizons)
{
	GlobalVisSharedRels.maybe_needed =
		FullXidRelativeTo(horizons->latest_completed,
						  horizons->shared_oldest_nonremovable);
	GlobalVisCatalogRels.maybe_needed =
		FullXidRelativeTo(horizons->latest_completed,
						  horizons->catalog_oldest_nonremovable);
	GlobalVisDataRels.maybe_needed =
		FullXidRelativeTo(horizons->latest_completed,
						  horizons->data_oldest_nonremovable);
	GlobalVisTempRels.maybe_needed =
		FullXidRelativeTo(horizons->latest_completed,
						  horizons->temp_oldest_nonremovable);

	/*
	 * In longer running transactions it's possible that transactions we
	 * previously needed to treat as running aren't around anymore. So update
	 * definitely_needed to not be earlier than maybe_needed.
	 */
	GlobalVisSharedRels.definitely_needed =
		FullTransactionIdNewer(GlobalVisSharedRels.maybe_needed,
							   GlobalVisSharedRels.definitely_needed);
	GlobalVisCatalogRels.definitely_needed =
		FullTransactionIdNewer(GlobalVisCatalogRels.maybe_needed,
							   GlobalVisCatalogRels.definitely_needed);
	GlobalVisDataRels.definitely_needed =
		FullTransactionIdNewer(GlobalVisDataRels.maybe_needed,
							   GlobalVisDataRels.definitely_needed);
	GlobalVisTempRels.definitely_needed = GlobalVisTempRels.maybe_needed;

	ComputeXidHorizonsResultLastXmin = RecentXmin;
}

/*
 * Update boundaries in GlobalVis{Shared,Catalog, Data}Rels
 * using ComputeXidHorizons().
 */
static void
GlobalVisUpdate(void)
{
	ComputeXidHorizonsResult horizons;

	/* updates the horizons as a side-effect */
	ComputeXidHorizons(&horizons);
}

/*
 * Return true if no snapshot still considers fxid to be running.
 *
 * The state passed needs to have been initialized for the relation fxid is
 * from (NULL is also OK), otherwise the result may not be correct.
 *
 * See comment for GlobalVisState for details.
 */
bool
GlobalVisTestIsRemovableFullXid(GlobalVisState *state,
								FullTransactionId fxid)
{
	/*
	 * If fxid is older than maybe_needed bound, it definitely is visible to
	 * everyone.
	 */
	if (FullTransactionIdPrecedes(fxid, state->maybe_needed))
		return true;

	/*
	 * If fxid is >= definitely_needed bound, it is very likely to still be
	 * considered running.
	 */
	if (FullTransactionIdFollowsOrEquals(fxid, state->definitely_needed))
		return false;

	/*
	 * fxid is between maybe_needed and definitely_needed, i.e. there might or
	 * might not exist a snapshot considering fxid running. If it makes sense,
	 * update boundaries and recheck.
	 */
	if (GlobalVisTestShouldUpdate(state))
	{
		GlobalVisUpdate();

		Assert(FullTransactionIdPrecedes(fxid, state->definitely_needed));

		return FullTransactionIdPrecedes(fxid, state->maybe_needed);
	}
	else
		return false;
}

/*
 * Wrapper around GlobalVisTestIsRemovableFullXid() for 32bit xids.
 *
 * It is crucial that this only gets called for xids from a source that
 * protects against xid wraparounds (e.g. from a table and thus protected by
 * relfrozenxid).
 */
bool
GlobalVisTestIsRemovableXid(GlobalVisState *state, TransactionId xid)
{
	FullTransactionId fxid;

	/*
	 * Convert 32 bit argument to FullTransactionId. We can do so safely
	 * because we know the xid has to, at the very least, be between
	 * [oldestXid, nextXid), i.e. within 2 billion of xid. To avoid taking a
	 * lock to determine either, we can just compare with
	 * state->definitely_needed, which was based on those value at the time
	 * the current snapshot was built.
	 */
	fxid = FullXidRelativeTo(state->definitely_needed, xid);

	return GlobalVisTestIsRemovableFullXid(state, fxid);
}

/*
 * Convenience wrapper around GlobalVisTestFor() and
 * GlobalVisTestIsRemovableFullXid(), see their comments.
 */
bool
GlobalVisCheckRemovableFullXid(Relation rel, FullTransactionId fxid)
{
	GlobalVisState *state;

	state = GlobalVisTestFor(rel);

	return GlobalVisTestIsRemovableFullXid(state, fxid);
}

/*
 * Convenience wrapper around GlobalVisTestFor() and
 * GlobalVisTestIsRemovableXid(), see their comments.
 */
bool
GlobalVisCheckRemovableXid(Relation rel, TransactionId xid)
{
	GlobalVisState *state;

	state = GlobalVisTestFor(rel);

	return GlobalVisTestIsRemovableXid(state, xid);
}

/*
 * Convert a 32 bit transaction id into 64 bit transaction id, by assuming it
 * is within MaxTransactionId / 2 of XidFromFullTransactionId(rel).
 *
 * Be very careful about when to use this function. It can only safely be used
 * when there is a guarantee that xid is within MaxTransactionId / 2 xids of
 * rel. That e.g. can be guaranteed if the caller assures a snapshot is
 * held by the backend and xid is from a table (where vacuum/freezing ensures
 * the xid has to be within that range), or if xid is from the procarray and
 * prevents xid wraparound that way.
 */
static inline FullTransactionId
FullXidRelativeTo(FullTransactionId rel, TransactionId xid)
{
	TransactionId rel_xid = XidFromFullTransactionId(rel);

	Assert(TransactionIdIsValid(xid));
	Assert(TransactionIdIsValid(rel_xid));

	/* not guaranteed to find issues, but likely to catch mistakes */
	AssertTransactionIdInAllowableRange(xid);

	return FullTransactionIdFromU64(U64FromFullTransactionId(rel)
									+ (int32) (xid - rel_xid));
}


/* ----------------------------------------------
 *		KnownAssignedTransactionIds sub-module
 * ----------------------------------------------
 */

/*
 * In Hot Standby mode, we maintain a list of transactions that are (or were)
 * running on the primary at the current point in WAL.  These XIDs must be
 * treated as running by standby transactions, even though they are not in
 * the standby server's PGPROC array.
 *
 * We record all XIDs that we know have been assigned.  That includes all the
 * XIDs seen in WAL records, plus all unobserved XIDs that we can deduce have
 * been assigned.  We can deduce the existence of unobserved XIDs because we
 * know XIDs are assigned in sequence, with no gaps.  The KnownAssignedXids
 * list expands as new XIDs are observed or inferred, and contracts when
 * transaction completion records arrive.
 *
 * During hot standby we do not fret too much about the distinction between
 * top-level XIDs and subtransaction XIDs. We store both together in the
 * KnownAssignedXids list.  In backends, this is copied into snapshots in
 * GetSnapshotData(), taking advantage of the fact that XidInMVCCSnapshot()
 * doesn't care about the distinction either.  Subtransaction XIDs are
 * effectively treated as top-level XIDs and in the typical case pg_subtrans
 * links are *not* maintained (which does not affect visibility).
 *
 * We have room in KnownAssignedXids and in snapshots to hold maxProcs *
 * (1 + PGPROC_MAX_CACHED_SUBXIDS) XIDs, so every primary transaction must
 * report its subtransaction XIDs in a WAL XLOG_XACT_ASSIGNMENT record at
 * least every PGPROC_MAX_CACHED_SUBXIDS.  When we receive one of these
 * records, we mark the subXIDs as children of the top XID in pg_subtrans,
 * and then remove them from KnownAssignedXids.  This prevents overflow of
 * KnownAssignedXids and snapshots, at the cost that status checks for these
 * subXIDs will take a slower path through TransactionIdIsInProgress().
 * This means that KnownAssignedXids is not necessarily complete for subXIDs,
 * though it should be complete for top-level XIDs; this is the same situation
 * that holds with respect to the PGPROC entries in normal running.
 *
 * When we throw away subXIDs from KnownAssignedXids, we need to keep track of
 * that, similarly to tracking overflow of a PGPROC's subxids array.  We do
 * that by remembering the lastOverflowedXid, ie the last thrown-away subXID.
 * As long as that is within the range of interesting XIDs, we have to assume
 * that subXIDs are missing from snapshots.  (Note that subXID overflow occurs
 * on primary when 65th subXID arrives, whereas on standby it occurs when 64th
 * subXID arrives - that is not an error.)
 *
 * Should a backend on primary somehow disappear before it can write an abort
 * record, then we just leave those XIDs in KnownAssignedXids. They actually
 * aborted but we think they were running; the distinction is irrelevant
 * because either way any changes done by the transaction are not visible to
 * backends in the standby.  We prune KnownAssignedXids when
 * XLOG_RUNNING_XACTS arrives, to forestall possible overflow of the
 * array due to such dead XIDs.
 */

/*
 * RecordKnownAssignedTransactionIds
 *		Record the given XID in KnownAssignedXids, as well as any preceding
 *		unobserved XIDs.
 *
 * RecordKnownAssignedTransactionIds() should be run for *every* WAL record
 * associated with a transaction. Must be called for each record after we
 * have executed StartupCLOG() et al, since we must ExtendCLOG() etc..
 *
 * Called during recovery in analogy with and in place of GetNewTransactionId()
 */
void
RecordKnownAssignedTransactionIds(TransactionId xid)
{
	Assert(standbyState >= STANDBY_INITIALIZED);
	Assert(TransactionIdIsValid(xid));
	Assert(TransactionIdIsValid(latestObservedXid));

	elog(DEBUG4, "record known xact %u latestObservedXid %u",
		 xid, latestObservedXid);

	/*
	 * When a newly observed xid arrives, it is frequently the case that it is
	 * *not* the next xid in sequence. When this occurs, we must treat the
	 * intervening xids as running also.
	 */
	if (TransactionIdFollows(xid, latestObservedXid))
	{
		TransactionId next_expected_xid;

		/*
		 * Extend subtrans like we do in GetNewTransactionId() during normal
		 * operation using individual extend steps. Note that we do not need
		 * to extend clog since its extensions are WAL logged.
		 *
		 * This part has to be done regardless of standbyState since we
		 * immediately start assigning subtransactions to their toplevel
		 * transactions.
		 */
		next_expected_xid = latestObservedXid;
		while (TransactionIdPrecedes(next_expected_xid, xid))
		{
			TransactionIdAdvance(next_expected_xid);
			ExtendSUBTRANS(next_expected_xid);
		}
		Assert(next_expected_xid == xid);

		/*
		 * If the KnownAssignedXids machinery isn't up yet, there's nothing
		 * more to do since we don't track assigned xids yet.
		 */
		if (standbyState <= STANDBY_INITIALIZED)
		{
			latestObservedXid = xid;
			return;
		}

		/*
		 * Add (latestObservedXid, xid] onto the KnownAssignedXids array.
		 */
		next_expected_xid = latestObservedXid;
		TransactionIdAdvance(next_expected_xid);
		KnownAssignedXidsAdd(next_expected_xid, xid, false);

		/*
		 * Now we can advance latestObservedXid
		 */
		latestObservedXid = xid;

		/* TransamVariables->nextXid must be beyond any observed xid */
		AdvanceNextFullTransactionIdPastXid(latestObservedXid);
	}
}

/*
 * ExpireTreeKnownAssignedTransactionIds
 *		Remove the given XIDs from KnownAssignedXids.
 *
 * Called during recovery in analogy with and in place of ProcArrayEndTransaction()
 */
void
ExpireTreeKnownAssignedTransactionIds(TransactionId xid, int nsubxids,
									  TransactionId *subxids, TransactionId max_xid)
{
	Assert(standbyState >= STANDBY_INITIALIZED);

	/*
	 * Uses same locking as transaction commit
	 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	KnownAssignedXidsRemoveTree(xid, nsubxids, subxids);

	/* As in ProcArrayEndTransaction, advance latestCompletedXid */
	MaintainLatestCompletedXidRecovery(max_xid);

	/* ... and xactCompletionCount */
	TransamVariables->xactCompletionCount++;

	LWLockRelease(ProcArrayLock);
}

/*
 * ExpireAllKnownAssignedTransactionIds
 *		Remove all entries in KnownAssignedXids and reset lastOverflowedXid.
 */
void
ExpireAllKnownAssignedTransactionIds(void)
{
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
	KnownAssignedXidsRemovePreceding(InvalidTransactionId);

	/*
	 * Reset lastOverflowedXid.  Currently, lastOverflowedXid has no use after
	 * the call of this function.  But do this for unification with what
	 * ExpireOldKnownAssignedTransactionIds() do.
	 */
	procArray->lastOverflowedXid = InvalidTransactionId;
	LWLockRelease(ProcArrayLock);
}

/*
 * ExpireOldKnownAssignedTransactionIds
 *		Remove KnownAssignedXids entries preceding the given XID and
 *		potentially reset lastOverflowedXid.
 */
void
ExpireOldKnownAssignedTransactionIds(TransactionId xid)
{
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	/*
	 * Reset lastOverflowedXid if we know all transactions that have been
	 * possibly running are being gone.  Not doing so could cause an incorrect
	 * lastOverflowedXid value, which makes extra snapshots be marked as
	 * suboverflowed.
	 */
	if (TransactionIdPrecedes(procArray->lastOverflowedXid, xid))
		procArray->lastOverflowedXid = InvalidTransactionId;
	KnownAssignedXidsRemovePreceding(xid);
	LWLockRelease(ProcArrayLock);
}

/*
 * KnownAssignedTransactionIdsIdleMaintenance
 *		Opportunistically do maintenance work when the startup process
 *		is about to go idle.
 */
void
KnownAssignedTransactionIdsIdleMaintenance(void)
{
	KnownAssignedXidsCompress(KAX_STARTUP_PROCESS_IDLE, false);
}


/*
 * Private module functions to manipulate KnownAssignedXids
 *
 * There are 5 main uses of the KnownAssignedXids data structure:
 *
 *	* backends taking snapshots - all valid XIDs need to be copied out
 *	* backends seeking to determine presence of a specific XID
 *	* startup process adding new known-assigned XIDs
 *	* startup process removing specific XIDs as transactions end
 *	* startup process pruning array when special WAL records arrive
 *
 * This data structure is known to be a hot spot during Hot Standby, so we
 * go to some lengths to make these operations as efficient and as concurrent
 * as possible.
 *
 * The XIDs are stored in an array in sorted order --- TransactionIdPrecedes
 * order, to be exact --- to allow binary search for specific XIDs.  Note:
 * in general TransactionIdPrecedes would not provide a total order, but
 * we know that the entries present at any instant should not extend across
 * a large enough fraction of XID space to wrap around (the primary would
 * shut down for fear of XID wrap long before that happens).  So it's OK to
 * use TransactionIdPrecedes as a binary-search comparator.
 *
 * It's cheap to maintain the sortedness during insertions, since new known
 * XIDs are always reported in XID order; we just append them at the right.
 *
 * To keep individual deletions cheap, we need to allow gaps in the array.
 * This is implemented by marking array elements as valid or invalid using
 * the parallel boolean array KnownAssignedXidsValid[].  A deletion is done
 * by setting KnownAssignedXidsValid[i] to false, *without* clearing the
 * XID entry itself.  This preserves the property that the XID entries are
 * sorted, so we can do binary searches easily.  Periodically we compress
 * out the unused entries; that's much cheaper than having to compress the
 * array immediately on every deletion.
 *
 * The actually valid items in KnownAssignedXids[] and KnownAssignedXidsValid[]
 * are those with indexes tail <= i < head; items outside this subscript range
 * have unspecified contents.  When head reaches the end of the array, we
 * force compression of unused entries rather than wrapping around, since
 * allowing wraparound would greatly complicate the search logic.  We maintain
 * an explicit tail pointer so that pruning of old XIDs can be done without
 * immediately moving the array contents.  In most cases only a small fraction
 * of the array contains valid entries at any instant.
 *
 * Although only the startup process can ever change the KnownAssignedXids
 * data structure, we still need interlocking so that standby backends will
 * not observe invalid intermediate states.  The convention is that backends
 * must hold shared ProcArrayLock to examine the array.  To remove XIDs from
 * the array, the startup process must hold ProcArrayLock exclusively, for
 * the usual transactional reasons (compare commit/abort of a transaction
 * during normal running).  Compressing unused entries out of the array
 * likewise requires exclusive lock.  To add XIDs to the array, we just insert
 * them into slots to the right of the head pointer and then advance the head
 * pointer.  This doesn't require any lock at all, but on machines with weak
 * memory ordering, we need to be careful that other processors see the array
 * element changes before they see the head pointer change.  We handle this by
 * using memory barriers when reading or writing the head/tail pointers (unless
 * the caller holds ProcArrayLock exclusively).
 *
 * Algorithmic analysis:
 *
 * If we have a maximum of M slots, with N XIDs currently spread across
 * S elements then we have N <= S <= M always.
 *
 *	* Adding a new XID is O(1) and needs no lock (unless compression must
 *		happen)
 *	* Compressing the array is O(S) and requires exclusive lock
 *	* Removing an XID is O(logS) and requires exclusive lock
 *	* Taking a snapshot is O(S) and requires shared lock
 *	* Checking for an XID is O(logS) and requires shared lock
 *
 * In comparison, using a hash table for KnownAssignedXids would mean that
 * taking snapshots would be O(M). If we can maintain S << M then the
 * sorted array technique will deliver significantly faster snapshots.
 * If we try to keep S too small then we will spend too much time compressing,
 * so there is an optimal point for any workload mix. We use a heuristic to
 * decide when to compress the array, though trimming also helps reduce
 * frequency of compressing. The heuristic requires us to track the number of
 * currently valid XIDs in the array (N).  Except in special cases, we'll
 * compress when S >= 2N.  Bounding S at 2N in turn bounds the time for
 * taking a snapshot to be O(N), which it would have to be anyway.
 */


/*
 * Compress KnownAssignedXids by shifting valid data down to the start of the
 * array, removing any gaps.
 *
 * A compression step is forced if "reason" is KAX_NO_SPACE, otherwise
 * we do it only if a heuristic indicates it's a good time to do it.
 *
 * Compression requires holding ProcArrayLock in exclusive mode.
 * Caller must pass haveLock = true if it already holds the lock.
 */
static void
KnownAssignedXidsCompress(KAXCompressReason reason, bool haveLock)
{
	ProcArrayStruct *pArray = procArray;
	int			head,
				tail,
				nelements;
	int			compress_index;
	int			i;

	/* Counters for compression heuristics */
	static unsigned int transactionEndsCounter;
	static TimestampTz lastCompressTs;

	/* Tuning constants */
#define KAX_COMPRESS_FREQUENCY 128	/* in transactions */
#define KAX_COMPRESS_IDLE_INTERVAL 1000 /* in ms */

	/*
	 * Since only the startup process modifies the head/tail pointers, we
	 * don't need a lock to read them here.
	 */
	head = pArray->headKnownAssignedXids;
	tail = pArray->tailKnownAssignedXids;
	nelements = head - tail;

	/*
	 * If we can choose whether to compress, use a heuristic to avoid
	 * compressing too often or not often enough.  "Compress" here simply
	 * means moving the values to the beginning of the array, so it is not as
	 * complex or costly as typical data compression algorithms.
	 */
	if (nelements == pArray->numKnownAssignedXids)
	{
		/*
		 * When there are no gaps between head and tail, don't bother to
		 * compress, except in the KAX_NO_SPACE case where we must compress to
		 * create some space after the head.
		 */
		if (reason != KAX_NO_SPACE)
			return;
	}
	else if (reason == KAX_TRANSACTION_END)
	{
		/*
		 * Consider compressing only once every so many commits.  Frequency
		 * determined by benchmarks.
		 */
		if ((transactionEndsCounter++) % KAX_COMPRESS_FREQUENCY != 0)
			return;

		/*
		 * Furthermore, compress only if the used part of the array is less
		 * than 50% full (see comments above).
		 */
		if (nelements < 2 * pArray->numKnownAssignedXids)
			return;
	}
	else if (reason == KAX_STARTUP_PROCESS_IDLE)
	{
		/*
		 * We're about to go idle for lack of new WAL, so we might as well
		 * compress.  But not too often, to avoid ProcArray lock contention
		 * with readers.
		 */
		if (lastCompressTs != 0)
		{
			TimestampTz compress_after;

			compress_after = TimestampTzPlusMilliseconds(lastCompressTs,
														 KAX_COMPRESS_IDLE_INTERVAL);
			if (GetCurrentTimestamp() < compress_after)
				return;
		}
	}

	/* Need to compress, so get the lock if we don't have it. */
	if (!haveLock)
		LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	/*
	 * We compress the array by reading the valid values from tail to head,
	 * re-aligning data to 0th element.
	 */
	compress_index = 0;
	for (i = tail; i < head; i++)
	{
		if (KnownAssignedXidsValid[i])
		{
			KnownAssignedXids[compress_index] = KnownAssignedXids[i];
			KnownAssignedXidsValid[compress_index] = true;
			compress_index++;
		}
	}
	Assert(compress_index == pArray->numKnownAssignedXids);

	pArray->tailKnownAssignedXids = 0;
	pArray->headKnownAssignedXids = compress_index;

	if (!haveLock)
		LWLockRelease(ProcArrayLock);

	/* Update timestamp for maintenance.  No need to hold lock for this. */
	lastCompressTs = GetCurrentTimestamp();
}

/*
 * Add xids into KnownAssignedXids at the head of the array.
 *
 * xids from from_xid to to_xid, inclusive, are added to the array.
 *
 * If exclusive_lock is true then caller already holds ProcArrayLock in
 * exclusive mode, so we need no extra locking here.  Else caller holds no
 * lock, so we need to be sure we maintain sufficient interlocks against
 * concurrent readers.  (Only the startup process ever calls this, so no need
 * to worry about concurrent writers.)
 */
static void
KnownAssignedXidsAdd(TransactionId from_xid, TransactionId to_xid,
					 bool exclusive_lock)
{
	ProcArrayStruct *pArray = procArray;
	TransactionId next_xid;
	int			head,
				tail;
	int			nxids;
	int			i;

	Assert(TransactionIdPrecedesOrEquals(from_xid, to_xid));

	/*
	 * Calculate how many array slots we'll need.  Normally this is cheap; in
	 * the unusual case where the XIDs cross the wrap point, we do it the hard
	 * way.
	 */
	if (to_xid >= from_xid)
		nxids = to_xid - from_xid + 1;
	else
	{
		nxids = 1;
		next_xid = from_xid;
		while (TransactionIdPrecedes(next_xid, to_xid))
		{
			nxids++;
			TransactionIdAdvance(next_xid);
		}
	}

	/*
	 * Since only the startup process modifies the head/tail pointers, we
	 * don't need a lock to read them here.
	 */
	head = pArray->headKnownAssignedXids;
	tail = pArray->tailKnownAssignedXids;

	Assert(head >= 0 && head <= pArray->maxKnownAssignedXids);
	Assert(tail >= 0 && tail < pArray->maxKnownAssignedXids);

	/*
	 * Verify that insertions occur in TransactionId sequence.  Note that even
	 * if the last existing element is marked invalid, it must still have a
	 * correctly sequenced XID value.
	 */
	if (head > tail &&
		TransactionIdFollowsOrEquals(KnownAssignedXids[head - 1], from_xid))
	{
		KnownAssignedXidsDisplay(LOG);
		elog(ERROR, "out-of-order XID insertion in KnownAssignedXids");
	}

	/*
	 * If our xids won't fit in the remaining space, compress out free space
	 */
	if (head + nxids > pArray->maxKnownAssignedXids)
	{
		KnownAssignedXidsCompress(KAX_NO_SPACE, exclusive_lock);

		head = pArray->headKnownAssignedXids;
		/* note: we no longer care about the tail pointer */

		/*
		 * If it still won't fit then we're out of memory
		 */
		if (head + nxids > pArray->maxKnownAssignedXids)
			elog(ERROR, "too many KnownAssignedXids");
	}

	/* Now we can insert the xids into the space starting at head */
	next_xid = from_xid;
	for (i = 0; i < nxids; i++)
	{
		KnownAssignedXids[head] = next_xid;
		KnownAssignedXidsValid[head] = true;
		TransactionIdAdvance(next_xid);
		head++;
	}

	/* Adjust count of number of valid entries */
	pArray->numKnownAssignedXids += nxids;

	/*
	 * Now update the head pointer.  We use a write barrier to ensure that
	 * other processors see the above array updates before they see the head
	 * pointer change.  The barrier isn't required if we're holding
	 * ProcArrayLock exclusively.
	 */
	if (!exclusive_lock)
		pg_write_barrier();

	pArray->headKnownAssignedXids = head;
}

/*
 * KnownAssignedXidsSearch
 *
 * Searches KnownAssignedXids for a specific xid and optionally removes it.
 * Returns true if it was found, false if not.
 *
 * Caller must hold ProcArrayLock in shared or exclusive mode.
 * Exclusive lock must be held for remove = true.
 */
static bool
KnownAssignedXidsSearch(TransactionId xid, bool remove)
{
	ProcArrayStruct *pArray = procArray;
	int			first,
				last;
	int			head;
	int			tail;
	int			result_index = -1;

	tail = pArray->tailKnownAssignedXids;
	head = pArray->headKnownAssignedXids;

	/*
	 * Only the startup process removes entries, so we don't need the read
	 * barrier in that case.
	 */
	if (!remove)
		pg_read_barrier();		/* pairs with KnownAssignedXidsAdd */

	/*
	 * Standard binary search.  Note we can ignore the KnownAssignedXidsValid
	 * array here, since even invalid entries will contain sorted XIDs.
	 */
	first = tail;
	last = head - 1;
	while (first <= last)
	{
		int			mid_index;
		TransactionId mid_xid;

		mid_index = (first + last) / 2;
		mid_xid = KnownAssignedXids[mid_index];

		if (xid == mid_xid)
		{
			result_index = mid_index;
			break;
		}
		else if (TransactionIdPrecedes(xid, mid_xid))
			last = mid_index - 1;
		else
			first = mid_index + 1;
	}

	if (result_index < 0)
		return false;			/* not in array */

	if (!KnownAssignedXidsValid[result_index])
		return false;			/* in array, but invalid */

	if (remove)
	{
		KnownAssignedXidsValid[result_index] = false;

		pArray->numKnownAssignedXids--;
		Assert(pArray->numKnownAssignedXids >= 0);

		/*
		 * If we're removing the tail element then advance tail pointer over
		 * any invalid elements.  This will speed future searches.
		 */
		if (result_index == tail)
		{
			tail++;
			while (tail < head && !KnownAssignedXidsValid[tail])
				tail++;
			if (tail >= head)
			{
				/* Array is empty, so we can reset both pointers */
				pArray->headKnownAssignedXids = 0;
				pArray->tailKnownAssignedXids = 0;
			}
			else
			{
				pArray->tailKnownAssignedXids = tail;
			}
		}
	}

	return true;
}

/*
 * Is the specified XID present in KnownAssignedXids[]?
 *
 * Caller must hold ProcArrayLock in shared or exclusive mode.
 */
static bool
KnownAssignedXidExists(TransactionId xid)
{
	Assert(TransactionIdIsValid(xid));

	return KnownAssignedXidsSearch(xid, false);
}

/*
 * Remove the specified XID from KnownAssignedXids[].
 *
 * Caller must hold ProcArrayLock in exclusive mode.
 */
static void
KnownAssignedXidsRemove(TransactionId xid)
{
	Assert(TransactionIdIsValid(xid));

	elog(DEBUG4, "remove KnownAssignedXid %u", xid);

	/*
	 * Note: we cannot consider it an error to remove an XID that's not
	 * present.  We intentionally remove subxact IDs while processing
	 * XLOG_XACT_ASSIGNMENT, to avoid array overflow.  Then those XIDs will be
	 * removed again when the top-level xact commits or aborts.
	 *
	 * It might be possible to track such XIDs to distinguish this case from
	 * actual errors, but it would be complicated and probably not worth it.
	 * So, just ignore the search result.
	 */
	(void) KnownAssignedXidsSearch(xid, true);
}

/*
 * KnownAssignedXidsRemoveTree
 *		Remove xid (if it's not InvalidTransactionId) and all the subxids.
 *
 * Caller must hold ProcArrayLock in exclusive mode.
 */
static void
KnownAssignedXidsRemoveTree(TransactionId xid, int nsubxids,
							TransactionId *subxids)
{
	int			i;

	if (TransactionIdIsValid(xid))
		KnownAssignedXidsRemove(xid);

	for (i = 0; i < nsubxids; i++)
		KnownAssignedXidsRemove(subxids[i]);

	/* Opportunistically compress the array */
	KnownAssignedXidsCompress(KAX_TRANSACTION_END, true);
}

/*
 * Prune KnownAssignedXids up to, but *not* including xid. If xid is invalid
 * then clear the whole table.
 *
 * Caller must hold ProcArrayLock in exclusive mode.
 */
static void
KnownAssignedXidsRemovePreceding(TransactionId removeXid)
{
	ProcArrayStruct *pArray = procArray;
	int			count = 0;
	int			head,
				tail,
				i;

	if (!TransactionIdIsValid(removeXid))
	{
		elog(DEBUG4, "removing all KnownAssignedXids");
		pArray->numKnownAssignedXids = 0;
		pArray->headKnownAssignedXids = pArray->tailKnownAssignedXids = 0;
		return;
	}

	elog(DEBUG4, "prune KnownAssignedXids to %u", removeXid);

	/*
	 * Mark entries invalid starting at the tail.  Since array is sorted, we
	 * can stop as soon as we reach an entry >= removeXid.
	 */
	tail = pArray->tailKnownAssignedXids;
	head = pArray->headKnownAssignedXids;

	for (i = tail; i < head; i++)
	{
		if (KnownAssignedXidsValid[i])
		{
			TransactionId knownXid = KnownAssignedXids[i];

			if (TransactionIdFollowsOrEquals(knownXid, removeXid))
				break;

			if (!StandbyTransactionIdIsPrepared(knownXid))
			{
				KnownAssignedXidsValid[i] = false;
				count++;
			}
		}
	}

	pArray->numKnownAssignedXids -= count;
	Assert(pArray->numKnownAssignedXids >= 0);

	/*
	 * Advance the tail pointer if we've marked the tail item invalid.
	 */
	for (i = tail; i < head; i++)
	{
		if (KnownAssignedXidsValid[i])
			break;
	}
	if (i >= head)
	{
		/* Array is empty, so we can reset both pointers */
		pArray->headKnownAssignedXids = 0;
		pArray->tailKnownAssignedXids = 0;
	}
	else
	{
		pArray->tailKnownAssignedXids = i;
	}

	/* Opportunistically compress the array */
	KnownAssignedXidsCompress(KAX_PRUNE, true);
}

/*
 * KnownAssignedXidsGet - Get an array of xids by scanning KnownAssignedXids.
 * We filter out anything >= xmax.
 *
 * Returns the number of XIDs stored into xarray[].  Caller is responsible
 * that array is large enough.
 *
 * Caller must hold ProcArrayLock in (at least) shared mode.
 */
static int
KnownAssignedXidsGet(TransactionId *xarray, TransactionId xmax)
{
	TransactionId xtmp = InvalidTransactionId;

	return KnownAssignedXidsGetAndSetXmin(xarray, &xtmp, xmax);
}

/*
 * KnownAssignedXidsGetAndSetXmin - as KnownAssignedXidsGet, plus
 * we reduce *xmin to the lowest xid value seen if not already lower.
 *
 * Caller must hold ProcArrayLock in (at least) shared mode.
 */
static int
KnownAssignedXidsGetAndSetXmin(TransactionId *xarray, TransactionId *xmin,
							   TransactionId xmax)
{
	int			count = 0;
	int			head,
				tail;
	int			i;

	/*
	 * Fetch head just once, since it may change while we loop. We can stop
	 * once we reach the initially seen head, since we are certain that an xid
	 * cannot enter and then leave the array while we hold ProcArrayLock.  We
	 * might miss newly-added xids, but they should be >= xmax so irrelevant
	 * anyway.
	 */
	tail = procArray->tailKnownAssignedXids;
	head = procArray->headKnownAssignedXids;

	pg_read_barrier();			/* pairs with KnownAssignedXidsAdd */

	for (i = tail; i < head; i++)
	{
		/* Skip any gaps in the array */
		if (KnownAssignedXidsValid[i])
		{
			TransactionId knownXid = KnownAssignedXids[i];

			/*
			 * Update xmin if required.  Only the first XID need be checked,
			 * since the array is sorted.
			 */
			if (count == 0 &&
				TransactionIdPrecedes(knownXid, *xmin))
				*xmin = knownXid;

			/*
			 * Filter out anything >= xmax, again relying on sorted property
			 * of array.
			 */
			if (TransactionIdIsValid(xmax) &&
				TransactionIdFollowsOrEquals(knownXid, xmax))
				break;

			/* Add knownXid into output array */
			xarray[count++] = knownXid;
		}
	}

	return count;
}

/*
 * Get oldest XID in the KnownAssignedXids array, or InvalidTransactionId
 * if nothing there.
 */
static TransactionId
KnownAssignedXidsGetOldestXmin(void)
{
	int			head,
				tail;
	int			i;

	/*
	 * Fetch head just once, since it may change while we loop.
	 */
	tail = procArray->tailKnownAssignedXids;
	head = procArray->headKnownAssignedXids;

	pg_read_barrier();			/* pairs with KnownAssignedXidsAdd */

	for (i = tail; i < head; i++)
	{
		/* Skip any gaps in the array */
		if (KnownAssignedXidsValid[i])
			return KnownAssignedXids[i];
	}

	return InvalidTransactionId;
}

/*
 * Display KnownAssignedXids to provide debug trail
 *
 * Currently this is only called within startup process, so we need no
 * special locking.
 *
 * Note this is pretty expensive, and much of the expense will be incurred
 * even if the elog message will get discarded.  It's not currently called
 * in any performance-critical places, however, so no need to be tenser.
 */
static void
KnownAssignedXidsDisplay(int trace_level)
{
	ProcArrayStruct *pArray = procArray;
	StringInfoData buf;
	int			head,
				tail,
				i;
	int			nxids = 0;

	tail = pArray->tailKnownAssignedXids;
	head = pArray->headKnownAssignedXids;

	initStringInfo(&buf);

	for (i = tail; i < head; i++)
	{
		if (KnownAssignedXidsValid[i])
		{
			nxids++;
			appendStringInfo(&buf, "[%d]=%u ", i, KnownAssignedXids[i]);
		}
	}

	elog(trace_level, "%d KnownAssignedXids (num=%d tail=%d head=%d) %s",
		 nxids,
		 pArray->numKnownAssignedXids,
		 pArray->tailKnownAssignedXids,
		 pArray->headKnownAssignedXids,
		 buf.data);

	pfree(buf.data);
}

/*
 * KnownAssignedXidsReset
 *		Resets KnownAssignedXids to be empty
 */
static void
KnownAssignedXidsReset(void)
{
	ProcArrayStruct *pArray = procArray;

	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	pArray->numKnownAssignedXids = 0;
	pArray->tailKnownAssignedXids = 0;
	pArray->headKnownAssignedXids = 0;

	LWLockRelease(ProcArrayLock);
}
