/*-------------------------------------------------------------------------
 *
 * varsup.c
 *	  postgres OID & XID variables support routines
 *
 * Copyright (c) 2000-2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/transam/varsup.c,v 1.52.4.1 2004/01/26 19:16:23 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/clog.h"
#include "access/transam.h"
#include "storage/ipc.h"
#include "storage/proc.h"


/* Number of OIDs to prefetch (preallocate) per XLOG write */
#define VAR_OID_PREFETCH		8192

/* pointer to "variable cache" in shared memory (set up by shmem.c) */
VariableCache ShmemVariableCache = NULL;


/*
 * Allocate the next XID for my new transaction.
 */
TransactionId
GetNewTransactionId(void)
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
	 * If we are allocating the first XID of a new page of the commit
	 * log, zero out that commit-log page before returning. We must do
	 * this while holding XidGenLock, else another xact could acquire and
	 * commit a later XID before we zero the page.	Fortunately, a page of
	 * the commit log holds 32K or more transactions, so we don't have to
	 * do this very often.
	 */
	ExtendCLOG(xid);

	/*
	 * Now advance the nextXid counter.  This must not happen until after
	 * we have successfully completed ExtendCLOG() --- if that routine fails,
	 * we want the next incoming transaction to try it again.  We cannot
	 * assign more XIDs until there is CLOG space for them.
	 */
	TransactionIdAdvance(ShmemVariableCache->nextXid);

	/*
	 * Must set MyProc->xid before releasing XidGenLock.  This ensures
	 * that when GetSnapshotData calls ReadNewTransactionId, all active
	 * XIDs before the returned value of nextXid are already present in
	 * the shared PGPROC array.  Else we have a race condition.
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
	 * A solution to the atomic-store problem would be to give each PGPROC
	 * its own spinlock used only for fetching/storing that PGPROC's xid.
	 * (SInvalLock would then mean primarily that PGPROCs couldn't be added/
	 * removed while holding the lock.)
	 */
	if (MyProc != (PGPROC *) NULL)
		MyProc->xid = xid;

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
