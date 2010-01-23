/*-------------------------------------------------------------------------
 *
 * procarray.c
 *	  POSTGRES process array code.
 *
 *
 * This module maintains an unsorted array of the PGPROC structures for all
 * active backends.  Although there are several uses for this, the principal
 * one is as a means of determining the set of currently running transactions.
 *
 * Because of various subtle race conditions it is critical that a backend
 * hold the correct locks while setting or clearing its MyProc->xid field.
 * See notes in src/backend/access/transam/README.
 *
 * The process array now also includes PGPROC structures representing
 * prepared transactions.  The xid and subxids fields of these are valid,
 * as are the myProcLocks lists.  They can be distinguished from regular
 * backend PGPROCs at need by checking for pid == 0.
 *
 * During recovery, we also keep a list of XIDs representing transactions
 * that are known to be running at current point in WAL recovery. This
 * list is kept in the KnownAssignedXids array, and updated by watching
 * the sequence of arriving xids. This is very important because if we leave
 * those xids out of the snapshot then they will appear to be already complete.
 * Later, when they have actually completed this could lead to confusion as to
 * whether those xids are visible or not, blowing a huge hole in MVCC.
 * We need 'em.
 *
 * It is theoretically possible for a FATAL error to explode before writing
 * an abort record. This could tie up KnownAssignedXids indefinitely, so
 * we prune the array when a valid list of running xids arrives. These quirks,
 * if they do ever exist in reality will not effect the correctness of
 * snapshots.
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/ipc/procarray.c,v 1.59 2010/01/23 16:37:12 sriggs Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>

#include "access/clog.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/twophase.h"
#include "miscadmin.h"
#include "storage/procarray.h"
#include "storage/standby.h"
#include "utils/builtins.h"
#include "utils/snapmgr.h"

static RunningTransactionsData	CurrentRunningXactsData;

/* Our shared memory area */
typedef struct ProcArrayStruct
{
	int			numProcs;		/* number of valid procs entries */
	int			maxProcs;		/* allocated size of procs array */

	int			numKnownAssignedXids;	/* current number of known assigned xids */
	int			maxKnownAssignedXids;	/* allocated size of known assigned xids */
	/*
	 * Highest subxid that overflowed KnownAssignedXids array. Similar to
	 * overflowing cached subxids in PGPROC entries.
	 */
	TransactionId	lastOverflowedXid;

	/*
	 * We declare procs[] as 1 entry because C wants a fixed-size array, but
	 * actually it is maxProcs entries long.
	 */
	PGPROC	   *procs[1];		/* VARIABLE LENGTH ARRAY */
} ProcArrayStruct;

static ProcArrayStruct *procArray;

/*
 * Bookkeeping for tracking emulated transactions in recovery
 */
static HTAB *KnownAssignedXidsHash;
static TransactionId	latestObservedXid = InvalidTransactionId;

/*
 * If we're in STANDBY_SNAPSHOT_PENDING state, standbySnapshotPendingXmin is
 * the highest xid that might still be running that we don't have in
 * KnownAssignedXids.
 */
static TransactionId standbySnapshotPendingXmin;

/*
 * Oldest transaction still running according to the running-xacts snapshot
 * we initialized standby mode from.
 */
static TransactionId snapshotOldestActiveXid;

#ifdef XIDCACHE_DEBUG

/* counters for XidCache measurement */
static long xc_by_recent_xmin = 0;
static long xc_by_known_xact = 0;
static long xc_by_my_xact = 0;
static long xc_by_latest_xid = 0;
static long xc_by_main_xid = 0;
static long xc_by_child_xid = 0;
static long xc_no_overflow = 0;
static long xc_slow_answer = 0;

#define xc_by_recent_xmin_inc()		(xc_by_recent_xmin++)
#define xc_by_known_xact_inc()		(xc_by_known_xact++)
#define xc_by_my_xact_inc()			(xc_by_my_xact++)
#define xc_by_latest_xid_inc()		(xc_by_latest_xid++)
#define xc_by_main_xid_inc()		(xc_by_main_xid++)
#define xc_by_child_xid_inc()		(xc_by_child_xid++)
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
#define xc_no_overflow_inc()		((void) 0)
#define xc_slow_answer_inc()		((void) 0)
#endif   /* XIDCACHE_DEBUG */

/* Primitives for KnownAssignedXids array handling for standby */
static int  KnownAssignedXidsGet(TransactionId *xarray, TransactionId xmax);
static int	KnownAssignedXidsGetAndSetXmin(TransactionId *xarray, TransactionId *xmin,
											TransactionId xmax);
static bool KnownAssignedXidsExist(TransactionId xid);
static void KnownAssignedXidsAdd(TransactionId *xids, int nxids);
static void KnownAssignedXidsRemove(TransactionId xid);
static void KnownAssignedXidsRemoveMany(TransactionId xid, bool keepPreparedXacts);
static void KnownAssignedXidsDisplay(int trace_level);

/*
 * Report shared-memory space needed by CreateSharedProcArray.
 */
Size
ProcArrayShmemSize(void)
{
	Size		size;

	size = offsetof(ProcArrayStruct, procs);

	/* Normal processing - MyProc slots */
#define PROCARRAY_MAXPROCS (MaxBackends + max_prepared_xacts)
	size = add_size(size, mul_size(sizeof(PGPROC *), PROCARRAY_MAXPROCS));

	/*
	 * During recovery processing we have a data structure called
	 * KnownAssignedXids, created in shared memory. Local data structures are
	 * also created in various backends during GetSnapshotData(),
	 * TransactionIdIsInProgress() and GetRunningTransactionData(). All of the
	 * main structures created in those functions must be identically sized,
	 * since we may at times copy the whole of the data structures around. We
	 * refer to this size as TOTAL_MAX_CACHED_SUBXIDS.
	 */
#define TOTAL_MAX_CACHED_SUBXIDS ((PGPROC_MAX_CACHED_SUBXIDS + 1) * PROCARRAY_MAXPROCS)
	if (XLogRequestRecoveryConnections)
		size = add_size(size,
						hash_estimate_size(TOTAL_MAX_CACHED_SUBXIDS,
										   sizeof(TransactionId)));

	return size;
}

/*
 * Initialize the shared PGPROC array during postmaster startup.
 */
void
CreateSharedProcArray(void)
{
	bool		found;

	/* Create or attach to the ProcArray shared structure */
	procArray = (ProcArrayStruct *)
		ShmemInitStruct("Proc Array",
						mul_size(sizeof(PGPROC *), PROCARRAY_MAXPROCS),
						&found);

	if (!found)
	{
		/*
		 * We're the first - initialize.
		 */
		/* Normal processing */
		procArray->numProcs = 0;
		procArray->maxProcs = PROCARRAY_MAXPROCS;
		procArray->numKnownAssignedXids = 0;
		procArray->maxKnownAssignedXids = TOTAL_MAX_CACHED_SUBXIDS;
		procArray->lastOverflowedXid = InvalidTransactionId;
	}

	if (XLogRequestRecoveryConnections)
	{
		/* Create or attach to the KnownAssignedXids hash table */
		HASHCTL		info;

		MemSet(&info, 0, sizeof(info));
		info.keysize = sizeof(TransactionId);
		info.entrysize = sizeof(TransactionId);
		info.hash = tag_hash;

		KnownAssignedXidsHash = ShmemInitHash("KnownAssignedXids Hash",
											  TOTAL_MAX_CACHED_SUBXIDS,
											  TOTAL_MAX_CACHED_SUBXIDS,
											  &info,
											  HASH_ELEM | HASH_FUNCTION);
		if (!KnownAssignedXidsHash)
			elog(FATAL, "could not initialize known assigned xids hash table");
	}
}

/*
 * Add the specified PGPROC to the shared array.
 */
