/*-------------------------------------------------------------------------
 *
 * varsup.c
 *	  postgres OID & XID variables support routines
 *
 * Copyright (c) 2000, PostgreSQL, Inc
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/transam/Attic/xlog_varsup.c,v 1.1 2000/11/03 11:39:35 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/transam.h"
#include "storage/proc.h"

SPINLOCK OidGenLockId;

extern SPINLOCK XidGenLockId;
extern void XLogPutNextOid(Oid nextOid);

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
	*xid = ShmemVariableCache->nextXid;
	(ShmemVariableCache->nextXid)++;

	if (MyProc != (PROC *) NULL)
		MyProc->xid = *xid;

	SpinRelease(XidGenLockId);

}

/*
 * Like GetNewTransactionId reads nextXid but don't fetch it.
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

#define VAR_OID_PREFETCH		8192
static Oid lastSeenOid = InvalidOid;

void
GetNewObjectId(Oid *oid_return)
{
	SpinAcquire(OidGenLockId);

	/* If we run out of logged for use oids then we log more */
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
