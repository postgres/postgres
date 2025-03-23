/*-------------------------------------------------------------------------
 *
 * csn_snapshot.c
 *		Support for cross-node snapshot isolation.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/csn_snapshot.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/csn_snapshot.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "portability/instr_time.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/snapmgr.h"
#include "miscadmin.h"

/* Raise a warning if imported snapshot_csn exceeds ours by this value. */
#define SNAP_DESYNC_COMPLAIN (1*NSECS_PER_SEC) /* 1 second */

static TransactionId xmin_for_csn = InvalidTransactionId;


/*
 * GUC to delay advance of oldestXid for this amount of time. Also determines
 * the size CSNSnapshotXidMap circular buffer.
 */
int csn_snapshot_defer_time;

int csn_time_shift;

/*
 * CSNSnapshotXidMap
 *
 * To be able to install csn snapshot that points to past we need to keep
 * old versions of tuples and therefore delay advance of oldestXid.  Here we
 * keep track of correspondence between snapshot's snapshot_csn and oldestXid
 * that was set at the time when the snapshot was taken.  Much like the
 * snapshot too old's OldSnapshotControlData does, but with finer granularity
 * to seconds.
 *
 * Different strategies can be employed to hold oldestXid (e.g. we can track
 * oldest csn-based snapshot among cluster nodes and map it oldestXid
 * on each node).
 *
 * On each snapshot acquisition CSNSnapshotMapXmin() is called and stores
 * correspondence between current snapshot_csn and oldestXmin in a sparse way:
 * snapshot_csn is rounded to seconds (and here we use the fact that snapshot_csn
 * is just a timestamp) and oldestXmin is stored in the circular buffer where
 * rounded snapshot_csn acts as an offset from current circular buffer head.
 * Size of the circular buffer is controlled by csn_snapshot_defer_time GUC.
 *
 * When csn snapshot arrives we check that its
 * snapshot_csn is still in our map, otherwise we'll error out with "snapshot too
 * old" message.  If snapshot_csn is successfully mapped to oldestXid we move
 * backend's pgxact->xmin to proc->originalXmin and fill pgxact->xmin to
 * mapped oldestXid.  That way GetOldestXmin() can take into account backends
 * with imported csn snapshot and old tuple versions will be preserved.
 *
 * Also while calculating oldestXmin for our map in presence of imported
 * csn snapshots we should use proc->originalXmin instead of pgxact->xmin
 * that was set during import.  Otherwise, we can create a feedback loop:
 * xmin's of imported csn snapshots were calculated using our map and new
 * entries in map going to be calculated based on that xmin's, and there is
 * a risk to stuck forever with one non-increasing oldestXmin.  All other
 * callers of GetOldestXmin() are using pgxact->xmin so the old tuple versions
 * are preserved.
 */
typedef struct CSNSnapshotXidMap
{
	int				 head;				/* offset of current freshest value */
	int				 size;				/* total size of circular buffer */
	CSN_atomic		 last_csn_seconds;	/* last rounded csn that changed
										 * xmin_by_second[] */
	TransactionId   *xmin_by_second;	/* circular buffer of oldestXmin's */
}
CSNSnapshotXidMap;

static CSNSnapshotXidMap *csnXidMap;


/* Estimate shared memory space needed */
Size
CSNSnapshotShmemSize(void)
{
	Size	size = 0;

	if (csn_snapshot_defer_time > 0)
	{
		size += sizeof(CSNSnapshotXidMap);
		size += csn_snapshot_defer_time*sizeof(TransactionId);
		size = MAXALIGN(size);
	}

	return size;
}

/* Init shared memory structures */
void
CSNSnapshotShmemInit()
{
	bool found;

	if (csn_snapshot_defer_time > 0)
	{
		csnXidMap = ShmemInitStruct("csnXidMap",
								   sizeof(CSNSnapshotXidMap),
								   &found);
		if (!found)
		{
			int i;

			pg_atomic_init_u64(&csnXidMap->last_csn_seconds, 0);
			csnXidMap->head = 0;
			csnXidMap->size = csn_snapshot_defer_time;
			csnXidMap->xmin_by_second =
							ShmemAlloc(sizeof(TransactionId)*csnXidMap->size);

			for (i = 0; i < csnXidMap->size; i++)
				csnXidMap->xmin_by_second[i] = InvalidTransactionId;
		}
	}
}

