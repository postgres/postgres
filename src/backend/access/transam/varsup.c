/*-------------------------------------------------------------------------
 *
 * varsup.c
 *	  postgres OID & XID variables support routines
 *
 * Copyright (c) 2000-2005, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/transam/varsup.c,v 1.61 2005/02/20 02:21:28 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/clog.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "utils/builtins.h"


/* Number of OIDs to prefetch (preallocate) per XLOG write */
#define VAR_OID_PREFETCH		8192

/* pointer to "variable cache" in shared memory (set up by shmem.c) */
VariableCache ShmemVariableCache = NULL;


/*
 * Allocate the next XID for my new transaction.
 */
TransactionId
GetNewTransactionId(bool isSubXact)
{
	TransactionId xid;

	/*
	 * During bootstrap initialization, we return the special bootstrap
	 * transaction id.
	 */
	if (AMI_OVERRIDE)
		return BootstrapTransactionId;

	LWLockAcquire(XidGenLock, LW_EXCLUSIVE);

	xid = ShmemVariableCache->nextXid;

	/*
	 * Check to see if it's safe to assign another XID.  This protects
	 * against catastrophic data loss due to XID wraparound.  The basic
	 * rules are: warn if we're past xidWarnLimit, and refuse to execute
	 * transactions if we're past xidStopLimit, unless we are running in
	 * a standalone backend (which gives an escape hatch to the DBA who
	 * ignored all those warnings).
	 *
	 * Test is coded to fall out as fast as possible during normal operation,
	 * ie, when the warn limit is set and we haven't violated it.
	 */
	if (TransactionIdFollowsOrEquals(xid, ShmemVariableCache->xidWarnLimit) &&
		TransactionIdIsValid(ShmemVariableCache->xidWarnLimit))
	{
		if (IsUnderPostmaster &&
			TransactionIdFollowsOrEquals(xid, ShmemVariableCache->xidStopLimit))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("database is shut down to avoid wraparound data loss in database \"%s\"",
							NameStr(ShmemVariableCache->limit_datname)),
					 errhint("Stop the postmaster and use a standalone backend to VACUUM in \"%s\".",
							 NameStr(ShmemVariableCache->limit_datname))));
		else
			ereport(WARNING,
					(errmsg("database \"%s\" must be vacuumed within %u transactions",
							NameStr(ShmemVariableCache->limit_datname),
							ShmemVariableCache->xidWrapLimit - xid),
					 errhint("To avoid a database shutdown, execute a full-database VACUUM in \"%s\".",
							 NameStr(ShmemVariableCache->limit_datname))));
	}

	/*
	 * If we are allocating the first XID of a new page of the commit log,
	 * zero out that commit-log page before returning. We must do this
	 * while holding XidGenLock, else another xact could acquire and
	 * commit a later XID before we zero the page.	Fortunately, a page of
	 * the commit log holds 32K or more transactions, so we don't have to
	 * do this very often.
	 *
	 * Extend pg_subtrans too.
	 */
	ExtendCLOG(xid);
	ExtendSUBTRANS(xid);

	/*
	 * Now advance the nextXid counter.  This must not happen until after
	 * we have successfully completed ExtendCLOG() --- if that routine
	 * fails, we want the next incoming transaction to try it again.  We
	 * cannot assign more XIDs until there is CLOG space for them.
	 */
	TransactionIdAdvance(ShmemVariableCache->nextXid);

	/*
	 * We must store the new XID into the shared PGPROC array before
	 * releasing XidGenLock.  This ensures that when GetSnapshotData calls
	 * ReadNewTransactionId, all active XIDs before the returned value of
	 * nextXid are already present in PGPROC.  Else we have a race
	 * condition.
	 *
	 * XXX by storing xid into MyProc without acquiring SInvalLock, we are
	 * relying on fetch/store of an xid to be atomic, else other backends
	 * might see a partially-set xid here.	But holding both locks at once
	 * would be a nasty concurrency hit (and in fact could cause a
	 * deadlock against GetSnapshotData).  So for now, assume atomicity.
	 * Note that readers of PGPROC xid field should be careful to fetch
	 * the value only once, rather than assume they can read it multiple
	 * times and get the same answer each time.
	 *
	 * The same comments apply to the subxact xid count and overflow fields.
	 *
	 * A solution to the atomic-store problem would be to give each PGPROC
	 * its own spinlock used only for fetching/storing that PGPROC's xid
	 * and related fields.	(SInvalLock would then mean primarily that
	 * PGPROCs couldn't be added/removed while holding the lock.)
	 *
	 * If there's no room to fit a subtransaction XID into PGPROC, set the
	 * cache-overflowed flag instead.  This forces readers to look in
	 * pg_subtrans to map subtransaction XIDs up to top-level XIDs. There
	 * is a race-condition window, in that the new XID will not appear as
	 * running until its parent link has been placed into pg_subtrans.
	 * However, that will happen before anyone could possibly have a
	 * reason to inquire about the status of the XID, so it seems OK.
	 * (Snapshots taken during this window *will* include the parent XID,
	 * so they will deliver the correct answer later on when someone does
	 * have a reason to inquire.)
	 */
	if (MyProc != NULL)
	{
		if (!isSubXact)
			MyProc->xid = xid;
		else
		{
			if (MyProc->subxids.nxids < PGPROC_MAX_CACHED_SUBXIDS)
			{
				MyProc->subxids.xids[MyProc->subxids.nxids] = xid;
				MyProc->subxids.nxids++;
			}
			else
				MyProc->subxids.overflowed = true;
		}
	}

	LWLockRelease(XidGenLock);

	return xid;
}

