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
 * See notes in GetSnapshotData.
 *
 * The process array now also includes PGPROC structures representing
 * prepared transactions.  The xid and subxids fields of these are valid,
 * as is the procLocks list.  They can be distinguished from regular backend
 * PGPROCs at need by checking for pid == 0.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/ipc/procarray.c,v 1.7.2.1 2005/11/22 18:23:18 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/subtrans.h"
#include "access/twophase.h"
#include "miscadmin.h"
#include "storage/proc.h"
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
static long xc_by_main_xid = 0;
static long xc_by_child_xid = 0;
static long xc_slow_answer = 0;

#define xc_by_recent_xmin_inc()		(xc_by_recent_xmin++)
#define xc_by_main_xid_inc()		(xc_by_main_xid++)
#define xc_by_child_xid_inc()		(xc_by_child_xid++)
#define xc_slow_answer_inc()		(xc_slow_answer++)

static void DisplayXidCache(void);
#else							/* !XIDCACHE_DEBUG */

#define xc_by_recent_xmin_inc()		((void) 0)
#define xc_by_main_xid_inc()		((void) 0)
#define xc_by_child_xid_inc()		((void) 0)
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
 */
void
ProcArrayRemove(PGPROC *proc)
{
	ProcArrayStruct *arrayP = procArray;
	int			index;

#ifdef XIDCACHE_DEBUG
	/* dump stats at backend shutdown, but not prepared-xact end */
	if (proc->pid != 0)
		DisplayXidCache();
#endif

	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

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
 * TransactionIdIsInProgress -- is given transaction running in some backend
 *
 * There are three possibilities for finding a running transaction:
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
	bool		result = false;
	ProcArrayStruct *arrayP = procArray;
	int			i,
				j;
	int			nxids = 0;
	TransactionId *xids;
	TransactionId topxid;
	bool		locked;

	/*
	 * Don't bother checking a transaction older than RecentXmin; it could not
	 * possibly still be running.
	 */
	if (TransactionIdPrecedes(xid, RecentXmin))
	{
		xc_by_recent_xmin_inc();
		return false;
	}

	/* Get workspace to remember main XIDs in */
	xids = (TransactionId *) palloc(sizeof(TransactionId) * arrayP->maxProcs);

	LWLockAcquire(ProcArrayLock, LW_SHARED);
	locked = true;

	for (i = 0; i < arrayP->numProcs; i++)
	{
		PGPROC	   *proc = arrayP->procs[i];

		/* Fetch xid just once - see GetNewTransactionId */
		TransactionId pxid = proc->xid;

		if (!TransactionIdIsValid(pxid))
			continue;

		/*
		 * Step 1: check the main Xid
		 */
		if (TransactionIdEquals(pxid, xid))
		{
			xc_by_main_xid_inc();
			result = true;
			goto result_known;
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
				xc_by_child_xid_inc();
				result = true;
				goto result_known;
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
	locked = false;

	/*
	 * If none of the relevant caches overflowed, we know the Xid is not
	 * running without looking at pg_subtrans.
	 */
	if (nxids == 0)
		goto result_known;

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
		goto result_known;

	/*
	 * It isn't aborted, so check whether the transaction tree it belongs to
	 * is still running (or, more precisely, whether it was running when this
	 * routine started -- note that we already released ProcArrayLock).
	 */
	topxid = SubTransGetTopmostTransaction(xid);
	Assert(TransactionIdIsValid(topxid));
	if (!TransactionIdEquals(topxid, xid))
	{
		for (i = 0; i < nxids; i++)
		{
			if (TransactionIdEquals(xids[i], topxid))
			{
				result = true;
				break;
			}
		}
	}

result_known:
	if (locked)
		LWLockRelease(ProcArrayLock);

	pfree(xids);

	return result;
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
		PGPROC	   *proc = arrayP->procs[i];

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
 * This is used by VACUUM to decide which deleted tuples must be preserved
 * in a table.	allDbs = TRUE is needed for shared relations, but allDbs =
 * FALSE is sufficient for non-shared relations, since only backends in my
 * own database could ever see the tuples in them.
 *
 * This is also used to determine where to truncate pg_subtrans.  allDbs
 * must be TRUE for that case.
 *
 * Note: we include the currently running xids in the set of considered xids.
 * This ensures that if a just-started xact has not yet set its snapshot,
 * when it does set the snapshot it cannot set xmin less than what we compute.
 */
TransactionId
GetOldestXmin(bool allDbs)
{
	ProcArrayStruct *arrayP = procArray;
	TransactionId result;
	int			index;

	/*
	 * Normally we start the min() calculation with our own XID.  But if
	 * called by checkpointer, we will not be inside a transaction, so use
	 * next XID as starting point for min() calculation.  (Note that if there
	 * are no xacts running at all, that will be the subtrans truncation
	 * point!)
	 */
	if (IsTransactionState())
		result = GetTopTransactionId();
	else
		result = ReadNewTransactionId();

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		PGPROC	   *proc = arrayP->procs[index];

		if (allDbs || proc->databaseId == MyDatabaseId)
		{
			/* Fetch xid just once - see GetNewTransactionId */
			TransactionId xid = proc->xid;

			if (TransactionIdIsNormal(xid))
			{
				if (TransactionIdPrecedes(xid, result))
					result = xid;
				xid = proc->xmin;
				if (TransactionIdIsNormal(xid))
					if (TransactionIdPrecedes(xid, result))
						result = xid;
			}
		}
	}

	LWLockRelease(ProcArrayLock);

	return result;
}

/*----------
 * GetSnapshotData -- returns information about running transactions.
 *
 * The returned snapshot includes xmin (lowest still-running xact ID),
 * xmax (next xact ID to be assigned), and a list of running xact IDs
 * in the range xmin <= xid < xmax.  It is used as follows:
 *		All xact IDs < xmin are considered finished.
 *		All xact IDs >= xmax are considered still running.
 *		For an xact ID xmin <= xid < xmax, consult list to see whether
 *		it is considered running or not.
 * This ensures that the set of transactions seen as "running" by the
 * current xact will not change after it takes the snapshot.
 *
 * Note that only top-level XIDs are included in the snapshot.	We can
 * still apply the xmin and xmax limits to subtransaction XIDs, but we
 * need to work a bit harder to see if XIDs in [xmin..xmax) are running.
 *
 * We also update the following backend-global variables:
 *		TransactionXmin: the oldest xmin of any snapshot in use in the
 *			current transaction (this is the same as MyProc->xmin).  This
 *			is just the xmin computed for the first, serializable snapshot.
 *		RecentXmin: the xmin computed for the most recent snapshot.  XIDs
 *			older than this are known not running any more.
 *		RecentGlobalXmin: the global xmin (oldest TransactionXmin across all
 *			running transactions).	This is the same computation done by
 *			GetOldestXmin(TRUE).
 *----------
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

	Assert(snapshot != NULL);

	/* Serializable snapshot must be computed before any other... */
	Assert(serializable ?
		   !TransactionIdIsValid(MyProc->xmin) :
		   TransactionIdIsValid(MyProc->xmin));

	/*
	 * Allocating space for maxProcs xids is usually overkill; numProcs would
	 * be sufficient.  But it seems better to do the malloc while not holding
	 * the lock, so we can't look at numProcs.
	 *
	 * This does open a possibility for avoiding repeated malloc/free: since
	 * maxProcs does not change at runtime, we can simply reuse the previous
	 * xip array if any.  (This relies on the fact that all callers pass
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
	}

	globalxmin = xmin = GetTopTransactionId();

	/*
	 * If we are going to set MyProc->xmin then we'd better get exclusive
	 * lock; if not, this is a read-only operation so it can be shared.
	 */
	LWLockAcquire(ProcArrayLock, serializable ? LW_EXCLUSIVE : LW_SHARED);

	/*--------------------
	 * Unfortunately, we have to call ReadNewTransactionId() after acquiring
	 * ProcArrayLock above.  It's not good because ReadNewTransactionId() does
	 * LWLockAcquire(XidGenLock), but *necessary*.	We need to be sure that
	 * no transactions exit the set of currently-running transactions
	 * between the time we fetch xmax and the time we finish building our
	 * snapshot.  Otherwise we could have a situation like this:
	 *
	 *		1. Tx Old is running (in Read Committed mode).
	 *		2. Tx S reads new transaction ID into xmax, then
	 *		   is swapped out before acquiring ProcArrayLock.
	 *		3. Tx New gets new transaction ID (>= S' xmax),
	 *		   makes changes and commits.
	 *		4. Tx Old changes some row R changed by Tx New and commits.
	 *		5. Tx S finishes getting its snapshot data.  It sees Tx Old as
	 *		   done, but sees Tx New as still running (since New >= xmax).
	 *
	 * Now S will see R changed by both Tx Old and Tx New, *but* does not
	 * see other changes made by Tx New.  If S is supposed to be in
	 * Serializable mode, this is wrong.
	 *
	 * By locking ProcArrayLock before we read xmax, we ensure that TX Old
	 * cannot exit the set of running transactions seen by Tx S.  Therefore
	 * both Old and New will be seen as still running => no inconsistency.
	 *--------------------
	 */

	xmax = ReadNewTransactionId();

	for (index = 0; index < arrayP->numProcs; index++)
	{
		PGPROC	   *proc = arrayP->procs[index];

		/* Fetch xid just once - see GetNewTransactionId */
		TransactionId xid = proc->xid;

		/*
		 * Ignore my own proc (dealt with my xid above), procs not running a
		 * transaction, and xacts started since we read the next transaction
		 * ID.	There's no need to store XIDs above what we got from
		 * ReadNewTransactionId, since we'll treat them as running anyway.  We
		 * also assume that such xacts can't compute an xmin older than ours,
		 * so they needn't be considered in computing globalxmin.
		 */
		if (proc == MyProc ||
			!TransactionIdIsNormal(xid) ||
			TransactionIdFollowsOrEquals(xid, xmax))
			continue;

		if (TransactionIdPrecedes(xid, xmin))
			xmin = xid;
		snapshot->xip[count] = xid;
		count++;

		/* Update globalxmin to be the smallest valid xmin */
		xid = proc->xmin;
		if (TransactionIdIsNormal(xid))
			if (TransactionIdPrecedes(xid, globalxmin))
				globalxmin = xid;
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

	snapshot->curcid = GetCurrentCommandId();

	return snapshot;
}

/*
 * DatabaseHasActiveBackends -- are there any backends running in the given DB
 *
 * If 'ignoreMyself' is TRUE, ignore this particular backend while checking
 * for backends in the target database.
 *
 * This function is used to interlock DROP DATABASE against there being
 * any active backends in the target DB --- dropping the DB while active
 * backends remain would be a Bad Thing.  Note that we cannot detect here
 * the possibility of a newly-started backend that is trying to connect
 * to the doomed database, so additional interlocking is needed during
 * backend startup.
 */
bool
DatabaseHasActiveBackends(Oid databaseId, bool ignoreMyself)
{
	bool		result = false;
	ProcArrayStruct *arrayP = procArray;
	int			index;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		PGPROC	   *proc = arrayP->procs[index];

		if (proc->databaseId == databaseId)
		{
			if (ignoreMyself && proc == MyProc)
				continue;

			result = true;
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
		PGPROC	   *proc = arrayP->procs[index];

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
		PGPROC	   *proc = arrayP->procs[index];

		if (proc == MyProc)
			continue;			/* do not count myself */
		if (proc->pid == 0)
			continue;			/* do not count prepared xacts */
		if (proc->xid == InvalidTransactionId)
			continue;			/* do not count if not in a transaction */
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
		PGPROC	   *proc = arrayP->procs[index];

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
		PGPROC	   *proc = arrayP->procs[index];

		if (proc->pid == 0)
			continue;			/* do not count prepared xacts */
		if (proc->roleId == roleid)
			count++;
	}

	LWLockRelease(ProcArrayLock);

	return count;
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
 */
void
XidCacheRemoveRunningXids(TransactionId xid, int nxids, TransactionId *xids)
{
	int			i,
				j;

	Assert(!TransactionIdEquals(xid, InvalidTransactionId));

	/*
	 * We must hold ProcArrayLock exclusively in order to remove transactions
	 * from the PGPROC array.  (See notes in GetSnapshotData.)	It's possible
	 * this could be relaxed since we know this routine is only used to abort
	 * subtransactions, but pending closer analysis we'd best be conservative.
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
			"XidCache: xmin: %ld, mainxid: %ld, childxid: %ld, slow: %ld\n",
			xc_by_recent_xmin,
			xc_by_main_xid,
			xc_by_child_xid,
			xc_slow_answer);
}

#endif   /* XIDCACHE_DEBUG */