/*
 * CSNSnapshotStartup
 *
 * Set csnXidMap entries to oldestActiveXID during startup.
 */
void
CSNSnapshotStartup(TransactionId oldestActiveXID)
{
	/*
	 * Run only if we have initialized shared memory and csnXidMap
	 * is enabled.
	 */
	if (IsNormalProcessingMode() &&
		enable_csn_snapshot && csn_snapshot_defer_time > 0)
	{
		int i;

		Assert(TransactionIdIsValid(oldestActiveXID));
		for (i = 0; i < csnXidMap->size; i++)
			csnXidMap->xmin_by_second[i] = oldestActiveXID;
		ProcArraySetCSNSnapshotXmin(oldestActiveXID);

		elog(LOG, "CSN map initialized with oldest active xid %u", oldestActiveXID);
	}
}

/*
 * CSNSnapshotMapXmin
 *
 * Maintain circular buffer of oldestXmins for several seconds in past. This
 * buffer allows to shift oldestXmin in the past when backend is importing
 * CSN snapshot. Otherwise old versions of tuples that were needed for
 * this transaction can be recycled by other processes (vacuum, HOT, etc).
 *
 * Locking here is not trivial. Called upon each snapshot creation after
 * ProcArrayLock is released. Such usage creates several race conditions. It
 * is possible that backend who got csn called CSNSnapshotMapXmin()
 * only after other backends managed to get snapshot and complete
 * CSNSnapshotMapXmin() call, or even committed. This is safe because
 *
 *		* We already hold our xmin in MyPgXact, so our snapshot will not be
 *		  harmed even though ProcArrayLock is released.
 *
 *		* snapshot_csn is always pessmistically rounded up to the next
 *		  second.
 *
 *		* For performance reasons, xmin value for particular second is filled
 *		  only once. Because of that instead of writing to buffer just our
 *		  xmin (which is enough for our snapshot), we bump oldestXmin there --
 *		  it mitigates the possibility of damaging someone else's snapshot by
 *		  writing to the buffer too advanced value in case of slowness of
 *		  another backend who generated csn earlier, but didn't manage to
 *		  insert it before us.
 *
 *		* if CSNSnapshotMapXmin() founds a gap in several seconds between
 *		  current call and latest completed call then it should fill that gap
 *		  with latest known values instead of new one. Otherwise it is
 *		  possible (however highly unlikely) that this gap also happend
 *		  between taking snapshot and call to CSNSnapshotMapXmin() for some
 *		  backend. And we are at risk to fill circullar buffer with
 *		  oldestXmin's that are bigger then they actually were.
 */
