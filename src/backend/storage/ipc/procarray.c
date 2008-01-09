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
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/ipc/procarray.c,v 1.40 2008/01/09 21:52:36 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>

#include "access/subtrans.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/twophase.h"
#include "miscadmin.h"
#include "storage/procarray.h"
#include "utils/tqual.h"


/* Our shared memory area */
typedef struct ProcArrayStruct
{
	int			numProcs;		/* number of valid procs entries */
	int			maxProcs;		/* allocated size of procs array */

	/*
	 * We declare procs[] as 1 entry because C wants a fixed-size array, but
	 * actually it is maxProcs entries long.
	 */
	PGPROC	   *procs[1];		/* VARIABLE LENGTH ARRAY */
} ProcArrayStruct;

static ProcArrayStruct *procArray;


#ifdef XIDCACHE_DEBUG

/* counters for XidCache measurement */
static long xc_by_recent_xmin = 0;
static long xc_by_my_xact = 0;
static long xc_by_latest_xid = 0;
static long xc_by_main_xid = 0;
static long xc_by_child_xid = 0;
static long xc_no_overflow = 0;
static long xc_slow_answer = 0;

#define xc_by_recent_xmin_inc()		(xc_by_recent_xmin++)
#define xc_by_my_xact_inc()			(xc_by_my_xact++)
#define xc_by_latest_xid_inc()		(xc_by_latest_xid++)
#define xc_by_main_xid_inc()		(xc_by_main_xid++)
#define xc_by_child_xid_inc()		(xc_by_child_xid++)
#define xc_no_overflow_inc()		(xc_no_overflow++)
#define xc_slow_answer_inc()		(xc_slow_answer++)

static void DisplayXidCache(void);
#else							/* !XIDCACHE_DEBUG */

#define xc_by_recent_xmin_inc()		((void) 0)
#define xc_by_my_xact_inc()			((void) 0)
#define xc_by_latest_xid_inc()		((void) 0)
#define xc_by_main_xid_inc()		((void) 0)
#define xc_by_child_xid_inc()		((void) 0)
#define xc_no_overflow_inc()		((void) 0)
#define xc_slow_answer_inc()		((void) 0)
#endif   /* XIDCACHE_DEBUG */


/*
 * Report shared-memory space needed by CreateSharedProcArray.
 */
