/*-------------------------------------------------------------------------
 *
 * varsup.c
 *	  postgres OID & XID variables support routines
 *
 * Copyright (c) 2000, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/transam/varsup.c,v 1.44 2001/08/23 23:06:37 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/transam.h"
#include "access/xlog.h"
#include "storage/proc.h"


/* Number of OIDs to prefetch (preallocate) per XLOG write */
#define VAR_OID_PREFETCH		8192

/* Spinlocks for serializing generation of XIDs and OIDs, respectively */
SPINLOCK	XidGenLockId;
SPINLOCK	OidGenLockId;

/* pointer to "variable cache" in shared memory (set up by shmem.c) */
VariableCache ShmemVariableCache = NULL;


/*
 * Allocate the next XID for my new transaction.
 */
void
GetNewTransactionId(TransactionId *xid)
{
	/*
	 * During bootstrap initialization, we return the special bootstrap
	 * transaction id.
	 */
	if (AMI_OVERRIDE)
	{
		*xid = BootstrapTransactionId;
		return;
	}

	SpinAcquire(XidGenLockId);

	*xid = ShmemVariableCache->nextXid;

	TransactionIdAdvance(ShmemVariableCache->nextXid);

	/*
	 * Must set MyProc->xid before releasing XidGenLock.  This ensures that
	 * when GetSnapshotData calls ReadNewTransactionId, all active XIDs
	 * before the returned value of nextXid are already present in the shared
	 * PROC array.  Else we have a race condition.
	 *
	 * XXX by storing xid into MyProc without acquiring SInvalLock, we are
	 * relying on fetch/store of an xid to be atomic, else other backends
	 * might see a partially-set xid here.  But holding both locks at once
	 * would be a nasty concurrency hit (and in fact could cause a deadlock
	 * against GetSnapshotData).  So for now, assume atomicity.  Note that
	 * readers of PROC xid field should be careful to fetch the value only
	 * once, rather than assume they can read it multiple times and get the
	 * same answer each time.
	 *
	 * A solution to the atomic-store problem would be to give each PROC
	 * its own spinlock used only for fetching/storing that PROC's xid.
	 * (SInvalLock would then mean primarily that PROCs couldn't be added/
	 * removed while holding the lock.)
	 */
	if (MyProc != (PROC *) NULL)
		MyProc->xid = *xid;

	SpinRelease(XidGenLockId);
}

/*
 * Read nextXid but don't allocate it.
 */
void
ReadNewTransactionId(TransactionId *xid)
{
	/*
	 * During bootstrap initialization, we return the special bootstrap
	 * transaction id.
	 */
	if (AMI_OVERRIDE)
	{
		*xid = BootstrapTransactionId;
		return;
	}

	SpinAcquire(XidGenLockId);
	*xid = ShmemVariableCache->nextXid;
	SpinRelease(XidGenLockId);
}

/* ----------------------------------------------------------------
 *					object id generation support
 * ----------------------------------------------------------------
 */

static Oid	lastSeenOid = InvalidOid;

Oid
GetNewObjectId(void)
{
	Oid		result;

	SpinAcquire(OidGenLockId);

	/*
	 * Check for wraparound of the OID counter.  We *must* not return 0
	 * (InvalidOid); and as long as we have to check that, it seems a good
	 * idea to skip over everything below BootstrapObjectIdData too.  (This
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

	SpinRelease(OidGenLockId);

	lastSeenOid = result;

	return result;
}

void
CheckMaxObjectId(Oid assigned_oid)
{
	if (lastSeenOid != InvalidOid && assigned_oid < lastSeenOid)
		return;

	SpinAcquire(OidGenLockId);

	if (assigned_oid < ShmemVariableCache->nextOid)
	{
		lastSeenOid = ShmemVariableCache->nextOid - 1;
		SpinRelease(OidGenLockId);
		return;
	}

	/* If we are in the logged oid range, just bump nextOid up */
	if (assigned_oid <= ShmemVariableCache->nextOid +
		ShmemVariableCache->oidCount - 1)
	{
		ShmemVariableCache->oidCount -=
			assigned_oid - ShmemVariableCache->nextOid + 1;
		ShmemVariableCache->nextOid = assigned_oid + 1;
		SpinRelease(OidGenLockId);
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

	SpinRelease(OidGenLockId);
}
