/*-------------------------------------------------------------------------
 *
 * lmgr.c
 *	  POSTGRES lock manager code
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/lmgr/lmgr.c,v 1.41 2000/06/08 22:37:24 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/transam.h"
#include "catalog/catalog.h"
#include "miscadmin.h"
#include "utils/inval.h"


static LOCKMASK LockConflicts[] = {
	(int) NULL,

/* AccessShareLock */
	(1 << AccessExclusiveLock),

/* RowShareLock */
	(1 << ExclusiveLock) | (1 << AccessExclusiveLock),

/* RowExclusiveLock */
	(1 << ExclusiveLock) | (1 << ShareRowExclusiveLock) | (1 << ShareLock) |
	(1 << AccessExclusiveLock),

/* ShareLock */
	(1 << ExclusiveLock) | (1 << ShareRowExclusiveLock) |
	(1 << RowExclusiveLock) | (1 << AccessExclusiveLock),

/* ShareRowExclusiveLock */
	(1 << ExclusiveLock) | (1 << ShareRowExclusiveLock) |
	(1 << ShareLock) | (1 << RowExclusiveLock) | (1 << AccessExclusiveLock),

/* ExclusiveLock */
	(1 << ExclusiveLock) | (1 << ShareRowExclusiveLock) | (1 << ShareLock) |
	(1 << RowExclusiveLock) | (1 << RowShareLock) | (1 << AccessExclusiveLock),

/* AccessExclusiveLock */
	(1 << ExclusiveLock) | (1 << ShareRowExclusiveLock) | (1 << ShareLock) |
	(1 << RowExclusiveLock) | (1 << RowShareLock) | (1 << AccessExclusiveLock) |
	(1 << AccessShareLock),

};

static int	LockPrios[] = {
	(int) NULL,
	1,
	2,
	3,
	4,
	5,
	6,
	7
};

LOCKMETHOD	LockTableId = (LOCKMETHOD) NULL;
LOCKMETHOD	LongTermTableId = (LOCKMETHOD) NULL;

/*
 * Create the lock table described by LockConflicts and LockPrios.
 */
LOCKMETHOD
InitLockTable()
{
	int			lockmethod;

	lockmethod = LockMethodTableInit("LockTable",
							LockConflicts, LockPrios, MAX_LOCKMODES - 1);
	LockTableId = lockmethod;

	if (!(LockTableId))
		elog(ERROR, "InitLockTable: couldnt initialize lock table");

#ifdef USER_LOCKS

	/*
	 * Allocate another tableId for long-term locks
	 */
	LongTermTableId = LockMethodTableRename(LockTableId);
	if (!(LongTermTableId))
	{
		elog(ERROR,
			 "InitLockTable: couldn't rename long-term lock table");
	}
#endif

	return LockTableId;
}

/*
 * RelationInitLockInfo
 *		Initializes the lock information in a relation descriptor.
 *
 *		relcache.c must call this during creation of any reldesc.
 */
void
RelationInitLockInfo(Relation relation)
{
	char	   *relname;

	Assert(RelationIsValid(relation));
	Assert(OidIsValid(RelationGetRelid(relation)));

	relname = (char *) RelationGetPhysicalRelationName(relation);

	relation->rd_lockInfo.lockRelId.relId = RelationGetRelid(relation);

	if (IsSharedSystemRelationName(relname))
		relation->rd_lockInfo.lockRelId.dbId = InvalidOid;
	else
		relation->rd_lockInfo.lockRelId.dbId = MyDatabaseId;
}

/*
 *		LockRelation
 */
void
LockRelation(Relation relation, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	if (LockingDisabled())
		return;

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = relation->rd_lockInfo.lockRelId.relId;
	tag.dbId = relation->rd_lockInfo.lockRelId.dbId;
	tag.objId.blkno = InvalidBlockNumber;

	if (!LockAcquire(LockTableId, &tag, lockmode))
		elog(ERROR, "LockRelation: LockAcquire failed");

	/*
	 * Check to see if the relcache entry has been invalidated while we
	 * were waiting to lock it.  If so, rebuild it, or elog() trying.
	 * Increment the refcount to ensure that RelationFlushRelation will
	 * rebuild it and not just delete it.
	 */
	RelationIncrementReferenceCount(relation);
	DiscardInvalid();
	RelationDecrementReferenceCount(relation);
}

/*
 *		UnlockRelation
 */
void
UnlockRelation(Relation relation, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	if (LockingDisabled())
		return;

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = relation->rd_lockInfo.lockRelId.relId;
	tag.dbId = relation->rd_lockInfo.lockRelId.dbId;
	tag.objId.blkno = InvalidBlockNumber;

	LockRelease(LockTableId, &tag, lockmode);
}

/*
 *		LockPage
 */
void
LockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	if (LockingDisabled())
		return;

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = relation->rd_lockInfo.lockRelId.relId;
	tag.dbId = relation->rd_lockInfo.lockRelId.dbId;
	tag.objId.blkno = blkno;

	if (!LockAcquire(LockTableId, &tag, lockmode))
		elog(ERROR, "LockPage: LockAcquire failed");
}

/*
 *		UnlockPage
 */
void
UnlockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	if (LockingDisabled())
		return;

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = relation->rd_lockInfo.lockRelId.relId;
	tag.dbId = relation->rd_lockInfo.lockRelId.dbId;
	tag.objId.blkno = blkno;

	LockRelease(LockTableId, &tag, lockmode);
}

void
XactLockTableInsert(TransactionId xid)
{
	LOCKTAG		tag;

	if (LockingDisabled())
		return;

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = XactLockTableId;
	tag.dbId = InvalidOid;
	tag.objId.xid = xid;

	if (!LockAcquire(LockTableId, &tag, ExclusiveLock))
		elog(ERROR, "XactLockTableInsert: LockAcquire failed");
}

#ifdef NOT_USED
void
XactLockTableDelete(TransactionId xid)
{
	LOCKTAG		tag;

	if (LockingDisabled())
		return;

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = XactLockTableId;
	tag.dbId = InvalidOid;
	tag.objId.xid = xid;

	LockRelease(LockTableId, &tag, ExclusiveLock);
}
#endif

void
XactLockTableWait(TransactionId xid)
{
	LOCKTAG		tag;

	if (LockingDisabled())
		return;

	MemSet(&tag, 0, sizeof(tag));
	tag.relId = XactLockTableId;
	tag.dbId = InvalidOid;
	tag.objId.xid = xid;

	if (!LockAcquire(LockTableId, &tag, ShareLock))
		elog(ERROR, "XactLockTableWait: LockAcquire failed");

	LockRelease(LockTableId, &tag, ShareLock);

	/*
	 * Transaction was committed/aborted/crashed - we have to update
	 * pg_log if transaction is still marked as running.
	 */
	if (!TransactionIdDidCommit(xid) && !TransactionIdDidAbort(xid))
		TransactionIdAbort(xid);
}