void
CSNSnapshotMapXmin(SnapshotCSN snapshot_csn)
{
	int offset, gap, i;
	SnapshotCSN csn_seconds;
	SnapshotCSN last_csn_seconds;
	volatile TransactionId oldest_deferred_xmin;
	TransactionId current_oldest_xmin, previous_oldest_xmin;
	TransactionId ImportedXmin;

	/* Callers should check config values */
	Assert(csn_snapshot_defer_time > 0);
	Assert(csnXidMap != NULL);
	/*
	 * Round up snapshot_csn to the next second -- pessimistically and safely.
	 */
	csn_seconds = (snapshot_csn / NSECS_PER_SEC + 1);

	/*
	 * Fast-path check. Avoid taking exclusive CSNSnapshotXidMapLock lock
	 * if oldestXid was already written to xmin_by_second[] for this rounded
	 * snapshot_csn.
	 */
	if (pg_atomic_read_u64(&csnXidMap->last_csn_seconds) >= csn_seconds)
		return;

	/* Ok, we have new entry (or entries) */
	LWLockAcquire(CSNSnapshotXidMapLock, LW_EXCLUSIVE);

	/* Re-check last_csn_seconds under lock */
	last_csn_seconds = pg_atomic_read_u64(&csnXidMap->last_csn_seconds);
	if (last_csn_seconds >= csn_seconds)
	{
		LWLockRelease(CSNSnapshotXidMapLock);
		return;
	}
	pg_atomic_write_u64(&csnXidMap->last_csn_seconds, csn_seconds);

	/*
	 * Count oldest_xmin.
	 *
	 * It was possible to calculate oldest_xmin during corresponding snapshot
	 * creation, but GetSnapshotData() intentionally reads only PgXact, but not
	 * PgProc. And we need info about originalXmin (see comment to csnXidMap)
	 * which is stored in PgProc because of threats in comments around PgXact
	 * about extending it with new fields. So just calculate oldest_xmin again,
	 * that anyway happens quite rarely.
	 */

	/*
	 * Don't afraid here because csn_snapshot_xmin will hold border of
	 * minimal non-removable from vacuuming.
	 */
	ImportedXmin = MyProc->xmin;
	MyProc->xmin = MyProc->originalXmin;
	current_oldest_xmin = GetOldestNonRemovableTransactionId(NULL);
	MyProc->xmin = ImportedXmin;
	Assert(TransactionIdIsNormal(current_oldest_xmin));

	previous_oldest_xmin = csnXidMap->xmin_by_second[csnXidMap->head];
	Assert(TransactionIdIsNormal(previous_oldest_xmin) || !enable_csn_snapshot);

	gap = csn_seconds - last_csn_seconds;
	offset = csn_seconds % csnXidMap->size;

	/* Sanity check before we update head and gap */
	Assert( gap >= 1 );
	Assert( (csnXidMap->head + gap) % csnXidMap->size == offset );

	gap = gap > csnXidMap->size ? csnXidMap->size : gap;
	csnXidMap->head = offset;

	/* Fill new entry with current_oldest_xmin */
	csnXidMap->xmin_by_second[offset] = current_oldest_xmin;

	/*
	 * If we have gap then fill it with previous_oldest_xmin for reasons
	 * outlined in comment above this function.
	 */
	for (i = 1; i < gap; i++)
	{
		offset = (offset + csnXidMap->size - 1) % csnXidMap->size;
		csnXidMap->xmin_by_second[offset] = previous_oldest_xmin;
	}

	oldest_deferred_xmin =
		csnXidMap->xmin_by_second[ (csnXidMap->head + 1) % csnXidMap->size ];

	LWLockRelease(CSNSnapshotXidMapLock);

	elog(DEBUG5, "Advance xmin for CSN. Oldest deferred xmin = %u",
		 oldest_deferred_xmin);

	/*
	 * Advance procArray->csn_snapshot_xmin after we released
	 * CSNSnapshotXidMapLock. Since we gather not xmin but oldestXmin, it
	 * never goes backwards regardless of how slow we can do that.
	 */
	/*Assert(TransactionIdFollowsOrEquals(oldest_deferred_xmin,
										ProcArrayGetCSNSnapshotXmin()));*/
	ProcArraySetCSNSnapshotXmin(oldest_deferred_xmin);
}


/*
 * CSNSnapshotToXmin
 *
 * Get oldestXmin that took place when snapshot_csn was taken.
 */
TransactionId
CSNSnapshotToXmin(SnapshotCSN snapshot_csn)
{
	TransactionId xmin;
	SnapshotCSN csn_seconds;
	volatile SnapshotCSN last_csn_seconds;

	/* Callers should check config values */
	Assert(csn_snapshot_defer_time > 0);
	Assert(csnXidMap != NULL);

	/* Round down to get conservative estimates */
	csn_seconds = (snapshot_csn / NSECS_PER_SEC);

	LWLockAcquire(CSNSnapshotXidMapLock, LW_SHARED);
	last_csn_seconds = pg_atomic_read_u64(&csnXidMap->last_csn_seconds);
	if (csn_seconds > last_csn_seconds)
	{
		/* we don't have entry for this snapshot_csn yet, return latest known */
		xmin = csnXidMap->xmin_by_second[csnXidMap->head];
	}
	else if (last_csn_seconds - csn_seconds < csnXidMap->size)
	{
		/* we are good, retrieve value from our map */
		Assert(last_csn_seconds % csnXidMap->size == csnXidMap->head);
		xmin = csnXidMap->xmin_by_second[csn_seconds % csnXidMap->size];
	}
	else
	{
		/* requested snapshot_csn is too old, let caller know */
		xmin = InvalidTransactionId;
	}
	LWLockRelease(CSNSnapshotXidMapLock);

	return xmin;
}

