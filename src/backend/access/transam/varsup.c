/*-------------------------------------------------------------------------
 *
 * varsup.c
 *	  postgres OID & XID variables support routines
 *
 * Copyright (c) 2000, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/transam/varsup.c,v 1.36 2001/03/13 01:17:05 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/transam.h"
#include "access/xlog.h"
#include "storage/proc.h"


/* Number of XIDs and OIDs to prefetch (preallocate) per XLOG write */
#define VAR_XID_PREFETCH		1024
#define VAR_OID_PREFETCH		8192

/* Spinlocks for serializing generation of XIDs and OIDs, respectively */
SPINLOCK XidGenLockId;
SPINLOCK OidGenLockId;

/* pointer to "variable cache" in shared memory (set up by shmem.c) */
VariableCache ShmemVariableCache = NULL;

void
GetNewTransactionId(TransactionId *xid)
{
	/*
	 * During bootstrap initialization, we return the special
	 * bootstrap transaction id.
	 */
	if (AMI_OVERRIDE)
	{
		*xid = AmiTransactionId;
		return;
	}

	SpinAcquire(XidGenLockId);

	/* If we run out of logged for use xids then we must log more */
	if (ShmemVariableCache->xidCount == 0)
	{
		XLogPutNextXid(ShmemVariableCache->nextXid + VAR_XID_PREFETCH);
		ShmemVariableCache->xidCount = VAR_XID_PREFETCH;
	}

	*xid = ShmemVariableCache->nextXid;

	(ShmemVariableCache->nextXid)++;
	(ShmemVariableCache->xidCount)--;

	SpinRelease(XidGenLockId);

	if (MyProc != (PROC *) NULL)
		MyProc->xid = *xid;
}

/*
 * Read nextXid but don't allocate it.
 */
void
ReadNewTransactionId(TransactionId *xid)
{
	/*
	 * During bootstrap initialization, we return the special
	 * bootstrap transaction id.
	 */
	if (AMI_OVERRIDE)
	{
		*xid = AmiTransactionId;
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

static Oid lastSeenOid = InvalidOid;

void
GetNewObjectId(Oid *oid_return)
{
	SpinAcquire(OidGenLockId);

	/* If we run out of logged for use oids then we must log more */
	if (ShmemVariableCache->oidCount == 0)
	{
		XLogPutNextOid(ShmemVariableCache->nextOid + VAR_OID_PREFETCH);
		ShmemVariableCache->oidCount = VAR_OID_PREFETCH;
	}

	if (PointerIsValid(oid_return))
		lastSeenOid = (*oid_return) = ShmemVariableCache->nextOid;

	(ShmemVariableCache->nextOid)++;
	(ShmemVariableCache->oidCount)--;

	SpinRelease(OidGenLockId);
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
	 * We have exceeded the logged oid range.
	 * We should lock the database and kill all other backends
	 * but we are loading oid's that we can not guarantee are unique
	 * anyway, so we must rely on the user.
	 */

	XLogPutNextOid(assigned_oid + VAR_OID_PREFETCH);
	ShmemVariableCache->oidCount = VAR_OID_PREFETCH - 1;
	ShmemVariableCache->nextOid = assigned_oid + 1;

	SpinRelease(OidGenLockId);
}