void
ProcArrayAdd(PGPROC *proc)
{
	ProcArrayStruct *arrayP = procArray;

	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	if (arrayP->numProcs >= arrayP->maxProcs)
	{
		/*
		 * Ooops, no room.	(This really shouldn't happen, since there is a
		 * fixed supply of PGPROC structs too, and so we should have failed
		 * earlier.)
		 */
		LWLockRelease(ProcArrayLock);
		ereport(FATAL,
				(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
				 errmsg("sorry, too many clients already")));
	}

	arrayP->procs[arrayP->numProcs] = proc;
	arrayP->numProcs++;

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
	int			index;

#ifdef XIDCACHE_DEBUG
	/* dump stats at backend shutdown, but not prepared-xact end */
	if (proc->pid != 0)
		DisplayXidCache();
#endif

	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	if (TransactionIdIsValid(latestXid))
	{
		Assert(TransactionIdIsValid(proc->xid));

		/* Advance global latestCompletedXid while holding the lock */
		if (TransactionIdPrecedes(ShmemVariableCache->latestCompletedXid,
								  latestXid))
			ShmemVariableCache->latestCompletedXid = latestXid;
	}
	else
	{
		/* Shouldn't be trying to remove a live transaction here */
		Assert(!TransactionIdIsValid(proc->xid));
	}

	for (index = 0; index < arrayP->numProcs; index++)
	{
		if (arrayP->procs[index] == proc)
		{
			arrayP->procs[index] = arrayP->procs[arrayP->numProcs - 1];
			arrayP->procs[arrayP->numProcs - 1] = NULL; /* for debugging */
			arrayP->numProcs--;
			LWLockRelease(ProcArrayLock);
			return;
		}
	}

	/* Ooops */
	LWLockRelease(ProcArrayLock);

	elog(LOG, "failed to find proc %p in ProcArray", proc);
}


/*
 * ProcArrayEndTransaction -- mark a transaction as no longer running
 *
 * This is used interchangeably for commit and abort cases.  The transaction
 * commit/abort must already be reported to WAL and pg_clog.
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
		 * We must lock ProcArrayLock while clearing proc->xid, so that we do
		 * not exit the set of "running" transactions while someone else is
		 * taking a snapshot.  See discussion in
		 * src/backend/access/transam/README.
		 */
		Assert(TransactionIdIsValid(proc->xid));

		LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

		proc->xid = InvalidTransactionId;
		proc->lxid = InvalidLocalTransactionId;
		proc->xmin = InvalidTransactionId;
		/* must be cleared with xid/xmin: */
		proc->vacuumFlags &= ~PROC_VACUUM_STATE_MASK;
		proc->inCommit = false; /* be sure this is cleared in abort */
		proc->recoveryConflictPending = false;

		/* Clear the subtransaction-XID cache too while holding the lock */
		proc->subxids.nxids = 0;
		proc->subxids.overflowed = false;

		/* Also advance global latestCompletedXid while holding the lock */
		if (TransactionIdPrecedes(ShmemVariableCache->latestCompletedXid,
								  latestXid))
			ShmemVariableCache->latestCompletedXid = latestXid;

		LWLockRelease(ProcArrayLock);
	}
	else
	{
		/*
		 * If we have no XID, we don't need to lock, since we won't affect
		 * anyone else's calculation of a snapshot.  We might change their
		 * estimate of global xmin, but that's OK.
		 */
		Assert(!TransactionIdIsValid(proc->xid));

		proc->lxid = InvalidLocalTransactionId;
		proc->xmin = InvalidTransactionId;
		/* must be cleared with xid/xmin: */
		proc->vacuumFlags &= ~PROC_VACUUM_STATE_MASK;
		proc->inCommit = false; /* be sure this is cleared in abort */
		proc->recoveryConflictPending = false;

		Assert(proc->subxids.nxids == 0);
		Assert(proc->subxids.overflowed == false);
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
	/*
	 * We can skip locking ProcArrayLock here, because this action does not
	 * actually change anyone's view of the set of running XIDs: our entry is
	 * duplicate with the gxact that has already been inserted into the
	 * ProcArray.
	 */
	proc->xid = InvalidTransactionId;
	proc->lxid = InvalidLocalTransactionId;
	proc->xmin = InvalidTransactionId;
	proc->recoveryConflictPending = false;

	/* redundant, but just in case */
	proc->vacuumFlags &= ~PROC_VACUUM_STATE_MASK;
	proc->inCommit = false;

	/* Clear the subtransaction-XID cache too */
	proc->subxids.nxids = 0;
	proc->subxids.overflowed = false;
}

void
ProcArrayInitRecoveryInfo(TransactionId oldestActiveXid)
{
	snapshotOldestActiveXid = oldestActiveXid;
}

/*
 * ProcArrayApplyRecoveryInfo -- apply recovery info about xids
 *
 * Takes us through 3 states: Uninitialized, Pending and Ready.
 * Normal case is to go all the way to Ready straight away, though there
 * are atypical cases where we need to take it in steps.
 *
 * Use the data about running transactions on master to create the initial
 * state of KnownAssignedXids. We also these records to regularly prune
 * KnownAssignedXids because we know it is possible that some transactions
 * with FATAL errors do not write abort records, which could cause eventual
 * overflow.
 *
 * Only used during recovery. Notice the signature is very similar to a
 * _redo function and its difficult to decide exactly where this code should
 * reside.
 */
void
ProcArrayApplyRecoveryInfo(RunningTransactions running)
{
	int				xid_index;	/* main loop */
	TransactionId	*xids;
	int				nxids;

	Assert(standbyState >= STANDBY_INITIALIZED);

	/*
	 * Remove stale transactions, if any.
	 */
	ExpireOldKnownAssignedTransactionIds(running->oldestRunningXid);
	StandbyReleaseOldLocks(running->oldestRunningXid);

	/*
	 * If our snapshot is already valid, nothing else to do...
	 */
	if (standbyState == STANDBY_SNAPSHOT_READY)
		return;

	/*
	 * If our initial RunningXactData had an overflowed snapshot then we
	 * knew we were missing some subxids from our snapshot. We can use
	 * this data as an initial snapshot, but we cannot yet mark it valid.
	 * We know that the missing subxids are equal to or earlier than
	 * nextXid. After we initialise we continue to apply changes during
	 * recovery, so once the oldestRunningXid is later than the nextXid
	 * from the initial snapshot we know that we no longer have missing
	 * information and can mark the snapshot as valid.
	 */
	if (standbyState == STANDBY_SNAPSHOT_PENDING)
	{
		if (TransactionIdPrecedes(standbySnapshotPendingXmin,
								  running->oldestRunningXid))
		{
			standbyState = STANDBY_SNAPSHOT_READY;
			elog(trace_recovery(DEBUG2),
					"running xact data now proven complete");
			elog(trace_recovery(DEBUG2),
					"recovery snapshots are now enabled");
		}
		return;
	}

	/*
	 * OK, we need to initialise from the RunningXactData record
	 */
	latestObservedXid = running->nextXid;
	TransactionIdRetreat(latestObservedXid);

	/*
	 * If the snapshot overflowed, then we still initialise with what we
	 * know, but the recovery snapshot isn't fully valid yet because we
	 * know there are some subxids missing (ergo we don't know which ones)
	 */
	if (!running->subxid_overflow)
	{
		standbyState = STANDBY_SNAPSHOT_READY;
		standbySnapshotPendingXmin = InvalidTransactionId;
	}
	else
	{
		standbyState = STANDBY_SNAPSHOT_PENDING;
		standbySnapshotPendingXmin = latestObservedXid;
		ereport(LOG,
				(errmsg("consistent state delayed because recovery snapshot incomplete")));
	}

	nxids = running->xcnt;
	xids = running->xids;

	KnownAssignedXidsDisplay(trace_recovery(DEBUG3));

	/*
	 * Scan through the incoming array of RunningXacts and collect xids.
	 * We don't use SubtransSetParent because it doesn't matter yet. If
	 * we aren't overflowed then all xids will fit in snapshot and so we
	 * don't need subtrans. If we later overflow, an xid assignment record
	 * will add xids to subtrans. If RunningXacts is overflowed then we
	 * don't have enough information to correctly update subtrans anyway.
	 */

	/*
	 * Nobody else is running yet, but take locks anyhow
	 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	/* Reset latestCompletedXid */
	ShmemVariableCache->latestCompletedXid = running->nextXid;
	TransactionIdRetreat(ShmemVariableCache->latestCompletedXid);

	/*
	 * Add our new xids into the array
	 */
	for (xid_index = 0; xid_index < running->xcnt; xid_index++)
	{
		TransactionId xid = running->xids[xid_index];

		/*
		 * The running-xacts snapshot can contain xids that did finish between
		 * when the snapshot was taken and when it was written to WAL. Such
		 * transactions are not running anymore, so ignore them.
		 */
		if (TransactionIdDidCommit(xid) || TransactionIdDidAbort(xid))
			continue;

		KnownAssignedXidsAdd(&xid, 1);
	}

	KnownAssignedXidsDisplay(trace_recovery(DEBUG3));

	/*
	 * Update lastOverflowedXid if the snapshot had overflown. We don't know
	 * the exact value for this, so conservatively assume that it's nextXid-1
	 */
	if (running->subxid_overflow &&
		TransactionIdFollows(latestObservedXid, procArray->lastOverflowedXid))
		procArray->lastOverflowedXid = latestObservedXid;
	else if (TransactionIdFollows(running->oldestRunningXid,
								  procArray->lastOverflowedXid))
		procArray->lastOverflowedXid = InvalidTransactionId;

	LWLockRelease(ProcArrayLock);

	/* nextXid must be beyond any observed xid */
	if (TransactionIdFollows(running->nextXid, ShmemVariableCache->nextXid))
		ShmemVariableCache->nextXid = running->nextXid;

	elog(trace_recovery(DEBUG2),
		"running transaction data initialized");
	if (standbyState == STANDBY_SNAPSHOT_READY)
		elog(trace_recovery(DEBUG2),
			"recovery snapshots are now enabled");
}