/*
 * CSNSnapshotPrepareCurrent
 *
 * Set InDoubt state for currently active transaction and return commit's
 * global snapshot.
 */
SnapshotCSN
CSNSnapshotPrepareCurrent(void)
{
	TransactionId xid = GetCurrentTransactionIdIfAny();

	if (!enable_csn_snapshot)
		ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("could not prepare transaction for global commit"),
				errhint("Make sure the configuration parameter \"%s\" is enabled.",
						"enable_csn_snapshot")));

	if (TransactionIdIsValid(xid))
	{
		TransactionId *subxids;
		int nsubxids = xactGetCommittedChildren(&subxids);
		CSNLogSetCSN(xid, nsubxids, subxids, InDoubtCSN, true);
	}

	/* Nothing to write if we don't have xid */

	return GenerateCSN(false, InvalidCSN);
}


/*
 * CSNSnapshotAssignCurrent
 *
 * Assign SnapshotCSN to the currently active transaction. SnapshotCSN is supposedly
 * maximal among of values returned by CSNSnapshotPrepareCurrent and
 * pg_csn_snapshot_prepare.
 */
void
CSNSnapshotAssignCurrent(SnapshotCSN snapshot_csn)
{
	if (!enable_csn_snapshot)
		ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("could not prepare transaction for global commit"),
				errhint("Make sure the configuration parameter \"%s\" is enabled.",
						"enable_csn_snapshot")));

	if (!CSNIsNormal(snapshot_csn))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_csn_snapshot_assign expects normal snapshot_csn")));

	Assert(snapshot_csn != InvalidCSN);
	/* We do not care the Generate result, we just want to make sure max
	 * csnShared->last_max_csn value.
	 */
	GenerateCSN(false, snapshot_csn);

	/* Set csn and defuse ProcArrayEndTransaction from assigning one */
	pg_atomic_write_u64(&MyProc->assignedCSN, snapshot_csn);
}

/*
 * CSNSnapshotSync
 *
 * Due to time desynchronization on different nodes we can receive snapshot_csn
 * which is greater than snapshot_csn on this node. To preserve proper isolation
 * this node needs to wait when such snapshot_csn comes on local clock.
 *
 * This should happend relatively rare if nodes have running NTP/PTP/etc.
 * Complain if wait time is more than SNAP_SYNC_COMPLAIN.
 */
void
CSNSnapshotSync(SnapshotCSN remote_csn)
{
	SnapshotCSN	local_csn;
	SnapshotCSN	delta;

	Assert(enable_csn_snapshot);

	for(;;)
	{
		if (GetLastGeneratedCSN() > remote_csn)
			return;

		local_csn = GenerateCSN(true, InvalidCSN);

		if (local_csn >= remote_csn)
			/*
			 * Everything is fine too, but last_max_csn wasn't updated for
			 * some time.
			 */
			return;

		/* Okay we need to sleep now */
		delta = remote_csn - local_csn;
		if (delta > SNAP_DESYNC_COMPLAIN)
			ereport(WARNING,
				(errmsg("remote global snapshot exceeds ours by more than a second"),
				 errhint("Consider running NTPd on servers participating in global transaction")));

		/* TODO: report this sleeptime somewhere? */
		pg_usleep((long) (delta/NSECS_PER_USEC));

		/*
		 * Loop that checks to ensure that we actually slept for specified
		 * amount of time.
		 */
	}

	Assert(false); /* Should not happend */
	return;
}