Size
ProcArrayShmemSize(void)
{
	Size		size;

	size = offsetof(ProcArrayStruct, procs);
	size = add_size(size, mul_size(sizeof(PGPROC *),
								 add_size(MaxBackends, max_prepared_xacts)));

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
		ShmemInitStruct("Proc Array", ProcArrayShmemSize(), &found);

	if (!found)
	{
		/*
		 * We're the first - initialize.
		 */
		procArray->numProcs = 0;
		procArray->maxProcs = MaxBackends + max_prepared_xacts;
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

	/* redundant, but just in case */
	proc->vacuumFlags &= ~PROC_VACUUM_STATE_MASK;
	proc->inCommit = false;

	/* Clear the subtransaction-XID cache too */
	proc->subxids.nxids = 0;
	proc->subxids.overflowed = false;
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
		xids = (TransactionId *)
			malloc(arrayP->maxProcs * sizeof(TransactionId));
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

	LWLockRelease(ProcArrayLock);

	/*
	 * If none of the relevant caches overflowed, we know the Xid is not
	 * running without looking at pg_subtrans.
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
 * will need to be done to determine what's running (see XidInMVCCSnapshot()
 * in tqual.c).
 *
 * We also update the following backend-global variables:
 *		TransactionXmin: the oldest xmin of any snapshot in use in the
 *			current transaction (this is the same as MyProc->xmin).  This
 *			is just the xmin computed for the first, serializable snapshot.
 *		RecentXmin: the xmin computed for the most recent snapshot.  XIDs
 *			older than this are known not running any more.
 *		RecentGlobalXmin: the global xmin (oldest TransactionXmin across all
 *			running transactions, except those running LAZY VACUUM).  This is
 *			the same computation done by GetOldestXmin(true, true).
 */
Snapshot
GetSnapshotData(Snapshot snapshot, bool serializable)
{
	ProcArrayStruct *arrayP = procArray;
	TransactionId xmin;
	TransactionId xmax;
	TransactionId globalxmin;
	int			index;
	int			count = 0;
	int			subcount = 0;

	Assert(snapshot != NULL);

	/* Serializable snapshot must be computed before any other... */
	Assert(serializable ?
		   !TransactionIdIsValid(MyProc->xmin) :
		   TransactionIdIsValid(MyProc->xmin));

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
		 * First call for this snapshot
		 */
		snapshot->xip = (TransactionId *)
			malloc(arrayP->maxProcs * sizeof(TransactionId));
		if (snapshot->xip == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		Assert(snapshot->subxip == NULL);
		snapshot->subxip = (TransactionId *)
			malloc(arrayP->maxProcs * PGPROC_MAX_CACHED_SUBXIDS * sizeof(TransactionId));
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
		if (subcount >= 0 && proc != MyProc)
		{
			if (proc->subxids.overflowed)
				subcount = -1;	/* overflowed */
			else
			{
				int			nxids = proc->subxids.nxids;

				if (nxids > 0)
				{
					memcpy(snapshot->subxip + subcount,
						   (void *) proc->subxids.xids,
						   nxids * sizeof(TransactionId));
					subcount += nxids;
				}
			}
		}
	}

	if (serializable)
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
	RecentGlobalXmin = globalxmin;
	RecentXmin = xmin;

	snapshot->xmin = xmin;
	snapshot->xmax = xmax;
	snapshot->xcnt = count;
	snapshot->subxcnt = subcount;

	snapshot->curcid = GetCurrentCommandId(false);

	return snapshot;
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
 * The array is palloc'd and is terminated with an invalid VXID.
 *
 * If limitXmin is not InvalidTransactionId, we skip any backends
 * with xmin >= limitXmin.	If allDbs is false, we skip backends attached
 * to other databases.  If excludeVacuum isn't zero, we skip processes for
 * which (excludeVacuum & vacuumFlags) is not zero.  Also, our own process
 * is always skipped.
 */
VirtualTransactionId *
GetCurrentVirtualXIDs(TransactionId limitXmin, bool allDbs, int excludeVacuum)
{
	VirtualTransactionId *vxids;
	ProcArrayStruct *arrayP = procArray;
	int			count = 0;
	int			index;

	/* allocate result space with room for a terminator */
	vxids = (VirtualTransactionId *)
		palloc(sizeof(VirtualTransactionId) * (arrayP->maxProcs + 1));

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
			/* Fetch xmin just once - might change on us? */
			TransactionId pxmin = proc->xmin;

			/*
			 * Note that InvalidTransactionId precedes all other XIDs, so a
			 * proc that hasn't set xmin yet will always be included.
			 */
			if (!TransactionIdIsValid(limitXmin) ||
				TransactionIdPrecedes(pxmin, limitXmin))
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
 * CheckOtherDBBackends -- check for other backends running in the given DB
 *
 * If there are other backends in the DB, we will wait a maximum of 5 seconds
 * for them to exit.  Autovacuum backends are encouraged to exit early by
 * sending them SIGTERM, but normal user backends are just waited for.
 *
 * The current backend is always ignored; it is caller's responsibility to
 * check whether the current backend uses the given DB, if it's important.
 *
 * Returns TRUE if there are (still) other backends in the DB, FALSE if not.
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
CheckOtherDBBackends(Oid databaseId)
{
	ProcArrayStruct *arrayP = procArray;
	int			tries;

	/* 50 tries with 100ms sleep between tries makes 5 sec total wait */
	for (tries = 0; tries < 50; tries++)
	{
		bool		found = false;
		int			index;

		CHECK_FOR_INTERRUPTS();

		LWLockAcquire(ProcArrayLock, LW_SHARED);

		for (index = 0; index < arrayP->numProcs; index++)
		{
			volatile PGPROC *proc = arrayP->procs[index];

			if (proc->databaseId != databaseId)
				continue;
			if (proc == MyProc)
				continue;

			found = true;

			if (proc->vacuumFlags & PROC_IS_AUTOVACUUM)
			{
				/* an autovacuum --- send it SIGTERM before sleeping */
				int			autopid = proc->pid;

				/*
				 * It's a bit awkward to release ProcArrayLock within the
				 * loop, but we'd probably better do so before issuing kill().
				 * We have no idea what might block kill() inside the
				 * kernel...
				 */
				LWLockRelease(ProcArrayLock);

				(void) kill(autopid, SIGTERM);	/* ignore any error */

				break;
			}
			else
			{
				LWLockRelease(ProcArrayLock);
				break;
			}
		}

		/* if found is set, we released the lock within the loop body */
		if (!found)
		{
			LWLockRelease(ProcArrayLock);
			return false;		/* no conflicting backends, so done */
		}

		/* else sleep and try again */
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
			"XidCache: xmin: %ld, myxact: %ld, latest: %ld, mainxid: %ld, childxid: %ld, nooflo: %ld, slow: %ld\n",
			xc_by_recent_xmin,
			xc_by_my_xact,
			xc_by_latest_xid,
			xc_by_main_xid,
			xc_by_child_xid,
			xc_no_overflow,
			xc_slow_answer);
}

#endif   /* XIDCACHE_DEBUG */