void
ProcArrayApplyXidAssignment(TransactionId topxid,
							int nsubxids, TransactionId *subxids)
{
	TransactionId max_xid;
	int		i;

	if (standbyState < STANDBY_SNAPSHOT_PENDING)
		return;

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
	 * Notice that we update pg_subtrans with the top-level xid, rather
	 * than the parent xid. This is a difference between normal
	 * processing and recovery, yet is still correct in all cases. The
	 * reason is that subtransaction commit is not marked in clog until
	 * commit processing, so all aborted subtransactions have already been
	 * clearly marked in clog. As a result we are able to refer directly
	 * to the top-level transaction's state rather than skipping through
	 * all the intermediate states in the subtransaction tree. This
	 * should be the first time we have attempted to SubTransSetParent().
	 */
	for (i = 0; i < nsubxids; i++)
		SubTransSetParent(subxids[i], topxid, false);

	/*
	 * Uses same locking as transaction commit
	 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	/*
	 * Remove from known-assigned-xacts.
	 */
	for (i = 0; i < nsubxids; i++)
		KnownAssignedXidsRemove(subxids[i]);

	/*
	 * Advance lastOverflowedXid when required.
	 */
	if (TransactionIdPrecedes(procArray->lastOverflowedXid, max_xid))
		procArray->lastOverflowedXid = max_xid;

	LWLockRelease(ProcArrayLock);
}

/*
 * TransactionIdIsInProgress -- is given transaction running in some backend
 *
 * Aside from some shortcuts such as checking RecentXmin and our own Xid,
 * there are three possibilities for finding a running transaction:
 *
 * 1. the given Xid is a main transaction Id.  We will find this out cheaply
 * by looking at the PGPROC struct for each backend.
 *
 * 2. the given Xid is one of the cached subxact Xids in the PGPROC array.
 * We can find this out cheaply too.
 *
 * 3. Search the SubTrans tree to find the Xid's topmost parent, and then
 * see if that is running according to PGPROC.	This is the slowest, but
 * sadly it has to be done always if the other two failed, unless we see
 * that the cached subxact sets are complete (none have overflowed).
 *
 * ProcArrayLock has to be held while we do 1 and 2.  If we save the top Xids
 * while doing 1, we can release the ProcArrayLock while we do 3.  This buys
 * back some concurrency (we can't retrieve the main Xids from PGPROC again
 * anyway; see GetNewTransactionId).
 */