/*
 * TransactionIdGetCSN
 *
 * Get CSN for specified TransactionId taking care about special xids,
 * xids beyond TransactionXmin and InDoubt states.
 */
CSN
TransactionIdGetCSN(TransactionId xid)
{
	CSN csn;

	/* Handle permanent TransactionId's for which we don't have mapping */
	if (!TransactionIdIsNormal(xid))
	{
		if (xid == InvalidTransactionId)
			return AbortedCSN;
		if (xid == FrozenTransactionId || xid == BootstrapTransactionId)
			return FrozenCSN;
		Assert(false); /* Should not happend */
	}

	/*
	 * If we just switch a xid-snapsot to a csn_snapshot, we should handle a start
	 * xid for csn base check. Just in case we have prepared transaction which
	 * hold the TransactionXmin but without CSN.
	 */
	xmin_for_csn = GetOldestXmin();

	/*
	 * For the xid with 'xid >= TransactionXmin and xid < xmin_for_csn',
	 * it defined as unclear csn which follow xid-snapshot result.
	 */
	if(!TransactionIdPrecedes(xid, TransactionXmin) &&
							TransactionIdPrecedes(xid, xmin_for_csn))
	{
		elog(LOG, "UnclearCSN was returned. xid=%u, TransactionXmin=%u, xmin_for_csn=%u",
			xid, TransactionXmin, xmin_for_csn);
		return UnclearCSN;
	}
	/*
	 * For xids which less then TransactionXmin CSNLog can be already
	 * trimmed but we know that such transaction is definitely not concurrently
	 * running according to any snapshot including timetravel ones. Callers
	 * should check TransactionDidCommit after.
	 */
	if (TransactionIdPrecedes(xid, TransactionXmin))
		return FrozenCSN;

	/* Read CSN from SLRU */
	csn = CSNLogGetCSNByXid(xid);

	/*
	 * If we faced InDoubt state then transaction is being committed and we
	 * should wait until CSN will be assigned so that visibility check
	 * could decide whether tuple is in snapshot. See also comments in
	 * CSNSnapshotPrecommit().
	 */
	if (CSNIsInDoubt(csn))
	{
		XactLockTableWait(SubTransGetTopmostTransaction(xid), NULL, NULL, XLTW_None);
		csn = CSNLogGetCSNByXid(xid);
		Assert(CSNIsNormal(csn) || CSNIsAborted(csn));
	}

	Assert(CSNIsNormal(csn) || CSNIsInProgress(csn) || CSNIsAborted(csn));
	return csn;
}

/*
 * XidInCSNSnapshot
 *
 * Version of XidInMVCCSnapshot for transactions. For non-imported
 * csn snapshots this should give same results as XidInLocalMVCCSnapshot
 * (except that aborts will be shown as invisible without going to clog) and to
 * ensure such behaviour XidInMVCCSnapshot is coated with asserts that checks
 * identicalness of XidInCSNSnapshot/XidInLocalMVCCSnapshot in
 * case of ordinary snapshot.
 */
bool
XidInCSNSnapshot(TransactionId xid, Snapshot snapshot)
{
	CSN csn;

	csn = TransactionIdGetCSN(xid);

	if (CSNIsNormal(csn))
		return (csn >= snapshot->snapshot_csn);
	else if (CSNIsFrozen(csn))
	{
		/* It is bootstrap or frozen transaction */
		return false;
	}
	else if(CSNIsUnclear(csn))
	{
		/*
		 * Some xid can not figure out csn because of snapshot switch,
		 * and we can follow xid-base result.
		 */
		return true;
	}
	else
	{
		/* It is aborted or in-progress */
		Assert(CSNIsAborted(csn) || CSNIsInProgress(csn));
		if (CSNIsAborted(csn))
			Assert(TransactionIdDidAbort(xid));
		return true;
	}
}