/*
 * Read nextXid but don't allocate it.
 */
TransactionId
ReadNewTransactionId(void)
{
	TransactionId xid;

	LWLockAcquire(XidGenLock, LW_SHARED);
	xid = ShmemVariableCache->nextXid;
	LWLockRelease(XidGenLock);

	return xid;
}

/*
 * Determine the last safe XID to allocate given the currently oldest
 * datfrozenxid (ie, the oldest XID that might exist in any database
 * of our cluster).
 */
void
SetTransactionIdLimit(TransactionId oldest_datfrozenxid,
					  Name oldest_datname)
{
	TransactionId xidWarnLimit;
	TransactionId xidStopLimit;
	TransactionId xidWrapLimit;
	TransactionId curXid;

	Assert(TransactionIdIsValid(oldest_datfrozenxid));

	/*
	 * The place where we actually get into deep trouble is halfway around
	 * from the oldest potentially-existing XID.  (This calculation is
	 * probably off by one or two counts, because the special XIDs reduce the
	 * size of the loop a little bit.  But we throw in plenty of slop below,
	 * so it doesn't matter.)
	 */
	xidWrapLimit = oldest_datfrozenxid + (MaxTransactionId >> 1);
	if (xidWrapLimit < FirstNormalTransactionId)
		xidWrapLimit += FirstNormalTransactionId;

	/*
	 * We'll refuse to continue assigning XIDs in interactive mode once
	 * we get within 1M transactions of data loss.  This leaves lots
	 * of room for the DBA to fool around fixing things in a standalone
	 * backend, while not being significant compared to total XID space.
	 * (Note that since vacuuming requires one transaction per table
	 * cleaned, we had better be sure there's lots of XIDs left...)
	 */
	xidStopLimit = xidWrapLimit - 1000000;
	if (xidStopLimit < FirstNormalTransactionId)
		xidStopLimit -= FirstNormalTransactionId;

	/*
	 * We'll start complaining loudly when we get within 10M transactions
	 * of the stop point.  This is kind of arbitrary, but if you let your
	 * gas gauge get down to 1% of full, would you be looking for the
	 * next gas station?  We need to be fairly liberal about this number
	 * because there are lots of scenarios where most transactions are
	 * done by automatic clients that won't pay attention to warnings.
	 * (No, we're not gonna make this configurable.  If you know enough to
	 * configure it, you know enough to not get in this kind of trouble in
	 * the first place.)
	 */
	xidWarnLimit = xidStopLimit - 10000000;
	if (xidWarnLimit < FirstNormalTransactionId)
		xidWarnLimit -= FirstNormalTransactionId;

	/* Grab lock for just long enough to set the new limit values */
	LWLockAcquire(XidGenLock, LW_EXCLUSIVE);
	ShmemVariableCache->xidWarnLimit = xidWarnLimit;
	ShmemVariableCache->xidStopLimit = xidStopLimit;
	ShmemVariableCache->xidWrapLimit = xidWrapLimit;
	namecpy(&ShmemVariableCache->limit_datname, oldest_datname);
	curXid = ShmemVariableCache->nextXid;
	LWLockRelease(XidGenLock);

	/* Log the info */
	ereport(LOG,
			(errmsg("transaction ID wrap limit is %u, limited by database \"%s\"",
					xidWrapLimit, NameStr(*oldest_datname))));
	/* Give an immediate warning if past the wrap warn point */
	if (TransactionIdFollowsOrEquals(curXid, xidWarnLimit))
		ereport(WARNING,
				(errmsg("database \"%s\" must be vacuumed within %u transactions",
						NameStr(*oldest_datname),
						xidWrapLimit - curXid),
				 errhint("To avoid a database shutdown, execute a full-database VACUUM in \"%s\".",
						 NameStr(*oldest_datname))));
}