bool
TransactionIdIsInProgress(TransactionId xid)
{
	static TransactionId *xids = NULL;
	int			nxids = 0;
	ProcArrayStruct *arrayP = procArray;
	TransactionId topxid;
	int			i,
				j;

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
	if (TransactionIdIsKnownCompleted(xid))
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
	 * If not first time through, get workspace to remember main XIDs in. We
	 * malloc it permanently to avoid repeated palloc/pfree overhead.
	 */
	if (xids == NULL)
	{
		/*
		 * In hot standby mode, reserve enough space to hold all xids in
		 * the known-assigned list. If we later finish recovery, we no longer
		 * need the bigger array, but we don't bother to shrink it.
		 */
		int	maxxids = RecoveryInProgress() ?
			arrayP->maxProcs : TOTAL_MAX_CACHED_SUBXIDS;

		xids = (TransactionId *) malloc(maxxids * sizeof(TransactionId));
		if (xids == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
	}

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	/*
	 * Now that we have the lock, we can check latestCompletedXid; if the
	 * target Xid is after that, it's surely still running.
	 */
	if (TransactionIdPrecedes(ShmemVariableCache->latestCompletedXid, xid))
	{
		LWLockRelease(ProcArrayLock);
		xc_by_latest_xid_inc();
		return true;
	}

	/* No shortcuts, gotta grovel through the array */
	for (i = 0; i < arrayP->numProcs; i++)
	{
		volatile PGPROC *proc = arrayP->procs[i];
		TransactionId pxid;

		/* Ignore my own proc --- dealt with it above */
		if (proc == MyProc)
			continue;

		/* Fetch xid just once - see GetNewTransactionId */
		pxid = proc->xid;

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
		for (j = proc->subxids.nxids - 1; j >= 0; j--)
		{
			/* Fetch xid just once - see GetNewTransactionId */
			TransactionId cxid = proc->subxids.xids[j];

			if (TransactionIdEquals(cxid, xid))
			{
				LWLockRelease(ProcArrayLock);
				xc_by_child_xid_inc();
				return true;
			}
		}

		/*
		 * Save the main Xid for step 3.  We only need to remember main Xids
		 * that have uncached children.  (Note: there is no race condition
		 * here because the overflowed flag cannot be cleared, only set, while
		 * we hold ProcArrayLock.  So we can't miss an Xid that we need to
		 * worry about.)
		 */
		if (proc->subxids.overflowed)
			xids[nxids++] = pxid;
	}

	/* In hot standby mode, check the known-assigned-xids list. */
	if (RecoveryInProgress())
	{
		/* none of the PGPROC entries should have XIDs in hot standby mode */
		Assert(nxids == 0);

		if (KnownAssignedXidsExist(xid))
		{
			LWLockRelease(ProcArrayLock);
			/* XXX: should we have a separate counter for this? */
			/* xc_by_main_xid_inc(); */
			return true;
		}

		/*
		 * If the KnownAssignedXids overflowed, we have to check
		 * pg_subtrans too. Copy all xids from KnownAssignedXids that are
		 * lower than xid, since if xid is a subtransaction its parent will
		 * always have a lower value.
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
		return false;
	}

	/*
	 * Step 3: have to check pg_subtrans.
	 *
	 * At this point, we know it's either a subtransaction of one of the Xids
	 * in xids[], or it's not running.  If it's an already-failed
	 * subtransaction, we want to say "not running" even though its parent may
	 * still be running.  So first, check pg_clog to see if it's been aborted.
	 */
	xc_slow_answer_inc();

	if (TransactionIdDidAbort(xid))
		return false;

	/*
	 * It isn't aborted, so check whether the transaction tree it belongs to
	 * is still running (or, more precisely, whether it was running when we
	 * held ProcArrayLock).
	 */
	topxid = SubTransGetTopmostTransaction(xid);
	Assert(TransactionIdIsValid(topxid));
	if (!TransactionIdEquals(topxid, xid))
	{
		for (i = 0; i < nxids; i++)
		{
			if (TransactionIdEquals(xids[i], topxid))
				return true;
		}
	}

	return false;
}

/*
 * TransactionIdIsActive -- is xid the top-level XID of an active backend?
 *
 * This differs from TransactionIdIsInProgress in that it ignores prepared
 * transactions.  Also, we ignore subtransactions since that's not needed
 * for current uses.
 */
bool
TransactionIdIsActive(TransactionId xid)
{
	bool		result = false;
	ProcArrayStruct *arrayP = procArray;
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
		volatile PGPROC *proc = arrayP->procs[i];

		/* Fetch xid just once - see GetNewTransactionId */
		TransactionId pxid = proc->xid;

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
 * GetOldestXmin -- returns oldest transaction that was running
 *					when any current transaction was started.
 *
 * If allDbs is TRUE then all backends are considered; if allDbs is FALSE
 * then only backends running in my own database are considered.
 *
 * If ignoreVacuum is TRUE then backends with the PROC_IN_VACUUM flag set are
 * ignored.
 *
 * This is used by VACUUM to decide which deleted tuples must be preserved
 * in a table.	allDbs = TRUE is needed for shared relations, but allDbs =
 * FALSE is sufficient for non-shared relations, since only backends in my
 * own database could ever see the tuples in them.	Also, we can ignore
 * concurrently running lazy VACUUMs because (a) they must be working on other
 * tables, and (b) they don't need to do snapshot-based lookups.
 *
 * This is also used to determine where to truncate pg_subtrans.  allDbs
 * must be TRUE for that case, and ignoreVacuum FALSE.
 *
 * Note: we include all currently running xids in the set of considered xids.
 * This ensures that if a just-started xact has not yet set its snapshot,
 * when it does set the snapshot it cannot set xmin less than what we compute.
 * See notes in src/backend/access/transam/README.
 */
TransactionId
GetOldestXmin(bool allDbs, bool ignoreVacuum)
{
	ProcArrayStruct *arrayP = procArray;
	TransactionId result;
	int			index;

	/* Cannot look for individual databases during recovery */
	Assert(allDbs || !RecoveryInProgress());

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	/*
	 * We initialize the MIN() calculation with latestCompletedXid + 1. This
	 * is a lower bound for the XIDs that might appear in the ProcArray later,
	 * and so protects us against overestimating the result due to future
	 * additions.
	 */
	result = ShmemVariableCache->latestCompletedXid;
	Assert(TransactionIdIsNormal(result));
	TransactionIdAdvance(result);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		volatile PGPROC *proc = arrayP->procs[index];

		if (ignoreVacuum && (proc->vacuumFlags & PROC_IN_VACUUM))
			continue;

		if (allDbs || proc->databaseId == MyDatabaseId)
		{
			/* Fetch xid just once - see GetNewTransactionId */
			TransactionId xid = proc->xid;

			/* First consider the transaction's own Xid, if any */
			if (TransactionIdIsNormal(xid) &&
				TransactionIdPrecedes(xid, result))
				result = xid;

			/*
			 * Also consider the transaction's Xmin, if set.
			 *
			 * We must check both Xid and Xmin because a transaction might
			 * have an Xmin but not (yet) an Xid; conversely, if it has an
			 * Xid, that could determine some not-yet-set Xmin.
			 */
			xid = proc->xmin;	/* Fetch just once */
			if (TransactionIdIsNormal(xid) &&
				TransactionIdPrecedes(xid, result))
				result = xid;
		}
	}

	LWLockRelease(ProcArrayLock);

	/*
	 * Compute the cutoff XID, being careful not to generate a "permanent" XID
	 */
	result -= vacuum_defer_cleanup_age;
	if (!TransactionIdIsNormal(result))
		result = FirstNormalTransactionId;

	return result;
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
 * *may* need to be done to determine what's running (see XidInMVCCSnapshot()
 * in tqual.c).
 *
 * We also update the following backend-global variables:
 *		TransactionXmin: the oldest xmin of any snapshot in use in the
 *			current transaction (this is the same as MyProc->xmin).
 *		RecentXmin: the xmin computed for the most recent snapshot.  XIDs
 *			older than this are known not running any more.
 *		RecentGlobalXmin: the global xmin (oldest TransactionXmin across all
 *			running transactions, except those running LAZY VACUUM).  This is
 *			the same computation done by GetOldestXmin(true, true).
 *
 * Note: this function should probably not be called with an argument that's
 * not statically allocated (see xip allocation below).
 */
Snapshot
GetSnapshotData(Snapshot snapshot)
{
	ProcArrayStruct *arrayP = procArray;
	TransactionId xmin;
	TransactionId xmax;
	TransactionId globalxmin;
	int			index;
	int			count = 0;
	int			subcount = 0;
	bool		suboverflowed = false;

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
		 * First call for this snapshot. Snapshot is same size whether
		 * or not we are in recovery, see later comments.
		 */
		snapshot->xip = (TransactionId *)
			malloc(arrayP->maxProcs * sizeof(TransactionId));
		if (snapshot->xip == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		Assert(snapshot->subxip == NULL);
		snapshot->subxip = (TransactionId *)
			malloc(TOTAL_MAX_CACHED_SUBXIDS * sizeof(TransactionId));
		if (snapshot->subxip == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
	}

	snapshot->takenDuringRecovery = RecoveryInProgress();

	/*
	 * It is sufficient to get shared lock on ProcArrayLock, even if we are
	 * going to set MyProc->xmin.
	 */
	LWLockAcquire(ProcArrayLock, LW_SHARED);

	/* xmax is always latestCompletedXid + 1 */
	xmax = ShmemVariableCache->latestCompletedXid;
	Assert(TransactionIdIsNormal(xmax));
	TransactionIdAdvance(xmax);

	/* initialize xmin calculation with xmax */
	globalxmin = xmin = xmax;

	/*
	 * Spin over procArray checking xid, xmin, and subxids.  The goal is to
	 * gather all active xids, find the lowest xmin, and try to record
	 * subxids.
	 */
	for (index = 0; index < arrayP->numProcs; index++)
	{
		volatile PGPROC *proc = arrayP->procs[index];
		TransactionId xid;

		/* Ignore procs running LAZY VACUUM */
		if (proc->vacuumFlags & PROC_IN_VACUUM)
			continue;

		/* Update globalxmin to be the smallest valid xmin */
		xid = proc->xmin;		/* fetch just once */
		if (TransactionIdIsNormal(xid) &&
			TransactionIdPrecedes(xid, globalxmin))
			globalxmin = xid;

		/* Fetch xid just once - see GetNewTransactionId */
		xid = proc->xid;

		/*
		 * If the transaction has been assigned an xid < xmax we add it to the
		 * snapshot, and update xmin if necessary.	There's no need to store
		 * XIDs >= xmax, since we'll treat them as running anyway.  We don't
		 * bother to examine their subxids either.
		 *
		 * We don't include our own XID (if any) in the snapshot, but we must
		 * include it into xmin.
		 */
		if (TransactionIdIsNormal(xid))
		{
			Assert(!snapshot->takenDuringRecovery);
			if (TransactionIdFollowsOrEquals(xid, xmax))
				continue;
			if (proc != MyProc)
				snapshot->xip[count++] = xid;
			if (TransactionIdPrecedes(xid, xmin))
				xmin = xid;
		}

		/*
		 * Save subtransaction XIDs if possible (if we've already overflowed,
		 * there's no point).  Note that the subxact XIDs must be later than
		 * their parent, so no need to check them against xmin.  We could
		 * filter against xmax, but it seems better not to do that much work
		 * while holding the ProcArrayLock.
		 *
		 * The other backend can add more subxids concurrently, but cannot
		 * remove any.	Hence it's important to fetch nxids just once. Should
		 * be safe to use memcpy, though.  (We needn't worry about missing any
		 * xids added concurrently, because they must postdate xmax.)
		 *
		 * Again, our own XIDs are not included in the snapshot.
		 */
		if (!suboverflowed && proc != MyProc)
		{
			if (proc->subxids.overflowed)
				suboverflowed = true;
			else
			{
				int			nxids = proc->subxids.nxids;

				if (nxids > 0)
				{
					Assert(!snapshot->takenDuringRecovery);
					memcpy(snapshot->subxip + subcount,
						   (void *) proc->subxids.xids,
						   nxids * sizeof(TransactionId));
					subcount += nxids;
				}
			}
		}
	}

	/*
	 * If in recovery get any known assigned xids.
	 */
	if (snapshot->takenDuringRecovery)
	{
		Assert(count == 0);

		/*
		 * We store all xids directly into subxip[]. Here's why:
		 *
		 * In recovery we don't know which xids are top-level and which are
		 * subxacts, a design choice that greatly simplifies xid processing.
		 *
		 * It seems like we would want to try to put xids into xip[] only,
		 * but that is fairly small. We would either need to make that bigger
		 * or to increase the rate at which we WAL-log xid assignment;
		 * neither is an appealing choice.
		 *
		 * We could try to store xids into xip[] first and then into subxip[]
		 * if there are too many xids. That only works if the snapshot doesn't
		 * overflow because we do not search subxip[] in that case. A simpler
		 * way is to just store all xids in the subxact array because this
		 * is by far the bigger array. We just leave the xip array empty.
		 *
		 * Either way we need to change the way XidInMVCCSnapshot() works
		 * depending upon when the snapshot was taken, or change normal
		 * snapshot processing so it matches.
		 */
		subcount = KnownAssignedXidsGetAndSetXmin(snapshot->subxip, &xmin, xmax);

		if (TransactionIdPrecedes(xmin, procArray->lastOverflowedXid))
			suboverflowed = true;
	}

	if (!TransactionIdIsValid(MyProc->xmin))
		MyProc->xmin = TransactionXmin = xmin;

	LWLockRelease(ProcArrayLock);

	/*
	 * Update globalxmin to include actual process xids.  This is a slightly
	 * different way of computing it than GetOldestXmin uses, but should give
	 * the same result.
	 */
	if (TransactionIdPrecedes(xmin, globalxmin))
		globalxmin = xmin;

	/* Update global variables too */
	RecentGlobalXmin = globalxmin - vacuum_defer_cleanup_age;
	if (!TransactionIdIsNormal(RecentGlobalXmin))
		RecentGlobalXmin = FirstNormalTransactionId;
	RecentXmin = xmin;

	snapshot->xmin = xmin;
	snapshot->xmax = xmax;
	snapshot->xcnt = count;
	snapshot->subxcnt = subcount;
	snapshot->suboverflowed = suboverflowed;

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
 * GetRunningTransactionData -- returns information about running transactions.
 *
 * Similar to GetSnapshotData but returning more information. We include
 * all PGPROCs with an assigned TransactionId, even VACUUM processes.
 *
 * This is never executed during recovery so there is no need to look at
 * KnownAssignedXids.
 *
 * We don't worry about updating other counters, we want to keep this as
 * simple as possible and leave GetSnapshotData() as the primary code for
 * that bookkeeping.
 */
RunningTransactions
GetRunningTransactionData(void)
{
	ProcArrayStruct *arrayP = procArray;
	RunningTransactions CurrentRunningXacts = (RunningTransactions) &CurrentRunningXactsData;
	TransactionId latestCompletedXid;
	TransactionId oldestRunningXid;
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
	 * Should only be allocated for bgwriter, since only ever executed
	 * during checkpoints.
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

	latestCompletedXid = ShmemVariableCache->latestCompletedXid;

	oldestRunningXid = ShmemVariableCache->nextXid;
	/*
	 * Spin over procArray collecting all xids and subxids.
	 */
	for (index = 0; index < arrayP->numProcs; index++)
	{
		volatile PGPROC *proc = arrayP->procs[index];
		TransactionId xid;
		int			nxids;

		/* Fetch xid just once - see GetNewTransactionId */
		xid = proc->xid;

		/*
		 * We don't need to store transactions that don't have a TransactionId
		 * yet because they will not show as running on a standby server.
		 */
		if (!TransactionIdIsValid(xid))
			continue;

		xids[count++] = xid;

		if (TransactionIdPrecedes(xid, oldestRunningXid))
			oldestRunningXid = xid;

		/*
		 * Save subtransaction XIDs. Other backends can't add or remove entries
		 * while we're holding XidGenLock.
		 */
		nxids = proc->subxids.nxids;
		if (nxids > 0)
		{
			memcpy(&xids[count], (void *) proc->subxids.xids,
				   nxids * sizeof(TransactionId));
			count += nxids;
			subcount += nxids;

			if (proc->subxids.overflowed)
				suboverflowed = true;

			/*
			 * Top-level XID of a transaction is always greater than any of
			 * its subxids, so we don't need to check if any of the subxids
			 * are smaller than oldestRunningXid
			 */
		}
	}

	CurrentRunningXacts->xcnt = count;
	CurrentRunningXacts->subxid_overflow = suboverflowed;
	CurrentRunningXacts->nextXid = ShmemVariableCache->nextXid;
	CurrentRunningXacts->oldestRunningXid = oldestRunningXid;

	LWLockRelease(XidGenLock);
	LWLockRelease(ProcArrayLock);

	return CurrentRunningXacts;
}

/*
 * GetTransactionsInCommit -- Get the XIDs of transactions that are committing
 *
 * Constructs an array of XIDs of transactions that are currently in commit
 * critical sections, as shown by having inCommit set in their PGPROC entries.
 *
 * *xids_p is set to a palloc'd array that should be freed by the caller.
 * The return value is the number of valid entries.
 *
 * Note that because backends set or clear inCommit without holding any lock,
 * the result is somewhat indeterminate, but we don't really care.  Even in
 * a multiprocessor with delayed writes to shared memory, it should be certain
 * that setting of inCommit will propagate to shared memory when the backend
 * takes the WALInsertLock, so we cannot fail to see an xact as inCommit if
 * it's already inserted its commit record.  Whether it takes a little while
 * for clearing of inCommit to propagate is unimportant for correctness.
 */
int
GetTransactionsInCommit(TransactionId **xids_p)
{
	ProcArrayStruct *arrayP = procArray;
	TransactionId *xids;
	int			nxids;
	int			index;

	xids = (TransactionId *) palloc(arrayP->maxProcs * sizeof(TransactionId));
	nxids = 0;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		volatile PGPROC *proc = arrayP->procs[index];

		/* Fetch xid just once - see GetNewTransactionId */
		TransactionId pxid = proc->xid;

		if (proc->inCommit && TransactionIdIsValid(pxid))
			xids[nxids++] = pxid;
	}

	LWLockRelease(ProcArrayLock);

	*xids_p = xids;
	return nxids;
}

/*
 * HaveTransactionsInCommit -- Are any of the specified XIDs in commit?
 *
 * This is used with the results of GetTransactionsInCommit to see if any
 * of the specified XIDs are still in their commit critical sections.
 *
 * Note: this is O(N^2) in the number of xacts that are/were in commit, but
 * those numbers should be small enough for it not to be a problem.
 */
bool
HaveTransactionsInCommit(TransactionId *xids, int nxids)
{
	bool		result = false;
	ProcArrayStruct *arrayP = procArray;
	int			index;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		volatile PGPROC *proc = arrayP->procs[index];

		/* Fetch xid just once - see GetNewTransactionId */
		TransactionId pxid = proc->xid;

		if (proc->inCommit && TransactionIdIsValid(pxid))
		{
			int			i;

			for (i = 0; i < nxids; i++)
			{
				if (xids[i] == pxid)
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
 * BackendPidGetProc -- get a backend's PGPROC given its PID
 *
 * Returns NULL if not found.  Note that it is up to the caller to be
 * sure that the question remains meaningful for long enough for the
 * answer to be used ...
 */
PGPROC *
BackendPidGetProc(int pid)
{
	PGPROC	   *result = NULL;
	ProcArrayStruct *arrayP = procArray;
	int			index;

	if (pid == 0)				/* never match dummy PGPROCs */
		return NULL;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		PGPROC	   *proc = arrayP->procs[index];

		if (proc->pid == pid)
		{
			result = proc;
			break;
		}
	}

	LWLockRelease(ProcArrayLock);

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
 * Beware that not every xact has an XID assigned.	However, as long as you
 * only call this using an XID found on disk, you're safe.
 */
int
BackendXidGetPid(TransactionId xid)
{
	int			result = 0;
	ProcArrayStruct *arrayP = procArray;
	int			index;

	if (xid == InvalidTransactionId)	/* never match invalid xid */
		return 0;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		volatile PGPROC *proc = arrayP->procs[index];

		if (proc->xid == xid)
		{
			result = proc->pid;
			break;
		}
	}

	LWLockRelease(ProcArrayLock);

	return result;
}

/*
 * IsBackendPid -- is a given pid a running backend
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
 *		(vacuumFlags & excludeVacuum) is not zero.
 *
 * Note: the purpose of the limitXmin and excludeXmin0 parameters is to
 * allow skipping backends whose oldest live snapshot is no older than
 * some snapshot we have.  Since we examine the procarray with only shared
 * lock, there are race conditions: a backend could set its xmin just after
 * we look.  Indeed, on multiprocessors with weak memory ordering, the
 * other backend could have set its xmin *before* we look.	We know however
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
		volatile PGPROC *proc = arrayP->procs[index];

		if (proc == MyProc)
			continue;

		if (excludeVacuum & proc->vacuumFlags)
			continue;

		if (allDbs || proc->databaseId == MyDatabaseId)
		{
			/* Fetch xmin just once - might change on us */
			TransactionId pxmin = proc->xmin;

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
 * limitXmin is supplied as either latestRemovedXid, or InvalidTransactionId
 * in cases where we cannot accurately determine a value for latestRemovedXid.
 *
 * If limitXmin is InvalidTransactionId then we are forced to assume that
 * latest xid that might have caused a cleanup record will be
 * latestCompletedXid, so we set limitXmin to be latestCompletedXid instead.
 * We then skip any backends with xmin > limitXmin. This means that
 * cleanup records don't conflict with some recent snapshots.
 *
 * The reason for using latestCompletedxid is that we aren't certain which
 * of the xids in KnownAssignedXids are actually FATAL errors that did
 * not write abort records. In almost every case they won't be, but we
 * don't know that for certain. So we need to conflict with all current
 * snapshots whose xmin is less than latestCompletedXid to be safe. This
 * causes false positives in our assessment of which vxids conflict.
 *
 * By using exclusive lock we prevent new snapshots from being taken while
 * we work out which snapshots to conflict with. This protects those new
 * snapshots from also being included in our conflict list. 
 *
 * After the lock is released, we allow snapshots again. It is possible
 * that we arrive at a snapshot that is identical to one that we just
 * decided we should conflict with. This a case of false positives, not an
 * actual problem.
 * 
 * There are two cases: (1) if we were correct in using latestCompletedXid
 * then that means that all xids in the snapshot lower than that are FATAL
 * errors, so not xids that ever commit. We can make no visibility errors
 * if we allow such xids into the snapshot. (2) if we erred on the side of
 * caution and in fact the latestRemovedXid should have been earlier than
 * latestCompletedXid then we conflicted with a snapshot needlessly. Taking
 * another identical snapshot is OK, because the earlier conflicted
 * snapshot was a false positive.
 * 
 * In either case, a snapshot taken after conflict assessment will still be
 * valid and non-conflicting even if an identical snapshot that existed
 * before conflict assessment was assessed as conflicting.
 * 
 * If we allowed concurrent snapshots while we were deciding who to
 * conflict with we would need to include all concurrent snapshotters in
 * the conflict list as well. We'd have difficulty in working out exactly
 * who that was, so it is happier for all concerned if we take an exclusive
 * lock. Notice that we only hold that lock for as long as it takes to
 * make the conflict list, not for the whole duration of the conflict
 * resolution.
 * 
 * It also means that users waiting for a snapshot is a good thing, since
 * it is more likely that they will live longer after having waited. So it
 * is a benefit, not an oversight that we use exclusive lock here.
 *
 * We replace InvalidTransactionId with latestCompletedXid here because
 * this is the most convenient place to do that, while we hold ProcArrayLock.
 * The originator of the cleanup record wanted to avoid checking the value of
 * latestCompletedXid since doing so would be a performance issue during
 * normal running, so we check it essentially for free on the standby.
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
	 * If not first time through, get workspace to remember main XIDs in. We
	 * malloc it permanently to avoid repeated palloc/pfree overhead.
	 * Allow result space, remembering room for a terminator.
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

	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	/*
	 * If we don't know the TransactionId that created the conflict, set
	 * it to latestCompletedXid which is the latest possible value.
	 */
	if (!TransactionIdIsValid(limitXmin))
		limitXmin = ShmemVariableCache->latestCompletedXid;

	for (index = 0; index < arrayP->numProcs; index++)
	{
		volatile PGPROC *proc = arrayP->procs[index];

		/* Exclude prepared transactions */
		if (proc->pid == 0)
			continue;

		if (!OidIsValid(dbOid) ||
			proc->databaseId == dbOid)
		{
			/* Fetch xmin just once - can't change on us, but good coding */
			TransactionId pxmin = proc->xmin;

			/*
			 * We ignore an invalid pxmin because this means that backend
			 * has no snapshot and cannot get another one while we hold exclusive lock.
			 */
			if (TransactionIdIsValid(pxmin) && !TransactionIdFollows(pxmin, limitXmin))
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
	vxids[count].backendId = InvalidBackendId;
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
	ProcArrayStruct *arrayP = procArray;
	int			index;
	pid_t		pid = 0;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		VirtualTransactionId procvxid;
		PGPROC	   *proc = arrayP->procs[index];

		GET_VXID_FROM_PGPROC(procvxid, *proc);

		if (procvxid.backendId == vxid.backendId &&
			procvxid.localTransactionId == vxid.localTransactionId)
		{
			proc->recoveryConflictPending = true;
			pid = proc->pid;
			if (pid != 0)
			{
				/*
				 * Kill the pid if it's still here. If not, that's what we wanted
				 * so ignore any errors.
				 */
				(void) SendProcSignal(pid, sigmode, vxid.backendId);
			}
			break;
		}
	}

	LWLockRelease(ProcArrayLock);

	return pid;
}

/*
 * CountActiveBackends --- count backends (other than myself) that are in
 *		active transactions.  This is used as a heuristic to decide if
 *		a pre-XLOG-flush delay is worthwhile during commit.
 *
 * Do not count backends that are blocked waiting for locks, since they are
 * not going to get to run until someone else commits.
 */
int
CountActiveBackends(void)
{
	ProcArrayStruct *arrayP = procArray;
	int			count = 0;
	int			index;

	/*
	 * Note: for speed, we don't acquire ProcArrayLock.  This is a little bit
	 * bogus, but since we are only testing fields for zero or nonzero, it
	 * should be OK.  The result is only used for heuristic purposes anyway...
	 */
	for (index = 0; index < arrayP->numProcs; index++)
	{
		volatile PGPROC *proc = arrayP->procs[index];

		/*
		 * Since we're not holding a lock, need to check that the pointer is
		 * valid. Someone holding the lock could have incremented numProcs
		 * already, but not yet inserted a valid pointer to the array.
		 *
		 * If someone just decremented numProcs, 'proc' could also point to a
		 * PGPROC entry that's no longer in the array. It still points to a
		 * PGPROC struct, though, because freed PGPPROC entries just go to the
		 * free list and are recycled. Its contents are nonsense in that case,
		 * but that's acceptable for this function.
		 */
		if (proc == NULL)
			continue;

		if (proc == MyProc)
			continue;			/* do not count myself */
		if (proc->pid == 0)
			continue;			/* do not count prepared xacts */
		if (proc->xid == InvalidTransactionId)
			continue;			/* do not count if no XID assigned */
		if (proc->waitLock != NULL)
			continue;			/* do not count if blocked on a lock */
		count++;
	}

	return count;
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
		volatile PGPROC *proc = arrayP->procs[index];

		if (proc->pid == 0)
			continue;			/* do not count prepared xacts */
		if (proc->databaseId == databaseid)
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
	pid_t		pid = 0;

	/* tell all backends to die */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		volatile PGPROC *proc = arrayP->procs[index];

		if (databaseid == InvalidOid || proc->databaseId == databaseid)
		{
			VirtualTransactionId procvxid;

			GET_VXID_FROM_PGPROC(procvxid, *proc);

			proc->recoveryConflictPending = conflictPending;
			pid = proc->pid;
			if (pid != 0)
			{
				/*
				 * Kill the pid if it's still here. If not, that's what we wanted
				 * so ignore any errors.
				 */
				(void) SendProcSignal(pid, sigmode, procvxid.backendId);
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
		volatile PGPROC *proc = arrayP->procs[index];

		if (proc->pid == 0)
			continue;			/* do not count prepared xacts */
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
 * Returns TRUE if there are (still) other backends in the DB, FALSE if not.
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
			volatile PGPROC *proc = arrayP->procs[index];

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
				if ((proc->vacuumFlags & PROC_IS_AUTOVACUUM) &&
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


#define XidCacheRemove(i) \
	do { \
		MyProc->subxids.xids[i] = MyProc->subxids.xids[MyProc->subxids.nxids - 1]; \
		MyProc->subxids.nxids--; \
	} while (0)

/*
 * XidCacheRemoveRunningXids
 *
 * Remove a bunch of TransactionIds from the list of known-running
 * subtransactions for my backend.	Both the specified xid and those in
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

	Assert(TransactionIdIsValid(xid));

	/*
	 * We must hold ProcArrayLock exclusively in order to remove transactions
	 * from the PGPROC array.  (See src/backend/access/transam/README.)  It's
	 * possible this could be relaxed since we know this routine is only used
	 * to abort subtransactions, but pending closer analysis we'd best be
	 * conservative.
	 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	/*
	 * Under normal circumstances xid and xids[] will be in increasing order,
	 * as will be the entries in subxids.  Scan backwards to avoid O(N^2)
	 * behavior when removing a lot of xids.
	 */
	for (i = nxids - 1; i >= 0; i--)
	{
		TransactionId anxid = xids[i];

		for (j = MyProc->subxids.nxids - 1; j >= 0; j--)
		{
			if (TransactionIdEquals(MyProc->subxids.xids[j], anxid))
			{
				XidCacheRemove(j);
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
		if (j < 0 && !MyProc->subxids.overflowed)
			elog(WARNING, "did not find subXID %u in MyProc", anxid);
	}

	for (j = MyProc->subxids.nxids - 1; j >= 0; j--)
	{
		if (TransactionIdEquals(MyProc->subxids.xids[j], xid))
		{
			XidCacheRemove(j);
			break;
		}
	}
	/* Ordinarily we should have found it, unless the cache has overflowed */
	if (j < 0 && !MyProc->subxids.overflowed)
		elog(WARNING, "did not find subXID %u in MyProc", xid);

	/* Also advance global latestCompletedXid while holding the lock */
	if (TransactionIdPrecedes(ShmemVariableCache->latestCompletedXid,
							  latestXid))
		ShmemVariableCache->latestCompletedXid = latestXid;

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
			"XidCache: xmin: %ld, known: %ld, myxact: %ld, latest: %ld, mainxid: %ld, childxid: %ld, nooflo: %ld, slow: %ld\n",
			xc_by_recent_xmin,
			xc_by_known_xact,
			xc_by_my_xact,
			xc_by_latest_xid,
			xc_by_main_xid,
			xc_by_child_xid,
			xc_no_overflow,
			xc_slow_answer);
}

#endif   /* XIDCACHE_DEBUG */

/* ----------------------------------------------
 * 		KnownAssignedTransactions sub-module
 * ----------------------------------------------
 */

/*
 * In Hot Standby mode, we maintain a list of transactions that are (or were)
 * running in the master at the current point in WAL.
 *
 * RecordKnownAssignedTransactionIds() should be run for *every* WAL record
 * type apart from XLOG_XACT_RUNNING_XACTS, since that initialises the first
 * snapshot so that RecordKnownAssignedTransactionIds() can be callsed. Uses
 * local variables, so should only be called by Startup process.
 *
 * We record all xids that we know have been assigned. That includes
 * all the xids on the WAL record, plus all unobserved xids that
 * we can deduce have been assigned. We can deduce the existence of
 * unobserved xids because we know xids are in sequence, with no gaps.
 *
 * During recovery we do not fret too much about the distinction between
 * top-level xids and subtransaction xids. We hold both together in
 * a hash table called KnownAssignedXids. In backends, this is copied into
 * snapshots in GetSnapshotData(), taking advantage
 * of the fact that XidInMVCCSnapshot() doesn't care about the distinction
 * either. Subtransaction xids are effectively treated as top-level xids
 * and in the typical case pg_subtrans is *not* maintained (and that
 * does not effect visibility).
 *
 * KnownAssignedXids expands as new xids are observed or inferred, and
 * contracts when transaction completion records arrive. We have room in a
 * snapshot to hold maxProcs * (1 + PGPROC_MAX_CACHED_SUBXIDS) xids, so
 * every transaction must report their subtransaction xids in a special
 * WAL assignment record every PGPROC_MAX_CACHED_SUBXIDS. This allows us
 * to remove the subtransaction xids and update pg_subtrans instead. Snapshots
 * are still correct yet we don't overflow SnapshotData structure. When we do
 * this we need
 * to keep track of which xids caused the snapshot to overflow. We do that
 * by simply tracking the lastOverflowedXid - if it is within the bounds of
 * the KnownAssignedXids then we know the snapshot overflowed. (Note that
 * subxid overflow occurs on primary when 65th subxid arrives, whereas on
 * standby it occurs when 64th subxid arrives - that is not an error).
 *
 * Should FATAL errors result in a backend on primary disappearing before
 * it can write an abort record then we just leave those xids in
 * KnownAssignedXids. They actually aborted but we think they were running;
 * the distinction is irrelevant because either way any changes done by the
 * transaction are not visible to backends in the standby.
 * We prune KnownAssignedXids when XLOG_XACT_RUNNING_XACTS arrives, to
 * ensure we do not overflow.
 *
 * If we are in STANDBY_SNAPSHOT_PENDING state, then we may try to remove
 * xids that are not present.
 */
void
RecordKnownAssignedTransactionIds(TransactionId xid)
{
	/*
	 * Skip processing if the current snapshot is not initialized.
	 */
	if (standbyState < STANDBY_SNAPSHOT_PENDING)
		return;

	/*
	 * We can see WAL records before the running-xacts snapshot that
	 * contain XIDs that are not in the running-xacts snapshot, but that we
	 * know to have finished before the running-xacts snapshot was taken.
	 * Don't waste precious shared memory by keeping them in the hash table.
	 *
	 * We can also see WAL records before the running-xacts snapshot that
	 * contain XIDs that are not in the running-xacts snapshot for a different
	 * reason: the transaction started *after* the running-xacts snapshot
	 * was taken, but before it was written to WAL. We must be careful to
	 * not ignore such XIDs. Because such a transaction started after the
	 * running-xacts snapshot was taken, it must have an XID larger than
	 * the oldest XID according to the running-xacts snapshot.
	 */
	if (TransactionIdPrecedes(xid, snapshotOldestActiveXid))
		return;

	ereport(trace_recovery(DEBUG4),
				(errmsg("record known xact %u latestObservedXid %u",
							xid, latestObservedXid)));

	/*
	 * When a newly observed xid arrives, it is frequently the case
	 * that it is *not* the next xid in sequence. When this occurs, we
	 * must treat the intervening xids as running also.
	 */
	if (TransactionIdFollows(xid, latestObservedXid))
	{
		TransactionId	next_expected_xid = latestObservedXid;
		TransactionIdAdvance(next_expected_xid);

		/*
		 * Locking requirement is currently higher than for xid assignment
		 * in normal running. However, we only get called here for new
		 * high xids - so on a multi-processor where it is common that xids
		 * arrive out of order the average number of locks per assignment
		 * will actually reduce. So not too worried about this locking.
		 *
		 * XXX It does seem possible that we could add a whole range
		 * of numbers atomically to KnownAssignedXids, if we use a sorted
		 * list for KnownAssignedXids. But that design also increases the
		 * length of time we hold lock when we process commits/aborts, so
		 * on balance don't worry about this.
		 */
		LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

		while (TransactionIdPrecedesOrEquals(next_expected_xid, xid))
		{
			if (TransactionIdPrecedes(next_expected_xid, xid))
				ereport(trace_recovery(DEBUG4),
						(errmsg("recording unobserved xid %u (latestObservedXid %u)",
									next_expected_xid, latestObservedXid)));
			KnownAssignedXidsAdd(&next_expected_xid, 1);

			/*
			 * Extend clog and subtrans like we do in GetNewTransactionId()
			 * during normal operation
			 */
			ExtendCLOG(next_expected_xid);
			ExtendSUBTRANS(next_expected_xid);

			TransactionIdAdvance(next_expected_xid);
		}

		LWLockRelease(ProcArrayLock);

		latestObservedXid = xid;
	}

	/* nextXid must be beyond any observed xid */
	if (TransactionIdFollowsOrEquals(latestObservedXid,
									 ShmemVariableCache->nextXid))
	{
		ShmemVariableCache->nextXid = latestObservedXid;
		TransactionIdAdvance(ShmemVariableCache->nextXid);
	}
}

void
ExpireTreeKnownAssignedTransactionIds(TransactionId xid, int nsubxids,
									  TransactionId *subxids)
{
	int			i;
	TransactionId max_xid;

	if (standbyState == STANDBY_DISABLED)
		return;

	max_xid = TransactionIdLatest(xid, nsubxids, subxids);

	/*
	 * Uses same locking as transaction commit
	 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	if (TransactionIdIsValid(xid))
		KnownAssignedXidsRemove(xid);
	for (i = 0; i < nsubxids; i++)
		KnownAssignedXidsRemove(subxids[i]);

	/* Like in ProcArrayRemove, advance latestCompletedXid */
	if (TransactionIdFollowsOrEquals(max_xid,
									 ShmemVariableCache->latestCompletedXid))
		ShmemVariableCache->latestCompletedXid = max_xid;

	LWLockRelease(ProcArrayLock);
}

void
ExpireAllKnownAssignedTransactionIds(void)
{
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
	KnownAssignedXidsRemoveMany(InvalidTransactionId, false);
	LWLockRelease(ProcArrayLock);
}

void
ExpireOldKnownAssignedTransactionIds(TransactionId xid)
{
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
	KnownAssignedXidsRemoveMany(xid, true);
	LWLockRelease(ProcArrayLock);
}

/*
 * Private module functions to manipulate KnownAssignedXids
 *
 * There are 3 main users of the KnownAssignedXids data structure:
 *
 *   * backends taking snapshots
 *   * startup process adding new knownassigned xids
 *   * startup process removing xids as transactions end
 *
 * If we make KnownAssignedXids a simple sorted array then the first two
 * operations are fast, but the last one is at least O(N). If we make
 * KnownAssignedXids a hash table then the last two operations are fast,
 * though we have to do more work at snapshot time. Doing more work at
 * commit could slow down taking snapshots anyway because of lwlock
 * contention. Scanning the hash table is O(N) on the max size of the array,
 * so performs poorly in comparison when we have very low numbers of
 * write transactions to process. But at least it is constant overhead
 * and a sequential memory scan will utilise hardware memory readahead
 * to give much improved performance. In any case the emphasis must be on
 * having the standby process changes quickly so that it can provide
 * high availability. So we choose to implement as a hash table.
 */

/*
 * Add xids into KnownAssignedXids.
 *
 * Must be called while holding ProcArrayLock in Exclusive mode
 */
static void
KnownAssignedXidsAdd(TransactionId *xids, int nxids)
{
	TransactionId *result;
	bool found;
	int i;

	for (i = 0; i < nxids; i++)
	{
		Assert(TransactionIdIsValid(xids[i]));

		elog(trace_recovery(DEBUG4), "adding KnownAssignedXid %u", xids[i]);

		procArray->numKnownAssignedXids++;
		if (procArray->numKnownAssignedXids > procArray->maxKnownAssignedXids)
		{
			KnownAssignedXidsDisplay(LOG);
			LWLockRelease(ProcArrayLock);
			ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("too many KnownAssignedXids")));
		}

		result = (TransactionId *) hash_search(KnownAssignedXidsHash, &xids[i], HASH_ENTER,
												&found);

		if (!result)
		{
			LWLockRelease(ProcArrayLock);
			ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of shared memory")));
		}

		if (found)
		{
			KnownAssignedXidsDisplay(LOG);
			LWLockRelease(ProcArrayLock);
			elog(ERROR, "found duplicate KnownAssignedXid %u", xids[i]);
		}
	}
}

/*
 * Is an xid present in KnownAssignedXids?
 *
 * Must be called while holding ProcArrayLock in shared mode
 */
static bool
KnownAssignedXidsExist(TransactionId xid)
{
	bool found;
	(void) hash_search(KnownAssignedXidsHash, &xid, HASH_FIND, &found);
	return found;
}

/*
 * Remove one xid from anywhere in KnownAssignedXids.
 *
 * Must be called while holding ProcArrayLock in Exclusive mode
 */
static void
KnownAssignedXidsRemove(TransactionId xid)
{
	bool found;

	Assert(TransactionIdIsValid(xid));

	elog(trace_recovery(DEBUG4), "remove KnownAssignedXid %u", xid);

	(void) hash_search(KnownAssignedXidsHash, &xid, HASH_REMOVE, &found);

	if (found)
		procArray->numKnownAssignedXids--;
	Assert(procArray->numKnownAssignedXids >= 0);

	/*
	 * We can fail to find an xid if the xid came from a subtransaction
	 * that aborts, though the xid hadn't yet been reported and no WAL records
	 * have been written using the subxid. In that case the abort record will
	 * contain that subxid and we haven't seen it before.
	 *
	 * If we fail to find it for other reasons it might be a problem, but
	 * it isn't much use to log that it happened, since we can't divine much
	 * from just an isolated xid value.
	 */
}

/*
 * KnownAssignedXidsGet - Get an array of xids by scanning KnownAssignedXids.
 * We filter out anything higher than xmax.
 *
 * Must be called while holding ProcArrayLock (in shared mode)
 */
static int
KnownAssignedXidsGet(TransactionId *xarray, TransactionId xmax)
{
	TransactionId xtmp = InvalidTransactionId;

	return KnownAssignedXidsGetAndSetXmin(xarray, &xtmp, xmax);
}

/*
 * KnownAssignedXidsGetAndSetXmin - as KnownAssignedXidsGet, plus we reduce *xmin
 * to the lowest xid value seen if not already lower.
 *
 * Must be called while holding ProcArrayLock (in shared mode)
 */
static int
KnownAssignedXidsGetAndSetXmin(TransactionId *xarray, TransactionId *xmin,
					 TransactionId xmax)
{
	HASH_SEQ_STATUS status;
	TransactionId *knownXid;
	int			count = 0;

	hash_seq_init(&status, KnownAssignedXidsHash);
	while ((knownXid = (TransactionId *) hash_seq_search(&status)) != NULL)
	{
		/*
		 * Filter out anything higher than xmax
		 */
		if (TransactionIdPrecedes(xmax, *knownXid))
			continue;

		*xarray = *knownXid;
		xarray++;
		count++;

		/* update xmin if required */
		if (TransactionIdPrecedes(*knownXid, *xmin))
			*xmin = *knownXid;
	}

	return count;
}

/*
 * Prune KnownAssignedXids up to, but *not* including xid. If xid is invalid
 * then clear the whole table.
 *
 * Must be called while holding ProcArrayLock in Exclusive mode.
 */
static void
KnownAssignedXidsRemoveMany(TransactionId xid, bool keepPreparedXacts)
{
	TransactionId	*knownXid;
	HASH_SEQ_STATUS status;

	if (TransactionIdIsValid(xid))
		elog(trace_recovery(DEBUG4), "prune KnownAssignedXids to %u", xid);
	else
		elog(trace_recovery(DEBUG4), "removing all KnownAssignedXids");

	hash_seq_init(&status, KnownAssignedXidsHash);
	while ((knownXid = (TransactionId *) hash_seq_search(&status)) != NULL)
	{
		TransactionId removeXid = *knownXid;
		bool found;

		if (!TransactionIdIsValid(xid) || TransactionIdPrecedes(removeXid, xid))
		{
			if (keepPreparedXacts && StandbyTransactionIdIsPrepared(xid))
				continue;
			else
			{
				(void) hash_search(KnownAssignedXidsHash, &removeXid,
								   HASH_REMOVE, &found);
				if (found)
					procArray->numKnownAssignedXids--;
				Assert(procArray->numKnownAssignedXids >= 0);
			}
		}
	}
}

/*
 * Display KnownAssignedXids to provide debug trail
 *
 * Must be called while holding ProcArrayLock (in shared mode)
 */
static void
KnownAssignedXidsDisplay(int trace_level)
{
	HASH_SEQ_STATUS status;
	TransactionId *knownXid;
	StringInfoData buf;
	TransactionId   *xids;
	int				nxids;
	int				i;

	xids = palloc(sizeof(TransactionId) * TOTAL_MAX_CACHED_SUBXIDS);
	nxids = 0;

	hash_seq_init(&status, KnownAssignedXidsHash);
	while ((knownXid = (TransactionId *) hash_seq_search(&status)) != NULL)
		xids[nxids++] = *knownXid;

	qsort(xids, nxids, sizeof(TransactionId), xidComparator);

	initStringInfo(&buf);

	for (i = 0; i < nxids; i++)
		appendStringInfo(&buf, "%u ", xids[i]);

	elog(trace_level, "%d KnownAssignedXids %s", nxids, buf.data);

	pfree(buf.data);
}