/*****************************************************************************
 * Functions to handle transactions commit.
 *
 * For local transactions CSNSnapshotPrecommit sets InDoubt state before
 * ProcArrayEndTransaction is called and transaction data potetntially becomes
 * visible to other backends. ProcArrayEndTransaction (or ProcArrayRemove in
 * twophase case) then acquires csn under ProcArray lock and stores it
 * in proc->assignedCSN. It's important that csn for commit is
 * generated under ProcArray lock, otherwise snapshots won't
 * be equivalent. Consequent call to CSNSnapshotCommit will write
 * proc->assignedCSN to CSNLog.
 *
 *
 * CSNSnapshotAbort is slightly different comparing to commit because abort
 * can skip InDoubt phase and can be called for transaction subtree.
 *****************************************************************************/


/*
 * CSNSnapshotAbort
 *
 * Abort transaction in CsnLog. We can skip InDoubt state for aborts
 * since no concurrent transactions allowed to see aborted data anyway.
 */
void
CSNSnapshotAbort(PGPROC *proc, TransactionId xid,
					int nsubxids, TransactionId *subxids)
{
	if (!get_csnlog_status())
		return;

	CSNLogSetCSN(xid, nsubxids, subxids, AbortedCSN, true);

	/*
	 * Clean assignedCSN anyway, as it was possibly set in
	 * XidSnapshotAssignCsnCurrent.
	 */
	pg_atomic_write_u64(&proc->assignedCSN, InProgressCSN);
}

/*
 * CSNSnapshotPrecommit
 *
 * Set InDoubt status for local transaction that we are going to commit.
 * This step is needed to achieve consistency between local snapshots and
 * csn-based snapshots. We don't hold ProcArray lock while writing
 * csn for transaction in SLRU but instead we set InDoubt status before
 * transaction is deleted from ProcArray so the readers who will read csn
 * in the gap between ProcArray removal and CSN assignment can wait
 * until CSN is finally assigned. See also TransactionIdGetCSN().
 *
 * This should be called only from parallel group leader before backend is
 * deleted from ProcArray.
 */
void
CSNSnapshotPrecommit(PGPROC *proc, TransactionId xid,
					int nsubxids, TransactionId *subxids)
{
	CSN oldassignedCSN = InProgressCSN;
	bool in_progress;

	if (!get_csnlog_status())
		return;

	/* Set InDoubt status if it is local transaction */
	in_progress = pg_atomic_compare_exchange_u64(&proc->assignedCSN,
												 &oldassignedCSN,
												 InDoubtCSN);
	if (in_progress)
	{
		Assert(CSNIsInProgress(oldassignedCSN));
		CSNLogSetCSN(xid, nsubxids, subxids, InDoubtCSN, true);
	}
	else
	{
		/* Otherwise we should have valid CSN by this time */
		Assert(CSNIsNormal(oldassignedCSN));
		Assert(CSNIsInDoubt(CSNLogGetCSNByXid(xid)));
	}
}

/*
 * CSNSnapshotCommit
 *
 * Write CSN that were acquired earlier to CsnLog. Should be
 * preceded by CSNSnapshotPrecommit() so readers can wait until we finally
 * finished writing to SLRU.
 *
 * Should be called after ProcArrayEndTransaction, but before releasing
 * transaction locks, so that TransactionIdGetCSN can wait on this
 * lock for CSN.
 */
void
CSNSnapshotCommit(PGPROC *proc, TransactionId xid,
				  int nsubxids, TransactionId *subxids)
{
	volatile CSN assignedCSN;

	if (!get_csnlog_status())
		return;

	if (!TransactionIdIsValid(xid))
	{
		assignedCSN = pg_atomic_read_u64(&proc->assignedCSN);
		Assert(CSNIsInProgress(assignedCSN));
		return;
	}

	/* Finally write resulting CSN in SLRU */
	assignedCSN = pg_atomic_read_u64(&proc->assignedCSN);
	Assert(CSNIsNormal(assignedCSN));
	CSNLogSetCSN(xid, nsubxids, subxids, assignedCSN, true);

	/* Reset for next transaction */
	pg_atomic_write_u64(&proc->assignedCSN, InProgressCSN);
}