/* ----------------------------------------------------------------
 *					object id generation support
 * ----------------------------------------------------------------
 */

static Oid	lastSeenOid = InvalidOid;

Oid
GetNewObjectId(void)
{
	Oid			result;

	LWLockAcquire(OidGenLock, LW_EXCLUSIVE);

	/*
	 * Check for wraparound of the OID counter.  We *must* not return 0
	 * (InvalidOid); and as long as we have to check that, it seems a good
	 * idea to skip over everything below BootstrapObjectIdData too. (This
	 * basically just reduces the odds of OID collision right after a wrap
	 * occurs.)  Note we are relying on unsigned comparison here.
	 */
	if (ShmemVariableCache->nextOid < ((Oid) BootstrapObjectIdData))
	{
		ShmemVariableCache->nextOid = BootstrapObjectIdData;
		ShmemVariableCache->oidCount = 0;
	}

	/* If we run out of logged for use oids then we must log more */
	if (ShmemVariableCache->oidCount == 0)
	{
		XLogPutNextOid(ShmemVariableCache->nextOid + VAR_OID_PREFETCH);
		ShmemVariableCache->oidCount = VAR_OID_PREFETCH;
	}

	result = ShmemVariableCache->nextOid;

	(ShmemVariableCache->nextOid)++;
	(ShmemVariableCache->oidCount)--;

	LWLockRelease(OidGenLock);

	lastSeenOid = result;

	return result;
}

void
CheckMaxObjectId(Oid assigned_oid)
{
	if (lastSeenOid != InvalidOid && assigned_oid < lastSeenOid)
		return;

	LWLockAcquire(OidGenLock, LW_EXCLUSIVE);

	if (assigned_oid < ShmemVariableCache->nextOid)
	{
		lastSeenOid = ShmemVariableCache->nextOid - 1;
		LWLockRelease(OidGenLock);
		return;
	}

	/* If we are in the logged oid range, just bump nextOid up */
	if (assigned_oid <= ShmemVariableCache->nextOid +
		ShmemVariableCache->oidCount - 1)
	{
		ShmemVariableCache->oidCount -=
			assigned_oid - ShmemVariableCache->nextOid + 1;
		ShmemVariableCache->nextOid = assigned_oid + 1;
		LWLockRelease(OidGenLock);
		return;
	}

	/*
	 * We have exceeded the logged oid range. We should lock the database
	 * and kill all other backends but we are loading oid's that we can
	 * not guarantee are unique anyway, so we must rely on the user.
	 */

	XLogPutNextOid(assigned_oid + VAR_OID_PREFETCH);
	ShmemVariableCache->nextOid = assigned_oid + 1;
	ShmemVariableCache->oidCount = VAR_OID_PREFETCH - 1;

	LWLockRelease(OidGenLock);
}
